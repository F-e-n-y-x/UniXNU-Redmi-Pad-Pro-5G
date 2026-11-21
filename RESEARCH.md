# Native macOS on Redmi Pad Pro 5G (ruan): feasibility research

Date: 2026-11-13. Scope: macOS booted natively, flashed like a custom ROM. VMs and emulators are out of scope.
Revised after the user linked `vildangil/UniXNU-Dizi-Ruan` and `iOSNJBrepo/macOS-uke`. The first draft said "impossible because EL2 is required". That was wrong: UniXNU runs XNU at EL1. PAC is the real blocker.

## Device facts (checked on the tablet, read-only)

| Item | Value |
|---|---|
| SoC | Snapdragon 7s Gen 2, SM7435, platform `parrot` |
| CPU | 4x Cortex-A78 (0xd41) + 4x Cortex-A55 (0xd05), ARMv8.2 |
| PAC (pointer authentication) | **None.** `/proc/cpuinfo` features have no `paca`/`pacg`. The garnet record (same SoC) reports the same. |
| GPU | Adreno 710 |
| RAM | 7.7 GB visible |
| Secure boot | fused (`ro.boot.secureboot=1`), bootloader unlocked |
| EL2 | owned by Qualcomm Gunyah, `/dev/kvm` absent. UniXNU does not need EL2. |

## Verdict

| OS | Natively on ruan? | Why |
|---|---|---|
| **macOS** | **No, not as is.** A software workaround is in progress elsewhere (macOS-Pad-6 on SM8250, not booting yet): see [WORKAROUND.md](WORKAROUND.md) and [SCENE.md](SCENE.md) | UniXNU's macOS profile is arm64e only. It needs a CPU with PAC, and SM7435 has none. All macOS system binaries are arm64e. |
| **iPadOS 17 / iOS 16–18** (arm64) | **Possible in principle; early research now** | These are the last arm64 (non-PAC) releases, and UniXNU supports them. The dizi port uses the `ipados-17.7.11-21H461` profile and so far reaches early XNU kernel entry, with no OS yet. |

The "macOS on Mi Pad 7" video is real. It works on the Pad 7 because the SM7675's Cortex-A720 cores are ARMv9 and have PAC. That doesn't carry over to SM7435.

## How native XNU on Qualcomm works: UniXNU

https://github.com/UniXNU/UniXNU (2.8k stars, very active). Docs: https://unixnu.com

- "Run iOS and macOS on non-Apple ARM64 hardware through UEFI. Like OpenCore…"
- Boot chain:
  1. A Mu-Silicium UEFI fork goes into `boot_a` as an Android boot image.
  2. The DarwinBoot EFI loader runs from that UEFI.
  3. DarwinBoot starts XNU at **EL1**: either a source-built kernel or Apple's kernelcache, patched for 4K pages, a GICv3 driver (ArmCpu) and a device-tree platform expert.
  4. The IPSW system volumes are written into windows of `userdata`.
- Drivers are UniXNU components:
  - storage: ShengUfs/NativeUfs
  - GPU: Adreno740 + AdrenoMetal (metal2vulkan + Turnip)
  - touch: Novatek
  - Wi-Fi: Ath12k
  - modem: QcomBaseband
- arm64 vs arm64e (https://github.com/UniXNU/UniXNU/blob/main/docs/porting/arm64e.md):
  - "arm64e needs native PAC hardware… A device without it stays on arm64: iOS and iPadOS 18 are the last releases that support arm64."
  - The macOS profile (`macos-27.3-26C5035r`) "is an arm64e profile, so the device must pass the arm64e requirements" (docs/porting/macos.md).
- Each image is built for one device's chip-ID seed (`UNIXNU_UNIQUE_CHIP_ID`), and Apple software has to be supplied by the user from IPSWs.

### Where it works

| Port | SoC | PAC | Result |
|---|---|---|---|
| UniXNU-sheng (Xiaomi Pad 6S Pro) | SM8550 | yes | iPadOS 27 with GPU; macOS 27.3 in development |
| CrossDarwin/macOS-ForSheng | SM8550 | yes | macOS 26.5 bring-up |
| iOSNJBrepo/macOS-uke (Xiaomi Pad 7) | SM7675 | yes | macOS 26.5 to the Finder desktop, with Adreno 732 Metal, touch, Dock and the menu bar. Broken: battery reading, rotation, performance (fence timeouts). Needs a Mac with Xcode to build. |
| chiba233/Trogdor-ipadOS (Chromebooks) | SC7180 | **no** | iPadOS 17, including LTE data. Shows a non-PAC Qualcomm chip can reach a usable iPadOS. |
| elden (Legion Y700 Gen 4) | SM8750 | yes | macOS Recovery reaches its first frame |
| whyred (Redmi Note 5) | SDM636 | no | iOS 16 profile, bring-up |

The device index lists about 300 device pages at https://github.com/UniXNU/UniXNU/tree/main/docs/devices

### SM7435 ports

| Repo | Device | State (2026-11-13) |
|---|---|---|
| https://github.com/vildangil/UniXNU-Dizi-Ruan | dizi (Redmi Pad Pro Wi-Fi). ruan is "unverified" | Profile `ipados-17.7.11-21H461` (arm64). Hardware test on 2026-10-28: UEFI → ExitBootServices → first XNU instructions → `common_start` → `_set_mmu_ttb` returns. Only 1 CPU, 3 GiB RAM in the ADT. MMU enable is still under test (A/B builds). No storage, display, touch or GPU drivers yet: `dizi_defconfig` has UFS, display and Adreno off. Releases are "temporary fastboot boot ONLY; NEVER fastboot flash". README and porting.toml are still sheng's. The UniXNU index page shows a validation error. |
| https://github.com/Scarpy19/UniXNU-garnet | garnet (Redmi Note 13 Pro 5G) | All areas `planned`; "not bootable". Notes the missing PAC. |

Blocked at **SoC**: there is no `UniXNU-sm7435-common`, and the Adreno 710 has no driver (Adreno740 targets the A740 and is reused for the A732 on uke).

## Older background (still accurate)

- Snapintosh (https://github.com/Snapintosh/Snapintosh) is a different approach. It is an EL2 monitor that runs the stock macOS 27 kernelcache on the ASUS Zenbook A16 (X2 Elite) only, with a Mac host attached. EL2 is unavailable on ruan, so it isn't the route here.
- Earlier XNU-on-non-Apple projects (xnu-qemu-arm64, xnu-bcm2837-hackintosh, darwin-on-arm) never got past the kernel.
- macOS licence: Apple-branded hardware only (https://www.apple.com/legal/sla/).

## If the user wants to pursue iPadOS 17 natively

Not macOS, but the closest native Apple OS this chip can run. Everything below is the dizi work plus months of driver work:

1. Join the dizi effort (vildangil, UniXNU Telegram @unixnu) rather than start over. ruan and dizi share the SoC. They differ in modem, possibly panel/touch, and RAM layout. Check that from the `ruan` vs `dizi` DTBs in `refs/`.
2. Build Mu-Silicium for ruan (NetraniPkg, garnetPkg/dizi as the template). `fastboot boot` writes nothing to the tablet, but it does reboot it, so arrange it with the ROM session first.
3. Get past MMU enable (the dizi frontier), then do UFS (NativeUfs), display (GopFramebuffer), touch, and the Adreno 710 later or never (Trogdor runs on CPU rendering).
4. Needs a Mac with Xcode for the builds (or the repo's GitHub macOS runner workflow), plus the iPadOS 17.7.11 IPSW.

Writing the system to `userdata` wipes Android data. The dizi releases say temporary boot only and never flash.
