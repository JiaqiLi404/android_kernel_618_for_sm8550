#!/bin/bash


set -euo pipefail

[ $# -ge 2 ] || { sed -n '2,16p' "$0"; exit 2; }
img=$(readlink -f "$1")
out=$(readlink -m "$2")
here=$(dirname "$(readlink -f "$0")")
mkb=${MKB:-${BUILD_ROOT:-$HOME/xiaomi13-k618-build}/home/mkbootimg}
[ -f "$img" ] || { echo "no such image: $img" >&2; exit 2; }
[ -f "$mkb/unpack_bootimg.py" ] || { echo "no unpack_bootimg.py in $mkb (set MKB)" >&2; exit 2; }

w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
python3 -I "$mkb/unpack_bootimg.py" --boot_img "$img" --out "$w/img" > "$w/info.txt"
sed -n 's/^command line args: //p' "$w/info.txt" | head -1 > "$w/cmdline.txt"
[ -s "$w/img/kernel" ] || { echo "no kernel in $img" >&2; exit 1; }
[ -s "$w/img/ramdisk" ] || { echo "no ramdisk in $img" >&2; exit 1; }
# lz4, legacy (02214c18, what the kernel's build writes) or frame (04224d18) format: it goes into
# the zip as it is, and magiskboot leaves an already-compressed ramdisk alone
case $(head -c4 "$w/img/ramdisk" | od -An -tx1 | tr -d '[:space:]') in
	02214c18 | 04224d18) ;;
	*) echo "::error::$(basename "$img")'s ramdisk is not lz4; see this script's head" >&2; exit 1;;
esac

# the install script sets the whole command line, so its list must be the image header's, item for
# item and in order -- then the flashed header carries exactly the image's
tr ' ' '\n' < "$w/cmdline.txt" | sed '/^$/d' > "$w/want.txt"
awk '/^k618_cmdline="$/ {on = 1; next} on && /^";$/ {on = 0} on' "$here/anykernel-618.sh" | sed '/^$/d' > "$w/have.txt"
if grep -q -e '|' -e '&' -e '\\' -e '"' "$w/have.txt"; then
	echo "::error::an item of k618_cmdline carries | & \\ or \", which the install script's sed cannot" >&2
	exit 1
fi
if ! diff -u "$w/want.txt" "$w/have.txt" > "$w/cmddiff.txt"; then
	echo "::error::$(basename "$img")'s command line and anykernel-618.sh's k618_cmdline disagree:" >&2
	sed -n '4,$p' "$w/cmddiff.txt" | sed 's/^-/image only: /; s/^+/script only: /' >&2
	exit 1
fi
echo "command line: $(wc -l < "$w/want.txt") items, the image's and anykernel-618.sh's the same, in order"

AK3_INSTALL="$here/anykernel-618.sh" AK3_EXTRA="$w/img/ramdisk=ramdisk-new.cpio.lz4" bash "$here/pack.sh" "$w/img/kernel" "$out" ${3:+"$3"}
echo "from $(basename "$img"): kernel $(stat -c %s "$w/img/kernel") B (sha256 $(sha256sum "$w/img/kernel" | cut -c1-16)), ramdisk $(stat -c %s "$w/img/ramdisk") B (lz4)"
unzip -l "$out" | grep -E "Image|ramdisk-new" | sed 's/^/  /'
