#!/bin/bash
# k618_modgap.sh: which tree each second-stage module of the phone could be built from. Collects the
# lists into $K618/modgap/ (the ROM's vendor_dlkm, what we build, Google's 6.12 common, the 6.12
# techpack repos in vendor-study/mods.git, Xiaomi's 5.15 tree) and runs k618_modgap.py on them with
# the soc tree's own registry. Read-only on every tree. Set ROMX to another extracted ROM if needed.
set -eu
. "$(dirname "$(readlink -f "$0")")/env.sh"
ROMX=${ROMX:-$NB/nikohyper/x}
VS=${VS:-$NB/vendor-study}
TP=${TP:-refs/vendor/op-8850-mods}
G=$K618/modgap
OUTM=${OUTM:-$K618/tpmods}	# the techpack modules k618_tp.sh collected
mkdir -p $G

dump_build() {	# <dir> <out>: every Makefile/Kbuild/.bzl line, prefixed with its path
	(cd "$1" && find . \( -name Makefile -o -name Kbuild -o -name '*.mk' -o -name '*.bzl' -o -name BUILD.bazel \) \
		-type f -printf '%P\n' | while read -r f; do sed "s|^|$f:|" "$f"; done) > "$2"
}

cp $ROMX/vendor_dlkm_a/lib/modules/modules.load $G/rom_modules.load
ls $ROMX/vendor_dlkm_a/lib/modules | grep '\.ko$' | sort > $G/rom_ko.txt
{ ls $S | grep '\.ko$'; ls $OUTM 2>/dev/null | grep '\.ko$'; } | sort -u > $G/our_ko.txt
find $O -name '*.ko' -printf '%f\n' | sort -u > $G/gki_ko.txt

(cd $W/common && find . -type f -name '*.c' -printf '%P\n') > $G/gki_c.txt
(cd $K618/src/fuxi/ && find . -type f -name '*.c' -printf '%P\n') > $G/fuxi_c.txt
dump_build $W/common $G/gki_build.txt
dump_build $K618/src/fuxi/ $G/fuxi_build.txt

git -C $VS/mods.git ls-tree -r --name-only $TP | grep '\.c$' > $G/tp_c.txt
git -C $VS/mods.git grep -I -n '' $TP -- '*Makefile*' '*Kbuild*' '*.mk' '*.bzl' 'BUILD.bazel' > $G/tp_build.txt || true

python3 -I $K618_DIR/k618_modgap.py $G $S $K618/soc-kconfig/.config
