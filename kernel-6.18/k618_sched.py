#!/usr/bin/env python3
"""k618_sched.py <common>: schedutilmium, this project's cpufreq governor, on android17-6.18.

The governor the phone ran on its 5.15 kernel (this project's own, first written there), unchanged
in what it does, built into the 6.18 GKI and its default. What it is and why each rule is there:
the files in kernel-6.18/sched/ themselves.

What this puts into <common> (all of it from kernel-6.18/sched/, each file a unit of its own -- no
#include into build_policy.c / build_utility.c, so the CRCs of what those export stay as they are):

  kernel/sched/cpufreq_sugov_ext.c   the governor
  kernel/sched/sugov_ext.h           sugov_ext_is_governor(), for cpufreq_schedutil.c
  kernel/sched/sugov_ext_math.h      its arithmetic: the 5.15 version's code statement for statement
  kernel/sched/slim_walt.c / .h      the tick-sampled busy share (refined 0)
  drivers/cpufreq/cpufreq_testbench.c  a cpufreq driver without hardware, inert unless the kernel is
                                     booted with cpufreq_testbench.enable=1 (the QEMU test,
                                     kernel-6.18/k618_sugov_qemu.sh)

and edits:

  kernel/sched/Makefile, drivers/cpufreq/{Kconfig,Makefile}   CONFIG_CPU_FREQ_GOV_SUGOV_EXT and
        CONFIG_CPUFREQ_TESTBENCH, both bool and "default y" (gki_defconfig + olddefconfig takes them
        without a line in k618_gki.sh)
  kernel/sched/cpufreq_schedutil.c   sugov_is_governor() -- what cpufreq_ready_for_eas() asks for
        sched_is_eas_possible() -- says yes for schedutilmium too, so EAS stays on with it. The
        governor rebuilds the sched domains' energy data at its init and exit as schedutil does
        (em_rebuild_sched_domains()).
  drivers/cpufreq/cpufreq.c   "walt" (what the vendor's post-boot script writes into every
        scaling_governor) means schedutilmium while no governor of that name is registered;
        cpufreq.walt_governor=<name> on the command line means that one instead (=schedutil: back to
        the stock governor). In get_governor(), not find_governor() where k618_gki.sh had put
        "walt -> schedutil" up to now: there it also made cpufreq_register_governor() refuse a real
        "walt" governor (-EBUSY). That earlier text is taken out where it is found; the new comment
        contains the old one's marker, so k618_gki.sh's old block, if it is still there, leaves the
        file alone once this has run.

Built in = what the phone ran on its 5.15 kernel with this project's tuning module:
sx_tunables_defaults() in cpufreq_sugov_ext.c, by the policy's place among the frequency domains --
refined 1, boost -20 (the mode "balance"), caps on the second and further clusters with caps_boost 5,
caps_front_ms 2000 / caps_front_pct 85, caps_burst 1536000 for 1000 ms on the second (mid_burst),
caps_floor 1344000 on the first, and on the first a touch floor of 1344000 kHz for 1500 ms (on 5.15
WALT's input boost did that, set by the module; there is no WALT on 6.18). The governor's own values
stay: rate limits 500 / 1000 us, react_us 4000, a window of 20 ms, iowait_boost 0. User space writes
every file as on 5.15 (policyN/schedutilmium/<name>). Not governor tunables, so not here: the
vendor's post-boot script writes sched_util_clamp_min_rt_default 0 itself (it runs to the end on 6.18
with k618_probe's walt_compat), and the PELT multiplier the 5.15 module only raised from 1 to 2 --
a placement matter on 6.18, where EAS reads PELT.

What differs from 5.15, and why (6.18's own cpufreq_schedutil.c where the skeleton is schedutil's):
  - effective_cpu_util() hands out the clamps and sugov_effective_cpu_perf() puts the headroom in:
    sx_get_util() is 6.18's sugov_get_util() where the busy share does not stand in, and the busy
    share gets 5.15's order (clamped, then the headroom) -- sg_cpu->util carries the headroom as
    6.18's does, sx_get_next_freq() no longer adds it. The I/O boost is worked out before the
    utilisation, as 6.18 does; get_capacity_ref_freq() for schedutil's mapping (enabled 0);
    ignore_dl_rate_limit() sets need_freq_update; the hold-while-busy rule has 6.18's exceptions
    (uclamp_rq_is_capped(), sched_ext); cpufreq_driver_adjust_perf() gets 6.18's arguments; the
    slow-path kthread follows dvfs_possible_from_any_cpu.
  - 6.18's deadline servers (fair, sched_ext) are in the run queue's running bandwidth nearly all the
    time a CPU runs anything: the busy share's floor "bandwidth of deadline tasks" and the DL rule of
    the rate limit leave them out (sx_dl_bw()), as 5.15, which had none, had nothing to leave out.
  - cpufreq_driver_resolve_freq() resolves to "efficient" steps only once the energy model has
    found inefficient ones: the governor picks among all steps as on 5.15, and sx_switch() sets the
    driver's table entry to the step picked where cpufreq would take another.
  - rq->donor (6.18's proxy execution) for "a real-time task waits behind a fair one"; delayed
    dequeue (a sleeping fair task kept queued, and in nr_running): "something of the fair class to
    run" is h_nr_runnable, and slim WALT's "anything runnable at the tick" leaves those tasks out.
  - the input handler's events() returns the count (6.18), as a passive observer; the cgroup attach
    tracepoint, the touch handler and slim WALT's tick hook go in from a work item -- a governor
    starts and stops in CPU hotplug, with the hotplug lock held, and a tracepoint's static key takes
    it (5.15 did that for the cgroup probe only).
  - dropped, absent from 6.18: android_rvh_set_sugov_update, android_vh_set_sugov_sched_attr and the
    old four-argument android_vh_map_util_freq (6.18's has the six arguments of 5.15's _new, called).

KMI: new units export nothing; cpufreq_schedutil.c (build_utility.o) gains a declaration-only header
and a call, cpufreq.c a static string parameter and a static function -- no type changes. Checked by
comparing the #SYMVER lines of build_utility.o and cpufreq.o before and after.

Each change is a list of (file, old, new, count), applied as k618_ports.py applies them: when none of
its new texts is in the tree, left alone when all are, an error when only some are or an old text is
not there count times. Copied files: written when they differ, an error where a file of that name is
not ours. Idempotent.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "sched")

# file in kernel-6.18/sched/ -> where it goes in <common>
FILES = {
    "cpufreq_sugov_ext.c": "kernel/sched/cpufreq_sugov_ext.c",
    "sugov_ext.h": "kernel/sched/sugov_ext.h",
    "sugov_ext_math.h": "kernel/sched/sugov_ext_math.h",
    "slim_walt.c": "kernel/sched/slim_walt.c",
    "slim_walt.h": "kernel/sched/slim_walt.h",
    "cpufreq_testbench.c": "drivers/cpufreq/cpufreq_testbench.c",
}
# a line each file has from its start, nothing else does: a file of that name in the tree without it
# is not ours (the 5.15 installer's files had the first three lines here too, and are taken as ours)
MARKS = {
    "cpufreq_sugov_ext.c": " * sugov_ext -- sched",
    "sugov_ext.h": " * schedutilmium (cpufreq_sugov_ext.c) as the rest of the scheduler sees it.",
    "sugov_ext_math.h": " * The arithmetic of the sched",
    "slim_walt.c": " * slim WALT -- how busy each CPU was, sampled at the scheduler tick.\n",
    "slim_walt.h": "#ifndef _KERNEL_SCHED_SLIM_WALT_H\n",
    "cpufreq_testbench.c": " * cpufreq_testbench -- a cpufreq driver without hardware, to try governors on.\n",
}

KCONFIG = """config CPU_FREQ_GOV_SUGOV_EXT
\tbool "'schedutilmium' cpufreq policy governor"
\tdepends on CPU_FREQ_GOV_SCHEDUTIL && ANDROID_VENDOR_HOOKS
\tselect CPU_FREQ_GOV_ATTR_SET
\tselect IRQ_WORK
\tdefault y
\thelp
\t  A governor on schedutil's skeleton: separate rate limits for going
\t  up and down, the lower table step for a request just above it, the
\t  CPU's busy time in place of the scheduler's utilisation, floors and
\t  a ceiling that follows the load. "walt" written to scaling_governor
\t  selects it (cpufreq.walt_governor= names another); EAS takes it as
\t  it takes schedutil. See kernel-6.18/k618_sched.py.

config CPUFREQ_TESTBENCH
\tbool "A cpufreq driver without hardware, to try governors on"
\tdepends on CPU_FREQ && SMP && GENERIC_ARCH_TOPOLOGY
\tdefault y
\thelp
\t  Registers a cpufreq driver that changes nothing but its own record of
\t  what it was told, and only when the kernel is booted with
\t  cpufreq_testbench.enable=1. For testing governors on a machine
\t  without frequency scaling (QEMU).

"""

WALT_HELPER = """/*
 * k618: what "walt" means while no governor of that name is registered (kernel-6.18/k618_sched.py).
 * The vendor's post-boot script writes "walt" into every policy's scaling_governor, and 6.18 has no
 * WALT: it means schedutilmium, or the governor cpufreq.walt_governor= names on the command line
 * (cpufreq.walt_governor=schedutil: the stock one) -- schedutil where that one is not there.
 */
static char walt_governor[CPUFREQ_NAME_LEN] = "schedutilmium";
module_param_string(walt_governor, walt_governor, CPUFREQ_NAME_LEN, 0444);

static struct cpufreq_governor *k618_walt_governor(void)
{
\tstruct cpufreq_governor *t = NULL;

\tif (strncasecmp(walt_governor, "walt", CPUFREQ_NAME_LEN))
\t\tt = find_governor(walt_governor);
\treturn t ? t : find_governor("schedutil");
}

"""

GET_GOVERNOR = """static struct cpufreq_governor *get_governor(const char *str_governor)
{
\tstruct cpufreq_governor *t;

\tmutex_lock(&cpufreq_governor_mutex);
\tt = find_governor(str_governor);
"""
GET_GOVERNOR_TAIL = """\tif (!t)
\t\tgoto unlock;
"""
WALT_MAP = """\t/* k618: "walt" is schedutilmium, or what cpufreq.walt_governor= names (kernel-6.18/k618_sched.py) */
\tif (!t && !strncasecmp(str_governor, "walt", CPUFREQ_NAME_LEN))
\t\tt = k618_walt_governor();
"""
# what k618_gki.sh put into find_governor() up to now (taken out where found)
OLD_WALT = """\t/* k618: "walt" is schedutil while no walt governor is registered (kernel-6.18/k618_gki.sh) */
\tif (!strncasecmp(str_governor, "walt", CPUFREQ_NAME_LEN))
\t\treturn find_governor("schedutil");

"""

CHANGES = [
    ("build", [
        ("kernel/sched/Makefile", "obj-y += build_utility.o\n",
         "obj-y += build_utility.o\n"
         "# k618: schedutilmium and slim WALT, units of their own (kernel-6.18/k618_sched.py)\n"
         "obj-$(CONFIG_CPU_FREQ_GOV_SUGOV_EXT) += cpufreq_sugov_ext.o slim_walt.o\n", 1),
        ("drivers/cpufreq/Kconfig", 'comment "CPU frequency scaling drivers"\n',
         KCONFIG + 'comment "CPU frequency scaling drivers"\n', 1),
        ("drivers/cpufreq/Makefile", "obj-$(CONFIG_CPUFREQ_DT)\t\t+= cpufreq-dt.o\n",
         "obj-$(CONFIG_CPUFREQ_TESTBENCH)\t+= cpufreq_testbench.o\t# k618 (kernel-6.18/k618_sched.py)\n"
         "obj-$(CONFIG_CPUFREQ_DT)\t\t+= cpufreq-dt.o\n", 1),
    ]),
    ("eas", [
        ("kernel/sched/cpufreq_schedutil.c", '#include <trace/hooks/sched.h>\n#include "sched.h"\n',
         '#include <trace/hooks/sched.h>\n#include "sched.h"\n'
         '#include "sugov_ext.h"\t/* k618: schedutilmium (kernel-6.18/k618_sched.py) */\n', 1),
        ("kernel/sched/cpufreq_schedutil.c",
         "bool sugov_is_governor(struct cpufreq_policy *policy)\n{\n\treturn policy->governor == &schedutil_gov;\n}\n",
         "bool sugov_is_governor(struct cpufreq_policy *policy)\n{\n"
         "\t/* k618: schedutilmium, on this governor's skeleton, is as good for EAS (kernel-6.18/k618_sched.py) */\n"
         "\treturn policy->governor == &schedutil_gov || sugov_ext_is_governor(policy);\n}\n", 1),
    ]),
    ("walt", [
        ("drivers/cpufreq/cpufreq.c", GET_GOVERNOR + GET_GOVERNOR_TAIL,
         WALT_HELPER + GET_GOVERNOR + WALT_MAP + GET_GOVERNOR_TAIL, 1),
    ]),
]


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def done(t, old, new):
    """an insertion keeps its old text (new contains it); a removal's new text is part of its old one"""
    if old in new:
        return new in t
    return new in t and old not in t


def apply_change(root, name, edits):
    texts = {}
    for f, _, _, _ in edits:
        if f not in texts:
            texts[f] = read(os.path.join(root, f))
    have = [done(texts[f], old, new) for f, old, new, n in edits]
    if all(have):
        return "already applied"
    if any(have):
        raise SystemExit(f"k618_sched: {name}: only partly applied ({sum(have)} of {len(edits)} edits)")
    for f, old, new, n in edits:
        c = texts[f].count(old)
        if c != n:
            raise SystemExit(f"k618_sched: {name}: {f}: expected {n}x {old.splitlines()[0]!r}..., found {c}")
        texts[f] = texts[f].replace(old, new)
    for f, t in texts.items():
        write(os.path.join(root, f), t)
    return "applied"


def copy_files(root):
    sources, written = {}, []
    for name in FILES:
        src = read(os.path.join(SRC, name))
        if "\r" in src:
            raise SystemExit(f"k618_sched: sched/{name}: CRLF line ends")
        if MARKS[name] not in src:
            raise SystemExit(f"k618_sched: sched/{name}: its own first lines are missing")
        sources[name] = src
    for name, rel in FILES.items():
        dest = os.path.join(root, rel)
        if os.path.exists(dest) and MARKS[name] not in read(dest):
            raise SystemExit(f"k618_sched: {rel}: exists and is not ours")
    for name, rel in FILES.items():
        dest = os.path.join(root, rel)
        if not os.path.exists(dest) or read(dest) != sources[name]:
            write(dest, sources[name])
            written.append(rel)
    return written


def old_walt(root):
    """k618_gki.sh's "walt -> schedutil" in find_governor(), taken out where it is"""
    path = os.path.join(root, "drivers/cpufreq/cpufreq.c")
    t = read(path)
    c = t.count(OLD_WALT)
    if c > 1:
        raise SystemExit(f"k618_sched: drivers/cpufreq/cpufreq.c: the old walt mapping {c} times")
    if c:
        write(path, t.replace(OLD_WALT, ""))
        return "the old mapping in find_governor() taken out"
    return "no old mapping"


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = argv[1].rstrip("/")
    written = copy_files(root)
    print("k618_sched: files: " + (", ".join(written) if written else "as they are"))
    print(f"k618_sched: walt: {old_walt(root)}")
    for name, edits in CHANGES:
        print(f"k618_sched: {name}: {apply_change(root, name, edits)}")
    # (what k618_gki.sh's old block greps for: it leaves the file alone once this has run)
    if 'k618: "walt" is schedutil' not in read(os.path.join(root, "drivers/cpufreq/cpufreq.c")):
        raise SystemExit("k618_sched: drivers/cpufreq/cpufreq.c: the walt marker is missing")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
