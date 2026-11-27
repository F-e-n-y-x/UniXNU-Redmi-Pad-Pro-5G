APRIORI DXE {
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF QcomPkg/Drivers/IortDtDxe/IortDtDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/ruan/QcomPkg/Drivers/TzDxe/ScmDxeLA.inf
  INF Binaries/ruan/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf

  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UFSDxe/UFSDxe.inf

  INF FatPkg/EnhancedFatDxe/Fat.inf

  INF Binaries/ruan/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/ruan/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/ruan/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/ruan/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/ruan/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/ruan/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/ruan/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/UCDxe/UCDxe.inf
  INF Binaries/ruan/QcomPkg/Drivers/RNGDxe/RngDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
}
