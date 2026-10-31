#!/usr/bin/env python3
"""Create synthetic ZIP fixtures and run the compiled archive regression test.

Build from the repository root (inside melonds-gpu if needed):
    c++ -std=c++17 -O2 -fPIC -Isrc tools/test_archive.cpp \
        src/frontend/qt_sdl/ArchiveUtil.cpp \
        $(pkg-config --cflags --libs Qt6Core libarchive) -o build/test_archive
Then run:
    python3 tools/test_archive.py build/test_archive
No game ROMs or third-party Python packages are required.
"""
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zipfile


def fixtures(root):
    payload = bytes(range(256)) * 1025
    for name, compression in [('stored.zip', zipfile.ZIP_STORED),
                              ('deflated.zip', zipfile.ZIP_DEFLATED),
                              ('café.zip', zipfile.ZIP_DEFLATED)]:
        with zipfile.ZipFile(root / name, 'w', compression=compression) as archive:
            archive.writestr('README.txt', 'Synthetic ROM fixtures')
            archive.writestr('games/', b'')
            archive.writestr('games/demo.nds', payload)
            archive.writestr('games/café.gba', b'GBA test')
    with zipfile.ZipFile(root / 'empty.zip', 'w'):
        pass
    with zipfile.ZipFile(root / 'empty-entry.zip', 'w') as archive:
        archive.writestr('empty.nds', b'')
    (root / 'invalid.zip').write_bytes(b'not an archive')
    stored = (root / 'stored.zip').read_bytes()
    corrupt = bytearray(stored)
    corrupt[stored.index(payload) + 1000] ^= 0x80
    (root / 'corrupt.zip').write_bytes(corrupt)
    (root / 'truncated.zip').write_bytes(stored[:stored.index(payload) + 1000])
    # Advertise a huge ROM without allocating one in the fixture or loader.
    small = io.BytesIO()
    with zipfile.ZipFile(small, 'w') as archive:
        archive.writestr('games/demo.nds', b'small')
    oversized = bytearray(small.getvalue())
    central = oversized.index(b'PK\x01\x02')
    struct.pack_into('<I', oversized, 22, 0x40000001)
    struct.pack_into('<I', oversized, central + 24, 0x40000001)
    (root / 'oversize.zip').write_bytes(oversized)
    encrypted = bytearray(small.getvalue())
    struct.pack_into('<H', encrypted, 6, 1)
    struct.pack_into('<H', encrypted, central + 8, 1)
    (root / 'encrypted.zip').write_bytes(encrypted)


if __name__ == '__main__':
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/test_archive').resolve()
    with tempfile.TemporaryDirectory(prefix='melonds-archive-test-') as directory:
        root = Path(directory)
        fixtures(root)
        subprocess.run([str(binary), str(root)], check=True)
