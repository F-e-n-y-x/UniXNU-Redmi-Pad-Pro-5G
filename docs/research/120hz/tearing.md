# 120 Hz: tear lines with zero-copy scanout (rbu182u photo 20261121_175712.jpg)

2026-11-21. Code written and compile-checked, not run. [V] = read in source or measured, [I] = inferred.
Sources: LineageOS `android_kernel_xiaomi_sm7435-modules` lineage-24.0, `display-drivers/msm/sde/` `sde_encoder_phys_vid.c`,
`sde_hw_ctl.c`, `sde_hw_intf.c`, `sde_hw_sspp.c`.

## 1. Does a flip latch at vsync or right away?

- At the next frame boundary. Android's video-mode kickoff writes CTL_FLUSH at any time, with no line-count wait. Its vblank IRQ
  (`sde_encoder_phys_vid_vblank_irq`) counts the frame as latched only once `get_flush_register()` reads 0 ("hw accepted the
  flush bits"), and only then signals the retire/release fences [V]. So the SSPP SRC0_ADDR/YSTRIDE are double-buffered and a
  flush cannot change a scan that has already started [V design, I for this pipe].
- CTL_START is for command mode only. Video mode needs no "frame trigger" [I: `sde_encoder_phys_vid.c` never calls trigger_start; its ops set only `trigger_flush`].
  The only line-count waits in the video encoder are `poll_for_active_region` (when the timing engine is first enabled) and
  `wait_for_active` (an op the encoder core calls; sde_encoder.c not read). Neither sits in the per-frame flush path [V for poll, I for wait_for_active].
- The latch point is either prog-fetch start (line 2733) or vsync (line 2758 = 0). Not known which; both come after the active area
  ends at line 2731 [I].
- So the register write alone does not tear. What does: `dpu-retire-flip=1` is a **built-in default** (`patches/xnu/ruan-default-bootargs.patch`).
  It retires swap N at the flip, which releases frame N-1's buffer while N-1 is still being scanned until the latch (up to a full
  8.3 ms at 120 Hz, wherever the flip lands). The compositor then renders into a buffer that is still on screen. pacing.md already
  warned this could tear [I, most likely cause]. At 60 Hz the render cadence is half as fast, so the reuse usually lands after the latch [I].
- Caveat [I]: in the photo, several dark lines stop exactly at the split-view divider (x≈720). If the tearing were only in scanout,
  each line would cross the full panel width. Part of it may be compositor/app buffer reuse inside CA. If the patch below doesn't
  clear it, test there next.

## 2. Fix: `gopfb-zzzzzzz-ruan-flip-sync.patch` (both package copies)

`dpu-flip-sync=1` is on by default when `ruan-120hz=1`. `dpu-flip-sync=0` forces it off, and it can be turned on at 60 Hz as well.
- `syncedFlip()` reads INTF1 timing: htotal = HSYNC_CTL>>16, vtotal = VSYNC_PERIOD_F0/htotal, last active line = DISPLAY_V_END_F0/htotal
  (2731). It writes the flip (VIG0 SRC + CTL flush) only while LINE_COUNT is in [vEnd-256, vEnd-8], with IRQs off around the
  check and the write. The latch then follows within about 0.85 ms (≤ 282 lines × 3.02 µs) [I].
- Outside that window it does not busy-wait. It arms `fenceTimer` (the GPU-wait retry path) for the time until the window
  opens and returns. `syncReady` skips re-capturing the GPU fence on the retry.
- With flip sync on, the swap retires at the latch (`latchTick`, flush bits clear), not at the flip. The old buffer is free only once
  it is off the panel, as Android's release fence works. This costs ≤ ~1 ms, not a frame, because the flip now sits right before the latch.
- If the timing can't be read or flip sync is off, it behaves exactly as before. 60 Hz is unchanged by default.
- Compile check: the full patch chain plus this patch, built with the `compile_cxx` recipe from build-gop-legacy-display.py
  (ruan_ufs IOPlatformExpert.cpo.json, -Wall -Wextra -Werror) passes. `_ml_set_interrupts_enabled` is exported by the ruan kernel [V].

## 3. Test order

1. A/B with no code change: `ruan-120hz=1 vsync=120 dpu-retire-flip=0`. If the tearing goes away, the cause is confirmed.
2. The patch: `ruan-120hz=1 vsync=120`. Expect no tearing and about the same fps as step 1 or better. Check that `native_legacy_direct[4]`
   (longest latch wait, µs) drops to about 1000 or less.
3. If the tearing is still there with step 1: look at CA/app buffer reuse, not the DPU.
