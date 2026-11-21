#!/usr/bin/env python3
"""Build QcaBluetooth (for the WCN6750) and ruan's UART clock for the selected kernel; prints OBJ <path> per object.

filipnavara/UniXNU-asteroids 9f07af3 scripts/build-qca-bluetooth.py, for ruan: the sources are the component's core, Linux boundary,
GENI SE DMA, UART.SK front end and Linux btqca.c (staged by scripts/fetch-qca-bluetooth.sh), compiled with the flags of the kernel's
kern_exec.c (the front end uses sysctl and kernel control), plus package/RuanGeniUart.c and package/BluetoothFirmware.c.
RuanBluetooth.cpp is compiled by build-ram-root-probe.py (IOKit record) like RuanWifi.cpp.

usage: build-qca-bluetooth.py COMPONENT OUTPUT   (cwd: the UniXNU tree the kernel was built in)"""
import json
import subprocess
import sys
from pathlib import Path

device = Path(__file__).resolve().parents[1]
component, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
recipe = json.loads((Path('.').resolve() / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json')
                    .read_text().strip().rstrip(','))
package = device / 'package'
sources = [component / f for f in ('src/qbt_core.c', 'src/qbt_linux.c', 'src/qbt_geni.c', 'src/qbt_transport.c', 'out/linux/btqca.c')]
sources += [package / 'RuanGeniUart.c', package / 'BluetoothFirmware.c']
for source in sources:
    obj = out / (source.stem + '.o')
    cmd, args, i = [], recipe['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == recipe['file']:
            cmd.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            cmd.append(value)
        i += 1
    cmd += ['-mgeneral-regs-only', '-DQBT_SOC_TYPE=QCA_WCN6750', '-Wno-sign-compare',
            *['-I' + str(d) for d in (component / 'include', component / 'src', component / 'shim', component / 'out/linux', package)]]
    result = subprocess.run(cmd, cwd=recipe['directory'], text=True, capture_output=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'{source}:\n' + result.stdout + result.stderr)
    print('OBJ ' + str(obj))
