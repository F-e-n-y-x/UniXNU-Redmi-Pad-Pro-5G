# Device tree inventory: Qualcomm Technologies, Inc. Parrot QRD, RUAN based on SM7435

- compatible: `qcom,parrot-qrd qcom,parrot qcom,qrd`
- qcom,msm-id: `0x27a 0x10000 0x277 0x10000 0x247 0x10000 0x219 0x10000 0x265 0x10000 0x279 0x10000 0x27e 0x10000`  board-id: `0x3000b 0x1`
- panel (bootargs): `qcom,mdss_dsi_n83_35_02_0a_wqxga_video_cphy`

## RAM banks (/memory)

- 0x080000000 .. 0x0e8e00000  (1678 MiB)
- 0x0ea700000 .. 0x100000000  (345 MiB)
- 0x800000000 .. 0x83fc00000  (1020 MiB)
- 0x880000000 .. 0x8b9b00000  (923 MiB)
- 0x840000000 .. 0x880000000  (1024 MiB)
- 0x8c0000000 .. 0x980000000  (3072 MiB)
- 0x8ba100000 .. 0x8bbf00000  (30 MiB)
- total 8092 MiB

## Reserved memory (no-map marked *)

- * hyp_region@80000000: 0x80000000+0x600000
- * xbl_dtlog_region@80600000: 0x80600000+0x40000
- * xbl_ramdump_region@80640000: 0x80640000+0x1c0000
- * aop_image_region@80800000: 0x80800000+0x60000
- * aop_cmd_db_region@80860000: 0x80860000+0x20000
- * aop_config_region@80880000: 0x80880000+0x20000
- * tme_crash_dump_region@808a0000: 0x808a0000+0x40000
- * tme_log_region@808e0000: 0x808e0000+0x4000
- * uefi_log_region@808e4000: 0x808e4000+0x10000
- * smem_region@80900000: 0x80900000+0x200000
- * cpucp_fw_region@80b00000: 0x80b00000+0x100000
- * chipinfo_region@80c00000: 0x80c00000+0x1000
- * wlan_fw_region@82a00000: 0x82a00000+0xc00000
- * camera_region@84b00000: 0x84b00000+0x800000
- * wpss_region@85300000: 0x85300000+0x1900000
- * video_region@86c00000: 0x86c00000+0x700000
- * adsp_region@9a200000: 0x9a200000+0x2900000
- * cdsp_region@89400000: 0x89400000+0xa00000
- * ipa_fw_region@89e00000: 0x89e00000+0x10000
- * ipa_gsi_region@89e10000: 0x89e10000+0xa000
- * gpu_microcode_region@89e1a000: 0x89e1a000+0x2000
- * mpss_region@8bc00000: 0x8bc00000+0xe600000
- * xbl_sc_region@a6e00000: 0xa6e00000+0x40000
- * global_sync_region@a6f00000: 0xa6f00000+0x100000
- * cpusys_vm_region@e0600000: 0xe0600000+0x400000
- * trust_ui_vm_region@e0b00000: 0xe0b00000+0x100000
- * trust_ui_vm_qrtr@e55f3000: 0xe55f3000+0x9000
- * trust_ui_vm_vblk0_ring@e55fc000: 0xe55fc000+0x4000
- * trust_ui_vm_swiotlb@e5600000: 0xe5600000+0x100000
- * tz_stat_region@e8800000: 0xe8800000+0x100000
- * tags_region@e8900000: 0xe8900000+0x680000
- * qtee_region@e8f80000: 0xe8f80000+0x500000
- * trusted_apps_region@e9480000: 0xe9480000+0x1200000
-   ramoops@80D00000: 0x80d00000+0x200000
-   splash_region: 0xb8000000+0x2b00000

## CPUs

- cpu@0: MPIDR 0x0 0x0, qcom,kryo, enable-method psci, capacity [1024]
- cpu@100: MPIDR 0x0 0x100, qcom,kryo, enable-method psci, capacity [1024]
- cpu@200: MPIDR 0x0 0x200, qcom,kryo, enable-method psci, capacity [1024]
- cpu@300: MPIDR 0x0 0x300, qcom,kryo, enable-method psci, capacity [1024]
- cpu@400: MPIDR 0x0 0x400, qcom,kryo, enable-method psci, capacity [1945]
- cpu@500: MPIDR 0x0 0x500, qcom,kryo, enable-method psci, capacity [1945]
- cpu@600: MPIDR 0x0 0x600, qcom,kryo, enable-method psci, capacity [1945]
- cpu@700: MPIDR 0x0 0x700, qcom,kryo, enable-method psci, capacity [1945]

## Controllers

| Block | Node | compatible | reg |
|---|---|---|---|
| GIC (arm,gic-v3) | `/soc/interrupt-controller@17200000` | `arm,gic-v3` | 0x17200000+0x10000, 0x17260000+0x100000 |
| Arch timer | `/soc/timer` | `arm,armv8-timer` | - |
| PSCI | `/soc/psci` | `arm,psci-1.0` | - |
| RPMh RSC (apps) | `/soc/rsc@17a00000` | `qcom,rpmh-rsc` | 0x17a00000+0x10000, 0x17a10000+0x10000, 0x17a20000+0x10000 |
| RPMh RSC (apps) | `/soc/rsc@af20000` | `qcom,rpmh-rsc` | 0xaf20000+0x10000 |
| TLMM pinctrl | `/soc/pinctrl@f000000` | `qcom,parrot-pinctrl` | 0xf000000+0x1000000 |
| TLMM pinctrl | `/soc/spf_core_platform/lpi_pinctrl@3440000` | `qcom,lpi-pinctrl` | - |
| TLMM pinctrl | `/soc/spf_core_platform/fm_i2s0_pinctrl` | `qcom,msm-cdc-pinctrl` | - |
| TLMM pinctrl | `/soc/spf_core_platform/wsa_swr_clk_data_pinctrl` | `qcom,msm-cdc-pinctrl` | - |
| TLMM pinctrl | `/soc/spf_core_platform/wsa2_swr_clk_data_pinctrl` | `qcom,msm-cdc-pinctrl` | - |
| TLMM pinctrl | `/soc/spf_core_platform/rx_swr_clk_data_pinctrl` | `qcom,msm-cdc-pinctrl` | - |
| SPMI PMIC arbiter | `/soc/qcom,spmi@c42d000` | `qcom,spmi-pmic-arb` | 0xc42d000+0x4000, 0xc400000+0x3000, 0xc500000+0x400000, 0xc440000+0x80000, 0xc4c0000+0x10000 |
| SPMI PMIC arbiter | `/soc/qcom,spmi@c432000` | `qcom,spmi-pmic-arb` | 0xc432000+0x4000, 0xc400000+0x3000, 0xc500000+0x400000, 0xc440000+0x80000, 0xc4d0000+0x10000 |
| SPMI PMIC arbiter | `/soc/qcom,spmi-debug@10b14000` | `qcom,spmi-pmic-arb-debug` | 0x10b14000+0x60, 0x221c8784+0x4 |
| Command DB | `/reserved-memory/aop_cmd_db_region@80860000` | `qcom,cmd-db` | 0x80860000+0x20000 |
| SMEM | `/soc/qcom,smem` | `qcom,smem` | - |
| IMEM | `/soc/qcom,msm-imem@146aa000` | `qcom,msm-imem` | - |
| IMEM | `/soc/qcom,msm-imem@146aa000/mem_dump_table@10` | `qcom,msm-imem-mem_dump_table` | 0x10+0x8 |
| IMEM | `/soc/qcom,msm-imem@146aa000/restart_reason@65c` | `qcom,msm-imem-restart_reason` | 0x65c+0x4 |
| IMEM | `/soc/qcom,msm-imem@146aa000/dload_type@1c` | `qcom,msm-imem-dload-type` | 0x1c+0x4 |
| IMEM | `/soc/qcom,msm-imem@146aa000/boot_stats@6b0` | `qcom,msm-imem-boot_stats` | 0x6b0+0x20 |
| IMEM | `/soc/qcom,msm-imem@146aa000/kaslr_offset@6d0` | `qcom,msm-imem-kaslr_offset` | 0x6d0+0xc |
| UFS host | `/soc/ufshc@1d84000` | `qcom,ufshc` | 0x1d84000+0x3000, 0x1d88000+0x8000, 0x1d90000+0x9000 |
| GPU (KGSL) | `/soc/qcom,kgsl-3d0@3d00000` | `qcom,adreno-gpu-gen7-3-0 qcom,kgsl-3d0` | 0x3d00000+0x40000, 0x3d61000+0x800, 0x3d50000+0x10000, 0x3d9e000+0x1000, 0x10900000+0x80000 |
| GMU | not found | | |
| Display MDP | `/soc/qcom,mdss_mdp@ae00000` | `qcom,sde-kms` | 0xae00000+0x84000, 0xaeb0000+0x2008, 0xaeac000+0x800 |
| DSI ctrl | `/soc/qcom,mdss_dsi_ctrl0@ae94000` | `qcom,dsi-ctrl-hw-v2.6` | 0xae94000+0x400, 0xaf0f000+0x4, 0xae36000+0x300 |
| USB DWC3 | `/soc/ssusb@a600000` | `qcom,dwc-usb3-msm` | 0xa600000+0x100000 |
| USB DWC3 | `/soc/ssusb@a600000/dwc3@a600000` | `snps,dwc3` | 0xa600000+0xd800 |
| SMMU (apps) | `/soc/kgsl-smmu@3da0000` | `qcom,qsmmu-v500 qcom,adreno-smmu` | 0x3da0000+0x40000, 0x3de6000+0x20 |
| SMMU (apps) | `/soc/apps-smmu@15000000` | `qcom,qsmmu-v500` | 0x15000000+0x100000, 0x151da000+0x20 |
| Wi-Fi (icnss2) | `/soc/bt_wcn6750` | `qcom,wcn6750-bt` | - |
| Wi-Fi (icnss2) | `/soc/qcom,wcn6750` | `qcom,wcn6750` | 0x17210040+0x0, 0xb0000000+0x10000 |
| Remoteproc (PIL) | `/soc/remoteproc-adsp@03000000` | `qcom,parrot-adsp-pas` | 0x3000000+0x10000 |
| Remoteproc (PIL) | `/soc/remoteproc-cdsp@32300000` | `qcom,parrot-cdsp-pas` | 0x32300000+0x10000 |
| Remoteproc (PIL) | `/soc/remoteproc-mss@04080000` | `qcom,parrot-modem-pas` | 0x4080000+0x10000 |
| Remoteproc (PIL) | `/soc/remoteproc-wpss@8a00000` | `qcom,parrot-wpss-pas` | 0x8a00000+0x10000 |
| Remoteproc (PIL) | `/soc/qcom,ipa_fws` | `qcom,pil-tz-generic` | - |
| Hypervisor | `/hypervisor` | `qcom,gunyah-hypervisor-1.0 qcom,gunyah-hypervisor simple-bus` | - |
| Hypervisor | `/hypervisor/qcom,gunyah-vm` | `qcom,gunyah-vm-id-1.0 qcom,gunyah-vm-id` | - |
| Hypervisor | `/hypervisor/qcom,resource-manager-rpc@dc42fb3d0a6eda44` | `qcom,resource-manager-1-0 qcom,resource-manager qcom,gunyah-` | 0xdc42fb3d0a6eda44+0x0, 0xdc42fb3d0a6e237d+0x0 |

## GPU details

- chipid , gpu-model `Adreno710v1`

## Touch / input candidates (SPI or I2C children with touch in name or compatible)

- `/soc/spi@a8c000/touch@0`  compatible `novatek,NVT-ts-spi`  status ok
- `/soc/spi@a8c000/touch@0/novatek-mp-criteria-5983@0`  compatible `novatek-mp-criteria-5983`  status okay
- `/soc/spi@a8c000/touch@0/novatek-mp-criteria-5986@0`  compatible `novatek-mp-criteria-5986`  status okay
- `/soc/xiaomi_touch`  compatible `xiaomi-touch`  status okay

## Panel nodes matching the bootargs panel

- `/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_n83_35_02_0a_wqxga_video_cphy`
  - qcom,mdss-dsi-panel-name: `xiaomi n83 35 02 0a cphy nt36532 boe`
  - qcom,mdss-dsi-panel-type: `dsi_video_mode`
  - mode timing@0: 1600x2560 @ 60 Hz
