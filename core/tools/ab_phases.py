#!/usr/bin/env python3
"""A/B of a relay log scored per phase: each 20 s phase of the A/B column against the mean of its two neighbours.

    python3 core/tools/ab_phases.py core/out/relay_367.csv rawstate 60 [ON_VALUE]

The last argument drops rows before that many seconds (the Ordon walk is a different scene for ~60 s, always in the
first arm: jobs 364-365 read +3% pooled and +0.2% / -0.6% this way). Pairing with neighbours removes a linear drift.
"""
import csv, sys, statistics as st

path, col = sys.argv[1], sys.argv[2]
skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0
on = sys.argv[4] if len(sys.argv) > 4 else "1"  # the column value of the "on" arm (e.g. an opt mask)
rows = [l for l in open(path, encoding='utf-8', errors='replace') if not l.startswith('#')]
hdr = None
data = []
for r in csv.reader(rows):
    if hdr is None:
        if 'speed_pct' in r:
            hdr = r
        continue
    if len(r) != len(hdr):
        continue
    try:
        d = dict(zip(hdr, r))
        data.append((float(d['real_s']), float(d['speed_pct']), d[col], float(d.get('gxthread_pct') or 0)))
    except (ValueError, KeyError):
        pass
phases = []
for t, sp, v, gx in data:
    if t < skip:
        continue
    if not phases or phases[-1][0] != v:
        phases.append([v, []])
    phases[-1][1].append(sp)
print(f"{path}: {len(phases)} phases after {skip:.0f} s")
for v, sps in phases:
    print(f"  {col}={v}: {len(sps):3d} rows, speed {st.mean(sps):.2f}")
diffs = []
for i in range(1, len(phases) - 1):
    a, b, c = phases[i - 1], phases[i], phases[i + 1]
    if len(a[1]) < 10 or len(b[1]) < 10 or len(c[1]) < 10:
        continue
    # each phase against the mean of its two neighbours (removes a linear drift), sign: on minus off
    m = st.mean(b[1]) - (st.mean(a[1]) + st.mean(c[1])) / 2
    diffs.append(m if b[0] == on else -m)
if len(diffs) > 1:
    print(f"  on - off (each phase vs its neighbours): {st.mean(diffs):+.3f} +- {st.stdev(diffs) / len(diffs) ** 0.5:.3f} "
          f"points over {len(diffs)} phases")
