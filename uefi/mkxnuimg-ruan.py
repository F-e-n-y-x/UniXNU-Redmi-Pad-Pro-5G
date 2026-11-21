#!/usr/bin/env python3
"""ruan's version of HakiOS's mkxnuimg.py: an Android boot image for `fastboot boot` holding
  kernel = gzip(BootShim + UEFI FD + ["XNUP" payload]) + ruan DTB     (boot header v4, as ruan's ABL took Mu-ruan.img)
The shim copies the FD to FD_BASE and the payload to PAYLOAD_BASE; the patched Boot Manager (XnuLoader.c) starts the
loader from there. SM7435 keeps 0xEA000000-0xF1400000 for TZ apps, so the payload sits at 0xF4000000 (as garnet's).

usage: mkxnuimg-ruan.py [-p usb-payload.bin] [-o out.img]   (payload: hakios xnuloader/mkpayload.py ... --usb)
"""
import argparse
import gzip
import os
import subprocess
import sys
import tomllib

MU = os.environ.get("MU", "/DATA/tower/Projects/mu-silicium-ruan")
PAYLOAD_BASE, PAYLOAD_MAX = 0xF4000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--payload")
ap.add_argument("-o", "--output", default="xnu-ruan.img")
args = ap.parse_args()

cfg = tomllib.load(open(os.path.join(MU, "Resources/Configs/ruan.toml"), "rb"))
fd_base, fd_size = cfg["uefi_fd"]["base"], cfg["uefi_fd"]["size"]
fd_path = os.environ.get("FD") or os.path.join(MU, "Build/ruanPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd")
shim_dir = os.path.join(MU, "BootShim")

for f in ("BootShim.elf", "BootShim.bin"):
    try:
        os.unlink(os.path.join(shim_dir, f))
    except FileNotFoundError:
        pass
subprocess.check_call(["make", f"REQUIRES_KERNEL_HEADER={int(cfg['boot_shim'].get('requires_kernel_header', False))}",
                       f"FD_BASE={fd_base:#x}", f"FD_SIZE={fd_size:#x}",
                       f"CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}"],
                      cwd=shim_dir, stdout=subprocess.DEVNULL)

shim = open(os.path.join(shim_dir, "BootShim.bin"), "rb").read()
fd = open(fd_path, "rb").read()
assert len(fd) == fd_size, f"FD is {len(fd):#x}, toml says {fd_size:#x}"
blob = shim + fd
if args.payload:
    data = open(args.payload, "rb").read()
    assert data[:4] == b"XNUP" and len(data) <= PAYLOAD_MAX - 0x1000, "bad or oversized payload"
    blob += data + b"\0" * (-(len(blob) + len(data)) % 16)
    print(f"payload: {len(data)} bytes at {PAYLOAD_BASE:#x}")

dtb = open(os.path.join(MU, "Resources/DTBs/ruan.dtb"), "rb").read()
kernel = args.output + ".kernel~"
open(kernel, "wb").write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, os.path.join(MU, "Resources/Scripts/mkbootimg.py"),
                           "--kernel", kernel, "--ramdisk", os.path.join(MU, "Resources/ramdisk"),
                           "--header_version", str(cfg["boot_image"]["header_version"]), "-o", args.output])
finally:
    os.unlink(kernel)
print(f"{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})")
