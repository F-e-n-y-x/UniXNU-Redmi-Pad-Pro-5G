# Daemon churn and Metal per-process memory (ruan, iPadOS 21H461)

Research only (2026-11-21). Nothing built, flashed or run on the tablet. **[V]** = verified in logs/source, **[I]** = inferred.

## 1. Who exits most

Sources: `logs/rbu130u/info-now.txt` (the only one of rbu127u/130u/131u with `exit` lines; the other two replays stop at `exit init`),
`rbu129u/flip-raw.txt`, `rbu120u/live.txt`, `rbu84/shot.txt` (UFS/Adreno era), `rbu83c/shot.txt` plus all `killing_*_process` lines under `logs/` (RAM-root era, heavy jetsam).
Reason namespaces from `xnu bsd/sys/reason.h`: ns 1 JETSAM (code 4 vm-pageshortage, 5 proc-thrashing, 7 per-process-limit), ns 2 SIGNAL, ns 7 LIBXPC, ns 15 RUNNINGBOARD. [V]

| # | Process | Count | Exit / cause | Safe to disable? |
|---|---|---|---|---|
| 1 | CollectionsPoster (WallpaperKit.framework/PlugIns/CollectionsPoster.appex) | 31 in rbu130u (every ~5.07 s), 120 in rbu129u, 79 in rbu84 | ns 7 code 3: libxpc kills it. Sandbox denies `iokit-get-properties` on NativeLegacyDisplay just before. Per `patches/xnu/ruan-memlimit-floor.patch`, its 15 MB ActiveHard limit is too small once the in-process GPU driver starts [I] | Not a launchd job. It is the default lock-screen poster. See §2 C |
| 2 | WidgetRenderer_Default | 98 (rbu84), 34 (rbu120u), 1 in rbu130u | SIGABRT (ns 2 code 6). In rbu125u it was RenderBox `unable to load shader library` [V, from the make-ramroot.py comment] | No (widgets). Fixed in rbu130u: 1 exit |
| 3 | suggestd | 3 per boot (rbu130u), 24 (rbu129u) | SIGABRT from an ObjC exception (std::terminate, .ips) [V]. Probably frameworks it needs were trimmed (`HEAVY_FEATURES` matches `Suggest/Proactive`) [I] | **Yes** |
| 4 | accountsd, CommCenterMobileHelper, nfcd, misd, rapportd, accessoryupdaterd, nehelper, mobiletimerd, routined, chronod, … | 268 / 153 / 104 / 79 / 69 / 57 / 44 / 41 / 31 / 31 kills (all logs) | jetsam vm-pageshortage, mostly at priority band 10 (idle) [V] | See §2 |
| 5 | mediaserverd | 39 | per-process-limit 140 MB at device open (rbu82/83) [V] | No. This is the Metal-memory question in §3 |
| 6 | SpringBoard | 1 per boot | SIGTRAP in FrontBoardServices (.ips) [V], after a dead poster scene [I] | No |
| 7 | SiriTTSService.TrialProxy (XPC service), apsd (EXC_GUARD XPC fault) | 1 each | — | TrialProxy goes with Siri, apsd see §2 B |

Also seen: `launchd[1] caught waking the CPU 46283 times over ~21 seconds` (2169/s) in rbu130u [V]. This is respawn churn.
The rbu127u–131u boots have no jetsam lines. The memory pressure came from RAM-root builds (rb8x) [V].

## 2. Labels to disable

Every label below exists in `System/Library/xpc/launchd.plist` and none is in the 54-entry disabled.plist yet [V].
Better place for them: `DISABLED_JOBS` in `make-ramroot.py`, so `seed_data` writes them on every build.

**A. Low risk (no radio, no Apple ID, no Siri on this tablet):**
`com.apple.CommCenterMobileHelper`, `com.apple.nfcd`, `com.apple.seld`, `com.apple.seserviced`, `com.apple.MobileInternetSharing`,
`com.apple.accessoryupdaterd`, `com.apple.betaenrollmentd`, `com.apple.ap.adprivacyd`, `com.apple.studentd` (KeepAlive+RunAtLoad),
`com.apple.tipsd`, `com.apple.financed`, `com.apple.icloud.fmfd`, `com.apple.icloud.fmflocatord`, `com.apple.routined` (locationd is already off),
`com.apple.suggestd`, `com.apple.triald`, `com.apple.PowerUIAgent`, `com.apple.dataaccess.dataaccessd`, `com.apple.syncdefaultsd`,
`com.apple.wifianalyticsd`, `com.apple.transparencyd`, `com.apple.icloudmailagent`, `com.apple.security.cloudkeychainproxy3`, `com.apple.ptpd` (KeepAlive; PTP photo import over USB).

**B. Medium risk (test one boot; a client may block or log errors):**
`com.apple.familycircled`, `com.apple.amsaccountsd`, `com.apple.duetexpertd`, `com.apple.remindd`, `com.apple.akd`, `com.apple.apsd` (KeepAlive; push, and Settings may wait on it), `com.apple.nehelper`, `com.apple.bird`, `com.apple.appleaccountd`.

**C. Keep (SpringBoard, Settings or the lock screen depend on them):** `accountsd`, `managedconfiguration.profiled`, `donotdisturbd`, `chronod` (widgets), `mediaremoted`, `mobiletimerd` (Clock), `mobileassetd`, `contextstored`/`rapportd`/`cloudd` (kept on purpose, make-ramroot.py:124).
For CollectionsPoster, either boot with `ruan-memlimit-floor=<MB>` (already drafted) or seed a non-Collections poster. Removing the appex is untested; rb11 showed the logo hangs when PosterBoard is missing.

**SKIP_DAEMONS additions** (they then cost no root space or RAM either): `suggestd`, `triald`, `tipsd`, `financed`, `studentd`, `betaenrollmentd`, `ap.adprivacyd`, `nfcd`.
Only labels whose name is `com.apple.<name>` fit `SKIP_DAEMON_PATHS`, so `ap.adprivacyd` works. Disabling them in launchd alone is enough to stop the churn.

## 3. Metal per-process memory knob

There is **no `host_ram_whole` chunk policy in play** [V]. `host_ram.rs:225` `CHUNK_BYTES` (64 MiB) and `:850` `LONGEST_LISTED_CHUNK = 512 << 20` belong to the VMM guest-RAM import (`use_host_ram`).
The ruan bridge (`drivers/AdrenoMetal/bridge/*.rs`) never calls it: a grep for `host_ram|HostRam` finds nothing. The string only appears in IPADOS-BRINGUP.md:345.
There is no env var or plist for any of the sizes below. They are Rust constants in the pinned translator (`UniXNU/.cache/gpu-reference/metal2vulkan`, HEAD 46e7cc9 plus local edits) or `ShaderCacheOptions` fields. All paths are under `crates/metal2vulkan-runtime/src/`.

| Knob | Where | Now | Recommend | What smaller costs |
|---|---|---|---|---|
| Kept empty blocks | `memory.rs:251` `KEPT_EMPTY_BYTES`, `:255` `KEPT_EMPTY_SHARE`, `kept_budget` `:265` | min(1/8 of all heaps, 2 GiB) per device, freed only after `KEPT_EMPTY_FOR` = 10 s (`:276`). On a unified ~7 GiB Adreno heap that is ~0.9 GiB per Metal process [I] | `64 << 20` and 2 s, as a new `bridge/patches/00xx` | More `vkAllocateMemory`/free work under bursty video or texture load. `KEPT_EMPTY_BLOCKS = 2` (`:245`) still stops per-frame block churn [V comment] |
| In-memory pipeline memo | `shader_cache/mod.rs:112` `DEFAULT_MEMORY_CAPACITY` 256 MiB, set through `ShaderCacheOptions.memory_capacity` | 256 MiB per process | `32 << 20`. No runtime patch needed: set `options.shader_cache.memory_capacity = 32 << 20;` after `bridge/NativeMetalObjects.rs:267` (and `NativeMetalBridge.rs:93`) | Pipelines it evicts are read back from the disk cache, so first-use hitches return. A live pipeline keeps its own copy anyway |
| Block size | `memory.rs:204` `BLOCK_BYTES` 64 MiB, `:298` `FIRST_BLOCK_BYTES` 4 MiB (already reduced for iPadOS guests) | — | leave | Smaller blocks mean more allocations and can hit `maxMemoryAllocationCount` |
| Disk cache `org.unixnu.metal` | `SharedShaderCache.h:26`, `shader_cache/mod.rs:109` 25 GiB | disk only, not RAM | 1–2 GiB on the 32 GiB unixnu partition | Older entries are evicted (LRU) |

Other fixed per-process caps that can add up: `recycle.rs:120` 64 MiB, `attachment_stand_in.rs:265` 256 MiB, `texture/twin_decode.rs:54` 256 MiB, `pipeline/compile_memory.rs:86` FREE_FLOOR 256 MiB [V that these exist; how much each holds when idle is not measured].
Recommended first step: the 32 MiB memo (one line, bridge only) and `KEPT_EMPTY_BYTES = 64 MiB`. Then compare mediaserverd and CollectionsPoster footprints against their 140 MB / 15 MB limits.

## 4. Snippet: add labels to the binary disabled.plist

```python
#!/usr/bin/env python3
import plistlib, sys
path = sys.argv[1]  # .../data/db/com.apple.xpc.launchd/disabled.plist
LABELS = """com.apple.CommCenterMobileHelper com.apple.nfcd com.apple.seld com.apple.seserviced com.apple.MobileInternetSharing
com.apple.accessoryupdaterd com.apple.betaenrollmentd com.apple.ap.adprivacyd com.apple.studentd com.apple.tipsd com.apple.financed
com.apple.icloud.fmfd com.apple.icloud.fmflocatord com.apple.routined com.apple.suggestd com.apple.triald com.apple.PowerUIAgent
com.apple.dataaccess.dataaccessd com.apple.syncdefaultsd com.apple.wifianalyticsd com.apple.transparencyd com.apple.icloudmailagent
com.apple.security.cloudkeychainproxy3 com.apple.ptpd""".split()  # tier A; append tier B after a test boot
with open(path, 'rb') as f:
    d = plistlib.load(f)
assert isinstance(d, dict) and d.get('com.apple.watchdogd') is True, 'not the seeded disabled.plist'
added = [l for l in LABELS if d.get(l) is not True]
d.update({l: True for l in LABELS})
with open(path, 'wb') as f:
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print(f'{len(added)} added, {len(d)} total')
```
Keep the file root:wheel 0644 as seed_data writes it, and keep `disabled.migrated` next to it.
