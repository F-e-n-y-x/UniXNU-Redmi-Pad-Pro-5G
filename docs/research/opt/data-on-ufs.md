# Data volume on UFS + UFS I/O speed (research, 2026-11-21)

Read-only research. Nothing was built, flashed or run on the tablet.
**[V]** = verified in source or on disk (file:line given). **[I]** = inferred, must be tested.

Sources read: ShengUfs 2f6c7b0 + dizi patch (`/root/unixnu-work/ufs-variant/ShengUfs-src/NativeUfs.cpp`, `RootLayout.h`,
`RootCommands.h`, `RootPublication.h`, `RootWindowRead.h`), UniXNU 8542980 `drivers/RamBlock/{NativeRamBlock.cpp,PersistOverlay.h}`,
`docs/concepts/userdata.md`, `docs/xnu/boot-arguments.md`, device record `dizi.toml`, DarwinBoot `DarwinBoot.c`, and ruan's
`make-ufs-root.py`, `make-ufs-data.py`, `build-ramroot.sh`, `make-ufs-board.sh`, `build-ram-root-probe.py`, `ruan-ram-data-*.patch`,
`package/RamBlockNoStore.c`.

---

## 1. How dizi and upstream UniXNU handle Data

- **dizi does not keep Data on UFS yet** [V] (`dizi.toml` storage area). Its Data is a RAM seed packed into the boot payload
  (`packed_data_ramdisk = true`), served by NativeRamBlock and mounted by `dizi-data-volume.patch` (the owner Data mount). The record
  lists its limit as "Data lives in RAM: 128 MB, lost on reboot". Its next action is
  "Keep Data changes in a UFS store inside unixnu (data-persist)".
  Two of its findings matter here:
  - A seed of 512 MB or more, hashed page by page at start, reset the tablet at setconf.
  - A seed above 2 GiB refuses writes without a persistent store.
- **Upstream already has the mechanism** [V] (`docs/concepts/userdata.md` "Data volume", `NativeUfs.cpp:1539-1695`,
  `NativeRamBlock.cpp:207-312`):
  - Data is still a NativeRamBlock disk built from a packed seed (DBRDISK1, only the nonzero pages).
  - ShengUfs opens a **store window at the end of the root partition**: `first = last+1 - log_reserve - windowBlocks(pages)`.
    It refuses the window if it overlaps the root image plus a 4 MiB guard. `hostInWindow` checks every WRITE(10) against the open
    window (`RootCommands.h:507-565`), so nothing outside `unixnu` can be written.
  - Store layout: 1 header block, then the bitmap, then one block per Data page (`PersistOverlay.h:46-48`).
  - `data-persist=2` opens the window read-write. NativeRamBlock then runs in **direct mode**: changed pages live only on UFS, and
    unchanged pages come from the seed.
  - Write-back uses a 5 s flush timer and SYNCHRONIZE CACHE. `data-fua=1` switches to write-through.
  - Seeds above 2 GiB ("big") keep and hash only their nonzero pages.
  - First boot: an empty window gives `Kept` (the store is left alone and Data runs from the seed only). `data-reset=1` makes it
    `Fresh`. Later boots give `Resumed` (`PersistOverlay.h:72-120`).
- **ruan already has this wiring.** `ruan-ram-data-mount.patch` calls `native_ram_block_volume_device()`, which calls
  `attachStore()` before the mount (`NativeRamBlock.cpp:670`). ShengUfs's real `native_ufs_store_*` functions override the weak stubs
  in `RamBlockNoStore.c`. **[V]** Two things are missing:
  1. ruan's Data is a flat USB RAM disk. `attachStore` only runs for a sparse or big disk (`sparse.pages || isBig`, :220), and the
     DarwinBoot USB path forces `RamdiskPacked = FALSE` (`DarwinBoot.c:849-851`). So today the store can never open.
  2. `data-persist` is not in the boot-args.
- **Keybag/AKS** [I]: ruan runs the AKS stand-in (`aks=1 offline-key-events=1`, KeyStoreProbe.cpp). It hands out no keys and keeps
  the owned service for a persistent store (:123). The Data container from mkapfs has no content protection, so nothing is encrypted.
  The risk is on the **second** boot: keybagd finds the `/private/var/keybags/systembag.kb` it wrote earlier. No UniXNU SM7435 device
  has tested this yet. If keybagd fails, `data-reset=2` gives a fresh Data plus Setup.

## 2. Plan for ruan

### Plan A (recommended): upstream data-persist, no driver or kernel code

**Geometry** [V numbers, I fit]:
- `unixnu` is LBA 22147072–30535674 = 8,388,603 blocks of 4 KiB.
- The rbu85u image is 5,615,124,480 B = 1,370,880 blocks, so the image ends at LBA 23,517,952.
- Log reserve is 262,144 blocks (1 GiB) and the guard is 1,024 blocks.
- Room for the store: 6,754,555 blocks ≈ 25.7 GiB.
- **Use a 16 GiB Data**: 4,194,304 pages, so the window is 4,194,433 blocks at LBA 26,079,098–30,273,530.
- That leaves the root image room to grow to about 15 GiB. Data sits after the SystemOS window, at the end of the partition.

**Steps:**
1. Build the Data container at 16 GiB: `DATA_SLACK=4194304`, with `mkapfs` on Linux (`seed-data-grow.py` needs a Mac).
   Then **pack** it with DarwinBoot's `Test/packed_ramdisk.pack()`. The current tree packs to about 5 MiB
   (rbu131u: 1,176 nonzero pages of 265,397).
2. Ship the seed inside the boot payload as a packed data ramdisk, the way dizi does: no USB transfer and no `ram-data-split`.
   ruan's `ram-data-split` hook in NativeRamBlock is skipped for packed input [V] (`build-ram-root-probe.py:137-149`).
3. Boot-args: drop `ram-data-split=`, keep `ram-data=1 ram-data-uuid=<uuid>`, and add `data-persist=2 data-fua=1`.
   `data-fua=1` stays on while rbu85u's silent resets are open; drop it later for write-back.
4. **First boot only:** add `data-reset=1`, or `data-reset=2` for Setup. Never leave it on: every boot would wipe Data.
   A big seed with no store **blocks writes** (`NativeRamBlock.cpp:471`), so the first boot must have the reset.
5. **Keep the seed file fixed.** The seed hash is the store's identity. Rebuilding the seed starts Data over (state `Kept`), so a
   new seed needs `data-reset=1`.
6. Check `native_data_persist_state` (2 = resumed, 3 = fresh, 4 = error, 7 = kept) and `_first_lba` on the boot-progress page
   or in a `[hb]` line.

**Fallback:** the current USB flat Data (`UFS=1` without `PERSIST=1`). Leaving out `data-persist` (or setting `data-persist=1`,
read-only proof) gives today's behaviour. The store blocks are not touched by a reflash of `unixnu-root.img`: the image file ends
at LBA 23.5 M [I: assumes this ABL writes only the image's length and does not erase the partition first. Check once with
`data-persist=1` after a reflash].

Diff sketches (not applied):

```diff
--- scripts/make-ufs-data.py
+# PERSIST=1: a packed (DBRDISK1) seed for the payload, for data-persist=2 (docs/research/opt/data-on-ufs.md)
+if os.environ.get('PERSIST'):
+    sys.path.insert(0, '/root/unixnu-work/UniXNU/drivers/DarwinBoot/Test')
+    import packed_ramdisk
+    packed_ramdisk.pack(data, image)              # only nonzero pages; volume UUID unchanged
+    Path(f'{image}.uuid').write_text(volume_uuid(data) + '\n')   # same APSB scan as build-ramroot.sh
+    sys.exit(0)
 split = mr.DATA_ALIGN
```
```diff
--- scripts/build-ramroot.sh (UFS branch)
-	DATA_CONTAINER=1 RAMBLOCK=1; base=$OUT/data-usb; rd=()
+	if [ -n "${PERSIST:-}" ]; then base=$OUT/data-seed; rd=(); : ${DATA_SLACK:=4194304}; export PERSIST DATA_SLACK
+		for a in ram-data=1 ram-data-uuid=$(cat $base.apfs.uuid) data-persist=2 ${DATA_FUA:+data-fua=1} ${DATA_RESET:+data-reset=$DATA_RESET}; do extra+=("--kernel-arg=$a"); done
+	else DATA_CONTAINER=1 RAMBLOCK=1; base=$OUT/data-usb; rd=(); fi
 ...
-initrd=$root; [ $MODE = usb ] && initrd=$base.desc
+initrd=$root; [ $MODE = usb ] && [ -z "${PERSIST:-}" ] && initrd=$base.desc
+ramdisk_flag=--data-ramdisk; [ -n "${PERSIST:-}" ] && ramdisk_flag=--packed-data-ramdisk  # MUST VERIFY the payload CLI (payload.py:234)
```
The PERSIST path must also skip the DarwinBoot-window size check, which measures the raw image, and the `.desc`/`.split`
requirements. `boot-local.sh` must not send anything over USB when the out dir has no `data-usb.apfs`.

### Plan B (what was asked literally): a "Data" manifest window after SystemOS — more work, not recommended now

- `make-ufs-root.py`: add a third entry `('Data', data_at, data_bytes, …)` with `data_at = sys_at + roundup(sys_bytes, MiB)`.
  The manifest allows up to 4 windows [V] (`RootLayout.h:170`). `imageBytes` must cover the window, because the bounds check
  rejects `offset >= imageBytes` [V]. The flashed file can stop before the window, so a root reflash leaves Data alone.
- ShengUfs already publishes extra windows as IOMedia: `ufsBuildRootPlan` orders SystemOS, the extras, then OS [V].
  The publisher waits for any child media [V] (unverified build).
- What is missing:
  - `NativeUfsBlock` is hard read-only: `reportWriteProtection` returns true, and a write gives `kIOReturnNotWritable`
    (`NativeUfs.cpp:2006,2296`). A patch would need about 60 lines: a writable flag for the label "Data", `hostStoreOpen(abs/4096,
    bytes/4096)` at configure (shared with data-persist, which must stay off), a 4 KiB-aligned write path through
    `hostWrite4k(..., fua)`, `doSynchronizeCache → hostSyncCache`, and `getWriteCacheState = true`.
  - Mount: in `ruan-ram-data-mount.patch`, fall back to `native_verified_block_volume_device(uuid,…)` (ShengUfs, window media only,
    `NativeUfs.cpp:2964`) when the RamBlock lookup fails.
  - The window SHA-256 goes stale after the first write. That is harmless now (it is never checked without
    NATIVE_VERIFIED_BLOCK [V]), but it rules out a verified build later.
- **Pros:** no seed in RAM, no seed-identity coupling. **Cons:** a new write path with no upstream testing, APFS barrier
  semantics are untested, and nothing else in UniXNU uses it.

## 3. `native-ufs-zero-copy` and queue depth

- **What it does** [V]: with `gZeroCopy`, `hostBuildPrds` builds the PRDT from the caller's pages, up to 1 MiB per command
  (`kXferMax`). Without it, each command of at most 128 KiB (`kDataMax`) goes through the slot's data area in the host DMA buffer,
  plus a `memcpy` (`RootCommands.h:452-505`).
  - `hostDmaAddress` only checks the address width taken from SMMU IDR2 (`NativeUfs.cpp:1152-1157`). It does **not** check what
    the firmware's reused mapping covers.
- **Safe on ruan? No** [I, strong]:
  - With `native-ufs-smmu-reuse=1`, stream 0x20 goes through the firmware's context bank (bank 2 on dizi). Gunyah owns the SMMU,
    and the bank's page table covers only what UEFI mapped.
  - dizi: "DMA straight into caller pages powered the tablet off". Host buffers worked only in the first 256 MiB of the window.
  - ruan is the same SoC, hypervisor and reuse path. Expect a fault or power-off as soon as a page falls outside that mapping.
  - Read-only check before trying: `native-ufs-probe=7` dumps the S2CR and bank of the stream. Only if S2CR is bypass, or the bank
    maps all DRAM, is zero-copy=1 worth one RAM-only test boot.
- **Gain if it worked** [I]: small for the root. Window reads always go through a per-window bounce (`RootWindowRead.h`), capped at
  128 KiB per command. Zero-copy only removes the slot→bounce copy, about 15–25 µs per 128 KiB against a command of roughly
  100–200 µs, so ≤10–15 %.
- **Better levers, all safe with zero-copy=0** [I]:
  - **`kBounces` 4→8 or 16** (`NativeUfs.cpp:1951`): this is the real per-window queue depth. The slots are already 16–32
    (`kSlots`, list mode 2). Check `wait n/ms` in `native_ufs_stats_text` first: if bounce waits are non-zero, this is the
    bottleneck.
  - A Data store in direct mode uses NativeRamBlock's own pool of 8 × 128 KiB staging buffers, so Data I/O does not compete with
    root bounces.
  - Interrupt mode is already on (`native_ufs_irq_mode`); check that it reads 1.

## 4. RAM freed

- Today: the whole Data image sits flat in DarwinBoot's 2816 MiB window [V]. That is 1,619,939,328 B (1545 MiB) for rbu85u and
  1,087,066,112 B (~1036 MiB) for rbu131u.
- Plan A resident [I]:
  - about 4.6 MiB of nonzero seed pages,
  - the big-disk index (~20 KiB),
  - the 128 KiB bitmap and the dirty map,
  - the 1 MiB staging pool,
  - the packed seed copy, about 5 MiB. It stays because `ml_static_mfree_prepare` is a stub that returns false in
    `RamBlockNoStore.c`.
  - Total: about 11 MiB.
- **Freed: about 1.0–1.5 GiB**, and Data grows from ~1 GiB to 16 GiB.
- Plan B frees the same and keeps nothing.
