#!/bin/bash
# k618_modinfo515.sh <list> <out>: for each module name in <list> (one a line, '_' or '-'), the
# 13 Pro ROM's own 5.15 .ko (vendor_dlkm, else vendor_boot), its description / author / depends from
# .modinfo, the strings in it that look like /dev /proc /sys paths or Xiaomi names, and whether the
# 18 Pro Max (madrid) has a .ko of that name. For the "5.15 has, 6.18 lacks" table of
# docs/PROVENANCE.en.md. NIKO: the unpacked ROM (x/ and vb/), MADRID_ROOT: madrid's firmware.
. "$(dirname "$(readlink -f "$0")")/env.sh"
N=${NIKO:-$NB/nikohyper}
M=${MADRID_ROOT:-$NB/madrid-OS4.0.24.0.XEOCNXM}
list=$1
out=$2
[ -r "$list" ] && [ -n "$out" ] || { echo "usage: $0 <list> <out>"; exit 2; }
: > "$out"
while read -r m; do
	[ -n "$m" ] || continue
	g=$(echo "$m" | tr _ -)
	f=
	for c in "$N/x/vendor_dlkm_a/lib/modules/$m.ko" "$N/x/vendor_dlkm_a/lib/modules/$g.ko" \
		"$N/vb/rd1/lib/modules/$m.ko" "$N/vb/rd1/lib/modules/$g.ko"; do
		[ -e "$c" ] && { f=$c; break; }
	done
	mad=no
	for c in "$M/vendor_boot/ramdisk0/lib/modules/$m.ko" "$M/vendor_boot/ramdisk0/lib/modules/$g.ko" \
		"$M/vendor_dlkm/lib/modules/$m.ko" "$M/vendor_dlkm/lib/modules/$g.ko" \
		"$M/system_dlkm/lib/modules/$m.ko" "$M/system_dlkm/lib/modules/$g.ko"; do
		[ -e "$c" ] && { mad=yes; break; }
	done
	if [ -z "$f" ]; then
		echo "### $m  (no 5.15 .ko found)  madrid:$mad" >> "$out"
		continue
	fi
	where=vendor_dlkm
	case "$f" in */vb/*) where=vendor_boot ;; esac
	{
		echo "### $m  ($where, $(stat -c %s "$f") B)  madrid:$mad"
		strings -a "$f" | grep -E "^(description|author|depends)=" | cut -c1-160
		strings -a "$f" | grep -E "^/dev/|^/proc/|^/sys/|^mi_|^miui|^xiaomi" | sort -u | head -12 | cut -c1-120
	} >> "$out"
done < "$list"
