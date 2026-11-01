#!/usr/bin/env python3
"""Which launch memory rung each run on disk was in, so a ledger entry can be checked without a device.

    python3 core/tools/rungs.py                       every relay log in core/out/, newest first
    python3 core/tools/rungs.py 1760 1761 1785 1786   only these jobs
    python3 core/tools/rungs.py --by-rung             grouped by rung, with each group's own spread
    python3 core/tools/rungs.py --scene tp_area2      one scene only

**Why this exists.** The 2DS launches into one of several memory states and one of them is ~5% slow (C55): every
log's `# heap: largest malloc block` labels the state, five values appear across the logs on disk (2.9, 7.1, 7.2,
8.6, 9.0 MB), and **9.0 is the slow one** - static Ordon reads 10.03 there against 10.59 on 7.1 for the same commit.
The value is fixed when the process starts and changes only when a job downloads a build and chainloads through
`hb:ldr`, so a measurement series that switches builds part-way through can move between rungs without saying so.
`runs.py` refuses to compare across rungs, but it only sees the logs you hand it; this prints the rung for every run
on disk so an entry recorded months ago can be checked against the ones it is quoted beside.

AGENTS' "Where things stand" calls that re-check the cheapest work available on this project, because it needs no
device time and decides whether existing entries are comparable at all. A figure whose arms straddle a rung change
is wrong by up to 5%.

Columns: job, rung, scene, mean settled speed_pct, the build's commit, and whether the job chainloaded (`dl`) or
reused the resident process (`re`) - the rung can only change on a `dl`.
"""
import csv
import glob
import os
import re
import sys


def read(path):
    rung = scene = rev = ''
    dl = False
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            if not rung and 'largest malloc block' in line:
                m = re.search(r'largest malloc block ([0-9.]+)', line)
                if m:
                    rung = m.group(1)
            elif not scene and 'states/' in line:
                m = re.search(r'states/([A-Za-z0-9_]+)', line)
                if m:
                    scene = m.group(1)
            elif not rev and line.startswith('# build: gc3ds '):
                rev = line.split('gc3ds', 1)[1].strip()
            elif 'download attempt' in line:
                dl = True
            continue
        rows.append(line)
    sp = []
    if rows:
        try:
            for r in csv.DictReader(rows):
                v = r.get('speed_pct')
                t = r.get('real_s')
                if v not in (None, '') and t not in (None, '') and float(t) >= 50.0:
                    sp.append(float(v))
        except Exception:
            pass
    return rung, scene, (sum(sp) / len(sp) if sp else 0.0), rev, dl


def main():
    args = sys.argv[1:]
    by_rung = '--by-rung' in args
    if by_rung:
        args.remove('--by-rung')
    scene_only = ''
    if '--scene' in args:
        i = args.index('--scene')
        scene_only = args[i + 1]
        del args[i:i + 2]
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out')
    if args:
        paths = [os.path.join(here, 'relay_%s.csv' % a) for a in args]
    else:
        paths = sorted(glob.glob(os.path.join(here, 'relay_*.csv')),
                       key=lambda p: int(re.sub(r'\D', '', os.path.basename(p)) or 0), reverse=True)
    out = []
    for p in paths:
        if not os.path.exists(p):
            print('%-6s (no log on disk)' % os.path.basename(p))
            continue
        job = re.sub(r'\D', '', os.path.basename(p))
        rung, scene, sp, rev, dl = read(p)
        if scene_only and scene != scene_only:
            continue
        out.append((job, rung, scene, sp, rev, dl))
    if by_rung:
        # Grouped by rung and scene, with the speed *range* and deliberately NOT a spread percentage: a group mixes
        # configurations - A/B arms, `--port check` runs at a twelfth of normal speed, profiled runs - so a span
        # computed over it measures the configurations and not the rung. The first version of this printed
        # "spread 563%" for one group, which would have taught exactly the wrong reading (and a label that teaches
        # the wrong reading is a bug on this project). Use `runs.py` on the specific logs to compare anything.
        groups = {}
        for row in out:
            groups.setdefault((row[1], row[2]), []).append(row)
        for (rung, scene), rows in sorted(groups.items()):
            sp = [r[3] for r in rows if r[3] > 0]
            print('rung %-5s %-12s %3d runs, speed %.2f-%.2f (MIXED configurations - not a spread figure): %s'
                  % (rung or '?', scene or '?', len(rows), min(sp) if sp else 0.0, max(sp) if sp else 0.0,
                     ' '.join(r[0] for r in rows)))
        return 0
    print('%-6s %-5s %-12s %7s  %-14s %s' % ('job', 'rung', 'scene', 'speed', 'build', 'launch'))
    for job, rung, scene, sp, rev, dl in out:
        print('%-6s %-5s %-12s %7.2f  %-14s %s' % (job, rung or '?', scene or '?', sp, rev or '?',
                                                   'dl' if dl else 're'))
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
