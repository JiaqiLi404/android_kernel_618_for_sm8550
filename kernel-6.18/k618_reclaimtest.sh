#!/bin/bash
# k618_reclaimtest.sh [seconds, 480]: QEMU check of k618_ports.py's look_around, kcompressd and
# kswapd_mid on the GKI build in $O (run it after k618_gki.sh). QEMU only, nothing for the phone.
# It boots $O's Image on QEMU's virt board (8 CPUs, 1 GB) with a ramdisk of its own -- no Android
# init, so no 7.5 s limit: k618_reclaimtest.c, static, as /init, and $O's zsmalloc.ko and zram.ko
# -- and with QEMU's own device tree given the 13 Pro's capacity-dmips-mhz (820 x 3, 1800 x 4,
# 1894), so that the kernel sees a little, a middle and a prime cluster. The program sets zram0 up
# as swap, the classic LRU and watermark_scale_factor 200 as on the phone, and runs three rounds of
# memory pressure (as built / all three off / on with reclaim_cpus 2); k618_reclaimtest.c says what
# each checks. PASS: its last line is "k618_reclaimtest: PASS" and the kernel printed no warning,
# BUG, oops or list corruption. Exit 0 on PASS. Log in $K618/reclaimtest/qemu.log.
# HARNESS=1: only the pressure and the data, for a kernel without the three ports.
. "$(dirname "$(readlink -f "$0")")/env.sh"
T=${1:-480}
D=$K618/reclaimtest
DTC=$TOOLS/dtc
IMG=${QEMU_KERNEL:-$O/arch/arm64/boot/Image}
mkdir -p $D
for f in $IMG $O/mm/zsmalloc.ko $O/drivers/block/zram/zram.ko $O/usr/gen_init_cpio $DTC; do
	[ -e $f ] || { echo "k618_reclaimtest.sh: no $f (k618_gki.sh first)"; exit 2; }
done
# the test program is /init: static, for arm64
aarch64-linux-gnu-gcc -static -O2 -Wall -o $D/init $K618_DIR/k618_reclaimtest.c || exit 2
# QEMU's device tree for this very machine (the same -M/-cpu/-smp/-m as the run below), each CPU
# node given its capacity-dmips-mhz: the kernel normalizes them to 443 / 973 / 1024 at boot
QM="-cpu max,pauth-impdef=on -smp 8 -m 1024"
qemu-system-aarch64 -M virt,dumpdtb=$D/virt.dtb $QM -nographic > $D/dumpdtb.log 2>&1
$DTC -q -I dtb -O dts -o $D/virt.dts $D/virt.dtb || exit 2
awk '
	/^[ \t]*cpu@[0-9]+ \{/ {
		print
		n = $0
		sub(/^[ \t]*cpu@/, "", n)
		sub(/ \{.*/, "", n)
		n = n + 0
		cap = n < 3 ? 820 : (n < 7 ? 1800 : 1894)
		printf "\t\t\tcapacity-dmips-mhz = <%d>;\n", cap
		next
	}
	{ print }
' $D/virt.dts > $D/virt-cap.dts
[ "$(grep -c capacity-dmips-mhz $D/virt-cap.dts)" = 8 ] || { echo "k618_reclaimtest.sh: the CPU nodes of $D/virt.dts were not found"; exit 2; }
$DTC -q -I dts -O dtb -o $D/virt-cap.dtb $D/virt-cap.dts || exit 2
# the ramdisk
cat > $D/cpio.list <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
file /init $D/init 0755 0 0
file /zsmalloc.ko $O/mm/zsmalloc.ko 0644 0 0
file /zram.ko $O/drivers/block/zram/zram.ko 0644 0 0
EOF
$O/usr/gen_init_cpio $D/cpio.list > $D/initrd.cpio || exit 2
start=$(date +%s)
timeout $T qemu-system-aarch64 -M virt $QM -dtb $D/virt-cap.dtb -nographic -no-reboot -nic none \
	-kernel $IMG -initrd $D/initrd.cpio \
	-append "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 rdinit=/init printk.devkmsg=on ${HARNESS:+k618rt.harness} ${QEMU_APPEND:-}" \
	> $D/qemu.log 2>&1
rc=$?
echo "qemu exit $rc after $(( $(date +%s) - start )) s; $(wc -l < $D/qemu.log) lines in $D/qemu.log"
grep -a "k618_reclaimtest:" $D/qemu.log
# (a KFENCE report starts with "BUG: KFENCE")
BADRE="Kernel panic|Oops|BUG:|Unable to handle|Internal error|Call trace|WARNING:|bad page|list_add corruption|list_del corruption|refcount_t:|UBSAN|rcu: INFO|soft lockup|blocked for more than"
problems=$(grep -a -cE "$BADRE" $D/qemu.log)
if [ "$problems" != 0 ]; then
	echo "kernel problems ($problems lines):"
	grep -a -nE "$BADRE" $D/qemu.log | head -20
fi
if grep -aq "k618_reclaimtest: PASS" $D/qemu.log && [ "$problems" = 0 ]; then
	echo "k618_reclaimtest.sh: PASS"
	exit 0
fi
echo "k618_reclaimtest.sh: FAIL"
exit 1
