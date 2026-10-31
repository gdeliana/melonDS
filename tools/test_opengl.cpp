// Real GL checks with verbatim production capture code in a minimal VRAM fixture.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <atomic>
#include <random>
#include <vector>
#define MELONDS_GL_HEADER <GL/glext.h>
#include "../src/OpenGL_Upload.h"
#include "../src/frontend/qt_sdl/xbrz_shader.h"
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
static void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s (GL error 0x%x)\n", message, glGetError()); std::exit(1); }
}
namespace Platform
{
enum class LogLevel { Error, Warn };
static void Log(LogLevel, const char*) {}
}
using Platform::LogLevel;
using Platform::Log;
struct GPUFixture
{
    std::array<std::array<u8, 128*1024>, 4> Storage;
    u8* VRAM[4];
    bool VRAMDirty[4][256] = {};
    GPUFixture() { for (int b = 0; b < 4; b++) VRAM[b] = Storage[b].data(); }
};
struct GLRenderer
{
    GPUFixture& GPU;
    GLuint CapDownShader, CaptureSyncFB, CaptureOutput128Tex, CaptureOutput256Tex;
    GLint CapDownInputLayerULoc;
    GLuint RectVtxBuffer, RectVtxArray;
    void DownscaleCapture(int, int, int);
    void SyncVRAMCapture(u32, u32, u32, bool);
};
struct GPUStamp
{
    u64 FramebufferVersion = 0;
    void InvalidateFramebuffer() noexcept;
};
struct ScreenPanelGL
{
    GLuint xbrzInfoTexture = 0, xbrzFramebuffer[2] = {}, xbrzAnalysisProgram = 0, screenVertexArray = 0;
    GLint xbrzAnalysisLayerULoc = 0;
    bool xbrzFramebufferComplete = false;
    int xbrzSourceWidth = 0, xbrzSourceHeight = 0;
    uint64_t analyzedFrameVersion = 0;
    unsigned int analyzedLayers = 0;
    bool prepareXbrz(int, int, unsigned int, uint64_t);
};
#include "capture_under_test.inc"

static GLuint Shader(GLenum type, const char* text)
{
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &text, nullptr); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); }
    Require(ok, "shader compilation"); return s;
}
static GLuint Program(const char* vs, const char* fs)
{
    GLuint p = glCreateProgram(), v = Shader(GL_VERTEX_SHADER, vs), f = Shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f); glBindAttribLocation(p, 0, "vPosition"); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "program linking");
    glDeleteShader(v); glDeleteShader(f); return p;
}
static void Target(GLuint& tex, GLuint& fb, int w, int h, GLenum format = GL_RGBA8)
{
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "target framebuffer");
}
template<class Before, class After> static std::array<double, 2> Compare(Before before, After after)
{
    std::vector<double> times[2];
    for (int warmup = 0; warmup < 20; warmup++) { before(); after(); }
    for (int batch = 0; batch < 5; batch++)
        for (int phase = 0; phase < 2; phase++)
    {
        const int mode = (batch+phase)%2;
        glFinish();
        auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; i++) { if (mode) after(); else before(); }
        glFinish();
        auto end = std::chrono::steady_clock::now();
        times[mode].push_back(std::chrono::duration<double, std::micro>(end-begin).count()/100);
    }
    for (auto& time : times) std::sort(time.begin(), time.end());
    return {times[0][2], times[1][2]};
}
static double DrawTime()
{
    std::vector<double> batches;
    for (int batch = 0; batch < 5; batch++)
    {
        GLuint queries[40]; glGenQueries(40, queries);
        for (GLuint query : queries)
        {
            glBeginQuery(GL_TIME_ELAPSED, query); glDrawArrays(GL_TRIANGLES, 0, 6); glEndQuery(GL_TIME_ELAPSED);
        }
        double total = 0;
        for (GLuint query : queries)
        {
            GLuint64 elapsed; glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed); total += elapsed;
        }
        glDeleteQueries(40, queries); batches.push_back(total/40/1000);
    }
    std::sort(batches.begin(), batches.end()); return batches[2];
}
static void CaptureChecks()
{
    auto gpu = std::make_unique<GPUFixture>();
    GLRenderer r{*gpu};
    r.CapDownShader = Program(CaptureDownscaleVS, CaptureDownscaleFS);
    r.CapDownInputLayerULoc = glGetUniformLocation(r.CapDownShader, "uInputLayer");
    glUseProgram(r.CapDownShader); glUniform1i(glGetUniformLocation(r.CapDownShader, "InputTex"), 0);
    const float quad[] = {0,0, 1,1, 0,1, 0,0, 1,0, 1,1};
    glGenVertexArrays(1, &r.RectVtxArray); glBindVertexArray(r.RectVtxArray);
    glGenBuffers(1, &r.RectVtxBuffer); glBindBuffer(GL_ARRAY_BUFFER, r.RectVtxBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    GLuint syncTex; Target(syncTex, r.CaptureSyncFB, 256, 256, GL_RGB5_A1);
    glGenTextures(1, &r.CaptureOutput128Tex); glGenTextures(1, &r.CaptureOutput256Tex);
    int cases = 0;
    for (int scale : {1, 2, 4})
    {
        for (int width : {128, 256})
        {
            int layers = width == 128 ? 16 : 4, size = width*scale;
            GLuint tex = width == 128 ? r.CaptureOutput128Tex : r.CaptureOutput256Tex;
            std::vector<u8> pixels(size*size*layers*4);
            for (size_t i = 0; i < pixels.size(); i++) pixels[i] = (i*13 + i/257 + i/4) & 255;
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }
        for (u32 bank = 0; bank < 4; bank++)
            for (u32 start = 0; start < 4; start++)
                for (u32 len = 0; len <= 3; len++)
                {
                    const int width = len == 0 ? 128 : 256;
                    std::vector<u16> full(width*width);
                    glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
                    r.DownscaleCapture(width, width, len == 0 ? (bank*4+start) : bank);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, r.CaptureSyncFB);
                    glReadPixels(0, 0, width, width, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, full.data());
                    for (auto& vram : gpu->Storage) vram.fill(0xA5);
                    memset(gpu->VRAMDirty, 0, sizeof(gpu->VRAMDirty));
                    r.SyncVRAMCapture(bank, start, len, true);
                    std::array<u8, 128*1024> expected; expected.fill(0xA5);
                    bool dirty[256] = {};
                    if (len == 0)
                    {
                        memcpy(expected.data()+start*32768, full.data(), 32768);
                        std::fill(dirty+start*64, dirty+(start+1)*64, true);
                    }
                    else for (u32 block = 0; block < len; block++)
                    {
                        u32 pos = (start+block)%4;
                        memcpy(expected.data()+pos*32768, full.data()+pos*16384, 32768);
                        std::fill(dirty+pos*64, dirty+(pos+1)*64, true);
                    }
                    for (u32 b = 0; b < 4; b++)
                    {
                        if (b == bank)
                        {
                            Require(gpu->Storage[b] == expected, "capture bytes / untouched VRAM");
                            Require(memcmp(gpu->VRAMDirty[b], dirty, sizeof(dirty)) == 0, "capture dirty map");
                        }
                        else
                        {
                            for (u8 value : gpu->Storage[b]) Require(value == 0xA5, "untouched bank");
                            for (bool value : gpu->VRAMDirty[b]) Require(!value, "untouched bank dirty map");
                        }
                    }
                    Require(!glIsEnabled(GL_SCISSOR_TEST), "capture restores scissor disable");
                    Require(glGetError() == GL_NO_ERROR, "capture GL state"); cases++;
                }
    }
    std::printf("PASS: %d capture cases vs full-surface conversion, all sizes/offsets/banks, 1x/2x/4x, wrapping, dirty flags and untouched VRAM\n", cases);
    // Synchronous draw + readback cost on the final 4x source textures.
    for (u32 len : {1u, 2u, 3u})
    {
        auto times = Compare([&] {
            glDisable(GL_SCISSOR_TEST);
            r.DownscaleCapture(256, 256, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, r.CaptureSyncFB);
            glReadPixels(0, 0, 256, len*64, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, gpu->VRAM[0]);
            std::fill(gpu->VRAMDirty[0], gpu->VRAMDirty[0]+len*64, true);
        }, [&] { r.SyncVRAMCapture(0, 0, len, true); });
        std::printf("4x capture, %u rows: full conversion %.2f us, partial %.2f us (includes synchronous readback)\n", len*64, times[0], times[1]);
        r.DownscaleCapture(256, 256, 0);
        glDisable(GL_SCISSOR_TEST); double fullGPU = DrawTime();
        glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 256, len*64); double partialGPU = DrawTime();
        glDisable(GL_SCISSOR_TEST);
        std::printf("  GPU downscale draw only: full %.2f us, partial %.2f us\n", fullGPU, partialGPU);
    }
    glDeleteTextures(1, &r.CaptureOutput128Tex); glDeleteTextures(1, &r.CaptureOutput256Tex);
    glDeleteFramebuffers(1, &r.CaptureSyncFB); glDeleteTextures(1, &syncTex);
    glDeleteVertexArrays(1, &r.RectVtxArray); glDeleteBuffers(1, &r.RectVtxBuffer); glDeleteProgram(r.CapDownShader);
}

static void UploadChecks()
{
    GLuint p = Program("#version 140\nin vec2 vPosition; void main(){gl_Position=vec4(vPosition,0,1);}",
        "#version 140\nlayout(std140) uniform Config {vec4 color;}; out vec4 c; void main(){c=color;}");
    glUseProgram(p);
    GLuint tex, fb; Target(tex, fb, 64, 64);
    GLuint vao, vbo, ibo, ubo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 10240*7*4, nullptr, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    glGenBuffers(1, &ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2048*40*2, nullptr, GL_STREAM_DRAW);
    glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_STREAM_DRAW); glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
    constexpr size_t edge = 2048*30*2;
    for (bool orphan : {false, true})
    {
        // Queue all draws before reading any pixels, so earlier draws must
        // retain their buffer contents while subsequent uploads replace them.
        for (int i = 0; i < 64; i++)
        {
            float vertices[] = {-1,-1, 1,-1, -1,1, 1,1};
            if (i%2) { vertices[0] = 1; vertices[2] = -1; vertices[4] = 1; vertices[6] = -1; }
            const u16 main[] = {0,1,2}, edges[] = {1,3,2};
            const float color[] = {float(i*3)/255, float(255-i*3)/255, 0, 1};
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            if (orphan) glBufferData(GL_UNIFORM_BUFFER, 16, color, GL_STREAM_DRAW);
            else
            {
                void* mapped = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
                Require(mapped, "map baseline UBO"); memcpy(mapped, color, 16); glUnmapBuffer(GL_UNIFORM_BUFFER);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            if (orphan) glBufferData(GL_ARRAY_BUFFER, 10240*7*4, nullptr, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
            glBindVertexArray(vao);
            if (orphan) glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2048*40*2, nullptr, GL_STREAM_DRAW);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(main), main);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, edge, sizeof(edges), edges);
            glViewport((i%8)*8, (i/8)*8, 8, 8);
            glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
            glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(edge));
        }
        std::array<u8, 64*64*4> pixels;
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        {
            int i = x/8 + (y/8)*8, offset = (x+y*64)*4;
            Require(pixels[offset] == i*3 && pixels[offset+1] == 255-i*3, "queued buffer uploads / VAO / split indices / UBO");
        }
        Require(glGetError() == GL_NO_ERROR, "upload GL state");
    }
    // Exercise both small and large uploads at the production buffer capacities
    // and 28-byte vertex stride. This is synthetic throughput, not game FPS.
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 28, nullptr);
    for (int vertices : {12, 1023, 9999})
    {
        std::vector<float> data(vertices*7);
        std::vector<u16> indices(vertices);
        for (int i = 0; i < vertices; i++)
        {
            data[i*7] = i%3 == 1 ? 1 : -1;
            data[i*7+1] = i%3 == 2 ? 1 : -1;
            indices[i] = i;
        }
        std::array<float, 308> config{}; config[3] = 1;
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(config), nullptr, GL_STREAM_DRAW);
        glViewport(0, 0, 1, 1);
        auto upload = [&](bool orphan) {
            config[0] += 0.001f;
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            if (orphan) glBufferData(GL_UNIFORM_BUFFER, sizeof(config), config.data(), GL_STREAM_DRAW);
            else
            {
                void* mapped = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
                Require(mapped, "benchmark UBO mapping");
                memcpy(mapped, config.data(), sizeof(config)); glUnmapBuffer(GL_UNIFORM_BUFFER);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            if (orphan) glBufferData(GL_ARRAY_BUFFER, 10240*7*4, nullptr, GL_STREAM_DRAW);
            glBufferSubData(GL_ARRAY_BUFFER, 0, data.size()*sizeof(float), data.data());
            glBindVertexArray(vao);
            if (orphan) glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2048*40*2, nullptr, GL_STREAM_DRAW);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices.size()*2, indices.data());
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, edge, 6, indices.data());
            glDrawElements(GL_TRIANGLES, vertices, GL_UNSIGNED_SHORT, nullptr);
            glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(edge));
        };
        auto results = Compare([&] { upload(false); }, [&] { upload(true); });
        std::printf("%d vertices, upload + draw: mapped/reused %.2f us, orphaned %.2f us\n", vertices, results[0], results[1]);
    }
    glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ibo); glDeleteBuffers(1, &ubo);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(p); glDeleteTextures(1, &tex); glDeleteFramebuffers(1, &fb);
    std::puts("PASS: queued baseline/orphaned VBO, split IBO and UBO uploads retain pixels and bindings");
}

static void SamplerChecks()
{
    GLuint p = Program("#version 140\nout vec3 uv; void main(){vec2 a=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(a*2-1,0,1);uv=vec3(a,0);}",
        "#version 140\nuniform sampler2DArray image; in vec3 uv; out vec4 c; void main(){c=texture(image,uv);}");
    glUseProgram(p); glUniform1i(glGetUniformLocation(p, "image"), 0);
    GLuint tex, fb; Target(tex, fb, 17, 11);
    GLuint input, vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenTextures(1, &input); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, input);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::array<u8, 4*4*4> source;
    for (size_t i = 0; i < source.size(); i++) source[i] = i*37;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 4, 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
    GLuint samplers[2]; glGenSamplers(2, samplers);
    for (int i = 0; i < 2; i++)
    {
        GLint filter = i ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, filter);
        glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, filter);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        std::array<u8, 17*11*4> reference, actual;
        glViewport(0, 0, 17, 11); glBindSampler(0, 0);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filter);
        glDrawArrays(GL_TRIANGLES, 0, 3); glReadPixels(0, 0, 17, 11, GL_RGBA, GL_UNSIGNED_BYTE, reference.data());
        // Deliberately make the texture's own filter disagree with its sampler.
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, i ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, i ? GL_NEAREST : GL_LINEAR);
        glBindSampler(0, samplers[i]); glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, 17, 11, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        Require(reference == actual, "sampler and GL 3.2 texture-filter paths agree");
    }
    glBindSampler(0, 0); glDeleteSamplers(2, samplers);
    Require(glGetError() == GL_NO_ERROR, "sampler GL state");
    glDeleteProgram(p); glDeleteTextures(1, &tex); glDeleteTextures(1, &input);
    glDeleteFramebuffers(1, &fb); glDeleteVertexArrays(1, &vao);
    std::puts("PASS: nearest/bilinear sampler objects match texture parameters, including edge clamping");
}

static void CachedUploadChecks()
{
    melonDS::OpenGL::BufferUploadCache cache;
    GLuint buffer; glGenBuffers(1,&buffer); glBindBuffer(GL_ARRAY_BUFFER,buffer);
    std::vector<u8> expected(8192,0), actual(8192), data(8192);
    glBufferData(GL_ARRAY_BUFFER,8192,expected.data(),GL_STREAM_DRAW);
    std::mt19937 random(0xCA43);
    int skips=0;
    for (int i=0;i<2000;i++)
    {
        size_t size = (random()%1025)*8;
        if (i%3==0) for (auto& byte:data) byte=random();
        bool sent=cache.Upload(GL_ARRAY_BUFFER,buffer,data.data(),size);
        if (!sent) skips++;
        std::copy(data.begin(),data.begin()+size,expected.begin());
        glBindBuffer(GL_ARRAY_BUFFER,buffer);
        glGetBufferSubData(GL_ARRAY_BUFFER,0,actual.size(),actual.data());
        Require(actual==expected,"cached upload prefixes and untouched tails");
        Require(!cache.Upload(GL_ARRAY_BUFFER,buffer,data.data(),size),"repeat upload must skip");
        if (i%101==0)
        {
            cache.Clear();
            std::fill(expected.begin(),expected.end(),0);
            glBufferData(GL_ARRAY_BUFFER,8192,expected.data(),GL_STREAM_DRAW);
        }
    }
    Require(skips>0,"cache skips redundant uploads");
    for (int size:{48,144,8192})
    {
        cache.Clear();
        auto times=Compare([&]{glBindBuffer(GL_ARRAY_BUFFER,buffer);glBufferSubData(GL_ARRAY_BUFFER,0,size,data.data());},
            [&]{cache.Upload(GL_ARRAY_BUFFER,buffer,data.data(),size);});
        std::printf("Unchanged %d-byte 2D upload: always %.3f us, cached %.3f us\n",size,times[0],times[1]);
    }
    glDeleteBuffers(1,&buffer);
    Require(glGetError()==GL_NO_ERROR,"cached upload GL state");
    std::puts("PASS: 2,000 cached upload cases, repeated/shrinking/growing prefixes, storage recreation and unchanged tails");
}

static void PresentationChecks()
{
    GPUStamp a,b;
    a.InvalidateFramebuffer();b.InvalidateFramebuffer();
    Require(a.FramebufferVersion!=b.FramebufferVersion,"new GPUs have distinct stamps");
    auto old=a.FramebufferVersion;a.InvalidateFramebuffer();Require(a.FramebufferVersion>old,"monotonic frame stamp");
    ScreenPanelGL panel;
    panel.xbrzAnalysisProgram=Program(kScreenVS_XbrzAnalysis,kScreenFS_XbrzAnalysis);
    panel.xbrzAnalysisLayerULoc=glGetUniformLocation(panel.xbrzAnalysisProgram,"uLayer");
    glUseProgram(panel.xbrzAnalysisProgram);glUniform1i(glGetUniformLocation(panel.xbrzAnalysisProgram,"ScreenTex"),0);
    glGenVertexArrays(1,&panel.screenVertexArray);
    glGenTextures(1,&panel.xbrzInfoTexture);glGenFramebuffers(2,panel.xbrzFramebuffer);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D_ARRAY,panel.xbrzInfoTexture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    GLuint input;glGenTextures(1,&input);
    auto resize=[&](int size){
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D_ARRAY,input);
        std::vector<u8> pixels(size*size*2*4,0);
        glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,size,size,2,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    };
    resize(4);
    auto check=[&](int size,unsigned int layers,uint64_t version,GLuint draws){
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D_ARRAY,0);glActiveTexture(GL_TEXTURE0);
        GLuint query;glGenQueries(1,&query);glBeginQuery(GL_PRIMITIVES_GENERATED,query);
        Require(panel.prepareXbrz(size,size,layers,version),"prepare cached analysis");
        glEndQuery(GL_PRIMITIVES_GENERATED);GLuint actual;glGetQueryObjectuiv(query,GL_QUERY_RESULT,&actual);
        Require(actual==draws,"analysis must run only for fresh frames / newly visible layers");glDeleteQueries(1,&query);
        GLint bound;glActiveTexture(GL_TEXTURE1);glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,&bound);
        Require(GLuint(bound)==panel.xbrzInfoTexture,"cached analysis restores metadata binding after presentation cleanup");
        glActiveTexture(GL_TEXTURE0);
    };
    check(4,1,1,1);check(4,1,1,0);check(4,3,1,1);check(4,2,1,0);
    check(4,3,2,2);resize(8);check(8,3,2,2);check(8,3,2,0);
    panel.analyzedFrameVersion=0;panel.analyzedLayers=0;check(8,3,2,2);
    glDeleteProgram(panel.xbrzAnalysisProgram);glDeleteTextures(1,&panel.xbrzInfoTexture);
    glDeleteTextures(1,&input);glDeleteFramebuffers(2,panel.xbrzFramebuffer);glDeleteVertexArrays(1,&panel.screenVertexArray);
    Require(glGetError()==GL_NO_ERROR,"presentation cache GL state");
    std::puts("PASS: frame identity, repeated presentation, new layers, frame changes, resizing and cache reset");
}

static void ComputeUploadChecks()
{
    const char* shader=R"GLSL(#version 430
layout(local_size_x=1) in;
layout(std430,binding=0) readonly buffer Input {uint values[];};
layout(std430,binding=1) writeonly buffer Output {uint results[];};
layout(std140,binding=0) uniform Meta {uvec4 meta;};
layout(rgba16ui,binding=0) readonly uniform uimageBuffer spans;
uniform uint outIndex; uniform uint lastIndex;
void main(){results[outIndex]=values[lastIndex]+imageLoad(spans,int(lastIndex)).r+meta.z;}
)GLSL";
    GLuint p=glCreateProgram(), sh=Shader(GL_COMPUTE_SHADER,shader);
    glAttachShader(p,sh);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);Require(ok,"compute linking");
    glDeleteShader(sh);glUseProgram(p);
    GLuint buffers[4],texture;glGenBuffers(4,buffers);glGenTextures(1,&texture);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);glBufferData(GL_SHADER_STORAGE_BUFFER,4096,nullptr,GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);glBufferData(GL_SHADER_STORAGE_BUFFER,64*4,nullptr,GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER,buffers[2]);glBufferData(GL_TEXTURE_BUFFER,8192,nullptr,GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER,texture);glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA16UI,buffers[2]);
    glBindBuffer(GL_UNIFORM_BUFFER,buffers[3]);glBufferData(GL_UNIFORM_BUFFER,16,nullptr,GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffers[0]);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,buffers[1]);
    glBindBufferBase(GL_UNIFORM_BUFFER,0,buffers[3]);glBindImageTexture(0,texture,0,GL_FALSE,0,GL_READ_ONLY,GL_RGBA16UI);
    melonDS::OpenGL::BufferUploadCache metaCache;
    const GLint outIndex=glGetUniformLocation(p,"outIndex"), lastIndex=glGetUniformLocation(p,"lastIndex");
    auto upload=[&](int frame,int mode){
        const bool replace=mode==1;
        const int count=frame==63?1024:1+(frame*37)%1024;
        std::vector<u32> values(count);std::vector<u16> spans(count*4);
        for(int j=0;j<count;j++){values[j]=frame*1000+j;spans[j*4]=frame*7+j;}
        u32 meta[4]={0,0,11,0};
        glUniform1ui(outIndex,frame);glUniform1ui(lastIndex,count-1);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);
        if(replace)glBufferData(GL_SHADER_STORAGE_BUFFER,count*4,values.data(),GL_STREAM_DRAW);
        else glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,count*4,values.data());
        glBindBuffer(GL_TEXTURE_BUFFER,buffers[2]);
        if(replace)glBufferData(GL_TEXTURE_BUFFER,count*8,spans.data(),GL_STREAM_DRAW);
        else glBufferSubData(GL_TEXTURE_BUFFER,0,count*8,spans.data());
        glBindBuffer(GL_UNIFORM_BUFFER,buffers[3]);
        if(replace)glBufferData(GL_UNIFORM_BUFFER,16,meta,GL_STREAM_DRAW);
        else if(mode==2)metaCache.Upload(GL_UNIFORM_BUFFER,buffers[3],meta,16);
        else glBufferSubData(GL_UNIFORM_BUFFER,0,16,meta);
        glDispatchCompute(1,1,1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    };
    for(int mode:{0,1,2})
    {
        metaCache.Clear();
        // Restore full capacity before testing baseline subdata after resizing.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);glBufferData(GL_SHADER_STORAGE_BUFFER,4096,nullptr,GL_STREAM_DRAW);
        glBindBuffer(GL_TEXTURE_BUFFER,buffers[2]);glBufferData(GL_TEXTURE_BUFFER,8192,nullptr,GL_STREAM_DRAW);
        for(int frame=0;frame<64;frame++)upload(frame,mode);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        std::array<u32,64> actual;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(actual),actual.data());
        for(int frame=0;frame<64;frame++)
            Require(actual[frame]==u32(frame*1007+2*(frame==63?1023:(frame*37)%1024)+11),"queued SSBO/texture/UBO resize preserves earlier dispatches");
    }
    // Paired throughput with populated ranges, matching the replacement strategy.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);glBufferData(GL_SHADER_STORAGE_BUFFER,4096,nullptr,GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER,buffers[2]);glBufferData(GL_TEXTURE_BUFFER,8192,nullptr,GL_STREAM_DRAW);
    auto timings=Compare([&]{
        for(int i=0;i<64;i++)upload(i,0);
    },[&]{for(int i=0;i<64;i++)upload(i,1);});
    std::printf("Compute uploads + 64 dispatches: reused %.2f us, replacement experiment %.2f us\n",timings[0],timings[1]);
    auto cached=Compare([&]{for(int i=0;i<64;i++)upload(i,0);},[&]{for(int i=0;i<64;i++)upload(i,2);});
    std::printf("Compute unchanged metadata, 64 dispatches: always %.2f us, cached %.2f us\n",cached[0],cached[1]);
    Require(glGetError()==GL_NO_ERROR,"compute upload GL state");
    glDeleteProgram(p);glDeleteTextures(1,&texture);glDeleteBuffers(4,buffers);
    std::puts("PASS: queued compute uploads and metadata cache, growing/shrinking ranges, dispatch results and bindings");
}

int main(int argc, char** argv)
{
    const int major = argc > 1 && strcmp(argv[1], "--gl43") == 0 ? 4 : 3;
    const int minor = argc > 1 && strcmp(argv[1], "--gl32") == 0 ? 2 : 3;
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialization");
    Require(eglBindAPI(EGL_OPENGL_API), "desktop OpenGL");
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
    EGLConfig config; EGLint count;
    Require(eglChooseConfig(display, configAttrs, &config, 1, &count) && count, "EGL config");
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION,major,EGL_CONTEXT_MINOR_VERSION,minor,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
    Require(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "OpenGL core context");
    std::printf("GPU: %s | OpenGL: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (major == 4) ComputeUploadChecks();
    else
    {
        CaptureChecks(); UploadChecks(); CachedUploadChecks(); PresentationChecks();
        if (minor >= 3) SamplerChecks();
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglTerminate(display);
}
