#!/bin/bash
# cycle.sh TAG : package the current kernel as out-TAG (root reused from out-$ROOT_FROM, default rbu31; rbu31 boot args), copy to the Pi,
# send the running tablet to fastboot over USB debug, and boot it RAM-only. Never flashes.
# ROOT_FROM=<tag>: reuse out-<tag>'s root (e.g. rbu44, the Turnip smoke root); the first cycle with it copies that root to the Pi.
# trust.bundle is the payload's own (build-ramroot.sh, plus the root's root-usb.cdhash module) and goes to the Pi with the image.
set -eu
T=${1:?tag}; R=/root/unixnu-work/ramroot; P="sshpass -p 2911 ssh -o StrictHostKeyChecking=no root@192.168.11.5"
RF=${ROOT_FROM:-rbu31}; RFILES="root-usb.apfs root-usb.desc root-usb.split"
mkdir -p $R/out-$T; for f in $RFILES root-usb.cdhash; do [ ! -f $R/out-$RF/$f ] || [ $RF = $T ] || ln -f $R/out-$RF/$f $R/out-$T/; done
ARGS=$(grep -o 'kernel args: .*' $R/out-rbu31/payload-usb-rbu31.log | sed -e 's/^kernel args: rd=md0 //' -e 's/ ram-data-split=.*//')
for d in ${DROP_ARGS:-}; do ARGS=$(echo " $ARGS " | sed "s| $d | |"); done
ARGS="$ARGS ${EXTRA_ARGS:-}"
rm -f $R/out-$T/ruan-ramroot-usb-$T.img
(cd /root/unixnu-work/UniXNU-ruan && REBUILD_KERNEL=${REBUILD_KERNEL:-} OUT=$R/out-$T TAG=$T DATA_CONTAINER=1 RAMBLOCK=1 KERNEL_ARGS="$ARGS" scripts/build-ramroot.sh usb > $R/out-$T/build.log 2>&1)
diff <(grep -o 'kernel args: .*' $R/out-rbu31/payload-usb-rbu31.log) <(grep -o 'kernel args: .*' $R/out-$T/payload-usb-$T.log) || true
# The Pi keeps each root once (out-<tag>); the boot script's sha256sum -c of SHA256SUMS still checks the copy.
if [ "$($P "stat -c %s ~/ramroot/out-$RF/root-usb.apfs 2>/dev/null || echo 0")" != "$(stat -c %s $R/out-$RF/root-usb.apfs)" ]; then
	echo "copying the $RF root to the Pi"; $P "mkdir -p ~/ramroot/out-$RF"
	(cd $R/out-$RF && sshpass -p 2911 scp -o StrictHostKeyChecking=no $RFILES root@192.168.11.5:/root/ramroot/out-$RF/)
fi
sshpass -p 2911 scp -o StrictHostKeyChecking=no $R/out-$T/ruan-ramroot-usb-$T.img root@192.168.11.5:/root/ramroot/
sshpass -p 2911 scp -o StrictHostKeyChecking=no $R/out-$T/trust.bundle root@192.168.11.5:/root/ramroot/trust-$T.bundle
$P "cd ~/ramroot && mkdir -p out-$T && ln -f out-$RF/root-usb.{apfs,desc,split} out-$T/ && mv trust-$T.bundle out-$T/trust.bundle && mv ruan-ramroot-usb-$T.img out-$T/ && sed 's/rbu31/$T/g' boot-rbu31.sh > boot-$T.sh && chmod +x boot-$T.sh && (timeout 30 python3 ruan-usbdbg.py reboot-fastboot >/dev/null 2>&1 || timeout 10 adb reboot bootloader || true) && (setsid nohup ./boot-$T.sh > boot-$T.log 2>&1 < /dev/null &)"
echo "booting $T"
