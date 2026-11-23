# macOS on Redmi Pad Pro 5G (ruan)

A native macOS port for the tablet, on a CPU without pointer authentication (PAC). It reuses the iPadOS port's drivers
(`../unixnu-ruan/`). **Status (2026-11-24, UFS boot 17): root on UFS (`sda22`), kernel boots, launchd runs and spawns system daemons. The root is still the restore ramdisk; next is the full OS system volume.**

## The problem: no PAC

macOS is built only for `arm64e`: every system binary uses pointer authentication, an ARMv8.3 feature. The tablet's Cortex-A78/A55
are ARMv8.2 and the hypervisor can't emulate PAC. Most PAC instructions are HINT no-ops on older CPUs; the rest (`AUT*`, `BRAA`,
`RETAA`, `LDRAA`, `PACGA`) are undefined and trap.

## The approach

1. **Kernel without PAC:** XNU 12377 (macOS 27.3, `26C5035r`) built as plain `arm64` with ruan's board patches.
2. **Kernel collection rewritten:** `tools/kc-nopac.py` rewrites the 250,332 PAC instructions in the prelinked collection.
3. **Userland rewritten:** the dyld shared cache and system binaries are rewritten (`unixnu-ruan-macos/package/pacstrip/`); the 2,114
   instructions that can't be rewritten are left to a kernel trap emulator.

Background: [docs/macos-no-pac.md](docs/macos-no-pac.md) (research and blueprint), [docs/feasibility-research.md](docs/feasibility-research.md),
[docs/pac-workaround.md](docs/pac-workaround.md), [docs/scene-2026-11-13.md](docs/scene-2026-11-13.md).

## Boot log

Details in [docs/first-boot.md](docs/first-boot.md). All boots are RAM-only (`fastboot boot`); nothing was flashed.

| Boot | Result | Fix |
|---|---|---|
| 1 | UniXNU menu, then `'UniXNU' returned: Load Error` | wrong file-type field in the kernel collection (fileset subtype) |
| 2 | panic `arm_vm_init.c:1376` (contiguous-hint assert) | `use_contiguous_hint=0` in the board's kernel args |
| 3 | panic `recount_track_cpu_kind: element modified after free` | `unixnu-ruan-macos/patches/xnu-12377/ruan-recount-kind-index.patch` |
| 4 | through AMFI, trust caches, IOKit and BSD init; panic `machine_routines.c:3671 _unsafe_kernel_text_initialized` (USB debug never enumerated) | `ruan-no-ctrr-chosen.patch`: no `/chosen/kernel-ctrr-to-be-enabled` from iBoot |
| 5 | root mounted from the RAM disk, `/sbin/launchd` exec'd, then AMFI killed it: rewritten binaries are "completely unsigned code" | test: `amfi_get_out_of_my_way=1` (boot 6); real fix: re-sign + trust cache |
| 6 | AMFI panics on `cs_enforcement_disable` ("can't has cs_enforcement_disable boot-arg") | boot 7: only `amfi_get_out_of_my_way=1` |
| 7 | same AMFI kill: the kernel can't load the stale Apple signature at all | `tools/adhoc-resign.py` + `rebuild-macos-root.py`: ad-hoc signatures, CDHashes in the trust cache |
| 8 | **launchd runs**; dyld finds no shared cache, init exits, pmap panic | needs the 7.1 GB dyld cache: a UFS root (too big for RAM) |

## Folder

| Path | Contents |
|---|---|
| `unixnu-ruan-macos/` | The macOS board dir (a fork of `../unixnu-ruan/` plus the macOS changes; it lags the iPadOS drivers): `board.toml`, `configs/ruan_macos*_defconfig`, `patches/xnu-12377/`, `package/pacstrip/`, `package/RuanMacBootProgress.c`, `scripts/build-gop-legacy-display-27.py` |
| `unixnu-core-macos/` | Changes to the UniXNU checkout used for macOS, as patches (`apply.sh <UniXNU>`; commits in `COMMITS.txt`) |
| `tools/` | `kc-nopac.py` (kernel-collection PAC rewrite), `build-macos-ramroot.sh` (RAM-boot set), `zima-ramroot-macos.sh` (boot it), `pacscan*.py` / `pacstats.py` / `featscan-entries.py` (PAC and CPU-feature scans) |
| `docs/` | Research, the boot log and the old task brief |

Not in git (Apple binaries, large): the work trees `../host-work/UniXNU-macos` and `../host-work/UniXNU-ruan-macos` (live copies of
the two folders above), build outputs in `../host-work/macos-build/out/`, and the finished boot set in `../host-work/macos-extract/`
(`ruan-ramroot-usb-macos.img`, `root-usb.apfs`, `data-usb.apfs`, recovery root; check with `sha256sum -c SHA256SUMS-macos`).

## Run boot 4

```bash
cd "../host-work/macos-extract"
sha256sum -c SHA256SUMS-macos
./zima-ramroot-macos.sh                       # tablet in fastboot; RAM boot, nothing flashed
python3 ../../tools/ruan-usbdbg.py dmesg      # read the console over USB while it boots
```

Kernel args for the debug console: `ruan-usbdbg=1 ruan-usbdbg-reboot=1 ruan-ramoops=1`. The payload is up to 256 MB, so its window
differs from iPadOS: `PAYLOAD_BASE=0xA8000000 PAYLOAD_MAX=0x10000000` for `../uefi/mkxnuimg-ruan.py`.

## Roadmap

1. Boot 4: read how far the kernel gets over USB; fix panics until launchd starts.
2. Boot the recovery root to a shell, then WindowServer on the GOP framebuffer.
3. Bring over the iPadOS drivers that work: UFS root, zero-copy display, Metal on Adreno, touch, Wi-Fi.
4. Install to the `unixnu` partition.

## Credits

[UniXNU](https://github.com/UniXNU) (sheng-macos-link-lab booted a macOS desktop on hardware with PAC); darwin-lisa / Bergamota for
the PAC-neutralisation approach; BxiiDN/macOS-Pad-6 for the SM8250 pacstrip work.
