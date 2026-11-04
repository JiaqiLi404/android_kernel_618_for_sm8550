#!/bin/bash
# drafts/k618_tlmm_qemu.sh: build k618_tlmm.ko against out-gki (as k618_bootimg.sh builds EXTRA modules) and run it in QEMU:
# A = QEMU's DT plus a fake kalama TLMM node whose pin 0 is QEMU's pl061 (pins 1/2 reserved,
# 3.. out of range), B = no node (base= path). Each run's finit_module result goes to the log.
set -u
KD=$(cd "$(dirname "$0")/.." && pwd)
. "$KD/env.sh"
export NB K618
SPW=$KD/drafts	# k618_qinit.c
. $KD/env.sh
MD=$K618/tlmm-mod
rm -rf $MD; mkdir -p $MD
cp $KD/k618_tlmm.c $MD/ && echo "obj-m += k618_tlmm.o" > $MD/Kbuild
PATH=$CLANG:${R:+$R/bin:}$TOOLS:/usr/bin:/bin KBUILD_GENDWARFKSYMS_STABLE=1 \
	make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$MD -j4 W=1 modules > $MD/build.log 2>&1 || { tail -20 $MD/build.log; exit 1; }
grep -E "warning|error" $MD/build.log | head
ls -la $MD/k618_tlmm.ko
$CLANG/llvm-modinfo $MD/k618_tlmm.ko 2>/dev/null | grep -E "vermagic|depends|parm" || \
	strings $MD/k618_tlmm.ko | grep -E "^vermagic=|^parm"
echo "== undefined symbols"; $CLANG/llvm-nm -u $MD/k618_tlmm.ko | tr -s ' ' | cut -d' ' -f3 | tr '\n' ' '; echo

Q=$K618/tlmm-q; rm -rf $Q; mkdir -p $Q
# QEMU's own DT, with the machine options k618_qemu.sh uses
qemu-system-aarch64 -M virt,dumpdtb=$Q/virt.dtb -cpu max,pauth-impdef=on -smp 4 -m 3072 -nic none -nographic > /dev/null 2>&1
DTC=$O/scripts/dtc/dtc
$DTC -q -I dtb -O dts -o $Q/virt.dts $Q/virt.dtb || exit 1
python3 - $Q/virt.dts <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
node='''
	tlmm@8f30000 {
		compatible = "qcom,kalama-pinctrl-v2";
		reg = <0x00 0x8f30000 0x00 0x103000>;
		gpio-reserved-ranges = <1 1>;
		qcom,gpios-reserved = <2>;
	};
'''
i=s.rindex('};')
s=s[:i]+node+s[i:]
open(p,'w').write(s)
PY
$DTC -q -I dts -O dtb -o $Q/tlmm.dtb $Q/virt.dts || exit 1
mk() {	# mk <name> <run>...: an init with those runs, and its initrd
	local n=$1; shift
	: > $Q/tlmm_runs.h
	for r in "$@"; do echo "	\"$r\"," >> $Q/tlmm_runs.h; done
	$CLANG/clang --target=aarch64-linux-gnu -O2 -nostdlib -static -ffreestanding -fno-stack-protector \
		-fuse-ld=lld -Wl,--build-id=none -s -I$Q -o $Q/init-$n $SPW/k618_qinit.c || exit 1
	cat > $Q/list-$n <<L
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0644 0 0 c 1 11
file /init $Q/init-$n 0755 0 0
file /k618_tlmm.ko $MD/k618_tlmm.ko 0644 0 0
L
	$O/usr/gen_init_cpio $Q/list-$n > $Q/initrd-$n.cpio || exit 1
}
mk A "" "pins=0,1,2,3 claim=0" "pins=0 claim=5" "base=4096 pins=0" "base=0x9030000 pins=0" "all=1" "pins=0 cfg=0:1:2:3" "pins=0 cfg=5:1:0:0" "pins=0 cfg=0:10:0:0" "pins=0 cfg=0:1" "all=1 cfg=0:1:0:0,,"
mk B "" "base=0x9030000 pins=0,300"
for n in A B; do
	echo "===== QEMU $n"
	dtb=; [ $n = A ] && dtb=$Q/tlmm.dtb
	QEMU_INITRD=$Q/initrd-$n.cpio QEMU_DTB=$dtb QEMU_KERNEL=$O/arch/arm64/boot/Image QEMU_SMP=4 \
		QEMU_APPEND="ignore_loglevel" bash $KD/k618_qemu.sh "" 90 | head -3
	cp $K618/boot-test/qemu.log $Q/qemu-$n.log
	grep -a -E "k618_tlmm|tlmm_q|Unable to handle|Internal error|Oops|BUG:|WARNING:|Kernel panic|SError|taint" $Q/qemu-$n.log | cut -c1-200
done
