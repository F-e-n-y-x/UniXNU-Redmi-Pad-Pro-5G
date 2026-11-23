// SPDX-License-Identifier: MIT
// ruan GPU step 4 (macOS/docs/research/GPU-METAL-PORT.md): the Turnip smoke test as a root daemon, UniXNU's
// drivers/AdrenoMetal/NativeAdrenoProbeDaemon.cpp without the log export. It stands in for /usr/bin/IOMFB_FDR_Loader (a RunAtLoad
// root job of the original launchd.plist, as sheng's probe daemon did), so the signed launchd.plist stays untouched;
// libvulkan_freedreno.dylib sits beside it (turnip::libraryPath). smoke() runs the raw test of NativeAdrenoUserTest.h (open, BO map,
// IB submit, fence), then NativeTurnipSmoke.h (Vulkan fill, compute, render, read back); every step reaches the kernel as a progress marker and the two
// result lines (R ..., T ...), which the USB link's `gpu` command prints.
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   // adrenoUserTest: smoke() below runs its steps in another order
#include "NativeAdrenoUserTest.h"
#pragma clang diagnostic pop

// adrenoUserTest's open and channel probe, then the raw test in the order write-combined, uncached, write-combined (rbu44: uncached
// first passed 4/4 and the write-combined rerun 1/4, the GPU's writes unseen; this order tells a cache-mode fault from a stale
// GPU translation of the re-used addresses), reported as "V wc<n> uc<n> wc<n>" in R; then the Turnip phase whatever they gave.
static int smoke(unsigned port) {
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return 1;
    Gpu gpu;
    gpu.openFn = (IOServiceOpenFn)dlsym(iokit, "IOServiceOpen");
    gpu.closeFn = (IOServiceCloseFn)dlsym(iokit, "IOServiceClose");
    gpu.call = (CallFn)dlsym(iokit, "IOConnectCallStructMethod");
    gpu.map = (MapFn)dlsym(iokit, "IOConnectMapMemory64");
    gpu.unmap = (UnmapFn)dlsym(iokit, "IOConnectUnmapMemory64");
    gpu.port = port;
    gpu.mark(1);
    if (!gpu.openFn || !gpu.closeFn || !gpu.call || !gpu.map || !gpu.unmap) return 1;
    const int opened = gpu.openFn(port, mach_task_self(), 0x4147, &gpu.conn);
    if (opened || !gpu.conn) {gpu.mark(0x4f01); return 5;}
    gpu.mark(3);
    if (!gpu.probe() && !gpu.openMailbox()) {gpu.closeFn(gpu.conn); return 6;}
    int v[3], step[3] = {}, error[3] = {};
    v[0] = runTest(gpu, 0x401, step[0], error[0]);
    v[1] = runTest(gpu, 0x101, step[1], error[1]);
    v[2] = runTest(gpu, 0x401, step[2], error[2]);
    char text[64];
    snprintf(text, sizeof text, "V wc%d uc%d wc%d s%d/%d/%d e%x/%x/%x", v[0], v[1], v[2], step[0], step[1], step[2], error[0], error[1],
             error[2]);
    streamText7(gpu, text);
    gpu.mark(0x5000 | (v[0] & 0xf) << 8 | (v[1] & 0xf) << 4 | (v[2] & 0xf));
    gpu.closeFn(gpu.conn);
    turnipPhase(gpu);
    // What the three read-backs held (patches/smoke): fill words 0/511 (pattern c0ffee42), 512/1023 (deadbeef kept); compute words 0/1
    // (1000/1003); render pixel 0 and the centre (clear ff0000ff blue, triangle ff00..ff orange; 5a5a5a5a = never written).
    snprintf(text, sizeof text, "D f%x/%x/%x/%x c%x/%x r%x/%x", turnip::gDump[0], turnip::gDump[1], turnip::gDump[2], turnip::gDump[3],
             turnip::gDump[4], turnip::gDump[5], turnip::gDump[6], turnip::gDump[7]);
    // Only when the fill went wrong: R is also the render server's row (the plugin's scanout P line), which a passing run leaves alone.
    if (turnip::gDump[0] != 0xc0ffee42u) streamText7(gpu, text);
    return 0;
}

using MatchingFn = void *(*)(const char *);
using GetServiceFn = unsigned (*)(unsigned, void *);

int main() {
    // tu-debug=<flags> (boot argument, commas) overrides the first-light default: system memory rendering, no UBWC, no concurrent binning.
    char args[1100] = {}, flags[64] = "sysmem,noubwc,nocb";
    size_t size = sizeof args - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0)
        for (const char *at = args; (at = strstr(at, "tu-debug=")); at += 9)
            if (at == args || at[-1] == ' ') {
                strlcpy(flags, at + 9, sizeof flags);
                flags[strcspn(flags, " ")] = 0;
                break;
            }
    setenv("TU_DEBUG", flags, 1);
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return 1;
    auto matching = (MatchingFn)dlsym(iokit, "IOServiceMatching");
    auto nameMatching = (MatchingFn)dlsym(iokit, "IOServiceNameMatching");
    auto getService = (GetServiceFn)dlsym(iokit, "IOServiceGetMatchingService");
    if (!matching || !nameMatching || !getService) return 2;
    // The accelerator comes with the display (LegacyDisplayProbe.cpp); its opens are served once gpu 18 published the driver
    // (NativeAdrenoGpu), which the boot's deferred glue thread does after init exists. Up to two minutes for both.
    unsigned port = 0, gpu = 0;
    for (unsigned i = 0; i < 240 && !(port && gpu); ++i) {
        if (!gpu) gpu = getService(0, nameMatching("NativeAdrenoGpu"));
        if (!port) port = getService(0, matching("IOAcceleratorES"));
        if (!(port && gpu)) usleep(500000);
    }
    if (!port || !gpu) return 3;
    return smoke(port);
}
