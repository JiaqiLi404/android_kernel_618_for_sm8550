#!/usr/bin/env python3
"""k618_uev_cmp.py <dev listing> <rc dir>: the device nodes whose owner / mode on the phone differ
from what the 18 Pro Max's own ueventd rules would give them.

The ROM's system side comes from the 18 Pro Max (madrid) and expects its device nodes as madrid's
vendor and odm ueventd.rc set them; this phone keeps its own vendor and odm, whose rules predate
madrid's modules. A node madrid's rules open up and ours leave at ueventd's default (0600 root
root) is one the system side cannot use.

<dev listing>: one line per node, "path mode user group major:minor", from the running phone:
  adb shell 'su -c "find /dev -type c -o -type b | while read f; do stat -c \\"%n %a %U %G %t:%T\\" \\"\\$f\\"; done"' > dev.txt
<rc dir>: phone_system.rc, phone_vendor.rc, phone_odm.rc (adb exec-out cat /system/etc/ueventd.rc ...)
  and madrid_system.rc, madrid_vendor.rc, madrid_odm.rc (madrid's system/etc, and vendor/odm etc/ueventd.rc).

Matching as Android's ueventd does it (devices.cpp, Permissions::Match): a '*' only at the end is a
prefix match, a '*' anywhere else fnmatch with FNM_PATHNAME, otherwise the whole path; files are read
system, vendor, odm and the last matching rule wins; no match = 0600 root root. Each line says what
the phone's own rules give: "match" means ueventd set the node as found; anything else was changed
after ueventd (an init.rc chmod / chown), so the difference to madrid is not ueventd's.
Only owner and mode are compared -- nothing here reads or compares SELinux labels.
"""
import os
import re
import sys


def rules(rc_dir, names):
    out = []
    for n in names:
        for line in open(os.path.join(rc_dir, n), encoding="utf-8", errors="replace"):
            f = line.split("#", 1)[0].split()
            if len(f) == 4 and f[0].startswith("/dev/"):
                out.append((f[0], f[1].lstrip("0") or "0", f[2], f[3], n))
    return out


def match(rule, path):
    if "*" in rule:
        if rule.index("*") == len(rule) - 1:
            return path.startswith(rule[:-1])
        rx = "".join("[^/]*" if c == "*" else "[^/]" if c == "?" else re.escape(c) for c in rule)
        return re.fullmatch(rx, path) is not None
    return rule == path


def perm(rs, path):
    for r in reversed(rs):
        if match(r[0], path):
            return r[1:4], r
    return ("600", "root", "root"), None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dev_list, rc_dir = sys.argv[1], sys.argv[2]
    phone = rules(rc_dir, ["phone_system.rc", "phone_vendor.rc", "phone_odm.rc"])
    madrid = rules(rc_dir, ["madrid_system.rc", "madrid_vendor.rc", "madrid_odm.rc"])
    n = bad = 0
    for line in open(dev_list, encoding="utf-8", errors="replace"):
        f = line.split()
        if len(f) != 5 or not f[0].startswith("/dev/"):
            continue
        path, actual = f[0], tuple(f[1:4])
        n += 1
        exp_p, _ = perm(phone, path)
        exp_m, rm = perm(madrid, path)
        if exp_m == actual:
            continue
        bad += 1
        print("%-44s now %s %s:%s  madrid %s %s:%s (%s)  phone rules: %s" % (
            path, actual[0], actual[1], actual[2], exp_m[0], exp_m[1], exp_m[2],
            "%s %s" % (rm[4], rm[0]) if rm else "default",
            "match" if exp_p == actual else "%s %s:%s, changed after ueventd" % exp_p))
    print("%d nodes, %d differ from madrid's rules" % (n, bad))


if __name__ == "__main__":
    main()
