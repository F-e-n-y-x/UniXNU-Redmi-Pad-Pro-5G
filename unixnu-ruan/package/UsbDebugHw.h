// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware bring-up and DWC3 gadget state machine, ported from UniXNU-j716f (TB-J716F, SM8250) to ruan (SM7435).
// ruan boots the same Qualcomm UEFI USB stack (Binaries/ruan UsbfnDwc3Dxe, UsbConfigDxe, ClockDxe, HALIOMMUDxe), so the
// j716f evidence that ExitBootServices leaves usb30_prim_gdsc, the HS PHY and the USB clocks on is assumed, and checked.
// Included by UsbDebugCdc.cpp inside its anonymous namespace.

#ifndef J716F_USB_DEBUG_HW_H
#define J716F_USB_DEBUG_HW_H

// 64 KiB physically contiguous Normal-Cacheable DMA layout (explicit ARM64 cache maintenance):
//   0x0000..0x0FFF  Event ring (4 KiB, DWC3 write-only after init)
//   0x1000..0x11FF  EP0 TRBs (up to 32 TRBs, 64B-aligned)
//   0x1200..0x1FFF  EP0 control buffer (64B-aligned)
//   0x2000..0x203F  EP1 OUT (dep2) TRBs (64B-aligned)
//   0x2100..0x28FF  EP1 OUT RX buffer (2 KiB, 64B-aligned)
//   0x3000..0x31FF  EP1 IN  (dep3) TRBs (64B-aligned)
//   0x4000..0xFFFF  EP1 IN  TX buffer (48 KiB, 4096B-aligned)
#define DMA_TOTAL_BYTES   0x10000u
#define DMA_EVT_OFF       0x0000u
#define DMA_EP0_TRB_OFF   0x1000u
#define DMA_EP0_BUF_OFF   0x1200u
#define DMA_EP2_TRB_OFF   0x2000u
#define DMA_EP2_BUF_OFF   0x2100u
#define DMA_EP3_TRB_OFF   0x3000u
#define DMA_EP3_BUF_OFF   0x4000u
#define DMA_EP3_MAX_CHUNK 0x4000u  // 16 KiB per Bulk IN burst

// op: 0 = clean (dc cvac), 1 = clean+inval (dc civac), 2 = inval (dc ivac)
static inline void usbCacheRange(const void *p, size_t len, int op) {
    if (!len) return;
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15u);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + len;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

struct UsbHw {
    OSSharedPtr<IOMemoryMap> coreMap, gccMap, phyMap, smmuMap, dmaMap;
    IOBufferMemoryDescriptor *dmaDesc = nullptr;
    volatile uint32_t *core    = nullptr;
    volatile uint32_t *gcc     = nullptr;
    volatile uint32_t *phy     = nullptr;
    volatile uint32_t *smmu    = nullptr;
    uint8_t *dmaVirt           = nullptr;
    uint64_t dmaPhys           = 0;

    uint32_t evtOff         = 0;
    uint16_t bulkMps        = 512;
    int ep0Stage            = 0;
    bool ep0Three           = false;
    bool ep0In              = false;
    bool ep0Zlp             = false;
    unsigned ep0StatusDep   = 1;
    unsigned ep0Armed       = 0;
    unsigned ep0Trbs        = 0;

    uint8_t configured      = 0;
    uint16_t lineState      = 0;
    bool hostReady          = false;
    bool needReplay         = true;
    bool ep2Armed           = false;
    bool ep3Busy            = false;
    uint32_t ep2RscIdx      = 0;
    uint32_t ep3RscIdx      = 0;
    uint64_t ep3ArmTick     = 0;
    uint32_t usbEvents      = 0;
    uint32_t usbResets      = 0;
    uint32_t usbCfgCount    = 0;
    int lastCmdSt           = 0;
    uint32_t smmuBank       = 0;
    uint32_t smmuOrigSmr    = 0;
    uint32_t smmuOrigS2cr   = 0;
    uint32_t phyInitMode    = 0;
    uint8_t lineCoding[7]   = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};

    void initDefaults() {
        bulkMps        = 512u;
        ep0StatusDep   = 1u;
        needReplay     = true;
        lineCoding[0]  = 0x00;
        lineCoding[1]  = 0xC2;
        lineCoding[2]  = 0x01;
        lineCoding[3]  = 0x00;
        lineCoding[4]  = 0x00;
        lineCoding[5]  = 0x00;
        lineCoding[6]  = 0x08;
    }

    uint32_t rd(uint32_t off) const { return core[off / 4u]; }
    void wr(uint32_t off, uint32_t v) const {
        core[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t grd(uint32_t off) const { return gcc[off / 4u]; }
    void gwr(uint32_t off, uint32_t v) const {
        gcc[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t prd(uint32_t off) const { return phy[off / 4u]; }
    void pwr(uint32_t off, uint32_t v) const {
        phy[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void pmask(uint32_t off, uint32_t mask, uint32_t val) const {
        pwr(off, (prd(off) & ~mask) | (val & mask));
    }
};

static uintptr_t mapRangeUncached(uint64_t phys, uint64_t len, OSSharedPtr<IOMemoryMap> &keep) {
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
    if (!mem) return 0;
    keep = OSSharedPtr<IOMemoryMap>(mem->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    mem->release();
    return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

[[maybe_unused]] static uint32_t smmuIoRd(void *ctx, unsigned off) {
    return static_cast<volatile uint32_t *>(ctx)[off / 4u];
}
[[maybe_unused]] static void smmuIoWr(void *ctx, unsigned off, uint32_t val) {
    static_cast<volatile uint32_t *>(ctx)[off / 4u] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

// shortcut: no RPMh LDO/CX votes on ruan (UEFI's votes are kept); port qcom_rpmh with parrot's RSC base if the HS PHY
// reads back powered down.

[[maybe_unused]] static bool usbConfigureSmmuBypass(UsbHw &hw, const char **outErr = nullptr) {
    if (outErr) *outErr = nullptr;
    if (!hw.smmu) {
        hw.smmu = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_APPS_SMMU_BASE, J716F_APPS_SMMU_SIZE, hw.smmuMap));
    }
    // Under Gunyah hypervisor on SM7435, writing GR1 and context banks aborts with ESR 0x96000010.
    // UEFI HALIOMMUDxe already configured USB stream 0x80 bypass mapping; write nothing.
    hw.smmuOrigSmr  = 0x80000080u;
    hw.smmuOrigS2cr = 0u;
    hw.smmuBank = 0;
    return true;
}

static bool usbEnableGccBranches(const UsbHw &hw) {
    static const uint32_t kBranches[] = {
        GCC_USB30_PRIM_MASTER_CLK,
        GCC_USB30_PRIM_SLEEP_CLK,
        GCC_USB30_PRIM_MOCK_UTMI_CLK,
        GCC_USB3_PRIM_PHY_AUX_CLK,
        GCC_USB3_PRIM_PHY_COM_AUX_CLK,
        GCC_USB3_PRIM_PHY_PIPE_CLK,
        GCC_CFG_NOC_USB3_PRIM_AXI_CLK,
        GCC_AGGRE_USB3_PRIM_AXI_CLK,
    };
    for (unsigned i = 0; i < sizeof(kBranches) / sizeof(kBranches[0]); ++i) {
        hw.gwr(kBranches[i], hw.grd(kBranches[i]) | GCC_BRANCH_ENABLE_BIT);
    }
    bool allOn = true;
    for (unsigned i = 0; i < sizeof(kBranches) / sizeof(kBranches[0]); ++i) {
        if (kBranches[i] == GCC_USB3_PRIM_PHY_PIPE_CLK) continue;  // Externally clocked until UTMI mux
        bool branchOn = false;
        for (unsigned w = 0; w < 2000; ++w) {
            if ((hw.grd(kBranches[i]) & GCC_BRANCH_CLK_OFF) == 0u) {
                branchOn = true;
                break;
            }
            IODelay(2);
        }
        if (!branchOn) allOn = false;
    }
    return allOn;
}

static void usbUpdateRcg(const UsbHw &hw, uint32_t cmdOff, uint32_t cfgOff, uint32_t cfgVal) {
    hw.gwr(cfgOff, cfgVal);
    hw.gwr(cmdOff, hw.grd(cmdOff) | 1u);
    for (unsigned i = 0; i < 1000; ++i) {
        if ((hw.grd(cmdOff) & 1u) == 0u) break;
        IODelay(2);
    }
}

static void usbEnsureGccRcg(const UsbHw &hw) {
    // Vote GPLL0 on in GCC_APCS_GPLL_ENA_VOTE (0x52000 bit 0) and wait for GPLL0_MODE bit 31
    hw.gwr(GCC_APCS_GPLL_ENA_VOTE, hw.grd(GCC_APCS_GPLL_ENA_VOTE) | 1u);
    for (unsigned i = 0; i < 1000; ++i) {
        if (hw.grd(GCC_GPLL0_MODE) & (1u << 31)) break;
        IODelay(2);
    }
    // Only reached when UEFI left the master clock off (gcc-parrot.c rates; see UsbDebugCdc.h).
    usbUpdateRcg(hw, GCC_USB30_PRIM_MASTER_CMD_RCGR, GCC_USB30_PRIM_MASTER_CFG_RCGR, RUAN_USB_MASTER_CFG);
    usbUpdateRcg(hw, GCC_USB30_PRIM_MOCK_UTMI_CMD_RCGR, GCC_USB30_PRIM_MOCK_UTMI_CFG_RCGR, RUAN_USB_MOCK_UTMI_CFG);
    usbUpdateRcg(hw, GCC_USB3_PRIM_PHY_AUX_CMD_RCGR, GCC_USB3_PRIM_PHY_AUX_CFG_RCGR, RUAN_USB_PHY_AUX_CFG);
}

static void usbInitFemtoHsPhy(UsbHw &hw) {
    hw.phyInitMode = 1u;
    // ZUI 15.0.485 msm_snps_hsphy_init (0x9acd14..0x9acd44) & UsbConfigDxe SSUsb1InitHSPhy (0x8388..0x83c8):
    // Pulse ONLY GCC_QUSB2PHY_PRIM_BCR (0x12000) before Femto v2 HS PHY register programming (never touch SS PHY BCRs!)
    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 1u);
    IODelay(150);
    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 0u);
    IODelay(150);

    // Exact ZUI 15.0.485 msm_snps_hsphy_init (0x9acd68..0x9ad04c) + UsbConfigDxe (0x83dc..0x8460):
    hw.pmask(FEMTO_CFG0, 0x03u, 0x02u);                  // Clear RETENTION_EN (bit 0), set OVERRIDE_EN (bit 1)
    hw.pmask(FEMTO_UTMI_CTRL5, 0x02u, 0x02u);            // POR = 1
    hw.pmask(FEMTO_COMMON0, 0x70u, 0x00u);               // FSEL = 0 (19.2 MHz refclk)
    hw.pmask(FEMTO_COMMON1, 0x20u, 0x20u);               // PLLBTUNE = 1
    hw.pmask(FEMTO_REFCLK, 0x03u, 0x02u);                // SELCK_SEP = 2
    hw.pmask(FEMTO_COMMON1, 0x10u, 0x10u);               // VBUSVLDEXTSEL0 = 1
    hw.pmask(FEMTO_CTRL1, 0x01u, 0x01u);                 // VBUSVLDEXT0 = 1
    hw.pmask(FEMTO_RTUNE_SEL, 0x01u, 0x00u);             // RTUNE_SEL = 0 (internal RCAL)
    hw.pwr(FEMTO_PARAM_OVRD_X0, 0x85u);                  // ruan-live.dts hsphy qcom,param-override-seq <0x85 0x6c 0xcb 0x70 0x1e 0x74 0x03 0x78>
    hw.pwr(FEMTO_PARAM_OVRD_X1, 0xCBu);
    hw.pwr(FEMTO_PARAM_OVRD_X2, 0x1Eu);
    hw.pwr(FEMTO_PARAM_OVRD_X3, 0x03u);
    hw.pmask(FEMTO_COMMON2, 0x01u, 0x01u);               // VREGBYPASS = 1
    hw.pmask(FEMTO_UTMI_CTRL5, 0x01u, 0x00u);            // Clear ATERESET
    hw.pmask(FEMTO_CHARGING_DET_OUTPUT, 0x10u, 0x00u);
    hw.pmask(FEMTO_CHARGING_DET_OUTPUT, 0x40u, 0x00u);
    hw.pmask(FEMTO_COMMON0, 0x03u, 0x00u);               // Clear SIDDQ / retention powerdown
    hw.pmask(FEMTO_CHARGING_DET_CTRL, 0xFFu, 0x00u);     // Disable BC1.2 charger detect mux
    hw.pmask(FEMTO_CTRL2, 0x08u, 0x08u);                 // SUSPENDM override = 1
    hw.pmask(FEMTO_CTRL2, 0x04u, 0x04u);                 // UTMI_CLK_EN = 1
    hw.pmask(FEMTO_UTMI_CTRL0, 0x01u, 0x01u);            // SLEEPM = 1
    hw.pmask(FEMTO_UTMI_CTRL5, 0x02u, 0x00u);            // Release POR = 0
    hw.pmask(FEMTO_CTRL2, 0x08u, 0x00u);                 // Release SUSPENDM override
    IODelay(150);
    hw.pmask(FEMTO_CFG0, 0x03u, 0x00u);                  // Release CMN_CTRL_OVERRIDE_EN = 0 & RETENTION_EN = 0
    IODelay(200);
}

static bool usbGccAndPhyInit(UsbHw &hw) {
    if (!hw.gcc) {
        hw.gcc = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_GCC_BASE, J716F_GCC_SIZE, hw.gccMap));
    }
    if (!hw.phy) {
        hw.phy = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_HSPHY_BASE, J716F_HSPHY_SIZE, hw.phyMap));
    }
    if (!hw.gcc || !hw.phy) return false;

    // Preserve live UEFI usb30_prim_gdsc & QMP SS PHY (0x088E9000) state!
    // Never collapse GCC_USB30_PRIM_GDSCR or assert GCC_USB3_PHY_PRIM_BCR / GCC_USB30_PRIM_BCR,
    // because UEFI ExitBootServices (UsbConfigDxe 0x7408 -> StopController 0x5b60 -> UsbfnDwc3Dxe 0x642c)
    // leaves usb30_prim_gdsc, QMP SS PHY (pipe_clk), Femto v2 HS PHY, and usb_ss1_bus powered and clocked.
    if (hw.grd(GCC_USB30_PRIM_GDSCR) & GCC_GDSCR_SW_COLLAPSE) {
        hw.gwr(GCC_USB30_PRIM_GDSCR, hw.grd(GCC_USB30_PRIM_GDSCR) & ~GCC_GDSCR_SW_COLLAPSE);
        for (unsigned i = 0; i < 5000; ++i) {
            if (hw.grd(GCC_USB30_PRIM_GDSCR) & GCC_GDSCR_PWR_ON) break;
            IODelay(5);
        }
    }
    if (hw.grd(GCC_USB3PHY_PHY_PRIM_BCR) & 1u) hw.gwr(GCC_USB3PHY_PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_USB3_PHY_PRIM_BCR) & 1u)    hw.gwr(GCC_USB3_PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_QUSB2PHY_PRIM_BCR) & 1u)    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_USB30_PRIM_BCR) & 1u)       hw.gwr(GCC_USB30_PRIM_BCR, 0u);

    if (hw.grd(GCC_USB30_PRIM_MASTER_CLK) & GCC_BRANCH_CLK_OFF) {
        usbEnsureGccRcg(hw);
    }
    (void)usbEnableGccBranches(hw);


    // If Femto v2 HS PHY is already awake and out of POR from UEFI (and not forced into full re-init),
    // preserve its live calibration and just clear retention + affirm external VBUS valid.
    const bool hsPhyAlive = (hw.phyInitMode == 0u) &&
                            ((hw.prd(FEMTO_UTMI_CTRL0) & 1u) != 0u) &&
                            ((hw.prd(FEMTO_UTMI_CTRL5) & 3u) == 0u);
    if (hsPhyAlive) {
        hw.pmask(FEMTO_CFG0, 0x03u, 0x00u);
        hw.pmask(FEMTO_COMMON1, 0x10u, 0x10u);
        hw.pmask(FEMTO_CTRL1, 0x01u, 0x01u);
        hw.pmask(FEMTO_CTRL2, 0x04u, 0x04u);
    } else {
        usbInitFemtoHsPhy(hw);
    }
    return true;
}

#endif // J716F_USB_DEBUG_HW_H
