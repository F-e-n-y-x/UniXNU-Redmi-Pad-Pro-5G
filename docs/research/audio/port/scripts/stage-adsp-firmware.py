#!/usr/bin/env python3
"""Stage the ADSP image from the user's modem image for ruan's audio port (macOS/docs/research/audio/port), RAM root only.

The UFS variant does not need this: RuanAdsp.cpp reads image/adsp.mdt + adsp.bNN of modem_a in place ("modem:adsp"). The RAM root has no
UFS driver, so this writes OUT/firmware/qcom/sm7435/ruan/adsp.mbn (adsp.mdt's headers + adsp.bNN merged at their file offsets), the
second source RuanAdsp.cpp tries (/private/var/firmware/qcom/sm7435/ruan/adsp.mbn). OUT mirrors /private/var: give it to
build-ramroot.sh as DATA_OVERLAY=OUT. The image is the device's own, signed and proprietary: OUT must be outside every git work tree.
merge_mdt and the self-test are ../../wifi/port/scripts/stage-wifi-firmware.py's.

usage: stage-adsp-firmware.py NON-HLOS.bin OUT
       stage-adsp-firmware.py --self-test
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def merge_mdt(headers, piece):
    """The split image as one file: piece(i) gives wpss.bNN's bytes (None when absent), placed at program header i's file offset."""
    if headers[:4] != b'\x7fELF' or headers[4] != 1 or headers[5] != 1:
        raise SystemExit('adsp.mdt: not a 32-bit little-endian ELF')
    phoff, = struct.unpack_from('<I', headers, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', headers, 0x2a)
    if phentsize != 32 or not 0 < phnum <= 64 or phoff + phnum * 32 > len(headers):
        raise SystemExit('adsp.mdt: bad program headers')
    out = bytearray()
    for i in range(phnum):
        _, offset, _, _, filesz, _, _, _ = struct.unpack_from('<8I', headers, phoff + 32 * i)
        if not filesz:
            continue
        data = piece(i)
        if data is None or len(data) < filesz:
            raise SystemExit(f'adsp.b{i:02d}: missing or shorter than its segment ({filesz} bytes)')
        if len(out) < offset + filesz:
            out.extend(bytes(offset + filesz - len(out)))
        out[offset:offset + filesz] = data[:filesz]
    if out[:len(headers)][:phoff + phnum * 32] != headers[:phoff + phnum * 32]:
        raise SystemExit('merged image: its headers differ from adsp.mdt')
    return bytes(out)


def self_test():
    # Two segments: 0 = the ELF and program headers (as b00), 1 = 16 bytes at 0x200, 2 = empty (no file, as ruan's absent b11).
    phoff, n = 0x34, 3
    head = bytearray(phoff + 32 * n)
    head[:6] = b'\x7fELF\x01\x01'
    struct.pack_into('<I', head, 0x1c, phoff)
    struct.pack_into('<HH', head, 0x2a, 32, n)
    struct.pack_into('<8I', head, phoff, 6, 0, 0, 0, len(head), len(head), 0, 0)
    struct.pack_into('<8I', head, phoff + 32, 1, 0x200, 0, 0, 16, 16, 0, 0)
    struct.pack_into('<8I', head, phoff + 64, 1, 0x210, 0, 0, 0, 0x100, 0, 0)
    body = bytes(range(16))
    merged = merge_mdt(bytes(head), {0: bytes(head), 1: body}.get)
    assert merged[:len(head)] == head and merged[0x200:0x210] == body and len(merged) == 0x210
    try:
        merge_mdt(bytes(head), {0: bytes(head)}.get)
        raise AssertionError('a missing segment file was accepted')
    except SystemExit:
        pass
    print('PASS: stage-adsp-firmware.py self-test')


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    image, out = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
    if not image.is_file():
        raise SystemExit(f'{image}: no such modem image')
    probe = out
    while not probe.exists():
        probe = probe.parent
    inside = subprocess.run(['git', '-C', str(probe), 'rev-parse', '--is-inside-work-tree'], capture_output=True, text=True)
    if inside.returncode == 0 and inside.stdout.strip() == 'true':
        raise SystemExit(f'{out}: inside a git work tree; firmware stays out of git, choose another directory')
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['7z', 'x', '-y', f'-o{tmp}', str(image), 'image/adsp.*'], check=True, stdout=subprocess.DEVNULL)
        files = {p.name.lower(): p for p in (Path(tmp) / 'image').iterdir()} if (Path(tmp) / 'image').is_dir() else {}
        if 'adsp.mdt' not in files:
            raise SystemExit(f'{image}: no image/adsp.mdt (is this the modem_a image?)')
        merged = merge_mdt(files['adsp.mdt'].read_bytes(),
                           lambda i: files[f'adsp.b{i:02d}'].read_bytes() if f'adsp.b{i:02d}' in files else None)
    dest = out / 'firmware/qcom/sm7435/ruan'
    dest.mkdir(parents=True, exist_ok=True)
    (dest / 'adsp.mbn').write_bytes(merged)
    print(f'staged {dest / "adsp.mbn"} ({len(merged)} bytes)')
    print(f'next: build-ramroot.sh usb with REBUILD_ROOT=1 DATA_CONTAINER=1 DATA_OVERLAY={out}')


main()
