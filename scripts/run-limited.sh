#!/usr/bin/env bash
# Public entry point: enforce memory, swap and CPU limits for preparation/build.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
action=${1:-}
case "$action" in bootstrap|prepare|build|verify|test-installer|test-runtime) ;; *) echo 'Usage: bash scripts/run-limited.sh bootstrap|prepare|build|verify|test-installer|test-runtime'; exit 2;; esac
shift
. "$here/project-env.sh"
launcher=()
if [ "$(id -u)" -ne 0 ]; then launcher=(sudo); fi
exec "${launcher[@]}" systemd-run --scope --quiet \
  --uid="$(id -u)" --gid="$(id -g)" \
  -p MemoryMax=12G -p MemoryHigh=10G -p MemorySwapMax=1G -p CPUQuota=400% \
  env BUILD_ROOT="$BUILD_ROOT" TAG="${TAG:-release}" JOBS="${JOBS:-4}" \
  bash "$here/cgroup-check.sh" bash "$here/$action.sh" "$@"
