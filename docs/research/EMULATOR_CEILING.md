# How far can genuine emulation go on the New 2DS XL? (2026-10-20)

A second, independent investigation at the owner's request, of the emulator path only: the game's own PowerPC code
keeps running through gc3ds, accuracy may be traded where measured gains justify it, native game ports are out of
scope. It re-derives the cost model from device measurements, examines why each rejected idea failed and whether a
materially different implementation avoids that failure, and ends with experiments that could disprove each proposal
and a roadmap. **Measured** numbers name their job; **derived** ones are arithmetic on measured numbers; **estimates**
are marked.

It revises one premise of DIRECTION_REVIEW.md: core 0 costs **~22 cycles per guest instruction today**, not ~35 (the
35.1 is job 157, a 10-14 build).

## 1. Today's cost model, rebuilt from the device

### Cycles per guest instruction now (derived from perf.csv pmu columns, current defaults)

| Scene (job) | Guest M instr/s | Cycles / guest | ARM instr / guest | I-misses / guest | Fetch stall / guest | Data stall / guest | Full speed needs |
|---|---|---|---|---|---|---|---|
| Ordon walk (702) | 35.4 | **22.7** | 7.69 | 0.234 | 6.1 | 5.2 | ~7.0 cycles: **3.2x** |
| Heavy area (700) | 36.9 | **21.7** | 7.60 | 0.220 | 5.5 | 4.9 | ~4.9 cycles: **4.4x** |
| Title cutscene (697) | 36.7 | **21.9** | 7.68 | 0.219 | 5.6 | 4.9 | ~8.1 cycles: **2.7x** |
| Cutscene, 10-14 build (157) | 22.9 | 35.1 | 11.32 | 0.280 | 8.6 | 8.0 | 4.5x |

(The guest count includes work stand-ins and ports stand in for, so it measures progress through the guest's program,
which is what full speed needs. "Full speed needs" = 0.97 x 804 M cycles / (guest M instr/s / speed).)

Since 10-14, executed ARM instructions per guest instruction fell from 11.3 to 7.7 and cycles from 35 to ~22: the JIT
and stand-in work of the last week was real. The remaining gap is **2.7x (cutscene), 3.2x (Ordon), 4.4x (heavy area)**
on core 0 alone.

### Where the 22.7 cycles go (Ordon, derived)

- **~7.7 cycles: executing ARM instructions** (7.69 per guest instruction, single issue).
- **~6.1: instruction fetch from L2** (0.234 misses x ~26 cycles; the device's ~21-28 cycles per miss, job 283, matches
  an L2 hit: the code streams from L2, it rarely reaches FCRAM).
- **~5.2: data stalls**, of which only **~1.3 are cache misses** (1.67 M D-misses/s x ~28 cycles, mostly L2 hits); the
  rest are load-use interlocks (3-cycle loads, block-entry reloads, VFP results).
- **~3.7: unattributed** (VFP dependent latency and taken-branch bubbles have no PMU event, job 183).

A cross-check from the other direction: the device's per-class bench (job 279), weighted by TP's dynamic mix, gives
**~11 cycles per guest instruction for code in L1 inside one block**; block boundaries cost **~20 cycles each with four
live guest registers** (job 283) at TP's 5.9 guest instructions per block, **~2-3.4 per guest instruction**; fetch ~6;
real data misses ~1.3. Sum ~21-22: the two decompositions agree.

### What the bench says each guest class costs in L1 (measured, job 279)

addi 1.7 (one register) to 5.4 (eight registers rotated: the register cache's pressure), lwz 4.9, stw 7.6, cmpwi 8.2,
rlwinm 7.3, bl+blr 33.7 a call, lfs 22.8, **stfs 39.8, psq_l 31.3, psq_st 76.5, fadds / fmuls 40-41 (twice their double
forms), ps_mul 51.6, ps_madd 59.1**. Executed hot code by opcode (job 512-era qemu count): **branches and compares are
about a third of it** (bc 16.8% at 6.3 words each, b 5.5% at 6.3, cmpli / cmpi 7.3% at 6.4-6.5), loads and stores
~17%, floats a few percent each.

## 2. The lower bound (question 2)

**Hardware floor, idealised (estimate):** with perfect register allocation, compare+branch fused into ARM flags, loads
as mask / ldr / rev (3), stores 3, float loads 2-3 with the value kept single, integer ops 1, branches 1, TP's mix
(loads 22%, stores 15%, branches ~15%, compares ~7%, the rest ALU and float) needs **~2.3-2.6 ARM instructions per
guest instruction**. With the ARM11's 3-cycle load-use and 8-cycle VFP latencies partly hidden, **~4-5 cycles per guest
instruction in L1**. Code at ~10 bytes per guest instruction fetched from L2 at today's reuse adds ~3. **Floor: ~7-8
cycles.** That floor equals full speed in the cutscene (8.1) and Ordon (7.0); the heavy area (4.9) is below it.

**Realistic best for a JIT (estimate):** 32 guest GPRs on ~13 usable host registers (ARMv6: r0-r12, lr; no MOVW / MOVT,
no Thumb-2), block boundaries that cannot all vanish (calls are frequent: bl+blr 33.7 cycles per call in L1), and
interlocks a single-issue core cannot hide put an excellent translator at **~10-13 cycles per guest instruction**:
**~1.7-2.3x better than today**. No measurement contradicts this range; nothing yet demonstrates it either.

**Consequence:** dynamic translation alone cannot reach full speed in Twilight Princess's gameplay (it needs 3.2-4.4x).
It can come within reach when combined with **workload elimination**: if ~30-40% of the guest's instructions run as
native subsystems at ~4x (measured for ports), the remaining code needs ~9.5-11 cycles per instruction in Ordon
(derived): inside the realistic JIT range. The heavy area would still need ~6.5-8.

## 3. Why the rejected ideas failed, and what a different implementation changes

| Idea | Measured result | Mechanism of the failure | Materially different version | Status |
|---|---|---|---|---|
| Traces (C14) | core 0 -5.8%, I-misses +16.5% (job 154) | tail duplication: every side exit and return point copies the code after it | blocks that grow across **forward branches inside a window** (hammocks): each guest instruction compiled once, join points inside the block | **open (H1 below)** |
| Region compilation / registers across blocks | modelled 0.6-1.3% (JIT_REGSTAT, 10-12) | the model counted saved words at linked exits only | the device bench prices a boundary at ~20 cycles with 4 live registers; the model did not count load-use stalls at entries, the exit sequences, or the compare / branch fusion bigger blocks allow | **re-open with a cycle model** |
| AOT C translation (C12) | -31 to -36% (job 144) | 6 MB of translated code (slow paths inline), registers loaded at every entry, calls leaving the function; 17% more ARM instructions | none credible: the footprint is the guest's code (1.86 MB of hot JIT lines already), a C compiler makes it larger | closed |
| CR stubs (OPT_CRSTUB) | neutral (job 155) | shrank the CR build in shared stubs, but the field still reaches guest state at every block exit | CR fields held in ARM flags across a hammock block, materialised only at exits that need them | **open (part of H1)** |
| CR dead stores (C13 #1) | modelled 0.3% exact | within today's 5.9-instruction blocks few compares are dead | same as above: larger blocks change the count | re-open inside H1 |
| Relaxed floats (C8 via OPT_FASTFLOAT) | +0.5% gameplay (job 373), title neutral (338) | removed rounding only; values still stored as doubles, every lfs / stfs / psq still converts and crosses core / VFP | a **single-precision register representation** (Dolphin's fprIsSingle analysis) so lfs / stfs / psq move singles without conversion | **open but second** (H4) |
| SETEND BE float stubs (C16) | neutral, I-misses +6-7% (job 166) | the BE switch added words to shared stubs | only useful together with H4 | folded into H4 |
| Fault mode (fastmem) | **+9.3% core 0 cutscene, +4.1% later** (job 168); kernel crash after 685-6,166 faults (earlier runs) | crash not root-caused | same mechanism, root cause first | **open (H3)** |
| Second worker / stealing (C2, C15) | -7.4% / -3.3% core 0 | L2 eviction of core 0's working set by another core's streams | not a CPU lever; core placement matters for the GX side | closed for core 0 |
| CPU display-list vertex cache (C3) | no gain (jobs 111-119) | its records lived in FCRAM: a hit cost ~1.4 us a vertex, as a gather | GPU-resident buffers (C1): no per-vertex CPU work at all | **open (G1)** |
| Small stand-ins (C20, C21, cXyz) | -0.7 to -2.4% | an entry hook costs a block exit and a dispatch, more than a 10-40 instruction function | the JIT **inlines** recognised leaf functions at the call site (no call boundary) | **open (H5)** |
| Memoizing functions | 0 hits (HUD, J3D) | inputs change every frame | at the GX level instead: the command stream repeats (G2) | moved to G2 |

## 4. Proposals

Each: the bottleneck, the supporting and contradicting measurements, the ceiling, and the smallest experiment that
could disprove it. Gains are on core 0 unless marked GX.

### H1. Hammock blocks: blocks that span forward branches, with CR in ARM flags

- **What:** today every branch ends a block (5.9 guest instructions on average). Let a block continue across a forward
  conditional branch whose target lies within a window ahead and is not entered from elsewhere (an if-then or
  if-then-else): the branch becomes an internal ARM branch, the join point a label. No code is duplicated (the
  failure of traces). Inside such a block, a compare whose only reader is the branch stays in ARM flags; the CR field
  is written back only at exits where it is live. (The downcount is already checked only in blocks that can loop:
  OPT_LATCH, on since 10-13, +2.4%. Traces differ: they turned conditional branches into side exits, each with its own
  exit sequence, and duplicated the code after followed branches; a hammock keeps both paths and the join inside.)
- **Supports:** a boundary costs ~20 cycles with four live registers (job 283); branches and compares are ~a third of
  executed hot code (6.3-6.5 words each); C22's reason compares could not use flags was that "a compare ends its
  block with the branch that reads it": hammocks remove that reason; every branch ends a block today (jit.c
  trace_branch: without traces each b, bc, blr and bctr ends the block), so each pays an exit and a link.
- **Contradicts:** the region model's ~1.3% (but it priced words, not cycles); traces' loss (but from duplication,
  which this avoids). Unknown: how many forward branches qualify in TP's code.
- **Ceiling (estimate):** boundaries ~2-3.4 cycles per guest instruction and compare / branch code ~30% of hot words:
  removing half of both is **~3-5 of 22.7 cycles: +15-25% core 0**; realistic **+8-15%**.
- **I-cache:** smaller (fewer exit sequences, fused compares); no duplication.
- **Difficulty:** high (the JIT's block model, the register cache across internal edges, the fuzzer extended to
  multi-path blocks). **Risk:** exceptions and fallbacks mid-block need the guest state reconstructible at each point.
- **Smallest disproving experiment:** offline, on the PC: run TP's instruction trace through a block former with the
  hammock rule (JIT_REGSTAT already counts blocks and register traffic): if block length rises by less than ~1.5x or
  fewer than ~30% of compare / branch pairs become internal, drop it. Then one device jitbench ring with internal
  forward branches against today's exits, to price the boundary saved.
- **Offline result (2026-10-20, MEASUREMENTS.md "Hammock blocks and inlined leaves, modelled offline"):** the block
  former reproduces today's 5.5-5.7 instructions per boundary; hammocks reach **7.5-8.9 (A, W = 16-32) and 8.3-10.1
  (B: other bc as side exits)**, boundaries -27 to -43%. The 1.5x threshold is **met** (A at W = 32, B at W = 16), 2x
  nowhere. The compare / branch threshold was mis-posed: the compare is already in the branch's block 95-96% of the
  time; what counts is whether the CR field can stay unwritten, and it is dead at every exit only **7-14%** of the time.
  **Revised ceiling (estimate): +3-7% core 0 from boundaries, ~+0.5-1% from CR in flags**, not +8-15%. Kept open
  behind the cheaper H5, which removes a further ~12-15% of boundaries (calls and returns are ~a third of them).

### H2. Leaner block exits and entries

- **What:** without changing block shape: exits that hand the register cache to a linked successor known to want the
  same registers (no write-back / reload pair), entries that load registers lazily on first use, and the budget
  decrement folded into fewer instructions.
- **Supports:** ~4.5 register loads / stores per block run (JIT_REGSTAT); r0 written back 0.47 times per block but
  loaded 0.04 (dead write-backs); the 20-cycle boundary.
- **Contradicts:** the 10-12 model (1.3%); dead-state approval is required (granted 10-15).
- **Ceiling:** ~1-1.5 cycles: **+5-7%**; realistic **+3-5%**. Smallest experiment: the same offline model, counting
  only write-backs whose register is dead in every successor (the r0 case), priced at the bench's per-word cost.

### H3. Fault mode's crash, root-caused (fastmem on the 3DS)

- **What:** memory accesses without range checks, out-of-range faults patched to the slow path. Already built.
- **Supports:** the largest measured JIT lever: **+9.3% / +4.1% core 0** (job 168).
- **Contradicts:** kernel crashes after 685-6,166 faults in earlier runs; n = 1 clean run.
- **Ceiling:** +4-9% (measured). **Smallest experiment:** reproduce the crash with a fault counter and the arm9 dump;
  measure how many faults a minute TP takes in steady state (if few after warm-up, pre-patching the faulting sites at
  compile time on their second fault removes the kernel path almost entirely).

### H4. Single-precision float representation

- **What:** Dolphin's per-op analysis (fprIsSingle / Duplicated / StoreSafe) so known-single FPRs live as singles: lfs,
  stfs and float psq move them without double conversion, paired singles as two single lanes, SETEND BE in those paths.
- **Supports:** stfs 39.8, psq_st 76.5, fadds 40 cycles against double forms at ~half (job 279); float loads / stores
  27.6% and float maths 15.3% of modelled cycles (D2).
- **Contradicts:** fastfloat +0.5% and BEFLOAT neutral (jobs 166, 338, 373): the float share in real gameplay may be
  smaller than the bench suggests, or fetch hides it.
- **Ceiling:** if floats are ~25% of core 0 and become ~2x cheaper, **+10-14%**; realistic **0-8%**. **Smallest
  experiment first:** a PMU split around the float stubs in gameplay (`--pmu-split` mode on the stub range) to measure
  the float share on the device; build only if it exceeds ~15%.

### H5. Inlined recognised leaf functions

- **What:** the JIT compiles a `bl` to a recognised small leaf (the PSVEC / PSMTX library, cXyz operators, GPR save /
  restore, GX setters) as native code at the call site: no call, no entry hook, the caller's register cache kept.
- **Supports:** bl+blr costs 33.7 cycles a call (job 279); stand-ins of the same functions lost only because of the
  call boundary (C20, C21, cXyz: -0.7 to -2.4%); GPR save / restore ~2.4%, cXyz ~2.8%, PS library several % of core 0.
- **Contradicts:** code growth at every call site (traces' lesson): keep to functions under ~20 instructions.
- **Ceiling:** **+4-8%**; realistic +2-5%. Smallest experiment: inline `_savegpr_N` / `_restgpr_N` only (pure stores /
  loads, exact) and A/B with separate runs.
- **Offline count (2026-10-20, same MEASUREMENTS.md section):** calls are ~16-18% of block boundaries, each with a
  return. Leaves reaching blr within 16 instructions (no call, frame, loop or tail jump) take 39-43% of calls: ~12-15%
  of boundaries. `_savegpr_N` / `_restgpr_N` alone take 17-22% of calls (~6-7% of boundaries, ~+1.5-2.5% core 0,
  estimate); OSDisableInterrupts / OSRestoreInterrupts take 9.4% in the later stretch but hold an mtmsr, which ends a
  block today (inlining them pays only if mtmsr's EE-only form is compiled in place).

### W1. Subsystem ports, generalised by library version (workload elimination)

- **What:** the JSystem / SDK subtrees as native code, recognised by code hash, verified call by call on the device:
  the J2D pane walk (designed), J3D packet and material drawing, JAudio's sound engine.
- **Supports:** ~60-65% of core 0 is shared library code (jobs 289, 370); ports reach ~4x on what they cover.
- **Contradicts:** each port so far gave +2-6% in Ordon, below its inclusive share; version-specific.
- **Ceiling:** ~1.8-2.4x core 0 by Amdahl at s = 4-10 (DIRECTION_REVIEW.md); realistic **+30-60%** over several ports.
  Smallest experiment: the pane walk, scored against its +3-3.5% prediction.

### G1. GPU-resident geometry for repeated display lists (GX)

- **What:** C1, using native mode's converted shapes: a repeated display-list draw issued from PICA vertex buffers in
  linear memory, matrices and state per frame only, page-dirty invalidation.
- **Supports:** 99.6% of TP's draws are inside display lists, 79-85% repeat bytes and array bases (job 107); the CPU
  vertex cap ~300k/s against ~750k needed at 30 fps; native mode: the worker 300-700 -> 112 ms/s (job 263).
- **Contradicts:** in native mode the GX thread still worked ~53 ms a frame with converted shapes (job 263, derived)
  against ~75 ms in the emulated cutscene: **geometry alone cut the GX thread only ~30%**; the GPU became the limit at
  9-12 fps there (lighting ~72 ns per light per vertex).
- **Ceiling:** GX thread **-20-35%**, the worker -60%; lifts the gameplay cap for core-0 work from ~+15% to ~+40-60%.
- **Smallest experiment:** ticks on the GX thread and the worker for draws whose list and arrays repeat (the `# dlist`
  counters), one run: the share G1 can touch.

### G2. A translated command cache per display list (GX)

- **What:** go past geometry: the GX thread translates each repeated display list once into a backend fragment (state
  deltas, batch keys, draw calls in citro3d's command format, the converted vertex buffers) and replays the fragment
  when the list's bytes, array bases and the incoming state match, patching only the matrices the CPU loaded through XF
  since the last draw. Decode, state work, batch keys and most draw-call work go away for repeated lists.
- **Supports:** the GX thread's heavy-area time: decode ~19%, state ~21%, vertex ~18%, draw calls ~14% (job 512):
  ~70% of it is per-draw work on lists that repeat 79-91% of the time.
- **Contradicts:** state depends on what came before the list (the key must include the relevant incoming state, or
  the fragment must be relocatable); EFB copies and render-to-texture break replays; not measured anywhere.
- **Ceiling:** GX thread work **-50-70% (2-3.3x)** in display-list-heavy scenes (estimate). Together with G1 and G3 this
  is the only credible route to the 3-4x the GX side needs.
- **Smallest experiment:** instrument the decoder to hash, per called display list, the incoming state words it
  depends on (BP / CP / XF registers it reads): the share of calls whose (list, state) key repeats bounds G2.

### G3. The GX pipeline split across cores 2 and 3 (GX)

- **What:** the decoder and state on core 2, batch keys, draw calls and the citro3d submission on core 3 (idle ~70%),
  or the reverse; the worker's gather shrinks under G1.
- **Supports:** the worker is ~29-32% busy in gameplay; the GX thread waits on it only ~8-10%.
- **Contradicts:** another stream between cores competes for the shared L2 (C2's lesson); citro3d's command buffer is
  single-producer.
- **Ceiling:** the GX critical path to **~55-65%** of today (estimate). Smallest experiment: move one stage (texture
  uploads / copies, ~9-10% of the GX thread) to core 3 and measure both cores and core 0.

### X. Ideas examined and set aside

- **A compact threaded interpreter or call-threaded tier** for the lukewarm code: its denser guest stream would cut
  fetch misses, but each guest instruction pays a dispatch and operand decode; at today's measured density (0.22-0.23
  I-misses per guest instruction) the fetch saved (~3 cycles) is about what the dispatch costs. No net gain (estimate).
- **A helper core warming core 0's I-cache:** an L1 is private; the code already sits in L2.
- **Emulated-CPU underclock, idle skipping:** TP never idles in gameplay; frame-locked work does not shrink.
- **Larger pages, AXI WRAM, L2 prefetch tuning:** ruled out by documentation or measurement (PERF_LEVERS).
- **Running the guest CPU on two cores:** the guest is one thread of control; speculation across cores has no
  credible design for in-order cores sharing an L2.

## 5. Recurring work that can be eliminated without decompilations (question 4)

- **GX command repetition (G2):** the most general. Every game that draws from display lists replays the same bytes
  every frame; caching their translation is game-agnostic.
- **Library subsystems recognised by code hash (W1, H5):** per SDK / JSystem version, not per game (GAMES.md: 40 TP
  stand-ins already match 3-17 functions in each other game, more with masked recognition).
- **Idle and polling loops:** gc3ds's idle hook already skips polling loops; it matters for lighter games that wait.
- **Not generally possible:** skipping guest render work across frames (no published emulator does it without
  artifacts, OUTSIDE_RESEARCH.md), and memoizing game functions (their inputs change every frame, jobs 454-456, 672-680).

## 6. Unavoidable limits and design limits (question 5)

- **Unavoidable:** one in-order, single-issue ARM11 at 804 MHz for the guest CPU; 16 KB L1 I-cache against ~1.9 MB of
  hot guest code (the guest's footprint); ~13 usable registers for 32 guest GPRs; no NEON, Thumb-2 or MOVW; 3-cycle
  load-use and 8-cycle VFP latencies; one outstanding cache miss; the L2 shared with the other cores and the GPU; three
  usable cores (core 1 ~80%, and memory-heavy work there hurts core 0); the PICA200's ~20 M vertices/s with a short
  shader and ~72 ns per light per vertex.
- **Design consequences (changeable):** blocks ending at every branch; CR materialised at every exit; FPRs stored as
  doubles; an exit and link sequence at every branch; per-access range checks (fault mode removes them); CPU vertex gathering; every
  display list decoded and translated every frame; one GX thread; core 1 unused.

## 7. The most ambitious credible emulator-only target (question 7)

- **Core 0 (estimate):** H1 +8-15%, H2 +3-5%, H3 +4-9%, H4 0-8%, H5 +2-5% compound to **~1.2-1.45x**; W1 adds
  **~1.3-1.6x**: together **~1.6-2.3x**. Ordon needs 3.2x, the heavy area 4.4x.
  - **Revised 2026-10-20 after H1's offline model:** H1 +3-8% (boundaries plus CR), H5 +2-4%: the JIT items compound
    to **~1.13-1.39x**, and with W1 **~1.5-2.2x**. The target below moves to its lower half.
- **GX side (estimate):** G1 + G2 + G3 together **~2-3.5x**; Ordon needs ~2.4x, the heavy area ~3.7x.
- **Most ambitious credible target: Ordon-class gameplay at ~15-22 game frames a second, heavy scenes ~10-15**, by
  emulation alone. **Full speed in Twilight Princess's gameplay requires the top of every range at once** (core 0
  ~3.2x), which no measurement supports today and none rules out: it is the stretch case, not the plan. The title's
  cutscene (2.7x) is the scene where full speed by emulation is closest.
- **Lighter games** (lower guest instructions per frame, fewer vertices) could reach full speed on this path; none has
  been measured (DIRECTION_REVIEW.md experiment 2).

Evidence that would move the target up: H1's offline model showing blocks 2x longer and most compare / branch pairs
internal; G2's key-repeat share above ~70% of display-list calls; the pane walk meeting its Amdahl prediction. Evidence
that would move it down: any of those failing. (H1's model, 2026-10-20: blocks 1.5-1.85x longer, not 2x, and the CR
field dead at the exits only 7-14% of the time: the first of these failed.)

## 8. Roadmap, ordered for information per cost

1. **Offline models, no device time (days):** H1's block former and boundary / compare counts on TP's PC trace; H2's
   dead write-back count; G2's (list, incoming state) repeat share from a decoder hash on the PC. These decide whether
   the two big items (H1, G2) are worth building. **H1 done 2026-10-20:** passes the length threshold, fails 2x,
   worth ~+3-8%; it stays behind H5 and the GX items.
2. **Cheap device measurements (one run each):** the share of GX-thread and worker time on repeated lists (G1 / G2);
   a PMU split on the float stubs (H4); fault counts per minute in steady gameplay (H3).
3. **The first builds, smallest first, each with separate-run scoring:** inline save / restore GPR (H5); one GX stage on
   core 3 (G3); the pane walk (W1); fault mode's crash investigation (H3).
4. **The two large redesigns, only if step 1 supports them:** G1 + G2 (the GX side: a translated command cache over
   GPU-resident geometry), then H1 + H2 (hammock blocks with CR in flags).
5. **Re-score the ceiling** after each large item, on Twilight Princess and on a second game, and revise section 7.
