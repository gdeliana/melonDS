// Standalone decoder differential tests. The fixture models GPU's masked flat
// VRAM reads; include the production decoder without linking the emulator.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "types.h"

#define GPU3D_TEXCACHE
namespace melonDS
{
enum { outputFmt_RGB6A5, outputFmt_RGBA8, outputFmt_BGRA8 };
struct GPU
{
    std::array<u8, 512*1024> texture;
    std::array<u8, 128*1024> palette;
    template<typename T> T ReadVRAMFlat_Texture(u32 addr) const
    {
        T value;
        memcpy(&value, texture.data() + (addr & 0x7FFFF), sizeof(value));
        return value;
    }
    template<typename T> T ReadVRAMFlat_TexPal(u32 addr) const
    {
        T value;
        memcpy(&value, palette.data() + (addr & 0x1FFFF), sizeof(value));
        return value;
    }
};
}
#ifndef TEXTURE_DECODER_SOURCE
#define TEXTURE_DECODER_SOURCE "../src/GPU3D_Texcache.cpp"
#endif
#include TEXTURE_DECODER_SOURCE
using namespace melonDS;

// Independent channel-based color expansion, including zero-channel handling.
static u32 Color(u16 color, int format, u32 alpha)
{
    u32 r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    if (format == outputFmt_RGB6A5)
    {
        r = r ? r*2+1 : 0; g = g ? g*2+1 : 0; b = b ? b*2+1 : 0;
    }
    else
    {
        // Preserve the decoder's existing packed BGRA8 layout exactly.
        if (format == outputFmt_BGRA8)
            return (r << 9) | (g << 11) | (b << 13) |
                   (((alpha << 3) | (alpha >> 2)) << 24);
        r *= 8; g *= 8; b *= 8;
        alpha = (alpha << 3) | (alpha >> 2);
    }
    return r | (g << 8) | (b << 16) | (alpha << 24);
}

static void ReferenceCompressed(u32 w, u32 h, u32* out, u32 addr, u32 aux,
                                u32 pal, GPU& gpu, int format)
{
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
        {
            u32 block = x/4 + (y/4)*(w/4);
            u32 bits = gpu.ReadVRAMFlat_Texture<u32>(addr + block*4);
            u16 info = gpu.ReadVRAMFlat_Texture<u16>(aux + block*2);
            u32 index = (bits >> (2*((x%4) + (y%4)*4))) & 3;
            u32 mode = info >> 14;
            u32 base = pal + (info & 0x3FFF)*4;
            u16 color = gpu.ReadVRAMFlat_TexPal<u16>(base + index*2);
            u32 alpha = 31;
            if (index == 3 && mode < 2) { color = 0; alpha = 0; }
            else if ((mode == 1 && index == 2) || (mode == 3 && index >= 2))
            {
                u16 c0 = gpu.ReadVRAMFlat_TexPal<u16>(base);
                u16 c1 = gpu.ReadVRAMFlat_TexPal<u16>(base+2);
                u32 a = mode == 1 ? 1 : index == 2 ? 5 : 3;
                u32 b = mode == 1 ? 1 : 8-a;
                color = 0;
                for (u32 shift : {0u, 5u, 10u})
                    color |= (((((c0 >> shift)&31)*a + ((c1 >> shift)&31)*b)/(a+b)) << shift);
            }
            out[x+y*w] = Color(color, format, alpha);
        }
}

template<int Format, int X, int Y>
static void CheckAXIY(GPU& gpu, u32 w, u32 h, u32 addr, u32 pal)
{
    std::vector<u32> out(w*h+2, 0xDEADBEEF);
    ConvertAXIYTexture<Format, X, Y>(w, h, out.data()+1, addr, pal, gpu);
    for (u32 i = 0; i < w*h; i++)
    {
        u8 val = gpu.ReadVRAMFlat_Texture<u8>(addr+i);
        u32 alpha = val >> Y;
        if (X == 3) alpha = alpha*4 + alpha/2;
        u16 color = gpu.ReadVRAMFlat_TexPal<u16>(pal + (val & ((1<<Y)-1))*2);
        if (out[i+1] != Color(color, Format, alpha)) std::abort();
    }
    if (out.front() != 0xDEADBEEF || out.back() != 0xDEADBEEF) std::abort();
}

template<int Format>
static void Check(GPU& gpu, u32 w, u32 h, u32 addr, u32 aux, u32 pal)
{
    std::vector<u32> expected(w*h), actual(w*h+2, 0xDEADBEEF);
    ReferenceCompressed(w, h, expected.data(), addr, aux, pal, gpu, Format);
    ConvertCompressedTexture<Format>(w, h, actual.data()+1, addr, aux, pal, gpu);
    if (!std::equal(expected.begin(), expected.end(), actual.begin()+1) ||
        actual.front() != 0xDEADBEEF || actual.back() != 0xDEADBEEF) std::abort();
    CheckAXIY<Format, 3, 5>(gpu, w, h, addr, pal);
    CheckAXIY<Format, 5, 3>(gpu, w, h, addr, pal);
}

template<int Format, int Bits>
static void CheckIndexed(GPU& gpu, u32 w, u32 h, u32 addr, u32 pal)
{
    for (bool transparent : {false, true})
    {
        std::vector<u32> out(w*h+2, 0xDEADBEEF);
        ConvertNColorsTexture<Format, Bits>(w, h, out.data()+1, addr, pal, transparent, gpu);
        for (u32 i = 0; i < w*h; i++)
        {
            const u8 packed = gpu.ReadVRAMFlat_Texture<u8>(addr+(i*Bits)/8);
            const u32 index = (packed >> ((i*Bits)%8)) & ((1<<Bits)-1);
            const u16 color = gpu.ReadVRAMFlat_TexPal<u16>(pal+index*2);
            if (out[i+1] != Color(color, Format, transparent && index == 0 ? 0 : 31)) std::abort();
        }
        if (out.front() != 0xDEADBEEF || out.back() != 0xDEADBEEF) std::abort();
    }
}

template<int Format> static void CheckAllIndexed(GPU& gpu, u32 w, u32 h, u32 addr, u32 pal)
{
    CheckIndexed<Format, 2>(gpu, w, h, addr, pal);
    CheckIndexed<Format, 4>(gpu, w, h, addr, pal);
    CheckIndexed<Format, 8>(gpu, w, h, addr, pal);
}

static volatile u32 checksum;

template<typename Decode>
static void Benchmark(const char* name, GPU& gpu, u32 size, Decode decode)
{
    auto function = +decode;
    decltype(function) volatile call = function;
    std::vector<u32> out(size*size);
    const int count = std::max(100, int(16000000/(size*size)));
    std::vector<double> batches;
    for (int batch = 0; batch < 5; batch++)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; i++)
        {
            call(size, out.data(), gpu);
            checksum = out[i % out.size()];
        }
        auto end = std::chrono::steady_clock::now();
        batches.push_back(std::chrono::duration<double, std::micro>(end-start).count()/count);
    }
    std::sort(batches.begin(), batches.end());
    std::printf("%s %ux%u: %.3f us/decode (median of five batches)\n", name, size, size, batches[2]);
}

int main(int argc, char** argv)
{
    auto gpu = std::make_unique<GPU>();
    std::mt19937 random(0xDEC0DE);
    for (int iteration = 0; iteration < 120; iteration++)
    {
        for (auto& byte : gpu->texture) byte = random();
        for (auto& byte : gpu->palette) byte = random();
        // Include black channels, high palette bits and all alpha/index values.
        if (iteration == 0) gpu->palette.fill(0);
        if (iteration == 1) gpu->palette.fill(255);
        u32 w = 8u << (iteration%8), h = 8u << ((iteration/8)%5);
        u32 addr = iteration%2 ? 0x7FFF8 : 0;
        u32 aux = iteration%3 ? 0x3FFF8 : 0x20000;
        u32 pal = iteration%2 ? 0x1FFFE : 0;
        Check<outputFmt_RGB6A5>(*gpu, w, h, addr, aux, pal);
        Check<outputFmt_RGBA8>(*gpu, w, h, addr, aux, pal);
        Check<outputFmt_BGRA8>(*gpu, w, h, addr, aux, pal);
        CheckAllIndexed<outputFmt_RGB6A5>(*gpu, w, h, addr, pal);
        CheckAllIndexed<outputFmt_RGBA8>(*gpu, w, h, addr, pal);
        CheckAllIndexed<outputFmt_BGRA8>(*gpu, w, h, addr, pal);
    }
    std::puts("PASS: 3,240 decoder cases, all formats, indexed palettes, transparency, interpolation, alpha, VRAM wrapping and bounds");
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0)
        for (u32 size : {8u, 64u, 256u})
        {
            Benchmark("2-bit indexed", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertNColorsTexture<outputFmt_RGB6A5, 2>(s, s, out, 0, 0, true, g);
            });
            Benchmark("4-bit indexed", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertNColorsTexture<outputFmt_RGB6A5, 4>(s, s, out, 0, 0, true, g);
            });
            Benchmark("8-bit indexed", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertNColorsTexture<outputFmt_RGB6A5, 8>(s, s, out, 0, 0, true, g);
            });
            Benchmark("compressed RGB6A5", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertCompressedTexture<outputFmt_RGB6A5>(s, s, out, 0, 0x20000, 0, g);
            });
            Benchmark("A3I5 RGB6A5", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertAXIYTexture<outputFmt_RGB6A5, 3, 5>(s, s, out, 0, 0, g);
            });
            Benchmark("A5I3 RGB6A5", *gpu, size, [](u32 s, u32* out, GPU& g) {
                ConvertAXIYTexture<outputFmt_RGB6A5, 5, 3>(s, s, out, 0, 0, g);
            });
        }
}
