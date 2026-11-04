#!/bin/bash
# k618_soc_init.sh: $K618/soc = Qualcomm's vendor driver tree for 6.12 ("soc-repo" of
# KERNEL.PLATFORM.5.0, built as external modules against the GKI kernel) as OnePlus published it
# for the OnePlus 15 (github.com/OnePlusOSS/android_kernel_oneplus_sm8850, branch
# oneplus/sm8850_b_16.0.0_oneplus_15; the tree used so far was its commit fc30e54174), as a git
# repo whose first commit is that tree as it is -- the 6.18 and kalama changes go on top as commits
# (k618_fix618.py, k618_kalama.py; commit their results: k618_base.sh starts from HEAD).
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
[ ! -e $S/.git ] || { echo "$S exists"; exit 0; }
SRC=$K618/src/op15-msm
export GIT_TERMINAL_PROMPT=0
[ -d $SRC/.git ] || git clone -q --depth 1 --single-branch -b oneplus/sm8850_b_16.0.0_oneplus_15 \
	https://github.com/OnePlusOSS/android_kernel_oneplus_sm8850.git $SRC
mkdir -p $S
git -C $SRC archive HEAD | tar -x -C $S
cd $S
git init -q -b main
git config core.excludesFile $K618_DIR/k618_soc_gitignore.txt
git add -A
git -c user.name=nuwa -c user.email=nuwa@localhost commit -q -m "soc-repo of KERNEL.PLATFORM.5.0 (6.12) as OnePlus published it for the OnePlus 15 ($(git -C $SRC log -1 --format=%h))"
echo "soc: $(git ls-files | wc -l) files at $(git log -1 --format=%h)"
ls configs 2>/dev/null | head -20
