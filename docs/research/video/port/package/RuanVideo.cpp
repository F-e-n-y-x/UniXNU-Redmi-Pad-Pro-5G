// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan-video=N: the iris2 VPU of the Redmi Pad Pro 5G (SM7435 "parrot") for the QcomIris driver. NOT BUILT, NOT BOOTED.
// (macOS/docs/research/video/port; ported from UniXNU/UniXNU-sheng 2359a86 package/VideoProbe.cpp, BSD-2-Clause-Patent.)
//
// What sheng's board file gives the iris core, with ruan's values (live DT macOS/hardware/private/ruan-live.dts, line):
//   qcom,vidc@aa00000 (17509): reg 0xaa00000 + 0xf0000, SPI 0xae (174) level, pas-id 9, vidc,firmware-name "vpu20_1v",
//   qcom,reg-presets <0xb0088 0 0x11>, compatible msm-vidc-parrot / msm-vidc-iris2 (one pipe: VPU_VERSION_IRIS2_1PIPE).
//   memory-region video_region@86c00000 + 0x700000 (134). SMMU apps-smmu@15000000 (phandle 0x28): venus_ns SID 0x2180 mask 0x20 (so
//   0x2180 and 0x21a0) and venus_ns_pixel 0x2187, IOVA pool 0x25800000 + 0xba800000 (= QcomIris's window). Secure banks 0x2181/0x2183/
//   0x2184 (vmid 9/10/11) belong to TZ and are never touched.
//   Supplies: iris-ctl = video_cc_mvsc_gdsc @0xaaf5004, vcodec = video_cc_mvs0_gdsc reg 0xaaf6004 (12264-12311); both on CX (0x11c).
//   Clocks (videocc 0x12e ids 6 3 5 4 10 2): MVSC_CTL_AXI, MVS0_AXI, MVSC_CORE, MVS0_CORE, VENUS_AHB, IRIS_CLK_SRC (RuanVideoCc.c).
//   RPMh: cx.lvl + mx.lvl (vdd_cx / vdd_mxa) at the rate's corner, and BCM MM1 (enable mask 1: qnm_video0, icc parrot.c).
//   UBWC: ubwc_config_parrot with hbb 14 (LPDDR4X: /memory ddr_device_type 7, line 23) -> build with -DQCOM_IRIS_UBWC_HBB=14.
//
// Boot arguments (each stage includes the ones before; a failed stage records its step and code, the boot goes on):
//   ruan-video=0     nothing (default)
//   ruan-video=1     survey, writes nothing: videocc/GCC/GDSC/PLL state, the VPU's registers only when its domain and clocks are already
//                    on, the apps SMMU's free bank/ASID/slots for the three SIDs, cmd-db (cx/mx/MM1), the firmware file's headers
//   ruan-video=2     power: RPMh votes, PLL0 and the iris root at ruan-video-mhz, MVSC + MVS0 on, VPU wrapper read, all off again
//   ruan-video=3     firmware: the VPU's S1 domain, the UC region, SPI 174, PAS 9 (vpu20_1v) + the TZ content-protection call, CTRL_INIT,
//                    HFI gen2 system init; pass = the firmware's version string (USB `video`), then autosuspend
//   ruan-video=4     session: one H.264 decoder session opened, its 1280x720 format set, closed (B3's first step)
//   ruan-video=5     the VideoToolbox host (qcom_iris_video_publish, B4; weak, a no-op until VtHost is linked)
//   ruan-video-delay=S   seconds after boot_progress_start before anything is mapped (default 30; the late probe starts at once)
//   ruan-video-mhz=M     133, 240 (default), 335, 424 or 460: the iris root (ftbl_video_cc_iris_clk_src); the rails follow
//   ruan-video-fw=PATH   the image, as sheng_fw_open takes it (default /private/var/firmware/qcom/vpu20_1v.mbn, scripts/stage-video-firmware.py)
//   ruan-video-tcs=T     the apps RSC active TCS for the votes (default 1: RuanWifiWpss.cpp's, idle once the WPSS is up)
//   ruan-video-order=1   the image's segments written before init_image (default; the A710 zap needed it under Gunyah), 0 Linux's order
//   log-video=1          the firmware's error/fatal/high debug messages (USB `video.log`)
// Shortcuts: no apps-SMMU lock and no RSC arbitration with RuanWifi.cpp / RuanTouch.c: video starts ruan-video-delay later, after the
// WPSS boot; upgrade to a shared lock if both ever program at once. The votes are made at every power-on (RuanWifiWpss drops cx/mx to 0
// on the same DRV after its handover) and never lowered.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "QcomIrisDevice.h"
extern "C" {
#include "QcomMdt.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "RuanVideoCc.h"
#include "Sm8550AppsSmmu.h"
#include "ArmSmmu4KLive.h"
#include "QcomIrisMmu.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);   // AdspFirmware.c (asteroids, in libRamRootProbe.a)
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
int armcpu_spi_mask(uint32_t, bool) __attribute__((weak_import));
void qcom_iris_video_publish(void) __attribute__((weak_import));     // B4: VtHost.cpp / VtParavirt.cpp
// [0] stage (negative: the stage that failed), [1] its error, [2] bank | asid << 8 | output bits << 24, [3] interrupts, [4] thread passes,
// [5] core state, [6] last system error, [7] recoveries, [8] suspends, [9] mvsc GDSCR, [10] firmware load result, [11] videocc failed
// step | value << 8, [12] SCM x0 of the last failed call, [13] its x1, [14] RPMh resources found (bit 0 cx, 1 mx, 2 MM1), [15] the last
// table status, [16] bytes mapped / 4 KiB, [17] live buffers, [18] mvs0 GDSCR, [19] PLL0 mode, [20] VPU WRAPPER_CORE_POWER_STATUS,
// [21] VPU CTRL_STATUS, [22] firmware KiB, [23] its span / 4 KiB, [24] free SMR slots, [25] session result.
volatile int32_t native_video_stats[26];
char native_video_text[128];
char native_video_fw_text[4][100];
volatile uint32_t native_video_fw_lines;
}
#include "RuanZap.h"   // ruan_smc_call

namespace {
constexpr uint64_t kVpu = QCOM_IRIS_BASE, kVpuSize = QCOM_IRIS_SIZE, kRsc = 0x17a20000, kRscTcs = 0xd00;
constexpr uint64_t kCmdDb = 0x80860000, kCmdDbSize = 0x20000;          // aop_cmd_db_region (67-70)
constexpr uint64_t kCarveout = 0x86c00000, kCarveoutSize = 0x700000;   // video_region (134)
constexpr uint64_t kMetaPa = 0x8bd00000;   // mpss_region + 1 MiB: the PAS metadata fallback (RuanWifiWpss uses its first pages)
constexpr uint32_t kIntid = 32 + QCOM_IRIS_SPI, kPas = QCOM_IRIS_PAS_ID;
constexpr uint16_t kSids[3] = {0x2180, 0x21a0, 0x2187};
constexpr unsigned kPollBudget = 1000, kPipes = 1;
constexpr uint32_t kWrapperCorePower = 0xb0080, kCtrlStatus = 0xa004c;
const char kDefaultFirmware[] = "/private/var/firmware/qcom/vpu20_1v.mbn";

enum Stage {StSurvey = 1, StPower, StMapped, StDomain, StQueues, StInterrupt, StThreads, StFirmware, StSession, StPublished};
enum Fail {FailMap = 1, FailCmdDb, FailFirmwareFile, FailClocks, FailVote, FailDomain, FailQueues, FailInterrupt, FailThreads, FailCore,
           FailSession, FailBusy};

struct Window {IOMemoryMap *map; volatile uint8_t *va; uint64_t size;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = size;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; barrier();}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void cleanToPoc(const volatile void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    barrier();
}
void fail(int stage, int error) {native_video_stats[1] = error; native_video_stats[0] = -stage;}

// ---- table pages of the VPU domain: one wired page at a time, sorted by physical address (sheng's, unchanged) ----
struct TablePages {
    struct Page {uint64_t physical; volatile uint64_t *mapping; IOBufferMemoryDescriptor *memory;};
    Page *pages = nullptr;
    size_t count = 0, capacity = 0;
    uint64_t limit = 0;
    ArmSmmu4KTablePages provider = {};
    size_t lowerBound(uint64_t physical) const
    {
        size_t low = 0, high = count;
        while (low < high) {const size_t mid = low + (high - low) / 2; if (pages[mid].physical < physical) low = mid + 1; else high = mid;}
        return low;
    }
    static int allocate(void *cookie, uint64_t *physical, volatile uint64_t **mapping)
    {
        auto *self = static_cast<TablePages *>(cookie);
        if (self->count == self->capacity) {
            const size_t size = self->capacity ? self->capacity * 2 : 64;
            Page *made = IONewData(Page, size);
            if (!made) return 1;
            if (self->count) memcpy(made, self->pages, sizeof(Page) * self->count);
            if (self->pages) IODeleteData(self->pages, Page, self->capacity);
            self->pages = made; self->capacity = size;
        }
        IOBufferMemoryDescriptor *page = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, (self->limit - 1) & ~UINT64_C(4095));
        if (!page) return 1;
        if (page->prepare() != kIOReturnSuccess) {page->release(); return 1;}
        IOByteCount length = 0;
        const addr64_t at = page->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
        void *virt = page->getBytesNoCopy();
        if (!at || (at & 4095) || at >= self->limit || length < 4096 || !virt) {page->complete(); page->release(); return 1;}
        memset(virt, 0, 4096);
        cleanToPoc(virt, 4096);
        const size_t i = self->lowerBound(at);
        memmove(self->pages + i + 1, self->pages + i, sizeof(Page) * (self->count - i));
        self->pages[i] = {at, static_cast<volatile uint64_t *>(virt), page};
        ++self->count;
        *physical = at; *mapping = static_cast<volatile uint64_t *>(virt);
        return 0;
    }
    static volatile uint64_t *find(void *cookie, uint64_t physical)
    {
        auto *self = static_cast<TablePages *>(cookie);
        const size_t i = self->lowerBound(physical);
        return i < self->count && self->pages[i].physical == physical ? self->pages[i].mapping : nullptr;
    }
    static int overlaps(void *cookie, uint64_t physical, uint64_t bytes)
    {
        auto *self = static_cast<TablePages *>(cookie);
        if (!bytes || !self->count) return 0;
        const size_t i = self->lowerBound(physical > 4095 ? physical - 4095 : 0);
        return i < self->count && self->pages[i].physical < physical + bytes;
    }
};
int publishTable(void *, volatile uint64_t *page, size_t bytes) {cleanToPoc(page, bytes); return 0;}

struct Video {
    Window vpu, gcc, videocc, smmu, rsc;
    IOLock *lock;
    struct qcom_iris_core core;
    struct qcom_iris_vpu_io vpuIo;
    struct ruan_video_cc_io ccIo;
    struct ruan_video_cc_state cc;
    struct sm8550_apps_smmu_domain domain;
    struct qcom_iris_mmu mmu;
    TablePages tables;
    QcomIrisBuffer uc;
    uint8_t *db;
    QcomRscLayout layout;
    uint32_t hz, tcs;
    bool ready, initSignalled;
    IOSimpleLock *irqLock;
    unsigned irqDepth;
    volatile uint32_t irqPending;
    uint64_t suspendAt, recoveryAt;
    uint32_t workPending;
} v;

// ---- register access ----
uint32_t vpuRead(void *, uint32_t offset) {return rd(v.vpu, offset);}
void vpuWrite(void *, uint32_t offset, uint32_t value) {wr(v.vpu, offset, value);}
void delayUs(void *, unsigned us) {IODelay(us);}
uint32_t ccRead(void *, enum ruan_video_cc_window w, uint32_t offset) {return rd(w == RuanVideoGcc ? v.gcc : v.videocc, offset);}
void ccWrite(void *, enum ruan_video_cc_window w, uint32_t offset, uint32_t value) {wr(w == RuanVideoGcc ? v.gcc : v.videocc, offset, value);}
uint32_t smmuRead(void *, uint32_t offset) {return rd(v.smmu, offset);}
void smmuWrite(void *, uint32_t offset, uint32_t value) {wr(v.smmu, offset, value);}
// Gunyah traps the apps SMMU: plain 32/64-bit loads and stores (no pair or writeback forms), as RuanWifi.cpp does.
int kitRead32(void *, size_t at, uint32_t *value) {if (at + 4 > v.smmu.size) return 1; *value = rd(v.smmu, at); return 0;}
int kitRead64(void *, size_t at, uint64_t *value)
{
    if (at + 8 > v.smmu.size || (at & 7)) return 1;
    *value = *reinterpret_cast<volatile uint64_t *>(v.smmu.va + at);
    return 0;
}
int kitWrite32(void *, size_t at, uint32_t value) {if (at + 4 > v.smmu.size) return 1; wr(v.smmu, at, value); return 0;}
int kitWrite64(void *, size_t at, uint64_t value)
{
    if (at + 8 > v.smmu.size || (at & 7)) return 1;
    *reinterpret_cast<volatile uint64_t *>(v.smmu.va + at) = value;
    barrier();
    return 0;
}
int kitBarrier(void *) {barrier(); return 0;}

// ---- secure calls: resumed when interrupted, retried when busy (RuanWifiWpss.cpp's, any service) ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t svc, uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t session = 0, function = qcom_scm_fn(svc, command);
        for (unsigned resumes = 0;; ++resumes) {
            uint64_t r[8] = {function, arginfo, a, b, c, d, session, 0};
            ruan_smc_call(r);
            result = {(int64_t)r[0], r[1]};
            if ((int64_t)r[0] != QCOM_SCM_INTERRUPTED || resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = r[6];
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    if (result.x0 || result.x1) {native_video_stats[12] = (int32_t)result.x0; native_video_stats[13] = (int32_t)result.x1;}
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}
constexpr uint32_t kSvcBoot = 0x01, kBootSetRemoteState = 0x0a, kSvcMp = 0x0c, kMpVideoVar = 0x08;
void pasShutdown() {scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPas, 0, 0, 0);}

// ---- RPMh: cx.lvl + mx.lvl at the rate's corner and MM1 enabled, on the chosen active TCS, completion polled ----
bool readCmdDb()
{
    if (v.db) return true;
    Window db = {};
    uint8_t *copy = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!copy || !mapWindow(db, kCmdDb, kCmdDbSize)) {if (copy) IOFreeData(copy, kCmdDbSize); return false;}
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);}
    unmap(db);
    v.db = copy;
    return true;
}
bool findResources(QcomRpmhResource &cx, QcomRpmhResource &mx, QcomRpmhResource &mm1)
{
    native_video_stats[14] = 0;
    if (!qcom_cmd_db_find(v.db, kCmdDbSize, "cx.lvl", &cx)) native_video_stats[14] |= 1;
    if (!qcom_cmd_db_find(v.db, kCmdDbSize, "mx.lvl", &mx)) native_video_stats[14] |= 2;
    if (!qcom_cmd_db_find(v.db, kCmdDbSize, "MM1", &mm1)) native_video_stats[14] |= 4;
    return native_video_stats[14] == 7;
}
void request(const uint32_t *addresses, const uint32_t *values, unsigned count)
{
    wr(v.rsc, kRscTcs + 0, rd(v.rsc, kRscTcs + 0) | 1u << v.tcs);
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&v.layout, v.tcs, addresses, values, count, writes, 16);
    for (unsigned i = 0; i < n; ++i) wr(v.rsc, kRscTcs + writes[i].offset, writes[i].value);
    for (unsigned t = 0; t < 100 && !(rd(v.rsc, kRscTcs + v.layout.irq_status) & 1u << v.tcs); ++t) IOSleep(1);
    const unsigned m = qcom_rpmh_finish(&v.layout, v.tcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr(v.rsc, kRscTcs + writes[i].offset, writes[i].value);
}
bool vote()
{
    QcomRpmhResource cx, mx, mm1;
    uint32_t cxIndex = 0, mxIndex = 0;
    const uint32_t level = ruan_video_cc_rail_level(v.hz);
    if (!readCmdDb() || (!v.rsc.va && !mapWindow(v.rsc, kRsc, 0x10000)) || !findResources(cx, mx, mm1)) return false;
    if (qcom_rpmh_level_index(&cx, level, &cxIndex) || qcom_rpmh_level_index(&mx, level, &mxIndex)) return false;
    v.layout = qcom_rsc_layout(rd(v.rsc, 0));
    const uint32_t rails[2] = {mx.address, cx.address}, levels[2] = {mxIndex, cxIndex};   // MX at least CX: MX first going up
    request(rails, levels, 2);
    const uint32_t bus = mm1.address, enable = 1u << 30 | 1u << 29 | 1u;   // BCM_TCS_CMD(commit, valid, vote_x 0, vote_y 1)
    request(&bus, &enable, 1);
    return true;
}

// ---- the core's platform ops ----
void noteCc() {native_video_stats[11] = (int32_t)(v.cc.failed_step | v.cc.failed_value << 8);}
int powerOnController(void *)
{
    if (!vote()) return -EIO;
    if (!ruan_video_cc_configure(&v.ccIo, &v.cc, v.hz) || !ruan_video_cc_controller_on(&v.ccIo, &v.cc)) {noteCc(); return -EIO;}
    return 0;
}
int powerOnHardware(void *) {if (ruan_video_cc_hardware_on(&v.ccIo, &v.cc)) return 0; noteCc(); return -EIO;}
int setHwmode(void *) {return ruan_video_cc_hardware_mode(&v.ccIo, &v.cc, true) ? 0 : -EIO;}
void powerOffHardware(void *) {ruan_video_cc_hardware_off(&v.ccIo, &v.cc);}
void powerOffController(void *) {ruan_video_cc_controller_off(&v.ccIo, &v.cc);}

const char *firmwarePath()
{
    static char path[128];
    if (!PE_parse_boot_arg_str("ruan-video-fw", path, sizeof path) || !path[0]) strlcpy(path, kDefaultFirmware, sizeof path);
    return path;
}
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
// The image's headers: the span must fit the carveout (stage 1 checks this without any secure call).
int openImage(void **file, QcomMdtImage *image)
{
    uint64_t size = 0;
    int rc = 0;
    for (unsigned tries = 0; tries < 30; ++tries) {   // the Data volume may mount after the start
        if (!(rc = sheng_fw_open(firmwarePath(), file, &size))) break;
        IOSleep(2000);
    }
    if (rc) {*file = nullptr; return -ENOENT;}
    native_video_stats[22] = (int32_t)(size >> 10);
    if (qcom_mdt_parse(image, size, fileRead, *file)) return -EINVAL;
    native_video_stats[23] = (int32_t)(qcom_mdt_span(image) >> 12);
    return qcom_mdt_span(image) > kCarveoutSize ? -EFBIG : 0;
}
// Each load segment at its offset in the carveout, the rest zeroed; byte stores through a write-combined map (RuanWifiWpss.cpp).
int loadSegments(void *file, const QcomMdtImage *image)
{
    Window region = {};
    if (!mapWindow(region, kCarveout, kCarveoutSize, kIOMapWriteCombineCache)) return -ENOMEM;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -ENOMEM;
    for (unsigned i = 0; !rc && i < image->count; ++i) {
        const QcomMdtSegment &s = image->segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(image, i, kCarveout, kCarveoutSize, &at)) {rc = -EINVAL; break;}
        volatile uint8_t *to = region.va + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(file, s.file_offset + done, bounce, n)) {rc = -EIO; break;}
            for (size_t k = 0; k < n; ++k) to[done + k] = bounce[k];
            done += n;
        }
        for (uint64_t k = s.file_size; !rc && k < s.memory_size; ++k) to[k] = 0;
    }
    barrier();
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}
// The metadata in contiguous memory, else in the fixed low pages (contiguous allocations over a page fail on ruan, RuanWifiWpss.cpp).
int initImage(void *file, const QcomMdtImage *image, IOBufferMemoryDescriptor **meta, Window &fixed)
{
    const uint64_t bytes = qcom_mdt_metadata_size(image);
    uint64_t phys = 0;
    *meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut |
        (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes, 0x0000fffffffff000ull);
    if (*meta && (*meta)->prepare() == kIOReturnSuccess) {
        uint8_t *va = static_cast<uint8_t *>((*meta)->getBytesNoCopy());
        if (qcom_mdt_metadata(image, fileRead, file, va, bytes)) return -EIO;
        cleanToPoc(va, bytes);
        phys = (*meta)->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    } else if (*meta) {
        (*meta)->release();
        *meta = nullptr;
    }
    if (!phys) {
        uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(bytes));
        if (!bounce || !mapWindow(fixed, kMetaPa, (bytes + 0xfff) & ~0xfffull)) {if (bounce) IOFreeData(bounce, bytes); return -ENOMEM;}
        const int rc = qcom_mdt_metadata(image, fileRead, file, bounce, bytes);
        for (uint64_t i = 0; !rc && i < bytes; ++i) fixed.va[i] = bounce[i];
        IOFreeData(bounce, bytes);
        if (rc) return -EIO;
        barrier();
        phys = kMetaPa;
    }
    return scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), kPas, phys, 0, 0))
        ? 0 : -EIO;
}
// iris_fw_load: metadata, memory setup, segments, authentication and reset, then the content-protection ranges (tz_cp_config: the
// DT's pools, cp 0 + 0x25800000, non-pixel 0x1000000 + 0x24800000, the same numbers as sheng's).
int loadFirmwareImage()
{
    unsigned order = 1;
    PE_parse_boot_argn("ruan-video-order", &order, sizeof order);
    void *file = nullptr;
    QcomMdtImage *image = IOMallocType(QcomMdtImage);
    IOBufferMemoryDescriptor *meta = nullptr;
    Window fixed = {};
    int rc = openImage(&file, image);
    bool initialized = false;
    if (!rc && order == 1) rc = loadSegments(file, image);
    if (!rc) rc = initImage(file, image, &meta, fixed);
    initialized = !rc;
    if (!rc && image->relocatable &&
        !scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), kPas, kCarveout, qcom_mdt_span(image), 0)))
        rc = -EIO;
    if (!rc && order != 1) rc = loadSegments(file, image);
    if (!rc && !scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPas, 0, 0, 0))) rc = -EACCES;
    if (!rc && !scmOk(scm(kSvcMp, kMpVideoVar, qcom_scm_args(4, 0, 0, 0), 0, 0x25800000, 0x01000000, 0x24800000))) rc = -EPERM;
    if (rc && initialized) pasShutdown();
    if (meta) {meta->complete(); meta->release();}
    unmap(fixed);
    IOFreeType(image, QcomMdtImage);
    if (file) sheng_fw_close(file);
    return rc;
}
int loadFirmware(void *) {const int rc = loadFirmwareImage(); native_video_stats[10] = rc; return rc;}
void unloadFirmware(void *) {pasShutdown();}
int setRemoteState(void *, bool resume)
{
    return scmOk(scm(kSvcBoot, kBootSetRemoteState, qcom_scm_args(2, 0, 0, 0), resume ? 1 : 0, 0, 0, 0)) ? 0 : -EIO;
}

// The line's disable depth, as enable_irq / disable_irq count it: masked while it is above zero.
void irqDepth(int change)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(v.irqLock);
    const unsigned before = v.irqDepth;
    v.irqDepth = change > 0 ? before + 1 : before ? before - 1 : 0;
    if (!before && v.irqDepth) armcpu_spi_mask(kIntid, true);
    else if (before && !v.irqDepth) armcpu_spi_mask(kIntid, false);
    IOSimpleLockUnlockEnableInterrupt(v.irqLock, state);
}
void enableIrq(void *) {irqDepth(-1);}
void disableIrq(void *) {irqDepth(1);}
void primary(void *)
{
    irqDepth(1);
    native_video_stats[3] = native_video_stats[3] + 1;
    __atomic_store_n(&v.irqPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&v.irqPending);
}
bool waitInit(void *, unsigned ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!v.initSignalled && mach_absolute_time() < deadline) IOLockSleepDeadline(v.lock, &v.initSignalled, deadline, THREAD_UNINT);
    const bool signalled = v.initSignalled;
    v.initSignalled = false;
    return signalled;
}
void signalInit(void *) {v.initSignalled = true; IOLockWakeup(v.lock, &v.initSignalled, false);}
void scheduleWork(uint64_t *slot, unsigned ms)
{
    *slot = nowMs() + ms;
    if (!*slot) *slot = 1;
    __atomic_store_n(&v.workPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&v.workPending);
}
void scheduleRecovery(void *, unsigned ms) {scheduleWork(&v.recoveryAt, ms);}
void scheduleSuspend(void *, unsigned ms) {scheduleWork(&v.suspendAt, ms);}
uint64_t nowOp(void *) {return nowMs();}
void logText(void *, const char *text)
{
    if (!strncmp(text, "video-firmware", 14) || !strncmp(text, "vfw", 3)) strlcpy(native_video_text, text, sizeof native_video_text);
    else {
        const uint32_t n = native_video_fw_lines;
        strlcpy(native_video_fw_text[n & 3], text, sizeof native_video_fw_text[0]);
        native_video_fw_lines = n + 1;
        IOLog("[video] %s\n", text);
    }
}
const struct qcom_iris_core_ops kOps = {nullptr, powerOnController, powerOnHardware, setHwmode, powerOffHardware, powerOffController,
    loadFirmware, unloadFirmware, setRemoteState, enableIrq, disableIrq, waitInit, signalInit, scheduleRecovery, scheduleSuspend, nowOp,
    logText};

void note()
{
    native_video_stats[5] = (int32_t)v.core.state;
    native_video_stats[6] = (int32_t)v.core.last_error;
    native_video_stats[7] = (int32_t)v.core.recoveries;
    native_video_stats[15] = (int32_t)v.mmu.last_status;
    native_video_stats[16] = (int32_t)(v.mmu.iova.allocated / 4096);
}

void irqThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "ruan-video-irq");
    for (;;) {
        assert_wait((event_t)&v.irqPending, THREAD_UNINT);
        if (__atomic_exchange_n(&v.irqPending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
        else {thread_block(THREAD_CONTINUE_NULL); __atomic_store_n(&v.irqPending, 0u, __ATOMIC_RELEASE);}
        IOLockLock(v.lock);
        const bool enable = qcom_iris_core_thread(&v.core);
        native_video_stats[4] = native_video_stats[4] + 1;
        note();
        IOLockUnlock(v.lock);
        if (enable) irqDepth(-1);
    }
}
void workThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "ruan-video-work");
    for (;;) {
        IOLockLock(v.lock);
        if (v.recoveryAt && nowMs() >= v.recoveryAt) {v.recoveryAt = 0; qcom_iris_core_recover(&v.core);}
        if (v.suspendAt && nowMs() >= v.suspendAt) {
            v.suspendAt = 0;
            const bool was = v.core.suspended;
            qcom_iris_core_autosuspend(&v.core);
            if (!was && v.core.suspended) native_video_stats[8] = native_video_stats[8] + 1;
        }
        note();
        uint64_t next = UINT64_MAX;
        if (v.recoveryAt && v.recoveryAt < next) next = v.recoveryAt;
        if (v.suspendAt && v.suspendAt < next) next = v.suspendAt;
        IOLockUnlock(v.lock);
        if (next == UINT64_MAX) assert_wait((event_t)&v.workPending, THREAD_UNINT);
        else {
            const uint64_t t = nowMs();
            uint64_t deadline;
            clock_interval_to_deadline(next > t ? (uint32_t)(next - t) : 0, kMillisecondScale, &deadline);
            assert_wait_deadline((event_t)&v.workPending, THREAD_UNINT, deadline);
        }
        if (__atomic_exchange_n(&v.workPending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
        else {thread_block(THREAD_CONTINUE_NULL); __atomic_store_n(&v.workPending, 0u, __ATOMIC_RELEASE);}
    }
}

// ---- memory for the VPU (sheng's) ----
void *hostAllocate(void *, size_t bytes) {return IOMallocData(bytes);}
void hostRelease(void *, void *memory, size_t bytes) {IOFreeData(memory, bytes);}
int mapPrepared(IOMemoryDescriptor *memory, uint64_t first, uint64_t end, uint32_t align, uint32_t *iova)
{
    size_t count = 0;
    for (uint64_t at = first; at < end;) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
        if (!pa || (pa & 4095) || !length) return -EFAULT;
        if (pa + length > (UINT64_C(1) << v.mmu.arena.config.output_bits)) return -EFAULT;
        at += length > end - at ? end - at : length;
        ++count;
    }
    struct qcom_iris_segment *segments = IONewData(struct qcom_iris_segment, count);
    if (!segments) return -ENOMEM;
    size_t n = 0;
    for (uint64_t at = first; at < end; ++n) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
        const uint64_t take = length > end - at ? end - at : length;
        segments[n] = {pa, (take + 4095) & ~UINT64_C(4095)};
        at += take;
    }
    const int rc = qcom_iris_mmu_map(&v.mmu, segments, count, align, iova);
    IODeleteData(segments, struct qcom_iris_segment, count);
    return rc;
}
}  // namespace

bool QcomIrisDeviceReady(void) {return __atomic_load_n(&v.ready, __ATOMIC_ACQUIRE);}
IOLock *QcomIrisDeviceLock(void) {return v.lock;}
struct qcom_iris_core *QcomIrisDeviceCore(void) {return QcomIrisDeviceReady() ? &v.core : nullptr;}

int QcomIrisDeviceAllocate(uint64_t bytes, QcomIrisBuffer *b)
{
    *b = {};
    bytes = (bytes + 4095) & ~UINT64_C(4095);
    const uint64_t limit = UINT64_C(1) << v.mmu.arena.config.output_bits;
    // Pageable memory that prepare() wires (sheng run125: wired kernel allocations are refused at once when the free list is short).
    IOBufferMemoryDescriptor *memory = limit >= (UINT64_C(1) << 36)
        ? IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPageable, bytes, 4096)
        : IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut, bytes, (limit - 1) & ~UINT64_C(4095));
    if (!memory) return -ENOMEM;
    if (memory->prepare() != kIOReturnSuccess) {memory->release(); return -ENOMEM;}
    bzero(memory->getBytesNoCopy(), (size_t)bytes);
    uint32_t iova = 0;
    const int rc = mapPrepared(memory, 0, bytes, 4096, &iova);
    if (rc) {memory->complete(); memory->release(); return rc;}
    *b = {memory, iova, 0, bytes, memory->getBytesNoCopy(), nullptr};
    native_video_stats[17] = native_video_stats[17] + 1;
    return 0;
}
int QcomIrisDeviceMap(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, QcomIrisBuffer *b)
{
    *b = {};
    if (!memory || !length || offset + length > memory->getLength() || offset + length < offset) return -EINVAL;
    const uint64_t first = offset & ~UINT64_C(4095), end = (offset + length + 4095) & ~UINT64_C(4095);
    if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) return -ENOMEM;
    memory->retain();
    uint32_t iova = 0;
    const int rc = mapPrepared(memory, first, end > memory->getLength() ? memory->getLength() : end, 4096, &iova);
    if (rc) {memory->complete(kIODirectionInOut); memory->release(); return rc;}
    *b = {memory, iova, first, end - first, nullptr, nullptr};
    native_video_stats[17] = native_video_stats[17] + 1;
    return 0;
}
void QcomIrisDeviceRelease(QcomIrisBuffer *b)
{
    if (!b->memory) return;
    if (qcom_iris_mmu_unmap(&v.mmu, b->iova, (b->bytes + 4095) & ~UINT64_C(4095))) {*b = {}; return;}   // still reachable: stays wired
    b->memory->complete(kIODirectionInOut);
    b->memory->release();
    native_video_stats[17] = native_video_stats[17] - 1;
    *b = {};
}
int QcomIrisDeviceSend(const void *message, uint32_t size) {return qcom_iris_core_send(&v.core, message, size);}
bool QcomIrisDeviceWait(struct qcom_iris_session *s, unsigned ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!s->responded && s->state != QcomIrisError && mach_absolute_time() < deadline)
        IOLockSleepDeadline(v.lock, s, deadline, THREAD_UNINT);
    return s->responded;
}
void QcomIrisDeviceSignal(struct qcom_iris_session *s) {IOLockWakeup(v.lock, s, false);}
int QcomIrisDeviceAllocateInternal(uint32_t bytes, uint64_t *device_addr, void **handle)
{
    QcomIrisBuffer *b = IOMallocType(QcomIrisBuffer);
    const int rc = QcomIrisDeviceAllocate(bytes, b);
    if (rc) {IOFreeType(b, QcomIrisBuffer); return rc;}
    *device_addr = b->iova;
    *handle = b;
    return 0;
}
void QcomIrisDeviceReleaseInternal(void *handle)
{
    auto *b = static_cast<QcomIrisBuffer *>(handle);
    if (!b) return;
    QcomIrisDeviceRelease(b);
    IOFreeType(b, QcomIrisBuffer);
}

namespace {
const struct sm8550_apps_smmu_io kSmmuIo = {nullptr, smmuRead, smmuWrite};

// ---- stage 1: look, write nothing ----
bool survey()
{
    if (!mapWindow(v.gcc, RUAN_GCC_VIDEO_BASE, RUAN_GCC_VIDEO_SIZE) || !mapWindow(v.videocc, RUAN_VIDEO_CC_BASE, RUAN_VIDEO_CC_SIZE) ||
        !mapWindow(v.smmu, SM8550_APPS_SMMU_BASE, SM8550_APPS_SMMU_SIZE)) {fail(StSurvey, FailMap); return false;}
    struct ruan_video_cc_survey s;
    ruan_video_cc_survey(&v.ccIo, &s);
    native_video_stats[9] = (int32_t)s.mvsc_gdscr;
    native_video_stats[18] = (int32_t)s.mvs0_gdscr;
    native_video_stats[19] = (int32_t)s.pll_mode;
    IOLog("[video] survey gcc %x/%x/%x pll %x l %x user %x rcg %x/%x gdsc %x/%x cbcr %x %x %x %x %x xo %x\n", s.gcc_ahb, s.gcc_axi0,
          s.gcc_xo, s.pll_mode, s.pll_l, s.pll_user, s.rcg_cmd, s.rcg_cfg, s.mvsc_gdscr, s.mvs0_gdscr, s.ctl_axi, s.mvs0_axi,
          s.mvsc_core, s.mvs0_core, s.venus_ahb, s.xo);
    if (ruan_video_cc_vpu_accessible(&s) && mapWindow(v.vpu, kVpu, kVpuSize)) {   // never read a block whose clocks are off
        native_video_stats[20] = (int32_t)rd(v.vpu, kWrapperCorePower);
        native_video_stats[21] = (int32_t)rd(v.vpu, kCtrlStatus);
        unmap(v.vpu);
    }
    const int picked = sm8550_apps_smmu_pick(&kSmmuIo, kSids, 3, &v.domain);   // reads only
    native_video_stats[2] = (int32_t)(v.domain.bank | v.domain.asid << 8 | ((v.domain.id2 >> 4) & 15) << 24);
    native_video_stats[24] = (int32_t)v.domain.free_slots;
    QcomRpmhResource cx, mx, mm1;
    if (!readCmdDb()) {fail(StSurvey, FailCmdDb); return false;}
    (void)findResources(cx, mx, mm1);
    void *file = nullptr;
    QcomMdtImage *image = IOMallocType(QcomMdtImage);
    const int image_rc = openImage(&file, image);
    if (file) sheng_fw_close(file);
    IOFreeType(image, QcomMdtImage);
    native_video_stats[10] = image_rc;
    if (picked) {fail(StSurvey, picked == -EBUSY ? FailBusy : FailDomain); return false;}
    if (image_rc) {fail(StSurvey, FailFirmwareFile); return false;}
    if (s.pll_mode & 1u) {fail(StSurvey, FailBusy); return false;}   // PLL0 output on: someone else runs the block
    native_video_stats[0] = StSurvey;
    return true;
}

// ---- stage 2: power the block up, read its wrapper, power it down ----
bool powerCycle()
{
    if (!vote()) {fail(StPower, FailVote); return false;}
    if (!ruan_video_cc_configure(&v.ccIo, &v.cc, v.hz) || !ruan_video_cc_controller_on(&v.ccIo, &v.cc)) {
        noteCc(); ruan_video_cc_controller_off(&v.ccIo, &v.cc); fail(StPower, FailClocks); return false;
    }
    const bool hardware = ruan_video_cc_hardware_on(&v.ccIo, &v.cc);
    if (!hardware) noteCc();
    if (mapWindow(v.vpu, kVpu, kVpuSize)) {
        native_video_stats[20] = (int32_t)rd(v.vpu, kWrapperCorePower);
        native_video_stats[21] = (int32_t)rd(v.vpu, kCtrlStatus);
    }
    ruan_video_cc_hardware_off(&v.ccIo, &v.cc);
    ruan_video_cc_controller_off(&v.ccIo, &v.cc);
    native_video_stats[9] = (int32_t)rd(v.videocc, 0x5004);
    native_video_stats[18] = (int32_t)rd(v.videocc, 0x6004);
    if (!hardware || !v.vpu.va) {fail(StPower, FailClocks); return false;}
    native_video_stats[0] = StPower;
    return true;
}

// ---- stage 3: iris_probe without the video devices, then iris_core_init ----
bool start()
{
    v.lock = IOLockAlloc();
    v.irqLock = IOSimpleLockAlloc();
    if (!v.lock || !v.irqLock || !v.vpu.va) {fail(StMapped, FailMap); return false;}
    native_video_stats[0] = StMapped;
    {   // the domain: the empty table prepared, the bank programmed and enabled, then the streams routed
        int rc = sm8550_apps_smmu_pick(&kSmmuIo, kSids, 3, &v.domain);
        static const uint8_t oas[6] = {32, 36, 40, 42, 44, 48};
        const unsigned code = (v.domain.id2 >> 4) & 15, output = code < 6 ? oas[code] : 0;
        if (!rc && !output) rc = -ENOTSUP;
        if (!rc) {
            v.tables.limit = UINT64_C(1) << output;
            v.tables.provider = {&v.tables, TablePages::allocate, TablePages::find, TablePages::overlaps};
            const struct qcom_iris_mmu_memory memory = {nullptr, hostAllocate, hostRelease};
            const ArmSmmu4KLiveVisibility visibility = {nullptr, publishTable};
            rc = qcom_iris_mmu_prepare(&v.mmu, &memory, &v.tables.provider, &visibility, output);
            const ArmSmmu4KIo kit = {nullptr, (size_t)SM8550_APPS_SMMU_SIZE, kitRead32, kitRead64, kitWrite32, kitWrite64, kitBarrier};
            if (!rc) rc = qcom_iris_mmu_program(&v.mmu, &kit, v.domain.bank, v.domain.asid, kPollBudget);
            if (!rc) rc = sm8550_apps_smmu_route(&kSmmuIo, &v.domain);
        }
        native_video_stats[2] = (int32_t)(v.domain.bank | v.domain.asid << 8 | output << 24);
        native_video_stats[15] = (int32_t)v.mmu.last_status;
        if (rc) {fail(StDomain, rc); return false;}
    }
    native_video_stats[0] = StDomain;
    const uint32_t region = qcom_iris_uc_region_size();   // the queue table, the queues, the SFR (1 MiB aligned)
    {
        IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut,
            region, ((UINT64_C(1) << v.mmu.arena.config.output_bits) - 1) & ~UINT64_C(4095));
        if (!memory || memory->prepare() != kIOReturnSuccess) {if (memory) memory->release(); fail(StQueues, -ENOMEM); return false;}
        bzero(memory->getBytesNoCopy(), region);
        uint32_t iova = 0;
        const int rc = mapPrepared(memory, 0, region, 1u << 20, &iova);
        if (rc) {memory->complete(); memory->release(); fail(StQueues, rc); return false;}
        v.uc = {memory, iova, 0, region, memory->getBytesNoCopy(), nullptr};
    }
    uint8_t *base = static_cast<uint8_t *>(v.uc.cpu);
    const struct qcom_iris_queues queues = {base, v.uc.iova};
    qcom_iris_core_setup(&v.core, &kOps, &v.vpuIo, &queues, reinterpret_cast<volatile uint32_t *>(base + QCOM_IRIS_QUEUE_BYTES),
                         v.uc.iova + (uint32_t)QCOM_IRIS_QUEUE_BYTES, kPipes);
    v.core.vpu2 = true;   // patches/qcom-iris-ruan.patch: iris2 one-pipe power-off halves
    uint32_t logging = 0;
    if (PE_parse_boot_argn("log-video", &logging, sizeof logging) && logging) v.core.debug_level = 1 | 2 | 8;
    native_video_stats[0] = StQueues;
    v.irqDepth = 1;   // requested disabled: the first power-on enables it
    if (!armcpu_spi_attach || !armcpu_spi_mask || armcpu_spi_attach(kIntid, false, primary, nullptr, nullptr, nullptr))
        {fail(StInterrupt, FailInterrupt); return false;}
    armcpu_spi_mask(kIntid, true);
    native_video_stats[0] = StInterrupt;
    thread_t thread = nullptr;
    if (kernel_thread_start(irqThread, nullptr, &thread) != KERN_SUCCESS) {fail(StThreads, FailThreads); return false;}
    thread_deallocate(thread);
    if (kernel_thread_start(workThread, nullptr, &thread) != KERN_SUCCESS) {fail(StThreads, FailThreads); return false;}
    thread_deallocate(thread);
    __atomic_store_n(&v.ready, true, __ATOMIC_RELEASE);
    native_video_stats[0] = StThreads;
    IOLockLock(v.lock);   // iris_open's core init: a get around it, then autosuspend
    int rc = qcom_iris_core_get(&v.core);
    if (!rc && v.core.state != QcomIrisCoreInit) rc = qcom_iris_core_init(&v.core);
    qcom_iris_core_put(&v.core);
    note();
    if (v.core.version[0]) strlcpy(native_video_text, v.core.version, sizeof native_video_text);
    IOLockUnlock(v.lock);
    if (rc) {fail(StFirmware, rc); return false;}
    native_video_stats[0] = StFirmware;
    return true;
}

// ---- stage 4: one decoder session opened, its format set and closed (IrisMedia.cpp openIris / closeIris without the device) ----
int sendOp(void *, const void *message, uint32_t size) {return QcomIrisDeviceSend(message, size);}
int allocateOp(void *, uint32_t bytes, uint64_t *addr, void **handle) {return QcomIrisDeviceAllocateInternal(bytes, addr, handle);}
void releaseOp(void *, void *handle) {QcomIrisDeviceReleaseInternal(handle);}
bool waitOp(void *, struct qcom_iris_session *s, unsigned ms) {return QcomIrisDeviceWait(s, ms);}
void signalOp(void *, struct qcom_iris_session *s) {QcomIrisDeviceSignal(s);}
void frameOp(void *, struct qcom_iris_session *, const struct qcom_iris_frame *) {}
void changeOp(void *, struct qcom_iris_session *, const struct qcom_iris_stream_info *) {}
void failedOp(void *, struct qcom_iris_session *, uint32_t error) {native_video_stats[25] = (int32_t)error;}
const struct qcom_iris_session_ops kSessionOps = {nullptr, sendOp, allocateOp, releaseOp, waitOp, signalOp, frameOp, frameOp, changeOp,
                                                  failedOp};
bool sessionTest()
{
    struct qcom_iris_session *s = IOMallocType(struct qcom_iris_session);
    IOLockLock(v.lock);
    int rc = qcom_iris_core_get(&v.core);
    if (!rc && v.core.state != QcomIrisCoreInit) rc = qcom_iris_core_init(&v.core);
    bool added = false, opened = false;
    if (!rc) {
        qcom_iris_session_init(s, &kSessionOps, &v.core.header_id, &v.core.packet_id, qcom_iris_core_session_id(&v.core), false,
                               QcomIrisCodecH264, v.core.num_vpp_pipe);
        rc = qcom_iris_core_add_session(&v.core, s);
        added = !rc;
    }
    if (!rc) {rc = qcom_iris_session_open(s) ? -EIO : 0; opened = !rc;}
    if (!rc) rc = qcom_iris_decoder_set_format(s, 1280, 720) ? -EINVAL : 0;
    if (opened && qcom_iris_session_close(s) && !rc) rc = -ETIMEDOUT;
    if (added) qcom_iris_core_remove_session(&v.core, s);
    qcom_iris_core_put(&v.core);
    note();
    IOLockUnlock(v.lock);
    IOFreeType(s, struct qcom_iris_session);
    if (!native_video_stats[25]) native_video_stats[25] = rc;
    if (rc) {fail(StSession, FailSession); return false;}
    native_video_stats[0] = StSession;
    return true;
}

void videoThread(void *context, wait_result_t)
{
    unsigned mode = 0, delay = (unsigned)(uintptr_t)context, mhz = 240;
    PE_parse_boot_argn("ruan-video", &mode, sizeof mode);
    PE_parse_boot_argn("ruan-video-delay", &delay, sizeof delay);
    PE_parse_boot_argn("ruan-video-mhz", &mhz, sizeof mhz);
    v.tcs = 1;
    PE_parse_boot_argn("ruan-video-tcs", &v.tcs, sizeof v.tcs);
    v.hz = mhz == 133 ? 133333333u : mhz * 1000000u;
    if (delay) IOSleep(delay * 1000);
    if (!ruan_video_cc_rate_supported(v.hz) || v.tcs > 2) {fail(StSurvey, -EINVAL); return;}
    v.vpuIo = {nullptr, vpuRead, vpuWrite, delayUs};
    v.ccIo = {nullptr, ccRead, ccWrite, delayUs};
    if (!survey() || mode < 2) return;
    if (!powerCycle() || mode < 3) return;
    if (!start() || mode < 4) return;
    if (!sessionTest() || mode < 5) return;
    if (qcom_iris_video_publish) {qcom_iris_video_publish(); native_video_stats[0] = StPublished;}
}

volatile uint32_t gStarted;
void startOnce(unsigned delay)
{
    unsigned mode = 0;
    PE_parse_boot_argn("ruan-video", &mode, sizeof mode);
    if (!mode || !__sync_bool_compare_and_swap(&gStarted, 0u, 1u)) return;
    thread_t thread;
    if (kernel_thread_start(videoThread, reinterpret_cast<void *>(uintptr_t(delay)), &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(StSurvey, FailThreads);
}
}  // namespace

// boot_progress_start (BootProgress.c, a strong reference that pulls this file out of libRamRootProbe.a) after ruan-video-delay, or the
// late probe (LegacyDisplayProbe.cpp, weak) at once, whichever comes first.
extern "C" void ruan_video_start(void) {startOnce(30);}
extern "C" void ruan_video_init(void) {startOnce(0);}

// USB `video`: one line. st/err, GDSCs mvsc/mvs0, PLL0 mode, videocc failed step, VPU power status / CTRL_STATUS, SMMU bank|asid|oas
// and free slots, rails found, firmware KiB / span pages / load result, last failed SCM x0/x1, interrupts, core state, session result.
extern "C" const char *ruan_video_status_str(void)
{
    static char line[320];
    const volatile int32_t *s = native_video_stats;
    if (!gStarted) return "not started (ruan-video=0, or before the start hooks)";
    snprintf(line, sizeof line, "video st %d err %d gdsc %x/%x pll %x cc %x vpu %x/%x smmu %x free %d rails %x fw %dK span %d load %d"
             " scm %d/%d irq %d core %d syserr %x sess %d mhz %u", s[0], s[1], s[9], s[18], s[19], s[11], s[20], s[21], s[2], s[24], s[14],
             s[22], s[23], s[10], s[12], s[13], s[3], s[5], s[6], s[25], v.hz / 1000000);
    return line;
}
// USB `video.log`: 0 the firmware version, 1-4 its last debug lines (log-video=1), oldest first.
extern "C" unsigned ruan_video_log(unsigned i, char *out, unsigned len)
{
    if (!i) return (unsigned)snprintf(out, len, "[usb] VIDEO.version: %s\r\n", native_video_text);
    const uint32_t total = native_video_fw_lines, first = total > 4 ? total - 4 : 0;
    if (i > 4 || first + i - 1 >= total) return 0;
    return (unsigned)snprintf(out, len, "[usb] VIDEO.fw: %s\r\n", native_video_fw_text[(first + i - 1) & 3]);
}
