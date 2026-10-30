# Contributing to gc3ds

Thanks for looking. gc3ds is a measurement-driven project: a change is merged when it is **exact** and its effect is
**measured**. Read `AGENTS.md` first; it holds the rules, the checks and how to measure.

**Looking for something to do?** `docs/CONTRIBUTOR_TASKS.md` lists real tasks with the measurement behind each and
the checks that prove them - four need no console at all. `docs/TELEMETRY.md` says what a measurement submission
needs and how it is checked (and what the project deliberately never collects).

## What helps most

- **Performance work with numbers.** A JIT or graphics change behind a switch, with the cycle model's before/after
  (`core/tools/arm11_sched.py`) and, if you have a New 3DS / New 2DS XL, an in-run A/B (`--ab`) on a console.
- **Hardware measurements.** Facts about the New 3DS that decide designs: instruction cache behaviour, L2 and RAM
  latencies, cross-core effects, PICA200 throughput. Add them to `docs/FINDINGS.md` with the method.
- **Correctness.** Bugs against the interpreter, new fuzzer cases, games that break (describe what happens; never
  attach game data).
- **Ideas.** Open an issue, or add a candidate to `docs/research/REDESIGN_CANDIDATES.md` with evidence for and against.

## Pull requests

- One idea per pull request.
- Run the checks for what you touched (`AGENTS.md`, "Checks") and paste their summary lines into the description.
- Performance changes: say what was measured, how, and the result with its standard error. "Faster" without numbers
  will be asked for numbers.
- New optimizations come off by default behind a switch; they become default after a measured A/B.
- Keep the code style of the file you touch; comments explain why.
- By contributing you agree that your work is licensed under GPL-2.0-or-later.

## Never

- Disc images, ROMs, save states of commercial games, BIOS, firmware, extracted assets, or links to them.
- Secrets: relay tokens, keys. `core/host3ds/relay.txt` stays local.

## Review

Every pull request is built and its checks are run before review. A maintainer may queue it on a console for an A/B
before merging.
