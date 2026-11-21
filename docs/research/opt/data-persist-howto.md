# Data on UFS (data-persist): how to build, boot, check and roll back

Plan A of `data-on-ufs.md`, as `data-persist.patch` (not applied). Nothing was built, flashed or booted for this.
**[V]** = verified in source, on disk or by a dry run on a scratch copy. **[I]** = inferred, must be tested on the tablet.

## What the patch does

- `build-ramroot.sh` with `UFS=1 PERSIST=1`:
  - It sends no USB image. The Data seed rides in the payload (`--data-ramdisk <seed>`).
  - `make-ufs-board.sh` adds `packed_data_ramdisk = true` to `[payload.adt]`. The ADT then reserves `/chosen/memory-map/PackedRAMDisk`
    instead of `RAMDisk`. **[V]** The payload CLI has no `--packed-data-ramdisk` flag: `payload.py:234` and `devicetree.py:373` read
    the board key, as dizi's board does.
  - The board is generated again whenever PERSIST is switched on or off. It checks the `packed_data_ramdisk` line, even with `REBUILD_KERNEL=`.
  - Boot-args: `ram-data=1 ram-data-uuid=<uuid> data-persist=2 data-fua=1`, plus `data-reset=$PERSIST_RESET` when that is set.
    `ram-data-split` is gone: ruan's NativeRamBlock hook skips it for packed input **[V]** (`build-ram-root-probe.py:142`).
  - The `.desc`/`.split` checks are skipped. The DarwinBoot window check still runs, and the 4.6 MiB seed passes it.
- `make-ufs-data.py` with `PERSIST=1`:
  - The default `DATA_SLACK` is 4194304. It builds the same Data tree with mkapfs, packs it with `packed_ramdisk.pack`, and writes `<seed>.uuid`.
  - **[V]** A scratch run on a copy of `work-full/data` built a 4,206,746-block (16.05 GiB) sparse container. apfsck returned exit 0.
    The packed seed was 4,821,120 B, and the UUID was `F8F27480-6A31-2156-1ACE-A905E4E7CFCF`, the same as today's data-usb.
    mkapfs takes the UUID from the file name, so it is stable.
- **The seed is the store's identity.** It is built once, at `PERSIST_SEED`, which defaults to `/root/unixnu-work/ufs-variant/data-seed.apfs`
  (with `.uuid` beside it). It is copied into each OUT and rebuilt only with `REBUILD_SEED=1`. `REBUILD_ROOT=1`, which `cycle-root.sh`
  always passes, leaves the seed alone.
  - The build refuses a new seed without `PERSIST_RESET`. With a seed that does not match the store, the store is "Kept", the disk is
    not in direct mode, and **Data writes wait forever**. **[V]** `NativeRamBlock.cpp:246,471`.
  - **Back up `data-seed.apfs` and `data-seed.uuid`.** Without that exact seed the store cannot be read.
- Store window: 4,206,876 blocks at LBA 26,066,655–30,273,530 of `unixnu`, below the 1 GiB log reserve.
  - **[V]** Computed with `PersistOverlay.h:46-47` and `NativeUfs.cpp:1600-1603`. The partition ends at LBA 30535674, and blocks are 4 KiB
    (8,388,603 blocks = 32 GiB).
  - The root image may grow to 3,918,559 blocks (14.9 GiB) before ShengUfs refuses the store ("overlaps the root image").
- The UFS kernel already has ShengUfs's store functions, not the weak stubs in `RamBlockNoStore.c`. **[V]** In `ruan_ufs/kernel/kernel.macho`
  of 2026-11-21 05:38, `_native_ufs_store_mode` sits next to `_native_ufs_store_open`/`hostStoreOpen` and calls `PE_parse_boot_argn`.
  No kernel rebuild is needed.
- **[I]** The pinned FD (`SILICIUM_UEFI-log.fd`, 2026-11-20) has a DarwinBoot with `KeepPacked`/PackedRAMDisk. Both the current DarwinBoot
  source and the pre-upstream source have it, but the FD binary was not inspected.

## Apply

The build runs `make-ufs-data.py`/`make-ufs-board.sh` from `$D=/root/unixnu-work/UniXNU-ruan`, which is a copy, so apply the patch in both
places. **[V]** `--dry-run` passes in both.
```
P="/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/docs/research/opt/data-persist.patch"
(cd "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/unixnu-ruan" && patch -p1 < "$P")
(cd /root/unixnu-work/UniXNU-ruan && patch -p1 < "$P")
```
`/root/unixnu-work/boot-local.sh` is outside the repo. Without this change it waits forever for a USB receiver that never comes:
```diff
 fastboot boot $D/ruan-ramroot-usb-$T.img
+[ -f $D/data-seed.apfs ] && { echo "== XNU booting $T (PERSIST: Data seed in the payload, nothing to send)"; exit 0; }
 sleep 5; echo "== waiting for DarwinBoot receiver"; until [ -n "$(fastboot devices)" ]; do sleep 0.5; done
```

## Build and boot

`R=<root tag>` is the out dir of the root image **currently flashed** on sda22.

First boot of a seed (reset):
```
W=/root/unixnu-work; T=rbuNNN; A=$(cat $W/args-current.txt)
RAMROOT=$W/ramroot WORK=$W/ramroot/work-full UFS=1 UFS_ROOT=$W/tr/out-${R}root PERSIST=1 PERSIST_RESET=1 REBUILD_KERNEL= \
  TAG=${T}p OUT=$W/tr/out-${T}p KERNEL_ARGS="$A" "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/unixnu-ruan/scripts/build-ramroot.sh" usb
ln -sfn $W/tr/out-${T}p $W/ramroot/out-${T}p && $W/boot-local.sh ${T}p
```
- `PERSIST_RESET=2` also runs Setup.
- Check that the printed `kernel_args` hold `data-reset=1`.

Later boots: the same command **without `PERSIST_RESET`**, with a new TAG. Never boot the reset image twice: each boot with it wipes Data.

`cycle-root.sh`: add `PERSIST=1` to its second build line. Its `fastboot flash unixnu` writes 5.6 GB, and the store begins 16.05 GB into
the partition. **[I]** This assumes ABL does not erase the partition before flashing. If it does, the next boot hangs on Data writes,
because a blank store is not direct mode; recover with one `PERSIST_RESET=1` boot.

## Check on the tablet

- `ruan-usbdbg.py cmd bootargs` shows `data-persist=2 data-fua=1`, and `data-reset=1` on the first boot only.
- `ruan-usbdbg.py dmesg` has `ram-data: /dev/diskNs1 on /private/var rw: error 0`.
  - It must **not** have `NativeRamBlock: persistent store refused: <why>`. A refusal means a volatile disk, and for this 16 GiB seed,
    Data writes that hang.
  - It must not have `persistent store holds no Data of this seed ... kept` either.
  - **[V]** A successful open logs nothing. The state is in `native_data_persist_state` (3 = fresh, 2 = resumed, 4 = error, 7 = kept),
    which ruan's page and USB commands do not show yet.
- **[I]** Settings > General > About shows about 16–17 GB capacity. iOS counts decimal GB, and the container is 17.2 GB.
- Data survives a reboot:
  1. Note `ruan-usbdbg.py ls /private/var/mobile/Library/Logs/CrashReporter` and that `/private/var/keybags/systembag.kb` exists.
  2. Change one setting (for example wallpaper or brightness).
  3. Boot the non-reset image.
  4. The same files and the setting are still there.
- **[I]** Risk on the second boot: keybagd finds its old systembag.kb under the AKS stand-in. If it fails, one `PERSIST_RESET=2` boot
  gives a fresh Data and Setup.
- **[I]** The ADT still carries `ephemeral-storage=1`, because `persistent_data` is not set, as today. If iOS treats Data as fresh on
  every boot even though files survive, try `persistent_data = true` in `[payload.adt]`. That is untested on ruan.

## Rollback

- Build exactly as today, without `PERSIST=1`. The board drops `packed_data_ramdisk` by itself, and `boot-local.sh` stages `data-usb.apfs` again.
- Nothing on the tablet needs undoing. Without `data-persist` the store is mode 0, and nothing writes it. **[V]** `storeWrite` needs mode 2.
- WRITE(10) is limited to the open window **[V]** (`hostInWindow`), so Android's userdata (sda21) and the root image are never written.
- The store blocks stay on sda22. Booting PERSIST again with the same seed resumes them.
- To discard them, do one `PERSIST_RESET=1` boot. Never `fastboot erase unixnu`: that also erases the root.

## Kernel command line (limit 1000 bytes)

- **[V]** `payload.py:52` fails the build above 1000 bytes. DarwinBoot's buffer is 1024.
- **[V]** The last UFS payload (`ruan_ufs/payload/payload.json`, 2026-11-21 06:52) has a 984-byte line, `ram-data-split=1048576` included.
- With PERSIST, the build drops `ram-data-split=1048576` (−24 bytes) and adds `data-persist=2 data-fua=1` (+26). **[V]** Computed:
  - **987 bytes** on later boots;
  - **exactly 1000** on the reset boot (`data-reset=1`, +13).

  That builds, but with zero headroom: one more argument in `args-current.txt` fails the build.
- To get room back:
  - drop one argument you do not need for the reset boot;
  - later, drop `data-fua=1` (−11) once write-back is trusted.
