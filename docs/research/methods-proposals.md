# ruan (SM7435) iPadOS/macOS port: method review and proposals

Date: 2026-11-14. Scope: research only. Nothing here was run on the tablet, zima or the network.
Inputs read: `macOS/README.md` (iPadOS sections), `UniXNU-ruan/`, `UniXNU/drivers/DarwinBoot`, `UniXNU/drivers/RamBlock`,
`UniXNU/patches/xnu/offline-owner-systemos-mount.patch`, refs (j716f, gts7xlwifi, Dizi-Ruan = sheng/SM8550, fuxi, amethyst,
nabu), `/root/hakios-garnet/xnuloader/loader.c`, the ruan live DT (`macOS/hardware/private/ruan-live.dts`) and
`stock/info/partitions.txt`.

"User OK" below means the user's explicit approval. Anything that writes tablet storage needs it. Anything that reboots
the tablet also needs a word with the ROM session (AGENTS.md).

---

## 0. The short version (candid)

1. **Logs come first.** The current loop gets about one bit per 5–10 minute cycle, and only if a photo catches the right
   line. Test 40 (aks3) is the proof: the one line that mattered (`ram-data: ... error N`) scrolled away, so the cycle
   told us nothing. Every reference port that got to a home screen had a machine-readable log channel before userland
   work: j716f (ramoops + USB CDC), gts7 (UFS log window + USB CDC), sheng (UFS log), nabu (ESP log over ADB), and HakiOS
   on ruan itself (loader stays resident and serves the log over USB). Do no more Data/launchd experiments until P1 exists.
2. **The Data volume design deviates from every working reference.** Data is a second volume inside the root container on
   md0, mounted "by role". That path is macOS-only code (`CONFIG_ROSV_STARTUP`) forced on for iOS, plus an APFS ioctl that
   refuses non-kernel callers. Every 17.x port that reached SpringBoard (amethyst on the same 21H461 build, nabu, j716f,
   gts7) mounts Data as **its own container on its own block device**, with a plain `mount("apfs", "/private/var",
   {spec=/dev/X})`. ruan's patch already has that branch (`ram-data-dev=`). It only lacks a second device. See P2.
3. **The full-RAM-root route is the outlier.** fuxi is the only reference that boots its whole OS from RAM. It stalled at a
   watchdogd panic and never reached a home screen. amethyst (same build, RAM-only `fastboot boot`) reads the root from a
   partition and keeps only Data in RAM. A 2.3 GB root in RAM costs the 110 s upload, RAM headroom (the jetsam kill in
   test 35) and a hand-written APFS (two device cycles were already lost to mkapfs.py bugs: spaceman, resource map).
4. **Everything runs on one Cortex-A55 (cpu0) at an unknown clock.** `board.toml` has `cpus=1`, and cpu0 is a little
   core. ROOTFS goes green at 225–357 s. HakiOS has already run 7 cores with cpufreq on this tablet (`bootcpu=7 cpus=8
   garnet_cpufreq=1`). Slow boots stretch every cycle and invite the watchdog and jetsam failures you are seeing.
5. **Compressing the upload is not worth it** (measured on `out-data2/root-usb.apfs`: zstd -1 shrinks 2386 MB to 1996 MB,
   −16 %; lz4 to 2044 MB; only 212 MiB of the image is zero blocks). The win is not sending it at all (P3).
6. Side note: the HakiOS loader runs on ruan wrote to the **`logdump` partition**
   (`logs/2026-11-13-first-xnu-run.log`: "log: 24576 bytes written to logdump"; `loader.c` `LOG_PARTITION u"logdump"`).
   That is a storage write. It is harmless in practice (Xiaomi's own dump partition), but tell the user and the ROM
   session, and turn it off if HakiOS runs again without an OK.

---

## Ranked proposals (value / effort)

| # | Proposal | Effort | Odds | Needs user OK |
|---|---|---|---|---|
| P1 | ramoops log carry + DarwinBoot `upload` + auto-return to fastboot | 1–2 days | high (80 %) | no storage write. Reboot loop: coordinate with the ROM session |
| P2 | Data as its own container on `md1` (split the RAM disk), seeded like amethyst/nabu | 0.5–1 day, 1–2 cycles | high (75 %) | no |
| P3 | Stop re-sending the root: (a) RAM reuse with CRC, (b) root on `testparti` read by UEFI | (a) 0.5 day, (b) 1 day | (a) 50 %, (b) 85 % | (a) no. (b) **yes**: writes sda11 |
| P4 | Boot on big cores: SMP plus EPSS cpufreq, or at least the boot CPU's clock | 1–2 days | 70 % | no |
| P5 | Host-side APFS checks (apfsprogs `apfsck`, Apple-parity diff vs RRD), or Apple-made images from a macOS CI runner | 0.5–1 day | 60 % catch rate | no |
| P6 | Fully scripted cycle on zima (watch for fastboot, stage, boot, fetch log; content-addressed root) | 0.5 day | 95 % | coordinate reboots |
| P7 | Off-device userland in QEMU (UniXNU-qemu, TCG on this x86 PC) | 1–2 days setup | 40 % (speed unknown) | no |
| P8 | Resident-loader method from HakiOS (UEFI stays on core 0, XNU on core 7, live log, `oem stop`, resident root) | 1–2 weeks | 50 % | no |
| P9 | XNU USB CDC-ACM (QcomXnuUsbDebug / j716f UsbDebugCdc on DWC3 0x0a600000, SMMU stream 0x80) | 3–5 days | 45 % (Gunyah/SMMU) | no |
| P10 | Faster USB receive in DarwinBoot (bigger chunk, no copy, check USB3) | 0.5 day | 60 % for ≥1.5× | no |

P1, P2 and P6 together turn the loop into: build → script → read a text log, with no photos and the user only needed for
approvals. Do those three first, in that order.

---

## P1. Log carry through a warm reset (ramoops zone) + fetch from DarwinBoot + auto-return to fastboot

**Why it works on ruan.** The stock DT reserves ramoops at `0x80D00000 + 0x200000` (`ruan-live.dts:244`, record 256 KiB,
console 512 KiB, pmsg 1 MiB, no ECC). Linux's ramoops layout puts the two dmesg records first, so the **console zone is at
`0x80D80000`, 512 KiB**. Stock Android relies on that DDR surviving XBL and ABL across a warm reset. ruanPkg's
`MemoryMapLib.c` does not describe `0x80C01000–0x82A00000`, so neither UEFI nor XNU's window touches it.

**What already exists (port, don't write):**
- `refs/UniXNU-j716f/package/BootRamoops.c`: the zone writer ("DBGC" persistent_ram_buffer: sig, start, size, ring),
  an early buffer, a thread that copies new `msgbufp` text every 100 ms, the panic hook (`boot_progress_panic_text`
  from `scripts/xnu-panic-screen.c`), and restart-after timers (`*-restart-after`, `*-panic-restart-after`,
  `*-restart-crashes`). The zone is in Linux's format, so Android's pstore can read it too.
- `refs/UniXNU-Dizi-Ruan/package/BootReset.c` + `BootResetCore.h`: writes "bootloader" into the PMIC SDAM reboot-reason
  cell through the SPMI arbiter, then PSCI reset. It has a host test model of the arbiter. **ruan's addresses are
  identical to sheng's:** arbiter cnfg `0xc42d000`, core `0xc400000`, chnls `0xc500000`, obsrvr `0xc440000`
  (`ruan-live.dts:7844-7847`). PMK8350 is SID 0, SDAM `0x7100`, cell `0x48` bits 7:1 (`ruan-live.dts:8021-8032`).
  So the port is close to a straight copy.
- `UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver/UsbRamdisk.h` `UsbRdServe()`: the fastboot server that every cycle
  already passes through. It answers getvar/download/continue/reboot. An `upload` command (the host's
  `fastboot get_staged FILE`) is about 25 lines: reply `DATA%08x`, Send the zone, `OKAY`. HakiOS's loader has the same
  command (`loader.c:4011`).

**Design.**
1. XNU (UniXNU-ruan `package/BootProgress.c`): tee `event()` and the console mirror into the zone at `0x80D80000`
   (mapped with `ml_io_map`). Copy the msgbuf from the existing `monitor()` thread. Take the panic hook from j716f. Each
   line carries the t= stamp.
2. Early in `boot_progress_start`, call `boot_reset_mark_bootloader()` (sheng). From then on, any reset lands in ABL
   fastboot: panic, watchdog, a timer, even a long power press, since SDAM survives resets. `ruan-restart-after=<s>` and
   `ruan-panic-restart-after=10` (j716f) end a run without the user.
3. Reset type: probe `PSCI_FEATURES(SYSTEM_RESET2)` (sheng prints it) and prefer `SYSTEM_RESET2` warm. That is the kind
   of reset under which Android's pstore survives. Fall back to `SYSTEM_RESET`.
4. DarwinBoot `upload`: the host runs `fastboot -s unixnu-ramdisk get_staged prev.log` **before** `stage` in the next
   cycle, so it costs nothing extra. Fallback that needs no UEFI change: boot Android and
   `adb shell su -c cat /sys/fs/pstore/console-ramoops-0` (read-only; the ROM session owns ADB).

**Concrete first step (one cycle, no XNU change).** Add `upload` to DarwinBoot, and add `{"RAM Oops", 0x80D00000,
0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN}` to ruanPkg `MemoryMapLib.c` so UEFI can read
the region. Then, after the next normal `adb reboot bootloader` → `fastboot boot`, fetch the zone. If it holds Android's
last kernel console behind a `DBGC` header, the path XBL → ABL → Mu → DarwinBoot preserves DDR, and P1 is proven before
any XNU code is written.

**Files:** `UniXNU-ruan/package/BootProgress.c` (+ new `BootRamoops.c` and `BootReset.c` from refs),
`UniXNU-ruan/patches/xnu/ruan-boot-progress.patch`, a ruan-local DarwinBoot patch (do not edit the shared UniXNU checkout;
carry it as `UniXNU-ruan/patches/uefi/0003-darwinboot-upload.patch`), `uefi-ruan/.../ruanPkg/Library/MemoryMapLib/MemoryMapLib.c`,
`UniXNU-ruan/scripts/zima-ramroot.sh`.

**User OK:** nothing is written to storage. The SDAM byte is the same thing `adb reboot bootloader` writes. But an
unattended loop keeps the tablet out of Android, so agree on it with the ROM session, and keep `fastboot reboot` as the
exit.

---

## P2. Data volume: its own container on md1 (the known-working shape)

**Root cause, as I read it.** `ruan-ram-data-mount.patch` mounts the Data *role* of the md0 container via
`VFSIOC_MOUNT_BYROLE`. It has to force on a macOS-only branch (`CONFIG_ROSV_STARTUP || !XNU_TARGET_OS_OSX`), and APFS
refuses non-kernel callers (error 45, test 39). After moving the call to a kernel thread (test 40), the result is unknown
because the line scrolled away. md0 is a bare memdev, not an IOMedia, so APFS publishes no per-volume node
(`/dev/md0s2` ENOENT, test 38). No reference does any of this.

**What the references do on 17.x:**
- UniXNU core, `patches/xnu/offline-owner-systemos-mount.patch`, `native_owner_data_prepare()`: Data is a **separate APFS
  container** on its own device, either `/dev/md0` (the non-RAM-block branch: root elsewhere) or a NativeRamBlock IOMedia
  found by volume UUID. It is mounted with `mount(p, {type "apfs", path "/private/var", data = 0x150-byte payload: spec
  pointer, mode 1 at +16, owner 99 at +0x144/+0x148})`. ruan's own `ram-data-dev=` branch is a copy of exactly that call.
- amethyst (iPadOS **17.7.11 21H461**, RAM-only boot, home screen): `CONFIG_XNU_NATIVE_RAM_BLOCK=y`, a packed Data seed,
  `-apfs_shared_datavolume`, root from a partition. The seed comes from `.github/workflows/data-seed.yml`: on a macOS
  runner, `hdiutil create -fs APFS`, then a `tar` of the OS image's `/private/var` (without `staged_system_apps`), plus
  `keybags mobile/tmp tmp(1777) run`. `scripts/log/skip-setup-seed.py` adds the skip-Setup preferences and
  `db/com.apple.xpc.launchd/disabled.plist` with `com.apple.watchdogd: true` (that is your recurring watchdogd SIGTRAP).
- nabu (iPadOS 17.7 21H16, SpringBoard on device): an **empty** apfsprogs container
  (`device_bringup/xnu_stage/make_ram_data_seed.py`) with a fixed UUID. The `/private/var` tree (modes and owners read
  from the OS image) is created in-kernel before init (`source-ci/nabu_data_skeleton.py` generates
  `zzzz-nabu-data-skeleton.patch`, including the timezone links that your `tzinit` failure points at).

**Proposal (smallest change that matches the references):**
1. `make-ramroot.py --data`: stop adding a Data *volume* to the root container. Append a **separate** Data container
   (64–256 MiB) after the root container, 16 KiB-aligned, in the same `root-usb.apfs`. One USB transfer, one CRC.
2. XNU, `iokit/bsddev/IOKitBSDInit.cpp:776` (feeds `xnu-b7545ced`), where the `RAMDisk` property becomes md0: with
   `ram-data-split=<bytes>`, call `mdevadd(-1, base, split>>12, 0)` for md0 and `mdevadd(-1, base+split, rest>>12, 0)`
   for md1. Overlapping memdevs panic in `mdevadd`, so you must split rather than add a second device over the same range.
   About 10 lines in a new `ruan-ram-data-split.patch`.
3. Boot with `ram-data=1 ram-data-dev=md1`. That path already exists in `ruan-ram-data-mount.patch`. Drop the by-role
   branch and the `vfs_syscalls.c` hunk once this works.
4. Data contents, pick one:
   - **(a)** Apple-made: amethyst's `data-seed.yml` on a GitHub macOS runner (size 256m instead of 2g). It produces a
     correct APFS for writes with no mkapfs.py risk. The artifact contains Apple files, so use a private repo and delete
     the run afterwards, as the UniXNU README says.
   - **(b)** Linux-only: `apt install apfsprogs` (Debian trixie has 0.2.0-1), run nabu's `make_ram_data_seed.py`
     (empty container), and port nabu's skeleton patch. Writes then go through apfsprogs' mkapfs layout, which iOS 17.7's
     APFS accepted read-write on nabu. HakiOS's mkapfs.py has never been tested as a *writable* volume under Apple's
     APFS; avoid it for Data.
   - Either way, add amethyst's `disabled.plist` (watchdogd) and the skip-Setup preferences (UniXNU
     `scripts/seed-data-skip-setup.py`).

**First step:** the split patch plus (b) with an empty container. With P1 in place, the one log line
`ram-data: /dev/md1 on /private/var rw: error 0, mounted from /dev/md1` and launchd's own log under
`/private/var/log/com.apple.xpc.launchd/` decide it in one cycle.

**Files:** `UniXNU-ruan/scripts/make-ramroot.py`, `UniXNU-ruan/patches/xnu/ruan-ram-data-mount.patch`, a new
`ruan-ram-data-split.patch`, `UniXNU-ruan/package/RamRootProbe.c` (`ram_data_describe`).
Refs: `UniXNU/patches/xnu/offline-owner-systemos-mount.patch` (around line 195), `refs/UniXNU-nabu/source-ci/nabu_data_skeleton.py`,
`refs/UniXNU-amethyst/.github/workflows/data-seed.yml`, `refs/UniXNU-amethyst/scripts/log/skip-setup-seed.py`.

**User OK:** not needed (RAM only).

---

## P3. Stop re-sending 2.3 GB

**(a) Reuse the root left in RAM (no OK needed).** DarwinBoot places the window deterministically from the same memory
map, so after a warm reset the previous image may still be at `Destination`. Add `getvar:resident`: run
`CalculateCrc32(Destination, Size)` against the descriptor's CRC and return `yes` or `no`. If yes, the host skips `stage`
and sends `continue`. That costs about 1–2 s of CRC instead of 110 s. The risk is that ABL's fastboot download buffer or
XNU itself overwrote part of it; the CRC catches that. Better: a per-64 MiB CRC table in an extended descriptor, so the
host resends only damaged chunks. Odds are about 50 %, because ABL's buffer placement is unknown. First step:
`getvar:resident` only, and see whether it ever says yes.

**(b) Root on `testparti`, read by UEFI (needs OK).** `stock/info/partitions.txt` lists `testparti` at
`/dev/block/sda11`, **4 GiB**. It is a Xiaomi factory-test partition that Android does not mount. The 2.3 GB root
fits, with no GPT change (unlike HakiOS's `tools/userdata-carve.py`, which shrinks userdata and is far riskier). A
DarwinBoot descriptor variant `DBPARTRD` (partition name, size, CRC) makes DarwinBoot find the partition through
PartitionInfo/BlockIo (Mu's UFSDxe works on ruan) and `ReadBlocks` straight into `Destination`. That is UFS speed, a
few seconds. A cycle then sends only the ~12 MB boot image, until the root changes.
- First step (read-only, no OK needed for this part): `adb shell su -c 'dd if=/dev/block/sda11 bs=1M | sha256sum'` and
  check whether it is all zeros or holds anything. Back it up to the PC.
- Then, with the user's OK: `dd` the root image in from Android's root shell.
- Note that `userdata` cannot be used as a raw window: Android's metadata encryption makes the raw LBAs ciphertext.
- Later, NativeUfs/ShengUfs reading the same partition directly (as nabu, amethyst and gts7 do) also frees the 2.3 GB of
  RAM. That needs a UFS driver port, though, and is a separate project.

**Files:** `UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver/UsbRamdisk.h` and `DarwinBoot.c` (descriptor parse, around
lines 841-850), carried as a ruan patch; `UniXNU-ruan/scripts/make-ramroot.py --descriptor`; `zima-ramroot.sh`.

---

## P4. Stop running everything on one little core

`UniXNU-ruan/board.toml:69-74`: `cpus=1`, and cpu0 is an A55, at whatever clock UEFI left it. HakiOS on ruan already ran
`bootcpu=7 cpus=8 garnet_cpufreq=1`, so PSCI CPU_ON works on this tablet. `patches/xnu/soc.series` already carries
`native-smp-secondary.patch` and `native-smp-type-clusters.patch`.

**First step (free):** add j716f's `cpu_mhz_estimate()` (`refs/UniXNU-j716f/package/BootRamoops.c`) to the title line of
the boot page. If cpu0 runs at a few hundred MHz, then:
1. Set the EPSS top performance level for domain 0. ruan's cpufreq-hw addresses are in the DT; j716f's code shows the
   register use: `0x100+4*i` LUT and `0x320` perf level.
2. Try `cpus=8` with the SMP patches.

Expected: ROOTFS and DAEMONS several times sooner, shorter cycles, and less pressure from timeouts and jetsam. No OK needed.

---

## P5. Catch APFS-writer bugs on the host, or stop hand-writing APFS

Two device cycles went to mkapfs.py output that Apple's APFS rejected: the spaceman `sm_addr_offset`, and the type-4
resource map. Both checks could run on the host:
- `apt install apfsprogs` and run `apfsck -cuw root-usb.apfs` on every image, in `build-ramroot.sh`. It is strict about
  the spaceman and the object maps. I have not checked whether it would have flagged those two exact bugs; test it
  against the pre-fix image from git history.
- An "Apple-parity" script: dump the NX superblock, spaceman, omap and volume superblocks of Apple's RRD (an Apple-made
  APFS you already have, `ramroot/rrd`) and of the mkapfs image, and diff the field sets. That is how the spaceman bug
  was found, by hand. Make it a script, and also diff one decmpfs type-4 file's xattr and resource fork against the
  same file in Apple's OS dmg.
- The bigger fix: build the root itself on a macOS runner (hdiutil plus `ditto --hfsCompression`), as amethyst builds
  its Data seed. Apple's own APFS writer then writes everything, and the mkapfs bug class goes away.

**Files:** `UniXNU-ruan/scripts/build-ramroot.sh`, `make-ramroot.py` (`fix_spaceman`, `MKAPFS_APPLE_RSRC`). No OK needed.

---

## P6. Script the whole cycle on zima

`zima-ramroot.sh` already does checks → `fastboot boot` → wait for the receiver → `stage` → `continue`. Missing:
- a watch loop: when `fastboot devices` shows `product=ruan`, run a cycle. With P1's SDAM mark, every XNU end
  (timer, panic) comes back to fastboot by itself;
- `get_staged prev.log` at the receiver (P1), saved as `logs/<run-id>.log` next to the boot args and the image hashes,
  then copied back to this PC;
- content-addressed staging: `root-<sha256>.apfs` on zima, copied only when the hash changes. Today the checkpoint says
  "replace root-usb.apfs" every time. That scp is pointless when only the kernel changed;
- `getvar:resident` (P3a) before `stage`.

**First step:** the content-addressed copy and the `get_staged` call. Both are trivial.
**User OK:** an unattended loop reboots the tablet repeatedly, so it needs the user's go-ahead and the ROM session's
agreement.

---

## P7. Off-device userland in QEMU

UniXNU lists **xVanTuring/UniXNU-qemu** (`UniXNU/devices.toml:3365`, `docs/devices/qemu.md`): iPadOS 17 on QEMU virt,
including a working "Data ramdisk", the original launchd, SystemOS union, the dyld cache, NativeLegacyDisplay on ramfb
and touch/Home. The runner `package/Scripts/qemu-run.py` captures the serial log and screenshots
(`docs/zh/reference/qemu.md`). The stages ruan is stuck on (launchd boot tasks, Data, keybag, backboardd on a dumb
framebuffer) are SoC-independent. The serial log is free there, and a cycle costs no tablet time and no user time.

Unknowns (I did not fetch the repo):
- whether its build runs on Linux without macOS tools (the Data seed and `--metal` need a Mac);
- TCG speed on this 12-core x86. arm64 on x86 TCG is maybe 15–30× slower per core than an A78. That might be tolerable
  with SMP for launchd-stage work, and too slow for UI work.

**First step:** clone it, read its README, and run one boot to launchd with `--timeout 1800`. If launchd does not start
within 20 minutes, drop P7.
**Files:** a new checkout `/root/unixnu-work/refs/UniXNU-qemu`; `apt install qemu-system-arm qemu-efi-aarch64`. No OK needed.

---

## P8. The HakiOS resident-loader method (the best long-term rig)

On this very tablet, HakiOS's `xnuloader/loader.c` never calls ExitBootServices. UEFI stays resident on core 0 and keeps
serving fastboot. XNU runs on another core (`bootcpu=7`, `run_on_other_cpu`). Through a shared `kernel_live` block
(heartbeat, log ring, screen recording) the host gets:
- `oem log` while the kernel runs;
- `oem screenshot`;
- `oem stop`;
- a kernel return to the loader on a timer or panic (`loader_return.c`, `loader_return_after=`);
- `oem ramdisk`: a resident root that survives across runs (`RESIDENT_RD_OFFSET`).

Iterating is then: stage a 12 MB kernel, `oem run`, `getvar result`, `get_staged log`. There is no reset, no ABL and no
root upload, and screenshots replace photos.

The cost is real: DarwinBoot would have to stop exiting boot services and hand XNU a window that avoids UEFI's memory.
`loader_return.c` would need a port to xnu-10063, and GIC/timer sharing has to be checked (HakiOS solved all of this for
xnu-8796 on ruan). Do it after P1, P2 and P6, if cycles are still the bottleneck once SpringBoard work starts.

**Files:** `/root/hakios-garnet/xnuloader/loader.c` (lines 600-700 return stub, 895-930 `kernel_live`, 3680-4100 USB
commands), `/root/hakios-garnet/xnu/files/osfmk/arm64/loader_return.c`. No OK needed.

---

## P9. USB CDC-ACM from XNU

- `refs/UniXNU-gts7xlwifi/drivers/QcomXnuUsbDebug/`: a polled DWC3 CDC driver with a private SMMUv2 domain. It is
  already rebased to xnu-10063.141.1 (`docs/port-sm8250.md`). The SoC is a data header (`QUD_SOC_HEADER`).
- `refs/UniXNU-j716f/package/UsbDebugCdc.cpp`: an IOKit CDC terminal with a ramoops streamer, framebuffer capture and a
  warm-fastboot reboot command.
- ruan: DWC3 at `0x0a600000` (same window as SM8250/SM8550), `iommus = <0x28 0x80 0x00>`, so stream ID `0x80`
  (`ruan-live.dts:731-760`).

Needed: a `Sm7435.h` profile (GCC USB3 PRIM CBCR offsets and GDSC from Parrot's GCC) and a check that Gunyah leaves an
apps-SMMU S1 context bank free for the HLOS.

This gives a live stream and screenshots without P8's loader rework, at medium effort and medium odds. Do it after P1;
P1 covers 80 % of the need.

---

## P10. Faster DarwinBoot receive

2386 MB in about 110 s is about 21 MB/s, roughly half of USB 2.0 HS bulk throughput. `UsbRamdisk.h` receives 1 MiB
chunks into a transfer buffer and then copies each with `CopyMem` (`USBRD_CHUNK SIZE_1MB`).

Try:
1. Measure stock ABL's `fastboot stage` of the same file as a baseline.
2. `USBRD_CHUNK` 8–16 MiB.
3. Check on zima that the tablet enumerates as SuperSpeed (`lsusb -t`; the receiver already supplies SS descriptors).
   A USB-C to USB-C 3.x cable or port might give 3–5×.

P3 makes this moot, but it is a half-day measurement. No OK needed.

---

## Q4 detail: the most robust known-working writable /private/var on 17.x

In order of evidence:
1. **Separate APFS container, block device, plain mount on /private/var.** This is UniXNU `native_owner_data_prepare`,
   used by amethyst (21H461, RamBlock seed, home screen), nabu (21H16, RamBlock, SpringBoard) and j716f/gts7 (UFS Data).
   ruan's equivalent is P2's md1 split.
2. Populated Apple-made seed (amethyst `data-seed.yml`), **or** an empty container plus in-kernel skeleton (nabu).
3. tmpfs: iOS XNU has no tmpfs in the kernelcache, and no reference uses it. Rejected.
4. An fstab-driven `mount -P`: launchd's `mount` boot task (exit 66 in test 37) expects the Data volume to be
   discoverable by role on an IOMedia container. On a memdev it cannot be. It may work once Data is a device the kernel
   has already mounted; leave it to launchd after P2.
5. Mount-by-role on md0 (the current approach): no reference, macOS-only code path. Retire it.

Boot args seen in references and worth trying with P2: `-apfs_shared_datavolume` (amethyst).

---

## Q6: Remaining blockers and estimates

### To a visible SpringBoard (iPadOS 17.7.11)

| # | Blocker | Effort | Odds | Note |
|---|---|---|---|---|
| 0 | Text log out of the device (P1) | 1–2 d | 80 % | Turns every later item from photo-guessing into reading |
| 1 | Writable /private/var (P2) | 0.5–1 d | 75 % | Known-working shape |
| 2 | Boot tasks finish (keybagd, tzinit, init_featureflags, mobile_obliterator, auearlyboot) | 1–3 d | 70 % | Mostly Data-seed content: skeleton, timezone links, disabled.plist (amethyst/nabu lists). AKS stand-in already works |
| 3 | backboardd spawns | 0–1 d | 80 % | Test 36 ran in **restore mode** (before `launchd-normal-boot`), where launchd only runs restore jobs, so "backboardd never spawns" was expected. Re-check after 1–2 before spending time on `_Conclave` |
| 4 | Display: backboardd presents through GopFramebuffer/NativeLegacyDisplay | 3–7 d | 60 % | Component already in `board.toml`. amethyst findings: CA compression keys off in the seed, exact sandbox grant for backboardd |
| 5 | Memory/jetsam with a 2.3 GB RAM root | 0–2 d | 70 % | P3b, plus later a UFS read window, removes it. watchdogd disabled via the seed |
| 6 | Touch (NT36532, SPI bit-bang, edge IRQ latched in TLMM) as a HID digitizer | 3–6 d | 65 % | HakiOS ruan driver exists. UniXNU has ShengNovatekTouch (sheng is Novatek). amethyst: backboardd drops events without Built-In/DisplayIntegrated/PrimaryUsage 0x0D/4 |

**Skippable with a smarter order:**
- Visible SpringBoard does not need touch, so do touch after 4.
- No `_Conclave` work until 3 is re-tested in normal mode.
- No more mkapfs.py fixes for Data (P2 b/a).
- No UFS driver port until a read-only window is needed for RAM (P3b covers the cycle time).
- Audio, Wi-Fi and battery are irrelevant to both goals right now.

Rough total to a visible, untouchable home screen: 1.5–3 weeks of agent time. With P1, P2 and P6 in place, that becomes
mostly unattended cycles.

### To macOS

UniXNU's own docs (`UniXNU/docs/porting/arm64e.md`) say arm64e profiles **require PAC hardware**. ruan has none
(`macOS/probe/isaprobe.c`: PACIA, LDRAA, FJCVTZS, LDAPUR and FRINT32Z all SIGILL). So macOS rests entirely on a
darwin-lisa-style rewrite plus emulation. That is a separate multi-month project: offline shared-cache rewrite, page-in
rewrite, trap emulation for v8.3–8.5 instructions, null-PAC XNU. On top of that come a writable multi-GB Data, which
means real storage, and WindowServer, which needs at least a solid CPU-rendered framebuffer. My odds: **under 15 % within
three months.**

Smarter order: the iPadOS 17 milestone matters for macOS only through drivers (display, touch, storage, SMP/cpufreq,
logging). The iOS-specific userland work (keybag, SpringBoard, Setup) does not carry over. So once SpringBoard is
visible and touch works, run a cheap **PAC gate** before more iPadOS polish: boot an arm64e iPadOS 26/27 launchd under
darwin-lisa's approach on ruan. If that fails within about two weeks, macOS is not reachable on this SoC in a sensible
time.

---

## What I did not verify

- That DDR at `0x80D00000` survives the ruan warm path into ABL fastboot. P1's first step tests exactly that.
- Whether PSCI `SYSTEM_RESET2` is offered through Gunyah on ruan. sheng's probe prints it.
- Whether `testparti` is empty on this unit.
- UniXNU-qemu's Linux buildability and its TCG speed.
- Whether `apfsck` would have caught the two past mkapfs.py bugs.
