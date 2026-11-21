# UniXNU core changes for ruan

ruan's build needs these changes on top of a UniXNU checkout. They are taken from the working tree that built the
2026-11-21 images (UniXNU superproject `8542980`; submodules Adreno740 `1c9029b`, AdrenoMetal `c9ebd2f`, DarwinBoot `5243c68`).

| Patch | Applies to | What |
|---|---|---|
| `00-unixnu-core.patch` | the UniXNU superproject | Turnip patches for the Adreno 710 (`patches/mesa/turnip-fd710.patch`, `turnip-darwin-open-report.patch`), `build-turnip-darwin.py` (extra CFLAGS, e.g. `-mstrict-align`), the Adreno compiler inputs, build-script and device-tree tool changes, the RAM-disk DXE |
| `10-Adreno740.patch` | `drivers/Adreno740` | `NativeAdrenoAccelerator.cpp` additions |
| `10-AdrenoMetal.patch` | `drivers/AdrenoMetal` | `bridge/patches/0065-ruan-ios-read-pages.patch` |
| `10-DarwinBoot.patch` | `drivers/DarwinBoot` | USB RAM-disk receiver (`UsbRamdisk.h`) and its test |

Apply them to a clean checkout at those commits with `./apply.sh <path-to-UniXNU>`. The rest of ruan's changes (kernel
patches, the Adreno driver patches in `../adreno/`, board drivers in `../../package/`) are applied by the build scripts.
