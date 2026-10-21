#!/usr/bin/env python3
"""X3 qemu whole-game gate. Reads disc/states in place, writes no game material into the repository."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
MODES = (('off', 0), ('gprinline', 1), ('zgpr', 2), ('zgx', 4), ('zxyz', 8))


def signature(record):
    return record['instructions'], record['frame_sha256']


def exact(a, b):
    assert a == b, (a, b)


def run_one(out, state, end, mode, bits):
    name = f'{state}-{end}-{mode}'
    directory = out/name
    directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in ('GC3DS_NOHLE', 'HLE_FAST', 'HLE_CHECK', 'HLE_FAST_PLANT', 'GC3DS_NODRAW', 'AOT',
                'GXCENSUS', 'GC3DS_PORT', 'GC3DS_NOFAULTMEM'):
        env.pop(key, None)
    env.update(GC3DS_X3=str(bits), GC3DS_JIT_NOTRACE='1', GC3DS_JIT_MB='16', GC3DS_OPT='0')
    command = ['qemu-arm', '-L', '/usr/arm-linux-gnueabihf', '-cpu', 'cortex-a7',
               str(ROOT/'core/build-arm/gcemu'), env['GC3DS_ISO'], '--load-state',
               f'/root/tp_arm_{state}.state', '--frames', str(end), '--jit', '--out', str(directory)]
    log = out/f'{name}.log'
    with log.open('w') as stream:
        subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    text = log.read_text()
    counts = re.findall(r'^JIT: (\d+) instructions run,', text, re.M)
    assert len(counts) == 1, log
    assert f', {end} fields)' in text, (log, 'run did not reach requested field')
    record = dict(state=state, end=end, mode=mode, instructions=int(counts[0]),
                  frame_sha256=hashlib.sha256((directory/'last.png').read_bytes()).hexdigest(),
                  counts=re.findall(r'^JIT_X3 .+$', text, re.M),
                  state_hash=re.findall(r'^state hash .+$', text, re.M))
    print(f'{name}: measured {record["instructions"]} instructions; frame {record["frame_sha256"]}; '
          f'{record["counts"]}', flush=True)
    return record


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--out', type=Path, default=Path('/root/x3/game'))
    ap.add_argument('--states', nargs='+', default=['950:970', '850:854'])
    ap.add_argument('--jobs', type=int, default=3)
    args = ap.parse_args()
    out = args.out.resolve()
    assert out != ROOT and ROOT not in out.parents
    out.mkdir(parents=True, exist_ok=True)
    states = [tuple(map(int, pair.split(':'))) for pair in args.states]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        work = [pool.submit(run_one, out, state, end, mode, bits)
                for state, end in states for mode, bits in MODES]
        records = [future.result() for future in work]
    (out/'results.json').write_text(json.dumps(records, indent=2)+'\n')
    for state, end in states:
        selected = [record for record in records if record['state'] == state and record['end'] == end]
        baseline = signature(selected[0])
        for i in (0, 1):
            planted = list(baseline); planted[i] = str(planted[i])+' PLANT'
            try:
                exact(baseline, tuple(planted))
            except AssertionError:
                print(f'PASS {state}: whole-game checker rejects planted '+('count' if i == 0 else 'frame'))
            else:
                raise AssertionError('whole-game checker accepted its plant')
        for record in selected[1:]:
            exact(baseline, signature(record))
            print(f'PASS {state}->{end} {record["mode"]}: exact instruction count and last frame')


if __name__ == '__main__':
    main()
