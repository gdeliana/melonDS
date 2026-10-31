#!/usr/bin/env python3
"""Linux/EGL xBRZ regression checks and GPU microbenchmarks (stdlib only).

Optional --reference accepts upstream xbrz-freescale.glsl for differential tests.
Optional --baseline accepts an earlier melonDS xbrz_shader.h for GPU comparisons.
No ROM, Qt application, or network access is needed. See PERFORMANCE.md.
"""
import argparse
import ctypes as C
import ctypes.util
import math
from pathlib import Path
import random
import re
import statistics


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, args
    return fn


I, U, F, P = C.c_int, C.c_uint, C.c_float, C.c_void_p
egl = C.CDLL(ctypes.util.find_library('EGL'))
gl = C.CDLL(ctypes.util.find_library('GL'))
display = bind(egl, 'eglGetPlatformDisplay', P, U, P, P)(0x31DD, None, None)
require(bind(egl, 'eglInitialize', U, P, P, P)(display, None, None), 'EGL initialization failed')
require(bind(egl, 'eglBindAPI', U, U)(0x30A2), 'Desktop OpenGL unavailable')
attrs = (I * 7)(0x3024, 8, 0x3033, 1, 0x3040, 8, 0x3038)
config, count = P(), I()
require(bind(egl, 'eglChooseConfig', U, P, P, P, I, P)(display, attrs, C.byref(config), 1, C.byref(count))
        and count.value, 'EGL config unavailable')
ctxattrs = (I * 7)(0x3098, 3, 0x30FB, 2, 0x30FD, 1, 0x3038)
context = bind(egl, 'eglCreateContext', P, P, P, P, P)(display, config, None, ctxattrs)
require(context and bind(egl, 'eglMakeCurrent', U, P, P, P, P)(display, None, None, context),
        'OpenGL 3.2 core context unavailable')

# Set signatures once; timed submissions should do as little Python work as possible.
for name, result, args in [
    ('GetString', C.c_char_p, [U]), ('GetError', U, []),
    ('CreateShader', U, [U]), ('ShaderSource', None, [U,I,P,P]),
    ('CompileShader', None, [U]), ('GetShaderiv', None, [U,U,P]),
    ('GetShaderInfoLog', None, [U,I,P,P]), ('CreateProgram', U, []),
    ('AttachShader', None, [U,U]), ('LinkProgram', None, [U]),
    ('GetProgramiv', None, [U,U,P]), ('GetProgramInfoLog', None, [U,I,P,P]),
    ('UseProgram', None, [U]), ('GetUniformLocation', I, [U,C.c_char_p]),
    ('Uniform1i', None, [I,I]), ('Uniform4f', None, [I,F,F,F,F]),
    ('GenVertexArrays', None, [I,P]), ('BindVertexArray', None, [U]),
    ('GenTextures', None, [I,P]), ('BindTexture', None, [U,U]),
    ('ActiveTexture', None, [U]), ('TexParameteri', None, [U,U,I]),
    ('TexImage3D', None, [U,I,I,I,I,I,I,U,U,P]), ('TexImage2D', None, [U,I,I,I,I,I,U,U,P]),
    ('GenFramebuffers', None, [I,P]), ('BindFramebuffer', None, [U,U]),
    ('FramebufferTextureLayer', None, [U,U,U,I,I]), ('FramebufferTexture2D', None, [U,U,U,U,I]),
    ('CheckFramebufferStatus', U, [U]), ('Viewport', None, [I,I,I,I]),
    ('DrawArrays', None, [U,I,I]), ('ReadPixels', None, [I,I,I,I,U,U,P]),
    ('Disable', None, [U]), ('GenQueries', None, [I,P]),
    ('BeginQuery', None, [U,U]), ('EndQuery', None, [U]),
    ('GetQueryObjectui64v', None, [U,U,P]), ('DeleteQueries', None, [I,P]),
]:
    bind(gl, 'gl' + name, result, *args)

print('GPU:', gl.glGetString(0x1F01).decode(), '| OpenGL:', gl.glGetString(0x1F02).decode())


def shader(kind, text):
    obj = gl.glCreateShader(kind)
    encoded = C.c_char_p(text.encode())
    gl.glShaderSource(obj, 1, C.byref(encoded), None)
    gl.glCompileShader(obj)
    ok, log = I(), C.create_string_buffer(16384)
    gl.glGetShaderiv(obj, 0x8B81, C.byref(ok))
    gl.glGetShaderInfoLog(obj, len(log), None, log)
    require(ok.value, log.value.decode())
    return obj


def program(vs, fs):
    obj = gl.glCreateProgram()
    gl.glAttachShader(obj, shader(0x8B31, vs))
    gl.glAttachShader(obj, shader(0x8B30, fs))
    gl.glLinkProgram(obj)
    ok, log = I(), C.create_string_buffer(16384)
    gl.glGetProgramiv(obj, 0x8B82, C.byref(ok))
    gl.glGetProgramInfoLog(obj, len(log), None, log)
    require(ok.value, log.value.decode())
    return obj


def source(text, name):
    return re.search(name + r' = R"\((.*?)\)";', text, re.S)[1]


VS = '''#version 140
smooth out vec3 fTexcoord;
uniform int layer;
uniform int rotate;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    fTexcoord = vec3(rotate == 1 ? vec2(p.y, 1.0-p.x) : p, float(layer));
}'''
PLAIN = '''#version 140
uniform sampler2DArray ScreenTex;
smooth in vec3 fTexcoord;
out vec4 oColor;
void main() { oColor = vec4(texture(ScreenTex, fTexcoord).rgb, 1.0); }
'''
plain = program(VS, PLAIN)
vao, input_texture, output_texture, output_fbo = U(), U(), U(), U()
gl.glGenVertexArrays(1, C.byref(vao))
gl.glBindVertexArray(vao)
gl.glGenTextures(1, C.byref(input_texture))
gl.glBindTexture(0x8C1A, input_texture)
for param in (0x2800, 0x2801):
    gl.glTexParameteri(0x8C1A, param, 0x2600)
gl.glGenTextures(1, C.byref(output_texture))
gl.glBindTexture(0x0DE1, output_texture)
gl.glGenFramebuffers(1, C.byref(output_fbo))
gl.glBindFramebuffer(0x8D40, output_fbo)
gl.glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, output_texture, 0)
gl.glDisable(0x0BD0)  # Dithering; required by the old RGBA8 metadata baseline.


def upload(width, height, pixels):
    gl.glActiveTexture(0x84C0)
    gl.glBindTexture(0x8C1A, input_texture)
    gl.glTexImage3D(0x8C1A, 0, 0x8058, width, height, 2, 0, 0x1908, 0x1401,
                    C.create_string_buffer(pixels))


def output_size(width, height):
    gl.glActiveTexture(0x84C0)
    gl.glTexImage2D(0x0DE1, 0, 0x8058, width, height, 0, 0x1908, 0x1401, None)


class Pipeline:
    def __init__(self, header=None, fragment=None):
        self.program = program(VS, source(header, 'kScreenFS_Xbrz') if header else fragment)
        self.analysis = None
        self.size = None
        self.locations = {name: gl.glGetUniformLocation(self.program, name.encode())
                          for name in ('layer', 'rotate', 'uNormalScale')}
        if header:
            self.analysis = program(source(header, 'kScreenVS_XbrzAnalysis'), source(header, 'kScreenFS_XbrzAnalysis'))
            self.analysis_layer = gl.glGetUniformLocation(self.analysis, b'uLayer')
            self.integer = 'uniform usampler2DArray XbrzInfoTex' in header
            self.info = U()
            self.fbos = (U * 2)()
            gl.glGenTextures(1, C.byref(self.info))
            gl.glGenFramebuffers(2, self.fbos)
            gl.glActiveTexture(0x84C1)
            gl.glBindTexture(0x8C1A, self.info)
            for param in (0x2800, 0x2801):
                gl.glTexParameteri(0x8C1A, param, 0x2600)
            gl.glActiveTexture(0x84C0)
            gl.glUseProgram(self.program)
            gl.glUniform1i(gl.glGetUniformLocation(self.program, b'XbrzInfoTex'), 1)

    def prepare(self, width, height):
        if not self.analysis:
            return
        gl.glActiveTexture(0x84C1)
        gl.glBindTexture(0x8C1A, self.info)
        if self.size != (width, height):
            fmt, layout, kind = (0x8236, 0x8D94, 0x1405) if self.integer else (0x8058, 0x1908, 0x1401)
            gl.glTexImage3D(0x8C1A, 0, fmt, width, height, 2, 0, layout, kind, None)
            for layer in range(2):
                gl.glBindFramebuffer(0x8D40, self.fbos[layer])
                gl.glFramebufferTextureLayer(0x8D40, 0x8CE0, self.info, 0, layer)
                require(gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, 'Incomplete analysis FBO')
            self.size = (width, height)
        gl.glActiveTexture(0x84C0)

    def analyze(self, width, height, draw):
        if not self.analysis:
            return
        gl.glUseProgram(self.analysis)
        gl.glViewport(0, 0, width, height)
        for layer in range(2):
            gl.glBindFramebuffer(0x8D40, self.fbos[layer])
            gl.glUniform1i(self.analysis_layer, layer)
            draw()

    def display(self, width, height, sw, sh, layer, rotate, draw):
        sx, sy = (height/sw, width/sh) if rotate else (width/sw, height/sh)
        # Mirror the frontend: native/downscaled screens use the plain shader.
        effective = plain if max(sx, sy) <= 1.0001 else self.program
        gl.glUseProgram(effective)
        for name, value in (('layer', layer), ('rotate', rotate)):
            loc = self.locations[name] if effective == self.program else gl.glGetUniformLocation(plain, name.encode())
            gl.glUniform1i(loc, value)
        if effective == self.program and self.locations['uNormalScale'] >= 0:
            sx, sy = max(sx, 1.0), max(sy, 1.0)
            diagonal = math.hypot(sx, sy)
            gl.glUniform4f(self.locations['uNormalScale'], diagonal/2, math.hypot(sx, 2*sy)/5,
                           math.hypot(2*sx, sy)/5, diagonal/4)
        gl.glBindFramebuffer(0x8D40, output_fbo)
        gl.glViewport(0, 0, width, height)
        draw()


def draw():
    gl.glDrawArrays(4, 0, 3)


def render(pipeline, sw, sh, width, height, layer=0, rotate=0):
    output_size(width, height)
    pipeline.prepare(sw, sh)
    pipeline.analyze(sw, sh, draw)
    pipeline.display(width, height, sw, sh, layer, rotate, draw)
    pixels = (C.c_ubyte * (width*height*4))()
    gl.glReadPixels(0, 0, width, height, 0x1908, 0x1401, pixels)
    require(gl.glGetError() == 0, 'OpenGL error')
    return bytes(pixels)


def upstream_fragment(text):
    fs = text[text.index('#define BLEND_NONE'):text.rindex('#endif')]
    fs = fs.replace('#define P(x,y) COMPAT_TEXTURE(Source, coord + SourceSize.zw * vec2(x, y)).rgb',
                    '#define P(x,y) texture(ScreenTex, vec3(clamp(coord + SourceSize.zw * vec2(x,y), '
                    'SourceSize.zw * 0.5, vec2(1.0) - SourceSize.zw * 0.5), fTexcoord.z)).rgb')
    fs = fs.replace('vec2 scale = OutputSize.xy * SourceSize.zw;',
                    'vec2 t = fTexcoord.xy * SourceSize.xy; vec2 scale = max(vec2(1.0), '
                    '1.0 / vec2(length(vec2(dFdx(t.x),dFdy(t.x))), length(vec2(dFdx(t.y),dFdy(t.y)))));')
    return '''#version 140
uniform sampler2DArray ScreenTex;
smooth in vec3 fTexcoord;
out vec4 FragColor;
#define vTexCoord fTexcoord.xy
#define SourceSize vec4(vec2(textureSize(ScreenTex,0).xy), 1.0/vec2(textureSize(ScreenTex,0).xy))
''' + fs


def benchmark(pipeline, scale):
    width, height = 256*scale, 192*scale
    output_size(width, height)
    pipeline.prepare(256, 192)
    for _ in range(5):
        pipeline.analyze(256, 192, draw)
        for layer in range(2):
            pipeline.display(width, height, 256, 192, layer, 0, draw)
    times = []
    # Time individual draw calls, not Python's uniform/state setup or readback.
    # Collect results after submitting the batch to avoid per-draw CPU/GPU sync.
    for _ in range(5):
        queries = (U * (40 * (4 if pipeline.analysis else 2)))()
        gl.glGenQueries(len(queries), queries)
        index = 0

        def timed_draw():
            nonlocal index
            gl.glBeginQuery(0x88BF, queries[index])  # GL_TIME_ELAPSED
            gl.glDrawArrays(4, 0, 3)
            gl.glEndQuery(0x88BF)
            index += 1

        for _ in range(40):
            pipeline.analyze(256, 192, timed_draw)
            for layer in range(2):
                pipeline.display(width, height, 256, 192, layer, 0, timed_draw)
        total = 0
        for query in queries:
            ns = C.c_uint64()
            gl.glGetQueryObjectui64v(query, 0x8866, C.byref(ns))
            total += ns.value
        gl.glDeleteQueries(len(queries), queries)
        times.append(total / 40 / 1e6)
    return statistics.median(times)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    header = (root / 'src/frontend/qt_sdl/xbrz_shader.h').read_text()
    program(source((root / 'src/frontend/qt_sdl/main_shaders.h').read_text(), 'kScreenVS'),
            source(header, 'kScreenFS_Xbrz'))
    current = Pipeline(header=header)
    reference = Pipeline(fragment=upstream_fragment(args.reference.read_text())) if args.reference else None
    baseline = Pipeline(header=args.baseline.read_text()) if args.baseline else None
    rng = random.Random(1234)
    palette = [(0,0,0,255), (255,255,255,255), (255,0,0,255), (0,128,255,255)]
    image = bytes(v for y in range(4) for x in range(4) for v in palette[int(x >= y)])
    upload(4, 4, image + bytes([255,0,0,255])*16)
    require(render(current,4,4,4,4) == image, 'Native image changed')
    require(render(current,4,4,16,16,1) == bytes([255,0,0,255])*256, 'Layer isolation / boundary clamping')
    pixels = render(current,4,4,16,16)
    require(any(0 < v < 255 for v in pixels[::4]), 'Diagonal not smoothed')
    rotated = b''.join(pixels[((15-x)*16+y)*4:((15-x)*16+y)*4+4] for y in range(16) for x in range(16))
    require(render(current,4,4,16,16,0,1) == rotated, 'Rotation mismatch')
    comparisons = 0
    for case in range(100):
        # Include colors close to the equality threshold as well as flat areas.
        colors = palette if case < 50 else [(rng.randrange(256),rng.randrange(256),rng.randrange(256),255) for _ in range(8)]
        upload(4,4,bytes(v for _ in range(32) for v in rng.choice(colors)))
        for w,h,layer,rotate in [(16,16,0,0),(7,11,1,0),(19,9,0,1),(16,2,1,0),(4,4,0,0)]:
            actual = render(current,4,4,w,h,layer,rotate)
            if baseline:
                require(actual == render(baseline,4,4,w,h,layer,rotate),
                        f'Baseline mismatch: {case,w,h,layer,rotate}')
            if reference:
                expected = render(reference,4,4,w,h,layer,rotate)
                require(max(abs(a-b) for a,b in zip(actual,expected)) <= 1, f'Upstream mismatch: {case,w,h,layer,rotate}')
                comparisons += 1
    for sw,sh in [(512,384),(256,192)]:
        upload(sw,sh,bytes(v for pixel in palette for v in pixel) * (sw*sh*2//4))
        actual = render(current,sw,sh,sw*2,sh*2,1)
        if baseline:
            require(actual == render(baseline,sw,sh,sw*2,sh*2,1),
                    'Enhanced source dimensions differ from baseline')
        if reference:
            expected = render(reference,sw,sh,sw*2,sh*2,1)
            require(max(abs(a-b) for a,b in zip(actual,expected)) <= 1, 'Enhanced source dimensions mismatch')
    print(f'PASS: compilation, native identity, smoothing, layers, rotation, fractional/anisotropic scaling, resizing; {comparisons} upstream comparisons')
    for pattern in ('flat', 'tiles', 'noise'):
        pixels = bytes(v for y in range(192*2) for x in range(256)
                       for v in (palette[0] if pattern == 'flat' else
                                 palette[(x//8+y//8)%4] if pattern == 'tiles' else rng.choice(palette)))
        upload(256,192,pixels)
        for scale in (4,6):
            previous = benchmark(baseline,scale) if baseline else None
            optimized = benchmark(current,scale)
            comparison = f', previous {previous:.3f} ms ({previous/optimized:.2f}x)' if previous else ''
            print(f'{pattern:5s}, two screens at {scale}x: {optimized:.3f} ms GPU draws{comparison}')
    require(gl.glGetError() == 0, 'OpenGL error in benchmark')


if __name__ == '__main__':
    main()
