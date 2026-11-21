#!/bin/bash
# The iris VPU driver (Linux iris core/HFI gen2/sessions/buffers/SMMU domain, GPL-2.0-only; the decoder device and VideoToolbox host in
# kernel/) from UniXNU/UniXNU-sheng drivers/QcomIris at the pinned commit, into $DEVICE/.cache/QcomIris (git-ignored: ".cache/"), plus
# patches/qcom-iris-ruan.patch (ruan's iris2 one-pipe power-off halves, the UBWC bank bit as a build define, a host test). Then the
# driver's own tools/fetch-sources.py fetches its pinned Linux files (iris_vpu_buffer.c etc., kernel.org) on first use.
# The pin is the last QcomIris commit of UniXNU-sheng main (2026-11-16); sheng's device evidence is 9252d988, which this extends
# (log-video, the allocation fixes). Licence headers stay as they are: src/, include/, shim/, kernel/IrisMedia.* GPL-2.0-only.
#
#   scripts/fetch-qcom-iris.sh [DEST]     (network: github.com only)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)                    # docs/research/video/port (or the board's scripts/.. once copied)
DEVICE=${UNIXNU_DEVICE:-$(cd "$HERE/../../../.." 2>/dev/null && pwd)/unixnu-ruan}
[ -d "$DEVICE/package" ] || DEVICE=$HERE
DEST=${1:-$DEVICE/.cache/QcomIris}
PIN=${QCOM_IRIS_REV:-2359a862eb0e79e6d5c920d44b6704a6960d4f81}
PATCH=${QCOM_IRIS_PATCH:-$HERE/patches/qcom-iris-ruan.patch}
[ -f "$PATCH" ] || PATCH=$DEVICE/patches/qcom-iris-ruan.patch
SRC=$(dirname "$DEST")/UniXNU-sheng
[ -d "$SRC/.git" ] || git clone -q --filter=blob:none --no-checkout https://github.com/UniXNU/UniXNU-sheng.git "$SRC"
git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin "$PIN"
rm -rf "$DEST"; mkdir -p "$DEST"
git -C "$SRC" archive "$PIN" drivers/QcomIris | tar -x -C "$DEST" --strip-components=2
patch -s -p1 -d "$DEST" < "$PATCH"
grep -q 'bool vpu2;' "$DEST/include/QcomIrisCore.h"
echo "$DEST (UniXNU/UniXNU-sheng $PIN drivers/QcomIris + $(basename "$PATCH"))"
