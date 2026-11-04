#!/system/bin/sh
# nuwa-618 at late boot: the switches that act at once (switches.sh), and the wakelock blocker's
# own list as the WebUI last saved it.
MODDIR=${0%/*}
# shellcheck disable=SC1091  # next to this script
. "$MODDIR/switches.sh"
. "$MODDIR/trial-guard.sh"
guard_boot
sw_apply_now
[ -r "$A/nuwa_618_wl_user" ] && w "$(cat "$A/nuwa_618_wl_user")" "$WLB/wakelock_blocker"
exit 0
