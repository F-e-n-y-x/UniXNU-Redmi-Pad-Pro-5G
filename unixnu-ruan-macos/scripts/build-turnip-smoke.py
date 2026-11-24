#!/usr/bin/env python3
"""GPU step 4 (macOS/docs/research/GPU-METAL-PORT.md): Turnip for iOS 17 with the FD710 backport and ruan's smoke tool
(package/RuanTurnipSmoke.cpp), built on this Linux host and staged as a root overlay for make-ramroot.py --overlay.

usage: build-turnip-smoke.py [OUT]   (default OUT = UniXNU build/ruan-turnip-smoke)

OUT/overlay/usr/bin/IOMFB_FDR_Loader       the smoke tool in place of the display FDR loader (a RunAtLoad root job of the
                                           original launchd.plist; launchd.plist and its .sig stay Apple's)
OUT/overlay/usr/bin/libvulkan_freedreno.dylib  Turnip beside it (NativeTurnipSmoke.h turnip::libraryPath)
OUT/cdhashes                               their CDHashes, one per line: build-ramroot.sh appends them to trust.bundle

Same steps as UniXNU scripts/build-adreno-metal-plugin.py for its probe daemon, with Linux tools: clang + lld against
$UNIXNU_SDKROOT (default the theos iPhoneOS16.5 SDK), llvm-strip, and theos ldid for the ad-hoc signatures (4 KiB pages).
The Metal plugin itself (step 5) needs the Rust metal2vulkan bridge and is not built here.
"""
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

DEVICE = Path(__file__).resolve().parents[1]
U = Path(os.environ.get('UNIXNU', '/root/unixnu-work/UniXNU'))
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else U / 'build/ruan-turnip-smoke'
SDK = os.environ.get('UNIXNU_SDKROOT', '/root/theos/sdks/iPhoneOS16.5.sdk')
LDID = os.environ.get('LDID', '/root/theos/toolchain/linux/iphone/bin/ldid')
TOOLS = U / '.cache/mesa-tools'          # glslang, glslc, spirv-val (Debian packages unpacked here)
TC = U / '.cache/ios-tc'                 # clang wrappers that link with lld, for xcrun -find (UniXNU's xcrun shim)


def run(*cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), file=sys.stderr)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


TC.mkdir(parents=True, exist_ok=True)
for name in ('clang', 'clang++'):
    (TC / name).write_text('#!/bin/sh\n# link-only: -fuse-ld on compile steps trips meson -Werror=unused-command-line-argument probes\n'
                           'for a; do case "$a" in -c|-E|-S) exec /usr/bin/%s "$@";; esac; done\nexec /usr/bin/%s -fuse-ld=lld "$@"\n'
                           % (name, name))
    (TC / name).chmod(0o755)
for name, target in (('ar', '/usr/bin/llvm-ar'), ('strip', '/usr/bin/llvm-strip')):
    if not (TC / name).is_symlink():
        (TC / name).symlink_to(target)
env = dict(os.environ, XNU_TOOLCHAIN_BIN=str(TC), UNIXNU_SDKROOT=SDK, PATH=f'{TOOLS}/bin:{TC}:' + os.environ['PATH'],
           LD_LIBRARY_PATH=f'{TOOLS}/usr/lib/x86_64-linux-gnu')
run(sys.executable, U / 'scripts/build-turnip-darwin.py', '--target', 'ios', cwd=U, env=env)
run(sys.executable, U / 'scripts/gen-turnip-smoke-shaders.py', cwd=U, env=env)
(U / 'build/turnip-smoke/TurnipReplay.h').unlink(missing_ok=True)   # the host-simulation replay is optional (plugin only)

shutil.rmtree(OUT, ignore_errors=True)
bin_dir = OUT / 'overlay/usr/bin'
bin_dir.mkdir(parents=True)
# The smoke headers from UniXNU, with ruan's diagnostic patches (patches/smoke: the read-back words for the D line), in a private copy.
src = OUT / 'src'
src.mkdir()
for name in ('NativeAdrenoUserTest.h', 'NativeTurnipSmoke.h'):
    shutil.copyfile(U / 'drivers/AdrenoMetal' / name, src / name)
for patch in sorted((DEVICE / 'patches/smoke').glob('*.patch')):
    run('patch', '-p3', '-s', '--forward', '-d', src, '-i', patch)
flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', SDK, '-O2', '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong']
pm4 = OUT / 'AdrenoPM4.o'
run('/usr/bin/clang', *flags, '-std=c11', '-c', U / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', pm4)
tool = bin_dir / 'IOMFB_FDR_Loader'
run('/usr/bin/clang++', *flags, '-fuse-ld=lld', '-std=c++17', '-I', U / 'drivers/Adreno740', '-I', U / 'kits/AdrenoPM4',
    '-I', src, '-I', U / 'drivers/AdrenoMetal', '-I', U / '.cache/mesa-work/include', '-I', U / 'build/turnip-smoke',
    DEVICE / 'package/RuanTurnipSmoke.cpp', pm4, '-o', tool)
run('/usr/bin/llvm-strip', '-x', tool)
turnip = bin_dir / 'libvulkan_freedreno.dylib'
shutil.copyfile(U / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib', turnip)
run('/usr/bin/llvm-strip', '-x', turnip)
run(sys.executable, U / 'scripts/macho-self-bind-weak-imports.py', turnip)
# The platform sandbox refuses IOServiceOpen of a user-client class a process was not granted: the driver's class is
# RuanAdrenoDeviceUserClient (build-ruan-adreno.py renames AGXDeviceUserClient); both names, as build-adreno-metal-plugin.py does.
entitlements = OUT / 'smoke-entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                         'com.apple.security.iokit-user-client-class': ['AGXDeviceUserClient',
                                                                                        'RuanAdrenoDeviceUserClient']}))
run(LDID, f'-S{entitlements}', '-Iorg.ruan.turnip-smoke', tool)
run(LDID, '-S', '-Iorg.blu-sharky.turnip', turnip)
hashes = []
for path in (tool, turnip):
    detail = subprocess.run([LDID, '-h', str(path)], capture_output=True, text=True, check=True).stdout
    found = re.findall(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    assert len(found) == 1, f'{path}: one CDHash expected:\n{detail}'
    hashes.append(found[0])
signed = plistlib.loads(subprocess.run([LDID, '-e', str(tool)], capture_output=True, check=True).stdout)
assert signed == plistlib.loads(entitlements.read_bytes()), 'the signed entitlement set differs'
for path in (tool, turnip):
    head = path.read_bytes()[:12]
    assert head[:8] == bytes.fromhex('cffaedfe0c000001'), f'{path}: not an arm64 Mach-O'
(OUT / 'cdhashes').write_text(''.join(h + '\n' for h in hashes))
print(f'PASS: {tool.name} {hashes[0]}, {turnip.name} {hashes[1]} ({turnip.stat().st_size} bytes) in {OUT}/overlay')
