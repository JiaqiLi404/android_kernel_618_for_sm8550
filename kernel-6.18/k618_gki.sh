#!/bin/bash
# k618_gki.sh [jobs, 8]: the GKI kernel of android17-6.18-2026-09 (6.18.32; gki_defconfig, as Google builds it,
# no LTO, like the 18 Pro Max's) with the workspace's own toolchain -- clang, Rust, bindgen,
# pahole, whatever version the manifest brought (env.sh finds them) -- by plain make into $K618/out-gki. Meant to run inside a
# memory-capped cgroup (systemd-run -p MemoryMax=...).
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
J=${1:-8}
[ -n "$CLANG" ] || { echo "no clang in $W/prebuilts: run k618_setup.sh first"; exit 1; }
export PATH=$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin
# what Google's build environment sets (build/kernel/_setup_env.sh): CRCs in gendwarfksyms' stable
# mode -- so this kernel's KMI is the 18 Pro Max's and its prebuilt android17 vendor modules can be
# loaded here (k618_kmi_check.py checks it) -- and a fixed build user and host.
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user KBUILD_BUILD_VERSION=1
M="make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 PAHOLE=$TOOLS/pahole"
[ -n "$R" ] && M="$M RUSTC=$R/bin/rustc HOSTRUSTC=$R/bin/rustc BINDGEN=$W/prebuilts/clang-tools/linux-x86/bin/bindgen RUST_LIB_SRC=$R/lib/rustlib/src/rust/library"
mkdir -p $O
start=$(date +%s)
# What the vendor modules need of the kernel and gki_defconfig does not give them. The display
# driver links against the DisplayPort, HDCP and HDMI helpers and against the bridge connector;
# 6.18's hidden-DRM list (init/Kconfig.gki) has none of the five, where 6.12's had them all (that
# is why the same display source built there), and all five are hidden symbols, so they can only be
# turned on by a select. The 18 Pro Max's own 6.18 build ships drm_display_helper as a module, so
# these are the same files Qualcomm builds, only inside the kernel here.
# Idempotent, and re-applied after a k618_setup.sh sync.
KG=$W/common/init/Kconfig.gki
if ! grep -q 'select DRM_DISPLAY_HELPER' $KG; then
	sed -i 's/^\tselect DRM_CLIENT if (DRM)$/\tselect DRM_CLIENT if (DRM)\n\tselect DRM_DISPLAY_HELPER if (DRM)\n\tselect DRM_DISPLAY_DP_HELPER if (DRM)\n\tselect DRM_DISPLAY_HDCP_HELPER if (DRM)\n\tselect DRM_DISPLAY_HDMI_HELPER if (DRM)\n\tselect DRM_BRIDGE_CONNECTOR if (DRM)/' $KG
	grep -q 'select DRM_DISPLAY_HELPER' $KG \
		&& echo "k618_gki: the five display helpers added to GKI_HIDDEN_DRM_CONFIGS" \
		|| { echo "k618_gki: could not add the display helpers to $KG"; exit 1; }
fi


TO=$W/common/drivers/thermal/thermal_of.c
if ! grep -q 'k618: a zone without a sensor' $TO; then
	perl -0pi -e 's|\t\tif \(count <= 0\) \{\n\t\t\tpr_err\("%pOFn: missing thermal sensor\\n", child\);\n\t\t\treturn ERR_PTR\(-EINVAL\);\n\t\t\}|\t\tif (count <= 0) {\n\t\t\t/* k618: a zone without a sensor (the phone\x27s socd) is skipped, as 5.15 did */\n\t\t\tpr_debug("%pOFn: missing thermal sensor\\n", child);\n\t\t\tcontinue;\n\t\t}|' $TO
	grep -q 'k618: a zone without a sensor' $TO \
		&& echo "k618_gki: thermal zones without a sensor are skipped" \
		|| { echo "k618_gki: could not edit $TO"; exit 1; }
fi


SE=$W/common/security/selinux/include/security.h
if ! grep -q 'k618: the old netlink checks' $SE; then
	perl -0pi -e 's|static inline bool selinux_policycap_netlink_xperm\(void\)\n\{\n\treturn READ_ONCE\(\n\t\tselinux_state.policycap\[POLICYDB_CAP_NETLINK_XPERM\]\);\n\}|static inline bool selinux_policycap_netlink_xperm(void)\n{\n\t/* k618: the old netlink checks, as 5.15 (see kernel-6.18/k618_gki.sh) */\n\treturn false;\n}|' $SE
	grep -q 'k618: the old netlink checks' $SE \
		&& echo "k618_gki: netlink extended permissions off" \
		|| { echo "k618_gki: could not edit $SE"; exit 1; }
fi


CF=$W/common/drivers/cpufreq/cpufreq.c
if ! grep -q 'k618: "walt" is schedutil' $CF; then
	perl -0pi -e 's|(\tfor_each_governor\(t\)\n\t\tif \(!strncasecmp\(str_governor, t->name, CPUFREQ_NAME_LEN\)\)\n\t\t\treturn t;\n\n)(\treturn NULL;\n\}\n)|$1\t/* k618: "walt" is schedutil while no walt governor is registered (kernel-6.18/k618_gki.sh) */\n\tif (!strncasecmp(str_governor, "walt", CPUFREQ_NAME_LEN))\n\t\treturn find_governor("schedutil");\n\n$2|' $CF
	grep -q 'k618: "walt" is schedutil' $CF \
		&& echo "k618_gki: walt -> schedutil" \
		|| { echo "k618_gki: could not edit $CF"; exit 1; }
fi


IO=$W/common/drivers/iommu/iommu.c
if ! grep -q 'k618: the handler of a domain' $IO; then
	perl -0pi -e 's#\tif \(WARN_ON\(!domain \|\| domain->cookie_type != IOMMU_COOKIE_NONE\)\)\n\t\treturn;\n\n\tdomain->cookie_type = IOMMU_COOKIE_FAULT_HANDLER;#\tif (WARN_ON(!domain))\n\t\treturn;\n\n\t/*\n\t * k618: the handler of a domain that already carries a cookie (the DMA API\x27s, for every\n\t * vendor driver that asks for its device\x27s domain) goes into the Android vendor data;\n\t * report_iommu_fault() looks there too (kernel-6.18/k618_gki.sh)\n\t */\n\tif (domain->cookie_type != IOMMU_COOKIE_NONE) {\n\t\tdomain->android_vendor_data1[0] = (u64)(uintptr_t)handler;\n\t\tdomain->android_vendor_data1[1] = (u64)(uintptr_t)token;\n\t\treturn;\n\t}\n\n\tdomain->cookie_type = IOMMU_COOKIE_FAULT_HANDLER;#' $IO
	perl -0pi -e 's#(static void iommu_domain_init\(struct iommu_domain \*domain, unsigned int type,\n\t\t\t      const struct iommu_ops \*ops\)\n\{\n)#$1\tandroid_init_vendor_data(domain, 1);\t/* k618: no stale fault handler in a new domain */\n#' $IO
	perl -0pi -e 's#\tif \(domain->cookie_type == IOMMU_COOKIE_FAULT_HANDLER &&\n\t    domain->handler\)\n\t\tret = domain->handler\(domain, dev, iova, flags,\n\t\t\t\t\t\tdomain->handler_token\);\n#\tif (domain->cookie_type == IOMMU_COOKIE_FAULT_HANDLER &&\n\t    domain->handler)\n\t\tret = domain->handler(domain, dev, iova, flags,\n\t\t\t\t\t\tdomain->handler_token);\n\telse if (domain->android_vendor_data1[0])\t/* k618, see iommu_set_fault_handler() */\n\t\tret = ((iommu_fault_handler_t)(uintptr_t)domain->android_vendor_data1[0])(\n\t\t\tdomain, dev, iova, flags,\n\t\t\t(void *)(uintptr_t)domain->android_vendor_data1[1]);\n#' $IO
	grep -q 'k618: the handler of a domain' $IO && grep -q 'k618, see iommu_set_fault_handler' $IO \
		&& grep -q 'k618: no stale fault handler' $IO \
		&& echo "k618_gki: a vendor SMMU fault handler is kept in the domain's vendor data" \
		|| { echo "k618_gki: could not edit $IO"; exit 1; }
fi


KSU_REF=${KSU_REF:-0c0fdb960178fa12d80d6cbcfdcb6ad8be6a865d}
# SUSFS v2.3.0 input from gki-android16-6.12. Its KernelSU patch and three kernel files
# match public gki-android13-5.15 revision bd27d72 byte for byte.
SUSFS_REF=${SUSFS_REF:-5e0b2b925865000ca7dce993ecf1d281ee312887}
SUSFS_GIT=$K618/susfs4ksu
KSU=$K618/KernelSU
if [ ! -d $KSU/.git ]; then
	src=$NB/ksu-look/KernelSU
	[ -d $src/.git ] || src=https://github.com/tiann/KernelSU
	git clone -q --no-checkout $src $KSU
fi
if [ ! -d $SUSFS_GIT/.git ]; then
	git clone -q https://gitlab.com/simonpunk/susfs4ksu.git $SUSFS_GIT
fi
git -C $SUSFS_GIT cat-file -e "$SUSFS_REF^{commit}" 2>/dev/null \
	|| git -C $SUSFS_GIT fetch -q https://gitlab.com/simonpunk/susfs4ksu.git gki-android16-6.12
git -C $SUSFS_GIT cat-file -e "$SUSFS_REF^{commit}" || { echo "k618_gki: no susfs4ksu $SUSFS_REF"; exit 1; }
# KernelSU: its commit, then SUSFS's patch, then the noksud switch -- from a clean checkout each time
# the SUSFS patch is not there yet (it rewrites the file the switch goes into)
if [ "$(git -C $KSU rev-parse HEAD 2>/dev/null)" != "$KSU_REF" ] || [ ! -f $KSU/kernel/Kbuild ] \
	|| ! grep -q 'config KSU_SUSFS$' $KSU/kernel/Kconfig; then
	git -C $KSU cat-file -e "$KSU_REF^{commit}" 2>/dev/null \
		|| git -C $KSU fetch -q https://github.com/tiann/KernelSU $KSU_REF
	git -C $KSU checkout -q -f $KSU_REF
	git -C $SUSFS_GIT show $SUSFS_REF:kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch \
		| (cd $KSU && patch -p1 --forward --no-backup-if-mismatch -s) \
		|| { echo "k618_gki: SUSFS's KernelSU patch does not apply"; exit 1; }
	echo "k618_gki: KernelSU $(git -C $KSU rev-parse --short HEAD) with SUSFS's KernelSU patch ($(git -C $SUSFS_GIT rev-parse --short $SUSFS_REF))"
fi
# SUSFS's selinux_hide.c tests two functions' addresses (`if (security_dump_masked_av_fn)`), declared
# plain extern -- the kernel side always defines both, so the test is always true, and 6.18's GKI is
# built with CONFIG_WERROR, where clang's -Wpointer-bool-conversion stops the build (5.15's is not).
# Disable this warning only for that file; preserve the pinned upstream source.
grep -q 'k618: SUSFS selinux_hide' $KSU/kernel/Kbuild \
	|| printf '\n# k618: SUSFS selinux_hide tests addresses of functions the kernel always has (kernel-6.18/k618_gki.sh)\nCFLAGS_feature/selinux_hide.o += -Wno-pointer-bool-conversion\n' >> $KSU/kernel/Kbuild
KI=$KSU/kernel/runtime/ksud_integration.c
if ! grep -q 'k618: ksud not started' $KI; then
	perl -0pi -e 's|#include <linux/stat.h>\n|#include <linux/stat.h>\n#include <linux/moduleparam.h>\n|' $KI
	perl -0pi -e 's|(\nstatic bool is_init_rc\(struct file \*fp\)\n)|\n/* k618: kernelsu.noksud=1 -- init.rc gets no ksud lines, so ksud never starts (kernel-6.18/k618_gki.sh) */\nstatic bool ksu_k618_noksud;\nmodule_param_named(noksud, ksu_k618_noksud, bool, 0);\n$1|' $KI
	perl -0pi -e 's|(        pr_info\("ksu_init_rc_hook is disabled\\n"\);\n    }\n)|$1    if (ksu_k618_noksud) {\n        pr_info("k618: ksud not started (noksud)\\n");\n        return;\n    }\n|' $KI
	grep -q 'k618: ksud not started' $KI && grep -q 'module_param_named(noksud' $KI \
		&& echo "k618_gki: KernelSU's noksud switch added" \
		|| { echo "k618_gki: could not edit $KI"; exit 1; }
fi
# /dev/iorap_dev as the 18 Pro Max has it: its own odm ueventd.rc says "0666 system system". This
# phone's odm, which the ROM keeps, has no such line, so ueventd leaves the node to root 0600 and the
# ROM's iorapd (user system) exits on EACCES every few seconds -- and init, counting it as a component
# that keeps crashing, runs flags_health_check, which clears the device_config flags.
# The node's type (lb_device) is already the ROM's own. Owner and
# mode only, as two lines of KernelSU's init.rc text ahead of ksud: init runs them at post-fs-data,
# before iorapd (class main) first starts. kernelsu.noksud=1 keeps the whole text out, these too.
if ! grep -q 'k618: iorap_dev' $KI; then
	perl -0pi -e 's#(    "on post-fs-data\\n"\n    "    start logd\\n"\n)#$1    // k618: iorap_dev as the 18 Pro Max odm ueventd.rc has it (kernel-6.18/k618_gki.sh)\n    "    chown system system /dev/iorap_dev\\n"\n    "    chmod 0666 /dev/iorap_dev\\n"\n#' $KI
	grep -q 'k618: iorap_dev' $KI && grep -q '"    chmod 0666 /dev/iorap_dev\\n"' $KI \
		&& echo "k618_gki: /dev/iorap_dev's owner and mode in KernelSU's init.rc lines" \
		|| { echo "k618_gki: could not edit $KI (iorap_dev)"; exit 1; }
fi


SP=$K618_DIR/susfs/50_add_susfs_in_gki-android17-6.18.patch
# applied once per version of the patch: a changed patch first has the files it touches taken back
# from git (none of them is edited by anything else here; the three SUSFS sources are copied again)
SP_SUM=$(sha1sum < $SP | cut -c1-16)-$(git -C $SUSFS_GIT rev-parse --short $SUSFS_REF)
SP_STAMP=$W/common/.k618_susfs
if [ "$(cat $SP_STAMP 2>/dev/null)" != "$SP_SUM" ]; then
	for f in $(sed -n 's#^+++ b/##p' $SP); do
		git -C $W/common ls-files --error-unmatch "$f" > /dev/null 2>&1 && git -C $W/common checkout -q -- "$f"
	done
	git -C $SUSFS_GIT show $SUSFS_REF:kernel_patches/fs/susfs.c > $W/common/fs/susfs.c
	git -C $SUSFS_GIT show $SUSFS_REF:kernel_patches/include/linux/susfs.h > $W/common/include/linux/susfs.h
	git -C $SUSFS_GIT show $SUSFS_REF:kernel_patches/include/linux/susfs_def.h > $W/common/include/linux/susfs_def.h
	(cd $W/common && patch -p1 --forward --no-backup-if-mismatch -F0 -s --dry-run < $SP) \
		|| { echo "k618_gki: $SP does not apply to $W/common"; exit 1; }
	(cd $W/common && patch -p1 --forward --no-backup-if-mismatch -F0 -s < $SP)
	echo "$SP_SUM" > $SP_STAMP
	echo "k618_gki: SUSFS $(sed -n 's/^#define SUSFS_VERSION "\(.*\)"/\1/p' $W/common/include/linux/susfs.h) added ($(grep -c '^@@' $SP) hunks, $SP_SUM)"
fi
ln -sfn $KSU/kernel $W/common/drivers/kernelsu
grep -q 'kernelsu/' $W/common/drivers/Makefile || printf '\nobj-$(CONFIG_KSU) += kernelsu/\n' >> $W/common/drivers/Makefile
grep -q 'drivers/kernelsu/Kconfig' $W/common/drivers/Kconfig \
	|| perl -0pi -e 's|endmenu\s*\z|source "drivers/kernelsu/Kconfig"\n\nendmenu\n|' $W/common/drivers/Kconfig
grep -q 'drivers/kernelsu/Kconfig' $W/common/drivers/Kconfig || { echo "k618_gki: KernelSU not in drivers/Kconfig"; exit 1; }


python3 -I $K618_DIR/k618_ports.py $W/common || exit 1
python3 -I $K618_DIR/k618_memory.py $W/common || exit 1
$M gki_defconfig > $O/../gki-config.log 2>&1
# The modules the phone's second stage loads that are the kernel's own, not a vendor's: MTD (whose
# block2mtd + mtdoops pair is the ROM's own kernel-oops area on the `oops` partition, with
# block2mtd.block2mtd=/dev/block/sda16,2097152 on its command line), the PMIC glink transport and
# its USB-C controller, the USB EHSET test fixture, the IR transmitter, an IIO buffer callback and
# STM's basic protocol. gki_defconfig has none of them; they are plain modules, so this only adds.
GKI_MODS="MTD MTD_BLKDEVS MTD_BLOCK MTD_OOPS MTD_BLOCK2MTD MTD_OF_PARTS QCOM_PMIC_GLINK
	  UCSI_PMIC_GLINK USB_EHSET_TEST_FIXTURE IR_SPI IIO_BUFFER_CB STM_PROTO_BASIC"
$W/common/scripts/config --file $O/.config $(for c in $GKI_MODS; do echo -n " -m $c"; done)
$W/common/scripts/config --file $O/.config -e KSU
# Enable the ten SUSFS features selected by this integration.
SUSFS_CFG="KSU_SUSFS KSU_SUSFS_SUS_PATH KSU_SUSFS_SUS_MOUNT KSU_SUSFS_SUS_KSTAT KSU_SUSFS_SPOOF_UNAME
	   KSU_SUSFS_ENABLE_LOG KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	   KSU_SUSFS_OPEN_REDIRECT KSU_SUSFS_SUS_MAP"
$W/common/scripts/config --file $O/.config $(for c in $SUSFS_CFG; do echo -n " -e $c"; done)


$W/common/scripts/config --file $O/.config --set-val LOG_BUF_SHIFT 17
# gki_defconfig sets CONFIG_STATIC_USERMODEHELPER=y with an empty path, which makes every
# call_usermodehelper() return 0 and run nothing (kernel/umh.c: the path is replaced by the empty
# CONFIG_STATIC_USERMODEHELPER_PATH, and an empty path is "done, success"). The batch loader of the
# test images (k618_load + /k618_umh) is a usermodehelper, and with this on it silently loaded
# nothing at all -- 219 modules "loaded" in 31 ms. Off here, so the test images can load a module
# in a process of its own; it changes nothing else (the kernel itself calls no helper on this boot).
$W/common/scripts/config --file $O/.config -d STATIC_USERMODEHELPER
# zram compresses with lz4 by default (HyperOS writes no algorithm, the 13 Pro's post_boot only on
# low-RAM devices): k618_ports.py's zram_tiered adds zstd for the pages that stay cold. Only zram.ko
# reads this option, a module this build makes, so the KMI is not touched.
$W/common/scripts/config --file $O/.config -d ZRAM_DEF_COMP_LZORLE -e ZRAM_DEF_COMP_LZ4
# Existing upstream capabilities: defer idle KFENCE sampling and retain tmpfs xattrs.
$W/common/scripts/config --file $O/.config -e KFENCE_DEFERRABLE -e TMPFS_XATTR
$M olddefconfig >> $O/../gki-config.log 2>&1
grep -q '^CONFIG_ZRAM_DEF_COMP="lz4"' $O/.config || echo "k618_gki: CONFIG_ZRAM_DEF_COMP is not lz4"
grep -q '^# CONFIG_STATIC_USERMODEHELPER is not set' $O/.config || echo "k618_gki: STATIC_USERMODEHELPER did not take"
for c in $GKI_MODS; do grep -q "^CONFIG_$c=m" $O/.config || echo "k618_gki: CONFIG_$c did not take"; done
grep -q '^CONFIG_KSU=y' $O/.config || echo "k618_gki: CONFIG_KSU did not take"
for c in $SUSFS_CFG; do grep -q "^CONFIG_$c=y" $O/.config || echo "k618_gki: CONFIG_$c did not take"; done
grep -E '^CONFIG_DRM_DISPLAY_(HELPER|DP_HELPER)=' $O/.config | tr '\n' ' '; echo
$M rustavailable > $O/../gki-rust.log 2>&1 || { echo "rust not available:"; tail -5 $O/../gki-rust.log; }
grep -E '^CONFIG_(RUST|GENDWARFKSYMS|MODVERSIONS|LTO_NONE|LTO_CLANG_THIN)=' $O/.config | tr '\n' ' '; echo
$M -j$J Image modules > $O/../gki-build.log 2>&1 || { tail -30 $O/../gki-build.log; echo "BUILD FAILED after $(( $(date +%s) - start )) s"; exit 1; }
echo "built in $(( $(date +%s) - start )) s"
ls -la $O/arch/arm64/boot/Image | awk '{print $5, $9}'
cat $O/include/config/kernel.release
grep -c . $O/Module.symvers
grep -c ' warning: ' $O/../gki-build.log || true
