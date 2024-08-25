#include "dstorage_stream.h"
#include "internal.h"

#include <atomic>
#include <mutex>
#include <future>
#include <filesystem>
#include <dstorage.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <ppl.h>


namespace ist {

#pragma region Misc
using winrt::check_hresult;
using winrt::com_ptr;

// functions
// d3d12.dll や dstorage.dll がないと exe が起動すらしなくなってしまうのは避けたいため、
// 古き悪しき LoadLibrary() & GetProcAddress() でインポートを解決する。
// (/DELAYLOAD ならスマートに解決できるのだが、関連する全プロジェクトにこのオプションを指定するのもやりたくないので…)
decltype(&D3D12CreateDevice) g_D3D12CreateDevice;
decltype(&DStorageSetConfiguration1) g_DStorageSetConfiguration1;
decltype(&DStorageGetFactory) g_DStorageGetFactory;

// global variables
static DSTORAGE_CONFIGURATION1 g_ds_config = {};
static com_ptr<ID3D12Device> g_d3d12_device;
static com_ptr<IDStorageFactory> g_ds_factory;
static com_ptr<IDStorageQueue> g_ds_queue;
static uint32_t g_ds_staging_buffer_size = 1024 * 1024 * 64;
static bool g_ds_async_free_buffer = true;
static std::mutex g_ds_mutex;


struct DirectStorageInitializer
{
    DirectStorageInitializer()
    {
        if (HMODULE d3d12 = LoadLibraryA("d3d12.dll")) {
            (void*&)g_D3D12CreateDevice = ::GetProcAddress(d3d12, "D3D12CreateDevice");
        }
        if (HMODULE dstorage = LoadLibraryA("dstorage.dll")) {
            (void*&)g_DStorageSetConfiguration1 = ::GetProcAddress(dstorage, "DStorageSetConfiguration1");
            (void*&)g_DStorageGetFactory = ::GetProcAddress(dstorage, "DStorageGetFactory");
        }

        if (!g_ds_factory) {
            if (!g_d3d12_device) {
                if (!g_D3D12CreateDevice || !g_DStorageGetFactory) {
                    return;
                }
                g_D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&g_d3d12_device));

                if (g_DStorageSetConfiguration1) {
                    g_DStorageSetConfiguration1(&g_ds_config);
                }

                g_DStorageGetFactory(IID_PPV_ARGS(g_ds_factory.put()));
                if (!g_d3d12_device || !g_ds_factory) {
                    return;
                }
            }

            if (!g_ds_queue) {
                DSTORAGE_QUEUE_DESC desc{};
                desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
                desc.Priority = DSTORAGE_PRIORITY_NORMAL;
                desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                desc.Device = g_d3d12_device.get();
                g_ds_factory->SetStagingBufferSize(g_ds_staging_buffer_size);
                g_ds_factory->CreateQueue(&desc, IID_PPV_ARGS(g_ds_queue.put()));
                if (!g_ds_queue) {
                    return;
                }
            }
        }
    }
};


void DStorageStream::set_device(ID3D12Device* device, IDStorageFactory* factory, IDStorageQueue* queue)
{
    g_d3d12_device.attach(device);
    g_ds_factory.attach(factory);
    g_ds_queue.attach(queue);
}

void DStorageStream::release_device()
{
    g_d3d12_device = {};
    g_ds_factory = {};
    g_ds_queue = {};
}

void DStorageStream::set_staging_buffer_size(uint32_t size)
{
    g_ds_staging_buffer_size = size;
}

uint32_t DStorageStream::get_staging_buffer_size()
{
    return g_ds_staging_buffer_size;
}

void DStorageStream::disable_bypassio(bool v)
{
    g_ds_config.DisableBypassIO = v;
}

void DStorageStream::force_file_buffering(bool v)
{
    g_ds_config.ForceFileBuffering = v;
    if (v) {
        g_ds_config.DisableBypassIO = TRUE;
    }
}

void DStorageStream::enable_async_free_buffer(bool v)
{
    g_ds_async_free_buffer = v;
}


void BufferDeleter::operator()(void* ptr) const
{
    // huge buffer can take long time to free. so, do it asynchronously.
    auto do_delete = [ptr]() {
        DS_PROFILE_SCOPE("BufferDeleter::operator()");
        ::VirtualFree(ptr, 0, MEM_RELEASE);
        };

    if (g_ds_async_free_buffer) {
        concurrency::create_task(do_delete);
    }
    else {
        do_delete();
    }
}

BufferPtr CreateBuffer(size_t size)
{
    static const size_t page_size = []() {
        ::SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        return si.dwPageSize;
        }();

    // align to page size
    size = (size + page_size - 1) & ~(page_size - 1);
    return BufferPtr((char*)::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}



static std::wstring ToWString(std::string_view str)
{
    size_t wclen = ::MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str.data(), (int)str.size(), nullptr, 0);

    std::wstring wpath;
    wpath.resize(wclen);
    ::MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str.data(), (int)str.size(), wpath.data(), (int)wpath.size());
    return wpath;
}
#pragma endregion Misc


#pragma region DStorageStreamBuf

struct DStorageStreamBuf::PImpl
{
    // movable std::atomic<status_code> to make PImpl implicitly movable
    class AtomicStatusCode : public std::atomic<status_code>
    {
    public:
        using super = std::atomic<status_code>;
        using super::super;

        AtomicStatusCode(AtomicStatusCode&& v) noexcept
        {
            *this = std::move(v);
        }
        AtomicStatusCode& operator=(AtomicStatusCode&& v) noexcept
        {
            this->exchange(v);
            return *this;
        }
    };

    BufferPtr buf_;
    std::wstring path_;
    std::future<HRESULT> future_;

    com_ptr<IDStorageFile> file_;
    com_ptr<ID3D12Fence> fence_;
    std::vector<ScopedHandle> events_;
    uint64_t file_size_ = 0;
    uint64_t read_size_ = 0;
    uint32_t event_pos_ = 0;
    AtomicStatusCode state_{ status_code::idle };
};


DStorageStreamBuf::DStorageStreamBuf()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::DStorageStreamBuf()");
    static DirectStorageInitializer s_resolve_imports;

    pimpl_ = std::make_unique<PImpl>();
}

DStorageStreamBuf::~DStorageStreamBuf()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::~DStorageStreamBuf()");
    close();
    pimpl_ = {};
}

DStorageStreamBuf::DStorageStreamBuf(DStorageStreamBuf&& v) noexcept
    : DStorageStreamBuf()
{
    *this = std::move(v);
}

DStorageStreamBuf& DStorageStreamBuf::operator=(DStorageStreamBuf&& v) noexcept
{
    swap(v);
    return *this;
}

void DStorageStreamBuf::swap(DStorageStreamBuf& v) noexcept
{
    super::swap(v);
    std::swap(pimpl_, v.pimpl_);
}

DStorageStreamBuf::pos_type DStorageStreamBuf::seekoff(off_type off, std::ios::seekdir dir, std::ios::openmode mode)
{
    auto& m = *pimpl_;
    char* head = m.buf_.get();
    char* tail = head + m.file_size_;

    char* current = this->gptr();
    if (dir == std::ios::beg)
        current = head + off;
    else if (dir == std::ios::cur)
        current += off;
    else if (dir == std::ios::end)
        current = tail - off;

    size_t distance = size_t(std::distance(head, current));
    while (m.read_size_ < distance) {
        if (!wait_next_block()) {
            break;
        }
    }
    current = std::min(current, head + m.read_size_);

    this->setg(current, current, tail);
    return pos_type(current - head);
}

DStorageStreamBuf::pos_type DStorageStreamBuf::seekpos(pos_type pos, std::ios::openmode mode)
{
    return seekoff(pos, std::ios::beg, mode);
}

// implement our own xsgetn() / xsputn() because std::streambuf can't handle count of > INT_MAX
// ( https://github.com/microsoft/STL/issues/388 )
std::streamsize DStorageStreamBuf::xsgetn(char* dst, std::streamsize count)
{
    auto& m = *pimpl_;
    char* head = m.buf_.get();
    char* tail = head + m.read_size_;
    char* src = this->gptr();

    size_t remain = count;
    while (remain) {
        size_t n = std::min(remain, size_t(tail - src));
        std::memcpy(dst, src, n);
        dst += n;
        src += n;
        remain -= n;
        this->setg(src, src, tail);

        if (remain) {
            if (underflow() == traits_type::eof()) {
                return count - remain;
            }
            else {
                head = m.buf_.get();
                tail = head + m.read_size_;
                src = this->gptr();
            }
        }
    }
    return count;
}

int DStorageStreamBuf::underflow()
{
    return wait_next_block() ? 0 : traits_type::eof();
}

long DStorageStreamBuf::do_read()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::do_read()");

    auto& m = *pimpl_;
    HRESULT hr;

    auto signal_all = [&m]() {
        for (auto& e : m.events_) {
            ::SetEvent(e.get());
        }
        };

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): OpenFile");

        // OpenFile() to large file may take long.
        hr = g_ds_factory->OpenFile(m.path_.c_str(), IID_PPV_ARGS(m.file_.put()));
        if (FAILED(hr)) {
            m.state_ = status_code::error_file_open_failed;
            signal_all();
            return hr;
        }
        g_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m.fence_.put()));
    }

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): Submit");

        std::unique_lock lock{ g_ds_mutex };

        uint64_t remain = m.file_size_;
        uint64_t progress = 0;
        uint64_t i = 0;
        while (remain > 0) {
            uint32_t read_size = (uint32_t)std::min<uint64_t>(g_ds_staging_buffer_size, remain);
            DSTORAGE_REQUEST request = {};
            request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            request.Source.File.Source = m.file_.get();
            request.Source.File.Offset = progress;
            request.Source.File.Size = read_size;
            request.UncompressedSize = read_size;
            request.Destination.Memory.Buffer = m.buf_.get() + progress;
            request.Destination.Memory.Size = read_size;
            g_ds_queue->EnqueueRequest(&request);

            remain -= read_size;
            progress += read_size;
            g_ds_queue->EnqueueSignal(m.fence_.get(), progress);
            m.fence_->SetEventOnCompletion(progress, m.events_[i].get());

            ++i;
        }

        // submit
        g_ds_queue->Submit();
        m.state_ = status_code::reading;
    }

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): WaitForSingleObject");

        // wait
        ::WaitForSingleObject(m.events_.back().get(), INFINITE);

        DSTORAGE_ERROR_RECORD rec{};
        g_ds_queue->RetrieveErrorRecord(&rec);
        hr = rec.FirstFailure.HResult;
        if (SUCCEEDED(hr)) {
            m.state_ = status_code::completed;
        }
        else {
            m.state_ = status_code::error_unknown;
        }
    }
    return hr;
}

bool DStorageStreamBuf::open(std::wstring&& path)
{
    close();

    DS_PROFILE_SCOPE("DStorageStreamBuf::open()");

    auto& m = *pimpl_;
    if (!g_ds_factory) {
        m.state_ = status_code::error_dll_not_found;
        return false;
    }

    // IDStorageFactory::OpenFile() can be very slow, so we run it asynchronously.
    // also, file size can be obtained by IDStorageFile::GetFileInformation() but it requires IDStorageFactory::OpenFile().
    // therefore, we use std::filesystem::file_size() instead. (which is reasonably fast)

    m.path_ = std::move(path);
    {
        // get file size
        std::error_code ec;
        m.file_size_ = std::filesystem::file_size(std::filesystem::path(m.path_), ec);
        if (ec) {
            m.state_ = status_code::error_file_open_failed;
            return false;
        }
        if (m.file_size_ == 0) {
            m.state_ = status_code::completed;
            return true;
        }

        // allocate buffer
        m.buf_ = CreateBuffer(m.file_size_);
        char* gp = m.buf_.get();
        this->setg(gp, gp, gp);

        // allocate events
        size_t event_count = (m.file_size_ / g_ds_staging_buffer_size) + (m.file_size_ % g_ds_staging_buffer_size ? 1 : 0);
        m.events_.reserve(event_count);
        for (size_t i = 0; i < event_count; ++i) {
            m.events_.emplace_back(::CreateEvent(nullptr, TRUE, FALSE, nullptr));
        }
    }
    m.state_ = status_code::launched;
    m.future_ = std::async(std::launch::async, [this]() { return do_read(); });
    return true;
}

bool DStorageStreamBuf::open(std::string_view path)
{
    return open(ToWString(path));
}

bool DStorageStreamBuf::open(const std::wstring& path)
{
    return open(std::wstring(path));
}

void DStorageStreamBuf::close()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::close()");

    auto& m = *pimpl_;
    if (m.future_.valid()) {
        m.future_.wait();
    }
    m = {};
}

bool DStorageStreamBuf::is_open() const
{
    auto& m = *pimpl_;
    return m.state_.load() >= status_code::launched;
}

DStorageStreamBuf::status_code DStorageStreamBuf::state() const noexcept
{
    return pimpl_->state_.load();
}

bool DStorageStreamBuf::is_complete() const noexcept
{
    return state() == status_code::completed;
}

bool DStorageStreamBuf::wait()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::wait()");

    auto& m = *pimpl_;
    if (m.future_.valid()) {
        m.future_.wait();
        m.future_ = {};
        while (wait_next_block()) {} // for setg() and advance event_pos_
    }
    return m.state_.load() == status_code::completed;
}

bool DStorageStreamBuf::wait_next_block()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::wait_next_block()");

    auto& m = *pimpl_;
    if (m.state_.load() <= status_code::idle) {
        return false;
    }
    else if (m.event_pos_ < m.events_.size()) {
        ::WaitForSingleObject(m.events_[m.event_pos_].get(), INFINITE);
        m.read_size_ = m.fence_ ? m.fence_->GetCompletedValue() : 0;
        m.event_pos_++;

        char* gp = this->gptr();
        this->setg(gp, gp, m.buf_.get() + m.read_size_);
        return true;
    }
    return false;
}

const char* DStorageStreamBuf::data() const noexcept
{
    return pimpl_->buf_.get();
}

size_t DStorageStreamBuf::file_size() const noexcept
{
    return pimpl_->file_size_;
}

size_t DStorageStreamBuf::read_size() const noexcept
{
    return pimpl_->read_size_;
}

BufferPtr&& DStorageStreamBuf::extract() noexcept
{
    return std::move(pimpl_->buf_);
}
#pragma endregion DStorageStreamBuf


#pragma region DStorageStream

DStorageStream::DStorageStream()
    : super(&buf_)
{}

DStorageStream::DStorageStream(DStorageStream&& v) noexcept
    : DStorageStream()
{
    *this = std::move(v);
}

DStorageStream& DStorageStream::operator=(DStorageStream&& v) noexcept
{
    swap(v);
    return *this;
}

bool DStorageStream::open(std::string_view path)
{
    return open(ToWString(path));
}
bool DStorageStream::open(const std::wstring& _path)
{
    std::wstring path = _path;
    return open(std::move(path));
}
bool DStorageStream::open(std::wstring&& path)
{
    if (buf_.open(std::move(path))) {
        this->clear();
        return true;
    }
    else {
        this->setstate(std::ios::failbit);
        return false;
    }
}

void DStorageStream::close()
{
    return buf_.close();
}

bool DStorageStream::is_open() const
{
    return buf_.is_open();
}

void DStorageStream::swap(DStorageStream& v) noexcept
{
    super::swap(v);
    buf_.swap(v.buf_);
}

DStorageStreamBuf* DStorageStream::rdbuf() const noexcept
{
    return const_cast<DStorageStreamBuf*>(&buf_);
}

const char* DStorageStream::data() const noexcept
{
    return buf_.data();
}

size_t DStorageStream::file_size() const noexcept
{
    return buf_.file_size();
}

size_t DStorageStream::read_size() const noexcept
{
    return buf_.read_size();
}

BufferPtr&& DStorageStream::extract() noexcept
{
    return std::move(buf_.extract());
}

DStorageStream::status_code DStorageStream::state() const noexcept
{
    return buf_.state();
}

bool DStorageStream::is_complete() const noexcept
{
    return buf_.is_complete();
}

bool DStorageStream::wait()
{
    return buf_.wait();
}

bool DStorageStream::wait_next_block()
{
    return buf_.wait_next_block();
}

#pragma endregion DStorageStream

} // namespace ist
