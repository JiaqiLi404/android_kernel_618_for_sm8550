#!/system/bin/sh
# nuwa-618: the perf HAL's touch and composition boosts on the little cores alone -- the switch
# "hal_little".
#
# Qualcomm's perf HAL (init service perf-hal-2-3) answers the framework's and the display's hints
# with "perf locks" from its table /vendor/etc/perf/perfboostsconfig.xml, read when it starts. The
# vendor partition is the same under k618, and so are four hints that come with touching the
# screen or with what it shows and raise the middle and the prime cluster too: a tap (0x1083: every
# cluster's minimum at its top for 400 ms), scrolls and flings (0x1080) and a drag (0x1087) --
# minimum about 1.0-1.1 GHz on every cluster -- and a "large composition" (0x1097, sent by the
# display's composer some 90 times a minute at 120 Hz): the middle cores at 1500 MHz or more for
# five seconds -- in Douyin all the time, as the composer keeps sending it.
#
# With the switch on those four keep what they do for the little cores and lose the middle's and
# the prime's minimum (0x40800000, 0x40800200 -- "BIG" and "PRIME" in the table's names; LITTLE is
# 0x40800100), and WALT's sched_boost and colocation lines (0x40C00000, 0x40C78000, 0x40C7C000:
# k618 has no WALT, the HAL's writes there go nowhere -- taken out all the same). A hint left with
# nothing is disabled. Starting an app (0x1081) is not touched: without it apps start 38-49 %
# slower (measured). The little cores keep schedutilmium's own floor after a touch.
#
# The table is the phone's own, edited at boot: perfhal_edit reads it and writes a copy, which is
# mounted over it -- in post-fs-data.sh, before the HAL starts and before the first unlock, from a
# tmpfs of this module's (a mount SUSFS marks). Turned from the WebUI the copy comes or goes and the
# HAL is started again to read it.
#
# ROOT (/) can point elsewhere for testing.

PH_R=${ROOT:-}
PH_TABLE=$PH_R/vendor/etc/perf/perfboostsconfig.xml
PH_DIR=$PH_R/dev/nuwa_618_hal
PH_COPY=$PH_DIR/perfboostsconfig.xml
PH_SERVICE=perf-hal-2-3
# the hints and the resources (opcodes) taken out of them, in hex without 0x and leading zeros
PH_IDS="1080 1083 1087 1097"
PH_OPS="40800000 40800200 40c00000 40c78000 40c7c000"

# perfhal_pairs: the "<hint>:<resource>" pairs taken out, one blank between two
perfhal_pairs() {
    ph_p=
    for ph_a in $PH_IDS; do
        for ph_b in $PH_OPS; do
            ph_p="${ph_p:+$ph_p }$ph_a:$ph_b"
        done
    done
    echo "$ph_p"
}

# perfhal_edit <in> <out> [pairs]: the table with the pairs' resources taken out of their hints
# (by default perfhal_pairs'), everything else byte for byte; the number of hints changed on
# stderr ("perfhal: N"). A <Config .../> is the text from "<Config" to the next "/>"; its
# Resources a comma-separated list of opcode and value pairs. Fails on a list of an odd length
# (nothing is written then).
perfhal_edit() {
    awk -v pairs=" ${3-$(perfhal_pairs)} " '
    function hexkey(s) {
        s = tolower(s)
        gsub(/[ \t\r\n]/, "", s)
        sub(/^0x/, "", s)
        sub(/^0+/, "", s)
        return s == "" ? "0" : s
    }
    function trim(s) {
        gsub(/^[ \t\r\n]+/, "", s)
        gsub(/[ \t\r\n]+$/, "", s)
        return s
    }
    function one(block,    m, id, rs, re, list, n, i, items, k, flat, out, changed) {
        if (!match(block, /[ \t\r\n]Id="[^"]*"/))
            return block
        id = substr(block, RSTART, RLENGTH)
        sub(/^[ \t\r\n]Id="/, "", id)
        sub(/"$/, "", id)
        id = hexkey(id)
        if (index(pairs, " " id ":") == 0)
            return block
        rs = index(block, "Resources=\"")
        if (rs == 0)
            return block
        rs += 11
        re = index(substr(block, rs), "\"")
        if (re == 0)
            return block
        list = substr(block, rs, re - 1)
        n = split(list, items, ",")
        k = 0
        for (i = 1; i <= n; i++) {
            items[i] = trim(items[i])
            if (items[i] != "")
                flat[++k] = items[i]
        }
        if (k % 2) {
            bad = 1
            return block
        }
        out = ""
        changed = 0
        for (i = 1; i < k; i += 2) {
            if (index(pairs, " " id ":" hexkey(flat[i]) " ")) {
                changed = 1
                continue
            }
            out = out (out == "" ? "" : ", ") flat[i] ", " flat[i + 1]
        }
        if (!changed)
            return block
        edits++
        if (out != "")
            return substr(block, 1, rs - 1) out substr(block, rs + re - 1)
        if ((m = index(block, "Enable=\"true\"")) == 0)
            return block
        return substr(block, 1, m - 1) "Enable=\"false\"" substr(block, m + 13)
    }
    { text = text $0 "\n" }
    END {
        res = ""
        while ((p = index(text, "<Config")) > 0) {
            c = substr(text, p + 7, 1)
            if (c ~ /[A-Za-z0-9_]/) {
                res = res substr(text, 1, p + 6)
                text = substr(text, p + 7)
                continue
            }
            q = index(substr(text, p), "/>")
            if (q == 0)
                break
            res = res substr(text, 1, p - 1) one(substr(text, p, q + 1))
            text = substr(text, p + q + 1)
        }
        if (bad)
            exit 1
        printf "%s", res text
        printf "perfhal: %d\n", edits > "/dev/stderr"
    }' "$1" > "$2.tmp" && mv "$2.tmp" "$2" && return 0
    rm -f "$2.tmp"
    return 1
}

# perfhal_policy <statement>...: into the running SELinux policy by one call of KernelSU's tool
# (each call copies the whole policy; ksud takes the statements as one argument split at ';').
# Fails where there is no tool or the call failed.
perfhal_policy() {
    [ $# -gt 0 ] || return 0
    [ -x "$PH_R/data/adb/ksud" ] || return 1
    ph_s=
    for ph_x in "$@"; do
        ph_s="$ph_s${ph_s:+;}$ph_x"
    done
    "$PH_R/data/adb/ksud" sepolicy patch "$ph_s" > /dev/null 2>&1
}

# is the module's copy mounted over the table? (the mount point is the fifth field of a line)
perfhal_mounted() {
    grep -q "${PH_TABLE#"$PH_R"} " "$PH_R/proc/self/mountinfo" 2>/dev/null
}

# perfhal_ctx <path>: its SELinux label into ph_c (empty, and fails, where none can be read).
# KernelSU runs post-fs-data.sh in its busybox's ash, the applets first, and busybox's stat prints
# a label with -Z alone (a "C" without it: the copy would get no usable label); toybox's stat (the
# WebUI's mksh) has no -Z: asked second.
perfhal_ctx() {
    ph_c=$(stat -Z -c %C "$1" 2>/dev/null)
    case "$ph_c" in
        *:*:*:*) return 0 ;;
    esac
    ph_c=$(stat -c %C "$1" 2>/dev/null)
    case "$ph_c" in
        *:*:*:*) return 0 ;;
    esac
    ph_c=
    return 1
}

# perfhal_type: the table's SELinux type into ph_type (fails where its label cannot be read)
perfhal_type() {
    perfhal_ctx "$PH_TABLE" || return 1
    ph_ctx=$ph_c
    ph_type=${ph_ctx#*:}
    ph_type=${ph_type#*:}
    ph_type=${ph_type%%:*}
}

# perfhal_label: the copy gets the table's SELinux label -- a file on a tmpfs can have it once the
# running policy lets the type be there (asked for here unless PH_ASKED says post-fs-data.sh's one
# call did). Fails where the copy does not have it then; without SELinux nothing is done.
perfhal_label() {
    if ! perfhal_type; then
        [ ! -e "$PH_R/sys/fs/selinux/enforce" ]
        return
    fi
    [ "$PH_ASKED" = "$ph_type" ] || perfhal_policy "allow $ph_type tmpfs filesystem associate"
    chcon "$ph_ctx" "$PH_COPY" 2>/dev/null
    perfhal_ctx "$PH_COPY" && [ "$ph_c" = "$ph_ctx" ]
}

# perfhal_on: the edited copy over the table (its count of edited hints next to it). Not mounted
# where the copy cannot have the table's label: the HAL could not read it. Fails where nothing was
# mounted.
perfhal_on() {
    [ -s "$PH_TABLE" ] || return 1
    ! perfhal_mounted || return 0
    mkdir -p "$PH_DIR" 2>/dev/null || return 1
    perfhal_edit "$PH_TABLE" "$PH_COPY" 2> "$PH_DIR/edits" || return 1
    chmod 644 "$PH_COPY"
    perfhal_label || {
        rm -f "$PH_COPY"
        return 1
    }
    mount --bind "$PH_COPY" "$PH_TABLE" 2>/dev/null || {
        rm -f "$PH_COPY"
        return 1
    }
}

# perfhal_off: the table the vendor's again
perfhal_off() {
    ! perfhal_mounted || umount -l "$PH_TABLE" 2>/dev/null || return 1
    rm -f "$PH_COPY" "$PH_DIR/edits"
}

# perfhal_apply [restart]: as the switch stands. With "restart" the HAL, where it runs, is started
# again to read the table.
perfhal_apply() {
    if sw_on hal_little; then
        perfhal_on
    else
        perfhal_off
    fi
    ph_rc=$?
    if [ "$1" = restart ] && [ "$(getprop "init.svc.$PH_SERVICE")" = running ]; then
        stop "$PH_SERVICE"
        start "$PH_SERVICE"
    fi
    return $ph_rc
}

# perfhal_status: "<state> <edits>" for the WebUI -- on (the copy over the table, labelled as it),
# unlabelled (mounted with a tmpfs label: the HAL cannot read it), off, or none (no such table)
perfhal_status() {
    if [ ! -s "$PH_TABLE" ] && ! perfhal_mounted; then
        echo none
        return
    fi
    if ! perfhal_mounted; then
        echo off
        return
    fi
    ph_e=$(sed -n 's/^perfhal: //p' "$PH_DIR/edits" 2>/dev/null)
    if perfhal_ctx "$PH_TABLE" && case "$ph_c" in *:tmpfs:*) true ;; *) false ;; esac; then
        echo "unlabelled $ph_e"
    else
        echo "on $ph_e"
    fi
}
