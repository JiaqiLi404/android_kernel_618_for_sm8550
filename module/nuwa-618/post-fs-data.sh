#!/system/bin/sh
# nuwa-618 at post-fs-data: before HyperOS's system_perf_init sizes zram and before apexd sets up
# most of its loop devices -- the switches those read when they are set up (switches.sh).
MODDIR=${0%/*}
# shellcheck disable=SC1091  # next to this script
. "$MODDIR/switches.sh"
. "$MODDIR/trial-guard.sh"
guard_boot
sw_apply_boot
