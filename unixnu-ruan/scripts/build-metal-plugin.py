#!/usr/bin/env python3
"""GPU step 5 (macOS/docs/research/GPU-METAL-PORT.md): UniXNU's AdrenoMetal plugin (Metal -> metal2vulkan -> Turnip) for iOS 17,
built on this Linux host against an iOS 17.5 SDK and staged, with step 4's smoke overlay, as one root overlay for make-ramroot.py --overlay.

usage: build-metal-plugin.py [OUT]   (default OUT = UniXNU build/ruan-metal-plugin; run build-turnip-smoke.py first)

OUT/overlay/System/Library/Extensions/NativeAdrenoMetal.bundle   the plugin with Turnip inside (MetalPluginName of ruan's accelerator)
OUT/overlay/System/Library/Extensions/NativeAdrenoMetal.publish  sheng's --publish marker: backboardd composites through the plugin
OUT/overlay/usr/bin/...                                          step 4's smoke tool and Turnip (build/ruan-turnip-smoke/overlay)
OUT/cdhashes                                                     all their CDHashes, for build-ramroot.sh's trust module

Same steps as UniXNU scripts/build-adreno-metal-plugin.py --target ios --no-replay without the probe daemon (sheng packs --no-daemon),
with Linux tools: clang + lld against $UNIXNU_SDKROOT, llvm-strip, theos ldid (4 KiB pages). The bundle sits in
/System/Library/Extensions of the root itself, so sheng's addon mount (gpu-addon=1) is not needed; ruan publishes the
accelerator unconditionally (LegacyDisplayProbe.cpp, sheng's gpu-metal=1). The Rust bridge comes from
scripts/build-adreno-metal-bridge.py --target ios (rustup 1.98.0 + aarch64-apple-ios, metal2vulkan in .cache/gpu-reference).
"""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

U = Path(os.environ.get('UNIXNU', '/root/unixnu-work/UniXNU'))
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else U / 'build/ruan-metal-plugin'
SMOKE = U / 'build/ruan-turnip-smoke'
# The plugin needs the iOS 17 Metal headers (MTLBindingAccess and others): xybp888/iOS-SDKs iPhoneOS17.5.sdk, sparse clone.
SDK = os.environ.get('UNIXNU_SDKROOT', '/root/sdk17/repo/iPhoneOS17.5.sdk')
LDID = os.environ.get('LDID', '/root/theos/toolchain/linux/iphone/bin/ldid')
TURNIP = U / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
VK = U / '.cache/mesa-work/include'


def run(*cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), file=sys.stderr)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def cdhash(path):
    found = re.findall(r'^CDHash=([0-9a-f]{40})$', run(LDID, '-h', path, capture_output=True, text=True).stdout, re.M)
    assert len(found) == 1, f'{path}: one CDHash expected'
    return found[0]


for need in (TURNIP, SMOKE / 'cdhashes', U / 'build/turnip-smoke'):
    if not need.exists():
        sys.exit(f'{need} missing: run build-turnip-smoke.py first')
# The Rust bridge; cc-rs compiles sha2's assembly for the iOS target with these.
env = dict(os.environ, PATH='/root/.cargo/bin:' + os.environ['PATH'], SDKROOT=SDK, CC_aarch64_apple_ios='/usr/bin/clang',
           AR_aarch64_apple_ios='/usr/bin/llvm-ar', CFLAGS_aarch64_apple_ios=f'-target arm64-apple-ios17.0 -isysroot {SDK}')
run(sys.executable, U / 'scripts/build-adreno-metal-bridge.py', '--target', 'ios', cwd=U, env=env)
BRIDGE = U / 'build/adreno-metal-bridge/ios/libadreno_metal_bridge.a'
(U / 'build/turnip-smoke/TurnipReplay.h').unlink(missing_ok=True)   # --no-replay: the host-simulation capture is diagnostic only

shutil.rmtree(OUT, ignore_errors=True)
shutil.copytree(SMOKE / 'overlay', OUT / 'overlay')
ext = OUT / 'overlay/System/Library/Extensions'
bundle = ext / 'NativeAdrenoMetal.bundle'
bundle.mkdir(parents=True)
source = hashlib.sha256()
for path in sorted((U / 'drivers/AdrenoMetal').rglob('*')):
    if path.is_file() and '.git' not in path.parts and path.suffix in ('.h', '.mm', '.cpp', '.rs', '.patch'):
        source.update(str(path.relative_to(U / 'drivers/AdrenoMetal')).encode())
        source.update(path.read_bytes())
build_id = source.hexdigest()
manifest = {'metal_source_sha256': build_id, 'target': 'ios'}
profile = TURNIP.parents[3] / 'builtin-build-profile.json'
if profile.is_file():
    manifest['turnip'] = json.loads(profile.read_text())
(bundle / 'gpu-build.json').write_text(json.dumps(manifest, indent=2) + '\n')

flags = ['-target', 'arm64-apple-ios17.0', '-isysroot', SDK, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong',
         f'-DNATIVE_METAL_BUILD_ID="{build_id}"']
pm4 = OUT / 'AdrenoPM4.o'
run('/usr/bin/clang', *flags, '-std=c11', '-c', U / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', pm4)
exe = bundle / 'NativeAdrenoMetal'
# Shader priming (UniXNU docs/porting/gpu.md "Warm the shader caches"): the OS image's .metallib paths, embedded as __DATA,__primelist
# (the render server's sandbox may refuse the file beside the bundle) and shipped beside the bundle. Written here from make-ramroot.py's
# root.manifest (ROOT_MANIFEST); gpu-prime=0 turns priming off.
# Opt-in (PRIME=1): every Metal process (mediaserverd too) primes the list, mediaserverd hit its per-process limit priming QuartzCore,
# was killed and relaunched over and over, and unowned anonymous memory ("throttled", no compressor) grew ~2.3 GB until jetsam had
# no victim and panicked (rbu82/rbu83, 2026-11-19). Without a list priming scans only what the sandbox shows, as rbu81 did.
PRIME_LIST = U / 'build/prime-list.txt'
PRIME_LIST.unlink(missing_ok=True)
MANIFEST = Path(os.environ.get('ROOT_MANIFEST', '/root/unixnu-work/ramroot/work-full/root.manifest'))   # make-ramroot.py's
if os.environ.get('PRIME') == '1' and MANIFEST.is_file():
    # mode, uid, gid, path, link target; Apple's AGX runtime libraries are not translated by the plugin
    paths = sorted({'/' + line.split('\t')[3] for line in MANIFEST.read_text().splitlines()
                    if line.count('\t') >= 3 and line.split('\t')[3].endswith('.metallib') and 'AGXMetal' not in line})
    PRIME_LIST.write_text(''.join(p + '\n' for p in paths))
prime_section = [f'-Wl,-sectcreate,__DATA,__primelist,{PRIME_LIST}'] if PRIME_LIST.is_file() else []
# The iOS 17.5 SDK predates MTLMathMode / MTLMathFloatingPointFunctions (iOS 18), which the frontend reads only through runtime
# selectors iOS 17 lacks: their SDK 18 values, so it compiles.
sdk18 = ['-DMTLMathModeSafe=0', '-DMTLMathModeRelaxed=1', '-DMTLMathModeFast=2',
         '-DMTLMathFloatingPointFunctionsFast=0', '-DMTLMathFloatingPointFunctionsPrecise=1']
run('/usr/bin/clang++', *flags, '-fuse-ld=lld', '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-bundle', *sdk18,
    '-I', U / 'drivers/Adreno740', '-I', U / 'kits/AdrenoPM4', '-I', VK, '-I', U / 'build/turnip-smoke',
    '-x', 'objective-c++', U / 'drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm', U / 'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm',
    '-x', 'none', pm4, BRIDGE, '-Wl,-dead_strip', *prime_section, '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
    '-o', exe)
run('/usr/bin/llvm-strip', '-x', exe)
# The linker writes the Rust bridge's debug symbols (~230 MB) beside the executable: keep them in OUT, not in the root.
shutil.rmtree(OUT / 'NativeAdrenoMetal.dSYM', ignore_errors=True)
if (bundle / 'NativeAdrenoMetal.dSYM').exists():
    shutil.move(bundle / 'NativeAdrenoMetal.dSYM', OUT / 'NativeAdrenoMetal.dSYM')
turnip = bundle / 'libvulkan_freedreno.dylib'
shutil.copyfile(TURNIP, turnip)
run('/usr/bin/llvm-strip', '-x', turnip)
run(sys.executable, U / 'scripts/macho-self-bind-weak-imports.py', turnip)
run(LDID, '-S', '-Iorg.blu-sharky.turnip', turnip)
(bundle / 'Info.plist').write_bytes(plistlib.dumps(dict(
    CFBundleIdentifier='org.blu-sharky.NativeAdrenoMetal', CFBundleExecutable='NativeAdrenoMetal', CFBundlePackageType='BNDL',
    CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeAdrenoMetalDevice', MinimumOSVersion='17.0'),
    fmt=plistlib.FMT_XML))
# shortcut: theos ldid signs Mach-Os only, so the bundle has no resource seal (_CodeSignature); dyld/AMFI check only the executable's
# own signature against the trust cache. Use codesign on a Mac if Metal's bundle load ever wants the seal.
run(LDID, '-S', '-Iorg.blu-sharky.NativeAdrenoMetal', exe)
(ext / 'NativeAdrenoMetal.publish').write_bytes(b'')
if PRIME_LIST.is_file():
    shutil.copyfile(PRIME_LIST, ext / 'NativeAdrenoMetal.primelist')
# PIPELINE_PACK=<file>: a pipeline pack (scripts/make-ruan-m2vpack.py from a pulled cache directory of the same runtime + Turnip build);
# the plugin imports it into each Metal process's cache directory before the runtime opens it (CacheExport.h importPack).
if os.environ.get('PIPELINE_PACK'):
    shutil.copyfile(os.environ['PIPELINE_PACK'], ext / 'NativeAdrenoMetal.m2vpack')
hashes = [cdhash(exe), cdhash(turnip)]
for path in (exe, turnip):
    assert path.read_bytes()[:8] == bytes.fromhex('cffaedfe0c000001'), f'{path}: not an arm64 Mach-O'
smoke = (SMOKE / 'cdhashes').read_text().split()
(OUT / 'cdhashes').write_text(''.join(h + '\n' for h in smoke + [h for h in hashes if h not in smoke]))
print(f'PASS: NativeAdrenoMetal {hashes[0]} ({exe.stat().st_size} bytes), Turnip {hashes[1]}, + smoke overlay in {OUT}/overlay')
