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
- **2026-10-18:** the opposite direction measured better: lighting moved out of the shader onto the worker (the
  lighting shader was 26 of the heavy area's 43.6 ms of GPU a frame; jobs 411-416, +2.9%). More shader work is a
  GPU cost; the worker has headroom (~25-35% busy).

## C7. Queue counters on their own cache lines

- **Status:** built (pica.c: `g_vwHeadL` / `g_vwTailL`, one 32-byte line each, `VwLine`).
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
  job 156); both off. Candidates 3 (float loads and stores, ~1% of hot words) and 4 (register pinning) are parked:
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

- **Status:** rejected 2026-10-17 (jobs 321-326); exact on the PC.
- **What:** the CodeWarrior runtime's register save and restore (2.5% of the cutscene, job 311), entered at each
  `_savegpr_N`, as C called in place of `bl` (ps_translate.py `__save_gpr.dis@K`).
- **Result:** a small loss (job 322 with them: cutscene 28.29%, later 15.70%, against 28.98 / 16.35 without, job
  318). The call writes back every dirty cached GPR first, and after a restore the caller forgets them all: more than
  the 1-18 word moves saved.
- **Re-evaluate when:** the JIT can pass the cached registers to a stand-in (or inline the moves itself: these are
  plain stores and loads it could emit in place, without a call).

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
- **Status:** not built.
- **Re-evaluate when:** GPU-heavier scenes become the target, or the frame-begin wait grows past ~10% of the GX thread.

