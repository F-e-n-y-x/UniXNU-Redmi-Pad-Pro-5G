# Swipe-up-and-pause goes Home instead of the App Switcher

Research only. Nothing was built or run on the tablet. Tags: **[V]** checked in our source or in the 21H… dyld shared cache (`/root/unixnu-work/dsc`); **[I]** inferred, a boot must confirm.

## Summary

The most likely cause is that **no touch samples are sent while the finger rests**. SpringBoard decides "pause" from an average velocity taken over a time window measured against the current time. When that window holds no new samples, it falls back to the speed of the last two moving samples. So a pause reads as a flick and the gesture goes Home. The fix is a ~120 Hz heartbeat with sub-point jitter for a resting finger in `RuanTouch.c`.

The other suspects (thresholds tied to the 800x1280 pt size, the edge band, frame pacing, timestamps) were checked. They are either fine or minor.

## 1. Gesture velocity and timing (ranked first)

### What SpringBoard does [V, disassembled from SpringBoard.framework in the cache]

- `SBHomeGesturePanGestureRecognizer` `touchesBegan:`/`touchesMoved:` read `coalescedTouchesForTouch:` (imps 0x1cf9d7180, 0x1cf9d98e0). Every HID sample goes into an `SBTouchHistory` (ring of {CGPoint, timestamp}, depth 32, `initWithDepth:` 32 at 0x1cfa14550) through `updateWithLocation:timestamp:`. That call stores `now - timestamp` as the touch latency.
  - Frame rate therefore does not lose samples: coalesced touches carry every HID event.
- `-[SBTouchHistory touchHistoryDepthForTimeDuration:forComputingDerivative:]` (0x1cf9d7418) walks back from the newest sample while `now - sample.timestamp < duration`. With the derivative flag set it returns that count plus one.
- `-[SBTouchHistory averageTouchVelocityOverTimeDuration:]` (0x1cf9d7314) averages Δpos/Δt over that many sample pairs, using a window of `duration + latency`.
  - **If no sample is inside the window, the depth is 1. The "average" is then the velocity between the last two samples ever received, which is the motion before the pause.** [V for the code path. That `now` is CACurrentMediaTime is I: it is a stub call whose result is subtracted from the sample timestamps.]
- `SBHomeGestureSettings -setHomeGestureTuningDefaults` (0x1cfff5a8c) values for idiom pad:

| setting | value |
|---|---|
| pauseVelocityThresholdForAppSwitcher | 51 |
| (setter at 0x1cfff5c28, almost surely) pauseVelocityThresholdForDefiniteAppSwitcher | 27 |
| maximumAdaptivePauseVelocityThresholdForAppSwitcher | 308 (phone 134) |
| snapToMaxVelocityThresholdAfterAccelerationDip | YES |
| appSwitcherVelocityThresholdIncreasingRateFraction | 0.2 |
| adaptiveThresholdsDecreasingRateFraction | 1/7 |
| velocityYThresholdForUnconditionalHome | -204 |
| minimumYVelocityForHome | -51 |
| minimumYDistanceForHomeOrAppSwitcher | 12 (phone 10) |
| normalizedDistanceYThresholdForUnconditionalHome | 0.8 ± 0.1 (phone 0.6 ± 0.2) |

### What we send [V]

- `RuanTouch.c:1458` posts MOVE **only when the coordinates changed**. A resting finger on the NT36532 (Xiaomi firmware filters jitter) produces identical coordinates [I], so nothing is posted. NativeHidInput only sends on a new seqlock value (`NativeHidInput.cpp` touchSend).
- Even if same-position reports were sent, IOHIDEventDriver would most likely send them without the Position bit, and UIKit would mark the touch as stationary, not moved [I]. Real Apple digitizers keep reporting with sub-pixel noise.
- In the touch_irq path, with a finger down and no interrupt, the thread re-reads only every 20 ms (`RuanTouch.c:1803`).
- Timestamps: `handleReport(buffer,0,0)` (`NativeHidInput.cpp:202`) stamps the report at the moment it is posted. The touch thread posts synchronously through `runAction`, so there is no queue or batching. The stamp is about one SPI read after the IRQ, a nearly constant offset that does not distort velocity. This is minor.
- Rate: the chip reports at 240 Hz, the ring holds 32 samples (133 ms), and the window logic is time-based. The rate is not the problem.

This matches "most of the time Home": the switcher wins only when the finger happens to wobble enough to change the raw coordinate during the hold. sheng (same NT36532 family, same NativeHidInput) reports the same failure ("Both App Switcher gestures failed", reports/ipados27-touch-frame-gaps.md, UniXNU/docs/devices/sheng-*.md). orangepi5's scripted fixed-rate swipe does open the switcher.

### Zero-build check (do this first)

Swipe up to about 1/3 of the screen height. Instead of holding still, **keep the finger wobbling slightly for about 0.3 s**, then lift. Repeat 10 times and compare with 10 still holds, watching the camera. If the wobble trials reliably open the switcher, cause 1 is confirmed.

### Fix A: resting-finger heartbeat (`package/RuanTouch.c`, focaltech_contact_update, line 1458)

Keep the real position stored. When a read finds the same position and at least 8 ms have passed since the last post for that id, post MOVE with y alternating between the real value and real+4. The +4 is in 0..65535 units: it survives the 65535→20319→32767 rescale and is about 0.08 pt, roughly 5 pt/s of fake speed, far below 27/51.

```c
static uint64_t focaltech_posted[FOCALTECH_MAX_TOUCHES], heartbeat_abs;  /* heartbeat_abs = 8 ms, nanoseconds_to_absolutetime once */
static uint8_t focaltech_wobble[FOCALTECH_MAX_TOUCHES];
...
	} else if (focaltech_contact[id].x != norm_x || focaltech_contact[id].y != norm_y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, norm_x, norm_y);
		focaltech_posted[id] = mach_absolute_time();
	} else if (mach_absolute_time() - focaltech_posted[id] >= heartbeat_abs) {
		/* a resting finger: SpringBoard's SBTouchHistory averages velocity only over samples inside its time window and
		 * otherwise reuses the last moving pair, so a silent pause reads as a flick (Home, not the App Switcher) */
		focaltech_wobble[id] ^= 1;
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, norm_x, focaltech_wobble[id] ? (norm_y > 65531 ? norm_y - 4 : norm_y + 4) : norm_y);
		focaltech_posted[id] = mach_absolute_time();
	}
```

Also set `focaltech_posted[id]` on DOWN.

### Fix B: keep reads at 120 Hz or more while a finger is down (`RuanTouch.c:1803`)

Change `touch_any_down() ? 20 : 80` to `? 8 : 80`, so the heartbeat has reads to ride on when the chip stops interrupting for a still finger. The blind 1 ms tick path already re-reads within 5 ms.

### Fix C (optional, minor): real sample time

Pass `nvt_irq_at` through and call `handleReportWithTime` instead of `handleReport(buffer,0,0)`. This is shared UniXNU code, so do it only if A and B are not enough.

### Test

Build with A+B. Add a heartbeat counter next to `ruan_touch_events` (`RuanInput.c`) for the USB `touch` dump. Run:
- 20× swipe to 1/3 height + 300 ms still hold + lift. Expect at least 18 switcher.
- 20× fast flick. Expect Home.
- Taps and typing for regressions: the jitter is below the hysteresis of 10 pt.

## 2. Coordinates and size (low)

- [V] The digitizer covers the full panel. The 16000x25600 fw range → 0..65535 → 0..30479 / 0..20319 (`RuanInput.c`) → 0..32767 (`NativeHidInput.cpp` transformCoordinate). The logical range is normalized by IOHIDEventDriver, so aspect and orientation are right (taps land correctly).
- [V] The thresholds are in points or normalized to the container height. Unconditional Home on iPad needs 0.8 ± 0.1 of the height, so a swipe to the middle (0.5) does not trigger it. minimumYDistanceForHomeOrAppSwitcher is 12 pt. Nothing depends on iPad7,1's 1366 pt.
- [V] Edge band: `kEdgeY = 48/2032` of the height, about 30 pt at the bottom (`NativeHidInput.cpp` edge::), so the gesture does begin. It is not the cause.

## 3. Frame pacing (low)

[V] The recognizer reads coalesced touches, so 30–60 fps does not drop samples from the history. Low fps only delays when the pause is evaluated (once per delivered touchesMoved). With Fix A those events keep arriving during the hold.

## 4. Other ports

- sheng: same failure, no fix found (docs above).
- orangepi5: a scripted swipe opens the switcher ("a third opens the App Switcher"). Its script is local only and was not available to check.
- No port found that adds a resting heartbeat. The searches covered UniXNU/drivers, kits and docs/devices; /root/unixnu-work/vg had no switcher notes.

## Settings and defaults (SEED_PREFERENCES)

- [V] `SBHomeGestureSettings` is a PrototypeTools `PTSettings` subclass in `/System/Library/PrivateFrameworks/SpringBoard.framework/SpringBoard` (class 0x1f56c97d0; `setDefaultValues` 0x1cfff5930, `setHomeGestureTuningDefaults` 0x1cfff5a8c).
- Overrides would come from PTDefaults (`com.apple.PrototypeTools`: PrototypeSettingsEnabled / ActivePrototypingEnabled) and archives in `/var/mobile/Library/Prototyping/Settings/*.archive`.
- [V string] PrototypeTools logs "Not observing PTDefaults on customer install." [I] On this customer build, seeding is therefore most likely ignored. There is no plain `SBAppSwitcherGesture…` defaults key in the cache.
- **Recommendation: no SEED_PREFERENCES change** in `make-ramroot.py:405`. If you want to try anyway, as one isolated test: `'com.apple.PrototypeTools': {'PrototypeSettingsEnabled': True}`. Expect no effect.

## Tools used

`/root/unixnu-work/dsc/sym.py` header code plus small scratch scripts: an objc classlist walk with the relative-method selector base at libobjc `__objc_opt_ro` + 29673820, and llvm-mc disassembly with msgSend-stub selector names.
