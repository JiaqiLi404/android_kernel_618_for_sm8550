#!/bin/bash
set -u
KD=$(cd "$(dirname "$0")/.." && pwd)
. "$KD/env.sh"
export NB K618
SPW=$(cd "$(dirname "$0")" && pwd)	# drafts/
. $KD/env.sh
TP=$K618/vendor/qcom/opensource
MD=$K618/synxq-mod
Q=$K618/synxq
rm -rf $MD $Q; mkdir -p $MD $Q/m
cp $SPW/k618_synx_bindtest.c $MD/
cat > $MD/Kbuild <<K
obj-m += k618_synx_bindtest.o
ccflags-y += -I$TP/synx-kernel/msm/synx -I$TP/synx-kernel/include -I$TP/synx-kernel/include/uapi -I$TP/synx-kernel/include/uapi/synx/media
K
PATH=$CLANG:${R:+$R/bin:}$TOOLS:/usr/bin:/bin KBUILD_GENDWARFKSYMS_STABLE=1 \
	make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$MD KBUILD_EXTRA_SYMBOLS=$K618/tp-log/synx.symvers \
	-j4 modules > $MD/build.log 2>&1 || { tail -20 $MD/build.log; exit 1; }
grep -E "warning|error" $MD/build.log | head
# synx-driver's dependencies, in load order
python3 -I - $K618/tpmods/synx-driver.ko $K618/soc $K618/tpmods $O > $Q/order.txt <<'EOF' || exit 1
import os, re, sys
top, dirs = sys.argv[1], sys.argv[2:]
idx = {}
for d in dirs:
    for dp, _dn, fns in os.walk(d):
        for fn in fns:
            if fn.endswith(".ko"):
                idx.setdefault(fn[:-3].replace("-", "_"), os.path.join(dp, fn))
def deps(path):
    m = re.search(rb"\0depends=([^\0]*)\0", b"\0" + open(path, "rb").read())
    return [x for x in m.group(1).decode().split(",") if x] if m else []
order, seen = [], set()
def visit(path):
    for dname in deps(path):
        key = dname.replace("-", "_")
        if key in seen:
            continue
        seen.add(key)
        if key not in idx:
            sys.exit(f"no module for dependency {dname}")
        visit(idx[key])
        order.append(idx[key])
visit(top)
order.append(top)
print("\n".join(order))
EOF
echo "load order: $(xargs -n1 basename < $Q/order.txt | tr '\n' ' ')"
: > $Q/mods.h
{ echo "dir /dev 0755 0 0"; echo "nod /dev/console 0600 0 0 c 5 1"; echo "nod /dev/kmsg 0644 0 0 c 1 11"
  echo "dir /m 0755 0 0"; echo "file /init $Q/init-S 0755 0 0"; } > $Q/list
for k in $(cat $Q/order.txt) $MD/k618_synx_bindtest.ko; do
	b=$(basename $k); echo "\"/m/$b\"," >> $Q/mods.h; echo "file /m/$b $k 0644 0 0" >> $Q/list
done
$CLANG/clang --target=aarch64-linux-gnu -O2 -nostdlib -static -ffreestanding -fno-stack-protector \
	-fuse-ld=lld -Wl,--build-id=none -s -I$Q -o $Q/init-S $SPW/k618_synx_v1_qtest.c || exit 1
$O/usr/gen_init_cpio $Q/list > $Q/initrd.cpio || exit 1
QEMU_INITRD=$Q/initrd.cpio QEMU_KERNEL=$O/arch/arm64/boot/Image QEMU_SMP=4 \
	QEMU_APPEND="ignore_loglevel" bash $KD/k618_qemu.sh "" 120 | head -2
cp $K618/boot-test/qemu.log $Q/qemu.log
L=$Q/qemu.log
grep -a "synx_q: " $L | sed 's/^.*synx_q: //' > $Q/results.txt
echo "checks: $(grep -c ' PASS$' $Q/results.txt) pass, $(grep -c ' FAIL$' $Q/results.txt) fail"
grep -E " FAIL$|^done" $Q/results.txt
echo "== kernel lines from synx / the bind test"
grep -a -E "synx|k618_synx_bindtest" $L | grep -a -v "synx_q: " | sed 's/^\[[ 0-9.]*\] //' | sort | uniq -c | sort -rn | head -20 | cut -c1-160
for p in "Unable to handle kernel" "Internal error" "BUG:" "WARNING:" "Oops" "UBSAN"; do
	printf "%-24s %s\n" "$p" "$(grep -ac "$p" $L)"
done
