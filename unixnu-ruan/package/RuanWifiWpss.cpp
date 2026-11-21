// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan-wifi=N: the WiFi processor (WPSS) of the Redmi Pad Pro 5G (SM7435) and the start of the WiFi module. NOT BUILT, NOT BOOTED.
// (macOS/docs/research/wifi/port, from draft/RuanWifiWpss.cpp.)
//
// The bring-up logic is filipnavara/UniXNU-asteroids 9f07af3 package/WpssProbe.cpp (ported in this directory). That code proved
// WCN6750 + ath11k on SM7635 under Gunyah. On the 3a it runs on the ADSP thread through AsteroidsRemote.h; ruan has no ADSP bring-up, so
// this file implements AsteroidsRemote.h on a polled thread of its own, with the same Qcom* helpers (sheng/asteroids, unchanged),
// RuanZap.h's ruan_smc_call for the secure calls, and asteroids' AdspFirmware.c + FatVolume.c to read qca6750/wpss.b00-.bNN in place
// from modem_a (FAT16) through ShengUfs (UFS variant), or a merged wpss.mbn from Data (RAM root, scripts/stage-wifi-firmware.py).
//
// ruan values, live DT macOS/hardware/private/ruan-live.dts (line):
//   SMEM smem_region@80900000 + 0x200000 (98-101); hwlock = TCSR mutex syscon@1f40000 stride 0x1000 (6238-6247), lock 3 (qcom,smem hwlocks
//   <0x124 0x03>, 6254) -> 0x1f43000; IPCC qcom,ipcc@ed18000 (6222-6225, SPI 0xe5), WPSS client 0x18 (6398-6399); AOSS QMP
//   power-controller@c300000 + 0x400, IPCC client 0 signal 0 (6493-6500); apps RSC DRV 2 at 0x17a20000, tcs-offset 0xd00, drv-id 2
//   (5113-5120); tcs-config <2 3 ...> = ACTIVE x3 first (5121), so TCS 0-2 are active: RuanTouch.c owns TCS 0, this file TCS 1 (cx/mx),
//   RuanWifi.cpp TCS 2 (WCN VRMs); cmd-db aop_cmd_db_region@80860000 + 0x20000 (67-70); PAS metadata fallback in mpss_region@8bc00000
//   (+0xe600000, 170-173), whose first 4 MiB RuanWifi.cpp leaves free (its DMA window starts at 0x8c000000).
// Shortcut: everything is polled (no IPCC/RSC interrupt attached), a look every kPollMs (1 ms while sends are queued). Fine for QMI, whose
// traffic is small; the data path does not use GLINK. Upgrade: attach SPI 229 (0xe5) with armcpu_spi_attach if QMI latency matters.
//
// Boot arguments (ruan-wifi=N, each stage includes the ones before):
//   1 survey: reads SMEM and the WPSS's SMP2P state, writes nothing          (USB `wifi`: st 2)
//   2 boot:   QMP load_state, cx/mx proxy votes, WL_EN, PAS 6, SMP2P ready, GLINK IPCRTR, the board's own QRTR router (pass: st 11, svc 45:x)
//   3 QMI:    the router link goes to the WiFi module (RuanWifi.cpp): QMI WLFW, board data, regdb, FW_READY, WMI ready; radio stays off
//   4 scan:   radio on, the board's self-test scan (networks in USB `wifi.log`)
//   5 net:    en0 (WifiNet.cpp), wpa_supplicant's control endpoint, DHCP by iOS
// ruan-wifi-delay=S waits S seconds after the start call before touching anything (default 20 from boot_progress_start, so IOKit and the
// UFS driver are up; the late-probe call starts at once). ruan-wifi-order=1 writes the segments before init_image, as the A710 zap needed
// (RuanAdrenoGlue.cpp gpu-zap=9); 0 uses Linux's order, which asteroids uses for the WPSS. ruan-wifi-meta=1 puts the PAS metadata in the
// first pages of mpss_region (0x8bc00000) instead of a contiguous kernel buffer.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);   // AdspFirmware.c (asteroids)
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
int kernel_thread_start(void (*)(void *, int), void *, thread_t *);
void thread_deallocate(thread_t);
extern volatile int32_t native_wpss_stats[16];   // WpssProbe.cpp
extern char native_wpss_text[96], native_wpss_services[160], native_wpss_crash[224], native_wpss_files[256];
extern volatile uint32_t native_wpss_wlen[4];
#if RUAN_WIFI_MODULE
void ruan_wifi_module_start(unsigned mode);   // RuanWifi.cpp (built only with RUAN_WIFI_MODULE=1)
#endif
extern volatile int32_t native_wlan_stats[16] __attribute__((weak_import));   // RuanWifi.cpp, when linked
extern char native_wlan_bss[224] __attribute__((weak_import));
extern char *native_wlan_lines __attribute__((weak_import));
extern volatile uint32_t native_wlan_lines_n __attribute__((weak_import));
}
#include "RuanZap.h"          // ruan_smc_call (the SMC with x0..x7 in and out)
#include "AsteroidsRemote.h"  // what WpssProbe.cpp needs from its host thread

namespace {
constexpr uint64_t kSmem = 0x80900000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0xed18000, kAoss = 0xc300000;
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x80860000, kCmdDbSize = 0x20000;
constexpr uint64_t kMetaPa = 0x8bc00000;   // mpss_region@8bc00000 (+0xe600000): UniXNU never starts the modem; below 4 GiB
constexpr uint64_t kWpssRegion = 0x85300000, kWpssRegionSize = 0x1900000;
constexpr unsigned kTcs = 1, kPollMs = 5;
// Without ruan-wifi: stages 1-4 (WPSS, QMI, radio on + scan; no en0). Each stage starts only after the one before passed, and a failed
// stage only records its step and code (USB `wifi`): the boot goes on. ruan-wifi=0 turns everything off.
constexpr unsigned kDefaultMode = 0;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}

Window gSmemW, gMutex, gIpcc, gAoss, gRsc;
QcomSmem gSmem;
QcomQmp gQmp;
QcomRscLayout gLayout;
uint8_t *gDb;        // cmd-db copy (128 KiB, heap: the kernel's __DATA is tight)
bool gDbRead;
int64_t gScmX0;

// ---- polling ----
void kickAop(void *) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP));}
// Drains the latched doorbells (no interrupt is attached); returns how many there were.
unsigned drainIpcc() {
    unsigned n = 0;
    for (; n < 32; ++n) {
        const uint32_t id = rd(gIpcc, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gIpcc, QCOM_IPCC_RECV_CLEAR, id);
    }
    return n;
}
void service() {
    drainIpcc();
    if (gQmp.state > QcomQmpIdle && gQmp.state != QcomQmpOpen) qcom_qmp_step(&gQmp);
    asteroids_wpss_service();
}

// ---- secure calls (PAS), resumed when interrupted, retried when busy (asteroids AdspProbe.cpp scm) ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c) {
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t session = 0, function = qcom_scm_fn(QCOM_SCM_SVC_PIL, command);
        for (unsigned resumes = 0;; ++resumes) {
            uint64_t r[8] = {function, arginfo, a, b, c, 0, session, 0};
            ruan_smc_call(r);
            result = {(int64_t)r[0], r[1]};
            if ((int64_t)r[0] != QCOM_SCM_INTERRUPTED || resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = r[6];
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    gScmX0 = result.x0;
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}
void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- the image ----
struct Image {void *file; uint64_t size; QcomMdtImage mdt; IOBufferMemoryDescriptor *meta; Window fixedMeta;};
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    if (image.meta) image.meta->release();
    unmap(image.fixedMeta);
    image.file = nullptr; image.meta = nullptr;
}
// The metadata (ELF and program headers + hash segment, ~5 KiB for ruan's wpss.mdt) in physically contiguous memory. Contiguous kernel
// allocations over one page fail on ruan (GPU-METAL-PORT.md, ctx e00002bd), so the fixed low pages are the fallback (the zap's metadata
// worked from a reserved low page too, 0x80D00000).
int initImage(Image &image, uint32_t pas, unsigned metaMode) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    uint64_t phys = 0;
    if (!metaMode) {
        image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut |
            (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes, 0x0000fffffffff000ull);
        if (image.meta && image.meta->prepare() == kIOReturnSuccess) {
            uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
            if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
            cleanToPoc(va, bytes);
            phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        } else if (image.meta) {
            image.meta->release();
            image.meta = nullptr;
        }
    }
    if (!phys) {
        const uint64_t span = (bytes + 0xfff) & ~0xfffull;
        uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(bytes));
        if (!bounce || !mapWindow(image.fixedMeta, kMetaPa, span)) {if (bounce) IOFreeData(bounce, bytes); return -12;}
        const int rc = qcom_mdt_metadata(&image.mdt, fileRead, image.file, bounce, bytes);
        for (uint64_t i = 0; !rc && i < bytes; ++i) image.fixedMeta.va[i] = bounce[i];   // byte stores: Device memory
        IOFreeData(bounce, bytes);
        if (rc) return -5;
        __asm__ volatile("dsb sy" ::: "memory");
        phys = kMetaPa;
    }
    return scmOk(scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0)) ? 0 : -1;
}
// Each load segment at its offset in the carveout, the rest of its memory size zeroed. Byte stores through a write-combined map: on
// a kernel where that map is Device memory a wider unaligned store would fault.
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    Window region = {};
    if (!mapWindow(region, base, size, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        volatile uint8_t *to = region.va + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            for (size_t k = 0; k < n; ++k) to[done + k] = bounce[k];
            done += n;
        }
        for (uint64_t k = s.file_size; !rc && k < s.memory_size; ++k) to[k] = 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}

// ---- RPMh: one active request for two ARC resources on TCS 1, completion polled (RuanTouch.c: the status bit follows the enable) ----
int voteResources(const QcomRpmhResource &ra, const QcomRpmhResource &rb, uint32_t va, uint32_t vb) {
    wr(gRsc, kRscTcs + 0, rd(gRsc, kRscTcs + 0) | 1u << kTcs);
    const uint32_t addresses[2] = {ra.address, rb.address}, values[2] = {va, vb};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&gLayout, kTcs, addresses, values, 2, writes, 16);
    if (!n) return -22;
    for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    for (unsigned t = 0; t < 100 && !(rd(gRsc, kRscTcs + gLayout.irq_status) & 1u << kTcs); ++t) IOSleep(1);
    const unsigned m = qcom_rpmh_finish(&gLayout, kTcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    return 0;
}
bool readCmdDb() {
    if (gDbRead) return true;
    Window db = {};
    if (!gDb) gDb = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!gDb || !mapWindow(db, kCmdDb, kCmdDbSize)) return false;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(gDb + i, &w, 4);}
    unmap(db);
    return gDbRead = true;
}

void wifiThread(void *context, int) {
    unsigned mode = kDefaultMode, delay = (unsigned)(uintptr_t)context;
    PE_parse_boot_argn("ruan-wifi", &mode, sizeof mode);
    PE_parse_boot_argn("ruan-wifi-delay", &delay, sizeof delay);
    if (delay) IOSleep(delay * 1000);   // before any IOKit mapping: boot_progress_start runs before IOKit is up
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) || !mapWindow(gIpcc, kIpcc, 0x1000) ||
        !mapWindow(gAoss, kAoss, 0x1000) || !mapWindow(gRsc, kRscDrv, 0x10000)) {
        native_wpss_stats[0] = -100;
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    gLayout = qcom_rsc_layout(rd(gRsc, 0));
    if (mode >= 2) {
        qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
        for (int phase = 0; phase < 4 && qcom_qmp_step(&gQmp) > 0 && gQmp.state != QcomQmpOpen; ++phase)
            asteroids_remote::waitUntil(1000, [](void *) {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;}, nullptr);
    }
    asteroids_wpss_run();   // 1: survey; 2+: QMP load_state, rails, PAS 6, SMP2P ready, GLINK IPCRTR (WpssProbe.cpp)
#if RUAN_WIFI_MODULE
    if (mode >= 3 && native_wpss_stats[0] >= 7) ruan_wifi_module_start(mode);   // RuanWifi.cpp: its own thread, attaches to the router link
#endif
    while (native_wpss_stats[0] >= 7) {   // StReady or later: keep the link served
        service();
        IOSleep(asteroids_wpss_pending() ? 1 : kPollMs);
    }
}
}  // namespace

namespace asteroids_remote {
void ring(uint32_t client, uint32_t signal) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(client, signal));}
void enableDoorbell(uint32_t client, uint32_t signal) {wr(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(client, signal));}
bool waitUntil(uint32_t ms, bool (*done)(void *), void *context) {
    for (uint32_t waited = 0; !done(context); waited += kPollMs) {
        if (waited >= ms) return done(context);
        service();
        IOSleep(kPollMs);
    }
    return true;
}
const QcomSmem &smem() {return gSmem;}
int allocItem(const QcomSmemPartition &part, uint16_t id, uint32_t size) {
    bool locked = false;
    for (unsigned i = 0; i < 1000 && !locked; ++i) {wr(gMutex, 0, 1); locked = rd(gMutex, 0) == 1; if (!locked) IODelay(10);}
    if (!locked) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &part, id, size);
    wr(gMutex, 0, 0);
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(const QcomSmemPartition &part, uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&gSmem, &part, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}
int qmp(const char *message) {
    if (gQmp.state != QcomQmpOpen) return -19;
    const int rc = qcom_qmp_send(&gQmp, message);
    if (rc < 0) return rc;
    if (!waitUntil(1000, [](void *) {return qcom_qmp_step(&gQmp) == QcomQmpOpen;}, nullptr)) {qcom_qmp_abandon(&gQmp); return -60;}
    return 0;
}
int voteRails(const char *a, const char *b, uint32_t level_a, uint32_t level_b) {
    QcomRpmhResource ra, rb;
    uint32_t ia = 0, ib = 0;
    if (!readCmdDb()) return -19;
    if (qcom_cmd_db_find(gDb, kCmdDbSize, a, &ra) || qcom_cmd_db_find(gDb, kCmdDbSize, b, &rb)) return -2;
    if (qcom_rpmh_level_index(&ra, level_a, &ia) || qcom_rpmh_level_index(&rb, level_b, &ib)) return -22;
    return voteResources(ra, rb, ia, ib);
}
int pasBoot(const char *path, uint32_t pas, uint64_t base, uint64_t size, int32_t *kib, int32_t *scm_x0) {
    static Image image;   // the segment table is a few KiB
    unsigned order = 0, metaMode = 0;
    PE_parse_boot_argn("ruan-wifi-order", &order, sizeof order);
    PE_parse_boot_argn("ruan-wifi-meta", &metaMode, sizeof metaMode);
    if (base != kWpssRegion || size != kWpssRegionSize) return -5;   // only the carveout the DT gives the WPSS
    bzero(&image, sizeof image);
    if (sheng_fw_open(path, &image.file, &image.size)) {image.file = nullptr; return -1;}
    if (kib) *kib = (int32_t)(image.size >> 10);
    if (qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file)) {release(image); return -2;}
    int rc = 0;
    bool inited = false;
    if (order == 1 && loadSegments(image, base, size)) rc = -5;
    if (!rc && initImage(image, pas, metaMode)) rc = -3;
    inited = !rc;
    if (!rc && image.mdt.relocatable &&
        !scmOk(scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base, qcom_mdt_span(&image.mdt))))
        rc = -4;
    if (!rc && order != 1 && loadSegments(image, base, size)) rc = -5;
    if (!rc && !scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), pas, 0, 0))) rc = -6;
    if (scm_x0) *scm_x0 = (int32_t)gScmX0;
    if (rc && inited) pasShutdown(pas);   // after init_image the monitor holds the image: let it go
    release(image);
    return rc;
}
void pasShutdown(uint32_t pas) {scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), pas, 0, 0);}
void wakeWpss() {}   // polled: the loop looks every kPollMs (1 ms while sends are queued)
}  // namespace asteroids_remote

// ruan has no known sleep-clock GPIO for the WCN (the live DT has no pmk8350 pinctrl state for it): nothing to do. Weak, so a stage-2
// AsteroidsWifi port with a real one replaces it.
extern "C" __attribute__((weak)) int asteroids_wifi_sleep_clock(void) {return 0;}

// Started from two places, whichever comes first, once: boot_progress_start (BootProgress.c, a strong reference, which also pulls this file
// out of libRamRootProbe.a) after ruan-wifi-delay, and the late probe (LegacyDisplayProbe.cpp's existing weak ruan_wifi_init call, after
// IOKitBSDInit) at once. ruan-wifi=0: nothing.
namespace {
volatile uint32_t gStarted;
void startOnce(unsigned delay) {
    unsigned mode = kDefaultMode;
    thread_t thread;
    PE_parse_boot_argn("ruan-wifi", &mode, sizeof mode);
    if (!mode) return;
    if (!__sync_bool_compare_and_swap(&gStarted, 0u, 1u)) return;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(delay)), &thread) == 0) thread_deallocate(thread);
    else native_wpss_stats[0] = -101;
}
}  // namespace
extern "C" void ruan_wifi_start(void) {startOnce(20);}
extern "C" void ruan_wifi_init(void) {startOnce(0);}

// USB `wifi` (UsbDebugCdc.cpp's existing command): one line. WPSS: stage/error, slave-kernel bits, items, image KiB, ms to ready, GLINK
// in/out, IPCRTR state, QRTR packets, WPSS node, services, WLFW instance, last SCM x0, WL_EN pin before/after; module (ruan-wifi>=3):
// stage/error, interrupts, QRTR in/out, DMA KiB, firmware files served|refused<<16, SMMU result, networks in the last scan.
extern "C" const char *ruan_wifi_status_str(void) {
    static char line[400];
    const volatile int32_t *s = native_wpss_stats;
    if (!gStarted) return "not started (ruan-wifi=0, or before the start hooks)";
    int n = snprintf(line, sizeof line, "wpss st %d err %d slave %x items %x kib %d ready %dms glink %d/%d ch %x qrtr %d node %d svc %d wlfw %d"
                     " scm %d wlen %x>%x", s[0], s[1], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[11], s[12], s[13], s[14], s[10],
                     native_wpss_wlen[0], native_wpss_wlen[1]);
    if (&native_wlan_stats && native_wlan_stats[0] && n > 0 && (size_t)n < sizeof line) {
        const volatile int32_t *w = native_wlan_stats;
        snprintf(line + n, sizeof line - n, " | wlan st %d err %d irq %d qrtr %d/%d dma %dK fw %x smmu %x nets %d", w[0], w[1], w[2], w[4], w[5],
                 w[6], w[8], w[9], w[12]);
    }
    return line;
}

// USB `wifi.log`: line i of the report, 0 when there is none. 0-3: the WPSS note, the services it announced, its crash reason, the
// firmware partition's WiFi files; 4: the networks of the last scan; then the module's log ring, oldest first (RuanWifi.cpp: 256 x 112).
extern "C" unsigned ruan_wifi_log(unsigned i, char *out, unsigned len) {
    const char *fixed[5] = {native_wpss_text, native_wpss_services, native_wpss_crash, native_wpss_files,
                            &native_wlan_bss ? native_wlan_bss : ""};
    static const char *const names[5] = {"note", "services", "crash", "files", "networks"};
    if (i < 5) return (unsigned)snprintf(out, len, "[usb] WIFI.%s: %s\r\n", names[i], fixed[i]);
    if (!&native_wlan_lines || !native_wlan_lines) return 0;
    constexpr unsigned kLines = 256, kLineBytes = 112;   // RuanWifi.cpp's ring
    const uint32_t total = native_wlan_lines_n, first = total > kLines ? total - kLines : 0;
    if (first + (i - 5) >= total) return 0;
    const char *row = native_wlan_lines + ((first + (i - 5)) % kLines) * kLineBytes;
    return (unsigned)snprintf(out, len, "[usb] WIFI.LOG: %.*s\r\n", (int)kLineBytes, row);
}
