#!/bin/bash
# Boot ruan's iPadOS RAM root from the host the tablet is plugged into (zima: Debian, fastboot). RAM only: the only
# commands sent are `fastboot boot` to the stock bootloader and getvar/stage/continue to DarwinBoot's USB receiver,
# which has no command that writes storage. Nothing is flashed, erased or set active.
#
#   zima-ramroot.sh mini [DIR]   boot ruan-ramroot-mini.img (root inside the payload)
#   zima-ramroot.sh usb  [DIR]   boot ruan-ramroot-usb.img, then send root-usb.apfs to the receiver and continue
#   zima-ramroot.sh log  [DIR]   boot ruan-ramroot-usb.img, fetch the log region only, then `fastboot reboot` (to Android)
#
# usb and log first ask the receiver for the ramoops region (`fastboot get_staged`, a DarwinBoot with "upload":
# core patch 10-DarwinBoot): logs/<time>-prev.bin, parsed by read-ramoops.py next to this script when it is there. It holds
# Android's last kernel log, or the previous XNU run's with ruan-ramoops=1. A receiver without "upload" just says FAIL.
# TAG=x uses ruan-ramroot-<mode>-x.img and SHA256SUMS-<mode>-x (log mode: the usb image). Keep each build's files in a
# directory of its own: every usb build names its root root-usb.apfs.
#
# DIR holds the files of build-ramroot.sh's output (images, SHA256SUMS-<mode>); default: this script's directory.
# The tablet must already be in its stock fastboot (Volume down + power, or `adb reboot bootloader` once the other
# session agreed to a reboot). SERIAL=<fastboot serial> picks one device when more are attached.
# AFTER=log|usb (usb mode, with an image built with ruan-autoreset=<s>): after 'continue', wait up to AFTER_LIMIT seconds
# (default 1200) for the tablet to restart into stock fastboot by itself, then run this script again in that mode: log fetches
# the run's ramoops log (and reboots to Android), usb fetches it and boots the same test again.
set -euo pipefail
MODE=${1:?usage: zima-ramroot.sh mini|usb [DIR]}
DIR=${2:-$(cd "$(dirname "$0")" && pwd)}
case $MODE in mini|usb|log) ;; *) echo "zima-ramroot: mode is mini, usb or log" >&2; exit 2 ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"
command -v fastboot >/dev/null || { echo "zima-ramroot: fastboot is not installed" >&2; exit 1; }

echo "== checking the files"
IMODE=$MODE; [ $MODE = log ] && IMODE=usb
NAME=$IMODE${TAG:+-$TAG}
sha256sum -c "SHA256SUMS-$NAME"
img=ruan-ramroot-$NAME.img
sel=(); [ -n "${SERIAL:-}" ] && sel=(-s "$SERIAL")
fb() { timeout "${FB_TIMEOUT:-15}" fastboot "${sel[@]}" "$@" 2>&1; }
# shortcut: stock fastboot sometimes answers a getvar right after another with nothing (2026-11-14); 3 tries, 1 s apart.
var() { local v; for _ in 1 2 3; do v=$(fb getvar "$1" | sed -n "s/^$1: //p" | head -1); [ -n "$v" ] && break; sleep 1; done; echo "$v"; }

echo "== stock fastboot"
[ "$(fastboot devices | grep -c fastboot)" -ge 1 ] || { echo "zima-ramroot: no fastboot device (tablet in its bootloader?)" >&2; exit 1; }
[ -n "${SERIAL:-}" ] || [ "$(fastboot devices | grep -c fastboot)" -eq 1 ] || { echo "zima-ramroot: several devices; set SERIAL" >&2; exit 1; }
product=$(var product); unlocked=$(var unlocked)
[ "$product" = ruan ] || { echo "zima-ramroot: product is '$product', not ruan" >&2; exit 1; }
[ "$unlocked" = yes ] || { echo "zima-ramroot: bootloader not unlocked ('$unlocked')" >&2; exit 1; }
echo "ruan, unlocked; serial $(fastboot devices | awk '/fastboot/{print $1; exit}')"

echo "== fastboot boot $img (RAM only)"
FB_TIMEOUT=60 fb boot "$img" | tail -2
if [ $MODE = mini ]; then
	echo "booted. Watch the screen: past 'Failed to mount root device', XNU should log 'BSD root: md0' and run"
	echo "/sbin/launchd; dyld then stops for want of its shared cache (that image carries none)."
	exit 0
fi

echo "== waiting for DarwinBoot's receiver (UniXNU menu 5 s, then 'USB RAMDISK: WAITING FOR fastboot stage')"
sel=(-s unixnu-ramdisk)
for i in $(seq 1 60); do
	fastboot devices | grep -q '^unixnu-ramdisk' && break
	sleep 2
done
[ "$(var product)" = unixnu-ramdisk ] || { echo "zima-ramroot: no receiver after 120 s; read the tablet's screen" >&2; exit 1; }
mkdir -p logs; prev=logs/$(date +%Y%m%d-%H%M%S)-prev.bin
echo "== fetching the ramoops region (fastboot get_staged) -> $prev"
if fb get_staged "$prev" | tail -2 && [ -s "$prev" ]; then
	[ -f "$HERE/read-ramoops.py" ] && python3 "$HERE/read-ramoops.py" "$prev" || true
else
	rm -f "$prev"; echo "(no log: this receiver has no upload)"
fi
if [ $MODE = log ]; then
	fb reboot | tail -1
	echo "rebooted (DarwinBoot resets cold): Android should come back."
	exit 0
fi
max=$(var max-download-size); size=$(stat -c %s root-usb.apfs)
[ "$((max))" -eq "$size" ] || { echo "zima-ramroot: receiver wants $((max)) bytes, root-usb.apfs has $size: image and boot image differ" >&2; exit 1; }
echo "== sending root-usb.apfs ($((size / 1048576)) MiB); the tablet shows the progress"
t0=$(date +%s)
FB_TIMEOUT=1800 fb stage root-usb.apfs | tail -3
echo "sent in $(( $(date +%s) - t0 )) s"
fb continue | tail -2
echo "booting. The receiver checked the image's CRC32 before it accepted 'continue'."
[ -n "${AFTER:-}" ] || exit 0
echo "== waiting up to ${AFTER_LIMIT:-1200} s for the tablet's own restart into fastboot (ruan-autoreset)"
end=$((SECONDS + ${AFTER_LIMIT:-1200}))
until fastboot devices | grep -v '^unixnu-ramdisk' | grep -q fastboot; do
	(( SECONDS < end )) || { echo "zima-ramroot: no fastboot after ${AFTER_LIMIT:-1200} s: enter it by hand (Volume down + power)" >&2; exit 1; }
	sleep 5
done
echo "back in fastboot after $((SECONDS + ${AFTER_LIMIT:-1200} - end)) s"
sleep 3
exec env AFTER= bash "$HERE/$(basename "$0")" "$AFTER" "$DIR"
