// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: UniXNU's Adreno740 driver on the A710 that package/RuanGpuPower.c powered (GMU booted, HFI up, GX on, gen7 hw_init
// without ruan's own stream-0 context). Steps 2 and 3 of macOS/docs/research/GPU-METAL-PORT.md, after the reference
// UniXNU-sheng package/GpuPowerProbe.cpp gpuStart() / publishAndWrapperTest(). What differs from sheng, and why:
//  - the SMMU and GPU registers go through RuanGpuPower.c's accessors (its allowlist and USB trace), not a raw mapping;
//  - the GPU context is bank 2 with ONE route, SID 0 mask 0x400 (ruan DT), not bank 0 with SIDs 0 and 1: under Gunyah bank 0 is
//    not ours to take (the survey found banks 1..5 free) and the DT has one masked stream;
//  - on top of the kit's own behaviour this file refuses any SMMU write that would disable a route or a bank's GR1 entry
//    (SMR without VALID, CBAR type 0): Gunyah answers CBAR 0 with an SEA (rbu6b). Nothing here tears the context down;
//  - no GMU bring-up (ShengGmuColdStart): RuanGpuPower.c's GMU boot, OOB GPU_SET and GX vote stay in charge;
//  - no zap shader / CP_SET_SECURE_MODE, no preemption (A710: gpu-preempt=0); GPU IRQ (SPI 300) on by default (ruan-gpu-irq=0: polled);
//  - the power-up list is the a7xx-common one with empty IFPC and dynamic parts (A710 runs without IFPC);
//  - CP_BV_RB_RPTR_ADDR is 0xa98 on the A710 (RuanA710Regs.h), not the A740 offset.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>
#include <string.h>
#include "RuanAdrenoGlue.h"
#include "RuanA710Regs.h"   // RUAN_RBBM_INT_* and the gen7 interrupt masks (attachIrq)
#include "RuanZap.h"
#include "RuanZapFw.h"   // build/ruan-adreno: a710_zap.mbn as ruan_zap_firmware[] (scripts/build-ruan-adreno.py)
extern "C" boolean_t PE_parse_boot_argn(const char *, void *, int);
extern "C" volatile uint32_t native_adreno_diag;   // NativeAdrenoSubmit.cpp
// diag=1's IB head words (NativeAdrenoVm::headWords) need a DRAM reader for BOs without a kernel mapping (all of Turnip's): one aligned
// word through a fresh uncached mapping, UniXNU-sheng GpuPowerProbe.cpp physRead64/dramRead32. Read only.
extern "C" int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
// UniXNU ArmCpu 5f37026 (#299) weak-imports this platform-sleep hook, which xnu-10063 lacks; nothing calls it here, so
// platform-sleep=1 stays a no-op on ruan. Defined in this force-loaded archive so the kernel links.
extern "C" void (*arm64_platform_sleep_hook)(boolean_t) = nullptr;
static int ruanDramRead32(uint64_t pa, uint32_t *value)
{
	const vm_offset_t va = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
	if (!va) return 1;
	*value = *reinterpret_cast<volatile uint32_t *>(va + (pa & 0xffc));
	ml_io_unmap(va, 0x1000);
	return 0;
}
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoMemory.h"   // NativeAdrenoMaintainOwnedPages (zapExit)
#include "AdrenoPM4.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" IOReturn native_adreno_pool_add(uint64_t physical, uint64_t bytes);

// NativeAdrenoPublish.cpp's scanout/handoff calls belong to GopFramebuffer's NativeGOPPresenter, which ruan does not build
// (it presents through NativeLegacyDisplay). Weak, so a kernel that links the presenter gets the real ones.
extern "C" __attribute__((weak)) IOReturn native_gop_handoff_call(uint32_t, task_t, const void *, size_t, void *, size_t)
{
	return kIOReturnUnsupported;
}
extern "C" __attribute__((weak)) IOMemoryDescriptor *native_gop_handoff_memory(void) {return nullptr;}
// The driver's optional (weak_import) kernel hooks: sheng's XNU patches define them, ruan's do not, and the kernel's static link
// refuses an undefined weak import. Each stub answers as the driver's "hook absent" branch would.
extern "C" __attribute__((weak)) int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -1;}
extern "C" {__attribute__((weak)) volatile uint32_t native_gpu_dcvs[15];}
extern "C" __attribute__((weak)) unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) {return 0;}
// shortcut: without XNU's gpupool-device-pager-close patch every client view of a pool grant is untracked, so the grant retires
// (the driver's documented no-hook behaviour; native_adreno_pool_views[4] then reads 1). Port the patch before Metal clients.
extern "C" __attribute__((weak)) kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *)
{
	return KERN_FAILURE;
}

namespace {
enum : uint32_t {
	A7xxCpSqeInstrBase = 0x0830, A7xxCpSqeCntl = 0x0808, A710CpBvRbRptrAddr = 0x0a98,   // RuanA710Regs.h
	Pm4MemWrite = 0x3d, ScratchMagic = 0x0a710c0du, Asid = 2, FenceTimeoutUs = 200000,
};
ruan_adreno_glue_io gio;   // copied: the driver keeps calling through it after the run
// Stage results: [0] step reached, [1] context IOReturn, [2] CP init IOReturn, [3] fence result, [4] fence seen,
// [5] mem-write IOReturn, [6] its fence result, [7] scratch word, [8] publish IOReturn, [9] native_adreno_pool_add IOReturn.
uint32_t r[10];
bool cpDone, published;
NativeAdrenoGmuContextReport ctxReport;
NativeAdrenoGpuBuffer bufs[GpuBufferCount];
IOMemoryMap *maps[GpuBufferCount];
volatile uint8_t *cpu[GpuBufferCount];
NativeAdrenoRing ring;
NativeAdrenoCpInit cpInit;
NativeAdrenoSubmit submit;
NativeAdrenoTimer timer;
// A710 facts for the Vulkan driver (GPU-METAL-PORT.md): chip 0x07010000, GMEM 512 KiB, the a7xx GMEM window and trap base,
// highest bank bit 15, UBWC 4.0 (Turnip starts with TU_DEBUG=noubwc), macrotile 0. va_size is set from the context's ias.
// gmem_base 0: KGSL gen7_3_0 (adreno-gpulist.h .gmem_base = 0); UCHE_GMEM_RANGE reads 0/0 on the device (rbu52b).
NativeAdrenoAbiDeviceInfo deviceInfo = {0x07010000u, 512u << 10, 0x0ull, 0x1fffffffff000ull, GpuClientVaStart,
                                        0, 15, 6, 0, 4, NativeAdrenoDeviceClientIova, 0};

void line(const char *s) {if (gio.line) gio.line(s);}

// SMMU: RuanGpuPower.c's window, 64-bit registers as two 32-bit accesses (low word first), as its own stream-0 code does.
int smmuRd32(void *, size_t at, uint32_t *v) {return at + 4 > gio.smmu_bytes || gio.smmu_rd(uint32_t(at), v) ? 1 : 0;}
int smmuRd64(void *, size_t at, uint64_t *v)
{
	uint32_t lo = 0, hi = 0;
	if (smmuRd32(nullptr, at, &lo) || smmuRd32(nullptr, at + 4, &hi)) return 1;
	*v = uint64_t(hi) << 32 | lo;
	return 0;
}
int smmuWr32(void *, size_t at, uint32_t v)
{
	if (at + 4 > gio.smmu_bytes) return 1;
	// Gunyah: never a GR1 CBAR of type 0 (rbu6b SEA) and never an SMR without VALID (a route teardown).
	if (at >= 0x1000 && at < 0x1800 && !((v >> 16) & 3u)) {line("glue: refused CBAR type 0"); return 1;}
	if (at >= 0x800 && at < 0xc00 && !(v >> 31)) {line("glue: refused SMR without VALID"); return 1;}
	return gio.smmu_wr(uint32_t(at), v) ? 1 : 0;
}
int smmuWr64(void *, size_t at, uint64_t v) {return smmuWr32(nullptr, at, uint32_t(v)) || smmuWr32(nullptr, at + 4, uint32_t(v >> 32));}
int barrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}

int gpuRd(void *, uint32_t reg, uint32_t *v) {return gio.gpu_rd(reg, v) ? 1 : 0;}
int gpuWr(void *, uint32_t reg, uint32_t v) {return gio.gpu_wr(reg, v) ? 1 : 0;}
// The buffers' CPU view is write-combined (Normal NC): ordering is all the ring and its fence page need.
int publishWords(void *, const volatile uint32_t *, size_t) {return barrier(nullptr);}
int acquireWords(void *, volatile uint32_t *, size_t) {return barrier(nullptr);}
uint64_t nowUs(void *)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}
// ruan-gpu-irq=1 (attachIrq, after publish): SPI 300's handler bumps irqCount and wakes this event. A pause that may block then sleeps
// until the next GPU interrupt or 1 ms, whichever is first (sheng GpuPowerProbe.cpp ringIdle); the 1 ms cap covers an interrupt that
// came between the caller's fence check and the sleep. Before the attach, or where it may not block, it spins 20 us as before.
volatile bool irqOn;
volatile uint32_t irqCount;
extern "C" int get_preemption_level(void);
extern "C" boolean_t ml_get_interrupts_enabled(void);
extern "C" boolean_t ml_at_interrupt_context(void);
extern "C" {volatile uint32_t ruan_gpu_irq[10];}   // see attachIrq
int idle(void *)
{
	if (irqOn && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
		assert_wait_timeout(event_t(&irqCount), THREAD_UNINT, 1000, NSEC_PER_USEC);
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) ruan_gpu_irq[7] = ruan_gpu_irq[7] + 1;
		ruan_gpu_irq[6] = ruan_gpu_irq[6] + 1;
		return 0;
	}
	IODelay(20);
	return 0;
}

const NativeAdrenoRingIo ringIo{nullptr, gpuWr, gpuRd, barrier, nullptr, nullptr};
const NativeAdrenoRingCache ringCache{nullptr, publishWords, acquireWords};
const NativeAdrenoRingClock ringClock{nullptr, nowUs, idle};
const NativeAdrenoTimeIo timeIo{nullptr, gpuRd};

ArmSmmu4KIo smmu{nullptr, 0, smmuRd32, smmuRd64, smmuWr32, smmuWr64, barrier};   // the context keeps a copy

void progress(uint32_t step)
{
	char s[48];
	snprintf(s, sizeof s, "glue: GPU context step %u", step);
	line(s);
}

// gpuStart() of the sheng reference, on ruan's bank 2.
void contextAndCp()
{
	smmu.register_bytes = gio.smmu_bytes;
	r[0] = 1;
	// The reserved pool first: the context's contiguous buffers fall back to it (patch 0003), and client BOs use it later.
	r[9] = gio.pool_bytes ? uint32_t(native_adreno_pool_add(gio.pool_base, gio.pool_bytes)) : uint32_t(kIOReturnNoResources);
	r[1] = uint32_t(NativeAdrenoGpuContextBringupIo(&smmu, gio.bank, Asid, gio.slot, gio.sid, gio.mask, progress, &ctxReport));
	if (r[1]) return;
	r[0] = 2;
	if (NativeAdrenoGpuContextBuffers(bufs)) return;
	for (unsigned i = 0; i < GpuBufferCount; ++i) {
		IODeviceMemory *m = IODeviceMemory::withRange(bufs[i].physical, bufs[i].bytes);
		if (m) {maps[i] = m->map(kIOMapAnywhere | kIOMapWriteCombineCache); m->release();}
		if (!maps[i]) return;
		cpu[i] = reinterpret_cast<volatile uint8_t *>(maps[i]->getVirtualAddress());
	}
	// adreno_fw_create_bo copies the SQE microcode without its first dword.
	r[0] = 3;
	if (!gio.sqe || gio.sqe_bytes < 8 || gio.sqe_bytes - 4 > bufs[0].bytes) return;
	for (size_t i = 4; i < gio.sqe_bytes; ++i) cpu[0][i - 4] = gio.sqe[i];
	barrier(nullptr);
	for (size_t i = 4; i < gio.sqe_bytes; ++i) if (cpu[0][i - 4] != gio.sqe[i]) return;
	// CP_SQE_INSTR_BASE before CP_SQE_CNTL, as a6xx hw_init does (CpInit then only reads it back).
	r[0] = 4;
	if (gpuWr(nullptr, A7xxCpSqeInstrBase, uint32_t(bufs[0].iova)) || gpuWr(nullptr, A7xxCpSqeInstrBase + 1, uint32_t(bufs[0].iova >> 32)))
		return;
	if (ring.attach(&ringIo, &ringCache, const_cast<uint8_t *>(cpu[1]), bufs[1].bytes, bufs[1].iova, const_cast<uint8_t *>(cpu[2]),
	        bufs[2].bytes, bufs[2].iova))
		return;
	if (gpuWr(nullptr, A710CpBvRbRptrAddr, uint32_t(ring.bvRptrIova())) || gpuWr(nullptr, A710CpBvRbRptrAddr + 1, uint32_t(ring.bvRptrIova() >> 32)))
		return;
	if (gpuWr(nullptr, A7xxCpSqeCntl, 1)) return;
	r[0] = 5;
	size_t written = 0;
	if (NativeAdrenoPwrupList::buildA740(gpuRd, nullptr, const_cast<uint8_t *>(cpu[3]), bufs[3].bytes, &written)) return;
	barrier(nullptr);
	const NativeAdrenoCpInitConfig config{bufs[0].iova, bufs[3].iova, 1};
	uint32_t fence = 0;
	r[2] = uint32_t(cpInit.run(&ringIo, &ring, &config, &fence));
	if (r[2]) return;
	r[0] = 6;
	r[3] = ring.wait(fence, &ringClock, FenceTimeoutUs, &r[4]);
	if (r[3]) return;
	// The first real command: CP_MEM_WRITE (address lo/hi, data) into the scratch page, then its fence.
	r[0] = 7;
	volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(cpu[4]);
	scratch[0] = 0;
	barrier(nullptr);
	uint32_t words[4] = {};
	if (AdrenoPM4Type7(Pm4MemWrite, 3, &words[0]) != AdrenoPM4Success) return;
	words[1] = uint32_t(bufs[4].iova), words[2] = uint32_t(bufs[4].iova >> 32), words[3] = ScratchMagic;
	r[5] = uint32_t(ring.submit(words, 4, &fence));
	if (r[5]) return;
	r[6] = ring.wait(fence, &ringClock, FenceTimeoutUs, &r[4]);
	r[7] = scratch[0];
	if (!r[6] && r[7] == ScratchMagic) {r[0] = 8; cpDone = true;}
}

// gpu-zap=1 (rbu58): leave secure mode as KGSL gen7 does with a zap shader, amethyst's mode 15 on ruan's values: SCM PAS 13
// (a710_zap.mbn) into the DT's gpu_microcode_region 0x89e1a000+0x2000, then CP_SET_SECURE_MODE 0 from the ring and a CP_MEM_WRITE
// behind it. RBBM_SECVID_TRUST_CNTL (0xf400) is secure-locked (rbu57: an HLOS write took a synchronous external abort), read only.
// Every step is a boot-progress event before it runs; z[] keeps the statuses. Runs once, after the CP is up, before publish.
constexpr uint32_t RuanZapPas = 13u, TrustCntl = 0xf400u;
constexpr uint64_t RuanZapRegion = 0x89e1a000ull, RuanZapRegionBytes = 0x2000ull;
uint64_t z[16];
unsigned zapMode;   // the gpu-zap boot argument
struct ScmReply {uint64_t status, value; unsigned again;};
extern "C" void hakios_spmi_lock(void);     // RuanInput.c
extern "C" void hakios_spmi_unlock(void);
// The boot-progress events already reach the ramoops zone synchronously (a device mapping, dsb sy per line, BootProgress.c
// zone_put); the pause after each zap line only gives the USB trace and the screen time before a call that may reset the SoC.
void zapLine(const char *s) {line(s); IODelay(20000);}
ScmReply scmCall(const char *what, uint32_t svc, uint32_t cmd, uint32_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3 = 0)
{
	char s[96];
	snprintf(s, sizeof s, "zap scm %s %08x %llx %llx %llx", what, gpb_scm_fn(svc, cmd), a0, a1, a2);
	zapLine(s);
	uint64_t q[8] = {gpb_scm_fn(svc, cmd), arginfo, a0, a1, a2, a3, 0, 0};
	ScmReply out{};
	// No SPMI arbiter traffic (RuanButtons/RuanBattery) while TZ runs or holds a suspended PAS call: buttons=1 boots reset
	// inside init_image (k3, k4b: last line "zap scm init_image", no reply); buttons=0 boots always got a reply.
	hakios_spmi_lock();
	for (;;) {
		uint64_t x[8] = {q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]};
		ruan_smc_call(x);
		if (x[0] != GPB_SCM_INTERRUPTED || out.again >= 1000) {out.status = x[0]; out.value = x[1]; break;}
		++out.again;
		q[0] = GPB_SCM_INTERRUPTED; q[6] = x[6];
	}
	hakios_spmi_unlock();
	snprintf(s, sizeof s, "zap scm %s -> st %llx val %llx again %u", what, out.status, out.value, out.again);
	zapLine(s);
	return out;
}
// The load segments into gpu_microcode_region (0x89e1a000: in the low bank, never adopted by memory-outside, so XNU has no aperture
// mapping of it; this is the only mapping). Write-combined as KGSL's memremap(MEMREMAP_WC), or device memory (gpu-zap=10).
// firstWord: one 32-bit store of the first segment's first word only (gpu-zap=8), logged before and after.
gpb_mdt zapImage;
int placeSegments(bool uncached, bool firstWord)
{
	char s[96];
	snprintf(s, sizeof s, "zap: mapping %llx+%llx %s", RuanZapRegion, RuanZapRegionBytes, uncached ? "uncached" : "write-combined");
	zapLine(s);
	IODeviceMemory *region = IODeviceMemory::withRange(RuanZapRegion, RuanZapRegionBytes);
	IOMemoryMap *map = region ? region->map(kIOMapAnywhere | (uncached ? kIOMapInhibitCache : kIOMapWriteCombineCache)) : nullptr;
	if (region) region->release();
	if (!map) {zapLine("zap: region map failed"); return 1;}
	volatile uint8_t *va = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
	if (firstWord) {
		const uint32_t o = zapImage.offset[0], word = uint32_t(ruan_zap_firmware[o]) | uint32_t(ruan_zap_firmware[o + 1]) << 8 |
		    uint32_t(ruan_zap_firmware[o + 2]) << 16 | uint32_t(ruan_zap_firmware[o + 3]) << 24;
		snprintf(s, sizeof s, "zap: one word %08x to %llx", word, RuanZapRegion + (zapImage.addr[0] - zapImage.min_addr));
		zapLine(s);
		*reinterpret_cast<volatile uint32_t *>(va + (zapImage.addr[0] - zapImage.min_addr)) = word;
		barrier(nullptr);
		zapLine("zap: one word written");
	} else {
		zapLine("zap: writing the load segments into the region");
		gpb_mdt_place(ruan_zap_firmware, &zapImage, va);
		barrier(nullptr);
		zapLine("zap: segments written");
	}
	map->release();
	return 0;
}
void zapExit()
{
	char s[96];
	uint32_t trust = 0;
	(void)gpuRd(nullptr, TrustCntl, &trust);
	z[14] = trust;
	snprintf(s, sizeof s, "zap: trust %x, PAS %u into %llx+%llx", trust, RuanZapPas, RuanZapRegion, RuanZapRegionBytes);
	zapLine(s);
	z[0] = 1;
	ScmReply q = scmCall("is_call_avail", GPB_SCM_SVC_INFO, GPB_SCM_INFO_IS_CALL_AVAIL, GPB_SCM_ARGS_1,
	                     gpb_scm_query(GPB_SCM_SVC_PIL, GPB_SCM_PAS_IS_SUPPORTED), 0, 0);
	z[1] = q.status; z[2] = q.value;
	if (q.status || !q.value) return;
	q = scmCall("is_supported", GPB_SCM_SVC_PIL, GPB_SCM_PAS_IS_SUPPORTED, GPB_SCM_ARGS_1, RuanZapPas, 0, 0);
	z[3] = q.status; z[4] = q.value;
	// gpu-zap=2 (rbu59a): go on when PAS 13 is reported unsupported (rbu58: st 0 val 0; amethyst's A810 said supported). The call is
	// Linux qcom_scm_pas_supported's: SMCCC 64-bit SiP std call 0x42000207, arginfo 1, x2 = 13, result in x1.
	if (q.status || !q.value) {
		if (zapMode < 2) return;   // rbu60: was `!= 2`, so 3..6 stopped here
		zapLine("zap: is_supported says no, going on (gpu-zap=2)");
	}
	z[0] = 2;
	struct gpb_mdt m;
	z[11] = gpb_mdt_parse(ruan_zap_firmware, sizeof ruan_zap_firmware, uint32_t(RuanZapRegionBytes), &m);
	zapImage = m;
	snprintf(s, sizeof s, "zap mbn rule %llu meta %x loads %u span %x-%x", z[11], m.meta_bytes, m.loads, m.min_addr, m.max_addr);
	zapLine(s);
	if (z[11] || m.meta_bytes > 0x1000) return;
	// The metadata page. gpu-zap 2/3: one kernel page (above 4 GiB: ruan's RAM all is). gpu-zap 4/5/6: the first page of ramoops
	// dmesg record 0 (0x80D00000, reserved HLOS memory below 4 GiB, BootProgress.c writes only the console zone after it), since
	// Linux's qcom_scm takes the metadata from dma_alloc_coherent with the default 32-bit mask. Kept for good.
	const bool low = zapMode >= 4;
	uint64_t metaPa = 0;
	if (low) {
		IODeviceMemory *page = IODeviceMemory::withRange(0x80D00000ull, 0x1000);
		IOMemoryMap *map = page ? page->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
		if (page) page->release();
		if (!map) {zapLine("zap: low metadata page map failed"); return;}
		volatile uint8_t *va = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
		static uint8_t buffer[0x1000];   // not on the 16 KiB kernel stack
		memset(buffer, 0, sizeof buffer);
		gpb_mdt_metadata(ruan_zap_firmware, &m, buffer);
		for (unsigned k = 0; k < 0x1000; ++k) va[k] = buffer[k];
		barrier(nullptr);
		metaPa = 0x80D00000ull;   // the map stays: the monitor may read the page until auth_and_reset
	} else {
		// One page is physically contiguous whatever the kernel's free memory (ruan serves no larger contiguous request).
		IOBufferMemoryDescriptor *meta = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 0x1000, 0x1000);
		if (!meta || meta->prepare() != kIOReturnSuccess) {zapLine("zap: metadata page refused"); return;}
		uint8_t *metaVa = static_cast<uint8_t *>(meta->getBytesNoCopy());
		IOByteCount length = 0;
		metaPa = meta->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
		if (!metaVa || !metaPa || length < 0x1000) {zapLine("zap: metadata page has no address"); return;}
		memset(metaVa, 0, 0x1000);
		gpb_mdt_metadata(ruan_zap_firmware, &m, metaVa);
		if (NativeAdrenoMaintainOwnedPages(metaVa, 0x1000, NativeAdrenoClean)) {zapLine("zap: metadata clean failed"); return;}
		barrier(nullptr);
	}
	snprintf(s, sizeof s, "zap: metadata %x bytes at %llx (gpu-zap=%u)", m.meta_bytes, metaPa, zapMode);
	zapLine(s);
	// gpu-zap=7 (rbu61e) put an SHM bridge around the metadata page: create st 0, then init_image st ffcffff6. Removed (not needed).
	// gpu-zap 9/10 (rbu62b/c): the load segments go into the region before init_image (TZ may lock it from mem_setup on).
	if (zapMode == 9 || zapMode == 10) {zapLine("zap: segments first (gpu-zap=9/10)"); if (placeSegments(zapMode == 10, false)) return;}
	z[0] = 3;
	q = scmCall("init_image", GPB_SCM_SVC_PIL, GPB_SCM_PAS_INIT_IMAGE, GPB_SCM_ARGS_INIT_IMAGE, RuanZapPas, metaPa, 0);
	z[5] = q.status; z[6] = q.value;
	if (q.status || q.value) return;
	if (zapMode == 3 || zapMode == 5) {zapLine("zap: stop after init_image (gpu-zap=3/5)"); return;}
	q = scmCall("mem_setup", GPB_SCM_SVC_PIL, GPB_SCM_PAS_MEM_SETUP, GPB_SCM_ARGS_3, RuanZapPas, RuanZapRegion, m.max_addr - m.min_addr);
	z[7] = q.status; z[8] = q.value;
	if (q.status || q.value) return;
	if (zapMode == 6) {zapLine("zap: stop after mem_setup, no segment writes (gpu-zap=6)"); return;}
	if (zapMode == 8) {(void)placeSegments(false, true); zapLine("zap: stop after the first word (gpu-zap=8)"); return;}
	if (zapMode != 9 && zapMode != 10 && placeSegments(false, false)) return;
	q = scmCall("auth_and_reset", GPB_SCM_SVC_PIL, GPB_SCM_PAS_AUTH_AND_RESET, GPB_SCM_ARGS_1, RuanZapPas, 0, 0);
	z[9] = q.status; z[10] = q.value;
	if (q.status || q.value) return;
	z[0] = 4;
	zapLine("zap: CP_SET_SECURE_MODE 0 from the ring");
	volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(cpu[4]);
	scratch[1] = 0;
	barrier(nullptr);
	uint32_t words[6] = {}, fence = 0, seen = 0;
	if (AdrenoPM4Type7(GPB_CP_SET_SECURE_MODE, 1, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(Pm4MemWrite, 3, &words[2]) != AdrenoPM4Success)
		return;
	words[1] = 0;
	const uint64_t at = bufs[4].iova + 4;
	words[3] = uint32_t(at), words[4] = uint32_t(at >> 32), words[5] = 0x05ec0ff1u;
	z[12] = uint32_t(ring.submit(words, 6, &fence));
	if (!z[12]) z[13] = ring.wait(fence, &ringClock, FenceTimeoutUs, &seen);
	barrier(nullptr);
	(void)gpuRd(nullptr, TrustCntl, &trust);
	z[15] = trust;
	if (!z[12] && !z[13] && scratch[1] == 0x05ec0ff1u) z[0] = 5;
	snprintf(s, sizeof s, "zap: secure off submit %llx fence %llx scratch %08x trust %x -> %s", z[12], z[13], scratch[1], trust,
	         z[0] == 5 ? "ok" : "miss");
	zapLine(s);
}

// rbu75: the CP's state when it stops moving, captured at the time (a later `gpu` read found the hung IB already overwritten: Turnip
// zero-fills a cached BO it hands out again). Every 250 ms: RBBM_STATUS busy (bit 23), and RB rptr, IB1 address and IB1 remaining all
// unchanged for 6 samples (1.5 s) -> once: the registers and 48 words around the CP's position in IB1 (its size from the logged ring
// packet when there is one, else from the IB's start) through bank 2's installed table, as text for `gpu` and as boot-progress events.
extern "C" int ruan_gpu_va_read32(uint64_t gva, uint32_t *value) __attribute__((weak_import));
extern "C" volatile uint32_t native_adreno_ring_log[4][4 + 112];
extern "C" {char ruan_gpu_hang_text[6][192]; volatile uint32_t ruan_gpu_hang_seen;}
extern "C" int kernel_thread_start(void (*)(void *, int), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
void hangWatch(void *, int)
{
	uint32_t last[4] = {}, same = 0;
	for (;;) {
		IOSleep(50);   // rbu77: 50 ms samples, capture after 150 ms: the driver's hangcheck (500 ms periods) ends the client's device first
		uint32_t st = 0, rptr = 0, lo = 0, hi = 0, rem = 0, cp1 = 0;
		if (gpuRd(nullptr, 0x210, &st) || gpuRd(nullptr, 0x806, &rptr) || gpuRd(nullptr, 0x928, &lo) || gpuRd(nullptr, 0x929, &hi) ||
		    gpuRd(nullptr, 0x92a, &rem)) continue;
		(void)gpuRd(nullptr, 0x825, &cp1);
		const uint32_t now[4] = {rptr, lo, hi, rem};
		same = (st & (1u << 23)) && !memcmp(now, last, sizeof now) ? same + 1 : 0;
		if (!same && ruan_gpu_hang_seen) ruan_gpu_hang_seen = 0;   // that stall resolved: the next one is captured over it (the last capture stays readable)
		memcpy(last, now, sizeof last);
		if (same != 3 || ruan_gpu_hang_seen) continue;
		ruan_gpu_hang_seen = 1;
		const uint64_t base = (uint64_t)hi << 32 | lo;
		uint32_t dwords = 0;
		for (unsigned k = 0; k < 4 && !dwords; ++k)
			for (unsigned i = 0; i + 3 < 112 && i + 3 < native_adreno_ring_log[k][3]; ++i) {
				const volatile uint32_t *w = &native_adreno_ring_log[k][4 + i];
				if (w[0] == 0x70bf8003u && w[1] == lo && w[2] == hi) {dwords = w[3]; break;}
			}
		const uint64_t at = dwords && rem <= dwords ? base + 4ull * (dwords - rem) : base;
		const uint64_t from = at >= base + 4 * 24 ? at - 4 * 24 : base;
		snprintf(ruan_gpu_hang_text[0], sizeof ruan_gpu_hang_text[0], "gpu hang: rbbm %x cpst1 %x rptr %x ib1 %llx size %x rem %x at %llx",
		         st, cp1, rptr, (unsigned long long)base, dwords, rem, (unsigned long long)at);
		for (unsigned row = 0; row < 2; ++row) {
			size_t n = 0;
			for (unsigned i = 0; i < 24 && n < sizeof ruan_gpu_hang_text[1] - 12; ++i) {
				const uint64_t a = from + 4ull * (row * 24 + i);
				uint32_t v = 0;
				const int e = &ruan_gpu_va_read32 ? ruan_gpu_va_read32(a, &v) : -9;
				n += e ? (size_t)snprintf(ruan_gpu_hang_text[1 + row] + n, sizeof ruan_gpu_hang_text[1] - n, " !%d", e)
				       : (size_t)snprintf(ruan_gpu_hang_text[1 + row] + n, sizeof ruan_gpu_hang_text[1] - n, "%s%x", a == at ? " >" : " ", v);
			}
		}
		// rbu76: the ring around the CP's read pointer (rbu75: IB1 done, the CP stuck in the ring itself), 32 words before and 16 after,
		// and the memptrs page the ring's fences and shadows write (+0 rptr, +4 fence, +8 BV rptr, +12 BV fence).
		const volatile uint32_t *ringWords = reinterpret_cast<const volatile uint32_t *>(cpu[1]);
		const uint32_t mask = uint32_t(bufs[1].bytes / 4 - 1);
		for (unsigned row = 0; row < 2; ++row) {
			size_t n = (size_t)snprintf(ruan_gpu_hang_text[3 + row], sizeof ruan_gpu_hang_text[3], "ring %x:", (rptr - 32 + row * 24) & mask);
			for (unsigned i = 0; i < 24 && n < sizeof ruan_gpu_hang_text[3] - 12; ++i) {
				const uint32_t idx = (rptr - 32 + row * 24 + i) & mask;
				n += (size_t)snprintf(ruan_gpu_hang_text[3 + row] + n, sizeof ruan_gpu_hang_text[3] - n, "%s%x", idx == (rptr & mask) ? " >" : " ",
				                      ringWords ? ringWords[idx] : 0u);
			}
		}
		const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(cpu[2]);
		uint32_t wptr = 0;
		(void)gpuRd(nullptr, 0x807, &wptr);
		snprintf(ruan_gpu_hang_text[5], sizeof ruan_gpu_hang_text[5], "memptrs rptr %x fence %x bv_rptr %x bv_fence %x wptr %x issued %x",
		         mem ? mem[0] : 0u, mem ? mem[1] : 0u, mem ? mem[2] : 0u, mem ? mem[3] : 0u, wptr, ring.lastSubmitted());
		for (auto &t : ruan_gpu_hang_text) line(t);
	}
}

// ruan-gpu-irq=1: the GPU line (kgsl_3d0_irq = GIC SPI 300, level, ruan-live.dts) into the driver's completion pass, as sheng's
// GpuPowerProbe.cpp gpuIrq/irqAndTimer do: NativeAdrenoPublish.cpp native_adreno_gpu_interrupt then lets fence waiters sleep until the
// pass wakes them (gpu-fence-wake=0 keeps them on idle() above). RBBM_INT_0_MASK already carries CACHE_FLUSH_TS (bit 20) from
// RuanGpuPower.c s3_cp (RUAN_A710_GEN7_INT_MASK), and every ring fence is a CP_EVENT_WRITE7 with its IRQ bit (NativeAdrenoRing.cpp).
// ruan_gpu_irq: [0] attach IOReturn (0xdead no armcpu_spi_attach, 0xbad no mapping), [1] GICD readback (group | enabled << 1 |
// edge << 2 | priority << 8), [2] interrupts, [3] OR of the statuses cleared, [4] the last one, [5] RBBM_INT_0_MASK at attach,
// [6] idle() pauses that slept, [7] of those, timed out, [8] storms, [9] OR of the error bits (RUAN_A710_INT_ERRORS) cleared here.
extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" IOReturn armcpu_spi_mask(uint32_t, bool) __attribute__((weak_import));
extern "C" void native_adreno_gpu_interrupt(uint32_t status);   // NativeAdrenoPublish.cpp
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
constexpr uint32_t GpuIntid = 300 + 32, StormEmpty = 100000;
constexpr uint64_t GpuRegBase = 0x03d00000ull;   // kgsl_3d0_reg_memory (RuanGpuPowerBoard.h GPB_GPU_BASE)
volatile uint32_t *rbbm;   // its first 4 KiB, raw: the handler must not go through RuanGpuPower.c's accessors (USB trace)
volatile uint32_t irqEmpty;
thread_call_t stormCall;
// After a storm the line is masked, but the driver's waiters already sleep on the completion pass (g.interrupts stays set): a
// synthetic CACHE_FLUSH_TS every 1 ms keeps them moving.
// shortcut: a 1 ms software tick for the rest of the boot; if a storm is ever seen, find the source that never clears instead.
void stormTick(thread_call_param_t, thread_call_param_t)
{
	native_adreno_gpu_interrupt(1u << 20);
	thread_wakeup(event_t(&irqCount));
	uint64_t deadline = 0;
	clock_interval_to_deadline(1, kMillisecondScale, &deadline);
	thread_call_enter_delayed(stormCall, deadline);
}
// a6xx_irq / gen7 KGSL: read RBBM_INT_0_STATUS and write it back to RBBM_INT_CLEAR_CMD before anything else, so the level line drops
// before the EOI. Interrupt context: no locks, no allocation, no USB.
void gpuIrq(void *)
{
	const uint32_t status = rbbm[RUAN_RBBM_INT_0_STATUS];
	if (status) {rbbm[RUAN_RBBM_INT_CLEAR_CMD] = status; __asm__ volatile("dsb sy" ::: "memory");}
	ruan_gpu_irq[2] = ruan_gpu_irq[2] + 1;
	ruan_gpu_irq[3] = ruan_gpu_irq[3] | status;
	ruan_gpu_irq[4] = status;
	ruan_gpu_irq[9] = ruan_gpu_irq[9] | (status & RUAN_A710_INT_ERRORS);
	irqEmpty = status ? 0 : irqEmpty + 1;
	if (irqEmpty > StormEmpty && irqOn) {   // a line with nothing to clear: mask it, back to the spin plus the 1 ms tick
		irqOn = false;
		(void)armcpu_spi_mask(GpuIntid, true);
		ruan_gpu_irq[8] = ruan_gpu_irq[8] + 1;
		if (stormCall) thread_call_enter(stormCall);
	}
	irqCount = irqCount + 1;
	native_adreno_gpu_interrupt(status);   // bit 20 (CP_CACHE_FLUSH_TS) or 8 (CP_SW) asks for the completion pass
	thread_wakeup(event_t(&irqCount));
}
void attachIrq()
{
	unsigned on = 1;   // macOS: default on (cpus=1: the 20 us fence spin starves WindowServer); ruan-gpu-irq=0 = polled
	PE_parse_boot_argn("ruan-gpu-irq", &on, sizeof on);
	if (!on) return;
	uint32_t *q = const_cast<uint32_t *>(ruan_gpu_irq);
	rbbm = reinterpret_cast<volatile uint32_t *>(ml_io_map(vm_offset_t(GpuRegBase), 0x1000));
	stormCall = thread_call_allocate(stormTick, nullptr);
	if (!rbbm || !stormCall) {q[0] = 0xbad; line("gpu irq: no RBBM mapping"); return;}
	(void)gpuRd(nullptr, RUAN_RBBM_INT_0_MASK, &q[5]);
	if (!(q[5] & (1u << 20))) (void)gpuWr(nullptr, RUAN_RBBM_INT_0_MASK, RUAN_A710_GEN7_INT_MASK);   // a restart that lost it
	uint64_t router = ~0ull;
	irqOn = true;   // before the attach: a fence left from CP init fires as soon as the line is enabled
	q[0] = armcpu_spi_attach ? uint32_t(armcpu_spi_attach(GpuIntid, false, gpuIrq, nullptr, &q[1], &router)) : 0xdeadu;
	if (q[0]) irqOn = false;
	IOSleep(20);
	char s[128];
	snprintf(s, sizeof s, "gpu irq: attach %x gicd %x router %llx mask %x irqs %u status %x", q[0], q[1], (unsigned long long)router,
	         q[5], q[2], q[3]);
	line(s);
}

// publishAndWrapperTest() of the reference without the wrapper rounds, the coherence probe, preemption and the CP aperture
// (bank 2 is not the SCM aperture's bank 0: the CPU switches TTBR0, table mode 1).
void publish()
{
	NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
	r[0] = 9;
	if (!vm || submit.attach(vm, &ring)) return;
	{   // diag=1: the driver's IB head words and job timeline (USB `gpu` IB lines), as sheng's board sets it before any client
		unsigned diag = 0;
		PE_parse_boot_argn("diag", &diag, sizeof diag);
		native_adreno_diag = diag == 1;
		if (diag == 1) native_adreno_dram_read32 = ruanDramRead32;
	}
	const IOReturn timed = timer.attach(&timeIo, NativeAdrenoTimeCp);
	const unsigned ias = NativeAdrenoGpuContextInputBits();
	deviceInfo.va_size = ias > 32 ? (1ull << ias) - GpuClientVaStart : 0;
	r[8] = uint32_t(native_adreno_publish(vm, &submit, &ring, &ringClock, timed ? nullptr : &timer, &deviceInfo, ias,
	                                      ctxReport.output_bits ? ctxReport.output_bits : 40));
	if (!r[8]) {
		r[0] = 10; published = true;
		attachIrq();   // after publish: native_adreno_gpu_interrupt needs the driver's fence thread call
		thread_t watcher = nullptr;
		if (kernel_thread_start(hangWatch, nullptr, &watcher) == KERN_SUCCESS) thread_deallocate(watcher);
	}
}
}

// patches/adreno/0004 (ArmSmmu4KLive.c): gpu-tlbiall=2 (default since rbu48a) TLBIASID, =1 CB TLBIALL (reset rbu47), =0 the kit's own
// per-page invalidation (stale on reuse), after every map/unmap of a process table; the first 24
// calls leave a boot-progress event before ("tlb m<mode> map<0|1> ..") and after ("... st<status> p<poll reads>"), so a ramoops replay
// shows whether a reset comes inside the invalidate or the sync.
extern "C" void boot_progress_stage(const char *);
extern "C" unsigned ruan_smmu_tlb_mode(void)
{
	static unsigned mode = ~0u;
	if (mode == ~0u) {unsigned v = 2; PE_parse_boot_argn("gpu-tlbiall", &v, sizeof v); mode = v <= 2 ? v : 2;}   // rbu48a: 2 fixed reuse
	return mode;
}
extern "C" void ruan_smmu_tlb_note(unsigned mode, unsigned map, int status, unsigned polls)
{
	static volatile uint32_t notes;
	if (notes >= 24) return;
	notes = notes + 1;
	char s[64];
	if (status < 0) snprintf(s, sizeof s, "tlb m%u map%u go", mode, map);
	else snprintf(s, sizeof s, "tlb m%u map%u st%d p%u", mode, map, status, polls);
	boot_progress_stage(s);
}

extern "C" int ruan_adreno_glue_run(unsigned stage, const struct ruan_adreno_glue_io *io, char *row, size_t row_bytes)
{
	if (!io || !io->gpu_rd || !io->gpu_wr || !io->smmu_rd || !io->smmu_wr || !row || (stage != 1 && stage != 2)) return -1;
	gio = *io;
	if (!r[0]) contextAndCp();       // one attempt per boot: the context and the CP cannot be brought up twice
	if (stage == 2 && cpDone && !published && r[0] != 9 && !z[0]) {
		// Default 9 since rbu62b (segments before init_image; the secure exit passes and Turnip's smoke test with it); gpu-zap=0 skips it.
		unsigned zap = 9;
		PE_parse_boot_argn("gpu-zap", &zap, sizeof zap);
		if (zap >= 1 && zap <= 10 && zap != 7) {zapMode = zap; zapExit();}
	}
	if (stage == 2 && cpDone && !published && r[0] != 9) publish();
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
	// macOS: the display never takes LegacyDisplayProbe's late path (where iPadOS publishes the Metal accelerator), so it is
	// published here, once the driver can serve its user client (full-OS boot 16: GPU m18 ok but METAL reg=0, nothing opened it).
	{
		extern void ruan_gpu_publish_accelerator(void) __attribute__((weak_import));
		if (stage == 2 && published && ruan_gpu_publish_accelerator) ruan_gpu_publish_accelerator();
	}
#endif
	char s[200];
	snprintf(s, sizeof s, "glue: step %u ctx %x (step %u kit %u/%u routes %u ias %u oas %u) cpinit %x fence %u seen %u "
	         "memwrite %x/%u scratch %08x publish %x pool %x", r[0], r[1], ctxReport.step, ctxReport.program_status, ctxReport.route_status,
	         ctxReport.routes_verified, NativeAdrenoGpuContextInputBits(), ctxReport.output_bits, r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]);
	line(s);
	snprintf(row, row_bytes, "glue s%u %s", r[0], stage == 2 ? (published ? "published" : "nopub") : (cpDone ? "fence ok" : "FAIL"));
	return stage == 2 ? (published ? 0 : -2) : (cpDone ? 0 : -2);
}
