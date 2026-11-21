#!/bin/bash
# The WiFi module (Linux ath11k + mac80211 + cfg80211 on UniXNU's Ath12kWifi runtime, plus wpa_supplicant 2.12's -Dsheng backend):
# drivers/Ath11kWifi of filipnavara/UniXNU-asteroids at the pinned commit, into $DEVICE/.cache/Ath11kWifi (git-ignored: macOS/.gitignore
# ".cache/"). It is not in UniXNU's component catalog (components.toml has Ath12kWifi only), so the board fetches it the way
# make-ufs-board.sh fetches ShengUfs. The module's own tools then fetch its pinned Linux and hostap sources (build-ath11k-wifi.py runs them).
#
# One ruan change on top: tools/build-supplicant.py targets arm64e iOS 27 (asteroids); ruan's userland is iPadOS 17.7.11 on an
# arm64 (A10X) identity, so the target comes from NW_IOS_TARGET (build-wifi-userspace.py sets arm64-apple-ios17.0).
#
#   scripts/fetch-ath11k-wifi.sh [DEST]     (network: github.com only)
set -euo pipefail
DEVICE=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$DEVICE/.cache/Ath11kWifi}
PIN=${ATH11K_WIFI_REV:-9f07af3b0bb431a01de12ab92f531619a7104083}
SRC=$DEVICE/.cache/UniXNU-asteroids
[ -d "$SRC/.git" ] || git clone -q --filter=blob:none --no-checkout https://github.com/filipnavara/UniXNU-asteroids.git "$SRC"
git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin "$PIN"
rm -rf "$DEST"; mkdir -p "$DEST"
git -C "$SRC" archive "$PIN" drivers/Ath11kWifi | tar -x -C "$DEST" --strip-components=2
f=$DEST/tools/build-supplicant.py
grep -q '"arm64e-apple-ios27.0" if args.target == "ios"' "$f" || { echo "$f changed: re-check the iOS target line" >&2; exit 1; }
sed -i 's|"arm64e-apple-ios27.0" if args.target == "ios"|os.environ.get("NW_IOS_TARGET", "arm64e-apple-ios27.0") if args.target == "ios"|; s|^import argparse$|import argparse\nimport os|' "$f"
grep -q 'NW_IOS_TARGET' "$f" && grep -q '^import os$' "$f"
# tools/fetch-sources.py clones each locked source at its branch head and refuses it when the branch has moved past the lock (freebsd
# releng/15.1 had by 2026-11-21): fetch the locked commit itself in that case.
g=$DEST/tools/fetch-sources.py
old='    revision = git(directory, "rev-parse", "HEAD")\n'
grep -qF "$(printf '%b' "$old" | head -1)" "$g" || { echo "$g changed: re-check the locked-commit fetch" >&2; exit 1; }
python3 - "$g" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '    revision = git(directory, "rev-parse", "HEAD")\n'
new = ('    if git(directory, "rev-parse", "HEAD") != source["commit"] and not check_only:   # ruan: the branch moved past the lock\n'
       '        subprocess.run(["git", "-C", str(directory), "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", source["commit"]], check=True)\n'
       '        subprocess.run(["git", "-C", str(directory), "checkout", "-q", source["commit"]], check=True)\n' + old)
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, new))
EOF
echo "$DEST (filipnavara/UniXNU-asteroids $PIN drivers/Ath11kWifi, NW_IOS_TARGET patch)"
