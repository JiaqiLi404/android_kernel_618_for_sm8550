#!/bin/bash
# k618_tp_get.sh: the techpack driver repos of Qualcomm's 6.12 release (KERNEL.PLATFORM.5.0, as
# OnePlus published them for the OnePlus 15) into $K618/vendor/qcom/opensource -- the sources
# k618_tp.sh builds. That path is not a choice: nine of their files find a neighbour's header by
# `../../../../vendor/qcom/opensource/...` relative to the kernel's include/trace, which lands
# exactly there when the kernel workspace is $K618/ws. $K618/tp is a link to it.
# Read-only on the study repo; already-extracted repos are left alone.
set -eu
. "$(dirname "$(readlink -f "$0")")/env.sh"
TP=$K618/vendor/qcom/opensource
MODS=${MODS:-$NB/vendor-study/mods.git}
REF=${REF:-refs/vendor/op-8850-mods}
mkdir -p $TP
[ -e $K618/tp ] || [ -L $K618/tp ] || ln -s vendor/qcom/opensource $K618/tp
for t in securemsm-kernel mm-drivers mmrm-driver synx-kernel dsp-kernel display-drivers \
         graphics-kernel video-driver camera-kernel eva-kernel audio-kernel bt-kernel \
         dataipa datarmnet datarmnet-ext wlan; do
	[ -d $TP/$t ] && continue
	git -C $MODS archive $REF vendor/qcom/opensource/$t | tar -x -C $TP --strip-components=3
	echo "k618_tp_get: $t ($(find $TP/$t -type f | wc -l) files)"
done
# The video and EVA drivers of this release no longer list kalama among their chipsets (video's
# object lists have no kalama entry, eva's config/ only waipio), so those two come from the 5.15
# release beside it -- the OnePlus 11's, the same chipset as this phone, which still has
# kalama_video.conf and CONFIG_EVA_KALAMA -- and are built as <name>-515 (k618_tp.sh's video515 and
# eva515 rows). The 6.12 copies stay for reference.
REF515=${REF515:-refs/vendor/op-8550-mods}
for t in video-driver eva-kernel; do
	[ -d $TP/$t-515 ] && continue
	mkdir -p $TP/$t-515
	git -C $MODS archive $REF515 vendor/qcom/opensource/$t \
		| tar -x -C $TP/$t-515 --strip-components=4
	echo "k618_tp_get: $t-515 ($(find $TP/$t-515 -type f | wc -l) files)"
done

# the NFC drivers are not Qualcomm's own: they sit beside them, under the chip vendor's name
for t in nxp st; do
	[ -d $K618/vendor/$t ] && continue
	mkdir -p $K618/vendor/$t/opensource/driver
	git -C $MODS archive $REF vendor/$t/opensource/driver \
		| tar -x -C $K618/vendor/$t/opensource/driver --strip-components=4
	echo "k618_tp_get: $t ($(find $K618/vendor/$t -type f | wc -l) files)"
done
# ST's embedded secure element (stm_st54se_gpio) is a repo of its own beside the NFC driver
if [ ! -d $K618/vendor/st/opensource/eSE-driver ]; then
	mkdir -p $K618/vendor/st/opensource/eSE-driver
	git -C $MODS archive $REF vendor/st/opensource/eSE-driver 		| tar -x -C $K618/vendor/st/opensource/eSE-driver --strip-components=4
	echo "k618_tp_get: st eSE ($(find $K618/vendor/st/opensource/eSE-driver -type f | wc -l) files)"
fi
