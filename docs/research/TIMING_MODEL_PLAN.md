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

## First device measurements (New 2DS XL, relay job 176, 2026-10-15)

Cycles at 804 MHz from the system tick (the counters were switched off by an over-eager check on the first, tiny
measurement; fixed). The run stopped in the bandwidth section (CPU access to VRAM suspected; removed for now), so
bandwidth, kernel, SD, clocks and the GPU are still to come (job 177).

| Mechanism | Measured | Notes |
|---|---|---|
| add (dependent) | 1.06 | |
| mul: latency / throughput | 5.0 / 2.1 | |
| rev (dependent) | 2.0 | |
| L1 load-to-use (pointer chase) | 4.0 | ldr then use or one instruction between: same |
| independent loads | 1.06 | |
| VFP add / mul single, dependent | 8.0 / 8.0 | double add 8.0, mul 9.0 |
| VFP add single, independent | 2.0 | |
| VFP div / sqrt single | 19 | |
| core -> VFP -> core moves | 10 | |
| vldr then use | 8 | |
| branch: predictable / random direction | 3.44 / 7.19 per round (5 instructions) | mispredict penalty ~7.5 |
| instruction fetch: code <= 16 KB | 1.0 per instruction | 20 KB 2.0, 24 KB 2.6 |
| instruction fetch from L2 (32 KB - 1 MB) | 3.39 per instruction | ~19 cycles per 32-byte line |
| instruction fetch beyond L2 (2 / 3 / 4 / 8 MB) | 3.9 / 5.2 / 5.6 / 6.0 | |
| load latency L1 (<= 16 KB) | 4.0 | |
| load latency L2 (24 KB: 18; 32 KB: 23; 64 KB - 1.5 MB) | ~28 | |
| load latency memory (2 / 3 / 4 / 8 / 16 MB) | 35 / 102 / 128 / 156 / 166 | |
| TLB: one line per page, 8 - 512 pages | 4.0 - 9.0 | no cliff: the heap is mapped in large pages |
| TLB: 1024 / 4096 pages (4 / 16 MB spans) | 23 / 144 | cache and memory, not TLB |
| streaming stores | 4.1 per word (~780 MB/s) | |
| **core 2 streaming stores over 4 MB, core 0 chase 256 KB / 1 MB / 8 MB** | **36 / 120 / 604** (alone 28 / 28 / 156) | shared L2 evicted: the mechanism behind C2 and C15 |
| svcGetSystemTick | 198 | |
| JIT cache sync (flush + svc 0x94) | 16,548 (~20 us) | job 178: 10,095 (203 I-cache misses after it) |

Job 178 (same day, counters on) repeated every CPU number above within 1% and added the counters' view: the 24 KB
chase misses L1 on 62% of loads, 32 KB on 82%, 64 KB and up on 100%; a mispredict is 0.50 misses per random branch,
so 7.5 cycles each; code from L2 is 0.125 I-cache misses per instruction (one per 8-instruction line), 2.39 cycles of
fetch stall per instruction. It also reached the rest of the CPU side and the GPU:

| Mechanism | Measured (job 178) |
|---|---|
| memcpy / memset / read (2 words per line), 4 MB FCRAM | 301 / 742 / 517 MB/s |
| thread switch: same core round trip / core 0 <-> 2 round trip / svcSleepThread(0) | 7.91 / 2.21 / 1.13 us |
| SD: 64 KB sequential / random 32 KB / random 2 KB | 5.11 MB/s / 9.7 ms / 4.0 ms |
| clocks: speedup on / off (add_dep; 1 MB chase per load) | 757 MHz, 34.4 ns / 252 MHz, 229.7 ns |
| GPU vertex, float3 at 12 / 16 / 24-byte strides; s16 at 16 | 49.7 / 49.7 / 49.8 / 56.9 ns per vertex (13.3 GPU clocks) |
| GPU draws, ~1-triangle: GPU / CPU per draw; with a combiner change | 0.55 / 3.21 us; 0.63 / 3.51 us |
| GPU memory fill VRAM / texture copy linear -> VRAM, VRAM -> linear | 2,945 / 234 / 229 MB/s |
| GPU display transfer 400x240 RGBA8 -> RGB8 | 1.98 ms |

**Void in job 178: every GPU fill number (15 Gpixel/s) and the vertex and draw cases' clipping path.** All positions
sat at clip z +0.5, outside the PICA200's 0 >= z >= -w, so every triangle was clipped and nothing was drawn. Fixed
(d8a59a8): z -0.5 and a read-back of the target's centre pixel beside each fill number; job 179 repeats the GPU set.
Vertex costs barely depend on the attribute format, so the vertex stage (not the fetch) sets the rate.

GPU fill, job 180 (d8a59a8 + 4f4ea87: drawn, colours normalised, textures unbound for untextured cases; every centre
pixel matches the expected colour, e.g. blend 0x64321940 = half of 0xc8643280). 400x240 RGBA8 target, 16 layers:

| Fill case | ns per pixel | Mpixel/s |
|---|---|---|
| plain / alpha blend / depth test + write / 3 or 6 combiner stages | 4.90 - 4.91 | 204 |
| texture RGBA8 64x64 (magnified, fits the texture cache) | 4.91 | 204 |
| texture ETC1 1024x1024, minified ~2.5x, no mipmaps | 8.56 | 117 |
| texture RGB565 1024x1024, the same | 36.5 | 27 |
| texture RGBA8 1024x1024, the same | 59.8 | 17 |

- The 204 Mpixel/s floor (~1.3 GPU clocks per pixel) does not move with blending, depth or combiner stages: those are
  free at this size; something before them (the rasterizer or framebuffer writes) sets it.
- Texture-cache misses dominate everything else, scaled by texel bytes: ETC1 (0.5 B/texel) is 7x faster than RGBA8
  (4 B) and 4.3x faster than RGB565 (2 B) on a texture that misses. This bears directly on which format gc3ds decodes
  GameCube textures into, and on mipmaps (a minified texture without them is the worst case).
- Memory fill read 2.9, 27 and 34 GB/s in jobs 178-180: the tick around C3D_SyncMemoryFill does not time the fill
  itself; not usable yet. Display transfer 1.69 - 1.98 ms; texture copies 230 - 250 MB/s.

## Second probe set: the survey's probes (jobs 181 and 183, 2026-10-15)

Added from BENCHMARK_TOOLING_SURVEY.md section 5 (a4c749d). Job 183 is the complete run with counters (3e4bdc5);
181 repeats every CPU number within 1%, except code install (below). Cycles at 804 MHz.

| Mechanism | Measured | Reading |
|---|---|---|
| L1 D ways (4 KB stride: 4 lines hit, 5: 8.3, 6: 16.5, 8: 27) | 4-way, 4 KB way (16 KB) | replacement not LRU: one line over costs a partial miss |
| L2 ways (128 KB and 256 KB stride: 16 hit, 17: 43.5, 20: 91; 64 KB stride: 32 hit) | 16-way, 128 KB way (2 MB) | not LRU either |
| line size (1 MB sequential chase: stride 4 / 8 / 16 / 32 = 6.9 / 9.7 / 15.4 / 26.8) | 32 bytes | dcrmiss 0.125 per 4-byte load |
| sequential vs random memory (8 MB, stride 32 / 64 / 128 / 4096 vs random) | 48 / 80 / 126 / 168 vs 156 | in-order lines ~3x cheaper: prefetch or open DRAM pages; gone by 128 B |
| misses in flight (8 MB random, 1 / 2 / 3 / 4 / 6 chains) | 156 / 147 / 146 / 146 / 146 per load | one outstanding miss: batching loads gains nothing |
| one store per line, 512 KB / 8 MB (load: 15.8 / 46.6) | 32.4 / 83.7 | write-allocate: a store miss fills the line (dcwmiss 1.0) and costs ~2x a load miss |
| store then load of the same word, + add | 5.07 per trio | no forwarding penalty beyond the load's |
| stores, independent / strd / stm of 4 | 1.13 / 1.13 / 2.06 | 2 registers per cycle |
| ldrh / ldrb then use | 4.07 | as ldr |
| ldrd (independent) / ldm of 4 / ldm of 7 | 3.0 / 4.0 / 6.0 | ldrd slower than two ldr (2.1); ldm ~1 per register |
| **ldr + rev as a dependent chain (the guest's big-endian load)** | **6.0** (ldr alone 4.0) | the swap adds 2 cycles to every dependent guest load |
| taken unconditional branch, 16 - 1024 of them 8 bytes apart | 4.75 - 4.02 per branch | ~4 cycles each, no capacity cliff: no BTAC folding seen |
| indirect call from one site: 1 target / 2 / 4 / 16 round-robin / 16 random (per call, with the loop) | 28.1 / 29.6 / 30.3 / 30.9 / 30.9 | brmiss ~0 everywhere: indirect targets are not predicted, so no mispredict either |
| call chains depth 1 - 12 (per call + return) | 16.5 - 18.3 | no cliff at depth 3: no return-stack overflow cost seen |
| code install: write + clean + whole I-cache invalidate + first run, 64 B / 256 B / 1 KB | 10,960 / 11,900 / 35,600 | ~200 I-cache misses after each (the whole cache refills) |
| the same, 4 / 16 / 64 KB (job 181 / 183) | 47,400 / 110,100 / 298,500 vs 31,000 / 97,500 / 285,200 | varies 10-35% between runs |

GPU, job 181 (repeated in 183):

| Mechanism | Measured | Reading |
|---|---|---|
| vertex shader +0 / +32 / +64 instructions | 49.9 / 99.3 / 188.7 ns per vertex | each instruction costs: generated shaders should be short |
| depth test rejecting every fragment vs drawing | 4.16 vs 4.91 ns per pixel | no real early-Z: hidden pixels cost ~85% of drawn ones |
| texture at 1 texel per pixel: ETC1 / RGB565 / RGBA8 | 199 / 165 / 114 Mpixel/s | ETC1 is free, RGBA8 loses 45% |
| RGBA8 at 0.5 / 1 / 2 / 2.56 texels per pixel | 178 / 114 / 30 / 17 Mpixel/s | the texture cache gives way past one texel per pixel: mipmaps matter |
| RGBA8 256x256 (0.64 texel per pixel) | 173 Mpixel/s | |
| full screen as 32 / 16 / 8 / 4-pixel tiles | 0.72 / 0.85 / 1.20 / 1.82 ms | small triangles are vertex-bound (50.6 ns per vertex at 4 px), setup is cheap |

What it changes for the JIT (each to be priced against a frame's own counters before building):
- the byte swap on guest loads (2 cycles per dependent load) -> byte-swapped guest RAM (REDESIGN_CANDIDATES);
- taken branches (~4 cycles) -> fall-through layout of the hot path;
- code install (~11,000 cycles fixed, the whole I-cache refills) -> batch installs;
- write-allocate and one miss at a time -> compact, sequential lookup structures; no gain from overlapping loads;
- ldrd slower than two ldr -> check where the JIT emits it.

## Where core 0's time goes in TP (job 184, 2026-10-15)

A clean run (tp850, 300 s, current defaults, no A/B: the A/B runs recompile everything at each switch and ran at 15.5%
against 20.5% here). core/tools/frame_budget.py over the PMU columns, plus the sampler and profiler lines:

| Share of core 0 | What |
|---|---|
| ~11% (up to ~20% by scene; gxwait_pct) | the idle hook waiting for the GX thread (core 2) to drain: core 2 is a co-limit |
| 21% | instruction fetch stalls: 5.8 M I-cache misses/s (~29 cycles each, from L2) |
| 22% | data stalls: load-to-use waits on L1 hits plus 1.1 M read and 0.3 M write misses/s |
| < 1% | the dispatcher itself (~8,500 returns/s in steady play; 265,000/s only in the state-load window) |
| ~0.5% | compiling |
| the rest | issuing instructions (IPC 0.36; 9.3 host instructions and 25.8 cycles per guest instruction) |

The sampler's "dispatch" bucket (13-21%) is jit_entry with no block running, which includes the idle hook's busy wait:
it is mostly GX wait, not dispatch. Guest mix (PC interpreter, TP 900, 1.04 G instructions): loads 22% (lwz 12.9%),
stores 15%, conditional branches 8.1%, blr 3.3%, b 3.5%; full speed is ~104-150 M guest instructions/s, i.e. ~5-8 core-0
cycles per guest instruction against 25.8 now, and core 2's GX work would have to scale ~5x with it.

## Contention and core 0's data misses (2026-10-15, jobs 192 and qemu)

- **GPU texture reads slow core 0's memory** (gpuprobe contention, job 192): a pointer chase over 1 MB (L2) took 34.6 ns
  per load idle and 70.1 ns while the GPU sampled a 1024x1024 RGBA8 texture in FCRAM; over 8 MB (memory) 194 vs 703 ns.
  gc3ds keeps game textures in FCRAM (pica.c C3D_TexInit), so every frame the GPU draws (~8-11 ms) competes with core 0.
  The VRAM comparison did not run (no 4 MB of VRAM free): retry with a smaller texture.
- **Where compiled code's data misses come from** (core/tools/qemu-plugins/dcache.c, TP 850-910 under qemu, compiled
  code's accesses only, the first 100 M as warm-up; L1 miss rate 1.4%): guest RAM 42%; the JIT buffer read as data 35%;
  large mappings 19% (the indirect tables); the Gekko struct 3.5%. By issuing code: the shared `indirect` stub's table
  read 427 k (L2 hits), `link` literals 203 k (mostly L2 misses: the PC build's 8 MB buffer refills, not the 2DS
  pattern), `storepipe` 161 k, the float stubs ~200 k (guest data).
- **The indirect-branch table** (FAST_BITS 16): 65,536 slots of 8 bytes, 512 KB per key, 4 keys: every blr / bctr
  reads one slot, and the hot targets sit one per cache line. ~1 M returns/s on the 2DS at ~28 cycles of L2 each would
  be ~3.5% of core 0. Variant FAST_BITS=13 + 2-way (--ind2 on) on the 2DS: job 193.

## Sharing the tooling (owner direction, 2026-10-15)

James: once the measurements are in, make the tooling something everyone can use. The probes already live in this
public repository (GPL-2); to make them usable outside gc3ds:

1. A standalone homebrew app built from timingprobe.c and gpuprobe.c: no gc3ds, relay or game disc needed; it runs every
   probe and writes one results file to the SD card.
2. A results format with the console model, firmware, clock mode and probe version, so runs from different consoles
   (Old / New 3DS, 2DS, XL) compare line by line.
3. A reference results file from this New 2DS XL, and a page describing each probe, what it isolates and its known
   caveats (from BENCHMARK_TOOLING_SURVEY.md).
4. The calibrated model, once it matches the device, beside them.

## Limits

- A/B effects here are 1-5%, smaller than any model's error: the 2DS stays the final judge for every default.
- Native mode benefits most: once it runs, tuning is many small changes, which is where a calibrated model pays.
