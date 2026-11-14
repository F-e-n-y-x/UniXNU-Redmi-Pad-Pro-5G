# Real Adreno GPU on ruan (SM7435, Adreno 710): feasibility and staged plan

Status 2026-11-14: research only. Nothing built or booted. The lock screen does not need a GPU, so this is the
speed and app-compatibility track. Paths: `dts` = `macOS/hardware/private/ruan-live.dts`, `AM` =
`refs/UniXNU-amethyst/scripts/log/WORKING-BOOT.md`, `fdd` = Mesa `src/freedreno/common/freedreno_devices.py` at
UniXNU's pinned commit c823a83e (`UniXNU/scripts/data/gpu-metal-vulkan-inputs.json`).

## Verdict

**Feasible, medium risk. About 5-8 weeks of bring-up, in roughly 15-25 RAM-only test boots.**

The strongest evidence is amethyst: SM7635, Adreno 810, also a 7-series SoC under Gunyah. It went from GDSC reads to
a smooth Metal UI on the GPU, using UniXNU's NativeAdreno and AdrenoMetal and the same staged probe that we would
reuse. Turnip already supports the A710. The work left is board data: the A710 hw-init tables, the parrot clock
offsets, and the GMU votes. No new architecture is needed.

## 1. How far each reference got with a real GPU

| Ref (SoC, GPU) | Power/GDSC | GMU boot + HFI | SQE/ring/CP | Zap | Real draw | Metal (Turnip) | CA on GPU |
|---|---|---|---|---|---|---|---|
| amethyst (SM7635, A810 gen8) | yes, AM:297-300 | yes, AM:347-349 (V41) | yes, AM:363-366 (V42) | yes, SCM PAS 13, AM:360-366 | bar drawn, AM:366-368 | V47: SpringBoard and Preferences open GPU devices, 53 submits, AM:387-392 | V50 picture plus GPU blur (AM:411), V51 smooth GPU hand-off (AM:418), V52 (AM:429) |
| sheng (SM8550, A740) | yes | yes | yes | yes | yes | yes | `UniXNU/components.toml:80-120`: Adreno740 and AdrenoMetal both `verified_devices = ["sheng"]` |
| nabu (SM8150, A640 a6xx) | yes | yes | yes | yes | yes | yes, through metal2vulkan and Turnip (README.md:15-16) | interactive SpringBoard on GPU compositing (README.md:17); 181 scanout frames (porting.toml:70); a stall after the first frames is still open (README.md:20-22) |
| j716f (SM8250, A650) | yes | yes | yes | n/a | compute-shader copy and brightness (porting.toml:151) | partial | GPU scanout path (porting.toml:97-107) |
| gts7xlwifi (SM8250, A650) | yes | yes | yes | n/a | yes | yes | "iPadOS desktop ... GPU" (board.toml description) |
| Dizi-Ruan (our SM7435) | **none**. Its Adreno*/ShengGmu* files are sheng copies, and `board.toml` includes no GPU component | | | | | | |

amethyst V45 confirms that a published accelerator without the GPU add-on leaves the UI on CPU rendering
(AM:378-381). V47 confirms that the add-on is what makes SpringBoard use the GPU (AM:387-392).

## 2. Gap from UniXNU Adreno740 (A740, SM8550) to our A710 (gen7_3_0, SM7435)

Ours: `qcom,adreno-gpu-gen7-3-0`, `Adreno710v1` (dts:16133-16144), chip id 0x07010000 (fdd:975).

| Item | Ruan value (evidence) | Reuse from A740 or amethyst | Gap |
|---|---|---|---|
| GPU register block | 0x3d00000+0x40000, cx_dbgc 0x3d61000, rscc 0x3d50000, cx_misc 0x3d9e000 (dts:16136) | same a7xx layout as A730/A740 | none expected |
| IRQ | SPI 0x12c kgsl, 0x11e freq limiter (dts:16138) | NativeAdrenoIrq | board constant |
| GMU | gen7 GMU at 0x3d68000+0x37000, pdc 0xb270000, ao 0x3d40000; IRQs hfi 0x130, gmu 0x131; SID 5 (dts:16873-16889) | gen7 HFI is the same family as Adreno740 (A740 uses gmu_gen70200, we use gmu_gen70000) | GMU fw version, HFI table sizes |
| Clocks and GDSCs | gpucc "qcom,parrot-gpucc" 0x3d90000+0xa000 (dts:6663); CX GDSC 0x3d99108, GX GDSC 0x3d9905c (dts:12205-12252) | amethyst `GpuPowerBoard.h:39-74` has the same structure; CX/GX offsets differ (amethyst 0x9080 vs ours 0x9108) | **the parrot GCC/GPUCC CBCR, RCG and PLL offsets** (downstream gcc-parrot.c / gpucc-parrot.c in the SM7435 vendor kernel; not in mainline) |
| SMMU | kgsl-smmu 0x3da0000 qsmmu-v500, 6 CBs (dts:16891-16905); gfx3d_user SID 0, secure SID 2, GMU SID 5 (dts:16852-16871) | ArmSmmu4K plus NativeAdrenoSmmuContext; amethyst used the same SID-5 GMU bank (AM:309-312) | board constants only |
| Zap shader | `zap-shader { memory-region = gpu_microcode_region@0x89e1a000+0x2000 }` (dts:164-167, 16159-16161) | amethyst loaded its own OEM zap from XNU at EL1 under Gunyah through **SCM PAS 13** (init_image/mem_setup/auth_and_reset status 0, AM:360-366; `porting.toml:363,375`; `AmethystSmc.c`) | Use our OEM-signed `a710_zap.mdt` + `.bNN` (or `.mbn`). The Android TZ/PIL path is not required: XNU makes the same SCM calls. Unverified on ruan, so it is test step S3. |
| Power-level and bus votes | DT pwrlevel bins (dts:16163-...), cmd-db 0x80860000 (dts:67), bus tables (dts:16151-16152), speed_bin nvmem | amethyst built them with prepare-*-gmu-votes.py from the live DT (AM:315-327) | generate the A710 vote JSON |
| HW init tables (hwcg, protect, GBIF/UCHE, pwrup reglist) | gen7_3_0 | `NativeAdrenoA740HwInit.h` (169 lines) and `NativeAdrenoPwrupRegs.h` are A740-specific | **write A710 tables** from downstream KGSL `adreno-gpulist.h`/`adreno_gen7*.c` (gen7_3_0). Mainline `a6xx_catalog.c` (master, 2026-11-14) has no 0x07010000 entry; UniXNU's adreno inputs pin `ianchb/sm8550-mainline` (`scripts/data/adreno-inputs.json`) |
| Firmware | in `work/analysis-vendor/stock/vendor_a/firmware/`: `a710_sqe.fw`, `a710_zap.mdt` + `a710_zap.b00..bNN` (+ `.mbn`, `.elf`), `gmu_gen70000.bin` | AdrenoFirmware component | names only; keep them out of git |

## 3. Userspace Metal path

- **Pipeline:** `drivers/AdrenoMetal` is a Metal frontend → metal2vulkan → Turnip (`components.toml:102-120`). Its runtime pieces are `NativeAdrenoMetal.bundle` and `libvulkan_freedreno.dylib`.
- **A710 is already in Turnip:** the pinned Mesa has **FD710** (chip_id 0x07010000, a7xx_gen1; fdd:975-979 and 1074-1091). Its comment names "LineageOS parrot/SM7435". GMEM is 512 KiB, highest_bank_bit is 15, and it has its own `a710_raw_magic_regs`.
- **The Darwin KMD patch needs no A710 special case.** It computes gpu_id from the chip id, and 0x07010000 gives 710 (`UniXNU/patches/mesa/turnip-darwin-kmd.patch:1847-1854`). Only A740's 0x43050a01 needed one.
- **Build host:** `build-turnip-darwin.py` and `build-adreno-metal-plugin.py` call `xcrun --sdk iphoneos` and `xcrun clang` (`build-turnip-darwin.py:48-57`, `build-adreno-metal-plugin.py:39-40`), so they need macOS with Xcode. Our Linux build host can't run them. amethyst built its add-on in GitHub Actions macOS runs (AM:376, 425-428), which is the route to copy.
- **Delivery:** the add-on goes in through `/chosen/memory-map/GpuAddon` (`NativeRamBlock.cpp:691-713` (`native_ram_block_gpu_addon` at 693), already in our kernel) and the kernel's add-on install.

## 4. Milestones

Each stage is a separate RAM-only boot, guarded by a boot-arg, and runs only if the stage before it passed.
amethyst's `GpuPowerProbe.cpp` modes are the template (`refs/UniXNU-amethyst/scripts/log/GpuPowerProbe.cpp:150-1491`).

| Stage | What | amethyst mode | Needs | Size |
|---|---|---|---|---|
| **S0** | read-only GDSC/gpucc survey (**written**, `staged/`) | 1-3 | nothing | 1 boot |
| S1 | GCC GPU branch reads, then CX GDSC on + SMMU vote clock, then a SMMU ID read | 4 | parrot GCC/GPUCC offsets | 2-4 days |
| S2 | GMU SMMU context (SID 5), cmd-db and votes, GMU clocks, **GMU firmware boot + HFI ready = "GMU ping"**, GX on through OOB | 5-11 | gmu_gen70000.bin, vote JSON, NativeAdreno Gmu* | ~1 week |
| S3 | GPU SMMU context, SQE copy, A710 hw_init, CP init + CP_MEM_WRITE, zap via SCM PAS 13, first IB draws a bar | 12-16 | A710 hw-init tables, a710_sqe/zap | 1-2 weeks |
| S4 | fences and IRQ, publish the NativeAdrenoGpu service and user client | 17 | — | ~1 week |
| S5 | Metal accelerator + GPU add-on (Turnip FD710) built on macOS CI; CA on GPU; GPU scanout hand-off | 18-19, V45-V52 | macOS runner | 1-3 weeks |

**S0, staged and not applied:**
- `staged/package/RuanGpuSurvey.c` (syntax-checked with `-Wall -Wextra -Werror` against stub headers).
- `staged/hook.patch`: one call in `boot_progress_start`, plus the build list entry. It applies to the current UniXNU-ruan with fuzz 1.
- **What it does:** with `ruan-gpu-survey=1` it reads 7 registers in the gpucc page (CX/GX GDSCR and the GDSC syscon cells) and prints them as boot-progress events. It writes nothing.
- **Risk:** if GCC's gpu_cfg_ahb branch is off, the first read can raise an SError. The breadcrumb line is printed before the read, so such a panic names this probe.
- **Why the "GMU ping" is not written yet:** it needs the S1 clock offsets and the S2 firmware boot. Writing it now would mean guessing parrot register offsets.

## Risks

- **A710 hw-init tables** must come from downstream KGSL; mainline has no gen7_3_0 entry. Wrong values show up as a hang at hw_init or at CP init.
- **The zap shader under Gunyah on ruan is unproven** (amethyst's success is a strong prior, not proof). If PAS 13 is refused, the CP stays in secure mode; amethyst ran its first CP_MEM_WRITE while still secure (`GpuPowerProbe.cpp:596`).
- **The macOS build host is required for S5.**
- **cpus/SMP and RAM pressure:** the GPU needs pool memory, which amethyst set aside as a 32 MiB GMU pool (`GpuPowerBoard.h:97-98`). Ours must be placed inside the adopted memory and recorded in the board memory map.
