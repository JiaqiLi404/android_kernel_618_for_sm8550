#!/bin/bash
# k618_irbuild.sh [vendor_boot.img]: a kernel that carries the test image's ramdisk itself, for when
# the phone's bootloader does not use the ramdisk in a `fastboot boot` image (it may take the one of
# the init_boot partition instead -- on this phone that is the ROM's, with KernelSU's 5.15 module).
#
# A copy of out-gki ($K618/out-gki-ir) gets CONFIG_INITRAMFS_SOURCE = the ramdisk the bootloader would
# have stacked -- vendor_boot's vendor ramdisk without its 5.15 modules (k618_cpiofilter.py; they are
# never loaded and would make the kernel 30 MB bigger), then k618_bootimg.sh's stage ramdisk (the
# ROM's plain generic ramdisk and our /lib/modules), gzip-compressed by the build -- and
# CONFIG_INITRAMFS_FORCE, so the ramdisk the bootloader passes is not unpacked (its bootconfig at the
# end is still read). Exported symbols and their CRCs must stay those of out-gki, or our modules would
# not load: checked against the vmlinux lines of its Module.symvers.
#
# Run k618_bootimg.sh first (its stage is the source), then this, then k618_bootimg.sh again with
# KERNEL=$K618/out-gki-ir/arch/arm64/boot/Image (or REUSE_STAGE=1 to keep the same stage).
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
VB=${1:-$NB/nikohyper/img/vendor_boot.img}
MKB=${MKB:-$NB/home/mkbootimg}
OI=$K618/out-gki-ir
T=$K618/boot-test
CPIO=$T/initramfs.cpio
[ -f $T/stage/ramdisk.lz4 ] || { echo "no stage ramdisk: run k618_bootimg.sh first"; exit 1; }
rm -rf $T/irvb && mkdir -p $T/irvb
python3 -I $MKB/unpack_bootimg.py --boot_img $VB --out $T/irvb > /dev/null
# both are LZ4 (legacy) streams; the kernel's unpacker takes concatenated cpio archives
lz4 -dcq $T/irvb/vendor_ramdisk00 > $T/irvb/vendor.cpio
python3 -I $K618_DIR/k618_cpiofilter.py $T/irvb/vendor.cpio $CPIO.new '^lib/modules/.*\.ko$'
lz4 -dcq $T/stage/ramdisk.lz4 >> $CPIO.new
# only touch the file when it changed, so that an unchanged ramdisk does not relink the kernel
if cmp -s $CPIO.new $CPIO; then rm $CPIO.new; else mv $CPIO.new $CPIO; fi
ls -la $CPIO | awk '{print "initramfs", $5, "bytes"}'
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# KBUILD_GENDWARFKSYMS_STABLE as in k618_gki.sh: the kernel these modules are built against has
# its CRCs in stable mode, and a module's own imports must be read the same way.
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1
M="make -C $W/common O=$OI ARCH=arm64 LLVM=1 LLVM_IAS=1 PAHOLE=$TOOLS/pahole"
[ -n "$R" ] && M="$M RUSTC=$R/bin/rustc HOSTRUSTC=$R/bin/rustc BINDGEN=$W/prebuilts/clang-tools/linux-x86/bin/bindgen RUST_LIB_SRC=$R/lib/rustlib/src/rust/library"
# the copy: everything of out-gki but its two settings; refreshed when out-gki was rebuilt
if [ ! -f $OI/.config ] || [ $O/vmlinux -nt $OI/.k618-copied ]; then
	rsync -a --delete --exclude .k618-copied $O/ $OI/
	touch $OI/.k618-copied
fi
$W/common/scripts/config --file $OI/.config --set-str INITRAMFS_SOURCE $CPIO --enable INITRAMFS_FORCE
$M olddefconfig > $OI/../gki-ir-config.log 2>&1
grep -E '^CONFIG_(INITRAMFS_SOURCE|INITRAMFS_FORCE|CMDLINE_EXTEND)=' $OI/.config
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
