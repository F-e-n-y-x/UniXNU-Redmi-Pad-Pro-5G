// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: the USB link's `gpu` command for the Adreno 710 stage S1 sequence (package/RuanGpuPower.c, gpu-dev/README.md).
// Included by UsbDebugCdc.cpp right before handleCommand (tools/ruan_additions.py inserts the include and three hooks:
// usbGpuCommand at the top of handleCommand, gFlushScratch = txScratch, usbGpuPump in the main loop), so it survives a
// regeneration from j716f.
//   gpu        the last result (ruan_gpu_short | ruan_gpu_row)
//   gpu <1-5>  queue that mode (4 = S2a: GMU clocks + read-only GMU/RSCC state; 5 = S2b step 1: GMU firmware check and
//              ITCM/DTCM load, GMU held in reset); gpu off = undo what 3/4 turned on. The USB thread's main loop runs it (usbGpuPump), outside pollUsbEvents: a traced run flushes
//              through pollUsbEvents and handleCommand itself runs inside it, so running there would re-enter the event ring
//              and handle the same event twice. Every register access is preceded by a "GPU rd|WR ..." line that usbFlush
//              has delivered (EP3 XFERCOMPLETE) before the access, so the last line the host holds names a hang or reset.
#ifndef RUAN_USB_DEBUG_GPU_H
#define RUAN_USB_DEBUG_GPU_H

extern "C" {
extern int ruan_gpu_run(unsigned mode, int (*trace)(void *, const char *), void *ctx) __attribute__((weak_import));
extern char ruan_gpu_row[128] __attribute__((weak_import));
extern char ruan_gpu_short[32] __attribute__((weak_import));
extern void ruan_gpu_set_firmware(const void *data, size_t size) __attribute__((weak_import));
extern void ruan_gpu_set_sqe_firmware(const void *data, size_t size) __attribute__((weak_import));
// The driver's record of what userspace did (NativeAdrenoPublish.cpp native_adreno_metal_diag; this view is UniXNU-sheng
// GpuPowerProbe.cpp's): the smoke tool's progress markers (open types 0x4147xxxx), its raw-test line and its Turnip line.
struct RuanAdrenoMetalDiagView {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status,
        ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16],
        chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17], reader[3][17], name_reader[17], starter[3][17], report[64], report2[64];
};
extern RuanAdrenoMetalDiagView native_adreno_metal_diag __attribute__((weak_import));
// Lines of processes other than the render server (the smoke tool's T line): NativeAdrenoPublish.cpp native_adreno_client_text.
struct RuanAdrenoClientTextView {char name[20]; char text[64];};
extern RuanAdrenoClientTextView native_adreno_client_text[8] __attribute__((weak_import));
// rbu50: what the GPU did with the submissions (RuanGpuPower.c registers; NativeAdrenoSubmit.cpp / NativeAdrenoRing.cpp records).
extern int ruan_gpu_fault_regs(char *out, size_t size) __attribute__((weak_import));
extern int ruan_gpu_va_read32(uint64_t gva, uint32_t *value) __attribute__((weak_import));   // RuanGpuPower.c, bank 2's current table
extern int ruan_gpu_reg_read(uint32_t reg, uint32_t *value) __attribute__((weak_import));
extern char ruan_gpu_hang_text[6][192] __attribute__((weak_import));   // RuanAdrenoGlue.cpp hangWatch
extern volatile uint32_t ruan_gpu_hang_seen __attribute__((weak_import));
extern volatile uint32_t native_adreno_submit_ibs[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log[4][4 + 28] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_fault[32] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall[2 + 22] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_mem[4 + 3 * 17 + 4] __attribute__((weak_import));
// rbu64: who opened the accelerator and why an open was refused (NativeAdrenoPublish.cpp): the newest opens, per-process grants and
// refusals, and the last mount failure (1 no address space, 2 the core's open, 3 the window scope; its IOReturn).
struct RuanAdrenoOpenRecordView {char name[20]; uint32_t type, macf, res, attach, foreign;};
extern RuanAdrenoOpenRecordView native_adreno_open_log[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_open_log_count __attribute__((weak_import));
struct RuanAdrenoClientRecordView {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern RuanAdrenoClientRecordView native_adreno_client_log[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
extern uint32_t ruan_adreno_marks_seen[0x7000 / 32] __attribute__((weak_import));   // LegacyDisplayProbe.cpp
// fb-gpu=1 hand-off (NativeGOPPresenter.cpp, LegacyDisplayProbe.cpp): presenter stage and its frame counters.
extern volatile uint32_t ruan_handoff_stage __attribute__((weak_import));
extern volatile uint64_t ruan_handoff_timeouts __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import)), native_gop_gpu_completions __attribute__((weak_import)),
    native_gop_gpu_failures __attribute__((weak_import)), native_gop_gpu_abandoned __attribute__((weak_import));
}

// `gpu 5`: the GMU firmware from the root (make-ramroot.py GPU_FIRMWARE), read once on this thread and kept for later runs.
static const char kGmuFirmwarePath[] = "/usr/standalone/firmware/ruan/gmu_gen70000.bin";
static uint8_t *gGmuFw;
static uint32_t gGmuFwBytes;
static int usbLoadGmuFirmware(char *buf, size_t cap, TerminalSession &s) {
    if (gGmuFw) return 0;
    vnode_t vp = nullptr;
    int err = vnode_open(kGmuFirmwarePath, FREAD, 0, 0, &vp, usbVfsContext());
    uint64_t size = 0;
    if (!err) err = usbVnodeSize(vp, &size);
    if (!err && (size == 0 || size > (1u << 20))) err = EFBIG;
    uint8_t *mem = err ? nullptr : static_cast<uint8_t *>(IOMalloc(static_cast<vm_size_t>(size)));
    if (!err && !mem) err = ENOMEM;
    // kFileChunk-sized reads, as usbFilePump (`cat`, whose bytes hash right) does; a short read ends with EIO.
    for (uint64_t off = 0; !err && off < size;) {
        const uint32_t want = (size - off) < kFileChunk ? static_cast<uint32_t>(size - off) : kFileChunk;
        uio_t uio = uio_create(1, static_cast<off_t>(off), UIO_SYSSPACE, UIO_READ);
        if (!uio) { err = ENOMEM; break; }
        uio_addiov(uio, CAST_USER_ADDR_T(mem + off), want);
        err = VNOP_READ(vp, uio, 0, usbVfsContext());
        const uint32_t got = want - static_cast<uint32_t>(uio_resid(uio));
        uio_free(uio);
        if (!err && !got) err = EIO;
        off += got;
    }
    if (vp) vnode_close(vp, FREAD, usbVfsContext());
    if (err) {
        if (mem) IOFree(mem, static_cast<vm_size_t>(size));
        snprintf(buf, cap, "GPU: %s: error %d\r\n", kGmuFirmwarePath, err);
        s.reply.writeStr(buf);
        return err;
    }
    gGmuFw = mem, gGmuFwBytes = static_cast<uint32_t>(size);
    snprintf(buf, cap, "GPU: %s: %u bytes read\r\n", kGmuFirmwarePath, gGmuFwBytes);
    s.reply.writeStr(buf);
    return 0;
}

// `gpu 10`: the SQE firmware from the root (a710_sqe.fw), read once on this thread and kept for later runs.
static const char kSqeFirmwarePath[] = "/usr/standalone/firmware/ruan/a710_sqe.fw";
static uint8_t *gSqeFw;
static uint32_t gSqeFwBytes;
static int usbLoadSqeFirmware(char *buf, size_t cap, TerminalSession &s) {
    if (gSqeFw) return 0;
    vnode_t vp = nullptr;
    int err = vnode_open(kSqeFirmwarePath, FREAD, 0, 0, &vp, usbVfsContext());
    uint64_t size = 0;
    if (!err) err = usbVnodeSize(vp, &size);
    if (!err && (size == 0 || size > (1u << 20))) err = EFBIG;
    uint8_t *mem = err ? nullptr : static_cast<uint8_t *>(IOMalloc(static_cast<vm_size_t>(size)));
    if (!err && !mem) err = ENOMEM;
    for (uint64_t off = 0; !err && off < size;) {
        const uint32_t want = (size - off) < kFileChunk ? static_cast<uint32_t>(size - off) : kFileChunk;
        uio_t uio = uio_create(1, static_cast<off_t>(off), UIO_SYSSPACE, UIO_READ);
        if (!uio) { err = ENOMEM; break; }
        uio_addiov(uio, CAST_USER_ADDR_T(mem + off), want);
        err = VNOP_READ(vp, uio, 0, usbVfsContext());
        const uint32_t got = want - static_cast<uint32_t>(uio_resid(uio));
        uio_free(uio);
        if (!err && !got) err = EIO;
        off += got;
    }
    if (vp) vnode_close(vp, FREAD, usbVfsContext());
    if (err) {
        if (mem) IOFree(mem, static_cast<vm_size_t>(size));
        snprintf(buf, cap, "GPU: %s: error %d\r\n", kSqeFirmwarePath, err);
        s.reply.writeStr(buf);
        return err;
    }
    gSqeFw = mem, gSqeFwBytes = static_cast<uint32_t>(size);
    snprintf(buf, cap, "GPU: %s: %u bytes read\r\n", kSqeFirmwarePath, gSqeFwBytes);
    s.reply.writeStr(buf);
    return 0;
}

static volatile unsigned gGpuPending;
static uint8_t *gFlushScratch;   // the main loop's txScratch; only the USB thread uses either

static void pollUsbEvents(TerminalSession &s);   // defined after handleCommand in UsbDebugCdc.cpp

// Sends everything in the reply ring and waits until the host has taken it (EP3 XFERCOMPLETE). Main-loop context only.
static bool usbFlush(TerminalSession &s, uint64_t timeoutMs) {
    const uint64_t end = nowMs() + timeoutMs;
    for (;;) {
        pollUsbEvents(s);
        if (!s.hw.configured || !s.hw.hostReady || !gFlushScratch) return false;
        if (!s.hw.ep3Busy) {
            if (s.reply.used == 0) return true;
            const uint32_t n = s.reply.pop(gFlushScratch, DMA_EP3_MAX_CHUNK);
            if (!startBulkIn(s.hw, gFlushScratch, n)) return false;
        }
        if (nowMs() > end) return false;
        IODelay(50);
    }
}

// ruan_gpu_run's trace: the line goes out and is acknowledged before the access it names; non-zero skips that access.
static int gpuTrace(void *ctx, const char *line) {
    TerminalSession &s = *static_cast<TerminalSession *>(ctx);
    s.reply.writeStr("GPU ");
    s.reply.writeStr(line);
    s.reply.writeStr("\r\n");
    return usbFlush(s, 2000) ? 0 : -1;
}

// handleCommand hook: true when raw was a gpu command (reply written).
static bool usbGpuCommand(TerminalSession &s, const char *raw, char *buf, size_t cap) {
    if (!strEq(raw, "gpu") && !strStarts(raw, "gpu ")) return false;
    const char *p = raw + 3;
    bool ok = false;
    uint32_t mode = parseUint(p, &ok);
    if (!ok && strEq(raw, "gpu off")) mode = 99u, ok = true;   // RuanGpuPower.c RUAN_GPU_OFF
    if (!&ruan_gpu_run) {
        s.reply.writeStr("GPU: not in this kernel (package/RuanGpuPower.c)\r\n");
    } else if (!ok) {
        snprintf(buf, cap, "GPU: last %s | %s\r\n[usb] METAL: stage=%u (status=%d) reg=%u opens=%u reads=%u prop='%s' cli='%s'\r\n",
                 &ruan_gpu_short ? ruan_gpu_short : "", &ruan_gpu_row ? ruan_gpu_row : "",
                 &ruan_metal_stage ? ruan_metal_stage : 0u,
                 &ruan_metal_status ? ruan_metal_status : -1,
                 &ruan_metal_registered ? ruan_metal_registered : 0u,
                 &native_adreno_metal_opens ? native_adreno_metal_opens : 0u,
                 &native_adreno_metal_property_reads ? native_adreno_metal_property_reads : 0u,
                 &native_adreno_last_property ? native_adreno_last_property : "",
                 &native_adreno_last_client ? native_adreno_last_client : "");
        s.reply.writeStr(buf);
        if (&native_adreno_metal_diag) {
            // TURNIP: K<markers> <newest 8, oldest first> (600a fill ok .. 6fff all passed, 6ffe failed; 4f01 open refused), the open
            // verdict (macf/resolver/type), R = the raw test's line, T = the Turnip line (UniXNU drivers/AdrenoMetal/NativeTurnipSmoke.h).
            const RuanAdrenoMetalDiagView &d = native_adreno_metal_diag;
            const unsigned total = d.marks_total, shown = total < 16 ? total : 16;   // the driver keeps 16
            size_t at = (size_t)snprintf(buf, cap, "[usb] TURNIP: K%u", total);
            for (unsigned i = 0; i < shown && at < cap; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", d.marks[(total - shown + i) & 15]);
            if (at < cap)
                snprintf(buf + at, cap - at, " | open %s macf %x res %x type %x calls %u | R %.63s | %.63s\r\n", d.opener[0], d.chk_macf,
                         d.chk_res, d.chk_type, d.calls, d.report, d.report2);
            s.reply.writeStr(buf);
            if (&ruan_gpu_fault_regs) {
                size_t at = (size_t)snprintf(buf, cap, "[usb] REGS");
                ruan_gpu_fault_regs(buf + at, cap - at - 3);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&ruan_gpu_hang_text && ruan_gpu_hang_text[0][0])
                for (unsigned i = 0; i < 6; ++i) {snprintf(buf, cap, "[usb] HANG%u %s\r\n", i, ruan_gpu_hang_text[i]); s.reply.writeStr(buf);}
            if (&ruan_gpu_va_read32 && &ruan_gpu_reg_read) {
                // rbu74: the IB the CP is in now. Its size comes from the CP_INDIRECT_BUFFER packet in the logged ring words that
                // names it (when logged), so the words around the CP's position are shown; otherwise the IB's first words.
                uint32_t lo = 0, hi = 0, rem = 0;
                if (!ruan_gpu_reg_read(0x928, &lo) && !ruan_gpu_reg_read(0x929, &hi) && !ruan_gpu_reg_read(0x92a, &rem)) {
                    const uint64_t base = (uint64_t)hi << 32 | lo;
                    uint32_t dwords = 0;
                    for (unsigned k = 0; &native_adreno_ring_log && k < 4 && !dwords; ++k)
                        for (unsigned i = 0; i + 3 < 112 && i + 3 < native_adreno_ring_log[k][3]; ++i) {
                            const volatile uint32_t *w = &native_adreno_ring_log[k][4 + i];
                            if (w[0] == 0x70bf8003u && w[1] == lo && w[2] == hi) {dwords = w[3]; break;}
                        }
                    const uint64_t at = dwords && rem <= dwords ? base + 4ull * (dwords - rem) : base;
                    const uint64_t from = at >= base + 4 * 12 ? at - 4 * 12 : base;
                    size_t n = (size_t)snprintf(buf, cap, "[usb] IBHANG ib1 %llx size %u rem %x at %llx:", (unsigned long long)base, dwords, rem,
                                                (unsigned long long)at);
                    for (unsigned i = 0; i < 24 && n < cap - 12; ++i) {
                        uint32_t v = 0;
                        const int e = ruan_gpu_va_read32(from + 4ull * i, &v);
                        n += e ? (size_t)snprintf(buf + n, cap - n, " !%d", e) : (size_t)snprintf(buf + n, cap - n, "%s%x", from + 4ull * i == at ? " >" : " ", v);
                    }
                    strlcat(buf, "\r\n", cap);
                    s.reply.writeStr(buf);
                }
            }
            if (&native_adreno_submit_ibs && &native_adreno_table_switches && &native_adreno_fence_wait) {
                snprintf(buf, cap, "[usb] SUBMIT n%u ibs%u max%u | tables cp%u cpu%u made%u freed%u live%u fail%x@%u | fwait %x %x %x %x %x %x %x %x\r\n",
                         native_adreno_submit_ibs[0], native_adreno_submit_ibs[1], native_adreno_submit_ibs[2], native_adreno_table_switches[0],
                         native_adreno_table_switches[1], native_adreno_table_switches[2], native_adreno_table_switches[3],
                         native_adreno_table_switches[4], native_adreno_table_switches[6], native_adreno_table_switches[7],
                         native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
                         native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
                s.reply.writeStr(buf);
            }
            // The last 4 submissions: IB (fence, iova, bytes, first 8 dwords with diag=1) and the ring words the CP reads for them.
            for (unsigned k = 0; &native_adreno_ib_log && &native_adreno_ib_log_count && k < 4 && k < native_adreno_ib_log_count; ++k) {
                const volatile uint32_t *e = native_adreno_ib_log[(native_adreno_ib_log_count - 1 - k) & 3];
                size_t at = (size_t)snprintf(buf, cap, "[usb] IB f%u %x%08x b%u:", e[0], e[2], e[1], e[3]);
                for (unsigned i = 0; i < 28 && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", e[4 + i]);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            for (unsigned k = 0; &native_adreno_ring_log && &native_adreno_ring_log_count && k < 2 && k < native_adreno_ring_log_count; ++k) {
                const volatile uint32_t *e = native_adreno_ring_log[(native_adreno_ring_log_count - 1 - k) & 3];
                size_t at = (size_t)snprintf(buf, cap, "[usb] RING f%u %u..%u n%u:", e[0], e[1], e[2], e[3]);
                for (unsigned i = 0; i < 24 && i < e[3] && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", e[4 + i]);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&native_adreno_stall_mem && &native_adreno_stall) {
                // The driver's own snapshot at its stall/hang detection (NativeAdrenoSubmit.cpp): SMMU FSR/FAR/FSYNR0, then 17 words each for
                // the BR IB1, BV IB1 and BV IB2 (start index, 16 words), then the next map handle at submission and the handles under the
                // three IBs: a handle at or above the first was mapped after the submission (freed and reused under the GPU).
                for (unsigned part = 0; part < 4; ++part) {
                    size_t at = (size_t)snprintf(buf, cap, "[usb] STALLMEM%u", part);
                    const unsigned first = part == 0 ? 0 : 4 + (part - 1) * 17, count = part == 0 ? 4 : 17;
                    for (unsigned i = 0; i < count && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", native_adreno_stall_mem[first + i]);
                    if (part == 3) for (unsigned i = 0; i < 4 && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " h%x", native_adreno_stall_mem[4 + 51 + i]);
                    strlcat(buf, "\r\n", cap);
                    s.reply.writeStr(buf);
                }
                size_t at = (size_t)snprintf(buf, cap, "[usb] STALL");
                for (unsigned i = 0; i < 24 && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", native_adreno_stall[i]);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&native_adreno_stall_fault) {
                size_t at = (size_t)snprintf(buf, cap, "[usb] STALLFAULT");
                for (unsigned i = 0; i < 16 && at < cap - 12; ++i) at += (size_t)snprintf(buf + at, cap - at, " %x", native_adreno_stall_fault[i]);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&ruan_handoff_stage && &native_gop_gpu_offers) {
                snprintf(buf, cap, "[usb] HANDOFF stage %x offers %llu done %llu failed %llu abandoned %llu timeouts %llu\r\n", ruan_handoff_stage,
                         (unsigned long long)native_gop_gpu_offers, (unsigned long long)native_gop_gpu_completions,
                         (unsigned long long)native_gop_gpu_failures, (unsigned long long)native_gop_gpu_abandoned,
                         &ruan_handoff_timeouts ? (unsigned long long)ruan_handoff_timeouts : 0ull);
                s.reply.writeStr(buf);
            }
            if (&ruan_adreno_marks_seen) {
                // Every marker ever seen in the plugin's families: 65xx-68xx (bridge, scanout, plugin) and 6Axx-6Exx (its diagnostics).
                size_t at = (size_t)snprintf(buf, cap, "[usb] SEEN");
                for (uint32_t v = 0x6500; v < 0x6f00 && at < cap - 8; ++v)
                    if (ruan_adreno_marks_seen[v >> 5] & (1u << (v & 31))) at += (size_t)snprintf(buf + at, cap - at, " %x", v);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&native_adreno_client_log && &native_adreno_mount_fail) {
                size_t at = (size_t)snprintf(buf, cap, "[usb] CLIENTS mountfail %u/%x x%u |", native_adreno_mount_fail[0], native_adreno_mount_fail[1],
                                             native_adreno_mount_fail[3]);
                for (unsigned i = 0; i < 8 && at < cap - 40; ++i)
                    if (native_adreno_client_log[i].name[0])
                        at += (size_t)snprintf(buf + at, cap - at, " %.12s o%u r%u c%u l%x", native_adreno_client_log[i].name,
                                               native_adreno_client_log[i].opens, native_adreno_client_log[i].refused,
                                               native_adreno_client_log[i].closes, native_adreno_client_log[i].last_refusal);
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            if (&native_adreno_open_log && &native_adreno_open_log_count) {
                const unsigned n = native_adreno_open_log_count, shown = n < 8 ? n : 8;
                size_t at = (size_t)snprintf(buf, cap, "[usb] OPENS %u:", n);
                for (unsigned k = 0; k < shown && at < cap - 48; ++k) {
                    const RuanAdrenoOpenRecordView &r = native_adreno_open_log[(n - 1 - k) & 7];
                    at += (size_t)snprintf(buf + at, cap - at, " %.12s t%x m%x r%x a%u", r.name, r.type, r.macf, r.res, r.attach);
                }
                strlcat(buf, "\r\n", cap);
                s.reply.writeStr(buf);
            }
            for (unsigned i = 0; &native_adreno_client_text && i < 8; ++i)
                if (native_adreno_client_text[i].name[0]) {
                    snprintf(buf, cap, "[usb] TEXT %.19s: %.63s\r\n", native_adreno_client_text[i].name, native_adreno_client_text[i].text);
                    s.reply.writeStr(buf);
                }
        }
    } else if ((mode < 1u || mode > 18u) && mode != 99u) {
        s.reply.writeStr("GPU: gpu 1..7 (S1/S2 GMU/clocks/SMMU), "
                         "8 (+GX on via OOB GPU_SET), "
                         "9 (+A710 HW init & stream-0 SMMU), "
                         "10 (+SQE fw & CP start to ME_INIT), "
                         "11 (+CP_MEM_WRITE smoke test), "
                         "12 (+secure mode exit), "
                         "13 (+2D blitter fill), "
                         "14 (+GOP FB blit), "
                         "15 (+CP_MEMCPY m2m blit), "
                         "16 (+CP_MEMCPY GOP FB DMA blit), "
                         "17 (boot-arg ruan-adreno=1: GX + hw_init, NativeAdreno context bank 2, CP init, fenced CP_MEM_WRITE), "
                         "18 (17 + native_adreno_publish), off\r\n");
    } else if (gGpuPending) {
        s.reply.writeStr("GPU: a run is already queued\r\n");
    } else {
        s.screen.streaming = false;   // nothing else in the reply ring while it runs
        gGpuPending = mode;
        snprintf(buf, cap, "GPU: mode %u queued; each line is sent before its register access\r\n", mode);
        s.reply.writeStr(buf);
    }
    return true;
}

// Main-loop hook: runs a queued mode once the host is configured and reading.
static void usbGpuPump(TerminalSession *s) {
    if (!gGpuPending || !&ruan_gpu_run || !s->hw.configured || !s->hw.hostReady || s->xfer.active || s->pendingFastboot) return;
    const unsigned mode = gGpuPending;
    char line[160];
    if (mode >= 5u && mode <= 18u) {   // the file read happens here, on the USB thread, outside pollUsbEvents
        if (usbLoadGmuFirmware(line, sizeof(line), *s) == 0 && &ruan_gpu_set_firmware) ruan_gpu_set_firmware(gGmuFw, gGmuFwBytes);
    }
    if (mode >= 10u && mode <= 18u) {
        if (usbLoadSqeFirmware(line, sizeof(line), *s) == 0 && &ruan_gpu_set_sqe_firmware) ruan_gpu_set_sqe_firmware(gSqeFw, gSqeFwBytes);
    }
    if (usbFlush(*s, 2000)) {
        const int rc = ruan_gpu_run(mode, gpuTrace, s);
        snprintf(line, sizeof(line), "GPU: mode %u done rc=%d\r\n", mode, rc);
    } else {
        snprintf(line, sizeof(line), "GPU: mode %u not started (could not flush to the host)\r\n", mode);
    }
    s->reply.writeStr(line);
    gGpuPending = 0;
}
#endif
