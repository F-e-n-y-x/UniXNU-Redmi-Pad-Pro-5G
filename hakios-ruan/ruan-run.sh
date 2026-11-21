#!/bin/bash
# ruan-run.sh "<boot args>" [secs]: one clean HakiOS run on the tablet (RAM only; never flashes).
# Gets the tablet into its stock fastboot (from Android, the loader or fastboot), boots the ruan
# UEFI + loader image, uploads the system image, runs the kernel, prints the log's path.
set -u
ARGS=${1:-}; SECS=${2:-240}
cd /root/hakios-garnet
fbprod() { timeout 5 fastboot getvar product 2>&1 | sed -n 's/^product: //p'; }
for i in $(seq 1 60); do
	p=$(fbprod)
	if [ "$p" = xnuloader ]; then fastboot reboot >/dev/null 2>&1; sleep 8
	elif [ -n "$p" ]; then break
	elif [ "$(timeout 5 adb -s cf10660 get-state 2>/dev/null)" = device ]; then adb -s cf10660 reboot bootloader; sleep 6
	else sleep 3; fi
done
[ "$(fbprod)" = ruan ] || { echo "ruan-run: no stock fastboot (got: $(fbprod))"; exit 1; }
fastboot boot /tmp/xnu-ruan-id.img >/dev/null 2>&1
for i in $(seq 1 20); do sleep 2; [ "$(fbprod)" = xnuloader ] && break; done
[ "$(fbprod)" = xnuloader ] || { echo "ruan-run: loader did not come up"; exit 1; }
rm -f "${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone.ran"
./xl ramdisk userland/system.apfs 2>&1 | tail -1
XL_CMDLINE="$ARGS" XL_SECS=$SECS XL_FOLLOW= timeout $((SECS + 300)) ./xl run 2>&1 | grep -E "^result|FAILED" | tail -2
