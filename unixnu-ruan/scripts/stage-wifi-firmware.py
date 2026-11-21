#!/usr/bin/env python3
"""Stage the WCN6750's own firmware from the user's modem image for ruan's WiFi port (macOS/docs/research/wifi/port).

The UFS variant does not need this: WpssProbe.cpp and RuanWifi.cpp read image/qca6750 of modem_a in place (ShengUfs + FatVolume.c). The RAM
root has no UFS driver, so this puts the same files on its Data volume, where the kernel looks next:
  OUT/firmware/qca6750/wpss.mbn                    the WPSS image, wpss.mdt's headers + wpss.bNN merged at their file offsets
                                                   (WpssProbe.cpp's third source, sheng_fw_open's VFS path)
  OUT/firmware/ath11k/WCN6750/hw1.0/bdwlan.bNN     the board data by board id (modem_a calls them bdwlan.eNN), board.bin (bdwlan.elf)
                                                   and regdb.bin (AsteroidsWifiFiles.c, read before modem_a)
  OUT/wifi-test/join.txt                           only with --join (the unattended join test, wifi-aj=1): holds a network key
OUT mirrors /private/var: give it to build-ramroot.sh as DATA_OVERLAY=OUT (make-ramroot.py --data-overlay; root-owned, 0600).

The files are the device's own, signed for its OEM key, and proprietary: OUT must be outside every git work tree (checked), and nothing
here is ever committed. --bdf NAME stages only that file as board.bin (e.g. bd83uin.elf, Xiaomi's India board data; icnss2.hwc=India).

usage: stage-wifi-firmware.py NON-HLOS.bin OUT [--bdf NAME] [--join FILE]
       stage-wifi-firmware.py --self-test
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def merge_mdt(headers, piece):
    """The split image as one file: piece(i) gives wpss.bNN's bytes (None when absent), placed at program header i's file offset."""
    if headers[:4] != b'\x7fELF' or headers[4] != 1 or headers[5] != 1:
        raise SystemExit('wpss.mdt: not a 32-bit little-endian ELF')
    phoff, = struct.unpack_from('<I', headers, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', headers, 0x2a)
    if phentsize != 32 or not 0 < phnum <= 64 or phoff + phnum * 32 > len(headers):
        raise SystemExit('wpss.mdt: bad program headers')
    out = bytearray()
    for i in range(phnum):
        _, offset, _, _, filesz, _, _, _ = struct.unpack_from('<8I', headers, phoff + 32 * i)
        if not filesz:
            continue
        data = piece(i)
        if data is None or len(data) < filesz:
            raise SystemExit(f'wpss.b{i:02d}: missing or shorter than its segment ({filesz} bytes)')
        if len(out) < offset + filesz:
            out.extend(bytes(offset + filesz - len(out)))
        out[offset:offset + filesz] = data[:filesz]
    if out[:len(headers)][:phoff + phnum * 32] != headers[:phoff + phnum * 32]:
        raise SystemExit('merged image: its headers differ from wpss.mdt')
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
    print('PASS: stage-wifi-firmware.py self-test')


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', type=Path, help="the modem image (NON-HLOS.bin, modem_a's FAT16 volume)")
    ap.add_argument('out', type=Path, help='the Data overlay directory (mirrors /private/var), outside any git work tree')
    ap.add_argument('--bdf', help='stage only this image/qca6750 file as board.bin (e.g. bd83uin.elf)')
    ap.add_argument('--join', type=Path, help='a local join.txt (network name, then key) for wifi-aj=1 tests; never for a release')
    args = ap.parse_args()
    if not args.image.is_file():
        raise SystemExit(f'{args.image}: no such modem image')
    out = args.out.resolve()
    probe = out
    while not probe.exists():
        probe = probe.parent
    inside = subprocess.run(['git', '-C', str(probe), 'rev-parse', '--is-inside-work-tree'], capture_output=True, text=True)
    if inside.returncode == 0 and inside.stdout.strip() == 'true':
        raise SystemExit(f'{out}: inside a git work tree; firmware stays out of git, choose another directory')
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['7z', 'x', '-y', f'-o{tmp}', str(args.image), 'image/qca6750/*'], check=True, stdout=subprocess.DEVNULL)
        src = Path(tmp) / 'image/qca6750'
        files = {p.name.lower(): p for p in src.iterdir()} if src.is_dir() else {}
        if 'wpss.mdt' not in files:
            raise SystemExit(f'{args.image}: no image/qca6750/wpss.mdt (is this the modem_a image?)')
        wpss = out / 'firmware/qca6750'
        board = out / 'firmware/ath11k/WCN6750/hw1.0'
        for d in (wpss, board):
            d.mkdir(parents=True, exist_ok=True)
        merged = merge_mdt(files['wpss.mdt'].read_bytes(),
                           lambda i: files[f'wpss.b{i:02d}'].read_bytes() if f'wpss.b{i:02d}' in files else None)
        (wpss / 'wpss.mbn').write_bytes(merged)
        staged = ['firmware/qca6750/wpss.mbn']
        if args.bdf:
            if args.bdf.lower() not in files:
                raise SystemExit(f'image/qca6750/{args.bdf}: not in the image ({" ".join(sorted(files))})')
            shutil.copyfile(files[args.bdf.lower()], board / 'board.bin')
            staged.append(f'board.bin = {args.bdf}')
        else:
            for name, path in sorted(files.items()):
                if name.startswith('bdwlan.e') and name[8:].isalnum():   # board data by id: the kernel asks for bdwlan.bNN
                    shutil.copyfile(path, board / ('bdwlan.b' + name[8:]))
                    staged.append('bdwlan.b' + name[8:])
            if 'bdwlan.elf' in files:
                shutil.copyfile(files['bdwlan.elf'], board / 'board.bin')
                staged.append('board.bin = bdwlan.elf')
        if 'regdb.bin' in files:
            shutil.copyfile(files['regdb.bin'], board / 'regdb.bin')
            staged.append('regdb.bin')
    if args.join:
        (out / 'wifi-test').mkdir(exist_ok=True)
        shutil.copyfile(args.join, out / 'wifi-test/join.txt')
        staged.append('wifi-test/join.txt (holds a network key: test image only)')
    print(f'staged in {out}: ' + ', '.join(staged))
    print(f'next: build-ramroot.sh usb with REBUILD_ROOT=1 DATA_CONTAINER=1 DATA_OVERLAY={out}')


main()
