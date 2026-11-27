  FILE FREEFORM = A1E235DE-E825-4591-9623-C43175811826 {
    SECTION UI = "SecParti.cfg"
    SECTION RAW = Binaries/ruan/RawFiles/SecParti.cfg
  }

  FILE FREEFORM = 21E9BDD9-6C3F-4F10-84A5-BBEC322741F1 {
    SECTION UI = "uefipil.cfg"
    SECTION RAW = Binaries/ruan/RawFiles/uefipil.cfg
  }

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  FILE FREEFORM = FFC4DC0D-0A9C-4BBA-87CB-511B22C55B62 {
    SECTION UI = "Panel_N83_35_02_0a_wqxga_video_cphy.xml"
    SECTION RAW = Binaries/ruan/RawFiles/Panel_N83_35_02_0a_wqxga_video_cphy.xml
  }

  FILE FREEFORM = 3D86EF1C-AC95-479C-906F-FACC9FA93CDD {
    SECTION UI = "Panel_N83_42_02_0b_wqxga_video_cphy.xml"
    SECTION RAW = Binaries/ruan/RawFiles/Panel_N83_42_02_0b_wqxga_video_cphy.xml
  }
!endif

  FILE FREEFORM = 45FE4B7C-150C-45DA-A021-4BEB2048EC6F {
    SECTION UI = "QcomChargerCfg.cfg"
    SECTION RAW = Binaries/ruan/RawFiles/QcomChargerCfg.cfg
  }
