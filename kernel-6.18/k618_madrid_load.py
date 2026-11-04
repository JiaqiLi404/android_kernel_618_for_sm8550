#!/usr/bin/env python3
"""k618_madrid_load.py <madrid root> <our symvers>... -- <module>...: could these madrid (18 Pro Max, 6.18)
modules load on our kernel? For every symbol each one imports (its __versions), whether our kernel or
our modules export it with the same CRC, whether one of the listed madrid modules exports it, or
whether nothing does. The listed set is closed over its madrid dependencies first.
Our symvers: $O/Module.symvers (GKI), $S/Module.symvers (soc), and k618_ko_symvers.py over the
staged techpack modules (boot-test/stage/mods/.../kernel); OURS=<that dir> takes the dependencies
we build ourselves from us. docs/PROVENANCE.en.md has what it found."""
import os
import re
import subprocess
import sys

root = sys.argv[1]
i = sys.argv.index("--")
symvers_files, wanted = sys.argv[2:i], sys.argv[i + 1:]
# OURS=<dir>: a dependency we build ourselves is taken from us, not from madrid
OURS_DIR = os.environ.get("OURS")
ours_names = set()
for od in (OURS_DIR or "").split(":"):
    if od:
        ours_names |= {f[:-3].replace("-", "_") for f in os.listdir(od) if f.endswith(".ko")}
DIRS = ["vendor_boot/ramdisk0/lib/modules", "vendor_dlkm/lib/modules", "system_dlkm/lib/modules"]


def find(name):
    for d in DIRS:
        p = os.path.join(root, d, name + ".ko")
        if os.path.exists(p):
            return p
        p2 = os.path.join(root, d, name.replace("_", "-") + ".ko")
        if os.path.exists(p2):
            return p2
    return None


def deps(p):
    out = subprocess.run(["modinfo", "-F", "depends", p], capture_output=True, text=True).stdout.strip()
    return [d for d in out.split(",") if d]


def versions(p):
    out = subprocess.run(["modprobe", "--dump-modversions", p], capture_output=True, text=True).stdout
    return {m.group(2): int(m.group(1), 16) for m in re.finditer(r"^(0x[0-9a-f]+)\s+(\S+)$", out, re.M)}


def exports(p):
    # the symbols a module exports: its __ksymtab entries, by name, from the symbol table
    out = subprocess.run(["nm", p], capture_output=True, text=True).stdout
    return {m.group(1) for m in re.finditer(r"__ksymtab_(\S+)$", out, re.M)}


ours = {}
for f in symvers_files:
    for line in open(f, encoding="utf-8", errors="replace"):
        parts = line.split("\t")
        if len(parts) >= 3:
            ours.setdefault(parts[1], (int(parts[0], 16), parts[2]))

# close the set over madrid dependencies
todo, mods = list(wanted), {}
while todo:
    m = todo.pop()
    if m in mods:
        continue
    if m not in wanted and m.replace("-", "_") in ours_names:
        continue
    p = find(m)
    if not p:
        mods[m] = None
        continue
    mods[m] = p
    todo += deps(p)
internal = {}
for m, p in mods.items():
    if p:
        for s in exports(p):
            internal[s] = m
print("madrid modules in the set:", " ".join(sorted(m for m, p in mods.items() if p)))
print("not found in madrid:", " ".join(sorted(m for m, p in mods.items() if not p)) or "-")
for m in sorted(mods):
    p = mods[m]
    if not p:
        continue
    v = versions(p)
    ok = mism = inside = missing = 0
    bad, gone = [], []
    for s, crc in sorted(v.items()):
        if s in internal and internal[s] != m:
            inside += 1
        elif s in ours:
            if ours[s][0] == crc:
                ok += 1
            else:
                mism += 1
                bad.append(f"{s}({os.path.basename(ours[s][1].strip())})")
        elif s == "module_layout":
            ok += 1
        else:
            missing += 1
            gone.append(s)
    print(f"== {m}: {len(v)} imports: {ok} match ours, {inside} from the set, {mism} CRC differ, {missing} nowhere")
    if bad:
        print("   CRC differ:", " ".join(bad[:30]) + (" ..." if len(bad) > 30 else ""))
    if gone:
        print("   nowhere:", " ".join(gone[:30]) + (" ..." if len(gone) > 30 else ""))
