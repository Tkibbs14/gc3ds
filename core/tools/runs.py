#!/usr/bin/env python3
"""Score 2DS runs as separate whole runs (the way every default since 10-19 was decided).

    python3 core/tools/runs.py [--skip S] core/out/relay_<job>.csv ...

Per run: settled means (rows from S real seconds, default 50) of speed_pct and fields_s, real frames a second (the
'# new frames 10s' lines after S, weighted by their real length) and the VI fields between XFB copies summed from the
'# new frames hist' lines (TP's gameplay is locked at 2 fields a picture: a change that adds 1- or 3-field gaps changed
the pacing, not only the speed).

**Rows where core 0 did not get the whole second are dropped** (`--keep-stolen` keeps them). The program takes a
screenshot every 30 perf rows (~33 s), and writing the BMP to the SD card - and shipping the frame to the relay on
a relay job - steals core 0 for part of that second: measured between 4% and 86% of it, 5 times in a 240 s run.
How many of the five land on the severe end varies run to run, and that alone moved a run's mean by up to 2.5%,
which read as session drift and voided a whole afternoon's comparisons on 2026-10-30 (jobs 1596-1607). Scored
without those rows, three identical reference runs agree to 0.13% instead of 2.1%. The test is the PMU's own cycle
count (`pmu_mcyc` below 800 M/s means core 0 lost time); with the PMU off, the row covering a `# shot` is dropped. Gameplay scenes: the heavy Ordon area (tp_area2.state, no presses: two runs of one
build agree to ~0.1%) and the scripted Ordon walk (tp_ordon.state with the --press list in REMOTE_RUNS.md). For the
title (tp850.state) use title_sections.py: its sections differ too much to pool.

Each line ends with the build's commit and the **launch memory state** (`heap N MB`, from the log's
'# heap: largest malloc block'). **The 2DS launches into one of several memory states and one of them is
~5% slow** (C55, 2026-11-01). Five values appear across the logs - 2.9, 7.1, 7.2, 8.6 and 9.0 MB - and at identical
defaults on the same commit static Ordon reads **10.030 / 10.020** with `heap 9.0 MB` against **10.587 / 10.592**
with `heap 7.1 MB`. The number itself predicts nothing (2.9 MB, the historical majority, performs like 7.1 despite
having the least free heap): it is a *label* for the state, and which labels are slow has to be measured per label.
The mechanism is unknown; C55 lists the hypotheses already killed. **Runs in different states are not
comparable whatever else matches**, so scoring several logs together prints a loud warning when they do not share
one; group by the heap column and compare only within a group. This is the check that would have saved an hour on
2026-11-01, when a device step became a "-5.6% regression" and was bisected across eight commits.
"""
import re
import sys


# A row is dropped only when core 0 got less than this share of the second - a steal, not a tax. The screenshot's
# severe steals leave core 0 at 14% of its rate (116 of 804 M cycles/s) while its mild ones leave 96%, and a busy
# scene that streams from disc sits at 83-99% all the way through (tp850: 635-804, continuous). Five severe steals
# are the whole 2.9% contamination; the mild ones are 0.1%. An absolute threshold (the first version used 800 M/s)
# dropped half of a title run for a few percent each and kept only its easiest seconds, which is worse than the
# contamination it removed: it selects the rows that flatter the build.
STOLEN_SHARE = 0.5


def score(path, skip, keep_stolen=False):
    """Scored rows are those where core 0 had the whole core: see `stolen` in the docstring."""
    hdr = None
    sp, fs, fr, fh = [], [], [], {}
    stolen = 0
    dropped_windows = 0
    shots = []        # '# shot' times: the window they fall in lost pictures to the screenshot
    rows = []         # (real_s, pmu_mcyc or None, speed_pct, fields_s, right after a shot)
    
    t = 0.0
    shot_at = None
    low_card = False
    rev = ''          # '# build: gc3ds <commit>': which source produced this log (host3ds/build.sh writes it)
    # '# heap: largest malloc block N MB' - the app's launch memory state, and the 2DS has more than one of them.
    # Measured 2026-11-01 (C55): at identical defaults and on the same commit, static Ordon reads ~32.96% with
    # 9.0 MB and ~34.6-34.9% with 7.1 MB, a ~5% step, and the correlation is exact over every plain Ordon run on
    # disk. In the 9.0 state the app has 1.9 MB *more* free heap, i.e. an allocation that size did not happen. So
    # two runs with different values here are not comparable, whatever else matches, and the arms of an A/B that
    # straddle the step are measuring the state and not the change.
    heap = ''
    # A log holding more than one run. `relay.sh log` appends to an existing file, so fetching the same job twice
    # leaves two runs' rows in one log - job 1887 on 2026-11-01 read 24 windows where every other run read 12, and
    # scored identically only because the two copies were the same run. Two *different* runs concatenated would
    # score as one and silently average them, so the second header is an error, not a note.
    revs, nbuild = [], 0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# heap: largest malloc block '):
            heap = line.split('block', 1)[1].strip()
            continue
        if line.startswith('# build: gc3ds '):
            rev = line.split('gc3ds', 1)[1].strip()
            nbuild += 1          # a single run writes this header exactly once
            if rev not in revs:
                revs.append(rev)
            continue
        if line.startswith('# card:') and 'LOW' in line:
            low_card = True      # a full card truncates runs and fails build downloads (2026-10-30)
            continue
        if line.startswith('# shot '):
            shot_at = t      # the row after this one is the second the screenshot was taken in
            shots.append(t)
            continue
        if line.startswith('# new frames 10s:'):
            m = re.match(r'# new frames 10s: ([0-9.]+)/s over ([0-9.]+) s', line)
            if m and t >= skip:
                # A window that contains a stolen second is contaminated: the pictures missing from that second are
                # the measurement's own, not the build's. Drop the whole window rather than correct it.
                length = float(m.group(2))
                if keep_stolen or not any(t - length <= b <= t for b in shots):
                    fr.append((float(m.group(1)), length))
                else:
                    dropped_windows += 1
            continue
        if line.startswith('# new frames hist'):
            if t >= skip and 'VI fields between copies:' in line:
                for k, v in re.findall(r'(\d+):(\d+)', line.split('VI fields between copies:')[1]):
                    fh[int(k)] = fh.get(int(k), 0) + int(v)
            continue
        if line.startswith('#'):
            continue
        r = line.rstrip('\n').split(',')
        if hdr is None:
            if 'speed_pct' in r:
                hdr = r
            continue
        if len(r) != len(hdr):
            continue
        d = dict(zip(hdr, r))
        try:
            t = float(d['real_s'])
            if t < skip:
                continue
            # Drop a row where core 0 did not get the whole second. The PMU's own cycle count says so directly; with
            # the PMU off, the row covering a screenshot is the one to drop (that is what steals it).
            mcyc = d.get('pmu_mcyc') or ''
            rows.append((t, float(mcyc) if mcyc else None, float(d['speed_pct']), float(d['fields_s']),
                         shot_at is not None and t - shot_at < 1.5))
        except (ValueError, KeyError):
            pass
    # The median rate is this run's own normal; anything far below it is a second core 0 did not get.
    have = sorted(r[1] for r in rows if r[1] is not None)
    med = have[len(have) // 2] if have else None
    for t, mcyc, speed, fields, after_shot in rows:
        if not keep_stolen:
            if med is not None and mcyc is not None:
                if mcyc < med * STOLEN_SHARE:
                    stolen += 1
                    continue
            elif after_shot:          # no PMU in this run: the row covering a screenshot is the one that lost time
                stolen += 1
                continue
        sp.append(speed)
        fs.append(fields)
    f = sum(a * b for a, b in fr) / sum(b for a, b in fr) if fr else 0.0
    score.heap_seen = getattr(score, 'heap_seen', {})
    score.heap_seen[path] = heap
    return '%-22s speed %.3f fields/s %.3f frames/s %.3f (%d windows) fields per copy %s%s' % (
        path.replace('\\', '/').split('/')[-1], sum(sp) / max(len(sp), 1), sum(fs) / max(len(fs), 1), f, len(fr),
        ' '.join('%d:%d' % kv for kv in sorted(fh.items())),
        ('' if not (stolen or dropped_windows) else
         '  [dropped %d row(s) and %d frame window(s): core 0 lost the second]' % (stolen, dropped_windows))
        + ('' if not low_card else '  *** the card was LOW on space: this run may be truncated ***')
        # The commit, when the build embedded one. 'dirty' is called out: such a run cannot be tied to source, so it
        # is not reproducible and must not be quoted as a measurement.
        + ('' if not rev else ('  [%s]' % rev if 'dirty' not in rev else
                               '  *** build %s: uncommitted changes, this run cannot be tied to source ***' % rev))
        + ('' if not heap else '  heap %s' % heap)
        # One log, two runs (see the `revs`/`heaps` comment above). Loud, because the scores it prints are an
        # average of runs the reader thinks is one run.
        # Counted, not de-duplicated: two runs of the *same* build concatenated is the dangerous case and the
        # one that actually happened, and distinct values would not have seen it.
        + ('' if nbuild < 2 else
           '  *** THIS LOG HOLDS %d RUNS (builds %s): delete it and fetch the job again ***'
           % (nbuild, '/'.join(revs) or '?')))


def selftest():
    """A synthetic log with a known steal, so the filter is checked rather than trusted. Each case plants a fault
    the filter must or must not react to; a check whose planted fault passes proves nothing."""
    import os
    import tempfile

    def write(path, rows, shots, med=803.0, heap=None, rev=None):
        with open(path, 'w') as f:
            f.write('# run synthetic\n')
            if rev:
                f.write('# build: gc3ds %s\n' % rev)
            if heap:
                f.write('# heap: largest malloc block %s MB\n' % heap)
            f.write('real_s,emu_s,speed_pct,fields_s,pmu_mcyc\n')
            for i, (sp, mc) in enumerate(rows):
                t = 81.0 + i
                if t in shots:
                    f.write('# shot /x.bmp at real %.1f s, emulated 0.00 s\n' % t)
                f.write('%.1f,0,%.3f,%.3f,%.2f\n' % (t, sp, sp * 0.586, mc))
                if (i + 1) % 10 == 0:
                    f.write('# new frames 10s: %.2f/s over 10.00 s, gap mean 0 ms, sd 0 ms, largest 0 ms\n'
                            % (sum(r[0] for r in rows[i - 9:i + 1]) / 10.0 * 0.293))
                    f.write('# new frames hist 10s: real gap (bins of 1/240 s: count) 32:10; '
                            'VI fields between copies: 2:10\n')
    d = tempfile.mkdtemp()
    bad = 0
    # 1. 40 clean rows at speed 24, with four seconds where core 0 got 14% of the core (a severe steal). The raw
    #    mean is dragged down; the filter must recover 24.000 and say it dropped four rows.
    rows = [(24.0, 803.0)] * 40
    for i in (5, 15, 25, 35):
        rows[i] = (3.2, 116.0)
    p1 = os.path.join(d, 'relay_steal.csv')
    write(p1, rows, shots=set())
    raw = score(p1, 80, keep_stolen=True)
    filt = score(p1, 80)
    raw_sp = float(raw.split('speed ')[1].split()[0])
    filt_sp = float(filt.split('speed ')[1].split()[0])
    if not (raw_sp < 22.0):
        print('selftest: the planted steal did not move the raw mean (%.3f)' % raw_sp); bad += 1
    if abs(filt_sp - 24.0) > 0.01:
        print('selftest: filtered mean %.3f, expected 24.000' % filt_sp); bad += 1
    if 'dropped 4 row(s)' not in filt:
        print('selftest: did not report four stolen rows: %s' % filt); bad += 1
    # 2. A busy scene that runs 15% below its own median all the way through is NOT stolen time - it is the
    #    workload (tp850 streams from disc at 635-804 M/s). Nothing may be dropped, or scoring selects the
    #    easiest seconds and flatters the build.
    rows = [(24.0 if i % 2 else 20.0, 803.0 if i % 2 else 690.0) for i in range(40)]
    p2 = os.path.join(d, 'relay_busy.csv')
    write(p2, rows, shots=set())
    out = score(p2, 80)
    if 'dropped' in out:
        print('selftest: dropped rows from a merely busy scene: %s' % out); bad += 1
    # 3. A frame window containing a screenshot must go, because the pictures it lost are the measurement's own.
    rows = [(24.0, 803.0)] * 40
    p3 = os.path.join(d, 'relay_shot.csv')
    write(p3, rows, shots={90.0})
    out = score(p3, 80)
    if 'frame window(s)' not in out:
        print('selftest: a window with a screenshot in it was kept: %s' % out); bad += 1
    if '(3 windows)' not in out:
        print('selftest: expected 3 of 4 windows kept: %s' % out); bad += 1
    # 4. The launch memory state (C55): two logs from different states must be called out when scored together,
    #    and two from the same state must not be. The planted fault is the pair that differs - without the warning
    #    a 5% device step reads as a 5% result, which is exactly what happened on 2026-11-01.
    import io
    import contextlib

    def scored_together(a, b):
        score.heap_seen = {}
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            sys.argv = ['runs.py', a, b, '--skip', '80']
            main()
        return buf.getvalue()

    rows = [(24.0, 803.0)] * 40
    pa = os.path.join(d, 'relay_heap_a.csv')
    pb = os.path.join(d, 'relay_heap_b.csv')
    pc = os.path.join(d, 'relay_heap_c.csv')
    write(pa, rows, shots=set(), heap='7.1')
    write(pb, rows, shots=set(), heap='9.0')
    write(pc, rows, shots=set(), heap='7.1')
    if 'DIFFERENT launch memory states' not in scored_together(pa, pb):
        print('selftest: two logs from different memory states were not called out'); bad += 1
    if 'DIFFERENT launch memory states' in scored_together(pa, pc):
        print('selftest: two logs from the SAME memory state were wrongly called out'); bad += 1
    if 'heap 7.1 MB' not in score(pa, 80):
        print('selftest: the heap state is missing from a log that reports one'); bad += 1

    # Two runs concatenated into one log, same build - what `relay.sh log` does when a log already exists, and
    # what job 1887 did on 2026-11-01. The planted fault is the dangerous one: identical builds, so de-duplicating
    # the header values would not see it.
    pd_ = os.path.join(d, 'relay_two_runs.csv')
    p1 = os.path.join(d, 'relay_one_run.csv')
    write(p1, rows, shots=set(), heap='7.1', rev='abc123abc123')
    with open(pd_, 'w') as f:
        f.write(open(p1).read())
        f.write(open(p1).read())
    if 'HOLDS 2 RUNS' not in score(pd_, 80):
        print('selftest: a log holding two runs of the same build was not called out'); bad += 1
    if 'HOLDS' in score(p1, 80):
        print('selftest: a single-run log was wrongly called a concatenation'); bad += 1

    print('runs.py selftest: %s' % ('FAILED' if bad else 'ok (steal removed, busy scene kept, shot window dropped, '
                                                        'mixed memory states called out, concatenated log caught)'))
    return 1 if bad else 0


def main():
    args = sys.argv[1:]
    if '--selftest' in args:
        return selftest()
    skip = 50.0
    keep = False
    files = []
    while args:   # --skip anywhere, not only first: `runs.py LOG LOG --skip 80` used to read "--skip" as a file
        if args[0] == '--skip':
            skip = float(args[1])
            args = args[2:]
        elif args[0] == '--keep-stolen':
            keep = True
            args = args[1:]
        else:
            files.append(args[0])
            args = args[1:]
    args = files
    if not args:
        print(__doc__)
        return
    for p in args:
        print(score(p, skip, keep))
    # The launch memory state, loudly, when the runs being compared do not share it (C55). Scoring several logs
    # together *is* the comparison, so this is the place to say it rather than leaving it to be noticed.
    seen = getattr(score, 'heap_seen', {})
    states = sorted({v for v in seen.values() if v})
    if len(states) > 1:
        print('*** these runs are in DIFFERENT launch memory states (%s): not comparable, whatever else matches.'
              % ', '.join(states))
        print('*** C55: at identical defaults the 2DS reads ~5% slower in the larger-free-heap state. Group the')
        print('*** logs by the "heap" column and compare only within a group.')
        for k, v in seen.items():
            if v:
                print('***   %-22s heap %s' % (k.replace(chr(92), '/').split('/')[-1], v))
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
