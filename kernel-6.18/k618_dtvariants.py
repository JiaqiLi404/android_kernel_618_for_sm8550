#!/usr/bin/env python3
"""k618_dtvariants.py <device> <vendor_boot.img> <dtbo.img> <stock module dir,...> <vmlinux> <our module dir>...:
every hardware node of one phone's device tree -- enabled or not -- with the driver that would bind it.

The base tree (kalama v2) comes out of vendor_boot, the phone's overlay out of dtbo.img (the entry
whose model names <device> and whose qcom,msm-id is the phones' 0x207/0x218; the other two entries
per phone are for other SoC parts), merged as the bootloader does (k618_dtcheck.py's helpers). A
node that is disabled in the tree can still be the one a unit uses: a second-source part the
bootloader or a driver's own detection turns on (the 13 Pro's two panels, its two touch buses).
So every compatible counts here, not only enabled ones.

Reports, read-only:
  1. compatibles that a stock 5.15 module binds and nothing in our build does -- enabled or not;
  2. the display panels: id, name, the mi,* switches each sets, and which of those k618_mi_disp.c
     reads at all (a switch nobody reads is a branch the port does not have);
  3. per function, the parts the tree describes (touch, fingerprint, NFC, amplifiers, haptics,
     charging, camera modules, ToF, IR), so a function with more than one part stands out.
"""
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k618_dtcheck as dc  # noqa: E402

FUNCS = [
    ("touch", r"touch|fts|goodix,gt|goodix,brl|focaltech|novatek,nvt|synaptics|himax|ilitek"),
    ("fingerprint", r"fingerprint|goodix,fp|fpc|silead|egis"),
    ("nfc", r"nfc|nxp,sn|nxp,pn|st,st21|st,st54|nq-|qcom,sn-nci"),
    ("audio amp", r"cs35l|aw88|aw87|tfa98|fs16|max98|wsa88|smartpa"),
    ("haptics", r"haptic|vibrator|aw86|aw869"),
    ("charging", r"charger|charge-pump|pump|battery|fuelgauge|fuel-gauge|bq2|ln8|sc85|nu16|rx16|wireless|ds28|maxim|slg|authentic|usb-pd|typec|ucsi"),
    ("camera", r"cam-sensor|qcom,eeprom|qcom,actuator|qcom,ois|camera-flash|cam-cci|tof|stmvl|laser"),
    ("ir", r"ir-spi|ir_spi|ir-blaster|irled"),
    ("hall / sensors", r"hall|sar|semtech|sx93|proximity|ambient|als"),
]


def effective_nodes(dts):
    """(path, [compatibles], enabled) like k618_dtcheck.nodes(), but a node counts as enabled only
    when it and every parent are (a part on a disabled bus is not probed)"""
    out, stack = [], []          # stack: (name, props, parent_enabled)
    for line in dts.splitlines():
        s = line.strip()
        m = re.match(r"(?:[\w-]+:\s*)*([^\s{]+)\s*\{$", s)
        if m:
            parent_on = all(st != "disabled" for _n, p, _o in stack for st in [p.get("status", "okay")])
            stack.append((m.group(1), {}, parent_on))
            continue
        if s.startswith("};"):
            if stack:
                name, props, parent_on = stack.pop()
                if "compatible" in props:
                    path = "/".join(n for n, _p, _o in stack[1:]) + "/" + name
                    own = props.get("status", "okay") in ("okay", "ok")
                    out.append((path, props["compatible"], own and parent_on))
            continue
        if not stack:
            continue
        m = re.match(r'compatible = (.*);$', s)
        if m:
            stack[-1][1]["compatible"] = [c for x in re.findall(r'"([^"]*)"', m.group(1)) for c in x.split("\\0")]
        m = re.match(r'status = "([^"]*)";', s)
        if m:
            stack[-1][1]["status"] = m.group(1)
    return out


def mi_props_read_by_port(src):
    """the mi,* / qcom,* property names k618_mi_disp.c names"""
    text = open(src, encoding="utf-8").read()
    return set(re.findall(r'"((?:mi|qcom),[a-z0-9,._-]+)"', text))


def panels(dts):
    """(node, name, id, {prop: value}) for every mdss_dsi panel with a qcom,mdss-dsi-panel-name
    (the 13 has a second node for its panel, "qcom,global,mdss_dsi_m3_...": prefix-insensitive)"""
    out, stack, cur = [], [], None
    for line in dts.splitlines():
        s = line.strip()
        m = re.match(r"(?:[\w-]+:\s*)*([^\s{]+)\s*\{$", s)
        if m:
            stack.append(m.group(1))
            if len(stack) >= 2 and stack[-2].startswith("qcom,mdss_mdp") and "mdss_dsi_" in stack[-1]:
                cur = {"node": stack[-1], "props": {}}
                out.append(cur)
            continue
        if s.startswith("};"):
            if stack:
                popped = stack.pop()
                if cur is not None and popped == cur["node"]:
                    cur = None
            continue
        if cur is not None and len(stack) >= 1 and stack[-1] == cur["node"]:
            m = re.match(r"((?:mi|qcom),[\w,.+-]+)(?: = (.*))?;$", s)
            if m:
                cur["props"][m.group(1)] = (m.group(2) or "true")[:80]
    return [p for p in out if "qcom,mdss-dsi-panel-name" in p["props"]]


def main(argv):
    device, vb, dtbo, stock, vmlinux, mods = argv[1], argv[2], argv[3], argv[4].split(","), argv[5], argv[6:]
    bases = dc.vendor_boot_dtbs(vb)
    base = None
    for n, b in enumerate(bases):
        p = f"/tmp/k618v_base{n}.dtb"
        open(p, "wb").write(b)
        if base is None and dc.model(dc.dtc(p)).endswith("Kalama v2 SoC"):
            base = p
    ovl = None
    for n, o in enumerate(dc.dtbo_entries(dtbo)):
        p = f"/tmp/k618v_ovl{n}.dtbo"
        open(p, "wb").write(o)
        d = dc.dtc(p)
        if device in dc.model(d).lower() and re.search(r"qcom,msm-id = <0x207 ", d):
            ovl = p
            break
    if not base or not ovl:
        print(f"no kalama v2 base or no {device} entry for 0x207", file=sys.stderr)
        return 1
    merged = f"/tmp/k618v_{device}.dtb"
    subprocess.run([f"{dc.TOOLS}/ufdt_apply_overlay", base, ovl, merged], check=True)
    dts = dc.dtc(merged)
    open(f"/tmp/k618v_{device}.dts", "w").write(dts)
    all_nodes = effective_nodes(dts)
    allc = {c for _p, cs, _on in all_nodes for c in cs}
    ours, stk = dc.aliases(mods, allc), dc.aliases(stock, allc)
    vml = open(vmlinux, "rb").read()
    builtin = {c for c in allc if b"\0" + c.encode() + b"\0" in vml}

    def mine(cs):
        return sorted({m for c in cs for m in ours.get(c, ())}) or (["(built-in)"] if any(c in builtin for c in cs) else [])

    def old(cs):
        return sorted({m for c in cs for m in stk.get(c, ())})

    print(f"== {device}: {dc.model(dts)}; {len(all_nodes)} nodes with a compatible, "
          f"{sum(1 for n in all_nodes if n[2])} enabled")
    print("\n-- 1. bound by a stock module, by nothing of ours (E = enabled, d = disabled)")
    gap = collections.defaultdict(list)
    for path, cs, on in all_nodes:
        o = old(cs)
        if o and not mine(cs):
            gap[(cs[0], ",".join(o))].append(("E" if on else "d") + " " + path)
    for (c, o), paths in sorted(gap.items()):
        print(f"   {c:44s} stock {o:36s} {len(paths)}: {'; '.join(paths[:3])[:150]}")
    if not gap:
        print("   (none)")

    print("\n-- 2. display panels")
    port = mi_props_read_by_port(os.path.join(os.path.dirname(os.path.abspath(__file__)), "k618_mi_disp.c"))
    for p in panels(dts):
        pr = p["props"]
        switches = sorted(k for k, v in pr.items() if k.startswith("mi,") and not k.endswith(("-command", "-command-state", "-command-update")))
        unread = [k for k in switches if k not in port]
        print(f"   {p['node']}: {pr.get('qcom,mdss-dsi-panel-name', '?')}, id {pr.get('mi,panel-id', '-')}")
        print(f"      mi,* settings {len(switches)}, not read by k618_mi_disp.c {len(unread)}: {' '.join(unread)[:600]}")

    print("\n-- 3. parts per function (E/d, compatible, ours | stock, node)")
    for name, rx in FUNCS:
        rows = [(on, cs, path) for path, cs, on in all_nodes if re.search(rx, " ".join(cs) + " " + path.split("/")[-1], re.I)]
        if not rows:
            continue
        print(f"   [{name}]")
        seen = set()
        for on, cs, path in rows:
            key = (on, cs[0], path)
            if key in seen:
                continue
            seen.add(key)
            print(f"      {'E' if on else 'd'} {cs[0]:34s} {','.join(mine(cs)) or '-':26s} | {','.join(old(cs)) or '-':26s} {path[-70:]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
