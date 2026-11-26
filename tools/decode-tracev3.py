#!/usr/bin/env python3
"""Decode macOS 27 unified-log tracev3 files (ZBM/LZBITMAP or bv41 chunksets) without uuidtext.

Prints one line per firehose/oversize record:
  <seconds> pid=<pid> <uuid8> [subsystem:category] <level> | <dynamic string args>
Format strings are not resolved (they live in uuidtext / the shared cache); only the
dynamic arguments (public %s/%@ strings, numbers) and catalog metadata are shown.
Ported from mandiant/macos-UnifiedLogs (Apache-2.0): lzbitmap.rs, catalog.rs, chunks/.
Usage: decode.py [--all-items] file.tracev3 ...
"""
import struct, sys

u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]
u24 = lambda b, o: b[o] | b[o + 1] << 8 | b[o + 2] << 16


def lzbitmap(data):
    """Apple LZBITMAP ('ZBM' + flags byte). flags 0x9 = APFS, 0xc = tracev3 (no RLE tokens, 16K chunks)."""
    assert data[:3] == b'ZBM', 'not ZBM'
    flags = data[3]
    assert flags in (0x8, 0x9, 0xc, 0xd), 'unknown ZBM flags %#x' % flags
    rle = not flags & 4
    nbmap = 12 if rle else 13
    out = bytearray()
    p = 4
    while p + 6 <= len(data):
        csize, dsize = u24(data, p), u24(data, p + 3)
        if dsize == 0:
            break
        if csize == dsize + 6:  # stored chunk
            out += data[p + 6:p + 6 + dsize]
            p += csize
            continue
        c = data[p:p + csize]
        doff, boff, toff = u24(c, 6), u24(c, 9), u24(c, 12)
        bits = c[csize - 17:]  # 17-byte packed 10-bit token table at chunk end
        tmap = [(0, 0), (0, 1), (0, 2)]
        for i in range(nbmap):
            v = u16(bits, i * 10 // 8) >> (i * 10 % 8)
            tmap.append((v & 0xff, (v >> 8) & 3))
        assert 15 <= doff <= boff <= toff <= csize - 17, 'bad ZBM chunk offsets'
        tokens = [n for b in c[toff:csize - 17] for n in (b & 15, b >> 4)]
        ti = 0
        cur = 15
        dist = 8
        while dsize > 0:
            idx = tokens[ti]; ti += 1
            rep = 1
            if rle:
                assert idx != 15
                if dsize > 8 and tokens[ti] == 15:
                    ti += 1; rep = 4; v = 15
                    while v == 15:
                        v = tokens[ti]; ti += 1; rep += v
            for _ in range(rep):
                bm, db = tmap[idx]
                if idx < 3:
                    bm = c[boff]; boff += 1
                if db == 1:
                    dist = c[doff]; doff += 1
                elif db == 2:
                    dist = u16(c, doff); doff += 2
                n = min(8, dsize)
                for _ in range(n):
                    if bm & 1:
                        out.append(c[cur]); cur += 1
                    else:
                        out.append(out[-dist])
                    bm >>= 1
                dsize -= n
                if dsize == 0:
                    break
        p += csize
    return bytes(out)


def lz4_block(src):
    out = bytearray(); i = 0
    while i < len(src):
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out += src[i:i + lit]; i += lit
        if i >= len(src): break
        off = u16(src, i); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        for _ in range(ml + 4): out.append(out[-off])
    return bytes(out)


def chunks(buf):
    o = 0
    while o + 16 <= len(buf):
        tag, sub, size = struct.unpack_from('<IIQ', buf, o)
        yield tag, buf[o + 16:o + 16 + size]
        o = (o + 16 + size + 7) & ~7


def cstr(b, o):
    e = b.find(b'\0', o)
    return b[o:e if e >= 0 else len(b)].decode('utf-8', 'replace')


def parse_catalog(c):
    sso, pio, npi, sco, nsc = struct.unpack_from('<5H', c, 0)
    base = 24
    uuids = [c[base + i * 16:base + i * 16 + 16].hex().upper() for i in range(sso // 16)]
    strs = c[base + sso:base + pio]
    o = base + pio
    procs = {}
    for _ in range(npi):
        _, _, main_i, _ = struct.unpack_from('<4H', c, o)
        fp, sp = struct.unpack_from('<QI', c, o + 8)
        pid, euid, persona, nuu, _ = struct.unpack_from('<5I', c, o + 20)
        o += 40 + nuu * 16
        nsub = u32(c, o); o += 8
        subs = {}
        for _ in range(nsub):
            ident, so, co = struct.unpack_from('<3H', c, o); o += 6
            subs[ident] = (cstr(strs, so), cstr(strs, co))
        o += (8 - nsub * 6 % 8) % 8
        procs[(fp, sp)] = dict(pid=pid, uuid=uuids[main_i] if main_i < len(uuids) else '?', subs=subs)
    return procs


# item types (see mandiant item.rs)
NUM, STR, ARB = {0x00, 0x02}, {0x20, 0x22, 0x40, 0x42}, {0x30, 0x32, 0xf2}
PRIV = {0x01, 0x05, 0x45, 0x85, 0x21, 0x25, 0x35, 0x81, 0xf1, 0x31, 0x41}


def parse_items(d, flags):
    """Return list of printable argument strings from a firehose item blob."""
    if len(d) < 2:
        return []
    n = d[1]; o = 2
    items, deferred = [], []
    for _ in range(n):
        t, sz = d[o], d[o + 1]; o += 2
        if t in NUM:
            items.append(str(int.from_bytes(d[o:o + sz], 'little', signed=True))); o += sz
        elif t in (0x10, 0x12):
            o += sz; items.append(None)
        elif t in STR or t in ARB:
            ssz = u16(d, o + 2); o += 4
            items.append('(null)' if ssz == 0 and t in (0x40, 0x42) else ['', t, ssz])
            if items[-1] != '(null)':
                deferred.append(len(items) - 1)
        elif t in PRIV:
            o += 4; items.append('<private>')
        else:
            o += sz; items.append('?%#x' % t)
    rest = d[o:]
    if flags & 0x1000 or rest[:3] == b'\x01\x00\x12':  # backtrace blob precedes strings
        uc, oc = rest[3], u16(rest, 4)
        rest = rest[6 + uc * 16 + oc * 5 + (-oc % 4):]
    for i in deferred:
        _, t, ssz = items[i]
        raw, rest = rest[:ssz], rest[ssz:]
        items[i] = raw.rstrip(b'\0').decode('utf-8', 'replace') if t in STR else raw.hex()
    return [x for x in items if x is not None]


LEVEL = {0: 'Default', 1: 'Info', 2: 'Debug', 0x10: 'Error', 0x11: 'Fault'}


def records(path, all_items):
    buf = open(path, 'rb').read()
    num, den = 1, 1
    procs = {}
    for tag, body in chunks(buf):
        if tag == 0x1000:
            num, den = u32(body, 0), u32(body, 4)
        elif tag == 0x600b:
            procs = parse_catalog(body)
        elif tag == 0x600d:
            if body[:3] == b'ZBM':
                data = lzbitmap(body)
            elif body[:4] in (b'bv41', b'bv4-'):
                data = lz4_block(body[12:12 + u32(body, 8)]) if body[:4] == b'bv41' else body[8:8 + u32(body, 4)]
            else:
                print('# unknown chunkset %r' % body[:4], file=sys.stderr); continue
            for t2, c in chunks(data):
                yield from decode_chunk(t2, c, procs, num, den, all_items)


def decode_chunk(tag, c, procs, num, den, all_items):
    if tag == 0x6001:  # firehose
        fp, sp = struct.unpack_from('<QI', c, 0)
        pub, _priv_off = u16(c, 16), u16(c, 18)
        base_t = u64(c, 24)
        pr = procs.get((fp, sp), dict(pid='?', uuid='?', subs={}))
        o, end = 32, 16 + pub
        while o + 24 <= end and c[o] != 0:
            atype, ltype, flags, _fmt, tid, dlo, dhi, dsz = struct.unpack_from('<BBHIQIHH', c, o)
            d = c[o + 24:o + 24 + dsz]
            o += 24 + dsz
            o += (-dsz % 8) if not any(c[o:o + (-dsz % 8)]) else 0
            args, sub = [], None
            try:
                if atype in (4, 6):  # nonactivity / signpost
                    p = (8 if flags & 1 else 0) + (4 if flags & 0x40 else 0) + (4 if flags & 0x100 else 0) + 4
                    ft = flags & 0xe
                    p += {0xc: 2, 0x8: 2, 0x4: 0, 0x2: 0, 0xa: 16}.get(ft, 0) + (2 if flags & 0x20 and ft in (4, 0xc) else 0)
                    if flags & 0x200:
                        sub = u16(d, p); p += 2
                    if atype == 6:
                        p += 8  # signpost id
                    if flags & 0x400:
                        p += 1
                    if flags & 0x800:
                        args.append('<oversize %d>' % u32(d, p)); p += 4
                    if atype == 6 and flags & 0x8000:
                        p += 4 + (2 if ft == 0xc or (ft == 4 and flags & 0x20) else 0)
                    args += parse_items(d[p:], flags)
                elif all_items:
                    args = ['<type %d raw %s>' % (atype, d.hex())]
            except (IndexError, struct.error) as e:
                args.append('<parse error %s>' % e)
            t = (base_t + (dlo | dhi << 32)) * num / den / 1e9
            ss = pr['subs'].get(sub, ('', ''))
            yield (t, pr['pid'], pr['uuid'], '%s:%s' % ss if sub is not None else '-',
                   LEVEL.get(ltype, 'lt%#x' % ltype), args)
    elif tag == 0x6002:  # oversize: args for a firehose entry that referenced it
        fp, sp = struct.unpack_from('<QI', c, 0)
        t, ref, pub = u64(c, 16), u32(c, 24), u16(c, 28)
        pr = procs.get((fp, sp), dict(pid='?', uuid='?', subs={}))
        yield (t * num / den / 1e9, pr['pid'], pr['uuid'], 'oversize#%d' % ref, '', parse_items(c[32:32 + pub], 0))
    elif tag in (0x6003, 0x6004):  # statedump / simpledump: just strings
        yield (0, '?', '?', 'dump%#x' % tag, '', [s.decode() for s in strings(c)])


def strings(b, n=6):
    import re
    return re.findall(rb'[\x20-\x7e]{%d,}' % n, b)


def selftest():
    # stored-chunk ZBM round trip: header + one stored chunk + terminator
    payload = b'hello tracev3'
    z = b'ZBM\x0c' + (len(payload) + 6).to_bytes(3, 'little') + len(payload).to_bytes(3, 'little') + payload + b'\0' * 6
    assert lzbitmap(z) == payload
    assert parse_items(bytes([2, 2, 0, 4, 7, 0, 0, 0, 0x22, 4, 0, 0, 3, 0]) + b'hi\0', 0) == ['7', 'hi']


if __name__ == '__main__':
    selftest()
    all_items = '--all-items' in sys.argv
    out = []
    for f in [a for a in sys.argv[1:] if not a.startswith('--')]:
        out += records(f, all_items)
    out.sort(key=lambda r: r[0])
    # shortcut: no uuidtext, so guess pid->name from launchd-style "name[pid]" / "svc [pid]" args
    import re, collections
    seen = collections.defaultdict(collections.Counter)
    for r in out:
        for a in r[5]:
            for name, pid in re.findall(r'(?:(?:system|gui/\d+|user/\d+|pid/\d+)/|requestor = )([\w.\-]+) ?\[(\d+)\]', a):
                seen[int(pid)][name.rsplit('.', 1)[-1] if name.startswith('com.apple.') else name] += 1
    names = {pid: c.most_common(1)[0][0] for pid, c in seen.items()}
    w = sys.stdout.write
    for t, pid, uuid, ss, lvl, args in out:
        w('%14.6f %s(%s) %s %s %s | %s\n' % (t, names.get(pid, '?'), pid, uuid[:8], ss, lvl, ' | '.join(args)))
