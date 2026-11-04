#!/system/bin/sh
# Persistent-setting trials. Unknown reboot reasons and kernel changes never trigger rollback.
# A waiter sleeps once in its own session/cgroup; there is no idle polling daemon.
G_MODULE=${0%/*}
G_DURATION=120

guard_names() {
    for g_s in $SWITCHES; do
        g_f=${g_s#*:}; printf 'nuwa_618_%s\n' "${g_f%%:*}"
    done
    printf '%s\n' nuwa_618_zram_mb nuwa_618_recomp_secs nuwa_618_wl_user
}

guard_allowed() {
    [ "$1" = "$A/${1##*/}" ] && [ ! -L "$1" ] || return 1
    [ ! -e "$1" ] || [ -f "$1" ] || return 1
    for g_n in $(guard_names); do [ "${1##*/}" != "$g_n" ] || return 0; done
    return 1
}

guard_flock() {
    if [ -x /data/adb/ksu/bin/busybox ]; then
        /data/adb/ksu/bin/busybox flock "$@"
    else
        flock "$@"
    fi
}

guard_lock() {
    G_STATE=$A/nuwa_618_trials
    umask 077
    [ ! -L "$G_STATE" ] || return 1
    mkdir -p "$G_STATE" && chmod 700 "$G_STATE" || return 1
    exec 9> "$G_STATE/lock" || return 1
    guard_flock -w 2 9 || { exec 9>&-; return 1; }
}

guard_unlock() { guard_flock -u 9; exec 9>&-; }

guard_now() {
    N_BOOT=$(cat /proc/sys/kernel/random/boot_id)
    read -r N_UP g_unused < /proc/uptime
    N_UP=${N_UP%%.*}
    N_WALL=$(date +%s)
    if [ -r /sys/kernel/notes ]; then
        N_KERNEL=$(sha256sum /sys/kernel/notes | cut -d' ' -f1)
    else
        N_KERNEL=$( { uname -r; uname -v; } | sha256sum | cut -d' ' -f1)
    fi
}

guard_load() {
    [ -f "$G_REC/meta" ] || return 1
    read -r T_BOOT T_KERNEL T_PHASE T_START T_END T_WALL T_TOKEN < "$G_REC/meta" || return 1
    case "$T_PHASE" in armed|boot) ;; *) return 1;; esac
    [ -n "$T_BOOT" ] && [ -n "$T_KERNEL" ] && [ -n "$T_TOKEN" ] || return 1
    for g_number in "$T_START" "$T_END" "$T_WALL"; do
        case "$g_number" in ''|*[!0-9]*) return 1;; esac
        [ "${#g_number}" -le 10 ] || return 1
    done
}

guard_remove() {
    for g_part in meta meta.tmp value present; do rm -f "$G_REC/$g_part"; done
    rmdir "$G_REC" 2>/dev/null || :
}

guard_save() {
    g_phase=$1
    g_deadline=$((N_UP + G_DURATION))
    g_wall=$(awk -v n="$N_WALL" -v d="$G_DURATION" 'BEGIN { printf "%.0f", n+d }')
    g_token=$N_BOOT:$N_UP:$$
    printf '%s %s %s %s %s %s %s\n' "$N_BOOT" "$N_KERNEL" "$g_phase" \
        "$N_UP" "$g_deadline" "$g_wall" "$g_token" > "$G_REC/meta.tmp" &&
        mv -f "$G_REC/meta.tmp" "$G_REC/meta"
}

# Called before the setting file is changed. Boot-only choices stay queued until boot.
guard_prepare() {
    guard_allowed "$1" || return 1
    case "$2" in now) g_phase=armed;; boot) g_phase=boot;; *) return 1;; esac
    guard_lock || return 1
    guard_now
    G_REC=$G_STATE/${1##*/}
    if [ -d "$G_REC" ]; then
        if ! guard_load || [ "$T_BOOT" != "$N_BOOT" ] || [ "$T_KERNEL" != "$N_KERNEL" ] ||
            { [ "$T_PHASE" = armed ] && [ "$N_UP" -ge "$T_END" ]; }; then
            guard_remove
        elif [ "$g_phase" = boot ] && [ "$T_PHASE" = armed ]; then
            echo "This setting is still in its boot trial; retry after two minutes."
            guard_unlock
            return 1
        fi
    fi
    if [ ! -d "$G_REC" ]; then
        mkdir -m 700 "$G_REC" || { guard_unlock; return 1; }
        if [ -f "$1" ]; then
            cp "$1" "$G_REC/value" && : > "$G_REC/present" || { guard_unlock; return 1; }
        fi
    fi
    guard_save "$g_phase"
    g_result=$?
    [ "$g_result" != 0 ] || rm -f "$G_STATE/last_rollback"
    guard_unlock
    return "$g_result"
}

guard_spawn() {
    if [ -x /data/adb/ksu/bin/busybox ]; then
        ASH_STANDALONE=1 /data/adb/ksu/bin/busybox setsid /data/adb/ksu/bin/busybox \
            sh "$G_MODULE/trial-guard.sh" _wait "$1" "$2" &
    elif command -v setsid >/dev/null 2>&1; then
        setsid sh "$G_MODULE/trial-guard.sh" _wait "$1" "$2" &
    else
        sh "$G_MODULE/trial-guard.sh" _wait "$1" "$2" &
    fi
}

guard_schedule() {
    guard_allowed "$A/${1##*/}" || return 1
    guard_lock || return 1
    G_REC=$G_STATE/${1##*/}
    g_token=
    if guard_load && [ "$T_PHASE" = armed ]; then g_token=$T_TOKEN; fi
    guard_unlock
    [ -z "$g_token" ] || guard_spawn "${1##*/}" "$g_token"
}

guard_settle() {
    guard_allowed "$A/$1" || return 1
    guard_lock || return 1
    guard_now
    G_REC=$G_STATE/$1
    if ! guard_load || [ "$T_TOKEN" != "$2" ] || [ "$T_BOOT" != "$N_BOOT" ] ||
        [ "$T_KERNEL" != "$N_KERNEL" ] || [ "$T_PHASE" != armed ]; then
        guard_unlock; return 0
    fi
    g_delay=$((T_END - N_UP))
    guard_unlock
    [ "$g_delay" -le 0 ] || sleep "$g_delay"
    guard_lock || return 1
    guard_now
    G_REC=$G_STATE/$1
    if guard_load && [ "$T_TOKEN" = "$2" ] && [ "$T_BOOT" = "$N_BOOT" ] &&
        [ "$T_KERNEL" = "$N_KERNEL" ] && [ "$N_UP" -ge "$T_END" ]; then guard_remove; fi
    guard_unlock
}

guard_requested() {
    case "$1" in *panic*|*watchdog*|*wdt*) return 1;; reboot,*|shutdown,*) return 0;; esac
    return 1
}

guard_loader_boot() {
    case "$(getprop ro.boot.bootreason 2>/dev/null) $(getprop sys.boot.reason 2>/dev/null)" in
        *bootloader*|*fastboot*) return 0;;
    esac
    return 1
}

guard_crash_boot() {
    guard_loader_boot && return 1
    guard_requested "$(getprop sys.boot.reason.last 2>/dev/null)" && return 1
    case "$(getprop ro.boot.bootreason 2>/dev/null) $(getprop sys.boot.reason 2>/dev/null)" in
        *panic*|*watchdog*|*wdt*) return 0;;
    esac
    return 1
}

# Only a new boot demonstrably inside the trial's wall-clock window qualifies.
# Clock jumps, long bootloader detours and ambiguous resets preserve user settings.
guard_in_window() {
    awk -v n="$N_WALL" -v u="$N_UP" -v d="$T_WALL" -v t="$G_DURATION" \
        'BEGIN { b=n-u; exit !(d>1577836800 && b>=d-t && b<=d && n>=d-t) }'
}

guard_boot() {
    guard_lock || return 1
    guard_now
    g_schedule=
    for g_name in $(guard_names); do
        G_REC=$G_STATE/$g_name
        [ -d "$G_REC" ] || continue
        if ! guard_load; then guard_remove; continue; fi
        if [ "$T_KERNEL" != "$N_KERNEL" ]; then guard_remove; continue; fi
        if [ "$T_BOOT" = "$N_BOOT" ]; then
            if [ "$T_PHASE" = armed ]; then
                if [ "$N_UP" -ge "$T_END" ]; then guard_remove
                else g_schedule="$g_schedule $g_name"; fi
            fi
        elif [ "$T_PHASE" = boot ]; then
            if guard_loader_boot; then guard_remove
            else guard_save armed; g_schedule="$g_schedule $g_name"; fi
        else
            if guard_crash_boot && guard_in_window && guard_allowed "$A/$g_name"; then
                if [ -f "$G_REC/present" ]; then
                    cp "$G_REC/value" "$A/$g_name"
                else
                    rm -f "$A/$g_name"
                fi
                printf '%s %s\n' "$N_WALL" "$g_name" > "$G_STATE/last_rollback"
            fi
            guard_remove
        fi
    done
    guard_unlock
    for g_name in $g_schedule; do guard_schedule "$A/$g_name"; done
}

guard_status() {
    guard_lock || return 1
    guard_now
    g_active=0; g_queued=0
    for g_name in $(guard_names); do
        G_REC=$G_STATE/$g_name
        guard_load || continue
        if [ "$T_BOOT" = "$N_BOOT" ] && [ "$T_KERNEL" = "$N_KERNEL" ] && [ "$T_PHASE" = armed ]; then
            if [ "$N_UP" -ge "$T_END" ]; then guard_remove
            else g_active=$((g_active+1)); fi
        elif [ "$T_PHASE" = boot ]; then g_queued=$((g_queued+1)); fi
    done
    printf 'trial_active=%s\ntrial_queued=%s\n' "$g_active" "$g_queued"
    [ ! -f "$G_STATE/last_rollback" ] || printf 'trial_rollback=1\n'
    guard_unlock
}

guard_clear() {
    guard_lock || return 1
    for g_name in $(guard_names); do G_REC=$G_STATE/$g_name; guard_remove; done
    rm -f "$G_STATE/last_rollback"
    guard_unlock
}

case "${1:-}" in
    _wait)
        exec </dev/null >/dev/null 2>&1
        . "$G_MODULE/switches.sh"
        # Escape the manager's app freezer; the waiter never polls while idle.
        for g_cg in /sys/fs/cgroup /dev/cpuctl /dev/cpuset /dev/memcg /acct; do
            [ ! -w "$g_cg/cgroup.procs" ] || echo $$ > "$g_cg/cgroup.procs" 2>/dev/null
        done
        guard_settle "$2" "$3"
        ;;
    _clear) . "$G_MODULE/switches.sh"; guard_clear ;;
esac
