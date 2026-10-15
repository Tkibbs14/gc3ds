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

## Coverage: every part of the 3DS that sets speed (owner direction, 2026-10-15)

James: complete coverage of the hardware, if at all possible; the tooling lives in the repo and is what tells us how
things will run before they reach the device. Each mechanism gets a device probe (ground truth), then a model, then a
recorded comparison of the two.

| Area | Mechanism | Device probe | Model |
|---|---|---|---|
| CPU core | pipeline: latencies / throughput (int, mul, load-use, VFP single / double, div, sqrt, core <-> VFP moves) | timingprobe.c `op` (built 10-15) | partial (scheduler model) |
| CPU core | branch prediction: mispredict penalty | timingprobe.c `branch` | no |
| Instruction fetch | L1 I-cache 16 KB, L2, memory: cycles per instruction by code size | timingprobe.c `icode` 1 KB - 8 MB | I-cache sim (icache.c) |
| Data side | L1 D-cache 16 KB, L2 2 MB, FCRAM latency by working set | timingprobe.c `chase` 4 KB - 16 MB | no |
| Data side | main / micro TLB reach and miss cost | timingprobe.c `tlb` 8 - 4096 pages | no |
| Data side | store / write-buffer / write-miss cost | timingprobe.c `stream` | no |
| Cores | another core's traffic through the shared L2 / bus | timingprobe.c `xcore` (core 2 streaming) | no |
| Cores | cores 1-3: availability and speed, cross-core handoff | hwprobe.c (since 10-10) | no |
| Kernel | system calls (tick, cache maintenance), thread switch, events, interrupts | timingprobe.c `svc` (part); hwprobe handoff | no |
| GPU (PICA200) | vertex throughput (shader / fixed), triangle setup, fill rate by format, texture fetch and its cache, combiner stages, depth / blend, command-list overhead, display / memory transfers | planned | no |
| DSP | ndsp mixing cost per voice, latency | planned | no |
| DMA | copies between FCRAM, VRAM and linear memory | planned | no |
| Storage | SD reads: throughput, latency, small reads (disc streaming) | planned | no |
| Clocks | 268 vs 804 MHz, L2 on / off (osSetSpeedupEnable) | hwprobe clock (part) | no |

First device run of timingprobe.c: relay job 172 (queued 2026-10-15). In Azahar it runs end to end (crash-tested) and
shows Azahar models no caches at all (chase flat at ~4.1 cycles from 4 KB to 16 MB, code fetch flat at 3.0 cycles to
8 MB): an emulator's timing is not a stand-in for the device, which is the point of probing the device.

## Limits

- A/B effects here are 1-5%, smaller than any model's error: the 2DS stays the final judge for every default.
- Native mode benefits most: once it runs, tuning is many small changes, which is where a calibrated model pays.
