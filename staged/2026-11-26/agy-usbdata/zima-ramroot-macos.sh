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

if [ "${FULL_STAGE:-0}" = "1" ]; then
    used_bytes=$file_size
elif [ -f root-usb.used ]; then
    used_bytes=$(cat root-usb.used)
else
    used_bytes=$(python3 -c '
import os, sys
p = sys.argv[1]
st = os.stat(p)
bs = 4096; cs = 16 * 1024 * 1024
zc = bytes(cs); zb = bytes(bs)
with open(p, "rb") as f:
    pos = st.st_size; last_nz = 0
    while pos > 0:
        take = min(pos, cs)
        pos -= take
        f.seek(pos)
        buf = f.read(take)
        if (take == cs and buf != zc) or (take < cs and buf != zc[:take]):
            for b_off in range((take // bs) - 1, -1, -1):
                if buf[b_off * bs : (b_off + 1) * bs] != zb:
                    last_nz = pos + (b_off + 1) * bs
                    break
            break
print(last_nz if last_nz > 0 else st.st_size)
' root-usb.apfs)
fi

stage_file="root-usb.apfs"
tmp_stage=""
if [ "$used_bytes" -lt "$file_size" ]; then
    tmp_stage=$(mktemp --tmpdir root-usb-stage.XXXXXX.apfs)
    trap 'rm -f "$tmp_stage"' EXIT INT TERM
    head -c "$used_bytes" root-usb.apfs > "$tmp_stage"
    stage_file="$tmp_stage"
    echo "== [5/5] Sending used prefix ($((used_bytes / 1048576)) / $((file_size / 1048576)) MiB) via fastboot stage..."
else
    echo "== [5/5] Sending root-usb.apfs ($((file_size / 1048576)) MiB) via fastboot stage..."
fi

t0=$(date +%s)
fastboot -s unixnu-ramdisk stage "$stage_file"
echo "Transferred in $(( $(date +%s) - t0 ))s"
[ -n "$tmp_stage" ] && rm -f "$tmp_stage"

echo "Sending fastboot continue..."
fastboot -s unixnu-ramdisk continue
echo "macOS kernel booting from RAM root (md0=root, md1=data)! Watch serial or framebuffer."
