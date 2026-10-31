#!/usr/bin/env python3
"""Test the production SPU audio-output methods in an isolated blip/mutex fixture.

Run inside melonds-gpu: python3 tools/test_audio.py [--sanitize] [--benchmark]
Use --source build/SPU-before-audio.cpp --baseline to benchmark the previous
implementation (skipping the assertion that unchanged rates are cached).
The runner extracts method bodies verbatim; no ROMs or audio device are needed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
METHODS = ('BufferAudio', 'TrimOutput', 'DrainOutput', 'InitOutput',
           'GetOutputSize', 'ReadOutput', 'SetOutputSampleRate', 'SetOutputSkew')


def extract(source, name):
    start = re.search(r'^(?:void|int) SPU::' + name + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/SPU.cpp')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text()
    with tempfile.TemporaryDirectory(prefix='melonds-audio-test-') as directory:
        temp = Path(directory)
        definition = re.search(r'^#define INTERNAL_SAMPLE_RATE .+$', source, re.M).group()
        (temp / 'audio_output_under_test.inc').write_text(
            definition + '\n' + '\n\n'.join(extract(source, name) for name in METHODS))
        binary = temp / 'test_audio'
        flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O3']
        subprocess.run(['c++', '-std=c++17', '-pthread', *flags, '-I' + str(temp),
                        str(ROOT / 'tools/test_audio.cpp'), '-o', str(binary)], check=True)
        mode = ['--baseline'] if args.baseline else ['--benchmark'] if args.benchmark else []
        subprocess.run([str(binary), *mode], check=True)
