#!/usr/bin/env python3
"""Sequential X1b title runs. Reads the user's disc/states in place; keeps all outputs outside the repository."""
import argparse
import os
from pathlib import Path
import subprocess
from verify import exact, signature
from verify_x1b import validate, pricing

STATES = ((4290, 4490), (5000, 5200), (900, 1100), (6590, 6790))


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--out', type=Path, default=Path('/root/gxcensus-x1b/runs'))
    ap.add_argument('--state-dir', type=Path, default=Path('/root'))
    ap.add_argument('--repeats', type=int, default=3)
    args = ap.parse_args()
    assert args.repeats >= 1
    iso = os.environ['GC3DS_ISO']
    assert Path(iso).is_file(), iso
    root = Path(__file__).resolve().parents[3]
    out = args.out.resolve()
    assert out != root and root not in out.parents, 'keep run output outside the repo'
    subprocess.run(['make', '-C', 'core'], cwd=root, check=True)
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in ('GXCENSUS', 'GXCENSUS_PLANT', 'AOT', 'AOT_CHECK', 'HLE_CHECK', 'GC3DS_PORT', 'PORT_CHECK'):
        env.pop(key, None)
    env.update(GC3DS_NOHLE='1', GC3DS_NODRAW='1', OPMIX='1')
    for state, end in STATES:
        statefile = args.state_dir/f'tp_pc_{state}.state'
        assert statefile.is_file(), statefile
        baseline = None
        for mode, repeat in [('off', 1), ('on', 1), ('plant', 1)] + [('on', i) for i in range(2, args.repeats+1)]:
            directory = out if repeat == 1 else out/f'repeat{repeat}'
            directory.mkdir(parents=True, exist_ok=True)
            name = f'{state}-{mode}'
            child_env = env.copy()
            if mode != 'off':
                child_env['GXCENSUS'] = str(directory/f'{name}.txt')
            if mode == 'plant':
                child_env['GXCENSUS_PLANT'] = '1'
            log = directory/f'{name}.log'
            with log.open('w') as f:
                subprocess.run([str(root/'core/build/gcemu'), iso, '--load-state', str(statefile),
                                '--frames', str(end), '--out', str(directory/name)],
                               cwd=root, env=child_env, stdout=f, stderr=subprocess.STDOUT, check=True)
            sig = signature(log)
            if baseline is None:
                baseline = sig
            else:
                exact(baseline, sig)
            if mode != 'off':
                d = validate(directory/f'{name}.txt')
                p = pricing(directory/f'{name}.txt')
                print(f'{state}->{end} {mode} repeat={repeat}: exact; '
                      f'equal decode budget {100*p["equal_share"]:.2f}%; '
                      f'dep {d["depactive"]}/{d["deplast"]} of {d["lists"]}; '
                      f'light {d["light1"]}/{d["light2"]}/{d["light4"]} of {d["lit"]}', flush=True)
            else:
                print(f'{state}->{end} off: reference captured', flush=True)


if __name__ == '__main__':
    main()
