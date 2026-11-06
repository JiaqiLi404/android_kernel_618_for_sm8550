#!/usr/bin/env bash
# Extract the shipped snapshot, then remove old build products in this owned copy.
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use bash scripts/run-limited.sh prepare to enforce resource limits.' >&2; exit 2; }
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
if [ -f "$BUILD_ROOT/.xiaomi13-k618-ready" ]; then
  echo "Already prepared: $BUILD_ROOT"
  exit 0
fi
if [ -e "$BUILD_ROOT" ] && [ -n "$(find "$BUILD_ROOT" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
  echo "BUILD_ROOT is nonempty and unprepared; choose an empty directory: $BUILD_ROOT" >&2
  exit 2
fi
for command in tar zstd git python3 rsync make gcc g++ flex bison bc lz4 cpio depmod zip unzip; do
  command -v "$command" >/dev/null || { echo "Missing $command; see docs/PROVENANCE.en.md#acquire" >&2; exit 2; }
done
cd "$PROJECT_ROOT"
for archive in assets/k618-workspace.tar.zst assets/support-inputs.tar.zst assets/anykernel3.tar.zst assets/qemu-user-static.deb; do
  [ -f "$archive" ] || { echo 'Offline input cache missing; use bash scripts/run-limited.sh bootstrap to reconstruct inputs from upstream.' >&2; exit 2; }
done
sha256sum -c assets/SHA256SUMS
mkdir -p "$BUILD_ROOT"
printf '%s\n' "$PROJECT_ROOT" > "$BUILD_ROOT/.xiaomi13-k618-workspace"
tar --no-same-owner --zstd -xf assets/k618-workspace.tar.zst -C "$BUILD_ROOT"
tar --no-same-owner --zstd -xf assets/support-inputs.tar.zst -C "$BUILD_ROOT"
mkdir -p "$AK3_SOURCE"
tar --no-same-owner --zstd -xf assets/anykernel3.tar.zst -C "$AK3_SOURCE" --strip-components=1
python3 "$PROJECT_ROOT/scripts/relocate.py" "$BUILD_ROOT"
for relative in k618/ws/common k618/KernelSU k618/src/fuxi k618/soc; do
  # These are newly extracted repositories inside our marked, independent build root.
  git -C "$BUILD_ROOT/$relative" reset --hard HEAD
  git -C "$BUILD_ROOT/$relative" clean -fdx > "$BUILD_ROOT/clean-$(basename "$relative").log"
done
# A cache made before the current SoC patch or without the 13 Ultra ToF source cannot build this tree.
want=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["target_tree"])' "$PROJECT_ROOT/provenance/soc-baseline.json")
[ "$(git -C "$K618/soc" rev-parse 'HEAD^{tree}')" = "$want" ] &&
  [ -f "$K618/src/ishtar/tree/drivers/input/stmvl53l5/stmvl53l5_module.c" ] || {
  echo 'Offline cache predates the current inputs; use bash scripts/run-limited.sh bootstrap with an empty BUILD_ROOT.' >&2
  exit 2
}
git -C "$K618/soc" config core.excludesFile "$PROJECT_ROOT/kernel-6.18/k618_soc_gitignore.txt"
git -C "$K618/KernelSU" remote set-url origin https://github.com/tiann/KernelSU.git
python3 - "$BUILD_ROOT" <<'PY'
from pathlib import Path
import shutil, sys
root = Path(sys.argv[1]).resolve()
assert (root / '.xiaomi13-k618-workspace').is_file()
for name in ('out-gki', 'out-gki-ir', 'soc-kconfig', 'mark', 'tpmods', 'tp-log', 'vendor', 'modgap'):
    target = root / 'k618' / name
    assert target.resolve().is_relative_to(root)
    if target.exists():
        shutil.rmtree(target)
boot = root / 'k618/boot-test'
for target in boot.iterdir():
    if target.name == 'init_boot.img':
        continue
    assert target.resolve().is_relative_to(root)
    if target.is_dir():
        shutil.rmtree(target)
    else:
        target.unlink()
PY
bash "$PROJECT_ROOT/kernel-6.18/k618_tp_get.sh"
date -u +%FT%TZ > "$BUILD_ROOT/.xiaomi13-k618-ready"
echo "Ready: $BUILD_ROOT (sources reset, previous build outputs removed)"
