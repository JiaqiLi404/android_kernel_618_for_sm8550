#!/usr/bin/env python3
"""k618_kmi_check.py <Module.symvers> <dir of .ko>...: for every module, the symbols it imports
with a CRC (its __versions section) against the kernel's Module.symvers: how many match, differ,
or are missing -- whether the 18 Pro Max's modules would load on the kernel built here."""
import os, struct, sys, collections

def symvers(path):
    s = {}
    for line in open(path):
        f = line.split("\t")
        if len(f) >= 2:
            s[f[1]] = int(f[0], 16)
    return s

def versions(ko):
    d = open(ko, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        return None
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    stro = secs[shstrndx][4]
    out = {}
    extended_names = None
    extended_crcs = None
    for s in secs:
        name = d[stro + s[0]: d.index(b"\0", stro + s[0])].decode()
        if name == "__version_ext_names":
            extended_names = d[s[4]:s[4] + s[5]].rstrip(b"\0").split(b"\0")
        elif name == "__version_ext_crcs":
            extended_crcs = struct.unpack_from("<" + "I" * (s[5] // 4), d, s[4])
        elif name == "__versions":
            off, size = s[4], s[5]
            # each entry: u32 crc (+ pad to 8) + name[56] in older; 6.x: struct modversion_info {unsigned long crc; char name[64-8]}
            for p in range(off, off + size, 64):
                crc = struct.unpack_from("<Q", d, p)[0]
                n = d[p + 8: p + 64].split(b"\0")[0].decode(errors="replace")
                out[n] = crc
    if extended_names is not None or extended_crcs is not None:
        if extended_names is None or extended_crcs is None or len(extended_names) != len(extended_crcs):
            raise ValueError(f"invalid extended module versions: {ko}")
        for name, crc in zip(extended_names, extended_crcs):
            decoded = name.decode(errors="strict")
            if decoded in out and out[decoded] != crc:
                raise ValueError(f"conflicting basic/extended version: {ko}: {decoded}")
            out[decoded] = crc
    return out

def main():
    sv = symvers(sys.argv[1])
    tot = collections.Counter()
    bad_mods = []
    for root in sys.argv[2:]:
        for dp, _d, fs in os.walk(root):
            for f in sorted(fs):
                if not f.endswith(".ko"):
                    continue
                v = versions(os.path.join(dp, f))
                if v is None:
                    continue
                same = sum(1 for n, c in v.items() if sv.get(n) == c)
                diff = sum(1 for n, c in v.items() if n in sv and sv[n] != c)
                miss = sum(1 for n in v if n not in sv)
                tot["mods"] += 1; tot["same"] += same; tot["diff"] += diff; tot["miss"] += miss
                if diff or miss:
                    bad_mods.append((f, same, diff, miss, [n for n, c in v.items() if n in sv and sv[n] != c][:3], [n for n in v if n not in sv][:3]))
    print(f"modules {tot['mods']}: imports with the same CRC {tot['same']}, another CRC {tot['diff']}, not exported here {tot['miss']}")
    print(f"modules with every import matching: {tot['mods'] - len(bad_mods)}")
    for m in bad_mods[:25]:
        print("  ", m[0], "same", m[1], "diff", m[2], "missing", m[3], m[4], m[5])

main()
