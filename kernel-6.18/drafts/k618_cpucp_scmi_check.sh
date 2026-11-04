#!/bin/bash
# k618_cpucp_scmi_check.sh: DRAFT check for k618_cpucp_scmi_port.py. Applies it to a scratch copy
# of the soc tree -- git HEAD plus k618_fix618.py, where k618_base.sh starts from -- evaluates the
# vendor Kconfig with its overrides as k618_soc_build.sh does, generates the soc Kbuild with
# k618_soc_kbuild.py and builds only the CPUCP modules against out-gki (their dependencies' symbols
# from the soc build's Module.symvers). Then: the options as Kconfig left them, the build, each
# module's imports against the phone's own 5.15 module (a vendor call the 5.15 one makes and ours
# does not = a header stub), the protocol ids, aliases and softdeps.
#
# Writes nothing outside $SCR (default /tmp/k618cpucp) -- not $S, not out-gki. Light (one olddefconfig,
# eight small modules); run it in a small scope:
#   systemd-run --user --scope --quiet -p MemoryMax=8G -p MemoryHigh=6G bash k618_cpucp_scmi_check.sh
set -u
KD=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
. $KD/env.sh
SCR=${SCR:-/tmp/k618cpucp}
DR=$KD/drafts
F=$K618/src/fuxi
S2=$SCR/soc
STOCK1=$NB/stockmods/vendor_boot/ramdisk0/lib/modules
STOCK2=$NB/nikohyper/x/vendor_dlkm_a/lib/modules
OURS="memlat_vendor pmu_vendor c1dcvs_vendor pmu_scmi c1dcvs_scmi memlat_scmi memlat qcom-pmu-lib"
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1

rm -rf $SCR && mkdir -p $S2/gen $S2/.git/info $SCR/kc || exit 1
git -C $S archive HEAD | tar -x -C $S2 || exit 1
echo "== scratch soc: $(git -C $S rev-parse --short HEAD) + k618_fix618.py + the draft"
python3 -I $KD/k618_fix618.py $S2 > $SCR/fix618.log || { tail $SCR/fix618.log; exit 1; }
python3 -I $DR/k618_cpucp_scmi_port.py overrides $KD/k618_overrides.bzl $S2/configs/k618_overrides.bzl || exit 1
python3 -I $DR/k618_cpucp_scmi_port.py $F $S2 $W/common > $SCR/port.log 2> $SCR/port.err || { cat $SCR/port.err; exit 1; }
cat $SCR/port.log
[ -s $SCR/port.err ] && { echo "-- port stderr:"; cat $SCR/port.err; }
# a second run must change nothing (the tables are idempotent, as k618_fuxi_mods.py's own)
python3 -I $DR/k618_cpucp_scmi_port.py $F $S2 $W/common > $SCR/port2.log 2>&1
grep -E "k618_fuxi_mods: (6.12 in|copied|[^ ]*modules.bzl:|[^ ]*Kconfig:|Kconfig.msm sources)" $SCR/port2.log \
	&& echo "NOT idempotent (above: what the second run changed)" || echo "second run: no change"

echo "== vendor Kconfig (as k618_soc_build.sh)"
python3 -I $KD/k618_soc_kbuild.py copies $S2 $W/common > /dev/null || exit 1
grep -v 'drivers/power/oplus/Kconfig' $S2/Kconfig.msm > $S2/gen/Kconfig.msm
KCONFIG_EXT_PREFIX=$S2/ $S2/flatten_kconfig.sh $S2/gen/Kconfig.msm > $S2/gen/Kconfig.ext || exit 1
python3 -I $KD/k618_soc_kbuild.py fragment $S2 $S2/configs/canoe_perf.bzl,k618_overrides.bzl > $S2/gen/fragment.config || exit 1
ok=
for try in $(seq 1 40); do
	cat $O/.config $S2/gen/fragment.config > $SCR/kc/.config
	make -C $W/common O=$SCR/kc ARCH=arm64 LLVM=1 LLVM_IAS=1 KCONFIG_EXT_PREFIX=$S2/gen/ \
		olddefconfig > $SCR/kconfig.log 2>&1 && { ok=1; break; }
	python3 -I $KD/k618_kconfig_fix.py $S2/gen/Kconfig.ext $SCR/kconfig.log || { tail -20 $SCR/kconfig.log; exit 1; }
done
[ -n "$ok" ] || { echo "kconfig: still failing"; exit 1; }
grep -E "^(# )?CONFIG_(QTI_SCMI_(VENDOR|PMU|C1DCVS|MEMLAT)_PROTOCOL|QTI_QCOM_SCMI_CLIENT|QTI_(PMU|C1DCVS|HW_MEMLAT)_SCMI_CLIENT|QCOM_(RIMPS|CPUCP|PMU_LIB|MEMLAT))[= ]" $SCR/kc/.config

echo "== build (only: $OURS)"
python3 -I $KD/k618_soc_kbuild.py kbuild $S2 $SCR/kc/.config $O/.config $OURS > $SCR/kbuild.log 2>&1 || { cat $SCR/kbuild.log; exit 1; }
tail -1 $SCR/kbuild.log
# the generated Kbuild's head (flags) and our modules' blocks; the dependencies come prebuilt
python3 -I - $S2/Kbuild $OURS <<'PY' || exit 1
import sys
path, ours = sys.argv[1], set(sys.argv[2:])
out, keep, head = [], True, True
for line in open(path).read().split("\n"):
    if line.startswith("obj-m += "):
        head = False
        keep = line[len("obj-m += "):-2] in ours
    if head or keep:
        out.append(line)
open(path, "w").write("\n".join(out) + "\n")
print("Kbuild:", sum(1 for l in out if l.startswith("obj-m")), "modules")
PY
grep -v -P "\t(memlat|memlat_vendor|pmu_vendor|c1dcvs_vendor|pmu_scmi|c1dcvs_scmi|memlat_scmi|qcom-pmu-lib|qcom_scmi_client)\t" \
	$S/Module.symvers > $SCR/extra.symvers
make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$S2 KBUILD_EXTRA_SYMBOLS=$SCR/extra.symvers \
	-k -j4 modules > $SCR/build.log 2>&1
echo "make exit $?; built: $(cd $S2 && ls *.ko 2>/dev/null | tr '\n' ' ')"
grep -E "(error|warning):|^(ERROR|WARNING): modpost" $SCR/build.log | sed "s|$S2/||" | sort | uniq | head -30

echo "== per module: imports vs the phone's 5.15 module (- only the 5.15 one, + only ours)"
modinf() {	# <ko> <key>: modinfo -F, without kmod (this WSL has no modinfo)
	llvm-objcopy -O binary --only-section=.modinfo "$1" $SCR/modinfo.bin && tr '\0' '\n' < $SCR/modinfo.bin | sed -n "s/^$2=//p"
}
for m in $OURS; do
	k=$S2/$m.ko
	[ -f $k ] || { echo "$m: NOT BUILT"; continue; }
	s=$(ls $STOCK1/$m.ko $STOCK2/$m.ko 2>/dev/null | head -1)
	echo "-- $m.ko ($(stat -c %s $k) bytes; vermagic $(modinf $k vermagic | cut -d' ' -f1);" \
		"alias $(modinf $k alias | grep -v '^of:' | tr '\n' ' ')softdep $(modinf $k softdep))"
	[ -n "$s" ] || { echo "   (no stock module)"; continue; }
	diff <(llvm-nm -u $s | awk '{print $2}' | sort -u) <(llvm-nm -u $k | awk '{print $2}' | sort -u) \
		| grep -E '^[<>]' | sed 's/^</   -/; s/^>/   +/' | tr '\n' ' '; echo
done
echo "== checks"
grep -P "\tcpucp_memlat_init\tmemlat\t" $S2/Module.symvers \
	&& echo "memlat exports cpucp_memlat_init" || echo "memlat: cpucp_memlat_init NOT exported"
for m in memlat qcom-pmu-lib; do
	llvm-nm -u $S2/$m.ko | grep -q get_qcom_scmi_device && echo "$m still imports get_qcom_scmi_device (unified path)" \
		|| echo "$m: no unified-protocol import"
done
llvm-nm -u $S2/memlat_scmi.ko | grep -w cpucp_memlat_init > /dev/null && echo "memlat_scmi imports cpucp_memlat_init"
llvm-nm -u $S2/pmu_scmi.ko | grep -w cpucp_pmu_init > /dev/null && echo "pmu_scmi imports cpucp_pmu_init"
echo "logs: $SCR/{port,kconfig,kbuild,build}.log"
