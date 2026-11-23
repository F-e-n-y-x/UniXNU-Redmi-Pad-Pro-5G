# UniXNU ruan (draft)

Xiaomi Redmi Pad Pro 5G, `ruan`, Qualcomm SM7435 (Parrot): 4x Cortex-A55 (cpu0-3) + 4x Cortex-A78 (cpu4-7),
no pointer authentication, Adreno 710. OS profile `ipados-17.7.11-21H461` (arm64, no PAC needed).
SoC layer: [vildangil/UniXNU-sm7435-common](https://github.com/vildangil/UniXNU-sm7435-common) cf85220.

Status: configuration, DTS and payload inputs only. Not booted. First milestone: XNU verbose boot on the
UEFI framebuffer with `cpus=1`; no storage, touch or GPU driver is selected yet.

| Path | Contents |
| --- | --- |
| `board.toml` | Device configuration (firmware = ruanPkg of Mu-Silicium) |
| `dts/ruan.dts` | Low DRAM bank, splash framebuffer, CPU clusters; SoC values from `sm7435.dtsi` (checked against ruan's live DT) |
| `configs/ruan_defconfig` | Kernel options |
| `patches/uefi/` | 0001: UniXNU modules + UniXNULib in ruanPkg DSC/FDF; 0002: BootShim image size = shim + FD |
| `patches/xnu/soc.series` | SMP + cluster patches from UniXNU-sm8250-common and sheng-platform-power (vildangil's SoC patch does not apply) |
| `porting.toml` | Porting record |

```sh
cd ../UniXNU
./unixnu defconfig ruan_defconfig -D ../UniXNU-ruan
./unixnu fetch && ./unixnu status
./unixnu build kernel
./unixnu build package
./unixnu build firmware --uefi ../uefi-ruan
./unixnu build payload --unique-chip-id <hex>
./unixnu bootimg --uefi ../uefi-ruan --dtb ../uefi-ruan/Resources/DTBs/ruan.dtb
```

`uefi-ruan` is a copy of the proven Mu-Silicium ruan checkout with the HakiOS XNU-loader hook reverted and the
patch above applied. The boot image is for `fastboot boot` only (RAM, nothing flashed).

## RAM root (route 1: nothing written to the tablet)

The kernel roots on `/dev/md0`: DarwinBoot puts the root image in its window and names it in
`/chosen/memory-map/RAMDisk`, and `rd=md0` makes XNU mount it (and skip the sealed-root check). The IPSW's
Static + SystemOS trust caches go in as the payload trust cache. Two forms (`scripts/build-ramroot.sh`):

| Mode | Root | Transport |
| --- | --- | --- |
| `mini` | 4 MiB APFS: `/sbin/launchd`, `/usr/lib/dyld` | inside the payload FAT (XNUP at 0xF4000000) |
| `usb` | trimmed OS + SystemOS cryptex (dyld cache without `.symbols`), zlib-compressed | stock `fastboot stage` to DarwinBoot's USB receiver (`UsbRamdisk.h`), straight into the window |

`scripts/make-ramroot.py` builds the images (HakiOS's APFS writer and selection, 7-Zip); `scripts/zima-ramroot.sh`
boots them from the USB host. Root images are Apple's files: keep them out of git (`/root/unixnu-work/ramroot`).
Boot stages on screen: `KERNEL_ARGS=boot-progress=1` draws `package/BootProgress.c`'s page into the GOP framebuffer
(milestone boxes KERNEL ... SPRINGBRD, kernel stage history, every exec'd process, console lines). `-v` alone gives the
plain text console (`patches/xnu/ruan-boot-progress.patch`).

`board.toml` `owned_memory_size` (2.75 GiB) must be one free run of memory; DarwinBoot names the largest when it is not.
