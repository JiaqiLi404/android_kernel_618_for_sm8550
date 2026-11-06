#!/system/bin/sh
# nuwa-618 at post-fs-data: before HyperOS's system_perf_init sizes zram and before apexd sets up
# most of its loop devices -- the switches those read when they are set up (switches.sh).
MODDIR=${0%/*}
# shellcheck disable=SC1091  # next to this script
. "$MODDIR/switches.sh"
. "$MODDIR/trial-guard.sh"
guard_boot
sw_apply_boot
# place_em is off unless its file is there (place_em_on): an older "off" file means nothing
rm -f "$A/nuwa_618_place_em_off"

# The perf HAL's touch and composition boosts on the little cores alone (the switch "hal_little",
# perfhal.sh): its table edited and mounted over its own now, before the HAL starts (it reads it
# once) and before the first unlock -- the copy on a tmpfs of its own, kept where the kernel's SUSFS
# marks it (a marked mount's id has ten digits and more: an app the root manager hides modules from
# does not see it), elsewhere a plain directory of /dev. The copy's SELinux type may be on a tmpfs
# once the policy says so: one call of KernelSU's tool.
if sw_on hal_little && [ -s /vendor/etc/perf/perfboostsconfig.xml ]; then
    # shellcheck disable=SC1091  # next to this script
    . "$MODDIR/perfhal.sh"
    if mkdir -p "$PH_DIR" 2>/dev/null && mount -t tmpfs -o mode=755 tmpfs "$PH_DIR" 2>/dev/null; then
        case "$(grep -m 1 " $PH_DIR " /proc/self/mountinfo 2>/dev/null)" in
            [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]*) ;;
            *) umount "$PH_DIR" 2>/dev/null ;;
        esac
    fi
    # shellcheck disable=SC2154  # perfhal_type sets ph_type
    if perfhal_type && perfhal_policy "allow $ph_type tmpfs filesystem associate"; then
        # shellcheck disable=SC2034  # perfhal.sh's perfhal_label reads it
        PH_ASKED=$ph_type
    fi
    # shellcheck disable=SC2119  # (no restart: the HAL has not started yet)
    perfhal_apply
fi
exit 0
