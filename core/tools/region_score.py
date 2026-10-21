#!/usr/bin/env python3
"""X4: score captured-call PMU batches, subtracting both reset and empty-call controls.

python3 core/tools/region_score.py core/out/relay_JOB.csv [...] --json /external/result.json
Each repeat supplies two PMU intervals per arm: pair 0 instructions, pair 1 I-cache misses.
Full-call rows remove reset only. Body rows remove the matching empty-call arm, including reset and host entry.
"""
import argparse
import json
from pathlib import Path
import re
import statistics as st

ARMS = ("jit", "region", "reset", "jit_empty", "region_empty")
PAT = re.compile(r"# regionbench sample rep (\d+) arm (\w+) pair (\d+) cycles ([\d.]+) "
                 r"ev0 ([\d.]+) ev1 ([\d.]+) calls (\d+)")


def estimate(values):
    return {"mean": st.mean(values), "se": st.stdev(values) / len(values)**0.5 if len(values) > 1 else 0,
            "n": len(values)}


def score(text):
    if "# regionbench check oracle PASS jit PASS plant CAUGHT" not in text:
        raise ValueError("real-call comparison/plant did not pass")
    if "# regionbench empty controls PASS plant CAUGHT" not in text:
        raise ValueError("empty-call comparison/plant did not pass")
    if "FAILED" in text or "# regionbench timed compiles 0;" not in text:
        raise ValueError("incomplete, failed, or compilation-contaminated timing")
    data = {}
    for match in PAT.finditer(text):
        rep, arm, pair, cyc, ev0, ev1, calls = match.groups()
        key = (int(rep), arm, int(pair))
        if key in data or int(calls) != 256:
            raise ValueError("duplicate batch or unexpected call count")
        data[key] = tuple(map(float, (cyc, ev0, ev1)))
    reps = sorted({r for r, _, _ in data})
    if len(reps) != 7 or len(data) != 70:
        raise ValueError("expected all seven repeats, five arms, two PMU pairs")
    out = {"kind": "derived from measured device PMU batches", "repeats": len(reps)}
    for arm in ARMS:
        out[arm] = {
            "cycles": estimate([st.mean(data[r, arm, p][0] for p in (0, 1)) for r in reps]),
            "instructions": estimate([data[r, arm, 0][1] for r in reps]),
            "imisses": estimate([data[r, arm, 1][1] for r in reps]),
        }
    for mode in ("full_call", "body"):
        out[mode] = {}
        for arm in ("jit", "region"):
            control = "reset" if mode == "full_call" else arm + "_empty"
            rows = {"cycles": [], "instructions": [], "imisses": []}
            for r in reps:
                rows["cycles"].append(st.mean(data[r, arm, p][0] - data[r, control, p][0] for p in (0, 1)))
                rows["instructions"].append(data[r, arm, 0][1] - data[r, control, 0][1])
                rows["imisses"].append(data[r, arm, 1][1] - data[r, control, 1][1])
            out[mode][arm] = {k: estimate(v) for k, v in rows.items()}
        ratios = []
        for r in reps:
            def cycles(arm):
                control = "reset" if mode == "full_call" else arm + "_empty"
                return st.mean(data[r, arm, p][0] - data[r, control, p][0] for p in (0, 1))
            if cycles("jit") <= 0 or cycles("region") <= 0:
                raise ValueError("nonpositive cost after control subtraction")
            ratios.append(cycles("jit") / cycles("region"))
        out[mode]["speedup"] = estimate(ratios)
    return out


def self_test():
    lines = ["# regionbench check oracle PASS jit PASS plant CAUGHT callouts 0",
             "# regionbench empty controls PASS plant CAUGHT", "# regionbench timed compiles 0;"]
    for rep in range(7):
        for arm, value in zip(ARMS, (500, 300, 100, 150, 100)):
            for pair in range(2):
                lines.append(f"# regionbench sample rep {rep} arm {arm} pair {pair} "
                             f"cycles {value} ev0 {value} ev1 0 calls 256")
    good = "\n".join(lines)
    assert score(good)["body"]["speedup"]["mean"] == 1.75
    for planted in (good.replace("oracle PASS", "oracle FAIL"), good.replace("plant CAUGHT", "plant MISSED"),
                    good.replace("compiles 0;", "compiles 1;"), "\n".join(lines[:-1])):
        try:
            score(planted)
        except ValueError:
            continue
        raise AssertionError("planted invalid evidence accepted")
    print("region_score self-test: known ratio correct; four planted invalid reports rejected")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
    result = {}
    for name in args.logs:
        result[name] = row = score(Path(name).read_text())
        print(name)
        for mode in ("full_call", "body"):
            for arm in ("jit", "region"):
                values = row[mode][arm]
                print(f"  {mode} {arm}: " + ", ".join(
                    f"{k} {v['mean']:.3f} +- {v['se']:.3f}" for k, v in values.items()))
            speed = row[mode]["speedup"]
            print(f"  {mode} speedup: {speed['mean']:.4f} +- {speed['se']:.4f}x")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
