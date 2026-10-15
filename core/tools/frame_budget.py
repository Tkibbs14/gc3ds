#!/usr/bin/env python3
"""Where core 0's cycles go in a relay run: perf.csv rows (one per second) averaged over a stretch, as cycles per
emulated second and per guest instruction, with the stall split the PMU gives (docs/research/TIMING_MODEL_PLAN.md).

    python3 core/tools/frame_budget.py core/out/relay_170.csv [--from S] [--to S] [--col opt=VALUE]

Columns used: pmu_mcyc (core 0 Mcycles in the row), inst_m, stall_inst_m, stall_data_m, branch_m, brmiss_k, icmiss_k,
dcrmiss_k, dcwmiss_k, tlbmiss_k, ins_k (guest instructions), emu_s, real_s, speed_pct, cpu_pct, jitcode_pct,
compiles_s, icbi_flushes, slowld_k, slowst_k."""
import csv
import sys


def rows(path):
    with open(path) as f:
        lines = [l for l in f if l.strip() and not l.startswith('#')]
    hdr = None
    out = []
    for r in csv.reader(lines):
        if r and r[0] == 'real_s':
            hdr = r
            continue
        if hdr and len(r) == len(hdr):
            d = {}
            for k, v in zip(hdr, r):
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
            out.append(d)
    return out


def main():
    a = sys.argv[1:]
    path = a[0]
    lo, hi, col = 60.0, 1e9, None
    i = 1
    while i < len(a):
        if a[i] == '--from':
            lo = float(a[i + 1]); i += 2
        elif a[i] == '--to':
            hi = float(a[i + 1]); i += 2
        elif a[i] == '--col':
            col = a[i + 1].split('='); i += 2
        else:
            sys.exit('unknown ' + a[i])
    rs = [r for r in rows(path) if lo <= r.get('real_s', 0) <= hi and r.get('pmu_mcyc', 0) > 0]
    if col:
        rs = [r for r in rs if str(r.get(col[0])).rstrip('0').rstrip('.') == col[1].rstrip('0').rstrip('.') or str(r.get(col[0])) == col[1]]
    if not rs:
        sys.exit('no rows with counters in that stretch')
    n = len(rs)

    # The PMU counts two events at a time and rotates through five pairs, one pair per row: an event is a mean over
    # the rows that measured it, times n (rows with the cell empty did not measure it).
    def s(k):
        v = [r[k] for r in rs if isinstance(r.get(k), float)]
        return sum(v) / len(v) * n if v else 0.0
    cyc = s('pmu_mcyc') * 1e6
    inst = s('inst_m') * 1e6
    gins = s('ins_k') * 1e3
    print(f'{path}: {n} rows, real {rs[0]["real_s"]:.0f}-{rs[-1]["real_s"]:.0f} s, speed {s("speed_pct") / n:.1f}%, cpu {s("cpu_pct") / n:.1f}%')
    print(f'core 0: {cyc / n / 1e6:.0f} Mcycles per row, {inst / n / 1e6:.0f} M host instructions (IPC {inst / cyc:.3f})')
    if gins:
        print(f'guest: {gins / n / 1e6:.2f} M instructions per row; per guest instruction {cyc / gins:.2f} cycles, {inst / gins:.2f} host instructions')
    parts = [('instruction fetch stall', s('stall_inst_m') * 1e6), ('data stall', s('stall_data_m') * 1e6)]
    rest = cyc - sum(p[1] for p in parts)
    print('cycle split (counter-measured stalls; the rest is issue + interlocks + mispredicts):')
    for name, c in parts + [('issue / other', rest)]:
        print(f'  {name:26s} {c / cyc * 100:5.1f}%  {c / n / 1e6:7.1f} M per row')
    ev = [('branches', 'branch_m', 1e6), ('branch mispredicts', 'brmiss_k', 1e3), ('I-cache misses', 'icmiss_k', 1e3),
          ('D-cache read misses', 'dcrmiss_k', 1e3), ('D-cache write misses', 'dcwmiss_k', 1e3), ('TLB misses', 'tlbmiss_k', 1e3)]
    print('events per row, per 1000 host instructions:')
    for name, k, m in ev:
        v = s(k) * m
        print(f'  {name:22s} {v / n / 1e6:8.3f} M  {v / inst * 1000:7.2f}')
    for k in ('jitcode_pct', 'compiles_s', 'icbi_flushes', 'slowld_k', 'slowst_k', 'native_pct', 'gxwait_pct', 'wait_pct'):
        if any(k in r for r in rs):
            print(f'  {k:22s} {s(k) / n:10.2f} per row (mean)')


main()
