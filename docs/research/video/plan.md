# Video decode / encode on ruan (iPadOS 17.7.11, 21H461): research and plan

Date: 2026-11-21. Research only: no device access. Update 2026-11-21 (later): B0 done (§6), B1–B3 staged as compile-checked code in `port/` (§7).
Tags: **[V]** = verified here (file, string or DT line read). **[I]** = inferred, still needs a test.

## TL;DR

1. iPadOS 17.7.11 already contains Apple's **VideoToolbox paravirtualization guest**: `VideoToolboxParavirtualizationSupport.framework` (AppleParavirtGPU-1), `AppleVideoToolboxParavirtualizationDriver`, `ParavirtualizedVideoDecoder` / `ParavirtualizedVideoEncoder` **[V, strings in the 21H461 dyld cache]**. VideoToolbox sends whole decode sessions to a kernel service with that name. We don't need to emulate AppleVXD / AVE.
2. **UniXNU-sheng already built this end to end** on SM8550: the Qualcomm iris VPU driver, a V4L2-shaped decoder device, and a kernel "host" behind `AppleVideoToolboxParavirtualizationDriver`. H.264 and HEVC (8 and 10 bit, up to 4K) decode on hardware there **[V, docs; tested on macOS 27.3]**. Its code is in `<work>/vg/UniXNU-sheng/drivers/QcomIris` (7.4 k lines) plus `package/VideoProbe.cpp` (677 lines).
3. ruan's VPU is the same family (iris2 on SM7435 "parrot", firmware `vpu20_1v`, PAS 9). The reserved memory, the TZ content-protection ranges and the IOVA pools in ruan's live DT **match the numbers sheng uses** **[V]**. PAS from XNU under Gunyah already works on ruan (zap shader, PAS 13) **[V]**.
4. Cheapest test that could already work today: VideoToolbox's **software H.264 decoder** (`H264SW.videocodec`), registered for `avc1` with **no defaults key** **[V, §6]**. A software HEVC decoder (`VCPHEVC.videocodec`, `hvc1`) **is** registered too **[V, §6]**; the earlier "no HEVC software decoder" was wrong, so HEIC fails for another reason **[I]**.
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
- 17.7.11 uses the **same service, selectors and header as 27**, with four message-level differences **[V, §6]**.
- The kernel side registers classes under Apple's names because the platform sandbox only lets a process open user clients that its profile names **[V, sheng comment]**.

### 1.3 Software decoders that exist in 21H461
| Codec | Software decoder | Evidence | Notes |
|---|---|---|---|
| H.264 | `H264VideoDecoder_CreateInstance/DecodeTile/…`, `ThreadCountH264SWDecoder`, `ForceSyncH264SWDecoder` | [V] | VideoToolbox's own CPU decoder. On j120ap it should be the fallback when no VXD service matches [I]. |
| HEVC | `VCPHEVC.videocodec` registers a software `'hvc1'` decoder unconditionally (@0x1c87080e4) | [V, registration only] | Corrected 2026-11-21 (§6). Whether its StartSession accepts on ruan is untested; HEIC fails for another reason [I]. |
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
| Firmware | `vpu20_1v`: **only on vendor** (`/vendor/firmware/vpu20_1v.mbn`, 2024040 B, sha256 3077519…89cf, plus split .mdt/.b00–.b19), not in modem_a/NON-HLOS; ELF32, 20 segments, span 0x6ff020 (fits 0x700000), **HFI gen 2** (89 `HFI_PROP_*` names, `HFI_CMD_SUBSCRIBE_MODE`) [V, stock OS3.0.305.0] | `vpu30_4v.mbn` |
| PAS id | **9** | 9 |
| Carveout | `video_region@86c00000`, size 0x700000 | 0x700000 |
| TZ CP config (svc 0x0c cmd 0x08) | DT pools: non-pixel 0x25800000 + 0xba800000, secure non-pixel 0x1000000 + 0x24800000 | `(0, 0x25800000, 0x01000000, 0x24800000)`, **same** |
| SMMU (apps SMMU, phandle 0x28) | venus_ns SID **0x2180 mask 0x20**, venus_ns_pixel **0x2187**; secure banks 0x2181/0x2183/0x2184 (vmids 9/10/11, leave alone) | 0x1940, 0x1947 |
| Clocks (videocc @0xaaf0000, `qcom,parrot-videocc`) | video_ctl_axi0, video_mvs0_axi, core, vcodec, iface, video_cc_iris_clk_src; rates 133.3/240/335/424/460 MHz | Sm8550VideoCc |
| Power | **Corrected:** iris-ctl-supply = `video_cc_mvsc_gdsc` **0xaaf5004** (phandle 0x317, status ok) and vcodec-supply = `video_cc_mvs0_gdsc` 0xaaf6004; mvs0c@0xaaf8084, mvs1, mvs1c are `disabled`. Both GDSCs hang off **CX** (parent-supply 0x11c = cx.lvl); parrot has no MMCX [V] | mvs0c/mvs0 on MMCX |
| Bus | interconnects `venus-cnoc`, `venus-ddr`; reg-preset <0xb0088 val 0 mask 0x11>; BCM **MM1** (enable mask 1) carries `qnm_video0` (icc parrot.c) [V] | RPMh MMCX/MXC nominal + MM1 |

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
| B0 | **Done 2026-11-21 (§6): same transport; 4 message fixes for B4.** **Offline:** disassemble `VideoToolboxParavirtualizationSupport` + VideoToolbox paravirt code from the 21H461 dsc (`ipsw dyld disass` / ghidra-headless). Diff the selectors, the `vtpv` header and the message FourCCs against sheng's `VtWire.h` / `VtParavirt.cpp`. Also find out what makes a process take the paravirt path on 17 (service present only, or also `vmm-present` / IsVirtualDevice). | 2-3 days | **this decides B** | sheng VtWire.h |
| B1 | **Staged (§7): firmware found on vendor, HFI gen 2; B2/B3 code in `port/`.** Get `vpu20_1v` from Android vendor (ROM session's `work/`), stage it like `scripts/stage-video-firmware.py`; check its HFI generation | 0.5 day | low | sheng script |
| B2 | Bring the VPU up: videocc parrot GDSC/clocks, RPMh MMCX/MXC + bus vote, PAS 9 with `pasBoot`, the TZ CP SCM, an S1 context for SID 0x2180/0x2187, VPU2 power sequence, HFI gen2 sys init → "firmware booted". Each stage behind a boot arg, as `ruan-wifi=N` does. | 1-2 weeks | med (Gunyah SMMU, VPU2 deltas, reset on carveout write) | `RuanWifiWpss.cpp` pasBoot/scm, `QcomRpmh.c`, `QcomMdt.c`, SmmuBypass/ArmSmmu4K, sheng `VideoProbe.cpp` + `drivers/QcomIris/src` |
| B3 | Decode session H.264 1080p → NV12 via IrisMedia; host self-test with a fixture clip | 1 week | med (VPU2 buffer sizes) | sheng `IrisSession.c`, `IrisBuffers.c` (+ mainline vpu2 calculator), `IrisMedia.cpp` |
| B4 | Publish `AppleVideoToolboxParavirtualizationDriver` (`vt-paravirt=1`) + VtHost; sandbox grant; play H.264 in Safari / YouTube | 1-2 weeks | med-high (17 vs 27 wire format) | sheng `VtParavirt.cpp`, `VtHost.cpp`, `VideoToolboxProbe.cpp` |
| B5 | HEVC (HEIC wallpapers without the overlay, HEVC clips), 10-bit, capped at 1080p for memory | 1 week | med | sheng HEVC fixes (coded-size rounding, persist buffer, `HFI_PROP_DECODE_ORDER_OUTPUT`) |
| C | Screen recording: §4 options 2 → 3 | 2-4 weeks | med | VtHost encoder half |

Test first: **A1** (free), then **B0** (offline, and it decides whether B is a port or a rewrite), then **B1/B2**.
Licensing: `drivers/QcomIris/src` is GPL-2.0-only (derived from Linux iris); the transport and host are BSD-2-Clause-Patent **[V]**.

## 6. B0 result: iOS 17.7.11 guest vs sheng's host (2026-11-21, dsc 21H461, scratch `…/scratchpad/b0/`)

**Transport: identical [V].** `VideoToolboxParavirtualizationSupport` (0x61c bytes of code), `SetUpWithHandlers` @0x219bff784:
- `IOServiceMatching("AppleVideoToolboxParavirtualizationDriver")` + `IOServiceGetMatchingService`; **no registry property is read**.
- `IOServiceOpen` type 0, no `IOConnectMapMemory`. sel 0: 1 scalar in (0x4000, re-called page-rounded for bigger messages) → 3 out
  (capacity, send buffer, queue) @0x219bffb10; sel 1/2: 16-byte struct in; sel 3: 6 scalars in (length + ≤5 IOSurface IDs) → 1 out,
  message ≥ 48 bytes @0x219bffd34; sel 4: no args. Notification: `IODataQueueAllocateNotificationPort` type 0, 16 KiB dequeues.
- Header `'vtpv'` @0x197bda7a4: size, `vtpv`, type, flags, UUID, 8-byte request id, 8 zero bytes = 48 bytes. Same as `VtWire.h`.

**Messages (decoder vtable @0x1e74257a0: `dinv -dec dcop dset dsta dfrm dsup dfin dsts dtil dtfi`):**

| 17.7.11 | sheng (`VtHost.cpp`, `VtWire.h`) |
|---|---|
| `decs` `,ver`=3, reply `,dec` plist; `+dec` `,cod` `,ver`=3 `,cid`, reply `,err` `,ver` | same (sheng answers ver 6: fine; extra `,hin` ignored) |
| `dsta` `,fds`; `dsup`→`,sup`; `dfin` `dinv` `-dec` `dcop` `dset` | same |
| `dfrm` `,frm` `,dfl` `,sbf`; samples > 2 MiB split, first part async with flag 0x10 | **differs:** sheng has no continuation (big keyframes) |
| host→guest `Dbuf Demt Dpba` | same |
| `Dfub`: key **`,uui`** type **`=ui+`** @0x197aeb42c | **differs:** sheng sends `,uu+` / `=uu+` (`VtHost.cpp:753`, `VtWire.h:245`) |
| guest also takes `Dems Dmie Dmif TDel TDem TDsr Bye!`; tile/JPEG `dsts dtil dtfi jcap jdec jenc` | absent in sheng; not on the plain decode path [I] |
| — | sheng's `dmul`/`emul`: 17 never sends them |

**What selects the paravirt path [V]:** when VideoToolbox builds its decoder registry (0x197abe834) it loads its own bundles, then
registers `paravirtualized:%@` decoders by sending `decs` (0x197abe860), unless restriction byte 0x1edd15aba is set (only
`VTRestrictVideoDecoders`/`VTApplyRestrictions` bit 1; WebCore soft-links it, probably for WebContent [I]). **No sysctl, no
`hv_vmm_present`, no MobileGestalt, no defaults key**: the service existing and answering `decs` is enough.
`AllowParavirtualizedDecoder` is the decoder-spec key `kVTVideoDecoderSpecification_AllowParavirtualizedDecoder`, default allowed
(only an explicit `kCFBooleanFalse` excludes, @0x197abd8c0). `EnableParavirtualizedProtectedCodecs` is an `os_feature` (CoreMedia),
protected/ProRes only.

**A1 [V]:** no key or plist. VideoToolbox (@0x19789f5f0) always dlopens `H264SW.videocodec` → `VCPAVCRegisterDecoder` registers `avc1`
(`IsHardwareAccelerated=false`); `H264H8Register` (@0x201874d28) registers nothing without an `AppleD5500` service. On ruan the
software decoder is the only avc1 decoder. A client that sets `RequireHardwareAcceleratedVideoDecoder` fails [I]. A1 is a plain test.

**Not checked:** `,sbf` (`sbuf`/`sdat`) byte layout; the sandbox profile (none in the RAM root) for WebKit's GPU process opening the
user client; whether `H264SW`/`VCPHEVC` StartSession refuses on ruan for other reasons.

## 7. B1–B3: staged code (`port/`, compile-checked, NOT built into a kernel, NOT booted)

| Path | What | Licence |
|---|---|---|
| `port/package/RuanVideoCc.{h,c}` | parrot videocc + GCC video branches: Lucid EVO PLL0, iris root (133.33/240/335/424/460 MHz), MVSC/MVS0 GDSCs, branches; survey and `vpu_accessible` gate. Shape from UniXNU-sm8550-common `Sm8550VideoCc.c` 2dc650c, values from downstream `videocc-parrot.c`/`gcc-parrot.c`/`msm_vidc_iris2.c` | GPL-2.0-only |
| `port/package/RuanVideo.cpp` | the board file (sheng `VideoProbe.cpp` 2359a86 ported): `ruan-video=1..5`, RPMh cx/mx/MM1 on a chosen TCS, PAS 9 with ruan's SCM/metadata/byte-store lessons, TZ CP call, S1 domain on SIDs 0x2180/0x21a0/0x2187, SPI 174, core init, session test, USB `video`/`video.log` | BSD-2-Clause-Patent |
| `port/package/Sm8550AppsSmmu.{h,c}` | unchanged from UniXNU-sm8550-common 2dc650c (apps SMMU at 0x15000000 on parrot too) | BSD-2-Clause-Patent |
| `port/patches/qcom-iris-ruan.patch` | on sheng `drivers/QcomIris`: iris2 one-pipe power-off halves (`vpu2` flag), UBWC HBB as `-DQCOM_IRIS_UBWC_HBB` (ruan 14: LPDDR4X, DT `ddr_device_type` 7), host test | GPL-2.0-only (headers kept) |
| `port/patches/video-port.patch` | `unixnu-ruan`: build hook (`RUAN_VIDEO`, default 1), `BootProgress.c` strong start, late-probe `ruan_video_init`, USB `video`/`video.log`. `patch --dry-run` clean vs the working tree 2026-11-21 (UsbDebugCdc.cpp has uncommitted edits) | — |
| `port/scripts/fetch-qcom-iris.sh` | UniXNU/UniXNU-sheng **2359a862** `drivers/QcomIris` → `.cache/QcomIris` + patch (2359a86 = 9252d988 + log-video/alloc fixes) | — |
| `port/scripts/stage-video-firmware.py` | vpu20_1v from .mbn, .mdt+.bNN, a vendor dir or the vendor EROFS image → `OUT/firmware/qcom/vpu20_1v.mbn`; refuses OUT inside git; `--self-test` | — |
| `port/check/compile-check.py`, `video_cc_host.c` | all port C/C++ + patched QcomIris + IrisMedia.cpp with ruan's kernel compile records (-Werror), Mach-O checked; host test of RuanVideoCc | — |

**Licence:** sheng's `drivers/QcomIris/src`, `include`, `shim`, `kernel/IrisMedia.*` are **GPL-2.0-only** (derived from Linux iris);
`VtHost.cpp`/`VtParavirt.cpp`/`VtWire.h` and `VideoProbe.cpp` are BSD-2-Clause-Patent. `RuanVideoCc.*` is GPL-2.0-only (derived from
GPL kit + downstream clock drivers). Headers are kept; the GPL parts are fetched, never vendored into the repo; linking them into the
kernel makes the kernel image a GPL-2.0 combination for distribution purposes.

**Checks run [V]:** `check/compile-check.py` 14/14 PASS (RuanVideoCc.c, Sm8550AppsSmmu.c, RuanVideo.cpp, 8 QcomIris src, iris_vpu_buffer.c,
IrisMedia.cpp); patched BootProgress.c/UsbDebugCdc.cpp/LegacyDisplayProbe.cpp `-fsyntax-only` PASS; undefined symbols all resolve to
XNU, `libRamRootProbe.a` (sheng_fw_*, qcom_mdt_*, qcom_rpmh_*), ArmCpu (`armcpu_spi_*`) or the ArmSmmu4K kit objects already linked
by build-ruan-adreno (6/6 found); host tests: video_cc_host, QcomIris vpu_host (with vpu2), buffers_host; staging script self-test and
a real run (vendor EROFS and split .mdt give the same bytes). Not run: QcomIris hfi/core/session/mmu host tests (arm64 `dsb` in
`IrisHfi.c`, this host is x86).

**Apply (the session that owns `macOS/unixnu-ruan`):** `cp port/package/* unixnu-ruan/package/`; `cp port/scripts/* port/patches/qcom-iris-ruan.patch`
into `unixnu-ruan/scripts/` and `patches/`; `patch -p1 < port/patches/video-port.patch`; build as usual (first build fetches GitHub + kernel.org);
stage firmware: `stage-video-firmware.py <vendor_a.img|vpu20_1v.mbn> /root/unixnu-work/video-fw`, then `DATA_OVERLAY=` it (RAM root) or
copy to `/private/var/firmware/qcom/` on the UFS root.

**Test order** (one RAM-only `fastboot boot` each, cleared with the user and the ROM session; read over USB with `video`, `video.log`):

| Boot arg | Writes | Pass string (USB `video`) |
|---|---|---|
| `ruan-video=1` | nothing | `video st 1 err 0 gdsc …/… pll … smmu <bank|asid<<8|oas<<24> free <n> rails 7 fw 1976K span 1791 load 0` (a failed stage shows `st -<stage> err <code>`: 12 SIDs already routed or PLL0 already running, 6 no usable SMMU bank, 3 no firmware file, 2 no cmd-db) |
| `ruan-video=2` | RPMh cx/mx/MM1, videocc, GDSCs | `st 2 … gdsc <off>/<off> vpu <power status>/<ctrl status> cc 0` (`cc` = failed step \| value<<8 otherwise) |
| `ruan-video=3` | + SMMU bank/SMRs, carveout, PAS 9, VPU | `st 8 … load 0 scm 0/0 irq >0 core 2` and `video.log` 0 = the firmware version (`ruan-video-order=0` retries Linux's PAS order) |
| `ruan-video=4` | + one session | `st 9 … sess 0` |
| `ruan-video=5` | B4 | `st 10` once VtHost is linked |

Knobs: `ruan-video-mhz=133|240|335|424|460`, `ruan-video-delay`, `ruan-video-fw`, `ruan-video-tcs`, `log-video=1`.

**Inferred, needs the device [I]:** Gunyah lets HLOS PAS 9 + the CP call (zap PAS 13 worked; video's CP svc 0x0c/0x08 untested); the
Lucid EVO even post-divider 0 = ÷1; GDSC polling on GDSCR bit 31 (no `qcom,poll-cfg-gdscr` in the DT); GCC video AXI0 is needed though
the vidc node does not list it; MM1 alone is enough bus (display holds DDR); no SMR already routes 0x2180/0x21a0/0x2187 under Gunyah;
sheng's gen2 property set is accepted by vpu20_1v; the TCS 1 / cx-vote sharing with RuanWifiWpss (shortcut, see RuanVideo.cpp header).

**Left for B4–B5:**
- B4: link `VtParavirt.cpp` + `VtHost.cpp` (stage 5) with the three 17.7.11 fixes: `Dfub` key `,uui`/`=ui+`; `dfrm` continuation
  (flag 0x10); an error reply for unknown types. Register classes late (sheng's `VT_LATE_INIT`), find the sandbox rule for the user
  client, then Safari/YouTube H.264. Not needed: any defaults key or vmm flag.
- B3 rest: a real decode self-test (fixture H.264 IDR → NV12 via `IrisMedia`), buffer caps for parrot (max 4096x2176, mbpf 69632
  instead of sheng's 8192 sizes, saves memory), stage 1 vs 2 work mode for 1-pipe.
- B5: HEVC/10-bit, 1080p cap, power collapse (`set_remote_state`) and the autosuspend under Gunyah.
