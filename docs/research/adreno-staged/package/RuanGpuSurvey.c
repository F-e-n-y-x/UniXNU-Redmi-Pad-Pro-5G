// SPDX-License-Identifier: MIT
// Stage S0 of the ruan Adreno 710 bring-up (gpu-prep/adreno/PLAN.md): a READ-ONLY look at the GPU clock controller's power
// domains, the first thing amethyst's GpuPowerProbe read (modes 1-3) before it powered anything. Runs only with
// ruan-gpu-survey=1. No writes anywhere. The addresses are the live DT's (macOS/hardware/private/ruan-live.dts):
//   clock-controller@3d90000 "qcom,parrot-gpucc" reg 0x3d90000+0xa000           (dts:6663-6665)
//   qcom,gdsc@3d99108 gpu_cc_cx_gdsc, qcom,gdsc@3d9905c gpu_cc_gx_gdsc          (dts:12205-12252)
//   syscon@3d9953c/3d99504/3d99058/3d99358/3d9958c (the GDSCs' hw-ctrl/collapse/retention cells, same block)
// Risk: gpucc CSRs sit behind GCC's gpu_cfg_ahb branch. If the boot firmware left it off, the first read may raise an
// SError; the breadcrumb is printed before it, so a panic names this probe (RAM-only boot, nothing persistent is touched).
#include <stdint.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

extern void boot_progress_stage(const char *s);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);

#define GPUCC_PAGE 0x03d99000ULL
#define GDSC_PWR_ON (1u << 31)   // GDSCR bit 31 (Linux gdsc.c PWR_ON_MASK)

static const struct {uint16_t off; const char *name;} regs[] = {
	{0x108, "cx_gdscr"}, {0x05c, "gx_gdscr"}, {0x53c, "sc53c"}, {0x504, "sc504"}, {0x058, "sc058"}, {0x358, "sc358"}, {0x58c, "sc58c"},
};

void
ruan_gpu_survey(void)
{
	unsigned mode = 0;
	if (!PE_parse_boot_argn("ruan-gpu-survey", &mode, sizeof(mode)) || mode != 1) {
		return;
	}
	boot_progress_stage("gpu survey: reading gpucc 0x3d99000 (read only; an SError here = gpu_cfg_ahb off)");
	vm_offset_t va = ml_io_map_unmappable((vm_offset_t)GPUCC_PAGE, 0x1000, 7);   // 7 = VM_WIMG_IO, uncached
	if (!va) {
		boot_progress_stage("gpu survey: map failed");
		return;
	}
	char line[200];
	int n = snprintf(line, sizeof(line), "gpu survey:");
	uint32_t cx = 0, gx = 0;
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]) && n > 0 && (size_t)n < sizeof(line); i++) {
		uint32_t v = *(volatile uint32_t *)(va + regs[i].off);
		if (regs[i].off == 0x108) cx = v;
		if (regs[i].off == 0x05c) gx = v;
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%08x", regs[i].name, v);
	}
	ml_io_unmap(va, 0x1000);
	boot_progress_stage(line);
	snprintf(line, sizeof(line), "gpu survey: CX %s, GX %s", (cx & GDSC_PWR_ON) ? "on" : "off", (gx & GDSC_PWR_ON) ? "on" : "off");
	boot_progress_stage(line);
}
