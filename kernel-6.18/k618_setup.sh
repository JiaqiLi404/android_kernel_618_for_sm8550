#!/bin/bash
# Optional upstream source synchronization. The normal offline build uses scripts/prepare.sh.
# Requires Google's repo executable on PATH and network access.
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
export PATH="$NB/home/bin:$PATH"
mkdir -p $W
cd $W
# Pin the ACK release manifest used by this project.
repo init -q -u https://android.googlesource.com/kernel/manifest -b common-android17-6.18-2026-09 --depth=1 --no-clone-bundle
start=$(date +%s)
repo sync -q -c -j4 --no-tags --no-clone-bundle --optimized-fetch --fail-fast
echo "synced in $(( $(date +%s) - start )) s: $(du -sh $W | cut -f1)"
git -C $W/common log -1 --format='common at %h %cs %s' | cut -c1-120
sed -n 2,4p $W/common/Makefile | tr '\n' ' '; echo
grep -h -E '^(CLANG_VERSION|RUSTC_VERSION|BINDGEN|AARCH64_NDK)' $W/common/build.config.constants 2>/dev/null
