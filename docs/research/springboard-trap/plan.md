# SpringBoard SIGTRAP in FrontBoardServices+0x1a570 (21H461)

Status 2026-11-21: root cause found offline (symbols + disassembly). The fix is not tested on the tablet yet.

## Root cause (verified)

The trap is `NSParameterAssert(processIdentity)` in `+[FBSSceneClientIdentity identityForProcessIdentity:]`
(FrontBoardServices+0x1a570 = +0x100 in that method). The message is `"Invalid condition not satisfying: %@"` with `@"processIdentity"`,
followed by `brk #0`.

Symbolized main thread (rbu179u `SpringBoard-2026-11-18-120835.ips`, every other copy is the same):

```
+[FBSSceneClientIdentity identityForProcessIdentity:]                     FBS +0x1a570   <- brk
+[SBApplicationSceneHandleRequest defaultRequestForApplication:sceneIdentity:displayIdentity:]  SB +0x509434
-[SBSpotlightMultiplexingViewController sceneHandle]                      SB +0x4deb18
-[SBSpotlightMultiplexingViewController _configureStatusBarScrollToTopView]
-[SBSpotlightMultiplexingViewController didBecomeActiveDelegate]
-[SBSpotlightDelegateManager _modifyDelegatesWithBlock:] / addSpotlightDelegate:forLevel:
-[SBSpotlightMultiplexingViewController viewWillAppear:]                  SB +0x80fa0
-[AXSB_SBSpotlightMultiplexingViewController viewWillAppear:]             (AX override, calls super; absent in rbu81)
-[SBTransientOverlayScenePresenter performPresentationRequest:] ... SBTransientOverlayPresentWorkspaceTransaction _begin
-[SBMainWorkspace _executeTransitionRequest:...]
-[SpringBoard beginInteractiveSpotlightTransientOverlayPresentationWithValidator:beginHandler:]
-[SBIconController _startSpotlightInteractiveGestureTransactionForGesture:]
-[SBSearchGesture _notifyThaWeStartedShowingOrHiding] <- -[SBSearchGesture scrollViewWillBeginDragging:]  (SpringBoardHome)
UIKit touch / gesture delivery
```

From the disassembly:
- `sceneHandle` does `app = [[SBApplicationController sharedInstance] applicationWithBundleIdentifier:@"com.apple.Spotlight"]`.
  The CFString was decoded from the cache. The scene identity is `identityForIdentifier:_spotlightSceneIdentifier workspaceIdentifier:@"com.apple.Spotlight"`.
- `defaultRequestForApplication:` calls `[FBSSceneClientIdentity identityForProcessIdentity:[[app info] processIdentity]]`.
  Because `app` is nil, `processIdentity` is nil, so the assert fires.
- `app` is nil because **`Applications/Spotlight.app` is left out of every root**. `make-ramroot.py` puts `SKIP_APPS += 'Spotlight'`
  for the RAM root and, for the RUAN_FULL_ROOT (UFS) root, excludes `Applications/Spotlight.app/` at line 189 (rb18g1: Spotlight's
  nil-AssetProperties abort loop). `ramroot/work-full/root/Applications` really has no Spotlight.app.

Trigger: the home-screen **pull-down search gesture** (`SBSearchGesture`, a downward drag on the icon pages). This explains
"when the user swipes". A Control Center or app-switcher swipe that starts on the icon area, or a stray or ghost touch from
RuanTouch, starts the same scroll drag. Uptimes are 130-2900 s. Other threads are idle (event fetch, metal2vulkan, Wi-Fi, AXSpeech).
`lastExceptionBacktrace` and `asi` are empty. The 120626 report in the same folder is a separate crash (IconFoundation SIGBUS on
the `SBHIconImageCache` queue).

The same signature appears in rbu81, rbu158p-2, rbu168p and rbu179u. The make-ramroot.py comment from rbu142p already names this assert.

## Accessibility and the daemons (verified)

- `AXSB_SBSpotlightMultiplexingViewController` is only the AX safe-category subclass, and it forwards to super.
  **rbu81 crashed the same way with no AX image loaded at all**, so AX is not the cause.
  AXSpringBoardServerInstance is loaded in rbu179u, most likely because of the seeded `com.apple.Accessibility`
  `EnhancedBackgroundContrastEnabled: True` (Reduce Transparency). That link is inferred, and it is harmless.
- passd and suggestd are not involved. The missing identity belongs to the Spotlight app.
- Turning off AX or the daemons will **not** fix this.

## Fixes, ranked

1. **Put Spotlight.app back in the UFS root (recommended, one line).** In `make-ramroot.py:189`, drop
   `'Applications/Spotlight.app/'` from the RUAN_FULL_ROOT exclusion. Keep Setup. LaunchServices then registers
   `com.apple.Spotlight`, `applicationWithBundleIdentifier:` returns an app, and the identity is valid.
   If Spotlight still aborts with nil AssetProperties, only the Spotlight process dies. SpringBoard handles a dead
   overlay scene through the normal scene invalidation and does not assert.
   *Test:* build rbu18x u, boot, swipe down on the home screen 10 times and open CC and the app switcher. Expect no
   `SpringBoard-*.ips` with FBS+0x1a570. Watch for `Spotlight-*.ips` and for any relaunch churn every ~20 s (CPU cost).
2. **If Spotlight's abort loop costs too much:** fix the abort and not the app. `searchd` (DISABLED_JOBS) and the MobileAsset
   SpotlightResources asset are what return nil AssetProperties. Try re-enabling `com.apple.searchd` and its
   `spotlightknowledged`/`corespotlightd` (SKIP_DAEMONS), or supply the SpotlightResources asset. Inferred, not checked.
3. **RAM root (SKIP_APPS):** same as fix 1 if the ~MBs fit, or accept the crash there. With Spotlight absent, nothing in a
   seed or plist turns `SBSearchGesture` off. `-[SBSearchGesture setDisabled:forReason:]` exists, but only code calls it.
   We have no userland hook mechanism, so a code fix (disable SBSearchGesture or nil-guard `sceneHandle`) would need new
   injection infrastructure. Not recommended.
4. Not fixes: AX seed changes or disabling passd/suggestd.

## Tools used
`sb_syms.txt` was extracted from the decompressed `dyld_shared_cache_arm64.symbols` (session scratch). objc stubs were resolved
through their selrefs, and CFStrings through the cache mappings (`/root/unixnu-work/dsc` sym.py/dis.py helpers).
