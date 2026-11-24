#!/bin/bash
# Boot ruan's macOS 27.3 RAM root from the host (fastboot).
# RAM-only execution: nothing is written to the tablet's storage.
#
#   ./zima-ramroot-macos.sh         boot macOS RAM root (boot image + USB stage)
#   ./zima-ramroot-macos.sh check   verify SHA256 sums only
#
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

echo "== [1/5] Verifying SHA256 checksums..."
sha256sum -c SHA256SUMS-macos

if [ "${1:-}" = "check" ]; then
    echo "Check passed. Exiting without sending commands."
    exit 0
fi

command -v fastboot >/dev/null || { echo "error: fastboot is not installed" >&2; exit 1; }

echo "== [2/5] Checking connected fastboot device..."
[ "$(fastboot devices | grep -c fastboot)" -ge 1 ] || { echo "error: no fastboot device found (tablet in fastboot?)" >&2; exit 1; }

product=$(fastboot getvar product 2>&1 | sed -n "s/^product: //p" | head -1)
unlocked=$(fastboot getvar unlocked 2>&1 | sed -n "s/^unlocked: //p" | head -1)
echo "Device: product='$product', unlocked='$unlocked'"
[ "$product" = "ruan" ] || { echo "error: device product is '$product', expected 'ruan'" >&2; exit 1; }

img="ruan-ramroot-usb-macos.img"
echo "== [3/5] Booting $img into RAM (fastboot boot)..."
fastboot boot "$img"

echo "== [4/5] Waiting for DarwinBoot USB receiver (unixnu-ramdisk)..."
for i in $(seq 1 60); do
    if fastboot devices 2>&1 | grep -q '^unixnu-ramdisk'; then
        break
    fi
    sleep 2
done

receiver_prod=$(fastboot -s unixnu-ramdisk getvar product 2>&1 | sed -n "s/^product: //p" | head -1)
[ "$receiver_prod" = "unixnu-ramdisk" ] || { echo "error: receiver not responding after 120s" >&2; exit 1; }
echo "Receiver ready: $receiver_prod"

max_size=$(fastboot -s unixnu-ramdisk getvar max-download-size 2>&1 | sed -n "s/^max-download-size: //p" | head -1)
file_size=$(stat -c %s root-usb.apfs)
echo "Receiver expected size: $((max_size)) bytes, root-usb.apfs size: $file_size bytes"
[ "$((max_size))" -eq "$file_size" ] || { echo "error: size mismatch between receiver descriptor and root-usb.apfs" >&2; exit 1; }

echo "== [5/5] Sending root-usb.apfs ($((file_size / 1048576)) MiB) via fastboot stage..."
t0=$(date +%s)
fastboot -s unixnu-ramdisk stage root-usb.apfs
echo "Transferred in $(( $(date +%s) - t0 ))s"

echo "Sending fastboot continue..."
fastboot -s unixnu-ramdisk continue
echo "macOS kernel booting from RAM root (md0=root, md1=data)! Watch serial or framebuffer."
