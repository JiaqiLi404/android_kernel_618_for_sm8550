#!/bin/bash


set -u
A=${ADB:-adb}
PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$PROJECT_DIR/logs/diagnostics}
TMP=$(mktemp -d)
trap 'rm -rf -- "$TMP"' EXIT
TAG=${1:?usage: k618_ph_read.sh <test tag>}
mkdir -p "$OUT"
$A shell 'su -c "dd if=/dev/block/by-name/rawdump of=/data/local/tmp/rd.bin bs=1M count='"${RD_MB:-140}"' 2>/dev/null; chmod 666 /data/local/tmp/rd.bin; rm -rf /data/local/tmp/pstore; mkdir /data/local/tmp/pstore; cp /sys/fs/pstore/* /data/local/tmp/pstore/ 2>/dev/null; chmod -R 755 /data/local/tmp/pstore; ls -la /sys/fs/pstore; cat /sys/bootinfo/powerup_reason /sys/bootinfo/poweroff_reason 2>/dev/null; dmesg | grep -m3 -E \"version timestamp|FSCK version from\"; uname -r; cat /proc/uptime"' \
	| tee "$OUT/$TAG-reasons.txt"
$A pull /data/local/tmp/rd.bin "$TMP/$TAG-rd.bin"
$A pull /data/local/tmp/pstore "$TMP/$TAG-pstore" && cp -r "$TMP/$TAG-pstore" "$OUT/$TAG-pstore"
python3 -I "$PROJECT_DIR/kernel-6.18/k618_rdread.py" "$TMP/$TAG-rd.bin" "$OUT/$TAG-kernel.log"
$A shell 'su -c "rm -rf /data/local/tmp/rd.bin /data/local/tmp/pstore"'
