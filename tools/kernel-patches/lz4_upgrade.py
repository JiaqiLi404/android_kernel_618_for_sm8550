#!/usr/bin/env python3
"""Integrate the pinned upstream LZ4 source with the Linux kernel API. Preserve export signatures and use the in-place decompression copy guard. Wrapper sources are in the adjacent lz4 directory. See docs/PROVENANCE.en.md."""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OURS = os.path.join(HERE, "lz4")

VERSION = (1, 10, 1)
UPSTREAM = ["lz4.c", "lz4.h", "lz4hc.c", "lz4hc.h"]
# our files: path in lib/lz4 -> path in tools/kernel-patches/lz4
OWN = {
    "lz4k.h": "lz4k.h",
    "lz4_up.c": "lz4_up.c",
    "lz4_kernel.c": "lz4_kernel.c",
    "Makefile": "Makefile",
    "shim/stddef.h": "shim/stddef.h",
    "shim/stdint.h": "shim/stdint.h",
    "shim/string.h": "shim/string.h",
    "shim/limits.h": "shim/limits.h",
}
GONE = ["lib/lz4/lz4_compress.c", "lib/lz4/lz4hc_compress.c"]

MAKEFILE_STOCK = """# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -O3

obj-$(CONFIG_LZ4_COMPRESS) += lz4_compress.o
obj-$(CONFIG_LZ4HC_COMPRESS) += lz4hc_compress.o
obj-$(CONFIG_LZ4_DECOMPRESS) += lz4_decompress.o
"""

# the 64-byte copies of arm64 behind a switch (three places: the copy itself, literals, matches)
WIDE_RE = re.compile(r"^([ \t]*)#([ \t]*)ifdef __aarch64__[ \t]*$", re.M)
WIDE_NEW = r"\1#\2if defined(__aarch64__) && !defined(LZ4_NO_WILDCOPY64)"
WIDE_MARK = (
    '/* lz4_upgrade.py: the three "#ifdef __aarch64__" around the 64-byte copies got a switch,\n'
    " * LZ4_NO_WILDCOPY64 (lib/lz4/Makefile sets it): those copies write further past a copy than\n"
    " * LZ4_DECOMPRESS_INPLACE_MARGIN leaves free, and EROFS decompresses in place with it. */\n"
)

HC_SIZE_OLD = "#define LZ4_STREAMHCSIZE        262192\n#define LZ4_STREAMHCSIZE_SIZET (262192 / sizeof(size_t))\n"
HC_SIZE_NEW = (
    "/*\n"
    " * lib/lz4 is upstream LZ4 v1.10.1 (lz4_upgrade.py): the stream types below are upstream's sizes\n"
    " * (LZ4_streamHC_t was 262192 bytes); the layouts of the *_internal structures are the old\n"
    " * library's, which only lib/lz4/lz4_decompress.c's pre-boot build still uses.\n"
    " */\n"
    "#define LZ4_STREAMHCSIZE        262200\n#define LZ4_STREAMHCSIZE_SIZET (262200 / sizeof(size_t))\n"
)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def upstream_version(up):
    text = read(os.path.join(up, "lib", "lz4.h"))
    parts = []
    for name in ("MAJOR", "MINOR", "RELEASE"):
        m = re.search(rf"^#define LZ4_VERSION_{name}\s+(\d+)", text, re.M)
        parts.append(int(m.group(1)) if m else -1)
    return tuple(parts)


def wide_switch(text):
    """lz4.c with the switch, or None when the three places are not exactly as expected."""
    found = list(WIDE_RE.finditer(text))
    if len(found) != 3:
        return None
    for m in found:
        # each guarded block is about the 64-byte copies
        block = text[m.end() : text.find("#", m.end() + 1)]
        if "LZ4_wildCopy64" not in block:
            return None
    return WIDE_MARK + WIDE_RE.sub(WIDE_NEW, text)


def apply(root, up):
    root = root.rstrip("/")
    problems, writes = [], {}
    if upstream_version(up) != VERSION:
        return [f"{up}: lib/lz4.h is v{'.'.join(map(str, upstream_version(up)))}, not v{'.'.join(map(str, VERSION))}"]
    lz4dir = f"{root}/lib/lz4"
    if os.path.exists(f"{lz4dir}/lz4armv8.S") or "lz4_arm64_fast" in read(f"{lz4dir}/lz4_decompress.c"):
        return ["lib/lz4 has lz4_arm64.py's NEON path; this replaces it (start from a tree without it)"]
    for name in UPSTREAM:
        text = read(os.path.join(up, "lib", name))
        if name == "lz4.c":
            text = wide_switch(text)
            if text is None:
                problems.append('upstream lib/lz4.c: expected three "#ifdef __aarch64__" around LZ4_wildCopy64')
                continue
        writes[f"lib/lz4/upstream/{name}"] = text
    for rel, src in OWN.items():
        writes[f"lib/lz4/{rel}"] = read(os.path.join(OURS, src))
    makefile = read(f"{lz4dir}/Makefile")
    if makefile not in (MAKEFILE_STOCK, writes["lib/lz4/Makefile"]):
        problems.append("lib/lz4/Makefile is neither GKI's nor ours")
    header = read(f"{root}/include/linux/lz4.h")
    if header.count(HC_SIZE_NEW) != 1:
        if header.count(HC_SIZE_OLD) != 1:
            problems.append(f"include/linux/lz4.h: expected 1x the LZ4_STREAMHCSIZE lines, found {header.count(HC_SIZE_OLD)}")
        header = header.replace(HC_SIZE_OLD, HC_SIZE_NEW)
    writes["include/linux/lz4.h"] = header
    if problems:
        return problems
    changed = 0
    for rel, text in writes.items():
        path = f"{root}/{rel}"
        if os.path.exists(path) and read(path) == text:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        changed += 1
    for rel in GONE:
        if os.path.exists(f"{root}/{rel}"):
            os.remove(f"{root}/{rel}")
            changed += 1
    return changed


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    result = apply(argv[1], argv[2])
    if isinstance(result, list):
        for p in result:
            print(f"::error::lz4_upgrade: {p}", file=sys.stderr)
        return 1
    what = "already applied" if result == 0 else f"{result} files written or removed"
    print(f"lz4_upgrade: lib/lz4 is upstream LZ4 v{'.'.join(map(str, VERSION))} behind the kernel's API ({what})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
