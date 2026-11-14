// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage S1 of the ruan Adreno 710 bring-up (gpu-dev/README.md): ruan-gpu=<mode> runs RuanGpuPowerBoard.h's gated sequence,
//   1  read GCC's GPU branches, then GPUCC (only if gcc_gpu_cfg_ahb_clk is enabled), never write
//   2  also enable gcc_gpu_cfg_ahb_clk if it is off (the one write gcc-parrot's probe makes), then read GPUCC
//   3  also power the CX GDSC (downstream gdsc-regulator sequence), enable gpu_cc_ahb_clk and gpu_cc_hlos1_vote_gpu_smmu_clk,
//      then read the GPU SMMU's global space 0 and every SMR/S2CR
// Drop-in replacement for rb15g's RuanGpuSurvey.c: same entry (ruan_gpu_survey, called from boot_progress_start) and the same
// banner fields (ruan_gpu_row, ruan_gpu_short). ruan-gpu-survey=1 alone now means ruan-gpu=1 (gated: GPUCC is not read while
// its AHB clock is off, the SError risk S0 had). Writes are allowed only to the four registers listed in allowed(); any other
// write is refused here as well, whatever the board header asks for. Every window is announced before its first access, so a
// bus hang leaves its name as the last boot-page line.
#include <stdint.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include "RuanGpuPowerBoard.h"

extern void boot_progress_stage(const char *s);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void delay(int usec);

char ruan_gpu_row[128], ruan_gpu_short[32];
struct gpb_result ruan_gpu_result;

static const uint64_t phys[GPB_WINDOWS] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE};
static const uint64_t len[GPB_WINDOWS] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW};
static const char *const name[GPB_WINDOWS] = {"GCC GPU page 0x181000", "GCC vote page 0x162000", "GPUCC 0x3d90000",
                                              "GPU SMMU GR0 0x3da0000", "GPU RBBM 0x3d00000"};
static vm_offset_t va[GPB_WINDOWS];

static int allowed(unsigned w, uint32_t off)
{
	return (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) ||
	    (w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK));
}
static int rd(void *c, unsigned w, uint32_t off, uint32_t *out)
{
	(void)c;
	if (w >= GPB_WINDOWS || !va[w] || off + 4 > len[w]) return -1;
	*out = *(volatile uint32_t *)(va[w] + off);
	return 0;
}
static int wr(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w >= GPB_WINDOWS || !va[w] || !allowed(w, off)) return -1;
	*(volatile uint32_t *)(va[w] + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
static void stage(void *c, unsigned w)
{
	(void)c;
	char line[96];
	snprintf(line, sizeof(line), "gpu: touching %s", w == GPB_STAGE_CX_GDSC ? "CX GDSC (power-up writes)" : w < GPB_WINDOWS ? name[w] : "?");
	boot_progress_stage(line);
}
static void delay_us(void *c, unsigned us)
{
	(void)c;
	delay((int)us);
}

void
ruan_gpu_survey(void)
{
	unsigned mode = 0, survey = 0;
	PE_parse_boot_argn("ruan-gpu", &mode, sizeof(mode));
	if (!mode && PE_parse_boot_argn("ruan-gpu-survey", &survey, sizeof(survey)) && survey == 1) mode = 1;
	if (!mode) return;
	if (mode > GPB_MAX_MODE) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u>max%u", mode, GPB_MAX_MODE);
		boot_progress_stage("gpu: ruan-gpu above this kernel's last stage, nothing done");
		return;
	}
	unsigned have = 0;
	for (unsigned w = 0; w < GPB_WINDOWS; w++) {
		va[w] = ml_io_map_unmappable((vm_offset_t)phys[w], (vm_size_t)len[w], 7);   // 7 = VM_WIMG_IO, uncached
		if (va[w]) have |= 1u << w;
	}
	struct gpb_io io = {NULL, rd, wr, stage, delay_us};
	struct gpb_result *r = &ruan_gpu_result;
	gpb_run(&io, mode, have, r);
	for (unsigned w = 0; w < GPB_WINDOWS; w++) {
		if (va[w]) ml_io_unmap(va[w], (vm_size_t)len[w]);
		va[w] = 0;
	}
	char line[200];
	snprintf(line, sizeof(line), "gpu m%u gcc %s ahb %08x>%08x memnoc %08x dvm %08x axi %08x vote %08x w%u e%d",
	    mode, gpb_state_text(r->gcc), r->cfg_ahb, r->cfg_ahb_after, r->memnoc, r->dvm, r->axi, r->pll_vote, r->writes, r->io_error);
	boot_progress_stage(line);
	snprintf(line, sizeof(line), "gpu cc %s cx %08x hw %08x gx %08x ahbclk %08x smmuclk %08x gmuclk %08x", gpb_state_text(r->cc),
	    r->cx_gdscr, r->cx_hwctrl, r->gx_gdscr, r->cc_ahb, r->cc_smmu, r->cc_gmu);
	boot_progress_stage(line);
	strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	if (mode >= 3) {
		snprintf(line, sizeof(line), "gpu cx-up before %08x init %08x after %08x tries %u refused %u cx %u clocks %u", r->cx_before,
		    r->cx_init, r->cx_after, r->cx_tries, r->cx_refused, r->powered_cx, r->powered_clocks);
		boot_progress_stage(line);
		static const char *const why[] = {"", " (cx_gmu off)", " (hub_cx_int off)", " (snoc_dvm off)"};
		snprintf(line, sizeof(line), "gpu smmu %s%s gmu %08x hub %08x dvm %08x scr0 %08x id0 %08x id1 %08x smr %u valid %u",
		    gpb_state_text(r->smmu), why[r->smmu_why & 3], r->gate_gmu, r->gate_hub, r->gate_dvm, r->scr0, r->id0, r->id1,
		    r->num_smr, r->smr_valid);
		boot_progress_stage(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	const int cx = gpb_gdsc_on(r->cx_gdscr) || gpb_gdsc_on(r->cx_hwctrl);
	snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u cx%s gx%s smmu%s", mode, cx ? "on" : "off",
	    gpb_gdsc_on(r->gx_gdscr) ? "on" : "off", r->smmu == GPB_DONE ? "ok" : "-");
}
