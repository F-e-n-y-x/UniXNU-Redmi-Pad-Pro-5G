#!/usr/bin/env python3
"""Stage the iris2 VPU firmware (vpu20_1v, PAS 9) for ruan's video port (macOS/docs/research/video/port).

Where it lives on the device (checked 2026-11-21 on OS3.0.305.0.WFSINXM): only on the **vendor** partition (EROFS, inside super), as
/vendor/firmware/vpu20_1v.mbn (2024040 bytes, merged and signed) and as the split vpu20_1v.mdt + .b00-.b19. modem_a (NON-HLOS.bin)
has no video firmware. The kernel cannot read vendor (dynamic partition, EROFS), so this puts the merged image on the Data volume:
  OUT/firmware/qcom/vpu20_1v.mbn   RuanVideo.cpp's default ruan-video-fw (/private/var/firmware/qcom/vpu20_1v.mbn)
OUT mirrors /private/var: give it to build-ramroot.sh as DATA_OVERLAY=OUT, or copy the file to that path on the UFS root.

SOURCE is any of: the .mbn itself; the split .mdt (its .bNN next to it are merged); a directory holding firmware/vpu20_1v.* or
vpu20_1v.* (a mounted or extracted vendor); a vendor EROFS image (extracted with fsck.erofs --extract when installed, otherwise
loop-mounted read-only, which needs root, and unmounted again).

The file is the device's own, signed for its OEM key, and proprietary: OUT must be outside every git work tree (checked), and nothing
here is ever committed.

usage: stage-video-firmware.py SOURCE OUT
       stage-video-firmware.py --self-test
"""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

NAME = 'vpu20_1v'
CARVEOUT = 0x700000            # video_region@86c00000
KNOWN = {'307751943986a12a202829b55242b62497b81947c265076eb5ed6e4d9ede89cf': 'OS3.0.305.0.WFSINXM vendor/firmware/vpu20_1v.mbn'}


def segments(elf):
    """The program headers of a 32-bit little-endian ELF: (type, offset, paddr, filesz, memsz, flags)."""
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1:
        raise SystemExit(f'{NAME}: not a 32-bit little-endian ELF')
    phoff, = struct.unpack_from('<I', elf, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2a)
    if phentsize != 32 or not 0 < phnum <= 64 or phoff + phnum * 32 > len(elf):
        raise SystemExit(f'{NAME}: bad program headers')
    out = []
    for i in range(phnum):
        t, off, _, pa, fs, ms, fl, _ = struct.unpack_from('<8I', elf, phoff + 32 * i)
        out.append((t, off, pa, fs, ms, fl))
    return out


def merge_mdt(headers, piece):
    """The split image as one file: piece(i) gives .bNN's bytes (None when absent), placed at program header i's file offset."""
    out = bytearray()
    for i, (_, offset, _, filesz, _, _) in enumerate(segments(headers)):
        if not filesz:
            continue
        data = piece(i)
        if data is None or len(data) < filesz:
            raise SystemExit(f'{NAME}.b{i:02d}: missing or shorter than its segment ({filesz} bytes)')
        if len(out) < offset + filesz:
            out.extend(bytes(offset + filesz - len(out)))
        out[offset:offset + filesz] = data[:filesz]
    return bytes(out)


def check(image):
    """What RuanVideo.cpp will check, plus the hash segment PAS needs; returns a one-line summary."""
    segs = segments(image)
    loads = [(pa, ms) for t, _, pa, _, ms, fl in segs if t == 1 and ms and (fl >> 24) & 7 != 2]
    if not any((fl >> 24) & 7 == 2 for _, _, _, _, _, fl in segs):
        raise SystemExit(f'{NAME}: no hash segment (not a signed image)')
    if not loads:
        raise SystemExit(f'{NAME}: no load segment')
    span = max(pa + ms for pa, ms in loads) - min(pa for pa, _ in loads)
    if span > CARVEOUT:
        raise SystemExit(f'{NAME}: span {span:#x} exceeds the 0x700000 carveout')
    for _, off, _, fs, _, _ in segs:
        if off + fs > len(image):
            raise SystemExit(f'{NAME}: a segment lies past the end of the file')
    gen2 = b'HFI_CMD_SUBSCRIBE_MODE' in image   # an HFI gen 2 firmware logs this command name
    return f'{len(segs)} segments, span {span:#x}, HFI gen {2 if gen2 else "1?"}'


def from_dir(d):
    for base in (d / 'firmware', d / 'vendor/firmware', d):
        if (base / f'{NAME}.mbn').is_file():
            return (base / f'{NAME}.mbn').read_bytes()
        if (base / f'{NAME}.mdt').is_file():
            return from_mdt(base / f'{NAME}.mdt')
    return None


def from_mdt(mdt):
    part = lambda i: (mdt.with_name(f'{NAME}.b{i:02d}').read_bytes() if mdt.with_name(f'{NAME}.b{i:02d}').is_file() else None)
    return merge_mdt(mdt.read_bytes(), part)


def from_erofs(img):
    with tempfile.TemporaryDirectory() as tmp:
        if shutil.which('fsck.erofs'):
            subprocess.run(['fsck.erofs', f'--extract={tmp}/x', str(img)], check=True, stdout=subprocess.DEVNULL)
            return from_dir(Path(tmp) / 'x')
        mnt = Path(tmp) / 'm'
        mnt.mkdir()
        if subprocess.run(['mount', '-t', 'erofs', '-o', 'loop,ro', str(img), str(mnt)]).returncode:
            raise SystemExit(f'{img}: cannot read EROFS (install erofs-utils, or run as root for a read-only loop mount)')
        try:
            return from_dir(mnt)
        finally:
            subprocess.run(['umount', str(mnt)], check=True)


def load(src):
    if src.is_dir():
        data = from_dir(src)
    elif src.suffix == '.mdt':
        data = from_mdt(src)
    elif open(src, 'rb').read(4) == b'\x7fELF':
        data = src.read_bytes()
    elif open(src, 'rb').read(0x404)[0x400:0x404] == b'\xe2\xe1\xf5\xe0':   # EROFS superblock magic at 1024
        data = from_erofs(src)
    else:
        raise SystemExit(f'{src}: not an ELF, a .mdt, a directory or an EROFS vendor image')
    if data is None:
        raise SystemExit(f'{src}: no {NAME}.mbn or {NAME}.mdt found')
    return data


def outside_git(out):
    probe = out
    while not probe.exists():
        probe = probe.parent
    inside = subprocess.run(['git', '-C', str(probe), 'rev-parse', '--is-inside-work-tree'], capture_output=True, text=True)
    if inside.returncode == 0 and inside.stdout.strip() == 'true':
        raise SystemExit(f'{out}: inside a git work tree; firmware stays out of git, choose another directory')


def self_test():
    # ELF + program headers as b00, one 16-byte load segment at 0x200 (b01), a hash segment (b02), an empty one (no file)
    phoff, n = 0x34, 4
    head = bytearray(phoff + 32 * n)
    head[:6] = b'\x7fELF\x01\x01'
    struct.pack_into('<I', head, 0x1c, phoff)
    struct.pack_into('<HH', head, 0x2a, 32, n)
    struct.pack_into('<8I', head, phoff, 0, 0, 0, 0, len(head), 0, 7 << 24, 0)
    struct.pack_into('<8I', head, phoff + 32, 1, 0x200, 0, 0xf500000, 16, 16, 0x8000005, 0)
    struct.pack_into('<8I', head, phoff + 64, 0, 0x210, 0, 0, 8, 8, 2 << 24, 0)
    struct.pack_into('<8I', head, phoff + 96, 1, 0x218, 0, 0xf500000 + 0x800000, 0, 0x1000, 0x8000006, 0)
    body = bytes(range(16))
    merged = merge_mdt(bytes(head), {0: bytes(head), 1: body, 2: b'H' * 8}.get)
    assert merged[0x200:0x210] == body and len(merged) == 0x218
    try:
        check(merged)   # the last segment ends 0x801000 past the first: over the carveout
        raise AssertionError('an oversized span was accepted')
    except SystemExit:
        pass
    struct.pack_into('<8I', head, phoff + 96, 1, 0x218, 0, 0xf500000 + 0x1000, 0, 0x1000, 0x8000006, 0)
    merged = merge_mdt(bytes(head), {0: bytes(head), 1: body, 2: b'H' * 8}.get)
    assert 'span 0x2000' in check(merged)
    try:
        merge_mdt(bytes(head), {0: bytes(head)}.get)
        raise AssertionError('a missing segment file was accepted')
    except SystemExit:
        pass
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(['git', 'init', '-q', tmp], check=True)
        try:
            outside_git(Path(tmp) / 'a/b')
            raise AssertionError('a directory inside git was accepted')
        except SystemExit:
            pass
    print('PASS: stage-video-firmware.py self-test')


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
    outside_git(out)
    data = load(src)
    summary = check(data)
    digest = hashlib.sha256(data).hexdigest()
    target = out / 'firmware/qcom' / f'{NAME}.mbn'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(0o600)
    print(f'staged {target} ({len(data)} bytes, {summary}) sha256 {digest} {KNOWN.get(digest, "(a build not seen before)")}')
    print(f'next: build-ramroot.sh with REBUILD_ROOT=1 DATA_CONTAINER=1 DATA_OVERLAY={out}, and ruan-video=N in the kernel arguments')


main()
