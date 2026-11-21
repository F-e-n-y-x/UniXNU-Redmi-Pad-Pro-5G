# Wi-Fi join: prior art and the 20 s delay (2026-11-21)

Read-only research. Labels: **[V]** verified in source or logs, **[I]** inferred.

## 1. The 20 s is in the kernel module, not in wpa_supplicant

**[V] Logs.** Every boot in `logs/rbu185u/wpa.log` shows `DISCONNECT`, then `REMOVE_NETWORK all … SELECT_NETWORK 0` exactly **20.18 s** later (for example 351.614 → 371.790, 420.759 → 440.935, 697.005 → 717.183). After that, the supplicant connects in 0.1–3 s. One run had "Fast associate: Old scan results" and reached COMPLETED 0.47 s after SELECT. The kernel log in `rbu180u` shows that wifid sends type 22 (leave) and then type 20 (join) only **15 ms** apart. So the time is lost between WifiNet's `nw_wifi_connect()` and the moment the JOIN reaches the supplicant. That is `nw_mainline_run()` → `nw_cfg80211_begin_attempt()`, which runs `dev_close()` + `dev_open()` (`.cache/Ath11kWifi/src/nw_cfg80211_control.c:400`, `nw_mainline.c:357/366`). Each JOIN and each DISCONNECT runs it again.

**[V] Where ath11k waits 20 s** (`out/ath11k-src`):
- `core.h:64` `#define ATH11K_SCAN_TIMEOUT_HZ (20 * HZ)`.
- `reg.c:816/830` `ath11k_regd_update_chan_list_work()` waits up to 20 s for `completed_11d_scan` (while `state_11d != IDLE`) or for `scan.completed`.
- `mac.c:7276` `add_interface` (on `dev_open`) sets `state_11d = ATH11K_11D_PREPARING` when the firmware advertises `WMI_TLV_SERVICE_11D_OFFLOAD`. `mac.c:6709` `op_start` queues the channel-list update. PREPARING is cleared only by a later hw_scan or by the 11d scan start (`mac.c:4206/4347`).
- `mac.c:6772-6780` `ath11k_mac_op_stop()` (on `dev_close`) runs `cancel_work_sync(&ar->channel_update_work)` **before** it completes `completed_11d_scan`. A `dev_close` that follows a `dev_open` therefore blocks until the 20 s wait in the worker times out.

**[I]** The leave's interface cycle leaves the worker waiting. The join's `dev_close` then waits about 20 s for it. 20 s plus the work and event delays fits 20.18 s. You can confirm this with the existing log line `"wifi: request %u to the supplicant took %llu ms"` (added by `patch-ath11k-userspace.py` hunk 4, never captured so far), and with ath11k's `reg 11d scan wait left time` debug line.

Your list of wpa_supplicant knobs (`fast_reauth`, `bss_expiration_age`, p2p, `scan_cur_freq`, sched_scan) does not apply here. The supplicant never waited. The only real supplicant stall, `Delay radio work start until externally triggered scan completes`, is already fixed by `nl_scan_event = 1` (patch hunk 1). It still appears in the older-build segments that are concatenated into the same wpa.log.

## 2. Ranked changes

1. **[I, high] Stop cycling the interface per request.** In `scripts/patch-ath11k-userspace.py`, add a hunk to `src/nw_mainline.c` `nw_mainline_run()` that drops the `nw_cfg80211_begin_attempt()` call in the JOIN and DISCONNECT branches. Rebuild the builder with `nw_cfg80211_event_begin` as now. `REMOVE_NETWORK all` + `SELECT_NETWORK` in `userspace/nw_supplicant_frontend.c:join()` already resets the SME, PMK and keys. The supplicant then gets the join within milliseconds and uses the held scan ("Fast associate"), about 0.5 s in all.
2. **[V source, I effect] Or fix the ath11k ordering** (smaller and upstream-shaped): in `ath11k_mac_op_stop()`, move the `if (ar->state_11d == ATH11K_11D_PREPARING) { … complete(&ar->completed_11d_scan); }` block above `cancel_work_sync(&ar->channel_update_work)`. As a fallback, clear `WMI_TLV_SERVICE_11D_OFFLOAD` in the svc_map, so that PREPARING is never set (ruan has no 11d need).
3. **[V] WifiNet already handles the idle leave** (`disconnect_user()`: "leave while idle: the module is not asked", present in the source but **not** in the rbu185u build: that build still returned 102 for 365). Rebuild before testing (1).
4. **[I] After the join.** Once the join finishes inside wifid's window, recheck Settings and the status bar before adding more ioctls. In rbu185u the assoc result (type 21 = 1), event 9, event 20 (RSN_HANDSHAKE_DONE), the SSID, BSSID and link events and DHCP all happened, but about 20 s late, so wifid had already reported "Unable to join". Gets that are still refused: 346 (1032 B), 387 (37 B), 386 (5536 B). Real drivers refuse many feature gets. AirportItlwm returns errors for most of the gets it does not support.

## 3. Prior art

- **AirportItlwm (OpenIntelWireless/itlwm)** [V, cloned]: event numbers come from `include/Airport/apple80211_var.h:618-681`: 1 POWER_CHANGED, 2 SSID, 3 BSSID, 4 LINK_CHANGED, 9 ASSOC_DONE, 10 SCAN_DONE, 17 LINK_QUALITY, 20 RSN_HANDSHAKE_DONE, 30 ROAMED, 32 DEAUTH_RECEIVED, 39 RSSI_CHANGED. Join flow: net80211 `IEEE80211_EVT_STA_ASSOC_DONE` → `postMessage(ASSOC_DONE)` (`AirportSTAIOCTL.cpp:1203`). `setLinkState(up)` + LINK_CHANGED. `RSN_HANDSHAKE_DONE` is posted when the PTK (key_flags 4) or GTK (0) is installed through `setCIPHER_KEY` (`:294/298`). That is Apple's userspace supplicant mode. Without `USE_APPLE_SUPPLICANT`, net80211's in-kext supplicant does the 4-way. `getASSOCIATE_RESULT` returns SUCCESS(1) only in RUN, otherwise an error. Result codes (`apple80211_var.h:168-193`): 1 success, 2 unspecified, 9 timeout, 21 INVALID_PMK, 22 SUPPLICANT_TIMEOUT. Our `kAssocBadKey = 21` matches INVALID_PMK.
- **UniXNU-asteroids** (iOS 27, same WifiNet and ath11k): wifi91 "Settings shows Wi-Fi and joins from its page". It still lists "Apple80211 requests answered 102" as open. No join-timing fix is in its history.
- **UniXNU-sheng / Dizi (ath12k)**: they have the same `begin_attempt` cycle. Sheng's `reports/ipados27-wifi-association.md` covers the 540-byte 24A446 assoc event and a 30 s join deadline. ath12k does not have this ath11k 11d wait. That is likely why they don't see the delay [I].
- **HakiOS (apollo)**: "lists and joins Wi-Fi networks from Settings" (UniXNU/docs/devices/apollo.md). No mechanism is documented there.
- **QEMU (qemu-t8030, xnu-qemu-arm64) / Corellium**: not checked in depth. I know of no Apple80211 emulation in them [I].

## 4. Apple80211 ioctl numbers

Public headers (itlwm `apple80211_ioctl.h`) stop at 353 (NSS): 20 ASSOCIATE, 21 ASSOCIATE_RESULT, 22 DISASSOCIATE, 345 HE_CAPABILITY, 347 SOFTAP_PARAMS. **346 is missing** from that header. **No public header defines 365, 386, 387, 390, 470 or 471.**

The iOS 17 shared cache (`dsc/dyld_shared_cache_arm64.29`) has a 490-entry `APPLE80211_IOC_*` name table. It lines up with the header (index − 2 up to 113, a gap at 114, then index − 1 up to 353). Past 353 the alignment cannot be checked: sheng's ABORT_SCAN = 382 implies offset 0 there. Candidates **[I, ±1]**:

| no. | candidate | fit with the observed buffer |
|---|---|---|
| 346 | BEACON_INFO (or HE_CAPABILITY) | 1032 B get |
| 365 | DISPLAY_STATE | set, val 1 |
| 386 | COLOCATED_NETWORK_SCOPE_ID / LQM_SUMMARY | 5536 B get |
| 387 | ROAM_CACHE_UPDATE | 37 B get |
| 390 | BEACON_SCAN_CACHE_REQ / SLOW_WIFI_FEATURE_ENABLED | 1228 B (BSS record) |
| 470/471 | MOVING_NETWORK / BATTERY_POWERSAVE_CONFIG / MIMO_CONFIG | 1 B set |

To make these exact, disassemble the number→name switch in IO80211 or CoreWiFi (the user of this table).
