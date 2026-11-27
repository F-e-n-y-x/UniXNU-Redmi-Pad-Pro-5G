#!/bin/bash
# Make uefi/Resources/DTBs/ruan.dtb from YOUR tablet's live device tree (never shipped: it holds Xiaomi/Qualcomm data).
#   adb shell su -c 'cat /sys/firmware/fdt' > ruan-live.dtb      (rooted Android)
#   tools/make-uefi-dtb.sh ruan-live.dtb                          (writes uefi/Resources/DTBs/ruan.dtb)
# It drops what Android's bootloader added at that boot: /chosen bootargs and the initrd range. Needs fdtput (dtc package).
set -euo pipefail
in=${1:?usage: make-uefi-dtb.sh <live.dtb> [out.dtb]}
out=${2:-$(cd "$(dirname "$0")/.." && pwd)/uefi/Resources/DTBs/ruan.dtb}
command -v fdtput >/dev/null || { echo "fdtput missing: install the dtc / device-tree-compiler package"; exit 1; }
mkdir -p "$(dirname "$out")"; cp "$in" "$out"
for p in bootargs linux,initrd-start linux,initrd-end; do fdtput -d "$out" /chosen "$p" 2>/dev/null || true; done
fdtget "$out" / model >/dev/null && echo "wrote $out"
