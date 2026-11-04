#!/usr/bin/env bash
# Source this file from Linux/WSL. Legacy port scripts receive independent paths.
PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
account_home=$(getent passwd "$(id -un)" | cut -d: -f6)
BUILD_ROOT=$(realpath -m -- "${BUILD_ROOT:-$account_home/xiaomi13-k618-build}")
case "$BUILD_ROOT" in
  /|/home|"$account_home"|/mnt/*)
    echo "Choose a new native Linux BUILD_ROOT, separate from the original workspace: $BUILD_ROOT" >&2
    return 2 2>/dev/null || exit 2;;
esac
case "$BUILD_ROOT$PROJECT_ROOT" in *[[:space:]]*) echo 'Legacy build scripts require paths without whitespace.' >&2; return 2 2>/dev/null || exit 2;; esac
export PROJECT_ROOT BUILD_ROOT
export NB="$BUILD_ROOT" K618="$BUILD_ROOT/k618"
export MKB="$BUILD_ROOT/home/mkbootimg" LZ4_DEV="$BUILD_ROOT/lz4-dev"
export AK3_SOURCE="$BUILD_ROOT/AnyKernel3"
export KBUILD_GENDWARFKSYMS_STABLE=1
