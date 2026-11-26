# macOS 27.3 ruan boot timeline (cpus=1), read-only analysis 2026-11-26

Note: [ev]/[hb] uptime = arch timer since power-on (NOT reset on warm reboot). Measure from "kernel entry".
Main boot used: live-k4c.log 2nd boot (kernel entry 1275.0, UAU already skipped); cross-checked with live-k4.log + ulog-k4/dec.txt,
live-k6.log, ulog-dt/b3.txt.

| Phase | t (s from kernel entry, k4c) | Dominated by |
|---|---|---|
| Host: fastboot boot + receiver + `fastboot stage` 1.48 GiB | ~-100..0 (kernel entry at uptime ~108 on cold boots) | USB at ~21 MB/s (~74 s); image is 68 MiB data + 1.41 GiB zeros |
| Kernel to launchd | 0..10 | IOKitInitializeTime 1.35 s, UFS publisher 2.3 s, setconf->mountroot 1.3 s (CPU/HW, minor) |
| Boot tasks | 10..25 | applekeystored 1.7, auearlyboot 1.6, cc_fips_test 1.3, init_exclavekit 1.0 s (CPU) |
| Daemons -> WindowServer exec | 25..39 | CPU (exec/dyld on one A55) |
| WindowServer pre-timesync | 39..~90 | k4: 392.7->440.0 no WS logs (47 s): CPU contention (dyld/page-in), no wait seen |
| **WindowServer blocked on clocksyncd** | ~60..313 | **WAIT: configd InterfaceNamer WaitQuiet 240.0 s timeout** (see below) |
| Login: en0 -> SecurityAgent -> Dock/Finder exec | 313..410 | CPU (loginwindow, lsd, cfprefsd); SecurityAgent 1606, Dock 1680, Finder 1685 |
| Finder exec -> menu bar drawn | 410..<=740 | fullshot-k4c-now.png at ~15:37:38 already shows Finder menu bar (wallpaper not yet) |

## The 200 s WindowServer wait (proven, both k4 and b3 ulogs)
- 441.94 WindowServer activates com.apple.timesync.expositor -> clocksyncd spawns 441.99, last line 443.10, silent until 643.56.
- configd InterfaceNamer `*START*` 397.458 -> `*TIMEOUT*` 637.461 = **240.003 s** (b3: 151.035 -> 391.041 = 240.006 s).
  = configd WAIT_QUIET_TIMEOUT_DEFAULT (240 s): a *new* interface is not named until IOKit is quiet.
- 637.50 en0 named -> ifnet_attach en0 637.50 -> IOTimeSync port "Created general notifier" -> 643.56 clocksyncd replies ->
  643.67 WindowServer DisplayExternalSync / iomfb_populate.
- IOKit never goes quiet because (InterfaceNamer busy dump at 637.46):
  - `ArmDTPlatform/defaults` "!matched" busy 289961 ms (since kernel start) — the /defaults DT node nub never finishes matching
  - `IOResources/IOHIDResource/NativeHidInput/IOHIDInterface` busy 188396 ms
- Same 240 s in k6/k4c (en0 attach ~240 s after configd start).

## Other fixed timeouts seen
- wifiFirmwareLoader: "Waiting for firmware to load" 387 -> "Runloop timed out" 687 = 300 s (holds a PreventSystemSleep
  assertion; nothing found waiting on it).
- WindowServer: 128x failed lookup com.apple.server.bluetooth.general.xpc (bluetoothd disabled) — fails at once (error 3), no wait.
- diskmanagementstartup: Preboot update fails -69808 (late, after desktop).
- No waitForMatchingService/ExitTimeOut hit found in the ulogs.

## Fixes, ranked
### Wait/timeout
1. (~240 s) Pre-name en0 so InterfaceNamer does not wait for quiet: make-macos-data.py, after the disabled.plist write (~l.315),
   write `Library/Preferences/SystemConfiguration/NetworkInterfaces.plist`:
   {Interfaces:[{Active:true, 'BSD Name':'en0', IOBuiltin:true, IOInterfaceNamePrefix:'en', IOInterfaceType:6, IOInterfaceUnit:0,
   IOMACAddress:bytes.fromhex('027275610001'), IOPathMatch:'IOService:/ArmDTPlatform/RuanDummyEth/IOEthernetInterface',
   SCNetworkInterfaceType:'Ethernet', SCNetworkInterfaceInfo:{UserDefinedName:'Ethernet'}}], Model:'Mac17,11'}  (MAC is fixed in
   RuanDummyEth). Verify in ulog: "(from database)" and no *TIMEOUT*.
   Belt-and-braces (root change, needs sda22 rebuild): `WaitQuietTimeout` = 5 (real) in
   /System/Library/SystemConfiguration/InterfaceNamer.bundle/Contents/Info.plist.
   Root-cause (also helps any other IOKitWaitQuiet user): stop publishing the `/defaults` DT node as a nub (ArmDTPlatform nub
   creation / make-macos-ufs-board.sh DT) and find what keeps NativeHidInput's IOHIDInterface busy (unmatched dext/kext).
2. (~74 s host) Don't ship 1.41 GiB of zeros: send only the first 72 MiB of root-usb.apfs and let DarwinBoot's UsbRamdisk
   zero-fill the rest up to the descriptor's total (root-usb.desc already carries total size; drop the max-download-size ==
   file_size check in zima-ramroot-macos.sh). Or cheap: lower DATA_SLACK (make-macos-data.py:364, 360448 blocks) to what's needed.
3. Disable com.apple.wifiFirmwareLoader in the disabled dict (300 s runloop timeout, no Wi-Fi in macOS).

### CPU (8 cores helps these)
4. cpus=8 smp=1 (planned) — boot tasks, WS 47 s dyld gap, login phase (~100 s), Finder -> draw.
5. Drop vc-text=1 exec-log=1 debug=0x14e for timing boots (console text per exec on one CPU).
6. Turn off cs-lazy re-hash cost via 4 KiB-page re-sign (next-steps.md row 14) if page-in still dominates.
