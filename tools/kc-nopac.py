#!/usr/bin/env python3
"""Make a macOS kernel collection (MH_FILESET) runnable on a CPU without FEAT_PAuth (ruan: A78/A55), in place.

  1. pacstrip every fileset entry's instruction sections (Apple's arm64e kexts; the plain arm64 kernel should have none):
     RETAA/RETAB -> RET, BRA* -> BR, BLRA* -> BLR, PAC*/AUT*/XPAC* -> NOP, PACGA -> EOR, in-range LDRAA/LDRAB -> LDR.
     LC_DATA_IN_CODE ranges and XnuPrelink's known constant pools are left alone. Anything left to emulate is an error:
     the kernel's PAC emulator (ruan-pac-emulator.patch) only handles EL0.
  2. Clear the authentication bit of every root chained fixup (format 8): a kernel without HAS_APPLE_PAC stops at
     "Unexpected authenticated fixup" (osfmk/mach/dyld_kernel_fixups.h). The target, cache level and next bits stay.
  3. Reseal the corecrypto FIPS HMAC (__fips_hmacs, HMAC-SHA256 with a zero key over __TEXT_EXEC,__text) of every entry
     whose text step 1 changed and whose seal matched before, as XnuPrelink's fileset_segmented.py does for rebinding.
  4. Check: no PAC instruction left in any entry, no auth fixup left, every entry's cpusubtype equals the collection's,
     every FIPS seal matches its text.

usage: kc-nopac.py KERNEL.macho [--check]      (--check: verify only, write nothing; exit 1 when not clean)
Env: PACSTRIP_TOOLS (insn.py/macho.py, default /root/unixnu-work/UniXNU-ruan-macos/tools),
     XNUPRELINK (default /root/unixnu-work/UniXNU-macos/kits/XnuPrelink).
"""
import hashlib
import hmac
import os
import struct
import sys

sys.path.insert(0, os.environ.get('PACSTRIP_TOOLS', '/root/unixnu-work/UniXNU-ruan-macos/tools'))
sys.path.insert(0, os.environ.get('XNUPRELINK', '/root/unixnu-work/UniXNU-macos/kits/XnuPrelink'))
from insn import KEEP, rewrite  # noqa: E402
from macho import DataRanges, Report, code_sections, data_in_code, load_commands, strip_section  # noqa: E402
from fileset_emit import FILESET_MATH_POOLS, inline_data_ranges  # noqa: E402
from emit_prelink import MATH_LITERAL_POOLS  # noqa: E402

LC_SEGMENT_64, LC_DATA_IN_CODE = 0x19, 0x29
LC_DYLD_CHAINED_FIXUPS, LC_FILESET_ENTRY = 0x80000034, 0x80000035
AUTH_BITS = 1 << 63 | 0x7ffff << 32  # isAuth, key (2), address diversity (1), diversity (16) of format 8


def entries(data):
    for cmd, at in load_commands(data, 0):
        if cmd == LC_FILESET_ENTRY:
            header, = struct.unpack_from('<Q', data, at + 16)
            name_at, = struct.unpack_from('<I', data, at + 24)
            yield data[at + name_at:data.index(b'\0', at + name_at)].decode(), header


def entry_code(data, header):
    """(code sections, data ranges) of one entry; its file offsets are absolute in the collection."""
    commands = list(load_commands(data, header))
    ranges = [r for cmd, at in commands if cmd == LC_DATA_IN_CODE for r in data_in_code(data, 0, at)]
    sections = [s for cmd, at in commands if cmd == LC_SEGMENT_64 for s in code_sections(data, 0, at)]
    for start, size in sections:
        blob = bytes(data[start:start + size])
        for pools in (FILESET_MATH_POOLS, MATH_LITERAL_POOLS):
            ranges += [(start + a, start + b) for a, b in inline_data_ranges(blob, pools)]
    return sections, DataRanges(ranges)


def fips_seal(data, header):
    """(text offset, text size, seal offset) of an entry's corecrypto FIPS HMAC, or None."""
    found = {}
    for cmd, at in load_commands(data, header):
        if cmd != LC_SEGMENT_64:
            continue
        nsects, = struct.unpack_from('<I', data, at + 64)
        for i in range(nsects):
            s = at + 72 + 80 * i
            sect, seg = (bytes(data[s + k:s + k + 16]).rstrip(b'\0').decode() for k in (0, 16))
            size, offset = struct.unpack_from('<QI', data, s + 40)
            found.setdefault(sect if sect == '__fips_hmacs' else (seg, sect), []).append((offset, size))
    seals, text = found.get('__fips_hmacs', []), found.get(('__TEXT_EXEC', '__text'), [])
    if not seals:
        return None
    if len(seals) != 1 or seals[0][1] != 32 or len(text) != 1:
        raise SystemExit('kc-nopac: unexpected FIPS HMAC layout')
    return text[0][0], text[0][1], seals[0][0]


def seal_of(data, text_at, text_size):
    return hmac.new(b'\0', bytes(data[text_at:text_at + text_size]), hashlib.sha256).digest()


def strip(data):
    total, per_entry, resealed = Report(), {}, []
    for name, header in entries(data):
        sections, ranges = entry_code(data, header)
        seal = fips_seal(data, header)
        sealed = seal and seal_of(data, seal[0], seal[1]) == bytes(data[seal[2]:seal[2] + 32])
        report = Report()
        for start, size in sections:
            report.add(strip_section(data, start, size, ranges))
        if sealed and report.patched:
            data[seal[2]:seal[2] + 32] = seal_of(data, seal[0], seal[1])
            resealed.append(name)
        per_entry[name] = report.patched
        total.add(report)
    return total, per_entry, resealed


def chain_sites(data):
    """File offsets of every root chained-fixup word (format 8, kernel collection)."""
    found = [at for cmd, at in load_commands(data, 0) if cmd == LC_DYLD_CHAINED_FIXUPS]
    if not found:
        return
    segments = [struct.unpack_from('<16sQQQQ', data, at + 8) for cmd, at in load_commands(data, 0) if cmd == LC_SEGMENT_64]
    base = next(va for _, va, _, fileoff, filesize in segments if fileoff == 0 and filesize)
    dataoff, = struct.unpack_from('<I', data, found[0] + 8)
    starts_offset, = struct.unpack_from('<I', data, dataoff + 4)
    table = dataoff + starts_offset
    count, = struct.unpack_from('<I', data, table)
    for index in range(count):
        relative, = struct.unpack_from('<I', data, table + 4 + 4 * index)
        if not relative:
            continue
        at = table + relative
        _, page_size, fmt, segment_offset, _, pages = struct.unpack_from('<IHHQIH', data, at)
        if fmt != 8:
            raise SystemExit(f'kc-nopac: chain format {fmt}, expected 8 (kernel collection)')
        segment = next(s for s in segments if s[1] - base == segment_offset)
        for page, start in enumerate(struct.unpack_from(f'<{pages}H', data, at + 22)):
            if start == 0xffff:
                continue
            if start & 0x8000:
                raise SystemExit('kc-nopac: multi-start chain pages are not supported')
            offset = start
            while True:
                site = segment[3] + page * page_size + offset
                value, = struct.unpack_from('<Q', data, site)
                yield site, value
                step = value >> 51 & 0xfff
                if not step:
                    break
                offset += step * 4


def deauth(data):
    cleared = sites = 0
    for site, value in chain_sites(data):
        sites += 1
        if value >> 63:
            struct.pack_into('<Q', data, site, value & ~AUTH_BITS)
            cleared += 1
    return sites, cleared


def check(data):
    """Problems that would stop the collection on ruan; empty when clean."""
    problems = []
    top, = struct.unpack_from('<I', data, 8)
    for name, header in entries(data):
        subtype, = struct.unpack_from('<I', data, header + 8)
        if subtype != top:
            problems.append(f'{name}: cpusubtype {subtype:#x} != collection {top:#x}')
        sections, ranges = entry_code(data, header)
        left = 0
        for start, size in sections:
            for offset in range(start, start + size - 3, 4):
                if offset not in ranges and rewrite(struct.unpack_from('<I', data, offset)[0])[0] != KEEP:
                    left += 1
        if left:
            problems.append(f'{name}: {left} PAC instructions left')
        seal = fips_seal(data, header)
        if seal and seal_of(data, seal[0], seal[1]) != bytes(data[seal[2]:seal[2] + 32]):
            problems.append(f'{name}: FIPS HMAC does not match its __text')
    auth = sum(1 for _, value in chain_sites(data) if value >> 63)
    if auth:
        problems.append(f'{auth} authenticated chained fixups left')
    return problems


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--check'):
        sys.exit(__doc__)
    path = sys.argv[1]
    data = bytearray(open(path, 'rb').read())
    if struct.unpack_from('<II', data, 0) != (0xfeedfacf, 0x100000c) or struct.unpack_from('<I', data, 12)[0] != 12:
        sys.exit(f'kc-nopac: {path} is not an arm64 MH_FILESET')
    if len(sys.argv) == 2:
        report, per_entry, resealed = strip(data)
        if report.traps:
            sys.exit(f'kc-nopac: {len(report.traps)} instructions need emulation in kernel mode: '
                     + ', '.join(f'{o:#x}:{w:#010x}' for o, w in report.traps[:8]))
        sites, cleared = deauth(data)
        print(f'kc-nopac: {report.patched} PAC instructions rewritten in {sum(1 for v in per_entry.values() if v)} entries '
              f'(kernel {per_entry.get("com.apple.kernel", 0)}); {cleared} of {sites} chained fixups de-authenticated; '
              f'FIPS HMAC resealed: {", ".join(resealed) or "none"}')
        open(path, 'wb').write(data)
    problems = check(data)
    for problem in problems:
        print('kc-nopac: NOT CLEAN:', problem)
    if problems:
        sys.exit(1)
    print(f'kc-nopac: clean: {sum(1 for _ in entries(data))} entries, 0 PAC instructions, 0 auth fixups, subtypes consistent, '
          'FIPS seals match')


if __name__ == '__main__':
    main()
