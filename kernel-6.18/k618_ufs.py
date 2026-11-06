#!/usr/bin/env python3
"""k618_ufs.py <soc tree> [--check]: the Xiaomi UFS nodes of the stock ufs-qcom that this ROM reads,
on soc-repo's ufs-qcom (ufs_monitor for every device; ufscld for the KIOXIA parts stock handles).

kernel-6.18/k618_ufs_mi.c (reverse-engineered from the 18 Pro Max's 6.18 ufs-qcom.ko and the 13 Pro's
5.15 ufs_qcom.ko; what it does and leaves out is in its header) is copied to
drivers/ufs/host/ufs-qcom-mi.c and included by ufs-qcom.c, so modules.bzl and the generated Kbuild
stay as they are; ufs_qcom_probe() starts it after the generic probe, ufs_qcom_remove() stops it
before ufshcd_remove(). Idempotent: the copy is written only when it differs, each edit only when
its text is missing. --check reports what would change and writes nothing. Prints what it did."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "k618_ufs_mi.c")
DST = "drivers/ufs/host/ufs-qcom-mi.c"
UFS = "drivers/ufs/host/ufs-qcom.c"

# (anchor, text to add, before|after)
EDITS = [
    ("static bool is_bootdevice_ufs = true;\n",
     "/* k618: the Xiaomi sysfs nodes (ufs_monitor, ufscld) the ROM reads; see that file */\n"
     "#include \"ufs-qcom-mi.c\"\n\n",
     "before"),
    ("\tufs_qcom_register_hooks();\n\treturn err;\n}\n",
     None,  # replaced below: the probe tail gains one call
     "replace"),
    ("\tufshcd_remove(hba);\n\tplatform_device_msi_free_irqs_all(hba->dev);\n",
     "\tk618_ufs_mi_remove(hba);\n",
     "before"),
]

PROBE_OLD = "\tufs_qcom_register_hooks();\n\treturn err;\n}\n"
PROBE_NEW = ("\tufs_qcom_register_hooks();\n"
             "\t/* k618: ufs_monitor now, ufscld once the async scan has found the device */\n"
             "\tk618_ufs_mi_probe(platform_get_drvdata(pdev));\n"
             "\treturn err;\n}\n")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    soc = argv[1].rstrip("/")
    check = "--check" in argv[2:]
    rc = 0

    src = open(SRC, encoding="utf-8").read()
    dst = os.path.join(soc, DST)
    old = open(dst, encoding="utf-8").read() if os.path.exists(dst) else None
    if old != src:
        print(f"k618_ufs: {DST}: {'would be ' if check else ''}{'updated' if old else 'created'}")
        if not check:
            open(dst, "w", encoding="utf-8", newline="").write(src)

    path = os.path.join(soc, UFS)
    text = open(path, encoding="utf-8").read()
    new = text
    for anchor, add, where in EDITS:
        if where == "replace":
            if PROBE_NEW in new:
                continue
            if new.count(PROBE_OLD) != 1:
                print(f"k618_ufs: {UFS}: probe tail not found once", file=sys.stderr)
                rc = 1
                continue
            new = new.replace(PROBE_OLD, PROBE_NEW)
            print(f"k618_ufs: {UFS}: ufs_qcom_probe starts the nodes")
            continue
        block = add + anchor if where == "before" else anchor + add
        if block in new:
            continue
        if new.count(anchor) != 1:
            print(f"k618_ufs: {UFS}: anchor not found once: {anchor.strip()!r}", file=sys.stderr)
            rc = 1
            continue
        new = new.replace(anchor, block)
        print(f"k618_ufs: {UFS}: added before {anchor.strip()!r}")
    if new != text and not check:
        open(path, "w", encoding="utf-8", newline="").write(new)
    elif new != text:
        print(f"k618_ufs: {UFS}: would be changed")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
