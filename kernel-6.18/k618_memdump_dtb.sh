#!/bin/bash
# k618_memdump_dtb.sh: a QEMU virt device tree with the 13 Pro's memory-dump layout, for checking
# memory_dump_v2 (or a fix of it) in QEMU: a reusable shared-dma-pool (a CMA area, 52 MiB as on the
# phone) and a qcom,mem-dump node whose dump entries are its direct children, as in the phone's DT.
# soc-repo's memory_dump_v2 frees most of such a pool a second time with free_reserved_page(), which
# QEMU shows as "BUG: Bad page state" lines. Writes $K618/boot-test/qemu/qemu-memdump.dtb; use it as
#   QEMU_DTB=$K618/boot-test/qemu/qemu-memdump.dtb bash k618_qemu.sh "" 60
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
D=$K618/boot-test/qemu
mkdir -p $D
DTC=$TOOLS/dtc
# the same machine as k618_qemu.sh, so that the memory and CPU nodes match
qemu-system-aarch64 -M virt,dumpdtb=$D/qemu.dtb -cpu max,pauth-impdef=on -smp ${QEMU_SMP:-4} -m 3072 -nic none \
	-nographic >/dev/null 2>&1 || true
$DTC -I dtb -O dts -o $D/qemu.dts $D/qemu.dtb 2>/dev/null
python3 -I - $D/qemu.dts $K618/phone/live.dts <<'PY'
import os
import re
import sys
p, live = sys.argv[1], sys.argv[2]
s = open(p).read()
assert "reserved-memory" not in s, "QEMU's DTB already has reserved-memory"
# the phone's own dump entries (99 of them, 47.5 MiB, with the CPU-context and SPR dumps), from its
# live DT, one level up and pointed at our pool; three made-up ones if that is not there
entries = None
if os.path.exists(live):
    t = open(live, encoding="utf-8", errors="replace").read()
    m = re.search(r"\n\t\tmem_dump \{\n(.*?)\n\t\t\};\n", t, re.S)
    if m:
        body = re.sub(r"\n\t\t\tmemory-region = <[^>]*>;", "", "\n" + m.group(1))
        entries = "".join("\n" + line[1:] if line.startswith("\t") else "\n" + line
                          for line in body.split("\n")[1:])
add = '''
\treserved-memory {
\t\t#address-cells = <0x02>;
\t\t#size-cells = <0x02>;
\t\tranges;

\t\tmem_dump_region {
\t\t\tcompatible = "shared-dma-pool";
\t\t\treusable;
\t\t\talignment = <0x00 0x400000>;
\t\t\tsize = <0x00 0x3400000>;
\t\t\tphandle = <0x7f10>;
\t\t};
\t};

'''
made_up = '''
\t\tcompatible = "qcom,mem-dump";

\t\tc0_context {
\t\t\tqcom,dump-size = <0x800>;
\t\t\tqcom,dump-id = <0x00>;
\t\t};

\t\tc100_context {
\t\t\tqcom,dump-size = <0x800>;
\t\t\tqcom,dump-id = <0x01>;
\t\t};

\t\tc200_context {
\t\t\tqcom,dump-size = <0x800>;
\t\t\tqcom,dump-id = <0x02>;
\t\t};'''
add += "\n\tmem_dump {\n\t\tmemory-region = <0x7f10>;" + (entries or made_up) + "\n\t};\n"
i = s.rstrip().rfind("};")          # the closing brace of the root node
assert i > 0, "no root close"
open(p, "w").write(s[:i] + add + s[i:])
print("mem_dump entries:", add.count("qcom,dump-id"), "from the phone's DT" if entries else "made up")
PY
$DTC -I dts -O dtb -o $D/qemu-memdump.dtb $D/qemu.dts
ls -la $D/qemu-memdump.dtb
