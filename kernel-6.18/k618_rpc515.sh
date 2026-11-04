#!/bin/bash


set -u
J=${1:-8}
KD=$(cd "$(dirname "$0")" && pwd)
. $KD/env.sh
F=$K618/src/fuxi
D=$K618/vendor/xiaomi/adsprpc-515
LOG=$K618/rpc515.log

[ -d $F/drivers/char ] || { echo "k618_rpc515: no fuxi tree at $F -- run k618_fuxi.sh"; exit 1; }
[ -f $S/Module.symvers ] || { echo "k618_rpc515: no soc Module.symvers -- build the soc modules"; exit 1; }


TPCONF=$K618/tp-autoconf.h
[ -f $TPCONF ] || { echo "k618_rpc515: no $TPCONF -- run k618_tp.sh first"; exit 1; }

rm -rf $D
mkdir -p $D/include/uapi/misc $D/include/uapi/linux $D/include/linux $D/include/trace/events
cp $F/drivers/char/adsprpc.c $F/drivers/char/adsprpc_compat.c $F/drivers/char/adsprpc_compat.h \
	$F/drivers/char/adsprpc_shared.h $F/drivers/char/adsprpc_rpmsg.c \
	$F/drivers/char/adsprpc_socket.c $D/ || exit 1
# the headers it includes that GKI does not have, from the same 5.15 tree
cp $F/include/uapi/misc/fastrpc.h $D/include/uapi/misc/ || exit 1
cp $F/include/uapi/linux/msm_ion.h $F/include/uapi/linux/msm_ion_ids.h $D/include/uapi/linux/ || exit 1
cp $F/include/linux/qcom-dma-mapping.h $F/include/linux/fastrpc.h $D/include/linux/ || exit 1
cp $F/include/trace/events/fastrpc.h $D/include/trace/events/ || exit 1
python3 -I $KD/k618_rpc515_fix.py $D || exit 1

cat > $D/Kbuild <<'EOF'
obj-m += frpc-adsprpc.o
frpc-adsprpc-objs += adsprpc.o adsprpc_rpmsg.o adsprpc_compat.o
ccflags-y += -I$(src) -I$(src)/include -I$(src)/include/uapi
# the soc tree's own headers (secure_buffer, the rproc_qcom tracepoints, ...)
ccflags-y += -I$(SOC)/include -I$(SOC)/include/uapi
# 5.15 vendor code under our stricter build: its %llu/%zu mismatches and one switch fall-through
# are warnings there and errors here, and say nothing about the port
ccflags-y += -Wno-format -Wno-implicit-fallthrough -Wno-missing-prototypes
# the driver's own header hides its exported calls behind this config
ccflags-y += -DCONFIG_MSM_ADSPRPC=1
EOF

cd $W/common || exit 1
# the techpack builds' PATH: pahole (the module's BTF) and the rest of the prebuilts
export PATH=$CLANG:$W/prebuilts/clang-tools/linux-x86/bin:$W/prebuilts/kernel-build-tools/linux_musl-x86/bin:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# as k618_tp.sh builds the techpacks: the vendor configuration, the chipset selector, and the soc
# tree's headers ahead of the kernel's own (both have qcom_scm.h and the vendor code wants Qualcomm's)
KBUILD_EXTRA_SYMBOLS="$S/Module.symvers" make -j$J SOC=$S O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	CC="$CLANG/clang" LD="$CLANG/ld.lld" M=$D CONFIG_ARCH_KALAMA=y \
	NOSTDINC_FLAGS="-nostdinc -I$S/include -I$S/include/uapi" \
	KCFLAGS="-include $TPCONF" modules > $LOG 2>&1
ret=$?
if [ $ret -ne 0 ] || [ ! -f $D/frpc-adsprpc.ko ]; then
	echo "k618_rpc515: build failed ($ret), $(grep -c 'error:' $LOG) error lines -- $LOG"
	grep -E "error:|ERROR:" $LOG | head -12
	exit 1
fi
# the calls into the vendor modules the phone's own 5.15 frpc-adsprpc.ko makes: one missing means a
# header handed us its stub again
for s in mem_buf_dma_buf_exclusive_owner qcom_register_ssr_notifier msm_minidump_add_region qcom_elf_dump; do
	llvm-nm -u $D/frpc-adsprpc.ko | grep -q -w "$s" || { echo "k618_rpc515: $s not imported -- a vendor-config stub"; exit 1; }
done
mkdir -p $K618/tpmods
cp $D/frpc-adsprpc.ko $K618/tpmods/ || exit 1
echo "k618_rpc515: $(stat -c %s $D/frpc-adsprpc.ko) bytes -> $K618/tpmods/frpc-adsprpc.ko"
echo "k618_rpc515: compatibles $(strings $D/frpc-adsprpc.ko | grep -c '^qcom,msm-fastrpc'), device nodes $(strings $D/frpc-adsprpc.ko | grep -c '^adsprpc-')"


TP=$K618/vendor/qcom/opensource
E=$TP/eva-kernel-515
if [ -d $E ] && [ "${EVA:-1}" != 0 ]; then
	MMI="$TP/synx-kernel/msm/synx $TP/synx-kernel/include $TP/synx-kernel/include/uapi
	     $TP/synx-kernel/include/uapi/synx/media $TP/mm-drivers/hw_fence/include
	     $TP/mm-drivers/hfi_core/inc $TP/mm-drivers/msm_ext_display/include
	     $TP/mm-drivers/sync_fence/include $TP/securemsm-kernel/hdcp
	     $TP/securemsm-kernel/include $TP/securemsm-kernel/include/linux $TP/dsp-kernel/include"
	EI="-I$D/include"; for i in $MMI; do EI="$EI -I$i"; done
	SY=$S/Module.symvers
	for f in $K618/tp-log/*.symvers; do
		case $(basename $f) in dsp.symvers|eva515.symvers) ;; *) SY="$SY $f";; esac
	done
	SY="$SY $D/Module.symvers"
	find $E \( -name '*.o' -o -name '*.ko' -o -name '.*.cmd' -o -name '*.mod' -o -name '*.mod.c' \
		-o -name 'modules.order' -o -name 'Module.symvers' \) -delete
	make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$E modules -j$J \
		KERNEL_SRC=$W/common KERNEL_ROOT=$W/common CONFIG_ARCH_KALAMA=y KBUILD_EXTRA_SYMBOLS="$SY" \
		NOSTDINC_FLAGS="-nostdinc -I$S/include -I$S/include/uapi $EI" \
		KCFLAGS="-Wno-error -include $TPCONF -DCONFIG_MSM_ADSPRPC=1" EVA_ROOT=$E UBSAN_SANITIZE=n \
		> $K618/rpc515-eva.log 2>&1 || { echo "k618_rpc515: msm-eva build failed -- $K618/rpc515-eva.log"; exit 1; }
	EK=$(find $E -name msm-eva.ko | head -1)
	grep -q "$D/include/linux/fastrpc.h" $(find $E -name '.cvp.o.cmd' | head -1) ||
		{ echo "k618_rpc515: msm-eva did not take this tree's fastrpc.h"; exit 1; }
	cp $EK $K618/tpmods/ || exit 1
	echo "k618_rpc515: msm-eva against this adsprpc -> $K618/tpmods/msm-eva.ko ($(stat -c %s $EK) bytes," \
		"$(llvm-objdump -d $EK | grep -c -E 'brk\s+#0x55') UBSAN traps)"
fi
