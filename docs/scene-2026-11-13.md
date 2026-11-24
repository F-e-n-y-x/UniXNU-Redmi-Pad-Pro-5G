# The XNU / macOS on Android scene (snapshot 2026-11-13)

Sources: GitHub (gh API), the UniXNU device index (368 device pages, cloned and parsed), the public Telegram preview of @unixnu, X and news sources found by search.
Reddit had no indexed threads; the scene lives on GitHub, Telegram, X, Bilibili and QQ. The UniXNU dev group @unixnuchat is private and dev-only, so it can't be read without joining.

## Timeline (Telegram @unixnu, public channel)

| Date | Event |
|---|---|
| 2026-10-19 | UniXNU open-sourced; iPadOS 17 on Xiaomi Pad 6S Pro (sheng) |
| 2026-10-20 | Adreno GPU working through Metal translation |
| 2026-10-21 | "macOS 26 arm64e running on QEMU using UniXNU"; a GitHub Actions macOS runner can do the Mac-only build steps |
| 2026-10-28 | iOS 27 arm64e boots on sheng using native ARMv9 PAC |
| 2026-10-31 | 120 fps Liquid Glass on iOS 27 |
| 2026-11-08 | "All drivers on iOS are almost done and we are working on macOS right now" |
| 2026-11-10 | macOS 27 on sheng: Wi-Fi, Bluetooth, Metal GPU, keyboard, touchpad, audio, mic, brightness and PD charging working |
| 2026-11-12 | macOS video: "almost done" |

## Native macOS ports

| Project | Device / SoC | PAC | State |
|---|---|---|---|
| [UniXNU/UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) | Xiaomi Pad 6S Pro, SM8550 | yes | macOS 27.3, most drivers working (per the channel and X) |
| [CrossDarwin/macOS-ForSheng](https://github.com/CrossDarwin/macOS-ForSheng) (+ forks Jelly10086 and others) | SM8550 | yes | macOS 26.5 bring-up |
| [lenzhang/UniXNU-xiaomipad8pro](https://github.com/lenzhang/UniXNU-xiaomipad8pro) | Xiaomi Pad 8 Pro, SM8750 | yes | macOS 27.3 installed on UFS and booting from power-on; Adreno 830 composites the desktop; keyboard cover, touch, camera frames, charging |
| [iOSNJBrepo/macOS-uke](https://github.com/iOSNJBrepo/macOS-uke) | Xiaomi Pad 7, SM7675 | yes | macOS 26.5 Finder desktop, Adreno 732 Metal |
| [TAY0123/UNIXNU_elden](https://github.com/TAY0123/UNIXNU_elden) | Legion Y700 Gen 4, SM8750 | yes | macOS Recovery, first frame |
| [Snapintosh/Snapintosh](https://github.com/Snapintosh/Snapintosh) | ASUS Zenbook A16, X2 Elite | yes | macOS 27 desktop; an EL2 monitor (not UniXNU) with a Mac tethered |
| Droid-VM/macOS-DroidVM | crosvm guest on Gunyah | yes | a VM, so out of scope |
| **[BxiiDN/macOS-Pad-6](https://github.com/BxiiDN/macOS-Pad-6)** | **Xiaomi Pad 6, SM8250** | **no** | **The only macOS-without-PAC attempt.** It does not boot yet. It has the tools: the FGE emulator, pacstrip and an instruction audit. |

## Apple OS on chips without PAC (the class SM7435 belongs to)

| Project | Device / SoC | Reached |
|---|---|---|
| **[mateusseiboth/darwin-lisa](https://github.com/mateusseiboth/darwin-lisa)** (MIT) | **Mi 11 Lite 5G NE, SM7325: Cortex-A78/A55 with no PAC, the same CPU class as ruan** | **arm64e iOS 26 cold-boots standalone, Settings usable, GPU-drawn glass UI** (handover of 2026-11-12). Pieces: `tools/pacstrip` (offline rewrite), `kernel/pacstrip` (C rewrite + trap emulation for LDRAA/LDRAB/FJCVTZS), a page-in "PAC pager" in mach_loader, `ml_grade_binary` and cpu-subtype patches, and dyld-pager / slide fixups (`kernel/pg16k`). This is the reference for a macOS no-PAC port. |
| [none24smu/UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta) | Surface Duo 2, SM8350 (no PAC) | iOS 26.5.2 rewritten offline ("29.6M PAC words lowered"); reached PID 1 |
| **[JosephMontfort/hakios-sm7435](https://github.com/JosephMontfort/hakios-sm7435)** | **POCO X6 5G (garnet), SM7435, the same SoC as ruan** | HakiOS (xnu-8796, iOS 16 generation): SpringBoard, 8 cores, FT3683G touch, buttons, UFS with writable /var, **Adreno 710 via freedreno GLES rendering SpringBoard**. Battery and audio are partial; no Wi-Fi or modem. Telegram: t.me/garnetglobal |
| [ry-diffusion/Bergamota](https://github.com/ry-diffusion/Bergamota) | Poco F1, SDM845 | **Arm64e iOS 18.7.8 (iPhone XR) userland to SpringBoard, rewritten by "Renegade" for a CPU without PAC.** This proves the static rewrite works. CPU rendering only. |
| [chiba233/Trogdor-ipadOS](https://github.com/chiba233/Trogdor-ipadOS) | Chromebooks, SC7180 | iPadOS 17 with Adreno 618 Metal and LTE data |
| rui12366B/UniXNU-gts7xlwifi | Galaxy Tab S7+, SM8250 | iPadOS 17 desktop with Adreno 650 compositing |
| DinnerABC/UniXNU-cheeseburger | OnePlus 5, SD835 | iPadOS 17 desktop and touch |
| dddqmmx/UniXNU-sirius | Mi 8 SE, SD710 | SpringBoard |
| zzweiqing/UniXNU-nabu | Xiaomi Pad 5, SD860 | iPadOS, 8 cores, touch, display |
| [vildangil/UniXNU-Dizi-Ruan](https://github.com/vildangil/UniXNU-Dizi-Ruan) | Redmi Pad Pro (dizi), SM7435 | early XNU entry only; README is still sheng's |
| [Scarpy19/UniXNU-garnet](https://github.com/Scarpy19/UniXNU-garnet) | garnet, SM7435 | scaffold, not bootable |

## How much of macOS needs fixing (measured by macOS-Pad-6 on real Apple binaries)

Instructions beyond ARMv8.2+RCpc+DotProd. This is the same instruction-set class as SM7435, confirmed with `probe/isaprobe.c` on our tablet:

| Class | dyld shared cache 26.7.2 (2.95 GB of code) | Kernelcache 26.5 (Mac mini M1) | Handling |
|---|---:|---:|---|
| PAC non-hint (RETAB, BLRAA, PACIA, AUTDA…) | 26,929,850 | 778,238 | pacstrip (static) + FGE |
| PACGA | 724 | 42 | FGE |
| SHA3 (EOR3, BCAX, XAR) | 14,766 | 257 | FGE |
| SHA512 + RAX1 | 1,197 (including dyld) | 154 | FGE |
| FJCVTZS | 1,009 (JavaScriptCore) | 0 | FGE |
| FCMA (FCADD, FCMLA) | 25,274 (BLAS, VectorKit…) | 0 | **not handled yet** |
| FHM | 128 (libBNNS) | 0 | not handled yet |
| DIT / SB / LRCPC2 | 192 / 7 / 15 | 27 / 4 / 4 | FGE |

Notes from that report:
- macOS userland has no arm64 fallback: only `dyld_shared_cache_arm64e` exists.
- dyld 26.5 has "function variants" chosen by `hw.optional.arm.caps` for sha3, fcma, jscvt and others, so some of those instructions may never execute. PAuth has no variant.
- The kernel must be arm64e with `HAS_APPLE_PAC` (or `ml_grade_binary` rejects arm64e) but run in "null-PAC" mode. It needs a constant thread-state hash, because otherwise every exception would execute 5 PACGA instructions and the emulation would recurse.
- Use the M1 (t8103) kernelcache: M2+ kernels use LRCPC2 about 600 times, and M1's never.

## Reddit (read through the live browser)

- Hubs: r/hackios, r/snapintosh, r/hackintosh.
- "macOS Tahoe on the Xiaomi Pad 7" (r/snapintosh, 886 upvotes, 2026-11-07).
- "Updates on macOS for Xiaomi Pad 6S Pro" (r/hackintosh, 2026-11-12).
- "MacOS tahoe running on Google Pixel 8a" (r/hackintosh, repo Rubanoxd/macos-pixel8a; Tensor G3 has PAC).
- iOS 26 on Exynos 9820 (no PAC), source not released (r/hackios 1xrar1m).
- Nothing about SM7435, Redmi Pad Pro or POCO Pad XNU on Reddit.
- HakiOS's "iOS 27 on mt6768/redwood" (2026-11-11/13) shows "iPhone X" on its About page with the iOS 16 look. It is probably a version-string spoof; unverified. Their own docs/comparison.md says HakiOS has no arm64e tool.

## Telegram channels found (not joined)

- t.me/oosexgangdevs (HakiOS dev), t.me/oos_chat (HakiOS chat, about 4.5k members), t.me/hakios_devblog

- https://t.me/unixnu (public channel), with @unixnuchat (dev-only group)
- https://t.me/garnetglobal and https://t.me/Garnet_Axion_Support (hakios-sm7435, POCO X6 5G)
- https://t.me/chiba2333 (HoshinoYumeka, baseband)
- QQ group for UniXNU's Chinese community (link in the UniXNU README)

## News

- HKEPC: hkepc.com/27142, /27237 (macOS 27.3 on the Pad 6S Pro)
- KOCPC: en.kocpc.com.tw/archives/26548
- pbxscience: "What UniXNU has actually proven"
- Bilibili BV1eiWR66E2C (Pad 6S Pro boots macOS 27.3), BV1wrx16AE7n (macOS 27.0 on an 8 Gen 3 device)
- X: x.com/UniXNU_new, x.com/sharky_dev
