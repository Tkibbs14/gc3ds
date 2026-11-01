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

- **X2 re-score (2026-10-20):** the half-way register setting is accepted and protects the core-2 chase, but vw2
  still loses: heavy -1.66% speed / -2.09% core 0 (752 vs 751), title matched cut/later -0.98%/-0.66%
  (756 vs 755; derived). Rejection and default-off status stand for this setting; see the X2 measurement section.

## C3. Display-list vertex cache on the CPU

- **Status:** built, off by default (`--dlcache on`). Result: no gain (jobs 111-119); kept as the evidence for C1.
- **Re-evaluate when:** records could live in fast memory (they cannot: AXI WRAM is kernel-only, PERF_LEVERS), or if a
  much smaller record (positions only, the stream's own format) fits the L2 with the rest of the working set.

## C4. Shrink VIn (the worker's per-vertex record)

- **Status:** built and on by default (`--vin`, jobs 588-602: heavy +0.7%, Ordon +0.9%); re-confirmed 2026-10-23 (jobs
  1100-1111: heavy +0.9%, Ordon +1.3%, Mario Party 4 +1.2%, Wind Waker +0.2%); the ring reservation is compact too
  since fffee2c (even within noise; the GX thread's wait for the worker 7.6 -> 6.4%).
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
- **2026-10-18:** the opposite direction measured better: lighting moved out of the shader onto the worker (the
  lighting shader was 26 of the heavy area's 43.6 ms of GPU a frame; jobs 411-416, +2.9%). More shader work is a
  GPU cost; the worker has headroom (~25-35% busy).

## C7. Queue counters on their own cache lines

- **Status:** built and **on by default** with no switch - the layout is unconditional (pica.c:337-340, `VwLine`
  aligned to 32 bytes: `g_vwHeadL`, `g_vwTailL`, and `g_vwPosL` at 2153). Nothing to re-run, so rule 4's trigger
  does not apply.
- **What:** `g_vwHead` (GX thread writes) and `g_vwTail` (worker writes) are declared together (pica.c:140), so they
  share a 32-byte line that moves between cores 2 and 3 on every job, even with one worker; with C2, `g_vwClaim` too.
  ARM11 MPCore: 32-byte lines, MESI, the SCU moves modified lines core to core (ARM DDI 0360F §1.3.4, §7.1.1, read by a
  Haiku pass; the TRM text is in the session scratchpad).
- **Also from that pass:** the 2 MB L2 is shared by all cores, so a second worker streaming through guest RAM evicts
  core 0's working set: the likeliest reason job 125's core 0 slowed even with few helper jobs, if job 126 (helper
  sleeps instead of spinning) does not recover it.

## C8. Single-precision floats in the JIT (PERF_LEVERS lever 2)

- **Status:** parked 2026-10-17 (was next, top priority since 2026-10-13): relaxed floats (OPT_FASTFLOAT), which remove
  all rounding to single, gained nothing on the title benchmark (job 338) and +0.5% at 1.7 SE in gameplay (job 373), so
  cheaper rounding cannot gain more. Re-evaluate when core 0 is no longer bound by instruction fetch.
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
- **2026-10-18, guest frame skip instead (`--gfskip ADDR`):** the game's renderer itself (TP: mDoGph_Painter) returns at
  once on every Nth call, so game logic runs every frame and only drawing is skipped (no stale render-to-texture):
  game frames a second +55-65% in Ordon, +48% in the heavy area; area transitions need the fade guards
  (`--gfskip-ok`, JUTFader idle), which fixed them (owner-checked). Off by default; the owner wants full speed before
  frame skipping.
- **Re-evaluate when:** ~~the owner wants a playability option rather than full speed~~ **answered 2026-10-30: the
  owner asked for this to be pursued and got right ("if we can make it look natural").** Note that the *GX-side*
  `--fskip` is separately disqualified as a speed lever: it reads +9% on `speed_pct` while real frames a second fall
  **46%** (7.17 -> 3.90, 2026-10-30), so only the guest-side `--gfskip` form above is a candidate.

### C9a. The trade this actually offers, and why it is paced to a game speed now (2026-10-30)

- **The arithmetic, now measured** (jobs 1616-1621, build `103456ecbdfe9264`, `--gfskip-n 2/3/4` on both scenes,
  three points per scene fitted to `G(N) = base / (1 - R(N-1)/N)`; `# guest frames 10s` gives the renderer's calls
  and how many drew). Rendering is **R = 0.737 of a game frame in static Ordon and 0.618 in the heavy area**, so
  logic is the other 26% and 38%:

  | | N=1 | N=2 | N=3 | N=4 | N=6 | N=8 | asymptote |
  |---|---|---|---|---|---|---|---|
  | **Ordon** game speed | 35% | 56% | 69% | 79% | 91% | 99% | 134% |
  | **Ordon** pictures/s | 10.5 | 8.3 | 6.9 | 5.9 | 4.6 | 3.7 | -> 0 |
  | **heavy** game speed | 24% | 35% | 41% | 45% | 50% | 52% | **63%** |
  | **heavy** pictures/s | 7.2 | 5.2 | 4.1 | 3.4 | 2.5 | 2.0 | -> 0 |

  Measured against fitted at the three points: Ordon 15.97 / 20.80 / 23.73 game frames a second against 16.69 /
  20.71 / 23.56, heavy 10.77 / 12.00 / 13.50 against 10.44 / 12.27 / 13.44 - within 5%, worst at N=2.
- **What that says.** Static Ordon reaches **full game speed at N = 8.4, showing 3.6 pictures a second**; the heavy
  area **cannot reach it at any N** (asymptote 63%: logic is 38% of its frame). A one-point refit from N=2 alone
  briefly read R = 0.68 and put the Ordon figure at N = 20 and 1.5 pictures a second - **N=2 is the worst-fitting
  of the three points, and fitting to it alone was wrong by more than the effect being measured.**
  So guest frame skip is not a 10% lever, it is the difference between slow motion and a correctly paced game at a
  low picture rate. And it **multiplies with base speed** rather than competing with it: at full game speed in Ordon,
  the picture rate is 4.0/s today, **11.1/s at 1.5x base speed and 18.0/s at 2x** - the metric worth optimising is
  *pictures a second while holding full game speed*, which no scene has ever been scored on.
- **Why a target, not a ratio (built 2026-10-30, `--gfskip-target PCT`, off by default).** What a player feels is the
  game's speed *changing* with the scene - Ordon 35%, the heavy area 24%, the cutscene 41% - which moves animation,
  input response and music together. A fixed ratio keeps that variation and adds its own (it skips in a cheap scene
  that did not need it and under-skips in an expensive one). The controller holds one game speed everywhere and lets
  the **picture rate** take the variation, which is much the less noticeable of the two. It learns the real cost of a
  drawn and of a skipped frame, solves `x*cd + (1-x)*cs = target` for the drawn fraction, and picks which frames to
  draw with an accumulator so they stay evenly spaced - the gap spread is the thing to protect (jobs 380-381:
  skipping added no stutter, sd 9-21 ms against 12-24 ms). It clamps to one picture in ten when the target is
  unreachable, so the heavy area degrades instead of freezing, and the existing fade / JUTFader guards still force a
  draw. `hle_tp.c`'s self-test drives it with a synthetic clock (AGENTS' checks table); four planted bugs must fail
  it, including one that clumps draws while keeping the right rate, since even spacing is the claim.
### C9b. The pacer on hardware: it holds one game speed, and the *floor* governs a heavy scene (jobs 1638-1643)

Build `dbac5779b30d8a92`, 240 s each, `--gfskip-target PCT` with the fader guards. **The design claim holds.**

| | fixed ratio N=2 | target 50 | target 60 | target 70 |
|---|---|---|---|---|
| **Ordon** game frames/s | 16.0 | **15.2** (51%) | 18.8 (63%) | 22.4 (75%) |
| **Ordon** pictures/s | 8.0 | 7.83 | 6.64 | 5.00 |
| **heavy** game frames/s | 10.8 | **15.9** (53%) | 15.9 (53%) | - |
| **heavy** pictures/s | 5.3 | 1.35 | 1.44 | - |

- **One target really does hold one game speed across scenes**, which is what the whole idea was for: at target 50
  Ordon runs 15.2 game frames a second and the heavy area 15.9, a **4.6% spread**, where the fixed ratio gives 16.0
  against 10.8 - a **48%** spread. The game no longer slows by a third as you walk into a busy area.
- **Ordon tracks its target to within +1 to +7%, always slightly fast** (51 / 63 / 75% for 50 / 60 / 70), so it skips
  a little more than it needs and gives up picture rate for nothing. The overshoot grows with the target, which is
  what an overestimated skipped-frame cost looks like; a calibration item, not a design one.
- **In the heavy area the floor governs, not the target.** Targets 50, 60 and **100** all give exactly 15.9 game
  frames a second and 1.4 pictures a second, all reporting `wants to draw 0.100, floored 159` - the solver asks for
  less than one picture in ten at every target, so the clamp decides and the target does nothing there. That is the
  floor test passing (it degrades and *says* it is floored rather than freezing or pinning silently), and it is also
  the feature's real limit: 53% of full speed at 1.4 pictures a second.
- **Pacing stays even, which was the other claim.** Gap standard deviation 10.2 ms on a 127.8 ms mean in Ordon at
  target 50 and 5.3 ms on 699 ms in the heavy area, with the largest gap only 8-9% over the mean and no spikes -
  against 5.3 ms on 95.3 ms and 3.7 ms on 137.4 ms with no skipping at all. Relatively jitterier, absolutely small.
- **ANSWERED by the owner, 2026-10-31: "I want smoother over choppy."** Built as `--gfskip-minfps P`, default **5
  pictures a second**: the drawn fraction is at least what P requires, so a heavy scene gives up *game speed* rather
  than the display. With the learned costs cd and cs, `x / (x*cd + (1-x)*cs) >= P` gives
  `x >= P*cs / (1 - P*(cd - cs))`, which reaches 1 exactly at `P = 1/cd` - the most the scene can show - so an
  impossible floor asks for every frame and no more. It is a floor and not a cap; the ratio floor stays as the
  backstop for the first frames, before any costs are learned.

  | floor | Ordon: game speed / pictures a second | heavy area: game speed / pictures a second |
  |---|---|---|
  | the ratio floor (1 in 10) | 82.5% / **2.48** | 47.6% / **1.43** |
  | **5 pictures/s (the default)** | 67.3% / 5.00 | 34.8% / 5.00 |
  | 7 pictures/s | 55.3% / 7.00 | 27.7% / 7.00 |

  Note what the floor does to the *shape* of the trade: it makes the **picture rate** the constant across scenes and
  lets the game speed vary - the mirror image of what `--gfskip-target` alone does. The two together are the useful
  pair: the target governs a scene that can afford it, the floor takes over in one that cannot.
- **Self-tested, with the promise as the assertion.** The synthetic-clock case now asserts the *realised* picture
  rate never falls below the floor at P = 4, 5 and 6 with the heavy area's measured costs, that an impossible floor
  (P = 30, where the expression's denominator turns negative) draws **every** frame rather than none, and that an
  easy scene is left alone. Three planted bugs fail it: the floor doing nothing (showed 1.43 against a floor of 4),
  the floor acting as a cap (an easy scene throttled, mean frame 32 ms against a 48 ms target), and the degenerate
  branch returning 0 instead of 1 (drew 60 frames of 600). **That third one passed every earlier case** - the
  branch had no test until the P = 30 case was added, which is the rule about self-tests doing its work.
- **Re-evaluate when:** the owner has watched Ordon at target 60-70 (6.6 and 5.0 pictures a second, the band worth
  seeing) and said which floor he wants.

- **The fixed ratio is measured; the pacer is built but unseen.** `--gfskip-target` is in build
  `12b2a797a2e18bb6`; jobs 1632-1637 ask the question the design rests on - whether one target holds **one game
  speed across scenes** (50% and 60% on both Ordon and the heavy area, where the fixed ratio gives 16.0 and 10.8
  game frames a second), plus 70% on Ordon as the first setting worth watching and 100% on the heavy area, which
  **cannot** reach it, as the floor test: it must degrade to one picture in ten and say it is floored rather than
  freeze. **"Natural" is a question for eyes after that, not arithmetic.**
- **Re-evaluate when:** the fixed-ratio trade curve is confirmed on the current build, and the owner has watched the
  paced version at a target that the scene can hold (Ordon 70% looks like the first thing worth trying).

### C9c. The pacer latched up on hardware and paced nothing (jobs 1681-1688, 2026-10-31) - fixed

Build `2ba59806b89d7a22`, the first runs with `--gfskip-minfps`. Four of the six runs **skipped not one frame** while
reporting a healthy-looking controller. Ordon read `game 11.00 drawn 11.00`, the heavy area `game 7.97 drawn 7.97`.

> **Correction to C9b's numbers, 2026-10-31.** The floor table above (jobs 1638-1643 and the follow-ups) was measured
> with the old single-sample warm-up, so **any run of that build could silently be a no-skip run**. The figures that
> show skipping happening (a drawn and a shown rate that differ) are still evidence; a run whose two rates are equal
> measured nothing. The floor table's own runs all show skipping, so it stands, but no number from that build should
> be trusted without checking that the two rates differ.

- **What the report said:** `a drawn frame 94.6 ms, a skipped one 149.5 ms, wants to draw 1.000 of them; the floor
  decided 0, the ratio backstop 0, drawing all 0`. A **skipped** frame costing more than a drawn one is physically
  impossible: it is the same frame's logic minus the rendering.
- **How it was caught, and the thing worth remembering:** not from the speed, which looked plausible, but because
  `cs` read **exactly 149.5 in every ten-second window of a four-minute run** while `cd` moved (94.6 -> 94.1). A
  learned value that never moves has only one sample. The pacer took one skip sample during warm-up, seeded the mean
  with it, found `cs >= cd`, so the solver could not act; and because the solver then skipped nothing, `cs` could
  never get another sample. **One bad frame disabled the feature permanently and nothing in the log said so.**
- **It was luck, not a setting.** Job 1685 (Ordon, floor 4) got `drawn 92.4, skipped 33.3, wants 0.243` and paced
  correctly; job 1642 on the older build got 33.3 too - but its *first* window read `drawn 363.6, skipped 34.1`, so
  warm-up samples are wildly noisy and the single one could land anywhere.
- **Fixed three ways** (`hle_tp.c`, the latch-up note above `gfskip_skip`): the means are incremental with weight
  1/n up to 1/16, so no sample seeds them forever; a skip sample at or above the drawn cost is **rejected** as
  impossible and counted; and while the costs cannot be solved, **one frame in 16 is skipped anyway** to re-sample
  `cs`, with the accumulator credited so the frame is moved rather than lost. The probe is what makes the failure
  impossible rather than unlikely. `# gfskip target 10s` now prints `probed` and `impossible samples`, so a latched
  controller is visible in the log instead of inferred from a value that does not move.
- **Self-tested with the failure itself:** the synthetic-clock case poisons **every** warm-up skip sample (400 ms
  against a 95 ms drawn frame) and asserts the run still lands within 3% of its target, that samples were rejected
  and that the probe fired. Removing the probe reproduces the device exactly - the scored mean comes back at 95.00 ms,
  a drawn frame's own cost, against a 47.62 ms target. Accepting the impossible samples fails it too (the draws end
  up uneven). The earlier cases did not reach either path.
- **Re-evaluate when:** the fixed build's runs are in. The hardware answer to "does the floor look natural" is still
  outstanding - everything measured so far at the floor came from the build that could latch.

### C9d. Seen by a human at last, and shelved as a speed strategy (jobs 1713-1722, 2026-10-31)

**The owner's decision, after watching six runs: "let's just get things at native speed and keep working at
improving performance."** Frame skip stays built, exact and off - a **user option**, not a route to full speed.
Everything below is why, and it is worth keeping because the reasoning is not obvious from the arithmetic alone.

**The trade curve is four points, not a dial.** A picture can only land on a game-frame boundary, and a frame costs
`cd` drawn or `cs` skipped, so the achievable picture intervals are `cd + n*cs` for whole n and nothing between.
With static Ordon's measured 92.5 and 34.6 ms:

| frames a picture | pictures a second | game speed | the owner's verdict, watching |
|---|---|---|---|
| 1 (draw everything) | 10.8 | 36% | fine |
| **2** | **8.4** | **51%** (job 1722) | **"a little slideshowy on camera pans"** |
| 3 | 6.2 | ~62% | - |
| 4 | 5.1 | ~68% | "very jittery", "more slideshowy" |

`--gfskip-target PCT` therefore does not set a speed; it picks one of these points. **A target or floor between two
points is rounded, and a floor just above a point collapses to "draw everything"** - asking for 8 pictures when the
points are 10.8 and 7.9 switches the feature off, because a floor is a promise and 7.9 breaks it. That edge is in
the code and should either gain a tolerance or report "your floor is not achievable here; the nearest points are X
and Y", because at present it silently does nothing.

**It helps where the game is already fastest and cannot help where it is slowest.** The useful band is between the
scene's native picture rate and the rate a player will accept. Static Ordon natively shows 10.8, so there is room.
TP's heavy area natively shows **7.3** - already below the 8.4 that reads as slightly slideshowy - so one picture in
two there would give ~5.1, the rate rejected twice. **A slow scene has no picture budget to spend because it is
already spending it**, which is the exact inverse of what the feature was wanted for.

**Where the cost lands: camera pans.** Standing still at 8.4 pictures a second is invisible; panning the camera is
not. This has a consequence for the whole measurement programme: **both benchmark scenes take no input, so neither
contains a camera pan**, and they are therefore the scenes where a low picture rate looks its best. Every number the
project can measure *understates* the perceptual cost, and the only place it shows is the scripted walk, which
cannot be used for speed (C41). **Speed from the static scenes, smoothness from the walk, never mixed.**

**And a human's verdict on a single run is not reproducible.** Jobs 1718, 1719 and 1720 all drew *every frame* (two
latch-ups, below) and were therefore the same configuration - the unpaced emulator - yet they were called
"smoother", "don't notice a difference, okay" and "definitely choppier, more slideshowy". The scene's own variation
along the walk (`cd` wandering 92-101 ms window to window) is larger than the differences being judged. **A
perceptual question needs a blind A/B inside one run** (`--ab gfskip` flips mid-run and logs the phase), not one
run watched at a time. Four of the owner's six viewings were spent before this was understood.

### C9e. Two more latch-ups, both found on hardware after the first was fixed (jobs 1718-1721)

Recorded because they are the same failure three times and the shape is worth recognising: **a controller that
cannot act stops producing the samples it needs to learn that it cannot act.**

1. **Unsolvable costs** (C9c, jobs 1681-1688): one bad warm-up sample left `cs >= cd`, the solver could not act, so
   nothing was skipped, so `cs` was never re-sampled. Fixed with incremental means, impossible-sample rejection, and
   a probe that skips one frame in 16 while the costs cannot be solved.
2. **Costs that merely look solvable** (jobs 1718-1719): `cd` 96.5 ms against a stale `cs` of 84.3 (true ~33). `cd >
   cs`, so the solver believed itself; the floor asked for 75% drawn; snapping rounded 1.34 frames a picture to 1;
   nothing was skipped; `cs` was never corrected. **The probe did not fire, because it only covered case 1.** Fixed
   by probing a solver pinned at `x = 1` as well - but only where the target says there is skipping to do and the
   floor is not itself demanding every frame, since a floor above what a scene can show is *correctly* served by
   drawing everything (job 1718 was right to do nothing). Four times slower there, as the estimate is suspect
   rather than unusable.
3. **The jitter nobody's test could see** (job 1717): the self-test asserted the spread of *skip counts* between
   pictures, which read 0 while the spread in *time* was 32% (job 1715, gaps alternating 92 and 125 ms). Frames have
   unequal cost, so an even number of skips is not an even amount of time. The harness now measures gaps in
   milliseconds; `--gfskip-snap` (on) rounds the period to a whole number of frames so every gap is identical.

Each has a planted-bug self-test that fails without its fix: *"gaps still vary by 28.2% of the mean"*, *"drew 1500
of 1500 scored frames - it never skipped"*, *"a solver pinned at x=1 was never probed"*, and *"landed at 92.00 ms,
more than a frame from the 66.67 ms target"* (the snap must round to nearest, not down).

- **Re-evaluate when:** base speed rises enough that a scene's native picture rate clears the acceptable rate with
  room to spare - the band only opens from above. At twice today's speed static Ordon would show ~21 pictures a
  second natively, and one in two would still leave ~10.5, which the owner finds fine. **So this feature becomes
  useful as a consequence of ordinary performance work, not as a substitute for it** - which is the owner's call of
  2026-10-31, reached by looking at it.

## C53. The pane walk is refused by exactly two pane classes, both already half-built (2026-10-31)

- **Status:** **next**, and the best-sized lever on the board. Found by three instrument steps in one afternoon, each
  narrowing the one before; no code written yet, on purpose.
- **How it was found, because the path matters.** AGENTS' top open direction was "other TEV block types", sized from
  4,720 refusals a 10 s window that perf.csv labelled **"tev block"**. That label was a bug: the reason code has
  **21 call sites** and only one is a TEV block, while `otherTev` - the only field recording a real unported TEV
  block - reads **0** in those runs. With the label fixed to name the refusing *line*, 100% of it turned out to be
  `walk_scan`'s pane-class test; with that test instrumented per class, it is **two addresses and no tail**:

  | address | class | code size | refusals a 10 s window |
  |---|---|---|---|
  | `0x802FDB90` | **`J2DPicture::drawSelf`** | 0x6C (108 bytes) | ~1,184 |
  | `0x803075AC` | **`J2DTextBoxEx::drawSelf`** | 0x300 (768 bytes) | ~3,182 |

  The two sum to **exactly** the walk-scan line's count (4,366 in that window), so they are the whole of it. Names
  from zeldaret/tp `config/GZ2E01/symbols.txt` (fetched outside the repository, as the tools expect).
- **Why this is unusually cheap.** Both connect to work already in the tree:
  - **`J2DTextBoxEx::drawSelf` is already ported** - it is `PORT_TEXT`, bit 10, built, checked exact, and **off**
    because it measured -4.3% in Ordon as a standalone **entry hook** (and bits 9+10 together -0.46% at the current
    default). That is the project's own documented lesson - *small functions lose as entry-hook stand-ins; a port
    pays only when it does less than the guest code*. **Inside the walk it is a different proposition:** the subtree
    is already in native context, so there is no per-call entry, no separate `port_commit`, and no cycle charge per
    call. This is precisely the "larger ports where a whole subtree is replaced" direction, with the component
    already written and proven exact.
  - **`J2DPicture::drawSelf` is 108 bytes** and `J2DPictureEx::drawSelf` - already ported - is its derived class, so
    the work is likely a subset of code that exists.
- **The prize, sized:** the walk takes 1,450 calls and refuses 3,500 per 10 s in TP's heavy area, so accepting both
  classes could take coverage from ~29% to near 100% - about **3.4x** - on a port credited **+2.2%** in Ordon and
  costing 4.52% of core 0 at today's coverage.
- **Why it might not pay, stated before building it:** a refusal is counted per *subtree*, and one unknown pane deep
  inside rejects the whole thing, so the refusals are an upper bound on what accepting these two classes unlocks - a
  subtree may still refuse on depth or the 512-pane cap once they are accepted (the line histogram will say
  immediately). The TextBoxEx port also cost 137 us a call as an entry hook; inside the walk it loses the entry and
  commit overhead but not the work itself, so the walk's own cost grows and the net is a hypothesis, not a
  prediction. And `J2DTextBoxEx` is *text*: if the heavy area's refusals are mostly the HUD, the same panes may be
  cheap for the guest to draw and dear for a port to replicate.
- **Checks it needs:** `PORT_CHECK=1` on the PC over TP's title states and `--port check` on the 2DS in the scenes
  that have the HUD (the existing bit-10 checks already cover the TextBoxEx path), plus `porthash.py` entries for
  any newly followed guest function.
- **The `J2DPicture` half is scoped (2026-10-31), from zeldaret/tp's sources - not from the DOL, so no disc image is
  needed to write it.** The vtable slot is the three-argument `drawSelf(f32, f32, Mtx*)`, and the chain is short:

  | guest function | size | what it does |
  |---|---|---|
  | `J2DPicture::drawSelf` | 0x6C | if `mTexture[0]` and `mTextureNum`, call `drawFullSet` with `mGlobalBounds` + the offsets |
  | `J2DPicture::drawFullSet` | 0x74 | forwards to `drawTexCoord` with `mBounds` and the four texture corners |
  | `J2DPicture::drawTexCoord` | 0x250 | `load(i)` per texture, `GXSetNumTexGens`, `getNewColor`, `setTevMode`, `MTXConcat(m, mGlobalMtx)`, `GXLoadPosMtxImm`, vtx desc + fmt, then `GXBegin(GX_QUADS, 4)` and four position/colour/texcoord vertices |
  | `J2DPicture::load` | 0x34 | `mTexture[i]->load(stage)`, only for `i < 2` |
  | `J2DPicture::getNewColor` | 0xE0 | the four corner colours |
  | `J2DPicture::setTevMode` | 0x490 | **not a mode switch** - straight-line GX writes, branching only on `mTextureNum` (1 or 2) and `mTexture[0]->getTransparency()` |

  **`setTevMode`'s size was the one thing that looked expensive and it is not**: 1,168 bytes of sequential GX setter
  calls with two boolean branches, not a switch over many cases, so the port reproduces a fixed program parameterised
  by two values rather than a table of modes.

  **Every primitive it needs is already in `port_j2d.c`**: `gx_clear_vtx_desc`, `gx_set_num_tex_gens`,
  `gx_load_pos_mtx_imm`, `gx_set_tev_order`, `gx_set_tev_color`, `gx_set_tev_color_in`, `gx_set_tev_alpha_in`,
  `gx_set_tev_op` / `op_mode`, `psmtx_concat` for `MTXConcat`, and `gx_begin(c, 128, 0, 4)` for the quad - which the
  PictureEx and text paths already use (`port_j2d.c` lines 940 and 2152). Field offsets come from the decompilation's
  header: `mTexture[2]` at 0x100, `mTextureNum` at 0x108.

  **So the work is:** accept `0x802FDB90` in `walk_scan`, add a `walk_picture()` in the walk's dispatch beside the
  existing `A_PICEX_DRAWSELF` branch, and **give up** rather than guess on `mTextureNum > 2`, a null texture, or
  anything in `getNewColor` / `setTevMode` not reproduced - the give-up histogram then shows the residue instead of
  it being silent. Checks: `PORT_CHECK=1` on the PC over TP's title states, `--port check` on the 2DS in a scene with
  the HUD, and `porthash.py` entries for each newly followed guest function.
- **The two halves are COUPLED, and J2DPicture alone is worth exactly nothing (a peer's PORT_CHECK on the owner's
  PC, 2026-10-31).** The port was built, its hashes generated and three faults in it fixed - and then
  `picture_draw_self` ran **0 times** over the title states, so the "0 differed" it reported was vacuous. The
  measurement that explains it: with the port on, the walk accepts 1,236 more subtrees and visits **412 J2DPicture
  panes, every one of them hidden** (`vis` false). None had no box, none were clipped. **Every *visible* picture
  sits in a subtree that also holds a `J2DTextBoxEx` pane, so the subtree is still refused** - of the 1,648 picture
  refusals, 412 simply move one pane further on and are refused by `0x803075AC` instead (7,931 -> 8,343).
  **A subtree is all-or-nothing: the walk cannot accept it unless *every* pane class in it is accepted**, so the
  ~1,184 picture and ~3,182 text refusals a window are not independent quantities that can be attacked separately.
  The 1,236 "vanishing" hook calls are also explained and are not a fault: an accepted parent absorbs its children's
  hooked calls, so they are never made - which is what a subtree port is supposed to do.
- **So the order of work is forced:** `J2DTextBoxEx` is the half that unblocks anything, and it is **already built
  and exact as port bit 10** (`PORT_TEXT`), off only because it lost as a standalone entry hook. Folding that into
  the walk is the whole of C53's value; `J2DPicture` is the cheap passenger that comes along with it.
- **And it cannot be checked on the PC at all.** The title states have no visible picture outside a text subtree, so
  exactness for this path needs the 2DS (`--port check` in a scene with the HUD and minimap, which AGENTS already
  says the PC states do not cover) or the text half first. **Queued as jobs 1755-1756** (`--port-picture on` with
  `--port check` and `--port plant`, static Ordon).
- **The float tail is 11% and is not what the record said** (jobs 1753-1754, 2026-10-31). After the class test
  (82%), the next-largest give-up reason is `plain()` refusing a float, and it is **100% NaN or infinity with zero
  denormals** - 864 per 10 s in Ordon, 640 in the heavy area, all in `psmtx_concat`. The nine-day-old claim that
  these were denormals came from a counter that merged the two cases; CONTRIBUTOR_TASKS 9 is corrected in place and
  is **no longer a prerequisite** for this entry or for a subtree port. Matching Gekko's NaN payloads means routing
  `ps_madd` through `gekko_softfp.c`, so it is worth doing only after C53 and only after the counter is split again
  (NaN against infinity).
- **BUILT and exact on the device (2026-11-01, `5e44cdb` + `b40dcc4` + `8aa6dc7`, `--port-walktext`).** The text half
  is folded into the walk: `walk_scan` accepts J2DTextBoxEx's two vtable slots, `walk_pane` reproduces its
  `rewriteAlpha` (source-identical to J2DPictureEx's, `mMaterial` at +0x138 against +0x150, so its own hash row -
  generated on a machine with the disc image, in a run that reproduced the J2DPictureEx row byte for byte as a
  calibration), and the walk now carries `J2DPane::draw`'s x and y, which no previously accepted pane class read
  (children get +0.0f, as `iter->draw(0, 0, ...)` does). A separate switch from `--port-hooks` bit 10 on purpose:
  that bit is the standalone entry hook, which lost 4.3% in Ordon, and the fold has to be measurable without it.
  - **Jobs 1757-1758, static Ordon:** `--port check` **differed 0** with `drew picture 0 text 1000` - and that
    second number is the point: the J2DPicture draft's "0 differed" was over **zero calls**, so the counter was
    added before the run rather than after it. `--port plant` **differed 29,442**, so the checker does report a
    planted fault on the new path.
  - **The pane-class test now refuses nothing.** `# port walk-scan 10s` prints an empty histogram and an empty
    unported-`drawSelf` list, where job 1755 (picture half alone) had 1,410 refusals a window and `803075ac`
    x16,074. The refusals that remain are the walk's own: roots (reason "memory") and `plain()`'s NaN/inf tail.
  - **It is a different port now, not a bigger one:** **104,292.4** real guest instructions a call against
    **13,377** before (job 1080), and 14,984 pipe bytes against 2,221. Hook entries a window fall from 2,610 to 160
    because an accepted parent absorbs its children's hooked calls. **Do not compare the two sums** - the old 870
    accepted calls a window were nested inside one another, so summing them double-counts, while the new 50 are
    disjoint top-level subtrees. Only a speed run settles it.
  - Its clock charge was recalibrated with it (`kInsScale` 129 -> 135, conditional on the switch so the off arm of an
    A/B is unchanged): the stale scale under-charged the emulated clock by 5% of the walk's own work.
- **MEASURED: static Ordon +3.46%, heavy area +2.31%** (jobs 1760-1767, 240 s, two runs an arm, two rounds with the
  second reversed, build `8aa6dc730f9b`): on 10.367 / 10.376 against 10.030 / 10.020, and 7.193 / 7.195 against
  7.031 / 7.032. **All four on-runs above all four off-runs**, spreads 0.01-0.10% against gaps of 2.3-3.5%. The
  largest single gain since the ports as a set. MEASUREMENTS has the full table and the reading notes.
- **It re-prices the float give-up it was supposed to wait behind.** With C53 on, `plain()` refuses 37.5% of the
  walk's hook entries in both scenes (Ordon 660 of 1,760, heavy area 480 of 1,280; the rest are roots), each
  abandoning a ~104,000-instruction subtree rather than a ~13,000-instruction one - so CONTRIBUTOR_TASKS 9, downgraded
  to an 11% tail hours earlier on the pre-C53 picture, is now the walk's biggest addressable refusal and would take
  coverage 2.2x. Both readings are right at their own time. **C49 in reverse: an option raising another's value.**
  The remedy is built (`--port-softfloat`, `psmtx_concat_exact`, off; jobs 1774-1775 check it).
- **Status: measured and exact; needs one re-measurement before it becomes a default.** The figures were taken on
  one side of a device step that moved static Ordon 5.6% the same night (jobs 1760-1784; MEASUREMENTS' withdrawn
  regression section). The *ratio* holds - the arms were interleaved and each repeated an hour apart, off 10.030 /
  10.020 against on 10.367 / 10.376, which is a split and not a trend - but the absolute figures belong to the
  pre-boundary state. The "~5% regression" this entry previously said it was held back by **does not exist**: all
  seven commits in range read 10.54-10.62, including the one that had read 10.03. (The earlier "re-evaluate when the J2DTextBoxEx half
  is folded into the walk" is now done. The J2DPicture half's own "0 differed" still covers zero drawn pictures and
  still must not be quoted as evidence on its own; what makes it safe to turn on is that the *walk* is checked with
  both halves accepted, which is what jobs 1757-1759 do.)
- **Re-evaluate when:** the speed runs land. If the fold does not pay despite replacing 7.6x the guest work a call,
  that is a result about subtree ports in general and belongs here with its numbers, not a reason to retry blind.

## C56. J3DDrawBuffer::drawHead as a subtree port - 9.88%, and it absorbs the hooks that lose alone (2026-11-01)

- **The target.** `J3DDrawBuffer::draw` is **9.88% inclusive** of core 0 in TP's heavy area at the current defaults
  (job 1821, after C53), the largest unexplored subtree in the profile, with `drawHead` 9.77%,
  `drawOpaDrawList__12dDlst_list_c` 7.89% and `J3DShapePacket::drawFast` **6.87%** nested inside it. It is the "J3D
  packet draw" direction AGENTS has named for weeks, now sized.
- **It is the pane walk's shape exactly.** `drawHead` is two loops - over an entry table, then over each bucket's
  linked list - calling a **virtual `packet->draw()`**, so a port recognises packet classes the way the pane walk
  recognises pane classes. There are **four, with no tail**: `J3DMatPacket` (the real work), `J3DShapePacket`,
  `J3DDrawPacket`, and `J3DPacket` whose `draw()` is empty. C53's two-class test is the precedent.
- **The spine is small, ~1,730 bytes over eleven functions** (sizes from zeldaret's map):
  `J3DDrawBuffer::draw` 0x54, `drawHead` 0x78, `J3DMatPacket::draw` 0x84, `J3DShapePacket::drawFast` 0x88,
  `J3DShapePacket::draw` 0x98, `prepareDraw` 0x11C, `J3DShape::drawFast` 0x194, `loadPreDrawSetting` 0x98,
  `J3DMaterial::load` 0x50, `setArrayAndBindPipeline` 0xB4, `J3DShapeDraw::draw` 0x2C, `J3DShapeMtx::load` 0x5C.
  For comparison, the pane walk's `J2DPane::draw` was 334 instructions (~1,336 bytes).
- **The strongest argument for it: it absorbs the hooks that lose on their own.** `J3DShape::drawFast`'s inner loop is
  `mShapeMtx[i]->load(); mShapeDraw[i]->draw();` and `mShapeMtx[i]->load()` *is* port bits 2 and 9 - the concat-view
  and multi-concat-view matrix loads, which measured **-0.03%** and **-0.46%** as standalone entry hooks even though
  bit 2 is the most-called port in the scene (1.53 M calls, ~5.8% of core 0 by its own timers). A subtree port has no
  per-call entry or commit, which is exactly what turned port bit 10 from **-4.3% standalone** into part of C53's
  **+3.4%**. The same structure would also absorb `J3DMaterial::load`, the VCD/VAT display-list call,
  `setArrayAndBindPipeline` and the shape draws into one context with one commit.
- **Expected, with C53's two discounts attached.** C53 converted a <=9.5% inclusive subsystem into +3.4%, so the
  honest order of magnitude here is similar - but **a subtree is all-or-nothing** (all four packet classes have to be
  accepted or the subtree is refused), and **coverage is not value** (C53's J2DPicture half accepts 412 panes a
  window and draws none). Expect less than the inclusive share, and do not quote a figure before a run.
- **Step 1 done: there is no unbounded tail**, which was the one thing that could have killed the entry.
  `J3DMaterial::load` is four lines (`setMaterialMode`, then `loadNBTScale` unless `j3dSys` flag 2 is set) and
  `setArrayAndBindPipeline` is seven stores plus `loadVtxArray`, which is three `J3DLoadArrayBasePtr` calls - FIFO
  writes gc3ds already stands in for. Both class sets are closed.
- **Step 2 done, and it is better than the sizing suggested** (`--j3dstat`, jobs 1822-1823, 10 s windows):

  | entry point | heavy area | static Ordon |
  |---|---|---|
  | `J3DDrawBuffer::draw` = `drawHead` | 1,680 | 2,310 |
  | **`J3DMatPacket::draw`** | **10,560** | **5,280** |
  | `J3DShapePacket::draw` | **0** | **0** |
  | `J3DDrawPacket::draw` | **0** | **0** |
  | `J3DPacket::draw` | **0** | **0** |
  | `J3DShapePacket::drawFast` | 11,600 | 7,260 |
  | `J3DShapeMtx::load` | **0** | **0** |
  | **`J3DShapeMtxConcatView::load`** | **10,160** | **5,390** |
  | `J3DShapeMtxMulti::load` | **0** | **0** |
  | **`J3DShapeMtxMultiConcatView::load`** | **10,320** | **14,410** |
  | `J3DShapeMtxBBoardConcatView::load` | **0** | **0** |
  | `J3DShapeMtxYBBoardConcatView::load` | **0** | **0** |

  **Only one packet class of four and two matrix classes of six occur at all**, in either scene - so the port is
  *simpler* than the pane walk, which had two pane classes, and three quarters of the ~3.3 KB sizing need not be
  written. Better still, **both surviving matrix loads are already written**: `J3DShapeMtxMultiConcatView::load`
  (0x80313FA4) *is* port bit 9, complete and exact, and the inner `loadMtxConcatView_PNGP` (0x80313828) that
  `J3DShapeMtxConcatView::load` calls *is* port bit 2. Those are the two hooks that measure -0.46% and -0.03% alone.
  **The absorption is large:** one `drawHead` call covers ~6.3 material loads, ~6.9 shape draws and ~12.2 matrix
  loads in the heavy area (2.3 / 3.1 / 8.6 in Ordon), so a subtree context removes that many entry-and-commit pairs
  per call. C53's equivalent was ~100 panes a walk.
  Note Ordon runs **more** skinned loads than the heavy area (14,410 against 10,320) - Link and the NPCs - so the two
  scenes exercise the two classes in opposite proportions, which is useful: a port that got either wrong would fail
  a check in one of them.
- **Step 3 done - the scan says the port would take 90.5% of calls** (`--j3dstat`, jobs 1826-1827, 10 s windows; the
  read-only form of the exact all-or-nothing test the port would make):

  | | heavy area | static Ordon |
  |---|---|---|
  | `drawHead` calls | 1,680 | 2,310 |
  | **would be taken** | **1,520 (90.5%)** | **2,090 (90.5%)** |
  | refused on a packet class | 160 | 220 |
  | refused on a **matrix** class | **0** | **0** |
  | unreadable | 0 | 0 |
  | a taken call covers | 6.6 mat packets, 6.6 shape packets, **10.4 matrix groups** | 2.2 / 2.2 / 5.4 |
  | entry table | 56 buckets a call | 56 |

  **Rule 8's cross-check passes**, which is what makes the figure usable: 6.6 mat packets x 1,520 taken calls =
  10,032 against the census's **10,560** total, i.e. **95% agreement** with the gap sitting in the 160 refused
  calls; the matrix groups reconcile to 77%. Two independent views of the same quantity agree.
  **The matrix-class test never refuses in either scene**, so the two classes the census found really are all of
  them and nothing beyond port bits 2 and 9 has to be written. The residue is the packet-class test alone, a bounded
  9.5% of calls.
  **Each taken call absorbs ~24 entry-and-commit pairs** in the heavy area (6.6 + 6.6 + 10.4). C53's pane walk
  absorbed ~100 panes a walk and converted a <=9.5% inclusive subsystem into +3.4%; this subtree is 9.88% inclusive
  with ~90% of calls and ~80-95% of the work addressable. **That is an analogy and not a prediction** - C53 itself
  showed coverage over-stating value - but it is the best-evidenced entry this list has had before a port was built.
- **Step 4 done: the scan is in the port framework and the two implementations agree exactly** (`PORT_J3DDRAW`, the
  hook at `drawHead` 0x80325500, declining; job 1850 with both scans on):

  | | calls | would be taken | packet-class refusals | matrix-class |
  |---|---|---|---|---|
  | the port framework's scan (`span`/`f32`, the port's guards) | 1,722 | 1,558 (90.5%) | 164 | **0** |
  | `--j3dstat`'s scan in `hle_tp.c` (its own guarded reads) | 1,722 | 1,558 (90.5%) | 164 | **0** |

  Identical in every figure, and the hook's own give-up line agrees too (reason 5 = 164, matching the packet-class
  count). **So the 90.5% is established through the path the drawing will use**, not only through a side instrument -
  which is why the scan went into the framework before the drawing rather than after.
  **It also took a fix to get there, and the fault is instructive:** `--j3dstat`'s branch in `hle_run` *returned*,
  and `drawHead` is one of its thirteen addresses, so enabling both gave the port hook 0 calls and no report line at
  all. The instrument built to validate another instrument prevented it from running. The census counts and falls
  through now. Rule 8 says to difference a new instrument against an existing one; it did not occur to me that the
  two had to be able to run at once.
- **The 13 hash rows are in and verified on their own** (job 1844): no "guest function differs" line, the pane walk
  still taking 400 calls a window, speed 7.222 against the 7.229/7.231 it read without them. Done separately and
  first because `port_valid()` turns **every** port off on one mismatch, which would have cost ~10% and looked like
  a regression in whatever was committed next.
- **And both matrix loads this port would absorb were re-measured as entry hooks at the current default** (jobs
  1840-1848): bit 2 **-0.03% -> -0.94%**, bit 9 **-0.46% -> +0.08%**. Same kind of code in the same subtree, moving
  a point apart in opposite directions, so "the baseline moved" does not explain it. Bit 2's cost as an entry hook
  going from nothing to about a point is a direct strengthening of this entry's argument.
- **The scan's one over-estimate, checked and dismissed.** The scan tests vtables only, so it does not model the
  refusals *inside* the matrix loads it would delegate to - and bit 2's port refuses outright when a differed
  texture matrix is present (`give_up(c, 7)`), rather than reproduce `J3DDifferedTexMtx::loadExecute`, which is
  1,532 bytes of matrix maths. C56 inherits that refusal, so 90.5% could have been an over-estimate.
  **It is not: bit 2 reads 74,872 taken against 8 not taken in a 10 s window (job 1840), and the 8 are float
  refusals, not texgen ones - zero differed-texmtx refusals in 74,880 calls.** So the inherited refusal fires about
  once in ten thousand and the coverage figure stands. Checked from a log already on disk rather than assumed in
  either direction.
  That count also reconciles two instruments: 74,872 PNGP calls against the census's 10,160
  `J3DShapeMtxConcatView::load` is right, because PNGP is the inner per-matrix function - ~10,160 from the single
  path plus ~64,700 from the multi path's loop over about six matrices a call.
- **The instrument fault worth keeping** (2026-11-01): the scan's first version read "66.7% taken, 1.8 mat packets a
  call" because a `mEntryTableSize > 64` guard rejected a quarter of all calls as unreadable - and they were the big
  buffers. The entry table averages **56** buckets, *below* that guard, so **the mean hid the tail**: a guard has to
  be set against the distribution, not the average. Caught by differencing against the census (6.3 a call against
  the scan's 1.8), which is rule 8 and is why the census was written first.
- **The drawing is written** (2026-11-01, `--port-hooks` bit 11 / 2048, off): `drawHead`'s two loops,
  `J3DMatPacket::draw`, `J3DShapePacket::drawFast`, `prepareDraw`, `J3DShape::drawFast` with both its matrix
  branches, `loadPreDrawSetting`, `J3DMaterial::load`, `setArrayAndBindPipeline`, `loadVtxArray`,
  `J3DCurrentMtx::load`, `resetMtxLoadCache`, `J3DShapeDraw::draw` and the single-matrix
  `J3DShapeMtxConcatView::load` wrapping bit 2's helper, with a `J3DShapeMtxMultiConcatView` matrix delegated to
  bit 9's existing port. **It is neither checked nor measured yet**, and three things gate that, in order:
  the four new hash rows (recorded as 0, which keeps every port off until a console run publishes them -
  `# port hashes`); `--port check` with 0 differed **and** a non-zero `drew`; and `kInsScale[11]`, still the
  placeholder 100. `docs/research/LIBRARY_PORTS.md` has the whole write-up.
- **The design's one open gap closed itself, and self-checked.** `J3DDifferedTexMtx::sTexGenBlock` and
  `sTexMtxObj` are in zeldaret's map after all, at 0x804515C0 and 0x804515C4 - the earlier search mangled the
  17-character class name as 16. The confirmation needed no disc image: port bit 2 has always refused on
  `f32(r13 - 28608)`, which is 0x804515C0 exactly, so the symbol map and code written months before it agree.
  `j3d_draw_head` now checks r13 against the implied 0x80458580 once per call so the absolute and relative forms
  cannot drift apart unnoticed.
- **One refusal the drawing adds that the scan did not model**, so the 90.5% above is now an upper bound: a shape
  packet with `mpTexMtxObj != 0` sets `sTexGenBlock` and sends the matrix load down bit 2's refused differed path.
  Refused in the scan, counted as `tex mtx obj N`. The bullet above established that bit 2's *own* texgen refusal
  fires once in ten thousand calls - but that was measured with bit 2 as an entry hook, where `sTexGenBlock` is
  whatever the guest's `drawFast` left; inside the subtree the port sets it, so the two are not the same question
  and the new counter is the one that answers it.
- **Measured, and it is a loss: -0.79% in TP's heavy area** (jobs 1877-1880, on 7.142 / 7.139 against off
  7.206 / 7.189, complete rank separation the wrong way, on-arm spread 0.04%). Exact first: 180 checked calls,
  differed 0, with `drew 180 calls, 920 mat packets, 1,640 shape draws` and no `CHECK INCOMPLETE`; plant 28,028
  (jobs 1875-1876). MEASUREMENTS has the full entry.
- **Why, in one number.** The port's own timers read 59.03 us building and 13.87 us committing = **72.9 us a call =
  58,612 ARM cycles**, to replace **3,010.6** guest instructions. **19.5 cycles per guest instruction replaced,
  against the JIT's 21.2** - the port is 8% faster than compiled code at the same work, and 8% cannot pay for a
  hook entry, a commit, a compiled-code flush and 1.2 KB more instruction-fetch footprint.
- **So this entry's own central argument was wrong, and it is worth saying which part.** "It is the pane walk's
  shape exactly" was true structurally and irrelevant economically. C53 did not win by being a subtree; it won by
  **doing less** - skipping hidden panes, absorbing a text path - and its real guest instructions a call went
  13,377 -> 104,292. C56 reproduces `drawHead` step by step and absorbs the two matrix loads without changing what
  they compute. Bit 2 at -0.94% per call and C56 at -0.79% per subtree are **the same finding at two scales**:
  absorption removes a hook's overhead, not the work.
> **Correction, same day: the "20x instrument gap" I published here does not reproduce, and the hypothesis
> attached to it is dead.** Job 1850's census read `J3DMatPacket::draw` entered 413 times where the scan said
> 8,177 mat packets; two runs on the current build read **5,994** and **6,480** (jobs 1881-1882), the same order as
> the scan. And `--tier` is not the mechanism: every counter in those two runs differs by exactly 1.081,
> `drawHead` included, so the per-frame structure is identical with region inlining on and off. One 70 s pair
> killed it, which is what writing it down as a hypothesis rather than a cause was for.
- **The real coverage finding, from the same pair: the port covers about half the packet-draw work.** With C56
  taking **1,332 of 1,554** drawHead calls and drawing 6,808 mat packets, the guest still entered
  `J3DMatPacket::draw` **5,994** times in that window, and the ~222 refused calls account for only ~1,130 of them.
  So ~45% of mat-packet draws reach that function by another route; `J3DShapePacket::draw` reads 0, so
  `J3DDrawBuffer::drawTail` is the candidate and is now census slot 2.
- **Coverage is nonetheless not the problem, and doubling it would not rescue this.** The port is 8% faster than
  compiled code at the work it does; twice the coverage is twice the saving *and* twice the cost. Anyone taking
  this further has to make the port **compute differently** - batch the matrix loads, drop redundant GX state,
  merge display-list calls - not cover more of the same work. That is the honest next step and it is a different
  piece of work from the one this entry proposed.
- **Still open: 9.88% inclusive against ~2.2% of core 0's instructions at the measured coverage.** Until that is
  explained, an inclusive profile share should not size a subtree candidate, C54 included. The census is no longer
  part of that question - it agrees with the scan.
- **Re-evaluate when:** someone has a way to make the subtree cost *less* rather than the same, or the
  9.88%-against-2.2% gap is explained. The drawing code is written and proven exact, so a changed hook or a changed
  strategy is cheap to try. **Status: built, exact, off (port bit 11 / 2048), measured as a loss.**

## C55. The 2DS launches into one of several memory states up to ~5% apart, set at each chainload (2026-11-01)

> **Two rungs priced directly, 2026-11-01 (jobs 1920-1925).** C48's `FAST_BITS` sweep produced a pair of builds
> that differ *only* by a no-op line (the indirect tables are malloc'd at the FAST_BITS 16 size, which at
> FAST_BITS 16 is the size they already were), so they are effectively one binary on two rungs:
> **rung 7.1 reads 7.352 / 7.341 and rung 8.9 reads 7.226 / 7.223 - rung 7.1 is +1.69% faster**, with within-arm
> spreads of 0.15% and 0.04%.
> So the ladder now has a measured middle: **7.1 fast, 8.9 about 1.7% below it, 9.0 about 5% below** (and 2.9
> performs like 7.1). That also means the absolutes of everything measured on 8.9 today - `--gpruns`'s
> 7.19-7.23, C40's re-pricing, C56's -0.79% - sit ~1.7% low, though each of those compared arms *within* one rung
> and so their differences stand.
> **What it does not tell us is why**, and the obvious guess is wrong: the 8.9 build allocates *the same* as the
> 7.1 one. Same code, same allocations, two launches, 1.7% apart - so the rung really is a property of the launch
> and not only of the build's footprint, exactly as this entry says. The `FAST_BITS` arms show the other half of
> it: a build that allocates 1.75 MB less lands on a *different* rung reproducibly. **Both halves are true, which
> is why the only safe rule is the one `runs.py` enforces - compare within a rung, never across.**

- **The measurement.** Static Ordon at unchanged defaults read **10.020** frames a second at ~00:33 (job 1766) and
  **10.592** at ~01:3x on the identical commit (job 1781). Seven commits were built and run across that boundary and
  the profile is flat on each side: everything before reads ~10.03, everything after reads **10.54-10.62**. It is a
  step, not a drift - the runs either side of it are tight (0.05-0.10% within a state).
- **What it is not.** The clock is the same (`pmu_mcyc` 804 both sides, so no throttle). The work is the same: every
  per-frame rate - `draws_s`, `verts_s`, `gxbytes_k`, `gpu_calls_s` - scales at exactly the speed ratio 0.945. The
  host side is idle in both (`log_pct`, `disc_pct`, `present_pct`, `wait_pct` all 0, `polls_s` scales with speed).
  The code is the same (the step happens on one commit re-run) and the JIT is the same (24,800 against 24,665 blocks,
  hot words 2.251 against 2.245).
- **What it looks like, from the PMU.** Identical work, a worse memory system: `dcrmiss_k` **+10.1%**, `dmtlb_k`
  **+14.3%**, `scu_c0miss_k` **+7.6%**, `icmiss_k` only +2.4% and `stall_inst_m` -2.0%. So core 0's *data* side is
  the whole of it, and the instruction side is untouched. Its IPC falls from 0.398 to 0.378.
- **Why it matters more than any single optimisation on this list.** Rule 3's method is "separate whole runs, one
  session, one build". **This says that is not sufficient.** Every ledger entry under ~5% taken from arms that were
  not interleaved inside one state is suspect, and the project has 100+ of them. It also cost this session an hour:
  the step was first written up as a -5.6% regression in our own code, bisected across eight commits, and withdrawn
  (MEASUREMENTS' withdrawn-regression section).
- **FOUND, and the instrument already existed: the step is the app's launch memory state, and every log prints it.**
  `# heap: largest malloc block` reads **7.1 MB** in the fast state and **9.0 MB** in the slow one, and over every
  plain Ordon run on disk the correlation is exact with no exceptions:

  | `largest malloc block` | `speed_pct`, defaults | runs |
  |---|---|---|
  | **7.1 MB** | 34.45-34.87 | 1718-1724, 1751, 1768, 1769, 1776-1781 |
  | **9.0 MB** | 32.94-32.98 | 1753, 1761, 1766 |

  In the 9.0 state the app holds **1.9 MB more free heap** (6.08 against 4.19 MB) and runs ~5% slower.
- **What the difference is *not* - four hypotheses killed from the logs already on disk, no device time.**
  (1) *An emulator allocation that failed.* The `# heap` probe runs **after** MEM1 and the JIT buffer, and both are
  at the **same address in both states** (`MEM1 at 081f6000`, the JIT buffer at `0aed7000`), with linear memory
  (10.5 MB free) and VRAM (388 KB free) identical too. (2) *The display-list vertex caches*, a peer's reading of
  `vcache_create`, which does fail silently and is about the right size: **both caches are present in both states**
  (every log has its `# dlcache 10s (gx thread)` and `(worker)` lines). (3) *A relay restart*: a peer's deploy log
  puts one at 00:24:14, inside job 1764, which stayed slow, and no log holds a failed-request line. (4) *The clock*:
  `pmu_mcyc` 804 and the same `clock MHz` header on both sides. **And the two states print an identical set of
  report-line types**, so nothing the program knows about is missing. The 1.9 MB is real and unexplained; it may be
  a symptom of the same system state rather than the cause of the slowdown.
- **Five values, and one of them is the bad one - it is not a ladder** (corrected twice the same night; the second
  reading, "speed falls monotonically as free heap rises", came from three profiled heavy-area runs and does not
  survive the 301 logs on disk). The values are **2.9, 7.1, 7.2, 8.6 and 9.0 MB**, with 2.9 the historical majority
  (228 of 301 logs, the headline runs among them). What the wider data says:
  - **9.0 is reproducibly bad.** Its Ordon defaults cluster at **32.89-32.98** across five runs, against 7.1's
    34.45-34.87, and the one *controlled* comparison - the same commit, same defaults - reads **10.030 / 10.020**
    on 9.0 and **10.587 / 10.592** on 7.1, about -4.8%.
  - **2.9 and 7.1 are equivalent**, which kills the monotone reading outright: 2.9 has the *least* free heap and its
    Ordon defaults sit at the same level (the headline runs 1690/1691 read 34.62 / 34.70).
  - **8.6 is unresolved**: three runs, all profiled heavy-area, sitting between 9.0's and 7.1's - too few to claim
    anything, and profiled runs are the wrong instrument for it.
  So the statement that survives is "**the 9.0 state is bad**", not "free heap predicts speed". The heap value is a
  *label* for the state; which labels are bad has to be measured per label rather than inferred from the number.
- **The state is fixed when the process starts and inherited by every job that reuses it** (a peer's reading of the
  launch path, confirmed from the `# relay:` headers). A job either downloads a build and restarts into it through
  Luma's `hb:ldr` - a self-chainload - or reuses the resident process. The heap value changes **only** at a
  chainload: job 1759 chainloaded into 9.0 and the eight jobs after it (1760-1767, which include all of C53's arms)
  inherited 9.0; job 1768 chainloaded into 7.1 and 1769-1770 inherited 7.1. **A chainload does not pick a state
  deterministically** - 1753, 1757, 1759 and 1774 landed in 9.0 while 1776-1784 all landed in 7.1 - which looks
  like fragmentation accumulating across successive self-chainloads rather than anything a run does.
- **So the practical rule, usable today:** a measurement series wants **one process**, or at least one heap value,
  and a fresh launch by hand before a series is the cheapest way to get a known starting point. `runs.py` enforces
  the weaker version by refusing to compare across values. **This is also the most likely identity of the project's
  "~2% day drift" lore** - a series that chainloads part-way through moves between rungs.
- **Where to look next, given all that.** Both hottest regions are identically placed and the clock matches, so the
  difference is not in *where* the big data sits. What remains: a system-level memory configuration that changes
  both the app's heap size and something performance-relevant (Luma's extended-memory mode, or whatever else was
  resident when gc3ds launched), or a smaller heap allocation that nothing reports. The heap probe is a *marker* of
  the state; treating it as the mechanism is the mistake this entry has already made twice.
- **The instrument is in and it narrows the question to one number** (`# heap shape`, jobs 1811-1820, all of which
  landed in the 9.0 rung): `region 08100020-0d8c5000 (87.77 MB), sbrk at 0cfc3000 (78.76 MB handed out), arena
  78.76 MB, in use 78.75 MB, free 0.01 MB in 11 blocks, linear heap 28 MB`. So **`largest malloc block` is simply the
  un-handed tail of the heap region** (87.77 - 78.76 = 9.01), and the arena holds essentially nothing free. The
  emulator's own allocations are 78.75 MB with 11 free blocks either way, which means **the rungs differ in the size
  of the region the OS gave the app, not in what we allocate** - the prediction for a 7.1 run is a region of ~85.9 MB
  with the same 78.75 MB in use. A *larger* app heap region correlating with *slower* points at the launcher's
  memory split, not at anything in this code base. **Waiting for a 7.1 sample is not worth device time**: the rung is
  sticky across chainloads (it switched twice in about twenty tonight), so the line will answer the moment one
  happens.
- **Done:** `runs.py` now prints `heap N MB` on every line and **warns loudly when the logs scored together do not
  share a state**, with a planted self-test (a differing pair must be called out, a matching pair must not). That is
  the check that would have saved the hour this entry cost.
- **Still open, in order.** (1) **Log the app's total heap and every allocation over ~256 KB** (asked, returned,
  address) at startup - the log currently says the largest free block and nothing about the shape of the heap, which
  is why four plausible hypotheses each had to be killed one at a time. `pica_vcache_alloc` is the first instance:
  `vcache_create` could lose 7.5 MB silently and now says so. (2) **What puts the device in the 9.0 state** - it
  spans 20:41 to 00:41 across many jobs and both states survive restarts into a resident build, so it is set
  outside a single run; the obvious suspects are system-level (extended memory, what was running before). (3)
  Whether the slow state can be refused at startup once it is identifiable from inside the program.
- **THE LEDGER IS SOUND ON THIS AXIS - audited, and nothing straddles a rung** (2026-11-01, `rungs.py`, from logs
  on disk with no device time). Every decision set checked is internally one rung:

  | entry | jobs | rung |
  |---|---|---|
  | the headline (all defaults, build 2ba59806) | 1690-1699 | all **2.9** |
  | the port-bit apportionment (bit 8 -0.26%, bit 2 -0.03%, bit 3 +0.08%) | 1700-1711 | all **2.9** |
  | bits 9 and 10's withdrawal (-0.46%) | 1667-1668 | all **2.9** |
  | the cloud-session baseline | 1723-1732 | all **7.1** |
  | rule 3's noise floor | 1692/1693, 1700/1701 | all **2.9** |
  | everything measured on 2026-11-01 (C53, soft float, C49) | 1753-1839 | all **9.0** |

  So C55 does **not** invalidate the recorded figures - it explains why tonight's absolutes sit below both
  baselines (the 9.0 rung is the slow one) and why the two baselines are themselves comparable in rung terms (2.9
  performs like 7.1), leaving the toolchain as the only difference between them, as documented. **The worry that
  prompted this audit was reasonable and the answer is no.**
- **Until (1) and (2):** group every comparison by the heap column, which `runs.py` now does for you, and
  `rungs.py --by-rung` for everything at once.
- **Re-evaluate when:** the allocation is named. **Status: next for the instrument side** - and note that the
  5%-wide state was invisible for at least one previous session (jobs 1753-1767 are all in it), so **old ledger
  entries whose arms straddle a state change are wrong by up to 5%**, and the heap column makes them re-checkable
  from the logs already on disk.


## C54. The J3D joint and animation calc tree: ~11% of core 0, never looked at (2026-10-31)

- **Status:** **next**, alongside C53, and the first new large target in some time.
- **What the measurement says** (job 1750, the first decoded guest *stack* profile, TP heavy area):
  `recursiveCalc__8J3DJoint` reads **11.42% of core 0 inclusive** and only **0.90% leaf**, with
  `J3DModel::calc` 6.03%, `calcAnmMtx__8J3DModel` 5.26% and `J3DJointTree::calc` 5.17% nested under or beside it.
  Time spread across a tree rather than concentrated in one function is the signature of a **subtree port**, not a
  stand-in - which is the direction AGENTS already ranks first.
- **Why it is a good shape:** it is the skeletal animation calc - recursive over the joint hierarchy, pure matrix
  work over guest structures, and it produces **no GX output at all**. Nothing to reproduce in the pipe, no TEV
  state, no texture objects; just matrices in guest memory. That is the easiest kind of port to make exact, and the
  paired-single matrix library it leans on (`PSMTX*`) is already a native stand-in (`hle_ps.c`).
- **Why nobody saw it.** AGENTS records "matrix-chain HLE (flat profile - **but see C54: that was a leaf profile, and the inclusive one shows an 11% joint-calc subtree it could not see**)" as *rejected*. That judgement came from a
  **leaf** profile, where this tree is 0.90% and looks like nothing. An 11% subtree spread thinly across dozens of
  functions is invisible to a leaf profile by construction - and the inclusive profile needed zeldaret's symbol map,
  which lives outside the repository and had never been fetched into a session. **The rejection was not wrong about
  what it measured; it measured the wrong thing.** Corrected in place rather than deleted (rule 7).
- **CORRECTED the same day, before anything was built: it is ~5.2-5.5%, not ~11-13%.** `recursiveCalc` is recursive
  and `stack_funcs.py` counts a function once per nesting level, so its inclusive share is inflated by its depth -
  the tool's own docstring says so and I quoted it and then ignored it. A child cannot exceed its parent, and the
  non-recursive parent `J3DJointTree::calc` reads **5.17%** in the heavy area and **5.47%** in static Ordon, with
  `calcAnmMtx` at 5.26 / 5.60%. **So the joint calc tree is ~5.2-5.5%**, which is still the third-largest
  self-contained subtree and still holds across both scenes - but it is half what the first write-up claimed, and
  the claim would have justified more work than the target deserves.
- **The scene check is done and it holds (job 1751): 5.47% in static Ordon against 5.17% in the heavy area**, so
  this is not a heavy-area artifact.
- **First steps, cheapest first:** (1) done - the share holds; (2) read `J3DJoint::recursiveCalc` and `J3DJointTree::calc` in zeldaret's sources and
  size the subtree the way C53's J2DPicture half was sized - which functions, how many bytes, which branches have
  to become give-ups; (3) only then write anything.
- **Expected:** unknown, deliberately. The honest upper bound is the corrected ~5.2-5.5%, and the honest discount is that ports
  have repeatedly delivered a fraction of their inclusive share (the ports as a *set* are +10.6% against ~3.7% for
  their parts, and PNGP cost 5.8% of core 0 to deliver -0.03%).
- **The sizing, from zeldaret's sources (2026-10-31):** `J3DModel::calc` is a short sequence - flags,
  `syncJ3DSysFlags`, `mVertexBuffer.frameInit`, four *optional virtual* hooks (`mUnkCalc2`, `mDeformData->deform`,
  `mVtxColorCalc`, `mUnkCalc1`), then `calcAnmMtx`. The joint walk's work is a **virtual call** on `J3DMtxCalc`,
  so a port has to recognise which implementation is in play exactly as the pane walk recognises pane classes - and
  that is where it would refuse. The whole `J3DMtxCalc` family is only **30 functions and 2.5 KB**, with three
  `calcTransform` strategies (Basic 0x120, Softimage 0x16C, Maya 0x180 bytes), so the code to reproduce is small;
  the open question is how many of the optional hooks and which transform strategies TP actually uses, which is a
  counter on the device rather than a guess.
- **Re-evaluate when:** a device counter says which `J3DMtxCalc` implementations and optional hooks TP exercises.
  C54 now sits clearly behind C53 (which is <=9.5% in the heavy area and <=14.8% in Ordon with its component already
  built and exact), and ahead of nothing else large.

> **2026-11-01: C54's ~5.15% is an inclusive profile share, and C56 has just shown that such a share does not size
> a subtree port.** C56 was ranked first on 9.88% inclusive and delivered **-0.79%**; the arithmetic closes
> independently (3,010.6 guest instructions a call at ~144 calls a second = 1.41% of core 0's block-code
> instructions, ~2.7% for the whole subtree, which cannot be 9.88% of samples unless those instructions cost ~3.6x
> an average one - and the port replaces them at 19.5 cycles each against compiled code's 21.2, so they do not).
> The only two calibrations that exist are C53 at <=9.5% inclusive -> +2.3-3.5% realised and C56 at 9.88% -> below
> zero, so the factor is somewhere between 3x and infinity.
> **So C54 is unsized, not second-ranked.** What it needs before any code is written is the same number C56's check
> mode produced: **guest instructions per call**, plus an answer to what the port could *skip or compute
> differently* - because C56's loss was not a coverage problem (it covers ~53% and doubling that doubles cost and
> saving alike) but a "the port does the same work" problem. **Status: parked, unsized.** **Re-evaluate when:**
> someone has the instructions-per-call figure for the joint-calc tree *and* a reason it can do less than the
> guest code, rather than an inclusive share.

## C10. Idle-loop skipping and an emulated-CPU underclock

- **Status:** parked (2026-10-13).
- **What:** detect the game's busy-wait loops (polling a register or a variable until an interrupt) and jump the
  emulated clock to the next event; optionally run fewer emulated cycles per field (Dolphin's CPU clock override).
- **For:** time the guest spends waiting is pure cost for core 0. Dolphin does both.
- **Unknown:** how much TP waits in the heavy scenes (the JIT profile was flat, D3, which argues for little).
  **Measure first:** count blocks that branch to themselves without changing state, from the JIT's block profile.
- **Re-evaluate when:** the measurement says waiting is a large share of core 0.
- **2026-10-17:** not a lever for TP's heavy stretches. The later stretch already runs 486.0 M guest instructions per
  emulated second (Gekko's clock: the guest never idles, job 97), and its profile is real work (job 336: ShdwDraw
  38%, its triangle callees ~10%, J3D; no spin loop among the top functions), so an underclock only gives the game
  fewer cycles per frame. The cutscene's waits are already skipped (~1.7 M instructions a field).

## C11. Float lanes: exact singles kept in VFP registers within a block (OPT_FCACHE)

- **Status:** built 2026-10-13 (18d6248, second pass 2026-10-13), exact; **on by default since 2026-10-19**
  (`--fcache off` to compare): separate runs give heavy area +1.5%, Ordon +1.6%, title +1.1% / +2.4% / +0.6%
  (MEASUREMENTS.md "Float lanes and the 2-way indirect table, re-measured"). The history below came from A/Bs that
  recompile all JIT code at each switch.
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
- **2026-10-17, gated (job 335):** lanes only in blocks with >= 8 float instructions before the first branch
  (`--fc-min`): cutscene and later stretch both within noise, I-cache misses +4-5%. The gate does not change the
  verdict.
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

- **Status:** partly built (2026-10-14). Candidate 1 (CR-field dead stores) rejected by its model: exact ~0.3% of hot
  words, inexact ~1.5-2%. Candidate 2 (OPT_RETSTUB) built: -2.1% hot words, +1.0% / +1.6% core 0 (job 148, trimmed
  means), default on together with OPT_SYSCALL and OPT_HLELINK (job 151: +2.2% / +4.2%). Shorter common sequences
  measured neutral on the 2DS: OPT_CRSTUB (compare CR updates in shared stubs, job 155), OPT_ROTMASK (rlwinm as shifts,
  job 156); both off.
  > **Correction, 2026-11-01 (an audit against the code): OPT_ROTMASK is ON by default and is not neutral.**
  > `main.c:661` reads `o->rotmask = true` with "heavy area +0.6%, Ordon +0.3%, title -0.1 to +0.3%; with vin
  > +1.3% / +1.2% (jobs 585-604)". So job 156's "neutral" was wrong by 0.3-0.6 points in the direction of nothing,
  > and this line carried the wrong verdict for two weeks.
  > **That makes its sibling the highest-yield re-test batch in the project, and for a stated reason**: OPT_CRSTUB
  > (job 155) came from the same session by the same method, and that method - pooled A/B means over runs that
  > recompile, before the stolen-second filter - is the one that also produced float lanes' rejection (reversed to
  > +1.5%/+1.6% and a default) and `--dlcache`'s "no gain" (re-tested at -1.05%). C16's OPT_BEFLOAT ("-0.8% / +0.1%",
  > signs disagreeing between scenes) and C27's OPT_PIPEPS ("+0.3% at 1.9 SE", an uncertainty six times the effect)
  > are the other two. All three are built, exact, and need only separate whole runs: four adjacent an arm,
  > `tp_area2`, one build, one rung, `icmiss_k` quoted, and the feature's own counter quoted for both arms to prove
  > they differed. Candidates 3 (float loads and stores, ~1% of hot words) and 4 (register pinning) are parked:
  words shaved per instruction have stopped showing on the hardware (HANDOFF 2026-10-14 late night).
- **Lesson:** executed hot words per guest instruction did not predict I-cache misses for this change: misses follow the
  hot code's footprint (distinct lines touched), not the words executed per instruction.
- **Re-evaluate when:** several small exact reductions can be stacked into one A/B, or a footprint measure (distinct hot
  lines per field) shows which code the misses come from.

## C14. Traces as the default (blocks follow branches, side exits)

- **Status:** rejected 2026-10-14 (job 154): core 0 cutscene -5.5%, I-cache misses +17%, later stretch even.
- **Why:** duplicated tails make the hot code bigger; the 2DS's core 0 is bound by instruction fetch, which follows
  the distinct hot code, not the words executed per instruction (-18% under qemu).
- **Re-evaluate when:** traces could share their tails (a trace ends where another block already starts), or the
  I-cache stops being the limit.

## C15. The GX thread runs vertex jobs while it would wait (--steal)

- **Status:** rejected 2026-10-15 (job 164): GX-side waits fell, core 0 slowed 3-5% (L2, as C2). Kept, off.
- **Re-evaluate when:** the gathers stop streaming guest RAM (C1), so the jobs no longer pollute the L2.

- **X2 re-score (2026-10-20):** with half-way allocation, stealing is heavy -0.18% speed / -0.70% core 0
  (753 vs 751), matched title -0.11%/+0.11% (757 vs 755; derived). The old large title penalty is absent,
  but the tiny differences demonstrate no gain. Kept off; X2's working hardware gate does not alone justify it.

## C16. Float load / store stubs in big-endian data mode (OPT_BEFLOAT)

- **Status:** built and measured 2026-10-15 (job 166): neutral (core 0 -0.8% / +0.1%), I-cache misses +6-7%. Off.
- **Why it lost:** the BE stubs were added beside the originals, growing the shared stub code the I-cache holds; the
  ~4 cycles saved per float load did not show.
- **Re-evaluate when:** the BE versions can replace the originals outright (stores of non-singles included), so the
  stub footprint does not grow.

## C17. Smaller texture formats on the PICA200 (and mipmaps)

- **Status:** built 2026-10-18, default on (MEASUREMENTS.md "The heavy Ordon area is GPU-bound by texture fetch"):
  mipmaps from the game's levels (heavy area +19%, GPU 100.6 -> 55.4 ms a frame), small formats at level 2 (exact for
  grey sources, CMPR / RGB5A3 rounded to 16 bits: +6%), then textures in VRAM by DMA (+12%, GPU 33.8 -> 14.4 ms).
  ETC1 not built. (Was parked 2026-10-15; the trigger was the heavy Ordon area, GPU-bound at ~100 ms a frame.)
- **What:** gc3ds decodes every GameCube texture to RGBA8 with level 0 only (`src/gx/pica/pica.c`, `C3D_TexInit(...,
  GPU_RGBA8)`). Decode to the smallest format that holds the source instead (RGB565 / RGBA4 / RGBA5551 / LA8 / L8, or
  ETC1 for CMPR), and upload the mip levels the GameCube texture already carries.
- **For:** the 2DS's fill rate on a texture that misses the texture cache is set by texel bytes (gpuprobe, job 180):
  1024x1024 minified, RGBA8 17 Mpixel/s, RGB565 27, ETC1 117, against 204 for anything that hits. Halving the texel size
  also halves the CPU's decode writes and the upload bandwidth (texture copy measured ~240 MB/s).
- **Against / cost:** GPU time was ~20 ms of a ~190 ms frame (gc3ds-gpu-direction): core 0 is the wall, so the GPU-side
  gain does not show until it moves. Lossy formats (RGB565 for RGB8 sources, ETC1 encoding on the CPU) change pixels:
  frames would no longer be byte-identical with the PC checker unless it learns the same conversion.
- **Re-evaluate when:** GPU time or texture decode shows up as a real share of the frame (native mode, where core 0 is
  freed, is the likely trigger), or a lossless narrower format fits a source (I4/I8/IA4/IA8 -> L8/LA8 now).

## C18. Byte-swapped guest RAM (no rev on word loads and stores)

- **Status:** parked (2026-10-15).
- **What:** keep guest memory in host (little-endian) order per 32-bit word, so a guest word load or store is a plain
  ldr / str with no rev; halfword and byte accesses flip the low address bits (XOR 2 / XOR 3), the trick N64
  emulators use. Doubles and paired singles need their own handling.
- **For:** ldr + rev as a dependent chain costs 6.0 cycles against 4.0 for ldr alone (timing probe, job 183): every
  guest word load whose value is used next pays 2 cycles, plus one instruction of I-cache footprint, the 2DS's main
  stall.
- **Against / cost:** every path that touches guest memory changes: DMA, the GX FIFO and vertex gathers, texture
  decode, the disc and ARAM, and the HLE hooks. The PC checker's byte-identical frames must keep holding.
- **Re-evaluate when:** a frame's counters price the rev share (guest word loads per frame x 2 cycles) against the
  frame, or the JIT's other levers are spent.
- **2026-10-17: started (James: "whatever makes gains").** The JIT's other levers measured 1-5% or neutral this day
  (float lanes, relaxed floats, compares, registers: C11, C22), so this is the next one. Expected core 0 +3-6%, plus
  core 3's attribute gathers and every stand-in's loads. Plan, one compile-time switch (`GC_SWAPRAM`) so both layouts
  build and the PC can compare them: guest RAM held as host-order 32-bit words; 32-bit accesses plain; 16-bit at
  ea ^ 2, 8-bit at ea ^ 3; 64-bit as two words. Sites (survey 2026-10-17): the bus and CPU helpers (system.c
  bus_*, gekko_internal.h gk_ld/st and the inline fast paths, gekko_mem.c byte paths); the JIT's load / store
  emitters, memory and float stubs, instruction fetch; bulk copies that must swap per word (disc and DI DMA, ARAM
  DMA, DOL / apploader loads, locked-cache DMA, TMEM / TLUT loads, EXI); the GX FIFO and display-list parser
  (fifo.c rd16 / rd32), the vertex loaders (vertex.c, vfast.c), texture decode (texture.c), EFB copies; DSP HLE;
  every stand-in's big-endian helpers; the XFB present path; save states (kept big-endian on disk, converted at load
  and save). **Gate:** the same frames, byte for byte, from the same states in both layouts on the PC (last.png and a
  layout-independent RAM hash), the PC HLE_CHECK, the JIT fuzzer; then the 2DS.
- **Stages A and B done (2026-10-17): both layouts build from one tree (`GC_SWAPRAM`, ramlayout.h).** Every guest
  memory access in C goes through ram_* / ramp_* (the CPU and bus, DMA and boot, the GX parser and vertex loaders,
  texture decode through a per-tile big-endian bounce, EFB copies, the stand-ins and ps_translate.py's output, DSP
  HLE, the XFB present paths); the gather pipe's bytes are stored in the RAM layout where they enter the GX
  (`pending`), so one parser reads the pipe and display lists; copies of a stream keep its place in a word
  (ram_copy: the vertex job's raw copy, the merge buffers); save states stay big-endian on disk. The JIT: ram_load /
  ram_store (no rev; a halfword or byte through an EOR of the address into r12), both memory-stub sets, the float
  stubs (grev), lfd / stfd, psq words and quantized lanes (per-lane EOR), lmw / stmw, instruction reads. Check
  hoisting under the swapped layout also proves alignment: the first access through rA checks its address's
  alignment with the window (deopt on failure) and later ones are unchecked only when that covers them (hoist_fits);
  fault mode and OPT_BEFLOAT are off in that layout (a misaligned word would not fault; the BE stubs read big-endian
  RAM).
  **Checks:** GC_SWAPRAM 0 byte-identical to HEAD (boot 600 fields, state hash and last.png). GC_SWAPRAM 1 (PC
  interpreter) against HEAD: boot 600 fields, tp_pc_5000 -> 5100, identical; PC HLE_CHECK in the swapped layout
  fields 900-2100: 5,007,286 PS-library calls, 1,096,284 J3DGD, 5,814 ShdwDraw, 0 differed (HLE_STOP_EVERY=37: 1,200
  stopped inside, 0 differed); JIT fuzzer (FUZZ_CFLAGS=-DGC_SWAPRAM=1, mask 4723): 20,000 cases, 11,148 float, 0
  fails, planted bugs detected, chain seeds 0 fails. Three bugs the comparisons found: the DI INQUIRY reply copied
  raw; `pending`'s leftover moved short of its last word; gx_decode_vertex's matrix-index bytes read raw (the boot
  run has none: only the in-game state showed it).
  **Build:** PC `CFLAGS="... -DGC_SWAPRAM=1" make BUILD=build-swap`; 3DS `bash core/host3ds/build.sh
  BUILD=build-swap EXTRA_CFLAGS=-DGC_SWAPRAM=1`.
- **Device result: neutral; off by default (2026-10-17, jobs 351-352 against 350).** Job 351 lost 1-6% (cutscene
  28.78, later 16.76, movie 11.67): the alignment check added a second deopt exit at each first hoisted access
  (JIT code 7.98 -> 9.80 host words per guest instruction), and the stand-ins' pointer reads tested alignment the
  callers had already checked. With the check folded into the window stub (ST_HOISTCHK2 / 4) and aligned accessors
  (ramp_rd32a etc.), **job 352: cutscene 28.92 (-0.6%), later 17.91 (+0.2%), movie 12.41 (even)**; code 7.78 against
  7.74. Dropping rev from every word load does not show: the same lesson as C11 / C22, core 0 is bound by
  instruction fetch and memory waits, not by the dependent rev. Kept in the tree with GC_SWAPRAM 0 (byte-identical
  to before) as the base for any later layout work. **Re-evaluate when:** something it enables is worth measuring on
  its own, such as float loads straight from guest memory (vldr with no SETEND; OPT_BEFLOAT's measured neutral with
  it), or a stand-in or translated function whose loads are a measured share of a section.

## C19. ShdwDraw descent cache (keep the octree path between cells)

- **Status:** rejected 2026-10-16 (job 299); exact on the PC (HLE_CHECK 6,122 calls, 0 differed), not committed.
- **What:** the ShdwDraw stand-in descends the collision octree from the root for every cell (~3.6 levels); keep the
  previous cell's path and its header reads, and start from the deepest level the next cell shares.
- **Result:** later stretch 15.41% against 15.11-15.4% without it; the stand-in's time per call did not move
  (~10.8 us). The descent was not where the call's cost sits: job 288 puts ~25 of ~420 ARM instructions a cell per
  level, and the store-buffer drains and head work dominate.
- **Re-evaluate when:** a `--pmu-split hle` profile shows the descent's levels as a real share of the call.
- **2026-10-17, retried in the lean fast cells (job 339, on top of the leaf memo):** later stretch 17.48% against 17.47%
  (job 337), the stand-in 2,390-2,530 ms per 10 s against 2,539: flat again. Removed. The descent's loads are L1 hits on
  a short dependent chain; skipping some of them does not show.

## C20. __save_gpr / __restore_gpr as in-place stand-ins

- **Status:** legacy C form rejected 2026-10-17 (jobs 321-326); X3 native form still rejected; **the word-inline
  form is ON by default** (`main.c:629`, `o->gprinline = true`, +0.80% Ordon / +0.57% heavy / +0.64% cutscene,
  jobs 708-725) - this line read "positive/off" until an audit on 2026-11-01 checked it against the code.
  **It also has no trigger of its own**: the one below covers only the rejected native-helper form.
- **What:** the CodeWarrior runtime's register save and restore (2.5% of the cutscene, job 311), entered at each
  `_savegpr_N`, as C called in place of `bl` (ps_translate.py `__save_gpr.dis@K`).
- **Result:** a small loss (job 322 with them: cutscene 28.29%, later 15.70%, against 28.98 / 16.35 without, job
  318). The call writes back every dirty cached GPR first, and after a restore the caller forgets them all: more than
  the 1-18 word moves saved.
- **Re-evaluate when:** the JIT can pass the cached registers to a stand-in (or inline the moves itself: these are
  plain stores and loads it could emit in place, without a call).
- **2026-10-20, re-opened as the JIT inline form (EMULATOR_CEILING.md H5; design in HANDOFF.md "Next JIT build"):**
  the offline call count puts the helpers at 22.3% / 16.7% of all calls (cutscene / later stretch), ~6-7% of block
  boundaries; inlined, no flush and no boundary. Estimate +1.5-2.5% core 0. Not built yet.

- **2026-10-20 X3 result (jobs 708-725):** word-inline helpers are built, off, and positive in separate gameplay
  runs (+0.80% / +0.57% Ordon / heavy, derived versus opening controls). The retained-register C form remains
  rejected (-2.03% / -1.94%); keeping cached callee-saved registers did not make it profitable. Closing controls
  preserve the verdict. [Checks, title results and limits](MEASUREMENTS.md#x3-gpr-helper-inlining-and-retained-register-native-calls-2026-10-20). The old estimate above was not attained.
  Re-evaluate native helpers only with a materially different call convention; further ordinary leaf inlining is a
  separate build, not part of X3.

## C21. GX register setters as in-place stand-ins (logged mode)

- **Status:** rejected 2026-10-17 (jobs 321-326); exact on the PC. Re-tested without the log (job 375, entry-checked
  computed ranges in ps_translate.py): -0.7%, still rejected: the call boundary, not the log, is the cost.
- **What:** GXSetArray, GXSetTevOrder, __GXSetMatrixIndex, the GXSetTev* family, GXSetChanCtrl, GXLoadPosMtxImm,
  GXSetTevIndirect (~9% of the cutscene together, job 311): computed addresses into __GXData, so stores go through a
  log applied at the end.
- **Result:** neutral (job 326 against 322). These are 17-100 instructions of integer work and stores; the call, the
  register flush and the log cost what the JIT's code did. The J3DGD display-list writers in this mode were a large loss
  (job 323: cutscene 21.8%): quadratic log reads, one pointer reload per byte.
- **Re-evaluate when:** the translator can prove a computed address stays within a range checked at entry (e.g.
  __GXData + small index), so these run without the log; or when calls get cheaper (C20).

- **2026-10-20 X3 result (jobs 708-725):** the selected GX setter group remains rejected with the retained-register
  native path (-1.71% / -2.07% Ordon / heavy, derived versus opening controls). The closing controls preserve the
  loss; the new `--zgx` stays off. [Exactness checks, title results and limits](MEASUREMENTS.md#x3-gpr-helper-inlining-and-retained-register-native-calls-2026-10-20). Small-SDK automatic
  replacement did not clear its cost gate; re-evaluate with a different call convention or larger eliminated subtree.

## C22. Cheaper compares and more register-cache slots (lever 3 of 2026-10-17)

- **Status:** evaluated 2026-10-17, not built: the ceiling is small.
- **Compares:** cmpli / cmpi / cmpl are ~8% of executed hot words (6.4-7.1 words each, qemu JIT_REGSTAT 950-962);
  building the CR field is 4 of them (BIC + three conditional ORR). Shrinking those through shared stubs was neutral on
  the 2DS (OPT_CRSTUB, job 155); a compare ends its block with the branch that reads it, so the field must reach guest
  state at the exit anyway, and the stores the model can prove dead are ~0.3% of hot words (C13 candidate 1). A flags
  representation of CR fields would have to be understood by every CR reader (mfcr, CR logic ops, record forms, fcmp,
  the interpreter, exceptions, stand-ins): large for at most the ~5% of hot words those four instructions are.
- **Register slots:** the cache uses every free host register (r4-r8); r9-r11 hold the downcount, the RAM base and
  the guest state, and r0-r3, r12 and lr are scratch for every memory access and stub call. Moving the downcount to
  memory costs ~2 words at every block entry (~0.33 per guest instruction, ~7% of hot words), more than a sixth slot
  would save: block-boundary traffic (~4.5 loads and stores per block) does not shrink with more slots.
- **Re-evaluate when:** blocks get longer (more compares consumed inside a block), or a cheaper CR encoding falls out
  of another change.

## C23. Call-heavy game code as native stand-ins (ShdwDraw's triangle path, J3D animation)

- **Status:** rejected 2026-10-17 (jobs 341-345); exact on the PC (4.7M calls, and 345k forced stops, 0 differed).
- **What:** translator v4 (calls between stand-ins, regions, stops before anything unsupported) applied to ShdwDraw's
  per-triangle code and its callees (GetTriPla, SetupNP, psdRealCallBack, dDlst_shadowPoly_c::set, getPolyCode, ...)
  and 13 hot J3D functions.
- **Result:** later stretch 17.47 -> 14.41% once the region always ran (job 345), cutscene 28.75 -> 27.9%. A native
  function's entry and exit move every GPR it uses through guest state, and each nested call adds a lookup and state
  checks: more than the JIT spends on small functions it runs inline. Functions whose callees have no stand-in stop on
  every call and pay the exit as well.
- **Re-evaluate when:** the translator can inline its callees (one C function for a call tree, registers kept in
  locals across the calls), or the JIT can enter compiled native code without going through guest state.

## C24. Lean state: skip citro3d setters that would upload what the GPU holds

- **Status:** rejected 2026-10-17 (jobs 361-363); off behind `--lean MASK` (bits: 1 cull, 2 texture binds, 4 combiner
  stages, 8 combiner buffer, 16 effect dirty bit after apply_frag, 32 float uniforms).
- **What:** citro3d uploads every block whose dirty bit is set, changed or not; a setter whose result equals citro3d's
  own copy is skipped (or its dirty bit dropped again), compared against citro3d's context (layout of 1.7.1 mirrored and
  checked at start).
- **Result:** the uploads fell (texture units 0.6 -> 0.17 a call, combiner stages 3.0 -> 0.6, words 110 -> 95) but the
  draw call only went 8.9 -> 8.2-8.5 us: what a batch uploads is not what the call costs (C25). **All bits at once hung
  the GPU** within a few fields (jobs 361, 362: the GX thread waiting in C3D_FrameBegin); bits 2, 4, 16 and 32 alone ran
  75 s each without a hang (job 363); bits 1 and 8 were never run alone. Cause not found.
- **Re-evaluate when:** the draw call's fixed cost is gone and uploads dominate it; then find the hang first (bits 1 and
  8 alone, then pairs) before measuring anything.

## C25. Raw draw calls and raw state (the batch's draw call without citro3d's per-call code)

- **Status:** built 2026-10-17, default on (`--rawdraw`, `--rawstate`; jobs 364, 366-368).
- **What:** the draw's registers as one block of citro3d 1.7.1's words; the dirty effect, texture-unit, combiner and
  vertex-shader float-uniform blocks written by pica.c in C3Di_UpdateContext's order and words; citro3d only for
  anything else dirty (~5% of calls). The first 256 calls and one in 4,096 after go through citro3d and are compared
  word for word, state included; one difference turns both off for the run (self-test `--rawplant on`, job 368).
- **Result:** a call 8.15 -> 5.3 us, flush_batch 61 -> 41 ms/s, Ordon static frame +0.73% (rawstate, job 367) on top
  of +0.2% (rawdraw, job 364). citro3d's code, not its stores, was the cost: cold in core 2's I-cache between draws.
- **Next steps if the GX thread becomes the limit again:** the remaining 5.3 us (c3d_dirty and raw_state themselves,
  the vertex worker sync), and update_tev (~28 ms/s: the 72-word TEV key built and compared per batch).

## C26. Prefetching citro3d's command buffer after each draw call

- **Status:** rejected 2026-10-17 (job 365); off behind `--cmdpld`.
- **What:** each frame rewrites the command buffer from its start after the submit cleaned it, so every line a call
  writes misses; prefetch the next 1 KB after each call.
- **Result:** 8.2 -> 7.85 us a call, speed -0.6% (-0.134 +- 0.021 points, 10 phase pairs).
- **Re-evaluate when:** never on its own; the misses are a small part of the call.

## C27. psq_st to the write-gather pipe without a jit_gather call per element (OPT_PIPEPS)

- **Status:** built 2026-10-17, off (`--pipeps on`): neutral (jobs 371-372).
- **What:** the SDK's matrix loads (GXLoadPosMtxImm and its kin) psq_st to a known pipe address, which OPT_PIPEK did
  not cover: the stub failed and the site's cold path called jit_gather per element (~127,000 calls/s in gameplay).
- **Result:** inlined at each site, -2.3% (job 371: code growth); appended inside the shared psq_st stubs (sites
  unchanged), +0.3% at 1.9 SE (job 372) with the calls down by ~45,000/s: a C call costs core 0 only ~50 cycles.
- **Re-evaluate when:** something makes the remaining pipe traffic expensive again (e.g. a pipe flush per burst), or
  together with other changes to the stubs; the code stays behind the switch.

## C28. Function memoization (record a call's inputs and effects, replay while the inputs hold)

- **Idea (2026-10-18):** a call's effects are fixed by what it reads: record once (input words with values, output
  words, gather-pipe bytes, result registers), and while every input still holds, apply the effects instead of running
  it. Exact by construction; general to any game.
- **Evidence (jobs 454-456, memo.c probe):** the TP HUD (dMeter2Draw_c::draw) is ~700,000 instructions a frame against
  ~18,000 input words, but its inputs change every frame (~11 animated words in J2DScreen::draw's ~3,900): 0 hits at the
  screen and HUD level.
- **Status:** probe built (`--memo ADDR`), replay not built.
- **Re-evaluate when:** a candidate function with stable inputs shows up (the probe reports hits), or per-pane
  (nested) recording is designed: the HUD's panes mostly do not change, the screen around them always does.
- **2026-10-20, J3D probed (MEASUREMENTS.md "J3D's per-model work, probed for memoization"):** calcMaterial repeats 91%
  of calls but is under 1% of the guest's work; J3DModel::calc 22%, J3DJoint::recursiveCalc 9% (the view matrix moves).
  The probe's previous-call table is now keyed by object (512 entries). Still no candidate worth a replay.

## C29. Native library ports (hand-written JSystem / GX SDK subtrees, one native call each)

- **Idea (2026-10-18, owner decision):** port shared libraries natively from the decompilation and the machine code, a
  subtree per hook, sharing one context (core/src/cpu/port_j2d.c; LIBRARY_PORTS.md).
- **Evidence (jobs 457-498, MEASUREMENTS.md "The first library port"):** J2DMaterial::setGX, J2DPictureEx::drawSelf and
  J3DShapeMtxConcatView's matrix load are exact over >500,000 checked calls (device and PC) and gain Ordon +3.6-5.9%,
  +4.5% and +5.8% (the title neutral for J3D when scored by section). A port that repeats the SDK step by step cost what the JIT did (~35,600
  cycles a call); doing less (plain loads after one range check, pipe bytes through a pointer, the SDK's bookkeeping
  folded) took it to ~15,700.
- **Status:** built, three on by default. Since gpbulk and ring batching the GX thread limits gameplay, so further
  core 0 ports pay mainly in core-0-bound scenes (the title's later stretch).
- **Re-evaluate when:** the GX thread stops being gameplay's limit (then J2DPane::draw, text, the other TEV block
  types, J3D packet draw), or another JSystem game is the target (the same ports by code hash, GAMES.md).

## C30. Double-buffered frames (record the next frame while the GPU draws this one)

- **Idea (2026-10-19):** C3D_FrameBegin waits for the GPU to finish the previous submit because the vertex arena, the
  index buffer and citro3d's command buffer are single-buffered. The GX thread starts the next frame at the first GX
  work after the XFB copy, so it waits out the GPU's tail of the frame just submitted (what came after its last early
  split, and the copy). Two arenas and two command buffers, swapped at each frame end (citro3d's context holds
  cmdBuf / cmdBufSize and the queue: pica.c already reads that layout), would let it record on.
- **Evidence (job 512, heavy area, `--profile on --gxdiag off`):** the GX thread at `framebegin` 5.0-8.3% of its samples
  while the GPU is ~11% busy (c3d_gpu_ms ~19 ms a frame at ~6 frames a second); Ordon 0.1% (GPU frames ~4.6 ms).
- **Against / risk:** a mistake in the GPU queue can freeze the console (`--gpudiag 1` did, job 403): run only when the
  owner is home. Memory: a second arena and command buffer from linear memory (~12 MB free). Gain bounded by the wait:
  heavy area up to ~5%.
- **Status:** built 2026-10-19 and **on by default** (`--dblframe off` to compare; pica.c "overlapped frames"): halves
  of the existing arenas plus one more 2 MB command buffer, a frame fenced on its slot's previous user, the screen
  swapped by the GX thread. Stable and correct on the 2DS (~14 runs). **Heavy area +1.9% frames, Ordon even, title
  even** (separate runs, jobs 548-560). A first verdict of "neutral" came from the broken frames line
  (MEASUREMENTS.md "The frame-rate line was broken").
- **Re-evaluate when:** a scene becomes GPU-bound (then the frame before last is what a frame waits for, and a third
  slot would be the next step).

## C31. Sharper textures (after speed)

- **Idea (2026-10-19, owner):** textures look soft on the 2DS. The owner wants them improved once full speed is reached;
  speed and smoothness come first.
- **Evidence / where the softness comes from (not yet measured one by one):** the whole picture is drawn into a
  512x256 EFB (the GameCube's 640x528 scaled 0.625 x 0.5) and shown at 400x240, so every texture is minified on top of
  the game's own sampling; `--smalltex 2` stores CMPR and RGB5A3 textures that are not exact in a 16-bit format as
  RGB565 / RGBA5551 / RGBA4 (banding and lost detail); `--mips` makes minified textures sample lower levels (blur at
  grazing angles, the PICA200 has no anisotropic filtering); sizes that are not powers of two are resampled nearest
  (upload_level).
- **Options to try then, cheapest first:** a per-texture choice that keeps RGBA8 for textures that lose most in 16 bits
  (an error measure at decode); an LOD bias on the texture units (sharper, more aliasing); mips only for the textures
  the screen minifies most; the 800-pixel-wide top screen mode (gfxSetWide: 800x240, twice the horizontal pixels; needs
  a wider EFB, more GPU fill and VRAM); a light sharpening pass in the present quad's combiner.
- **Against / risk:** each costs VRAM, texture bandwidth or GPU time, which the speed work has been buying back (VRAM
  textures and smaller formats took the heavy area's GPU from 43.6 to ~14 ms a frame).
- **Status:** parked by the owner until full speed.
- **Re-evaluate when:** the target scenes run at full speed, or GPU time per frame has room to spare.


## C32. ShdwDraw walk memo (replay a repeated shadow walk)

- **Idea (2026-10-19):** the shadow scan's stand-in still cost ~25% of core 0 in the title's later stretch, and 98% of
  its calls repeat a previous call exactly (PC SHDW_KEYSTAT). Record a call's walk segment by segment and replay it when
  the same inputs come again, instead of walking ~30,000 cells.
- **Status:** built and **on by default** (`--shdwmemo`; core/src/cpu/hle_tp.c "The walk memo"). Exact per segment
  (PC SHDW_MEMOCHECK: 1,108,244 replayed segments, 0 differed); a replayed segment may overrun the slice's budget by at
  most 32,768 guest instructions (owner-approved timing drift; +0.05% cycles over 400 fields on the PC). 2DS: later
  stretch 19.09 -> 21.81% (+14.2%), 24% more pictures; cutscene, movie, Ordon, heavy area even (MEASUREMENTS.md "The
  ShdwDraw walk memo"). Made the loop head a stand-in entry too (exact on its own).
- **Lessons:** the slice's budget is part of a replay's inputs in effect: segments that stopped at the budget landed
  elsewhere each frame (replays ended at the first); unbounded overruns stretched VI fields (fields are scheduled from
  the last one), which showed as more pictures but a slower game clock. Check the field gaps, not only speed.
- **Re-evaluate when:** shadows move every frame in the target scene (the cutscene records every call and replays
  none; but its shadow walks cost only ~1-18 ms per 10 s, under 0.2% of core 0, job 618).
- **Next step, measured 2026-10-20 (C32b, the triangle path cache):** with the walk replayed, what is left of the shadow
  system in the later stretch is the per-triangle path in ShdwDraw (0x8007FCE4 .. the callback's bctrl at 0x8007FE94, or
  a skip back to 0x8007FEA4) and its callees: ShdwDraw's own blocks 22.4% of core 0, psdRealCallBack 3.5%, GetTriPla
  3.1%, SetupNP 2.6%, getPolyCode 1.5% (job 620, `--profile on`). Facts for a cache of that path (PC, SHDW_BODYSTAT=1,
  tp_pc_5000, 200 fields):
  - 431,847 passes reached the callback and 92,550 skipped (through, or a near-zero dot), over **6,152 distinct keys**
    (r15 the collision object, the triangle index lhz 0(r16), r1): each triangle repeats ~85 times.
  - Everything up to the callback depends only on the collision data and the triangle index. The path writes r0,
    r3-r8, r11, r12, r31; f0-f13 and f28-f31 (both paired-single lanes); CR, LR, CTR, FPSCR; and the frame words at
    r1 + 4, 12-32, 60-96, 100-116, 120-152, 2204, 2208 (plus callee frames below r1, dead). Its only other stores are
    interrupt context saves into mainThread (0x803DB420), not the path's own.
  - The callback's arguments: r3 = lwz 8(r1) (the cBgS_ShdwDraw object, which differs between the four callers: load
    it live), r4 = r1 + 120 (the three vertices), r5-r7 = 0, 1, 2, r8 = r1 + 80 (the plane), CTR = r12 = lwz 48(r3).
  - Plan: a hook at 0x8007FCE4. On a miss, interpret the path to its exit to capture the outputs, then once more from
    deliberately perturbed scratch registers to classify each register as written (same result both times) or passed
    through (result equals its own input); a register that depends on its input makes the key uncacheable. On a hit:
    check a guard of the collision words the path reads (the prism record, its position and normals), write the
    written registers and the frame words, load r3 / r12 / CTR live, and continue at the bctrl or the skip address.
    Memory: ~400 B a key (floats packed where exact), ~2.5 MB for 6,152 keys; the walk memo's records were compacted
    to their exact size first (they held 3.75 MB and left 1.6 MB of linear memory free).
  - Expected: most of the ~23% of core 0 the path costs, so the later stretch could gain ~20%+.
- **Done 2026-10-20, as a native stand-in instead of the cache (`--shdwtri`, on by default; MEASUREMENTS.md "The
  triangle path as a native stand-in"):** later stretch +2.0-2.7% (~5% more pictures), heavy area +0.3%, Ordon +0.2%;
  exact against the interpreter. Far below the ~20% expected: the 22.4% "ShdwDraw's own blocks" included the replay's
  time, and the native path still costs ~5,300 cycles a triangle (the stand-in is 34% of core 0's cycles, with 39% of
  its I-cache misses). The callback natively too (`--shdwcb`) was neutral.
- **Re-evaluate when:** someone shrinks the stand-in's hot code (tri_run is inlined into hle_run: 8.9 KB, plus the
  7.9 KB walk and 3.5 KB replay against a 16 KB I-cache) or batches triangles: e.g. record each triangle's outputs in
  the walk memo's record (the path's results depend only on the collision data, so a replayed walk could replay its
  triangles' frame writes too, and the callback would run from a list).
- **2026-10-20, night: the fast form (`--trifast`, on):** both chains in registers, final words once: later stretch +3.8%
  (+7.7% pictures), gameplay +0.2-0.4%; the path 21% -> ~10% of core 0. The callback rewritten compact stayed neutral
  (in place ~1,600 cycles a call against ~300 warm), so what is left is how the stand-in runs in place (replay ~7%,
  path ~11%, callback ~7% in '# shdw where'), not the code's arithmetic.
- **2026-10-20, later: the callback read stage by stage (`--shdwcb`, now on):** later stretch +1.1% (+3.8% pictures),
  heavy area +0.22%, Ordon +0.7%. Reading everything first was the cost (~1,300 ARM instructions, ~43 micro-TLB misses
  a call), not the I-cache.


## C33. A second tier of function-scope regions (X4 oracle)

- **Status:** **built as the region tier (`--tier`, on since 2026-10-22: Wind Waker +7.2%, TP heavy -0.9%, Ordon +0.2%); what is left
  (leaves with branches, devirtualised bctrl, CR flags across labels) has no measured headroom in TP, the nursery was
  rejected 2026-10-24 (below).** Original plan: [X4 results](MEASUREMENTS.md#x4-hand-compiled-function-regions-on-arm11-2026-10-20),
  device jobs 745-747; retains C13's words-only and C14's duplicated-trace rejections as distinct experiments.
- **What:** function regions including loops, liveness allocation, dead CR fields kept in ARM flags and reached
  leaves inlined; current block JIT remains the fallback. X4 handwrote ARM, not a compiler.
- **Evidence (derived from measured PMU body costs after matching empty-entry controls):** pane 1.724 +- 0.027x,
  position matrix 2.507 +- 0.064x, integer texture setter 1.028 +- 0.043x. The hot pane clears the >=1.5x gate;
  integer SDK work does not improve uniformly. All three live-output comparisons and applicable plants pass.
- **Cost / limits:** event polls, faults, wider inputs, whole-game layout and inter-core pressure remain unpriced.
  The gain needs allocation/inlining/flags together; the preliminary spilled matrix leaf read only 1.255x.
  Pipe writes use a conservative callback lowering. Current JIT dumps and captured Ordon inputs stay on the SD.
- **Build decision:** supports a targeted tier as architecture section 6 item 4, behind a switch, then scored with
  separate whole gameplay runs. No default change follows from this microbenchmark.
- **Stage 1 built and measured, 2026-10-22 (`--region on`, off by default):** forward branches inside a block,
  registers forgotten at labels, no profile. Exact (fuzz, plants 20-22, qemu TP), and **slower in every scene: -2.3% to
  -4.4%** with I-cache misses +9-13% ([results](MEASUREMENTS.md#region-tier-stage-1-forward-branches-inside-a-block---region-on-opt_region-jobs-818-833-2026-10-22)):
  it compiles never-run branch sides inline in hot code. The next stage needs an edge profile from the block tier
  (compile only executed edges, cold sides out of line) and registers carried across labels; both, not either.
- **Stage 2 built and measured, 2026-10-22 (`--tier on`, off by default):** tiered regions over the code that ran,
  GPR slots carried across labels, inline joins, single entry, run-weighted coverage. Exact. Five rounds on the 2DS
  moved it from -1.3..-4.0% to **-0.6% / -1.6% / +0.3% / -0.1%** (heavy, Ordon, Wind Waker, Mario Party 4; jobs
  866-873). Regions run ~1.5% fewer ARM instructions; instruction-fetch stalls take it back
  ([results](MEASUREMENTS.md#region-tier-stage-2-tiered-regions-of-executed-code-registers-across-labels---tier-on-opt_tier-jobs-834-873-2026-10-22)).
  Float lanes across labels then (0e4fff4, jobs 882-893): -0.3% / -1.1% / +0.6% / +0.1%. Region plans with loops and
  inlined calls (b7eb9ac, jobs 936-947): **Wind Waker +7.2%** (C35's actor search in one region), heavy -0.9%, Ordon
  +0.2%, Mario Party 4 +0.2%. Across builds (948-1014): WW +7.4%, heavy -0.9%, Ordon -0.6..-1.4%, MP4 flat; the loss
  is instruction-fetch stall cycles (regions are 0.7x the code they replace, but the replaced blocks stay in place).
  Not micro-TLB (aligned buffer: misses /9, speed unchanged). Region zone and relayout (flush + recompile from
  remembered profiles) measured and not kept. Re-evaluate trigger: a way to reuse the replaced blocks' space without a
  flush, or a scene where the tier's stalls are not paid.
- **Nursery (2026-10-24, `--tier-nursery`, off):** pending blocks in a recycled area, decided code packed in the main
  area. Exact (fuzzer, qemu). Fetch stalls -4% (heavy 220 -> 212 M cycles/s), but every decided block compiled twice:
  compile time +0.7-0.9% of core 0, speed heavy -0.8%, Ordon -1.8% (jobs 1295-1298). Next form: move a decided block
  instead of compiling it again. **Moved (b9ba75c, jobs 1299-1314):** exact; steady state (from 100 s) heavy +0.2%, Ordon
  -1.9% (the walk keeps reaching new code; recycles retire live blocks). Rejected in both forms.
- **Re-evaluate when:** a prototype demonstrates coverage and exact event/fault semantics without code growth
  consuming the gain. The selected profile subset is only 4.84%; +15-30% core 0 is conditional on roughly 35-61%
  coverage at the derived 1.609x subset gain, not a measured gameplay result.


## C34. L2C-310 allocation partition by master (X2)

- **Status:** probe built, hardware mechanism confirmed 2026-10-20; experimental, default off. No production policy.
- **What:** preserve core-0 allocation freedom and restrict selector-1..3 D/I allocations to half the ways;
  probe through Luma's mirror, verify all readbacks and restore on every handled exit. Individual core1/core3
  selector wiring remains unverified. Hardware jobs require James home to restart.
- **Evidence:** job 748's measured samples give derived streaming chase 122.74 -> 32.97 -> 122.74 cycles/load
  (original / partition / restored). Eight separate runs 750-757 give only modest partition-alone changes and no
  positive vw2/steal result; [X2 details](MEASUREMENTS.md#x2-l2-lockdown-by-master-on-the-new-2ds-xl-2026-10-20).
- **Cost / limits:** kernel-adjacent system-wide registers, all streaming cores share the remaining ways, and code/
  data larger than the reserved share still compete. Bus pressure remains. A fatal kernel failure cannot run cleanup.
- **Build decision:** architecture 4.3.4's hardware precondition is open; a larger GX split still needs workload
  evidence. The tested partition, vw2 and stealing remain off. No additional optimisation is started.
- **Re-evaluate when:** a partitioned whole workload shows a repeatable gain with verified restoration and correct
  pacing, rather than assuming the isolated chase speedup applies to the emulator.

## C35. Wind Waker's actor search: devirtualised list walks (found 2026-10-22; built as region plans, b7eb9ac: +7.2%)

- **Status:** owner decision 2026-10-22: **the general form** (a JIT redesign that benefits every game: C33's region tier
  with speculative inlining of indirect-call targets), not an engine stand-in. Not built.
- **What:** Wind Waker spends ~21% of core 0 on Outset Island searching process lists:
  - cTgIt_JudgeFilter 10.9%;
  - cNdIt_Judge 7.2%;
  - fpcSch_JudgeByID 3.1% (job 792, `hot_funcs.py` with zeldaret/tww symbols).

  cNdIt_Judge walks a linked list and calls cTgIt_JudgeFilter through a function pointer for each node. That calls the
  filter's judge (fpcSch_JudgeByID: one ID compare) through a second pointer. So each node costs two indirect calls,
  two returns and a stack frame, ~30 guest instructions for one compare, repeated for many searches each frame. TP's
  profiles do not show it, though TP has the same f_pc framework.
- **Two forms:**
  1. **General:** a region tier (C33) that inlines an indirect-call target speculatively, guarded by a compare,
     turning the walk into a tight loop. It would help any game whose engine iterates with callbacks.
  2. **Engine-family stand-in:** at cNdIt_Judge's entry (recognised by bytes), when the judge is the recognised
     cTgIt_JudgeFilter and the filter's judge the recognised fpcSch_JudgeByID, walk the list natively; otherwise run
     the guest code. Like the ShdwDraw stand-in it replaces a whole loop, so the entry cost is paid once per search,
     not per node. But it is game-engine code (Wind Waker and TP's framework), the per-title kind of work the owner
     does not want the architecture to depend on.
- **Expected:** up to ~+15-20% core 0 in that Wind Waker scene (estimate); unmeasured elsewhere.
- **Prior art, found 2026-10-30:** the mechanism is standard outside emulation and measured. V8 replaces a Wasm
  `call_indirect` with a runtime check of the target against the assumed callee, inlining the body when it passes
  and deoptimising when it does not: **~1.19x from indirect-call inlining alone and ~1.59x combined with deopts**.
  Guarded inlining with a class or method test is long-established in JIT literature, and emulator-side there is a
  patent on polymorphic inline caches plus a branch-target buffer inside an emulator's dispatch. So the shape is not
  speculative.
  **But every one of those hosts has instruction-cache headroom, and this one does not.** Inlining a callee at each
  of its call sites multiplies its footprint, which is the thing that has lost here every time it was tried (traces
  -5.1%, relayout -7.6%, the region nursery, AOT). **So if this is built, it must not duplicate code:** inline the
  single hottest target once at the walk site that dominates the profile, not at every call site, and score it on
  hot words per guest instruction as well as on speed.
- **Re-evaluate when:** the owner picks a form, or the region tier (C33) is built. Take Wind Waker's profile again
  first - the ~21% figure is from job 792 and predates two weeks of changes, and `--tier` (which already inlines
  reached leaves) has been on by default since 2026-10-22 and may have taken part of it.

## C36. Translated display lists (G2): lists decoded once and replayed per state variant (2026-10-23)

- **Status:** **G2a built and measured** (`--g2 on|check|plant`, off; core/src/gx/g2.c, fifo.c `g2_replay`): exact on the
  PC and in the 2DS check mode (0 differed), the GX thread 1.3-2.5 points less busy in four scenes, speed unchanged
  (jobs 1147-1161, MEASUREMENTS.md "G2a"). G2b-G2e designed, not built (docs/research/G2_PLAN.md section 4 orders the
  next steps by the `ns_draw` phase profile, jobs 1162 / 1164).
- **What:** EMULATOR_CEILING.md G2. Each display list called from the FIFO is decoded once into ops and replayed while
  its bytes (page stamps, sampled hashes) and the state its decoding depends on hold; later stages keep the backend's
  per-run batch decisions with each state variant (G2b), compile TEV programs by structure with colours as parameters
  (G2c) and keep their raw state words (G2d).
- **For (G2-0 census, jobs 1142-1145):** every draw-carrying list repeats its structural read-before-write state from
  one picture to the next (99.9-100% of draws in TP heavy, Ordon, Mario Party 4, Wind Waker), with ~11-38 structural
  words to check a call. PC exactness of G2a: state hash and last frame equal to the decoder on three title spans,
  `G2=check` 0 differences in 12,099 compared replays, `G2=plant` differs.
- **Against / cost:** one entry per list does not work (last-use repeats only 51-98%: variants are needed); a
  whole-state signature does not either (0% in Mario Party 4); the projection, TEV colours, lights and matrices change
  every picture and must be applied live; list calls are only ~32-34% of TP's GX thread (X5), and TP's heavy area is
  core-0-bound today (core 0 waits on the GX thread ~0.5%, job 1124), so the GX saving reads as GX busy time before
  speed.
- **Re-evaluate when:** after G2a's device runs (whether decoding is worth replaying at all), and whenever the GX
  thread becomes the limit again (core 0 gains, Mario Party 4's minigame, the title cutscene).

> **That trigger fired, and it was right (2026-11-01).** "Whenever the GX thread becomes the limit again (core 0
> gains)" is exactly what happened - C53, the port set and the soft-float concat all made core 0 faster - and
> re-measured at the current default `--g2 on` reads **+0.53%** in TP's heavy area (on 7.247 / 7.246 against off
> 7.206 / 7.210, complete rank separation, on-arm spread 0.01%; jobs 1867-1870, MEASUREMENTS' G2a correction has
> the whole thing). The October reading of "no speed change" was taken with the pre-filter method and
> `--profile on`, so part of the difference is method rather than baseline, but the mechanism the trigger named is
> real: 1.3-2.5 points off GX busy had nowhere to go when core 0 waited on the GX thread 0.4-1.2% of the time.
> **This is the first time a "re-evaluate when" trigger on this list has paid out**, which is an argument for
> rule 4 that was not available before. **Status moves to: built, exact, off, +0.53% in TP's heavy area at the
> current default - a default candidate pending `--g2 check` on the current build and a figure from the owner's
> toolchain** (it is GX-side work, which a cloud session must not size). **Re-evaluate when:** that confirmation
> comes in, or core 0 gains again - the trigger has proven itself and stays.
>
> > **Amended the same day, by the breadth runs that should have come first: it reverses in static Ordon.**
> > Exactness re-confirmed on the current build (`--g2 check` 0 differed over ~5,400 replays a window across 13
> > windows, job 1883; `--g2 plant` 845-936, job 1884). Static Ordon, two runs an arm adjacent and reversed (jobs
> > 1885-1888): on **10.373 / 10.361** against off **10.399 / 10.394** = **-0.28%**, rank-separated the wrong way.
> > **Ordon replays 90.5% of list calls against the heavy area's 68.5% and loses by it** - more replay is not
> > better, because where the GX thread already has slack the decode saving has nowhere to go while the replay's
> > own work and cache footprint land on the core that is the limit. `--dlcache`'s lesson from the other side.
> > **Status: built, exact, off, scene-dependent (+0.53% heavy / -0.28% Ordon, net ~+0.1% across TP), not a
> > default.** The trigger still paid out - it correctly predicted that GX-side slack would become worth
> > something - it just did not predict that the scene with more slack would be the one that loses.
> > **Re-evaluate when:** the GX thread is the limit in a scene that matters (Mario Party 4's minigame, the title
> > cutscene), which is a narrower and more testable version of the old trigger.

## C37. Textures re-checked by page stamps instead of a once-a-frame re-hash (2026-10-24)

- **Status:** **built, measured as a gain** (`--texfast 2`; bit 1, a hint table in front of the texture table,
  measured even; bit 4 the check). **On by default since 2026-10-24 (b4436ae).**
- **What:** pica.c `cached_texture` re-hashed (sampled, at most 4,096 words) every texture's RAM once per emulated frame
  to catch the CPU changing it. With bit 2 the hash runs only for textures whose RAM pages were stamped (cache
  flushes, DMA, EFB copies, pipe bursts held in RAM) since it was last hashed.
- **For (jobs 1206-1228):** TP heavy +0.6-0.8%, Ordon +0.9%, Mario Party 4 +4.2%, Wind Waker +0.4% (separate whole
  runs); `--texfast 7` found 0 textures clean by stamps yet changed in ~5,800-11,000 re-hashes per 10 s per scene.
  The gain reaches core 0 although the hash ran on core 2: it streamed guest RAM through the shared L2 (as C2).
- **Against:** a game that writes texture memory with plain stores and no cache flush would keep a stale texture (the
  sampled hash may have caught it, on hardware the GPU would read whatever reached RAM). G1's converted shapes take the
  same rule for vertex data.
- **Re-evaluate when:** a game shows a stale texture with it on: then add a sampled verify (a few textures re-hashed a
  frame, round robin) as G1's ns_verify does.

## C38. EFB copies checked by page stamps, their RAM hashed only when reused (2026-10-24)

- **Status:** **built, measured as a gain, on by default since 2026-10-24** (5736668; `--copystamp on|off|check`).
- **What:** a GPU-side EFB copy hashed its destination's RAM when made, so that a lookup in a later frame could tell
  whether the CPU had overwritten it. With it the copy notes the page stamp after its own and hashes late: at its first
  reuse in a later frame with clean stamps; a copy stamped before any reuse is dropped.
- **For (jobs 1238-1268):** the copy-time hash cost 8-36 ms/s of the GX thread and was never used (0 later-frame
  lookups in TP heavy, Ordon, Wind Waker, the title); TP heavy +0.7%, Ordon +0.9%, Wind Waker +0.3%.
- **Against:** a copy reused across frames whose 4 KB pages are written by the CPU (flushed) before its first reuse is
  dropped where the hash might have kept it; the texture then comes from RAM, which a GPU-side copy never wrote.
- **Re-evaluate when:** a game reuses copies across frames (`# efb copy hashes 10s`: lookups in a later frame, dropped
  on a stamp).

## C39. Stamped converted shapes kept when their sources did not change (2026-10-24)

- **Status:** **built, measured as a gain in Mario Party 4, off** (`--nskeep on|check|plant|count`; pica.c `ns_keep`).
  Default is the owner's call.
- **What:** a converted shape (G1, pica.c `ns_get`) whose source pages were stamped since it was made was always made
  again, and after three times left on the old path as changing. With the switch, its sources are hashed over every
  byte (`ns_xhash`, 64-bit, every word and a range's last partial word) and the shape is kept, its stamp moved to now,
  when the hash matches the certified one. Certification happens only when a shape is made again with the same hash
  before and after converting (a hash taken only after converting could describe bytes written meanwhile).
  `ns_verify` hashes certified shapes the same way.
- **For (jobs 1242-1256, 1269-1274, MEASUREMENTS.md "--nskeep"):** 60% of Mario Party 4's stamps on shapes are false
  alarms (the game flushes unchanged data), 100% of TP heavy's few; Mario Party 4 **+1.9%** (two pairs, and against
  the base build bracketing it: 14.159 / 14.150% -> 14.428%, off 14.180%), conversions
  after stamps 5,976 -> 1,442 per 10 s; TP heavy, Ordon, Wind Waker even. Check 0 differed in 106,264 compared; the
  plant is caught (ns_verify and the next stamp).
- **Against:** each stamp on a kept shape costs a full hash of its sources (cheaper than converting it again, which
  reads the same bytes); a 64-bit hash collision on a real change would keep a stale shape (one word changed is always
  caught). CPU-deformed geometry (Mario Party 4's ~1,580 old-path draws a second) is untouched: that needs the vertex
  data skinned on the GPU or converted in place.
- **Re-evaluate when:** a game shows many `made again after a stamp` with `# nskeep 10s` "unchanged" high (the
  switch's case), or the GX thread limits a scene that converts after stamps.

## C57. The pipe path per byte: a run of bursts at once, then the ring copy (2026-11-01)

- **Status: part built and ON by default (`--gpruns`), part open.**
- **What the measurement found.** `port_cost.py` on a default heavy-area run (job 1878) puts **12.64% of core 0
  inside the ports**, with **~3.0% in `port_commit`** - the figure AGENTS has carried since job 1569 as "the
  technique is proven and the next place to apply it has to be found rather than assumed". It is found, and it is
  not a copy to remove from the commit: the pane walk commits 15,210 bytes in 345.98 us and `J3DSys::reinitGX`
  1,705 in 39.16 us, **44.0 and 43.5 bytes a microsecond** - two unrelated ports with different call rates and
  byte counts agreeing on the per-byte rate to 1%, which is what says the commit *is* `gp_fifo_write`.
- **Three independent routes to ~19 cycles a byte.** Today's profile timers give 18.3; C40's PMU split from a week
  earlier (jobs 1371-1374) gives **19.7** for the walk (43,800 cycles for 2,221 bytes) and **20.1** for reinitGX.
  C40 also supplies the attribution, which is the part that matters: **data stalls**, because the FIFO mirror is a
  large always-cold ring and every line is a write-allocate miss that evicts core 0's data too.
- **The traffic's shape, from the log's own counters.** 352,182,478 pipe bytes in 7,896,116 writes, 44.6 bytes a
  write - but the ports' 963 KB a second arrives in ~250 calls a second, so **46% of every pipe byte comes through
  0.5% of the calls**, and those runs were being chopped into **30,095 bursts a second** and reassembled into
  512-byte ring records by `ringbatch`.
- **Built: `--gpruns` (hw/gx.c `gp_run`), ON since 2026-11-01.** A qualifying run is moved at once - one mirror
  copy, one ring feed, the per-burst wrap arithmetic, CP-control read and counters done once. **TP heavy area
  +0.47%, static Ordon +0.43%** (jobs 1889-1897, rank-separated, spreads 0.01-0.11%), `--port check` differed 0,
  and **Wind Waker is a negative control that behaves like one**: 0 of 8.87 M bursts qualify there and the speed
  moves -0.01%. The mechanism is streaming, not arithmetic - one contiguous memcpy into the mirror beats 476
  interleaved 32-byte ones.
- **Still open, and in order.**
  1. **The ring copy, for a run.** `ringbatch` copies the bytes into the ring record, but for a run they are
     already in the mirrored FIFO in MEM1, so a record that *references* them needs no copy - which is what the
     hardware does. Cost: lifetime safety, since the GX thread must stay inside the FIFO's unread region. A design,
     not a tweak.
  2. **`--feeddirect` re-tested with `--gpruns` on.** It removes a *third* copy (the GX thread's pending buffer)
     and measured "even" when records were 512 bytes; `--gpruns` has just made them up to 15 KB, so the copy it
     removes is now one large memcpy per record instead of thirty small ones. A genuinely new condition, and
     exactly the "re-rank after every default lands" case. **But it is GX-thread work**, and `--g2` has just shown
     what GX-thread savings are worth when core 0 is the limit, so expect little.
  3. **C40 should be re-priced before anyone builds its "let core 2 write the mirror" design** (below): its
     measured upper bound (`--fifomirror off`, Ordon +1.6%, heavy +0.5%, jobs 1375-1380) was taken when every
     mirror write was a separate 32-byte store, and 37% of them are now contiguous.
- **What the run path cannot reach.** The other 63% of bursts are `gp_wgp_put`'s single-store path - the JIT
  assembling 32 bytes one guest store at a time - which cannot be batched without changing the JIT's store path.
  So 36.7% is close to this idea's ceiling.
- **Owed:** the PC state-hash check over 400 fields that `g_gpBulk` itself was validated with. The session that
  built this had no disc image, no devkitPPC and no built test DOLs, so the exactness evidence is `--port check`,
  the counter reconciliation, the negative control and the construction argument - not that check.
- **Re-evaluate when:** item 1 is designed, or C40 is re-priced.

## C40. The pipe's FIFO mirror in guest RAM without polluting core 0's caches (2026-10-24)

- **Status:** parked (measured upper bound). `--fifomirror off` (hw/gx.c `g_gpMirror`) is MEASUREMENT ONLY: not exact.
- **What:** every 32-byte pipe burst is copied into the CPU FIFO in guest RAM (`gp_burst`) as well as fed to the GX
  thread. The FIFO is a large ring, always cache-cold: each line is a write-allocate miss that evicts core 0's data.
  The ports pay it in their commits (PMU split, Ordon, jobs 1371-1374: the pane walk 43,800 cycles a commit for
  2,221 bytes, reinitGX 34,200 for 1,705, mostly data stalls), and so does every JIT pipe store.
- **Evidence (separate whole runs, from 100 s, jobs 1375-1380):** mirror off, Ordon 33.48 / 33.52 -> 34.08 / 34.03%
  (+1.6%), heavy area 23.55 -> 23.68% (+0.5%). That is the most an exact version could get.
- **Exact options and their risks:** writing the mirror through the uncached alias of the same physical memory (Luma
  maps physical memory at VA | 0x80000000) needs the range cleaned and invalidated in L1 / L2 whenever the FIFO is set
  and before any cached read of it (state save, a game reading its FIFO), or a stale dirty line evicted later would
  overwrite the bursts; deferring the copy to the GX thread breaks if the game writes or reuses that memory before
  the copy lands.
- **An exact way, proposed 2026-10-30: let core 2 write the mirror, since it already has the bytes.** `gp_burst`
  (hw/gx.c) hands every 32-byte burst to the GX thread (`gxAsync->feed`, batched by `ringbatch`) *and* memcpys the
  same 32 bytes into guest RAM on core 0. The ring record already holds the data, so the mirror write is a duplicate
  that core 0 pays for and core 2 - idle 12-13% in gameplay - could do for nothing.
  - **What makes it exact.** The only readers of FIFO memory are the GP (which is our GX thread, and has the bytes),
    the game itself through the FIFO's read pointer (it only reuses space the GP has consumed, so advancing the
    reported read pointer *after* the mirror write keeps reuse safe), a save state (flush the pending writes first),
    and a display list recorded through the pipe - the unlinked case in `gp_burst`, which keeps writing on core 0.
  - **The hole and how to close it.** A guest *read* of FIFO memory for any other reason would see stale bytes. The
    FIFO is a region nothing else touches, so marking its pages as not-fast (as MMIO pages are) makes any guest
    access there take a slow path that flushes the pending writes first - no cost on normal memory, since nothing
    else reads those pages.
  - **Worth:** the mirror is Ordon 1.6% and the heavy area 0.5% of core 0 measured directly (`--fifomirror off`,
    jobs 1375-1380), every game, plus whatever the write-allocate misses cost the shared L2.
> **Re-priced 2026-11-01 at the current defaults (`--gpruns` on), build `6db3d1d7c129`, 8.9 MB rung, two runs an
> arm adjacent and reversed in each scene:**
>
> | scene | mirror off | default | | the old figure |
> |---|---|---|---|---|
> | TP heavy area (jobs 1908-1911) | 7.292 / 7.291 | 7.220 / 7.224 | **+0.96%** | +0.5% |
> | static Ordon (jobs 1912-1915) | 10.551 / 10.552 | 10.423 / 10.422 | **+1.24%** | +1.6% |
>
> Complete rank separation in both, every spread 0.01% except one at 0.06% - the tightest pair of arms in this
> file.
> **I predicted both would shrink, because `--gpruns` had just made 37% of mirror writes contiguous. One did and
> one nearly doubled, so the prediction had no predictive value.** The explanation for the heavy area is that the
> old 0.5% was taken on 2026-10-24, before the stolen-second filter existed and several points of core-0 work ago,
> and **a fixed memory cost becomes a larger *share* as core 0 gets faster** - which outweighed the contiguity
> there and not in Ordon. The useful rule: "this will be worth less now that core 0 is faster" is not a safe
> inference in either direction, and only a run settles it. The two scenes are also *more* consistent than before
> (~1.0-1.2% against 0.5-1.6%), which is what a cleaner method should do.
> That leaves C40 the **largest single measured target in this area**, ~1% of core 0 in both TP scenes, for a
> mechanism that already has a written exact design (core 2 writes the mirror, below). `--fifomirror off` remains
> MEASUREMENT ONLY and not exact, so these are upper bounds on what an exact version could get, not promises.
- **2026-11-01: this entry's own PMU split turned out to be the key to C57, and C57 has now changed this entry's
  price.** The split (jobs 1371-1374: the pane walk 43,800 cycles a commit for 2,221 bytes, reinitGX 34,200 for
  1,705) is **19.7 and 20.1 cycles a byte**, which two later instruments reproduce as 18.3 - and the "mostly data
  stalls" attribution here is what identified the mirror rather than bookkeeping as the cost. `--gpruns` (C57, on
  by default) now makes **37% of mirror writes contiguous** instead of separate 32-byte stores, and is worth
  +0.47% / +0.43% on its own. **So the 1.6% / 0.5% upper bound above was measured in a world that no longer
  exists.** **That re-run is done and is the block above: TP's heavy area reads +0.96%, up from +0.5%, not down.**
  I had written here that the 1.6% / 0.5% bound was "now an over-estimate"; it was an under-estimate in the heavy
  area. **Re-evaluate when:** someone starts on the core-2 design - which is now the best-evidenced
  unbuilt idea on this list, at ~1% in both TP scenes with the mechanism named and the exactness argument written.
  - **Cost:** hw/gx.c, the GX thread's ring consumer, the save-state path and the JIT's page classification; a check
    mode that compares the mirror against the eager version over a run, with a plant.
- **Re-evaluate when:** the ports' commits or JIT pipe stores grow to a larger share of core 0, or an L2 / write-buffer
  measurement shows uncached bursts at well under a miss's cost.

## C41. A port's charge diverts the scripted walk; it does not make a scene cost more (2026-10-25, answered 2026-10-29)

- **Status:** answered (finding). Changes how every port and stand-in is judged: not on the scripted walk.
- **Answer (jobs 1480-1488, MEASUREMENTS.md "What the emulated clock is made of"):** in Ordon at defaults the
  emulated clock is **79% poll fill** (the game waiting), 13% instructions really run, 7.6% port charges, 0.2%
  stand-ins, with mean field slack 7.4 tenths and no field ever tight. A charge is therefore absorbed by a smaller
  fill: it cannot be read as speed and cannot push a picture past its deadline. The sweep's 30.89 / 32.64 / 27.90%
  came from the **scripted walk diverging**: `--press` is applied when the guest polls the pad, the charge moves the
  clock, the poll lands in a different field, and Link walks a different path. Three runs of one setting agree to a
  draw or two in draws a picture at matched emulated seconds; the arms differ 4 s into the run, and the 330 arm ends
  up parked in a static view ~40% heavier. The render structure per picture is identical (EFB copies, clears) while
  the geometry is not, so the arms drew **different scenes**.
- **Proof that the charge itself does nothing (jobs 1484-1487):** in the heavy area, taking the charge away
  (`--port-charge 1`) moves 12 points of the clock from ports to fills and leaves cycles a window, speed and frames
  alone (23.91 / 23.72 / 23.88% at off / 1 / 100); and flipping 100 against 330 inside one run (`--ab portcharge`,
  nothing recompiled) gives 9.84 against 9.92 frames a second. A 3.3x charge is worth nothing when the scene is held
  still.
- **What to do instead:** judge a port or any timeline-shifting change on a state that takes no input (the heavy area,
  tp_area2, no `--press`), or check that the arms drew the same scene (draws and vertices a picture at matched
  emulated seconds) before believing a walk. **The HUD text port (bit 10) is neutral, not a 4.3% loss** (heavy area
  -0.1% frames): it stays off for want of a gain, not because it costs. The multi-matrix port's Ordon -2.9% is being
  re-read the same way (job 1489).
- **Evidence:** the HUD text port (bit 10) is exact and removes ~3% of Ordon's core 0, yet Ordon reads -4.3%; with its
  cycle charge (kInsScale) at 100 / 165 / 330 Ordon reads 30.89 / 32.70 / 27.90% against 34.10% off, each repeatable
  to ~0.1% (MEASUREMENTS.md "HUD text as a port, and the ports' cycle charge"). Core 0's busy share is unchanged; the
  game does more work per frame. The multi-matrix load port's -2.9% fits the same pattern.
- **Hypothesis:** TP's time-based logic (timebase reads: animation steps, camera, audio frames per emulated second) does
  more per frame when a frame's emulated time shifts; a port's charge is an estimate of the guest instruction count,
  so every port moves the timeline a little, and some placements cross a threshold.
- **What the clock is made of (2026-10-29, read from the code; nothing measured yet).** `speed_pct` is
  `cycles / real seconds / 486 MHz` (main.c), and one guest instruction is one cycle, so every cycle the clock advances
  comes from one of four places: an instruction the JIT or the interpreter really ran, a charge for a native stand-in,
  a charge for a library port, or a **fill** — the interpreter and the dispatcher jump an idle loop (`b .`) or a
  polling loop straight to the next scheduled event (gekko.c, jit.c; the VI schedules one per line). That gives two
  regimes, and they predict opposite things:
  - **Slack** (the game reaches a wait before its deadline): the fill tops the clock up to the same event whatever the
    charges inside the frame were, so a charge is absorbed by a smaller fill and cannot move speed at all. Speed is
    then pictures a second, and a port gains exactly the real work it removes.
  - **Saturated** (no wait in the frame): a charge adds emulated time for nothing. Speed *rises* with the charge while
    the game gets less done per emulated second — its per-second logic, animation and audio run against a clock that
    advanced for free.
  Neither regime makes a bigger charge read *slower*, so the measured 330 -> 27.90% needs the game to do something
  different: either a frame that overran changes what it renders (it reads the timebase), or the picture is no longer
  locked to two fields and `speed_pct` and the real frame rate have parted company.
- **The charge is far too small to move the clock by accounting (2026-10-29, arithmetic from the numbers already
  measured).** A picture in Ordon is two fields, 16.2 M emulated cycles (`486e6 * 100 / 5994` a field). The text port
  runs ~3 calls a picture of ~10.8 k guest instructions each (300 calls per 10 s in check mode, jobs 1421-1422):
  ~32 k cycles, 0.2% of the picture's budget, and going from charge 100 to 330 moves the clock by ~45 k cycles a
  picture — **0.28%**, against a measured gap of 30.89 vs 27.90% (~10%, 35x larger). With the game locked to two
  fields a picture the clock per picture is fixed anyway, so the charge cannot be read directly as speed: it has to be
  changing what the game does, or how the picture is paced (two fields against three), which the `# new frames hist`
  field-gap histogram records in every run already taken.
- **Step 1 done (2026-10-29): it is not a measurement artifact — the game really is slower.** Re-scored jobs
  1423-1432 on the real frame rate (MEASUREMENTS.md table): port off 10.191 frames a second, charge 100 9.344
  (-8.3%), 165 9.770 (-4.1%), 330 8.375 (**-17.8%**), with speed / frames constant at 3.31-3.35 and the picture
  locked to two fields in every arm. So the charge does not merely move `speed_pct`'s clock: it costs the game up to
  18% of its pictures. Both of this candidate's own regime predictions are therefore wrong as stated — a charge of
  ~45 k cycles in a 16.2 M-cycle picture is changing something structural, not the bookkeeping.
- **Next, in this order:** (2) with the new instruments, one run each at `--port-charge 1 / 100 / 330` and the text
  port on (`bash core/tools/c41_jobs.sh queue`), read with `clock_split.py`: if the fills shrink by the charge and
  nothing else moves, the cost is in what the fill was doing; if the field slack collapses into bin 0, the picture is
  being pushed past its deadline after all. Charge ~0 is the case the old sweep never tried. (3) Then the game's own
  time-based logic (it reads the timebase ~1 k times a second): audio frames and actor executes per emulated second
  with the port on and off.
- **Instruments built for it (2026-10-29, not yet run on the device):** `# clock 10s` in perf.csv and the same line at
  the end of a PC run name the four clock sources and, per VI field, how much of the field was fill ("field slack",
  tenths; bin 0 = the game filled the field with work, so a charge that grows pushes the picture past its deadline).
  `--port-charge PCT` scales every port's charge at run time, with `--ab portcharge` flipping 100 against PCT in
  20 s phases; it recompiles nothing and changes no state, so both phases read from their first second.
- **Re-evaluate when:** a port or stand-in measures a loss its own cost does not explain.

## C42. Float results written straight into their lane register (OPT_FDIRECT, 2026-10-29)

- **Status:** **on by default since 2026-10-30** (Wind Waker +2.6%, Mario Party 4 +0.7%, TP flat): static Ordon +0.20% with
  the on arm noisier than that, the heavy area +0.03%; the device's hot-words metric does not move. Float
  arithmetic is too small a share of TP's compiled code. **(That "kept off" is a leftover from the
  pre-decision draft and is wrong: `main.c:648` has `o->fdirect = true` - the entry's own status line four
  lines up says "on by default since 2026-10-30". Caught by an audit, 2026-11-01. It matters because it made
  C43's trigger look unfired when it has fired twice - the A/B is in, and Wind Waker at +2.6% is the
  float-dense scene C43 asked for.)** The bench numbers below are for a scene
  where float work dominates.
- **What:** the float-lane path computed every result into a scratch single register and then moved it into frD's
  lane register - a word a lane on every float arithmetic instruction. With OPT_FDIRECT the arithmetic, or the
  rounding that ends the double path, writes the lane register itself, except where that register is a source the
  other lane still has to read.
- **Evidence (core/tools/jitbench.py, the device's opt mask, no disc image needed):** hot ARM words per guest
  instruction psloop 8.89 -> 8.00 (-10%), floatloop 6.46 -> 6.00 (-7%), intloop and callloop unchanged. Hot code size
  is the console's first-class cost (13 ns a linked block while the hot code fits in L1, 53-62 ns from 32 KB), and
  OPT_RETSTUB's 1.8% fewer static hot words bought +1-1.6%, so a few percent of the float code is worth an A/B.
- **Cost:** ~30 lines in compile_fc. Exact: the fuzzer passes with FCACHE|FDIRECT, and removing the aliasing guard
  fails 1 case in 4,000 (seed 1, mask 7|FDIRECT) where keeping it passes - the guard is exercised, not decoration.
- **Re-evaluate when:** the device A/B is run (gameplay states that take no input, per C41).

## C43. The second lane of a scalar single-precision result need not be materialised (2026-10-29)

- **Status:** parked (measured, not built). Found by reading the JIT's ARM output with OPT_FDIRECT on
  (core/tools/jitbench.py, floatloop.S, device mask): every scalar single-precision operation and every `lfs` writes
  both lanes of frD, as the Gekko does, and the second write is a bare `vmov.f32` of the first - 8 words of that
  loop's 80 hot words (10%), after FDIRECT had already taken 10%.
- **Idea:** a "lane 1 equals lane 0, with no register of its own" flag per FPR in the float cache. Only a
  paired-single read, a store of the pair or a write-back needs the value, and a write-back can write lane 0's
  register into both halves of guest state.
- **Cost and risk:** every reader of lane 1 has to honour the flag, and a reader that is handed lane 0's register
  must not write it - the same aliasing class that OPT_FDIRECT's guard exists for (removing that guard failed a
  fuzzer case, so this is not theoretical). Eviction of lane 0 has to materialise or write back both halves.
- **Worth it when:** float-dense code is a large enough share of the hot code. In TP's heavy area the clock is 79%
  poll fill and instructions really run are 21%, so this is a fraction of a fraction: measure OPT_FDIRECT on the
  device first (C42) and read how much it moved before building this.
- **Re-evaluate when:** OPT_FDIRECT's device A/B is in, or a scene turns out to be float-dense in its hot code.

## C44. Exit stubs in their own arena, so hot block bodies pack together (2026-10-30)

- **Status:** parked on arrival, with its ceiling already measured. Not built.
- **What it is:** a block's hot code is emitted as `[body][exit stubs]` in the hot arena (`cold_begin` / `cold_end`
  only separate the *cold* paths; HANDOFF 2026-10-12 confirms "stubs, hot paths, exit stubs" share the first 40% of
  the buffer). Exit stubs are **1.2 of every 3.6 hot-arena words per guest instruction** - a third of the arena -
  and once a site is linked the stub behind it is bypassed. Give them a third arena and the bodies pack contiguously.
- **Evidence it is dead weight:** job 1569 (TP heavy area, today's defaults) reports `disp_k` 8,010 dispatcher
  entries a second against 37.98 M guest instructions, i.e. **one return to the C dispatcher per 4,742 guest
  instructions**, or roughly one per 800 block executions at today's ~5.9-instruction blocks. An exit stub runs on
  the order of 0.1% of the times its block does. 33% of the hot arena is code that almost never executes, sitting
  between the bodies that always do.
- **Why it is parked rather than next:** the gain is bounded by what packing is worth, and that is measured. The
  nursery packed decided blocks in the main area and cut fetch stalls by only **4% (220 -> 212 M cycles/s = 1% of
  core 0)**, losing overall on its double compile (jobs 1295-1314, C-entry above). Job 1569 says why: 50% of time in
  compiled code is 54.8 KB over 84 pages, so the working set is 3.4x the 16 KB L1 *at any density*. Removing the
  stubs raises density from 16% to ~24% and cannot do better than the nursery's 1% of core 0, for a change that
  touches every exit, every link patch, every snapshot and the fault tables.
- **And do not expect a peephole to beat it:** the exchange rate says fewer hot words beats better-placed ones
  (OPT_RETSTUB: -2.1% hot words, +1.0-1.6% core 0), but the two large items in the executed-words table turn out to
  be worked ground - a `bc`'s 12.2 words are the block's shared `rc_flush` (the handover modelled at ~1.3% net), and
  the paired-single accesses already went through OPT_FCACHE and OPT_PIPEPS, with 72.8% of `psloop`'s stall cycles
  in the VFP itself. The reading is at the end of MEASUREMENTS' job-1569 section.
- **Re-evaluate when:** a change shrinks the 50%-of-time working set towards 16 KB (then density starts to decide), or
  the exit stubs can move without a second compile and without new link-patch bookkeeping - for instance if a future
  arena change is being made for another reason and this rides along.

## C45. Register-cache eviction by liveness lookahead instead of LRU (prior art: Mupen64Plus, 2026-10-30)

- **Status:** next, not built. The one footprint-*reducing* idea found by surveying other projects, and the only
  direction that has ever won on this hardware.
- **What:** `rc_alloc` (jit.c) picks its victim **least-recently-used** (`c->rc.use[k] < c->rc.use[best]`), which can
  evict a GPR the next instruction needs - paying a write-back now and a reload immediately after, two words of hot
  code each time. Mupen64Plus's new dynarec instead evicts by liveness: *"a ten-instruction lookahead is used to
  determine which registers will not be needed soon and these registers are evicted from the cache"*. With 5 slots
  (r4-r8) for 32 GPRs in ~5.9-instruction blocks, the victim choice is made constantly.
- **The prize, from this project's own numbers.** The regstat model (HANDOFF 2026-10-12) splits the register traffic
  per block run: loads 2.04 at first touch + **0.28 after evictions**; write-backs 1.54 at constant exits + 0.45 at
  blr/bctr + **0.49 at evictions**. So evictions cause **0.77 of 4.8 register memory operations a block run, 16% of
  the traffic**, and they are the only part a better victim choice can remove. Against ~14.2 hot words a block run
  and the measured exchange rate (OPT_RETSTUB: -2.1% hot words bought +1.0-1.6% of core 0):

  | evictions avoided | words saved a block | share of hot words | core 0 |
  |---|---|---|---|
  | 30% | 0.23 | 1.6% | +0.8 to +1.2% |
  | 50% | 0.39 | 2.7% | +1.3 to +2.1% |
  | 70% | 0.54 | 3.8% | +1.8 to +2.9% |

- **Why it is cheap to try:** the liveness data already exists and is already fuzzed - `gekko_regs(inst, &reads,
  &writes)` (gekko.h:156, `gekko_regs.c`), which `crf_use` already uses for the CR-field fate model. A lookahead over
  the next N decoded instructions of the block being compiled needs no new analysis and no new verification surface,
  and it is **compile-time only**: the generated code is the same shape, just with fewer spill/reload pairs. The
  regstat counters (`RW_EVICT`, `RL_EVICT`) already measure exactly what it is meant to reduce, so the model can
  score it under qemu before any console time.
- **Against:** blocks are short, so a 10-instruction lookahead often runs past the end of the block - at the exit
  everything live is written back anyway, which may leave less to win than the table suggests. The honest first step
  is to count, under qemu with `JIT_REGSTAT=1`, how many evictions had a victim that was dead for the rest of the
  block (those are the free ones) before writing any code generation.
- **Re-evaluate when:** that count is in. If dead-for-the-rest-of-the-block victims are a small share, the idea is
  bounded well below the table and should be parked.

## C46. A return stack buffer for blr, instead of the indirect table (prior art: emudev.org, 2026-10-30)

> **TRIGGER FIRED, and the entry is now the best-evidenced JIT candidate on this list (2026-11-01).** Its trigger
> was verbatim "a counter for indirect-table hits by kind (LR / CTR, first slot / second / miss) has run on the
> device". C48's `# jit indirect 10s` is exactly that counter and it ran (job 1916): **798,300 probes a second**,
> `blr` 5.02 per 1k against `bctr` 0.95, so **84% of probes are returns - about 670,000 a second**. The blocker this
> entry recorded ("the two available estimates disagree by eight times") is gone, and the low end that would have
> killed it is excluded: 670,000 a second is the *top* of the old range.
>
> **And the cost is now attached, which it never was.** Reconciling three counters already in the file:
> misses are **41,134 a second** (the dispatcher's own count) or 41,512 (5.2% of 798,300) - 0.9% apart - and C48's
> per-miss cost is on record at 480-820 cycles, the sampler's implied figure at ~1,600. At 804 MHz:
>
> | per-miss cost | share of core 0 |
> |---|---|
> | 480 cycles | **2.48%** |
> | 820 cycles | **4.23%** |
> | ~1,600 cycles (the sampler's implied figure) | **8.26%** |
>
> So indirect-table misses cost **2.5-4.2% of core 0** at the per-miss figure on record, which is this entry's
> size and stands on the miss count alone.
> **And the bucket it should be compared against is now clean, after a detour worth recording.** A code audit found
> that `WH_HLE` and `WH_INTERP` were declared in `common.h:51` and never assigned anywhere, so I wrote here that
> the `dispatch` bucket was contaminated by port and stand-in C code and that this entry's arithmetic could not be
> set against it. **That was wrong, for a reason only a run could show**: `jitsample.c:99` branched on
> `jit_in_run`, which stays 1 while a block calls into C, so port samples were counted as *compiled code* and
> never reached the bucket at all. With the sampler fixed (jobs 1926-1927) the ports appear as **`hle` 12.0-12.1%**
> - cross-checked against `port_cost.py`'s 10.46% plus ~1.14% of stand-ins on the same run, agreeing to ~0.5
> points - and `dispatch` reads **7.6-7.8%**, holding the dispatcher and little else.
> So this entry's sizing stands as first written: **2.5-4.2% of core 0 in indirect-table misses**, now against an
> uncontaminated bucket of 7.6-8.6% rather than a contaminated one. The claim went through three versions in an
> hour and only the measured one survived; **a code audit says what a program can attribute, never what it does.**
>
> **The trade to resolve, and it is arithmetic before it is a build.** ~8 instructions saved on each of ~670,000
> returns a second is ~5.4 M host instructions a second against core 0's ~217 M - about 2.5% - *plus* whatever
> share of those 41,134 misses an RSB converts into hits. Against it: **+3 words at every linked `bl` site**, in a
> project where every call-site growth has lost (OPT_CALLS -1.2%, traces -5.1%, relayout -7.6%) and where the
> governing rule is that misses follow the hot code's *footprint*, not words executed (C13's lesson).
> **Next step, no device time:** hand-emit the RSB sequence in `jitbench.py` on `core/tests/jit/bench/callmany.S`
> (the many-caller leaf: 19.2 probes per 1k with `--tier` on, 153.8 with it off) and read **hot ARM words per
> guest instruction** with and without it, plus `arm11_sched.py --cycles`. If words per guest instruction rise,
> close this entry on that number rather than spending a device session - that is the cheap half of the answer and
> it needs no disc image.
> **One caution carried over:** C48's two readings of the miss rate differ 26-fold (0.2% from the 2026-10-31
> instrument against 5.2% on the device), and the 480-820 cycle figure was derived from the 0.2% baseline. The
> table above uses it anyway because it is the only per-miss cost on record, but it is the number most in need of
> an independent second view before anything is built on it.

- **Status:** parked pending an instrument - **its value cannot currently be estimated, which is itself the finding.**
- **What:** `blr` goes through `ST_INDLR` into `ST_INDIRECT`: a 2-way address-indexed table probe (load LR, mask,
  index, `ldmia`, compare, budget check, `bx`), ~13 ARM instructions on a first-slot hit and ~19 on the second.
  The standard alternative (emudev.org, "Return Stack Buffer") pushes the guest return address and the host block
  address at a call and pops and compares them at the return, *"if they don't match, the entire buffer is cleared and
  the block falls back to hash table lookup"* - about 5 instructions on a hit.
- **Why it may well lose here, which is the interesting part:** the saving is on the *return*, but the push is new
  cost on every *call*, and in this JIT a linked `bl` costs **one** patched word (`exit_const`). So a pair goes from
  ~13+1 to ~5+4, and the call site grows fourfold in a build where instruction fetch is 27.4% of core 0 and where
  every call-site growth measured so far has lost: OPT_CALLS (following `bl` into the callee) **-1.2%**, traces
  **-5.1%**, hot-block relayout **-7.6%**.
- **Blocked on a measurement nobody can make today.** The rate of `blr` through the table is unknown: there is no
  counter for `ST_INDIRECT` traversals, `# other exits` records only the ~512 a second that reach the C dispatcher,
  and the two available estimates disagree by eight times (21% of exits being indirect implies ~1.35 M a second at
  6.4 M block runs; `bclr` at 3.30% of hot words and 7.5 words each implies ~167 k a second). At the low end the
  whole idea is worth under 0.5% and is not worth building.
- **Re-evaluate when:** a counter for indirect-table hits by kind (LR / CTR, first slot / second / miss) has run on
  the device. That counter is worth adding on its own: it also prices OPT_IND2 and the table's width.

## C47. Attribute core 0's data stalls - 22.1% of the core with no breakdown (2026-10-30)

- **Status:** next, and the owner's direction of 2026-10-30 ("the more things that are unknown to us right now we
  need to put measurements on"). This is an **instrument, not an optimisation**.
- **The unknown:** the fetch half of core 0 is attributed in detail - 50% of compiled-code time in 54.8 KB over 84
  pages, a per-block miss distribution, a blockmap. The data half has **only totals** in the whole record ("data
  stalls ~22%", job 184, 2026-10-15). Job 1569 adds that core 0 misses L2 **1.83 M times a second** and that ~81% of
  its data misses reach FCRAM, but nothing says *what is being touched*. That is the largest unexamined block in the
  budget - larger than any candidate below it.
- **The split that matters**, because only some of it is ours to fix:
  1. **guest RAM the game genuinely touches** - physics, nothing to do;
  2. **the Gekko state struct** (the register file at `fp`, FPRs, SPRs) - ours, and its layout is ours to choose;
  3. **JIT metadata** - the indirect table (C48), the block table, link tables, fault tables;
  4. **the GX ring and the FIFO mirror** (C40 already prices the mirror's duplicate copy).
  If 2-4 are a meaningful share they are addressable; if 1 dominates, the data half is a wall like the fetch half and
  should be recorded as such so nobody chases it.
- **The instrument already exists and the regions were the missing half.** `core/tools/qemu-plugins/dcache.c` is a
  qemu TCG plugin that simulates the ARM11's 16 KB 4-way L1 D and the 3DS's 2 MB 16-way L2 at 32-byte lines, counts
  misses per cache line, and `dcache_report.py` maps the lines back to regions from the emulator's own
  `JIT_RANGES=1` output. But that output named only **jit / mem1 / jitstate / gekko** - the two tables the
  dispatcher actually reads were not in it, so a miss in them could not be attributed and the biggest suspects were
  invisible. `fasttable` and `blocktable` are now printed too (2026-10-31), after the allocations rather than with
  the other ranges, since the allocations happen in between and emitting them earlier would have printed NULL and
  had the report attribute misses confidently to nothing.
- **And the static size alone is a finding, no device needed:** `FastEntry` is 8 bytes, `FAST_SIZE` is 65,536 and
  `FAST_TABLES` is 4, so the indirect-branch tables are **2 MB - exactly the size of the shared L2** that core 0
  divides with cores 2 and 3, probed at a pc-derived index on every `blr` / `bctr`. Whatever share of the data
  stalls that turns out to be, nothing about the number is reassuring (C48).
- **Cheapest measurement first:** the PC build already has a data-cache simulator (`dcache.c`, whose own header calls
  its L2 misses a lower bound). Tag each simulated access with which of the four regions it fell in and report the
  miss counts by region - no device time, no new hardware probe. Only if that says 2-4 matter is a device
  confirmation worth the PMU bracket (`--pmu-split` around the suspect path).
- **Why to expect something:** the model says the simulator sees only compiled code's data accesses on core 0 and
  models the L2 as LRU where the device's is not, so it is not the answer on its own - but it is enough to *rank* the
  four regions, which nothing does today.
- **Re-evaluate when:** the region split is in. It decides whether C48, C50 and any Gekko-state relayout are worth
  anything at all.

## C48. The indirect-branch table's cache and TLB cost (2026-10-30)

- **Status: ANSWERED on hardware, 2026-11-01 (job 1916, TP's heavy area, build `6db3d1d7c129`).** The counter had
  been built since 2026-10-31 and "waiting on a device build"; it ran, and both things this entry was opened to
  settle are settled.

> **`# jit indirect 10s`: 798,300 probes a second, 6.16 per 1,000 guest instructions - first slot 80.9%, second
> slot 14.0%, missed 5.2%.** Entered from `blr` 5.02 per 1k and `bctr` 0.95 per 1k, so **84% of probes are
> returns**. Hits that ran out of budget: 0.006 per 1k, i.e. nothing.
> **Cross-checked exactly as this entry designed it**, and on hardware rather than qemu: the dispatcher's own
> independent miss counter reads **41,134 a second** and 5.2% of 798,300 is **41,512** - **0.9% apart**. Two
> instruments, one quantity, agreeing (rule 8).
> **So OPT_IND2's +0.9% is explained, which this entry called out as adopted "with no idea why": the second slot
> serves 14.0% of probes, about 112,000 a second that would otherwise have missed.** A 94.9% hit rate on a
> pc-derived index is the answer to "is the table working".
> **The sweep is done too, and it says keep 16 - but the way it says so is the valuable part.**
> `FAST_BITS` is a compile-time `#define`, so the sweep is builds of one commit (`GC3DS_FAST_BITS=N` in
> `build.sh`, which tags the rev `+fbN` and uses its own object directory). Correcting one number first: the
> allocation is **`FAST_TABLES * FAST_SIZE * sizeof(FastEntry)` = 4 x 512 KB = 2 MB, the whole shared L2**, not
> the 512 KB this entry and AGENTS both said - 512 KB is one table of four.
>
> | `FAST_BITS` | probes/1k | first slot | second slot | missed | heavy area |
> |---|---|---|---|---|---|
> | 16 (512 KB/table) | 6.160 | 80.9% | 14.0% | **5.2%** | **7.352 / 7.341** |
> | 13 (64 KB/table) | 6.163 | 80.5% | 16.6% | **2.9%** | **7.295 / 7.294** |
>
> **A table eight times smaller misses 44% less** - at identical probe rates, so it is the same workload - **and
> is 0.71% slower.** Complete rank separation, spreads 0.15% and 0.01% (jobs 1922-1925, commit `8c39063a4703`,
> both arms on the 7.1 MB rung). So the table's size is **not** a cache-footprint problem, the miss rate is not
> what costs, and 16 stays. The likely reason a smaller table misses less is the **247 table resets a second**
> (job 1878): 65,536 entries cannot warm up between resets where 8,192 can - which makes the miss rate a measure
> of warm-up, not of capacity, and explains why improving it bought nothing.
>
> > **The methodological result, which matters more than the sweep.** The naive comparison - fb13 against the
> > unpadded default, same commit, two runs an arm with 0.04% spreads and complete rank separation - read
> > **+0.96%**. The real figure is **-0.71%**. The whole 1.7-point error was the **launch rung**: FAST_BITS 13
> > allocates 1.75 MB less, so it is a different heap, and it landed on rung 8.8 while 16 landed on 8.9, each
> > *reproducibly across two runs*. `runs.py` refused the comparison and was right to.
> > **So a change that alters allocation sizes moves the rung, and the rung is then not separable by re-running** -
> > it is a property of the build. The fix used here is to **pad the allocation**: the indirect tables are now
> > malloc'd at the FAST_BITS 16 size whatever FAST_BITS is, so a sweep changes only the footprint the dispatcher
> > *touches*. With that, both arms land on 7.1 and the comparison is clean. **Any future A/B of an allocation size
> > needs the same treatment**, and without it a rung can turn a 0.7% regression into a 1% gain - which is exactly
> > how this one would have shipped.
> **One number this did *not* explain, and it is worth flagging rather than quietly dropping.** The sampler's
> `dispatch` bucket reads **8.6-8.7% of core 0** in the settled windows (jobs 1878 and 1908, gpruns off and on),
> against 43,352 block runs a second - about **1,600 cycles per dispatcher entry**, which is not credible for a
> hash lookup and a block-table probe. So that bucket contains more than the dispatcher loop; the obvious
> candidate is the JIT's C helpers, which run with `g_where` left at `WH_DISPATCH` from the previous dispatch.
> **That is a hypothesis, not a finding** - `--gpruns` removed pipe-helper work and moved the bucket only
> 8.7% -> 8.6%, which neither confirms nor refutes it at this resolution. Whoever picks it up should set `g_where`
> in the helpers and see where the 8.6% goes, because an 8.6% bucket with unestablished contents is the largest
> unattributed number in core 0's budget.

- **The instrument** (`jit.c`, `ind_emit_count`): five counting sites in the **shared stub only**, so hot code does
  not grow - `blr` entries, `bctr` entries, a hit in the first slot, a hit in the second, a miss, and a hit that ran
  out of budget. Four instructions a probe, using r0 and r12, which `ST_INDIRECT` clobbers unconditionally on entry
  and so are provably dead at every site. Reported as `# jit indirect 10s` **per 1,000 guest instructions**: the
  counting slows core 0, so a rate per real second would be depressed by the instrument itself. Cross-checked under
  qemu against the dispatcher's own independent miss counter - `callloop.S` gives 128 probes, all misses, all from
  `blr`, against the dispatcher's "indirect-table misses 128". One trap found while building it: the stubs are
  emitted once at `jit_create`, *before* the front end calls `jit_set_opt`, so the first version of this counted
  **zero** with the option on; `jit_set_opt` now re-emits them when the bit flips.
- **First readings under qemu, and they span five orders of magnitude** (`jitbench.py` cases, 486 M guest
  instructions each, probes per 1,000 guest instructions):

  | case | `--tier` | probes per 1k | first slot | second slot | missed |
  |---|---|---|---|---|---|
  | `callloop.S` (one leaf, called twice from one block) | on | **0.00026** | - | - | 128 (all) |
  | `callmany.S` (one leaf, eight call sites) | on | **19.2** | 99.995% | **0** | 512 |
  | `callmany.S` | **off** | **153.8** | 99.99999% | **0** | 8 |

  Three things fall out, and the first two were not known:

  - **The region tier is an indirect-branch-elimination pass, which nobody had attributed to it.** With eight call
    sites a `blr` probes once per 52 guest instructions with `--tier` off (153.8 per 1k = exactly the 8 returns in
    the case's 51-instruction iteration) and **once per iteration** with it on (19.2 per 1k): the region inlines
    seven of the eight returns away, **-87.5% of probes**. `--tier` is on by default (Wind Waker +7.2%, TP heavy
    -0.9%, jobs of 2026-10-22) and part of that gain may be this rather than the inlining itself. It also means the
    device's probe rate depends on how much of the hot code gets regionized, which varies by scene.
  - **The second slot was used exactly zero times in every case.** OPT_IND2 was adopted on ~+0.9% with no idea why;
    this is the first direct evidence about the mechanism, and in these shapes it is *not* second-slot hits. Not
    conclusive for the game (a shape with real collisions would differ), but it moves the burden of proof.
  - **The probe rate is not guessable**, which is why the two prior estimates differed eightfold: it swings from
    0.00026 to 153.8 per 1k between two call shapes that both look like SDK code. Where a leaf has one caller the
    return is *linked* and the 2 MB of tables is never touched at all.
  A synthetic loop is not the game's call graph, so these set the range and the mechanism; the device decides the
  value. If it comes out near the bottom of this range the tables are nearly cold and shrinking `FAST_BITS` is close
  to free; near the top they are a live cache cost. **Either answer kills or builds this candidate, which is the
  point.**
- **The unknown:** `FAST_BITS` is **16**, so each indirect table is 65,536 entries of 8 bytes = **512 KB**, and
  `FAST_TABLES` is 4 (one per recent block key), so the allocation is **2 MB - the size of the entire shared L2**,
  spanning 512 pages. `blr` / `bctr` probe it at a pc-derived index - effectively a random offset in a structure 32x
  the 16 KB L1 D-cache and a quarter of the shared 2 MB L2. Every indirect branch is therefore a likely L1 D miss, a
  plausible L2 miss and a plausible data micro-TLB miss (C50). **None of this has ever been measured**, and the table
  was sized without a measurement of the trade.
- **Measurements, cheapest first:**
  1. **A counter, which is wanted for its own sake:** indirect-table probes by kind - LR vs CTR, first slot, second
     slot (OPT_IND2), miss. There is none today, which is why C46 cannot even be estimated and why the two available
     guesses at the `blr` rate differ by **eight times** (~1.35 M/s from "21% of exits are indirect" against ~167 k/s
     from `bclr` at 3.30% of hot words).
  2. **Sweep `FAST_BITS`** (16 -> 14 -> 12, i.e. 512 / 128 / 32 KB) as separate whole runs. A smaller table means
     more collisions and more dispatcher returns but a far better cache and TLB footprint; nobody knows which wins.
     OPT_IND2 exists precisely to absorb collisions, so the two should be swept together.
- **Expected:** unknown by construction, which is the point. The upper bound is whatever share of the 22.1% data
  stalls C47 attributes to JIT metadata.
- **ANSWERED AND CLOSED (2026-10-31, jobs 1733, 1741-1749). The table is correctly sized; shrinking it loses.**
  The probe counter ran first: TP's heavy area does **6.46 probes per 1,000 guest instructions, ~800,000 a second**,
  91.0% hitting the first slot, **8.8% the second** (so OPT_IND2 earns its ~+0.9% - the qemu shapes that showed
  *zero* second-slot use were unrepresentative, and recording that as "moves the burden of proof" rather than
  settled is the only reason it did not become a false conclusion), and **0.2% missing**. That closed the eightfold
  disagreement between the two guesses on record (~1.35 M/s against ~167 k/s): the truth is 0.8 M/s.

  Then the `FAST_BITS` sweep, three arms from one commit with only `-DFAST_BITS` differing, **interleaved**
  16/14/12/12/14/16 so session drift could not land on one arm:

  | `FAST_BITS` | tables | frames a second | spread | against 16 |
  |---|---|---|---|---|
  | **16** (the default) | 2,048 KB | 7.296 / 7.296 | 0.00% | - |
  | 14 | 512 KB | 7.261 / 7.251 | 0.14% | **-0.55%** |
  | 12 | 128 KB | 7.034 / 7.039 | 0.07% | **-3.56%** |

  Monotonic, and both losses are far outside the heavy area's ~0.2% floor. **The footprint theory is wrong:** 2 MB
  spanning 512 pages probed 800,000 times a second looked like it had to be costing cache, and the arithmetic said
  ~2.9% of core 0 if every probe stayed in L2 - but whatever it costs, collisions cost more. At 0.2% misses the
  table is doing its job, and the headroom to trade hit rate for footprint does not exist.
- **The mechanism, from the counters in the same arms** (`--indstat`, jobs 1747-1749), which is why the sweep lost:

  | `FAST_BITS` | first slot | second slot | **missed** | frames a second |
  |---|---|---|---|---|
  | 16 | 91.8% | 8.0% | **0.2%** | 7.296 |
  | 14 | 83.9% | 14.8% | **1.3%** | -0.55% |
  | 12 | 80.1% | 15.0% | **4.9%** | -3.56% |

  The probe *rate* is unchanged (6.47-6.48 per 1,000 guest instructions in all three), which is the check that the
  sweep changed only what it meant to: the rate is a property of the guest's call graph, not of the table. Misses go
  up 6x then 25x, and OPT_IND2's second slot takes up some of the slack (8.0 -> 14.8 -> 15.0%) exactly as it was
  meant to - which is a second, independent sign that OPT_IND2 does real work. **Note also that probes per *second*
  fell at 12 bits (840 k -> 745 k) purely because the emulator got slower**: reporting this counter per 1,000 guest
  instructions rather than per second is what makes the three arms comparable at all.
- **A number worth keeping, derived from the two loss arms: a dispatcher return costs roughly 480-820 cycles.**
  At 14 bits, 9,236 extra misses a second cost 0.55% of core 0 (~479 cycles each); at 12 bits, 34,996 extra cost
  3.56% (~818 cycles each). The two do not agree, so the cost is not purely per-miss - the larger arm also probes
  the second slot more and disturbs the caches differently - but **the order of magnitude is now measured rather
  than assumed, and it prices any future change that adds or removes dispatcher returns.** For scale, the project's
  normal rate is one dispatcher entry per 4,742 guest instructions.
- **What this rules out, so nobody re-runs it:** shrinking the indirect tables, in any direction. `FAST_BITS` stays
  16. C46 and the rest of the table's cache cost are closed with it - the sweep bounds them from above, because if
  the tables were costing several percent in cache, the 16x smaller one would have recovered some of it and it lost
  3.56% instead.
- **Re-evaluate when:** never, unless the probe rate changes by an order of magnitude (it is reported by
  `--indstat`, so a future scene can be checked cheaply) or the dispatcher's miss handling gets much cheaper, which
  would change the collision side of the trade rather than the footprint side.

## C49. Why switch effects are sub-additive - it affects every default decision (2026-10-30)

- **FIRST CLEAN DATA POINT, and the pair ADDS (2026-11-01, jobs 1832-1839).** All three configurations of C53 and
  `--port-softfloat` measured on **one build** (`787ed3743e11`), **one launch rung** (9.0 MB, C55), arms adjacent
  and the order reversed, in TP's heavy area:

  | configuration | runs | mean |
  |---|---|---|
  | both off | 7.068 / 7.071 / 7.075 / 7.076 | **7.0725** |
  | C53 on, soft float off | 7.211 / 7.213 | **7.2120** |
  | both on | 7.221 / 7.229 | **7.2250** |

  C53 alone **+1.97%**, soft float alone at the post-C53 baseline **+0.18%**, the pair **+2.16%**.
  **Composition predicts 1.0197 x 1.0018 = +2.15% and the pair delivers +2.16% - they add to within 0.01 points.**
- **And the apparent sub-additivity was a cross-build artifact, which is probably this entry's real subject.** The
  first attempt at this test (jobs 1828-1831) took the middle configuration from an *older* build and read the pair
  as +2.19% against +2.51% predicted - "short by 0.34 points". That shortfall is **exactly** C53's own figure moving
  between builds: +2.31% on `8aa6dc7`, **+1.97%** on `787ed374`, same scene and same rung. Each arm moved 0.43-0.57%
  across the rebuild, which is larger than the effect being hunted.
  **So the question this entry asks has to be asked of parts measured in the configuration they are being summed
  in.** The recorded "+1.04% predicted, +0.36% delivered" of 2026-10-30 was assembled from parts measured at
  different times; until it is re-run with every configuration on one build and one rung, it is not evidence of
  sub-additivity - it may be the same artifact. **That is now the cheapest thing this entry needs**, and it would
  partly dissolve it.
- **The caveat on the new point:** one pair, one scene, one rung. Notably these two options touch the *same*
  subsystem (the pane walk - soft float removes a give-up inside it), so if any pair should interact it is this one,
  and it does not measurably. That is evidence against a general sub-additivity rule, not proof of its absence.
- **Status:** parked on a measurement; **not a speed idea, a validity one.** The first clean point says the effect
  may be smaller than recorded, or absent for options that do not share a cache budget.
- **The unknown:** the four defaults of 2026-10-30, measured one at a time and then together on one build with two
  runs an arm, do not add up. Static Ordon: the parts predict **+1.04%**, the set delivers **+0.36%** (short by
  0.68 points). TP heavy area: the parts predict **-1.26%**, the set delivers **-0.17%** (short by 1.09). The ports
  are the opposite - superadditive as a set, +10.6% against ~3.7% for the parts, which is understood (the set keeps
  guest code out of the compiled footprint).
- **A second mechanism, found 2026-10-31 and much more mundane than the cache: one option can subsume another's
  work.** The J3D light port (bit 4) is credited +2.7% in TP's heavy area, measured with hooks 39 against 55 -
  before `J3DMaterial::diff` (bit 6) existed. Bit 6 does the whole material diff *including* the light block, so with
  it on the light hook now runs **84 times in a 240 s run**, and the counters show why: it fires **exactly** as many
  times as bit 6 declines (84/84, 84/84, 84/84, 84/84, 85/85 across jobs 1700, 1701, 1692, 1693, 1702). Its credit
  is *inside* bit 6's +3.0%. No cache argument is needed - the work simply moved. **So before quoting any entry in
  the ledger, check that its option still runs at the current default** (`port_cost.py` prints call counts). This is
  cheap to check and it has now caught two entries this way (`hlegd`'s stand-in group ran 0 times; this one runs
  0.35 times a second).
- **Why it matters more than its size:** every default in this project is decided by measuring one switch against a
  baseline. If effects routinely do not compose, then a stack of individually-measured gains is not the sum of its
  parts and the ledger overstates the whole. It would also explain why the headline numbers have moved far less over
  twenty days than the ledger's entries suggest they should.
- **The hypothesis to test first:** shared resources that saturate - all four switches move hot-code footprint and
  I-cache pressure, which is a *shared* budget, so two changes that each free cache lines cannot both be paid for
  the same lines. If so, the right unit of measurement is not the switch but the footprint, and `hot words per guest
  instruction` should be recorded next to frames a second for every default (the exchange rate -2.1% words ->
  +1.0-1.6% core 0 already exists to convert between them).
- **Measurement:** for one pair that is known sub-additive, record hot words per guest instruction and I-cache misses
  for each arm and for both together. If the footprint saving of the pair is less than the sum of the singles, the
  hypothesis holds and the ledger needs a note on every stacked claim.
- **ANSWERED 2026-10-30, and it is worse than the hypothesis: the ranking of one switch reverses depending on the
  other.** Four arms on `tp_area2`, build `dbac5779b30d8a92`, two runs an arm, within-arm spreads 0.04-0.10%:

  | arm | frames/s | vs A | I-cache misses |
  |---|---|---|---|
  | **A** `--port-hooks 243`, psflush off | 7.2140 | - | 8.215 M/s |
  | **B** `--port-hooks 243`, **psflush on** | **7.2755** | **+0.85%** | **8.152 M/s (-0.77%)** |
  | **C** `--port-hooks 503`, psflush off | 7.2405 | +0.37% | 8.255 M/s (+0.49%) |
  | **D** `--port-hooks 503`, psflush on | 7.2450 | +0.43% | 8.251 M/s (+0.44%) |

  Additively D should be **+1.22%**; it measures **+0.43%** - sub-additive by 0.79 points, and **D is 0.42% worse
  than B alone**, so adding a second measured gain *lost* speed.
- **The mechanism, visible in the counters:** the two switches pay in different currencies. `--psflush` buys
  **-0.77% I-cache misses** and returns +0.85% speed, almost exactly 1:1 - its gain *is* the cache. The extra ports
  in 503 **add 0.49% more misses** and are still +0.37% faster, because they do less work than the guest code they
  replace and pay part of it back in fetch. Put together, the ports' fetch cost **cancels the stand-in's cache
  saving** (D's misses are C's, 8.251 against 8.255) and only the work saving survives. Two changes cannot both be
  paid out of the same cache lines.
- **So one switch's best setting depends on another's:** with psflush **off**, `--port-hooks 503` beats 243 by
  **+0.37%**; with psflush **on**, 503 *loses* to 243 by **-0.42%**. The ranking reverses. Every "measure one switch
  against the baseline" result in this project is therefore conditional on the rest of the baseline, and a default
  decided while another default was wrong may be the wrong default now.
- **What to do about it, applied immediately:** re-measure any port or stand-in decision at the *current* default
  rather than reusing a figure taken at an older one - `--port-hooks 1779` (bits 9+10) read +0.64% against the 243
  default with psflush off and is being re-run with psflush on (jobs 1667-1668) before anything is banked, and Wind
  Waker's +3.2% for 503 is being re-checked the same way (1669-1672). Record **I-cache misses beside frames a
  second** for every such decision, since that is what says whether two changes can coexist; `# jit words per guest
  instruction` now prints three decimals for the same reason (one could not resolve 0.8%).
- **It also explains the twenty-day puzzle:** the ledger's entries sum to far more than the headline numbers have
  moved. Where entries share the I-cache budget they cannot be added, and some of them cancel. The ledger is not
  wrong about each measurement; it is wrong to read it as a total.
- **Re-evaluate when:** more pairs are measured. Worth re-reading every "as a set" claim against this, and the
  per-switch defaults chosen before 2026-10-30 in particular.

## C50. The data micro-TLB: 3.09 M misses a second, never examined (2026-10-30)

> **ANSWERED 2026-11-01, from logs already on disk and with no device time.** This entry's own stated measurement
> was "sweep `FAST_BITS` with C48 and watch `dmtlb_k`". That sweep ran (C48), and neither write-up read the
> counter. Reading it out of jobs 1922-1925 - the padded pair, so allocation and rung are held constant and only
> the *touched* footprint changes - settles it:
>
> | | `dmtlb_k` | `dcrmiss_k` | `icmiss_k` | speed |
> |---|---|---|---|---|
> | `FAST_BITS` 16 (512 KB a table, 2 MB in all) | 3,367 | 1,438 | 7,668 | - |
> | `FAST_BITS` 13 (64 KB a table) | 3,088 | 1,541 | 7,902 | **-0.71%** |
> | | **-8.3%** | **+7.2%** | **+3.1%** | |
>
> **So the answer is yes-and-no, and the no is the useful half.** The indirect table *is* a measurable share of the
> micro-TLB - shrinking the project's single largest JIT data structure eight-fold removes **8.3%** of the 3.09 M
> misses a second - but that is all it is worth, and taking it makes **data read misses rise 7.2% and instruction
> misses 3.1%**, which is why the net is -0.71%. **The micro-TLB is therefore not a handle on JIT metadata**: the
> biggest piece of metadata there is accounts for under a tenth of it, and the remaining ~92% is guest memory and
> Gekko state, which is C47's territory and not this entry's.
> **Status: answered, closed as a lever.** It cost nothing because the data was already on disk - the general
> lesson being that a sweep should read every counter its own entry names, not just the one it was run for.
> **Re-evaluate when:** C47 attributes the other ~92%, or someone proposes a change that moves page *count* rather
> than footprint (large pages are unavailable on this kernel, so that is unlikely).

- **Status:** parked; cheap to size, and AGENTS already lists "whether the micro-TLB can be made to matter" as an
  open hardware question.
- **The unknown:** job 1569 measures **dmtlb 3.09 M misses a second** against imtlb 0.22 M and main-TLB 0.19 M - so
  **5.9% of core 0's 52.06 M data reads a second miss the data micro-TLB**, fourteen times the instruction side's
  rate. The ARM1176's data micro-TLB is small (ten entries), and the JIT touches guest RAM (24 MB), the Gekko state,
  the code buffer and a 512 KB indirect table (C48) in the same breath, which is more live pages than it can hold.
  At a few cycles a miss this is of order **1% of core 0** - small, but it is a *measured* 1% that nobody has looked
  at, and it points at the same page-footprint problem as C48.
- **Measurement:** the PMU counter is already there (`dmtlb_k`), so the question is only what moves it. Sweep
  `FAST_BITS` with C48 and watch `dmtlb_k`; separately, check whether the Gekko state and the hot JIT tables share
  pages or scatter across them.
- **Against:** a few cycles a miss is the optimistic reading; if the micro-TLB miss is absorbed by the main TLB in
  the shadow of another stall it may cost nothing at all. Size it before building anything.
- **Re-evaluate when:** C47 says whether JIT metadata is a real share of the data stalls.

## C51. Spill guest GPRs to VFP registers instead of memory (2026-10-30, novel for this host)

- **Status:** parked, **model first** - and there is a specific reason to expect it to fail, which is why it is
  written down rather than built.
- **What:** the register cache has **5 host registers (r4-r8) for 32 guest GPRs** - worse than any comparable JIT
  (wasm-dolphin keeps all 32 in locals; Dolphin's x86-64 has 16 host registers), and the eviction and
  block-boundary traffic that follows is 4.8 memory operations a block run. ARMv6K has no more core registers, but
  VFPv2 has **32 single-precision registers**, of which the float lanes use s16-s31 (OPT_FCACHE). Whichever of
  s0-s15 the float path does not need could hold spilled GPRs: `vmov` is a register-to-register move with no address
  calculation, no store buffer and no cache line.
- **Why to doubt it, from this project's own model:** the arm11_sched stall breakdown already attributes **8.5% of
  stall cycles to ARM->VFP transfers** and 27.0% to the VFP itself. ARM11's VFP is a coprocessor and a core-to-VFP
  move has real latency, while a spill to memory goes into the write buffer and usually reloads from L1 at 4 cycles.
  So the trade may be strictly worse, and the honest expectation is that it loses.
- **Measurement, no device needed:** `core/tools/jitbench.py` and `arm11_sched.py` already model exactly this - hand
  a guest loop that spills (more than five live GPRs) through the JIT with and without a VFP spill slot and read the
  modelled stall cycles and host words. That is an afternoon with no console time, and it either kills the idea
  cheaply or justifies the first real register-allocator change in the project.
- **First measurements, 2026-10-31 (`core/tools/gpr_traffic.py`, new): the ceiling is 6-9% of hot words in the
  shapes that matter, and two thirds to four fifths of it is *stores*.** Four `jitbench.py` shapes through the real
  JIT at the device's own opt mask, every hot host instruction of every executed block classified by what it touches
  and weighted by execution count:

  | shape | guest-GPR spill/fill, share of executed hot words | of that, stores | distinct GPRs through memory a block |
  |---|---|---|---|
  | `floatloop.S` | **1.00%** | 0% | 1.0 |
  | `psloop.S` | **1.04%** | 0% | 1.0 |
  | `intloop.S` | **6.05%** | **79.7%** | 4.01 |
  | `callmany.S` | **8.95%** | **64.7%** | 3.67 |

  Three readings, and the second is the one that matters:

  - **The ceiling is modest.** Even if a VFP spill were free, 6-9% of hot words is all it could remove in integer
    and call-heavy code, and ~1% in float-heavy code (where OPT_FCACHE already holds the floats). At the measured
    exchange rate (OPT_RETSTUB: -2.1% of hot words bought +1.0-1.6% of core 0) the whole of it is worth a few
    percent at the absolute most, before paying for the transfers.
  - **It is mostly write-back, not demand fills.** 80% stores in the integer shape and 65% in the call shape. A
    fill-heavy profile would mean capacity pressure, which a bigger or cheaper cache fixes; a store-heavy one means
    the traffic is the register cache **flushing architectural state at block boundaries and calls** - and a
    VFP-backed cache must still do that, because the interpreter, the helpers and every exit read `Gekko.gpr` from
    memory. **So C51 as written attacks the wrong half.** The lever on write-back is fewer boundaries (block
    linking, handover - modelled at ~1.3% net, C44), not a different spill destination.
  - **The latency argument survives contact with the numbers.** ARM->VFP is 4 cycles and VFP->ARM 2 against an L1
    load's 3 (+1 when the consumer needs it early) and a store that goes to the write buffer. A VFP spill/fill is
    **not** cheaper per access, so any gain would have to come from D-cache *pressure* rather than latency - i.e.
    only if those accesses are causing misses, which is **C47's** question and is unmeasured. **That orders the two:
    C47 before C51.**
- **Honest limits of this.** These are hand-written loops, not the game's instruction mix, and the "distinct GPRs a
  block" column mixes two causes that this tool cannot yet separate - capacity overflow (a block wanting more than
  the cache's 5) and boundary flush (writing back registers the cache *did* hold). The store share argues for the
  second, but separating them properly needs the prologue/regstat information in the dump or a counter in `jit.c`,
  and it is the distinction that would actually decide this candidate. A game dump also needs one qemu run with a
  disc image, which a cloud session does not have.
- **Re-evaluate when:** the two causes above are separated, or C47 attributes a real share of core 0's data misses
  to Gekko-state accesses. Absent either, the expectation stays "it loses", now for a measured reason rather than a
  suspected one. If it loses, record the numbers so nobody revisits it - that is the point
  of this file.

## C52. The pacer overshoots its target, and the overshoot grows with it (2026-10-30)

- **Status:** open, small, and in the feature currently in flight (C9b).
- **The unknown:** `--gfskip-target` lands Ordon at 51 / 63 / 75% of full speed for targets of 50 / 60 / 70 - always
  *fast*, by +1.4%, +4.2% then +6.7% relative. Running fast means it skipped more than it needed and gave up picture
  rate for nothing: at target 70 it shows 5.00 pictures a second when a correctly calibrated controller would show
  more.
- **The hypothesis:** the solver takes `x = (target - cs) / (cd - cs)` from running means of the drawn and skipped
  frame costs, and a **systematically high `cs`** produces exactly this signature - too little drawing, and the
  error growing as the target approaches `cs`. A drawn frame's cost may also be bimodal (guarded draws during a fade
  cost differently), which a single running mean cannot represent.
- **The arithmetic discriminates the two candidates, and it picks `cs` (2026-10-31).** Which cost is wrong decides
  the *direction* the overshoot moves as the target rises, so the device's three points identify it. With Ordon's
  measured costs (cd 95 ms, cs 33 ms):

  | target | if `cd` is 10% too high | if `cs` is 5 ms too high | **measured** |
  |---|---|---|---|
  | 50 | +7.2% (53.6% of full) | +3.9% (51.9%) | 51% |
  | 60 | +5.7% (63.4%) | +6.6% (64.0%) | 63% |
  | 70 | +4.3% (73.0%) | +9.6% (76.7%) | 75% |

  A high `cd` makes the overshoot **shrink** with the target; a high `cs` makes it **grow**. The device grows
  (+2, +5, +7), so **`cs` is the one that is wrong**, and by roughly 5 ms on 33 - about 15%.

- **And the mechanism is almost certainly the defect fixed for C9c, in its mild form.** That bug seeded `cs` from a
  *single* warm-up sample, and every contaminated sample on record was **too high** (149.5, 187.2, 173.6 ms against
  a true 33 ms), because what pollutes the skipped-frame slot is work that belongs to a drawn frame. Severe
  contamination latched the solver (C9c); mild contamination leaves `cs` 10-20% high and produces exactly this
  signature. The fix therefore predicts the overshoot shrinks or disappears: **three warm-up samples with 1/n
  weighting, impossible samples rejected, and the probe re-sampling `cs`.**
- **What would falsify that:** a run of the fixed build that still overshoots by +5 to +7% at target 70 while
  `# gfskip target 10s` reports a `cs` close to the per-frame cost actually seen in a no-skip run of the same
  scene. That would mean the skipped-frame cost is genuinely state-dependent (a skip after a draw costing more than
  a skip after a skip), which one running mean cannot represent, and the solver would need two.
- **Measurement:** the self-test already drives the real controller with a synthetic clock, so the solver is
  verifiable without hardware - but note that a *constant* synthetic cost cannot reproduce this at all (the maths is
  exact there, which is why the existing case passes). A case with a state-dependent cost would test the
  falsification above. On the device: `# gfskip target 10s` against a no-skip run of the same scene.
- **Re-evaluate when:** the fixed build has run `--gfskip-target 50/60/70` on Ordon again (C9c). The prediction is
  recorded before the measurement on purpose.
