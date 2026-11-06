#!/bin/bash
# k618_tof.sh [jobs]: the 13 Ultra's ToF ranging sensor -- ST VL53L5 on SPI, "st,stmvl53l5" in its
# device tree. stmvl53l5.ko from Xiaomi's own 13 Ultra source: MiCode Xiaomi_Kernel_OpenSource
# branch ishtar-t-oss, drivers/input/stmvl53l5 (ST driver 3.3.0.0 -- the stock module's version:
# the same functions, the 'p' ioctls the camera HAL calls, the firmware by the names its node
# gives, stmvl53l5.bin / stmvl53l8.bin, from the vendor partition). bootstrap checks out that
# directory alone into $K618/src/ishtar/tree (pinned in provenance/input-sources.json). The 12 Pro's
# copy (zeus-s-oss drivers/input/stmvl53l5) is ST's older driver, one 'a' ioctl: not the 13 Ultra's.
# k618_tof_fix.py makes the 6.18 edits; built like k618_touch.sh, the result goes to $K618/tpmods.
# Only the 13 Ultra's device tree has the part: elsewhere the module loads and binds nothing.
set -u
J=${1:-8}
KD=$(cd "$(dirname "$0")" && pwd)
. $KD/env.sh
Z=$K618/src/ishtar/tree/drivers/input/stmvl53l5
D=$K618/vendor/xiaomi/tof
LOG=$K618/tof.log

[ -f $Z/stmvl53l5_module.c ] || { echo "k618_tof: no 13 Ultra ToF source at $Z (see the header)"; exit 1; }
rm -rf $D && mkdir -p $D && cp $Z/*.c $Z/*.h $D/ || exit 1
python3 -I $KD/k618_tof_fix.py $D || exit 1
cat > $D/Kbuild <<'EOF'
obj-m += stmvl53l5.o
stmvl53l5-objs := stmvl53l5_register_utils.o stmvl53l5_power.o stmvl53l5_platform.o stmvl53l5_load_firmware.o stmvl53l5_module.o
EOF
cd $W/common || exit 1
export PATH=$CLANG:$W/prebuilts/clang-tools/linux-x86/bin:$W/prebuilts/kernel-build-tools/linux_musl-x86/bin:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
KBUILD_GENDWARFKSYMS_STABLE=1 make -j$J O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	CC="$CLANG/clang" LD="$CLANG/ld.lld" M=$D \
	KCFLAGS="-Wno-missing-prototypes -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable" \
	modules > $LOG 2>&1
ret=$?
if [ $ret -ne 0 ] || [ ! -f $D/stmvl53l5.ko ]; then
	echo "k618_tof: build failed ($ret), $(grep -c 'error:' $LOG) error lines -- $LOG"
	grep -E "error:|ERROR:" $LOG | head -20
	exit 1
fi
mkdir -p $K618/tpmods
cp $D/stmvl53l5.ko $K618/tpmods/ || exit 1
echo "k618_tof: stmvl53l5.ko $(stat -c %s $D/stmvl53l5.ko) bytes -> $K618/tpmods/ ($(grep -c 'warning:' $LOG) warnings)"
echo "k618_tof: binds $(strings $D/stmvl53l5.ko | grep -x 'st,stmvl53l5' | head -1), misc device $(strings $D/stmvl53l5.ko | grep -x stmvl53l5 | head -1)"
