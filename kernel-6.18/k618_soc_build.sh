#!/bin/bash
# k618_soc_build.sh <dict .bzl[,dict...]> [jobs] [module ...]: soc-repo's modules ($K618/soc) built out
# of tree against our GKI 6.12 kernel ($K618/out-gki). The target's dicts are evaluated with Kconfig
# on top of the kernel's .config, as Kleaf's DDK does (soc-repo's Kconfig files as the kernel's
# Kconfig.ext; the result in $K618/soc-kconfig/.config), then Kbuild is generated and make keeps going
# past errors; the log in $K618/soc-build.log, the first error of each file in $K618/soc-errors.txt.
# Meant to run inside a memory-capped cgroup.
. "$(dirname "$(readlink -f "$0")")/env.sh"
KC=$K618/soc-kconfig
CFG=${1:?config}; J=${2:-8}; shift 2 2>/dev/null
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# KBUILD_GENDWARFKSYMS_STABLE as in k618_gki.sh: the kernel these modules are built against has
# its CRCs in stable mode, and a module's own imports must be read the same way.
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1
mkdir -p $S/gen $KC
# what soc-repo takes from the kernel tree (common_sources.bzl)
python3 -I $K618_DIR/k618_soc_kbuild.py copies $S $W/common || exit 1
# soc-repo's Kconfig files in one, as its kconfig.msm.generated rule makes them (less the file of
# OnePlus' that was not published)
grep -v 'drivers/power/oplus/Kconfig' $S/Kconfig.msm > $S/gen/Kconfig.msm
KCONFIG_EXT_PREFIX=$S/ $S/flatten_kconfig.sh $S/gen/Kconfig.msm > $S/gen/Kconfig.ext || exit 1
python3 -I $K618_DIR/k618_soc_kbuild.py fragment $S $S/$CFG > $S/gen/fragment.config || exit 1
ok=
for try in $(seq 1 40); do
	cat $O/.config $S/gen/fragment.config > $KC/.config
	KCONFIG_EXT_PREFIX=$S/gen/ make -C $W/common O=$KC ARCH=arm64 LLVM=1 LLVM_IAS=1 KCONFIG_EXT_PREFIX=$S/gen/ \
		olddefconfig > $K618/soc-kconfig.log 2>&1 && { ok=1; break; }
	# a dependency loop between soc-repo's Kconfig and the GKI's own: the soc side's edge goes
	python3 -I $K618_DIR/k618_kconfig_fix.py $S/gen/Kconfig.ext $K618/soc-kconfig.log || { tail -20 $K618/soc-kconfig.log; exit 1; }
done
[ -n "$ok" ] || { echo "kconfig: still failing after $try tries"; exit 1; }
grep -c "warning" $K618/soc-kconfig.log | sed 's/^/kconfig warnings: /'
python3 -I $K618_DIR/k618_soc_kbuild.py kbuild $S $KC/.config $O/.config "$@" || exit 1
start=$(date +%s)
make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$S -k -j$J modules > $K618/soc-build.log 2>&1
rc=$?
echo "make exit $rc in $(( $(date +%s) - start )) s; modules built: $(find $S -name '*.ko' -newer $S/Kbuild | wc -l) of $(grep -c '^obj-m' $S/Kbuild)"
grep -E ': (fatal )?error: ' $K618/soc-build.log | sed -E "s|$S/||" | awk -F: '!seen[$1]++' > $K618/soc-errors.txt
echo "files with errors: $(wc -l < $K618/soc-errors.txt)"
grep -E ': (fatal )?error: ' $K618/soc-build.log | sed -E 's/.*error: //' | sed -E "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -25
grep -E '^ERROR: modpost' $K618/soc-build.log | head -40
exit "$rc"
