#!/bin/bash
# boot-local.sh TAG : RAM-only boot of ramroot/out-TAG from this PC (tablet on local USB). Never flashes.
set -eu
T=${1:?tag}; D=/root/unixnu-work/ramroot/out-$T; U=/root/unixnu-work/usb-dev/host/ruan-usbdbg.py
[ -e /dev/ttyACM0 ] && (timeout 30 python3 $U --tty /dev/ttyACM0 reboot-fastboot >/dev/null 2>&1 || true)
( until [ -n "$(fastboot devices)" ]; do adb reboot bootloader 2>/dev/null; sleep 3; done ) &
echo "== waiting for fastboot"; until [ -n "$(fastboot devices)" ]; do sleep 0.5; done
fastboot boot $D/ruan-ramroot-usb-$T.img
[ -f $D/data-seed.apfs ] && { echo "== XNU booting $T (PERSIST: Data seed in the payload, nothing to send)"; exit 0; }
sleep 5; echo "== waiting for DarwinBoot receiver"; until [ -n "$(fastboot devices)" ]; do sleep 0.5; done
S=$D/root-usb.apfs; [ -f $D/data-usb.apfs ] && S=$D/data-usb.apfs   # UFS variant (build-ramroot.sh UFS=1): only the RAM Data goes over USB
fastboot stage $S
fastboot continue
echo "== XNU booting $T"
