#!/system/bin/sh
# nuwa-618's WebUI backend, run by the page through ksu.exec (as /system/bin/sh, mksh: its
# arithmetic is 32-bit, so sizes in bytes are left to awk). Commands:
#   status                    key=value lines for the page
#   switch <name> <1|0>       a switch on / off (switches.sh); acts at once where it can
#   num <zram_mb|recomp_secs> <value>   a choice; recomp_secs acts at once
#   wl_user <list>            the wakelock blocker's own list (';'-separated), kept for next boot
#   recomp_now                a round of zram's cold tier now
M=${0%/*}
# shellcheck disable=SC1091  # next to this script
. "$M/switches.sh"
. "$M/trial-guard.sh"
# shellcheck disable=SC1091  # next to this script
. "$M/perfhal.sh"

p() { printf '%s=%s\n' "$1" "$2"; }
rl() { cat "$1" 2> /dev/null | head -n 1; }	# (not r: mksh has r as an alias of fc)

status() {
    v=
    read -r x1 x2 v x3 2> /dev/null < /proc/version
    p kernel "$v"
    p module "$(sed -n 's/^version=//p' "$M/module.prop")"
    guard_status || p trial_error 1
    k618=0
    [ -e $Z/disksize_mb ] && k618=1
    p k618 $k618
    for s in $SWITCHES; do
        n=${s%%:*}
        if sw_on "$n"; then p "sw_$n" 1; else p "sw_$n" 0; fi
    done
    p zram_mb "$(sw_num zram_mb 16384)"
    p recomp_secs "$(sw_num recomp_secs 600)"
    # zram as it runs now
    z=/sys/block/zram0
    p zram_alg "$(sed -n 's/.*\[\(.*\)\].*/\1/p' $z/comp_algorithm 2> /dev/null)"
    p zram_recomp "$(sed -n 's/.*\[\(.*\)\].*/\1/p' $z/recomp_algorithm 2> /dev/null)"
    rl $z/disksize | awk '{ printf "zram_size=%.0f\n", $1 / 1048576 }'
    rl $z/mm_stat | awk '{ printf "zram_orig=%.0f\nzram_compr=%.0f\nzram_used=%.0f\n", $1 / 1048576, $2 / 1048576, $3 / 1048576;
        if ($2 > 0) printf "zram_ratio=%.2f\n", $1 / $2 }'
    p recomp_rounds "$(rl $Z/recomp_rounds)"
    p recomp_ms "$(rl $Z/recomp_ms)"
    t=$(pidof zram_recomp 2> /dev/null)
    [ -n "$t" ] && awk '{ printf "recomp_cpu_s=%.1f\n", ($14 + $15) / 100 }' /proc/$t/stat 2> /dev/null
    # what the other switches have done since boot
    p rmap_walk_stops "$(rl /proc/sys/vm/rmap_walk_stops)"
    p rmap_mapcount_kept "$(rl /proc/sys/vm/rmap_mapcount_kept)"
    p throttle_exempt_count "$(rl /proc/sys/vm/throttle_exempt_count)"
    p erofs_prio "$(rl $P/erofs/parameters/worker_prio)"
    # scheduling -- each policy's governor, schedutilmium's ceiling state, k618_place's state
    # ("key value" lines -> place_key=value), the reclaim counters and where kswapd may run
    for n in 0 3 7; do p "gov$n" "$(rl $CF/policy$n/scaling_governor)"; done
    p caps3 "$(rl $CF/policy3/schedutilmium/caps_state)"
    p caps7 "$(rl $CF/policy7/schedutilmium/caps_state)"
    # the perf HAL's table -- "on N" (our copy, N hints edited), "unlabelled N", "off", "none"
    p hal "$(perfhal_status)"
    p sbs_now "$(getprop persist.sys.sched_boost_enable)"
    [ -r $PL/state ] && awk '{ k = $1; $1 = ""; sub(/^ /, ""); printf "place_%s=%s\n", k, $0 }' $PL/state 2> /dev/null
    p look_around_skipped "$(rl /proc/sys/vm/look_around_skipped)"
    p kcompressd_pages "$(rl /proc/sys/vm/kcompressd_pages)"
    p kcompressd_freed "$(rl /proc/sys/vm/kcompressd_freed)"
    t=$(pidof kswapd0 2> /dev/null)
    [ -n "$t" ] && p kswapd_cpus "$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/$t/status 2> /dev/null)"
    # standby
    p suspend_ok "$(rl /sys/power/suspend_stats/success)"
    p suspend_fail "$(rl /sys/power/suspend_stats/fail)"
    d=$(grep -m1 debugfs /proc/mounts | cut -d' ' -f2)
    [ -n "$d" ] && p aosd "$(sed -n 's/^Count = //p' "$d/qcom_sleep_stats/aosd" 2> /dev/null)"
    p wl_default "$(rl $WLB/wakelock_blocker_default)"
    p wl_user "$(rl $WLB/wakelock_blocker)"
    # the wakeup sources that kept the phone awake longest since boot (name, ms)
    if [ -n "$d" ] && [ -r "$d/wakeup_sources" ]; then
        awk 'NR > 1 { print $(NF-3), $1 }' "$d/wakeup_sources" | sort -rn | head -n 8 |
            awk '{ printf "ws=%s %s\n", $2, $1 }'
    fi
}

case "$1" in
    status) status ;;
    switch)
        case "$3" in 0|1) ;; *) echo "bad switch value"; exit 1;; esac
        f=$(sw_file "$2") || { echo "no switch $2"; exit 1; }
        phase=now
        for s in $SWITCHES; do case "$s" in "$2":*) phase=${s##*:};; esac; done
        guard_prepare "$f" "$phase" || { echo "cannot record setting trial"; exit 1; }
        sw_set "$2" "$3" || { echo "no switch $2"; exit 1; }
        guard_schedule "$f"
        sw_apply_now
        sw_apply_gov
        # the perf HAL's table: the copy comes or goes, the HAL started again to read it
        [ "$2" != hal_little ] || perfhal_apply restart || { echo "perf HAL table not changed"; exit 1; }
        [ "$2" != sbs_off ] || sw_sbs_store
        ;;
    num)
        case "$3" in ''|*[!0-9]*) echo "bad number"; exit 1;; esac
        [ "${#3}" -le 5 ] || { echo "number too large"; exit 1; }
        case "$2:$3" in
            zram_mb:[0-9]*) [ "$3" -le 65536 ] || exit 1; phase=boot ;;
            recomp_secs:[0-9]*) [ "$3" -le 86400 ] || exit 1; phase=now ;;
            *) echo "bad $2 $3"; exit 1 ;;
        esac
        f=$A/nuwa_618_$2
        guard_prepare "$f" "$phase" || exit 1
        echo "$3" > "$f"
        guard_schedule "$f"
        [ "$2" = recomp_secs ] && sw_apply_now
        ;;
    wl_user)
        [ "${#2}" -lt 1024 ] || { echo "list too long"; exit 1; }
        guard_prepare "$A/nuwa_618_wl_user" now || exit 1
        printf '%s' "$2" > "$A/nuwa_618_wl_user"
        guard_schedule "$A/nuwa_618_wl_user"
        w "$2" $WLB/wakelock_blocker
        ;;
    recomp_now) w 1 $Z/recomp_now ;;
    *) echo "usage: webui.sh status|switch|num|wl_user|recomp_now"; exit 2 ;;
esac
