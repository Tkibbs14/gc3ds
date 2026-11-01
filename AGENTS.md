# AGENTS.md: working on gc3ds

For people and coding agents. Read this whole file before changing anything; then read the end of
`docs/HANDOFF.md` (the newest state) and `docs/research/REDESIGN_CANDIDATES.md` (what has been tried).

gc3ds is a GameCube emulator for the New Nintendo 3DS / New 2DS XL (ARM11 MPCore, 4 cores at 804 MHz, 2 MB shared L2,
PICA200 GPU). The goal is the highest speed the hardware allows, with **exact** emulation: the interpreter is the
reference, and the JIT and the fast graphics paths must produce the same results. *Twilight Princess* is the stress
test. Today it runs a static Ordon scene at 10.55 frames a second (35.2% of full speed) and Ordon's heaviest area at
7.28 (24.3%); Wind Waker's Outset 6.23 (20.8%), Mario Party 4's board 9.27 (15.5%), the title's cutscene 40.9%, its
later Link / Epona stretch 23.0%, the THP movie 14.8% (31 October 2026, build 2ba59806b89d7a22, two runs an arm,
scored with `runs.py`'s stolen-second filter - arms agree to 0.00-0.22%). **These are not comparable with the
previous block's** (10.54 / 7.22 / 6.24 / 9.28 on build 103456ecbdfe9264): a rebuild moves a baseline ~0.8%, the
device drifts ~2% between days, and only runs from one session on one build may be compared - the defaults did not
change between those two builds, so the difference between them *is* that drift, measured. **Judge
gameplay on frames a second from a scene that takes no input, not on the scripted walk** - see "Measuring" and C41.

## Rules

1. **No game data, ever.** No disc images, save states of commercial games, BIOS, firmware or extracted assets in the
   repository, in issues or in pull requests. Scripts take a disc image from `GC3DS_ISO`. Test programs are built from
   libogc's open-source examples (`gc-tests/`).
2. **Exactness first.** The interpreter (`core/src/cpu/gekko*.c`) is the reference. A JIT or graphics change that
   changes results is a bug, not a trade-off. Known, documented differences are limited to the host float model
   (`jit.c`, the comment above `ld_fpr`: flush-to-zero denormals, default NaN, no FPSCR exception bits); do not add new
   ones.
3. **Measure, do not guess.** A performance change is judged by **separate whole runs on real hardware, on a scene
   that takes no input**, scored on **frames a second** (`core/tools/runs.py`): two runs an arm, one session, one
   build. **Measure it against the *current* defaults, and record I-cache misses beside frames a second** - switches
   that both move instruction-fetch pressure cannot both be paid out of the same cache lines, and one switch's best
   setting can depend on another's: with `--psflush` off, `--port-hooks 503` beats 243 by +0.37%; with `--psflush`
   on, 503 *loses* to 243 by 0.42% (C49, measured 2026-10-30). A figure taken at an older baseline is not evidence
   about today's, and **the ledger's entries must not be added up** - where they share the cache budget some of them
   cancel, which is why the headline numbers have moved far less than the entries suggest.
   **The device can step ~5.6% between two consecutive runs of one session, so "one session, one build" is not
   sufficient - the arms must be interleaved** (2026-11-01, jobs 1760-1784; MEASUREMENTS' withdrawn-regression
   section has the whole story). Static Ordon at unchanged defaults read **10.020** at ~00:33 and the same build
   read **10.592** a half hour later; every run before that boundary reads ~10.03 and every run after it reads
   10.54-10.62, on seven different commits, with the clock identical across it (`pmu_mcyc` 804) and every host-side
   column at 0. **A commit does reproduce tightly *within* a state** - `088fb0df6610` rebuilt in another container
   gave 10.619 / 10.614 against a previous session's 10.605 / 10.619 - **but do not read that as the device being
   stable**: it shows only that those runs sat on one side of a step. **So when a baseline moves by more than a
   point, re-run the *new* build late as well as the old one.** Running only the old one late is how a device step
   became a "-5.6% regression" in this very file for an hour, after the arms were batched - the exact thing this
   rule forbids, done while investigating a measurement artifact.
   **A change that alters allocation sizes moves the rung, and the rung is then not separable by re-running**
   (2026-11-01, C48's `FAST_BITS` sweep). `FAST_BITS` 13 allocates 1.75 MB less than 16, so it is a different heap:
   it landed on rung 8.8 where 16 landed on 8.9, each *reproducibly across two runs*. The naive comparison - one
   commit, two runs an arm, 0.04% spreads, complete rank separation - read **+0.96%**. The true figure, with both
   arms on one rung, is **-0.71%**. **The rung was worth 1.7 points and flipped the sign**, which is how a
   regression would have shipped as a gain. The fix is to **pad the allocation** so only the quantity under test
   changes (`jit.c` now malloc's the indirect tables at the `FAST_BITS` 16 size whatever `FAST_BITS` is); both arms
   then land on 7.1 and the comparison is clean. **Any A/B of an allocation size needs that treatment**, and
   `runs.py`'s refusal to compare across rungs is what catches it when you forget.
   **The step is the launch memory state, and `runs.py` now prints and polices it** (C55): every log's
   `# heap: largest malloc block` labels the state (2.9, 7.1, 7.2, 8.6 or 9.0 MB across the logs on disk), **one of
   those labels is ~5% slow** - 9.0, whose Ordon defaults read 10.03 against 7.1's 10.59 on the same commit, while
   2.9 (the historical majority) performs like 7.1 - and the value is fixed when the process starts and inherited by
   every job reusing it - it changes **only** when a job downloads
   a build and chainloads through `hb:ldr`. So **a measurement series wants one process, or at least one heap
   value**, and `runs.py` refuses to compare across values. **This is the likeliest identity of the "~2% day drift"
   this rule has quoted for weeks**: a series that chainloads part-way through moves between rungs. Before a series
   that matters, have the device launched by hand so it starts from a known rung. Runs agree closely once the seconds core 0 did not get are left out, which `runs.py`
   does by default - but **the scene sets the floor, not the method**, and **two runs understate it**: four runs of
   one configuration on one build in one session span **0.23% in `tp_area2`** (7.264-7.281, jobs 1692/1693 and
   1700/1701, two hours apart), where the two adjacent *pairs* inside that span read 0.01% and 0.17%. Static Ordon
   repeats only to ~0.25-0.31% (jobs 1673-1676). So **the heavy area's floor is ~0.2%, not the 0.07% a lucky pair
   showed**: a claim under ~0.4% needs more than two runs an arm, or the arms interleaved rather than batched, and
   a 0.3% claim from static Ordon is not a result at all.
   **The floor depends on how far apart the runs are, even inside one launch rung** (2026-11-01, from logs already
   on disk). Four runs of one configuration two hours apart span **0.25%** (jobs 1692/1693 and 1700/1701 - and all
   four are on the 2.9 MB rung, so that span is *not* a rung artifact: that hypothesis was checked and killed).
   Four runs of one configuration queued back to back span **0.11-0.15%** (`--port-softfloat`'s two arms, jobs
   1797-1804, heavy area). So **adjacency buys about a factor of two on the floor** and is what makes a 0.2% claim
   decidable: soft float's +0.20% was settled by four adjacent runs an arm with complete rank separation, where arms
   hours apart could not have carried it. Use the heavy area to decide anything small. the program's own periodic screenshot used to steal **4-86% of one second** five times in a 240 s run,
   and a run that caught several severe ones read up to **3.2% slow with nothing saying so** (17 of 136 logs on
   disk; it is what looked like "session drift" on 2026-10-30 until `pmu_mcyc` named it). The periodic shot is off
   by default now (`--shots N`). **Always quote the within-arm spread** - a difference smaller than it is not a
   result, and today it was the spread, not the difference, that exposed this. `runs.py` prints how many rows and
   frame windows it dropped; a run that drops many is telling you something. An A/B within one run (`--ab NAME`) is a cross-check,
   not the verdict: switches that flush compiled code stall for seconds and read the on arm ~2% low, and `speed_pct`
   is the emulated clock, which native stand-ins and ports charge for work they did natively. **Do not judge a change
   on the scripted walk** (`--press`): the input is sampled when the guest polls the pad, so a change that shifts
   timing sends Link down another path and the arms render different scenes (C41). This is not pedantry - on
   2026-10-30 it turned out that eight recorded per-port gains, two stand-in gains and frame skip's "+10%" were all
   artifacts of the old method.
4. **Keep the evidence.** Every idea with redesign potential goes into `docs/research/REDESIGN_CANDIDATES.md` with its
   evidence, cost, status (parked / next / built / rejected) and a "re-evaluate when" trigger. Never delete an entry;
   update its status. A rejected idea keeps the measurement that rejected it, so nobody re-runs it blind.
5. **Off by default until measured.** New optimizations come behind a switch (`jit_set_opt` bits, `--name on|off`,
   `--ab name`), off by default, and become default only after a hardware A/B shows a gain and the checks pass (the
   owner, 2026-10-24: a measured, exact gain is turned on without asking; crash risks and trade-offs still go to the owner).
6. **Small, reviewable changes.** One idea per pull request, with its checks and its measurement in the description.
7. **Leave nothing stale.** A change is not finished when the code works - it is finished when every place that
   quotes what you changed says the new thing. This is part of the change, not follow-up work, and it goes in the
   same commit. Run `python3 core/tools/docs_stale.py` before you commit: it is mechanical (it cannot read meaning),
   so the table below is the part you do by hand.

   | what you changed | what must be updated in the same commit |
   |---|---|
   | a headline speed | the opening paragraph of `AGENTS.md` **and** `README.md`, "Where things stand" below, and HANDOFF's newest block - with the build id, and a note that figures from another session are not comparable (~2% day drift, ~0.8% a rebuild) |
   | a default (`--name`, an `OPT_` bit, `--port-hooks`) | the option's own comment where it is set in `main.c` / `opts_default` (with the jobs), "Defaults that measured as gains" in `AGENTS.md`, `docs/REMOTE_RUNS.md` if the switch is new, and MEASUREMENTS with the runs |
   | a measurement that contradicts a recorded one | the old entry is **corrected in place, never deleted** (a `> **Correction, DATE...**` block under it), its REDESIGN_CANDIDATES status, and any file quoting the old number - `grep` for it |
   | an idea tried, parked or rejected | a REDESIGN_CANDIDATES entry with evidence, cost, status and "re-evaluate when" (rule 4), and AGENTS' "Open directions" if the ordering moved |
   | a new tool or option | its own `--help` / docstring, the `core/tools/` row of the repository map, the checks table if it is a check, and `docs/REMOTE_RUNS.md` for a job option |
   | a new **instrument** (a counter, a census, a scan, a scoring rule) | rule 8: the quantity it shares with an existing instrument, and the two figures differenced in its first run - recorded wherever its result is quoted |
   | an instrument's meaning or a column's name | every doc that quotes its output, and the tool's docstring - a label that teaches the wrong reading is a bug (`port_cost.py`'s "building the context", 2026-10-30) |
   | a contributor-visible fact | `docs/CONTRIBUTOR_TASKS.md` (a task that is already done is worse than no task), `docs/TELEMETRY.md`, `CONTRIBUTING.md` |

   **Withdrawing a credit is normal and must be explicit.** When a recorded gain turns out to be an artifact, say so
   where it was recorded and say whether the option stays on - a withdrawn credit is not a rejected option
   (`--hlegd`, 2026-10-30).
8. **Difference a new instrument against something that already exists, before believing it.** Every measurement in
   this project arrives through an instrument someone wrote, and **an instrument's first result is the least
   trustworthy number in the file**. On 2026-11-01 five of them were wrong on their first run, in five different
   ways, and **every one was caught by a second, independent view of the same quantity - never by reading the code**:
   - `--port-size-refused` **destroyed what it measured**: sizing a refusal meant executing the call, and
     `gekko_run_call` never consults the hook, so the refused call's children vanished. Caught by a peer's run
     showing 72,000 hook entries collapse to 1,476.
   - the C56 scan **silently discarded the interesting cases**: a `mEntryTableSize > 64` guard threw away a quarter
     of all calls, and they were the big buffers. Caught by differencing against the entry-point census, which said
     6.3 mat packets a call where the scan said 1.8.
   - `--port-softfloat`'s check was **vacuous**: it reported "0 differed" while the refusal had merely moved
     downstream into `psq_copy`, so no compared call ever ran the new code. Caught by a counter that says the new
     code *ran* (`drew N`), added because the J2DPicture port had already failed the same way.
   - the pane walk's give-up reason was **mislabelled** "tev block" over 21 sites, and sized a whole open direction
     from a bucket of unrelated causes. Caught by `otherTev` reading 0.
   - `runs.py`'s scoring **attributed a device step to the code**: a batched old-against-new comparison became a
     "-5.6% regression" and an eight-commit bisect. Caught by re-running the *new* build late.

   So: **when you add an instrument, find a quantity it shares with an existing one and compare them in the first
   run.** If no such quantity exists, make one - the census before the scan, `drew N` beside `differed 0`, an
   entry-point count beside a structure walk. Two views that agree are evidence; one view is a hypothesis. And
   **report the new instrument's figure next to the old one in the same log line** where you can, so the next reader
   differences them without having to think of it.

## Repository map

| Path | What |
|---|---|
| `core/src/cpu/` | Gekko interpreter (`gekko_int.c` integer, `gekko_fpu.c` float and paired singles, `gekko_softfp.c` exact soft float, `gekko_mem.c` MMU/BATs), `gekko_regs.c` (registers each instruction reads and writes, fuzzed), `hle_tp.c` / `hle_gd.c` / `hle_ps.c` / `hle_thp.c` (native stand-ins for hot guest functions, checked with `HLE_CHECK=1` on the PC; `hle_ps_gen.inc` is generated by `core/tools/ps_translate.py`), `memo.c` (function memoization probe, `--memo ADDR`) |
| `core/src/jit/jit.c` | The JIT: ARMv6K code generation, register cache (`Rc`, r4-r8), float lanes (`OPT_FCACHE`, s16-s31), check hoisting (`OPT_HOIST`), known-single stores (`OPT_KSTORE`), shared memory and float stubs, block linking and traces, cold code, exits |
| `core/src/jit/arm_emit.h` | ARM and VFPv2 instruction encoders |
| `core/src/sys/`, `core/src/hw/` | System bus, interrupts, VI, DI (disc), DSP/ARAM (`dsp_hle.c`), EXI, SI |
| `core/src/gx/fifo.c` | GX command FIFO and display lists; page stamps (`gx.h` `MemStamps`) for cache invalidation |
| `core/src/gx/vfast.c` | The fast vertex path: gather (`gather_range`), lighting, texgen, split into `vfast_prepare` (GX thread) and `vfast_finish` (vertex worker, which also gathers and lights CPU-lit draws: `g_vfDeferCpu`); vertex set-ups cached for 8 keys; the display-list vertex cache (off by default) |
| `core/src/gx/pica/pica.c` | PICA200 backend: batching, draw merging, palette-based GPU transform, raw draw calls and state, the vertex worker's job ring (`vw_*`), textures (mipmaps, small texel formats, VRAM by DMA), lighting on the CPU by default (`--shlight`), a 2 MB command buffer, GPU diagnostics (`--gpudiag`), second worker (`--vw2`, rejected), frame skip (`--fskip`, experiment) |
| `core/src/gx/tev_pica.c` | GameCube TEV to PICA combiners (with a linear path for out-of-range programs such as THP's YUV) |
| `core/src/gx/soft.c` | Software renderer (PC reference) and the PC-side checkers (`VFAST_CHECK`, `VCACHE_CHECK`, `MERGE_CHECK`) |
| `core/host3ds/` | The 3DS program: `main.c` (options, perf.csv, A/B switching, live commands), `gxthread.c` (core 2), `relay.c` (remote runs), `asyncfile.c` (log writer on core 1), `crashlog.c`, `jit_3ds.c` |
| `core/src/host/` | PC build: `pc_main.c`, `jit_glue.c` (the ARM build's JIT under qemu) |
| `core/tests/` | `jit/` differential fuzzer, `cpu/` interpreter tests, `tev/` TEV tests |
| `core/tools/` | `perf_heavy.py` (A/B analysis), `clock_split.py` (where a run's emulated clock came from and each field's slack, C41), `port_cost.py` (what each port costs core 0), `jitbench.py` (the JIT's ARM output for a hand-written guest loop, no disc image; its cases are `core/tests/jit/bench/*.S` - `callmany.S` is the one with a many-caller leaf, which is what makes indirect branches happen), `arm11_sched.py` (ARM11 cycle model), `gpr_traffic.py` (where compiled code's data accesses go, weighted by executions: guest GPRs / floats / other Gekko state / guest memory, and how much of it is write-back - C51's ceiling and C47's cheap first cut; same `JIT_SCHEDDUMP` as the model), `host3ds_syntax.sh` + `stub3ds/` (syntax-check the 3DS host code with no devkitARM), `docs_stale.py` (rule 7's mechanical documentation checks), `rungs.py` (which launch memory rung each run on disk was in, and whether it chainloaded - C55: one rung is ~5% slow, so an entry whose arms straddle a rung change is wrong by up to 5%, and this checks it with no device time), `relay.sh` (remote runs), `vfast_check.sh`, `arm_*.sh`, `tp_*.sh`, `hammock.py` (block-shape model over a PC profile from gcemu's `PCHIST` + `EDGEHIST` + `MEMDUMP`) |
| `tools/relay-server/` | Reference relay server for remote runs (Node, no dependencies) |
| `bench/` | `gcbench`: 3DS hardware benchmarks (CPU shapes, memory, GPU rules) |
| `docs/` | Designs, findings and the running log (see README) |

Threads on the 3DS: core 0 runs the emulated CPU (JIT); core 1 (80% for the app) the log writer, samplers and relay
client; core 2 the GX thread (FIFO, `vfast_prepare`, PICA command lists); core 3 the vertex worker (`vfast_finish`,
culling, triangle assembly).

## Toolchain

Linux or WSL (Debian). devkitPro at `/opt/devkitpro` (devkitARM, libctru, citro3d, 3ds portlibs: curl, mbedtls, zlib),
**A cloud session with no devkitARM can install one** by extracting devkitPro's official container image through the
registry API with `curl` (no daemon needed; the apt and pacman hosts are usually blocked by the environment's
network policy, and `downloads.devkitpro.org` only redirects to one of them). That brings **devkitARM 16.1.0**,
which is **not a neutral substitution**: it is +0.3 to +1.0% on TP and Wind Waker but **-4.7% on Mario Party 4**,
because `gxvtx_pct` doubles (18.2% -> 36.8%) - gcc 16 compiles `vfast.c`'s vertex path about half as fast. Proven to
be the toolchain and not the code by building the *old* commit with it (jobs 1734-1735), and **not**
auto-vectorisation (`-fno-tree-vectorize` changes nothing, job 1736). So from such a session: core-0 work is safe to
measure, **GX-side and vertex work is not**, and Mario Party 4's number must not be compared across toolchains.
Every log's header now carries `# build: gc3ds <commit>` and `runs.py` prints it, so read it before differencing.

devkitPPC for `gc-tests/`, a host gcc, `arm-linux-gnueabihf-gcc` and `qemu-arm` (`qemu-user`) for the ARM checks,
Python 3 for the tools, Node 18+ for the relay server.

```bash
make -C core                      # PC: core/build/gcemu
bash core/host3ds/build.sh        # 3DS: core/host3ds/gc3ds.3dsx
bash core/tools/arm_build.sh      # ARM Linux for qemu: core/build-arm/gcemu (the real JIT, run under qemu-arm)
```

The PC build runs the interpreter and the software renderer; the JIT is exercised through the ARM build under qemu and
on the console.

## Checks (run the rows your change touches; all must pass before review)

| Change | Checks |
|---|---|
| JIT (anything in `jit.c` / `arm_emit.h`) | `bash core/tests/jit/run_jit_fuzz.sh 20000 1` (default and chain mode, planted bugs must be caught); then with `JIT_FUZZ_OPT=7` and with `JIT_FUZZ_NOFAULT=1` for seeds 1 and 3. New code paths need new fuzzer cases (see the paired-single chains in `jit_fuzz.c`); the script also runs the tier nursery
  (`JIT_FUZZ_NURSERY`, a small one recycled often) and plant 29. Then the whole-game check below (qemu: set
  `GC3DS_NOFAULTMEM=1`, as the qemu build runs fault mode by default and fault mode turns the nursery off). |
| JIT, whole game | Under qemu from a save state, the JIT with and without the change must run **the same number of guest instructions and produce the same final frame** (`core/build-arm/gcemu "$GC3DS_ISO" --load-state S --frames N --jit --out DIR`, compare `last.png` and the `JIT: N instructions run` line). `--frames` counts fields from boot, not from the state. This caught a bug the fuzzer missed (HANDOFF, float lanes). |
| Interpreter | `bash core/tests/cpu/run_tests.sh` (needs devkitPPC for its PowerPC cases; it also runs the frame pacer's self-test, which does not) |
| Guest frame pacer (`--gfskip*`, `hle_tp.c`) | `GC3DS_GFSKIP_SELFTEST=1 core/build/gcemu`, also run by `run_tests.sh`: it drives the real controller with a synthetic clock and asserts the game lands on its target speed, the drawn frames stay **evenly spaced**, the **realised** picture rate never falls below `--gfskip-minfps` (at P = 4, 5 and 6, and an impossible P = 30 must draw every frame - that degenerate branch had no test until it was added and a planted 0 passed everything else), an easy scene is not capped, the ratio backstop holds, the fixed ratio is untouched, and **a poisoned warm-up recovers** (every warm-up skip sample measures longer than a drawn frame: the impossible samples must be rejected, the probe must fire, and the run must still land on its target - the latch-up of jobs 1681-1688, where the controller paced nothing for four minutes while its report looked healthy). Feedback over real time cannot be judged by reading it or by one console run. Planted-bug self-tests that must fail: dropping the skipped-frame cost from the maths, choosing draws by a counter instead of the accumulator, removing the floor, clumping draws into pairs while keeping the right rate (that one protects "looks natural"), accepting an impossible skip sample, and removing the recovery probe (which reproduces the latch-up exactly: the scored mean reads a drawn frame's own cost). |
| Vertex path (`vfast.c`, `xform.c`) | `bash core/tools/vfast_check.sh 2` (all test DOLs, 0 draws over tolerance); deferred CPU lighting: `VFAST_CHECK=1 VFAST_DEFLIT_CHECK=1` on TP (0 differ) and `VFAST_DEFLIT_PLANT=1` / `=2` (must differ) |
| Native stand-ins (`hle_*.c`, `hle_ps_gen.inc`) | `HLE_CHECK=1` on the PC over TP (0 differed; `HLE_STOP_EVERY=N` for the stops inside); after regenerating with `ps_translate.py`, the existing functions must come out byte-identical |
| Display-list cache | `VCACHE_CHECK=1` on the PC build (every hit gathered again, 0 differences) and `VCACHE_PLANT=1` (must report differences) |
| TEV / combiners | `bash core/tests/tev/run_tev_tests.sh` |
| Draw merging | `MERGE_CHECK=1` on the PC build |
| Translated lists (`g2.c`, fifo.c `g2_replay`) | PC, TP title spans: `G2=1` gives the state hash and last frame of G2 off; `G2=check` 0 differed; `G2=plant` must differ. 2DS: `--g2 check` 0 differed, `--g2 plant` differs |
| Native library ports (`port_j2d.c`) | PC: `PORT_CHECK=1` over TP's title states (0 differed) and `PORT_CHECK=plant` (every checked call must differ); 2DS: `--port check` in the scenes the port runs in (the minimap and HUD text are not in the PC states) with 0 differed; hash every guest function the port follows (`core/tools/porthash.py` into `port_j2d_hashes.inc`, code only: run-time tables make the ports switch off); calibrate `kInsScale` from check mode's instructions per call. **A session with no disc image cannot run `porthash.py`: record the row with hash `0ull`, never a guess.** A zero row is treated exactly as a mismatch - every port off, so it can never be the weaker check - and the console publishes the hash it computed as a paste-ready `# port hashes` line (one short run fills them in; that build has no ports and must not be used for speed) |
| The ports' give-up histogram (`port_j2d.c` `why_line`) | `GC3DS_PORTWHY_SELFTEST=1 core/build/gcemu`, also run by `run_tests.sh`: it cannot be exercised without a disc image (the ports only hook TP's code), so the histogram has its own check - a repeated line increments its own slot rather than taking a new one, the slots fill in order, and give-ups past them are counted in `other` rather than lost. Planted bugs that must fail it: a repeated line taking a new slot, and the overflow dropped. The thing it protects is that **`# port give-ups 10s` says which line refused**: the reason codes are coarse (reason 5 has 21 sites and its old "tev block" label was sizing the top open direction from a bucket of unrelated causes) |
| Measurement tools (`runs.py`) | `python3 core/tools/runs.py --selftest`: a synthetic log with a planted steal must be recovered exactly, a merely busy scene must **not** be filtered (an absolute threshold dropped half a tp850 run and kept only its easiest seconds - that mistake has its own planted test), a frame window containing a screenshot must be dropped, two logs from different launch memory states must be called out, and **a log holding two runs must be called out** - `relay.sh log` appends, so fetching a job twice concatenates them and the scores silently become an average of two runs (job 1887, 2026-11-01; its planted test uses the same build in both halves, because de-duplicating the header values would not have seen it). |
| **Any change at all** | `python3 core/tools/docs_stale.py` (rule 7: headline speeds agree across AGENTS and README, one build id with a not-comparable note, frames against `speed_pct`, every option the docs name is really parsed, every `CNN` referenced exists and every shelved one keeps its trigger). Mechanical only - rule 7's table is the part done by hand. `--selftest` plants a fault per check. |
| 3DS host code | `bash core/host3ds/build.sh` with no new warnings. Without devkitARM (a cloud session), `bash core/tools/host3ds_syntax.sh` first: it compiles 8 of the 15 files `-fsyntax-only` against the stubs in `core/tools/stub3ds/` and catches plain C mistakes (on 2026-10-30 a `double tot` declared inside an `if` block broke a build and cost a device session). It is not a build, and never replaces one before a relay push. |

Every check that compares has a planted-bug self-test; a check whose self-test cannot fail proves nothing (the first
`VCACHE_PLANT` did not: its planted change never altered vertex data).

## Measuring

On a console: `docs/REMOTE_RUNS.md` (relay, job options). Typical:

```bash
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 600 --ab fcache
bash core/tools/relay.sh log <job>
python3 core/tools/perf_heavy.py core/out/relay_<job>.csv --by opt --cols gxwait_pct,valloc_pct,vwork_pct
```

- **Gameplay benchmark:** `--state sdmc:/gc3ds/states/tp_ordon.state --seconds 300 --press` a square walk of the main
  stick (`SUP@40-159,SRIGHT@160-279,SDOWN@280-399,SLEFT@400-519`, repeated every 480 fields; `--press` counts fields
  from the run's start). New gameplay states are made by steering a run live: `relay.sh cmd JOB "press A 6"`,
  `"stick X Y N"`, `"save sdmc:/gc3ds/states/NAME.state"`, with `"shot"` and `relay.sh frames JOB` to see where it is.
- **Heavy gameplay state:** `--state sdmc:/gc3ds/states/tp_area2.state` (Ordon's heaviest area; no `--press` needed).
- **The walk cannot judge a change that shifts the guest's timeline** (a port's or stand-in's cycle charge, event
  timing): `--press` is applied when the guest polls the pad, so a moved clock lands the poll in another field, Link
  walks a different path, and the scenes differ by tens of percent in draws a picture (C41, jobs 1423-1432: three runs
  of one setting agree to a draw or two, the arms diverge 4 s in). Use the heavy area, which takes no input, or check
  that the arms drew the same scene (`draws_s / (fields_s / 2)` at matched `emu_s`) before believing a walk.
- **Separate whole runs decide defaults:** `python3 core/tools/runs.py core/out/relay_<job>.csv ...` (gameplay: settled
  speed, fields and real frames a second, VI fields per picture) and `python3 core/tools/title_sections.py ...` (tp850:
  cutscene, later stretch, movie). Queue the change's run and a baseline on the same build.
- The A/B switch flips every 20 **rows** of perf.csv (`AB_SECS` is a row count, not seconds: a row is emitted on the
  first loop pass after a second, so a phase is 21-27 s depending on how long a field takes). Each row records which
  half it is in - which is why the phases are read out of the option column and never by binning the log into fixed
  blocks. The option column is the `--ab` name's own column where it has one, or the bit the name sets in a shared
  one (`hlegd` is bit 1 of `hle`, so the arms read 383 and 381); the header line names the A/B that actually ran, and
  says so loudly when the build had no such name. **Score it with
  `python3 core/tools/ab_phases.py LOG COLUMN 60 ON_VALUE`** (each phase against its neighbours, from 60 s), not with
  pooled means (they showed phantom gains); `core/tools/ab_split.py --col NAME` for the tp850 state (cutscene / later
  stretch). Read "speed" and **core 0's own speed** (`speed / (1 - gxwait_pct)`): while the GX side limits the game, a
  faster core 0 only turns into waiting. A/B switches that flush compiled code (stand-in groups, JIT options, ports)
  stall for seconds: ab_phases.py drops each phase's first 12 s, and in the heavy area such A/Bs still read the on arm
  ~2% low, so confirm them with separate whole runs (two heavy-area runs of one build agree to ~0.1%).
- **Frames:** the game is locked to two VI fields a picture ('# new frames hist': field gaps), so while it stays
  locked settled speed_pct is the frame rate; when a change unlocks it, only the '# new frames 10s' line counts (real
  frames a second since 10-19; before that the line was broken and read ~6.9 in the heavy area at any speed).
- `# ...` lines in perf.csv report every 10 s: vertex worker phases, job ring waits, display lists, caches, JIT code
  size, fallbacks, frames ended early, EFB copies, textures. With `--profile on`: hot guest code, guest stacks (map them
  with `core/tools/stack_funcs.py`), where the GX thread is (`# gx thread where`) and the decoder's time by command
  class (`# gx decode`). The host profile's clock is sampled (1 pair in 16, per-draw timers 1 in 64): the columns are
  scaled estimates. PMU columns (`icmiss_k`, `inst_m`, `stall_inst_m`) are sampled on some rows.
- **The cycle model** (no console needed): run the ARM build under qemu with `JIT_REGSTAT=1 JIT_SCHEDDUMP=path`, then
  `python3 core/tools/arm11_sched.py path --cycles --top 40`: host instructions and modelled stall cycles per guest
  instruction, by guest opcode. Use it to rank and check ideas before spending console time. **Its blind spot is
  instruction fetch**, which costs core 0 ~10 cycles per guest instruction on the console: a change that grows hot
  code can lose on the console while winning in the model (float lanes, job 132). Report hot words per guest
  instruction (the `JIT: kept blocks` line) next to the model.

## Where things stand (docs/research/MEASUREMENTS.md has the newest numbers)

- **All defaults (31 October 2026, build 2ba59806b89d7a22, frames a second; the `speed_pct` in brackets; two runs an
  arm, scored with the stolen-second filter; jobs 1690-1699):** static Ordon 10.55
  (35.2%), heavy area 7.28 (24.3%), Wind Waker 6.23 (20.8%), Mario Party 4 9.27 (15.5%); the title: cutscene 40.9%,
  later stretch 23.0%, movie 14.8% (the cutscene ends at ~150 s: windows must follow it). The previous build
  (103456ecbdfe9264) read 10.54 / 7.22 / 6.24 / 9.28 and **must not be differenced against these**: the device drifts
  ~2% between days and a rebuild moves a baseline ~0.8%, so compare runs of one session on one build only. **Those two
  builds have the same defaults**, so what lies between them (Ordon +0.1%, heavy +0.8%, Wind Waker -0.2%, Mario Party 4
  -0.1%) is that drift with nothing else in it - a useful scale for how large a "gain" has to be. Within this session
  the arms agree to 0.00-0.22%. The
  scripted Ordon *walk* also cannot be compared across a change that shifts the guest's timeline (C41).
  **Both baselines below also need a launch rung attached** (C55, 2026-11-01): jobs 1723-1732 are all on the
  **7.1 MB** rung, and tonight's default runs on the **9.0 MB** one read ~5% lower at unchanged defaults, so the two
  are not differenceable. Every figure in this project needs its rung, and **they are all recoverable from the logs
  already on disk** (`# heap: largest malloc block`, which `runs.py` now prints, and
  `python3 core/tools/rungs.py --by-rung` for every run at once) with no device time.
  **That audit is done and the ledger is sound: nothing straddles a rung** (C55). The headline (1690-1699), the
  port-bit apportionment (1700-1711), bits 9 and 10's withdrawal (1667-1668) and rule 3's noise floor are **all on
  the 2.9 rung**; the cloud baseline (1723-1732) is **all 7.1**; the 2026-11-01 work up to job ~1900 is **all 9.0 or 8.9**.
  **That audit line is only true to job ~1900** (corrected 2026-11-01 after an audit): the last stretch of
  that day spans three rungs - `--gpruns` and C40 on **8.9**, `FAST_BITS` 13 unpadded on **8.8**, and the
  padded `FAST_BITS` pair on **7.1** - and rung **8.9 is 1.7% below 7.1**, measured with effectively one
  binary (jobs 1920-1925). So read every absolute from that day with its own `heap` value beside it, which
  `runs.py` prints; the differences within each pair stand because each pair shares a rung.
  Since 2.9 performs like 7.1 and 9.0 is the slow one, the two baselines remain comparable in rung terms - the
  toolchain is the only difference between them - and tonight's absolutes sit below both because of the rung.
  **There is now a second baseline, for cloud sessions** (commit `088fb0df6610`, devkitARM 16.1.0, jobs 1723-1732,
  7.1 MB rung):
  static Ordon **10.61**, heavy area **7.35**, Wind Waker **6.25**, Mario Party 4 **8.83**; cutscene 41.10%, later
  22.99%, movie 14.80%. **The headline above stays on the project's own toolchain** - the cloud toolchain is
  +0.3 to +1.0% on TP and Wind Waker but **-4.7% on Mario Party 4** (gcc 16 compiles `vfast.c`'s vertex path about
  half as fast), so putting its numbers in the headline would read as an emulator regression that does not exist.
  Measure against whichever baseline your build's `# build: gc3ds <commit>` line belongs to, and never across them.
  **Core 0 is the limit in every scene:** the joint-state line ('# joint 10s') shows it
  running guest code 93-97% of the time and almost never waiting; the GX thread is idle 12-13% in gameplay and 40-75%
  in the title, the vertex worker idle ~70-96%, the GPU ~5-15 ms a frame. In the title's later stretch the ShdwDraw
  walk is mostly replayed (C32) and each triangle's path runs natively in a fast form (C32b): the stand-in is replay
  ~7% + path ~10-11% of core 0 there, plus the native callback; the cutscene's moving shadows cost under 0.2%.
- **Native library ports** (`core/src/cpu/port_j2d.c`, `docs/research/LIBRARY_PORTS.md`): J2DMaterial::setGX,
  J2DPictureEx::drawSelf, J3DShapeMtxConcatView's matrix load, the J3D lights (`diffLightObj`, heavy area +2.7% -
  **superseded, do not add it to bit 6's: with `J3DMaterial::diff` on, this hook runs 84 times in 240 s, exactly the
  cases bit 6 declines, so its credit is inside bit 6's +3.0%; it stays on because it is exact and free**), the
  J2DPane::draw walk, all of J3DMaterial::diff (heavy area and Ordon +3.0%), J3DSys::reinitGX (Ordon +3.9%) and the
  minimap's renderingDecoration (Ordon +3.1%, heavy area -0.4%), on by default (`--port-hooks 243` since 2026-10-30, when the PNGP matrix load and the minimap decoration came off: together +0.71% in static Ordon, +0.55% in the heavy area - **apportioned one bit at a time on 2026-10-31, three runs an arm, jobs 1700-1711: the minimap decoration (bit 8) is the whole of it at -0.26%, with all three of its runs below all three baseline runs, while the PNGP matrix load (bit 2) is neutral at -0.03% despite being the most-called port in the scene by 6x (1.53 M calls, ~5.8% of core 0 by its own timers, for nothing - the clearest case yet of a port that copies the SDK step by step), and the `__GXSendFlushPrim` port (bit 3) is neutral at +0.08%**); bits 2, 3, 8
  (`__GXSendFlushPrim`), 9 (the skinned-shape matrix load) and 10 (HUD text) are built, exact and off. **Bits 9 and
  10's +1.3% is withdrawn (2026-10-31):** re-measured at the then-current default they are **-0.46%** in TP's heavy
  area (jobs 1667-1668) - **and that -0.46% is itself superseded: bit 9 alone reads +0.08%, i.e. neutral, at the
  post-C53 default (jobs 1845-1848), while bit 2 moved the other way from -0.03% to -0.94% (jobs 1840-1843). Two
  matrix loads in one subtree, re-measured together, moving a point apart in opposite directions** - where they read
  +0.64% with `--psflush` off - a sign flip from C49's interaction, since the old
  +0.9-1.0% / +0.8-0.9% figures (jobs 1491-1497) were taken at a baseline that no longer exists. **Wind Waker's
  +3.2% for 243 over 503 is also withdrawn:** it came from one run truncated by a full card (8 scored windows of
  15), and clean runs make it neutral (jobs 1669-1672). 243 is still right, now on the heavy area's +0.42% at the
  current default. Each is checked
  exact on the 2DS itself with `--port check` (every call also run by the interpreter and compared; `--port plant`
  proves the checker reports a difference). A port pays only when it does less than the guest code, not when it copies
  the SDK step by step; it must also copy what the guest code leaves that the ABI does not promise (PSMTXConcat's ps1
  lanes of f14, f15, f31) and not run where the guest would take an exception (MSR.FP off).
- **Defaults that measured as gains:** **a contiguous run of pipe bursts moved at once rather than 32 bytes at a
  time (`--gpruns`: TP heavy area +0.47%, static Ordon +0.43%, both rank-separated with spreads 0.01-0.11%;
  jobs 1889-1897. `--port check` differed 0, and Wind Waker is a clean negative control - the run path never
  qualifies there, 0 of 8.87 M bursts, and the speed moves -0.01%. The gain is core-0 work and the mechanism
  is streaming: C40's PMU split blames data stalls on the FIFO mirror, a large always-cold ring, and one
  contiguous memcpy into it beats 476 interleaved 32-byte ones)**, **PSMTXConcat exactly in soft float for a denormal / NaN / infinity input
  instead of giving up (`--port-softfloat`: heavy area +0.20% with all four on-runs above all four off-runs, Ordon
  +0.31%; jobs 1797-1808 - it also removes the give-up class entirely, 60 refusals a window to 0)**, **C53's
  pane-walk subtree - `--port-picture` and `--port-walktext` together
  (static Ordon +3.46% and +3.17% on two builds, heavy area +2.31%; jobs 1760-1767 and 1785-1788, every on-run above
  every off-run, all arms inside one launch memory state)**, draw merging (+24%), check hoisting (core 0 +5%), the job-ring and TEV work in
  HANDOFF, matrix indices apart from vertex records (VIx), the native stand-ins (shadow scan with a leaf memo, the
  paired-single library, J3D display-list writers, THP decode), PE register reads that no longer wait for the GX thread
  (cutscene +3.2%), pipe stores at known addresses (`OPT_PIPEK`, gameplay +7.0%), raw draw calls and state, mipmaps
  (heavy area +19%), small texel formats (+6%), CPU-lit draws and then all lighting on the vertex worker (+6.8%, +2.9%),
  textures in VRAM (+12%), vertex set-ups cached for 8 keys (+1.1%), uploads without a divide per texel, the
  measurement-only decoder statistics and profile clock out of normal runs (+3.2%, +6%), a 2 MB command buffer (+2.6%),
  the library ports (as a set +10.6%; per port see the ledger in HANDOFF, the old per-port figures were walk artifacts), pipe bursts copied whole (`gpbulk`, +3.2% / +4.5%), GX-thread
  ring records batched (`ringbatch`, heavy area +2.6%), overlapped GPU frames (`dblframe`, heavy area +1.9%), float
  lanes (`fcache`, +1.5% / +1.6% / title up to +2.4%; rejected on 10-13 from A/Bs that recompile, reversed by separate
  runs), the 2-way indirect table (`ind2`, ~+0.9%, likewise), compact vertex records (`vin`, ~+0.7-0.9%), rlwinm as
  shifts (`rotmask`, ~+0.3-0.6%), the __GXSendFlushPrim stand-in (`--psflush`, static Ordon +0.40%, TP heavy +0.85%,
  Mario Party 4 and Wind Waker neutral; turned off on 2026-10-30 on pre-filter scoring and **reverted the same day**
  once the stolen seconds were removed - Ordon's sign flipped), the ShdwDraw walk memo (`shdwmemo`, later stretch +14.2%, C32), the
  ShdwDraw's triangle
  path natively (`shdwtri`, later stretch +2.0-2.7%, gameplay +0.2-0.3%, C32b) and its fast form (`trifast`, both
  chains in registers: later stretch +3.8%, +7.7% pictures, gameplay +0.2-0.4%), the shadow callback natively, read
  stage by stage (`shdwcb`, later stretch +1.1%, Ordon +0.7%).
- **Added 24-25 October (MEASUREMENTS.md, HANDOFF):** ports, on: the J3D lights with a one-copy commit (heavy area
  +2.7%), all of J3DMaterial::diff (heavy area and Ordon +3.0%), J3DSys::reinitGX (Ordon +3.9%), the minimap's
  renderingDecoration (re-measured 2026-10-30: ~0 in Ordon and **-0.5% in the heavy area**, a candidate to turn off
  with the PNGP matrix load; both are in the ledger). Built, exact, off: the skinned-shape
  matrix load (bit 9, Ordon -2.9%) and HUD text with J2DPrint / JUTResFont (bit 10, Ordon -4.3%). **A port's charge diverts the
  scripted walk rather than making a scene cost more** (C41, answered 2026-10-29: in Ordon the emulated clock is 79%
  poll fill, so a charge is absorbed by a smaller fill; flipping it 100 against 330 inside one run moves frames a
  second by 0.8%, while separate walk runs swing up to 20% because the presses land in another field and Link walks
  elsewhere). Judge a port on a scene that takes no input, never on the walk. The
  FIFO mirror in guest RAM costs **Ordon +1.24% and TP's heavy area +0.96%** at the current defaults (C40,
  re-priced 2026-11-01, jobs 1908-1915, every spread 0.01-0.06% with complete rank separation; the old
  figures were 1.6% and 0.5%, so the heavy area nearly doubled while Ordon came down - a fixed memory cost
  grows as a *share* when core 0 gets faster, so "this is worth less now" is not a safe inference either
  way). `--fifomirror off` is measurement only, so those are upper bounds - but C40 has a written exact
  design (core 2 writes the mirror, since it already has the bytes) and is now the best-evidenced unbuilt
  idea on the list.
- **Added 22-24 October (details in HANDOFF; `--tier` turns out to remove 87.5% of indirect-table probes in a
  many-caller leaf, qemu 2026-10-31, so part of its gain may be fewer indirect branches rather than the inlining -
  C48):** regions of executed code with calls inlined (`--tier`, on: Wind Waker
  +7.2%, TP heavy -0.9%), textures re-checked by page stamps (`--texfast 2`, on: Mario Party 4 +4.2%, TP +0.6-0.9%),
  EFB copies hashed only when reused (`--copystamp`, on: TP +0.7-0.9%), the J2D pane walk (on: Ordon +2.2%). Built, off:
  pre-decoded display lists (`--g2 on`, exact - re-checked on the current build, 0 differed over ~5,400 replays a
  window, plant 845-936 - and **scene-dependent rather than neutral: TP's heavy area +0.53%, static Ordon
  -0.28%**, both rank-separated with on-arm spreads of 0.01% and 0.12%, jobs 1867-1870 and 1885-1888. October's
  "no speed change" came from the pre-filter method with `--profile on`, and in the heavy area the baseline has
  also moved in g2's favour (core 0 waited on the GX thread 0.4-1.2% then, so 1.3-2.5 points off GX busy had
  nowhere to go). **Ordon loses while replaying more** - 90.5% of list calls against the heavy area's 68.5% - which
  is `--dlcache`'s lesson from the other side: where the GX thread already has slack, the saving has nowhere to go
  and the replay's own work lands on the core that is the limit. Net ~+0.1% across TP's two scenes, **stays
  off**), `--nskeep` (**on by default since
  2026-10-30, Mario Party 4 +1.2% - job 1559 against 1560; the +1.9% of 10-24 is superseded. This list said
  "built, off ... owner's call" until an audit checked it against `pica.c:3366`, which has
  `g_nsKeep = 1`**), fault mode (`--faultmem`, +2-5% but crashes the kernel in svc 0x94), and the
  region tier's nursery (`--tier-nursery`: even in the heavy area, Ordon -1.9%, rejected with its relocation kept as a
  tool).
- **Built, off, measured without a clear gain:** display-list vertex cache, known-single stores (on; +1%), the
  __GXSendFlushPrim *port* (neutral, separate runs; the J3D light port turned positive with its one-copy commit -
  not to be confused with the `--psflush` *stand-in*, which is on by default and measured +0.40% / +0.85% in the two
  TP scenes on 2026-10-30, jobs 1644-1659),
  the cXyz operator stand-ins (`psxyz`, heavy area -0.9%), and re-tested with separate runs on 10-19 and 10-20: crstub
  (-1.0%), befloat (-0.8%), calls (-1.2%), fastfloat (-0.6%), relayout (-7.6%), traces (-5.1%), vw2 (-2.3%), steal,
  sched off, dlcache, vwmem 256 (-0.65 to -0.75%), pipeps, icbi, qbig, feeddirect, cmdpld (even),
  frame skip (+10%, visual glitches; a candidate user option), byte-swapped guest RAM (`GC_SWAPRAM`, neutral).
- **Rejected with measurements:** second vertex worker on core 1 (evicts core 0's L2), shared-index fast path (TP: <1%
  of vertices), CPU display-list cache as a speed lever (RAM latency), matrix-chain HLE (flat profile), AOT translation
  (slower on the console), translating call-heavy small functions (entry and exit cost more than the JIT's code). See
  REDESIGN_CANDIDATES for the numbers.
- **Also measured without a gain (10-17/18):** lean citro3d state, command-buffer prefetch, psq_st to the pipe in the
  stubs, GX setters as stand-ins, black-light skipping (exact, kept: less worker time), a bigger
  job ring, a 128-entry GX queue, other early-split sizes, the L2C-310 prefetch offset, function memoization of the HUD
  (its inputs change every frame, C28).
- **Core 0's whole budget, measured (job 1569, TP heavy area, today's defaults; MEASUREMENTS' job-1569 section):**
  **80.9%** JIT block code and the helpers blocks call, **9.65%** port C code (7.05% in `port_run`, which for a
  subtree port *is* the work, 2.61% in `port_commit`), **1.14%** stand-in C code, 0.6% `gxsync`, 7.4% not running
  guest code. Inside that: **IPC 0.334, instruction-fetch stalls 27.4% and data stalls 22.1%, so half of core 0 is
  memory** - the fetch half is 8.20 M L1 I misses a second served by L2, the data half core 0's 1.83 M L2 misses a
  second to FCRAM. 7.07 host instructions and 1.37 host data reads per guest instruction; one dispatcher entry per
  4,742 guest instructions.
- **What that rules out:** no code *layout* change fixes fetch (50% of time in compiled code is 54.8 KB over 84
  pages, 3.4x the 16 KB L1 at any density; the nursery's packing bought 4% of fetch stalls = 1% of core 0), and **no
  peephole gets the -15 to -20% of hot words that +10% of core 0 would need** - a `bc`'s 12.2 words are the block's
  one shared `rc_flush` (handover modelled ~1.3% net), the paired-single accesses already went through OPT_FCACHE
  and OPT_PIPEPS, and everything else measured 0.3-2% of hot words (C13, C44). **Core 0's codegen is not where a
  large gain is**; `core/tools/jitbench.py` still ranks candidates cheaply with no disc image (OPT_FDIRECT came
  from it, C42).
- **The unknowns now have measurements attached (owner's direction, 2026-10-30: "the more things that are unknown to
  us right now we need to put measurements on").** Each is an instrument or a model run, not a build, and the cheap
  one comes first: **C47** attribute core 0's data stalls - 22.1% of the core with only totals in the record, split
  into guest RAM / Gekko state / JIT metadata / the GX ring, because only some of it is ours to fix (PC cache
  simulator, no device time); **C48 is answered** (job 1916, TP's heavy area): the table takes
  **798,300 probes a second**, 6.16 per 1,000 guest instructions, **80.9% first slot, 14.0% second, 5.2% missed**
  - a 94.9% hit rate - with 84% of probes coming from `blr`. The dispatcher's own miss counter (41,134/s) and the
  probe counter (41,512/s) agree to 0.9%, which is the cross-check C48 was designed around. **OPT_IND2's +0.9% is
  thereby explained**: the second slot serves 112,000 probes a second that would otherwise miss. **The `FAST_BITS` sweep is
  done and says keep 16** (jobs 1917-1925): a table eight times smaller (fb13, 64 KB each) **misses 44% less -
  2.9% against 5.2%, at identical probe rates - and is 0.71% slower**, so the size is not a cache-footprint
  problem and the miss rate is not what costs. The likely reason it misses less is the **247 table resets a
  second**: 65,536 entries cannot warm up between resets where 8,192 can, which makes that miss rate a measure of
  warm-up rather than capacity. (Also correcting a number this file had wrong: the allocation is
  `FAST_TABLES * FAST_SIZE * sizeof(FastEntry)` = **4 x 512 KB = 2 MB, the whole shared L2** - 512 KB is one table
  of four.) **What C48 did not explain is the sampler's `dispatch`
  bucket at 8.6-8.7% of core 0**: that is ~1,600 cycles per dispatcher entry over 43,352 block runs a second,
  which is not credible for a hash lookup, so the bucket holds more than the loop (the JIT's C helpers run with
  `g_where` left at `WH_DISPATCH`). **That is the largest unattributed number in core 0's budget** and wants
  `g_where` set in the helpers; **C49** why switch effects are sub-additive (the four defaults' parts predict +1.04% in Ordon and
  deliver +0.36%), which decides whether a stack of individually-measured gains can be added at all; **C50** the data
  micro-TLB at 3.09 M misses a second, 5.9% of data reads, fourteen times the instruction side; **C51** spilling GPRs
  to VFP registers instead of memory (5 host registers for 32 guest GPRs is worse than any comparable JIT), modelled
  first because ARM->VFP transfers are already 8.5% of modelled stalls and it may lose outright; **C52** the pacer's
  systematic overshoot. **A result that kills one of these is as valuable as one that builds it** - the point is that
  none of them is currently knowable.
- **Where core 0's guest time actually goes, by subsystem (job 1750, the first decoded guest *stack* profile,
  2026-10-31).** `stack_funcs.py` needs zeldaret's symbol map, which lives outside the repository and had never been
  fetched into a session, so inclusive time per subsystem had never been read. Past the framework spine the tree is
  ~50% under the actor method dispatch and ~36% under the painter, and the self-contained subtrees are:
  **`J2DPane::draw` <=9.5% inclusive / 5.49% leaf (C53 - and that 5.49% is the largest single leaf in the
  profile)**, **the J3D joint calc tree ~5.2% (C54, unexplored)**, `dMeter2Draw_c::draw` 9.53%,
  `J3DDrawBuffer::draw` 10.0%, `J2DScreen::draw` 8.49%, the minimap ~6.9%, `J3DShapePacket::drawFast` 6.92%.
  **Read a recursive function's inclusive share as an upper bound inflated by its nesting depth and quote its
  nearest non-recursive ancestor instead** - `recursiveCalc` read 11.42% against its parent's 5.17%, and C54 was
  opened at twice its real size before the parent was checked (corrected the same day; `stack_funcs.py`'s docstring
  now says this where it cannot be missed).
  **Two consequences.** First, **C53's prize is several percent of core 0, not the ~2.2% on its ledger line**: a taken call
  produces no guest samples, so `J2DPane::draw`'s inclusive share *is* the refused walks, and the +2.2% measured
  what the walk captures at ~29% coverage rather than what the rest is worth. Second, **inclusive and leaf profiles
  answer different questions** - a subsystem that is 11% inclusive and 0.9% leaf is a subtree port, and it is
  invisible to a leaf profile, which is how "matrix-chain HLE (flat profile)" came to be rejected (C54).
  **Third, added 2026-11-01 and the one that should change behaviour: an inclusive share is not a prize and is not
  even a reliable upper bound.** There are now two cases where a subtree was ported and the realised figure can be
  set against the share that motivated it, and they do not agree with each other:

  | subtree | inclusive share | realised | ratio |
  |---|---|---|---|
  | `J2DPane::draw` (C53) | <=9.5% | **+2.3 to +3.5%** | ~3x over |
  | `J3DDrawBuffer::draw` (C56) | 9.88% | **-0.79%** | no ratio exists |

  C56's case is the instructive one, because the arithmetic can be closed independently: the subtree is
  **3,010.6 guest instructions a call** (check mode, job 1875) at ~144 taken calls a second, which is **1.41%** of
  core 0's block-code instructions (job 1569's budget: IPC 0.334, 80.9% block code, 7.07 host instructions a guest
  instruction = 30.7 M guest instructions a second), or **~2.7%** for the whole subtree at the ~53% coverage
  measured in jobs 1881-1882. For 2.7% of instructions to be 9.88% of samples those instructions must cost ~3.6x
  an average one - and the port replaces them at **19.5 cycles each against compiled code's 21.2**, which says
  they do not. **The measurement is ground truth, so the inclusive share is the instrument at fault**, by a factor
  somewhere between 3x and infinity.
  **So rank a subtree candidate by work per call and by what it can skip - never by its inclusive share** (which
  AGENTS already said for *call count* after the PNGP matrix load, and the same correction now applies to the
  profile). Before sizing a candidate off an inclusive share, get its guest instructions a call: check mode gives
  it exactly for a port, and `--port-size-refused` gives it for a refusal. C54's 5.15% has had no such number
  taken, so **C54 is currently unsized, not second-ranked.**
- **Open directions, in the order the budget supports them:** removing guest code wholesale - **larger ports where a
  whole subtree is replaced** (other TEV block types, J3D packet draw; `docs/research/LIBRARY_PORTS.md`) - but
  **"other TEV block types" has never actually been sized, and the number that looked like its size was a label
  bug** (2026-10-31): the pane walk refuses 5,600 calls per 10 s in the heavy area with 4,720 under the reason
  perf.csv called "tev block", and that reason has **21 sites** of which one is a TEV block, while `otherTev` - the
  only field recording a real unported TEV block - reads 0 in those runs. `# port give-ups 10s` now names the
  refusing line, and the answer came back (**C53**, jobs 1738-1740): 100% of the pane walk's refusals are its
  pane-class test, and that is **exactly two classes with no tail** - `J2DPicture::drawSelf` (108 bytes, whose
  derived `J2DPictureEx` is already ported) and `J2DTextBoxEx::drawSelf`, **which is already built and exact as port
  bit 10** and is off only because it lost as a standalone *entry hook*. Folding it into the walk is the subtree
  direction with the component already written: coverage could go from 1,450 taken / 3,500 refused to near all of
  them, ~3.4x, on a port credited +2.2%. **C53 is now built, exact and measured: static Ordon +3.46%, heavy area
  +2.31%** (jobs 1760-1767, two runs an arm, second round reversed; all four on-runs above all four off-runs, spreads
  0.01-0.10%) - the largest single gain since the ports as a set, and the first completed instance of the subtree
  direction. The walk went from 13,377 real guest instructions a call to **104,292** and its pane-class test now
  refuses nothing. Two lessons generalise: **a subtree is all-or-nothing**, so the classes a walk refuses are not
  independent quantities to attack one at a time, and **coverage is not value** - the J2DPicture half of the same
  work accepts 412 panes a window and draws none, every one being hidden. **On by default since 2026-11-01**, after a
  second measurement on a different build with adjacent arms (+3.17%, jobs 1785-1788) and `--port check` on the
  default build itself. Its absolute figures belong to the 9.0 MB launch rung (C55): the device chainloaded into
  that rung both times, so what C53 gives on the 7.1 rung is unmeasured. **And its heavy-area figure is
  build-specific: +2.31% on `8aa6dc7`, +1.97% on `787ed374`** (jobs 1836-1839, same scene and rung) - both right
  where they were taken. That 0.34-point spread is what a rebuild moves, and reading it as sub-additivity is how
  C49's first test went wrong before all three configurations were put on one build. The
  direction itself still holds: the ports as a *set* are +10.6% while their parts sum to ~3.7%, and small functions
  lose as entry-hook stand-ins - the PNGP matrix load is the proof, 1.53 M calls and ~5.8% of core 0 for -0.03%, so
  **rank candidates by work per call, not by call count**. **The "denormal give-up" that CONTRIBUTOR_TASKS 9 called a
  prerequisite for big ports is not one** (corrected 2026-10-31, jobs 1753-1754): with the counter split, the ports'
  float give-ups are **100% NaN or infinity and zero denormals** (Ordon 13,216 all-time, heavy area 9,544, all in
  `psmtx_concat`), they are ~11% of give-ups against the pane-class test's 82%, and matching Gekko's NaN propagation
  costs far more than flushing a denormal would have. **But C53 re-priced it the same week, upward** (2026-11-01):
  with C53 on, that same give-up refuses **37.5% of the pane walk's hook entries in both scenes** and each refusal
  now abandons a ~104,000-instruction subtree instead of a ~13,000-instruction one, so it is the walk's biggest
  addressable refusal and worth 2.2x its coverage. Both readings are right at their own time, which is the point:
  **a candidate's value depends on which other options are on, so re-rank after every default lands, not once** -
  C49 with the sign reversed. **The remedy is built, exact and ON by default since 2026-11-01**
  (`--port-softfloat`, `psmtx_concat_exact`): heavy area **+0.20%**, Ordon **+0.31%**, and the give-up class is gone
  (60 refusals a window to 0). **But its measured value is a tenth of what the re-pricing above predicted**, and the
  reason is the recurring one: the refused calls never became taken calls, they vanished from the hook because walks
  that already succeeded absorbed them, so coverage rose 1.4% rather than 2.2x. CONTRIBUTOR_TASKS 9 carries the whole
  history - premise wrong, then size wrong twice in opposite directions - as the worked example of why only separate
  whole runs settle a port's worth;
  the ports' `port_commit` (2.61% of core 0, and the J3D light
  port's one-copy commit turned it from negative to positive at the time, so the technique is proven and unapplied
  elsewhere - though that port's +2.7% is now inside bit 6's, and PNGP's commit is already one copy, so the next
  place to apply it has to be found rather than assumed); C40
  (~1.6% of Ordon in every game); the ShdwDraw stand-in's code footprint (C32b); GPU-resident vertex data for
  repeated display lists (C1).
- **The next subtree port is sized and it is `J3DDrawBuffer::drawHead` (C56).** Re-ranked after C53 landed (job 1821):
  the J2D side has given what it has - `dMeter2Draw_c::draw` fell 9.53% -> 7.05% and `J2DScreen::draw` 8.49% -> 5.93%,
  which is C53's +2.31% seen from the other side, and those two were never independent prizes but the pane walk
  through its containers. **`J3DDrawBuffer::draw` is 9.88% and untouched**, with `drawFast` 6.87% inside it; C54's
  joint tree is second at 5.15%. C56 has the pane walk's exact shape (two loops, a virtual `packet->draw()`, **four**
  packet classes with no tail), a spine of only ~1,730 bytes, and the strongest argument of any candidate on this
  list: **its inner loop calls port bits 2 and 9**, the matrix loads that measure **-0.94% and +0.08%** as standalone
  entry hooks at the post-C53 default (jobs 1840-1848; they read -0.03% and -0.46% at the older one), so a subtree
  context would absorb them the way the pane walk absorbed bit 10 (-4.3% alone, part of +3.4% folded in).
  **Built, exact - and measured as a LOSS: -0.79% in TP's heavy area** (1 November 2026, port bit 11 / 2048, off;
  jobs 1875-1880. Check: 180 calls, differed 0, `drew 180 calls, 920 mat packets, 1,640 shape draws`; plant 28,028.
  Speed: on 7.142 / 7.139 against off 7.206 / 7.189, rank-separated the wrong way).
  **The reason is one number and it restates this whole direction.** The port's own timers read 72.9 us a call =
  58,612 ARM cycles to replace 3,010.6 guest instructions = **19.5 cycles per guest instruction replaced, against
  the JIT's 21.2**. The port is 8% faster than compiled code at the same work, and 8% cannot pay for a hook entry,
  a commit, a compiled-code flush and 1.2 KB more fetch footprint. **C53 did not win by being a subtree; it won by
  doing less** - skipping hidden panes, absorbing a text path, 13,377 -> 104,292 guest instructions absorbed per
  call. C56 copies `drawHead` step by step. Bit 2 at -0.94% per call and C56 at -0.79% per subtree are the same
  finding at two scales: **absorption removes a hook's overhead, not the work.** So rank a subtree candidate by
  *what it can skip or compute differently*, never by its inclusive share.
  **A "20x instrument gap" was published here the same day and withdrawn the same day**: job 1850's census read
  `J3DMatPacket::draw` entered 413 times where the scan said 8,177, and two runs on the current build read 5,994
  and 6,480 (jobs 1881-1882) - the same order as the scan. `--tier` inlining past the hook was the hypothesis and
  **it is dead**: every counter in those two runs differs by exactly 1.081, `drawHead` included, so the per-frame
  structure is identical with region inlining on and off. One 70 s pair killed it.
  **What that pair did establish: the port covers about half the work.** With C56 taking 1,332 of 1,554 drawHead
  calls and drawing 6,808 mat packets, the guest still entered `J3DMatPacket::draw` 5,994 times, and the 222
  refused calls account for ~1,130 - so ~45% reaches it by another route (`drawTail` is the candidate, now census
  slot 2). **Coverage is still not the problem**: 8% faster than compiled code over twice as much work is still
  8%, so the next attempt has to make the subtree *cost less*, not cover more. **9.88% inclusive against ~2.2% of
  core 0's instructions remains unexplained, so an inclusive share still sizes nothing - C54 included.** The
  zero-hash mechanism C56 needed is now general and documented in the checks table.
- **Guest frame skip: built, exact, off, and shelved as a *speed strategy* - a user option instead (C9d, the
  owner's decision of 2026-10-31 after watching six runs: "let's just get things at native speed and keep working
  at improving performance").** The first human look at it settled three things the arithmetic could not.
  **The trade is four points, not a dial:** a picture lands only on a game-frame boundary and a frame costs `cd`
  drawn or `cs` skipped, so the achievable intervals are `cd + n*cs` - in static Ordon (cd 92.5, cs 34.6 ms) that is
  10.8 pictures a second at 36% speed (draw all), **8.4 at 51%** (one in two, job 1722), 6.2 at ~62%, 5.1 at ~68%,
  and nothing between. The owner called 10.8 fine, **8.4 "a little slideshowy on camera pans"**, and 5.1 "very
  jittery". **It helps where the game is already fastest and cannot help where it is slowest:** the useful band is
  between a scene's native picture rate and the rate a player accepts, and TP's heavy area natively shows 7.3 -
  already below 8.4 - so there is no acceptable skipping point there at all. **And the cost lands on camera pans,
  which neither benchmark scene contains** (both take no input), so every number here *understates* it: judge
  smoothness on the walk, speed on the static scenes, never mixed. `--gfskip-snap` (on) rounds the period to a whole
  number of frames, because a fractional one alternates between two intervals and reads as jitter (32% of the mean
  gap at target 50, job 1715). **A perceptual verdict on one run is not reproducible** - jobs 1718-1720 were all the
  unpaced emulator (two latch-ups, C9e) and were called "smoother", "okay" and "choppier"; use a blind A/B inside
  one run. **Re-evaluate when base speed rises**: the band opens from above, so this becomes useful *because of*
  ordinary performance work, not instead of it.

## Code style

- C11, 4-space indent, lines up to ~120 columns, `u8`/`u32`/`s32`/`u64` types.
- Comments say why, with the measurement that justified a choice (job numbers, model numbers). No commented-out code.
- JIT code: hot code must stay small (instruction fetch is expensive); rare paths go to cold code (`cold_begin` /
  `cold_end`); cold code may read the register cache but never changes its state; every exit writes back through the
  `Rc` snapshot. Read the comments on `Rc`, `rc_emit_flush`, `fc_*` and `emit_fallback` before touching them.
- Commit subjects: `type: summary` (`feat`, `fix`, `perf`, `docs`, `tools`, `test`), lower case, with the measurement
  or check in the body.

## Hardware facts worth knowing (measured on a New 2DS XL; FINDINGS.md and GPU_GUIDANCE.md have the rest)

- L1 instruction and data caches are 16 KB each (4-way, 32-byte lines). Linked JIT blocks cost 13 ns while the hot
  code fits in 16 KB and 53-62 ns from 32 KB: **hot code size is a first-class cost**. Data latency: L1 4 cycles, L2
  ~29, FCRAM 126-170.
- Cores run at 804 MHz; the app gets cores 0, 2, 3 and 80% of core 1. Luma3DS maps physical memory 0-0x30000000 at
  VA | 0x80000000 for user mode (uncached, read and write: job 443), so the L2C-310's registers (0x97E10000) and AXI
  WRAM are reachable; the kernel already enables L2 prefetch (offset tuning measured no gain). No large pages; WFE does
  not sleep in user mode. Textures can live in VRAM (6 MB, CPU writes by DMA only).
- A second core reading through emulated RAM evicts core 0's working set from the shared 2 MB L2 (core 0 -6%).
- The PICA200: shader length is free; a fourth vertex attribute costs ~45% of the vertex rate; a uniform change
  between draws stalls ~2 µs; ~1,100 draw calls/s is cheap.
- Luma3DS's homebrew loader is the named port `hb:ldr` (`svcConnectToPort`), not a srv service.
