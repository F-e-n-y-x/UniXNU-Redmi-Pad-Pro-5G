set -e; W=/root/unixnu-work; U=$W/UniXNU; L=$W/logs/macos-full; T="/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/macos/tools"
: data already built at 09:2x
cp $W/macos-build/data-full/root-usb.desc $W/macos-build/data-full/root-usb.split $W/macos-build/full-set/
cd "$T"; SKIP_KERNEL=1 CPUS=8 KERNEL_ARGS="panic_on_crash_disabled=1 vm_compressor=2 ruan-gpu=18 ruan-adreno=1 tu-debug=sysmem,noubwc offline-hid-input=1 offline-hid-home=1 -timesync_bringup ruan-wc-dram=0x80000000 fb-wc=1 touch-pointer=1 ruan_touch=1" UFS_ROOT_UUID=4C15B884-6D41-FA68-15FA-10423D08C1A0 UFS=1 NO_INSTALL=1 EXTRACT=$W/macos-build/full-set OUT=$W/macos-build/out-gpu bash build-macos-ramroot.sh > $L/build-full.log 2>&1 || { tail -8 $L/build-payload.log; exit 1; }
B=$W/macos-build/boot-local; cp $W/macos-build/out-gpu/ruan-ramroot-usb-macos.img $B/; cp $W/macos-build/data-full/root-usb.apfs $B/root-usb.apfs; cd $B; for f in $(awk '{print $2}' SHA256SUMS-macos); do sha256sum $f; done > S.new && mv S.new SHA256SUMS-macos
pkill -f "^/bin/bash ./live-log" || true; pkill -f "^python3 -u ruan-usbdbg.py --tty /dev/ttyACM0 log" || true; sleep 2
[ -e /dev/ttyACM0 ] && { timeout 20 python3 -u ruan-usbdbg.py --tty /dev/ttyACM0 reboot-fastboot 2>&1 | tail -1 || true; }
until fastboot devices | grep -q .; do sleep 2; done; sleep 2
R="/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/tools/host/rec-session.sh"; bash "$R" stop --prune || true; bash "$R" start smp8
rm -f live-smp8.log; (setsid nohup ./live-log.sh smp8 >/dev/null 2>&1 &); ./zima-ramroot-macos.sh 2>&1 | tail -1
s=$(date +%s); until grep -aqE "exit pid [0-9]+ usermanagerd|exec: pid [0-9]+ (Dock|Finder)$" live-smp8.log 2>/dev/null || [ $(( $(date +%s) - s )) -gt 1800 ]; do sleep 10; done
sleep 60; date +%T; grep -a "^\[hb\]" live-smp8.log | tail -1 | cut -c1-90
grep -aoE "exec: pid [0-9]+ (usermanagerd|Dock|Finder|SystemUIServer|ControlCenter)$|exit pid [0-9]+ (usermanagerd|WindowServer|loginwindow)[^|]{0,30}" live-smp8.log | awk '!s[$0]++' | head -12
pkill -f "^/bin/bash ./live-log" || true; pkill -f "^python3 -u ruan-usbdbg.py --tty /dev/ttyACM0 log" || true; sleep 2
timeout 200 python3 -u ruan-usbdbg.py --tty /dev/ttyACM0 fullshot -o "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/testing/fullshot-smp8.png" 2>&1 | tail -1
(setsid nohup ./live-log.sh smp8 >/dev/null 2>&1 &)
python3 -c "
from PIL import Image; im=Image.open('/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/testing/fullshot-smp8.png'); px=im.resize((160,256)).getdata(); print('non-black samples', sum(1 for p in px if sum(p)>30))"