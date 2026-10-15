# Redesign candidates: ideas parked with their evidence, re-evaluated later

Owner direction (2026-10-13): anything with redesign potential is logged here and re-evaluated later, never dropped.
Each entry says what it is, the evidence for and against, what it would cost, and what would make it worth another
look ("re-evaluate when"). Review this list whenever a bottleneck moves (a new cap, a new measurement); update the
status line instead of deleting entries. Ranked levers that are already being built live in PERF_LEVERS.md.

Status values: **parked** (not started, waiting for its trigger), **next** (chosen for upcoming work), **built**
(done; the entry stays with its result), **rejected** (measured and refuted; the entry keeps why).

## C1. GPU-resident vertex data for repeated display lists

- **Status:** parked (2026-10-13).
- **What:** keep the gathered vertices of a display-list draw in GPU-readable linear memory, so a repeated draw is
  issued straight from that buffer with no per-vertex CPU work (no gather, no copy into the arena).
- **For:** TP replays identical vertex data: the display-list cache hit 84-85% of lookups on the 2DS with no entry ever
  invalidated by a page stamp (jobs 113-119), and the PC checker found 0 differences over 12.36M hits. TP's heavy draws
  are already GPU-transformed (palette matrices) and shader-lit.
- **Against / cost:** each vertex carries per-draw data today: its palette slot (`slot3[pn]`, chosen per draw),
  colours from lighting, and texture coordinates after texgen. A persistent buffer needs per-draw palette slots to be
  stable or moved into an index the shader resolves, and texgen in the shader. Memory: ~25k vertices a field.
- **Why parked:** the CPU-side cache (DL_CACHE_PLAN.md) measured a hit at ~1.4 us a vertex, the same as a plain gather
  (~1.35 us), because both wait for RAM: the 6 MB record ring never stays in the 2 MB L2 (job 119). Only skipping the
  CPU entirely can cash in on the repetition.
- **Re-evaluate when:** the vertex worker stops being the cap (e.g. the second worker, C2, lifts it) and the GPU's
  time per frame is known with headroom; or a design for stable palette slots appears.

## C2. A second vertex worker on core 1

- **Status:** rejected 2026-10-13 (jobs 125, 127); built (722caa0, `--vw2 on`, `--ab vw2`), off by default.
- **What:** core 1 (the app gets 80%, ~0.75 of core 0's speed) takes indexed and fixed-place jobs from the same queue;
  jobs finish out of order and g_vwTail moves over finished jobs in order.
- **For:** the worker on core 3 is the GX side's busiest part (~60% busy, the GX thread waiting for ring space ~23%).
- **Against:** core 1 also hosts the log writer and samplers; L2 and RAM bandwidth are shared, and the gather is
  memory-latency bound, so two workers may contend.
- **Job 125 (2DS, `--ab vw2`, 400 s):** the second worker took ~820 jobs/s (a third) in the cutscene and did its job on
  the GX side (core 0's wait for it 10.0% -> 5.7%, the GX thread's wait for ring space 21% -> 12.5%), but core 0 itself
  ran slower with it on: cutscene speed 20.28% -> 19.83% (core 0 22.77% -> 21.16%), the core-0-bound section 11.70% ->
  11.36% (+-0.05: real). Net zero or worse. Suspect: the helper spin-polled the queue counters the GX thread writes
  (cache lines moving between cores on every read); job 126 sleeps instead. If core 0 still slows, it is memory
  contention, and the helper would only pay when the GX side is the cap (switch it on by gxwait).
- **Job 127 (helper sleeps when idle; queue counters padded, C7):** the same: cutscene 20.29% -> 19.87%, core 0's
  own speed 22.66% -> 21.24% (-6%), the core-0-bound section 11.75% -> 11.19%. **Rejected (2026-10-13):** not the
  polling; the helper's gathers through guest RAM evict core 0's working set from the shared 2 MB L2. The same tax is
  likely paid already for the core-3 worker's gathers: less vertex work helps core 0 too (raises C9, C6).
- **Re-evaluate when:** the gather no longer streams through guest RAM (C1, C6), or core 0 stops being L2-sensitive.

## C3. Display-list vertex cache on the CPU

- **Status:** built, off by default (`--dlcache on`). Result: no gain (jobs 111-119); kept as the evidence for C1.
- **Re-evaluate when:** records could live in fast memory (they cannot: AXI WRAM is kernel-only, PERF_LEVERS), or if a
  much smaller record (positions only, the stream's own format) fits the L2 with the rest of the working set.

## C4. Shrink VIn (the worker's per-vertex record)

- **Status:** parked (2026-10-13).
- **What:** VIn is 108 bytes (8 texture-coordinate slots, 64 bytes, though TP uses one or two). Pack only the used
  coordinates, and keep the per-draw fields out of it (already done for the matrix indices: VIx, 9ecfeeb).
- **For:** splitting the matrix indices out of VIn alone moved cache-off speed from 20.0-20.8% (jobs 113, 115) to
  21.3-21.4% (116, 118): the worker's memory traffic per vertex matters.
- **Cost:** texgen and the finish phases index `t[unit]`; a packed layout needs a per-job map.
- **Re-evaluate when:** after C2's result (the same bottleneck); cheap enough to try next if C2 does not lift the cap.

## C5. Shared-index fast path (no CPU gather)

- **Status:** rejected for TP (2026-10-13, job 127): only 0.6-0.9% of TP's indexed vertices give all their indexed
  attributes one index (4.5-8.9% of draws do on every vertex, small ones); TP indexes positions, normals and texture
  coordinates separately. Re-evaluate only for another game whose counter (`# shared index 10s`) says otherwise.
- **What:** when every indexed attribute of a draw's vertices uses the same index (and the formats are ones the PICA
  reads), upload each array once (byte-swapped) as a linear attribute buffer and draw with the PICA's one index buffer:
  no per-vertex CPU work. The PICA has one index buffer per draw, not one per attribute (3dbrew GPU/Internal_Registers,
  via a Haiku research pass), so draws with different indices per attribute cannot use it.
- **Unknown:** how many of TP's draws qualify. **Measure first:** a `# vfast` counter of draws whose indexed attributes
  carry equal indices on every vertex.
- **Risk:** the byte-swap pass, the 12 attribute buffers, formats the PICA cannot read (8-bit normals, fractional
  fixed point), and arrays that change (page stamps already exist for that, DL_CACHE_PLAN).
- **Re-evaluate when:** the counter says a large share of vertices qualifies.

## C6. Texgen and normal transforms in the PICA vertex shader

- **Status:** parked (2026-10-13).
- **What:** the CPU writes raw attributes; the shader applies texture matrices (texgen) and normal transforms, as
  Dolphin's VertexShaderGen does (matrix index as an attribute, matrices in uniform arrays). Texture coordinates take
  11% of the worker and positions/normals 17% (job 115).
- **For:** shader length measured free on the 2DS (+128 MADs: no change; GPU_GUIDANCE rule 3).
- **Risk:** 96 vec4 uniforms shared with the palette; `a0` relative addressing for per-vertex matrix choice is
  unverified on hardware; variants per texgen setup; a 4th attribute costs ~45% of the vertex rate (rule 2).
- **Re-evaluate when:** the worker is still the cap after C7 and C2's follow-up.

## C7. Queue counters on their own cache lines

- **Status:** next (cheap).
- **What:** `g_vwHead` (GX thread writes) and `g_vwTail` (worker writes) are declared together (pica.c:140), so they
  share a 32-byte line that moves between cores 2 and 3 on every job, even with one worker; with C2, `g_vwClaim` too.
  ARM11 MPCore: 32-byte lines, MESI, the SCU moves modified lines core to core (ARM DDI 0360F §1.3.4, §7.1.1, read by a
  Haiku pass; the TRM text is in the session scratchpad).
- **Also from that pass:** the 2 MB L2 is shared by all cores, so a second worker streaming through guest RAM evicts
  core 0's working set: the likeliest reason job 125's core 0 slowed even with few helper jobs, if job 126 (helper
  sleeps instead of spinning) does not recover it.

## C8. Single-precision floats in the JIT (PERF_LEVERS lever 2)

- **Status:** **next, top priority** (2026-10-13): job 128 showed core 0 is the wall once the GX side is relieved.
  Research done 2026-10-13 (Haiku pass on Dolphin's sources).
- **Plan from the research:** port Dolphin's per-op analysis (`fprIsSingle`, `fprIsDuplicated`, `fprIsStoreSafe`,
  PPCAnalyst.cpp); fadds/fsubs/fdivs on known-single inputs are exact in single (double rounding is innocuous when
  q >= 2p+2: Roux, JFR); fmuls only when frC is known single (Gekko cuts frC to 25 bits otherwise: Dolphin's
  Force25BitPrecision, truncate vs round still open); lfs/stfs and frsp elision with per-register format tracking;
  paired singles as two S ops; float-type psq without conversion. fmadds last: VFPv2 has no fused multiply-add and
  double-then-round differs by 1 ULP in real games (Dolphin bug 13865, Mario Strikers Charged).
- **Before building:** a bit-exact harness (the JIT fuzzer already compares against the interpreter); denormals under
  RunFast flush on conversions (Dolphin bug 6936 is the same hazard on x86 DAZ) - our fuzzer keeps them out today.

## C9. Frame skip on the GX side

- **Status:** parked (2026-10-13).
- **What:** for fields that will not be shown, skip vertex work (gather, worker jobs, GPU draws) while still running
  the game's GX command stream for its side effects (EFB copies the game reads back, tokens, interrupts, draw done).
  TP renders at 30 fps; skipping every other rendered frame halves the GX side's load in a GX-bound scene.
- **For:** the cutscene is GX-bound (~20-21%, core 0 alone ~23-24%); common practice in handheld emulators.
- **Against / risk:** EFB copies used as textures (TP's bloom, shadows, map) must still be produced, or skipped
  frames feed stale textures; frame pacing; it lifts only the GX cap, never core 0's.
- **Built as an experiment 2026-10-13 (`--fskip on`, `--ab fskip`); job 128 (cutscene, 300 s):** speed 19.81% ->
  21.80% (+10%, ~3 sigma); core 0's own speed 22.33% -> 23.29% (within noise); core 0's wait for the GX side
  10.2% -> 5.4%; the worker 54% -> 30% busy. (The first 110 rows read +21%: the early cutscene is heavier on the GX
  side.) **Reading: with half the vertex work gone, core 0 is the wall at ~23%;** removing all vertex work would add
  little more. The JIT (C8 and the optimizing stages) is what 30-40% needs. Visible glitches: skipped frames repeat
  the previous image and render-to-texture effects lag (owner saw "rendering bugs and jitters").
- **Status now:** parked as a user option for later (needs the EFB-copy glitches fixed); not the main lever.

## C10. Idle-loop skipping and an emulated-CPU underclock

- **Status:** parked (2026-10-13).
- **What:** detect the game's busy-wait loops (polling a register or a variable until an interrupt) and jump the
  emulated clock to the next event; optionally run fewer emulated cycles per field (Dolphin's CPU clock override).
- **For:** time the guest spends waiting is pure cost for core 0. Dolphin does both.
- **Unknown:** how much TP waits in the heavy scenes (the JIT profile was flat, D3, which argues for little).
  **Measure first:** count blocks that branch to themselves without changing state, from the JIT's block profile.
- **Re-evaluate when:** the measurement says waiting is a large share of core 0.

## C11. Float lanes: exact singles kept in VFP registers within a block (OPT_FCACHE)

- **Status:** built 2026-10-13 (18d6248, second pass 2026-10-13), exact, **off by default** (`--fcache on`,
  `--ab fcache`).
- **Model** (arm11_sched over TP's cutscene, 20 fields): 6.29 + 2.48 -> 6.17 + 2.30 host instructions + stall cycles
  per guest instruction (-3.4%); lfs 18.7 -> 12.9 cycles, stfs 22.4 -> 15.3, psq_st 39.0 -> 30.1, fadds 22.0 -> 12.7,
  ps_mul 31.4 -> 18.4.
- **2DS, jobs 132-134 (three 600 s runs, same build):** every run agrees. Cutscene core 0 +4.3%, +3.5%, +3.0%
  (22.42 -> 23.38, 22.45 -> 23.23, 22.49 -> 23.16); the later core-0-bound stretch -2.7%, -2.9%, -4.4% (11.56 -> 11.25,
  11.46 -> 11.13, 11.66 -> 11.15). **A real, scene-dependent effect: +3.5% where float work is dense, -3.5% in the more
  varied code later; net zero, so off by default.** Job 131 (first pass): +1% / noise.
- **Lesson:** the model leaves out instruction fetch, and on the 2DS core 0 loses ~9.7 cycles per guest instruction to
  fetch stalls (OPT_JIT_PLAN). The lanes made hot code ~2-3% larger (and exit stubs bigger); TP's blocks are short, so
  values seldom live long in lanes. **Judge JIT changes by hot words per guest instruction as well as by the model.**
- **Re-evaluate when:** blocks get longer (region compilation or traces that pay), or code size per guest instruction
  drops elsewhere so the lanes' extra words matter less.

## C12. Static recompilation: translate the game's PowerPC code to ARM ahead of time

- **Status:** built 2026-10-14 (docs/research/AOT_PLAN.md): exact on the PC (10.7 M calls compared with the interpreter, 0
  differences; last frame byte-identical), 96.7% of TP's executed guest instructions translated. 2DS A/B pending.
  Raised while scoping a Wind Waker port.
- **What:** instead of compiling 1–30-instruction blocks at run time, translate a game's whole DOL and RELs
  into C (or straight to ARM) on the PC once. GCC then optimizes each guest *function* as a unit, with registers
  allocated across the function, no block exits and no dispatch. gc3ds keeps the GameCube memory image, GX, DSP and
  timing; only core 0's code changes. Instructions the translator can't handle fall back to the interpreter.
- **Evidence for:**
  - Wind-Waker-Recomp (GPL-3, PC/Mac/iOS) does exactly this for all of Wind Waker's DOL and its 415 modules, with
    a Dolphin-derived runtime.
  - Core 0 is the wall at ~33 cycles per guest instruction (HANDOFF). ~10 of those are instruction-fetch stalls on
    short JIT blocks and ~13 are executed instructions, which a whole-function compiler is better placed to cut.
  - It also covers code a decompilation hasn't reached yet: Wind Waker is 79.6% decompiled (decomp.dev, 4d78c99).
- **Evidence against / unknowns:**
  - The output must still do big-endian accesses (SETEND as now, or `rev`) and match Gekko's float and paired-single
    semantics, so part of the 33 cycles stays.
  - Code size: a whole game translated may exceed the I-cache worse than the JIT's hot set does. Unmeasured.
  - The output is per game, so this is a per-game build step, not a general emulator.
  - The gain on the ARM11 is unmeasured.
- **Cost:** large.
  - A PowerPC-to-C translator: function discovery, jump tables, indirect calls through a lookup, and REL relocation
    at load time.
  - A build pipeline that reads the user's own disc, so no game code is ever distributed.
- **Cheapest test:** translate one hot TP function set (the cutscene's top blocks from JIT_REGSTAT) by hand or with a
  small translator. Link it into gc3ds behind an `--ab` switch that routes those guest addresses to the native
  versions, and measure core 0 on the 2DS.
- **Re-evaluate when:** now. This is the only candidate that attacks the 33-cycle wall as a whole, and the test runs on
  TP, which is already on the card.

## C13. JIT code size, peephole candidates (CR dead stores, bl / bclr, float loads and stores, register pinning)

- **Status:** measuring (2026-10-14). Candidate 1 (CR-field dead stores) rejected by its model: exact ~0.3% of hot
  words, inexact ~1.5-2%. Candidate 2 (OPT_RETSTUB) built, -2.1% hot words under qemu, within noise on the 2DS (job 148),
  off. HANDOFF 2026-10-14 night has the numbers.
- **Lesson:** executed hot words per guest instruction did not predict I-cache misses for this change: misses follow the
  hot code's footprint (distinct lines touched), not the words executed per instruction.
- **Re-evaluate when:** several small exact reductions can be stacked into one A/B, or a footprint measure (distinct hot
  lines per field) shows which code the misses come from.
