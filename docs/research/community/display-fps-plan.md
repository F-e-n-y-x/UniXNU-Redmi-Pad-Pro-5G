# ruan display frame rate: findings and fix plan (2026-11-19)

Research and code reading only; nothing was built or booted. Inputs: rbu80 `cmd display` (`/root/unixnu-work/display-rbu80.txt`),
GopFramebuffer `b0d812d` (= origin/HEAD, fetched today), ruan's `package/gopfb-*.patch` + `LegacyDisplayProbe.cpp`, a disassembly of
the 21H461 family in `UniXNU/out/ruan/.../build/xnu-prelink/native-owner-mobile-graphics-candidate.macho`, and shallow clones of
sheng, picasso (PR 19 head `30f4437`), uke, amethyst, Pagani_SM8750, diting, houji, shennong, hakios-sm7435 and
RigoLigoRLC/unixnu-nativemobiledisplay (scratchpad `ports/`).

## 1. Root cause (short)

**The display path is not what limits ruan to ~0.35 fps.** Every swap backboardd sent was accepted, copied and retired
(`accepted=231 rejected=0`, `copies=231`, each copy 12-17 ms with `fb-wc=1`). Selector 6's e00002d5 is **kIOReturnBusy, not a
timeout** (kIOReturnTimeout is e00002d6). It is the family's answer to a **non-blocking** swap-wait poll on a swap that is still
queued (see §3). That is one poll per frame (230 Busy for 231 swaps), and then a blocking wait succeeds. A missing vsync client (selector 72) is not the cause
either: sheng (int-209) and picasso (wf131) never see a selector-72 client and still run 40-70 fps on the render server's own
1/60 s display timer. So backboardd simply **submits a frame only every ~3 s**. The time goes into producing the frame
(Turnip in `tu-debug=sysmem,noubwc,nolrz`, cold pipeline compiles, render threads on the A55 cores), not into presenting it.

One display-side possibility is still open and must be ruled out first (step 1). If the family's 5 s swap-wait timeout fires
(the path that reached slot 345 and panicked rbu70), each such frame costs 5 s, and 231 × 5 s ≈ the ~1100 s of UI time in rbu80.
The counters cannot tell this apart from a slow producer today. The family's Busy also covers that case, and the rbu80 log replay
has no "CommandWake didn't happen" line, but the replay may not carry IOLog text.

## 2. Selector map (IOMobileFramebuffer user client, 21H461)

| Sel | Meaning | Evidence | rbu80 |
|---|---|---|---|
| 3 | default framebuffer surface (`create_default_fb_surface`) | unixnu-site-notes §1; returns error with no hardware surface | 1/1 e00002bc (Error), harmless |
| 4 | SwapBegin | sheng `package/OfflineLegacySwap.mm` call order; 4 = 5 = swaps | 231/0 |
| 5 | SwapEnd (→ validateSwap slot 296, kickSwapQueue slot 290) | same | 231/0 |
| 6 | **SwapWaitWithTimeout** → `swap_wait_gated`; option bit 31 = noBlock | sheng `OfflineLegacySwap.mm:61-69` + `check-renderbox-presentation.py:69`; disassembly below | 562/230 **e00002d5 = Busy** |
| 8 | GetDisplaySize-class query | 131 successful calls | ok |
| 68, 88 | setup queries (68 fails once, then succeeds) | — | ok |
| 72 | **vsync client registration** (fills the queue at +5592 via slot 208; ends in kickSwapQueue) | `sheng scripts/build-gop-legacy-display.py:157`, GopFramebuffer comments | never called |
| 12, 15, 22, 27, 29, 33, 50, 55, 57, 74, 79, 84 | optional panel features the family answers itself without a platform subclass (calibration, brightness/colour, CRC, factory, AOD-class) | e00002c7 = Unsupported, 28 = e00002f0 NotFound; one call each at setup and never retried | harmless |

The unclassified selectors were not decoded one by one (the dispatch table has no names). None is retried, so backboardd treats
them as optional. picasso reportedly answers 27/68 with NotReady (coordinator's web sweep; I did not find it in the PR 19 code).

### Selector 6 in the family (disassembly, 21H461)
- `swap_wait_gated` (strings: `IOMFB %s: swapId = %d, options = 0x%x, noBlock = 0x%x, HPD..., TimingEnable = %d`). If the swap is
  still in the pending queue (`this+5480`) and noBlock is set, it stores `0xe00002d5` (`mov w8,#0x2d5; movk #0xe000` at
  `0xfffffff00578bffc`) and returns.
- Otherwise it calls `commandSleep(this+5480, deadline 5000 ms)` (gate vtable +272, at `…c0c8`-`…c11c`). On a timeout it logs, calls
  **slot 345** (`ldr x8,[x8,#2760]; blr` at `…c1c8`, = `isTimingEnabled`, ruan's `gopfb-zz-slot345-timing.patch`) and clears the swaps
  ("CommandWake didn't happen even after 5 seconds … cancelling swaps").
- The wakeup comes from `completeCurrentSwap()`, which ruan calls in `retireSwap` after each copy. So a poll is Busy only while
  the copy is still queued or running on the work loop.

## 3. What QuartzCore needs for vsync, and what the other ports did

- No port makes backboardd call selector 72 on iPadOS 17 with a GOP display. sheng's own note (GopFramebuffer
  `NativeLegacyDisplay.cpp`, getRefreshAttributes comment, "int-209: no vsync client ever registered, frames came every 16.7 ms")
  and picasso wf131 ("the compositor kicking the swap queue three times with the client queue at +5592 still empty every time")
  show the same. The render server paces itself by **IOMFBDisplayRefresh** (slot 301 → `setDisplayRefreshProperties`), defaulting
  to its own 1/60 s heartbeat when the property is missing.
- In GopFramebuffer (= UniXNU PR #33 / `c031d88`, `c99046a`) **IOMFBDisplayRefresh is published only when `vsync` > 60**
  (`registerService`: `if(primary && hz>60 && hz<=240)`; `getRefreshAttributes`: `if(hz<=60) return`). ruan runs `vsync=60`, so
  the property is never published and the tick has nothing to do. Every smooth port uses `vsync=120` (sheng also `vsync-fixed=1`;
  uke/amethyst `sheng-vsync=120`).
- hakios-sm7435 (same SoC; `bootargs.md:106`): "animations that ask for nothing ran at 24 fps through Metal" until `anim_max=1`.
  24 fps is the family's default maximum interval (1/24 s). A published range of 24-120 Hz lets CA pick 24 Hz. `vsync-fixed=1`
  closes the range.
- picasso PR 19 (`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp:162-197`): it treats the first kick as "vsync wanted"
  (`vsyncKicked`), so the tick always runs. It also **re-kicks the swap queue once a second from the tick** ("a lost submission
  kick must not require another user frame to recover"). The notify itself is a no-op with an empty client queue, so the pump is
  the useful part. ruan's `gopfb-vsync-poll.patch` only polls for a client every 100 ms (`no_client=13705` = 1370 s of polls).
- picasso BootProgress.c:1308-1310: "The boot core is a little A510 … a lowest-priority thread bound to it (`sheng-cpu0-park=1`)
  keeps it busy so work goes to the big cores … Compositing frames ran **40 fps without it and 55 fps with it**." That holds
  when there are no E/P cluster types. ruan's cpu0-3 are A55 and cpu4-7 A78. `native-smp-type-clusters.patch` only splits them
  if the ADT cpu nodes carry `cluster-type`, and `board.toml` sets none.

Where the fps of the smooth ports comes from (presentation side):

| Port | Present path | Result | Files |
|---|---|---|---|
| sheng (SM8550) | `dpu-scanout=2` direct scanout, else `fb-gpu=1` GPU copy, early retire; `vsync=120 vsync-fixed=1`, `cpufreq=4`, `gpu-dcvs=1` | 59-73 fps | `UniXNU-sheng/board.toml:61,243`; GopFramebuffer `NativeLegacyDisplay.cpp` (directSlot/flip/latch, `gpuWrote`) |
| amethyst | CPU copy ~170 ms/frame → GPU hand-off presenter `sheng-fb-gpu=1 sheng-vsync=120` ≈ 10 ms | smooth | device record `amethyst-bluedmc-amethyst.toml`; `NativeGOPPresenter.cpp` |
| picasso PR 19 | retire at the flip instead of after the latch (31 → 46 fps per the PR text); tick pump; cpu0 park; read-only SDE/SMMU probe (`sheng-sde-probe=1`, SID 0xb40) | 40-55 fps | `UniXNU-picasso@pr19 drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp:162-197,314-360`; `boot-progress/BootProgress.c:1308` |
| uke | ports the GopFramebuffer tick `33e941a` + slot 301, `sheng-vsync=120` | — | device record `uke-*.toml` |
| diting | GPU copy completion, 8-18 fps "driver stats", user reports low fps | slow | `diting.toml` |
| Pagani_SM8750 | build 63: slot 345 answered (as ruan's patch) | no fps claim | — |
| hakios-sm7435 (garnet, same SM7435) | own MiniIOMobileFramebuffer: copy split over `fb_threads=6` cores, `fb_intf=0x0ae36000` (INTF_1 frame/line counters +0xac/+0xb0) for beam-synced writes and measured refresh, `bootcpu=7` (gold core), `anim_max=1` | "scan-synced frames" | `hakios-sm7435/bootargs.md:17,36,68,81,106` (the kernel repo `hakios-xnu-kernel` is not public: clone failed) |
| RigoLigoRLC nativemobiledisplay (SM7325) | DPU scanout: IOSurface → display SMMU CB4, VIG0 via CTL0, INTF1 vblank IRQ drives the vsync message | real vblank | `NativeDpuScanout.cpp`, `NativeDpuInterrupt.cpp`, `NativeDpuSurface.cpp`, README |

Our copy is single-threaded. `kickOnce` → `native_legacy_copy_pixels` runs on the family's one work-loop thread, on whatever
core it was scheduled (with no cluster types, the lowest idle core: often an A55). `cpus=8 smp=1` does not change that.

## 4. Plan for ruan, in order (effort → gain)

1. **Measure first (small, decides everything).** In `package/gopfb-zz-slot345-timing.patch` count `isTimingEnabled()` calls
   (`ruan_slot345_calls`). Print it and `native_legacy_swap_timeouts` in the USB `display` command (`package/UsbDebug*.h` /
   LegacyDisplayProbe), together with `native_legacy_swapstat[0..15]` and the copy time `native_legacy_copy_last/max`. Also dump
   the last ~256 entries of `native_legacy_ring` (µs timestamp + event 1 validated / 6 retired, already recorded): the swap
   interval and the validate→retire latency per frame.
   - slot345 ≈ swaps → display-side bug: a swap stays queued for 5 s. Fix at step 3b.
   - slot345 = 0 and validate→retire ≈ 15 ms → producer-bound (expected). Go on with step 2.
2. **Boot args only (no code):** `vsync=120 vsync-fixed=1` (publishes IOMFBDisplayRefresh at one fixed rate and stops CA
   picking 24 Hz; the panel is 120 Hz). Add a USB check that the registry has `IOMFBDisplayRefresh`. Also try `fb-diff=1` (A/B on
   the `cp` banner): a mostly static lock screen should store far fewer bytes.
3. **Small GopFramebuffer patches (`package/gopfb-*.patch`):**
   a. Replace `gopfb-vsync-poll.patch` with picasso's version: `vsyncKicked` after the first kick, plus `kickSwapQueue()` once a
      second from the tick (picasso PR 19 lines 162-197; rename `sheng-vsync` back to `vsync`).
   b. Fix `gopfb-retire-transfer.patch`. It now ignores `retireSwap()`'s result and clears `transferred` anyway, so when
      `activateOnlyPendingSwap` refuses (another swap is `current`), the frame is copied but never completed, and the waiter
      sits out the 5 s. Keep upstream's rule (stay latched when retirement fails) and only keep the `force=true` part.
4. **CPU placement (small):** check whether the ADT gives the cpu nodes `cluster-type` E (0-3) / P (4-7) so
   `native-smp-type-clusters.patch` engages (boot log: two processor sets / Edge scheduler). If not, add them in the ADT builder
   (`[payload.adt]`). Otherwise port picasso's `cpu0_park` (`BootProgress.c:1308-1320`) into ruan's `package/BootProgress.c` behind
   a boot-arg. hakios's `bootcpu=7` is the same idea.
5. **Producer speed (medium; the real fps lever if step 1 says producer-bound):** run without `tu-debug=sysmem` / `nolrz` once
   GMEM is safe; `gpu-dcvs`-style GPU clock (sheng `gpu-dcvs=1`); ship a pipeline pack (unixnu-site-notes §4) so frames don't
   compile shaders; keep `ruan-cpufreq=2`.
6. **Parallel copy (medium):** split `native_legacy_copy_pixels` rows over 4 A78 threads (hakios `fb_threads=6`). This takes
   the 12-17 ms copy to ~4 ms, which matters once the producer reaches 30+ fps.
7. **Real vsync from the DSI counters (medium):** read INTF_1 at `0x0ae36000` (+0xac frame count, +0xb0 line count; verify
   against ruan's DT, same SoC as garnet). Drive the vsync tick and the copy start from it (beam-synced, no tearing; measure the
   real refresh rate).
8. **Direct DPU scanout (large; removes the copy):** first a read-only SMMU/SDE probe (picasso `sdeDiag`, PR 19 lines 314-360,
   SID 0xb40). Then RigoLigoRLC's `NativeDpuScanout.cpp` / `NativeDpuSurface.cpp` / `NativeDpuInterrupt.cpp` (SM7325, the
   DPU closest to SM7435: CB4, VIG0/CTL0, INTF1 vblank), or GopFramebuffer's `dpu-scanout=2` (which needs a NativeUfs-owned
   display SMMU window ruan lacks). Under Gunyah we can probably only reuse the firmware's context bank (dizi/sheng notes).
   With this, swaps retire at the flip (picasso's 31→46 fps step).

Not useful for ruan: `fb-gpu=1` GPU copy into 0xb8000000 (hangs the GPU: the GOP buffer is not GPU-writable, IPADOS-BRINGUP §26).
