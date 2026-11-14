# UniXNU: iPadOS on Redmi Pad Pro 5G

UniXNU port of iPadOS 17.7.11 (21H461) to the Xiaomi Redmi Pad Pro 5G / POCO Pad 5G ("ruan", SM7435), booted RAM-only
with `fastboot boot` (nothing is flashed). Status: boots to the **iPadOS 17 home screen** with working touch, buttons, icons and wallpaper; live USB debug (screenshots, logs, reboot to fastboot). CPU-rendered (no GPU yet): the Adreno 710 bring-up is in progress (power, clocks, GMU clocks and GMU firmware load work).

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

### GPU (Adreno 710) bring-up

| Stage | What it does | Status |
|---|---|---|
| S1 | Power the GPU CX domain and clocks, read the GPU SMMU | ✅ |
| S2a | GMU clocks (200 MHz GMU, 150 MHz hub), GMU registers readable | ✅ |
| S2b-1 | GMU firmware (`gmu_gen70000.bin`) verified and loaded into its TCM | ✅ |
| S2b-2 | SMMU stream-5 page tables, GMU main code copied to DDR, GMU released, wait for "HFI ready" | ⏳ in progress |
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
