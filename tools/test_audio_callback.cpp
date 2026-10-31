#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using s16 = int16_t;
using s32 = int32_t;
using Uint8 = uint8_t;
static void SDL_LockMutex(void*) {}
static void SDL_UnlockMutex(void*) {}
static void SDL_CondSignal(void*) {}
struct Output
{
    int available = 0, requested = 0;
    void SetOutputSkew(double) {}
    int ReadOutput(s16* out, int count)
    {
        requested = count;
        int n = std::min(count, available);
        for (int i = 0; i < n; i++) { out[2*i] = i+1; out[2*i+1] = -i-1; }
        return n;
    }
};
struct Console { Output SPU; };
struct EmuInstance
{
    double targetFPS = 60;
    float curFPS = 60, audioSampleFrac = 0;
    int audioBufSize = 512, audioVolume = 256;
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    void* audioSyncLock = nullptr;
    void* audioSyncCond = nullptr;
    Console console;
    Console* nds = &console;
    int audioGetNumSamplesOut(int);
    static void audioCallback(void*, Uint8*, int);
};
#include "callback.inc"
static void Check(bool ok) { if (!ok) { std::puts("FAIL: callback output / bounds"); std::abort(); } }
int main()
{
    int cases = 0;
    for (int frames : {0,1,127,512,1024})
    for (int preferred : {64,512,2048})
    for (int available : {0,1,100,512,4096})
    for (float ratio : {0.f,.25f,.5f,1.f,1.5f,2.f,4.f})
    for (int volume : {0,128,256})
    for (int mute = 0; mute < 4; mute++)
    {
        EmuInstance inst;
        inst.audioBufSize = preferred; inst.curFPS = ratio*60;
        inst.audioVolume = volume; inst.console.SPU.available = available;
        inst.audioMutedToggle = mute == 1; inst.audioMutedByWindowFocus = mute == 2; inst.audioMutedByFastForward = mute == 3;
        std::vector<s16> out(frames*2+16, 30000);
        EmuInstance::audioCallback(&inst, reinterpret_cast<Uint8*>(out.data()+8), frames*4);
        const int request = std::min({frames, preferred, int(frames*ratio)});
        const int consumed = std::min(request, available);
        Check(inst.console.SPU.requested == request);
        for (int i = 0; i < frames; i++)
        {
            int sample = std::min(i, consumed-1)+1;
            s16 left = !consumed || mute ? 0 : (sample*volume)>>8;
            s16 right = !consumed || mute ? 0 : (-sample*volume)>>8;
            Check(out[8+i*2] == left && out[8+i*2+1] == right);
        }
        for (int i = 0; i < 8; i++) Check(out[i] == 30000 && out[out.size()-1-i] == 30000);
        cases++;
    }
    std::printf("PASS: %d callback cases, full output fill, volume, mute, underruns, unequal device buffers and guards\n", cases);
}
