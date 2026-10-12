#!/usr/bin/env python3
"""Group a 2DS run's sampled guest blocks (perf.csv '# hot guest code' lines, run with --profile on) by function.

    python3 core/tools/hot_funcs.py core/out/relay_72.csv SYMBOLS.txt [--top 40]

SYMBOLS.txt: a decomp symbol map with lines like
    name = .text:0x8007F9A4; // type:function size:0x5A0 ...
(zeldaret/tp config/GZ2E01/symbols.txt for Twilight Princess, kept outside the repository). Shares are of all samples,
summed over every '# hot guest code' line in the file (only the blocks each line lists).
"""
import re, sys, bisect


def load_symbols(path):
    syms = []
    pat = re.compile(r'^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.match(line)
        if m:
            syms.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    syms.sort()
    return syms


def main():
    args = sys.argv[1:]
    top = 40
    if '--top' in args:
        i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
    csv, symfile = args[0], args[1]
    syms = load_symbols(symfile)
    starts = [s[0] for s in syms]
    funcs, total, outside, lines = {}, 0.0, 0.0, 0
    for line in open(csv, encoding='utf-8', errors='replace'):
        if not line.startswith('# hot guest code'):
            continue
        lines += 1
        m = re.search(r'outside compiled code ([0-9.]+)%;', line)
        if m:
            outside += float(m.group(1))
        for pc, share in re.findall(r' ([0-9a-f]{8}) ([0-9.]+)%', line):
            pc, share = int(pc, 16), float(share)
            k = bisect.bisect_right(starts, pc) - 1
            name = '?'
            if k >= 0 and syms[k][0] <= pc < syms[k][0] + max(syms[k][1], 4):
                name = syms[k][2]
            funcs[name] = funcs.get(name, 0.0) + share
            total += share
    if not lines:
        print('no "# hot guest code" lines'); return
    print(f'{lines} sample reports; listed blocks cover {total / lines:.1f}% of samples, outside compiled code '
          f'{outside / lines:.1f}% (averages per report)')
    for name, share in sorted(funcs.items(), key=lambda x: -x[1])[:top]:
        print(f'  {share / lines:6.2f}%  {name}')


if __name__ == '__main__':
    main()
