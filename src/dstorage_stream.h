#pragma once
#include <iostream>
#include <vector>
#include <memory>

struct ID3D12Device;
struct IDStorageFactory;
struct IDStorageQueue;


namespace ist {

// wrapped VirtualAlloc() / VirtualFree()
void* valloc(size_t size);
void vfree(void* ptr);

struct vfree_deleter
{
    void operator()(void* p) const noexcept { vfree(p); }
};


class DStorageStreamBuf : public std::streambuf
{
    using super = std::streambuf;

public:
    // std::vector is difficult to resize without zero-clear. so we use std::unique_ptr<char[]> instead.
    // also, huge buffer allocated by malloc() can take long time to free() for some reason. (I observed 300ms to free 8GB)
    // so we use VirtualAlloc() / VirtualFree() instead.
    using buffer_ptr = std::unique_ptr<char[], vfree_deleter>;

    // movable but non-copyable
    DStorageStreamBuf(DStorageStreamBuf&& v) noexcept;
    DStorageStreamBuf& operator=(DStorageStreamBuf&& v) noexcept;
    DStorageStreamBuf(const DStorageStreamBuf& v) = delete;
    DStorageStreamBuf& operator=(const DStorageStreamBuf& rhs) = delete;

    DStorageStreamBuf();
    ~DStorageStreamBuf() override;

    // overrides
    pos_type seekoff(off_type off, std::ios::seekdir dir, std::ios::openmode mode) final override;
    pos_type seekpos(pos_type pos, std::ios::openmode mode) final override;
    std::streamsize xsgetn(char* ptr, std::streamsize count) final override;
    int underflow() final override;
    using super::gptr;

    bool open(std::string_view path);
    bool open(const std::wstring& path);
    bool open(std::wstring&& path);
    void close();
    bool is_open() const;

    void swap(DStorageStreamBuf& v) noexcept;
    const char* data() const noexcept;
    size_t file_size() const noexcept; // == size of buffer, but potentially data is not read yet.
    size_t read_size() const noexcept; // size of data actually read.
    buffer_ptr&& extract() noexcept;

    // state and wait methods. these are called internally on read(), so you do not need to care about usually.
    enum class status_code {
        idle,
        launched,
        reading,
        completed,

        error_dll_not_found = -10000,
        error_file_open_failed,
        error_unknown,
    };
    status_code state() const noexcept;
    bool is_complete() const noexcept;
    bool wait();
    bool wait_next_block();

private:
    long do_read();

    struct PImpl;
    std::unique_ptr<PImpl> pimpl_;
};


class DStorageStream : public std::istream
{
    using super = std::istream;

public:
    // call this if you want to share existing device/factory/queue, otherwise these will be created internally.
    static void set_device(ID3D12Device* device, IDStorageFactory* factory = nullptr, IDStorageQueue* queue = nullptr);
    static void release_device();

    // staging buffer size is the maximum read size per request.
    // when large file is opened, it will be split into multiple requests.
    static void set_staging_buffer_size(uint32_t size);
    static uint32_t get_staging_buffer_size();

    // disable Bypass IO even if the drive supports.
    static void disable_bypassio(bool v);

    // enable file buffering. this may improve performance on HDD, but may worsen on SSD.
    // this implicitly disables Bypass IO.
    static void force_file_buffering(bool v);

public:
    using buffer_ptr = DStorageStreamBuf::buffer_ptr;
    using status_code = DStorageStreamBuf::status_code;

    // movable but non-copyable
    DStorageStream(DStorageStream&& v) noexcept;
    DStorageStream& operator=(DStorageStream&& v) noexcept;
    DStorageStream(const DStorageStream& v) = delete;
    DStorageStream& operator=(const DStorageStream& rhs) = delete;

    DStorageStream();

    bool open(std::string_view path, std::ios::openmode mode = std::ios::in);
    bool open(const std::wstring& path, std::ios::openmode mode = std::ios::in);
    bool open(std::wstring&& path, std::ios::openmode mode = std::ios::in);
    void close();
    bool is_open() const;

    void swap(DStorageStream& v) noexcept;
    DStorageStreamBuf* rdbuf() const noexcept;
    const char* data() const noexcept;
    size_t file_size() const noexcept;
    size_t read_size() const noexcept;
    buffer_ptr&& extract() noexcept;

    status_code state() const noexcept;
    bool is_complete() const noexcept;
    bool wait();
    bool wait_next_block();

private:
    DStorageStreamBuf buf_;
};

} // namespace ist
