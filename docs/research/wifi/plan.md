# Wi-Fi on ruan (WCN6750 behind the WPSS, under Gunyah): plan

Status 2026-11-21: research and drafts only. Nothing here has been built, booted or run on the tablet.
Tags: **[V]** = verified (read in a file, the live DT, the firmware image or a working port's source and test record). **[I]** = inferred (reasoned from Linux sources or a sibling board, not checked on ruan).

## 0. Bottom line

- **A working port of this exact chain already exists.** filipnavara/UniXNU-asteroids (Nothing Phone 3a, SM7635, also under Gunyah) runs WCN6750 Wi-Fi on UniXNU. **[V]**
  - Its record (UniXNU `docs/devices/asteroids-filipnavara.md`, wifi91, 2026-11-13) shows WPA2 association, DHCP from iOS's own client, traffic, wifid and the Settings join page all working.
  - Its chain: `WpssProbe.cpp` (PAS, SMP2P, GLINK, QRTR), then `drivers/Ath11kWifi` (Linux ath11k + mac80211 + cfg80211 on the UniXNU Ath12kWifi runtime, plus wpa_supplicant 2.12), then sheng's `WifiNet.cpp` (en0 + Apple80211).
- **ruan's WPSS DT matches the 3a's almost value for value.** **[V]**
  - Same on both: SMP2P items 617/616, remote pid 13, IPCC client 24 (signals 0 and 2), GLINK `IPCRTR` intents, `qca6750/wpss.mdt`, the 0x1900000 carveout size, RSC DRV2 0x17a20000 with TCS offset 0xd00, and 32 CE SPIs from 0x300.
  - So this is a port of a proven stack: change constants, then debug the board-specific parts (supplies, sleep clock, DMA window, SMMU stream). There is no driver to write from scratch.
- **iOS exposure:** use UniXNU's own design: an ifnet `en0` with Apple80211 answers, the supplicant in userland, and iOS's own DHCP. Do not reimplement Apple's IO80211Family. Section 4 covers this.
- **Effort:** about 2–4 weeks of test cycles for stages 1–5 (section 6). The largest unknowns are the ruan-specific supplies and sleep clock, and how Apple80211 behaves on 17.7.11.
- **Ready now:** stage 1 draft code in `draft/`. It needs `ruan-wifi=2`, a build, and a tablet test cycle that has to be cleared with the user and the ROM session first.

## 1. Facts

### 1.1 Live DT (`hardware/private/ruan-live.dts`) [V]

| Item | ruan | asteroids (3a), for comparison |
|---|---|---|
| WPSS remoteproc | `remoteproc-wpss@8a00000`, `qcom,parrot-wpss-pas`, `firmware-name = "qca6750/wpss.mdt"` | `qcom,volcano-wpss-pas`, same firmware name |
| WPSS carveout | `wpss_region@85300000` + 0x1900000 | 0x9bb00000 + 0x1900000 |
| Proxy rails | cx = pm6450_s1_level, mx = pm6150l_s3_level, both at level 0x180 (384); AOSS `qcom,qmp` | cx.lvl and mx.lvl at 384 |
| WPSS interrupts | wdog SPI 0x24b; fatal/ready/handover/stop-ack/shutdown-ack from smp2p `slave-kernel` bits 0/1/2/3/7 | same bits |
| SMP2P | `qcom,smp2p-wpss`: smem <617 616>, remote pid 13, IPCC 0x18 signal 2. Entries: master-kernel, slave-kernel, wlan, wlan_soc_wake (in and out), wlan_ep_power_save (out) | identical |
| GLINK | `glink-edge` transport smem, IPCC 0x18 signal 0, channel `IPCRTR`, intents 0x800×5, 0x2000×3, 0x4400×2 | identical |
| IPCC | `qcom,ipcc@ed18000`, SPI 0xe5 (229) | 0x405000, SPI 229 |
| SMEM / mutex | `smem_region@80900000` + 0x200000; TCSR mutex syscon 0x1f40000, lock 3 | different base |
| AOSS QMP | `power-controller@c300000` (+0x400), IPCC client 0 signal 0 | same |
| RPMh | apps_rsc DRV2 0x17a20000, tcs-offset 0xd00; cmd-db `aop_cmd_db_region@80860000` + 0x20000 | cmd-db 0x81c60000 |
| WLAN node | `qcom,wcn6750`: reg msi_addr **0x17210040**, smmu_iova_ipa 0xb0000000+0x10000; `iommus = <&apps_smmu 0x1c00 0x1>`; dma-coherent; IOVA pool 0xa0000000+0x10000000; interrupts SPI **0x300–0x31f** edge; `qcom,wlan-msa-fixed-region` = wlan_fw_region 0x82a00000+0xc00000; `use-nv-mac`; `qcom,vreg_ol_cpr = "s7b"` | msi 0x17110040, SMMU 0x1400/0x1401, same SPIs |
| WCN supplies | vdd-cx-mx = **smpb7** (880–1120 mV config), vdd-1.8-xo = **smpe8** (1872 mV), vdd-1.3-rfa = **smpb8** (1256 mV). BT node adds ldob22 (I/O 1.8 V) and smpb9 (ipa 2.2 V) | smpb3 / smpb1 / smpb2, plus a GPIO-switched 2.2 V |
| WLAN enable | `bt_wcn6750`: `qcom,wl-reset-gpio = <&tlmm 36>`, bt-reset 35, sw-ctrl 61; TLMM `pinctrl@f000000` | TLMM 0xf100000, pin 44 |
| Sleep clock | no pmk8350 pinctrl state for a WCN sleep clock in the live DT (pmk8350 `pinctrl@b000` has no states) | PMK8550 GPIO5, which UEFI leaves off |
| Low DMA candidate | `mpss_region@8bc00000` + 0xe600000 (230 MiB). Kept out of memory-outside by board.toml `owned_memory_outside_reserved = 0x80000000:0x80000000` | the 3a's mpss carveout (also 230 MiB) |
| Kernel cmdline | `icnss2.hwname=ruan icnss2.hwc=India icnss2.hwarea=n83unotgl` (Xiaomi BDF selection inputs) | n/a |

### 1.2 Firmware (device's own, never in git) [V]

- **Where it is:** `NON-HLOS.bin` (the modem_a FAT16 image, for example `work/release/OneUI8.5-ruan-v2.5-b40-20261110/images/NON-HLOS.bin`, listed with `7z l`, nothing extracted). Its `image/qca6750/` holds:
  - **WPSS image:** `wpss.mdt` (4844 B) + `wpss.b00`–`b10`, `b12` (`b06` is 5.8 MB).
  - **Board data:** `bdwlan.elf`, `bdwlan.e01`–`e17`, and Xiaomi's region files `bd83u{in,gl,cn,ce,fcc,jp}.elf`.
  - **Regulatory database:** `regdb.bin`, `regdb_6g.bin`.
  - `image/adrastea/` is a WCN3990 set. Do not use it.
- **What WCN6750 does not use:** `amss.bin` and `m3.bin`. The old note `research/wifi-wcn6750.md` lists them, and is wrong there. On WCN6750 the WLAN firmware is the WPSS image, and ath11k sends only board data and regdb over QMI. That matches asteroids' README. **[V for asteroids, I for ruan]**
- **Other Wi-Fi files:** `vendor_a/etc/wifi/qca6750/WCNSS_qcom_cfg.ini` and `vendor_a/firmware/wlan/qca_cld/qca6750/wlan_mac.bin` belong to qcacld. ath11k does not need them. `wlan_mac.bin` is a possible MAC source.
- **BDF choice:** `bd83uin.elf` fits an India unit (`hwc=India`), with `bdwlan.elf` as the fallback. **[I]** Xiaomi's icnss2 patch picks the file by hwc. Make it a boot argument (`ruan-wifi-bdf=`).

### 1.3 ruan code that already exists [V]

- **PAS from XNU at EL1 under Gunyah works on ruan:** the A710 zap shader, PAS 13, in `RuanAdrenoGlue.cpp` and GPU-METAL-PORT.md "Milestone" item 6. Lessons from it:
  - `is_supported` answered 0, but the calls worked anyway.
  - The load segments had to be written **before** init_image/mem_setup. Writing them after mem_setup reset the SoC (rbu59a).
  - The metadata could live above or below 4 GiB. With an SHM bridge, init_image failed.
- **Other pieces already in the tree:**
  - `RuanZap.h`: `ruan_smc_call` plus the SCM encoders.
  - `SmmuBypass.h`: sheng's pass-through bank, already used for UFS on this board.
  - `armcpu_spi_attach`: used by RuanBacklightSvc and the GPU IRQ.
  - ShengUfs 2f6c7b0 (ruan's pin plus the dizi patch) includes `native_ufs_part_find/read`, a GPT partition reader over LUNs 0–5. Checked in vildangil-dizi's checkout of the same commit.
- **`RuanWifiProbe.c` is dead code.** It is compiled into libRamRootProbe.a, but nothing calls `ruan_wifi_init`. The stage-1 hook patch drops its compile line.

## 2. The bring-up chain (what Linux does, and what runs on XNU)

1. **Power and clocks:**
   - AOSS QMP `{class: image, res: load_state, name: wpss, val: on}`.
   - RPMh ARC votes cx.lvl/mx.lvl at 384 (proxy votes, dropped at the WPSS "handover" bit).
   - WCN rails as RPMh VRM votes: mode, mV, enable.
   - WL_EN GPIO high.
   - WCN sleep clock, if the board needs one.
   - Linux reference: `qcom_q6v5_pas.c` (sc7280 wpss: PAS id 6, crash reason SMEM 626), and icnss2 for the WCN rails.
2. **WPSS boot through SCM PAS 6:**
   - The PAS calls: init_image(metadata = ELF headers + hash segment), mem_setup(carveout, span), copy the loads into `wpss_region`, then auth_and_reset.
   - First, allocate SMEM item 616 (our SMP2P out) with entries master-kernel/wlan/wlan_soc_wake/wlan_ep_power_save, and ring IPCC 24/2.
   - Ready = slave-kernel bit 1, fatal = bit 0. The crash text is in SMEM item 626.
3. **GLINK over SMEM:**
   - Items 478 (descriptor), 479 (our FIFO, 0x4000) and 480 (the WPSS's FIFO) in the partition apps↔pid 13. Doorbell IPCC 24/0.
   - Version negotiation, then the WPSS opens `IPCRTR`.
4. **QRTR:** hello/bye, and the name service. The WPSS announces WLFW (service 69). Stage 1 ends here.
5. **QMI WLFW (ath11k `qmi.c`):**
   - Indication register, then host capability.
   - The firmware's memory request: the host answers with DMA chunks.
   - BDF download (ELF board data), then regdb download.
   - Device info: the register window by physical address.
   - wlan_cfg (CE pipe config), mission mode, then **FW_READY_IND**.
6. **ath11k "hybrid" AHB bus:**
   - CE and SRNG/HAL rings in host DMA memory, reached by the chip through apps SMMU SID 0x1c00/0x1c01.
   - Interrupts: the chip writes vector numbers to msi_addr (a GIC register), which raises SPI 0x300+n (INTID 800+n).
   - WMI then HTT over the CEs, then mac80211 `add_interface`, scan, connect, data path (REO/TCL rings).
7. **Userland:**
   - wpa_supplicant (UniXNU's `unixnu-wpa_supplicant`, through the `org.unixnu.wifi.control` kernel-control endpoint) does WPA2/SAE.
   - WifiNet registers `en0` (ifnet KPI) and answers wifid's Apple80211 ioctls.
   - iOS configd/IPConfiguration runs DHCP on en0.
   - wifid needs a `/arm-io/wlan` DT node, plus a MAC in `/chosen/mac-address-wifi0` and the wlan node's `local-mac-address` for MobileGestalt and Settings. **[V asteroids wifi89–91]**

**MSA (`wlan_fw_region` 0x82a00000):** in the TZ-PAS flow (no `wifi-firmware` subnode, so ath11k uses `use_tz`) the host does not program the firmware's SMMU or MSA. asteroids never touches its wlan_fw region. **[I for ruan]** Leave the region alone, as the DT's `no-map` says.

## 3. What UniXNU and its forks provide

| Piece | Source | Use on ruan |
|---|---|---|
| WPSS boot + GLINK + QRTR router, raw-packet link for the module | asteroids `package/WpssProbe.cpp` (532 lines) | **Unchanged** apart from `draft/WpssProbe-ruan.patch` (carveout, TLMM, WL_EN pin, boot-argument names) |
| Host-thread services WpssProbe needs (IPCC, SMEM+mutex, QMP, RPMh, PAS) | asteroids `AdspProbe.cpp` (lends them through `AsteroidsRemote.h`) | ruan has no ADSP thread, so **`draft/RuanWifiWpss.cpp`** implements `AsteroidsRemote.h` standalone, polled, with `ruan_smc_call` |
| SMEM / SMP2P / GLINK / QRTR / QMP / RPMh / MDT helpers | sheng and asteroids `package/Qcom*.c/.h` (BSD-2-Clause-Patent, host-tested) | copy unchanged into `package/` |
| Firmware from modem_a in place (FAT16 over the UFS partition reader) | asteroids `AdspFirmware.c` + `FatVolume.c` (`modem:qca6750/wpss`) | copy unchanged |
| ath11k + mac80211 + cfg80211 + supplicant runtime | asteroids `drivers/Ath11kWifi` (fork of UniXNU/Ath12kWifi 5f7f5a8). Linux v7.0.14 pinned; `nw_ahb.c` hybrid bus; `nw_board.qrtr_send`, `msi_addr`, `msi_spi_first` | stage 2: build with `scripts/build-ath11k-wifi.py` |
| Board glue: SMMU bypass, low DMA window, MSI SPIs, VRM votes, MAC to DT, log ring, join.txt | asteroids `package/AsteroidsWifi.cpp` (1047 lines) | stage 2: copy as `RuanWifi.cpp` and change the constants (section 5) |
| en0 + Apple80211 + DHCP hand-off | sheng `package/WifiNet.cpp` (same in asteroids and j716f) | stage 3–5, unchanged at first |
| Signed `unixnu-wpa_supplicant` payload + launchd job merge | asteroids/sheng `scripts/build-wifi-userspace.py`; j716f bundles it on **iPadOS 17.7.11** | stage 4–5 |
| QCA6390 (ath11k-class chip) on 17.7.11 | j716f record: en0, wifid and supplicant running (association not recorded) | evidence for the 17.7.11 userland side |

UniXNU core (`/root/unixnu-work/UniXNU`) has `docs/drivers/ath12k-wifi.md` and `docs/porting/network.md`, which describe the `nw_abi.h` contract. Core `drivers/` has no Wi-Fi driver of its own. The forks carry it.

## 4. How iOS sees the interface

- **Apple's own stack:** IO80211Family plus the Broadcom/Apple driver ABI, with wifid talking to an `IO80211Interface`. It expects Apple silicon radios and private firmware ABIs. Reimplementing it for ath11k is out of scope.
- **What UniXNU does instead [V]:**
  - The kernel owns the 802.11 stack: Linux mac80211/cfg80211 compiled into XNU.
  - `WifiNet.cpp` creates a plain **ifnet `en0`** (`ifnet_allocate`, Ethernet framing, MTU 1500). It registers the classes and properties that make wifid treat it as the Wi-Fi interface, and answers Apple80211 requests (scan, associate, state, RSSI) from `nw_abi.h`.
  - **WPA runs in userland:** `/usr/libexec/unixnu-wpa_supplicant` (wpa_supplicant 2.12 with a `driver_sheng.c` backend) talks to the kernel over the `org.unixnu.wifi.control` kernel-control socket. Settings/wifid provide the network selection and password.
  - **IP:** once the link is up, iOS's own IPConfiguration runs DHCP on en0. asteroids got DHCP (wifi87/88) **before** wifid worked (wifi89/90), so stage 5's IP check does not depend on Apple80211.
- **Fallback for tests:** `wifi-aj=1` plus a Data add-on `join.txt` (SSID and key, test images only, never committed). The kernel worker then joins without wifid. Stages 4–5 can use this regardless of Apple80211.
- **17.7.11 caveat [I]:** WifiNet's join-result layout is the 24A446 (iOS 27) one (540 bytes, result at 0x214). j716f ships the same layout on 17.7.11 and its record does not show a Settings join. Expect to adjust Apple80211 structure sizes for 21H461 in stage 5. Joins through join.txt are unaffected.

## 5. Port map: asteroids to ruan

| Constant / step | asteroids | ruan |
|---|---|---|
| `kWpssBase` | 0x9bb00000 | **0x85300000** (patch) |
| TLMM / WL_EN | 0xf100000 / 44 | **0xf000000 / 36** (patch; 0x1000 per pin assumed **[I]**) |
| SMEM, mutex, IPCC, cmd-db | 0x81d00000…, 0x405000, 0x81c60000 | **0x80900000, 0x1f43000, 0xed18000, 0x80860000** (RuanWifiWpss.cpp) |
| RPMh TCS | ADSP TCS 1, Wi-Fi VRMs TCS 2 | RuanTouch owns **TCS 0**; WPSS rails **TCS 1**, WLAN VRMs **TCS 2** |
| VRMs | smpb3 968 mV, smpb1 1856 mV, smpb2 1256 mV, plus a PMR735B GPIO1 PA enable | **smpb7** (880 mV min; the CPR value from QMI `nw_wifi_cpr_mv`), **smpe8** 1872 mV, **smpb8** 1256 mV, **smpb9** 2200 mV (ipa 2.2 V) as a VRM vote instead of a GPIO; maybe **ldob22** 1800 mV |
| Sleep clock | PMK8550 GPIO5 func1 | unknown **[I]**; stub. If the stage-3 scan asserts (`whalRxMpduPop`, `whalRecvDmaLeakRecovery`), look at PMK8350 GPIOs (`pinctrl@b000`) as Android leaves them (read-only adb dump, only with the ROM session's OK) |
| SMMU streams | 0x1400, 0x1401 (boot-loader bank 70 redirected to the bypass bank) | **0x1c00, 0x1c01**; same redirect logic; plain LDR/STR only (Gunyah traps the SMMU) |
| MSI | 0x17110040, SPI 0x300, 16 of 32 vectors | **0x17210040**, SPI 0x300 (INTID 800+), the same 16 vectors (CE 0–6, DP 10–12, 14, 16–20). Check that ArmCpu's 24 SPI slots are not used up |
| DMA window | 48 MiB at 0x8ac00000 (mpss); 32-bit DMA mask required | **48 MiB at 0x8c000000** (inside mpss_region; 0x8bc00000 + 8 KiB holds the stage-1 PAS metadata fallback) |
| Firmware paths | `modem:qca6750/wpss`, regdb + board data from vendor_b | `modem:qca6750/wpss`; BDF `bd83uin.elf` or `bdwlan.elf`, regdb from modem_a `image/qca6750/` (FAT reader) |
| Boot args | `wifi`, `wifi-fw`, `wifi-wlen` | **`ruan-wifi`**, `ruan-wifi-fw`, `ruan-wifi-wlen` |

## 6. Staged plan (USB debug channel `wifi`, ramoops replay as backup)

Every stage is a RAM-only `fastboot boot` of a new build (flash nothing, no slot changes). Each cycle needs the user's OK and the ROM session's agreement (AGENTS.md: no tablet reboots without asking redmi-pad-pro-5g-b4).

| Stage | Boot args | Work | Pass marker (USB `wifi`) | Effort |
|---|---|---|---|---|
| **0 survey** | `ruan-wifi=1` | Apply `draft/` (copy the asteroids files, both patches) | `st 2 … slave ffffffff items 1xx`: partition with pid 13 present, the WPSS not running (slave −1 or 0). No writes | 0.5 d (one cycle) |
| **1 WPSS boots, acks** | `ruan-wifi=2` | Draft as is | `st 7` (ready) → `8` (GLINK version) → `9` (IPCRTR up) → `10` (QRTR hello) → **`11`, `wlfw 1`+, svc list containing `45:`** (service 69) and `ready <n>ms`. Failure decode: `st -6 err -3/-4/-6` = PAS step (`scm` = x0); `-7 -60` = no ready bit (read `crash`). If the SoC resets during PAS, retry with `ruan-wifi-order=1`, then `ruan-wifi-meta=1` | 1–3 d |
| **2 QMI FW ready** | `ruan-wifi=5 wifi-scan=0` | Port AsteroidsWifi.cpp → `RuanWifi.cpp` (section 5 constants), Ath11kWifi build, link into the probe archive; USB `wifi.log` prints `native_wlan_lines` | log: host cap ok, mem request answered, BDF + regdb downloaded, `FW_READY`, WMI ready, MAC; `native_wlan_stats[0]` ≥ 6 (started), SMMU result bits 0x3, no SMMU faults | 3–5 d |
| **3 scan** | `ruan-wifi=5` | Interface up, the board's self-test scan | `native_wlan_bss` lists nearby SSIDs; interrupts climbing (`[2]`), worker rounds sane. Known asteroids failures: firmware assert at first vdev = WL_EN/supplies; scan asserts = sleep clock; RX DMA leak = window above 4 GiB | 2–5 d |
| **4 WPA2 associate** | `+ wifi-aj=1`, join.txt in Data (test image only) | Supplicant payload (build-wifi-userspace.py), launchd job merge, signature/trust hashes | log: auth, assoc, 4-way done, port authorized (about 100 ms on asteroids); `wifi-net` rows | 1–3 d |
| **5 IP + iOS sees it** | as 4, then without `wifi-aj` | `/arm-io/wlan` node in ruan's dts, MAC to `/chosen/mac-address-wifi0` + `local-mac-address`, 21H461 Apple80211 layout fixes | en0 gets a DHCP address (USB `ifconfig`-style row from WifiNet), packets both ways; wifid opens en0; Settings shows the Wi-Fi page and joins | 3–7 d |

Total: about 10–23 working days, mostly test cycles. Stages 0–1 are about one day of build plus 2–4 boots.

## 7. Risks

1. **Gunyah and PAS 6 [I]:** the hypervisor/TZ could refuse PAS 6 from HLOS, or reset the SoC when HLOS writes the carveout after mem_setup (this happened with the zap). Mitigations:
   - `ruan-wifi-order=1` writes the segments first, as the zap needed.
   - asteroids loads the WPSS in Linux order under Gunyah. That is evidence, not proof.
   - Every PAS step logs its SCM x0 before and after.
2. **A SoC reset during a test.** A bad SCM call or an SMMU register access Gunyah dislikes (SError) reboots the tablet. Mitigations: the RAM-only boot; every SMMU access is a plain LDR/STR (asteroids wifi42–50); stage 0 makes no writes.
3. **Firmware signing [V generally].** `wpss.mdt` is signed for this device's OEM key. Only the device's own modem_a image will authenticate; linux-firmware's `wpss.mbn` will not. The image is read in place, and nothing is copied into git (proprietary-files only).
4. **Contiguous allocations [V on ruan].** Kernel contiguous allocations over one page fail on this kernel. The PAS metadata is about 5 KiB, so the draft falls back to fixed low pages in mpss_region. The 48 MiB DMA window must be a fixed low carveout anyway (the 32-bit DMA mask, which is asteroids' RX-leak lesson).
5. **mpss_region access [I].** It is assumed to be stage-2 mapped for the HLOS VM, because the modem never starts. If Gunyah protects it, choose another reserved low range (asteroids had the same situation and it worked).
6. **Supplies and sleep clock [I].** ruan's WCN rails differ from the 3a's (smpb7/smpe8/smpb8/smpb9), and the sleep-clock source is unknown. These were asteroids' slowest bugs (firmware asserts at the first vdev and during scans).
7. **Interrupt slots [I].** 16 MSI SPIs + IPCC in ArmCpu's 24 SPI slots, next to UFS, GPU, backlight and touch. A polled fallback exists (`wifi-irq=0`, the 1 ms poll).
8. **Kernel size and memory [I].** ath11k + mac80211 + cfg80211 add several MB of kernel code (qcacld did not fit the 3a's code window; ath11k did). ruan's kernel __DATA is already tight. The DMA window is not taken from XNU's RAM.
9. **17.7.11 userland [I].** Apple80211 request and result layouts were tuned for 24A446. The join.txt path and DHCP do not depend on them.
10. **Coordination.** The tablet is shared with the ROM session, so each boot cycle needs explicit OKs.

## 8. Draft files (`draft/`)

- **`RuanWifiWpss.cpp`:** the stage-1 host.
  - Implements `AsteroidsRemote.h` for ruan: IPCC, SMEM + TCSR mutex, AOSS QMP open + send, RPMh ARC votes on TCS 1, PAS boot with selectable order and metadata placement.
  - Uses ruan's existing `ruan_smc_call` (RuanZap.h) and QcomScm.h encoders.
  - Adds a polled thread started by `ruan_wifi_start()`, and `ruan_wifi_report()` for USB.
  - Boot arguments: `ruan-wifi=1|2|5`, `ruan-wifi-delay=S` (20), `ruan-wifi-order=0|1`, `ruan-wifi-meta=0|1`.
  - Checked: a host syntax pass (clang++ -Wall -Wextra, aarch64, stub IOKit headers). Not compiled against XNU.
- **`WpssProbe-ruan.patch`:** asteroids `WpssProbe.cpp` → ruan (carveout 0x85300000, TLMM 0xf000000, WL_EN 36, `ruan-wifi*` argument names).
- **`stage1-hooks.patch`:** applies against the current working tree. It adds:
  - the USB `wifi` command in UsbDebugCdc.cpp;
  - `ruan_wifi_start()` in BootProgress.c `boot_progress_start` (a strong reference, like cpufreq);
  - compile lines in `scripts/build-ram-root-probe.py`, replacing the dead `RuanWifiProbe.c` one.

**Applying (in the session that owns `macOS/unixnu-ruan`):**

1. Copy `WpssProbe.cpp`, `AsteroidsRemote.h`, `Qcom{Glink,Mdt,Qmp,Qrtr,Rpmh,Smem,Smp2p,Scm}.{c,h}`, `AdspFirmware.c` and `FatVolume.{c,h}` from filipnavara/UniXNU-asteroids@9f07af3b `package/` into `unixnu-ruan/package/`. Note the provenance in a header line, as SmmuBypass.h does.
2. Apply `WpssProbe-ruan.patch` and `stage1-hooks.patch`.
3. Copy `draft/RuanWifiWpss.cpp` to `package/`.
4. Build, then boot `ruan-wifi=1`, then `ruan-wifi=2`.

## 9. Open questions to settle on the device (read-only where possible)

- Does PAS 6 need the PAS id that downstream parrot uses? Upstream sc7280 uses 6, and so does asteroids' volcano. If init_image returns non-zero, read `qcom_q6v5_pas` in the device's vendor kernel (`work/analysis-vendor/stock/vendor_dlkm_a`), or read it from Android's dmesg later, with permission.
- Are cx/mx already at NOM from the boot loader? The proxy-consumer votes are never released under XNU. If they are, the rail vote is a no-op.
- What instance does WLFW use? Stage 1 prints it. ath11k checks it against the WCN6750 instance id.
- Which BDF does Xiaomi's icnss2 load for this unit? Android's dmesg ("BDF file name") answers it in one line.
