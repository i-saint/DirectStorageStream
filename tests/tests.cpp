﻿#include "mmap_stream.h"
#include "dstorage_stream.h"
#include "internal.h"

#include <fstream>
#include <random>
#include <vector>
#include <span>
#include <chrono>
#include <filesystem>


#define check(...) if(!(__VA_ARGS__)) { throw std::runtime_error("failed: " #__VA_ARGS__ "\n"); }

using nanosec = uint64_t;
nanosec NowNS()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}


static void Test_MMapStream()
{
    DS_PROFILE_SCOPE("Test_MMapStream()");

    const char* filename = "Test_MMapStreamStream.bin";
    const uint32_t block_size = ist::MMapStreamBuf::default_reserve_size;
    const uint32_t file_size = block_size * 2 + 1234 * 4;

    std::vector<uint32_t> data;
    data.resize(file_size / sizeof(uint32_t));
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = (uint32_t)i;
    }

    {
        ist::MMapStream of;
        of.open(filename, std::ios::out);
        of.write((char*)data.data(), data.size() * sizeof(uint32_t));
    }

    {
        ist::MMapStream ifs;
        ifs.open(filename, std::ios::in);
        check(ifs.size() == file_size);

        std::vector<uint32_t> data2;
        data2.resize(ifs.size() / sizeof(uint32_t));

        ifs.read((char*)data2.data(), ifs.size());
        check(data == data2);
    }
}

static void Test_DStorageStream()
{
    DS_PROFILE_SCOPE("Test_DStorageStream()");

    const char* filename = "Test_DStorageStream.bin";
    const uint32_t block_size = ist::DStorageStream::get_staging_buffer_size();
    const uint32_t file_size = block_size * 2 + 1234 * 4;
    {
        std::ofstream of(filename, std::ios::out | std::ios::binary);

        std::vector<uint32_t> data;
        data.resize(file_size / sizeof(uint32_t));
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = (uint32_t)i;
        }
        of.write((char*)data.data(), data.size() * sizeof(uint32_t));
    }

    // test wait_next_block()
    {
        ist::DStorageStream ifs;
        ifs.open(filename, std::ios::in);
        check(ifs.is_open());

        ifs.wait_next_block();
        check(ifs.read_size() == block_size);
        ifs.wait_next_block();
        check(ifs.read_size() == block_size * 2);
        ifs.wait_next_block();
        check(ifs.read_size() == file_size);
    }

    // test seekg()
    {
        ist::DStorageStream ifs;
        ifs.open(filename, std::ios::in);

        ifs.seekg(1);
        check(ifs.read_size() == block_size);
        ifs.seekg(block_size * 2 + 1);
        check(ifs.read_size() == file_size);
    }

    // test undeflow()
    {
        ist::DStorageStream ifs;
        ifs.open(filename, std::ios::in);

        std::vector<uint32_t> data;
        data.resize(file_size / sizeof(uint32_t));

        ifs.read((char*)data.data(), 16);
        check(ifs.read_size() == block_size);

        ifs.read((char*)data.data() + 16, block_size - 16);
        check(ifs.read_size() == block_size);

        ifs.read((char*)data.data() + block_size, file_size - block_size);
        check(ifs.read_size() == file_size);
    }
}

static std::vector<float> GenRandom(size_t n, int seed = 0)
{
    std::mt19937 engine(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<float> result;
    ist::DirtyResize(result, n);
    for (size_t i = 0; i < n; ++i) {
        result[i] = dist(engine);
    }
    return result;
}

template<class T>
static double CalcTotal(const char* path)
{
    double total = 0.0;
    if constexpr (std::is_same_v<T, std::fstream>) {
        std::fstream ifs;
        ifs.open(path, std::ios::in | std::ios::binary);
        if (ifs) {
            size_t size = 0;
            ifs.seekg(0, std::ios::end);
            size = ifs.tellg() / sizeof(float);
            ifs.seekg(0, std::ios::beg);

            std::vector<float> data;
            ist::DirtyResize(data, size);
            ifs.read((char*)data.data(), size * sizeof(float));

            for (float v : data) {
                total += v;
            }
        }
    }
    else if constexpr (std::is_same_v<T, ist::MMapStream>) {
        ist::MMapStream ifs;
        if (ifs.open(path, std::ios::in)) {
            std::span data{ (const float*)ifs.data(), ifs.size() / sizeof(float) };
            for (float v : data) {
                total += v;
            }
        }
    }
    else if constexpr (std::is_same_v<T, ist::DStorageStream>) {
        ist::DStorageStream ifs;
        if (ifs.open(path)) {
            size_t pos = 0;
            while (ifs.wait_next_block()) {
                std::span data{ (const float*)(ifs.data() + pos), (ifs.read_size() - pos) / sizeof(float) };
                for (float v : data) {
                    total += v;
                }
                pos = ifs.read_size();
            }
        }
    }
    else {
        static_assert(std::is_same_v<T, std::fstream>);
    }
    return total;
}

static void Test_Benchmark()
{
    DS_PROFILE_SCOPE("Test_Benchmark()");

    constexpr size_t KiB = 1024;
    constexpr size_t MiB = 1024 * 1024;
    constexpr size_t GiB = 1024 * 1024 * 1024;
    std::tuple<const char*, size_t> table[] = {
        {"data_4K.bin", 4 * KiB},
        {"data_256K.bin", 256 * KiB},
        {"data_4MB.bin", 4 * MiB},
        {"data_64MB.bin", 64 * MiB},
        {"data_256MB.bin", 256 * MiB},
        {"data_1GB.bin", 1 * GiB},
        {"data_8GB.bin", 8 * GiB},
    };

    {
        int i = 0;
        for (const auto& [filename, size] : table) {
            if (!std::filesystem::exists(filename)) {
                printf("making %s...", filename);
                std::ofstream of(filename, std::ios::out | std::ios::binary);
                std::vector<float> data = GenRandom(size / sizeof(float), i);
                of.write((const char*)data.data(), data.size() * sizeof(float));
                printf(" done\n");
            }
            ++i;
        }
    }

    constexpr int num_try = 3;
    for (const auto& [filename, size] : table) {
        double total_fstream = 0;
        double total_mmap = 0;
        double total_dstorage = 0;

        printf("file size %llu:\n", size);
        {
            for (int i = 0; i < num_try; ++i) {
                DS_PROFILE_SCOPE("DStorageStream (%lluB)", size);
                nanosec start = NowNS();
                total_dstorage = CalcTotal<ist::DStorageStream>(filename);
                double elapsed = (NowNS() - start) / 1000000000.0;
                double mbps = (double)size / (1024 * 1024) / elapsed;
                printf("DStorageStream:\t%.2lfms (%.1lfMB/s)\n", elapsed * 1000, mbps);
            }
            for (int i = 0; i < num_try; ++i) {
                DS_PROFILE_SCOPE("MMapStream (%lluB)", size);
                nanosec start = NowNS();
                total_mmap = CalcTotal<ist::MMapStream>(filename);
                double elapsed = (NowNS() - start) / 1000000000.0;
                double mbps = (double)size / (1024 * 1024) / elapsed;
                printf("MMapStream:\t%.2lfms (%.1lfMB/s)\n", elapsed * 1000, mbps);
            }
            for (int i = 0; i < num_try; ++i) {
                DS_PROFILE_SCOPE("std::fstream (%lluB)", size);
                nanosec start = NowNS();
                total_fstream = CalcTotal<std::fstream>(filename);
                double elapsed = (NowNS() - start) / 1000000000.0;
                double mbps = (double)size / (1024 * 1024) / elapsed;
                printf("std::fstream:\t%.2lfms (%.1lfMB/s)\n", elapsed * 1000, mbps);
            }
            check(total_fstream == total_mmap);
            check(total_fstream == total_dstorage);
        }
    }
}


int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string_view param(argv[i]);
        if (param == "--disable-bypassio") {
            ist::DStorageStream::disable_bypassio(true);
        }
        else if (param == "--force-file-buffering") {
            ist::DStorageStream::force_file_buffering(true);
        }
    }

    Test_MMapStream();
    Test_DStorageStream();
    Test_Benchmark();
}
