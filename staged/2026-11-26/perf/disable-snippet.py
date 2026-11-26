# --- add inside the disabled.update({...}) call in make-macos-data.py (system domain) ---
                     # postfixsetup: one bash job forking bind_unix_socket/chmod/mkfifo, ran 694 s pre-login (k4c)
                     'com.apple.postfixsetup': True, 'com.apple.postfix.master': True, 'org.cups.cupsd': True,
                     # Kerberos digest-service exits 1 and is respawned 30x per boot; local KDC unused
                     'com.apple.Kerberos.digest-service': True, 'com.apple.Kerberos.kdc': True, 'com.apple.configureLocalKDC': True,
                     # MDM stack: mdmclient looks up apsd/mobileactivationd in a loop; remotemanagementd spawns 16 subscribers
                     'com.apple.mdmclient.daemon': True, 'com.apple.mdmclient.daemon.runatboot': True, 'com.apple.ManagedClient': True,
                     'com.apple.ManagedClient.startup': True, 'com.apple.ManagedClient.enroll': True, 'com.apple.remotemanagementd': True,
                     'com.apple.devicemanagementclient.managedeventsd': True, 'com.apple.managedappdistributiond': True,
                     # Spotlight, updates, Find My, telemetry, Time Machine
                     'com.apple.metadata.mds': True, 'com.apple.softwareupdated': True, 'com.apple.mobile.softwareupdated': True,
                     'com.apple.suhelperd': True, 'com.apple.icloud.findmydeviced': True, 'com.apple.icloud.searchpartyd': True,
                     'com.apple.findmymacd': True, 'com.apple.triald.system': True, 'com.apple.analyticsd': True,
                     'com.apple.osanalytics.osanalyticshelper': True, 'com.apple.cloudtelemetryd': True, 'com.apple.SubmitDiagInfo': True,
                     'com.apple.rtcreportingd': True, 'com.apple.systemstats.analysis': True, 'com.apple.systemstats.daily': True,
                     'com.apple.tailspind': True, 'com.apple.symptomsd-diag': True, 'com.apple.PerfPowerServices': True,
                     'com.apple.PerfPowerServicesExtended': True, 'com.apple.perfpowermetricd': True,
                     'com.apple.backupd': True, 'com.apple.backupd-helper': True, 'com.apple.coreduetd': True, 'com.apple.contextstored': True,
                     # hardware that is not there: camera, NFC, Touch ID, ANE, AirPlay, Wi-Fi P2P, accessory firmware, speech
                     'com.apple.appleh13camerad': True, 'com.apple.cmio.videodriverkithostextension': True,
                     'com.apple.cmio.uvcassistantextension': True, 'com.apple.cmio.registerassistantservice': True, 'com.apple.corecaptured': True,
                     'com.apple.nfcd': True, 'com.apple.biometrickitd': True, 'com.apple.seld': True, 'com.apple.aned': True, 'com.apple.aneuserd': True,
                     'com.apple.AirPlayXPCHelper': True, 'com.apple.wifip2pd': True, 'com.apple.nearbyd': True, 'com.apple.accessoryupdaterd': True,
                     'com.apple.uarpd': True, 'com.apple.uarpassetmanagerd': True, 'com.apple.corespeechd_system': True,
                     'com.apple.modelcatalogd': True, 'com.apple.eligibilityd': True, 'com.apple.wallpaper.export': True,
                     # medium risk (test separately): mediaremoted spams 1475 HALC lines without coreaudiod; locationd 2k lines
                     'com.apple.mediaremoted': True, 'com.apple.locationd': True,

# --- replace the disabled.501.plist dict (user domain gui/501) ---
    user_off = ('com.apple.duetexpertd', 'com.apple.routined', 'com.apple.intelligenceplatformd', 'com.apple.imagent',
                'com.apple.knowledge-agent', 'com.apple.knowledgeconstructiond', 'com.apple.BiomeAgent', 'com.apple.biomesyncd',
                'com.apple.suggestd', 'com.apple.siriknowledged', 'com.apple.assistantd', 'com.apple.siriactionsd', 'com.apple.corespeechd',
                'com.apple.generativeexperiencesd', 'com.apple.intelligenceflowd', 'com.apple.intelligencecontextd', 'com.apple.mlhostd',
                'com.apple.ModelCatalogAgent', 'com.apple.voicebankingd', 'com.apple.textunderstandingd', 'com.apple.speechmaintenanced',
                'com.apple.speech.speechsynthesisd.arm64', 'com.apple.accessibility.axassetsd',
                'com.apple.photolibraryd', 'com.apple.photoanalysisd', 'com.apple.mediaanalysisd', 'com.apple.homed', 'com.apple.CommCenter',
                'com.apple.identityservicesd', 'com.apple.telephonyutilities.callservicesd', 'com.apple.CallHistoryPluginHelper',
                'com.apple.CallHistorySyncHelper', 'com.apple.sharingd', 'com.apple.rapportd', 'com.apple.bird', 'com.apple.cloudd',
                'com.apple.replicatord', 'com.apple.mediastream.mstreamd', 'com.apple.findmy.findmylocateagent', 'com.apple.ScreenTimeAgent',
                'com.apple.UsageTrackingAgent', 'com.apple.ScreenTimeSettingsAgent', 'com.apple.financed', 'com.apple.Maps.mapssyncd',
                'com.apple.parsecd', 'com.apple.triald', 'com.apple.corespotlightd', 'com.apple.spotlightknowledged.updater',
                'com.apple.metadata.mdbulkimport', 'com.apple.managedcorespotlightd', 'com.apple.chronod', 'com.apple.liveactivitiesd',
                'com.apple.AMPDeviceDiscoveryAgent', 'com.apple.cmio.ContinuityCaptureAgent', 'com.apple.amsaccountsd',
                'com.apple.amsengagementd', 'com.apple.storekitagent', 'com.apple.itunescloudd', 'com.apple.sociallayerd',
                'com.apple.SoftwareUpdateNotificationManager', 'com.apple.mdmclient.agent', 'com.apple.ContextStoreAgent',
                'com.apple.followupd', 'com.apple.milod', 'com.apple.mobilerepaird', 'com.apple.askpermissiond', 'com.apple.appplaceholdersyncd')
    tree.write('private/var/db/com.apple.xpc.launchd/disabled.501.plist', plistlib.dumps(
        {k: True for k in user_off}, fmt=plistlib.FMT_XML), 0o644)
