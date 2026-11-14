# UniXNU: iPadOS on Redmi Pad Pro 5G

UniXNU port of iPadOS 17.7.11 (21H461) to the Xiaomi Redmi Pad Pro 5G / POCO Pad 5G ("ruan", SM7435), booted RAM-only
with `fastboot boot` (nothing is flashed). Status: boots to the **iPadOS 17 lock screen** (wallpaper, clock, status bar). Touch, boot speed and the GPU are next.

![Lock screen on ruan](docs/img/lockscreen-rb13.jpg)

- [docs/IPADOS-BRINGUP.md](docs/IPADOS-BRINGUP.md): every blocker, error text, cause and fix, in order. Start here.
- [unixnu-ruan/](unixnu-ruan/): the board package (board.toml, XNU patches, kernel-side packages, build scripts, DTS).
- [docs/research/](docs/research/): GPU (offline and Adreno 710), display and method plans.

Built on [UniXNU](https://github.com/UniXNU) and its device ports (amethyst, fuxi, Dizi-Ruan, gts7xlwifi, j716f, nabu),
whose notes this work leans on. No Apple binaries, IPSW contents or vendor firmware are included: bring your own IPSW.
