#!/bin/bash
# k618_cs35l41.sh [jobs]: the speaker amplifiers -- two Cirrus Logic CS35L41 on I2C bus 2
# (cs35l41@40 "B" bottom, cs35l41@42 "T" top, also the earpiece), built from the upstream 6.18
# driver as one external module named like the phone's own: cs35l41_dlkm.ko.
#
# The phone's 5.15 kernel drives them with Xiaomi's cs35l41_dlkm.ko (Cirrus Logic's Android driver
# with its own wm_adsp; no source published), and the GKI has no CS35L41 at all
# (CONFIG_SND_SOC_CS35L41_I2C not set; CS_DSP / WM_ADSP not built either). So the upstream driver
# and its DSP layers come over from the GKI's own tree, $W/common:
#   drivers/firmware/cirrus/cs_dsp.c (+ its private cs_dsp.h)   the Halo DSP loader
#   sound/soc/codecs/wm_adsp.c, wm_adsp.h                        the ASoC side of it
#   sound/soc/codecs/cs35l41-lib.c, cs35l41.c, cs35l41.h, cs35l41-i2c.c
# linked into one module, as the stock one was: the phone's module lists name cs35l41_dlkm.ko, so
# the test images' batch (k618_bootimg.sh, by name) loads it without a RENAME entry, and nothing in
# the GKI exports any of these symbols (no clash).
#
# k618_cs35l41_fix.py holds every change and says why; in short: the ROM's tuning file names
# (B-/T-cs35l41-dsp1-spk-prot.bin), protection firmware never started without its tuning, the
# per-unit calibration handed to the DSP before it starts, the control names and DAPM switches the
# ROM's mixer paths set, the 32-bit TDM slot width, the older devicetree GPIO binding (the
# interrupt line), no DSP hibernation (k618_hibernate=0, as the stock here), the use-case delta files.
#
# The machine side (both amps on TDM-LPAIF-RX-SECONDARY, DSP_A, codec as clock consumer, SCLK =
# rate x 32 x 4) is a separate techpack edit: k618_audio_amp_edits.py (into k618_tp_fix.py).
#
# Knobs: D=<staging dir> (default $K618/vendor/cirrus/cs35l41), LOG=<file>, INSTALL=0 (build and
# check only, no copy into $K618/tpmods).
set -u
J=${1:-8}
KD=$(cd "$(dirname "$0")" && pwd)
if [ -f $KD/env.sh ]; then . $KD/env.sh; else . $KD/../env.sh; fi
C=$W/common
D=${D:-$K618/vendor/cirrus/cs35l41}
LOG=${LOG:-$K618/cs35l41.log}
INSTALL=${INSTALL:-1}

[ -f $C/sound/soc/codecs/cs35l41.c ] || { echo "k618_cs35l41: no $C/sound/soc/codecs/cs35l41.c"; exit 1; }
[ -f $O/Module.symvers ] || { echo "k618_cs35l41: no $O/Module.symvers -- build the GKI (k618_gki.sh)"; exit 1; }

rm -rf $D
mkdir -p $D
cp $C/drivers/firmware/cirrus/cs_dsp.c $C/drivers/firmware/cirrus/cs_dsp.h \
	$C/sound/soc/codecs/wm_adsp.c $C/sound/soc/codecs/wm_adsp.h \
	$C/sound/soc/codecs/cs35l41-lib.c $C/sound/soc/codecs/cs35l41.c \
	$C/sound/soc/codecs/cs35l41.h $C/sound/soc/codecs/cs35l41-i2c.c $D/ || exit 1
python3 -I $KD/k618_cs35l41_fix.py $D || exit 1
cat > $D/Kbuild <<'EOF'
# one module, as the phone's own: the DSP loader, its ASoC layer, the CS35L41 library, codec, I2C
obj-m += cs35l41_dlkm.o
cs35l41_dlkm-y := cs_dsp.o wm_adsp.o cs35l41-lib.o cs35l41.o cs35l41-i2c.o
ccflags-y += -I$(src)
EOF

cd $C || exit 1
export PATH=$CLANG:$W/prebuilts/clang-tools/linux-x86/bin:$W/prebuilts/kernel-build-tools/linux_musl-x86/bin:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# Upstream code against the GKI's own configuration: no vendor calls, so neither the soc headers
# nor tp-autoconf.h are wanted here (the soc tree shadows no header these files include -- it has
# no include/sound, and of include/linux/firmware only qcom/).
make -j$J O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 CC="$CLANG/clang" LD="$CLANG/ld.lld" M=$D \
	modules > $LOG 2>&1
ret=$?
K=$D/cs35l41_dlkm.ko
if [ $ret -ne 0 ] || [ ! -f $K ]; then
	echo "k618_cs35l41: build failed ($ret), $(grep -c 'error:' $LOG) error lines -- $LOG"
	grep -E "error:|ERROR:|warning:" $LOG | head -20
	exit 1
fi
grep -q "warning:" $LOG && { echo "k618_cs35l41: warnings --"; grep "warning:" $LOG | head -10; }
# the safety lines: the ROM's tuning name, protection firmware held back without it, calibration
# handed over before the DSP runs, the slot width, the devicetree match
strings $K | grep -q "k618: tuning '%s'" ||
	{ echo "k618_cs35l41: the prefixed tuning name is not in"; exit 1; }
strings $K | grep -q "k618: no tuning (.bin) for %s: firmware not started" ||
	{ echo "k618_cs35l41: protection firmware would start without its tuning"; exit 1; }
strings $K | grep -q "k618: calibration CAL_R %u applied" ||
	{ echo "k618_cs35l41: calibration hook missing"; exit 1; }
strings $K | grep -qx "cirrus,cs35l41" || { echo "k618_cs35l41: no cirrus,cs35l41 match"; exit 1; }
for c in "AMP Enable" "ASPTX Ref" "AMP PCM Gain" "ASPRX1 Slot Position" "Boost Target Voltage" \
	 "Calibration Resistance" "Fast Use Case Delta File" "None"; do
	strings $K | grep -qx "$c" || { echo "k618_cs35l41: control '$c' missing"; exit 1; }
done
echo "k618_cs35l41: $(stat -c %s $K) bytes, $($CLANG/llvm-nm -u $K | wc -l) imports," \
	"params: $(strings $K | sed -n 's/^parm=\([^:]*\):.*/\1/p' | tr '\n' ' ')"
if [ "$INSTALL" = 1 ]; then
	mkdir -p $K618/tpmods
	cp $K $K618/tpmods/ || exit 1
	echo "k618_cs35l41: -> $K618/tpmods/cs35l41_dlkm.ko"
fi
