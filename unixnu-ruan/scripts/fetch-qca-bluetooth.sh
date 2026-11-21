#!/bin/bash
# QcaBluetooth (Linux btqca.c + GENI SE DMA + H4/IBS + the UART.SK kernel control) as filipnavara/UniXNU-asteroids vendors it for the
# WCN6750 (drivers/QcaBluetooth, sheng's 986d631 plus QBT_SOC_TYPE), into $DEVICE/.cache/QcaBluetooth (git-ignored, as Ath11kWifi).
# The same asteroids clone and pin as scripts/fetch-ath11k-wifi.sh. Its tools/prepare-linux.py then fetches and hash-checks Linux
# v6.18.60 btqca.c/btqca.h from GitHub into out/linux.
#
#   scripts/fetch-qca-bluetooth.sh [DEST]     (network: github.com only)
set -euo pipefail
DEVICE=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$DEVICE/.cache/QcaBluetooth}
PIN=${QCA_BLUETOOTH_REV:-9f07af3b0bb431a01de12ab92f531619a7104083}
SRC=$DEVICE/.cache/UniXNU-asteroids
[ -d "$SRC/.git" ] || git clone -q --filter=blob:none --no-checkout https://github.com/filipnavara/UniXNU-asteroids.git "$SRC"
git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin "$PIN"
rm -rf "$DEST"; mkdir -p "$DEST"
git -C "$SRC" archive "$PIN" drivers/QcaBluetooth | tar -x -C "$DEST" --strip-components=2
python3 -I "$DEST/tools/prepare-linux.py"
echo "$DEST (filipnavara/UniXNU-asteroids $PIN drivers/QcaBluetooth, btqca v6.18.60)"
