#!/usr/bin/env python3
"""add-macos-metal.py FULL BUNDLE: put UniXNU's Adreno Metal plugin (NativeAdrenoMetal.bundle, built for ruan's macOS by
UniXNU scripts/build-adreno-metal-plugin-ruanmac.py) into make-macos-full-tree.py's root tree FULL/root.

- The bundle goes to /System/Library/Extensions (Metal's bundle search: /Library/GPUBundles, then /System/Library/Extensions;
  the WebKit GPU process and replayd sandboxes accept only the latter), owned by root.
- Its Mach-Os are built as plain arm64 (no PAC code: this LLVM's ld64.lld cannot link arm64e, and the CPU has no PAC anyway) and
  marked arm64e here (CPU subtype 0x80000002, as Apple's arm64e slices in the tree): WindowServer and the other Metal clients are
  arm64e processes, which do not load plain arm64 images. Nothing in them authenticates a pointer, so the flag changes nothing else.
- Then adhoc-resign.py re-signs them (the header is hashed) and their CDHashes join FULL/root.cdhash for the trust cache.
FULL/root.manifest gets the entries; make-macos-ufs-root.py then builds the image as usual.
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARM64E = 0x80000002
DEST = 'System/Library/Extensions/NativeAdrenoMetal.bundle'


def mark_arm64e(path):
    data = bytearray(path.read_bytes())
    magic, cputype = struct.unpack_from('<II', data, 0)
    if magic != 0xfeedfacf or cputype != 0x100000c:
        return False
    counts = fit_cpu(data)
    print(f'{path.name}: {counts}')
    assert not counts['eor3'], f'{path.name}: EOR3 (SHA3) left: build with -mcpu=cortex-a78'
    struct.pack_into('<I', data, 8, ARM64E)
    path.write_bytes(data)
    return True


NOP = 0xD503201F


def text_ranges(data):
    """(offset, size) of every __TEXT,__text section of a thin 64-bit Mach-O."""
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at, out = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 0x19:                                     # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            for i in range(nsects):
                sect = at + 72 + 80 * i
                if data[sect:sect + 16].rstrip(b'\0') == b'__text' and data[sect + 16:sect + 32].rstrip(b'\0') == b'__TEXT':
                    ssize, offset = struct.unpack_from('<QI', data, sect + 40)   # section_64: size at +40, offset at +48
                    out.append((offset, ssize))
        at += size
    return out


def fit_cpu(data):
    """ruan's Cortex-A78/A55 (ARMv8.2) lacks LRCPC2 and PAC. On the single-CPU boot (cpus=1) an acquire load or release store
    is a plain one, so LDAPUR*/STLUR* become the LDUR*/STUR* of the same size and opcode (bits 29:24 011001 -> 111000); no
    pointer is signed in this build, so XPACI Xd (RustPacBoundary.c's thunks) is a NOP. EOR3 (SHA3) has no such rewrite: the
    build must not contain it (-mcpu=cortex-a78). shortcut: valid only while the kernel runs one CPU; with SMP, rebuild instead."""
    counts = {'lrcpc2': 0, 'xpaci': 0, 'eor3': 0}
    for offset, size in text_ranges(data):
        for at in range(offset, offset + size - 3, 4):
            insn = struct.unpack_from('<I', data, at)[0]
            if insn & 0x3F200C00 == 0x19000000:
                struct.pack_into('<I', data, at, (insn & ~0x3F000000) | 0x38000000)
                counts['lrcpc2'] += 1
            elif insn & 0xFFFFFFE0 == 0xDAC143E0:
                struct.pack_into('<I', data, at, NOP)
                counts['xpaci'] += 1
            elif insn & 0xFFE08000 == 0xCE000000:
                counts['eor3'] += 1
    return counts


def main():
    full, bundle = Path(sys.argv[1]), Path(sys.argv[2])
    root, dest = full / 'root', full / 'root' / DEST
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(bundle, dest, symlinks=True)
    marked = [p for p in sorted(dest.rglob('*')) if p.is_file() and mark_arm64e(p)]
    assert len(marked) >= 2, f'expected the plugin and Turnip, marked {marked}'
    hashes_file = full / 'metal.cdhash'
    subprocess.run([sys.executable, HERE / 'adhoc-resign.py', dest, hashes_file], check=True)

    entries = {l.split('\t')[3]: l for l in (full / 'root.manifest').read_text().splitlines() if l}
    entries = {k: v for k, v in entries.items() if k != DEST and not k.startswith(DEST + '/')}
    for top, dirs, files in os.walk(dest):
        rel = Path(top).relative_to(root).as_posix()
        entries[rel] = f'40755\t0\t0\t{rel}\t'
        for name in files:
            r = f'{rel}/{name}'
            mode = 0o100755 if (root / r) in marked else 0o100644
            entries[r] = f'{mode:o}\t0\t0\t{r}\t'
    (full / 'root.manifest').write_text('\n'.join(entries[p] for p in sorted(entries)) + '\n')
    hashes = (full / 'root.cdhash').read_text().split()
    new = [h for h in hashes_file.read_text().split() if h not in hashes]
    (full / 'root.cdhash').write_text('\n'.join(hashes + new) + '\n')
    print(f'{DEST}: {len(marked)} Mach-Os marked arm64e and re-signed, {len(new)} CDHashes added')


if __name__ == '__main__':
    main()
