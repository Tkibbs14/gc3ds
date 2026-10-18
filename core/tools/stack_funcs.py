#!/usr/bin/env python3
"""Inclusive guest profile of a 2DS run (--profile on): the '# guest stacks 10s' lines mapped to calling functions.

    python3 core/tools/stack_funcs.py core/out/relay_374.csv SYMBOLS.txt [--top 60] [--skip 60]

Each sample walked the guest stack and counted every return address on it once; a return address lies in the function
that made the call, so a function's share here is the share of samples taken somewhere below it: its whole call tree
(inclusive time), which says which subsystem is worth running natively as one piece. Shares are averaged over the
reports after --skip seconds. A function that called two different children in one sample (recursion through two call
sites) is counted twice, so a share can exceed its true value there; the leaf's own time is in '# hot guest code'.
SYMBOLS.txt: zeldaret/tp config/GZ2E01/symbols.txt (outside the repository), as hot_funcs.py takes.
"""
import bisect, re, sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from hot_funcs import load_symbols  # noqa: E402


def main():
    args = sys.argv[1:]
    top, skip = 60, 60.0
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    if '--skip' in args:
        i = args.index('--skip'); skip = float(args[i + 1]); del args[i:i + 2]
    path, symfile = args[0], args[1]
    syms = load_symbols(symfile)
    starts = [s[0] for s in syms]

    def func(a):
        k = bisect.bisect_right(starts, a) - 1
        if k >= 0 and a < syms[k][0] + syms[k][1]:
            return syms[k][2]
        return '?%08x' % a

    reports, t = [], 0.0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line[:1].isdigit():
            try:
                t = float(line.split(',', 1)[0])
            except ValueError:
                pass
        if not line.startswith('# guest stacks 10s') or t < skip:
            continue
        body = line.split(':', 1)[1].split()
        per = {}
        for k in range(len(body)):
            if body[k].endswith('%') and k > 0 and re.fullmatch(r'[0-9a-f]{8}', body[k - 1]):
                f = func(int(body[k - 1], 16))
                per[f] = per.get(f, 0.0) + float(body[k][:-1])
        reports.append(per)
    if not reports:
        print('no "# guest stacks" reports after %.0f s (run with --profile on)' % skip)
        return
    tot = {}
    for per in reports:
        for f, v in per.items():
            tot[f] = tot.get(f, 0.0) + v / len(reports)
    print('%d reports after %.0f s; inclusive share of all core-0 samples (callers only)' % (len(reports), skip))
    for f, v in sorted(tot.items(), key=lambda x: -x[1])[:top]:
        print('  %6.2f%%  %s' % (v, f))


if __name__ == '__main__':
    main()
