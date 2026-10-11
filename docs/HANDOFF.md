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
