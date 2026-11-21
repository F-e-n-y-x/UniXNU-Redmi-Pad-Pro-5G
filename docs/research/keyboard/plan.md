# Software keyboard 16 pt shift on ruan (iPadOS 17.7.11, 21H461)

Research only, 2026-11-21. No device access, no builds. Tags: **[V]** means verified here from the 21H461 shared cache,
the root or the logs. **[I]** means inferred and not yet checked on the device.

## Symptom, measured

- In both shots (790x1264, half resolution) the keys are the same width (Q is 59 px wide in each). In the shifted shot
  every key moves +16 px, which is 32 native px or **16 pt**. Delete and the right shift key get clipped. [V]
- 16 pt = (800 − 768) / 2, where 800 is our portrait width and 768 is the iPad 9.7" ("Wildcat") keyplane width. The
  shift looks like a 768-wide keyplane centred in the 800 pt window while the keys are already stretched to 800. [I]
- The layout on screen is the compact iPad layout: no number row, no tab key, flick digits on the letter keys. The
  12.9" Pro layout does not appear, even though the identity is 12.9". [V]

## 1. How the iPad keyboard picks its size (21H461 UIKitCore and TextInputUI)

- `_UIKeyboardComputeKeyboardIdiomFromScreenTraits` (UIKitCore 0x18a1a1160) [V]:
  - Takes the size from `UIScreen _referenceBounds` and falls back to `nativeBounds/scale` when that is larger. It
    does not use MobileGestalt width or height.
  - Walks `iPadWidthFilterTable` (0x18b33efa0): 768x1024→26, 810x1080→25, 834x1112→24, 820x1180→24, 834x1194→24,
    954x1366→23. For each row with w+h ≤ ours it takes that row's idiom, unless the size is an exact match, in which
    case it keeps the previous value.
  - Real iPads always match a row exactly. Our 800x1280 matches none, so the result is **24** (the 11" class).
  - After the table: `enableProKeyboard` → **23**. Otherwise, if the idiom is 24 and MobileGestalt
    `main-screen-class == 28` (a dispatch_once block at 0x18aac91b4), the result becomes **26**, the 768 class.
- `-[UIKeyboardPreferencesController enableProKeyboard]` reads the preference key **`KeyboardUseProLayout`**. When
  the key is unset, the default is `mainScreen._referenceBounds.height == 1366`, so it is off on our 1280 pt screen. [V]
- Layout data lives in `TextInputUI.framework/KBLayouts_iPad.dat` and `KBLayouts_Dynamic.dat` [V]:
  - `KBLayouts_iPad.dat` holds static `Wildcat-Portrait-*` and `Wildcat-Landscape-*` layouts. These have fixed
    768/1024 geometry, and `UIKBRenderFactoryiPadFudge*` / `Wildcat-PortraitFudge` stretch them.
  - `KBLayouts_Dynamic.dat` holds `Dynamic-*-{Mini,Small,Medium,Large,Grand}` layouts, which are fitted to the size
    (`updateDynamicKeyboard:usingSize:isLandscape:`, `-[TUIKeyboardLayoutFactory keyboardSuffixForScreenDimensions:]`).
- `-[UIKeyboardLayoutStar stretchKeyboardToFitKeyplane:]` runs when `keyplane.frame.width != screenTraits.keyboardWidth`.
  `UIKBKeyplaneView`, `UIKBKeyView` and `UIKBBackgroundView` cache images by `cachedWidth`, and
  `-[UIKBTree centerKeyplaneInRect:scale:]` exists. [V: symbols]
- Every keyboard width source reads UIScreen bounds, not MobileGestalt: `keyboardScreenReferenceSize`,
  `+[UIKeyboardImpl keyboardWidthForScreen:withOrientation:]` and `-[UIRemoteKeyboardWindowHosted
  keyboardWidthForCurrentDevice]`. [V]
- `_UIScreenTypeForScreen` matches native size, scale and corner radius against 45 Apple screen types. 1600x2560@2
  matches none, so the type is −1. [V: code. I: that it returns −1 on the device]

**Most likely cause** [I]: our 800 pt width is not a native layout width, so we land on Apple's "stretch a fixed
keyplane" path, which real iPads never hit in full screen. On a plane switch (or the first touch), one draw uses the
unstretched 768 frame or cached image, centred, and the next full layout stretches it back. This fits the +16 pt
shift exactly. If `main-screen-class` is 28, the idiom becomes 26, which always loads the static 768 Wildcat
keyplanes.

## 2. What the port reports

- `[adt.chosen.u32] display-scale = 2` in `unixnu-ruan/board.toml` gives a UIScreen of 800x1280 pt. [V]
- The profile (`UniXNU/profiles/os/ipados-17.7.11-21H461/profile.toml`) sets iPad7,1 / J120: `artwork-device-subtype
  = 2732`, `artwork-scale-factor = 2`, `product-id` (row J120), and "iPad Pro (12.9-inch) (2nd generation)". Ruan
  only overrides `artwork-display-gamut = sRGB`. [V]
- **Mismatch** [V]: the artwork subtype says 2732 (1024x1366 pt) while the screen is 800x1280 pt. UIKit's keyboard
  idiom does not read the subtype (see section 1), but assets, the `_screenType` (−1) and `main-screen-*` can
  disagree. The HakiOS `mg_test` log (iPhone X identity) showed `main-screen-width/height/class` taken from the
  **product** (1125x2436, class 15), not from the panel. On ruan they are probably 2048/2732 and the J120 class. [I]
- No MobileGestalt cache or override is in `make-ramroot.py` or the seed data. The only seed preferences are
  springboard, purplebuddy, backboardd and the like. [V]

## 3. Other ports

- j716f and tb365fc (both 1600x2560) and sirius (1080x2240) all use the same iPad7,1 profile with
  `display-scale = 2`. None of them has a keyboard note about this. j716f reports only that "keyboard typing works". [V]
- aston (phone) chose an iPhone17,2 identity whose subtype matches. sheng uses J820AP / iPad17,x on iPadOS 26/27. No
  port overrides `main-screen-*` or MobileGestalt for the keyboard. [V]

## 4. kbd 5 s waits (symbolicated, slide 0x138c0000)

```
semaphore_timedwait_trap ← _dispatch_semaphore_wait_slow
← -[_ICPortraitPredictionSource _quickTypeQueryWithQuery:limit:timeoutInMilliseconds:]   (5000 ms)
← -[_ICPortraitPredictionSource searchForMeCardRegionsWithTimeout:handler:]
← -[_ICPredictionManager searchForMeCardRegions]                                           (then a 5500 ms wait)
← TIAssetManager updatedActiveRegionsWithCompletion block ← TIDispatchAsync (workqueue)
```
[V] This is a Me-card region lookup in PersonalizationPortrait (suggestd). It runs on a background queue at kbd start
(about 65–72 s) and again at about 401–406 s. suggestd keeps crashing (`macOS/crashes/suggestd-*.ips`, 9 files), so
the query times out. It is not on the main thread or on the layout path, so it **should not** shift the planes. It may
delay predictions or the first QuickType bar. [I]

## Ranked fixes

1. **Check first, no build** (needs a probe on the tablet, which only the ROM session or the user can run):
   - Print MobileGestalt `main-screen-width`, `-height`, `-scale`, `-class`, `-pitch` and `ArtworkDeviceSubType`.
   - Print `UIScreen.mainScreen` bounds, nativeBounds, scale and `_referenceBounds`.
   - Read `/var/mobile/Library/Preferences/com.apple.keyboard.preferences.plist`.
   - This decides between rows 2 and 3.
2. **Turn on the Pro layout** (`KeyboardUseProLayout = true`) to force idiom 23 and the Dynamic "Grand/Large"
   layouts, which are fitted to the width. In `make-ramroot.py` `SEED_PREFERENCES` add
   `'com.apple.keyboard.preferences': {'KeyboardUseProLayout': True}`. On the UFS data, write the same key into the
   mobile plist, or set the Settings toggle if it shows. [V: key and branch. I: domain name, and that idiom 23 avoids
   the shift.] Test: tap `.?123` 10 times, then `#+=` and back, and check that the keys line up with taps.
3. **If `main-screen-class` = 28**: change that answer, or the J120 row data, so the idiom stays 24 (Dynamic) and
   not 26 (Wildcat 768). [I]
4. **Bring the identity in line with the panel**: give MobileGestalt `main-screen-width = 1600` and `-height = 2560`,
   and set `artwork-device-subtype` to the nearest real pad value (2360 for 820x1180, or 2388 for 834x1194) in
   `[adt.product.u32]` of `board.toml`. This removes the 2732-vs-1280 disagreement for assets and for any code that
   reads MobileGestalt. [I: it does not change the keyboard idiom by itself]
5. **Last resort**:
   - Patch `iPadWidthFilterTable` or `_UIKeyboardComputeKeyboardIdiomFromScreenTraits` in the shared cache (for
     example a row with 800x1280→24), or use a UIKit hook in kbd, InputUI and the apps.
   - Or show a real iPad point size: no integer scale of 1600x2560 matches one, and letterboxing wastes the panel.
6. **kbd waits**: fix the suggestd crash. If it is not needed, leave it, since the waits are not in the layout path.

Not checked: the actual MobileGestalt answers on the device, the KBLayouts plist internals, and the `cachedWidth`
redraw path itself.
