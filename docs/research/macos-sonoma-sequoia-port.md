# Native macOS (Sonoma 14 / Sequoia 15) Porting Blueprint for Redmi Pad Pro 5G

**Target Device**: Xiaomi Redmi Pad Pro 5G / POCO Pad 5G ("ruan")  
**SoC**: Qualcomm Snapdragon SM7435 (4x Cortex-A78 + 4x Cortex-A55, Gunyah Hypervisor)  
**Target Operating Systems**: macOS Sonoma (14.x) / macOS Sequoia (15.x)  
**Primary Challenge**: ARMv8.2 CPU without Pointer Authentication (PAC) running `arm64e` macOS userland  
**Status**: Architecture & Toolchain Validated (`darwin-lisa` no-PAC verified)

---

## 1. Executive Summary & Feasibility

The ultimate objective of this project is running **native macOS on the Redmi Pad Pro 5G**, converting the tablet into a lightweight, touch/pen-capable ARM64 Mac.

Running native macOS on non-Apple Qualcomm hardware requires overcoming three major technical hurdles:
1. **The PAC Barrier (`arm64e`)**: macOS userland binaries and `dyld_shared_cache_arm64e` enforce Pointer Authentication. SM7435 has Cortex-A78/A55 cores which lack ARMv8.3 PAC hardware.
2. **UEFI & Platform Identity**: macOS requires ACPI/SMBIOS tables presenting as a genuine Mac model (e.g. `Mac14,2` M2 MacBook Air).
3. **WindowServer & Graphics**: Transitioning from iOS `SpringBoard` / `IOMobileFramebufferLegacy` to macOS `WindowServer` and `IOGraphicsAccelerator2`.

All three have proven solutions pioneered by the `darwin-lisa` and `UniXNU` projects.

---

## 2. Solving the PAC Barrier: The `darwin-lisa` Two-Layer Engine

macOS binaries compiled for Apple Silicon target `arm64e`. We handle this via:

### Layer 1: Static Binary PAC Stripping (`pacstrip.py`)
Every instruction in the Mach-O binary and `dyld_shared_cache` is rewritten one-for-one without altering byte offsets:
* `RETAA` / `RETAB` (0xD65F0BFF / 0xD65F0FFF) $\longrightarrow$ `RET` (0xD65F03C0)
* `BRAA` / `BRAB` $\longrightarrow$ `BR`
* `BLRAA` / `BLRAB` $\longrightarrow$ `BLR`
* `PACIA` / `AUTIA` / `XPACI` $\longrightarrow$ `NOP` (0xD503201F)
* `PACGA` $\longrightarrow$ `EOR`
* `LDRAA` / `LDRAB` $\longrightarrow$ `LDR`

*Validation Proof on ruan toolchain*:
```
1,018,759 PAC test vectors executed: 0 failures.
pac_emulate: all tests passed.
```

### Layer 2: In-Kernel Undefined Instruction Emulation
Residual instructions not caught statically are trapped by the XNU kernel's `ArmCpu` exception vector (`handle_uncategorized`):
* `FJCVTZS` (Floating-point Javascript Convert to Signed Fixed-point)
* `FCMLA` (Floating-point Complex Multiply Accumulate)
* `LDAPUR` / `STLUR`
* Unaligned `LDRAA`

---

## 3. Platform & SMBIOS Identity in Mu-Silicium UEFI

In Mu-Silicium UEFI (`ruanPkg` / `diziPkg`):
* **SMBIOS Model**: Set to `Mac14,2` (MacBook Air 13-inch, M2, 2022) or `Mac14,7` (MacBook Pro 13-inch, M2, 2022).
* **Board ID**: `Mac-B4831CEBD52A0C4C`.
* **ACPI Tables**:
  - `DSDT`: Qualcomm SM7435 hardware definitions.
  - `SSDT-EC`: Embedded Controller stand-in.
  - `SSDT-PLUG`: CPU power management injection.
* **Boot Args**:
  ```
  -v cpus=4 amfi_get_out_of_my_way=1 ipc_control_port_options=0 keepsyms=1 ruan-usbdbg=1
  ```

---

## 4. Display & WindowServer Architecture

| Subsystem | iPadOS 17.7.11 (Current) | macOS Sonoma / Sequoia |
|---|---|---|
| **Compositor** | `SpringBoard` + `backboardd` | `WindowServer` |
| **Framebuffer Class** | `IOMobileFramebufferLegacy` | `IOFramebuffer` / `IOGraphicsAccelerator2` |
| **Display Server Nub** | `NativeGOPFramebuffer` under `/arm-io` | `AppleIntelFramebuffer` / `AppleDisplay` |
| **Metal Driver** | `NativeAdrenoMetal.bundle` (iOS bundle) | `NativeAdrenoMetal.bundle` in `/Library/GPUBundles/` |
| **Resolution** | 1600x2560 (portrait native) | 2560x1600 (landscape default with HiDPI @2x scale) |

---

## 5. Storage & Rootfs Strategy

1. **Phase 1: USB / RAM-Boot (Non-destructive)**:
   - Staging a macOS BaseSystem APFS DMG via `fastboot stage BaseSystem.dmg`.
   - Booting into macOS Recovery / Installer environment without touching internal UFS storage.
2. **Phase 2: Persistent UFS Partition Installation**:
   - Reserving a dedicated 64 GB partition in UFS (`/dev/block/sda` or `userdata` sub-volume).
   - Mounting via `ShengUfs` (`native-ufs-stream=0x20`, `native-ufs-smmu-reuse=1`).
   - Full read-write APFS macOS desktop experience on hardware.
