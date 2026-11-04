#!/usr/bin/env python3
"""k618_ko_symvers.py <dir>... : a Module.symvers-style list (crc, symbol, module, EXPORT_SYMBOL) of what
the .ko files in these directories export, read from each module's __kcrctab (the __crc_<symbol>
entries). For modules built without a kept Module.symvers (our techpacks)."""
import os
import struct
import sys


def syms(path):
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2:
        return {}
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x3a)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize)
        secs.append((name, typ, off, size, link, entsize))
    shstr = secs[shstrndx]

    def sname(o, tab):
        s = b[tab[2] + o:b.index(b"\0", tab[2] + o)]
        return s.decode()

    names = [sname(s[0], shstr) for s in secs]
    if "__kcrctab" not in names:
        return {}
    kc = names.index("__kcrctab")
    out = {}
    for i, s in enumerate(secs):
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[4]]
        for j in range(s[3] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", b, s[2] + j * 24)
            n = sname(st_name, strtab)
            if n.startswith("__crc_") and st_shndx == kc:
                crc, = struct.unpack_from("<I", b, secs[kc][2] + st_value)
                out[n[6:]] = crc
    return out


for d in sys.argv[1:]:
    for f in sorted(os.listdir(d)):
        if f.endswith(".ko"):
            for s, c in sorted(syms(os.path.join(d, f)).items()):
                print(f"0x{c:08x}\t{s}\t{os.path.join(d, f[:-3])}\tEXPORT_SYMBOL\t")
