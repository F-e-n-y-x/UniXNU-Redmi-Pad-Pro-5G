#!/bin/bash
# live-log.sh TAG: stream the macOS kernel console over USB into live-TAG.log for every boot; the client is killed when the
# USB device goes away (a reset), so the next boot reconnects.
cd /root/unixnu-work/macos-build/boot-local
while true; do
  until [ -e /dev/ttyACM0 ]; do sleep 1; done
  echo "=== $(date +%T) console up" >> live-$1.log
  python3 -u ruan-usbdbg.py --tty /dev/ttyACM0 log >> live-$1.log 2>&1 &
  c=$!
  while [ -e /dev/ttyACM0 ] && kill -0 $c 2>/dev/null; do sleep 1; done
  kill $c 2>/dev/null; wait $c 2>/dev/null
  echo "=== $(date +%T) console gone" >> live-$1.log
  sleep 2
done
