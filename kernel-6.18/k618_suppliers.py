#!/usr/bin/env python3
"""k618_suppliers.py <merged .dts> <compatible> <module dir>...: the suppliers of the device tree
nodes with the given compatible (clocks, resets, power domains, interconnects, PHYs, IOMMUs,
regulators, pinctrl, mailboxes...), and whether a module of ours has each supplier's compatible.
With fw_devlink a node waits for every supplier's driver, so one missing supplier holds a device
back (until deferred_probe_timeout): this lists them, recursively. The .dts is k618_dtcheck.py's
merged tree (/tmp/k618_dt.dts)."""

import os
import re
import sys

PH_PROPS = re.compile(r"^(clocks|resets|power-domains|interconnects|phys|iommus|mboxes|"
                      r"pinctrl-\d+|[\w,-]+-supply|qcom,[\w-]*(?:phandle|syscon)|interrupts-extended|"
                      r"memory-region|nvmem-cells|thermal-sensors|dmas)$")


def parse(dts):
    nodes, stack, by_ph = [], [], {}
    for line in open(dts, encoding="utf-8", errors="replace"):
        s = line.strip()
        m = re.match(r"(?:[\w-]+:\s*)*([^\s{]+)\s*\{$", s)
        if m:
            stack.append({"name": m.group(1), "props": {}, "path": "/".join(n["name"] for n in stack[1:]) + "/" + m.group(1)})
            continue
        if s.startswith("};"):
            if stack:
                n = stack.pop()
                nodes.append(n)
                ph = n["props"].get("phandle")
                if ph:
                    by_ph[int(ph[0], 16)] = n
            continue
        m = re.match(r"([\w,#.+-]+)\s*=\s*(.*);$", s)
        if m and stack:
            stack[-1]["props"][m.group(1)] = re.findall(r"0x[0-9a-f]+|\"[^\"]*\"", m.group(2))
    return nodes, by_ph


def compats(n):
    c = n["props"].get("compatible", [])
    return [x.strip('"') for s in c for x in s.strip('"').split("\\0")]


def module_strings(dirs):
    strs = {}
    for d in dirs:
        for dp, _dn, fns in os.walk(d):
            for fn in fns:
                if fn.endswith(".ko"):
                    strs[fn[:-3]] = set(re.findall(rb"[\x20-\x7e]{3,}", open(os.path.join(dp, fn), "rb").read()))
    return strs


def main(argv):
    nodes, by_ph = parse(argv[1])
    strs = module_strings(argv[3:])
    seen = set()

    def drivers(n):
        return sorted({m for c in compats(n) for m, s in strs.items() if c.encode() in s})

    def walk(n, depth):
        for prop, vals in sorted(n["props"].items()):
            if not PH_PROPS.match(prop):
                continue
            # the first cell of each specifier is a phandle; the rest are arguments: take every
            # value that is a known phandle (good enough to see the suppliers)
            for v in vals:
                if v.startswith('"'):
                    continue
                sup = by_ph.get(int(v, 16))
                if not sup or id(sup) in seen or sup is n:
                    continue
                seen.add(id(sup))
                c = compats(sup)
                d = drivers(sup) if c else []
                mark = "ok " if d else ("-- " if c else ".. ")
                print(f"{'  ' * depth}{mark}{prop}: {sup['path'][-60:]}  [{', '.join(c[:2]) or 'no compatible'}]  {' '.join(d[:3])}")
                if c and depth < 3:
                    walk(sup, depth + 1)

    for n in nodes:
        if argv[2] in compats(n) and n["props"].get("status", ['"okay"'])[0].strip('"') in ("okay", "ok"):
            print(f"== {n['path']}  {' '.join(drivers(n))}")
            walk(n, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
