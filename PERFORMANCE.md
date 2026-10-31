# Performance review and checks

This review covered first-party CPU/JIT dispatch and scheduling, DMA/memory
timings, software and OpenGL rendering, texture decoding/invalidation, audio,
frontend configuration/frame pacing, save/storage paths, networking, and build
settings. The measurements below are isolated benchmarks, not game FPS results.
No game/ROM workload was used, so the remaining candidates are source-review
findings rather than a ranking from whole-emulator profiling.

## Changes implemented

### xBRZ

`src/frontend/qt_sdl/xbrz_shader.h`, `Screen.cpp`, `Screen.h`:

- Retain the two-pass algorithm: classify source pixels once, then blend at
  display resolution. Both software and OpenGL renderer output textures work.
- Precompute edge-normal scale factors once per screen draw. The display pass
  now uses signed dot products instead of per-pixel derivatives, projection,
  division, and square roots. Rotation, HiDPI and nonuniform scaling are handled
  by the layout-derived factors.
- Pack corner decisions into a single `R32UI` texel. This replaces RGBA8
  normalization/rounding and removes the need to query/modify dithering state.
  Metadata remains four bytes per source pixel.
- Skip detailed analysis in flat regions, and skip neighbor sampling/blending
  when there are no active corners.
- Use squared color distances for equality and nearest-neighbor comparisons.
  The distance sums used to classify corners still use the original distances.
- Use the ordinary display shader for each screen that is not being enlarged,
  including small secondary screens in hybrid layouts.
- Allocate/attach/check the analysis framebuffers only when the source size
  changes; reuse one framebuffer for each screen layer during drawing.

Example GPU draw timings on an NVIDIA RTX 3060 Laptop GPU, driver 615.78.08,
OpenGL 3.2. Two 256x192 screens; median of five 40-frame batches. Baseline is
the preceding two-pass implementation, **not** the original single-pass shader.
Setup, readback and CPU emulation are outside the timed draw calls.

| Synthetic input | Scale | Previous | Optimized | Ratio |
| --- | ---: | ---: | ---: | ---: |
| Flat color | 4x | 0.089 ms | 0.056 ms | 1.59x |
| Flat color | 6x | 0.147 ms | 0.090 ms | 1.64x |
| Colored 8x8 tiles | 4x | 0.093 ms | 0.073 ms | 1.28x |
| Colored 8x8 tiles | 6x | 0.160 ms | 0.098 ms | 1.63x |
| Random palette pixels | 4x | 0.100 ms | 0.100 ms | ~1x |
| Random palette pixels | 6x | 0.197 ms | 0.176 ms | 1.12x |

These small timings depend on GPU clocks, drivers and workload. They do not
imply the same improvement to emulator FPS.

### Texture-cache invalidation

`src/GPU3D_Texcache.h`:

- Hash the complete texture at most once when any overlapping dirty bit is set.
  Previously, an unchanged texture could be rehashed for every overlapping
  dirty-map word. This affects the shared texture cache used by the OpenGL
  renderers.
- Correct the dirty-map wrap mask to count 64-bit words, rather than bits.
  Wrapped textures could previously index beyond the dirty array.
- Skip empty ranges immediately.

A 512 KiB unchanged texture with all 16 dirty words set measured approximately
909 us/check before and 56 us/check after (~16x). This is a deliberately
worst-case validation workload, not a 16x improvement to rendering. Clean
textures already avoided hashing, and changed textures already returned early.

### Small allocation/copy reductions

- `src/frontend/qt_sdl/Config.{h,cpp}`: pass default maps by const reference
  instead of copying the entire map on a missing/invalid setting lookup.
- `src/CRC32.cpp`: keep the compile-time CRC table in static read-only storage
  instead of constructing/copying a local table on every checksum call.

### Further texture-decoding reductions

`src/GPU3D_Texcache.cpp`:

- Convert the four interpolated colors of a compressed 4x4 block once, then
  index those converted colors for its sixteen pixels.
- Preconvert the 8-entry A5I3 or 32-entry A3I5 palette once per decode. Alpha
  expansion remains per pixel and the palette is rebuilt for each decode, so
  palette changes are immediately visible.
- Preserve existing output bits, including transparent colors and all three
  output formats. These changes reduce CPU work in texture-cache misses for
  the GPU renderers; they do not reduce GPU draw complexity.

Isolated GCC `-O3` benchmarks in `melonds-gpu`, random texture/palette data,
RGB6A5 output, median of five batches (microseconds per decode):

| Texture | Size | Previous | Optimized |
| --- | --- | ---: | ---: |
| Compressed | 8x8 | 0.209 | 0.080 |
| A3I5 | 8x8 | 0.193 | 0.084 |
| A5I3 | 8x8 | 0.180 | 0.073 |
| Compressed | 256x256 | 215.449 | 82.490 |
| A3I5 | 256x256 | 226.734 | 63.421 |
| A5I3 | 256x256 | 191.123 | 62.412 |

These measure decoding only, using a fixture with the same masked flat-VRAM
reads as `GPU`. They do not establish whole-game CPU usage or power savings.
No ROM workload was run. A further xBRZ experiment fetching only the selected
neighbor for each active corner passed pixel comparisons, but repeated GPU
timings showed mixed results, including regressions. That experiment was not
retained.

### Audio output

`src/SPU.cpp`:

- Read and write the stereo FIFO using at most two bulk copies per call.
  Preserve stereo order and the existing overflow policy: drop oldest frames,
  retain newest frames and leave one stereo frame unused. Oversized batches
  copy only the samples that survive in the ring.
- Skip rate recalculation when `SetOutputSkew()` receives the current skew.
  `InitOutput()` still reapplies rates on reset and output-rate changes.
- Keep the existing mutex protection. No changes to mixing, interpolation,
  sample quality, callback buffer size or audio synchronization.

Isolated GCC `-O3` benchmarks in `melonds-gpu`, 512 stereo frames per call,
median of five 100,000-call batches:

| Operation | Previous | Optimized |
| --- | ---: | ---: |
| FIFO read | 344.9 ns | 28.4 ns |
| FIFO write, including fixture sample interleaving | 798.4 ns | 292.6 ns |

The fixture uses real mutexes and deterministic mock blip input, and calls the
production method bodies through volatile member-function pointers to prevent
elimination of the copies. These are FIFO microbenchmarks, not whole-audio or
gameplay measurements. No actual audio device or game playback was tested.

### OpenGL transfers and display state

- `src/GPU3D_OpenGL.cpp`: replace the regular OpenGL 3D renderer's per-frame
  mapped uniform upload with a new buffer store, and orphan the vertex/index
  stores before filling them. Buffer object names, VAO bindings, capacities and
  separate main/edge index ranges remain the same. Earlier draws can retain
  their old storage. This uses existing OpenGL 3.2 APIs.
- `src/GPU_OpenGL.cpp`: scissor non-wrapped capture downscaling to the rows
  actually being read. Wrapped ranges still use one full-surface downscale.
  Fix wrapped readback lengths to use the remaining block count; the previous
  loop could copy/dirty extra VRAM blocks after wrapping.
- `src/GPU.h`, `src/GPU_OpenGL.h`, `Screen.cpp`: obtain output dimensions from
  renderer-maintained CPU metadata instead of two per-draw OpenGL queries.
  Native software output remains 256x192; OpenGL reports its current scaled
  dimensions, including after resolution changes or renderer recreation.
- `Screen.cpp`: create nearest/bilinear sampler objects once on OpenGL 3.3+,
  bind the selected sampler for presentation and unbind it afterward. This
  avoids changing shared output texture filter parameters every frame. The
  OpenGL 3.2 texture-parameter path remains available.

Synthetic NVIDIA RTX 3060 Laptop GPU / driver 615.78.08 results in `melonds-gpu`,
OpenGL 3.2. Upload measurements include draws and GPU completion, using production
buffer capacities, a 28-byte vertex stride, split index ranges, a simple shader
and a 1-pixel viewport. They measure submission/transfer throughput, not the
actual DS shader workload. Median of five alternating 100-frame batches:

| Upload + draw workload | Previous mapped/reused stores | Orphaned stores |
| --- | ---: | ---: |
| 12 vertices | 48.16 us | 16.16 us |
| 1,023 vertices | 44.90 us | 22.51 us |
| 9,999 vertices | 105.31 us | 71.39 us |

Capture downscale GPU draw timings from a 4x source, median of five 40-draw
batches, excluding CPU setup and readback:

| Rows needed | Full conversion | Partial conversion |
| --- | ---: | ---: |
| 64 | 1.54 us | 0.77 us |
| 128 | 1.48 us | 0.82 us |
| 192 | 1.54 us | 0.97 us |

End-to-end synchronous capture timings were mixed: in the same run, 64 rows
took 77.23 us before and 79.79 us after; 128 rows took 110.38/102.34 us; 192 rows
took 151.67/145.65 us. Thus reduced GPU conversion work is measurable, while
readback still dominates and total latency is not uniformly improved. No PBO or
deferred readback was introduced: the emulated CPU still sees captured data
immediately. The core already skips synchronized captures using its synced flag.
These synthetic results do not establish gameplay FPS, CPU use or power savings.

### Indexed textures, upload reuse, presentation and audio mixing

The next pass covered all five follow-up targets and the callback fill fix:

- Ordinary 2/4/8-bit indexed textures now convert their palette once per decode.
  Tiny 8-bit textures with fewer than 256 pixels retain per-pixel conversion to
  avoid converting unused palette entries. All three output formats and the
  original color-zero transparency behavior are preserved.
- `OpenGL_Upload.h` holds CPU snapshots for exclusively CPU-written buffer
  prefixes. The 2D renderer skips identical layer/sprite/compositor configuration
  and sprite-geometry uploads, including safe shrinking-prefix reuse. Scanline
  buffers still upload their changed ranges normally. Caches clear on GL buffer
  creation and never bypass draw calls or VRAM-dependent sprite rendering.
- The compute renderer reuses unchanged metadata uploads, with zero-initialized
  metadata padding. An experiment replacing every populated SSBO/image-buffer
  store was slower and was removed. Existing span/polygon buffer capacities and
  upload behavior remain intact, including padded-workgroup storage.
- The GPU assigns process-unique presentation stamps for finished frames,
  resets, state loads, renderer replacement and output-resolution changes.
  These stamps are not serialized. Repeated presentations of the same frame
  skip software-output texture uploads and reuse per-layer xBRZ analysis.
  Newly visible layers still analyze on demand; resize/context recreation
  invalidates analysis. Reused analysis restores its texture binding after
  presentation cleanup. Final drawing and OSD updates still run.
- The mixer skips format dispatch and panning for disabled channels 4..15.
  Their existing runners returned zero without advancing any channel state.
  Channels 0..3, sound capture, active-channel processing, interpolation and
  blip resampling arithmetic are unchanged.
- The SDL callback caps input reads to the actual output capacity and fills
  the entire output buffer. A short nonempty read repeats the final scaled
  stereo frame; empty reads and muted output produce silence. Previously the
  fill stopped at the requested input length, leaving an unwritten tail when
  that length was smaller than the output length.

Further isolated results in `melonds-gpu`, GCC `-O3`, RGB6A5 indexed output,
median of five batches, using opaque decoder calls to prevent copy elimination:

| Indexed texture | Previous | Optimized |
| --- | ---: | ---: |
| 2-bit, 8x8 | 0.209 us | 0.042 us |
| 4-bit, 8x8 | 0.208 us | 0.077 us |
| 8-bit, 8x8 (small-texture fallback) | 0.235 us | 0.226 us |
| 2-bit, 256x256 | 190.073 us | 28.308 us |
| 4-bit, 256x256 | 204.228 us | 28.697 us |
| 8-bit, 256x256 | 224.896 us | 28.360 us |

Repeated identical GL 3.2 uploads measured 0.147/0.033 us (48 bytes),
0.196/0.038 us (144 bytes) and 1.469/0.115 us (8192 bytes), always-upload/cache
respectively. In the GL 4.3 synthetic 64-dispatch workload, unchanged metadata
caching measured 489.23/407.35 us; the rejected full replacement experiment
measured 490.91/626.61 us for reused/replaced stores. These are driver/fixture
costs, not an actual 2D or compute renderer game profile.

The extracted production mixing loop for channels 4..15 measured 34.51/5.94 ns
with none active, 38.21/22.60 ns with four active and 53.10/52.56 ns with all
twelve active, previous/current. Synthetic channel interpolation costs ranged
from 4.78 ns without interpolation through 10.51 ns for Gaussian interpolation;
sample fetching was mocked. Real stereo blip resampling with dense transitions
measured 3.74 us per 128 input samples. No resampling quality changes or SIMD
rewrite were introduced based on this isolated profile. Whole-game CPU/GPU use
and power savings remain unmeasured.

## Remaining profiling targets

| Area and source | Observation and next measurement |
| --- | --- |
| GPU capture, `src/GPU_OpenGL.cpp`, `GLRenderer::SyncVRAMCapture` | Partial conversion and wrapped read sizes are optimized. Synchronous `glReadPixels` still dominates the synthetic test; profile capture-heavy games before attempting earlier/asynchronous transfers. |
| GPU uploads, `src/GPU3D_OpenGL.cpp`, `src/GPU2D_OpenGL.cpp`, `src/GPU3D_Compute.cpp` | Regular OpenGL 3D stores orphan; 2D and compute metadata skip unchanged uploads. Full compute store replacement regressed in the fixture. Profile actual scanline/compute stalls before introducing fenced rings or further caching. |
| Texture decoding, `src/GPU3D_Texcache.cpp` | Compressed, alpha-indexed and ordinary indexed palette reuse is implemented. Profile cache-miss frequency and bitmap decoding in real games before adding larger lookup tables. |
| Software rasterization, `src/GPU3D_Soft.cpp`, `src/GPU2D_Soft.cpp` | Per-pixel texture access, blending and scanline work are candidates for CPU profiles. Changes need image comparisons covering depth, shadows, blending and scanline-dependent VRAM. |
| CPU/JIT/scheduler, `src/ARM.cpp`, `src/ARMJIT.cpp`, `src/NDS.cpp`, `src/CP15.cpp` | Profile interpreter dispatch versus JIT execution/compilation, event-mask scans, and protection/timing-map rebuilds. Preserve event order, code invalidation and cycle timing when changing these paths. |
| DMA, `src/DMA.cpp`, `src/DSi_NDMA.cpp` | Transfers model bus timing and peripheral side effects. Bulk-copy candidates need proof that the source/destination are plain memory and that timing remains equivalent. |
| Audio, `src/SPU.cpp`, `src/frontend/qt_sdl/EmuInstanceAudio.cpp` | FIFO/rate/disabled-channel work is reduced and callback filling is fixed. Interpolation and resampling have synthetic profiles; gameplay samples are needed before altering sample processing or synchronization. |
| Frame pacing, `src/frontend/qt_sdl/EmuThread.cpp` | VSync, FPS limiting and audio sync deliberately wait. Measure uncapped throughput separately from normal-play frame pacing. |
| Saving/storage, `src/frontend/qt_sdl/SaveManager.cpp`, `src/FATStorage.cpp`, `src/DSi_SD.cpp` | Saving already debounces disk writes, but requested saves still copy the full save buffer under a mutex. Measure save-heavy workloads before introducing dirty ranges or changing snapshot ownership. |
| Networking, `src/net/LAN.cpp` | Discovery has repeated map lookups and a temporary removal list, but that is not evidence of a gameplay bottleneck. Profile active multiplayer packet processing and synchronization first. |
| Build, `CMakeLists.txt`, `cmake/DefaultBuildFlags.cmake` | Release builds already request optimization; JIT support is enabled in this build. GCC 15+ explicitly disables LTO due to a compiler issue. Compare a supported compiler/LTO or profile-guided build on a fixed workload rather than globally enabling unsafe floating-point transformations. |

## Reproduce the checks

Run from the repository root. On Bazzite, compilation can be done inside
`distrobox enter melonds-dev`. The shader script can run on the host using its
OpenGL/EGL driver; it needs only Python's standard library and supports the
surfaceless EGL platform.

### CPU checks

```sh
c++ -std=c++17 -O2 -Isrc tools/test_texcache.cpp \
  src/CRC32.cpp src/xxhash/xxhash.c -o build/test_texcache
./build/test_texcache

c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc tools/test_texcache.cpp src/CRC32.cpp src/xxhash/xxhash.c \
  -o build/test_texcache_sanitized
./build/test_texcache_sanitized
```

The tests cover 4,000 randomized texture/palette invalidation cases, exact
boundaries, empty ranges, single/multiple VRAM wraps, and CRC reference vectors,
seeded inputs, streaming and implicit zero-filled data.

### OpenGL transfer checks

```sh
distrobox enter melonds-gpu -- python3 tools/test_opengl.py
```

Requires a C++17 compiler, pkg-config, EGL and OpenGL development libraries.
The runner extracts the production capture methods and shaders into a minimal
VRAM fixture and creates real surfaceless GL 3.2 and 3.3 contexts. It checks 192
capture cases per context against full-surface GPU conversion, covering every
bank, offset and capture size at 1x/2x/4x, wrapped ranges, dirty flags and untouched
VRAM. Queued upload draws verify that changing VBO/IBO/UBO storage preserves VAO
bindings, split index ranges and earlier draw contents. Nearest/bilinear sampler
output is compared pixel-for-pixel with the texture-parameter fallback in GL 3.3.
It also prints paired synthetic upload/readback timings and GPU draw timings.
The complete application was rebuilt, and `tools/test_xbrz.py` passed in GL 3.2.
No game/ROM capture workload or interactive multiwindow session was run.

The same runner also checks 2,000 randomized cached-prefix uploads in each GL
3.2/3.3 context and queued compute dispatches in GL 4.3. Presentation checks use
the actual `prepareXbrz()` body and GPU stamp allocator: primitive queries verify
zero repeated analysis draws, on-demand layers, new frames, resizing and reset.
Metadata texture bindings are checked after emulating presentation cleanup.

### Audio callback and mixing checks

```sh
distrobox enter melonds-gpu -- python3 tools/test_audio_callback.py --sanitize
distrobox enter melonds-gpu -- python3 tools/profile_audio.py
```

The callback fixture compiles its production body and checks 6,300 combinations
of output/preferred buffer sizes, playback ratios, available samples, volume
and mute modes, including full output contents and guards. The profile runner
extracts channel dispatch, interpolation, panning and the channels-4..15 mixing
loop. It checks 10,000 dispatch/interpolation and 1,000 mixing cases against
independent reference dispatch/mixing, then times those methods and the real
blip library. Sample fetching is synthetic; this does not replace playback or
whole-emulator profiling.

### Audio-output checks

```sh
distrobox enter melonds-gpu -- python3 tools/test_audio.py --benchmark
distrobox enter melonds-gpu -- python3 tools/test_audio.py --sanitize
```

The runner extracts the current SPU output methods verbatim and compiles them
in an isolated mutex/blip fixture. Checks compare 72,000 FIFO operations to an
independent deque model across six ring sizes, including wraparound, multiple
wraps in a single write, overflow, underrun, zero-length requests, stereo order,
drains and output bounds. Rate tests check unchanged/changed skew, both channels,
output-rate changes and resets. Sanitizer checks cover the same cases.

To compare an earlier SPU source snapshot with the same FIFO checks/benchmark:

```sh
distrobox enter melonds-gpu -- python3 tools/test_audio.py \
  --source /path/to/previous/SPU.cpp --baseline
```

Baseline mode skips the new assertion that unchanged skew causes no rate calls.

### Texture-decoder checks

```sh
distrobox enter melonds-gpu -- c++ -std=c++17 -O3 -Isrc \
  tools/test_texture_decode.cpp -o build/test_texture_decode
distrobox enter melonds-gpu -- build/test_texture_decode --benchmark

distrobox enter melonds-gpu -- c++ -std=c++17 -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
  tools/test_texture_decode.cpp -o build/test_texture_decode_sanitized
distrobox enter melonds-gpu -- build/test_texture_decode_sanitized
```

The standalone test includes the production decoder against masked flat-VRAM
fixtures and compares 3,240 cases to independent per-pixel color/interpolation
formulas. It covers 2/4/8-bit indexed, compressed, A3I5 and A5I3 data in all three output formats,
dimensions from 8 through 1024, palette/texture wrapping and output bounds.
To benchmark an earlier decoder with the same harness, compile with
`-DTEXTURE_DECODER_SOURCE='"/absolute/path/to/old/GPU3D_Texcache.cpp"'`.

### Shader checks

```sh
python3 tools/test_xbrz.py
```

For differential testing, download the original upstream shader to a scratch
directory and pass its path. The script does not access the network itself:

```sh
curl -L --fail \
  https://raw.githubusercontent.com/libretro/glsl-shaders/c6a82efb2d811de70fd64a0971c819dfeb93d7d4/xbrz/shaders/xbrz-freescale.glsl \
  -o build/xbrz-freescale.glsl
python3 tools/test_xbrz.py --reference build/xbrz-freescale.glsl
```

This checks 500 randomized image/scale/layer/rotation combinations against
upstream (maximum allowed difference: one RGB8 quantization step), plus native
identity, layer isolation, edge clamping, diagonal smoothing, and switching
between native and enhanced source sizes. `--baseline /path/to/old/xbrz_shader.h`
additionally compares GPU timings with an earlier two-pass implementation.
It also requires pixel-identical output in 500 randomized cases and the
enhanced-source resizing checks when `--baseline` is supplied.

### Application build

```sh
cmake --build build -j4
```

For game profiling, use a fixed scene and identical renderer, resolution, filter,
JIT and synchronization settings. Capture CPU samples and GPU timings separately;
test both software and OpenGL rendering and report frame-time distributions in
addition to average FPS.
