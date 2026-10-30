# AGENTS.md: working on gc3ds

For people and coding agents. Read this whole file before changing anything; then read the end of
`docs/HANDOFF.md` (the newest state) and `docs/research/REDESIGN_CANDIDATES.md` (what has been tried).

gc3ds is a GameCube emulator for the New Nintendo 3DS / New 2DS XL (ARM11 MPCore, 4 cores at 804 MHz, 2 MB shared L2,
PICA200 GPU). The goal is the highest speed the hardware allows, with **exact** emulation: the interpreter is the
reference, and the JIT and the fast graphics paths must produce the same results. *Twilight Princess* is the stress
test. Today it runs a static Ordon scene at 10.54 frames a second (35.1% of full speed) and Ordon's heaviest area at
7.22 (24.1%); Wind Waker's Outset 6.24 (20.8%), Mario Party 4's board 9.28 (15.5%), the title's cutscene 40.9%, its
later Link / Epona stretch 23.0%, the THP movie 14.7% (30 October 2026, build 103456ecbdfe9264, two runs an arm,
scored with `runs.py`'s stolen-second filter - arms agree to 0.01-0.2%). **These are not comparable with the
previous block's** (10.40 / 7.17 / 6.18 / 9.05 on build 0f3c3eac6e709b6a): a rebuild moves a baseline ~0.8%, older
numbers were scored before the filter, and only runs from one session on one build may be compared. **Judge
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
   build. Such runs agree to **0.07%** once the seconds core 0 did not get are left out, which `runs.py` does by
   default: the program's own periodic screenshot used to steal **4-86% of one second** five times in a 240 s run,
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
   | an instrument's meaning or a column's name | every doc that quotes its output, and the tool's docstring - a label that teaches the wrong reading is a bug (`port_cost.py`'s "building the context", 2026-10-30) |
   | a contributor-visible fact | `docs/CONTRIBUTOR_TASKS.md` (a task that is already done is worse than no task), `docs/TELEMETRY.md`, `CONTRIBUTING.md` |

   **Withdrawing a credit is normal and must be explicit.** When a recorded gain turns out to be an artifact, say so
   where it was recorded and say whether the option stays on - a withdrawn credit is not a rejected option
   (`--hlegd`, 2026-10-30).

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
| `core/tools/` | `perf_heavy.py` (A/B analysis), `clock_split.py` (where a run's emulated clock came from and each field's slack, C41), `port_cost.py` (what each port costs core 0), `jitbench.py` (the JIT's ARM output for a hand-written guest loop, no disc image), `arm11_sched.py` (ARM11 cycle model), `host3ds_syntax.sh` + `stub3ds/` (syntax-check the 3DS host code with no devkitARM), `docs_stale.py` (rule 7's mechanical documentation checks), `relay.sh` (remote runs), `vfast_check.sh`, `arm_*.sh`, `tp_*.sh`, `hammock.py` (block-shape model over a PC profile from gcemu's `PCHIST` + `EDGEHIST` + `MEMDUMP`) |
| `tools/relay-server/` | Reference relay server for remote runs (Node, no dependencies) |
| `bench/` | `gcbench`: 3DS hardware benchmarks (CPU shapes, memory, GPU rules) |
| `docs/` | Designs, findings and the running log (see README) |

Threads on the 3DS: core 0 runs the emulated CPU (JIT); core 1 (80% for the app) the log writer, samplers and relay
client; core 2 the GX thread (FIFO, `vfast_prepare`, PICA command lists); core 3 the vertex worker (`vfast_finish`,
culling, triangle assembly).

## Toolchain

Linux or WSL (Debian). devkitPro at `/opt/devkitpro` (devkitARM, libctru, citro3d, 3ds portlibs: curl, mbedtls, zlib),
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
| Guest frame pacer (`--gfskip*`, `hle_tp.c`) | `GC3DS_GFSKIP_SELFTEST=1 core/build/gcemu`, also run by `run_tests.sh`: it drives the real controller with a synthetic clock and asserts the game lands on its target speed, the drawn frames stay **evenly spaced**, the picture-rate floor holds when the target is unreachable, and the fixed ratio is untouched. Feedback over real time cannot be judged by reading it or by one console run. Planted-bug self-tests that must fail: dropping the skipped-frame cost from the maths, choosing draws by a counter instead of the accumulator, removing the floor, and clumping draws into pairs while keeping the right rate (that last one is the check that protects "looks natural"). |
| Vertex path (`vfast.c`, `xform.c`) | `bash core/tools/vfast_check.sh 2` (all test DOLs, 0 draws over tolerance); deferred CPU lighting: `VFAST_CHECK=1 VFAST_DEFLIT_CHECK=1` on TP (0 differ) and `VFAST_DEFLIT_PLANT=1` / `=2` (must differ) |
| Native stand-ins (`hle_*.c`, `hle_ps_gen.inc`) | `HLE_CHECK=1` on the PC over TP (0 differed; `HLE_STOP_EVERY=N` for the stops inside); after regenerating with `ps_translate.py`, the existing functions must come out byte-identical |
| Display-list cache | `VCACHE_CHECK=1` on the PC build (every hit gathered again, 0 differences) and `VCACHE_PLANT=1` (must report differences) |
| TEV / combiners | `bash core/tests/tev/run_tev_tests.sh` |
| Draw merging | `MERGE_CHECK=1` on the PC build |
| Translated lists (`g2.c`, fifo.c `g2_replay`) | PC, TP title spans: `G2=1` gives the state hash and last frame of G2 off; `G2=check` 0 differed; `G2=plant` must differ. 2DS: `--g2 check` 0 differed, `--g2 plant` differs |
| Native library ports (`port_j2d.c`) | PC: `PORT_CHECK=1` over TP's title states (0 differed) and `PORT_CHECK=plant` (every checked call must differ); 2DS: `--port check` in the scenes the port runs in (the minimap and HUD text are not in the PC states) with 0 differed; hash every guest function the port follows (`core/tools/porthash.py` into `port_j2d_hashes.inc`, code only: run-time tables make the ports switch off); calibrate `kInsScale` from check mode's instructions per call |
| Measurement tools (`runs.py`) | `python3 core/tools/runs.py --selftest`: a synthetic log with a planted steal must be recovered exactly, a merely busy scene must **not** be filtered (an absolute threshold dropped half a tp850 run and kept only its easiest seconds - that mistake has its own planted test), and a frame window containing a screenshot must be dropped. |
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

- **All defaults (30 October 2026, build 103456ecbdfe9264, frames a second; the `speed_pct` in brackets; two runs an
  arm, scored with the stolen-second filter):** static Ordon 10.54
  (35.1%), heavy area 7.22 (24.1%), Wind Waker 6.24 (20.8%), Mario Party 4 9.28 (15.5%); the title: cutscene 40.9%,
  later stretch 23.0%, movie 14.7% (the cutscene ends at ~150 s: windows must follow it). The previous build
  (0f3c3eac6e709b6a) read 10.40 / 7.17 / 6.18 / 9.05 and **must not be differenced against these**: the device drifts
  ~2% between days and a rebuild moves a baseline ~0.8%, so compare runs of one session on one build only. The
  scripted Ordon *walk* also cannot be compared across a change that shifts the guest's timeline (C41).
  **Core 0 is the limit in every scene:** the joint-state line ('# joint 10s') shows it
  running guest code 93-97% of the time and almost never waiting; the GX thread is idle 12-13% in gameplay and 40-75%
  in the title, the vertex worker idle ~70-96%, the GPU ~5-15 ms a frame. In the title's later stretch the ShdwDraw
  walk is mostly replayed (C32) and each triangle's path runs natively in a fast form (C32b): the stand-in is replay
  ~7% + path ~10-11% of core 0 there, plus the native callback; the cutscene's moving shadows cost under 0.2%.
- **Native library ports** (`core/src/cpu/port_j2d.c`, `docs/research/LIBRARY_PORTS.md`): J2DMaterial::setGX,
  J2DPictureEx::drawSelf, J3DShapeMtxConcatView's matrix load, the J3D lights (`diffLightObj`, heavy area +2.7%), the
  J2DPane::draw walk, all of J3DMaterial::diff (heavy area and Ordon +3.0%), J3DSys::reinitGX (Ordon +3.9%) and the
  minimap's renderingDecoration (Ordon +3.1%, heavy area -0.4%), on by default (`--port-hooks 243` since 2026-10-30, when the PNGP matrix load and the minimap decoration came off: together +0.71% in static Ordon, +0.55% in the heavy area); bits 2, 3, 8
  (`__GXSendFlushPrim`), 9 (the skinned-shape matrix load) and 10 (HUD text) are built, exact and off; **9 and 10
  measure +0.9-1.0% and +0.8-0.9% each, +1.3% together, on a gameplay scene that takes no input** (jobs 1491-1497;
  their old Ordon losses were the walk diverging, C41). Each is checked
  exact on the 2DS itself with `--port check` (every call also run by the interpreter and compared; `--port plant`
  proves the checker reports a difference). A port pays only when it does less than the guest code, not when it copies
  the SDK step by step; it must also copy what the guest code leaves that the ABI does not promise (PSMTXConcat's ps1
  lanes of f14, f15, f31) and not run where the guest would take an exception (MSR.FP off).
- **Defaults that measured as gains:** draw merging (+24%), check hoisting (core 0 +5%), the job-ring and TEV work in
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
  shifts (`rotmask`, ~+0.3-0.6%), the ShdwDraw walk memo (`shdwmemo`, later stretch +14.2%, C32), the
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
  FIFO mirror in guest RAM costs Ordon up to 1.6% (C40, parked: no exact way yet).
- **Added 22-24 October (details in HANDOFF):** regions of executed code with calls inlined (`--tier`, on: Wind Waker
  +7.2%, TP heavy -0.9%), textures re-checked by page stamps (`--texfast 2`, on: Mario Party 4 +4.2%, TP +0.6-0.9%),
  EFB copies hashed only when reused (`--copystamp`, on: TP +0.7-0.9%), the J2D pane walk (on: Ordon +2.2%). Built, off:
  pre-decoded display lists (`--g2 on`, exact, no speed change: TP and Wind Waker are core-0-bound), `--nskeep`
  (Mario Party 4 +1.9%, owner's call), fault mode (`--faultmem`, +2-5% but crashes the kernel in svc 0x94), and the
  region tier's nursery (`--tier-nursery`: even in the heavy area, Ordon -1.9%, rejected with its relocation kept as a
  tool).
- **Built, off, measured without a clear gain:** display-list vertex cache, known-single stores (on; +1%), the
  __GXSendFlushPrim port (neutral, separate runs; the J3D light port turned positive with its one-copy commit),
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
- **Open directions, in the order the budget supports them:** removing guest code wholesale - **larger ports where a
  whole subtree is replaced** (other TEV block types, J3D packet draw; `docs/research/LIBRARY_PORTS.md`), since the
  ports as a *set* are +10.6% while their parts sum to ~3.7% and small functions lose as entry-hook stand-ins (do
  the denormal give-up first, CONTRIBUTOR_TASKS 9); the ports' `port_commit` (2.61% of core 0, and the J3D light
  port's one-copy commit turned it from negative to +2.7%, so the technique is proven and unapplied elsewhere); C40
  (~1.6% of Ordon in every game); the ShdwDraw stand-in's code footprint (C32b); GPU-resident vertex data for
  repeated display lists (C1).
- **Guest frame skip, paced to a game speed (C9/C9a; the owner's direction, 2026-10-30: "if we can make it look
  natural").** Not a few percent, and now measured (jobs 1616-1621, three ratios a scene): rendering is **0.737 of a
  game frame in static Ordon and 0.618 in the heavy area**, so Ordon reaches **full game speed at one picture in 8.4
  (showing 3.6 pictures a second)** while the heavy area cannot reach it at any ratio (asymptote **63%**, logic being
  38% of its frame). The useful band is N=2-4: Ordon 56 / 69 / 79% of full speed at 8.3 / 6.9 / 5.9 pictures a
  second, the heavy area 35 / 41 / 45% at 5.2 / 4.1 / 3.4. It **multiplies with base speed** instead of competing
  with it - at full game speed Ordon shows 3.6 pictures a second today and ~18 at twice the base rate - so
  *pictures a second while holding full game speed* is the metric to carry, and no scene has ever been scored on it. `--gfskip-target PCT` holds one game speed everywhere and lets the picture rate
  take the variation, because the speed *changing* between scenes (Ordon 35%, heavy 24%, cutscene 41%) is what a
  player feels; built, off by default, self-tested on a synthetic clock, **not yet measured on hardware or seen by
  anyone**.

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
