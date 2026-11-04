#!/bin/bash
# k618_gpt_save.sh: a copy of every UFS disk's partition table, off the phone, before a test that
# lets the boot control HAL rewrite one.
#
# The A/B slot flags ("this slot booted fine", "try this one next") are bits in the GPT's own
# partition entries, so the HAL marking a slot rewrites the table. That is what the phone does on
# every normal boot, but a 6.18 test kernel doing it is new, so both copies of each disk's table --
# the primary at the start and the backup at the end -- are saved first. Read-only on the phone.
#
# Results: $OUT/gpt-<disk>-{head,tail}.bin + parts.txt + SHA256SUMS.txt, outside the repo (a GPT
# carries the phone's own partition layout). Restoring one is a decision of its own; the files are
# here so that it can be made.
set -u
A=${ADB:-adb}
PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$PROJECT_DIR/backup/gpt-$(date +%Y%m%d-%H%M)}
mkdir -p "$OUT" || exit 1
TMP=/data/local/tmp/k618gpt
$A shell "su -c 'rm -rf $TMP; mkdir -p $TMP'" || exit 1
# every whole disk (sda, sdb, ... -- no trailing digits), its size in 512-byte sectors.
# `sd?` and not `sd[a-z]`: the phone's shell leaves the bracket form unexpanded here.
DISKS=$($A shell "su -c 'for d in /sys/class/block/sd?; do echo \${d##*/} \$(cat \$d/size); done'" | tr -d '\r')
[ -n "$DISKS" ] || { echo "k618_gpt_save: no disks found"; exit 1; }
# no pipeline: a loop in one would run in a subshell, and a single disk's failure would end it
echo "$DISKS" > "$OUT/sizes.txt"
while read -r d sz; do
	case "$d$sz" in "") continue;; esac
	[ -n "$d" ] && [ -n "$sz" ] || continue
	# 1 MB off each end, not the 34 + 33 sectors a GPT takes: these disks have 4096-byte logical
	# sectors (the size in /sys is in 512-byte units either way), so the table is six 4 KB sectors
	# at each end, and a megabyte covers both copies whatever the sector size turns out to be.
	# </dev/null: adb reads stdin, and this loop's stdin is the list it is walking
	$A shell "su -c 'dd if=/dev/block/$d of=$TMP/gpt-$d-head.bin bs=512 count=2048 2>/dev/null;
		dd if=/dev/block/$d of=$TMP/gpt-$d-tail.bin bs=512 skip=\$(( $sz - 2048 )) count=2048 2>/dev/null'" < /dev/null
	echo "  $d: $sz sectors, dd exit $?"
done < "$OUT/sizes.txt"
$A shell "su -c 'cat /proc/partitions; echo; ls -l /dev/block/by-name/ | sed \"s|.*/||\"; echo; chmod -R 666 $TMP/*'" > "$OUT/parts.txt" 2>&1
$A pull $TMP "$OUT/bin" > /dev/null 2>&1 || { echo "k618_gpt_save: pull failed"; exit 1; }
mv "$OUT/bin"/* "$OUT"/ 2>/dev/null; rmdir "$OUT/bin" 2>/dev/null
$A shell "su -c 'rm -rf $TMP'"
( cd "$OUT" && sha256sum gpt-*.bin > SHA256SUMS.txt )
echo "k618_gpt_save: $(ls "$OUT"/gpt-*.bin | wc -l) files in $OUT"
ls -la "$OUT" | tail -n +2 | awk '{print "  " $5, $NF}'
