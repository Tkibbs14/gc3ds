# GameCube/Wii on the New 2DS XL — measured findings

Everything here was measured on a New 2DS XL (Luma3DS, launched through the Homebrew Launcher, 804 MHz mode)
with `bench/` (gcbench), unless marked otherwise. Hardware background and sources: [ARCHITECTURE.md](ARCHITECTURE.md).

## CPU: running Gekko/Broadway code through a JIT

The JIT can work on this device:
- Luma lets a homebrew app make its own memory read/write/execute.
- Rewritten code must have its instruction cache refreshed: 199 of 200 rewrites ran stale code without the refresh,
  and none did with it. A full refresh costs 2.8 µs, and a 256-byte data-cache flush 4.7 µs.

Guest instructions per second for each test program (run 2). "% GC" is against Gekko's 486 MHz clock and "% Wii"
against Broadway's 729 MHz:

| program | interpreter | JIT | % GC (JIT) | % Wii (JIT) |
|---|---|---|---|---|
| integer loop | 22 MIPS | 329 MIPS | 68% | 45% |
| scalar float | 19 MIPS | 100 MIPS | 21% | 14% |
| paired singles (vertex-transform shape) | 9.5 MIPS | 55 MIPS | 11% | 8% |
| native C, same integer loop | — | 658 MIPS | 135% | 90% |

Real games do not retire one instruction per Gekko clock, so "% GC clock" is a pessimistic bound, not a frame rate.
A game's actual demand (instructions per second) still has to be measured, for example with Dolphin's JIT
profiler on the user's own dumps.

### Why floats are slow

VFP11 cost in cycles at 804 MHz (run 2):

| op | latency | throughput |
|---|---|---|
| f32 add / mul / mla | 8 | 1.1 |
| f64 add | 8 | 2.0 |
| f64 mul / mla | 9 | 2.3 |
| f32 div / sqrt | 19 | 17 |
| f64 div | 33 | 31 |
| short-vector pair add / mul / mla | 9 | 2.3 |
| int↔float convert | 8–9 | 1.1 |
| core↔VFP register move (round trip) | 5 | 5 |

- **The long result delay is the main cost.** A chain of dependent float operations pays 8–9 cycles per step, so
  the JIT should reorder independent work to overlap those delays. Run 3 measures what that buys (`pilp` vs `pseq`).
- **Short-vector mode is not faster.** One instruction per pair costs the same as two scalar instructions, so it
  only saves code size.
- **Moving a value between the integer registers and the FPU costs 5 cycles.** Paired loads and stores that route
  through integer registers pay it per lane. Run 3 measures two fixes: the two-register move, and byte-swapped
  guest RAM, so a float pair loads straight into the FPU.

### Leaving a block

| code footprint | linked jump | via C dispatcher |
|---|---|---|
| 4 KB (64 blocks) | 13 ns | 37 ns |
| 128 KB (2048 blocks) | 61 ns | 65 ns |

Once JIT code overflows the instruction cache, a block transition costs about 50 cycles, and linking stops
helping. Run 3 sweeps the code size to find the exact cliff.

### Memory paths (sequential 4 MB, run 2)

| path | ns per access |
|---|---|
| masked fast path (`bic` + `ldr` + `rev`) | 16.6 |
| range check + fast path | 17.7 |
| C function call per access (hardware-register path) | 33.7 |

The range check is nearly free. The sequential numbers are dominated by RAM bandwidth (below), not by the
instructions themselves.

## Memory (run 2)

- **Bandwidth is low.** `memcpy` runs at 307 MB/s, and a byte-swapping copy at 313 MB/s.
- **Latency is high.** A dependent random read costs 5 ns within 8 KB, 34 ns within 256 KB, and 210 ns over 24 MB,
  so a cache miss in an emulated game's working set costs about 170 cycles. Run 3 sweeps 4 KB–32 MB to locate
  L1, L2 and FCRAM exactly.
- **Space:**
  - largest single `malloc`: 58 MB (after the CPU module had fragmented the heap);
  - an 88 MB `malloc` succeeded in run 1 (Wii MEM1 + MEM2 is 88 MB);
  - `linearAlloc`: 30 MB;
  - `vramAlloc`: 3 MB per allocation.
- **The CPU cannot write VRAM.** A `memcpy` into `vramAlloc` memory killed the app, so VRAM is reachable only
  through GPU transfers. GX copy from linear memory to VRAM runs at 895 MB/s, or 248 MB/s including the
  data-cache flush.

## Cores

- Core 2 (New 3DS only) runs JIT code at full speed in parallel with core 0.
- Core 1, at the 80% app time limit, delivers about 78% of a core.
- That suggests a split: guest CPU on core 0, GPU command translation on core 2, audio and I/O on core 1.

## Instruction mix of compiled GameCube code (static, PC-side)

From `gc-tests/analyze_mix.py` over the 29 libogc example programs. This is homebrew and library code, not a
commercial game.

| class | share |
|---|---|
| integer ALU | 31.9% |
| branches | 17.8% |
| integer loads | 14.1% |
| integer stores | 13.5% |
| compares | 6.6% |
| rotate/shift | 6.6% |
| SPR/MSR moves | 4.7% |
| float load/store + arithmetic | 2.6% |
| paired (`ps_*` + `psq_*`) | 1.1% |

- **Branches come every 5.6 instructions** (median 4, 90th percentile 11). The JIT must compile across
  conditional branches (multi-exit blocks) and keep its hot code small, or block transitions dominate.
- **`lwz`/`stw` alone are 21%.** A byte-swapped RAM layout removes a `rev` from each.
- **`mtmsr`/`mfmsr` are 1.4%** (libogc switching interrupts off and on) and need a cheap path.

## Instruction mix of Twilight Princess (static, PC-side)

From a user-supplied dump of the NTSC-U release (game ID GZ2E01, revision 0). `main.dol` holds 900k instructions
of Metrowerks-compiled game code. The REL modules (`rel/`, `RELS.arc`), where most actor code lives, are not
analysed yet.

| class | Twilight Princess | libogc examples |
|---|---|---|
| integer ALU | 29.2% | 31.9% |
| branches | 19.8% | 17.8% |
| integer loads | 13.3% | 14.1% |
| integer stores | 10.7% | 13.5% |
| **float load/store** | **9.7%** | 1.7% |
| **float arithmetic** | **4.5%** | 0.9% |
| compares | 4.5% | 6.6% |
| rotate/shift | 3.5% | 6.6% |
| SPR/MSR moves | 3.4% | 4.7% |
| paired (`ps_*` + `psq_*`) | 0.8% | 1.1% |

- **Float work is mostly scalar single precision:** `lfs` 5.2%, `stfs` 2.9%, `fmuls` 0.75%, `fsubs` 0.68%.
  - The 8-cycle VFP delay is on the critical path.
  - So is the per-load cost. Byte-swapped RAM turns `lfs` into one VFP load.
- **Paired arithmetic is rare (0.1%).** The 6,600 `psq_l`/`psq_st` are most likely the compiler's FPR save/restore
  sequences (inference, not yet checked against the code).
- **Double precision is present:** `lfd`/`stfd`/`fmul` total about 2%. Gekko FPRs are 64-bit, so treating all
  floats as single precision would be an accuracy shortcut, not a free choice.
- **Calls are frequent:**
  - `bl` 7.1%, `blr` 1.9%, `bctrl` (virtual calls) 0.8%.
  - Calls and returns need linking between blocks, such as a return-address cache, not just jump linking.
- **A branch every 5.1 instructions** (median 4, 90th percentile 9).

## Run 3 (2026-10-10, full suite, no crashes; raw output in `bench/results/latest.txt`)

### CPU
- **Run JIT blocks in big-endian data mode (`SETEND BE`).** It is correct on hardware and costs 1 cycle per switch.
  It is as fast as byte-swapped RAM (+10% integer, +12% float, +14% paired over byte-swapping each access) while
  keeping RAM in console byte order, which the GPU and DMA paths need.
- **Instruction reordering bought only about 10%** on paired code (pseq 55 → pilp 61 MIPS). The bigger cost is
  moving values between the integer registers and the FPU: about 3.5–4 cycles each, and they don't overlap.
  Float-type paired loads avoid that by going straight into the FPU. Integer-quantised ones cannot.
- **The caches** (CP15 cache type register):
  - L1 instruction and data caches are 16 KB, 4-way, with 32-byte lines.
  - **Instruction cache cliff:** linked-block cost goes from 13 ns up to 16 KB of JIT code to 53–62 ns from 32 KB.
    Misses are served by the L2 at about 45 cycles.
  - **Data latency:** 4 cycles in L1 (up to 16 KB), about 29 cycles in L2 (up to 2 MB), and 126–170 cycles in
    FCRAM (4 MB and up).
- **Clock and L2 cache: use both** (804 MHz + L2).
  - Without the L2, a 1 MB working set costs 188 ns per access instead of 36 ns.
  - JIT integer speed tracks the clock (109 → 329 MIPS) and is unaffected by the L2, because it fits in L1.
- **The JIT compiles at 0.49 µs per guest instruction** (2 M instructions/s). That is about 50 ms for a
  100k-instruction hot set.
- **Read the system tick sparingly:** `svcGetSystemTick` is a system call costing 251 ns.

### System
- **SD card:**
  - sequential reads 10 MB/s, writes 6.1 MB/s;
  - random reads: 8 ms per 32 KB (3.9 MB/s), 2.4 ms per 2 KB.

  That covers GameCube disc speed (2–3 MB/s) and is borderline for Wii speed (6–9 MB/s). Needs a read cache.
- **Wii disc encryption:**
  - Software AES runs at 10.1 MB/s, so decrypting at Wii disc speed takes 61–92% of a core.
  - Hardware AES is not usable: the PS service only takes fixed key slots.
  - So **decrypt Wii images once on the PC** (a conversion tool for the user's own dumps), not at runtime.
- **Audio is cheap:**
  - DSP-ADPCM decode, resampling and mixing cost 0.42% of a core per voice (239 voices per core).
  - `ndsp` works on this console, so the DSP firmware dump is present.
- **Cross-core handoff:**
  - spinning on a shared flag: 0.5 µs round trip;
  - LightEvent between cores 0 and 2 **timed out on the third trip**, which needs investigating before relying on
    it. Use lock-free queues with spinning or backoff for the GPU FIFO.

### GPU (PICA200 through citro3d, off-screen, no vsync)
- **Geometry is plentiful:**
  - 6.0 Mtri/s (5.0 at 20k triangles per frame);
  - 4.3 Mtri/s with a GameCube-style 4-light vertex shader;
  - 10 Mtri/s with shared indexed vertices.

  A GameCube 3D game probably needs about 1–2 Mtri/s (assumption).
- **Fill rate:**
  - 193 Mpx/s with textures in VRAM; blending, depth test, and 1, 3 or 6 combiner stages are all free.
  - Textures in FCRAM are 2.5–3.6× slower, so keep the texture cache in VRAM. That has only 6 MB.
- **Texture upload:** 2 ms per 256 KB RGBA8 texture (about 130 MB/s), and over half of that is the data-cache
  flush.
- **Render to texture (EFB copy):**
  - rendering into a texture costs nothing extra;
  - an extra 256×256 copy costs 2 ms;
  - reading 256×256 back to the CPU costs 3.9 ms.

  EFB copies need to stay on the GPU, and CPU readback must be rare.
- **Vertex conversion on the CPU is expensive:**
  - big-endian GameCube vertices to floats: 7.3 Mvert/s (13.7 ms per 100k vertices);
  - to 16-bit integers: 21.5 Mvert/s (4.6 ms).

  Keep GameCube's integer vertex formats where the PICA200 supports them, and only byte-swap. Run it on core 2.

## Open questions and what answers them

| question | answered by |
|---|---|
| Does byte-swapped RAM and two-register moves fix paired-single speed? | run 3, CPU section |
| How much does reordering instructions buy against VFP latency? | run 3, `pseq` vs `pilp` |
| Where are the L1/L2/FCRAM and instruction-cache cliffs? | run 3 sweeps |
| Can JIT blocks run in big-endian data mode (`SETEND BE`), keeping RAM in console byte order with no swaps? What does `SETEND` cost? | run 3: `setend` rows, `setend be/le` kernel (all 90 qemu cases pass) |
| Do 804 MHz and the L2 cache each help, separately? (DaedalusX64-3DS reported a slowdown with both on) | run 3: clock/L2 table |
| Real L1 I/D cache sizes (the ARM manual allows 16/32/64 KB) | run 3: CP15 cache type |
| Can the Wii's 24 + 64 MB fit? (libctru's default heap split gives 58 + 32 MB) | next: override `__ctru_heap_size`, or a 124/178 MB memory mode |
| SD read speed, AES/SHA-1 speed, audio mixing cost, cross-core handoff latency | run 3, system section |
| PICA200 triangles, fill rate, combiners, texture upload, render to texture | run 3, GPU section |
| How many instructions per second a real game executes | Dolphin JIT profiler on the user's own dumps |
| Which GameCube/Wii hardware features games actually use | Dolphin logs on the user's own dumps; ARCHITECTURE.md |
