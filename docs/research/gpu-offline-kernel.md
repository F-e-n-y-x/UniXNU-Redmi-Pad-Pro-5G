# GPU agent A (kernel side): does ruan need an offline/real GPU to leave the Apple logo?

## Verdict: no. The GPU is not the logo blocker
- amethyst (SM7635, iPadOS 17.7.11 21H461, the same OS build) reached the **lock screen with a live clock before it had
  any GPU**. It presented frames by CPU copy, "continuous swaps every ~2.3s (CPU presentation)":
  refs/UniXNU-amethyst/scripts/log/WORKING-BOOT.md:131-140. Its GPU work came later (V52, Adreno 810 + Turnip add-on):
  UniXNU/docs/devices/amethyst-bluedmc-amethyst.md:7, :276, :350-388. So CoreAnimation/backboardd fall back to software
  rendering on 21H461. No Metal device is required for SpringBoard UI.
- amethyst's logo stall had **our exact shape**: "only 4 swaps at 137-146s, none afterwards; Apple logo stayed"
  (WORKING-BOOT.md:106-108) and "Six swaps ... then none ... the first UI frame never arrives" (:118-121).
  - **Cause: crash-looping daemons.** Disabling liveactivitiesd, privacyaccountingd, ScreenTimeAgent, dmd,
    assistantd, biomed and biomesyncd "removed the v2 stall" (:131-139).
  - **ruan rb8s–rb11 crash-loop as well:** audiomxd every ~5 s, searchd, siriactionsd, xpcproxy, and maild was killed.
    Our make-ramroot.py keeps biomed on purpose. That is userspace (agent B's area). It is the top candidate.
- **The "offline GPU" in UniXNU does not apply to a tablet.**
  - NativeOfflineGPU (UniXNU/drivers/AdrenoMetal/NativeOfflineGPU.cpp:1-3, 312 lines) is a broker. It forwards Metal
    commands over VirtioConsole (`NativeVirtioConsoleTransport.h`, `native_console_receive`) to a host Metal server
    (HostMetalCommandServer.mm).
  - It needs a VM host with Metal behind it, and ruan has none.
  - NativeOfflineAccelerator.cpp is an 11-line stub.
- **A real GPU means porting Adreno740** (UniXNU/drivers/Adreno740: A740/SM8550 only, README.md:5) to ruan's
  Adreno 710 (SM7435). That covers GMU and zap PIL, the SMMU context bank, a GMU pool reservation in UEFI PrePi, and the
  AdrenoMetal/Turnip add-on (amethyst-bluedmc-amethyst.md:102, :173, :363-388). Size: weeks.
  This is for speed later (CPU copy is ~170 ms per frame, :276), not for reaching the UI.

## Ranked kernel-side actions (all small)
1. **Add `armcpu-self-ipi=1` to every SMP image.** rb8s–rb11 use `smp=1 armcpu-spi-ipi=1` without it.
   - **Evidence:** amethyst V20/V21/V24 hung at the logo with "cpu_signal failed in DebuggerXCallEnter". Without the
     self-IPI, XNU leaves SIGPdisabled set on the secondaries, so a stackshot or an idle cannot signal them
     (WORKING-BOOT.md:193-199). V25 with self-ipi fixed it: "IPIs taken on all 8 cores, no cpu_signal failed"
     (:210-212).
   - **Why it matters for ruan:** a stackshot is just what a stuck-UI watchdog (FrontBoard/backboardd hang detection)
     triggers, so this can cause or prolong a logo stall.
   - **Code:** ArmCpu reads it at UniXNU/drivers/ArmCpu/ArmCpu.cpp:36 and :511.
   - **Staged:** see kernel-args.diff.
2. **No GPU component, no offline GPU, no IOAccelerator stub.** Publishing a fake IOAccelerator/IOGPU with no working
   Metal behind it would make CA pick the Metal path and fail. Today it correctly takes the software path.
3. **Keep `offline-mobile-graphics-load=1`** (already set; it loads IOMobileGraphicsFamily, see
   UniXNU/patches/xnu/offline-component-load-probes.patch:70). That is the only graphics load the CPU-presentation refs
   use (amethyst-bluedmc-amethyst.md:265; sirius docs/devices/sirius-huajifruit.md:90).
   - **Not needed:** `offline-legacy-unit-scale=2`. LegacyDisplayProbe.cpp:6 sets display-scale statically.
   - **Not needed:** sirius' `offline-mobile-graphics-client=1`. No CPU-presentation lock-screen ref needs it.

## For agent B / the parent (userspace, outside this scope)
- Disable the crash-loopers through launchd's disabled.plist in the Data seed, the same way amethyst's v3 seed did:
  audiomxd, searchd/corespotlightd, siriactionsd, maild, plus amethyst's set (liveactivitiesd, privacyaccountingd,
  ScreenTimeAgent, dmd, assistantd, biomed, biomesyncd).
- Then expect what amethyst saw: a stall of up to ~130 s after the first swaps, then UI with swaps every ~2 s.

## Not done
- No kernel component to pin-verify: none is proposed.
- No real-GPU port started; its size is above.
