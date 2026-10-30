---
name: Measurement
about: A speed measurement from your own console, for or against a change
title: 'measure: '
labels: measurement
---

<!-- docs/TELEMETRY.md says what is checked and why. Please do not attach game data of any kind: no disc images,
     no save states of commercial games, no assets. Numbers and perf.csv logs only. -->

## What was measured

- **Change under test:** <!-- the switch, e.g. `--fdirect on`, or "the default set, as a baseline" -->
- **Commit / build:** <!-- the commit you built; the build id from `relay.sh push` if you used the relay -->

## Console

- **Model:** <!-- New 3DS / New 3DS XL / New 2DS XL -->
- **Boot:** <!-- Luma version, or how you launch homebrew -->

## Scene

- **Game and state:** <!-- e.g. Twilight Princess, a state in Ordon village -->
- **Scripted input:** <!-- none, ideally. If you used --press, say so: it cannot judge a timeline change (C41) -->
- **Seconds a run, and from where you scored:** <!-- e.g. 240 s, scored from 80 s -->

## Numbers

Paste `python3 core/tools/runs.py --skip 80 <each log>` — two runs an arm, one session, one build:

```
baseline run 1:
baseline run 2:
change   run 1:
change   run 2:
```

- **Frames a second, baseline vs change:**
- **Spread within each arm:**

Optional but welcome: `core/tools/clock_split.py` (where the emulated clock went) and `core/tools/port_cost.py`
(what the ports cost core 0).

## Logs

<!-- Attach each perf.csv. Unedited: the option columns are how a reader checks the arms differ in one switch only. -->

## Anything odd

<!-- Glitches, stalls, a crash, a scene that behaved differently between arms. Say so even if it seems minor:
     a difference between arms in draws or vertices a picture means the arms were not the same scene. -->
