#!/usr/bin/env python3
"""k618_abidiff.py [--dt <phone.dts>] <out dir> <stock module dir>... -- <our module dir>...: what the phone's own
5.15-era user space and device tree can see of each driver, compared module by module with ours.

The vendor partition and the device tree stay the 13 Pro's own, so for every module the phone
loads this lists what its stock build offers and ours does not:
  compat  devicetree compatibles it binds (the of: aliases)    -> a node nobody probes
  ioctl   ioctl numbers its *ioctl* functions compare against   -> a HAL call answered -ENOTTY
          (mov/movk immediates of the _IOC shape), plus words of
          the same ioctl types in its data (the drivers that
          dispatch through a table: kgsl, drm)
  size    the same ioctl with another argument size             -> a struct that changed layout
  attr    sysfs attributes (dev_attr_/driver_attr_/class_attr_/ -> a file a HAL opens or writes
          bus_attr_/bin_attr_/kobj_attr symbols)
  param   module parameters                                     -> a write by an init script
  export  symbols it exports                                    -> another module's import
and the modules that have no counterpart of ours at all. With --dt (the merged tree k618_dtcheck.py
leaves in /tmp/k618_dt.dts) a compatible counts only if an enabled node of the phone carries it.
Read only. Writes <out>/abidiff.tsv
(module, kind, item, detail) and <out>/abidiff.md (one line a module, worst first).
The tools come from $CLANG (env.sh) or PATH: llvm-readelf, llvm-nm, llvm-objdump.
"""
import collections
import os
import re
import shutil
import subprocess
import sys

TOOLS = {}


def tool(name):
    if name not in TOOLS:
        cand = os.path.join(os.environ.get("CLANG", ""), name)
        TOOLS[name] = cand if os.access(cand, os.X_OK) else shutil.which(name)
        if not TOOLS[name]:
            sys.exit(f"k618_abidiff: no {name} (set CLANG)")
    return TOOLS[name]


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, errors="replace").stdout


def norm(name):
    return name.removesuffix(".ko").replace("-", "_")


def modinfo(ko):
    """the .modinfo strings as (key, value) pairs"""
    out = run(tool("llvm-readelf"), "-p", ".modinfo", ko)
    pairs = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*[0-9a-f]+\]\s+(\w+)=(.*)$", line)
        if m:
            pairs.append((m.group(1), m.group(2)))
    return pairs


ATTR = re.compile(r"^(dev|driver|class|bus|bin|kobj|module|device)_attr_(\w+)$")


def symbols(ko):
    attrs, exports = set(), set()
    for line in run(tool("llvm-nm"), ko).splitlines():
        parts = line.split()
        if not parts:
            continue
        sym = parts[-1]
        m = ATTR.match(sym)
        if m:
            attrs.add(f"{m.group(1)}:{m.group(2)}")
        if sym.startswith("__ksymtab_"):
            exports.add(sym[len("__ksymtab_"):].removeprefix("gpl_"))
        elif sym.startswith("__export_symbol_"):
            exports.add(sym[len("__export_symbol_"):])
    return attrs, exports


def ioc_shape(v):
    """an _IOC number: direction 1..3, an argument of 1 byte to 4 KiB, a type byte that is not 0 --
    and not one of the look-alikes: a small negative number (an error code) or four printable
    characters (a piece of a string in the data)"""
    d, size, typ = v >> 30, (v >> 16) & 0x3FFF, (v >> 8) & 0xFF
    if v >= 0xFFFF0000 or all(0x20 <= b < 0x7F for b in v.to_bytes(4, "little")):
        return False
    return d != 0 and 0 < size <= 0x1000 and typ != 0


INS = re.compile(r"^\s*[0-9a-f]+:\s+(mov|movk|movz)\s+(w\d+|x\d+),\s+#(0x[0-9a-f]+|\d+)(?:,\s+lsl\s+#(\d+))?")
FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:")


def ioctls(ko):
    """ioctl-shaped constants built in the module's *ioctl* functions"""
    found = set()
    func, regs = "", {}
    out = run(tool("llvm-objdump"), "-d", "--no-show-raw-insn", ko)
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            func, regs = m.group(1).lower(), {}
            continue
        if "ioctl" not in func:
            continue
        m = INS.match(line)
        if not m:
            continue
        op, reg, imm, sh = m.group(1), m.group(2)[1:], int(m.group(3), 0), int(m.group(4) or 0)
        if op in ("mov", "movz"):
            regs[reg] = (imm << sh) & 0xFFFFFFFF
        elif reg in regs:
            regs[reg] = (regs[reg] & ~(0xFFFF << sh) | (imm << sh)) & 0xFFFFFFFF
        else:
            continue
        if ioc_shape(regs[reg]):
            found.add(regs[reg])
    return found


def data_words(ko, types):
    """ioctl-shaped 32-bit words of the given types in the module's data (dispatch tables)"""
    if not types:
        return set()
    found = set()
    secs = run(tool("llvm-readelf"), "-S", "-W", ko)
    names = [m.group(1) for m in re.finditer(r"\]\s+(\.(?:data|rodata)[\w.]*)\s+PROGBITS", secs)]
    for sec in names:
        dump = run(tool("llvm-readelf"), "-x", sec, ko)
        for line in dump.splitlines():
            m = re.match(r"\s*0x[0-9a-f]+\s+((?:[0-9a-f]{8}\s+){1,4})", line)
            if not m:
                continue
            for word in m.group(1).split():
                v = int.from_bytes(bytes.fromhex(word), "little")
                if ioc_shape(v) and (v >> 8) & 0xFF in types:
                    found.add(v)
    return found


def describe(ko):
    info = modinfo(ko)
    compat = set()
    for k, v in info:
        if k == "alias" and v.startswith("of:"):
            m = re.search(r"C(.*?)(?:C\*)?$", v.split("T*", 1)[-1])
            if m and m.group(1):
                compat.add(m.group(1))
    params = {v.split(":", 1)[0] for k, v in info if k in ("parm", "parmtype")}
    attrs, exports = symbols(ko)
    code = ioctls(ko)
    types = {(v >> 8) & 0xFF for v in code}
    ioc = code | data_words(ko, types)
    return {"compat": compat, "param": params, "attr": attrs, "export": exports, "ioctl": ioc}


def dt_compatibles(dts):
    """the compatibles of the enabled nodes of a decompiled device tree (dtc -O dts)"""
    found, stack = set(), []
    for line in open(dts, encoding="utf-8", errors="replace"):
        t = line.strip()
        if t.endswith("{"):
            stack.append({"compat": [], "ok": True})
        elif t.startswith("};") and stack:
            node = stack.pop()
            if node["ok"]:
                found.update(node["compat"])
        elif stack and t.startswith("compatible ="):
            stack[-1]["compat"] = re.findall(r'"([^"]+)"', t)
        elif stack and t.startswith("status =") and "okay" not in t and '"ok"' not in t:
            stack[-1]["ok"] = False
    return found


def collect(dirs):
    mods = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(".ko"):
                    mods.setdefault(norm(f), os.path.join(root, f))
    return mods


def ioc_name(v):
    d = {1: "W", 2: "R", 3: "RW"}[v >> 30]
    return f"0x{v:08x}({d} type 0x{(v >> 8) & 0xFF:02x} nr {v & 0xFF} size {(v >> 16) & 0x3FFF})"


def main():
    if "--" not in sys.argv or len(sys.argv) < 5:
        sys.exit(__doc__)
    dt = None
    if sys.argv[1] == "--dt":
        dt = dt_compatibles(sys.argv[2])
        del sys.argv[1:3]
    sep = sys.argv.index("--")
    out, stock_dirs, our_dirs = sys.argv[1], sys.argv[2:sep], sys.argv[sep + 1:]
    os.makedirs(out, exist_ok=True)
    stock, ours = collect(stock_dirs), collect(our_dirs)
    rows, summary = [], []
    for name in sorted(stock):
        if name not in ours:
            rows.append((name, "missing", "", stock[name]))
            summary.append((1000, name, "no module of ours"))
            continue
        a, b = describe(stock[name]), describe(ours[name])
        score, notes = 0, []
        # with the phone's tree given, only the compatibles one of its enabled nodes carries
        lost_compat = sorted(c for c in a["compat"] - b["compat"] if dt is None or c in dt)
        for c in lost_compat:
            rows.append((name, "compat", c, "stock only"))
        # the same ioctl (direction, type, number) with another size: a struct that changed layout
        key = lambda v: (v >> 30, (v >> 8) & 0xFF, v & 0xFF)
        b_by_key = collections.defaultdict(set)
        for v in b["ioctl"]:
            b_by_key[key(v)].add(v)
        resized, lost_ioc = [], []
        for v in sorted(a["ioctl"] - b["ioctl"]):
            other = b_by_key.get(key(v))
            if other:
                resized.append(v)
                rows.append((name, "size", ioc_name(v), "ours: " + " ".join(ioc_name(o) for o in sorted(other))))
            else:
                lost_ioc.append(v)
                rows.append((name, "ioctl", ioc_name(v), "stock only"))
        lost = {k: sorted(a[k] - b[k]) for k in ("attr", "param", "export")}
        for k, items in lost.items():
            for i in items:
                rows.append((name, k, i, "stock only"))
        score = 50 * len(lost_compat) + 20 * len(resized) + 10 * len(lost_ioc) + 3 * len(lost["attr"]) + len(lost["param"]) + len(lost["export"])
        for label, n in (("compat", len(lost_compat)), ("ioctl size", len(resized)), ("ioctl", len(lost_ioc)),
                         ("attr", len(lost["attr"])), ("param", len(lost["param"])), ("export", len(lost["export"]))):
            if n:
                notes.append(f"{n} {label}")
        if notes:
            summary.append((score, name, ", ".join(notes)))
    with open(os.path.join(out, "abidiff.tsv"), "w", newline="\n") as f:
        f.write("module\tkind\titem\tdetail\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    with open(os.path.join(out, "abidiff.md"), "w", newline="\n") as f:
        f.write(f"stock modules {len(stock)}, ours {len(ours)}; stock items ours lacks, worst first\n\n")
        for score, name, note in sorted(summary, key=lambda s: (-s[0], s[1])):
            f.write(f"- {name}: {note}\n")
    missing = sum(1 for s in summary if s[0] == 1000)
    print(f"stock {len(stock)}, ours {len(ours)}, missing {missing}, with differences {len(summary) - missing}")
    print(f"wrote {out}/abidiff.tsv and abidiff.md")


if __name__ == "__main__":
    main()
