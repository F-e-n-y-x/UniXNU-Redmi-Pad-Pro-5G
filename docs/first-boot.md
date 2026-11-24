# macOS 27.3 first RAM boot: `'UniXNU' returned: Load Error`

2026-11-21. Desk analysis of `macos-extract/ruan-ramroot-usb-macos.img`. The tablet was not touched.

## Symptom

`fastboot boot ruan-ramroot-usb-macos.img`: UEFI and the UniXNU menu (0.1.0) come up, then `'UniXNU' returned: Load Error`.
DarwinBoot never starts the USB receiver (`unixnu-ramdisk`).

## What the image holds (verified)

- Boot image v4: gzip kernel blob of 39,461,941 B, which unpacks to 199,229,632 B. XNUP sits at +0x3000b0 and holds a 187 MiB FAT32.
  ABL accepted it, because the UEFI ran.
- The FAT holds `EFI/UniXNU/unixnu.toml` (entry `xnu`, path `Payloads/kernel.macho`, initrd `Payloads/scratch-data.img`, a 32 B
  DBUSBRD1 descriptor). The Payloads are `kernel.macho` (142,278,656 B, identical to `out/ruan/.../kernel/kernel.macho`),
  `.adt` (16,060 B) and `.trustcache` (449,148 B, the trust.bundle). The layout matches the iPadOS rbu176u payload.
- The kernel is an `MH_FILESET` (type 12, arm64e) with 79 entries. Its VA span is 135.7 MiB, which fits DarwinBoot's 256 MiB window
  (limit 254 MiB). `KERNEL_LIMIT` is also 256 MiB. Neither size limit is the problem.
- The iPadOS kernel that works is `MH_EXECUTE` arm64 (subtype 0), so DarwinBoot's fileset path had never run on the tablet before.

## Cause 1: the loader rejects the kernel (verified)

`drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c` `ParseKernel()` checks the `com.apple.kernel` fileset member:

```c
(U32 (Data + Header + 8) != U32 (Data + 8))   // member cpusubtype must equal the fileset's -> EFI_LOAD_ERROR
```

- Fileset header: `0xc0000002`, which is Apple's, taken over from the IPSW kernelcache.
- Member `com.apple.kernel` at file offset 0x198000: `0x80000002`. This is the native XNU build, which lacks the kernel-ABI bit 0x40000000.
- In Apple's own 26C5035r kernelcache both values are `0xc0000002`.

Proof: `drivers/DarwinBoot/Test/test-kernel-parser.py <kernel.macho>` compiles the production parser.
It fails on this kernel and passes on the rbu176u kernel. With the member subtype changed to `0xc0000002`, this kernel passes too.
The mismatch is therefore the only parser failure.

Fix options: (a) `kits/XnuPrelink/fileset_emit.py` writes the member subtype equal to the fileset's.
(b) DarwinBoot compares `& 0x00ffffff` only, which needs a UEFI rebuild.
(c) A quick repack that patches the payload copy:

```bash
python3 - EFI/UniXNU/Payloads/kernel.macho <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b'); d = f.read(); top = d[8:12]
o, end = 32, 32 + struct.unpack_from('<I', d, 20)[0]
while o < end:
    c, l = struct.unpack_from('<II', d, o)
    if c == 0x80000035 and d[o + struct.unpack_from('<I', d, o + 24)[0]:].startswith(b'com.apple.kernel\0'):
        f.seek(struct.unpack_from('<Q', d, o + 16)[0] + 8); f.write(top)
    o += l
PY
```

## Cause 2: the kernel is a native-PAC arm64e build (verified), so it can't run on the A78/A55 (inferred)

The README says the kernel is "plain arm64, HAS_NATIVE_PAC=0". That is not what is in `out/`:

- `profiles/os/macos-27.3-26C5035r/profile.toml` has `arch = "arm64e"`. The native XNU (`.../DEVELOPMENT_ARM64_BCM2837/mach...noctf`)
  is `Mach-O arm64e (caps: PAC00)`.
- Non-HINT PAC instructions (RETAA/RETAB, BRAA/BRAB) count 28,597 in the XNU member's `__TEXT_EXEC` and 413,320 across all of
  `__TEXT_EXEC`, which includes the 78 Apple kexts. BLRAA/BLRAB adds another ~244k.
- `ruan-pac-emulator.patch` only handles **user** traps (`pac_emulate_user_trap`, copyin).
- ARMv8.2 has no FEAT_PAuth. The first kernel `retab` is UNDEFINED, which means a hang right after DarwinBoot's green bar with no
  panic output. That outcome is inferred and has not been run.

What it takes: build XNU with the `arm64` arch profile (no `native-pac-*` patches), run `pacstrip` over the kext text of the kernel
collection (or emit it from the IPSW's kexts after stripping), then re-emit the fileset with matching subtypes. Then rebuild the payload FAT,
the XNUP and the boot image (the agy steps; `build-ramroot.sh` has no macOS mode) and run `sha256sum`.

## Not checked

ADT window-size, trust-cache validation and the later `EFI_COMPROMISED_DATA` paths were not simulated past `ParseKernel`.
It was also not verified that the FD in the image was built from the current DarwinBoot.c (the submodule has staged changes).

## Fix (2026-11-21): plain arm64 kernel, stripped kexts, consistent subtypes

Rebuilt with `macOS/unixnu-ruan/scripts/build-macos-ramroot.sh` (desk only; the tablet was not touched):

- Kernel: XNU 12377 as **plain arm64** (`[os_overrides."macos-27.3-26C5035r".os] arch = "arm64"` in `UniXNU-ruan-macos/board.toml`,
  `configs/ruan_macos_arm64_defconfig`, out dir `ruan_macos_arm64`). The arm64e arch series stays: its `native-pac-*` code is under
  `HAS_NATIVE_PAC`, which `BCM2837.h` defines only for `__arm64e__`, so it compiles out. Added `ruan-arm64-kernel-chained-fixups.patch`
  (`ld -fixup_chains -kernel`: format 6 chains, now read by `kits/XnuPrelink/kc_image.py`), a portable `ptrauth_strip` in
  `fileset-unmapped-call.patch`, and an arm64 mode in `build-gop-legacy-display-27.py` (plain vtable relocations).
- `fileset_emit.py` / `fileset_segmented.py` write the `com.apple.kernel` member's cpusubtype equal to the collection's (0xc0000002).
- `scripts/kc-nopac.py`: pacstrip of every entry (250,332 instructions, 0 left to emulate), clears the auth bit of the root chained
  fixups (298,406 of 455,801; a non-PAC XNU stops at "Unexpected authenticated fixup"), reseals the corecrypto FIPS HMACs
  (kernel, com.apple.kec.corecrypto), then checks: 0 PAC instructions, 0 auth fixups, subtypes equal, seals match.
  `test-kernel-parser.py` passes. Old arm64e set: `/root/unixnu-work/macos-build/backup-arm64e-20261121/`.

Still not checked: ADT/trust-cache paths after ParseKernel; the dyld cache's user-mode PAC leftovers (2,114, emulator only);
corecrypto's SHA3/SHA512/SB code (assumed behind CPU-capability checks); AppleARMWatchdogTimer's Apple system register
(S3_5_C15_C2_6) if it ever matches.

## Boot 2 (2026-11-21): panic at arm_vm_init.c:1376, contiguous hint

The arm64 set booted through policy and trust-cache load, then `arm_vm_init.c:1376` asserted
(`!kva_active || ppte[i] == ARM_PTE_EMPTY || (ppte[i] & ARM_PTE_HINT) == (ptmp & ARM_PTE_HINT)`). In this fileset the native
kernel `__TEXT` (0xfffffff00719c000, +0x1a8000) lies inside the collection's `__PRELINK_TEXT` (0xfffffff00700c000, +0xdb8000).
arm_vm_init maps `__PRELINK_TEXT` without the hint and then `__TEXT` with `ARM64_GRANULE_ALLOW_HINT`. arm_vm_prot_finalize (live
tables) re-protects `__PRELINK_TEXT` without the hint, which clears it on the hinted kernel `__TEXT` PTEs, so the assert fires. kc-nopac
did not move anything; the segment layout is the prelinker's. Fix: `use_contiguous_hint=0` in the board's macOS kernel_args, the
same switch UniXNU's 4 KiB fileset boards pass (vermeer record). Hints are never set, so both hint asserts hold.

## Boot 3 (2026-11-21): recount panic, and USB debug added

- Panic `recount_track_cpu_kind: element modified after free (sz:24)`. The recount tracks are indexed by CPU kind (P = 0, E = 1),
  but xnu-12377 sizes them by the popcount of the kinds present. With `cpus=1` only cpu0 (a Cortex-A55, E) runs, so recount
  allocated one 24-byte track and wrote index 1 past it. Fixed by `UniXNU-ruan-macos/patches/xnu-12377/ruan-recount-kind-index.patch`,
  which sizes the tracks to the highest kind + 1.
- USB debug: `package/UsbDebugCdc.cpp`, `UsbDebugHw.h`, `UsbDebugCommands.h` and `BootReset.c` (current iPadOS copies) plus a new
  `package/RuanMacBootProgress.c` (12377 boot-progress hooks). The hooks tee the console into the ramoops zone and add
  `[ev]` lines and a one-second `[hb]` heartbeat. `ruan-usbdbg-early.patch` starts the USB thread right after `PE_init_iokit`
  and interrupts on. Built by `build-gop-legacy-display-27.py`, which gives the iPadOS-only weak imports weak no-op
  definitions. Kernel args: `ruan-usbdbg=1 ruan-usbdbg-reboot=1 ruan-ramoops=1`.

## Boot 4 (2026-11-23): far past early boot, panic on a missing /chosen property

Photos: `testing/20261123_172902.jpg`, `20261123_172918.jpg`. The kernel got through zone init, corecrypto's FIPS self-tests, NVRAM,
AppleImage4, AMFI, the Sandbox/Quarantine/EndpointSecurity policies, both external trust caches, IOKit power management,
CoreAnalyticsHub, BSD init ("Copyright (c) 1982 … Regents") and `dlil_init`, then panicked:
`machine_routines.c:3671 Assertion failed: _unsafe_kernel_text_initialized`. `ml_unsafe_kernel_text()` asserts that
`ml_unsafe_kernel_text_init` found `/chosen/kernel-ctrr-to-be-enabled`, which Apple's iBoot sets and DarwinBoot does not.
Fix: `patches/xnu-12377/ruan-no-ctrr-chosen.patch` treats the missing property as "no CTRR" (kernel text not locked, true on
SM7435). Only DTrace and the PPL switch read it, and this build has no PPL.

The USB debug port (`ruan-usbdbg=1`) never enumerated on the host, although the panic came after IOKit start; to check next.
The panic text on screen has doubled characters (two console writers), which the photo still shows readably.

## Boot 5 (2026-11-23): root mounted, launchd killed by AMFI

Photo: `testing/20261123_181700.jpg`. With `ruan-no-ctrr-chosen.patch` the kernel loaded the APFS and compression kexts, attached
the RAM disk (`md0`), mounted the System volume (`apfs_vfsop_mount … mount-complete volume System`), started swap and exec'd
`/sbin/launchd`. AMFI then refused it: `check_signature(pid 1): error = 85`, `load code signature error 4 for file "launchd"`
(also `dyld`), `AMFI: hook..execve() killing init (pid 1): Attempt to execute completely unsigned code (must be at least ad-hoc
signed)`, and the kernel panicked on init's death. Cause: the PAC rewrite changes the binaries, so their code signatures no longer
match. The USB port enumerated (`/dev/ttyACM0`) but the debug client got no answer.

Boot 6 tests the shortcut: the same kernel with `amfi_get_out_of_my_way=1 cs_enforcement_disable=1` (DEVELOPMENT kernel). The
proper fix is to ad-hoc sign every rewritten binary and add the new CDHashes to the trust cache, as the iPadOS root does.

## Boot 6 (2026-11-23): AMFI refuses cs_enforcement_disable

Photo: `testing/20261123_190433.jpg`. AMFI's init panics as soon as it sees the boot arg: `_initializeAppleMobileFileIntegrity():
cs_enforcement disabled by boot-arg` → `panic: "can't has cs_enforcement_disable boot-arg"`. Boot 7 keeps only `amfi_get_out_of_my_way=1`.

## Boot 7 (2026-11-23): amfi_get_out_of_my_way=1 is not enough

Photo: `testing/20261123_191239.jpg`. Same AMFI kill as boot 5. The kernel cannot even load the signature (`load code signature
error 4`): the rewritten arm64e binaries still carry Apple's CMS blob, which no longer matches their CodeDirectory, and their
new CDHashes are in no trust cache. No boot arg gets past that.

## Boot 8 (2026-11-23): launchd runs; no dyld shared cache

Photo: `testing/20261123_192035.jpg`. Fix for boot 7: `tools/adhoc-resign.py` makes all 546 rewritten binaries ad hoc (recomputed
code hashes, CS_ADHOC, empty CMS) and `tools/rebuild-macos-root.py` rebuilds the root (now compressed: 760 MB instead of 1.3 GB)
and adds the 1,052 CDHashes as a third trust-cache module. The kernel loaded all 3 modules and **exec'd launchd** (`exec: pid 1
launchd`). dyld then found no shared region (`check_np … vm_shared_region_start_address() returned 0x1`, `result from check_np():
-1, errno 12`): the recovery root has no dyld shared cache. launchd exited, and the kernel panicked in `pmap_enter_options_internal:
attempt to map illegal VA 0x7d5… in pmap` (pmap.c:6290), below the 4 GB page zero, while tearing down init.

**Blocker:** macOS 27.3's dyld shared cache (rewritten by pacstrip) is 7.1 GB in 84 files (`host-work/macos-extract/26C5035r__Mac17,11/`).
A RAM root is capped at about 1.7 GB by DarwinBoot's window, so macOS needs a root on UFS, as the iPadOS build uses (`sda22`).
The previous boot set is kept in `host-work/macos-extract/set-boot7/`.

## Toward a UFS root (2026-11-23, in progress)

The iPadOS root on `sda22` is backed up (`host-work/backups/`, SHA-256 checked against the tablet; restore steps in its README).
Kernel side, `UFS=1 tools/build-macos-ramroot.sh` (variant made by `tools/make-macos-ufs-board.sh`):
- `ruan_macos_arm64_ufs_defconfig` (`CONFIG_XNU_NATIVE_UFS=y`), `ruan-ufs-unverified-root.patch` (12377 port, made against the
  tree after the other patches: the `owner_authorized` check), ShengUfs from the iPadOS variant (2f6c7b0 + dizi + ruan edits).
- The kernel and ShengUfs compile. The link fails on `IOBlockStorageDevice` (IOStorageFamily) symbols: the iPadOS build binds them
  with `build-ufs-original-bindings.py`, which is pinned to the iPadOS 17 kernelcache and a prelinked candidate. For macOS that
  binder needs the 26C5035r kernelcache (`d1003b51…`), IOStorageFamily `7edb88fb…`, and a prelinked macOS candidate. Next step.
Root side: the root needs the SystemOS cryptex (`141-22145-021.dmg.aea`, AEA-encrypted) for the libraries beside the dyld cache;
decrypting it on Linux needs blacktop `ipsw` (downloaded to `host-work/tools/ipsw-3.1.750/`, not yet approved to run).
`tools/adhoc-resign.py` already re-signs dyld caches too.

## UFS boot 1 (2026-11-23): NativeUfs starts on macOS, faults in bzero

`sda22` flashed with `macos/tools/make-macos-ufs-root.py` output (2.71 GiB: signed recovery root + the 84-file dyld cache in
`/System/Library/dyld`, root volume `4C15B884-6D41-FA68-15FA-10423D08C1A0`), kernel `UFS=1` booted from the Pi 5. Photo
`testing/20261123_222128.jpg`. IOKit's personality first logs `Couldn't alloc class "NativeUfs"` (expected: the class registers
late). Then `RuanMacUfs` registered the source IOBlockStorageDevice and NativeUfs, which started (`NativeUfs: host DMA
0x8fe55e000 + 0x20a000`) and panicked: `Unaligned kernel data abort at pc 0xfffffff00c22e384` = `_bzero`, lr = `hostContiguous`
(clearing an uncached/write-combined DMA buffer). Fix in `scripts/build-ruan-mac-ufs.py`: the macOS copy of NativeUfs.cpp
replaces every bzero with aligned 64-bit (or byte) stores. The USB console did not come up (panic before its thread).

## UFS boots 2-4 (2026-11-23): alignment, SIP, launchd runs from UFS

- **Boot 2:** `Unaligned kernel data abort` in `hostIssue` (UPIU fields written into an uncached/write-combined UCD; this kernel maps
  those as Device memory). Fix: the macOS NativeUfs copy is built with `-mstrict-align` and its `memcpy`s use an aligned copy.
- **Boot 3** (`testing/20261123_223921.jpg`): root mounted from UFS (`disk3s1`, `4C15B884…`, "rootvp not authenticated: accepted
  read-only for UFS bring-up"), launchd exec'd, dyld found the cache but the kernel refused to map it: `vnode is not SIP-protected`
  (vm_unix.c: the cache needs SF_RESTRICTED unless SIP allows an unrestricted FS). Fix: `lp-sip0 = 0x867` in the board's
  `[adt.chosen.asmb.u64]` (SIP disabled + unauthenticated root) over the profile's 0.
- **Boot 4** (USB log `host-work/logs/macos-ufs1/ufs4-dmesg-*.txt`): no mapping error; launchd lives but starts nothing, waking the
  CPU ~1300×/s (`process launchd[1] caught waking the CPU … 1302 wakes / second`), then a panic that the panic writer itself turned
  into `pe_init.c:741 Assertion failed: panic_info` (no panic region from the bootloader), hiding the original message
  (`testing/20261123_225743.jpg`).
- Next build adds: `ruan-no-panicinfo-assert.patch` (the first panic stays on screen), `ruan-sctrace.patch` (`ruan-sctrace=N`:
  launchd's first N syscalls with arguments and results), and a USB `pac` command (PAC emulator total, per task, top PCs).
- Also fixed along the way: the USB debug client needs `tools/debug_screen.py` next to it (the Pi captures were empty without it);
  `UNIXNU_PMGR_CHECKED=1` skips UniXNU's PMGR symbol check for this board (it can't read fileset symbols; AMFI starts fine here).

## UFS boots 5-7 (2026-11-23/24): the cache maps, launchd dies on a raw arm64e pointer

- **Boot 5** (syscall trace, `host-work/logs/macos-ufs1/ufs5-*`): PAC emulator count 0. dyld opens the 84 cache files one by one and
  `F_ADDFILESIGS` validates each whole file (~3 s each, ~4 min): the "1300 wakes/s" was that I/O. Then `dyld cache mapped
  system-wide: development, auth GOTs: mapped`, and `pid 1 exited -- exit reason namespace 2 subcode 0xb` = SIGSEGV
  (`testing/20261123_231851.jpg`).
- **Boot 6** (`ruan-uabort-log.patch`): `addr 0x801e6afc62112da0 pc …704 x16 0x801e6afc62112da0`: x16 holds an unprocessed arm64e
  chained-fixup word (auth bit, key DB, diversity 0x6afc, target 0x62112da0). Two non-PAC gaps found in XNU and patched
  (`ruan-dyld-pager-arm64e.patch`: the dyld pager had no case for arm64e pointer formats; `ruan-shared-region-auth-store.patch`:
  the v3/v5 cache slide computed auth pointers but stored them only under `ptrauth_calls`).
- **Boot 7** (`testing/20261124_001501.jpg`): same pointer, byte for byte. The faulting code is dyld itself, `dyld4::prepare`
  at dyld offset 0x33704: `ldr x16, [x24]` (libdyld's `__DATA_CONST,__helper` object in the shared cache, i.e. its vtable pointer)
  then `ldr x8, [x16]`. So that cache page was never slid. The cache uses slide info v5 for all data mappings.
- **Boot 8** adds `ruan-srtrace.patch` (`ruan-srtrace=N`: every mapping dyld requests with its VM_PROT_SLIDE bit, every slide
  setup, the first N slid auth pointers) to see where that page is skipped.
- `ruan-panic-fastboot.patch` (restart into fastboot after a panic) is built in from boot 7; panics on these boots were still
  nested, so it has not been seen working yet.

## UFS boots 8-14 (2026-11-24): cache fixups, address space, malloc

- **Boot 8** (`ruan-srtrace=N`): the cache slides were set up for every data mapping (slide info v5), yet no auth pointer was ever
  stored. UniXNU's 4 KiB kernel slides v5 pages in 4 KiB parts through `vm_shared_region_slide_part_v5`, which had the same
  `ptrauth_calls`-only store: `ruan-shared-region-part-auth-store.patch`.
- **Boot 9:** cache auth pointers correct (`part auth -> 0x1ff742da0`); launchd ran past dyld; panic `pmap_enter_options_internal:
  attempt to map illegal VA` (pmap.c:6290).
- **Boot 10** (`ruan-pmap-illegal-va-log.patch`: log + refuse for user pmaps): `illegal VA 0x7cc4000000 ... max 0x4000000000`: the
  macOS map limit is MACH_VM_MAX_ADDRESS while the 4 KiB tables (T0SZ 26) cover 256 GiB. `ruan-user-va-clamp.patch` clamps
  `pmap_max_64bit_offset` to `1 << PGTABLE_ADDR_BITS`.
- **Boot 11:** no illegal VA. launchd SIGTRAP (`user breakpoint esr 0xf2000001`). **The tablet restarted into fastboot by itself**
  (`ruan-panic-reset.patch`: 15 s, then PSCI SYSTEM_RESET, from `panic_spin_forever` and the panic reboot path; RuanMacUfs marks the
  PMIC reboot reason "bootloader" at boot), and the ramoops replay brought the previous boot's console over USB.
- **Boots 12-13** (`ruan-ubrk-log.patch`, `tools/cache-addr.py` with the cache .map): the brk is in `libsystem_malloc`
  (`__TEXT+0x46094`), message `BUG IN LIBMALLOC: pointer range initial reservation failed`: the macOS data/heap range is placed at
  97..127 TiB, outside the 256 GiB map. `ruan-small-va-data-range.patch` carves it out of [max/2, max) instead (boot 14).
- Live console on the Pi: `tools/host/live-log.sh` (reconnects after every reset).

## UFS boot 16 (2026-11-24): launchd runs (47-bit user VA)

The other macOS ports showed the real fix for the malloc abort: xzone malloc maps its range groups `VM_FLAGS_FIXED` at 455..512 GiB, so
user maps need a 47-bit TTBR0 (sheng's `T0SZ_USER`); the 256 GiB clamp could never work. Ported from UniXNU-sheng: `ruan-user-va-47bit.patch`
(T0SZ_USER 17, user L0 root, TCR switching), `ruan-dyld-pager-16k.patch` (macOS binaries use 16 KiB fixup pages; the pager walked 4 KiB),
`vmwait-dyld-pager-source-release.patch` (dyld pager deadlock). Dropped `ruan-user-va-clamp` and `ruan-small-va-data-range`.
Boot 16 (live log `host-work/logs/macos-ufs1/live-macos.log`): launchd prints `boot-mode set by sysctl: main`, `hello, launchd UUID`,
`Darwin Bootstrapper Version 7.0.0`, runs boot tasks (exclaves-boot, fsck/mount-phase-1/data-protection skipped), a child (pid 2) exits 1,
then launchd aborts (SIGABRT). Boot 17 adds sheng's Darwin 26 `posix_spawnattr` conversion and the lockdown-mode sysctl
(`ruan-darwin26-spawnattr.patch`, `ruan-darwin26-lockdown-sysctl.patch`). Roadmap: `next-steps.md`; other ports: `macos-ports-survey.md`.

## UFS boot 17 (2026-11-24): launchd spawns daemons — last test of this session

With the Darwin 26 `posix_spawnattr` conversion and lockdown sysctl, launchd leaves bootstrap mode and spawns services through
xpcproxy: `kernelmanagerd` (pid 3), `syslogd` (pid 4) and `restored_external` (pid 5) all reach "service state: running".
Then `kernelmanagerd` exits on SIGTRAP (after `objc[3]: Class SimpleKernelExtension is implemented in both KernelManagement and
kernelmanagerd`), `restored_external` (the restore daemon, the recovery ramdisk's main job) exits 1 without a restore host, and the
tablet resets into fastboot by itself (~170 s). The kernel side works; the root is the limit: sda22 holds the 26C5035r **restore
ramdisk**, not the macOS system. Next: build the root from the full OS volume + SystemOS cryptex (`141-22145-021.dmg.aea`, needs the
`ipsw` tool, not yet approved), then the Data volume (see next-steps.md rows 3-7), FIPS re-seal (HakiOS fips26.py), setup skip.
