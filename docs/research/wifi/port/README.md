# Wi-Fi port for ruan: WCN6750 stack from UniXNU-asteroids

Status 2026-11-21: ready-to-build code. **Nothing has been built into a kernel, booted or run on the tablet.**

The source is filipnavara/UniXNU-asteroids at `9f07af3b0bb431a01de12ab92f531619a7104083` (Nothing Phone 3a, SM7635, under Gunyah).
That port has WPA2, DHCP and Settings joins working (wifi91). This directory ports its whole chain to ruan (SM7435): WPSS boot over
SCM PAS 6, SMP2P, GLINK/SMEM, QRTR, then QMI WLFW and ath11k on the hybrid bus, then en0 with Apple80211 and wpa_supplicant.
Background is in `../plan.md`. That plan's section 5 and the draft both had one error: TLMM `0xf000000` (see "Verified").

## What is here

| Path | What it is |
|---|---|
| `package/RuanWifiWpss.cpp` | ruan's host for the WPSS. It implements `AsteroidsRemote.h` on its own polled thread: IPCC, SMEM plus the TCSR mutex, AOSS QMP, RPMh ARC on TCS 1, PAS with `ruan_smc_call`. It also handles the `ruan-wifi=N` gate, the two start hooks, and the USB `wifi` and `wifi.log` reports. |
| `package/WpssProbe.cpp` | asteroids' file, ported: carveout, WL_EN pin 36, `ruan-wifi*` boot-arg names, module handoff from stage 3, and a Data `wpss.mbn` source. |
| `package/RuanWifi.cpp` | asteroids' `AsteroidsWifi.cpp`, ported: SMMU streams 0x1c00/0x1c01, MSI 0x17210040, DMA window 0x8c000000, the smpb7/smpe8/smpb8/smpb9/ldob22 VRMs, the s7b CPR rail, and BDF names. The PMIC GPIO code is dropped and class registration uses preModLoad/postModLoad. |
| `package/AsteroidsWifiFiles.c` | asteroids' file; only the join.txt path changes (ruan has no Data add-on). |
| `package/WifiNet.cpp`, `Qcom*.{c,h}`, `AsteroidsRemote.h`, `AdspFirmware.c`, `FatVolume.{c,h}` | asteroids' files, byte for byte (`cmp`-checked). |
| `scripts/fetch-ath11k-wifi.sh` | Fetches `drivers/Ath11kWifi` at the pin into `unixnu-ruan/.cache/Ath11kWifi` (git-ignored). It adds one `NW_IOS_TARGET` line so the supplicant builds as arm64 iOS 17. |
| `scripts/build-ath11k-wifi.py`, `wifi_entry_points.py` | asteroids' module build, changed only to use Linux `llvm-nm`/`llvm-objcopy` and UniXNU's `kernel_target`. |
| `scripts/build-wifi-userspace.py` | Builds the supplicant (theos SDK, lld, ldid) as a root overlay plus cdhashes and the launchd job. |
| `scripts/stage-wifi-firmware.py` | Stages the WCN6750 firmware from the user's `NON-HLOS.bin` into a Data overlay. It refuses to write inside any git tree. `--self-test` runs without firmware. **Not run.** |
| `userland/` | The launchd job (XML), `wpa_supplicant.conf.example` with a placeholder network, and `join.txt.example`. |
| `patches/wifi-port.patch` | Unified diff against `macOS/unixnu-ruan`: `build-ram-root-probe.py`, `BootProgress.c` hook, USB `wifi.log` in `UsbDebugCdc.cpp`, the `wlan` node in `dts/ruan.dts`, and a `board.toml` comment. |
| `patches/asteroids-to-ruan.diff` | Every change made to asteroids' files, for review. |
| `check/syntax-check.py` | `-fsyntax-only` with the exact flags from ruan's last XNU build. |

Superseded: `../draft/WpssProbe-ruan.patch` (wrong TLMM base) and `../draft/stage1-hooks.patch`. The tree has changed since that
patch was written: `UsbDebugCdc` already has a `wifi` command, and the late probe already calls `ruan_wifi_init`.

## Board and components: why there are so few changes

- **`[components]`:** cannot list `Ath11kWifi`. UniXNU's `components.toml` catalogs only `Ath12kWifi`, and asteroids does not list it
  either. The module is fetched by script, the way `make-ufs-board.sh` fetches ShengUfs.
- **`[kernel] archives`:** no new entry. Everything goes into `libRamRootProbe.a` through `build-ram-root-probe.py`. Both board
  variants already run that script and link the archive through `CONFIG_XNU_EXTRA_LD_ARCHIVES`.
- **What gets built:**
  - Stages 1–2 are always built. They are small and need no network.
  - `RUAN_WIFI_MODULE=1` in the build environment adds stages 3–5 (ath11k, mac80211, cfg80211, RuanWifi, WifiNet). The first build
    fetches the Linux sources from GitHub.
- **How the code gets linked in:**
  - `BootProgress.c`'s strong `ruan_wifi_start()` pulls `RuanWifiWpss.o` out of the archive.
  - The late probe's existing weak `ruan_wifi_init()` call then binds as a second chance.
  - The unused `RuanWifiProbe.c` compile line is replaced; its source files can be deleted.
- **defconfig:** no change.

## Apply order (in the session that owns `macOS/unixnu-ruan`)

1. `cp port/package/* unixnu-ruan/package/`. No name collides; the existing `SmmuBypass.h` and `RuanZap.h` are used as they are.
2. `cp port/scripts/{fetch-ath11k-wifi.sh,build-ath11k-wifi.py,wifi_entry_points.py,build-wifi-userspace.py,stage-wifi-firmware.py} unixnu-ruan/scripts/`
3. `cd unixnu-ruan && patch -p1 < ../docs/research/wifi/port/patches/wifi-port.patch`
   - Checked: `patch --dry-run` is clean against the working tree as of 2026-11-21.
4. Sync to the build copy `/root/unixnu-work/UniXNU-ruan` as usual.
5. Build as usual for stages 1–2. Add `RUAN_WIFI_MODULE=1` for stages 3–5.
   - Kernel size: the module adds several MB of code. Check the build's size and prelink reports first.
6. Optional: `python3 check/syntax-check.py` re-runs the syntax check.

## Test order (each stage is one RAM-only `fastboot boot`, cleared with the user and the ROM session first)

Boot args go on the command line, for example `KERNEL_ARGS="ruan-wifi=2"` with `build-ramroot.sh`. `ruan-wifi` is not in the built-in
defaults, so Wi-Fi is off unless it is given. Read results over the USB debug terminal (`ruan-usbdbg=1` is a default) with the
commands `wifi` (one line) and `wifi.log`.

**Firmware source:**
- **UFS variant (current, recommended):** the kernel reads `image/qca6750` of modem_a in place. No staging is needed.
- **RAM root (no UFS driver):** first run
  `scripts/stage-wifi-firmware.py <NON-HLOS.bin> /root/unixnu-work/wifi-fw` (outside git). Then build with
  `REBUILD_ROOT=1 DATA_CONTAINER=1 DATA_OVERLAY=/root/unixnu-work/wifi-fw`.

### Stage 1: survey (`ruan-wifi=1`)

- **Writes:** nothing.
- **Pass:** `[usb] WIFI: wpss st 2 err 0 slave ffffffff items 1xx ...`
  - `items` bit 0x100 means the SMEM partition with pid 13 exists.
  - `slave` -1 or 0 means the WPSS is not running.
- **Failure `st -100`:** a register window did not map.

### Stage 2: WPSS boots, board QRTR router (`ruan-wifi=2`)

- **Pass:** `wpss st 11 ... ready <ms> ... svc <n> wlfw <instance>`.
  - In `wifi.log`, `[usb] WIFI.services:` lists `45:<inst>` (service 69 = WLFW).
  - `wlen <before>><after>` is the WL_EN pin as `in/out << 16 | config`. After should read `30200` or `20200`: config bit 9 (output) set,
    out bit 1 high (in bit 0 too once the pad follows).
- **Failure decode:**

  | Code | Meaning |
  |---|---|
  | `st -4` | AOSS QMP `load_state` refused |
  | `st -5` | cx/mx vote failed |
  | `st -6 err -1` | no image (`WIFI.note: fw: no wpss image`; `WIFI.files` shows what modem_a holds) |
  | `st -6 err -3` / `-4` / `-5` / `-6` | init_image / mem_setup / segment load / auth_and_reset failed; `scm` = x0 |
  | `st -7 err -60` | no ready bit (read `WIFI.crash`) |
  | `st -3 err -114` | the WPSS was already running |

- **If the SoC resets during PAS:**
  1. Retry with `ruan-wifi-order=1`, which writes the segments before init_image, as the zap needed.
  2. Then retry with `ruan-wifi-meta=1`.
- If nothing happens at all, try `ruan-wifi-delay=40` (the default is 20 s after `boot_progress_start`; the late probe starts it
  sooner).

### Stage 3: QMI, firmware ready (`RUAN_WIFI_MODULE=1`, `ruan-wifi=3`)

- **Radio:** stays off.
- **Pass:** the `wifi` line ends `| wlan st 9 err 31 ... smmu 3 ...`.
  - `err 31` = 0x1f: all five rails voted.
  - `smmu 3`: both streams are pass-through.
  - `fw` counts served files in the low 16 bits; files refused are counted from bit 16 up.
- **Pass, in `wifi.log`:**
  - `wifi: firmware bdwlan.bNN from image/qca6750/bdwlan.eNN` (or `from Data`)
  - `regdb.bin`
  - `wifi: firmware ready, mac80211 registered; radio left off (ruan-wifi=3)`
  - no `smmu bank ... fsr=` lines
- **Also check:** `wifi: mapping DMA window start: ... attr 44 (Normal non-cacheable)`. If it shows CACHEABLE, see risk 4.
- **Failure:** `wlan st -N` = the step that failed (Stage enum in `RuanWifi.cpp`), and `err` is its code.
- **If the board data is refused:** try `ruan-wifi-bdf=image/qca6750/bd83uin.elf`.

### Stage 4: radio on, self-test scan (`ruan-wifi=4`)

- **Pass:** `wlan st 8 ... nets <n>` with n > 0.
  - `WIFI.networks:` lists `SSID/-dBm`.
  - `irq` climbs, and `wifi: interrupts on: vectors ...` appears.
- **Known failures (from asteroids):**
  - firmware assert at the first vdev: WL_EN or supplies
  - `whalRxMpduPop` or `wal_monitor` asserts: the sleep clock (risk 2)
  - `whalRecvDmaLeakRecovery`: the DMA window
- **Interrupt fallback:** `ruan-wifi-irq=0` polls instead of attaching interrupts.

### Stage 5: en0, supplicant, DHCP (`ruan-wifi=5 no-lc=1`)

- **Prerequisites:**
  - the userland overlay: `build-wifi-userspace.py <out> --into <turnip overlay>`, then `OVERLAY=` in `build-ramroot.sh`;
  - the `wlan` node from the patch.
- **Pass, in `wifi.log`:**
  - `wifi-net: en0 is up, hardware address ...`
  - after a join, WifiNet's `wifi-net: join finished: module result 0`. WifiNet takes the module's events, so the board's own
    `join result` line appears only with `ruan-wifi-net=0`.
- **Pass, DHCP:** `ls /private/var/db/dhcpclient/leases` over USB shows an `en0` lease (inferred path).
- **Without wifid or a launchd job (test images only):**
  - `wifi-aj=1`, with `stage-wifi-firmware.py --join <local file>` placing `/private/var/wifi-test/join.txt`. That file holds a key.
  - The kernel joins through the supplicant's control socket, so the supplicant must run.

## Verified vs inferred

**Verified (read in a file, or checked on this host):**
- **Every ruan constant against the live DT `hardware/private/ruan-live.dts`** (line numbers are in the source comments):
  - WPSS carveout 0x85300000+0x1900000 (128–130), SMP2P 617/616, pid 13, IPCC 0x18/2 (6394–6401), GLINK 0x18/0 IPCRTR (7827–7839)
  - wl-reset GPIO 36 (26294), wcn6750 msi 0x17210040, SMMU 0x1c00/1, SPIs 0x300–0x31f, supplies (26315–26341)
  - VRM names smpb7/smpe8/smpb8/smpb9/ldob22 (5214/5788/5228/5242/5625)
  - RSC DRV2/0xd00 with three active TCS first (5113–5121), cmd-db (67), SMEM (98), mutex (6238–6254), IPCC (6222), AOSS (6493)
  - GICD 0x17200000 (5087), mpss_region (170)
- **TLMM GPIO base is 0xf100000, not 0xf000000:**
  - The DT's `pinctrl@f000000` is the whole tile.
  - `RuanTouch.c` drives ruan's pins at 0xf100000 + 0x1000·n, and touch works.
  - The draft patch and the plan's table were wrong here.
- **Syntax:**
  - All kernel files pass `-fsyntax-only -Wall -Wextra -Werror` with the compile-record flags of ruan's last XNU build (clang 24,
    arm64-apple-ios17.0).
  - The patched `BootProgress.c` and `UsbDebugCdc.cpp` pass the same check.
  - The patched `ruan.dts` builds with `dtc`.
  - The patched build script parses.
- **Symbols:**
  - `__GLOBAL__sub_I_WifiNet.cpp` is the static-init symbol in an object compiled with the real flags.
  - The weak imports are weak.
- **Scripts:**
  - `fetch-ath11k-wifi.sh` was tested against a local clone at the pin.
  - The `stage-wifi-firmware.py` merge passed its self-test.
  - `patch --dry-run` is clean.

**Inferred (not checked on ruan):**
- PAS 6 is accepted from HLOS under ruan's Gunyah (asteroids' evidence is from SM7635).
- `mpss_region` is reachable from the HLOS VM, both for the DMA window and the metadata fallback.
- WL_EN is enough to power up the chip.
- The rail voltages at their DT minimums are right.
- No WCN sleep clock is needed (none in the live DT).
- `bdwlan.bNN` maps to modem_a's `bdwlan.eNN`.
- Write-combined mappings are Normal non-cacheable on xnu-10063.
- ath11k and WifiNet run unchanged on iPadOS 17.7.11. asteroids' Wi-Fi ran on iOS 27. j716f's record is the only evidence for 17.7.11,
  where its Apple80211 join layouts may differ.
- The DHCP lease path.

## Open risks

1. **SoC reset at PAS or SMMU writes (Gunyah):**
   - Every SMMU access is a plain LDR/STR, as asteroids learned.
   - Stage 1 writes nothing.
   - PAS order and metadata placement are selectable by boot arg.
2. **Sleep clock and supplies:**
   - These were asteroids' slowest bugs.
   - If stage 4 shows scan asserts, read the PMK8350 GPIO states that Android leaves. That is a read-only adb dump, and only with the
     ROM session's OK.
   - Then add a GPIO write where `asteroids_wifi_sleep_clock()` is (currently a weak stub in `RuanWifiWpss.cpp`).
3. **The supplicant's launchd job:**
   - launchd reads jobs only from its signed cache.
   - Merging the job needs a detached ad-hoc signature. That takes a real `codesign` (macOS); `/usr/bin/codesign` here is a no-op shim.
   - `build-wifi-userspace.py --launchd-cache` stops early without `CODESIGN=`.
   - Options: sign on a Mac; or write the detached CodeDirectory generator in Python; or start the supplicant from a hijacked RunAtLoad
     job, as `build-turnip-smoke.py` does.
   - Stages 1–4 do not need it.
4. **Cache attributes:** if `logMapping` shows the DMA window CACHEABLE on xnu-10063, the chip and the CPU disagree. asteroids needed
   a kernel patch on xnu-12377 for the opposite case.
5. **ArmCpu SPI slots:** the module asks for 16 slots, next to the GPU, backlight and touch. `ruan-wifi-irq=0` polls instead.
6. **Kernel code window and `__DATA`:** asteroids' qcacld did not fit, but ath11k did. ruan's `__DATA` is already tight; the big
   buffers here are on the heap.
7. **RSC IRQ-enable register:** `RuanWifiWpss.cpp` and `RuanTouch.c` both read-modify-write the shared register (offset 0xd00) on
   different threads. A rare race; the completion wait is bounded.
8. **The tablet is shared:** every boot needs the user's and the ROM session's OK (AGENTS.md).

## Update 2026-11-21 (next-build integration, prepared but not applied)

The coordinator asked for these changes; the permission system blocked writing to the live trees, so they live only here:

- **Default stage:** without the argument, `ruan-wifi` defaults to 4 (`RuanWifiWpss.cpp` `kDefaultMode`; `WpssProbe.cpp` reads the same
  default). `ruan-wifi=0` turns everything off.
- **Module build:** on by default. `RUAN_WIFI_MODULE=0` leaves it out.
- **Supplicant without a Mac signature:**
  - `build-wifi-userspace.py` puts `scripts/RuanWifiLauncher.c` in the slot of Apple's RunAtLoad root job `com.apple.DumpPanic`, the
    way oslogx wraps IOMFB_FDR_Loader.
  - The launcher runs `DumpPanic.real`, then restarts the supplicant whenever it exits, with a backoff from 3 to 60 s.
  - The launchd cache stays Apple's; the two new CDHashes go into the overlay's `cdhashes`.
  - `/root/unixnu-work/wifi-join.txt` (network name, then passphrase) is copied to the root as `/private/etc/wifi/join.txt` at mode 0600.
    `AsteroidsWifiFiles.c` reads that file before the Data copy.
  - Run `build-wifi-userspace.py <dir> --into /root/unixnu-work/oslogx/overlay-dir` before cycle-root.sh, then boot with
    `ruan-wifi=5 wifi-aj=1`.
- **Locked sources:** `fetch-ath11k-wifi.sh` also patches the module's `fetch-sources.py`. That script refused freebsd releng/15.1
  because the branch has moved past the locked commit; the patch makes it fetch the locked commit instead.
- **Host checks:**
  - The module built on this host: 102 objects.
  - Its only symbols from outside are kernel libc ones (`sscanf` is in XNU's `libkern/stdio/scanf.c`).
  - Size: about 1.05 MiB of text and 24 KiB of data, plus about 70 KiB for the board files. The kernel goes from 52.6 MB to about
    54 MB; the payload is 64 MiB and its limit is 95 %. The XNU code-window limit is unchecked until a real link.
