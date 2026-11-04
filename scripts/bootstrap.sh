#!/usr/bin/env bash
# Construct build inputs directly from fixed upstream sources and official OTAs.
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use bash scripts/run-limited.sh bootstrap.' >&2; exit 2; }
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
exec python3 "$PROJECT_ROOT/scripts/bootstrap-inputs.py" "$@"
