# Bluetooth on ruan (WCN6750 over a GENI UART, iPadOS 17.7.11): plan

Status 2026-11-21: research plus ready-to-build kernel code in `port/`. **Nothing has been built into a kernel, booted or run on the
tablet.** Tags: **[V]** = verified (read in a file, the live DT, the stock image or a working port's source and record).
**[I]** = inferred.

## 0. Bottom line

- **The kernel side is a port, not new work.**
  - filipnavara/UniXNU-asteroids (`9f07af3`, the source of our Wi-Fi port) runs the WCN6750's Bluetooth on UniXNU. Its README for
    2026-11-14 records "WCN6750 Bluetooth scanning". **[V]**
  - The chain is `package/BluetoothProbe.cpp` plus `drivers/QcaBluetooth`. QcaBluetooth is sheng's driver: Linux `btqca.c`, GENI SE
    DMA, H4/IBS, and the `com.apple.uart.sk.bluetooth` kernel control. On sheng it is device-verified: pairing, A2DP, reconnect. **[V]**
  - `port/package/RuanBluetooth.cpp` is that probe with ruan's wiring. All 8 kernel sources compile with ruan's XNU-10063 flags, and
    their objects resolve against ruan's last kernel. **[V]**
- **The kernel interface already matches iOS 17.** **[V]**
  - The 21H461 `bluetoothd` has the same strings as on 27: `com.apple.uart.sk.bluetooth`, `transport-encoding`/`transport-speed`
    ("DeviceTree transport/speed"), `local-mac-address`, and the BlueTool XPC keys (`kMsgId`, `RunBuiltinScript`, `boot`, `init`,
    `deepsleep`, `manufacturer`, `firmware`).
  - So QcaBluetooth's `userland/BlueTool.c` and its UART.SK front end fit as they are. (The 21H461 file was extracted from
    `044-76040-038.dmg` to /tmp only.)
- **The one real gap: userland routing in iOS 17's `bluetoothd`.**
  - The native daemon only accepts Broadcom chips (1..0x7cf) or Apple ACI chips (0x7d0..0xf9f). Any other chipset ends in "Invalid
    chipset" or "Unknown platform/chipset/manufacturer combination, restarting" (section 3).
  - Patches exist for iOS 16 (daimile2/renoir_xnu_bluethooth `tools/patch-renoir-bluetoothd.py`, 20H392, arm64) and for iOS 27
    (QcaBluetooth `native_ios27.py`, arm64e with PAC). Neither covers 17.
  - 21H461's `bluetoothd` is plain arm64 (cpusubtype 0, no PAC) and has the same header-cave layout as 20H392 (`__text` at 0x5550).
    So the iOS 16 patcher is the template. **[V]**
- **Cheapest path:** run stages 1–3 below (kernel only, no userland risk). Then run stage 4 with the replacement BlueTool and the
  patched daemon. Both are now written (`port/scripts/patch-bluetoothd.py`, `build-bluetooth-userspace.py`; section 2). Settings →
  Bluetooth on, discovery and pairing come from the native stack. A2DP and extended LE features come later.
- **Effort:** kernel stages 1–3 take about 3–5 test cycles; stage 4 then takes a few cycles.

## 1. Facts

| Item | ruan | Source |
|---|---|---|
| BT node | `bt_wcn6750` `qcom,wcn6750-bt`: bt-reset (BT_EN) **gpio35**, sw-ctrl **gpio61**, wl-reset gpio36 (WpssProbe.cpp drives it) | live DT 26288 [V] |
| BT supplies | io = ldob22 1.8 V; aon/dig/rfacmn/rfa-0p8 = smpb7; rfa1 = smpe8; rfa2 = smpb8; ipa-2p2 = smpb9. **All five are already voted by RuanWifi.cpp** (`kVrms`) | DT 5222–5796, RuanWifi.cpp:318 [V] |
| UART | `qcom,qup_uart@a94000` (`msm-geni-serial-hs`, alias hsuart0) = QUPv3 wrapper 1 SE5. IRQ SPI 0x166 (INTID 390); wakeup gpio17; wakeup byte 0xfd (IBS); wrapper `qupv3_1_geni_se@ac0000`, SMMU SID **0x403** | DT 16073, 15759 [V] |
| UART pins | CTS 14, RTS 15, TX 16, RX 17, function 1 (`qup1_se5_l0..l3`); sleep/default = GPIO with pulls | DT 12960–13080, pinctrl-parrot.h [V] |
| TLMM | GPIO blocks at 0xf100000, 0x1000 per pin; summary IRQ SPI 0xd0 (INTID 240); interrupt target bit **5** (kpss value 3), raw status 4 (the 3a uses bit 8) | pinctrl-parrot.h, WpssProbe.cpp [V] |
| GCC | clocks <&gcc 88 93 94> = WRAP1_S5, WRAP_1_M_AHB, WRAP_1_S_AHB. Vote reg 0x62008 (wrap1 core_2x 18, core 19, m/s_ahb 20/21, s0–s5 22–27). SE5 RCG 0x28618, CBCR 0x28610, RO post-div 0x28744. GPLL0 vote 0x62018 bit 0. Parent 6 = GPLL0-even 300 MHz | gcc-parrot.c, qcom,gcc-parrot.h (MotorolaMobilityLLC/kernel-msm) [V] |
| Baud clocks | 115200: 7.3728 MHz (384/15625, in parrot's table). 3.2 M: 51.2 MHz (64/375; not in the table but under the 75 MHz low-corner cap; asteroids uses the same). GENI divider 1, oversampling 16 | gcc-parrot.c ftbl [V]; the 51.2 MHz choice [I] |
| Firmware | `bluetooth_a/b` = stock `BTFM.bin`, a FAT volume with 4 KiB sectors: `image/msbtfw11.mbn` (203072 B), `msbtfw11.tlv`, `msnv11.bin` (6366 B), and others. `BluetoothFirmware.c` reads them in place through ShengUfs (`native_ufs_part_*`), as AdspFirmware.c reads modem. Nothing is copied into git | stock flash_all.sh, `7z l BTFM.bin` [V]; WCN6750 ROM version 0x11 [I] |
| iOS 17 transport | H4 via `com.apple.uart.sk.bluetooth` kern_ctl + Skywalk kernel pipe; DT `transport-encoding` 1 = H4, `transport-speed`; also env `BT_HCI_TRANSPORT` (H4/H4DS/H4BC/H5/USB/APPLEBT/PCIe) | 21H461 bluetoothd strings [V] |
| Skywalk KPI on xnu-10063 | `kern_nexus_*`, `kern_pbufpool_*`, `kern_channel_*` all defined in ruan's last kernel | `llvm-nm` vs objects [V] |

## 2. Cheapest path (stages, `ruan-bt=N`; default 0 = nothing runs)

Each stage is one RAM-only or UFS `fastboot boot`, cleared with the user and the ROM session first. Every stage only logs on failure
and switches back to off.

| N | What runs | Touches | Pass = USB `bt` / `bt.log` shows |
|---|---|---|---|
| 1 | Survey, read only: GCC vote/SE5 branch/RCG/post-div, GPLL0, TLMM cfg/io of pins 14–17, 35, 36, 61. The SE5 `FW_REVISION` only if its branch and both AHB branches already run | nothing written | `survey …` lines; whether UEFI left SE5 clocked, its protocol (2 = UART), BT_EN low |
| 2 | Plus GPLL0/SE5 votes, RCG 7.3728 MHz, QUP ≥ v2 and protocol 2 check, SMMU bypass slot for SID 0x403, IRQ 390 + TLMM summary share, GENI internal loopback of 16 bytes. **BT_EN stays low.** Then off | GCC, QUP1 SE5, apps SMMU (one bypass slot) | `dma bypass slot=…`, `loop=` with bit 17 set (match), `power=1 result=0` |
| 3 | Plus waits for the WPSS (ruan-wifi ≥ 2: `native_wpss_stats[0] ≥ 7`, ≤ 60 s), BT_EN high, `qbt_boot`: HCI reset, read version, rampatch `msbtfw11`, NVM `msnv11`, 3.2 Mbaud, then pre-shutdown and BT_EN low | the chip | `product/soc/rom/patch` non-zero, `fw=…`, `result=0 baud=3200000` |
| 4 | Plus `/arm-io/bluetooth` (`local-mac-address` from unique-chip-id) and a matching `bluetooth` IOService, `kern.qbt_*` sysctls, UART.SK. Stays on and serves bluetoothd | userland | (a) original daemon: connect logged, then the "chipset combination" failure; (b) patched daemon: Settings → Bluetooth on, scan, pair |

Use `ruan-wifi=3` (or higher) for stages 3–4. RuanWifi.cpp votes the shared rails, and the WPSS has already brought the WCN6750 out of
reset. This is asteroids' order. Without Wi-Fi the rails may be off: the `start … rails=` line shows 0x1f when all five are voted.

### Userland for stage 4 (written 2026-11-21; built offline, never run)

`port/scripts/build-bluetooth-userspace.py OUT [--into OVERLAY] [--root-tree ROOT]` makes a root overlay, as build-wifi-userspace.py does:

- **`overlay/usr/sbin/BlueTool`** is QcaBluetooth `userland/BlueTool.c`, unchanged.
  - It is built for `arm64-apple-ios17.0` with the theos SDK and lld. The SDK has no `<xpc/xpc.h>`, so `port/userland/xpc-shim`
    declares the calls libSystem exports.
  - It is signed with ldid (identifier `com.apple.BlueTool`, entitlement `no-container`).
  - Apple's job (UserName mobile, `-R`) starts it. It answers `boot`/`init` by reading `kern.qbt_power_on`. **[V: builds, signs]**
- **`overlay/usr/sbin/bluetoothd`** is 21H461's own daemon, patched by `patch-bluetoothd.py`.
  - It accepts only the unchanged original (sha256 `3f1e4493…8df4`, the same as `ramroot/work-full/root`).
  - Every site is checked word for word, and a mismatch refuses.
  - It re-signs with ldid, keeping `com.apple.bluetoothd` and the original entitlements (the script compares them after signing).
    **[V: patched, signed, entitlements unchanged, every site and the cave disassembled and the branch targets checked]**
- **`OUT/cdhashes`** gets both CDHashes: BlueTool `0f0b5b8e…63b8`, bluetoothd `316301be…392a` (the test build in /tmp; no Apple
  bytes go to git).

21H461 sites, against renoir's 9 (iOS 16):

| renoir | 21H461 | Patch |
|---|---|---|
| 1 factory (3× `movz w1,#0xf`) | Every platform, including the `BT_CHIP=ACI BT` env path and the default (`mov w1,#1` at 0x10031ccc8), constructs its controller through the thunk 0x1002d50e4 (`b 0x1002d4680`, w1 = chip; 77 call sites) | thunk → cave: `mov w1,#0; b 0x1002d4680` |
| 2 registration 1..0xf9f | 0x1001e1dc8 `sub w9,w0,#1; cmp w9,#0xf9e` | `cmp w0,#0xf9f; nop` |
| 3 vendor init chip 0 | 0x1001e1cdc: in 17 one function dispatches 1..0x7cf to BCM, 0x7d0..0xf9f to ACI, else the error at 0x1001e1d68 | chip 0: bzero the op table 0x100931940 (0x878 B, both backends' size), clear the event handler 0x1009321b8, return; other ids keep the error |
| 4 callbacks chip 0 | 0x1001e1f9c, error at 0x1001e2004 (generic callback already saved at 0x1009321e8) | chip 0 returns |
| 5 0x0C5B optional step | step 0x1001840a0 → CB 0x100184cf4 → advance 0x100181a48 (status ≠ 0 fails the chain via 0x10023e254) | the CB advances with status 0 (0x100184d4c) |
| (iOS 17 extra) Host Flow Control | CB 0x100184c84 only tolerated status 0x2cd | always advances with 0 (0x100184c90) |
| 6 H4 pre-stop | 0x10015f238 `br [transport+0x60]` with no null check; the H4 table 0x100925708 has +0x60 = 0 | `ldr x0,[x8,#0x60]; cbz x0,ret; br x0` |
| 7 LE-features publication | end of `initCmdReadLeLocalSupportedFeaturesCB` at 0x1001846ec | located, **not applied** |
| 8 LeBroadcaster start | not located (no `stackDidStart` string in 17; it has to be found through the vtable) | — |
| 9 LC_LOAD_DYLIB Type0 | needs a 21H461 port of renoir's `renoir_bt_type0.c` | — |

Sites 7–9 only publish extended LE capabilities. Without them the stack keeps its defaults: legacy scan and advertising, classic
pairing. On renoir their absence caused an advertising-retry loop; watch for that in stage 4 (stage 5b if needed).

Other iOS 17 checks seen, left alone: callers of `get chip` 0x1001e1dbc test the BCM/ACI ranges (chip 0 = neither, as on renoir);
the controller constructor sets a flag (+0xa0) only for BCM/ACI ids; "Couldn't find a platform / chipset / firmware combination" at
0x1006cb0a4 sits on the BlueTool-result path, which our BlueTool answers with result 0.

**Root-build hook** for the main session (after the Wi-Fi overlay, into the same overlay directory):

```sh
cd macOS/unixnu-ruan
python3 scripts/build-wifi-userspace.py /root/unixnu-work/ovl-work --into /root/unixnu-work/ovl     # existing
python3 scripts/build-bluetooth-userspace.py /root/unixnu-work/ovl-work --into /root/unixnu-work/ovl --root-tree /root/unixnu-work/ramroot/work-full/root
REBUILD_ROOT=1 OVERLAY=/root/unixnu-work/ovl scripts/build-ramroot.sh ...   # overlay/ joins the root; cdhashes -> root-usb.cdhash -> trust.bundle
```

The UFS root takes the same make-ramroot.py output (make-ufs-root.py), so the same `OVERLAY=` applies.

## 3. Apply (in the session that owns `macOS/unixnu-ruan`)

1. Copy the port files:
   - `cp port/package/* unixnu-ruan/package/`
   - `cp port/scripts/* unixnu-ruan/scripts/`
   - `mkdir -p unixnu-ruan/userland && cp -r port/userland/xpc-shim unixnu-ruan/userland/` (BlueTool's header shim)
   - No name collides. FatVolume, SmmuBypass and the Qcom* files are the Wi-Fi port's, used as they are.
2. `cd unixnu-ruan && patch -p1 < ../docs/research/bluetooth/port/patches/bt-port.patch`
   - The patch covers the `bluetooth` DT node, the BootProgress.c `ruan_bt_start`, the late probe's `ruan_bt_init`, the USB `bt` and
     `bt.log` commands, and the build-ram-root-probe.py objects.
   - `--dry-run` is clean against the working tree as of 2026-11-21.
3. Build as usual. The first build runs `scripts/fetch-qca-bluetooth.sh` (GitHub only) into `.cache/QcaBluetooth` (git-ignored). That
   script also fetches Linux v6.18.60 `btqca.c` and checks its hash.
4. Optional: `python3 docs/research/bluetooth/port/check/syntax-check.py` (all 8 PASS on 2026-11-21).

## 4. USB test steps

1. Build with `KERNEL_ARGS="ruan-bt=1"` (stage 2: `ruan-bt=2`; stages 3–4: `ruan-bt=3 ruan-wifi=3`, plus `log-bluetooth=1` for the
   controller log, or `log-bluetooth=2` for the boot capture).
   - `ruan-bt-delay=<s>` (default 20) sets the BootProgress start. The late probe starts at once, and whichever comes first runs.
2. Run the usual cycle: `fastboot boot` via zima (RAM only, nothing flashed).
3. Open the USB debug terminal (`ruan-usbdbg=1` is a default) and run `bt` (one line) and `bt.log` (the last 32 lines). The camera
   check follows the usual cycle.
4. Stage 4 additionally needs the userland of section 2. Then: Settings → Bluetooth. Run `bt` again: tx/rx counters climb while it
   scans.

## 5. Risks

- **Bus stall on an unclocked QUP read.** Stage 1 reads SE registers only when the clocks are already running. Stage 2 votes them first.
- **Shared GCC vote 0x62008 with touch (QUP1 SE3).** On power-off, BT clears only its SE5 bit and the bus bits it added itself.
  RuanTouch enables SE3 through its own CBCRs, not the vote. [V by code]
- **SMMU:** RuanWifi.cpp plans its bypass slots without the `AppsSmmuLock` that BT takes. BT starts after the WPSS is ready, so they do
  not race in practice (a comment marks this shortcut). The 0x403 stream may already have a context assigned by Gunyah. Stage 2's
  `dma` line reports it. [I]
- **Gunyah ownership** of QUP1 SE5, TLMM 14–17/35/61 and the summary IRQ: Android HLOS uses all of them for BT, so they should be
  HLOS-owned. [I]
- **The RCG at 51.2 MHz without a CX vote** (Linux votes CX per rate). This is below the 75 MHz low-corner limit. [I]
- **Supplies:** BT relies on Wi-Fi's votes. A BT-only boot can fail at stage 3 with timeouts. The `rails=` line in the log tells. [I]
- **iOS 17 daemon:**
  - The patch is checked statically only. A chip-0 path may still hit a BCM/ACI-only assumption at runtime; the bluetoothd log in
    stage 4 tells.
  - Replaced Apple jobs run ad-hoc binaries from the owner trust module. If AMFI refuses them with a launch-constraint violation, use
    `no-lc=1` (patches/xnu zzyc). If it refuses the private entitlements of the re-signed bluetoothd, that is a new blocker. [I]
  - A2DP needs `PRIV_SKYWALK_REGISTER_USER_PIPE` (renoir patch 0140). [I]
- **Not checked:** whether `/chosen/unique-chip-id` is present on the UFS variant (RuanWifi.cpp relies on it too); behaviour of the
  ruan TLMM summary line when it is shared with other drivers.

## 6. Verified vs inferred (summary)

- **Verified:**
  - DT wiring, GCC/pinctrl constants from parrot sources, firmware file names in the stock BTFM.bin.
  - iOS 17 daemon transport strings and its plain-arm64 layout; the chip dispatch sites above.
  - Syntax and object compile of all port sources with ruan's flags; symbol resolution.
  - `patch --dry-run` of the integration patch, and a syntax check of the patched UsbDebugCdc.cpp, LegacyDisplayProbe.cpp and
    BootProgress.c.
- **Inferred:** everything on hardware (no run on ruan); 51.2 MHz without a CX vote; SMMU slot availability; the iOS 17 patch size.
