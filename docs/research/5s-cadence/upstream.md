# Why backboardd presents one frame every 5.0 s: what upstream UniXNU says (2026-11-21)

This was read-only research. Nothing was built or booted, and the tablet was not touched. Sources:
- UniXNU `origin/HEAD` 8542980 at `/root/unixnu-work/UniXNU`. A fetch found nothing new.
- The metal2vulkan reference checkout at `.cache/gpu-reference/metal2vulkan` (46e7cc9).
- The amethyst repo (`IOS4Amethyst/UniXNU-amethyst`, `scripts/log/WORKING-BOOT.md`).
- ruan's logs in `/root/unixnu-work/logs`.
- The 21H461 dyld shared cache in `/root/unixnu-work/dsc`, disassembled with llvm-mc. The local symbols are redacted, and the `.symbols` file is LZBITMAP-compressed (decmpfs type 14), so no function names.

Labels: **[V]** = verified (read in source, a log or disassembly). **[I]** = inferred.

## 1. Where the 5 s is *not*

| Place | 5 s? | Evidence |
|---|---|---|
| IOMFB family swap wait | Yes, but it does not fire on ruan | `swap_wait_gated` sleeps up to 5000 ms and then calls slot 345 → `cancelSwapsAfterTimeout`. That bumps `native_legacy_swap_timeouts` (`drivers/GopFramebuffer/NativeLegacyDisplay.cpp:104-105, 815-838`). On ruan, rbu145p `logs/rbu145p/display-1.txt` shows `PACING slot345=0 timeouts=0`. **[V]** |
| Display latency | No | Same log, `SWAPS 78530:1 78531:7 78552:6 83369:1 ...`. Validate→retire takes ~20 ms, then the next validate comes ~4.8-5.3 s later (`SUBMIT-MS 5333 4839 5042 5043`). So the whole gap lies between one retire and backboardd's next SwapBegin, in userspace. **[V]** |
| Swap retirement gate | No | `retireSwap` (`NativeLegacyDisplay.cpp:793-804`) needs `offline-legacy-retire=1`, `offline-legacy-cpu-present=1` or `force`. Without one of them a CPU-copied swap would never retire and the 5 s family timeout would fire. It does not fire (timeouts=0), so the gate is fine. **[V]** |
| Adreno740 hangcheck | No | 500 ms period (`drivers/Adreno740/NativeAdrenoPublish.cpp:271-317`, `IOSleep(500)`). It also runs `fencePass(true)` as msm's "missing irq" workaround, so a lost GPU interrupt costs at most 500 ms, not 5 s. **[V]** |
| Adreno740 stall handler | No | Stages at 50 ms and 200 ms (`NativeAdrenoSubmit.cpp` `noteStall`, about lines 838-905). **[V]** |
| Adreno740 WaitFence | No default | The caller supplies the timeout (`NativeAdrenoABI.h:99-101`, `NativeAdrenoUserClient.cpp:774-782`). **[V]** |
| Turnip Darwin KMD | No | Waits in 100 ms slices until the caller's deadline (`patches/mesa/turnip-darwin-kmd.patch:531-556`). `darwin_wait_idle` waits forever and leaves hangs to the hangcheck (`:1295-1301`). The only `5000000000` is in a smoke test (`drivers/AdrenoMetal/NativeTurnipSmoke.h:91`). **[V]** |
| AdrenoMetal frontend | No | The only waits are `waitUntilCompleted` → `nmb_command_buffer_wait(h,0)` (unbounded, `NativeBridgeMetalFrontend.mm:3722-3762`), timers at 10 s, 30 s, 250 ms and 500 ms, and a test-only 5 s (`bridge/FrontendChecks.h:380`). **[V]** |
| metal2vulkan | 4.5 s, but switched off | `DevicePolicy::UNTRUSTED` has `pipeline_commit_wait`/`pipeline_creation_wait = 4500 ms` (`crates/metal2vulkan/src/device/policy.rs:204-205`; deadline `metal2vulkan-runtime/src/command_buffer/mod.rs:4462-4469`). A 4.5 s wait plus ~0.5 s of frame work would give 5.0 s. Upstream AdrenoMetal sets `PipelineWaitPolicy::Exact` (`bridge/NativeMetalObjects.rs:271`), which removes it. **[V]** for the source. **[I]** that ruan's build matches: it is unchecked. |
| Selector 72 / vsync | Not the cause | sheng and picasso never get a selector-72 client and still run at 40-70 fps (our `docs/research/community/display-fps-plan.md` §3). amethyst (iPadOS 17.7.11, same family) swaps every ~15 ms. **[V]** |

## 2. Where a 5 s *is*: CoreAnimation (QuartzCore 21H461)

- **[V]** QuartzCore holds the render-fence strings `timed out fence %llu`, `timed out batch %x`, `fence tx observer %x timed out after %f`, `failed to wait for fence tx reply: %x`, `Failed to create batch fence in applyTransaction.` and `render-fence.cpp`. Their xrefs are at 0x189449fbc, 0x18944930c, 0x1896d0860 and 0x1894fa974.
- **[V]** The function at **0x1894f7fe0** is a locked wait (lock at +44) for a counter at +56 to reach a target. Its timeout argument (w2, in ms) is handled like this:
  - It is clamped to **5000 ms** (`mov w8,#5000; csel`, 0x1894f8130).
  - When the caller passes ≤0, the deadline is `now + 5.0` (`fmov d1,#5.0; fadd`, 0x1894f8168), and it waits with `dispatch_time(0, 0x12a05f200 = 5 000 000 000 ns)` (0x1894f8170-78).

  So **every caller gets at most 5.0 s, and 5.0 s by default.** It sits about 10 KB from the "fence tx reply" xref, probably the same unit (`render-fence.cpp`) **[I]**. Callers: 0x1894f7f44 and 0x189563950. The second runs after a mach send that failed with 0x10000003 (MACH_SEND_INVALID_DEST) **[V]**.
- **[V]** The fence sweeper at 0x189449f48 walks the pending fences. It times out any whose stored deadline (double at entry+24 → +8) has passed, and logs `timed out fence`.
- Only four `fmov #5.0` exist in all of QuartzCore (0x1894f8168, 0x1895214b8, 0x18953d254, 0x189614d8c). 0x1895214b8 is a `now > [obj+304] + 5.0` check in a large server-side function **[V]**. Its role is unknown.

**Mechanism [I]:**
1. A client (SpringBoard, a poster extension, an app) opens a CA fence or batch for a synchronized update and never answers it. This happens if it is stuck, its Metal device failed, or it was killed or denied the GPU.
2. The server holds that transaction until the fence times out, 5.0 s later.
3. It then applies the transaction, and backboardd renders and swaps exactly once.
4. If a fence is re-armed every frame, the screen updates every 5.0 s. Input-driven changes queue up behind the held transactions, which fits UI changes appearing 10-20 s late.

This explains why no display or GPU knob changes the cadence. A 4990-5050 ms spread fits a timer plus about one frame of work, not GPU workload.

## 3. How the fast ports avoid it

- **amethyst** (SM7635, iPadOS 17.7.11, the closest analog), from `WORKING-BOOT.md` **[V]**:
  - Its stalls disappeared once the crash-looping daemons were disabled through the Data seed. This was its "130 s stall" / "6 swaps then none" phase.
  - With GPU compositing on (`sheng-fb-gpu=1 sheng-vsync=120`) it swaps every ~15 ms.
  - PhotosPosterProvider, CollectionsPoster and WidgetRenderer still failed Vulkan (-3) there, and frames were still fast.
- **alioth / sheng-lite=1:** disables the AegirPoster and KaleidoscopePoster extensions by launchd label (`docs/devices/alioth.md:355`). sheng's lite list disables others the same way.
- **sheng, picasso:** `vsync=120 vsync-fixed=1`, direct scanout or GPU copy. No 5 s cadence is recorded. A sweep of every device record for "5 s", "5000 ms", "one frame every" and "fence tx" found no 5 s frame cadence anywhere **[V]**.
- **Inferred difference on ruan:**
  - `ruan-grant-provider.patch` (commit d8d615e) **denies the GPU to the lock-screen poster extensions**.
  - CollectionsPoster crash-looped every ~5 s in earlier builds (`docs/research/opt/pacing.md:7`).
  - A poster that cannot render but still takes part in SpringBoard's synchronized wallpaper updates is the prime candidate for the fence that is never answered.

## 4. Ranked causes and tests

1. **A CA fence or batch held by an unresponsive client, released by the 5 s timeout [I, strongest].**
   - a. Check the unified log export (or `log stream` / os_log capture) for `timed out fence`, `timed out batch`, `fence tx observer … timed out after 5.0` and `failed to receive fence reply`. Expected: one per frame, from backboardd. This settles it.
   - b. One boot with the poster/wallpaper extensions disabled in the Data seed (launchd `disabled.plist`, as `sheng-lite`). Candidates: CollectionsPoster, PhotosPosterProvider, PridePosterExtension, AegirPoster, KaleidoscopePoster, plus PosterBoard if needed. A second boot with `ruan-poster-gpu=1` (GPU allowed again). Watch whether SUBMIT-MS drops below 5000.
   - c. Line up crash/jetsam/corpse timestamps of every process with the SWAPS timestamps (as `pacing.md` did). A process that dies or respawns every 5 s is the fence holder.
2. **Bounded metal2vulkan pipeline wait (4.5 s + frame) [I, cheap to rule out].** On the device, read `runtime-policy.log` in the Metal shader-cache directory. It must say `pipeline_waits: Exact`. If it says `Bounded`, the bridge build differs from upstream `NativeMetalObjects.rs:271`. This also fits "sysmem slow, gmem fast" if sysmem pipeline compiles never finish. A cold compile also shows in `PipelineRecords`/`warm`.
3. **A Metal client that never completes (sysmem jobs hanging) [I].** An unanswered client frame would hold the fence as in 1. Check `native_adreno_recovery[0..1]` (hangs/restarts), `native_adreno_unstick`, `native_adreno_fence_pending[7]` (≥64 ms pending) and the `[gpu stall]` rows during a 5 s run. amethyst needed `[gpu unstick]` kicks before it ran smoothly.
4. **Display family 5 s swap wait [V, ruled out]**: timeouts=0. Keep printing `PACING` to make sure it stays 0.

## 5. Concrete fix steps once 1a/1b confirm

- Disable the culprit extensions in the Data seed (`make-data-seed.py` / launchd overrides, as amethyst v3 and sheng-lite do). Or give them a working GPU (`ruan-poster-gpu=1`) if their failure comes from the deny.
- Keep `vsync=120 vsync-fixed=1` (display-fps-plan §4.2) and GPU or direct presentation.
- Don't patch QuartzCore's 5 s constant: it is the backstop, not the bug.

## Not checked
- The exact callers and semantics of QuartzCore 0x1894f7fe0 and 0x1895214b8: the symbols are redacted, and decompressing the LZBITMAP `.symbols` fork would name them.
- ruan's actual on-device `runtime-policy.log`, and the oslog contents of the current builds.
