#!/bin/bash
# k618_base.sh [jobs] [module ...]: the OnePlus-only fixes applied to $K618/soc, then soc-repo's base
# set (what the phone needs to reach a shell: clocks, RPMh, SMMU, pinctrl, interconnects, regulators,
# SPMI, UFS, USB, watchdog -- and what they depend on), kalama's SoC drivers and any modules named,
# built against out-gki. Run it in a memory-capped scope.
. "$(dirname "$(readlink -f "$0")")/env.sh"
J=${1:-8}; shift 2>/dev/null
# from the committed tree each time: the fixes are made again, as the script has them now
git -C $S checkout -q -- . || exit 1
python3 -I $K618_DIR/k618_fix618.py $S || exit 1
cp $K618_DIR/k618_overrides.bzl $S/configs/
bash $K618_DIR/k618_soc_build.sh configs/canoe_perf.bzl,k618_overrides.bzl $J \
	clk-qcom gdsc-regulator clk-rpmh cmd-db qcom_rpmh smem socinfo qcom-scm qcom_ipc_logging \
	pinctrl-msm icc-rpmh icc-bcm-voter qcom-pdc rpmh-regulator spmi-pmic-arb qcom-spmi-pmic \
	arm_smmu ufs-qcom phy-qcom-ufs dwc3-msm phy-msm-snps-eusb2 phy-msm-ssusb-qmp repeater qcom_wdt_core \
	gcc-kalama camcc-kalama dispcc-kalama gpucc-kalama tcsrcc-kalama videocc-kalama debugcc-kalama \
	pinctrl-kalama qnoc-kalama phy-qcom-ufs-qmp-v4-kalama "$@"
