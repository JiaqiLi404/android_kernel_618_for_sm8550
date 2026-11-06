#!/usr/bin/env bash
# End-to-end build; no adb, fastboot, device writes or original-workspace access.
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use bash scripts/run-limited.sh build to enforce resource limits.' >&2; exit 2; }
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
[ -f "$BUILD_ROOT/.xiaomi13-k618-ready" ] || { echo 'Prepare inputs with bash scripts/run-limited.sh bootstrap (or prepare for offline caches).' >&2; exit 2; }
JOBS=${JOBS:-4}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }
TAG=${TAG:-release}
[[ "$TAG" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo 'Invalid TAG' >&2; exit 2; }
export TAG JOBS
. "$PROJECT_ROOT/profiles/default.env"
mkdir -p "$PROJECT_ROOT/logs/$TAG" "$PROJECT_ROOT/dist/$TAG"
LOGDIR="$PROJECT_ROOT/logs/$TAG"
rm -f "$LOGDIR/completed.txt" "$PROJECT_ROOT/dist/$TAG/installer-validation.json" "$PROJECT_ROOT/dist/$TAG/runtime-validation.json"
exec > >(tee -a "$LOGDIR/build.log") 2>&1
date -u +%FT%TZ | tee "$LOGDIR/started.txt"
run() { echo "[$(date -u +%FT%TZ)] $*"; "$@"; }
run bash "$PROJECT_ROOT/kernel-6.18/k618_gki.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_stage2.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_tp.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_rpc515.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_touch.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_tof.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_cs35l41.sh" "$JOBS"
run bash "$PROJECT_ROOT/kernel-6.18/k618_modgap.sh"
run bash "$PROJECT_ROOT/kernel-6.18/k618_timg.sh"
cp "$K618/boot-test/boot-618-$TAG.img" "$PROJECT_ROOT/dist/$TAG/boot.img"
run bash "$PROJECT_ROOT/tools/ak3/pack618.sh" "$PROJECT_ROOT/dist/$TAG/boot.img" "$PROJECT_ROOT/dist/$TAG/nuwa-kernel-6.18-AnyKernel3.zip"
# an earlier version's tuning zip would otherwise enter SHA256SUMS through *.zip
rm -f "$PROJECT_ROOT/dist/$TAG"/nuwa-618-v*.zip
(cd "$PROJECT_ROOT/module/nuwa-618" && zip -qr9 "$PROJECT_ROOT/dist/$TAG/nuwa-618-v4.zip" .)
run bash "$PROJECT_ROOT/scripts/verify.sh"
