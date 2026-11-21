# Userland stability: UniXNU vs ruan (2026-11-21)

From a read-only code-explorer pass. [V] means verified, [I] means inferred.

## How UniXNU keeps userland stable

- **Disabled daemons:** a boot-arg-driven disabled.plist (UniXNU-sheng BootWatchdogPolicy.c:28-44; `lite=1|2`, `disable-jobs`). Disabling crash-looping daemons is what got amethyst to the lock screen.
- **Seed data** (seed-data-skip-setup.py): Setup marked done, BKNoWatchdogs, FBLaunchWatchdogExceptions, no auto-dim or idle sleep. ruan seeds the same values.
- **Keybag:** the AKS stand-in (aks=1 offline-key-events=1). ruan has it.
- **Framebuffer/IOSurface compression off:** ruan has it.
- **GPU sandbox grant:** NativeSandboxGrantProcess. ruan extends it (ruan-grant-provider.patch).
- **owner-dataprotect=1** (UniXNU-sheng zzzz-sheng-ipados27-bringup.patch:533-616): emulates data-protection classes on the unencrypted Data volume. Without it, PosterBoard judges its poster store non-viable. **ruan lacks this.**
- **HEIC → PNG wallpaper overlay** (scripts/make-wallpaper-overlay.py): ImageIO can't decode wallpaper 7570's HEIC layers without a hardware HEVC decoder. With the overlay, alioth reports CollectionsPoster:device-ok. **ruan lacks this.**
- **Failing posters:** disabled by label (alioth sheng-lite=1). `wallpaper-freeze=1` is a diagnostic mode.
- **Memory:** no per-process limit raise anywhere in UniXNU.

## ruan's poster kill

- **What it is:**
  - Every ~5.0 s a poster extension gets SIGKILL with reason namespace 7 (LIBXPC) code 3. No UniXNU device documents this kill, and the meaning of code 3 wasn't found.
  - A touch on the dead poster's scene then makes SpringBoard trap (FBS processIdentity).
- **Lead:** the rbu125u CollectionsPoster crash report shows RUNNINGBOARD 0xDEAD10CC ("suspended while holding a file lock"). Its thread was in `flock` inside libvulkan_freedreno, the Mesa/Turnip disk cache.
  - So when RunningBoard suspends the poster, it still holds the cache lock and gets killed. [I, strong]
- **Memory floor:** most likely not the cause, since the kill isn't jetsam (ns 1).

## Fixes, ranked

1. **Deny the GPU to poster extensions** (pong's fix). Done in ruan-grant-provider.patch; boot-arg `ruan-poster-gpu=1` turns it back off. With no Turnip in the poster, there's no lock to hold.
2. **Log the ns 7 payload and sender pid** in the exit hook, and pull the corpse .ips files.
3. **HEIC → PNG wallpaper overlay** at root-build time; then restore CollectionsPoster.
4. **Disable or relocate the Mesa disk cache in non-render-server processes** (MESA_SHADER_CACHE_DISABLE, or a cache dir per process).
5. **Port owner-dataprotect=1**, including the open_dprotected_np clamp (vfs_syscalls.c:5034-5043).
6. **Treat ruan-memlimit-floor as optional.**
7. **Add the remaining tier A/B daemon disables.**
