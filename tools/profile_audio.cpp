#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "types.h"
#include "blip-buf/blip_buf.h"
using namespace melonDS;
enum class AudioInterpolation { None, Linear, Cosine, Cubic, SNESGaussian };
struct SPUChannel
{
    u32 Num = 0, Cnt = 0, Length = 64, LoopPos = 0, Timer = 0, TimerReload = 0xF000;
    bool KeyOn = false;
    s16 CurSample = 0, PrevSample[3] = {};
    u8 VolumeShift = 0, Volume = 64, Pan = 64;
    AudioInterpolation InterpType = AudioInterpolation::None;
    u32 phase = 0;
    void Start() { Timer = TimerReload; }
    void NextSample_PCM8() { CurSample = (++phase*37) & 0x7FFF; }
    void NextSample_PCM16() { NextSample_PCM8(); }
    void NextSample_ADPCM() { NextSample_PCM8(); }
    void NextSample_PSG() { NextSample_PCM8(); }
    void NextSample_Noise() { NextSample_PCM8(); }
    void PanOutput(s32, s32&, s32&);
    template<u32 type> s32 Run(u32);
    s32 DoRunBaseline(u32 cycles)
    {
        const u32 format = (Cnt >> 29) & 3;
        if (format == 0) return Run<0>(cycles);
        if (format == 1) return Run<1>(cycles);
        if (format == 2) return Run<2>(cycles);
        if (Num >= 14) return Run<4>(cycles);
        if (Num >= 8) return Run<3>(cycles);
        return 0;
    }
#include "dispatch.inc"
};
#include "interpolation.inc"
static volatile s32 checksum;
template<class Function> static double Measure(Function f)
{
    std::vector<double> times;
    for (int batch = 0; batch < 5; batch++)
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100000; i++) f();
        auto end = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double,std::nano>(end-start).count()/100000);
    }
    std::sort(times.begin(),times.end()); return times[2];
}
static s32 MixReference(std::array<SPUChannel,16>& Channels)
{
    s32 left=0,right=0;
    for(int i=4;i<16;i++) { auto* c=&Channels[i];c->PanOutput(c->DoRun(512),left,right); }
    return left ^ right;
}
int main()
{
    std::mt19937 random(0xA0D10);
    for (int i = 0; i < 10000; i++)
    {
        SPUChannel a;
        a.Num = random()%16; a.Cnt = (random()%4)<<29;
        if (random()%2) a.Cnt |= 1u<<31;
        a.InterpType = static_cast<AudioInterpolation>(random()%5);
        a.KeyOn = random()%2; a.Timer = a.TimerReload+(random()%4096);
        SPUChannel b = a; u32 cycles = random()%1024;
        s32 x = a.DoRun(cycles), y = b.DoRunBaseline(cycles);
        if (x != y || a.Timer != b.Timer || a.phase != b.phase || a.KeyOn != b.KeyOn ||
            !std::equal(std::begin(a.PrevSample),std::end(a.PrevSample),std::begin(b.PrevSample))) std::abort();
    }
    for(int trial=0;trial<1000;trial++)
    {
        std::array<SPUChannel,16> a;
        for(int i=0;i<16;i++)
        {
            a[i].Num=i;a[i].Cnt=(random()%8)<<29;a[i].Timer=a[i].TimerReload+(random()%4096);
            a[i].Pan=random()%129;a[i].KeyOn=random()%2;a[i].InterpType=static_cast<AudioInterpolation>(random()%5);
        }
        auto b=a;
        if(MixReference(a)!=MixOptimized(b))std::abort();
        for(int i=0;i<16;i++)if(a[i].Timer!=b[i].Timer || a[i].phase!=b[i].phase ||
            a[i].KeyOn!=b[i].KeyOn || a[i].CurSample!=b[i].CurSample ||
            !std::equal(std::begin(a[i].PrevSample),std::end(a[i].PrevSample),std::begin(b[i].PrevSample)))std::abort();
    }
    std::puts("PASS: 10,000 dispatch/interpolation and 1,000 mixing cases match reference output and sample state");
    for (int active : {0,4,12})
    {
        std::array<SPUChannel,16> channels;
        for (int i=0;i<16;i++) { channels[i].Num=i;channels[i].Pan=i*8;channels[i].Cnt=i>=4 && i<4+active?1u<<31:0; channels[i].Timer=channels[i].TimerReload; }
        s32 (*volatile old)(std::array<SPUChannel,16>&)=MixReference;
        s32 (*volatile now)(std::array<SPUChannel,16>&)=MixOptimized;
        double before=Measure([&]{checksum=old(channels);});
        double after=Measure([&]{checksum=now(channels);});
        std::printf("Mix channels 4..15, %d active: previous %.2f ns, current %.2f ns/sample\n",active,before,after);
        auto copy=channels;
        s32 x=old(channels),y=now(copy);
        if(x!=y)std::abort();
        for(int i=0;i<16;i++) if(channels[i].Timer!=copy[i].Timer || channels[i].phase!=copy[i].phase)std::abort();
    }
    const char* names[] = {"none","linear","cosine","cubic","Gaussian"};
    for (int mode = 0; mode < 5; mode++)
    {
        SPUChannel a; a.Cnt = 1u<<31; a.Timer = a.TimerReload; a.InterpType = static_cast<AudioInterpolation>(mode);
        s32 (SPUChannel::* volatile current)(u32) = &SPUChannel::DoRun;
        std::printf("Interpolation %s: %.2f ns/channel/sample (synthetic fetch)\n", names[mode], Measure([&]{ checksum = (a.*current)(512); }));
    }
    blip_t* left = blip_new(4096); blip_t* right = blip_new(4096);
    blip_set_rates(left,16756991,48000); blip_set_rates(right,16756991,48000);
    std::array<short,4096> output;
    int sign = 1;
    double ns = Measure([&]{
        for (int i = 0; i < 128; i++)
        {
            sign = -sign;
            blip_add_delta(left,i*512,sign*4096); blip_add_delta(right,i*512,-sign*4096);
        }
        blip_end_frame(left,65536); blip_end_frame(right,65536);
        int n = blip_samples_avail(left);
        blip_read_samples(left,output.data(),n,1); blip_read_samples(right,output.data()+1,n,1);
        checksum = output[0];
    });
    std::printf("Real blip stereo resampling: %.2f us / 128 input samples (dense transitions)\n",ns/1000);
    blip_delete(left); blip_delete(right);
}
