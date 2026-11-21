// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan-audio=N: the speaker path of the Redmi Pad Pro 5G (SM7435), stage by stage. NOT BUILT, NOT BOOTED. Replaces the status stub
// RuanAudioProbe.c under the same two names (ruan_audio_init: LegacyDisplayProbe.cpp's late probe; ruan_audio_status_str: USB `audio`).
// (macOS/docs/research/audio/plan.md has the path and the evidence; port/README.md the apply and test steps.)
//
// The path (verified on stock Android, logs/2026-11-13-call + vendor mixer_paths_parrot_qrd.xml "speaker"): PAL plays the speaker on
// CODEC_DMA-LPAIF_RXTX-RX-1 (2 ch, 48 kHz, 16 bit) -> LPASS RX macro (RX2/RX3 -> INT0/INT2) -> RX SoundWire -> WCD937x EAR and AUX
// outputs (analog) -> four SI-A SIA81x9 amplifiers (I2C 0x28/0x29 on QUP0 SE0 and QUP1 SE0; enables GPIO 89/88 and 106/103).
// The WSA883x and FS1815 nodes in the DT are not fitted (wsa-max-devs 0; the FS amps NAK on Android: "fsm_reg_read ... fail:-5").
//
// Each stage includes the ones before; a failed step records -step and the boot goes on (USB `audio` prints the line):
//   1  survey: ADSP SMEM/SMP2P state (RuanAdsp.cpp, reads only) and the TLMM config of the nine audio pins (reads only). No LPASS access.
//   2  ADSP: QMP load_state, lcx/lmx votes, PAS 1 from modem_a image/adsp.*, SMP2P ready, GLINK; "adsp_apps" GPR; APM says ready.
//   3  PRM: LPASS core macro and digital codec votes, RX core MCLK (0x30e, 22.5792 MHz) and RX MCLK2 2x (0x318, 19.2 MHz). Only when all
//      four answers are status 0 does it read two RX macro registers (0x3200000 + 0x0, + 0x40). LPASS MMIO is never touched otherwise:
//      a clock-gated LPASS read reset the tablet (README rbu24).
//   4  amplifiers: enable pins high, bit-banged I2C (the two QUP pairs taken as GPIOs and given back), one read of register 0 from each of
//      the four SIA81x9; enable pins and pins restored after. No amplifier register is written.
//   5  DSP data path, inaudible: SMMU pass-through for the LPASS stream 0x1801, a 64 KiB period buffer in low DRAM, the codec DMA RX-1
//      device graph + PCM stream graph opened, configured, prepared and started, 100 periods of silence (1 s) written, WRITE_DONEs
//      counted, graphs stopped and closed. The RX macro, SoundWire and WCD937x are not set up, so nothing reaches the speakers.
//   6+ not implemented: RX macro mixer, RX SoundWire enumeration, WCD937x power-up and EAR/AUX PAs, amplifier enable, IOAudio2Device.
// Boot args: ruan-audio=N (default 0: nothing), ruan-audio-pa=<phys> (period buffer, default 0x8bf00000), ruan-audio-fw=<dir>
// (adsp.mbn from a directory instead of modem_a).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengGpr.h"
#include "ShengAudioReach.h"
#include "SmmuBypass.h"
void ruan_adsp_probe(void);                                   // RuanAdsp.cpp
extern volatile int32_t native_adsp_stats[16];
extern char native_adsp_text[96];
int kernel_thread_start(void (*)(void *, int), void *, thread_t *);
void thread_deallocate(thread_t);
}

// [0] stage reached (10 x stage, 1x sub-step) or -step that failed, [1] last DSP status or error, [2] DSP answers, [3] map handle,
// [4] periods sent, [5] returned, [6] PRM statuses: macro | dcodec << 8 | rx core clk << 16 | rx 2x clk << 24 (0xff no answer),
// [7] RX macro +0x0, [8] RX macro +0x40, [9] SMMU result (1 ok, else error code << 8), [10] amps that ACKed (bit per amp: R1 R2 L1 L2),
// [11] their register 0 (byte per amp), [12] last opcode, [13] I2C NAKs, [14] ms from start to APM ready, [15] ms the stage took
extern "C" {volatile int32_t ruan_audio_stats[16]; volatile uint32_t ruan_audio_pins[9][2];}

namespace {
constexpr uint64_t kTlmm = 0xf100000;                         // RuanWifi port "Verified": GPIO blocks at 0xf000000 + 0x100000
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0xd0000;  // RuanWifi.cpp
constexpr uint16_t kStream = 0x1801;                          // msm-audio-ion iommus <&apps_smmu 0x1801 0> (DT 22024)
constexpr uint64_t kRxMacro = 0x3200000;                      // rx-macro@3200000 (DT 24699)
constexpr uint64_t kDefaultPa = 0x8bf00000;                   // mpss_region + 3 MiB: below RuanWifi's 0x8c000000 window, above the PAS meta page
// Pins (live DT): I2C QUP0 SE0 SDA 28 SCL 29 (right amps), QUP1 SE0 SDA 4 SCL 5 (left amps); amp enables R1 89, R2 88, L1 106, L2 103;
// WCD937x reset 102.
constexpr unsigned kPins[9] = {28, 29, 4, 5, 89, 88, 106, 103, 102};
struct Amp {unsigned sda, scl, enable; uint8_t address;};
constexpr Amp kAmps[4] = {{28, 29, 89, 0x28}, {28, 29, 88, 0x29}, {4, 5, 106, 0x28}, {4, 5, 103, 0x29}};
constexpr uint32_t kPort = 0x2001, kIntentSize = 512, kIntentCount = 20, kRing = 16, kMessage = 128;
constexpr uint32_t kRate = 48000, kChannels = 2, kPeriodFrames = 480, kPeriodBytes = kPeriodFrames * kChannels * 2, kSlots = 8;
constexpr uint32_t kBufferBytes = 64 * 1024, kPeriods = 100, kInFlight = 4;
constexpr uint32_t kRx1 = 2;                                  // AudioReach CODEC_RX1 (RX0 = 1), PAL backend CODEC_DMA-LPAIF_RXTX-RX-1

void stage(int s) {ruan_audio_stats[0] = s;}
void fail(int s, int error) {ruan_audio_stats[1] = error; ruan_audio_stats[0] = -s;}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
// One plain LDR/STR per access (RuanWifi.cpp: Gunyah emulates trapped SMMU accesses from the syndrome; a writeback load SErrors).
uint32_t rd(volatile uint8_t *base, uint32_t at) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(base + at) : "memory");
    return v;
}
void wr(volatile uint8_t *base, uint32_t at, uint32_t v) {
    __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(base + at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- TLMM: config at +0 (pull 1:0, function 5:2, drive 8:6, output enable 9), in/out at +4 (in bit 0, out bit 1) ----
Window gPin[9];
volatile uint8_t *pin(unsigned gpio) {
    for (unsigned i = 0; i < 9; ++i) if (kPins[i] == gpio) return gPin[i].va ? gPin[i].va : (mapWindow(gPin[i], kTlmm + 0x1000ull * gpio, 0x1000) ? gPin[i].va : nullptr);
    return nullptr;
}
bool surveyPins() {
    for (unsigned i = 0; i < 9; ++i) {
        volatile uint8_t *p = pin(kPins[i]);
        if (!p) return false;
        ruan_audio_pins[i][0] = rd(p, 0);
        ruan_audio_pins[i][1] = rd(p, 4);
    }
    return true;
}

// ---- bit-banged I2C at about 100 kHz (HakiOS hakios.speaker.c's way): a line is driven low (OE on, out 0) or let go (OE off, pull-up) ----
struct Bus {volatile uint8_t *sda, *scl;};
void lineLow(volatile uint8_t *p) {wr(p, 4, 0); wr(p, 0, rd(p, 0) | 1u << 9);}
void lineGo(volatile uint8_t *p) {wr(p, 0, rd(p, 0) & ~(1u << 9));}
bool lineIn(volatile uint8_t *p) {return rd(p, 4) & 1u;}
void half() {IODelay(5);}
bool sclHigh(const Bus &b) {
    lineGo(b.scl);
    for (unsigned i = 0; i < 200 && !lineIn(b.scl); ++i) IODelay(5);   // clock stretching, 1 ms at most
    return lineIn(b.scl);
}
bool start(const Bus &b) {
    lineGo(b.sda); if (!sclHigh(b)) return false; half();
    if (!lineIn(b.sda)) return false;                                   // someone holds the bus
    lineLow(b.sda); half(); lineLow(b.scl); half();
    return true;
}
void stop(const Bus &b) {lineLow(b.sda); half(); sclHigh(b); half(); lineGo(b.sda); half();}
bool writeByte(const Bus &b, uint8_t v) {
    for (int bit = 7; bit >= 0; --bit) {
        if (v >> bit & 1) lineGo(b.sda); else lineLow(b.sda);
        half(); if (!sclHigh(b)) return false; half(); lineLow(b.scl);
    }
    lineGo(b.sda); half(); if (!sclHigh(b)) return false;
    const bool ack = !lineIn(b.sda);
    half(); lineLow(b.scl);
    return ack;
}
uint8_t readByteNak(const Bus &b) {
    uint8_t v = 0;
    lineGo(b.sda);
    for (int bit = 7; bit >= 0; --bit) {half(); sclHigh(b); v = (uint8_t)(v << 1 | lineIn(b.sda)); half(); lineLow(b.scl);}
    lineGo(b.sda); half(); sclHigh(b); half(); lineLow(b.scl);       // NAK: last byte
    return v;
}
// One register read; false on any NAK. The caller holds the pins as GPIOs.
bool readReg(const Bus &b, uint8_t address, uint8_t reg, uint8_t *value) {
    bool ok = start(b) && writeByte(b, (uint8_t)(address << 1)) && writeByte(b, reg) && start(b) && writeByte(b, (uint8_t)(address << 1 | 1));
    if (ok) *value = readByteNak(b);
    stop(b);
    return ok;
}

// ---- the DSP channel: callbacks copy and wake (RuanAdsp.cpp's thread); this thread parses ----
struct Dsp {
    ShengAdspChannel *channel;
    IOLock *lock;
    IOSimpleLock *ringLock;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t head, count, token, handle;
    volatile uint32_t up;
} gDsp;
void dspState(void *, int up) {__atomic_store_n(&gDsp.up, up ? 1u : 0u, __ATOMIC_RELEASE); IOLockWakeup(gDsp.lock, &gDsp.count, false);}
void dspReceive(void *, const uint8_t *data, uint32_t length) {
    IOSimpleLockLock(gDsp.ringLock);
    if (gDsp.count < kRing) {
        auto &slot = gDsp.ring[(gDsp.head + gDsp.count) % kRing];
        slot.length = length < kMessage ? length : kMessage;
        memcpy(slot.data, data, slot.length);
        gDsp.count++;
    }
    IOSimpleLockUnlock(gDsp.ringLock);
    IOLockWakeup(gDsp.lock, &gDsp.count, false);
}
// The next raw packet, waiting up to ms; 0 on timeout.
uint32_t nextPacket(uint8_t *out, uint32_t ms) {
    const uint64_t end = nowMs() + ms;
    for (;;) {
        uint32_t length = 0;
        IOSimpleLockLock(gDsp.ringLock);
        if (gDsp.count) {
            auto &slot = gDsp.ring[gDsp.head];
            length = slot.length;
            memcpy(out, slot.data, length);
            gDsp.head = (gDsp.head + 1) % kRing;
            gDsp.count--;
        }
        IOSimpleLockUnlock(gDsp.ringLock);
        if (length) {ruan_audio_stats[2] = ruan_audio_stats[2] + 1; return length;}
        const uint64_t now = nowMs();
        if (now >= end) return 0;
        uint64_t deadline;
        clock_interval_to_deadline((uint32_t)(end - now), kMillisecondScale, &deadline);
        IOLockLock(gDsp.lock);
        if (!gDsp.count) IOLockSleepDeadline(gDsp.lock, &gDsp.count, deadline, THREAD_UNINT);
        IOLockUnlock(gDsp.lock);
    }
}
bool send(const uint8_t *packet, uint32_t length) {return length && sheng_adsp_send(gDsp.channel, packet, length) == 0;}
uint32_t gWritesDone;
// Sends one command and waits up to 2 s for its answer: a basic result for `opcode`, the map handle, or a PRM done. Write answers that
// arrive meanwhile are counted (stage 5). Returns the status (0 ok), -60 on no answer, -1 when the send was refused.
int command(const uint8_t *packet, uint32_t length, uint32_t opcode) {
    if (!send(packet, length)) return -1;
    const uint64_t end = nowMs() + 2000;
    uint8_t in[kMessage];
    while (nowMs() < end) {
        const uint32_t n = nextPacket(in, 2000);
        ArAnswer a;
        if (!n || ar_parse(in, n, &a) == ArBad) continue;
        ruan_audio_stats[12] = (int32_t)a.opcode;
        if (a.opcode == AR_OP_WRITE_DONE) {gWritesDone++; ruan_audio_stats[5] = ruan_audio_stats[5] + 1; continue;}
        if (a.opcode == AR_OP_MEM_MAP_DONE && opcode == AR_OP_MEM_MAP) {gDsp.handle = a.handle; ruan_audio_stats[3] = (int32_t)a.handle; return 0;}
        if (a.opcode == GPR_OP_BASIC_RESULT && a.result_opcode == opcode) return ruan_audio_stats[1] = (int32_t)a.status;
        if ((a.opcode == AR_OP_PRM_REQUEST_DONE && opcode == AR_OP_PRM_REQUEST) || (a.opcode == AR_OP_PRM_RELEASE_DONE && opcode == AR_OP_PRM_RELEASE))
            return ruan_audio_stats[1] = (int32_t)a.status;
    }
    return ruan_audio_stats[1] = -60;
}

// ---- stage 2: the channel and the framework ----
bool apmReady() {
    static const ShengAdspClient client = {dspState, dspReceive};
    gDsp.channel = sheng_adsp_channel("adsp_apps", kIntentSize, kIntentCount, 0, &client, nullptr);   // the DSP opens it (sheng)
    if (!gDsp.channel) {fail(21, -12); return false;}
    for (unsigned i = 0; i < 1200 && !__atomic_load_n(&gDsp.up, __ATOMIC_ACQUIRE); ++i) {   // up to 2 min: the firmware may wait for UFS
        if (native_adsp_stats[0] < 0) {fail(22, native_adsp_stats[0]); return false;}
        IOSleep(100);
    }
    if (!gDsp.up) {fail(22, native_adsp_stats[0]); return false;}
    GprSpfProbe spf;
    gpr_spf_init(&spf, kPort, 20);
    uint8_t q[GPR_HEADER_BYTES], in[kMessage];
    for (const uint64_t end = nowMs() + 30000;;) {
        if (nowMs() >= end) {fail(23, -60); return false;}
        const uint32_t n = gpr_spf_next(&spf, q);
        if (n && !send(q, n)) {fail(23, -1); return false;}
        const uint32_t got = nextPacket(in, 1000);
        const int r = got ? gpr_spf_receive(&spf, in, got) : gpr_spf_timeout(&spf);
        if (r == SpfIsReady) return true;
        if (r == SpfDone || spf.state == SpfGaveUp) {fail(23, (int)spf.last_status); return false;}
        if (r == SpfNotReady || r == SpfError) IOSleep(500);
    }
}

// ---- stage 3: PRM votes and clocks (sent from the PRM's own port, sheng i504), then two guarded RX macro reads ----
bool prm() {
    uint8_t b[GPR_MAX_PACKET];
    auto status = [](int s) {return s == 0 ? 0u : s == -60 ? 0xffu : (uint32_t)(s & 0xff) ? (uint32_t)(s & 0xff) : 0xfeu;};
    uint32_t s = status(command(b, ar_prm_vote(b, sizeof b, AR_PRM_IID, ++gDsp.token, AR_HW_MACRO, 1), AR_OP_PRM_REQUEST));
    s |= status(command(b, ar_prm_vote(b, sizeof b, AR_PRM_IID, ++gDsp.token, AR_HW_DCODEC, 1), AR_OP_PRM_REQUEST)) << 8;
    s |= status(command(b, ar_prm_clock(b, sizeof b, AR_PRM_IID, ++gDsp.token, 0x30e, 22579200), AR_OP_PRM_REQUEST)) << 16;   // rx_core_clk
    s |= status(command(b, ar_prm_clock(b, sizeof b, AR_PRM_IID, ++gDsp.token, 0x318, 19200000), AR_OP_PRM_REQUEST)) << 24;   // rx_mclk2_2x_clk
    ruan_audio_stats[6] = (int32_t)s;
    if (s) {fail(31, (int32_t)s); return false;}   // no LPASS access without all four
    stage(31);
    Window rx = {};
    if (!mapWindow(rx, kRxMacro, 0x1000)) {fail(32, -12); return false;}
    ruan_audio_stats[7] = (int32_t)rd(rx.va, 0x0);    // CDC_RX_TOP_TOP_CFG0 (Linux lpass-rx-macro.c)
    ruan_audio_stats[8] = (int32_t)rd(rx.va, 0x40);   // CDC_RX_CLK_RST_CTRL_MCLK_CONTROL
    unmap(rx);
    return true;
}

// ---- stage 4: the amplifiers answer ----
bool amps() {
    uint32_t saved[9][2];
    for (unsigned i = 0; i < 9; ++i) {volatile uint8_t *p = pin(kPins[i]); if (!p) {fail(41, -12); return false;} saved[i][0] = rd(p, 0); saved[i][1] = rd(p, 4);}
    for (const Amp &a : kAmps) {volatile uint8_t *e = pin(a.enable); wr(e, 4, 2); wr(e, 0, 1u << 9);}   // GPIO, output high, 2 mA
    IOSleep(5);
    // The I2C pairs as GPIOs: function 0, pull-up, 2 mA, released; output latch 0 so that output enable drives low.
    for (unsigned i = 0; i < 4; ++i) {volatile uint8_t *p = pin(kPins[i]); wr(p, 4, 0); wr(p, 0, 3u);}
    uint32_t acked = 0, regs = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const Bus bus = {pin(kAmps[i].sda), pin(kAmps[i].scl)};
        uint8_t v = 0;
        if (readReg(bus, kAmps[i].address, 0x00, &v)) {acked |= 1u << i; regs |= (uint32_t)v << (8 * i);}
        else ruan_audio_stats[13] = ruan_audio_stats[13] + 1;
    }
    for (unsigned i = 0; i < 9; ++i) {volatile uint8_t *p = pin(kPins[i]); wr(p, 4, saved[i][1]); wr(p, 0, saved[i][0]);}   // as found
    ruan_audio_stats[10] = (int32_t)acked;
    ruan_audio_stats[11] = (int32_t)regs;
    if (acked != 0xf) {fail(41, (int32_t)acked); return false;}
    return true;
}

// ---- stage 5: the DSP half of the path, with silence ----
uint32_t smmuRd(void *ctx, unsigned o) {return rd(static_cast<volatile uint8_t *>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr(static_cast<volatile uint8_t *>(ctx), o, v);}
Window gSmmu, gBuffer;
bool dataPath() {
    uint64_t pa = kDefaultPa;
    PE_parse_boot_argn("ruan-audio-pa", &pa, sizeof pa);
    if ((pa & 0xfff) || pa + kBufferBytes > 0x100000000ULL || !mapWindow(gBuffer, pa, kBufferBytes, kIOMapWriteCombineCache)) {fail(51, -12); return false;}
    for (uint32_t i = 0; i < kBufferBytes; ++i) gBuffer.va[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!mapWindow(gSmmu, kSmmu, kSmmuBytes)) {fail(52, -12); return false;}
    const struct smmu_io io = {const_cast<uint8_t *>(gSmmu.va), smmuRd, smmuWr};
    struct smmu_plan plan = smmu_plan_bypass(&io, kStream, pa, kBufferBytes);
    const char *e = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, kStream);
    ruan_audio_stats[9] = e ? 2 << 8 : 1;
    if (e) {fail(52, -22); return false;}
    stage(52);
    // The DSP takes a 32-bit address with the stream's index (0x1801 & 0xf) in the upper word (sheng i491-495).
    const uint64_t dsp = pa | (uint64_t)(kStream & 0xf) << 32;
    uint8_t b[GPR_MAX_PACKET];
    const ArFormat f = {kRate, 16, kChannels};
    int rc;
    if ((rc = command(b, ar_map(b, sizeof b, kPort, ++gDsp.token, dsp, kBufferBytes), AR_OP_MEM_MAP))) {fail(53, rc); return false;}
    if ((rc = command(b, ar_open_codec_rx_device(b, sizeof b, kPort, ++gDsp.token), AR_OP_GRAPH_OPEN))) {fail(54, rc); return false;}
    if ((rc = command(b, ar_config_codec_rx_device(b, sizeof b, kPort, ++gDsp.token, &f, kRx1, 3), AR_OP_SET_CFG))) {fail(55, rc); return false;}
    if ((rc = command(b, ar_open_playback_stream_to(b, sizeof b, kPort, ++gDsp.token, AR_IID_SPEAKER_DMA), AR_OP_GRAPH_OPEN))) {fail(56, rc); return false;}
    if ((rc = command(b, ar_config_playback_stream(b, sizeof b, kPort, ++gDsp.token, &f), AR_OP_SET_CFG))) {fail(56, rc); return false;}
    if ((rc = command(b, ar_config_playback_shmem(b, sizeof b, kPort, ++gDsp.token, &f), AR_OP_SET_CFG))) {fail(56, rc); return false;}
    stage(56);
    bool ok = true;
    static const uint32_t kUp[2] = {AR_OP_GRAPH_PREPARE, AR_OP_GRAPH_START}, kDevFirst[2] = {AR_SG_SPEAKER, AR_SG_PLAY},
                          kStreamFirst[2] = {AR_SG_PLAY, AR_SG_SPEAKER};
    for (uint32_t op : kUp)
        for (uint32_t sg : kDevFirst)
            if (ok && (rc = command(b, ar_graph_command(b, sizeof b, kPort, ++gDsp.token, op, sg), op))) {fail(57, rc); ok = false;}
    if (ok) {
        stage(57);
        uint32_t sent = 0;
        uint8_t in[kMessage];
        const uint64_t end = nowMs() + 3000;
        while (gWritesDone < kPeriods && nowMs() < end) {
            while (sent < kPeriods && sent - gWritesDone < kInFlight) {
                const uint32_t n = ar_write(b, sizeof b, kPort, sent, dsp + (sent % kSlots) * kPeriodBytes, gDsp.handle, kPeriodBytes);
                if (!send(b, n)) break;
                sent++;
                ruan_audio_stats[4] = (int32_t)sent;
            }
            const uint32_t got = nextPacket(in, 100);
            ArAnswer a;
            if (got && ar_parse(in, got, &a) == ArWriteDone) {gWritesDone++; ruan_audio_stats[5] = (int32_t)gWritesDone; if (a.status) ruan_audio_stats[1] = (int32_t)a.status;}
        }
        if (gWritesDone < kPeriods) {fail(58, (int32_t)gWritesDone); ok = false;}
    }
    for (uint32_t sg : kStreamFirst) command(b, ar_graph_command(b, sizeof b, kPort, ++gDsp.token, AR_OP_GRAPH_STOP, sg), AR_OP_GRAPH_STOP);
    for (uint32_t sg : kStreamFirst) command(b, ar_graph_command(b, sizeof b, kPort, ++gDsp.token, AR_OP_GRAPH_CLOSE, sg), AR_OP_GRAPH_CLOSE);
    command(b, ar_unmap(b, sizeof b, kPort, ++gDsp.token, gDsp.handle), AR_OP_MEM_UNMAP);
    return ok;
}

void audioThread(void *, int) {
    unsigned mode = 0;
    PE_parse_boot_argn("ruan-audio", &mode, sizeof mode);
    const uint64_t began = nowMs();
    uint64_t at = began;
    auto took = [&at] {const uint64_t now = nowMs(); ruan_audio_stats[15] = (int32_t)(now - at); at = now;};
    ruan_adsp_probe();   // its own thread: survey at 1, boot at 2+
    if (!surveyPins()) {fail(11, -12); return;}
    stage(10);
    if (mode < 2) return;
    if (!apmReady()) return;
    ruan_audio_stats[14] = (int32_t)(nowMs() - began);
    took();
    stage(20);
    if (mode < 3 || !prm()) return;
    took();
    stage(30);
    if (mode < 4 || !amps()) return;
    took();
    stage(40);
    if (mode < 5 || !dataPath()) return;
    took();
    stage(50);
}

volatile uint32_t gStarted;
}  // namespace

extern "C" void ruan_audio_init(void) {
    unsigned mode = 0;
    thread_t thread;
    if (!PE_parse_boot_argn("ruan-audio", &mode, sizeof mode) || !mode) return;
    if (!__sync_bool_compare_and_swap(&gStarted, 0u, 1u)) return;
    gDsp.lock = IOLockAlloc();
    gDsp.ringLock = IOSimpleLockAlloc();
    if (!gDsp.lock || !gDsp.ringLock || kernel_thread_start(audioThread, nullptr, &thread)) {fail(1, -12); return;}
    thread_deallocate(thread);
}

// USB `audio`: one line. Stage/error, DSP st/err (RuanAdsp.cpp: 11 ready, 13 channel up), PRM, RX macro, amps, DSP periods, the ADSP note.
extern "C" const char *ruan_audio_status_str(void) {
    static char line[360];
    const volatile int32_t *s = ruan_audio_stats, *d = native_adsp_stats;
    if (!gStarted) return "not started (ruan-audio=0, or before the late probe)";
    snprintf(line, sizeof line, "st %d err %d | adsp st %d err %d slave %x items %x kib %d ready %dms glink %d/%d ch %x | apm %dms answers %d op %x"
             " | prm %x rx %x/%x | amps %x regs %08x nak %d | smmu %x map %x periods %d/%d | %dms | %s",
             s[0], s[1], d[0], d[1], d[3], d[4], d[7], d[8], d[9], d[10], d[11], s[14], s[2], s[12], s[6], s[7], s[8], s[10], s[11], s[13],
             s[9], s[3], s[4], s[5], s[15], native_adsp_text);
    return line;
}
