#!/bin/bash
# k618_qemu.sh [vendor_boot.img] [seconds, 240]: the 6.12 test boot of the phone, in QEMU instead: our
# GKI Image with the ramdisk the bootloader would build (the ROM's vendor ramdisk, then the test
# image's own ramdisk: Android's generic first-stage init and our /lib/modules), on QEMU's virt board.
# The hardware is not there, so the drivers do not probe; what shows is what does not depend on it --
# the kernel's own start, first-stage init, and every module's load and init -- with a serial console,
# which the phone does not have. Log in $K618/boot-test/qemu.log.
. "$(dirname "$(readlink -f "$0")")/env.sh"
VB=${1:-$NB/nikohyper/img/vendor_boot.img}
T=${2:-240}
MKB=${MKB:-$NB/home/mkbootimg}
OUT=$K618/boot-test
mkdir -p $OUT/qemu
python3 -I $MKB/unpack_bootimg.py --boot_img $VB --out $OUT/qemu/vb > /dev/null || exit 1
# vendor ramdisk first, the boot image's ramdisk after it: as the bootloader concatenates them
cat $OUT/qemu/vb/vendor_ramdisk00 $OUT/stage/ramdisk.lz4 > $OUT/qemu/initrd.img
start=$(date +%s)
# QEMU_SMP: CPUs (the phone has 8); QEMU_APPEND: more of the command line, e.g. what the phone's
# bootloader adds (log_buf_len=2M loglevel=6 printk.always_kmsg_dump=1 ...) or a test image's own;
# QEMU_KERNEL: another Image (k618_irbuild.sh's); QEMU_INITRD: another initrd than vendor + stage;
# QEMU_DTB: a device tree of our own instead of the one QEMU builds (dump it with -M virt,dumpdtb=...
# and add nodes, e.g. to give a test module a device); QEMU_DRIVE: a disk image to attach, e.g. a
# GPT-partitioned scratch file to let a test module find a partition by name and write to it
[ -n "$QEMU_INITRD" ] && cp $QEMU_INITRD $OUT/qemu/initrd.img
timeout $T qemu-system-aarch64 -M virt -cpu max,pauth-impdef=on -smp ${QEMU_SMP:-4} -m 3072 -nic none \
	${QEMU_DTB:+-dtb $QEMU_DTB} ${QEMU_DRIVE:+-drive file=$QEMU_DRIVE,format=raw,if=virtio} \
	-nographic -no-reboot -kernel ${QEMU_KERNEL:-$O/arch/arm64/boot/Image} -initrd $OUT/qemu/initrd.img \
	-append "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 printk.devkmsg=on ${QEMU_APPEND:-}" > $OUT/qemu.log 2>&1
echo "qemu exit $? after $(( $(date +%s) - start )) s; $(wc -l < $OUT/qemu.log) lines in $OUT/qemu.log"
grep -a -m1 "Linux version" $OUT/qemu.log | cut -c1-120
grep -a -nE "Kernel panic|Oops|BUG:|Unable to handle|Internal error|Call trace|WARNING:|init: (Failed|Reboot|.*fatal)|Attempted to kill init|Unknown symbol|disagrees|Run /init|First stage" $OUT/qemu.log | head -40
