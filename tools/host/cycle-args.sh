#!/bin/bash
# cycle-args.sh TAG "extra args": payload-only build (same kernel/root/Data) with args-rbu152p + extra, RAM boot, read display after boot.
T=$1; W=/root/unixnu-work; cd $W
A="$(cat args-current.txt) $2"
RAMROOT=$W/ramroot WORK=$W/ramroot/work-full UFS=1 PERSIST=1 UFS_ROOT=${UFS_ROOT:-$W/tr/out-rbu143root} REBUILD_KERNEL=$KR TAG=$T OUT=$W/tr/out-$T KERNEL_ARGS="$A" "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/unixnu-ruan/scripts/build-ramroot.sh" usb > build-$T.log 2>&1; rc=$?; echo build rc=$rc; [ $rc = 0 ] || exit 1
ln -sfn $W/tr/out-$T ramroot/out-$T
ps -eo pid,args | awk '/watch-long.sh rbu1|^[0-9 ]*python3 [^ ]*ruan-usbdbg/ && !/awk/ {print $1}' | xargs -r kill
./boot-local.sh $T > boot-local-$T.log 2>&1; tail -1 boot-local-$T.log
until [ -e /dev/ttyACM0 ]; do sleep 5; done; sleep 170
mkdir -p logs/$T; U="timeout 60 python3 usb-dev/host/ruan-usbdbg.py --tty /dev/ttyACM0"
$U cmd bootargs > logs/$T/bootargs.txt 2>&1; $U cmd display > logs/$T/display-1.txt 2>&1
grep -a -E "SUBMIT-MS|PACING" logs/$T/display-1.txt | tail -2
