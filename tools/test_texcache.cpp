// Standalone texture-cache regression checks and microbenchmark.
// See PERFORMANCE.md for build/run instructions.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "GPU3D_Texcache.h"
#include "CRC32.h"

using namespace melonDS;

struct UnusedLoader {};
using Cache = Texcache<UnusedLoader, u32>;

static void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static bool OverlapsDirty(u32 start, u32 size, const std::vector<u64>& dirty, u32 vramSize)
{
    // Independent, deliberately simple reference: inspect each 512-byte block.
    if (size == 0) return false;
    for (u32 bit = start / VRAMDirtyGranularity;
         bit <= (start + size - 1) / VRAMDirtyGranularity; bit++)
    {
        u32 wrapped = bit % (vramSize / VRAMDirtyGranularity);
        if (dirty[wrapped / 64] & (1ULL << (wrapped % 64))) return true;
    }
    return false;
}

static u64 ReferenceHash(const std::vector<u8>& vram, u32 start, u32 size)
{
    u64 hash = 0;
    while (size)
    {
        u32 count = std::min<u32>(size, vram.size() - start);
        hash = XXH64(vram.data() + start, count, hash);
        size -= count;
        start = 0;
    }
    return hash;
}

static bool PreviousCheck(u32 start, u32 size, u64 hash, u64* dirty, u8* vram, u32 vramSize)
{
    // Previous repeated-hash behavior, used only on non-wrapping ranges.
    u32 first = start / VRAMDirtyGranularity;
    u32 count = (start + size + VRAMDirtyGranularity - 1) / VRAMDirtyGranularity - first;
    for (u32 word = first / 64; word < (first + count + 63) / 64; word++)
        if ((GetRangedBitMask(word, first, count) & dirty[word]) &&
            Cache::MaskedHash(vram, vramSize, start, size) != hash)
            return true;
    return false;
}

static void CheckTextures()
{
    std::mt19937 random(0x5842525A);
    for (u32 vramSize : {128u * 1024, 512u * 1024})
    {
        std::vector<u8> vram(vramSize);
        for (auto& byte : vram) byte = random();
        std::vector<u64> dirty(vramSize / (VRAMDirtyGranularity * 64));
        for (int iteration = 0; iteration < 2000; iteration++)
        {
            // Exercise exact bit/word boundaries, empty ranges, partial words,
            // wraparound, and textures larger than the physical VRAM region.
            u32 start = random() % vramSize;
            u32 size = random() % (vramSize * 4 + 1);
            if (iteration < 128)
            {
                start = (iteration * VRAMDirtyGranularity) % vramSize;
                size = iteration % 3 == 0 ? 0 : iteration % 3 == 1 ? 1 : 32768;
            }
            for (auto& word : dirty) word = 0;
            dirty[random() % dirty.size()] = 1ULL << (random() % 64);
            if (iteration % 5 == 0)
                for (auto& word : dirty) word = ~0ULL;
            u64 oldHash = ReferenceHash(vram, start, size);
            Require(!Cache::CheckInvalid(start, size, oldHash, dirty.data(), vram.data(), vramSize),
                    "unchanged data must stay cached");
            u32 changed = random() % vramSize;
            vram[changed] ^= 0x80;
            bool expected = OverlapsDirty(start, size, dirty, vramSize) &&
                            ReferenceHash(vram, start, size) != oldHash;
            Require(Cache::CheckInvalid(start, size, oldHash, dirty.data(), vram.data(), vramSize) == expected,
                    "invalidation differs from reference");
            vram[changed] ^= 0x80;
        }

        // Explicitly put the only dirty bit AFTER the end of VRAM. The old
        // word mask indexed past dirty[] instead of wrapping to word zero.
        std::fill(dirty.begin(), dirty.end(), 0);
        dirty[0] = 1;
        u64 hash = ReferenceHash(vram, vramSize - 512, 1024);
        vram[0] ^= 1;
        Require(Cache::CheckInvalid(vramSize - 512, 1024, hash, dirty.data(), vram.data(), vramSize),
                "wrapped dirty data must invalidate");
    }
    std::puts("PASS: texture/palette invalidation, boundaries, empty and wrapped ranges (4,000 randomized cases)");
}

static u32 ReferenceCRC(const u8* data, int size, u32 seed)
{
    u32 crc = ~seed;
    for (int i = 0; i < size; i++)
    {
        crc ^= data ? data[i] : 0;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return ~crc;
}

static void CheckCRC()
{
    Require(CRC32(reinterpret_cast<const u8*>("123456789"), 9) == 0xCBF43926, "CRC known vector");
    std::mt19937 random(1234);
    std::vector<u8> bytes(4096);
    for (auto& byte : bytes) byte = random();
    for (int size = 0; size <= 4096; size++)
    {
        u32 seed = random();
        Require(CRC32(bytes.data(), size, seed) == ReferenceCRC(bytes.data(), size, seed), "CRC seeded input");
        Require(CRC32(nullptr, size, seed) == ReferenceCRC(nullptr, size, seed), "CRC implicit zeros");
        int split = size / 2;
        Require(CRC32(bytes.data() + split, size - split, CRC32(bytes.data(), split, seed)) ==
                CRC32(bytes.data(), size, seed), "CRC streaming");
    }
    std::puts("PASS: CRC known vector, seeded inputs, streaming and zero-filled blocks");
}

int main()
{
    CheckTextures();
    CheckCRC();

    std::vector<u8> vram(512 * 1024, 42);
    std::vector<u64> dirty(vram.size() / (VRAMDirtyGranularity * 64), ~0ULL);
    u64 hash = Cache::MaskedHash(vram.data(), vram.size(), 0, vram.size());
    using Check = bool (*)(u32, u32, u64, u64*, u8*, u32);
    volatile unsigned int result = 0;
    for (Check check : {PreviousCheck, Cache::CheckInvalid})
    {
        auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < 1000; iteration++)
            result += check(0, vram.size(), hash, dirty.data(), vram.data(), vram.size());
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 1000;
        std::printf("512 KiB unchanged texture, all words dirty, %s: %.2f us/check\n",
                    check == PreviousCheck ? "previous" : "optimized", us);
    }
    return result != 0;
}
