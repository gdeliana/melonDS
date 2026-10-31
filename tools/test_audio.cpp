// Exercises verbatim SPU output methods, with deterministic blip sample input.
// tools/test_audio.py extracts the methods and compiles this standalone fixture.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;

struct blip_t
{
    std::vector<s16> input;
    int updates = 0;
    double clock = 0, rate = 0;
};
static void blip_end_frame(blip_t*, int) {}
static int blip_samples_avail(blip_t* b) { return b->input.size(); }
static void blip_read_samples(blip_t* b, s16* out, int count, bool stereo)
{
    for (int i = 0; i < count; i++) out[i * (stereo ? 2 : 1)] = b->input[i];
}
static void blip_set_rates(blip_t* b, double clock, double rate)
{
    b->updates++; b->clock = clock; b->rate = rate;
}
namespace Platform
{
static void Mutex_Lock(std::mutex* m) { m->lock(); }
static void Mutex_Unlock(std::mutex* m) { m->unlock(); }
}
struct SPU
{
    u32 OutputBufferSize = 0;
    double OutputSampleRate = 48000, OutputSkew = 1;
    blip_t left, right;
    blip_t* BlipLeft = &left;
    blip_t* BlipRight = &right;
    int BlipTimer = 0;
    s16* OutputBuffer = nullptr;
    u32 OutputBufferWritePos = 0, OutputBufferReadPos = 0;
    std::mutex lock;
    std::mutex* AudioLock = &lock;
    ~SPU() { free(OutputBuffer); }
    void BufferAudio();
    void TrimOutput();
    void DrainOutput();
    void InitOutput();
    int GetOutputSize() const;
    int ReadOutput(s16*, int);
    void SetOutputSampleRate(double);
    void SetOutputSkew(double);
};
#include "audio_output_under_test.inc"

static void Require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using Frame = std::array<s16, 2>;
static void Fill(SPU& spu, int count, std::mt19937& random)
{
    spu.left.input.resize(count); spu.right.input.resize(count);
    for (int i = 0; i < count; i++)
    {
        spu.left.input[i] = static_cast<s16>(random());
        spu.right.input[i] = static_cast<s16>(random());
    }
}

static void CheckFIFO()
{
    std::mt19937 random(0xA0D10);
    size_t cases = 0;
    for (u32 size : {2u, 4u, 8u, 16u, 1024u, 2048u})
    {
        SPU spu;
        spu.OutputBufferSize = size;
        spu.OutputBuffer = static_cast<s16*>(calloc(size * 2, sizeof(s16)));
        std::deque<Frame> expected;
        u32 read = 0, write = 0;
        for (int step = 0; step < 12000; step++)
        {
            if (step % 101 == 0)
            {
                spu.DrainOutput(); expected.clear(); read = write = 0;
            }
            const bool writing = random() % 2;
            // Exercise zero-length, capacity-sized and multiple-wrap batches.
            const int count = random() % (size * 4 + 1);
            if (writing)
            {
                Fill(spu, count, random);
                for (int i = 0; i < count; i++)
                {
                    expected.push_back({spu.left.input[i], spu.right.input[i]});
                    write = (write + 2) % (size * 2);
                    if (expected.size() == size)
                    {
                        expected.pop_front(); read = (read + 2) % (size * 2);
                    }
                }
                spu.BufferAudio();
            }
            else
            {
                std::vector<s16> out(count * 2 + 2, 12345);
                int wanted = std::min<size_t>(count, expected.size());
                Require(spu.ReadOutput(out.data() + 1, count) == wanted, "read frame count");
                for (int i = 0; i < wanted; i++)
                {
                    Require(out[i*2+1] == expected.front()[0] && out[i*2+2] == expected.front()[1], "stereo output");
                    expected.pop_front(); read = (read + 2) % (size * 2);
                }
                Require(out.front() == 12345, "read prefix guard");
                for (size_t i = wanted*2+1; i < out.size(); i++)
                    Require(out[i] == 12345, "read suffix / underrun guard");
            }
            Require(spu.OutputBufferReadPos == read && spu.OutputBufferWritePos == write, "FIFO positions");
            Require(spu.GetOutputSize() == static_cast<int>(expected.size()), "FIFO available frames");
            for (size_t i = 0; i < expected.size(); i++)
            {
                u32 pos = (read + i*2) % (size*2);
                Require(spu.OutputBuffer[pos] == expected[i][0] && spu.OutputBuffer[pos+1] == expected[i][1], "queued frames");
            }
            cases++;
        }
    }
    std::printf("PASS: %zu FIFO operations against deque reference, stereo order, wrapping, oversized writes, overflow, underrun, zero lengths and drain\n", cases);
}

static void CheckRates()
{
    SPU spu;
    spu.InitOutput();
    Require(spu.GetOutputSize() == 0 && spu.left.rate == 48000 && spu.right.rate == 48000, "initial output rate");
    const int before = spu.left.updates;
    for (int i = 0; i < 1000; i++) spu.SetOutputSkew(1.0);
    Require(spu.left.updates == before && spu.right.updates == before, "unchanged skew must skip rate updates");
    spu.SetOutputSkew(1.25);
    Require(spu.left.updates == before+1 && spu.right.updates == before+1, "changed skew updates both channels");
    Require(spu.left.clock == INTERNAL_SAMPLE_RATE*1.25 && spu.right.clock == spu.left.clock, "skew clock");
    for (double rate : {44100.0, 96000.0, 22050.0, 48000.0})
    {
        spu.SetOutputSampleRate(rate);
        Require(spu.left.rate == rate && spu.right.rate == rate && spu.OutputSkew == 1.25, "rate change preserves skew");
        Require(spu.GetOutputSize() == 0, "rate change drains output");
        const int updates = spu.left.updates;
        spu.SetOutputSkew(1.25);
        Require(spu.left.updates == updates && spu.right.updates == updates, "same skew after rate change");
    }
    spu.InitOutput();
    Require(spu.left.clock == INTERNAL_SAMPLE_RATE*1.25 && spu.left.rate == 48000, "reset reapplies rates");
    std::puts("PASS: unchanged/changed skew, both channels, output-rate changes, reset");
}

static volatile int checksum;
template<class Function> static void Benchmark(const char* name, Function function)
{
    std::vector<double> times;
    for (int batch = 0; batch < 5; batch++)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100000; i++) function();
        auto end = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::nano>(end-start).count()/100000);
    }
    std::sort(times.begin(), times.end());
    std::printf("%s: %.1f ns/call (median of five batches)\n", name, times[2]);
}
int main(int argc, char** argv)
{
    CheckFIFO();
    if (argc < 2 || strcmp(argv[1], "--baseline") != 0) CheckRates();
    if (argc > 1 && (strcmp(argv[1], "--benchmark") == 0 || strcmp(argv[1], "--baseline") == 0))
    {
        SPU spu; spu.InitOutput();
        std::mt19937 random(123); Fill(spu, 512, random);
        std::array<s16, 1024> out;
        // Keep calls opaque to the optimizer, as they are across translation
        // units in the normal build; do not time an eliminated memcpy.
        int (SPU::* volatile read)(s16*, int) = &SPU::ReadOutput;
        void (SPU::* volatile write)() = &SPU::BufferAudio;
        Benchmark("Read 512 stereo frames", [&] {
            spu.OutputBufferReadPos = 0; spu.OutputBufferWritePos = 1024;
            checksum = (spu.*read)(out.data(), 512);
        });
        Benchmark("Write 512 stereo frames (including mock blip interleave)", [&] { (spu.*write)(); });
    }
}
