# read-only dry run of wifi_patches.py: every anchor found exactly once, edits applied in memory in
# order (k618_kalama.py / k618_tp_fix.py semantics), nothing written
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("wp", sys.argv[1])
wp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wp)
SOC, TP = sys.argv[2], sys.argv[3]

texts = {}


def get(root, rel):
    key = (root, rel)
    if key not in texts:
        texts[key] = open(f"{root}/{rel}", encoding="utf-8", errors="surrogateescape").read()
    return texts[key]


bad = 0
for rel, old, new in wp.KALAMA_SUPPORT_EDITS:
    t = get(SOC, rel)
    n = t.count(old)
    print(f"kalama edit  {rel}: anchor x{n}")
    if n != 1:
        bad += 1
        continue
    texts[(SOC, rel)] = t.replace(old, new)
for rel, anchor, add, where in wp.KALAMA_SUPPORT:
    t = get(SOC, rel)
    n = t.count(anchor)
    print(f"kalama insert {rel}: anchor x{n}")
    if n != 1:
        bad += 1
        continue
    i = t.index(anchor)
    at = i + len(anchor) if where == "after" else i
    texts[(SOC, rel)] = t[:at] + add + t[at:]
for rel, old, new in wp.TP_EDITS:
    t = get(TP, rel)
    n = t.count(old)
    print(f"tp edit {rel.split('/')[-1]}: {old.strip().splitlines()[0][:60]!r} x{n}")
    if n != 1:
        bad += 1
        continue
    texts[(TP, rel)] = t.replace(old, new)

# after the edits: the ops table names only wrappers for the changed ops, no old names left there
c = texts[(TP, "wlan/qcacld-3.0/core/hdd/src/wlan_hdd_cfg80211.c")]
tbl = c[c.index("static struct cfg80211_ops wlan_hdd_cfg80211_ops = {"):]
tbl = tbl[:tbl.index("\n};\n")]
print("k618_ entries in ops table:", tbl.count("k618_"))
p = texts[(SOC, "drivers/pci/controller/pci-msm.c")]
print("pci-msm uses of k618_small_dbi:", p.count("k618_small_dbi(dev)"), "k618_ecam_res:", p.count("k618_ecam_res(dev)"))
print("helper defined before first use:",
      p.index("static bool k618_small_dbi") < p.index("k618_small_dbi(dev)"))
print("BAD anchors:", bad)
