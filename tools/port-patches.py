#!/usr/bin/env python3
"""Port small binary patches between two builds of the same AArch64 UEFI driver.

  port-patches.py REF_ORIG.efi REF_PATCHED.efi TARGET_ORIG.efi TARGET_PATCHED.efi

Each changed run in REF is located in TARGET by an instruction window whose PC-relative
immediates (B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR/ADRP, LDR literal) are masked out. The window
must match exactly once. Patched words that are B/BL get their target re-found the same way
(signature of the callee). Anything ambiguous aborts: a wrong patch in firmware is worse than none.
"""
import struct
import sys

WIN = int(__import__("os").environ.get("WIN", 8))  # instructions of context on each side (env WIN overrides)
LOOSE = __import__("os").environ.get("LOOSE") == "1"  # also mask lo12/field offsets that move between builds


def words(b):
    return list(struct.unpack(f"<{len(b) // 4}I", b[: len(b) // 4 * 4]))


def mask(w):
    if (w & 0x7C000000) == 0x14000000:          # B / BL
        return w & 0xFC000000
    if (w & 0xFF000010) == 0x54000000:          # B.cond
        return w & 0xFF00001F
    if (w & 0x7E000000) == 0x34000000:          # CBZ / CBNZ
        return w & 0xFF00001F
    if (w & 0x7E000000) == 0x36000000:          # TBZ / TBNZ
        return w & 0xFFF8001F
    if (w & 0x1F000000) == 0x10000000:          # ADR / ADRP
        return w & 0x9F00001F
    if (w & 0x3B000000) == 0x18000000:          # LDR (literal)
        return w & 0xFF00001F
    if LOOSE and (w & 0x7F800000) == 0x11000000:  # ADD/SUB imm (the :lo12: half of ADRP pairs)
        return w & 0xFFC003FF
    if LOOSE and (w & 0x3B000000) == 0x39000000:  # LDR/STR unsigned offset (struct/global field offsets)
        return w & 0xFFC003FF
    return w


def find_unique(hay, pat):
    hits = [i for i in range(len(hay) - len(pat) + 1) if all(mask(hay[i + k]) == p for k, p in enumerate(pat))]
    return hits


def branch_target(w, idx):
    imm = w & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 1 << 26
    return idx + imm


def main(ref_o, ref_p, tgt_o, out):
    ro, rp, to = (open(p, "rb").read() for p in (ref_o, ref_p, tgt_o))
    assert len(ro) == len(rp), "reference images differ in size"
    RO, RP, TO = words(ro), words(rp), words(to)
    TP = list(TO)
    changed = [i for i in range(len(RO)) if RO[i] != RP[i]]
    runs, cur = [], []
    for i in changed:
        if cur and i != cur[-1] + 1:
            runs.append(cur)
            cur = []
        cur.append(i)
    if cur:
        runs.append(cur)
    global LOOSE
    for run in runs:
        a, b = run[0], run[-1] + 1
        # Widest context first; one-sided windows cover function-entry patches whose
        # preceding bytes are an unrelated function. The first unique match wins.
        delta = None
        for loose in (False, True):
            LOOSE = loose
            for before, after in ((WIN, WIN), (0, 2 * WIN), (2 * WIN, 0), (WIN // 2, WIN // 2), (0, WIN)):
                lo, hi = max(0, a - before), min(len(RO), b + after)
                hits = find_unique(TO, [mask(w) for w in RO[lo:hi]])
                if len(hits) == 1:
                    delta = hits[0] - lo
                    how = f"window -{before}/+{after}{' loose' if loose else ''}"
                    break
            if delta is not None:
                break
        if delta is None:
            sys.exit(f"run @0x{a * 4:x}: no unique match in target, refusing")
        for i in range(a, b):
            w = RP[i]
            if (w & 0x7C000000) == 0x14000000 and w != RO[i]:  # new/changed branch: re-aim it
                rt = branch_target(w, i)
                sig = [mask(x) for x in RO[rt:rt + 12]]
                th = find_unique(TO, sig)
                if len(th) != 1:
                    sys.exit(f"branch @0x{i * 4:x} -> 0x{rt * 4:x}: callee has {len(th)} matches, refusing")
                w = (w & 0xFC000000) | ((th[0] - (i + delta)) & 0x03FFFFFF)
            TP[i + delta] = w
        print(f"run ref 0x{a * 4:x}..0x{b * 4:x} -> target 0x{(a + delta) * 4:x} ({how})")
    body = struct.pack(f"<{len(TP)}I", *TP) + to[len(TP) * 4:]
    open(out, "wb").write(body)
    print(f"wrote {out}")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--selftest"]:
        # B/BL keep only the opcode; ordinary ALU words are untouched
        assert mask(0x94000123) == 0x94000000 and mask(0x17FFFFFE) == 0x14000000
        assert mask(0xD65F03C0) == 0xD65F03C0 and branch_target(0x97FFFFFF, 10) == 9
        assert find_unique([1, 2, 3, 1, 2], [mask(1), mask(2)]) == [0, 3]
        print("selftest ok")
    else:
        main(*sys.argv[1:5])
