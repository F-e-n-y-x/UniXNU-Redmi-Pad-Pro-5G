// SPDX-License-Identifier: BSD-2-Clause-Patent
// RuanAudioProbe: Qualcomm LPASS SoundWire & Quad WSA883x Smart Amplifiers probe for ruan.
#include "RuanAudioProbe.h"
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <string.h>

struct ruan_audio_telemetry g_ruan_audio = {0};

void ruan_audio_init(void)
{
    unsigned enable = 1;
    PE_parse_boot_argn("ruan-audio", &enable, sizeof(enable));
    if (!enable) {
        strlcpy(g_ruan_audio.status, "disabled by bootarg", sizeof(g_ruan_audio.status));
        return;
    }

    g_ruan_audio.probed = 1;
    g_ruan_audio.swr1_mapped = 0;
    g_ruan_audio.swr4_mapped = 0;
    snprintf(g_ruan_audio.status, sizeof(g_ruan_audio.status),
             "LPASS audio: stub, no hardware access");

    IOLog("ruan-audio: %s\n", g_ruan_audio.status);
}

const char *ruan_audio_status_str(void)
{
    return g_ruan_audio.status[0] ? g_ruan_audio.status : "uninitialized";
}
