#!/usr/bin/env python3
"""Check census totals, planted sensitivity, and exactness lines; no guest data required."""
import argparse
import copy
import re
from pathlib import Path

GROUPS = ('TEV', 'texture', 'XF_matrices', 'XF_lights', 'CP_arrays', 'other')


def report(path):
    lines = Path(path).read_text().splitlines()
    records = []
    total = None
    for line in lines:
        if not line.startswith(('field ', 'partial ', 'total ')):
            continue
        data = {}
        for key, value in re.findall(r'(\w+)=([\d/]+)', line):
            if '/' in value:
                same, writes = map(int, value.split('/'))
                data[key + '_same'], data[key + '_writes'] = same, writes
                assert same <= writes, (path, key)
            else:
                data[key] = int(value)
        for key in ('upper', 'lower', 'lastuse', 'activeupper', 'activelower'):
            assert data[key] <= data['lists'], (path, key)
        assert data['lhit'] <= data['lit'] and data['activelight'] <= data['lit'], path
        if line.startswith('total '):
            total = data
        else:
            records.append(data)
    assert total is not None and records, path
    assert all(sum(row[k] for row in records) == total[k] for k in total), path
    return total


def signature(path):
    text = Path(path).read_text()
    result = re.findall(r'^(?:state hash .+|OPMIX: \d+ instructions interpreted)$', text, re.M)
    assert len(result) == 2, (path, result)
    return result


def exact(a, b):
    assert a == b, (a, b)


def synthetic(data):
    expected = dict(TEV_same=3, lists=3, upper=2, lower=2, lhit=8,
                    lit=12, spec=12, sdk=24, direct=12, reads=0, ptrstores=0)
    for key, value in expected.items():
        assert data[key] == value, (key, data[key], value)


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--synthetic', nargs=2, metavar=('NORMAL', 'PLANT'))
    ap.add_argument('--runs', type=Path, help='directory with STATE-on/off/plant.{txt,log}')
    args = ap.parse_args()
    if args.synthetic:
        normal, plant = map(report, args.synthetic)
        synthetic(normal)
        try:
            synthetic(plant)
        except AssertionError:
            print('PASS: normal census oracle rejects planted input')
        else:
            raise AssertionError('plant did not fail normal oracle')
        for label, keys in [('a', ['TEV_same']), ('b', ['upper', 'lower']),
                            ('c', ['sdk', 'direct']), ('d', ['lhit']), ('e', ['reads', 'ptrstores'])]:
            assert all(normal[k] != plant[k] for k in keys), label
            print(f'PASS plant {label}: ' + ', '.join(f'{k} {normal[k]} -> {plant[k]}' for k in keys))
    if args.runs:
        for normalpath in sorted(args.runs.glob('*-on.txt')):
            state = normalpath.name.removesuffix('-on.txt')
            normal = report(normalpath)
            off = signature(args.runs / f'{state}-off.log')
            on = signature(args.runs / f'{state}-on.log')
            exact(off, on)
            bad = copy.copy(on)
            bad[0] += ' PLANTED'
            try:
                exact(off, bad)
            except AssertionError:
                pass
            else:
                raise AssertionError('exactness checker failed to reject planted state hash')
            bad = copy.copy(on)
            bad[1] += ' PLANTED'
            try:
                exact(off, bad)
            except AssertionError:
                pass
            else:
                raise AssertionError('instruction checker failed to reject planted count')
            plantpath = args.runs / f'{state}-plant.txt'
            if plantpath.exists():
                planted = report(plantpath)
                exact(off, signature(args.runs / f'{state}-plant.log'))
                assert sum(planted[g+'_same'] for g in GROUPS) == 0
                assert sum(normal[g+'_same'] for g in GROUPS) > 0
                assert planted['sdk'] == normal['direct'] and planted['direct'] == normal['sdk']
                assert planted['reads'] > normal['reads'] and planted['ptrstores'] > normal['ptrstores']
                for key in ('upper', 'lower', 'lhit', 'activeupper', 'activelower', 'activelight'):
                    assert planted[key] <= normal[key]
                print(f'PASS {state}: plant changes state/source/watch counters; repeats {normal["activeupper"]}/{normal["activelight"]} -> {planted["activeupper"]}/{planted["activelight"]}')
            print(f'PASS {state}: on/off state hash and interpreted instruction count; both negative checker plants rejected')


if __name__ == '__main__':
    main()
