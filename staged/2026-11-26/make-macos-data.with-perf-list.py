#!/usr/bin/env python3
"""make-macos-data.py FULL OUT_DIR: the macOS Data volume for the UFS root, on Linux, sent to the tablet over USB.

FULL is make-macos-full-tree.py's folder (FULL/os + os.manifest from extract-apfs-tree.py). The Data tree is the system
volume's /System/Library/Templates/Data with its owners (from the manifest), plus what UniXNU-macos's
make-macos-data-seed.py adds on a Mac, done here as files:
  - /private/var/db/.AppleSetupDone and the managed SkipSetupItems: no Language Chooser or Setup Assistant;
  - a local administrator (uid 501, admin + staff, SALTED-SHA512-PBKDF2 shadow hash) with a home made from the user template
    and .skipbuddy, and auto-login (autoLoginUser + /etc/kcpassword): an unattended test seed, never a default;
  - an ANSI keyboard type, so Keyboard Setup Assistant does not ask;
  - launchd's disabled.plist with watchdogd and cameracaptured off (no watchdog hardware or cameras; UniXNU-sheng's kernel
    writes the same file with disable-watchdogd=1).
The user's name and password come from MACOS_USER / MACOS_PASSWORD (defaults: ruan; the password defaults to the user name for this throwaway test account).

Writes OUT_DIR/macos-data.apfs (its volume UUID is fixed by the file name: mkapfs.py derives UUIDs from it; printed and written
to macos-data.uuid for the kernel's ruan-mac-data-uuid=), then OUT_DIR/root-usb.apfs = the Data container alone (the root is on
UFS; DarwinBoot loads this as /chosen/memory-map RAMDisk and NativeRamBlock serves it to APFS from offset 0), with root-usb.split
(0, unused on macOS) and the DBUSBRD1 descriptor root-usb.desc.
"""
import hashlib
import importlib.util
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

W = Path('/root/unixnu-work')
TEMPLATE = 'System/Library/Templates/Data'
DSLOCAL = 'private/var/db/dslocal/nodes/Default'
SETUP_SKIP = 'Library/Managed Preferences/com.apple.SetupAssistant.managed.plist'
LOGINWINDOW_PREFS = 'Library/Preferences/com.apple.loginwindow.plist'
KEYBOARD_TYPE = 'Library/Preferences/com.apple.keyboardtype.plist'
LAUNCHD_DISABLED = 'private/var/db/com.apple.xpc.launchd/disabled.plist'
KCPASSWORD_KEY = bytes.fromhex('7d895223d2bcddeaa3b91f')
SETUP_SKIP_ITEMS = ['Accessibility', 'AdditionalPrivacySettings', 'Appearance', 'AppleID', 'AppStore', 'Biometric', 'Diagnostics',
                    'DisplayTone', 'EnableLockdownMode', 'FileVault', 'iCloudDiagnostics', 'iCloudStorage', 'Intelligence', 'Location',
                    'OSShowcase', 'Privacy', 'Restore', 'Safety', 'ScreenTime', 'Siri', 'SoftwareUpdate', 'TermsOfAddress', 'TextSize',
                    'TOS', 'TouchIDPrompt', 'UnlockWithWatch', 'Wallpaper', 'Welcome']
MIB = 1 << 20


class Tree:
    """A directory plus its mkapfs manifest (path -> line), so owners can be set without being root on an ext4 tree."""

    def __init__(self, root):
        self.root, self.entries = root, {}

    def add(self, rel, mode, uid=0, gid=0, target=''):
        self.entries[rel] = f'{mode:o}\t{uid}\t{gid}\t{rel}\t{target}'

    def write(self, rel, data, mode, uid=0, gid=0):
        path = self.root / rel
        parts = Path(rel).parts
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d not in self.entries:
                (self.root / d).mkdir(exist_ok=True)
                self.add(d, 0o40755)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.add(rel, 0o100000 | mode, uid, gid)

    def mkdir(self, rel, mode, uid=0, gid=0):
        (self.root / rel).mkdir(parents=True, exist_ok=True)
        self.add(rel, 0o40000 | mode, uid, gid)

    def owner(self, rel):
        f = self.entries[rel].split('\t')
        return int(f[1]), int(f[2])


def copy_template(full, tree):
    src = full / 'os' / TEMPLATE
    subprocess.run(['cp', '-a', f'{src}/.', tree.root], check=True)
    prefix = TEMPLATE + '/'
    for line in (full / 'os.manifest').read_text().splitlines():
        mode, uid, gid, path, target = (line.split('\t') + [''])[:5]
        if path.startswith(prefix):
            tree.add(path[len(prefix):], int(mode, 8), int(uid), int(gid), target)
    return src


def add_user(tree, name, password, uid=501):
    salt, iterations = os.urandom(32), 50000
    entropy = hashlib.pbkdf2_hmac('sha512', password.encode(), salt, iterations, 128)
    shadow = plistlib.dumps({'SALTED-SHA512-PBKDF2': {'entropy': entropy, 'iterations': iterations, 'salt': salt}},
                            fmt=plistlib.FMT_BINARY)
    guid = str(uuid.uuid4()).upper()
    tree.write(f'{DSLOCAL}/users/{name}.plist', plistlib.dumps({
        'name': [name], 'realname': [name], 'uid': [str(uid)], 'gid': ['20'], 'home': ['/Users/' + name], 'shell': ['/bin/zsh'],
        'generateduid': [guid], 'passwd': ['********'], 'authentication_authority': [';ShadowHash;HASHLIST:<SALTED-SHA512-PBKDF2>'],
        'ShadowHashData': [shadow], '_writers_passwd': [name], '_writers_realname': [name], '_writers_hint': [name]},
        fmt=plistlib.FMT_BINARY), 0o600)
    for group in ('admin', 'staff'):
        rel = f'{DSLOCAL}/groups/{group}.plist'
        record = plistlib.loads((tree.root / rel).read_bytes())
        record['users'] = [u for u in record.get('users', []) if u != name] + [name]
        record['groupmembers'] = [g for g in record.get('groupmembers', []) if g != guid] + [guid]
        tree.write(rel, plistlib.dumps(record, fmt=plistlib.FMT_BINARY), 0o600)


def add_home(tree, template, name, uid=501, gid=20):
    """createhomedir's result: the user template's Non_localized then English.lproj parts, owned by the user, plus .skipbuddy."""
    home = f'Users/{name}'
    tree.mkdir(home, 0o750, uid, gid)
    for part in ('Non_localized', 'English.lproj'):
        src = template / 'Library/User Template' / part
        if not src.is_dir():
            continue
        subprocess.run(['cp', '-a', f'{src}/.', tree.root / home], check=True)
        for top, dirs, files in os.walk(src):
            for n in dirs + files:
                p = Path(top) / n
                rel = f'{home}/{p.relative_to(src)}'
                st = os.lstat(p)
                target = os.readlink(p) if os.path.islink(p) else ''
                tree.add(rel, st.st_mode, uid, gid, target)
    tree.write(f'{home}/.skipbuddy', b'', 0o644, uid, gid)
    for private in ('Desktop', 'Documents', 'Downloads', 'Library', 'Movies', 'Music', 'Pictures'):
        rel = f'{home}/{private}'
        if rel in tree.entries:
            tree.add(rel, 0o40700, uid, gid)


def kcpassword(password):
    data = password.encode() + b'\0'
    data += bytes(-len(data) % 12)
    return bytes(b ^ KCPASSWORD_KEY[i % len(KCPASSWORD_KEY)] for i, b in enumerate(data))


KEEP = ('private/', 'Users/', 'Library/Preferences/', 'Library/Keychains/')
READONLY = ('Library/Apple', 'Library/Application Support/Apple', 'Library/Documentation', 'Library/WebServer')


def link_template(tree):
    """As UniXNU's make-macos-data-seed.py: files outside KEEP become links to the same file in the root's template (which is on the
    UFS root anyway), and each READONLY bundle directory one link, so the ~0.8 GiB template costs the RAM Data almost nothing and its
    room goes to free space (logd and coresymbolicationd filled 512 MiB within minutes, full-OS boot 11)."""
    root, linked = tree.root, 0
    for rel in READONLY:
        if rel in tree.entries and (root / rel).is_dir():
            uid, gid = tree.owner(rel)
            shutil.rmtree(root / rel)
            for r in [r for r in tree.entries if r.startswith(rel + '/')]:
                del tree.entries[r]
            os.symlink('/' + TEMPLATE + '/' + rel, root / rel)
            tree.add(rel, 0o120755, uid, gid, '/' + TEMPLATE + '/' + rel)
            linked += 1
    for rel, line in list(tree.entries.items()):
        mode = int(line.split('\t')[0], 8)
        if (mode & 0o170000) != 0o100000 or rel.startswith(KEEP) or rel == 'private':
            continue
        uid, gid = tree.owner(rel)
        (root / rel).unlink()
        os.symlink('/' + TEMPLATE + '/' + rel, root / rel)
        tree.add(rel, 0o120755, uid, gid, '/' + TEMPLATE + '/' + rel)
        linked += 1
    print(f'template: {linked} entries linked to /{TEMPLATE}')


def main():
    full, out = Path(sys.argv[1]), Path(sys.argv[2])
    name, password = os.environ.get('MACOS_USER', 'ruan'), os.environ.get('MACOS_PASSWORD', os.environ.get('MACOS_USER', 'ruan'))
    work = full / 'data'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    tree = Tree(work)
    template = copy_template(full, tree)
    # No /cores in Data: its firmlink then stays unset and /cores is the read-only root's empty directory, so a crashing
    # process's core dump fails at once instead of filling the RAM Data volume (a boot task's core covers the whole dyld
    # cache: ENOSPC and 54 s, full-OS boot 3).
    shutil.rmtree(work / 'cores', ignore_errors=True)
    for rel in [r for r in tree.entries if r == 'cores' or r.startswith('cores/')]:
        del tree.entries[rel]

    link_template(tree)
    tree.write('private/var/db/.AppleSetupDone', b'', 0o644)
    # usermanagerd's persona login (uid 501) works in the keybag folders an installer creates; with neither present it aborted right
    # after logging /private/var/db//keybags, PersonaLogin failed and no user session started (darwin-lisa handover-2026-11-11:56-58).
    for d in ('private/var/db/keybags', 'private/var/keybags'):
        if d not in tree.entries:
            tree.mkdir(d, 0o700)
    tree.write(SETUP_SKIP, plistlib.dumps({'SkipSetupItems': SETUP_SKIP_ITEMS}, fmt=plistlib.FMT_XML), 0o644)
    add_user(tree, name, password)
    add_home(tree, template, name)
    tree.write(KEYBOARD_TYPE, plistlib.dumps({'keyboardtype': {}}, fmt=plistlib.FMT_XML), 0o644)
    prefs_path = work / LOGINWINDOW_PREFS
    prefs = plistlib.loads(prefs_path.read_bytes()) if prefs_path.is_file() else {}
    # The build and version of this tree, for the "already set up on this build" keys below.
    sysver = plistlib.loads((full / 'os/System/Library/CoreServices/SystemVersion.plist').read_bytes())
    build, version = sysver['ProductBuildVersion'], sysver['ProductVersion']
    # System loginwindow: auto-login without locking the session at once (no Secure Enclave/keyboard to unlock it: macos-pixel8a),
    # and as a Mac already logged in on this build (macOS-uke macos-skip-setup.py:154-157; UniXNU make-macos-autologin-addon.py:54-57).
    prefs.update(autoLoginUser=name, autoLoginUserScreenLocked=False, OptimizerPreviousBuild=build, lastUserName=name,
                 RecentUsers=[name], AccountInfo={'FirstLogins': {name: True}, 'MaximumUsers': 1, 'OnConsole': {}})
    tree.write(LOGINWINDOW_PREFS, plistlib.dumps(prefs, fmt=plistlib.FMT_BINARY), 0o644)
    # Per user: no MiniBuddy (post-login Setup Assistant). Without these the Dock and Finder wait for it (sheng-macos-link-lab
    # technical-route.md:72; macOS-uke macos-skip-setup.py:125-146, whose keys these are, with this build's values).
    user_prefs = f'Users/{name}/Library/Preferences'
    tree.write(f'{user_prefs}/com.apple.loginwindow.plist', plistlib.dumps({'MiniBuddyLaunch': False}, fmt=plistlib.FMT_BINARY),
               0o600, 501, 20)
    seen = {k: True for k in ('DidSeeCloudSetup', 'DidSeePrivacy', 'DidSeeSiriSetup', 'DidSeeScreenTime', 'DidSeeAppearanceSetup',
                              'DidSeeTouchIDSetup', 'DidSeeAccessibility', 'DidSeeActivationLock', 'DidSeeSyncSetup', 'DidSeeSyncSetup2',
                              'DidSeeIntelligence', 'DidSeeAppStore', 'DidSeeApplePaySetup', 'DidSeeLockdownMode', 'DidSeeTermsOfAddress',
                              'DidSeeiCloudLoginForStorageServices')}
    seen.update({k: build for k in ('InitialSetupBuildVersion', 'LastSeenBuddyBuildVersion', 'PreviousBuildVersion',
                                    'LastPreLoginTasksPerformedBuild')})
    seen.update({k: version for k in ('DidSeeNewFeaturesProductVersion', 'InitialSetupProductVersion', 'PreviousSystemVersion',
                                      'LastPreLoginTasksPerformedVersion', 'LastSeenCloudProductVersion',
                                      'LastSeenDiagnosticsProductVersion', 'LastSeenIntelligenceProductVersion',
                                      'LastSeenSiriProductVersion', 'LastSeenSyncProductVersion',
                                      'LastSeeniCloudStorageServicesProductVersion', 'LastSeenAgeRangeSelectionProductVersion',
                                      'LastLaunchedMiniBuddyForStorageServicesVersion')})
    seen.update(LastPrivacyBundleVersion=2, MiniBuddyLaunchReason=0, MiniBuddyLaunchedPostMigration=False,
                MiniBuddyShouldLaunchToResumeSetup=False, SkipFirstLoginOptimization=True, InitialAccountOnMac=True,
                GPOnboardingHasShown=True)
    # loginwindow runs UserAccountUpdater (the first-login-on-this-build migration, ~19 min on one CPU before Dock and Finder start)
    # unless the per-user loginwindow domain carries this build's stamps; it writes them only after UAU succeeds, and our Data is
    # rebuilt every boot. Encodings from loginwindow (0x1000094e4, 0x10000958c): version (major<<8|minor)<<16, build
    # major<<21 | (letter-'A')<<16 | number<<5 | (suffix-0x60).
    major, minor = (int(x) for x in version.split('.')[:2])
    b = re.fullmatch(r'(\d+)([A-Z])(\d+)([a-z]?)', build)
    build_number = int(b[1]) << 21 | (ord(b[2]) - 65) << 16 | int(b[3]) << 5 | (ord(b[4]) - 0x60 if b[4] else 0)
    if build == '26C5035r':
        assert build_number == 54687090 and (major << 8 | minor) << 16 == 0x1B030000
    tree.write(f'{user_prefs}/loginwindow.plist', plistlib.dumps({
        'SystemVersionStampAsString': version, 'BuildVersionStampAsString': build,
        'SystemVersionStampAsNumber': (major << 8 | minor) << 16, 'BuildVersionStampAsNumber': build_number},
        fmt=plistlib.FMT_BINARY), 0o600, 501, 20)
    tree.write(f'{user_prefs}/com.apple.SetupAssistant.plist', plistlib.dumps(seen, fmt=plistlib.FMT_BINARY), 0o600, 501, 20)
    # Keep the display awake (UniXNU make-macos-autologin-addon.py:116-117, macos-pixel8a): no sleep while nothing can wake it.
    tree.write('Library/LaunchDaemons/org.ruan.desktop-awake.plist', plistlib.dumps(dict(
        Label='org.ruan.desktop-awake', ProgramArguments=['/usr/bin/caffeinate', '-d', '-i', '-s'], RunAtLoad=True, KeepAlive=True),
        fmt=plistlib.FMT_XML), 0o644)
    tree.write('private/etc/kcpassword', kcpassword(password), 0o600)
    disabled_path = work / LAUNCHD_DISABLED
    disabled = plistlib.loads(disabled_path.read_bytes()) if disabled_path.is_file() else {}
    # apsd, mobileactivationd, cryptexd never get past xpcproxy on ruan, and WindowServer's XPC to a launchd-held port waited on
    # one of them (no-darwinbg boot, launchd.log); disabled, a lookup fails at once. As macos-pixel8a and macOS-uke disable them.
    disabled.update({'com.apple.watchdogd': True, 'com.apple.cameracaptured': True, 'com.apple.apsd': True,
                     'com.apple.mobileactivationd': True, 'com.apple.security.cryptexd': True,
                     # launchd holds their ports, so callers wait for daemons that cannot work here (DroidVM product-k27.json:72-81,
                     # stage-gpu-plugin.py:37-41; macos-pixel8a): FairPlay, FSKit, AppSSO.
                     'com.apple.fairplayd': True, 'com.apple.filesystems.fskitd': True, 'com.apple.AppSSODaemon': True,
                     # Installer Progress held the screen ~390 s, then died on SIGILL; bluetoothd exits 1 in a loop (no BT driver)
                     'com.apple.InstallerProgress': True, 'com.apple.bluetoothd': True,
                     # without bluetoothd, coreaudiod retries its BTAudio connection in a tight loop (455k log lines in 18 min on the one
                     # CPU, 2026-11-26) and slows the whole user session; no audio driver yet anyway
                     'com.apple.audio.coreaudiod': True,
                     # then audiomxd took over the spinning (90k log lines in 17 min)
                     'com.apple.audiomxd': True,
                     # waits 300 s for Wi-Fi firmware that this port never loads
                     'com.apple.wifiFirmwareLoader': True,
                     # one CPU: daemons that loop, fail and respawn, or index/sync/analyse (perf agent 2026-11-26, k4/k4c logs)
                     'com.apple.postfixsetup': True,
                     'com.apple.postfix.master': True,
                     'org.cups.cupsd': True,
                     'com.apple.Kerberos.digest-service': True,
                     'com.apple.Kerberos.kdc': True,
                     'com.apple.configureLocalKDC': True,
                     'com.apple.mdmclient.daemon': True,
                     'com.apple.mdmclient.daemon.runatboot': True,
                     'com.apple.ManagedClient': True,
                     'com.apple.ManagedClient.startup': True,
                     'com.apple.ManagedClient.enroll': True,
                     'com.apple.remotemanagementd': True,
                     'com.apple.devicemanagementclient.managedeventsd': True,
                     'com.apple.managedappdistributiond': True,
                     'com.apple.metadata.mds': True,
                     'com.apple.softwareupdated': True,
                     'com.apple.mobile.softwareupdated': True,
                     'com.apple.suhelperd': True,
                     'com.apple.icloud.findmydeviced': True,
                     'com.apple.icloud.searchpartyd': True,
                     'com.apple.findmymacd': True,
                     'com.apple.triald.system': True,
                     'com.apple.analyticsd': True,
                     'com.apple.osanalytics.osanalyticshelper': True,
                     'com.apple.cloudtelemetryd': True,
                     'com.apple.SubmitDiagInfo': True,
                     'com.apple.rtcreportingd': True,
                     'com.apple.systemstats.analysis': True,
                     'com.apple.systemstats.daily': True,
                     'com.apple.tailspind': True,
                     'com.apple.symptomsd-diag': True,
                     'com.apple.PerfPowerServices': True,
                     'com.apple.PerfPowerServicesExtended': True,
                     'com.apple.perfpowermetricd': True,
                     'com.apple.backupd': True,
                     'com.apple.backupd-helper': True,
                     'com.apple.appleh13camerad': True,
                     'com.apple.cmio.videodriverkithostextension': True,
                     'com.apple.cmio.uvcassistantextension': True,
                     'com.apple.cmio.registerassistantservice': True,
                     'com.apple.corecaptured': True,
                     'com.apple.nfcd': True,
                     'com.apple.biometrickitd': True,
                     'com.apple.seld': True,
                     'com.apple.aned': True,
                     'com.apple.aneuserd': True,
                     'com.apple.AirPlayXPCHelper': True,
                     'com.apple.wifip2pd': True,
                     'com.apple.nearbyd': True,
                     'com.apple.accessoryupdaterd': True,
                     'com.apple.uarpd': True,
                     'com.apple.uarpassetmanagerd': True,
                     'com.apple.corespeechd_system': True,
                     'com.apple.modelcatalogd': True,
                     'com.apple.wallpaper.export': True})
    tree.write(LAUNCHD_DISABLED, plistlib.dumps(disabled, fmt=plistlib.FMT_XML), 0o644)
    # The user's heavy agents, which only fill the RAM Data volume and memory here (DroidVM stage-gpu-plugin.py:44,300; pixel8a).
    # gui/501: Siri/intelligence, Photos, cloud, Screen Time, telephony and sync agents (perf agent 2026-11-26)
    user_off = ('com.apple.duetexpertd', 'com.apple.routined', 'com.apple.intelligenceplatformd', 'com.apple.imagent', 'com.apple.knowledge-agent', 'com.apple.knowledgeconstructiond', 'com.apple.BiomeAgent', 'com.apple.biomesyncd', 'com.apple.suggestd', 'com.apple.siriknowledged', 'com.apple.assistantd', 'com.apple.siriactionsd', 'com.apple.corespeechd', 'com.apple.generativeexperiencesd', 'com.apple.intelligenceflowd', 'com.apple.intelligencecontextd', 'com.apple.mlhostd', 'com.apple.ModelCatalogAgent', 'com.apple.voicebankingd', 'com.apple.textunderstandingd', 'com.apple.speechmaintenanced', 'com.apple.speech.speechsynthesisd.arm64', 'com.apple.accessibility.axassetsd', 'com.apple.photolibraryd', 'com.apple.photoanalysisd', 'com.apple.mediaanalysisd', 'com.apple.homed', 'com.apple.CommCenter', 'com.apple.identityservicesd', 'com.apple.telephonyutilities.callservicesd', 'com.apple.CallHistoryPluginHelper', 'com.apple.CallHistorySyncHelper', 'com.apple.sharingd', 'com.apple.rapportd', 'com.apple.bird', 'com.apple.cloudd', 'com.apple.replicatord', 'com.apple.mediastream.mstreamd', 'com.apple.findmy.findmylocateagent', 'com.apple.ScreenTimeAgent', 'com.apple.UsageTrackingAgent', 'com.apple.ScreenTimeSettingsAgent', 'com.apple.financed', 'com.apple.Maps.mapssyncd', 'com.apple.parsecd', 'com.apple.triald', 'com.apple.corespotlightd', 'com.apple.spotlightknowledged.updater', 'com.apple.metadata.mdbulkimport', 'com.apple.managedcorespotlightd', 'com.apple.chronod', 'com.apple.liveactivitiesd', 'com.apple.AMPDeviceDiscoveryAgent', 'com.apple.cmio.ContinuityCaptureAgent', 'com.apple.amsaccountsd', 'com.apple.amsengagementd', 'com.apple.storekitagent', 'com.apple.itunescloudd', 'com.apple.sociallayerd', 'com.apple.SoftwareUpdateNotificationManager', 'com.apple.mdmclient.agent', 'com.apple.followupd', 'com.apple.milod', 'com.apple.mobilerepaird', 'com.apple.askpermissiond', 'com.apple.appplaceholdersyncd')
    tree.write('private/var/db/com.apple.xpc.launchd/disabled.501.plist', plistlib.dumps(
        {k: True for k in user_off}, fmt=plistlib.FMT_XML), 0o644)
    tree.write('private/var/db/com.apple.xpc.launchd/disabled.migrated', b'', 0o644)
    # configd's InterfaceNamer waits for IOKit to go quiet (two entries never do here) before it names a new interface, and
    # clocksyncd, so WindowServer, waits for en0: 240.0 s on every boot (boot-time agent 2026-11-26, *START*/*TIMEOUT* in the log).
    # An interface already in this database is named at once. RuanDummyEth's fixed MAC 02:72:75:61:00:01.
    tree.write('Library/Preferences/SystemConfiguration/NetworkInterfaces.plist', plistlib.dumps({
        'Interfaces': [{
            'BSD Name': 'en0', 'IOBuiltin': True, 'IOInterfaceNamePrefix': 'en', 'IOInterfaceType': 6, 'IOInterfaceUnit': 0,
            'IOMACAddress': bytes.fromhex('027275610001'),
            'IOPathMatch': 'IOService:/ArmDTPlatform/RuanDummyEth/IOEthernetInterface',
            'SCNetworkInterfaceType': 'Ethernet', 'SCNetworkInterfaceInfo': {'UserDefinedName': 'Ethernet'}}]},
        fmt=plistlib.FMT_XML), 0o644)

    # OSLOG_DAEMON=<adrenoprobed>: UniXNU's probe daemon (drivers/AdrenoMetal, built by build-adreno-metal-plugin-ruanmac.py) as a
    # RunAtLoad job; it writes the unified log (every process's os_log lines, WindowServer's included) as gzip text to
    # /private/var/mobile/Library/Caches/com.apple.backboardd/m2v/zz-oslog/, which the host pulls with ruan-usbdbg.py get.
    # Marked arm64e and re-signed like the Metal plugin; its CDHash goes to OUT_DIR/oslog.cdhash for the trust bundle.
    daemon = os.environ.get('OSLOG_DAEMON')
    if daemon:
        rel = 'private/var/ngpu/bin/adrenoprobed'
        data_bytes = bytearray(Path(daemon).read_bytes())
        assert struct.unpack_from('<II', data_bytes, 0) == (0xfeedfacf, 0x100000c), 'expected an arm64 Mach-O'
        struct.pack_into('<I', data_bytes, 8, 0x80000002)
        tree.write(rel, bytes(data_bytes), 0o755)
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, Path(__file__).resolve().parent / 'adhoc-resign.py', work / 'private/var/ngpu/bin',
                        out / 'oslog.cdhash'], check=True)
        tree.write('Library/LaunchDaemons/org.blu-sharky.adrenoprobed.plist', plistlib.dumps(dict(
            Label='org.blu-sharky.adrenoprobed', ProgramArguments=['/' + rel], RunAtLoad=True, KeepAlive=False,
            UserName='root', StandardErrorPath='/dev/null'), fmt=plistlib.FMT_XML), 0o644)
        for d in ('private/var/mobile', 'private/var/mobile/Library', 'private/var/mobile/Library/Caches',
                  'private/var/mobile/Library/Caches/com.apple.backboardd', 'private/var/mobile/Library/Caches/com.apple.backboardd/m2v'):
            if d not in tree.entries:
                tree.mkdir(d, 0o755)
    # The Metal plugin in /Library/GPUBundles (Data): on 27.3 IOGPU's trusted-bundle check refuses it in /System/Library/Extensions
    # alone and WindowServer aborts in WSSystemCanCompositeWithMetal (UniXNU-sheng porting.toml, run64). The root copy
    # (add-macos-metal.py) is already marked arm64e and re-signed, its CDHashes are in the trust cache: copied byte for byte.
    # GPU_BUNDLE=0 leaves it out.
    bundle = full / 'root/System/Library/Extensions/NativeAdrenoMetal.bundle'
    if os.environ.get('GPU_BUNDLE', '1') != '0' and bundle.is_dir():
        for src in sorted(p for p in bundle.rglob('*') if p.is_file()):
            rel = 'Library/GPUBundles/NativeAdrenoMetal.bundle/' + src.relative_to(bundle).as_posix()
            tree.write(rel, src.read_bytes(), 0o755 if src.parent.name == 'MacOS' else 0o644)
        # The publish marker beside the bundle: without it the plugin only runs its self-tests and returns no device (stage 1,
        # NativeAdrenoMetalPlugin.mm:2-5, :720), so WindowServer found no Metal device and aborted after 30 s. A data file: no CDHash.
        tree.write('Library/GPUBundles/NativeAdrenoMetal.publish', b'1\n', 0o644)
        print('Data: NativeAdrenoMetal.bundle + .publish in /Library/GPUBundles')
    manifest = full / 'data.manifest'
    manifest.write_text('\n'.join(tree.entries[p] for p in sorted(tree.entries)) + '\n')
    out.mkdir(parents=True, exist_ok=True)
    data = out / 'macos-data.apfs'          # fixed name: mkapfs.py derives the volume UUID from it
    spec_mr = importlib.util.spec_from_file_location('make_ramroot', W / 'UniXNU-ruan/scripts/make-ramroot.py')
    mr = importlib.util.module_from_spec(spec_mr)
    spec_mr.loader.exec_module(mr)
    slack = int(os.environ.get('DATA_SLACK', 360448))   # 1.375 GiB free for the running system's writes (RAM-backed)
    subprocess.run([sys.executable, '-I', mr.MKAPFS, '-o', data, '--slack', str(slack), f'Data={work},manifest={manifest}'],
                   check=True)
    mr.fix_spaceman(data)
    with open(data, 'rb') as f:
        head = f.read(64 * MIB)
    found = [head[at + 240:at + 256] for at in range(0, len(head), 4096) if head[at + 32:at + 36] == b'APSB']
    volume = str(uuid.UUID(bytes=found[0])).upper()
    (out / 'macos-data.uuid').write_text(volume + '\n')

    usb = out / 'root-usb.apfs'
    with open(usb, 'wb') as o:
        with open(data, 'rb') as d:
            shutil.copyfileobj(d, o, 16 * MIB)
        o.write(bytes(-o.tell() % 4096))
    crc = 0
    with open(usb, 'rb') as s:
        for block in iter(lambda: s.read(16 * MIB), b''):
            crc = zlib.crc32(block, crc)
    total = usb.stat().st_size
    (out / 'root-usb.split').write_text('0\n')
    (out / 'root-usb.desc').write_bytes(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, total, 0))
    print(f'{data}: Data volume {volume}, {len(tree.entries)} entries; {usb}: {total} bytes; user {name}')


if __name__ == '__main__':
    main()
