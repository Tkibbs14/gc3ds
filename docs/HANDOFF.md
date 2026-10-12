# gc3ds handoff (2026-10-10)

Start here in a new session. Measured facts are in [FINDINGS.md](FINDINGS.md), hardware references in
[ARCHITECTURE.md](ARCHITECTURE.md), and the next big piece of work in [GPU_BACKEND_PLAN.md](GPU_BACKEND_PLAN.md).
The repository is `Tkibbs14/gc3ds`, **private**. It goes public only when the owner says games run well.

## Where it stands

- **PC build (`core/build/gcemu`):** boots DOLs and GameCube disc images.
  - All 29 libogc test DOLs run (`core/tools/run_all.sh`).
  - Twilight Princess (GZ2E01) boots through its apploader and plays into the Ordon Spring opening cutscene: title,
    name entry, TV settings, dialogue.
  - Rendering is the software reference renderer (`gx/soft.c`), 13–25 s of wall time per emulated second for TP.
- **3DS build (`core/host3ds`):** runs on the New 2DS XL.
  - A menu lists the bundled test DOLs plus `sdmc:/gc3ds` and `sdmc:/gc3ds/games` (DOL/ISO/GCM).
  - JIT on by default (Y toggles it).
  - Stable: consoletest at 99.7% speed.
  - **GPU backend (`gx/pica/pica.c`, 2026-10-10):** GX draws, clears, EFB copies and the XFB run on the PICA200,
    with TEV compiled to combiners by `gx/tev_pica.c` (checked by `core/tests/tev/run_tev_tests.sh`). In Azahar:
    triangle 99.6%, texturetest 99.7% (was about 5%); NeHe 5 and 7 render. Not yet measured on the 2DS, and TP is
    untested on it. Known gaps are listed at the top of pica.c (fog, Z textures, indirect, depth copies, partial
    clears, mipmaps, textures in FCRAM rather than VRAM).
  - Test 3DS builds in Azahar with `core/host3ds/az_test.ps1 -Prog <name>` (build with autorun + screenshot). The
    TP image is in Azahar's virtual SD, but Azahar is too slow to get TP to its first frame.
- **CPU:**
  - Interpreter verified against qemu-ppc (`core/tests/cpu/run_tests.sh`).
  - JIT level 1 (`core/src/jit`): integer, load/store and branches native, the rest falls back per instruction.
    Verified by the differential fuzzer under qemu-arm (`core/tests/jit/run_jit_fuzz.sh`).
  - Idle `b .` loops and RAM polling loops (load/compare/branch back, e.g. TP 0x80341144) are skipped to the next
    event (`gekko_poll_loop_len`); on the PC that is 88% of TP's loading time.
  - The 3DS bottom screen shows a time profile (GX, disc, log, JIT compile, present, wait, guest; X toggles).
- **Not yet:** audio output (DSP is mail-level HLE only), memory cards (both slots empty), Wii (no MEM2, IOS, disc
  decryption).

## How to work (cheap and verified)

- **Builds:**
  - PC: `make -C core`.
  - ARM Linux under qemu: `bash core/tools/arm_build.sh run ...`.
  - 3DS: `bash core/host3ds/build.sh`.
  - Send to the 2DS: `bash core/host3ds/send.sh <ip>`, after HBL → Y.
  - Or copy `gc3ds.3dsx` to `sdmc:/3ds/` when the card is in the PC.
- **Regression gates:**
  - `run_all.sh` (images).
  - CPU tests.
  - JIT fuzzer, plus `core/tools/arm_jit_check.sh` (JIT frames equal interpreter frames).
- **TP debugging (PC):**
  - `--press BUTTON@from-to` drives the pad. Menus answer the stick (SLEFT, SRIGHT, ...), not the d-pad.
  - `--save-state F:PATH` / `--load-state PATH`. States for the title, menus and cutscene are in WSL
    `/root/tp_*.state`; they only load into a build with the same struct layout.
  - Other options: `--frames`, `--every`, `--dump-efb`, `--trace`, `--profile`.
  - Input sequence to the cutscene (60 fields per second):
    1. `A@200-206,A@400-406`: health and Dolby screens.
    2. `START@920-926,A@1100-1106,A@1250-1256`: title, no memory card, continue without saving.
    3. From the state at 2000: `A@2010-2016,SLEFT@2080-2086,A@2120-2126` (Yes, then name entry).
    4. From the state at 2300: `START@2310-2316,A@2340-2346,START@2450-2456,A@2480-2486,START@2600-2606,A@2630-2636`.

    The cutscene shows by field 2800–3300.
- **3DS rules learned the hard way** (each crashed something):
  - **CPU can't write VRAM.** Everything goes through GPU DMA.
  - **Present at most once per refresh.**
  - **JIT cache sync:** use `svcFlushProcessDataCache` plus `svc 0x94` only. Not svcBackdoor, and not Luma's ranged
    0x91/0x93.
- **The owner's PC:** a graphics-driver BSOD followed a GPU-rendered Azahar run. Azahar is now on its software
  renderer; ask before running GPU-heavy tools.
- **Never** download or commit game data. The owner's own images are on his SD card under `sdmc:/gc3ds/games`.

## Measured on the 2DS (2026-10-10, bottom-screen profile)

| TP stage | speed | where the time went |
|---|---|---|
| Health screen, logos | 68–69% | after the polling-loop skip; disc reads up to 57% while loading |
| Title scene (field ~700) | 3.0% | GX 46% (vertex decode + transform on the CPU 30%, textures 2%, GPU wait 4%); guest code 54% |
| Title scene, vfast + batching (0b29e22) | 3.1% | GX 38% (vtx 29.3%, tex 0.5%, gpu 0.9%); guest+other 61%; 8,300 draws/s |

- **After 0b29e22 the vertex share did not move** (29% for about 27,000 vertices per field, about 5.7 us each),
  although the new path does far less work per vertex; batching did cut the rest of GX from 16% to 9%. The
  title scene evaluates about 5.5 lights per vertex (sqrt and divides each) and reads 3.6 indexed attributes per
  vertex (PC counts, `VFAST_CHECK`). a3b6498 adds vertices/s to the screen so the cost per vertex can be read
  directly; if it stays near 5 us, measure the light loop and the attribute reads on the device next.
- **The title scene's guest code is small:** about 1.55 M guest instructions per field (PC, `OPMIX=1` from
  `/root/tp_900.state`), and about 15% of them are float / paired-single, which the JIT hands to the interpreter
  and its exact software float. a3b6498's counters (instructions, interpreted, blocks, slow loads and stores per
  second) split "guest+other" on the device.
- **a3b6498 feeds the GP in 32-byte write-gather bursts** instead of one parse attempt per 1-4 byte store (TP
  writes its command stream byte by byte through a pointer). Not yet measured on the 2DS.

- **The title scene issues about 4,400 draws of ~5 vertices per field** (PC: 1.3 M draws by field 900). The CPU
  vertex path costs about 7.5 µs per vertex on the ARM11.
- **Guest code alone caps the title scene near 5–6%.** The likely cause is float and paired-single code falling back
  to the interpreter one instruction at a time; check the JIT's dynamic fallback count first.
- **The pink/white title screen: fixed and confirmed on the 2DS (2026-10-10).** The castle, dunes and Link on
  Epona render. Against the PC reference, the orange distance haze and the dark far mountain are missing: GX fog
  is not implemented in pica.c yet (the next correctness item).
  - **Cause:** TP resets its depth buffer every frame with ~650 quads that replace depth from a uniform 4x4 Z texture.
    The PICA backend ignored Z textures, so every later draw failed the depth test. Uniform Z textures now set the
    vertex depth (`apply_ztex` in pica.c).
  - TP also makes a Z copy each frame for its haze; depth copies sample the depth buffer as a texture.
- **Checking the 3DS translation on the PC:**
  - `soft.c --pica-tev` renders with the TEV -> PICA translation (`tev_pica_sim.c`).
  - `core/tools/tp_tevcheck.sh N` logs the TEV programs that differ from the reference.
  - `MASK=hex` limits the PICA path to draws with those approximation flags.
  - `tp_ztex.sh A B` lists Z-texture draws.
  - Result: the TP title frame with every draw through the PICA model is recognisable, but its haze/bloom
    composite is stronger than it should be. Four TEV programs differ, with approximation flags REGS 40, RANGE 10,
    STAGES+TEXUNITS 03 and BIAS 80; they are the next targets for `tev_pica.c`.
- **Tools:**
  - `core/tools/tp_frame.sh N` renders TP field N on the PC.
  - `tp_copies.sh A B` lists TP's EFB copies.
  - `tp_profile.sh S` gives guest hot spots.

## Reading 2DS runs without photos (26ffafc)

**Read `sdmc:/gc3ds/perf.csv` (E:\gc3ds\perf.csv when the card is in the PC) before asking the owner for photos.**
- Each run starts with a `# run` line (program, measured CPU clock, JIT on/off) and a `# cores` line: how much work
  threads on cores 1-3 get done compared with core 0.
- Then one row per second:
  - speed, fields emulated / shown / dropped, average and worst host time per field;
  - every profile area;
  - JIT counters, compiles, poll skips and code-buffer use;
  - GX and disc bytes;
  - GPU batches, submits, splits, texture decodes, EFB and XFB copies, clears;
  - citro3d CPU and GPU ms per frame;
  - free heap, linear memory and VRAM.
- Every 10 s: the most frequent interpreted instructions and the CPU clock.
- Screenshots of the top screen as the 2DS drew it: `sdmc:/gc3ds/shots/shot_NN.bmp` (400 x 240, ~290 KB) at 5 s,
  15 s and then every 30 s of a run, in a ring of 16 (`next.txt` holds the next slot), so the card never holds more
  than ~4.6 MB of them. Each one has a `# shot` line in perf.csv with its real and emulated time. The GX thread
  copies the screen with GX_TextureCopy after the present and writes the file at the next one (`pica_capture`).
  Orientation checked against Azahar's own screenshot of graphics_gx_texturetest.

## Measured on the 2DS, 2026-10-11 (build cbff68c; perf.csv copy: core/out/perf_2026-10-11_jit2.csv, not tracked)

- **TP title scene: 9.2% speed with linking, 8.0% with linking off in the same run** (7.2-8.7% the day before).
  With linking **core 0 waits for the GX thread 53% of the time** (15% without): the CPU side is no longer the
  limit. The GX thread on core 2 is 78% busy: vertices 39% (gather 11%, lighting 2%, the rest position, texgen,
  assembly), GPU sync 14%. Next limit: the GX thread.
- **hw.txt JIT benchmarks** (ns per guest instruction, linked / unlinked): ALU loop 4.5 / 66, memory loop 7.6 / 59,
  call+return 12.2 / 190, compare+branch 7.7 / 130; straight code inside the 16 KB I-cache 1.6 ns (1.3 cycles),
  beyond it 5.5 ns with one I-cache miss per ~6 guest instructions (~19 cycles each, served by the L2); 256k
  instructions (1.2 MB of code) 7.0 ns.
- **Counters:** per core (a 20M-instruction loop reads 20,013,335 instructions, 804 MHz). Title scene, core 0: IPC
  0.36, 6.4M I-cache misses/s, 143M cycles/s stalled for instructions, 188M for data hazards, 383k mispredictions/s.
- Fallbacks left at run time (10 s): cror 215k, psq_l / psq_st ~47k each, mtspr 27k, frsqrte 19k, fres 17k, rfi 15k.

## Title scene on the 2DS through 2026-10-11 (each line its own run; A/B halves of one run where given)

| Build | Change | Title scene |
|---|---|---|
| 4bf5279..bb7267c | JIT level 2: linking, register cache, guest time | 9.2% (linking off 8.0%) |
| ab635d0 | vertex worker on core 3 (kept triangles only) | 9.9% (off 8.3%) |
| e4fa5a1 | no CPU light setup for shader-lit draws | 11.6% (worker off 9.4%) |
| 29fce90 | batch key and vertex setup cached against register versions | 12.4% |
| bb18017 | slimmer lighting shader | 12.6% |
| 1d45007 | citro3d's GPU queue started at each split | **15.1%** (off 12.2%) |
| 82a091b | CR logic ops; indirect table 4096 entries, wiped on every block-key change | 12.9% (regression) |
| b439173 | one indirect table per block key (4 x 2048, switched, not wiped); quantized psq; fres / frsqrte | 15.0% |

### ARM11 MPCore facts from the TRM (ARM DDI 0360F, read 2026-10-11) and what they mean here

- Branch prediction: a 128-entry direct-mapped BTAC (2-bit history, zero-cycle hits, folding in ARM state), then a
  static predictor (backward taken, forward not taken; a statically predicted taken branch costs one cycle).
  **Only branches with an immediate or fixed PC-relative target are predicted.** `BX r12`, `LDR pc, [r12...]` and
  `BLX <reg>` are not (BLX reg still pushes the return stack). The return stack has 3 entries and predicts only
  unconditional `BX r14`, `LDM sp!, {..pc}` and `LDR pc, [sp..]`.
- So the dispatcher's indirect jumps, `call()`'s `blx r12` and ST_INDIRECT's jump are never predicted. But the 2DS
  counters show ~0.7M mispredicted branches/s against 770M cycles/s (under 1%): **branches are not the problem,
  instruction fetch is** (~11M I-cache misses/s, ~310M stall cycles/s, about 40% of core 0).
- Our layout already agrees with the static rule: slow-path and exit branches go forward (predicted not taken).
- Thumb-2 and MOVW/MOVT do not exist on ARMv6K: constants cost up to 4 instructions (`load_const`).

### Code layout (2026-10-11)

`JIT: kept blocks, host words per guest instruction` (qemu, TP 150 fields): **8.2 hot, 2.7 exit stubs, 17.1 cold**.
Cold code was appended after each block, so two-thirds of the bytes between consecutive hot paths were never run.
Since this change the code buffer has a hot region (stubs, hot paths, exit stubs: the first 40%) and a cold region
(slow paths: the rest); `base` in compile_block is the cold code's index relative to the block. perf.csv prints the
same breakdown (`# jit words per guest instruction`). In the same session: `psq_st` to the write-gather pipe is
compiled (it was ~8k fallbacks/s), and mfspr / mtspr HID0 are plain (the GC SDK's `sc` sync handler at 0xC00 reads and
writes HID0 twice per call, ~2.5k/s). `# spr fallbacks 10s` in perf.csv names the SPRs still interpreted.

**Refused compiles are silent and expensive:** a block whose compile fails is interpreted whole and recompiled on
every visit. The pipe `psq_st` path first pushed TP's vertex-submission blocks past the 256 fix-up entries (1,090
refusals in 150 fields, seen only as "instructions compiled" doubling); `exit_snap` had no bound at all. Both tables
are larger now, a full table refuses the block, and the qemu summary prints `compiles refused: hot, cold, fix-ups`.
Check that line after any change that adds branches or exits per instruction.

### The GX thread is the second limit (2026-10-11, build b439173)

With core 0's outside-JIT time down to 18%, core 0 waits for the GX thread 17% of the time (`gxwait_pct`; it was
2%). Core 2 is busy 75%: vertex path 47% (decode 20%, lighting 4%), state 15%, textures 6%, sync 6%. TP's title scene
sends ~44k draws/s of ~5 vertices (the PC build: 4.67M draws, 23.2M vertices in 1,500 fields), about 17 us of core 2
per draw. The vertex setup cache hits 99.1% (setups for 0.9% of draws), so the cost is fixed per-draw work.
perf.csv's `# gx draws 10s` line (after b439173) splits draws by batch path (fast / key equal / new batch, with what
changed) and counts vertex setups with their causes: read it before changing the per-draw path in pica.c / vfast.c.

Build 5328b90 on the 2DS: 14.8%, core 0 waiting for core 2 22% of the time. The waits are the game's own GPU syncs (PE
register reads and idle loops call `sync`, which drains the GX thread): core 0 waits at the end of a frame and core 2
idles at the start of the next. Card run data: 99.0% of draws take the batch fast path; setups for 0.9%.
**Since then (this commit):** shader-lit draws (99% of TP's) leave the vertex gather (all reads of guest memory, 20%
of core 2) to the vertex worker on core 3: `vfast_prepare(..., defer)` reads only the matrix indices and copies the
stream into the job, `vfast_finish` gathers. The worker reads guest memory later than the GX thread did, so the GX
thread waits for the worker before posting a PE token / finish, and `wait_drained` also waits for it
(`pica_vertex_worker_idle`). The PC path defers too, so `vfast_check.sh` checks it (TP 1,500 fields: 4,672,789 draws,
3,708 over tolerance, as before). The batch fast path no longer copies the 384-byte key per draw.
**Measured on the 2DS (6673eb2): slower, 14.2%.** vdec fell from 20% to 2% of core 2, but the GX thread then waited
for the worker 18% (`vwait_pct`) because each batch draw (~445/s) waits for the worker to finish the batch, and the
worker went from 35% to 51% busy; core 0 waited 26%. Switched off (`kVwGather` in pica.c; the code and its PC check
stay). Moving work between cores does not help while every batch flush is a join.

**The GPU is mostly idle:** citro3d reports ~20 ms of GPU drawing per frame against ~190 ms of CPU work
(`c3d_gpu_ms` / `c3d_cpu_ms`). The real lever is to give the PICA's vertex units the position transform (raw
positions and per-draw matrices as uniforms), as Dolphin does, instead of transforming every vertex on the CPU.

### Research digest (2026-10-11, five Haiku agents; facts marked SOURCED by them, TRM read here)

- **Next big GX step:** Dolphin transforms on the GPU and uploads XF matrices only when a change tracker says so; the
  CPU never transforms. PICA vertex shaders could do the same (uniform limits unverified). Dolphin's developers trace
  TP's Hyrule Field slowdown to draw count (the minimap); Dolphin ships a two-instruction "Hyrule Field speed hack"
  (`GZ2E01.ini`) and per-game manual vertex culling. A game patch is an owner decision.
- citro3d writes ~13 GPU registers per `C3D_DrawArrays` (mode toggles and a post-vertex cache clear each time); the
  `C3D_Sync*` copies split the frame and wait.
- **JIT:** Dolphin's ARMv7 JIT reported "fairly large performance gains" from caching FPRs in VFP registers (PR
  #1542); we load and store FPRs per instruction. ARM11 MPCore TRM (DDI 0360F ch. 15/19): LDR result latency 3 (a
  dependent op stalls 2, 3 if used as a shifter operand or base); REV latency 1; VFP add/mul latency 8/9, FLDD 4,
  FMRS to ARM 2, FMSR from ARM 4, FDIVD 29+ cycles blocking; register-target branches are never predicted (5-7
  cycles); L1 4-way, 32-byte lines, round robin; store buffer forwards to loads.
- **Already right:** threads on cores 2 and 3 (hw.txt), 804 MHz + L2 (`osSetSpeedupEnable`), FPSCR round-to-nearest
  with flush-to-zero and default NaN (hw.txt `FPSCR 23000010`; threads start rounding to zero, main and the GX/worker
  threads set it), mispredicts under 1% of core 0.

- **Core 0 is the limit again** (waits for the GX thread ~10%). Its counters on the title scene: 769M cycles/s,
  251M instructions (IPC 0.33), **323M cycles stalled fetching instructions (42%)**, 11.8M I-cache misses/s (~0.85 per
  guest instruction, ~27 cycles each), 127M cycles of data-hazard stalls. The sampling profiler (da2aefd, `# hot guest
  code` lines) shows no hot spot: the top block is 0.2-0.3%, the top 16 about 2%, 14-20% outside compiled code. So
  what counts is the code footprint per guest instruction (compiled code and the C code core 0 runs next to it), not
  one routine. Fallbacks left: cror ~40k/s (compiled since), psq_l / psq_st with quantized GQRs ~15k/s, frsqrte and
  fres ~6k/s (Gekko's estimate tables), mtspr / mfspr ~5k/s.
- perf.csv's `# dispatches/s` line (after da2aefd) splits the returns to the C dispatcher (~174k/s on the title scene)
  into link requests, indirect-table misses (blr / bctr; the table is now 4096 entries), budget ends and others, with
  gekko_run slices per second.

## Vertex worker on core 3

- **2DS, 7f2033e (degenerate triangles):** worker on 8.83% vs off 8.62%. Writing culled triangles as degenerate
  ones (to fix each draw's arena place when queued) tripled the GPU's time per frame (87 vs 30 ms): the lighting
  vertex shader still runs for every vertex. GX-thread vertex time also grew (51% vs 44%) with a 4 MB job ring.
- **Since then:** the worker appends only the triangles it keeps (`g_vwPos`); the GX thread syncs with it
  (`vw_sync`) before recording a batch's draw call, writing the arena itself (`need`) or handing the arena to the
  GPU; worker draws reserve arena room without waiting (`need_work`, `g_vwReserved`). The job ring is 256 KB (stays
  in the L2). Not yet measured on the 2DS. NeHe 7's crate shows diagonal seams in Azahar with the worker on and off
  alike (an older rendering artifact, to look at separately).
- 2DS logs of that run: core/out/perf_2026-10-11_vw1.csv (not tracked). Its log.txt has four "GX thread: no
  progress for 2 s" warnings; the run before (no worker) had the same ~1 s stalls after a JIT compile spike at the
  title scene's loop point (jitc 60-66%, then GX 99%): look at them next.

### The worker's design (7f2033e)

- `vfast_run` is now `vfast_prepare` (GX thread: everything that reads the stream, RAM, CP and XF state: gather,
  lighting, and copies of the matrices the vertices use) plus `vfast_finish` (positions, shader-lighting inputs,
  texgen from the job alone). VFAST_CHECK on TP to field 760 gives exactly the numbers of the unsplit code (370 of
  652,863 draws over tolerance, max pos error 2.7e-4, the old float-precision cases).
- pica.c queues each triangle draw's second half (finish, Z-texture depth, culling, triangle assembly) to a worker
  thread on core 3 (`vw_*`): the draw's arena range is fixed when queued (culled triangles become degenerate ones),
  job memory is a 4 MB ring freed in order, `flush_arena` (the only place the GPU gets the arena) waits for the
  worker. Lines and points stay on the GX thread. Profile areas `vwork` (core 3 busy) and `vwait` (GX thread
  waiting for it). perf.csv's A/B now alternates the worker every 20 s (`vworker` column); linking stays on.
- Azahar: NeHe 8 and NeHe 11 render correctly through it.

## JIT level 2 and the ARM11 counters (2026-10-11)

On the title scene core 0 ran ~8.0M guest instructions/s in ~1.3M blocks/s (~6 instructions per block, ~650 host
cycles per block): the level-1 dispatcher (C hash lookup, 10-register save/restore, return per block) dominated.
- **Block linking** (`jit.c`): shared stubs at the start of the code buffer (ST_ENTER/EXIT/LINK/DYN/BUDGET/INDIRECT)
  keep RG, RRAM, RDC (budget left), RFAST and RSTOP in registers. A constant exit is a link site that the dispatcher
  patches to the target block once compiled; blr/bctr go through a 512-entry table (`fast`, valid for one block key);
  every block starts with `cmp r9, #0; ble`. Never linked: polling-loop blocks (the dispatcher must see each round),
  idle blocks, and exits after instructions that change the block key (mtmsr, mtspr HID2, ...). A flush bumps `gen` so
  a pending link from before it is dropped. qemu: test DOLs return to C once per ~50 instructions (was 6).
- **Native now:** mfmsr; mtmsr (stays in the block unless more than EE changes or EE is set with an interrupt pending:
  `jit_mtmsr`); dcbst/dcbf/dcbi/dcbz on MEM1; sync, eieio, dcbt, dcbtst; isync (ends the block, linkable); mfspr/mtspr
  of SRR0/1, DAR, DSISR, SPRG0-3, GQR0-7. These were TP's top fallbacks (dcbi ~150k/s, mfmsr/mtmsr ~40k/s each).
- **Compare + branch fusion:** CR updates no longer clobber the ARM flags (SO is or-ed in with a shift), and a `bc`
  right after the compare that set its field branches on the flags.
- **Register cache** (replaced the store-to-load forwarding of 4bf5279; CR and XER and the fifth slot r5 since
  b3275ef): guest GPRs, CR (id 32) and XER (id 33) live in r4-r8 from their
  first use in a block (`src`/`dst`/`dst_done`, LRU, pinned per instruction), written back when dirty at every exit
  (each exit stub carries a snapshot), before every interpreter fallback (hot code then forgets the cache, cold code
  reloads it) and at branches. Update forms keep the effective address in r0 (slow paths push it around their helper
  call). **Rule:** no cache allocation between a branch into cold code and that cold code (the cold path would skip an
  eviction's write-back); psq computes its address before its GQR check for this reason. LR and CTR stay in memory.
  There is no stop-flag register any more: slow paths keep the stop status in r3 and every memory instruction has a
  tail (`set_tail`, lfs's FPR in `tailFpr`) that finishes it when it must leave; a slow path without one refuses to
  compile the block.
- **Interpreter fallbacks** go through the shared stub ST_INTERP (`bl` + pc, instruction and instruction count as
  literals; 4 words instead of ~20).
- **Guest time inside compiled code:** `g->cycles` only advances when a run returns, so helpers (MMIO, the
  interpreter: timebase, decrementer, event scheduling) used to see time frozen for a whole run. Compiled code now
  stores its budget left in `g_jitLeft` before each helper call (`emit_time`) and the helpers run with the exact time
  (`time_enter`/`time_leave`). Found because TP under the qemu JIT reached field 500 with 739 draws and 15,262 DSP
  mails where the qemu and PC interpreters both had 1,270 and 28,051 (linking on/off identical: not the linking).
  **After the fix (e689553) the JIT matches: 28,051 mails, 1,268 draws** (the last few from runs overshooting their
  budget by up to one block), with 3.5M dispatches instead of 578M and 9.9 host words per guest instruction compiled
  instead of 23.6; qemu took 23 min instead of 32. Check a JIT change on TP by running
  `qemu-arm -cpu cortex-a7 build-arm/gcemu ISO --frames 500 --jit --press A@200-206,A@400-406 --out DIR` (~25 min)
  against the interpreter (WSL `/root/tpC/out.txt`, the same command without `--jit`): the DI, DSP and GX counts on
  the last lines must agree.
- **Cold code:** memory slow paths (MMIO, unaligned, faults), psq/dcb fallbacks and the pipe path are emitted into a
  second buffer and appended after the block, with branches fixed up (`cold_begin`/`cold_end`/`fix`). Fast paths fall
  through. The working hypothesis is that TP's JIT is bound by instruction fetch (1.5M guest instructions per field,
  ~10 host words each, against a 16 KB I-cache and 2 MB L2); the counters below test it.
- **Write-gather pipe:** compiled stores to exactly 0xCC008000 call `gp_wgp_put` (`jit_set_gather`) instead of
  translation + bus + MMIO (TP: ~136k stores/s).
- **Checks:** `run_jit_fuzz.sh` now also generates the new instructions, pipe stores, user mode and two-instruction
  blocks (so forwarding and fusion are exercised), with planted bugs 3 (wrong fused condition) and 4 (wrong forwarded
  register) that must be caught. `JIT_FUZZ_TRACE=1` prints each case and saves each compiled block to
  /tmp/jit_last.bin; `gdb-multiarch` is installed in WSL (`qemu-arm -g PORT` + `target remote`).
- **perf.csv:** `disp_k` (was `blk_k`) counts entries from the C dispatcher; `jit_link` and `links_s` follow the
  automatic A/B (linking alternates every 20 s, `LINK_AB_SECS` in main.c, so one run compares both).
- **ARM11 performance counters** (`host3ds/pmu.c`, svcControlPerformanceCounter): perf.csv gets `pmu_mcyc` (core 0
  cycles/s) every second, two core events per second in rotation (instructions, mispredicted branches, I/D cache
  misses, stalls for instructions / data hazards / LSU, D-cache write misses, main TLB misses, branches), and the SCU's
  line-fill misses from cores 0 and 2 and external reads/writes. hw.txt has a self-test (a 20M-instruction loop and a
  4 MB read) that shows whether the counters are per core and what clock they count. Azahar does not emulate the SVC
  (its values are garbage).

## GX on core 2 (068ae2f), measured on the 2DS

| TP title scene | GX inline | GX on core 2 |
|---|---|---|
| speed | 4.2-4.9% | **6.6-8.2%** |
| core 2 (GX thread) busy | n/a | 67-86% |
| core 0 waiting for GX | n/a | 15-37% |

- **The GX thread is now the limit.** Vertices take 49-61% of wall time on core 2 (lighting 25-30%), while the
  PICA draws in ~2.4 ms per frame.
- **7b57b16 moves lighting to the vertex shader; it has not yet been run on the 2DS.** The shader is
  `gxpass.v.pica`, generated by `tools/gen_gxpass.sh`. VFAST_CHECK compares the C model
  `vfast_gpu_light_model` with the reference. X in the menu switches back to CPU lighting.
- **Shader traps found in Azahar:**
  - Clamp reciprocals: the PICA flushes tiny values to 0, and 0 * inf is not 0.
  - Don't use bool uniforms through citro3d; use a float flag and `cmp`/`ifc`.
  - libctru numbers bool uniforms from 0x68.
- **Still to try: the transform and texgen in the shader too.** They need per-draw matrices, which break batching. The CPU then only gathers and byte-swaps
  attributes into a PICA-readable buffer. Matrices and lights go in as uniforms; per-vertex matrix indices use
  relative addressing (a0).
- **After that, core 0 (CPU emulation) is the limit:** JIT register caching and block linking.
- **Self-test numbers** (`hw.txt`; a copy is in core/out, not tracked):
  - tick read 0.25 us;
  - spin handoff core 0 <-> 2: 0.9 us;
  - LightEvent across cores is unreliable;
  - synthetic vertex: 1.1 us unlit, +0.3 us per light;
  - random reads: 34 ns within 1 MB, 210 ns across 16 MB.

## First perf.csv run (2026-10-10, a copy is in core/out/perf_2026-10-10_run1.csv, not tracked)

- **Clock:** 268 MHz at start, 804 after `osSetSpeedupEnable`. The clock was never the problem; the loop estimate
  reads 5% high.
- **Cores:** threads on core 1 / 2 / 3 do 75% / 95% / 92% of core 0's work. **Core 2 is available nearly at full
  speed.**
- **TP title scene:**
  - 4.2-4.9% speed, about 330 ms per field, alternating ~650 ms and light fields (TP runs at 30 fps).
  - The field splits about 50% GX work on the CPU (vtx 38%, of which lighting 19%) and about 48% CPU emulation.
  - About 12.8k draws/s become ~140 `C3D_DrawArrays`/s.
  - GPU drawing time is about 2.3 ms per frame, so the PICA is nearly idle.
  - VRAM has about 1.2 MB free. The JIT code buffer is 91% full and still growing; a full buffer drops all code.
  - `c3d_cpu_ms` is meaningless here, because our frames span a whole field.
- **Next: GX on core 2, as Dolphin's dual-core mode.** The CPU thread on core 0 queues write-gather bursts and
  display-list calls into a ring buffer. A GX thread on core 2 parses them, runs vfast and does all citro3d work,
  including present. Sync points are CP/PE register reads, PE token and finish interrupts, EFB peeks and copies the
  CPU reads, and boot/reset. Expected gain: up to ~2x where GX and CPU are balanced, as in the title scene.
  - Check `hw.txt`'s handoff latency first (written by 0611f54 at startup).

## Late 2026-10-10: the title scene's two biggest costs, fixed but not yet measured on the 2DS

| Item | Measured on the 2DS (eb867fd) | Change |
|---|---|---|
| Float / paired-single fallbacks (`fpfb`) | 50.5% of the time, ~1.1 us each | b614a7a: compiled on VFP ("fast float"); 97.1% native under qemu |
| Lighting (`lit`) | 17.8% (~600 cycles per light evaluation) | 3979659: light by light, four vertices interleaved, no compares in the loop |
| Gathering vertex attributes (`dec`) | 3.9% | — |
| Vertex position and texgen (rest of `vtx`) | ~9% | next candidate |

- **Still interpreted:**
  - `psq_st` to the write-gather pipe (non-RAM address);
  - cache instructions (`icbi`, `dcbi`);
  - `mfmsr` / `mtmsr`.
- **Save states (38bf728) store events by index,** so a state now survives code changes. A struct change still
  invalidates it; remake it with `tools/tp_run.sh --frames 901 --press A@200-206,A@400-406 --save-state 900:...`.
- **qemu cannot catch VFPv3-only encodings.** Its `-cpu cortex-a7` accepts VFPv3, and the ARM11 only has VFPv2.
  Check new VFP encoders by hand (b614a7a's are all VFPv2, d0-d7 only).

## NEXT SESSION: move everything we can onto the GPU (owner's direction, 2026-10-11)

The PICA200 draws for ~20 ms of each ~190 ms frame; cores 0, 2 and 3 are the limit. James: "try absolutely
everything with the GPU". Take these in order, each as its own commit, each measured on the 2DS (one card run per
step) and checked against the reference (`core/tools/vfast_check.sh`, NeHe and TP in Azahar on the software
renderer only):

1. **Position transform in the vertex shader: built, checked on the PC and in Azahar, not yet measured on the 2DS.**
   - `gxpass.v.pica` does `proj * (pal[v0.w] * v0.xyz)` when `lon.w = 1`, and lights from that eye position. The
     palette is 14 matrices (`pal[42]`, 95 of 96 float uniforms used), with the slot index in v0.w (one `mova`).
   - pica.c (`pal_assign`, `flush_sub`): a slot holding the same matrix is reused, else the next free one. When a
     slot the open draw call uses (or the projection) must change, the batch's vertices so far are drawn first.
   - The GPU culls (`kCull`; the cull mode is in the batch key), so each draw's arena range is fixed when queued.
     Closing a batch no longer waits for the vertex worker; only `flush_arena` does (`vw_sync_all`).
   - Lines, points and Z-texture draws keep the CPU path. So does a draw with more than 14 matrices
     (`draw_cpu_alone`, counted as "draws over the palette").
   - **Float24 precision:** see GPU_GUIDANCE rule 29. Positions go relative to a grid origin folded into the matrix
     (`vfast_job_origin`). TP to field 1,500 (`vfast_check.sh 2 1500`): 0.114 texels at most, against 1.48 without
     the origin. Depth is up to 299 units of 2^24 off (CPU path 220). Watch for z-fighting where a GPU draw and a
     CPU-path draw (Z texture, lines) overlap.
   - **Card run:** perf.csv alternates the transform every 20 s (`gpupos` column), with a `# gpu transform 10s`
     line (palette uploads, extra draw calls, overflows). Test builds fix it with `GPUPOS=0|1` (`az_test.ps1
     -GpuPos`); `az_test.ps1` now also saves the program's own screenshot (`*_shot.png`), which works whatever
     window is in front.
   - **2DS, 29a22f7 (core/out/perf_2026-10-11_gpupos.csv, title scene, 80 / 98 rows):** slower. Transform on 11.3%,
     off 12.7%. Core 2's vertex share did not drop (50 → 56%): its cost is per draw (~12 us of ~20 us per draw,
     the gather ~5 us of it), not the vertex maths, which ran on core 3 (34 → 30% busy). Core 0 waited for core 2
     28 → 36%. `gxsync` 7 → 14%: the GPU shades back faces too (30 → 40 ms per frame), and C3D_FrameBegin waits for
     everything after the frame's last split. Palette: ~2,150 uploads/s, 116 extra draw calls/s, no overflows.
     (That run printed `gpupos` twice per row, so its PMU columns are shifted by one.)
   - **Since then (not yet measured):** GPU-transformed shader-lit draws are gathered on core 3 (deferred jobs;
     their batches never wait for the worker, and only PE tokens / finish wait for the reads, `g_vwDeferEnd`).
     The origin of a deferred job comes from its first vertex alone. A one-matrix draw repeating the previous
     draw's matrix and origin skips the palette search. The frame is split every 8,192 arena vertices so the GPU
     starts sooner (`split_early`). Arena and texture cache flushes use `svcFlushProcessDataCache`. perf.csv
     cycles four 20 s phases: both on, transform off, splits off, both off (`gpupos`, `esplit`).
   - **2DS, 24e4585 (core/out/perf_2026-10-11_split4.csv, ~80 s of title scene, one 20 s sample per phase):**
     inconclusive for speed. The phases fell on different parts of the title loop (39k-48k draws/s), so speed
     moved with the scene, not the setting. Mechanisms did show: early splits cut `gxsync` to 0.3-2% and core 0's
     wait for core 2 from ~27% to 8-12%; the deferred gather cut `vdec` on core 2 from 20% to 0.2-2% (core 3 48-60%
     busy). But with both on, each early split waited for the worker (`vwait` 10.5%).
   - **Since then:** phases are 5 s (each setting samples the whole loop), an early split happens only while the
     worker's queue is empty, and core 2's vertex time is split into `vprep` / `vpal` / `vqueue` columns.
   - **2DS, eee65ec (core/out/perf_2026-10-11_ab5.csv, ~120 rows per phase, standard error ~0.2):** CPU
     transform + early splits **13.77%**, baseline 12.20%, GPU transform 12.01% (no splits) / 11.91% (splits).
     Early splits win (+13%; core 0's wait for core 2 20 → 6%); the GPU transform loses (GPU time 25 → 36 ms per
     frame, core 2 trades the gather for `vqueue` 11% and `vpal` 6%). Core 2 on the winner: `vprep` 50% of its time
     (gather `vdec` 21%), ~18 us per draw.
   - **Since then (not yet measured): indexed draws** (`pica_set_indexed`, `vw_do_indexed`): each draw's vertices
     once at a fixed arena place, triangles as u16 indices (the vertex buffer start moves along the arena,
     `set_vbase`), culled triangles as one repeated vertex. Every draw's ranges are known when queued, so the CPU
     path also leaves the gather to core 3. Early splits fire with up to 4 draws queued. Azahar: NeHe 5, 8 and 11
     correct; NeHe 11 also with `VBASE_SPAN` at 2000 (rebasing several times a frame). perf.csv phases: indexed x
     gpupos (`indexed`, `gpupos` columns), early splits always on.
   - **2DS, 862fe63 (core/out/perf_2026-10-11_idx.csv, ~105 rows per phase, standard error ~0.27):** indexed +
     GPU transform **14.56%** (now the default), not indexed / CPU transform 13.93%, indexed / CPU 13.75%, not
     indexed / GPU 12.93%. With the default, core 0 waits for core 2 only 2.8%: **core 0 is the limit now.**
     Its counters (each pair is live one second in five; average only those seconds): 767 Mcycles/s, IPC 0.31,
     11.7 M I-cache misses/s, **340 M cycles/s (44%) stalled for instructions**, 120 M for data hazards, D-cache
     read misses only 0.56 M/s.
   - **Core 0's hottest code on the title scene** (`# hot guest code`, ~30% of samples): 0x8007f9a4
     `dBgWKCol::ShdwDraw` (zeldaret/tp `config/GZ2E01/symbols.txt`, `src/d/d_bg_w_kcol.cpp`), the real-time
     shadow projection onto terrain collision: a grid walk plus per-triangle PSVEC calls (`PSVECCrossProduct`
     0x803471b4, `PSVECDotProduct` 0x80347194, `PSVECScale` 0x803470d8, `PSVECAdd` 0x80347090). The PC build prints
     guest code with `DISASM=start:end`.
   - **PC count (`MERGE_STATS=1`, TP to field 1500):** 76% of draws could join the previous draw's vertex job (same
     format, state, arrays and matrices); 99.6% of draws are triangle strips. Merging is the next core 2 lever
     when a scene makes core 2 the limit again.
   - **Since then (not yet measured): JIT memory-access stubs** (`emit_mem_stubs`, `jit_set_mem_stubs`): each load
     and store calls a shared stub for its MEM1 fast path (2 words per site instead of ~9). JIT fuzzer 20,000 cases
     0 fails, `arm_jit_check.sh` frames identical. perf.csv A/B: 20 s phases (`memstub`; each switch recompiles,
     so skip each phase's first seconds).
   - **2DS, 3bffa35 (perf_2026-10-11_memstub.csv, ~160 rows per phase):** memory stubs on **15.26%**, off 14.72%;
     I-cache misses 12.6 → 9.5 M/s, cycles stalled for instructions 358 → 283 M/s. On.
   - **2DS, a5318cb (perf_2026-10-11_pipefast.csv, TP run at line 5744, ~165 rows per phase):** inline pipe
     stores on 14.69%, off 14.16% (+3.7%): on. Both halves fell below 15.26% because the indirect-branch tables
     went from 4 x 2048 to 2 x 8192 entries: TP uses more than two block keys, so tables were reset 186 times a
     second (was 0), misses rose 19.6k → 24k/s and time outside compiled code 9 → 26%. Now 4 x 4096.
   - **2DS, 2bbe8ab (perf_2026-10-11_fixed.csv, 373 rows, no A/B):** 15.11% (standard error 0.16), level with
     15.26%; indirect misses 13.9k/s, no table resets, 7.4% of core 0 outside compiled code.
   - **Since then (not yet measured): native stand-in for TP's shadow scan** (`src/cpu/hle_tp.c`, `hle.h`): the
     cell / octree / triangle-list loops of `dBgWKCol::ShdwDraw` (0x8007fb48..0x8007fee8) run natively from the loop
     entry or the next-triangle step until a new triangle (0x8007fce4, guest code) or the end (0x8007fee8), counting
     the guest instructions they stand in for. Recognised by an FNV-1a hash of those bytes. The JIT compiles a hooked
     PC as a call (`jit_hle`) and ends other blocks before it; the interpreter checks the two PCs. `HLE_CHECK=1`
     (PC) runs every call natively and in the interpreter: TP to field 2500, 6,105 calls, 0 differences (GPRs, CR,
     XER, PC, instruction count, 2,304-byte frame). JIT fuzzer 0 fails, `arm_jit_check.sh` identical. perf.csv A/B:
     `hle`, 20 s phases; `# hle 10s` counts runs and instructions stood in for.
   - **2DS, 9d7c239: the stand-in froze TP at the first Link / Epona close-up** (speed read 85% with no draws): compiled
     blocks link to each other without writing g->pc, and `jit_hle` read the stale one. Fixed (17799fe): the block passes
     its PC. The first check ran the interpreter only, which never exercises that path; the JIT path was then checked under
     qemu (`HLE_CHECK=1 qemu-arm ... --jit`, TP to field 1100, 7,735 s): 1,235 calls, 0 differences. Re-enabled in the A/B.
   - **2DS, f7e63ab (core/out/perf_2026-10-12_hle.csv), stand-in on vs off:** the run has two scenes. In the title loop the
     scan hardly runs (~5k instructions per 10 s), so on/off there is noise. In the Link / Epona cutscene (from ~437 s) it stands
     in for ~36M instructions a second, and there the **game's field rate fell 5.02 -> 3.85 /s while the cycle-based speed
     rose 8.39% -> 9.67%**. Cause: one stand-in call could run up to 4M instructions (MAX_INSNS) regardless of the slice
     (`sys_run` slices are 1M, and its `end` is recomputed each call, so overshoot is not made up), which also delays events.
     Fix: `hle_run(g, budget)` stops at the next loop head once the caller's budget (JIT: RDC; interpreter: min(downcount,
     limit - cycles)) is used up. PC `HLE_CHECK`: 6,129 calls to field 2500, 0 differences. JIT-path check under qemu
     (`HLE_CHECK=1 ... --jit`, TP to field 1100, 8,994 s; `/root/hlejit2.txt`, also wrote `/root/tp_arm_850.state`):
     **1,236 calls, 0 differences.** Back in the A/B as a run option: `--ab hle` (relay job 6, from the field-850 state).
     Judge the next run by the cutscene's `fields_s`, not `speed_pct` alone.
   - **Where the time goes (2026-10-12 analysis, perf_2026-10-12_hle*.csv; read before choosing the next step):**
     - *GPU:* 6-7 ms per submitted frame (max ~15), so ~40% busy at full speed (60 fields/s): it looks idle only because
       core 0 feeds it ~7x too slowly. The GPU can only draw, so only drawing-shaped work can move to it (done: GX
       draws, TEV, lighting, copies; the vertex transform lost on the 2DS). Core 2 per draw cost is the GPU-side lever
       (draw merging: 76% of TP's draws could share a job; 99.6% of draws are strips) and only matters once core 0 is not
       the limit.
     - *Core 0:* IPC 0.31, 44% of cycles stalled on instruction fetch. Hot guest code in the title scene is spread thin
       (hundreds of functions, none >1%); only the Link/Epona cutscene has one: `dBgWKCol::ShdwDraw` 12.5% (the HLE
       stand-in). **23% of core 0's time is outside compiled code** (dispatcher, events, GX poll, frontend, JIT
       compile) and was not attributable: `g_where` markers (common.h `WH_*`) and `# core0 outside compiled code 10s:`
       lines in perf.csv now name it (needs a card run).
     - *Compiled code size* (`JIT: compiled code by opcode` in the qemu stats; per-opcode hot / cold words): compares
       took 10-11 hot words each (CR update), `bc` 12, `b` 10, `bclr` 7.6 (every branch flushed the register cache
       and spent two 4-word link sites). Next candidates, in order: CR-field dead-store elimination (a compare whose
       field is overwritten before it is read), `bl` (8 words), float loads / stores (`stfs` 15, `lfs` 10, `psq_st` 42),
       register pinning across blocks.
   - **JIT traces (not yet measured on the 2DS): blocks follow `b` and continue past `bc` / conditional `blr` / `bctr`**
     (`trace_branch`, `emit_branch_side`, `exit_kind_if`; EXIT_LINK / EXIT_INDIRECT stubs): the taken side is an
     out-of-line stub that writes back the register cache as it stood and leaves through a link site (or the indirect
     table); no flush or reload at branches. MAX_BLOCK_INSNS 32 -> 64. Controlled qemu comparison on 150 TP fields:
     hot words per guest instruction 6.0 -> 4.9, total code 4,598 -> 4,090 KB, cached GPR reads +58%, instructions
     run identical. JIT fuzzer: random programs with mid-trace branches, 30,000 cases x 3 seeds, 0 differences; planted
     bugs 5 (a followed b skips an instruction) and 6 (a side exit miscounts) are caught. perf.csv A/B: `traces` column,
     20 s phases (`--ab traces`, the default); the shadow-scan stand-in is off in this build (`--ab hle` / `--hle on`).
     From `tp_arm_850.state`, 100 fields into the shadow scene, traces on vs off: code 7,217 -> 5,160 KB, kept hot words
     per guest instruction 7.1 -> 6.0; same final PC, GX bytes and last frame; instructions 150,419,831 vs 150,419,842
     (longer blocks end a slice a few cycles later, so events land a few instructions apart: timing, not semantics).
   - **qemu ARM state for fast JIT-path tests:** `/root/tp_arm_850.state` (TP at field 850, just before the shadow scene),
     made by `HLE_CHECK=1 qemu-arm ... --jit --save-state 850:...`; `--load-state` it with the same build layout.
     `JIT_DUMP=big|<pc>` prints blocks' guest instructions and ARM words; `GC3DS_JIT_NOTRACE=1` turns traces off.
   - **Remote runs (2026-10-12): the 2DS takes jobs from anywhere** (full workflow: `docs/REMOTE_RUNS.md`). `core/host3ds/relay.c` (libcurl + mbedTLS from
     portlibs, roots in `host3ds/certs/relay_ca.pem`) talks to nakama-backend `/api/devrelay` (`src/routes/devRelay.js`,
     tables in its `sql/2026-10-12_devrelay.sql`, builds in the private storage bucket `devrelay`). On the PC:
     `bash core/tools/relay.sh run --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab traces`
     uploads this build if new, queues the job and follows its perf log into `core/out/relay_<job>.csv`. On the 2DS:
     menu L (or `auto 1` in `sdmc:/gc3ds/relay.txt`) polls every 10 s; a job for another build downloads it over the
     running `.3dsx`, keeps the job, and asks for one restart. Run options (also as 3dslink arguments,
     `3dslink -a <ip> gc3ds.3dsx -- --game ...`): `--game`, `--state`, `--save-state FIELD:PATH`, `--seconds`,
     `--ab traces|hle|none`, `--traces on|off`, `--hle on|off`; relay jobs default to 600 s. `relay.txt` (url + token)
     is ignored by git; the token's sha256 is in `devrelay_tokens`. Checked end to end in Azahar (job claimed, run,
     log streamed, done) and every endpoint against the live database.
   - **Save states load on the 2DS:** `System` is 0x6b78 bytes and `GXCore` 0x4a68 in both the qemu ARM build and the
     3DS build (same ARM EABI), so a state written under qemu (`--save-state 850:...`) loads with `--state`. A build that
     changes either size refuses old states; then make a new one (qemu, or a 2DS job with `--save-state`).
   - **2DS, traces on (relay jobs 4-35, from tp850.state): 1.3-1.4% vs 14-17% off, and the cause is code-buffer
     capacity, not a hardware bug.** With traces the 16 MB buffer fills every ~1.7 s and is flushed whole
     (`# jit cache` flushes 5-7 per 10 s); ~98% of the ~6,000 compiles/s are new PCs since the flush (`# jit repeats`), so
     the lookup works: about 14,000 entry points per interval, as many as without traces, but trace blocks are ~2x
     longer (tail duplication: every side-exit target and return point starts its own trace over the code after it),
     ~26 MB of working set. Without traces the scene fits (~21,500 blocks, 0 flushes). qemu shows it too once the
     counter is read: 50 fields, 8 MB buffer, 60 flushes with traces vs 35 without, 4.18M vs 2.42M guest instructions
     compiled for the same execution; qemu's wall time just does not show compile cost. Bisect: follow-only 15.0%,
     side exits only 12.2-12.9%, linking off no help, fused flags innocent; guest exceptions per emulated second equal
     on and off (`# exceptions`). The 2DS fuzzer (`--fuzz 40000 3 int,long`: 177,598 side exits) found 0 differences;
     the default mode found float sign-of-zero differences (ARM11 flushes tiny results to +0.0, interpreter -0.0).
     Traces stay off by default until duplication is bounded (`--trace-len` jobs pending).
   - **ARM11 fetch facts (DDI 0360F, read 2026-10-12):** 16 KB 4-way I-cache, 32 B lines, round-robin, virtually indexed
     (code 4 KB apart shares a set); 128-entry direct-mapped BTAC indexed by VA[9:3] (branches 1 KB apart alias);
     3-entry return stack (BL pushes, BX LR pops); folded B costs 0 cycles, unfolded predicted 1, mispredict 5-7.
     Program-flow prediction is off at reset (CP15 c1 Z bit): check it is on for our process. L2C-310 instruction
     prefetch exists but is secure-write only (not reachable from an app).
   - **Then:** from gcbench (GPU_GUIDANCE 20, 23, 6, 8): indexed draws (each vertex shaded once instead of per
     triangle corner), several draws in one draw window, GX strips as strips, raw draw commands.
2. **Normals, eye position and texgen on the GPU** with the same palette (the shader-lighting path already gets eye /
   normal from the CPU; texgen is 1.69 per vertex in TP).
3. **Less CPU decode:** where a GX attribute is byte / short / float, the PICA can read that type with a scale in
   the shader; the CPU then only byte-swaps and copies (indexed GX arrays still need the CPU gather, since the PICA
   has one index buffer).
4. **Culling on the GPU:** the vertex worker compacts kept triangles on the CPU; let the PICA cull when the CPU
   no longer has clip positions.
5. **Copies and conversions on the GPU's DMA engines:** EFB copies (~32/s), texture conversion and tiling through
   `GX_DisplayTransfer` / `GX_TextureCopy` (hardware tiling and format conversion), never through the
   `C3D_Sync*` calls (they split the frame and wait).
6. **Fewer GPU register writes per draw:** citro3d writes ~13 registers per `C3D_DrawArrays`; with a palette the
   batches get longer anyway.

Read first: "The GX thread is the second limit" and "Research digest" above, `src/gx/vfast.c` (VJob, vfast_prepare /
vfast_finish, the setup cache), `src/gx/pica/pica.c` (pica_draw, batches, the vertex worker, uniforms), and
`core/tools/gen_gxpass.sh`. Keep the CPU path as the fallback for anything the shader cannot do (lines and points,
CPU lighting forms), switchable at run time for an A/B on the card.

## Next work, ordered by speed gained per token

1. **Vertices: done on the CPU instead (0b29e22, not yet measured on the 2DS).** `gx/vfast.c` replaces the
   reference transform in pica.c with a per-draw compiled path, and pica.c batches draws with an unchanged state
   key into one `C3D_DrawArrays`. Checked with `core/tools/vfast_check.sh [secs] [tp-frames]` (`VFAST_CHECK=1`
   in soft.c), and NeHe 5/7, texturetest and triangle render correctly in Azahar. A vertex shader transform would
   gain little more while guest code is 54% of the field. **Next:** read the 2DS profile (`vtx` and draw counts) on
   the TP title scene.
   - Azahar needs about 35 s before a test program draws, so use `az_test.ps1 -Seconds 50`.
2. **JIT level 2 (Sonnet, fuzzer-checked).** First measure: `core/tools/arm_tp_jitstats.sh` runs TP under
   qemu-arm with the JIT and prints the instructions interpreted at run time. The ARM build needs its own states
   (the script's header shows how to make `/root/tp_arm_700.state`; the PC states do not load into it). A boot to
   field 700 under qemu takes well over 20 minutes, so run it in the background.
   - FPU and paired singles on VFP (TP is about 14% float).
   - Register caching across a block.
   - Block linking (direct branches; keep hot code inside the 16 KB I-cache).
3. **Gameplay blockers (Sonnet, one per agent, TP save states as the start point):**
   - Memory card in slot A (EXI card commands plus the DSP "CARD" unlock).
   - Play past the cutscene to controllable Link.
   - Zelda-microcode audio mixer.
   - Renderer correctness: mipmaps, near-plane clipping, range fog, EFB precision.
4. **Wii, later:**
   - MEM2 (heap split, see FINDINGS).
   - Offline disc decryption on the PC.
   - IOS HLE.

## Model choice

Opus for open-ended debugging and design; Sonnet for coding with a checker (fuzzer, reference renderer, CPU tests);
Haiku for lookups and log reading. Set `model` explicitly on every agent.

## Later milestone (owner, 2026-10-12): picture quality, after full speed and working sound

Order set by the owner: 100% playable first, sound fully working second, then textures and resolution "as good as
possible", choosing whatever measures best on the 2DS. Today the PICA backend renders the EFB scaled 0.625 x 0.5 straight
to the 400x240 top screen (no wasted fill). Candidates, best value first: the New 2DS XL's 800x240 wide 2D mode (2x
horizontal detail, ~2x fill; the GPU is ~40% busy at full speed); supersampling (render larger, downscale with the
display transfer; 2-4x fill); full-resolution textures with proper filtering (memory, little speed); a few HD replacement
textures (VRAM-limited, not whole packs). Decide with 2DS measurements once speed allows.

## 2026-10-12 afternoon: shared slow paths measured on the 2DS (relay jobs 40-45)

- Shared slow-path stubs (385c1ae) + hot region 62.5% (this commit's parent): title from boot **16.0-16.1%** (was
  15.7%), 0 compile fails (391 cold-overflow fails before), buffer 65% full after 240 s (was 97%), 14.6 host words per
  guest instruction (was ~27). The tp850 cutscene is unchanged at 15.5% (it never overflowed). `--slow inline|stub` A/B
  on the cutscene: 15.53 / 15.52.
- Traces now fit (no thrash) but give nothing: cutscene 15.52% on and off, title 15.94% on vs 16.03% off. They stay off.
- Core 0 counters (pmu, title): I-cache misses 9.0 -> 8.8 M/s, instruction-fetch stalls 274 -> 277 M cycles/s (~36% of
  cycles): slow paths seldom run, so shrinking them freed capacity, not fetch time. **The fetch stalls are hot code:**
  compares ~11 words (full CR update each), b ~10 and bc ~11 (register-cache flush + link site), stfs 15, lfs 9,
  psq_st 41, psq_l 24 (per-opcode table in the qemu stats). Next: CR dead-store elimination / lazy CR, then branches.
- **4c5012a on the 2DS (relay jobs 46-49):** `--pmu-split on`: compiled code (with the helpers it calls) has 88-96% of
  core 0's cycles, 95-99% of instruction-fetch stalls and ~all I-cache misses, so the emulator's C code is not where
  the fetch time goes. Traces had cut executed ARM instructions 5% and mispredicts ~30% without moving I-cache misses
  (their duplicated code keeps the touched footprint the same). Budget at block exits + SO-free CR updates: heavy
  rows of the title 16.24 -> **16.62%**, tp850 cutscene 15.52 -> **15.89%**; I-cache misses 8.8 -> 8.2-8.4 M/s,
  fetch stalls 277 -> 245-261 M cycles/s. Job 46 also met a light screen (~180 draws/s) that ran at a steady 60 fields/s
  for a minute: the whole pipeline holds full speed when the guest's work is light.
- **Next hot-path candidates** (qemu per-opcode, 4c5012a): bc 11.8 and b 10.1 hot words (register-cache flush at every
  block end plus the link site), psq_st 41 / psq_l 24, stfs 15, lfd 14, lfs 9. Pinning the most used guest registers
  across blocks would remove most of the flush / reload at block boundaries.
- **Region compilation is the next big item and has its own brief: `docs/REGION_JIT_PLAN.md`** (block-boundary
  register traffic ~25% of executed work; traces, register pinning and a float register cache measured and ruled out
  or deferred; constraints, options and how to judge them). The owner will give it a dedicated session.
