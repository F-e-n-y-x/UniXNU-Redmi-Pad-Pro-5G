# Install and setup: iPadOS on the Redmi Pad Pro 5G

Back to the [README](../README.md).

> [!CAUTION]
> **Experimental research software. Use it entirely at your own risk.** There is no warranty and no support.
> - It can brick your tablet or leave it unbootable. Recovery may need Xiaomi's flashing tools or a service centre.
> - Unlocking the bootloader and repartitioning void your warranty.
> - **Step 1 wipes all Android user data** (apps, photos, accounts). Back up first.
> - You need **your own, legally obtained iPadOS 17.7.11 IPSW** and an iOS SDK you are allowed to use. This project
>   distributes **no Apple software and no Qualcomm or Xiaomi firmware**. Everything proprietary is built on your PC, from
>   your own copies and your own tablet's stock firmware, and must not be redistributed.
> - Only the Redmi Pad Pro 5G (`ruan`, SM7435) is supported. The numbers below are for the 8 GB / 256 GB model; read your own.

Only `userdata` (shrunk) and the new `unixnu` partition change. The kernel is RAM-booted with `fastboot boot` every time, so
Android's boot partitions are never written and Android still boots normally.

### 0. Requirements

Everything Apple, Qualcomm or Xiaomi comes from **you**: your IPSW, your SDK and **your own tablet's** firmware. The repo
ships only the port's own code and the scripts that turn your files into what the build needs.

- **Tablet:** unlocked bootloader; root (Magisk) for the partitioning step and for pulling firmware; a good USB cable.
- **Linux build host, x86-64**, with: `git`, `python3` (3.11+), `cc`, `rsync`, `7z` (p7zip), `mkfs.vfat` (dosfstools),
  `mcopy` (mtools), `sgdisk` (gdisk), `losetup`, `sha256sum`, `fdtput` (device-tree-compiler), `gh` and `curl`, `uv`,
  `fastboot` and `adb` (android-tools). The GPU stack also needs `clang`, `lld`, `llvm-strip`, `rustup` with the
  `aarch64-apple-ios` target, and theos `ldid` (`LDID=` if it is not at `/root/theos/toolchain/linux/iphone/bin/ldid`).
- **An iOS 17.5 SDK** for the Metal plugin (`UNIXNU_SDKROOT=`), obtained by you.
- **Disk space:** about 60 GB free on a fast disk (SSD, Linux file system; not NTFS/exFAT). The IPSW cache is about 8 GB, the
  root work tree about 4 GB, and a root output directory up to 16 GB.

**What comes from where.** Use exactly these; nothing else in the repo is needed to build iPadOS.

| You need | Where it comes from | How |
|---|---|---|
| This repo | here, branch `main` | `git clone` (step 2a) |
| UniXNU at `8542980` + submodules | [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU), public | step 2b, then `unixnu-ruan/patches/unixnu-core/apply.sh` |
| Mu-Silicium at `e51cd877` | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium), public | `uefi/setup-uefi-ruan.sh` (step 2d) |
| HakiOS APFS tools | this repo, `third_party/hakios-tools/` | `third_party/hakios-tools/build.sh` once |
| iPadOS 17.7.11 IPSW (21H461) | Apple, fetched by UniXNU's `./unixnu fetch` | step 2b |
| Firmware files (GPU, Wi-Fi, video, touch) | your tablet's `/vendor/firmware` | `adb pull` (step 2c) |
| UEFI drivers `uefi/Binaries/ruan` | your tablet's `uefi_a` partition | `tools/regen-binaries.sh` (step 2c) |
| UEFI device tree `uefi/Resources/DTBs/ruan.dtb` | your tablet's live device tree | `tools/make-uefi-dtb.sh` (step 2c) |
| Touch firmware header | `novatek_ts_fw_boe.bin` from `/vendor/firmware` | `tools/make-touch-fw.py` (step 2c) |

The `macos` branch, `docs/research/` and `tools/host/` are not part of the iPadOS build: ignore them when you start.

### 1. Partition the UFS (wipes Android data)

The partition table of UFS LUN 0 (`/dev/block/sda`, **4096-byte sectors**) changes like this, on our 256 GB tablet:

| # | Name | Before | After |
|---|---|---|---|
| 21 | `userdata` | the rest of the disk | `3514920`–`22147071` (71 GiB), **same type and unique GUID** |
| 22 | `unixnu` | (none) | `22147072`–`30535674` (32 GiB): the iPadOS root and the persistent Data store |

The root image needs at least 6 GiB and the persistent Data store starts about 16 GB into `unixnu`, so use 32 GiB. Android
finds `userdata` by name and GUID, which is why both stay the same.

**a. Back up both GPT copies** (rooted Android over `adb`). Write down the disk size, and keep these files safe: they are your
only way back.

```bash
adb shell su -c 'cat /sys/block/sda/queue/logical_block_size'      # must print 4096
adb shell su -c 'blockdev --getsize64 /dev/block/sda'             # BYTES. N = BYTES / 4096; type the number for $((N-6)) below
adb shell su -c 'dd if=/dev/block/sda of=/sdcard/sda-primary.orig.bin bs=4096 count=6'
adb shell su -c 'dd if=/dev/block/sda of=/sdcard/sda-backup-tail.orig.bin bs=4096 skip=$((N-6)) count=6'   # put your N in
adb pull /sdcard/sda-primary.orig.bin; adb pull /sdcard/sda-backup-tail.orig.bin
```

**b. Edit the table on the PC**, on a sparse copy of the disk that holds only the two GPTs:

```bash
truncate -s <BYTES> disk.img
dd if=sda-primary.orig.bin of=disk.img conv=notrunc
dd if=sda-backup-tail.orig.bin of=disk.img bs=4096 seek=$((N-6)) conv=notrunc
L=$(sudo losetup --sector-size 4096 -f --show disk.img)
sudo sgdisk --backup=sda-gpt-sgdisk.bak $L      # a third backup, in sgdisk's format
sudo sgdisk -p $L; sudo sgdisk -i 21 $L         # check: entry 21 is userdata; note its first LBA, type GUID and unique GUID
```

Pick `unixnu`'s first LBA `S` = last usable LBA − 0x800000 + 1, rounded **up** to a multiple of 4096 sectors. On a 256 GB
tablet that gives `S = 22147072` and a last usable LBA of `30535674`. Then:

```bash
sudo sgdisk -d 21 -n 21:<userdata first LBA>:$((S-1)) -t 21:<type GUID> -u 21:<unique GUID> -c 21:userdata \
            -n 22:$S:<last usable LBA> -c 22:unixnu $L
sudo sgdisk -v $L; sudo sgdisk -p $L
dd if=disk.img of=sda-primary.new.bin bs=4096 count=6
dd if=disk.img of=sda-backup-tail.new.bin bs=4096 skip=$((N-6)) count=6
sudo losetup -d $L
```

**c. Write the new table**, then read it back and compare checksums before rebooting:

```bash
adb push sda-primary.new.bin sda-backup-tail.new.bin /sdcard/
adb shell su -c 'dd if=/sdcard/sda-primary.new.bin of=/dev/block/sda bs=4096 count=6 conv=fsync'
adb shell su -c 'dd if=/sdcard/sda-backup-tail.new.bin of=/dev/block/sda bs=4096 seek=$((N-6)) count=6 conv=fsync'
adb shell su -c 'dd if=/dev/block/sda bs=4096 count=6 | sha256sum'           # must match sha256sum sda-primary.new.bin
```

**d. Reformat Android's data:** reboot to recovery and wipe / format Data. Android's old file system is larger than the
shrunk partition and will not mount until it is recreated. Android then starts like new.

### 2. One-time setup

All paths below hang off one work folder, `<work>`. The scripts read it from **`UNIXNU_WORK`** (default `/root/unixnu-work`).
Pick a folder **without spaces** on a fast Linux disk and export it in every shell you build from:

```bash
export UNIXNU_WORK=/home/you/unixnu-work      # your choice; <work> below means this folder
mkdir -p $UNIXNU_WORK/ramroot $UNIXNU_WORK/firmware
```

When this step is done, `<work>` looks like this:

| Path | What |
|---|---|
| `<work>/repo` | this repository |
| `<work>/UniXNU-ruan` | **symlink** to `<work>/repo/unixnu-ruan` (not a copy, so the scripts can find `uefi/` and `third_party/`) |
| `<work>/UniXNU` | UniXNU at `8542980`, core patches applied |
| `<work>/uefi-ruan` | Mu-Silicium at `e51cd877` with ruan's package |
| `<work>/firmware/` | your tablet's `/vendor/firmware` |
| `<work>/ramroot/SILICIUM_UEFI-log.fd` | the UEFI firmware you build in 2d |
| `<work>/unique-chip-id.txt` | any hex value, e.g. `0x1234abcd`: the device tree's platform seed |

**a. This repo**

```bash
cd $UNIXNU_WORK
git clone https://github.com/F-e-n-y-x/UniXNU-Redmi-Pad-Pro-5G repo
ln -s $UNIXNU_WORK/repo/unixnu-ruan $UNIXNU_WORK/UniXNU-ruan
echo 0x1234abcd > $UNIXNU_WORK/unique-chip-id.txt
repo/third_party/hakios-tools/build.sh
```

**b. UniXNU**, pinned, with ruan's core patches. Install UniXNU's Linux toolchain to `/opt/unixnu-linux` first (UniXNU's
`docs/building.md`).

```bash
cd $UNIXNU_WORK
git clone https://github.com/UniXNU/UniXNU && cd UniXNU
git checkout 8542980 && git submodule update --init --recursive
$UNIXNU_WORK/UniXNU-ruan/patches/unixnu-core/apply.sh $UNIXNU_WORK/UniXNU    # must print "applied" four times
export PATH=/opt/unixnu-linux/bin:$PATH
./unixnu defconfig ruan_defconfig -D ../UniXNU-ruan
./unixnu fetch                                                                # downloads the 21H461 IPSW (~8 GB)
```

**c. Files from your tablet** (rooted Android, `adb` connected). These stay on your PC; never commit or share them.

```bash
cd $UNIXNU_WORK
adb shell su -c 'cp -r /vendor/firmware /sdcard/fw' && adb pull /sdcard/fw/. firmware/ && adb shell rm -r /sdcard/fw
adb shell su -c 'cat /sys/firmware/fdt' > ruan-live.dtb
adb shell su -c 'dd if=/dev/block/by-name/uefi_a of=/sdcard/uefi_a.img' && adb pull /sdcard/uefi_a.img && adb shell rm /sdcard/uefi_a.img
repo/tools/make-touch-fw.py firmware           # -> unixnu-ruan/.cache/hakios-fw/ (touch firmware header)
repo/tools/make-uefi-dtb.sh ruan-live.dtb      # -> uefi/Resources/DTBs/ruan.dtb
repo/tools/regen-binaries.sh uefi_a.img        # -> uefi/Binaries/ruan (needs gh logged in, curl, uv); must end without MISSING lines
```

**d. The UEFI firmware**

```bash
repo/uefi/setup-uefi-ruan.sh                   # clones Mu-Silicium e51cd877 to <work>/uefi-ruan and adds ruan's package
cd $UNIXNU_WORK/UniXNU && ./unixnu build firmware --uefi ../uefi-ruan
cp ../uefi-ruan/Build/ruanPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd $UNIXNU_WORK/ramroot/SILICIUM_UEFI-log.fd
```

Rebuild the firmware whenever you update the repo's `uefi/` or the DarwinBoot core patch. An old FD makes the tablet refuse
the root ("size differs from the descriptor").

### 3. Build

All commands run from `<work>/UniXNU-ruan`, with `UNIXNU_WORK` exported. `<t>` is any build tag you choose. Every output lands in `OUT`.

**a. GPU userland overlay** (Turnip, AdrenoMetal and the smoke tool, signed ad hoc, with their CDHashes for the trust bundle):

```bash
UNIXNU_SDKROOT=<iOS SDK> python3 scripts/build-turnip-smoke.py      # the 17.5 SDK works; its default path is /root/theos/sdks/iPhoneOS16.5.sdk
UNIXNU_SDKROOT=<iOS 17.5 SDK> python3 scripts/build-metal-plugin.py <work>/overlay
# optional, Wi-Fi joining (ruan-wifi=5): python3 scripts/build-wifi-userspace.py <out> --into <work>/overlay
```

**b. The root.** This builds the full OS + SystemOS root from the IPSW, with the overlay and a Data tree. The same run also
produces a RAM kernel and payload you don't need. Then pack it into the `unixnu` partition image:

```bash
RUAN_FULL_ROOT=1 OVERLAY=<work>/overlay DATA_CONTAINER=1 REBUILD_ROOT=1 TAG=<t>root OUT=<work>/out-<t>root \
  KERNEL_ARGS="boot-progress=1" scripts/build-ramroot.sh usb
cd <work>/out-<t>root
python3 <work>/UniXNU-ruan/scripts/make-ufs-root.py root-usb.apfs $(cat root-usb.split) \
  <work>/UniXNU/out/ipsw/21H461/044-76034-038.dmg unixnu-root.img
cd -
```

`make-ufs-root.py` prints the root volume UUID and the image size. The image is raw; `fastboot` sparses it itself.

**c. Kernel, Data seed and boot image.** `UFS=1` generates the UFS board (`make-ufs-board.sh`: ShengUfs driver,
`patches/xnu/ufs-own-root.series`) and builds the kernel. `PERSIST=1` packs a 16 GiB Data seed into the payload, whose changed
pages are stored at the end of `unixnu`. For the **first** build of a seed add `PERSIST_RESET=1` (or `2` to run Setup), and
never boot that image twice: every boot of it wipes Data.

```bash
UFS=1 PERSIST=1 PERSIST_RESET=1 UFS_ROOT=<work>/out-<t>root REBUILD_ROOT=1 TAG=<t>u OUT=<work>/out-<t>u \
  KERNEL_ARGS="boot-progress=1 smp-gic=1 cpus=8 smp=1 launchd_disable_all_timeouts=1 armcpu-edge=1 gpu-fence-wake=0 gpu-prime=0 gpu-prime-system=0" \
  scripts/build-ramroot.sh usb
```

Later builds use the same command without `PERSIST_RESET` and with a new tag. Add `REBUILD_KERNEL=` (empty) to change only
boot arguments: that rebuilds just the boot image. Check the printed `kernel_args` before you boot.

What the boot image is: UniXNU's payload (kernel, device tree, trust caches, Data seed) is repacked as a **58 MiB FAT inside
an `XNUP` container**, behind a BootShim and the UEFI FD, in an Android boot image (`ruan-ramroot-usb-<t>u.img`). The stock
bootloader (ABL) accepts about 62 MB for `fastboot boot`, so the FAT stays at 58 MiB (60.8 MB). The build stops if the root
does not fit DarwinBoot's 2.75 GiB memory window.

#### Boot arguments

UniXNU caps the kernel command line at **1000 bytes**, and the build adds about 490 of them (UFS, Data and board arguments).
ruan's fixed settings therefore sit in the kernel as **built-in defaults** (`patches/xnu/ruan-default-bootargs.patch`). The
command line always wins over them, but userland (launchd) only sees the real command line. Useful knobs:

| Argument | Default | Meaning |
|---|---|---|
| `dpu-scanout=2` | built in | Zero-copy scanout: the display engine reads the GPU's frames. `0` falls back to CPU copies |
| `ruan-bl-svc=1` | on | The AppleARMBacklight-conforming backlight service. `0` is the old path, which brings back the 5 s frame stall |
| `ruan-bl-mirror=1` | on | Also drives the second backlight chip (the other half of the LED strings). `0` leaves it at the boot level |
| `ruan-120hz=1` / `=2` | off | `1` switches the panel to 120 Hz (checked, reverted if the measured rate is wrong); `2` only reads and checks |
| `ruan-wifi=0..5` | off | Wi-Fi stages: `3` firmware ready (radio off), `4` radio on + self-test scan, `5` also brings up `en0` (needs the step 3a Wi-Fi overlay and `no-lc=1`) |
| `no-lc=1` | off | Skips launch constraints, so the overlay's ad-hoc binaries can start |
| `ruan-gpu=18`, `ruan-gpu-perf`, `ruan-gpu-dcvs`, `ruan-gpu-irq` | built in | Adreno power-up, clock levels, governor, interrupts. `ruan-gpu-max-level=N` lowers the clock ceiling (default 9 = 940 MHz) |
| `gpu-prime=0`, `gpu-prime-system=0` | | Turn off background shader-cache priming |
| `boot-progress=1` | | The on-screen boot page and banner |

Never pass `cs_enforcement_disable=1` (the kernel panics) or `-v` (backboardd never presents).

### 4. Flash the root and boot

Put the tablet in fastboot (Power + Volume down). This is the **only** flash, and only to `unixnu`. Never flash anything to
`userdata`, `boot` or any other partition.

```bash
fastboot flash unixnu <work>/out-<t>root/unixnu-root.img      # 6-10 GB, takes a while
fastboot boot <work>/out-<t>u/ruan-ramroot-usb-<t>u.img        # RAM only
```

With `PERSIST=1` nothing more is sent. (Builds without `PERSIST` want their Data over USB once DarwinBoot's receiver shows
up: `fastboot stage <out>/data-usb.apfs` then `fastboot continue`.) Re-flash `unixnu` only when the root changes. Kernel or
argument changes need only a new `fastboot boot`.

### 5. First boot

- The boot page (`boot-progress=1`) steps through KERNEL … SPRINGBRD. Expect the lock screen after a few minutes. The first
  boot of a fresh Data is the slowest, because every shader compiles cold.
- A few boots reset by themselves after launchd. Just boot again.
- The kernel's USB debug port enumerates as a serial device (`1209:0001`, `/dev/ttyACM*`). Use `tools/ruan-usbdbg.py`
  (Python standard library only; one client at a time):

| Command | What it shows |
|---|---|
| `cmd info` | Uptime and status lines, plus the saved log of the previous boot |
| `cmd bootargs` | The command line the kernel got |
| `cmd display` | Frame pacing (`PACING`, `SUBMIT-MS`) and the display state |
| `cmd backlight` / `cmd backlight N` | Backlight chip state; set the brightness (16–4095) |
| `cmd wifi` | Wi-Fi stage and status |
| `cmd panel120` | 120 Hz switch result (`0x120` switched, `0x60` dry run ok, negative = refused step) |
| `cmd cpufreq` | CPU cluster frequencies and governor |
| `shot -o screen.png` | Screenshot of the panel |
| `dmesg -o dmesg.txt` | The whole kernel log |
| `pull-crashes -d crashes` | Every iOS crash report (`.ips`) |
| `reboot-fastboot` / `reboot` | Back to fastboot / a normal (Android) boot |

### 6. Back to Android, and undo

- **Back to Android:** `ruan-usbdbg.py reboot`, or force a restart. Android boots normally, because
  nothing it boots from was changed. Run `ruan-usbdbg.py reboot-fastboot` to get to fastboot for the next iPadOS boot.
- **Undo the partitioning:** with rooted Android (or recovery), write the original tables back, then format Data again:

```bash
adb push sda-primary.orig.bin sda-backup-tail.orig.bin /sdcard/
adb shell su -c 'dd if=/sdcard/sda-primary.orig.bin of=/dev/block/sda bs=4096 count=6 conv=fsync'
adb shell su -c 'dd if=/sdcard/sda-backup-tail.orig.bin of=/dev/block/sda bs=4096 seek=$((N-6)) count=6 conv=fsync'
# alternative from a recovery with sgdisk: sgdisk --load-backup=sda-gpt-sgdisk.bak /dev/block/sda
```

Reboot to recovery and format Data. `userdata` then spans the whole disk again, and the iPadOS root is gone.

### 7. Known issues

See [What works](../README.md#what-works). In short: SpringBoard sometimes resprings; Turnip has an alignment fault (SIGBUS) in some GPU
writes; closing apps can take over 5 s; widgets and app snapshots draw as blank boxes; 120 Hz and Wi-Fi are still being
tested; audio, cameras, sensors, cellular and Bluetooth do not work. A device left idle must not sleep (`noidle=1` is built in).

### 8. Common build errors

| Error | Cause | Fix |
|---|---|---|
| `hakios.novatek_ruan_fw.h: No such file` (or `focaltech_garnet_fw.h`) | touch firmware header not generated | step 2c, `tools/make-touch-fw.py <work>/firmware` |
| `UEFI FD ... missing` | no firmware built | step 2d |
| `.../uefi/mkxnuimg-ruan.py missing` | `<work>/UniXNU-ruan` is a copy, not a symlink | `rm -r` it and make the symlink (step 2a), or set `MKXNUIMG=<repo>/uefi/mkxnuimg-ruan.py` |
| `missing .../mkapfs.py (set HAKIOS)` or `unzbm` | HakiOS tools not built | `third_party/hakios-tools/build.sh` |
| `apply.sh`: `patch does not apply` | UniXNU not at `8542980`, submodules not updated, or patches already applied | fresh clone, step 2b exactly |
| `No module named unixnu` / paths under `/root/unixnu-work` | `UNIXNU_WORK` not exported in this shell | `export UNIXNU_WORK=<work>` |
| `a710_zap.mbn` / `gmu_gen70000.bin` not found | firmware not pulled | step 2c (`FIRMWARE_DIR=` points elsewhere if you keep it elsewhere) |
| `Binaries/ruan missing` / `ruan.dtb missing` | step 2c skipped | step 2c, then `uefi/setup-uefi-ruan.sh` again |
| edk2 build errors about paths | the work folder has a space in its path | use a path without spaces |
| `kernel_args` over 1000 bytes | too many boot arguments | drop arguments; most are built-in defaults already |
| tablet shows "size differs from the descriptor" | the FD is older than the root format | redo step 2d |

Still stuck? Open an issue with the full command, the last 30 lines of output and the commit you built (`git -C <work>/repo rev-parse --short HEAD`).
