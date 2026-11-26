# ruan macOS 27.3 thermal policy (prepared, not deployed)

`ruan-thermal.patch`: apply in `/root/unixnu-work/UniXNU-ruan-macos` with `patch -p1` (the dry run on 2026-11-26 was clean).
Nothing was built or run, and nothing was committed or run on the tablet.

Checks run on the host:
- `cc -DRUAN_THERMAL_HOST -DRUAN_THERMAL_SELFTEST package/RuanThermal.c` passes: tsens decode, hysteresis steps and cap map.
- The NTC table was checked on its own: 938 mV gives 25.0 C, 883 mV gives 27.5 C.
- `RuanThermal.c`, `RuanCpuFreq.c` and `RuanBattery.c` pass `clang -fsyntax-only` against stub headers.
- `RuanThermalLevel.cpp` has not been compiled. It is a cut-down copy of UniXNU-sheng's `ThermalLevel.cpp`.

## Sensors

| Source | Address / register | Where found | State |
|---|---|---|---|
| TSENS0 | TM `0x0c263000` (SROT 0x0c222000). Sensors: aoss0 0, cpuss 1-2, **A78 cpu-1-0..7 = 3..10**, **gpuss 11-12**, camera 13 | `macOS/docs/hardware/private/ruan-live.dts` 17113 and thermal-zones 9904-10371 | The iPadOS `ruan-cpufreq=3` cap reads it (README CPU row: "thermal cap") |
| TSENS1 | TM `0x0c265000`. Sensors: aoss1 0, **A55 cpu-0-0..3 = 1..4**, nsp 5-7, video 8, ddr 9, mdm 10-13 | dts 17123 and 10392-10847 | Same register layout; never read yet |
| TSENS word | `TM + 0xa0 + 4n`: bits 11:0 signed deci-C, bit 21 = valid, up to 3 reads | tsens-v2.c, iPadOS `RuanCpuFreq.c` `ruan_tsens_dc` | Verified (source) |
| LMh | EPSS `domain_state` `+0x20` (L x 19.2 MHz) against our `perf_state` vote `+0x320`, domains `0x17d91000`/`0x17d92000` (`dcvsh0/1_int` = SPI 30/31). Delivered below the vote = throttled; upstream `qcom_lmh_get_throttle_freq` reads the same register | dts 6715, `RuanCpuFreq.c` | Verified (source) |
| PMIC die | temp-alarm STATUS `0x08`, subtype `0x05`; PM7250B `0x824` (SID 8, `0x2400`) and PM6150L `0x424`. Gives a **stage 0-3**, not degrees | dts 8931/8224, Linux `qcom-spmi-temp-alarm.c` | Bit layout recalled from memory, **[I]** |
| Battery NTC | PM7250B VADC `0x831`, channel `0x4a` (`bat_therm`, 100k, ratiometric, 200 us settle). mV = code x 1875 / 0x70e4; table from B 4250 K | dts 9013, `qcom-spmi-adc5.c` | **[I]**. It writes to the VADC, so it is gated by `ruan-batt-therm=1` and needs the arbiter owner = EE 0 |
| JEITA | CHGR `0x100D` (BATTERY_CHARGER_STATUS_7), raw | `smb5-reg.h` | **[I]** |
| GPU / GMU | No GMU temperature found; only TSENS gpuss-0/1 | iPadOS `RuanGpuPower.c` 2027 | — |
| Skin (quiet/xo/sys therm) | ADC_TM 0x3400/0x3500/0x3600. Reading these needs a VADC conversion (`get_temp` goes through IIO) | dts 7983, 8330, 8465 | Skipped |

The charger is controlled by the ADSP (`qcom,pmic-glink`; `qpnp-smb5` and `qg` are disabled in the DT). Battery temperature is therefore report-only.

## Policy (`ruan-thermal`, default 2; 1 = report only, 0 = off; 1 s thread in `RuanThermal.c`)

Each step lifts 5 C under its threshold. "No reading" counts as step 0.

| Group | Step 1 | Step 2 | Step 3 = emergency |
|---|---|---|---|
| A78 (d1) | 80 C: one level under top | 90 C: middle level | 100 C: both CPU domains lowest, plus a log line every 10 s |
| A55 (d0) | — | 90 C: middle level | 100 C: as above |
| GPU | 85 C | 95 C | 105 C |
| Battery | 45 C | 50 C | 60 C (log only) |

- **GPU: there is no cap.** The macOS perf table has a single level (734 MHz). A GPU cap needs `docs/research/opt/ruan-gpu-perf.patch` and `ruan-gpu-dcvs.patch` first. Both are broken against macOS `RuanGpuPower.c` (2 hunks fail).
- **Pressure level** = the highest step of any group: 0 nominal, 1 moderate, 2 heavy, 3 trapping.

## Telling macOS (`thermal-level=1`, default off)

On Apple Silicon, `thermalmonitord` polls `AppleSMCSensorDispatcher` once a second, using selectors 0-2. It posts the key `mTPL` as `com.apple.system.thermalpressurelevel`, and Foundation reads that as `NSProcessInfo.thermalState`. This was reverse-engineered by sheng on the same build, 26C5035r. `RuanThermalLevel.cpp` publishes that service without an SMC.

`IOPMrootDomain`'s thermal-warning path only covers the legacy warning level, so it was not used **[I]**.

## Test plan (USB terminal)

1. Boot with no new boot-args. `cmd thermal` should show:
   - all tsens rows reading 30-50 C;
   - `invalid 0`;
   - `d1 vote ... state ...`.
2. `dmesg` should show `ruan-thermal: mode 2, tsens0 mapped, tsens1 mapped`. If the first boot panics on the read, retry with `ruan-thermal=0`.
3. Put a CPU load on (Safari/WebGL, or `yes` processes) and watch `cmd thermal` every 10 s:
   - expect `THROTTLED` / LMh seconds;
   - expect `cap 0 -> 1` at 80 C and level 1.
4. `thermal-level=1`: expect `macOS service st=1`, and reads rising once a second with the `thermalmonitord` pid.
5. `ruan-batt-therm=1`: the `battery` line should show `temp=NNC`, and `dmesg` should show the VADC owner. Compare against the room / Android value. If the owner is not EE 0, the temperature stays unknown.

## Risks / unchecked

- **TSENS read at EL1 under Gunyah.** It worked on iPadOS. tsens1 has never been read.
- **The `bat_therm` conversion could race the ADSP** if the ADSP converts on the same ADC (filtered out, see the shortcut comment).
- **`kits/ruan` sync is not checked.** The build compiles from `kits/ruan/`, so the 2 new files must be copied there the same way the other package files are.
