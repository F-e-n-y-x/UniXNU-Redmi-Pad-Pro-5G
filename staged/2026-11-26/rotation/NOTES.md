# Rotation on ruan macOS 27.3: notes (2026-11-26, offline, nothing built or run)

## Files
- fb-rotate-display.patch: UniXNU-macos (drivers/GopFramebuffer/NativeLegacyDisplay.cpp + NativeLegacyRequest.h). `git apply --check` OK on 8542980.
- fb-rotate-touch.patch: UniXNU-ruan-macos (package/RuanTouch.c). `git apply --check` OK.
- rotate-test.c: host self-test of native_legacy_copy_rotated (`cc -I b rotate-test.c`), passes.

## Boot-arg fb-rotate=1 (90 deg) / 3 (270 deg); 0 or absent = unchanged
- startHardware installs GOP height x width (2560x1600) as the display size, so publishDisplayAttributes,
  setDigitalOutMode (DisplayWidth/Height) and the swap checks see landscape. At scale 2 that is 1280x800 pt.
- kickOnce: panel checks use panelMatches(); the blank fill uses the panel geometry; the CPU copy calls
  native_legacy_copy_rotated (row-major 64-bit stores into the panel, column reads from the cached source map).
- Turned off in rotate mode (they cannot rotate): direct DPU flip (startDirect), GPU blit (native_legacy_gpu_offer
  is CP_MEMCPY and requires 1600x2560), the fb-diff shadow, half-float surfaces (rejected with kIOReturnUnsupported).
- Touch: fb-rotate also sets swap + one mirror after normalization (1: swap, flip y; 3: swap, flip x); the goodix
  path got the swap the focaltech path already had. touch_flip still XORs on top if the side is wrong.
- Pick 1 vs 3 by which side the USB port should be on; if the image is right but upside-down, use the other value.

## Risks
- Speed: CPU copy only (it was 143-285 ms/swap before the DPU/GPU paths). Rotated is likely similar or slower.
- If WindowServer sends half-float (EDR) surfaces, rotate mode shows nothing (rejected). Check countFormat stats.
- getDisplayDimensions is also read at line ~1218 (setDigitalOutMode): fine, it reports landscape.

## Not usable
- IOFramebuffer rotation (kIOFBSetTransform, kIOFBCapabilityRotation / IOFBRotateFlags, Displays "Rotation" popup):
  our display is an IOMobileFramebufferLegacy subclass, not IOFramebuffer, so none of those calls reach it.
  Whether SkyLight's CGSConfigureDisplayRotation works on IOMFB displays (e.g. by sending transformed layers) is
  unverified; validateSwap rejects nonzero option words, so any such swap would be refused today.
- DPU rotation: SSPP 90-deg inline rotation on DPU 9.x is UBWC NV12-only; linear BGRA cannot be rotated by the
  DPU. SSPP H/V flip would give 180 deg on the direct path, not 90.

## Real auto-rotate
- Accel: on SM7435 the IMU sits behind the ADSP sensor island (Snapdragon Sensor Core, QMI svc 400). ruan's
  port has no ADSP boot at all (RuanBattery reads the fuel gauge around it). Part number not checked
  (needs /vendor/etc/sensors/config on the tablet).
- Working reference: refs/darwin-lisa/kernel/drivers/adsp (hakios.adsp.c PIL+GLINK+QRTR, dlisa.ssc.c, dlisa.fastrpc.c
  sensors-PD file server). Proven on lisa (SM7325): run sns1 streamed accel `29 -277 9842 mm/s2`. Port = ~5.4k lines
  + ADSP firmware + sensors registry/JSON; weeks, not days.
- Then: a kernel thread maps gravity to 1/3 and sets `rotate`. 1<->3 is copy + touch only (no mode change).
  0<->1/3 changes the display size: needs re-publishing the timing element and a hotplug 0->1 cycle to WindowServer.
