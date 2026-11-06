#!/system/bin/sh
# nuwa-618's switches, for post-fs-data.sh, service.sh and webui.sh. The 6.18 kernel (k618) has
# every change on by default; a switch that is OFF puts the stock value back. A
# switch is a file in /data/adb, there when the switch is off; a choice (a size, an interval) is a
# file holding the number. No file at all = the kernel as built. The switches of SW_ASKED are the
# other way round: off unless their file is there (off by default).
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
#   verity_cache      verity_cache_off       dm_bufio.retain_when_free Y / N (k618_memory.py) now
#   -- scheduling --
#   gov_mium          gov_stock              schedutilmium on every policy / schedutil        now*
#   caps              caps_off               schedutilmium caps 1 on policy3 and 7 / 0        now*
#   mid_burst         mid_burst_off          policy3 caps_burst 1536000 / 0                   now*
#   touch_floor       touch_floor_off        policy0 touch_floor 1344000 / 0                  now*
#   hal_little        hal_little_off         the perf HAL's tap / scroll / drag / large-       boot,
#                                            composition hints without their middle and prime  now
#                                            floors (perfhal.sh) / the vendor's table
#   sbs_off           sbs_stock              MIUI's SchedBoostService off: no real-time,       boot
#                                            middle-and-prime-bound UI threads and no perf
#                                            locks at each swipe / the phone's own
#   place_rt          place_rt_off           k618_place rt 1 / 0                              now
#   place_pull        place_pull_off         k618_place pull 1 / 0                            now
#   place_slice       place_slice_off        k618_place slice 1 / 0                           now
#   place_sync        place_sync_off         k618_place sync 1 / 0                            now
#   place_em          place_em_on (SW_ASKED) k618_place em 1, the measured energy model / 0,   now
#                                            the device tree's (Qualcomm's), the kernel's own
#                                            default -- off unless its file is there
#   look_around       look_around_off        vm.look_around 1 / 0                             now
#   kswapd_mid        kswapd_mid_off         vm.kswapd_mid 1 / 0                              now
#   kcompressd        kcompressd_off         vm.kcompressd 2 / 0                              now
#   (* the vendor's post-boot script writes "walt" -- schedutilmium -- into every policy late in
#   the boot, so service.sh applies these once more after sys.boot_completed: sw_apply_gov)
#
# Whatever the kernel lacks (another kernel, an older k618) is left alone: every write is
# guarded by the node existing.

A=/data/adb
P=/sys/module
Z=/sys/module/zram/parameters
CF=/sys/devices/system/cpu/cpufreq
PL=/sys/module/k618_place/parameters
WLB=/sys/devices/virtual/misc/wakelock_blocker
# the kernel's built-in list (tools/kernel-patches/wakelock_blocker.py --defaults), for switching back on
WL_DEFAULT="qcom_rx_wakelock;IPA_CLIENT_APPS_WAN_COAL_CONS;IPA_CLIENT_APPS_WAN_LOW_LAT_CONS;IPA_CLIENT_APPS_LAN_CONS;IPA_WS;DIAG_WS;RMNET_DFC;RMNET_SHS;rmnet_ctl;rmnet_ipa%d;hal_bluetooth_lock;IPA_CLIENT_APPS_LAN_COAL_CONS;1-0008"

# name file default-on(1) when(boot|now)
SWITCHES="zram_tiered:zram_stock:boot rmap_stop:rmap_stop_off:now rmap_keep:rmap_keep_off:now
kill_reap:kill_reap_off:now throttle_exempt:throttle_off:now erofs_prio:erofs_prio_off:now
loop_hipri:loop_hipri_off:boot fence_names:fence_names_on:now wl_blocker:wl_blocker_off:now
wl_untimed:wl_untimed_off:now alarm_hold:alarm_hold_off:now verity_cache:verity_cache_off:now
gov_mium:gov_stock:now caps:caps_off:now mid_burst:mid_burst_off:now touch_floor:touch_floor_off:now
hal_little:hal_little_off:now sbs_off:sbs_stock:boot place_rt:place_rt_off:now place_pull:place_pull_off:now
place_slice:place_slice_off:now place_sync:place_sync_off:now place_em:place_em_on:now
look_around:look_around_off:now kswapd_mid:kswapd_mid_off:now kcompressd:kcompressd_off:now"
# off unless their file is there: place_em (the measured table sends more tasks to the prime)
SW_ASKED="place_em"

sw_file() {
    for s in $SWITCHES; do
        case "$s" in "$1":*) f=${s#*:}; echo "$A/nuwa_618_${f%%:*}"; return 0 ;; esac
    done
    return 1
}

sw_asked() {	# sw_asked <name>: 0 when the switch is off unless its file is there
    case " $SW_ASKED " in *" $1 "*) return 0 ;; esac
    return 1
}

# sw_on <name>: 0 when the switch is on (its file is absent; for SW_ASKED, there)
sw_on() {
    f=$(sw_file "$1") || return 1
    if sw_asked "$1"; then [ -e "$f" ]; else [ ! -e "$f" ]; fi
}

sw_set() {	# sw_set <name> 1|0
    f=$(sw_file "$1") || return 1
    if sw_asked "$1"; then
        if [ "$2" = 1 ]; then : > "$f"; else rm -f "$f"; fi
    elif [ "$2" = 1 ]; then rm -f "$f"; else : > "$f"; fi
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
    # placement (k618_place, loaded in the first stage) and reclaim
    for x in em rt pull slice sync; do
        if sw_on place_$x; then w 1 $PL/$x; else w 0 $PL/$x; fi
    done
    if sw_on look_around; then w 1 /proc/sys/vm/look_around; else w 0 /proc/sys/vm/look_around; fi
    if sw_on kswapd_mid; then w 1 /proc/sys/vm/kswapd_mid; else w 0 /proc/sys/vm/kswapd_mid; fi
    if sw_on kcompressd; then w 2 /proc/sys/vm/kcompressd; else w 0 /proc/sys/vm/kcompressd; fi
    sw_apply_mium
}

# the governor of every policy -- schedutilmium (what "walt" means on k618) or schedutil.
# Only a governor the policy offers is written, and only when it is not the one running.
sw_apply_gov() {
    if sw_on gov_mium; then g=schedutilmium; else g=schedutil; fi
    for pd in $CF/policy*; do
        [ -e "$pd/scaling_governor" ] || continue
        grep -qw "$g" "$pd/scaling_available_governors" 2> /dev/null || continue
        [ "$(cat "$pd/scaling_governor" 2> /dev/null)" = "$g" ] || echo "$g" > "$pd/scaling_governor" 2> /dev/null
    done
    sw_apply_mium
}

# schedutilmium's own files where it runs (each policy has its directory): the values the kernel
# starts with when a switch is on, 0 when it is off
sw_apply_mium() {
    m=schedutilmium
    if sw_on caps; then v=1; else v=0; fi
    w $v $CF/policy3/$m/caps
    w $v $CF/policy7/$m/caps
    if sw_on mid_burst; then w 1536000 $CF/policy3/$m/caps_burst; else w 0 $CF/policy3/$m/caps_burst; fi
    if sw_on touch_floor; then w 1344000 $CF/policy0/$m/touch_floor; else w 0 $CF/policy0/$m/touch_floor; fi
}

# The switch "sbs_off": MIUI's SchedBoostService in
# system_server off. While an interface animates -- a touch, a swipe, the shade, an app that starts --
# it makes the app's main thread and RenderThread real-time and binds them to
# persist.sys.miui_animator_sched.bigcores (3-7 on the port: the prime included; seen with a probe
# on sched_setaffinity, Douyin's two threads at every swipe), and takes perf locks (little and
# middle at 1785 MHz or more for ~0.4-0.6 s). k618_place leaves a draw thread made real-time to the
# kernel's own RT placement, which takes any idle CPU of that mask -- often the prime. The service
# reads persist.sys.sched_boost_enable (true unless set) and persist.sys.sched_boost_cloud_enable
# (false: the cloud's setting may turn it on again) once, when system_server starts: sw_apply_boot
# puts both in memory before that, sw_sbs_store keeps them for later boots -- on: false and true;
# off: the phone's true and false. It takes the next boot.
sw_sbs_store() {
    if sw_on sbs_off; then
        setprop persist.sys.sched_boost_enable false 2> /dev/null
        setprop persist.sys.sched_boost_cloud_enable true 2> /dev/null
    else
        setprop persist.sys.sched_boost_enable true 2> /dev/null
        setprop persist.sys.sched_boost_cloud_enable false 2> /dev/null
    fi
    return 0
}

# the switches zram and the loop devices read when they are set up (post-fs-data.sh: HyperOS
# sets zram up later, at system_perf_init), and the properties system_server reads at its start
sw_apply_boot() {
    if sw_on sbs_off; then
        resetprop -n persist.sys.sched_boost_enable false 2> /dev/null
        resetprop -n persist.sys.sched_boost_cloud_enable true 2> /dev/null
    fi
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
