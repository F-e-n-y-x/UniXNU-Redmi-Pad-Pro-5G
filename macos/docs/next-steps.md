# macOS 27.3 (26C5035r) on ruan: from "launchd runs" to "desktop on screen"

2026-11-24. This is desk research only. Nothing was built and the tablet was not touched. The aim is to batch the fixes that
come after libmalloc's xzone reservation, rather than finding them one crash per boot. Two things are left out on purpose because
another agent owns them: the 47-bit user TTBR0 / T0SZ_USER port and `vmwait-dyld-pager-source-release.patch`.

Sources compared:
- sheng: `/root/unixnu-work/UniXNU-sheng`. The relevant parts are `board.toml [os_overrides."macos-27.3-26C5035r"]`,
  `configs/sheng_macos_defconfig`, `patches/xnu-12377/*` and `package/BootWatchdogPolicy.c`.
- ruan: `/root/unixnu-work/UniXNU-ruan-macos`.
- The UniXNU macOS profile and the device records under `UniXNU-macos/docs/.vitepress/device-records/`. The useful records are
  sheng.toml, houji-hannana-26.toml and elden-tay0123.toml.
- Our boot log: `first-boot.md`, plus `host-work/logs/macos-ufs1/ufs11-dmesg-6.txt`.

The OS patch series is the same for both boards (`os:patches/xnu/series` = the profile's). The differences therefore sit in four
places: the device patch series, the defconfig, the kernel args and the root/Data layout.

## Prioritized list, in boot order

P1 means it blocks the desktop. P2 means it will probably crash or stall a boot. P3 is performance or quality.
Effort: S < ½ day, M 1–2 days, L multi-day.

| # | Pri | Stage | Symptom we'd see | Fix (patch / arg / root) | Source | Effort | Conf. |
|---|---|---|---|---|---|---|---|
| 1 | P1 | libSystem init in launchd, right after the malloc fix | abort/brk in `os_lockdown_mode_enabled` (the sysctl lookup fails). dby-w09's console also stopped at `lockdown_mode` | Add the `security.mac.lockdown_mode_state_public` sysctl. This is the `bsd/kern/lockdown_mode.c` hunk of sheng's bring-up patch, 16 lines | `UniXNU-sheng/patches/xnu-12377/zzzz-sheng-ipados27-bringup.patch` (lockdown_mode.c) | S | high (sheng and houji both list it as a post-launchd blocker) |
| 2 | P1 | first `posix_spawn` by launchd | Daemons start with misread `psa_sec_flags` (16 vs 32 bit), crash count, throttle timeout and kqworkloop limits. Sheng saw every daemon reported for "too many kqworkloops" | `psa_from_darwin26()`: when `attr_size == 0x100`, convert the Darwin 26 `_posix_spawnattr` layout (the kern_exec.c hunk) | same patch, kern_exec.c `psa_from_darwin26` | S | high |
| 3 | P1 | before launchd's first jobs | Nothing usable starts: the root is the **restore ramdisk** tree. It has 9 LaunchDaemons (`restored_external`, `PurpleReverseProxy.ramdisk` …) and no WindowServer, loginwindow, Finder, Dock or launchd service cache | Build sda22 from the full 26C5035r **OS** System volume (the IPSW's OS window, not `141-22079-021.dmg`). Merge in the **SystemOS cryptex** (`141-22145-021.dmg.aea`, which still needs decrypting) and optionally App/Safari. Then pacstrip, `adhoc-resign.py` and the trust cache as now. Sheng's OS+SystemOS+App is 16.4 GiB and sda22 is 32 GiB | `macos/tools/make-macos-ufs-root.py`; `UniXNU-macos/profiles/os/macos-27.3-26C5035r/profile.toml [userdata] windows`; sheng `payload.runtime.data_gib` note | L | high (verified: `macos-extract/recovery-tree/System/Library/LaunchDaemons`) |
| 4 | P1 | launchd's first writes to `/private/var` | Writes fail with EROFS, or land on the read-only root's `/private`. dslocal, logd, `/var/db` and launchd's own db are all missing. ufs11 shows `disk1s1` (the RAM Data, 709080 blocks) **attached but never mounted**: only `System` mounts | Enable sheng's Data path in `ruan_macos_arm64*_defconfig`: `CONFIG_XNU_NATIVE_RAM_BLOCK=y`, `CONFIG_XNU_OWNER_DATA_MOUNT=y`, `CONFIG_XNU_OWNER_DATA_VOLUME_UUID="<ram-data-uuid>"`. `macos-owner-data-firmlinks.patch` (already in the OS series, but compiled out without these) then mounts Data at `/System/Volumes/Data` and sets every `/usr/share/firmlinks` entry (`/private` is required). `ruan-ram-data-mount.patch` is only in the iPadOS series, not xnu-12377 | `UniXNU-sheng/configs/sheng_macos_defconfig`; `…/macos-owner-data-firmlinks.patch` | M | high (config gap verified) |
| 5 | P1 | loginwindow | Language Chooser or Setup Assistant (asks to activate) appears instead of a desktop. With a user but no home you get **black screen behind MiniBuddy** (sheng macOS run42), and Dock/Finder never start | The Data seed comes from `System/Library/Templates/Data` with owners kept. Add an admin uid 501 in dslocal, `.AppleSetupDone`, the managed `SkipSetupItems`, a user home containing `.skipbuddy`, `com.apple.keyboardtype.plist`, and auto-login (`autoLoginUser` + `/private/etc/kcpassword`). The script needs a macOS host (ditto/hdiutil), so port its steps into our Linux mkapfs pipeline | `UniXNU-macos/scripts/make-macos-data-seed.py` (`--skip-setup --user --auto-login --keyboard-type`), `make-macos-autologin-addon.py`; `UniXNU-sheng/scripts/prepare-macos-install.py` | M | high |
| 6 | P1 | boot tasks `mount-phase-1/2` (RequireSuccess) | `apfs_boot_util` tries to create the VM volume (and Preboot/Update/xART/Hardware) in the root container and fails, so launchd stops the boot | Sheng uses `apfs-boot-shim=1` with the payload addon. **ruan can skip the addon**: build `macos-boot-shims/apfs_boot_util.c` for arm64e and drop it into our root at `/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_boot_util`, ad-hoc signed with its CDHash in the trust cache. `macos-vm-volume.patch` / `macos-hardware-volume.patch` (in the series) cover the missing mount points | `UniXNU-macos/scripts/macos-boot-shims/apfs_boot_util.c`; `…/macos-apfs-boot-shim.patch` | S | high |
| 7 | P1 | after Data mount, before launchd reads overrides | watchdogd has no hardware backend, so it panics or restarts. cameracaptured loops | Pre-write `/private/var/db/com.apple.xpc.launchd/disabled.plist` (`com.apple.watchdogd`=true, `com.apple.cameracaptured`=true) plus an empty `disabled.migrated` into the Data seed. Sheng writes these from the kernel (`disable-watchdogd=1`, `disable-jobs=`). ruan has no `sheng_boot_watchdog_policy` and its `zzz-boot-progress` hook only runs on the scratch-Data path | `UniXNU-sheng/package/BootWatchdogPolicy.c` | S | medium-high |
| 8 | P1 | any process using private entitlements, XPC or the sandbox platform profile | Daemons killed for restricted entitlements, launchd refusing services, sandbox differences. **ruan only**: our ad-hoc root "is not platform" (`ruan-all-tasks-user-ranges.patch` works around one symptom of this) | Make trust-cache hits grant `CS_PLATFORM_BINARY`: load the CDHash module as a static/loadable type that AMFI treats as platform, or patch `kern_trustcache`/AMFI query use. Keep `amfi_get_out_of_my_way=1` until then | `UniXNU-ruan-macos/patches/xnu-12377/ruan-all-tasks-user-ranges.patch`; `UniXNU-macos/patches/xnu-12377/trustcache-runtime-layout.patch` | M | medium (cause not traced) |
| 9 | P2 | early daemons (logd, launchd, dyld, tzlinkd) | Lookups of `/System/Volumes/Preboot/<uuid>` use the profile's `boot-uuid`/`apfs-preboot-uuid` = `84581E99…`, but ruan's root is `4C15B884-6D41-FA68-15FA-10423D08C1A0` | Override `[adt.chosen.string] boot-uuid / apfs-preboot-uuid` in ruan's board.toml with the root volume UUID (and in `root_match_volume_uuid`) | `profile.toml [adt.chosen.string]` | S | medium |
| 10 | P2 | exec of boot tasks after fsck | `AMFI: failed to create OSDictionary copy: 14`. CoreEntitlements' stack check `sp & ((1<<shift)-1) < 0x80`. ruan sets `unixnu_kernel_stack_shift = 15` as a constant but does **not** align the stacks the way sheng's `stack.c` hunk does | Port the `osfmk/kern/stack.c` hunk (`stack_addr_mask = roundup_pow2(size + ptoa(3)) - 1`, real `unixnu_kernel_stack_shift`) and drop the constant in `build-gop-legacy-display-27.py:403` | zzzz-sheng-ipados27-bringup.patch (stack.c); `UniXNU-macos/kits/XnuPrelink/fixtures/fileset-26C5035r-symbols.json` | S | medium |
| 11 | P2 | launch of system daemons | Launch-constraint kills (an ad-hoc root has no constraint categories) | `no-lc=1` (`zzyc-owner-skip-launch-constraints.patch`, already in the arch series). The ruan iPadOS boots pass it | sheng kernel_args | S | medium |
| 12 | P2 | while the system is under load (1 slow A55, PAC traps) | DEVELOPMENT-kernel hygiene panics: spinlock timeout, "interrupts masked for too long" (sheng: 786 µs) | `-no_slto_panic sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0 validation_disables=0x11040` (sheng builds these into `PE_builtin_boot_args`) | sheng kernel_args; `BootWatchdogPolicy.c:65` | S | medium-high |
| 13 | P2 | kernelmanagerd / IOKit quiet | `openat_authenticated_np` gets EAUTH on the unsealed root, so no system dext starts. IOKit never goes quiet (sheng run29/31) and boot tasks waiting on it stall | `macos-owner-root-authenticated-open.patch` is compiled only with `XNU_OWNER_ROOT_TRUST`, which ruan lacks. Add a ruan variant: after EAUTH, reopen without the check when the vnode is on the UFS root mount (bring-up root) | `…/macos-owner-root-authenticated-open.patch` | S-M | medium |
| 14 | P2 | first ~4 min of launchd | `F_ADDFILESIGS` reads and hashes each of the 84 cache files whole ("1300 wakes/s", UFS boot 5) | `cs-lazy=1` (`cshash-native4k-lazy.patch`, in the series). **ruan only:** `adhoc-resign.py` could instead re-sign with 4 KiB hash pages (it keeps the 16 KiB `pshift`), so the native4k derivation goes away completely | sheng builtin args; `macos/tools/adhoc-resign.py` | S | high |
| 15 | P2 | WindowServer start | `WSSystemCanCompositeWithMetal` → SIGABRT before the first frame (sheng run64, elden). **No Metal device = no desktop** | (a) Ship a macos27 build of ruan's Adreno 710 Metal plugin (`build-ruan-adreno.py` is not in the macOS archives) in `/Library/GPUBundles` (`macos-gpu-bundles.patch`). It also needs the IOUserClient kobject map grant and `hw.pagesize` sysctl hunks from zzzz, plus `ruan-native-sandbox-grant-any`. (b) Elden's software path: QuartzCore/WindowServer text rules (Metal check, uncompressed BGRA instead of FourCC 0x26424741). Elden reached a first frame on macOS 26.5 Recovery that way | sheng `payload.runtime.gpu_addon`, `components.include AdrenoMetal`; elden-tay0123.toml | L | high that it's needed; medium on the route |
| 16 | P2 | WindowServer first swap | Frames in half float (`RGhA`) because the device reports virtualized (vmm-present): wrong colours or a refused format on the GOP path | **ruan only:** apply sheng's fixup `dyld_shared_cache_arm64e.03 @0x5de1134: 08060037 → 1f2003d5` directly to the cache before re-signing (no `text_fixups` needed). Or handle half float like sheng's `fb-half=2` | sheng `payload.text_fixups` | S | medium |
| 17 | P3 | the whole boot | ~10× slower with `cpus=1` on an A55 | `cpus=8 smp=1 smp-gic=1 armcpu-edge=1` as in the ruan iPadOS boots. Sheng's `zzzzzzzzzzz-sheng-edge-matrix.patch` (advisory matrix check) is needed for `armcpu-edge`. Check the macOS recount fix with both kinds present | `host-work/args-current.txt`; sheng patches | M | medium |
| 18 | P3 | every `mach_absolute_time` | With `timebase-hz=24000000` each read traps (`xnuconfig-timebase-trap-count`) | `timebase-user=1` (`timebase-user-scaled-read.patch` rewrites libsystem_kernel after CS validation). On ruan it can be baked into the re-signed cache instead | sheng builtin args | S | medium |
| 19 | P3 | memory after the desktop | RAM Data, no swap (no VM volume), Data template ~0.8 GiB | Later: put Data on UFS (a Data-role volume in the sda22 container or a second partition), check `hw.memsize` with outside-memory adoption, and consider `kt_fixed_zones=120` | sheng `data_gib`, `owned_memory_*` | M | medium |
| 20 | P3 | many processes (>255) | Hardware ASIDs are shared and the TLB is flushed on every switch | `HAS_16BIT_ASID` plus the `TCR_EL1.AS` hunks of zzzz. A78/A55 have 16-bit ASIDs | zzzz (BCM2837.h, pmap.c) | S | medium |
| 21 | P3 | after first frame | Kernel console text drawn over the desktop | Drop `vc-text=1` (and `exec-log=1`) for desktop boots | ruan kernel_args | S | low-medium |

Possible extras, if they show up: `aks-sls=3` (MobileKeyBag "no passcode", sheng) when keybagd or loginwindow complain about the lock
state, and `owner-dataprotect=1` (the data-protection class xattr from zzzz `kpi_vfs.c`) if a daemon rejects an unencrypted store.

## Task 1: sheng vs ruan

### Device kernel patches that sheng has and ruan lacks (`UniXNU-sheng/patches/xnu-12377/series`)

| Patch | Class | Notes |
|---|---|---|
| zzzz-sheng-ipados27-bringup.patch | **mixed** | macOS userspace: posix_spawnattr D26, lockdown_mode sysctl, stack.c alignment, map_with_linking 16 KiB + dyld pager 16 KiB assembly, hw.pagesize sysctl, kobject map grant, bindfs vget, owner-dataprotect, `PE_builtin_boot_args`, launchd job-policy call. 47-bit TTBR0 / T0SZ_USER / VM_USER_STRIP_MASK / sleh mask: **other agent**. Hardware or diagnostics only: sigp reinit, 16-bit ASID (perf), pmap WC flush, pmap user-mapping query (Adreno), sched_clutch advisory check, latency/profile rings, sheng-exit printf. iPadOS only: LaunchServices store drop, wallpaper/staged-apps unions |
| vmwait-dyld-pager-source-release.patch | macOS userspace (4K) | **other agent**. Only matters once `map_with_linking_np` accepts 16 KiB pages |
| zzzzzzzzzzz-sheng-edge-matrix.patch | performance (SMP) | needed with `armcpu-edge=1` |
| zzzzzz-sheng-bsd-checkpoints, sheng-bootstrap-checkpoints, sheng-headless-startup-checkpoints, sheng-startup-failure-log, sheng-process-exit-log, zzzzzz-sheng-latency-trace, zzzzzzzz-sheng-exit-latency, zzzzzzzzz-sheng-unnest-trace, sheng-main-thread-profile, sheng-cache-input-diagnostic | diagnostics only | ruan has its own: ruan-sctrace, srtrace, uabort, ubrk, exec-log |
| sheng-platform-sleep, zzzzzzzzzz-sheng-sigp-reinit-assert | sheng hardware / SMP | revisit when SMP comes on |
| zzyd-sheng-audio-addon-mount, source-storage-runtime, sheng-wifi-*, sheng-bluetooth-*, sheng-appsync-* | sheng hardware / iPadOS | not for the desktop |
| (PAC-specific) | none | ruan's PAC work (`ruan-pac-emulator`, `ruan-dyld-pager-arm64e`, `*-auth-store`) has no sheng counterpart |

ruan already has `zzye-sheng-cpu-idle`, `zzyf-sheng-amp` and `sheng-platform-power` (soc.series).

### Defconfig (sheng_macos_defconfig vs ruan_macos_arm64_ufs_defconfig)

Sheng sets these and ruan does not: `NATIVE_RAM_BLOCK`, `OWNER_DATA_MOUNT`, `OWNER_DATA_VOLUME_UUID`, `OWNER_UNION_SYSTEMOS`,
`OWNER_CACHE_MOUNT/STARTUP`, `OWNER_BOOT_MANIFEST`, `OWNER_ROOT_TRUST/AUDIT`, `SOURCE_STORAGE`, `NATIVE_ADRENO`.
- For the desktop ruan needs the first three (row 4).
- The SystemOS union and cache mount can be avoided by merging the cryptex into our own root tree.
- ROOT_TRUST matters only for the authenticated-open fix (row 13).

### Kernel args sheng passes for macOS that ruan lacks (relevant ones)

| Arg | What it does | ruan |
|---|---|---|
| `apfs-boot-shim=1` | shows the owner `apfs_boot_util` over apfs.fs so mount-phase-1/2 succeed | replace the file in our root instead (row 6) |
| `disable-watchdogd=1`, builtin `disable-jobs=com.apple.cameracaptured` | `BootWatchdogPolicy.c` writes launchd's `disabled.plist` | put it in the seed (row 7) |
| `data-persist=2`, `data-reset=` | persistent UFS Data overlay / re-init from seed (ShengUfs) | not needed while Data is on RAM |
| `no-lc=1` | skip launch constraints | add (row 11) |
| `aks=1 aks-sls=3` | key store stand-in, lock state "no passcode" | ruan has `aks=1` |
| builtin `cs-lazy=1 timebase-user=1 armcpu-edge=1 boot-payload-release=1 thermal* board-clock=7` | lazy 16 KiB CS, trap-free timebase, Edge matrix, return addon memory, thermal, RTC | cs-lazy and timebase-user are worth adding |
| builtin `owner-staged-apps=1 owner-dataprotect=1 validation_disables=0x11040 sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0` | iPadOS apps view, DP class emulation, DEVELOPMENT hygiene checks off | the hygiene trio is worth adding (row 12) |
| `-no_slto_panic`, `kt_fixed_zones=120` | no spinlock-timeout panic; fewer kalloc_type zones | add `-no_slto_panic` |
| `gpu-*`, `fb-*`, `dpu-scanout`, `ca-*`, `ci-mps=0`, `vsync*`, `offline-hid-*`, `touch*`, `keyboard`, `usb`, `wifi`, `bluetooth`, `audio`, `adsp`, `cpufreq`, `backlight`, `autoreset`, `uefi-watchdog`, `diag*`, `log*` | sheng hardware, Metal plugin, lab | GPU set only with row 15 |
| `-apfs_shared_datavolume` | **not used by sheng** (not in its args or builtins). Sheng's Data is mounted by the kernel | none |

### Root, Data and payload layout

| | sheng | ruan now |
|---|---|---|
| System | the unchanged, owner-verified OS image (read-only, own seal policy) + SystemOS unioned over `/` + App cryptex volume | own unsealed APFS on sda22: **restore ramdisk tree** + pacstripped cache in `/System/Library/dyld` |
| Data | persistent APFS (210 GiB) from the macOS seed (`make-macos-data-seed.py`, admin + auto-login, Setup done). The kernel mounts it at `/System/Volumes/Data` and sets the firmlinks | RAM disk volume passed in, **not mounted** (row 4) |
| Preboot/cryptex | OS cryptex view at `/System/Volumes/Preboot/Cryptexes/OS`; no Preboot volume (boot-uuid = OS UUID) | none; `dyld_flags=8` skips libignition |
| Payload addon | Metal plugin, boot shim, audio/Wi-Fi/BT bits, autologin addon | none (everything can go into our root, re-signed) |
| Install seed | "Initialize macOS Data and boot" (`data-reset=1`) | n/a |

## Task 2: known blockers after launchd and what fixed them

| Blocker | Seen on | Fix |
|---|---|---|
| launchd SIGSEGV after the cache maps (garbage class pointer in `realizeClassWithoutSwift`) | houji r50–r53 (iOS 27) | missing zzzz: 47-bit TTBR0, 16 KiB fixup assembly, posix_spawnattr, lockdown_mode. ruan's equivalent was the raw arm64e pointers, already fixed |
| `os_lockdown_mode_enabled` abort | sheng; houji's analysis; dby-w09 console stop | `lockdown_mode_state_public` sysctl (zzzz) |
| kqworkloop limit kills / wrong spawn attributes | sheng | `psa_from_darwin26` (zzzz) + `macos-kqworkloop-limits.patch` (in the series) |
| xzone reservation at 455–512 GiB | sheng, ruan now | `macos-user-address-space.patch` + T0SZ_USER (other agent) |
| `/private` and `/var` missing; Data role | sheng | `macos-owner-data-firmlinks.patch`, `DoNotUseSEP` (ruan has it) |
| mount-phase-1/2 failing | sheng | `apfs-boot-shim=1` + owner `apfs_boot_util` |
| swap EROFS → swap threads spin | sheng | `macos-vm-volume.patch` |
| mobileactivationd restarting every 10 s | sheng | `macos-hardware-volume.patch` |
| fskitd exits / launchd restarts it | sheng | `com.apple.filesystems.lifs` in the prelink roots (profile) |
| IOKit never quiet (dexts) | sheng run29/31 | authenticated-open patch + driverkit.serial / UVCService roots |
| CoreServices aborts on two volumes on `/` | sheng | `macos-union-one-volume.patch` |
| jetsam highwater kills (dock.extra, iconservicesagent) | sheng | `xnuconfig-macos-memorystatus-policy.patch` |
| AMFI "failed to create OSDictionary copy: 14" | sheng (4 KiB kernel) | `unixnu_kernel_stack_shift` binding + stack.c alignment |
| watchdogd without hardware | sheng | `disabled.plist` via `disable-watchdogd=1` |
| WindowServer SIGABRT `WSSystemCanCompositeWithMetal` | sheng run64, elden | a Metal device (Adreno plugin) or elden's software rules |
| WindowServer half-float frames | sheng | `text_fixups` at .03 0x5de1134, then `fb-half=2` |
| Language Chooser / Setup / activation | sheng | seed with `.AppleSetupDone`, SkipSetupItems, user |
| black screen behind MiniBuddy, no Dock/Finder | sheng run42 | user home in the seed with `.skipbuddy` |
| Keyboard Setup Assistant prompt | sheng | `--keyboard-type` in the seed |
| auto-login | sheng | `autoLoginUser` + `kcpassword` (seed `--auto-login`) |

## Task 3: what is specific to ruan

- **No PAC.** ~2,114 cache instructions are left to the trap emulator. WindowServer, QuartzCore and Metal hot paths that hit
  `LDRAA`/`LDRAB` will be slow. Count them per process with the USB `pac` command before blaming GPU or display.
- **The ad-hoc root has two sides.**
  - Cost: binaries are not platform (row 8), and every new binary (boot shim, Metal plugin, autologin tools) needs its CDHash
    in the trust cache.
  - Benefit: we own the bytes. Sheng's kernel-side work-arounds can become plain file edits before re-signing: text fixups
    (row 16), timebase-user, apfs_boot_util (row 6), merging SystemOS into the tree, disabled.plist and seed contents. That
    means fewer kernel patches and no addon/union machinery.
- **The root is unsealed and unverified** (`native-ufs-allow-unverified=1`, `lp-sip0=0x867`). Any macOS path that asks APFS
  for a seal (authenticated open, `apfs_checkseal`, rosp) fails differently from sheng's owner-verified root. Rows 13, 6 and
  `macos-read-only-system-volume.patch` assume sheng's layout, so check their conditions against ours.
- **4 KiB kernel** (shared with sheng):
  - the 16 KiB CS blocks → `cs-lazy`; or re-sign with 4 KiB pages (ruan only);
  - the stack-shift check (row 10);
  - `map_with_linking_np` page size (zzzz). Without it dyld falls back to in-process fixups, which is slower but works;
  - the dyld-pager 16 KiB assembly (other agent).
- **Small user VA today** (256 GiB, `ruan-user-va-clamp`, `ruan-small-va-data-range`). These are temporary until the 47-bit
  port lands. Drop them then, or xzone and the data range will disagree with `macos-user-address-space.patch`.
- **RAM Data, no swap, a 2.75 GiB DarwinBoot window.** Sheng had 6 GiB plus the adopted low bank and 4 GiB of swap.
  WindowServer with software rendering will press on memory.
- **No GPU stack in the macOS build.** Sheng's desktop always had Adreno Metal, so the GPU (row 15) is the one large piece
  sheng's config doesn't hand us.

## Suggested batches

1. Next kernel (with the 47-bit port):
   - patches: rows 1, 2, 10, 13;
   - defconfig: row 4;
   - args: `no-lc=1 -no_slto_panic cs-lazy=1 sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0 validation_disables=0x11040`;
   - ADT: boot-uuid (row 9).
2. Next root and Data:
   - full OS + SystemOS (row 3);
   - boot shim in the root (row 6);
   - Data seed with skip-setup, user and auto-login, plus `disabled.plist` (rows 5, 7);
   - cache fixup baked in (row 16).
3. Display: Metal plugin port or software rules (row 15). Then SMP (row 17).

Not checked:
- The sm8550 SoC feed (sheng's `soc:` series) is not local.
- `141-22145-021.dmg.aea` decryption is still pending.
- Whether dyld really falls back when `map_with_linking_np` refuses 16 KiB.
- Why the trust-cache entries are not platform (row 8).
