# CPU placement on ruan: UI work on the Cortex-A78s

2026-11-21. Research draft. Nothing here was built, flashed or run.
[V] = verified (in source, in a build output or in a device log). [I] = inferred (not checked on the device).

## TL;DR

- XNU already knows the clusters. cpu0-3 are an E cluster (A55) and cpu4-7 a P cluster (A78), with two psets and the Edge scheduler. [V]
- But nobody calls `ml_cpu_init_completed()`. ArmCpu calls it only with **`armcpu-edge=1`**, and our args don't have it. So the
  Edge matrix stays all-closed and every thread group keeps preferring **pset0 = the boot cluster = the A55s**. The A78s get
  almost no work: only threads bound there or placed there before they first block. [V code, I effect]
- Fix #1 is one boot-arg, `armcpu-edge=1`. j716f runs the same OS (21H461), the same `native-smp-type-clusters.patch` path and
  E/P split, and reports it "device-verified". [V j716f README/porting.toml]
- `ruan-cpu-placement.patch` (an XNU patch, `osfmk/kern/sched_prim.c` only, off by default) adds three boot-args:
  `ruan-pcores=1` (backboardd and SpringBoard get a P-cluster task hint), `ruan-cpu0-park=1` (the picasso/sheng park), and
  `ruan-cpu-report=N` (per-CPU load and backboardd's P-core time in the kernel log). It passes
  `patch -p1 --dry-run -d .../ruan_ufs/tree/.cache/xnu-work`. [V dry-run; not compiled]

## 1. Current state

### Topology XNU sees

| Item | Value | Source |
|---|---|---|
| ADT `/cpus` | 8 nodes, `reg` 0x000..0x700. cpu0 `state=running`, the rest `waiting`. `cluster-type` 'E' for cpu0-3, 'P' for cpu4-7 | [V] `ruan_ufs/device-tree/hardware.dtb` (from `sm7435.dtsi` `unixnu,cluster-type`); `UniXNU/scripts/unixnu/devicetree.py:381-399` |
| Not exported | `unixnu,die-cluster-id` / `unixnu,cluster-core-id` from ruan.dts (devicetree.py drops them; XNU falls back to MPIDR) | [V] devicetree.py |
| Kernel config | `MAX_CPUS 8`, `MAX_CPU_CLUSTERS 2`, `__ARM_AMP__ 1` -> `CONFIG_SCHED_EDGE` | [V] xnu-work `board_config.h:137,166-168`, `sched.h:200-204` |
| Cluster id | no `HAS_CLUSTER`, so `phys_cluster_id = (type == P)`: E = cluster 0, P = cluster 1 | [V] `machine_routines.c:1097` |
| Boot CPU | the ADT node with `state=running`, always the first enabled node (cpu0, an A55). DarwinBoot enters XNU on the core UEFI runs on (cpu0) | [V] `devicetree.py:388`, `machine_routines.c:966-980`, `DarwinBoot.c:1147-1161` |
| On device (rbu138u) | `ml_processor_register` cpu0-3 "cluster_id 0 ... type 1" (E), cpu4-7 "cluster_id 1 ... type 2" (P), "pset_create(cluster_id=1) returned pset 1" | [V] `/root/unixnu-work/logs/rbu138u/dmesg-1.txt:150-167` |

So pset0 is the E cluster (the boot cluster) and pset1 is the P cluster. `processor_bootstrap()` makes `pset_node0` = AMP_E and
`pset_node1` = AMP_P (processor.c:207-213). [V]

### Why the A78s sit idle

1. `sched_clutch_bucket_group_init()` sets every bucket group's preferred cluster to `pset0.pset_cluster_id`, which is the E
   cluster. [V]
2. `native-smp-type-clusters.patch` moves FIXPRI/FG/IN/DF to the P cluster (UT/BG stay on E). It does that **only inside
   `sched_edge_cpu_init_completed()`**, which also opens the edges: P->E migration with weight 64, steal allowed. E->P stays
   closed. [V]
3. That hook runs only through `ml_cpu_init_completed()`. In our tree the only caller is `ArmCpu::startThread`
   (`UniXNU/drivers/ArmCpu/ArmCpu.cpp:713-727`), and only `if (... edge != 0)`, where edge is boot-arg `armcpu-edge`
   (default 0). `AppleARMSMP` is not linked; ArmCpu is the CPU driver (`components.toml`). [V]
4. `args-current.txt` / `args-rbu138.txt` have `cpus=8 smp=1 smp-gic=1` and **no `armcpu-edge`**. [V]
5. Result: every edge has `sce_migration_allowed = 0` (from `sched_edge_pset_init`), and every thread prefers E.
   `sched_edge_migrate_candidate()` returns false on a closed edge (sched_clutch.c:4194), and RT threads always go to their
   preferred pset (4491). So backboardd, SpringBoard, the CA render threads and the IOMFB copy work loop all run on cpu0-3.
   The A78s only get threads that land there through binding or initial placement. [I effect; V code path]
6. Interrupts: ArmCpu routes every SPI to the boot core (`GICD_IROUTER` = `gBootAffinity`, ArmCpu.cpp:1304). So touch, GPU,
   DPU and USB IRQs all run on cpu0 (A55). [V]
7. CLPC is absent, and nothing restricts the recommended cores (all 8 recommended). `ruan-cpufreq=3` clocks each domain by its
   busiest CPU (RuanCpuFreq.c:199-215), so an idle A78 domain also runs at a low clock. [V]

### Where backboardd and SpringBoard run

They are ordinary timeshare/RT threads in their own thread groups. Today they **prefer the E cluster** (pset0) and have no path
to the A78s (point 5). With `armcpu-edge=1` they prefer the P cluster for FIXPRI/FG/IN/DF, and RT threads (FIXPRI bucket) go
to P. Only UT/BG threads stay on E. [I: thread QoS of backboardd's render thread not checked on device]

This also explains why picasso/sheng's `cpu0-park` gained 40 -> 55 fps. In their setup, without cluster types the lowest idle
core (the little boot core) got work first, so taking cpu0 away helped (`UniXNU-sheng/package/BootProgress.c:1186-1213`).
`docs/research/community/display-fps-plan.md` says "board.toml sets none" (no cluster types). That is out of date: the dtsi
sets them. [V]

## 2. Options, ranked (gain per effort and risk)

| # | Option | Effort | Risk | Expected effect |
|---|---|---|---|---|
| 1 | **`armcpu-edge=1`** (boot-arg only) | none | low: the same OS and patch path run on j716f [V]. The ArmCpu comment says the matrix fails a consistency check on iPadOS 27, which is not our 17.7.11 | Everything above UT runs on the A78s, P->E spill when they are full. Likely the biggest single gain [I] |
| 2 | **`ruan-pcores=1`** (patch) | small | low: uses the existing DEVELOPMENT `TF_USE_PSET_HINT_CLUSTER_TYPE` path that `sched_edge_choose_processor` honors before the RT check (sched_clutch.c:4477) | Pins all backboardd/SpringBoard threads to P, including UT ones, and works without #1. Children inherit the hint (task.c:1765) |
| 3 | Route SPIs to an A78 (ArmCpu `gBootAffinity` -> e.g. 0x400, or a boot-arg `armcpu-spi-aff`) | small, but in ArmCpu (UniXNU driver, not ours) | medium: the per-core SGI/redistributor setup must not assume boot core == SPI target | Touch/vsync/GPU IRQ handling and the wakeups it causes run on a fast core [I] |
| 4 | **`ruan-cpu0-park=1`** (patch) | done | low. Cost: cpu0 is always 100 % busy, so `ruan-cpufreq=3` holds domain 0 at its top clock (power/heat) | After #1 it only helps for P->E spill (spill then picks cpu1-3, not the IRQ core). Small gain [I]. Without #1 it makes the P cluster no more reachable, because the edges stay closed |
| 5 | Boot XNU on cpu7 | large: DarwinBoot + devicetree.py | medium/high | pset0 becomes the P cluster, so the default preference is P even without #1, SPIs go to cpu7 automatically, and early boot runs ~2.5x faster [I] |

Not proposed: raising QoS/priorities in userland (no CLPC to honor work-interval hints, and #1/#2 already place by bucket and
by task). Custom thread-group recommendation code is also not needed, because the type-clusters patch already sets the
CLPC-like defaults once #1 runs.

### Option 5 in detail: start XNU on cpu7

- Feasible under Gunyah PSCI. HakiOS on SM7435 (garnet, same SoC and Gunyah) runs `bootcpu=7` in every boot option. Its loader
  PSCI-`CPU_ON`s core 7 at `other_cpu_entry`, which enters the kernel "exactly as core 0 would have", while UEFI stays parked on
  core 0 (`vg/hakios-xnu-kernel/xnuloader/loader.c:775-783, 2994-3018`, `vg/hakios-sm7435/bootargs.md:17`). Our own
  secondaries also come up through PSCI `CPU_ON` (smp=1). [V source/doc; not on ruan]
- Needed in DarwinBoot (`UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c` around `DarwinEnter`, after
  `ExitBootServices`):
  1. Mark ADT cpu7 `state=running` and cpu0 `waiting`. Do it in `devicetree.py` with a `boot_cpu` option, because it hardcodes
     the first node as running at :388, or patch the ADT in DarwinBoot. Keep the node order: cpu ids follow DT order, so cpu7
     stays cpu_id 7 (XNU supports a non-zero boot cpu, machine_routines.c:1045-1051, 1180-1188).
  2. Clean the kernel/ADT/args to PoC (DarwinEnter already flushes the window), then `CPU_ON(0x700, trampoline, args)`. The
     trampoline runs with MMU off at EL1 and sets x0 = boot args, then branches to the entry.
  3. cpu0 then either does `PSCI CPU_OFF`, so ArmCpu can `CPU_ON` it later as an ordinary secondary, or spins in WFE forever
     with ADT cpu0 disabled and `cpus=7` (the HakiOS way). The HakiOS way is safer, because `CPU_OFF` of vCPU0 under Gunyah is
     untested here. [I]
- XNU side: nothing more. `processor_bootstrap` makes pset0 = AMP_P, `thread_bind_cluster_type` and the default preferences
  follow pset0, and ArmCpu reads the boot core's MPIDR for SPI routing. [V code; I runtime]
- Risks: the cpufreq governor and boot-progress assume cpu0-3 = domain 0 by XNU cpu number. That still holds, because ids follow
  DT order. Panics before the console comes up give no log. Rollback is RAM-only `fastboot boot`.

## 3. The patch: `ruan-cpu-placement.patch`

File: `macOS/docs/research/cpu-placement/ruan-cpu-placement.patch`. One hunk appended to `osfmk/kern/sched_prim.c`, inside
`#if (DEVELOPMENT || DEBUG) && __AMP__`. Our kernel is `DEVELOPMENT_ARM64_BCM2837` [V dmesg]. It reuses `find_pset_of_type()`
and `enable_task_set_cluster_type` from the same file. Started by `STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, ...)`.

| Boot-arg | Does |
|---|---|
| `ruan-pcores=1` | Sets `enable_task_set_cluster_type=1`. A kernel thread then scans `tasks` once a second (references taken under `tasks_threads_lock`, the task lock taken after it is dropped). For backboardd and SpringBoard it sets `TF_USE_PSET_HINT_CLUSTER_TYPE` with `pset_hint` = the P pset, exactly as `sysctl_task_set_cluster_type('P')` does for the caller. Logs `ruan-cpu: backboardd[47] -> P cluster (pset 1)` |
| `ruan-cpu0-park=1` | A priority-1 thread bound to `master_processor` spins on `yield` (sheng's `cpu0_park`). Only when the boot cluster is E |
| `ruan-cpu-report=N` | Every N s: `ruan-cpu: busy/1000 c0 .. c7 .. \| boot E edge 1` (per-CPU busy from recount idle time; `edge` = 1 once `sched_edge_cpu_init_completed` opened P->E, i.e. armcpu-edge took effect) and per UI task `ruan-cpu: backboardd[47] cpu 1234 ms, on P 1200 ms, hint P` (`recount_task_times_perf_only`) |

Apply: `patch -p1 -d <xnu-work> < ruan-cpu-placement.patch`. Copy it to `macOS/unixnu-ruan/patches/xnu/` and list it in
`ufs-own-root.series` (and `series` for the RAM build) to adopt it. Not compiled. Watch for `strcmp`/`snprintf` declarations
(libsa/string.h, misc_protos.h, both reachable from sched_prim.c) and `proc_best_name` (declared locally the way vm_pageout.c
does).

Tests: none written. This is device-only scheduling code. The report line is the check.

## 4. Device verification (owner's tablet session; RAM-only boot)

A/B, all with the current set (`cpus=8 smp=1 smp-gic=1 ruan-cpufreq=3 ...`):

| Run | Added boot-args |
|---|---|
| A (baseline) | `ruan-cpu-report=5` |
| B | `armcpu-edge=1 ruan-cpu-report=5` |
| C | `armcpu-edge=1 ruan-pcores=1 ruan-cpu-report=5` |
| D | C + `ruan-cpu0-park=1` |

Checks per run:
1. `bootargs` over usbdbg shows them (the line is ~525 bytes now, limit 1024).
2. usbdbg `dmesg`:
   - `ml_processor_register ... cluster_id 1 ... type 2` for cpu4-7 (already true).
   - `ruan-cpu: ... edge 1` in B-D, `edge 0` in A.
   - `ruan-cpu: busy/1000 ...`: in A expect c4-c7 near 0 and c0-c3 loaded; in B-D expect c4-c7 to carry the load while
     scrolling/animating.
   - `backboardd[..] cpu X ms, on P Y ms`: the ratio of the deltas between two lines is the share on A78s. Expect ~0 in A,
     most of it in B, ~all in C.
3. Frame rate: the existing `display` usbdbg counters (fps / present rate) and the live camera (iphone.jpg :8091) while
   swiping the home screen; compare A vs B vs C.
4. Thermal: `ruan_cpufreq_temp_dc` / `ruan_cpufreq_capped`. More A78 load can now reach the 90 C cap; D also holds domain 0 at
   its top clock.

Expected (inferred): B gives most of the gain, C adds a little for backboardd's UT work, D is marginal. If B shows `edge 1` but
c4-c7 stay idle, the default preferences did not reach the existing thread groups. Then check
`thread_group_set_default_preferred_clusters` with a printf, or rely on C.

## Sources

- XNU (patched): `/root/unixnu-work/ufs-variant/out/ruan/ipados-17.7.11-21H461/ruan_ufs/tree/.cache/xnu-work`
  (`osfmk/arm64/machine_routines.c`, `osfmk/kern/{processor.c,sched_clutch.c,sched_prim.c,task.c}`)
- Patches: `macOS/unixnu-ruan/patches/xnu/native-smp-{secondary,type-clusters}.patch`, `soc.series`
- ArmCpu: `/root/unixnu-work/UniXNU/drivers/ArmCpu/ArmCpu.cpp`, `README.md`
- Built ADT: `.../ruan_ufs/device-tree/hardware.dtb`; feed dtsi `UniXNU/out/feeds/UniXNU-sm7435-common-*/dts/sm7435.dtsi`
- Boot args: `/root/unixnu-work/args-current.txt`, `args-rbu138.txt`
- Device log: `/root/unixnu-work/logs/rbu138u/dmesg-1.txt`
- References: `UniXNU-sheng/package/BootProgress.c:1186-1213` (cpu0-park), `refs/UniXNU-j716f/{README.md,porting.toml}`
  (armcpu-edge=1 on 21H461), `vg/hakios-sm7435/bootargs.md`, `vg/hakios-xnu-kernel/xnuloader/loader.c` (bootcpu),
  `refs/darwin-lisa/kernel/soc/smp/README.md`
- `cpu0-park` / `sheng-cpu0-park` / `bootcpu` / `ruan-pcores`: no reader in our tree (`macOS/unixnu-ruan`) or in UniXNU. Only
  UniXNU-sheng (and refs Dizi-Ruan/gts7xlwifi/aston) read `cpu0-park`. [V grep; `cpu0-park` string absent from the
  `ruan_ufs` kernel.macho, `armcpu-edge` present]
