#!/usr/bin/env python3
"""k618_cpiofilter.py <in.cpio> <out.cpio> <regex>: copy a newc cpio archive (or several concatenated
ones) entry by entry, leaving out the entries whose name matches the regex -- headers and data
unchanged, so modes, owners and links stay as they were. Used to drop vendor_boot's 5.15 modules
from the ramdisk that k618_irbuild.sh builds into the kernel."""

import re
import sys


def entries(data):
    i = 0
    while i + 110 <= len(data):
        if data[i:i + 6] not in (b"070701", b"070702"):
            i += 1          # padding between concatenated archives
            continue
        size, nlen = int(data[i + 54:i + 62], 16), int(data[i + 94:i + 102], 16)
        name = data[i + 110:i + 110 + nlen - 1].decode()
        body = (i + 110 + nlen + 3) & ~3
        end = (body + size + 3) & ~3
        yield name, data[i:end]
        i = end


def main(argv):
    data = open(argv[1], "rb").read()
    drop = re.compile(argv[3])
    out, kept, dropped = [], 0, 0
    for name, raw in entries(data):
        if name == "TRAILER!!!":
            continue
        if drop.search(name):
            dropped += 1
            continue
        out.append(raw)
        kept += 1
    # one trailer at the end
    trailer = b"070701" + b"0" * 8 * 11 + b"0000000B" + b"0" * 8 + b"TRAILER!!!\0"
    trailer += b"\0" * (-len(trailer) % 4)
    blob = b"".join(out) + trailer
    blob += b"\0" * (-len(blob) % 512)
    open(argv[2], "wb").write(blob)
    print(f"kept {kept} entries, dropped {dropped}; {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
