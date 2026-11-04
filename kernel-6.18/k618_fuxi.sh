#!/bin/bash
# k618_fuxi.sh: Xiaomi's kernel source release for the 13 series (fuxi-t-oss, 5.15, SM8550: the
# 13 Pro's own Qualcomm and Xiaomi drivers and device trees -- "Xiaomi 13 Pro Android T", Qualcomm
# LA.VENDOR.13.2.0.r1-07300-KAILUA.0), one branch at depth 1, in $K618/src/fuxi. Network and disk only.
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
D=$K618/src/fuxi
mkdir -p $(dirname $D)
export GIT_TERMINAL_PROMPT=0
[ -d $D/.git ] || git clone -q --depth 1 --single-branch -b fuxi-t-oss https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git $D
echo "fuxi: $(du -sh $D | cut -f1), $(git -C $D ls-files | wc -l) files"
git -C $D ls-tree --name-only HEAD | tr '\n' ' '; echo
