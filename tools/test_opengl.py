#!/usr/bin/env python3
"""Run real EGL/OpenGL regression checks for uploads, capture and sampling.

Inside melonds-gpu: python3 tools/test_opengl.py
Extracts the production capture methods and shaders verbatim into an isolated
GPU/VRAM fixture. No ROM or Qt application is required.
"""
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, name, prefix='void GLRenderer::'):
    start = source.index(prefix + name + '(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    source = (ROOT / 'src/GPU_OpenGL.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='melonds-opengl-test-') as directory:
        temp = Path(directory)
        code = '\n'.join(method(source, name) for name in ('DownscaleCapture', 'SyncVRAMCapture'))
        stamp = method((ROOT / 'src/GPU.cpp').read_text(), 'InvalidateFramebuffer', 'void GPU::')
        code += '\n' + stamp.replace('GPU::', 'GPUStamp::', 1)
        code += '\n' + method((ROOT / 'src/frontend/qt_sdl/Screen.cpp').read_text(), 'prepareXbrz', 'bool ScreenPanelGL::')
        for name in ('CaptureDownscaleVS', 'CaptureDownscaleFS'):
            shader = (ROOT / 'src/OpenGL_shaders' / (name + '.glsl')).read_text()
            code += '\nconst char* ' + name + ' = R"GLSL(' + shader + ')GLSL";\n'
        (temp / 'capture_under_test.inc').write_text(code)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'egl', 'gl'], text=True))
        binary = temp / 'test_opengl'
        subprocess.run(['c++', '-std=c++17', '-O2', '-I' + str(temp), str(ROOT / 'tools/test_opengl.cpp'),
                        *flags, '-o', str(binary)], check=True)
        subprocess.run([str(binary), '--gl32'], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([str(binary), '--gl43'], check=True)
