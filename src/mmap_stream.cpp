#include "mmap_stream.h"
#include "internal.h"

#include <windows.h>
#include <ppltasks.h>


namespace ist {

#pragma region MemoryMappedFile

struct MemoryMappedFile::PImpl
{
    ScopedHandle file_;
    ScopedHandle mapping_;
    void* data_ = nullptr;
    size_t size_ = 0;
    std::ios::openmode mode_ = 0;

    void unmap();
};


MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& v) noexcept
    : MemoryMappedFile()
{
    swap(v);
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& v) noexcept
{
    swap(v);
    return *this;
}

MemoryMappedFile::MemoryMappedFile()
{
}

MemoryMappedFile::~MemoryMappedFile()
{
    close();
}

void MemoryMappedFile::swap(MemoryMappedFile& v)
{
    std::swap(pimpl_, v.pimpl_);
}

bool MemoryMappedFile::is_open() const
{
    return pimpl_ && pimpl_->file_;
}

void* MemoryMappedFile::data()
{
    return pimpl_ ? pimpl_->data_ : nullptr;
}

const void* MemoryMappedFile::data() const
{
    return pimpl_ ? pimpl_->data_ : nullptr;
}

size_t MemoryMappedFile::size() const
{
    return pimpl_ ? pimpl_->size_ : 0;
}

std::ios::openmode MemoryMappedFile::mode() const
{
    return pimpl_ ? pimpl_->mode_ : 0;
}

bool MemoryMappedFile::open(const char* path, std::ios::openmode mode)
{
    close();

    DS_PROFILE_SCOPE("MemoryMappedFile::open()");

    pimpl_ = std::make_shared<PImpl>();
    auto& m = *pimpl_;

    m.mode_ = mode;
    if (mode & std::ios::out) {
        // open for write
        m.file_ = ScopedHandle(::CreateFileA(path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL));
        if (m.file_) {
            return true;
        }
    }
    else if (mode & std::ios::in) {
        // open for read
        m.file_ = ScopedHandle(::CreateFileA(path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            NULL));
        if (m.file_) {
            m.mapping_ = ScopedHandle(::CreateFileMapping(m.file_.get(), NULL, PAGE_READONLY, 0, 0, NULL));
            if (m.mapping_) {
                LARGE_INTEGER size;
                ::GetFileSizeEx(m.file_.get(), &size);
                m.size_ = size.QuadPart;
                m.data_ = ::MapViewOfFile(m.mapping_.get(), FILE_MAP_READ, 0, 0, 0);
                if (m.data_) {
                    if (m.mode_ & async_prefetch) {
                        concurrency::create_task([data = m.data_, size = m.size_]() {
                            prefetch(data, size);
                            });
                    }

                    return true;
                }
            }
        }
    }

    close();
    return false;
}

void MemoryMappedFile::close()
{
    if (is_open()) {
        auto& m = *pimpl_;
        auto do_close = [pimpl_ = std::move(pimpl_)]() {
            auto& m = *pimpl_;
            m.unmap();
            m.file_.reset();
            m.mode_ = 0;
            };

        if (m.mode_ & async_unmap) {
            concurrency::create_task(std::move(do_close));
        }
        else {
            do_close();
        }
        pimpl_ = {};
    }
}

void MemoryMappedFile::close_with_truncation(size_t filesize)
{
    auto& m = *pimpl_;
    if (is_open() && m.mode_ & std::ios::out) {
        auto do_close = [pimpl_ = std::move(pimpl_), filesize]() {
            auto& m = *pimpl_;
            m.unmap();

            LARGE_INTEGER pos;
            pos.QuadPart = filesize;
            ::SetFilePointer(m.file_.get(), pos.LowPart, &pos.HighPart, FILE_BEGIN);
            ::SetEndOfFile(m.file_.get());

            m.file_.reset();
            m.mode_ = 0;
            };

        if (m.mode_ & async_unmap) {
            concurrency::create_task(std::move(do_close));
        }
        else {
            do_close();
        }
        pimpl_ = {};
    }
}

void* MemoryMappedFile::map(size_t capacity)
{
    if (!is_open()) {
        return nullptr;
    }

    DS_PROFILE_SCOPE("MemoryMappedFile::map()");
    auto& m = *pimpl_;
    m.unmap();

    LARGE_INTEGER size;
    size.QuadPart = capacity;
    m.mapping_ = ScopedHandle(::CreateFileMapping(m.file_.get(), NULL, PAGE_READWRITE, size.HighPart, size.LowPart, NULL));
    if (m.mapping_) {
        m.data_ = ::MapViewOfFile(m.mapping_.get(), FILE_MAP_WRITE, 0, 0, capacity);
        m.size_ = capacity;
    }
    return m.data_;
}

void MemoryMappedFile::PImpl::unmap()
{
    DS_PROFILE_SCOPE("MemoryMappedFile::unmap()");

    if (mapping_) {
        ::UnmapViewOfFile(data_);
        data_ = nullptr;
        size_ = 0;
        mapping_.reset();
    }
}

bool MemoryMappedFile::prefetch(void* ptr, size_t size)
{
    DS_PROFILE_SCOPE("MemoryMappedFile::prefetch()");

    WIN32_MEMORY_RANGE_ENTRY ranges[1];
    ranges[0].VirtualAddress = ptr;
    ranges[0].NumberOfBytes = size;
    return ::PrefetchVirtualMemory(::GetCurrentProcess(), 1, ranges, 0);
}

bool MemoryMappedFile::prefetch(size_t pos, size_t size)
{
    auto& m = *pimpl_;
    return prefetch((char*)m.data_ + pos, size);
}
#pragma endregion MemoryMappedFile


#pragma region MMapStreamBuf

MMapStreamBuf::MMapStreamBuf()
{
}

MMapStreamBuf::MMapStreamBuf(MMapStreamBuf&& v) noexcept
{
    swap(v);
}

MMapStreamBuf::~MMapStreamBuf()
{
    if (mmap_.is_open() && mmap_.mode() & std::ios::out) {
        size_t filesize = std::max(pmax_, size_t(this->pptr() - data()));
        mmap_.close_with_truncation(filesize);
    }
}

MMapStreamBuf& MMapStreamBuf::operator=(MMapStreamBuf&& v) noexcept
{
    swap(v);
    return *this;
}

void MMapStreamBuf::swap(MMapStreamBuf& v)
{
    super::swap(v);
    mmap_.swap(v.mmap_);
    std::swap(pmax_, v.pmax_);
}

bool MMapStreamBuf::open(const char* path, std::ios::openmode mode)
{
    if (mmap_.open(path, mode)) {
        if (mmap_.mode() & std::ios::out) {
            reserve(default_reserve_size);
        }
        else if (mmap_.mode() & std::ios::in) {
            seekpos(0);
        }
        return true;
    }
    return false;
}

bool MMapStreamBuf::open(const std::string& path, std::ios::openmode mode)
{
    return open(path.c_str(), mode);
}

void MMapStreamBuf::close()
{
    mmap_.close();
    pmax_ = 0;
}

bool MMapStreamBuf::is_open() const
{
    return mmap_.is_open();
}

MMapStreamBuf::pos_type MMapStreamBuf::seekoff(off_type off, std::ios::seekdir dir, std::ios::openmode /*mode*/)
{
    std::ios::openmode mode = mmap_.mode();
    auto* head = (char*)mmap_.data();
    auto* tail = head + mmap_.size();

    if (mode & std::ios::out) {
        char* current = this->pptr();
        pmax_ = std::max(pmax_, size_t(current - head));

        if (dir == std::ios::beg)
            current = head + off;
        else if (dir == std::ios::cur)
            current += off;
        else if (dir == std::ios::end)
            current = tail - off;

        this->setp(current, tail);
        return pos_type(current - head);
    }
    else if (mode & std::ios::in) {
        char* current = this->gptr();
        if (dir == std::ios::beg)
            current = head + off;
        else if (dir == std::ios::cur)
            current += off;
        else if (dir == std::ios::end)
            current = tail - off;

        this->setg(current, current, tail);
        return pos_type(current - head);
    }
    return pos_type(-1);
}

MMapStreamBuf::pos_type MMapStreamBuf::seekpos(pos_type pos, std::ios::openmode mode)
{
    return seekoff(pos, std::ios::beg, mode);
}

static size_t ExpandFileSize(size_t base, size_t expand)
{
    constexpr size_t GiB = 1024 * 1024 * 1024;
    size_t size = base;
    size_t required = base + expand;
    while (size < required) {
        size = size < GiB ? size * 2 : size + GiB;
    }
    return size;
}

int MMapStreamBuf::overflow(int c)
{
    char* head = (char*)mmap_.data();
    char* tail = head + mmap_.size();
    char* current = this->pptr();
    if (current < tail) {
        *current++ = (char)c;
        this->setp(current, tail);
    }
    else {
        reserve(ExpandFileSize(mmap_.size(), 1));
        char* head = (char*)mmap_.data();
        char* tail = head + mmap_.size();
        char* current = this->pptr();

        *current++ = (char)c;
        this->setp(current, tail);
    }
    return c;
}

int MMapStreamBuf::underflow()
{
    char* head = (char*)mmap_.data();
    char* tail = head + mmap_.size();
    char* current = this->gptr();
    if (current < tail) {
        int ret = *current++;
        this->setg(current, current, tail);
        return ret;
    }
    else {
        return traits_type::eof();
    }
}

// implement our own xsgetn() / xsputn() because std::streambuf can't handle count of > INT_MAX
// ( https://github.com/microsoft/STL/issues/388 )

std::streamsize MMapStreamBuf::xsgetn(char* ptr, std::streamsize count)
{
    char* head = (char*)mmap_.data();
    char* tail = head + mmap_.size();
    char* current = this->gptr();
    size_t remaining = size_t(tail - current);
    size_t readSize = std::min<size_t>(count, remaining);
    std::memcpy(ptr, current, readSize);

    current += readSize;
    this->setg(current, current, tail);
    return std::streamsize(readSize);
}

std::streamsize MMapStreamBuf::xsputn(const char* ptr, std::streamsize count)
{
    char* head = (char*)mmap_.data();
    char* tail = head + mmap_.size();
    char* current = this->pptr();
    if (current + count > tail) {
        reserve(ExpandFileSize(mmap_.size(), count));
        head = (char*)mmap_.data();
        tail = head + mmap_.size();
        current = this->pptr();
    }
    std::memcpy(current, ptr, count);

    current += count;
    this->setp(current, tail);
    return count;
}

char* MMapStreamBuf::reserve(size_t size)
{
    if (size > mmap_.size()) {
        size_t pos = size_t(this->pptr() - (char*)mmap_.data());
        char* head = (char*)mmap_.map(size);
        char* tail = head + size;
        char* current = head + pos;
        this->setp(current, tail);
    }
    return (char*)mmap_.data();
}

bool MMapStreamBuf::prefetch(size_t position, size_t size)
{
    return mmap_.prefetch(position, size);
}

char* MMapStreamBuf::data()
{
    return (char*)mmap_.data();
}

const char* MMapStreamBuf::data() const
{
    return (char*)mmap_.data();
}

size_t MMapStreamBuf::size() const
{
    return mmap_.size();
}

MemoryMappedFile& MMapStreamBuf::mmap_file()
{
    return mmap_;
}

#pragma endregion MMapStreamBuf


#pragma region MMapStream

MMapStream::MMapStream()
    : super(&buf_)
{
}

MMapStream::MMapStream(MMapStream&& v) noexcept
    : super(&buf_)
{
    swap(v);
}

MMapStream::MMapStream(const char* path, std::ios::openmode mode)
    : super(&buf_)
{
    open(path, mode);
}

MMapStream::MMapStream(const std::string& path, std::ios::openmode mode)
    : super(&buf_)
{
    open(path, mode);
}

MMapStream& MMapStream::operator=(MMapStream&& v) noexcept
{
    swap(v);
    return *this;
}

void MMapStream::swap(MMapStream& v)
{
    super::swap(v);
    buf_.swap(v.buf_);
}

MMapStreamBuf* MMapStream::rdbuf() const
{
    return const_cast<MMapStreamBuf*>(&buf_);
}

bool MMapStream::open(const char* path, std::ios::openmode mode)
{
    if (buf_.open(path, mode)) {
        this->clear();
        return true;
    }
    else {
        this->setstate(std::ios::failbit);
        return false;
    }
}

bool MMapStream::open(const std::string& path, std::ios::openmode mode)
{
    return open(path.c_str(), mode);
}

void MMapStream::close()
{
    buf_.close();
}

bool MMapStream::is_open() const
{
    return buf_.is_open();
}

char* MMapStream::reserve(size_t size)
{
    return buf_.reserve(size);
}

bool MMapStream::prefetch(size_t position, size_t size)
{
    return buf_.prefetch(position, size);
}

char* MMapStream::data()
{
    return buf_.data();
}

const char* MMapStream::data() const
{
    return buf_.data();
}

size_t MMapStream::size() const
{
    return buf_.size();
}

MemoryMappedFile& MMapStream::get_memory_mapped_file()
{
    return buf_.mmap_file();
}
#pragma endregion MMapStream

} // namespace ist
