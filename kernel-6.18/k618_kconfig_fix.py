#!/usr/bin/env python3
"""k618_kconfig_fix.py <flattened Kconfig.ext> <kconfig log>: soc-repo's Kconfig was written for 6.12's
kernel Kconfig and defines some of its symbols again; where the two definitions disagree (6.12's GKI selects
QCOM_SCM from QCOM_LMH, Qualcomm's QCOM_LMH depends on it, ...) together they make a dependency loop
and Kconfig gives up. For each loop the log reports, the soc side's edge in it is taken out of the
flattened file -- its `select` if it has one, else the symbol in its `depends on` -- and the kernel's
definition decides. Exit 0 when something was changed (run Kconfig again), 1 when the log has a loop
it cannot break, 2 when the log has none."""

import re
import sys


def blocks(lines):
    """(symbol, line index) of every line that belongs to a config block"""
    cur = None
    for i, line in enumerate(lines):
        m = re.match(r"\s*(?:menu)?config\s+(\w+)\s*$", line)
        if m:
            cur = m.group(1)
            continue
        if re.match(r"\s*(?:menu|endmenu|choice|endchoice|if|endif|source|comment)\b", line):
            cur = None
        yield cur, i


def drop_select(lines, by, target):
    for sym, i in blocks(lines):
        if sym == by and re.match(rf"\s*select\s+{target}\b", lines[i]):
            print(f"k618_kconfig_fix: {by}: '{lines[i].strip()}' taken out")
            lines[i] = "\t# k618: " + lines[i].strip() + " (a loop with 6.12's own Kconfig)"
            return True
    return False


def drop_depend(lines, sym_, on):
    for sym, i in blocks(lines):
        if sym != sym_ or not re.match(rf"\s*depends\s+on\b.*\b{on}\b", lines[i]):
            continue
        old = lines[i]
        new = re.sub(rf"\s*&&\s*{on}\b(?!\s*\|\|)", "", old)
        if new == old:
            new = re.sub(rf"\b{on}\s*&&\s*", "", old)
        if new == old and re.fullmatch(rf"\s*depends\s+on\s+{on}\s*", old):
            new = "\t# k618: " + old.strip()
        if new == old:
            continue
        print(f"k618_kconfig_fix: {sym_}: '{old.strip()}' -> '{new.strip()}'")
        lines[i] = new + ("" if new.lstrip().startswith("#") else "  # k618: a loop with 6.12's own Kconfig")
        return True
    return False


def main(argv):
    path, log = argv[1], argv[2]
    lines = open(path, encoding="utf-8").read().split("\n")
    text = open(log, encoding="utf-8", errors="replace").read()
    loops = re.findall(r"error: recursive dependency detected!\n((?:\t.*\n)+)", text)
    if not loops:
        return 2
    for loop in loops:
        sel = re.findall(r"symbol (\w+) is selected by (\w+)", loop)
        dep = re.findall(r"symbol (\w+) depends on (\w+)", loop)
        if not (any(drop_select(lines, by, t) for t, by in sel) or
                any(drop_depend(lines, s, on) for s, on in dep)):
            print("k618_kconfig_fix: cannot break the loop:\n" + loop, file=sys.stderr)
            return 1
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
