# Live rotation (USB `rotate`), 2026-11-26. Offline: nothing built, nothing run on the tablet.

## Files
- `rotate-live-display.patch`: UniXNU-macos (`git apply`, checked on top of today's fb-rotate working tree).
  NativeLegacyDisplay.cpp + NativeLegacyRequest.h.
- `rotate-live-touch-usb.patch`: UniXNU-ruan-macos (`patch -p1`, dry run OK). RuanTouch.c + UsbDebugCdc.cpp.
- `turn-test.c`: host self-test (`cc -I b turn-test.c && ./a.out`): turn180(rotate 1 copy) == rotate 3 copy, odd heights, back again. Passes.

## What changed
- `extern "C" volatile uint32_t native_legacy_rotate` (display): set from fb-rotate in startHardware, changed by USB.
  The copy path still reads the member `rotate`; both change together on the display's work loop, so no copy runs half-way.
- `native_legacy_set_rotate(r, live)` runs `setRotate` under `getWorkLoop()->runAction`:
  - 1<->3 (booted with fb-rotate=1|3): `native_legacy_turn180` turns the scanout picture 180 deg in place, so a still screen is
    right at once (no WindowServer frame needed); the next swap is copied with the new angle (the rotated copy is always full).
    macOS sees no change (same 2560x1600).
  - 0<->1/3: refused (`-3`) unless `live` is given. With `live`: hotplug 0, direct scanout back to boot memory, new rotate,
    `installBootDimensions` with swapped size, `publishDisplayAttributes` (new TimingElements/NativeFormat*), DisplayWidth/Height,
    black fill, hotplug 1. Refused while a GPU blit or DPU flip is in flight. The direct DPU path now also checks `!rotate`.
- RuanTouch: one `touch_orient()` for both report paths; reads `native_legacy_rotate` per report (boot-arg fb-rotate if the
  display is not linked). touch_swap/touch_flip still apply on top, as before.
- USB: `rotate` (show), `rotate 1|3`, `rotate 0|1|3 live`. Answer: `[usb] ROTATE: <result> now=<n>`.

## Use
    ruan-usbdbg.py cmd rotate            # now=1
    ruan-usbdbg.py --wait 5 cmd rotate 3 # 90 -> 270
    ruan-usbdbg.py --wait 5 cmd rotate 0 live   # experimental

## Verify over USB
1. Boot fb-rotate=1 (unchanged default behaviour: same picture/touch as today). `cmd rotate` -> `now=1`.
2. `cmd rotate 3`: `done now=3`; the picture turns 180 at once; `fullshot` still landscape and correct; tap a Dock icon:
   touch lands under the finger. `cmd rotate 1` back. `cmd rotate 0` -> "needs 'live'"; `cmd rotate 2` -> bad value.
3. `cmd display`: copy timing unchanged; no BadArgument transfers after the turn.
4. Experimental: `cmd rotate 0 live`. Watch WindowServer: does it re-read the mode (fullshot portrait 1600x2560) or keep a
   virtual display/hang? If it hangs or crashes, Part 2 is not live-feasible this way; use the fallback.

## Part 2 fallback (persistent rotation)
- The ramoops cell does not work as storage: IPADOS-BRINGUP.md "RAM does not survive this reset (reads back 0xFF)".
- A file on Data is mounted long after startHardware has fixed the size, so the next boot cannot read it in time.
- The only store the next boot reads before the display starts is the boot-args, which are built on the host:
  `sed -i 's/ fb-rotate=[0-9]//; s/$/ fb-rotate=1/' /root/unixnu-work/args-current.txt` before cycle-args.sh/cycle-root.sh
  (cmdline is at ~987/1000 bytes: drop another arg if needed).

## Known limits
- After a live 0<->1/3 switch the family's default surface and the IODisplay EDID keep the boot size.
- Half-float (EDR) surfaces are still refused while rotated.
