#!/bin/bash
# k618_irbuild.sh: a kernel that carries our ramdisk layer itself, for when the phone's bootloader
# does not use the ramdisk in a `fastboot boot` image (it takes the init_boot partition's instead --
# on this phone the ROM's, with KernelSU's 5.15 module -- and vendor_boot's, with its 5.15 modules).
#
# A copy of out-gki ($K618/out-gki-ir) gets CONFIG_INITRAMFS_SOURCE = k618_bootimg.sh's overlay.cpio
# (our /lib/modules, its module lists and the batch loader, nothing else), gzip-compressed by the
# build. The kernel unpacks it AFTER the ramdisk the bootloader hands over (k618_ports.py
# initramfs_last), so the phone's own vendor ramdisk and init_boot -- or TWRP's, in recovery -- are
# what runs, with our modules and lists in place of the 5.15 ones. INITRAMFS_FORCE stays off: with
# it the bootloader's ramdisk would not be unpacked at all, so a recovery ramdisk (TWRP) would never
# start and another model would get this phone's vendor ramdisk in place of its own.
# Exported symbols and their CRCs must stay those of out-gki, or our modules would not load: checked
# against the vmlinux lines of its Module.symvers.
#
# Run k618_bootimg.sh first (its stage is the source), then this, then k618_bootimg.sh again with
# KERNEL=$K618/out-gki-ir/arch/arm64/boot/Image (or REUSE_STAGE=1 to keep the same stage).
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
OI=$K618/out-gki-ir
T=$K618/boot-test
CPIO=$T/initramfs.cpio
[ -f $T/stage/overlay.cpio ] || { echo "no stage layer: run k618_bootimg.sh first"; exit 1; }
# without initramfs_last the built-in layer would go FIRST and the bootloader's 5.15 lists over it
grep -q "k618 initramfs_last" $W/common/init/initramfs.c || { echo "initramfs_last is not in the tree (k618_gki.sh)"; exit 1; }
# only touch the file when it changed, so that an unchanged ramdisk does not relink the kernel
cmp -s $T/stage/overlay.cpio $CPIO || cp $T/stage/overlay.cpio $CPIO
ls -la $CPIO | awk '{print "initramfs", $5, "bytes"}'
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# KBUILD_GENDWARFKSYMS_STABLE as in k618_gki.sh: the kernel these modules are built against has
# its CRCs in stable mode, and a module's own imports must be read the same way.
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1
M="make -C $W/common O=$OI ARCH=arm64 LLVM=1 LLVM_IAS=1 PAHOLE=$TOOLS/pahole"
[ -n "$R" ] && M="$M RUSTC=$R/bin/rustc HOSTRUSTC=$R/bin/rustc BINDGEN=$W/prebuilts/clang-tools/linux-x86/bin/bindgen RUST_LIB_SRC=$R/lib/rustlib/src/rust/library"
# the copy: everything of out-gki but its built-in ramdisk; refreshed when out-gki was rebuilt
if [ ! -f $OI/.config ] || [ $O/vmlinux -nt $OI/.k618-copied ]; then
	rsync -a --delete --exclude .k618-copied $O/ $OI/
	touch $OI/.k618-copied
fi
# INITRAMFS_FORCE off: the bootloader's ramdisk is unpacked, then ours
$W/common/scripts/config --file $OI/.config --set-str INITRAMFS_SOURCE $CPIO --disable INITRAMFS_FORCE
$M olddefconfig > $OI/../gki-ir-config.log 2>&1
grep -E '^CONFIG_(INITRAMFS_SOURCE|CMDLINE_EXTEND)=' $OI/.config
! grep -q '^CONFIG_INITRAMFS_FORCE=y' $OI/.config || { echo "INITRAMFS_FORCE is still on"; exit 1; }
start=$(date +%s)
$M -j${JOBS:-4} Image > $OI/../gki-ir-build.log 2>&1 || { tail -30 $OI/../gki-ir-build.log; echo "BUILD FAILED"; exit 1; }
echo "built in $(( $(date +%s) - start )) s"
# vmlinux.symvers is written again at every link: the exports and CRCs of this vmlinux, against the
# vmlinux lines of out-gki's Module.symvers (out-gki keeps no vmlinux.symvers of its own)
[ $OI/vmlinux.symvers -nt $OI/.k618-copied ] || { echo "vmlinux was not relinked"; exit 1; }
if cmp -s <(awk -F'\t' '$3 == "vmlinux"' $O/Module.symvers | sort) <(sort $OI/vmlinux.symvers); then
	echo "exports and CRCs: same as out-gki ($(grep -c . $OI/vmlinux.symvers))"
else
	echo "exports or CRCs DIFFERENT from out-gki"; exit 1
fi
cmp -s $O/include/config/kernel.release $OI/include/config/kernel.release || { echo "kernel release DIFFERENT"; exit 1; }
ls -la $OI/arch/arm64/boot/Image | awk '{print "Image", $5, "bytes"}'
