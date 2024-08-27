#pragma once
#include <iostream>
#include <memory>

namespace ist {

class MemoryMappedFile
{
public:
    static constexpr std::ios::openmode async_prefetch  = 0x1000;
    static constexpr std::ios::openmode async_unmap     = 0x2000;

public:
    // movable but non-copyable
    MemoryMappedFile(MemoryMappedFile&& v) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& v) noexcept;
    MemoryMappedFile(const MemoryMappedFile& v) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile& v) = delete;

    MemoryMappedFile();
    ~MemoryMappedFile();
    void swap(MemoryMappedFile& v);

    bool open(const char* path, std::ios::openmode mode);
    void close();

    void* map(size_t size);
    void unmap();
    void truncate(size_t filesize);
    bool prefetch(size_t position, size_t size);
    static bool prefetch(void* ptr, size_t size);

    bool is_open() const;
    void* data();
    const void* data() const;
    size_t size() const;
    std::ios::openmode mode() const;

private:
    struct PImpl;
    std::unique_ptr<PImpl> pimpl_;
};


class MMapStreamBuf : public std::streambuf
{
    using super = std::streambuf;

public:
    static constexpr size_t default_reserve_size = 1024 * 1024 * 16;

    // movable but non-copyable
    MMapStreamBuf(const MemoryMappedFile& v) = delete;
    MMapStreamBuf& operator=(const MMapStreamBuf& rhs) = delete;
    MMapStreamBuf(MMapStreamBuf&& v) noexcept;
    MMapStreamBuf& operator=(MMapStreamBuf&& v) noexcept;

    MMapStreamBuf();
    ~MMapStreamBuf();

    // std::filebuf compatible
    void swap(MMapStreamBuf& buf);
    bool open(const char* path, std::ios::openmode mode);
    bool open(const std::string& path, std::ios::openmode mode);
    void close();
    bool is_open() const;

    // overrides
    pos_type seekoff(off_type off,
                     std::ios::seekdir dir,
                     std::ios::openmode mode = std::ios::in | std::ios::out) final override;
    pos_type seekpos(pos_type pos, std::ios::openmode mode = std::ios::in | std::ios::out) final override;
    int overflow(int c) final override;
    int underflow() final override;
    std::streamsize xsgetn(char* ptr, std::streamsize count) final override;
    std::streamsize xsputn(const char* ptr, std::streamsize count) final override;

    char* reserve(size_t size);
    bool prefetch(size_t position, size_t size);
    char* data();
    const char* data() const;
    size_t size() const;
    MemoryMappedFile& mmap_file();

public:
    MemoryMappedFile mmap_;
    size_t pmax_ = 0;
};


class MMapStream : public std::iostream
{
    using super = std::iostream;

public:
    static constexpr std::ios::openmode async_prefetch = MemoryMappedFile::async_prefetch;
    static constexpr std::ios::openmode async_unmap = MemoryMappedFile::async_unmap;

public:
    // movable but non-copyable
    MMapStream(MMapStream&& v) noexcept;
    MMapStream& operator=(MMapStream&& v) noexcept;
    MMapStream(const MMapStream& v) = delete;
    MMapStream& operator=(const MMapStream& rhs) = delete;

    MMapStream();
    MMapStream(const char* path, std::ios::openmode mode);
    MMapStream(const std::string& path, std::ios::openmode mode);

    void swap(MMapStream& buf);
    MMapStreamBuf* rdbuf() const;

    bool open(const char* path, std::ios::openmode mode);
    bool open(const std::string& path, std::ios::openmode mode);
    void close();
    bool is_open() const;

    char* reserve(size_t size);
    bool prefetch(size_t position, size_t size);
    char* data();
    const char* data() const;
    size_t size() const;
    MemoryMappedFile& get_memory_mapped_file();

private:
    MMapStreamBuf buf_;
};

} // namespace ist
