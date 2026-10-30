#!/usr/bin/env python3
"""What each native port costs core 0 in a run, from the '# port 10s' lines.

    python3 core/tools/port_cost.py core/out/relay_<job>.csv ... [--from S]

Per port: calls a second, microseconds a call (building the context and committing it, one call in 16 timed) and the
share of core 0 that is, with the total. The share is `calls * us / 10 s` against one core at 804 MHz, i.e. the wall
time core 0 spends inside port code.

Why it matters (MEASUREMENTS.md "Why a port pays or loses", 2026-10-29): a port is charged instruction fetch, not
work. In TP's heavy area the ports hold 20.2% of core 0, while a bisect on a static scene says they are worth
+0.9%, -0.5%, ~0, +2.2%, +0.9% and +0.2% - one of them (a 40-instruction guest function) is a net loss because its
native code fetches more than the code it replaces. So read this table next to a bisect, never on its own: a port
with a large share here is a candidate for removal or for a cheaper commit, not a success.
"""
import re
import sys

CALLS = re.compile(r'# port 10s (.+?): (\d+) taken, (\d+) checked, (\d+) not taken')
TIMES = re.compile(r'us per call: building ([0-9.]+), committing ([0-9.]+)')
CORE_MS = 1000.0  # one core's milliseconds in a second


def rows(path, frm):
    """The last '# port 10s' window at or after `frm` seconds, per port."""
    out, t = {}, 0.0
    hdr = None
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# port 10s'):
            c = CALLS.match(line)
            if not c or t < frm:
                continue
            tm = TIMES.search(line)
            out[c.group(1)] = (int(c.group(2)), int(c.group(3)), int(c.group(4)),
                               (float(tm.group(1)), float(tm.group(2))) if tm else None)
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
        try:
            t = float(dict(zip(hdr, r))['real_s'])
        except (ValueError, KeyError):
            pass
    return out


def report(path, frm):
    data = rows(path, frm)
    name = path.replace('\\', '/').split('/')[-1]
    if not data:
        return '%s: no "# port 10s" lines from %.0f s' % (name, frm)
    lines = ['%s (the last window from %.0f s)' % (name, frm)]
    total = 0.0
    for port, (taken, checked, refused, t) in data.items():
        n = taken + checked
        if not n:
            continue
        if t is None:
            lines.append('  %-44s %8.0f calls/s  (not timed: no profile clock)' % (port[:44], n / 10.0))
            continue
        us = t[0] + t[1]
        ms = n * us / 1000.0 / 10.0   # milliseconds a second
        total += ms
        lines.append('  %-44s %8.0f calls/s %8.2f us (build %6.2f commit %6.2f) %6.1f ms/s = %5.2f%% of core 0'
                     % (port[:44], n / 10.0, us, t[0], t[1], ms, 100.0 * ms / CORE_MS))
    lines.append('  %-44s %54s %6.1f ms/s = %5.2f%% of core 0' % ('TOTAL inside ports', '', total, 100.0 * total / CORE_MS))
    return '\n'.join(lines)


def main():
    args = sys.argv[1:]
    frm = 80.0
    while args and args[0] == '--from':
        frm = float(args[1])
        args = args[2:]
    if not args:
        print(__doc__)
        return
    for p in args:
        print(report(p, frm))


if __name__ == '__main__':
    main()
