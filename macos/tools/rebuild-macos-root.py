#!/usr/bin/env python3
"""rebuild-macos-root.py EXTRACT: rebuild the macOS RAM root with ad-hoc signed binaries (boot 5's AMFI kill).

1. Copy EXTRACT/recovery-tree to EXTRACT/recovery-tree-signed and ad-hoc re-sign every Mach-O there (adhoc-resign.py);
   the CDHashes go to EXTRACT/recovery-tree-signed.cdhash.
2. Write the System volume with HakiOS's mkapfs (same options as the iPadOS make-ramroot.py) from that tree and
   EXTRACT/recovery.manifest -> EXTRACT/recovery-root.apfs.
3. root-usb.apfs = recovery-root.apfs, zero pad to 1 MiB, data-usb.apfs (unchanged); new root-usb.split and root-usb.desc.
4. trust.bundle gets one more module with the CDHashes (trust-append-module.py), on top of trust.bundle.orig.
Then run build-macos-ramroot.sh with SKIP_KERNEL=1 to make the boot image that carries the new descriptor and bundle.
"""
import importlib.util
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

W = Path('/root/unixnu-work')
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('make_ramroot', W / 'UniXNU-ruan/scripts/make-ramroot.py')
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    x = Path(sys.argv[1])
    src, tree, cdhash = x / 'recovery-tree', x / 'recovery-tree-signed', x / 'recovery-tree-signed.cdhash'
    if tree.exists():
        shutil.rmtree(tree)
    run('cp', '-a', src, tree)
    run(sys.executable, HERE / 'adhoc-resign.py', tree, cdhash)

    root = x / 'recovery-root.apfs'
    plain = [a for d in ('sbin', 'usr/lib', 'usr/libexec') for a in ('--plain', d)]
    run(sys.executable, '-I', '-c', mr.MKAPFS_APPLE_RSRC, mr.MKAPFS, '-o', root, '-z', *plain, '--slack', 16384,
        f'System={tree},manifest={x / "recovery.manifest"}')
    mr.fix_spaceman(root)

    out = x / 'root-usb.apfs'
    size = root.stat().st_size
    split = -(-size // mr.DATA_ALIGN) * mr.DATA_ALIGN
    with open(out, 'wb') as o:
        with open(root, 'rb') as r:
            shutil.copyfileobj(r, o, 16 << 20)
        o.write(bytes(split - size))
        with open(x / 'data-usb.apfs', 'rb') as dsrc:
            shutil.copyfileobj(dsrc, o, 16 << 20)
        o.write(bytes(-o.tell() % 4096))
    total, crc = out.stat().st_size, 0
    with open(out, 'rb') as s:
        for block in iter(lambda: s.read(16 << 20), b''):
            crc = zlib.crc32(block, crc)
    (x / 'root-usb.split').write_text(f'{split}\n')
    (x / 'root-usb.desc').write_bytes(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, total, 0))

    orig = x / 'trust.bundle.orig'
    if not orig.exists():
        shutil.copy2(x / 'trust.bundle', orig)
    run(sys.executable, W / 'UniXNU/scripts/trust-append-module.py', orig, x / 'trust.bundle', *cdhash.read_text().split())

    # read back: launchd must come out of the new image byte for byte
    got = subprocess.run([sys.executable, '-I', str(mr.APFS), 'cat', str(root), '/sbin/launchd'], check=True,
                         stdout=subprocess.PIPE).stdout
    assert got == (tree / 'sbin/launchd').read_bytes(), 'launchd read back differs'
    print(f'{out}: {total} bytes, split {split}, crc32 {crc:08x}')


if __name__ == '__main__':
    main()
