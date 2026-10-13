# Performance levers: what is known, what is ruled out, what is open

Written 2026-10-13 from six research passes (Haiku agents, each fact tagged SOURCED / INFERRED in their reports) checked
against the measurements in HANDOFF.md, FINDINGS.md, OPT_JIT_PLAN.md and research/*.md. Load-bearing claims were
re-read in the primary source (marked **checked**). Goal: TP at 30-40% now, full speed later, on the New 2DS XL.

## Where TP's time goes today (2DS, measured)

- TP runs ~20-21% in the heavy scenes. Core 0 alone would run ~22-24% (`speed / (1 - gxwait)`): the GX side (core 2 +
  the vertex worker on core 3) caps the game today. A later stretch of the tp850 run (~300 s in) runs ~11% with core 0
  as the only limit.
- Core 2 (GX thread) waits for vertex-job ring space 22-25% of its time; core 0 waits for the GX side 9-16% at its
  polling syncs. A bigger ring trades the first for the second (live A/B, job 99).
- Core 3 (vertex worker): **62% gathering attributes from guest RAM**, 17% positions/normals, 11% texture coordinates,
  9% copy/cull/triangles (job 101).
- Core 0: ~13 ARM instructions per guest instruction; memory instructions ~40% of guest instructions; float and
  paired-single loads/stores are the most expensive per execution. Check hoisting: core 0 +5.7% (job 97, cutscene).

## Ranked levers (expected gain for TP, cost)

1. **Cache gathered vertices per display list (GX side; largest).** TP draws models by replaying display lists stored in
   memory: `J3DShapeDraw::draw()` calls `GXCallDisplayList(mDisplayList, mDisplayListSize)`, the list holds per-vertex
   records, and it is only rebuilt at load (`addTexMtxIndexInDL`) (**checked**, zeldaret/tp
   libs/JSystem/src/J3DGraphBase/J3DShapeDraw.cpp and J3DShape.cpp). Dolphin's OpcodeDecoding.cpp comment: "Zelda TP
   uses almost 100% display lists except menus" (SOURCED by the agent, not re-read). Skinning is matrix-index based
   (GPU blends, CPU only concatenates matrices), so the arrays a list indexes are mostly static frame to frame, and our
   worker re-gathers them every frame (62% of core 3). The PICA has one index per draw for all attributes (SOURCED:
   3dbrew GPUREG_INDEXBUFFER_CONFIG; Azahar vertex_loader.cpp), so the flattening must stay on the CPU, but it can be
   done once per (list, arrays) and reused. **Open:** how often lists and their arrays really repeat (J3DSkinDeform
   may deform some models on the CPU: INFERRED), and a cheap safe change check. **Next:** instrument on the 2DS: share
   of draws inside display lists, repeats of (list address, list hash, array ranges) per second.
2. **Paired-single and single-precision float math in single precision (core 0).** VFP11: FMULS / FMACS issue 1 per
   cycle, FMULD / FMACD 1 per 2 cycles; FADD 8-cycle latency either way; FCVT 4 (SOURCED, ARM DDI 0360F Table 19-16;
   matches tools/arm11_sched.py). Our JIT does all of it in double with conversions in between. Single-precision
   add/sub/mul/div/sqrt equal double-then-round-to-single (INFERRED: double has more than 2p+2 bits); Gekko's madd and
   the 25-bit frC cut need care. Combine with stage 2 of OPT_JIT_PLAN (FPRs kept in VFP registers within a block).
3. **VFP short vectors for paired singles (core 0, after 2).** LEN=1 (2-wide) single precision runs one element per
   cycle pipelined; s0-s7 destinations stay scalar; LEN is global state and FMXR serialises the VFP (SOURCED, DDI 0360F
   §18-19). Halves float instruction count (fetch), not FP cycles. Only for float-heavy blocks, LEN set once per block.
4. **Finish the optimizing JIT stage 1 and measure it at length.** Check hoisting is +5.7% core 0 in one run; the
   deferred byte swap and the float paths are in the same build. PPSSPP and Flycast avoid per-access checks the same
   way (mask into one window, per-register host pointers, constant addresses) (SOURCED: ppsspp
   Core/MIPS/ARM/ArmCompLoadStore.cpp; flycast core/rec-ARM/rec_arm.cpp). ARMv6 has **no MOVW/MOVT**: constants stay up
   to 4 instructions, so constant folding into immediates matters more than on ARMv7.
5. **HLE the per-frame matrix chain.** J3D's per-shape matrix work: J3DShapeMtxMultiConcatView::load and the
   loadMtxConcatView_* variants call MTXConcat per used slot; J3DDifferedTexMtx::loadExecute calls MTXInverse,
   MTXConcat, MTXCopy (SOURCED: zeldaret/tp J3DShapeMtx.cpp, not re-read). Measure their share with `--profile on` and
   hot_funcs.py before writing stand-ins; match by MEGA-style signatures with wildcards (SOURCED: Dolphin
   SignatureDB/MEGASignatureDB.cpp).
6. **Per-game emulated-CPU underclock as a fallback.** Dolphin's CPU clock override lowers the bar; some games tolerate
   large cuts, others break (SOURCED: forum excerpts only, no TP data). Would need its own measurement on TP.

## Answered (no longer open)

- **THP movies purple:** the PICA clamps every combiner stage; TP's YUV->RGB adds a negative register first. Fixed by
  the linear TEV path (e19200a); the 2DS's own register dump is a TEV test case. Y2R (hardware YUV->RGB) would also fit
  the planes (INPUT_YUV420_INDIV_8, 8-pixel multiples) (SOURCED: libctru y2r.h): a fallback only.
- **RunFast:** our FPSCR 0x03000000 (FZ, DN, round to nearest, no trapped exceptions) is the VFP11's full-speed mode
  (SOURCED: DDI 0360F FPSID / §20.2).
- **Shader hazard:** two consecutive `mova` freeze the PICA (SOURCED: picasso Manual). gxpass.v.pica has one `mova`,
  after an `ifc`: safe (**checked**).
- **Dolphin's ARMv7 JIT** depended on fastmem and was removed in June 2015 after its load/store paths broke (SOURCED:
  Dolphin commits 59e2225, b038b61): no ARM32 fastmem design to copy, and none works on the 3DS kernel for us.

- **Already measured on the 2DS (GPU_GUIDANCE.md, gcbench 4/4b/5; the research passes did not know them):** shader
  length is free (+128 MAD: no change) (rule 3); a uniform change between draws stalls the GPU ~2 us (rule 7); several
  draws share one draw window at 0.34 us CPU each (rule 20); WFE does not sleep in user mode (rule 17); the GPU does not
  slow CPU memory (rule 25); a 4th vertex attribute costs ~45% of the vertex rate (rule 2); integer attributes are as
  fast as float (rule 4). The published sources call most of these unknown: our own measurements outrank them.
- **Draw-call cost is not a lever now:** pica.c still draws through citro3d (3.25 us CPU per call against 0.34 in one
  draw window), but batching and merging leave ~1,100 GPU draw calls/s in TP's heavy rows (gpu_calls_s, jobs 93-98):
  ~0.3% of core 2.

## Ruled out (with the reason)

- **DSP as a coprocessor:** loads only RSA-signed firmware (SOURCED: 3dbrew DSP Services, snippet).
- **ARM9 at run time:** no homebrew path while an app runs (SOURCED: arm9loaderhax, Luma3DS docs).
- **MVD hardware video:** H.264 only, no JPEG (SOURCED: 3dbrew MVD Services).
- **AXI WRAM for hot data:** mapped kernel-only (SOURCED: 3dbrew System Flaws / Memory layout).
- **Large pages (64 KB / 1 MB) for the JIT buffer:** no user SVC takes a page size (SOURCED: 3dbrew SVC page).
- **L2C-310 prefetch tuning:** Secure-only register, no reachable SVC (SOURCED: DDI 0246H §3.3.14; 3dbrew SVC).
- **Vertex texture fetch / per-attribute indices on the PICA:** neither exists (SOURCED: picasso instruction list;
  3dbrew index-buffer register).

## Open: answered only by measuring on the 2DS

| # | Question | Decides | How |
|---|---|---|---|
| D1 | Share of TP's draws inside display lists; how often (list, arrays) repeat | lever 1 | counters in fifo.c/pica.c, live job |
| D2 | **Answered (qemu model, 40 TP fields):** float loads/stores 27.6%, float/paired-single maths 15.3% (43% float), integer loads/stores 29.1%, integer other 17.0%, branches 10.3% of modelled core-0 cycles | levers 2-3: single precision also removes the double<->single conversions in every float load and store | tools/arm11_sched.py --cycles over JIT_SCHEDDUMP, classified by opcode |
| D3 | Share of core 0 in J3D matrix functions | lever 5 | `--profile on` + hot_funcs.py with TP symbols |
| X1 | The Luma exceptions in jobs 99 and 102 | stability | arm9 dumps now reported (e19200a) |

## Sources the research could not read (re-try before relying on them)

Dolphin blog progress reports (403), Dolphin wiki (404), YAGCD THP page (404), FFmpeg thp.c (404), dynarmic design
doc, mupen64plus new_dynarec internals, citro3d buffers.h (503). Treat claims that rest on them as unverified.
