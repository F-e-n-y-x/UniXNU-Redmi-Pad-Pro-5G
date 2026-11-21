#!/usr/bin/env python3
"""Syntax check of the port's kernel files with the exact flags of ruan's last XNU build (its compile records), -fsyntax-only: nothing is
built or linked, no object is written. The recipes are build-ram-root-probe.py's compile_like / compile_cxx_like (C: startup.o or kern_exec.o
flags; C++: IOPlatformExpert.cpo flags). Audio port copy of ../../wifi/port/check/syntax-check.py. Also builds and runs the two host tests
(package/tests) with the host cc into a temporary directory.

usage: syntax-check.py [TREE]
  TREE            the kernel build tree (default /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree)
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
DEVICE = Path(__file__).resolve().parents[5] / 'unixnu-ruan'       # macOS/unixnu-ruan (read only: RuanZap.h, SmmuBypass.h)
TREE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree')
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
INCLUDES = ['-I', str(PORT / 'package'), '-I', str(DEVICE / 'package')]


def command(record_name, source, cxx, extra):
    record = json.loads((RECORDS / record_name).read_text().strip().rstrip(','))
    out, suppressions, args, i = [], [], record['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == record['file']:
            out.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif cxx and value.startswith('-Wno-'):
            suppressions.append(value)
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-c'):
            out.append(value)
        i += 1
    out += ['-fsyntax-only', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
    if cxx:
        out += ['-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-I', record['directory'], '-I', str(TREE / '.cache/xnu-work/osfmk')]
    return out + INCLUDES + list(extra) + suppressions, record['directory']


C_STARTUP, C_BSD, CXX = 'osfmk/DEVELOPMENT/startup.o.json', 'bsd/DEVELOPMENT/kern_exec.o.json', 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
# The recipe per file of patches/audio-port.patch (build-ram-root-probe.py): C++ with the IOKit record, C with startup.o's.
JOBS = [(CXX, 'RuanAudio.cpp', True, []), (CXX, 'RuanAdsp.cpp', True, []),
        (C_STARTUP, 'ShengGpr.c', False, []), (C_STARTUP, 'ShengAudioReach.c', False, [])]
failed = 0
for record, name, cxx, extra in JOBS:
    cmd, cwd = command(record, PORT / 'package' / name, cxx, extra)
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    tag = name + (' ' + ' '.join(extra) if extra else '')
    print(('PASS ' if not r.returncode else 'FAIL ') + tag)
    if r.returncode:
        failed += 1
        sys.stdout.write(r.stderr[-6000:])
with tempfile.TemporaryDirectory() as tmp:
    for test, sources in (('audioreach_host.c', ['../ShengAudioReach.c', '../ShengGpr.c']), ('gpr_host.c', ['../ShengGpr.c'])):
        exe = Path(tmp) / test[:-2]
        r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), test, *sources], cwd=PORT / 'package/tests',
                           capture_output=True, text=True)
        r = r if r.returncode else subprocess.run([str(exe)], capture_output=True, text=True)
        print(('PASS ' if not r.returncode else 'FAIL ') + test + ': ' + (r.stdout + r.stderr).strip()[-300:])
        failed += 1 if r.returncode else 0
sys.exit(1 if failed else 0)
