#!/bin/bash


set -u
KD=$(cd "$(dirname "$0")/.." && pwd)
. "$KD/env.sh"
export NB K618
SPW=$(cd "$(dirname "$0")" && pwd)	# drafts/: k618_camfix_test.c and k618_qinit.c
. $KD/env.sh
MD=$K618/camfix-mod
rm -rf $MD; mkdir -p $MD
cp $SPW/k618_camfix_test.c $MD/ && echo "obj-m += k618_camfix_test.o" > $MD/Kbuild
PATH=$CLANG:${R:+$R/bin:}$TOOLS:/usr/bin:/bin KBUILD_GENDWARFKSYMS_STABLE=1 \
	make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$MD -j4 W=1 modules > $MD/build.log 2>&1 || { tail -20 $MD/build.log; exit 1; }
grep -E "warning|error" $MD/build.log | head
ls -la $MD/k618_camfix_test.ko
strings $MD/k618_camfix_test.ko | grep -E "^vermagic="
Q=$K618/tlmm-q
cat > $Q/tlmm_runs.h <<R
	"",
	"cci=0 csiphy=0",
R
$CLANG/clang --target=aarch64-linux-gnu -O2 -nostdlib -static -ffreestanding -fno-stack-protector \
	-fuse-ld=lld -Wl,--build-id=none -s -I$Q '-DKO="/k618_camfix_test.ko"' '-DMODNAME="k618_camfix_test"' \
	-o $Q/init-F $SPW/k618_qinit.c || exit 1
cat > $Q/list-F <<L
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0644 0 0 c 1 11
file /init $Q/init-F 0755 0 0
file /k618_camfix_test.ko $MD/k618_camfix_test.ko 0644 0 0
L
$O/usr/gen_init_cpio $Q/list-F > $Q/initrd-F.cpio || exit 1
QEMU_INITRD=$Q/initrd-F.cpio QEMU_KERNEL=$O/arch/arm64/boot/Image QEMU_SMP=4 \
	QEMU_APPEND="ignore_loglevel" bash $KD/k618_qemu.sh "" 90 | head -2
cp $K618/boot-test/qemu.log $Q/qemu-F.log
grep -a -E "k618_camfix|tlmm_q|Unable to handle|Internal error|Oops|BUG:|WARNING:|Kernel panic" $Q/qemu-F.log | grep -v taint | cut -c1-200
