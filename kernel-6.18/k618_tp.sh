#!/bin/bash
# k618_tp.sh [jobs] [name ...]: Qualcomm's techpack driver repos (display, GPU, camera, audio, video,
# WLAN, crypto, IPA ...) built as external modules against our GKI 6.12 and the soc tree, for kalama.
#
# Each repo is laid out for `make -C <kernel> M=<dir> modules <ROOT>=<dir>`, which is how Qualcomm's
# own Android.mk path builds them; its Kbuild then picks the chipset's configuration from
# CONFIG_ARCH_KALAMA. They are built in dependency order and each one's Module.symvers is added to
# KBUILD_EXTRA_SYMBOLS for the ones after it, so modpost resolves across repos.
#
# Sources in $K618/vendor/qcom/opensource (k618_tp_get.sh puts them there), results collected in
# $K618/tpmods, log per repo in $K618/tp-log. With names given, only those are built (their
# predecessors' symbols must already be there). Run it in a memory-capped scope.
. "$(dirname "$(readlink -f "$0")")/env.sh"
J=${1:-8}; shift 2>/dev/null
TP=$K618/vendor/qcom/opensource
OUTM=$K618/tpmods
LOG=$K618/tp-log
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
export KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1

# the headers the multimedia repos take from one another; Qualcomm's own build passes these as a
# dependency's exported includes, which the `make M=` path has no way to know about (%M below)
MMINC="%T/synx-kernel/msm/synx %T/synx-kernel/include %T/synx-kernel/include/uapi
       %T/synx-kernel/include/uapi/synx/media %T/mm-drivers/hw_fence/include
       %T/mm-drivers/hfi_core/inc %T/mm-drivers/msm_ext_display/include
       %T/mm-drivers/sync_fence/include %T/securemsm-kernel/hdcp
       %T/securemsm-kernel/include %T/securemsm-kernel/include/linux %T/dsp-kernel/include"

# the data path's repos: each module's own uapi headers (it includes them as "uapi/linux/x.h"),
# rmnet's core and memory pool, and IPA's headers -- all of which they take from one another (%R)
RMINC="%D/include %D/include/uapi/linux %T/datarmnet/core %T/datarmnet-ext/mem
       %T/datarmnet-ext/mem/include %T/datarmnet-ext/mem/include/uapi/linux
       %T/dataipa/drivers/platform/msm/include %T/dataipa/drivers/platform/msm/include/uapi"

# name | directory to build (M=) | make variables | extra include dirs (%T = the techpack root,
# %M = the multimedia set above) | extra -D macros
#
# A techpack's per-chipset configuration comes in two halves: a .conf of make variables (which
# objects to build) and a conf.h of the same options as macros (which code to compile). Where an
# option has to be turned on for kalama although its conf does not have it, both halves are given
# here: the make variable in column 3 and the macro in column 5.
REPOS=(
	# its own include directory is named explicitly: tz_log.c includes <linux/tmelog.h>, which is
	# this repo's own header, and the Kbuild's -I does not reach it through `make M=` here
	"securemsm|securemsm-kernel|SSG_MODULE_ROOT=%D CONFIG_DISABLE_TEST_MODULES=y|%T/securemsm-kernel/include %T/securemsm-kernel/include/linux|"
	"synx|synx-kernel|SYNX_ROOT=%D|"
	"sync_fence|mm-drivers/sync_fence|SYNC_FENCE_ROOT=%P/|"
	"msm_ext_display|mm-drivers/msm_ext_display|MSM_EXT_DISPLAY_ROOT=%P/|"
	"hw_fence|mm-drivers/hw_fence|MSM_HW_FENCE_ROOT=%P/|%T/synx-kernel/msm/synx %T/synx-kernel/include %T/synx-kernel/include/uapi %T/synx-kernel/include/uapi/synx/media"
	"mmrm|mmrm-driver|MMRM_ROOT=%D|"
	"dsp|dsp-kernel|DSP_ROOT=%D|"
	"hfi_core|mm-drivers/hfi_core|MSM_HFI_CORE_ROOT=%P/|%M|"
	# This display release has HFI in the core files (sde_crtc.c, sde_plane.c use hfi_kms /
	# hfi_adapter unconditionally), while kalama's own conf predates it and leaves the three HFI
	# options off -- so nothing compiles. Build them as canoe does; the phone has no HFI node, so
	# hfi_kms stays NULL and those paths return early (`invalid hfi_kms`)
	"display|display-drivers|DISPLAY_ROOT=%D CONFIG_DRM_MSM=m CONFIG_QTI_HFI_CORE=y CONFIG_MDSS_HFI_ADAPTER=y CONFIG_MDSS_HFI=y|%M|-DCONFIG_QTI_HFI_CORE=1 -DCONFIG_MDSS_HFI_ADAPTER=1 -DCONFIG_MDSS_HFI=1 -include %T/k618_oplus_stub.h"
	# kgsl_sync.c's code after its #else is written for the synx path only (the branch for the older
	# msm_hw_fence API no longer compiles), so build it as canoe does. On this phone synx's graphics
	# hardware-fence client will not initialize, and KGSL then says "HW fences not supported" and
	# runs without them -- which is what its 5.15 build does anyway
	# CONFIG_ARM_SMMU picks kgsl_iommu.o; it is the soc tree's option, not one of the kernel's own
	"graphics|graphics-kernel|KGSL_PATH=%D CONFIG_QCOM_KGSL_SYNX=y CONFIG_ARM_SMMU=y|%M %W/drivers/devfreq|-DCONFIG_QCOM_KGSL_SYNX=1 -DCONFIG_ARM_SMMU=1"
	# video and eva are PARKED in their 6.12 form: that release lists sources per chipset (niobe,
	# pineapple, sun, canoe ...) and kalama is not among them any more -- video's object lists have no
	# kalama entry, and eva's config/ has only waipio, so its HFI structures never get defined.
	"video|video-driver|VIDEO_ROOT=%D CONFIG_MSM_VIDC_ANDROID=m|%M|"
	"eva|eva-kernel|EVA_ROOT=%D|%M %T/dsp-kernel/include/linux|"
	# ... so these two come from the 5.15 release instead, which is the OnePlus 11's -- the same
	# chipset as this phone, and it still has kalama_video.conf and CONFIG_EVA_KALAMA.
	# k618_tp_get.sh puts them beside the 6.12 ones as <name>-515.
	#
	# video515 BUILDS (msm_video.ko). Where the 5.15 code met an interface that had moved, the answer
	# was taken from Qualcomm's own 6.12 release rather than invented: the system-cache DMA attribute
	# becomes 0 (their 6.12 video driver writes `|= 0UL /*TODO: define ...*/`, and soc's
	# qcom-dma-mapping.h defines DMA_ATTR_SYS_CACHE as (0UL) when the kernel has none), the best-fit
	# IOVA allocator call goes away (in their 6.12 camera driver that function is empty for >= 5.15),
	# and vb2_queue's count is read with vb2_get_num_buffers(). All of it is in k618_tp_fix.py.
	#
	# eva515 BUILDS too (msm-eva.ko), and from the 5.15 release rather than the 6.12 one on purpose:
	# the 6.12 EVA still has a kalama branch in its Kbuild and still carries kalama's generation of
	# the HFI definitions as cvp_hfi_v1.h, but its C files have moved to the v2 names, so the packet
	# ids for this phone's firmware would be ours to guess -- while the 5.15 release's definitions
	# are what the phone's own evass firmware answers to. Rewriting the interfaces it meets is the
	# safe half of that trade: synx v2, iommu_map's gfp, of_icc_get, const bus_type, the file
	# refcount, iosys_map, qcom_scm_assign_mem, ghd_rm_get_vmid -- all in k618_tp_fix.py.
	# Its bus votes have nowhere to go on this phone: the cvp@ab00000 node still has the 5.15 form
	# (qcom,bus-master / qcom,bus-slave per child) and 6.12 reads interconnects/interconnect-names,
	# so of_icc_get() gives the NULL path the interconnect core documents as "skip setting
	# constraints" -- EVA runs without bus scaling until the node gains those two properties.
	"video515|video-driver-515|VIDEO_ROOT=%D CONFIG_MSM_VIDC_KALAMA=y|%M|-DCONFIG_MSM_VIDC_KALAMA=1"


	"eva515|eva-kernel-515|EVA_ROOT=%D UBSAN_SANITIZE=n|%M|"


	"camera|camera-kernel|CAMERA_KERNEL_ROOT=%D MODNAME=camera|%M|-DCONFIG_MSM_GLOBAL_SYNX=1 -DCONFIG_SPECTRA_SECURE_SCM_API=1"
	# MODNAME: with it empty the audio Kbuilds think they are inside a kernel tree and rewrite
	# AUDIO_ROOT to a 5.4-era path (which shows as a recursion of './' in the object paths)
	"audio|audio-kernel|AUDIO_ROOT=%D MODNAME=audio_dlkm|%M|"
	# (not CONFIG_SLIM_BTFM_CODEC: it links the same slimbus objects into a second module)
	"bt|bt-kernel|BT_ROOT=%D CONFIG_MSM_BT_POWER=m CONFIG_BTFM_SLIM=m CONFIG_I2C_RTC6226_QCA=m|%M|"


	"rmnet_mem|datarmnet-ext/mem|%C|%R|%L"
	"ipa|dataipa/drivers/platform/msm|KP_MODULE_ROOT=%P/../../..|%R|%L -DCONFIG_QCOM_MDT_LOADER_MODULE=1"
	"rmnet_core|datarmnet/core|RMNET_CORE_ROOT=%D %C|%R|%L %I"
	"rmnet_aps|datarmnet-ext/aps|%C|%R|%L %I"
	"rmnet_shs|datarmnet-ext/shs|%C|%R|%L %I"
	"rmnet_wlan|datarmnet-ext/wlan|%C|%R|%L %I"
	"rmnet_offload|datarmnet-ext/offload|%C|%R|%L %I"
	"rmnet_perf|datarmnet-ext/perf|%C|%R|%L %I"
	"rmnet_perf_tether|datarmnet-ext/perf_tether|%C|%R|%L %I"
	"rmnet_sch|datarmnet-ext/sch|%C|%R|%L %I"


	"nfc_nxp|../../nxp/opensource/driver|NFC_ROOT=%D CONFIG_NXP_NFC_I2C=m|%T/securemsm-kernel/include|-DNFC_SECURE_PERIPHERAL_ENABLED -DNFC_CLK_REQ_GPIO_WAKEUP -include %T/k618_nfc_trace.h ${NFC_TRACE:+-DK618_NFC_TRACE}"
	"nfc_st|../../st/opensource/driver|NFC_ROOT=%D CONFIG_STM_NCI=m CONFIG_STM_SECURE_ELEMENT=m|%T/securemsm-kernel/include|"
	"nfc_st_ese|../../st/opensource/eSE-driver|NFC_ROOT=%D|%T/securemsm-kernel/include|"


	"wlan_platform|wlan/platform|WLAN_PLATFORM_ROOT=%D CONFIG_CNSS2=m CONFIG_CNSS_UTILS=m CONFIG_CNSS_GENL=m CONFIG_WCNSS_MEM_PRE_ALLOC=m CONFIG_CNSS2_QMI=y CONFIG_CNSS_QMI_SVC=m CONFIG_CNSS_PLAT_IPC_QMI_SVC=m CONFIG_CNSS_OUT_OF_TREE=y CONFIG_PCI_MSM=y|%R|-include %T/k618_oplus_stub.h"
	# the WLAN host driver, named as the phone's own: Qualcomm's kalama build (Android.mk, several
	# chips, TARGET_MULTI_WLAN) made one module per chip with MODNAME = DEVNAME = the chip, so the
	# phone has qca_cld3_kiwi_v2.ko whose module name is kiwi_v2 (modinfo), whose MULTI_IF_NAME is
	# kiwi_v2 (INI wlan/qca_cld/kiwi_v2/WCNSS_qcom_cfg.ini and MAC wlan/qca_cld/kiwi_v2/wlan_mac.bin,
	# as its 5.15 dmesg loads them), and whose state node is /dev/wlan (WLAN_CTRL_NAME; the Wi-Fi HAL
	# writes ON there and finds the driver in /proc/modules by name). WLAN_PROFILE=kiwi_v2 is the chip
	# profile kalama used (default_defconfig + KIWI v2; IPA offload only on pineapple/SDX, so off -- the
	# phone's 5.15 module imports nothing from IPA either). The CNSS variables are what its
	# default_defconfig keys the PCIe/cnss2 bus on; the GKI config has none of them.
	# CONFIG_BUILD_TAG=n: the tag runs git in the (non-git) tree for every object.
	"wlan|wlan/qcacld-3.0|WLAN_ROOT=%D WLAN_COMMON_ROOT=../qca-wifi-host-cmn MODNAME=kiwi_v2 DEVNAME=kiwi_v2 WLAN_CTRL_NAME=wlan WLAN_PROFILE=kiwi_v2 CONFIG_QCA_CLD_WLAN=m CONFIG_CNSS2=m CONFIG_CNSS_GENL=m CONFIG_CNSS_UTILS=m CONFIG_WCNSS_MEM_PRE_ALLOC=m CONFIG_CNSS_OUT_OF_TREE=y CONFIG_BUILD_TAG=n|%R|"
)

PARKED="video eva"	# built only when named: see the table
mkdir -p $OUTM $LOG
python3 -I $K618_DIR/k618_tp_fix.py $TP || exit 1
# These repos are written for 6.12 like the soc tree, so they meet the same interface changes on the
# way to 6.18: the same sweeps run over them (namespaces as strings, .remove, timers, nth_page,
# dma_buf_ops, pinctrl and the rest -- k618_fix618.py's own file-specific edits are keyed by
# soc-relative paths and do not match here). Idempotent, so it runs on every build; FIX618=0 leaves
# them alone, for telling a drift fix apart from a techpack fix.
if [ "${FIX618:-1}" != 0 ]; then
	for d in $TP/* $K618/vendor/nxp/opensource/driver $K618/vendor/st/opensource/driver \
		 $K618/vendor/st/opensource/eSE-driver; do
		[ -d "$d" ] || continue
		python3 -I $K618_DIR/k618_fix618.py "$d" | sed "s|^|$(basename $d): |" | grep -v ': k618_fix618: 0 files changed' || true
	done
fi
# The vendor configuration, as the techpacks have to see it. The kernel's autoconf.h has only the
# GKI options; the ~380 the soc tree adds (QCOM_RPROC_COMMON, the minidump and IPC logging ones,
# mem_buf, ...) live in the soc build's own gen/autoconf_vendor.h, which nothing here included --
# so every IS_ENABLED(CONFIG_<vendor option>) in a shared header gave the techpacks its stub.
# synx-driver, for one, was built with the qcom_register_ssr_notifier() that returns NULL, and its
# init then frees itself and still returns 0. Qualcomm's own DDK build compiles them against the
# combined configuration, so they get it here too -- less the chipset selectors (CONFIG_ARCH_*),
# which the techpacks take from the make variables below (kalama), not from the soc's canoe base,
# and less the options below, which name drivers this phone does not have:
#   LEDS_QCOM_FLASH   canoe's new flash-LED driver, which binds "qcom,spmi-flash-led". This phone's
#                     node is "qcom,pm8350c-flash-led", driven on its own 5.15 kernel by the older
#                     leds-qti-flash (CONFIG_LEDS_QTI_FLASH, which we do build), so the camera has
#                     to take its QTI path: with this option defined it calls instead
#                     qcom_flash_led_get_max_avail_current, which only canoe's driver exports.
#                     k618_overrides.bzl turns it off for the soc build too.
TPCONF=$K618/tp-autoconf.h
[ -f $S/gen/autoconf_vendor.h ] || { echo "no $S/gen/autoconf_vendor.h: build the soc modules first"; exit 1; }


awk '/^#ifndef CONFIG_ARCH_/ && $2 != "CONFIG_ARCH_KALAMA" { skip = 3 }
     /^#ifndef CONFIG_DEVFREQ_GOV_QCOM_(ADRENO_TZ|GPUBW_MON)(_MODULE)?$/ { skip = 3 }
     /^#ifndef CONFIG_LEDS_QCOM_FLASH(_MODULE)?$/ { skip = 3 }
     /^#ifndef CONFIG_MSM_TMECOM_QMP(_MODULE)?$/ { skip = 3 }
     /^#ifndef CONFIG_QCOM_WCD_USBSS_I2C(_MODULE)?$/ { skip = 3 }
     skip { skip--; next } { print }' \
	$S/gen/autoconf_vendor.h > $TPCONF
SYMS=$S/Module.symvers
[ -f "$SYMS" ] || { echo "no soc Module.symvers: build the soc modules first"; exit 1; }
# earlier runs' symbols, each once: modpost refuses a module that two of the files name
addsym() { case " $SYMS " in *" $1 "*) ;; *) SYMS="$SYMS $1";; esac; }
for f in $LOG/*.symvers; do [ -f "$f" ] && addsym "$f"; done
ok=0; bad=0
for entry in "${REPOS[@]}"; do
	IFS='|' read -r name dir vars incs defs <<<"$entry"
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*) ;; *) continue;; esac
	else
		case " $PARKED " in *" $name "*) echo "$name: parked (no kalama target in this release)"; continue;; esac
	fi
	D=$TP/$dir
	[ -d "$D" ] || { echo "$name: no $D (run k618_tp_get.sh)"; bad=$((bad+1)); continue; }
	incs=${incs//%M/$MMINC}; incs=${incs//%R/$RMINC}
	defs=${defs//%L/-DCONFIG_RMNET_LA_PLATFORM=1}; defs=${defs//%I/-DCONFIG_IPA3=1}
	# %C: the data path's Kbuilds build rmnet_ctl (which rmnet_core calls into) only for
	# the targets they list, kalama not among them -- take canoe's selection
	vars=${vars//%C/CONFIG_ARCH_CANOE=y}
	for v in vars incs defs; do
		eval "$v=\${$v//%D/\$D}; $v=\${$v//%P/\$(dirname \$D)}; $v=\${$v//%T/\$TP}; $v=\${$v//%W/\$W/common}"
	done
	EI=; for i in $incs; do EI="$EI -I$i"; done
	# CLEAN=1: drop this repo's build products first. A run that failed with wrong make variables can
	# leave a .cmd file kbuild then chokes on ("missing separator")
	[ -n "${CLEAN:-}" ] && find $D \( -name '*.o' -o -name '*.ko' -o -name '.*.cmd' -o -name '*.mod' \
		-o -name '*.mod.c' -o -name 'modules.order' -o -name 'Module.symvers' \) -delete
	start=$(date +%s)
	# CONFIG_ARCH_KALAMA: the chipset each Kbuild keys its configuration off.
	# NOSTDINC_FLAGS (the kernel's own value is just -nostdinc, Makefile:542 and :1026) puts the soc
	# tree's headers *before* the kernel's own, as the soc build itself does: both trees have
	# include/linux/firmware/qcom/qcom_scm.h and the vendor code needs Qualcomm's, which has more
	# KERNEL_SRC / KERNEL_ROOT: the kernel tree, which their Kbuilds reach into for a few
	# kernel-internal headers (regmap's internal.h, the clock drivers); Qualcomm's own Makefile
	# wrapper passes them, and calling make directly does not
	make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$D modules -j$J \
		KERNEL_SRC=$W/common KERNEL_ROOT=$W/common \
		CONFIG_ARCH_KALAMA=y KBUILD_EXTRA_SYMBOLS="$SYMS" \
		NOSTDINC_FLAGS="-nostdinc -I$S/include -I$S/include/uapi$EI" \
		KCFLAGS="-Wno-error -include $TPCONF $defs" $vars > $LOG/$name.log 2>&1
	rc=$?
	n=$(find $D -name '*.ko' | wc -l)
	echo "$name: make exit $rc in $(( $(date +%s) - start )) s, $n modules"
	if [ $rc -ne 0 ]; then
		bad=$((bad+1))
		grep -E ': (fatal )?error: |^ERROR: modpost|No rule to make target' $LOG/$name.log \
			| sed -E "s|$TP/||" | awk -F: '!seen[$1$2]++' | head -8
		continue
	fi
	ok=$((ok+1))
	[ -f $D/Module.symvers ] && cp $D/Module.symvers $LOG/$name.symvers && addsym "$LOG/$name.symvers"
	find $D -name '*.ko' -exec cp {} $OUTM/ \;
done
echo "techpacks built: $ok, failed: $bad; modules in $OUTM: $(ls $OUTM/*.ko 2>/dev/null | wc -l)"
[ "$bad" -eq 0 ]
