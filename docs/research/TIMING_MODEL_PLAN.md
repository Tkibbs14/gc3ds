# 2DS timing model: predicting the 2DS's speed on the PC (planned 2026-10-15)

Owner direction (James, 2026-10-15): a standalone project, **after** native mode's feasibility build has been tested on
the 2DS (docs/research/NATIVE_PORT_PLAN.md). Goal: emulate every aspect of the hardware's speed that can be emulated,
so changes are judged on the PC before they reach the device, then measure each one against the 2DS.

## Why

- Correctness is checked on the PC (qemu, the JIT fuzzer, byte-identical frames); speed is not. qemu instruction counts
  have misled us: AOT (C12) looked ~17% heavier under qemu and ran ~30% slower on the 2DS; BEFLOAT and the second
  vertex worker lost on hardware for reasons qemu does not see (I-cache, shared L2).
- Every speed decision now waits on a 2DS run (~11 min, the device must be up, crash-risk runs only when James is home).

## What sets the 2DS's speed, and what is modelled today

| Mechanism | Today |
|---|---|
| ARM11 pipeline: single issue, in order, interlocks (load-use, multiply, VFP11) | partial: interlock scheduler model from the TRM's latencies, never calibrated |
| L1 instruction cache, 16 KB 4-way, 32 B lines (core 0's main stall) | yes: core/tools/qemu-plugins/icache.c, tracks the 2DS's misses fairly well |
| L1 data cache 16 KB, write buffer, micro / main TLB | no |
| Branch prediction (BTB, return stack) | no (mispredicts are small on the 2DS) |
| Shared 2 MB L2, FCRAM latency, contention between cores | no (what sank C2 and C15) |
| Other cores (GX thread, vertex worker), PICA200 GPU time, kernel / svc costs | no |

## Plan

1. **Calibrate against the 2DS's own counters.** Every relay run logs the PMU (cycles, instructions, I/D-cache misses,
   fetch and data stalls, branch misses, TLB misses; perf.csv `pmu_*`, `icmiss_k`, `stall_*`). Run the same stretch under
   qemu with the model and match counter by counter; a model is an instrument only once it matches the device.
2. **Core-0 model:** qemu instruction and memory trace -> I-cache, D-cache, TLB, L2 simulators + the pipeline interlock
   model + a branch predictor. Target: rank two variants as the 2DS does, within ~10% on absolute speed.
3. **The rest of the machine:** L2 sharing with cores 2-3, the GX side's own cost, PICA200 time, kernel overhead.
   Hardest part, weakest accuracy; full simulators (gem5) cannot run the 3DS OS and run ~1 M instructions/s.
4. Then measure **every** modelled mechanism against the 2DS and record which ones the model gets right.

## Limits

- A/B effects here are 1-5%, smaller than any model's error: the 2DS stays the final judge for every default.
- Native mode benefits most: once it runs, tuning is many small changes, which is where a calibrated model pays.
