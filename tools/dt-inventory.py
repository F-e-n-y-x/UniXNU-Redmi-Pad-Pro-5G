#!/usr/bin/env python3
"""Hardware inventory of a Qualcomm Android device tree, for XNU porting (UniXNU DTS / HakiOS soc.args).

Usage: dt-inventory.py <live.dtb>   (get it with: adb exec-out su -c 'cat /sys/firmware/fdt' > live.dtb)
Needs the `fdt` package (pip install fdt).
"""
import re
import subprocess
import sys

import fdt


def cells(node, name):
    p = node.get_property(name)
    return list(p.data) if p is not None and hasattr(p, "data") else []


def string(node, name):
    p = node.get_property(name)
    if p is None:
        return ""
    return " ".join(p.data) if hasattr(p, "data") and isinstance(p.data, list) and p.data and isinstance(p.data[0], str) else str(p.value)


def regs(node):
    par = PARENT.get(id(node))
    ac = (cells(par, "#address-cells") or [2])[0] if par is not None else 2
    sc = (cells(par, "#size-cells") or [1])[0] if par is not None else 1
    c = cells(node, "reg")
    out, step = [], ac + sc
    for i in range(0, len(c) - step + 1, step):
        addr = 0
        for x in c[i:i + ac]:
            addr = (addr << 32) | x
        size = 0
        for x in c[i + ac:i + step]:
            size = (size << 32) | x
        out.append((addr, size))
    return out


def fmt(rs):
    return ", ".join(f"0x{a:x}+0x{s:x}" for a, s in rs)


PARENT = {}


def walk(node, path="", parent=None):
    p = f"{path}/{node.name}" if node.name != "/" else ""
    PARENT[id(node)] = parent
    yield p or "/", node
    for n in node.nodes:
        yield from walk(n, p, node)


def main(path):
    # Live /sys/firmware/fdt has FDT_NOP tags the fdt package rejects; dtc re-encodes without them.
    blob = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dtb", path], check=True, capture_output=True).stdout
    t = fdt.parse_dtb(blob)
    root = t.get_node("/")
    nodes = list(walk(root))
    by_compat = lambda pat: [(p, n) for p, n in nodes if re.search(pat, string(n, "compatible"))]
    out = []
    w = out.append

    w(f"# Device tree inventory: {string(root, 'model')}\n")
    w(f"- compatible: `{string(root, 'compatible')}`")
    w(f"- qcom,msm-id: `{' '.join(hex(x) for x in cells(root, 'qcom,msm-id'))}`  board-id: `{' '.join(hex(x) for x in cells(root, 'qcom,board-id'))}`")
    m = re.search(r"msm_drm\.dsi_display0=([^ :]+)", string(t.get_node('/chosen'), "bootargs"))
    w(f"- panel (bootargs): `{m.group(1) if m else '?'}`\n")

    w("## RAM banks (/memory)\n")
    total = 0
    for a, s in regs(t.get_node("/memory")):  # parent is / (2+2 cells)
        if s:
            total += s
            w(f"- 0x{a:09x} .. 0x{a + s:09x}  ({s >> 20} MiB)")
    w(f"- total {total >> 20} MiB\n")

    w("## Reserved memory (no-map marked *)\n")
    for n in t.get_node("/reserved-memory").nodes:
        r = regs(n)
        if r:
            w(f"- {'*' if n.get_property('no-map') is not None else ' '} {n.name}: {fmt(r)}")
    w("")

    w("## CPUs\n")
    for n in t.get_node("/cpus").nodes:
        if n.name.startswith("cpu@"):
            w(f"- {n.name}: MPIDR {' '.join(hex(x) for x in cells(n, 'reg'))}, {string(n, 'compatible')}, "
              f"enable-method {string(n, 'enable-method')}, capacity {cells(n, 'capacity-dmips-mhz')}")
    w("")

    want = [
        ("GIC (arm,gic-v3)", r"arm,gic-v3"),
        ("Arch timer", r"arm,armv8-timer"),
        ("PSCI", r"arm,psci"),
        ("RPMh RSC (apps)", r"qcom,rpmh-rsc"),
        ("TLMM pinctrl", r"qcom,.*-pinctrl|qcom,parrot-pinctrl"),
        ("SPMI PMIC arbiter", r"qcom,spmi-pmic-arb"),
        ("Command DB", r"qcom,cmd-db"),
        ("SMEM", r"qcom,smem"),
        ("IMEM", r"qcom,msm-imem"),
        ("UFS host", r"qcom,ufshc"),
        ("GPU (KGSL)", r"qcom,kgsl-3d0"),
        ("GMU", r"qcom,gpu-gmu"),
        ("Display MDP", r"qcom,sde-kms"),
        ("DSI ctrl", r"qcom,dsi-ctrl-hw"),
        ("USB DWC3", r"qcom,dwc-usb3-msm|snps,dwc3"),
        ("SMMU (apps)", r"qcom,qsmmu-v500|arm,mmu-500"),
        ("Wi-Fi (icnss2)", r"qcom,wcn6750|qcom,icnss"),
        ("Remoteproc (PIL)", r"qcom,pil-tz-generic|qcom,.*-pas"),
        ("Hypervisor", r"qcom,gunyah|qcom,haven|gunyah-hypervisor"),
    ]
    w("## Controllers\n")
    w("| Block | Node | compatible | reg |")
    w("|---|---|---|---|")
    for label, pat in want:
        hits = by_compat(pat)
        for p, n in hits[:6]:
            w(f"| {label} | `{p}` | `{string(n, 'compatible')[:60]}` | {fmt(regs(n)) or '-'} |")
        if not hits:
            w(f"| {label} | not found | | |")
    w("")

    w("## GPU details\n")
    for p, n in by_compat(r"qcom,kgsl-3d0"):
        w(f"- chipid {' '.join(hex(x) for x in cells(n, 'qcom,chipid'))}, gpu-model `{string(n, 'qcom,gpu-model')}`")
        for pl in n.nodes:
            if pl.name.startswith("qcom,gpu-pwrlevels"):
                freqs = sorted({cells(l, 'qcom,gpu-freq')[0] // 1000000 for l in pl.nodes if cells(l, 'qcom,gpu-freq')})
                if freqs:
                    w(f"- {pl.name}: {freqs} MHz")
    w("")

    w("## Touch / input candidates (SPI or I2C children with touch in name or compatible)\n")
    for p, n in nodes:
        c = string(n, "compatible")
        if re.search(r"touch|goodix|novatek|focaltech|synaptics", c.lower()) and "mdss" not in p:
            st = string(n, "status") or "okay"
            w(f"- `{p}`  compatible `{c[:70]}`  status {st}")
    w("")

    w("## Panel nodes matching the bootargs panel\n")
    if m:
        for p, n in nodes:
            if n.name == m.group(1) or p.endswith("/" + m.group(1)):
                w(f"- `{p}`")
                for k in ("qcom,mdss-dsi-panel-name", "qcom,mdss-dsi-panel-type", "qcom,mdss-dsi-panel-physical-type"):
                    if n.get_property(k) is not None:
                        w(f"  - {k}: `{string(n, k)}`")
                for tm in n.nodes:
                    for mode in tm.nodes:
                        wd, ht = cells(mode, "qcom,mdss-dsi-panel-width"), cells(mode, "qcom,mdss-dsi-panel-height")
                        fr = cells(mode, "qcom,mdss-dsi-panel-framerate")
                        if wd:
                            w(f"  - mode {mode.name}: {wd[0]}x{ht[0]} @ {fr[0] if fr else '?'} Hz")
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
