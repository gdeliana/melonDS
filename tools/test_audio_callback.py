#!/usr/bin/env python3
"""Check the production SDL callback in a bounded deterministic audio fixture."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = re.search(r'^(?:void|int) EmuInstance::' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    text = (ROOT / 'src/frontend/qt_sdl/EmuInstanceAudio.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='melonds-callback-test-') as directory:
        temp = Path(directory)
        (temp / 'callback.inc').write_text(re.search(r'^#define INTERNAL_FRAME_RATE .+$', text, re.M).group() + '\n' +
            '\n'.join(method(text, name) for name in ('audioGetNumSamplesOut', 'audioCallback')))
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
        binary = temp / 'test_callback'
        subprocess.run(['c++', '-std=c++17', *flags, '-I'+str(temp), str(ROOT/'tools/test_audio_callback.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
