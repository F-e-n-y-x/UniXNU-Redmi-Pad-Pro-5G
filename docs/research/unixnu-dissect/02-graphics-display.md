# UniXNU graphics and display stack vs ruan (2026-11-21)

From a read-only code-explorer pass. [V] means verified in code or docs, [I] means inferred.

## Frame path

1. **CoreAnimation in backboardd** renders through the Metal plugin (NativeAdrenoMetalPlugin.mm).
   - The render server does the full setup: shader cache, pipeline pack import, priming.
   - Every other Metal process also opens Turnip (clientDevice :219-274).
2. **Metal → Vulkan → Turnip → kernel.** Adreno740 handles submission and fence waits (runCore :757, WaitFence :774-833).
   - Without a GPU interrupt, waiters spin. On ruan that's `IODelay(20)` in RuanAdrenoGlue.cpp:131.
3. **Swap.** IOMobileFramebuffer selectors 4/5 run validateSwap, then kickSwapQueue (NativeLegacyDisplay.cpp:591). One swap is in flight at a time.
4. **Direct scanout** (dpu-scanout=2):
   1. gpuWrote waits on the frame's GPU fence.
   2. directSlot maps the surface into the display SMMU window.
   3. flip writes the VIG pipe's SRC0/STRIDE registers plus the CTL flush.
   4. latchTick waits for the flush to clear, then retireSwap wakes selector 6.
5. **Pacing.**
   - CA paces by IOMFBDisplayRefresh. With `vsync=120` and no `vsync-fixed`, the advertised range is 24–120 Hz, so CA animates at 24 Hz, which on a 60 Hz panel gives alternating 33/50 ms frames.
   - The vsync tick needs a selector 72 client, and none exists on GOP ports.

## Knobs used by fast devices

| Knob | Effect | Who uses it | ruan |
|---|---|---|---|
| dpu-scanout=2 | Flip with no copy | sheng, alioth, pong, dizi | on (rbu130u+) |
| vsync-fixed=1 | One fixed refresh rate published to CA | sheng | on (rbu133u+, at 60) |
| Retire at flip | picasso 31 → 46 fps | picasso | dpu-retire-flip=1 (rbu137u) |
| gpu-dcvs=1 (16 ms window, 80 ms idle, 1000 ms touch boost) | GPU governor | sheng, amethyst, pong | ruan-gpu-dcvs drafted |
| gpu-preempt=2 | Background clients can't block the render server | sheng | no (single ring) |
| TU_DEBUG=gmem,nocb | On-chip tile rendering | upstream default | ruan uses sysmem,noubwc,nolrz |
| Pipeline pack + priming | No cold shader compiles | sheng | not shipped |
| Deny the GPU to posters | pong: 6/6 clean runs, 54.5 fps | pong | not done |
| CA_FORCE_TRIPLE_BUFFERED_LATENCY=1 | CA renders one frame ahead | WindowServer ports | not done |
| bootcpu=7 / park cpu0 | picasso 40 → 55 fps | hakios-sm7435, picasso | no |

Measured on other ports:
- sheng: 67–73 fps average, lows of 30–41 fps.
- pong: 57 → 95 fps at 120 Hz, using a UIKit animation-manager hook.

## Ranked plan for ruan

1. vsync=60 vsync-fixed=1, then dpu-retire-flip=1. Optionally add CA_FORCE_TRIPLE_BUFFERED_LATENCY=1 for backboardd.
2. Stop the poster churn and deny the GPU to poster extensions (pong). Also remove other Metal clients: there's only one GPU ring, so every client removed helps backboardd directly.
3. GMEM: write `gmem,nocb` to /private/var/ngpu/tu_debug. Add upstream 10eccf9 (turnip-skip-invisible-tiles-and-clip-render-area.patch).
4. ruan-gpu-perf=1 at level 6, then 7. Then the DCVS governor. Don't run fixed at 940 MHz.
5. ruan-cpufreq=3, then move the boot CPU to an A78 or park cpu0.
6. Pipeline pack: run once with PRIME=1, pull the cache, run make-ruan-m2vpack.py, then build with PIPELINE_PACK=.
7. Shrink the Metal caches for clients that aren't the render server.
8. GPU interrupt (gpu-fence-wake) instead of spinning.
9. INTF1 vsync source, then 120 Hz.

## Risks

- **dpu-retire-flip:** can tear for up to one frame.
- **GMEM/UBWC:** unverified on A710.
- **GPU speed bin:** unconfirmed, so levels above 6 may overclock.
- **Benchmarks:** sheng and pong numbers come from other SoCs and OS versions.
