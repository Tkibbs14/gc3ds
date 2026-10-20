#!/usr/bin/env python3
"""Score 2DS runs as separate whole runs (the way every default since 10-19 was decided).

    python3 core/tools/runs.py [--skip S] core/out/relay_<job>.csv ...

Per run: settled means (rows from S real seconds, default 50) of speed_pct and fields_s, real frames a second (the
'# new frames 10s' lines after S, weighted by their real length) and the VI fields between XFB copies summed from the
'# new frames hist' lines (TP's gameplay is locked at 2 fields a picture: a change that adds 1- or 3-field gaps changed
the pacing, not only the speed). Gameplay scenes: the heavy Ordon area (tp_area2.state, no presses: two runs of one
build agree to ~0.1%) and the scripted Ordon walk (tp_ordon.state with the --press list in REMOTE_RUNS.md). For the
title (tp850.state) use title_sections.py: its sections differ too much to pool.
"""
import re
import sys


def score(path, skip):
    hdr = None
    sp, fs, fr, fh = [], [], [], {}
    t = 0.0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# new frames 10s:'):
            m = re.match(r'# new frames 10s: ([0-9.]+)/s over ([0-9.]+) s', line)
            if m and t >= skip:
                fr.append((float(m.group(1)), float(m.group(2))))
            continue
        if line.startswith('# new frames hist'):
            if t >= skip and 'VI fields between copies:' in line:
                for k, v in re.findall(r'(\d+):(\d+)', line.split('VI fields between copies:')[1]):
                    fh[int(k)] = fh.get(int(k), 0) + int(v)
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
            if t >= skip:
                sp.append(float(d['speed_pct']))
                fs.append(float(d['fields_s']))
        except (ValueError, KeyError):
            pass
    f = sum(a * b for a, b in fr) / sum(b for a, b in fr) if fr else 0.0
    return '%-22s speed %.3f fields/s %.3f frames/s %.3f (%d windows) fields per copy %s' % (
        path.replace('\\', '/').split('/')[-1], sum(sp) / max(len(sp), 1), sum(fs) / max(len(fs), 1), f, len(fr),
        ' '.join('%d:%d' % kv for kv in sorted(fh.items())))


def main():
    args = sys.argv[1:]
    skip = 50.0
    if args[:1] == ['--skip']:
        skip = float(args[1])
        args = args[2:]
    if not args:
        print(__doc__)
        return
    for p in args:
        print(score(p, skip))


if __name__ == '__main__':
    main()
