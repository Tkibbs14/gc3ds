#!/usr/bin/env python3
"""The GX side (core 2's GX thread, core 3's vertex worker) of a 2DS relay log, per A/B arm and stretch.

    python3 core/tools/gx_split.py core/out/relay_163.csv [--col opt] [--val 627]

Same row selection as ab_split.py (tp850: cutscene to 303 s, later stretch to 576 s, 4 s settle after A/B switches,
10% trimmed means). Prints the GX-thread and worker columns: busy shares, what the GX thread waits for, its per-draw
phases, draw / vertex rates and the GPU's time per frame.
"""
import sys
sys.argv = sys.argv[:2] + [a for a in sys.argv[2:]]
exec(open(__file__.replace("gx_split.py", "ab_split.py")).read().split("def main")[0])

COLS = ("speed_pct gxwait_pct gxthread_pct vwork_pct vwait_pct valloc_pct vqueue_pct vprep_pct vpal_pct gxvtx_pct "
        "gxstate_pct gxtex_pct gxsync_pct gxpres_pct gxend_pct vdec_pct vlight_pct draws_s verts_s gpu_calls_s "
        "gpu_splits_s c3d_cpu_ms c3d_gpu_ms texdec_s efbcopy_s").split()


def main():
    a = sys.argv[1:]
    path = a[0]
    col = a[a.index("--col") + 1] if "--col" in a else "opt"
    val = a[a.index("--val") + 1] if "--val" in a else None
    split, gap, end, skip, settle = 303.0, 8.0, 576.0, 25.0, 4.0
    rows, keep, last, start = load(path), [], None, 0.0
    for r in rows:
        t, v = num(r, "real_s"), r.get(col)
        if t is None or v is None:
            continue
        if v != last:
            start, last = t, v
        if t < skip or t - start < settle or abs(t - split) < gap or t >= end:
            continue
        if val is None or v == val:
            keep.append(r)
    for name, lo, hi in (("cutscene", skip, split), ("later", split, end)):
        part = [r for r in keep if lo <= num(r, "real_s") < hi]
        print(f"== {name} ({len(part)} rows{', ' + col + '=' + val if val else ''})")
        for c in COLS:
            print(f"  {c:14s} {stat([num(r, c) for r in part])}")


main()
