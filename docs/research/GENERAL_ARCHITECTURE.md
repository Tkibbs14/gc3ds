# The architecture that would get the most out of the New 2DS XL (2026-10-20)

The owner's question: if any part of gc3ds could be redesigned, keeping ordinary GameCube binaries as the input and
no per-game decompilation or manual porting, what architecture extracts the most performance from the New 2DS XL?
Twilight Princess (TP) is the benchmark, not the target. This review builds on DIRECTION_REVIEW.md and
EMULATOR_CEILING.md (read them for the cost model) and on the hammock analysis (MEASUREMENTS.md "Hammock blocks and
inlined leaves"), adds one new offline model, and re-opens conclusions whose materially different version was never
tested. **Measured** numbers name their job, **derived** ones are arithmetic on measured numbers, **estimates** are
marked; nothing here is built.

## 1. The system as one budget

At 30 game frames a second a frame is 33 ms. Today (DIRECTION_REVIEW.md section 1, measured):

| | Core 0 (guest CPU) | Core 2 (GX thread) | Core 3 (vertex worker) | Core 1 | GPU |
|---|---|---|---|---|---|
| Ordon walk, 9.53 fps | ~100 ms a frame (3.2x over) | ~78 ms (2.4x over) | ~32% busy | sampler / relay only | ~8 ms |
| Heavy area, 6.84 fps | ~142 ms (4.4x over) | ~121 ms (3.7x over) | ~29% | same | ~14 ms |

Four couplings make the parts one problem, not four:

- **Two walls of nearly equal height.** Core-0 gains show only up to ~+15-19% before the GX thread limits (derived from
  the idle shares). Any plan must cut both, together.
- **The FIFO round trip.** Core 0 spends ~23% of its time in the GX SDK (job 370) encoding commands into bytes, and
  J3D's display-list writers write more bytes; the GX thread then spends ~19-22% decoding them and ~21% rebuilding state
  (job 512). The same information is encoded on one core and decoded on another every frame.
- **The shared L2.** Core 0's hot code (1.86 MB of JIT lines) and data live in a 2 MB L2 that cores 1-3 also fill: core
  2 streaming raises core 0's L2-resident loads from 28 to 116-124 cycles (jobs 176-196); a second vertex worker cost
  core 0 -7.4%, job stealing -3.3%. This is why the idle cores (core 3 ~70% idle, core 1 unused) cannot simply take work.
- **The GPU wall behind the GX wall.** When native mode moved geometry to GPU-resident buffers, the GX thread fell only
  ~30% and the GPU became the limit at 9-12 fps, on lighting (~72 ns per light per vertex, job 263). Moving vertex work
  to the GPU moves the bill unless lighting work shrinks too.

## 2. New evidence: function-scope regions (offline, 2026-10-20)

`hammock.py`'s profile data (PC interpreter, TP title stretches, stand-ins on as on the device; 386 M and 236 M guest
instructions) re-run with a broader rule: every direct non-linking `b` / `bc` within +-4 KB stays inside the compiled
region, both directions (loops included), and calls to leaves under 16 instructions are inlined (the
rows `core/tools/hammock.py` now prints).

| Rule | Guest instructions per boundary (cut / later) | Boundaries removed |
|---|---|---|
| Today (every branch ends a block) | 5.73 / 5.50 | - |
| Hammock B, W = 32 (forward only) | 10.10 / 8.87 | 43 / 38% |
| Regions, +-256 bytes | 11.91 / 11.85 | 52 / 54% |
| **Regions, +-4 KB** | **13.58 / 13.47** | **58 / 59%** |
| **Regions + leaves under 16 inlined** | **19.11 / 20.44** | **70 / 73%** |

Code concentration from the same profiles: **90% of executed guest instructions sit in 17 KB (cut) / 61 KB (later) of
guest code, 99% in 39 / 228 KB** (PC title stretches; gameplay will be broader). A heavyweight second-tier compiler
needs to cover only ~4-16k guest words to reach 90%: compile time is not a constraint, and nothing requires moving
compilation to another core.

What this changes: DIRECTION_REVIEW.md's "region compilation stays rejected: no new evidence" rested on a model that
counted saved words at linked exits only (0.6-1.3%). The device prices a boundary at ~20 cycles with four live
registers (job 283), and regions remove 58-73% of them. Priced (derived): 0.178 boundaries per guest instruction x
58-73% x ~14-18 cycles saved each = **~1.4-2.3 of ~22 cycles: +7-12% core 0 from boundaries alone**, before register
allocation over the region and denser code. That is larger than hammocks (+3-8%) and is why a region tier is the JIT
form worth testing, not hammocks on their own.

## 3. Conclusions to re-open: the tested version is not the one that matters

| Earlier conclusion | What was actually tested | The untested version | Experiment |
|---|---|---|---|
| Small stand-ins lose (C20, C21, cXyz: -0.7 to -2.4%; "measured four times") | every one called C through a path that **writes back every cached guest register and forgets them after** (emit_ps_call: rc_flush) | a **zero-flush native call**: arguments passed from the register cache, callee-saved guest registers (r14-r31) left in host registers, volatile ones dropped (dead volatile state is owner-approved) | X3 |
| Region compilation rejected | a words-only model of registers across blocks | regions priced at the measured boundary cost (section 2), with liveness-based allocation | X4 |
| Do not run memory-streaming work against core 0 | L2 shared by replacement, no partitioning | **L2 lockdown by master** on the L2C-310 (ways reserved per core), whose registers are user-reachable through Luma's mirror (job 443; the prefetch register was written in jobs 444-445) | X2 |
| Core 1: light work only | same coupling | same | X2 |
| GPU-resident geometry hits a GPU wall at 9-12 fps | native mode, lighting in the vertex shader for every lit vertex every frame | lighting results cached across frames where their inputs repeat, lights bounded per draw | X1 (d) |
| Memoization fails: inputs change every frame | function-level memos (HUD, J3D) | repetition at the GX level: display lists, state, lighting | X1 |
| Native mode is the only CPU path with room for 30 fps | native mode (4-5x on game CPU) against today's JIT | the emulator with elimination and a region tier combined (estimate ~2-2.5x core 0, section 5): still below native, never measured | X3 + X4 |

Conclusions that stand (the mechanism of the failure is unchanged): traces with tail duplication, AOT through C
(footprint, 17% more ARM instructions), a second vertex worker without L2 partitioning, idle skipping and underclock
(TP's frame-locked work does not shrink), function memoization, and frame or render skipping before a steady 30 (owner).

## 4. The candidates, by kind

### 4.1 Optimise existing work (the same work, cheaper)

- **Fault mode (fastmem), root-caused** (H3): measured **+9.3% / +4.1% core 0** (job 168); blocked by a kernel crash.
  General. Crash-risk runs only when the owner can restart the 2DS.
- **Single-precision FPR representation** (H4): 0-8% (estimate), gated by a PMU split of the float stubs in gameplay.
- **Leaner exits and entries** (H2): +3-5% (estimate); mostly subsumed by a region tier.
- **The GX thread's own code**: decode and state paths optimised in place; each item ~1-3% of the GX thread.

These are worth ~1.15-1.3x on each wall together (estimate). None changes the ceiling.

### 4.2 Eliminate redundant work (work the hardware never needed to redo)

- **Block-boundary work** (region tier, section 2): exits, links, register write-back / reload and CR builds at
  boundaries that a function-scope region does not have. Core 0 +7-12% from boundaries (derived), more with
  allocation.
- **The SDK's bookkeeping, through automatic library replacement:** recognised SDK and JSystem functions (by masked code
  hash, per library version, never per title) run natively. The measured lesson (LIBRARY_PORTS.md): a native version
  pays when it does less, not the same steps. Today only large subtrees pay; small functions would need X3's
  zero-flush call. The **SDK** (GX, MTX / PSVEC, OS, DVD, AX / audio) is in every game; **JSystem** only in Nintendo EAD
  games (GAMES.md: five). Generality therefore ranks the GX SDK first, JSystem subtrees (the HUD pane walk, J3D drawing)
  second.
- **GX command repetition** (G2): 99.6% of TP's draws are inside display lists and 79-91% repeat their bytes and array
  bases (job 107); a translated-fragment cache keyed on (list, incoming state) skips decode, state and most draw-call
  work on hits. The repeat share with state in the key is unmeasured (X1 b).
- **Redundant GX state writes:** J3D re-sends whole TEV / material state per draw; writes that leave a register
  unchanged could be dropped at decode against a shadow, before any state work. Unmeasured (X1 a); cheap to build.
- **Vertex gathering** (G1): GPU-resident buffers for repeated lists; removes the CPU vertex cap (~300k/s against
  ~750k needed). Only pays together with the lighting point below.
- **Lighting results across frames:** for diffuse and attenuated lights, N.L and distance are invariant under a rigid
  transform applied to both the vertex and the light, so a static model under static lights keeps its lit colours
  while the camera moves (only specular depends on the view). Cached lit colours per (list, model-to-light transform,
  light parameters) remove the work that made the GPU the wall in native mode. Unmeasured (X1 d); general (GX
  lighting is the hardware's, not the game's).

### 4.3 Architectural changes that move the ceiling

1. **A two-tier CPU: today's block JIT for cold code, a region compiler for the hot ~5-20k guest words.** Function-
   scope regions (loops included), leaves inlined, guest registers allocated across the region by liveness, compares
   left in ARM flags where the field is dead, fastmem (H3) once safe. The block JIT stays as the profiler, the fallback
   and the exactness reference's partner. Ceiling: the realistic JIT range in EMULATOR_CEILING.md (~10-13 cycles per
   guest instruction, ~1.7-2.2x) becomes reachable for the code the tier covers. **Unproven**: X4 measures it on the
   device before anything is built.
2. **The GX SDK replaced as a unit, with a structured command channel between cores.** Recognise the whole GX library
   of the disc's SDK version; its functions run natively on core 0 against a native GX state that replaces __GXData,
   and emit decoded state and draw records to the GX thread instead of FIFO bytes. Display lists still flow as bytes
   (and hit the G2 cache). This removes the FIFO round trip on both walls at once: most of core 0's ~23% GX SDK share
   (estimate: to ~6-8%) and the GX thread's decode of immediate commands. **Precondition**: no code outside the GX
   library reads __GXData or the FIFO write pointer in ways the native state cannot answer (X1 e). Risk: games that
   write the FIFO directly (the channel must accept bytes too), SDK revisions (one native library per revision:
   GAMES.md lists three generations).
3. **The GX side as a translated, retained pipeline: G1 + G2 + state filtering + cached lighting, split across cores 2
   and 3.** Per frame: matrices, changed state and draw records; cached fragments for repeating lists; geometry in
   linear memory; lighting cached or bounded. Estimate: GX-thread work -50-70% on display-list-heavy scenes, and the
   split takes the critical path to ~55-65% of what remains (G3).
4. **L2 partitioned by core.** If the L2C-310's lockdown by master works on this SoC, core 0 keeps a reserved share of
   ways for code and hot data while cores 1-3 stream through the rest. This is the change that turns cores 1 and 3
   from "light work only" into usable capacity for the GX pipeline (decode on one core, translation / submission on
   another) and audio. It is the cheapest ceiling-moving experiment here (X2) and the only one that is purely a
   hardware question.
5. **Audio on the 3DS DSP, not the ARM cores.** ndsp mixes and resamples in the DSP and decodes Nintendo DSP-ADPCM, the
   GameCube's own sample format: GameCube voices mapped onto ndsp channels (24, so priority stealing above that) keep
   real audio off core 1. Audio produces no samples yet, so this is about not adding a third wall; TP's Zelda
   microcode is not AX, so the mapping is per microcode family (AUDIO.md, ARCHITECTURE.md's plan decodes on core 1).

Set aside, with reasons: a persistent code cache across runs (compile is 0.2-0.7% of core 0); the ARM9 (134 MHz,
owned by Process9); the GPU for CPU work (no compute); PICA fragment lighting as a substitute for vertex lighting
(fragments outnumber TP's ~25k vertices a field several times over); a threaded interpreter tier (EMULATOR_CEILING.md X).

## 5. What the combination could reach (estimate, every term unproven unless marked)

- **Core 0:** library replacement covering ~50-60% of core 0 at ~4x (measured s for subtree ports; small functions
  need X3), the region tier at ~1.3-1.6x on the rest (X4 decides; boundaries alone give 1.07-1.12x), fastmem +4-9%
  (measured): time = 0.45 / 1.5 + 0.55 / 4 ~ 0.44 -> **~2.0-2.5x**. Ordon needs 3.2x, the cutscene 2.7x, the heavy area 4.4x.
- **GX thread:** G1 + G2 + filtering + split: **~2-3.3x**. Ordon needs 2.4x, the heavy area 3.7x.
- **GPU:** stays under budget only if lighting is cached or bounded (X1 d decides).
- **Reading:** Ordon-class TP gameplay at **~20-25 game frames a second** by emulation alone, the title cutscene ~22-28,
  the heavy area ~13-17. Full speed in TP needs every term at the top of its range. Lighter games (fewer guest
  instructions and vertices a frame) may reach full speed on this architecture; none has been measured (X6), which is
  the largest hole in every estimate here.

## 6. Ranked experiments: each decides a hypothesis before anything large is built

Ordered by information per cost, generality first. Each names what confirms and what disproves.

**X1. GX redundancy census (PC, one instrumented build, ~1-2 days, no device risk).** Per field over TP's title and
Ordon states, counted at the decoder:

- (a) BP / CP / XF writes that leave the register unchanged. **Confirms** state filtering if over ~40% of state writes;
  under ~15% drops it.
- (b) Display-list calls whose key (list bytes hash, array bases, and the incoming BP / CP / XF words the list reads)
  repeats from the previous field. **Confirms** G2 above ~70%; under ~40% means the key must be relocatable or G2 dies.
- (c) FIFO bytes by source: GX SDK immediate calls, called display lists, direct writes. Bounds item 4.3.2's GX-thread
  half.
- (d) Lit vertices whose (list, model-to-light transform, light parameters) key repeats. **Confirms** the lighting
  cache above ~50% of lit vertices; under ~20% means lighting must be bounded instead (an approximation).
- (e) Reads of __GXData and the FIFO pointer from code outside the recognised GX functions (a PC watch over the range).
  **Zero or a short, recognisable list** keeps 4.3.2 open; scattered readers in game code close it.

- **Result (2026-10-20, PC title scenes only; [X1 census](MEASUREMENTS.md#x1-gx-redundancy-census-pc-interpreter-title-scenes-2026-10-20)):**
  **(a) Confirmed:** unchanged words 58.09-92.45% (derived from measured per-run counts), above 40%; build item 2
  stays supported, with side-effecting commands protected. **(b) Unclear for G2; whole-shadow variant disproved:**
  strict previous-VI lower 0%, active-field lower 0-2.47% (<40%); active upper 82.26-93.32% in geometry scenes and
  last-use 79.06-86.97% reproduce the byte-level opportunity, not incoming-state compatibility. Item 5 needs a
  dependency-aware/relocatable key and retention across empty VI fields before G2 is built. **(c) Confirmed mixed
  sources:** SDK immediate bytes 13.20-42.09% of all source bytes in geometry scenes; a native channel must retain
  direct-byte and DL paths. Byte volume does not price the time saved; item 6 still needs X3/timing evidence.
  **(d) Unclear overall:** strict previous-VI lighting hits 0% (<20%), but previous-active-field hits are 16.47%,
  83.47%, 66.68% at states 900, 5000, 6590 (4290 has no lit vertices). This disproves the one-VI lookup window,
  not retained lighting reuse: item 5 keeps cached/bounded lighting gated on gameplay and exact validity/timing.
  **(e) Confirmed in these title scenes:** zero outside SDK struct/pointer loads/stores; 4.3.2 stays open, item 6
  still needs X3 and gameplay. Job 107 used last-use repeats; the immediately preceding VI is often empty. Four
  200-field off/on/plant triples have identical state hashes and instruction counts; all five synthetic plants
  move their counters. No optimisation or X2-X6 was built or run.

- **Result (2026-10-20, X1b prerequisite):** the primary CPU test runner now links explicit inert optional
  AOT/HLE/memo hooks directly. Run X1b-cpu passes 420 directed and 12,800 differential cases; both planted
  CPU bugs are caught (313 / 181 mismatches). Census follow-ups are recorded separately below when run.

**X2. L2 lockdown by master (device, probe only, ~1 day; when the owner can restart the 2DS).** Read the L2C-310 TRM
(ARM DDI 0246H, r3p3) lockdown-by-master section first; read the lockdown registers (0x900-0x93C) through the mirror,
set cores 1-3 to allocate only into half the ways, re-run the cross-core chase probe (`# timing xcore`) and restore.
**Confirms** if core 0's 1 MB chase under core-2 streaming falls from ~120 toward ~30-40 cycles; then re-score vw2 and
stealing (the -7.4% / -3.3% losses must vanish). **Disproves** if the registers read back zero or do not change the
chase: the coupling is the bus, not eviction, and the idle cores stay limited to light work (4.3.4 closed; G3 shrinks).

**X3. Zero-flush native calls (build + device A/B, ~3-5 days).** One mechanism, two uses: the queued GPR save / restore
inlining (HANDOFF.md) and a call path that keeps the register cache. Re-score the rejected small stand-ins with it: C20
(GPR helpers), C21 (GX setters), the cXyz operators, one separate whole run each. **Confirms** if they turn positive
(each was -0.7 to -2.4%): automatic replacement then reaches small SDK functions, the precondition for 4.3.2.
**Disproves** if they stay at or below zero: library replacement stays limited to large subtrees, and the SDK-level
generality of 4.2 is mostly lost.

**X4. Hand-compiled region oracle (device bench, ~2-3 days).** Take the 2-3 hottest TP functions in Ordon (`--profile
on`), write by hand the ARM code a region compiler would emit (regions, liveness allocation, CR in flags, no range
checks), and run both versions against captured inputs in a jitbench ring. **Confirms** the region tier at >= ~1.5x on
those functions (worth building; expected core 0 +15-30% where it covers). **Disproves** below ~1.2x: the JIT is near
its floor and core-0 gains must come from elimination only. This replaces the separate H1 and H2 device benches.

**X5. GX ceiling instrument (device, one run).** GX-thread and worker ticks on draws whose X1 key repeats: turns X1's
shares into time. Confirms 4.3.3 if repeating work holds over ~50% of the GX thread in gameplay.

**X6. A second game to gameplay (device, days to weeks).** An early-SDK game, then Wind Waker: speed, the joint states,
the guest family profile, which recognised functions match. **Decides generality**: whether the two walls, the GX SDK
share and the display-list repeat rate hold outside TP. It is ranked sixth only because X1-X4 are cheaper; nothing in
section 5 is trustworthy for "as many games as possible" without it.

Then builds, each behind a switch, off until measured, scored with separate whole runs, gated by the experiment named:

1. GPR save / restore inlining and the zero-flush call (X3; already the first queued item).
2. Redundant state filtering at decode (X1 a).
3. Fault mode's crash root cause (measured +4-9%; independent of the rest).
4. The region tier, smallest first: function-scope regions without loops, then loops, then liveness allocation, then
   CR in flags (X4).
5. G2 + G1 with cached or bounded lighting (X1 b, d; X5).
6. The GX SDK as a native unit with the command channel (X1 c, e; X3).
7. The GX pipeline split across cores (X2).
8. JSystem subtree ports (the HUD pane walk, J3D drawing): still worth doing for TP and the other EAD games, and the
   test of the subtree Amdahl prediction, but under the general-purpose goal they rank below SDK-level work.

## 7. How this changes the queue in HANDOFF.md

The current queue (GPR inlining, then the HUD pane walk, then GX measurements) was set for TP. Under the
general-emulator goal this review proposes: **X1 (PC census) and X2 (L2 probe) first** (cheap, decisive, nothing to
build), **then X3 with the GPR inlining as its first half, then X4**, with the pane walk moving behind the SDK-level
items. The owner decides; HANDOFF.md's queue is unchanged until then.
