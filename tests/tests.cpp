#include "mmap_stream.h"
#include "dstorage_stream.h"
#include "internal.h"

#include <fstream>
#include <random>
#include <vector>
#include <span>
#include <chrono>
#include <cassert>

using millisec = uint64_t;
millisec NowMS()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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
        assert(ifs.size() == file_size);

        std::vector<uint32_t> data2;
        data2.resize(ifs.size() / sizeof(uint32_t));

        ifs.read((char*)data2.data(), ifs.size());
        assert(data == data2);
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
        assert(ifs.is_open());

        ifs.wait_next_block();
        assert(ifs.read_size() == block_size);
        ifs.wait_next_block();
        assert(ifs.read_size() == block_size * 2);
        ifs.wait_next_block();
        assert(ifs.read_size() == file_size);
    }

    // test seekg()
    {
        ist::DStorageStream ifs;
        ifs.open(filename, std::ios::in);

        ifs.seekg(1);
        assert(ifs.read_size() == block_size);
        ifs.seekg(block_size * 2 + 1);
        assert(ifs.read_size() == file_size);
    }

    // test undeflow()
    {
        ist::DStorageStream ifs;
        ifs.open(filename, std::ios::in);

        std::vector<uint32_t> data;
        data.resize(file_size / sizeof(uint32_t));

        ifs.read((char*)data.data(), 16);
        assert(ifs.read_size() == block_size);

        ifs.read((char*)data.data() + 16, block_size - 16);
        assert(ifs.read_size() == block_size);

        ifs.read((char*)data.data() + block_size, file_size - block_size);
        assert(ifs.read_size() == file_size);
    }
}

static void Test_Benchmark()
{
    // create an 8 GB file containing a sequence of random floats.
    // measure the time that takes to read the file and calculate the sum.

    DS_PROFILE_SCOPE("Test_Benchmark()");

    if (!std::filesystem::exists("data.bin"))
    {
        printf("maiking data.bin...\n");
        millisec start = NowMS();

        std::ofstream of("data.bin", std::ios::out | std::ios::binary);

        std::random_device seed_gen;
        std::mt19937 engine(seed_gen());
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        std::vector<float> data;
        data.resize(1024 * 1024 * 1024 / sizeof(float));
        for (int i = 0; i < 8; ++i) {
            for (auto& d : data) {
                d = dist(engine);
            }
            of.write((const char*)data.data(), data.size() * sizeof(float));
            printf("%llu bytes written.\n", 1024llu * 1024llu * 1024llu * (i + 1));
        }

        millisec elapsed = NowMS() - start;
        printf("done. (%lfs)\n", elapsed / 1000.0);
    }


    constexpr int num_try = 4;
    double elapsed_fstream = 0;
    double elapsed_mmap = 0;
    double elapsed_dstorage = 0;
    double total_fstream = 0;
    double total_mmap = 0;
    double total_dstorage = 0;

    auto testStdFStream = [&]() {
        DS_PROFILE_SCOPE("std::fstream");

        double total = 0.0;
        std::fstream ifs;
        std::vector<float> data;

        millisec start = NowMS();
        ifs.open("data.bin", std::ios::in | std::ios::binary);
        if (ifs)
        {
            size_t size = 0;
            ifs.seekg(0, std::ios::end);
            size = ifs.tellg() / sizeof(float);
            ifs.seekg(0, std::ios::beg);

            data.resize(size);
            ifs.read((char*)data.data(), size * sizeof(float));

            for (float v : data) {
                total += v;
            }
        }
        double elapsed = (NowMS() - start) / 1000.0;
        elapsed_fstream += elapsed;
        total_fstream = total;

        double mbps = (data.size() * 4 / (1024 * 1024) / elapsed);
        printf("std::fstream:\t%.1lfs (%.1lfMB/s)\n", elapsed, mbps);
        return total;
        };

    auto testMMFS = [&]() {
        DS_PROFILE_SCOPE("MMapStream");

        double total = 0.0;
        ist::MMapStream ifs;

        millisec start = NowMS();
        if (ifs.open("data.bin", std::ios::in))
        {
            std::span data{ (const float*)ifs.data(), ifs.size() / sizeof(float) };
            for (float v : data) {
                total += v;
            }
        }
        double elapsed = (NowMS() - start) / 1000.0;
        elapsed_mmap += elapsed;
        total_mmap = total;

        double mbps = (ifs.size() / (1024 * 1024) / elapsed);
        printf("MMFStream:\t%.1lfs (%.1lfMB/s)\n", elapsed, mbps);
        return total;
        };

    auto testDStorageFS = [&]() {
        DS_PROFILE_SCOPE("DStorageStream");

        double total = 0.0;
        ist::DStorageStream ifs;

        millisec start = NowMS();
        if (ifs.open("data.bin"))
        {
            size_t pos = 0;
            while (ifs.wait_next_block()) {
                std::span data{ (const float*)(ifs.data() + pos), (ifs.read_size() - pos) / sizeof(float) };
                for (float v : data) {
                    total += v;
                }
                pos = ifs.read_size();
                //printf("%llu\n", pos);
            }
        }
        double elapsed = (NowMS() - start) / 1000.0;
        elapsed_dstorage += elapsed;
        total_dstorage = total;

        double mbps = (ifs.file_size() / (1024 * 1024) / elapsed);
        printf("DStorageStream:\t%.1lfs (%.1lfMB/s)\n", elapsed, mbps);
        return total;
        };


    for (int i = 0; i < num_try; ++i) {
        testMMFS();
        testDStorageFS();
        testStdFStream();

        assert(total_fstream == total_mmap);
        assert(total_fstream == total_dstorage);
    }
    printf("std::fstream:\t%lfs\n", (elapsed_fstream / num_try));
    printf("MMFStream:\t%lfs\n", (elapsed_mmap / num_try));
    printf("DStorageStream:\t%lfs\n", (elapsed_dstorage / num_try));

}

int main()
{
    Test_MMapStream();
    Test_DStorageStream();
    Test_Benchmark();
}
