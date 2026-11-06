#!/system/bin/sh
# nuwa-618's switches, for post-fs-data.sh, service.sh and webui.sh. The 6.18 kernel (k618) has
# every change on by default; a switch that is OFF puts the stock value back. A
# switch is a file in /data/adb, there when the switch is off; a choice (a size, an interval) is a
# file holding the number. No file at all = the kernel as built.
#
#   switch            file (nuwa_618_...)    what the kernel has on / what off writes        when
#   zram_tiered       zram_stock             lz4, cold pages recompressed with zstd /        boot
#                                            lzo-rle, no second algorithm
#   zram_mb           zram_mb (a number)     16384 MB whatever HyperOS writes / 0 = HyperOS's boot
#   recomp_secs       recomp_secs (number)   600 s awake between two rounds                  now
#   rmap_stop         rmap_stop_off          vm.rmap_walk_stop 1 / 0                          now
#   rmap_keep         rmap_keep_off          vm.rmap_mapcount_keep 32 / 0                     now
#   kill_reap         kill_reap_off          vm.reap_killed_mb 32 / 0                         now
#   throttle_exempt   throttle_off           vm.throttle_exempt 1 / 0                         now
#   erofs_prio        erofs_prio_off         erofs.worker_prio 3 / 1                          now
#   loop_hipri        loop_hipri_off         loop.hipri_wq Y / N (loop devices set up later)  boot
#   fence_names       fence_names_on         sync_file.omit_names Y / N                       now
#   wl_blocker        wl_blocker_off         the built-in wakelock list / none                now
#   wl_untimed        wl_untimed_off         wakelock.untimed_ms 500 / 0                      now
#   alarm_hold        alarm_hold_off         alarmtimer.short_hold Y / N                      now
#
# Whatever the kernel lacks (another kernel, an older k618) is left alone: every write is
# guarded by the node existing.

A=/data/adb
P=/sys/module
Z=/sys/module/zram/parameters
WLB=/sys/devices/virtual/misc/wakelock_blocker
# the kernel's built-in list (tools/kernel-patches/wakelock_blocker.py --defaults), for switching back on
WL_DEFAULT="qcom_rx_wakelock;IPA_CLIENT_APPS_WAN_COAL_CONS;IPA_CLIENT_APPS_WAN_LOW_LAT_CONS;IPA_CLIENT_APPS_LAN_CONS;IPA_WS;DIAG_WS;RMNET_DFC;RMNET_SHS;rmnet_ctl;rmnet_ipa%d;hal_bluetooth_lock;IPA_CLIENT_APPS_LAN_COAL_CONS;1-0008"

# name file default-on(1) when(boot|now)
SWITCHES="zram_tiered:zram_stock:boot rmap_stop:rmap_stop_off:now rmap_keep:rmap_keep_off:now
kill_reap:kill_reap_off:now throttle_exempt:throttle_off:now erofs_prio:erofs_prio_off:now
loop_hipri:loop_hipri_off:boot fence_names:fence_names_on:now wl_blocker:wl_blocker_off:now
wl_untimed:wl_untimed_off:now alarm_hold:alarm_hold_off:now verity_cache:verity_cache_off:now"

sw_file() {
    for s in $SWITCHES; do
        case "$s" in "$1":*) f=${s#*:}; echo "$A/nuwa_618_${f%%:*}"; return 0 ;; esac
    done
    return 1
}

# sw_on <name>: 0 when the switch is on (its file is absent)
sw_on() {
    f=$(sw_file "$1") || return 1
    [ ! -e "$f" ]
}

sw_set() {	# sw_set <name> 1|0
    f=$(sw_file "$1") || return 1
    if [ "$2" = 1 ]; then rm -f "$f"; else : > "$f"; fi
}

# a number chosen in the WebUI, or the kernel's default
sw_num() {	# sw_num <file> <default>
    v=
    [ -r "$A/nuwa_618_$1" ] && read -r v < "$A/nuwa_618_$1"
    case "$v" in '' | *[!0-9]*) echo "$2" ;; *) echo "$v" ;; esac
}

w() {	# w <value> <node>: write when the node is there
    [ -e "$2" ] && echo "$1" > "$2" 2> /dev/null
}

# the switches that act at once (service.sh at boot, webui.sh on a change)
sw_apply_now() {
    if sw_on rmap_stop; then w 1 /proc/sys/vm/rmap_walk_stop; else w 0 /proc/sys/vm/rmap_walk_stop; fi
    if sw_on rmap_keep; then w 32 /proc/sys/vm/rmap_mapcount_keep; else w 0 /proc/sys/vm/rmap_mapcount_keep; fi
    if sw_on kill_reap; then w 32 /proc/sys/vm/reap_killed_mb; else w 0 /proc/sys/vm/reap_killed_mb; fi
    if sw_on throttle_exempt; then w 1 /proc/sys/vm/throttle_exempt; else w 0 /proc/sys/vm/throttle_exempt; fi
    if sw_on erofs_prio; then w 3 $P/erofs/parameters/worker_prio; else w 1 $P/erofs/parameters/worker_prio; fi
    if sw_on fence_names; then w Y $P/sync_file/parameters/omit_names; else w N $P/sync_file/parameters/omit_names; fi
    if sw_on wl_blocker; then w "$WL_DEFAULT" $WLB/wakelock_blocker_default; else w "" $WLB/wakelock_blocker_default; fi
    if sw_on wl_untimed; then w 500 $P/wakelock/parameters/untimed_ms; else w 0 $P/wakelock/parameters/untimed_ms; fi
    if sw_on alarm_hold; then w Y $P/alarmtimer/parameters/short_hold; else w N $P/alarmtimer/parameters/short_hold; fi
    if sw_on verity_cache; then w Y $P/dm_bufio/parameters/retain_when_free; else w N $P/dm_bufio/parameters/retain_when_free; fi
    if sw_on zram_tiered; then w "$(sw_num recomp_secs 600)" $Z/recomp_idle_secs; else w 0 $Z/recomp_idle_secs; fi
}

# the switches zram and the loop devices read when they are set up (post-fs-data.sh: HyperOS
# sets zram up later, at system_perf_init)
sw_apply_boot() {
    if sw_on loop_hipri; then w Y $P/loop/parameters/hipri_wq; else w N $P/loop/parameters/hipri_wq; fi
    w "$(sw_num zram_mb 16384)" $Z/disksize_mb
    if sw_on zram_tiered; then
        w zstd $Z/recomp_default
    else
        w "" $Z/recomp_default
        w 0 $Z/recomp_idle_secs
        # zram0 exists already (zram.ko loads in the first stage) with zstd as its second
        # algorithm: before HyperOS sizes it, a reset takes recomp_default (now empty) again
        if [ "$(cat /sys/block/zram0/disksize 2> /dev/null)" = 0 ]; then
            w 1 /sys/block/zram0/reset
            w lzo-rle /sys/block/zram0/comp_algorithm
        fi
    fi
}
