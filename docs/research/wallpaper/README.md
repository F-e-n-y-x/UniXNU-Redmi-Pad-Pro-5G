# Lock-screen wallpaper (poster) fixes for ruan

Three independent pieces. Each one addresses a different cause of "no lock-screen poster". Nothing here has been built or run on the tablet.
[V] means verified on this host, [I] means inferred.

| File | What it does |
|---|---|
| `make-wallpaper-png.py` | Builds the HEIC to PNG root overlay for the default wallpaper |
| `ruan-owner-dataprotect.patch` | XNU: emulates data-protection classes behind `owner-dataprotect=1` |
| `adrenometal-no-disk-cache.patch` | AdrenoMetal plugin: no Mesa disk cache outside backboardd |

## 1. HEIC to PNG wallpaper overlay

Findings:
- ruan uses the OS profile's iPad7,1/J120 identity. See UniXNU-ruan `board.toml:180-182`, which overrides only `artwork-display-gamut`, and UniXNU `docs/guide/mobilegestalt.md:16-17,52`. [V for the profile, I that PosterBoard picks by it]
- The default wallpaper is 7570 (`Library/Wallpaper/DefaultWallpapers~ipad.plist`). Of its variants in `iPadOS_17~ipad.wallpaperCollection/Wallpapers/` [V]:
  - `7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/Wallpaper.plist` lists `productTypes = [iPad7,1, iPad7,2]`. This is the variant ruan gets.
  - The layered `1024w` variant lists `unsupportedProductTypes = [iPad7,1, iPad7,2]`.
- So the bundle to convert is `.../7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca`. This is the same bundle UniXNU's default profile converts. Its `main.caml` references `assets/J120.HEIC` (13 times) and `assets/J120-Dark.HEIC` (12 times). Both images are 3416x3416. [V]
- The only other Flat bundle for iPad7,1 is Lake (`7560.Lake_Flat...`, `J99-Flat.HEIC`). To convert it too, pass its path as an extra argument.
- The root has no `Library/Wallpaper/Stills`, so LegacyPoster has no still images to show. [V]
- **The work-full root has no CollectionsPoster.appex.** make-ramroot.py:174-177 drops it with `SKIP_PREFIX`, so the overlay has no effect until that line goes. [V]

Tools:
- ImageMagick 7.x with libheif 1.23.6 (`magick -list format`: HEIC rw+) is installed. [V]
- pillow-heif is not installed and not needed.

Proof run [V] (`make-wallpaper-png.py /root/unixnu-work/ramroot/work-full/root /tmp/wallpaper-overlay`, 6 s):

```
   4919967  2732x2732 Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca/J120-Dark.png
   5127792  2732x2732 .../J120.png
    105940            .../main.caml
```

- Only the 25 `relativePath`/`src` pairs changed (`"/assets/J120.HEIC"` became `"/J120.png"`, and the same for Dark).
- The one remaining "HEIC" string is a layer's `name=` attribute, which UniXNU also leaves.
- The PNGs are sRGB 8-bit with alpha. I viewed a downscaled copy and it is the expected colour disc.
- They are scaled to 2732 px, matching UniXNU. The layer stretches the contents to its 3416 bounds.

Hook into make-ramroot.py. `--overlay` already exists (make-ramroot.py:329 `overlay()`, :541 argument, :580-581 merge) but takes a single directory, and build-ramroot.sh:103 already uses it for the Metal plugin. Two ways to add the wallpaper:

- Proposed one-liner in make-ramroot.py, under `if args.full:` after line 581:
  ```python
  if os.environ.get('RUAN_WALLPAPER_OVERLAY'): overlay(tree, manifest, Path(os.environ['RUAN_WALLPAPER_OVERLAY']))
  ```
- Or turn `--overlay` into `action='append', default=[]` and loop over it at :581 and :625.

In either case, also delete make-ramroot.py:177, the CollectionsPoster `SKIP_PREFIX` line. Otherwise there is no poster to use the PNGs.

## 2. owner-dataprotect=1 (`ruan-owner-dataprotect.patch`)

Source: UniXNU-sheng `patches/xnu-12377/zzzz-sheng-ipados27-bringup.patch:533-616` (kpi_vfs.c), ported to xnu-10063. [V]

How it works:
- On a writable volume without `MNT_CPROTECT`, `VNOP_SETATTR` stores `va_dataprotect_class` in the xattr `com.unixnu.dataprotect-class` instead of passing it to APFS.
- `VNOP_GETATTR` reports the stored class, or class D when the file has none.

Two additions beyond sheng's version (my design, [I]):
- `VNOP_CREATE` strips the class that `open_dprotected_np(O_CREAT, class)` passes before APFS sees it, and stores it on the new file. That way a create-time class reads back.
- The clamp in `openat_dprotected_internal` (vfs_syscalls.c, the `O_CREAT` block at about 5034-5043): with the boot-arg set, a class outside A..CX becomes D instead of being stored. The task's "clamp" had no precise spec anywhere I could find, so this is my reading of it.

Gating: `native_dataprotect_enabled()` in kpi_vfs.c (not static) reads `owner-dataprotect=1` once. Nothing changes without the boot-arg.

Verification:
- `patch -p1 --dry-run` applies cleanly in `/root/unixnu-work/ufs-variant/out/ruan/ipados-17.7.11-21H461/ruan_ufs/tree/.cache/xnu-work`, against the current, already-patched tree. [V]
- All APIs it uses exist in this tree: `vn_get/setxattr` (vnode_internal.h:518-519), `XATTR_NOSECURITY`, `MNT_CPROTECT`, `vfs_isrdonly`, `PROTECTION_CLASS_*`, and `pexpert.h`, which vfs_syscalls.c already includes. [V]
- **Not compiled.** No kernel builds were allowed.

To apply:
1. Copy the patch to `UniXNU-ruan/patches/xnu/`.
2. Append it to `series` (it touches only bsd/vfs, so it can go after the existing patches).
3. Add `owner-dataprotect=1` to `board.toml` `kernel_args`.

Per UniXNU `docs/devices/asteroids-filipnavara.md:244`, a Data volume whose PosterBoard store was made before the change keeps no wallpaper until it is reset. On ruan, reset PosterBoard's container data or seed a fresh Data container. [I for the exact path]

## 3. Mesa disk-cache flock (`adrenometal-no-disk-cache.patch`)

Findings [V]:
- Turnip creates its cache in `tu_device.cc:1875` (`disk_cache_create`).
- This Mesa (`UniXNU/.cache/mesa-work`, the source of `build/mesa-tu-ios`, which has `shader-cache=enabled`) uses the **multi-file** cache by default (`disk_cache.c:226-237`).
- Its writer takes `flock(fd, LOCK_EX|LOCK_NB)` on the temporary file while writing (`disk_cache_os.c:807`).
- The database variant blocks in `mesa_db_flock` (`mesa_cache_db.c:100-108`), but it is only used with `MESA_DISK_CACHE_DATABASE`.
- `disk_cache_enabled()` (`disk_cache_os.c:1003-1040`) returns false when `MESA_SHADER_CACHE_DISABLE` is true.

The fix:
- In `NativeAdrenoMetalPlugin.mm` `deviceForPort()`, the iOS branch (`#else`, line 461) is where every non-render-server process goes to `clientDevice()`. That function is what loads Turnip (`native_bridge_metal_open`, :234/:257/:268).
- The patch calls `setenv("MESA_SHADER_CACHE_DISABLE", "true", 0)` just before that, so backboardd keeps its cache.
- It applies cleanly with `patch -p1 --dry-run` in `UniXNU/drivers/AdrenoMetal`. [V]
- The plugin's own shader-cache files also use flock, but only non-blocking and only briefly (`ShaderCache/ShaderCacheState.h:24`, `ShaderCacheWorker.mm:55`). I did not change them. [I that they are harmless]

To rebuild:
1. `cd /root/unixnu-work/UniXNU/drivers/AdrenoMetal && patch -p1 < .../adrenometal-no-disk-cache.patch`
2. `python3 /root/unixnu-work/UniXNU-ruan/scripts/build-metal-plugin.py [OUT]`. It compiles `drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm`, line 102, and the output defaults to `UniXNU/build/ruan-metal-plugin`.
3. Rebuild the root with `build-ramroot.sh` using `OVERLAY=<that OUT>`, so the new cdhashes go into trust.bundle.

## Test order

Test one change per boot, in this order:
1. Kernel GPU deny for posters (ruan-grant-provider.patch, tested separately) plus the **wallpaper overlay** plus CollectionsPoster restored. A poster that renders in software decodes the PNG. The pass condition: no ns 7 code 3 kill and no SpringBoard trap on touch.
2. Add **owner-dataprotect=1** with a fresh Data container. Check that PosterBoard no longer says the store is not viable, and that the poster survives a reboot.
3. **Plugin no-disk-cache**, then `ruan-poster-gpu=1` (posters get the GPU again). The pass condition: no 0xDEAD10CC. Also watch widget and app first-launch times, since they now compile shaders every launch.

## Risks

- Overlay: about +10 MB in the root. Restoring CollectionsPoster brings back the old ns 7 code 3 kill if the cause was not HEIC or the GPU.
- Dataprotect: only reports classes, never encrypts. Daemons that refuse class A/B/C without encryption will now see them "set". Using `XATTR_NOSECURITY` from `VNOP_CREATE` and `VNOP_SETATTR` is untested on xnu-10063 APFS, so a lock-order problem there is possible. [I]
- Plugin: with no disk cache in clients, every process compiles shaders on each launch (CPU and time). A poster can still be killed at launch if compiling is slow, and memory pressure goes up.
- None of the three has been compiled or run on the device.
