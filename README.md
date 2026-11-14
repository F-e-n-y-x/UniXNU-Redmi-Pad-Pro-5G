# UniXNU: iPadOS on Redmi Pad Pro 5G

UniXNU port of iPadOS 17.7.11 (21H461) to the Xiaomi Redmi Pad Pro 5G / POCO Pad 5G ("ruan", SM7435), booted RAM-only
with `fastboot boot` (nothing is flashed). Status: boots to the **iPadOS 17 home screen** with working touch, buttons, icons and wallpaper; live USB debug (screenshots, logs, reboot to fastboot). CPU-rendered (no GPU yet): the Adreno 710 bring-up is in progress (power, clocks, GMU firmware load and GMU SMMU context work; the GMU starts and needs a privileged mapping fix).

<p>
  <img src="docs/img/usb-lockscreen-full.png" alt="Lock screen" width="32%">
  <img src="docs/img/usb-homescreen-full.png" alt="Home screen" width="32%">
  <img src="docs/img/usb-settings-about-full.png" alt="Settings > About: iPadOS 17.7.11" width="32%">
</p>

<sub>Screenshots taken from the tablet's framebuffer over the live USB debug link. The rows at the bottom are the port's
live diagnostic banner (boot time, frame copies, milestones, touch, buttons, USB and GPU state), drawn by the kernel over the iPadOS image.</sub>

**Photos of the tablet:**

<p>
  <img src="docs/img/lockscreen-rb13.jpg" alt="First lock screen on ruan (photo)" width="45%">
  <img src="docs/img/settings-about-rb14f.jpg" alt="Settings > About on ruan (photo)" width="45%">
</p>

## Progress

| Area | Status |
|---|---|
| Boot chain: stock ABL `fastboot boot` → Mu-Silicium UEFI → DarwinBoot → XNU | ✅ |
| RAM root over USB (about 2.6 GB, about 140 s), dyld shared cache, launchd | ✅ |
| Writable `/private/var` (UniXNU RamBlock Data volume) | ✅ |
| Daemons, backboardd, SpringBoard | ✅ |
| Display (GOP framebuffer, write-combined copies, 13–18 ms per frame) | ✅ |
| Lock screen, home screen, wallpaper, app icons | ✅ |
| Touch (Novatek NT36532) | ✅ |
| Buttons (power, volume) | ✅ |
| All 8 CPU cores at full clock (cpufreq 1958 / 2400 MHz) | ✅ |
| Live USB debug: screenshots, kernel log, file pull, reboot to fastboot | ✅ |
| Boot time to lock screen | ⚠️ about 7–9 min (about 2 min of it is the USB upload) |
| Smoothness | ⚠️ CPU rendering only (no GPU yet) |
| Settings → Accessibility | ❌ crashes: CoreImage's GL path needs a GPU |
| Wi-Fi, audio, sensors (auto-rotate), camera | ❌ not started |

### Hardware status

| Component | Hardware | Status | Notes |
|---|---|---|---|
| CPU | Snapdragon 7s Gen 2 (SM7435): 4× Cortex-A78 + 4× Cortex-A55 | ✅ all 8 cores | SMP (`smp=1`); `ruan-cpufreq` sets the big cluster to 2400 MHz and the little cluster to 1958 MHz. No PAC (ARMv8.2). |
| RAM | 8 GB LPDDR4X | ✅ about 8.1 GB usable | 2.75 GiB DarwinBoot window plus 702720 adopted outside pages (`memory-outside=1`); no swap yet |
| GPU | Adreno 710 (gen7-3-0) | 🔧 bring-up | Powered, clocked, GMU firmware loaded, GMU released from reset (S1–S2b-2); not yet usable by iPadOS |
| Display | 12.1" 2560×1600 LCD, GOP framebuffer | ✅ | CPU-composited frames, write-combined copy 13–18 ms per frame; no brightness control yet |
| Touch | Novatek NT36532 (SPI) | ✅ | HakiOS driver + UniXNU NativeHidInput |
| Buttons | Power, volume up/down (PMK8350 PON, PMIC GPIO) | ✅ | |
| Storage | UFS | ➖ unused | RAM root + RAM Data volume (RamBlock); nothing on the tablet's storage is touched |
| USB | DWC3 (high speed) | ✅ debug only | CDC-ACM link: screenshots, log, files, `gpu` commands, reboot to fastboot |
| Battery / charging | PMIC fuel gauge | ❌ | Battery icon shown without a real level |
| Wi-Fi / Bluetooth | Qualcomm (integrated) | ❌ | not started |
| Audio | Qualcomm ADSP + codec | ❌ | audiomxd crashes without hardware |
| Cellular | Qualcomm modem | ❌ | not started |
| Sensors (auto-rotate) | Qualcomm sensor hub (SSC/ADSP) | ❌ | not started |
| Cameras | | ❌ | not started |
| RTC / clock | PMIC RTC | ❌ | clock always starts at the build's time (Fri Nov 13, 7:00) |

### GPU (Adreno 710) bring-up

| Stage | What it does | Status |
|---|---|---|
| S1 | Power the GPU CX domain and clocks, read the GPU SMMU | ✅ |
| S2a | GMU clocks (200 MHz GMU, 150 MHz hub), GMU registers readable | ✅ |
| S2b-1 | GMU firmware (`gmu_gen70000.bin`) verified and loaded into its TCM | ✅ |
| S2b-2 | SMMU stream-5 page tables, GMU main code copied to DDR, GMU released, wait for "HFI ready" | 🔧 GMU context accepted and GMU released from reset; first instruction fetch hits an SMMU permission fault (fix: privileged-only mappings) |
| S3 | GPU microcode (SQE), zap shader via SCM, first command ring and fence | next |
| S4 | Publish the GPU to iPadOS (IOGPU / AGX-style provider) | later |
| S5 | Metal through Turnip (FD710), GPU-composited UI | later |

Each stage runs on the tablet as a USB debug command (`gpu <n>`), after boot. Every register access is streamed to the host
before it happens, so a hang or reset names its exact step.

## Documents

- [docs/IPADOS-BRINGUP.md](docs/IPADOS-BRINGUP.md): every blocker, error text, cause and fix, in order. Start here.
- [unixnu-ruan/](unixnu-ruan/): the board package (board.toml, XNU patches, kernel-side packages, build scripts, DTS).
- [docs/research/](docs/research/): GPU (offline and Adreno 710), display and method plans.

Built on [UniXNU](https://github.com/UniXNU) and its device ports (amethyst, fuxi, Dizi-Ruan, gts7xlwifi, j716f, nabu),
whose notes this work leans on. No Apple binaries, IPSW contents or vendor firmware are included: bring your own IPSW.
