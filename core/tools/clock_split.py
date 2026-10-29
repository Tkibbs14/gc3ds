#!/usr/bin/env python3
"""Where a run's emulated clock came from, and how much slack each VI field had (the '# clock 10s' lines, C41).

    python3 core/tools/clock_split.py [--from S] [--by COLUMN] core/out/relay_<job>.csv ...
    python3 core/tools/clock_split.py --selftest

Each report window names the share of the clock that was guest instructions really run, a native stand-in's charge, a
library port's charge, and a fill (the interpreter and the dispatcher jump an idle or polling loop to the next event),
plus the field-slack histogram: the tenths of each field that were fill. Bin 0 means the game left no slack, so a
charge that grows pushes the picture past its deadline; a high mean slack means charges are absorbed by a smaller fill
and cannot move speed at all.

With --by COLUMN (e.g. `--by pcharge` for a `--ab portcharge` run) the windows are grouped by that column's value, so
both arms of an A/B read side by side. Frames a second comes from the '# new frames 10s' line of the same window: with
a charge sweep that is the only honest metric, since speed_pct is the clock itself.
"""
import re
import sys

KEYS = ['stand-ins', 'ports', 'idle fills', 'poll fills']
CLOCK = re.compile(r'# clock 10s: ([0-9.]+) M emulated cycles; (.*); guest instructions run ([0-9.]+)%;'
                   r' field slack \(tenths filled\):(.*)$')


def parse(path_or_lines):
    """[(real_s, row dict, window dict)] for every '# clock 10s' line, in order."""
    lines = path_or_lines if isinstance(path_or_lines, list) else open(
        path_or_lines, encoding='utf-8', errors='replace')
    hdr, row, t, frames, out = None, {}, 0.0, None, []
    for line in lines:
        if line.startswith('# new frames 10s:'):
            m = re.match(r'# new frames 10s: ([0-9.]+)/s over ([0-9.]+) s', line)
            frames = float(m.group(1)) if m else None
            continue
        if line.startswith('# clock 10s:'):
            m = CLOCK.match(line.rstrip('\n'))
            if not m:
                continue
            w = {'Mcyc': float(m.group(1)), 'run': float(m.group(3)), 'frames': frames, 'slack': {}}
            for part in m.group(2).split('; '):
                p = re.match(r'(.+) ([0-9.]+)% in (\d+)$', part.strip())
                if p and p.group(1) in KEYS:
                    w[p.group(1)] = float(p.group(2))
                    w[p.group(1) + ' n'] = int(p.group(3))
            if any(k not in w for k in KEYS):
                continue
            for b, n in re.findall(r'(\d+):(\d+)', m.group(4)):
                w['slack'][int(b)] = w['slack'].get(int(b), 0) + int(n)
            out.append((t, dict(row), w))
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
        row = dict(zip(hdr, r))
        try:
            t = float(row['real_s'])
        except (ValueError, KeyError):
            pass
    return out


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else 0.0


def summarize(windows):
    """One line's worth of numbers for a group of windows."""
    fields = {}
    for _, _, w in windows:
        for b, n in w['slack'].items():
            fields[b] = fields.get(b, 0) + n
    n = sum(fields.values())
    s = {
        'windows': len(windows),
        'Mcyc': mean([w['Mcyc'] for _, _, w in windows]),
        'run': mean([w['run'] for _, _, w in windows]),
        'fill': mean([w['idle fills'] + w['poll fills'] for _, _, w in windows]),
        'frames': mean([w['frames'] for _, _, w in windows]),
        'speed': mean([float(r['speed_pct']) for _, r, _ in windows if 'speed_pct' in r]),
        'fields': n,
        'slack': sum(b * c for b, c in fields.items()) / n if n else 0.0,
        'tight': 100.0 * fields.get(0, 0) / n if n else 0.0,
    }
    for k in KEYS:
        s[k] = mean([w[k] for _, _, w in windows])
    return s


def line(label, s):
    return ('%-14s %3d windows  %6.2f M cyc  run %5.2f%%  stand-ins %5.2f%%  ports %5.2f%%  fills %5.2f%% '
            '(idle %5.2f%% poll %5.2f%%)  speed %6.3f  frames/s %5.2f  field slack %4.1f/10 (%4.1f%% tight)' % (
                label, s['windows'], s['Mcyc'], s['run'], s['stand-ins'], s['ports'], s['fill'], s['idle fills'],
                s['poll fills'], s['speed'], s['frames'], s['slack'], s['tight']))


def report(path, frm, by):
    ws = [(t, r, w) for t, r, w in parse(path) if t >= frm]
    name = path.replace('\\', '/').split('/')[-1]
    if not ws:
        return '%-14s no "# clock 10s" lines from %.0f s (a build before 2026-10-29 does not write them)' % (name, frm)
    out = [line(name, summarize(ws))]
    if by:
        for v in sorted({r.get(by, '?') for _, r, _ in ws}):
            out.append(line('  %s=%s' % (by, v), summarize([x for x in ws if x[1].get(by, '?') == v])))
    return '\n'.join(out)


SELFTEST = [
    'real_s,emu_s,speed_pct,fields_s,pcharge\n',
    '10.0,3.4,34.10,20.44,100\n',
    '# new frames 10s: 10.19/s over 10.20 s, gap mean 98.1 ms, sd 2.0 ms, largest 110.0 ms\n',
    '# clock 10s: 164.00 M emulated cycles; stand-ins 12.00% in 1200; ports 1.00% in 30; idle fills 50.00% in 9000;'
    ' poll fills 10.00% in 500; guest instructions run 27.00%; field slack (tenths filled): 0:4 6:96\n',
    '20.0,6.8,27.90,16.72,330\n',
    '# new frames 10s: 10.15/s over 10.10 s, gap mean 98.5 ms, sd 2.0 ms, largest 110.0 ms\n',
    '# clock 10s: 134.00 M emulated cycles; stand-ins 12.00% in 1200; ports 3.00% in 30; idle fills 40.00% in 9000;'
    ' poll fills 10.00% in 500; guest instructions run 35.00%; field slack (tenths filled): 0:50 6:50\n',
]


def selftest():
    ws = parse(list(SELFTEST))
    assert len(ws) == 2, ws
    a, b = summarize([ws[0]]), summarize([ws[1]])
    assert abs(a['run'] - 27.0) < 1e-9 and abs(a['ports'] - 1.0) < 1e-9, a
    assert abs(a['fill'] - 60.0) < 1e-9 and a['fields'] == 100, a
    assert abs(a['slack'] - 5.76) < 1e-9, a['slack']          # (4 * 0 + 96 * 6) / 100
    assert abs(a['tight'] - 4.0) < 1e-9, a['tight']
    assert abs(a['frames'] - 10.19) < 1e-9 and abs(a['speed'] - 34.10) < 1e-9, a
    assert abs(b['tight'] - 50.0) < 1e-9 and abs(b['ports'] - 3.0) < 1e-9, b
    both = summarize(ws)
    assert both['windows'] == 2 and both['fields'] == 200, both
    # A planted change in one parsed field must move the summary: a reader that ignores it proves nothing.
    bad = [s.replace('ports 1.00%', 'ports 9.00%') for s in SELFTEST]
    assert abs(summarize([parse(list(bad))[0]])['ports'] - 1.0) > 1e-9, 'plant not seen'
    bad = [s.replace('0:4 6:96', '0:96 6:4') for s in SELFTEST]
    assert abs(summarize([parse(list(bad))[0]])['tight'] - 4.0) > 1e-9, 'slack plant not seen'
    print('clock_split selftest: ok (2 windows, plants caught)')


def main():
    args = sys.argv[1:]
    frm, by = 0.0, None
    while args and args[0].startswith('--'):
        if args[0] == '--selftest':
            selftest()
            return
        if args[0] == '--from':
            frm = float(args[1])
        elif args[0] == '--by':
            by = args[1]
        else:
            print(__doc__)
            return
        args = args[2:]
    if not args:
        print(__doc__)
        return
    for p in args:
        print(report(p, frm, by))


if __name__ == '__main__':
    main()
