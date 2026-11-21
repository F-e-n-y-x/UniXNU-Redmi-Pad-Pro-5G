# Adreno 710 performance: DDR votes, multi-level DCVS, governor

Patch: `ruan-gpu-perf.patch` (diff against `unixnu-ruan/package/RuanGpuPower.c`, apply in `package/` with `patch -p1`).
Tags: **[V]** = verified against source or the live DT; **[I]** = inferred, needs a device check.

## 1. Why the GPU is slow today

`ruan_build_hfi_tables()` sends the GMU one active GPU level and one bandwidth row with SH0/MC0 = `0x60000001`
(commit|valid, y = 1 unit). y = 1 is the smallest non-zero vote, so the GPU's DDR path stays at the floor
whatever the core clock is. Raising `ruan-gpu-khz` without bandwidth only gives a faster core that waits on memory. [V: code]

## 2. BCM DDR vote formula (KGSL gen7)

Source: `/root/unixnu-work/gpu-dev/src-ref/adreno_gen7_rpmh.c` (OnePlus SM8475 KGSL, the vendor driver that matches our GMU fw) [V]:

- BCMs in this order: `SH0` (buswidth 16), `MC0` (buswidth 4), `ACV` (fixed). CNOC: `CN0` (buswidth 4). [V]
- cmd-db per BCM: `addr` = record field +16; aux data = 8 bytes `{le32 unit, le16 width, u8 vcd, u8 reserved}`. [V]
- Per level with `ib` = `qcom,bus-table-ddr7[i]` in KB/s, `ab` = 0:
  - `y = ib * width / buswidth`, then `y = y * 1000 / unit`; if ib != 0 and y == 0, y = 1; clamp to 0x3fff. `x = 0` (ab = 0).
  - ACV (fixed): `y = 0x8` when ib != 0, otherwise invalid.
  - `data = BCM_TCS_CMD(commit, valid, x, y)` = `commit<<30 | valid<<29 | x<<14 | y`; valid = (x|y) != 0.
  - `commit` = last BCM, or `vcd[i] != vcd[i+1]`; `ddr_wait_bitmask` gets bit i for each commit BCM.
- The bandwidth table has one row per entry of `qcom,bus-table-ddr7` (9 rows, row 0 = 0 KB/s). Each pwrlevel picks a row with `qcom,bus-freq-ddr7`. [V]
- CNOC: `qcom,bus-table-cnoc = <0 100>` gives 2 CN0 rows. [V]
- DDR type: root `ddr_device_type = <7>` (LPDDR4X), so use the `ddr7` tables. [V: dts:23]
- msm-drm `a6xx_build_bw_table` for A7xx sets x = y (both). KGSL leaves x at 0. The patch follows KGSL. [V]

The current code's `res[6]` "SH2" is used as the CNOC address. KGSL votes `CN0`. The patch looks up CN0 (in perf mode only). [V: code; whether SH2 was deliberate is unknown]

## 3. ARC (GX corner) votes

KGSL `setup_volt_dependency_tbl` [V]: primary `gfx.lvl` index k = the first entry >= vlvl (if none: **fail**, do not clamp); `cur` = that value;
secondary `mx.lvl` index j = the first entry >= `cur`, else the last **non-padding** entry. `vote = cur<<16 | j<<8 | k`.
The existing `ruan_arc_vote()` clamps a missing corner to the top and can pick a zero-padding mx slot. The patch adds a
KGSL-exact `ruan_arc_vote_kgsl()` for perf mode and leaves the old one unchanged.
`cx_vote` = 0xffffffff for every level (no `qcom,cx-level` in the DT). [V]

## 4. Table for ruan (speed-bin 0, LPDDR4X)

| HFI perf idx | MHz | `qcom,level` (vlvl) | bus idx | DDR KB/s |
|---|---|---|---|---|
| 0 | off | 0 | 0 | 0 |
| 1 | 295 | 0x38 (LOW_SVS_D1) | 2 | 2136718 |
| 2 | 345 | 0x40 (LOW_SVS) | 2 | 2136718 |
| 3 | 500 | 0x80 (SVS) | 4 | 3972656 |
| 4 | 600 | 0xc0 (SVS_L1) | 5 | 5285156 |
| 5 | 650 | 0xe0 (SVS_L2) | 6 | 6074218 |
| 6 | 734 | 0x100 (NOM) | 7 | 6671875 |
| 7 | 816 | 0x140 (NOM_L1) | 8 | 8332031 |
| 8 | 875 | 0x180 (TURBO) | 8 | 8332031 |
| 9 | 940 | 0x1a0 (TURBO_L1) | 8 | 8332031 |

Bus rows 1 and 3 (781250 and 3000000 KB/s) exist only for bus-only votes. [V: dts:16151-16290; 875 MHz is exact, not 876]
**Speed bin [I]:** the DT has bins 0x00 (940), 0xbe (900 at 0x1a0), 0xb2 (top 875), 0x8f (top 676 at 0x100), 0x82, 0xc6.
The patch hard-codes bin 0, which fits the 7s Gen 2 spec (940 MHz), but the fuse (qfprom 0x119 bits 5..12) has not been read.
Check it on Android first (`/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies`). On bin 0x8f/0x82, levels 6 and up would be overclocked.

The ARC/BCM numeric votes depend on cmd-db contents we have not dumped. The patch prints every computed row
(`gpu perf: L.. vote`, `gpu perf: bw.. sh0/mc0/acv`). Check those lines on the first boot. [I]

## 5. HFI messages (gen7, ids [V] from KGSL/existing code)

- `H2F_MSG_BW_VOTE_TBL` = 3 (159 words: count, cnoc_n, ddr_n, cnoc_wait, ddr_wait, cnoc_addr[6], cnoc_data[2][6], ddr_addr[8], ddr_data[16][8]).
- `H2F_MSG_PERF_TBL` = 4 (58 words: gpu_n, gmu_n, gx[16]{vote, cx_vote, kHz}, cx[4]{vote, kHz}).
- `H2F_MSG_GX_BW_PERF_VOTE` = 30: `{ack_type (1 = DCVS_ACK_BLOCK), freq = perf index, bw = bus index}`. Use 0xFF (`INVALID_DCVS_IDX`)
  to leave one of them unchanged (KGSL `gen7_gmu_dcvs_set`). KGSL never votes perf 0 (that is XO/off). The same message is used for
  every runtime change after `H2F_MSG_START`. Send it while holding an OOB/GPU-active state, as KGSL does. [V]
- `H2F_MSG_PREPARE_SLUMBER` = 33 `{freq, bw}` only matters once slumber/IFPC is used (not yet). [I: id from adreno_hfi.h memory]

## 6. Patch behaviour

- Default (no `ruan-gpu-perf`): byte-identical tables and the `{1,1,1}` vote, as before. `ruan-gpu-corner`/`ruan-gpu-khz` still apply.
- `ruan-gpu-perf=1`: 10 GPU levels + 9 bw rows built as above. The boot vote is `{1, N, bus(N)}`, with N from `ruan-gpu-level=N` (1..9, default 6 = 734 MHz / bw7).
  If any input is missing (gfx corner, an 8-byte BCM aux, CN0), it logs this and falls back to the old one-level tables.
- A host self-test of the new functions (fake cmd-db) passed: padding handling, the commit mask, and refusal of a bad aux. It has not been built for XNU or run on the device.

## 7. Governor proposal (port of sheng/amethyst `gpuDcvsThread`)

1. Busy counter (KGSL `gen7_start` [V]): `GPU_GMU_AO_GPU_CX_BUSY_MASK` (0x23b0e) = 0xff000000; `GMU_CX_GMU_POWER_COUNTER_SELECT_0` (0x1f841) low byte = 0x20;
   `..._ENABLE` (0x1f840) = 1. Read `XOCLK_0_L/H` (0x1f844/0x1f845, 19.2 MHz ticks while busy; read hi, lo, hi again).
2. Every 16 ms: busy% = ticks*52.083 ns / window. `need = busy * curMHz * 2` (keep it 50 % busy). Go up at once to the lowest level >= 1.1 x need.
   Go to the ceiling when busy is over 85 % or the GPU has been busy for 50 ms straight. Go down one level after 4 windows in a row that need <= 0.8 x the next lower level.
   Drop to the idle level (1) after 80 ms with no submissions. A touch boost holds the ceiling for 1 s.
3. Each change sends Msg 30 `{1, L, bus(L)}`. On an HFI error, keep the old level and count it.
4. Thermal cap [I]: gpuss-0/1 are tsens sensors 11/12 on phandle 0x168 (tsens-v2 at 0xc263000/0xc222000 or 0xc265000/0xc223000).
   The DT trips are 95 / 115 / 125 °C passive. Proposal: cap the ceiling at level 6 at 85 °C, at level 3 at 95 °C, and raise it again 5 °C lower. Until tsens is read, set the default ceiling to level 7 (816 MHz), as amethyst kept off its top three levels.
5. Boot-args: `ruan-gpu-dcvs=1`, `-min`, `-max`, `-idle`, mirroring `sheng-gpu-dcvs-*`.

## 8. Risks

- **Wrong ARC corner → GX brown-out / GMU hang / SError.** A clock above what its corner supports is the classic hard hang. Never pair a kHz with a lower vlvl.
  The patch takes both from the same DT row, and it fails (rather than clamps) when gfx.lvl lacks the corner.
- Speed bin unknown (see §4). Start with `ruan-gpu-level=6`, then 7, before trying 8 or 9.
- MX dependency: if mx.lvl tops out below 0x1a0, KGSL (and the patch) votes mx at its top index. That matches the vendor driver.
- DDR votes that are too high only cost power. Votes too low (today's) cost speed. A bad BCM *address* could hit another client's BCM: the addresses come from cmd-db by name.
- ACV y = 0x8 on all active rows follows the KGSL gen7 code (the same as today's 0x60000008).
- No thermal throttling exists yet. Sustained 940 MHz with no cap risks hitting the 125 °C hardware trip (reset).
