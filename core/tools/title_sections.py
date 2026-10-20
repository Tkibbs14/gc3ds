#!/usr/bin/env python3
"""Score title-screen runs (tp850.state, --seconds 420) by section, as separate whole runs.

    python3 core/tools/title_sections.py core/out/relay_<job>.csv ...

speed_pct means over the cutscene (25-150 s real time), the later Link / Epona stretch (165-300 s, core-0 bound, where
the ShdwDraw walk memo and triangle path work) and the THP movie (320-420 s), plus the real pictures a second in the
later stretch (the '# new frames 10s' lines in it). Two runs of one build agree to ~0.05 points per section.
"""
import re
import sys

SECTIONS = (('cutscene', 25, 150), ('later', 165, 300), ('movie', 320, 420))


def score(path):
    hdr = None
    vals = {name: [] for name, _, _ in SECTIONS}
    pics = []
    t = 0.0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# new frames 10s:'):
            m = re.match(r'# new frames 10s: ([0-9.]+)/s over ([0-9.]+) s', line)
            if m and 165 <= t < 300:
                pics.append((float(m.group(1)), float(m.group(2))))
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
        except (ValueError, KeyError):
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
    return '  '.join(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    for p in sys.argv[1:]:
        print(score(p))


if __name__ == '__main__':
    main()
