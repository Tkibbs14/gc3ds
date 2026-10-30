#!/usr/bin/env python3
"""Mechanical staleness checks over the repository's documentation (AGENTS.md rule 7).

    python3 core/tools/docs_stale.py [--selftest]

It cannot read meaning, so it only catches what arithmetic and string matching can catch. Rule 7's table is the part
done by hand. Exits non-zero if anything fails, so a pre-commit hook or a CI step can use it.

What it checks:

1. **The headline speeds agree across the files that quote them.** AGENTS.md's opening paragraph, AGENTS.md's "All
   defaults" line and README.md each state frames a second for the four scenes; they drifted apart before (on
   2026-10-30 README and AGENTS still carried the morning's build after the defaults moved).
2. **One build id for the headline.** Every file quoting a headline names the same build, so a number from another
   session cannot be read as progress (the device drifts ~2% between days, a rebuild ~0.8%).
3. **frames a second and speed_pct are consistent**: frames = speed_pct/100 * the scene's refresh (30 for TP and
   Wind Waker, 60 for Mario Party 4), within 1.5%.
4. **Every `--switch` named in AGENTS.md's checks or defaults exists in `core/host3ds/main.c`** - a renamed or
   removed switch leaves the docs telling people to pass something that is silently ignored (which is how an
   `--ab NAME` the build lacked ran for 300 s with no A/B, 2026-10-30).
5. **Every `CNN` candidate referenced from AGENTS.md or HANDOFF has an entry** in REDESIGN_CANDIDATES.md.
6. **No candidate entry has lost its "re-evaluate when" trigger** (rule 4).

Planted-bug self-test: `--selftest` runs each check against text with a known fault and fails if the check passes.
A check whose self-test cannot fail proves nothing (AGENTS, "Checks").
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCENES = ('Ordon', 'heavy area', 'Wind Waker', 'Mario Party 4')
REFRESH = {'Ordon': 30.0, 'heavy area': 30.0, 'Wind Waker': 30.0, 'Mario Party 4': 60.0}


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


# ---- the checks. Each takes the texts it needs and returns a list of complaints. ----

def check_headline_agrees(agents, readme):
    """Ordon and the heavy area, as frames a second, in AGENTS' opening, AGENTS' defaults line and README."""
    bad = []
    pats = {
        'AGENTS opening': r'static Ordon scene at ([0-9.]+) frames a second .*?heaviest area at\s*\n?([0-9.]+)',
        'AGENTS defaults': r'static Ordon ([0-9.]+)\s*\n?\s*\([0-9.]+%\), heavy area ([0-9.]+)',
        'README': r'\*\*([0-9.]+) of the game\'s 30 frames a second in Ordon village \([0-9.]+% of full speed\) and ([0-9.]+) in its heaviest area',
    }
    found = {}
    for name, pat in pats.items():
        text = readme if name == 'README' else agents
        m = re.search(pat, text, re.S)
        if not m:
            bad.append('%s: no headline speeds found (pattern changed? update docs_stale.py with it)' % name)
            continue
        found[name] = (float(m.group(1)), float(m.group(2)))
    vals = list(found.values())
    for i in range(1, len(vals)):
        for k, scene in ((0, 'Ordon'), (1, 'the heavy area')):
            a, b = vals[0][k], vals[i][k]
            if abs(a - b) > 0.051:   # one decimal place of rounding
                bad.append('%s for %s: %s says %.2f, %s says %.2f' %
                           ('headline disagrees', scene, list(found)[0], a, list(found)[i], b))
    return bad


def check_one_build(agents, readme):
    """The headline's build id, wherever it is quoted, is one id."""
    ids = set(re.findall(r'\b([0-9a-f]{16})\b', agents + readme))
    heads = set()
    for text in (agents, readme):
        for m in re.finditer(r'build[s]?\s+`?([0-9a-f]{16})`?', text):
            heads.add(m.group(1))
    if not heads:
        return ['no build id quoted beside a headline: say which build the numbers came from']
    # Only the ids introduced as "build X" matter; a superseded one must be marked as not comparable.
    bad = []
    if len(heads) > 1:
        for h in sorted(heads):
            ctx = ''
            for text in (agents, readme):
                for m in re.finditer(r'build[s]?\s+`?%s`?' % h, text):
                    ctx += text[max(0, m.start() - 400):m.end() + 400]
            if not re.search(r'not comparable|must not be differenced|weather, not progress|previous build', ctx):
                bad.append('build %s is quoted beside a headline with no note that other sessions are not '
                           'comparable (~2%% day drift, ~0.8%% a rebuild)' % h)
    return bad + ([] if ids else [])


def check_frames_vs_speed(agents):
    """frames a second against the speed_pct quoted next to it."""
    bad = []
    m = re.search(r'All defaults.*?movie [0-9.]+%', agents, re.S)
    if not m:
        return ['AGENTS "All defaults" block not found (pattern changed? update docs_stale.py)']
    block = m.group(0)
    for scene in SCENES:
        mm = re.search(re.escape(scene) + r'\s*\n?\s*([0-9.]+)\s*\(([0-9.]+)%\)', block)
        if not mm:
            bad.append('AGENTS "All defaults": no "frames (speed%%)" for %s' % scene)
            continue
        fps, pct = float(mm.group(1)), float(mm.group(2))
        want = pct / 100.0 * REFRESH[scene]
        if want and abs(fps - want) / want > 0.015:
            bad.append('%s: %.2f frames a second against %.1f%% of %.0f Hz = %.2f (off by %.1f%%)'
                       % (scene, fps, pct, REFRESH[scene], want, 100 * abs(fps - want) / want))
    return bad


def check_switches_exist(agents, parsers):
    """Every --switch AGENTS names is parsed by the 3DS program or the PC build."""
    named = set(re.findall(r'`?--([a-z][a-z0-9-]{1,20})\b', agents))
    # Tool flags and rule 5's `--name on|off` placeholder are not program switches.
    named -= {'ab', 'press', 'help', 'from', 'skip', 'by', 'cols', 'split', 'end', 'col', 'selftest',
              'game', 'state', 'seconds', 'out', 'frames', 'load-state', 'profile', 'disasm', 'cycles', 'top',
              'arm', 'max-findings', 'comment', 'fix', 'name'}
    bad = []
    for s in sorted(named):
        if not any(('"--%s"' % s) in p for p in parsers):
            bad.append('AGENTS names --%s but neither main.c nor pc_main.c parses it (renamed or removed?)' % s)
    return bad


def check_candidates(agents, handoff, cands):
    """Every CNN referenced has an entry, and every entry keeps a re-evaluate trigger."""
    bad = []
    have = set(re.findall(r'^## (C\d+)\.', cands, re.M))
    for name, text in (('AGENTS', agents), ('HANDOFF', handoff)):
        for c in sorted(set(re.findall(r'\b(C\d{1,2})\b', text))):
            if c not in have:
                bad.append('%s references %s but REDESIGN_CANDIDATES has no "## %s." entry' % (name, c, c))
    for m in re.finditer(r'^## (C\d+)\..*?(?=^## C\d+\.|\Z)', cands, re.M | re.S):
        body = m.group(0)
        # Rule 4's trigger stops anyone re-running a shelved idea blind. An entry that is built and on by default
        # has nothing to re-run, so it is exempt; parked, next and rejected entries are not.
        done = re.search(r'\*\*Status:?\*\*[^\n]*(default on|on by default)', body)
        if not done and 'Re-evaluate when' not in body and 're-evaluate when' not in body:
            bad.append('%s has no "Re-evaluate when" trigger (rule 4)' % m.group(1))
    return bad


# ---- self-test: each check must fail on text with a planted fault ----

def selftest():
    agents = read('AGENTS.md')
    readme = read('README.md')
    parsers = [read('core/host3ds/main.c'), read('core/src/host/pc_main.c')]
    handoff = read('docs/HANDOFF.md')
    cands = read('docs/research/REDESIGN_CANDIDATES.md')
    fails = []

    def must_fail(what, bad):
        if not bad:
            fails.append('planted %s was NOT caught' % what)

    # 1: move README's Ordon figure away from AGENTS'
    m = re.search(r'\*\*([0-9.]+) of the game\'s 30 frames', readme)
    must_fail('headline disagreement',
              check_headline_agrees(agents, readme.replace(m.group(0), '**99.9 of the game\'s 30 frames', 1)))
    # 2: a second build id with no not-comparable note
    must_fail('second build id', check_one_build(agents + '\nbuild 0123456789abcdef\n', readme))
    # 3: break frames-vs-speed
    m = re.search(r'(static Ordon )([0-9.]+)(\s*\n?\s*\([0-9.]+%\))', agents)
    must_fail('frames/speed mismatch',
              check_frames_vs_speed(agents.replace(m.group(0), m.group(1) + '99.9' + m.group(3), 1)))
    # 4: a switch main.c does not parse
    must_fail('missing switch', check_switches_exist(agents + '\nPass `--nosuchswitch on` to do nothing.\n', parsers))
    # 5: a candidate with no entry
    must_fail('dangling candidate', check_candidates(agents + '\nsee C99 for this.\n', handoff, cands))
    # 6: a candidate entry with its trigger removed
    m = re.search(r'^## C\d+\..*?(?=^## C\d+\.|\Z)', cands, re.M | re.S)
    must_fail('missing re-evaluate trigger',
              check_candidates(agents, handoff, cands.replace(m.group(0), re.sub(
                  r'[Rr]e-evaluate when', 'xxx', m.group(0)), 1)))

    for f in fails:
        print('SELFTEST FAIL: %s' % f)
    print('docs_stale selftest: %s (%d checks)' % ('ok, every planted fault caught' if not fails else 'FAILED', 6))
    return 1 if fails else 0


def main():
    if '--selftest' in sys.argv[1:]:
        return selftest()
    agents = read('AGENTS.md')
    readme = read('README.md')
    parsers = [read('core/host3ds/main.c'), read('core/src/host/pc_main.c')]
    handoff = read('docs/HANDOFF.md')
    cands = read('docs/research/REDESIGN_CANDIDATES.md')
    bad = (check_headline_agrees(agents, readme) + check_one_build(agents, readme) +
           check_frames_vs_speed(agents) + check_switches_exist(agents, parsers) +
           check_candidates(agents, handoff, cands))
    for b in bad:
        print('STALE: %s' % b)
    print('docs_stale: %d problem(s)' % len(bad))
    print('(mechanical only - rule 7\'s table in AGENTS.md is the part done by hand)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
