#!/usr/bin/env python3
"""Check the X3 operand variants through HLE_CHECK on title states; forced stops and a failing oracle plant too."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def run_one(out, state, end, suffix='', **extra):
    name = f'{state}-{end}{suffix}'
    directory = out/name
    directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in ('GC3DS_NOHLE', 'AOT', 'GXCENSUS', 'HLE_STOP_EVERY', 'HLE_FAST_PLANT'):
        env.pop(key, None)
    env.update(HLE_CHECK='1', HLE_FAST='7', GC3DS_NODRAW='1', **extra)
    log = out/f'{name}.log'
    with log.open('w') as stream:
        subprocess.run([str(ROOT/'core/build/gcemu'), env['GC3DS_ISO'], '--load-state',
                        f'/root/tp_pc_{state}.state', '--frames', str(end), '--out', str(directory)],
                       cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    text = log.read_text()
    found = re.findall(r'^HLE_CHECK \(PS library\): (\d+) stand-in calls compared with the interpreter, (\d+) differed \((\d+) stopped inside\)', text, re.M)
    assert len(found) == 1, log
    calls, bad, mid = map(int, found[0])
    assert calls, log
    if suffix == '-plant':
        assert bad > 0, 'HLE checker failed to reject planted result'
        verdict = 'PASS: planted result rejected'
    else:
        assert bad == 0, (log, bad)
        assert not re.search(r'compared with the interpreter, [1-9]\d* differed', text), log
        if suffix == '-stops':
            assert mid > 0, 'forced-stop run did not stop inside any callee'
        verdict = 'PASS'
    fast = re.findall(r'^HLE_FAST .+$', text, re.M)
    print(f'{verdict} {name}: measured calls={calls} differed={bad} partial={mid}; {fast}', flush=True)


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--out', type=Path, default=Path('/root/x3/hle'))
    args = ap.parse_args()
    out = args.out.resolve()
    assert out != ROOT and ROOT not in out.parents
    out.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=2) as pool:
        work = [pool.submit(run_one, out, state, end) for state, end in
                ((4290,4490),(5000,5200),(900,1100),(6590,6790))]
        for future in work:
            future.result()
    run_one(out, 5000, 5020, '-stops', HLE_STOP_EVERY='13')
    run_one(out, 5000, 5002, '-plant', HLE_FAST_PLANT='1')


if __name__ == '__main__':
    main()
