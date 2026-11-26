# ADSP sensors → auto-rotate (macOS 27.3, ruan)

Nothing built or booted; stage (a) objects compile with the macOS tree's flags (`check/syntax-check.py`).

## 1. SM7325 (lisa) → SM7435 (ruan)

Unchanged from lisa: PAS 1, SMEM 0x80900000, mutex 3, SMP2P 443/429 (pid 2), AOSS, SSC = QMI 400, FastRPC stream 0x1805 (shared-cb 5, IOVA 0x80000000).

What changes on ruan (live DT):
- **Carveout:** 0x9a200000+0x2900000. adsp.mdt spans exactly that and is relocatable.
- **IPCC:** 0xed18000 (client 3).
- **RSC:** 0x17a20000+0xd00, TCS 1. LCX/LMX vote 384.
- **GLINK channels:** IPCRTR, `fastrpcglink-apps-dsp`, `adsp_apps` (GPR, not APR).
- **Firmware:** `modem_a:image/adsp.b00..b42`, read in place through NativeUfs, never embedded. There is also an `adsp2.*` set; fuse 0x221c8117[7:2]==4 selects it.
- **Registry:** `sns_reg_config` v3 plus 57 JSON from `vendor_a/etc/sensors`, served from the root after it mounts. Answer `QRD`, a listed soc_id and hwname `ruan`.

**IMU:** a Bosch **BMI3x0** (Xiaomi's factory list) or a TDK **ICM‑4x6xx**, both on I3C bus 1 at 0x68 with irq 64. The DSU logcat names both, so the part is unconfirmed. The host reads generic `accel`; the part only changes the axis map.

## 2. Stages

- **(a) PAS boot (this patch).** Survey, QMP `load_state`, rail vote, init_image → mem_setup → segments → auth_and_reset, then SMP2P ready. Every SMC holds `hakios_spmi_lock`, as the GPU zap's `scmCall` does. To pass, USB `adsp` must show `st 9`, slave bit 1 set, and `fatal 0` for 2 minutes.
- **(b) GLINK + QRTR.** Use iPadOS `QcomGlink.c` and `QcomQrtr.c`. To pass, service 400 must be listed.
- **(c) SSC client + FastRPC file server.** Port `dlisa.ssc.c`, and the SMMU context, attach and `apps_std` server from `dlisa.fastrpc*`. Writes stay in RAM; lisa's SSC streamed only with this server.
- **(d) Orientation.** Decode the floats to integers, since the kernel has no FPU. Ignore the device when it lies flat. Switch quadrant only 60° past the current centre and only after the reading holds for 400 ms. Then set `native_legacy_rotate` and call the display re-announce (weak import). RuanTouch must read the same global. `ruan-rotate-fake=S` lets the hook be tested without the DSP.

## 3. Unknown / risky

- **__TEXT:** stage (a) takes 19.6 KiB at -Os. Stages (b) and (c) are estimated at 30–40 KiB more, which is over the ~50 KiB margin. Proposal: keep the transport in the kernel and move SSC and the file server into a userspace daemon.
- **PAS order under Gunyah:** the zap reset the SoC with Linux's order; the WPSS works with it. `ruan-adsp-order=1` writes the segments first.
- **adsp vs adsp2:** guessing wrong means auth fails. `ruan-adsp-fuse=1` reads the fuse, which could cause an external abort. `ruan-adsp-img=2` forces adsp2.
- **Not served or untested:** the PD mapper is not served (lisa worked without it). A DSP running without GLINK has never been seen. There is no orderly stop.
- **RSC race:** RuanTouch also read-modify-writes the RSC IRQ_ENABLE register.
- **Patch target:** the patch is for `/root/unixnu-work/UniXNU-ruan-macos` (dry-run clean). Against the older git copy, 4 hunks fail.

Boot args, all off by default: `ruan-adsp=1|2` plus `-delay -order -img -fuse -fw`.
