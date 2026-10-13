#!/usr/bin/env python3
"""Average a 2DS perf.csv (or relay_<job>.csv) over its heavy rows.

    python3 core/tools/perf_heavy.py core/out/relay_50.csv [more.csv ...] [--min-draws 1000] [--cols a,b,c] [--by COL]

Heavy rows are rows with draws_s above --min-draws (the TP title scene and cutscene; light screens run at full speed
and would dilute every average). Each file's last header line names the columns, so files from different builds
compare by name. --by splits the average by the value of an A/B column (traces, hle, ...). Prints mean and standard
error of the mean for speed and for core 0's own speed (speed / (1 - gxwait_pct): JIT changes show up there while the
GX thread limits the game), then the requested columns (default: the core 0 / core 2 / core 3 profile columns).
"""
import sys, math

DEFAULT = ("speed_pct fields_s draws_s verts_s gxthread_pct gxwait_pct vwork_pct vwait_pct gxvtx_pct vdec_pct "
           "vprep_pct vpal_pct vqueue_pct gxstate_pct gxtex_pct gxsync_pct gxend_pct gxpres_pct gpu_calls_s "
           "gpu_splits_s c3d_gpu_ms icmiss_k stall_inst_m inst_m").split()


def rows(path):
    header = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("real_s,"):
            header = line.split(",")
            continue
        if header is None:
            continue
        vals = line.split(",")
        if len(vals) != len(header):
            continue
        yield dict(zip(header, vals))


def num(r, k):
    v = r.get(k, "")
    try:
        return float(v)
    except ValueError:
        return None


def stats(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None, None, 0
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, 0.0, len(xs)
    var = sum((x - m) ** 2 for x in xs) / (len(xs) - 1)
    return m, math.sqrt(var / len(xs)), len(xs)


def main():
    args = sys.argv[1:]
    min_draws, cols, by, files = 1000.0, DEFAULT, None, []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--min-draws":
            min_draws = float(args[i + 1]); i += 2; continue
        if a == "--cols":
            cols = args[i + 1].split(","); i += 2; continue
        if a == "--by":
            by = args[i + 1]; i += 2; continue
        files.append(a); i += 1
    for f in files:
        heavy = [r for r in rows(f) if (num(r, "draws_s") or 0) > min_draws]
        groups = {}
        for r in heavy:
            groups.setdefault(r.get(by, "") if by else "all", []).append(r)
        for g, rs in sorted(groups.items()):
            m, se, n = stats([num(r, "speed_pct") for r in rs])
            label = f"{f} [{by}={g}]" if by else f
            if m is None:
                print(f"{label}: no heavy rows"); continue
            print(f"{label}: {n} heavy rows, speed {m:.2f}% +- {se:.2f}")
            # Core 0's own speed: what it ran in the time it did not spend waiting for the GX thread (gxwait_pct). While
            # the GX thread limits the game, a faster core 0 shows up here and as more gxwait, not as speed (2DS job 88:
            # fault mode on / off 19.44 / 19.53% speed, gxwait 16.1 / 7.5%, so core 0 itself ~10% faster).
            c0, c0se, c0n = stats([num(r, "speed_pct") / (1 - num(r, "gxwait_pct") / 100)
                                   for r in rs if num(r, "speed_pct") is not None and num(r, "gxwait_pct") is not None
                                   and num(r, "gxwait_pct") < 95])
            if c0 is not None:
                print(f"   core 0 speed (speed / (1 - gxwait)): {c0:.2f}% +- {c0se:.2f}")
            line = []
            for c in cols:
                cm, _, cn = stats([num(r, c) for r in rs])
                if cm is not None:
                    line.append(f"{c}={cm:.4g}" + ("" if cn == n else f"(n{cn})"))
            for k in range(0, len(line), 6):
                print("   " + "  ".join(line[k:k + 6]))


if __name__ == "__main__":
    main()
