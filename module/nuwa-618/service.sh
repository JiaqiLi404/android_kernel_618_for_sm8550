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
# SchedBoostService's two properties kept for later boots as the switch stands (switches.sh)
sw_sbs_store
# the vendor's post-boot script sets every policy's governor late in the boot ("walt", which
# is schedutilmium on k618), so the governor switches are applied once more after it
(
    i=0
    while [ "$(getprop sys.boot_completed)" != 1 ] && [ $i -lt 300 ]; do sleep 2; i=$((i + 1)); done
    sleep 20
    sw_apply_gov
) &
exit 0
