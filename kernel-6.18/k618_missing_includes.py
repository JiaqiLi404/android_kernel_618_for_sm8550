#!/usr/bin/env python3
"""k618_missing_includes.py <soc dir> <kernel source dir> [module json]: every #include of the
vendor sources that resolves nowhere (the file's own directory, the soc tree's include/ and
include/uapi, the kernel's include/, arch/arm64/include and source root), with the files that
include it -- the headers of features the publisher kept to itself (OnePlus' oplus_*) and those
the soc tree lacks. With the converter's gen/modules.json, only the chosen modules' sources."""

import collections
import json
import os
import re
import sys

INC = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def main(argv):
    soc, ker = argv[1].rstrip("/"), argv[2].rstrip("/")
    only = None
    if len(argv) > 3:
        mods = json.load(open(argv[3]))
        only = set()
        for m in mods.values():
            only.update(s for s in m["srcs"])
            for by_val in m["conditional_srcs"].values():
                for lst in by_val.values():
                    only.update(lst)
    paths = [f"{soc}/include", f"{soc}/include/uapi", f"{soc}/arch/arm64/include",
             f"{ker}/include", f"{ker}/arch/arm64/include", f"{ker}/arch/arm64/include/generated",
             f"{ker}/include/uapi", f"{ker}/arch/arm64/include/uapi", ker, soc]
    missing = collections.defaultdict(set)
    seen = set()
    todo = []
    for dp, _dn, fns in os.walk(soc):
        if "/.git" in dp or dp.endswith("/gen"):
            continue
        for fn in fns:
            if fn.endswith((".c", ".h")):
                rel = os.path.relpath(os.path.join(dp, fn), soc)
                if only is None or rel in only:
                    todo.append(os.path.join(dp, fn))
    while todo:
        f = todo.pop()
        if f in seen or not os.path.isfile(f):
            continue
        seen.add(f)
        text = open(f, encoding="utf-8", errors="replace").read()
        for inc in INC.findall(text):
            cands = [os.path.join(os.path.dirname(f), inc)] + [os.path.join(p, inc) for p in paths]
            hit = next((c for c in cands if os.path.isfile(c)), None)
            if hit is None:
                missing[inc].add(os.path.relpath(f, soc))
            elif hit.startswith(soc) and hit not in seen:
                todo.append(hit)
    for inc in sorted(missing, key=lambda k: -len(missing[k])):
        users = sorted(missing[inc])
        print(f"{inc}: {len(users)}  {' '.join(users[:4])}")
    print(f"-- {len(missing)} headers found nowhere", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv)
