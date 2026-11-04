#!/usr/bin/env python3
"""k618_mi_brightness_check.py [--k618 DIR] [--tmp DIR]: offline checks of the brightness drafts.

Writes only into --tmp (default: a new temp dir); reads the tree, never builds or edits it.
  1. every EDITS `old` of k618_mi_brightness_edits.py occurs exactly once in the tree's dsi_panel.c,
     and a second pass over the edited text changes nothing (k618_tp_fix.py's re-run rule);
  2. control: the tree's own mi_disp_k618.c and dsi/dsi_panel.c pass `clang -fsyntax-only` with the
     exact command lines of the last techpack build (msm/.mi_disp_k618.o.cmd, msm/dsi/.dsi_panel.o.cmd:
     same -I/-D/-include/-W flags, kCFI, -Werror; only -Wp,-MMD / -c / -o / the source are replaced);
  3. a merged mi_disp_k618.c -- the repo's k618_mi_disp.c with k618_set_feature(),
     k618_mi_disp_panel_on() and k618_mi_disp_bl() taken out and the whole draft put where
     k618_set_feature() was -- and the edited dsi_panel.c pass the same, against a mi_disp_k618.h
     with HEADER_PROTOTYPES added.
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import k618_mi_brightness_edits as ED  # noqa: E402


def cut_function(text, start):
    """remove the definition beginning with `start` (up to its closing brace at column 0)"""
    i = text.index(start)
    j = text.index("\n}\n", i) + 3
    return text[:i], text[j:]


def compile_cmd(cmd_file, src):
    line = open(cmd_file).readline()
    cmd = line.split(":=", 1)[1].split(";")[0]
    args = shlex.split(cmd)
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a.startswith(("-Wp,-MMD", "-Wp,-MD")) or a in ("-MD", "-MMD", "-c"):
            continue
        if a in ("-o", "-MF"):
            skip = True
            continue
        if a.endswith(".c"):
            continue
        out.append(a)
    return out + ["-fsyntax-only", src]


def run(args, cwd, path_dir, label):
    """returns (ok, number of warnings)"""
    env = dict(os.environ, PATH=path_dir + ":" + os.environ.get("PATH", ""))
    p = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)
    msg = (p.stdout + p.stderr).strip()
    warns = sum(1 for m in msg.splitlines() if ": warning: " in m)
    print("%-34s %s, %d warning(s)" % (label, "OK" if p.returncode == 0 else
                                       "FAILED (%d)" % p.returncode, warns))
    if msg:
        print("\n".join("    " + m for m in msg.splitlines()[:60]))
    return p.returncode == 0, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--k618", default=os.environ.get("K618", os.path.expanduser("~/xiaomi13-k618-build/k618")))
    ap.add_argument("--tmp", default=None)
    a = ap.parse_args()
    dd = os.path.join(a.k618, "vendor/qcom/opensource/display-drivers")
    msm = os.path.join(dd, "msm")
    clang_dir = os.path.join(a.k618, "ws/prebuilts/clang/host/linux-x86/clang-r584948c/bin")
    tmp = a.tmp or tempfile.mkdtemp(prefix="k618_bright_")
    os.makedirs(os.path.join(tmp, "dsi"), exist_ok=True)
    ok = True

    # 1. anchors + idempotency
    panel_c = open(os.path.join(msm, "dsi/dsi_panel.c"), encoding="utf-8").read()
    text = panel_c
    for n, (rel, old, new) in enumerate(ED.EDITS, 1):
        cnt = panel_c.count(old)
        text, what = ED.apply(text, old, new)
        print("edit %d: old found %d time(s) in the tree, %s" % (n, cnt, what))
        ok &= cnt == 1 and what == "applied"
    again = text
    for rel, old, new in ED.EDITS:
        again, what = ED.apply(again, old, new)
        ok &= what == "already"
    print("second pass:", "no change" if again == text else "CHANGED")
    ok &= again == text

    # header with the new prototypes, next to both scratch sources (quote includes look there first)
    hdr = open(os.path.join(REPO, "k618_mi_disp.h"), encoding="utf-8").read()
    hdr = hdr.replace("#endif /* _MI_DISP_K618_H_ */",
                      ED.HEADER_PROTOTYPES + "\n#endif /* _MI_DISP_K618_H_ */")
    for d in (tmp, os.path.join(tmp, "dsi")):
        open(os.path.join(d, "mi_disp_k618.h"), "w").write(hdr)
    open(os.path.join(tmp, "dsi/dsi_panel.c"), "w").write(text)

    # merged mi_disp_k618.c
    src = open(os.path.join(REPO, "k618_mi_disp.c"), encoding="utf-8").read()
    draft = open(os.path.join(HERE, "k618_mi_brightness.c"), encoding="utf-8").read()
    head, tail = cut_function(src, "static int k618_set_feature(int disp_id, u32 feature_id, int val)\n")
    merged = head + draft + "\n" + tail
    for sig in ("void k618_mi_disp_panel_on(struct dsi_panel *panel)\n{\n\tstruct k618_mi *mi = g_mi;\n\tstruct dsi_display *d;\n",
                "void k618_mi_disp_bl(struct dsi_panel *panel, u32 bl_lvl)\n{\n\tstruct dsi_display *d;\n"):
        h, t = cut_function(merged, sig)
        merged = h + t
    open(os.path.join(tmp, "mi_disp_k618.c"), "w").write(merged)

    # 2. control, 3. drafts. The build line carries -Wno-error, so the draft is also held to zero
    # warnings (-Werror appended last), and the edited dsi_panel.c to the control's warning count.
    mcmd = os.path.join(msm, ".mi_disp_k618.o.cmd")
    pcmd = os.path.join(msm, "dsi/.dsi_panel.o.cmd")
    r, _ = run(compile_cmd(mcmd, os.path.join(msm, "mi_disp_k618.c")), dd, clang_dir,
               "control: tree mi_disp_k618.c")
    ok &= r
    r, wp0 = run(compile_cmd(pcmd, os.path.join(msm, "dsi/dsi_panel.c")), dd, clang_dir,
                 "control: tree dsi_panel.c")
    ok &= r
    r, _ = run(compile_cmd(mcmd, os.path.join(tmp, "mi_disp_k618.c")) + ["-Werror"], dd, clang_dir,
               "merged mi_disp_k618.c, -Werror")
    ok &= r
    r, wp1 = run(compile_cmd(pcmd, os.path.join(tmp, "dsi/dsi_panel.c")), dd, clang_dir,
                 "edited dsi_panel.c")
    ok &= r and wp1 == wp0
    print("dsi_panel.c warnings: control %d, edited %d" % (wp0, wp1))
    print("scratch files in", tmp)
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
