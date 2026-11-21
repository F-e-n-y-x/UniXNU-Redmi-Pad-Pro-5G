#!/usr/bin/env python3
"""A pulled Metal cache directory (e.g. caches-rbuNN/private/var/mobile/Library/Caches/com.apple.backboardd/m2v) -> a pipeline pack
(SHEXP001, UniXNU scripts/make-m2vpack.py's format) for build-metal-plugin.py PIPELINE_PACK=. Paths are relative to the directory: the
plugin imports them into every Metal process's own cache directory (CacheExport.h importPack). Only useful with the same metal2vulkan
runtime and Turnip build (LC_UUID) that wrote the cache; other entries are ignored as cold.

usage: make-ruan-m2vpack.py CACHE_DIR OUT"""
import struct
import sys
import zlib
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
files = sorted(p for p in src.rglob('*') if p.is_file())
root = str(src).encode()
stream = bytearray(b'M2VCACH1' + struct.pack('<I', len(root)) + root + struct.pack('<I', len(files)))
for path in files:
    name, data = path.relative_to(src).as_posix().encode(), path.read_bytes()
    stream += struct.pack('<IIIIQ', len(name), 0o100600, 501, 501, len(data)) + name + data
z = zlib.compress(bytes(stream), 6)
out.write_bytes(b'SHEXP001' + struct.pack('<II', len(stream), len(z)) + z)
# self-check: the pack reads back to the same stream
blob = out.read_bytes()
assert blob[:8] == b'SHEXP001' and zlib.decompress(blob[16:]) == stream and struct.unpack('<II', blob[8:16]) == (len(stream), len(z))
print(f'{out}: {len(files)} files, {len(stream)} bytes, {len(z)} compressed')
