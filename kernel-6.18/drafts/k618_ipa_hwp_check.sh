#!/bin/bash
# k618_ipa_hwp_check.sh: check drafts/k618_ipa_hwp_edits.py without touching $K618.
#  1. copies dataipa (as k618_tp.sh last built it) to /tmp/k618_ipa_hwp and applies the edits
#     there (EDITS + OPTIONAL_EDITS), twice -- the second run must apply 0;
#  2. recompiles the five touched objects with the techpack build's own command lines (the
#     savedcmd lines of the .cmd files k618_tp.sh left next to them), paths moved to the copy;
#  3. compares: ipa_5_5_mem_part in the new ipa_utils.o vs the phone's own ipam.ko (must be equal
#     word for word) and vs our current tpmods/ipam.ko; the 0xABCDEF01 store in _ipa_init_sram_v3
#     (present before, gone after); the QMI FnR window immediates (stock 0x340 / 0xb380).
# Light: five clang runs, no make, no link. Run: wsl -e bash -lc 'bash <this file>'
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../env.sh"
export PATH=$CLANG:$PATH
SRC=$K618/vendor/qcom/opensource/dataipa
MSM=drivers/platform/msm
STOCK=${STOCK:-$NB/nikohyper/x/vendor_dlkm_a/lib/modules/ipam.ko}
OURS=${OURS:-$K618/tpmods/ipam.ko}
T=/tmp/k618_ipa_hwp
rm -rf "$T"; mkdir -p "$T/opensource"
cp -a "$SRC" "$T/opensource/"
D=$T/opensource/dataipa

echo "== edits"
python3 -I "$HERE/k618_ipa_hwp_edits.py" "$T/opensource" --optional
python3 -I "$HERE/k618_ipa_hwp_edits.py" "$T/opensource" --optional | tail -1

echo "== compile"
for o in ipa/ipa_v3/ipa_utils.o ipa/ipa_v3/ipa.o ipa/ipa_v3/ipa_qmi_service.o gsi/gsi.o ipa/ipa_v3/ipa_dp.o; do
	cmdf=$SRC/$MSM/$(dirname $o)/.$(basename $o).cmd
	cmd=$(head -1 "$cmdf" | sed 's/^savedcmd_[^ ]* := //')
	cmd=${cmd//"$K618/ws/common/../../vendor/qcom/opensource/dataipa"/"$D"}
	cmd=${cmd//"$K618/vendor/qcom/opensource/dataipa"/"$D"}
	cmd=${cmd//'$$'/'$'}
	rm -f "$D/$MSM/$o"
	( cd "$D/$MSM" && bash -c "$cmd" ) 2> "$T/$(basename $o).log" || { echo "$o: FAILED"; tail -20 "$T/$(basename $o).log"; exit 1; }
	echo "$o: ok ($(grep -c 'warning:' "$T/$(basename $o).log" || true) warnings)"
done

echo "== ipa_5_5_mem_part (stock = phone's ipam.ko, new = patched ipa_utils.o, cur = tpmods/ipam.ko)"
FIELDS=$T/fields.txt
n=$(grep -n '^struct ipa3_mem_partition {' "$D/$MSM/ipa/ipa_v3/ipa_i.h" | cut -d: -f1)
sed -n "${n},/^};/p" "$D/$MSM/ipa/ipa_v3/ipa_i.h" | grep -oE 'u32 [a-z0-9_]+' | sed 's/u32 //' > "$FIELDS"
cat > "$T/dump.py" <<'EOF'
import struct, subprocess, sys
clang, fields_file, sym = sys.argv[1], sys.argv[2], sys.argv[3]
fields = open(fields_file).read().split()
def dump(path):
    syms = subprocess.check_output([clang + "/llvm-readelf", "-sW", path]).decode()
    ent = None
    for l in syms.splitlines():
        p = l.split()
        if len(p) >= 8 and p[7] == sym:
            ent = (int(p[1], 16), p[6])
    if ent is None:
        sys.exit("%s: no %s" % (path, sym))
    secs = subprocess.check_output([clang + "/llvm-readelf", "-SW", path]).decode()
    off = None
    for l in secs.splitlines():
        l = l.replace("[ ", "[")
        p = l.split()
        if p and p[0].startswith("[") and p[0].strip("[]") == ent[1]:
            off = int(p[4], 16)
    data = open(path, "rb").read()[off + ent[0]: off + ent[0] + 4 * len(fields)]
    return struct.unpack("<%dI" % len(fields), data)
stock, new, cur = (dump(p) for p in sys.argv[4:7])
bad = 0
for f, s, n, c in zip(fields, stock, new, cur):
    if s != n:
        bad += 1
    if s != n or s != c:
        print("%-28s stock 0x%-6x new 0x%-6x cur 0x%-6x%s" % (f, s, n, c, "  <-- new != stock" if s != n else ""))
print("fields: %d, new != stock: %d, cur != stock: %d" % (len(fields), bad, sum(s != c for s, c in zip(stock, cur))))
sys.exit(1 if bad else 0)
EOF
python3 -I "$T/dump.py" "$CLANG" "$FIELDS" ipa_5_5_mem_part "$STOCK" "$D/$MSM/ipa/ipa_v3/ipa_utils.o" "$OURS"

echo "== uC descriptor RAM marker in _ipa_init_sram_v3 (mov/movk 0xef01 / 0xabcd)"
for f in "$SRC/$MSM/ipa/ipa_v3/ipa.o" "$D/$MSM/ipa/ipa_v3/ipa.o"; do
	c=$(llvm-objdump -d --no-show-raw-insn --disassemble-symbols=_ipa_init_sram_v3 "$f" | grep -ciE '#0xef01|#0xabcd' || true)
	echo "$f: $c"
done

echo "== QMI FnR window immediates (stock: 0x340 size, 0xb380 = start 128 / end 179)"
for f in "$SRC/$MSM/ipa/ipa_v3/ipa_qmi_service.o" "$D/$MSM/ipa/ipa_v3/ipa_qmi_service.o"; do
	echo "$f: $(llvm-objdump -d --no-show-raw-insn "$f" | grep -oE '#0x(340|b380|bf0|fe40)\b' | sort | uniq -c | tr '\n' ' ')"
done
echo "== done; copy left in $T"
