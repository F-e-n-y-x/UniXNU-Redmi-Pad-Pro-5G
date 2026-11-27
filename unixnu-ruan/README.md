# UniXNU ruan: the iPadOS device port

Xiaomi Redmi Pad Pro 5G, `ruan`, Qualcomm SM7435 (Parrot): 4x Cortex-A55 + 4x Cortex-A78, no pointer authentication,
Adreno 710. OS profile `ipados-17.7.11-21H461`. SoC layer: [vildangil/UniXNU-sm7435-common](https://github.com/vildangil/UniXNU-sm7435-common) `cf85220`.

**To build and install, follow [docs/INSTALL.md](../docs/INSTALL.md).** This page only describes the folder.

| Path | Contents |
| --- | --- |
| `board.toml` | Device configuration for UniXNU (firmware = Mu-Silicium `ruanPkg`, from the repo's `uefi/`) |
| `dts/ruan.dts` | Low DRAM bank, splash framebuffer, CPU clusters |
| `configs/` | Kernel options |
| `package/` | ruan's drivers (touch, buttons, battery, backlight, display, GPU glue, Wi-Fi, ...) |
| `patches/xnu/` | Kernel patches (the `*.series` files list what each build applies) |
| `patches/adreno/`, `patches/drivers/`, `patches/mesa/`, `patches/smoke/`, `patches/unixnu/` | Applied by the build scripts |
| `patches/unixnu-core/` | Changes to the UniXNU checkout itself: apply once with `apply.sh` (INSTALL step 0) |
| `scripts/` | The build (`build-ramroot.sh` is the entry point) |
| `userland/` | Small user-space shims built into the root |
| `porting.toml`, `unixnu.lock`, `feeds.toml` | Porting record and pinned source commits |

Generated, never in git: `.cache/` (touch firmware header, fetched Wi-Fi/Bluetooth/video sources).
