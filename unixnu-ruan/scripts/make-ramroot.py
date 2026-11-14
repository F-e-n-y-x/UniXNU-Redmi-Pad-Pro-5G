#!/usr/bin/env python3
"""Build ruan's RAM root: an APFS image of iPadOS 17.7.11 (21H461) that XNU mounts as /dev/md0 (boot-arg rd=md0).

usage: make-ramroot.py --mini|--full --os OS.dmg [--systemos SYSTEMOS.dmg] --work DIR --out IMAGE [--descriptor FILE]

  --mini   sbin/launchd and usr/lib/dyld with the directories around them (a few MB): small enough for the payload
           FAT, it proves the RAM root mount and launchd's exec; dyld then stops for want of its shared cache.
  --full   the OS volume trimmed as HakiOS trims its iOS 16.5 root (sysimage/select-system.py, tightened below), plus
           the SystemOS cryptex at private/preboot/Cryptexes/OS, where the OS volume's System/Cryptexes/OS link points,
           except its dyld shared cache (without the .symbols file), which is a real System/Library/Caches/com.apple.dyld
           (the cryptex's com.apple.dyld links there); one volume, so the cache is on the process' root volume as the
           kernel's shared-region check wants. Every file is zlib-compressed (decmpfs), as Apple's root.
  --data   (with --full) a second volume, Data (md0s2), with the OS image's private/var skeleton plus the directories a
           first boot creates (UniXNU make-data-seed.py / prepare-offline-data.py), with the Data role (set_data_role);
           patches/xnu/ruan-ram-data-mount.patch mounts it read-write on /private/var by role (ram-data=1). The container gets DATA_SLACK more free blocks for it.
  --data-container  (with --full) Data as a container of its own after the root container instead (PROPOSALS.md P2, the
           shape of every reference port): IMAGE = root container (System only), zeros to DATA_ALIGN, Data container.
           Same skeleton as --data plus the Setup-skip preferences and launchd's disabled.plist (seed_data), no role.
           The Data container's offset goes to IMAGE.split; boot with ram-data-split=<it> ram-data=1 ram-data-dev=md1
           (patches/xnu/ruan-ram-data-split.patch: md0 = root, md1 = Data). apfsck (apt apfsprogs) checks both.
  --descriptor  also write the 32-byte DarwinBoot USB RAM disk descriptor (DBUSBRD1: CRC32 and size) for IMAGE.

Files are copied byte for byte from Apple's images (code signatures and trust-cache hashes unchanged). The APFS
writer and reader are HakiOS's (tools/apfs, MIT; $HAKIOS, default /root/hakios-garnet), 7-Zip reads the DMGs and
HakiOS's unzbm expands LZBITMAP files 7-Zip leaves compressed. Nothing here touches a device. Keep WORK and IMAGE out
of git: they are Apple's files.
"""
import argparse
import hashlib
import importlib.util
import os
import re
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import zlib

HAKIOS = Path(os.environ.get('HAKIOS', '/root/hakios-garnet'))
MKAPFS = HAKIOS / 'tools/apfs/mkapfs.py'
APFS = HAKIOS / 'tools/apfs/apfs.py'
UNZBM = HAKIOS / 'xnu-deps/host/bin/unzbm'
SELECT = HAKIOS / 'sysimage/select-system.py'
CRYPTEX = 'private/preboot/Cryptexes/OS'
DYLD = 'System/Library/Caches/com.apple.dyld'
MINI_FILES = ['sbin/launchd', 'usr/lib/dyld']
MINI_DIRS = ['dev', 'sbin', 'usr', 'usr/lib', 'private', 'private/etc', 'private/var', 'private/tmp', 'private/preboot',
             'System', 'System/Library', 'System/Cryptexes', 'Library', 'Applications', 'bin']
LINKS = {'etc': 'private/etc', 'tmp': 'private/tmp', 'var': 'private/var'}
# beyond HakiOS's cuts: all applications, staged apps (Maps, Notes, ...: 400 MB), fonts over 8 MB (emoji, PingFang: HakiOS trims them
# with its own font tool), the privacy notices and Health, and frameworks of features far from the home screen
SKIP_FRAMEWORKS = ('CameraEffectsKit', 'Portrait', 'AvatarKit', 'PhotosUICore', 'PhotosFormats', 'HomeUI', 'WorkflowKit',
                   'CoreRE', 'ToneLibrary', 'PassKit', 'GeoServices', 'Memoji', 'VideosUI', 'NewsCore', 'NewsUI2')


# mkapfs.py with Apple's resource map in its type-4 (zlib, resource fork) decmpfs forks. iPadOS 17.7.11's AppleFSCompression
# (ChunkCompression.cpp:404, "invalid resource map") compares the fork's last 50 bytes with its own constant, whose first 16
# bytes (the map's copy of the fork header) are zero; mkapfs fills them, so every type-4 file read failed: the dyld cache
# ("no dyld cache", test 2026-11-14 03:00). The workers are forked, so they see the patched writer.
MKAPFS_APPLE_RSRC = r"""
import importlib.util, multiprocessing, sys
multiprocessing.set_start_method('fork', force=True)
spec = importlib.util.spec_from_file_location('mkapfs', sys.argv[1])
mk = importlib.util.module_from_spec(spec)
sys.modules['mkapfs'] = mk
spec.loader.exec_module(mk)
writer = mk.zlib_rsrc
def zlib_rsrc(content):
    fork = bytearray(writer(content))
    fork[-50:-34] = bytes(16)
    return bytes(fork)
mk.zlib_rsrc = zlib_rsrc
APPLE_MAP = bytes.fromhex('00' * 25 + '1c00320000636d70660000000a0001ffff0000000000000000')
assert zlib_rsrc(bytes(70000))[-50:] == APPLE_MAP and zlib_rsrc(bytes(70000))[:4] == writer(bytes(70000))[:4]
sys.argv = sys.argv[1:]
mk.main()
"""


def run(command, **kwargs):
    print('+', ' '.join(str(part) for part in command), file=sys.stderr)
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def listing(dmg, work):
    out = work / (Path(dmg).name + '.slt')
    if not out.is_file() or out.stat().st_mtime < Path(dmg).stat().st_mtime:
        with open(str(out) + '.part', 'w') as stream:
            run(['7z', 'l', '-slt', dmg], stdout=stream)
        os.replace(str(out) + '.part', out)
    return out


def extract(archive, out, *selection):
    """7z x; exit 2 is accepted when every error is a method 7-Zip lacks (LZBITMAP: expand_zbm finishes those)."""
    print('+ 7z x', archive, *selection, file=sys.stderr)
    done = subprocess.run(['7z', 'x', '-y', '-snld20', f'-o{out}', str(archive), *selection], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors='replace')
    errors = [line for line in done.stdout.splitlines() if line.startswith('ERROR:')]
    if done.returncode and not (done.returncode == 2 and errors and all('Unsupported Method' in e for e in errors)):
        raise SystemExit(f'7z x {archive} failed ({done.returncode}): {errors[:5] or done.stdout[-2000:]}')


def expand_zbm(tree):
    """7-Zip leaves LZBITMAP files as an empty file plus decmpfs/resource-fork side files; unzbm finishes them."""
    left = [str(path)[:-len(':com.apple.decmpfs')] for path in tree.rglob('*:com.apple.decmpfs')]
    for at in range(0, len(left), 200):
        run([UNZBM, *left[at:at + 200]])
    rest = [path for path in tree.rglob('*:com.apple.*')]
    if rest:
        raise SystemExit(f'{len(rest)} files still compressed, e.g. {rest[0]}')


# Lighter root (test 35 died of jetsam with ~500 MB free): resources over 256 KiB (models, asset catalogs, sounds) of
# private frameworks for features far from the home screen. The bundles and their plists stay, so lookups find them.
HEAVY_FEATURES = re.compile(r'^System/Library/PrivateFrameworks/[^/]*(Health|Heart|Sleep|Fitness|Workout|Medical|Siri|'
                            r'Assistant|Voice|Speech|Dictation|Translat|Weather|News|Stocks|Books|Podcast|Music|TV|Game|'
                            r'Arcade|Wallet|Payment|Maps|Navigation|Memories|Portrait|Avatar|Memoji|ARKit|Reality|Backup|'
                            r'iWork|Feedback|Biome|Intelligence|Suggest|Proactive)[^/]*\.framework/')


# Launch daemons of those features, so they cost no RAM either (whatever asks for them gets no such service). Kept on
# purpose: coreduetd, biomed, contextstored, cloudd, sharingd, rapportd, diagnosticd (SpringBoard and logging use them).
SKIP_DAEMONS = ('assistantd', 'assistant_cdmd', 'assistant_service', 'corespeechd', 'siriinferenced', 'siriknowledged',
                'sirittsd', 'SiriTTSTrainingAgent', 'siriactionsd', 'siri.sirireaderd', 'speechmodeltrainingd', 'healthd',
                'healthappd', 'healthrecordsd', 'fitnesscoachingd', 'sleepd', 'homed', 'newsd', 'gamed', 'gamepolicyd',
                'GameController.gamecontrollerd', 'passd', 'photoanalysisd', 'mediaanalysisd', 'mediaanalysisd.service',
                'knowledgeconstructiond', 'intelligenceplatformd', 'biomesyncd', 'spotlightknowledged',
                'spotlight.IndexAgent', 'searchd', 'parsecd', 'parsec-fbf', 'cloudphotod', 'backupd', 'mobilebackup2',
                'maps.destinationd', 'Maps.geocorrectiond', 'Maps.mapssyncd', 'Maps.pushdaemon', 'findmy.findmybeaconingd',
                'findmy.findmylocated', 'icloud.searchpartyd', 'icloud.findmydeviced', 'itunescloudd', 'medialibraryd',
                'analyticsd', 'osanalytics.osanalyticshelper', 'geoanalyticsd', 'audioanalyticsd', 'spindump',
                'avatarsd', 'bookdatastored', 'appstored', 'appstorecomponentsd', 'storekitd', 'itunesstored',
                'mobilestoredemod', 'mobilestoredemodhelper', 'feedbackd', 'proactiveeventtrackerd')
SKIP_DAEMON_PATHS = frozenset(f'System/Library/LaunchDaemons/com.apple.{name}.plist' for name in SKIP_DAEMONS)


def heavy_feature_resource(path, size):
    return size > 256 << 10 and not path.endswith('.plist') and bool(HEAVY_FEATURES.match(path))


assert heavy_feature_resource('System/Library/PrivateFrameworks/SiriTTS.framework/model.bin', 1 << 20)
assert not heavy_feature_resource('System/Library/PrivateFrameworks/SiriTTS.framework/Info.plist', 1 << 20)
assert not heavy_feature_resource('System/Library/PrivateFrameworks/SpringBoard.framework/x.car', 1 << 20)


# /Applications stays (rb11, 2026-11-14: SpringBoard sat on the Apple logo after 3 swaps). Its LaunchAngels (PosterBoard,
# InputUI, PaperBoard, ClockAngel, ...: System/Library/LaunchAngels) run programs from /Applications, and amethyst's logo stall
# ended when PosterBoard, InputUI and WidgetRenderer launched (refs/UniXNU-amethyst/scripts/log/WORKING-BOOT.md:107,137). All apps
# take 257 MiB uncompressed; these large ones are not needed for the lock/home screen (Setup is skipped by the Data seed).
SKIP_APPS = tuple(f'Applications/{name}.app/' for name in (
    'Setup', 'AppStore', 'SharingViewService', 'DiagnosticsService', 'MTLReplayer', 'Diagnostics', 'GameCenterWidgets',
    'HDSViewService', 'SOSBuddy', 'TVRemoteUIService', 'Climate'))
def select(slt, work):
    """HakiOS's selection with ruan's tighter limits; returns the files list and the manifest."""
    spec = importlib.util.spec_from_file_location('select_system', SELECT)
    pick = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pick)
    pick.STAGED_APPS = '\0'                        # staged apps stay under private/var, which is not taken
    pick.MAX_FONT, pick.TRIMMED_FONTS = 8 << 20, ()
    pick.SKIP_SYSTEM_LIBRARY += ('OnBoardingBundles', 'Health')
    pick.SKIP_PREFIX += tuple(f'System/Library/{name}/' for name in ('OnBoardingBundles', 'Health')) + \
        tuple(f'System/Library/PrivateFrameworks/{name}.framework/' for name in SKIP_FRAMEWORKS) + \
        tuple(f'System/Library/Frameworks/{name}.framework/' for name in SKIP_FRAMEWORKS)
    wanted = pick.wanted_file
    # the window leaves XNU what the image does not take: the large applications (SKIP_APPS), and string tables over 2 MB
    # (every language in one file; a missing one shows keys, not text)
    pick.wanted_file = lambda path, size: not path.startswith(pick.SKIP_PREFIX + SKIP_APPS) and \
        not (path.endswith('.loctable') and size > 2 << 20) and not heavy_feature_resource(path, size) and \
        path not in SKIP_DAEMON_PATHS and \
        wanted(path, size)
    files, manifest = work / 'root.files', work / 'root.manifest'
    sys.argv = ['select-system.py', str(slt), str(files), str(manifest)]
    pick.main()
    return files, manifest


def fix_spaceman(image):
    """Set the space manager's dev[TIER2].addr_offset (0x80), which mkapfs leaves 0, and reseal the object.

    iPadOS 17.7.11's APFS (spaceman_sanity_check, line 318) wants dev[MAIN].addr_offset + 8 * (cab_count or
    cib_count) <= dev[TIER2].addr_offset, as Apple's spaceman create writes it; with 0 it refused every mkapfs
    container ("Container corruption detected by spaceman_sanity_check:318", checkpoint data error 92, test 30)."""
    spec = importlib.util.spec_from_file_location('mkapfs', MKAPFS)
    mk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mk)
    with open(image, 'r+b') as f:
        nx = f.read(4096)
        bs, = struct.unpack_from('<I', nx, 0x24)
        desc_base, = struct.unpack_from('<Q', nx, 0x70)
        desc_index, = struct.unpack_from('<I', nx, 0x88)
        sm_oid, = struct.unpack_from('<Q', nx, 0x98)
        f.seek((desc_base + desc_index) * bs)
        cpm = f.read(bs)                                # checkpoint map: cpm_count at 0x24, 40-byte entries from 0x28
        count, = struct.unpack_from('<I', cpm, 0x24)
        entries = [struct.unpack_from('<IIIIQQQ', cpm, 0x28 + 40 * i) for i in range(count)]
        size, paddr = next((e[2], e[6]) for e in entries if e[5] == sm_oid)
        f.seek(paddr * bs)
        sm = bytearray(f.read(size))
        cib, cab, _, main_off = struct.unpack_from('<IIQI', sm, 0x40)
        tier2_off = main_off + 8 * (cab or cib)
        if struct.unpack_from('<Q', sm, 0x60)[0] or tier2_off > size:
            raise SystemExit(f'{image}: spaceman has a tier 2 device or no room at {tier2_off}')
        struct.pack_into('<I', sm, 0x80, tier2_off)
        struct.pack_into('<Q', sm, 0, mk.fletcher64(sm))
        f.seek(paddr * bs)
        f.write(sm)
    print(f'{image}: spaceman at block {paddr}: tier-2 addr_offset {tier2_off}', file=sys.stderr)


APFS_VOL_ROLE_DATA = 0x40
# Shared apfs_volume_group_id (superblock +0x3f0) of System and Data. The iOS APFS kext's VFSIOC_MOUNT_BYROLE handler (21H461
# kernelcache, the loop after "Can only be called from kernel") takes a Data-role volume only if its volume group id equals
# the origin (root) volume's, as macOS' System/Data pairs have. A fixed id: the same input gives the same image.
VOLUME_GROUP = bytes.fromhex('5275616e52616d526f6f744444617461')      # "RuanRamRootDData"


def set_data_role(image):
    """Give the Data volume (fs_index 1, named Data) the Data role (apfs_role, superblock +0x3c4), put System and Data in one
    volume group, and reseal both superblocks. The kernel mounts Data by role from the root volume
    (patches/xnu/ruan-ram-data-mount.patch, VFSIOC_MOUNT_BYROLE): md is no IOMedia, so APFS publishes no BSD node per
    volume (/dev/md0s2: ENOENT, test 2026-11-14 04:21). mkapfs writes neither roles nor groups."""
    import mmap
    spec = importlib.util.spec_from_file_location('mkapfs', MKAPFS)
    mk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mk)
    found = {}
    with open(image, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
        at = m.find(b'APSB')
        while at >= 0:
            block = at - 32
            name = bytes(m[block + 0x2c0:block + 0x2c7])
            if block % 4096 == 0 and (name.startswith(b'System\0') or name.startswith(b'Data\0')):
                found.setdefault(name.split(b'\0')[0], []).append(block)
            at = m.find(b'APSB', at + 4)
        if sorted(found) != [b'Data', b'System'] or any(len(v) != 1 for v in found.values()):
            raise SystemExit(f'{image}: volume superblocks {found}')
        for name, (block,) in found.items():
            sb = bytearray(m[block:block + 4096])
            if name == b'Data':
                struct.pack_into('<H', sb, 0x3c4, APFS_VOL_ROLE_DATA)
            sb[0x3f0:0x400] = VOLUME_GROUP
            struct.pack_into('<Q', sb, 0, mk.fletcher64(sb))
            m[block:block + 4096] = bytes(sb)
    print(f'{image}: Data role 0x{APFS_VOL_ROLE_DATA:x}, System+Data volume group {VOLUME_GROUP.hex()} '
          f'(superblocks at blocks {found[b"System"][0] // 4096}, {found[b"Data"][0] // 4096})', file=sys.stderr)


def mini_manifest(slt, work):
    """The two files with their directories, modes as Apple's listing gives them."""
    spec = importlib.util.spec_from_file_location('select_system', SELECT)
    pick = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pick)
    meta = {e['Path']: e for e in pick.entries(slt) if e['Path'] in MINI_FILES + MINI_DIRS}
    missing = [path for path in MINI_FILES if path not in meta]
    if missing:
        raise SystemExit(f'not in the OS image: {missing}')
    files, manifest = work / 'root.files', work / 'root.manifest'
    files.write_text(''.join(path + '\n' for path in MINI_FILES))
    with open(manifest, 'w') as out:
        for path in sorted(MINI_DIRS + MINI_FILES):
            e = meta.get(path)
            mode = pick.mode_bits(e['Mode']) if e else stat.S_IFDIR | 0o755
            out.write(f'{mode:o}\t{int(e.get("User ID") or 0) if e else 0}\t{int(e.get("Group ID") or 0) if e else 0}\t{path}\t\n')
    return files, manifest


def cryptex_union(tree):
    """Links on the root to every cryptex path the root lacks (the topmost missing one). iOS unions the OS cryptex over
    the root; without it libobjc's dlopen of /usr/lib/libobjc-trampolines.dylib fails and SpringBoard, contextstored and
    the rest abort with SIGABRT (rb3 2026-11-14; amethyst ROOT-BRINGUP.md saw the same abort string). 42 links on 21H461."""
    cryptex, links = tree / CRYPTEX, {}
    for current, dirs, files in os.walk(cryptex):
        rel = Path(current).relative_to(cryptex)
        for name in list(dirs) + files:
            path = (rel / name).as_posix()
            if path.startswith('System/Library/Caches/com.apple.dyld') or os.path.lexists(tree / path):
                continue
            links[path] = f'/{CRYPTEX}/{path}'
            if name in dirs:
                dirs.remove(name)
    assert links.get('usr/lib/libobjc-trampolines.dylib') or (tree / 'usr/lib/libobjc-trampolines.dylib').exists()
    return links


def add_links(manifest, links):
    present = {line.split('\t')[3] for line in manifest.read_text().splitlines()}
    with open(manifest, 'a') as out:
        for path, target in links.items():
            if path not in present:
                out.write(f'{stat.S_IFLNK | 0o755:o}\t0\t0\t{path}\t{target}\n')


# Directories a first boot expects on Data that the OS image's private/var does not have (UniXNU scripts/make-data-seed.py and
# prepare-offline-data.py, plus the ones below); those under mobile/ belong to mobile (501).
DATA_DIRS = ('keybags', 'keybags/backup', 'tmp', 'run', 'root', 'Library', 'preboot', 'PersonaVolumes', 'containers',
             'containers/Bundle', 'containers/Temp', 'containers/Data', 'containers/Data/System', 'containers/Data/Application',
             'containers/Shared', 'containers/Shared/SystemGroup', 'db', 'db/dslocal', 'db/dslocal/nodes',
             'db/dslocal/nodes/Default', 'db/dslocal/nodes/Default/users', 'db/dslocal/nodes/Default/groups',
             'db/dslocal/nodes/Default/config', 'mobile', 'mobile/tmp', 'mobile/Library', 'mobile/Library/Preferences',
             'mobile/Library/Caches', 'mobile/Library/Logs', 'mobile/Containers', 'mobile/Documents', 'mobile/Media',
             # launchd's own log (data test: "fopen(/private/var/log/com.apple.xpc.launchd/launchd.log): 2" on every line),
             # and what tzinit, auearlyboot and keybagd write first (paths from their strings)
             'log/com.apple.xpc.launchd', 'db/timezone', 'db/fud', 'firmware', 'root/Library',
             'root/Library/Preferences')
DATA_SLACK = int(os.environ.get("DATA_SLACK", 32768))  # free 4 KiB blocks in Data (default 128 MiB; 136 MiB hit ENOSPC, rb 2026-11-14)
DATA_ALIGN = 1 << 20                            # the Data container's offset in IMAGE (md1's base: page aligned is the minimum)
APFSCK = '/usr/sbin/apfsck'                     # Debian apfsprogs; skipped with a note when it is not installed

# --data-container's seed (refs/UniXNU-amethyst scripts/log/skip-setup-seed.py, the values of UniXNU
# scripts/seed-data-skip-setup.py --no-auto-dim, which sheng booted to SpringBoard): Setup marked done, no dimming or sleep
# without touch, framebuffer compression off for the GOP display path, and launchd jobs disabled: watchdogd (amethyst's seed;
# ruan's recurring SIGTRAP, test 36) plus amethyst's list of jobs that block the first boot or respawn every 10 s.
BUILD = '21H461'
CA_FRAMEBUFFER = {'CAEnableDeepFramebuffer': False, 'CAEnableFramebufferCompression': False,
                  'CAEnableIOSurfaceCompression': False}
SEED_PREFERENCES = {
    'com.apple.migration': {'LastSystemVersion': BUILD,
                            'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}},
    'com.apple.springboard': {'BKNoWatchdogs': True, 'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
                              'MCForceShowCloudConfigurationUI': False,
                              'SBDisableAutoDim': True, 'SBDontDimOrLockOnAC': True, 'SBDisableIdleSleep': True},
    'com.apple.purplebuddy': {'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
                              'daemonizeNeedsToRun': False},
    'com.apple.purplebuddy.notbackedup': {k: False for k in (
        'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
        'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy', 'SilentICDPUpgrade')},
    'com.apple.managedconfiguration.notbackedup': {'LockdownActivationIndicatesCloudConfigurationAvailable': False},
    'backboardd': CA_FRAMEBUFFER, '.GlobalPreferences': CA_FRAMEBUFFER,
    # CPU-only CoreAnimation (no GPU): Reduce Motion + Reduce Transparency cut most animations and blurs. Settings >
    # Accessibility crashed when opened (rb16), so they are preset here.
    'com.apple.Accessibility': {'ReduceMotionEnabled': True, 'ReduceMotionReduceSlideTransitionsPreference': True,
                                'EnhancedBackgroundContrastEnabled': True},
}
DISABLED_JOBS = ('com.apple.watchdogd', 'com.apple.identityservicesd', 'com.apple.locationd', 'com.apple.CommCenter',
                 # iconservicesagent stays enabled: disabled (amethyst's list), every icon on the home screen is a blank
                 # placeholder (rb14f, 2026-11-14). It keeps its cache in the system group container on Data.
                 'com.apple.bluetoothd',
                 'com.apple.telephonyutilities.callservicesd', 'com.apple.fitcored', 'com.apple.liveactivitiesd',
                 'com.apple.privacyaccountingd', 'com.apple.ScreenTimeAgent', 'com.apple.dmd', 'com.apple.assistantd',
                 'com.apple.biomesyncd', 'com.apple.biomed',
                 # ruan rb8s-rb11: crash-loop every few seconds (audiomxd SIGSEGV without audio hardware; searchd/siriactionsd
                 # NSInvalidArgumentException AssetProperties/NSBundle nil; maild killed). amethyst v2->v3: crash-loopers kept
                 # the first UI frame from coming (scripts/log/WORKING-BOOT.md:104-140).
                 'com.apple.audiomxd', 'com.apple.searchd', 'com.apple.siriactionsd', 'com.apple.email.maild')


def missing_jobs(tree):
    """Labels of launchd jobs (System/Library/xpc/launchd.plist) whose executable is not in the root or the OS cryptex: launchd
    logs "Missing executable detected" and xpcproxy exits 78 for each on every demand (39 on 21H461 with SKIP_DAEMONS)."""
    import plistlib
    jobs = plistlib.loads((tree / 'System/Library/xpc/launchd.plist').read_bytes())['LaunchDaemons']
    out = []
    for job in jobs.values():
        prog = (job.get('Program') or (job.get('ProgramArguments') or [''])[0]).lstrip('/')
        if prog and not any(os.path.lexists(tree / p) for p in (prog, 'private/preboot/Cryptexes/OS/' + prog)):
            out.append(job['Label'])
    return out


def seed_data(tree, manifest, extra_disabled=()):
    """Merge SEED_PREFERENCES into the Data tree (mobile's preferences, mobile 501 0600; the two CA domains for root too)
    and write db/com.apple.xpc.launchd/disabled.plist (+ disabled.migrated), with manifest lines for their owners."""
    import plistlib
    lines = []

    def merge(rel, values, owner, mode):
        path = tree / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        data = plistlib.loads(path.read_bytes()) if path.exists() else {}
        assert isinstance(data, dict), rel
        data.update(values)
        path.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
        lines.append(f'{stat.S_IFREG | mode:o}\t{owner}\t{owner}\t{rel}\t\n')

    for name, values in SEED_PREFERENCES.items():
        merge(f'mobile/Library/Preferences/{name}.plist', values, 501, 0o600)
    for name in ('backboardd', '.GlobalPreferences'):
        merge(f'root/Library/Preferences/{name}.plist', CA_FRAMEBUFFER, 0, 0o600)
    launchd = 'db/com.apple.xpc.launchd'
    if not any(line.split('\t')[3] == launchd for line in manifest.read_text().splitlines()):
        lines.append(f'{stat.S_IFDIR | 0o755:o}\t0\t0\t{launchd}\t\n')
    merge(f'{launchd}/disabled.plist', {job: True for job in (*DISABLED_JOBS, *extra_disabled)}, 0, 0o644)
    (tree / launchd / 'disabled.migrated').touch()
    lines.append(f'{stat.S_IFREG | 0o644:o}\t0\t0\t{launchd}/disabled.migrated\t\n')
    with open(manifest, 'a') as out:
        out.writelines(lines)
    assert plistlib.loads((tree / launchd / 'disabled.plist').read_bytes())['com.apple.watchdogd'] is True


def apfsck(image):
    """apfsprogs' checker on one container (read only), reported only: Apple's APFS is the judge (the root mounts with
    findings apfsck may not share), but a Data container it calls corrupt should not be sent."""
    if not Path(APFSCK).exists():
        print(f'{image}: {APFSCK} not installed (apt install apfsprogs), not checked', file=sys.stderr)
        return
    done = subprocess.run([APFSCK, '-cu', str(image)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(f'{image}: apfsck -cu exit {done.returncode} {done.stdout.strip()[-400:]}', file=sys.stderr)
    return done.returncode


def data_volume(osdmg, slt, work):
    """The Data volume's tree and manifest: the OS image's private/var (not staged_system_apps, which installs apps on
    first boot) with Apple's modes and owners, plus DATA_DIRS. Returns (tree, manifest)."""
    spec = importlib.util.spec_from_file_location('select_system', SELECT)
    pick = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pick)
    raw, tree, manifest = work / 'data-src', work / 'data', work / 'data.manifest'
    shutil.rmtree(raw, ignore_errors=True)
    shutil.rmtree(tree, ignore_errors=True)
    extract(osdmg, raw, 'private/var/*', '-xr!staged_system_apps')
    expand_zbm(raw)
    (raw / 'private/var').rename(tree)
    lines = {}
    for e in pick.entries(slt):
        path = e['Path']
        if not path.startswith('private/var/') or '/staged_system_apps' in path or path == 'private/var/staged_system_apps':
            continue
        rel, mode = path[len('private/var/'):], pick.mode_bits(e['Mode'])
        if mode is None:
            continue
        if e['Mode'][9:10] in 'tT':
            mode |= stat.S_ISVTX
        lines[rel] = (mode, int(e.get('User ID') or 0), int(e.get('Group ID') or 0), e.get('Symbolic Link') or '')
    for rel in DATA_DIRS:
        (tree / rel).mkdir(parents=True, exist_ok=True)
        if rel not in lines:
            owner = 501 if rel == 'mobile' or rel.startswith('mobile/') else 0
            mode = stat.S_IFDIR | (0o1777 if rel == 'tmp' else 0o700 if rel.startswith('keybags') else 0o755)
            lines[rel] = (mode, owner, owner, '')
    missing = [rel for rel, v in lines.items() if not (tree / rel).exists() and not (tree / rel).is_symlink() and not v[3]]
    if missing:
        raise SystemExit(f'Data: listed but not extracted: {missing[:5]}')
    tree.chmod(0o755)
    manifest.write_text(''.join(f'{m:o}\t{u}\t{g}\t{rel}\t{link}\n' for rel, (m, u, g, link) in sorted(lines.items())))
    return tree, manifest


def main():
    ap = argparse.ArgumentParser()
    kind = ap.add_mutually_exclusive_group(required=True)
    kind.add_argument('--mini', action='store_true')
    kind.add_argument('--full', action='store_true')
    ap.add_argument('--os', required=True)
    ap.add_argument('--systemos')
    ap.add_argument('--work', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--data', action='store_true')
    ap.add_argument('--data-container', action='store_true')
    ap.add_argument('--descriptor', type=Path)
    args = ap.parse_args()
    if args.full and not args.systemos:
        ap.error('--full needs --systemos')
    if (args.data or args.data_container) and not args.full:
        ap.error('--data and --data-container need --full')
    if args.data and args.data_container:
        ap.error('--data or --data-container, not both')
    for tool in (MKAPFS, APFS, UNZBM, SELECT):
        if not tool.exists():
            raise SystemExit(f'missing {tool} (set HAKIOS)')
    args.work.mkdir(parents=True, exist_ok=True)
    slt = listing(args.os, args.work)
    files, manifest = (mini_manifest if args.mini else select)(slt, args.work)
    tree = args.work / 'root'
    shutil.rmtree(tree, ignore_errors=True)
    tree.mkdir()
    extract(args.os, tree, f'@{files}')
    expand_zbm(tree)
    links = dict(LINKS)
    if args.full:
        cryptex = tree / CRYPTEX
        cryptex.mkdir(parents=True, exist_ok=True)
        # all of the cryptex but the cache's local symbols (685 MB that only symbolication reads)
        extract(args.systemos, cryptex, '-xr!dyld_shared_cache_arm64.symbols')
        expand_zbm(cryptex)
        # the cache itself sits at the pre-cryptex path, a real directory: dyld's default cache directory and the one
        # libignition checks for ("dylib cache directory present; not overriding"), with no symbolic link to follow
        # (libignition opens with O_NOFOLLOW_ANY); the cryptex path links to it
        (tree / 'System/Library/Caches').mkdir(parents=True, exist_ok=True)
        (cryptex / DYLD).rename(tree / DYLD)
        links.update({'System/Cryptexes/OS': '../../private/preboot/Cryptexes/OS', f'{CRYPTEX}/{DYLD}': f'/{DYLD}'})
        links.update(cryptex_union(tree))
        with open(manifest, 'a') as out:      # the directories leading to the cryptex, and the cryptex's own root
            for path in ('private/preboot/Cryptexes', CRYPTEX, 'System/Cryptexes', 'System/Library/Caches'):
                out.write(f'{stat.S_IFDIR | 0o755:o}\t0\t0\t{path}\t\n')
    add_links(manifest, links)
    volumes = [f'System={tree},manifest={manifest}']
    if args.data:
        data_tree, data_manifest = data_volume(args.os, slt, args.work)
        volumes.append(f'Data={data_tree},manifest={data_manifest}')
    image = args.out
    image.parent.mkdir(parents=True, exist_ok=True)
    # launchd and dyld stay plain: on ruan exec of a decmpfs /sbin/launchd failed with ENOEXEC (errno 8) at PID 1,
    # before the zlib decmpfs handler can serve it (test 20, 2026-11-14). Small next to the cache.
    plain = [a for d in ('sbin', 'usr/lib', 'usr/libexec') for a in ('--plain', d)]
    # 64 MiB free (tried for spaceman_sanity_check:318, test 30; not the cause, which fix_spaceman fixes)
    run([sys.executable, '-I', '-c', MKAPFS_APPLE_RSRC, MKAPFS, '-o', image, '-z', *plain, '--slack',
         str(16384 + (DATA_SLACK if args.data else 0)), *volumes])
    fix_spaceman(image)
    if args.data:
        set_data_role(image)
    size = image.stat().st_size
    if size % 4096:
        with open(image, 'ab') as out:
            out.write(bytes(4096 - size % 4096))
        size = image.stat().st_size
    if args.data_container:
        apfsck(image)
        data_tree, data_manifest = data_volume(args.os, slt, args.work)
        seed_data(data_tree, data_manifest, missing_jobs(tree))
        data = args.work / 'ruan-data.apfs'             # a fixed name: mkapfs.py derives the UUIDs from it
        run([sys.executable, '-I', MKAPFS, '-o', data, '--slack', str(DATA_SLACK), f'Data={data_tree},manifest={data_manifest}'])
        fix_spaceman(data)
        if apfsck(data):
            raise SystemExit(f'{data}: apfsck finds the Data container corrupt')
        split = -(-size // DATA_ALIGN) * DATA_ALIGN
        with open(image, 'ab') as out, open(data, 'rb') as src:
            out.write(bytes(split - size))
            shutil.copyfileobj(src, out, 16 << 20)
            out.write(bytes(-out.tell() % 4096))
        size = image.stat().st_size
        image.with_suffix('.split').write_text(f'{split}\n')
        print(f'{image}: root container 0..{split:#x} (md0), Data container {split:#x}..{size:#x} (md1): '
              f'ram-data-split={split}', file=sys.stderr)
    # read back: launchd (and the cache's first file) must come out of the image byte for byte
    checks = ['sbin/launchd'] + ([f'{DYLD}/dyld_shared_cache_arm64'] if args.full else [])
    for path in checks:
        got = subprocess.run([sys.executable, '-I', APFS, 'cat', str(image), '/' + path], check=True,
                             stdout=subprocess.PIPE).stdout
        want = (tree / path).read_bytes()
        if hashlib.sha256(got).digest() != hashlib.sha256(want).digest():
            raise SystemExit(f'{path}: read back differs from the source')
    crc, digest = 0, hashlib.sha256()
    with open(image, 'rb') as stream:
        for block in iter(lambda: stream.read(16 << 20), b''):
            crc = zlib.crc32(block, crc)
            digest.update(block)
    if args.descriptor:
        args.descriptor.write_bytes(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, size, 0))
    print(f'{image}: {size} bytes ({size / 1048576:.1f} MiB), crc32 {crc:08x}, sha256 {digest.hexdigest()}')


if __name__ == '__main__':
    main()
