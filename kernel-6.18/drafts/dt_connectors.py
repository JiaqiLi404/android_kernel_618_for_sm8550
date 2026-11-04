#!/usr/bin/env python3
"""dt_connectors.py <dtb>: the display node's "connectors" phandle list, by node name -- the
components msm_drm waits for and binds (msm_smmu_bind sets each SMMU client's host_dev there, and
6.12's msm_smmu_get_smmu() only finds a client whose host_dev is set). Run from the repo root."""
import sys

sys.path.insert(0, "portkit")
from fdt import Fdt  # noqa: E402


def words(v):
    if isinstance(v, bytes):
        return [int.from_bytes(v[i:i + 4], "big") for i in range(0, len(v), 4)]
    if isinstance(v, int):
        return [v]
    return list(v)


f = Fdt.parse(open(sys.argv[1], "rb").read())
ph = {}
for path, node in f.root.walk():
    v = node.props.get("phandle")
    if v is not None:
        for w in words(v):
            ph[w] = path

for path, node in f.root.walk():
    c = node.props.get("connectors")
    if c is None:
        continue
    print(path, "connectors:")
    for w in words(c):
        print("   ", ph.get(w, "phandle 0x%x (not found)" % w).split("/")[-1])
