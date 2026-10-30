#!/usr/bin/env python3
"""Score title-screen runs (tp850.state, --seconds 420) by section, as separate whole runs.

    python3 core/tools/title_sections.py core/out/relay_<job>.csv ...

speed_pct means over the cutscene (25-150 s real time), the later Link / Epona stretch (165-300 s, core-0 bound, where
the ShdwDraw walk memo and triangle path work) and the THP movie (320-420 s), plus the real pictures a second in the
later stretch (the '# new frames 10s' lines in it). Two runs of one build agree to ~0.05 points per section.

Seconds where core 0 did not get the core are left out, and a picture window containing a screenshot is dropped, the
same way and for the same reason as `runs.py` (its docstring has the measurement): the program's own periodic
screenshot stole 4-86% of one second. `runs.py --selftest` covers the shared rule.
"""
import re
import sys

from runs import STOLEN_SHARE

SECTIONS = (('cutscene', 25, 150), ('later', 165, 300), ('movie', 320, 420))


def score(path):
    hdr = None
    vals = {name: [] for name, _, _ in SECTIONS}
    pics = []
    rows = []
    shots = []
    stolen = 0
    dropped = 0
    t = 0.0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# shot '):
            shots.append(t)
            continue
        if line.startswith('# new frames 10s:'):
            m = re.match(r'# new frames 10s: ([0-9.]+)/s over ([0-9.]+) s', line)
            if m and 165 <= t < 300:
                length = float(m.group(2))
                if not any(t - length <= b <= t for b in shots):
                    pics.append((float(m.group(1)), length))
                else:
                    dropped += 1
            continue
        if line.startswith('#'):
            continue
        r = line.rstrip('\n').split(',')
        if hdr is None:
            if 'speed_pct' in r:
                hdr = r
            continue
        if len(r) != len(hdr):
            continue
        d = dict(zip(hdr, r))
        try:
            t = float(d['real_s'])
            v = float(d['speed_pct'])
            mc = d.get('pmu_mcyc') or ''
        except (ValueError, KeyError):
            continue
        rows.append((t, v, float(mc) if mc else None))
    have = sorted(r[2] for r in rows if r[2] is not None)
    med = have[len(have) // 2] if have else None
    for t, v, mc in rows:
        if med is not None and mc is not None and mc < med * STOLEN_SHARE:
            stolen += 1
            continue
        for name, t0, t1 in SECTIONS:
            if t0 <= t < t1:
                vals[name].append(v)
    out = [path.replace('\\', '/').split('/')[-1]]
    for name, _, _ in SECTIONS:
        v = vals[name]
        out.append('%s %.2f (%d rows)' % (name, sum(v) / len(v) if v else 0.0, len(v)))
    p = sum(a * b for a, b in pics) / sum(b for a, b in pics) if pics else 0.0
    out.append('later pictures/s %.2f' % p)
    if stolen or dropped:
        out.append('[dropped %d row(s) and %d picture window(s): core 0 lost the second]' % (stolen, dropped))
    return '  '.join(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    for p in sys.argv[1:]:
        print(score(p))


if __name__ == '__main__':
    main()
