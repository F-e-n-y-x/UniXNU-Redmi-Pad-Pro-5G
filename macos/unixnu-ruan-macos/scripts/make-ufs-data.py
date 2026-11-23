#!/usr/bin/env python3
"""UFS-root variant: the RAM Data image sent over USB (build-ramroot.sh UFS=1). The root comes from the "unixnu" partition, so the
image is only 1 MiB of zeros (md0, unused) + the Data container (md1, served by NativeRamBlock from ram-data-split=1048576 and
mounted by ruan-ram-data-mount.patch with ram-data=1 ram-data-uuid=). Same Data tree as the RAM root (make-ramroot.py's
WORK/data + data.manifest, from its last --data-container build), with DATA_SLACK free 4 KiB blocks (default 1 GiB: the RAM root's
~2.5 GiB is free in this variant).

usage: make-ufs-data.py WORK OUT_IMAGE      writes OUT_IMAGE, OUT_IMAGE.desc (DBUSBRD1) and OUT_IMAGE.split"""
import importlib.util
import os
import struct
import sys
import zlib
from pathlib import Path

os.environ.setdefault('DATA_SLACK', str(262144))
spec = importlib.util.spec_from_file_location('make_ramroot', Path(__file__).with_name('make-ramroot.py'))
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)

work, image = Path(sys.argv[1]), Path(sys.argv[2])
tree, manifest = work / 'data', work / 'data.manifest'
if not tree.is_dir() or not manifest.is_file():
    sys.exit(f'{tree}: no Data tree; build the RAM root once (build-ramroot.sh usb with REBUILD_ROOT=1 DATA_CONTAINER=1)')
data = work / 'ruan-data.apfs'          # the fixed name make-ramroot.py uses: mkapfs.py derives the UUIDs from it
mr.run([sys.executable, '-I', mr.MKAPFS, '-o', data, '--slack', str(mr.DATA_SLACK), f'Data={tree},manifest={manifest}'])
mr.fix_spaceman(data)
if mr.apfsck(data):
    sys.exit(f'{data}: apfsck finds the Data container corrupt')
split = mr.DATA_ALIGN
crc = zlib.crc32(bytes(split))
with open(image, 'wb') as out, open(data, 'rb') as src:
    out.write(bytes(split))
    for block in iter(lambda: src.read(16 << 20), b''):
        out.write(block)
        crc = zlib.crc32(block, crc)
    pad = bytes(-out.tell() % 4096)
    out.write(pad)
    crc = zlib.crc32(pad, crc)
size = image.stat().st_size
image.with_suffix('.desc').write_bytes(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, size, 0))
image.with_suffix('.split').write_text(f'{split}\n')
assert zlib.crc32(image.read_bytes()) == crc   # the descriptor matches what the receiver will check
print(f'{image}: {size} bytes ({size >> 20} MiB), Data container at {split:#x}, crc32 {crc:08x}')
