# Optimizing JIT: design and stages

Status: **stage 1 in progress** (2026-10-13). The owner chose an optimizing JIT over game patches, more tuning or lighter
games. This file records what the measurements say, what is built in which order, and how each stage is judged.

## What the measurements say (read before building anything here)

- **TP is held by the GX thread, not by core 0.** perf.csv's `gxwait_pct` is the time core 0 waits for the GX thread
  (`gxthread.c` `wait_drained`, mostly at polling-loop skips through the idle hook; the sampler files it under
  "dispatch"). It is 9-16% in TP's heavy rows. While it is non-zero, a faster core 0 turns into more waiting, not into
  speed. 2DS job 88 (`--ab faultmem`): speed 19.53 / 19.44% (off / on), but gxwait 7.5 / 16.1%: **fault mode made core
  0 ~11% faster** (core 0 speed 21.2 -> 23.6%) and the speed column could not show it. The HANDOFF's earlier lesson
  ("removing ~7 executed instructions per memory access did not move speed, because core 0 is bound by stalls") was a
  misreading of the same data.
- **Judge JIT work by core 0's own speed**, `speed / (1 - gxwait)`, which `core/tools/perf_heavy.py` now prints, within one
  run (`--ab`). TP's speed itself will rise only to where the GX thread saturates (~21% now -> roughly 25-26%); the rest
  of a core-0 gain becomes headroom that pays off once core 2 is cheaper, and on scenes and games with less GX work.
- **Where core 0's modelled cycles go** (`tools/arm11_sched.py --cycles` over a `JIT_SCHEDDUMP` of TP's cutscene, 40
  fields, 5.9e7 guest instructions: 8.0 host instructions + 2.3 interlock cycles per guest instruction, before fetch
  stalls): memory instructions are ~60% of the model's cycles. lwz 13.6%, psq_l 8.0%, stw 7.4%, psq_st 6.1%, lfs 5.5%,
  bc 5.5%, stfs 4.4%, addi 3.3%, icbi 2.5%, bclr 2.4%, lhz 2.4%, rlwinm 2.3%. Per execution: lwz 10.5 cycles, stw 9.6,
  lfs 22.6, stfs 27.8, psq_l 35.8, psq_st 49.3.
- On the 2DS core 0 spends ~33 cycles per guest instruction while not waiting (~13 executed instructions, ~9.7
  instruction-fetch stall cycles, ~8 data stall cycles). Hot code size per guest instruction drives the fetch stalls;
  executed instructions and load-use chains drive the rest.
- Earlier region measurements (REGION_JIT_PLAN.md, HANDOFF 2026-10-12) still hold: with five cache slots, keeping
  registers across blocks saves ~1.3% and loop regions ~0.6%. The lever is not register allocation; it is what each
  memory instruction costs.

## Stage 1: memory checks hoisted to one per base register per block (check hoisting)

Every D-form load and store today checks its own address (MEM1, alignment) through a shared stub: lwz is `add`, `bl`,
five stub instructions, `bcs`, `rev`. Fault mode removed the checks entirely and gave core 0 ~11%, but the 3DS kernel
crashes delivering user data aborts now and then (HANDOFF 2026-10-13), so it stays off.

Check hoisting gets most of that without faults:

- The first access through base register rA in a block checks **rA itself** against a window inside MEM1,
  `[0x80008000, 0x817C8000)` (`sub`, `sub`, `cmp`, `bhs`: the window size 0x017C0000 is one ARM immediate). Every D-form
  displacement is within +-0x8000 and every access at most 8 bytes, so every later access through the same rA, while rA
  is not written, is inside MEM1 through the cached mapping.
- Later accesses through rA are unchecked: `add r0, rA, #d` (none when d = 0), `ldr rd, [RRAM, r0]`, `rev`. Floats call
  unchecked variants of the float stubs (no range or alignment checks; the GQR type check of psq stays). No slow path, no
  `bcs`.
- **Alignment does not matter:** the interpreter never raises the alignment exception (EXC_ALIGNMENT is never raised),
  and the ARM11 with SCTLR.U set (probed on the 2DS) and qemu load unaligned words correctly. The unchecked float stubs
  use single-word `ldr` / `str` instead of `ldm` / `stm`, which need word alignment.
- **A failed check deoptimizes the block:** it leaves compiled code before the access (guest state exact: the
  instruction has not run, the register cache is written back) through `ST_DEOPT` with the block's PC; the dispatcher
  remembers that PC (a small set that survives flushes), recompiles the block with per-access checks as today, and
  points the old code's first word at the new block, so linked predecessors and the indirect table follow. A block that
  touches MMIO (the gather pipe, hardware registers) or the top 224 KB of MEM1 through a register base deoptimizes once
  and then runs exactly as before.
- Which accesses are hoisted is decided while compiling (a mask of guest GPRs checked so far, cleared for every GPR an
  instruction writes, per `gekko_regs`, and entirely after an instruction it cannot analyse).
- A/B switch: `jit_set_opt(j, OPT_HOIST)`; 3DS `--opt` / `--ab opt`. Drops all compiled code.

How it is judged:

1. **Model first** (JIT_REGSTAT, `check hoisting model` line): the share of D-form accesses whose base was already
   checked in the block.
2. **Exactness:** the fuzzer with fault mode off (`JIT_FUZZ_NOFAULT=1`), default and chain modes, with bases near both
   window edges, MMIO and the mirror, and the same base reused within a case; a planted bug (a hoisted access whose base
   check is skipped) must be caught. TP under qemu with hoisting on and off: same instruction count and last frame.
3. **2DS:** `--ab opt` in one run, judged by core 0 speed (`perf_heavy.py`), with gxwait, executed instructions and
   I-cache misses next to it.

## Later stages (ordered by the model's cycles; each gets its own measurement before building)

- Float values: FPRs stay in VFP registers within a block (d8-d15), loads and stores of FPR lanes only at block ends
  and helpers; psq_l / lfs results used straight from registers (the 8-lane simulation said 1.6% at today's block sizes,
  so only together with something that makes float code live longer).
- CR updates only when read: needs cross-block liveness *and* agreement on exactness (an interrupt delivered at a block
  boundary saves CR in its context, so a skipped dead update changes memory the interpreter would have written).
  Not started; needs the owner's call on whether architecturally dead state may differ from the interpreter.
- Constant propagation within a block (lis / addi / ori chains): absolute addresses known at compile time skip the
  check altogether, and constants fold into immediates.

## 2026-10-15: what comes next, checked against the records above

The owner asked for the JIT redesign to be grounded in what is already known. Registers across blocks (region
compilation, handing state over at links, pinning, loop regions) stays ruled out: ~1.3% / 0.6% by the region model,
and nothing measured since contradicts it. The 2DS rewards less distinct hot code and fewer excursions into C
(HANDOFF 2026-10-14/15). Ordered by evidence:

1. **SETEND BE in the float load/store stubs (OPT_BEFLOAT).** FINDINGS.md run 3 measured the float penalty as the move
   between core and VFP registers (~3.5-4 cycles each way, not overlapping) and big-endian data mode as +12% (scalar
   float) / +23% (paired) on gcbench; the JIT never used it. Today every lfs / psq_l / stfs / psq_st stub goes ldr ->
   rev -> vmov (or back); in BE mode the guest value goes straight to or from a VFP register. Float loads / stores are
   27.6% of core 0's modelled cycles (PERF_LEVERS D2). Only the shared stubs change: no code growth. BE-8's 64-bit VLDR
   puts the lower-address word in the high half, which is right for lfd / stfd and wrong for two singles (two VLDR.32).
2. **C8, single-precision maths** (15.3% of modelled cycles), only where inputs are already singles in registers.
3. Constant addresses (lis / addi bases) without checks; fault mode only if its kernel crash is understood.

## Owner decision, 2026-10-15

James: "I'd like to try out and measure and root cause all [three]. I approve all of it as long as we can get real
gain from it." So:

1. **Event timing may drift from the interpreter** (interrupts and events delivered some instructions later, as in
   Dolphin) and **architecturally dead state may differ at exits** (dead GPR / CR write-backs dropped). Each change is
   still judged by a 2DS A/B and kept only with a real gain. Correctness then rests on the fuzzer (which compares after
   the same instruction count, so it still holds) and on visual checks, since the qemu same-frame test cannot hold
   once timing moves.
2. **Fault mode's kernel crash gets a root-cause investigation** before any retry.
3. **C1 (GPU-resident vertex data)** follows.
