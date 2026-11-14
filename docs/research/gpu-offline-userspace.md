# ruan iPadOS 17.7.11: the userspace side of rendering without a GPU, and a Metal path later (GPU agent B, 2026-11-14)

## Verdict
The first UI does not need software Metal. On 17.7.11, QuartzCore renders on the CPU when there is no Metal device, and the
reference port reached the lock screen that way:
- refs/UniXNU-amethyst/scripts/log/WORKING-BOOT.md:134-140: v3 shows the lock screen with a live clock and **no GPU**, at
  "continuous swaps every ~2.3s (CPU presentation)", with the same offline-legacy-* arguments as our rb11.
- refs/UniXNU-amethyst/scripts/log/WORKING-BOOT.md:378-381: even with a Metal accelerator published, "UI stayed on CPU rendering"
  until the add-on was installed.
- unixnu-fuxi's Offline*.mm are host and diagnostic probes (OfflineMetalAddon.h:2-3 says "Explicit offline read-only bindfs
  view ... not ... production"). fuxi never reached display presentation or the home screen (refs/unixnu-fuxi/README.md:33).
  There is no CPU Metal implementation in it to reuse.

So CA does not wait for a Metal device. **We need no CA_\* environment variable or extra root files.** CA_FRAMEBUFFER
(compression and deep framebuffer off) is already seeded (make-ramroot.py CA_FRAMEBUFFER), and the 13 display-format text fixups
come from display-prep.

## Ranked plan
1. **Userspace: stop the crash loops that hold back the first UI** (staged here as `make-ramroot-disabled-jobs.patch`,
   about 30 lines, 0 bytes of root growth since it only changes the Data seed's disabled.plist).
   - Evidence: amethyst v1 and v2 stayed on the Apple logo after 4-6 swaps (WORKING-BOOT.md:104-130). v3 disabled the
     crash-looping daemons, and the UI came about 130 s after the first swaps (:134-140). ruan rb11 has the same signature:
     3 swaps, then nothing. Its crash-loopers are audiomxd (SIGSEGV at FAR 0 every ~5 s, no audio hardware), searchd and
     siriactionsd (NSInvalidArgumentException: AssetProperties / NSBundle nil URL), maild (killed), and xpcproxy exiting 78
     for "Missing executable" jobs.
   - The patch adds com.apple.audiomxd, com.apple.searchd, com.apple.siriactionsd and com.apple.email.maild to DISABLED_JOBS.
   - It also disables every launchd job whose executable is not in the root or the OS cryptex: `missing_jobs(tree)` reads
     System/Library/xpc/launchd.plist and finds 40 on 21H461 with our SKIP_DAEMONS, e.g. corespeechd, geod and the App-cryptex
     webbookmarksd. Self-check run against work-full/root: SpringBoard and backboardd are kept, geod is disabled.
   - Risk: SpringBoard may expect a live mediaserverd/audiomxd XPC peer. A disabled job fails its connection at once, which is
     better than a peer that keeps dying. mediaserverd stays enabled.
2. **Kernel side (agent A, not here):** rb11's banner shows `st1 ar0`: no vsync client registers. amethyst reached the UI with CPU
   presentation, so check after step 1 whether `a` grows past 3. If it doesn't, the vsync-client question is agent A's.
3. **Later, a real GPU instead of software Metal.** This is the only Metal path the refs ship:
   - **Kernel (agent A):** a kernel Adreno driver for ruan's **Adreno 710** ("Adreno710v1", qcom,adreno-gpu-gen7-3-0;
     macOS/hardware/private/ruan-live.dts:16134). UniXNU has only drivers/Adreno740, which is a7xx and the nearest base.
   - **Userspace pieces:**
     - The NativeAdrenoMetal.bundle plugin and Turnip, built with UniXNU/scripts/build-adreno-metal-plugin.py. It needs xcrun
       and an iphoneos SDK (line 39); this host has /usr/bin/xcrun. It also has macos27 and ios27 targets, which matter for the
       later macOS goal.
     - Packing into a GPU add-on with UniXNU/scripts/make-gpu-addon.py:1-20 (NGPUADD2 format, zlib).
     - Delivery: the payload's `--gpu-addon`, then /chosen/memory-map GpuAddon, then native_ram_block_gpu_addon
       (UniXNU/drivers/RamBlock/NativeRamBlock.cpp:691-713), then the kernel writes /private/var/ngpu and mounts it
       (gpu-addon=1).
     - An owner trust module with the plugin's CDHashes, placed in front of the two Apple modules
       (refs/UniXNU-gts7xlwifi/board.toml:132-134, make-owner-trust-bundle.py).
     - A sandbox grant that names backboardd exactly (refs/UniXNU-amethyst/porting.toml:376), or backboardd falls back to the
       CPU (WORKING-BOOT.md V47 cause).
   - **Size:** weeks, dominated by the A710 kernel driver. The userspace pieces are mostly reuse.

## Root size
Step 1 adds nothing to the root: it only changes a plist in the Data seed, so the root stays 2533 MiB (limit 2816 - 128 = 2688
MiB). Step 3's add-on travels in the payload, not the root.

## Trust cache and signing
Step 1 adds no code, so trust.bundle is unchanged (IPSW Static and SystemOS modules, 7416ca4f…). Step 3 needs an owner module
(ad-hoc 4 KiB-page signature, its CDHash in a trust module prepended to the Apple ones), as gts7xlwifi and amethyst do. AMFI and
Apple signatures stay untouched.

## Apply
`cd /root/unixnu-work/UniXNU-ruan && patch -p0 scripts/make-ramroot.py < /root/unixnu-work/gpu-prep/userspace/make-ramroot-disabled-jobs.patch`
(the patch's file names are make-ramroot.orig.py and make-ramroot.py, so name the target explicitly as shown). Then rebuild the
root with REBUILD_ROOT=1 DATA_CONTAINER=1 RAMBLOCK=1 DATA_SLACK=98304, keeping rb11's kernel and arguments. The Data UUID stays
the same because mkapfs derives it from the fixed name ruan-data.apfs; check that ram-data-uuid in the build log still matches.
