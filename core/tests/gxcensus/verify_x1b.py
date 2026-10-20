#!/usr/bin/env python3
"""X1b report accounting, planted counter sensitivity and raw-interpreter exactness."""
import argparse
import json
import re
import tempfile
from pathlib import Path
from verify import report, signature, exact

EQUAL = {'BP_equal', 'CP_equal', 'XF_equal', 'IDX_equal'}


def pricing(path):
    rows, totals = {}, {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r'decode_(field|partial|total) (\d+) (\w+) (.*)', line)
        if not m:
            continue
        data = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', m[4])}
        assert data['samples'] <= data['commands'], (path, line)
        assert data['ticks'] <= data['raw'], (path, line)
        if m[1] == 'total':
            totals[m[3]] = data
        else:
            accum = rows.setdefault(m[3], {k: 0 for k in data})
            for k, v in data.items():
                accum[k] += v
    assert totals and totals == rows, path
    estimates, raw_estimates = {}, {}
    for name, data in totals.items():
        if not data['commands']:
            estimates[name] = raw_estimates[name] = 0
            continue
        assert data['samples'], (path, name, 'nonempty class without a timing sample')
        estimates[name] = data['commands'] * data['ticks'] / data['samples']
        raw_estimates[name] = data['commands'] * data['raw'] / data['samples']
    total = sum(estimates.values())
    equal = sum(estimates[k] for k in EQUAL)
    return dict(classes=totals, estimated_decode_ticks=total, estimated_equal_ticks=equal,
                equal_share=equal/total if total else 0,
                raw_equal_share=sum(raw_estimates[k] for k in EQUAL)/sum(raw_estimates.values()))


def validate(path):
    d = report(path)
    assert d['depactive'] <= d['lists'] and d['deplast'] <= d['lists'], path
    assert d['light1'] <= d['light2'] <= d['light4'] <= d['lit'], path
    return d


def synthetic_oracle(d, p):
    assert d['depactive'] >= 2 and d['deplast'] >= 2, 'dependency hits absent'
    assert d['light1'] >= 4 and d['light2'] >= 8 and d['light4'] >= 16, 'retained hits absent'
    assert all(p['classes'][k]['commands'] >= 4096 for k in EQUAL), 'unchanged commands absent'


def reject_accounting_plants(path):
    """An internally inconsistent report must fail both field-sum checkers."""
    text = Path(path).read_text()
    with tempfile.TemporaryDirectory() as directory:
        bad = Path(directory)/'bad.txt'
        for prefix, checker in [('total ', validate), ('decode_total ', pricing)]:
            lines = text.splitlines()
            index = next(i for i, line in enumerate(lines) if line.startswith(prefix))
            lines[index], changed = re.subn(r'(\w+=)(\d+)',
                lambda m: m[1]+str(int(m[2])+1), lines[index], count=1)
            assert changed, prefix
            bad.write_text('\n'.join(lines)+'\n')
            try:
                checker(bad)
            except AssertionError:
                print(f'PASS: {prefix.strip()} accounting rejects planted total')
            else:
                raise AssertionError(f'{prefix}checker accepted planted total')


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--synthetic', nargs=2)
    ap.add_argument('--runs', type=Path)
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    if args.synthetic:
        normal, plant = map(validate, args.synthetic)
        np, pp = map(pricing, args.synthetic)
        synthetic_oracle(normal, np)
        reject_accounting_plants(args.synthetic[0])
        try:
            synthetic_oracle(plant, pp)
        except AssertionError:
            print('PASS: normal X1b oracle rejects the planted report')
        else:
            raise AssertionError('plant failed to fail normal X1b oracle')
        assert all(pp['classes'][k]['commands'] == 0 for k in EQUAL)
        assert not plant['depactive'] and not plant['deplast']
        assert not plant['light1'] and not plant['light2'] and not plant['light4']
        for label, keys in [('decode', ['estimated_equal_ticks']), ('dependency', ['depactive', 'deplast']),
                            ('retention', ['light1', 'light2', 'light4'])]:
            a, b = (np, pp) if label == 'decode' else (normal, plant)
            assert all(a[k] > b[k] for k in keys), label
            print(f'PASS plant {label}: '+', '.join(f'{k} {a[k]} -> {b[k]}' for k in keys))
    results = {}
    if args.runs:
        paths = sorted(args.runs.glob('*-on.txt'))
        assert len(paths) == 4, paths
        for path in paths:
            state = path.name.removesuffix('-on.txt')
            d, p = validate(path), pricing(path)
            off = signature(args.runs/f'{state}-off.log')
            exact(off, signature(args.runs/f'{state}-on.log'))
            # Each equality checker is also exercised with an actual changed signature.
            for i in (0, 1):
                bad = off.copy(); bad[i] += ' PLANTED'
                try:
                    exact(off, bad)
                except AssertionError:
                    pass
                else:
                    raise AssertionError('exactness oracle accepted a planted signature')
            planted = validate(args.runs/f'{state}-plant.txt')
            pp = pricing(args.runs/f'{state}-plant.txt')
            exact(off, signature(args.runs/f'{state}-plant.log'))
            assert all(not pp['classes'][k]['commands'] for k in EQUAL)
            assert not planted['depactive'] and not planted['deplast']
            assert not planted['light1'] and not planted['light2'] and not planted['light4']
            results[state] = dict(counts=d, pricing=p, signature=off)
            results[state]['timing_repeats'] = [dict(run='on', pricing=p)]
            for directory in sorted(args.runs.glob('repeat*')):
                repeated = directory/f'{state}-on.txt'
                assert repeated.exists(), repeated
                exact(off, signature(directory/f'{state}-on.log'))
                repeated_counts = validate(repeated)
                exact(d, repeated_counts)
                bad = repeated_counts.copy(); bad['depactive'] += 1
                try:
                    exact(d, bad)
                except AssertionError:
                    pass
                else:
                    raise AssertionError('repeat counter checker accepted planted inconsistency')
                results[state]['timing_repeats'].append(dict(run=directory.name, pricing=pricing(repeated)))
            print(f'PASS {state}: exact off/on/plant, accounting, all three X1b plants; '
                  f'equal decode budget {100*p["equal_share"]:.2f}%, '
                  f'dependency {d["depactive"]}/{d["deplast"]} of {d["lists"]}, '
                  f'lighting {d["light1"]}/{d["light2"]}/{d["light4"]} of {d["lit"]}')
            print(f'PASS {state}: repeated on runs have identical counters/hash/instructions; '
                  'planted counter inconsistency rejected; budgets ' +
                  ', '.join(f'{100*x["pricing"]["equal_share"]:.2f}%' for x in results[state]['timing_repeats']))
    if args.json:
        args.json.write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()
