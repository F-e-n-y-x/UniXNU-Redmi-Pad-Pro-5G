# Smoother iPadOS UI on ruan (2026-11-21)

[V] = verified in code, binaries or logs on the host. [I] = inferred. Nothing here was run on the tablet.

## Ranking (expected gain vs risk)

| # | Change | Gain | Risk | State |
|---|---|---|---|---|
| 1 | Fresh pipeline cache: pack + Data overlay from the current plugin build | High: first-time screens, and every screen after a Data reset | Low | Shipped pack is cold [V]. Needs one USB pull |
| 2 | Turnip `-mstrict-align` + kernel `patches/adreno/0007` (BOs write-combined) | High: ends backboardd/SpringBoard SIGBUS respawns. 0007 also speeds CPU writes to command streams | Low / medium | Turnip built [V]. 0007 written, never built [V] |
| 3 | More disabled daemons (DumpPanic, parsecd, ML, indexing, OTA) | Medium: boot CPU and memory | Low | In make-ramroot.py [V] |
| 4 | `RUAN_CA_OPTS=CA_FORCE_GENERIC_SHADERS` (A/B test) | Medium: fewer pipeline compiles on the render thread | Medium: per-pixel cost | Wired, off by default |
| 5 | `RUAN_CA_OPTS=CA_FORCE_TRIPLE_BUFFERING` (A/B test) | Low to medium | Adds a frame of latency | Wired, off by default |
| 6 | GMEM | Unknown. Could be negative with 512 KiB GMEM at 2560x1600 | High | Needs the UCHE window first (see §4) |

## 1. Pipeline pack

### Why it is slow after a Data reset

- **The shipped pack and `data-overlay` are cold for the shipped plugin [V].**
  - The translator cache lives at `<dir>/v1/<build>/…`.
  - `<build>` is SHA-256("metal2vulkan shader cache build\0" ‖ "0.2.0" ‖ 0 ‖ ARTIFACT 525 ‖ SEMANTICS 0x8000085E ‖ SOURCE_DIGEST) (metal2vulkan-runtime `shader_cache/mod.rs this_build`).
  - The shipped `NativeAdrenoMetal` embeds SOURCE_DIGEST `274639…1926`, which gives build `ed3666c5…`.
  - The pack (`NativeAdrenoMetal.m2vpack`, sha256 2560db93…, built 11-20 00:20 from `m2v-pulled/`) only holds `v1/1272d509…` (177 files). The other directory, `529a20fd…`, is the shared content directory, a constant.
  - So every translation is rebuilt after a reset.
- **Clients never import the pack [V]** (`NativeAdrenoMetalPlugin.mm`).
  - Only the render-server path (around line 607) calls `cacheexport::importPack`.
  - Clients such as SpringBoard, PosterBoard and apps use `sharedShaderCacheDirectory()` = `/private/var/mobile/Library/Caches/org.unixnu.metal`, or else their own `com.apple.metal/m2v`.
  - The render server's order is: shared dir (candidate 5), then `~/Library/Caches/com.apple.backboardd/m2v` (candidate 4), then the temp dirs.
  - The 11-20 pull came from candidate 4, so on that boot backboardd was not on the shared dir. That boot may have been the older rbu74-root plugin [I].
- **Turnip keys [V].**
  - The Vulkan and Mesa caches key on `-Dtu-build-id`, built from the Mesa source diff only. In this build it is `d5d5c509…`, and the old and new dylibs carry the same bytes.
  - A flags-only Turnip rebuild, such as strict-align, therefore keeps them valid.
  - The LC_UUID only keys `prime-done`, and priming is off.
  - Do not rebuild the plugin or bridge between the pull and the build that ships it.

### Steps (on zima, the build being shipped already booted)

1. Warm the cache: use the UI for about 10 min. Cover lock screen, home pages, folders, Control Center, Notification Center, app switcher, Settings panes, Safari, keyboard and Spotlight. Priming has no primelist in this build.
2. Stop any `ruan-usbdbg.py log` stream (`pull-m2v.py` has no flock), then run:

```sh
cd /root/unixnu-work
python3 usb-dev/host/ruan-usbdbg.py ls /private/var/mobile/Library/Caches/org.unixnu.metal
python3 usb-dev/host/ruan-usbdbg.py ls /private/var/mobile/Library/Caches/org.unixnu.metal/v1
python3 usb-dev/host/ruan-usbdbg.py ls /private/var/mobile/Library/Caches/com.apple.backboardd/m2v
python3 usb-dev/host/ruan-usbdbg.py ls /private/var/mobile/Library/Caches/com.apple.backboardd/m2v/v1
# pull-m2v.py now takes REMOTE LOCAL (recursive, skips same-size files; retry FAILED ones with `ruan-usbdbg.py get PATH -o FILE`)
python3 pull-m2v.py /private/var/mobile/Library/Caches/org.unixnu.metal              caches-TAG/shared
python3 pull-m2v.py /private/var/mobile/Library/Caches/com.apple.backboardd/m2v     caches-TAG/bbd-m2v
ls caches-TAG/*/v1    # must contain ed3666c5… (or the new build's hash), not only 1272d509…
```

3. Build it in.
   - The pack goes into the render server's directory: use whichever of the two directories holds `v1/<build>` with the most files.
   - The Data overlay gives both clients and the render server warm files on a fresh RAM Data.

```sh
python3 UniXNU-ruan/scripts/make-ruan-m2vpack.py caches-TAG/shared NativeAdrenoMetal.m2vpack   # self-checked
cp NativeAdrenoMetal.m2vpack oslogx/overlay-dir/overlay/System/Library/Extensions/NativeAdrenoMetal.m2vpack   # data file, no cdhash
rm -rf data-overlay && mkdir -p data-overlay/mobile/Library/Caches/com.apple.backboardd
cp -a caches-TAG/shared  data-overlay/mobile/Library/Caches/org.unixnu.metal
cp -a caches-TAG/bbd-m2v data-overlay/mobile/Library/Caches/com.apple.backboardd/m2v
./build-only-root.sh TAG     # already passes OVERLAY=oslogx/overlay-dir DATA_OVERLAY=data-overlay
```

- To check after boot, `ls` the same two directories. Only `v1/<build>` should hold files (no fresh compiles), and `.import.*` temp files must not linger.
- Keep the pack under a few MiB. backboardd copies it into its cache directory at every start, which uses Data space.

## 2. Turnip SIGBUS

- **Faults [V]** (disassembly of the shipped Turnip, 88bde5b3…):
  - +0x4eb74 `str d0,[x8],#8` writes a two-dword packet to `cs->cur`, which is only 4-byte aligned.
  - +0x86d10 `stur q0,[x8,#0x18]` stores a 64-byte descriptor at +0x18.
  - Both are stores into GPU BOs mapped as Device memory (`kIOMapInhibitCache`). Unaligned stores to Device memory fault.
- **Root fix:** `unixnu-ruan/patches/adreno/0007-Adreno740-user-views-write-combined.patch` (Normal-NC).
  - It is not in any build yet: `UniXNU/build/ruan-adreno/src` is from 11-20, and rbu180–186 ran with `KR=`.
  - It needs a kernel rebuild (`KR=1`).
  - It also covers `memcpy` from libSystem, which strict-align does not.
- **Turnip build [V, host-only, ran]:**
  - `UniXNU/scripts/build-turnip-darwin.py` now honours `TURNIP_EXTRA_CFLAGS`. These are compile args only, also passed as `-Dc_args`/`-Dcpp_args` so `--reconfigure` picks them up. The flags are recorded as `extra_cflags` in builtin-build-profile.json.
  - The core diff is uncommitted, like the rest of core-local. Re-record it in `unixnu-ruan/patches/unixnu/core-local-8542980.patch`.
  - Store counts drop: `stur/str q` to non-sp addresses go from 3862 to 190, and post-index `str d` from 1463 to 23.

```sh
cd /root/unixnu-work/UniXNU; T=$PWD/.cache/mesa-tools; TC=$PWD/.cache/ios-tc
TURNIP_EXTRA_CFLAGS=-mstrict-align XNU_TOOLCHAIN_BIN=$TC UNIXNU_SDKROOT=/root/theos/sdks/iPhoneOS16.5.sdk \
  PATH=$T/bin:$TC:$PATH LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu python3 scripts/build-turnip-darwin.py --target ios
```

- **Outputs:**
  - Raw: `build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib`, sha256 `de7f8008…15f1`.
  - Stripped, weak-bound and ldid-signed: `build/ruan-turnip-strict/libvulkan_freedreno.dylib`, sha256 `b61259da…8659`, CDHash `bd545aaa9e8f25201f451c1b643f76dfa7fd6d3c`.
  - The old CDHash is `2996d02e79072878588707c081bb0a75c9db9df4`.
  - The previous raw build is kept in `build/mesa-tu-ios.rbu186-unaligned/`.
  - Note: build-turnip-smoke.py and build-metal-plugin.py now pick up the strict-align build.
- **Install:**

```sh
cd /root/unixnu-work; N=UniXNU/build/ruan-turnip-strict/libvulkan_freedreno.dylib; O=oslogx/overlay-dir
cp $N $O/overlay/usr/bin/libvulkan_freedreno.dylib
cp $N $O/overlay/System/Library/Extensions/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib
sed -i 's/^2996d02e79072878588707c081bb0a75c9db9df4$/bd545aaa9e8f25201f451c1b643f76dfa7fd6d3c/' $O/cdhashes
./build-only-root.sh TAG
```

## 3. Userland (make-ramroot.py, synced to UniXNU-ruan)

- **Disabled jobs [V]:** launchd reads `System/Library/xpc/launchd.plist`. Every SKIP_DAEMONS label is still there in the full root, so removing the plist files never stopped them; passd still crash-looped 25 times in rbu182u.
  - Added to DISABLED_JOBS: `parsecd` (writes SiriSearchFeedback reports), `DumpPanic` (90 s of CPU at 96 %, a cpu_resource report in rbu182u), `photoanalysisd`, `mediaanalysisd` (+ `.service`), `knowledgeconstructiond`, `intelligenceplatformd`, `spotlightknowledged`, `spotlight.IndexAgent`, `biomesyncd`, `proactiveeventtrackerd`, `spindump`, `mobile.softwareupdated` and `OTATaskingAgent`.
  - passd was added by another session at the same time.
  - ReportCrash and osanalytics stay on, so the .ips reports keep coming.
  - The cost: no panic .ips (use ramoops) and no Spotlight content index.
  - Optional: `com.apple.apsd` faults 13–25×/boot (ExcUserFault). Not disabled, because push may matter.
- **CA options [V]:** `CA_FORCE_TRIPLE_BUFFERED_LATENCY` does not exist in 21H461's QuartzCore; its dyld cache has 298 `CA_*` names.
  - Present instead: `CA_FORCE_TRIPLE_BUFFERING`, `CA_ENABLE_PREFERS_TRIPLE_BUFFERING`, `CA_FORCE_GENERIC_SHADERS` and `CA_DISABLE_PRECOMPILED_PIPELINES`.
  - j716f found that every QuartzCore pipeline is made synchronously on the render thread.
  - `RUAN_CA_OPTS="NAME …"` sets these to true in backboardd's domain. CADefaults reads the environment, then the defaults [I that bools take the defaults path].
  - A/B test one name per build.
- **Blur and animations:**
  - Reduce Transparency is already on (`EnhancedBackgroundContrastEnabled`).
  - Reduce Motion is off on purpose (rbu140).
  - No other UniXNU port seeds more than the CAEnable*Compression and SB dim keys [V, grep of refs/].

## 4. GMEM

- **10eccf9 [V]:**
  - It is on `origin/perf/turnip-tile-scissor-and-gpu-pacing`, not in main or our pin.
  - It is a performance change only (skip tiles with no visible views, clip render area to the scissor union). It does not fix garbage tiles.
- **Likely cause of garbage tiles [I]:**
  - Turnip gets `gmem_base 0` (`RuanAdrenoGlue.cpp deviceInfo`), and `UCHE_GMEM_RANGE` reads 0/0 on the device (rbu52b).
  - Any GMEM read through UCHE (memory-side swap loads, input attachments) then hits VA 0 instead of tile memory.
- **Suggestion for the RuanGpuPower.c owner:**
  - Program `UCHE_GMEM_RANGE_MIN/MAX` = 0x1000000 .. +512 KiB−1 after GX is on.
  - Also put the range in the pwrup restore list, as KGSL gen7 does [I].
  - Then report `gmem_base 0x1000000` in `deviceInfo` (`RuanAdrenoGlue.cpp`).
- **Test order:**
  1. `gmem,nocb,noubwc,nolrz` in `/private/var/ngpu/tu_debug`.
  2. If tiles are still garbage, add the UCHE window.
  3. Only then cherry-pick 10eccf9 (`git -C UniXNU show 10eccf9:patches/mesa/turnip-skip-invisible-tiles-and-clip-render-area.patch`, add it to adreno-compiler-inputs.json). This changes the Mesa diff, so redo §1 afterwards.
- **Expected gain [I]:** at 2560x1600 with 512 KiB, each pass needs about 32 bins. Simple CA passes may be faster in sysmem, so measure before shipping.
