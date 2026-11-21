# Direct DPU scanout on ruan (SM7435): porting dizi's `dpu-scanout=2`

2026-11-21. Research only: nothing was built or run. **[V]** means read from a file, **[I]** means inferred.

## 1. SM8550 assumptions in the dizi code

ShengUfs (`/root/unixnu-work/ufs-variant/ShengUfs-src/NativeUfs.cpp`, 2f6c7b0 + ShengUfs-dizi.patch as checked out):
- :329 `kDpuWindow 0x10000000`, 4 slots of 32 MiB: the IOVA window for surfaces. Not tied to the SoC; it can stay.
- :1417-1418, :1437: display streams `0x1c00 + 2*k`, from `iommus = <&apps_smmu 0x1c00 0x2>`. **SM8550 only.**
- :1424-1426: groups capped at 128. SMMU map is `0xd0000` (:420/:914) and the bank page is `(pages+bank)*4096`. [I] This is fine for ruan: 0x78 SMRs, 0x4e banks, (128+78)*4K = 0xce000.
- :1455-1456: needs S2CR type 0, SCTLR.M=0 and CBAR type 1. It never checks CBA2R.VA64, but the TCR/TTBR layout it writes assumes AArch64.
- :349-413 `dpuEnable`: writes only context-bank registers (TTBR0, TCR2 `5|7<<15`, TCR `T0SZ 32`, MAIR0 0x44, SCTLR `M|CFRE|CFIE`). It keeps identity 1 GiB blocks for 0x80000000-0xffffffff so the boot scanout survives. There are no GR1 writes, which suits Gunyah.
- :1723 `native_ufs_dpu_map`: L3 NC leaves, then TLBIALL and sync.

NativeLegacyDisplay (`…/ufs-variant/out/ruan/…/GopFramebuffer/NativeLegacyDisplay.cpp`; the ruan build already compiles it):
- :284-286, :305: `kVig {0x4000,0x6000}`, relative to an MDP map at **0xae01000** (:309, size **0x8f000**). That is two VIG pipes with source split, DPU 9.2.
- :305 `kCtlFlush 0x15018` (CTL0, 0xae16018), `kBootIova 0xb8000000`, **`kBootStride 0x2fa0`** (dizi's 3048 px).
- :338 flip writes SRC0_ADDR at `+0x14` and YSTRIDE0 at `+0x24`, then :340 writes flush **`0x3`** (VIG0|VIG1). :423 waits on `flush & 0x3`.
- :308 gate `native_ufs_dpu_state>>31`. **Bug [V]:** the failure codes `0xdead000n` also have bit 31 set, so `startDirect` turns on even when the bank was refused. Each swap then fails to map (`direct[2]++`) and falls back to the copy. Harmless, but it wastes time. The gate should be `(state & 0xffff0000) == 0x80000000`.
- :124-130, :697: only 32-bit BGRA/`&BGA` is scanned out; half float is not.

## 2. ruan values (live DT `hardware/private/ruan-live.dts`) [V]

| Item | ruan | Source |
|---|---|---|
| MDSS/MDP | 0xae00000 size 0x84000; VBIF 0xaeb0000; MDP top at +0x1000 (`sde-off`) | :17728, :17718 |
| SSPP | VIG0 **+0x5000**, DMA0 **+0x25000**, DMA1 **+0x27000** (one VIG, two DMA) | `sde-sspp-off/type` |
| CTL | +0x16000/17000/18000/19000; CTL0 is primary | `sde-ctl-off` |
| LM | +0x45000 (primary), 0x47000, 0x48000 | `sde-mixer-off` |
| INTF | INTF1 = DSI at **+0x36000**; INTF0/INTF2 are DP | `sde-intf-off/type` |
| Panel | n83_35_02_0a cphy nt36532 BOE, **1600×2560** video mode, DSC, topology 1 LM/1 DSC/1 INTF, 30-120 Hz | bootargs, :20742 |
| MDSS streams | unsecure `iommus = <apps_smmu 0x800 0x402>` (0x800/0x802/0xc00/0xc02); secure 0x801/0xc01 (VMID 0xa) | :21152, :21161 |
| Handoff | `qcom,handoff-smrs = <0x800 0x402>`: UEFI programmed this SMR | :16932 |
| Splash | `cont_splash_region` 0xb8000000 size 0x2b00000 | :346 |
| UBWC | v4.0, highest bank bit 8 for LP5 and 7 for LP4, swizzle 6 | `sde-ubwc-*` |

In dizi's 0xae01000-based numbering this is VIG0 0x4000, DMA0 0x24000, DMA1 0x26000, CTL0 flush **0x15018** (unchanged), INTF1 0x35000.

**SM7325 (RigoLigoRLC, `/root/unixnu-work/vg/unixnu-nativemobiledisplay/`) has the same layout** [V]: pipes 0x5000/0x25000/0x27000, CTL 0x16000, INTF1 0x36000, firmware stride 6400, 1600×2560. Its firmware used **VIG0** with SRC_FORMAT `0x000237ff`, UNPACK `0x03020001`, `CTL+0xfc` (FETCH_PIPE_ACTIVE) `0x10000`, `CTL+0xf4` (INTF_ACTIVE) `2`, display SMR `0x84020900` in slot 4 with bank 4. [I] ruan is very likely the same with SID 0x800. Bank and slot are unknown and must be probed. The MDP hardware version (0xae00000+0) is also unknown: SM7325 is 0x70020000, and parrot is probably 8.x or 9.x.

How to find the splash pipe: for each pipe, read SRC0_ADDR (+0x14) and look for 0xb8000000 (or the GOP base), plus `CTL0+0xfc`. Fetch-active bit 16 is VIG0, bit 0 DMA0, bit 1 DMA1. The flush bit is then **VIG0 0x1, DMA0 0x800, DMA1 0x1000** (sde_hw_ctl) [I].

## 3. Can the DPU fetch GPU buffers directly?

- **Different SMMUs.** The GPU uses kgsl-smmu at 0x3da0000 (bank 2, SID 0). The DPU uses apps-smmu at 0x15000000 (SID 0x800). A Turnip/GPU IOVA means nothing to the DPU. The DPU needs the IOSurface's **physical pages** mapped into the display bank, which is what `native_ufs_dpu_map` does. [V]
- SSPP address registers are 32-bit, and ruan has no RAM below 4 GiB apart from carve-outs, so the display bank must translate. [V+I]
- **Gunyah [I, strong prior]:** Gunyah owns stage 2. On ruan HLOS was allowed to write stage-1 context-bank registers and CBAR type 1 on kgsl-smmu, but a GR1 write of type 0 raised an SEA (IPADOS-BRINGUP §19). dizi under Gunyah (SM8550) enabled the display bank's stage 1 with CB-only writes. So: reuse the bank UEFI handed off, never write GR0/GR1, and only accept the bank if its CBAR type reads 1. IOSurface pages are HLOS memory, so they are in HLOS's stage 2. The splash carve-out is the risk if stage 2 differs for SID 0x800; the identity blocks keep it fetchable.
- **Format:** BGRA8888 linear, stride 6400 (1600×4, already 256-aligned), the same as the GOP. Turnip `sysmem,noubwc` gives linear, uncompressed surfaces, so keep UEFI's SRC_FORMAT and FETCH_CONFIG and change only the address and stride. **`&BGA` (compressed) and half-float frames must keep the copy path.** One 16 MiB surface fits a 32 MiB slot.
- **Coherency:** the DPU reads non-cacheable. The GPU writes reach DRAM through the existing wc-clean scheme, and the code waits for the fence before flipping (`gpuWrote`). CPU-drawn surfaces would need a `dc cvac`.

## 4. Step plan

**(a) Read-only probe** (new `dpu` command in `package/UsbDebugCdc.cpp` next to `"display"`, using `mapRangeUncached`):
- MDP 0xae00000: `+0x0` HW_VERSION.
- For P in {0x5000, 0x25000, 0x27000}: `+0x00` SRC_SIZE, `+0x08` SRC_XY, `+0x0c` OUT_SIZE, `+0x14` SRC0_ADDR, `+0x24` YSTRIDE0, `+0x30` FORMAT, `+0x34` UNPACK, `+0x38` OP_MODE, `+0x48` FETCH_CONFIG.
- CTL0 0xae16000: `+0x18` FLUSH, `+0xf4`, `+0xfc`.
- LM0 0xae45000 `+0x0/+0x20..`.
- INTF1 0xae36000 `+0xac` FRAME_COUNT, `+0xb0` LINE_COUNT, read twice to measure Hz.
- SMMU 0x15000000: IDR0/1/2. For each SMR `0x800+4i` that matches 0x800/0x802: S2CR `0xc00+4i`, CBAR `0x1000+4b`, CBA2R `0x1800+4b`, and the bank page `(pages+b)*4K` `+0x0/0x10/0x20/0x24/0x30/0x38/0x58`.

Do this before the UFS build's USB link comes up, or log it through the boot-progress zone.

**(b) Single-flip test** (`dpu-scanout=2`, one frame): apply the changes below, enable the bank, map one surface, flip once, watch the camera, then `directToBoot`.

**(c) Per-frame flip:** dizi's swap path already does this (`directSlot`/`flip`/`latchTick`, fence-gated). Replace the 500 µs flush poll with the INTF1 frame counter (as SM7325 does) and drive vsync from it.

Diff sketch (NativeUfs through `patches/drivers/ShengUfs-dizi.patch`; NativeLegacyDisplay through a new `package/gopfb-zzzz-ruan-dpu.patch`):
```diff
-        if (r[0] != 0xffffffffU || ((smr ^ (0x1c00U + 2 * k)) & 0x7fff & ~mask)) continue;
+        if (r[0] != 0xffffffffU || ((smr ^ (0x800U + 2 * k)) & 0x7fff & ~mask)) continue;   // ruan iommus 0x800 0x402
-    if (scanout && found != ... && ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1) {
+    if (scanout && found != ... && ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1 && (native_ufs_dpu_smmu[8] & 1)) {   // CBA2R.VA64
--- NativeLegacyDisplay.cpp
-    static constexpr uint32_t kVig[2]={0x4000,0x6000},kCtlFlush=0x15018,kBootIova=0xb8000000U,kBootStride=0x2fa0;
+    // ruan: one pipe (probe (a) decides: VIG0 0x4000/bit 0x1, DMA0 0x24000/0x800, DMA1 0x26000/0x1000)
+    static constexpr uint32_t kVig[1]={0x4000},kFlushBits=0x1,kCtlFlush=0x15018,kBootIova=0xb8000000U,kBootStride=0x1900;
-        if(mode<2 || ... || !(native_ufs_dpu_state>>31) || !primary)return;
-        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);
+        if(mode<2 || ... || (native_ufs_dpu_state&0xffff0000U)!=0x80000000U || !primary)return;
+        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x83000);   // mdp_phys ends at 0xae84000
-        mdp[kCtlFlush/4]=0x3;
+        mdp[kCtlFlush/4]=kFlushBits;
-        if((mdp[kCtlFlush/4]&0x3) && ns<50000000ull)
+        if((mdp[kCtlFlush/4]&kFlushBits) && ns<50000000ull)
```
`LegacyDisplayProbe.cpp:70-72` (weak stubs) does not change. Boot arg: `dpu-scanout=2` (and `dpu=2` for the SMMU dump only). This needs the UFS build, because the window lives in NativeUfs.

Risks: if dizi's level-0 self-reference trick (:362-366) does not fit ruan's walker, `dpuEnable` sees faults within 50 ms and undoes them on its own. A wrong flush bit only stalls the latch (50 ms fallback). Pipe, bank and HW version are unverified until probe (a) runs.
