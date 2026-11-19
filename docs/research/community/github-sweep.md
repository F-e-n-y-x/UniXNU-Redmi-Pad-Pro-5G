# GitHub sweep: UniXNU / HakiOS ports useful for ruan (2026-11-19)

Research only. Clones (depth 1 unless noted) are in `/root/unixnu-work/vg/<name>`. Nothing was built and no device was touched.
Our state: IPADOS-BRINGUP.md §26 (rbu79/rbu80). Problem numbers below follow the brief: 1 fps / IOMFB, 2 DPU scanout, 3 silent
resets, 4 boot time, 5 UFS / Wi-Fi / audio / battery.

## Copy these first (ranked)

1. **Real vblank from the SM7435 DPU interface counters (problems 1, 2).** HakiOS on garnet (same SoC, also under Gunyah)
   reads INTF_1 at `0x0ae36000` (`INTF_TIMING_ENGINE_EN 0x000`, `INTF_FRAME_COUNT 0x0ac`, `INTF_LINE_COUNT 0x0b0`) from the
   kernel without faulting and uses it to time frame writes and to measure the real panel rate.
   Source: `hakios-xnu-kernel/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (`beamProbe` ~l.953, `beamNextScan` ~l.1008,
   `beamWait` ~l.1036, `fb_measure` ~l.290), boot args in `hakios-sm7435/bootargs.md` (`fb_intf=0x0ae36000`, `fb_measure=1`).
   The pong port locks CoreAnimation's vsync to the same counter: `UniXNU-t970/patches/xnu-ipados18/*pong-fb-pacing.patch`
   (`fb_vsync_lock=1`). For us, it gives a hardware tick to drive `native_legacy_vsync_notify` (already bound in our
   `scripts/build-gop-legacy-display.py`) instead of the 60 Hz timer. First step: read only (`fb_measure`-style, two reads
   1 s apart) to confirm ruan's panel is on INTF_1 and the counter runs. Ruan's panel may be dual-DSI (INTF_1 + INTF_2); check the
   live DT `qcom,mdss_mdp` first.
2. **SMP / a big boot core (problem 1).** We boot `cpus=1` on CPU 0, an A55. HakiOS runs SM7435 with `cpus=8 bootcpu=7`
   (A78, ~2.5x a silver core) and splits the frame copy over cores (`fb_threads=6`): `hakios-sm7435/metal.args`,
   `soc/configs/sm7435.config`, `bootargs.md`; kernel side `hakios-xnu-kernel/xnu/files/osfmk/arm64/loader_return.c` l.253
   (`bootcpu`). Under Gunyah, SGIs are not delivered: asteroids (SM7635) uses `sheng-smp=1 armcpu-spi-ipi=1`
   (UniXNU-core `docs/devices/asteroids.md`); elden's two-core admission found IPI counters stuck at 0
   (`UniXNU_elden-zks/drivers/EldenCpuActivation/README.md`). The cheap test is a single CPU, moved to core 7.
3. **Gunyah virtual watchdog (problem 3).** `UniXNU_elden-zks/drivers/EldenCpuActivation/GunyahWatchdog.{c,h}`, `GunyahSmc.c`:
   the SMC ABI (`0x86000005` CONTROL, `…06` STATUS, `…07` PET, `…08` SET_TIME), 27 s bark / 30 s bite, STATUS first to
   learn whether the watchdog is already armed at hand-off. Stock Android binds `gh_virt_wdt` to `qcom,gh-watchdog`. HakiOS passes
   `hv_wdt_pet=1` on every run (`hakios-xnu-kernel/tools/boot-args.py` l.86). Our `package/BootReset.c` already has the same
   SMC IDs behind `ruan-wdt-pet=1`, which is off. Next: log STATUS once after launchd, then do one series with `ruan-wdt-pet=1`
   and compare reset rates.
4. **DPU scanout of our own buffer (problem 2).** `RigoLigoRLC/unixnu-nativemobiledisplay` (SM7325, a GopFramebuffer fork):
   `NativeDpuScanout.cpp` maps each IOSurface into display SMMU CB4 and switches VIG0 through CTL0. `NativeDpuInterrupt.cpp`
   takes the MDSS INTF1 vblank interrupt (`interruptBase+0x10/0x14/0x18`) and feeds it to the original vsync message.
   SM7325 is the closest sibling with this working. Caveats for ruan: dizi found that Gunyah blocks SMMU GR1 and context-bank
   writes (`native-ufs-smmu-reuse=1`, UniXNU-core `docs/devices/dizi.md`), so plan to reuse the firmware's display stream/bank.
   Aston's paced DPU survey (`UniXNU-aston/package/AstonDisplaySurvey.cpp`) has the SDE catalog offsets and notes that one
   burst of DPU reads hung the phone, so space the reads out.
5. **Upstream core (problem 4):** shader-cache registration agent #304 (`630b85a`) and chunk-aware cache reads #290
   (`aa9ab08`), Metal R2507 (#292–#298), copied-buffer mapping guard (`1b8af52`, `ca0eb9d`, 2026-11-18). Core is at `8542980`
   (2026-11-19); our checkout is at `7d85ede`.
6. **Wi-Fi WCN6750, audio, battery (problem 5).** HakiOS redwood (SM7325) has **WCN6750 Wi-Fi working through Linux's ath11k**
   (joins a network), speaker and mic through the ADSP, and battery level through the ADSP: `hakios-xnu-kernel/drivers/wifi/hakios.wifi.c`,
   `wifi/lx/src/{lx_bus.c,lx_80211.c}`, `drivers/audio/hakios.adsp.c`, `drivers/storage/hakios.ufs*.c`. The UniXNU-native battery via
   ADSP battmgr/GLINK is in `UniXNU-sheng/package/{ShengBattmgr.c,QcomGlink.c,QcomQmp.c,ShengBattery.cpp}` (property offsets in
   porting.toml l.328).

## Problem 1 notes (selector 6 / selector 72)

- **Never calling selector 72 is normal on GOP ports.** GopFramebuffer's own comment (`drivers/GopFramebuffer/NativeLegacyDisplay.cpp`
  ~l.570, "int-209: no vsync client ever registered, frames came every 16.7 ms") and opi5plus (`vsync sent=0 arms=0` on every line,
  54 swaps still completed; UniXNU-core `docs/devices/opi5plus.md`) show that the render server falls back to its own 1/60 s timer.
  So selector 72 is not what stalls us.
- **e00002d5 on selector 6 is the swap wait returning NotReady.** In sheng's check `scripts/check-renderbox-presentation.py` l.69, the
  original wait returns `(0xe00002d5, -1)` when retirement is off and `(0,0)` only after a real retirement. So our 230/562 failures are
  swaps that had not retired when backboardd waited: retirement comes too late. The likely cause is the CPU copy time (amethyst measured
  CPU-copy presenting at ~170 ms/frame against ~10 ms with fb-gpu; `docs/devices/amethyst-bluedmc-amethyst.md`), here on one A55 and at
  2.8K. Items 1, 2 and 4 above address this. `native_legacy_copy_timing_observed` already records the copy cost per frame; read it first.
- `getRefreshAttributes` / `vsync-fixed` (GopFramebuffer ~l.575) only affect rates above 60 Hz. gtaxllte uses `sheng-vsync-fixed=1` at
  60 (`UniXNU-gtaxllte/board.toml` l.168), which is a no-op there.

## Per repository

| Repo | Device / SoC | Last push | Works | Relevant to us |
|---|---|---|---|---|
| [JosephMontfort/hakios-sm7435](https://github.com/JosephMontfort/hakios-sm7435) | POCO X6 5G garnet, **SM7435**, A710 (HakiOS, xnu-8796, iOS 16.5) | 2026-11-14 | display (GOP + beam sync), touch, buttons, **8 cores**, GLES via Freedreno, UFS r/w `/private/var`, Metal WIP | `bootargs.md`, `metal.args` (`bootcpu=7 cpus=8 fb_hz=120 fb_threads=6 adreno_speed=6 ddr=2 fb_queue=1`), `soc.args` (rsc 0x17a20000, tlmm 0x0f100000, spmi 0x0c400000, cmd_db 0x80860000, smem 0x80900000, imem 0x14600000), garnetPkg UEFI under `uefi/files/Platforms/Xiaomi/garnetPkg` |
| [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | common HakiOS kernel; reference redwood SM7325 | 2026-11-06 | see redwood table: Wi-Fi ath11k WCN6750, ADSP audio, camera, cellular partial | `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (beam/INTF, fb_threads, vsync = timer `kFBNotifyVSync 5`), `drivers/wifi/hakios.wifi.c`, `wifi/lx/`, `drivers/audio/hakios.adsp.c`, `drivers/storage/hakios.ufs*.c`, `docs/boot-args.md`, `docs/devices/redwood.md` |
| [RigoLigoRLC/unixnu-nativemobiledisplay](https://github.com/RigoLigoRLC/unixnu-nativemobiledisplay) | Tab S7 FE SM-T733, SM7325 | 2026-11-04 | CPU and **GPU/DPU scanout**, INTF1 vblank interrupt as vsync | `NativeDpuScanout.cpp`, `NativeDpuSurface.cpp` (CB4 tables, MAIR choice), `NativeDpuInterrupt.cpp`; SoC side in [RigoLigoRLC/unixnu-sm7325-common](https://github.com/RigoLigoRLC/unixnu-sm7325-common) |
| [zenkernelsam/UniXNU_elden](https://github.com/zenkernelsam/UniXNU_elden) | Legion Y700 Gen4, SM8750, Gunyah | 2026-11-19 | UFS idle/wake, USB logs, fastboot recovery | `drivers/EldenCpuActivation/GunyahWatchdog.*`, `GunyahSmc.c`, `WatchdogTrial.inc`, `SmpGuard.inc` (guarded 2-core admission, IPI blocker) |
| [MoyuZJ912/UniXNU-aston](https://github.com/MoyuZJ912/UniXNU-aston) | OnePlus Ace 3, SM8550 | 2026-11-14 | sheng-based; own display driver stage 0 | `package/AstonDisplaySurvey.cpp` (paced DPU/DSI reads, SDE catalog), `package/DisplayTimingProbe.cpp`; Gunyah traps WFI (idle cores stay shallow without PSCI power-down), see core `docs/devices/aston.md` |
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) core | — | 2026-11-19 | — | items under "copy first" #5; `docs/devices/*.md` is a searchable index of every fork's findings (dpu-scanout: alioth, asteroids, gtaxllte; Gunyah: cupid, dizi, elden, asteroids) |
| [UniXNU/GopFramebuffer](https://github.com/UniXNU/GopFramebuffer) | — | 2026-11-11 (`b0d812d`) | — | our copy is already at `b0d812d`; nothing newer |
| [vildangil/UniXNU-Dizi-Ruan](https://github.com/vildangil/UniXNU-Dizi-Ruan) | Xiaomi Pad 7 dizi | 2026-11-19 | lock screen, touch (`af3de3d`), memory-outside | mostly our code synced back (`4b99e19`, `f97cf88`, `f4c4e34`). New for us: `64fbe99` `dualboot/ksu-module/` (one-shot slot-b boot via KernelSU, for UFS persistence without touching slot a; flashing boot_b is the user's call) |
| [shanggeshijidefeng/UniXNU-sheng](https://github.com/shanggeshijidefeng/UniXNU-sheng) | Pad 6S Pro, SM8550 | 2026-11-19 | reference port | `scripts/build-gop-legacy-display.py` l.155–175 (vsync sender, slot 301 refresh words), `scripts/check-renderbox-presentation.py` l.69 (e00002d5 meaning), battery/ADSP/Wi-Fi package files listed above |
| [yeyingjun-codes/unixnu-sheng-fence](https://github.com/yeyingjun-codes/unixnu-sheng-fence) | sheng, A740 fence-fix CI build | 2026-11-19 | CI harness | porting.toml l.228: "board writes retry AHB-fence drops" and GMU keepalive across restore writes; maybe relevant if A710 register writes get dropped |
| [linux0922/UniXNU-erhai](https://github.com/linux0922/UniXNU-erhai) | OnePlus Pad 2 Pro, SM8750 | 2026-11-19 | lock/home screen; all-USB disk (OS/SystemOS served from the PC, ~4.8 GiB, no errors); NT36536 touch firmware loaded to SRAM | USB-served root as an alternative to UFS persistence; sources "still being uploaded" |
| [donyanchen/UniXNU-piano](https://github.com/donyanchen/UniXNU-piano) | Xiaomi Pad 8 Pro, SM8750 | 2026-11-19 | not booted yet | nothing yet |
| [kmu1757/UniXNU-t970](https://github.com/kmu1757/UniXNU-t970) (README says UniXNU-pong) | Nothing Phone 2 pong, SM8475, iPadOS 18.7.10 | 2026-11-19 | desktop, Turnip/Metal, native UFS/APFS root with 128 GB Data | `patches/xnu-ipados18/*pong-fb-pacing.patch` (vsync locked to INTF1 frame counter), `*pong-fb-120hz.patch`, `*pong-fb-ar-idle.patch` (autorefresh), `migration/ipados18/services*.md` |
| [OnyxAxisOwO/UniXNU-VMware](https://github.com/OnyxAxisOwO/UniXNU-VMware) | VMware Fusion ARM64 | 2026-11-19 | iPadOS 17.7.11 home + Dock blur via host Metal | `src/kernel/FusionLegacyDisplay.cpp` l.105–150: minimal vsync timer that arms only when a client queued at +5592 (`vsyncWanted`), calls the original sender; `docs/graphics.md` (backboardd's direct Metal access is restricted by policy) |
| [ck66/unixnu-droidvm-build](https://github.com/ck66/unixnu-droidvm-build) | DroidVM (crosvm on Gunyah pVM) | 2026-11-19 | CI builds of payload and a virtio-mmio crosvm | not useful for native ruan; it notes that the DroidVM APK's crosvm has only virtio-pci |
| [ahmedraafat-official/UniXNU-gtaxllte](https://github.com/ahmedraafat-official/UniXNU-gtaxllte) | Tab A 10.1 2016 | 2026-11-19 | GOP CPU display | `board.toml` l.149–170: same legacy args as ours plus `sheng-vsync-fixed=1` |
| [Scarpy19/UniXNU-garnet](https://github.com/Scarpy19/UniXNU-garnet) | garnet, SM7435 | 2026-11-11 | nothing compiled (`board.example=true`) | `docs/uefi-integration.md`, `patches/uefi/0001-garnet-unixnu-handoff.patch` (points to hakios-sm7435's UEFI) |
| [F-e-n-y-x/UniXNU-Redmi-Pad-Pro-5G](https://github.com/F-e-n-y-x/UniXNU-Redmi-Pad-Pro-5G) | ruan | 2026-11-17 | our own public mirror (older) | — |

Also seen, not cloned (no SM7435, Gunyah or display-pacing angle): xl8757/myron-ios26 (SM8850), Kerfand/UniXNU-Fuxi-Arm64e-iOS17,
zhangp996/UniXNU-instantnoodlep-ios167, zhouzhoublm/UniXNU-unicorn, keyfan/UniXNU-enchilada-bringup, hannana-26/UniXNU-houji-ios27,
amethyst8118/UniXNU-zeus, midopix msm8996, bangbang0822/uefi-dby_w09 (empty), many hakios-* device repos (MT68xx, Exynos, SM8xxx).
No repository other than hakios-sm7435 and UniXNU-garnet targets SM7435.
