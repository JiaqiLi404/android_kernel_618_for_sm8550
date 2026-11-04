#!/bin/bash
# k618_madrid_inv.sh <mods515> <mods618> <out>: every module of the 18 Pro Max (madrid, 6.18) --
# description, author, dependencies, partition -- and whether the 13 Pro's 5.15 boot and our 6.18
# boot load one of that name. <mods515> / <mods618>: one module name a line, '_' for '-', e.g.
# `awk '{print $1}' /proc/modules | sort -u` of each boot. One '|'-separated line a module:
#   name|partition|in 5.15|in 6.18|description|author|depends
# For docs/PROVENANCE.en.md. MADRID_ROOT: madrid's unpacked firmware.
. "$(dirname "$(readlink -f "$0")")/env.sh"
M=${MADRID_ROOT:-$NB/madrid-OS4.0.24.0.XEOCNXM}
m515=$1
m618=$2
out=$3
[ -r "$m515" ] && [ -r "$m618" ] && [ -n "$out" ] || { echo "usage: $0 <mods515> <mods618> <out>"; exit 2; }
: > "$out"
seen=$(mktemp)
for d in vendor_boot/ramdisk0/lib/modules vendor_dlkm/lib/modules system_dlkm/lib/modules; do
	for f in "$M/$d"/*.ko; do
		u=$(basename "$f" .ko | tr - _)
		grep -qx "$u" "$seen" && continue
		echo "$u" >> "$seen"
		desc=$(modinfo -F description "$f" 2>/dev/null | head -1 | tr '|' '/')
		auth=$(modinfo -F author "$f" 2>/dev/null | head -1 | tr '|' '/')
		dep=$(modinfo -F depends "$f" 2>/dev/null)
		a=no; grep -qx "$u" "$m515" && a=yes
		b=no; grep -qx "$u" "$m618" && b=yes
		echo "$u|${d%%/*}|$a|$b|$desc|$auth|$dep" >> "$out"
	done
done
rm -f "$seen"
echo "$(wc -l < "$out") modules: $(awk -F'|' '{print $3"/"$4}' "$out" | sort | uniq -c | tr '\n' ' ')(5.15/6.18)"
