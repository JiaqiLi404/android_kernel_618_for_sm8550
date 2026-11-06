#!/usr/bin/env python3
"""k618_stage_crc.py <Module.symvers of vmlinux> <dir of .ko>...: every versioned import of every
module in the given directories, checked against whoever provides that symbol -- another module in
the directories (its own __crc_* symbols) or the kernel (Module.symvers, vmlinux lines only).

Why: a module whose import CRC differs from its provider's export is refused at load ("disagrees
about version of symbol"). The QEMU boots refuse the Wi-Fi group for want of PCIe anyway, so a
version mismatch there (a Wi-Fi driver built against an earlier build of cfg80211, say) looks like
the expected refusal. This check reads the files instead of a boot.

Prints one line per mismatch and a summary; exit 1 when any import differs from its provider."""
import os
import struct
import sys


def sections(b):
    shoff = struct.unpack_from("<Q", b, 0x28)[0]
    ent, num, stri = struct.unpack_from("<HHH", b, 0x3A)
    s = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * ent) for i in range(num)]
    so = s[stri][4]
    names = [b[so + x[0]: b.index(b"\0", so + x[0])].decode() for x in s]
    return dict(zip(names, s)), s


def imports(b, S):
    out = {}
    if "__versions" in S:
        o, sz = S["__versions"][4], S["__versions"][5]
        for i in range(0, sz, 64):
            crc = struct.unpack_from("<Q", b, o + i)[0] & 0xffffffff
            nm = b[o + i + 8: o + i + 64].split(b"\0")[0].decode()
            out[nm] = crc
    # 6.14+ extended modversions: names too long for __versions are listed here only
    if "__version_ext_names" in S and "__version_ext_crcs" in S:
        no, nsz = S["__version_ext_names"][4], S["__version_ext_names"][5]
        co = S["__version_ext_crcs"][4]
        names = [n.decode() for n in b[no:no + nsz].split(b"\0") if n]
        for i, n in enumerate(names):
            out[n] = struct.unpack_from("<I", b, co + 4 * i)[0]
    return out


def exports(b, S, secs):
    out = {}
    st, strt = S.get(".symtab"), S.get(".strtab")
    if not st or not strt:
        return out
    for i in range(st[4], st[4] + st[5], 24):
        n_off, _info, _other, shndx, val, _size = struct.unpack_from("<IBBHQQ", b, i)
        nm = b[strt[4] + n_off: b.index(b"\0", strt[4] + n_off)].decode()
        if nm.startswith("__crc_") and 0 < shndx < len(secs):
            sec = secs[shndx]
            out[nm[6:]] = struct.unpack_from("<I", b, sec[4] + val)[0]
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    kernel = {}
    for line in open(argv[1]):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 3 and f[2].endswith("vmlinux"):
            kernel[f[1]] = int(f[0], 16)
    mods = {}
    for d in argv[2:]:
        for n in sorted(os.listdir(d)):
            if n.endswith(".ko"):
                mods[n[:-3]] = os.path.join(d, n)
    exp, imp = {}, {}
    for m, p in mods.items():
        b = open(p, "rb").read()
        S, secs = sections(b)
        imp[m] = imports(b, S)
        for sym, crc in exports(b, S, secs).items():
            exp.setdefault(sym, []).append((m, crc))
    checked = bad = nowhere = 0
    for m in sorted(imp):
        for sym, crc in sorted(imp[m].items()):
            if sym in ("module_layout",):
                continue
            prov = exp.get(sym)
            if prov:
                checked += 1
                if all(c != crc for _pm, c in prov):
                    bad += 1
                    print(f"{m}: {sym} {crc:#010x}, {prov[0][0]} exports {prov[0][1]:#010x}")
            elif sym in kernel:
                checked += 1
                if kernel[sym] != crc:
                    bad += 1
                    print(f"{m}: {sym} {crc:#010x}, vmlinux exports {kernel[sym]:#010x}")
            else:
                nowhere += 1
    print(f"k618_stage_crc: {len(mods)} modules, {checked} versioned imports checked, {bad} differ, "
          f"{nowhere} with no provider here")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
