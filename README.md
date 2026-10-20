# gc3ds

A GameCube emulator for homebrew-enabled **New Nintendo 3DS / New 2DS XL** systems, built around what that hardware can
actually do: a Gekko-to-ARM11 JIT on one core, the GameCube's graphics pipeline split across two more cores, and the
PICA200 GPU doing the drawing. Every design decision here was measured on a real New 2DS XL.

**Status: research project, not a usable emulator yet.** *The Legend of Zelda: Twilight Princess* (the stress test)
boots and plays: its title, THP movies and opening cutscene, then gameplay in Ordon Village (Link walking around).
On a New 2DS XL it runs at about **32% of full speed walking in Ordon and ~22.8% in its heaviest area** (9.5 and 6.8
of the game's 30 frames a second), ~37% in the intro cutscene, ~23.5% in the scene after it and ~13.4% in the THP movie
(20 October 2026). Most of the work so far went into finding out where the time goes and what does and does not
help on this hardware; that knowledge is written down in `docs/` and is the most useful part of the repository today.

**Bring your own games.** gc3ds contains no game data, BIOS or firmware, and never will. Use dumps of discs you own.

## Where the time goes (New 2DS XL, Twilight Princess gameplay in Ordon)

- **Core 0 (the emulated CPU, JIT) is the limit in every scene.** Sampled together, core 0 runs guest code 93-97% of
  the time and almost never waits, while the GX thread (core 2) is idle 12-13% in gameplay and 40-75% in the title, and
  the vertex worker (core 3, which also lights vertices) is idle ~70-96%. Core 0's profile is flat and bound by
  instruction fetch: rendering ~39% (the game building its command stream), the 2D UI ~22%, game logic ~19%; in the
  title's scene after the cutscene (Link and Epona) the shadow-drawing walk was ~25% of it, until repeated walks were
  replayed from a record (+14%) and each shadow triangle's work and callback ran natively (+7%). Native ports of library
  functions that do less than the guest code pay (`docs/research/LIBRARY_PORTS.md`), as did JIT options that keep
  floats in VFP registers and cut indirect-branch misses.
- **The GPU has headroom:** ~5-15 ms a frame since textures moved to VRAM and lighting off the vertex shader, and
  frames overlap on it (the GX thread no longer waits for the previous frame's tail).
- Full speed is still ~4-5x away. `docs/research/MEASUREMENTS.md` holds every measurement, and `docs/research/REDESIGN_CANDIDATES.md` every idea that was tried, kept or rejected, with its numbers.

## How it works

| Part | Where |
|---|---|
| Gekko CPU: interpreter (the reference) and an ARMv6K JIT with a register cache, check hoisting, linked blocks and traces | `core/src/cpu/`, `core/src/jit/` |
| GameCube hardware: memory, interrupts, VI, DI, DSP/ARAM, EXI, SI | `core/src/sys/`, `core/src/hw/` |
| GX: command FIFO, display lists, transform and lighting (`vfast.c`), TEV, EFB copies | `core/src/gx/` |
| PICA200 backend: batching, draw merging, GPU-side position transform and lighting, a vertex worker thread | `core/src/gx/pica/` |
| 3DS host: threads on cores 0-3, perf logging, the remote-runs client, crash logging | `core/host3ds/` |
| PC host: the reference build (software renderer), used for correctness checks | `core/src/host/`, `core/src/gx/soft.c` |
| Hardware benchmarks for the 3DS (CPU shapes, memory, GPU) | `bench/` |

## Building (Linux or WSL)

Needs devkitPro (devkitARM with libctru and citro3d for the 3DS build, devkitPPC for the test programs), a host C
compiler, and for the ARM correctness checks `arm-linux-gnueabihf-gcc` and `qemu-arm`.

```bash
make -C core                                  # PC reference build: core/build/gcemu
core/build/gcemu program.dol --seconds 2 --out out   # run a DOL, save the screen as PNG
bash core/host3ds/build.sh                    # the 3DS build: core/host3ds/gc3ds.3dsx (Homebrew Launcher, Luma3DS)
bash core/tools/arm_build.sh                  # the ARM Linux build for qemu: core/build-arm/gcemu
bash gc-tests/build_examples.sh               # libogc's GameCube examples as test programs
```

Scripts that run a game take its path from `GC3DS_ISO`.

## Testing

Every change to the JIT or the graphics path must pass the checks that apply (details in `AGENTS.md`):

```bash
bash core/tests/jit/run_jit_fuzz.sh 20000 1   # JIT vs interpreter differential fuzzer under qemu, with planted-bug self-tests
bash core/tests/cpu/run_tests.sh              # interpreter tests
bash core/tests/tev/run_tev_tests.sh          # TEV (combiner) tests against the PICA model
bash core/tools/vfast_check.sh 2              # fast vertex path against the reference transform on the test DOLs
```

## Measuring on a console

`docs/REMOTE_RUNS.md`: queue builds and A/B runs to a console on any network through a relay you run yourself
(`tools/relay-server/`), and score the results with `core/tools/runs.py` (gameplay) and `core/tools/title_sections.py`
(the title screen) as separate whole runs. A running job can be steered live
(`relay.sh cmd JOB "press A 6"`, `"stick X Y N"`, `"save PATH"`, `"shot"`; `relay.sh frames JOB` fetches the
screenshots), which is how gameplay save states for benchmarks are made; `--press` scripts buttons and the main stick
(`SLEFT` / `SRIGHT` / `SUP` / `SDOWN`) for repeatable gameplay runs.

## Documentation

| | |
|---|---|
| `AGENTS.md` | How to work on this repository (people and coding agents): rules, gates, how to measure, where things stand |
| `docs/HANDOFF.md` | The running log of what was built, measured and learned, newest at the end |
| `docs/research/MEASUREMENTS.md` | Every measurement, with its job, conditions and what it informs |
| `docs/research/PERF_LEVERS.md`, `docs/research/REDESIGN_CANDIDATES.md` | Ranked levers; every idea with its evidence and status |
| `docs/research/DIRECTION_REVIEW.md` | The 2026-10-20 independent review: what limits gc3ds (two walls), library ports by Amdahl, execution models, ranked directions, experiments, ceilings |
| `docs/research/EMULATOR_CEILING.md` | How far emulation alone can go: today's cost model (~22 cycles per guest instruction), the lower bound, every rejected idea re-examined, proposals with disproving experiments, a roadmap |
| `docs/research/OUTSIDE_RESEARCH.md` | What published work (other ARM JITs, the TP decomp and Dusklight, frame skipping, 3DS homebrew) says that bears on gc3ds |
| `docs/OPT_JIT_PLAN.md`, `docs/REGION_JIT_PLAN.md`, `docs/DL_CACHE_PLAN.md`, `docs/GPU_BACKEND_PLAN.md` | Designs |
| `docs/FINDINGS.md`, `docs/research/GPU_GUIDANCE.md`, `docs/research/PICA200.md`, `docs/ARCHITECTURE.md` | Hardware facts measured on a New 2DS XL, and references |

## Contributing

Contributions are welcome and reviewed: see `CONTRIBUTING.md`. A change is merged when it is exact (passes the checks)
and its effect is measured.

## Licence

GPL-2.0-or-later (`LICENSE`). Dolphin, YAGCD, WiiBrew, 3dbrew and the IBM and ARM manuals were read as documentation.
