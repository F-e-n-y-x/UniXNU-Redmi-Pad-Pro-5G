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
        snprintf(buf, cap, "GPU: last %s | %s\r\n", &ruan_gpu_short ? ruan_gpu_short : "", &ruan_gpu_row ? ruan_gpu_row : "");
        s.reply.writeStr(buf);
    } else if ((mode < 1u || mode > 5u) && mode != 99u) {
        s.reply.writeStr("GPU: gpu 1 (read), 2 (+cfg_ahb), 3 (+CX power-up, clocks, SMMU), 4 (+GMU clocks, GMU state), "
                         "5 (+GMU firmware check, ITCM/DTCM load, GMU stays in reset), off\r\n");
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
    if (mode == 5u) {   // the file read happens here, on the USB thread, outside pollUsbEvents
        if (usbLoadGmuFirmware(line, sizeof(line), *s) == 0 && &ruan_gpu_set_firmware) ruan_gpu_set_firmware(gGmuFw, gGmuFwBytes);
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
