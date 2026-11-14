# ruan display: backboardd -> GOP framebuffer (iPadOS 17.7.11 21H461)

Research and preparation only (2026-11-14). Nothing here was applied to UniXNU-ruan or UniXNU; no device access.
Files in this directory:

| Path | What |
| --- | --- |
| `PLAN.md` | this plan: pieces, ruan values, order of work, risks |
| `VERIFY.md` | kernelcache pin table (50/50), dry build result, text-fixup check |
| `diffs/all.patch` | everything below as one `patch -p1` for UniXNU-ruan (applies to the 2026-11-14 05:18 files) |
| `diffs/000{1..5}-*.diff` | the same, one file each |
| `proposed/` | the patched copies (`board.toml`, `configs/ruan_defconfig`, `package/BootProgress.c`) and the two new files |
| `proposed/orig/` | the UniXNU-ruan files the diffs were made against |
| `board/text_fixups.toml` | the 13-entry list on its own, with comments |
| `tools/verify-kernel-pins.py` | re-checks every pinned kernel address/byte string in a kernel.macho |
| `tools/check-text-fixups.py` | re-checks text fixups against extracted dyld subcaches |

## How the reference boards present on 21H461

There is no Apple display driver to port and no kernelcache bytes are patched. The "vtable patch" is a build-time
binding: UniXNU's `NativeLegacyDisplay` is a C++ subclass of Apple's prelinked, unchanged `IOMobileFramebufferLegacy`
(IOMobileGraphicsFamily). The board builder generates the original class's header (0x18a0 bytes, 358 virtual slots),
compiles the subclass, checks that its vtable overrides exactly the 15 qualified slots plus the 9 abstract leaves, and
emits `.set` symbols for the 193 original methods/getters it calls (absolute 21H461 kernelcache addresses). At boot:
`bsd_init` loads IOMobileGraphicsFamily and registers the class; after `IOKitBSDInit` the board probe loads IOSurface,
publishes a `NativeGOPFramebuffer` nub (the GOP memory, under `/arm-io` as `display-subsystem`) and starts the display on
it. backboardd then opens `IOMobileFramebufferUserClient` like on an iPad; each swap is validated and its IOSurface is
copied by the CPU into the GOP framebuffer, with a 60 Hz software vsync. With no GPU, QuartzCore renders the layers on
the CPU (proven on 21H461: UniXNU-sirius reached the Setup screen, UniXNU-muyu the desktop at about 1.4 fps, both
without a GPU driver).

## The pieces

"In build" = already in our ruan build today (xnu-patches.list, OS profile, UniXNU checkout). Sizes are lines.

| # | Piece | Source | What it does | Board dependency, ruan adaptation | Size | State |
|---|---|---|---|---|---|---|
| 1 | bsd_init / bsd_autoconf hooks | UniXNU `patches/xnu/offline-component-load-probes.patch`, `offline-legacy-start.patch` (arch series) | `offline-mobile-graphics-load=1` loads IOMobileGraphicsFamily; with `XNU_NATIVE_LEGACY_DISPLAY` calls `native_legacy_display_probe()` and, after IOKitBSDInit, `native_legacy_display_late_probe()` | none | 190 + 19 | **in build** (only the CONFIG switch is off) |
| 2 | Archive force-load | UniXNU `scripts/build-xnu-4k-baseline.sh` | with `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` force-loads `build/native-legacy-display/libNativeLegacyDisplay.late.a` after checking its build report hashes | none | - | **in build** |
| 3 | Prelinked families | OS profile `[kernel.prelink] roots` | IOMobileGraphicsFamily, IOSurface, IOHIDFamily, IOHIDEventDriver are prelinked | none | - | **in build** |
| 4 | Text fixups (mechanism) + entries 1-4 | UniXNU `native-text-fixup.patch`, OS profile `text_fixups` | patches signed pages at page-in when the original bytes match | none | - | **in build** |
| 5 | Key store stand-in | UniXNU-ruan `KeyStoreProbe.cpp`, `ruan-keystore-probe.patch` | keybag "unlocked, no passcode" for SpringBoard/backboardd | none | - | **in build**; needs `aks=1 offline-key-events=1` on the command line (piece 10) |
| 6 | Owned display backend | UniXNU `drivers/GopFramebuffer/NativeLegacyDisplay.cpp` (+ `NativeLegacyRequest.h`, `OriginalMobileFramebufferABI.h`), HEAD b0d812d | the IOMobileFramebufferLegacy subclass: boot mode from PE_Video, PM, swap validation, BGRA/compressed/half-float CPU copy, vsync | none (reads PE_Video). Use as is: the 5 extra imports of HEAD are macOS-only (`NATIVE_LEGACY_MACOS`) | 1230 | source present (component selected), **not compiled** |
| 7 | Class audit | UniXNU `scripts/audit-mobile-graphics-legacy.py` | re-derives the 358-slot layout from the reference kernel, checks the family's text hash | 21H461 kernelcache (verified, VERIFY.md rows 1-11) | 87 | present, not run |
| 8 | Archive builder | gts7xlwifi `scripts/build-gop-legacy-display.py` -> **`proposed/scripts/build-gop-legacy-display.py`** | runs 7, generates the ABI header, compiles 6 and 9, checks vtable + every pinned address/byte, emits `original-bindings.S`, strips the static initializer, archives | kernelcache addresses (all 50 verified on our kernel.macho); no SoC dependency. Adapted: display only (no presenter/HID/keystore/cpufreq/T970 services), compiles with the record's recipe itself (host clang-24 fix), basename-preserving staging | 705 -> 330 | **new**, dry-built OK |
| 9 | Board probe | gts7xlwifi `package/LegacyDisplayProbe.cpp` -> **`proposed/package/LegacyDisplayProbe.cpp`** | the two entries, the nub, the hooks the backend imports, link stubs (`native_adreno_fences*`, `native_ufs_dpu_*`, `native_gpu_trace_*`), the IOSurface stride-in-pixels wrapper, scanout hand-off with BootProgress, once-only panel bands 7/8/9 | none left (all T970 parts removed) | 1264 -> 339 | **new**, compiles `-Werror` |
| 10 | Kernel arguments | gts7xlwifi/j716f/sirius board.toml | `offline-mobile-graphics-load=1 offline-legacy-register=1 offline-legacy-phase=4 offline-legacy-gop=1 offline-legacy-pm=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1 vsync=60 aks=1 offline-key-events=1`; never `-v` | none | 279 chars total | **new** (diff 0001) |
| 11 | ADT identity for the display | gts7/j716f `[adt.chosen.u32] display-scale`, `[adt.product.string] artwork-display-gamut` | scale 2 (1600x2560 -> 800x1280 pt, j716f has the same GOP size), rotation 0, gamut sRGB | panel size only | 9 | **new** (diff 0001) |
| 12 | Display format fixups 5-12 | gts7xlwifi board.toml (UniXNU-sm-t733 policy "21H461-srgb-bgra8-1") | RenderBox picks BGRA8; QuartzCore: no CIF10, no universal/lossy compression, no EDR/HDR. Keeps every surface BGRA8, which piece 6 copies directly (it has no XR10 path; gts7 saw XR10 stripes without these) | none; verified byte-exact in 044-76034-038.dmg | 8 entries | **new** (diff 0001) |
| 13 | CoreUI no-GPU fixup (13) | UniXNU-sirius / j716f board.toml | `+[CUIShapeEffectStack sharedCIContext]` -> CI::GLContext::init calls a zeroed GL table and kills iconservicesagent without a GPU; the bl becomes `mov x0, xzr` | none; verified byte-exact | 1 entry | **new** (diff 0001) |
| 14 | CONFIG switch | gts7/j716f/sirius defconfig | `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` | none | 5 | **new** (diff 0002) |
| 15 | Boot page yields the panel | gts7 BootProgress.c `boot_progress_prepare_scanout` -> ruan's own BootProgress.c | before the first CPU copy the page stops drawing for good (else it repaints over SpringBoard) | ruan's BootProgress.c | +25 | **new** (diff 0003), compiles |

Not needed for the first frame, left out on purpose: `NativeGOPPresenter` (GPU hand-off, `fb-gpu`), AdrenoMetal and the
GPU driver, DPU direct scanout (`dpu-scanout`), `offline-legacy-unit-scale` (static ADT scale instead), the HID/touch
stack, backlight, buttons, CPU frequency, gts7's BootWatchdogPolicy (`lite`/`disable-watchdogd`/`disable-jobs`: add only
if watchdogd starts panicking the box).

Later pieces (after the first frame), with their reference sources:

| Piece | Source | ruan adaptation | Size |
|---|---|---|---|
| HID publisher | UniXNU `scripts/build-sheng-hid-input.py` + component ShengHidInput (IOHIDDevice ABI pins: VERIFY.md rows 41-50, all match) | Linux host: gts7's copy replaces `xcrun --sdk iphoneos`/`brew --prefix llvm`; add its two objects to the archive | 120 |
| Touch | catalog ShengNovatekTouch (NT36532) + QcomGeniSpi; gts7/j716f TouchProbe for the board half | ruan's touch is `novatek,NVT-ts-spi` on `spi@a8c000`; the panel is NT36532 BOE, so the sheng NT36532 core is the closest match | large |
| Brightness | j716f `AppleARMBacklight.cpp` (324) + `BacklightProbe.cpp` (96) | ruan is `bl_ctrl_external`: SY7758 on I2C (below), not DCS and not WLED | ~400 |
| Buttons | gts7 `ButtonProbe.cpp`, `[adt.buttons]` | PMK8350 PON `pon_hlos@1300` (SPMI sid 0): pwrkey + resin | ~300 |
| Speed | `cpus=8` (SMP patches), cached source mapping (sirius `sirius-fb-cached`) | a 16 MB copy per frame on one A55 | - |

## ruan values a port needs

| Value | ruan | Source |
|---|---|---|
| GOP framebuffer base | `0xB8000000` (UEFI "Display Reserved", Android `splash_region`/`cont_splash_region` 0xB8000000+0x2B00000) | ruanPkg `MemoryMapLib.c:34`, live DT, UniXNU-ruan `dts/ruan.dts` |
| GOP size | 1600 x 2560 x 4 = 16,384,000 bytes (0xFA0000) used; region 0x2B00000 | ruanPkg `ruan.dsc:60-62` (`PcdFrameBufferWidth/Height/ColorDepth` 1600/2560/32), `SimpleFb.c:169` |
| Stride | 6400 bytes (`PixelsPerScanLine` = width = 1600) | `SimpleFb.c:177` |
| Pixel format | `PixelBlueGreenRedReserved8BitPerColor` = BGRX8888 (memory B,G,R,X); IOSurface 'BGRA' copies 1:1 | `SimpleFb.c:180-182` (ColorDepth 32) |
| Orientation | portrait 1600x2560, `v_rotate` 0 (required by the probe) | GOP mode; BootProgress already draws through it |
| Panel | `xiaomi n83 35 02 0a cphy nt36532 boe`, NT36532 driver IC, DSI video mode, C-PHY, DSI ctrl0/phy0, DSC 1.2 (10 bpc -> 8 bpp, 2 slices of 800x20), 1600x2560, dfps 120/90/60/50/48/30, panel peak 0x626b50 | live DT `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_n83_35_02_0a_wqxga_video_cphy` (line 20742) |
| Physical size | 162.9 x 260.6 mm (DT 0x65d x 0xa2e, 0.1 mm), about 249 ppi | same node |
| MDSS | MDP `0xAE00000+0x84000`, DSI ctrl0 `0xAE94000` (dsi-ctrl-hw-v2.6) | DT inventory |
| Backlight | **external**: `qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_external"`, max level 0xFFF; enable GPIO = TLMM GPIO 57 (`qcom,platform-bklight-en-gpio = <&tlmm 0x39 0>`); driver IC **Silergy SY7758** (`sy,sy7758`) at **I2C 0x2E** on QUPv3 GENI I2C `0x990000` (SPI 605, status ok) | live DT lines 15651-15672, 20744-20748 |
| Panel bias | OCP2131 at I2C 0x3E on the same bus (LCM CS GPIO 13, IOVDD GPIO 6, ENN GPIO 104, ENP GPIO 105) | live DT 15673 |
| WLED / PWM | not used: `qcom,wled@d800` (`qcom,pm6150l-spmi-wled`, ctrl 0xD800 / sink 0xD900) on **PM6150L, SPMI sid 5** is `status = "disabled"`; the LPG `pwms@e800` is disabled too. Pin states exist but nothing references them from the panel: PM6450 (sid 1) GPIO7 `lcd_backlight_pwm_default` (func1), PM6150L (sid 4) GPIO10 `lcd_backlight_en_default`. (Parrot has no PM8550: its PMICs here are PMK8350 sid 0, PM6450 sid 1, PM7250B sid 2/3, PM6150L sid 4/5.) | live DT 8110, 8178-8200, 8450-8460, 8490, 8670-8679 |
| Reset GPIO | TLMM 98 (`qcom,platform-reset-gpio = <&tlmm 0x62 0>`) | panel node |
| Touch | `novatek,NVT-ts-spi` at `/soc/spi@a8c000/touch@0` | DT inventory |

The first milestone does not touch any of this hardware: the panel, DSC link and backlight stay as XBL/UEFI left them
(the boot page proves the scanout is live), and the display driver only writes pixels into 0xB8000000.

## Order of work

0. Wait for the `/private/var` work to land (it edits the same `board.toml`). Re-run `patch -p1 --dry-run < diffs/all.patch`
   in UniXNU-ruan; if it no longer applies, merge the hunks by hand (they are small and independent).
1. Copy in: `scripts/build-gop-legacy-display.py`, `package/LegacyDisplayProbe.cpp`, and apply the `board.toml`,
   `ruan_defconfig`, `BootProgress.c` diffs. A previous kernel must exist in `out/ruan/.../kernel/` (it does); on a clean
   tree build once with the switch off first, because the builder takes that kernel as its reference candidate.
2. `./unixnu build kernel`: the archive step must print `PASS: libNativeLegacyDisplay.late.a ...`; the link must succeed
   (only `___ubsan_handle_add_overflow_minimal` is not an exported kernel symbol, and the kernel link has it).
   `python3 display-prep/tools/verify-kernel-pins.py <new kernel.macho>` must still say 50/50.
3. Payload as today plus the test-only arguments `boot-progress=1 offline-legacy-reached=1 offline-legacy-verdict=1`
   (verdict panics with the failing step's name; drop both once a frame shows).
4. Boot (RAM-only, as today) and read the panel.

### Minimal first milestone: "backboardd draws one frame, no touch"

Pass criteria, all readable from one photo of the panel with `offline-legacy-reached=1`:
- band 7 (grey, 7th sixteenth from the top) appears: NativeLegacyDisplay started and registered (kernel side done);
- band 8 (violet): backboardd opened the framebuffer and submitted a swap;
- band 9 (white): a swap was validated and copied into 0xB8000000;
- then the boot page is gone and the panel shows backboardd's own frame (black, the Apple logo, or SpringBoard).

Each missing band names the next thing to look at: no 7 -> verdict panic text (probe/attach/start, IOSurface wait, PM);
7 without 8 -> backboardd not presenting (isMainDisplay, launchd, keybag, display power); 8 without 9 -> swaps rejected
(`native_legacy_swap_validation_reason`, surface format: check the text fixups were applied, console logs
`text-fixup ... applied`); 9 but black -> the CPU render path (CoreUI/CI, data migration, Setup.app absent).

M2: SpringBoard's lock/home screen stays on screen (display-sleep handling, see risks). M3: brightness (SY7758), touch
(NT36532 + HID), buttons, `cpus=8`.

## Risks and unknowns

1. **CPU composition without a Metal device.** Expected to work: sirius (no GPU) and muyu (21H461 line, CPU composed at
   ~1.4 fps) show it; QuartzCore falls back to its software renderer. Not yet seen on ruan. Sirius first saw black
   frames (8 swaps, all black) before the key store stand-in and the CoreUI fixup; ruan has both now (pieces 5, 13).
2. **Speed.** 16 MB per frame, one Cortex-A55 (`cpus=1`), uncached mapping: sirius measured 182-200 ms per 9.4 MB
   uncached and 9.7 ms cached; expect 0.5-3 fps until SMP and a cached source mapping. CPU rendering of the layers
   competes for the same core. Slow is fine for M1; a watchdog (backboardd/SpringBoard hang detection, watchdogd) might
   not be: gts7 adds `disable-watchdogd=1 lite=1` (their BootWatchdogPolicy.c), ruan has no such policy yet.
3. **Display sleep.** With `offline-legacy-pm=1` backboardd drives display power; sirius logged power 1 then 0 about
   40 s later. Without touch nothing wakes it, so the UI may blank after the auto-lock timeout. Photograph early, or later
   seed the Data volume's auto-lock preference to never.
4. **RAM root content.** make-ramroot.py drops `/Applications` (no Setup.app, no PosterBoard), `.car` files over 1 MB in
   SpringBoard.framework and some frameworks. SpringBoard staying alive was already seen; what it draws without Setup
   and posters is unknown (first-boot Setup/migration states are the usual blockers: sirius waited on DataMigrator).
5. **HID/keyboard.** Not needed to present: sirius reached Setup without any touch driver. backboardd logs missing
   digitizers but presents. Touch later needs the HID publisher (Linux-host build fix as gts7) and an NT36532 SPI core.
6. **IOMFB_FDR_Loader exit 1** is the display-calibration (FDR) loader for Apple panels; expected to fail here, harmless
   on gts7/j716f.
7. **Text fixups apply per signed page at page-in.** They need the exact 21H461 subcaches (verified) and the
   native-text-fixup patch (in build). Without fixups 5-12 surfaces may be XR10 ('b3a8'), which upstream
   NativeLegacyDisplay does not convert (gts7's private copy does): swaps would be refused (band 8 without 9).
8. **Bands and boot page.** BootProgress must yield (diff 0003) or it paints over the UI every second. The panic screen
   (`panic_screen = true`) still draws on panic.
9. **Unverified on hardware:** the final kernel link with the archive (only the objects and their symbols were checked),
   DarwinBoot's PE_Video values on ruan with the display path enabled (v_depth 32, v_rotate 0, v_display 1 without -v;
   BootProgress's own binding suggests they hold), and the ADT `/arm-io` node that phase 4 attaches under (devicetree.py
   always writes it).

## Effort and odds

- Merge + first build: 1-2 hours, mostly re-basing `board.toml` onto the other agent's edits. The archive already builds
  from these files in a scratch tree.
- First boot test to M1: 1-3 boot cycles if it goes like gts7/sirius; each failure mode has a band or a verdict string.
- Odds of M1 (some backboardd frame on the panel) within a day of device testing: about 60-70%. The kernel side is
  low-risk (same family bytes as three boards that present; 50/50 pins; dry build passes). The uncertain part is
  userspace on ruan's trimmed RAM root: first-boot state, missing apps, one slow core, and whether backboardd powers the
  display on at all before something times out.
- Odds of a usable SpringBoard home screen (M2) without further userspace work: about 35-45%; expect at least one
  round of Data-volume preference seeding (setup done, auto-lock) or watchdog relief.
