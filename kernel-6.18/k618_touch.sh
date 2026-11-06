#!/bin/bash
# k618_touch.sh [jobs]: the touch screen -- ST FTS over SPI (fts_touch_spi.ko) and Xiaomi's
# xiaomi_touch.ko, the two modules the phone loads for it from vendor_dlkm (rom_modules.load names
# both, fts_touch_spi depends on xiaomi_touch and panel_event_notifier).
#
# The phone's own build comes from Xiaomi's unpublished tree -- its fts.c path reads
# vendor/xiaomi/proprietary/touch/touchfeature_v2/touch_driver/m2/fts_spi -- and none of the 5.15
# kernel branches on MiCode carries it. The nearest published source is the 12 Pro's (MiCode
# Xiaomi_Kernel_OpenSource, branch zeus-s-oss, drivers/input/touchscreen/{fts_spi,xiaomi}): the same
# driver ("st,spi") before THP, touch host processing, where the IC hands raw frames to a userspace
# algorithm. Without THP the IC reports the touches itself, as touch screens normally do.
#
# Built as external modules against the GKI and the soc tree, as k618_rpc515.sh does;
# k618_touch_fix.py holds every change, first of all: no firmware update at probe -- the 12 Pro's
# tree must never write this phone's touch IC. Sources are fetched with
#   git clone --depth 1 --filter=blob:none --sparse -b zeus-s-oss \
#       https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git $K618/src/zeus-touch/repo
#   git -C ... sparse-checkout set drivers/input/touchscreen/fts_spi drivers/input/touchscreen/xiaomi
set -u
J=${1:-8}
KD=$(cd "$(dirname "$0")" && pwd)
. $KD/env.sh
Z=$K618/src/zeus-touch/repo/drivers/input/touchscreen
D=$K618/vendor/xiaomi/touch
LOG=$K618/touch.log

[ -d $Z/fts_spi ] && [ -d $Z/xiaomi ] || { echo "k618_touch: no zeus touch sources at $Z (see the header)"; exit 1; }
[ -f $S/Module.symvers ] || { echo "k618_touch: no soc Module.symvers -- build the soc modules"; exit 1; }

rm -rf $D
mkdir -p $D
cp -r $Z/fts_spi $Z/xiaomi $D/ || exit 1
rm -f $D/fts_spi/fts_lib/*.rej $D/fts_spi/fts_lib/*.orig
python3 -I $KD/k618_touch_fix.py $D || exit 1


python3 -I $KD/k618_touch_gesture.py $D || exit 1
if [ "${THP:-1}" != 0 ]; then python3 -I $KD/drafts/k618_touch_thp.py $D || exit 1; fi
# the 13 and the 13 Ultra: THP as each node says (in k618_touch_thp.py), the 13 Ultra's second
# interrupt pin chosen by board id as its stock driver does
python3 -I $KD/k618_touch_devices.py $D || exit 1
cat > $D/Kbuild <<'EOF'
obj-m += xiaomi/ fts_spi/
EOF
cd $W/common || exit 1
export PATH=$CLANG:$W/prebuilts/clang-tools/linux-x86/bin:$W/prebuilts/kernel-build-tools/linux_musl-x86/bin:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
KBUILD_EXTRA_SYMBOLS="$S/Module.symvers" make -j$J SOC=$S O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	CC="$CLANG/clang" LD="$CLANG/ld.lld" M=$D CONFIG_ARCH_KALAMA=y \
	CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE=m CONFIG_TOUCHSCREEN_ST_FTS_V521_SPI=m \
	NOSTDINC_FLAGS="-nostdinc -I$S/include -I$S/include/uapi" \
	KCFLAGS="-DCONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE=1 -DCONFIG_TOUCHSCREEN_ST_FTS_V521_SPI=1 -DCONFIG_QCOM_PANEL_EVENT_NOTIFIER_MODULE=1 -Wno-missing-prototypes -Wno-implicit-fallthrough -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable" \
	modules > $LOG 2>&1
ret=$?
if [ $ret -ne 0 ] || [ ! -f $D/fts_spi/fts_touch_spi.ko ] || [ ! -f $D/xiaomi/xiaomi_touch.ko ]; then
	echo "k618_touch: build failed ($ret), $(grep -c 'error:' $LOG) error lines -- $LOG"
	grep -E "error:|ERROR:" $LOG | head -20
	exit 1
fi


grep -q "FW_UPDATE_ON_PROBE" $D/fts_spi/fts.h && ! grep -q "^#define FW_UPDATE_ON_PROBE" $D/fts_spi/fts.h ||
	{ echo "k618_touch: FW_UPDATE_ON_PROBE still on"; exit 1; }
strings $D/fts_spi/fts_touch_spi.ko | grep -q "k618: firmware update and panel init skipped" ||
	{ echo "k618_touch: the firmware update path is not cut"; exit 1; }


strings $D/fts_spi/fts_touch_spi.ko | grep -E '^depends=' | grep -qE '(=|,)panel_event_notifier(,|$)' ||
	{ echo "k618_touch: fts_touch_spi does not import panel_event_notifier"; exit 1; }
strings $D/fts_spi/fts_touch_spi.ko | grep -q "k618: flash write refused" ||
	{ echo "k618_touch: the flash guard is missing"; exit 1; }
mkdir -p $K618/tpmods
cp $D/fts_spi/fts_touch_spi.ko $D/xiaomi/xiaomi_touch.ko $K618/tpmods/ || exit 1
echo "k618_touch: fts_touch_spi.ko $(stat -c %s $D/fts_spi/fts_touch_spi.ko) bytes, xiaomi_touch.ko $(stat -c %s $D/xiaomi/xiaomi_touch.ko) bytes -> $K618/tpmods/"
echo "k618_touch: compatibles $(strings $D/fts_spi/fts_touch_spi.ko | grep -x 'st,spi' | head -1) / $(strings $D/xiaomi/xiaomi_touch.ko | grep -x 'xiaomi-touch' | head -1)"
