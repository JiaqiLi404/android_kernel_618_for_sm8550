#!/usr/bin/env python3
'Device support and compatibility helpers.'
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from fdt import Fdt  # noqa: E402


def text(v):
    if v is None:
        return "-"
    if isinstance(v, bytes):
        return v.rstrip(b"\x00").replace(b"\x00", b",").decode(errors="replace")
    return str(v)


f = Fdt.parse(open(sys.argv[1], "rb").read())
for path, node in f.root.walk():
    name = path.split("/")[-1]
    if len(sys.argv) > 2 and sys.argv[2] not in path:
        continue
    if not any(k in node.props for k in ("iommus", "qcom,iommu-faults", "qcom,iommu-group",
                                         "qcom,secure-cb")):
        continue
    flags = [k for k in ("qcom,secure-context", "qcom,secure-cb", "qcom,iommu-dma")
             if k in node.props]
    print("%-30s label=%-16s faults=%-26s %s" % (
        name[:30], text(node.props.get("label")), text(node.props.get("qcom,iommu-faults")),
        ",".join(flags) or "-"))
