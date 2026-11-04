#!/bin/bash
# k618_rom_use.sh <work dir> <module>...: who in the phone's ROM uses what these modules make. For
# each module (madrid's 6.18 build, else the 13 Pro's 5.15 one): the strings of its read-only data
# that look like node names (generic words dropped), then -- among the files of the ROM that mention
# any of them (system / system_ext / product / mi_ext: the 18 Pro Max's HyperOS 4; vendor / odm: the
# 13 Pro's own) -- each name that fewer than 20 files mention, with those files. A name in hundreds of
# files is a word, not an interface. For docs/PROVENANCE.en.md; reads only.
# NIKO: the unpacked ROM, MADRID_ROOT: madrid's firmware.
. "$(dirname "$(readlink -f "$0")")/env.sh"
X=${NIKO:-$NB/nikohyper}/x
M=${MADRID_ROOT:-$NB/madrid-OS4.0.24.0.XEOCNXM}
N515=${NIKO:-$NB/nikohyper}
out=$1
shift
[ -n "$out" ] && [ $# -gt 0 ] || { echo "usage: $0 <work dir> <module>..."; exit 2; }
mkdir -p "$out"
STOP='^(enable|disable|debug|status|state|mode|value|version|config|level|count|show|store|reset|start|stop|init|exit|data|info|name|type|size|list|test|open|close|read|write|stats|event|events|module|driver|device|power|thermal|cpu|cpus|gpu|ddr|freq|task|tasks|pid|uid|tid|comm|boost|period|timeout|threshold|limit|max|min|policy|work|queue|thread|threads|lock|mutex|wait|sleep|time|timer|irq|clock|clk|log|logs|trace|dump|memory|mem|page|pages|file|files|path|proc|sys|class|kernel|vendor|system|android|qcom|xiaomi|default|unknown|none|true|false|null|ok|error|fail|failed|success)$'
DIRS="system_a/system/bin system_a/system/lib64 system_a/system/etc system_a/system/framework
system_ext_a/bin system_ext_a/lib64 system_ext_a/etc system_ext_a/framework system_ext_a/priv-app
product_a/etc product_a/framework product_a/priv-app mi_ext_a
vendor_a/bin vendor_a/lib64 vendor_a/etc odm_a/bin odm_a/lib64 odm_a/etc"
for m in "$@"; do
	g=$(echo "$m" | tr _ -)
	f=
	for c in "$M"/{vendor_boot/ramdisk0,vendor_dlkm,system_dlkm}/lib/modules/{$m,$g}.ko \
		"$N515"/x/vendor_dlkm_a/lib/modules/{$m,$g}.ko "$N515"/vb/rd1/lib/modules/{$m,$g}.ko; do
		[ -e "$c" ] && { f=$c; break; }
	done
	[ -n "$f" ] || { echo "== $m: no .ko"; continue; }
	for s in $(readelf -S -W "$f" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^\.rodata/) print $i}' | sort -u); do
		readelf -p "$s" "$f" 2>/dev/null
	done | sed -n 's/^ *\[ *[0-9a-f]*\] *//p' | grep -E '^/?[a-zA-Z][a-zA-Z0-9_./-]{4,48}$' | grep -v -E "$STOP" \
		| grep -v -E '^(include|drivers|kernel|mm|fs|net|arch)/' | sort -u > "$out/$m.str"
	[ -s "$out/$m.str" ] || { echo "== $m: no names"; continue; }
	# shellcheck disable=SC2086  # DIRS is a list
	( cd "$X" && grep -rlaowF -f "$out/$m.str" $DIRS 2>/dev/null ) > "$out/$m.files"
	echo "== $m ($(wc -l < "$out/$m.str") names, $(wc -l < "$out/$m.files") files; $f)"
	while read -r name; do
		hits=$(cd "$X" && xargs -r -a "$out/$m.files" grep -laowF -- "$name" 2>/dev/null)
		n=$(printf '%s\n' "$hits" | grep -c .)
		[ "$n" -gt 0 ] && [ "$n" -lt 20 ] || continue
		echo "   $name: $(printf '%s\n' "$hits" | tr '\n' ' ' | cut -c1-260)"
	done < "$out/$m.str"
done
