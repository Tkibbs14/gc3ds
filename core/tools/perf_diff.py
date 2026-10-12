#!/usr/bin/env python3
"""Compare the heavy-row averages of two perf.csv runs (or A/B halves) column by column.

    python3 core/tools/perf_diff.py A.csv[:COL=VAL] B.csv[:COL=VAL] [--min-draws 1000] [--all]

Prints every numeric column whose average differs by more than 5% (all of them with --all), largest change first.
A ':COL=VAL' suffix keeps only the rows of that A/B phase (e.g. relay_52.csv:merge=1).
"""
import sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from perf_heavy import rows, num


def load(spec, min_draws):
    path, _, sel = spec.partition(":")
    col, _, val = sel.partition("=")
    rs = [r for r in rows(path) if (num(r, "draws_s") or 0) > min_draws and (not col or r.get(col) == val)]
    avg = {}
    for k in (rs[0].keys() if rs else []):
        xs = [num(r, k) for r in rs]
        xs = [x for x in xs if x is not None]
        if xs:
            avg[k] = sum(xs) / len(xs)
    return len(rs), avg


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    min_draws = 1000.0
    if "--min-draws" in sys.argv:
        min_draws = float(sys.argv[sys.argv.index("--min-draws") + 1])
        args = [a for a in args if a != str(sys.argv[sys.argv.index("--min-draws") + 1])]
    show_all = "--all" in sys.argv
    na, a = load(args[0], min_draws)
    nb, b = load(args[1], min_draws)
    print(f"A {args[0]}: {na} heavy rows; B {args[1]}: {nb} heavy rows")
    out = []
    for k in a:
        if k not in b:
            continue
        x, y = a[k], b[k]
        rel = (y - x) / abs(x) if x else (0.0 if y == 0 else float("inf"))
        if show_all or abs(rel) > 0.05:
            out.append((abs(rel), k, x, y, rel))
    for _, k, x, y, rel in sorted(out, reverse=True):
        print(f"  {k:16s} {x:12.4g} -> {y:12.4g}  ({rel * 100:+.1f}%)")


if __name__ == "__main__":
    main()
