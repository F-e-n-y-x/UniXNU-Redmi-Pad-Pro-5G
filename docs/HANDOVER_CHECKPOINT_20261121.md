# Handover Checkpoint: iPadOS 17.7.11 on Redmi Pad Pro 5G ("ruan")
**Date**: 2026-11-21 01:10 IST  
**Target SoC**: Qualcomm Snapdragon 7s Gen 2 (SM7435 / "parrot", Adreno 710)  
**Target Hardware**: Xiaomi Redmi Pad Pro 5G ("ruan", 8GB RAM / 256GB UFS)  
**Target OS**: Apple iPadOS 17.7.11 (`21H461`, arm64e static rewritten via UniXNU)  
**Root Filesystem**: Internal UFS Partition `sda22` (`unixnu`, 32 GiB APFS)  
**Current Device State**: **Fastboot Mode** (`cf10660 fastboot`)

---

## 1. Quick Summary of Current State & Handover
- The device successfully booted into iPadOS 17.7.11 LockScreen (`rbu113u`) via `sda22` root (`ufs-own-root.series`).
- The AMFI code-signature rejection loop on `CollectionsPoster` has been **completely resolved** (0 crashes in `rbu113u` vs 600+ crashes in `rbu111u`) by enrolling CDHashes for `NativeAdrenoMetal`, `libvulkan_freedreno.dylib`, and `/usr/bin/IOMFB_FDR_Loader` in `trust.bundle`.
- Screen snapshots and framebuffer capture via CDC-ACM debug interface (`ruan-usbdbg.py`) are fully working.
- The device recently rebooted back to **fastboot** due to an idle Sleep/Wake watchdog panic in `IOPMRootDomain`. The tablet is currently connected to USB and sitting in **fastboot** (`cf10660 fastboot`).

---

## 2. In-Depth Diagnostics of Recent Issues & Solutions

### A. Panic: `Sleep/Wake hang detected @ IOPMRootDomain, cpp:1281`
* **Observed**: Screen froze and printed `Sleep/Wake hang detected @ IOPMRootDomain`, followed by an automatic reboot to fastboot after panic timeout.
* **Root Cause**:
  - After 10 minutes of user inactivity (no touch registering), IOPM idle timer fired:
    `PMRD: idle time -> 3000 ms`, `PMRD: Enter Sleep`, calling `IOCPUSleepKernel`.
  - Qualcomm SM7435 under Gunyah EL2 does not have Apple PMGR / SMC hardware sleep state handling, so CPU core powerdown hangs indefinitely.
  - Apple's sleep-wake watchdog (`SWD`) detects that the sleep transition never finished and panics the kernel.
* **Solution**:
  - Add `noidle=1` and `swd_panic=0` to the kernel boot-args in `unixnu.toml`.
  - In `xnu/iokit/Kernel/IOPMrootDomain.cpp`:
    `PE_parse_boot_argn("noidle", &gNoIdleFlag, sizeof(gNoIdleFlag));`
    Setting `noidle=1` completely suppresses idle display/system sleep transitions.

---

### B. Touch Digitizer Not Functioning (`hid0`, `ev563`, `rffffffff`)
* **Observed**:
  - Banner displayed: `in tp3 hid0 rffffffff hs3 ev563 raw 14993,3323 max 16000,25600 n 61414,8507 | novatek: polls, valid`.
  - Novatek hardware driver (`RuanTouch.c`) is actively receiving valid SPI touch packets (`raw 14993, 3323`).
  - However, `hid0` (0 reports dispatched) and `ev563` (563 seconds where `hasEventDriver` returned false).
* **Root Cause**:
  - In `/root/unixnu-work/UniXNU-ruan/package/NativeHidInput.cpp`:
    ```cpp
    void touchSend() {
        NativeHidInput *self=this;
        if(!self->hasEventDriverCached())return; // <-- DROPPING ALL TOUCHES HERE
    ...
    ```
  - `hasEventDriverCached()` looks for an `IOHIDEventDriver` attached to the `NativeHidInput` service in `gIOServicePlane`.
  - On iOS/iPadOS, `IOHIDEventDriver` only matches against `IOHIDInterface` with specific usage declarations (or is deferred via `IOMatchDefer`).
  - Because `hasEventDriverCached()` returned false, every physical touch was dropped before `self->handleReport()` could even be called!
* **Solution**:
  1. In `NativeHidInput.cpp`, do not drop reports if `hasEventDriverCached()` is false; allow `handleReport` to execute directly once `NativeHidInput` is started, or register an explicit digitizer matching personality so `IOHIDEventDriver` attaches.
  2. In `HidInputProbe.cpp`: Verify `sheng-touch-split=1` and `sheng-touch-hid=1` properly instantiate and publish the digitizer nub to `IOHIDResource`.

---

### C. Jetsam Thrashing & SpringBoard Instability
* **Observed**:
  - Kernel log flooded with:
    `memorystatus: System is unhealthy! memorystatus_available_pages: 56704 compressor_size:4`
    `memorystatus: available_pages_below_critical=1... killing_top_process`
  - Over 20,000 process exits (`exits=20541`).
  - Eventually killed `cfprefsd` (`exit status 0x9 reason ns 1 code 0x4`).
  - Without `cfprefsd`, UIKit / SpringBoard crashed with `uncaught exception of type NSException` and entered a restart cycle.
* **Root Cause**:
  - Background iOS daemons (`accessoryupdaterd`, `triald`, `nfcd`, `calaccessd`, `familycircled`, `mobiletimerd`, `suggestd`) are constantly spawned by `launchd`.
  - Available memory drops below the `memorystatus_available_pages_critical` threshold on this 8GB device (~5.6GB visible to XNU).
  - Jetsam aggressively kills processes in order of memory band until core system services (`cfprefsd`, `ptpd`) get hit.
* **Solution**:
  - Add `memorystatus_control` or memory threshold boot-args:
    Check `kern_memorystatus.c` for boot-args like `jetsam_disable=1` or `memorystatus_low_mem_threshold=...`.
  - Disable non-essential launch daemons on the root filesystem (or disable them in `launchd.plist` / `/private/var/mobile` overrides).

---

## 3. Key Technical Constraints
1. **Command Line Length Limit**:
   - UniXNU's payload builder strictly enforces a **1000-byte cap** on the kernel command line (`args` in `unixnu.toml`).
   - `rbu113u` args were 944 bytes. Keep new boot-args within the 56-byte headroom or replace/prune unused args.
2. **Never Pass `cs_enforcement_disable=1`**:
   - Triggers kernel panic: `"can't has cs_enforcement_disable"`. Code signing exceptions must be added to `trust.bundle` via `ldid -h` + `trust-append-module.py`.
3. **Partition & Workspace Rules**:
   - Only commit files within `macOS/` (`git add macOS/`).
   - Android `boot_a` and Android `userdata` (`sda21`) must NOT be modified or flashed.
   - iPadOS root lives in `sda22` (`unixnu`).

---

## 4. Key Paths & Tools
* **Live USB Tool**:
  `/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/tools/ruan-usbdbg.py`
  - `python3 tools/ruan-usbdbg.py shot -o screen.png`
  - `python3 tools/ruan-usbdbg.py dmesg -o dmesg.txt`
  - `python3 tools/ruan-usbdbg.py cmd button.power`
* **Driver Sources**:
  `/root/unixnu-work/UniXNU-ruan/package/`
  - `NativeHidInput.cpp` (Touch HID bridge & event driver matching)
  - `HidInputProbe.cpp` (IOHID device allocation & initialization)
  - `RuanTouch.c` (Novatek NT36532 hardware SPI driver)
  - `BootProgress.c` (On-screen diagnostic banner)
* **Boot Script**:
  `/root/unixnu-work/boot-local.sh <TAG>`
* **Previous Images**:
  `/root/unixnu-work/ramroot/out-rbu113u/`
