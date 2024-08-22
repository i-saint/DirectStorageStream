#pragma once
#include <iostream>
#include <filesystem>
#include <functional>
#include <future>

struct ID3D12Device;
struct IDStorageFactory;
struct IDStorageQueue;


namespace ist {

class DStorageStreamBuf : public std::streambuf
{
    using super = std::streambuf;

public:
    enum class status_code {
        idle,
        launched,
        reading,
        completed,

        error_dll_not_found = -10000,
        error_file_open_failed,
        error_unknown,
    };

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
    size_t file_size() const noexcept;
    size_t read_size() const noexcept;
    std::vector<char>&& extract() noexcept;

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
    // call set_device() if you want to share the existing device, otherwise the device will be created internally.
    static void set_device(ID3D12Device* device, IDStorageFactory* factory = nullptr, IDStorageQueue* queue = nullptr);
    static void reset_device();

    // staging buffer size is the maximum read size per request.
    // when large file is opened, it will be split into multiple requests.
    static void set_staging_buffer_size(uint32_t size);
    static uint32_t get_staging_buffer_size();

public:
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
    std::vector<char>&& extract() noexcept;

    status_code state() const noexcept;
    bool is_complete() const noexcept;
    bool wait();
    bool wait_next_block(); // ** busy loop **

private:
    DStorageStreamBuf buf_;
};

} // namespace ist
