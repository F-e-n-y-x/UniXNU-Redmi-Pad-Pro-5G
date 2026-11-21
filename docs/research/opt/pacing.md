# backboardd frame pacing on ruan (2026-11-21)

Code reading and log analysis only. Nothing was built or booted. Source: staged `NativeLegacyDisplay.cpp` (all ruan `gopfb-*.patch` applied), line numbers below refer to that file. Logs: `/root/unixnu-work/logs/rbu130u/`. rbu130u args: `vsync=120 dpu-scanout=2 fb-wc=1`, and no `vsync-fixed`.

## Cause

1. **The ~5 s idle cadence comes from a crash loop, not from the display.** SUBMIT-MS gaps are exact multiples of 5 s (5000, 5011, 15014, 25033), and slot345 and timeouts both stay at 0. The swap timestamps match CollectionsPoster corpses in `dmesg-5.txt`. Swaps at 572.69, 587.71 and 602.72 s line up with poster crashes at 572.7 and 587.7 s, and there are 59 respawns, one every ~5 s. Each respawn redraws the wallpaper. When nothing changes, backboardd correctly sends no frames. The display side is fast: SWAPS shows validate→flip→latch→retire in 5-13 ms (`display-5.txt`).
2. **33-50 ms while animating means CA runs at 24 Hz, rounded to a 60 Hz latch.** `getRefreshAttributes` (594-602) publishes a minimum interval of 1/120 s. Without `vsync-fixed=1` it keeps the family's maximum interval of 1/24 s. CA is therefore free to drive animations that request no rate at 24 Hz. hakios-sm7435 saw the same thing on this SoC ("animations that ask for nothing ran at 24 fps", `bootargs.md:106`). The panel really runs at 60 Hz (INTF1 counter). A 41.7 ms cadence latched at 16.7 ms steps gives alternating 33 and 50 ms frames, which is exactly what was measured. It also means the 120 Hz claim is wrong for this panel.
3. **Every frame is serialized behind the latch.** In direct scanout the swap retires only in `latchTick` (430-432 → `finishTransfer`). Selector 6 is non-blocking Busy (e00002d5) followed by a blocking wait (display-fps-plan §2). So the next render can only start after the panel's vblank: frame time = render + up to 16.7 ms.
4. Vsync delivery does not matter here: selector 72 is never called, so `native_legacy_vsync_notify` (465) has no client. Phase-aligning the tick to INTF1 would change nothing until a client registers. Selector 6 Busy is the family's correct reply to a noBlock poll, so nothing needs fixing there.

## Fix (`gopfb-zzzzz-ruan-pacing.patch`)

- **One fixed 60 Hz rate.** `vsync=60 vsync-fixed=1` now publishes IOMFBDisplayRefresh at 60 Hz with min = max (`registerService` 586, `getRefreshAttributes` 599). Before, 60 Hz was never published. Boot-args only, and existing args behave the same.
- **Retire at flip, opt-in with `dpu-retire-flip=1`** (flip branch at 712-718, `latchTick` 432). The swap completes right after the VIG0 flip, so CA renders N+1 while N waits for the latch. `transferred` stays set until the latch, so a pending flip is never overwritten. If the family refuses the retire, the old behavior (retire at latch) applies. This is the same idea as picasso PR 19 (31 → 46 fps).
- Upstream e711666 poll cadence: latch polled every 200 µs, fence every 100 µs.

## Risks

- `dpu-retire-flip=1` releases the frame that is still being scanned out until the latch. If CA reuses that buffer immediately, the next frame can tear for ≤16 ms. Default is off.
- If CA treats swap completion as "on screen", its present timestamps shift up to 1 frame early.
- With `vsync-fixed=1`, ProMotion-style rate changes are disabled. That is correct for this 60 Hz panel.
- Not compiled. `patch --dry-run` applies cleanly to the staged file.

## Measure on device

- Args A: `vsync=60 vsync-fixed=1`. Then B: A + `dpu-retire-flip=1`.
- `display`: SUBMIT-MS during a swipe or app open. Target is 16-17 ms. Under A, 33-50 → ~16-33 ms means the 24 Hz cap was the cause.
- VSYNC tick ≈ 60/s. ioreg must show `IOMFBDisplayRefresh` on NativeLegacyDisplay.
- PACING: slot345, timeouts and retire_refused stay 0. Selector 6 Busy ratio should drop under B.
- Camera: watch for tearing during fast scrolls with B.
- Separately: fix the CollectionsPoster crash loop (sandbox denies `NativeLegacyDisplay` properties `IOMFBIntDcpUsedForExtWhenLidClose` and `GainMapVersion`, plus `canvas-width/height`). It wastes CPU and a redraw every 5 s.
