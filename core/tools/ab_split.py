#!/usr/bin/env python3
"""A/B of a 2DS relay log, split into the tp850 cutscene and the later core-0-bound stretch.

    python3 core/tools/ab_split.py core/out/relay_148.csv [--col opt] [--split 303] [--gap 8] [--end 576] [--skip 25] [--settle 4]

Rows with real_s below --skip are dropped, the first --settle seconds of every A/B phase (each switch drops all
compiled code), and --gap seconds on both sides of --split: tp850's cutscene ends at ~301 s, and its last rows (~26%)
counted in the later stretch inflated whichever arm was running then (jobs 148-155); and rows from --end on: at ~580 s
a heavier section starts (~5%), again always in the same arm. Prints, per stretch and per value of the A/B column, the mean and standard error of speed_pct, core 0's
own speed (speed / (1 - gxwait_pct)), I-cache misses and instruction-fetch stalls.
"""
import sys, math


def load(path):
    header, out = None, []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("real_s,"):
            header = line.split(",")
            continue
        if header is None:
            continue
        v = line.split(",")
        if len(v) != len(header):
            continue
        out.append(dict(zip(header, v)))
    return out


def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError):
        return None


def stat(xs):
    """10% trimmed mean +- its standard error (winsorized variance), and the median: a few rows far off (a hitch, a
    loading moment) move a plain mean more than a 2% change does (jobs 148-149)."""
    xs = sorted(x for x in xs if x is not None)
    n = len(xs)
    if not n:
        return "-"
    g = n // 10
    core = xs[g:n - g]
    m = sum(core) / len(core)
    w = [xs[g]] * g + core + [xs[n - g - 1]] * g
    wm = sum(w) / n
    se = math.sqrt(sum((x - wm) ** 2 for x in w) / (n - 1)) / ((1 - 2 * g / n) * math.sqrt(n)) if n > 1 else 0.0
    med = xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2
    return f"{m:7.2f} +-{se:5.2f} (median {med:6.2f}, n {n})"


def main():
    a = sys.argv[1:]
    path = a[0]
    opt = lambda k, d: type(d)(a[a.index(k) + 1]) if k in a else d
    col, split, skip, settle = opt("--col", "opt"), opt("--split", 303.0), opt("--skip", 25.0), opt("--settle", 4.0)
    gap, end = opt("--gap", 8.0), opt("--end", 576.0)
    rows = load(path)
    phase_start, last = {}, None
    keep = []
    for r in rows:
        t, v = num(r, "real_s"), r.get(col)
        if t is None or v is None:
            continue
        if v != last:
            start, last = t, v
        if t < skip or t - start < settle or abs(t - split) < gap or t >= end:
            continue
        keep.append(r)
    for name, lo, hi in (("cutscene", skip, split), ("later", split, end)):
        print(f"== {name} (real_s {lo:.0f}-{hi if hi < 1e9 else 'end'})")
        part = [r for r in keep if lo <= num(r, "real_s") < hi]
        for v in sorted({r[col] for r in part}):
            g = [r for r in part if r[col] == v]
            core0 = [num(r, "speed_pct") / max(1e-6, 1 - num(r, "gxwait_pct") / 100) for r in g
                     if num(r, "speed_pct") is not None and num(r, "gxwait_pct") is not None]
            print(f"  {col}={v}: speed {stat([num(r, 'speed_pct') for r in g])}  core0 {stat(core0)}")
            print(f"  {' ' * len(col)}  icmiss_k {stat([num(r, 'icmiss_k') for r in g])}  stall_inst_m {stat([num(r, 'stall_inst_m') for r in g])}")


main()
