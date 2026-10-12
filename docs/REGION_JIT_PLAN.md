# Region compilation: design brief for a dedicated session

Status: **open design, not started** (written 2026-10-12, after the measurements below). The owner will give this its
own session. The brief is deliberately open-ended: it records what is known, what constrains the design, the options
worth evaluating and how to judge them, and leaves the design itself to that session. The goal is the best speed on
the New 2DS XL, not a particular technique.

## Why: the measured cost

All numbers are Twilight Princess from `tp_arm_850.state`, 30 fields, traces and the shadow-scan stand-in off, build
3a1696f, unless marked as 2DS (relay jobs; see `docs/REMOTE_RUNS.md`).

- **Block-boundary register traffic is ~25% of what core 0 executes.** Every block loads the guest registers it uses
  into the five cache slots (r4-r8) and writes the dirty ones back at its exits: about **4.5 loads and stores per
  block run**, with blocks averaging **5.9 guest instructions** and **~6.4 hot ARM words per guest instruction**
  (`JIT_REGSTAT=1`, PC build). Per register per block run (loads / write-backs): CR 0.39 / 0.38, r3 0.28 / 0.35,
  r0 0.04 / 0.47, r4 0.15 / 0.25, r6 0.19 / 0.17, r1 0.22 / 0.13, r31 0.21 / 0.12, r30 0.16 / 0.10.
- **r0 is written back 0.47 times per block run but loaded only 0.04 times**: most write-backs are of values no later
  block reads (PowerPC r0 is a scratch register). Liveness across blocks would drop them without any allocation work.
- **Compiled code, not the emulator's C code, is the bottleneck** (2DS, `--pmu-split on`): compiled code and its
  helpers take 88-96% of core 0's cycles and essentially all instruction-fetch stalls and I-cache misses.
- **Core 0 waits on instruction fetch for ~33-36% of its cycles** (2DS pmu, ~8.2-8.8 M I-cache misses/s, IPC ~0.31).

## What has already been tried (do not repeat blindly)

- **Traces** (blocks follow `b` and continue past conditional branches as side exits; `--traces on`): cut executed
  ARM instructions 5% and mispredicts ~30%, but **did not reduce I-cache misses or improve speed**, because every
  side-exit target and return point starts its own trace and copies the code after it (tail duplication). Before the
  code-size work they also overflowed the 16 MB buffer and thrashed (1.3% speed). Lesson: **duplication grows the
  touched code footprint, and the footprint is what the I-cache pays for.** Any region design must bound duplication
  and be judged by I-cache misses, not only by instruction counts.
- **Pinning guest registers to host registers** across all code: measured and rejected. ARM has no spare registers,
  so a pinned guest register costs a dynamic cache slot: 4 slots instead of 5 raised in-block traffic by +0.47
  loads/stores per block run, while pinning CR would save 0.77 (net ~1.5%).
- **A float register cache** (FPR lanes in d8-d15): a simulated 8-lane cache over the same run would save ~1.6% of
  executed words (49.8% of `vldr` would hit, 13.5% of `vstr` are overwritten in-block). Not worth it alone; it may be
  worth folding into a region design if regions make float values live across more code.

## Hard constraints of the hardware and of the current JIT

- **ARM11 MPCore, ARMv6K, 804 MHz.** 16 KB 4-way I-cache, 32-byte lines, round-robin, virtually indexed; 128-entry
  BTAC indexed by VA[9:3]; 3-entry return stack; 2 MB L2 shared; mispredict 5-7 cycles (`docs/HANDOFF.md`, "ARM11
  fetch facts", from DDI 0360F). No MOVW/MOVT: a 32-bit constant costs up to 4 instructions.
- **Registers:** r0-r3, r12, lr scratch; r4-r8 the guest register cache (callee-saved, survive C helpers); r9 RDC
  (budget), r10 RRAM (RAM base), r11 RG (Gekko*). VFPv2: d0-d15, d8-d15 callee-saved.
- **Budget:** checked at block exits (`SUBS` + `BGT` link sites, `ST_INDIRECT` checks); a run executes at most its
  budget plus one block. Events and interrupts are only taken between runs.
- **Code buffer:** 16 MB on the 3DS (hot region 62.5%, cold 37.5%); full flush when either region fills; ~65% used
  after 4 minutes of the title. Region code must fit this or bring a better eviction policy.
- **Exactness:** compiled code must match the interpreter instruction for instruction (instruction counts, guest
  state at every exit, interrupt timing at run boundaries). Interpreter fallbacks, slow memory paths, exceptions
  (DSI), `mtmsr`, `icbi` (TP executes icbi ~1.4% of instructions) and the write-gather pipe all leave or call out of
  compiled code mid-block and need the guest state in memory or reconstructible.
- **Self-modifying code / icbi:** today any icbi on a page with compiled code flushes everything; a region design
  should keep that cheap or make invalidation finer.

## Options to evaluate (not a decision)

1. **Cross-block liveness only.** Analyse guest code ahead of a block's exits (the target block's guest instructions
   are in RAM) and skip write-backs of registers that are overwritten before being read on every path. Cheapest;
   attacks r0-style dead write-backs. Needs a fallback when the target is unknown (indirect branches) and care with
   exits into exceptions and the interpreter, which read guest state from memory.
2. **Register-state handoff on links.** When block A links to block B, B is compiled (or specialised) to expect A's
   cache state at entry, so registers stay in r4-r8 across the jump. Variants: one specialisation per predecessor
   state (bounded), or a canonical entry state per block chosen from profiles. Risk: duplication (see traces).
3. **Region formation from profiles.** Count block executions (the `JIT_REGSTAT` counters already do it on the PC; a
   cheap 3DS version is possible), pick hot regions (loops, hot call paths) and compile each as one unit with a
   global allocation over the region, side exits for the cold paths. Classic approach (Dynamo/HotSpot-style
   superblocks, LuaJIT trace trees, QEMU TB chaining with register passing); the region boundary policy decides the
   duplication.
4. **Loop-focused regions.** Detect loops (backward branches to an earlier block) and compile the loop body as one
   region with registers held across iterations; leave straight-line code as today. Smaller scope, likely good
   coverage for TP's math and collision loops.
5. **Combinations,** for example liveness (1) everywhere plus loop regions (4) where the profile says loops dominate.

Whatever the design, consider what else a region makes possible: CR liveness (compares whose result no branch reads),
keeping float values in d8-d15 across a loop, and hoisting the MEM1 range check out of loops.

## How to judge it

- **2DS speed is the score.** Use the relay (`bash core/tools/relay.sh run ...`): title from boot (heavy rows only:
  `draws_s > 1000`) and the `tp850` cutscene, before vs after, plus `--pmu-split on` and the pmu columns for I-cache
  misses, fetch stalls and executed instructions. Current baselines (2DS, 3a1696f not yet measured; 4c5012a):
  title heavy rows 16.62%, cutscene 15.89%; I-cache misses ~8.2-8.4 M/s.
- **Predict before building.** `JIT_REGSTAT=1` on the PC gives executed words per opcode and register traffic per
  block run; extend it to simulate a candidate (as the float-cache simulation did) and compare predicted savings
  across options before writing the real thing. Two designs were already ruled out this way in an hour each.
- **Exactness gates, every step:** `bash core/tests/jit/run_jit_fuzz.sh` (default and `int,long` modes, plus new
  planted bugs for any new write-back or handoff path; a crash must fail, which the script now enforces), the TP run
  from `tp_arm_850.state` with identical instruction count, dispatches and last frame, `bash core/tools/arm_jit_check.sh`
  (test DOL frames), and the on-device fuzzer (`--fuzz 40000 3 int,long` relay job). The fuzzer currently tests single
  blocks; a region design needs multi-block fuzz cases (chains of blocks with links, loops, side exits).
- **Footprint:** report hot code bytes and I-cache misses next to speed; reject designs whose instruction-count gains
  come from duplication that the I-cache then pays back.

## Open questions for the session

- Where does the 25% concentrate: a few hot loops (then option 4 captures most) or spread across many small blocks
  (then options 1-2)? Measure block execution distribution and loop coverage first.
- How much of the register traffic is at exits to known targets (linkable) vs indirect branches (`blr`, `bctr`) and
  exits to the dispatcher?
- Can liveness be computed cheaply and safely enough at compile time from guest code alone (with a conservative
  answer at unknown targets)?
- How should invalidation (icbi, code-buffer flush) treat regions: drop whole regions, or blocks inside them?
- Is a profile-guided recompile (tier 1 = today's blocks, tier 2 = regions for hot code) worth its complexity on a
  device with a 16 MB buffer and slow compile?
