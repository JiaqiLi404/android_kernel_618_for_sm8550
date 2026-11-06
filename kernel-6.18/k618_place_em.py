#!/usr/bin/env python3
"""k618_place_em.py: the measured CPU energy model that k618_place.c installs, how it is derived, and
host-side checks of the arithmetic the module does with it.

  python3 -I k618_place_em.py                 the derivation: per OPP, the device tree's model and ours
  python3 -I k618_place_em.py --c             the C tables (paste into k618_place.c)
  python3 -I k618_place_em.py --check FILE.c  unit tests: FILE.c's tables are these, and the module's
                                              power / cost / interpolation arithmetic (as C does it)
  python3 -I k618_place_em.py --emdata F...   compare the embedded measurements with emdata files

What EAS compares. find_energy_efficient_cpu() prices a task's utilisation u on a performance domain as
cost(OPP) * u, where em_compute_costs() makes cost = 10 * power / performance and performance =
capacity * f / f_top. Utilisation is capacity-invariant (the device tree's capacity-dmips-mhz), so for
the comparison between clusters to be right, cost has to be proportional to the energy one unit of real
work takes on that cluster at that OPP -- whatever the capacities claim about speed. The device tree's
model is dynamic power only (C * V^2 * f, dynamic-power-coefficient 100 / 270 / 588): no leakage, no
cluster or memory power, and the capacities' speed ratios hold for no real workload (sort: middle at
2803 MHz is 1.8x little at 2016 MHz, the capacities say 3x; pointer chasing: 3.7x).

The measurements (this phone: the same SoC and OPPs, on its 5.15 kernel; one core of a cluster busy with one
workload at a pinned frequency, another cluster kept awake at a low frequency, the screen dozing; mW =
whole-device power over the same state with that core idle; units/s = the workload's own throughput):
four kinds of work -- sort (integer compute), chase (dependent loads: memory latency), copy (memory
bandwidth), lz4 (compression) -- at 5 / 6 / 6 OPPs of the three clusters.

How the table is made, at every OPP the kernel has (16 / 20 / 21):
  1. per workload and cluster, power = a * DT(f) + b (DT(f) = the device tree's own power at that OPP,
     which carries the V^2 * f shape of the hardware voltage table; b >= 0 is what does not scale with
     it: leakage, the cluster, memory), fitted by weighted least squares; one point whose relative
     residual exceeds 35 % is dropped and the fit repeated (sort at middle 499 MHz, +31 mW: below the
     dynamic power alone; chase at middle 1056 MHz; lz4 at little 1555 MHz: above its 2016 MHz point);
  2. per workload and cluster, throughput = k * f^g (log-log least squares; no point dropped);
  3. energy per unit of work e = power / throughput at every OPP from the two fits, normalised per
     workload to little at 1228.8 MHz, and the geometric mean over the four workloads: E. The
     geometric mean weighs each kind of work alike and does not depend on each workload's units;
  4. the module sets power = K * E * performance (performance as the kernel computed it), so the
     kernel's own cost = 10 * power / performance = 10 * K * E -- proportional to E on all three
     clusters, whatever capacities the kernel uses. K = 588 uW per capacity unit: little at 1228.8 MHz
     then has the 108 mW its core measured for CPU-bound work (sort 102, lz4 115) with the 302 / 891 /
     1024 capacities 6.18 reports. The other entries are therefore not watts at the battery but watts
     scaled by (capacity / real speed) of their cluster, which is exactly what makes cost right.
"""
import math
import re
import sys

# --------------------------------------------------------------------------- the measurements
# (MHz, mW over rest, units/s): two series of measurements, sort and chase in one, copy and lz4 in the other
MEAS = {
    ("sort", "little"): [(307, 29, 75.3), (787, 50, 195.9), (1228, 102, 305.4), (1555, 132, 386.5), (2016, 237, 494.4)],
    ("sort", "middle"): [(499, 31, 157.0), (1056, 228, 328.4), (1536, 377, 477.7), (2188, 779, 681.6), (2707, 1290, 844.8),
                         (2803, 1370, 873.8)],
    ("sort", "prime"): [(595, 226, 213.7), (1248, 546, 442.1), (1843, 853, 650.2), (2342, 1590, 829.2), (2956, 2695, 1044.2),
                        (3187, 3417, 1117.3)],
    ("chase", "little"): [(307, 44, 1155.2), (787, 55, 1677.2), (1228, 141, 2322.6), (1555, 177, 2466.4), (2016, 261, 2695.1)],
    ("chase", "middle"): [(499, 88, 1627.1), (1056, 343, 2872.3), (1536, 249, 4070.6), (2188, 660, 8923.5), (2707, 846, 9266.2),
                          (2803, 846, 9840.4)],
    ("chase", "prime"): [(595, -50, 4371.9), (1248, 350, 6107.9), (1843, 809, 15789.4), (2342, 1045, 16247.9),
                         (2956, 1473, 19306.7), (3187, 1668, 19926.0)],
    ("copy", "little"): [(307, 504, 1763.6), (787, 991, 3984.0), (1228, 1345, 5890.8), (1555, 1694, 7066.1), (2016, 2100, 8169.3)],
    ("copy", "middle"): [(499, 1388, 7216.5), (1056, 1818, 10350.4), (1536, 2385, 11953.3), (2188, 3597, 15400.1),
                         (2707, 3806, 15869.4), (2803, 4114, 16992.0)],
    ("copy", "prime"): [(595, 1804, 10167.5), (1248, 2511, 13960.3), (1843, 3581, 17781.3), (2342, 4231, 18240.0),
                        (2956, 5282, 19751.4), (3187, 5802, 19797.3)],
    ("lz4", "little"): [(307, 29, 12509.9), (787, 77, 33248.0), (1228, 115, 52076.7), (1555, 252, 65877.3), (2016, 212, 85077.3)],
    ("lz4", "middle"): [(499, 94, 37316.4), (1056, 265, 78131.0), (1536, 415, 113696.0), (2188, 1003, 162314.7),
                        (2707, 1422, 200789.3), (2803, 1469, 207978.7)],
    ("lz4", "prime"): [(595, 149, 50658.2), (1248, 466, 105646.4), (1843, 862, 155744.0), (2342, 1569, 198794.7),
                       (2956, 2608, 251168.0), (3187, 3289, 269546.7)],
}
WORKS = ["sort", "chase", "copy", "lz4"]
CLUSTERS = ["little", "middle", "prime"]
# a point that is not a measurement: chase on the prime at 595 MHz read below rest (the rest state itself
# was 724 mW there, ~400 above the others: something else ran)
INVALID = {("chase", "prime", 595)}

# --------------------------------------------------------------------------- the OPPs and the DT model
# every OPP the 6.18 energy model has (kHz), with the device tree model's power in mW as the 5.15 kernel's
# debugfs energy_model printed it (whole mW, truncated; 6.18 has the same voltages, in uW: little
# 10327..132923)
DT_MW = {
    "little": "307200:10 441600:14 556800:19 672000:24 787200:29 902400:34 1017600:42 1113600:47 1228800:56 "
              "1344000:65 1459200:76 1555200:85 1670400:96 1785600:108 1900800:120 2016000:132",
    "middle": "499200:42 614400:56 729600:71 844800:88 940800:103 1056000:124 1171200:141 1286400:164 1401600:187 "
              "1536000:217 1651200:249 1785600:287 1920000:331 2054400:376 2188800:424 2323200:481 2457600:542 "
              "2592000:602 2707200:673 2803200:703",
    "prime": "595200:111 729600:142 864000:175 998400:214 1132800:252 1248000:293 1363200:332 1478400:373 1593600:422 "
             "1708800:475 1843200:549 1977600:622 2092800:695 2227200:796 2342400:890 2476800:1007 2592000:1116 "
             "2726400:1241 2841600:1365 2956800:1509 3187200:1858",
}
OPP = {c: [(int(a), int(b)) for a, b in (kv.split(":") for kv in s.split())] for c, s in DT_MW.items()}
REF = ("little", 1228800)
K_UW = 588           # uW per capacity unit at E = 1.000 (see the docstring)
EM_MAX_POWER = 64000000
CAPS = {"6.18 (302/891/1024)": {"little": 302, "middle": 891, "prime": 1024},
        "DT (280/855/1024)": {"little": 280, "middle": 855, "prime": 1024}}


def dt_mw(cl, khz):
    for k, mw in OPP[cl]:
        if k == khz:
            return mw + 0.5          # the middle of the truncated value
    raise KeyError((cl, khz))


def khz_of(cl, mhz):
    for k, _ in OPP[cl]:
        if k // 1000 == mhz:
            return k
    raise KeyError((cl, mhz))


def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def wls(xs, ys, ws):
    """y = a x + b, weighted least squares."""
    sw = sum(ws)
    sx = sum(w * x for w, x in zip(ws, xs))
    sy = sum(w * y for w, y in zip(ws, ys))
    sxx = sum(w * x * x for w, x in zip(ws, xs))
    sxy = sum(w * x * y for w, x, y in zip(ws, xs, ys))
    a = (sw * sxy - sx * sy) / (sw * sxx - sx * sx)
    return a, (sy - a * sx) / sw


def fit_power(pts, cl):
    """power = a * DT + b (b >= 0); weights for a reading good to ~8 mW or 5 %."""
    def once(pp):
        xs = [dt_mw(cl, khz_of(cl, f)) for f, _, _ in pp]
        ys = [p for _, p, _ in pp]
        ws = [1.0 / (64.0 + (0.05 * y) ** 2) for y in ys]
        a, b = wls(xs, ys, ws)
        if b < 0:
            b = 0.0
            a = sum(w * x * y for w, x, y in zip(ws, xs, ys)) / sum(w * x * x for w, x in zip(ws, xs))
        return a, b
    a, b = once(pts)
    dropped = None
    if len(pts) >= 5:
        res = [abs(p - (a * dt_mw(cl, khz_of(cl, f)) + b)) / (a * dt_mw(cl, khz_of(cl, f)) + b) for f, p, _ in pts]
        worst = max(range(len(pts)), key=lambda i: res[i])
        if res[worst] > 0.35:
            dropped = pts[worst]
            a, b = once([q for i, q in enumerate(pts) if i != worst])
    return a, b, dropped


def fit_speed(pts):
    xs = [math.log(f) for f, _, _ in pts]
    ys = [math.log(u) for _, _, u in pts]
    g, lk = wls(xs, ys, [1.0] * len(xs))
    return g, lk


def derive():
    """E (energy per unit of blended work, little at 1228.8 MHz = 1) at every OPP, plus the fits."""
    fits, drops = {}, []
    for (w, cl), pts in MEAS.items():
        pts = [q for q in pts if (w, cl, q[0]) not in INVALID]
        a, b, d = fit_power(pts, cl)
        if d:
            drops.append((w, cl, d))
        g, lk = fit_speed(pts)
        fits[(w, cl)] = (a, b, g, lk)

    def e_of(w, cl, khz):
        a, b, g, lk = fits[(w, cl)]
        power = a * dt_mw(cl, khz) + b
        speed = math.exp(lk + g * math.log(khz / 1000.0))
        return power / speed, power, speed

    E = {}
    for cl in CLUSTERS:
        for khz, _ in OPP[cl]:
            rel = [e_of(w, cl, khz)[0] / e_of(w, *REF)[0] for w in WORKS]
            E[(cl, khz)] = gmean(rel)
    return E, fits, drops, e_of


def measured_blend():
    """The geometric blend at the measured points, straight from the readings (for comparison)."""
    out = {}
    ref = {w: next((p / u) for f, p, u in MEAS[(w, "little")] if f == 1228) for w in WORKS}
    for cl in CLUSTERS:
        for f, _, _ in MEAS[("sort", cl)]:
            rel = []
            for w in WORKS:
                for ff, p, u in MEAS[(w, cl)]:
                    if ff == f and p > 0 and (w, cl, f) not in INVALID:
                        rel.append((p / u) / ref[w])
            out[(cl, khz_of(cl, f))] = (gmean(rel), len(rel))
    return out


def e_milli_table():
    E, _, _, _ = derive()
    return {cl: [(khz, round(E[(cl, khz)] * 1000)) for khz, _ in OPP[cl]] for cl in CLUSTERS}


# --------------------------------------------------------------------------- the module's arithmetic
def kernel_perf(cap, khz, fmax):
    """em_init_performance(): div64_u64(max_cap * freq, fmax)."""
    return cap * khz // fmax


def kernel_costs(states):
    """em_compute_costs(): states = [(khz, perf, power)] ascending; returns [(cost, inefficient)]."""
    out = [None] * len(states)
    prev = None
    for i in range(len(states) - 1, -1, -1):
        _, perf, power = states[i]
        cost = power * 10 // perf
        ineff = prev is not None and cost >= prev
        if not ineff:
            prev = cost
        out[i] = (cost, ineff)
    return out


def module_e(table, khz):
    """k618_em_e(): the table's E at khz; linear in kHz between neighbours, the end value outside."""
    if khz <= table[0][0]:
        return table[0][1]
    for (k0, e0), (k1, e1) in zip(table, table[1:]):
        if khz == k1:
            return e1
        if k0 < khz < k1:
            num, den = (e1 - e0) * (khz - k0), k1 - k0
            return e0 + (num // den if num >= 0 else -(-num // den))   # C division: toward zero
    return table[-1][1]


def module_power(e_milli, perf):
    """k618_em_power(): K * E * performance, in uW."""
    return K_UW * e_milli * perf // 1000


# --------------------------------------------------------------------------- output
def report():
    E, fits, drops, e_of = derive()
    meas = measured_blend()
    print("fits: power = a * DT + b (mW), throughput = k * f^g")
    for cl in CLUSTERS:
        for w in WORKS:
            a, b, g, _ = fits[(w, cl)]
            print(f"  {w:5s} {cl:6s}: a {a:5.2f} b {b:7.1f}  g {g:5.3f}")
    print("dropped as outliers (relative residual > 35 %): " +
          ", ".join(f"{w} {cl} {d[0]} MHz (+{d[1]} mW)" for w, cl, d in drops))
    caps = CAPS["6.18 (302/891/1024)"]
    ref_dt = None
    rows = []
    for cl in CLUSTERS:
        fmax = OPP[cl][-1][0]
        for khz, mw in OPP[cl]:
            perf = kernel_perf(caps[cl], khz, fmax)
            dt_cost = (mw + 0.5) * 1000 * 10 / perf
            if (cl, khz) == REF:
                ref_dt = dt_cost
            rows.append((cl, khz, mw, perf, dt_cost))
    tab = e_milli_table()
    print("\nper OPP (capacities as 6.18 reports them, 302 / 891 / 1024): DT = device tree model, new = measured;"
          "\ncost relative to little 1228.8 MHz; E measured = the geometric blend of the raw readings there")
    print(f"{'cluster':7s} {'kHz':>8s} {'perf':>5s} {'DT mW':>6s} {'new mW':>7s} {'DT cost':>8s} {'new cost':>8s} "
          f"{'change':>7s}  E measured")
    for cl, khz, mw, perf, dt_cost in rows:
        e = dict(tab[cl])[khz]
        new_uw = module_power(e, perf)
        new_cost = new_uw * 10 / perf / (module_power(1000, kernel_perf(caps["little"], REF[1], OPP["little"][-1][0])) * 10 /
                                         kernel_perf(caps["little"], REF[1], OPP["little"][-1][0]))
        dt_rel = dt_cost / ref_dt
        m = meas.get((cl, khz))
        ms = f"{m[0]:5.2f} (n={m[1]})" if m else "  -   (interpolated)"
        print(f"{cl:7s} {khz:8d} {perf:5d} {mw:6d} {new_uw / 1000:7.1f} {dt_rel:8.2f} {new_cost:8.2f} "
              f"{(new_cost / dt_rel - 1) * 100:+6.0f}%  {ms}")


def c_tables():
    tab = e_milli_table()
    out = ["/* generated by k618_place_em.py --c: {kHz, E x 1000}, E = energy per unit of blended work, little at",
           " * 1228.8 MHz = 1000 */"]
    for cl in CLUSTERS:
        out.append(f"static const struct k618_em_point k618_em_{cl}[] = {{")
        cells = [f"{{{k}, {e}}}" for k, e in tab[cl]]
        for i in range(0, len(cells), 6):
            out.append("\t" + ", ".join(cells[i:i + 6]) + ",")
        out.append("};")
    return "\n".join(out)


def check(path):
    src = open(path, encoding="utf-8").read()
    fails = 0

    def ok(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
        print(("PASS " if cond else "FAIL ") + what)

    tab = e_milli_table()
    for cl in CLUSTERS:
        m = re.search(r"k618_em_%s\[\]\s*=\s*\{(.*?)\};" % cl, src, re.S)
        got = [(int(a), int(b)) for a, b in re.findall(r"\{(\d+),\s*(\d+)\}", m.group(1))] if m else []
        ok(got == tab[cl], f"{cl}: the table in {path} is the derived one ({len(got)} OPPs)")
    m = re.search(r"#define K618_EM_UW\s+(\d+)", src)
    ok(m and int(m.group(1)) == K_UW, f"K618_EM_UW is {K_UW}")

    for name, caps in CAPS.items():
        ratios, lines = [], []
        for cl in CLUSTERS:
            fmax = OPP[cl][-1][0]
            states = []
            for khz, _ in OPP[cl]:
                perf = kernel_perf(caps[cl], khz, fmax)
                states.append((khz, perf, module_power(module_e(tab[cl], khz), perf)))
            costs = kernel_costs(states)
            powers = [s[2] for s in states]
            ok(all(b > a for a, b in zip(powers, powers[1:])), f"{name} {cl}: power rises with every OPP")
            ok(max(powers) < EM_MAX_POWER and min(powers) > 0, f"{name} {cl}: power within (0, EM_MAX_POWER)")
            for (khz, _, _), (cost, _), (_, e) in zip(states, costs, tab[cl]):
                ratios.append(cost / e)
            ineff = [s[0] for s, c in zip(states, costs) if c[1]]
            lines.append(f"    {cl}: inefficient by the measured model: {ineff or 'none'}")
        spread = (max(ratios) - min(ratios)) / min(ratios)
        ok(spread < 0.01, f"{name}: cost / E is one constant over all 57 OPPs (spread {spread * 100:.2f} %)")
        print("\n".join(lines))

    # interpolation: every OPP left out once is recovered from its neighbours within the curve's own bend
    worst = 0.0
    for cl in CLUSTERS:
        t = tab[cl]
        for i in range(1, len(t) - 1):
            e = module_e(t[:i] + t[i + 1:], t[i][0])
            worst = max(worst, abs(e - t[i][1]) / t[i][1])
    ok(worst < 0.08, f"interpolation: an OPP left out comes back within {worst * 100:.1f} % (< 8 %)")
    ok(module_e(tab["little"], 100000) == tab["little"][0][1] and module_e(tab["little"], 3000000) == tab["little"][-1][1],
       "interpolation: outside the table the end values")

    # what EAS would pick for one task on an idle system: util u needs performance >= 1.25 u (schedutil's
    # margin), energy = cost(OPP) * u; the cluster with the lowest energy that fits
    caps = CAPS["6.18 (302/891/1024)"]

    def pick(model, u):
        best = None
        for cl in CLUSTERS:
            fmax = OPP[cl][-1][0]
            for khz, mw in OPP[cl]:
                perf = kernel_perf(caps[cl], khz, fmax)
                if perf * 4 < u * 5:
                    continue
                power = (mw + 0.5) * 1000 if model == "DT" else module_power(module_e(tab[cl], khz), perf)
                energy = power * 10 // perf * u
                if best is None or energy < best[0]:
                    best = (energy, cl, khz)
                break
        return best

    for model in ("DT", "measured"):
        spans, last = [], None
        for u in range(8, 1000, 8):
            p = pick(model, u)
            if not p:
                break
            if not last or p[1] != last[1]:
                spans.append([p[1], u, u])
            spans[-1][2] = u
            last = p
        print(f"    {model:8s}: one task on an idle system goes to " +
              ", ".join(f"{c} for util {a}-{b}" for c, a, b in spans))
    print(f"\n{'all checks passed' if not fails else f'{fails} check(s) FAILED'}")
    return 1 if fails else 0


def compare_emdata(paths):
    head = re.compile(r"^== (\w+) on policy(\d)")
    point = re.compile(r"^\s*(\d+) MHz: rest\s+\d+ mW \| \+\s*(-?\d+) mW,\s+([\d.]+) units/s")
    cl_of = {"0": "little", "3": "middle", "7": "prime"}
    got, cur = {}, None
    for path in paths:
        for line in open(path, encoding="utf-8", errors="replace"):
            m = head.match(line)
            if m:
                cur = (m.group(1), cl_of[m.group(2)])
                continue
            m = point.match(line)
            if m and cur:
                got.setdefault(cur, []).append((int(m.group(1)), int(m.group(2)), float(m.group(3))))
    same = got == MEAS
    print("the embedded measurements are the files' own" if same else "the embedded measurements DIFFER from the files")
    return 0 if same else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--c":
        print(c_tables())
    elif len(sys.argv) > 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2]))
    elif len(sys.argv) > 2 and sys.argv[1] == "--emdata":
        sys.exit(compare_emdata(sys.argv[2:]))
    else:
        report()
