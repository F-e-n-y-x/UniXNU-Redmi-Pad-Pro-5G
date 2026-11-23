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
