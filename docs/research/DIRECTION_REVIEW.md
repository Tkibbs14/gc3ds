# Direction review: what limits gc3ds, and the best path from here (2026-10-20)

An independent assessment written at the owner's request before any further implementation. It reads the repository's
measurements (MEASUREMENTS.md, HANDOFF.md, NATIVE_PORT_PLAN.md, AOT_PLAN.md, REDESIGN_CANDIDATES.md, PERF_LEVERS.md,
LIBRARY_PORTS.md, GAMES.md, OPT_JIT_PLAN.md, REGION_JIT_PLAN.md) and today's device runs, plus OUTSIDE_RESEARCH.md.
Every number is labelled **measured** (with its job), **derived** (arithmetic on measured numbers) or **estimate**.

> **Correction (same day, EMULATOR_CEILING.md):** core 0 costs ~22 cycles per guest instruction at today's defaults
> (Ordon 22.7, heavy area 21.7, cutscene 21.9), not ~35 (job 157, a 10-14 build). The gaps below are 2.7x (cutscene),
> 3.2x (Ordon) and 4.4x (heavy area); the emulator-only investigation re-derives the ceilings from there.

## 1. Where gc3ds stands, and what actually limits it

### The frame budget, by core (measured, 2026-10-20 defaults)

| Scene | Game frames / s | Core 0 (guest code) | GX thread, core 2 (working) | Vertex worker, core 3 | GPU |
|---|---|---|---|---|---|
| Ordon walk | 9.53 (jobs 701-702) | ~93-96% running | ~73-80% working, ~10% waiting on the worker, ~12-16% idle (joint states, job 702) | ~32% | ~8 ms a frame (job 370) |
| Heavy Ordon area | 6.84 (699-700) | ~97% | ~83%, 8% waiting on the worker, 13% idle (jobs 561-563) | ~29% | ~14 ms a frame (job 424) |

Derived, per game frame at the target of 30 frames a second (33 ms): the heavy area spends ~142 ms of core 0 and ~121 ms
of the GX thread a frame; Ordon ~100 ms and ~78 ms. **Both cores are 3-4x over budget at once.** The GPU and the
vertex worker are not.

**Consequence one: there are two walls of nearly equal height.** Core 0 is the limit, but the GX thread is only ~13-16%
behind it. Any core-0 improvement shows up only until the GX thread saturates: **at most ~+15-19% in gameplay** (derived
from the idle shares). This is why today's core-0 work (fast triangle path +3.8% in the title) moved gameplay only
+0.2-0.7%, and why ports that shave a few percent of core 0 can never be the whole answer.

### Core 0: why the JIT costs what it does (measured)

- **~35 host cycles per guest instruction** in the title cutscene (job 157): 11.3 executed ARM instructions, IPC 0.32,
  fetch stalls 21-24%, data stalls 22-23%, ~21% unattributed (VFP dependent latency and taken-branch bubbles, job 183).
  Full speed there needs ~103.7 M guest instructions per emulated second against 22.9 M now: **4.5x** (job 157/184).
- **The hot code is far larger than the cache:** 59.5k hot JIT lines (1.86 MB) touched in steady state, 24k lines
  (756 KB) holding 95% of the misses, against a 16 KB L1 I-cache and a 2 MB L2 shared with the GX thread and the GPU
  (offline I-cache simulation matched to the device). Twilight Princess's top 500 functions cover only 95% of its
  instructions (PC histogram): the footprint is a property of the game's code, not of the translator.
- **Every change that grew distinct hot code lost**, even with fewer executed instructions: traces -5.8%, relayout -4.6%,
  ahead-of-time C translation -31 to -36% (6 MB of translated code; I-misses 6.7 -> 11.5 M/s, job 144).
- **The remaining single JIT levers are small** (~1% each), except **fault mode** (fault-backpatched memory): +9.3% core 0
  in the cutscene, +4.1% later (job 168), parked because the 3DS kernel crashed after 685-6,166 faults in earlier runs.

**Physical limits** (not design choices): a single-issue in-order ARM11 at 804 MHz; VFP11 with 8-cycle add latency, no
NEON; Thumb-1 only (no Thumb-2 code compaction); a 3-entry return stack; one outstanding cache miss; FCRAM at 102-166
cycles; L2 shared with core 2 and the GPU (core 2 streaming raises core 0's 1 MB chase from 28 to ~120 cycles).
**Design limits** (changeable): CPU-side vertex gathering, re-decoding the same display lists every frame, one GX thread,
core 1 unused, and audio not yet producing samples (its guest cost, JAudio 6-8% of core 0, is already paid).

### The graphics side: a hard throughput cap (measured)

- **CPU-gathered vertices cap at ~300-340k a second in both execution modes**: the emulator's cutscene does 316-330k/s;
  native mode's title demo hit the same wall at ~50k a frame and 6-7 fps (jobs 184, 229-238).
- Twilight Princess draws ~24-26k vertices a field: **~750k a second at 30 fps (derived): 2.2-2.5x past the cap.**
- The PICA200 does ~20 M vertices/s with a short shader and ~180-300 ns per vertex with 1-3 lights (probe jobs 180-183).
  Once native mode converted shapes into GPU-resident buffers (job 263), the title demo rose from 5.6-7.5 to 9-12 fps
  and **the GPU became the limit** (~500 ms/s of GX-thread waiting on it; lighting at ~72 ns per light per vertex).

**Consequence two: per-frame CPU vertex work must go, in any execution model.** No CPU improvement, emulated or native,
reaches 30 fps while vertices are gathered on the CPU every frame; and moving them to the GPU moves the bill to the
vertex shader's lighting, which must then be kept small.

### Native code is several times cheaper than emulation (measured)

Native mode compiles Twilight Princess's decompilation for the 3DS (NATIVE_PORT_PLAN.md). Its game logic costs 14.8% of
a 33 ms frame in idle Hyrule Field (job 210); the heaviest scene, the title demo with GX encoding and no audio, 54-85%
of the frame at 30 fps (jobs 213, 217, 219). The emulator spends ~83 ms of core 0 per game frame in the same title
cutscene (derived: 37% speed, 93% running). **The game's own CPU work is ~4-5x cheaper natively** (derived, title demo):
the only measured CPU path with room for 30 fps.

## 2. The library-port hypothesis, tested against the numbers

### How much of core 0 is library code (measured)

- Device, gameplay families (job 370): GX SDK 23%, J3D 14%, J2D / JUT 13%: **~50%**; plus PS maths ~4%, GPR save /
  restore ~2.5%, JAudio 6-8% (job 289): **~60-65% of core 0 is shared library code**, ~35-40% is game code (actors,
  logic, the game's own rendering setup).
- Title (PC histogram, fields 900-1020): J3D 19.8%, JAS 15.6%, J3DGD 14.7%, GX SDK 9.4%, collision 8.6%, PSMTX 7.6%.

### Amdahl (derived)

Replacing library work of share *f* with native code *s* times faster gives core 0 a speedup of 1 / ((1 - f) + f / s):

| f (library share) | s = 2 | s = 4 | s = 10 | s = infinite |
|---|---|---|---|---|
| 0.50 | 1.33x | 1.60x | 1.82x | 2.00x |
| 0.60 | 1.43x | 1.82x | 2.17x | 2.50x |
| 0.65 | 1.48x | 1.95x | 2.41x | 2.86x |

Measured *s* for our ports: J2DMaterial::setGX ~15,700 cycles against the guest's ~1,920 instructions (~67,000 cycles at
35 cycles each): **~4x**; isolated small functions **below 1x** (cXyz operators -0.9%, GPR save / restore, GX setters:
the call boundary costs more than the function). So **subsystem ports at s ~ 4 on ~60% of core 0 give ~1.8x core 0 at
best**, against the 4.3x the heavy area needs. And by consequence one, that 1.8x only shows as ~+15-19% until the GX
side changes.

**Verdict on the hypothesis:** library-level replacement is real and general enough to be part of the foundation, but
by itself it cannot carry gc3ds to full speed in a demanding game. Its ceiling is set by the ~35-40% of core 0 that is
each game's own code, which stays at JIT speed, and by the GX thread. The distinction that matters:

- **Isolated function stand-ins** pay only for loops and heavy leaf functions (PS maths, THP, the shadow walk); for small
  functions they lose (measured four times).
- **Whole-subtree ports** (a material, a picture pane, a matrix load, the shadow triangle) reach ~4x and are verifiable
  call by call (`--port check`). Their cost is engineering per library version.

## 3. Execution models considered

| Model | Evidence | Verdict |
|---|---|---|
| Today's block JIT, more tuning | each item ~1%; fault mode +4-9% if its crash is understood | incremental: ~1.2-1.3x over months (estimate) |
| Region / trace compilation | modelled 0.6-1.3%; traces measured -5.8% (footprint) | stays rejected: no new evidence |
| Ahead-of-time PowerPC -> C | measured -31 to -36% (footprint) | stays rejected: the footprint is the guest's, a translator cannot shrink it |
| Interpreter + JIT tiers | dispatcher <1%, compile 0.2-0.7% | nothing to win: compile cost is not the problem |
| Library / subsystem HLE inside emulation | ~4x on what it covers; ~60% of core 0 coverable | **a real lever, ceiling ~1.8-2.4x core 0** (Amdahl) |
| Native execution of a decompiled game (native mode) | game CPU ~4-5x cheaper than emulation (measured) | **the only CPU path with room for 30 fps**; per game; needs a complete decomp |
| Hybrid: emulated game code + native engine (e.g. JSystem rendering natively, async) | not measured | possible middle ground; becomes a port of each engine version |

**Is a better approach than library ports available?** For demanding games, yes, but not as a single emulator-wide
technique: the measurements point to **a two-tier architecture**:

1. **A general emulator** whose GX side no longer does per-frame CPU vertex work (retained, GPU-resident geometry),
   whose cores are balanced, and whose CPU side gains from subsystem ports where libraries are shared. This is the path
   for most games, and lighter games may reach full speed on it (unmeasured: no other game has been run on the device).
2. **Native mode** for the flagship games with complete decompilations (Twilight Princess first), sharing gc3ds's
   platform layer and GX backend, delivered invisibly from the same disc image. This is a port, honestly named, but it
   reuses all of the emulator's graphics work, and it is the only measured route to 30 fps for Twilight Princess.

The two share the most important piece: the GX backend that turns GX streams into PICA200 work without per-frame CPU
vertex work. Investment there pays twice.

## 4. Five directions, ranked by expected impact and feasibility

### 1. Retained, GPU-resident geometry and a rebalanced GX pipeline (emulator and native)

- **Bottleneck:** the GX thread (~80-83% busy: decode ~19%, state ~21%, vertex prep ~18%, draw calls ~14%, job 512) and
  the CPU vertex cap (~300k/s).
- **Idea:** convert each display-list draw once into PICA vertex and index buffers in linear memory (native mode already
  does this: "converted shapes", job 263), keyed by the list's address, bytes and array bases, with page-dirty
  invalidation (DL_CACHE_PLAN's machinery). 99.6% of Twilight Princess's draws come from display lists and 79-85% repeat
  the same bytes and array bases (job 107). Per frame only matrices and state go to the GPU. Lighting goes to the vertex
  shader with only the enabled lights, or stays on the idle worker for lit models. Then split the GX thread's remaining
  stages across core 3 (idle ~70%).
- **Ceiling:** removes the worker's gather (~60% of it) and most of the GX thread's vertex prep and list decoding:
  GX thread work estimated **-40 to -60%**, which lifts the gameplay cap from ~+15% to ~+70-150% for core-0 work
  (estimate).
- **Realistic, alone:** +10-20% gameplay (core 0 then limits again), but it makes every later CPU gain visible.
- **Cache / memory:** buffers in linear memory (budget ~8 MB, as native mode); fewer CPU passes over FCRAM; the GPU's
  vertex fetch competes for the shared bus (measured: GPU texture reads slow core 0's misses).
- **Difficulty:** medium-high; the converted-shapes code exists in native mode. **Risk:** CPU skinning and vertex-colour
  animation (changed arrays); the GPU becoming the limit (native: 9-12 fps GPU-bound): lighting must be bounded.
- **General:** yes: display lists are GX, not game code. Games built on immediate-mode GX gain less.
- **Smallest experiment:** in the emulator, measure the GX-thread and worker time spent on draws whose list and arrays
  repeat (instrument the existing `# dlist` counters with ticks): that is the ceiling, in one run.
- **Strongest argument against:** C3 (a CPU display-list vertex cache) measured no gain, and native mode with converted
  shapes moved the wall to the GPU at 9-12 fps rather than reaching 30. If the GPU's lighting cost cannot be bounded, the
  gain stops at the GPU. (The difference from C3: C3's records lived in FCRAM, so a hit cost ~1.4 us a vertex, the same
  as a gather, job 119; this path has no per-vertex CPU work at all. C1's own re-evaluation condition, GPU time with
  headroom, now holds: 8-14 ms a frame in gameplay. Its listed blockers, per-draw palette slots and texgen, are what
  native mode's converted shapes already moved into the shader.)

### 2. Native mode for flagship games with complete decompilations (Twilight Princess first)

- **Bottleneck:** the JIT's ~35 cycles per guest instruction; the footprint that no translator can shrink.
- **Evidence:** the game's CPU work is ~4-5x cheaper natively (derived from jobs 210-219 against job 157); the title demo
  runs at 30 fps on the CPU side (54-85% busy) with audio off.
- **Ceiling:** Twilight Princess at 30 fps on core 0, if gameplay with movement, combat and audio fits (unmeasured).
- **Realistic:** 20-30 fps after direction 1 lands in its renderer (estimate; the renderer is the open question).
- **Difficulty:** high per game (Dusklight-scale code, heap limits, 3DS endian / platform fixes); the platform layer is
  shared. **Risk:** heap (the game's heaps plus a large binary), audio cost, and that each port is one game.
- **General:** no. Games with complete decomps today: Twilight Princess (GameCube matching), Pikmin, Melee (reported),
  with Wind Waker and Sunshine partial. It must never replace the emulator path; it is an optional accelerator chosen by
  disc ID and revision (the owner's rule: the disc image is all a player provides).
- **Smallest experiment:** native gameplay with scripted movement (the input path is the known blocker) and audio on:
  core-0 milliseconds a frame in Ordon.
- **Strongest argument against:** it turns the flagship into a port. It does nothing for the other twelve games without
  decomps, and it divides effort between two products. If direction 1 plus subsystem ports bring emulated Twilight
  Princess within reach, native mode is a large detour.

### 3. Subsystem ports of the shared JSystem / SDK libraries (with call-by-call verification)

- **Bottleneck:** ~60% of core 0 that is shared library code.
- **Order by measured share:** the J2D pane walk (25% of Ordon's core 0 inclusive; designed in LIBRARY_PORTS.md), J3D
  packet and material drawing (J3D + J3DGD ~15-35%), JAudio's sound engine (6-8% now, more once audio is real), then the
  matrix and animation paths (recursiveCalc ~10%).
- **Ceiling:** ~1.8-2.4x core 0 (Amdahl table above); realistic **+30-60% core 0** over several ports, visible only after
  direction 1.
- **Difficulty:** high per subsystem; each needs a version per JSystem generation (2002, 2004 SDKs; GAMES.md: five
  JSystem games, three SDK generations). **Verification** is a strength: the port check runs on the device.
- **Strongest argument against:** each port is a reimplementation of one library version, and the gains so far
  (+2-6% each in Ordon) fall short of their inclusive shares because the port still does most of the SDK's work. The
  maintenance cost grows with every game and version, and the game's own ~35-40% stays at JIT speed.

### 4. A multi-game measurement program (the guard against overfitting)

- **Gap:** **no game other than Twilight Princess has ever been measured on the device** (sweep of MEASUREMENTS.md).
  Every conclusion above may be specific to one 2004 JSystem game at 30 fps.
- **Plan:** a save state and a scored run for 4-6 games across SDK generations and engines (GAMES.md): Animal Crossing
  and Mario Party 4 (early SDK, lighter), Wind Waker (JSystem 2002), Metroid Prime (Retro engine, 60 fps), Melee (HSD,
  60 fps), Super Mario Sunshine. For each: speed, the joint states, the guest family profile, and whether stand-ins and
  ports are recognised. Then every change is scored on the set, not on one scene.
- **Ceiling:** none directly; it decides which of directions 1-3 generalise and whether some games are already close.
- **Smallest experiment:** boot one early-SDK game to gameplay and record `--profile on` plus the joint states.
- **Strongest argument against:** each game costs bring-up time (boot bugs, save states, input scripts) before it
  yields one number, and Twilight Princess is still far from playable.

### 5. Measured accuracy trades on the CPU, and opt-in presentation compromises

- **Fault mode's kernel crash, root-caused** (+9.3% / +4.1% core 0 measured): the largest single JIT lever on record.
- **Dead-state and timing relaxations** the owner already approved (CR liveness, dead write-backs): modelled ~0.3-1.5%.
- **Guarded render skipping as a player option** (the game's renderer every other frame while fades and copies are
  guarded): measured +50-60% game frames with glitches (C9, `--gfskip`); general only through a per-game renderer entry.
- **Ceiling:** ~1.1-1.15x core 0 for the CPU trades; presentation compromises up to ~1.5x frames, at a visible cost.
- **Strongest argument against:** the CPU trades are small; the presentation ones are exactly what the owner wants to
  avoid before 30 fps, and published emulators found no glitch-free way to skip guest render work (OUTSIDE_RESEARCH.md).

### Ideas examined and set aside (with reasons)

- **Region compilation, traces, AOT, a second vertex worker on core 1, GX-thread stealing, a bigger job ring:** measured
  losses or below 1.5%; nothing has changed the mechanisms (footprint, L2 coupling).
- **Emulated-CPU underclock / idle skipping:** Twilight Princess's core 0 never idles in gameplay; frame-locked work
  does not shrink with fewer cycles.
- **Single-precision float shortcuts:** measured +0.5% (`--fastfloat`, job 373) and -0.6% later; the cost is the core /
  VFP moves and latency, not precision.
- **Memoizing whole functions across frames:** probed; the inputs change every frame (HUD, J3D: jobs 454-456, 672-680).
- **Core 1:** usable up to ~80% (APT limit), but a memory-heavy job there evicted core 0's L2 (vw2 -7.4%). Light,
  cache-friendly work only (audio mixing, I/O), not vertex streams.

## 5. Near-term improvements and architectural breakthroughs

- **Near-term (weeks, a few percent each):** the J2D pane walk (+3-3.5% Ordon, designed), fault mode's root cause
  (+4-9% core 0), masked recognition of SDK stand-ins in other games (GAMES.md), JAudio offload once audio is real.
- **Architectural (months):** retained GPU-resident geometry with the GX pipeline split across cores 2-3 (direction 1);
  native mode for Twilight Princess on top of it (direction 2); J3D drawing as one subsystem port (direction 3).

## 6. Experiments, ordered for information

1. **GX ceiling instrument (one run, no new mechanism):** ticks on the GX thread and worker for draws whose list bytes
   and array bases repeat (`# dlist` already counts them). Decides how much direction 1 can lift the gameplay cap.
2. **A second game to gameplay** (an early-SDK game, then Wind Waker): speed, joint states, family profile. Decides
   whether the two-wall picture and the library shares generalise.
3. **The C1 prototype in the emulator:** reuse native mode's converted shapes for repeated display lists only, lighting
   on the worker; measure the GX thread, the worker and the GPU on Ordon and the heavy area. Kills or confirms
   direction 1.
4. **Native gameplay CPU budget:** native Twilight Princess in Ordon with scripted movement and audio, core-0 ms a frame.
   Decides direction 2 (go if comfortably under 33 ms, no-go if not).
5. **The J2D pane walk port:** the first subsystem-scale port, scored against its Amdahl prediction (+3-3.5% Ordon). If
   the measured gain falls far short, direction 3's ceiling is lower than the table says.
6. **Fault mode's kernel crash:** reproduce with a fault counter and the arm9 dumps; the largest measured JIT lever.
7. **Render-skip as a player option:** only if the owner wants a presentation compromise before 30 fps.

## 7. Ceilings (with the evidence behind them)

- **Twilight Princess, emulated:** with direction 1, the pane walk and J3D ports, fault mode and the JIT's remaining
  items, an **estimate of ~12-16 game frames a second in Ordon and ~9-12 in the heavy area** (today 9.5 / 6.8): core 0
  stays ~2.5-3x over budget because ~35-40% of its work is game code at JIT speed. **Full speed is not supported by
  the evidence** on this path.
- **Twilight Princess, native mode:** **30 fps is plausible but unproven**: the CPU side measured 54-85% of a frame in
  the heaviest title scene (audio off); the GX side needs direction 1 and a bounded lighting cost (it was GPU-bound at
  9-12 fps). Realistic estimate after both: 20-30 fps.
- **Other GameCube games:** unknown, because none has been measured. By reasoning from the same costs: games whose CPU
  demand per frame is well below Twilight Princess's (early, simpler 30 fps titles) could reach full speed on the
  emulator after direction 1; 60 fps games with heavy CPU work (Melee four-player, F-Zero GX, Metroid Prime) need twice
  the throughput and are unlikely to reach full speed by emulation (estimate). Measuring them (experiment 2) is the
  first step.

## 8. Can library and subsystem ports be a foundation for general emulation?

Partly. They are the right tool for the ~60% of a JSystem / SDK game's CPU time that sits in shared libraries, they
generalise per SDK generation and per JSystem version (not per game), and they can be verified call by call on the
device. They are not a foundation that carries a game to full speed: Amdahl caps them at ~2-2.5x core 0, the game's own
code is untouched, and their value is invisible until the GX side stops being the second wall. The foundation that
generalises best is the graphics pipeline (direction 1): it is library-agnostic, it serves the emulator and native mode
alike, and it removes a limit every game hits.
