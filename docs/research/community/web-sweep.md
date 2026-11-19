# Web sweep: UniXNU / iOS-iPadOS on Snapdragon community (2026-11-19)

Research only. Public pages read with WebSearch and WebFetch. Nothing logged in, posted or contacted, and no device touched.
Excluded: the user's own Telegram group. The official UniXNU Telegram https://t.me/unixnu and QQ group 1126116759, linked from unixnu.com, were not read, on the assumption that one of them is that group.
Caveat: WebFetch summarises pages with a small model. Re-read the source before acting on a quote. Repo "% done" tables are self-reported.
Ruan-relevant problems (§26): slow display (sel 6 e00002d5, no vsync client / sel 72), GPU scanout, resets after launchd, 16-min boot, UFS root, Wi-Fi, audio.

## Most useful first

### 1. YinCQL/UniXNU-picasso PR #19: display pacing, fps 31 -> 46 (merged 2026-11-06)
https://github.com/YinCQL/UniXNU-picasso/pull/19 (picasso, Qualcomm SDE/Adreno, 1080x2400 @120 Hz)
- Biggest fps gain came from **retire-at-flip**: "The latch completes asynchronously and only gates the next flip, so the compositor can commit its next frame without waiting for it." wf339 had the latch in the retirement path: 31-33 fps. wf341 retires at flip: 42-46 fps, median 25.2 ms. A later gpu11 run reached a DPU period of 16.4-18.6 ms (~60 fps).
- Remaining cadence ~24 ms is put down to the render server's display timer. `IOMFBDisplayRefresh` reports min 1/60 s and max 1/24 s and "settles near 40 Hz". The next test was `sheng-vsync=120 sheng-vsync-fixed=1`, with no result posted.
  -> For us: check that our sel-6 swap completion doesn't wait on the copy/latch. Retire the swap first, and let the copy gate only the next swap. Also check what IOMFBDisplayRefresh we publish.
- An early overlay `picasso-fbuc-soften` returned `kIOReturnNotReady` for **IOMFB selectors 27 and 68**. Review removed it ("FBUC return-code changes" dropped). Selectors 27 and 68 also appear in our rbu80 list (27 fails once, 68 is 2/1).
- Direct DPU scanout (removed from the merged scope but described in the commits): firmware scans VIG0 (0x5000), CTL0 (0x2000), SRC0_ADDR = GOP phys. Compositor surfaces are page-scattered ("getPhysicalSegment(0) = 4 KiB vs 10,444,768 needed"), so they were mapped into four 32 MiB windows at 0x10000000 through **apps SMMU bank 2, SID 0xb40**. Per swap: wait for GPU fences, write SRC0_ADDR/SRC_YSTRIDE0, flush VIG0, poll the latch (500 us interval, 50 ms timeout). Counters: "flips=1200 maps=3 mapfails=0 fallbacks=0 latch_timeouts=0", latch mean 4.8 ms. Review reason for removal: "did not yet establish a safe consumer-release lifetime across flips, timeout, fallback and stop."
  -> This is a concrete template for our "GPU scanout needs our own DPU scanout buffer" item.
- GPU: backboardd abort fixed by sizing 3-wide function constants (`MTLDataTypeUChar3` = 3 bytes) in `constantBytes()`. Open issues: SpringBoard ASTC abort, backboardd restarts about every 10 s.

### 2. UniXNU/UniXNU PR #33 and UniXNU/GopFramebuffer: refresh publishing and the fence contract
- https://github.com/UniXNU/UniXNU/pull/33 (merged 2026-10-21, `5a9264b`): NativeMobileDisplay "publishes IOMFBDisplayRefresh at the rate given by sheng-vsync=<Hz>". The goal is that the render server paces to the panel instead of its "own 1/60 s heartbeat". Pairs with GopFramebuffer#1 and UniXNU-sheng#6 (sheng at 144 Hz).
  -> Check whether our core has this commit and whether `vsync=` actually feeds IOMFBDisplayRefresh on ruan.
- https://github.com/UniXNU/GopFramebuffer: "with vsync=<Hz>, a work-loop timer sends the MobileFramebuffer vsync message to the compositor". Busy fence queries retry after 200 us. "Reaching 250 milliseconds records a slow wait; the current frame stays on screen" until done or cancelled. `dpu-scanout=2` lets the display pipes fetch 8-bit surfaces directly. Half-float needs `fb-gpu=1` or a CPU conversion. `native_legacy_formats` counts swaps, refusals and paths per format. `gpu-trace=1` logs arrival, fence and latch.
  -> `dpu-scanout=2` plus `native_legacy_formats` are the upstream knobs to try or compare for our GPU-scanout gap.

### 3. dameriek-bit/ios-pipa: Xiaomi Pad 6 (SM8250, Adreno 650), the only port claiming Wi-Fi + audio + UFS root together
https://github.com/dameriek-bit/ios-pipa (iOS 26 / iPadOS; no dates on the page; percentages are self-reported, unverified)
- **Wi-Fi 90%:** QCA6390 over PCIe, "FreeBSD LinuxKPI plus Linux ath11k". The status line: "QCA6390 / WCN6851 PCIe scan, WPA2/WPA3 association, Safari web browsing". Our WCN6750 also uses ath11k, but over AHB/SNOC with the MSA/Q6 path, so only the ath11k/LinuxKPI layer would carry over.
- **Audio 80%:** "ADSP AudioReach firmware loader with a CoreAudio HAL output plugin", with AW882xx/CS35L41 amps. Compare with our WSA883x (soundwire) plan.
- **UFS root 100%:** journaled HFS+ on the `userdata` partition, boot-args `rd=ufsroot0 ufsrw=1 rootrw=1`. It wipes Android userdata, which is not acceptable for ruan without the user's decision.
- **Display 95%:** "TE-synced double buffering, tear-free page flips" over the ABL `cont_splash` buffer with AppleCLCD/IOMobileFramebuffer. TE (tearing-effect) interrupt as the vsync source is an alternative to a timer.
- Boot-args include `wdt=-1 wdt_test=no_user_checkin`. That is relevant to our resets after launchd: try it to rule out the userland watchdog.

### 4. sheng (UniXNU reference) shader cache, for the 16-min boot
https://pbxscience.com/?p=24278 (updated 2026-10-20)
- "A cold boot spent roughly 40 to 43 seconds compiling interface shaders… Exported pipeline caches are meant to avoid this on later boots." The pipeline is NativeMetal -> metal2vulkan (AIR->SPIR-V) -> Turnip -> NativeAdreno, with a 1 GiB GPU window for the render server.
  -> Our ~16 min is far from sheng's 40 s. Pick up the shader-cache registration agent (#302/#304, already noted in §26) and compare per-shader compile times on the A710.

### 5. none24smu/UniXNU-zeta: Surface Duo 2 (SM8350 "Lahaina", Adreno 660), Turnip in backboardd
https://github.com/none24smu/UniXNU-zeta (latest dated entry 2026-10-26). Press: https://www.windowslatest.com/2026/10/27/ios-26-can-run-on-an-android-phone-and-a-developer-proved-it-on-microsofts-abandoned-surface-duo-by-turning-it-into-an-iphone-duo/ ("37fps, up from 3-10fps"; the repo does not say what changed).
- Fixes listed: shared VM-table capacity, per-client object limits, scattered buffer allocation, **GPU VA expanded to 4 GiB**, a ranged blit selector, and submit back-pressure. "No DeviceLost, old job-slot-error or queue-deadline messages" over 8 min after the pressure fix. Presentation completion still averaged "about 55-83ms".
- Stability: 1,200 s run with "no spontaneous disconnect or recorded kernel panic".
- UFS root: native UFS reads, GPT, a signed root image flashed to `userdata`, OS volume found as `disk3s1`, launchd and SpringBoard running.
  -> The allocation-pressure fixes are worth comparing with our A710 path if fps stays low after the display fixes.

## SM7435 (same SoC as ruan)
- **vildangil/UniXNU-Dizi-Ruan**: https://github.com/vildangil/UniXNU-Dizi-Ruan (Redmi Pad Pro Wi-Fi `dizi`; ruan "not tested"; checked on a real dizi 2026-11-17). Display works through the GOP framebuffer with a CPU copy; vsync is not discussed. **UFS root works:** OS and SystemOS mount read-only from a dedicated `unixnu` partition, and Data is a writable 128 MB RAM volume. GPU not started (the next stage mentions "the ruan port's GPU work"). Notes a "reset after MMU-on" early on and a USB debug link that "drops after a few minutes". No Wi-Fi or audio.
- **Scarpy19/UniXNU-garnet**: https://github.com/Scarpy19/UniXNU-garnet (Redmi Note 13 Pro 5G / POCO X6 5G, SM7435-AB). Archived 2026-11-12 with nothing booted. Reference data: Adreno 710 "Gen 7_3_0", chip ID 0x07010000; UEFI FD 0xA7100000 size 0x300000; kernel 0xA8000000; XNU payload 0xF4000000 size 0x04000000; 72 XNU patches. No Gunyah notes.
- **F-e-n-y-x/UniXNU-Redmi-Pad-Pro-5G**: https://github.com/F-e-n-y-x/UniXNU-Redmi-Pad-Pro-5G is our own public repo. Its README stops at the rbu29 checkpoint (2026-11-16), so it is stale against local rbu80.

## Other UniXNU ports with relevant notes
- **GFander/UniXNU-kebab PR #7** (OnePlus 9 / SM8350, merged 2026-10-26): https://github.com/GFander/UniXNU-kebab/pull/7. AppleCLCD copies swaps on the CPU into the bootloader FB, and the FB "drives vsync from a 60 Hz timer". "SpringBoard rejects a mode whose scale is zero", so they set a display-scale in /chosen. A display node under /arm-io panicked with "cannot find GIC node in DT". "the UFS node has no iommus at all", unlike USB, which sits behind an SMMU (explains UFS DMA working while USB DMA crashed).
- **aogg2023/songyuan-device** (Redmi K90 Pro Max, SM8850, Adreno 840): https://github.com/aogg2023/songyuan-device. GOP FB 1220x2656, "publish 120 Hz (a work-loop timer, not the panel's vblank)", "the scan-out hand-off stalls intermittently". The `unixnu` UFS partition carries a persistent Data store. No Wi-Fi or audio. Press: https://tech.ifeng.com/c/8xcCkLX2hTE (2026-10-29, dev 江灵夏草: "GPU 图形渲染、多核 CPU 调度、120Hz 屏幕显示以及 USB 数据通道" work; camera, keys, baseband and speaker do not).
- **xVanTuring/UniXNU-qemu**: https://github.com/xVanTuring/UniXNU-qemu. QEMU virt on Apple silicon (HVF), iPadOS 17 userland, and a Metal device run on the Mac's GPU. Could be a fast loop for IOMFB/QuartzCore experiments without the tablet.
- **7Night-kqx/UniXNU-dada** (Xiaomi 15, SM8750): https://github.com/7Night-kqx/UniXNU-dada. "no IOMobileFramebuffer service reaches userland" (WIP).
- **mouya-q/UniXNU-umi** (Mi 10, SM8250): search snippet says "iPadOS 17.7.11 Hello, UFS, persistent Data, CPU display". The repo URL returned 404 on fetch (renamed or deleted).
- Other SM8150/SM8475/SM8850/SDM845 forks, all early: mxy218/UniXNU-raphael (iOS 16.7.16 home screen), giannoccarol/UniXNU-andromeda, lengcece/UniXNU-cupid (iOS 27, SM8450), menglilongxia, LuoRogers and zhouzhoublm/UniXNU-unicorn (SM8475), NiandeNian/UniXNU-songyuan, Clover2304/UniXNU-polaris, MiLacy/UniXNU-marble, losthappines/UniXNU-xun, ahmedraafat-official/UniXNU-gtaxllte. Nothing ruan-specific was found in their search snippets.

## HakiOS / ios-sweet family (separate from UniXNU)
- **Telegram channel @oosexgangdevs** (HakiOS, ~21K subs): https://t.me/s/oosexgangdevs. Recent posts: "First frames rendered on GPU with metal already", "27 booted on mt6768", "Metal work on mt6768, release in 20s of month". The dev portal hakios-developers.com is announced, with a repo over 3,000 commits and a one-click start. Group: https://t.me/oos_chat (not read). MediaTek-focused right now, with no Adreno or display-pacing content.
- **hackiosx.dev** (independent index): https://hackiosx.dev/projects and https://hackiosx.dev/compatibility. Reddit: https://www.reddit.com/r/hackios/ (blocked for our fetcher). Ports listed with Wi-Fi/audio on Qualcomm: MakrSas/ios-topaz (Redmi Note 12 4G, SD685: "Wi-Fi with Safari, sound, charging"), NWylynko/ios-flame (Pixel 4, SD855: "Metal/Mesa, Wi-Fi, Safari"), joranv1/ios-sailfish (Pixel 2016: Wi-Fi, charging), oosexgang/hackios-xnu-kernel (POCO X5 Pro, SM7325: Wi-Fi, camera; no Metal). These are good places to look for Qualcomm Wi-Fi/audio driver approaches in XNU.
- dipa-studio/ios-sweet: https://github.com/dipa-studio/ios-sweet (Redmi Note 10 Pro, SM7150; CPU rendering).

## Searched, nothing specific found
4PDA (only general 3dnews/iphones.ru coverage), XDA (no thread), Reddit (fetch blocked; r/hackios exists), Bilibili, Coolapk, YouTube descriptions and Discord invites (none found, and unixnu.com lists only Telegram, QQ and X: https://x.com/UniXNU_new). The press pieces (kocpc, pbxscience, ifeng/ithome, windowslatest, androidauthority, decrypt) only repeat what the repos already say.
