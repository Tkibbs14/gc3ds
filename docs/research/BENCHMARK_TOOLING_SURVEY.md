# Benchmark tooling survey for the New 2DS XL timing model

Date: 2026-10-15. Scope: web research only (no binaries or ROMs downloaded). Model: claude-haiku-5-5 research agent.

Evidence tags used throughout:
- **SOURCED**: read on a fetched page.
- **SNIPPET**: taken from a search-result excerpt; the page itself was not opened. A pointer, not a finding. Do not cite as evidence until read.
- **INFERRED**: my reasoning from the sources above.
- **NOT FOUND**: searched; nothing usable.

## 0. Bottom line

- Published 3DS hardware ground truth is thin. The 3dbrew Hardware page gives clocks, a few memory sizes and the DSP/GPU identity. It gives no L1 sizes, no MPCore cache latencies, no GPU cache geometry, no DMA throughput, no Y2R or DSP timing. (SOURCED)
- No published cycle-level measurement was found for: ARM11 MPCore L1/L2 latency, the New 3DS L2 hit cost, PICA200 texture or vertex cache geometry, NDMA throughput, Y2R conversion time, DSP cycle cost. Our own probes are the only candidate ground truth. (NOT FOUND)
- The closest ARM11 cache numbers come from a single-core ARM11 part (SD5113), not MPCore. Use them as order-of-magnitude priors only. (SOURCED, with caveat, see section 2)
- Largest gaps in our current probe list: cache line size and associativity, the store path (write-allocate, store-buffer depth), indirect-branch and return prediction (dispatch), I-cache coherence cost after writing JIT code, bus contention with DMA and GPU, and GPU fixed-function costs (overdraw, setup rate, vertex reuse, lighting/fog/shadow stages).
- ROB-size and ROB-based MLP methods do not fit ARM11, which is single-issue. Use pointer-chase lanes for MLP and hit-under-miss probes instead. (INFERRED from the SD5113 description, SOURCED for that part)

## 1. Hardware ground truth (what the sources state)

| Item | Value stated | Tag | URL |
|---|---|---|---|
| Old3DS ARM11 | 2x MPCore, 2x VFPv2, 268 MHz (exact 268,111,856.0 Hz) | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| New3DS ARM11 | 4x MPCore, 4x VFPv2, up to 804 MHz, "optional 2MB L2 cache" | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| New3DS clock policy | 804 MHz "appears to be limited" to app cores; Home Menu runs 804; other apps 268 unless a flag is set; only a 3x multiplier with official kernel code | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| Tick counter | svcGetSystemTick uses a 268 MHz tick counter in this mode | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| ARM9 | ARM946 at 134 MHz | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| GPU | DMP PICA at 268 MHz; OpenGL ES 1.1 compatible; 3 programmable vertex processors plus a Primitive Engine that can act as a 4th; 4 texture units, the 4th only for procedural textures | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| VRAM | 6 MB inside the SoC | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| DSP | CEVA TeakLite at 134 MHz, 24 channels, 32728 Hz rates | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| FCRAM (3DS) | 2x64 MB Fujitsu parts in table; New 2DS XL entry "not fully specified" | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| NDMA | 8 channels; word = 32 bits; block = smallest burst, bus held until block completes; block length 1<<x words; 16-bit interval timer with prescaler 1/4, 1/16, 1/64 of an undefined "system frequency"; arbitration fixed or round-robin; "cycle selection" bits undefined | SOURCED | https://3dbrew.org/wiki/NDMA |
| NDMA throughput | not stated | NOT FOUND | https://3dbrew.org/wiki/NDMA |

Discrepancies and open points (do not resolve by assumption):

1. **FCRAM size.** The 3dbrew Hardware page says "One FCRAM IC" in the FCRAM section, and lists "2x64MB" in the Specifications table for the 3DS (SOURCED, internal inconsistency). The 256 MB figure in our brief is not stated for the New 2DS XL on the fetched page. Confirm on device.
2. **New 2DS XL vs New 3DS.** The fetched text does not explicitly tie the New 2DS XL to the New3DS CPU/L2 block. INFERRED that it shares the New3DS SoC family; confirm.
3. **"Shared" L2.** Our brief says a 2 MB shared L2. The page says "optional 2MB L2 cache" and does not say shared (SOURCED). Unverified.
4. **Older L2 figure.** Search excerpts of older 3dbrew revisions mention a 32K L2 (SNIPPET, revision-diff titles only). The 2 MB figure is the current one. Not verified.
5. **DSP name.** 3dbrew says "CEVA TeakLite" (SOURCED). A Teakra-related excerpt says GBATEK labels it TeakLite II (SNIPPET). Unresolved; read GBATEK before naming it.
6. **Timer resolution.** svcGetSystemTick is 268 MHz (about 3.7 ns per tick), which is too coarse for single-op timing on its own. A cycle-level counter needs the ARM11 PMU. Whether user mode can read it on this kernel: NOT FOUND. The ARM11 PMU exists (SNIPPET: Apple archive "ARM11PerformanceCounterEventList" page, https://developer-mdn.apple.com/library/archive/documentation/DeveloperTools/Conceptual/SharkUserGuide/ARM11PerformanceCounterEventList/ARM11PerformanceCounterEventList.html). Check DDI 0360F for the counter encoding and access rules before relying on it.

## 2. CPU, cache and memory methodologies

### 2a. Suites and methods

| Tool / paper | Mechanism measured | Method (concise) | Covered by our list? | Gap it fills | Tag / URL |
|---|---|---|---|---|---|
| lmbench `lat_mem_rd` | Load-to-use latency vs size and stride | Pointer ring at fixed stride, serialized loads; sizes about 512 B to 8 MB; ns per load. Caveat: fixed stride can be prefetched, so results depend on prefetcher policy | Partly (pointer chase by working set) | Stride dimension: line size, prefetch response | SNIPPET https://manpages.org/lat_mem_rd/8 ; https://fastpath.docs.arm.com/en/latest/user-guide/benchmarkcatalog/lmbench-lat-mem-rd.html |
| tinymembench | Random-access latency and sequential bandwidth | Latency averaged over random loads per buffer size, reported as extra time over L1; bandwidth with several prefetch variants; ns per access | Mostly | Reporting convention (extra over L1) to copy for comparability | SNIPPET https://github.com/sysprog21/tinymembench |
| Calibrator (Manegold, CWI) | Cache levels, line size, miss latency, TLB | Working set much larger than largest cache; reports miss latency on an idle bus (about 100 cycles between misses) and on a busy bus (back-to-back misses); main-memory latency = sum of miss latencies; per TLB level: entries, page size, miss latency | Partly (working-set sweep) | Idle-bus vs busy-bus miss latency distinction, which maps directly to DMA and GPU contention; TLB page-size dimension | SOURCED https://homepages.cwi.nl/~manegold/Calibrator/ |
| Saavedra and Smith (UCB/CSD-93-767) | Cache size, line size, associativity, TLB | 2D sweep over address range and stride; about 200 observations per test; the program must consume the loaded values, otherwise the compiler removes the loads (noted in a course assignment based on the method) | No (line size and associativity are not in our list) | Line size and associativity for L1D, L1I, L2 | SNIPPET https://www2.eecs.berkeley.edu/Pubs/TechRpts/1993/6283.html (page not opened; description from secondary sources) |
| Dongarra et al. 2004 (PAPI) | Same parameters from hardware event counts | Uses counters instead of timing inference | No | Not usable here unless the PMU is user-readable | SNIPPET https://hpcg.icl.utk.edu/files/publications/2004/icl-utk-186-2004.pdf |
| Auckland cache/TLB study (Linux) | Data cache capacity, line size, associativity, latency; TLB size, associativity, latency | Separates read and write characterization; does not assume bit-select set mapping | No | Write-path characterization; non-bit-select mapping check | SNIPPET https://www.cs.auckland.ac.nz/~cthombor/Perf/Cache/cachespects.pdf |
| STREAM | Achievable bandwidth | Arrays at least 4x total LLC or 1M elements (rule as stated in search excerpt; primary page not opened). Byte accounting: 16 B/iter for copy and scale, 24 B/iter for add and triad; write-allocate corrections of about 1.5x (copy/scale) and 1.33x (add/triad) | Partly (memcpy/memset) | Byte accounting: our memcpy and memset numbers must say whether write-allocate traffic is counted | SNIPPET https://blogs.fau.de/hager/archives/8263 ; https://stream.cs.virginia.edu/ref.html (not opened) |
| likwid-bench | Kernel bandwidth and throughput | `-w` sets working set per workgroup; `-s` sets minimum runtime and the iteration count is derived; `-l` prints bytes per update for each kernel; counter validation through likwid-perfctr (x86) | Partly | Method: derive iteration count from min runtime; report bytes per update explicitly | SNIPPET https://github.com/RRZE-HPC/likwid/wiki/Likwid-Bench |
| RAMspeed | Cache and memory bandwidth | `-m` MB per array (default 32), `-g` GB per pass (default 8); decimal MB/s unless `-r` | No | Units only; versions differ, confirm defaults | SNIPPET https://gitee.com/cputests/ramspeed |
| nanoBench (Abel and Reineke) | Instruction latency and throughput; cache analysis of 11 Intel cores | Kernel-space execution; very low counter-read overhead; generated microbenchmarks for cache characterization | No | Method only; x86 kernel-mode tool, not usable on 3DS as is | SNIPPET https://arxiv.org/abs/1911.03282 |
| uops.info / Agner Fog | Instruction latency, throughput, port usage | Dependent chains for latency, independent chains for throughput; uops.info automates it on nanoBench; Agner's tables are hand-interpreted and often lack memory-operand latency | Partly (ALU and VFP latencies) | Confirm our VFP and ALU probes separate latency chains from throughput chains | SNIPPET https://arxiv.org/pdf/1810.04610 |
| uarch-bench (travisdowns) | Store buffer and other fine-grained effects | x86 assembly benchmarks with a PMC library; README warns results are not a general CPU measure | No | Store-buffer probe pattern (concept only) | SNIPPET https://github.com/travisdowns/uarch-bench |
| Lemire MLP lanes | Memory-level parallelism | N independent pointer chains; increase N until a further lane adds under 5 percent; author says this underestimates true MLP | No | Outstanding-miss count for FCRAM reads | SNIPPET https://lemire.me/blog/2018/11/05/measuring-the-memory-level-parallelism-of-a-system-using-a-small-c-program/ |
| par_mem | Available parallelism | Latency at parallelism 1 divided by average latency across parallelism levels | No | Single MLP scalar | SNIPPET https://manpages.org/par_mem/8 |
| FetchBench (CCS 2023) | Proprietary prefetchers | Cross-platform tests on 19 ARM and x86-64 CPUs; found a replay-based prefetcher on Cortex-A72 | No | Prefetch detection method | SNIPPET https://misc0110.net/files/fetchbench_ccs23.pdf |
| Pref-X | Prefetch inference without counters | Infers prefetch activity from L1 content changes; reported accuracy on Cortex-A7 and A53 | No | Counter-free prefetch detection | SNIPPET https://hal-lirmm.ccsd.cnrs.fr/lirmm-03767077 |
| robsize (Wong method) | Reorder buffer size | Dependent-load blocking; Chips and Cheese reports the method largely failed on Cortex-A73 except for scheduler size; a retirement-blocking variant was used | Not applicable | ARM11 is single-issue (see below) | SNIPPET https://gitee.com/guoami/robsize ; https://chipsandcheese.com/i/149874023/out-of-order-retire |

### 2b. ARM11 facts available

Source for all of the following is the 7-cpu page, which describes a **single-core SD5113 ARM11 part, not MPCore**. Use as priors only. https://7-cpu.com/cpu/ARM11.html (SOURCED, with caveat)

- L1D and L1I: 16 KB, 32 B lines, 4-way. The MPCore L1 size is configurable in the TRM, so 16 KB is not confirmed for the 3DS.
- L1 hit: 4 cycles (table row). L1 TLB: 10 entries, fully associative. L2 TLB: 64 entries, 2-way.
- Branch misprediction penalty: 6 cycles. BTAC: 128 entries, direct-mapped, 2-bit. Static prediction: forward not taken, backward taken. Return stack: 3 entries, unconditional returns only.
- Single-issue, out-of-order completion. Hit-under-miss continues until a dependent instruction needs the data; stalls after three successive data misses.
- RAM: about 100 ns per 32 B line for parallel random reads (not MPCore).
- Missing from the page: L2 size and latency, any MPCore detail, whether the 180 ns and 400 ns figures are measured.

Primary documents located but **not read** (search index only):
- ARM11 MPCore TRM, DDI 0360 series (revision r2p0 listed as DDI 0360F). Contains the L1 configuration, cycle timing and PMU sections that we need. https://documentation-service.arm.com/ (SNIPPET; specific PDF not identified)
- L2C-310 TRM, DDI 0246H (r3p3). Latency and timing sections needed if the New 3DS L2 is a PL310-class controller. This is INFERRED, not verified. (SNIPPET)

### 2c. Methodology notes that transfer

- Pointer chains defeat most prefetchers; stride sweeps do not (lmbench caveat). Run both and compare.
- Iteration count from a minimum runtime (likwid -s) is more robust than a fixed loop count.
- Report bytes per update explicitly (STREAM/Hager). Our memcpy and memset numbers need this.
- Compilers delete unused loads: consume the result (Saavedra/Smith assignment note; SNIPPET).
- The Calibrator distinction between idle-bus and busy-bus miss latency is the cleanest way to measure contention on a shared FCRAM bus.
- Clock mode and L2 state must be recorded with every number (268 vs 804 MHz; L2 on or off). The only L2 evidence found is user reports of Clock+L2 (SNIPPET: https://nyc1.lr.ggtyler.dev/r/3dshacks/comments/5aysu3/luma3ds_v65_released/d9kjc2s/?context=3 ; https://gbatemp.net/threads/speeding-up-3ds-games-on-a-new-3ds.492398/). These are anecdotal, not measurements.

## 3. GPU (PICA200) methodologies and sources

### 3a. 3DS-specific sources

| Source | What it provides | Tag | URL |
|---|---|---|---|
| 3dbrew Hardware | Pipeline counts, texture unit count, 268 MHz GPU clock | SOURCED | https://www.3dbrew.org/wiki/Hardware |
| 3dbrew GPU register and shader pages | Register-level detail, texture cache controls | NOT READ (not opened in this pass) | https://www.3dbrew.org/wiki/ |
| aspargas2/3ds_hw_test | Memory tester only (subset of Memtest86+). No GPU or timing tests; roadmap lists "add tests for other 3DS hardware components". Build needs firmtool and arm-none-eabi or devkitARM | SOURCED | https://github.com/aspargas2/3ds_hw_test |
| Azahar "Archive of 3DS Documentation" | Index of 17 sources. GPU items: fincs notes (procedural textures, shadow mapping, gas rendering), wwylele diagrams, DMP papers (Ohbuchi and Unno, "A Real-Time Configurable Shader Based on Lookup Tables"; Kazakov and Ohbuchi, "Primitive Processing and Advanced Shading Architecture for Embedded Space"). No cache, DSP, DMA or timing sources listed | SOURCED (index); papers NOT READ | https://citra.azahar-emu.org/wiki/archive-of-3ds-documentation/ |
| Archive of 3DS Tests (openpunk wiki) | Lists yuriks GPU test repositories including "3DS_gpu_tests". Last edited 2017; content and existence NOT VERIFIED | SNIPPET | https://git.openpunk.com/OpenPunk/citra/wiki/Archive-of-3DS-Tests |
| Azahar 2126.0 release notes | Experimental "Simulate 3DS GPU timings" (PRs #2095, #2145), off by default, "highly experimental"; CPU timing accuracy (PR #2262); GSP accuracy (PR #2273); headphone toggle (PR #2099). Notes say no New 3DS clock or L2 items. Read via a third-party mirror, not the GitHub release page | SOURCED (via newreleases.io mirror) | https://newreleases.io/project/github/azahar-emu/azahar/release/2126.0 |
| Azahar hardware renderer write-up | Part of PICA emulation runs on the CPU; OpenGL cannot express some PICA edge cases | SNIPPET | https://citra.azahar-emu.org/entry/improvements-to-hardware-renderer/ |
| Panda3DS README | HLE emulator; credits Corgi3DS (LLE) as documentation for PICA details. Corgi3DS not located in this pass | SNIPPET | https://github.com/wheremyfoodat/Panda3DS |
| citro3d / citro2d | API libraries. citro2d states it removes bottlenecks for higher throughput. No timing benchmarks found | SNIPPET | https://citro2d.devkitpro.org |

Texture-cache hint: a Citra commit flushes the texture cache when bit 16 of register 0x80 is set (SNIPPET: https://git.openpunk.com/OpenPunk/citra/commit/046daac427f937fdb570ae267c339ad25f006d68). This is emulator behaviour, not a hardware cache specification.

### 3b. General GPU microbenchmark literature

| Source | Method or finding | Tag | URL |
|---|---|---|---|
| GPUBench (Stanford) | Texture bandwidth with three access cases: same texel (hit), sequential, random dependent. ALU work made dependent so the compiler cannot remove it. Also download, readback, instruction throughput, precision | SNIPPET | https://graphics.stanford.edu/projects/gpubench/ |
| Wong et al., "Dissecting GPU Memory Hierarchy through Microbenchmarking" | Cache size, then line size by stride sweep, per excerpt | NOT READ (PDF returned as binary by the fetch tool; abstract-level only) | https://arxiv.org/pdf/1509.02308 |
| Kerbl et al., "Revisiting the Vertex Cache" (HPG 2018) | Tests for a post-transform cache on several vendors' GPUs; results inconsistent with a common post-transform cache; behaviour looks batch-based | SNIPPET | https://www.tugraz.at/institute/icg/research/team-steinberger/research-projects/gpu-rendering-pipelines/shadingrate |
| TCOR (HPCA 2022) | Optimal replacement for the tile cache in tile-based GPUs | SNIPPET | https://digitum.um.es/digitum/bitstream/10201/138323/1/TCOR-HPCA2022-preprint.pdf |
| Early Visibility Resolution (HPCA 2019) | Early-Z on a Mali-450-like TBR design; about 20 percent less overshading | SNIPPET | https://webs.um.es/jlaragon/papers/anglada_HPCA19b.pdf |
| Rendering Elimination | Tile skipping by signature; reported 1.74x speedup on Android workloads | SNIPPET | https://arxiv.org/abs/1807.09449 |
| Triangle Dropping | About 60 percent of parameter-buffer primitives occluded in its benchmarks | SNIPPET | https://upcommons.upc.edu/entities/publication/a57f340a-1aed-4f58-a057-cdd7abc4d977/full |
| Texture fill rate | Commonly TMUs x clock, with no agreed reporting formula | SNIPPET | https://www.techpowerup.com/forums/threads/texture-fill-bandwidth-guide.44330 |

### 3c. GPU items with no source found

- PICA200 texture cache capacity, line size, associativity: NOT FOUND.
- Post-transform vertex cache on PICA200: NOT FOUND.
- Early-Z or hierarchical-Z behaviour: NOT FOUND.
- Triangle setup rate: NOT FOUND.
- Fragment lighting LUT, fog, shadow, procedural-texture cost: NOT FOUND (feature list only, from 3dbrew).
- MSAA or antialiasing support on PICA200: NOT FOUND.
- GPU timers or performance counters on PICA200: NOT FOUND.
- Command-buffer or GX submission overhead: NOT FOUND.

Practical method for GPU timing (INFERRED): the GPU runs at 268 MHz, so GPU cycles are wall time x 268 MHz. Wall time can be taken with svcGetSystemTick (268 MHz tick) over many repeated draws, with an explicit GPU-idle barrier before and after. Overhead of the barrier must be measured separately.

## 4. DSP, audio, DMA, Y2R

- **DSP.** CEVA TeakLite, 134 MHz, 24 channels (SOURCED, 3dbrew). Cycle timing: NOT FOUND. Emulation approaches: Citra HLE audio was built by logging shared-memory reads and writes and replaying them on hardware to derive firmware responses (SNIPPET: https://citra.azahar-emu.org/entry/hle-audio-comes-to-citra/). Citra's LLE option is far slower (SNIPPET, same source family). Teakra is a DSi/3DS DSP emulator, disassembler, assembler and tester (SNIPPET: https://github.com/DesperateProgrammer/teakra). Measuring DSP cost directly is not possible from the CPU side with these sources. INFERRED options: measure CPU idle per ndsp audio frame at different channel counts; measure completion latency of ndsp frames; neither isolates DSP cycles.
- **NDMA.** Register map, interval timer, prescaler and block semantics are SOURCED (https://3dbrew.org/wiki/NDMA). Sustained throughput: NOT FOUND. The "cycle selection" field is undefined on the page (SOURCED).
- **CDMA and XDMA.** Not searched in depth in this pass. Treat as NOT FOUND.
- **Y2R.** libctru exposes start-conversion, busy and DMA-done polling, input formats (planar and packed YUV 4:2:2 and 4:2:0, 8- and 16-bit), output RGB32, RGB24, RGB16 555 and 565 (SNIPPET: https://libctru.devkitpro.org/y2r_8h.html). No timing source found on 3dbrew in this pass; the Y2R page itself was not located. NOT FOUND.
- **Cache maintenance cost** (flush, invalidate, instruction-cache sync after writing code). No source found. This is directly relevant to JIT code installation. NOT FOUND.

## 5. Top 15 additional probes (prioritised for JIT, texture decode and GX/PICA emulation)

Priority is by expected cost to the emulator and by how completely our list misses the mechanism. Methods are concise; each needs a self-check against a known positive and negative before use.

1. **I-cache coherence and code-install cost.**
   - Mechanism: cost of writing N bytes of code, cleaning the D-cache, invalidating the I-cache, then first execution.
   - Method: N from 64 B to 64 KB; time write + clean + invalidate + first run; compare to warm run.
   - Why: every JIT block install pays this. No 3DS number is published. (NOT FOUND)

2. **Indirect-branch prediction.**
   - Mechanism: target prediction for a single indirect site with 1 to 16 targets, round-robin and random patterns, and many sites sharing a predictor.
   - Method: vary targets per site and number of sites; measure cycles per dispatch.
   - Why: interpreter and JIT dispatch. ARM11 mispredict penalty 6 cycles (SOURCED for SD5113 only).

3. **Return-stack depth.**
   - Mechanism: return misprediction once call depth exceeds the return stack.
   - Method: call chains of depth 1 to 8, then unwind; measure return cost per level.
   - Why: nested helper calls in JIT runtime. SD5113 prior: 3 entries (SOURCED for SD5113).

4. **BTAC capacity.**
   - Mechanism: zero-cycle taken-branch prediction capacity.
   - Method: N direct branches (32 to 512) at spaced addresses, loop over them, measure cycles per branch.
   - Why: large JIT blocks with many taken branches. SD5113 prior: 128 entries (SOURCED for SD5113).

5. **L1D line size and associativity.**
   - Mechanism: conflict and capacity knees.
   - Method: Saavedra-Smith 2D sweep (size x stride); conflict probe with addresses spaced at the way size; consume loaded values.
   - Why: layout of guest-state structs and block-local data. Our current list has capacity only.

6. **Instruction-side conflicts.**
   - Mechanism: L1I aliasing and L2 code fetch under large or sparse code.
   - Method: extend the I-fetch-by-code-size probe with aliasing spacing (power-of-two strides) and sparse basic blocks.
   - Why: code-cache thrash when compiled blocks are spread out.

7. **Store path.**
   - Mechanism: write-allocate vs no-allocate, store-miss cost vs load-miss cost, store-buffer depth, store-to-load forwarding latency.
   - Method: store to a cold line with and without a preceding load; N back-to-back stores then a dependent load; forwarding latency with varying gap.
   - Why: guest-state and register-file writes, framebuffer and PICA command writes. Auckland study notes read and write should be characterized separately (SNIPPET).

8. **Load/store-multiple and sub-word costs, including byte swap.**
   - Mechanism: LDM and STM cost by register count; LDRD and STRD; LDRH, LDRB; unaligned LDR (SD5113 penalty: 1 cycle for a 4-byte range cross, SOURCED for SD5113); REV.
   - Method: per instruction form, unrolled, with empty-loop subtraction.
   - Why: GameCube and Wii guest code is big-endian; every guest load may need REV. This cost is not in our list.

9. **Prefetch detection.**
   - Mechanism: whether ARM11 MPCore has a data prefetcher, and its response to stride.
   - Method: sequential vs stride 32 to 4096 B, with and without pointer-chase randomization; FetchBench-style pattern inference.
   - Why: guest memory streaming and texture reads. ARM11 prefetch behaviour is unknown (NOT FOUND).

10. **Memory-level parallelism (outstanding misses).**
    - Mechanism: how many FCRAM misses can be in flight from one core.
    - Method: Lemire lanes or par_mem ratio over FCRAM-sized working sets; hit-under-miss check (SD5113 prior: stall after three misses).
    - Why: decides whether batched or interleaved guest loads help.

11. **Bus contention matrix (idle-bus vs busy-bus).**
    - Mechanism: CPU miss latency while other agents use FCRAM and the bus.
    - Method: Calibrator-style pointer chase in FCRAM while (a) the second core streams, (b) NDMA runs at several block sizes, (c) GPU fill or texture copy runs; record idle-bus and busy-bus latency separately.
    - Why: on hardware these run concurrently; decides whether the timing model needs a contention term.

12. **NDMA sustained rate and CPU stall.**
    - Mechanism: words per second as a function of block size (1 << x words), interval timer, prescaler, arbitration, and CPU latency during transfers.
    - Method: sweep the 3dbrew register fields; measure throughput and concurrent CPU latency; record the undefined "cycle selection" values as found.
    - Why: guest DMA transfers and texture or audio uploads.

13. **PICA200 texture-fetch cache geometry.**
    - Mechanism: capacity and line size from fetch stride over textures of 4 KB to 1 MB, for RGBA8 and ETC1.
    - Method: fill-bound draws with a GPU-bound check; sweep stride; knee in time per fragment; Wong-style procedure after reading the paper.
    - Why: the cost of texture decode path choices in the emulator (native ETC1 vs software decode).

14. **Vertex-shader instruction-count scaling, with and without the Primitive Engine.**
    - Mechanism: vertex throughput vs shader instruction count, with 3 vertex processors vs 4 (Primitive Engine enabled as a 4th vertex processor, per 3dbrew).
    - Method: shader with N arithmetic instructions for N from 1 to the program limit; fixed vertex count; compare the two modes.
    - Why: decides shader translation and caching costs for the emulator.

15. **Overdraw, early-Z and triangle-setup rate.**
    - Mechanism: fill cost vs overdraw count; front-to-back vs back-to-front; setup rate for 1 to 16 pixel triangles at fixed pixel count; post-transform reuse with index locality (Kerbl test).
    - Method: overdraw 1 to 8 with depth on and off; small-triangle sweep; index-order sweep.
    - Why: game-dependent GPU load inside the emulator frame budget. Early-Z and setup behaviour on PICA are NOT FOUND.

Ranked lower but worth adding: fixed-function fragment stage costs (lighting LUT, fog, shadow, procedural texture unit, one to four TMUs); GX display transfer and texture copy with format conversion and tiling; Y2R conversion time by format and size; ndsp CPU load per audio frame; svc and interrupt latency with DMA interrupts active.

## 6. Method hygiene for the existing probes

- State byte accounting (bytes requested vs bytes moved, including write-allocate). Difference is 1.33x to 1.5x (SNIPPET: STREAM/Hager).
- Keep the working set well above the largest cache, and show the knee (Calibrator; STREAM 4x rule as a floor only; SNIPPET).
- Separate latency chains from throughput chains (Agner, uops.info; SNIPPET).
- Consume every loaded value (Saavedra-Smith assignment note; SNIPPET).
- Timer: svcGetSystemTick is 268 MHz (SOURCED). Check whether a user-mode cycle counter exists before using per-cycle claims (NOT FOUND).
- Replicate and report spread, not a single run. Standing rule: a single sample per cell is not evidence.
- Record clock mode (268 or 804 MHz), L2 state, and app-core flag with each result.

## 7. Not found

- Any published ARM11 MPCore cache latency, line size or associativity for the 3DS: NOT FOUND.
- Measured New 3DS L2 hit or miss cost: NOT FOUND. Only anecdotal Clock+L2 reports (SNIPPET).
- Published 3DS benchmark numbers of any kind beyond clocks and sizes: NOT FOUND. Digital Foundry reported no performance change on New 3DS for existing titles unless software requests it (SNIPPET: https://mynintendonews.com/2016/03/28/digital-foundry-takes-another-look-at-new-nintendo-3ds/).
- PICA200 cache, early-Z, setup, post-transform, MSAA, GPU timers: NOT FOUND (see 3c).
- NDMA throughput, Y2R time, DSP cycle cost, CDMA/XDMA: NOT FOUND.
- Cache maintenance cost on the 3DS: NOT FOUND.
- Corgi3DS repository contents: NOT FOUND (referenced, not located).

## 8. Next reads (ranked by value to our probes)

1. ARM DDI 0360F (MPCore TRM): L1 configuration, cycle timings, PMU counter access and user-mode rules. Not read.
2. ARM DDI 0246H (L2C-310): latency and timing sections. Not read. Applies only if the New 3DS L2 is that controller (INFERRED).
3. 3dbrew GPU register and texture pages: texture cache controls and register 0x80 bits. Not read.
4. Azahar GPU-timing PRs #2095 and #2145 (GitHub): implementation parameters for the experimental GPU timing model. Not read; mirror only.
5. Teakra source and GBATEK DSP section: resolves the TeakLite naming discrepancy and DSP timing. Not read.
6. Wong et al., arXiv 1509.02308: needs text extraction (fetch tool returned raw PDF bytes). Run pdftotext locally, then read.
7. Kerbl et al. 2018 (vertex cache) and GPUBench source: GPU probe methods. Not read.
8. CDMA/XDMA search: not done.
9. Corgi3DS: locate the repository and check its PICA documentation. Not located.

## Sources

- https://www.3dbrew.org/wiki/Hardware (SOURCED)
- https://3dbrew.org/wiki/NDMA (SOURCED)
- https://7-cpu.com/cpu/ARM11.html (SOURCED, SD5113 part)
- https://homepages.cwi.nl/~manegold/Calibrator/ (SOURCED)
- https://github.com/aspargas2/3ds_hw_test (SOURCED)
- https://citra.azahar-emu.org/wiki/archive-of-3ds-documentation/ (SOURCED)
- https://newreleases.io/project/github/azahar-emu/azahar/release/2126.0 (SOURCED, mirror)
- Search-excerpt sources (SNIPPET) as linked in the tables above.
