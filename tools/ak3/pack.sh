#!/bin/bash
# Pack a kernel Image into an AnyKernel3 zip for the Xiaomi 13 Pro (nuwa).
#
# AnyKernel3 is pinned to a commit: its anykernel.sh variable names follow the bundled
# ak3-core.sh (they were renamed to uppercase in 2024), so an unpinned clone can silently
# stop matching the install script. Bump AK3_REF and re-run tools/ak3/flash_test.sh together.
#
# AK3_INSTALL=<file> packs that install script instead of tools/ak3/anykernel.sh, and
# AK3_EXTRA="<src>=<name> ..." puts more files in the zip (the 6.18 test kernel needs both:
# tools/ak3/anykernel-618.sh and the ramdisk, packed by tools/ak3/pack618.sh).
#
# usage: tools/ak3/pack.sh <Image> <out.zip> [ak3-ref]
set -euo pipefail

AK3_REF_DEFAULT=020dfeccf9d7e962a48400fc94d3e451df92eead  # 2026-09-04, magisk utils v31.0
[ $# -ge 2 ] || { sed -n '2,12p' "$0"; exit 2; }
image=$(readlink -f "$1")
out=$(readlink -m "$2")
ref=${3:-$AK3_REF_DEFAULT}
here=$(dirname "$(readlink -f "$0")")

w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
if [ -n "${AK3_SOURCE:-}" ]; then
  git clone -q --no-hardlinks "$AK3_SOURCE" "$w/ak3"
else
  git clone -q https://github.com/osm0sis/AnyKernel3 "$w/ak3"
fi
git -C "$w/ak3" checkout -q "$ref"
echo "AnyKernel3 at $(git -C "$w/ak3" log -1 --format='%h %ad %s' --date=short)"
rm -rf "$w/ak3/.git" "$w/ak3/.github" "$w/ak3/README.md"
cp "${AK3_INSTALL:-$here/anykernel-618.sh}" "$w/ak3/anykernel.sh"
cp "$image" "$w/ak3/Image"
for extra in ${AK3_EXTRA:-}; do
  cp "${extra%%=*}" "$w/ak3/${extra##*=}"
done
# the install script and the bundled core must agree on the variable names
# shellcheck disable=SC2016  # a literal $BLOCK: the name the core reads
if ! grep -q '^BLOCK=' "$w/ak3/anykernel.sh" ||
   ! grep -q 'abort "Unable to determine \$BLOCK partition' "$w/ak3/tools/ak3-core.sh"; then
  echo "::error::anykernel.sh and this AnyKernel3's ak3-core.sh disagree on variable names" >&2
  exit 1
fi
mkdir -p "$(dirname "$out")"
rm -f "$out"
(cd "$w/ak3" && zip -qr9 "$out" . -x '.git*')
echo "packed $out"
