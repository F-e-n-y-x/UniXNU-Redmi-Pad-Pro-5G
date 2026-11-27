#!/usr/bin/env python3
"""The "unixnu" partition image with ruan's own root (GPU stack included), for `fastboot flash unixnu` (raw image).

Same SHNGROOT layout as `unixnu build userdata` (UniXNU scripts/unixnu/userdata.py), but the "OS" window is ruan's RAM-root
root container (make-ramroot.py output, bytes [0, split): OS + SystemOS cryptex + the AdrenoMetal/Turnip/smoke overlay) in a GPT
with one APFS partition at LBA 34, laid out like the IPSW's OS disk. "SystemOS" is the IPSW's SystemOS disk image unchanged
(ShengUfs needs both windows; ruan-ufs-systemos-union.patch mounts it over the root as on dizi).

usage: make-ufs-root.py ROOT_APFS SPLIT SYSTEMOS_DMG OUT_IMAGE
Prints the root volume UUID (board root_match_volume_uuid must name it) and the image size."""
import hashlib
import struct
import zlib
import os
import sys
import uuid
from pathlib import Path

sys.path.insert(0, os.environ.get('UNIXNU_WORK', '/root/unixnu-work') + '/UniXNU/scripts')
from unixnu import userdata   # noqa: E402  (image_size, disk_blocks: the IPSW disk image's raw disk, udif.py on Linux)

MIB = 1 << 20
root, split, systemos, out = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
assert split % 4096 == 0 and split <= root.stat().st_size

# The root volume's UUID: the first APSB in the container.
volume = None
with root.open('rb') as f:
    for at in range(0, split, 4096 * 256):
        f.seek(at)
        data = f.read(4096 * 256)
        hits = [i for i in range(0, len(data), 4096) if data[i + 32:i + 36] == b'APSB']
        if hits:
            volume = str(uuid.UUID(bytes=data[hits[0] + 240:hits[0] + 256])).upper()
            break
assert volume, 'no APFS volume superblock in the root container'

os_bytes = -(-(34 * 512 + split + 33 * 512) // 4096) * 4096      # GPT + container + backup GPT, 4 KiB multiple
sys_bytes = userdata.image_size(systemos)
os_at = MIB
sys_at = os_at + -(-os_bytes // MIB) * MIB
total = sys_at + -(-sys_bytes // MIB) * MIB

with out.open('wb') as target:
    target.truncate(total)
# The OS window: GPT disk (written by gpt() below) with the container at LBA 34.
window = out.with_suffix('.os-window')
with window.open('wb') as w:
    w.truncate(os_bytes)


def gpt(lbas, first, last):
    """Protective MBR + primary and backup GPT (512-byte sectors) with one APFS partition first..last."""
    entry = (uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC').bytes_le + uuid.uuid4().bytes_le + struct.pack('<QQQ', first, last, 0)
             + 'System'.encode('utf-16le').ljust(72, b'\0'))
    table = entry.ljust(128 * 128, b'\0')
    disk = uuid.uuid4().bytes_le

    def header(mine, other, entries_lba):
        h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, mine, other, 34, lbas - 34, disk,
                                  entries_lba, 128, 128, zlib.crc32(table)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)))
        return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, min(lbas - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr) + header(1, lbas - 1, 2) + table, table + header(lbas - 1, 1, lbas - 33)


head, tail = gpt(os_bytes // 512, 34, 34 + split // 512 - 1)
with window.open('r+b') as w:
    w.write(head)
    w.seek(os_bytes - len(tail))
    w.write(tail)
with window.open('r+b') as w, root.open('rb') as src:
    w.seek(34 * 512)
    left = split
    while left:
        chunk = src.read(min(left, 16 * MIB))
        w.write(chunk)
        left -= len(chunk)
entries = []
with out.open('r+b') as target:
    for name, offset, size, blocks in (('OS', os_at, os_bytes, window.open('rb')), ('SystemOS', sys_at, sys_bytes, None)):
        digest, done = hashlib.sha256(), 0
        target.seek(offset)
        source = iter(lambda: blocks.read(16 * MIB), b'') if blocks else userdata.disk_blocks(systemos, sys_bytes)
        for data in source:
            digest.update(data)
            target.write(data)
            done += len(data)
        assert done == size, f'{name}: {done} bytes, expected {size}'
        entries.append((name, offset, size, digest.digest()))
    header = bytearray(4096)
    header[0:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 1, len(entries), total)
    for index, (name, offset, size, digest) in enumerate(entries):
        struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
    target.seek(0)
    target.write(header)
window.unlink()
# self-check: the manifest reads back, the OS window holds a GPT whose partition starts with the container
with out.open('rb') as f:
    h = f.read(4096)
    assert h[:8] == b'SHNGROOT' and hashlib.sha256(h[:4064]).digest() == h[4064:]
    f.seek(os_at + 512); assert f.read(8) == b'EFI PART'
    f.seek(os_at + 34 * 512 + 32); assert f.read(4) == b'NXSB'
print(f'{out}: {total} bytes ({total / 2**30:.2f} GiB), OS window {os_bytes} at {os_at:#x}, SystemOS {sys_bytes} at {sys_at:#x}; '
      f'root volume {volume}')
