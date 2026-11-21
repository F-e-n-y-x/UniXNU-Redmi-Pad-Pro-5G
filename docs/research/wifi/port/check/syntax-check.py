#!/usr/bin/env python3
"""Syntax check of the port's kernel files with the exact flags of ruan's last XNU build (its compile records), -fsyntax-only: nothing is
built or linked, no object is written. The recipes are build-ram-root-probe.py's compile_like / compile_cxx_like (C: startup.o or kern_exec.o
flags; C++: IOPlatformExpert.cpo flags), plus -DRUAN_WIFI_MODULE=1 for RuanWifiWpss.cpp.

usage: syntax-check.py [TREE] [ATH11K_INCLUDE]
  TREE            the kernel build tree (default /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree)
  ATH11K_INCLUDE  drivers/Ath11kWifi/include of filipnavara/UniXNU-asteroids 9f07af3 (default /tmp/wifi-src/asteroids/...)
"""
import json
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
DEVICE = Path(__file__).resolve().parents[5] / 'unixnu-ruan'       # macOS/unixnu-ruan (read only: RuanZap.h, SmmuBypass.h)
TREE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree')
ATH = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('/tmp/wifi-src/asteroids/drivers/Ath11kWifi/include')
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
INCLUDES = ['-I', str(PORT / 'package'), '-I', str(DEVICE / 'package'), '-I', str(ATH)]


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
# The same recipe per file as the build-ram-root-probe.py lines in patches/wifi-port.patch.
JOBS = [(CXX, 'RuanWifiWpss.cpp', True, ['-DRUAN_WIFI_MODULE=1']), (CXX, 'RuanWifiWpss.cpp', True, []),
        (CXX, 'WpssProbe.cpp', True, []), (CXX, 'RuanWifi.cpp', True, []),
        (CXX, 'WifiNet.cpp', True, []),
        *[(C_STARTUP, f'{n}.c', False, []) for n in ('QcomGlink', 'QcomMdt', 'QcomQmp', 'QcomQrtr', 'QcomRpmh', 'QcomSmem', 'QcomSmp2p',
                                                     'FatVolume')],
        (C_BSD, 'AdspFirmware.c', False, ['-Wno-unused-but-set-global']), (C_BSD, 'AsteroidsWifiFiles.c', False, [])]
failed = 0
for record, name, cxx, extra in JOBS:
    cmd, cwd = command(record, PORT / 'package' / name, cxx, extra)
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    tag = name + (' ' + ' '.join(extra) if extra else '')
    print(('PASS ' if not r.returncode else 'FAIL ') + tag)
    if r.returncode:
        failed += 1
        sys.stdout.write(r.stderr[-6000:])
sys.exit(1 if failed else 0)
