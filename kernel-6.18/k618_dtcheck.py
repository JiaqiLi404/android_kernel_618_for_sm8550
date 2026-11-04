#!/usr/bin/env python3
"""k618_dtcheck.py <vendor_boot.img> <dtbo.img> <stock modules dir> <vmlinux> <module dir>...:
which nodes of the 13 Pro's device tree our 6.12 build would bind, offline.

The base tree (kalama) comes out of vendor_boot's dtb section, nuwa's overlay out of dtbo.img;
ufdt_apply_overlay merges them as the bootloader would. For every enabled node with a compatible
the report says which of our modules matches it (their of: aliases), whether vmlinux has the
string (a built-in driver, likely), and which stock 5.15 module matched it -- so what the phone
used that we do not have yet stands out. Read only; the merged tree is left in /tmp/k618_dt.dts."""

import collections
import os
import pwd
import re
import struct
import subprocess
import sys

# Google's dtc and ufdt_apply_overlay, from the 6.12 workspace (env.sh: K618, NB)
NB = os.environ.get("NB") or os.path.join(pwd.getpwuid(os.getuid()).pw_dir, "xiaomi13-k618-build")
TOOLS = os.path.join(os.environ.get("K618") or os.path.join(NB, "k618"), "ws/prebuilts/kernel-build-tools/linux_musl-x86/bin")


def dtc(blob_path):
    return subprocess.run([f"{TOOLS}/dtc", "-q", "-I", "dtb", "-O", "dts", blob_path],
                          capture_output=True, text=True).stdout


def vendor_boot_dtbs(path):
    d = open(path, "rb").read()
    assert d[:8] == b"VNDRBOOT", d[:8]
    page = struct.unpack_from("<I", d, 12)[0]
    rd_size = struct.unpack_from("<I", d, 24)[0]
    hdr_size, dtb_size = struct.unpack_from("<II", d, 28 + 2048 + 4 + 16)
    al = lambda x: (x + page - 1) // page * page  # noqa: E731
    off = al(hdr_size) + al(rd_size)
    dtb = d[off:off + dtb_size]
    out, i = [], 0
    while True:
        i = dtb.find(b"\xd0\x0d\xfe\xed", i)
        if i < 0:
            break
        size = struct.unpack(">I", dtb[i + 4:i + 8])[0]
        out.append(dtb[i:i + size])
        i += size
    return out


def dtbo_entries(path):
    d = open(path, "rb").read()
    magic, _total, _hsz, esz, count, eoff = struct.unpack_from(">IIIIII", d, 0)
    assert magic == 0xd7b7ab1e, hex(magic)
    out = []
    for n in range(count):
        size, off = struct.unpack_from(">II", d, eoff + n * esz)
        out.append(d[off:off + size])
    return out


def model(dts):
    m = re.search(r'^\tmodel = "([^"]*)";', dts, re.M)
    return m.group(1) if m else "?"


def nodes(dts):
    """(path, [compatibles], enabled) of every node with a compatible"""
    out, stack, cur = [], [], {}
    for line in dts.splitlines():
        s = line.strip()
        m = re.match(r"(?:[\w-]+:\s*)*([^\s{]+)\s*\{$", s)
        if m:
            stack.append((m.group(1), cur))
            cur = {}
            continue
        if s.startswith("};"):
            if stack:
                name, parent = stack.pop()
                if "compatible" in cur:
                    path = "/".join(n for n, _ in stack[1:]) + "/" + name
                    out.append((path, cur["compatible"], cur.get("status", "okay") in ("okay", "ok")))
                cur = parent
            continue
        m = re.match(r'compatible = (.*);$', s)
        if m:
            # dtc writes some string lists as one string with \0 between them
            cur["compatible"] = [c for s in re.findall(r'"([^"]*)"', m.group(1)) for c in s.split("\\0")]
        m = re.match(r'status = "([^"]*)";', s)
        if m:
            cur["status"] = m.group(1)
    return out


def aliases(dirs, compats):
    """compatible -> [module]: the modules whose strings have it (a match table entry is a string of
    its own; several drivers -- rpmh-regulator, gdsc-regulator -- export no of: aliases)"""
    out = collections.defaultdict(set)
    for d in dirs:
        for dp, _dn, fns in os.walk(d):
            for fn in fns:
                if not fn.endswith(".ko"):
                    continue
                strs = set(re.findall(rb"[\x20-\x7e]{3,}", open(os.path.join(dp, fn), "rb").read()))
                for c in compats:
                    if c.encode() in strs:
                        out[c].add(fn[:-3])
    return out


def main(argv):
    vb, dtbo, stock, vmlinux, mods = argv[1], argv[2], argv[3], argv[4], argv[5:]
    bases = vendor_boot_dtbs(vb)
    print(f"vendor_boot: {len(bases)} base trees")
    base = None
    for n, b in enumerate(bases):
        p = f"/tmp/k618_base{n}.dtb"
        open(p, "wb").write(b)
        mdl = model(dtc(p))
        print(f"  {n}: {mdl}")
        if base is None and mdl.endswith("Kalama v2 SoC"):
            base = p
    ovl = None
    for n, o in enumerate(dtbo_entries(dtbo)):
        p = f"/tmp/k618_ovl{n}.dtbo"
        open(p, "wb").write(o)
        mdl = model(dtc(p))
        if ovl is None and "nuwa" in mdl.lower():
            ovl = p
            print(f"dtbo entry {n}: {mdl}")
    if not base or not ovl:
        print("no kalama base or no nuwa overlay", file=sys.stderr)
        return 1
    subprocess.run([f"{TOOLS}/ufdt_apply_overlay", base, ovl, "/tmp/k618_dt.dtb"], check=True)
    dts = dtc("/tmp/k618_dt.dtb")
    open("/tmp/k618_dt.dts", "w").write(dts)
    print(f"merged: {model(dts)}")
    allc = {c for _p, cs, _on in nodes(dts) for c in cs}
    ours, stk = aliases(mods, allc), aliases([stock], allc)
    vml = open(vmlinux, "rb").read()
    rows, missing = [], collections.Counter()
    for path, compats, on in nodes(dts):
        if not on:
            continue
        mine = sorted({m for c in compats for m in ours.get(c, ())})
        builtin = [c for c in compats if b"\0" + c.encode() + b"\0" in vml]
        old = sorted({m for c in compats for m in stk.get(c, ())})
        if not mine and not builtin:
            missing[(compats[0], ",".join(old) or "-")] += 1
        rows.append((path, compats[0], ",".join(mine) or ("(built-in)" if builtin else "-"), ",".join(old) or "-"))
    n_ok = sum(1 for r in rows if r[2] != "-")
    print(f"enabled nodes with a compatible: {len(rows)}; matched by ours or built in: {n_ok}")
    print("\nnot matched (compatible, stock 5.15 module that matched it; nodes):")
    for (c, old), k in sorted(missing.items(), key=lambda x: (x[0][1] == "-", x[0][1], x[0][0])):
        print(f"  {c:48s} {old:40s} {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
