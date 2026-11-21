#!/usr/bin/env python3
"""ruan-bt=4's userland as a root overlay (build-ramroot.sh OVERLAY=, make-ramroot.py --overlay), next to build-wifi-userspace.py's:

  overlay/usr/sbin/BlueTool    QcaBluetooth userland/BlueTool.c (com.apple.BlueTool XPC service; boot/init -> kern.qbt_power_on), arm64 iOS 17
  overlay/usr/sbin/bluetoothd  21H461's own daemon routed to a type-0 controller (scripts/patch-bluetoothd.py), original identifier and
                               entitlements

Both replace the originals at their own paths, so Apple's launchd jobs (com.apple.BlueTool, com.apple.bluetoothd) and the signed launchd
cache stay unchanged. Both are signed ad hoc with theos ldid; their CDHashes go to OUT/cdhashes, which build-ramroot.sh adds to the trust
bundle as an owner module (as the Wi-Fi binaries). If AMFI refuses a replaced job with a launch-constraint violation, boot with no-lc=1
(UniXNU patches/xnu zzyc-owner-skip-launch-constraints), as asteroids does.

usage: build-bluetooth-userspace.py OUT [--into OVERLAY_DIR] [--root-tree DIR]
  --into DIR       add to an existing overlay (overlay/ + cdhashes), e.g. the one build-wifi-userspace.py --into filled: build-ramroot.sh takes one
  --root-tree DIR  make-ramroot.py's extracted 21H461 root (default /root/unixnu-work/ramroot/work-full/root), for the original bluetoothd
Env: QCA_BLUETOOTH_DIR (default <device>/.cache/QcaBluetooth, scripts/fetch-qca-bluetooth.sh), UNIXNU_SDKROOT, LDID.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys

DEVICE = Path(__file__).resolve().parents[1]
SDK = os.environ.get('UNIXNU_SDKROOT', '/root/theos/sdks/iPhoneOS16.5.sdk')
LDID = os.environ.get('LDID', '/root/theos/toolchain/linux/iphone/bin/ldid')


def run(*cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), file=sys.stderr)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--into', type=Path)
    ap.add_argument('--root-tree', type=Path, default=Path('/root/unixnu-work/ramroot/work-full/root'))
    args = ap.parse_args()
    original = args.root_tree / 'usr/sbin/bluetoothd'
    if not original.is_file():
        raise SystemExit(f'{original}: the original bluetoothd is missing (--root-tree)')
    qca = Path(os.environ.get('QCA_BLUETOOTH_DIR', DEVICE / '.cache/QcaBluetooth'))
    if not (qca / 'userland/BlueTool.c').is_file():
        run('bash', DEVICE / 'scripts/fetch-qca-bluetooth.sh', qca)
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    out = (args.into or args.out).resolve()
    sbin = out / 'overlay/usr/sbin'
    sbin.mkdir(parents=True, exist_ok=True)

    bluetool = sbin / 'BlueTool'
    run('/usr/bin/clang', '-target', 'arm64-apple-ios17.0', '-isysroot', SDK, '-fuse-ld=lld', '-O2', '-Wall', '-Wextra', '-Werror',
        '-fblocks', '-I', DEVICE / 'userland/xpc-shim', qca / 'userland/BlueTool.c', '-o', bluetool)   # the SDK has no <xpc/xpc.h>
    run('/usr/bin/llvm-strip', '-x', bluetool)
    entitlements = work / 'no-container.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    run(LDID, f'-S{entitlements}', '-Icom.apple.BlueTool', bluetool)
    bluetool.chmod(0o755)

    daemon = sbin / 'bluetoothd'
    patched = run(sys.executable, '-I', Path(__file__).with_name('patch-bluetoothd.py'), original, daemon, '--sign',
                  capture_output=True, text=True).stdout
    sys.stderr.write(patched)

    hashes = []
    for path in (bluetool, daemon):
        found = re.findall(r'^CDHash=([0-9a-f]{40})$', run(LDID, '-h', path, capture_output=True, text=True).stdout, re.M)
        assert len(found) == 1, f'{path}: one CDHash expected'
        assert path.read_bytes()[:8] == bytes.fromhex('cffaedfe0c000001'), f'{path}: not an arm64 Mach-O'
        hashes.append(found[0])
    listed = (out / 'cdhashes').read_text().split() if (out / 'cdhashes').is_file() and args.into else []
    (out / 'cdhashes').write_text(''.join(h + '\n' for h in dict.fromkeys(listed + hashes)))   # no duplicates on a re-run
    print(f'PASS: BlueTool {hashes[0]}, bluetoothd {hashes[1]} in {out}/overlay')


main()
