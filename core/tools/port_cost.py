#!/usr/bin/env python3
"""What each native port costs core 0 in a run, from the '# port 10s' lines.

    python3 core/tools/port_cost.py core/out/relay_<job>.csv ... [--from S]

Per port: calls a second, microseconds a call and the share of core 0 that is, with the total. The share is
`calls * us / 10 s` against one core at 804 MHz, i.e. the wall time core 0 spends inside port code.

The two microsecond columns are `port_run` ("build") and `port_commit` ("commit"), one call in 16 timed.
**"build" is not overhead.** It is everything `port_run` does, which for a leaf port is mostly reading guest state
into a native struct, but for a subtree port such as the pane walk is the actual job the guest code would have done.
Only "commit" is overhead - writing the result back - and even there the pipe handoff belongs to C40, not to the
port: commit time tracks pipe bytes (job 1569: J3DMaterial::diff 0 bytes 7.03 us, J3DSys::reinitGX 1,705 bytes
39.78 us, the pane walk 2,221 bytes 40.40 us, so ~0.019 us a byte), and every pipe byte is copied twice in
`gp_burst` - once into the FIFO mirror in guest RAM, once to the GX thread.

Why the table matters at all (MEASUREMENTS.md "Why a port pays or loses", 2026-10-29): a port is charged instruction
fetch, not work, so a large share here is not a success - one 40-instruction guest function was a net loss because
its native code fetches more than the code it replaces. **Read this next to a bisect, never on its own.**
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
        lines.append('  %-44s %8.0f calls/s %8.2f us (run %6.2f commit %6.2f) %6.1f ms/s = %5.2f%% of core 0'
                     % (port[:44], n / 10.0, us, t[0], t[1], ms, 100.0 * ms / CORE_MS))
    lines.append('  %-44s %54s %6.1f ms/s = %5.2f%% of core 0' % ('TOTAL inside ports', '', total, 100.0 * total / CORE_MS))
    return '\n'.join(lines)


def main():
    args = sys.argv[1:]
    frm = 80.0
    rest = []
    while args:   # --from anywhere, not only first: `port_cost.py LOG --from 80` used to read "--from" as a file
        if args[0] == '--from':
            frm = float(args[1])
            args = args[2:]
        else:
            rest.append(args[0])
            args = args[1:]
    args = rest
    if not args:
        print(__doc__)
        return
    for p in args:
        print(report(p, frm))


if __name__ == '__main__':
    main()
