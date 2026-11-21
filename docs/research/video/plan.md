# Video decode / encode on ruan (iPadOS 17.7.11, 21H461): research and plan

Date: 2026-11-21. Research only: no device access, no builds.
Tags: **[V]** = verified here (file, string or DT line read). **[I]** = inferred, still needs a test.

## TL;DR

1. iPadOS 17.7.11 already contains Apple's **VideoToolbox paravirtualization guest**: `VideoToolboxParavirtualizationSupport.framework` (AppleParavirtGPU-1), `AppleVideoToolboxParavirtualizationDriver`, `ParavirtualizedVideoDecoder` / `ParavirtualizedVideoEncoder` **[V, strings in the 21H461 dyld cache]**. VideoToolbox sends whole decode sessions to a kernel service with that name. We don't need to emulate AppleVXD / AVE.
2. **UniXNU-sheng already built this end to end** on SM8550: the Qualcomm iris VPU driver, a V4L2-shaped decoder device, and a kernel "host" behind `AppleVideoToolboxParavirtualizationDriver`. H.264 and HEVC (8 and 10 bit, up to 4K) decode on hardware there **[V, docs; tested on macOS 27.3]**. Its code is in `<work>/vg/UniXNU-sheng/drivers/QcomIris` (7.4 k lines) plus `package/VideoProbe.cpp` (677 lines).
3. ruan's VPU is the same family (iris2 on SM7435 "parrot", firmware `vpu20_1v`, PAS 9). The reserved memory, the TZ content-protection ranges and the IOVA pools in ruan's live DT **match the numbers sheng uses** **[V]**. PAS from XNU under Gunyah already works on ruan (zap shader, PAS 13) **[V]**.
4. Cheapest test that could already work today: VideoToolbox's **software H.264 decoder** (`H264VideoDecoder_*`) **[V, present]**. HEVC has **no software decoder** on iOS 17 **[I: none found]**, which is why HEIC fails.
5. Ranked plan: (A) test software H.264 and Safari/YouTube with H.264 → (B) port sheng's iris + paravirt host to iris2/ruan → (C) encoder for screen recording later.

---

## 1. How iOS 17 decodes and encodes video

### 1.1 Hardware path on the IPSW device (iPad7,1 / j120ap, A10X)
- Decode: plugin **AppleD5500** (FigWrapper `AppleD5500_H264VideoDecoder.c`, `_HEVCVideoDecoder.c`, `_MPEG4VideoDecoder.c`), which opens the kernel services **AppleVXD375 / AppleVXD390 / AppleVXD393** **[V, dsc strings]**. This IPSW does not use AppleAVD (that is the A12+/M1 decoder). The string `AppleAVD` does exist in the cache.
- Encode: **AppleAVEVA** user driver → kernel **AppleAVE2Driver**. Its user-client calls are `kAVEUserClientOpen`, `...DriverSetSessionSettings`, `...EncodeFrame`, `...StopSession`, `...Close`, `...DriverResetBetweenPasses` **[V]**. Encoder IDs: `com.apple.videotoolbox.videoencoder.ave.avc` and `.ave.hevc` **[V]**.
- Emulating VXD/AVE would mean reverse-engineering private parameter structs, ring buffers and IOSurface handling (`kAppleD5500DisplayCallBack`, `kAppleVXD393EnableIOFence`, …). Not worth it while the paravirt path exists.

### 1.2 The paravirtualized path (the one to use)
- Present in 21H461 **[V]**: `/System/Library/PrivateFrameworks/VideoToolboxParavirtualizationSupport.framework`, `AppleVideoToolboxParavirtualizationDriver`, `VTIsParavirtualized`, `VTIsParavirtualizationAware`, `VTParavirtualizationReplyClerk`, `VTParavirtualizedJPEGSession`, `<ParavirtualizedVideoDecoder %p UUID …>`, `<ParavirtualizedVideoEncoder …>`, `paravirtualized:com.apple.videotoolbox.videoencoder.ave.{avc,hevc}`. Also the defaults keys `AllowParavirtualizedDecoder`, `AllowParavirtualizedEncoder`, `EnableParavirtualizedProtectedCodecs` **[V]**.
- Protocol, from sheng's reading of the iPadOS/macOS 27 cache (`drivers/QcomIris/kernel/VtParavirt.cpp` header, `VtWire.h`). The user client is opened with type 0:
  - sel 0 `{size}` → `{capacity, send buffer, IODataQueue}`
  - sel 1/2 register/deregister a 16-byte guest UUID
  - sel 3 `{length, ≤5 IOSurface IDs}` → `{status}`
  - sel 4: queue drained
  - notification port type 0 (an IODataQueue)
  - Messages are big-endian FigAtoms: a 48-byte `'vtpv'` header, then `'para'` atoms (`=s32 =u32 =s64 =uui =dat =pls =fds =sbf`).
- Whether 17.7.11 uses the **same selectors and message types as 27** is **[I]**. This is the first thing to check (§5, step B0).
- The kernel side registers classes under Apple's names because the platform sandbox only lets a process open user clients that its profile names **[V, sheng comment]**.

### 1.3 Software decoders that exist in 21H461
| Codec | Software decoder | Evidence | Notes |
|---|---|---|---|
| H.264 | `H264VideoDecoder_CreateInstance/DecodeTile/…`, `ThreadCountH264SWDecoder`, `ForceSyncH264SWDecoder` | [V] | VideoToolbox's own CPU decoder. On j120ap it should be the fallback when no VXD service matches [I]. |
| HEVC | none found (only `AppleD5500_HEVC*` and `decodeHEVCDecoderConfigurationRecord`) | [I] | Explains the HEIC failure; the PNG overlay stays needed until hardware decode works. |
| VP9 | libvpx inside libwebrtc (`WebM Project VP9 Decoder v1.13.1`, `WebKitVP9Decoder.cpp`, `WK_RTCVideoDecoderVTBVP9`), gated by `VP9DecoderEnabled`, `VP9SWDecoderEnabledOnBattery`, `WebKitVP9DecoderEnabled` | [V] | WebKit registers it with VideoToolbox for MSE/WebM. |
| AV1 | `com.apple.videotoolbox.videodecoder.av1.sw` with dav1d (`dav1d_open`, …), `allowsAppleOnlySWAV1Decode`, `useSWDecoderForAV1` | [V] | Gated to Apple-only clients; Safari on iOS 17 does not offer AV1 to the web [I]. |
| ProRes | `AppleProResSWDecoder.videodecoder` | [V] | Not relevant here. |

Force/selection knobs present **[V]**:
- `kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder`, `..._RequireHardwareAcceleratedVideoDecoder`, `..._EnableSandboxedVideoDecoder`, `VTIsHardwareDecodeSupported`, `kVTHEVCDecoderProfileCapability_IsHardwareAccelerated`.
- WebKit: `preferSWDecode`, `maxH264VideoDecoders`, `forceSoftwareVideoEncoder`, `AlternateWebMPlayerEnabled`, `MediaSourceEnabled`, `ManagedMediaSourceEnabled`, `MediaSourceInlinePaintingEnabled`.

How these are read (domains: `com.apple.coremedia`, `com.apple.mobilesafari`, the WebKit feature flags in Settings › Safari › Advanced) is **[I]**.

### 1.4 What Safari needs to play YouTube [I]
- Safari on iPad (iOS 17) uses MSE and ManagedMediaSource. YouTube picks the codec from `MediaSource.isTypeSupported`.
  - If VP9 is not reported, YouTube serves **avc1 (H.264) up to 1080p** plus AAC/Opus.
  - If VP9 is reported (the software decoder is gated by power state and hardware checks), it may serve VP9.
- H.264 through MSE decodes via AVSampleBufferDisplayLayer → VideoToolbox. That is the software H.264 decoder now, and the paravirt iris decoder later.
- Audio is a separate blocker. The README says audio is "not started"; with no audio output device, playback may stall [I].

## 2. What other ports did

| Port | Video result | Where |
|---|---|---|
| **sheng** (Xiaomi Pad 6S Pro, SM8550, iris3, `vpu30_4v`) | **Hardware H.264 + HEVC (8/10-bit, 4K) through the VideoToolbox paravirt host.** Tested on macOS 27.3: 60/60 frames of 1280x720 H.264, and the 3840x2160 10-bit aerial wallpaper plays. Decoder only: the encoder half is compiled out. Some sheng variants (abdulfarizi, jsongmax, 111027866) had HEVC refused by firmware and fell back to "VideoToolbox's software decoder" (macOS has one). | `<work>/vg/UniXNU-sheng`: `drivers/QcomIris/{src,kernel,include,tools}`, `package/VideoProbe.cpp`, `package/VideoToolboxProbe.cpp`, `scripts/{build-qcom-iris.py,stage-video-firmware.py,check-video-platform.py}`; docs `UniXNU/docs/devices/sheng*.md` "QcomIris"; device evidence revision `9252d988c0b668bc1f68c3e482951c8b2b33abc1`; local fork HEAD `c42e9a0` (QcomIris arrived in squash `8ad2fa2`). [V] |
| evb10 (RK3588, iPadOS newer than 17) | No video. The doc notes that iPadOS decodes through the `AppleAVDUserClient` service, which that kernel lacks, and that only the AV1 software decoder (a Metal program) is in the image. | `UniXNU/docs/devices/evb10.md:522` [V] |
| diting / pong-xz1c / rmx3551 / thor | "qualified native software decoder selection" (one changelog line, no details) | `docs/devices/diting.md:927` etc. [V line only] |
| alioth, amethyst, asteroids, picasso, dizi, pclm10, Dizi-Ruan | Nothing on video; alioth only uses the HEIC → PNG overlay | `docs/unixnu-dissect/03-userland-stability.md:13` [V] |
| QEMU t8030 / inferno / Corellium | No public video-decode work found. Corellium markets "play video … via native hardware" without saying how. | web search [I] |

## 3. SM7435 hardware: the iris2 VPU

From `macOS/hardware/private/ruan-live.dts` **[V]**:
| Item | ruan (parrot) | sheng (SM8550), for comparison |
|---|---|---|
| Node | `qcom,vidc@aa00000`, `reg 0xaa00000 0xf0000`, compatible `qcom,msm-vidc-parrot`, `qcom,msm-vidc-iris2` | iris @0xaa00000 |
| IRQ | SPI 0xae (174), level | SPI 174 |
| Firmware | `vpu20_1v` (.mdt/.bNN or .mbn from Android `vendor/firmware`; not found locally) | `vpu30_4v.mbn` |
| PAS id | **9** | 9 |
| Carveout | `video_region@86c00000`, size 0x700000 | 0x700000 |
| TZ CP config (svc 0x0c cmd 0x08) | DT pools: non-pixel 0x25800000 + 0xba800000, secure non-pixel 0x1000000 + 0x24800000 | `(0, 0x25800000, 0x01000000, 0x24800000)`, **same** |
| SMMU (apps SMMU, phandle 0x28) | venus_ns SID **0x2180 mask 0x20**, venus_ns_pixel **0x2187**; secure banks 0x2181/0x2183/0x2184 (vmids 9/10/11, leave alone) | 0x1940, 0x1947 |
| Clocks (videocc @0xaaf0000, `qcom,parrot-videocc`) | video_ctl_axi0, video_mvs0_axi, core, vcodec, iface, video_cc_iris_clk_src; rates 133.3/240/335/424/460 MHz | Sm8550VideoCc |
| Power | GDSC `video_cc_mvs0c_gdsc` (0xaaf8084, iris-ctl) and `video_cc_mvs0_gdsc` (0xaaf6004, vcodec); mvs1/mvs1c exist but are unused (1 pipe, "1v") | mvs0c/mvs0 |
| Bus | interconnects `venus-cnoc`, `venus-ddr`; reg-preset 0xb0088 = 0x11 | RPMh MMCX/MXC nominal + MM1 |

- **HFI:** downstream msm-vidc for parrot/waipio is the gen2 ("hfi_packet") driver. So ruan most likely speaks **HFI gen2 like sheng**, but on **VPU2 hardware** (power-up/down sequence, `iris_vpu2`-style register set, VPU2 buffer sizing, 1 VPP pipe) **[I]**. Confirm offline: check the strings and version in `vpu20_1v.mdt`, and whether the Linux tree pinned in `tools/source-lock.json` (v7.3.3) has a parrot/sm7435 or qcs8300-class iris2+gen2 platform.
- **Can XNU drive it like the WPSS?** Yes **[I, strong]**.
  - `RuanWifiWpss.cpp::pasBoot(path, pas, base, size, …)` already does init_image → mem_setup → auth_and_reset with resume/busy retry.
  - PAS 13 (zap) works from XNU under Gunyah when the segments are written before init_image (rbu62b+) **[V]**.
  - Sheng adds one extra SCM call: `scm(0x0c, 0x08, {0, 0x25800000, 0x01000000, 0x24800000})`.
- **Gunyah/Android VM ownership:** the non-secure VPU context banks are HLOS banks on the apps SMMU. Under Gunyah, HLOS may program CBAR TYPE 1 (S1) but not TYPE 0 (`IPADOS-BRINGUP.md:197`) **[V]**; sheng's IrisMmu uses an S1 translating context **[V]**. Gunyah traps SMMU MMIO, so use plain LDR/STR (as in the Wi-Fi plan) **[V note]**. Secure video (vmids 9-11) belongs to TZ / the trusted VM: skip it, so no DRM (FairPlay) playback **[I]**. No sign that the Android VM (OEM VM) owns the VPU **[I]**.
- **Size of a minimal driver:** sheng's is the measure **[V]**: HFI/core/session/buffers/MMU/IOVA ≈ 2.5 k lines of C, IrisMedia 0.55 k, paravirt transport 0.4 k, VtHost 3.0 k (decoder + encoder), board probe 0.7 k. Our `AdrenoHfi.c` (115 lines) is GMU HFI, a different protocol: reuse only the queue idea, not the code.
- **Memory:** a 4K session needs 150-440 MiB wired on sheng **[V]**. ruan already fights jetsam (mediaserverd 140 MB limit, RAM root), so cap at 1080p (roughly 30-60 MiB, **[I]**). The buffers are wired in the kernel, not counted against mediaserverd.

## 4. Encode for screen recording (cheapest → hardest)

1. **Host-side capture (dev only):** stream framebuffer snapshots over the existing USB CDC channel and encode on the PC. No iOS encoder needed. Days. [I]
2. **Software encoder already in VideoToolbox:** `com.apple.videotoolbox.videoencoder.h264.rtvc.sw` and `forceSoftwareVideoEncoder` exist **[V]**. Whether ReplayKit or the Control Center recorder accepts a software encoder, or requires hardware (`ave.avc`), is **[I]**. Test with a small app: `VTCompressionSession` with `EnableHardwareAcceleratedVideoEncoder=false`. If it works, screen recording needs only the GPU/IOSurface path.
3. **Paravirt encoder → iris encoder:** iris2 has an encoder (iris2 encodes, `[I]`). Sheng's `VtHost.cpp` already contains the encoder half (`VtEncoder=false`, line 96; enc protocol from line 1539) **[V]**. Needs an iris encoder session plus the IrisMedia encoder card. 2-4 weeks after decode works.
4. Emulating AppleAVE2Driver: no.

## 5. Ranked plan

| # | Step | Effort | Risk | Reuse |
|---|---|---|---|---|
| A1 | On the current build: play an H.264 .mp4 in Safari / Files, and run `avconvert`, to see whether VideoToolbox falls back to `H264VideoDecoder` with no VXD service. Log the VT errors. If it crashes, find out where (mediaserverd / videocodecd jetsam vs. a failed VXD open). | 0.5 day | low | none |
| A2 | Safari YouTube on H.264: check `MediaSource.isTypeSupported('video/mp4; codecs="avc1.4d401f"')`. Audio output may block playback. | 0.5 day | med (audio) | none |
| A3 | Optional: VP9 software in WebKit (feature flags, `VP9DecoderEnabled`, `VP9SWDecoderEnabledOnBattery`) | 1 day | med | none |
| B0 | **Offline:** disassemble `VideoToolboxParavirtualizationSupport` + VideoToolbox paravirt code from the 21H461 dsc (`ipsw dyld disass` / ghidra-headless). Diff the selectors, the `vtpv` header and the message FourCCs against sheng's `VtWire.h` / `VtParavirt.cpp`. Also find out what makes a process take the paravirt path on 17 (service present only, or also `vmm-present` / IsVirtualDevice). | 2-3 days | **this decides B** | sheng VtWire.h |
| B1 | Get `vpu20_1v` from Android vendor (ROM session's `work/`), stage it like `scripts/stage-video-firmware.py`; check its HFI generation | 0.5 day | low | sheng script |
| B2 | Bring the VPU up: videocc parrot GDSC/clocks, RPMh MMCX/MXC + bus vote, PAS 9 with `pasBoot`, the TZ CP SCM, an S1 context for SID 0x2180/0x2187, VPU2 power sequence, HFI gen2 sys init → "firmware booted". Each stage behind a boot arg, as `ruan-wifi=N` does. | 1-2 weeks | med (Gunyah SMMU, VPU2 deltas, reset on carveout write) | `RuanWifiWpss.cpp` pasBoot/scm, `QcomRpmh.c`, `QcomMdt.c`, SmmuBypass/ArmSmmu4K, sheng `VideoProbe.cpp` + `drivers/QcomIris/src` |
| B3 | Decode session H.264 1080p → NV12 via IrisMedia; host self-test with a fixture clip | 1 week | med (VPU2 buffer sizes) | sheng `IrisSession.c`, `IrisBuffers.c` (+ mainline vpu2 calculator), `IrisMedia.cpp` |
| B4 | Publish `AppleVideoToolboxParavirtualizationDriver` (`vt-paravirt=1`) + VtHost; sandbox grant; play H.264 in Safari / YouTube | 1-2 weeks | med-high (17 vs 27 wire format) | sheng `VtParavirt.cpp`, `VtHost.cpp`, `VideoToolboxProbe.cpp` |
| B5 | HEVC (HEIC wallpapers without the overlay, HEVC clips), 10-bit, capped at 1080p for memory | 1 week | med | sheng HEVC fixes (coded-size rounding, persist buffer, `HFI_PROP_DECODE_ORDER_OUTPUT`) |
| C | Screen recording: §4 options 2 → 3 | 2-4 weeks | med | VtHost encoder half |

Test first: **A1** (free), then **B0** (offline, and it decides whether B is a port or a rewrite), then **B1/B2**.
Licensing: `drivers/QcomIris/src` is GPL-2.0-only (derived from Linux iris); the transport and host are BSD-2-Clause-Patent **[V]**.
