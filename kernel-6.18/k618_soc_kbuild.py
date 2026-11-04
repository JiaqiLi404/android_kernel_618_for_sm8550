#!/usr/bin/env python3
"""k618_soc_kbuild.py: Qualcomm's soc-repo modules turned into a kbuild external-module build.

Qualcomm's soc-repo defines its vendor modules for Kleaf's DDK (modules.bzl: registry.register(
name, out, config, srcs, deps, conditional_srcs, includes, local_defines, copts)) and a target's
vendor configuration as a Starlark dict (configs/<target>_perf.bzl). DDK evaluates that dict with
Kconfig on top of the kernel's own .config (soc-repo's Kconfig files flattened into the kernel's
Kconfig.ext), so options the dict leaves out still get their defaults and what other options
select. This does the same in three steps (k618_soc_build.sh runs them):

  fragment <soc> <dict.bzl>[,<dict.bzl>...]     the dicts (later ones override) as a .config
                                                 fragment on stdout; OnePlus' own options off
  (make olddefconfig with KCONFIG_EXT_PREFIX: the kernel's .config + the fragment, evaluated)
  kbuild <soc> <evaluated .config> <kernel .config> [module ...]
      <soc>/Kbuild                  obj-m for each module whose config is y or m (or, with names
                                    given, those and what they depend on), its objects, its
                                    conditional sources, its include paths and defines
      <soc>/gen/autoconf_vendor.h   the evaluated options the kernel's own autoconf.h lacks,
                                    force-included before every vendor source
      <soc>/gen/modules.json        what was chosen
"""

import json
import os
import re
import subprocess
import sys


class Registry:
    def __init__(self):
        self.mods = {}

    def register(self, name, out, config=None, srcs=(), deps=(), conditional_srcs=None,
                 includes=(), local_defines=(), copts=(), **kw):
        self.mods[name] = {
            "name": name,
            "out": out,
            "config": config,
            "srcs": list(srcs),
            "deps": list(deps),
            "conditional_srcs": conditional_srcs or {},
            "includes": list(includes),
            "local_defines": list(local_defines),
            "copts": list(copts),
            "extra": sorted(kw),
        }


# modules built into another: madrid (Xiaomi's 6.18) has no qcom_tzmem.ko, it is part of qcom-scm;
# apart, the two need each other in SHM bridge mode (scm allocates from tzmem's pools, tzmem
# registers them with scm's bridge calls) and neither can load first
MERGE = {"drivers/firmware/qcom/qcom_tzmem": "drivers/firmware/qcom/qcom-scm"}


def merge(mods):
    for src, into in MERGE.items():
        if src not in mods or into not in mods:
            continue
        s, d = mods.pop(src), mods[into]
        d["srcs"] += [x for x in s["srcs"] if x not in d["srcs"]]
        d["includes"] += [x for x in s["includes"] if x not in d["includes"]]
        for m in mods.values():
            m["deps"] = [into if x == src else x for x in m["deps"]]
            m["deps"] = [x for i, x in enumerate(m["deps"]) if x != m["name"] and x not in m["deps"][:i]]
    return mods


def load_registry(soc):
    reg = Registry()
    for dp, _dn, fns in os.walk(soc):
        if "/.git" in dp:
            continue
        for fn in fns:
            if fn != "modules.bzl":
                continue
            path = os.path.join(dp, fn)
            text = open(path, encoding="utf-8").read()
            if "registry.register" not in text:
                continue
            code = "\n".join(line for line in text.splitlines() if not line.startswith("load("))
            # an aggregating file also calls the register_modules() of the files it loads: each of
            # those is read on its own here, so the names it loads them under do nothing
            env = {}
            for line in text.splitlines():
                if line.startswith("load("):
                    for alias in re.findall(r"(\w+)\s*=\s*\"register_modules\"", line):
                        env[alias] = lambda _registry: None
            exec(compile(code, path, "exec"), env)  # noqa: S102 -- our own copy of Qualcomm's file
            if "register_modules" in env:
                env["register_modules"](reg)
    return merge(reg.mods)


def load_dicts(paths):
    """the target's dicts, comma-separated (the later ones override; a bare file name is looked
    for next to the first)"""
    paths = paths.split(",")
    cfg = {}
    for one in paths:
        if "/" not in one:
            one = os.path.join(os.path.dirname(paths[0]), one)
        env = {}
        exec(compile(open(one, encoding="utf-8").read(), one, "exec"), env)  # noqa: S102
        for k, v in env.items():
            if isinstance(v, dict) and k.endswith("_config"):
                cfg.update(v)
    return cfg


def vendor_only(flag):
    # OnePlus' features (OPLUS_*, its hybridswap) whose sources are not in the tree
    return "OPLUS" in flag or "HYBRIDSWAP" in flag


def read_config(path):
    cfg = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"(CONFIG_\w+)=(.*)$", line.rstrip("\n"))
        if m:
            cfg[m.group(1)] = m.group(2)
            continue
        m = re.match(r"# (CONFIG_\w+) is not set$", line.rstrip("\n"))
        if m:
            cfg[m.group(1)] = "n"
    return cfg


def enabled(cfg, opt):
    return opt is None or cfg.get(opt, "n") in ("y", "m")


def own_includes(m):
    """what Qualcomm's registry gives a module as include directories (and exports to the
    modules that depend on it): its includes, then the directory of each header in its srcs"""
    out = list(m["includes"])
    for s in m["srcs"]:
        d = os.path.dirname(s)
        if s.endswith(".h") and d not in out:
            out.append(d)
    return out


def fragment(argv):
    cfg = load_dicts(argv[1])
    for k in cfg:
        if vendor_only(k):
            cfg[k] = "n"
    for k in sorted(cfg):
        v = cfg[k]
        print(f"# {k} is not set" if v == "n" else f"{k}={v}")
    return 0


def kbuild(argv):
    soc, evaluated, kernel, wanted = argv[0].rstrip("/"), argv[1], argv[2], argv[3:]
    mods = load_registry(soc)
    cfg = read_config(evaluated)
    kcfg = read_config(kernel)
    changed = sorted(k for k in kcfg if cfg.get(k, "n") != kcfg[k])
    if changed:
        print(f"k618_soc_kbuild: the vendor configuration changes {len(changed)} options of the kernel's "
              f"own (not applied to it): {' '.join(changed[:8])}", file=sys.stderr)
    by_name = {m["name"]: m for m in mods.values()}
    # a module by the name of its .ko as well ('-' and '_' the same, as modprobe has them)
    by_out = {m["out"][:-3].replace("-", "_"): m for m in mods.values()}
    if wanted:
        todo, chosen = list(wanted), {}
        while todo:
            w = todo.pop()
            m = by_name.get(w) or by_out.get(w.replace("-", "_"))
            if not m:
                print(f"k618_soc_kbuild: no module {w!r}", file=sys.stderr)
                return 1
            if m["name"] in chosen:
                continue
            if not enabled(cfg, m["config"]):
                print(f"k618_soc_kbuild: {w}: {m['config']} is off in the evaluated configuration", file=sys.stderr)
            chosen[m["name"]] = m
            # as Qualcomm's registry does: a dependency whose config is off is left out (the
            # code that uses it is behind the same config)
            todo += [d for d in m["deps"] if d in by_name and enabled(cfg, by_name[d]["config"])]
    else:
        chosen = {n: m for n, m in by_name.items() if enabled(cfg, m["config"])}

    def dep_includes(m, seen):
        out = []
        for d in m["deps"]:
            if d in chosen and d not in seen:
                seen.add(d)
                for inc in own_includes(chosen[d]) + dep_includes(chosen[d], seen):
                    if inc not in out:
                        out.append(inc)
        return out

    lines = [
        "# generated by kernel-6.18/k618_soc_kbuild.py -- do not edit",
        "ccflags-y += -include $(M)/gen/autoconf_vendor.h",
        "# the vendor's headers before the kernel's own (as Kleaf's DDK has them): where both have a",
        "# header of the same name, the vendor code was written against the vendor's",
        "NOSTDINC_FLAGS += -I$(M)/include -I$(M)/include/uapi -I$(M)/arch/arm64/include",
        "# kernel-internal headers by their path in the kernel tree (\"drivers/clk/clk.h\"), as DDK allows",
        "ccflags-y += -I$(srctree)",
        "# the GKI builds with W=1's prototype warnings and -Werror: vendor code is not held to them",
        "ccflags-y += -Wno-error -Wno-missing-prototypes -Wno-missing-declarations",
        "",
    ]
    outs = {}
    for name in sorted(chosen):
        m = chosen[name]
        mod = m["out"][:-3]
        if mod in outs:
            print(f"k618_soc_kbuild: two modules named {mod}: {outs[mod]} and {name}", file=sys.stderr)
            return 1
        outs[mod] = name
        srcs = [s for s in m["srcs"] if s.endswith((".c", ".S"))]
        # ACK's mac80211 object list includes the NAN implementation. The vendor
        # registry predates that source split; use the matching kernel file.
        if mod == "mac80211" and os.path.isfile(f"{soc}/net/mac80211/nan.c"):
            if "net/mac80211/nan.c" not in srcs:
                srcs.append("net/mac80211/nan.c")
        # a genrule's label among the sources (cfg80211's "net/wireless/shipped-certs"): its output,
        # which the copies step has made
        srcs += [s + ".c" for s in m["srcs"] if "." not in os.path.basename(s) and os.path.exists(f"{soc}/{s}.c")]
        for opt, by_val in m["conditional_srcs"].items():
            on = enabled(cfg, opt)
            srcs += [s for s in by_val.get(on, []) if s.endswith((".c", ".S"))]
        objs = [re.sub(r"\.[cS]$", ".o", s) for s in srcs]
        if not objs:
            continue
        lines.append(f"obj-m += {mod}.o")
        lines.append(f"{mod}-y := " + " ".join(objs))
        # include directories as DDK has them: the module's own (and those of the modules it
        # depends on, which export theirs); each source also sees its own directory with <>
        # (trace headers include themselves that way)
        incs = own_includes(m)
        incs += [i for i in dep_includes(m, {name}) if i not in incs]
        flags = [f"-I$(M)/{i}" for i in incs]
        flags += [f"-D{d}" for d in m["local_defines"] if not vendor_only(d)]
        flags += [c for c in m["copts"] if not vendor_only(c)]
        for o in objs:
            lines.append(f"CFLAGS_{o} += " + " ".join([f"-I$(M)/{os.path.dirname(o)}"] + flags))
        lines.append("")
    os.makedirs(f"{soc}/gen", exist_ok=True)
    open(f"{soc}/Kbuild", "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    # the kernel's own autoconf.h comes first (kbuild force-includes it): only what it lacks
    hdr = ["/* generated by kernel-6.18/k618_soc_kbuild.py: the evaluated vendor options the kernel's",
           " * own autoconf.h does not have */"]
    for k in sorted(cfg):
        v = cfg[k]
        if v == "n" or kcfg.get(k) == v:
            continue
        if v == "y":
            hdr += [f"#ifndef {k}", f"#define {k} 1", "#endif"]
        elif v == "m":
            hdr += [f"#ifndef {k}_MODULE", f"#define {k}_MODULE 1", "#endif"]
        else:
            hdr += [f"#ifndef {k}", f"#define {k} {v}", "#endif"]
    open(f"{soc}/gen/autoconf_vendor.h", "w", encoding="utf-8", newline="\n").write("\n".join(hdr) + "\n")
    json.dump({n: chosen[n] for n in sorted(chosen)}, open(f"{soc}/gen/modules.json", "w"), indent=1)
    print(f"k618_soc_kbuild: {len(outs)} modules of {len(mods)}, Kbuild + gen/autoconf_vendor.h")
    return 0


def copies(argv):
    """common_sources.bzl's sources taken from the kernel tree: COPY_FILES as they are, and the
    patch genrules' outputs (patch -o out -i <diff> <kernel file>); the paths go to .git/info/exclude"""
    soc, ksrc = argv[0].rstrip("/"), argv[1].rstrip("/")
    text = open(f"{soc}/common_sources.bzl", encoding="utf-8").read()
    env = {}
    exec(compile("\n".join(l for l in text.splitlines() if not l.startswith("load(")), "common_sources.bzl", "exec"), env)  # noqa: S102
    made = []
    copy_files = list(env.get("COPY_FILES", []))
    if os.path.isfile(f"{ksrc}/net/mac80211/nan.c") and "net/mac80211/nan.c" not in copy_files:
        copy_files.append("net/mac80211/nan.c")
    for f in copy_files:
        src, dst = f"{ksrc}/{f}", f"{soc}/{f}"
        if not os.path.exists(src):
            print(f"k618_soc_kbuild: copies: {f} not in the kernel tree", file=sys.stderr)
            continue
        data = open(src, "rb").read()
        if not os.path.exists(dst) or open(dst, "rb").read() != data:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(data)
        made.append(f)
    for out, diff, kfile in re.findall(r'outs = \["([^"]+)"\],\s*cmd = "patch [^"]*-i \$\(execpath :([^)]+)\) '
                                       r'\$\(execpath //common:([^)]+)\)"', text):
        r = subprocess.run(["patch", "-s", "-o", f"{soc}/{out}", "-i", f"{soc}/{diff}", f"{ksrc}/{kfile}"],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"k618_soc_kbuild: copies: {diff} does not apply to {kfile}: {r.stdout}{r.stderr}", file=sys.stderr)
            continue
        made.append(out)
    # BUILD.bazel's genrule for cfg80211's built-in regulatory database keys (as net/wireless/Makefile
    # makes shipped-certs.c in a kernel build)
    certs = sorted(f for f in os.listdir(f"{soc}/net/wireless/certs") if f.endswith(".hex")) \
        if os.path.isdir(f"{soc}/net/wireless/certs") else []
    if certs:
        body = "".join(open(f"{soc}/net/wireless/certs/{f}", encoding="utf-8").read() for f in certs)
        text = ('#include "reg.h"\nconst u8 shipped_regdb_certs[] = {\n\n' + body + "};\n"
                "unsigned int shipped_regdb_certs_len = sizeof(shipped_regdb_certs);\n")
        out = f"{soc}/net/wireless/shipped-certs.c"
        if not os.path.exists(out) or open(out, encoding="utf-8").read() != text:
            open(out, "w", encoding="utf-8", newline="\n").write(text)
        made.append("net/wireless/shipped-certs.c")
    ex = f"{soc}/.git/info/exclude"
    have = set(open(ex).read().split()) if os.path.exists(ex) else set()
    with open(ex, "a") as fp:
        for f in made:
            if "/" + f not in have:
                fp.write("/" + f + "\n")
    print(f"k618_soc_kbuild: copies: {len(made)} files from the kernel tree")
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "fragment":
        return fragment(argv[2:])
    if len(argv) >= 4 and argv[1] == "copies":
        return copies(argv[2:])
    if len(argv) >= 5 and argv[1] == "kbuild":
        return kbuild(argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
