#!/usr/bin/env python3
"""Profile actual channel interpolation bodies and blip resampling in a fixture.
Sample fetching is synthetic; this is not a whole-emulator audio profile.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]


def body(source, start):
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


if __name__ == '__main__':
    source = (ROOT/'src/SPU.cpp').read_text()
    header = (ROOT/'src/SPU.h').read_text()
    with tempfile.TemporaryDirectory(prefix='melonds-audio-profile-') as directory:
        temp = Path(directory)
        constants = source[source.index('template <typename T>'):source.index('SPU::SPU(')]
        run = body(source, source.index('template<u32 type>\ns32 SPUChannel::Run'))
        dispatch = body(header, header.index('    s32 DoRun('))
        pan = body(source, source.index('void SPUChannel::PanOutput('))
        loop = body(source, source.index('        for (int i = 4; i < 16; i++)'))
        mix = 'static s32 MixOptimized(std::array<SPUChannel,16>& Channels) { s32 left=0, right=0; const u32 spucycles=512;\n' + loop + '\nreturn left ^ right; }'
        (temp/'interpolation.inc').write_text(constants+'\n'+run+'\n'+pan+'\n'+mix)
        (temp/'dispatch.inc').write_text(dispatch)
        binary = temp/'profile_audio'
        subprocess.run(['c++','-std=c++17','-O3','-Isrc','-I'+str(temp), str(ROOT/'tools/profile_audio.cpp'),
            str(ROOT/'src/blip-buf/blip_buf.c'),'-o',str(binary)],cwd=ROOT,check=True)
        subprocess.run([str(binary)],check=True)
