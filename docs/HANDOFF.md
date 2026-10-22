# gc3ds handoff (2026-10-10)

> **▶ Current state (2026-10-20, night).** All defaults (jobs 696-702): **heavy area ~22.8% (6.84 game frames a
> second), Ordon walk ~31.95% (9.53 frames a second); title cutscene (25-150 s) 37.0%, its later Link / Epona stretch
> 23.5% (~7.7 pictures a second), THP movie 13.4%.** Gameplay is locked to two VI fields a picture (frames a second =
> fields_s / 2); the later stretch mixes two- and three-field pictures. New defaults on 10-20 (separate whole runs):
> the __GXSendFlushPrim stand-in (`--psflush`; heavy area +1.7%, Ordon +0.8%) and the ShdwDraw triangle path as a
> native stand-in (`--shdwtri`, C32b; later stretch +2.0-2.7%, heavy area +0.3%, Ordon +0.2%) and its fast form
> (`--trifast`: both cross / dot / scale chains in registers, later stretch +3.8%, +7.7% pictures, gameplay +0.2-0.4%),
> and the shadow callback natively, read stage by stage (`--shdwcb`; later stretch +1.1%, Ordon +0.7%). On 10-19: overlapped GPU
> frames (`--dblframe`, C30), float lanes (`--fcache`, C11), the 2-way indirect table (`--ind2`), compact vertex records
> (`--vin`), rlwinm as shifts (`--rotmask`), the ShdwDraw walk memo (`--shdwmemo`, C32; later stretch +14.2%). Earlier:
> three native library ports (core/src/cpu/port_j2d.c), gpbulk, ringbatch. Built and off, measured: the shadow callback
> cXyz operator stand-ins (`--psxyz`, heavy area -0.9%), flush-prim and J3D-light
> ports, crstub / befloat / calls / fastfloat / relayout (losses), pipeps (even); the rest of the old ruled-out list
> re-measured with separate runs on 10-20 (traces, vw2, steal, sched off, dlcache, vwmem 256 lose; icbi, qbig,
> feeddirect, cmdpld even).
> **Limits now: core 0 in every scene.** The joint-state line ('# joint 10s', every run) shows core 0 running guest
> code 93-97% of the time; the GX thread is idle 12-13% in gameplay and 40-75% in the title, the vertex worker idle
> ~70-96%. So the levers are cheaper guest code on core 0 (the JIT, ports, stand-ins, memos) and work taken off core 0.
> **The general-emulator experiments, 2026-10-20 (research/GENERAL_ARCHITECTURE.md section 6; numbers in
> MEASUREMENTS.md "X1" to "X4").** Every CPU-side lever measured about 1-2%, as the two walls predict:
> - **X3:** `_savegpr` / `_restgpr` inlined at the bl (`--gprinline`): Ordon +0.80%, heavy area +0.57%, cutscene
>   +0.64% (jobs 708-725); **on by default since 10-21**. Native calls that keep the register cache (`--zgpr`, `--zgx`,
>   `--zxyz`) still lose 0.1-2.1%: small functions pay only as inlined guest code, not as native calls.
> - **X4:** hand-written whole-function ARM against today's JIT (jobs 745-747): J2DPane::draw 1.72x, GXLoadPosMtxImm
>   2.51x, GXLoadTexObjPreLoaded 1.03x. Those functions are ~5% of core 0 (~+1.9%); a region tier needs ~35-60%
>   coverage to reach +15-30%. Supported as a targeted build, not built.
> - **X2:** L2 lockdown by master works (core 0's streaming chase 122.7 -> 33.0 cycles, restored and verified, jobs
>   748-763), but the split alone gives only +0.2% (heavy area) to ~+1% (title), and the second vertex worker still
>   loses with it: the idle cores' loss is not L2 eviction. All `--l2lock` switches off.
> - **X1 / X1b (PC, title states):** unchanged GX state writes 58-92% (the decoder already skips their state work;
>   decode cost 18-28% of the PC front end, device gain unproven); display-list keys with read-before-write state
>   repeat 42-61% (between the G2 thresholds); lighting reuse 16-89% by scene; no code outside the GX SDK touches
>   __GXData (the SDK-as-a-unit option stays open). The CPU test runner works again (`core/tests/cpu/run_tests.sh`).
> **Library atlas (10-21): research/LIBRARY_ATLAS.md** maps SDK / middleware / microcode families and the features that
> gate other games (EFB to RAM, CPU EFB reads, bounding box, MMU, FPRF); its section 9 proposes X6's game set.
> **Baseline at the end of 10-21's night (build a45608e, jobs 813-817, separate whole runs):**
>
> | Scene | Speed | Frames / s |
> |---|---|---|
> | TP heavy area | 22.74% | 6.82 |
> | TP Ordon walk | 31.84% | 9.49 |
> | TP title: cutscene / later stretch / movie | 37.10 / 23.46 / 13.41% | |
> | Wind Waker, Outset (`ww_outset.state`) | 19.20% | 5.76 |
> | Mario Party 4, board (`mp4_board.state`) | 14.11% | 8.43 |
>
> **Night of 10-21 (MEASUREMENTS.md "Second games on the 2DS"):**
> - Mario Party 4 and Wind Waker run on the 2DS, with gameplay states `sdmc:/gc3ds/states/mp4_board.state` and
>   `ww_outset.state`. The card holds TP, Mario Party 4 and Wind Waker; Galaxy 2 was removed.
> - Mario Party 4's three general fixes:
>   1. AX / CARD microcode;
>   2. the GP link bit;
>   3. PE signals one interrupt at a time.
> - New PC mode `GXDEFER=1` reproduces the 3DS's asynchronous GX ordering.
> - Per-SDK-generation PS stand-ins: Mario Party 4 board +15.2%, Wind Waker +2.0%, TP unchanged.
> - Wind Waker spends 21% of core 0 in actor search (cTgIt_JudgeFilter, cNdIt_Judge, fpcSch_JudgeByID): the largest
>   single lever found for it. REDESIGN_CANDIDATES C35 (a general form or an engine stand-in) waits on the owner.
> - **X5 done:** repeating display-list work is only ~19-33% of TP's GX thread (Mario Party 4: 52-86%). G2 alone cannot
>   fix TP's GX side.
> - The PC stand-in check is exact with `HLE_FTZ=1`: 65.4 M calls, 0 differed.
> - Exact estimate stand-ins (PSVECMag, PSVECNormalize, PSMTXInvXpose via the interpreter's soft float) were slower in
>   every scene: measured, not kept.
> - Mario Party 4 plays turns into a minigame (renders correctly; 11.7%, GX-bound).
>
> **Next (in order):** more games through the same path (an AX game such as Melee or Super Monkey Ball: the card has
> 0.5 GB free); TP's speed work. Then, by what those show: the HUD pane walk
> (research/LIBRARY_PORTS.md, prerequisite: why ~2% of J2DPictureEx port calls give up), a targeted region tier, G1 + G2.
> Still open: fault mode's crash (H3, measured +4-9%; only when James can restart the 2DS), a PMU split of the float
> stubs (H4). Parked by the owner: native mode (emulator first, 10-16); frame or render skipping (a steady 30 first).
> **Direction (2026-10-20): read research/DIRECTION_REVIEW.md first.** Gameplay has two walls of nearly equal height
> (core 0 ~93-97% running, the GX thread ~80-83% working), so core-0 work alone shows only up to ~+15-19%; CPU vertex
> gathering caps at ~300k vertices/s in both emulated and native mode (TP needs ~750k at 30 fps). Its ranked directions:
> retained GPU-resident geometry (C1) with the GX pipeline split across cores, native mode for TP, subsystem ports, a
> multi-game measurement program, measured accuracy trades. Its experiment order starts with a GX-ceiling instrument.
> research/EMULATOR_CEILING.md is the emulator-only view (cost model, H1-H5 / W1 / G1-G3); its hammock model (`core/tools/hammock.py`)
> put H1 at ~+3-8% core 0. The GPR inlining it proposed is X3 above.
> Earlier next steps, by measured size: (0) **the J2D pane walk as one native port** (the HUD: J2DPane::draw is 25% of Ordon's core
> 0 inclusive, ~6,600 cycles a pane in its own code; designed from the machine code in research/LIBRARY_PORTS.md "The pane
> walk's design", with the five pane classes gameplay draws and every callee it needs; ~+3-3.5% in Ordon expected);
> (1) **the library ports' own cost** (Ordon, jobs 648-649: building their context is 7-8% of
> core 0, committing 4-5%; J2DPictureEx::drawSelf 31 + 13 us a call, J3D's concat-view matrix load 4 + 2.7 us at ~8,000
> calls a second; the ports hold 13-22% of core 0's data-TLB misses): cheaper versions of the three ports on now;
> (2) new ports from job 630-631's profiles (heavy area / Ordon self time: J2DPane::draw 2.0 / 3.1%,
> mDoExt_3DlineMat1_c::update 1.3%, __save_gpr / __restore_gpr ~2.4% (C20: an in-place stand-in loses; only code the JIT
> emits in place would pay), cXyz operators ~2.8% (same: a stand-in lost)); (3) the shadow stand-in in the later stretch
> (replay ~7%, path ~11%, the native callback a few % of core 0, '# shdw where'; `--pmu-split cb` / `path` measure
> them per run). Small functions lose as
> entry-hook stand-ins on the 3DS: a port pays only when it replaces a large subtree or a hot loop. **Measuring:** check the field gaps ('# new frames hist'), then compare settled speed_pct or
> the '# new frames 10s' line across separate whole runs (two heavy-area runs of one build agree to ~0.1%); a change
> that stretches VI fields can show more pictures with a slower game clock (C32's unbounded version did). Texture
> sharpness is parked by the owner until full speed (REDESIGN_CANDIDATES C31).
>
> **Earlier on 2026-10-18.** Gameplay benchmarks on the 2DS, all defaults: Ordon walk ~23.5%, the heavy Ordon
> area (`sdmc:/gc3ds/states/tp_area2.state`) ~20.0% (14.6% at the start of 10-18), title cutscene ~26.8% (jobs 437-439, 449). In game
> frames a second (TP runs at 30): ~7 and ~6. The owner's goal: a steady 30 before any frame skipping, with the work
> balanced across the cores (move busy work to idle cores). What 10-18 found and kept (MEASUREMENTS.md, "Three scenes
> after the texture work" onward):
> - Lighting off the GPU's vertex shader and off the GX thread, onto the vertex worker (core 3): the shader was 26 of
>   the heavy area's 43.6 ms of GPU a frame. Textures in VRAM (DMA from a staging buffer; XFBs moved to FCRAM): GPU
>   33.8 -> 14.4 ms. Vertex set-ups cached for 8 keys; black lights skipped; uploads without a divide per texel. A 2 MB command buffer (no more frames ended early).
> - Measurement overhead was real: display-list hashes (3.9% of the GX thread) and the host profile's clock reads (8%
>   of the heavy area's speed) now run sampled or only with `--profile on`.
> - `--profile on` now also samples the GX thread's position ('# gx thread where 10s'): in the heavy area the decoder
>   (outside the backend) is ~22% of the GX thread, vertex prep ~7%, draw calls ~5%, batch key ~4.5%, TEV ~3%.
> - Limits now: **core 0 in every scene** (the game's own code: rendering ~39%, the 2D UI ~22%, logic ~19%); the GX
>   thread ~81-88% busy. `--gpudiag 1` (skip EFB copy draws) froze the 2DS: do not run it.
>
> - **Next: native library ports** (owner decision 10-18; plan in [research/LIBRARY_PORTS.md](research/LIBRARY_PORTS.md)): J2D first
>   (J2DPane::draw + J2DPicture), hand-written from the zeldaret/tp decompilation (sparse clone at /root/tpdecomp in WSL).
>   Since 10-18 evening: 2 MB command buffer (heavy area ~20.0%); memoization probe (`--memo`, C28) showed the HUD's
>   inputs change every frame.
>
> The older notes: the sections below are the 2026-10-10 snapshot and the running log after it (newest at the end);
> the newest numbers are in [research/MEASUREMENTS.md](research/MEASUREMENTS.md).
> - The repository is **public** now.
> - Twilight Princess plays into gameplay: a 2DS state with Link in Ordon (`sdmc:/gc3ds/states/tp_ordon.state`) is
>   the benchmark (a stick walk, AGENTS.md "Measuring"; ~22% on 10-17, 28.0% on the night of 10-18). The older `tp850` state is the title / attract
>   sequence.
> - (10-17: the GX thread, ~88% busy, was the next limit; on 10-18 core 0 was, then after the ports the GX thread again
>   in gameplay, see the top.) Heavy-area state:
>   `sdmc:/gc3ds/states/tp_area2.state`.
> - The PC input sequence below has drifted: with its presses up to field 2636 the game is still at the memory card
>   prompt at 3250; adding `A@3410` (no memory card), `SLEFT@3660`, `A@3700` (continue without saving), `START@4000`,
>   `A@4030` (name) and `A@4310` (horse name) reaches the opening by field ~4800 (2026-10-17, the current PC build).

Start here in a new session. Measured facts are in [FINDINGS.md](FINDINGS.md), hardware references in
[ARCHITECTURE.md](ARCHITECTURE.md), and the newest numbers and the open work in the current-state block above and
[research/MEASUREMENTS.md](research/MEASUREMENTS.md). The repository is `Tkibbs14/gc3ds`, **public**.

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

The PICA200 draws for ~20 ms of each ~190 ms frame; cores 0, 2 and 3 are the limit. The owner: "try absolutely
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
- **3a1696f on the 2DS (relay jobs 50-51): float steps did not move speed** (title heavy rows 16.55% vs 16.62%,
  cutscene 15.89% vs 15.89%) although I-cache misses fell ~5% (8.24 -> 7.89 M/s). **Core 2 is now a co-bottleneck:**
  the GX thread is ~80% busy at ~50,000 draws/s and **core 0 waits for it 12.0% of the time** (6.9% this morning,
  9.7% after the slow-path stubs, 11.5% after budget-at-exit): each core-0 gain turned into waiting. The next gains need
  core 2 cheaper as well: draw merging (`MERGE_STATS=1`: 76% of TP's draws could join the previous draw's vertex job;
  99.6% are strips) is the known lever, then per-draw setup cost on the GX thread.

## 2026-10-12 evening: draw merging (core 2), measured on the 2DS (relay jobs 52-53)

- **Where core 2's time went (3a1696f, title heavy rows):** busy 80% at ~50,000 draws/s, ~16 us per draw and all of it
  fixed per draw: `vprep` 6.8 us, `vpal` 3.0 us, `gxstate` 2.9 us, the rest of the job path 2.8 us. Per-vertex work
  was already on core 3. So the lever was the number of jobs, not the cost per vertex.
- **Draw merging (f56d430):** consecutive triangle draws with the same VAT and no state change between them become
  one vertex job. fifo.c calls the backend's `flush` before any command that changes state the open draw reads
  (register values, palette loads, PE signals, EFB copies); pica.c keeps each draw's stream and primitive and issues
  one indexed job whose triangles are assembled draw by draw (`vw_assemble_idx`); the GX thread issues the open draw
  when its ring stays empty, and `wait_drained` also waits for `g_gxMergeOpen`. Only with the GX thread (inline GX
  never merges). Options `--merge on|off`, `--ab merge`; perf.csv `merge` column and `# merge 10s` line.
- **PC model first (`MERGE_CHECK=1`, soft.c):** TP to field 1,500: **96.4% of 4.67M draws join an open draw, 28 draws
  (140 vertices) per job.** The old MERGE_STATS estimate (76%) excluded draws with per-vertex matrix indices, which TP
  uses everywhere. CPU-transform vertices of merged jobs are bit-identical to the separate draws (proves no state the
  job reads changes while it is open); the GPU transform with the merged job's single origin: max 0.70 texels on 39
  of 12.3M vertices (0.114 with each draw's own origin). Test DOLs: gxSprites and NeHe 11 merge 98%, the others not at
  all, 0 differences; NeHe 11 renders correctly in Azahar (98.4% joined there too).
- **2DS, `--ab merge` (20 s phases):** title heavy rows **18.77% on vs 16.26% off (+15%)**, tp850 cutscene **17.44% vs
  15.97% (+9%)**. Core 2 busy 80 -> 66%, core 0's wait for it 11.8 -> 6.0%, `vpal` 15 -> 1.8%, `vprep` 33 -> 15%.
  New wait: the GX thread waited for space in the vertex worker's 256 KB job ring 17.6% of the time (`vwait`, inside
  `vqueue`) while core 3 was 47% busy: merged jobs are ~30 KB. 9388639 makes the ring 1 MB (run option `--vwmem KB`,
  `--ab vwmem`) and stops reserving job memory deferred GPU-transformed jobs never touch (relay jobs 54-55).
- `core/tools/perf_heavy.py FILE.csv [--by COL]` averages heavy rows (draws_s > 1000) with standard errors, split by
  an A/B column.
- **Job ring (9388639, relay jobs 54-55, `--ab vwmem`): 1 MB gains nothing** (title 17.17 vs 17.31%, cutscene 16.15 vs
  17.19%), and the whole build was slower than f56d430 in both phases: core 0 waited for core 2 13-17% again. Cause,
  read from the columns (perf_diff.py): with merged jobs the old early-split gate ("only with <= 4 jobs queued, since a
  split waits for every queued job") rarely opened, splits fell 64 -> 49-55/s and the GX thread's wait for the GPU
  (`gxsync` + `gxpres`) rose 2 -> 7-12%. Default back to 256 KB.
- **Splits at the recorded draw calls (60d54fa, relay jobs 56-57, `--ab splitopen`):** an early split flushes and
  submits only what recorded draw calls use and waits only for their (older) jobs, leaving the open batch open. Title
  **18.00% vs 17.56%**, cutscene **18.14% vs 17.61%**; core 0's wait for core 2 12.7 -> 1.6% (title), `gxsync` 5.9 ->
  0.4%, splits 58 -> 78/s. On by default. Lesson: compare builds only through a within-run A/B: the title from boot
  lands on different parts of its loop from run to run (texture decodes 10x, compiles 2.5x between jobs 52 and 54).
- `core/tools/perf_diff.py A.csv[:COL=VAL] B.csv[:COL=VAL]` lists the columns whose heavy-row averages differ most.

## 2026-10-12 evening: region compilation measured and set aside (docs/REGION_JIT_PLAN.md's open questions)

`JIT_REGSTAT=1` (ARM build, c639aeb) now models the brief's options. TP from `tp_arm_850.state`, 30 fields, 7.6M block
runs, 47.9 hot words per run (the model's own exit counters inflate branch words; use the numbers below, not its
per-opcode table):
- **Where the traffic is:** per block run, loads 2.04 at first touch + 0.28 after evictions; write-backs 1.54 at
  constant exits + 0.45 at blr/bctr + 0.49 at evictions. ~4.8 memory accesses per run, ~10% of executed hot words
  (the brief's "25%" counted the whole block boundary: link sites and budget checks as well).
- **Exits:** 79% constant, 21% blr/bctr; back edges are 36% of constant exits. Traffic is fairly concentrated (top 64
  blocks 35%, top 256 61%) and 56% of it is in blocks inside a back edge's range.
- **Handing entry state over at linked exits** (no duplication; each block's entry = the first 5 ids it touches):
  saves 2.2-2.4% of executed hot words before the register moves it needs (~0.5-0.7 per run), so ~1.3% net.
- **Loop regions:** 0.64%. The loops that hold the traffic touch more than 5 registers, and the cache has 5 slots; the
  loops that fit carry only 0.46 accesses per run.
- **Decision:** not worth building now; the block boundary's code size is the bigger lever (next section). If revisited,
  the decoder it needs exists: `src/cpu/gekko_regs.c` (GPR / CR / XER / LR / CTR read and write sets), checked against the
  interpreter by the fuzzer on every run (third machine, PC only; planted bug 7).
- **Multi-block fuzzing (fuzzer mode `chain`, in run_jit_fuzz.sh):** random programs of segments ending in b / bc /
  bdnz / bl / blr, run through the dispatcher twice (compile + link, then mostly linked) with random budgets, the
  interpreter following each run; planted bug 8 (a wrong dispatcher entry count) must be caught. With traces off (the
  2DS default) 0 differences in 80,000 cases. **With traces on, seed 5 case 16475 differs** (`jit_fuzz 20000 5 chain`;
  the committed JIT before this session fails it the same way): a trace-mode bug to fix before traces are tried again.
- **Open trace-mode bug found by a new seed (older than this session; traces only, off on the 2DS):** `jit_fuzz 30000 31` fails at
  case 13483, an 11-instruction trace with a `bdz` side exit, where `mfspr r21,SPRG3` (r21 := 0) is followed by
  `sth r0,0(r21)`: the interpreter faults at address 0 (DSISR 0x42000000), the JIT stores through r21's old value (the
  pipe address). The committed JIT before this session fails it the same way; with `JIT_FUZZ_NOTRACES=1` seeds 31 and
  41 pass. Fix before traces are tried again; `JIT_FUZZ_CASE=k` prints a case's state and program.

## 2026-10-12 evening: one-word exits (block-entry budget check)

The data said the block boundary's *code size* is the lever, not its register traffic: branches were 34% of static hot
words (qemu, TP from tp_arm_850, 30 fields: `bc` 11.8 words each, `b` 10.1, `bclr` 7.5), because every exit carried
`SUBS RDC; BGT link; BL ST_LINK; .word target` in the hot path, two words of which never run once linked.
- Each block now starts with `SUBS RDC, #count; BLE budget_stub` (the same two instructions per block that each exit
  ran before) and every constant exit is one word, `B site`, whose `BL ST_LINK` and literals (target, site address) sit
  in the stub area after the block. A `bc` with one condition is itself the taken side's link site (`B<cond> site`,
  link_site keeps the condition). The dispatcher enters after the two-word prologue with the count already taken off
  (`run(..., pre)`, `Entry.count`), so a run still executes at most its budget plus one block; ST_INDIRECT no longer
  checks the budget (the target block does). Exits from the middle of a block give back what it did not run; emit_time's
  immediate and ST_INTERP's literal are patched at the block's end (`tfix`).
- qemu, TP 30 fields: same 44,798,955 instructions, same last frame (md5 34d7c70e...), same final PC and GX bytes as
  before. Hot words 681,522 -> 589,393 (-13.5%; `bc` 11.8 -> 5.8 words, `b` 10.1 -> 7.1); the stub area grew 2.0 -> 3.4
  words per guest instruction (not executed once linked). Fuzzer: default seeds 11/21 and chain mode 0 fails, plants 1-8
  caught; arm_jit_check frames identical.
- **2DS, dc09f3e (relay jobs 58-59): neutral.** Title 18.21% (job 57's same-config phase 18.14%), cutscene 17.45% (job
  56's 18.00%, but job 58 ran with `--pmu-split on`, whose hooks cost something: never mix it into a speed comparison).
  Executed instructions fell 3-11% while I-cache misses rose ~6% (7.97 -> 8.46 M/s title): the stub area between blocks
  grew from 2.0 to 3.4 words per guest instruction and spread hot code over more cache lines.
- **3605dfd moves every block stub (exit, link and budget stubs) to the cold region**, which is now emitted at its final
  address (no copy from a scratch buffer), so hot code and stubs are patched by address (`patch_to`). The hot region
  holds only block code: 5.6 words per guest instruction. qemu TP exact; fuzzer clean. 2DS: relay jobs 60-61.
- **2DS, 3605dfd (relay jobs 60-61): title 19.26%, cutscene 19.27%** (from 18.21 / ~18.0): I-cache misses 8.46 -> 7.58 M/s,
  instruction-fetch stalls 286 -> 238 M cycles/s (title). One-word exits only paid once their stubs left the hot region.
  Session total: title 16.6 -> 19.3%, cutscene 15.9 -> 19.3%. Core 0 waits for core 2 2-4%; the GX thread waits for
  the vertex worker ~19% (`vwait`), the next core-2 item once core 0 speeds up again.
- **Next, by expected gain:** float / paired-single code size (psq_st 31 hot words, psq_l 24, ps_madds 18-24: GQR guard
  baking, known-single operands); the vertex worker's per-job cost (~220 us per merged job on core 3; the 116-byte VIn
  record is mostly unused texture slots); fix the two trace-mode bugs (chain seed 5, default seed 31) before traces are
  tried again, now that blocks are smaller and stubs are cold.

## 2026-10-12 night: float stubs, the code buffer split, perf.csv off core 0

- **Shared float stubs (b7534e3):** psq_l / psq_st with a plain-float GQR (98% / 99.9% of their executions in TP,
  `JIT_REGSTAT` counts them by GQR type) and stfs call shared stubs per GQR and W (`emit_float_stubs`) that do the GQR,
  alignment and MEM1 checks, the access and the conversions; the cut for values that are not exact singles is shared once.
  A site keeps the call, a branch to its slow path and its FPR moves: psq_l 24 -> 5.6 hot words, psq_st 31 -> 5.2. The
  gather-pipe path of psq_st stays in the site's cold code (the stub reports "only outside MEM1" in r3).
- **The code buffer's split:** moving block stubs to the cold region (3605dfd) overfilled it: 17 full flushes in 30 qemu
  fields on the PC's 8 MB buffer, and the 2DS's 16 MB buffer was 75% full after 4 minutes (no flush yet). The hot region
  is now 37.5% (hot 5.0-5.1 vs cold ~9 words per guest instruction); 0 flushes again. **Check `# jit cache` flushes and
  `jitcode_pct` after any change that moves code between the regions.**
- **2DS, b7534e3 (relay jobs 62-63): cutscene 19.62%, title 19.77%** (from 19.27 / 19.26): I-cache misses 7.58 -> 6.79
  M/s, fetch stalls 238 -> 205 M cycles/s. Hot code per guest instruction 6.0 this morning -> 5.1.
- **Core 0 outside compiled code** (3605dfd, title): dispatcher 6.4-6.8%, **perf_log 4.5-4.7%**. The second was perf.csv
  flushed to the SD card once a second on core 0: perf.csv now goes through a memory ring that a thread on core 1 drains
  (`host3ds/asyncfile.c`); judge it by the `perf_log` share. The dispatcher's ~35k indirect-table misses per second are
  not aliasing: folding higher address bits into the index changed nothing under qemu (86,499 -> 91,663 misses in 30
  fields), so it was not kept; most are probably blr / bctr into polling-loop and idle blocks, which the table never holds
  (unverified: count misses by target kind before working on it). qemu stats now print the dispatcher's return reasons.
- **2DS results since:** d7ac114 (perf.csv via core 1) 19.83 / 19.86% (cutscene / title); 9b96050 (lfs / lfd / stfd stubs)
  20.10 / 20.13%; afe3fc0 (LR literals, known-single frC, bottom-screen text on core 1) 20.73 / 20.69%; **a1dd311 (indirect
  table 16,384 entries) 21.01 / 21.04%** (relay jobs 64-71). perf_log on core 0: 4.6% -> 0.0%. Indirect misses: TP's were
  slot conflicts (79,307 of 86,499 under qemu; folding high address bits in did nothing, a 4x table cut them to 36,621; on
  the 2DS 35k -> 13.6k/s).
- **Where core 0 stands (title, a1dd311-era counters):** TP needs ~1.65M guest instructions per field, so 80% speed is
  ~79M guest instructions/s against ~21M now (~3.9x). Core 0 executes ~13 ARM instructions per guest instruction at IPC
  0.33 (25% of cycles fetch stalls, 21% data-hazard stalls). Memory instructions are 40% of TP's guest instructions
  (`JIT_REGSTAT`: r1+d 21% of them, r13+d 8%, r2+d 3%, other rA+d 63%, rA+rB 4%) and each runs ~12 ARM instructions
  through its stub (call, alignment and MEM1 checks, access, byte swap, return): ~37% of all executed ARM instructions.
- **The guest profile is flat** (relay jobs 72-73, `--profile on`, `core/tools/hot_funcs.py` with zeldaret/tp's
  `config/GZ2E01/symbols.txt` kept outside the repo in WSL `/root/tp_symbols/`): the 200 hottest blocks are 13.5% (title)
  and 6.2% (cutscene) of core 0, no game function above ~0.5% except the DVD driver's `Read` while loading. Native
  stand-ins for single functions cannot give big steps on these scenes; the per-instruction cost of compiled code can.

## 2026-10-13 early: memory checks, the job ring's size, attribute prefetch

- **One-compare memory checks (bf169a0):** the MEM1 check is `SUB r1,r0,#0x80000000; MOV r12,r1,ROR #log2(size); CMP
  r12,#(0x01800000>>sh)` (alignment and range in one compare; the uncached mirror 0xC0000000 now takes the slow path, which
  TP never does). 2DS: cutscene 21.01 -> 21.19%; title 20.62% on fewer heavy rows (not comparable). The chain fuzzer's
  crash while building it was `in_lc` overflowing for addresses near 4 GB (`sys/system.c`, fixed: compare offsets, not
  ends).
- **The vertex worker's job ring is 128 KB now (was 256 KB).** Deep rings lose: the GX thread waits much longer on the worker
  (inferred cause: every sync with the worker waits for the whole queue). `--ab vwmem` title, relay 76-77:
  1 MB 18.9% vs 256 KB 20.8-20.9% in the same runs, GX-thread wait 19% vs 3-7%. Cutscene, separate runs (relay 78-80):
  96 KB 21.48%, 128 KB 21.41%, 256 KB 21.18% (+-0.18-0.19 each). 96 and 128 tie, so 128 was chosen; the worker's own
  wait grows as the ring shrinks (vwait 19.7 -> 22.1 -> 23.9%), so smaller than 96 KB is not worth trying. `--vwmem KB`
  sets it.
- **Indexed-attribute prefetch (e30b707, `--ab pld`):** the gather prefetches the array elements of indexed attributes 4
  vertices ahead (`vfast.c`, `prefetch_table`). In-run A/B (relay 81-82): cutscene 21.00% off / 20.91% on (+-0.15 /
  0.26), title 21.21 / 21.47% (+-0.15 / 0.16): no measurable change in speed, the worker's busy share 62.2 -> 60.5%. Kept
  on: the worker is not the limit now (core 0 is: 98% busy, 93% of it compiled code; the GX thread 78%). A press of X
  on the 2DS turns the time breakdown off (job 82 from 12 s): those columns read 0 then.

## 2026-10-13: fault mode (fault-backpatched loads and stores, Dolphin's fastmem on a 32-bit host)

- **Why:** core 0 is the limit (98% busy, 93% of it compiled code; the GX thread 78%), and memory instructions are 40%
  of TP's guest instructions, each ~9 executed ARM instructions through a shared stub (call, one-compare check, access,
  swap, return). In fault mode an integer load is `ldr rd, [RRAM, r0]; rev rd, rd` and a store `rev r3, rs; str r3,
  [RRAM, r0]`: the same two hot words as the stub call, two executed instead of ~9, no call and return.
- **How:** RRAM is now ram - 0x80000000, so `[RRAM, ea]` is MEM1 through the cached mapping. Every other guest region
  lands on host memory that is not mapped, the access faults, and the host handler calls `jit_fault`, which patches the
  site into `b <its slow path>` (the cold code every site already had) and resumes there; from then on the site is
  the old slow path. Sites live in an open-addressed table (2^18 slots, 2 MB) cleared with the code.
- **The 2DS probe (`--probe-faults on`, relay job 85, 249f8dc):** a data abort reaches a `threadOnException` handler
  under Luma3DS with the exact PC of the faulting instruction (+0), and the handler resumes the thread itself (flags,
  then `ldmia r0, {r0-r15}`); a fault costs 1.65 us and the patch's cache sync (svc 0x94) 21 us, once per site. MEM1 is at
  0x0800cfe8, so the uncached mirror, the EFB, MMIO, the gather pipe, the locked cache and physical addresses all land
  at 0x40000000 or above, which user mode cannot reach: all fault. Unaligned `ldr` loads the right bytes (SCTLR.U set).
- **Not exact:** an *invalid* guest address whose host alias is mapped memory goes through instead of raising a DSI
  (on the 2DS: guest 0x78000000-0x7FFEFFFF and 0x81810000-0xB7FFFFFF; no game uses them, a real GameCube would crash
  there too). MEM1 now sits between two 64 KB no-access guard regions (`host_mem1_alloc`, sys/system.c; 3DS:
  jit_3ds.c), so displacements past either end of MEM1 fault and stay exact. The fuzzer does not compare a case whose
  interpreter took a DSI at an aliasing address and counts them (chain mode, seed 1: 28 of 30,000).
- **Found on the way (fuzzer-only, fixed):** the pipe stub read the gather address from the helper table filled at
  `jit_create`, before `jit_set_gather` set it, so code run without `jit_entry` (the fuzzer) sent stores to guest
  address 0 into the pipe (seeds 1 and 7 failed at HEAD); and fast paths ignored that a supervisor-only BAT gives
  problem-state code a DSI (`fastmemUser`). Mismatches now print their case number for `JIT_FUZZ_CASE`.
- **2DS result: no gain, and a kernel crash (relay 86-90, 65f291ac).** In-run A/B (`--ab faultmem`, cutscene, job 88):
  on 19.44% +- 0.65 (69 heavy rows), off 19.53% +- 0.60 (60): no difference. Both runs with fault mode crashed (after
  ~700 and ~2,700 faults); Luma's dump (reported by the next run, see below) is a **prefetch abort in the kernel**
  (supervisor mode, pc fffffffe, lr 0, core 0) while it handled our data abort on a fault-mode store to the gather
  pipe (far 54025000 = MEM1 + 0x4C008000, dfsr 805). The kernel's delivery of user exceptions fails now and then (the
  probe's 202 faults passed); nothing on our side can make that safe. **Fault mode stays off on the 2DS** (default;
  gcemu and the fuzzer keep it on, where it is verified). The lesson is the useful part: removing ~7 executed
  instructions per memory access did not move speed, because a shared stub reached with a predicted call and return
  is cheap on the ARM11 and core 0 is bound by stalls, not instruction count.
- **Where core 0's cycles go (title and cutscene, d0b37d79-era, relay 83-84):** ~802 M cycles/s; 262-294 M
  instructions; instruction-fetch stalls 195 M cycles/s (24%, 6.4 M I-cache misses/s); **data-dependency stalls 173 M
  (22%) with only ~1.3 M D-cache misses/s**, so they are pipeline interlocks (a load's or a multiply's result used too
  soon), not memory; branch mispredicts ~0.5 M/s; TLB misses 127 k/s. Next: schedule the emitted code against ARM11
  interlocks (simulate first: weight each block's host code by its executions under qemu and count interlock cycles
  with the TRM's latencies), and keep shrinking hot code.
- **Crash reports from the device (crashlog.c):** our threads (main, GX, vertex worker) record an exception to
  sdmc:/gc3ds/crash.bin, and every run reports a previous record and the newest Luma dump not yet reported
  (sdmc:/luma/dumps/arm11, as hex: header, 23 registers r0-r12 sp lr pc cpsr dfsr ifsr far fpexc fpinst fpinst2,
  then the process name) in perf.csv ("# crash log", "# luma dump"). An exception the kernel or another process
  takes shows up only in Luma's dump. ELFs of relay builds are kept in WSL /root/elfs for decoding addresses.

## 2026-10-13: the ARM11 interlock model, and code scheduled against it

- **The model (`core/tools/arm11_sched.py`):** a qemu run with `JIT_REGSTAT=1 JIT_SCHEDDUMP=path` writes every executed
  block's hot code, its (guest opcode, hot words) list and its execution count, plus the shared stubs; the script runs
  each block through an in-order issue model with the TRM's result latencies (ALU 1, +1 for a shifted operand or an
  address base; load 3; VFP add 8, mul 8/9; VFP load 4; ARM->VFP 4; VFP->ARM 2), follows calls into the stubs, skips
  the statistics build's counters, and reports interlock cycles by producer and by guest opcode (`--show KEY` prints
  the hottest block holding KEY word by word; the scratchpad's showop.py disassembles it). Check: it gives 0.34 stall
  cycles per instruction from interlocks alone; the 2DS counter says 0.64, and ~1.3 M D-cache misses/s at ~50 cycles
  account for most of the rest.
- **TP's interlocks (40 fields of the cutscene):** loads 63% (lwz alone 17.5%: 3.7 cycles each, a fresh SUB as a
  shifted operand in the stub's check and the swap right after the load), VFP 23% (fmuls 15.7 cycles each, psq_st
  14.3, ps ops 11-12, psq_l 9.6: every float instruction loads its FPRs, computes, converts and stores in a chain).
- **Scheduled code (`jit_set_sched`, `--sched on`, `--ab sched`):** memory stubs whose check reads r0 (r0 - 0x80000000
  is r0 with bit 31 flipped, and rotation distributes over EOR) and whose loads leave the byte swap to the site after
  its slow-path branch; and `add_const` building constants from up to two immediates (a negative 16-bit displacement
  from r13 / r2 was mov + 3 orr + add). Model: interlock cycles 1.70e8 -> 1.39e8 (-18%), executed instructions
  -4%, lwz 3.73 -> 0.78 cycles, hot words 4.7 -> 4.6 per guest instruction. Verified: fuzz seeds 1 and 3 with fault mode
  off (the stubs in use), DOL check, TP under qemu on vs off: identical instruction count and last frame.
- **2DS (relay 91-94):** `--ab sched` within noise (cutscene 19.89 off / 19.72 on, title 18.86 / 19.94, +-0.5; the
  data-stall counter fell only 2-3%, less than the model's 18% of interlock cycles); kept on as the default (fewer
  instructions and words, no harm seen). **Indirect table 64K entries (d613012):** indirect misses 13k -> 3.6k/s, the
  dispatcher's share of core 0 in the cutscene ~10-16% -> 4.5-6%; speed cutscene 21.35% (21.17 before), title 20.58%
  (21.11; a different stretch, 136 heavy rows against 188). Malloc's largest free block 17-20 MB.
- **Where this leaves TP (2026-10-13):** both cores near their limits at ~21%: core 0 98% busy (fetch stalls 24%,
  interlocks ~20%, dispatcher 5-12%), the GX thread 80% with 9-16% of it waiting on the vertex worker. Each remaining
  lever measured or modelled this session is worth a point or less. 80% needs ~4x on core 0 and on core 2; this JIT's
  design (~13 ARM instructions per guest instruction, FPRs through memory, registers cached only within a block) does
  not get there by tuning. Options for the owner: an optimizing JIT (registers and FPRs across blocks, constant
  folding, flags only when read; Dolphin-class code is ~4-6 ARM instructions per guest instruction), game patches for
  TP (fewer draws, effects off), or TP as the stress test while other games set the playable target.

## 2026-10-13 day: TP is GX-bound; check hoisting; live 2DS control (relay commands and frames)

Owner direction this session: work on the 2DS itself, live, and catch slowdowns fast; PC runs (fuzzer, qemu same-frame
check) only as quick correctness gates before a build reaches the card. Plan in docs/OPT_JIT_PLAN.md.
- **Read speed through core 0's own speed.** `gxwait_pct` (core 0 waiting for the GX thread, mostly the idle hook's
  `wait_drained` at polling loops; the sampler files it as "dispatch") is 9-16% in TP's heavy rows, so a faster core 0
  shows up as more gxwait, not as speed. Fault mode (job 88): speed 19.53 / 19.44% off / on, but core 0 speed 21.2 ->
  23.6% (+11%). `tools/perf_heavy.py` prints `speed / (1 - gxwait)`. The earlier "stalls, not instruction count" lesson
  was this effect.
- **Check hoisting (OPT_HOIST, `--opt on`, `--ab opt`):** one window check per base register per block (ST_HOISTCHK),
  later D-form accesses unchecked (integer ldr/str + rev, floats through unchecked stubs), a failed check deoptimizes
  the block (ST_DEOPT, recompiled with per-access checks, ~450 blocks in TP); hoisted lwz defers its `rev` to the
  value's first use (Rc.swap). Fuzzer: JIT_FUZZ_OPT, plants 9 and 10; qemu TP same last frame (instruction counts differ
  by ~750 in 30 fields: deopts split blocks, which moves run boundaries). 2DS job 97 (cutscene, first 300 s): core 0
  speed 21.41 -> 22.62%, speed 19.48 -> 20.07%. Not default yet.
- **TP after ~300 s of the tp850 state** reaches a section at ~11% speed with half the draws where gxwait is ~0: core 0
  alone limits there (job 97, 300-620 s).
- **The GX thread waits for vertex-job ring space ~22-25% of its time** (`valloc_pct`, new; waits with ~3.8 jobs in
  flight). Trimming unused job reservations (`--ab vtrim`, 23 -> 19.6 KB per job) barely moved it. A bigger ring (live,
  job 99): 512 KB speed 19.03% vs 20.08% at 128 KB, because core 0's drain wait doubled (30% vs 14%): every polling-loop
  sync waits for the whole queue. The lever is the worker's cost per job: `# vworker phases 10s` (a23f13f) splits it.
- **Live 2DS control** (nakama-backend e9ee0a8, 4a82655): `relay.sh cmd JOB "..."` (shot, set opt|sched|merge|vtrim|
  pld|splitopen|vwmem, tev [N], stop; applied within ~2 s, logged as `# cmd`), `relay.sh frames JOB` (top-screen BMPs,
  every 15 s and on `shot`; convert to PNG with PowerShell System.Drawing to view). Downloads are gzipped with progress,
  stall detection (512 B/s for 30 s) and 3 attempts; new builds restart themselves through Luma's hb:ldr target +
  aptSetChainloaderToSelf (download and restart times go to the job log: check them on the first job of a new build).
- **Open:** the THP movie after the title renders purple on the 2DS (owner report). Hypothesis: YUV->RGB TEV needs
  signed intermediates the PICA combiners clamp (TEVA_RANGE); confirm with the live `tev` command while it plays.

## 2026-10-13 evening: research, crash cause, display lists; next = the display-list vertex cache

- **Read first:** docs/research/PERF_LEVERS.md (ranked levers, ruled-out ideas with sources, what only the 2DS answers)
  and docs/DL_CACHE_PLAN.md (the next build).
- **Answered on the 2DS:** 99.6% of TP's draws are inside display lists and ~80% of list calls replay identical bytes
  over the same array bases (D1, job 107); core 0's profile is flat (top 200 blocks 5.4%; no stand-in worth writing)
  (D3); 43% of core 0's modelled cycles are float loads/stores and maths (D2, qemu model).
- **THP movies:** no longer purple (linear TEV path, e19200a; checked on a 2DS frame, job 104). They play at ~4.6%:
  the game decodes JPEG in guest code; a native THP decoder is a later item.
- **The 2DS crashes of jobs 99 / 102** were a data abort in Process9 (the ARM9's file-system server) with perf.csv text on
  its stack (Luma arm9 dump, reported since e19200a). Frames are now uploaded from memory, not read back from the card
  (7732b4f). Watch `# luma arm9 dumps` in every run.
- **Relay:** downloads are gzipped with progress (~110 s for 4 MB on the 2DS's Wi-Fi); the restart into a new build
  froze in a blocking srvGetServiceHandle("hb:ldr") (fixed in d18f4c0, first used by the download *after* the build
  that contains it). The relay server rate-limits bursts (429): relay.sh frames retries.
- **Order of work:** (1) display-list vertex cache (GX side, the cap today); (2) single-precision floats in the JIT
  (PERF_LEVERS lever 2); (3) measure check hoisting at length and make it default if it holds; (4) native THP decode.

## 2026-10-13 late: display-list vertex cache built and measured; relay restarts by itself

- **Relay:** new builds now restart by themselves (job 114: "restarted into this build 11.1 s after the download").
  hb:ldr is a named port of Luma's loader: `svcConnectToPort`, as hbmenu does; `srvGetServiceHandle` never answered.
  A job resumed after a restart numbers its log chunks from 100000 (the relay upserts by seq; job 111 lost its header).
  `relay.sh queue` refuses to queue when the build upload failed (job 110 ran the old build after an HTTP 500).
- **Display-list vertex cache** (`--dlcache on`, `--ab dlcache`, live `set dlcache 0|1`; off by default). Exact: PC
  `VCACHE_CHECK=1` gathered 12.36M hits again, 0 differences (TP to field 2000); no entry was ever invalidated by the
  game's flushes in the cutscene (stale 0/s), so TP replays unchanged vertex data.
- **Measured on the 2DS (tp850 cutscene, `--ab dlcache`):**

  | job | change | worker hit rate | speed off | speed on |
  |---|---|---|---|---|
  | 111 | 116-byte records, 2.5 MB ring | 0% (ring wrote over every match) | 18.77% | 14.74% |
  | 112 | compact records, 6 MB, 2-way | 60-70% | 20.36% | 18.74% |
  | 113 | missed segments gathered in runs | 84% | 20.47% | 19.30% |
  | 116 | matrix indices split from VIn (VIx) | 85% | 21.34% | 20.13% |

  Hits took 32-36% of the worker (~7 us per hit segment): the ring is larger than the L2 and every record line waited
  for RAM. 118 prefetches slots and hit records a chunk ahead. If hits stay expensive, the next idea is to skip VIn for
  hits (compute positions straight from the record), or to cache smaller (positions as the stream's own format).
- **JIT fuzzer:** compares the pages compiled dcbf/dcbst/dcbz stamp with the interpreter's (planted bug 9); window edges
  randomized and compared; float sequences store sanitized values; unmapped-alias cases skipped like DSI ones.

## 2026-10-13 night: what moves TP's speed (jobs 117-129); core 0 is the wall now

- **Read first:** docs/research/REDESIGN_CANDIDATES.md (C1-C10, each with its evidence and re-evaluation trigger; the
  owner's rule: log every redesign idea, re-evaluate later, never delete).
- **Defaults now:** check hoisting on (job 117: core 0 +5.0% cutscene, +4.7% core-0-bound section), known-single
  stores on (OPT_KSTORE, job 129: +1%, within noise, exact). Every older switch re-measured on today's build (jobs
  120-124): merge is worth 24%; pld, traces, sched and splitopen are within noise (defaults kept).
- **Rejected with measurements:** the CPU display-list cache (hits cost as much as gathers: RAM latency, job 119);
  the second vertex worker on core 1 (core 0 -6%: its gathers evict core 0's working set from the shared 2 MB L2,
  jobs 125/127, not the polling); the shared-index fast path (TP: under 1% of indexed vertices qualify, job 127).
- **Frame skip experiment (`--ab fskip`, job 128):** +10% speed; with half the vertex work gone, core 0 waits on the
  GX side only ~5%: **core 0 (~23% own speed in the cutscene, ~11% in the later stretch) is the wall.** 30-40% needs
  the JIT: C8 single precision (research done; the store half of it is OPT_KSTORE), constant propagation, fault mode's
  +11% (kept off for the kernel crash), float values kept in VFP registers.
- **Relay:** builds restart by themselves (hb:ldr via svcConnectToPort); downloads retry on a stall (job 126's first
  attempt stalled and the second finished). A run interrupted by the 2DS sleeping stays "running" on the relay:
  cancel it (job 126).
- **Baseline of the night's defaults (job 130, tp850, 600 s, no A/B):** cutscene (first 300 s) 20.06% +- 0.26, core 0
  22.83% (gxwait 11.1%, valloc 21%, worker 55% busy); later core-0-bound stretch 11.80% +- 0.05 (core 0 11.87%).
  No new Luma dumps (arm11 9, arm9 2). Compare the next builds' runs against these, within-run A/B first.

## 2026-10-14 night: JIT code size, candidates 1-2 measured (job 148)

- **Corrected qemu baseline** (TP 850-880 from `tp_arm_850.state`, traces off, fault mode off, hoist + kstore, as the
  2DS): **4.80 executed hot words per guest instruction**. Earlier JIT_REGSTAT tables counted the statistics' own
  counters (8.58); they are left out now. Largest shares: lwz 11.7% (4.2 words), bc 11.4% (6.2), stw 6.5% (3.9),
  addi 5.4% (1.7), bclr 4.2% (6.8), cmpli + cmpi 5.6% (6.4-6.5), paired-single / float arithmetic ~13% (10-21 words).
  This window starts with a load: icbi is 1.4% of its instructions (each one an interpreter fallback and a dispatcher
  round trip, as icbi ends the block), while the 2DS runs only ~15k fallbacks/s in the measured scenes.
- **Candidate 1, CR-field dead stores: not built.** The fate model (JIT_REGSTAT, `CR-field fate`) over 2.9M executed
  CR-field writes: a fused bc next and the field dead on every path 5.3%, dead only past a load / store or a call 42.2%,
  live 2.4% + 32.8%, unknown 17.3%. Exact elimination reaches ~0.3% of hot words; even the inexact (Dolphin-style)
  version, where a dead field may be stale at exceptions and dispatcher exits, reaches ~1.5-2%.
- **Candidate 2, OPT_RETSTUB (`--retstub on`, `--ab retstub`), off:** blr / bctr / bctrl through shared stubs. qemu:
  bclr 6.8 -> 3.8 words, all hot words 4.80 -> 4.70 (-2.1%), identical instruction count, final PC, GX bytes and last
  frame; fuzzer clean (run_jit_fuzz.sh with JIT_FUZZ_OPT=19 incl. planted bugs, chain seeds 1-8 and default seeds
  11/21/31 with traces and fault mode off). **2DS job 148 (`--ab retstub`, 600 s):** core 0 cutscene 24.40 +- 0.21 on vs
  24.10 +- 0.24 off, later stretch 12.13 +- 0.07 vs 12.34 +- 0.40: within noise. Cutscene I-cache misses did not move
  (6.69 vs 6.71 M/s): 2% fewer executed words are not 2% fewer misses. bl's LR write (literal load, store, literal) has
  no shorter form without a taken branch into cold code.
- **Fuzzer fix:** a chain case skipped for an aliasing access (fault mode) left its memory differing, and the next ten
  cases failed on it (seed 1, both with and without the new opt bit); the skip now takes the compared memory from the
  interpreter.
- Tools: `core/tools/ab_split.py relay_N.csv` (A/B by the opt column, cutscene vs later stretch, settle after switches).

## 2026-10-14 late night: what the I-cache misses are, and fewer dispatcher round trips (jobs 149-151)

- **I-cache simulation** (`core/tools/qemu-plugins/icache.c`, the ARM11's 16 KB 4-way L1 under qemu, misses per
  line; `icache_report.py RUN GCEMU --minus WARMUP` splits them by JIT region, gcemu function and, with JIT_BLOCKMAP,
  by block and TP function). TP 852-880, warm-up subtracted: JIT hot region 47% of all misses, gcemu C 36% (on the PC
  most of it is GX code, which runs on cores 2-3 on the 2DS), libc 8%, cold region 5%, shared stubs 4%. **For core 0,
  block code is ~3/4 of the misses, spread over ~60k lines** (top 1,024 lines 32%, top 4,096 64%): no small hot set.
  The dispatcher path (jit_entry, lookup, run, gekko_run, jit_interp, ...) is the next 5-10%.
- **Dispatcher round trips cost more than their own cycles**: each one runs C code that evicts JIT code. On the 2DS
  the cutscene makes ~3,400 `sc`/s (TP's PPCSync; `# exceptions` line, c00) and the Link / Epona stretch ~21k
  stand-in calls/s, each of which returned to the dispatcher (several round trips per sc).
- **OPT_SYSCALL** (job 149): sc and its handler (checked at compile time: SPR moves, ori, isync, sync, rfi) run in one
  interpreter call inside the block. **OPT_HLELINK** (job 150): a stand-in's block goes on through the indirect table.
  **Together with OPT_RETSTUB (`--ab stack`, job 151): core 0 cutscene 24.47 -> 25.00% (+2.2%), Link / Epona stretch
  12.04 -> 12.54% (+4.2%). Default on** (`--retstub/--syscall/--hlelink off` to compare). Alone: retstub +0.8 / +1.6%,
  syscall +1.8 / +2.0%, hlelink +0.2 / +2.8% (cutscene / later; all numbers by ab_split.py as fixed below).
  qemu: same instruction count, final PC, GX bytes and last frame; fuzzer clean (plant 11 checks the handler count).
- **Reading A/Bs:** `core/tools/ab_split.py` now reports 10% trimmed means and medians. A few rows far off (a hitch,
  a load) moved plain means more than the 2% effects measured here (job 148's later stretch read "within noise" by
  means and +1.6% by trimmed means).
- **Heap:** the second job on a build without a restart started with malloc's largest block at 1.1 MB (jobs 145 and
  150; job 145 is the stand-in A/B that made it default). Relay jobs now restart the process when a game already ran.
- **Next, by the miss data:** the misses follow the total hot code and the block boundaries, so the structural
  levers come first. (1) Re-measure traces with these tools: -18% hot words under qemu, filed as "within noise" by
  plain means while the 2DS's code buffer overflowed; fix the two trace-mode fuzzer bugs and the overflow first.
  (2) Register write-backs at block exits (~14% of hot words). (3) The shared stubs (31 KB, 4% of misses) ordered so
  the hot ones share lines.
- **Traces re-measured (job 154, `--ab traces`, today's defaults): they lose.** Core 0 cutscene 23.58 +- 0.17 on vs
  24.95 +- 0.22 off (-5.5%), I-cache misses +17%, fetch stalls +28%; later stretch even (12.21 vs 12.10, medians
  12.38 vs 12.46), fetch stalls +15%. Fewer words executed per guest instruction, but tails compiled into several
  traces make the hot code bigger. **What the 2DS rewards is less distinct hot code** (static words of the blocks in
  use), not fewer executed words: OPT_RETSTUB removed ~1.8% of static hot words and gained ~1-1.6%. The two old
  trace-mode fuzzer failures no longer reproduce (default seeds 31 / 41, chain 5 / 2 with traces on: 0 fails).
- **Baseline of the new defaults (jobs 152-153, 120 s):** core 0 cutscene 25.56 / 25.61% (+-0.21), speed ~21.3%.
- **Next: OPT_CRSTUB (job 155):** a compare's CR update (BIC + 3 conditional ORRs) as one BL to a shared stub per field,
  signedness and register; ~3 words fewer per compare and record form, ~3-4% of static hot words.
- **OPT_CRSTUB (job 155): neutral** (core 0 cutscene 24.91 on vs 25.06 off, later 12.49 vs 12.47); off. Three words
  fewer per compare were paid back by the call and return (BL / BX on every compare).
- **ab_split.py fixed (2026-10-14 night):** tp850's cutscene ends at ~301 s and a heavier section (~5%) starts at
  ~580 s; with the A/B's phases at the same times in every job, those rows always fell into the same arm and inflated
  its spread (+-0.25 instead of +-0.07). It now splits at 303 s, drops 8 s around the split and everything from
  576 s, and settles 4 s after each switch. All results above use it.
- **Next:** OPT_ROTMASK (rlwinm / rlwimi as one to three shifts instead of a rotate and a mask constant of up to four
  words; rlwinm averages 4.9 static hot words) after its fuzzer gate.
- **I-cache misses by TP function (qemu 952-975 from `/root/tp_arm_950.state`, defaults, JIT_BLOCKMAP):** the same
  region split as 852-880 (block code 48%, gcemu C 35% mostly GX, cold 5%, stubs 4%), but **by block the misses are
  concentrated** where time is not: top 100 blocks 37%, top 1,000 76% of block-code misses. Audio channel updates
  (JASChannel updateDSPChannel / updateEffectorParam / updateAutoMixer, pow2, JASTrack callbacks) ~12%, PSMTXConcat
  5.5%, __save_gpr / __restore_gpr 3.1% (each _savegpr_N entry compiles its own copy of the tail), J3DPSCalcInverse-
  Transpose 2.2%, J3DGD* setup ~1% each. They run periodically and are evicted in between. Scale: block-code misses
  are ~17% of core 0's cycles (fetch stalls ~23%, ~3/4 of them block code), so each of these is worth ~0.5-2%;
  native stand-ins (PSMTXConcat first, the audio channel math next) are the candidates, judged on the 2DS.
- **OPT_ROTMASK (job 156): within noise** (core 0 cutscene 25.21 on vs 25.00 off, later 12.49 vs 12.46); off.
  With CRSTUB neutral too, shaving words off common instructions has stopped paying: the gains of this session came
  from removing dispatcher / C round trips (stack: +2.2% / +4.2%), not from smaller sequences.
- **What still left compiled code (job 157, defaults on; new perf.csv lines):** indirect misses ~3,400/s, nearly all
  "slot taken"; "other" returns ~4,600/s, mostly polling loops (by design), mtmsr ~600/s; interpreter fallbacks
  ps_cmpo0 ~2k/s, lmw + stmw ~2.5k/s, mftb ~1k/s, divw ~1k/s, sraw ~0.7k/s.
- **OPT_NATIVE2 (ps_cmp*, lmw / stmw, divw / divwu, sraw compiled): default on.** Jobs 160 and 162: core 0 cutscene
  +0.5 / +0.9%, Link / Epona stretch +1.5 / +1.4%, I-cache misses there -9 / -4%.
- **OPT_IND2 (2-way indirect table): neutral, off** (job 161): slot-taken misses 3.7k -> 1.2k/s, speed unchanged. A
  dispatcher round trip costs much less than OPT_SYSCALL / OPT_HLELINK suggested; those removed tens of thousands per
  second plus interpreter work. The fast tables are 16-byte aligned now (the pair lookup read malloc's header).
- **Fuzzer fixes:** chain cases set LR / CTR to the program's real address (they pointed at the last case's), zero the
  exception vectors' page in both machines, and skip cases whose program stored into itself (no icbi: the interpreter
  runs the new bytes, compiled code the old). Default mode with fault mode off, seed 1 case 1423 differs on a denormal
  single (the JIT flushes it, as documented): pre-existing, outside the standard gates.
- **Device profile (job 146) is flat outside the shadow scene: no TP function above ~0.25% of core 0**, so the I-cache
  miss clusters (audio channel updates, PSMTXConcat) are not stand-in targets after all.
- **Where the next real step is:** in the cutscene core 0 alone runs ~25% but the game ~21.5%, because core 0 waits for
  the GX thread ~11% of the time; the GX side (core 2) and the vertex worker (core 3) are worth up to ~+15% there. In
  the core-0-bound stretch, what is left in this JIT is ~1% items; more needs registers and FPRs across blocks.
- **Owner direction (2026-10-15): GX side next (cores 2-3, the cutscene's limit), then back to the JIT for the rebuild**
  (registers and FPRs kept across blocks; the peephole route is exhausted). The rebuild is a committed next step.

## 2026-10-15: GX side (cores 2-3), first look

- **Where the cutscene's time goes on cores 2-3 (job 163, defaults):** core 0 waits for the GX side 14%; the GX thread is
  81.5% busy, ~23% of it waiting for space in the vertex-job ring (`valloc`, ~5 jobs of ~21 KB fit in 128 KB) while the
  worker is 61% busy (gather and colours ~70% of it, random reads of guest vertex arrays); ~63k draws/s merged into
  ~2,900 jobs/s; the GPU ~11.6 ms per frame. `core/tools/gx_split.py relay_N.csv --col C --val V` prints these per arm.
- **Rejected: the GX thread running queued jobs while it would wait (`--steal`, job 164, off).** It ran ~400 jobs/s and
  the GX-side waits fell (ring 21 -> 16%, core 0's wait 10 -> 8.8%), but core 0 got slower in both stretches (medians
  cutscene 25.37 -> 24.52, later 12.31 -> 11.72): gathers on another core's schedule cost core 0 its shared L2, as the
  second worker did (C2, jobs 125 / 127). Moving gather work around does not pay; less of it might (C1).

## 2026-10-15: back to the JIT, grounded; OPT_BEFLOAT measured (job 166)

- **The redesign was checked against the records first** (OPT_JIT_PLAN.md 2026-10-15 section). Registers across
  blocks stays ruled out (region model ~1.3%); the grounded candidates were SETEND BE float stubs, C8 single
  precision, constant addresses.
- **OPT_BEFLOAT (float stubs in big-endian data mode): neutral, off.** Job 166: core 0 cutscene 25.09 on vs 25.28 off,
  later 12.51 vs 12.50; I-cache misses +6-7% with it on: ~800 words of new stubs beside the old ones (the stores of
  values not known single still use the originals) cost about what the shorter latency chains saved. Exact (fuzzer,
  qemu frame). If retried, the BE versions must replace the originals, not join them.
- **Where this leaves core 0:** every change that lowered the hot footprint or removed excursions into C paid; every
  change that only shortened latency or executed instructions did not. The grounded JIT items left are ~1% each.
  Larger steps need either a decision on exactness (timing of event delivery: budget checks only at loop heads, ~7% of
  executed hot words; or CR / register dead stores across blocks) or fault mode (+11% core 0 measured, off for a 3DS
  kernel crash while delivering a data abort, jobs 86-90).

## 2026-10-15: owner decision items: budget checks only where loops close (OPT_LATCH), fault mode's crash

- **Owner decision (James, 2026-10-15):** event timing may drift and dead state may differ at block exits, if the 2DS
  shows a real gain; fault mode's kernel crash gets a root-cause investigation. Each item behind its own bit, kept only
  on a 2DS A/B gain. Recorded in docs/OPT_JIT_PLAN.md.
- **Modelled first (JIT_REGSTAT "owner-decision models", TP 850-880, mask 627):** 30.3% of block runs are in blocks
  with a backward exit; the rest could drop the budget branch, 0.70 hot words per block run (2.5%; the "~7%" above
  was wrong: each block's count must stay, only the branch goes). Dead write-backs at block exits: of 1.56 per run at
  constant exits, 0.46 are dead on every path from the target (0.66 trusting the calling convention at calls); at
  blr / bctr 0.12 of 0.42 by the return convention. ~1.6% exact-enough; not built yet.
- **OPT_LATCH (4096; `--latch`, `--ab latch`):** a block keeps the budget branch only if a scan of its guest code finds
  a branch to its own address or below (`block_may_loop`; traces, OPT_CALLS, polling blocks and stand-ins always keep
  it). Every loop of linked blocks passes such a block; ST_INDIRECT now checks the budget on a hit (exactly what the
  target's own check did, so behaviour with the bit off is unchanged); the dispatcher enters at `Entry.pro`. Blocks
  still count every instruction, so emulated time stays exact; only where a slice ends drifts. Fuzzer full gate clean
  with 627|4096 (chain mode: 67% of blocks without the branch, ~1,400 runs past their budget per 20,000 cases, the
  interpreter runs what the JIT reports, 0 fails). qemu TP 850-880: same final PC, GX bytes and last frame (19k of 7.1
  G cycles drift); hot 9.4 -> 9.3 and exit stubs 3.1 -> 2.8 words per guest instruction.
  **2DS job 169 (cancelled at 204 s, cutscene only):** core 0 26.06 +- 0.24 on vs 25.46 +- 0.23 off (+2.4%),
  I-cache misses -4%, fetch stalls -3%; game speed unchanged there (GX-bound). Job 170 runs the full A/B.
- **Fault mode, root cause so far.** Luma's dump of the old crash (job 88): kernel prefetch abort at pc fffffffe with
  r9 = 0x94, i.e. inside svc 0x94 (Luma's InvalidateEntireInstructionCache, a tail call into a stock kernel routine; its
  svc entry re-enables interrupts), which our fault handler issued from inside the user exception handler; the 2DS
  probe's 202 faults (job 85) never made a system call there. DFSR / FAR in that dump are stale from our data abort.
  Step 1: `jit_fault` only rewrites the site; `fault_sync` in jit_entry syncs later (fuzzer: fault mode on, chain and
  default, seeds 1 2 3 7, 0 fails). **Job 167 still died** (~390 s, after 6,166 faults; earlier runs: ~700, ~2,700).
  Step 2: `--faultsync off`, no cache maintenance for patches at all (a stale copy faults again into the same slow
  path). **Job 168** stopped at 406 s right after an A/B switch (8,011 faults), but left no new Luma dump and no crash
  record; whether the 2DS crashed is open. Job 167's dump (#9) was marked reported by a run outside any relay job:
  `--luma-dumps 9,10,11` (job 170) prints it. Luma research (k11_extension sources): the TLS redirect is the stock
  kernel's; Luma only decides fatal / not; its custom svc table has no per-thread exception state.

## 2026-10-15 (afternoon): full-length A/B data; the relay log fix

- **The relay logs of jobs 168-171 had been read only to ~410 s:** the relay returns at most 200 chunks (~400 s of a
  run, one chunk per 2 s) per request, and `relay.sh log` stopped paging once a job was done. Fixed (it pages until a
  page brings nothing new, and starts the file fresh); all three logs now run to 600 s. Analyses before this fix used
  only the first ~400 s of the later stretch.
- **OPT_LATCH, full data:** later stretch 12.56 vs 12.41 (job 170) and 12.51 vs 12.44 (job 171); cutscene 21.86 vs
  21.45 and 21.72 vs 21.51. Small, same direction every time: stays the default.
- **Fault mode without any cache maintenance for patches (`--faultsync off`, job 168), full data:** later stretch
  **12.90 vs 12.41 (+3.9%, ~6 SE)**, core 0 cutscene 27.29 vs 24.96 (+9.3%), and the run lasted 600 s through 8,011
  faults with no Luma dump (the earlier crashes: ~700, ~2,700, 6,166 faults; each in svc 0x94 per the dumps). Needs
  more crash-free runs before a default; crash-risk runs only with James able to restart the 2DS.
- **Relay heartbeat:** `relay.sh status` (nakama-backend GET /status): the 2DS's last poll and log post.
- **Hardware probes for the timing model:** `--timing-probe only` jobs (CPU, memory, TLB, kernel, SD, clocks in
  timingprobe.c; PICA200 in gpuprobe.c). First device runs: jobs 174-175. Matrix: docs/research/TIMING_MODEL_PLAN.md.

## 2026-10-15 (evening): timing probes, where core 0's time goes, OPT_ICBI

- **Timing probes** (core/host3ds/timingprobe.c, gpuprobe.c; `--timing-probe only`): the New 2DS XL measured mechanism by
  mechanism (jobs 176-183), results and readings in docs/research/TIMING_MODEL_PLAN.md; survey of further probes in
  docs/research/BENCHMARK_TOOLING_SURVEY.md.
- **TP's core 0 budget** (job 184, clean run without A/B, 20.5% speed; core/tools/frame_budget.py): GX wait ~11%
  (idle hook busy-waits for core 2), instruction-fetch stalls 21%, data stalls 22%, real dispatcher < 1%, compiling
  ~0.5%. A/B runs recompile everything at each switch and ran at 15.5%: compare A/B phases with each other only.
  The sampler's "dispatch" bucket is jit_entry with no block running, i.e. mostly the idle hook's GX wait.
- **Link requests** (job 185, new counters): 155,000/s only in the state-load window, 10-140/s in play: nothing to fix.
- **OPT_ICBI** (308e516, off): icbi inside blocks. Under qemu (TP 850-880) the dispatcher's "other" returns fell from
  636,584 to 23,792 with identical frames; on the 2DS (jobs 186 off / 187 on, 60 s) the load window's ~622,000 icbi
  exits were unchanged and speed identical. Open: why the 2DS still leaves after each icbi (the mask was on, opt 12915).
  It only matters while loading; stays off.
- **Next:** the levers by measured size: core 2's GX work (core 0 waits on it), I-cache footprint, data stalls; then
  C18 (byte-swapped RAM, ~3-4% priced). Native mode remains the full-speed path (docs/research/NATIVE_PORT_PLAN.md,
  DECOMP_SURVEY.md).
- **Compact vertex-worker jobs (`--vin`, c93ac0c), 2DS A/B job 190: neutral, off.** Kept per job 20 -> 11 KB, jobs in
  flight 3.4 -> 5.6, `valloc` 11.8 -> 10.0%, but waits for ring space stayed ~650-700/s (each job still reserves the
  full-size maximum, ~22 KB, before it trims) and speed medians were equal (cutscene 21.75 / 21.91, later 12.00 /
  11.97). Core 0's median GX wait in this run was 1.1-1.4%: job 184's 11% mean comes from a few heavy scenes, so the
  GX side is not core 0's limit most of the time. Re-evaluate with a reservation sized from the draw's registers if
  the GX side becomes the limit.
- **Hot-block relayout (`--relayout`, c93ac0c): the 2DS stopped at the first relayout (job 188, ~20 s, no log).**
  The same path under qemu (JIT_RELAYOUT_TEST, 3 relayouts of up to 3,861 blocks) ran cleanly: 2DS-specific. Step
  logging added; the next device attempt waits for James at home (crash-risk rule).
- **Hot-block relayout, 2DS A/B job 195: rejected (off).** With step logging the relayout ran cleanly every 24 s
  (230-270 hot blocks of the current block key compiled first, ~50 ms); job 188's stop did not recur. Cutscene neutral
  (21.22 / 21.63), the later stretch **-4.5%** (12.43 -> 11.87) with I-cache misses **+40%** (4.31 -> 6.06 M/s):
  laying hot blocks out first raised misses, as traces did (job 154). The key filter (blocks of the key at the perf
  loop's moment only) kept the list short; not worth pursuing given the direction.
- **Denser indirect table (FAST_BITS=13, --ind2), job 193: neutral** (TIMING_MODEL_PLAN.md).

## 2026-10-17 (night): the batch's draw call, raw draws and raw state (jobs 361-369)

- **Measured first:** a batch's citro3d draw call wrote ~111 command words and cost ~9 us; uploading less (lean state,
  C24) barely changed that, so the cost was citro3d's per-call code, cold in core 2's I-cache. **Raw draws + raw state
  (C25, default on):** the draw block and the dirty state blocks written by pica.c in citro3d's exact words (compared
  with citro3d on the first 256 calls and one in 4,096, self-test `--rawplant on`): 8.15 -> 5.3 us a call, flush_batch
  61 -> 41 ms/s, Ordon +0.73% (+0.2% for the draw block before it). Command buffer prefetch: -0.6% (C26).
- **Core 0 is the limit again:** in the Ordon frame core 0 waits for the GX thread only ~0.65% of its time now, with
  the GX thread ~87% busy. GX-thread work buys at most that until core 0 gets faster.
- **The walk benchmark:** after ~60 s the square walk usually parks Link at a villager (one static frame, 7,618
  draws/s), but not in every run (job 369). Score A/B runs with `core/tools/ab_phases.py LOG COL 60 [ON]`, never pooled
  means (the first, different scene always falls in the off arm). OPT_PIPEK re-checked in one run: +6.6% (job 369).
- **Lean state hung the GPU** with all bits on (jobs 361-362; `gx at 6`: waiting in C3D_FrameBegin). It stays off; the
  watchdog lines now name where the GX thread is (`pica_watch`, GW_* in pica.h).

## 2026-10-17 (late): core 0 in gameplay, and the next session's plan

- **Profile with the new defaults (job 370):** GX SDK 23%, J3D 14%, J2D / JUT 13%, flat (top function 3.5%). psq_st
  to the pipe through the shared stubs (OPT_PIPEPS, C27) cut ~45,000 jit_gather calls a second for +0.3% (not kept);
  the per-site version lost 2.3%. Targeted GX-side fixes are now worth <= 1% each.
- **Float work is ruled out as the next lever (corrected the same night).** Relaxed floats were already built
  (OPT_FASTFLOAT) and measured: no gain on the title benchmark (job 338) and +0.5% at 1.7 SE in gameplay (job 373).
  Removing all rounding to single gains nothing, so C8 (cheaper rounding) cannot either; it is not the next step.
- **What core 0's time is (job 279 and the synthesis):** ~11 of ~22 cycles per guest instruction are the JIT's code in
  cache; the rest is instruction fetch at scale (hot code ~16x L1), data stalls and block boundaries. Every lever that
  grew code lost (traces, float lanes, per-site inlining tonight); peephole shaving was neutral. Full speed (~7.7
  cycles) needs less guest work or a different execution model, not cheaper instructions: the candidates are
  frame skip (C9, +10%, visual glitches, a user option), native code for whole subsystems (NATIVE_PORT_PLAN.md; the
  profile is flat, so whole subsystems, not functions), and the owner's call on timing / exactness trade-offs.
- **Inclusive profile (job 374, `core/tools/stack_funcs.py`):** rendering 56.5% of core 0 against game logic 15.8%;
  the 2D UI (J2DScreen::draw) 24.8%, the HUD meter alone 16.4%. **Proposed first native subsystem: J2D** (panes,
  pictures, text boxes, materials, J2DPrint and JUTResFont), a closed library whose output is GX commands, checkable
  byte for byte against the JIT's FIFO output on the PC (as the stand-ins' HLE_CHECK). Design questions before code:
  the game's own J2D subclasses (virtual drawSelf in dMsgScrn*), how a native call tree enters and leaves guest state
  once (C23's lesson), and the decomp source (zeldaret/tp, matching) as the reference.
