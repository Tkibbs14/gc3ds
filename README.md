# gc3ds

A GameCube emulator for homebrew-enabled **New Nintendo 3DS / New 2DS XL** systems, built around what that hardware can
actually do: a Gekko-to-ARM11 JIT on one core, the GameCube's graphics pipeline split across two more cores, and the
PICA200 GPU doing the drawing. Every design decision here was measured on a real New 2DS XL.

**Status: research project, not a usable emulator yet.** *The Legend of Zelda: Twilight Princess* (the stress test)
boots, plays its title, its THP movies and its opening cutscene, at about **20% of full speed in its heavy scenes** and
~12% in the heaviest stretch measured. Most of the work so far went into finding out where the time goes and what does
and does not help on this hardware; that knowledge is written down in `docs/` and is the most useful part of the
repository today.

**Bring your own games.** gc3ds contains no game data, BIOS or firmware, and never will. Use dumps of discs you own.

## Where the time goes (New 2DS XL, Twilight Princess's opening cutscene)

- **Core 0 (the JIT)** alone would run the cutscene at ~23% and the heaviest stretch at ~12%. It spends ~33 cycles per
  emulated instruction while not waiting: ~13 executed instructions, ~10 instruction-fetch stall cycles, ~8 data stall
  cycles. There is no hot spot: the profile is flat (no Twilight Princess function above 0.5% of core 0).
- **The GX side** (the command FIFO on core 2, a vertex worker on core 3) limits the cutscene at ~20%: gathering vertex
  attributes from emulated RAM is memory-latency bound (~1.35 µs a vertex). Skipping every other frame's vertex work
  gives +10%, and then core 0 is the wall.
- Full speed would need ~9 host cycles per emulated instruction on core 0 and ~5x the vertex throughput. See
  `docs/research/PERF_LEVERS.md` and `docs/research/REDESIGN_CANDIDATES.md` for what has been tried, measured, kept or
  rejected, and what is still open.

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
(`tools/relay-server/`), and read the results with `core/tools/perf_heavy.py`.

## Documentation

| | |
|---|---|
| `AGENTS.md` | How to work on this repository (people and coding agents): rules, gates, how to measure, where things stand |
| `docs/HANDOFF.md` | The running log of what was built, measured and learned, newest at the end |
| `docs/research/PERF_LEVERS.md`, `docs/research/REDESIGN_CANDIDATES.md` | Ranked levers; every idea with its evidence and status |
| `docs/OPT_JIT_PLAN.md`, `docs/REGION_JIT_PLAN.md`, `docs/DL_CACHE_PLAN.md`, `docs/GPU_BACKEND_PLAN.md` | Designs |
| `docs/FINDINGS.md`, `docs/research/GPU_GUIDANCE.md`, `docs/research/PICA200.md`, `docs/ARCHITECTURE.md` | Hardware facts measured on a New 2DS XL, and references |

## Contributing

Contributions are welcome and reviewed: see `CONTRIBUTING.md`. A change is merged when it is exact (passes the checks)
and its effect is measured.

## Licence

GPL-2.0-or-later (`LICENSE`). Dolphin, YAGCD, WiiBrew, 3dbrew and the IBM and ARM manuals were read as documentation.
