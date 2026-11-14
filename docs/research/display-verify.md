# Verification (2026-11-14, no device access)

All checks are read-only and re-runnable:

```sh
python3 tools/verify-kernel-pins.py /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/kernel/kernel.macho
python3 -I tools/check-text-fixups.py <dir with the extracted 21H461 dyld subcaches> board/text_fixups.toml
```

## 1. Kernelcache pins against our prelinked kernel

Every absolute address and byte string that `audit-mobile-graphics-legacy.py` (UniXNU), `build-gop-legacy-display.py`
(gts7xlwifi, the same pins as j716f/alioth/sheng) and `build-sheng-hid-input.py` hard-code, checked in our
`kernel.macho`. The prelinked Apple kexts stay at their original 21H461 addresses in every UniXNU kernel (only the XNU
kernel itself is ours), so the pins carry over unchanged: **50/50 match, 0 mismatches.**

Row 12 is the one check that depends on our native kernel rather than Apple's code: every IOService slot the original
Legacy vtable inherits (rebound by XnuPrelink) resolves to an exported symbol of our kernel.

| # | Source | Check | Address | Result | Detail |
|---|---|---|---|---|---|
| 1 | audit-mobile-graphics-legacy | IOMobileGraphicsFamily __TEXT_EXEC sha256 (FAMILY_TEXT_SHA) | `extent` | MATCH | 2e044ee68c165039 |
| 2 | audit-mobile-graphics-legacy | kernel.json sha256 equals kernel.macho | `file` | MATCH |  |
| 3 | audit-mobile-graphics-legacy | original kernelcache sha256 (OS profile pin) | `file` | MATCH |  |
| 4 | audit-mobile-graphics-legacy | Framebuffer/Service/Legacy constructor ADRP+ADD vptr stores | `0x...77b324/77b51c/7865d8` | MATCH | legacy_vptr=0xfffffff005f38570 |
| 5 | audit-mobile-graphics-legacy | Legacy C2 -> Service C2 -> Framebuffer C2 chain | `0xfffffff0057865fc,0xfffffff00577b540` | MATCH |  |
| 6 | audit-mobile-graphics-legacy | Service overrides only slots 0,1,7 | `vtable` | MATCH |  |
| 7 | audit-mobile-graphics-legacy | 358 non-null slots, 24 zero bytes after | `0xfffffff005f38570` | MATCH |  |
| 8 | audit-mobile-graphics-legacy | abstract slots == [211,213,245,246,306,307,337,338,345] | `vtable` | MATCH | [211, 213, 245, 246, 306, 307, 337, 338, 345] |
| 9 | audit-mobile-graphics-legacy | 103 generic overrides, <168 = [0,1,7,14,21,44,79,85,86,87,111,141,155] | `vtable` | MATCH |  |
| 10 | audit-mobile-graphics-legacy | slot 141 == native IOService::newUserClient (4-arg) | `vtable` | MATCH |  |
| 11 | audit-mobile-graphics-legacy | every slot >=168 (non-abstract) inside the family text | `vtable` | MATCH |  |
| 12 | build-gop-legacy-display | inherited native slots (<168) resolve to exported kernel symbols | `vtable` | MATCH |  |
| 13 | build-gop-legacy-display | Legacy C2 binding | `0xfffffff0057865cc` | MATCH |  |
| 14 | build-gop-legacy-display | Legacy vtable = legacy_vptr-16 | `0xfffffff005f38560` | MATCH | derived, recomputed above |
| 15 | build-gop-legacy-display | Legacy gMetaClass is backed data | `0xfffffff006b35940` | MATCH |  |
| 16 | build-gop-legacy-display | Legacy C2 calls Service C2; D2 calls Service D2 | `0xfffffff0057865fc,0xfffffff005786630` | MATCH |  |
| 17 | build-gop-legacy-display | IOSurface getter descriptor (ldr +48; ret) | `0xfffffff004b9ead0` | MATCH |  |
| 18 | build-gop-legacy-display | IOSurface getter width (ldr +88; ret) | `0xfffffff004b9ebbc` | MATCH |  |
| 19 | build-gop-legacy-display | IOSurface getter height (ldr +96; ret) | `0xfffffff004b9ebc4` | MATCH |  |
| 20 | build-gop-legacy-display | IOSurface getter stride (ldr +104; ret) | `0xfffffff004b9ebd4` | MATCH |  |
| 21 | build-gop-legacy-display | IOSurface getter allocation (ldr +144; ret) | `0xfffffff004b9ebfc` | MATCH |  |
| 22 | build-gop-legacy-display | IOSurface getter format (ldr +128; ret) | `0xfffffff004b9ebac` | MATCH |  |
| 23 | build-gop-legacy-display | IOSurface getter planes (ldr +152; ret) | `0xfffffff004b9ebf4` | MATCH |  |
| 24 | build-gop-legacy-display | vsync sender prologue | `0xfffffff005796720` | MATCH |  |
| 25 | build-gop-legacy-display | vsync sender +5592 client queue load | `0xfffffff005796830` | MATCH |  |
| 26 | build-gop-legacy-display | vsync six-word async send | `0xfffffff00579685c` | MATCH |  |
| 27 | build-gop-legacy-display | vsync drop-allowed constant | `0xfffffff005795fbc` | MATCH |  |
| 28 | build-gop-legacy-display | slot 301 address | `0xfffffff00579ea5c` | MATCH | 0xfffffff00579ea5c |
| 29 | build-gop-legacy-display | slot 301 1/60 s minimum (0x04444444) | `0xfffffff00579ea68` | MATCH |  |
| 30 | build-gop-legacy-display | slot 301 tail | `0xfffffff00579ec54` | MATCH |  |
| 31 | build-gop-legacy-display | setDisplayRefreshProperties prologue | `0xfffffff00579ebb8` | MATCH |  |
| 32 | build-gop-legacy-display | setDisplayRefreshProperties IOMFBDisplayRefresh publish | `0xfffffff00579ed54` | MATCH |  |
| 33 | build-gop-legacy-display | start_hardware() call + result check | `0xfffffff005787ef8` | MATCH |  |
| 34 | build-gop-legacy-display | slot 249 getDisplayDimensions reads +5808 | `0xfffffff00579fcbc` | MATCH | 0xb956b108 |
| 35 | build-gop-legacy-display | slot 249 getDisplayDimensions reads +5812 | `0xfffffff00579fce4` | MATCH | 0xb956b508 |
| 36 | build-gop-legacy-display | slot 295 address | `0xfffffff005795b88` | MATCH | 0xfffffff005795b88 |
| 37 | build-gop-legacy-display | slot 295 passes true to completion body | `0xfffffff005795b98` | MATCH |  |
| 38 | build-gop-legacy-display | completion calls slot 337 with null outputs | `0xfffffff00578aa7c` | MATCH |  |
| 39 | build-gop-legacy-display | isMainDisplay dispatch tests bit 0 | `0xfffffff005777e9c` | MATCH |  |
| 40 | build-gop-legacy-display | slot 354 applyStateRequest body (comment pin) | `0xfffffff0057970cc` | MATCH | 0xfffffff0057970cc |
| 41 | build-sheng-hid-input | IOHIDFamily __TEXT_EXEC sha256 (HID_TEXT_SHA) | `extent` | MATCH | 7e4c5104814f2e77 |
| 42 | build-sheng-hid-input | IOHIDDevice native-base overrides | `0xfffffff005d045c0` | MATCH |  |
| 43 | build-sheng-hid-input | 207 slots, abstract == [180] | `0xfffffff005d045c0` | MATCH |  |
| 44 | build-sheng-hid-input | IOHIDDevice C2 + vptr store | `0xfffffff004ad94f8` | MATCH |  |
| 45 | build-sheng-hid-input | C2 GOT -> native IOService C2 | `0xfffffff005d00f10` | MATCH |  |
| 46 | build-sheng-hid-input | D2 GOT -> native IOService D2 | `0xfffffff005d00f18` | MATCH |  |
| 47 | build-sheng-hid-input | handleStart + report descriptor ABI | `0xfffffff004ad9aec` | MATCH |  |
| 48 | build-sheng-hid-input | slot 181 handleReport address | `0xfffffff004add0c4` | MATCH |  |
| 49 | build-sheng-hid-input | handleReport forwards (ts, report, type, options) | `0xfffffff004add110` | MATCH |  |
| 50 | build-sheng-hid-input | IOHIDDevice gMetaClass backed | `0xfffffff006ae6fa0` | MATCH |  |

50/50 match; kernel /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/kernel/kernel.macho sha256 cbf29ea1a26757f87d78a8ac318973cdad9e6758a15069078af6cc36e6c43120

The kernel was rebuilt by the other agent during this work (a9d63477... at the start, cbf29ea1... at the end); the table
was produced on both and matched 50/50 both times (the family is prelinked unchanged).

## 2. End-to-end dry build of the proposed archive (scratch tree, nothing written to UniXNU or UniXNU-ruan)

`proposed/scripts/build-gop-legacy-display.py` was run in a scratch copy of the build tree (symlinks to the real tree's
read-only `build/xnu`, `.cache`, `drivers/GopFramebuffer` = UniXNU HEAD b0d812d, `kits/XnuPrelink`; `kits/ruan` = `proposed/package`;
a copy of our `kernel.{macho,json}` as reference candidate). Result:

- `audit-mobile-graphics-legacy.py`: PASS (358 slots, abstract [211,213,245,246,306,307,337,338,345]).
- NativeLegacyDisplay.cpp (iOS build) and LegacyDisplayProbe.cpp compile with the kernel's IOPlatformExpert recipe, `-Werror`.
- Owned vtable: 358 relocated entries; owned slots exactly [0,1,7,14,84,86,87,141,155,184,188,288,290,296,301] + the 9 leaves.
- 193 absolute bindings emitted; `libNativeLegacyDisplay.late.a` built (684 KB).
- Undefined symbols of the three objects: 216, all exported by our kernel except `___ubsan_handle_add_overflow_minimal`, which is a
  local symbol of the kernel link itself (same `-fsanitize=signed-integer-overflow` minimal runtime the record compiles with), so the
  final link resolves it. The real link was not run (it needs CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y in the live tree).
- Found and fixed on the way: upstream `build-arm-driver.py` cannot compile NativeLegacyDisplay.cpp on this host (clang-24 `-Wextra`
  `missing-designated-field-initializers` errors inside XNU's IOLib.h/kalloc.h/vm_kern.h, because it drops the record's `-Wno-`
  switches), and staging a source under another basename renames its `__GLOBAL__sub_I_<file>` initializer. The proposed script
  compiles both files itself with the record's suppressions and keeps the basename.
- `proposed/package/BootProgress.c` compiles with the startup.o recipe (`-Werror`) and exports `boot_progress_prepare_scanout`.
- `diffs/all.patch` applies cleanly (`patch -p1 --dry-run`) to copies of the current UniXNU-ruan files.

## 3. Text fixups against the 21H461 SystemOS cryptex (044-76034-038.dmg, subcaches extracted to a scratch directory)

```
OK    dyld_shared_cache_arm64.15 +0x421e8bc: have 28ad1bb0008544f9 want 28ad1bb0008544f9 -> c0035fd6008544f9
OK    dyld_shared_cache_arm64.13 +0x1b05bec: have 44008052 want 44008052 -> 24008052
OK    dyld_shared_cache_arm64.13 +0x1b05b08: have 44008052 want 44008052 -> 24008052
SKIP  BTAudioHALPlugin +0x38bec: file not extracted
OK    dyld_shared_cache_arm64.31 +0x40c448: have 4001899ac2fc1736e7ffff17 want 4001899ac2fc1736e7ffff17 -> 4001899ae8ffff17e7ffff17
OK    dyld_shared_cache_arm64.03 +0x27cfe84: have f39c35f060423839c0010034 want f39c35f060423839c0010034 -> f39c35f07f423839c0010034
OK    dyld_shared_cache_arm64.03 +0x27cfebc: have e8079f1a68423839e80740f9 want e8079f1a68423839e80740f9 -> e8079f1a7f423839e80740f9
OK    dyld_shared_cache_arm64.03 +0x272df00: have 08a2359000813039e00313aa want 08a2359000813039e00313aa -> 08a235901f813039e00313aa
OK    dyld_shared_cache_arm64.03 +0x272dea4: have 08a2359000412539e00313aa want 08a2359000412539e00313aa -> 08a235901f412539e00313aa
OK    dyld_shared_cache_arm64.03 +0x28d6ae4: have 490932b028013b39fd7bc1a8 want 490932b028013b39fd7bc1a8 -> 490932b03f013b39fd7bc1a8
OK    dyld_shared_cache_arm64.03 +0x28d7b7c: have 4809329000813b39fd7bc1a8 want 4809329000813b39fd7bc1a8 -> 480932901f813b39fd7bc1a8
OK    dyld_shared_cache_arm64.03 +0x28d7ce0: have 2900805209c12339c0035fd6 want 2900805209c12339c0035fd6 -> 290080521fc12339c0035fd6
OK    dyld_shared_cache_arm64.11 +0x34f3b84: have 979701944914d794 want 979701944914d794 -> e0031faa1f2003d5
```

BTAudioHALPlugin (entry 4) lives in the OS image, not the SystemOS cryptex; it is the OS profile's own entry, unchanged.
Our RAM root carries these subcaches unchanged (make-ramroot.py only drops `dyld_shared_cache_arm64.symbols`), so the
fixups apply at page-in exactly as on gts7/j716f/sirius.
