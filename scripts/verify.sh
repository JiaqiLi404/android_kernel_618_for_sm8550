#!/usr/bin/env bash
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use the limited entry point.' >&2; exit 2; }
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
TAG=${TAG:-release}
[[ "$TAG" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
dist="$PROJECT_ROOT/dist/$TAG"
mkdir -p "$PROJECT_ROOT/logs/$TAG"
exec > >(tee "$PROJECT_ROOT/logs/$TAG/validation.log") 2>&1
python3 "$PROJECT_ROOT/scripts/verify.py" "$dist"
cp "$K618/out-gki/.config" "$dist/kernel.config"
cp "$K618/boot-test/first-stage-missing.txt" "$dist/first-stage-missing.txt"
cp "$K618/boot-test/depmod.err" "$dist/depmod.txt"
extra=()
for report in installer-validation.json runtime-validation.json; do
  [ ! -f "$dist/$report" ] || extra+=("$report")
done
(cd "$dist" && sha256sum boot.img *.zip kernel.config kernel.symvers validation.json "${extra[@]}" first-stage-missing.txt depmod.txt > SHA256SUMS)
date -u +%FT%TZ | tee "$PROJECT_ROOT/logs/$TAG/completed.txt"
echo "BUILD AND PACKAGE VALIDATION PASSED: dist/$TAG"
