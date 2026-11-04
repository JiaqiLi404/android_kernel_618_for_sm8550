#!/usr/bin/env python3
"""k618_modgap.py <dir> [soc tree] [evaluated vendor .config]: sort the phone's second-stage modules
by the tree that could build them.

<dir> holds what k618_modgap.sh collected. "soc" means soc-repo's own registry (its modules.bzl, read
as k618_soc_kbuild.py reads it) defines a module of that name -- the only thing k618_soc_kbuild.py
can build; its config is noted when the evaluated configuration has it off. Elsewhere a module
matches a tree by a source file of its name (single-file modules) or by a build-file target of its
name (obj-...+= name.o, "name.ko"), with '-' and '_' taken as equal. First match wins, cheapest first:

  built      we already build it (soc/)          gki-built  GKI already builds it
             ("as <name>" when our file name differs: '-' for '_', or a 6.12 release's new name)
  soc        soc-repo's registry has it          techpack   a 6.12 techpack repo has it
  gki        Google's 6.12 common has it          xiaomi515  only Xiaomi's 5.15 tree has it
  none       no tree we hold has it

Writes <dir>/modgap.tsv (module, class, where) and prints the summary.
"""
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k618_soc_kbuild as kb  # noqa: E402

SKIP = ("arch/mips", "drivers/staging", "samples/", "tools/", "Documentation")
# a module the phone's list names by its 5.15 name and a 6.12 release ships under another: same
# driver, same symbols, so the phone's module is covered
RENAMED = {"qcom_ipc_lite": "ipclite"}		# IPCLite moved into the synx techpack
OBJ = re.compile(r"\b([A-Za-z0-9][A-Za-z0-9_.-]*)\.(?:o|ko)\b")
KO = re.compile(r'"([A-Za-z0-9_][A-Za-z0-9_.-]*)\.ko"')


def norm(name):
    return name.replace("-", "_")		# modprobe makes no difference between the two


def lines(d, name):
    p = os.path.join(d, name)
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\n") for ln in f]


def area_src(p):
    return "/".join(p.split("/")[:3])


def area_tp(p):
    q = p.split("/")
    if q[0] == "vendor" and len(q) > 3 and q[1] == "qcom":
        return q[3]			# vendor/qcom/opensource/<techpack>
    if q[0] == "vendor" and len(q) > 2:
        return "/".join(q[1:3])
    return "/".join(q[:2])


def table(c_files, build, area, git=False):
    out = defaultdict(set)
    for p in c_files:
        if not p.startswith(SKIP):
            out[os.path.basename(p)[:-2]].add(area(p))
    for ln in build:
        if git:				# git grep: <ref>:<path>:<line>:<text>
            ln = ln.partition(":")[2]
        path, _, text = ln.partition(":")
        num, _, rest = text.partition(":")
        if git and num.isdigit():
            text = rest
        if path.startswith(SKIP):
            continue
        t = text.strip()
        if ".o" in t and ("obj-" in t or "-objs" in t or "-y" in t):
            for m in OBJ.finditer(t):
                out[m.group(1)].add(area(path))
        for m in KO.finditer(t):
            out[m.group(1)].add(area(path))
    return out


def find(tbl, n):
    hit = set()
    for v in {n, n.replace("-", "_"), n.replace("_", "-")}:
        hit |= tbl.get(v, set())
    return "; ".join(sorted(hit))


def registry(soc, evaluated):
    out = defaultdict(set)
    if not soc:
        return out
    cfg = kb.read_config(evaluated) if evaluated and os.path.exists(evaluated) else {}
    for m in kb.load_registry(soc).values():
        note = m["name"]
        if cfg and not kb.enabled(cfg, m["config"]):
            note += f" ({m['config']} off)"
        out[m["out"][:-3]].add(note)
    return out


def main(d, soc=None, evaluated=None):
    have = {ln[:-3] for ln in lines(d, "rom_ko.txt")}
    rom = [n for n in dict.fromkeys(os.path.basename(ln.strip())[:-3]
                                    for ln in lines(d, "rom_modules.load") if ln.strip().endswith(".ko"))
           if n in have]		# modules.load also names the first stage's, absent here
    # what we build, by the name modprobe would match it with: '-' and '_' are the same character
    # to it, and a module a 6.12 release renamed is still the phone's module
    ours = {norm(ln[:-3]): ln[:-3] for ln in lines(d, "our_ko.txt")}
    gki_built = {norm(ln[:-3]) for ln in lines(d, "gki_ko.txt")}
    order = [
        ("soc", registry(soc, evaluated)),
        ("techpack", table(lines(d, "tp_c.txt"), lines(d, "tp_build.txt"), area_tp, git=True)),
        ("gki", table(lines(d, "gki_c.txt"), lines(d, "gki_build.txt"), area_src)),
        ("xiaomi515", table(lines(d, "fuxi_c.txt"), lines(d, "fuxi_build.txt"), area_src)),
    ]
    rows = []
    for n in rom:
        k = RENAMED.get(norm(n), norm(n))
        if k in ours:
            rows.append((n, "built", "" if ours[k] == n else f"as {ours[k]}"))
        elif k in gki_built:
            rows.append((n, "gki-built", ""))
        else:
            for cls, tbl in order:
                where = find(tbl, n)
                if where:
                    rows.append((n, cls, where))
                    break
            else:
                rows.append((n, "none", ""))

    with open(os.path.join(d, "modgap.tsv"), "w", encoding="utf-8", newline="") as f:
        for r in rows:
            f.write("\t".join(r) + "\n")

    by = defaultdict(list)
    for n, cls, where in rows:
        by[cls].append((n, where))
    print(f"{len(rom)} second-stage modules on vendor_dlkm")
    for cls in ("built", "gki-built", "soc", "techpack", "gki", "xiaomi515", "none"):
        print(f"  {cls:10s} {len(by[cls]):4d}")
    off = sorted(f"{n} {w[w.index('(') + 1:-1]}" for n, w in by["soc"] if "off)" in w)
    if off:
        print(f"\nsoc, config off in the evaluated configuration: {', '.join(off)}")
    for cls in ("techpack", "gki", "xiaomi515", "none"):
        print(f"\n{cls}:")
        groups = defaultdict(list)
        for n, where in by[cls]:
            groups[where.split(";")[0]].append(n)
        for where, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(names):3d} {where or '-'}: {' '.join(sorted(names))}")


if __name__ == "__main__":
    main(*(sys.argv[1:4] if len(sys.argv) > 1 else ["."]))
