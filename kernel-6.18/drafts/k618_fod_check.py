#!/usr/bin/env python3
"""k618_fod_check.py [--k618 DIR] [--tmp DIR]: offline checks of the FOD/LHBM + doze drafts. Writes
only into --tmp; reads the tree and the staged touch sources, never builds or edits them.

  1. k618_fod_disp_edits self-check + every FOD_CMD_SETS DT property is present on the panel node.
  2. touch: stage the zeus sources as k618_touch.sh does (touch_fix), apply k618_fod_touch_edits --
     each `old` occurs once, a second pass changes nothing -- then clang -fsyntax-only xiaomi_touch.c
     and fts.c with the exact command lines of the last touch build (.<obj>.o.cmd), control (tree) and
     edited, requiring the edited warning count not to exceed the control's.
  3. display: splice drafts/k618_fod_disp.c into a copy of the tree's k618_mi_disp.c (its
     k618_set_feature replaced by the draft's, the four struct fields + the doze DT reads added), with
     a small -include that provides the FOD/doze DSI_CMD_SET_MI_* enum constants the tree's dsi_defs.h
     does not have yet (they are added by k618_fod_disp_edits at build time). clang -fsyntax-only with
     the recorded .mi_disp_k618.o.cmd + -Werror, i.e. the hand-written C parses and is type-consistent.
     Not a full build.
"""
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import k618_fod_disp_edits as DE   # noqa: E402
import k618_fod_touch_edits as TE  # noqa: E402


def compile_cmd(cmd_file, src, extra=()):
    line = open(cmd_file).readline()
    cmd = line.split(":=", 1)[1].split(";")[0]
    args, out, skip = shlex.split(cmd), [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a.startswith(("-Wp,-MMD", "-Wp,-MD")) or a in ("-MD", "-MMD", "-c"):
            continue
        if a in ("-o", "-MF"):
            skip = True
            continue
        if a.endswith(".c") or a.endswith(".o"):
            continue
        out.append(a)
    return out + list(extra) + ["-fsyntax-only", src]


def run(args, cwd, path_dir, label):
    env = dict(os.environ, PATH=path_dir + ":" + os.environ.get("PATH", ""))
    p = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)
    msg = (p.stdout + p.stderr).strip()
    warns = sum(1 for m in msg.splitlines() if ": warning: " in m)
    print("%-36s %s, %d warning(s)" % (label, "OK" if p.returncode == 0 else
                                       "FAILED(%d)" % p.returncode, warns))
    if msg:
        print("\n".join("    " + m for m in msg.splitlines()[:50]))
    return p.returncode == 0, warns


def cut_function(text, start):
    i = text.index(start)
    j = text.index("\n}\n", i) + 3
    return text[:i], text[j:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--k618", default=os.environ.get("K618", os.path.expanduser("~/xiaomi13-k618-build/k618")))
    ap.add_argument("--tmp", default=None)
    a = ap.parse_args()
    tmp = a.tmp or tempfile.mkdtemp(prefix="k618_fod_")
    os.makedirs(tmp, exist_ok=True)
    clang = os.path.join(a.k618, "ws/prebuilts/clang/host/linux-x86/clang-r584948c/bin")
    ok = True

    # --- 1. disp edits self-check + DT properties ---
    print("== 1. display command-set wiring ==")
    ok &= DE.main(["k618_fod_disp_edits.py"]) == 0
    dts = os.path.join(a.k618, "abidiff/phone.dts")
    if not os.path.exists(dts):
        dts = os.path.join(a.k618, "phone/live.dts")
    if os.path.exists(dts):
        blob = open(dts, encoding="utf-8", errors="replace").read()
        miss = [p for _, p in DE.FOD_CMD_SETS
                if not re.search(r"(?m)^\s*" + re.escape(p) + r"\s*=", blob)]
        print("DT properties present:", "all" if not miss else "MISSING %s" % miss)
        ok &= not miss
    else:
        print("DT not found, skipped property check")

    # --- 2. touch ---
    print("== 2. touch sources ==")
    zeus = os.path.join(a.k618, "src/zeus-touch/repo/drivers/input/touchscreen")
    td = os.path.join(a.k618, "vendor/xiaomi/touch")   # where k618_touch.sh built; holds .o.cmd
    tsrc = os.path.join(tmp, "touch")
    if os.path.isdir(zeus):
        shutil.rmtree(tsrc, ignore_errors=True)
        shutil.copytree(zeus, tsrc)
        subprocess.run([sys.executable, "-I", os.path.join(REPO, "k618_touch_fix.py"), tsrc],
                       capture_output=True)
        edited = tsrc
        # apply + idempotency
        for rel, old, new in TE.EDITS:
            path = os.path.join(edited, rel)
            text = open(path, encoding="utf-8", errors="surrogateescape").read()
            text2, what = TE.apply(text, old, new)
            if what != "applied":
                print("  touch edit %s: %s" % (rel, what))
                ok &= False
            else:
                open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text2)
        for rel, old, new in TE.EDITS:
            path = os.path.join(edited, rel)
            _, what = TE.apply(open(path, encoding="utf-8", errors="surrogateescape").read(), old, new)
            ok &= what == "already"
        print("  anchors unique + idempotent:", "OK")
        # clang against the recorded touch build commands, if present
        for rel, obj in (("xiaomi/xiaomi_touch.c", "xiaomi/.xiaomi_touch.o.cmd"),
                         ("fts_spi/fts.c", "fts_spi/.fts.o.cmd")):  # per-object cmd, not the .ko link
            cmd = os.path.join(td, obj)
            if not os.path.exists(cmd):
                print("  (%s not built; skip compile of %s)" % (obj, rel))
                continue
            r, wc = run(compile_cmd(cmd, os.path.join(td, rel)), td, clang, "control: " + rel)
            ok &= r
            r, we = run(compile_cmd(cmd, os.path.join(edited, rel)), td, clang, "edited:  " + rel)
            ok &= r and we <= wc
    else:
        print("  zeus touch sources not staged; skipped")

    # --- 3. display: merged mi_disp_k618.c syntax check ---
    print("== 3. display mi_disp_k618.c (merged) ==")
    dd = os.path.join(a.k618, "vendor/qcom/opensource/display-drivers")
    msm = os.path.join(dd, "msm")
    mcmd = os.path.join(msm, ".mi_disp_k618.o.cmd")
    src = open(os.path.join(REPO, "k618_mi_disp.c"), encoding="utf-8").read()
    draft = open(os.path.join(HERE, "k618_fod_disp.c"), encoding="utf-8").read()
    # draft region: from the first new #define to the end of the draft's k618_set_feature()
    reg_start = draft.index("#define DISP_FEATURE_DOZE_BRIGHTNESS 3")
    reg_end = draft.index("\n}\n", draft.index(
        "static int k618_set_feature(int disp_id, u32 feature_id, int val)")) + 3
    region = draft[reg_start:reg_end]
    # splice: drop the tree's k618_set_feature, put the draft region where it was
    head, tail = cut_function(src, "static int k618_set_feature(int disp_id, u32 feature_id, int val)\n")
    # add the four struct fields to struct k618_bl
    head = head.replace(
        "\tbool hbm_on;\t/* stock feature_val[DISP_FEATURE_HBM] (panel+0x8f0) */\n",
        "\tbool hbm_on;\t/* stock feature_val[DISP_FEATURE_HBM] (panel+0x8f0) */\n"
        "\tu32 panel_state;\tu32 doze_level;\tu32 doze_hbm_dbv;\tu32 doze_lbm_dbv;\tu32 ui_ready_frames;\n")
    # k618_set_local_hbm / k618_doze are reached only from k618_ioctl() (snippets merged separately in
    # the real integration); reference them so this isolated TU does not trip -Wunused-function.
    merged = (head + region + "\n" + tail +
              "\nstatic __attribute__((used)) int k618_fod_check_refs(int d, int c, unsigned v)\n{\n"
              "\treturn k618_set_local_hbm(d, c) + k618_doze(g_mi->disp[d], v);\n}\n")
    if not os.path.exists(mcmd):
        print("  .mi_disp_k618.o.cmd not found (build msm_drm once); skipped compile")
    else:
        # -include stub: the FOD/doze enum constants not yet in the tree's dsi_defs.h
        stub = os.path.join(tmp, "k618_fod_enum_stub.h")
        with open(stub, "w") as f:
            for e, _ in DE.FOD_CMD_SETS:
                f.write("#define %s ((enum dsi_cmd_set_type)(DSI_CMD_SET_MAX + %d))\n"
                        % (e, DE.FOD_CMD_SETS.index((e, dict(DE.FOD_CMD_SETS)[e])) + 1))
        mc = os.path.join(tmp, "mi_disp_k618.c")
        open(mc, "w").write(merged)
        r, _ = run(compile_cmd(mcmd, os.path.join(msm, "mi_disp_k618.c")), dd, clang,
                   "control: tree mi_disp_k618.c")
        ok &= r
        r, _ = run(compile_cmd(mcmd, mc, extra=("-include", stub)) + ["-Werror"], dd, clang,
                   "merged mi_disp_k618.c -Werror")
        ok &= r

    print("scratch in", tmp)
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
