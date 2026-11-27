#!/usr/bin/env python3
"""ruan-wifi=5's userland as a root overlay (build-ramroot.sh OVERLAY=, make-ramroot.py --overlay): the native wpa_supplicant
(drivers/Ath11kWifi's wpa_supplicant 2.12 with the -Dsheng backend, which talks to the kernel's org.unixnu.wifi.control socket) and the
program that starts it.

filipnavara/UniXNU-asteroids 9f07af3 scripts/build-wifi-userspace.py, for ruan's Linux host and iPadOS 17.7.11. asteroids merges a job into
the OS launchd cache, which needs a detached signature from a real codesign; this host has none. ruan instead takes the slot of an Apple
RunAtLoad root job: /usr/bin/IOMFB_FDR_Loader becomes scripts/RuanWifiLauncher.c, which runs the slot's previous program
(IOMFB_FDR_Loader.oslogx: oslogx's wrapper, which runs Apple's loader .real and the log export) and then keeps the supplicant running.
rbu178: the first slot, /usr/libexec/DumpPanic, is under a Protobox syscall filter on iPadOS 17 (posix_spawn/wait4 denied); this
script now puts DumpPanic back to stock (removes it and DumpPanic.real from the overlay, and the old launcher's hash). The launchd cache
and its signature stay Apple's. Our two binaries are signed ad hoc with theos ldid (4 KiB pages, com.apple.private.security.no-container)
and their CDHashes go to OUT/cdhashes, which build-ramroot.sh adds to the trust bundle as an owner module.

  overlay/usr/bin/IOMFB_FDR_Loader               the launcher           overlay/usr/bin/IOMFB_FDR_Loader.oslogx  the slot's previous program
  overlay/usr/libexec/unixnu-wpa_supplicant      the supplicant         overlay/private/etc/wifi/wpa_supplicant.conf  no networks
  overlay/private/etc/wifi/join.txt              only with a join file: the network (name, then passphrase) RuanWifi/WifiNet join
                                                 with wifi-aj=1. It lands in the flashed root image only; never in git.

usage: build-wifi-userspace.py OUT [--into OVERLAY_DIR] [--root-tree DIR] [--join FILE]
  --into DIR       add to an existing overlay (overlay/ + cdhashes, e.g. /root/unixnu-work/oslogx/overlay-dir): build-ramroot.sh takes one
  --chain FILE     the slot's previous program (default /root/unixnu-work/oslogx/IOMFB_FDR_Loader, oslogx's wrapper; never the overlay's
                   IOMFB_FDR_Loader, which is this launcher after the first run)
  --join FILE      default /root/unixnu-work/wifi-join.txt when it exists
Env: ATH11K_WIFI_DIR (default <device>/.cache/Ath11kWifi, scripts/fetch-ath11k-wifi.sh), UNIXNU_SDKROOT, LDID.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

DEVICE = Path(__file__).resolve().parents[1]
U = Path(os.environ.get('UNIXNU', os.environ.get('UNIXNU_WORK', '/root/unixnu-work') + '/UniXNU'))
SDK = os.environ.get('UNIXNU_SDKROOT', '/root/theos/sdks/iPhoneOS16.5.sdk')
LDID = os.environ.get('LDID', '/root/theos/toolchain/linux/iphone/bin/ldid')
TC = U / '.cache/ios-tc'   # build-turnip-smoke.py's clang wrappers that link with lld
SLOT = 'usr/bin/IOMFB_FDR_Loader'


def run(*cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), file=sys.stderr)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def ldid_hash(path):
    """A signed file's CDHash, or None (absent or unsigned)."""
    if not path.is_file():
        return None
    found = re.findall(r'^CDHash=([0-9a-f]{40})$', subprocess.run([LDID, '-h', str(path)], capture_output=True, text=True).stdout, re.M)
    return found[0] if len(found) == 1 else None


def sign(path, identifier, entitlements):
    run(LDID, f'-S{entitlements}', f'-I{identifier}', path)
    found = re.findall(r'^CDHash=([0-9a-f]{40})$', run(LDID, '-h', path, capture_output=True, text=True).stdout, re.M)
    assert len(found) == 1, f'{path}: one CDHash expected'
    assert path.read_bytes()[:8] == bytes.fromhex('cffaedfe0c000001'), f'{path}: not an arm64 Mach-O'
    return found[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--into', type=Path)
    ap.add_argument('--chain', type=Path, default=Path(os.environ.get('UNIXNU_WORK', '/root/unixnu-work')) / 'oslogx/IOMFB_FDR_Loader')
    ap.add_argument('--join', type=Path, default=Path(os.environ.get('UNIXNU_WORK', '/root/unixnu-work')) / 'wifi-join.txt')
    args = ap.parse_args()
    original = args.chain
    if not original.is_file() or original.read_bytes()[:4] not in (bytes.fromhex('cffaedfe'), bytes.fromhex('cafebabe')):
        raise SystemExit(f'{original}: the slot\'s previous program is missing (--chain)')
    ath = Path(os.environ.get('ATH11K_WIFI_DIR', DEVICE / '.cache/Ath11kWifi'))
    if not (ath / 'tools/build-supplicant.py').is_file():
        run('bash', DEVICE / 'scripts/fetch-ath11k-wifi.sh', ath)
    work = args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    out = (args.into or args.out).resolve()
    libexec = out / 'overlay/usr/libexec'
    libexec.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, NW_IOS_TARGET='arm64-apple-ios17.0', UNIXNU_SDKROOT=SDK, XNU_TOOLCHAIN_BIN=str(TC),
               PATH=f'{TC}:' + os.environ['PATH'])
    run(sys.executable, DEVICE / 'scripts/patch-ath11k-userspace.py', ath)   # idempotent; a tree fetched before a change gets it here
    shutil.rmtree(ath / 'out/wpa-ios', ignore_errors=True)   # hostap's make does not rebuild on a .config change
    run(sys.executable, ath / 'tools/build-supplicant.py', '--target', 'ios', env=env)
    entitlements = work / 'no-container.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))   # else iOS containers the daemon
    supplicant = libexec / 'unixnu-wpa_supplicant'
    shutil.copyfile(ath / 'out/wpa-ios/bin/wpa_supplicant', supplicant)
    run('/usr/bin/llvm-strip', '-x', supplicant)
    # rbu178: DumpPanic back to stock (the launcher there could not spawn); its hash leaves the list below
    stale = [h for h in (ldid_hash(libexec / 'DumpPanic'),) if h]
    for name in ('DumpPanic', 'DumpPanic.real'):
        (libexec / name).unlink(missing_ok=True)
    bindir = out / 'overlay/usr/bin'
    bindir.mkdir(parents=True, exist_ok=True)
    stale += [h for h in (ldid_hash(bindir / 'IOMFB_FDR_Loader'),) if h and (bindir / 'IOMFB_FDR_Loader').read_bytes() !=
              original.read_bytes()]   # a launcher of an earlier run
    launcher = bindir / 'IOMFB_FDR_Loader'
    run('/usr/bin/clang', '-target', 'arm64-apple-ios17.0', '-isysroot', SDK, '-fuse-ld=lld', '-O2', '-Wall', '-Wextra', '-Werror',
        '-framework', 'CoreFoundation', DEVICE / 'scripts/RuanWifiLauncher.c', '-o', launcher)
    run('/usr/bin/llvm-strip', '-x', launcher)
    chained = bindir / 'IOMFB_FDR_Loader.oslogx'
    shutil.copyfile(original, chained)   # its signature and trust entry are unchanged (the hash is already listed)
    for path in (supplicant, launcher, chained):
        path.chmod(0o755)
    hashes = [sign(supplicant, 'org.unixnu.wifi.supplicant', entitlements), sign(launcher, 'org.unixnu.wifi.launcher', entitlements)]
    chained_hash = ldid_hash(chained)
    if chained_hash:
        hashes.append(chained_hash)
    etc = out / 'overlay/private/etc/wifi'
    etc.mkdir(parents=True, exist_ok=True)
    (etc / 'wpa_supplicant.conf').write_text('# Networks arrive from the native Wi-Fi control channel (wifid, or the kernel join of join.txt).\n'
                                             'ap_scan=1\n')
    (etc / 'join.txt').unlink(missing_ok=True)
    if args.join.is_file():
        lines = args.join.read_bytes().splitlines()
        if len(lines) < 2 or not 0 < len(lines[0]) <= 32 or not 8 <= len(lines[1]) <= 64:
            raise SystemExit(f'{args.join}: expected the network name (1-32 bytes), then the WPA2 passphrase (8-64)')
        shutil.copyfile(args.join, etc / 'join.txt')
        (etc / 'join.txt').chmod(0o600)
    listed = [h for h in ((out / 'cdhashes').read_text().split() if (out / 'cdhashes').is_file() and args.into else []) if h not in stale]
    (out / 'cdhashes').write_text(''.join(h + '\n' for h in dict.fromkeys(listed + hashes)))   # no duplicates on a re-run
    print(f'PASS: supplicant {hashes[0]}, launcher {hashes[1]} in {out}/overlay'
          + ('; join.txt from ' + str(args.join) if args.join.is_file() else '; no join file'))


main()
