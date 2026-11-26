#!/usr/bin/env python3
"""Stage (a) check: compiles the new kernel files with the flags build-gop-legacy-display-27.py uses (its compile records of the macOS
arm64 tree) into a temporary directory and prints each object's __TEXT/__DATA size. Nothing is linked; nothing outside the temp dir
is written.

usage: syntax-check.py [TREE] [PATCHED_PACKAGE]
  TREE             macOS kernel build tree (default /root/unixnu-work/UniXNU-macos/out/ruan/macos-27.3-26C5035r/ruan_macos_arm64/tree)
  PATCHED_PACKAGE  package/ with the patch applied (default ../work/b/package next to this script)
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
TREE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/root/unixnu-work/UniXNU-macos/out/ruan/macos-27.3-26C5035r/ruan_macos_arm64/tree')
PKG = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parent / 'work/b/package'
LIVE = Path('/root/unixnu-work/UniXNU-ruan-macos/package')   # RuanZap.h and the rest of the board package, read only
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
TRIPLE, IOS = 'arm64-apple-ios27.0', '27.0'
COMMON = ['-Wno-overriding-option', '-Wno-missing-designated-field-initializers', '-Wno-unused-but-set-global', '-Wno-deprecated-volatile',
          '-I', str(TREE / '.cache/xnu-work/osfmk'), '-I', str(PKG), '-I', str(LIVE), '-mstrict-align'] + __import__('os').environ.get('EXTRA', '').split()


def command(recipe, src, dst, cxx):
    rec = json.loads((RECORDS / recipe).read_text().strip().rstrip(','))
    cmd, args, i = [], rec['arguments'], 0
    while i < len(args):
        v = args[i]
        if v == rec['file']:
            cmd.append(str(src))
        elif v in ('-o', '-MF', '-MT', '-MJ'):
            if v == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif v == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif v.startswith(('--target=', '-mios-version-min=', '-miphoneos-version-min=')) or v.startswith('-W'):
            pass
        else:
            cmd.append(v)
        i += 1
    cmd += [f'--target={TRIPLE}', f'-mios-version-min={IOS}', '-Wall', '-Wextra', '-Werror']
    cmd += (['-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
             '-Wno-deprecated-enum-enum-conversion'] if cxx else ['-mgeneral-regs-only'])
    return cmd + COMMON, rec['directory']


CXX, C, BSD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', 'osfmk/DEVELOPMENT/startup.o.json', 'bsd/DEVELOPMENT/kern_exec.o.json'
JOBS = [(CXX, 'RuanAdsp.cpp', True)] + [(C, f'{n}.c', False) for n in ('QcomMdt', 'QcomQmp', 'QcomRpmh', 'QcomSmem', 'QcomSmp2p', 'FatVolume')] + \
       [(BSD, 'AdspFirmware.c', False)]
failed, text, data = 0, 0, 0
with tempfile.TemporaryDirectory() as tmp:
    for recipe, name, cxx in JOBS:
        obj = Path(tmp) / (name.rsplit('.', 1)[0] + '.o')
        cmd, cwd = command(recipe, PKG / name, obj, cxx)
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        if r.returncode:
            failed += 1
            print('FAIL', name)
            sys.stdout.write(r.stderr[-4000:])
            continue
        size = subprocess.run(['llvm-size', '-m', str(obj)], capture_output=True, text=True).stdout
        tx = dt = 0
        for l in size.splitlines():
            l = l.strip()
            if l.startswith('Section ('):
                seg = l[9:].split(',')[0]
                v = int(l.rsplit(':', 1)[1])
                tx += v if seg == '__TEXT' else 0
                dt += v if seg == '__DATA' else 0
        text += tx
        data += dt
        undefined = subprocess.run(['llvm-nm', '-u', str(obj)], capture_output=True, text=True).stdout.split()
        print(f'PASS {name}: text+const {tx} B, data+bss {dt} B; undefined: {" ".join(sorted(undefined))}')
print(f'total: text+const {text} B ({text / 1024:.1f} KiB), data+bss {data} B ({data / 1024:.1f} KiB)')
sys.exit(1 if failed else 0)
