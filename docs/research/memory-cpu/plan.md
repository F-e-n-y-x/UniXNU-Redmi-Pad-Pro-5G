# Memory and CPU placement on ruan (iPadOS 17.7.11, xnu-10063 BCM2837 config)

2026-11-21. Code changes made, nothing built or run. [V] = verified in source, a build tree or a device log. [I] = inferred.
Logs: `/root/unixnu-work/logs/rbu170p..rbu185u`. Source: `ufs-variant/out/ruan/.../ruan_ufs/tree/.cache/xnu-work`.

## A. Memory

### What XNU manages

| Item | Value | |
|---|---|---|
| Page size | 4 KiB (BCM2837 config, not iOS' 16 KiB). `vm_page_bootstrap: 689145 free pages` in the 2816 MiB window | [V] |
| DarwinBoot window | 0x8c007c000 + 2816 MiB (`owned_memory_size = 0xB0000000`) | [V] rbu185u |
| memory-outside=1 (default arg) | 0x800000000-0x8bc000000, `698624 pages adopted, 0 refused` = 2729 MiB | [V] |
| max_mem after adoption | ~5545 MiB (ml_adopt_outside_memory raises max_mem/sane_size/mem_actual) | [V] patch |
| Not used | low bank 0x80000000-0x100000000 (2 GiB: firmware, Gunyah, PIL, GOP scanout, TZ; also beyond the 4 GiB aperture reach), HYP 256 MiB, the 6 + 65 MiB holes, the 16 MiB GPU pool, and ~256 MiB above the window (0x97007c000-0x980000000; only one side is adopted) | [V] board.toml / [I] the last item |

So ~5.4 of 8 GiB. The only cheap gain left is the ~256 MiB above the window: test `owned_memory_size = "0xBF000000"` if DarwinBoot
does not print `NO WINDOW` (UEFI allocates top-down, so it may hold part of that run) [I]. Both sides at once does not work with the
current patch: `pa_index()` checks the outside range first, so a range that spans the window would misindex window pages [V].

Sized before adoption (at KMEM/ZALLOC, from max_mem = 2816 MiB) [V]: the compressor pool (`compressor_pool_size = max_mem`, so at
most ~2.75 GiB of uncompressed pages) and the zone wired cap (37.5 % = ~1 GiB). Neither is near its limit in any log. Leave them;
`vm_compression_limit=<pages>` exists but must stay below the 3456 MiB compressor VA range.

### Why `[hb] free=` is ~1200-4400

`free=` is `vm_page_free_count` in 4 KiB pages [V BootProgress.c]. The pageout daemon keeps it at `vm_page_free_target`, which on
!OSX is capped at **2000 pages, min 1500, reserved 100** (`vm_pageout.c:246-249`) [V]. Apple tuned those for 16 KiB pages (32 MB
target); on ruan they are 8 MB. So the floor is the VM working as designed with a 4x too small target, not a leak [V code, I effect].
Free drops from 1.06 M pages at 36 s to 2004 at 67 s while UFS reads go 3.9k -> 21k: mostly file cache plus userland start-up [I].
Too small a target means more `VM_PAGE_WAIT` stalls on bursts (GPU BOs, Wi-Fi DMA) and contiguous runs almost never exist [I].

### Jetsam

- `[ev] exit … reason ns 1` (OS_REASON_JETSAM) appears **only in rbu83c** (old): codes 4 (VM page shortage), 5 (proc thrashing),
  7 (per-process limit, mediaserverd). None in rbu170p-rbu185u [V grep].
- Thresholds are percentages of max_mem, computed in memorystatus_init after adoption: critical 4 % ≈ 220 MB [V].
- `ruan-memlimit-floor` default 512 MB (ledger + memorystatus active/inactive limits): keep. No highwater kills since it landed [V].
  Big-app limits are above 512 MB, so only small daemons/extensions are raised. Raise to 768 only if `code 0x7` returns.
- Compressor: `vm_compressor_mode 8` (with swap), freezer mapped to COMPRESSOR_NO_SWAP. No swap file is ever created in the logs, and
  Data is not a disk worth swapping to. Nothing to enable [V log, I value].

### GPU / DMA wired memory

- GPU pool 16 MiB at 0x8B8B00000, kept out of the VM (board.toml) [V]. Wi-Fi `dma 35248K` ≈ 0.6 % of RAM: not the problem [I].
- `gpu-bo-contig` is **off** (not in `args-current.txt`), so the 5 s back-off path costs nothing today [V]. Keep it off. Retest
  `gpu-bo-contig=64` only with the bigger free target below; the kernel's contiguous search scans the free list and was 163 ms median
  per failure (NativeAdrenoMemory.cpp comment).
- Display carveouts (splash 0xB8000000) are in the low bank, not VM [V].

### Changes made (memory)

1. **`patches/xnu/ruan-vm-free-target.patch`** (new, in `ufs-own-root.series`). `vm_page_free_reserve()` multiplies the three caps by
   `ruan-vm-free-scale` (default `16384/PAGE_SIZE` = 4: target 8000 pages = 32 MB, min 6000, reserved 400+compressor). `=1` = stock.
   Logs `ruan-vm: free reserved .. min .. target .. (scale 4, 4096-byte pages)`. Default **on** (only restores the iPad's bytes).
   Dry-run applies on the ruan_ufs tree.
2. **`package/BootProgress.c` heartbeat**: `[hb] … free= tgt= avail= wire= act= inact= spec= cmp=` (avail =
   `memorystatus_available_pages`, what jetsam reads; cmp = pages in the compressor). Next boot tells where the 4 GiB goes.

## B. CPU placement

### State [V]

- ADT gets `cluster-type` E for cpu0-3, P for cpu4-7 from `sm7435.dtsi` (`unixnu,cluster-type`, devicetree.py:392). ruan.dts only adds
  `unixnu,die-cluster-id`, which devicetree.py drops. rbu138u: cpu0-3 `cluster_id 0 type 1`, cpu4-7 `cluster_id 1 type 2`.
- `native-smp-type-clusters.patch`: `__ARM_AMP__`, 2 clusters, Edge scheduler; after `ml_cpu_init_completed` FIXPRI/FG/IN/DF prefer P,
  UT/BG prefer E. There is no separate sched_amp config; `ml_get_cluster_type` follows the ADT type.
- ArmCpu calls `ml_cpu_init_completed` only with `armcpu-edge=1`. It is in `args-current.txt` now (rbu185u). The A78 domain (d1)
  hits its 90 °C cap repeatedly in rbu185u, so the A78s do carry the load [I].
- Boot CPU = cpu0 (A55, pset0); every SPI goes to the boot core (ArmCpu `gBootAffinity`).

### Changes made (CPU)

1. **`armcpu-edge=1` in the built-in defaults** (`ruan-default-bootargs.patch`). Default on, already device-run. The command line still wins (`armcpu-edge=0`).
2. **`patches/xnu/ruan-cpu-placement.patch`** (from `docs/research/cpu-placement/`, in `ufs-own-root.series`). All off by default:
   - `ruan-pcores=1`: backboardd + SpringBoard (and children) get the P-cluster task hint (`TF_USE_PSET_HINT_CLUSTER_TYPE`), so their UT threads go to P too.
   - `ruan-cpu0-park=1`: idle spinner bound to cpu0, so E spill lands on cpu1-3. Costs power: cpu0 never idles.
   - `ruan-cpu-report=N`: `ruan-cpu: busy/1000 c0..c7 | boot E edge 1` and the UI tasks' ms on P.
   The foreground app is FG/IN QoS, which already prefers P through armcpu-edge. A per-app hint would need RunningBoard state.

### Suggestions for other owners (not done)

- **ArmCpu (UniXNU driver)**: route SPIs to cpu4 (boot-arg e.g. `armcpu-spi-aff=0x400`) so touch/vsync/GPU IRQs and their wakeups run on an A78.
- **RuanCpuFreq.c**: more P load means more 90 °C caps. Use a stepped cap (one OPP down) rather than on/off at 903/839 dC, and keep the P floor clock up while backboardd is runnable.
- **bootcpu=7** (DarwinBoot + devicetree.py, the HakiOS way): pset0 becomes P. Large change; only if the above falls short.

## Test boot-args (RAM-only boots, owner session)

| Run | Add | Check |
|---|---|---|
| 1 | (defaults) | `ruan-vm: … target 8000 (scale 4…)`; `[hb] free=` settles near 8000; `avail=` vs `wire=` split |
| 2 | `ruan-vm-free-scale=1` | A/B: free back to ~2000; compare app-launch stalls (`ruan-slow`) |
| 3 | `ruan-cpu-report=5` | `edge 1`; c4-c7 load while scrolling |
| 4 | `ruan-cpu-report=5 ruan-pcores=1` | `backboardd … on P` ≈ cpu; fps (display counters), d1 caps |
| 5 | 4 + `ruan-cpu0-park=1` | marginal; watch d0 clock and heat |
| 6 | `gpu-bo-contig=64` (with run 1) | `native_adreno_bo_contig` made vs refused |
| — | `armcpu-edge=0` | rollback of the new default |

Not compiled: watch `proc_best_name`/`snprintf` declarations in sched_prim.c and the new externs in BootProgress.c (`memorystatus_available_pages` is `unsigned int` under CONFIG_JETSAM).
