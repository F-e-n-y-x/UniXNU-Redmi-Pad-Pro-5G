# Speaker audio on iPadOS 17.7.11 for ruan: plan

Status 2026-11-21: research done. Stages 1–5 are written and syntax-checked (`port/`). **Nothing has been built, booted or run on the
tablet.** Stage 5 still produces no sound. Audible output needs stages 6–7 (section 4).

## 1. The answer

**The cheapest path is ADSP + AudioReach codec DMA RX-1 → LPASS RX macro → RX SoundWire → WCD937x EAR/AUX (analog) → 4× SIA81x9
amplifiers.** No shorter path exists on this board:

- The amplifiers take analog input. The only analog source is the WCD937x headphone/earpiece codec, so the codec has to be brought up.
- The codec is fed only by the RX macro over SoundWire, and the RX macro only by LPASS codec DMA.
- The DSP owns codec DMA (AudioReach), and the DSP's PRM owns the LPASS clocks and votes. Without the ADSP we cannot even read an LPASS
  register safely (the earlier clock-gated reset, README rbu24).

So the ADSP comes first. This is the order sheng uses (UniXNU-sheng: Xiaomi Pad 6S Pro, SM8550, AudioReach), and sheng plays speakers and
records on iPadOS through it. We reuse its ADSP host, GPR and AudioReach encoders.

The old research note `../audio-soundwire-wsa883x.md` is wrong for this tablet. The WSA883x nodes are not fitted: `qcom,wsa-max-devs = <0>`.
The FS1815 nodes are not fitted either: on Android `fsm_reg_read ... fail:-5`.

## 2. Evidence

**Verified (file and line):**

| Fact | Source |
|---|---|
| Speaker backend is `CODEC_DMA-LPAIF_RXTX-RX-1`, 48 kHz, 2 ch, 16 bit | `logs/2026-11-13-call/logcat.txt.gz`: `PAL: ... setDeviceMediaConfig: 968: CODEC_DMA-LPAIF_RXTX-RX-1 rate ch fmt data_fmt 48000 2 2 1` for `PAL_DEVICE_OUT_SPEAKER` |
| Speaker routing is the RX macro (RX2/RX3 MUX AIF2_PB, INT0/INT2) into WCD937x EAR and AUX DACs; `wcd937x_sia81xx_index=4` | `work/analysis-vendor/stock/vendor_a/etc/audio/sku_parrot/mixer_paths_parrot_qrd.xml`, path `speaker` |
| Amplifiers are SIA81x9 (`chip_type = 4`), channels 0–3, enable GPIOs 106/103/89/88 | Same log, dmesg: `sipa_resume ... rst = 476/473/459/458` (GPIO base 370). DT `si_pa_*` nodes (ruan-live.dts 26817–26861) |
| Amp I2C: R one/two 0x28/0x29 on `i2c@980000` (SDA 28, SCL 29); L on `i2c@a80000` (SDA 4, SCL 5) | ruan-live.dts 15296–15343, 15760–15843, pinctrl 0x19c/0x19d/0x1c5/0x1c6 |
| ADSP: `qcom,parrot-adsp-pas`, carveout 0x9a200000+0x2900000, rails `lcx.lvl`/`lmx.lvl`, SMP2P 443/429, pid 2, IPCC client 3 | ruan-live.dts 140, 5590ff, 6294, 7625–7700 |
| GPR on `adsp_apps` (APM 1, PRM 7 in DT; PRM port 2 as sheng found) | ruan-live.dts 7669–7693 |
| ADSP firmware is one image (`adsp.mdt` + b00–b41), no `adsp_dtb` | `mdir` of `stock/fastboot_rom/.../NON-HLOS.bin ::/image` |
| LPASS clock IDs: rx_core 0x30e @ 22.5792 MHz, rx_mclk2_2x 0x318 @ 19.2 MHz, tx 0x30c, va 0x307 | ruan-live.dts 26736–26812 |
| RX macro 0x3200000, RX SWR master 0x3210000 (2 devices), WCD937x RX slave `0x0a 0x1170224`, WCD reset GPIO 102 | ruan-live.dts 24699–24745, 12880 |
| WCD937x supplies: ldob20 (buck 1.8 V), ldob21 (rxtx/vddpx 1.8 V) | ruan-live.dts 5601, 5619 |
| LPASS stream for the apps SMMU: 0x1801 | ruan-live.dts 22024 (`msm-audio-ion`) |
| `audiomxd` is disabled as a crash-looper in our RAM root | `unixnu-ruan/scripts/make-ramroot.py:437-440` |

**Inferred:**

- The AudioReach codec DMA sink layout and that RX-1 = interface index 2. This is Linux `audioreach.h` (`CODEC_RX0 = 1`) by analogy
  with sheng's verified TX3 = 4.
- The device-graph container position (`POS_GLOBAL_DEV`).
- That SIA81x9 enables are active-high.
- That SIA register 0 answers.
- That `0x8bf00000` is free.
- That the DSP takes `pa | 1 << 32` for stream 0x1801, as sheng's 0x1001 does.

## 3. What iOS needs to route to it

- **Kernel side (stage 7):** an `IOAudio2Device` subclass with one output stream, published once stage 6 works.
  - UniXNU-t970 found that on iPadOS 18 (22H374) the audio server opens an AudioServerPlugIn but never calls StartIO. A kernel
    IOAudio2Device got the first StartIO: `UniXNU-t970/patches/xnu-ipados18/*pong-audio2.patch`, `iokit/Kernel/T970IOAudio2.cpp`, from
    Chengdong-Yu/UniXNU-pong `PongIOAudio2.h`. It has 433 lines and ready ring/StartIO/zero-timestamp logic.
  - iOS 17 is closer to 18 than to sheng's 27, so take this route first.
  - Needs `com.apple.iokit.IOAudio2Family` in the kernelcache. Our board.toml `include` has no kext list: check whether the prelinked
    kernelcache already carries it.
- **Fallback:** sheng's AudioServerPlugIn (`UniXNU-sheng/hal/ShengAudioPlugIn.c` + `ShengAudioService.cpp`, `ShengAudioAbi.h`). Two
  requirements, from sheng's porting.toml:
  - It only shows up with the Thunderbolt transport type.
  - It must correct CoreAudio's 24 MHz host-clock constant to 19.2 MHz. Ruan's timer is 19.2 MHz too.
- **Userland:** re-enable `com.apple.audiomxd` in `make-ramroot.py` (it SIGSEGVs without an audio device) once the device publishes.
  `mediaserverd` comes with it. No AppleEmbeddedAudio DT nodes are needed for either route (neither sheng nor pong adds any).

## 4. Stages (`ruan-audio=N`, default 0; each failure only records `-step`)

| N | Does | Touches |
|---|---|---|
| 1 | ADSP SMEM/SMP2P survey, TLMM config of 9 audio pins | reads only |
| 2 | ADSP boot (PAS 1), GLINK, `adsp_apps`, APM ready | SMEM, IPCC, AOSS, RSC TCS 1, carveout, SCM |
| 3 | PRM macro + dcodec votes, RX clocks; then 2 RX macro reads only if all 4 answers are 0 | PRM, LPASS 0x3200000 (guarded) |
| 4 | SIA81x9 enables high, bit-banged I2C read of reg 0 on 4 amps, everything restored | TLMM 4,5,28,29,88,89,103,106 |
| 5 | SMMU bypass 0x1801, map 64 KiB, codec DMA RX-1 + stream graphs, 100 silent periods, close | SMMU, DSP |
| 6 | **to write:** RX macro path (mixer_paths `speaker` → registers via Linux `lpass-rx-macro.c`), RX SWR master enumerate (sheng `micBringUp` TX-master code, RX instance), WCD937x power-up/EAR+AUX PA (Linux `sound/soc/codecs/wcd937x.c`, ldob20/21 via RuanWifiWpss `voteVrm`, reset GPIO 102), SIA enable + `sipa.bin` register set (vendor `firmware/sipa.bin`, sipa_dlkm.ko) | audible tone |
| 7 | **to write:** IOAudio2Device (port T970IOAudio2.cpp) + audiomxd back | iOS playback |

## 5. Risks

- **Bus faults:**
  - Any LPASS read before the PRM votes can reset the tablet. Stage 3 reads only after four status-0 answers.
  - SWR master registers also need the macro's SWR clock gate. Stage 6 must set the RX macro clock control before it reads `0x3210000`,
    as sheng found for TX/VA.
- **Hypervisor:**
  - Sheng found that programming a translating SMMU context reset the board. We only use the bypass bank, as the ruan UFS, USB and Wi-Fi
    code does.
  - The SMMU is trapped: plain LDR/STR only (RuanWifi.cpp).
- **Shared owners:** the ADSP host and the Wi-Fi WPSS host both own IPCC/QMP/TCS 1/SMEM mutex, so `ruan-audio` refuses when
  `ruan-wifi>=2`. To run both, merge the two hosts on one thread (asteroids' AsteroidsRemote design).
- **Metadata page:** the PAS metadata falls back to 0x8bc00000, the same page the Wi-Fi fallback uses (fine while the two are
  exclusive).
- **Amplifiers:** amp enable polarity and SIA register map are unverified. Stage 4 writes no amp register, and the speakers are idle with
  no analog input. Speaker protection (the DSP `libsixth_sia81xx_rx` module) is absent: keep gain low in stage 6.
- **ADSP crash:** the crash reason goes to `native_adsp_text` and the boot continues. PAS shutdown runs on failure.
