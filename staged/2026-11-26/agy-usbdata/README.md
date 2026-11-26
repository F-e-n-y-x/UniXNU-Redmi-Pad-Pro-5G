# Fast RAM-Root USB Staging (~70s Boot Cut)

### 1. Unified Diffs
- **`UsbRamdisk.h.diff`**: Relaxes receiver download size check from `DownloadSize == Size` to `0 < DownloadSize <= Size`. Replies `DATA<DownloadSize>`, streams `DownloadSize` bytes, zero-fills remainder up to `Size`, and checks descriptor CRC32 across full `Size`. Full-size transfers remain 100% backward compatible.
- **`zima-ramroot-macos.sh.diff`**: Keeps descriptor verification (`max-download-size == file_size`), computes used bytes (or reads `root-usb.used`), creates a temp prefix via `head -c` on tmpfs, stages `~72 MiB` instead of `~1480 MiB`, and cleans up. `FULL_STAGE=1` bypasses truncation.

### 2. Robust Used-Length Computation
The APFS image allocates metadata and files contiguously at the start; free slack (`DATA_SLACK`) is trailing zeros.
Scan backward from EOF in 16 MiB chunks comparing against pre-allocated zero buffers via C `memcmp`. On finding the first non-zero chunk, scan backward block-by-block (4 KiB) for the first non-zero block $B$. Used length is $(B + 1) \times 4096$.
This scan completes in ~0.3s across a 1.48 GiB image. Can also be pre-recorded at build time in `root-usb.used`.

### 3. Deployment & Rebuild Scope
- **Rebuilt**:
  - `DarwinBoot.efi`: Recompile `DarwinBootPkg` via `scripts/build-darwinboot.sh` (or EDK2 `build -p DarwinBootPkg/DarwinBoot.dsc`).
  - `ruan-ramroot-usb-macos.img`: Inject updated `DarwinBoot.efi` into the UEFI firmware volume (`SILICIUM_UEFI.fd`) via `boot_template.replace_loader()`, and re-pack the Android v4 boot image (`gzip(BootShim + FD + XNUP) + DTB`).
- **NOT Rebuilt**:
  - XNU kernel Mach-O (`kernel.macho`).
  - Payload FAT/XNUP (`root-usb.desc` is unchanged: descriptor size and CRC32 are calculated over the full image and remain identical).
  - Storage partitions (RAM-only execution).

### 4. Risks & Mitigations
- **Dirty buffer**: `DarwinBoot.c` already zeroes the window (`ZeroMem(Address, Used)`). Defensive `ZeroMem(Destination + DownloadSize, Size - DownloadSize)` in receiver guarantees zero-fill.
- **CRC verification**: Descriptor CRC covers full `Size`. Because suffix is all zeros, prefix + zero-fill produces identical CRC32.
- **Old receiver mismatch**: An unpatched receiver rejects `DownloadSize < Size` with `FAILsize differs from the descriptor`. Mitigated by updating boot image first or setting `FULL_STAGE=1`.
- **Temp file space**: Prefix is ~72 MiB, written to tmpfs (`/tmp`) in ~0.08s and unlinked on trap.
