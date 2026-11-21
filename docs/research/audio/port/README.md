# Audio port for ruan: stages 1–5 (ADSP, PRM, amplifiers, DSP data path)

Status 2026-11-21: syntax-checked with the flags of ruan's last XNU build, and the host tests pass. **Not built into a kernel, not booted,
not run.** No stage makes sound yet; see `../plan.md` for stages 6–7.

## Files

| Path | What it is |
|---|---|
| `package/RuanAudio.cpp` | The staged bring-up. `ruan-audio=N` gates it. It replaces `RuanAudioProbe.c`, keeping the names `ruan_audio_init` and `ruan_audio_status_str`. |
| `package/RuanAdsp.cpp` | UniXNU-sheng `c42e9a0` `package/AdspProbe.cpp`, ported to ruan. Diff: `patches/sheng-adspprobe-to-ruanadsp.diff`. |
| `package/ShengGpr.{c,h}`, `ShengAdsp.h` | Sheng's files, byte for byte (`cmp`-checked). |
| `package/ShengAudioReach.{c,h}` | Sheng's files plus codec DMA RX sink builders and `ar_open_playback_stream_to`. Diff: `patches/sheng-to-ruan.diff`. |
| `package/tests/*_host.c` | Sheng's host tests, plus checks for the new builders (236 + 37 checks pass). |
| `check/syntax-check.py` | `-fsyntax-only` with the kernel flags from the compile records, then the host tests. Writes nothing outside a temp dir. |
| `patches/audio-port.patch` | `scripts/build-ram-root-probe.py`: swaps the stub for the four files. `patch --dry-run` is clean on 2026-11-21. |
| `scripts/stage-adsp-firmware.py` | RAM root only: merges `image/adsp.*` from `NON-HLOS.bin` into `OUT/firmware/qcom/sm7435/ruan/adsp.mbn`. Refuses to write inside a git tree. |

## Apply (in the session that owns `macOS/unixnu-ruan`)

1. Copy the package files: `cp port/package/{RuanAudio.cpp,RuanAdsp.cpp,ShengGpr.c,ShengGpr.h,ShengAdsp.h,ShengAudioReach.c,ShengAudioReach.h} unixnu-ruan/package/`
2. Patch the build script: `cd unixnu-ruan && patch -p1 < ../docs/research/audio/port/patches/audio-port.patch`
   - `RuanAudioProbe.{c,h}` can then be deleted.
3. Sync to `/root/unixnu-work/UniXNU-ruan` and build as usual.
4. Firmware:
   - **UFS variant:** read in place from modem_a.
   - **RAM root:** run `scripts/stage-adsp-firmware.py <NON-HLOS.bin> /root/unixnu-work/audio-fw` and build with `DATA_OVERLAY` pointing at
     that directory.

## Test

Do one RAM-only `fastboot boot` per stage, after clearing it with the user and the ROM session. Use `KERNEL_ARGS="ruan-audio=N"`, and
leave out `ruan-wifi`, or keep it at 1 or below. Run USB `audio` about 3 minutes after boot. The line has this shape:
`[usb] AUDIO: st S err E | adsp st A err .. slave .. items .. kib .. ready ..ms glink ../.. ch .. | apm ..ms answers .. op .. | prm P rx X/Y | amps M regs R nak K | smmu .. map .. periods n/m | ..ms | note`.

| N | Pass | Fail (no reset expected; the line says where) |
|---|---|---|
| 1 | `st 10`, `adsp st 2`, `slave ffffffff` or `0` (DSP not running), `items` with bit `100` | `st -11` (TLMM map), `adsp st -1` (windows) |
| 2 | `st 20`, `adsp st 13`, `ch` bit 0 set, `apm <ms>` | `adsp st -3` + `fw: modem:adsp` (firmware), `-6` QMP, `-7` rails, `-9` load, `-10` auth, `-11` + `crash: …` (DSP fatal), `st -22` (channel never up), `st -23` (APM not ready); `adsp st -1 err -16`: ruan-wifi≥2 |
| 3 | `st 30`, `prm 0`, `rx` two hex values (not `0/0` before the read) | `st -31 err <prm>`: a byte per request; `ff` no answer. No LPASS register was read. |
| 4 | `st 40`, `amps f`, `regs` 4 bytes | `st -41 err <mask>`: which amps did not ACK, `nak` count. Pins are restored. |
| 5 | `st 50`, `smmu 1`, `map <non-zero>`, `periods 100/100` | `st -52` SMMU, `-53` map, `-54..-56` graph open/config (err = DSP status), `-57` prepare/start, `-58` timeout (`err` = periods returned) |

Stage 5 passing proves the DSP half: the graphs, the LPAIF RXTX RX-1 DMA running, and the buffers crossing the SMMU. The speakers stay
silent until stage 6.
