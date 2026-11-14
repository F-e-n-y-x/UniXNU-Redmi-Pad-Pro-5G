# UniXNU: iPadOS on Redmi Pad Pro 5G

UniXNU port of iPadOS 17.7.11 (21H461) to the Xiaomi Redmi Pad Pro 5G / POCO Pad 5G ("ruan", SM7435), booted RAM-only
with `fastboot boot` (nothing is flashed). Status: boots to the **iPadOS 17 home screen** with working touch (Settings opens). About 1 fps without a GPU; icons, buttons and speed are next.

<p>
  <img src="docs/img/lockscreen-rb13.jpg" alt="Lock screen on ruan" width="45%">
  <img src="docs/img/settings-about-rb14f.jpg" alt="Settings > About: iPadOS 17.7.11 on ruan" width="45%">
</p>

- [docs/IPADOS-BRINGUP.md](docs/IPADOS-BRINGUP.md): every blocker, error text, cause and fix, in order. Start here.
- [unixnu-ruan/](unixnu-ruan/): the board package (board.toml, XNU patches, kernel-side packages, build scripts, DTS).
- [docs/research/](docs/research/): GPU (offline and Adreno 710), display and method plans.

Built on [UniXNU](https://github.com/UniXNU) and its device ports (amethyst, fuxi, Dizi-Ruan, gts7xlwifi, j716f, nabu),
whose notes this work leans on. No Apple binaries, IPSW contents or vendor firmware are included: bring your own IPSW.
