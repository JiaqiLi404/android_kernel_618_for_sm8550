#!/bin/bash


. "$(dirname "$(readlink -f "$0")")/env.sh"
OURS=${1:-$K618/boot-test/stage/rd/lib/modules}
THEIRS=$NB/stockmods
python3 -I - "$OURS" "$THEIRS" "$CLANG/llvm-nm" <<'PY'
import os, re, subprocess, sys
ours, theirs, nm = sys.argv[1], sys.argv[2], sys.argv[3]
def norm(n): return n.replace("-", "_").removesuffix(".ko")
stock = {}
for dp, _, fns in os.walk(theirs):
    for f in fns:
        if f.endswith(".ko"):
            stock.setdefault(norm(f), os.path.join(dp, f))
def undef(p):
    out = subprocess.run([nm, "-u", p], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in out.splitlines() if l.strip()}
# vendor-specific calls: what a per-target define tends to switch on
VENDOR = re.compile(r"^(msm_|qcom_|qti_|gh_|ghd_|smcinvoke|get_client_env|qseecom|qtee|scm_|qcom_scm|mem_buf|"
                    r"cnss|ipa_|ipa3|rmnet|synx|hw_fence|llcc|icc_|msm_gpio|ipclite|sps_|glink|qmp_|"
                    r"pdr_|ssr|subsys|socinfo|cmd_db|rpmh|minidump|va_md|sec_|pil_)", re.I)
rows = []
for f in sorted(os.listdir(ours)):
    if not f.endswith(".ko") or f.startswith("k618_"):
        continue
    k = norm(f)
    if k not in stock:
        continue
    a, b = undef(os.path.join(ours, f)), undef(stock[k])
    only_ours = sorted(s for s in a - b if VENDOR.search(s))
    if only_ours:
        rows.append((k, only_ours))
print(f"modules compared: {sum(1 for f in os.listdir(ours) if f.endswith('.ko') and norm(f) in stock)}; "
      f"with vendor calls the phone's own build does not make: {len(rows)}")
for k, s in rows:
    print(f"  {k:28s} {' '.join(s)[:150]}")
PY
