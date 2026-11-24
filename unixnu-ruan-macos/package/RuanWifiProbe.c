// SPDX-License-Identifier: BSD-2-Clause-Patent
// RuanWifiProbe: Qualcomm WCN6750 Wi-Fi (icnss2 / ath11k) hardware probe & MSA management for ruan.
#include "RuanWifiProbe.h"
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <string.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t va, vm_size_t size);

struct ruan_wifi_telemetry g_ruan_wifi = {0};

void ruan_wifi_init(void)
{
    unsigned enable = 1;
    PE_parse_boot_argn("ruan-wifi", &enable, sizeof(enable));
    if (!enable) {
        strlcpy(g_ruan_wifi.status, "disabled by bootarg", sizeof(g_ruan_wifi.status));
        return;
    }

    g_ruan_wifi.msa_phys = WCN6750_MSA_PHYS;
    g_ruan_wifi.msa_size = WCN6750_MSA_SIZE;
    g_ruan_wifi.msi_addr = WCN6750_MSI_ADDR;

    // Check MSA reserved physical memory window (in DRAM 0x82A00000, 12 MiB)
    volatile uint32_t *msa = (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)WCN6750_MSA_PHYS, 0x1000, 7);
    if (msa) {
        g_ruan_wifi.msa_verified = 1;
        ml_io_unmap((vm_offset_t)msa, 0x1000);
    }

    g_ruan_wifi.probed = 1;
    if (g_ruan_wifi.msa_verified) {
        strlcpy(g_ruan_wifi.status, "WCN6750: MSA window mapped (no chip access, not started)", sizeof(g_ruan_wifi.status));
    } else {
        strlcpy(g_ruan_wifi.status, "WCN6750: MSA window unmapped (not started)", sizeof(g_ruan_wifi.status));
    }

    IOLog("ruan-wifi: %s\n", g_ruan_wifi.status);
}

const char *ruan_wifi_status_str(void)
{
    return g_ruan_wifi.status[0] ? g_ruan_wifi.status : "uninitialized";
}
