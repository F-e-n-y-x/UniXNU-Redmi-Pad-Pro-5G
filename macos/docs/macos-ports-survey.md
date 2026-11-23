# macOS-on-non-Apple-ARM ports: survey for ruan (2026-11-24)

What this covers: public projects that boot macOS, or Darwin 26/27, through UniXNU or similar XNU efforts on Qualcomm and other ARM chips, and what each one can give ruan. Ruan is SM7435, A78/A55, ARMv8.2, **no PAC**, Gunyah with XNU at EL1. It runs a 4 KiB UniXNU XNU 12377 built as plain arm64 and a pacstripped, ad-hoc signed macOS 27.3 26C5035r userland. Root is on UFS and Data is in RAM. It reaches launchd and the dyld cache mapping. The 47-bit user VA / xzone topic is out of scope.

Clones (`git clone --depth 1`) are in `macOS/host-work/refs/macos-ports/<owner>__<repo>`, and paths below are relative to that clone. Projects already present locally were not cloned again: UniXNU core is `/root/unixnu-work/UniXNU-macos`, UniXNU-sheng is `/root/unixnu-work/UniXNU-sheng`, and darwin-lisa, sm7435-common and others are in `/root/unixnu-work/refs`.

How the scan was done: `gh search repos/code` (pacstrip, 26C5035r, macos-27, hakios, snapintosh, UniXNU …), the UniXNU `devices.toml` macOS records (sheng-macos, sheng-mataht, piano-lenzhang, piano-code002-2, tb710fu-limingerf, sheng-doubleloves), and unixnu.com. I did not build or run anything.

**Licence key.** "BSD-2-Clause-Patent" = UniXNU-style licence, which fits our use. "none" = no licence file: read it for ideas only and re-implement anything we take (or ask the author). GPL-3.0 code cannot be copied into BSD trees. **PAC note:** every port except HakiOS, macOS-Pad-6, Bergamota/willowios and darwin-lisa runs on CPUs **with** PAC and builds an arm64e kernel. Their PAC/JOP patches and their byte offsets into the stock arm64e cache do not apply to us. Their logic, scripts, plists and IOKit services do apply.

## Skipped / not cloned
| repo | why |
|---|---|
| doubleloves/UniXNU-sheng | Contains a vendor touch firmware blob (`firmware/novatek/*.bin`). The clone was removed. Its macOS 27.3 items (`make-nosetup-seed27.py`, `legacy-display-26C5035r.json`, `build-gop-legacy-display-27.py`, `cycle27.sh`, `vmwait-dyld-pager-source-release.patch`, the board.toml `text_fixups` WindowServer BGRA fix) are all in the local `/root/unixnu-work/UniXNU-sheng` (2026-11-17). |
| UniXNU org kits (XnuPrelink, AdrenoMetal, GopFramebuffer, RamBlock, AppleKeyStoreState, VerifiedRoot …) | Already submodules of the local UniXNU core. |
| mateusseiboth/darwin-lisa | Already in `/root/unixnu-work/refs/darwin-lisa` (upstream is newer: 2026-11-17 vs the local 11-13, so a `git pull` is worth doing). |
| hakios device repos (sm7325, sm7125, dada, umi …) and iOS-only UniXNU device repos | iOS 16/26 only. The core `hakios-xnu-kernel` was cloned instead. |
| SkyBlue997/enableMacosAI, aloedawn/MacIsland-Notch, other macOS-27 hits | These are apps for real Macs and are not relevant. |
| Committed non-Apple Mach-O: `ios-asteroids/tools/renegade-rs/tests/fixtures/*.dylib` (self-built test fixtures), `libNeoAppleArchive/ios-support/libplist-2.0.a`, `liberator/upstream/OpCore-Simplify/Scripts/macserial` | Not Apple binaries. Kept. |
| Owner root-policy `.bin` in `code002-2`, `limingerf` (`scripts/policies/**`) | These are owner keys/manifests, not Apple firmware. Kept. |

---

## 1. Same situation as ours (no PAC, A78, xnu-12377)

### oosexgang/hakios-xnu-kernel — https://github.com/oosexgang/hakios-xnu-kernel
- **Licence:** MIT (own code) + APSL-2.0 (Apple's XNU), in `LICENSES/`. **Last commit:** 2026-11-22.
- **Reached:** Darwin 26.6.2 (iOS) on **renoir, Snapdragon 780G, Cortex-A78/A55, no PAC**. launchd runs all boot tasks, about 110 processes, and SpringBoard reaches the home screen (`Documentation/darwin26-boot.md`). The patch set also covers Darwin 27 (`build.config.darwin27`). It does **not** run macOS, but it has the same CPU, the same XNU and the same PAC problem we have.

| path | what it solves | our stage | licence fit |
|---|---|---|---|
| `arch/arm64/tools/pacstrip.c` | C pacstrip with the modes `cache`, `macho`, `census` and `word`. Walks every image plus the cache's **TEXT_STUBS mappings**, rewrites LDAPUR/STLUR (RCPC2) to LDAPR/STLR, flag-less `__TEXT,text_env` (Metal), and re-signs page hashes. Faster and more complete than the Python tools | no-PAC | MIT |
| `arch/arm64/kernel/a64emu.c` | EL0 undefined-instruction emulator: RETAA/BRAA/BLRAA, PAC*/AUT*, PACGA, LDRAA, LDAPUR/STLUR, **FJCVTZS**, SB | no-PAC | MIT |
| `darwin/tools/fips26.py` | **corecrypto FIPS self-test fix after pacstrip**: replaces the stored HMAC with the computed one and re-signs. Without it, launchd's `fips` boot task (RequireSuccess) stops the boot with "FIPS_POST failed!" | launchd | MIT |
| `kernel/patches/0005-vm.patch` (`vm_shared_region.c`, `vm_dyld_pager.c`, `shared_region.h`, `vm_unix.c`) | **7 GiB shared region** (a Darwin 27 cache with max slide does not fit in 6), and the dyld pager / slide applying arm64e fixups **without signing** on a no-PAC CPU | kernel VA / dyld pager | APSL diff |
| `kernel/patches/0006-bsd-process.patch` | arm64e exec on arm64 CPUs; the **longer Darwin 27 `posix_spawnattr`**; `security.mac.lockdown_mode_state_public`; `rd=md0` hidden from launchd; `exec_swap=NAME:PATH`; `kern.exec_inject`; `systrace=`, `trace_sysctl=1`, kill-reason logging | launchd, debug | APSL diff |
| `kernel/patches/0004-mach.patch` | Answers the sandbox message-filter query (libxpc 26 drops clients otherwise); work-interval calls answered; panics and fatal user exceptions sent to the log | launchd/xpc, debug | APSL diff |
| `kernel/patches/0002-platform-arm64.patch` | `HAS_STD_PAC` guards (no PAC register access on v8.2); no `DC ZVA` on memory Gunyah treats as device | kernel | APSL diff |
| `kernel/patches/0001-build.patch` | Builds XNU **on Linux** with upstream clang + cctools; `XNU_PTRAUTH_DATA_PTRS_PLAIN` | build | APSL diff |
| `drivers/iokit/MiniAppleKeyStore.cpp`, `kernel/overlay/bsd/kern/kern_minimac.c` | AppleKeyStore stand-in; AMFI/Sandbox stand-in | keybag, launchd | MIT |
| `darwin/tools/aea.py` | **AEA1 decrypt on Linux** (HPKE P-256 key fetch, LZFSE). Reads straight from an Apple CDN URL with range requests | cryptex/AEA | MIT |
| `darwin/tools/mkimage26.py`, `select-system.py`, `ram-budget` | Pruned system image builder, RAM budget | storage/boot tooling | MIT |
| `darwin/userland/console_log.c`, `uncaught.c`, `rc.jobs.*` | Console logger, uncaught-exception reporter, probe job sets | debug | MIT |

### BxiiDN/macOS-Pad-6 — https://github.com/BxiiDN/macOS-Pad-6
- **Licence:** BSD-style ("macOS-Pad-6 contributors"). **Last commit:** 2026-11-11.
- **Reached:** host-side only. macOS 26.5 on SM8250 (no PAC) has not booted; the patch chain applies cleanly (118/118) but has not been built (`docs/compatibility.md`). Its value is the analysis and the emulator.

| path | what it solves | stage | licence fit |
|---|---|---|---|
| `kernel/fge/fge.c`, `fge.h`, `kernel/xnu/fge_xnu.c`, `kernel/patches/xnu-12377/0001-fge-undefined-instruction-emulation.patch` | libc-free emulator for v8.3–8.5 instructions: null-PAC, **FJCVTZS, FlagM/FlagM2, SB, LDAPUR/STLUR, DIT/SSBS, SHA3, SHA512**. Tested in `tests/fge/` (fuzzed against LLVM and QEMU) | no-PAC | BSD |
| `profiles/macos-26.5/patches/xnu-12377/null-pac-armv82.patch` | `HAS_NULL_PAC`: sign_blob_generic returns 0, monitor_call hash 0, no key registers | no-PAC kernel | BSD |
| `tools/isa/audit.py`, `scan.c`, `inspect_macho.py` | ISA census of the KC and dyld cache (works as a gate) | no-PAC | BSD |
| `docs/tahoe-26-report.md` §table | **Open gaps for macOS on v8.2:** FCMA (FCADD/FCMLA) 25,274 sites in libBLAS/libSparseBLAS/VectorKit/libsystem_m, FHM 128 in libBNNS. Nobody emulates these yet. Also: M2+ kernels use LRCPC2 | no-PAC (userland) | — |
| `tools/isa/ipsw.py` | IPSW member reader over HTTP range | tooling | BSD |

### Bergamota family (iOS, no PAC): nothing-sm7635-devs/ios-asteroids, FoxxoOwO/willowios, tomcat080/ios-xnu
- **Licence:** none on any of the three. **Last commits:** 2026-10-29, 2026-11-19 and 2026-11-24.
- **Reached:** iOS 18.7.8 SpringBoard on SD845 (ios-asteroids README); iOS 16 on SM6125 (willowios). tomcat080 is an XNU-8796 tree with 135 patches and an AKS WrapKey, and has no README.
- Reusable, ideas only because there is no licence:
  - `ios-asteroids/tools/renegade-rs/src/{pacstrip,dsc,aea,codesign,apfs/*,hfs*}.rs`: Rust pacstrip, dsc, AEA and APFS tools.
  - `ios-asteroids/tools/renegade/pacstrip_cache.py`.
  - `willowios/tools/renegade/dsctools/{crash,symb,dcache,objc,stubs}.py`: dsc symbolizer and crash tools.
  - `willowios/tools/renegade/fips_rehmac.py`: the same FIPS fix as HakiOS.
- These are the lineage of darwin-lisa, which we already have.

### aspauldingcode/FuckArm64eBootFlags — MIT, 2026-03-14
Rewrites the Mach-O cpusubtype from arm64e to arm64 and runs on macOS. It is minor: its only idea is the subtype flip, which our kernel's grading patches already cover.

---

## 2. macOS 27 under Gunyah at EL1 (closest platform match)

### lenzhang/UniXNU-xiaomipad8pro — https://github.com/lenzhang/UniXNU-xiaomipad8pro
- **Licence:** none at the repo root; some files are SPDX BSD-2-Clause-Patent. **Last commit:** 2026-11-18.
- **Reached:** **macOS 27.3 26C5035r natively at EL1 under Gunyah** on SM8750, with root and Data on UFS, an Adreno 830 GPU desktop (Dock, Finder), touch, Wi-Fi and audio. Restart still panics (README "State 2026-11-17"). Same userland as ours; the CPU has PAC.

| path | what it solves | stage | fit |
|---|---|---|---|
| `patches/k27/0067-…cs-native4k-skip-1…patch`, `0070-…` | `cs-native4k-skip=1` / `-threads=N`: skips the native-4K code-signing pre-hash of the 27.3 dyld cache (**about 37 s of launchd stall**) | kernel 4K / dyld | check per file |
| `patches/k27/0055`, `0058` (pstore-console), `0059`, `0063`, `0065`, `0066`, `0069` | RAM-mailbox console; busy-IOService dump on `!BSD`; Gunyah watchdog boot guard; kmem contig fix; boot-args >1023 B via the ADT; PSCI halt/restart | debug, Gunyah, boot | 〃 |
| `patches/k273/0001-darwin26-syscalls.patch` | pipe2, dup3, fsctlat … needed by 27.3 | launchd | 〃 |
| `docs/el1-hyp-notes.md` | **Gunyah at EL1:** watchdog SMCs 0x86000005-8, memory window, PSCI, SMMU ownership, logging without a UART | kernel/Gunyah | doc |
| `docs/k27-userland-args.md`, `docs/bringup-debugging.md` §5.2 | Full boot-arg set, each with the code that reads it (`offline-apfs-load`, `union-hide-lower`, `rootauth=0`, `exec-env`, `vm_compression_limit`, `k27-deny-exec`) | boot-args | doc |
| `package/NativeGopDisplay`, `docs/drivers/native-gop-display.md` | **WindowServer on the GOP framebuffer** (own AppleParavirtGPU nub, cursor layer, damage copies) | display/WindowServer | ? |
| `package/NativeSoftGpu`, `NativeSoftMetal`, `tooling/k27/stage-soft-metal.py` | Software Metal, a path to the desktop before Adreno works | GPU fallback | ? |
| `docs/data-store.md`, `package/NativeRamBlock`, `package/NativeUfs` | RAM Data seed + persistent page store in a UFS window (`data-persist=2`) | Data/storage | ? |
| `docs/attended-desktop.md`, `tooling/userland/piano-login-front/` | Login delay, Keyboard Setup Assistant, respawn loops; LaunchAgent that brings Finder to the front | loginwindow | ? |
| `tooling/k273/{make-systemos.sh,make-trust.py,decrypt-aea.py,decompress-kernelcache.py}` | SystemOS / trust cache / AEA for 27.3 | boot tooling | ? |
| `tooling/native/{xnu-log.py,usb-console.py,boot-timeline.py,pc-sample-symbolize.py}` | Log readers, boot timeline, PC-sample profiling | debug | ? |

### Droid-VM/macOS-DroidVM — https://github.com/Droid-VM/macOS-DroidVM
- **Licence:** none. **Last commit:** 2026-11-16.
- **Reached:** macOS 27.0.1 guest on crosvm + Gunyah reaches the login window, logs in as droid/droid, and shows Finder and Dock with the GPU (Turnip + metal2vulkan), about 118 fps after tuning (`TODO.md` handoffs 10-26…10-28, `docs/k27-userspace-manifest.md`). Kernel artifacts are published only in encrypted form.
- Note on our blocker: `TODO.md` 10-27 says xzone stayed on in their guest even with an `arm64_maxoffset` override, but without its VM range ("security feature, not speed"). They use `malloc-ranges-off=1`.

| path | what it solves | stage | fit |
|---|---|---|---|
| `patches/xnu-12377/0001…0048` + `README.md` | Opens with 0008 `rootauth=0`/IOMedia UUID; 0011 no kqworkloop limits for launchd; **0013 shared-region pager deadlock**; **0017 native-4K code signing: a 4K page past codeLimit is validated NX** (otherwise loginwindow dies with KERN_CODESIGN_ERROR); 0022 `exec-env=`; 0033 `k27-deny-exec`; 0035 `k27_amfi_page_shift`; 0036 FIRMLINKS; 0038/0041 Sandbox for 27; **0044 mmap on 16K boundaries**; 0046 missing macOS syscalls; 0026–0028 panic-spin/stall-dump. **0009 and 0042 are PAC-only: drop them** | kernel/dyld, launchd, Data | none (re-derive) |
| `overlay/droidvm/patches/xnu/virt-*.patch` + `overlay/droidvm/README.md` | Boot-arg-gated patches: `exec-log`, `crash-log`, `stackshot-dump`, `syscall-err-log`, `mount-log`, `vc-text`, `union-hide-lower`, `union-single-dev`, `mac-sandbox-permissive`, `malloc-ranges-off`, `shared-region-8g` | logging, root union | 〃 |
| `docs/k27-userspace-manifest.md` | **Best single document** on userland: rule "no Apple binary modified"; required args `mac-amfi-launch-permissive=1 union-hide-lower=1 malloc-ranges-off=1 ml-timeout-lock-scale=40`; SIP 0x2 so launchd can map the cache | launchd | doc |
| `scripts/stage-gpu-plugin.py` (`--setup-done`, `--user`, `--disable-job`) | `.AppleSetupDone`, managed SkipSetupItems, a dslocal uid 501 user with a PBKDF2 ShadowHash, `.skipbuddy`, launchd `disabled.plist` | setup skip / user | none |
| `scripts/make-data-seed.sh` | APFS Data seed on **Linux** (apfsprogs + linux-apfs-rw) with a fixed volume UUID | Data | none |
| `TODO.md` §2026-10-26/28 | loginwindow does not start Dock/Finder/SystemUIServer, so they use `launchctl kickstart gui/501/com.apple.Dock.agent`. kernelmanagerd `mkdir /dev` EROFS on the union root (patch 0014). fseventsd fails on the union root (0013). applekeystored abort comes from `sandbox_init`; usermanagerd asserts on `/chosen/ephemeral-storage` | loginwindow, keybag | doc |
| `patches/drivers/ArmCpu-idle-wfi.patch` | Under Gunyah, SIGPdisabled is never cleared, so every vCPU spins idle. The fix is self-IPI + WFI | kernel/Gunyah | none |
| `scripts/{crash-symbolize.py,stackshot-from-serial.py,stackshot-summary.py,kc-panic.sh,dsc-disasm.py}` | Symbolization, stackshots | debug | none |

### Droid-VM/UniXNU-droidvm (branch wip/hackintosh) — BSD-2-Clause-Patent, 2026-11-21
The device repository behind the DroidVM port. It reaches the same desktop.

| path | what it solves | stage |
|---|---|---|
| `package/DroidVMLaunchPolicy.c` (MIT) | The kernel writes launchd's `disabled.plist` into Data **before launchd reads it**, because a plist written by the seed is not honoured | launchd |
| `scripts/seed-data-disable-jobs.py` | Same, plus the `disabled.migrated` marker in the seed | launchd |
| `package/KeyStoreProbe.cpp`, `KeyStoreWrap.inc` | AKS stub: no passcode, unlocked; `aks_wrap_key`/`unwrap` stubs so securityd and the keychain work | keybag |
| `package/DroidVMBootArgsMacOS27.c` | macOS 27 boot args, each with its reason (timebase, `ml-timeout-*-scale`) | boot-args |
| `tools/rig/logarchive.py` | Builds a `.logarchive` from `/var/db/diagnostics` + uuidtext (with the timesync Info.plist 27 needs) | logging |
| `tools/rig/{panic,stackshot,dmesg,sym,crashes}.py` | Panic region and stackshot (kcdata) readers | debug |
| `tools/rig/pacprobe/` | Bare-metal PAC probe under Gunyah | no-PAC check |
| `scripts/make-droidvm-adt.py` | FDT → ADT | boot tooling |
| Apple-derived: `package/SoftwareAES-24A446.S` (iPadOS fileset addresses) | — | note |

### code002-2/piano-macos-unixnu — none, 2026-11-17
iPadOS 17 reaches the home screen. Its macOS 27.3 kernel does not link (`macos/README.md`). Useful patches:
- `patches/xnu/zzz-root-unverified.patch`: unsealed root.
- `zzz-initproc-no-panic.patch`: a watchdogd exit does not panic initproc.
- `zzz-systemos-volume-retry.patch`.
- `piano-user-trap-report.patch`: user BRK traps reported with the cache slide removed (**good for our launchd crashes**).

### limingerf/UniXNU-tb710fu — none, 2026-11-17
SM8650, kernel built on CI, early MMU probes, no userland. Useful patches:
- `patches/xnu/0005-native-4k-bootstrap-pool-9g.patch`.
- `0004/0008-ramoops*`, `0010-cdc-kernel-log-tap.patch`.
- `diagnostics/early-report/`: QR early log.

---

## 3. macOS desktops on PAC tablets (logic transferable)

### iOSNJBrepo/macOS-uke — https://github.com/iOSNJBrepo/macOS-uke
- **Licence:** none. **Last commit:** 2026-11-08.
- **Reached:** **macOS 26.5 Finder desktop** on SM7675: WindowServer and loginwindow are stable, Adreno 732 Metal, autologin with no Setup Assistant, touch works as a pointer (README).

| path | what it solves | stage |
|---|---|---|
| `scripts/macos-skip-setup.py` | **Complete setup skip + autologin**: `.AppleSetupDone`, managed `SkipSetupItems`, a dslocal user with ShadowHash, `com.apple.loginwindow.plist` `autoLoginUser` + **`OptimizerPreviousBuild`**, `/etc/kcpassword`, per-user SetupAssistant `DidSee*`/`LastSeenBuddyBuildVersion`/`MiniBuddy*`, and a launchd `disabled.plist` covering mobileactivationd and watchdogd. Set `OS_BUILD` to 26C5035r | loginwindow / setup skip |
| `scripts/apfs-set-volume-group.py` | **Pure-Python, runs on Linux.** Puts System and Data in one volume group: patches the APSB `apfs_volume_group_id` (0x3f0) and recomputes Fletcher-64 | Data |
| `scripts/fullos-populate.sh`, `fullos-volumes.sh` | Data from `System/Library/Templates/Data`; dyld cache in `/System/Library/dyld`; cryptex into Preboot `/Cryptexes/OS`; Data/Preboot/VM volumes (needs a macOS host) | Data, dyld |
| `touch/src/package/UkeTouchPointer.cpp` | NT36532 touch as an absolute pointer | input |
| `gpu/src/macos/UkeGopHandoff.cpp` | GOP swap with a CPU-copy fallback | display |
| `scripts/macos-cycle.sh` | build/flash/pull cycle, 512 KiB retained log in the `macoslog` FAT | boot tooling, logging |
| README lessons | No Dock or menu bar unless the SetupAssistant version keys and `OptimizerPreviousBuild` are set; the cache must be in `/System/Library/dyld`; SkyLight uses the GPU only if `-[MTLDevice acceleratorPort]` exists; the Metal plugin needs SIP 0x7f | — |

### mataht/sheng-macos-link-lab — https://github.com/mataht/sheng-macos-link-lab
- **Licence:** GPL-3.0. **Last commit:** 2026-11-12 (archived).
- **Reached:** on sheng, macOS 26.5 went Recovery GUI → **CPU-rendered desktop** → GPU desktop, with UFS persistence and restart and shutdown both working.

| path | what it solves | stage |
|---|---|---|
| `platform/sheng/macos26/login/native_login_services.cpp`, `aks_protocol.h`, `scripts/native_login.py` | AppleKeyStore stub at `IOResources/AppleKeyStore` (`sheng-aks=1` unlocked; `=2` software wrap) + IOWatchdog stub | keybag, launchd |
| `docs/technical-route.md` (lines 66–82) | Autologin `autoLoginUserScreenLocked=false`, `MiniBuddyLaunch=false`, `panic_on_crash_disabled=1`, platform UUID needed for the Data mount | autologin |
| `config/macos26-full-ui-pages.json`, `recovery/ui_policy_kernel.h` | **CPU rendering fallback** policies (software-pool-bgra, native-bgra-framebuffer, sw-cursor-forced …), applied as in-memory dyld-cache page edits checked against the page hashes | WindowServer |
| `display/0001-native-gop-display.patch` | GOP framebuffer as IOMobileFramebuffer through the swap queue | display |
| `input/touch_pointer.h`, `input/native_touch.cpp` | Touch as pointer (oldest finger drives the pointer; WindowServer ignores multitouch) | input |
| `storage/macoslog_*.h`, `scripts/{create_log_partition,decode_native_log,capture_persistent_log,decode_xnu_panic}.py`, `log/panic_qr.h` | Retained log FAT + decoders, panic QR | logging |
| `macos26/patches/0053-development-dyld-commpage.patch` | Publishes the commpage bits dyld reads | dyld |

GPL-3.0: take ideas from it, do not copy its code into BSD trees.

### Allenwdk/UniXNU-uke — BSD-2-Clause-Patent, 2026-11-19
iPadOS 27 GPU desktop on SM7675. `docs/MACOS-BUILD.md` describes a macOS cpu-desktop candidate built with the **4 KiB** defconfig. Its `patches/xnu-12377/` is the same XNU version as ours:

| patch | purpose |
|---|---|
| `upstream/cshash-native4k-{lazy,parallel,stats}.patch` | **16K code-signature hash pages on a 4K kernel, lazy (`cs-lazy=1`)**: about 30 s less cache validation |
| `darwin26-posix-spawn-attrs.patch` | 0x100-byte `_posix_spawnattr` (**launchd-critical**) |
| `native-4k-stack-shift.patch` | 4 KiB kernel stacks |
| `upstream/xnuconfig-work-interval-counters.patch` | Work intervals; AppKit aborts on a scroll-bar drag without them |
| `uke-files-union-view`, `uke-systemos-root-identity`, `upstream/vfs-union-below-walk` | SystemOS cryptex union over root |
| `uke-video-console-text`, `uke-video-console-physical-fb` | Text console on GOP |
| `upstream/window-memory-excluded-ranges.patch` | Memory-map holes |

Other paths: `drivers/NativeNovatekTouch/`, `drivers/NativeUfs/`, `tools/{decode-ukelog,read-flight,collect-freeze}.py`. Data seed notes are in `docs/PROGRESS.md` (OWNER_DATA_VOLUME_UUID must equal the seed's volume UUID).

### liimagination/UniXNU-tb375fc — none, 2026-11-23
- **Reached:** **macOS 27.3 26C5035r desktop** on MT6897 (arm64e).
- Paths:
  - `drivers/display/NativeLegacyDisplay.cpp`: display for 27.3.
  - `drivers/net/Tb375fcDummyEth.cpp`: dummy en0.
  - `scripts/build_desktop_tier3.py`.
  - `porting.toml`: `validation_disables=0x10` avoids a stackshot panic.
  - `patches/xnu-vm-kern-kmem-io-128m.patch`.
- Remote branches (not cloned): `feat/ufs-persist-jit` has a **SwiftShader** disk JIT cache (CPU Metal path); `feat/touch-nt36532`.

### UniXNU-sheng (local) — `/root/unixnu-work/UniXNU-sheng`
Upstream reached a macOS 27.3 desktop (GPU copy and direct scanout, Wi-Fi, audio, DVFS), according to the mataht archive note.
- `board.toml` `[os_overrides."macos-27.3-26C5035r"]` (around line 163–300): full arg set (`aks=1 aks-sls=3 data-persist=2 disable-watchdogd=1 cs-lazy timebase-user apfs-boot-shim=1 vc-text=1 exec-log=1`).
- `text_fixups = dyld_shared_cache_arm64e.03,5de1134,…` NOPs a `tbnz` in QuartzCore `IOMFBDisplay::framebuffer_pixel_format` so that WindowServer outputs **BGRA8** under vmm-present. The offset must be recomputed for our pacstripped cache.
- `scripts/legacy-display-26C5035r.json` and `build-gop-legacy-display-27.py`: IOMobileFramebufferLegacy slots for 26C5035r.
- `scripts/make-nosetup-seed27.py`.
- `scripts/{hid-device,usb-*,software-aes}-26C5035r.json`: ABI fixtures for exactly our build.
- `patches/xnu-12377/vmwait-dyld-pager-source-release.patch`.

### Others in this group
- **CrossDarwin/macOS-ForSheng** (GPL-3.0, 2026-10-30): macOS 26.5 Recovery chooser by CPU copy. Earlier versions of mataht's files: `platform/sheng/macos26/display/0001-native-gop-display.patch`, `scripts/package_macos26_ufs.py`, `log/panic_qr.h`.
- **zy-xyz/macos-ForSheng-ci** (GPL-3.0, 2026-11-09): CI mirror of the above.
- **alghiffaryfa19/macOS-ForSheng** (MIT, 2026-10-24): EL1 probes only.
- **Skylarrkuo/sheng-macos-dualboot-public** (none, 2026-11-23): CI wrapper for macOS 27.3 on sheng (`unixnu build userdata --os macos-27`). It notes that the first install of the Data seed needs macOS tools (aea/hdiutil/diskutil/ditto); WSL cannot do it.
- **BxiiDN/UniXNU-pipa**: iPadOS only, not cloned.
- **Enceka/unixnu-e5-kernel-builder**, **QQ127/k27-kc-ci** (none): CI only. QQ127 points to `k27/kc27-layout.py` / `kc-build27.py` in a DroidVM fork for building the kernel-only KC.

---

## 4. Snapintosh, QEMU, PC, and Linux tooling

### Snapintosh/Snapintosh — https://github.com/Snapintosh/Snapintosh — MIT, 2026-10-25
- **Reached:** stock macOS 27 (26A428, J700 kernelcache) under its own **EL2** monitor on an X2 Elite laptop. It boots to a GPU-accelerated desktop with keyboard and touchpad (`docs/STATUS.md`). The EL2 parts (`shadow/`, `compat/sptm*`, `compat/txm*`, `compat/pauth*`) do not apply to us; the GPU/HID code is in submodules that were not checked out.

| path | what it solves | stage |
|---|---|---|
| `docs/GUI-BOOT-ROADMAP.md` §6 | **WindowServer waits 30 s for a Metal device and then aborts** ("No suitable Metal devices present after 30 seconds"). SkyLight has no software-compositor start path, so a plain framebuffer is not enough. This is the key planning fact | WindowServer/GPU |
| `docs/GUI-BOOT-ROADMAP.md` §9–12 | Post-WindowServer checklist: panel-mode argument order, a wait on the timesync clock, the 10-bit framebuffer format, **AMX must appear absent**, commitAndWaitUntilSubmitted | WindowServer/display |
| `docs/KERNEL-PATCHES.md` "Application AMX hide", `tools/kc-load.py` `hide_amx` | Clears commpage AMX caps bits 27–30 (check ours) | apps |
| `docs/FULL-INSTALL.md` (L40–70, 263–302) | Data deliberately unencrypted; Setup finished in a Mac VM; no SEP key services | Data/keybag |
| `compat/sep-sks.c` etc. | SEP keystore wire protocol (reference only) | keybag |
| `tools/apfs-checkpoints.py`, `tools/decode-mach-messages.py` | APFS checkpoint inspection, Mach message decoder | storage, debug |

- **adamikoo/Snapintosh** (MIT, 2026-11-09): X1 Elite QEMU-TCG fork that only reaches kernel-side matching. Its "Gunyah" appears only in the README, and `tools/build-slbounce.py` relies on Windows Secure Launch, so it does not apply to us. Low value.

### xVanTuring/UniXNU-qemu — BSD-2-Clause-Patent, 2026-10-21
- **Reached:** iPadOS 17 at 4 KiB with no SPTM: launchd, about 120 daemons, Data ramdisk, framebuffer, AKS stand-in, SpringBoard, setup skipped.
- `package/KeyStoreProbe.cpp`: AKS stand-in, "no passcode, unlocked", which fits macOS's AKS user client.
- `package/{HidInputProbe,TouchState}.cpp`: input.
- `package/QemuMetal/*`: a Metal device that forwards work to a host GPU over shared memory. Use it as an architecture reference for a soft or remote Metal device.
- README "State" rows: prefs in the Data seed stop the idle-sleep panic; `kern.maxvnodes` must be 66560 (1024 killed apps); Setup hangs on lockdownd → fairplayd, so skip it.
- `scripts/qemu-ps.py`: allproc/msgbuf reader.

### lateautumn233/gunyah-xnu-ci — no licence, 2026-10-22
CI for a single-vCPU Gunyah guest. It stops at source-kernel bring-up, but its 4K patches are worth diffing against ours:
- `UniXNU-gunyah/patches/upstream/native-4k-user-abi.patch` (mach_loader, arm_init, commpage, vm_shared_region).
- `native-4k-signature-hash-cache.patch`.
- `UniXNU-gunyah/patches/xnu/gunyah-{code-signing-enforcement,null-amfi-tolerance,null-amfi-iokit,user-fault-log}.patch`.
- `UniXNU-gunyah/security/GunyahTrustCache.c`.
- `UniXNU-gunyah/linux/` builds XNU from a Linux host.

### nullmoth/liberator — PolyForm **Noncommercial**, 2026-11-23
- **Reached:** macOS 27 desktop on x86 PCs. It commits OpenCore `.efi` files and `macserial`, which are not Apple binaries.
- `p1401/finish-setup.sh` (L61–126): uid-501 admin, `kcpassword` (11-byte XOR key), `autoLoginUser`, `.AppleSetupDone`.
- `kit/system/bringup/lw-gate.sh` (+ ws-gate): holds loginwindow until `com.apple.windowserver.active` answers.
- `realmac27/README.md` L77 + `realmac27/config/com.nullmoth.rbcpu.*.plist`: `RB_DISABLE_GPU=1` puts RenderBox on its CPU renderer.
- `mac/Resources/nullmoth-setup.sh` L2199–2202: **"mapping process is a platform binary, but mapped file is not"** made WindowServer find no Metal device. That is what our ad-hoc-signed plugins would hit; it has to be fixed in the trust cache or AMFI.

### Linux tooling: AEA, dyld cache, kernelcache
- **blacktop/ipsw** (MIT, 2026-11-23):
  - AEA: `pkg/aea/decrypt.go` + `cmd/ipsw/cmd/fw/aea.go`. Pure Go on Linux; takes `--key-val`/`--pem-db`, otherwise fetches the key from Apple's WKMS.
  - dyld: `pkg/dyld/`, `cmd/ipsw/cmd/dyld/{dyld_extract,dyld_macho,dyld_info,dyld_slide}.go`. `dyld_split.go` is darwin-only.
  - kernel: `cmd/ipsw/cmd/kernel/{kernel_dec,kernel_extract,kernel_kexts}.go`.
  - other: `pkg/img4/`, `pkg/devicetree/`, `pkg/launchd/`, `internal/codesign/`.
- **kinnay/AEA** (MIT): `aea/aea.py` + `FORMAT.md`, the simplest Python decoder once you have the key.
- **GavBog/aea-tools** (MIT/Apache): Rust streaming decrypt (`src/reader.rs`) and IPSW over HTTP range (`examples/asahi_firmware`).
- **0xilis/libNeoAppleArchive** (MIT): C library for Apple Archive (`.aar`/YAA) + AEA (`libNeoAppleArchive/neo_aea_archive.c`), with partial profile and compression support.
- **HakiOS `darwin/tools/aea.py`** (MIT, section 1) does the HPKE key fetch and range reads in one Python file.
- **Gap:** no repo has a Linux **tracev3 / unified-log** parser. DroidVM uses the external mandiant `macos-UnifiedLogs` (`unifiedlog_iterator`), and the others use their own kernel log rings (macoslog FAT, UFS window, pstore mailbox).

---

## Top 10 to use next (ordered for our current stage: launchd → desktop)

1. **HakiOS `darwin/tools/fips26.py`** (MIT). Once launchd gets further, the `fips` boot task (cc_fips_test, RequireSuccess) will fail on our pacstripped corecrypto. Patch in the computed HMAC and re-sign. willowios has the same fix in `fips_rehmac.py`.
2. **HakiOS `kernel/patches/0005-vm.patch` + `0006-bsd-process.patch`**. Gives a 7 GiB shared region for the Darwin 27 cache's max slide, dyld pager fixups without signing, the Darwin 27 `posix_spawnattr`, `lockdown_mode_state_public`, and `rd=md0` hidden from launchd. Diff these against our tree, together with Allenwdk `darwin26-posix-spawn-attrs.patch`. Same CPU and same XNU as ours.
3. **HakiOS `arch/arm64/tools/pacstrip.c` (`census` mode) + `arch/arm64/kernel/a64emu.c`**, plus **BxiiDN `kernel/fge/`**. Re-check our cache for TEXT_STUBS mappings, RCPC2 and `text_env` coverage, and add FJCVTZS, SB and FlagM emulation. FCMA/FHM (libBLAS, libBNNS) is still open: nobody emulates it yet.
4. **Allenwdk `patches/xnu-12377/upstream/cshash-native4k-{lazy,parallel}.patch`** or **lenzhang `patches/k27/0067`/`0070` (`cs-native4k-skip`)**. Removes the roughly 30–37 s stall while launchd validates the 27.3 cache on a 4K kernel. Also DroidVM **0017** (a 4K page past codeLimit validated NX, otherwise loginwindow hits KERN_CODESIGN_ERROR) and **0044** (mmap on 16K boundaries).
5. **lenzhang `docs/k27-userland-args.md` + `docs/el1-hyp-notes.md`**, and **DroidVM `docs/k27-userspace-manifest.md`**. The tested macOS 27 boot-arg set (`union-hide-lower`, `rootauth=0`, `mac-amfi-launch-permissive`, `malloc-ranges-off`, `ml-timeout-*-scale`, SIP 0x2), plus Gunyah watchdog and PSCI facts for EL1.
6. **AppleKeyStore stub**: `Droid-VM__UniXNU-droidvm/package/KeyStoreProbe.cpp` + `KeyStoreWrap.inc` (BSD-2-Patent; unlocked, no passcode, wrap/unwrap stubs), with the same idea in xVanTuring-qemu. For the Data/keybag stage and securityd.
7. **iOSNJBrepo `scripts/macos-skip-setup.py`** (setup skip + autologin, including `OptimizerPreviousBuild` and the SetupAssistant version keys) and **`scripts/apfs-set-volume-group.py`** (System+Data volume group on Linux). No licence, so re-implement them; liberator `p1401/finish-setup.sh` covers the same ground.
8. **launchd job control under the kernel**: `Droid-VM__UniXNU-droidvm/package/DroidVMLaunchPolicy.c` (MIT; writes `disabled.plist` before launchd reads it) plus the `kickstart` workaround for Dock/Finder/SystemUIServer (DroidVM `TODO.md` 10-26). Also `ArmCpu-idle-wfi.patch` for spinning vCPUs under Gunyah.
9. **Display path to the first frame**: lenzhang `package/NativeGopDisplay` (+ `NativeSoftMetal`), mataht `display/0001-native-gop-display.patch`, and UniXNU-sheng `text_fixups` (BGRA8 under vmm-present) with `legacy-display-26C5035r.json`. Plan around Snapintosh's finding that **WindowServer needs a Metal device within 30 s**, so a soft Metal device has to exist (NativeSoftMetal / QemuMetal / SwiftShader in tb375fc `feat/ufs-persist-jit`). Fix the "platform binary maps non-platform file" AMFI case (liberator) for our ad-hoc plugin.
10. **Debug and logging**:
    - code002-2 `patches/xnu/piano-user-trap-report.patch` (user BRK reported without the cache slide).
    - DroidVM `virt-*` `exec-log`/`crash-log`/`syscall-err-log` patches.
    - lenzhang pstore-console `0055`/`0058`, mataht `macoslog` FAT decoders, DroidVM-rig `tools/rig/logarchive.py`.
    - For touch, take mataht `input/touch_pointer.h` / uke `UkeTouchPointer.cpp` (touch as an absolute pointer, because WindowServer ignores multitouch). Their controller is NT36532; the pointer logic does not depend on the controller.

Read-only survey. Nothing was built or run.
