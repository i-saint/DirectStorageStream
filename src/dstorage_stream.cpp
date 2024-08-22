#include "dstorage_stream.h"
#include "internal.h"

#include <atomic>
#include <chrono>
#include <dstorage.h>
#include <dxgi1_4.h>
#include <winrt/base.h>


namespace ist {

#pragma region Misc
using winrt::check_hresult;
using winrt::com_ptr;

// functions
// d3d12.dll や dstorage.dll がないと exe が起動すらしなくなってしまうのは避けたいため、
// 古き悪しき LoadLibrary() & GetProcAddress() でインポートを解決する。
// (/DELAYLOAD ならスマートに解決できるのだが、関連する全プロジェクトにこのオプションを指定するのもやりたくないので…)
decltype(&D3D12CreateDevice) g_D3D12CreateDevice;
decltype(&DStorageGetFactory) g_DStorageGetFactory;
struct DSResolveImports
{
    DSResolveImports()
    {
        if (HMODULE d3d12 = LoadLibraryA("d3d12.dll"))
        {
            (void*&)g_D3D12CreateDevice = ::GetProcAddress(d3d12, "D3D12CreateDevice");
        }
        if (HMODULE dstorage = LoadLibraryA("dstorage.dll"))
        {
            (void*&)g_DStorageGetFactory = ::GetProcAddress(dstorage, "DStorageGetFactory");
        }
    }
};


// global variables
static com_ptr<ID3D12Device> g_d3d12_device;
static com_ptr<IDStorageFactory> g_ds_factory;
static com_ptr<IDStorageQueue> g_ds_queue;
static uint32_t g_ds_staging_buffer_size = 1024 * 1024 * 64;



void DStorageStream::set_device(ID3D12Device* device, IDStorageFactory* factory, IDStorageQueue* queue)
{
    g_d3d12_device.attach(device);
    g_ds_factory.attach(factory);
    g_ds_queue.attach(queue);
}

void DStorageStream::reset_device()
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


static std::wstring ToWString(std::string_view str)
{
    size_t wclen = ::MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str.data(), (int)str.size(), nullptr, 0);

    std::wstring wpath;
    wpath.resize(wclen);
    ::MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str.data(), (int)str.size(), wpath.data(), (int)wpath.size());
    return wpath;
}

// resize std::vector without zero-clear
// ( https://qiita.com/i_saint/items/59c394a28a5244ec94e1 )
template<class T>
static inline void DirtyResize(std::vector<T>& dst, size_t new_size)
{
#ifdef _DEBUG
    dst.resize(new_size);
#else
    struct Proxy
    {
        T value;
#pragma warning(disable:26495)
        // value is uninitialized by intention
        Proxy() {}
#pragma warning(default:26495)
    };
    static_assert(sizeof(T) == sizeof(Proxy));

    if (new_size <= dst.capacity()) {
        reinterpret_cast<std::vector<Proxy>&>(dst).resize(new_size);
    }
    else {
        std::vector<T> tmp;
        size_t new_capacity = std::max<size_t>(dst.capacity() * 2, new_size);
        tmp.reserve(new_capacity);
        reinterpret_cast<std::vector<Proxy>&>(tmp).resize(new_size);
        std::memcpy(tmp.data(), dst.data(), sizeof(T) * dst.size());
        dst.swap(tmp);
    }
#endif
}
#pragma endregion Misc


#pragma region DStorageStreamBuf

struct DStorageStreamBuf::PImpl
{
    std::vector<char> buf_;
    std::wstring path_;
    std::future<HRESULT> future_;

    com_ptr<IDStorageFile> file_;
    com_ptr<ID3D12Fence> fence_;
    ScopedHandle fence_event_;
    uint64_t read_size_ = 0;
    std::atomic<status_code> state_{ status_code::idle };


    PImpl(const PImpl&) = delete;
    PImpl& operator=(const PImpl&) = delete;
    PImpl() {}
    PImpl(PImpl&& v) noexcept
    {
        *this = std::move(v);
    }
    PImpl& operator=(PImpl&& v) noexcept
    {
        swap(v);
        return *this;
    }
    void swap(PImpl& v) noexcept
    {
        std::swap(buf_, v.buf_);
        std::swap(path_, v.path_);
        std::swap(future_, v.future_);

        std::swap(file_, v.file_);
        std::swap(fence_, v.fence_);
        std::swap(fence_event_, v.fence_event_);
        std::swap(read_size_, v.read_size_);
        state_.exchange(v.state_);
    }
};


DStorageStreamBuf::DStorageStreamBuf()
{
    static_assert(sizeof(DStorageStreamBuf::members_) >= sizeof(PImpl));
    pimpl_ = new (members_) PImpl();


    static DSResolveImports s_resolve_imports;
    if (!g_ds_factory)
    {
        if (!g_d3d12_device)
        {
            if (!g_D3D12CreateDevice || !g_DStorageGetFactory)
            {
                return;
            }
            g_D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&g_d3d12_device));
            g_DStorageGetFactory(IID_PPV_ARGS(g_ds_factory.put()));
            if (!g_d3d12_device || !g_ds_factory)
            {
                return;
            }
        }

        if (!g_ds_queue)
        {
            DSTORAGE_QUEUE_DESC desc{};
            desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
            desc.Priority = DSTORAGE_PRIORITY_NORMAL;
            desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            desc.Device = g_d3d12_device.get();
            g_ds_factory->SetStagingBufferSize(g_ds_staging_buffer_size);
            g_ds_factory->CreateQueue(&desc, IID_PPV_ARGS(g_ds_queue.put()));
            if (!g_ds_queue)
            {
                return;
            }
        }
    }
}

DStorageStreamBuf::~DStorageStreamBuf()
{
    close();
    pimpl_->~PImpl();
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

DStorageStreamBuf::pos_type DStorageStreamBuf::seekoff(off_type off, std::ios::seekdir dir, std::ios::openmode mode)
{
    auto& m = *pimpl_;
    status_code s = m.state_.load();
    bool is_active = s >= status_code::launched && s < status_code::completed;
    if (is_active && m.read_size_ == 0) {
        wait_next_block();
    }

    auto& buf = pimpl_->buf_;
    char* head = buf.data();
    char* tail = head + buf.size();

    char* current = this->gptr();
    if (dir == std::ios::beg)
        current = head + off;
    else if (dir == std::ios::cur)
        current += off;
    else if (dir == std::ios::end)
        current = tail - off;

    while (m.read_size_ < static_cast<uint64_t>(std::distance(head, current))) {
        wait_next_block();
        if (m.state_.load() == status_code::completed) {
            break;
        }
    }

    this->setg(current, current, tail);
    return pos_type(current - head);
}

DStorageStreamBuf::pos_type DStorageStreamBuf::seekpos(pos_type pos, std::ios::openmode mode)
{
    return seekoff(pos, std::ios::beg, mode);
}

// implement owr own xsgetn() / xsputn() because std::streambuf can't handle count of > INT_MAX
// ( https://github.com/microsoft/STL/issues/388 )
std::streamsize DStorageStreamBuf::xsgetn(char* dst, std::streamsize count)
{
    auto& m = *pimpl_;
    if (m.read_size_ == 0) {
        if (underflow() == traits_type::eof()) {
            return 0;
        }
    }

    auto& buf = m.buf_;
    char* head = buf.data();
    char* tail = head + m.read_size_;
    char* src = this->gptr();

    size_t remain = count;
    while (remain)
    {
        size_t n = std::min<size_t>(remain, size_t(tail - src));
        std::memcpy(dst, src, n);
        dst += n;
        src += n;
        remain -= n;
        this->setg(src, src, tail);

        if (remain)
        {
            if (underflow() == traits_type::eof())
            {
                return count - remain;
            }
            else
            {
                head = buf.data();
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
    uint64_t file_size;

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): OpenFile");

        hr = g_ds_factory->OpenFile(m.path_.c_str(), IID_PPV_ARGS(m.file_.put()));
        if (FAILED(hr)) {
            m.state_ = status_code::error_file_open_failed;
            return hr;
        }
    }
    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): GetFileInformation");

        BY_HANDLE_FILE_INFORMATION info{};
        hr = m.file_->GetFileInformation(&info);
        if (FAILED(hr)) {
            m.state_ = status_code::error_file_open_failed;
            return hr;
        }
        file_size = (uint64_t)info.nFileSizeHigh << 32 | (uint64_t)info.nFileSizeLow;
    }

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): Submit");

        // allocate buffer
        auto& buf = pimpl_->buf_;
        DirtyResize(buf, file_size);
        m.state_ = status_code::reading;

        g_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m.fence_.put()));

        uint64_t remain = file_size;
        uint64_t progress = 0;
        while (remain > 0)
        {
            uint32_t readSize = (uint32_t)std::min<uint64_t>(g_ds_staging_buffer_size, remain);
            DSTORAGE_REQUEST request = {};
            request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            request.Source.File.Source = m.file_.get();
            request.Source.File.Offset = progress;
            request.Source.File.Size = readSize;
            request.UncompressedSize = readSize;
            request.Destination.Memory.Buffer = buf.data() + progress;
            request.Destination.Memory.Size = readSize;
            g_ds_queue->EnqueueRequest(&request);

            remain -= readSize;
            progress += readSize;
            g_ds_queue->EnqueueSignal(m.fence_.get(), progress);
        }

        // fire event on complete
        m.fence_event_ = ScopedHandle(::CreateEvent(nullptr, FALSE, FALSE, nullptr));
        m.fence_->SetEventOnCompletion(progress, m.fence_event_.get());

        // submit
        g_ds_queue->Submit();
    }

    {
        DS_PROFILE_SCOPE("DStorageStreamBuf::do_read(): WaitForSingleObject");

        // wait
        ::WaitForSingleObject(m.fence_event_.get(), INFINITE);

        DSTORAGE_ERROR_RECORD errorRecord{};
        g_ds_queue->RetrieveErrorRecord(&errorRecord);
        if (SUCCEEDED(errorRecord.FirstFailure.HResult)) {
            m.state_ = status_code::completed;
            return S_OK;
        }
        else {
            m.state_ = status_code::error_unknown;
            return errorRecord.FirstFailure.HResult;
        }
    }
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
        wait_next_block(); // for setg()
        return true;
    }
    return false;
}

bool DStorageStreamBuf::wait_next_block()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::wait_next_block()");

    bool r = false;

    auto& m = *pimpl_;
    status_code s = m.state_.load();
    if (s >= status_code::launched && s < status_code::completed) {
        size_t pos = m.read_size_;
        for (;;) {
            uint64_t v = m.fence_ ? m.fence_->GetCompletedValue() : 0;
            if (v > pos) {
                m.read_size_ = v;
                r = true;
                break;
            }
            else if (s = m.state_.load(); s < status_code::idle || s == status_code::completed) {
                break;
            }
            else {
                // ** busy loop **
                ::Sleep(0);
            }
        }
    }
    else if (s == status_code::completed) {
        uint64_t v = m.fence_ ? m.fence_->GetCompletedValue() : 0;
        if (m.read_size_ != v) {
            m.read_size_ = v;
            r = true;
        }
    }

    if (r) {
        char* gp = gptr();
        if (!gp) {
            gp = m.buf_.data();
        }
        this->setg(gp, gp, m.buf_.data() + m.read_size_);
    }
    return r;
}


bool DStorageStreamBuf::open(std::string_view path)
{
    return open(ToWString(path));
}

bool DStorageStreamBuf::open(const std::wstring& path)
{
    return open(std::wstring(path));
}

bool DStorageStreamBuf::open(std::wstring&& path)
{
    close();

    DS_PROFILE_SCOPE("DStorageStreamBuf::open()");

    auto& m = *pimpl_;
    if (!g_ds_factory)
    {
        m.state_ = status_code::error_dll_not_found;
        return false;
    }

    m.path_ = std::move(path);
    m.state_ = status_code::launched;
    m.future_ = std::async(std::launch::async, [this]() { return do_read(); });
    return true;
}

void DStorageStreamBuf::close()
{
    DS_PROFILE_SCOPE("DStorageStreamBuf::close()");

    if (pimpl_->future_.valid()) {
        pimpl_->future_.wait();
    }
    *pimpl_ = {};
}

bool DStorageStreamBuf::is_open() const
{
    status_code s = pimpl_->state_.load();
    return s >= status_code::launched && s <= status_code::completed;
}

void DStorageStreamBuf::swap(DStorageStreamBuf& v) noexcept
{
    super::swap(v);
    pimpl_->swap(*v.pimpl_);
}

const char* DStorageStreamBuf::data() const noexcept
{
    return pimpl_->buf_.data();
}

size_t DStorageStreamBuf::file_size() const noexcept
{
    return pimpl_->buf_.size();
}

size_t DStorageStreamBuf::read_size() const noexcept
{
    return pimpl_->read_size_;
}

std::vector<char>&& DStorageStreamBuf::extract() noexcept
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

bool DStorageStream::open(std::string_view path, std::ios::openmode)
{
    return buf_.open(path);
}
bool DStorageStream::open(const std::wstring& path, std::ios::openmode)
{
    return buf_.open(path);
}
bool DStorageStream::open(std::wstring&& path, std::ios::openmode)
{
    return buf_.open(std::move(path));
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

std::vector<char>&& DStorageStream::extract() noexcept
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
