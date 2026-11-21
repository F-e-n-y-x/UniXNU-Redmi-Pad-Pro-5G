#!/usr/bin/env python3
"""Route iPadOS 17.7.11 (21H461) bluetoothd to a generic (type 0, "third-party") HCI controller, then re-sign it.

The iOS 16 template is daimile2/renoir_xnu_bluethooth tools/patch-renoir-bluetoothd.py (20H392, BSD-3-Clause); 21H461 is the same plain
arm64 layout (no PAC, empty header padding before __text). Each site is checked word for word before it is written; any mismatch refuses.
Code goes into the r-x header padding (load commands end at 0x2b40, __text starts at 0x5550). Sites (macOS/docs/research/bluetooth/plan.md):

  1 factory     every platform constructs its controller through the thunk 0x1002d50e4 (b 0x1002d4680, w1 = chip id): w1 := 0
  2 register    0x1001e1dc8 accepted 1..0xf9f; now 0..0xf9f
  3 vendor init 0x1001e1cdc, chip 0 (was "Invalid chipset"): empty vendor op table (0x100931940, 0x878 bytes), no vendor event handler
                (0x1009321b8), no BCM/ACI hook tables; other ids keep the original error path
  4 callbacks   0x1001e1f9c, chip 0: return after saving the generic callback (0x1009321e8), as for the native chips
  5 optional    Write Default Erroneous Data Reporting (0x0C5B) and Host Flow Control: a rejection no longer fails the init chain
  6 H4 stop     0x10015f238 jumped to the transport's +0x60 hook without a null check; H4 has none
  Not applied (stage 5b, with a Type0 capability library): LE-features publication at 0x1001846ec, the LeBroadcaster start refresh,
  LC_LOAD_DYLIB. Without them the stack uses its default (legacy) LE capabilities.

usage: patch-bluetoothd.py ORIGINAL OUTPUT [--sign] (ORIGINAL: usr/sbin/bluetoothd of the extracted 21H461 root, e.g.
       /root/unixnu-work/ramroot/work-full/root/usr/sbin/bluetoothd). --sign re-signs with theos ldid (env LDID) keeping the original
       identifier and entitlements and prints the CDHash. --self-test checks the encoders only (no Apple file needed)."""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ORIGINAL = '3f1e4493ea6a76976625f7af58a8f330c0c90d82d9020b11110b4f3f5b758df4'   # 21H461 /usr/sbin/bluetoothd, 9893760 bytes
BASE = 0x100000000
CAVE, CAVE_END = 0x100005000, 0x100005550
LC_END = 0x2b40
BZERO = 0x100708bcc   # __stubs _bzero
LDID = os.environ.get('LDID', '/root/theos/toolchain/linux/iphone/bin/ldid')


def rel(pc, target, bits):
    off = (target - pc) >> 2
    if not -(1 << (bits - 1)) <= off < (1 << (bits - 1)):
        raise ValueError(f'branch out of range {pc:#x}->{target:#x}')
    return off & ((1 << bits) - 1)


def b(pc, t): return 0x14000000 | rel(pc, t, 26)
def bl(pc, t): return 0x94000000 | rel(pc, t, 26)
def b_ne(pc, t): return 0x54000001 | rel(pc, t, 19) << 5
def cbz_w(pc, rt, t): return 0x34000000 | rel(pc, t, 19) << 5 | rt
def cbnz_w(pc, rt, t): return 0x35000000 | rel(pc, t, 19) << 5 | rt
def cbz_x(pc, rt, t): return 0xb4000000 | rel(pc, t, 19) << 5 | rt
def movz_w(rd, imm): assert imm < 0x10000; return 0x52800000 | imm << 5 | rd
def movz_x(rd, imm): assert imm < 0x10000; return 0xd2800000 | imm << 5 | rd
def add_imm(rd, rn, imm): assert 0 <= imm < 4096; return 0x91000000 | imm << 10 | rn << 5 | rd
def sub_w_imm(rd, rn, imm): assert 0 <= imm < 4096; return 0x51000000 | imm << 10 | rn << 5 | rd
def cmp_w_imm(rn, imm): assert 0 <= imm < 4096; return 0x7100001f | imm << 10 | rn << 5
def ldr_w(rt, rn, off): assert off % 4 == 0; return 0xb9400000 | (off // 4) << 10 | rn << 5 | rt
def ldr_x(rt, rn, off): assert off % 8 == 0; return 0xf9400000 | (off // 8) << 10 | rn << 5 | rt
def str_x(rt, rn, off): assert off % 8 == 0; return 0xf9000000 | (off // 8) << 10 | rn << 5 | rt
def br(rn): return 0xd61f0000 | rn << 5


def adrp(pc, rd, t):
    imm = ((t & ~0xfff) - (pc & ~0xfff)) >> 12
    if not -(1 << 20) <= imm < (1 << 20):
        raise ValueError('adrp range')
    imm &= (1 << 21) - 1
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


NOP, RET, MOV_X0_X19, XZR = 0xd503201f, 0xd65f03c0, 0xaa1303e0, 31


class Image:
    def __init__(self, data): self.d = bytearray(data)
    def word(self, va): return struct.unpack_from('<I', self.d, va - BASE)[0]

    def expect(self, va, words):
        for i, w in enumerate(words):
            if self.word(va + 4 * i) != w:
                raise SystemExit(f'site {va + 4 * i:#x}: {self.word(va + 4 * i):#010x}, expected {w:#010x}: not the audited 21H461 file')

    def put(self, va, words):
        for i, w in enumerate(words):
            struct.pack_into('<I', self.d, va - BASE + 4 * i, w)

    def assemble(self, va, program):
        words = [p(va + 4 * i) if callable(p) else p for i, p in enumerate(program)]
        self.put(va, words)
        return va + 4 * len(words)


def patch(data):
    if hashlib.sha256(data).hexdigest() != ORIGINAL:
        raise SystemExit('unexpected bluetoothd: expected the unchanged 21H461 original (sha256 ' + ORIGINAL + ')')
    im = Image(data)
    if struct.unpack_from('<I', im.d, 20)[0] + 32 != LC_END or any(im.d[LC_END:CAVE_END - BASE]):
        raise SystemExit('header padding is not empty')
    cave, sites = CAVE, {}

    # 1. Factory thunk: b 0x1002d4680 -> cave: mov w1,#0; b 0x1002d4680.
    im.expect(0x1002d50e4, [b(0x1002d50e4, 0x1002d4680)])
    im.expect(0x10031ccc8, [movz_w(1, 1), bl(0x10031cccc, 0x1002d50e4)])   # the factory's default case uses it too
    sites['factory'] = cave
    cave = im.assemble(cave, [movz_w(1, 0), lambda pc: b(pc, 0x1002d4680)])
    im.put(0x1002d50e4, [b(0x1002d50e4, sites['factory'])])

    # 2. Registration accepts 0..0xf9f (sub w9,w0,#1; cmp w9,#0xf9e -> cmp w0,#0xf9f; nop; b.hi unchanged).
    im.expect(0x1001e1dc8, [sub_w_imm(9, 0, 1), cmp_w_imm(9, 0xf9e)])
    im.put(0x1001e1dc8, [cmp_w_imm(0, 0xf9f), NOP])

    # 3. Vendor init, error path entry 0x1001e1d68 (x19 = chip): chip 0 gets the vendor-less backend.
    im.expect(0x1001e1d2c, [0xaa0003f3])                       # mov x19, x0 (the chip)
    im.expect(0x1001e1d68, [movz_w(0, 2), movz_w(1, 0x54)])
    im.expect(0x1001e1da0, [0xa9427bfd, 0xa9414ff4, 0x9100c3ff, RET])   # epilogue
    im.expect(0x1001a5e88 + 8, [adrp(0x1001a5e90, 1, 0x1008d4000), add_imm(1, 1, 0xee8), movz_w(2, 0x878)])   # BCM op table size
    entry = cave
    orig = cave + 9 * 4
    cave = im.assemble(cave, [
        lambda pc: cbnz_w(pc, 19, orig),
        lambda pc: adrp(pc, 0, 0x100931000), add_imm(0, 0, 0x940), movz_x(1, 0x878),
        lambda pc: bl(pc, BZERO),
        lambda pc: adrp(pc, 8, 0x100932000), str_x(XZR, 8, 0x1b8),
        lambda pc: b(pc, 0x1001e1da0), NOP,
        movz_w(0, 2), lambda pc: b(pc, 0x1001e1d6c)])
    sites['vendor_init'] = entry
    im.put(0x1001e1d68, [b(0x1001e1d68, entry)])

    # 4. Callbacks, error path entry 0x1001e2004 (x19 = page of the chip id at +0x830): chip 0 returns.
    im.expect(0x1001e1fb4, [adrp(0x1001e1fb4, 19, 0x100931000), ldr_w(8, 19, 0x830)])
    im.expect(0x1001e2004, [movz_w(0, 2), movz_w(1, 0x54)])
    im.expect(0x1001e2040, [0xa9427bfd, 0xa9414ff4, 0x9100c3ff, RET])
    entry = cave
    orig = cave + 3 * 4
    cave = im.assemble(cave, [ldr_w(8, 19, 0x830), lambda pc: cbnz_w(pc, 8, orig), lambda pc: b(pc, 0x1001e2040),
                              movz_w(0, 2), lambda pc: b(pc, 0x1001e2008)])
    sites['callbacks'] = entry
    im.put(0x1001e2004, [b(0x1001e2004, entry)])

    # 5. Optional init steps: their callbacks advance the chain with status 0 whatever the controller answered.
    im.expect(0x100184d4c, [MOV_X0_X19, bl(0x100184d50, 0x100181a48)])             # initCmdWriteErroneousDataReportingCB
    im.put(0x100184d4c, [movz_w(0, 0)])
    im.expect(0x100184c90, [cmp_w_imm(0, 0x2cd), b_ne(0x100184c94, 0x100184ccc)])  # initCmdHostFlowControlCB
    im.expect(0x100184cd4, [bl(0x100184cd4, 0x100181a48)])
    im.put(0x100184c90, [movz_w(0, 0), b(0x100184c94, 0x100184ccc)])

    # 6. Transport pre-stop: br [transport + 0x60] only when the hook exists.
    im.expect(0x10015f238, [adrp(0x10015f238, 8, 0x100925000), ldr_x(8, 8, 0xd30), ldr_w(9, 8, 0),
                            cbz_w(0x10015f244, 9, 0x10015f250), ldr_x(0, 8, 0x60), br(0), RET])
    im.put(0x10015f240, [ldr_x(0, 8, 0x60), cbz_x(0x10015f244, 0, 0x10015f250), br(0), NOP])

    if cave > CAVE_END:
        raise SystemExit('cave overflow')
    return bytes(im.d), {k: hex(v) for k, v in sites.items()} | {'cave_end': hex(cave)}


def sign(path):
    with tempfile.TemporaryDirectory() as tmp:
        ent = Path(tmp) / 'ent.plist'
        ent.write_bytes(subprocess.run([LDID, '-e', str(path)], check=True, capture_output=True).stdout)
        if b'<plist' not in ent.read_bytes():
            raise SystemExit('no entitlements read from the original signature')
        subprocess.run([LDID, f'-S{ent}', '-Icom.apple.bluetoothd', str(path)], check=True)
    found = re.findall(r'^CDHash=([0-9a-f]{40})$', subprocess.run([LDID, '-h', str(path)], check=True, capture_output=True,
                                                                     text=True).stdout, re.M)
    if len(found) != 1:
        raise SystemExit(f'{path}: one CDHash expected')
    return found[0]


def self_test():
    # Encodings cross-checked against llvm-objdump of the original (b, bl, cbz/cbnz, adrp/add, movz, cmp, ldr, br).
    assert b(0x1002d50e4, 0x1002d4680) == 0x17fffd67
    assert cmp_w_imm(9, 0xf9e) == 0x713e793f and sub_w_imm(9, 0, 1) == 0x51000409
    assert movz_w(0, 2) == 0x52800040 and NOP == 0xd503201f
    print('PASS: encoder self-test')


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        self_test()
        sys.exit(0)
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    out, info = patch(src.read_bytes())
    dst.write_bytes(out)
    dst.chmod(0o755)
    entitlements_of_original = subprocess.run([LDID, '-e', str(src)], capture_output=True).stdout if '--sign' in sys.argv else b''
    for k, v in info.items():
        print(k, v)
    if '--sign' in sys.argv:
        cdhash = sign(dst)
        if subprocess.run([LDID, '-e', str(dst)], capture_output=True).stdout != entitlements_of_original:
            raise SystemExit('entitlements changed by re-signing')
        print('CDHash', cdhash)
