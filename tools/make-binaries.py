#!/usr/bin/env python3
"""Build a Mu-Silicium Device-Binaries tree for a device from its own stock UEFI.

Uses a reference device's tree (same SoC, here garnet) as the layout: every reference .inf is
recreated with the target's driver of the same FILE_GUID, and every RawFiles entry is taken
by name from the target's firmware volume.

  1. uefi-firmware-parser -e -O -o OUT stock-uefi.fv   (FV = uefi_a.img from the first _FVH - 40)
  2. make-binaries.py OUT/uefi.fv_output REF_DIR DEST_DIR

REF_DIR is the reference device folder of Project-Silicium/Device-Binaries (only its .inf files
are read). Drivers the reference patched (*.patched.efi) are listed at the end; they need
tools/port-patches.py before the image is usable.
"""
import os
import re
import shutil
import sys


def ui_name(d):
    for f in os.listdir(d):
        if f.endswith(".ui"):
            return open(os.path.join(d, f), "rb").read().decode("utf-16-le", "ignore").rstrip("\0")
    return None


def index(fv_out):
    by_guid, by_name = {}, {}
    for root, dirs, _ in os.walk(fv_out):
        for d in dirs:
            if not d.startswith("file-"):
                continue
            p = os.path.join(root, d)
            files = {}
            for r2, _, fs in os.walk(p):
                # stop at nested files: only this file's own sections
                if r2 != p and os.path.basename(r2).startswith("file-"):
                    continue
                for f in fs:
                    files.setdefault(f.rsplit(".", 1)[-1] if "." in f else f, os.path.join(r2, f))
            by_guid[d[5:].lower()] = (p, files)
            n = ui_name(p)
            if n:
                by_name[n] = (p, files)
    return by_guid, by_name


def main(fv_out, ref, dest):
    by_guid, by_name = index(fv_out)
    missing, patched, made = [], [], 0
    for root, _, fs in os.walk(ref):
        for f in fs:
            src = os.path.join(root, f)
            rel = os.path.relpath(src, ref)
            if f.endswith(".inf"):
                inf = open(src).read()
                guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", inf).group(1).lower()
                base = re.search(r"BASE_NAME\s*=\s*(\S+)", inf).group(1)
                hit = by_guid.get(guid)
                if not hit:
                    missing.append(rel)
                    continue
                _, files = hit
                out = os.path.join(dest, os.path.dirname(rel))
                os.makedirs(out, exist_ok=True)
                # name each file exactly as the INF's [Binaries] lines do (they can differ from BASE_NAME)
                for kind, name in re.findall(r"^\s*(PE32|TE|DXE_DEPEX|PEI_DEPEX)\|([^|\s]+)", inf, re.M):
                    src_f = files.get("depex") if kind.endswith("DEPEX") else (files.get("pe") or files.get("te"))
                    if not src_f:
                        continue
                    if ".patched." in name:
                        patched.append(rel)  # port-patches.py writes the .patched file
                        name = name.replace(".patched", "")
                    shutil.copy(src_f, os.path.join(out, name))
                open(os.path.join(out, f), "w").write(inf)
                made += 1
            elif rel.startswith("RawFiles" + os.sep):
                hit = by_name.get(f)
                if hit and "raw" in hit[1]:
                    os.makedirs(os.path.join(dest, "RawFiles"), exist_ok=True)
                    shutil.copy(hit[1]["raw"], os.path.join(dest, "RawFiles", f))
                    made += 1
                else:
                    missing.append(rel)
    # target-only raw files (its own panels etc.)
    for n, (_, files) in by_name.items():
        if n.endswith((".xml", ".cfg", ".csv", ".bmp")) and "raw" in files:
            p = os.path.join(dest, "RawFiles", n)
            if not os.path.exists(p):
                os.makedirs(os.path.dirname(p), exist_ok=True)
                shutil.copy(files["raw"], p)
                made += 1
    print(f"wrote {made} entries to {dest}")
    print(f"needs patch porting ({len(patched)}):", *patched, sep="\n  ")
    print(f"not in target firmware ({len(missing)}):", *missing, sep="\n  ")


if __name__ == "__main__":
    main(*sys.argv[1:4])
