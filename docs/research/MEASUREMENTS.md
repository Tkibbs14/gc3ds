# gc3ds measurements catalogue

Every measurement the project has made, in one place: 2DS device runs (relay jobs 2-244), hardware probes
(timingprobe, gpuprobe, hwprobe, PMU), the qemu cache models, PC checks, Azahar and native mode. Built on 2026-10-16 by
a measurement-audit workflow that read every doc, all relay logs, probe outputs, source comments and memory notes
(1274 measurements from 13 readers, 1274 after merging repeats), then had five
assessors judge sufficiency, a critic find unread sources, and two verifiers check each headline number against its source.

**Rules (James, 2026-10-16):** read this before proposing a redesign, a probe or an A/B. After every device run or
probe, add the result here in the same commit as the change, and mark what it supersedes. Always name the section
(cutscene / later stretch / movie, or the native scene) and the trust level: `device` numbers decide; `pc`, `qemu_model`,
`azahar` and `estimate` only explain.

**Newest first (2026-10-20):** the entries from "The frame-rate line was broken" to "The triangle path as a native
stand-in" (all defaults by the night: heavy area ~22.8% / 6.84 frames a second, Ordon ~31.95% / 9.53, title 37.0% /
23.5% / 13.4%; core 0 the limit in every scene, from the joint states). Before them (2026-10-18): "The first
library port, J2DMaterial::setGX, and pipe bursts with one copy" (the
state on 10-19: Ordon 28.0%, heavy area 20.6%, title cutscene 36.1% / later stretch 19.0% / movie 12.8%; the GX
thread the limit in gameplay, core 0 in the later stretch; see "The title's sections moved" there), and before it the sections from "Three scenes after the texture work, and what limits each" (gameplay
~20-23.5%, core 0 the limit everywhere, the GPU and GX side off the critical path). The verdict and headline
numbers below are the 10-16 audit's and describe that state (GX thread ~80-88% busy, the GPU slow on FCRAM textures).

## Verdict: is it enough to work with?

Yes for choosing a direction, no for predicting what a given redesign will gain. The hardware is measured well and the measurements repeat. The probes repeat within 1% across jobs 176-196, and the PMU is validated on the device. The data rules out most incremental JIT and GX ideas, and it shows the scale of the problem. Core 0 is 4.7x short of full speed in the cutscene and 8.7x short in the later stretch. The JIT's touched hot code is about 1.9 MB against a 16 KB L1I. The CPU vertex path caps at about 300-340k vertices/s in both modes while the PICA can do about 20 M. Five things stop the data from ranking or pricing specific redesigns. (1) About 21% of core-0 cycles land in no PMU event: VFP latency and taken-branch bubbles. (2) The cutscene A/B noise floor is 2-3x larger than ab_split reports, so most verdicts of 0.5-3% are unresolved. (3) Native mode's converted-shapes path has no number because it deadlocks. (4) Everything comes from one save state (tp850, the title attract demo) with no player-controlled gameplay and no emulated audio. (5) No emulator-mode run has been made since the native commits changed the shared GX code. Most gaps can be closed cheaply with existing tools, and several of them offline.

| Area | Status | What we know | What is missing |
|---|---|---|---|
| Core-0 cycle budget and JIT codegen | not_enough | Clean baselines exist (jobs 157 and 184). The cycle split is fetch 21-24%, data 22-23%, other ~53%, IPC 0.32 in the cutscene. Per guest instruction: 35.1 cycles in the cutscene, 13.3 in the later stretch, 60.4 in the movie. The dispatcher is under 1%, compile 0.2-0.7%, fallbacks 0.15-0.56%, branch mispredicts under 1%. About 25 A/B verdicts exist. The interlock model was re-scored with job-183 device latencies: only +3-6% more stall, and it still covers only ~34-36% of device cycles. | Device attribution of the ~21% (cutscene) / ~18% (later) unattributed cycles: VFP dependent latency and taken-branch bubbles. Device cost of each guest construct class (float, paired-single, memory, branch) with the real emitted sequences. Accounting of the ShdwDraw stand-in inside ins_k, which may confound the later-stretch 13.3-cycle figure. Fault-mode stability, which rests on n=1. |
| Memory hierarchy and code layout | mostly | Measured on device: L1 is 4 cycles, L2 ~28, FCRAM 102-166, 32-byte lines, not LRU, one outstanding miss, write-allocate. In-order lines cost ~3x less than random. ldr+rev costs 6 cycles vs 4 for ldr. MEM1 is page-aligned. Footprint is now counted offline from ic_w850/ic_w950: 59.5k hot lines (1.86 MB) touched, and 24k lines (756 KB) hold 95% of JIT misses. Simulated misses per field (392-471k) match the device (~472k). In dcache2, 14.3% of L1 D-misses also miss L2, and 1.9% once the JIT-buffer artefact is removed. That favours the interlock / L2-hit reading of the data stall. | A device split of stall_data into L1-hit waits, L2 hits and FCRAM misses. Simulators with random replacement and the 16 MB JIT buffer, calibrated against device A/B deltas (traces, relayout, BEFLOAT). The VRAM side of the texture-contention probe. The source of TP's 0.13-0.36 M TLB misses/s. The device JIT's 'hot 2.0 words/guest' differs from qemu's 3.8-4.6, unexplained. |
| GX / GPU pipeline (emulator and native) | mostly | In the cutscene the GX thread is 80-84% busy (~23% of it ring wait) and the worker ~61% (gather 61-68%). The GPU is ~8-16% busy at c3d_gpu_ms 10.5 ms per submitted frame. citro3d costs 3.2 us CPU per draw, and merging already joins 96.4% of draws. Fill is 204 Mpx/s. Texture formats: ETC1 117, RGB565 27, RGBA8 17 Mpx/s minified. Shader length costs ~1.5 ns per instruction per vertex. Negative results with mechanisms: CPU DL cache, vw2, steal, deep rings, deferred gather. | Any number for native converted shapes (jobs 242/244 hang). GPU cost of the real GX lighting shader at 44-79k vertices per frame. True GX-thread command-decode cost at 30 fps (sampling 1 in 16 with a 0.25 us tick inflates it). Core 2/3 PMU events. A current per-job cost model; the per-draw figures are pre-merge. The DMA/transfer timing conflict (0.57 vs 1.7-2.0 ms; 600-900 vs 230-250 MB/s), probably a sync artefact. Whether job 184's GX figures still hold after the shared-GX native commits. |
| TP workload characterization | not_enough | Opcode mix (PC): loads 22%, stores 15%, memory ~40%. Per field: ~1.7 M guest instructions in the cutscene, ~8-10 M in the Link/Epona stretch. A family profile (hist_900.bin, PC 900-1020) gives J3D 19.8%, JAS audio 15.6%, J3DGD 14.7%, GX SDK 9.4%, collision 8.6%, PSMTX 7.6%; the top 500 functions cover 95%. GX per field: ~4,870-5,090 draws, ~24-26k vertices. Disc and texture decode matter only during loads. Idle share varies from 9% to 51% by stretch. | Player-controlled gameplay, which has never been measured in emulation (tp850 is the attract demo). Guest audio cost, since dsp_hle produces no samples. Per-subsystem frame structure and what the idle time waits on. Device cycles per function rather than instruction counts. A second device-loadable save state: tp_1100-2900 predate state v2 and may not load. |
| Native mode | not_enough | Game logic is cheap. The title runs at 60 fps with busy median 8.9% (job 204). Hyrule Field, idle, uses 14.8% of 33 ms (job 210). Display lists by reference took the title demo from 80-100% to 53-66% busy (job 219). Through gc3ds GX the title demo runs at 5-7 fps with the GX thread saturated at ~50.7k CPU-gathered vertices per frame (jobs 229-238). A 4 MB ring moved the wall to the worker (job 240). | Converted-shapes performance, blocked by a deadlock at frame 241. Logic cost with movement, combat and audio. The input path has no effect (jobs 210/211). PMU, A/B harness or sampler in native mode. logic_ms includes the limiter sleep. |
| Methodology and trust | mostly | CPU and GPU probes repeat within 1%, except code install (10-35%). The PMU is accurate on core 0. The later stretch resolves ~0.3% (null rms 0.17-0.32%). Large verdicts are robust. Azahar, qemu instruction counts and the arm11 model all failed as speed predictors. Relay logs 168-171 are now complete. relay_88a.csv is the deduplicated copy of job 88. | A corrected cutscene noise floor: the null A/B gives rms 1.8-3.0% core 0 with lag-1 autocorrelation 0.74, and ab_split's SE treats rows as independent. An arm-order swap: jobs 132-134 replicate the same arm alignment. A pass that re-scores the pre-fix verdicts. The harness's own observer cost (relay and SD on core 1). Clock and thermal drift. Frame-time distribution rather than means. |

## Headline numbers (each checked against its source by two verifiers)

- **corrected** (0/2): Clean baseline at current core-0 defaults: 20.5% speed. Instruction-fetch stalls 21%, data stalls 22%, real dispatcher under 1%, compile ~0.5%. A/B runs read 15.5% because they recompile at each switch. *Source:* core/out/relay_184.csv; docs/research/TIMING_MODEL_PLAN.md:177-189
  - *Correction:* The job 184 numbers reproduce. frame_budget.py over 61-299 s gives speed 20.5%, fetch stalls 21.2%, data stalls 22.2%. The sampler puts compile at ~0.6% after 60 s, with ~8,500 dispatcher returns/s. Two later clean 300-s runs (191, 193) read 20.4%, and no later run changed the defaults. The 15.5% explanation is wrong. 15.5% is the A/B runs' 600-s mean, and that window adds the later stretch (~12.4%) and the THP movie (~4.3%). A clean 600-s run with no A/B (job 157) also reads 15.3% overall, and GX-only A/B runs (164, 190) read 15.2-15.3%. So the window explains the gap, not the recompile. The recompile does cost something inside a section: JIT-toggle A/B runs reach the end of the cutscene at ~300 s instead of ~250 s, and their mean speed over 30-250 s is 20.3-20.4% against 22.1%.
- **verified** (2/2): Cost per guest instruction differs by section. Cutscene: 35.1 cycles, 11.3 host instructions, IPC 0.32. Later Link/Epona stretch: 13.3 cycles, 6.35 host instructions, IPC 0.48. Movie: 60.4 cycles, 27.7 host instructions. Any budget must name its section. *Source:* core/out/relay_157.csv via core/tools/frame_budget.py, windows 30-250 / 265-475 / 510-600 s
- **corrected** (0/2): Size of the gap: full speed needs ~102 M guest instructions/s in the cutscene (4.7x today's 21.9 M/s) and ~486 M per emulated second in the later stretch, where nothing is idle-skipped (8.7x). The remaining JIT items measure ~1% each. *Source:* core/out/relay_83.csv, relay_97.csv, relay_157.csv (ins_k / speed_pct)
  - *Correction:* Cutscene: 21.9 M/s and ~102 M (4.6x) come from job 83, an older 12 Oct run. At current defaults (jobs 157 and 184, 30-250 s) the cutscene runs 22.9 M/s and needs 103.7 M per emulated second, a 4.5x gap. Later stretch: 486 M per emulated second holds, and it comes out at exactly 486.0 in every run. The emulator charges one Gekko cycle per guest instruction there, so this figure is the 486 MHz clock under that accounting, not a measured demand of TP's code. Today's rate is 60-61 M/s at 12.4-12.6% speed, so the gap is 8.0-8.1x, not 8.7x. Job 97, an opt 0/1 A/B, gives 9.2x. 'Remaining JIT items ~1% each' leaves out two levers: fault mode, measured at +9.3% / +4.1% core 0 (job 168), and C18 byte-swapped RAM, priced at ~3-4% but not measured. HANDOFF says only the 'grounded' exact items are ~1% each.
- **verified** (2/2): About 21% of cutscene core-0 cycles (~18% later) fit neither executed instructions nor counted stalls. VFP dependent latency (8 cycles, stall_data 0.01) and the ~3-cycle taken-branch bubble land in no PMU event. *Source:* core/out/relay_183.csv '# timing op v*', '# timing btac', '# timing indirect' lines
- **corrected** (1/2): Hot-code footprint, counted offline: 59,524 hot JIT lines (1.86 MB) touched in steady state, and 24,186 lines (756 KB) miss at least once per field, holding 95% of JIT misses, against a 16 KB L1I. Simulated misses per field (392-471k) match the device (~472k). *Source:* //wsl$/Debian/root/jitwork/ic_w850/icache.txt minus ic_smoke/icache.txt; ic_w950 minus ic_w950s; device figure from TIMING_MODEL_PLAN.md:185 (job 184)
  - *Correction:* The footprint numbers reproduce. ic_w850 minus ic_smoke (28 fields) gives 59,524 hot-region lines touched (1.82 MiB / 1.90 MB; '1.86 MB' mixes units). It also gives 24,186 JIT lines with at least 28 misses (756 KiB, 95.3% of JIT misses); w950 gives 24,414 lines and 95.8%. The 'match' is built by choosing regions. 392k / 471.7k per field is JIT (345k / 415k) plus libc (~47k / ~56k), with all gcemu C code left out (223k / 256k per field, ~36% of simulated misses). That C code includes GX work, which runs on core 2 on the device. It also includes core 0's own dispatcher, interpreter, events and HLE, which the device's count includes, and no split exists. Counting every region the sim gives 616-728k. The device's ~472k is job 184's 61-299 s mix; the cutscene alone (30-250 s) is ~459k. The sim windows (fields 852-880 and 952-975) cover only the cutscene's first ~2 emulated seconds, from a 14 Oct ARM-Linux build that predates OPT_NATIVE2.
- **corrected** (0/2): Every change that grew distinct hot code lost on the device even when it executed fewer instructions: traces -5.5% core 0 (I-misses +17%, job 154), relayout -4.5% (I-misses +40%, job 195), AOT -31 to -36% (job 144), BEFLOAT neutral (I-misses +6-7%, job 166). *Source:* core/out/relay_154.csv, relay_195.csv, relay_144.csv (deduplicated), relay_166.csv via core/tools/ab_split.py
  - *Correction:* The numbers hold, each in one section only. ab_split today gives traces 25.10 -> 23.64 core 0 in the cutscene (-5.8%; HANDOFF says -5.5%), I-misses +16.5%, and the later stretch even. Relayout: later stretch 12.51 -> 11.93 core 0 (-4.6%, speed -4.5%), I-misses +40.5%, cutscene neutral. AOT, with job 144 deduplicated (932 duplicate rows): -36% cutscene, and -31% later with the default 303 s split. With job 144's own boundary (its cutscene runs to ~350 s) AOT is -36% in both. BEFLOAT: neutral (-0.8% / +0.1%), I-misses +6.1-6.9%. The mechanism is not shown for relayout. It flushes and recompiles the same sampled hot blocks first, every 24 s; no measurement says its distinct hot code grew. So it shows that layout or flushes raise misses, not that footprint growth does. AOT executed more instructions (~17% under qemu), not fewer.
- **verified** (2/2): Memory hierarchy on the device: L1 4 cycles, L2 27.7-29.2 (64 KB-1.5 MB), FCRAM 102/128/156/166 cycles at 3/4/8/16 MB. Only one miss is in flight at a time (146-156 cycles with 1-6 chains). Neither cache is LRU. *Source:* core/out/relay_183.csv lines 105-121 (chase), 136-163 (assoc), '# timing mlp 8M'
- **corrected** (0/2): Simulated data misses: 14.3% of compiled code's L1 D-misses also miss L2, and only 1.9% once the PC-only JIT-buffer-as-data artefact is excluded. Data stalls are therefore mostly L1-hit load-use waits and L2 hits, not FCRAM. *Source:* //wsl$/Debian/root/dcache2/dcache.txt (342,367 / 2,394,943 L2 misses)
  - *Correction:* The numbers reproduce: 342,367 / 2,394,943 = 14.3%. JIT-buffer lines read as data account for 839,486 L1 misses (35%) and 312,782 of the L2 misses; the rest is 29,585 / 1,555,457 = 1.90%. The 'therefore' does not follow for the device. dcache.c's L2 sees only compiled code's data accesses on core 0. It does not see code fetches, which put ~1.9 MB of hot JIT lines against the 2 MB shared L2. It does not see cores 2-3 or the GPU. It models the L2 as LRU, which the device's is not, and its own header calls its L2 misses a lower bound. Device probes show that other traffic matters: with core 2 streaming, the 1 MB chase goes from 28 to 116-124 cycles, and GPU texture reads take it from 34.6 to 70 ns. Treating JIT-buffer reads as PC-only is also unverified: the 2DS code loads literals from its buffer too. The device counters cannot settle it either. At 1.37 M D-misses/s, the misses cost ~20% of the 178 M data-stall cycles/s if all are L2 hits, and more than 100% if all go to FCRAM.
  - *Settled on the device, 2026-10-30 (job 1569, TP heavy area, today's defaults):* **most data misses do reach FCRAM, so the simulation's 1.9% does not describe the console.** The run gives D read misses 1.61 M/s and D write misses 0.645 M/s (2.25 M/s together) against **core 0's own L2 misses 1.83 M/s** (`scu_c0miss_k`), i.e. ~81% of core 0's data misses also miss L2. Those L2 misses are predominantly data, not code: the 8.20 M L1 I-misses/s cost 26.9 cycles each on average (220.17 M fetch-stall cycles/s), which is L2 latency (27.7-29.2), and even a tenth of them reaching FCRAM would raise that average to ~38. The stall arithmetic agrees: 1.83 M x ~115 cycles = 210 M against 177.51 M measured data-stall cycles/s (over, so some overlap or write buffering), whereas 1.9% to FCRAM would predict only ~65 M - far below what is measured. **So the data half of core 0's stalls is FCRAM latency on guest memory, and the dcache.c model's L2 is too optimistic for this hardware** (its own header calls its L2 misses a lower bound, it models LRU where the device is not, and it sees neither code fetches nor cores 2-3).
- **verified** (2/2): Fault mode with --faultsync off is the largest measured JIT lever: core 0 +9.3% in the cutscene and +4.1% later, over 600 s and 10,589 faults without a crash. That is one run; earlier runs crashed the kernel after 685-6,166 faults. *Source:* core/out/relay_168.csv via ab_split.py --col faultmem; relay_86/88/167.csv
- **corrected** (1/2): The shared L2 couples the cores. Core 2 streaming stores raise core 0's 1 MB chase from 28 to 117-124 cycles. A second vertex worker cost core 0 -7.4% (jobs 125, 127). GX-thread job stealing cost -4.8% (job 164). *Source:* core/out/relay_176-196.csv '# timing xcore'; relay_125.csv, relay_127.csv, relay_164.csv
  - *Correction:* Chase: holds. 1 MB alone is 27.7 cycles, and with core 2 streaming it is 116.2-123.6 across jobs 176-196. The 8 MB chase goes from 156 to 603 cycles, so memory/bus contention adds to L2 eviction. Second vertex worker: -7.4% / -7.3% applies to the cutscene only, with the default split. ab_split's fixed 303 s split fits only JIT-toggle A/B runs; in GX-only A/B runs the cutscene ends at ~250 s. With that boundary the loss is -7.9% / -8.0% in the cutscene and -3.3% / -3.1% later. Job stealing: -4.8% is the later-stretch median from a 303-576 s window. In job 164 that window holds ~100 s of the THP movie (from ~475 s), and the 'cutscene' window holds ~50 s of the later stretch. With the run's own boundaries, core 0 loses 3.3% in the cutscene (25.71 -> 24.85) and 2.1% later (12.70 -> 12.43; medians -1.4%).
- **verified** (2/2): GPU sampling of an FCRAM texture slows core 0's memory: a 1 MB chase goes 34.6 -> 70 ns and an 8 MB chase 194 -> 703 ns. This supersedes GPU_GUIDANCE rule 25. The VRAM arm never ran, and only 1,266 KB of VRAM is free. *Source:* core/out/relay_192.csv, relay_196.csv '# gpu contention chase'
- **corrected** (0/2): GX side in the cutscene: the GX thread is 80-84% busy, ~23% of that waiting for ring space. The worker is ~61% busy, 61-68% of it gather. Core 0's GX wait is 12-14% mean but 1.1-1.4% median, because a few heavy scenes carry the mean. *Source:* core/out/relay_163.csv (gx_split.py), relay_184.csv, relay_190.csv
  - *Correction:* The busy shares hold. GX thread median 80-83%. valloc (the GX thread waiting for ring space, common.h:47) is 20.5-24.5%. Worker 58-62.5%. 'Gather and colours' is 57-69% of worker time per 10-s line. The median claim is wrong. In the cutscene, core 0's gxwait median equals the mean or exceeds it: job 163 (25-290 s) mean 12.0 / median 12.5; job 184 (25-240 s) 14.6 / 15.4; job 190 (25-236 s) 13.2 / 14.1. A 1.0-1.2% median appears only over a whole 600-s run (163, 190). There roughly half the rows are the later stretch at ~0.5-0.7%, giving a whole-run mean of ~6%, not 12-14. So the cutscene is uniformly GX-heavy; it is not a few heavy scenes. Separate defect: gx_split.py and ab_split.py hard-code the cutscene end at 303 s. In runs without a JIT-flushing A/B (101, 130, 157, 164, 184, 190, 191, 193) the cutscene ends at real_s ~238-256 (emu_s ~66.1-66.3, the same emulated point as ~294 s in 131-134 and 163). For 184 and 190, gx_split's 'cutscene' therefore includes ~45-60 s of later-stretch rows, which lowers its means.
- **corrected** (1/2): Both modes cap at ~300-340k CPU-gathered vertices/s: the emulator cutscene does 316-330k/s, and the native title demo ~50.7k per frame at 6.7 fps. The PICA does ~20 M vertices/s (49.7 ns each) and is ~8-16% busy (c3d_gpu_ms 10.5 ms median at ~7.3 submits/s). *Source:* core/out/relay_184.csv verts_s, c3d_gpu_ms; relay_229-238.csv gx column; relay_180.csv/relay_183.csv '# gpu vertex'
  - *Correction:* The cap holds; the PICA busy range is overstated. Emulator cutscene verts_s: median 316k/320k/323k (relays 163/184/190), trimmed mean 305-319k, p25-p75 283-339k. Native title demo (relays 229/231, cumulative gx vertices per frame): 50.7k at about 7.0 fps; at 6.6-6.8 fps it is 47.8-49.9k. Over all demo rows it is 44-62k per frame at 5.4-7.6 fps and 264-374k/s. PICA: '# gpu vertex' gives 49.6-49.8 ns per vertex (relay_180/183), but that uses the probe's minimal shader (see the shader-length fact). In-game GPU frame time is 10.45 ms for about 44k vertices. Busy share: per-row c3d_gpu_ms x gpu_submits_s has a median of 7.3-8.1%, mean 7.7-8.5% and p90 12-13%. The claim's own inputs (10.5 ms x 7.3/s) also give 7.7%, so the 16% upper bound is not supported. c3d_gpu_ms is C3D_GetDrawingTime of a single frame sampled once per row, not an average.
- **corrected** (1/2): Native mode: game logic is cheap on the ARM11, 8.9% busy at 60 fps on the title (job 204) and 14.8% of a 33 ms frame in idle Hyrule Field (job 210). Rendering through gc3ds GX gives only 5-7 fps. The converted-shapes path deadlocked at frame 241 in both runs, so it has no number. *Source:* core/out/relay_204.csv, relay_210.csv, relay_229-240.csv, relay_242.csv, relay_244.csv
  - *Correction:* The numbers hold. Job 204: busy median 8.8% (mean 9.0) at 60 fps, and NATIVE_PORT_PLAN.md reports 8.9%. Job 210: median 14.8% of a 32.8 ms frame at 30 fps. Jobs 229-240: medians 6.0-6.7 fps (range ~5-10). Jobs 242 and 244 both show 'GX thread: no progress' (fixed tail) and then 'hang: no game frame for 20 s at frame 241'. relay_242 holds the same run printed twice, so 242 and 244 are the two runs. The generalization fails on conditions. Both cheap cases ran with nothing drawn and audio off (NATIVE_PORT_PLAN.md items 3-4): the logo/opening scene at ~10 KB of GX a frame, and Link standing still. The heavier in-engine title demo with the same stubs is not cheap. Job 213: busy median 56% at 30 fps. Job 217: 84%. Job 219 (display lists by reference): 54%, plus ~44 s at 17.4 fps and 100% busy. So in a real scene the game's own core-0 work is ~50-85% of a 33 ms frame.
- **verified** (2/2): The A/B noise floor differs by section. In the cutscene a null A/B (no change at all) gives core-0 arm differences of 1.8-3.0% rms (max 4.4%), with row lag-1 autocorrelation 0.74, so ab_split's ~1% SE is 2-3x optimistic. In the later stretch the floor is 0.17-0.32% rms. Replicates 132-134 hit identical emulated time in the same arm. *Source:* Null A/B over core/out/relay_101/130/157/184.csv (scratch scripts C:/Users/tyler/AppData/Local/Temp/null_ab.py, ac.py); relay_132-134.csv emu_s/opt columns; core/host3ds/main.c:1005-1018
- **corrected** (1/2): Guest work by library family (PC, fields 900-1020): J3D 19.8%, JAS audio 15.6%, J3DGD display-list writers 14.7%, GX SDK 9.4%, collision 8.6%, PSMTX math 7.6%, __save/__restore_gpr 2.7%. The top 500 functions cover 95%. The device sampler's 'flat profile' (D3) could not see family totals. *Source:* //wsl$/Debian/root/aot/hist_900.bin (decoded with zeldaret symbols)
  - *Correction:* Decoded /root/aot/hist_900.bin (196.2 M instructions, 98.1% inside zeldaret functions). J3DGD 14.74%, GX SDK 9.39% and __save/__restore_gpr 2.63-2.67% match. Collision is 8.5-8.8%, J3D 18.4-19.8% depending on the prefix rule, and JAS 15.1% for JAS-named functions (all audio 16.2%). PSMTX does not match: PSMTX/PSVEC/PSQUAT total 4.87%, of which PSMTXConcat is 2.53%. Reaching about 7.5% needs mDoMtx/cXyz wrappers added. The top 500 functions cover 93.7% of executed instructions (95.5% only of function-attributed ones), while the family shares use the all-executed denominator; 95% of all is reached at 601 functions. The field range 900-1020 rests only on AOT_PLAN.md:95. This histogram is not flat even per function (J3DGDSetLightAttn 3.49%, top 10 = 20.6%), which conflicts with D3's 'no function above 0.5%' (PERF_LEVERS.md:93). That is a PC instruction count against device time samples.
- **verified** (2/2): Vertex-shader length costs GPU time: +0/+32/+64 instructions give 49.6/99.2/188.6 ns per vertex. gcbench G1's 'free' result is implausible: pad128 at 6 Mtri/s would need ~2.3 G shader instructions/s against a ≤1.07 G/s peak. *Source:* core/out/relay_183.csv '# gpu shader'; bench/results/gcbench4_2026-10-11.txt L435-446
- **corrected** (0/2): Memory headroom in emulator mode is tight: largest malloc block 3.9-4.2 MB, linear memory free 11.6-13 MB, VRAM free 1,266 KB, JIT buffer 41-71% full. *Source:* '# heap' lines and linear_free_mb / vram_free_kb columns, core/out/relay_171-196.csv
  - *Correction:* VRAM holds: vram_free_kb median 1266 in every run 171-195 (1522 only at start). Largest malloc block is 3.9 MB in 184-195 and 4.2 MB in 171, but 5.7 MB in job 193 (the --ind2 build), so the range is 3.9-5.7 MB. linear_free_mb per-run medians are 11.8-14.2 MB (means 12.1-13.7, rows 10.1-14.6), not 11.6-13. JIT buffer: the '# jit code ... % of the buffer' lines and the jitcode_pct column run 35-48% by the end of the 300-s runs (184, 191, 193). 190 reaches 66-68% at 600 s. 171 and 195 (opt A/B, which flushes) stay ≤42%. No value of 71% appears; the maximum is 67.7% (jitcode_pct) / 68% (note).
- **verified** (2/2): The interlock model (arm11_sched) covers only ~34-36% of device core-0 cycles (8.8-9.3 of 25.8 per guest instruction). Re-scoring with device latencies moves it only 3-6%, and it widens rather than closes the model-vs-device gap on fcache. The missing term is fetch and footprint, not latencies. *Source:* //wsl$/Debian/root/sch3/model.txt, sch7, schb7 re-scored with job-183 latencies; TIMING_MODEL_PLAN.md:189
- **verified** (2/2): The documented fcache (C11) rejection does not reproduce. Current ab_split gives +1.5 to +2.0% in the cutscene and +0.3 to +1.6% in the later stretch, never negative; C11 recorded -2.7 to -4.4% later. C11's baseline (22.4) also differs from ab_split's (24.4). *Source:* core/out/relay_131-134.csv via core/tools/ab_split.py --col opt; docs/research/REDESIGN_CANDIDATES.md:148-151

## Gaps, ranked (cheapest measurement with the existing tools)

1. **Native converted-shapes path has no fps, GX-thread, worker or GPU number: both device runs deadlocked at frame 241 (GX ring tail stuck at 00347d24).** Measure: Reproduce the hang offline from the stream dump of frames 236-241 (GX_AURORA replay on PC). After the fix, run one native relay job with a gxopt on/off toggle for native shapes, logging the existing native perf columns plus C3D_GetDrawingTime. (device, crash risk: only with James home). Unlocks: Whether native rendering can reach 30 fps, and whether GX-side redesign beyond converted shapes is needed
2. **About 21% (cutscene) / 18% (later) of core-0 cycles are unattributed: VFP result latency, taken-branch bubbles, LDM/STM, indirect calls.** Measure: Add rotating PMU pairs in core/host3ds/pmu.c (PC-changed / taken-branch, D-cache read) and run one clean 600 s tp850 relay job read with frame_budget.py. Offline, re-score //wsl$/Debian/root/sch3/sched.bin with arm11_sched.py at VFP 8, taken branch +3, indirect ~28 and compare with the device remainder (~170 M cycles/s cutscene, ~146 M later). (device). Unlocks: Pricing C8 single precision / float-in-VFP against block layout (fall-through) before building either
3. **The cutscene A/B noise floor is understated 2-3x (autocorrelated rows), and replicates share the same arm alignment, so most 0.5-3% cutscene verdicts are unresolved.** Measure: Offline first: replace ab_split.py's row SE with a block bootstrap over phase pairs and re-score jobs 117-171. Then two device jobs: one A/A on an inert option (e.g. --ab icbi) and one contested A/B (fcache) started from the other arm. (offline). Unlocks: Which of fcache, retstub (cutscene), kstore, sched, pld, splitopen, vin, steal and VIx stand; the minimum effect worth building; replicates needed per A/B
4. **No emulator-mode TP run since job 196, although the native commits (3c5251a, d7a9948) changed shared GX code: fifo.c per-command tick sampling, pica.c, vfast.c, vertex.c, main.c.** Measure: One clean 300-600 s tp850 relay run at HEAD (no A/B), read with frame_budget.py and gx_split.py and compared with job 184. (device). Unlocks: Whether the job 184 baseline (GX 80-84% busy, gxwait 11-14%, 20.5%) is still the current baseline every redesign is measured against
5. **Fault mode's +9.3% core-0 gain rests on one crash-free run; earlier runs crashed the kernel at 685-6,166 faults.** Measure: Two or three more 600 s relay runs with --ab faultmem --faultsync off on tp850, read with ab_split.py, tracking the Luma arm11 dump count. Run only while James is home. (device, crash risk: only with James home). Unlocks: Fault mode (fastmem) as a default and as the memory-access basis of any rebuilt JIT
6. **GPU cost of a GX-style lighting/texgen vertex shader at native vertex counts (44-79k per frame) is unknown. Job 181's slope predicts 10-20+ ms of a 33 ms frame.** Measure: One probe-only gpuprobe job running the actual gxpass_native.v.pica / gxpass lighting shaders over 60k indexed strip vertices. Repeat G1's configuration with the pad results feeding an output. (device). Unlocks: Whether the GPU becomes the next wall once converted shapes move transform and lighting onto it; vertex vs fragment lighting
7. **Per-command GX-thread decode cost at 30 fps: ~39k non-draw BP/XF/CP commands per frame. The sampled timing is inflated by the 0.25 us tick pair, yet it still implies 40-75% of core 2.** Measure: Offline, count commands per frame by class from the existing gx_dump stream. On device, time whole command batches with one tick pair (or read the core-2 cycle counter) in one native relay job with profiling on vs off. (device). Unlocks: Whether the GX decoder (state handling, register-write caching) needs a redesign alongside converted shapes
8. **Code-layout changes cannot be predicted before a device A/B. Footprint is now counted, but the simulator uses round-robin replacement and has never been checked against device deltas.** Measure: Offline: switch //wsl$/Debian/root/jitwork/libicache.so (core/tools/qemu-plugins/icache.c) to random replacement. Check that it reproduces the device icode sweep (16/20/24/32 KB: 0.002/0.053/0.084/0.125 misses per instruction). Replay the job 154, 166 and 195 builds and compare predicted and measured miss deltas (+17%, +6-7%, +40%). (offline). Unlocks: Pre-screening any codegen or layout redesign offline instead of by an 11-minute device run
9. **Later-stretch accounting: '# hle 10s' reports 36-44 M stand-in instructions/s against ins_k of 55-58 M/s. If the stand-in is counted in ins_k, ~70% of that stretch is one ShdwDraw loop and its 13.3 cycles per guest instruction is a blend.** Measure: Read jit.c:226-238, 3789-3794 and 5214 to see whether jit_hle's return reaches instrsRun. Run a PC PCHIST from tp_arm_850 over the stretch with HLE off. Re-split relay_145/146 on the windows where '# hle 10s' is non-zero. (offline). Unlocks: Whether the core-0-bound stretch is a general-JIT problem or a single-function (shadow) problem
10. **Everything comes from tp850 (the attract demo). No player-controlled gameplay has been measured, and the existing tp_1100-2900 states predate state v2.** Measure: Make a gameplay state on PC (Hyrule Field or the Ordon cutscene at 2700-3300, which PC logs show at 4,433 draws/field with lines and points), convert it via the qemu ARM build, profile it with PC NODRAW + PCHIST, then run one clean 600 s device job read with frame_budget.py. (device). Unlocks: Whether the cutscene / later-stretch budgets and the vertex/GX targets generalise to play
11. **The device split of the data stall (L1-hit waits vs L2 hits vs FCRAM) is still inferred. The simulator favours L2 hits but uses LRU and an 8 MB buffer.** Measure: Offline: rerun dcache.c with random replacement and the 16 MB buffer over TP 850-880 and predict stall_data = 2.75 x dependent L1-hit loads + ~28 x L2 hits + ~150 x FCRAM misses, against the device's 172-190 M cycles/s. Add CORE_DATA_CACHE_READ to the same pmu.c change as the unattributed-cycles gap. (offline). Unlocks: C18 byte-swapped RAM (ceiling ~1.3% cutscene / ~3.3% later), load scheduling, literal/table placement
12. **Texture placement and transfer costs: the VRAM-vs-FCRAM contention arm never ran, and DMA timings disagree 3x (0.57 vs 1.7-2.0 ms; 600-900 vs 230-250 MB/s), probably a sync-method artefact.** Measure: One probe-only gpuprobe job: the contention case with a 512x512 RGBA8 texture in VRAM vs FCRAM (fits free VRAM), plus display transfer / TextureCopy timed by both the tick around C3D_Sync* and the completion interrupt, back to back. (device). Unlocks: VRAM texture-cache policy and the per-frame cost of EFB/XFB copies (~0.3-0.5 ms vs ~2 ms each)

## Directions the data supports, and what it has ruled out

- Change core 0 by a large factor, not by tuning. Full speed needs 4.7x in the cutscene and 8.7x in the later stretch (relay_83/97/157). The last JIT A/Bs gave ~0.5-2% each (NATIVE2 +0.5-1.5%, LATCH +0.8-2.4%, CRSTUB and ROTMASK neutral). Native mode is the only measured route with that headroom: game logic runs 8.9% busy on the title (job 204) and 14.8% of 33 ms in idle Hyrule Field (job 210).
- Judge every codegen change by distinct hot code, not executed instructions. Shrinking footprint paid: memory stubs took I-misses 12.36 -> 9.65 M/s for +5.5%, cold-region stubs +7%, shared float stubs -10% I-misses, removing C round trips +2.2% / +4.2%. Growing it lost: traces -5.5%, relayout -4.5% (I-misses +40%), AOT -31 to -36%, BEFLOAT neutral (I-misses +6-7%). The touched hot set is 1.86 MB against a 16 KB L1I and 2 MB L2. Above 2 MB of code, fetch rises from 3.4 to 5-6 cycles per instruction (relay_183 icode).
- Move vertex data and transform out of the per-frame CPU path into GPU-resident buffers (C1 / converted shapes). Both modes cap at ~300-340k CPU-gathered vertices/s while the PICA does ~20 M/s and is 8-16% busy. 99.5% of draws come from display lists and 83-90% replay identical bytes (relay_184 '# dlist'). A CPU-side cache cannot pay: hits cost ~1.4 us/vertex against ~1.35 us to gather (jobs 113-119). Format rules for the buffers: strips are 2.4x faster than lists, in-order indices 2.7x faster than shuffled, palette index in position.w is free, shaders kept short (~1.5 ns per instruction per vertex).
- Keep work on cores 2-3 compact, and don't parallelise memory-streaming work against core 0. Core 2 streaming raises core 0's L2-resident loads from 28 to ~120 cycles. vw2 cost -7.4% and steal -4.8%. Kernel DMA (192 MB/s) leaves core 0's chase unchanged, so it suits bulk copies (relay_196).
- Spin with timeouts for cross-core handoffs, never kernel events. The spin round trip is 0.45-0.5 us. svcArbitrateAddress loses ~1 wake in 4 (501 of 2000). LightEvent times out (gcbench5 C3, hwprobe).
- Fault-mode (fastmem) memory access is the largest remaining JIT lever: +9.3% core 0 in the cutscene and +4.1% later (job 168), against check hoisting's +5.7% / +2.8% (job 117). It is gated on kernel stability.
- Texture decode should target ETC1 or RGB565 with mipmaps. Minified 1024x1024 without mips: ETC1 117, RGB565 27, RGBA8 17 Mpx/s (job 180). At 1 texel per pixel RGBA8 loses 45%. VRAM placement is suggested because FCRAM sampling doubles core 0's L2 latency, but it is unmeasured on TP and only ~1.2 MB of VRAM is free.
- Offload audio to the 3DS DSP. GC DSP-ADPCM through ndsp has correlation 1.00000 (A1). 24 channels cost core 0 nothing (A2). Software mixing is 0.42% of a core per voice. WSOLA time-stretch is 0.66% of a core.
- Ruled out by device data (do not re-propose without a new argument; several of these verdicts came from A/Bs that recompile all JIT code, which read the on arm ~2% low, and IND2 and float lanes were reversed by separate runs on 10-19, see "Float lanes and the 2-way indirect table, re-measured"): dispatcher or indirect-table redesign (dispatcher under 1%; IND2 neutral then, now on: +0.85%; FAST_BITS=13 neutral); faster compile (0.2-0.7%); traces, OPT_CALLS, relayout, AOT / static recompilation (footprint losses above); registers across blocks or loop regions (modelled 0.5-2.5% of hot words); CR dead-store elimination (~0.3%); peephole word shaving (CRSTUB, ROTMASK neutral); interlock scheduling alone (SCHED within noise against a modelled -18%); branch-prediction work (mispredicts under 1%); VFP short vectors (no throughput gain); CPU display-list vertex cache; second vertex worker; GX-thread stealing; deferred gather; deeper job rings (1 MB 18.9% vs 256 KB 20.9%); OPT_ICBI (matters only while loading); a single-function stand-in outside the shadow scan (no device block above 0.3%). Re-measured with separate runs on 10-20 ("The rest of the ruled-out switches, re-measured"): traces, the second worker, stealing, sched off, the display-list cache and a bigger job ring still lose; icbi, qbig, feeddirect and cmdpld are even; the flush-prim stand-in (`--psflush`) gained +1.7% (Ordon +0.8%) and is on by default.

## Conflicts and stale numbers

- C11 fcache verdict ('net zero, later -2.7 to -4.4%') does not reproduce with the current ab_split (+0.3 to +1.6% later, never negative). The cutscene gain sits inside the cutscene noise floor. Re-evaluate before treating C11 as closed. **Resolved 10-19:** separate runs give +1.1% / +2.4% / +0.6% (cutscene, later, movie), heavy area +1.5%, Ordon +1.6%; on by default.
- Data-stall interpretation: HANDOFF L828-852 reads it as interlocks (~50 cycles per miss); the relay-mid reading is FCRAM (~150 cycles). The dcache2 simulator (1.9-14.3% of L1 misses reach FCRAM) favours interlocks plus L2 hits, but it is not device-confirmed.
- Shader length: gcbench G1 / GPU_GUIDANCE rule 3 'free' vs gpuprobe jobs 181/183 (+32 instructions doubles cost per vertex). G1 is arithmetically implausible, so trust jobs 181/183. PERF_LEVERS, C6 and the gpu-direction memory still cite 'free'.
- GPU_GUIDANCE rule 25 'GPU does not slow CPU memory' (gcbench5 M2, GPU fill only) is superseded for FCRAM texture sampling by jobs 192/196 and still cited in PERF_LEVERS.md:68-70.
- '4th attribute costs ~45%' (GPU_GUIDANCE rule 2, C6) is over-generalised. A 4-attribute 36-byte vertex with a float3 normal ran 6.01 Mtri/s; the loss is specific to the float1 4th attribute.
- DMA timings: display transfer 0.57 ms (gcbench 4b) vs 1.67-1.98 ms (gcbench 4, gpuprobe 178-180). TextureCopy ~600-900 MB/s vs 230-250 MB/s. Probably a sync or wait artefact; unproven.
- SD random 2 KB read: 2.39 ms (gcbench 3/4) vs 4.0-4.08 ms (timingprobe jobs 178-196). The methods have not been compared.
- Hoisting later-stretch gain: +4.7% (HANDOFF, main.c:392, older split) vs +2.8% (current ab_split on relay_117).
- Stack A/B (job 151): commit ca3793a subject +2.5% / +5.7% is superseded by +2.2% / +4.2% after the ab_split fix.
- Traces job 122 was filed 'within noise'; ab_split shows -5.2% (~5 SE), consistent with job 154.
- Cycles per guest instruction quoted as 25.8 (TIMING_MODEL_PLAN:189, mixed window), 33 (OPT_JIT_PLAN:23) and 35.1 / 13.3 (relay_157 by section). Always name the section.
- Stale: fetch-stall shares of 40-44% (HANDOFF L166-168, L232-236, L449-451; jit.c:4052; AOT_PLAN). Current is 21-24%.
- Stale: arm11_sched opcode tables and D2's '43% float' (10-13 code, never device-validated). The model covers only ~34-36% of device cycles.
- Stale: per-draw GX costs of 16-18 us/draw (pre-merge builds). GPU '~20 ms of ~190 ms per frame' (REDESIGN_CANDIDATES:232, PICA200.md:15) is superseded by 10.5 ms median per submitted frame (job 184).
- Stale: memory note 'Run JIT blocks under SETEND BE (adopted)'. jit.c uses SETEND only in the OPT_BEFLOAT float stubs, which are off; integer loads still use ldr+rev.
- Stale source comments: pica.h:44 ring 256 KB (actually 128 KB); main.c:306-309 HLE off (actually on); main.c:360 --vin on (actually off); pmu.h four pairs (actually five).
- Doubtful claim corrected: 'libicache.so is not built'. It exists at //wsl$/Debian/root/jitwork/libicache.so (2026-10-14), and complete I-cache runs exist in ic_w850 / ic_w950.
- Doubtful claim corrected: 'float cost has no device measurement'. bench/results/latest.txt (gcbench run 3) has VFP latency/throughput (f32 8 / 1.1, paired 9 / 2.28, div 19 / 33). What is missing is device cost per guest construct in TP code.
- Job 168 '8,011 faults, stopped at 406 s' was a relay truncation; the full log runs 600 s with 10,589 faults. relay_88.csv holds its run twice; use //wsl$/Debian/root/relay_88a.csv. relay_72/73/104/107/144/146 are also duplicated, relay_111/112 lack headers, and relay_126/167 end in HTTP 429.
- The device JIT reports 'hot 2.0 words per guest instruction' while qemu reports 3.8-4.6 for similar builds. Definitions differ; unexplained.
- icbi's 3.8% of modelled cycles comes from a one-off boot/state-load burst (622,595 in every run from the 850 state), not gameplay. The model share is inflated.
- The workload lens's 'ShdwDraw ~70% of later-stretch instructions' rests on unverified counter semantics; the sampler says 12.5% and the measured gain is only +16%. Unresolved.

## 2026-10-16 device runs (jobs 245-251): baseline, the missing cycles, noise floor

- **Job 245** failed: tp850.state would not load after the native commits added fields to GXCore (now native-only, 187b0f1).
- **Baseline at HEAD (job 246, tp850, 600 s, no A/B), device:** cutscene (30-250 s) speed 21.4%, 36.2 core-0 cycles and
  11.8 host instructions per guest instruction (IPC 0.33), fetch stall 22.4%, data stall 23.3%; later stretch (265-475 s)
  12.5%, 13.2 cycles (blended, see below), fetch 14.7%, data 18.8%; movie (510-600 s) 4.1%, 64.1 cycles. Matches job 184
  (20.5%, fetch 21%, data 22%) within noise: the native commits did not slow the emulator.
- **The later stretch's 13.3 cycles per guest instruction is a blend** (jit.c: a stand-in's instruction count is taken
  off the block budget, so it lands in ins_k). Job 157, later stretch: 60.2 M guest instructions/s of which 44.2 M (73%)
  are the ShdwDraw HLE stand-in; the remaining JIT code costs **~39-47 cycles per guest instruction** (stand-in at 1-4
  cycles each), about the cutscene's 35-36. Core 0 costs ~35-45 cycles per real guest instruction in every section.
- **What the PMU events count (probe job 251, device):** `pcchg` (PC changed by software, event 0x0F) counts every taken
  branch, folded or not (predictable loop branch 1.06 per iteration; random branch 0.56). Folded branches (`folded`,
  0x09) cost nothing (3.44 cycles for 3.3 instructions). A taken branch that is not folded costs **~3 cycles** (BTAC
  loop: 4.0 cycles per 1-instruction iteration, `brnopred` 1.0). A call plus return is 2 PC changes, ~10 cycles beyond its
  instructions with 2-4 of them stack data stalls; an indirect jump ~19 cycles beyond. Dependent results: vadd/vmul
  single and double 8 cycles, vmul.f64 9, vdiv/vsqrt 19, integer mul 5, ldr 4, vmov to core 10, with **stall_data ~0**
  (invisible to every stall event).
- **Where the missing ~20% of core-0 cycles goes (job 247, new event pairs, device):** cutscene 803 Mcycles/s = 259
  instructions + 183 fetch stall + 188 data stall + 16 LSU-full + **157 unattributed (19.6%)**; later 137 (17.1%); movie
  128 (16.2%). Cutscene branch events per second: 38.0 M branches, 34.7 M PC changes, 4.9 M folded, 12.2 M not predicted,
  0.54 M mispredicted; 54.2 M D-cache reads. **Unfolded taken branches (~29.8 M/s x ~3 cycles) explain ~11-13% of core
  0, mispredicts ~0.5%, and the remaining ~6-7% fits float / multiply result latency.** 1.6 taken PC changes per guest
  instruction: every guest load and store (~40% of guest instructions) is a `bl` to a shared memory stub and a `bx lr`
  back (jit.c emit_load / emit_mem_stubs), plus block links and conditional branches. This is the mechanism behind fault
  mode's +7-9%: it inlines the access with no call.
- **Noise floor, A/A (job 248, --ab icbi, an inert option):** core 0 +0.24% (95% CI +-3.15) cutscene, +0.35% (+-1.72) later:
  a clean null; one 600 s run resolves ~+-3% cutscene, ~+-1.7% later.
- **fcache with the arm order swapped (job 249, --ab-flip on):** +1.22% (+-2.58) cutscene, +1.12% (+-1.73) later. With jobs
  131-134 every fcache run is +0.3 to +2.7%, never negative: C11's rejection is wrong; fcache is a small gain (~1-2%),
  below a single run's resolution.

## Fault mode on the 2DS, 2026-10-16 (jobs 252-258)

- **Speed (job 252, --ab faultmem --faultsync off, 600 s, device):** core 0 **+7.20% (95% CI +-4.90)** in the cutscene,
  **+5.14% (+-2.41)** in the later stretch. Overall speed: cutscene 21.33% -> 20.88% (no gain: core 0 waits longer for
  GX, which is near its limit there), later stretch 12.53% -> 13.01% (**+3.8%**, core-0-bound). Fault mode only pays
  where core 0 is the wall until the GX side gets cheaper.
- **Stability:** job 252 ran 600 s, 10,909 faults, no crash (with job 168: two clean 600 s runs). Job 253 crashed the
  console at 145 s / 2,762 faults; job 254 within ~20 s of starting. Earlier: 685-6,166 faults (jobs 86-90, 167).
- **The crash is in the kernel, in Luma3DS's exception delivery** (Luma ARM11 dump crash_dump_00000011, reported in job
  255): CPU mode 0x13 (SVC), pc fffffffe, lr ffff0260 (the exception vector page), DFSR 0x805 (translation fault,
  write), FAR 541ab000, r9 0x94. A user exception handler on a retail kernel works only because Luma enables it
  (libctru thread.h: UNITINFO / svcKernelSetState 6), so the code that delivers each fault to jit_3ds.c's handler is
  Luma's. The cache sync is not the cause (crashes with --faultsync on and off); Luma's ranged cache SVCs 0x91/0x93
  are ruled out separately (2026-10-10: they reached a core running gsp). Per-fault crash odds look like ~1 in a few
  thousand.
- **Test of the delivery path (job 256, --faultmem on full-time, --faultsync off, --faultdata fixed):** the kernel wrote
  each fault's data to a fixed buffer instead of the handler's stack. **Crashed 12.7 s in, after 909 faults**: the buffer
  is not the cause (257-258 cancelled). Three of the last four crashes came early in a run (12.7 s, ~20 s, 145 s), when
  faults come in bursts (~70/s while code is compiled): the odds look tied to the fault rate. Open: Luma's exception
  delivery under rapid exceptions; the installed Luma version (ask James); a handler on the faulting stack.

## Inline memory access vs the shared stubs (jobs 259-260, OPT_INLMEM, device)

`--ab inlmem`: checked integer loads/stores check and access inline (6 words, no taken branch on the fast path) instead of
`bl` to the shared stub and `bx lr` back (3 words, two taken branches). JIT fuzzer (checked mode, today's options +/-
the bit): 40,000 cases, 0 differences. Core 0, inline vs stub: job 259 cutscene +1.84% (95% CI +-3.57), later +0.26%
(+-1.62); job 260 (arm order flipped) +1.31% (+-3.62), +0.53% (+-1.96). Pooled about **+1.6% (+-2.5) cutscene, +0.4%
(+-1.3) later: a consistent small gain inside the noise; not the default.** The two taken branches saved and the three
extra words per access nearly cancel: removing the call pays only when the access stays small (fault mode's two words).

## The emulator's vertex shader on the PICA (probe job 261, device)

`gxpass.v.pica` with the backend's 68-byte vertex (vfast.h PVert, 7 attributes), 60,000 vertices, zero-area triangles:
**108.0 ns per vertex** with the CPU's clip position, **107.5 ns** with the transform in the shader (free: attribute
fetch, not arithmetic, sets the cost), **684.7 ns** with the eight-light lighting path (~72 ns per light). For comparison
a 12-24-byte vertex with a short shader runs at 49.7-49.8 ns. At TP's ~750k vertices/s (25k per field, 30 fps) a 1-3
light shader (~180-300 ns) would take ~15-25% of the GPU; eight lights ~51%. A converted vertex should carry only the
attributes it uses.

## Converted shapes run (native mode, job 263, device)

The native converted-shapes path (d7a9948) hung at frame 241 in jobs 242 and 244. Cause: texgen from the position loaded
a 3-float attribute from the 16-byte position buffer, a loader narrower than its buffer's stride, which hung the GPU in
probe job 177 too. Loading all four floats (the shader reads xyz) fixed it: **job 263 ran 90 s to frame 1,054 with no
hang.** Title demo, steady: **9-12 fps** (5.6-7.5 on the CPU vertex path, jobs 229-240): 65,385 converted draws from 86
conversions; the vertex worker 112 ms/s (was 300-700); the GX thread ~1,060 ms/s busy of which **~500 ms/s waiting for
the GPU** (GXSYNC): with models on the GPU, the GPU (transform, lighting at ~72 ns per light per vertex, fill,
textures) is now the wall.

## J3D display-list writer stand-ins (HLE, jobs 264-265, device)

core/src/cpu/hle_gd.c: native stand-ins for J3DGDSetLightAttn (190 instructions), SetLightPos and SetLightDir (115
each), 7.7% of TP's guest instructions in the PC profile (hist_900). Recognised by byte hash and shape (straight-line
apart from the compiled code's light-index clamp: index > 7 -> 0, not in the decompilation's header). PC HLE_CHECK over
2,400 fields: **1,629,894 calls compared with the interpreter, 0 differences** (ShdwDraw 6,122 calls, 0). TP calls
them ~40,000 times a second. Device, `--ab hlegd`, core 0: job 264 cutscene +1.99% (+-3.54), later +0.06% (+-1.66);
job 265 +2.81% (+-3.90), later +1.39% (+-2.33); pooled 10 pairs (29-250 s) **+5.17% (+-5.08)** cutscene, 8 pairs
later +0.77% (+-1.45). Real speed in the cutscene ~+2-3%. **Adopted** (positive in every reading, exact, no crash risk).

> **Correction, 2026-10-30 (jobs 1573, 1578-1583, 1588-1591): this group runs in none of the four states the project
> benchmarks on, so it contributes nothing to any headline number and the +2-3% cutscene credit cannot be
> reproduced.** Separate whole runs with `--hlegd off` against the defaults, two runs an arm: static Ordon 10.522 /
> 10.521 against 10.519 / 10.527; heavy area 7.224 / 7.225 against 7.215 / 7.227; tp850 cutscene 40.90 / 40.84
> against 40.91 / 40.87, later 22.98 / 22.99 against 23.03 / 23.06, movie 14.78 / 14.75 against 14.72 / 14.71. The
> arms are identical because `# hle 10s` reports **`J3DGD runs 0` with the group ON** in every one of them.
> **Why:** `hle_tp.c`'s dispatch runs `hle_ps_run` *before* `hle_gd_run`, and the light work in these states goes
> through `GXLoadLightObjImm`, an `hle_ps` stand-in (job 1590: 28,648 calls per 10 s). `hle_gd.c` serves the J3D
> *display-list recording* path (`__GDCurrentDL`), which these states never take. Note also that the figures above
> are pooled A/B means with uncertainties as large as the effect (+5.17% +- 5.08), the method since shown to produce
> phantom gains (C41).
> **Not turned off:** nothing here says it costs anything, and the recording path may be taken in scenes or games
> not benchmarked - this is a withdrawn credit, not a rejected option. `# hle 10s` now also reports `entries` and
> `refused`, because until today "the game never calls them here" and "recognition broke" were the same line
> (`runs 0`) on the device.
**Lesson:** 7.7% of guest instructions were ~2-5% of core-0 time: straight-line stores are cheap per instruction on
the 2DS. Rank HLE targets by device cycles per function (the sampler, `--profile on`, job 266), not instruction counts.

## Where core 0's cycles go by guest function, on the device (job 266, --profile on)

The block sampler (host3ds/jitsample.c, 54 reports of 10 s, ~100k samples each) grouped by function with the zeldaret
symbol map (core/tools/hot_funcs.py; /root/tp_symbols/symbols.txt outside the repo). Whole run: ShdwDraw 15.46%,
unmapped 11.51%, psdRealCallBack 0.67%, calcWeightEnvelopeMtx 0.64%, J3DFifoLoadPosMtxImm 0.60%, GetTriPla 0.56%,
getPolyCode 0.37%, SetupNP 0.33%, JASCalc::pow2 0.31%; the new J3DGD stand-ins' entries 0.1% each; outside compiled
code 11.1%. **By section (share of core-0 samples per 10 s report):**

| Section | Dominant guest code |
|---|---|
| Cutscene, 0-250 s | none: spread over hundreds of functions (plus the GX wall) |
| Later stretch, 250-460 s | **dBgWKCol::ShdwDraw 36-40%** (the shadow projection; its scan loop is already a stand-in, the per-triangle part is guest code) |
| Movie, 470-540 s | **one region, 0x80CE0000-0x80CEFFFF, 75-79%** (code loaded at run time, not in the map: the THP movie decoder, by its timing) |

**Consequences:** a full ShdwDraw stand-in could take up to ~40% of core 0 off the later stretch (~1.5x there); a native
THP decoder (Dolphin SDK motion JPEG, used by many games' movies) up to ~78% off movie playback (now 4.1% speed). The
cutscene has no single target: it needs the GPU port and the general JIT levers.

## The ShdwDraw stand-in's own cost (jobs 267-268, device, `# hle 10s` counters)

Per 10 s of the later stretch (steady reports 265-475 s; ms inside from g_profClock around scan()):

| Job | Build | Calls | Triangles skipped fast | Other guest instructions | ms inside | Cycles per guest instruction |
|---|---|---|---|---|---|---|
| 267 | registers in an array | ~215k | ~10.6M | ~301M | **~3,830** (38% of core 0) | ~7.0 |
| 268 | registers in locals (4aaa465) | ~217k | ~10.7M | ~304M | **~3,425** (34%) | ~6.2 |

- Per unit of work (ms per million other instructions) locals cost **11.5% less** (12.7 -> 11.3). Spills were a minor
  part: the stand-in still runs at ~6 host cycles per guest instruction (all guest instructions incl. the fast-skip
  loop's 13 each; 804 MHz).
- ~1,400 other guest instructions per call: each call walks ~20 grid cells (octree descent + the leaf checks) before
  it reaches an unvisited triangle and returns to guest code.
- **Where the stand-in's time goes (job 269, `--pmu-split hle`, later stretch, runs sampled 1/8; share of core 0's
  totals):** cycles 34-35%, ARM instructions ~51% (~189M/s of 371M/s), branch mispredicts ~77% (of 1.17M/s), data
  stalls ~35% (of 159M cycles/s), D-micro-TLB misses ~36%, store-buffer drains ~48%, but D-cache read misses only ~10%
  (of 866k/s), I-cache misses ~10%, main TLB 1%. **Instruction-bound, not memory-bound:** ~4.3 ARM instructions per
  guest instruction. Mispredicts (~0.9M/s) and D-misses (~87k/s) explain only a few percent of its ~277M cycles/s.
- **Tighter stand-in (job 270, PC HLE_CHECK 6,122 calls 0 differed):** guest memory through locals (base and limits no
  longer reloaded after every store; 0x80000000 mapping only, anything else stops it for guest code), CR0 computed once
  at the end, branch-free srw/slw, visited bits tested on one byte. ShdwDraw **~3,045 ms per 10 s while doing 6% more
  work** (231k calls, 323.6M other instructions): 9.4 ms per million against 11.3 (job 268) and 12.7 (job 267), **26%
  cheaper than at the start**; ~30-32% of core 0 (pmu split). **Later stretch speed 12.18% (267) -> 12.72% (268) ->
  13.46% (270, with the split's ~1% overhead on): +10.5%**, cutscene 21.4% in all three (no ShdwDraw there). Separate
  runs, not an in-run A/B; the later stretch's run-to-run spread is ±1.5-3.5%.

## Device cost of each guest instruction class through the real JIT (job 279, `--jit-bench only`)

host3ds/jitbench.c: loops of 32 copies of one guest instruction (+ addic. / bne), compiled with the run's JIT options
(opt 0x1273), code and data in L1, one run of 2 M guest instructions per PMU pair (job 278 was void: the loaded
state's pending interrupt ended every run after one block). Per guest instruction, loop overhead included:

| Class | Cycles | Host instructions | Note |
|---|---|---|---|
| addi, one register (dependent chain) | 1.74 | 1.53 | |
| addi / or, 8 registers rotated | 5.38 / 5.41 | 3.36 / 3.38 | **register-cache pressure: 2x the one-register case**, stall_data 2.0 |
| rlwinm / mullw / cmpwi | 7.27 / 10.13 / 8.22 | 4.30 / 4.33 / 6.18 | cmpwi: the CR update |
| divw | 32.73 | 18.45 | helper call (pcchg 3.8) |
| lwz / lbz / lhz | 4.88 / 3.94 / 7.72 | 4.47 / 3.53 / 4.47 | |
| stw / stb / sth | 7.63 / 5.74 / 7.63 | 4.47 / 3.53 / 4.47 | |
| lfs / lfd | 22.82 / 16.22 | 9.22 / 10.16 | shared stubs (pcchg ~2) |
| **stfs** / stfd | **39.75** / 19.91 | 12.05 / 10.16 | |
| psq_l float / u8 | 31.30 / 53.53 | 17.69 / 24.11 | |
| **psq_st float / u8** | **76.52 / 86.50** | 27.12 / 28.83 | |
| **fadds / fmuls / fmadds** (single) | **39.98 / 41.10 / 44.96** | 7.13 / 11.84 / 12.78 | **~2x the double forms**; the stalls are VFP latency (no PMU event) |
| fadd / fmadd (double) | 23.02 / 17.63 | 4.30 / 5.24 | |
| frsp / fctiwz | 29.68 / 28.00 | 5.25 / 12.78 | |
| ps_add / ps_mul / ps_madd | 31.77 / 51.55 / 59.09 | 11.84 / 21.26 / 23.14 | |
| bl + blr (per guest instruction; 33.7 per call) | 16.85 | 8.52 | |
| b +4 (taken) / bc not taken | 2.56 / 4.44 | 2.41 / 3.36 | |

- **Weighted by TP's dynamic mix (PC interpreter, 1.04 G instructions): ~11 cycles per guest instruction in cache.**
  The cutscene measures ~35, so **about two thirds of core 0's cost is what real code at scale adds**: instruction
  fetch from L2 (footprint ~16x L1), data stalls, and block boundaries (TP's blocks average 5.9 guest instructions with
  ~4.5 register loads / stores each; these loops are one 34-instruction block).
- Single-precision arithmetic, stfs and psq_st are the classes far above their double / integer counterparts: the
  rounding to single (vcvt pairs) and the store conversions. Weighted, the float classes are ~3-4 of the ~11 cycles.

### Block boundaries and footprint through the JIT (job 283, jitbench rings; job 281 for the in-L1 rows)

Rings of M blocks of K guest instructions (K - 1 addi over r3-r6, then a taken bne to the next block, 64 B apart),
cycles per guest instruction (per block). Job 283's "JIT code N% of its buffer" field is the build's host words per
guest instruction compiled, rounded (a label slip): **3 (K 12), 4 (K 6), 7 (K 2)**.

| K | M 16 (L1) | M 256 | M 1024 | M 4096 | I-misses per guest instruction at M 4096 |
|---|---|---|---|---|---|
| 2 | 5.00 (10.0) | 10.79 (21.6) | 19.55 (39.1) | 19.55 (39.1) | 0.50 |
| 6 | 4.46 (26.7) | 7.61 (45.7) | 11.20 (67.2) | 11.20 (67.2) | 0.38 |
| 12 | 2.68 (32.2) | 7.50 (90.0) | 8.02 (96.3) | 8.02 (96.3) | 0.25 |

- **A block boundary costs ~20 cycles with four live guest registers** (K 6 / K 12 at M 16: ~1 cycle per addi, ~21
  per block: written back at the exit, loaded again at the next entry), ~9 with one.
- **Out of L1 the cost per guest instruction grows 2.5-3.9x, in proportion to code bytes:** ~21 cycles per I-miss, one
  miss per 32-byte line of compiled code. Halving host bytes per guest instruction would halve fetch stalls.

### Where core 0's time goes by guest function with converted shapes on (job 284, `--profile on`, emulated windows)

- **Cutscene (19-66 s): flat.** The listed blocks cover 12.1% of samples; top functions 0.2-0.8% each. About half of
  the named ones are the game's audio engine (JASChannel::updateEffectorParam 0.78%, JASCalc::pow2 0.77%,
  JASTrack::updateSeq 0.67%, updateDSPChannel 0.66%, updateAutoMixer 0.41%, ...; the PC family profile put JAS audio at
  15.6% of guest instructions), then J3DUClipper::clip 0.71%, cBgS::GroundCross 0.56%.
- **Later stretch (69-97 s):** ShdwDraw **35.2%**; the SDK vector / matrix library next: PSVECDotProduct 2.18%,
  PSMTXConcat 1.90%, PSVECCrossProduct 1.53%, PSVECAdd 1.38%, PSVECScale 1.11%, PSMTXMultVec 0.40% (~8.5%);
  psdRealCallBack 2.07%, GetTriPla 1.62%, J3DFifoLoadPosMtxImm 1.55%, calcWeightEnvelopeMtx 1.37%.
- **Movie (99-110 s):** runtime-loaded code (THP decoder) 45.5%, Read 3.3%.
- Job 280 (and 282) listed host addresses / nothing: the sampler's work arrays failed to allocate with shapes' smaller
  heap (fixed: one allocation at sampler start).

### ShdwDraw's cell walk and the fast cells (jobs 285-288, device; PC HLE_CHECK 6,122 calls 0 differed)

- **Shape (job 285, per 10 s of the later stretch):** ~234k calls, ~3.1M cells (~13 a call), ~11.3M tree levels
  (**3.6 per cell**, each descent from the root), new leaf 89% of cells (its triangle list scanned, ~4 visited
  triangles each), the previous row's leaf 11%, empty leaves ~35%, ~0.22M triangles checked one by one.
- **Fast cells (job 286-287):** whole cells keeping only what carries to the next one, rewound to the exact code at any
  stop. 93% of cells run fast; rewinds per 10 s: ~80k for an unvisited triangle, ~150k at a row's end, ~16k at a slice
  end, 0 for addresses. **13.1 -> 10.8 us per call**; later stretch **14.39 -> 15.11%** (emulated window; job 287 also
  timed one call in 16 instead of every call: two clock reads were ~1.3% of core 0).
- **Why not more (job 288, `--pmu-split hle`):** the stand-in is 29% of core 0's cycles, 42% of its instructions,
  ~5,550 ARM instructions and ~8,800 cycles a call, **~420 instructions a cell**: head loads with checks ~45, 3.6
  levels x ~25, ~4 visited-triangle checks x ~18, tail and leaf bookkeeping ~70, the head snapshot and spills ~60.
  Store-buffer drains fell 259k -> 163k/s (their share rose: 84%). Next lever there: keep the descent path between
  neighbouring cells (most share their upper levels).

- **Descent cache (job 299, not adopted):** keep the octree path of the previous cell and descend from the deepest
  level the next cell shares. PC HLE_CHECK: 6,122 calls, 0 differed. Device: cutscene 27.94%, later stretch **15.41%**
  (against 15.11-15.4% without it); the stand-in still takes ~29.5% of core 0 (2,948 ms per 10 s, ~10.8 us a call).
  Within noise, so left out (C19).

### Core 0 by library family (job 289, `--profile on`, 1,500 blocks a report at 0.001%; hot_funcs.py --families)

| Family | Cutscene (listed 25.2%) | Later stretch (66.4%) | Movie (88.1%) |
|---|---|---|---|
| JAudio (JASChannel / JASTrack / JASCalc) | **8.36%** | 5.95% | 0.40% |
| game code and the rest | 8.95% | 5.79% | 4.04% |
| collision (ShdwDraw 31.4% of the later stretch) | 4.26% | **35.88%** | 0.83% |
| J3D | 1.87% | 12.31% | 8.43% |
| vector / matrix (PSVEC, PSMTX, mDoMtx, cM3d) | 1.36% | 4.80% | 2.77% |
| GX SDK | - | 0.00% | 12.35% |
| runtime-loaded code (THP decoder) | 0.15% | 0.09% | **53.66%** |

- **The cutscene is flat:** three quarters of its samples fall in blocks past the top 1,500. Its largest named family
  is JAudio (updateDSPChannel 1.09%, updateEffectorParam 0.98%, updateSeq 0.77%, pow2 0.74%, updateAutoMixer 0.56%;
  the guest's audio engine, whose output dsp_hle discards). Only general JIT work moves the cutscene much.

### THP movie decoder: the inverse DCTs as a stand-in (job 300, hle_thp.c)

- **What:** __THPInverseDCTY8 and __THPInverseDCTNoYPos (d_a_movie_player.rel, 44% of core 0 in the movie, job 289)
  as C generated from the module's own code on the disc (core/tools/thp_translate.py), recognised wherever the module
  loads (only its .rodata / .bss bases are relocated in these functions).
- **The decompilation's asm is not the shipped code:** a first version generated from it counted other instruction
  totals on rare paths (PC HLE_CHECK: a few calls in thousands). Generated from the disc's REL instead.
- **The FPU can be off at entry:** the OS leaves MSR[FP] clear after a thread switch and the first FP instruction
  traps to load the thread's registers (112 of 957,600 calls). The stand-in refuses then, and unless round to nearest,
  paired singles on, GQR5 = s16 loads and GQR6 = u8 stores at scale -3.
- **PC HLE_CHECK (tp_movie.state, fields 6000-6300):** 957,488 calls compared with the interpreter, **0 differed**.
- **Device (job 300, 640 s, first version):** ~230k calls per 10 s, ~490 guest instructions a call. Movie (emu
  99-110 s) **8.16-8.65% -> 11.23%** (jobs 277, 290, 291, 298 -> 300; +30%); GX thread 23-24.5 -> 31.8% busy.
- **Device (job 309, exact version, --profile on):** movie **8.46% (job 289, profile on) -> 10.91%** (+29%); ~90-120
  calls refused per 10 s (the FPU off), the rest stood in.

### Silent jobs were the relay running out of memory, not crashes (jobs 301-308)

- Jobs went quiet mid-run with no exception and no Luma dump; the 2DS showed "relay error: Failed sending HTTP
  request", then "Out of memory", every time (James). The emulator leaves the heap nearly empty (heap_free_mb 0.0 in
  every run; the profiler's ~1 MB of sample buffers made it worse): once the relay's connection dropped, the TLS
  reconnect could not allocate and every request failed until gc3ds restarted.
- **Fix (relay.c):** a 384 KB ballast taken at startup and freed at the first failed request, taken back between jobs;
  the sockets service and curl handle restarted after 3 failures in a row; `# relay network resets` lines with the
  2DS's address. main.c's watchdog notes where core 0 is when rows stop for 20 s inside a run.
- **The cause (job 310 still died 6.7 s in):** libctru gives the linear heap min(total - 24 MB, 32 MB); TP left
  7.7-11.4 MB of it unused while malloc started with a largest block of 2.4 MB. `__ctru_linear_heap_size` = 28 MB
  (job 311): largest malloc block **5.9 MB**, malloc free a steady 2.4-2.5 MB all run, linear free never under 4.7 MB;
  no relay failures in 640 s with --profile on. heap_free_mb now reads malloc's real headroom (it read 0.0 always).

### Profiles were missing their high addresses (fixed, job 311)

- The sampler's report built (PC, count) pairs in PC order and stopped at 4,096 distinct blocks; TP samples 4,600-7,300
  a report, so the highest addresses (the runtime-loaded modules first) were dropped silently. Earlier family tables
  (job 289: "listed 25.2%" in the cutscene) are short by that much. With room for 16,384 (job 311, emulated windows):

| Family | Cutscene (listed 79.1%) | Later stretch (89.1%) | Movie (84.7%) |
|---|---|---|---|
| J3D | **23.25%** | 12.32% | (not split) |
| GX SDK | **18.77%** | 7.79% | |
| vector / matrix (PSMTX, PSVEC, mDoMtx, cM3d) | 9.49% | **18.24%** | |
| game code and the rest | 9.78% | 6.47% | |
| OS / DVD / AI / DSP SDK | 7.21% | 4.83% | |
| JAudio | 5.06% | 1.35% | |
| collision | 3.35% | **36.64%** (ShdwDraw 32.3%) | |
| runtime-loaded (THP decoder) | 1.11% | 1.01% | **39.86%** |

- **Cutscene top functions:** PSMTXConcat 4.00%, GXFinishInterruptHandler 2.87%, J3DFifoLoadPosMtxImm 2.81%,
  calcWeightEnvelopeMtx 2.36%, __save_gpr + __restore_gpr 2.50%, GXLoadLightObjImm 1.45%, PSMTXMultVec 1.32%,
  GXSetArray 1.18%, J3DFifoLoadNrmMtxImm / 3x3 1.16% each, GXLoadTexObjPreLoaded 1.16%, DCFlushRange 1.07%.
- **Later stretch:** after ShdwDraw, the SDK's vector library: PSVECDotProduct 4.06%, PSVECCrossProduct 2.77%,
  PSMTXConcat 2.15%, PSVECAdd 1.98%, PSVECScale 1.64%.
- **Movie (module at 0x80CE9D2C):** the IDCT stand-ins 27.9% (NoYPos 18.3, Y8 9.6: a stand-in's samples count to
  its hook block), Huffman decoding 10.3% (__THPHuffDecodeDCTCompY 7.2%). The native IDCT is ~9,700 cycles a call
  (~23k calls/s at 28% of ~800 M cycles): only ~1.6x the guest code.

### The SDK's paired-single maths library as stand-ins called in place (jobs 312-317, hle_ps.c)

- **What:** 14 PSMTX / PSVEC functions (Concat, Copy, MultVec, MultVecSR, Identity, ScaleApply, TransApply, VECAdd,
  Subtract, Scale, DotProduct, CrossProduct, SquareMag, SquareDistance) as C generated from main.dol's own code
  (core/tools/ps_translate.py), recognised at any address by a hash of their bytes. TP calls them ~75k times a
  second in the cutscene and ~230k in the later stretch (PSVECAdd ~40k/s there).
- **TP runs with FPSCR[NI]** (non-IEEE: denormals flush); the first build refused every call for it (job 312 void).
- **PC HLE_CHECK:** 5,006,874 calls (fields 900-2100 and 6000-6300), **0 differed**.
- **As entry hooks they lose (job 313, `--ab ps`):** later stretch **14% slower** (phase pairs, resolved), cutscene
  -3%; job 314 (all on): later stretch 13.22% against 15.1-15.5% without. Each call left compiled code, went through
  the generic stand-in dispatch and came back through the indirect table: more than the 8-51 instructions it saves.
- **Called in place:** the JIT compiles `bl` to one of them as a direct C call inside the caller's block (LR set as bl
  sets it; refused: the block leaves for the function as the bl would), adding the function's instruction count
  statically (as the inline sc handler does); on the 3DS the functions are no longer entry hooks. Job 315: within noise.
  Job 316 (only f1 and f14-f31 written back, the caller's GPR cache kept across the call): later +2.1% (+-6, within
  noise). **Plain run (job 317) against plain runs without them (299, 300, 305, 306, 309, 311): cutscene 27.4-27.9 ->
  28.61%, later stretch 15.1-15.5 -> 16.20%, movie 11.73 (314) -> 11.95%.** JIT fuzzer gate: 0 failures (the known
  seed-11 stfs denormal gap aside).

### FIFO writers added to the in-place stand-ins (job 318) and the PE wait (job 319)

- **What:** J3DFifoLoadPosMtxImm, J3DFifoLoadNrmMtxImm, J3DFifoLoadNrmMtxImm3x3 and GXLoadLightObjImm (2.81 +
  1.16 + 1.16 + 1.45% of the cutscene, job 311): ps_translate.py now takes integer instructions (decoded from the
  words), stores to the gather pipe (collected in order, to gp_fifo_write once; a GP stop request makes the caller
  leave after the call) and a pointer loaded from the small-data area (fetched and checked at entry).
- **PC HLE_CHECK** (each run's pipe bytes captured and compared, then fed once): 5,925,523 calls, **0 differed**;
  the runs' GX byte totals equal the earlier runs' to the byte.
- **2DS (job 318, plain) against job 317:** cutscene 28.61 -> **28.98%**, later stretch 16.20 -> **16.35%**, movie
  11.95 -> 11.86 (noise); core 0's GX wait in the cutscene 5.65 -> 7.25%. ~68k FIFO-writer calls/s in the cutscene.
- **GXFinishInterruptHandler's 2.9% (not adopted):** its first instruction reads PE register 0x0A, and every PE read
  waits for the GX thread to drain (gxAsync->sync). Job 319 (`--ab pesync`, the 0x0A read taking only the posted
  signals): cutscene +3.6 +- 10.8%, later -3.0 +- 5.7% (phase pairs): no effect; reverted. The handler's share is
  something else (it also switches OS contexts twice).

### More SDK functions in place: branches, computed addresses, GPR helpers (jobs 320-327): not kept

- **Translator v3 (ps_translate.py):** forward branches (CR fields, XER[CA], LR as C locals), the function's own stack
  frame as a local array, `__save_gpr` / `__restore_gpr` entered at each `_savegpr_N`, and a logged mode for computed
  addresses (stores wait in a log, applied at the end; out of range refuses with nothing applied). Compiled code takes
  the instruction count the stand-in reports off RDC (branches make it vary). HLE_CHECK now records every stand-in store
  and compares it with the interpreter (not only near the arguments): 452,461 calls, 0 differed. psq_st stores a
  single-denormal value as signed zero, as the interpreter does (the earlier stand-ins stored the bits).
- **Added (cutscene share, job 311):** the GPR helpers (2.5%), GXSetArray, GXSetTevOrder, __GXSetMatrixIndex, the
  GXSetTev* family, GXSetChanCtrl, GXLoadPosMtxImm, GXSetTevIndirect, the J3DGD light and texture-matrix writers.
- **Job 320 slower than 318** (28.98 -> 28.03%); the recognition table held 32 functions, so most of the new ones were
  not even used. With room for them (jobs 321-325, `--psoff` per group): **cutscene 21.8%**, all of it in
  J3DLightObj::load (38.5% of samples, job 325, `--profile on`): the J3DGD writers in logged mode reload the display
  list's write pointer for every byte, and each load scanned a log growing by two entries a byte (quadratic).
- **Without them (job 326, J3DGD writers dropped, the log's newest exact store returned first):** cutscene 28.28%,
  later 15.76%: the same as without the GX setters (job 322: 28.29 / 15.70) and below the 18 functions alone (job 318:
  28.98 / 16.35). The GX setters are neutral, the GPR helpers a small loss (the call flushes every cached GPR, and the
  restore drops them all). Back to the 18 functions; the translator keeps the new forms (C20, C21).
- Refusals (job 326, per 10 s): GXLoadLightObjImm ~17k, GXLoadPosMtxImm ~8k, PSMTXMultVec ~660: FP functions called
  with the FPU off (the OS's lazy FPU switch), where the guest's exception must run: correct, and rare enough.
- **Defect found 2026-10-17 (PC HLE_CHECK from boot, title screen):** `frsp f1,f1` was translated as `f1 = PS(f1.p0)`,
  reading the C variable before anything wrote it (the destination counted as written before the source was looked
  at). PSMTXScaleApply could store 0.0 for 1.0 (calls ~1250-1750 from boot, scale 1 on an identity). The 5M-call
  checks over fields 900-2100 passed by luck (the register happened to hold the argument). Fixed in the translator
  (frsp decides its source first); regenerated; the PC check over fields 900-2100 then: PS library 4,896,101 calls,
  ShdwDraw 5,814, J3DGD 1,096,284, **0 differed**.

### ShdwDraw redesigned again: commit at the end, header hoisted, its own function (jobs 328-332)

- **Commit instead of snapshot (job 328):** a fast cell computes into temporaries and commits at its end (no frame
  save / restore); the grid header read once a call; the frame's step words in locals; a slice's budget exit at the
  loop head leaves only dead state (r0, r3-r8, CR0, XER[CA], masked by the PC check at that PC). Cutscene 28.57 ->
  **28.59%**, later stretch 16.12 -> **16.47%** (job 327 -> 328), movie flat; the stand-in 3,060 -> 2,920 ms per 10 s
  (11.0 -> 10.2 us a call, ~285k calls / 10 s, 93% of ~3.8M cells fast).
- **Fast cells in a function of their own (job 329):** inside scan the compiler spilled around every cell (~450 ARM
  instructions of the fast block, 161 stack accesses); shdw_fast holds only what carries between cells. **No gain:**
  later stretch 16.30%, ~3,030 ms per 10 s. Fewer instructions per cell did not make cells cheaper (C19's descent
  cache did not either).
- **PMU split (job 330, `--pmu-split hle`, later stretch):** core 0 runs 362M ARM instructions/s, 166M cycles/s of data
  stalls, 139M of fetch stalls, 1.33M mispredicts/s. The stand-in: cycles 30%, instructions 39-46%, data stalls 30-34%,
  mispredicts 73-84%, D-micro-TLB misses 41-47%, store-buffer drains 93-110%, D-cache read misses 11%. That reads as
  ~400 ARM instructions a cell, about twice what shdw_fast runs; the bracket's counter reads are system calls inside
  it, so jobs 331-332 measure the fast cells alone and an empty bracket.
- **Per run, absolute (jobs 331 `--pmu-split fast`, 332 `--pmu-split empty`, later stretch):** an empty bracket costs
  ~1,140 cycles and ~386 instructions (the counter reads); a run of fast cells ~8,180 cycles, ~5,240 instructions,
  ~1,800 data-stall cycles, ~39 mispredicts, ~1,110 loads. Less the bracket, with ~13.3 cells a run (3.47M fast cells,
  ~260k runs / 10 s): **~365 ARM instructions, ~530 cycles, ~74 loads, ~113 data-stall cycles, ~2.8 mispredicts a
  cell.** Instruction-bound, and the loads were spills: GCC kept ~30 values live through the cell.
- **Only the walk's position, count and leaf scan live (job 334):** the carried guest values stay in the run's struct
  and are touched only at a cell's commit, the instruction count is a formula, the row's end is checked before the
  triangle scan, the walk's diagnostic counters are PC-only. Stack accesses in the function 161 -> 64. The stand-in
  **3,030 -> 2,680 ms per 10 s**; later stretch 16.30 -> **16.55%**, cutscene 28.64 -> 28.61% (job 329 -> 334).

- **Leaf memo (job 337):** within one ShdwDraw call the visited bitset only gains bits, so a leaf whose triangles were
  all found visited stays so; a 256-entry table (leaf address, the call's generation and its triangle count) skips the
  rescan when the walk reaches the leaf again from another row or slice. **Later stretch 16.69 -> 17.47% (+4.7%)**,
  cutscene 28.75%; the stand-in ~324k calls / 10 s (from ~285k) in 2,539 ms (from 2,680): 7.8 us a call (from 9.4).
  PC HLE_CHECK fields 900-2100: 5,814 calls, 0 differed.
- **Descent cache on top (job 339, C19 retried):** later stretch 17.48% (job 337: 17.47%), cutscene 28.79%: flat.
  Removed.
- **Function profile after these (job 336, `--profile on`):** later stretch ShdwDraw 38.2% of samples (the stand-in's
  hook block), psdRealCallBack 3.3%, GetTriPla 2.7%, SetupNP 2.2%, getPolyCode 1.1%, dDlst_shadowPoly_c::set 0.6%:
  the shadow system ~48%. Cutscene flat: GXFinishInterruptHandler 4.3%, loadMtxConcatView_PNGP 2.5%,
  GXLoadLightObjImm 1.6%, __save_gpr + __restore_gpr 2.4%, calcWeightEnvelopeMtx 1.4%, loadNrmMtx 1.4%.
- **Executed hot words by opcode now (qemu JIT_REGSTAT, tp_arm_950 950-962, opt 4723):** 4.53 hot words per guest
  instruction; bc 16.8% (6.3 each), lwz 9.8% (4.2), stw 6.7% (4.2), addi 6.2% (1.7), b 5.5% (6.3), cmpli 4.2% (6.4),
  lfs 3.7% (5.7), rlwinm 3.2%, cmpi 3.1% (6.5), bclr 2.8% (3.9), fmuls 2.4% (10.3), fcmpo 1.8% (15.1). Branches and
  compares are about a third of executed hot code; float loads and stores now go through shared stubs (psq_l 7.9
  words, stfs 3.8). This window still runs icbi loops (3% of instructions, interpreted).

### Translator v4: calls, regions, stops for anything unsupported; the shadow path and J3D (jobs 340-346)

- **What it adds (ps_translate.py direct mode):** `bl` / `bctrl` / `blrl` to any stand-in, called in place through
  ps_icall (state synced: before a call only the GPRs and CR fields written since the last sync, after it only the
  volatile registers); a region (`NAME.dis^END!`: a stretch of a function left at END with everything in guest state:
  ShdwDraw's triangle code, 0x8007fce4-0x8007fea4); CTR loops; scalar single ops, fcmpo / fcmpu, fneg / fabs / fmr,
  fctiwz; lfd values held as doubles; quantized psq_l through the GQR at run time; CR bit ops; ps_cmp*; and any
  instruction it does not handle becomes a stop before it. Direct-mode stand-ins are entry hooks on the 3DS too.
- **Exact (PC HLE_CHECK, 42 stand-ins, fields 900-2100):** 4,715,437 calls, 0 differed; with `HLE_STOP_EVERY=37`
  (900-1300) 1,844,530 calls, 345,474 stopped inside, 0 differed. Defects the checks found on the way: a direct-mode
  function did not write r3 (its result) back; volatile registers a callee leaves were compared after a stop (static
  stand-ins now write them back when another stand-in called them); two-word stores were checked a word at a time.
- **Device: a loss.** Shadow triangle path (job 341): later stretch 17.47 -> 16.34%. Plus 13 J3D functions (342):
  cutscene 28.75 -> 27.83%. Lean sync (343): 27.96 / 16.09. The region was refused ~199k times per 10 s (job 344, a
  per-reason counter): **the FPU was off** (the OS turns it on lazily; most triangles never reach an FP instruction,
  ChkShdwDrawThrough filters them first). With the FP state checked at the first FP instruction instead of at entry
  (job 345) it always runs, and the later stretch falls to **14.41%**: the native triangle path is slower than the
  JIT's code for it.
- **Why:** every native function loads the GPRs it uses from guest state at entry and writes them back at its end,
  and each in-place call adds a lookup and the state checks: tens of ARM instructions a call. The triangle path makes
  ~15 nested calls, several into 6-11 instruction functions (getCode, setCode, ChkShdwDrawThrough), which the JIT runs
  inline in a few words each. kankyo_color_ratio_set, recursiveCalc and dKy_GlobalLight_set stopped on every call
  (a callee with no stand-in). **Translation pays for large self-contained loop functions (calcWeightEnvelopeMtx,
  ShdwDraw's walk), not for call-heavy small ones.**

- **J3D functions alone (job 346, `--psoff` the shadow path and the three that always stopped):** cutscene 28.34%
  against 28.75% (job 337): still a loss. **Back to the 19 stand-ins (job 347): 28.66 / 17.50 / 11.86**, as job 337.

### GXLoadLightObjImm had never run; relaxed stand-in floats (jobs 348-349)

- **A translator bug (job 344's per-reason counter: refusals by the stand-in itself, ~8-17k / 10 s):** GXLoadLightObjImm
  writes paired singles to the gather pipe with psq_st, whose second word lands at 0xCC008004; only 0xCC008000 itself
  was taken as the pipe, so that word became a store to RAM outside MEM1 and every call refused (on the PC too). The
  pipe now takes any address in its 32 bytes. PC HLE_CHECK fields 900-2100: 111,185 calls of it, 5,007,286 in all,
  0 differed. **Job 349: cutscene 28.66 -> 28.84% (+0.6%)**, later 17.47%. (MEASUREMENTS' earlier note that its
  refusals were the lazy FPU was wrong.)
- **Relaxed floats in the stand-ins (job 348, owner-approved):** the paired multiply-adds in single precision (two
  roundings; hle_psops.h HLE_FASTFLOAT): **cutscene 28.66 -> 28.89 (+0.8%), later 17.50 -> 17.71 (+1.2%), movie
  11.86 -> 12.15 (+2.4%)** (the IDCT uses them too). On by default on the 3DS; the PC build stays exact so its
  HLE_CHECK keeps comparing the logic bit for bit. (In the JIT the same idea did nothing, job 338.)

- **The commit (675acb2) on the 2DS (job 350):** cutscene **29.09%**, later stretch **17.87%**, movie **12.42%**;
  the day's start (job 327) was 28.57 / 16.12 / 11.72: +1.8% / +10.9% / +6.0%.

### Gameplay benchmark and pipe stores at known addresses (jobs 356-359, device)

- **The first gameplay state (job 356):** `sdmc:/gc3ds/states/tp_ordon.state`, Link in Ordon after the opening (made
  by steering a run live: relay `cmd JOB "press A 6"`, `"stick X Y N"`, `"save PATH"`). Benchmark: `--state
  sdmc:/gc3ds/states/tp_ordon.state --seconds 300 --press` a square walk (SUP / SRIGHT / SDOWN / SLEFT, 120 fields each
  from field 40). The old tp850 benchmark is the title / attract sequence (see the memory note), not play.
- **Gameplay is ~20% speed, with both cores near their limit:** core 0 98-99%, the GX thread 80-84% busy (job 357).
- **Profile (job 357, `--profile on`):** flat; **the GX SDK 29.6% of core 0 samples** (GXLoadPosMtxImm 2.2%,
  GXLoadTexObjPreLoaded 2.0%, __GXSendFlushPrim 1.5%, GXSetTev* ~6%, ...), game code 23%, J3D 13.4%, OS / DVD / AI / DSP
  3.9%, collision 2.9%, vector / matrix 2.7%; J2DPane::draw 3.3% and font drawing (the HUD). ShdwDraw ~1%.
- **OPT_PIPEK (job 358 off, 359 on, same walk):** a D-form store (stw sth stb stfs) whose base the block set with lis /
  li / addi / ori to a pipe address (the SDK's GX functions: lis rX,0xCC01; st* -0x8000(rX)) appends to the pipe's
  burst through a 2-word site and a shared stub (~8 instructions) instead of the RAM check, cold code and the slow
  stub (~50). **Speed 20.26 -> 21.68% (+7.0%)**; the GX thread 80.8 -> 88.3% busy; draws 7,176 -> 7,618/s. JIT
  fuzzer (new sequences: lis + stores in and just past the pipe's 32 bytes, with the pipe's buffer set): 0 fails at
  masks 70259 and 4723, ~2,200 known-address pipe stores compiled per seed. Default on (`--pipek off` to compare).

- **The GX thread is now the limit in gameplay (job 360, after OPT_PIPEK; superseded by job 367 below: core 0 waits
  for it only ~1.3% of the time, so core 0 is the limit):** busy 88.6%; vertex work (gxvtx) 21.6%,
  of which vfast_prepare 13.5%; state work (gxstate) 20.6% with the split's timers (17.1% without). **State split, ms
  per second:** the previous batch's draw call (flush_batch) **57**, update_tev **28**, the batch key 8.5, apply_frag 5,
  light uniforms 4, split_early 3.4, texture binds 3; textures (gxtex) 5.6%. 88% of ~7,600 draws/s start a new batch
  (~6,700/s), so flush_batch is ~8.5 us a batch, nearly all the citro3d draw call (vwait 1.8%): its state upload
  (combiners, fragment state, vertex-shader uniforms such as the matrix palette) and the draw. gcbench measured a bare
  C3D_DrawArrays at 3.25 us of CPU against 0.74 us for a raw register sequence (HANDOFF item 6). **Next:** what the
  draw call uploads per batch (uniform counts, dirty flags), then raw command-buffer writes for the per-batch state
  that changes, and longer batches; the vertex worker on core 3 is only ~16% busy.

### What a batch's draw call costs, and raw draw calls (jobs 361-369, device, 2026-10-17)

- **The walk benchmark is a static frame after ~60 s.** Since job 359 the square walk takes Link into a villager
  ("Speak A" prompt) and he stays there: every 10 s report from ~60 s on shows exactly 7,618 draws/s (jobs 359, 360,
  364, 367; 7,125 draw calls/s). Job 358 (OPT_PIPEK off) took a different path and kept moving. So the walk is
  gameplay for its first ~60 s and then one repeated, GX-heavy Ordon frame: good for an A/B within a run and for
  comparing runs from 60 s on, but **358 against 359 (OPT_PIPEK's +7.0%) compared different content** (re-checked in
  job 369 below). Score A/B runs per 20 s phase, each phase against the mean of its neighbours, from 60 s
  (core/tools/ab_phases.py; perf_heavy's pooled means put the first phase, a different scene, in the off arm: jobs 364
  and 365 both read +3% that phase pairing turned into +0.2% and -0.6%).
- **What the draw call uploads (job 361, counters `# gx draw call 10s`):** ~6,500-7,100 calls/s, **111 command words**
  per call, **8.96 us in the call** (58.6 of flush_batch's 61 ms/s; the wait for the vertex worker 2.5 ms/s). Per call:
  the effect block 0.95 (the cull mode is set every batch), 3.1 combiner stages, 0.6 texture units, 0.5 combiner
  buffer blocks, 3.6 float uniform vectors in 1.24 runs (palette rows).
- **Uploading less does not make the call cheaper (lean state, jobs 361-363).** Skipping citro3d setters whose result
  equals citro3d's copy (one bit per kind, live `set lean N`): texture binds 0.6 -> 0.17 units per call, combiner
  stages 3.0 -> 0.6, uniforms 3.6 -> 3.1, words 110 -> 95, but the call went only 8.9 -> 8.2-8.5 us. **All six kinds
  on at once hung the GPU** (jobs 361 and 362: the GX thread waits in C3D_FrameBegin for a submit that never finishes,
  `gx at 6`, 3 fields after the state load in 362); each of texture binds, the effect guard, uniforms and combiner
  stages alone ran 75 s cleanly (job 363); the combiner buffer and cull bits were not tested alone. Off, not pursued
  (REDESIGN_CANDIDATES C24).
- **Raw draw calls (job 364, `--ab rawdraw`):** the draw's own registers as one block of citro3d's words and
  C3Di_UpdateContext only when something is dirty: 8.8 -> 8.25 us a call, **speed +0.045 +- 0.018 points (+0.2%)**
  over 10 phase pairs after 40 s. The first 256 calls and one in 4,096 after are also issued through citro3d and
  compared: 0 of 481 differed.
- **Command buffer prefetch (job 365, `--ab cmdpld`):** prefetching the next 1 KB of the command buffer after each
  call (each frame rewrites it from the start after the submit cleaned it): 8.2 -> 7.85 us a call, **speed -0.134 +-
  0.021 points (-0.6%)**. Rejected (C26): the misses are not the cost.
- **Raw state (jobs 366-367, `--ab rawstate`):** the blocks a batch changes (effect, texture units, combiner stages
  and buffer, vertex-shader float uniforms) written by pica.c in citro3d's order and words, without
  C3Di_UpdateContext (~95% of calls; the rest, e.g. after a vertex-buffer move, go through citro3d). **8.15 -> 5.3 us a
  call, flush_batch 61 -> 41 ms/s, speed +0.157 +- 0.022 points (+0.73%)** over 9 phase pairs from 60 s (job 367;
  job 366, with James playing, +1.17 +- 0.71). Compared with citro3d state included: 0 of 220 differed (366);
  planted-difference self-test (job 368, `--rawplant on`): caught at the first compared state, raw draws then off.
  A bare C3D_DrawArrays is 3.25 us on an idle 2DS (gcbench 4) and ~8 us inside TP whatever it uploads, so the cost is
  citro3d's code itself, cold in core 2's 16 KB I-cache between draws, more than its stores. Default on with raw draws.
- **Core 0 is the limit in this frame, not the GX thread (job 367, from 60 s):** core 0's wait for the GX thread was
  **1.27% -> 0.65%** of its time (rawstate off -> on), the GX thread 89.8 -> 87.4% busy. Freeing 20 ms/s of the GX
  thread bought 0.73%: work removed from the GX thread is now capped by that remaining ~0.65% wait. The next speed has
  to come from core 0.
- **OPT_PIPEK re-checked in one run (job 369, new defaults, `--ab pipek`):** **+1.257 +- 0.073 points (+6.6%)** over 8
  phase pairs from 60 s (19.9-20.2% on, 18.7-18.8% off). Jobs 358/359's +7.0% stands. In this run Link did not stop at
  the villager (draws varied 6,300-7,600/s), so the walk's end is not fixed either: compare runs only within a run.
  New defaults on the device: 614 draw calls compared with citro3d, 560 with raw state, 0 differed; 5.3 us a call.

### Core 0 in gameplay after raw state, and psq_st to the pipe (jobs 370-372, device, 2026-10-17)

- **Profile (job 370, new defaults, `--profile on`, hot_funcs.py over 27 reports, 76% of samples listed):** GX SDK
  **23.2%**, J3D 14.0%, J2D / JUT (the HUD and the villager's dialog text) 12.9%, game code and the rest 16.1%,
  __save_gpr / __restore_gpr 2.7%, vector / matrix 2.9%, collision 2.9%, JAudio 1.2%. Flat: the top function is
  J2DPane::draw at 3.5%, then loadMtxConcatView 2.5%, GXLoadPosMtxImm 2.2%, __GXSendFlushPrim 1.5%,
  GXLoadTexObjPreLoaded 1.3%.
- **psq_st to the pipe:** GXLoadPosMtxImm / GXLoadTexMtxImm / GXLoadNrmMtxImm send their matrices with six psq_st to
  lis + addi 0xCC008000, which OPT_PIPEK did not cover: the psq_st stub fails (outside MEM1) and the site's cold path
  calls jit_gather once per element. The `# jit code` line's "pipe stores" counts those calls (JitStats.gatherStores):
  **~127,000 a second** in this frame (job 370).
- **Per-site version (job 371, rejected):** each element converted inline and appended through the pipe stub: C calls
  -40,000/s but **speed -0.458 +- 0.052 points (-2.3%)**: two inline conversions and a cold path per site cost more
  fetch than the calls saved.
- **In the shared stubs (job 372, `--ab pipeps`, OPT_PIPEPS, off):** the psq_st stubs (plain float and known singles)
  append to the pipe's buffer themselves when the address is the pipe's and the burst has room; sites unchanged. C calls
  ~120,000 -> ~73,000/s, **speed +0.065 +- 0.035 points (+0.3%, 1.9 SE)**: a jit_gather call costs ~50 cycles of core 0,
  not the 100+ assumed. Not a clear gain: off. JIT fuzzer (new cases: lis + addi -0x8000, then psq_st with random GQRs,
  one or two elements, some past the pipe's 32 bytes): 0 fails at masks 201331, 70259, 4723, 7, fault mode off, seeds
  1 and 3; planted bugs caught.
- **Relaxed floats in gameplay (job 373, `--ab fastfloat`, OPT_FASTFLOAT):** no rounding to single, no 25-bit frC cut,
  hardware conversions on stores: **+0.100 +- 0.058 points (+0.5%, 1.7 SE)** over 8 phase pairs from 60 s (18.3-18.6%
  in this run's scene). As on the title benchmark (job 338): float rounding is not what limits core 0. This bounds
  C8 (single precision in the JIT) too: making the rounding cheaper cannot gain more than removing it.
- **Inclusive profile (job 374, `--profile on`: each sample walks the guest stack's back chain from r1, plus LR, and
  counts each return address once; core/tools/stack_funcs.py maps them to the calling functions; 22 reports from
  60 s, 50,000 walks a report, 0 addresses lost).** Share of all core-0 samples taken under each function:
  rendering (mDoGph_Painter) **56.5%**, game logic (fpcM_Execute: every actor's execute) 15.8%. Under the renderer:
  **J2DScreen::draw 24.8%** (the 2D UI), of which the HUD meter (dMeter2Draw_c::draw) **16.4%** and the talk / message
  screens (dMsgScrnBase_c::draw) 14.8% (overlapping); J3DDrawBuffer::draw 8.9%, particles (dPa_control_c::draw) 5.3%,
  the minimap (renderingAmap_c::draw) 5.0%. J2DPane::draw reads 47% but recursion through two call sites counts twice:
  use J2DScreen::draw. Core 0 spends ~3.5x as long building the frame's GX commands as running the game, and the 2D
  UI library is the largest self-contained part of it (this frame has the villager's talk prompt open; the HUD meter
  is always on).
- **GX register setters as stand-ins without the store log (job 375, `--ab psgx`):** ps_translate.py now tracks values
  computed from entry registers and bases with such offsets (`x` keys: __GXData + 4 * stage), checks those ranges once
  at entry, and fetches __GXData again after a store when no store range overlaps the word (checked at entry): 13 GX
  setters translate in static mode (GXSetTev* x 10, GXSetChanCtrl, GXSetArray, __GXSetMatrixIndex); the 19 existing
  stand-ins regenerate byte-identical. PC HLE_CHECK, TP fields 900-1500: **4,384,272 calls, 0 differed** (each setter
  34k-293k calls, 0 refused). Device: **-0.143 +- 0.032 points (-0.7%)** over 8 phase pairs: still slower than the
  JIT's code. The stand-in call's fixed cost (state checks, arguments from guest state, range checks, the pipe flush
  call) exceeds what a ~20-instruction function does; with C20 / C21 / C23: per-function native code pays only for
  large functions. Off (the group is `--ab psgx`).
- **The HUD's ceiling, and the two limits are balanced (job 376, `--skipfn 8021151C --ab skipfn`, measurement only):**
  dMeter2Draw_c::draw (the HUD meter, 16.4% of core 0 inclusive, job 374) returning at once: **+1.129 +- 0.041 points
  (+5.6%)** over 8 phase pairs from 60 s (19.84 -> 21.15%). Core 0's wait for the GX thread **0.93% -> 9.94%**, the GX
  thread 79.9 -> 85.4% busy: once core 0 is faster the GX thread is the limit within ~6%. A lever on one side alone
  is capped near that; full speed needs both core 0's and the GX thread's work per frame cut. (The HUD drawn at a
  lower rate or replayed would be worth at most this +5.6%.)
- **Guest frame skip (jobs 377-378, `--gfskip 8000A604 --ab gfskip`; owner: whatever makes the game run better):** a
  hook makes TP's renderer (mDoGph_Painter) return at once on every other call; its logic runs every frame. speed_pct
  is the wrong ruler here: it fell (job 377: 21.1 -> 16.7%) because the game stops idling in the renderer's retrace
  wait (polling-loop skips 2,699 -> 1,796/s). **Game frames per real second (renderer calls, job 378): ~7.4-8.9
  without, ~12.8-13.4 with (+55-65%); frames drawn ~8.2 -> ~6.5/s.** The game runs ~60% faster toward its 30 frames a
  second, at a lower display rate. Visual side effects (effects that copy the previous frame) not yet checked. Off
  by default (needs the game's renderer address); `# guest frames 10s` reports the rates.
- **Pacing and draw 1 in N (jobs 380-381, `# new frames 10s`: gaps between EFB -> XFB copies):** in Ordon's steady
  play the gaps are as even with skipping as without (sd 9-21 ms against 12-24 ms): fewer pictures, no added stutter.
  1 in 2: game frames ~12-13.6/s against ~7.3-8.9, pictures ~6-6.8/s; **1 in 3 (`--gfskip-n 3`): game frames
  ~14.4-17.4/s, pictures ~4.9-5.8/s**.
- **Transitions (jobs 379, 382-383, James playing):** skipping broke TP's area change (faded to black, never back):
  the scene change counts game frames while TP's JUTFader (mDoGph_gInf_c::mFader) advances once per rendered frame. A
  guard on the mFade byte (job 382) never triggered (TP's transitions use the JUTFader); `--gfskip-ok
  80450BC8+4=1,80450BC8+20=ffffffff` (skip only while the fader's mStatus is 1, faded in and idle, and mEStatus -1)
  fixed it (job 383: the transition drew every frame, 7.8 guarded calls/s, and faded correctly). Past the transition
  (a heavier area): ~6 game frames/s with skipping, speed_pct ~9%; state saved there as tp_area2.state (job 384).

### The heavy Ordon area is GPU-bound by texture fetch; mipmaps (jobs 385-388, device, tp_area2.state)

- **Frame skip there (job 385):** game frames ~4.0/s without, ~5.8-6.0/s with (+48%); pictures 4.0 -> 3.0/s.
- **Profile (job 386):** speed 11.5%, core 0 waits for the GX thread **39%**, the GX thread 93.8% busy of which
  **gxsync 34.8%: waiting for the GPU. The PICA took 99.5 ms a frame** (c3d_gpu_ms) against 8.4 in Ordon's start (job
  370), with fewer vertices (335k/s against 447k) and similar draw calls (7.8k/s against 8.7k): the GPU is the limit.
- **Texture diagnostic (job 387, `--texdiv 4 --ab texdiv`, measurement only):** textures at 1/4 size: GPU 100.7 -> 28.1
  ms, speed 11.5 -> 15.9% (+38%): texture fetch (C17's minified-RGBA8 case: 17 Mpixel/s against 204 in the cache).
- **Mipmaps (job 388, `--ab mips`):** textures whose GX filter samples mips get the levels the game stores after level 0
  (TX_SETMODE0 mip mode, TX_SETMODE1 max LOD; power-of-two sizes from 16; down to 8 x 8): **GPU 100.6 -> 55.4 ms a
  frame, speed 11.50 -> 13.73% (+2.207 +- 0.019 points over 7 phase pairs, +19.2%)**, core 0's GX wait 40 -> 30%.
  Screenshots of both arms match (distant ground slightly smoother). Default on.
- **Mipmaps in the other scenes:** Ordon walk (job 390): **+1.138 +- 0.023 points (+5.3%)**, GPU 10.9 -> 8.3 ms a
  frame; title / cutscene (job 389, ab_split): cutscene 25.0 -> 27.3% (phase pairs +10.9% +-21, within that
  benchmark's noise), later 13.6 -> 13.8%, GPU 17.2 -> 10.8 ms. No scene got slower.
- **Smaller texel formats (C17; jobs 391-392, tp_area2, mips on):** level 1 (`--smalltex 1`: the smallest PICA format
  that holds level 0 exactly): only grey textures converted (LA4 170, LA8 62, L8 10 of 1,049; CMPR's blended colours are
  never exactly 5-6-5): +0.062 +- 0.028 points, neutral. **Level 2 (`--smalltex 2`: CMPR / RGB5A3 sources, and RGB5A3
  palettes, rounded to RGB565, RGBA5551 or RGBA4; RGBA8 sources stay): RGB565 187, RGBA5551 48, RGBA4 46 more; GPU
  55.2 -> 43.5 ms a frame, +0.849 +- 0.020 points (+6.2%)** over 6 phase pairs. Screenshots unchanged at the 3DS's
  resolution. Default level 2.
- **What is left (job 393, new defaults):** textures at 1/4 size would still take the GPU 43.4 -> 28.0 ms (14.6 -> 15.9%);
  with tiny textures core 0 still waits ~21% on the GX thread (vertex work ~23%, state ~21% of it). RGB8 for opaque
  RGBA8 sources (lossless, 24 bits) added: none in this scene (job 394, same +0.89 points as job 392).
- **TEV key skipped while no TEV register changed (job 395, `--ab tevver`):** a counter bumped by bp_write for the
  registers tev_pica_key reads (outside GXCore: save states keep their layout): update_tev 41 -> 34 ms/s (TEV registers
  really change on most batches), **+0.074 +- 0.027 points (+0.5%)**. Exact; default on.

### Three scenes after the texture work, and what limits each (jobs 396-401, device, 2026-10-18)

- **Baselines (all of today's defaults):** Ordon walk (job 397) **22.64%** (21.45% in job 360); heavy area
  (tp_area2, job 398) **14.57%** (11.47% in job 386); title cutscene 25-303 s (job 396, `--profile on`) 26.3%.
- **The title's later stretch is the THP movie, not a regression.** Its first ~60 s shift with the cutscene's speed (a
  faster cutscene reaches the movie earlier), so 303-420 s means mislead; over the movie's steady 370-420 s jobs 350,
  355 and 389 read 12.3-12.8% and job 396 11.8% (profiling on). The movie: 34 draws/s, **10.6 texture decodes/s of ~512
  KB (texdec_kb 5,447/s), the GX thread 33% in texture decode**, core 0 waiting on it 13-17% (the idle hook drains the
  GX thread). smalltex level 2 costs the movie 13.18 -> 12.52% (job 400: each frame's textures scanned and converted
  again); level 2 stays (gameplay decodes ~0 textures/s: cached).
- **What limits each scene:** Ordon walk: core 0 (97% busy, waits 0.9% on the GX thread; the GX thread 85% busy: vertex
  work 32.5% of which gather 9.4% and CPU lighting 7.1%, state 16%). Heavy area: the GPU (43.6 ms a frame, gxsync 19%,
  core 0 waits 25%). Movie: texture uploads.
- **Texture size in Ordon (job 399, `--texdiv 4 --ab texdiv`):** textures at 1/4 size gain only **+0.364 +- 0.030
  points (+1.6%)**: Ordon's walk has little texture headroom (GPU 4.5 ms a frame).
- **Texture uploads without a divide per texel (job 408 against 402, title):** upload_level / upload_level_fmt resampled
  with `x * w / W` per texel (for --texdiv), a library call on the ARM11 (no divide instruction); now a column table
  built per level by stepping (col_map: exact against the division for every w 1-1024 and power-of-two W 8-1024,
  2,088,960 columns) and the format switch outside the texel loop. **Movie 12.05 -> 12.76% (+5.9%)**, texture decode
  33 -> 24% of the GX thread at 11.5 decodes/s; cutscene 26.63 -> 26.64% (unchanged). Default (no switch).
- **Where the heavy area's GPU time goes (jobs 404-406, tp_area2, `--gpudiag BITS --ab gpudiag`, measurement only):**
  every game draw scissored to 16 x 16 (bit 8): **GPU 42.7 -> 20.9 ms a frame**, speed 14.77 -> 16.37%: about half is
  fragment work and **~21 ms is not** (vertex shading with the lighting shader, EFB copies and clears, transfers).
  Blending off (bit 2): 42.9 -> 43.0 ms; depth test off (bit 4): 42.7 -> 43.1 ms: neither costs anything. Bit 1 (EFB
  -> texture copies not drawn) **froze the 2DS ~7 s after it took effect (job 403**; the log stopped mid-line, no
  watchdog report; cause not found); bit 16 (XFB copies not made) was not run. EFB -> texture copies there: ~2.6-2.9
  million target texels/s (~1.7-1.8 M source pixels), ~45 copies/s.
- **CPU-lit draws' gather and lighting on the vertex worker (`--deflit`, off until measured):** vfast_prepare gathers and
  lights CPU-lit draws on the GX thread (gather 9.4% and lighting 7.1% of it in Ordon, job 397) while the worker (core
  3) is 19% busy. With g_vfDeferCpu they are deferred like shader-lit draws: the lighting set-up (only the lights each
  half uses) is copied into the job after its matrices and vfast_finish lights in runs of 32 vertices from the job's
  own matrix copies. PC (VFAST_CHECK + VFAST_DEFLIT_CHECK, TP 1,500 frames): **4,682,498 CPU-lit draws done both ways,
  0 differed** (4.1 M lit halves); self-tests caught: lit draws' colour flipped 4,101,790 differ, lights left out
  3,881,137 differ (leaving out only each half's last light changed nothing: TP's last enabled light is black).
  **Device: heavy area (job 410) +1.008 +- 0.025 points (+6.8%, 14.73 -> 15.74%)**, core 0's GX wait 26.5 -> 20.6%,
  the GX thread's vertex work 23.7 -> 17.1%, the worker 13.6 -> 18.8%; **Ordon walk (job 409) +0.165 +- 0.029 points
  (+0.7%)**, the GX thread 86.6 -> 79.4% busy (vertex work 34.4 -> 17.6%), worker 19 -> 25% (core 0 limits Ordon).
  Default on.
- **The lighting vertex shader is most of the heavy area's GPU time (jobs 411-416, `--ab shlight`):** lighting on the
  CPU instead of in the shader: **GPU 43.6 -> 17.1 ms a frame** (job 411, deflit off: the lighting then lands on the GX
  thread, vertex work 23 -> 36%, speed +0.192 +- 0.036 points). With deflit (lighting on the worker, job 412): **+0.452
  +- 0.036 points (+2.9%, 15.75 -> 16.20%)**, GPU 43.6 -> 27.1 ms, the worker 19 -> 28%, the GX thread again the limit
  (93%; 8% waiting on the worker, whose 128 KB job ring is full 178 times a second with ~19 jobs in flight). Ordon
  (job 415): **+0.081 +- 0.015 points (+0.35%)**; title (job 416): cutscene and movie within noise. CPU lighting is
  vfast's lighting, the reference's order (the shader's 24-bit floats are not). Default off (`--shlight on` restores
  the shader). EFB -> texture copy and present quads scissored to 16 x 16 (job 413, gpudiag 32): GPU 43.8 -> 43.9 ms,
  copies cost the GPU nothing.
- **Mipmaps and small formats, pictures (job 414 against 398, tp_area2):** with both off and with both on the frames
  match at the 3DS's resolution (the distant ground slightly smoother with mips); the minimap's blocky shapes are
  older (job 386 has them).
- **The job ring with CPU lighting (job 417, `--shlight off --ab vwmem`, tp_area2):** 256 KB 16.16%, 1 MB 15.86%,
  against 16.20% at the default 128 KB (job 412): the ring's size is not the lever (the GX thread still waits on the
  worker 3-6% with 1 MB). 128 KB stays.
- **Vertex set-ups kept for the last 8 keys (job 421, `--ab vscache`, tp_area2):** vfast_prepare kept one set-up per
  lighting mode and recomputed it for 74% of draws (45,744 of 62,300 a second, 42,350 for a different vertex format:
  draws alternate between a few). With 8 kept: **4,746 a second, vertex prep 8.0 -> 6.7% of the GX thread, +0.179 +-
  0.039 points (+1.1%)**. Exact (the same key decides; PC VFAST_CHECK summary identical over 4,682,498 draws). Default.
- **Three scenes on today's defaults (jobs 418-420):** heavy area **15.96%** (`--profile on`; 14.57% this morning),
  Ordon walk **23.43%** (22.64%), title cutscene 26.67% (26.63%), movie 12.70% (12.05%). In game frames a second
  (TP runs at 30): Ordon ~7, the heavy area ~5. With CPU lighting on the worker, Ordon's GX thread waits for ring
  space 10.3% (1.4% before): the worker's gather and lighting are 85% of its time and it falls behind in bursts.
- **Black lights left out of deferred CPU lighting (jobs 422-423, `--ab blacklit`):** TP lights 54% of its light
  evaluations with black lights (PC, 1,500 title frames: 15.6 M of 28.8 M, all attenuated); a black light adds 0 x da,
  exactly nothing while da is finite, which holds with k0 > 0, k1 >= 0, k2 >= 0 (true for all of them). PC: all
  15,560,545 left out, 4,682,498 draws compared, 0 differed. Device: the worker 36.4 -> 31.1% (Ordon) and 28.6 -> 24.4%
  (heavy area), ring waits 10.3 -> 6.3% and 7.8 -> 4.6%, but speed **-0.048 +- 0.020 (Ordon) and -0.044 +- 0.048
  points (heavy area): neutral**; the GX thread stays the limit (its freed wait turns into GPU wait). Kept on (exact,
  less work, worker headroom for the next offload).
- **Textures in VRAM (jobs 424-425, `--ab vramtex`; the XFBs moved to FCRAM in both arms, freeing 1.5 MB of VRAM):**
  each new texture goes to VRAM while more than 512 KB stays free (for EFB copies): tiled into a linear staging buffer
  (freed at the next frame) and DMA-copied by GX_TextureCopy in the GX queue ahead of the draws that sample it (the CPU
  cannot write VRAM). Switching re-decodes every texture. **Heavy area (job 424): GPU 33.8 -> 14.4 ms a frame, +1.952
  +- 0.040 points (+12%, 16.2 -> 18.0%)**, core 0's GX wait 17.5 -> 10.7%; 346 textures in VRAM, 177 left in FCRAM
  for want of room; frames of both arms match. **Ordon (job 425): +0.088 +- 0.029 points (+0.4%)**. The XFBs in FCRAM
  cost nothing measurable (the FCRAM arm read 16.2%, as job 421). Default on. Which textures get VRAM is first come.
- **The GX thread's time by position (jobs 427-432, tp_area2, `--profile on`):** the profiler thread now also samples
  the GX thread's position byte (g_gxWhere, ~10,000 samples a second; markers through the decoder and the backend,
  reset to the decoder when a backend entry returns): decoder outside any backend call ~34% at first, of which the
  display-list statistics' hashes (dl_note) **3.9%**, BP writes 2.6%, list run scans 1.5%, the FIFO copy 1.5%, XF 1.3%;
  vertex prep 7.2%, the frame-begin GPU wait 6.1%, draw calls 5.3%, batch key 4.5%, ring-space waits 4.3%, texture
  lookups 3.6%, TEV 3.3%, EFB copies 2.8%. The decoder's command-class profile (`# gx decode 10s`, job 427): draws
  356 ms/s, BP writes 238 (233,000 a second; mostly the open merged draw issued by merge_break), XF 91, CP 54, list
  runs to the backend 99. A direct-mapped texture index (job 431) found 90% of lookups past the last hit but there are
  only ~800 a second: texture lookup time unchanged, reverted.
- **The decoder's statistics only when profiling (job 433, `--ab gxdiag`):** dl_note's hashes and the command-class
  profile (two kernel clock reads per 16 commands) off unless `--profile on`: **+0.568 +- 0.034 points (+3.2%, 17.7 ->
  18.2%)**, core 0's GX wait 13.1 -> 9.2%.
- **The host profile's own cost (jobs 434-437, `--ab profclock`: g_profClock on and off; the off phases' profile
  columns read 0):** every prof_begin / prof_end pair read the clock (svcGetSystemTick, a system call of ~200 cycles):
  **heavy area 18.3 -> 19.7% with it off (+1.458 +- 0.057 points, 8%)**, Ordon +0.255 +- 0.014 (+1.1%, core 0's per-slice
  timers). Now each pair is timed one time in 16 (a pseudo-random draw per translation unit, scaled by 16), and the
  per-draw timers and the worker's per-job timer one draw / job in 64 (scaled by 64): the clock's remaining cost in the
  heavy area **+0.434 +- 0.071 points (16 / 16) then +0.326 +- 0.053 (16 / 64), 1.7%** (job 437: 19.4% on, 19.7% off).
  The profile columns stay (sampled estimates). Earlier A/B verdicts stand (both arms carried the cost).
- **Early split size (jobs 441-442, `--splitverts N --ab splitverts`, tp_area2):** 24,576 arena vertices between splits
  **-0.462 +- 0.053 points** (GX thread's GPU wait 10.1 -> 14.3%), 4,096 **-0.824 +- 0.046 points** (more splits, 105 ->
  123 a second, and a longer GPU wait): 8,192 stays.
- **Luma3DS's physical-memory mirror is live and user-writable (job 443, `--probe-mirror on`):** Luma's k11_extension
  (main.c, relocateAndSetupMMU) maps PA 0x00000000-0x30000000 at VA | 0x80000000 as strongly-ordered memory, "rwx for
  all". A linear buffer (VA 30a09d00, PA 20a09d00) read through VA a0a09d00 matched, a store through it reached the
  buffer, no fault. The New 3DS L2C-310 (PA 0x17E10000, 3dbrew) reads at VA 0x97E10000: ID 410000c9 (r3p3), control
  00000001 (enabled), aux 32890000, tag / data latency 111 / 221, **prefetch control 74000001: instruction and data
  prefetch and double linefill already on, prefetch offset 1**, power 00000000. This corrects two records: the L2C-310's
  registers are reachable (PERF_LEVERS.md called its prefetch tuning "Secure-only, no reachable SVC"), and user mode
  can reach VA 0x80000000-0xAFFFFFFF (jit_3ds.c's fault-mode comment says nothing at 0x40000000 and above is user
  accessible). Guest RAM through the mirror would be uncached: not a lever.
- **L2C-310 prefetch offset (jobs 444-445, Ordon walk, `--l2pf N --ab l2pf`, the kernel's value restored after each
  run):** offset 7 against the kernel's 1: **+0.008 +- 0.015 points (neutral)**, instruction-fetch stalls 21.5 -> 20.5 M/s;
  offset 15: **-0.193 +- 0.015 points**. The kernel's setting stands; L2 prefetch tuning is not a lever for core 0.
- **Frames ended early, and the command buffer (jobs 446-450, tp_area2 and Ordon):** the heavy area submitted 10.9-11.9
  frames a second against 5.8-5.9 pictures; each early end makes the next C3D_FrameBegin wait for the GPU to finish
  everything (the GX thread's 'framebegin' ~7%). A 128-entry GX queue (`--qbig`, citro3d's 32-entry array swapped while
  the queue is empty; citro3d never reads maxEntries after C3D_Init): +0.055 +- 0.046 (heavy area), +0.031 +- 0.012
  (Ordon), submits unchanged: not the cause; off. A count of causes (`# frames ended early 10s`, job 448): **all ~6 a
  second from the 1 MB command buffer passing 80%** (~2,000 draw calls of ~100 words a frame). **2 MB (job 449, `--ab
  cmdbuf`: the limit at 0.4 and 0.8 of 2 MB): +0.506 +- 0.039 points (+2.6%, 19.6 -> 20.0%), submits 11.9 -> 5.9 a
  second (one a picture), the GX thread's GPU wait 11.0 -> 0.1%, its busy time 95.7 -> 87.7%**: core 0 is now the
  heavy area's limit too. Linear memory free stays 12.1 MB. Ordon (job 450): +0.033 +- 0.017 (it never filled 1 MB).
  Default 2 MB at 0.8.
- **Core 0 in the heavy area now (job 451, `--profile on`):** outside compiled code only ~6% (dispatch 4.1%); inclusive
  (stack_funcs.py): rendering (mDoGph_Painter) 39%, actors' draw 28.6%, J2DPane::draw 22%, the HUD meter 12.5%, game
  logic (fpcM_Execute) 19%, mDoExt_modelDiff 10.7%, J3DJoint::recursiveCalc 9.9%. Self time is flat: the top function
  is __GXSendFlushPrim (the GX SDK's dummy primitive of zero words after draws) at 3.4%, then
  loadMtxConcatView_PNGP 2.4%, J2DPane::draw 1.8%, __save_gpr / __restore_gpr 2.5%.
- **__GXSendFlushPrim as a stand-in (jobs 452-453, `--ab psflush`; translated by ps_translate.py in direct mode from
  main.dol's bytes, the existing list regenerated byte-identical first):** PC HLE_CHECK 9,847 calls, 0 differed. Device:
  heavy area +0.420 +- 0.200 points over 5 phases with equal arm means (18.6 / 18.5%), Ordon **-0.017 +- 0.054
  (neutral)**: its cost is the pipe bytes (the same either way), not the stores. Off (`--psflush on`). Each switch of a
  stand-in flushes all compiled code: these A/Bs run ~1.5 points below plain runs in both arms.
- **Function memoization probe (jobs 454-456, Ordon walk, `--memo ADDR`; memo.c, measurement only):** each call of the
  hooked function runs in the interpreter with every data access recorded (inputs: words read before written, with
  their values; outputs: words written above the stack pointer; gather-pipe bytes), compared with the previous call's
  for the same r3. **J2DScreen::draw (802f8ed4): ~200 calls / 10 s, ~110,000-120,000 instructions, ~3,600-3,900 input
  words, ~1,500 output words, ~15-16 KB of pipe bytes a call; dMeter2Draw_c::draw (the HUD, 8021151c): ~38 calls / 10 s,
  ~640,000-710,000 instructions, ~18,000 input words, ~7,900 output words, ~88 KB of pipe bytes a call.** No aborts
  (no hardware reads, exceptions or overflow). **Hits 0**: of 1,474 misses, all had a floating-point / quantizer
  register in the key differ (garbage in unused argument registers: the key should hold only registers read), and
  1,460 had **~11 of the ~3,900 input words changed**: animated floats (126.0, 124.0, 153.0), colours, and words of GX
  state left by earlier drawing. Verifying the inputs would cost ~30x less than running the call, but a screen's
  inputs change every frame (some pane animates), so replay at the screen or HUD level never applies; per pane it would
  need nested recordings split at child calls, coupled to the GX state earlier draws leave. Not pursued.

### The first library port, J2DMaterial::setGX, and pipe bursts with one copy (jobs 457-480, device, 2026-10-18)

- **The port (core/src/cpu/port_j2d.c, `--port off|on|check|plant`):** J2DMaterial::setGX with its colour, texgen,
  TevBlock2, indirect and PE blocks, JUTTexture / JUTPalette loads and the 42 GX SDK functions they reach, as one native
  call: __GXData's words loaded at first touch, pipe bytes in one buffer, texture-object stores logged, everything
  committed at the end; anything else (another TEV block type, indirect stages, a non-default region callback) is
  refused before the commit and the guest code runs. Code hashes of all 47 guest functions and tables gate it.
- **Exactness, on the device (`--port check`: each call run by the port without committing, then by the interpreter
  for real, its stores and pipe bytes recorded and compared, registers too):** job 459 heavy area ~45,000 calls, job
  463 heavy area (palettes ported) ~47,000, job 464 Ordon walk ~105,000, job 474 (-Os build) **0 differed**, 0 refused
  after the palette path. **The checker is not blind: `--port plant` (one pipe bit flipped in the port, job 460)
  flagged every call, at the flipped byte (the last of 419).** The title screen (job 465) uses J2DTevBlock16
  (802f132c): refused, guest code. Interpreter: **1,920 guest instructions a call** (job 470); the port's timing
  estimate was 1,510.
- **Speed (`--ab port`, phase-paired, each switch drops all compiled code):** heavy area **+0.42 +- 0.03** (job 461,
  6 phases from 20 s), **+0.32 +- 0.09 (+1.7%)** (job 467), **+0.30 +- 0.08 (+1.6%)** (job 475); Ordon **-0.05 +- 0.05**
  (job 462), **+0.10 +- 0.04 (+0.5%)** (job 468), **+0.11 +- 0.05 (+0.5%)** (job 476). In Ordon core 0's guest-CPU time
  falls 2.7-4.8% with the port on (cpu_pct) for a smaller speed gain. On by default.
- **Why so little: the port costs nearly what the guest code does.** Timed one call in 16 (job 471): **~40 us building,
  ~20 us committing** (~1,300 calls a second in Ordon: ~8% of core 0, the hook's block 8.1-8.4% of samples in job 466).
  PMU split around each half (jobs 479-480, less the bracket's ~1,140 cycles): **building ~35,600 cycles, ~16,300 ARM
  instructions, ~10,100 data-stall and ~4,200 instruction-stall cycles; committing ~17,500 cycles, ~10,500
  instructions.** The work matches the JIT's translation of the same 1,920 guest instructions (per-byte pipe writes, a
  checked read per field, the SDK's bookkeeping kept word for word), so a port that copies the SDK step by step is not
  structurally cheaper. -Os (code 24.5 -> 10.6 KB) did not help (job 473: 46 / 20 us): not instruction-cache bound.
  Parts (job 472, `--port-parts on`, each with ~1.2 us of clock read): set-up 1.3, colour 4.2, texgens 5.6, TEV
  textures 7.2, TEV stages 20.1, indirect + PE 3.7 us.
- **The commit's cost was the pipe path every game uses:** `gp_burst` mirrored each 32-byte burst into the FIFO in RAM
  byte by byte with a wrap test per byte, and `gp_fifo_write` moved bytes one at a time. **One copy per burst when it
  neither leaves MEM1 nor wraps before its last byte, and whole runs in gp_fifo_write (`--gpbulk`, hw/gx.c): PC state
  hash unchanged over 400 fields and 62.7 MB of pipe bytes; heavy area +0.64 +- 0.03 points (+3.2%), Ordon +1.07 +-
  0.03 (+4.5%), 9 phases each (jobs 477-478), the GX thread 6-10% busier.** On by default.
- **The material port doing less (jobs 481-488):** each structure's fields read with plain loads after one range check,
  pipe bytes stored through a pointer (the call's counts bounded before it starts), bpSentNot and the dirty bits kept
  aside and applied once. **PMU (job 482): ~15,700 cycles and ~4,500 ARM instructions a call, from ~35,600 and ~16,300;
  instruction-fetch stalls ~7,200 cycles are now the largest part.** -Os on top: 18,400 cycles (job 485), so -O2 stays.
  Exact (job 481). Ordon **+0.81 +- 0.05 (+3.6%)** (job 484); with the emulated-time charge scaled to the interpreter's
  count (x1.27) **+1.33 +- 0.43 (+5.9%)** (job 487). Heavy area **-0.06 +- 0.07, +0.00 +- 0.05** (jobs 483, 488): since
  gpbulk that scene waits on the GX thread (core 0's ring waits +1.4-1.7 points with the port on, the GX thread 85-91%
  busy), so core 0's saving does not show there.
- **J2DPictureEx::drawSelf ported (jobs 490-493):** the material, GXClearVtxDesc / GXSetVtxDesc / GXSetVtxAttrFmt,
  PSMTXConcat (multiply-adds rounded once, as the interpreter), GXLoadPosMtxImm, GXBegin with the SDK's dirty-state
  flush (__GXSetSUTexRegs, __GXSetGenMode, __GXSetVCD, __GXSetVAT, __GXCalculateVLim) and dummy primitive, the quad.
  **Exact over 84,402 calls on the device**, ~2% refused (float edge cases); 2,739 guest instructions a call. **Ordon
  +1.16 +- 0.42 (+4.5%) on top of the material port; heavy area -0.10 +- 0.07.** On by default. Ordon's off arm read
  25.8% speed (21.9% at the day's start).
- **J3DShapeMtxConcatView::loadMtxConcatView_PNGP ported (jobs 494-495, PC checks):** concat, position matrix, normal
  matrix (as is, or J3DPSCalcInverseTranspose: fres taken as the correctly rounded single reciprocal, as the interpreter
  does, then two Newton steps), for either class's loadNrmMtx. The check caught two guest effects a port must copy:
  **PSMTXConcat leaves ps1 of f14, f15 and f31 changed (it saves them with stfd and restores with lfd, which loads ps0
  only)**, and **with MSR.FP off the guest's first float instruction takes the FP-unavailable exception (the OS's lazy
  FP switch)**, so a float port is not taken then. Exact: PC 223,652 + 146,753 calls over the title sequence; device
  ~72,000 (Ordon and the heavy area). Only ~209 guest instructions a call, ~3,500 calls a second in Ordon.
  **Speed (`--ab j3dhook`, jobs 496-498): Ordon +1.46 +- 0.33 (+5.8%), heavy area -0.13 +- 0.07 (-0.7%); the title
  per section (job 498 re-scored 10-19, its cutscene ending at ~175 s): cutscene +0.30 +- 0.58, later stretch -0.07 +-
  0.29, neutral.** (A first reading over 40-300 s, +7.3%, put the cutscene into the arms unevenly: wrong.) On by default. (One call stands in for three of the existing stand-ins' calls and the
  glue between them.)
- **The heavy area is the GX thread's now (job 499, `--profile on`):** core 0's "dispatch" bucket rose to 10.2% (4.1% in
  job 451) and Ordon's to 7.9% (job 489): it holds the idle hook's wait for the GX thread to drain (gekko_idle_hook ->
  sync runs from the dispatcher loop, outside compiled code), i.e. core 0 idling for the GX thread. Every port and
  gpbulk measured -0.5 to -0.7% there, each within ~2 sigma: core 0's saving turns into waiting. The GX thread's
  position (job 499): fifo 8%, vprep 8.2%, drawtail 7%, drawcall 6.7%, vwalloc 5.7%, batchkey 5.3%, tex 4.2%, tev 3.7%,
  copy 3.6%, split 3.2%, idle 7-8% (dec_dlnote and dec_op, ~9% together, are the profile's own decoder statistics:
  `--profile on` turns gxdiag on).
- **Baselines with the ports and gpbulk (jobs 500-502, plain runs):** Ordon walk 60-300 s **28.03%** (22.64% in job 397
  this morning; ~23.5% before the ports), heavy area 30-240 s **20.64%** (~20.0%), THP movie 330-420 s 12.84%. The GX
  thread is 94.9% busy in the heavy area and 89.3% in Ordon: it is the next limit in both (Ordon's dispatch bucket,
  core 0 idling for the GX thread, is 11.5% in job 503).
- **The title's sections moved (re-read 10-19):** the cutscene now runs ~34-38% and **ends at ~150 s** (~175 s in an
  A/B run that drops compiled code at each switch); the later Link / Epona stretch runs ~19% from there to ~300 s;
  the movie follows. **Job 502 by section: cutscene 25-150 s 36.05%, later stretch 165-300 s 19.02%, movie 12.84%;
  job 439 (before the ports, 10-18): 35.15%, 18.94%, 12.72%.** So the title gained ~2.6% in the cutscene and nothing in
  the later stretch, where the ShdwDraw stand-in is ~38% of core 0. The "cutscene 25-250 s" means quoted for today
  (26.8%, 28.94%) mixed the cutscene with ~100 s of the later stretch. Windows must follow the boundary (speed_pct
  drops from ~35% to ~19% within one row); ab_split.py and gx_split.py still hard-code older ones.
- **GX-thread ring records batched (`--ringbatch`, gxthread.c, jobs 505-507):** the pipe's 32-byte bursts appended to
  one open record of up to 512 bytes, published when full and before every poll (each event slice), sync, present and
  drain. **Heavy area +0.54 +- 0.02 points (+2.6%), the GX thread 3.7% less busy; Ordon +0.45 +- 0.36 (+1.6%), the GX
  thread 7.4% less busy; title by section (job 507): cutscene -0.09 +- 0.42, later stretch +0.07 +- 0.02, movie -0.05 +-
  0.04 (none GX-bound).** On by default.
- **Where core 0 goes now (jobs 503-504, `--profile on`):** Ordon: the J3D port's block 5.0% and drawSelf's 4.3% of
  samples (their commits wait for ring space when the GX thread is behind), GXLoadPosMtxImm 1.4%, __GXSendFlushPrim
  1.1%, J2DPane::draw 1.2%. **The title's later stretch (job 504's last reports, past its ~150 s cutscene): core 0
  bound (dispatch 1.8%), the ShdwDraw stand-in ~38%** (its block 25.2% plus its neighbours), SetupNP 2.3%, GetTriPla
  2.3%, psdRealCallBack 2.3%, the J3D port 2.9%.
- **The vertex worker's job ring re-tested in gameplay (jobs 508-509, `--vwmem 128 --vwmem-b 256 --ab vwmem`):** the GX
  thread waits for job-ring space 7-10% of its time (valloc) while the worker is 30-34% busy. 256 KB against 128:
  valloc -28% / -34%, the GX thread 6.4% / 5.6% less busy, but **heavy area -0.188 +- 0.017 points (-0.9%), its GPU
  time a frame +22% and core 0's GX wait +1.9 points; Ordon -0.144 +- 0.383 (-0.5%)**. 128 KB stays: the waits are
  where the GX thread's time goes, not what limits the frame.
- **The GX thread's honest position (job 512, heavy area, `--profile on --gxdiag off`):** decode (fifo + dec_*) ~19%,
  state (batch key 5.3, TEV 4.0, textures 4.6, state2 1.8, flush and draw ~4) ~21%, vertex (vprep 8.6, job-ring waits
  7.7, queue, merge) ~18%, draw calls (drawcall 6.5, drawtail 7.4) ~14%, **framebegin 5.0-8.3%** (C3D_FrameBegin
  waiting for the GPU's tail of the previous submit: REDESIGN_CANDIDATES C30), copies 4.6%, splits 2.3%, idle 7-10%.
  Flat, like core 0's.
- **The idle hook's sync with the GX thread (jobs 510-515, `--idlewait US --ab idlewait`; gxthread.c):** at a guest idle
  loop core 0 waits until the GX thread has executed everything (as Dolphin's SyncOnSkipIdle). It does so **~3,900
  times a second**, and nearly every wait ends drained within 200 us (job 514: 38,241 drained, 520 past 200 us).
  Bounded waits of 2 ms and 200 us: neutral. **No wait (0 us): Ordon speed_pct +7.2%, heavy area +47%, but new game
  frames a second ('# new frames 10s') 9.59 -> 9.73 (+1.5%) and 6.84 -> 6.81 (even):** with no wait, idle skips run
  emulated time ahead of the work, so speed_pct inflates and stops measuring the frame rate. (The per-row xfbcopy_s
  column is printed rounded to whole copies; its phase means read +2.4% / +2.1%: use the 10 s line.) Timing drift of
  that kind would also move anything the game or its audio clocks by time rather than by frames. Not worth it: off
  (`--idlewait 0` to try); judge any change to idle-time behaviour by new frames a second, never by speed_pct.
  (The frames line here was the broken one, entry "The frame-rate line was broken" below; re-measured properly, job 556:
  6.30 frames a second against 6.35, the verdict holds.)
- **GX commands parsed in place (jobs 516-517, `--ab feeddirect`; fifo.c):** the decoder reads the ring record itself
  and only a command split across records goes through the pending buffer (PC state hash unchanged over 400 fields).
  **Heavy area -0.080 +- 0.019 points (-0.4%), Ordon -0.013 +- 0.368: no gain.** The copy (dec_feed ~1.9% of the GX
  thread's samples) was not where its time goes; off.
- **TEV programs' citro3d stages cached with them (jobs 520-521, `--ab tevenv`; pica.c):** 7,500 program changes a
  second cost update_tev 48 ms/s in the heavy area; with each cached program keeping its six built stages, **update_tev
  still 48-50 ms/s, speed +0.025 +- 0.018 (heavy) and -0.006 +- 0.365 (Ordon)**: building the stages was not the cost
  (the key, its hash and compare are). Off. **50 us idle waits (jobs 518-519): neutral** (new frames 6.88 / 6.90, 8.85 /
  8.93).
- **Where the GX thread stands after these (10-19):** five GX-thread changes measured neutral or worse in one day (a
  256 KB job ring, bounded idle waits, in-place parsing, cached TEV stages; ring batching was the gain). Its time is
  spread over ~15 items of 2-8% each; the one wait with a clear cause left is framebegin (C30, double-buffered frames,
  5-8% in the heavy area). Frame rates now (10 s lines): Ordon ~8.9-9.6 game frames a second, heavy area ~6.9 (that
  line was broken, see below: the real rates were ~9.05 and ~6.35).
- **The frame-rate line was broken; how to measure frames now (10-19, jobs 547-556):**
  - **What was wrong.** '# new frames 10s' divided the XFB copies by 10, but a report comes after ten perf rows, and
    each row is cut at the first emulated field past a second of real time: the window is ~10.8 s and holds a whole
    number of fields. The game is locked to **exactly two VI fields per picture** (new '# new frames hist' line: 2 in
    ~99% of gaps, all defaults, heavy area and Ordon). So the count stayed at ~69 a window ("6.9/s") for any speed
    within a few percent. Every "frames flat" verdict scored with that line, including C30's and the ports' below, is
    void; the real heavy-area rate was ~6.35 frames a second, not 6.9.
  - **Now.** The line divides by the window's real length; the histogram gives the real-time gaps (1/240 s bins) and
    the VI fields between copies. While the game stays locked at two fields, frames a second = fields_s / 2, so a
    settled speed_pct is a frame-rate measure; when a change unlocks it (fields per copy 3 and more), speed_pct
    inflates and only the frames line counts.
  - **Separate whole runs are precise in the heavy area:** two runs of a build agree to ~0.1% (6.349 / 6.344). A/B
    switches that recompile all JIT code (ports, opt) bias the on arm by ~2% there even when settled
    (core/tools/ab_phases.py now drops each phase's first 12 s): confirm port results with separate runs.
- **Overlapped frames, C30 (jobs 522-525, 548-560; `--dblframe`, pica.c): on by default.** citro3d's frame is never
  ended after the first; a frame ends with a split and, at present, a screen transfer queued by pica.c; the vertex
  arena and index arena alternate halves and the command buffer alternates with a second 2 MB one; a frame waits only
  for the GPU to finish the frame before last; retired textures, staging buffers and converted shapes carry the frame
  that retired them; the screen is swapped once the queue has passed the transfer; the queue (128 entries) is cleared
  only when idle. Stable through ~14 runs and ~60 on / off switches; the picture is right (job 522's captures); no
  early frame ends from the half-size arenas. **Separate runs, frames a second: heavy area 6.465 / 6.465 against
  6.349 / 6.344 (+1.9%); Ordon 9.060 / 9.060 against 9.053 / 9.056 (even); the title even by section (job 525, an A/B
  that recompiles nothing).** In the heavy area the GX thread's frame-begin wait goes (gxsync -98%, the GX thread 96.6
  -> 87.5% busy). C3D_GetDrawingTime (c3d_gpu_ms) is not updated while it is on.
- **What limits the heavy area, first reading (jobs 526-555; corrected by the joint states below: with overlapped
  frames on, core 0 is the limit and the gains here came from removing its waits for the GX thread):**
  - **Two more ports, exact, off.** `__GXSendFlushPrim` (`--port-hooks` bit 3, `--ab flushhook`): PC 4,822 checked calls
    and device 26,761, 0 differed; ~2,700 calls/s of 48 guest instructions (19 pipe words).
    `J3DColorBlockLightOn::diffLightObj` (bit 4, `--ab lighthook`; each light's four XF loads, 72 bytes, appended to
    `__GDCurrentDL` natively): PC 18,699 and device 3,973 checked, 0 differed; ~945 calls/s of ~3,800 guest
    instructions; the port takes 7.3 + 8.7 us a call (building, committing byte by byte), ~1.5% of core 0, against
    ~3.6% inclusive for the guest path (job 512 stacks). **Separate runs, heavy area: lights 6.338 / 6.344, flush
    6.334 / 6.332 frames a second against 6.349 / 6.344: neutral and -0.2%.**
  - **Core 0 has slack there:** every HLE stand-in off (0 runs; core 0 runs ~3M more guest instructions a second)
    left speed at 21.14% (job 542); removing core-0 work that sends nothing down the pipe (the lights port) changes
    nothing. What moves it is the GX side: overlapped frames +1.9%; gpbulk and ring batching off -4% (job 543); the
    three default ports +2.0% (ports off 20.71% against 21.13%, jobs 539-541), plausibly through their bulk pipe
    writes (one ring record each) rather than core 0's time. A lighter GPU (`--texdiv 8`, job 545) and a second vertex
    worker (`--vw2 on`, job 546, the GX thread 8 points less busy) did not raise it.
  - **Per guest thread (new '# guest threads 10s' line, `--profile on`, job 544):** the main thread 89% of core 0's
    samples, one other 4%; no thread soaks up spare time by polling.
  - **Idle waits again (job 556, `--idlewait 0`):** speed 31.1% but the lock breaks (fields per copy 2 in under half of the gaps, up to
    7) and frames are 6.30 a second (-0.7%): the old verdict stands.
- **Who waits on whom: joint states (jobs 561-563, 10-19; '# joint 10s', every run).** The sampler records core 0
  (running guest code / waiting for ring space / waiting for the GX thread to drain / other), the GX thread (working /
  ring empty / waiting for the vertex worker / waiting for the GPU) and the worker (working / idle) at the same instant.
  All defaults (overlapped frames on):
  - **Heavy area:** core 0 runs guest code ~97% of the time; it waits for the GX thread 0.3%. The GX thread works ~83%
    (idle ~13%, waiting for the worker ~8%); the worker works ~29%.
  - **Ordon:** core 0 running ~96%, waiting 1.1%; the GX thread idle ~12%, waiting for the worker ~10%.
  - **Title:** cutscene: core 0 ~93% running, the GX thread idle ~40%; later stretch: the GX thread idle ~75%, the
    worker idle ~96%.
  - **So core 0 is the limit in every scene now**, and the other cores have room: moving GX-thread work to core 3
    would not raise the frame rate; the levers are core 0's guest code (the JIT, ports) and anything that takes work
    off core 0. Core 0's samples by guest thread (`--profile on`, job 544): the main thread 89%, one other 4%.
- **Float lanes and the 2-way indirect table, re-measured (jobs 564-582, separate runs): both on by default.**
  Both were rejected from A/Bs that recompile all JIT code (C11 in 10-13; IND2 job 161), in scenes and builds where
  core 0 waited for the GX thread.
  - `--fcache on` (OPT_FCACHE, float lanes, exact): heavy area 6.551 / 6.556 frames a second against 6.459 / 6.457
    (+1.5%); Ordon 9.199 against 9.057 (+1.6%); title 36.52 / 19.51 / 13.13% against 36.12 / 19.05 / 13.05 (cutscene
    +1.1%, later stretch +2.4%, movie +0.6%).
  - `--ind2 on` (OPT_IND2): heavy area 6.513 / 6.514 (+0.85%); Ordon 9.137 (+0.9%); title +0.5% / +0.8% / +0.3%.
  - Both: heavy area 6.613 (+2.4%), Ordon 9.279 (+2.5%), title 36.77 / 19.48 / 13.14% (+1.8% / +2.3% / +0.7%).
  - `--psgx on` (the GX register setters' stand-ins): heavy area 6.401 (-0.9%), the old verdict stands.
- **The other rejected core-0 options, re-measured (jobs 583-604, separate runs, heavy area unless named):** baseline
  6.609 / 6.611 frames a second (open and close).
  - `--vin on` (compact vertex records in the worker's jobs): 6.653 / 6.656 (+0.7%); Ordon 9.344 against 9.264 (+0.9%);
    title 36.86 / 19.47 / 13.16 against 36.68 / 19.44 / 13.13 (+0.5% / +0.15% / +0.2%). **On by default.**
  - `--rotmask on` (OPT_ROTMASK): 6.648 / 6.646 (+0.6%); Ordon 9.289 (+0.3%); title 36.78 / 19.43 / 13.17. **On by
    default.** Both: heavy area 6.693 (+1.3%), Ordon 9.377 (+1.2%), title 36.93 / 19.44 / 13.19 (+0.7% / 0 / +0.5%).
  - Confirmed losses: `--crstub` 6.546 (-1.0%), `--befloat` 6.557 (-0.8%), `--calls` 6.532 (-1.2%), `--fastfloat`
    6.567 (-0.6%), `--relayout` 20.4% speed (-7.6%, with stalls); `--pipeps` 6.608 (even).
- **J3D's per-model work, probed for memoization (jobs 672-675, 678-680, 10-20; heavy area, `--memo ADDR`; the probe's
  table of previous calls now keyed by object, 512 entries, allocated on first use: 16 in turn never met a model
  twice):** J3DModel::calcMaterial 0x803276EC **91% of calls repeat their inputs exactly** (4,841 of 5,310 per 10 s;
  1,321 guest instructions, 124 input words a call), but that is ~0.7M guest instructions a second, under 1% of core
  0's guest work. J3DModel::calc 0x80327CF0 22% (3,063 instructions a call), J3DJoint::recursiveCalc 0x8032F3F8 9%
  (the view matrix at 0x80434bf0.. and an animation word change every frame). J3DMaterial::diff 0x803167D8 and
  J3DModel::diff 0x803279A0 could not be probed: every call stops on an exception in the interpreter run (the OS's lazy
  FP switch). **Not a lever** (C28).
- **cXyz operator stand-ins (`--psxyz`, jobs 660-663, 10-20; operator+, operator-, operator* translated in direct
  mode, 2.8% of the heavy area's core 0 between them in job 630's profile; PC HLE_CHECK 0 of 3,024,366 PS calls
  differed):** heavy area 6.672 -> 6.611 (-0.9%), Ordon 9.325 -> 9.319 (even). **Off.** Direct-mode stand-ins are entry
  hooks on the 3DS (a block exit and the stand-in dispatch a call), more than these 20-instruction wrappers cost; the
  same verdict as C20 (GPR save / restore) and C21 (GX setters). A wrapper this small would pay only as code the JIT
  emits in place.
- **The rest of the ruled-out switches, re-measured (jobs 626, 632-643, 10-20; heavy area, separate runs, all other
  defaults):** baseline 6.673 / 6.675 frames a second (open and close, two runs of one build: 0.03% apart).
  - Losses confirmed: `--traces on` 6.334 (-5.1%), `--vw2 on` 6.521 (-2.3%), `--steal on` 6.623 (-0.75%),
    `--sched off` 6.625 (-0.7%), `--dlcache on` 6.624 (-0.7%), `--vwmem 256` 6.629 (-0.65%).
  - Even: `--icbi on` 6.668, `--qbig on` 6.672, `--feeddirect on` 6.682 (+0.1%), `--cmdpld on` 6.667.
  - **Gain: `--psflush on` (the __GXSendFlushPrim stand-in, hle_ps.c) 6.785 (+1.7%)**, against "neutral" from its
    in-run A/B (jobs 452-453, which recompiles all code). Confirmed on another build (jobs 650-654): heavy area 6.679 -> 6.792 (+1.7%), Ordon 9.343 -> 9.417 (+0.8%), title 36.92 / 21.95 / 13.16% against 36.94 / 21.95 / 13.18 (even). **On by default.**
- **The ShdwDraw walk memo (jobs 605-619, 10-19; hle_tp.c "The walk memo", `--shdwmemo`):**
  - **Why:** in the title's later stretch the game makes ~1.3 ShdwDraw calls a frame (four distinct ones in turn; the long
    one ~6,500 stand-in segments, ~30,000 cells), and the stand-in spends ~2.6 s per 10 s report inside. 98% of calls
    repeat a previous call's walk exactly (PC SHDW_KEYSTAT: 369 of 377, all with the same outcome). Stand-in "calls" in
    the '# hle' line are segments (entries), not calls.
  - **What:** a call is recorded segment by segment (from the call's start, a triangle's return or the loop head to the
    next triangle, the loop head or the end): the walk's inputs where they differ from what the previous segment left,
    and the registers, CR0, XER[CA], frame words and bitset word it changed (delta-encoded, ~12 words a segment; five
    records of up to 192k words in linear memory). A call whose start inputs match a record replays it while each
    segment's inputs match. Records are dropped when a logged DMA (disc, ARAM, EXI) overlaps the collision data the walk
    read. The loop head (0x8007FB70), where the stand-in stops at the slice's budget, is now a stand-in entry too (exact;
    before, compiled guest code walked cells there until the next triangle and re-entered mid-cell).
  - **Timing:** a replayed segment may pass the slice's budget by at most 32,768 guest instructions. Unbounded (job 616)
    stretched every VI field (fields are scheduled from the last one): +1.7G cycles over 400 fields on the PC, and on the
    2DS more pictures (6.1 a second) but a slower game clock (~9.6 fields a second against ~11.4). Bounded: +0.05% cycles
    over 400 fields on the PC.
  - **Exactness (PC, tp_pc_5000 -> 5400):** memo off unchanged with the loop-head entry (state hash 44a254e5); memo on
    replays 491 of 507 calls, 1,108,244 segments, each compared with the walk from the same state (SHDW_MEMOCHECK): 0
    differed. Bugs the checks found on the way: the stand-in's leaf memo kept the previous call's generation when a
    call's start was replayed (one cycle off at the end; now bumped at every call start), and dead-at-start inputs
    (r14, r16-r18, r23-r25, frame 2224 / 2228) in the start key.
  - **2DS (job 618 against 617, title):** later stretch **19.09 -> 21.81% (+14.2%)**, real pictures 4.79 -> 5.95 a second
    (+24%), the game's clock ~11.3 -> ~13.5 fields a second, more pictures on two fields (45:17 against 32:18 two- and
    three-field gaps); cutscene 37.02 / 37.00, movie 13.18 / 13.15. Heavy area 6.669 against 6.673 frames a second
    (job 611, an earlier version).
  - The cutscene's shadows move every frame: its calls are recorded, not replayed (PC, tp_pc_900 -> 2100: 209 recorded, 1
    replayed), at no measured cost.
  - **Replay fast path (job 621 against 618, title):** a replayed segment's expected inputs are compared register by
    register with what the previous segment left (no unpack of the whole input set): the stand-in's time per report
    1,085 -> 890 ms, later stretch **21.81 -> 21.94%**; cutscene and movie even.
  - **Records copied out at their size (job 622):** a call is recorded into one 192k-word scratch stream and copied to a
    block of its exact size when it finishes (the long call ~80k words, the others a few thousand). Linear memory free
    1.6 -> 4.0 MB, later stretch 21.94% (even), and the later stretch still replays 184-190 of ~192 calls a report to the
    end (2-5 recorded, 1-2 dropped by DMA). The cutscene's rows show "calls 102, recorded 102": recorded, not replayed,
    as above.
  - **What the replay still costs (job 623, `--pmu split`, the stand-in's share of core 0 in the later stretch):** cycles
    12%, instructions 13-14%, **branch misses 37-44%**, I-cache misses 10%, D-cache read misses 7-9%. The remaining cost
    is the replay loop itself (~425k segments per 10 s, data-dependent branches per segment) plus the triangle bodies'
    compiled guest code (callbacks, GetTriPla), which run on every replayed triangle.
  - **The triangle path, measured for a cache (PC, SHDW_BODYSTAT, tp_pc_5000 -> 5400):** 431,847 passes from 0x8007FCE4 to
    the callback's bctrl (0x8007FE94) and 92,550 skips (to 0x8007FEA4), over **6,152 distinct (r15, triangle, r1) keys**.
    It changes GPRs r0, r3-r8, r11, r12, r31; FPRs f0-f13 and f28-f31 (both halves); CR, LR, CTR, FPSCR; frame words at
    r1 + 4, 12-32, 60-96, 100-116, 120-152, 2204, 2208; outside the frame it stores nothing but the interrupt handler's
    context saves. That bounds a per-triangle cache (REDESIGN C32b): ~85 passes and skips per key.
- **The triangle path as a native stand-in (`--shdwtri`, REDESIGN C32b; jobs 624-670, 10-20; hle_tp.c "The triangle
  path"):** written by hand from the guest instructions (0x8007fce4 to the callback's bctrl or the skip, with
  getPolyCode, ChkShdwDrawThrough and GetTriPla), run while the walk stops there, then the walk goes on natively from a
  skip. Instead of the cache: the path is fixed straight-line work, so native code needs no memory or guards.
  - **Exact (PC, TRI_CHECK=1, every run against the interpreter from the same state):** 1,047,230 runs over
    tp_pc_5000 -> 5400, 0 differed (FPSCR excepted: the PSVEC stand-ins leave FPRF, as the JIT does). Whole-state
    hashes differ (+0.0012% cycles over 400 fields): a triangle now runs in one go, so slices and interrupts land
    elsewhere (timing drift, owner-approved).
  - **v1, every float operation through the interpreter's handler (soft float):** a loss. Later stretch 21.94 ->
    19.80% (job 625 against 624), heavy area -0.8%, Ordon -1.3% (626-629).
  - **v2, float operations as the JIT computes them** (native for finite normal values, one rounding of a double result;
    the handler otherwise): later stretch 21.95 -> 22.13% (+0.8%, 644-645), heavy area and Ordon even (646-647).
  - **v3, the PSVEC calls inline** (their stand-ins' arithmetic; the in-place call, its checks and address lookups were
    most of their cost): **later stretch 21.89 -> 22.48% (+2.7%, 657-658) and 22.03 -> 22.46% (+2.0%, 665-666), ~5%
    more pictures (6.96 against 6.60 a second in the later stretch, core/tools/title_sections.py); heavy area 6.775 -> 6.794 (+0.3%), Ordon 9.395 -> 9.416 (+0.2%,
    667-670); cutscene and movie even. On by default.**
  - Where it stands: the ShdwDraw stand-in (walk replay and triangle path) is **34% of core 0's cycles** in the later
    stretch (job 659, `--pmu-split hle`; 12% for the replay alone, job 623), with 39% of core 0's I-cache misses and
    58% of its load/store stalls: ~5,300 cycles a triangle for ~150 statements. The callback's guest code runs between
    triangles and evicts the stand-in (hle_run with the path inlined is 8.9 KB, the walk 7.9 KB, replay 3.5 KB, against a
    16 KB I-cache).
  - **Where the stand-in's time goes (jobs 676-677, '# shdw where 10s', a phase byte the core-0 sampler counts; later
    stretch, share of all core-0 samples):** walk replay 7.3%, the path's reads and copies 6.3%, its vector maths
    10.7%, GetTriPla and the call set-up 4.3% (the path ~21%); with `--shdwcb` the callback adds 10.5% (as much as its
    guest code cost). **On-device bench (`--tribench N`, jobs 681-682: one later-stretch triangle's state, each piece
    4,000 times, warm caches):** the whole path ~4,200 cycles (reads 660, getPolyCode 120, first cross + dot 800,
    scale + add + second cross + dot 1,190, scale + add + GetTriPla 1,180, plane and set-up 280); the eight PSVEC
    operations 1,250 (~156 each), the 17 lfs / stfs moves 780. So it is not the I-cache: every piece is slow alike,
    VFP11 latency in dependent single-precision chains (one PSVEC op after another, stores between them) plus the
    byte-swapped loads and stores. Interleaving the two independent cross / dot / scale chains would be the next step
    for it; it pays only in the title (gameplay +0.2-0.3% for the whole path).
  - **v4, the fast form (`--trifast`, on; jobs 685-687):** every input loaded first, both cross / dot / scale / add
    chains computed side by side in registers, then each exit's final frame words written once; same operations per
    value as the PSVEC stand-ins, taken when every input and result is zero or normal (else v3 runs). PC: 1,048,860
    runs with the callback native too, 0 differed, the fast form took all 864,429 that reached the callback. **2DS: later
    stretch 22.41 -> 23.26% (+3.8%), pictures 6.92 -> 7.45 a second (+7.7%), movie 13.15 -> 13.42%, cutscene even (685
    against 686); the path's share of core 0 21.4 -> 9.9% ('# shdw where'); bench ~2,550 cycles a triangle against
    ~4,200; heavy area 6.800 -> 6.816 (+0.24%), Ordon 9.407 -> 9.443 (+0.4%) (688-691). On by default.**
  - **The callback rewritten compact (10-20; singles only, no FPR emulation: f0-f13 are dead when it returns; every float
    zero or normal, else guest code): PC 105,058 runs, 0 differed; 2DS later stretch 23.21 against 23.15% (692 against
    693), neutral again.** In place it is still 7.4% of core 0 (~1,600 cycles a call, most of which return after the
    light test) against ~300 warm in the bench: the cost is how the stand-in runs in place, not the callback's code.
    The later stretch's shadow stand-in then: replay 6.7%, path 11.5%, callback 7.4%.
  - **The callback read stage by stage (jobs 694-702):** the compact version still read and checked every input first
    (`--pmu-split cb`, job 694: ~3,060 cycles, ~1,300 ARM instructions, ~43 data micro-TLB misses a call with the
    bracket's ~1,100) though ~77% of calls return at the light test. Each test now reads only what it needs: ~2,540
    cycles, ~975 instructions with the bracket (698). **Later stretch 23.22 -> 23.47% (+1.1%), pictures 7.45 -> 7.73
    (+3.8%) (696 against 697); heavy area 6.824 -> 6.839 (+0.22%), Ordon 9.463 -> 9.528 (+0.7%) (699-702). `--shdwcb`
    on by default.** Exact: TRI_CHECK 105,058 runs, 0 differed. (`--pmu-split path`, job 695: the path with the
    callback ~5,780 cycles a run, the path alone ~2,700, as warm.)
  - **The replay's output loops over the set bits only (10-20, `__builtin_ctz`; a branch per bit of 29 mispredicted
    often):** SHDW_MEMOCHECK 156,206 segments, 0 differed; later stretch 23.47 -> 23.61 / 23.62% (two runs, 703-704,
    against 696), pictures 7.73 -> 7.77, cutscene and movie even.
  - **The callback natively too (`--shdwcb`, off):** psdRealCallBack and dDlst_shadowPoly_c::set (light, plane and box
    tests; the triangle pushed off the plane), so a triangle never leaves native code. PC: 105,058 runs, 86,597 through
    the callback, 0 differed. 2DS: later stretch 22.48 against 22.46% (job 664 against 665): **neutral**, so the cost is
    not the trips through guest code; ~27k callbacks per second return before set, ~6k are refused for a full poly.
  - The Translator v4 region of the same path (job 345, generated code, FP checked at the first FP instruction) lost:
    later stretch 17.47 -> 14.41%. The difference is the float model and the inline PSVEC calls.

### Hammock blocks and inlined leaves, modelled offline (PC interpreter profile, 2026-10-20)

- **What:** EMULATOR_CEILING.md H1's smallest disproving experiment. The PC interpreter counted every executed guest
  instruction (`PCHIST`) and every taken control transfer by (from, to) (`EDGEHIST`, new), and dumped MEM1 at exit
  (`MEMDUMP`, new); `core/tools/hammock.py` replays the counts through today's block rule and two hammock rules. The
  stand-ins ran as on the device (SHDWMEMO, SHDWTRI, SHDWCB, PORT on), so their native work is not counted. Two
  stretches: **cut** tp_pc_4290 -> 4490 (386 M guest instructions) and **later** tp_pc_5000 -> 5200 (236 M). Model
  self-test: a synthetic if-then over a compare (hm_selftest) gives 0% / 50% dead fields with the rewrite after the
  join and 0% / 0% without it, as constructed.
- **Today's rule reproduces the JIT:** 5.73 (cut) and 5.50 (later) guest instructions per block boundary, against
  JIT_REGSTAT's ~5.9 on the device. Boundaries by kind (cut / later): forward bc 38.2 / 34.8%, blr 19.2 / 18.8%,
  backward bc 12.8 / 17.8%, forward bl 12.8 / 10.2%, forward b 6.5 / 6.5%, bctrl 5.4 / 2.8%, backward bl 2.6 / 6.6%,
  bctr 1.7 / 0.6%, other enders (sc, rfi, mtmsr, uncompiled mtspr, mtfs*) 0.2 / 1.8%.
- **Hammock A** (a forward b / bc within W instructions stays inside the block) and **hammock B** (also: any other bc
  leaves only when taken, its fall-through stays inside), guest instructions per boundary and boundaries removed:

  | W | cut A | cut B | later A | later B |
  |---|---|---|---|---|
  | 8 | 7.60 (-24.6%) | 9.14 (-37.3%) | 7.01 (-21.6%) | 7.85 (-30.0%) |
  | 16 | 8.31 (-31.1%) | 9.77 (-41.4%) | 7.54 (-27.1%) | 8.31 (-33.8%) |
  | 32 | 8.85 (-35.3%) | 10.10 (-43.3%) | 8.18 (-32.8%) | 8.87 (-38.0%) |
  | 64 | 9.58 (-40.2%) | 10.58 (-45.8%) | 8.80 (-37.6%) | 9.31 (-40.9%) |

  Pre-registered threshold (block length x1.5): **met** at W = 32 with A (1.54x / 1.49x) and at W = 16 with B (1.70x /
  1.51x); x2 is reached nowhere (B at W = 64: 1.85x / 1.69x).
- **CR in ARM flags: the second threshold was mis-posed.** The compare that sets a conditional branch's CR field is
  already in the branch's block 96.3% (cut) / 94.6% (later) of the time today (C22's "a compare ends its block" is
  about where the field must be written, not where it is computed). What hammocks change is whether the field can stay
  unwritten: it is overwritten after the join before any read or exit only **10.5-14.2% (cut) / 7.2-10.3% (later)** of
  the time (W = 8-64). In the other ~87-93% the field is live at an exit and must still be built.
- **Calls and returns are the other third of boundaries:** calls are 2.76% (cut) / 3.27% (later) of guest
  instructions, ~16-18% of boundaries, and each brings a blr. Calls to a leaf that reaches its blr within N
  instructions with no call, frame, loop or tail jump: **N = 8: 29.7 / 35.1%; N = 16: 38.8 / 42.6%; N = 32: 51.5 /
  48.6%** of calls. The GPR save / restore entries (`_savegpr_14..31` / `_restgpr_14..31`, 0x803621A0-0x8036223C, pure
  stw / lwz off r11) take **22.3% (cut) / 16.7% (later) of all calls**: inlining those alone removes ~6-7% of
  boundaries (two per call). OSDisableInterrupts / OSRestoreInterrupts (0x8033D6F4, 0x8033D71C) take 9.4% of calls in
  the later stretch, but each holds an mtmsr, which ends a block today.
- **Reading (estimate):** at ~2-3.4 cycles of boundary cost per guest instruction (job 283), hammock B at W = 16-32
  removes ~0.7-1.5 of ~22 cycles: **+3-7% core 0**, plus fewer exit sequences in the I-cache; the dead-CR part adds
  ~0.5-1%. Inlining leaves under 16 instructions removes a further ~12-15% of boundaries (~+2-4%), the save / restore
  entries alone ~+1.5-2.5%. Below EMULATOR_CEILING.md's earlier H1 estimate (+8-15%).
- **Limits:** PC title stretches (tp_pc_4290 may not be the device's cutscene window), not gameplay; the 64-instruction
  block cap, page ends and register-cache pressure across internal edges are not modelled; boundaries are counted,
  not priced per kind (a taken side exit costs what an exit costs today).
- **Function-scope regions (same profiles, GENERAL_ARCHITECTURE.md section 2):** every direct non-linking b / bc
  within +-4 KB inside the region, loops included: **13.58 / 13.47 guest instructions per boundary (-57.8 / -59.2%)**;
  with leaves under 16 instructions inlined **19.11 / 20.44 (-70.0 / -73.1%)** (cut / later). Guest code holding the
  executed instructions: 90% in 17 KB (cut) / 61 KB (later), 99% in 39 / 228 KB.
- **Reproduce:** `PCHIST=pc.bin EDGEHIST=edges.bin MEMDUMP=mem.bin gcemu <disc> --load-state S --frames N`, then
  `python3 core/tools/hammock.py pc.bin edges.bin mem.bin --window 8,16,32,64`.

### X1: GX redundancy census (PC interpreter, title scenes, 2026-10-20)

**Scope:** GENERAL_ARCHITECTURE.md X1 only; measurement, no optimisation. Four **title scenes**, each 200 VI fields:
`tp_pc_4290 -> 4490`, `tp_pc_5000 -> 5200`, `tp_pc_900 -> 1100`, `tp_pc_6590 -> 6790`. No PC gameplay state exists;
the gameplay states are on the 2DS SD card. These results do not establish gameplay speed or repeat rates.
Native stand-ins were off (`GC3DS_NOHLE=1`, no AOT/PORT), so the interpreter sees the actual storing PCs and data
accesses. Instruction counts therefore differ from the earlier hammock profile with stand-ins enabled.
Rasterisation was off (`GC3DS_NODRAW=1`); the census decodes every vertex before that early return. No frame or speed
claim follows from these runs. Counts below are **measured** in the named runs; every percentage is **derived** from
those counts, pooled over that run's 200 fields, not an estimate or a mean of per-field percentages.

**Implementation:** `GXCENSUS=file` writes one text report (one row per VI field, totals, up to 20 outside PCs) and a
short stderr summary. `make -C core` also produces `gcemu-census`; the ordinary PC frontend execs this companion
only when the environment variable is set. The ordinary interpreter and GX decoder have no census calls or branches
(checked with `nm` on their objects); the extra cost when off is one startup environment check. ARM Linux and 3DS do
not compile the hooks or companion. No 3DS build or device run was performed for X1.

**(a) Unchanged state writes.** Each effective masked BP value, CP word, direct XF word and indexed XF load is
compared with its preceding shadow value. E0-E7 compare the selected TEV/konst bank, not the last bank addressed.
TEV includes BP 06-1F, 28-2F, C0-FD; texture includes BP 30-3F, 60-69, 80-BB; XF matrices are below 0600, lights
0600-067F; CP arrays are A0-BF; everything else is other. Groups are disjoint. Raw reports retain same/total counts
for every group in every field.

| Run (start -> end) | Unchanged / all words (measured) | All (derived) | TEV | Texture | XF matrices | XF lights | CP arrays | Other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 4290 -> 4490 | 6,095,021 / 6,592,500 | 92.45% | 93.85% | 86.21% | 89.69% | 99.59% | 50.00% | 93.86% |
| 5000 -> 5200 | 2,500,974 / 4,305,593 | 58.09% | 85.38% | 81.56% | 12.74% | 94.69% | 87.15% | 80.15% |
| 900 -> 1100 | 3,337,484 / 5,231,427 | 63.80% | 85.33% | 80.87% | 16.73% | 96.09% | 86.95% | 79.06% |
| 6590 -> 6790 | 5,276,138 / 7,581,709 | 69.59% | 84.19% | 83.34% | 30.94% | 97.12% | 86.52% | 82.77% |

**Verdict (a): confirmed in these title scenes** (>40% in every run). Build-list item 2 remains supported.
This is register equality, not permission to suppress side effects: PE tokens/finish, EFB copies, palette loads and
BP-mask consumption must keep their semantics even when the stored bits compare equal. Matrices are much less
redundant than material/light state in the geometry scenes.

**(b) Display lists: two requested bounds plus timing diagnostics.** Upper key: full list bytes (64-bit FNV hash
and length) plus all 16 incoming CP array bases. Lower key: upper plus the **whole** incoming BP/CP/XF shadow,
including the BP mask and both TEV colour banks; no per-command dependency tracking. This includes irrelevant
state and words overwritten by the list, so a low lower bound cannot disprove a more precise key. Array contents
are not hashed. No sampled calls, address-cache evictions, or silent set overflow: a full set aborts.

| Run | Calls (measured) | Prior-VI upper hits (measured; derived %) | Prior-VI lower hits | Prior-active-list-field upper | Prior-active-list-field lower | Same address, last use |
|---|---:|---:|---:|---:|---:|---:|
| 4290 | 100 | 0 (0.00%) | 0 (0.00%) | 99 (99.00%) | 0 (0.00%) | 99 (99.00%) |
| 5000 | 37,464 | 155 (0.41%) | 0 (0.00%) | 30,818 (82.26%) | 0 (0.00%) | 32,582 (86.97%) |
| 900 | 49,017 | 0 (0.00%) | 0 (0.00%) | 42,357 (86.41%) | 0 (0.00%) | 38,754 (79.06%) |
| 6590 | 33,200 | 0 (0.00%) | 0 (0.00%) | 30,983 (93.32%) | 820 (2.47%) | 28,479 (85.78%) |

The game generally renders every other VI field. The strict previous-VI counts above are the requested measurement,
including empty preceding fields. The active-field diagnostic retains the last field with list calls; it does not
change those strict counters. Job 107 compared the same list address with its **last use**, not the immediately
preceding VI field, and hashed bases plus strides. The geometry runs' last-use 79.06-86.97% reproduces its approximate
79-91% range; the strict previous-VI upper does not. At 4290 there is only one 96-byte list per picture, so its 99%
last-use rate is not representative of the geometry workload.

**Verdict (b): unclear for G2; the whole-shadow key is disproved as a useful key** (strict lower 0%; even active lower
0-2.47%, below 40%). The active upper is 82.26-93.32% in the geometry runs (>70%), showing the byte-level opportunity,
but not confirming state-compatible fragments. Keep build-list item 5 gated on a dependency-aware/relocatable key
and a retained window that survives empty VI fields; do not build G2 from the old job-107 figure alone.

**(c) FIFO sources.** Gather-pipe stores are classified by their actual interpreter instruction PC, over each
function's symbol extent. The 179 recognised functions occupy the GX SDK link span `8035921C..80360F98`, derived
from GX/__GX-named endpoints, including the local `__GetImageTileCount` and `__SetSURegs` inside that span. Using
only GX-prefixed names would wrongly report `__SetSURegs` as game code. DL bytes count the whole called length,
including NOP padding. The immediate call's 9-byte command belongs to its pipe source; fetched contents belong to
DL, so these are disjoint source volumes. Immediate store volume can differ from delivered 32-byte bursts at a
boundary because the write-gather buffer retains a partial burst.

| Run | SDK pipe bytes (measured; derived % of all three) | Other pipe bytes | DL fetched bytes | SDK share of pipe stores (derived) |
|---|---:|---:|---:|---:|
| 4290 | 35,785,300 (83.84%) | 6,890,400 (16.14%) | 9,600 (0.02%) | 83.85% |
| 5000 | 8,152,116 (13.31%) | 7,743,540 (12.64%) | 45,365,376 (74.05%) | 51.29% |
| 900 | 9,357,674 (13.20%) | 6,874,587 (9.70%) | 54,652,192 (77.10%) | 57.65% |
| 6590 | 32,624,367 (42.09%) | 11,542,896 (14.89%) | 33,344,000 (43.02%) | 73.87% |

**Verdict (c): confirmed mixed sources.** A native SDK command channel must still accept direct game/JSystem bytes
and called display lists. In the geometry runs the SDK accounts for only 13.20-42.09% of total source bytes; DLs
account for 43.02-77.10%. These byte counts bound the immediate decoder work item 4.3.2 could remove; they do not
price decode/state time (job 512) or confirm a speedup. Build-list item 6 stays gated on X3 and later timing evidence.

**(d) Lighting keys, weighted by vertices.** A vertex counts as lit when any active colour or alpha channel has XF
lighting enabled. Any enabled specular-light channel excludes the whole vertex and counts it separately. For each
non-specular vertex, key the list hash (immediate draws use their draw bytes), array bases/strides, matrix index,
channel controls, ambient/material colours, enabled lights' colour/attenuation, and each light's position/direction
transformed by the full affine inverse modelview (direction has no translation), rounded to 1/1024 model-space unit.
Vertex-sourced colours are included when selected. Singular modelviews count as misses, reported separately.
The per-matrix calculation is reused only within a draw; every vertex is counted and its decoded matrix index is used.

| Run | Non-specular lit vertices (measured) | Prior-VI key hits (measured; derived %) | Prior-active-lit-field hits | Specular vertices | Singular vertices |
|---|---:|---:|---:|---:|---:|
| 4290 | 0 | 0 (n/a) | 0 (n/a) | 0 | 0 |
| 5000 | 3,247,188 | 0 (0.00%) | 2,710,290 (83.47%) | 0 | 0 |
| 900 | 4,441,604 | 0 (0.00%) | 731,354 (16.47%) | 0 | 0 |
| 6590 | 2,163,900 | 0 (0.00%) | 1,442,826 (66.68%) | 0 | 0 |

**Verdict (d): unclear overall, strongly scene dependent.** Strict previous-VI hits are 0% (<20%), so that lookup
window is disproved; it is not evidence that lighting must be approximated/bounded. Retention across empty fields
changes the result: 5000 and 6590 exceed 50%, while 900 is below 20%; 4290 has no lit vertices. Item 5 must retain
both cached and bounded-lighting investigations, gated on gameplay and a timing/validity test, rather than assuming
one lighting solution globally. This is key repetition, not an exact lit-colour-cache implementation: array contents,
skinning and non-rigid normal transforms may need additional guards. Quantisation estimates an opportunity; it is
not an authorised rendering approximation. No specular work was observed, so exclusion is synthetically tested only.

**(e) Outside SDK data accesses.** Symbols identify `__GXData` at `80456580` (a pointer) and its `gxData` object at
`8044CE80`, size `5B0`; the live pointer is followed at startup and after writes. Watch struct loads/stores, pointer
loads/stores and the PI FIFO write-pointer register at physical `0C003014` (effective alias `CC003014`). The gather
port itself (`CC008000`) is the source-volume probe in (c), not a readable pointer variable. Fast RAM paths and slow
MMIO paths use the same interpreter hooks; mapped physical aliases share the watch. Native stand-ins/AOT and
DMA/cache-line operations are not observed as individual guest load/store instructions, hence native code is off in
these runs. GXCENSUS reports a warning flag if native stand-ins remain enabled.

**Measured:** all four runs have **0 outside struct loads, 0 struct stores, 0 pointer loads and 0 pointer stores**;
there are **0 PCs** to list (the report has the top-20 facility). **Verdict (e): confirmed for these title scenes**:
4.3.2 remains open, and build-list item 6 still needs X3 and gameplay coverage. This is not a proof that all TP
scenes or other games keep SDK state private.

**Planted self-test:** `GXCENSUS_PLANT=1` corrupts the observed incoming shadow before the ordinary equality test,
salts list/lighting keys by field, swaps SDK/direct source classifications, and passes a recognisable synthetic
outside struct reader and pointer writer through the memory-watch path. It never writes guest/GX state.
The no-disc synthetic fixture has consecutive active fields, ensuring the primary previous-VI counters have hits
to destroy even where real scenes have none. The normal fixture's oracle rejects the planted fixture:

| Probe | Synthetic normal -> plant (measured) | Real 5000 -> 5200 normal -> plant (measured) |
|---|---|---|
| (a) | TEV unchanged 3 -> 0; total unchanged 83.33% -> 0% | unchanged words 2,500,974 -> 0 |
| (b) | upper/lower hits 2/2 -> 0/0 | active upper 30,818 -> 0 (strict lower already 0) |
| (c) | SDK/direct bytes 24/12 -> 12/24 | 8,152,116/7,743,540 -> 7,743,540/8,152,116 |
| (d) | lighting hits 8 -> 0 (12 eligible; 12 specular excluded) | active lighting hits 2,710,290 -> 0 |
| (e) | outside struct reads/pointer stores 0/0 -> 3/3 | 0/0 -> 200/200 at FFFFFFFC, PLANTED_OUTSIDE |

The 6590 run also moves the nonzero active lower bound: 820 -> 0. All four real plants drive populated prior-field repeat
counters to zero and add one outside reader/writer per field. The synthetic fixture additionally checks affine
inverse scaling/translation and invariant model-space light position under common camera translation.

**Exactness and checks:** all four final runs have identical complete `state hash` lines (including cycle) and
`OPMIX` instruction counts in off/on/plant modes. Both comparison checks have negative self-tests (corrupted
hash/count rejected).

| Run | State hash (measured, identical in all three modes) | Interpreted instructions (measured) |
|---|---|---:|
| 4290 | `51a944177b96fd8f` | 439,627,123 |
| 5000 | `4658c8957510abdc` | 1,621,605,400 |
| 900 | `363817a95d0a0766` | 321,460,241 |
| 6590 | `d7000c85cf89e935` | 503,577,574 |

`core/tests/gxcensus/run.sh` passes and shows all five plants moving their counters. The unmodified required CPU
runner was executed and **fails to link** optional `aot_*`, `hle_*`, `g_memoRec`/`memo_access` hooks; the same failure
was reproduced from baseline `7cfa41a0`. It is not reported as passing. The X1-local
`core/tests/gxcensus/run_cpu_checks.sh` supplies inert, test-only optional hooks and runs that unchanged suite:
**420 directed cases, 12,800 differential cases, 0 failures; both CPU plants caught (313 and 181 mismatches);
11,806 disassembly comparisons, 0 mismatches**, with gcc/clang/devkitARM warning checks passing. No optimisation,
JIT, vertex-transform implementation or 3DS host behaviour was changed.

**Limits:** these are title scenes, not gameplay. First measured fields have no predecessor and count cold misses;
no extra warm-up is silently discarded. The whole-shadow lower bound deliberately over-keys. Array bases do not
prove static vertex contents. Quantisation and skinning need an exact cache validity check before any cache is built.
Symbol recognition here is for GZ2E01 and its contiguous SDK linkage, not an automatic cross-title SDK detector.
The supplied symbols file SHA-256 is `8b8c98b86b6270543709adbbd489ca4a5cd4fa5c30fd4a410420702fd37a085a`.
64-bit key hashes have a collision risk; this census does not establish bit-exact cache reuse. No instruction count,
byte count or repeat share is a measured speedup; X5 timing and gameplay remain unrun.

**Evidence:** [per-field reports and checks](gxcensus/2026-10-20/checks.txt); sibling `STATE-on.txt` and
`STATE-plant.txt` files retain all counters and outside-PC rows. Only counters and state-hash/instruction-count
signatures are committed; no commercial game data, screenshots, generated AOT code or save states.

**Exact reproduction (WSL Debian, repository root):**

```bash
make -C core
export GC3DS_ISO='/mnt/c/Users/tyler/TP3DS/game/Legend of Zelda, The - Twilight Princess (USA).nkit.iso'
mkdir -p /root/gxcensus-x1
for span in 4290:4490 5000:5200 900:1100 6590:6790; do
    start=${span%:*}; end=${span#*:}
    for mode in off on plant; do
        envs=(GC3DS_NOHLE=1 GC3DS_NODRAW=1 OPMIX=1)
        if [ "$mode" != off ]; then envs+=(GXCENSUS=/root/gxcensus-x1/$start-$mode.txt); fi
        if [ "$mode" = plant ]; then envs+=(GXCENSUS_PLANT=1); fi
        env -u GXCENSUS -u GXCENSUS_PLANT -u AOT -u GC3DS_PORT -u PORT_CHECK "${envs[@]}" \
            core/build/gcemu "$GC3DS_ISO" --load-state /root/tp_pc_$start.state --frames "$end" \
            --out /root/gxcensus-x1/$start-$mode > /root/gxcensus-x1/$start-$mode.log 2>&1
    done
done
bash core/tests/gxcensus/run.sh
python3 core/tests/gxcensus/verify.py --runs /root/gxcensus-x1
bash core/tests/gxcensus/run_cpu_checks.sh
```

`GXCENSUS_SYMBOLS=/root/tpdecomp/config/GZ2E01/symbols.txt` is the default and can be overridden. `--frames` is the
absolute boot-field number, not the duration after loading the state. Stop here: X2-X6 and all optimisations are unrun.

### X1b prerequisite: primary CPU test runner repaired (2026-10-20)

The main `bash core/tests/cpu/run_tests.sh` now directly links `core/tests/cpu/optional_hooks.c`:
explicit inert implementations of the optional AOT/HLE/memo hooks. These tests exercise the interpreter;
native stand-ins, AOT dispatch and memory recording remain disabled. The former census-specific hook file
was moved here, with AOT/HLE prototype headers so interface drift is checked. The old census wrapper now
only delegates to the primary runner; no compiler/PATH shim is required.

**Measured, run X1b-cpu (default seed/counts, full primary command):** 420 directed cases, 12,800 differential
cases, 0 failures; 11,806 disassembly comparisons, 0 mismatches. The logic and floating-point planted cases
fail as required: 313 and 181 mismatches respectively (8 failed batches each). gcc, clang and devkitARM
warning checks pass. The command's incidental host throughput measurements, not an emulator performance claim,
are retained with the [complete check summary](gxcensus/x1b-2026-10-20/cpu-checks.txt).
Reproduce: `bash core/tests/cpu/run_tests.sh` from the repository root in WSL Debian. This repairs the baseline
link failure documented in X1; it does not change the interpreter or implement an optimisation.

### X1b: decoder cost, read-before-write keys, and retained lighting (PC, 2026-10-20)

**Scope and runs:** X1b only, no optimisation. The primary CPU runner was repaired and committed separately as
`70fa649` (preceding subsection). The census companion remains PC-only; the normal runner and decoder contain no
census calls when off. The four **title states** are the same as X1, each over **200 VI fields**: 4290 -> 4490,
5000 -> 5200, 900 -> 1100, 6590 -> 6790. **No PC gameplay state exists**; gameplay states are on the 2DS SD card.
All runs use the raw interpreter (`GC3DS_NOHLE=1`, no AOT/PORT) and `GC3DS_NODRAW=1`; the observer evaluates draw
dependencies/lighting keys before rasterisation's early return. No device run or speedup is claimed.

Run IDs below are **X1b-final-STATE-r1/r2/r3**, three separate, sequential PC census runs per state (no concurrent
performance jobs), plus one off reference and one planted run. Host: WSL Debian, Intel Core i5-9300H, gcc 14.2.0.
Counts and sampled counter readings are **measured** in those named runs. Repeat percentages from exact counts are
**derived**. Extrapolated full decode costs/shares are **estimates** from sampling, explicitly labelled below.

**(a) Price complete unchanged commands, not unchanged words.** The observer prepass classifies each complete BP,
CP, direct XF or indexed XF command, before the real executor runs it. An eligible command changes no word:
all words in an XF transfer must match; BP requires both the selected logical colour bank and its BP mirror to
match, with the incoming mask already all ones. PE signals, copies, palette loads, cache invalidation and mask
commands are excluded from eligibility even if bits match. Equal subwords in a mixed XF command are not counted
as a removable complete command. The existing early returns/version guards still execute; no filtering is built.

The companion samples the first occurrence of each class and then a deterministic pseudorandom 1/64 of events,
using fenced TSC readings. Timer frames are **exclusive**: child commands, observer work, backend draw calls, EFB
copies and palette byte copies are paused out. The denominator includes command execution, DL dispatch/scan loops,
failed partial-command parse attempts, and FIFO ingress append/memmove/dispatch. It is the full PC decoder front
end, not only successful state-command handlers. Every class's event count, sample count, raw/corrected ticks and
number of timed segments is retained per field. The minimum of 4,096 empty counter pairs is subtracted per segment;
TSC ticks are host elapsed-counter units, **not ARM11 cycles or retired instructions**.

For class c, estimated time is `exact_events[c] * corrected_sample_ticks[c] / samples[c]`; sum all classes for the
denominator, and only BP/CP/XF/indexed-equal classes for the numerator. The table gives **estimated shares**, with
mean and standard error across the three separate repeats (percentage points), not a measured emulator speed gain.

| Run | Eligible no-op commands (measured, r1) | Estimated share r1 / r2 / r3 | Estimated mean +/- SE |
|---|---:|---|---:|
| 4290 | 4,230,219 | 26.54% / 27.84% / 28.63% | 27.67 +/- 0.61 |
| 5000 | 1,633,074 | 18.35% / 16.22% / 18.70% | 17.75 +/- 0.78 |
| 900 | 2,070,307 | 19.35% / 25.64% / 19.73% | 21.58 +/- 2.04 |
| 6590 | 3,583,002 | 20.93% / 21.51% / 19.52% | 20.65 +/- 0.59 |

For scale, the **derived corrected mean ticks per sampled eligible command** in r1 are:

| Run | BP equal | CP equal | XF equal | Indexed XF equal |
|---|---:|---:|---:|---:|
| 4290 | 61.95 | 58.39 | 87.76 | not observed |
| 5000 | 65.56 | 61.96 | 102.59 | not observed |
| 900 | 54.50 | 64.53 | 84.50 | not observed |
| 6590 | 57.81 | 52.92 | 81.23 | not observed |

**Verdict (a): a gross budget greater than 3% exists in all four title windows.** This does **not** confirm a net
filtering gain: a predecode filter must still identify boundaries and compare state, and could simply duplicate the
cheap guards already present. The estimate includes dispatch, parsing, equality checks and trivial shadow stores
of no-op commands, not a second saving from state updates the decoder already skips. X1's 58-92% unchanged-word
share must not be read as removable state-work time. Build item 2 remains a measured-budget candidate, requiring
an incremental-cost prototype and separate whole-device runs before any performance verdict/default change.

The observer prepass warms input/state and the measured companion still has skip-observer guards in its handlers.
PC cache, scheduling and timer costs are different from ARM11. Raw unsubtracted samples remain in the reports for
sensitivity checks; repeat spread is shown rather than selecting the best run. No conversion to total GX-thread
or game speed is justified by this experiment.

**(b) Dependency-aware incoming state.** Each list starts with an incoming shadow snapshot and a clear written-word
mask. The command tracer follows the reference decoder/vertex/XF/TEV state-read paths, records a read only before a
write to that word, and keys `(list bytes+length, all 16 array bases, sorted external word IDs+incoming values)`.
BP mask consumption/partial writes, separate TEV/konst colour banks and CP-sourced indexed XF loads are included.
Equality/version-bookkeeping reads are not semantic incoming-state dependencies. Draws select the enabled arrays,
VAT, actual vertex matrix indices, active texture units/TEV stages/indirect matrices, channels/lights/texgens,
projection and viewport; they do not key the whole XF file. The renderer's state-initialisation reads are included
at word granularity, not subject to per-pixel dead-value elimination. This catalogue is evaluated even with NODRAW.

First execution learns the list's read-set signature. **Every later execution retraces and validates the guards**;
a different read set becomes a different key (word IDs are part of the hash), rather than silently reusing an
incomplete first manifest. Changed list bytes establish a new manifest. These are observation manifests, not
translated/cached execution. Previous-active-field sets rotate only when that VI field had list calls. The other
comparison is the immediately preceding use of the same list address, including uses in the same field.

| Run | Calls (measured) | Hits vs previous field with list calls (measured; derived %) | Hits vs same address's last use |
|---|---:|---:|---:|
| 4290 | 100 | 99 (99.00%) | 99 (99.00%) |
| 5000 | 37,464 | 17,407 (46.46%) | 18,012 (48.08%) |
| 900 | 49,017 | 24,212 (49.40%) | 20,603 (42.03%) |
| 6590 | 33,200 | 20,331 (61.24%) | 17,731 (53.41%) |

The reduction from the old full shadow is visible in the **derived mean external words per list call** (the fixed
16-base prefix is additional; these are reads before writes, not all words the list touches):

| Run | BP | CP | XF | Logical TEV banks | BP mask | First/new-byte manifests (measured) | Guard/read-set changes (measured) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4290 | 0.00 | 0.00 | 0.00 | 0.00 | 1.00 | 1 | 0 |
| 5000 | 9.37 | 5.20 | 78.95 | 7.03 | 0.40 | 3,294 | 7,515 |
| 900 | 10.05 | 4.55 | 70.10 | 6.16 | 0.45 | 6,341 | 8,410 |
| 6590 | 10.20 | 5.42 | 94.72 | 7.28 | 0.38 | 2,132 | 10,348 |

**Verdict (b): unclear for the geometry scenes.** All three geometry-state repeat rates lie between the <40% kill
and >70% support thresholds, for both requested comparisons. The 4290 window exceeds 70%, but it has only 100 tiny
96-byte list calls and no lit vertices; its external read set is only the BP mask, so it does not establish a
general geometry-fragment opportunity. The dependency-aware key substantially improves on X1's whole-shadow
0-2.47% lower bound, but does not clear the build gate for G2. Keep item 5 gated; gameplay and a deliberately
relocatable key are next measurement options, not implementations authorised by these title data.

This is the current PC reference's dependency catalogue, not proof of bit-exact reuse on every GX/PICA feature.
Array/TMEM/texture contents, EFB feedback and resource versions are outside this requested state-only key. A future
cache needs validity checks as well as a matching key; additional backend dependencies can reduce these rates.

**(c) Lighting retention by completed pictures.** N=1,2,4 means the last N **EFB-to-XFB copies**, matching the device
new-picture instrument, not the last N VI fields or only the last N lit fields. Empty VI fields do not evict keys;
unlit/specular-only pictures do age them. Lookups exclude the current picture, so repeated draws within one picture
cannot manufacture hits. Keys are otherwise X1's model-space, 1/1024-quantised light keys, with vertex-selected
matrix indices and material/ambient/channel inputs. Specular vertices are excluded as before. The first observed
picture is cold; no warm-up or failed picture is silently removed from the denominator.

| Run | Completed pictures (measured) | Eligible lit vertices (measured) | N=1 hits (measured; derived %) | N=2 hits | N=4 hits | Specular / singular |
|---|---:|---:|---:|---:|---:|---:|
| 4290 | 100 | 0 | 0 (n/a) | 0 (n/a) | 0 (n/a) | 0 / 0 |
| 5000 | 84 | 3,247,188 | 2,884,001 (88.82%) | 2,884,001 (88.82%) | 2,884,001 (88.82%) | 0 / 0 |
| 900 | 100 | 4,441,604 | 731,354 (16.47%) | 733,102 (16.51%) | 733,512 (16.51%) | 0 / 0 |
| 6590 | 100 | 2,163,900 | 1,442,826 (66.68%) | 1,442,826 (66.68%) | 1,442,826 (66.68%) | 0 / 0 |

**Verdict (c): scene dependent, and a larger retained window does not rescue state 900.** States 5000 and 6590
clear >50% already at N=1 and support a lighting cache in those scenes. State 900 remains below <20% even at N=4,
supporting the bounded-lighting direction there. State 4290 supplies no lighting evidence. This does not authorise
an approximation: quantisation, changing vertex arrays/skinning, non-rigid normal transforms and exact validity
still need checks before a real cache, and gameplay remains unmeasured. Item 5 needs scene-dependent evidence,
not an assumption that a global cache or global light bound works everywhere.

**Planted cases and exactness.** `GXCENSUS_PLANT=1` makes observed unchanged state compare unequal, perturbs the
incoming dependency key on every list call and the lighting key by picture/field. All three new hit/budget counters
fall to zero where populated, while the real command/state execution is untouched. The synthetic decoder fixture
has genuine positive hits for all windows: it proves overwritten and unused state are ignored, reads before later
writes remain, partial masks and indexed XF reads matter, and changed draw guards do not freeze the first read set.
It also checks a masked copy whose **raw command** creates an XFB picture even when its masked BP mirror does not.
The A/B/C/A lighting sequence distinguishes N=1,2,4; unlit pictures test expiry at the window boundary.

The independent `GX_CENSUS_TIMING_PLANT` test deliberately charges a slow backend to decode. Its normal timing
oracle fails, proving observer/backend exclusion is checked rather than merely claimed. Only its temporary test
FIFO object has that define; no production timing run uses it. The normal synthetic report oracle rejects the
ordinary planted report. Field-to-total accounting, repeated counter consistency, state-hash and instruction-count
comparisons also reject their planted inconsistencies.

All four off/on/plant triples and all eight extra on repeats have identical complete state-hash lines and interpreted
instruction counts. All repeat runs also have identical non-timing counters. The primary CPU runner was rerun after
the census extension: **420 directed, 12,800 differential cases, 0 failures; plants caught (313/181 mismatches);
11,806 disassembly comparisons, 0 mismatches**, with gcc/clang/devkitARM warning checks passing.

| Run | State hash (measured, all modes/repeats identical) | Interpreted instructions (measured) |
|---|---|---:|
| 4290 | `51a944177b96fd8f` | 439,627,123 |
| 5000 | `4658c8957510abdc` | 1,621,605,400 |
| 900 | `363817a95d0a0766` | 321,460,241 |
| 6590 | `d7000c85cf89e935` | 503,577,574 |

**Method-check measurements retained, not selected away:** an early 5000 -> 5008 smoke run estimated 26.48% using
successful commands only. The preliminary 4290 command-only trials estimated 65.02%, 68.24%, 65.91%; they omitted
partial parses/ingress and are not the final denominator or verdict. A first full-front-end corpus preceded the
masked-copy edge-case audit; its three-trial estimates are retained below for comparison, separately from the final
audited corpus above. The final counter sets match that earlier full-front-end corpus exactly; no result depends
on the masked-copy corner occurring in these title windows. Synthetic timing values in the check log are functional
test observations, not game performance measurements.

| Earlier full-front-end validation runs | Estimated r1 / r2 / r3 no-op decode share |
|---|---|
| 4290 | 28.21% / 28.28% / 25.79% |
| 5000 | 16.85% / 17.59% / 17.56% |
| 900 | 18.04% / 19.53% / 18.47% |
| 6590 | 20.66% / 20.62% / 22.08% |

**Evidence:** [checks and exactness signatures](gxcensus/x1b-2026-10-20/checks.txt), sibling `STATE-on.txt` and
`STATE-plant.txt` retain per-field counters and decode samples; repeat/validation `*-totals.txt` files retain their
full measured aggregate counters/sample sums. The run summaries and preliminary method checks are retained too.
No game data, states, screenshots or generated AOT sources are committed. No 3DS build/run, G2, filtering or lighting
optimisation was built. Normal PC object files have no census references; the 3DS/ARM code removes the probes at
compile time.

**Reproduce in WSL Debian, repository root (disc and states read in place):**

```bash
bash core/tests/cpu/run_tests.sh
make -C core
bash core/tests/gxcensus/run.sh
bash core/tests/gxcensus/run_x1b.sh
export GC3DS_ISO='/mnt/c/Users/tyler/TP3DS/game/Legend of Zelda, The - Twilight Princess (USA).nkit.iso'
python3 core/tests/gxcensus/run_title.py --out /root/gxcensus-x1b/final --repeats 3
python3 core/tests/gxcensus/verify_x1b.py --runs /root/gxcensus-x1b/final --json /root/gxcensus-x1b/final-results.json
```

`run_title.py` invokes the exact pairs 4290/4490, 5000/5200, 900/1100, 6590/6790 with `--load-state` and absolute
`--frames` counts, and sets `GC3DS_NOHLE=1 GC3DS_NODRAW=1 OPMIX=1` plus `GXCENSUS=file` for on/plant. It performs the
off reference, normal census, `GXCENSUS_PLANT=1`, and two additional normal census runs, sequentially per state.
Symbols default to `/root/tpdecomp/config/GZ2E01/symbols.txt` (`GXCENSUS_SYMBOLS` overrides); the file is unchanged
from X1. Stop after X1b. Gameplay coverage, net filter cost and device-time validation remain next, unrun work.

### X3: GPR helper inlining and retained-register native calls (2026-10-20)

Scope: X3 only. Four independent switches, all off by default: `--gprinline`, `--zgpr`, `--zgx`, `--zxyz`.
The legacy `--psgx` and `--psxyz` groups stay off, including on short-budget fallback paths.
Recognition of GPR suffix helpers uses instruction words, consecutive registers and aligned displacements, then
plain `blr`, rather than TP addresses. Inlining uses normal integer emitters and cached operands, sets LR at the BL,
uses each callee PC for exception snapshots, accounts for the full callee, and marks every callee code page for icbi.

The native path copies descriptor-selected entry operands from existing cache slots into a 136-byte host-stack
argument block (34 words; derived from `HleFastArgs`), without allocating or evicting cache slots. Unrelated
cached r14-r31 remain in host callee-saved registers across C. SDK volatile GPR slots are discarded (owner-approved
ABI dead state); internal GPR prologue helpers retain volatile arguments too. Restore helpers reload their written
cached registers. FP inputs are selectively materialised and written FP lanes invalidated. Refusal and partial-stop
exits preserve the appropriate snapshot. Native GPR accepts MEM1 only: the JIT's MEM1 proof does not prove the
locked-cache BAT. Unsupported regions enter the original guest helper. Entry-operand variants are generated from
the existing translated C by `ps_fast.py`; the existing translations remain byte-identical.

A budget guard preserves the original BL/return event boundaries: insufficient budget enters the original callee;
completed calls yield at the original return boundary when needed. The first qemu attempt matched frames but changed
instruction totals by 3-4 instructions, so it failed the exactness gate. The guards and paired budget fixtures were
added before scoring hardware. An earlier default/OPT7 chain run also exposed four out-of-scope comparisons from the old
fault-alias classifier: clearing JIT_FAULT_BIC bit 30 maps 0x7ffffxxx to 0x3ffffxxx, outside compared MEM1. The checker
now classifies this alias correctly and has a plant that restores the wrong classification. Neither failed run is
counted as passing. The alias correction is a harness scope fix, not a production memory fix; exact behaviour at
those excluded aliases remains unvalidated.

Checks and measurements below use whole runs. The PC states are title scenes, not gameplay. Hardware gameplay uses
Ordon's scripted square walk and the heavy-area state on the 2DS SD card. No game data or state files are committed.

**Software exactness (measured, final3 runs).** The CPU suite passed 420 directed and 12,800 differential
cases, with 436,891 generated instructions over 261 mnemonics; both logic/FP plants were caught (313/181 mismatches),
and all 11,806 disassembly comparisons matched. Its gcc/clang/devkitARM compile checks passed. The 3DS build completed;
existing warnings remain (including the old CMPU macro and toolchain executable-stack warning), with no warning
introduced by the new code. The generator equality check passed and its deliberately different output failed.

| PC HLE_CHECK title run (measured) | PS-library calls checked | New GPR / GX / cXyz calls | Differences |
|---|---:|---:|---:|
| 4290 -> 4490 | 4,112,564 | 2,387,966 / 1,505,100 / 200 | 0 |
| 5000 -> 5200 | 9,376,584 | 3,333,037 / 489,048 / 59,428 | 0 |
| 900 -> 1100 | 2,867,899 | 1,452,562 / 569,809 / 99,403 | 0 |
| 6590 -> 6790 | 4,825,171 | 2,153,727 / 1,554,881 / 70,595 | 0 |

Derived sums: 21,182,218 PS-library comparisons, including 13,675,756 calls through the new operand variants
(9,327,292 GPR, 4,118,838 GX, 229,626 cXyz). The forced-stop 5000 -> 5020 run checked 928,486 calls, with
2,868 stops inside callees and zero differences. The 5000 -> 5002 result plant produced 80,943 differences in
80,943 checks, so the oracle cannot silently accept that corruption. These PC checks use interpreter entry operands;
the differential JIT fixtures separately exercise extraction from dirty cached slots, preserved r31, refusal,
partial stores, FP-unavailable exits, precise DSI, mirrored RAM, callee-page invalidation and short event budgets.

The final directed corpus contains 636 cases: every save/restore suffix r14-r31 in both inline/native modes,
seven memory/code forms (including mapped and unmapped locked cache), four synthetic native-ABI cases and
128 paired original/new runs over budgets 1-32. Plant 13 changes a helper slot, 14 loses a preserved dirty slot,
17 restores the bad fault-alias classifier, 18 removes event guards and 19 bypasses the native memory guard.
The standalone memory-guard check passed all 636 cases; plant 19 produced 72 failures, including a wrong DSI.
The fixtures contain synthetic instructions only. The existing generated SDK translations were not regenerated
or edited; the new entry variants derive from that unchanged source.

The required final3 JIT matrix passed: default/seed 1, OPT=7/seed 1, checked memory/seed 1 and checked
memory/seed 3 each generated 20,000 single-block and 20,000 linked-chain cases with zero failures, plus 636 directed
cases per invocation. All existing plants and new plants 13,14,17,18,19 were caught in all four configurations.
Plant 13 produced 195 failures in default/OPT=7 and 196 in checked-memory runs; plants 14,17,18,19 produced
87,1,88,72 respectively in every configuration. The fault-alias classifier
still deliberately excludes unmapped aliases outside the compared RAM windows; correcting that classifier does
not expand the emulated memory model. No production memory semantics changed for that fix.

Measured exclusions: default and OPT=7 each left 5 single-block and 34 linked-chain fault-alias cases un-compared;
both checked-memory runs left zero cases un-compared. The generated-case count is not a claim that these aliases
were validated. Faults patched 1,783 single-block / 8,337 chain sites in each fault-mode run.

All ten final3 qemu runs reached the requested field and passed the instruction-count / last-frame gate,
with each option tested separately against off. Both the planted count and planted frame signature were rejected
for each starting state. Measured counts and final-frame SHA-256:

| State -> field | Instructions, all five modes (measured) | Final frame SHA-256, all modes |
|---|---:|---|
| 950 -> 970 | 37,039,663 | `8a7c99c6a6557b5bf99953b695c0e7569d575cf16e6ca884864b296c2f88484e` |
| 850 -> 854 | 8,561,347 | `8a208fc4c96e1a00d0744025026beaad7ae3420f27689bde3597c0182797d96c` |

Measured native successes (GPR / GX / cXyz respectively, in their independent runs): 950 -> 970 had
162,947 / 58,721 / 9,549; 850 -> 854 had 25,364 / 10,836 / 1,982. All reported zero runtime refusals.
These totals do not count near-budget sites that went directly to the original guest helper before calling C.
HLE_CHECK uses the existing ABI comparison contract: volatile dead registers and private callee frames are
excluded by the owner's earlier approval. Neither it nor the final-frame test proves full machine-state equality
outside that contract; the new GPR directed fixtures compare all GPRs and precise exception state.

**2DS whole runs (measured; derived relative changes).** All jobs use build `871a05981ef37a0e`. Each speed is a per-run mean plus/minus its row standard error in percentage points. These row errors do not measure independent-run uncertainty. No opening/closing controls are pooled.


Ordon: rows from 50 real seconds; pictures/s from the existing `runs.py` windows.

| Mode / job | Speed % (measured mean ± row SE) | Derived Δ vs opening / closing baseline | Pictures/s (measured) |
|---|---:|---:|---:|
| baseline-first / 708 | 31.616 ± 0.206 | +0.00% / -0.09% | 9.418 |
| gprinline / 711 | 31.869 ± 0.199 | +0.80% / +0.71% | 9.494 |
| zgpr / 714 | 30.974 ± 0.208 | -2.03% / -2.12% | 9.210 |
| zgx / 717 | 31.076 ± 0.208 | -1.71% / -1.80% | 9.246 |
| zxyz / 720 | 31.589 ± 0.199 | -0.09% / -0.17% | 9.410 |
| baseline-last / 723 | 31.644 ± 0.208 | +0.09% / +0.00% | 9.426 |

Heavy: rows from 50 real seconds; pictures/s from the existing `runs.py` windows.

| Mode / job | Speed % (measured mean ± row SE) | Derived Δ vs opening / closing baseline | Pictures/s (measured) |
|---|---:|---:|---:|
| baseline-first / 709 | 22.637 ± 0.072 | +0.00% / -0.05% | 6.787 |
| gprinline / 712 | 22.765 ± 0.074 | +0.57% / +0.52% | 6.826 |
| zgpr / 715 | 22.197 ± 0.074 | -1.94% / -1.99% | 6.651 |
| zgx / 718 | 22.168 ± 0.074 | -2.07% / -2.12% | 6.640 |
| zxyz / 721 | 22.460 ± 0.073 | -0.78% / -0.83% | 6.730 |
| baseline-last / 724 | 22.648 ± 0.072 | +0.05% / +0.00% | 6.792 |

Title tp850: fixed windows 25-150 / 165-300 / 320-420 real seconds (`title_sections.py`); cell parentheses give derived change versus opening baseline. The later picture rate is measured.

| Mode / job | Cutscene speed % ± row SE (Δ) | Later speed % ± row SE (Δ) | Movie speed % ± row SE (Δ) | Later pictures/s |
|---|---:|---:|---:|---:|
| baseline-first / 710 | 36.796 ± 0.217 (+0.00%) | 23.604 ± 0.704 (+0.00%) | 13.403 ± 0.169 (+0.00%) | 7.70 |
| gprinline / 713 | 37.031 ± 0.220 (+0.64%) | 23.510 ± 0.712 (-0.40%) | 13.378 ± 0.174 (-0.19%) | 7.74 |
| zgpr / 716 | 36.027 ± 0.223 (-2.09%) | 23.723 ± 0.688 (+0.50%) | 13.158 ± 0.169 (-1.83%) | 7.58 |
| zgx / 719 | 36.329 ± 0.223 (-1.27%) | 23.579 ± 0.707 (-0.11%) | 13.168 ± 0.170 (-1.75%) | 7.62 |
| zxyz / 722 | 36.636 ± 0.226 (-0.44%) | 23.504 ± 0.703 (-0.42%) | 13.365 ± 0.173 (-0.28%) | 7.67 |
| baseline-last / 725 | 36.794 ± 0.221 (-0.00%) | 23.521 ± 0.717 (-0.35%) | 13.400 ± 0.175 (-0.02%) | 7.71 |

Retained code in the heavy area (measured last reported window; full-buffer flush count, not icbi count):

| Mode / job | Buffer % | Hot / exit / cold words per guest instruction | Full-buffer flushes | Reported native successes / refusals |
|---|---:|---:|---:|---:|
| baseline-first / 709 | 43 | 2.2 / 1.0 / 1.3 | 0 | n/a |
| gprinline / 712 | 47 | 2.2 / 1.1 / 1.4 | 0 | n/a |
| zgpr / 715 | 47 | 2.3 / 1.1 / 1.4 | 0 | 11,894,698 / 0 |
| zgx / 718 | 46 | 2.2 / 1.1 / 1.4 | 0 | 6,719,546 / 0 |
| zxyz / 721 | 44 | 2.2 / 1.0 / 1.3 | 0 | 3,248,823 / 0 |
| baseline-last / 724 | 43 | 2.2 / 1.0 / 1.3 | 0 | n/a |

Native counts sum the emitted 10-second reports after 50 real seconds, excluding startup and the unreported tail; near-budget fallbacks do not enter C and do not increment runtime refusal counts. Compiled-word ratios count copied callee instructions at each inline site; buffer occupancy also shows code growth. The companion JSON retains per-run sample counts, row errors, sparse PMU samples, option masks and frame-gap histograms.


**Verdicts (derived from the measured whole runs above).** Word inlining is positive in both gameplay scenes
(+0.80% / +0.57%, Ordon / heavy versus opening controls); the closing controls retain the same sign.
The earlier +1.5-2.5% core-0 estimate was larger than the realised gameplay gain. Native GPR (C20) does not turn
positive (-2.03% / -1.94%); native GX setters (C21) do not turn positive (-1.71% / -2.07%); cXyz does not turn
positive (-0.09% / -0.78%, essentially even in Ordon and a loss in the heavy area). Closing-control comparisons
are reported separately in the tables. None of the three tested native sets establishes a gameplay gain.

X3's native-call hypothesis is disproved for these sets with the implemented generic argument-block ABI.
Automatic replacement of small SDK functions stays closed; native replacement remains focused on large subtrees.
The word-inline GPR path is a small positive result for leaf inlining. All four new switches remain off by default.
Other calling conventions, automatic translation of additional functions and other games remain unmeasured.
No X4 or additional optimisation was built.

**Limits.** These are separate fixed-duration whole runs on TP, with two control runs per scene. Row standard
errors describe within-run variation, not independent-run uncertainty. Gameplay uses SD-card states; all PC states
are title scenes. The title's fixed real-time windows can sample different virtual progress: native GPR's later
speed mean rises slightly while its picture rate falls, so that mean alone does not establish a gain.
Gameplay retains the same observed gap kinds (mostly two VI fields per picture);
the counts and actual pictures/s are retained in the evidence. Per-call diagnostic counters are included in
the measured cost. Code-size and sparse PMU samples do not isolate argument packing, budget guards or fetch costs.
The initial older-build baseline jobs 705-707 are retained in the JSON and excluded from final comparisons because
the memory-guard review changed the binary. No game images, states or commercial instruction dumps are included.


Reproduce software checks from the repository root (WSL Debian, disc read in place):

```bash
export GC3DS_ISO='/mnt/c/Users/tyler/TP3DS/game/Legend of Zelda, The - Twilight Princess (USA).nkit.iso'
make -C core
bash core/tests/cpu/run_tests.sh
python3 core/tools/ps_fast.py --check
# Must fail: python3 core/tools/ps_fast.py --check --plant
X3_OUT=/root/x3/repeat-matrix bash core/tests/jit/run_x3_matrix.sh
bash core/tools/arm_build.sh
python3 core/tests/jit/x3_game.py --out /root/x3/repeat-game --jobs 3
python3 core/tests/cpu/check_x3_hle.py --out /root/x3/repeat-hle
bash core/host3ds/build.sh
```

The matrix directory must be new: its shared binary is compiled once, then reused for all four masks/seeds.
The runner now refuses a pre-existing binary; a planted stale-binary sentinel was rejected with exit 2 before any
check started. This wrapper guard was added after the final matrix, which had used a fresh directory; the test
commands and tested JIT binary are unchanged.
`x3_game.py` runs `GC3DS_OPT=0 GC3DS_JIT_NOTRACE=1 GC3DS_JIT_MB=16 GC3DS_X3=BITS` under
`qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7`, with `--jit --load-state /root/tp_arm_S.state
--frames END --out DIR`: S:END = 950:970 and 850:854, BITS = 0,1,2,4,8 independently. It compares the
`JIT: N instructions run` line and SHA-256 of `last.png`, then plants a different count and frame signature.
`check_x3_hle.py` runs `HLE_FAST=7 HLE_CHECK=1 GC3DS_NODRAW=1` on PC states 4290:4490, 5000:5200,
900:1100 and 6590:6790; it adds 5000:5020 with `HLE_STOP_EVERY=13` and 5000:5002 with
`HLE_FAST_PLANT=1`. These are all title scenes. No PC gameplay check is claimed.

Hardware reproduction: all four new options and `--psgx off --psxyz off` explicitly set; switch on only the
named experimental option per treatment, keep the other three off. Queue with `bash core/tools/relay.sh queue
--game Twilight --state sdmc:/gc3ds/states/STATE.state --seconds SECONDS OPTIONS`. STATE/SECONDS are
tp_ordon/180, tp_area2/150, tp850/420. Ordon also uses the exact square walk in REMOTE_RUNS.md (40-159 up,
160-279 right, 280-399 down, 400-519 left, each offset by 480 repeatedly through 2919). Fetch with
`bash core/tools/relay.sh log JOB`; score gameplay with `python3 core/tools/runs.py core/out/relay_JOB.csv`
and title with `python3 core/tools/title_sections.py core/out/relay_JOB.csv`. No within-run A/Bs or pooled A/B
means decide these verdicts. Reported row standard errors describe within-run variation, not uncertainty across
independent whole runs. Title sections are 25-150, 165-300 and 320-420 real seconds; gameplay drops the first 50 s.

**Evidence and exact hardware replay:** [count-only results and job arguments](x3/2026-10-20.json) retain
software signatures, each hardware job's exact options, and per-run statistics. No game bytes, states, images or
commercial disassemblies are included. After building, this requeues the complete opening/control/treatment order:

```bash
python3 - <<'PY'
import json, subprocess
for run in json.load(open('docs/research/x3/2026-10-20.json'))['device_jobs']:
    subprocess.run(['bash', 'core/tools/relay.sh', 'queue', *run['args']], check=True)
PY
```

### X4: Hand-compiled function regions on ARM11 (2026-10-20)

**Result: supports a targeted region tier; no compiler built.** The hottest ordinary JIT function selected from
Ordon, J2DPane::draw, clears X4's >=1.5x gate at **1.724 +- 0.027x** after subtracting the host-entry control.
GXLoadPosMtxImm is **2.507 +- 0.064x**; GXLoadTexObjPreLoaded is **1.028 +- 0.043x**, below the <1.2x floor criterion
for this integer path. These are **derived ratios of measured device counts, jobs 745-747**, not gameplay gains.
The SDK oracles use a conservative C pipe sink; the texture result does not prove an absolute lower bound for every
possible pipe lowering. The pane result contains no pipe writes or remaining external calls.

**Selection (measured profile, job 631):** `hot_funcs.py` maps 16 hot-block reports to TP USA symbols. Its listed
blocks cover 72.7% of core-0 samples; shares below are means of the truncated reports, not exhaustive coverage.
The two largest entries, drawSelf (4.51%) and concat-view matrix load (4.29%), are native ports. The flush-primitive
entry (1.81%) is now a default stand-in; the shadow scan is also intercepted. The shared save/restore tails
(1.49% / 1.03%) were X3. Excluding those and unmapped PCs leaves these complete ordinary guest functions:

| Function | Guest entry | Job 631 self samples | Final run | Reference guest instructions/call | Recorded write words | FIFO bytes |
|---|---|---|---|---|---|---|
| J2DPane::draw | 802F67E0 | 3.08% | 745 | 373 | 96 | 0 |
| GXLoadTexObjPreLoaded | 8035E298 | 0.90% | 746 | 82 | 16 | 30 |
| GXLoadPosMtxImm | 8036024C | 0.86% | 747 | 20 | 0 | 53 |

The last four columns are **measured from real Ordon calls on the 2DS**. This is gameplay-state evidence, unlike
X1's PC title scenes. The first visible pane with no children is captured; its two context-copy loops execute, while
its child walk takes the empty branch. The texture call is non-TLUT. These are individual real calls, not a census of
all calls or all paths. Input-register hashes (measured): `66a5255c9e8236a9`, `dfbcd5a955135b2c`,
`1d86ba170574dd97`, respectively. Registers and guest memory remain privately on the SD as
`sdmc:/gc3ds/x4_input_1.state` through `_3.state`; no state, memory dump or game asset is committed.

**What was built:** `--region-bench 1|2|3`, default off, arms the existing memo-entry mechanism, halts at the real
call, unwinds the running JIT, then runs the experiment in `host3ds/jitbench.c` / `regionbench.inc`. It does not install
an optimization. `region_oracle.S` is handwritten ARM for the entire functions, including the pane's branches,
copy loops and child loop. Guest r27-r31 and r1 stay in host r6-r11; f30/f31 stay in d14/d15. Dead CR0 compares use
ARM flags. The reached leaves are inlined (including the empty virtual methods, GPR helpers, matrix copy/concat and
cull setter); non-leaf/cold call sites retain a checked call bridge to today's JIT. All final captured calls use
zero such bridges. Guest memory accesses have no range checks. The matrix leaf holds its input singles in s0-s23,
uses d12-d15 for paired independent calculations, spills only the two live caller doubles, and preserves the guest's
single-rounding boundaries and nonvolatile paired lanes. Dead result-to-double-to-single round trips before stores
are omitted. This is fixed code for the hashed TP binary, not a translator or recognizer for general games.

**Timing and controls:** normal JIT defaults; standard MEM1 mappings, interrupts masked, GX drained, private empty
FIFO sink. The JIT and oracle emit the same byte stream into that sink. A zero `jit_run` return is handled exactly as
`gekko_run` handles it: interpret one instruction. The JIT consumes the reference's instruction count and reaches
its return PC. Each version has seven batches of 256 calls for each PMU pair (instructions, then I/D read misses),
with state/write-set reset before each call. Those batch sizes are measurement settings. Every interval's last
output is checked; all final runs report **zero timed compiles (measured)**. Reset-only, empty JIT (`blr`) and empty
ARM controls are measured in the same run. The empty controls have their own equality check and planted return-PC
failure. We average the two cycle intervals per repeat and report mean +- SE across seven repeats, without selecting
minimum cycles or pooling different builds.

The following are **derived per-call body costs from measured PMU counts**, subtracting each version's matching
empty-call control (which includes reset, entry and return). This removes the C dispatch that a normally linked JIT
call need not pay. It is an estimate of linked body cost, not a direct timing of a linked full-game region.

| Run / function | Version | Cycles/call body | ARM instructions/call body | Raw PMU I-misses/call |
|---|---|---|---|---|
| 745 / pane | JIT | 6,559 +- 79 | 2,565 +- 5 | 62.51 +- 4.32 |
| 745 / pane | Region oracle | 3,806 +- 15 | 1,409 +- 3 | 5.04 +- 1.22 |
| 746 / texture | JIT | 1,167 +- 45 | 550 +- 5 | 15.23 +- 1.57 |
| 746 / texture | Region oracle | 1,137 +- 24 | 776 +- 3 | 0.24 +- 0.08 |
| 747 / position matrix | JIT | 3,578 +- 38 | 1,918 +- 5 | 23.35 +- 4.01 |
| 747 / position matrix | Region oracle | 1,431 +- 25 | 977 +- 7 | 0.61 +- 0.41 |

I-misses are raw measured PMU counts including wrappers. Cache interference is not additive: some empty-control
subtractions give negative miss differences. Those differences remain in the JSON audit and are not interpreted as
negative physical misses. Cycle/instruction columns above are the derived body costs.

For completeness, **derived full-call costs after reset-only subtraction** (including host entry/return):

| Run | JIT cycles/call | Oracle cycles/call | Ratio |
|---|---|---|---|
| 745 | 8,336 +- 71 | 3,870 +- 28 | 2.155 +- 0.026x |
| 746 | 2,802 +- 18 | 1,251 +- 12 | 2.241 +- 0.019x |
| 747 | 5,143 +- 36 | 1,555 +- 11 | 3.309 +- 0.038x |

The entry correction matters: job 739's earlier spilled matrix oracle read 1.532x from full calls but only 1.255x
from bodies. Its final allocation/scheduling and dead-FPR lowering clear the gate in 745. The integer setter's
2.241x full-call ratio becomes only 1.028x after that correction; it cannot be used to justify a broad tier gain.

**Today's emitted JIT code is captured too (measured successful writes):** `_1.bin`: 38 records / 1,212 ARM words /
5,304 file bytes; `_2.bin`: 2 / 291 / 1,188; `_3.bin`: 9 / 345 / 1,488, under `sdmc:/gc3ds/x4_jit`. Each record is
little-endian `(guest PC, host PC, word count)` followed by the emitted hot/exit words, before linking. The records
include reached blocks and recompilations, not unexecuted paths, shared stubs or the separate cold arena. The matching
ELF is retained by the relay under build `39e7764efefb1bed` (full SHA256 in the evidence JSON). The dump reader can
disassemble these outside the repository. Static oracle function sizes are **derived from that ELF's symbols**:
pane 7,396 bytes, texture 1,096, position 492; these include cold branches and are not dynamic instruction counts.

**Exactness and plants (measured, 745-747):** both versions match the interpreter for caller-visible memory, FIFO,
return PC/stack, nonvolatile GPRs, both lanes of nonvolatile FPRs and CR2-CR4. A fingerprint of all MEM1 after undo
also checks for writes absent from the recorded set. Value comparisons omit only the known callee frames (derived
from the prologues/captured path: 336 / 64 / 0 bytes), not an arbitrary span below SP. A planted extra write below
the stack, outside those frames, proves it is still covered. PC, GPR, FPR, CR, FIFO length/content, recorded-memory and
extra-write plants are applied where relevant and reversed before timing; every applied difference is caught. This
uses the owner-approved dead-volatile boundary contract, as the port checker does. It is not a claim of identical
volatile registers, dead stack bytes, guest timing or a full architectural state hash.

Supplemental run **X4-oracle-strict-final**: six synthetic qemu-arm calls pass (position, texture, pane early exit, root,
parent and nonempty child walk), with six planted register differences caught. **X4-cpu**: warning-free core builds,
420 directed / 12,800 differential cases pass, planted bugs give 313 / 181 mismatches, 11,806 disassembly cases pass.
**X4-score**: known ratio 1.75 is reproduced and four invalid evidence plants rejected. The dump reader decodes an
open ARM example and rejects a truncated-record plant. PC and device builds complete; the device build adds no new
warnings (the existing devkitARM crtn executable-stack warning remains). The production JIT is unchanged, so no new
whole-game JIT/fuzzer speed-path check or gameplay speed run is claimed.

**Audit of preliminary runs:** 726/729 had no valid timing: the harness initially treated a zero JIT return as an
error; 726's old plant flag is not valid evidence because its base comparison failed. 727/728 were reset-only
preliminaries; 730-732 expanded leaves but still used the call bridge / reset-only controls. 733-738 added full-memory
and then empty-entry controls. The fingerprint rejected 734/737: the recorder's lazy `rhost` map omitted the first
LR save on a cold page. X4 now primes standard data mappings before recording; the general recorder is unchanged.
739-741 validate that correction and retain the earlier scalar/spilled matrix oracle. Every run's raw measurement
lines, final summaries and roles are retained in [the evidence JSON](x4/2026-10-20.json); rejected runs are not included
in the verdict. 742-744 add the final allocation and checked dump I/O but still exclude an overly broad span
below SP in the fingerprint. Final review narrowed value exceptions to the actual frames and removed the fingerprint exclusion
entirely; 745-747 repeat the experiment with that stricter guard and a planted write in the formerly excluded span.

**Limits and build decision:** this is a warm, isolated benchmark with one real input per function. It omits event
polls, exception/fault recovery, arbitrary BATs, compilation, inter-core cache pressure and whole-game code layout.
Unexecuted branches and broader class/TLUT input distributions have not been validated. No range-check removal is
safe for production merely because valid captured inputs passed. The pane's gain requires allocation, leaf inlining
and flags together; this does not validate every intermediate compiler stage. The SDK pipe lowering is conservative.

**Derived conditional arithmetic**, using job 631's selected self-time weights: the three bodies average **1.609x**
within their **4.84%** listed subset. Improving only that subset implies about **+1.87% core 0**, not +15-30%.
At the same average gain, +15-30% core 0 would require approximately **35-61% coverage**. That coverage and a gameplay
speedup remain unmeasured. X4 therefore supports building a targeted region tier (architecture build item 4), with
code size and event/fault correctness still gates. It does not justify replacing every integer SDK leaf, changing a
default, or claiming the architecture plan's whole-core estimate as a measured result. Stop at this oracle experiment.

Exact reproduction (WSL; captured inputs/code stay outside git):

```bash
bash core/host3ds/build.sh
# First capture from the real Ordon state; repeat for N=1,2,3.
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/states/tp_ordon.state --seconds 90 --region-bench 1
# The final runs replayed the saved entries:
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/x4_input_1.state --seconds 90 --region-bench 1
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/x4_input_2.state --seconds 90 --region-bench 2
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/x4_input_3.state --seconds 90 --region-bench 3
bash core/tools/relay.sh log 745
bash core/tools/relay.sh log 746
bash core/tools/relay.sh log 747
python3 core/tools/region_score.py core/out/relay_745.csv core/out/relay_746.csv core/out/relay_747.csv
python3 core/tools/region_score.py --self-test
python3 core/tests/region/run_check.py /root/hm_cut/mem.bin
NO_BENCH=1 bash core/tests/cpu/run_tests.sh
# After a private SD copy; the destination must be outside the repository:
python3 core/tests/region/dump_arm.py /private/x4_jit_1.bin /private/x4-arm
```


### X2: L2 lockdown by master on the New 2DS XL (2026-10-20)

**Hardware gate:** James explicitly confirmed he was home and able to restart before job 748 was queued. This is
measurement-only host code, off by default; no region compiler, GX optimisation or production default was built.

**Manual read first:** [ARM DDI 0246H, L2C-310 r3p3](https://documentation-service.arm.com/static/5e8e1df188295d1e18d365bb),
section 2.3.6 (pp. 2-18..21), section 3.3.4 (auxiliary control) and section 3.3.11 (pp. 3-27..31,
tables 3-17 and 3-20..35). Document SHA-256 `7905475a394e1e4fc042423f321f6eaa30721566b1e831a539e719b8eea2aea6`.
The master feature is optional. Selection uses AXI `AyUSER[7:5]`, not an architectural CPU number; the manual's
Cortex-A9 example is not proof of Nintendo's wiring. A set bit prevents allocation into a way; hits remain shared.
Auxiliary bit 16 selects 8/16 ways, bit 26 controls non-secure lockdown writes. Auxiliary control cannot be changed
while the cache is enabled and is never written here. Register offsets from mirror VA `0x97E10000`:

| Purpose | Offset(s), configuration rather than timing measurements |
|---|---|
| Cache ID / control / auxiliary control | `0x000` / `0x100` / `0x104` (read only in this experiment) |
| Data / instruction lockdown, selector m = 0..7 | `0x900 + 8*m` / `0x904 + 8*m`, every word through `0x93C` snapshotted |
| Modified D/I pairs, selectors 1..3 | `0x908, 0x90C, 0x910, 0x914, 0x918, 0x91C` |

**Probe:** `--probe-l2lock only` bypasses guest boot and the full timing/GPU suites. The decoder/JIT are unchanged.
It reuses timingprobe.c's random 1 MB dependent chase and the core-2 4 MB store streamer, same allocation and linked
cycle throughout, without cache-maintenance tricks. Each phase has three alone/streaming trials; each trial reports
the minimum cycle count over the existing nine PMU-pair measurements after adaptive warm-up. The minimum and PMU
events are from separate measurements, so events cannot be multiplied by that minimum as a synchronous sample.
All sixteen lockdown words plus auxiliary/control are verified after restoration. The six attempted stores are
marked before issuing them; delivered MMIO aborts enter cleanup, which continues across failures and retries once.
Every handled exit restores; a kernel hang, power loss or undelivered abort cannot execute user cleanup. No such
failure occurred in job 748. A failed restoration stops the relay and refuses further jobs until the 2DS is power-cycled.

**Measured, job 748, build `74f6f566d0f9a763`:** ID `410000c9`, control `00000001`, auxiliary `32890000` (16-way,
NS-lockdown bit clear). All sixteen lockdown words started zero. Each selector-1..3 D/I word accepted and read back
`00005555`, prohibiting allocation into the even-numbered eight ways; selector 0 and selectors 4..7 stayed zero.
All sixteen returned to zero; auxiliary/control equality verified. These writes worked despite the NS permission
bit being clear; this does not establish the security tagging of the mirror accesses.

| Phase, job 748 | Alone, measured cycles/load (three trials) | Core 2 streams, measured cycles/load | Derived streaming mean +- SE |
|---|---|---|---|
| Original registers | 30.332, 30.372, 30.442 | 121.147, 122.675, 124.398 | 122.740 +- 0.939 |
| Half-way allocation for selectors 1..3 | 30.442, 30.403, 30.313 | 32.469, 33.460, 32.989 | 32.973 +- 0.286 |
| Original registers restored | 30.346, 30.387, 30.373 | 123.941, 122.062, 122.223 | 122.742 +- 0.601 |

**Chase verdict: confirmed**, inside the requested 30-40-cycle band. Derived speedup under streaming is **3.722x**;
the reversed effect after restoration supports allocation/eviction coupling, rather than an unavoidable bus-only
limit. The combined selector setting demonstrably constrains the core-2 traffic; individual core-to-selector wiring
for cores 1/3 is not established. Raw samples and derived arithmetic: [chase evidence](x2/2026-10-20-chase.json).

**Conditional whole runs:** `--l2lock half` uses the same transaction only for X2 scoring. An outer wrapper restores on
all returns, keeping perf.csv open until cleanup finishes. `--l2lock plant` forces a handled error immediately after
successful application, so restoration is exercised on a failing run. Defaults stay untouched. Whole-run results
are recorded below; the synthetic chase alone does not decide their speed verdict.

**Heavy gameplay, measured runs 750-753, final scoring build `86342a5619fa6a8a`:** 180 s each from
`tp_area2.state`, no presses, scored from 50 s with runs.py. The numbers below are **derived means and ratios** of
those runs; core-0 speed is each row's speed divided by `(1 - gxwait_pct/100)`. The half-only run is the helper
control, so partitioning's own effect is not credited to a helper. No pooled A/B mean is used.

| Run | Mode | Mean speed % | Mean core-0 speed % | Speed vs half control | Core 0 vs half control |
|---|---|---|---|---|---|
| 750 | baseline | 22.738 | 22.862 | -0.23% | -0.52% |
| 751 | half | 22.789 | 22.983 | +0.00% | +0.00% |
| 752 | half-vw2 | 22.411 | 22.502 | -1.66% | -2.09% |
| 753 | half-steal | 22.749 | 22.822 | -0.18% | -0.70% |

Measured frame counters scored by runs.py give derived rates **6.815 / 6.835 / 6.718 / 6.821 pictures/s** in that
order. Almost all pictures retain two VI fields. Half-only versus the unpartitioned control is +0.23% speed and
+0.53% core 0 (derived); this small effect is not an established production gain. **VW2 still loses; stealing has no
demonstrated gain.** Each mode has one run, so the tiny stealing difference is not a significance claim.
Current unpartitioned helper-on variants were not re-run; smaller penalties than historical jobs cannot be
causally credited to partitioning alone. Title
re-scores and their limits follow below.

**Title scoring correction, chosen from control 754 before helper results:** the standard title_sections.py output
is retained, but its real-time 165-300 s window mixes geometry with the following movie in these runs. Control
754 already reports nonzero THP IDCT calls at real_s 252.3 (measured); its draw rate is down to 44 draws/s at 253.3,
versus 72,998 at 211.1. A setting that shifts this transition changes the mix in a fixed host-time mean.
For the X2 decision, l2lock_score.py therefore compares the **same guest-clock intervals** (`emu_s`): 25-60 s
inside the cutscene and 75-92 s inside later geometry. The control reaches them at derived real-time intervals
35.925-129.1725 s and 174.390625-230.9 s. These windows were fixed before the vw2/steal title results.
Speed is derived as `100 * guest-clock span / host duration`; core-0 speed divides by host duration with the
recorded GX-wait shares removed. Boundary times are linearly interpolated from rounded CSV clocks; core-0 figures
are profile-based estimates. Very small differences are not significance claims. No complete standard 320-420 s
movie window or Ordon walk is measured by this X2 batch. Whole-run input is unchanged; only scoring selects content.

**Title, measured runs 754-757, build `86342a5619fa6a8a`, 310 s each:** derived matching-guest-clock scores below.
Changes are relative to half-only run 755, not to unpartitioned run 754. Core-0 values are derived profile estimates.

| Run | Mode | Cutscene speed / core 0 % | Later geometry speed / core 0 % | Speed change cut / later | Core-0 change cut / later |
|---|---|---|---|---|---|
| 754 | baseline | 37.535 / 38.011 | 30.084 / 30.112 | -1.11% / -0.84% | -1.05% / -0.78% |
| 755 | half | 37.954 / 38.413 | 30.337 / 30.349 | +0.00% / +0.00% | +0.00% / +0.00% |
| 756 | half-vw2 | 37.582 / 38.191 | 30.137 / 30.152 | -0.98% / -0.66% | -0.58% / -0.65% |
| 757 | half-steal | 37.914 / 38.386 | 30.371 / 30.383 | -0.11% / +0.11% | -0.07% / +0.11% |

For traceability, title_sections.py's fixed host-time cutscene/later means are respectively **36.89/23.65,
37.34/23.57, 36.89/23.64 and 37.27/23.58%** (derived, runs 754-757). Its mixed-window later pictures/s are
**7.77, 7.82, 7.76, 7.83**. Its movie field prints zero with zero rows because the runs end before 320 s; that zero
is not a measured movie result. In particular the mixed window makes vw2 look +0.33% later, while matched geometry
is -0.66%. Sampled GX-wait shares can exceed 100% during the movie; per-row core-0 divisions there are invalid and
are excluded from the decision. Both selected geometry windows have valid shares.

**Build decision:** the allocation/eviction mechanism is **confirmed**, clearing architecture 4.3.4's hardware gate.
Partitioning alone gives derived +0.23% heavy / +1.12% title cutscene / +0.84% later geometry in these single runs;
this is not enough evidence to promote a kernel-adjacent default. **VW2 does not turn positive** in any tested scene.
**Stealing is neutral in the title and not a demonstrated gain in gameplay**; its old multi-percent title penalty is
absent at this resolution, but that is not a speed win. Keep all three switches off. Section 6 build item 7 still
needs a repeatable whole-workload gain before a larger parallel pipeline is justified. The synthetic result rules
out an unavoidable bus-only explanation for that chase; it does not remove bus pressure or prove that the game's
hot code/data fit in the protected share. No optimisation beyond the requested experimental register setting is built.

Raw register evidence, derived scores, exact arguments and observed switch flags are in
[the whole-run evidence](x2/2026-10-20-runs.json). All eight speed runs use one build. Final safety hardening and
its separate checks are recorded below; they are not pooled into these performance comparisons.

**Final safety checks, measured jobs 762-763, build `62a2adc4447aee00`:** after the eight speed runs, review
hardened the cold error paths: guarded MMIO restores the main crash recorder, L2 experiments disable automatic
fatal-crash relaunch, and a failed register restoration stops the relay before it can restart into another job.
Job 762 deliberately returns a handled error after applying the partition; relay status is failed as intended,
while all sixteen lockdown words, auxiliary/control and the report checker verify restoration. Job 749 exercised
the earlier version of that same handled-error path. No deliberately fatal crash is tested or claimed recoverable.
Jobs 758-761 were cancelled before execution while this hardening was completed; they contribute no measurements.
The final 3DS build adds no warnings; both hardware checks use the final source. The speed comparisons remain
on their single earlier build, without pooling builds or treating error-path hardening as a measured speed gain.

Repeat chase job 763 gives measured streaming samples **122.279/123.229/128.213** original,
**33.185/33.215/33.416** partitioned and **123.219/122.053/124.391** restored cycles/load. Derived means +- SE are
**124.574 +- 1.840 -> 33.272 +- 0.073 -> 123.221 +- 0.675**, a derived **3.744x** chase speedup. Alone means are
30.368 / 30.402 / 30.365 cycles/load (derived). Readbacks and restoration verify again, confirming job 748's verdict
on the hardened final build. Full samples and both handled-error records are in the whole-run evidence JSON.

**Local validation, run X2-transaction:** 150 synthetic register cases passed, including every snapshot/apply/restore
I/O failure in 8/16-way geometries, stores that reach hardware before reporting failure, ignored writes and aliases.
Four planted source bugs (mask polarity, missing readback, lost failed-store cleanup, skipped restoration writes)
all fail. **Run X2-report:** the normal, unavailable and no-change synthetic reports pass their expected verdicts; seven invalid
report plants are rejected (partition, restoration, missing trial, truncation, duplicate snapshot and two whole-run
cleanup failures). The guest-clock scorer reproduces 50% speed / 62.5% core-0 speed on a known synthetic trace;
three invalid-clock/wait plants are rejected and a planted wait change fails the known-value comparison.
Plants never deliberately corrupt hardware. The 3DS build passes with no new warnings: the
existing main.c truncation, timingprobe.c k_ldr_ind uninitialised-variable and crtn executable-stack warnings remain;
compiling the parent main/timingprobe sources independently reproduces their three/four diagnostics exactly;
the warning comparison rejects an injected new diagnostic. No JIT/interpreter edits or
unrun emulation exactness checks are claimed.

Reproduce in WSL Debian (hardware commands only while James confirms he can restart):

```sh
python3 core/tests/l2lock/run_check.py
python3 core/tools/l2lock_score.py --self-test
bash core/host3ds/build.sh
bash core/tools/relay.sh queue --game Twilight --probe-l2lock only --seconds 60
bash core/tools/relay.sh log JOB
python3 core/tools/l2lock_score.py core/out/relay_JOB.csv
# Handled-error check; relay status must be failed, but the restore line must verify:
bash core/tools/relay.sh queue --game Twilight --seconds 10 --l2lock plant
python3 core/tools/l2lock_score.py --whole-run core/out/relay_JOB.csv
# Only after the chase succeeds; separate whole runs, never A/B pooling:
for scene in tp_area2:180 tp850:310; do
    state=${scene%:*}; seconds=${scene#*:}
    for mode in baseline half vw2 steal; do
        extra=()
        case "$mode" in
            half) extra=(--l2lock half);;
            vw2) extra=(--l2lock half --vw2 on);;
            steal) extra=(--l2lock half --steal on);;
        esac
        bash core/tools/relay.sh queue --game Twilight --state "sdmc:/gc3ds/states/$state.state" \
            --seconds "$seconds" "${extra[@]}"
    done
done
# Fetch each log, validate every partitioned run, then score:
python3 core/tools/l2lock_score.py --whole-run core/out/relay_JOB.csv
python3 core/tools/runs.py core/out/relay_{750,751,752,753}.csv
python3 core/tools/title_sections.py core/out/relay_{754,755,756,757}.csv
for job in 754 755 756 757; do
    python3 core/tools/l2lock_score.py --title-clock "core/out/relay_$job.csv"
done
```

### Second games on the 2DS: Mario Party 4 and Wind Waker (X6, jobs 764-798, 2026-10-21/22)

- **What it took for Mario Party 4 (Hudson, early SDK, MusyX on AX) to run.** Three emulator defects, none specific to the
  game (commits 02bbb87, 8f0b992):
  1. **AX and CARD microcode:** an AX microcode (hash 4e8a8b21) got no DSP_INIT, so MusyX's salInitDsp waited forever.
     AX now answers DSP_INIT, acknowledges each command list with DSP_YIELD and takes the task mails (no mixing, as
     for Zelda); CARD answers DSP_DONE. The hashes cover Dolphin's AX list (Melee, TTYD, F-Zero GX, Monkey Ball,
     Star Fox, Mario Party 4 / 5, ...).
  2. **The GP link bit:** GXBeginDisplayList records a list through the pipe with CP control bit 4 cleared; gc3ds fed
     those bytes to the GP, which drew them with the wrong vertex format and desynced the stream. Bursts now reach the
     GP only while it is linked.
  3. **PE signals one interrupt at a time:** with the GX thread, a token and GXDrawDone's finish were applied together;
     the SDK's token handler writes back the control register it read (write-one-to-clear status bits) and cleared the
     finish too, so the game slept in GXDrawDone. Signals now wait while an enabled PE interrupt is pending
     (`gx_pe_event_must_wait`). Found with GXDEFER=1, a PC mode with the 3DS's asynchronous ordering (pc_main.c),
     which reproduced it exactly.
- **Wind Waker (JSystem 2002, Zelda microcode)** ran on the 2DS without changes: title, file select, the intro scroll,
  Outset Island and gameplay render; Link's cel shading looks off in close-ups (not investigated). Neither game sees a
  memory card; both continue without saving.
- **Gameplay states** (made on the 2DS with relay presses): `sdmc:/gc3ds/states/ww_outset.state` (Outset lookout, HUD
  up) and `sdmc:/gc3ds/states/mp4_board.state` (Toad's Midway Madness, four players).
- **Measured, all defaults, separate whole runs (runs.py):**

  | Scene (job) | Speed | Game frames / s | Core 0 running | GX thread working | Largest families (hot_funcs.py --families) |
  |---|---|---|---|---|---|
  | Wind Waker, Outset (792) | 18.55% | 5.56 (30 fps game) | 94.5% | ~37% | game 48% (actor search 21%: cTgIt_JudgeFilter 10.9, cNdIt_Judge 7.2, fpcSch_JudgeByID 3.1), J3D 12.8, GX SDK 11.9, vector / matrix 6.6, collision 3.7 |
  | Mario Party 4, board (794) | 12.08% | 7.24 (60 fps game) | 86.5% (+10% waiting on GX) | ~76% | game 43, **vector / matrix 27.9** (PSMTXROMultVecArray 10.2, PSMTXConcat 9.7), GX SDK 11.8 |
  | Wind Waker, title and intro (772) | 29.6% | 8.9 | | | |

  Wind Waker has one wall (core 0), Mario Party 4 two (as TP). TP's actor search does not show in its profiles.
- **Automatic library replacement for a second SDK generation:** Mario Party 4's PSMTX functions differ in their bytes
  from TP's, so TP's stand-ins did not match. The same generator (ps_translate.py) run on Mario Party 4's copies adds
  PSMTXROMultVecArray (direct mode: a loop), PSMTXConcat, PSMTXMultVecArray, PSMTXRotTrig, PSMTXIdentity, PSMTXTrans,
  PSMTXScale and PSMTXReorder; TP's 36 come out byte-identical. PC HLE_CHECK (Mario Party 4 from boot, 2,400 fields):
  56,806 calls, 0 differed. **2DS, board: 12.08 -> 13.92% speed (+15.2%), 7.24 -> 8.32 frames a second (job 795 against
  794); vector / matrix 27.9 -> 6.6% of core 0.** TP unchanged (heavy area 22.80%, Ordon 32.02%, jobs 797-798) and
  Wind Waker unchanged (18.46%, job 796: its PSMTXConcat is a third version). Wind Waker's own copies (PSMTXConcat,
  Identity, RotTrig, Scale, Trans) added next: PC HLE_CHECK from boot, 2,400 fields: 19,533,871 calls, 0 differed;
  **2DS, Outset: 18.50% (mean of 792, 796) -> 18.87% (+2.0%, job 799); vector / matrix 6.6 -> 3.5%.**
- **Open (exactness):** the PC HLE_CHECK of Mario Party 4 scripted to its board (`--press`, every recognised function
  exercised) reported, part way through:
  - 16 PSMTXROMultVecArray calls whose memory matched but whose FPR lanes differed (f13, a scratch register, and f17's
    ps1): a denormal natively against zero in the interpreter;
  - 4 PSVECCrossProduct calls (TP's stand-in, the same bytes in both games) that stored -0.0 where the interpreter
    stored +0.0.

  Both fit the PC host not flushing denormals, while the interpreter (FPSCR[NI]) and the 2DS's VFP (flush-to-zero)
  do. **Resolved:** with `HLE_FTZ=1` (hle_ps.c: x86 FTZ and DAZ while native code runs, as the 2DS's VFP), the same
  scripted run to the board gives **65,389,934 calls, 0 differed** (231 stopped inside, handed to the guest). The
  7,599 differences of the run without it were all the PC host's denormals. Use `HLE_FTZ=1` with `HLE_CHECK=1`.
- **The estimate functions as stand-ins, measured and not kept (2026-10-22):** frsqrte / fres / fsel in the translator's
  direct mode (the interpreter's estimate and soft-float routines, frC rounded to 25 bits) made PSVECMag,
  PSVECNormalize (TP, Mario Party 4, Wind Waker) and PSMTXInvXpose (Mario Party 4) exact: PC HLE_FTZ + HLE_CHECK, TP
  tp_pc_900 -> 2100 8,572,382 calls, Wind Waker 20,927,253, Mario Party 4 73,973, 0 differed. (Before the soft-float
  arithmetic, C doubles differed in 1,858 / 3,005 calls: Gekko rounds a multiply's frC to 25 bits.) **But every scene
  was slower** (jobs 806-809 against 795, 799, 797, 798): Mario Party 4 13.92 -> 13.17%, Wind Waker 18.87 -> 18.75%, TP
  heavy area 22.80 -> 22.53%, Ordon 32.02 -> 31.83%. The soft-float calls on the ARM11 cost more than the JIT's code
  for these short functions. Removed from the table (49 stand-ins as committed in fc59ef8); the translator support
  stays.
- **Reproduce:** `--game Party --state sdmc:/gc3ds/states/mp4_board.state --seconds 180` (or `--game Wind --state
  sdmc:/gc3ds/states/ww_outset.state`), `python3 core/tools/runs.py core/out/relay_JOB.csv`, `python3
  core/tools/hot_funcs.py core/out/relay_JOB.csv SYMBOLS --families` with mariopartyrd/marioparty4 `config/GMPE01_01`
  or zeldaret/tww `config/GZLE01` symbols. Stand-in inputs: the functions' bytes from a PC `MEMDUMP` at their symbol
  addresses, through devkitPPC's objdump (as LIBRARY_PORTS.md describes for TP).

### Region tier stage 1: forward branches inside a block (`--region on`, OPT_REGION, jobs 818-833, 2026-10-22)

- **What (C33 stage 1, commits 20e9cb0 and the extent trim):** forward b / bc whose target lies inside the block's
  extent become labels; outward conditional branches are side exits; the register cache is written back and forgotten
  at each label. No registers cross a label yet, no loops, no profile.
- **Correctness:** region fuzz matrix (opt R, R|7, R|262144|7; main, nofault, chain; 20,000 cases each) 0 fails;
  plants 20-22 caught; standard suite 0 fails; qemu TP 900 fields from tp_arm_850: last frame identical,
  78,626,767 (off) against 78,626,757 / 78,626,775 (on) instructions (block-length timing drift).
- **First form (jobs 818-825):** an extent ran past calls and loop heads, so the return site and the loop head
  compiled the rest of the function again. Code buffer 58 -> 91% (Ordon; one full flush at ~110 s, compiles 25 ->
  103/s, JIT compile 0.8 -> 3.8% of core 0). Heavy area 21.31 -> 20.82% (-2.3%), Ordon 30.50 -> 27.89% (-8.6%),
  Wind Waker 18.78 -> 18.15% (-3.4%), Mario Party 4 13.87 -> 13.33% (-3.9%).
- **Trimmed form (jobs 826-833, separate whole runs, runs.py):** the extent ends at a call that leaves the block and
  before a backward branch's target inside it. Compile churn gone (Ordon 22.5 -> 19.1 compiles/s, no flush).

  | Scene | off | on | change | I-cache misses (k/s) off -> on | code buffer off -> on |
  |---|---|---|---|---|---|
  | TP heavy area (826/827) | 21.408 | 20.910 | **-2.3%** | 8,038 -> 8,787 (+9%) | 47 -> 62% |
  | TP Ordon walk (828/829) | 30.598 | 29.264 | **-4.4%** | 8,332 -> 9,110 (+9%) | 58 -> 74% |
  | Wind Waker, Outset (830/831) | 18.779 | 17.976 | **-4.3%** | 7,336 -> 8,289 (+13%) | 35 -> 45% |
  | Mario Party 4, board (832/833) | 13.874 | 13.513 | **-2.6%** | 8,429 -> 9,416 (+12%) | 15 -> 20% |

- **Why it loses:** a region compiles both sides of every internal branch, including paths that never run, and lays
  them inline in the hot code; lazy per-block compilation never compiled them. That shows up as I-cache misses in
  every scene. What stage 1 saves (a budget check and a linked jump per merged block) is small, because a label
  forgets the register cache. Branch mispredicts fell slightly (heavy 864 -> 847 k/s, Ordon 925 -> 872).
- **Verdict:** stays off. A region tier must be **profile-guided** (only executed edges, never-run sides moved to
  the cold region) and must **carry registers across labels** before its saving can exceed its layout cost.

### Region tier stage 2: tiered regions of executed code, registers across labels (`--tier on`, OPT_TIER, jobs 834-873, 2026-10-22)

- **What (commits 3b0ce88, 8a0e090):** blocks start in the block tier, whose blocks end at every branch, so the blocks
  that exist are the profile. After 64 runs a block that starts a region (return site, loop head, function entry)
  is replaced by its region over the code that ran. Labels keep the GPR slots of the first path in, and the other
  paths reconcile to them. Float lanes, flags and known constants are still dropped at labels.
- **Correctness every round:** region and tier fuzz (main, nofault, chain, pseudo-random coverage) 0 fails; plants
  20-22 caught; standard suite 0 fails; qemu TP fields 850-900: last frame identical, instruction count equal or 4 off
  (timing drift).
- **Separate whole runs, tier off -> on (runs.py), one pair per scene per round:**

  | Round (jobs) | Heavy area | Ordon walk | Wind Waker | Mario Party 4 |
  |---|---|---|---|---|
  | counter in each block, labels flush (834-841) | -1.3% | -3.9% | -4.0% | -2.4% |
  | dispatcher counts, registers across labels (842-849) | -1.1% | -2.8% | +0.0% | -1.0% |
  | one-condition joins inline: ADD<cond> + B<cond>, no cold stub (850-857) | -0.6% | -2.0% | +0.9% | -0.4% |
  | single-entry extents (858-865) | -0.6% | -1.7% | +0.0% | -0.4% |
  | run-weighted coverage (1/8 of the head), checks across labels (866-873) | -0.6% | -1.6% | +0.3% | -0.1% |
  | ... and regions in their own hot area, the top third of it (874-881, not kept) | -1.0% | -2.2% | +0.4% | -0.1% |
  | float lanes and known singles across labels, float lanes gated over the extent (882-889, commit 0e4fff4) | -0.3% | -1.3% | +0.6% | +0.1% |
  | ... and a tier compile with no forward branch stops before the page scan (890-893) | -0.3% | -1.1% | | |
  | ... and straight-line leaves inlined at bl inside regions (894-901) | -0.4% | -1.1% | +0.6% | -0.05% |
  | loops inside regions (back edges with a budget check; 902-909, commit aeb01d2) | -0.5% | -1.1% | +1.0% | -0.1% |
  | region plans: calls inlined (bl, guarded bctrl), three deep, everywhere (910-917) | -1.0% | -2.0% | -0.6% | -0.3% |
  | ... only in regions whose head function loops; profiles kept by call address; plans past mtlr / mtctr (936-947, b7eb9ac) | **-0.9%** | **+0.2%** | **+7.2%** | **+0.2%** |

- **Where it stands (866-873):** regions run 1.2-1.6% fewer ARM instructions (heavy 281.7 -> 278.4 M/s, Ordon 272.5
  -> 268.1). The loss is instruction fetch: stall_inst 201 -> 205 M cycles/s (heavy) and 219 -> 230 (Ordon),
  instruction micro-TLB misses +34% / +65%, I-cache misses +0.5% / +3.4%. Code buffer 47 -> 56% (heavy).
  Regions are compiled after the blocks around them, so hot code spreads over more pages and lines.
- **Leaves inlined (894-901):** 292 / 334 / 166 / 38 leaves (heavy, Ordon, Wind Waker, Mario Party 4), qemu TP 125;
  the body lengths spread from 0 to 10 instructions. Most leaves branch inside and are not taken, so it moves nothing
  measurable. 3-13 tier compiles failed (code overflow) in those runs; the block tier's block stays.
- **Wind Waker's actor search (C35) through the region tier (jobs 936-947):** speed 18.73 / 18.74 -> 20.10 / 20.09%
  (+7.2%), frames a second 5.61 / 5.62 -> 6.01 / 6.02, two fields a picture throughout. cNdIt_Judge's loop region
  holds cTgIt_JudgeFilter (behind a CTR check) and fpcSch_JudgeByID inside it (its beqlr a join); the three
  functions' share of core 0 went 21.7% -> 15.0% (now all sampled as cNdIt_Judge's region). What had kept it out:
  ends_block stops at every mtspr, so the plan stopped at the bctrl's mtctr and every callee at its epilogue's mtlr
  (found with --tier-watch, jobs 934-935); a callee whose own block had become a region lost its bctrl profile.
  The heavy area's -0.9% repeats (946/947).
- **Ordon's remaining loss is compile time:** JIT compile 0.84 -> 1.21% of core 0 (890/891), the regions' own
  compiles in a scene that keeps reaching new code; stopping the rejected attempts early barely moved it.
- **I-cache simulator, TP fields 900-950 after 850-900 (qemu, core/tools/qemu-plugins/icache.c, 16 KB 4-way):** JIT
  hot-region misses 14.35 M (block tier) -> 13.01 M (tiers), -9%, with 9% more accesses. The simulated L1 does not
  show the 2DS's fetch loss. What it leaves out is the micro-TLB and the L2 behind the L1 (regions add ~20% code: more
  instruction fetches go past L2). The off run's window also held a burst of compiles, so its C-code share is not
  comparable.
- **Regions apart from the block tier's code lose (874-881):** instruction micro-TLB misses 45 -> 129 k/s (heavy),
  67 -> 172 k/s (Ordon). A region keeps calling and linking into block-tier code (callees, its exits), so it is better
  next to the code compiled around the same time than next to other regions.
- **What did not pay:** a run counter inside each block (a branch in every block, and links into a replaced block went
  through its patched first word). Cold stubs for taken joins (two taken branches and a cold line per taken branch).
- **Verdict:** stays off. The saving the tier has without float lanes, flags, longer extents or inlining (about
  1.5% of ARM instructions) is spent on code layout. Next levers in the same tier: hot regions placed together, and
  more work removed per region (float lanes and CR flags across labels, extents past 64 instructions, leaf and
  devirtualised calls inlined). Grouping regions apart was measured and loses (row above).

### Region tier nursery: pending blocks in a recycled area (`--tier-nursery`, jobs 1285-1298, 2026-10-24)

- **What (commit 7332fb9 and the fixes after it):** C33's TP loss is instruction fetch over the blocks regions replace,
  which stay between live code. With the nursery, a block still pending (nothing links to it) compiles into the top
  quarter of the hot and cold areas, recycled whole when full; a block decided plain is compiled again into the main
  area (settled), regions go there directly, and a pending block that ran 4+ times by a recycle is tenured (compiled
  into the main area from then on, still pending). Retired entries go on a free list (a pool full of them flushed the
  cache).
- **Checks:** JIT fuzzer, every mode and plant, 0 fails, with a nursery run (835 settled) and a nursery just over one
  block's room (874 recycles); TP under qemu with fault mode off (as on the 2DS; the nursery is off with fault mode)
  850 -> 900, tier on: nursery off / on 78,626,764 instructions both, same frame, same state hash; a 16K-word nursery
  (1,837 recycles) the same frame, 8 instructions off (timing). The first device round (1285-1294) hung TP and Wind
  Waker: a deoptimised decided block was compiled again into the nursery while its links reached it through a patched
  branch (fixed: a decided block's recompile stays decided); the qemu check had missed it because qemu ran with fault
  mode, which turns the nursery off and check hoisting (the deopts) with it.
- **Device, nursery off -> on (1295-1298), from 50 s:**

  | Scene | Speed | Fetch stall cycles (M/s) | I-cache misses (k/s) | JIT compile (% of core 0) |
  |---|---|---|---|---|
  | TP heavy | 22.290 -> 22.105% (-0.8%) | 220.3 -> 211.6 | 8,434 -> 8,141 | 1.39 -> 2.08 |
  | TP Ordon walk | 31.390 -> 30.840% (-1.8%) | 234.2 -> 226.4 | 8,594 -> 8,449 | 1.77 -> 2.64 |

- **Reading:** the layout works: instruction-fetch stalls fall ~4% (~1% of core 0, about the tier's TP loss), I-cache
  misses ~3%. The cost is compiling twice: 10,000-17,000 blocks settled or tenured per run (heavy 10,246 settled +
  5,760 tenured), and compile time stays 0.7-0.9% of core 0 higher after warm-up. Off. Keeping the gain needs the
  decided block moved, not compiled again (relocation records for every branch leaving a block: hot / cold, stubs,
  linked exits).
- **Moved instead of compiled again (commit b9ba75c, `reloc_move`):** every data word in block code goes through
  `lit()`, a nursery block carries a relocation record, and a decided block's words are copied to the main area with
  each branch re-aimed. Exact: fuzzer 0 fails (835 moved; plant 29, branches out of a moved block left unaimed,
  detected), the PC / qemu builds trap on a moved block's old copy, TP under qemu 850 -> 900 identical with 6,791 and
  5,146 blocks moved. (The final version with a 16,384- or 20,000-word nursery ends 8 guest instructions early at
  fields 900, 905 and 912 alike, with the same last frame and interpreted count, moves on or off
  (`GC3DS_NURSERYMOVE=0`): `--frames` stops at the next block exit, and its regions differ there. A run that stops
  at another instruction is not a divergence by itself; one that does not come back identical at a later field is.)
  Device (separate whole runs; from 50 s / **from 100 s**, the steady state: compile time is 2x for
  the first 40 s with the nursery and back to baseline by 100 s):

  | Version (jobs) | TP heavy | TP Ordon walk |
  |---|---|---|
  | compiled again (1295-1298) | -0.8% / **+0.2%** | -1.8% / **-2.4%** |
  | moved, warm pending blocks moved at each recycle (1299-1308) | -2.4% / -1.9% | -3.5% / -4.1% |
  | moved when decided, warm pending blocks compiled again if they run again (1309-1314) | -0.5% / **+0.2%** | -1.2% / **-1.9%** |

  Moving everything warm at a recycle filled the main area with blocks that never ran again (fetch stalls 221 -> 228
  M cycles/s); moving only decided blocks keeps the layout gain (stalls 220 -> 215) but the walk keeps reaching new
  code, so recycles retire live blocks that are compiled again. **Verdict: the nursery does not pay in either form**
  (heavy area even, Ordon -2%). Off; the relocation stays as a tool (`--nursery-move`).

### The J3D light port with a one-copy commit (`--port-hooks 55`, jobs 1315-1322, 2026-10-24)

- **What:** `J3DColorBlockLightOn::diffLightObj`'s port (bit 4, built 10-19 and measured neutral, 6.338 / 6.344 against
  6.349 / 6.344 frames a second) committed its display-list bytes to guest RAM with a `ram_wr8` per byte (8.7 us a
  call, as long as building them). `port_commit` now copies them with one `ram_from_be`.
- **Exact:** PC, TP 900 -> 1100, 9,348 light calls taken: the old byte-loop commit and the new copy end on the same
  state hash (7111abdb2510a43e at cycle 8922970066) and last frame; `PORT_CHECK=1` with the hook on, 9,348 checked,
  0 differed (check mode compares the port's bytes directly, so the hash comparison is what covers the commit).
- **Separate whole runs, interleaved, scored from 100 s (`runs.py --skip 100`), hooks 39 (default) against 55:**

  | Scene | speed, hooks 39 | speed, hooks 55 | frames a second 39 -> 55 |
  |---|---|---|---|
  | TP heavy area (1315/1317 against 1316/1318) | 22.244 / 22.245% | 22.853 / 22.895% (**+2.7%**) | 6.667 -> 6.853 (+2.8%) |
  | TP Ordon walk (1319/1321 against 1320/1322) | 31.394 / 31.388% | 31.354 / 31.375% (-0.1%) | 9.309 -> 9.410 (+1.1%) |

  Pairs agree to 0.02-0.2%. The heavy area wins because it has the lights (`J3DMaterial::diff` is 9.1% of its core 0,
  Ordon's 5.5%); Ordon's speed is even (its fields stay locked to two a picture), its real frames a second read +1.1%.
- **On by default since 2026-10-24** (James; `--port check` on the 2DS with it on, job 1323: 0 differed). The whole of `J3DMaterial::diff` then followed
  by the same method (next section: +3.0% in both scenes).

### J3DMaterial::diff as one native port (`--port-hooks` bit 6, 119 with the defaults; jobs 1324-1347, 2026-10-24)

- **What (commit 023e0c5, `port_j2d.c` `j3d_material_diff`):** the material's display-list diff (9.1% of the heavy area's
  core 0 inclusive, Ordon's 5.5%: lights 4.4 / 2.5, TEV 1.6 / 1.0, PE fog 1.1 / 0.6, endDiff 0.8 / 0.5, texgen 0.6 /
  0.5) in one context: beginDL (the buffer swap, interrupts off, GDInitGDLObj), every block's `diff` with their GD
  writers, endDL (pad to 32, the length, `__GDCurrentDL` cleared), the bytes copied once, then DCFlushRange's dcbf per
  line through the bus (page stamps) and its `sc` with the OS handler run by the interpreter (as `jit_sc_handler`
  does). Fog follows J3DGDSetFog's single arithmetic (plain singles only), texture numbers loadTexNo's LOD floats
  through `fctiwz`.
- **Census first (`--port-hooks 119` with a counting-only hook, jobs 1324-1327):** one class per block in both scenes:
  J3DColorBlockLightOn, J3DTexGenBlockPatched, J3DTevBlock2 / 4, J3DIndBlockFull, J3DPEBlockFull; flags used: ambient
  colour (94% of calls), 8 lights (94%), TEV registers and fog (all), texture numbers (16-25%), texture matrices and
  texgens (17%, 1.5% in Ordon), gen mode (17-38%), colour channels (5-12%); never TEV stages, indirect stages, texcoord
  scale or blend. The post-texture-matrix flag varies between calls.
- **Exact:** PC `PORT_CHECK`, TP 900 -> 1100: 10,000 calls compared, 0 differed (98% taken: 200 calls of another block
  class give up); `PORT_CHECK=plant` reports every checked call. 2DS `--port check` (jobs 1338-1339): heavy area 2,899
  and Ordon 1,347 calls per 10 s compared, 0 differed, all taken (1-2 with the FPU off). The two bugs check mode found
  on the way: the texgen count is a u32 (its low byte, not the first), and check mode's interpreter stopped at
  OSDisableInterrupts' `mtmsr` (`gekko_run_call` now runs through mtmsr / sc / rfi for port checks).
- **Separate whole runs, interleaved, from 100 s (hooks 55 = all defaults, 119 = with the port):**

  | Version (jobs) | Heavy area | Ordon walk |
  |---|---|---|
  | without texture numbers and texgens, 67-72% taken (1330-1337) | 22.90 -> 23.21% (+1.35%) | 31.42 -> 31.94% (+1.65%) |
  | **all (1340-1347)** | **22.885 / 22.896 -> 23.573 / 23.571% (+3.0%)**, frames 6.871 -> 7.094 | **31.456 / 31.456 -> 32.440 / 32.349% (+3.0%)**, frames 9.415 -> 9.647 |

- **Its own cost now (job 1341):** 18.9 us building, 9.0 us committing a call (~870 calls a second in the heavy area,
  ~2.4% of core 0); the commit is mostly DCFlushRange's lines and the interpreted `sc` handler.

### J3DSys::reinitGX as one native port (`--port-hooks` bit 7, 247 with the defaults; jobs 1355-1370, 2026-10-24)

- **What (commit 9444269):** the GX state J3D resets to before each pass, 5.3% of Ordon's core 0 inclusive (heavy area
  4.0%), ~8,400 guest instructions and ~1,705 pipe bytes a call: reinitGenMode / Lighting / Transform / Texture /
  TevStages / IndStages / PixelProc, then GXFlush (__GXSetDirtyState, 32 zero bytes, PPCSync). Every argument is a
  constant; fifteen GX functions were added to the ports, GXInitTexObj builds the null texture in the guest's stack
  frame through the store log (not committed: dead once the call returns), and PPCSync's `sc` runs after the commit
  with the OS handler (`port_syscall`, which DCFlushRange's now shares).
- **A memo first (`--memo 0x8031073C`, jobs 1355-1360):** every call missed: the SDK's read-modify-writes carry the
  previous draws' `__GXData` (8.8-9.3 words differ a call) into the pipe bytes. The probe needed two fixes to say so:
  it now follows `sc` / `rfi` (GXFlush) and keeps its 1.1 MB table in linear memory on the 3DS (calloc failed there,
  so every call had been recorded and none compared).
- **Exact:** PC `PORT_CHECK`, TP 900 -> 1100, all ports on: 2,626 calls, 0 differed; plant: every call differs. 2DS
  `--port check` (jobs 1361-1362): 519 (heavy) and 500 (Ordon) calls per 10 s, 0 differed. Check mode found GXSetFog's
  f31: `fdivs` writes both lanes and the epilogue's `lfd` restores ps0 only, so ps1 keeps C.
- **Separate whole runs, interleaved, from 100 s (hooks 119 against 247):** heavy area 23.577 / 23.585 -> 23.569 /
  23.585% (even); **Ordon 32.288 / 32.362 -> 33.591 / 33.561% (+3.9%), frames 9.607 -> 10.049 (+4.6%)**. On by default.

### The minimap's renderingDecoration as a port (`--port-hooks` bit 8, 503 with the defaults; jobs 1381-1404, 2026-10-24)

- **What (commit 3426f0a):** dRenderingFDAmap_c::renderingDecoration (4.4% of the heavy area's core 0 inclusive) draws a
  map line's border point by point, each point a line and a point with the TEV set again around them (~16,400 guest
  instructions and ~9 KB of pipe bytes a call in the heavy area, ~7,300 and 3.5 KB in Ordon). The GX work is native;
  its two virtual leaves (line width and colour: the stage's display type, the visited rooms, the map's settings) run
  in the interpreter on the side (`port_guest_call`).
- **Exact:** 2DS `--port check`: Ordon 240 calls per 10 s (job 1382), heavy area 160 and then 239 with the larger buffer
  (jobs 1383-1384), 0 differed. The first heavy-area check crashed the 2DS (job 1381, it restarted itself): every
  GXBegin after a BP write also sends __GXSendFlushPrim's strip, and long lines overran the 8 KB pipe buffer, which the
  writers do not bound. Now a room check per point, the pane walk's 64 KB buffer, and the bytes streamed once the
  first point went through (no give-up can follow it).
- **Separate whole runs, from 100 s (hooks 247 against 503):**

  | Version (jobs) | Heavy area | Ordon walk |
  |---|---|---|
  | first (1385-1392) | 23.49 -> 23.41% (-0.4%) | 33.37 -> 33.97% (+1.8%) |
  | timing scale 115 (1393-1398) | 23.51 -> 23.37% (-0.6%) | 33.33 -> 34.11% (+2.3%) |
  | **streamed (1399-1404)** | **23.566 -> 23.475% (-0.4%)** | **33.13 -> 34.16% (+3.1%)** |

  In the heavy area core 0 waits for the GX thread 2.2 -> 4.0% of its time with the port, streamed or not: the
  minimap's thousands of tiny line and point draws make the GX thread the limit there, so a faster core 0 only waits.
  On by default (net gain, largest in the walk); the heavy area's -0.4% is a GX-thread cost (the decoration's draws),
  which is where to look next for it.

### J3DShapeMtxMultiConcatView::load as one port call (`--port-hooks` bit 9, off: Ordon -2.9%; jobs 1405-1414, 2026-10-24)

- **What (commit 5a98cf5):** a skinned shape's matrices in one call instead of the PNGP port hooked once per matrix
  (~7,400 calls a second in Ordon at 4.4 + 2.9 us each). Exact: PC 10,036 calls and 2DS (jobs 1405-1406, ~2,600
  calls per 10 s) 0 differed; plant caught.
- **Separate whole runs, from 100 s (hooks 503 against 1015):** heavy area 23.41 -> 23.42% (even); **Ordon 34.16 ->
  33.17% (-2.9%)**, frames 10.22 -> 9.94.
- **Why it loses although the ports' own time fell:** Ordon's port time went from ~53 ms/s (PNGP 7,357 calls at 7.26
  us) to ~43 ms/s (1,051 multi-loads at 36.3 us plus 519 PNGP calls), but time in the dispatcher rose 6.1 -> 7.4% of
  core 0 and core 0 waited for the GX thread 2.3 -> 3.0%. Cause not established: the dispatcher's share probably
  counts the hooks' own C code too (dispatches a second were unchanged, ~12,000), and the region tier no longer
  inlining load is one candidate. Not chased further; off.
- **Lesson:** a port whose own time falls can still lose; only whole runs decide.

### Core 0's budget, every part accounted for (job 1490's settled window, TP heavy area, 2026-10-30)

| where core 0's wall time goes | share |
|---|---|
| **in compiled code** (C helpers called from blocks included) | **89.6%** |
| - of which inside port C code (`port_cost.py`, job 1533) | 20.2% |
| - JIT block code **or** stand-in C code: no instrument split these | 69.4% |
| waiting for the GX thread to answer (`gxsync`) | 3.4% |
| outside compiled code | 7.1% |
| - the dispatcher bucket (mostly the idle hook's GX wait, not dispatch overhead) | 5.9% |
| - the measurement's own reports (profiled runs only) | 1.7% |
| - events (VI, DI, DSP) 0.8%, the gekko_run loop 0.4%, compiling 0.2%, the rest 0.2% | 1.6% |
| **total** | **100.1%** |

Read with the PMU over the same scene: **instruction-fetch stalls are 27% of core 0's cycles** (218 of 804 M a
second) and data stalls ~22% (job 184). So about half of core 0 is memory stalls, and ~9% is waiting for the GX side
(`gxsync` plus the idle hook inside the dispatcher bucket) - which is the ceiling on what moving work to cores 2 and 3
could ever buy in this scene.

**The one blind spot: the 69.4%.** The ports have had their own timers since job 553, so their 20.2% is known, but
nothing separates the JIT's block code from the stand-ins' C code. We know what the stand-ins are *worth* (the
paired-single library: Mario Party 4 +15.6%, TP Ordon +3.8%) and not what they cost - and the two questions have
different answers, as the ports showed when two of eight turned out to cost more than they saved. `hle_run` now times
itself the way the ports do (one call in 16, nothing when the profile clock is off) and perf.csv reports
`# stand-in cost 10s`; the next profiled run splits that 69.4% for the first time.

### The blind spot closed: core 0 is 81% JIT block code, and half of it is memory stalls (job 1569, 2026-10-30)

Job 1569, the first profiled run with the stand-in timer: TP heavy area, 240 s, build `103456ecbdfe9264` (today's
defaults), scored from 80 s. The three instruments now cover core 0 with nothing left over.

| where core 0's wall time goes | share | from |
|---|---|---|
| running guest code | **91.7%** | `# joint 10s`, the `run/*` states |
| - of which **JIT block code and the C helpers blocks call** | **80.9%** | the remainder, now that the other two are measured |
| - inside port C code | 9.65% | `port_cost.py` (calls x microseconds a call, one call in 16 timed) |
| - inside stand-in C code (`hle_tp.c`) | 1.14% | `# stand-in cost 10s`, 36-39 calls/s at ~300 us |
| waiting for the GX thread (`gxsync`) | 0.6% | `# joint 10s` |
| not running guest code (dispatcher, events, reports, compiling) | 7.4% | `# joint 10s`, the `other/*` states |

The stand-ins' own cost is the new number, and it is **small: 1.14% of core 0**, against a worth of +15.6% in Mario
Party 4 and +3.8% in TP Ordon for the paired-single library alone. That asymmetry is the opposite of the ports',
where a 40-instruction leaf cost more than it saved. The ports' 9.65% splits 7.05% in `port_run` and 2.61% in
`port_commit`; `port_run` is the actual work for a subtree port such as the pane walk, so only the commit is
overhead - **`port_cost.py`'s "building the context" label is accurate for a leaf port and misleading for a subtree
one**, and HANDOFF's "building their context 7-8% of core 0" should be read that way.

**The PMU over the same window** (means of the 16 sampled rows from 80 s; core 0 is busy at 803.06 M cycles/s of 804):

| counter | per second | share of core 0 |
|---|---|---|
| instructions retired | 268.58 M | IPC **0.334** |
| instruction-fetch stalls | 220.17 M cycles | **27.4%** |
| data stalls | 177.51 M cycles | **22.1%** |
| L1 I-cache misses | 8.20 M | 26.9 cycles each, i.e. ~L2 latency (29): fetch stalls *are* L1 I misses served by L2 |
| L1 D read / write misses | 1.61 M / 0.645 M | |
| core 0's L2 misses (SCU) | 1.83 M | at the measured 126-170 ns FCRAM latency that is 185-250 M cycles before overlap, so FCRAM accounts for the data stalls |
| data micro-TLB / main TLB misses | 3.09 M / 0.191 M | |
| guest instructions | 37.98 M | **7.07 host instructions and 1.37 host data reads per guest instruction** |
| dispatcher entries (`disp_k`) | 8,010 | **one per 4,742 guest instructions** |

So **half of core 0 is memory stalls**, almost exactly split between fetch (L2) and data (FCRAM), and the JIT retires
one instruction every three cycles.

**Why no layout change fixes the fetch half.** The footprint line over the same window: 50% of sampled time in
compiled code is **116 blocks / 54.8 KB of host code spanning 84 distinct 4 KB pages** (16% density), 80% in 885
blocks / 290 KB / 289 pages, with 28,058 blocks live. Against a 16 KB L1 the 50% working set is 3.4x too big *at any
density*, which is why the nursery's packing cut fetch stalls only 4% (220 -> 212 M cycles/s = 1% of core 0) and
still lost on its compile cost. The sparsity is not mainly the exit stubs either, though they are 1.2 of every 3.6
hot-arena words and, at one dispatcher entry per 4,742 guest instructions, are traversed on the order of **0.1% of
block executions** - dead code interleaved between live bodies (C44).

**What that leaves, and why it is not a 10% lever.** Fetch follows the hot code's size, so the only lever is *fewer
host words per guest instruction*, and the exchange rate is measured: OPT_RETSTUB's -2.1% hot words bought
+1.0-1.6% of core 0 (job 148), so +10% of core 0 would need roughly -15 to -20% of hot words. The executed-words
table (2026-10-12) seems to offer two items that big, and **reading the emitter shows both are already-worked
ground**:

- **The branch sequences (`bc` 16.0% of hot words at 12.2 each, `b` 4.7%, `bclr` 3.3% = 24%) are not an exit-code
  problem.** `compile_branch` calls `rc_flush` **once**, before the branch, so both sides of a `bc` share one
  writeback; with a single condition the conditional branch itself becomes the taken side's link site
  (`condSite` in `emit_branch_cond`) and `exit_const` is a **single** patched word. So a `bc`'s 12.2 words are
  almost all the block's register-cache writeback, attributed to the instruction that carries it - the
  block-boundary cost under another name, and handing entry state over at linked exits was modelled at
  **2.2-2.4% of executed hot words, ~1.3% net** (HANDOFF 2026-10-12). Nothing here is 15%.
- **The paired-single accesses (`psq_l` 8.3%, `psq_st` 7.4% = 16%) have been attacked twice**: OPT_FCACHE took
  modelled `psq_st` 39.0 -> 30.1 cycles and is on by default (C11), and OPT_PIPEPS (psq_st straight to the pipe)
  measured -2.3% inlined and no gain inside the stubs (C27). jitbench's `psloop` at the device mask now reports
  `psq_l` 19.5 words / `psq_st` 22.0 words, i.e. the post-FCACHE figure, with **72.8% of its stall cycles in the
  VFP itself** - ARM11 VFPv2 latency, not code we emit.

Every peephole tried on the rest came in at 0.3-2% of hot words (C13). **So there is no identified 15-20% hot-word
reduction, and core 0's codegen is not where a 10% gain is**: the remaining upside is in removing guest code
wholesale (the library ports as a *set* are +10.6% while their parts sum to ~3.7%) and in the structural items still
open (C40's ~1.6% in every game, fault mode's +2-5%). This is why gains now arrive in 0.5-3% increments.

### The four defaults of 2026-10-30 verified together, and they are sub-additive (jobs 1570-1587)

One build (`103456ecbdfe9264`), one session, 240 s runs scored from 80 s, frames a second. "Old" is
`--fdirect off --port-hooks 503 --nskeep off --psflush on`, i.e. the four changes banked that day reverted together;
"new" is plain defaults.

| scene | old | new | change | within-arm spread |
|---|---|---|---|---|
| Mario Party 4 board | 9.023 | 9.238 | **+2.38%** | one run an arm (second pair queued) |
| Wind Waker Outset | 6.079 | 6.223 | **+2.37%** | one run an arm (second pair queued) |
| TP static Ordon | 10.482 / 10.481 | 10.519 / 10.520 | **+0.36%** | 0.01% both arms |
| TP heavy area | 7.233 / 7.231 | 7.215 / 7.222 | **-0.19%** | 0.03% / 0.10% |

The two TP arms agree to 0.01-0.10% between their own runs, so both TP figures are real: a small gain in Ordon and
a small **loss** in the heavy area.

**They do not add up.** The four parts were measured separately as `--port-hooks 243` (Ordon +0.71%, heavy +0.55%),
`--psflush off` (Ordon +0.33%, heavy +0.31%), OPT_FDIRECT (Wind Waker +2.6%, Mario Party 4 +0.7%, TP flat) and
`--nskeep` (Mario Party 4 +1.2%), which predicts **+1.04% in Ordon and +0.86% in the heavy area**. Measured together
they give +0.36% and -0.19% - **sub-additive by 0.7 and 1.05 points**, and the wrong sign where it matters. That is
the opposite of the library ports, which are superadditive as a set (+10.6% against ~3.7% for the parts), and it
means at least one of the four is worse in TP than its own measurement said. A one-at-a-time bisect on the heavy
area (jobs 1596-1603, each arm two runs against the new-defaults runs above) is queued to find which; OPT_FDIRECT is
the first suspect, since "TP flat" was the weakest of the four claims and it grows hot code in float-dense paths.

**Method note that cost an hour: a null result needs its arms checked, not just its spread.** The `--hlegd on|off`
separate runs (jobs 1573, 1578-1583) came back agreeing to 0.01% in Ordon and 0.05% in the heavy area, which reads
as a clean "worth nothing". `# hle 10s` reports **`J3DGD runs 0`** in every one of those runs: the J3D display-list
writers never fire in a TP gameplay scene, both arms ran the same code, and the numbers measure only the device's
repeatability. The stand-in was adopted on the title cutscene at ~40,000 calls a second (jobs 264-265), so tp850
decides it (jobs 1588-1591; 1588, hlegd off: cutscene 40.90, later 22.98, movie 14.78). Same family of error as
C41 - the arms were not what they looked like - by a different mechanism.

### The "within-session drift" was the screenshot stealing core 0 (jobs 1574-1612, 2026-10-30; **resolved**)

**The "two runs an arm agree to ~0.1%" rule held only while a batch was short.** Over ~3 hours of continuous
running on 2026-10-30 the console's output drifted ~2.5-3% *within one session on one build*, which is 25x the
assumed noise floor and large enough to swamp every result the day was chasing.

| arm | first run | later run | within-arm spread |
|---|---|---|---|
| Mario Party 4, old defaults | 9.023 | 8.800 | **2.5%** |
| Mario Party 4, new defaults | 9.238 | 8.998 | **2.7%** |
| Wind Waker, old defaults | 6.079 | 5.923 | **2.6%** |
| TP heavy area, old defaults (both early) | 7.233 | 7.231 | 0.03% |
| TP heavy area, new defaults (both early) | 7.215 | 7.222 | 0.10% |

**What it is not:** not the scene diverging (draws and vertices a picture agree to 0.1% between the runs of every
arm, and the emulated windows match - this is not C41), and not downclocking (the measured clock is 851-852 MHz in
every job). **What moves with it:** `cpu_pct`, core 0's own share, falls by about as much as the frame rate -
Mario Party 4 old 99.03 -> 96.14, new 99.72 -> 96.54, Wind Waker old 100.81 -> 97.86. So core 0 is getting less
done per real second for the same work. The cause is not identified; the apparatus (a perf.csv opened in append
mode and grown all session, the relay stream, heap state across state loads) is the first place to look.

**Consequences, applied to the same day's results:**
- **A paired difference survives; an absolute level does not.** Mario Party 4 reads +2.38% early (9.238 / 9.023)
  and +2.25% late (8.998 / 8.800) - the arms drifted together, so the comparison holds. The default set's
  **+2.3% in Mario Party 4 therefore stands.**
- ~~Any comparison across batches below ~3% is void~~ **- withdrawn: re-scored with the filter, the bisect is
  sound and reproduces across two independent batches.** See the bisect's own section below.
- **The heavy area's -0.19% for the set is the one cross-arm figure that survives**, because both its pairs
  (1570/1584 old, 1571/1585 new) ran early and each agreed internally to 0.03% and 0.10%.

**Found, an hour later: there is no drift.** Three *identical* reference runs (jobs 1608, 1610, 1612) read 7.074 /
7.188 / 7.226 frames a second - a 2.1% spread - with everything scaling together (speed, fields, draws, vertices,
instructions all 2.4% apart) while the proportions were identical (`rest_pct` 98.5%, `jitcode_pct` 58.2%). That is
a uniform throughput change, and `pmu_mcyc` named it: core 0's own cycle rate was **785.2 / 798.5 / 803.0 M/s**
against 804, with individual rows as low as **116 M/s**. Core 0 was not getting the whole second.

**It is the program's own screenshot.** Every run has exactly five short rows, at 33 s intervals (105, 138, 171,
204, 237 s), and `main.c` took a screenshot every 30 perf rows - the `# shot` lines land at 104.2, 137.0, 169.8,
202.8, 235.7 s. Writing the BMP to the card, and shipping the frame to the relay on a relay job, steals core 0 for
part of that second: **between 4% and 86% of it**, and how many of the five land on the severe end varies run to
run. Four severe steals in 147 rows is 2.4% of the mean, which is the whole effect.

**Scored without those seconds, the three references agree to 0.07%** (frames a second 7.213 / 7.211 / 7.216,
speed 24.109 / 24.091 / 24.093) - the noise floor is better than the ~0.1% this file has always claimed, and
**nothing about the console or the session was ever drifting**.

- **The fix in the scorer (retroactive, every log on disk).** `core/tools/runs.py` drops a row where core 0 got less
  than half the second, judged against **that run's own median** rate, and drops a frame window containing a
  screenshot (the pictures it lost are the measurement's own). `--keep-stolen` restores the old behaviour, and
  `--selftest` plants three faults. **An absolute threshold is wrong and was nearly shipped:** at "below 800 M/s" it
  dropped *half* of a tp850 run, because the title streams THP data from disc and sits at 635-804 M/s continuously -
  that is the workload, not a steal, and filtering it keeps only the easiest seconds and flatters the build. The
  self-test now fails on exactly that mistake.
- **The fix in the program.** The periodic screenshot is **off by default** since 2026-10-30 (`--shots N` brings it
  back). The two warm-up shots stay: they land before the scored window.
- **How much of the record this touches.** Over the 136 logs on disk the stolen seconds depressed a run's mean by a
  **median of 0.00% and a mean of 0.26%**, but **17 logs lost 0.5-3.2%** - so most historical numbers are fine and
  any sub-1% result is at risk, with nothing in the log having flagged it. The affected runs cluster (1602-1607
  consecutively, which is what produced the "drift"), so an arm can be hit while its neighbour is clean.

**Method, from here:** score with `runs.py` (the filter is the default) and **quote the within-arm spread always** -
today it was the spread, not the difference, that gave this away, and the dropped-row note in the output is how a
reader sees it. Interleaving arms is still a reasonable habit in a long batch, but it was treating a symptom.

### The four defaults bisected on the heavy area, re-scored: two of them are losses there (jobs 1570-1612)

All re-scored with `runs.py`'s stolen-second filter, so every arm agrees internally to 0.01-0.22%.

**The set, two runs an arm:**

| scene | old defaults | new defaults | change |
|---|---|---|---|
| Mario Party 4 | 9.061 / 9.081 | 9.278 / 9.279 | **+2.29%** |
| Wind Waker | 6.084 / 6.078 | 6.241 / 6.236 | **+2.59%** |
| static Ordon | 10.501 / 10.500 | 10.535 / 10.539 | **+0.35%** |
| TP heavy area | 7.230 / 7.228 | 7.218 / 7.216 | **-0.17%** |

**One at a time on the heavy area** (reference: the new defaults, 7.217; batch A is two runs an arm, batch B
interleaves a reference before each arm, and the two agree):

| reverted | batch A (2 runs) | batch B (interleaved) | what the default is worth in this scene |
|---|---|---|---|
| `--fdirect off` | 7.2035 | 7.213 | +0.19% / **0.00%** - neutral here; it was banked on Wind Waker +2.6%, which stands |
| `--port-hooks 503` | 7.275 | 7.272 | **-0.80% / -0.81%: `--port-hooks 243` is a loss here**, two independent batches agreeing to 0.01 points, against a recorded +0.55% |
| `--psflush on` | 7.293 | 7.286 | **-1.05% / -0.87%: `--psflush off` is a loss here**, against a recorded +0.31% |
| `--nskeep off` | 7.189 | 7.200 | **+0.39% / +0.41%** (banked on Mario Party 4 +1.2%) |

Batch B's references ran before and after each arm (7.213, 7.211, 7.216, 7.230) and each arm is scored against its
neighbours, so the two batches are independent in both scheduling and scoring. They agree to 0.01-0.02 points on
three of the four arms.

So the set is **+2.3 to +2.6% in Mario Party 4 and Wind Waker and -0.17% in TP's heavy area because two of its four
members are losses there** - which is also why the parts do not sum to the whole (they predict -1.3% against -0.17%
measured, so an interaction remains, but the signs and the dominant terms are now consistent). Both reverted
figures were measured before the screenshot was understood, so **`--port-hooks` and `--psflush` are scene-dependent
trade-offs, not settled gains, and which way they should go is the owner's call** (rule 5). **Nothing has been
changed on the strength of this**: the bisect covers the heavy area only, and the set is +2.3 to +2.6% in Mario
Party 4 and Wind Waker without anyone knowing which of its members drives that. Jobs 1622-1631 take `--port-hooks`
and `--psflush` apart in those two games, two runs an arm with a reference, before either default moves.

### `--port-hooks` and `--psflush` across the four games: one is vindicated, the other is not (jobs 1622-1631, **provisional**)

Taking the two switches apart in Mario Party 4 and Wind Waker, since the heavy-area bisect found both to be losses
there while the set as a whole is +2.3 to +2.6% in those two games. **Provisional and not acted on:** the card filled
up mid-batch (see the entry below), so most arms are a single run and three logs came back truncated at 7-9 scored
windows instead of 10. Jobs 1625, 1627, 1629 and 1631 - the second run of each pair and Wind Waker's own reference -
never completed and were cancelled.

| what the default is worth | static Ordon | TP heavy area | Mario Party 4 | Wind Waker |
|---|---|---|---|---|
| `--port-hooks 243` (vs 503) | +0.71% | **-0.80%** | +0.18% | **+3.2%** |
| `--psflush off` (vs on) | +0.33% | **-0.96%** | **-0.44%** | ~0% |

- **`--port-hooks 243` is vindicated.** Wind Waker's +3.2% pays for the heavy-area loss several times over, so it is
  a real scene-dependent trade-off and keeping it is right. (References: Mario Party 4 9.278 / 9.279 / 9.259 against
  `--port-hooks 503` 9.273 / 9.238; Wind Waker 6.241 / 6.236 against 6.044.)
- **`--psflush off`: the provisional -0.44% in Mario Party 4 was noise from a truncated log, and the switch is
  simply neutral in both of those games.** Re-run properly on build `dbac5779b30d8a92`, two runs an arm with
  references on the same build (jobs 1644-1651): Mario Party 4 off 9.188 / 9.180 against on 9.187 / 9.187
  (**+0.03%**), Wind Waker off 6.212 / 6.215 against on 6.215 / 6.213 (**+0.01%**), within-arm spreads 0.00-0.09%.
  The single-run figure came from the log the full card truncated; this supersedes it.
  **So what remains against `--psflush off` is TP's heavy area alone** (-0.96%, two independent batches on the older
  build) against +0.33% in static Ordon from this morning's pre-filter scoring. Jobs 1652-1659 measure both TP
  scenes on `dbac5779b30d8a92`, two runs an arm, which is what a default should turn on. If Ordon is neutral too,
  reverting is unambiguous: a ~1% loss in the heaviest scene bought nowhere else.
- **Note the rebuild movement, and why fresh references were queued rather than reusing the morning's:** the same
  default settings read Mario Party 4 9.184 on `dbac5779b30d8a92` against 9.278 on `103456ecbdfe9264` (-1.0%) and
  Wind Waker 6.214 against 6.238 (-0.4%). That is the documented ~0.8% a rebuild moves a baseline, and it is larger
  than the effect being chased.

### What other projects on this hardware did, and what transfers (survey, 2026-10-30)

A survey of comparable projects, prompted by the owner asking how others made things playable. Sources are in the
list at the end of this section. Two things it settled, two it corroborated, and two candidates it produced.

**Where this hardware's frontier actually is.** On a New 3DS: **PS1 runs at full speed** (RetroArch's PCSX-ReARMed),
**N64 barely runs at all** (DaedalusX64: "lots of GFX errors as well as emulation issues regarding speed"), and
there is **no credible GameCube emulator** - the pages claiming one are SEO spam on staging-domain mirrors that
contradict each other and credit a nonexistent Dolphin port. A *native* port of an N64 game (SM64, from its
decompilation) holds 30 fps. So TP at 35% of full speed is past the frontier for emulation on this chip, one whole
console generation beyond where it stops working for everyone else.

**What does not transfer, with the reason:**
- **The PS1 win was an accuracy trade in the renderer**, not a CPU one: they swapped P.E.Op.S. for PCSX4ALL's Unai
  renderer, "less accurate, but much faster", worth 10-20 fps. Here the GPU is already only 5-15 ms a frame and core
  0 is 93-97% busy, so that lever is spent - and it was spent *exactly*, by mipmaps (+19% in the heavy area), with
  the inexact version (`--texdiv 4`, GPU 100.7 -> 28.1 ms) kept only as a diagnostic.
- **Dolphin's largest speed hack does not apply.** "Skip EFB Access from CPU" takes Super Mario Galaxy from ~30-75%
  to ~95-140% - a 2-3x swing - because the emulator otherwise stalls the GPU to read back pixels. gc3ds already
  decoupled that path (PE register reads that no longer wait for the GX thread, cutscene +3.2%) and core 0 now waits
  **0.6%** on the GX side, so there is nothing left there to buy.
- **Static recompilation's success is host headroom, not technique.** N64Recomp is literal and
  instruction-by-instruction, like `ps_translate.py`, needs symbol metadata that in practice comes from a
  decompilation, and needs a runtime; Hackaday calls it "quite similar to what an emulator does in real-time, just
  with the goal of creating a permanent copy". It wins on PC because the host dwarfs the guest. Here the host is
  804 MHz ARM11 against a 486 MHz Gekko with a weaker memory system - which is why AOT measured *slower* on the
  console, and why translator v3 and v4 both lost (jobs 320-327, 340-346).

**What it corroborated, which matters because it is independent of this project's methodology:**
- **Mupen64Plus's new dynarec documentation states the footprint rule outright:** optimizations that expand code
  "generally lose out overall... The increase in code size often outweighs the benefit of this." A different
  emulator, a different host, the same conclusion this project reached through traces (-5.1%), hot-block relayout
  (-7.6%), AOT (-31 to -36%), the region nursery and float lanes. That is strong evidence the rule is a property of
  constrained hosts and not an artifact of how gc3ds measures.
- **Mupen64Plus also names this project's block-boundary problem independently:** jumping into recompiled code "very
  often" needs "a stub of code that loads the registers required in the middle, and because those stubs can be
  rather large and used often, they displace lots of code in the host's instruction cache."
- **Register pressure here is worse than anywhere compared.** wasm-dolphin holds all 32 GPRs in locals for a block's
  life; Dolphin's x86-64 backend has 16 host registers; ARMv6K has 16 of which **5** (r4-r8) are free for the guest's
  32. The block-boundary flush is not a design choice that could be undone - it is the host.
- **Adaptive frame skipping is where judder comes from, in practice.** PPSSPP users report flicker and stutter with
  *auto* frameskip and some prefer a fixed value; its auto mode is bounded by a maximum skip. That is the failure
  mode `--gfskip-target`'s accumulator was built to avoid, and the device runs measured gap sd 10.2 ms on a 127.8 ms
  mean with no spikes (C9b), so the mitigation works - but it is the right thing to keep watching.

**Two candidates it produced:** **C45**, register-cache eviction by liveness lookahead rather than LRU (Mupen does
this with a ten-instruction lookahead; worth +0.8 to +2.9% of core 0 by this project's own eviction counters, and it
*shrinks* hot code), and **C46**, a return stack buffer for `blr`, which is parked because the rate of `blr` through
the indirect table is not instrumented and the two available estimates of it differ by eight times.

**Sources:** [PS1 full speed on New 3DS](https://www.libretro.com/index.php/retroarch-3ds-full-speed-ps1-now-possible-with-pcsx-rearmed-w-unai-renderer/) ·
[DaedalusX64 on New 3DS](https://gbatemp.net/threads/release-daedalusx64-n64-emulator.562195/) ·
[SM64 3DS port at 30 fps](https://www.videogameschronicle.com/news/mario-64-3ds-port/) ·
[N64Recomp](https://github.com/N64Recomp/N64Recomp) ·
[static recompilation (Hackaday)](https://hackaday.com/tag/binary-translation/) ·
[Mupen64Plus new dynarec](https://github.com/mupen64plus/mupen64plus-core/blob/master/doc/new_dynarec.mediawiki) ·
[common dynarec optimizations (emudev.org)](https://emudev.org/2021/02/01/Dynarec) ·
[V8 speculative Wasm optimizations](https://v8.dev/blog/wasm-speculative-optimizations) ·
[wasm-dolphin](https://github.com/Remenby31/wasm-dolphin) ·
[Dolphin EFB options](https://bugs.dolphin-emu.org/issues/7892)

### Ranking directions by emulated clock delivered per unit of core-0 wall time (job 1569, heavy area)

Frames a second is the emulated clock rate while the game stays locked to two fields a picture, so a direction's
value is **how much emulated clock it delivers for the core-0 wall time it costs**. Job 1569 measures both sides of
that ratio for the first time - the clock side from `clock_split.py`, the wall side from the budget above:

| the clock comes from | clock share | core-0 wall share | clock per wall point |
|---|---|---|---|
| poll fill (the guest spinning, fast-forwarded) | 66.61% | ~0 | free |
| **port C code** | 9.25% | 9.65% | **0.959** |
| stand-in C code | 0.70% | 1.14% | 0.614 |
| **JIT block code (guest code actually executed)** | 23.43% | 80.90% | **0.290** |
| | sums 99.99% | sums 91.69% (the `run/*` share) | |

So a port delivers emulated clock **3.3x** as cheaply as running the guest code it replaces, and a stand-in 2.1x.
That is the quantitative reason the library ports are worth +10.6% as a set, and the argument for ranking subtree
ports above JIT codegen: codegen improves the 0.290, and no 15-20% improvement to it exists (above), whereas a port
moves clock out of that bucket entirely.

**Use this to rank, never to predict.** Extrapolating it naively - moving the ports' 9.25% of the clock out of the
JIT bucket should free 22.25 wall points and give +32% - over-predicts the ports' **measured** set value of +10.6%
by three times. The model is wrong in a known way: the guest code a port replaces was not all in the JIT bucket,
because part of what a port's charge saves is absorbed by a smaller poll fill instead of by less real work (C41),
and a port's charge (`kInsScale`) is not the guest instruction count. Any port's worth still has to be measured on
a scene that takes no input.

**The stand-in timer cross-checks against the old counter.** Inside job 1569's heavy area, one window:
`# hle 10s` 357 calls and 105.1 ms inside per 10 s; `# stand-in cost 10s` 36 calls/s and 10.7 ms/s = 107 ms per
10 s. The two instruments agree to 2%, so the 1.14% of core 0 above is independently confirmed; window to window
they vary ~12%, which is the sampled profile clock's own spread and the reason to read several windows.

### The stand-ins audited the way the ports were, and two more record corrections (jobs 1553-1561, 2026-10-30)

- **Frames a second, separate 240 s runs scored from 80 s, build 0f3c3eac6e709b6a (its default mask, `--fdirect` off,
  so the baselines are static Ordon 10.400 / 10.406 and heavy area 7.172 / 7.174):**

  | group turned off | static Ordon | heavy area | that group is worth | on record |
  |---|---|---|---|---|
  | `--hle off` (hle_tp.c: the shadow-scan family and the rest) | 10.405 | 7.195 | **-0.3%** (a small loss in gameplay) | the title's lever (+14% there) |
  | `--ps off` (the paired-single library) | 10.026 | 7.097 | **+3.8% Ordon, +1.1% heavy** | the project's biggest stand-in, confirmed |
  | `--psflush off` (`__GXSendFlushPrim`) | 10.437 | 7.195 | **-0.3% in both** (a loss) | Ordon +0.8%, heavy +1.7% |

  So the paired-single library is the single most valuable native stand-in and keeps its reputation, while
  **`--psflush` is a small loss in both TP scenes** - its recorded gains came from the walk and from an A/B that
  recompiles. The hle_tp.c group costs ~0.3% in gameplay and earns its keep only in the title, so it is scene-
  dependent rather than wrong. `--ps off` and `--psflush off` are running on Wind Waker and Mario Party 4 (jobs
  1562-1565) before `--psflush` is changed, since these stand-ins act in every game.
- **Both stand-ins across all four games (jobs 1553-1565), frames a second against each game's baseline:**

  | | TP static Ordon | TP heavy | Wind Waker | Mario Party 4 |
  |---|---|---|---|---|
  | the paired-single library (`--ps`) | **+3.8%** | **+1.1%** | **+0.5%** | **+15.6%** |
  | `__GXSendFlushPrim` (`--psflush`) | -0.33% | -0.31% | -0.06% | -0.24% |

  The paired-single library is the single most valuable thing in the project after draw merging - Mario Party 4 runs
  8.987 frames a second with it and **7.773 without**. `__GXSendFlushPrim` is a loss in **every** game measured:
  **off by default since 2026-10-30**. It stands in for one guest store of a word, which is far too small to pay -
  the same rule that made the PNGP port a loss, since a stand-in is charged instruction fetch rather than work.
- **`--nskeep` on by default (Mario Party 4 +1.2%):** 9.011 -> **9.122** frames a second (job 1560 against 1559),
  TP and Wind Waker even; 60% of that game's stamps on shapes are false alarms. Exact: 106,264 kept shapes compared
  stream by stream, 0 differed, both plants caught (jobs 1242-1274). It had been left as the owner's call since 10-24.
- **Frame skip is not the +10% it looks (job 1561, heavy area).** `--fskip on` reads `speed_pct` 23.9 -> **26.1%
  (+9%)** while **real frames a second fall 7.17 -> 3.90 (-46%)**: the pacing goes to four VI fields between copies
  (`3:2 4:23`) with gaps up to **6.2 s**. The recorded "+10%, visual glitches" was the emulated clock, not the frame
  rate - the game's logic advances faster while the player sees half the pictures. **Not a user option in this form.**

### Why a port pays or loses: its own code footprint (jobs 1491-1534, 2026-10-29)

- **20.2% of core 0 runs inside port code** in TP's heavy area (job 1533, calls x us per call from the `# port 10s`
  lines): the J2D pane walk 5.7% (2,291 calls per 10 s at 249.7 us), the PNGP matrix load 5.6% (73,941 at 7.5),
  the minimap decoration 3.4% (944 at 364.7), J3DMaterial::diff 2.9% (11,450 at 25.1), J3DSys::reinitGX 2.4%
  (2,054 at 116.9), J2DMaterial::setGX 0.2%. Against the static-Ordon bisect, their worth is +0.9%, **-0.5%**, ~0,
  +2.2%, +0.9%, +0.2%.
- **The PNGP port loses because its native code fetches more than the guest code it replaces.** The guest function is
  0xa0 bytes (40 instructions); the port's C path is span checks, 24 guest words read and byte-swapped, a paired-single
  matrix concat, an inverse transpose and 94 pipe bytes. Turning it off (PMU columns, means from 80 s):

  | scene | I-cache misses | instruction-fetch stalls | instructions retired |
  |---|---|---|---|
  | heavy, port on | 8,236 k/s | 218 M cycles/s | 265 M/s |
  | heavy, port off | 7,827 (**-5.0%**) | 209 (**-4.1%**) | 268 (+1.1%) |
  | Ordon, port on | 8,413 k/s | 241 M cycles/s | 257 M/s |
  | Ordon, port off | 8,007 (**-4.8%**) | 231 (**-4.1%**) | 259 (+0.8%) |

  Fewer misses, fewer stalls and **more** instructions retired with the port off, in both scenes. So the rule is
  sharper than "a port must do less than the guest code": **a port must remove much more guest code than the native
  code it adds**, because instruction fetch, not work, is what it is charged. A 40-instruction guest function can
  never pay as a port.
- **Instruction-fetch stalls are 27% of core 0** (218 of 804 M cycles a second in the heavy area). That is the largest
  single line item on core 0 and the reason the port *set* is worth +10.6% while its parts sum to +3.7%: the set keeps
  whole guest functions out of the compiled code. Any 10%-class lever has to shrink what core 0 fetches - a bigger
  port that replaces a whole subtree, or work skipped outright - not shorten what it executes.

### OPT_FDIRECT on the device: +2.6% in Wind Waker, flat in TP (jobs 1510-1538, 2026-10-29/30; on by default)

- **What (`--fdirect on`, off by default; C42):** float arithmetic writes frD's lane register instead of a scratch
  register and a vmov. Exact: the fuzzer with FCACHE|FDIRECT (20,000 cases, default and chain, plants all caught), and
  the whole-game qemu gate off against on - **238,313,992 guest instructions both and the same last.png hash**
  (tp_arm_850.state, 1,000 frames).
- **Offline bench (core/tools/jitbench.py, the device's mask):** hot ARM words per guest instruction psloop
  8.89 -> 8.00, floatloop 6.46 -> 6.00; integer and call loops unchanged.
- **Device, separate whole runs of 240 s scored from 80 s, frames a second:**

  | scene | off | on | change |
  |---|---|---|---|
  | static Ordon (3 pairs) | 10.400, 10.406, 10.397 | 10.441, 10.428, 10.395 | +0.20% (the on arm spreads 0.44%, the off arm 0.09%) |
  | heavy area (2 pairs) | 7.172, 7.174 | 7.180, 7.171 | +0.03% |

- **The other games, where it pays (two pairs each, 240 s, frames a second):**

  | scene | off | on | change |
  |---|---|---|---|
  | Wind Waker, Outset | 6.024, 6.023 | 6.173, 6.185 | **+2.6%** |
  | Mario Party 4, board | 8.960, 9.014 | 9.046, 9.047 | **+0.7%** |
  | TP title (1526-1527) | cutscene 40.36, later 23.05, movie 14.67 | 40.37, 23.10, 14.60 | flat |

- **Reading:** in TP it is below the noise floor - the device's own `# jit words per guest instruction` reads 2.3 hot
  either way, float arithmetic being a small share of TP's compiled code - but Wind Waker's hot code is float-dense
  enough to show the bench's -10% hot words, twice over, with an off arm that repeats to 0.02%. Exact (fuzzer with
  FCACHE|FDIRECT; the whole-game qemu gate identical: 238,313,992 instructions and the same last.png), a gain in two
  games of four and a loss in none: **on by default (rule 5)**.

### What the emulated clock is made of, and why the charge sweep read what it did (C41 answered; jobs 1480-1488, 2026-10-29)

- **Instruments (build fa1505fa264916fd, branch of a05f91d):** `# clock 10s` splits every cycle the clock advances
  into instructions really run, a stand-in's charge, a port's charge and the fills that jump an idle (`b .`) or
  polling loop to the next event, and records per VI field how much of it was fill ("field slack", tenths).
  `--port-charge PCT` scales every port's charge at run time. Read with `core/tools/clock_split.py`.
- **Ordon at defaults (job 1480, from 100 s): the clock is mostly the game waiting.** 1,810 M cycles a window:
  **poll fills 79.0%**, instructions really run **13.2%**, ports **7.6%**, stand-ins 0.2%, idle (`b .`) fills 0.0%
  (TP's idle thread polls, it does not spin on `b .`). Mean field slack **7.4 tenths, and 0.0% of fields with no
  slack left**. So the game uses about a fifth of its emulated budget: a port's charge of ~45 k cycles in a 16.2 M
  cycle picture is absorbed by a smaller fill, cannot be read as speed, and cannot push a picture past its deadline.
  (Anchor speed on this build: Ordon 35.68%, 10.708 frames a second — the instruments cost nothing measurable, and
  the device reads ~4.5% faster today than on 10-25.)
- **So where did 30.89 / 32.64 / 27.90% come from? The scripted walk takes a different path in each arm.** Draws per
  picture (`draws_s / (fields_s / 2)`) at matched *emulated* seconds, from the logs of the old sweep:

  | arm | 252 | 258 | 264 | 270 | 276 | 282 | 288 | 294 |
  |---|---|---|---|---|---|---|---|---|
  | port off (1423) | 13701 | 11398 | 11840 | 10767 | 10481 | 10065 | 10447 | 10344 |
  | port off (1425) | 13700 | 11401 | 10858 | 10722 | 10170 | 10069 | 10062 | 10348 |
  | port off (1429) | 13702 | 11399 | 11838 | 10766 | 10482 | 10064 | 10446 | 10349 |
  | charge 165 (1424) | 12954 | 10843 | 13014 | 10380 | 12236 | 11299 | 12167 | 11237 |
  | charge 165 (1432) | 12952 | 10856 | 13017 | 11086 | 11563 | 10745 | 12178 | 11228 |
  | charge 100 (1431) | 13718 | 11274 | 9984 | 10017 | 10446 | 12722 | 11201 | 15142 |
  | charge 330 (1430) | 12937 | 12509 | 14771 | 14771 | 14767 | 14842 | 15634 | 14771 |

  Three runs of one setting agree to a draw or two; the arms differ **from the first sample, 4 s into the run**, and
  the 330 arm settles on a static, heavier view (~14,800-15,600 draws a picture) by 264 s, where Link has evidently
  stopped somewhere else. Per picture the render structure is identical in every arm (EFB copies 9.0, clears 3.0 a
  picture, off and 330 alike) while the geometry differs by up to 50% (draws a picture +38%, vertices +38% in 1430),
  i.e. **the arms rendered different scenes, not the same scene at a different cost**.
- **The mechanism:** `--press` is applied when the guest polls the pad, and the charge moves the clock, so the poll
  lands in a different field; a different stick sample sends Link down a different path, and in Ordon the paths differ
  in cost by tens of percent. The charge really does change the measured speed, but what it changes is *which scene
  is drawn*, not what a scene costs.
- **The charge is absorbed by the fill, 1:1 (jobs 1485-1487, the heavy area, which takes no input).** Text port on,
  `--port-charge` 1 / 100, against the port off:

  | run | clock: run | ports | fills | M cycles a window | speed | frames a second |
  |---|---|---|---|---|---|---|
  | 1485 port off | 21.01% | 11.66% | 66.62% | 1273.1 | 23.907 | 7.171 |
  | 1486 charge 1 | 20.45% | 0.12% | 78.73% | 1269.0 | 23.716 | 7.113 |
  | 1487 charge 100 | 20.45% | 12.23% | 66.63% | 1275.3 | 23.882 | 7.164 |

  Taking the charge away moves 12 points of the clock from ports to fills and leaves the cycles a window, the speed
  and the frame rate alone: **the text port is neutral in a scene that cannot diverge** (-0.1% frames at charge 100,
  -0.8% at charge 1), so its recorded Ordon -4.3% was the walk, not the port.
- **Flipping the charge inside one run (job 1484, `--ab portcharge 330` on the walk) moves nothing.** Scored by arm:
  charge 100 ports 8.87% of the clock, fills 76.50%, 9.84 frames a second; charge 330 ports 29.33%, fills 56.12%,
  **9.92 frames a second** (+0.8%, i.e. noise). The A/B recompiles nothing and changes no state, so both arms sample
  the same trajectory: a 3.3x charge is worth nothing when the scene is held still. The walk's separate runs at
  charge 1 / 100 / 330 (jobs 1481-1483) read 8.608 / 10.204 / 10.275 frames a second against 10.708 off - swings of
  up to 20%, in a different order from the 10-25 sweep: the walk's outcome is a lottery that the timing seeds.
- **The two "losing" ports judged on a scene that cannot diverge (jobs 1491-1493, the Ordon walk state with no
  `--press`, so Link stands in the village with the HUD up; 240 s, scored from 80 s).** Port off 10.367 frames a
  second; the HUD text port (bit 10, `--port-hooks 1527`) **10.452 (+0.8%)**; the multi-matrix load port (bit 9,
  1015) **10.473 (+1.0%)**. Both read as small gains where the walk had them at -4.3% and -2.9%. The clock split
  shows the substitution plainly: ports 8.04 -> 8.60% of the clock with the text port on, instructions really run
  13.34 -> 12.79%, fills unchanged at 77.96%. Repeats queued (1494-1496) before either becomes a default.
- **Ports 9 and 10, the full picture (jobs 1485-1502, build fa1505fa264916fd).** Frames a second, repeats where given:

  | scene | port off | bit 10 (HUD text) | bit 9 (skinned matrices) | both (2039) |
  |---|---|---|---|---|
  | static Ordon (tp_ordon, no `--press`) | 10.367, 10.367 | 10.452, 10.461 (+0.8 / +0.9%) | 10.473, 10.463 (+1.0 / +0.9%) | 10.498, 10.491 (**+1.2%**) |
  | heavy area (tp_area2) | 7.162, 7.156 | 7.164 (-0.1%) | 7.162 (-0.1%) | 7.131 (**-0.4%**) |

  Exact on the device in both scenes with both bits on (`--port check`, jobs 1501-1502: 2,639 and 2,451 matrix calls,
  200 and 190 text calls checked per 10 s, **0 differed**). So the two are a +1.2% / -0.4% trade-off, not a pure gain:
  they pay where the HUD and skinned shapes are hot and cost a little where the GX thread is the limit. Owner's call
  (rule 5); off until then.
- **The default ports re-measured one at a time on the static scene (jobs 1503-1507, tp_ordon with no `--press`,
  against 10.367 / 10.367 frames a second with all defaults).** Each run turns one bit off:

  | run | frames a second | that port is worth | its credit from the walk |
  |---|---|---|---|
  | all ports off (`--port-hooks 0`, 1503) | 9.374 | **the set: +10.6%** | - |
  | minus bit 8, the minimap decoration (1504) | 10.361 | **+0.06%** | +3.1% |
  | minus bit 7, J3DSys::reinitGX (1505) | 10.275 | **+0.9%** | +3.9% |
  | minus bit 6, J3DMaterial::diff (1506) | 10.141 | **+2.2%** | +3.0% |
  | minus bit 5, the J2DPane::draw walk (1507) | 10.280 | **+0.9%** | +2.2% |
  | minus bit 0, J2DMaterial::setGX (1516) | 10.345 | +0.2% | - |
  | minus bit 1, J2DPictureEx::drawSelf (1517) | 10.369 | ~0 (not taken here) | Ordon +4.5% |
  | minus bit 2, the PNGP matrix load (1518, 1532) | 10.423, 10.419 | **-0.5%** (a loss) | +4.5% |
  | minus bit 4, the J3D lights (1519) | 10.367 | 0 (5 calls per 10 s) | heavy +2.7% |

- **The same bisect in the heavy area (jobs 1533-1543, default 7.154 / 7.156 frames a second):**

  | run | frames a second | that port is worth |
  |---|---|---|
  | minus bit 0, J2DMaterial::setGX (1539) | 7.020 | **+1.9%** |
  | minus bit 5, the J2DPane::draw walk (1540) | 7.121 | +0.5% |
  | minus bit 6, J3DMaterial::diff (1541) | 7.107 | +0.7% |
  | minus bit 7, J3DSys::reinitGX (1542) | 7.160 | ~0 |
  | minus bit 2, the PNGP matrix load (1534) | 7.157 | ~0 |
  | minus bit 8, the minimap decoration (1543) | 7.192 | **-0.5%** (a loss) |

  Every port pays in at least one of the two scenes **except bit 2 and bit 8**, and those two are complementary: the
  PNGP load is the loss in Ordon and neutral in the heavy area, the decoration the reverse. **`--port-hooks 243`
  (both off) is the default since 2026-10-30:** static Ordon 10.367 -> **10.441 (+0.71%)**, heavy area 7.155 ->
  **7.194 (+0.55%)** (jobs 1547-1548), a little more than the parts, as the set effect predicts. **The title gains
  too** (jobs 1551-1552): cutscene 40.38 -> 40.51, later stretch 23.15 -> 23.24, movie 14.67 -> 14.73, i.e. +0.3 to
  +0.4% in all three sections - so the trim pays in every TP scene measured. With `--fdirect` on top of 243 in static
  Ordon (1549-1550): 10.463 -> 10.473, flat, as FDIRECT is in TP generally.
- **The PMU bracket round the PNGP port's two halves (jobs 1544-1546, `--pmu-split port|portcommit|empty`,
  `--pmu-port-hooks 4`, runs sampled 1 in 8; shares of core 0 rounded to whole percent):** building the context reads
  cycles 1%, instructions 0%, **instruction-fetch stalls 3%**, I-cache misses 2%; committing reads cycles 1%, fetch
  stalls 2%, I-cache misses 1%; the empty bracket 0% everywhere (its own cost is below rounding). Coarse, but it
  corroborates the bisect's reason: the port's share of *fetch stalls* is three times its share of cycles, i.e. its
  native code is fetch-heavy for the work it does.

  The set is a large, real win, and the four bits above account for 3.9 of its 10.6 points - the rest is bits 0-2 and
  4 (setGX, drawSelf, the PNGP matrix load at 98,000 calls per 10 s, the lights), measured separately in 1516-1519.
  But **every individual credit from the walk was inflated**, by 1.4x (the material diff) to the whole of it (the
  decoration port, which is break-even here although it runs 1,296 calls per 10 s at ~212 us each). The decoration
  port also read -0.4% in the heavy area, so on today's evidence it earns nothing anywhere: a candidate to turn off.
- **Consequence for how ports are judged:** the scripted Ordon walk cannot settle any change that shifts the guest's
  timeline (a port's or stand-in's charge, event timing). Use a state that takes no input - the heavy area
  (tp_area2, no `--press`) - or verify that the arms drew the same scene (draws and vertices a picture at matched
  emulated seconds) before believing a walk comparison. The text port's heavy-area reading was already even
  (23.23 / 23.26%, jobs 1423-1426), which is what this predicts; its Ordon -4.3% was the walk diverging.

### HUD text as a port, and the ports' cycle charge (`--port-hooks` bit 10, off; jobs 1415-1432, 2026-10-24/25)

- **What (commit e90ac53):** J2DTextBoxEx::drawSelf with J2DPrint (construction, printReturn, both parse passes:
  widths, wrap, bindings) and JUTResFont (code map, widths, page loads with GXInitTexObj / GXInitTexObjLOD, the
  character quads), natively. Census first (jobs 1415-1416): every HUD string plain ASCII, ~4 characters, one font class.
- **Exact:** 2DS `--port check` (jobs 1421-1422): Ordon 300 and heavy area 200 calls per 10 s, all taken, 0 differed,
  ~600 font page changes per 10 s among them.
- **Speed (separate whole runs, from 100 s):** heavy area even (23.23 / 23.26%); **Ordon 34.09 -> 32.63% (-4.3%)**
  (jobs 1423-1426), although core 0's guest HUD work fell (J2DPane::draw inclusive 21.0 -> 14.5%, the text subtree's
  5.2% replaced by 2.1% native) and core 0 stayed busy the same share of the time (joint states ~88% running).
- **The ports' cycle charge decides more than their work (Ordon, port on, the text port's kInsScale):**

  | Charge (jobs) | Ordon |
  |---|---|
  | port off (1423, 1425, 1429) | 34.10 / 34.08 / 34.10% |
  | 100 (1431) | 30.89% |
  | 165, calibrated to the interpreter's count (1424, 1426, 1432) | 32.64 / 32.63 / 32.70% |
  | 330 (1430) | 27.90% |

- **The game really is slower: frames a second move with speed (2026-10-29, the same jobs re-scored).** `runs.py
  --skip 100` over the logs of 1423-1432, reading the real frame rate rather than `speed_pct`:

  | Arm | speed | frames a second | against port off | fields a picture |
  |---|---|---|---|---|
  | port off (1423 / 1425 / 1429) | 34.103 | 10.191 | - | 2:714, 1:28 |
  | charge 100 (1431) | 30.886 | 9.344 | **-8.3%** | 2:661, 1:27 |
  | charge 165 (1424 / 1426 / 1432) | 32.656 | 9.770 | **-4.1%** | 2:689, 1:27 |
  | charge 330 (1430) | 27.898 | 8.375 | **-17.8%** | 2:666, 1:37 |

  Speed divided by frames is 3.346 / 3.305 / 3.342 / 3.331 — the picture stays locked to two fields in every arm, so
  this is not `speed_pct` accounting: the port with its charge makes the game deliver fewer pictures a second, by as
  much as 18%. That rules out the first explanation C41 offered itself (that a charge change unlocks the pacing
  behind speed_pct) and leaves the charge changing the real work per picture, which the `# clock 10s` instrument
  (2026-10-29) is there to find.
  Each setting repeats to ~0.1%, but the response is not monotonic and far larger than the ~1.3% of emulated
  instructions the text calls are: the emulated timeline a port produces changes how much work the game does per
  frame (inclusive shares outside the HUD grew ~1% each with the port on). Likely the game's own time-based logic
  (it reads the timebase); not pinned down. **Consequence for every port:** a measured loss or gain can come from the
  timeline, not the work; the multi-matrix load's -2.9% (above) is probably the same effect. Kept off; REDESIGN C41.

### Region tier across scenes, and what its loss is (jobs 948-1014, 2026-10-22, `--profile on`)

- **Per scene, tier off -> on (runs.py; one off/on pair per build, builds differ only outside the measured path):**

  | Scene | Tier off -> on, each build | Reading |
  |---|---|---|
  | Wind Waker, Outset (ww_outset) | +7.2, +7.4, +7.4% | **+7.4%**, every build |
  | TP heavy area (tp_area2) | -0.9, -1.0, -0.7, -1.1, -0.8, -0.9% | **-0.9%**, every build |
  | TP Ordon walk (tp_ordon) | +0.2% (940/941); -1.45, -1.45% (1005/1006, 1010/1011) | -0.6 to -1.4% |
  | Mario Party 4 board (mp4_board) | +0.2% (942/943), -0.25% (1012/1013) | flat |
  | TP title (tp850, title_sections.py, 948/949) | cutscene 35.77 -> 35.38 (-1.1%), after it 23.76 -> 24.38 (+2.6%) | one pair |
  | TP / WW / MP4 boot (no state) | the logos run at full speed (99.7%): nothing to measure; runs 952-955 cancelled | - |

- **Builds move the baseline by up to ~0.8% with nothing in the path changed:** Ordon tier off read 30.08 on one build
  and 30.31 / 30.32 on another, tier on 30.14 and 29.89 / 29.87. One off/on pair on one build cannot settle a 1% effect;
  pool pairs over builds.
- **--tier-loops on (regions only where the head's function loops):** heavy -0.9%, Wind Waker +7.3%, Ordon -1.1%,
  Mario Party 4 -0.45% (962/965/1007/1014): the same as all regions, with a third to a half of them (720 vs 1905 heavy).
- **--tier-runs (runs before a region, default 64):** 16: heavy -1.7%, WW +7.9%; 32: heavy -1.5%, WW +6.7%; 256: heavy
  -2.6%, WW +5.1%; 1024: heavy -7.7%, WW -0.4% (971-982, against 961/964). Pending blocks are unlinked, so a high
  threshold keeps hot code going through the dispatcher and compiling inside the scored window (jitc 0.8 -> 2.0%).
  64 stays.
- **Regions are smaller than what they replace:** host words of the regions against the head and member blocks they
  replaced: heavy 328,793 / 452,417, WW 263,227 / 374,093, Ordon 377,647 / 526,831, MP4 88,117 / 132,226 (0.67-0.73).
  The code buffer still grows (heavy 47 -> 60%): the replaced blocks stay where they were, between live ones. Hot
  footprint (`# code footprint`, blocks covering 90% of compiled time): heavy 504 -> 580 KB, WW 320 -> 385 KB; the new
  `# code pages 10s` line (4 KB host pages those blocks span): heavy 398 -> 477, WW 262 -> 336.
- **Instruction micro-TLB misses are not the cost.** They swing 2-3x between builds with the same settings (heavy tier
  on 238 k/s in 967, 91 k/s in 979, 405 k/s in 990), with the code buffer's address. GC3DS_CODE_ALIGN (993-998, one
  build each, buffer at 0af0c000 / 0af10000 / 0b000000): 4 KB / 64 KB / 1 MB alignment, tier off 288 / 85 / 40 k/s at
  21.15 / 21.26 / 21.15%, tier on 411 / 151 / 45 k/s at 21.00 / 21.03 / 20.99%; stall_inst unchanged (204 / 204 / 203
  M cycles/s). The kernel maps an aligned buffer with large pages, and the micro-TLB miss is cheap. Alignment stays 4 KB
  (1 MB costs up to 1 MB of heap for nothing measurable).
- **What the loss is:** instruction-fetch stall cycles. Heavy stall_inst 204 -> 215 M cycles/s (~1.4% of core 0),
  Ordon 220 -> 242, MP4 174 -> 189, with I-cache misses +1.3%, +6.8%, +5.8%; regions' own compiles add ~0.3% after the
  first 50 s (heavy compile 0.5 -> 0.8% of core 0). Wind Waker pays the same stalls (150 -> 175) and gains more from
  its actor search.
- **Tried against it, not kept:** regions in the top quarter of the hot area (`--region-zone on`, 969/970): heavy
  21.07 -> 20.94%, micro-TLB 238 -> 323 k/s, WW unchanged (repeats 874-881: regions link into the code around them).
  A relayout (`--tier-relayout KB`: regions remember their coverage and the decisions against regions; once regions
  replaced KB of code the cache is flushed and remembered heads compile straight into regions; 975-978, 983-986):
  stall_inst back to the tier-off level (heavy 205), but the flush's recompile and the re-tiering cost as much:
  heavy 20.96-21.01% (tier on 21.07), WW 20.24-20.29% (20.12), 512 KB worse than 1024 KB (three flushes, the last
  inside the scored window). Off by default, commit abb44aa.
- **Dispatcher:** link requests 2-22/s -> 144-499/s with the tier on are links to pending targets (counted as
  "target polling / idle"); under 0.1% of core 0.

### Profiled runs paid for the decoder's statistics; the device drifted ~2% (jobs 1017-1039, 2026-10-22/23)

- **What looked like a regression:** every scene's tier-off speed sat 2-6.5% under the 10-21 baseline (heavy 22.74 ->
  21.3%). A bisect on the 2DS ran the baseline commit a45608e itself at **21.32%** (1017) and 20e9cb0 / 3b0ce88 / 0e4fff4
  / HEAD at 21.19-21.38% (1018-1021): no code regression.
- **The cause:** `--profile on` also turned on the GX decoder's statistics (a clock read around one command in 16, the
  display-list sampler) unless `--gxdiag off` was given; the baseline jobs gave it, jobs 818-1021 did not. The cost:
  heavy area ~6.5% (core 0's GX wait 0.4 -> 5.2%, dispatch 2.9 -> 7.9%). **Since 01b4be6 the statistics are opt-in
  (`--gxdiag on`).** A `# gx decode 10s` line with 0.0 ms/s means they were off. Off/on pairs inside one setting held:
  the tier's effects re-measured without them (1024-1031): heavy 22.11 -> 21.92 (-0.9%), WW 18.78 -> 20.15 (+7.3%),
  MP4 13.95 -> 13.90 (-0.4%), Ordon 30.91 -> 30.12 (-2.5%).
- **The rest is the device:** the same a45608e binary with `--gxdiag off` (1038-1039): heavy 22.24% (22.74 on 10-21),
  Ordon 30.96% (31.84): ~2-3% lower a day later, with HEAD within 0.2-0.6% of it. Compare only within one session.

### Mario Party 4's GX thread did the vertex work of draws too big for the job ring (jobs 1015-1043)

- **What:** GW_DRAWTAIL split into palette assignment, the CPU fallback, the worker queue and vw_do on the GX thread
  (jitsample.c `vwdo`, ...). Mario Party 4 (1015): **vwdo 20.6% of the GX thread**, the worker idle ~70%: draws whose
  job is larger than the 128 KB ring in use run on the GX thread. TP heavy (1016): none.
- **Ring size for everything (`--vwmem`, 1022-1023):** 512 KB / 1 MB: 13.82 -> 14.14 / 14.07% (+2.2%), vwdo gone;
  deeper rings lose for TP (ruled out earlier).
- **`--vwbig` (81739bf, on by default):** a job larger than the ring takes the buffer past it (VW_MEM is 1 MB whatever
  the ring), one at a time. First form, from the buffer's start (1032-1037): +0.1%, the GX thread waited in vwalloc
  (19.9%) for the ring to drain. Final form, past the ring (1040-1042): **Mario Party 4 13.89 -> 14.29% (+2.9%)**,
  core 0's GX wait 9.9 -> 5.7%, the GX thread idle 22 -> 28%; TP heavy 21.86 -> 21.82%, Ordon 30.04 -> 30.15%, Wind Waker 20.11 -> 20.12% (even;
  1044-1047). Frames correct (1043).

### Fault mode re-measured with the region tier (jobs 1048-1070, 2026-10-23, James restarting the 2DS)

- **Cold-code overflow:** with fault mode every access has its own slow path in cold code; one block overflowed it, failed
  to compile and was retried at every entry: heavy area 17.19% against 21.86, compile 12-33% of core 0, 19,502 failed
  compiles (1048). Fixed in 0bc59ff: such a block is compiled again with checked accesses (1 block in 1056).
- **Same session, fault mode against off (`--faultsync off` unless noted):**

  | Scene | Off | Fault mode | |
  |---|---|---|---|
  | Wind Waker (1068 / 1059) | 20.13% | 21.44% | **+6.5%** |
  | TP Ordon walk, 50-100 s (1070 / 1065, sync on, stopped at 102 s) | 30.50% | 31.54% | +3.4% |
  | TP heavy area (1067 / 1056, 1057 sync on) | 21.86% | 20.43 / 20.44% | **-6.5%** |
  | Mario Party 4 (1069 / 1060) | 14.28% | no complete run | |

- **Why the heavy area lost (1056):** core 0 waited for the GX thread 15.6% of the time (0.5% off), the GX thread
  working more for fewer frames. Not L2 contention, as first read: it was the heap. **With the site table in linear
  memory (0bc59ff, jobs 1071-1074) every scene gains and none stopped:**

  | Scene | Off (1067-1070) | Fault mode, table in linear memory | |
  |---|---|---|---|
  | Wind Waker | 20.13% | 21.15% (1074) | **+5.1%** |
  | TP Ordon walk | 30.09% | 30.84% (1072) | +2.5% |
  | TP heavy area | 21.86% | 22.36% (1073), GX wait 0.57% | +2.3% |
  | Mario Party 4 | 14.28% | 14.41% (1071) | +0.9% |

- **The "crashes" today were relay memory:** jobs 1058 (Ordon, at 5 s) and 1060 (Mario Party 4, at 15 s) stopped
  posting; the 2DS showed a relay error, not a Luma crash, and left no crash record. Fault mode's 2 MB site table came
  out of a heap with 0.2-0.5 MB free (linear memory had 7-12 MB): moved to linear memory (0bc59ff), not yet run.
  The sync-on configuration that crashed in svc 0x94 on 10-15 ran 150 s clean (1057) and Ordon 102 s (1065).
  Fault-mode runs have no sampler profile lines.
- **Soak runs, 600 s each, no stop:** title (1075: cutscene 36.33%, later 23.92%, movie 13.30%) and heavy area (1076:
  22.47% over 53 windows, against 21.86% off; 1,430 faults, heap 0.36 MB free).
- **On by default it crashed (6669041, reverted in 621b7cc):** jobs 1096 (Mario Party 4, at 9 s) and 1097 (TP heavy,
  before its first log post) stopped within an hour; Luma's dump 16 (reported by 1098): kernel prefetch abort at pc
  fffffffe, r9 = 0x94, svc mode, the same as 10-15's dumps: inside svc 0x94 (Luma's instruction-cache invalidation),
  which every compile issues (jit_host_sync_icache), not only fault mode's patch sync. With `--faultsync off` the crash
  is rarer, not gone. **Off by default again.** Open: why svc 0x94 faults only after the user exception handler has
  run (it resumes the thread straight from the handler: jit_fault_resume); a clean return through the kernel, or no
  svc 0x94 while fault mode is on (a cache sync without it), are the next things to try, with James at the 2DS.
- **What the crash is (Luma3DS sources, k11_extension, read 2026-10-23):** svc 0x94 is `mvn r1, #0; ldr r12,
  =invalidateInstructionCacheRange; bx r12`, a tail call into the stock kernel's ranged I-cache routine with length -1
  (svc.c buildAlteredSvcTable; utils.s); the routine's address is found at boot by scanning for `pop {r3, pc}`
  (0xE8BD8008, main.c). Luma keeps no per-thread exception state (fatalExceptionHandlersMain.c only decides fatal or
  not). Dump 16: r12 = fff1da0c (in that routine), r3 = r4 = r5 = 0xFFFFFFFF, lr 0, pc 0xFFFFFFFE: the routine's final
  `pop {r3, pc}` read two 0xFFFFFFFF words: its kernel stack had been overwritten. User code cannot write there.
- **Not an escape:** our own I-cache routine through svcBackdoor also ended in a kernel prefetch abort (2d9947f,
  2026-10-10, before fault mode existed; replaced by svc 0x94 in 9c61dc7); Luma's ranged 0x93 reached other cores.
  The fault itself must be what disturbs the kernel: next, check the kernel's exception entry (what our resume skips)
  and whether a fault taken while an interrupt or another core's cache broadcast is pending is the trigger.

### What keeps draws from merging (jobs 1116-1126, 2026-10-23, `# merge why` / `# merge unmergeable` / `# merge breaks`)

- **Could not join an open draw:** another vertex format, the byte cap and the segment cap never; the 256-vertex cap
  ~130 a second. TP's heavy area: ~5,800 line or point draws a second are its whole "not mergeable" count (Ordon ~3,000).
  Joining lines and points (`--mergelines`, tried in 1121-1122): jobs issued rose 8,583 -> 11,619 a second for no
  speed (22.08 / 22.06%), since nearly every line draw follows a state change; not kept.
- **Open draws issued by a state change (per 10 s):**

  | Scene | BP | CP array bases | XF matrices | XF registers | other |
  |---|---|---|---|---|---|
  | TP heavy area (1124) | 8,978 | 7,772 | 4,047 | 2,412 | 134 |
  | Wind Waker (1125) | 43,864 | 630 | 2,016 | 485 | 0 |
  | Mario Party 4 (1126) | 959 | 87 | 6,398 | 263 | 0 |

- **Reading:** no cheap merging headroom. BP changes are mostly real material / TEV changes (separate draw calls
  anyway); merging across array-base changes needs per-segment array bases in the gather, and across matrix loads
  per-segment matrices (a palette): both are G1 / G2 territory (EMULATOR_CEILING.md). Mario Party 4's breaks are
  83% matrix loads, the heavy area's 34% array bases + 17% matrices.

### X5: GX-thread time inside display-list calls, by repetition (jobs 801-804, 2026-10-22, `--profile on`)

- **What:** each list call's GX-thread ticks (nested commands and draws included; the vertex worker's gather and draws
  issued later from the merge buffer are not), split by what fifo.c `dl_note` found: a new address or size, a sampled
  call (1 in 4) whose bytes and array bases equal its last use ("repeat"), a sampled call that changed, unsampled.
  Report line `# dlist time 10s`. GX-thread busy time = 1000 ms/s minus `# gx thread where`'s idle share.

  | Scene (job) | GX thread busy, ms/s | Inside list calls | Repeat share of sampled list time (with new calls as not repeating) | Repeating list time as a share of the GX thread |
  |---|---|---|---|---|
  | TP heavy area (801) | ~925 | 292 ms/s, 32% | 81% (60%) | **~19-26%** |
  | TP Ordon walk (802) | ~880 | ~300 ms/s, 34% | 96% (84%) | **~28-33%** |
  | Wind Waker, Outset (803) | ~370 | 98 ms/s, 27% | 92% (46%) | ~12-24% (not its limit) |
  | Mario Party 4, board (804) | ~815 | 705 ms/s, 86% | 99.6% (61%) | **~52-86%** |

- **Verdict against X5's threshold (over ~50% of the GX thread on repeating work):** **not met for TP** (19-33%),
  met for Mario Party 4. "Repeat" is an upper bound (no incoming-state key; X1b's dependency-aware key repeated 42-61%
  on the PC). Most of TP's GX-thread time is outside display-list calls: immediate commands (the GX SDK's own writes),
  frame end, textures, presents. A translated-list cache (G2) cannot be TP's GX fix alone. "New" is inflated by the
  one-way address table's collisions (455-1,250 new addresses/s).

### G2-0: display-list keys on the 2DS (jobs 1127-1146, 2026-10-23, `--dlkey on`)

- **What:** per display list called from the FIFO, whether its key repeats, by how much incoming state the key holds
  (`core/src/gx/dlkey.c`, `# dlkey 10s` / `# dlkey more 10s` / `# dlkey misses 10s`). A word enters the key when the
  reference decoder or renderer reads it before the list writes it: X1b's dependency rules (`gxcensus_x1b.inc`),
  ported to run inside the 3DS build on gameplay states. Keys, each the one before plus more: **bytes + array bases**;
  **+ structural** (BP, CP, CP arrays, XF registers); **+ parametric** (TEV colour registers and konsts with their BP
  registers 0xE0-0xE7, fog 0xEE-0xF2, ambient / material colours XF 0x100A-0x100D, viewport and projection XF
  0x101A-0x1026: values a replay can apply as they are); **+ lights**; **+ matrices** (and words indexed loads bring
  from RAM). Compared with the same address's last use and with every key of the previous picture (EFB -> XFB copy).
  Also a superset key over every register a draw could read, read or not (what a signature kept on register writes
  would see), and, for misses on the same path (same bytes, same words read in the same order), which words differed.
- **Checks:** on the PC its full key reproduces X1b exactly on three title spans (5000 -> 5200: 37,464 calls, last-use
  repeat 48.08%, 7,515 read-set changes; 900 -> 1100: 42.03%; 6590 -> 6790: 53.41%: X1b's numbers), the state hash is
  the same with the census off, on and planted, and `--dlkey plant` / `DLKEY=plant` (every key made unique) reads 0% on
  every key, on the PC and the 2DS (job 1146). The census is heavy (the heavy area falls to ~3 pictures a second): its
  time columns include its own work, so the shares below are by calls and by draws.
- **Results (round 3, jobs 1142-1145; last use / last picture):**

  | Scene | Calls/s | Draws/s | Bytes + arrays | + structural | + parametric | + matrices | Structural, by draws (last picture) |
  |---|---|---|---|---|---|---|---|
  | TP heavy area | 2,709 | 51,144 | 69.7 / 85.4% | 54.3 / 85.4% | 48.0 / 84.5% | 35.9 / 50.4% | **100.0%** |
  | TP Ordon walk | 2,629 | 44,799 | 89.7 / 93.0% | 68.5 / 93.0% | 61.2 / 73.3% (by draws 14.9%) | 59.7 / 63.0% | **99.9%** |
  | Mario Party 4 board | 4,986 | 4,986 | 98.2 / 100.0% | 97.5 / 100.0% | 94.9 / 97.4% | 41.7 / 48.9% | **100.0%** |
  | Wind Waker, Outset | 2,416 | 41,781 | 84.4 / 83.4% | 50.5 / 83.1% | 50.5 / 82.5% | 43.8 / 41.5% | **99.9%** |

- **Reading:**
  1. **Every draw-carrying list repeats its structural state from one picture to the next** (99.9-100% of draws in all
     four scenes). The calls that miss carry almost no draws: lists of state alone whose bytes change every frame.
  2. **Against the last use it is much lower** (TP heavy 54%, Wind Waker 51%): the same list is drawn under several
     states in one picture (the differing words are bp00, bp40, bp41, bpc0 / bpc1: other passes or materials). A cache
     needs a few variants per list, keyed by state, not one entry per list.
  3. **The parametric words must be applied, not checked:** in Ordon the key with them repeats on 14.9% of draws, without
     them on 99.9%; the words that differ are the projection (XF 0x1020 / 0x1022, every picture while walking) and TEV
     colour register 1 (an animated colour). Matrices change for ~50% of calls in every scene.
  4. **A whole-state signature does not work:** the superset key repeats as often as the read-before-write key in TP
     (85.4%, 92.9%), but 0.0% in Mario Party 4 and 21.1% in Wind Waker (registers no list reads change every picture).
     The guard must be the list's own read set: **~15-40 structural words a call** (TP heavy 16.6, Ordon 11.4, Mario
     Party 4 37.5, Wind Waker 19.4) plus ~10-32 parametric words applied live.
  5. Lists with side effects (PE signals, copies, palette loads): 0.0-0.4% of calls in TP and Mario Party 4, 4.5% in Wind
     Waker. Lists that load matrices or lights themselves: 0-20%. Distinct structural keys a picture: 253-650.
- **Two findings outside the census:** (a) Mario Party 4's converted shapes are made again after page stamps ~6,100
  times per 10 s (job 1129: ~161 ms/s of the GX thread converting) and ~17,500 draws per 10 s go to the old path as
  "changing", while the sampled hash checks find no change; (b) display lists recorded through the write-gather pipe
  (GXBeginDisplayList, the GP unlinked) reached RAM without a page stamp, and a save state's RAM without any: G1's
  shapes (and G2) could replay a list re-recorded at the same address. Both are now stamped (hw/gx.c gp_burst,
  savestate.c).
- **X5 still bounds G2 in TP:** list calls are 32-34% of TP's GX thread (19-33% repeating); G2 alone moves
  TP's GX wall by at most that share. The design that follows from these numbers is in docs/research/G2_PLAN.md.

### G2a: pre-decoded display lists (jobs 1147-1161, 2026-10-23, `--g2`)

- **What:** G2_PLAN.md stage a (`core/src/gx/g2.c`, fifo.c `g2_replay`): a list called from the FIFO is decoded once
  into ops and replayed through the decoder's own state functions while its pages are unstamped and the vertex formats
  its parse read hold. Lists whose bytes change every frame are given up after four decodings.
- **Exactness:** PC, title spans 5000, 900, 6590 (200 fields each, drawing on): state hash and last frame of G2 off;
  `G2=check` 0 differed in 12,099 compared replays; `G2=plant` 1,039 / 1,228 / 783 differed. 2DS, `--g2 check` (1
  replay in 8 run by the decoder on a copy of the state and compared): **0 differed** in TP heavy (job 1147, ~5,300
  checked per 10 s), Ordon (1148, ~7,000), Mario Party 4 (1149, ~7,900), Wind Waker (1150, ~2,450); byte hashes 0
  changed; `--g2 plant` (1151) ~900 differed per 10 s. Replayed: TP heavy 68.5% of list calls (the rest change every
  frame), Ordon 91%, Mario Party 4 100%, Wind Waker 83.5%.
- **Separate whole runs, one build, `--profile on` (jobs 1152-1161):**

  | Scene | Speed off -> on | GX thread busy off -> on | Decoder share (`dec_*`) |
  |---|---|---|---|
  | TP heavy area (two pairs) | 22.077 / 22.087 -> 22.048 / 22.073% | 88.7 / 88.8 -> 86.3 / 86.3% | 15.8 -> 12.1% |
  | TP Ordon walk | 30.848 -> 30.833% | 83.5 -> 81.0% | 15.6 -> 11.2% |
  | Mario Party 4 board | 14.369 -> 14.379% | 74.7 -> 74.1% | 5.5 -> 2.6% |
  | Wind Waker, Outset | 20.133 -> 20.136% | 38.3 -> 37.0% | 7.7 -> 6.5% |

- **Reading:** exact, and it takes **1.3-2.5 points off the GX thread's busy time** (the decoder 3-4.4 points; the rest
  of the saving reappears as the replay's own work), the size G2_PLAN.md predicted for removing the parse. **Speed
  does not move** in any scene (within 0.1%): TP and Wind Waker are core-0-bound (core 0 waits on the GX thread
  0.4-1.2% of the time), and Mario Party 4's GX thread waits on the vertex worker (`vwalloc` + `vwqueue` ~25%), so the
  decoder's saving there became waiting (busy -0.6 points). Kept off (owner's call); it is the base G2b builds on.
- **Where a converted run's time goes (jobs 1162, 1164, `# shapes phases 10s`, 1 `ns_draw` in 16 timed, GX thread
  ms/s):**

  | Scene | Entry | Batch check | Shape lookup | Batch state | Matrices, attributes | Draw call | Total |
  |---|---|---|---|---|---|---|---|
  | TP heavy area (~2,090 runs/s) | 6.5 | 15.3 | 6.6 | 53.7 | 10.2 | 25.0 | ~117 (~55 us a run) |
  | Mario Party 4 board (~4,400 runs/s) | 32-46 | 15.0 | 188-204 | 68-79 | 17.0 | 38.6 | ~370 |

  The batch state (flush, `update_tev`, the batch key built again, texture lookups and setters, the lighting set-up
  again, fragment state, uniforms) is ~46% of TP's converted-run time, the draw call (`C3D_DrawElements`, not C25's raw
  path) ~21%, the batch check ~13%. Mario Party 4's lookup is its conversions after stamps (~180 ms/s, "G2-0" finding
  (a)). (Ordon's phase run, job 1163, did not start: its `--press` list was empty.)

### The GX thread's batch-state steps: --nsonce, --drawonce, --nsraw, --texfast (jobs 1165-1228, 2026-10-23/24)

G2_PLAN.md section 4's next GX steps, each behind its own switch with a check mode, scored with separate whole runs
in one session (`--profile on`, runs.py; GX busy from `# gx thread where`). `# shapes state 10s` (job 1179) split
ns_draw's batch state step in TP heavy: **textures 30 ms/s** of ~49, issue + split 8, fragment + binds + uniforms 4.4,
TEV + key 3.6, lighting 2.7; each converted draw call writes ~259 command words (6 dirty blocks, 27 uniform vectors).

- **--nsonce** (ns_draw's batch check hands its batch key, TEV lookup and lighting set-up to the state step): check
  0 differed in ~83,000 compared (jobs 1165-1167), plant differs (1168). TP heavy 21.959 / 21.969 -> 21.972 / 21.974%,
  GX busy 88.8 / 88.7 -> 88.0 / 88.0%, state step 55.8 -> 48.5 ms/s; Ordon 30.791 -> 30.769%, busy 83.7 -> 83.3%;
  Mario Party 4 14.347 -> 14.350%; Wind Waker 20.139 -> 20.125% (1169-1178). Exact, GX -0.2-0.8 points, speed even.
- **--drawonce** (the same in pica_draw, the old path): check 0 differed in ~257,000 compared (1191-1194), plant
  differs (1195). TP heavy 21.956 / 21.946 -> 21.972 / 21.958%, busy 89.0 / 89.1 -> 87.9 / 88.0% (`batchkey` 5.8 ->
  4.3%); Ordon 30.744 -> 30.744%, busy 83.6 -> 83.1%; Mario Party 4 14.285 -> 14.275%; Wind Waker 20.131 -> 20.137%
  (1196-1205). Exact, GX -0.1-1.1 points, speed even.
- **--nsraw** (converted draws through C25's raw draw call, attribute / buffer blocks by citro3d's own binds inside
  raw state): C25's compare 0 differed (736 compared, job 1182). **No gain:** TP heavy 21.991 / 21.992 -> 21.989 /
  21.975%, busy unchanged (1181-1190): the converted draws still went to citro3d (2,100 a second "to citro3d" against
  366 without it) because each also leaves fixed attributes dirty (ns_const), which raw state does not write.
- **--texfast 3** (bit 1: a hint table in front of the 384-entry texture table scan; bit 2: the once-a-frame texture
  re-hash only for textures whose RAM pages were stamped since): check (`--texfast 7`, 1206-1209) **0 textures clean
  by stamps yet changed** in ~5,800-11,000 re-hashes per 10 s per scene; the hint took 11,000-19,000 lookups per
  10 s that scanned the table before (scans 11,799 -> 702 in TP heavy). Separate whole runs (1210-1219):

  | Scene | Speed off -> on | GX busy off -> on |
  |---|---|---|
  | TP heavy (two pairs) | 21.923 / 21.921 -> 22.083 / 22.099% (**+0.8%**) | 89.3 / 89.2 -> 86.8 / 86.9% |
  | TP Ordon walk | 30.733 -> 31.006% (**+0.9%**) | 83.5 -> 81.4% |
  | Mario Party 4 board | 14.225 -> 14.833% (**+4.3%**, 8.50 -> 8.88 frames a second) | 74.8 -> 72.4% |
  | Wind Waker, Outset | 20.094 -> 20.166% (+0.4%) | 38.2 -> 36.1% |

  TP heavy's converted-run texture time 31.1 -> 4.9 ms/s. Speed moves even where core 0 is the limit: the re-hash
  streamed guest texture RAM through the shared L2 every frame, evicting core 0's working set (the effect measured in
  C2: a second core reading guest RAM costs core 0 ~6%). Mario Party 4's core 0 waited on the GX thread 6.1 -> 2.6%.
  Exactness: the re-hash it replaces was itself sampled (at most 4,096 words a texture); the stamps cover every byte
  of a texture written by DMA, a cache flush or an EFB copy, the rule G1's shapes already rely on. A game writing
  texture memory without flushing it would keep a stale texture where the sampled hash may have caught it.
- **The two bits apart (jobs 1220-1228, separate whole runs, one build):** TP heavy off 21.953 / 21.906%, hint only
  (`--texfast 1`) 21.920 / 21.930%, stamps only (`--texfast 2`) **22.058 / 22.074%** (busy 89.1 / 88.9 -> 86.9 /
  87.0%); Mario Party 4 off 14.231%, hint 14.220%, stamps **14.825%**. **The stamps carry the whole gain; the hint
  table measures nothing** (the per-map last-texture check already takes 86% of TP's lookups). Candidate default:
  `--texfast 2` (owner's call).
- **--copystamp (jobs 1238-1268):** EFB copies hashed their destination's RAM when made (sampled, up to 4,096 words of
  ~150 KB, so nearly every load a miss): **621 / 855 / 256 hashes per 10 s costing 24 / 36 / 8 ms/s** of the GX thread
  in TP heavy / Ordon / Wind Waker (1238-1240), to compare at a lookup in a later frame, of which there were **0** in
  every scene (check runs 1257-1260, title included). With it the copy notes the stamp after its own (fifo.c now
  stamps a copy's destination before the backend copies) and hashes late, only when reused in a later frame with clean
  stamps. TP heavy 22.055 / 22.082 -> 22.213 / 22.229% (**+0.7%**, busy 87.4 / 87.2 -> 85.2 / 85.2%), Ordon 30.982 ->
  31.261% (**+0.9%**), Wind Waker 20.180 -> 20.245% (+0.3%) (1261-1268): another RAM stream off the shared L2. PC: the
  reorder leaves the state hash and frames unchanged (5000 -> 5200). Candidate default (owner's call).

### Stamped shapes kept when their sources did not change: --nskeep (jobs 1242-1256, 1269-1274, 2026-10-24, C39)

Job 1129 found Mario Party 4 converting ~611 shapes a second again after page stamps on their sources (~161 ms/s of
the GX thread), and a shape made again three times stays on the old path for good (`ns_dyn_note`). `--nskeep`
hashes a stamped shape's sources over every byte (`ns_xhash`, 64-bit; `ns_sources_hash` goes through `hash_words`,
which samples ranges past 16 KB and drops a range's last 1-3 bytes) and keeps the shape when the hash matches the one
certified when it was made. A shape is certified only when made again with its sources hashed the same before and
after converting, so the hash describes the very bytes the conversion read. Build b4436ae + the change, `--profile
on`, no `--gxdiag`, separate whole runs, runs.py.

- **Count (`--nskeep count`, made again as before, 1242-1243):** Mario Party 4 **3,625 of 6,052 stamped shapes per
  10 s unchanged (60%)**; TP heavy **68 of 68 (100%)**, but only ~7 a second (its ~6,400 draws per 10 s "left as
  changing" are shapes marked dynamic before they were ever hashed; with the switch on they stay, so those are real
  CPU-deformed geometry). The sampled hash agreed with the exact one on every count.
- **Check (`--nskeep check`, each kept shape converted again into a spare slot and compared stream by stream):** **0
  differed** in 99,408 compared (Mario Party 4, 1244) and 6,856 (TP heavy, 1246). **Plant (1245):** the fresh
  conversion with one byte altered differed in 33,772 of 33,772; of 4,221 kept shapes recorded as made from sources
  whose last byte of a range then changed (the stamp path clean), **ns_verify caught 18 and the next stamp 4,199**;
  none was kept again.
- **Separate whole runs:**

  | Scene | Speed off -> on | GX busy off -> on | Made again after a stamp / 10 s | ms converting / 10 s |
  |---|---|---|---|---|
  | Mario Party 4 board (two pairs, second in reverse order) | 14.101 / 14.178 -> 14.395 / 14.428% (**+1.9%**, 8.45 -> 8.60 frames a second) | 73.4 / 73.9 -> 75.5 / 75.7% | 5,976 -> 1,442 | 1,331 -> 874 |
  | TP heavy (two pairs) | 22.233 / 22.102 -> 22.218 / 22.103% (even) | 86.8 / 86.2 -> 86.8 / 86.1% | 69 -> 0 | 10 -> 0 |
  | TP Ordon walk | 30.981 -> 31.012% (+0.1%) | 81.2 -> 80.4% | 290 -> 0 | 34 -> 0 |
  | Wind Waker, Outset | 20.156 -> 20.164% (even) | 35.8 -> 35.8% | 0 -> 0 | 0 |

  Mario Party 4 keeps ~847 shapes a second that it made again before; its GX thread does more in all (busy +2 points)
  because it draws more frames, and core 0's wait on the GX thread fell 8.8 -> 7.9%. Its draws left on the old path
  as changing fell only 19,513 -> 15,767 per 10 s, and ~144 shapes a second are still made again: real changes (CPU-
  skinned geometry), which this cannot help. TP and Wind Waker convert almost nothing after stamps, so they are even.
  Candidate default (owner's call).
- **The off path costs nothing (jobs 1271-1274, Mario Party 4, one session):** base build 8105c4a 14.159%, the change on 8105c4a
  off 14.180%, on **14.428%**, base again 14.150%. Every Mario Party 4 run of 10-24 night reads ~14.15% off, against
  14.83% for job 1231 earlier the same day on b4436ae: the device moved, not the build (TP heavy, Ordon and Wind Waker
  read the same as jobs 1261-1268 there).

### PE reads no longer drain the GX thread (jobs 354-355, device)

- **What:** every PE register read ran gxAsync->sync, waiting until the GX thread had executed everything fed
  (vertex worker included); GXFinishInterruptHandler (the cutscene's top function, 4.3% of samples, job 353) reads the
  status right after each finish interrupt. With `--pesync off` a read applies the signals posted so far (poll) only.
- **Same build, job 354 (wait) against 355 (no wait):** cutscene **29.10 -> 30.03% (+3.2%)**, later 17.84 -> 18.01%,
  movie 12.38 -> 12.43%; core 0's GX wait in the cutscene 7.37 -> 4.08%, later 1.20 -> 0.21%. No errors; frames
  presented per 10 s equal (99 / 99 xfb copies in the cutscene). Default since; `--pesync on` restores the wait.
- **Profiles were off before this (job 353):** `--profile on` is needed for '# hot guest code' (blocks store their
  start only then); without it each report credits ~93% to one stale block (jobs 350-352).

### Byte-swapped guest RAM, C18 (jobs 351-352, device; PC checks)

- **Job 351 (first swapped build):** cutscene **28.78%**, later **16.76%**, movie **11.67%** against job 350's
  29.09 / 17.87 / 12.42 (-1.1% / -6.2% / -6.0%). JIT code **9.80 host words per guest instruction against 7.98**
  (exit stubs 0.9 -> 1.4, cold 1.2 -> 1.6 per guest instruction): a second deopt exit per first hoisted access.
- **Job 352 (alignment folded into the window stub, aligned accessors in the stand-ins):** cutscene **28.92%**,
  later **17.91%**, movie **12.41%** (-0.6% / +0.2% / even); code 7.78 against 7.74. Neutral: removing rev from
  every guest word load is worth nothing measurable on core 0.
- **Correctness (PC):** GC_SWAPRAM 1 against HEAD byte-identical (state hash and last.png) for boot 0-600, tp_pc_900
  -> 2100 and tp_pc_5000 -> 5400; HLE_CHECK fields 900-2100 in the swapped layout 0 differed (5,007,286 PS, 1,096,284
  J3DGD, 5,814 ShdwDraw; HLE_STOP_EVERY=37 1,200 stopped inside); JIT fuzzer mask 4723, swapped: main 20,000 (11,148
  float), chain seeds 1-8, default seeds 11/21/31, 0 fails, planted bugs detected.

### Translator direct mode: calcWeightEnvelopeMtx (jobs 333-334, device; PC HLE_CHECK)

- **Exact:** fields 900-1400, 1,500 calls, 0 refused, 0 differed; with `HLE_STOP_EVERY=37` (every 37th access fails,
  so each call stops somewhere inside and guest code goes on from there) 750 calls, all stopped inside, every GPR, FPR,
  CR, XER[CA], LR and the stores compared: 0 differed.
- **Device (job 333 with it, 334 without, same build):** 462 calls / 10 s in the later stretch, 0 refused, 0 stops.
  **Cutscene 28.61 -> 28.78% (+0.6%)**, later stretch 16.55 -> 16.69%. Cutscene runs repeat within ~0.05 (328, 329,
  334: 28.59 / 28.64 / 28.61), so the gain is real, but it is small for a function that was 2.36% of the cutscene
  (job 311): the C runs it only ~1.3x faster than compiled code.
- **Why (objdump of the 3DS build):** 2,353 ARM instructions, 394 VFP loads and 141 stores (19 FPR pairs live: the
  VFP's 32 single registers spill), and every exact paired-single multiply-add is three widenings to double, a double
  multiply-accumulate and a narrowing (134 vcvt.f64.f32, 52 vcvt.f32.f64, 42 vmla.f64). The JIT pays much the same
  VFP work, so translating FP-heavy code gains little; integer and control-heavy code (ShdwDraw, the dispatch around
  small functions) is where native C wins.
- **Relaxed floats (job 338, `--ab fastfloat`, 600 s, owner-approved experiment):** single and paired-single results
  not rounded to single, frC not cut to 25 bits, stores converted by the hardware. Cutscene core 0 30.64 exact vs 29.11
  relaxed (medians; phase pairs -2% +-20), later stretch 14.28 vs 13.79 (+0.7% +-12.6): **no gain**, I-cache misses
  +5% with it on. VFP rounding latency is not what limits core 0; option kept, off. The fuzzer with it on shows the
  expected differences (lfs / psq_l / fdivs cases with double inputs); exact by default, clean.
- **Float lanes only in float-dense blocks (job 335, `--ab fcache --fc-min 8`, 600 s, ab_split --col opt):** cutscene
  core 0 30.70 off vs 29.69 on (medians; phase pairs +7% +-20, within noise), later stretch 14.85 vs 14.52 (+1.0%
  +-2.5, within noise); I-cache misses +4-5% with lanes. Gating by float density does not rescue C11: the lanes' extra
  words cost what their cheaper arithmetic saves, on a core bound by instruction fetch. Not kept as a default.
- JIT fuzzer gate for the mid-function exit (`_fuzz.sh midstop 4723`): 0 fails except default-mode seed 11, case 5393
  (`stfs f24,-32(r1)`: f30.ps1 a single denormal in the interpreter, 0 in the JIT), **the same on the committed HEAD**:
  the known denormal flush (host RunFast; TP runs with FPSCR[NI]) in a case the generator let a denormal into.

### DSP voices and JAudio (jobs 297-298)

- **Census (job 297):** the mail-level Zelda microcode never marked a voice done: **~30 voices enabled and not done in
  every frame** (~5 looping), all with 0 samples left (the counter a real DSP sets on a voice's reset).
- **Voice lifetime (job 298, `--ab voices`, Dolphin's Zelda.cpp rules, no mixing):** voices end (~90-110 per 10 s out
  of samples), live voices per frame **~30 -> ~13**. **Speed: no effect** (cutscene 27.93 +- 0.19 off / 27.64 +- 0.27
  on; later medians 15.65 / 15.77; phase pairs within noise). JAudio's cost is its own per-frame sequence and channel
  work, not the number of live DSP voices. Kept on by default (closer to the hardware; a mixer needs it).

### Fault delivery on the 2DS kernel (fault probe, jobs 293-295)

- **Job 293:** 20,000 faults on MEM1's guard page (user space, no access) were all delivered (1,660 ns each); then
  faults at 0x50000000 (above user space) stopped the kernel after ~13,000. The Luma dumps' data abort at 541ab000 is
  the gather pipe's host address in fault mode (mem1 - 0x80000000 + 0xCC008000).
- **Job 294:** crashed within the probe's first ~220 faults above 0x40000000 (nothing logged).
- **Job 295:** with every fault in user space (the guard page first) it **also** crashed within the first faults.
- **Verdict:** the kernel's fault delivery fails at random, not only above 0x40000000; job 293's 20,000 user-space
  faults were luck. Fault mode stays off; its accesses now clear bit 30 (harmless: uncached MEM1 reads MEM1, the other
  regions fault in user space) but that is not the fix. No further fault experiments without a new idea about the
  kernel side (each costs a reboot by hand).

### Core-0 switches re-tested with the GX wall gone (jobs 290-292, `--ab`, 600 s, ab_split --split 200 --end 403)

The cutscene no longer waits on GX (converted shapes), so switches judged "within noise" behind that wall were run
again. **None helps:**

| A/B | Cutscene speed (arm that starts / the other) | Later stretch medians |
|---|---|---|
| fcache (OPT_FCACHE, float lanes) | off 27.86 +- 0.12 (n 48) / on 27.24 +- 0.27 (n 65) | 15.40 / 15.62 |
| inlmem (OPT_INLMEM) | off 27.87 +- 0.13 (n 48) / on 27.20 +- 0.27 (n 65) | 15.41 / 15.57 |
| kstore (OPT_KSTORE) | off 27.79 +- 0.15 (n 48) / on 26.93 +- 0.27 (n 65) | 15.26 / 15.41 |

In all three the starting arm has fewer cutscene rows and reads faster: phase alignment over the window (the ab_split
scene trap), not an effect. Phase pairs (3-4 per window) are within noise everywhere.

### What full speed needs on core 0 (2026-10-16 synthesis)

- Cutscene with shapes on: ~37 M guest instructions/s on ~800 M core-0 cycles: **~21.6 cycles per guest instruction**
  (everything on core 0 included). Full speed needs ~103.7 M per emulated second: **~7.7 cycles**.
- The JIT's code for TP's mix costs **~11 cycles in cache** (job 279); the other ~10 are footprint, data and block
  boundaries (jobs 281 / 283). Perfect layout alone would stop near ~50% speed in the cutscene: full speed needs both
  far denser code and cheaper code per class. The in-cache gaps that matter, by weight in TP's mix: register-cache
  pressure (5 host slots: addi 3.4 host instructions with 8 guest registers vs 1.5 with one), CR updates (cmpwi 6.2
  host instructions), loads / stores through the stubs (stw 7.6 cycles), single-precision floats and paired singles
  (30-80 cycles), calls (~34 cycles per bl / blr).

## Converted shapes in the emulator (jobs 272, 274-276, device)

- **Why (job 272):** in the cutscene core 0 waits **gxwait_pct ~19%** for the GX thread (gxthread_pct ~83%, vertex
  worker ~64% busy, waiting ~27% of the time itself); in the later stretch the wait is 0.8%.
- **Runs of draws inside lists (job 274, `# dl runs 10s`, cutscene):** 64-71k draws/s inside lists in 1.26-1.61k runs/s,
  **43-54 draws per run**; runs of 2+ draws hold **99.3-99.9% of list vertices** (320-354k vertices/s); no run ended by
  a CP, XF, BP or VAT command (each list is one run of one vertex format); no lines or points.
- **First run (job 275, ab57c1b, `set shapes 1` live for ~54 s of the cutscene):** 11-15k runs per 10 s drawn
  converted, ~70 shapes (~1.8 MB linear), 2-5 conversions per 10 s once warm; ~4.4k runs per 10 s on the old path for
  their layout (retried at every draw: fixed in the next build, ns_never) and ~710 left as changing; source hash checks
  178-192 per 10 s, **0 changed** (the page stamps missed nothing). Screenshots with shapes on (shots 9-12: Epona and
  Link, the cliffs, the wolf at the twilight wall) render as with them off. Adjacent windows, not an A/B:

| Window (real s) | Shapes | Speed | GX thread busy | Core 0 waits on GX | Vertex worker busy |
|---|---|---|---|---|---|
| 30-63 | off | 18.8% | 86.5% | 24.0% | 62.1% |
| 66-117 | **on** | **28.3%** | **49.0%** | **2.3%** | **10.4%** |
| 120-170 | off | 23.0% | 81.9% | 14.4% | 63.3% |

- **A/B (job 276, `--ab shapes`, 600 s, 20 s phases; ab_split.py --split 250 --end 476):** cutscene **21.09 +- 0.24 ->
  27.20 +- 0.22% speed (+29%)** (n 80 / 82), core 0's own speed 25.83 -> 27.84 (+8%: less GX-side traffic too; phase
  pairs +11.2 +- 13.4, 5 pairs); later stretch 13.23 +- 0.21 -> 13.46 +- 0.31 (within noise; it never waited on GX);
  I-cache misses 5.68 -> 7.24 M/s in the cutscene (more guest code run per second). **Adopted: on by default.**
- **Sections by emulated time from now on.** A faster build reaches each section sooner in real time, so the old real
  windows (30-250 / 265-475 / 510-600 s) mix sections. Their emulated equivalents (job 272): **cutscene 19-66 s,
  later stretch 69-97 s, movie 99-110 s** (scratchpad emu.py; ab_split.py still splits by real time).
- **Baseline with shapes on (job 277, 743076e + default on) against job 272, emulated windows:** cutscene **21.32 ->
  27.38%** (GX thread 82.7 -> 50.4% busy, core 0's GX wait 19.1 -> 2.1%, vertex worker 63.5 -> 10.8%); later stretch
  **13.66 -> 14.39%** (GX thread 48.4 -> 23.1%); movie **4.10 -> 8.65%** (job 273's fix). Shapes never made (ns_never):
  ~270-280 per 10 s, all for their vertex layout (texture matrix indices in the vertices); left on the old path as
  changing or never made ~4.7-7.7k draws per 10 s; ~80-90 shapes made again after a stamp per 10 s; hash checks 0
  changed; 81-134 shapes, 1.9-2.2 MB.

## Movie: psq_st to the locked cache compiled (jobs 266, 272, 273, device)

- **Cause (job 272, `# psq_st fallbacks 10s`):** in TP's movie (510-600 s) all ~3.1-3.7M interpreted psq_st per 10 s
  are **u8 quantized stores to the locked cache** (0xE0000000; the SDK's THP decoder writes its pixels there); 2-6 per
  10 s for any other cause (a GQR changed after compiling). Job 266: float fallbacks took **fbfp_pct 54.5-58%** of
  real time in the movie.
- **Fix (35a6343):** the compiled quantized path takes an address outside MEM1 to an out-of-line locked-cache check
  (lc_remap) and continues on its host copy; only while the BATs map 0xE0000000 there (TP: yes). Fuzzer: opt 4723
  standard + chain seeds 1-8 0 fails; planted bug 12 caught.
- **Job 273 (600 s):** locked-cache fallbacks 0, fbfp_pct 0.0, **movie 4.1% -> 8.6% speed (2.1x)**, guest
  instructions 12.7 -> 25.7 M/s; cutscene 21.4%, later stretch 13.6% (unchanged).

## How much compiled code core 0's time is spread over (job 271, `--profile on`, device)

New `# code footprint 10s` line (jitsample.c, jit_footprint): the sampler's block hits sorted by count, and the fewest
blocks that cover each share of core 0's time in compiled code, with their host bytes (a block's bytes run to the next
block's start, exit stubs included). The profiled run is slower, so its sections sit later (cutscene to ~245 s, later
stretch ~250-445 s, movie from ~455 s). Typical 10 s report:

| Section | 50% of time | 80% | 90% | 99% | all sampled |
|---|---|---|---|---|---|
| Cutscene | 100-180 blocks, **46-73 KB** | 875-1,285 blocks, 209-284 KB | 2,100-2,850, 400-505 KB | ~6,000-7,000, 840-970 KB | ~7,000, ~1 MB |
| Later stretch | **14 blocks, 8 KB** (ShdwDraw) | ~300, ~100 KB | ~1,000, ~230 KB | ~4,750, ~705 KB | ~5,500, ~790 KB |
| Movie | **4 blocks, 12 KB** (the psq_st fallback loop) | ~28, ~40 KB | ~140, ~80 KB | ~2,000, ~350 KB | ~3,000, ~470 KB |

- **The cutscene's hot code is about 4x the 16 KB L1 I-cache for half its time, and ~16x for 80%.** No layout puts it
  in L1; every guest instruction streams its host code from L2 (~28 cycles a line). With the straight-line probe (code
  past 16 KB: 4.4 vs 1.3 cycles per guest instruction), this makes fetch a structural cost of the cutscene: it falls
  with host bytes executed per guest instruction (code density), not with placement.
- The later stretch and the movie are each dominated by one small piece of code (ShdwDraw; the psq_st fallbacks),
  which stand-ins and JIT fixes can remove.

## A/B verdicts re-scored with phase pairs (2026-10-16)

Row-level standard errors understate the cutscene noise 2-3x (rows are autocorrelated). `core/tools/ab_split.py` now also
prints a **phase-pair** line per section: each A/B phase is one sample, non-overlapping adjacent phases are paired, and a
t interval (95%) is taken over the pairs' relative core-0 differences. Calibration on six runs with no A/B (101, 130, 157,
184, 191, 193) under imposed 20 s schedules, five offsets each: **0 of 45 null intervals exclude zero** (honest, slightly
conservative). Null half-widths: cutscene +-1.3-2.9% (6 pairs), later stretch +-0.8-1.0% (5 pairs). Real A/B runs are
wider (each switch recompiles): **one 600 s run resolves only effects above ~+-3-5% in the cutscene and ~+-1.5-3.5% in
the later stretch.** Smaller effects need replicates pooled by pairs.

Sections used for re-scoring: cutscene 29-250 s, later 311-475 s of real time (inside the section for both JIT- and
GX-toggle runs); duplicate rows dropped; the A/B column found from the data. Core 0 = speed / (1 - gxwait). Device data.

| Job | A/B | Cutscene (95% CI) | Later (95% CI) | Re-scored verdict |
|---|---|---|---|---|
| 117 | opt 1 vs 0 (check hoisting) | +4.83% +-3.34 | +2.30% +-1.53 | **stands** (gain) |
| 167, 168 | faultmem 1 vs 0 | +7.52 +-3.95; +7.41 +-4.79 | 168: +3.83 +-2.53 | **stands** (gain; kernel stability open) |
| 163 | opt 627 vs 515 | +3.55 +-3.70 | +5.61 +-1.53 | later **resolved** gain |
| 151 | opt 3 vs 115 | -3.20 +-3.62 | -3.60 +-0.58 | 115 better, later **resolved** |
| 148 | opt 3 vs 19 | -1.81 +-4.06 | -2.29 +-2.10 | 19 better, later resolved (marginal) |
| 149 | opt 35 vs 3 | +2.98 +-3.98 | +1.87 +-1.69 | later resolved (marginal) |
| 122, 147, 154 | traces 1 vs 0 | -5.61 +-2.93; -1.22 +-3.15; -4.89 +-4.93 | 154: -1.18 +-2.07 | loss in 122; others within noise |
| 195 | relayout 1 vs 0 | -0.40 +-3.18 | -4.47 +-3.25 | later **resolved** loss |
| 144 | aot 1 vs 0 | -43.97 +-1.07 | -33.6 +-46.5 | **stands** (loss) |
| 125, 126, 127 | vw2 1 vs 0 | -8.34 +-5.47; -9.10 +-9.08; -8.40 +-5.82 | - | **stands** (loss) |
| 116, 118, 119 | dlcache 1 vs 0 | -5.26 +-4.67; -4.49 +-4.25; -4.63 +-4.34 | - | **stands** (loss); 113-115 within noise |
| 164 | steal 1 vs 0 | -3.81 +-6.31 | -2.28 +-1.49 | later **resolved** loss |
| 128 | fskip 1 vs 0 | +10.78 +-7.39 | - | resolved (frame skip, not a speed-up of the work) |
| 131-134 | opt 7 vs 3 (fcache) | +0.33..+2.65, each +-3.7-4.3 | +0.35..+1.77, each +-2.5-4.2 | **unresolved** (C11's rejection and today's gain both within noise) |
| 120, 121, 123, 124, 129, 145, 146, 150, 155, 156, 160-162, 166, 169-171, 190 | pld, merge, sched, splitopen, opt bits, hle, vin | all within +-3-5% | all within +-1-5% | **unresolved** |

Re-score: `python3 core/tools/ab_split.py core/out/relay_<job>.csv --col <A/B column>` (phase-pair line), sections as above.

## Full inventory by area

Each entry: metric: value (conditions) [trust; when] source. Caveats and supersessions follow in italics. Entries are
as the readers recorded them; where a headline correction above disagrees, the correction wins.

### Core-0 cycle budget (73)

- ARM instructions per guest instruction (device); memory share of guest instructions: **~13 ARM instructions per guest instruction; memory instructions ~40% of guest instructions** (2DS, TP) [device; 2026-10-13] docs/research/PERF_LEVERS.md:16-17 *Compare 6.4 hot words/guest instr (PC JIT_REGSTAT), 8.0 (qemu model), 8.04 (qemu, inside JIT buffer only): different scopes (dispatcher/helpers included or not). Informs: Executed-instruction share of the 33-cycle budget*
- Baseline of 10-13 night defaults (job 130): **Cutscene 20.06% +-0.26, core 0 22.83% (gxwait 11.1%, valloc 21%, worker 55% busy); later stretch 11.80% +-0.05 (core 0 11.87%)** [device; 2026-10-13] git a73e15f diff (HANDOFF) *Informs: Reference baseline*
- Baseline of 10-14 defaults (jobs 152-153): **Core 0 cutscene 25.56 / 25.61% (+-0.21); speed ~21.3%** [device; 2026-10-14] git ede002f diff *Informs: Reference baseline*
- Baseline of new defaults (jobs 152-153, 120 s): **core 0 cutscene 25.56 / 25.61% (+-0.21); speed ~21.3%** [device; 2026-10-14] docs/HANDOFF.md L1021
- Baseline of night defaults (job 130, tp850, 600 s, no A/B): **cutscene 20.06% +- 0.26, core 0 22.83% (gxwait 11.1%, valloc 21%, worker 55% busy); later stretch 11.80% +- 0.05 (core 0 11.87%)** [device; 2026-10-13] docs/HANDOFF.md L960-962
- Bottom-screen console drawing cost: **~13 lines/s took 2.6% of core 0** (perf_log share in # core0 outside compiled code) [device; commit 9b96050] core/host3ds/main.c:116-118 *Informs: Console drawn by a thread on core 1*
- Branch misprediction share of core 0 (FINDINGS): **under 1% of core 0** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:81 *Second-hand. Informs: Branch predictor modelling is low priority*
- Check hoisting (OPT_HOIST): **Job 117: core 0 cutscene 21.63 -> 22.72% (+5.0%), later 10.75 -> 11.25% (+4.7%); job 97: +5.7%** [device; 2026-10-13] git 9202ea9 body *Informs: On by default*
- Core 0 (JIT) speed in its own right, after frame skip removed most of the GX wait: **~23% in the tp850 cutscene, ~11% in the later (Link/Epona) stretch; core 0 waited on GX only ~5%** (TP tp850 save state, frame skip on (job 128), 2DS 804 MHz) [device; 2026-10-13, jobs up to 130] memory/gc3ds-region-jit-next.md:12-13 (job 128) *Superseded in part by later defaults (retstub/syscall/hlelink +2-4%, native2, shadow-scan HLE +12-17%), so current core-0 speed is somewhat higher Informs: Core 0 is the wall; reaching 30-40% needs JIT work, not GX work*
- Core 0 budget, clean run (job 184): **20.5% speed; GX wait ~11%, instruction-fetch stalls 21%, data stalls 22%, real dispatcher <1%, compiling ~0.5%; A/B runs recompile at each switch and ran at 15.5%** [device; 2026-10-15] docs/HANDOFF.md ~l.1142 (git fcc2d35 / 3e4c041) *Informs: The core 0 wall: fetch + data stalls ~43%*
- Core 0 cycle breakdown (relay 83-84, d0b37d79 era): **~802M cycles/s; 262-294M instr; fetch stalls 195M (24%, 6.4M I-cache misses/s); data-dependency stalls 173M (22%) with only ~1.3M D-cache misses/s; mispredicts ~0.5M/s; TLB misses 127k/s** [device; 2026-10-13] docs/HANDOFF.md L828-833 *Informs: data stalls are interlocks, not memory -> scheduling model*
- Core 0 cycle split in the cutscene (PMU, clean run): **instruction-fetch stall 24.4 %, data stall 22.7 %, issue/other 52.8 %; IPC 0.322; 803 Mcycles per 1 s row** (opt 115 defaults, tp850 cutscene) [device; job 157] core/out/relay_157.csv via core/tools/frame_budget.py --from 30 --to 250 *PMU rotates event pairs, one pair per row; each event is averaged over ~42 rows. Job 101 (older build): 26.1 / 22.6 / 51.3 % Informs: Fetch and data stalls are ~47 % of core 0 in the GX-heavy section*
- Core 0 cycle split in the later core-0-bound stretch: **instruction-fetch stall 15.4 %, data stall 18.8 %, issue/other 65.8 %; IPC 0.476** (Link/Epona stretch, gxwait ~0.5 %) [device; job 157] core/out/relay_157.csv frame_budget.py --from 265 --to 475 *Job 101: 14.2 / 19.6 / 66.2 % Informs: Where core 0 is the wall, issue work (instruction count) dominates, not stalls*
- Core 0 cycle split in the movie section (THP playback): **fetch stall 21.3 %, data stall 15.0 %, other 63.7 %; IPC 0.459; 27.7 host instructions and 60.4 cycles per guest instruction** (emulated ~96.5 s onward, 12-17 draws/s) [device; jobs 101, 157] core/out/relay_157.csv frame_budget.py --from 510 --to 600 (job 101: 27.9 / 60.4) *Informs: Movie playback is a separate core-0 problem (interpreter fallbacks)*
- Core 0 cycles per guest instruction while not waiting, and breakdown: **~33 cycles: ~13 executed ARM instructions, ~9.7 instruction-fetch stall cycles, ~8 data stall cycles** (2DS PMU, TP) [device; HANDOFF 2026-10-12/13 (cited)] docs/OPT_JIT_PLAN.md:23-25; REDESIGN_CANDIDATES.md:152-153,170-171 (C11, C12) *Cited from HANDOFF.md, not read here. Components sum to ~30.7, not 33; ~2 cycles unattributed. Informs: The core-0 wall; which component (fetch vs executed vs data) a JIT change must cut*
- Core 0 IPC and cost per guest instruction: **IPC 0.36; 9.3 host instructions and 25.8 cycles per guest instruction** (TP tp850) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:189 *Informs: Target is ~5-8 cycles/guest instr: both host instruction count and stalls must fall*
- Core 0 outside compiled code (3605dfd, title) and successive builds: **dispatcher 6.4-6.8%, perf_log 4.5-4.7% (perf.csv SD flush on core 0); after moving logging to core 1: perf_log 4.6 -> 0.0%; d7ac114 19.83/19.86%, 9b96050 20.10/20.13%, afe3fc0 20.73/20.69%, a1dd311 21.01/21.04% (cutscene/title, relay 64-71)** [device; 2026-10-12] docs/HANDOFF.md L755-763
- Core 0 PMU (862fe63): **767 Mcycles/s, IPC 0.31, 11.7M I-cache misses/s, 340M cycles/s (44%) fetch-stalled, 120M data-hazard, D-cache read misses 0.56M/s** [device; 2026-10-11] docs/HANDOFF.md L449-451 *each counter pair live one second in five*
- Core 0 PMU before the slow-path stubs (tp850 cutscene and title): **771-773 M cycles/s. 232-242 M instructions/s (IPC about 0.30). I-cache misses 10.05-10.13 M/s. Fetch stalls 306-311 M cycles/s (about 40%). Data stalls 119-121 M (15%). D-cache read misses 0.61 M/s. Mispredicts 0.28-0.48 M/s. TLB misses 88-98 k/s.** (tp850 cutscene / title) [device; relay jobs 7-8, 2026-10-12] core/out/relay_7.csv, relay_8.csv *Informs: The I-fetch-dominated starting point. Fetch stall share later fell from 40% to 24%.*
- Core 0 PMU breakdown, cutscene (d0b37d79 era): **804 M cycles/s. 264 M instructions/s (IPC 0.33). Fetch stalls 196 M (24%). Data stalls 172 M (21%). I-cache misses 6.47 M/s. D-cache read misses 1.09 M/s, write misses 0.27 M/s. Branches 44.5 M/s, 0.51 M mispredicted. TLB misses 129 k/s. LSU stalls 14.7 M. SCU reads 14.4 M/s. The title (job 84) is within 2% on every counter.** (tp850 cutscene / title, heavy rows) [device; relay jobs 83-84, 2026-10-12] core/out/relay_83.csv, relay_84.csv *TLB misses read 338 k/s in job 97's cutscene: not explained Informs: Core 0 is stall-bound: fetch 24% plus interlocks 21%. D-cache misses are too few to explain the data stalls.*
- Core 0 PMU in TP play (per second): cycles; host instructions; IPC: **804 M cycles/s; ~260 M host instructions/s; IPC ~0.32** (tp850 cutscene stretch, clean run) [device; 2026-10-15, job 184] relay_184.csv columns pmu_mcyc, inst_m (30-200 s windows) *Event pairs rotate per row: each event is sampled one row in five. Informs: Core 0 issues instructions only about a third of the time; stalls dominate*
- Core 0 PMU on title (cbff68c): **IPC 0.36; 6.4M I-cache misses/s; 143M cycles/s stalled on instruction fetch; 188M cycles/s data-hazard stalls; 383k mispredicts/s** [device; 2026-10-11, cbff68c] docs/HANDOFF.md L142-143 *I-cache figures here are lower than the ~11-12M/s measured hours later on later builds*
- Core 0 PMU on title after level 2 (da2aefd era): **769M cycles/s, 251M instructions (IPC 0.33), 323M cycles stalled fetching (42%), 11.8M I-cache misses/s (~0.85 per guest instr, ~27 cycles each), 127M data-hazard cycles; core 0 waits GX ~10%; sampling: top block 0.2-0.3%, top 16 ~2%, 14-20% outside compiled code** [device; 2026-10-11, da2aefd] docs/HANDOFF.md L232-236 *Informs: footprint per guest instruction matters, not one routine*
- Core 0 time outside compiled code (device sampler): **job 93 windows: dispatch 10.0%, 10.8%, 15.1%; compile 19.0% (first 10 s after state load), 3.3%, 0.9%; gekko_run/sys_run/events/gx_poll each 0.2-0.5%; perf_csv 0.4-0.7%; hot guest samples outside compiled code 16.2-31.0%** (2DS cutscene) [device; 2026-10-13] relay_93a.csv '# core0 outside compiled code 10s' and '# hot guest code' lines *Sampler files idle-hook waits as 'dispatch' (HANDOFF line 876-877). Informs: after warm-up, core 0 is ~85% in compiled code; dispatcher 10-15% is the main non-JIT cost*
- Core 0 time outside compiled code (sampler): **cutscene: dispatch 10.1 % (the idle hook busy-waiting for the GX thread), compile 0.1 %, events 0.5 %, sys_run 0.4 %, gx_poll 0.3 %, gekko_run 0.3 %, perf 0.8 %; later: dispatch 1.3 %, events 0.3 %** (clean run) [device; job 157] core/out/relay_157.csv '# core0 outside compiled code 10s' lines *'dispatch' is mostly GX wait, not dispatcher work (HANDOFF 2026-10-15) Informs: Dispatcher/compile overhead is ~1 %; GX wait is the visible non-JIT cost in the cutscene*
- Core 0's own speed, speed/(1-gxwait): **~22-24% in the cutscene (A/B arms 21.2-23.6%)** (2DS tp850 cutscene; perf_heavy.py) [device; 2026-10-13..15] docs/research/PERF_LEVERS.md:9-10; OPT_JIT_PLAN.md:15-17; REDESIGN_CANDIDATES.md:120,148-150 *Derived metric; assumes gxwait time would convert 1:1 into core-0 progress. Informs: Judging JIT changes independent of the GX cap*
- Core <-> VFP register move cost: **round trip 5 cycles latency / 5 throughput (run 2); ~3.5-4 cycles each way, not overlapping (run 3)** (2DS gcbench) [device; run 2 / run 3 2026-10-10] docs/FINDINGS.md:39,46-48,151-153 *Informs: Float load/store stub design (C16, C18)*
- Core<->VFP register move cost: **~3.5 cycles, does not overlap (run 3); 3.5-4 cycles each way (jit-rebuild note)** (gcbench) [device; 2026-10-10] memory/tp3ds-gc-wii-on-2ds.md:41; memory/gc3ds-jit-rebuild-after-gx.md:15-16 *FINDINGS run 2 lists a 5-cycle round trip Informs: Float loads should go straight into VFP (OPT_BEFLOAT), avoid ldr->rev->vmov*
- Cost per guest instruction by section: **cutscene 35.1 cycles, 11.3 host instructions; later 13.3 cycles, 6.35 host instructions; movie 60.4 cycles, 27.7 host instructions** (clean run windows) [device; job 157] core/out/relay_157.csv frame_budget.py (job 101: 36.1/11.2, 14.2/6.55, 60.4/27.9) *Guest count from ins_k; host count from PMU Informs: Per-instruction cost differs 2.6x between sections*
- Dispatcher and sampler on core 0 in TP play: **indirect-table misses ~3,360/s; budget ends ~980/s; other returns ~4,090/s; gekko_run slices ~3,380/s. Sampler 'dispatch' 10-13% of core 0, 'compile' 0.6-0.8%, events 0.4%, gx_poll 0.2%. Hot-code sampler: 2.5-26.5% of samples outside compiled code (typ. ~14%).** (tp850) [device; 2026-10-15, jobs 184/191] relay_184.csv / relay_191.csv averaged '# dispatches/s', '# core0 outside compiled code', '# hot guest code' lines *The hot-code sampler reports only the top guest PC per 10 s: too coarse for a function-level profile. Informs: The real dispatcher is <1%. The 'dispatch' bucket is mostly jit_entry idle while waiting for GX.*
- Dispatcher share in play: **<1%; sampler's 'dispatch' was the idle hook's GX wait** (tp850 gameplay) [device; 2026-10-15] memory/gc3ds-jit-dead-ends.md:35 *Earlier sampler readings over-attributed time to dispatch Informs: No dispatcher redesign needed*
- Emulated TP speed after the 10-14 / 10-15 JIT work: **~22% of full speed (cutscene); ~12.6% (later stretch); each JIT step worth a few percent** (TP, emulation mode, HANDOFF summary) [device; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:9-10 *Summary quoted from HANDOFF; scenes differ. The task framing says ~20%. Informs: Core 0 emulating Gekko is the wall; motivates native mode*
- Fault mode without in-handler sync (jobs 167-168): **With the sync moved out of the handler the kernel crash recurred after 6,166 faults (job 167). With no patch sync (job 168): +3.9% in the core-bound stretch, 600 s through 8,011 faults without a crash** [device; 2026-10-15] git e05e7a4, e2d52e9 bodies *One crash-free run is not proof of stability. The owner decision on fault mode is pending. Informs: Fault mode (+4-11% core 0) blocked on kernel stability*
- Fault-mode memory access, first device A/B (relay 88): **19.44% vs 19.53% speed (no change), but core 0 ~11% faster, hidden in gxwait; the kernel crashed (supervisor prefetch abort) delivering a fault; core 0 then 24% fetch stalls, 22% interlocks** [device; 2026-10-13] git 0414e4f, 531e4de bodies *Informs: Fault mode stays off (crash)*
- Frame skip experiment (C9), job 128: **Speed 19.81 -> 21.80% (+10%, ~3 sigma); core 0 own speed 22.33 -> 23.29 (noise); core 0 wait for GX 10.2 -> 5.4%; worker 54 -> 30% busy; first 110 rows read +21% (heavier early cutscene)** [device; 2026-10-13] git 0b1251e diff *Informs: With half the vertex work gone, core 0 is the wall at ~23%*
- gcbench guest MIPS, interpreter vs JIT: **integer loop 22 vs 329 MIPS (68% of Gekko); scalar float 19 vs 100 (21%); paired singles 9.5 vs 55 (11%); native C same loop 658 MIPS (135%)** (2DS gcbench run 2, 804 MHz) [device; run 2] docs/FINDINGS.md:16-21 *Synthetic loops, fit in L1; real TP code is ~33 cycles per guest instruction. Informs: Float/paired code is the expensive class*
- Guest exceptions and interpreter fallbacks per second in TP play: **syscalls (c00) ~3,450/s (also the top fallback, sc); external interrupts ~161/s; FP unavailable ~18/s; decrementer ~6/s; mftb fallbacks ~1,200/s; rfi ~450/s; mtspr ~250/s** (tp850 cutscene) [device; 2026-10-15, job 184] relay_184.csv averaged '# exceptions 10s', '# fallbacks 10s' lines *Informs: System-call / exception paths are frequent enough to matter for HLE or fast-path work*
- Guest MIPS, interpreter vs JIT (gcbench run 2): **integer 22 -> 329 MIPS (68% GC); scalar float 19 -> 100 MIPS (21%); paired 9.5 -> 55 MIPS (11%); native C same loop 658 MIPS (135%)** (New 2DS XL 804 MHz) [device; 2026-10-10, gcbench run 2] C:/Users/tyler/TP3DS/emu/docs/FINDINGS.md:16-21 *Read only as a cross-check of the memory note; other agents cover FINDINGS.md Informs: Ceiling of JIT vs native code (native is 2x JIT on integer) -> supports native mode*
- JIT level 2 linking A/B on title (cbff68c): **9.2% with linking vs 8.0% linking off (same run); core 0 waits for GX thread 53% with linking vs 15% without; GX thread core 2 78% busy: vertices 39% (gather 11%, lighting 2%), GPU sync 14%** (title scene, perf_2026-10-11_jit2.csv) [device; 2026-10-11, cbff68c] docs/HANDOFF.md L134-137 *Informs: after linking, GX thread became limit*
- JIT speed vs Gekko clock (gcbench run 3): **74% integer, 23% float, 14% paired singles** (gcbench microbenchmarks, 804 MHz, L2 on, SETEND BE) [device; 2026-10-10, gcbench run 3] memory/tp3ds-gc-wii-on-2ds.md:40 *Run 2 in docs/FINDINGS.md gives 68% / 21% / 11%; MEMORY.md index line says 68% int / 25% float. Three slightly different figures; % of Gekko clock is a pessimistic bound, not a frame rate Informs: Float and paired-single code is the weak spot (C8 single precision, SETEND float stubs)*
- JIT-only reference (job 142): **~20% cutscene, ~11% after, minute by minute equal to job 134; texture decodes ~0/s** [device; 2026-10-14] git 92ac7af body *Informs: Baseline*
- Job 93 steady state (cutscene, no A/B): **speed 21.55% (n=185, sd 1.94), fields 12.92/s, field_ms_avg 77.5, gxthread 81.0%, gxwait 9.3%, vwork 61.9%, vwait 25.6%, rest 90.0%, core-0 speed speed/(1-gxwait) 23.76%** (2DS, tp850.state, sched on, indirect table 64K) [device; 2026-10-13] relay_93a.csv *Duplicate of core/out/relay_93.csv. Informs: both core 0 and the GX thread (core 2) are near their limits at ~21%*
- Once-a-second perf.csv flush on core 0: **4.5-4.7% of core 0** (synchronous SD writes) [device; commit 3605dfd] core/host3ds/asyncfile.c:1-3 *Informs: Async file writer on core 1*
- perf.csv written from core 1 instead of core 0 (d7ac114): **Cutscene 20.06%, title 19.92%. Core 0's perf_log share fell from 4.5-5.9% (jobs 52-59) to 0.0% (job 70 on).** (Separate runs) [device; relay jobs 64-65, 2026-10-12] core/out/relay_64.csv, relay_65.csv; '# core0 outside compiled code' in relay_52, 59, 70 *Informs: Keep instrumentation I/O off core 0*
- PMU split: compiled code (with the helpers it calls) as a share of core 0: **Cycles 79-95%. Instructions 73-95%. I-cache misses 89-101%. Fetch stalls 86-99%. D-cache read misses 51-85%. Data stalls 58-94%. TLB misses 93-98%.** (--pmu-split on, runs sampled 1 in 8) [device; relay jobs 46-47, 58, 2026-10-12] core/out/relay_46.csv, relay_47.csv, relay_58.csv ('# pmu split 10s', two lines sampled each) *--pmu-split slows the run (job 58 cutscene 17.69% against about 18.0%) Informs: Fetch cost sits in JIT-emitted code, not in the emulator's C*
- Speed of the later core-0-bound stretch of tp850 (~300 s in): **~11% (A/B arms range 11.0-12.8%)** (2DS tp850, later stretch, core 0 the only limit) [device; 2026-10-13..14] docs/research/PERF_LEVERS.md:10-11; REDESIGN_CANDIDATES.md:38-43,149-150; AOT_PLAN.md:76,86 *Varies by job/build: 11.13-11.75 (jobs 125-134), 11.0-11.4 (job 147), 12.8 (job 144 JIT arm). Informs: Second scene in every A/B; shows core-0-only effects*
- The core-0-bound later stretch of tp850 (311-600 s): **11.46% speed, 6.87 fields/s. 55.6 M guest instructions/s = 486.0 M per emulated second, exactly Gekko's clock. Every emulated cycle runs a guest instruction, with no idle skipping: 8.1 M per field, 4.8x the cutscene. IPC 0.44-0.45 (352.8 M ARM instructions/s). I-cache misses 3.8 M/s. Fetch stalls 103 M cycles/s (13%). Data stalls 147 M (18%). gxwait 0.5%. 30.4k draws/s.** (tp850 state, 600 s) [device; relay job 97, 2026-10-13] core/out/relay_97.csv (311-700 s); the same pattern in r117b, r125b, r129b-r134b (11.5-11.9%, inst_m 340-380) *Not established whether this is a spin or poll loop or real game work. The 486.0 figure only shows the guest never idles in that stretch. Informs: Idle-loop or wait skipping (REDESIGN C10) could be a large lever here. At full speed this stretch needs 486 M guest instructions/s, 8.7x core 0 today.*
- TP core 0 budget, clean run (job 184): **20.5% speed; GX wait ~11% (idle hook busy-waits for core 2); instruction-fetch stalls 21%; data stalls 22%; real dispatcher <1%; compiling ~0.5%; A/B runs (recompile at each switch) ran at 15.5%** [device; 2026-10-15, frame_budget.py] docs/HANDOFF.md L1142-1145 *compare A/B phases only with each other Informs: dispatcher/compile are no longer levers*
- TP emulation speed in heavy scenes (perf.csv speed): **~20-21% of full speed** (2DS, tp850 cutscene / heavy rows, GX side (core 2 + worker) is the cap) [device; 2026-10-13] docs/research/PERF_LEVERS.md:9-11; REDESIGN_CANDIDATES.md:120 (C9) *Older baseline at 4c5012a was 16.62% (title heavy rows) / 15.89% (cutscene) (REGION_JIT_PLAN.md:83-84): superseded by later builds. Same-run A/B arms in later jobs read 19.8-20.4%. Informs: Baseline for all A/Bs; which side (core 0 vs GX) caps*
- TP shadow-scan loop (dBgWKCol::ShdwDraw cell/triangle scan) share of core 0: **~33% of core 0 samples** (TP title scene, jitsample profiler) [device; 2026-10-11] core/src/cpu/hle_tp.c:1-4 *Sampler attribution goes to the last block entered (jitsample.c). Informs: Native stand-in (hle_tp.c)*
- TP speed, clean run vs A/B runs: **20.5% full speed clean vs 15.5% in A/B runs** (tp850, 300 s, current defaults) [device; job 184, 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:179-180 *A/B runs recompile everything at each switch, depressing speed. Informs: A/B harness overhead must be accounted for*
- TP title scene speed and split, earliest 2DS profile: **3.0% speed; GX 46% (CPU vertex decode+transform 30%, textures 2%, GPU wait 4%), guest code 54%** (GX inline on core 0, JIT level 1) [device; 2026-10-10] docs/HANDOFF.md L73 *superseded by every later build; baseline only*
- Unattributed core 0 time: **23% of core 0 outside compiled code (dispatcher, events, GX poll, frontend, compile) not attributable; IPC 0.31, 44% fetch-stalled** [device; 2026-10-12] docs/HANDOFF.md L500-504 *g_where markers added afterward; later runs: dispatcher 6.4-6.8%, perf_log 4.5-4.7%*
- VFP short-vector pairs and int/float conversion: **short-vector pair add/mul/mla 9 latency / 2.3 throughput (no faster than two scalars); int<->float convert 8-9 / 1.1** (2DS gcbench run 2) [device; run 2] docs/FINDINGS.md:38-45 *Short vectors only save code size (relevant now that core 0 is fetch-bound; PERF_LEVERS lever 3). Informs: Lever 3 (VFP short vectors)*
- VFP11 latency / throughput at 804 MHz (gcbench run 2): **f32 add/mul/mla 8 / 1.1; f64 add 8 / 2.0; f64 mul/mla 9 / 2.3; f32 div/sqrt 19 / 17; f64 div 33 / 31; short-vector pair 9 / 2.3; int<->float 8-9 / 1.1; core<->VFP round trip 5 / 5 cycles** (gcbench) [device; 2026-10-10, run 2] C:/Users/tyler/TP3DS/emu/docs/FINDINGS.md:29-40 *Cross-check only Informs: Cost model for float JIT design (C8 single precision, keep floats in VFP)*
- VFP11 single vs double latency/throughput (cycles): **f32 add/mul/mla 8 / 1.1; f64 add 8 / 2.0; f64 mul/mla 9 / 2.3; f32 div/sqrt 19 / 17; f64 div 33 / 31** (2DS gcbench run 2, 804 MHz) [device; run 2] docs/FINDINGS.md:29-37 *Datasheet (ARM DDI 0360F, PERF_LEVERS.md:34-36): FMULS/FMACS 1/cycle, FMULD/FMACD 1 per 2 cycles, FADD latency 8, FCVT 4 — consistent with the device. Informs: C8 single precision: throughput gain ~2x on mul, latency mostly unchanged*
- ARM11 interlock model, baseline 20-field dump (sch3, fcache off): **2.90e8 host instructions, 1.15e8 stall cycles = 0.395 per instruction. 4.62e7 guest instructions: 6.29 host instructions + 2.48 stall cycles per guest instruction (8.77 modelled cycles). Stall producers: load 54.9%, VFP 27.0%, ARM->VFP 8.5%, VFP load 5.6%, ALU 3.4%.** (TP cutscene 850->870, traces on, fault mode off, JIT_REGSTAT stats build (counters stripped by the script); in-order issue model, hot code only, fall-through paths) [qemu_model; 2026-10-13 16:00] //wsl$/Debian/root/sch3/model.txt lines 1-15 (reproduced exactly by re-running core/tools/arm11_sched.py on sch3/sched.bin) *No instruction fetch, no cache misses, no exit stubs, no C code Informs: What interlocks cost per guest instruction; the baseline for the FPR-lanes (fcache) A/B*
- ARM11 model, 40-field dumps (/root/sched.bin, /root/sched2.bin): **Both: 4.77e8 host instructions, 1.39e8 stall cycles = 0.291 per instruction. 5.94e7 guest: 8.04 host + 2.34 stall per guest instruction (10.38 cycles). Load 59.1%, VFP 27.6%. lwz 10.5 cycles (9.7 words) 13.6%; psq_l 35.8 (26.2) 8.0%; stw 9.6 (9.4) 7.4%; psq_st 49.3 (35.0) 6.1%; lfs 22.6 (16.7) 5.5%; bc 6.7 (6.2) 5.5%.** (TP cutscene 850->890, scheduled code (SCHED_STUBS) on) [qemu_model; dumps 2026-10-13 04:24 / 05:42; re-run 2026-10-16] re-run of core/tools/arm11_sched.py --cycles on //wsl$/Debian/root/sched.bin and sched2.bin (outputs in the session scratchpad); matches OPT_JIT_PLAN.md lines 18-22 *sched.bin and sched2.bin score identically, so both are post-scheduling dumps. The pre-scheduling dump (HANDOFF: 1.70e8 stall, ~0.34 per instruction) is not on disk. Not comparable with the later 20-field build (8.04 vs 6.29 host per guest). Informs: The OPT_JIT_PLAN breakdown (memory instructions ~60% of modelled cycles)*
- ARM11 model, fcache first pass (sch7): **0.388 stall per instruction; 6.29 host + 2.44 stall per guest (8.73 cycles, -0.5% vs sch3). Producers: load 55.8%, VFP 30.5%, VFP load 5.2%, ARM->VFP 4.0%. lfs 12.9 cycles, stfs 16.2, psq_st 37.3.** (OPT_FCACHE first pass, 850->870) [qemu_model; 2026-10-13 16:00] //wsl$/Debian/root/sch7/model.txt *Device job 131 (first pass): +1% / noise Informs: FPR lanes A/B history*
- ARM11 model, fcache second pass (schb7) vs baseline: **0.373 stall per instruction; 6.17 host + 2.30 stall per guest (8.47 cycles) vs sch3 8.77 = -3.4%. lfs 18.7 -> 12.9, stfs 22.4 -> 15.3, psq_st 39.0 -> 30.1, fadds 22.0 -> 12.7, ps_mul 31.4 -> 18.4.** (OPT_FCACHE second pass) [qemu_model; 2026-10-13 16:22] //wsl$/Debian/root/schb7/model.txt; REDESIGN_CANDIDATES.md lines 141-156 *Device jobs 132-134 (three 600 s runs): cutscene core 0 +4.3 / +3.5 / +3.0%, later stretch -2.7 / -2.9 / -4.4%; net zero, off by default Informs: A model gain that the device did not deliver: negative result for FPR lanes*
- Modelled cycles by guest opcode (sch3 baseline): **lwz 8.0 cycles (6.5 words), 12.53% of guest, 11.4% of cycles. psq_l 29.3 (20.3), 8.1%. lfs 18.7 (12.0), 6.5%. stw 6.0 (5.4), 4.7%. addi 2.7 (1.7), 15.26% of guest, 4.7%. stfs 22.4 (15.1), 4.2%. icbi 25.0 (21.0), 3.8%. psq_st 39.0 (26.6), 3.8%. fmuls 26.4 (10.7), 3.3%. lfd 14.6 (11.3), 3.1%. bclr 8.3 (6.4), 3.0%. b 5.8 (5.1), 2.9%. bc 2.6 (2.0). fcmpo 20.3 (14.4). ps_madds0/1 ~29. bcctr 13.4.** (As sch3) [qemu_model; 2026-10-13] sch3/model.txt lines 17-56 *icbi's share may be a post-state-load burst (identical count in every run) Informs: Memory and float instructions dominate modelled cycles; paired-single and float load/store paths cost 19-39 cycles each*
- Re-score of sch3 (baseline) with job-183 device latencies: **Variant A (rev 2, mul 5, VFP->ARM 6): 1.26e8 stall = 0.434 per instruction, 2.73 stall per guest (total 9.02 cycles, +2.9%). Variant B (rev 2, mul 5, ARM->VFP 8): 1.40e8 = 0.481, 3.03 per guest (9.32 cycles, +6.3%). Original 0.395 / 2.48 / 8.77. lwz 8.0 -> 8.3 cycles; psq_l 29.3 -> 30.3 / 34.5; lfs 18.7 -> 19.7 / 23.7.** (No new emulator run; same dump) [qemu_model; 2026-10-16 (this session)] scratchpad copies sched183a.py / sched183b.py of core/tools/arm11_sched.py (only the LAT_ lines changed) run on //wsl$/Debian/root/sch3/sched.bin *The vmov round-trip split is unknown, so A and B bracket it. VFP load latency is still unmeasured on the device. Informs: Calibration error in the interlock table is ~3-6% of modelled cycles: small next to the unmodelled fetch and miss terms*
- Re-score of the 40-field dump with job-183 latencies: **0.291 -> 0.312 (A) / 0.351 (B) stall per instruction; 2.34 -> 2.51 / 2.82 stall per guest; total 10.38 -> 10.55 (+1.6%) / 10.86 (+4.6%) cycles per guest instruction** (As the 40-field dump) [qemu_model; 2026-10-16] scratchpad re-runs on //wsl$/Debian/root/sched.bin (sched2.bin identical with the original and variant A) *As above Informs: Same conclusion on a second dump*
- Branch mispredicts and branches on core 0 in TP: **0.54-0.80 M mispredicts/s (~0.5% of cycles at ~7.5 each); 37-47 M branches/s** (tp850) [estimate; 2026-10-15, job 184] relay_184.csv columns brmiss_k, branch_m *The cycle share is derived from the probe's 7.5-cycle penalty. Informs: Mispredicts are not a lever*
- Core 0 ARM instructions and cycles per guest instruction (device-derived): **265.6 M ARM inst/s for 21.9 M guest/s = ~12.1 ARM instructions and ~36.6 core-0 cycles per guest instruction; full speed (~102 M guest/s) needs ~7.9 cycles per guest instruction at 802 Mcycles/s, a ~4.6x gap** (job 93, cutscene, no A/B, sched on, indirect table 64K) [estimate; 2026-10-13] relay_93a.csv (inst_m, pmu_mcyc, ins_k means) *Ratio of averages; inst_m sampled one second in five (rotating pairs); core-0 instructions include dispatcher, helpers and GX submission on core 0. Informs: tuning cannot close a 4.6x gap; redesign scale*
- Modelled core-0 cycles per emulated field (20-field dump): **(2.90e8 host + 1.15e8 stall) / 20 fields = ~20.3 M cycles per field for hot JIT code alone, against 13.4 M cycles per field at full speed (804 MHz / 60)** (TP cutscene, 2.31 M guest instructions per field by JIT_REGSTAT) [estimate; computed 2026-10-16] derived from sch3/model.txt *Ignores fetch, misses and C. At ~20% speed the device spends ~65 M cycles per field. Informs: Even with zero fetch and miss stalls, today's code is ~1.5x over the full-speed budget, so full speed needs fewer executed instructions as well as smaller code*
- Modelled share of core 0 cycles: float loads/stores and float math: **float ld/st 27.6% of modelled cycles; float math 15.3%** (Model (interlock scheduler / qemu trace), not PMU) [estimate; 2026-10-15] memory/gc3ds-jit-rebuild-after-gx.md:16-20 *Model share; OPT_BEFLOAT then measured neutral on device (job 166) so the model over-predicted Informs: Ordering of OPT_BEFLOAT then C8 single precision*
- Predicted TP speed ceiling from core-0 gains with current GX side: **~25-26% (where the GX thread saturates)** (estimate from gxwait) [estimate; 2026-10-13] docs/OPT_JIT_PLAN.md:16-17 *Later fskip result (job 128) suggests core 0 is the wall at ~23% once GX is relieved. Informs: How much a core-0 gain can show*
- Remaining exact-only JIT items: **~1% each; native mode needs ~5-8x** (Judgement from measured A/Bs) [estimate; 2026-10-15] memory/gc3ds-timing-drift-approved.md:15; memory/gc3ds-native-mode-plan.md:34 *Informs: Pivot to native mode (decomp port) for full speed*
- Share of device core-0 cycles the interlock model covers: **Model 8.8-9.3 cycles per guest instruction (hot code issue + interlocks) against 25.8 device cycles per guest instruction (job 184), about 34-36%. The device runs 9.3 host instructions per guest instruction against 6.29 modelled hot words.** (Different windows and builds (10-13 dump vs 10-15 device run)) [estimate; computed 2026-10-16] derived: sch3 model + re-score vs TIMING_MODEL_PLAN.md lines 177-189 *The rest is fetch stalls (~21%), data misses (~22% incl. L1-hit waits), GX wait (~11%), stubs, C and the idle hook Informs: A redesign judged only by this model sees about a third of core 0. The I-cache and D-cache simulators are needed to price the rest.*
- State of both cores at ~21% (2026-10-13 summary): **core 0 98% busy (fetch stalls 24%, interlocks ~20%, dispatcher 5-12%); GX thread 80% with 9-16% waiting on worker; 80% needs ~4x on core 0 and core 2; this JIT ~13 ARM instr/guest instr vs Dolphin-class ~4-6** [estimate; 2026-10-13] docs/HANDOFF.md L864-870 *Dolphin 4-6 figure not measured here Informs: tuning cannot reach 80%; optimizing JIT, game patches, or native mode*
- Throughput gap to 80% speed: **80% needs ~79M guest instr/s vs ~21M now (~3.9x); core 0 runs ~13 ARM instr per guest instr at IPC 0.33 (25% fetch stalls, 21% data-hazard stalls)** [estimate; 2026-10-12, a1dd311] docs/HANDOFF.md L766-768 *derived from 1.65M instr/field and device counters*
- L2 + 804 MHz effect on a JIT (DaedalusX64-3DS user report): **performance reportedly dropped with L2 + 804 MHz on (cause unknown)** (third-party user report) [unknown; 2026-10-10] docs/ARCHITECTURE.md:278-281 *The doc says 'worth an A/B on our own JIT'. No gc3ds A/B result appears in these files. Informs: Clock/L2 configuration*

### JIT code generation (311)

- 'Other' exits by instruction, first 10 s after loading the state: **icbi 622,556 in the load window; afterwards per 10 s b ~25-30k, mtmsr ~5-6k, bclr ~3-4k, mtspr ~2.5-3k, lwz ~1.6-2k; with syscall off sc 16-35k** (every run from job 157) [device; jobs 157-170] '# other exits after 10s' lines, relay_157-170.csv *Informs: icbi matters only while loading (OPT_ICBI later neutral)*
- 'Stack' A/B (retstub + syscall + hlelink together): **core0 cutscene 24.47 -> 25.00 (+2.2 %), later 12.04 -> 12.54 (+4.2 %); later I-misses 5.15 -> 4.42 M/s (-14 %)** (600 s) [device; job 151] core/out/relay_151.csv, ab_split.py --col opt (3 vs 115) *Informs: Default on*
- 'Stack' re-check with NATIVE2 on (opt 515 vs 627): **core0 cutscene 24.64 -> 25.31 (+2.7 %), later 11.93 -> 12.63 (+5.9 %); later I-misses 5.51 -> 4.34 M/s (-21 %)** (600 s; HANDOFF lists job 163 as the GX-side look at defaults) [device; job 163 (2026-10-15)] core/out/relay_163.csv, ab_split.py --col opt *Larger than job 151's later-stretch effect Informs: Replicates the stack gain*
- 2-way indirect table (OPT_IND2, job 161) and FAST_BITS=13 (job 193): **slot-taken misses 3.7k -> 1.2k/s, speed unchanged; denser table job 193 neutral** [device; 2026-10-14 / 2026-10-15] docs/HANDOFF.md L1048-1050, L1168 *Informs: a dispatcher round trip is cheap; off*
- A/B OPT_ICBI: icbi inside blocks (job 186 off vs 187 on): **speed mean 19.36 vs 19.33, median 19.40 vs 19.39; load window's 622,556 icbi exits per 10 s unchanged in both** (tp850, 60 s each) [device; 2026-10-15, jobs 186/187] relay_186.csv, relay_187.csv (opt column 4723 vs 12915; '# other exits after 10s') *Informs: Rejected/neutral: stays off (only matters while loading). It is also unexplained why the 2DS still exits on each icbi.*
- A/B OPT_LATCH (job 171, opt 627 off vs 4723 on, 20 s phases): **Cutscene 30-300 s median 21.28 vs 21.63; later 300-570 s 12.48 vs 12.47. Docs: cutscene 21.72 vs 21.51, later 12.51 vs 12.44. Verdict: small, kept as default.** (tp850, 600 s, A/B every 20 s) [device; 2026-10-15, job 171] relay_171.csv perf rows split on column 'opt' *My windowed medians differ slightly from the HANDOFF figures, whose window definitions I don't know. The effect is at the noise floor. Informs: OPT_LATCH stays on*
- A/B: FAST_BITS=13 + 2-way indirect table (--ind2) vs default: **speed 20.4% vs 20.4%; D-read misses 1.08 vs 1.04 M/s; indirect-table misses 1.8k -> 9.7k/s. Neutral, not adopted** (TP) [device; job 193 vs job 191] docs/research/TIMING_MODEL_PLAN.md:213-215 *Negative result. Informs: Indirect table is not a lever*
- A/B: instruction reordering (pseq vs pilp): **BE 55.4 -> 61.0 MIPS (+10%); sw 61.0 -> 67.9 (+11%); SETEND pilp 67.9; pilp sw vec 67.0 (vector mode gains nothing)** (gcbench 3, paired kernel scheduled sequentially (pseq) vs interleaved (pilp)) [device; 2026-10-10] bench/results/latest.txt L37-41 *Hand-scheduled kernels, not JIT output. The gain is capped by core<->VFP moves (3.5-4 cycles each, not overlapped). Informs: A scheduler buys about 10% on paired code. Avoiding core<->VFP moves matters more*
- A/B: SETEND BE data mode vs rev per access: **SETEND vs BE: int 361.7 vs 328.8 (+10.0%), float 112.8 vs 100.5 (+12.2%), paired 68.4 vs 59.8 (+14.4%). SETEND equals byte-swapped RAM to 0.1 MIPS. All 16 correctness checks OK** (gcbench 3, core 0) [device; 2026-10-10] bench/results/latest.txt L9-24, L27-45 *Verdict was adopted (FINDINGS run 3) Informs: Keep guest RAM in console byte order and run blocks in SETEND BE*
- AOT (C12) memory headroom and invalid A/B: **MEM1/ARAM/JIT leave malloc ~0.4 MB; job 142 ran with AOT off throughout; AOT build left malloc ~2 MB, so textures were skipped (white THP movie, flat textures, jobs 135-141)** (AOT build) [device; jobs 135-142] core/src/cpu/aot.h:10-11; core/src/gx/pica/pica.c:151-153 *Job 142's AOT A/B is void. The AOT verdict (30% slower) is not in source comments. Informs: aot_reserve at startup; scratch from linear memory*
- AOT (static recompilation, C12) 2DS A/B, job 144: **cutscene 20.2% JIT vs 14.4% translated; later stretch 12.8% vs 8.6%; translated code ~6 MB** (2DS tp850, --ab aot, --cover 0.90) [device; 2026-10-14 evening] docs/research/AOT_PLAN.md:75-80 *Supersedes 'A/B pending' at AOT_PLAN.md:3 and REDESIGN_CANDIDATES.md:160-161 (stale status lines). qemu counted only ~17% more ARM instructions per guest instruction, so the loss is fetch. Informs: AOT off; whole-function translation loses on a fetch-bound core*
- AOT coverage and runtime counters: **18-20 % of guest instructions translated in the cutscene, ~94 % in a later section; 233-282k AOT calls/s, 89-117k runs/s, 1.1-2.7k fallbacks/s, 19-53k slow accesses/s, 0 hash failures** (aot on/off phases (label lags by one report)) [device; job 144] '# aot 10s' lines, relay_144.csv *Informs: Even at 94 % coverage AOT was slower*
- AOT static recompilation vs JIT, A/B: **core0 cutscene 24.98 -> 15.91 (-36 %), later 11.90 -> 8.19 (-31 %); I-misses 6.72 -> 11.52 M/s and 4.09 -> 10.68 M/s; fetch stalls 200 -> 291 and 104 -> 227 M/s** (602 s; --cover 0.90 build) [device; job 144 (2026-10-14)] core/out/relay_144.csv deduplicated, ab_split.py --col aot *Raw log holds the run 4 times Informs: Binary static recompilation rejected; code footprint dominates*
- AOT translation (C12) vs JIT: **Cutscene 20.2% JIT vs 14.4% AOT; later stretch 12.8% vs 8.6% (~30% slower); qemu counted only ~17% more ARM instructions** (tp850, 20 s A/B phases) [device; 2026-10-14, jobs 142-147] memory/gc3ds-aot-verdict.md:13-15 (job 144) *Verdict: rejected, --aot off Informs: AOT/binary recompile rejected; native mode via decomp instead*
- AOT verdict on the 2DS (job 144): **Cutscene 20.2% JIT vs 14.4% translated; later 12.8% vs 8.6%; qemu had counted only ~17% more ARM instructions per guest instruction; core 0 IPC 0.31, 44% of cycles stalled on fetch** [device; 2026-10-14] git 0903f5f diff (AOT_PLAN.md) *Informs: AOT rejected: 6 MB of translated code fetches worse than compact JIT blocks*
- ARM core <-> VFP register transfer round trip: **5 cycles** (FMRS/FMSR round trip) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:287, :408 (quoted from FINDINGS.md) *TRM: FMRS 2, FMSR 4; FMXR/FMRX serialize. Informs: Quantized load/store (GQR) cost per lane; keeping FPRs in VFP registers*
- ARM instructions executed per guest instruction: **Cutscene: 12.1 (264 M / 21.9 M). Later stretch: 6.3 (352.8 M / 55.6 M).** (tp850) [device; 2026-10-12/13] Derived from relay_83 and relay_97 (inst_m, ins_k) *Derived; inst_m is sampled in rotation with other PMU events Informs: The JIT's cost per guest instruction depends on the code mix. Tight loops already run at about 6.*
- Big-endian float stubs (OPT_BEFLOAT, job 166): **Core 0 cutscene 25.09 on vs 25.28 off, later 12.51 vs 12.50; I-cache misses +6-7% (~800 words of extra stubs). Grounding: SETEND BE +12% scalar / +23% paired on gcbench; core-VFP moves ~3.5-4 cycles; float loads/stores 27.6% of modelled core-0 cycles** [device; 2026-10-15] git 402d95d body; 136545a diff *Informs: Neutral; only paid if it replaces the originals*
- Binary static recompilation (C12, AOT PPC->C via GCC) vs JIT, negative result: **~30% slower than the JIT on the 2DS (exact on PC)** (TP) [device; job 144, 2026-10-14] docs/research/NATIVE_PORT_PLAN.md:11-14 *Code size worsened core-0 I-fetch stalls; the output keeps GC memory image, byte order and float semantics. Rejected: do not re-propose without a new argument. Informs: Rejects DolRecomp/XenonRecomp-style recompilers on the 2DS*
- Block linking, in-run A/B (20 s phases): **Linking off: speed 7.94%, core 0 own 9.64%, gxwait 15.2%. Linking on: 9.20%, 19.40%, 53.3%. Core 0 got 2.0x faster but the game was GX-bound.** (Title from boot, GX on core 2) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 2 (= perf_2026-10-11_jit2.csv); heavy rows, 4 s settle *Informs: Linking is mandatory. A core-0 gain only shows once GX is not the limit.*
- Block-link requests vs links made per second: **155,000 requests/s, ~2,600 links/s** (TP, linking on) [device; job 184] core/host3ds/main.c:1245 *A per-reason breakdown was added afterwards. No result from it is in source. Informs: Diagnosing why link requests fail (dispatcher round trips)*
- Budget at block exits + SO-free CR updates (4c5012a): **title heavy rows 16.24 -> 16.62%; cutscene 15.52 -> 15.89%; I-cache 8.8 -> 8.2-8.4 M/s; fetch stalls 277 -> 245-261 M cycles/s** [device; 2026-10-12, relay 46-49] docs/HANDOFF.md L629-631
- Budget check at block exits plus SO-free CR updates (4c5012a/era): **Title 16.72% (all rows), cutscene 16.00%. I-cache misses 8.22/8.41 M/s. Fetch stalls 259/247 M cycles/s. gxwait 10.3/12.1%.** (Separate runs) [device; relay jobs 48-49, 2026-10-12] core/out/relay_48.csv, relay_49.csv *HANDOFF quotes 16.62 / 15.89 on its own row filter Informs: Small core-0 gains turn into GX waiting*
- C4 load-use scheduling: **8 loads + 8 adds, used at once: 33.0 cycles per loop; used 2 instructions later: 17.0 (2.0 cycles saved per load)** (gcbench 5, L1-resident) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L9-11 *Timingprobe job 176 says 'ldr then use or one instruction between: same' (4.0 chase). Consistent: the gain needs a gap of 2 Informs: JIT should hoist loads at least 2 instructions ahead of use (guest register loads from the context)*
- C4 load-use: 8 loads used at once vs used 2 instructions later: **33 vs 17 cycles: +2 cycles per load used immediately** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:55; docs/research/GPU_GUIDANCE.md:105-106 *Informs: Emit loads two instructions before use*
- Call chains depth 1-12 (per call + return): **16.5-18.3 cycles; no cliff at depth 3** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:155 *SD5113 prior: 3-entry return stack; no overflow cost seen. Informs: Helper-call nesting is not penalised*
- Check hoisting (opt), in-run A/B, 300 s: **Cutscene: 20.94 → 21.34% (core 0 23.16 → 23.73%). Title: 20.88 → 21.63% (core 0 23.86 → 24.16%). Deoptimized blocks: about 420-500.** (20 s phases) [device; relay jobs 95-96, 2026-10-13] core/out/relay_95.csv, relay_96.csv *Informs: Hoisting helps by about 2.5%*
- Check hoisting (OPT_HOIST): **job 97: core 0 21.41 -> 22.62%, speed 19.48 -> 20.07%; ~450 blocks deoptimize in TP; job 117: core 0 +5.0% cutscene, +4.7% core-0-bound section (made default)** [device; 2026-10-13] docs/HANDOFF.md L881-886, L947
- Check hoisting (OPT_HOIST, stage 1) core 0 speed: **+5.7% core 0** (2DS, job 97, cutscene, --ab opt) [device; job 97] docs/research/PERF_LEVERS.md:16-17,42 *One run only ('in one run'); same build also had deferred byte swap and float paths. Informs: Keep hoisting; memory-check cost is a lever*
- Check hoisting re-measured (job 117): **Cutscene: speed 21.12 → 21.65%, core 0 23.50 → 24.83% (+5.7%). Later: 11.48 → 11.79%, core 0 11.54 → 11.86% (+2.8%).** (tp850, 600 s) [device; relay job 117, 2026-10-13] core/out/r117a.csv, r117b.csv; checked with core/tools/ab_split.py on relay_117.csv *HANDOFF says +4.7% in the core-0-bound stretch; these logs give +2.8% Informs: Default on*
- Check hoisting, in-run A/B, 600 s with the later stretch: **Cutscene: 20.94 → 21.58%, core 0 23.16 → 24.39% (+5.3%). Later: 11.43 → 11.69%, core 0 11.49 → 11.75% (+2.3%). ARM instructions 266 → 251 M/s (cutscene) and 395 → 372 M/s (later).** (tp850, 600 s) [device; relay job 97, 2026-10-13] core/out/relay_97.csv *Informs: Hoisting made default*
- Code install (write + clean + whole I-cache invalidate + first run) by block size: **64 B 10,962 / 256 B 11,896 / 1 KB 35,575 / 4 KB 30,952 / 16 KB 97,484 / 64 KB 285,159 cycles (job 183); job 181: 10,968 / 11,883 / 35,607 / 47,417 / 110,111 / 298,488** (core 0) [device; 2026-10-15, jobs 181/183] relay_183.csv and relay_181.csv '# timing install' *Runs differ by 10-35%. In 183 the PMU disagreed with the tick at 1 KB, so that size was timed by the tick only, and 1 KB sync+run (33.9k) costs more than 4 KB (24.2k). Informs: Fixed ~11k-cycle install cost: batch compiles and installs, don't install block by block*
- Code install 4 / 16 / 64 KB: **job 181: 47,400 / 110,100 / 298,500; job 183: 31,000 / 97,500 / 285,200 cycles** [device; jobs 181, 183] docs/research/TIMING_MODEL_PLAN.md:157 *Varies 10-35% between runs.*
- Code install: write + clean + whole I-cache invalidate + first run, 64 B / 256 B / 1 KB: **10,960 / 11,900 / 35,600 cycles; ~200 I-cache misses after each (whole cache refills)** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:156,173 *Informs: Batch installs (~11,000 cycles fixed each)*
- Code scheduled against ARM11 interlocks (sched), in-run A/B: **Cutscene: 21.31% off vs 21.11% on (core 0 22.73 vs 23.14%). Title: 20.21% vs 21.04% (core 0 23.98 vs 23.51%). Data stalls 170.7 → 165.8 and 173.9 → 167.5 M cycles/s (-3 to -4%).** (20 s phases) [device; relay jobs 91-92, 2026-10-13] core/out/relay_91.csv, relay_92.csv *Informs: The interlock model's -18% did not reach the device. Within noise.*
- Compiling share of core 0: **~0.5%** (TP tp850 steady) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:188 *Informs: Compile speed is not a lever (AOT rejected for other reasons)*
- Core <-> VFP register move cost: **~3.5-4 cycles each, moves do not overlap** (gcbench micro-benchmark) [device; gcbench run 3, 2026-10-10] core/src/jit/jit.h:115-117 (cites docs/FINDINGS.md run 3) *OPT_BEFLOAT (SETEND BE) defaults to off. No 2DS A/B of it is in source. Informs: OPT_BEFLOAT / SETEND BE float stubs*
- Data-dependency (interlock) stall cycles per host instruction: **~0.64 cycles per instruction** (2DS PMU stall_data counter, TP) [device] core/tools/arm11_sched.py:6-7 *The PMU's data-hazard event. The offline model it calibrates (arm11_sched.py) uses datasheet latencies. Informs: Where scheduling work goes (SCHED_STUBS / SCHED_CONST)*
- Denser indirect table FAST_BITS=13 + 2-way: **Neutral; table reads hit L2** (tp850 A/B) [device; 2026-10-15, job 193] memory/gc3ds-jit-dead-ends.md:34 (job 193) *Informs: Rejected*
- Device interpreter fallbacks per 10 s: **Cutscene: ps_cmpo0 22,560, rfi 21,359, sc 17,912, lmw 11,073, stmw 11,073, mftb 9,393, divw 8,655, sraw 6,604 (first window); later windows rfi 38,573, sc 34,591; fb_k column ~15 k/s** (2DS relay jobs 86-94, TP from tp850.state, ~12 fields/s) [device; 2026-10-13] //wsl$/Debian/root/relay_86a.csv, relay_93a.csv '# fallbacks 10s' lines and fb_k column *Counts at 20% speed; scale x5 for full speed. Informs: fallback cost is ~15k instructions/s out of ~22 M/s (<0.1%)*
- Device JIT buffer occupancy and block counts: **13.80-13.89 host words per guest instruction compiled, 38-41% of the buffer used; ~19,700-21,400 blocks in cache, 0 flushes; kept words per guest (all blocks so far) hot 2.0, exit stubs 0.9-1.0, cold 2.4-2.5** (2DS cutscene, jobs 86-93) [device; 2026-10-13] relay_93a.csv / relay_86a.csv '# jit code', '# jit cache', '# jit words per guest instruction' lines *Device 'hot 2.0' words per guest differs from qemu's ~3.8-4.1 for similar builds; definitions or sampling may differ (unexplained). Informs: device buffer does not overflow in steady state (unlike qemu runs)*
- Dispatcher exits per second: **cutscene: indirect misses ~3.67k (slot taken 3.64k), budget ends ~756, other ~5.2k, gekko_run slices ~3.7k; later: indirect misses ~1.94k, budget ends ~2.1k, other ~1.65k** (opt 115) [device; job 157] '# dispatches/s' lines, relay_157.csv *Informs: Sized IND2 (rejected, see job 161)*
- Dispatcher return reasons and core 0 time outside compiled code: **Job 97 cutscene: 3.7k indirect misses/s, about 760 budget ends/s, 12.6k other/s, about 28 link requests/s, 3,650 gekko_run slices/s. Core 0 outside compiled code (jobs 70-99): dispatch 5-10% (this includes the idle hook's wait for GX), gekko_run 0.4-0.5%, events 0.4%, GX poll 0.3%, compile 0.1-0.7%.** (tp850 cutscene) [device; 2026-10-12/13] '# dispatches/s' and '# core0 outside compiled code 10s' in relay_70-99 *'dispatch' mixes waiting on GX with real dispatching Informs: The dispatcher's remaining share is round trips, not table misses*
- Dispatcher round-trip reduction (retstub+syscall+hlelink together, --ab stack): **core 0 cutscene +2.2%, Link / Epona stretch +4.2%** (tp850, ab_split.py stretches) [device; job 151] core/host3ds/main.c:395 *The individual arms (next entry) sum to more (+2.8 / +6.4) than the combined result, so the gains are not additive. Informs: All three on by default*
- Dispatcher share and return rate: **<1%; ~8,500 returns/s steady play; 265,000/s in state-load window** (TP tp850) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:187 *Informs: Dispatcher redesign is not worth it*
- Dispatcher traffic before/after 64K indirect table: **job 91: indirect misses 12,109-13,467/s, dispatcher entries ~24.4 k/s; job 93: indirect misses 3,380-3,550/s, entries ~15.9 k/s (link requests 37-137, budget ends ~700, other ~11,000-11,600); gekko_run slices 3,200-3,360/s** (2DS cutscene) [device; 2026-10-13] relay_91a.csv, relay_93a.csv '# dispatches/s' lines and disp_k column *Speed effect +0.2 points (HANDOFF line 861-863). Informs: the dispatcher's residual load is 'other' returns (~11k/s), not table misses*
- Exceptions per 10 s: **cutscene c00 (sc) 33-39k (~3.4-3.9k/s), 500 ~1.6-1.8k, 800 ~190, 900 ~70; later c00 ~20k (2.0k/s), 500 ~1.06k** [device; jobs 149-157] '# exceptions 10s' lines, relay_149.csv and relay_157.csv *Informs: sc rate sized OPT_SYSCALL*
- Fallbacks left (da2aefd era): **cror ~40k/s (compiled since), psq_l/psq_st with quantized GQR ~15k/s, frsqrte/fres ~6k/s, mtspr/mfspr ~5k/s** [device; 2026-10-11] docs/HANDOFF.md L237-238
- FAST_BITS=13 + 2-way indirect table (job 193): **Speed 20.4 vs 20.4 (job 191); D-read misses 1.08 vs 1.04 M/s; indirect misses 1.8k -> 9.7k/s; the device has ~1M read misses/s** [device; 2026-10-15] git 91a78d2 diff *The qemu simulator's L1 counts overstate misses on L2-resident data. Informs: Neutral, not adopted*
- Fault mode (fastmem) A/B job 88: **speed on 19.44% +- 0.65 (69 rows) vs off 19.53% +- 0.60 (60): no difference; core 0 own speed 21.2 -> 23.6% (+11%) when re-read via gxwait; both fault-mode runs crashed after ~700 and ~2,700 faults (kernel prefetch abort inside svc 0x94)** [device; 2026-10-13, relay 86-90] docs/HANDOFF.md L819-827, L876-880, L1109-1112 *initial 'stalls not instruction count' lesson later retracted: gain was hidden as gxwait Informs: fault mode off on 2DS pending crash root cause*
- Fault mode (faultmem: per-access window checks removed, faults patch sites) A/B, with patch sync: **core0 cutscene 25.14 -> 27.40 (+9.0 %), later (partial) 12.55 -> 12.84; I-misses 6.10 -> 5.41 M/s; run died at ~389 s after 6,166 faults** (fault_sync in jit_entry) [device; job 167 (2026-10-15)] core/out/relay_167.csv, ab_split.py --col faultmem; faults column *Log ends in HTTP 429; later stretch n 33/18 Informs: Fault mode is the largest single JIT lever measured; crash blocks a default*
- Fault mode (no memory checks, data aborts) A/B, job 88: **speed 19.53% off / 19.44% on; gxwait 7.5% / 16.1%; core 0 speed 21.2% -> 23.6% (~+11%)** (2DS, --ab faultmem) [device; job 88] docs/OPT_JIT_PLAN.md:11-14,32-34 *Kept off: the 3DS kernel crashes delivering user data aborts now and then; root cause investigation pending (owner decision 2026-10-15). Earlier HANDOFF reading of the same data ('did not move speed') was a misreading. Informs: Upper bound on removing per-access checks; fault-mode retry*
- Fault mode (unchecked loads/stores + data-abort patching): **+11% core 0, but the 3DS kernel crashes delivering user data aborts; check hoisting instead +5%** (tp850) [device; jobs 86-90 (~2026-10-12/13)] memory/gc3ds-jit-dead-ends.md:26-27 (jobs 86-90); memory/gc3ds-region-jit-next.md:16 *Crash risk: run only when James is home Informs: Largest single JIT lever found, blocked by kernel crash; hoisting kept*
- Fault mode crash recurrence: **job 167 (sync deferred) died ~390 s after 6,166 faults; earlier crashes ~700 and ~2,700 faults; job 168 (--faultsync off) initially read as stopping at 406 s with 8,011 faults, later shown to run 600 s with no Luma dump** [device; 2026-10-15] docs/HANDOFF.md L1109-1118, L1129-1132 *crash root cause attributed to svc 0x94 issued inside user exception handler; only one clean run*
- Fault mode crash root-cause attempts: **Old Luma dump: kernel died in svc 0x94 from our handler; job 167 (sync moved out) still crashed after 6,166 faults; job 168 (--faultsync off) ended at 406 s with no new dump** (Fault mode stress) [device; 2026-10-15, jobs 167-168] memory/gc3ds-timing-drift-approved.md:23-25 *Unresolved Informs: Whether fault mode (+11%) can be retried*
- Fault mode with --faultsync off A/B: **core0 cutscene 24.96 -> 27.29 (+9.3 %), later 12.48 -> 12.99 (+4.1 %); game speed in the cutscene 21.82 -> 21.51 (GX-bound); I-misses -12 % cutscene; ran 600 s through 10,589 faults** (no cache maintenance for patches) [device; job 168 (2026-10-15)] core/out/relay_168.csv, ab_split.py --col faultmem; faults column *HANDOFF reports 8,011 faults (count at ~406 s). One crash-free run; earlier runs crashed at ~700, ~2,700 and 6,166 faults Informs: Needs more crash-free runs before a default*
- Fault mode without cache maintenance (--faultsync off, job 168, full data): **later stretch 12.90 vs 12.41% (+3.9%, ~6 SE); core 0 cutscene 27.29 vs 24.96% (+9.3%); 600 s, 8,011 faults, no crash** [device; 2026-10-15] docs/HANDOFF.md L1129-1132 *single run; needs more crash-free runs before default Informs: largest remaining measured JIT lever*
- Fault-mode (unchecked MEM1 access) A/B: **speed 19.44% (on) vs 19.53% (off); gxwait 16.1% vs 7.5%: core 0 itself ~10% faster** (TP heavy rows) [device; job 88] core/tools/perf_heavy.py:79-81 *Shows that the GX thread capped overall speed, so the core 0 gain appeared only as more gxwait. Fault mode crashed the kernel (next entry) and is off by default. Informs: Core-0-only gains are invisible while GX limits; fault mode parked*
- Fault-mode loads and stores (faultmem), in-run A/B, and crashes: **Speed 20.78 → 21.29%. Core 0 own 22.69 → 25.89% (+14%). gxwait 8.2 → 17.7%. Data stalls 173 → 200 M cycles/s. Job 88 crashed at 147 s. Job 86 (fault mode on only, 18.2% speed) crashed at 41.6 s after 685 faults. Luma dump (job 90): kernel prefetch abort, pc fffffffe, far 54025000, dfsr 805.** (tp850 cutscene, 20 s phases) [device; relay jobs 86-90, 2026-10-13] core/out/relay_86.csv, relay_88.csv (log duplicated), relay_90.csv *HANDOFF quotes speed 19.53/19.44 and core 0 21.2 → 23.6 on its own filter Informs: +11-14% on core 0 is available, but kernel delivery of user faults is unsafe. Fault mode is off on the 2DS.*
- Fault-mode stability: **kernel crashed in svc 0x94 from a user exception handler (jobs 86-90); job 167 still crashed after 6,166 faults with the cache sync moved out of the handler** (--faultmem on) [device; jobs 86-90, 167] core/src/jit/jit.c:94-99, 5407-5409 *A root-cause test drops svc 0x94 entirely (--faultsync off). No result for it is in source. Informs: Fault mode off by default*
- fcache second pass, three 600 s runs of one build: **Cutscene core 0: 24.49 → 24.97, 24.44 → 24.93, 24.56 → 24.93% (+1.5 to +2.0%). Later core 0: 11.75 → 11.94, 11.78 → 11.86, 11.86 → 11.89% (+0.3 to +1.6%). Cutscene I-cache misses +2.5 to +5.5%.** (tp850, 600 s) [device; relay jobs 132-134, 2026-10-13] core/out/r132a/b, r133a/b, r134a/b; the same numbers from core/tools/ab_split.py on relay_132-134 --col opt *DISCREPANCY: REDESIGN_CANDIDATES C11 reports cutscene +3.0-4.3% (22.42 → 23.38 …) and later -2.7 to -4.4%. These logs show fcache never negative. Not reconciled. Informs: Whether to make fcache default (C11)*
- Float / paired-single fallback cost (eb867fd): **fpfb 50.5% of time, ~1.1 us each; b614a7a fast float made 97.1% native under qemu** [device; 2026-10-10, eb867fd] docs/HANDOFF.md L378 *97.1% figure is qemu*
- Float JIT (fast float) effect on the title scene: **3.3% -> 4.9%; interpreted floats 50.5% -> 1.4% of time; vertices then 44% (lighting 24%, gather 6%). qemu: TP compiled code 91.9% -> 97.1% native** [device; 2026-10-10] git 4e491c4, b614a7a bodies *Informs: Float compile was mandatory*
- Float JIT gain on title scene: **3.3% -> 5.1% (+55% relative); vertices then 40% of time** (TP title scene) [device; 2026-10-10 night, b614a7a] memory/tp3ds-gc-wii-on-2ds.md:92 *Sequential builds, not same-run A/B Informs: Float handling is a large lever*
- Float lanes C11 (model and device): **Model: 6.29 + 2.48 -> 6.17 + 2.30 host instructions + stall cycles per guest instruction (-3.4%). Device jobs 132-134: cutscene core 0 +4.3 / +3.5 / +3.0%, later stretch -2.7 / -2.9 / -4.4%; job 131 +1% / noise** [device; 2026-10-13] git 4839521 body; 28c6690 diff; a88ca28 subject *Informs: Scene-dependent, off*
- Float steps (3a1696f), relay 50-51: **title 16.55% vs 16.62%; cutscene 15.89 vs 15.89; I-cache misses 8.24 -> 7.89 M/s (-5%)** [device; 2026-10-12] docs/HANDOFF.md L639-640
- Float steps, build 3a1696f: **Title 16.63%, cutscene 16.01%: no change. I-cache misses 7.88/7.92 M/s. GX thread 79.6/81.4% busy. gxwait 10.7/13.0%.** (Separate runs) [device; relay jobs 50-51, 2026-10-12] core/out/relay_50.csv, relay_51.csv *Informs: Core 2 had become a co-bottleneck*
- Float values kept in VFP registers (fcache, opt 3 → 7), first pass: **Cutscene: speed 21.14 → 21.60% but core 0 24.46 → 24.40%. Later: 11.72 → 11.60%.** (tp850) [device; relay job 131, 2026-10-13] core/out/r131a.csv, r131b.csv; ab_split.py on relay_131 *Informs: No gain*
- FPSCR state: **FPSCR 0x03000010 at start (DN + FZ set), 0x23000010 after kernels (C flag)** (gcbench 3) [device; 2026-10-10] bench/results/latest.txt L5, L116 *Flush-to-zero and default-NaN are on by default: they differ from Gekko denormal and NaN semantics Informs: Float accuracy vs speed policy in the JIT*
- Guest MIPS, integer loop (JIT variants vs interpreter vs native C): **JIT with rev per access (BE) 328.8; byte-swapped RAM (sw) 361.7; SETEND BE 361.7; interpreter 21.1; native C on the same loop 657.5 MIPS. JIT sw/setend = 74.4% of a 486 MHz Gekko at 1 IPC, 55% of native C (about 2.2 ARM cycles per guest instruction)** (gcbench 3, synthetic integer kernel, core 0, 804 MHz + L2) [device; 2026-10-10 (run 3); repeated 2026-10-11 (gcbench 4)] bench/results/latest.txt L27-46; gcbench4_2026-10-11.txt L44-82 (identical) *Synthetic loop, not TP. The % GC column assumes Gekko retires 1 instruction per clock, so it is a pessimistic bound. FINDINGS run 2 gives 22 / 329 / 658 for the same row. Informs: Integer codegen ceiling: native C is only 1.8x the JIT, so integer codegen has limited headroom left*
- Guest MIPS, paired-single loop (vertex-transform shape): **JIT BE 59.8, sw 68.0, sw vector mode 65.9, SETEND 68.4; interpreter 8.6 MIPS (14% of GC, about 11.8 ARM cycles per guest instruction)** (gcbench 3, core 0) [device; 2026-10-10] bench/results/latest.txt L33-36, L44 *Supersedes run 2's 55 MIPS (FINDINGS L20). Vector mode is 3% slower than scalar. Informs: Paired singles are the slowest class. Do not use VFP short-vector mode for paired ops*
- Guest MIPS, scalar float loop: **JIT BE 100.5, sw 112.8, SETEND 112.8; interpreter 18.7 MIPS (23.2% of GC, about 7.1 ARM cycles per guest float instruction)** (gcbench 3, core 0, 804 MHz + L2) [device; 2026-10-10; gcbench 4 identical] bench/results/latest.txt L30-32, L43 *Synthetic kernel Informs: Float JIT is 3.2x slower per instruction than integer. VFP latency (8-9 cycles) is the bound, so the work is reordering and scheduling*
- HLE shadow-scan stand-in A/B, speed: **Link / Epona stretch 10.6% -> 12.4%; cutscene within noise** (tp850, --ab hle) [device; job 145] core/host3ds/main.c:394 *The comment at main.c:306-309 still says the stand-in is off pending a check, but opts_default sets hle = true (a stale comment). An earlier version froze TP (9d7c239, stale PC). Informs: Stand-in on by default*
- HLE stand-in activity in the later stretch: **~215-217k stand-in runs per 10 s (~21.7k/s), standing in for ~440-445 M guest instructions per 10 s; ~0.07-0.15k runs/s in the cutscene** (hle on) [device; jobs 145, 157] '# hle 10s' lines, relay_145.csv and relay_157.csv *The 'instructions stood in for' count is huge vs the stretch's 60 M guest instr/s; it probably counts skipped loop iterations Informs: The stand-ins cover a wait/loop pattern; native stand-ins elsewhere have no target*
- HLE stand-ins A/B: **core0 cutscene 24.21 -> 24.14 and 24.12 -> 24.12 (neutral); later 11.71 -> 12.09 (+3.2 %) and 11.64 -> 11.88 (+2.1 %); later I-misses +31 % / +36 %** (145: 415 s, 146: 417 s) [device; jobs 145-146 (2026-10-14)] core/out/relay_145.csv; relay_146.csv deduplicated; ab_split.py --col hle *146's log is duplicated (844 rows / 497 unique) Informs: Default on; it gains despite the I-miss rise*
- Hot-block relayout: **Later stretch -4.5%; I-cache misses +40%** (tp850 A/B) [device; 2026-10-15 evening, job 195] memory/gc3ds-jit-dead-ends.md:32 (job 195) *Informs: Rejected*
- I-cache coherence after code rewrite, and sync cost: **199/200 rewrites ran stale code without sync, 0/200 with sync. Full sync 2.8 us. 256-byte D-cache flush 4.6 us** (gcbench 3/4, RWX JIT buffer) [device; 2026-10-10] bench/results/latest.txt L6-8; gcbench4 L6-8 *Conflict: timingprobe job 176 measures 'JIT cache sync (flush + svc 0x94)' at 16,548 cycles (~20 us), and job 178 at 10,095. That is 4-7x the bench figure, probably a different operation scope. Unreconciled Informs: Cost of block invalidation and recompile, and of batching cache syncs*
- I-cache refresh cost and need (device): **199 of 200 rewrites ran stale code without refresh; full refresh 2.8 us; 256-byte data-cache flush 4.7 us** (gcbench) [device; 2026-10-10] C:/Users/tyler/TP3DS/emu/docs/FINDINGS.md:10-11 *Cross-check only Informs: Cost of self-modifying code handling / icbi*
- icbi block exits vs real code flushes: **~62,000 icbi exits/s vs ~10 flushes/s** (TP) [device; job 184] core/src/jit/jit.h:122-124 *OPT_ICBI defaults to off in main.c. No 2DS A/B result for it is recorded in source. Informs: OPT_ICBI (icbi interpreted in place; leave the block only when code was dropped)*
- Indirect call from one site: 1 / 2 / 4 / 16 round-robin / 16 random targets: **28.1 / 29.6 / 30.3 / 30.9 / 30.9 cycles per call incl. loop; brmiss ~0: indirect targets not predicted** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:154 *Informs: Dispatcher/indirect branch cost is fixed ~28 cycles regardless of target count*
- Indirect table 64K entries (d613012): **misses 13k -> 3.6k/s; dispatcher share of core 0 (cutscene) ~10-16% -> 4.5-6%; cutscene 21.35% (21.17 before); title 20.58% (21.11, different stretch); malloc largest free block 17-20 MB** [device; 2026-10-13] docs/HANDOFF.md L861-863
- Indirect table sizing 2 x 8192 vs 4 x 4096: **2 x 8192: table resets 186/s (was 0), misses 19.6k -> 24k/s, time outside compiled code 9 -> 26%; fixed 2bbe8ab (4 x 4096): 15.11% (SE 0.16), misses 13.9k/s, 0 resets, 7.4% outside compiled code** [device; 2026-10-11] docs/HANDOFF.md L467-471
- Indirect-branch table size: **Indirect misses/s: 32.4-32.9k (4K entries, jobs 52-53) → 14.1k (16K, a1dd311, jobs 70-91) → 3.5-3.7k (64K, d613012, jobs 97-99). Speed with 16K: cutscene 21.30%, title 21.07%. With 64K: 21.60% / 20.64% (jobs 93/94).** (Separate runs) [device; relay jobs 70-99] '# dispatches/s' in relay_52, 70, 91, 97, 98, 99; relay_70/71, 93/94 *Informs: Indirect-table misses are solved; the dispatcher share is no longer from misses*
- Indirect-branch table wiping regression and fix: **82a091b (CR logic ops; 4096-entry indirect table wiped on every block-key change) 12.9% (regression); b439173 (4 x 2048 tables, switched per key; quantized psq; fres/frsqrte) 15.0%** [device; 2026-10-11] docs/HANDOFF.md L156-157 *Informs: indirect table must not be flushed per key change*
- Indirect-table misses because the slot holds another target: **~3,400/s** (TP) [device; job 157] core/src/jit/jit.h:125-126 *OPT_IND2 defaults to off. No A/B result is in source. Informs: OPT_IND2 (2-way indirect table)*
- Indirect-table misses: cause and table size: **qemu 30 fields: 79,307 of 86,499 misses are slot conflicts; folding high bits 86,499 -> 91,663 (no help); 4x table 36,621; 2DS 35k -> 13.6k/s with 16,384 entries** [device; 2026-10-12, a1dd311] docs/HANDOFF.md L758-765 *miss-cause split is qemu; rate is device*
- Inline pipe stores A/B (a5318cb): **on 14.69% vs off 14.16% (+3.7%)** [device; 2026-10-11, perf_2026-10-11_pipefast.csv] docs/HANDOFF.md L466-467 *both halves depressed by indirect-table regression in same build*
- Inline pipe stores and indirect-table count: **Inline pipe stores 14.69% on vs 14.16% off (~155k pipe stores/s); with two tables too few: 186 table resets/s, 26% of core 0 outside compiled code; four tables of 4096 restored** [device; 2026-10-11] git a5318cb, 2bbe8ab bodies *Informs: Indirect-table sizing*
- Instruction reordering against VFP latency: **pseq 55 -> pilp 61 MIPS (~10%) on paired code** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:151-153 *Informs: Scheduling is a small lever*
- Interlock scheduling (SCHED_ALL) A/B: **within noise: cutscene -0.2 points, title +1.1 points; fewer instructions and words** (--ab sched) [device; relay jobs 91-92] core/host3ds/main.c:391 *The model predicted the stubs were where interlocks are (loads 63% of modelled interlock cycles), but the device showed no measurable speed change. Informs: Kept on for code-size reasons only*
- Interpreter fallbacks per 10 s (opt 115, before NATIVE2): **cutscene: sc 39.3k, ps_cmpo0 19.3k, lmw 14.6k, stmw 14.6k, divw 12.6k, mftb 12.2k, sraw 8.0k, rfi 4.4k (~12.5k/s); later: divw 21.8k, sc 20.4k, ps_cmpo0 16.6k, lmw/stmw 11.0k each, sraw 7.3k** (clean run) [device; job 157] '# fallbacks 10s' lines, relay_157.csv *Informs: Sized NATIVE2; what remains after it is sc, mftb, rfi, mtspr*
- Interpreter fallbacks per second (top 8, cutscene): **rfi 4.1k, sc 3.7k, ps_cmpo0 1.8k, lmw 1.4k, stmw 1.4k, mftb 1.2k, divw 1.1k, sraw 0.8k. About 15.5k/s in total.** (tp850 cutscene) [device; relay job 73, 2026-10-12] '# fallbacks 10s' in relay_73.csv *Informs: Which opcodes to compile next*
- Interpreter fallbacks still left at run time: **per 10 s: cror 215k, psq_l / psq_st ~47k each, mtspr 27k, frsqrte 19k, fres 17k, rfi 15k; earlier (pre level 2): dcbi ~150k/s, mfmsr/mtmsr ~40k/s each** [device; 2026-10-11] docs/HANDOFF.md L144, L282 *'10 s' window counts, not per-second; superseded by later lists (L237, L1043-1045)*
- JIT block exit cost: linked direct B vs BX dispatcher: **13 ns -> 37 ns per exit** (microbenchmark, small code footprint) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:262-266 (quoted from FINDINGS.md) *TRM says register-indirect branches are never predicted (5-7 cycles), which fits the gap. Informs: Patching block links with direct B/BL; dispatcher design*
- JIT cache sync (D-cache flush + svc 0x94 whole I-cache invalidate): **10,095-10,182 cycles (~12.6 us) with ~203 I-cache misses afterwards (jobs 178/183/192/196); 16,548 in job 176** (core 0) [device; 2026-10-15] relay_176.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing svc icache sync' *Job 176 (16,548 cycles) ran with the PMU off. Later runs are 10.1k cycles, so 176 is superseded. Informs: Batch code installs and icbi handling; each install refills the whole I-cache*
- JIT cache sync (flush + svc 0x94): **16,548 cycles (~20 us) job 176; 10,095 cycles job 178, with 203 I-cache misses after** (timingprobe svc) [device; jobs 176, 178] docs/research/TIMING_MODEL_PLAN.md:93 *Two runs differ ~40%. Informs: Batch code installs*
- JIT code buffer occupancy: **16 MB buffer (hot 62.5%, cold 37.5%); ~65% used after 4 minutes of the title; full flush when a region fills** (3DS build) [device; 2026-10-12] docs/REGION_JIT_PLAN.md:48-49 *Informs: Room for duplication/regions*
- JIT code shape in TP: **8.0 host words per guest instruction (all code incl. stubs); per block kept: hot 2.1, exit stubs 1.0, cold 1.2; 41-42% of the buffer used; ~25,300 blocks; steady-state compiles ~4/s median (0.2% of core 0)** (tp850, traces off) [device; 2026-10-15, job 184] relay_184.csv averaged '# jit code', '# jit words per guest instruction', '# jit cache' lines; compiles_s, jitc_pct columns *Informs: Code size feeds the I-cache misses; compile time is negligible in steady play*
- JIT code size and cache state: **Total host words per guest instruction: 27.4-27.8 (jobs 7/8) → 14.2-15.1 (jobs 40-61) → 13.4-14.0 (jobs 62-83). Blocks: about 22,700-24,000 in the cutscene, 33,600-34,500 in the title. 0 flushes and 0 compile failures after the stubs. The 16 MB buffer is 44-76% full.** (tp850 / title) [device; 2026-10-12/13] '# jit code', '# jit cache' and '# jit words' in relay_7-97 *Job 97 reads 8.6 words at 23% full after a reset. The 'jit words (all blocks kept so far)' figures swing between runs. Informs: Code-buffer headroom for larger code (regions, AOT)*
- JIT code space overflow: **4 MB code space overflowed (JIT thrash, title 1.9%); raised to 16 MB** (TP title scene) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:71 *Later traces were turned off on the 2DS partly for code-buffer overflow Informs: Code buffer size vs malloc heap trade-off*
- JIT compile load in steady play: **~19 compiles/s cutscene, ~9 later, ~3-6 movie; jitc_pct 0.68 % cutscene, 0.33 % later; 60-80 % of core 0 in the first second after loading** (clean run) [device; job 157] compiles_s / jitc_pct columns, relay_157.csv (and boot rows of 103/108) *Informs: Compile speed is not a steady-state lever; heavier optimisation per block is affordable*
- JIT compile speed: **0.49 us per guest instruction (2 M/s); ~50 ms for a 100k-instruction hot set** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:163-164 *Informs: Tiered/region recompile cost; flush cost*
- JIT compile speed: **0.49 us per guest instruction (2036k instr/s); gcbench 4: 0.50 us (2017k/s), about 395 ARM cycles per guest instruction compiled** (gcbench 3/4 compile-speed kernel) [device; 2026-10-10 / 2026-10-11] bench/results/latest.txt L47; gcbench4_2026-10-11.txt L84 *Bench JIT, not core/src/jit. Optimisation passes added since would raise it Informs: Budget for heavier region or optimising JIT passes (about 50 ms per 100k-instruction hot set at this speed)*
- JIT compile spike at title loop point: **jitc 60-66% of a second, then GX 99%; ~1 s stalls; four 'GX thread: no progress for 2 s' warnings** [device; 2026-10-11] docs/HANDOFF.md L253-255 *not followed up in HANDOFF*
- JIT fuzzer run on the 2DS: **Integer and long modes (job 30): 40,000 cases, 0 failures, 177,598 side exits. Default mode (job 29): 60,000 cases including 2,490 float, 10 failures, all float sign-of-zero (JIT +0 against interpreter -0) in ps_muls, fdivs, ps_madds, ps_nmsub and ps_div.** (--fuzz on the 2DS) [device; relay jobs 29-30, 2026-10-12] core/out/relay_29.csv, relay_30.csv *Informs: The JIT is correct on the device. Flush-to-zero has to be handled for exact floats.*
- JIT microbenchmarks per guest instruction, block linking on vs off: **alu 4.5 vs 66.2 ns (3.6 vs 53.2 cycles; 2.80 vs 31.21 host instructions). mem 7.6 vs 58.7 ns (4.86 vs 25.16 instructions). call 12.2 vs 189.6 ns. cmp 7.7 vs 130.4 ns. line1k 1.6 vs 11.5 ns.** (hwprobe, JIT of 2026-10-11) [device; 2026-10-11] core/out/hw_2026-10-11.txt (jit benchmarks) *Synthetic. The JIT has changed a lot since. Informs: Floor cost of compiled code with no fetch misses: about 1.2-6.6 host instructions per guest instruction. Dispatching unlinked costs 10-15x.*
- JIT traces A/B (blocks follow branches): **traces lost 5.5% (duplicated code); also overflow the 16 MB code buffer on the 2DS** (TP, --ab traces) [device; job 154; HANDOFF 2026-10-12] core/src/jit/jit.h:133; core/host3ds/main.c:386 *Negative result. The 2DS rewards less distinct hot code. Informs: Traces off by default; OPT_CRSTUB rationale*
- JIT traces, in-run A/B (20 s phases): **Traces off: 15.09% speed, core 0 16.75%, 9.05 fields/s. Traces on: 1.34%, 1.35%, 0.81 fields/s. With traces on, 81% of core 0's time outside compiled code was the compiler, and the 16 MB buffer flushed 5-7 times per 10 s.** (tp850 state, about 300 s) [device; relay job 4, 2026-10-12] core/out/relay_4.csv (heavy rows); '# core0 outside compiled code' *Informs: Traces thrash the code buffer through tail duplication. Rejected.*
- Known-single stores (opt 1 → 3), in-run A/B: **Cutscene: 21.32 → 21.59%, core 0 24.52 → 24.84% (+1.3%). Later: 11.80 → 11.90%, core 0 11.86 → 11.97% (+0.9%).** (tp850) [device; relay job 129, 2026-10-13] core/out/r129a.csv, r129b.csv; ab_split.py on relay_129 *Informs: Kept on; within noise*
- ldr + rev as dependent chain (guest big-endian load) vs ldr alone: **6.0 vs 4.0 cycles: +2 cycles every dependent guest load** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:152,171 *Guest mix has loads 22% of instructions (PC interpreter). Not yet priced against a frame's counters. Informs: Byte-swapped guest RAM redesign candidate*
- ldrd (independent) / ldm of 4 / ldm of 7; two ldr: **3.0 / 4.0 / 6.0 cycles; two ldr 2.1** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:151,175 *Informs: Check where JIT emits ldrd (slower than two ldr)*
- Level-1 dispatcher cost: **~8.0M guest instr/s in ~1.3M blocks/s (~6 instr per block, ~650 host cycles per block)** [device; 2026-10-11 (pre linking)] docs/HANDOFF.md L272-273 *Informs: block linking*
- lfs/lfd/stfd stubs (9b96050), then LR literals and known-single frC (afe3fc0): **9b96050: cutscene 20.35%, title 20.17%. afe3fc0: 21.02% / 20.74%. Core 0 PMU cycles rose from 774-783 to 804 M/s once the bottom-screen text moved to core 1.** (Separate runs) [device; relay jobs 66-69, 2026-10-12] core/out/relay_66-69.csv *Informs: Anything else on core 0 costs about 3% of its cycles*
- Link requests (job 185): **155,000/s only in the state-load window; 10-140/s in play** [device; 2026-10-15] docs/HANDOFF.md L1146
- Linking (level 2) A/B, title scene: **9.2% linked vs 8.0% unlinked (7.2-8.7% the day before); core 0 then waited for the GX thread 53%; GX thread 78% busy, vertices 39%** [device; 2026-10-11] git 7f2033e body *Informs: GX became the limit, leading to the vertex worker*
- Memory-access stubs (memstub), in-run A/B: **First 295 s: 15.57 → 16.43% (+5.5%). Core 0 own 16.24 → 17.02%. I-cache misses 12.36 → 9.65 M/s. Fetch stalls 369 → 299 M cycles/s. ARM instructions 218 → 243 M/s. Later stretch: 12.81 vs 12.50% (noise).** (Title from boot, 20 s phases) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 18 (= perf_2026-10-11_memstub.csv) *Informs: Fewer hot words beats fewer executed instructions. Code size is the fetch lever.*
- Movie section fallback: psq_st: **psq_st 3.47-3.56 M per 10 s (~350k/s), plus stmw/lmw ~8.4k, mtspr 922/923 2.4k each per 10 s** (THP movie section, emulated ~96.5 s+) [device; jobs 101, 157] '# fallbacks 10s' lines at 550-890 s, relay_101.csv and relay_157.csv *Still present on the 10-14 build; quantized-store type not identified in the log Informs: Compiling this psq_st form (or a native THP decoder) is the lever for the 4.3 % section*
- One-compare MEM1 check (bf169a0): **Cutscene 21.49%. Title heavy rows 20.69%.** (Separate runs) [device; relay jobs 74-75, 2026-10-12/13] core/out/relay_74.csv, relay_75.csv *HANDOFF reads 21.01 → 21.19 Informs: Small gain (about +0.2 against job 70)*
- One-compare memory check (bf169a0): **cutscene 21.01 -> 21.19%; title 20.62% (fewer heavy rows, not comparable)** [device; 2026-10-13] docs/HANDOFF.md L778-782
- OPT_BEFLOAT (float stubs in big-endian mode) A/B: **core0 cutscene 25.28 -> 25.09 (-0.8 %), later 12.50 -> 12.51; I-misses +7 % / +6 %** (600 s) [device; job 166 (2026-10-15)] core/out/relay_166.csv, ab_split.py --col opt (627 vs 2675) *Informs: Off; added stub code costs what shorter latency saves*
- OPT_BEFLOAT (SETEND BE float load/store stubs): **core 0 -0.8% / +0.1% (neutral); I-cache misses +6-7%; off** (2DS job 166) [device; 2026-10-15] docs/research/REDESIGN_CANDIDATES.md:217-220; OPT_JIT_PLAN.md:86-91 *Plan predicted 'no code growth', but BE stubs were added beside originals; ~4 cycles saved per float load did not show. Informs: Re-try only if BE stubs replace originals*
- OPT_BEFLOAT (SETEND BE float stubs): **Neutral; added stubs grew the footprint, I-cache misses +6-7%** (tp850 A/B) [device; 2026-10-15, job 166] memory/gc3ds-jit-dead-ends.md:28-29 (job 166) *Contradicts the model's 27.6% float ld/st share prediction Informs: Only worth retrying as a replacement of original stubs*
- OPT_BEFLOAT big-endian float stubs (job 166): **neutral: core 0 cutscene 25.09 on vs 25.28 off; later 12.51 vs 12.50; I-cache misses +6-7% (~800 words of new stubs beside old)** [device; 2026-10-15] docs/HANDOFF.md L1079-1082 *Informs: off; latency shortening without footprint reduction does not pay*
- OPT_CALLS (traces follow bl/blr): **Within noise** (tp850 A/B) [device; 2026-10-14, job 147] memory/gc3ds-aot-verdict.md:22 (job 147) *Traces are off on the 2DS (code buffer overflow, two open trace-mode fuzzer bugs) Informs: Rejected, off*
- OPT_CALLS (traces follow calls) A/B: **cutscene 20.4% -> 19.9%; later 11.0% -> 11.4%; both within noise; off** (2DS job 147, --traces on --trace-side off --calls on --ab traces, 600 s, 20 s phases) [device; 2026-10-14] docs/research/AOT_PLAN.md:85-87 *Informs: Calls in traces rejected*
- OPT_CALLS (traces follow calls) A/B: **core0 cutscene 24.47 off / 24.37 on, later 12.08 / 12.19; fetch stalls +17 % / +21 %; ~7.4k calls followed per 10 s** (400 s) [device; job 147] core/out/relay_147.csv, ab_split.py --col traces (opt 11) *Informs: Off*
- OPT_CRSTUB (CR update via shared stub) A/B: **core0 cutscene 25.06 -> 24.91 (-0.6 %), later 12.47 -> 12.49** (600 s) [device; job 155] core/out/relay_155.csv, ab_split.py --col opt (115 vs 243) *Informs: Rejected: fewer words paid back by BL/BX*
- OPT_CRSTUB (CR updates in shared stubs): **Neutral; call/return cost ate the 3 words saved per compare** (tp850 A/B) [device; 2026-10-14, job 155] memory/gc3ds-codesize-candidates.md:21 (job 155) *Informs: Rejected*
- OPT_CRSTUB (job 155): **neutral: core 0 cutscene 24.91 on vs 25.06 off; later 12.49 vs 12.47; expected ~3-4% of static hot words** [device; 2026-10-14] docs/HANDOFF.md L1022-1025 *Informs: off; BL/BX per compare pays back saved words*
- OPT_CRSTUB (shared CR-update stubs) and OPT_ROTMASK (rlwinm as shifts): **Both neutral on the 2DS; both off** (2DS jobs 155 (CRSTUB), 156 (ROTMASK)) [device; 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:194-195 *Words shaved per instruction stopped showing on hardware; candidates 3 (float ld/st, ~1% of hot words) and 4 (pinning) parked. Informs: Peephole size cuts exhausted; need a footprint measure*
- OPT_CRSTUB and OPT_ROTMASK: **CRSTUB job 155: core 0 cutscene 24.91 on vs 25.06 off, later 12.49 vs 12.47 (neutral). ROTMASK job 156: within noise** [device; 2026-10-14] git b17e546 HANDOFF diff; a02513d subject *Informs: Shorter sequences have stopped paying*
- OPT_FCACHE (C11, float lanes in VFP within a block): **+3.5% cutscene, -3.5% later stretch: net zero** (tp850 A/B) [device; before 2026-10-15] memory/gc3ds-jit-dead-ends.md:19 *Informs: Rejected; code growth cancels the gain*
- OPT_FCACHE 2DS A/B (three 600 s runs, jobs 132-134): **Cutscene core 0 +4.3%/+3.5%/+3.0% (22.42->23.38, 22.45->23.23, 22.49->23.16); later stretch -2.7%/-2.9%/-4.4% (11.56->11.25, 11.46->11.13, 11.66->11.15). Net zero; off by default** (2DS tp850, --ab fcache, same build x3) [device; jobs 131-134, 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:148-151 *Job 131 (first pass) +1%/noise. Scene-dependent real effect. Informs: C11 off; re-evaluate if blocks get longer*
- OPT_HLELINK A/B: **core0 cutscene 24.49 -> 24.54 (+0.2 %), later 12.02 -> 12.36 (+2.8 %)** (600 s; second job without restart (heap block 1.1 MB)) [device; job 150] core/out/relay_150.csv, ab_split.py --col opt (3 vs 67) *Informs: Default on*
- OPT_HOIST (check hoisting) A/B: **core0 cutscene 23.50 -> 24.83 (+5.7 %), later 11.54 -> 11.86 (+2.8 %)** (600 s A/B) [device; job 117 (2026-10-13 night)] core/out/relay_117.csv, ab_split.py --col opt (0 vs 1) *HANDOFF: +5.0 % / +4.7 % (older split) Informs: Made default*
- OPT_HOIST (check hoisting) A/B, core 0 speed: **+5.7% core 0 (job 97); +5.0% cutscene and +4.7% core-0-bound section (job 117)** (tp850 state, --ab opt) [device; jobs 97, 117] core/host3ds/main.c:392 *The largest single JIT gain recorded in source. Informs: OPT_HOIST on by default*
- OPT_ICBI (icbi handled in-block): **No change on the device** (tp850, load-only) [device; 2026-10-15, jobs 186-187] memory/gc3ds-jit-dead-ends.md:33 (jobs 186/187) *Informs: Rejected*
- OPT_ICBI (icbi inside blocks): **qemu TP 850-880: dispatcher 'other' returns 636,584 -> 23,792; 2DS jobs 186/187: load window's ~622,000 icbi exits unchanged, speed identical; icbi 1.4% of instructions in qemu window, 2DS ~15k fallbacks/s in measured scenes** [device; 2026-10-15, 308e516] docs/HANDOFF.md L970-971, L1147-1150 *why the 2DS still exits after icbi is open Informs: off; matters only while loading*
- OPT_IND2 (2-way indirect table): **Slot-conflict misses 3.7k -> 1.2k/s; speed unchanged** (tp850 A/B) [device; 2026-10-15, job 161] memory/gc3ds-codesize-candidates.md:17; memory/gc3ds-jit-dead-ends.md:23 (job 161) *Informs: Rejected, off*
- OPT_IND2 (2-way indirect table) A/B: **core0 cutscene 25.07 -> 25.08, later 12.48 -> 12.54 (+0.5 %); slot-taken misses ~3.67k/s -> 1.2-1.3k/s** (600 s) [device; job 161] core/out/relay_161.csv, ab_split.py --col opt (115 vs 1139); '# dispatches/s' *Informs: Rejected: a dispatcher round trip is cheap; ~2.4k fewer/s buys nothing*
- OPT_KSTORE (known-single stores skip round trip) A/B: **+1% core 0, within noise** (--ab kstore) [device; job 129] core/host3ds/main.c:393 *The model predicted psq_st ~49 and stfs ~28 modelled cycles (jit.c:4356), but the device gain was within noise. Correctness checked by the fuzzer (opt 3). Informs: Kept on (exact, no loss)*
- OPT_KSTORE (known-single stores): **+1%** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:16-17 *Informs: Kept default*
- OPT_KSTORE (known-single stores) A/B: **core0 cutscene 24.52 -> 24.84 (+1.3 %), later 11.86 -> 11.97 (+0.9 %, n 32/27)** (400 s A/B) [device; job 129] core/out/relay_129.csv, ab_split.py --col opt (1 vs 3) *Within ~1.5 SE Informs: Default on (exact)*
- OPT_LATCH (budget check only in blocks that may loop) A/B: **job 169 (partial, 204 s): core0 cutscene 25.46 -> 26.06 (+2.4 %), I-misses -4 %; job 170 (600 s): cutscene 25.21 -> 25.41 (+0.8 %), later 12.49 -> 12.63 (+1.1 %), later I-misses 4.61 -> 4.30 M/s; game speed cutscene 21.45 -> 21.86** (owner-approved timing drift) [device; jobs 169-170 (2026-10-15)] core/out/relay_169.csv, relay_170.csv, ab_split.py --col opt (627 vs 4723) *Informs: Default on (small, consistent)*
- OPT_LATCH (budget checks only in loop-closing blocks) A/B: **job 169 core 0 +2.4% cutscene; job 170 +0.8% / +1.4%; I-cache misses -1 to -5%** (tp850, owner decision 2026-10-15 allows event-timing drift) [device; jobs 169, 170] core/host3ds/main.c:401 *relay.sh:65-66 says jobs 168-171 logs had been cut at ~410 s by fetching only the first page. If they were not refetched, the later stretch (303-576 s) of jobs 169/170 is truncated. The model predicted 2.5% (jit.h:121). Informs: OPT_LATCH on since job 170*
- OPT_LATCH (budget checks only where loops need them; timing drift): **Job 169 partial: core 0 +2.4% cutscene; job 170 full A/B pending** (tp850, fuzz + qemu frame clean) [device; 2026-10-15, jobs 169-170] memory/gc3ds-timing-drift-approved.md:21-22 *Partial result; full A/B result not in memory notes Informs: Owner-approved timing drift lever*
- OPT_LATCH device A/B: **Job 169: core 0 26.06 vs 25.46 (+2.4%); job 170: cutscene 21.86 vs 21.45, core 0 25.41 vs 25.21, later 12.58 vs 12.41, I-cache -1% / -5%; full-length refetch: +0.6 to +1.9%** [device; 2026-10-15] git 2d30184, e2d52e9 bodies *Informs: On by default (event timing may drift)*
- OPT_NATIVE2 (jobs 160, 162): **core 0 cutscene +0.5 / +0.9%; Link/Epona +1.5 / +1.4%; I-cache misses there -9 / -4%** [device; 2026-10-14] docs/HANDOFF.md L1046-1047 *Informs: default on*
- OPT_NATIVE2 (ps_cmp*, lmw/stmw, divw, sraw compiled) A/B: **cutscene +0.5 / +0.9%; Link / Epona +1.5 / +1.4%** (two jobs, two values each) [device; jobs 160 and 162] core/host3ds/main.c:398-399 *Small, roughly 1-2 standard errors (from ab_split.py style). Informs: OPT_NATIVE2 on by default*
- OPT_NATIVE2 (ps_cmp*, lmw/stmw, divw, sraw compiled; made default): **Later stretch +1.4-1.5%** (tp850 A/B) [device; 2026-10-15, 9bf332e] memory/gc3ds-codesize-candidates.md:16 *Informs: Native fallbacks pay; kept*
- OPT_NATIVE2 (ps_cmp*, lmw/stmw, divw/divwu, sraw compiled) A/B, two runs: **core0 cutscene 25.17 -> 25.29 (+0.5 %) and 25.05 -> 25.27 (+0.9 %); later 12.40 -> 12.59 (+1.5 %) and 12.44 -> 12.62 (+1.4 %); later I-misses 4.73 -> 4.32 and 4.52 -> 4.35 M/s** (600 s each) [device; jobs 160, 162] core/out/relay_160.csv and relay_162.csv, ab_split.py --col opt (115 vs 627) *Informs: Default on; remaining fallbacks are sc/mftb/rfi/mtspr*
- OPT_NATIVE2 and OPT_IND2: **~7k/s interpreted ps_cmp, lmw/stmw, divw and sraw (job 157); NATIVE2 jobs 160 / 162: core 0 +0.5 / +0.9% cutscene, +1.5 / +1.4% later (on). IND2 (2-way table; ~3,400 misses/s were slot conflicts) job 161 neutral** [device; 2026-10-15] git f1cdcfb, 9bf332e bodies *Informs: NATIVE2 on, IND2 off*
- OPT_RETSTUB (job 148): **qemu: bclr 6.8 -> 3.8 words, all hot 4.80 -> 4.70 (-2.1%); 2DS core 0 cutscene 24.40 +- 0.21 on vs 24.10 +- 0.24 off, later 12.13 +- 0.07 vs 12.34 +- 0.40; I-cache 6.69 vs 6.71 M/s; trimmed means: +0.8% cutscene / +1.6% later** [device; 2026-10-14] docs/HANDOFF.md L976-982, L1003, L1005-1007 *plain means read 'within noise'; trimmed means reversed later-stretch sign*
- OPT_RETSTUB + OPT_SYSCALL + OPT_HLELINK (--ab stack, job 151): **core 0 cutscene 24.47 -> 25.00% (+2.2%), Link/Epona 12.04 -> 12.54% (+4.2%); alone: retstub +0.8/+1.6, syscall +1.8/+2.0, hlelink +0.2/+2.8 (cutscene/later)** [device; 2026-10-14] docs/HANDOFF.md L999-1004 *Informs: default on; removing C round trips pays*
- OPT_RETSTUB + OPT_SYSCALL + OPT_HLELINK (made default): **Core 0 cutscene +2.2%, Link/Epona stretch +4.2%** (tp850 A/B) [device; 2026-10-14, job 151] memory/gc3ds-codesize-candidates.md:14-15 (job 151) *Informs: Removing dispatcher/C trips pays; kept*
- OPT_RETSTUB + OPT_SYSCALL + OPT_HLELINK together: **+2.2% / +4.2% (cutscene / later stretch, core 0)** (2DS job 151) [device; job 151, 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:193-194 *Which arm is which scene inferred from the file's usual order; file says only '+2.2% / +4.2%'. Informs: All three on by default*
- OPT_RETSTUB A/B: **-2.1% hot words; core 0 +1.0% / +1.6% (trimmed means)** (2DS job 148) [device; job 148, 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:192-193 *Informs: Default on*
- OPT_RETSTUB A/B: **core0 cutscene 24.45 -> 24.65 (+0.8 %), later 12.06 -> 12.25 (+1.6 %); later I-misses 5.09 -> 4.84 M/s** (600 s) [device; job 148 (2026-10-14 night)] core/out/relay_148.csv, ab_split.py --col opt (3 vs 19) *Informs: Part of the 'stack' default*
- OPT_ROTMASK (job 156): **within noise: core 0 cutscene 25.21 on vs 25.00 off, later 12.49 vs 12.46; rlwinm averaged 4.9 static hot words** [device; 2026-10-14] docs/HANDOFF.md L1030-1031, L1040-1042 *Informs: off; word shaving stopped paying*
- OPT_ROTMASK (rlwinm as shifts): **Within noise** (tp850 A/B) [device; 2026-10-14, job 156] memory/gc3ds-codesize-candidates.md:21 (job 156) *Informs: Rejected, off*
- OPT_ROTMASK (rlwinm as shifts) A/B: **core0 cutscene 25.00 -> 25.21 (+0.8 %), later 12.46 -> 12.49** (600 s) [device; job 156] core/out/relay_156.csv, ab_split.py --col opt (115 vs 371) *Within noise Informs: Off; shaving words off common sequences has stopped paying*
- OPT_SYSCALL A/B: **core0 cutscene 24.49 -> 24.94 (+1.8 %), later 11.99 -> 12.23 (+2.0 %); dispatcher 'other' exits ~10-12.4k/s -> ~4.8-5.2k/s** (600 s) [device; job 149] core/out/relay_149.csv, ab_split.py --col opt (3 vs 35); '# dispatches/s' lines *Informs: Removing C round trips pays more than shortening sequences*
- OPT_SYSCALL and OPT_HLELINK: **SYSCALL job 149: core 0 cutscene 24.88 +-0.19 vs 24.39 +-0.19, later 12.21 +-0.06 vs 11.94 +-0.08; I-cache misses -4.7% / -7%. HLELINK job 150: later 12.36 +-0.07 vs 11.93 +-0.10 (~21k stand-in calls/s)** [device; 2026-10-14] git 509061e body *Informs: On by default*
- Paired-single code execution rate: **55 MIPS** (PS-heavy microbenchmark) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:407 (quoted from FINDINGS.md) *Gekko runs 486 MHz with two SP FMAs/cycle and 3-cycle FMA latency; VFP has 8-cycle latency and one FP op/cycle. Informs: FP-heavy guest code as a core-0 bottleneck*
- Pipe stores appended inline (pipefast), in-run A/B: **First 295 s: 15.21 → 15.65% (+2.9%). Core 0 own 16.04 → 16.59%. Later: 12.45 → 12.69%.** (Title from boot) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 19 (= perf_2026-10-11_pipefast.csv) *Effect is about 2 SE Informs: Kept on*
- psq_st interpreter fallbacks in a late TP scene, and speed: **117k-373k psq_st fallbacks/s with speed 3.4-5.4% (job 190 from ~479 s, job 171 from ~588 s); GQR mtspr 922/923 fallbacks 140-252/s; only 17-18 GX draws/s in that scene** (tp850 state, ~8 min in) [device; 2026-10-15, jobs 171/190] relay_190.csv and relay_171.csv '# fallbacks 10s', '# spr fallbacks 10s' lines with speed_pct *Seen in two runs. Job 195 did not reach it within 600 s, so timing varies. Not analysed in the docs I read. Informs: A quantized paired-single store path the JIT leaves to the interpreter collapses speed ~2.5x in that scene*
- retstub / syscall / hlelink alone (cutscene / Link-Epona): **retstub +0.8 / +1.6%; syscall +1.8 / +2.0%; hlelink +0.2 / +2.8%** (ab_split.py) [device; jobs 148, 149, 150] core/host3ds/main.c:396 *ab_split.py:6-8 says jobs 148-155 had cutscene-tail rows counted in the later stretch, inflating one arm. It is unclear whether these figures came before or after the --gap fix. Informs: OPT_RETSTUB / OPT_SYSCALL / OPT_HLELINK defaults*
- Returns to C dispatcher, title: **~174k/s** [device; 2026-10-11, da2aefd] docs/HANDOFF.md L239-241
- Scheduled code (--sched): model vs device: **model: interlock cycles 1.70e8 -> 1.39e8 (-18%), executed instr -4%, lwz 3.73 -> 0.78 cycles, hot words 4.7 -> 4.6; 2DS relay 91-94: cutscene 19.89 off / 19.72 on, title 18.86 / 19.94 (+-0.5), data-stall counter only -2-3%** [device; 2026-10-13] docs/HANDOFF.md L853-861 *model overpredicted device effect Informs: kept default, within noise*
- SETEND BE switch cost: **1 cycle per switch; RAM can stay in console byte order with no swaps** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:39 *Informs: Run JIT blocks under SETEND BE (adopted)*
- SETEND cost: **setend be/le 1.03 cycles latency and throughput** (gcbench 3) [device; 2026-10-10] bench/results/latest.txt L115 *Informs: SETEND around C calls and block boundaries is affordable*
- SETEND gain in gcbench: **paired +23%, float +12% with SETEND (jit-rebuild note); FINDINGS.md: +10% integer, +12% float, +14% paired over byte-swapping each access** (gcbench) [device; 2026-10-10] memory/gc3ds-jit-rebuild-after-gx.md:17; C:/Users/tyler/TP3DS/emu/docs/FINDINGS.md:149 *Paired figure disagrees (23% vs 14%) between the memory note and FINDINGS.md; in TP, OPT_BEFLOAT measured neutral (job 166) Informs: Basis for OPT_BEFLOAT float stubs*
- Shared float stubs (b7534e3): **Cutscene 19.92%, title 19.85%. I-cache misses 6.85/6.83 M/s. Fetch stalls 221/207 M cycles/s. 13.7-14.0 host words per guest instruction.** (Separate runs) [device; relay jobs 62-63, 2026-10-12] core/out/relay_62.csv, relay_63.csv *Informs: Float code size is a fetch lever*
- Shared memory-access stubs A/B (3bffa35): **on 15.26% vs off 14.72%; I-cache misses 12.6 -> 9.5 M/s; fetch stalls 358 -> 283 M cycles/s** [device; 2026-10-11, perf_2026-10-11_memstub.csv, ~160 rows/phase] docs/HANDOFF.md L464-465 *Informs: memory stubs on (smaller hot code wins)*
- Shared slow-path stubs: **qemu: cold words per guest instruction 18.6 -> 6.0, blocks compiled 266,847 -> 131,910, flushes 21 -> 7 (slow paths had been ~2/3 of compiled code). 2DS: cutscene 15.52 / 15.53% (no flushes either way); title from boot 16.12% with 0 compile fails (391 before) but buffer 97% full; +3% title, fetch stalls unchanged; hot region raised to 62.5%** [device; 2026-10-12] git 385c1ae, bd25360 bodies; 9ac6051 subject *Informs: Code buffer split*
- Shared slow-path stubs (385c1ae), relay 40-45: **title from boot 16.0-16.1% (was 15.7%); compile fails 391 -> 0; buffer 65% full after 240 s (was 97%); 14.6 host words/guest instr (was ~27); tp850 cutscene unchanged 15.5%; --slow inline/stub 15.53 / 15.52** [device; 2026-10-12] docs/HANDOFF.md L617-620 *Informs: cold code size affects capacity, not fetch time*
- Shared slow-path stubs against inline slow paths: **Host words per guest instruction: 27.4-27.8 (jobs 7/8) → 14.5-15.1 (jobs 40, 42, 45); 23.9 in the inline arm (job 41). Cutscene: stubs 15.63%, inline 15.65%; buffer 66% vs 70%; I-cache misses 9.04 vs 9.88 M/s; fetch stalls 284 vs 305. Title heavy rows: 15.93% (job 39) → 16.25/16.33% (jobs 42/43). 0 compile failures.** (tp850 cutscene (40/41) and title from boot (39, 42, 43)) [device; relay jobs 39-43, 2026-10-12] core/out/relay_39-43.csv ('# jit code' and '# jit cache' lines) *Separate runs, not in-run A/B Informs: Slow paths free buffer capacity but barely move speed*
- Stale JIT code without I-cache invalidation: **199/200 stale** (JIT write then execute) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:396 (quoted from FINDINGS.md) *Correctness requirement; its cost is not given here. Informs: Invalidate cost in block compilation*
- State-load window compile cost: **first 10 s: 31,737 compiles, jitc_pct 33% mean (18% median), 1,464 compiles/s mean; links 155k requests/s with 151,895/s 'not linkable here'** (tp850 load) [device; 2026-10-15, jobs 185-188] relay_185.csv, relay_186.csv, relay_187.csv perf rows 0-10 s; relay_171.csv/relay_188.csv '# jit repeats'; relay_185.csv '# link requests/s not linked' *Informs: Compile cost matters only while loading; linking in play is 9-141/s (nothing to fix)*
- Static JIT code size and buffer use (clean runs): **~8.4-8.6 host words per guest instruction; per-instruction hot 2.2 / exit stubs 1.1 / cold 1.4 words; ~27k blocks and 46-47 % of the buffer at 245 s; 42-43k blocks and 68-71 % of the buffer at 550-884 s** (no A/B (A/B runs recompile and show ~18k blocks, 29-30 %)) [device; jobs 101-157] '# jit code' / '# jit words' / '# jit cache' lines in relay_101, 130, 157.csv *Informs: Code buffer headroom for larger code (traces, unrolling) is limited over long sessions*
- Taken unconditional branch, 16-1024 branches 8 B apart: **4.75-4.02 cycles per branch; no capacity cliff, no BTAC folding seen** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:153,172 *SD5113 prior: 128-entry BTAC; not observed. Informs: Fall-through layout of hot paths*
- Trace-mode bisect (90 s runs from tp850): **Full traces: 1.34-1.41% (jobs 17, 25, 26, 31, 33, 34; 5-7 flushes per 10 s). Off: 14.14-14.85% (jobs 20, 32, 35; 0 flushes). Follow branches only: 14.93% (job 18). Side exits only: 12.57-13.02% (jobs 19, 22, 27). Side modes 2/4: 14.14/14.21% (jobs 23, 24). Trace length 12/20/32: 13.19/12.29/5.01% (jobs 36-38).** (tp850 state, 90 s each) [device; relay jobs 17-38, 2026-10-12] core/out/relay_17-38.csv ('# jit cache' lines; mean speed_pct from 25 s) *Short runs; means include the post-flush warm-up Informs: The cost comes from code-buffer capacity, not a hardware bug*
- Traces (blocks follow branches): **Core 0 -5.5% in cutscene; I-cache misses +17% (duplicated tails grow the footprint)** (tp850 A/B) [device; 2026-10-14, job 154] memory/gc3ds-codesize-candidates.md:20; memory/gc3ds-jit-dead-ends.md:18 (job 154) *Informs: Rejected*
- Traces A/B once fitting: **cutscene 15.52% on and off; title 15.94% on vs 16.03% off** [device; 2026-10-12, relay 40-45] docs/HANDOFF.md L621 *Informs: traces stay off*
- Traces after the buffer stopped thrashing: **Cutscene with traces: 15.63% (job 44), against 15.63-15.65% off (jobs 40/41). Title: 16.21% (job 45) against 16.25-16.33% (jobs 42/43). ARM instructions 242.5 → 233.8 M/s and mispredicts 578 → 384 k/s, but I-cache misses about 9.0 M/s either way.** (Separate runs) [device; relay jobs 44-45, 2026-10-12] core/out/relay_44.csv, relay_45.csv against relay_40-43 *Two open trace-mode fuzzer bugs (from the docs) Informs: Traces give nothing once they fit. Rejected.*
- Traces as default (job 154): **core 0 cutscene -5.5%; I-cache misses +17%; later stretch even; executed words -18% under qemu** (2DS job 154) [device; 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:204-206 *Rejected: tail duplication grows footprint. Informs: Traces rejected; any region design must bound duplication*
- Traces bisect (2DS): **follow-only 15.0%; side exits only 12.2-12.9%; linking off no help; 2DS fuzzer 177,598 side exits 0 differences** [device; 2026-10-12] docs/HANDOFF.md L546-549
- Traces on the 2DS (relay jobs 4-35): **1.3-1.4% with traces vs 14-17% off; 16 MB buffer filled every ~1.7 s (5-7 flushes/10 s); ~6,000 compiles/s, 98% new PCs; ~14,000 entry points/interval; trace blocks ~2x longer, ~26 MB working set; without traces ~21,500 blocks, 0 flushes; qemu 50 fields 8 MB: 60 vs 35 flushes, 4.18M vs 2.42M guest instr compiled** [device; 2026-10-12] docs/HANDOFF.md L538-549 *Informs: traces off; tail duplication must be bounded*
- Traces re-measured on today's defaults: **core0 cutscene 25.10 off -> 23.64 on (-5.8 %), later 12.38 -> 12.19 (-1.5 %); I-misses +16.5 % / +8.3 %; fetch stalls +27 % / +16 %** (600 s, opt 115) [device; job 154] core/out/relay_154.csv, ab_split.py --col traces *Informs: Rejected: more distinct hot code loses even with fewer executed words*
- Traces, earlier measurement: **executed ARM instructions -5%, mispredicts ~-30%, no I-cache miss reduction, no speed gain; before code-size work overflowed 16 MB buffer and thrashed (1.3% speed)** (2DS, --traces on, pre-2026-10-12) [device; before 2026-10-12] docs/REGION_JIT_PLAN.md:26-31 *Superseded by job 154 (misses +17%, -5.5%). Informs: Same*
- VFP divide and square root: **f32 div 19.01 / 17.01, f32 sqrt 19.00 / 17.00, f64 div 33.01 / 31.01 cycles (latency / throughput)** (gcbench 3) [device; 2026-10-10] bench/results/latest.txt L99-100, L104 *Informs: Use single precision for fdiv/frsqrte where the guest allows it (C8 single precision)*
- VFP f32 add/mul/mla latency and throughput: **latency 8.00 cycles, throughput 1.10 (gcbench 4: 1.09)** (gcbench 3/4 VFP kernels at 804 MHz) [device; 2026-10-10] bench/results/latest.txt L94-98; gcbench4 L165-175 *Timingprobe job 176 gives independent f32 add at 2.0 cycles throughput, not 1.1. The difference is method (unrolled independent chains) Informs: Dependent float chains cost 8 cycles per step, so interleave independent work*
- VFP f64 add/mul/mla: **f64 add 8.00 / 2.00; f64 mul 9.00 / 2.28; f64 mla 9.00 / 2.28 cycles** (gcbench 3) [device; 2026-10-10] bench/results/latest.txt L101-103 *Informs: Double-precision guest ops cost about 2x f32 throughput, which argues for single-precision paths*
- VFP int/float convert and core<->VFP moves: **cvt f32<-s32 9.00 / 1.10; cvt s32<-f32 8.00 / 1.09. vmov s<-r 4.50 / 3.50; r<-s 4.00 / 3.50; 2s<-2r 4.50 / 3.50; 2r<-2s 4.00 / 4.00; s<->r round trip 5.00 / 5.00 cycles** (gcbench 3) [device; 2026-10-10] bench/results/latest.txt L108-114 *Timingprobe job 176 measures core->VFP->core at 10 cycles and vldr-then-use at 8 Informs: Paired loads through integer registers pay 3.5-4 cycles per lane. Float-type loads should go straight to VFP (SETEND BE)*
- VFP latency measured on device: **add/mul 8-9 cycles; div/sqrt 17-19 cycles** (VFP11 microbenchmarks) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:287 (quoted from FINDINGS.md) *Agrees with the TRM: 8-cycle single FADD/FMUL/FMAC, FDIV/FSQRT 19 latency single. Informs: FP code scheduling in the JIT; how expensive paired singles are*
- VFP short-vector mode throughput (negative result): **no throughput gain** (short vectors up to 8 single / 4 double) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:288-289 (quoted from FINDINGS.md) *The TRM explains it: short vectors issue serially. Informs: Rejects short-vector paired-single emulation*
- VFP short-vector pair ops: **pair add/mul/mla 9.00 latency / 2.28 throughput, the same as two scalar ops** (gcbench 3, FPSCR LEN=2) [device; 2026-10-10] bench/results/latest.txt L105-107 *Negative result: vector mode saves code size only Informs: Reject VFP short vectors for speed. Use them only if code size matters*
- What still leaves compiled code (job 157): **indirect misses ~3,400/s (nearly all slot taken); other returns ~4,600/s (polling loops), mtmsr ~600/s; fallbacks ps_cmpo0 ~2k/s, lmw+stmw ~2.5k/s, mftb ~1k/s, divw ~1k/s, sraw ~0.7k/s** [device; 2026-10-14] docs/HANDOFF.md L1043-1045
- 8-lane float register cache simulation (FPR lanes in d8-d15): **~1.6% of executed words saved; 49.8% of vldr would hit; 13.5% of vstr overwritten in-block** (PC simulation over the same 30-field run) [pc; 2026-10-12] docs/REGION_JIT_PLAN.md:35-37; OPT_JIT_PLAN.md:70-72 *Informs: Float lanes only with longer-lived float code (later built as C11)*
- AOT build size for the 3DS: **.3dsx 14,806,036 bytes; build 7m04s real** (AOT 90% coverage build) [pc; 2026-10-14] aot/g_3ds.txt *Build-host timing only. Informs: memory budget of translated code (AOT_PLAN: a 28 MB build starved malloc; 0.97 coverage left 0.4 MB largest block)*
- AOT exactness on PC: **120 fields: 10.7 M calls, 168 M guest instructions, 0 differences (2,730 calls differ by 1 ulp in double fmadd, libm sin/cos); last frame byte-identical (md5 09585a69...)** (PC, AOT=1 AOT_CHECK=1, TP from field 900) [pc; 2026-10-14] docs/research/AOT_PLAN.md:37-44 *Informs: Check-mode infrastructure reusable*
- AOT fallbacks by address (what the translator still interprets): **28,397 fallbacks over 120 fields: 25,529 (90%) at one sc 8033b5d4; mftb at 80342714/803426fc/80342704/80342700 2,513; sc 8033b604 115; mtspr GQR 914-917 240** (PC, AOT on, TP 900-1020) [pc; 2026-10-14] aot/fb.txt *PC only. Informs: one syscall site dominates residual interpretation*
- AOT interpreter fallbacks over 120 fields: **1,008,067 -> 28,397 after inlining cache instructions** (PC) [pc; 2026-10-14] docs/research/AOT_PLAN.md:108-109 *Informs: Inline cache-op handling*
- Block-boundary register traffic: **~25% of what core 0 executes; ~4.5 loads+stores per block run; blocks average 5.9 guest instructions; ~6.4 hot ARM words per guest instruction** (PC build 3a1696f, JIT_REGSTAT=1, tp_arm_850.state, 30 fields, traces and shadow stand-in off) [pc; 2026-10-12] docs/REGION_JIT_PLAN.md:13-17 *Informs: Region compilation (later found worth only ~1.3%)*
- Per-register loads / write-backs per block run: **CR 0.39/0.38, r3 0.28/0.35, r0 0.04/0.47, r4 0.15/0.25, r6 0.19/0.17, r1 0.22/0.13, r31 0.21/0.12, r30 0.16/0.10** (PC JIT_REGSTAT, build 3a1696f, 30 fields) [pc; 2026-10-12] docs/REGION_JIT_PLAN.md:16-19 *r0 dead write-backs suggest cross-block liveness; owner approved dropping dead write-backs 2026-10-15. Informs: Cross-block liveness / dead state at exits*
- Pinning guest registers to host registers (model): **4 slots instead of 5: +0.47 loads/stores per block run; pinning CR saves 0.77; net ~1.5%** (PC model) [pc; 2026-10-12] docs/REGION_JIT_PLAN.md:32-34 *Rejected; C13 candidate 4 (register pinning) parked on the same grounds. Informs: Pinning rejected*
- Region model: registers kept across blocks; loop regions: **~1.3% saved (across blocks); ~0.6% (loop regions) with five cache slots** (PC model (JIT_REGSTAT simulation)) [pc; 2026-10-12] docs/OPT_JIT_PLAN.md:26-28,81-83 *Negative result; re-stated as still holding 2026-10-15. Informs: Region compilation ruled out*
- A/B (model): float store / paired-single rework, executed words: **dynop 6.71 -> dynop2 6.40 words per instruction (-4.6%); psq_st 39.3 -> 30.2; stfs 15.4 -> 10.4; ps_madds0/1 24.0 -> 18.0; ps_muls0 22.0 -> 16.0; code 6,243 -> 6,374 KB** (qemu, same 30 fields) [qemu_model; 2026-10-12] dynop.txt vs dynop2.txt *No device A/B in these files. Informs: magnitude of PS codegen tweaks*
- AOT translation to C (C12): coverage and cost: **PC: 96.7% of executed guest instructions translated, AOT_CHECK 10.7M calls 0 differences; 3,030 inlined call sites (-35% calls); top-level calls 7.5M -> 2.9M; 13,704 functions found; fallbacks 1,008,067 -> 28,397 per 120 fields; qemu ARM 9.66 -> 9.37 instructions per guest instruction vs JIT 8.04** [qemu_model; 2026-10-14] git 24f05d5, 9c5b797, 9b9d753, fbcb703, 4b3da7c, 720e400 bodies *Informs: AOT*
- ARM instructions per guest instruction executed inside the JIT buffer (JIT only, no AOT): **8.04 (30,365,361 guest instructions, 244.1 M ARM instructions in range 43804000-44004000)** (TP from tp_arm_850.state, fields 850-870 (20 fields), qemu-arm rangecount plugin, ARM build; excludes dispatcher, helpers and stubs outside the buffer) [qemu_model; 2026-10-14] //wsl$/Debian/root/aot/g_meas.txt line 1; aot/rc/jit.plugin, aot/rc2/jit.plugin *In-buffer only. Device-derived total on core 0 is ~12 ARM per guest instruction (see job 93 entry). qemu counts instructions, not cycles. Informs: baseline host-code density of the current JIT; whether a redesign must cut ARM/guest*
- ARM instructions per guest instruction inside translated (AOT) functions vs JIT: **AOT 10.26 -> 9.37 after register/entry work; JIT 8.04 inside its buffer** (qemu, TP 850-870, AOT_PROF counters) [qemu_model; 2026-10-14] docs/research/AOT_PLAN.md:111-113 *JIT figure excludes dispatcher and helpers. Informs: Executed instructions are not the deciding factor*
- ARM per guest instruction with AOT on: JIT part and translated (AOT) part: **JIT 4,494,618 guest at 10.77 ARM each; AOT 25,870,766 guest at 9.38 ARM each in 542 translated functions** (same 20-field qemu stretch (850-870), AOT build with span counting) [qemu_model; 2026-10-14] aot/g_meas.txt lines 2-4; aot/ab_533458.plugin (range1 245,217,350 insns); aot/rc/aot.plugin *AOT_PLAN.md: device A/B job 144 found translated code slower (cutscene 20.2% JIT vs 14.4% AOT) although qemu counted only ~17% more ARM per guest; qemu instruction counts mispredicted the device outcome (I-fetch bound). Informs: AOT vs JIT code quality; AOT was later rejected on device*
- ARM11 interlock model (arm11_sched.py) calibration and TP breakdown: **model 0.34 stall cycles/instr from interlocks vs device 0.64 (rest ~1.3M D-misses/s x ~50 cycles); interlocks: loads 63% (lwz 17.5%, 3.7 cycles each), VFP 23% (fmuls 15.7 cycles each, psq_st 14.3, ps ops 11-12, psq_l 9.6)** (TP cutscene 40 fields) [qemu_model; 2026-10-13] docs/HANDOFF.md L842-852
- ARM11 interlock model and scheduled stubs: **Loads 63% of interlock cycles, lwz 3.7 cycles each; scheduled stubs: interlock cycles -18%, instructions -4%, hot words 4.7 -> 4.6 (model). Device relay 91-92: within noise** [qemu_model; 2026-10-13] git c112731, d613012 bodies *Informs: Latency scheduling does not pay on the device*
- Block exit kinds: **6,955,941 exits: constant 79.1%, blr/bctr 20.8%, to the dispatcher 0.1%; back edges 36.0% of constant exits; constant exits to uncompiled targets 0.0%** (qemu, 30 fields) [qemu_model; 2026-10-12] regstat_base/out.txt *Informs: linking covers almost all exits; indirect (blr/bctr) at 21% is the dispatcher's load*
- Block-boundary register traffic: **~25% of executed work; 4.5 loads/stores per block run of 5.9 guest instructions; r0 written back 0.47 vs read 0.04 per run; 8-lane FPR cache: 49.8% of vldr would hit, 13.5% of vstr overwritten in-block; a float register cache ~1.6%** [qemu_model; 2026-10-12] git a8c26d8 body *Informs: Region compilation brief*
- Block-end exit mix: **40 fields: 9,100,880 exits: constant 78.5%, blr/bctr 21.4%, to the dispatcher 0.1%; back edges 33.6% of constant exits; untaken (not compiled) targets 0.0%. sch7 (traces on): 1,708,701 exits: constant 44.7%, blr/bctr 55.0%, dispatcher 0.3%, back edges 13.9%.** (As above) [qemu_model; 2026-10-13] stats2.txt line 43; sch7/log.txt line 147 *Traces absorb constant exits, which leaves blr/bctr dominant Informs: Indirect branches (returns) are about a fifth of exits untraced: return prediction / RETSTUB design*
- C13 candidate 1: CR-field dead stores (model): **exact ~0.3% of hot words; inexact ~1.5-2%** (model) [qemu_model; 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:191-192 *Rejected by model. Informs: CR liveness not worth it alone*
- Code buffer capacity after stubs went cold: **17 full flushes in 30 qemu fields (8 MB); 2DS 16 MB buffer 75% full after 4 min; fixed with hot region 37.5% (hot 5.0-5.1 vs cold ~9 words/guest instr), 0 flushes** [qemu_model; 2026-10-12] docs/HANDOFF.md L749-752 *Informs: check jit cache flushes and jitcode_pct after moving code*
- Cold-path shrink: effect on churn: **Cold words per guest 18.6 -> 6.0 -> 5.5 (coldstat 1/2/3); blocks compiled 266,847 -> 131,910 -> 24,507; flushes 21 -> 7 -> 0; blocks live 6,389 -> 13,243 -> 19,175; lwz cold 34.4 -> 14.4, stw 76.3 -> 13.9, stfs 74.6 -> 11.8 words; coldstat4 hot 6.9 -> 6.2, stubs 2.4 -> 2.0** (qemu, traces off, tp_arm_850.state to 880 (30 fields), 44,798,955 instructions each) [qemu_model; 2026-10-12] coldstat.txt, coldstat2.txt, coldstat3.txt, coldstat4.txt *Oct 14 runs (tc7 cold 12.1, fault mode on) show cold size grew again with fault-mode/other stubs. Informs: code-buffer capacity, not compile speed, drove recompiles; keep cold code small*
- Compile-to-execute ratio in qemu runs: **tc7 3.95 M compiled / 73.96 M run = 1:18.7; cs/jit 1.47 M / 30.37 M = 1:20.7; coldstat3 0.113 M / 44.8 M = 1:396; hlejit 24.3 M / 1,136 M = 1:47** (qemu runs as listed) [qemu_model; 2026-10-12 to 2026-10-14] aot/tc7.txt, aot/cs/jit.out, coldstat3.txt, hlejit.txt *Device job 93 compiles only ~32 blocks/s with 0 flushes, so qemu churn is not device-representative. Informs: compile cost matters only when the buffer overflows*
- Concentration of register traffic by block: **40 fields: top 16 blocks 18.6%, 64 36.7%, 256 62.0%, 1,024 83.2%, 4,096 97.0%; 55.7% in blocks inside a back edge's range. sch7: 24.3 / 46.1 / 70.8 / 89.6 / 98.7%; 27.3%.** (As above) [qemu_model; 2026-10-13] stats2.txt line 51; sch7/log.txt line 155 *Informs: An optimizing tier for the top ~1,000 blocks would cover ~83-90% of register traffic: argues for tiered compilation*
- Concentration of register traffic by block: **top 16 blocks 19.7%, 64 37.7%, 256 62.8%, 1024 83.6%, 4096 97.0%; 56.0% in blocks inside a back edge's range** (qemu, 30 fields) [qemu_model; 2026-10-12] regstat_base/out.txt (model1: 18.1/35.1/60.5/81.8/96.3%) *Informs: an optimizing tier on ~256-1024 hot blocks would cover 63-84% of traffic*
- Corrected qemu baseline of executed hot words: **4.80 executed hot words/guest instr (earlier tables 8.58 included stats counters); shares: lwz 11.7% (4.2 words), bc 11.4% (6.2), stw 6.5% (3.9), addi 5.4% (1.7), bclr 4.2% (6.8), cmpli+cmpi 5.6% (6.4-6.5), PS/float arithmetic ~13% (10-21 words)** (TP 850-880, traces off, fault mode off, hoist + kstore) [qemu_model; 2026-10-14] docs/HANDOFF.md L966-971
- CR-field dead-store fate model: **2.9M executed CR-field writes: fused bc next and dead every path 5.3%; dead only past load/store or call 42.2%; live 2.4% + 32.8%; unknown 17.3%; exact elimination ~0.3% of hot words, inexact ~1.5-2%** [qemu_model; 2026-10-14] docs/HANDOFF.md L972-975 *Informs: not built*
- Dispatcher returns in the ic_w850 run: **689,648 dispatches; link requests 24,879; indirect-table misses 14,853 (slot taken 7,682, to blocks compiled then 7,092, polling/idle 75); budget ends 6,250; other 642,437** (30 fields from a state load (includes the state-load window)) [qemu_model; 2026-10-14] ic_w850/out.txt lines 27-29 (ic_w950: 679,811 / 24,420 / 14,026 / 6,272 / 633,867) *Most dispatches are in the state-load window. Job 184 says ~8,500 returns/s in steady play on the device. Informs: The dispatcher is not a steady-state cost; do not redesign around it*
- Dispatcher returns with AOT on (AOT entries are never linked): **5,346,946 dispatches and 2,519,700 indirect-table misses with AOT vs 815,073 dispatches and 62,586 misses JIT-only (different lengths: 880 vs 20 fields)** (qemu, aa (850-1730, AOT) vs cs/jit (850-870, JIT)) [qemu_model; 2026-10-14] aot/aa/out.txt; aot/cs/jit.out *Run lengths differ; compare per field: aa ~6.1k dispatches/field vs cs ~40.8k/field (cs dominated by warm-up compiles). Informs: any mixed translated/JIT design must link entries; the AOT design sent every call back through the dispatcher*
- Executed hot ARM words per guest instruction (JIT_REGSTAT) and per-opcode executed cost: **6.71 words per instruction (300,807,690 words / 44,814,781 instr); bc 15.99% of words (12.2 each, 8.79% of instr); psq_l 8.33% (24.3, 2.30%); lwz 7.52% (3.8, 13.29%); psq_st 7.39% (39.3, 1.26%); b 4.67% (9.9); stw 4.13% (3.5); stfs 3.74% (15.4); addi 3.71% (1.6, 15.54% of instr); bclr 3.30% (7.5); lfs 3.08% (8.3); ps_madds0/1 2.28% each (24.0); ps_muls0 2.00% (22.0)** (qemu, traces off, 850-880) [qemu_model; 2026-10-12] dynop.txt executed by opcode *Excludes exit stubs and shared stubs; instruction words, not cycles. Informs: conditional branches and paired-single ops dominate hot code*
- Executed hot words by opcode (sch7, JIT_REGSTAT): **46,200,920 guest instructions, 310,693,249 hot ARM words (6.72 per instruction, exit and shared stubs excluded). lwz 18.81% of executed words (12.53% of instructions, 10.1 words each); stw 10.01% (9.8); b 7.07% (10.8); bclr 6.43% (13.5); stb 4.41% (10.2); addi 3.93% (1.7); psq_l 2.96% (8.2); bc 2.54% (2.1); lfs 2.53% (5.6); bcctr 2.18% (17.6); isync 1.81% (9.0); fcmpo 1.65% (17.7)** (Traces on, statistics build) [qemu_model; 2026-10-13] //wsl$/Debian/root/sch7/log.txt lines 96-126 (sch3 6.71 / schb7 6.59 words per instruction) *Words-per-op include the statistics build's counters, which the model strips (model: lwz 6.5 words) Informs: Which guest ops a new JIT must make cheap first*
- Executed words per guest instruction across statistics builds (not comparable): **dynop 6.71, dynop2 6.40, regstat_model1/2 8.11, regstat_psq 7.66, regstat_base 8.92 (lwz 9.8, stw 9.5, bc 20.2 words each in base vs lwz 3.8 in dynop)** (qemu, same 30-field stretch, different builds) [qemu_model; 2026-10-12] dynop*.txt, regstat_model1/2/out.txt, regstat_psq/out.txt, regstat_base/out.txt *regstat_base appears to use a different memory-access scheme (inline checks) and/or counters in hot code. Informs: instrumented builds change the code they measure; always compare within one build*
- Executed-word share of lesser opcodes (40 fields): **cmpli 1.73% of executed words (6.3 each), lbz 1.58% (9.9), stwu 1.46% (10.9), ps_madds1 1.29% (18.0), lfd 1.28% (10.5), ps_madds0 1.28% (17.8), cmpi 1.26% (6.4), ps_muls0 1.08% (15.7), fmuls 1.00% (10.3), fcmpo 0.82% (15.1), bcctr 0.81% (16.0), stfd 0.79% (10.2), dcbi 0.77% (12.0), ps_madd 0.76% (21.4), ps_mul 0.73% (19.8), icbi 0.70% (6.0)** (Statistics build) [qemu_model; 2026-10-13] stats2.txt lines 5-22 *stats2.txt is a tail; the top of the list is missing Informs: Paired-single arithmetic costs 16-21 words each*
- Float and paired-single share of executed hot words: **38% of executed hot words from 13% of instructions; hardware conversion when exact: executed words 300.8M -> 286.9M (psq_st 39.3 -> 30.2, stfs 15.4 -> 10.4)** [qemu_model; 2026-10-12] git 3a1696f body *Informs: Float code is the densest code*
- Float load / store share and shared float stubs: **Float loads/stores ~19% of executed hot code; plain-float GQRs are 98% of psq_l and 99.9% of psq_st; psq_l 24 -> 5.6 hot words, psq_st 31 -> 5.2; hot words per guest instruction 6.0 -> 5.1; the 2DS's 16 MB buffer was 75% full after 4 min** [qemu_model; 2026-10-12] git b7534e3 body *Informs: Shared float stubs*
- Float share of core 0 modelled cycles (D2): **43% of core 0's modelled cycles are float loads/stores and maths** [qemu_model; 2026-10-13] docs/HANDOFF.md L907 *Informs: single-precision / float register work*
- Float store cost (model): **psq_st ~49 modelled cycles, stfs ~28 (round-trip check + vmrs); 43% of modelled core-0 cycles are float work** [qemu_model; 2026-10-13] git b8428e5, 18d6248 bodies; ae4a9c9 subject *Informs: OPT_KSTORE, float lanes*
- FPR lanes (fcache) model gain re-scored with device latencies: **Off -> on (second pass): original 8.77 -> 8.47 (-3.4%); variant A 9.02 -> 8.67 (-3.9%); variant B 9.32 -> 8.82 (-5.4%). First pass: -0.5% / -1.0% / -2.4%.** (20-field dumps, same build family) [qemu_model; 2026-10-16] scratchpad re-runs on sch3, sch7 and schb7 sched.bin *The device measured net zero (jobs 132-134) Informs: Calibrating latencies widens the model's predicted gain rather than closing the gap, so the model-device disagreement comes from the missing fetch/code-size term, not latencies. Models that skip I-fetch will keep mispredicting code-growing changes.*
- Guest register cache traffic per block execution: **7,588,548 block executions for 44.8 M instructions (5.91 instr per block); loads into a slot ~2.15 and write-backs ~2.32 per block run (top 16 regs); CR 0.393/0.380, r3 0.280/0.353, r0 0.040/0.471, r4 0.147/0.246, r1 0.221/0.130, r31 0.211/0.122** (qemu JIT_REGSTAT, 850-880) [qemu_model; 2026-10-12] regstat2.txt, regstat_base/out.txt *regstat.txt (first version, same code size) reported write-backs ~6.40 per block run; regstat2 corrected the count (inferred). regstat4 build: loads 2.39, stores 2.55. Informs: ceiling for cross-block register allocation*
- HLE stand-in correctness check: **1,235 calls up to field 1100 identical to the interpreter** (qemu ARM build with JIT) [qemu_model] core/host3ds/main.c:306-309 *A correctness check, not a speed measurement. Informs: Re-enabling the stand-in*
- Host words per guest instruction by region (code layout): **8.2 hot, 2.7 exit stubs, 17.1 cold (2/3 of bytes between hot paths never run)** (qemu, TP 150 fields) [qemu_model; 2026-10-11] docs/HANDOFF.md L174-176 *Informs: hot/cold region split of code buffer*
- Hot / exit-stub / cold words per guest instruction (pre-split): **8.2 hot, 2.7 exit-stub, 17.4 cold; 1,090 blocks refused in 150 fields (tables too small); psq_st to the pipe ~8k fallbacks/s; HID0 SPR moves ~2.5k/s; psq_l / psq_st ~7k/s each interpreted (title)** [qemu_model; 2026-10-11] git 5328b90, b439173 bodies *Informs: Hot/cold split; quantized psq compile*
- Hot words per opcode (early 2026-10-12): **compares 10-11, bc 12, b 10, bclr 7.6, bl 8, stfs 15, lfs 10, psq_st 42** [qemu_model; 2026-10-12] docs/HANDOFF.md L505-509 *superseded by later per-opcode tables*
- Hot words per opcode, qtp1 (opt 1) -> qtp5 (opt 5): **fmuls 10.2 -> 5.5; fsubs 7.0 -> 4.8; b 4.8 -> 6.3; bclr 6.9 -> 8.3; bcctr 10.1 -> 11.1; fcmpo 14.4 (both); fctiwz 16.8; psq_l cold 21.1 -> 42.8. Shares of hot words: lwz ~10.3%, b 8.4-11.3%, lfs ~7%, stw 6.7-7.3%** [qemu_model] //wsl$/Debian/root/qtp1/log.txt; qtp5/log.txt *Informs: Which opcodes to shrink for I-cache footprint*
- Hot-block relayout under qemu (JIT_RELAYOUT_TEST): blocks live / hot / compiled first at each relayout step: **3 relayouts: 2/2/2 (during boot), 4,207 live / 3,861 hot / 3,861 first, 7,642 live / 399 hot / 399 first; ran clean to field 870** (build-arm-snap gcemu under qemu-arm, tp_arm_850.state, fields 850-870, fault mode on, traces on) [qemu_model; 2026-10-15 (file 13:38)] //wsl$/Debian/root/relayout/out.txt *Shows only that the relayout path runs clean in qemu; job 188's device stop was not reproduced here. The device verdict is job 195 (separate entry): later stretch -4.5%, I-cache misses +40%. Informs: Relayout rejected; code layout by hotness does not cut I-cache misses on the 2DS*
- icbi counts in gameplay: **622,595 icbi in every run loaded from the field-850 state (identical to field 1730), i.e. 0 in gameplay 850-1730; from cold boot 622,692 by field 150 and 647,642 by field 1100 (~25k during loading 150-1100)** (qemu runs tc7, tc15, ic, aa, nohle, coldstat vs opstat (boot-150) and hlejit (boot-1100)) [qemu_model; 2026-10-11 to 2026-10-14] aot/tc7.txt, aot/aa/out.txt, opstat.txt, hlejit.txt *Derived by comparing totals across runs. Informs: self-modifying-code handling can be simple for steady-state TP; REL loads cause the in-game invalidations*
- Indirect-branch table size: **16K entries: misses 86,499 -> 36,621 (79,307 were slot conflicts; a hashed index did not help). 64K entries: 36,621 -> 14,845, collisions 29,445 -> 7,674; 2 MB for four tables. 2DS after 16K: cutscene 20.73%, title 20.69%, then 21.0%** [qemu_model; 2026-10-12/13] git a1cdcfb, d613012, c4b2388 bodies *Informs: 64K table default*
- Indirect-table index folding in bits from 4 KB up: TP indirect misses: **86,499 -> 91,663 (worse)** (qemu ARM build, 30 fields from tp_arm_850) [qemu_model] core/src/jit/jit.c:5041-5043 *Rejected. The remaining misses are attributed to polling and idle blocks that never enter the table (an inference). Informs: Kept the simple (pc>>2) slot index*
- Instructions interpreted at run time (ic runs): **1.5% of instructions interpreted. icbi 622,595 (2,451 dropped all code). 850 run: ps_cmpo0 5,753, rfi 4,222, sc 3,287, lmw/stmw 2,998 each, mftb 2,573, divw 2,347, sraw 1,770, divwu 643, mtspr 551.** (Pre-OPT_NATIVE2 builds (ps_cmp, lmw/stmw, divw, sraw later compiled)) [qemu_model; 2026-10-14] ic_w850/out.txt lines 33,51-67; ic_w950 lines 51-67 *The icbi count 622,595 is identical in every run from both the 850 and 950 states (ic, dcache, sch), so it is a one-off burst or a cumulative counter, not a per-field rate. It also appears as 6.23e5 icbi executions (3.8% of modelled cycles) in the 20-field model, which may inflate icbi's model share. Informs: Interpreter fallbacks are a small cost. Check icbi handling before attributing model cycles to it.*
- Instructions still interpreted at run time (qemu TP 850-875): **645,415 of 37.7M (1.7%): icbi 622,595; ps_cmpo0 4,892; rfi 3,367; sc 2,588; lmw 2,554; stmw 2,553; mftb 2,103; divw 2,008; sraw 1,524; divwu 532** [qemu_model] //wsl$/Debian/root/qtp1/log.txt; qtp5/log.txt *OPT_NATIVE2 later compiled ps_cmp, lmw/stmw, divw and sraw (+0.5-1.5% on the device). icbi is the state-load window only. Informs: Interpreter fallbacks are not a lever*
- Instructions still interpreted at run time by the JIT (dynamic): **Per 50 fields: icbi 622,595 (boot), ps_cmpo0 9,859, rfi 7,596, sc 6,054, lmw 4,968, stmw 4,968, mftb 4,209, divw 3,920, sraw 2,950, divwu 1,069, mtspr 894; 669,478 interpreted of 73.96 M (0.9%, mostly boot icbi)** (qemu, tc7/tc15/nohle, 850-900) [qemu_model; 2026-10-12 to 2026-10-14] aot/tc7.txt, nohle_on.txt *Gameplay-only interpretation is ~47k per 50 fields after removing boot icbi. Informs: which ops a redesign must compile natively (ps_cmpo0, lmw/stmw, divw, sraw)*
- JIT churn from boot over 1,100 fields (Oct 11 build): **4,436,366 blocks compiled, 359 flushes, 24,258,321 guest instructions compiled vs 1,135,758,834 run (1:47); kept hot 7.1, exit stubs 2.3, cold 18.6 words per guest; 0.1% interpreted** (qemu-arm JIT, HLE_CHECK build, boot to 1100) [qemu_model; 2026-10-11/12] hlejit.txt, hlejit2.txt *Superseded by cold-path shrink (coldstat series) and by device runs showing 0 flushes; qemu build buffer size may differ from device. Informs: cold-code size drove flush/recompile churn in this build*
- JIT churn in the dcache2 / sch configuration: **dcache2: 371,100 blocks compiled, 32 flushes, 4,305 live, 189,063 indirect misses (185,428 to blocks compiled then), 318,206 link requests in 60 fields, 8 MB buffer. sch7: 132,895 compiled, 13 flushes in 20 fields. ic runs: ~26k compiled, 0 flushes (16 MB, traces off).** (Traces on + 8 MB buffer vs traces off + 16 MB) [qemu_model; 2026-10-13 to 10-15] dcache2/out.txt lines 29-36; sch7/log.txt lines 23-29; ic_w850/out.txt lines 26-29 *TIMING_MODEL_PLAN calls the 8 MB refills a PC-build pattern, not the 2DS's. Device job 154 recorded a buffer overflow with traces on. Informs: Code-buffer size versus hot footprint (~1.9 MB touched) decides whether a design churns. A redesign that grows code needs buffer headroom, and malloc headroom on the 2DS is limited (17-20 MB largest free block).*
- JIT churn in the qemu relayout run vs a plain run: **Relayout run (20 fields): 130,645 blocks compiled, 10 flushes, 812,555 dispatches, 106,331 links, indirect misses 63,183 (61,449 to blocks compiled since the flush). qtp5 (25 fields, no relayout): 30,940 compiled, 1 flush, 693,921 dispatches, 23,286 links, 18,328 misses** (TP from tp_arm_850; the relayout run has fault mode and traces on, qtp5 has both off) [qemu_model] //wsl$/Debian/root/relayout/out.txt; //wsl$/Debian/root/qtp5/log.txt *Different builds, options and lengths, so the comparison is only indicative. Each relayout flushes and recompiles everything (about 4x the compiles). Informs: Cost of flush-based relayout*
- JIT compile statistics in the ic runs (traces off, 16 MB buffer): **850 run: 26,020 blocks compiled (22,976 of them in the first 2 fields per ic_smoke), 20,600 live, 4,020 KB code; 7.9 host words per guest instruction compiled; kept blocks 4.7 hot + 3.2 exit-stub + 4.1 cold words per guest instruction; 97.9% of compiled instructions native; 0 flushes. 950 run: 25,540 compiled, 20,124 live, 3,906 KB, 4.6 hot words.** (PC qemu build, traces off, fault mode off) [qemu_model; 2026-10-14] ic_w850/out.txt lines 26-33; ic_w950/out.txt lines 26-33; ic_smoke/out.txt *'instructions run' (44.8 M / 45.5 M) is the JIT's own counter and is not comparable with the model's 4.62e7 per 20 fields Informs: Static hot words per guest instruction (~4.6-4.7) is the code-size metric the 2DS rewards*
- JIT traces, qemu code size: **150 TP fields: hot words/guest instr 6.0 -> 4.9, total code 4,598 -> 4,090 KB, cached GPR reads +58%; from tp_arm_850 100 fields 'on vs off': code 7,217 -> 5,160 KB, kept hot words 7.1 -> 6.0** [qemu_model; 2026-10-12] docs/HANDOFF.md L510-520 *direction of the second pair is ambiguous in the text; device later showed traces double working set*
- Kept-block code size per guest instruction (relayout run): **3.9 hot, 3.3 exit-stub and 9.6 cold host words per guest instruction; 2.1% interpreted (642,182 of 30,365,361)** (traces on, fault mode on, TP 850-870) [qemu_model] //wsl$/Debian/root/relayout/out.txt *Informs: I-cache footprint baseline*
- Loop-region model: **40 fields: 7,547 innermost loops touch <= 5 ids; their traffic is 0.420 per block run against 0.146 to enter and leave, saving 0.274 (0.50% of hot words). 14,456 loops touch more (traffic 2.350 per run). sch7: 1,388 loops save 0.040 (0.06%); 4,477 touch more (1.709 per run).** (As above) [qemu_model; 2026-10-13] stats2.txt line 46; sch7/log.txt line 150 *Informs: Negative result: loop-region register caching is worth <= 0.5%*
- Memory-check hoisting model (D-form accesses): **40 fields: integer 17,347,296: base first used 52.0%, checked before 44.8%, + across update forms 3.2%; float 5,725,244: 31.1 / 67.6 / 1.3%; absolute (rA = 0) 1,684. sch runs: integer 12,028,062: 47.9 / 49.2 / 2.8%; float 4,874,201: 25.5 / 73.2 / 1.3%; absolute 835.** (Per-block check hoisting (OPT_JIT_PLAN stage 1)) [qemu_model; 2026-10-13] stats2.txt line 47; sch7/log.txt line 151 *Fault mode (which removes all checks) gave core 0 ~11% on the device (job 88) but crashes the kernel occasionally Informs: Check hoisting can drop ~45-52% of integer and ~68-75% of float address checks*
- Modelled core-0 cost per guest instruction (arm11_sched over JIT_SCHEDDUMP): **8.0 host instructions + 2.3 interlock cycles per guest instruction (before fetch stalls); memory instructions ~60% of modelled cycles** (qemu dump, TP cutscene, 40 fields, 5.9e7 guest instructions) [qemu_model; 2026-10-13] docs/OPT_JIT_PLAN.md:18-22 *Model excludes instruction fetch, which is ~30-44% of device cycles. C11's model reads 6.29 + 2.48 for a later build. Informs: Ordering JIT stages by modelled cycles*
- Modelled cycles per execution of memory instructions: **lwz 10.5, stw 9.6, lfs 22.6, stfs 27.8, psq_l 35.8, psq_st 49.3** (qemu model, TP cutscene) [qemu_model; 2026-10-13] docs/OPT_JIT_PLAN.md:21-22 *C11 model for a later build: lfs 18.7, stfs 22.4, psq_st 39.0 (fcache off). Informs: Float load/store stubs (C16, C8, C18)*
- Modelled share of core-0 cycles by guest opcode: **lwz 13.6%, psq_l 8.0%, stw 7.4%, psq_st 6.1%, lfs 5.5%, bc 5.5%, stfs 4.4%, addi 3.3%, icbi 2.5%, bclr 2.4%, lhz 2.4%, rlwinm 2.3%** (qemu model, TP cutscene, 40 fields) [qemu_model; 2026-10-13] docs/OPT_JIT_PLAN.md:20-21 *Informs: Which instructions to optimize*
- One-word exits (block-entry budget check), qemu: **branches were 34% of static hot words (bc 11.8, b 10.1, bclr 7.5); hot words 681,522 -> 589,393 (-13.5%); bc 11.8 -> 5.8, b 10.1 -> 7.1; stub area 2.0 -> 3.4 words/guest instr; identical 44,798,955 instructions** [qemu_model; 2026-10-12, dc09f3e] docs/HANDOFF.md L712-725
- OPT_FCACHE (float lanes) modelled cost: **6.29 + 2.48 -> 6.17 + 2.30 host instructions + stall cycles per guest instruction (-3.4%); lfs 18.7->12.9, stfs 22.4->15.3, psq_st 39.0->30.1, fadds 22.0->12.7, ps_mul 31.4->18.4 cycles** (arm11_sched over TP cutscene, 20 fields) [qemu_model; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:145-147 *Model omits instruction fetch; device result was net zero. Informs: Shows model gains do not transfer when footprint grows*
- OPT_LATCH model: hot words saved: **0.70 hot words per block run (2.5%)** (TP fields 850-880, JIT_REGSTAT on qemu ARM build) [qemu_model] core/src/jit/jit.h:118-121 *Words-saved model, not cycles. The device result was +0.8 to +2.4%. Informs: Prediction for OPT_LATCH*
- OPT_RETSTUB: **qemu: bclr 6.8 -> 3.8 hot words, all executed hot words 4.80 -> 4.70 per guest instruction (-2.1%); device job 148 within noise; ~1.8% of static hot words removed and +1-1.6% in the stack. CR-field fate: 5.3% dead with a fused bc, 42.2% dead only past a load/store or call** [qemu_model; 2026-10-14] git 2a5cfd8 body; cac9c7c subject; ede002f diff *Informs: Static hot footprint is the lever*
- Origin of whole-code-cache icbi drops: **2,451 'dropped all code' icbi flushes: all from the boot apploader region loop (lr 812fff80, pc 812fffa8/812fffac, pa 012ff000-012fffff: 2,432; pa 01200000: 19)** (qemu, icbi trace run from tp_arm_850.state (boot apploader replayed before state load), 850-860) [qemu_model; 2026-10-14] aot/icbi.txt (2,451 icbi-flush lines + JIT summary) *Only this boot path traced. Informs: whole-cache invalidation is a boot cost, not a gameplay cost*
- Per-opcode hot + cold ARM words per compiled instruction (Oct 14 build): **lwz 3.8+16.3; stw 3.5+15.6; lfs 5.8+23.5; stfs 4.3+20.6; lfd 5.1+40.0; stfd 4.3+25.2; psq_l 6.8+54.5; psq_st 5.2+94.9; fcmpo 14.4; fctiwz 16.8; bcctr 11.1; bclr 8.2; b 6.2; cmpli 5.8; addi 1.5; bc 1.3** (qemu, tc7 (traces on, fault mode on), 50 fields) [qemu_model; 2026-10-14] aot/tc7.txt compiled code by opcode *The task's quoted 'psq_l 6.0 hot + 18.3 cold, psq_st 5.4 + 23.4' does not appear in any file read here; nearest are tc7 (6.8+54.5 / 5.2+94.9), ic (5.8+21.3 / 5.2+82.3) and aa (5.4+15.9). Informs: where code size lives: float loads/stores and psq carry 20-95 cold words each*
- PSMTXConcat cost in AOT: **15 ARM instructions per guest instruction (bound by double multiply-adds, register pressure)** (qemu/PC) [qemu_model; 2026-10-14] docs/research/AOT_PLAN.md:125-126 *Informs: Matrix code is float-precision bound (C8)*
- psq_l / psq_st by GQR type: **psq_l: type 0 (float) W=0 930,697 / W=1 82,826, type 7 W=1 19,194 -> 98.1% float; psq_st: type 0 462,954 / 102,395, type 4 288, type 7 105 -> 99.93% float** (qemu, 30 fields) [qemu_model; 2026-10-12] regstat_base/out.txt, regstat_psq/out.txt *One scene. Informs: specialise quantized load/store for GQR type 0 and keep a slow path*
- qemu host instruction totals, JIT vs AOT run, and share inside the JIT buffer: **JIT run: 244,056,375 in buffer, 13,052,504,309 other (buffer = 1.8%); AOT run: 48,410,267 in JIT buffer + 619,917,517 in AOT text + 7,500,601,546 other** (qemu rangecount, TP 850-870 (20 fields); JIT run compiled 126,235 blocks with 10 flushes, AOT run 13,418 blocks, 0 flushes) [qemu_model; 2026-10-14] aot/rc/jit.plugin, aot/rc/aot.plugin, aot/rc/ranges.txt, aot/cs/*.out *Process-wide (all threads, including GX); the 5.5 G difference coincides with ~112k more compiles; attribution inferred, not measured. Informs: qemu whole-process counts are dominated by non-buffer code (compile, flush, GX, software paths), so they cannot stand in for device core-0 time*
- qemu TP 850-875: opt 1 (qtp1) vs opt 5 (float lanes, qtp5): **Both: 37,708,598 guest instructions, 875 fields, GX 25,719,008 bytes, same final PC. opt1: 20,715 blocks, 5,579 KB code, 7.0 host words per guest instruction compiled, kept 4.1 hot / 2.8 stubs / 3.5 cold, 98.6% native. opt5: 30,940 blocks, 3,877 KB, 3.1 words, kept 4.2 / 3.1 / 4.1, 99.0% native, 1 flush** [qemu_model; 2026-10-13] //wsl$/Debian/root/qtp1/log.txt; //wsl$/Debian/root/qtp5/log.txt; git 18d6248 / 4839521 bodies *That qtp1 = opt 1 and qtp5 = opt 5 is inferred: the commit bodies' 'opt 1 vs 5, same 37,708,598 instructions' and the dates match. qemu wall time (462 vs 465 s) means nothing for the device. On the device float lanes measured +3.5% / -3.5% by scene (separate entry). Informs: Float-lane JIT (C11)*
- Region model: register hand-over at constant exits: **Saves 1.326 memory accesses per block run = 2.52% of hot words (base build), 2.77% (model1), 2.93% (psq build); variant 1 2.02-2.35%; variant 2 2.21-2.58%; loop regions (<=5 ids) save 0.307-0.314 per run = 0.59-0.69%** (qemu, 30 fields, model over JIT_REGSTAT traces) [qemu_model; 2026-10-12] regstat_base/out.txt, regstat_model2/out.txt, regstat_psq/out.txt *Model counts accesses, not cycles; moves not counted. Informs: registers-across-blocks is worth ~2-3% of hot words, consistent with the measured 1.3% dead end*
- Region options modelled gain: **entry-state handover at linked exits: 2.2-2.4% of executed hot words gross, ~1.3% net; loop regions 0.64%** [qemu_model; 2026-10-12] docs/HANDOFF.md L692-696 *Informs: registers across blocks / region JIT not worth building (rejected)*
- Region-compilation models: **Handing a block's entry state over at linked exits saves 2.2-2.4% of executed hot words (~1.3% after register moves); loop regions 0.64%** [qemu_model; 2026-10-12] git c639aeb body *Informs: Registers across blocks ruled out (~1.3%)*
- Region-compilation register traffic model (JIT_REGSTAT, c639aeb): **TP 30 fields, 7.6M block runs, 47.9 hot words/run; per run loads 2.04 first touch + 0.28 after evictions; write-backs 1.54 constant exits + 0.45 blr/bctr + 0.49 evictions; ~4.8 accesses/run = ~10% of executed hot words; exits 79% constant / 21% blr-bctr; back edges 36% of constant exits; top 64 blocks 35%, top 256 61%** [qemu_model; 2026-10-12] docs/HANDOFF.md L684-691
- Register pinning (rejected): **4 dynamic slots instead of 5 cost +0.47 loads/stores per block run; pinning CR would save 0.77; net ~1.5%** [qemu_model; 2026-10-12] git 3a1696f body *Informs: Rejected*
- Register-file memory traffic per block run (region model, 40 fields): **Loads: first touch 2.112, after a fallback 0.002, after an eviction 0.289. Write-backs: constant exits 1.583, blr/bctr 0.471, evictions 0.513, fallbacks 0.003. About 5.0 register-file accesses per block run against 54.8 hot words per run (~9%). 9,737,652 block executions.** (TP 850->890 (40 fields), sched2 build) [qemu_model; 2026-10-13 05:42] //wsl$/Debian/root/stats2.txt lines 25-42 *Counts guest register loads/stores only, not stub or memory work Informs: Upper bound for what register allocation alone can remove from executed words (~9%)*
- Remaining JIT levers as priced 10-15: **Budget checks only at loop heads ~7% of executed hot words; OPT_LATCH modelled 0.70 hot words per block run (2.5%), 67% of blocks without the check; blocks with a backward exit 30.3% of block runs; dead write-backs at exits 0.46 (strict) / 0.66 (ABI) of 1.56 per run; 'grounded JIT items left ~1% each'** [qemu_model; 2026-10-15] git e05e7a4 body; 136545a diff *Informs: JIT is near its ceiling without exactness trade-offs*
- Remaining static-footprint shares: **register write-backs at block exits ~14% of hot words; shared stubs 31 KB, 4% of misses** [qemu_model; 2026-10-14] docs/HANDOFF.md L1010-1013
- Savings from handing register state across block exits (region model): **40 fields (stats2): today 1.608 write-backs + 1.457 target loads; handover saves 1.375 accesses per run = 2.51% of hot words. Variant 1: 2.01%; variant 2 (read-first only): 2.20%. sch7 (traces on): 0.57% / -0.68% / 0.48%.** (Moves not counted) [qemu_model; 2026-10-13] stats2.txt lines 44-45; sch7/log.txt lines 148-149 (sch3/schb7 identical) *Consistent with the earlier ~1.3% (REGION_JIT_PLAN / OPT_JIT_PLAN line 26) Informs: Negative result: registers across blocks are worth ~0.5-2.5% of hot words, which bears directly on the planned 'registers and FPRs across blocks' rebuild*
- Scheduled code (SCHED_STUBS): model vs device: **Model: interlock cycles 1.70e8 -> 1.39e8 (-18%), executed instructions -4%, lwz 3.73 -> 0.78 stall cycles, hot words 4.7 -> 4.6 per guest instruction. Device (relay 91-94): cutscene 19.89 off / 19.72 on, title 18.86 / 19.94 (+-0.5), within noise; the data-stall counter fell only 2-3%.** (40 fields of the cutscene) [qemu_model; 2026-10-13] HANDOFF.md lines 853-860; post-sched dump re-scored at 1.39e8 (/root/sched.bin) *Kept on as default with no device gain Informs: Negative result: interlock scheduling alone does not move the device*
- Shared float stubs (b7534e3), qemu: **psq_l/psq_st with plain-float GQR are 98% / 99.9% of executions; psq_l 24 -> 5.6 hot words, psq_st 31 -> 5.2** [qemu_model; 2026-10-12] docs/HANDOFF.md L744-748
- Silent refused compiles: **1,090 refusals in 150 fields when pipe psq_st exceeded 256 fix-up entries (seen only as compiled instructions doubling)** [qemu_model; 2026-10-11] docs/HANDOFF.md L182-186 *Informs: check 'compiles refused' after any change adding branches/exits*
- Simulated 8-lane FPR cache per block: **ld_fpr 9,484,118 executed, 53.5% hits; st_fpr 9,752,358, 10.3% overwritten before read (base); model builds 7,070,202 / 49.8% and 9,185,595 / 13.5%** (qemu model, 30 fields) [qemu_model; 2026-10-12] regstat_base/out.txt, regstat_model1/out.txt *Model only. Informs: block-local FPR caching removes about half the FPR loads*
- Simulated FPR lane cache hit rates (8 lanes per block): **40 fields: ld_fpr 12,753,406 executed, 53.5% hits; st_fpr 13,145,607, 10.4% overwritten in-block before read. sch3: 8.39 M, 62.6% / 10.70 M, 18.3%. sch7: 6.95 M, 49.4% / 5.00 M, 11.5%. schb7: 6.12 M, 46.1% / 1.57 M, 1.1%.** (Per-block lanes) [qemu_model; 2026-10-13] stats2.txt line 23; sch7/sch3/schb7 log.txt line 127 *Within-block only: TP's blocks are short Informs: About half of FPR loads could hit a block-local lane; device showed no net gain*
- Slow-path memory accesses: fault mode on vs off: **Fault mode on (relayout run, 20 fields): slow loads 5,772, stores 6,768; 1,330 patched sites, 6,211 faults. Off (qtp1/qtp5, 25 fields): slow loads 11,864, stores 8,478** (qemu-arm, TP 850-870/875) [qemu_model] //wsl$/Debian/root/relayout/out.txt; qtp1/log.txt; qtp5/log.txt *Different run lengths. Fault mode is off on the 2DS because of kernel crashes (separate entries). Informs: Fault-mode memory access*
- Static compiled hot words by guest opcode (ic_w950): **lwz 10.7% of hot words (4.2 hot + 1.0 cold each); bc 9.9% (4.8); stw 8.1% (4.4+1.7); b 8.1% (5.6); lfs 5.9% (5.9+0.5); addi 5.8% (1.5); rlwinm 4.0% (4.8); cmpli 3.9% (6.7); icbi 2.7% (6.0); bclr 2.4% (3.6); fcmpo 2.1% (15.0); fmuls 2.1% (10.1); psq_l 1.1% (6.0 + 18.3 cold); psq_st 1.0% (5.3 + 23.7 cold); fctiwz 0.6% (12.9)** (950 build with OPT_RETSTUB) [qemu_model; 2026-10-14] ic_w950/out.txt lines 68-98 (ic_w850 lines 68-98 similar; bclr there 4.26%, 6.6 words) *Static (compiled) words, not executed Informs: Where code volume sits: loads/stores and branches are ~45% of hot words; float compares/multiplies are the most expensive per instruction*
- Static hot words per guest instruction with traces on (sch runs) vs off (ic runs): **Traces on (sch7/sch3/schb7): kept 7.0 / 6.9 / 6.9 hot + 2.9 / 2.7 / 3.0 exit-stub + 3.7 / 3.4 / 4.0 cold words per guest instruction, 13 flushes and ~133k blocks compiled in 20 fields. Traces off (ic runs): 4.7 / 4.6 hot, 0 flushes.** (Different builds (2026-10-13 vs 10-14)) [qemu_model; 2026-10-13 / 10-14] //wsl$/Debian/root/sch7/log.txt lines 23-30; sch3/schb7 log.txt same lines; ic_w850/ic_w950 out.txt line 31 *The 'kept blocks' accounting under traces may differ, and the builds differ Informs: Agrees with device job 154: traces lose (more distinct hot code)*
- TP float loads/stores share of executed hot code; words per site: **~19% of executed hot code; psq_l 24 words, psq_st 31, stfs 10 per site** (qemu ARM build, JIT_REGSTAT dynamic opcode profile) [qemu_model] core/src/jit/jit.c:4118-4119 *Code-size share, not cycle share. Informs: Shared float stubs*
- Traces following calls (OPT_CALLS) vs traces without calls: code growth: **Without calls (tc7) vs with (tc15): blocks compiled 327,373 vs 251,826; flushes 35 vs 101; guest instructions compiled 3,946,940 vs 8,598,044 (2.18x); kept hot/exit-stub/cold words per guest 4.1/4.5/12.1 vs 4.0/5.1/15.5; blocks live at end 7,186 vs 710; fault-mode faults 17,096 vs 43,786; same 73.96 M instructions run** (qemu, tp_arm_850.state to 900 (50 fields), fault mode on) [qemu_model; 2026-10-14] aot/tc7.txt, aot/tc15.txt *qemu stats; device verdict is from AOT_PLAN.md, not these files. Informs: inlining calls into traces bloats code and triples flushes; device job 147 found it within noise (AOT_PLAN) and it is off*
- Traces on vs off (runs named nohle_on/off, HLE off): **Same 73.96 M instructions; blocks compiled 351,523 vs 440,969; flushes 60 vs 35; code 5,212 vs 6,550 KB; kept hot 6.0 vs 7.1, exit stubs 2.9 vs 2.4, cold 18.2 vs 18.6 words per guest; bc hot words 1.3 vs 11.3; b 8.1 vs 10.1** (qemu, tp_arm_850.state to field 900) [qemu_model; 2026-10-12] //wsl$/Debian/root/nohle_on.txt, nohle_off.txt *These runs cover 50 fields (850-900), not 900 fields as the task text says; the on/off is traces ('traces on/off' lines), HLE is off in both. Informs: traces cut hot code ~15% but raised flushes*
- Traces on vs off over 100 fields: **150,419,831 vs 150,419,842 instructions; blocks compiled 695,579 vs 878,483; flushes 120 vs 71; code 5,160 vs 7,217 KB; kept hot 6.0 vs 7.1 words per guest** (qemu, tp_arm_850.state to 950) [qemu_model; 2026-10-12] fromstate_on.txt, fromstate_off.txt *Instruction counts differ by 11 (timing of slice ends). Informs: same as above; confirms HANDOFF numbers*
- JIT code density (lesson11 test DOL): **12.26-12.38 host words per guest instruction overall; all blocks kept: hot 4.1, exit stubs 3.0, cold 8.2; 4% of buffer; 1,504 blocks** (graphics_gx_neheGX_lesson11.dol in Azahar) [azahar; 2026-10-15] azahar-sdmc/perf.csv lines ~1902, 1957-1961 *Code size is deterministic, but this is a test DOL, not TP Informs: Code size budget vs I-cache footprint*
- ARM11 branch costs (TRM Table 15-11): **folded predicted 0; not folded 1; static taken 4; mispredict 5-7; BX lr via return stack 4 (mispredicted 7, empty 5); register-indirect BX/BLX never predicted, 5-7** (ARM DDI 0360F) [estimate; 2026-10-10] docs/ARCHITECTURE.md:262-264 *Manual figures, not measured on device. Informs: Dispatcher and return-stub design (retstub, hlelink)*
- ARM11 interlock model: share of modelled interlock cycles: **loads 63% of TP's modelled interlock cycles; lwz alone 17.5%** (arm11_sched.py over JIT_SCHEDDUMP) [estimate] core/src/jit/jit.c:4085-4086 *Datasheet-latency model (branches fall through, no caches). The device A/B of the resulting SCHED_STUBS was within noise. Informs: SCHED_STUBS memory stubs*
- ARMv6K ISA constraints for codegen: **no Thumb-2, no MOVW/MOVT (32-bit constants need a literal pool or MOV+ORR); no NEON; unaligned LDR +1 cycle; REV/SXTH/PKH 1/1; SETEND 1; SSAT/USAT 1/2; SIMD-in-register 1/1-3; LDREX/STREX supported** (MPCore TRM tables 15-7, 15-12) [estimate; 2026-10-10] docs/ARCHITECTURE.md:295-310 *Manual timings. Informs: Byte-swap strategy (REV vs SETEND BE), constant materialization*
- Branch predictor capacity: **128-entry BTAC, 3-entry return stack** (MPCore TRM) [estimate; 2026-10-10] docs/ARCHITECTURE.md:260 *Manual figure. Small BTAC means hot-loop branch density matters. Informs: Block-linking and trace layout*
- CR dead-store elimination ceiling: **Exact ceiling ~0.3% of hot words** (Static count over hot words) [estimate; 2026-10-14] memory/gc3ds-codesize-candidates.md:15; memory/gc3ds-jit-dead-ends.md:22 *Informs: Not worth building*
- Dead GPR/CR write-backs across blocks: **Modelled ~1.6% (not built)** (Model) [estimate; 2026-10-15] memory/gc3ds-timing-drift-approved.md:22 *Informs: Small remaining exact-ish lever*
- Derived: per-guest-instruction ARM cycle cost by class: **int 2.2, float 7.1, paired 11.8 ARM cycles per guest instruction (804 MHz / JIT sw MIPS); native C int 1.2** (derived from gcbench 3) [estimate; 2026-10-16] bench/results/latest.txt L29, L32, L35, L46 *Synthetic kernels. TP's real mix (from profiles) decides the weighting Informs: Ranking JIT work: float and paired paths are 3-5x more expensive per instruction than integer*
- Gekko FP timing (guest reference, 750CL tables): **FMA-class fully pipelined, 3-cycle (1-1-1); fdivs/ps_div ~17 cycles, fdiv ~31; peak 1.9 GFLOPS; 486 MHz; 1125 DMIPS** (IBM 750CL manual) [estimate; 2026-10-10] docs/ARCHITECTURE.md:19-46 *The PDF extraction shifted the cycles column, so per-mnemonic assignment needs a re-check against the PDF. Informs: Guest-vs-host throughput ratio; cycle accounting for timing*
- Indirect-branch table FAST_BITS 16 size and upper-bound cost: **65,536 slots x 8 B = 512 KB per key, 4 keys; ~1 M returns/s at ~28 cycles L2 would be ~3.5% of core 0** [estimate; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:211-213 *~1 M returns/s here vs ~8,500 dispatcher returns/s in job 184 (different events: table lookups vs dispatcher exits).*
- Per-site code size of shared stubs: **inline memory fast path ~9 words -> stub call; slow-path stub call 5 words vs ~25; interpreter stub 4 words vs ~20; SCHED_CONST 2 words vs up to 5** (static code-size counts) [estimate] core/src/jit/jit.c:1587-1589, 1787, 4050-4052; core/src/jit/jit.h:94-96 *These are counts of emitted words, not timings. Informs: Code-size reduction strategy*
- psq_st / stfs modelled cost: **psq_st ~49 modelled cycles, stfs ~28** (OPT_JIT_PLAN model) [estimate] core/src/jit/jit.c:4354-4356 *The model's prediction did not show up on the device (kstore +1%, within noise). Informs: OPT_KSTORE*
- Registers kept across blocks / region compilation: **~1.3% of hot words; loop regions 0.6%; pinning a guest register net ~1.5% at best** (Region model over hot words, not built) [estimate; 2026-10-12 (region model)] memory/gc3ds-jit-dead-ends.md:14-16 *Modelled only; never device-tested Informs: Register allocation is not the lever; memory instruction cost is*
- Share of executed hot words in paired-single and quantized load/store: **psq_l+psq_st+ps_madds0/1+ps_muls0+ps_madd+ps_mul = ~6.1% of executed instructions but ~24.5% of executed hot words** (derived from dynop.txt) [estimate; 2026-10-12] dynop.txt *Derived sum; build of 2026-10-12. Informs: FPRs-through-memory paired-single design is a major code-size cost; register-resident FPRs / NEON-less PS design*
- VFP RunFast / subnormal handling: **subnormal inputs trap to support code unless flush-to-zero; RunFast keeps everything in hardware** (VFP11 TRM 1.5.4) [estimate; 2026-10-10] docs/ARCHITECTURE.md:290-293 *A correctness risk for NaN-boxing and bit-exact physics. Not measured. Informs: FPSCR mode in the JIT*
- VFP11 spec throughput (TRM): **single-cycle throughput for single precision; double multiply/MAC 2-cycle throughput; FDIV/FSQRT single 19 latency/15 throughput, double 33/29** (ARM DDI 0274G) [estimate; 2026-10-10] docs/ARCHITECTURE.md:283-287 *Manual. NATIVE_PORT_PLAN risk: 'VFP11 doubles are slow' for native game code. Informs: Single-precision JIT (C8) and double avoidance*
- AOT interpreter fallbacks by cause: **cache-maintenance loops (DCInvalidateRange, DCFlushRange) were 96% of all fallbacks** (AOT_FBHIST run (PC build)) [unknown] core/tools/aot/ppc2c.py:668-671 *The run and platform are not stated. Informs: dcbst/dcbf/dcbi/dcbz translated natively*
- fres/frsqrte accuracy in gc3ds: **computed exactly in both interpreter and JIT; hardware returns table estimates (resolution < 1/4000); TP and others misbehave with exact results (Dolphin #7304)** (code state at the time) [unknown; 2026-10-11] docs/research/GCWII.md:5-9, :63-69 *An accuracy bug, not a speed figure. Tables: owner decision on licence or source. Not verified against current code. Informs: FP instruction implementation in a redesigned JIT*
- Hot vs cold code size: **hot ~35% of TP's code; 5.0 hot vs 9.3 cold words per guest instruction** (after block stubs and float stubs moved out) [unknown] core/src/jit/jit.c:5264 *Source not stated; probably JIT stats from a PC/qemu run. Informs: coldStart split at 3/8 of the buffer*
- JIT code buffer size: recompilation at 4 MB: **~1 million blocks recompiled in 12 emulated seconds (35% of the time) at 4 MB** (TP title scene; a full cache is dropped whole) [unknown] core/host3ds/main.c:1760-1761 *The device is not named in the comment. Traces later overflowed even 16 MB (main.c:386). Informs: 16 MB code buffer*

### Instruction cache and fetch (61)

- A/B hot-block relayout (job 195, relayout 0 vs 1): **Cutscene 21.22 vs 21.50 (neutral). Later 300-585 s 12.50 -> 11.77 (-5.8%) with I-cache misses 4.30 -> 6.05 M/s (+40%) and stall_inst 110 -> 144 M/s. Each relayout: 245-274 hot blocks compiled first in ~56 ms every 24 s.** (tp850, 600 s) [device; 2026-10-15, job 195] relay_195.csv perf rows split on column 'relayout'; '# relayout on at real N s' lines *HANDOFF reports -4.5% (12.43 -> 11.87) over a different window. Job 188 (first relayout attempt) stopped the 2DS at ~20 s with no log, and that stop did not recur in 195. Informs: Rejected: laying hot blocks out first raises I-cache misses (as traces did, job 154)*
- Block exit cost: linked jump vs C dispatcher: **64 blocks: linked 13.1 ns vs dispatch 36.9 ns (2.8x). 2048 blocks: linked 61.5 vs dispatch 66.1 ns (gcbench 4: 61.6 / 65.8)** (gcbench 3/4, block-chain kernel) [device; 2026-10-10] bench/results/latest.txt L49-53; gcbench4_2026-10-11.txt L86-94 *Once code exceeds the I-cache, linking stops helping (61.5 vs 66.1) Informs: Block linking matters only while hot code fits the 16 KB L1I. Code footprint beats exit mechanism*
- Block stubs moved to cold region (3605dfd, relay 60-61): **title 19.26%, cutscene 19.27% (from 18.21 / ~18.0); I-cache 8.46 -> 7.58 M/s; fetch stalls 286 -> 238 M cycles/s; hot region 5.6 words/guest instr; session total title 16.6 -> 19.3%, cutscene 15.9 -> 19.3%** [device; 2026-10-12] docs/HANDOFF.md L730-735 *Informs: hot-region density is the lever*
- Block stubs moved to the cold region (3605dfd): **Cutscene 19.53%, title 19.43% (from about 17.7/18.2). I-cache misses 7.87/7.65 M/s. Fetch stalls 245/240 M cycles/s.** (Separate runs) [device; relay jobs 60-61, 2026-10-12] core/out/relay_60.csv, relay_61.csv *Informs: Keeping the hot region dense pays: about +7%*
- Block transition cost vs code footprint: **4 KB (64 blocks): linked 13 ns, via C dispatcher 37 ns; 128 KB (2048 blocks): linked 61 ns, dispatcher 65 ns; ~50 cycles once JIT code overflows I-cache** (2DS gcbench run 2) [device; run 2] docs/FINDINGS.md:52-58 *Informs: Linking stops helping beyond I-cache; footprint matters*
- Branch mispredicts vs instruction-fetch stalls, core 0: **~0.7M mispredicted branches/s vs 770M cycles/s (<1%); ~11M I-cache misses/s, ~310M stall cycles/s (~40% of core 0)** [device; 2026-10-11] docs/HANDOFF.md L166-168 *Informs: branches are not the problem; instruction fetch is*
- Budget checked at exits + SO-free CR: **+2.3-2.4% on the 2DS with fewer I-cache misses; qemu hot words 7.1 -> 6.2 per guest instruction, exit stubs 2.4 -> 2.0, cmpli 10.6 -> 6.5; compiled code holds 88-96% of core 0 cycles and nearly all I-cache misses and fetch stalls (--pmu-split)** [device; 2026-10-12] git 4c5012a body, 1e4ef05 subject *Informs: Where core 0's cycles go*
- Core 0 cycles stalled waiting for instructions, TP title scene: **44% of core 0 cycles; about 1 I-cache miss per 2 guest instructions** (TP title scene, JIT with inline memory fast paths (~9 words per site)) [device; commit 862fe63] core/src/jit/jit.c:4052-4053 *Superseded: job 184 measured 21% I-fetch stalls (jit.h:188) after the shared stubs, cold-code split and other code-size work. Both numbers are kept here. Informs: Shared memory-access stubs: code size per guest instruction is the lever*
- Core 0 I-cache misses and instruction-fetch stall in TP: **Cutscene: 5.6-6.3 M misses/s, stall_inst 171-194 M cycles/s (~21-24% of core 0). Later stretch (300-570 s): 4.3-4.6 M/s, 110-120 M cycles/s (~14%).** (tp850, PMU on) [device; 2026-10-15, jobs 171/184/195] relay_184.csv, relay_171.csv, relay_195.csv columns icmiss_k, stall_inst_m *Informs: I-cache footprint of JIT code is the largest single measured stall (C codesize work, block layout)*
- Core 0 instruction fetch stall share in TP: **21%; 5.8 M I-cache misses/s at ~29 cycles each (from L2)** (TP tp850, PMU, frame_budget.py) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:185 *Supersedes earlier ~40% I-cache stall share (FINDINGS, MEASURE_PLAN:82). Informs: JIT code size / layout redesign value*
- Core 0 instruction-fetch stalls (first PMU reading): **Title ~14.8%: core 0 stalled on instruction fetch 42% of cycles, 11.8M I-cache misses/s; hot-block samples flat (top block 0.2-0.3%); CR logic ops ~40k/s interpreted** [device; 2026-10-11] git 82a091b body *Informs: Cut code footprint per guest instruction, not one routine*
- Core 0 IPC (PMU): **0.31; core 0 is instruction-fetch bound** (TP tp850, JIT) [device; 2026-10-14, jobs 142-147] memory/gc3ds-aot-verdict.md:14-15 *Informs: qemu instruction counts do not predict 2DS speed; I-cache footprint is the lever*
- Core 0 PMU after slow-path stubs: **I-cache misses 9.0 -> 8.8 M/s; fetch stalls 274 -> 277 M cycles/s (~36%)** [device; 2026-10-12] docs/HANDOFF.md L622-625 *Informs: fetch stalls are hot code, not slow paths*
- Core 0 PMU, cutscene (job 93): **802.1 Mcycles/s; 265.6 M instructions/s (IPC 0.331); instruction-fetch stalls 205.2 M cycles/s (25.6%); data-hazard stalls 164.7 M (20.5%); I-cache misses 6.72 M/s; D-cache read misses 1.06 M/s; write misses 0.33 M/s; main TLB misses 340 k/s; branches 46.0 M/s; mispredicts 457 k/s; LSU-full stalls 15.6 M** (2DS, tp850.state, 185 rows, rows with real_s>5 and no recompile) [device; 2026-10-13] relay_93a.csv PMU columns (identical to core/out/relay_93.csv) *Each counter pair is live one second in five; duplicate of an original already in core/out. Informs: core 0 is fetch- and interlock-bound, not memory-bound*
- Core 0 PMU, title/boot stretch (job 94) and earlier jobs: **job 94: 614.1 Mcycles/s (core 0 not saturated; vblank_wait 55-58% in title windows), 212.8 M inst/s (IPC 0.347), I-misses 5.23 M/s, fetch stalls 141.3 M; job 91: 793.1 Mcyc, IPC 0.314, I-misses 6.55 M/s; job 86/88: 792-794 Mcyc, IPC ~0.31, I-misses 6.0-6.1 M/s** (2DS relay jobs 86-94) [device; 2026-10-13] relay_86a/88a/91a/94a.csv PMU columns *Means over rows; jobs 86/88/91/92 include A/B recompile seconds. Informs: IPC ~0.31-0.35 is stable across builds and scenes*
- Cost of straight-line code by footprint (I-cache): **line1k: 1.3 cycles per guest instruction, 0 I-cache misses. line8k: 4.4 cycles, 0.165 misses per guest instruction. line64k: 4.5 cycles, 0.164 misses. line256k: 5.6 cycles, 0.165 misses. All at 1.19 host instructions per guest instruction. Derived: about 19-26 cycles per I-cache miss.** (hwprobe) [device; 2026-10-11] core/out/hw_2026-10-11.txt *The penalty per miss is my estimate: (cycles - 1.3) / misses Informs: Per-miss fetch penalty for code-size work. Code larger than 16 KB pays about 3-4 cycles per guest instruction.*
- Float stubs on 2DS (b7534e3, relay 62-63): **cutscene 19.62%, title 19.77% (from 19.27 / 19.26); I-cache 7.58 -> 6.79 M/s; fetch stalls 238 -> 205 M cycles/s; hot code 6.0 -> 5.1 words/guest instr** [device; 2026-10-12] docs/HANDOFF.md L753-754
- Hot-block relayout (job 195): **relayout every 24 s of 230-270 hot blocks (~50 ms each); cutscene neutral 21.22 / 21.63; later stretch -4.5% (12.43 -> 11.87) with I-cache misses +40% (4.31 -> 6.06 M/s); job 188 stopped at first relayout (not recurring)** [device; 2026-10-15] docs/HANDOFF.md L1160-1167 *Informs: rejected; laying hot blocks first raised misses (as traces did)*
- Hot-block relayout A/B (job 195): **Later stretch 12.43 -> 11.87 (-4.5%); I-cache misses 4.31 -> 6.06 M/s (+40%); cutscene 21.22 / 21.63 (neutral); relayout every 24 s, 230-270 hot blocks, ~50 ms each; job 188's stop did not recur** [device; 2026-10-15] git 0d26611; docs/HANDOFF.md ~l.1163 *Informs: Rejected*
- hw.txt straight-line code cost vs code footprint: **inside 16 KB I-cache 1.6 ns/guest instr (1.3 cycles); beyond it 5.5 ns with one I-cache miss per ~6 guest instr (~19 cycles each, L2-served); 256k instr (1.2 MB code) 7.0 ns** [device; 2026-10-11] docs/HANDOFF.md L139-141 *Informs: code footprint is the cost driver; ~3.4-4.4x penalty once out of L1*
- I-cache cliff and L2 refill: **linked-block cost 13 ns up to 16 KB of JIT code -> 53-62 ns from 32 KB; misses served by L2 at ~45 cycles** (2DS gcbench run 3 sweep) [device; run 3, 2026-10-10] docs/FINDINGS.md:154-157 *Informs: Hot-code footprint budget (~16 KB)*
- I-cache misses and fetch-stall cost per miss by section: **cutscene 6.40 M/s (24.7 per 1000 host instr), later 4.76 M/s (12.4), movie 9.07 M/s (25.0); fetch-stall cycles per I-miss ~30.6 / ~25.9 / ~18.6** (clean run) [device; job 157] core/out/relay_157.csv frame_budget.py *Cycles per miss derived by me (stall_inst / icmiss) Informs: Most I-misses are served from L2 (~20-30 cycles); hot-code size is the lever*
- I-cache stall share of core 0 in TP (FINDINGS): **~40%** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:82 *Superseded by job 184 PMU breakdown: 21% fetch stalls (code-size work landed in between: retstub/syscall/hlelink).*
- I-fetch stall share of core 0: **21%** (TP save state (tp850-era), default JIT options of that build) [device; job 184] core/src/jit/jit.h:184-188 *Later than the 44% figure (jit.c:4052). Shows the code-size work roughly halved the I-fetch share. The relayout A/B result that this motivated is not recorded in source (job 188 stopped at the first relayout, main.c:977). Informs: Hot-block relayout (--relayout / --ab relayout)*
- Instruction fetch beyond L2 (2 / 3 / 4 / 8 MB code): **3.9 / 5.2 / 5.6 / 6.0 cycles per instruction** (timingprobe icode) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:84 *Informs: JIT cache size vs L2 (2 MB)*
- Instruction fetch cost by code footprint (cycles per instruction): **<=12 KB 1.0; 16 KB 1.02-1.13; 20 KB 2.0; 24 KB 2.6; 32 KB-1 MB 3.39 (icmiss 0.125/instr, 2.39 stall cycles/instr); 2 MB 3.85-3.88; 3 MB 5.24-5.26; 4 MB 5.62-5.63; 8 MB 6.02-6.09** (straight-line code loop on core 0) [device; 2026-10-15, jobs 176/178] relay_176.csv and relay_178.csv '# timing icode' *Informs: JIT code size / hot-code footprint (L1 I-cache is 16 KB; ~19 cycles per 32-byte line from L2)*
- Instruction fetch cycles per instruction by code size (L1): **<=16 KB 1.0; 20 KB 2.0; 24 KB 2.6** (timingprobe icode) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:82 *Informs: JIT hot-code footprint target: under 16 KB*
- Instruction fetch from L2 (code 32 KB - 1 MB): **3.39 cycles per instruction (~19 cycles per 32-byte line); counters: 0.125 I-misses/instr, 2.39 cycles fetch stall/instr** (timingprobe icode) [device; job 176; counters job 178] docs/research/TIMING_MODEL_PLAN.md:83,96-97 *Job 184 measured ~29 cycles per I-miss in TP, higher than this sequential-code probe's ~19. Informs: Code layout / compaction value*
- L1 size and block-to-block cost vs JIT code footprint: **L1 I and D 16 KB each; block-to-block cost jumps from 13 ns to ~55 ns once JIT code exceeds 16 KB** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:42 *Informs: Keep hot JIT code compact; code size, not instruction count, is the lever*
- Linked block cost vs JIT code size (I-cache cliff): **2 KB 13.2, 4 KB 13.1, 8 KB 13.1, 16 KB 13.1, 32 KB 53.5, 64 KB 62.2, 128 KB 61.6 ns per block (about 10.5 cycles, then 43-50 cycles)** (gcbench 3, linked blocks spread over the given code size) [device; 2026-10-10] bench/results/latest.txt L54-61; gcbench4_2026-10-11.txt L95-109 (64 KB 62.4) *Later timingprobe (job 176, TIMING_MODEL_PLAN L82-84) refines this: 1.0 cycle per instruction up to 16 KB, 2.0 at 20 KB, 2.6 at 24 KB, 3.39 from L2. FINDINGS reads it as 'L2 at about 45 cycles' Informs: Hot JIT code must stay under 16 KB per phase, which drives code-size and region-JIT choices*
- One-word block exits (dc09f3e): **Cutscene 17.69% (with --pmu-split on). Title heavy rows 18.15%. I-cache misses 9.42/8.55 M/s, up from about 8.3-8.8. Fetch stalls 288-289 M cycles/s.** (Separate runs) [device; relay jobs 58-59, 2026-10-12] core/out/relay_58.csv, relay_59.csv (second segment) *Job 58 had --pmu-split on Informs: Stubs between blocks spread hot code: neutral*
- One-word exits (budget at block entry): **Executed instructions -3 to -11%, I-cache misses +6%, title 18.21% vs 18.14% (level); stub area between blocks 2.0 -> 3.4 words per guest instruction. Stubs moved to the cold region: title 19.26%, cutscene 19.27%** [device; 2026-10-12] git 3605dfd body; 989b433 subject *Informs: Keep hot code dense; cold stubs out of line*
- One-word exits on 2DS (dc09f3e, relay 58-59): **neutral: title 18.21% (vs 18.14%), cutscene 17.45% (pmu-split on); executed instructions -3-11% while I-cache misses +6% (7.97 -> 8.46 M/s)** [device; 2026-10-12] docs/HANDOFF.md L726-729 *Informs: fewer executed instructions != speed; stubs between blocks spread hot code*
- Per-block time once JIT code is large (I-cache/BTAC thrash): **13 ns -> 61 ns** (microbenchmark, large JIT code footprint) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:406 (quoted from FINDINGS.md) *A synthetic footprint, not TP. Same mechanism NATIVE_PORT_PLAN blames for the AOT loss. Informs: JIT code-size budget and block layout; -Os vs -O2 for native builds*
- PMU split by region (--pmu-split, 4c5012a, relay 46-49): **compiled code (+called helpers) 88-96% of core 0 cycles, 95-99% of fetch stalls, ~all I-cache misses; traces cut executed ARM instr 5% and mispredicts ~30% without moving I-cache misses** [device; 2026-10-12] docs/HANDOFF.md L626-629 *pmu-split hooks cost speed; never mix into a speed comparison*
- Predictor of I-cache misses: **Executed hot words per guest instruction did NOT predict misses for OPT_RETSTUB; misses follow distinct hot lines touched (footprint)** (2DS jobs 148-156 vs model) [device; 2026-10-14] docs/research/REDESIGN_CANDIDATES.md:197-198,205-206 *No footprint counter (distinct hot lines per field) exists yet. Informs: What metric to optimize/predict JIT changes with*
- Share of core 0 cycles in compiled code + helpers (vs emulator C code): **88-96% of core 0 cycles, and essentially all fetch stalls and I-cache misses** (2DS, --pmu-split on) [device; 2026-10-12] docs/REGION_JIT_PLAN.md:20-21 *Informs: The JIT's output, not the emulator's C code, is the target*
- Share of core 0 cycles stalled on instruction fetch; I-cache miss rate; IPC: **33-36% of cycles; 8.2-8.8 M I-cache misses/s; IPC ~0.31** (2DS pmu columns, TP) [device; 2026-10-12] docs/REGION_JIT_PLAN.md:22,83-84 *AOT_PLAN.md:77-78 cites 44% of cycles stalled on fetch with IPC 0.31 (HANDOFF 2026-10-12); OPT_JIT's 9.7/33 = ~29%. Three different figures for the same quantity. Informs: Fetch-bound core 0: judge JIT changes by footprint/I-cache misses*
- Shared memory stubs (2 words per site instead of ~9): **15.26% on vs 14.72% off; I-cache misses 12.6 -> 9.5 M/s** [device; 2026-10-11] git a5318cb body *Informs: Code-size reductions pay*
- Traces on the 2DS (first runs): **Traces on: ~5,000 compiles/s and 1.3% speed vs 17% with traces off; ~98% of compiles at PCs new since the flush; trace blocks ~2x longer; the 16 MB buffer flushed every ~1.7 s (qemu: 60 vs 35 flushes in 50 fields at 8 MB); bc-on-CR side exits alone 15% -> 12.9%** [device; 2026-10-12] git 454f74f, 3eaa3cb, d4742d0 bodies *Informs: Traces off on the device*
- Traces re-measured (job 154): **core 0 cutscene 23.58 +- 0.17 on vs 24.95 +- 0.22 off (-5.5%), I-cache misses +17%, fetch stalls +28%; later stretch 12.21 vs 12.10 (medians 12.38 vs 12.46), fetch stalls +15%** [device; 2026-10-14] docs/HANDOFF.md L1015-1020 *Informs: traces rejected; 2DS rewards less distinct hot code, not fewer executed words (RETSTUB -1.8% static words gave +1-1.6%)*
- Traces re-measured (job 154): **Core 0 cutscene 23.58 +-0.17 on vs 24.95 +-0.22 off (-5.5%); I-cache misses +17%, fetch stalls +28%; later stretch even (12.21 vs 12.10), fetch stalls +15%; qemu showed -18% executed words** [device; 2026-10-14] git ede002f subject + diff (HANDOFF / REDESIGN_CANDIDATES C14) *qemu counts of executed words point the wrong way; the device follows distinct hot code. qemu also reported -29% code from the shadow-scene state (1fcb4d3) and 6.0 -> 4.9 hot words (a59e3d9). Informs: Rejected (C14); reduce distinct hot code*
- Hot code growth from float lanes: **~2-3% larger hot code (bigger exit stubs)** (OPT_FCACHE build) [pc; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:153-154 *Measurement method not stated in this file. Informs: Judge JIT changes by hot words / footprint as well as model*
- Concentration of JIT line touches (whole run incl. warm-up): **850 run: 50% of JIT line touches in 224 lines (7 KB), 75% in 1,869 (58 KB), 90% in 6,303 (197 KB), 99% in 26,759 (836 KB); 950 run: 209 (7 KB) / 1,826 (57 KB) / 5,928 (185 KB) / 25,421 (794 KB)** (Whole 30/25-field runs) [qemu_model; computed 2026-10-16] computed offline (icjit.awk) from ic_w850 and ic_w950 icache.txt *Includes warm-up compiles; touches per TB, not per instruction Informs: Half of execution sits in ~7 KB (tight loops already fit), but the long tail drives the misses*
- Concentration of steady-state JIT I-misses by cache line: **852-880: 25% in 392 lines (12 KB), 50% in 2,165 (68 KB), 75% in 6,442 (201 KB), 90% in 13,747 (430 KB). 952-975: 25% in 340 (11 KB), 50% in 1,621 (51 KB), 75% in 5,644 (176 KB), 90% in 12,500 (391 KB).** (JIT regions = stubs + hot + cold) [qemu_model; computed 2026-10-16] computed offline (icminus.awk) from ic_w850/ic_smoke and ic_w950/ic_w950s icache.txt *Line ranking by misses only Informs: Even pinning or locking the hottest lines covers at most a quarter of misses inside the 16 KB cache; no small hot set*
- Distinct hot JIT line footprint (steady state), 852-880: **59,524 hot-region lines touched (1.86 MB). 46,388 JIT lines (1,450 KB) touched at least once per field on average. 24,186 JIT lines (756 KB) miss at least once per field on average and hold 95.3% of JIT misses. The I-cache holds 512 lines (16 KB).** ('Once per field' means touches >= the 28 fields in the window, an average rather than every field) [qemu_model; computed 2026-10-16] computed offline from ic_w850 minus ic_smoke icache.txt (awk; scratchpad icminus.awk) *Previously called 'unmeasured' by the core-0 and memory lenses. Counts are of 32-byte lines of JIT code touched by executed qemu TBs. Informs: The per-field hot set is ~45-90x the I-cache, and the touched set (~1.9 MB) nearly fills the 2 MB shared L2, so layout tweaks cannot fit it; only cutting code volume by multiples can (optimizing JIT, AOT sizing)*
- Distinct hot JIT line footprint (steady state), 952-975: **58,452 hot-region lines touched (1.83 MB); 47,468 JIT lines (1,483 KB) >= 1 touch/field; 24,414 lines (763 KB) >= 1 miss/field holding 95.8% of JIT misses** (23-field window, retstub build) [qemu_model; computed 2026-10-16] computed offline from ic_w950 minus ic_w950s icache.txt *As above Informs: Confirms the footprint size in a second window*
- I-cache misses by TP function (qemu 952-975): **block code 48%, gcemu C 35%, cold 5%, stubs 4%; top 100 blocks 37%, top 1,000 76% of block misses; audio channel updates ~12%, PSMTXConcat 5.5%, __save/__restore_gpr 3.1%, J3DPSCalcInverseTranspose 2.2%, J3DGD* ~1% each; block-code misses ~17% of core 0 cycles (fetch stalls ~23%, ~3/4 block code)** [qemu_model; 2026-10-14] docs/HANDOFF.md L1032-1039 *device profile (job 146) found no function >0.25%, so not stand-in targets*
- I-cache misses by TP function (qemu 952-975): **Regions: block code 48%, gcemu C 35% (mostly GX), cold 5%, stubs 4%; top 100 blocks 37%, top 1,000 76% of block-code misses; audio channel updates ~12%, PSMTXConcat 5.5%, save/restore gpr 3.1%, J3DPSCalcInverseTranspose 2.2%; block-code misses ~17% of core 0 cycles (fetch stalls ~23%), so each is worth ~0.5-2%** [qemu_model; 2026-10-14] git 7071df8 diff *Informs: Native stand-ins for PSMTXConcat and audio math*
- I-cache simulation split (qemu plugin, TP 852-880): **JIT hot region 47% of misses, gcemu C 36% (on PC mostly GX code that runs on cores 2-3 on 2DS), libc 8%, cold 5%, shared stubs 4%; for core 0 block code ~3/4 of misses spread over ~60k lines (top 1,024 lines 32%, top 4,096 64%); dispatcher path 5-10%** [qemu_model; 2026-10-14] docs/HANDOFF.md L990-995 *Informs: no small hot set to pin; total footprint matters*
- I-cache simulator accuracy vs 2DS: **tracks the 2DS's misses 'fairly well' (no number)** (core/tools/qemu-plugins/icache.c, 16 KB 4-way 32 B) [qemu_model; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:19 *Qualitative; no calibration error stated. Only modelled mechanism.*
- I-cache simulator: where core 0 misses come from: **~3/4 of misses are JIT block code spread over ~60k lines; dispatcher C code 5-10%** (core/tools/qemu-plugins/icache.c + icache_report.py on qemu-arm) [qemu_model; 2026-10-14 night] memory/gc3ds-codesize-candidates.md:12-13 *Model; not calibrated against PMU Informs: Prefer removing C excursions and duplicated code over shaving executed words*
- Per-region miss rate per line touch: **JIT hot 13.5% (850) / 13.4% (950); JIT cold 18.6% / 19.2%; shared stubs 2.0% / 2.2%; C text 0.023% / 0.030%; all JIT 9.5%** (Whole runs) [qemu_model; computed 2026-10-16] computed offline from ic_w850 / ic_w950 icache.txt (icfoot.awk) *Line touches per TB execution Informs: Block code misses roughly 1 touch in 7: it is spread thin rather than looping*
- qemu cache simulator: instruction fetch misses, JIT vs AOT, same 20 fields: **JIT: 13,296,384,191 fetches, 49,681,133 misses (0.3736%); AOT: 8,185,697,726 fetches, 29,996,799 misses (0.3665%)** (qemu-arm cache plugin, core 0 of the qemu process, TP 850-870) [qemu_model; 2026-10-14] aot/cs/jit.plugin, aot/cs/aot.plugin *Device found AOT slower; the model's miss rates are near-identical and the AOT run fetched 38% fewer instructions, so this cache model did not predict the device result. Cache geometry of the plugin not verified here. Informs: whether the cache model can predict device I-fetch stalls*
- Simulated L1 I-cache totals, whole vcpu-0 thread, ic_w850 run: **47.57 G line accesses, 22.78 M misses over 30 fields (850->880)** (qemu-arm + libicache.so, 16 KB 4-way round-robin, 32 B lines, vcpu 0; build jit_create 0093c2d4 (before OPT_RETSTUB/SYSCALL/HLELINK defaults); 16 MB JIT buffer, traces off, fault mode off; from /root/tp_arm_850.state (TP cutscene)) [qemu_model; 2026-10-14 21:02] //wsl$/Debian/root/jitwork/ic_w850/icache.txt line 1; out.txt lines 23-25,111 (cycles 6,891,959,341 -> 7,135,214,444) *'accesses' = lines touched per qemu translation-block execution, not per instruction. The window is 30 fields, not 880: '880 fields' in out.txt is the field number since boot. The PC build renders GX on the same thread, so C text is 96.6% of accesses and 47% of misses. On the 2DS that GX code runs on cores 2-3. The totals include a 2-field warm-up (see the steady-state entries). Informs: Scale of core 0's instruction-fetch problem; baseline for code-size redesigns*
- Simulated L1 I-cache totals, whole vcpu-0 thread, ic_w950 run: **38.27 G line accesses, 23.23 M misses over 25 fields (950->975)** (Same simulator. Later build (jit_create 0093c484, OPT_RETSTUB on: bclr 3.6 hot words vs 6.6); from /root/tp_arm_950.state; JIT_BLOCKMAP written) [qemu_model; 2026-10-14 22:10] //wsl$/Debian/root/jitwork/ic_w950/icache.txt line 1; out.txt lines 23-25,111 *Different scene window and build from ic_w850, so the two are not an A/B. Includes a 2-field warm-up. Informs: Second sample of the I-fetch footprint (stability across scenes)*
- Steady-state I-misses by JIT block (blockmap): **20,124 live blocks spanning 2,090 KB. Block size incl. exit stubs: mean 106 B, median 76 B, p90 196 B, max 2,652 B. 18,488 blocks ran in steady state; 17,372 had >= 1 line touch/field; 14,143 had >= 1 miss/field. Hot-region misses: 25% in the top 46 blocks (33 KB of code), 50% in 224 (92 KB), 75% in 983 (234 KB), 90% in 3,036 (499 KB).** (952-975, 23 fields; a line is attributed to the block starting at or before line+31) [qemu_model; computed 2026-10-16] computed offline (blk.awk) from ic_w950/blockmap.txt + ic_w950 minus ic_w950s icache.txt *The blockmap is the end-of-run state (25,540 compiled, 20,124 live), so replaced blocks are misattributed. Consistent with the HANDOFF's 'top 100 blocks 37%, top 1,000 76%'. Informs: Short blocks (~19 words median) mean exit/entry overhead and alignment waste matter; per-function native stand-ins cover at most a few %*
- Steady-state simulated I-misses per field by region (warm-up subtracted), 852-880: **615.7k misses/field total: JIT hot 290.7k (47.2%), JIT cold 31.0k (5.0%), shared stubs 23.8k (3.9%), libc/other 47.0k (7.6%), gcemu C text 223.3k (36.3%, mostly PC-side GX)** (Subtraction per line; same build and state (deterministic emulation, matching 'range regions' lines)) [qemu_model; 2026-10-14 runs; computed 2026-10-16] computed offline from ic_w850/icache.txt minus ic_smoke/icache.txt (850->852), 28 fields *C text is inflated relative to the device (PC GX code shares this simulated cache). JIT misses are probably overstated slightly because PC-only C code evicts JIT lines. Informs: Block code is ~3/4 of core-0-relevant I-misses; shared stubs and cold code are small*
- Steady-state simulated I-misses per field by region (warm-up subtracted), 952-975: **728.1k misses/field total: JIT hot 348.9k (47.9%), cold 34.3k (4.7%), stubs 32.1k (4.4%), libc 56.4k (7.7%), C text 256.5k (35.2%)** (Retstub build, from tp_arm_950) [qemu_model; 2026-10-14 runs; computed 2026-10-16] computed offline from ic_w950/icache.txt minus ic_w950s/icache.txt (950->952), 23 fields *As for 852-880 Informs: Same region split in a second window: the hot-region share is stable*
- Cross-check: device I-misses per field vs simulator: **Device: 5.8 M I-misses/s at 20.5% speed is about 472k misses per emulated field. Simulator steady state: JIT + libc = 392k-471k per field.** (Assumes 59.94 fields/s at 100%) [estimate; 2026-10-16] derived: TIMING_MODEL_PLAN.md line 185 (job 184) vs ic_w850/ic_w950 steady-state computation *Different builds and scenes. Device misses include core-0 C code (dispatcher, HLE). The simulator includes PC-only C pollution. Informs: The I-cache simulator is calibrated well enough to rank code-size redesigns offline before 2DS runs*
- 3DS ARM11 L1 I/D cache size: **unverified: TRM allows 16/32/64 KB, 4-way, 32 B lines (16 KB is commonly claimed)** (would need a CP15 Cache Type Register read via svcCustomBackdoor) [unknown; 2026-10-10] docs/ARCHITECTURE.md:255-256, :453 *Open gap in this doc. hwprobe/qemu work elsewhere may have settled it; not in these files. Informs: I-cache footprint budget for JIT blocks and the qemu cache model's parameters*

### Data cache and memory (87)

- 804 MHz and L2 effect: **without L2 a 1 MB working set costs 188 ns per access vs 36 ns with L2; JIT integer speed tracks clock 109 -> 329 MIPS, unaffected by L2 (fits L1)** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:160-162 *Informs: Run with both 804 MHz and L2*
- A/B: CPU clock and L2 (JIT int MIPS, 1 MB chase, 16 MB chase): **268 MHz no L2: 109 MIPS, 237.2 ns, 243.2 ns. 804 no L2: 328, 188.4, 190.3. 268 + L2: 109, 108.7, 260.7. 804 + L2: 329, 36.1, 205.1** (gcbench 3, exheader clock/L2 modes) [device; 2026-10-10] bench/results/latest.txt L73-77; gcbench4 L126-134 *The L2 makes 16 MB misses slightly slower (190 -> 205 ns) but 1 MB 5.2x faster Informs: Run with 804 MHz + L2 both on (adopted)*
- Allocatable memory: **app region free 0 KB; max malloc 58.2 MB; max linearAlloc 30.44 MB; max vramAlloc 3.00 MB (free 0.00 after)** (gcbench 3, Homebrew Launcher) [device; 2026-10-10] bench/results/latest.txt L126-129 *Only 3 MB of VRAM is available to the app Informs: Budget for JIT cache, texture cache and EFB copies, and whether textures live in VRAM or FCRAM*
- AOT code size vs heap: **--cover 0.97: 8.4 MB of 9.9 MB text, largest malloc 6.6 MB (job 134) -> 0.4 MB (job 142), textures missing (jobs 135-143); --cover 0.90: 312 functions, 5.9 MB text, 94% of the guest instructions 0.97 ran; .3dsx 16 MB with 625 functions; 28 MB build left too little heap** (2DS (heap), PC (coverage TP 900-1020)) [device; 2026-10-14] docs/research/AOT_PLAN.md:92-95,118 *Informs: Heap budget for any large code/data structure; '# heap: largest malloc block' to watch*
- Cache line size (1 MB sequential chase stride 4/8/16/32): **6.9 / 9.7 / 15.4 / 26.8 cycles: 32-byte line; dcrmiss 0.125 per 4-byte load** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:144
- Chase vs random-read latency (system module): **chase: 8K 5.0, 256K 33.7, 24M 209.3 ns. Random: 8K 18.7, 256K 45.5, 24M 239.1 ns. 24M vs 256K miss cost +176 ns dependent** (gcbench 3 system module) [device; 2026-10-10] bench/results/latest.txt L132-134 *Informs: Miss cost for random guest access patterns (TLB and texture lookup)*
- Core 0 D-cache, TLB and branch events by section: **cutscene: D-read misses 1.14 M/s (4.42/1000), D-write 0.30 M/s, TLB 0.36 M/s (1.38/1000), branches 38 M/s, mispredicts 0.56 M/s (2.15/1000); later: D-read 0.83 M/s (2.18), TLB 0.20 M/s (0.52), mispredicts 1.01 M/s (2.65)** (clean run) [device; job 157] core/out/relay_157.csv frame_budget.py *Informs: Data stalls (~19-23 %) come from ~1 M misses/s: each costs ~150 cycles (derived), i.e. RAM latency*
- Core 0 data stall share in TP: **22%; 1.1 M read and 0.3 M write misses/s, plus load-to-use waits on L1 hits** (TP tp850) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:186 *Informs: Data layout / byte-swap / load scheduling*
- Core 0 data-side stalls and misses in TP: **read misses 1.07-1.15 M/s; write misses 0.25-0.39 M/s; stall_data 180-190 M cycles/s (~22-23%) in the cutscene, ~150 M/s (~19%) later; TLB misses ~0.33 M/s; stall_lsu 16-19 M/s** (tp850) [device; 2026-10-15, jobs 171/184] relay_184.csv, relay_171.csv columns dcrmiss_k, dcwmiss_k, stall_data_m, tlbmiss_k, stall_lsu_m *Informs: Data stalls are as large as I-fetch stalls; much of stall_data is load-to-use waits on L1 hits (byte swap, dependent loads)*
- CPU access to VRAM: **not CPU-accessible: a memcpy into vramAlloc memory crashed the app (run 2)** (gcbench 2/3) [device; 2026-10-10] bench/results/latest.txt L138 *Informs: Everything reaches VRAM via GPU DMA. The CPU writes linear FCRAM*
- Data latency by level (cycles): **L1 4 cycles (to 16 KB); L2 ~29 cycles (to 2 MB); FCRAM 126-170 cycles (4 MB+)** (2DS gcbench run 3 sweep 4 KB-32 MB) [device; 2026-10-10] docs/FINDINGS.md:158-159 *Informs: Keeping vertex/record working sets inside 2 MB L2 (C1, C3)*
- Data stall share of core 0: **22% of core 0 in TP** (TP, PMU stall_data_m) [device; relay job 184] core/tools/qemu-plugins/dcache.c:2 *PMU stall events rotate, so they are measured 1 s in 5 (pmu.c). They are not exclusive of the I-fetch stalls. Informs: Built the qemu D-cache simulator to trace where data misses come from*
- Denser indirect-branch table FAST_BITS=13 + 2-way (--ind2): job 193 vs baseline 191: **speed median (30+ s) 21.56 vs 21.47, mean 20.26 vs 20.21; dispatcher indirect misses 3.3k -> 16.7-17.6k/s; D-read misses 1.08 vs 1.04 M/s; largest malloc block 5.7 vs 3.9 MB** (tp850, 300 s each, separate runs (not A/B)) [device; 2026-10-15, jobs 191/193] relay_191.csv, relay_193.csv perf rows and '# dispatches/s' lines *TIMING_MODEL_PLAN quotes 1.8k -> 9.7k/s for table misses, a different counter or window from the dispatcher line I averaged. Informs: Neutral, not adopted: table reads mostly hit L2*
- Dependent load latency vs working set: **4-16 KB 5.0 ns (4 cycles); 32 KB 28.6 (23); 64 KB-1 MB 35.2-36.2 (28-29); 2 MB 45.0 (36); 4 MB 157.3 (126); 8 MB 192.0 (154); 16 MB 205.2 (165); 32 MB 210.8 (170)** (gcbench 3 pointer chase, 804 MHz + L2) [device; 2026-10-10] bench/results/latest.txt L78-92; gcbench4 L135-163 (4 MB 127, 32 MB 169) *Agrees with timingprobe job 176 (L2 ~28; 2/3/4/8/16 MB 35/102/128/156/166) Informs: The emulated working set beyond 2 MB costs 126-170 cycles per miss. Data layout and the timing model depend on it*
- Dependent random read latency: **5 ns within 8 KB, 34 ns within 256 KB, 210 ns over 24 MB (~170 cycles per miss)** (2DS gcbench run 2) [device; run 2] docs/FINDINGS.md:74-76 *Informs: Cost of guest working-set misses*
- Device memory headroom: **largest malloc block 20.0 MB at startup (job 93); linear free 13.0-14.2 MB, VRAM free 1,266 KB during runs; heap_free_mb column 0.0** (2DS jobs 91-94) [device; 2026-10-13] relay_93a.csv '# heap' line; heap/linear/vram columns *heap_free 0.0 suggests the app region is fully reserved by the allocator. Informs: memory budget for code buffers, texture caches or translated code*
- FCRAM bandwidth, 4 MB: memcpy / memset / read (2 words per line): **289-304 / 726-752 / 496-525 MB/s** (core 0) [device; 2026-10-15, jobs 177-196] relay_177.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing bw' *Informs: Cost of bulk copies (EFB/XFB, textures, vertex data) on the CPU*
- FCRAM memcpy bandwidth: **~307 MB/s** (CPU memcpy) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:418 (quoted from FINDINGS.md) *Informs: Texture decode/upload and vertex conversion throughput ceilings*
- FCRAM size and random miss: **128 MB Old 3DS + 128 MB New 3DS at PA 0x28000000; memcpy ~307 MB/s; random miss ~210 ns over 24 MB** [device; 2026-10-11] docs/research/PICA200.md:386-387 *Sizes from docs; speeds measured (FINDINGS). Survey notes FCRAM size inconsistent on 3dbrew.*
- Free memory during TP: **Heap free 0 MB as counted; largest malloc block 20.0 MB (job 99). Linear memory free 10.6-13.0 MB. VRAM free 1,266 KB.** (tp850) [device; 2026-10-13] heap_free_mb, linear_free_mb, vram_free_kb and '# heap:' in relay_83-99 *Informs: Room for caches and AOT code: about 20 MB heap, about 1.2 MB VRAM*
- GPU texture reads vs core 0 latency (job 192) and qemu data-miss sources: **GPU sampling a texture in FCRAM doubles core 0's L2 latency and makes its memory latency 3.6x. qemu D-miss sources in compiled code: guest RAM 42%, JIT buffer read as data 35%, indirect tables 19% (FAST_BITS 16: 512 KB per key)** [device; 2026-10-15] git a2625ba body *Supersedes gcbench 5's 'GPU does not slow CPU memory' (54db0da), which measured memcpy during a GPU fill, not texture sampling. The data-miss split is qemu. Informs: Texture placement (VRAM vs FCRAM); data-stall sources*
- Guest load path cost: **fastmem (bic+ldr+rev) 16.53 ns per access; range-check + fastmem 17.69 (+1.16 ns, +7%); C call per access 33.69 (2.04x). All paths agree** (gcbench 3, sequential over 4 MB) [device; 2026-10-10] bench/results/latest.txt L63-67; gcbench4 L111-118 (16.54/17.63/33.68) *Dominated by memory bandwidth (sequential 4 MB), so the instruction cost difference is understated for L1-resident data Informs: Range checks are nearly free. Avoid C calls on hot memory paths (2x)*
- H9 software TLB lookup cost: **+7.9 ns per access sequential; +38.8 ns random over 16 MB** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:248 *Informs: MMU (page-table) games are affordable*
- H9: software TLB lookup cost: **sequential: direct 109.8 ns, via TLB 117.7 (+7.9 ns, about 6 cycles). Random 16 MB: direct 236.6, via TLB 275.4 (+38.8 ns, about 31 cycles, +16%)** (gcbench 6) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L21-24 *Per-access cost of a software TLB vs fastmem. The random case is dominated by memory misses Informs: Fastmem vs software TLB: about 7% overhead sequential and 16% random*
- Heap headroom per build ('largest malloc block' at start): **20.0 MB (jobs 101-109); 9.6 MB (dlcache builds, 6 MB ring); 6.6-6.8 MB (second job without restart); 4.2 MB (job 144 on); 2.1 MB and 0.4 MB on AOT builds (135, 142); 1.1 MB on second jobs without restart (145, 150)** [device; jobs 101-170] '# heap' header lines, relay_101-170.csv *AOT builds lost textures (AOT_PLAN) while '0 textures not decoded for want of memory' still read 0 in relay_142/144 Informs: Memory, not only speed, limits code-heavy designs (AOT, big caches)*
- hw.txt random read latency: **34 ns within 1 MB; 210 ns across 16 MB** [device; 2026-10-10/11] docs/HANDOFF.md L353 *Informs: L2-resident data (2 MB) vs FCRAM; ring/cache sizing*
- Indirect-table wipe cost: **Wiping the 4096-entry blr/bctr table on block-key change cost 3.2M D-cache write misses/s and took the title from ~15% to 12.9%** [device; 2026-10-11] git 60d83c0 body *Informs: One indirect table per block key*
- Kernel DMA (svcStartInterProcessDma) 4 MB FCRAM->FCRAM and core 0 impact: **192 MB/s (memcpy ~300); core 0 1 MB chase unchanged 34.5 ns** (--probe-risky, default config) [device; job 196] docs/research/TIMING_MODEL_PLAN.md:203-205 *Informs: Candidate for bulk copies off core 0 (does not pollute L2)*
- Kernel DMA probe (job 196): **4 MB FCRAM -> FCRAM at 192 MB/s (memcpy on core 0 ~300 MB/s); core 0's 1 MB chase unchanged at 34.5 ns during DMA; VRAM vs FCRAM texture: 70 vs 35 ns, 704 vs 195 ns** [device; 2026-10-15] git 86fe949 diff *Informs: DMA can offload bulk copies without L2 pollution*
- L1 cache geometry (CP15 cache type): **0x1d152152: L1 I 16 KB 4-way 32-byte lines; L1 D 16 KB 4-way 32-byte lines** (CP15 read) [device; 2026-10-10] bench/results/latest.txt L70-72 *Informs: Code and data footprint budgets, and cache-line separation of shared counters*
- L1 D associativity (4 KB stride: 4 lines / 5 / 6 / 8): **hit / 8.3 / 16.5 / 27 cycles: 4-way, 4 KB way, 16 KB; replacement not LRU** [device; job 183 (181 within 1%)] docs/research/TIMING_MODEL_PLAN.md:142 *Informs: Cache simulator parameters*
- L1 load-to-use (pointer chase), cycles: **4.0; ldr then use or one instruction between: same** (timingprobe chase <=16 KB) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:74,85 *gcbench 5 C4 found +2 cycles per load used immediately vs 2 instructions later (33 vs 17 cycles for 8 loads); the two framings are consistent only if the hiding needs 2 intervening instructions. Matches SD5113 prior of 4-cycle L1 hit. Informs: JIT load scheduling*
- L2 and FCRAM latency: **L2 (2 MB) ~29 cycles; FCRAM ~170 cycles** (gcbench, 804 MHz, L2 on) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:43 *Informs: Memory misses dominate; shared L2 makes cross-core work costly*
- L2 associativity (128/256 KB stride: 16 / 17 / 20 lines; 64 KB stride 32): **16 hit / 43.5 / 91 cycles: 16-way, 128 KB way, 2 MB; not LRU** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:143 *Informs: L2 simulator parameters*
- Load latency L2 by working set: **24 KB 18; 32 KB 23; 64 KB-1.5 MB ~28 cycles; L1 miss rate 62% at 24 KB, 82% at 32 KB, 100% at 64 KB+** (timingprobe chase) [device; job 176; counters job 178] docs/research/TIMING_MODEL_PLAN.md:86,95-96
- Load latency memory (FCRAM) by working set 2 / 3 / 4 / 8 / 16 MB: **35 / 102 / 128 / 156 / 166 cycles** (timingprobe chase, 804 MHz) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:87 *Earlier figure: random miss ~210 ns over 24 MB (PICA200.md:387), consistent (166 cycles ~206 ns).*
- M1 bandwidth by heap and size (read / write / copy MB/s): **heap 16 KB 828 / 1980 / 469; 256 KB 500 / 801 / 464; 4 MB 437 / 785 / 322; 12 MB 427 / 786 / 305. Linear heap the same (16 KB 828/1981/469, 256 KB 499/802/463, 4 MB 436/782/322; 12 MB alloc failed)** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L562-578 *Informs: Linear heap costs nothing over the normal heap, so GPU-visible buffers can live in linear memory*
- M1 bandwidth: 16 KB (L1) and >=4 MB, read / write / copy: **16 KB: 828 / 1980 / 469 MB/s; >=4 MB: ~430 / ~785 / ~310 MB/s; heap = linear** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:18
- M2 memcpy bandwidth while the GPU fills: **gcbench 5: 2 MB memcpy idle 376 MB/s, during GPU fill 414 MB/s (110%), so no contention. gcbench 4b: 4,935,878 vs 4,883,252 MB/s, INVALID (memcpy optimised away)** (gcbench 5 with the result consumed (checksum)) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L79-81; gcbench4b_2026-10-11.txt L48-50 *The fill test touches VRAM only. Contention with GPU reads of FCRAM (vertex or texture in linear heap) is not measured Informs: Whether the CPU loses bandwidth while the PICA renders*
- M3 data-cache flush of dirty lines: GSPGPU_FlushDataCache vs svcFlushProcessDataCache: **4 KB: GSPGPU 42.0 us, svc 26.3. 64 KB: 126.0 vs 104.6. 256 KB: 1678.2 vs 194.9. 1 MB: 2643.9 vs 609.7. 4 MB: 2705.4 vs 2096.1** (gcbench 4 wave 2, linear buffer memset dirty, mean of 4) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L579-589; method bench/src/wave.c L57-87 *Informs: Use svcFlushProcessDataCache, and flush only changed ranges (adopted in MEASURE_PLAN)*
- M3 GSPGPU_FlushDataCache on already-clean lines: **4 KB 436.0 us; 64 KB 1612.1; 256 KB 1461.5; 1 MB 2058.7; 4 MB 2514.5 us** (gcbench 4 wave 2, third call on the same range right after the svc flush) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L581-589 ('clean' column) *Not transcribed in MEASURE_PLAN. Flushing a clean 4 KB via GSPGPU cost 10x the dirty flush: GSP IPC overhead is large and erratic Informs: Never call GSPGPU flush speculatively or per draw. Track dirty ranges*
- M3 svcFlushProcessDataCache cost 4 KB / 256 KB / 1 MB / 4 MB: **26 us / 195 us / 610 us / 2.1 ms; GSPGPU_FlushDataCache 1.5-8x slower** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:19; docs/research/GPU_GUIDANCE.md:71-73 *Informs: Flush with SVC, only what changed*
- M5 PLD on random 16 MB gather: **236 -> 203 ns per word with prefetch (-14%)** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:56 *Informs: Prefetch in gather / decode loops*
- M5 PLD prefetch on a random gather over 16 MB: **0 ahead 236.2 ns per word; 4 ahead 202.9; 8 ahead 203.0; 16 ahead 202.9 (-14%)** (gcbench 5) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L12-17 *Latency-bound. Only one outstanding miss appears to help Informs: Prefetch buys at most about 14% on gathers (e.g. vertex index fetch, texture decode)*
- Malloc heap left vs .3dsx build size: **16 MB .3dsx left 0.4 MB largest malloc block (vs 6.6 MB before); 28 MB build failed socInit** (AOT builds, AOT on or off) [device; 2026-10-14] memory/gc3ds-aot-verdict.md:16-18 *Texture decode scratch now falls back to linear memory Informs: Build size budget; textures silently vanish when heap is starved*
- Max malloc / linearAlloc available to the .3dsx: **58 MB max malloc; ~30 MB linearAlloc** (.3dsx under Luma, libctru default heap split) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:333-339, :413 (quoted from FINDINGS.md) *Matches libctru's default split with ~90 MB free (inference). Overridable with __ctru_heap_size. Later native-mode runs report different free figures (78.5 MB free at start). Informs: Heap budget for JIT cache, texture cache, Wii MEM2 (24+64 MB exceeds 58 MB default)*
- memcpy / memset / read (2 words per line), 4 MB FCRAM: **301 / 742 / 517 MB/s** (core 0) [device; job 178] docs/research/TIMING_MODEL_PLAN.md:102 *Earlier memcpy ~307 MB/s (FINDINGS) and gcbench 4 M1 copy ~310 MB/s agree. Byte accounting (write-allocate) not stated.*
- memcpy and bswap copy, 4 MB heap: **memcpy 307 MB/s; bswap32 copy 314 MB/s (ok)** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L130-131 *gcbench 5 M2 got 376 MB/s for a 2 MB memcpy; timingprobe job 178 got 301 MB/s for 4 MB Informs: Byte swapping on copy is free relative to bandwidth*
- Memory access path cost (sequential 4 MB): **masked fast path (bic+ldr+rev) 16.6 ns; range check + fast path 17.7 ns; C function call per access 33.7 ns** (2DS gcbench run 2) [device; run 2] docs/FINDINGS.md:60-69 *Dominated by RAM bandwidth, not instruction count. Informs: Range checks nearly free in streaming; C calls double cost*
- Memory headroom with large builds: **97% AOT coverage: 8.4 MB of 9.9 MB text, malloc's largest block 6.6 MB -> 0.4 MB, textures went missing (jobs 135-143). 90% coverage: 312 functions, 5.9 MB, still 94% of guest instructions. 3DS build 13 MB vs 28 MB with hot/cold layout. Second job on one build: largest block 1.1 MB vs 4.2 MB** [device; 2026-10-14] git a939fde diff; 11c376d, 56bd0cd, 509061e bodies *Informs: Heap is a hard constraint on code size and caches*
- Memory space available: **largest single malloc 58 MB (after CPU module fragmentation); 88 MB malloc succeeded in run 1; linearAlloc 30 MB; vramAlloc 3 MB per allocation; VRAM 6 MB total** (2DS gcbench) [device; run 1/2] docs/FINDINGS.md:77-81,194 *In the TP emulator, largest malloc block was 6.6 MB (job 134) — far smaller than the bench figure. Informs: Budgets for caches, translated code, texture cache*
- Misses in flight: 8 MB random chase with 1/2/3/4/6 independent chains (cycles per load): **155.9 / 146.7 / 146.3 / 145.7 / 146.2** (core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing mlp 8M' *Informs: One outstanding miss: batching or overlapping loads gains nothing; favour compact, sequential structures*
- Misses in flight: 8 MB random, 1/2/3/4/6 chains: **156 / 147 / 146 / 146 / 146 cycles per load: one outstanding miss** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:146 *SD5113 prior said hit-under-miss up to 3 misses; device shows no MLP. Informs: No gain from interleaving/batching loads*
- Native-mode memory headroom: **78.5 MB free at start; 33.5 MB after the 45 MB arena** (native build, run 2) [device; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:81 *Differs from the 58 MB max malloc of the emulator .3dsx (different binary and heap split). Informs: Native heap sizing; code-size growth risk*
- One store per line, 512 KB / 8 MB (vs load): **32.4 / 83.7 cycles (load 15.8 / 46.6): write-allocate, dcwmiss 1.0, ~2x a load miss** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:147 *Informs: Avoid scattered stores; compact write structures*
- Other memory headroom: **linear memory free 11.9-13.0 MB; VRAM free 1,266 KB; heap_free 0.0 MB** [device; jobs 101-170] linear_free_mb / vram_free_kb / heap_free_mb columns at ~200 s, relay_101-170.csv *Informs: Budget for GPU-resident vertex buffers (C1 needs ~25k vertices per field)*
- Pointer-chase load latency by working set (cycles): **<=16 KB 4.0; 24 KB 18.2 (62% L1 miss); 32 KB 23.0 (82%); 64 KB-1.5 MB 27.6-29.2; 2 MB 33-35; 3 MB 102; 4 MB 128; 8 MB 156; 16 MB 166** (core 0, 804 MHz, L2 on) [device; 2026-10-15, jobs 176/178] relay_176.csv, relay_178.csv '# timing chase' *Informs: Data-structure sizing for guest RAM, lookup tables and the JIT's own data (L2 hit ~28 cycles, memory 100-166)*
- RAM bandwidth: **memcpy 307 MB/s; byte-swapping copy 313 MB/s** (2DS gcbench run 2) [device; run 2] docs/FINDINGS.md:73 *Informs: Vertex gather / texture decode bandwidth limits*
- Random read latency: **256 KB: 33.6 ns. 1 MB: 33.7 ns. 16 MB: 208.8-209.8 ns.** (hwprobe, one thread) [device; 2026-10-10/11] core/out/hw_2026-10-10.txt, hw_2026-10-11.txt *Informs: Cost of a miss in L2 (about 34 ns) against RAM (about 210 ns, about 170 cycles). Explains why the display-list cache lost: a hit waits for RAM.*
- SCU (coherency) counters by section: **cutscene scu_rd ~14.6-15.7 M/s, core-0 misses ~1.13-1.18 M/s, core-2 misses ~1.26-1.66 M/s, writes ~4.9-5.3 M/s; later scu_rd ~10.3-10.6 M/s** [device; jobs 101-164] scu_* columns, relay_101/127/157/164.csv *Column semantics not verified from the code Informs: Possible direct gauge of L2/coherency traffic in multicore experiments*
- SCU traffic (job 93): **core-0 linefill misses 1.13 M/s, core-2 1.48 M/s; SCU external reads 14.95 M/s, writes 4.45 M/s** (2DS cutscene) [device; 2026-10-13] relay_93a.csv scu_* columns *Units are events/s as scaled by pmu.c; bytes per event not established. Informs: bus pressure is moderate; core 2 misses more than core 0*
- Sequential vs random memory access, 8 MB (cycles per load): **stride 4/8/16/32/64/128/256/4096 = 7.9/13.7/25.0/47.9/79.8/125.9/129.1/167.5 vs random chase 156** (core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing seqchase 8192K' *Informs: In-order line access is ~3x cheaper than random up to 64 B stride (prefetch or open DRAM page); gone by 128 B*
- Sequential vs random memory, 8 MB (stride 32 / 64 / 128 / 4096 vs random): **48 / 80 / 126 / 168 vs 156 cycles: in-order lines ~3x cheaper, gone by 128 B** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:145 *Cause (prefetch vs open DRAM pages) not identified. Informs: Sequential data layouts pay off*
- Store miss vs load miss, one access per line (cycles): **8 KB store 2.06 / load 2.56; 512 KB store 32.4 / load 15.8; 8 MB store 83.7 / load 46.6 (dcwmiss 1.0: write-allocate)** (core 0) [device; 2026-10-15, jobs 183/192/196] relay_183.csv '# timing store/load N one per line'; repeated relay_192.csv, relay_196.csv *Informs: Write-allocate makes store misses ~2x a load miss: matters for vertex-worker output buffers, guest stores, job rings*
- Store then load same word, + add: **5.07 cycles per trio: no forwarding penalty** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:148 *Informs: Spilling guest regs to the Gekko struct is cheap*
- Streaming stores: **4.1 cycles per word (~780 MB/s)** (timingprobe stream) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:90 *gcbench 4 M1 write >=4 MB ~785 MB/s agrees.*
- Streaming stores over 4 MB (cycles per word): **4.1 (about 780 MB/s), stall_lsu 2.75-2.87 per word** (core 0) [device; 2026-10-15] relay_176.csv, relay_177.csv, relay_178.csv, relay_183.csv '# timing stream 4M stores' *Informs: Bandwidth ceiling for bulk writes such as vertex output and texture decode*
- TLB: one line per page across N pages (cycles per load): **8 pages 4.0; 16 pages 9.0; 32-512 pages 6.9-4.5; 1024 pages 23.5; 4096 pages 144** (core 0) [device; 2026-10-15, job 176] relay_176.csv '# timing tlb' *Informs: Whether page/TLB layout matters: no TLB cliff (heap in large pages); the 1024/4096 costs are cache/memory, not TLB*
- TLB: one line per page, 8-512 pages; 1024 / 4096 pages: **4.0-9.0 cycles (no cliff, heap in large pages); 23 / 144 cycles (cache/memory, not TLB)** (timingprobe tlb) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:88-89 *Informs: TLB modelling not needed for heap; negative result*
- VRAM per-allocation cap: **3 MB (two pools A/B, one allocation never spans both)** (libctru vramAlloc) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:340-341; docs/GPU_BACKEND_PLAN.md:38 *6 MB VRAM total. Informs: Texture cache and render-target placement in VRAM*
- Compiled code's data miss sources (L1 miss rate 1.4%): **guest RAM 42%; JIT buffer read as data 35%; large mappings (indirect tables) 19%; Gekko struct 3.5%** (core/tools/qemu-plugins/dcache.c, TP 850-910, compiled code only, first 100 M warm-up) [qemu_model; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:206-208 *Simulator L1 counts overstate L2-resident data; PC build uses 8 MB buffer, not 2DS pattern. Informs: JIT literal placement, indirect table layout*
- Data misses by issuing code: **indirect stub table read 427 k (L2 hits); link literals 203 k (mostly L2 misses); storepipe 161 k; float stubs ~200 k** (dcache.c, TP 850-910) [qemu_model; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:208-210 *link literal misses reflect PC build's 8 MB buffer refills.*
- dcache2 data misses by issuing code: **Shared stubs (52 PCs): 1.062 M load + 90k store, 216k L2. Hot block code (201k PCs): 1.038 M load + 200k store, 126k L2. Cold: 5.1k. Top PCs: 0x438040bc (indirect table read) 427k load, 3.3k L2; 0x43804034 (link literals) 203k, 172k L2; 0x43804240 (storepipe) 161k, 10.9k L2; 0x438058a8 115k, 2.8k L2; 0x43804b18 58k.** (As dcache2) [qemu_model; computed 2026-10-16] computed offline from dcache2/dcache.txt 'pc' records; stub names from TIMING_MODEL_PLAN.md lines 208-210 *Data region and issuing code are not cross-tabulated in the file Informs: The indirect table and link literals are the stub-side data costs; job 193 (FAST_BITS 13 2-way) was neutral on the device*
- dcache2 L1 data misses by data region: **Guest RAM (MEM1): 773k load + 235k store = 1.008 M (42.1%), 20,824 lines (651 KB), L2 26.3k (2.6%). JIT buffer read as data: 839k load (35.1%), 185,462 lines, L2 312.8k (37.3%; 91% of all L2 misses). Stack/mmap (indirect tables): 454k (19.0%), 5,530 lines, L2 3.3k (0.7%). Gekko struct: 84.5k (3.5%) from 28 lines, L2 0. ELF static: 9.4k (0.4%).** (As dcache2) [qemu_model; computed 2026-10-16 (matches TIMING_MODEL_PLAN.md lines 206-210)] computed offline (dreg.awk) from dcache2/dcache.txt using out.txt ranges (mem1 40a0b000-4220b000, jit 43804000-44004000, gekko 00af4bb8-00af5f18) *The JIT-buffer-as-data misses are mostly link-literal reads after the PC build's 8 MB buffer refills, not the 2DS pattern (TIMING_MODEL_PLAN) Informs: Guest-RAM misses are mostly L2 hits. 28 lines of the Gekko struct missing 84k times show the guest register file is evicted between uses, which bears on register allocation.*
- Memory instructions by address form: **r1+d 3,860,994 (21.4%), r2+d 581,799 (3.2%), r13+d 1,359,057 (7.5%), other rA+d 11,421,864 (63.4%), rA+rB 799,085 (4.4%), 0+d 868; total 18.02 M = 40.2% of 44.81 M executed** (qemu, 30 fields) [qemu_model; 2026-10-12] regstat_base/out.txt *Informs: stack/SDA accesses (~32%) could skip address checks; memory ops are 40% of the stream*
- qemu cache simulator: data accesses and misses, and the single dominant missing instruction: **JIT: 7,704,045,618 accesses, 798,446,445 misses (10.36%), of which 551,551,409 (69%) at host address 0x40931798; AOT: 4,165,267,677 accesses, 415,912,902 misses (9.99%), 203,136,133 (49%) at 0x40931798** (qemu-arm cache plugin, TP 850-870) [qemu_model; 2026-10-14] aot/cs/jit.plugin, aot/cs/aot.plugin *Address not symbolised in these files; whole-process counts; JIT run also compiled ~9x more blocks with 10 flushes, which may drive this routine. Informs: a single host routine (outside the JIT buffer and the AOT text, likely a shared-library memset/memcpy) holds most simulated D-misses; identify it before any memory redesign*
- Share of simulated L1 data misses that also miss L2 (the 50- vs 150-cycle question): **14.3% of all L1 misses (342,367 / 2,394,943). Excluding the JIT-buffer-as-data artefact: 1.9% (29,585 / 1,555,457). Guest RAM 2.6%, indirect tables 0.7%, Gekko 0%.** (As dcache2) [qemu_model; computed 2026-10-16] computed offline from dcache2/dcache.txt *A lower bound: the device L2 is shared with code, the GX thread, the vertex worker and the GPU. GPU texture reads double L2-hit latency (job 192: 34.6 -> 70.1 ns at 1 MB). Informs: Price core 0's data misses at roughly L2-hit cost (~28 cycles idle, up to ~2x under GPU contention), not memory cost (~130-166), when budgeting redesigns*
- Simulated L1/L2 D-cache totals, compiled code only (dcache2): **93.50 M loads, 72.57 M stores; 2.103 M load misses (2.25%), 292k store misses (0.40%); L1 miss rate 1.44%; 342k L2 misses** (libdcache.so: L1 16 KB 4-way round-robin, L2 2 MB 16-way LRU, 32 B lines, write-allocate, vcpu 0, compiled code's accesses only, first 100 M accesses as warm-up (TIMING_MODEL_PLAN.md line 207). TP 850->910 (60 fields). 8 MB JIT buffer, traces on, fault mode on (14,984 sites, 18,980 faults), 32 flushes, 371,100 blocks compiled.) [qemu_model; 2026-10-15 14:07] //wsl$/Debian/root/dcache2/dcache.txt line 1; out.txt *The L2 sees only this thread's data (no code, no other cores, no GPU), so L2 misses are a lower bound. L2 is LRU here but not on the device. The heavy flush churn is a PC-build artefact. Informs: Data-miss volume and where it lands (L2 vs FCRAM)*
- Unfiltered whole-thread D-cache run (dcache, first run): **27.0 G loads, 22.1 G stores; 45.4 M load misses, 776 M store misses, 377 M L2 misses (45.9% of L1 misses). ELF static data 51.6% and stack/mmap 46.9% of misses, from C code ('other code' 558 M store misses). JIT code in this run: hot 1.04 M load / 386k store, 200k L2; stubs 734k / 455k, 142k L2.** (850->870 (20 fields), no code filter, no warm-up skip, 10 flushes, traces on, fault mode on) [qemu_model; 2026-10-15 13:40] //wsl$/Debian/root/dcache/dcache.txt line 1 + dreg.awk; dcache/out.txt *Dominated by PC-only same-thread rendering. Superseded by dcache2 (code-filtered) for compiled-code numbers. Informs: Only as evidence that unfiltered PC runs mislead about core 0's memory behaviour*
- Display-list vertex cache memory vs game heap (native): **DL vertex cache ~10 MB malloc; removing it in native builds raised the game's largest free block after OSInit from 16.8 to 26.8 MB (Aurora's FIFO had failed to grow)** (native build) [azahar; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:136-138 *The doc says these findings came from Azahar and a stream dump. Whether these heap figures were taken on the device is not stated. Informs: Removing the DL vertex cache in native builds*
- Application memory modes (New 3DS): **Old 0: 64 MB; New 1/3: 124 MB; New 2 (Dev1): 178 MB** (3dbrew Memory layout/exheader) [estimate; 2026-10-10] docs/ARCHITECTURE.md:321-331, :462 *A .3dsx inherits the host title's mode (inference). Whether a .3dsx under Luma can raise its own mode is unverified. FCRAM 256 MB; VRAM 6 MB; AXI WRAM 512 KB. Informs: .cia vs .3dsx delivery; Wii feasibility; native-mode heap*
- ARAM DMA rate (guest): **246 CPU ticks per 32 bytes, ~63 MB/s** (Dolphin, measured on hardware by Dolphin) [estimate; 2026-10-11] docs/research/GCWII.md:148 *Informs: ARAM DMA as memcpy plus timed completion*
- C18 byte-swapped RAM priced gain: **~3-4%** [estimate; 2026-10-15] docs/HANDOFF.md L1151-1152 *priced, not built*
- L2 cache, TLB, store buffer: **L2 2 MB PL310 (New 3DS only); main TLB 64-entry 2-way, 8 lockable, 4K/64K/1M/16M pages; store buffer 4 x 64-bit merging slots** (gbatek/3dbrew/TRM) [estimate; 2026-10-10] docs/ARCHITECTURE.md:257-259 *Spec values, not measured. Informs: Guest RAM mapping and page sizes; memory-model parameters*
- Memory budget for native ports: **New 3DS APPMEMTYPE 6 = 124 MB; GC needs 24+16 = 40 MB; Wii up to 88 MB, which leaves ~36 MB for code, GX, audio and loader** [estimate; 2026-10-15] docs/research/DECOMP_SURVEY.md:75-80 *Treating the New 2DS XL as matching New 3DS is inferred. ARCHITECTURE lists a 178 MB Dev1 mode; the native run measured 78.5 MB free at start. Informs: Wii titles in native mode*
- Simulated vs device L1 D-misses per field: **Device: 1.1 M read + 0.3 M write misses/s at 20.5% speed is about 114k per field (all core-0 code). Simulator, compiled code only: about 64k per field (dcache2, assuming ~37 counted fields); 133k per field for JIT code in the unfiltered run, where PC C code pollutes L1.** (Assumes uniform access rate to convert the 100 M warm-up skip into fields) [estimate; computed 2026-10-16] derived: TIMING_MODEL_PLAN.md line 186 (job 184); dcache2 / dcache dcache.txt *Rough bracket only; different builds and windows Informs: Compiled code accounts for roughly half to all of the device's D-misses. The rest is C, idle hook and stubs.*
- Wii guest RAM: **88 MB; heap split needs changing** (Wii support) [estimate; 2026-10-10] memory/tp3ds-gc-wii-on-2ds.md:47 *Informs: Memory map for Wii*

### GX pipeline (GX thread, worker, backend) (103)

- 5328b90 title speed and wait: **14.8%; core 0 waits for core 2 22% (game's own GPU syncs); 99.0% of draws on batch fast path; setups 0.9%** [device; 2026-10-11, 5328b90] docs/HANDOFF.md L197-199
- Batch / merged-job size: **batch ~90 TP draws; merged job ~28 draws (~140 vertices, ~30 KB)** (TP with merging) [device] core/src/gx/pica/pica.c:990-996 *Informs: Job-ring sizing, split policy*
- Core 0 time waiting for the GX thread (gxwait_pct) in TP: **Job 184 over 300 s: mean 12.0%, median 13.3%. By window: 30-100 s 18.5%, 100-200 s 11.9%, 200-300 s 6.8% mean / 2.0% median. Later stretch (300-570 s, jobs 171/190/195): 0.5-1.2%.** (tp850; GX thread on core 2) [device; 2026-10-15] relay_184.csv, relay_171.csv, relay_190.csv, relay_195.csv column gxwait_pct *HANDOFF cites ~11% for job 184 and a 1.1-1.4% median for job 190. Both depend on which stretch is included. Informs: GX co-limits core 0 only in the heavy cutscene. In the later stretch core 0's JIT is the sole limit.*
- Core 0 waiting for the GX side (gxwait_pct): **9-16% in TP heavy rows (10.0-10.2% in later A/B off arms)** (2DS, perf.csv gxwait_pct, mostly at polling-loop skips) [device; 2026-10-13] docs/OPT_JIT_PLAN.md:8-11; PERF_LEVERS.md:12-13; REDESIGN_CANDIDATES.md:37,124 *Sampler files this under 'dispatch'. Informs: Converting speed to core-0 speed; GX vs core 0 cap*
- Core 2 cost per draw (3a1696f): **busy 80% at ~50,000 draws/s; ~16 us/draw, all fixed per draw: vprep 6.8 us, vpal 3.0 us, gxstate 2.9 us, rest of job path 2.8 us** [device; 2026-10-12] docs/HANDOFF.md L648-650
- Cost of copying display lists into Aurora's FIFO: **~388 display lists/frame copied = 680 KB, 4.2 ms; stream scanned again 7.3 ms** (title demo) [device; job 217, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:95-96 *Informs: Display lists by reference*
- Cutscene cores 2-3 breakdown (job 163): **core 0 waits GX 14%; GX thread 81.5% busy, ~23% waiting ring space (~5 jobs of ~21 KB fit in 128 KB); worker 61% busy (gather and colours ~70% of it, random reads of guest arrays); ~63k draws/s merged into ~2,900 jobs/s; GPU ~11.6 ms/frame** [device; 2026-10-15] docs/HANDOFF.md L1065-1068
- Display transfer 2x2 downscale: **240x400 -> 120x200 hung the GPU in gcbench 4 and again in 4b (reproducible). G14b 480x800 -> 240x400 (citro3d anti-alias layout) works: 0.154 ms, correct centre pixel** (gcbench 4 / 4b / 5) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L495-497; gcbench4b L65-67; gcbench5 L82-84 *Informs: Half-scale EFB copies must use a 2x source layout or another path*
- Display transfer 400x240 RGBA8 -> RGB8: **1.98 ms; 1.69-1.98 ms across jobs** (gpuprobe) [device; jobs 178-180] docs/research/TIMING_MODEL_PLAN.md:109,133 *gcbench 4 G14: tiled->linear 1.67 ms; gcbench 4b: 0.57 ms (RGB565 0.27). 3x discrepancy unresolved. Informs: EFB->XFB via display transfer cost per frame*
- Display-list share and replay (D1): **99.6% of TP draws inside display lists; ~80% of list calls replay identical bytes over the same array bases** [device; 2026-10-13, job 107] docs/HANDOFF.md L905-906 *Informs: display-list vertex cache*
- Draw merge switch: **merge = 24% (re-check); other existing switches within noise** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:19 *Unclear whether 24% is total speed or a component Informs: Merge kept default*
- Draw merging A/B: **game speed 17.26 -> 21.60 % (+25 %); core0 own 24.47 -> 24.75 (unchanged); core 0 GX wait 27.1 -> 12.7 %; GX thread vertex time 67.1 -> 42.6 %** (300 s, cutscene) [device; job 121 (2026-10-13)] core/out/relay_121.csv, ab_split.py / gx_split.py --col merge *Informs: Merge is the biggest GX-side lever; keep on*
- Draw merging A/B (f56d430, relay 52-53): **title 18.77% on vs 16.26% off (+15%); cutscene 17.44 vs 15.97 (+9%); core 2 busy 80 -> 66%; core 0 wait 11.8 -> 6.0%; vpal 15 -> 1.8%; vprep 33 -> 15%** [device; 2026-10-12] docs/HANDOFF.md L663-665 *Informs: merge on (re-measured later as worth 24%)*
- Draw merging and GX jobs on device: **56,517-73,157 guest draws/s merged, 96.1-96.6% joined an open draw, 2,065-2,844 jobs/s (25.7-29.4 draws each); batch path fast 66.5-71.8%, new batch 28.2-33.5%; vertex setups 5,084 of 29,855 draws per 10 s** (2DS cutscene at ~20% speed) [device; 2026-10-13] relay_86a.csv, relay_93a.csv '# merge 10s', '# gx draws 10s' lines *Scaled estimate for full speed is mine. Informs: at full speed TP would need ~290-360k guest draws/s merged into ~10-14k jobs/s*
- Draw merging on the GX thread: **Title 18.77% vs 16.26% off (+15%); cutscene 17.44% vs 15.97% (+9%). PC: 96.4% of 4.67M draws join, 28 per job. The GX thread then waited for ring space 17% while core 3 was half idle. Later batch 120-124: merge worth 24%** [device; 2026-10-12] git f56d430, 9388639, c62ee46, d1b179b bodies *Informs: Biggest GX-side win*
- Draw merging, in-run A/B: **Title: 16.60 → 18.87% (+13.7%); core 0 18.60 → 19.92%; GX thread 81.1 → 67.2%; gxwait 11.0 → 4.7%; the GX thread's wait for the vertex worker (vwait) 0 → 17.7%. Cutscene: 15.94 → 18.20% (+14.2%); GX thread 82.2 → 69.2%; gxwait 14.6 → 3.5%; vwait 0 → 18.2%. 96.5% of draws joined an open draw: about 28.6 draws per job, 1,991-2,234 jobs/s.** (Title / tp850, 20 s phases) [device; relay jobs 52-53, 2026-10-12] core/out/relay_52.csv, relay_53.csv (A/B on the merge column; '# merge 10s') *Informs: Draw merging is the largest GX-side lever (later measured at +24%). Kept on.*
- Early frame splits with merged jobs: **Splits 64 -> 49-55/s; GX thread waiting for GPU 1.7 -> 6-7%; core 0 waiting for core 2 4 -> 14% (old gate); core 2 a co-bottleneck (core 0 waits 12%)** [device; 2026-10-12] git 60d54fa body; cc53e52 subject *Informs: Split gating*
- Early splits at the recorded draw calls (splitopen), in-run A/B: **Cutscene: 17.60 → 18.40% (+4.5%), gxwait 12.8 → 1.2%, core 0 own 20.33 → 18.72%. Title: 17.78 → 18.34% (+3.1%), gxwait 10.6 → 5.7%.** (20 s phases) [device; relay jobs 56-57, 2026-10-12] core/out/relay_56.csv, relay_57.csv *Informs: Kept on (later re-measured as within noise)*
- Frame skip experiment (--ab fskip, job 128): **+10% speed; with half the vertex work gone core 0 waits on GX ~5%; core 0 own speed ~23% cutscene, ~11% later stretch** [device; 2026-10-13] docs/HANDOFF.md L953-956 *Informs: core 0 is the wall*
- Frame skip on the GX side (--fskip, job 128): **speed 19.81% -> 21.80% (+10%, ~3 sigma); core 0 own speed 22.33% -> 23.29% (noise); core 0 GX wait 10.2% -> 5.4%; worker 54% -> 30% busy; first 110 rows +21%** (2DS cutscene, 300 s, --ab fskip) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:123-128 *Visible glitches (repeated images, lagging render-to-texture). Shows core 0 is the wall at ~23% once vertex work halves. Informs: Core 0 (JIT) is what 30-40% needs; fskip parked as user option*
- G14 display transfer 400x240 tiled->linear; to RGB565: **gcbench 4: 1.67 ms / 0.88 ms; gcbench 4b: 0.57 ms / 0.27 ms** [device; 2026-10-11] docs/research/MEASURE_PLAN.md:17,37; docs/research/GPU_GUIDANCE.md:66-68 *Runs disagree 3x; gpuprobe jobs 178-180 gave 1.69-1.98 ms (RGBA8->RGB8).*
- G14 DMA engine, gcbench 4 run: **display transfer 400x240 RGBA8 tiled->linear 1.671 ms (230 MB/s of source); ->RGB565 0.878 ms (438 MB/s); 2x2 downscale 240x400->120x200 hung the GPU (>2 s timeout); TextureCopy, combiner and contention skipped** (gcbench 4 W2 after about 23 s of GPU tests; frame_begin -> transfer -> frame_end_timed (poll C3D_FrameBegin NONBLOCK, 20 us sleeps), mean of 5) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L490-507; method bench/src/gpu_w2.inc L383-424 *2.9-3.3x slower than gcbench 4b with the same code path. See the reconciliation entry Informs: EFB->XFB and EFB-copy cost on the DMA engine*
- G14 DMA engine, gcbench 4b run: **display transfer 400x240 tiled->linear 0.570 ms (673 MB/s); ->RGB565 0.267 ms (1439 MB/s); TextureCopy 256 KB linear->VRAM 0.434 ms (604 MB/s); clear 400x240 colour + depth target 0.166 ms (4617 MB/s); 2x2 downscale hung the GPU again** (gcbench 4b mode f, fresh GPU init, the same xfer_timed code, frame overhead included) [device; 2026-10-11] bench/results/gcbench4b_2026-10-11.txt L56-69 *GPU_GUIDANCE rule 13 adopted these numbers and ignored the 1.67 ms run Informs: DMA budget for XFB present, EFB copies and texture moves*
- G14b 2x2 downscale from 2x target 480x800 -> 240x400: **works, 0.154 ms** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:52; docs/research/GPU_GUIDANCE.md:102-103
- G15 framebuffer flush + invalidate between draws: **+0.8 us GPU per draw (0.77 -> 1.58 us)** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:49; docs/research/GPU_GUIDANCE.md:98-99 *Informs: Mid-frame EFB copies are cheap GPU-side*
- G15: framebuffer flush + invalidate between draws: **plain 0.77 us per draw GPU; with flush + invalidate 1.58 us per draw (+0.8 us)** (gcbench 5, 1000 one-triangle draws) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L54-58 *Informs: Cost of EFB-coherence points (read-after-write of the framebuffer) inside a frame*
- G3 uniform upload per draw: one matrix (3 vec4); 84 vec4: **+0.7 us CPU, +2.1 us GPU (GPU 0.73 -> 2.8 us per draw); 84 vec4: 9.4 us CPU, 10 us GPU** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:12; docs/research/GPU_GUIDANCE.md:38-40 *Informs: Upload palette only when matrices change*
- G3: uniform upload per draw (CPU / GPU us per draw): **0 vec4: 3.26 / 0.73 (1000 draws); 3 vec4: 3.92 / 2.80 (892 draws); 12: 4.75 / 3.58 (480); 48: 7.26 / 4.61 (168); 84: 9.36 / 10.19 (102)** (gcbench 4 W2, citro3d draws) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L460-469 *Draw counts drop with uniform size (probably a command-buffer limit), so the per-draw figures come from different N Informs: One uniform change stalls the GPU about 2 us. Upload the palette when matrices change, not per draw*
- G4 CPU cost per draw: citro3d C3D_DrawArrays vs minimal register sequence; GPU per draw: **3.25 us vs 0.74 us CPU; GPU 0.74 vs 1.25 us** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:11; docs/research/GPU_GUIDANCE.md:32-37 *Informs: Write draws as raw commands*
- G4: per-draw cost, citro3d vs raw register sequence: **C3D_DrawArrays: CPU 3.25 us per draw, GPU 0.74 us. Raw sequence: CPU 0.74 us, GPU 1.25 us (1000 one-triangle draws)** (gcbench 4 W2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L455-459 *Timingprobe job 178: GPU 0.55 / CPU 3.21 us per draw; 0.63 / 3.51 with a combiner change Informs: Write draws directly into the command buffer (GPU_GUIDANCE rule 6)*
- G5 several draws in one draw window: **works, no hang; 0.34 us CPU per draw (raw 0.74, citro3d 3.25); GPU 0.66 us** (gcbench 5, draw_onewindow) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:47; docs/research/GPU_GUIDANCE.md:90-95 *Does not cover state changes between draws. GPU_GUIDANCE:126-127 'Not yet measured' list is stale on this. Informs: Batch draws in one window*
- G5: several draws in one draw window: **works (drawn, no hang): CPU 0.34 us per draw, GPU 0.66 us per draw (1000 one-triangle draws)** (gcbench 5) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L85-87 *Informs: Draw batching cost: about 0.34 us CPU per draw is the floor*
- G7 fog index: z-buffer vs W-buffer depth map: **z-buffer: near 1 / far 100 puts everything past 24 units in indices 0-5; W-buffer (scale 0, offset 1/far): 2 at d=1.5, 123 at d=96 (linear)** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:7; docs/research/GPU_GUIDANCE.md:52-60 *Hardware = Azahar's model. z-fighting on TP terrain after change not yet checked. Informs: Dune fog fix: W-buffer + linear LUT*
- GPU draw calls, submits, EFB copies, GPU time on device: **gpu_calls ~1,134-1,153/s, submits ~7.2-7.3/s, EFB copies ~46/s (3.6 per field), texture decodes ~0.1/s, GX bytes ~960 KB/s; c3d_gpu_ms 11.3-12.1 ms per frame (cutscene)** (2DS cutscene jobs 91/93; title job 94: 756 calls/s, 16.3 submits/s, 28 EFB copies/s) [device; 2026-10-13] relay_91a.csv, relay_93a.csv, relay_94a.csv *c3d times are citro3d's own counters; c3d_cpu_ms (~120-150) not interpreted here. Informs: GPU is lightly used at 20% speed; ~89 GPU calls per field*
- GPU draw cost, ~1-triangle draw: GPU / CPU per draw; with a combiner change: **0.55 / 3.21 us; 0.63 / 3.51 us** (gpuprobe, citro3d path) [device; job 178] docs/research/TIMING_MODEL_PLAN.md:107 *Clipped triangles (void clipping path, see job 178 note). Agrees with gcbench 4 G4 citro3d 3.25 us CPU. Informs: Per-draw CPU overhead of citro3d*
- GPU draw time on TP title scene: **~20 ms of each ~190 ms frame; CPU side is the limit** (TP title scene) [device; 2026-10-11] docs/research/PICA200.md:15-16 *Older; later ~8-11 ms per frame (TIMING_MODEL_PLAN.md:200, gameplay).*
- GPU draw time per TP frame: **~8-11 ms** (TP gameplay) [device; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:200 *Source run not named. PICA200.md:15 gave ~20 ms of a ~190 ms frame on the title scene. Informs: Window during which GPU contends with core 0*
- GPU draw-call cost: **citro3d 3.25 us CPU per call vs 0.34 us inside one draw window; ~1,100 GPU draw calls/s in TP heavy rows = ~0.3% of core 2** (2DS gpu_calls_s, jobs 93-98; GPU_GUIDANCE rule 20) [device; jobs 93-98] docs/research/PERF_LEVERS.md:69,72-74 *Informs: Draw-call batching not a lever*
- GPU drawing time vs CPU work per frame (citro3d C3D_GetDrawingTime): **~20 ms GPU vs ~190 ms CPU per frame; TP title scene ~15% speed (30 fps = 100%)** (TP title scene, PICA backend, before GPU position transform was measured) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:11-12 *Before many later JIT changes; title scene, not gameplay Informs: GPU is mostly idle; move work onto the GPU (vertex shader transform, C1 GPU-resident vertex data)*
- GPU position transform A/B (29a22f7): **transform on 11.3% vs off 12.7%; core 2 vertex share 50 -> 56% (cost per draw ~12 us of ~20 us, gather ~5 us); core 3 34 -> 30%; core 0 wait 28 -> 36%; gxsync 7 -> 14%; GPU 30 -> 40 ms/frame (back faces); ~2,150 palette uploads/s, 116 extra draw calls/s** [device; 2026-10-11, perf_2026-10-11_gpupos.csv] docs/HANDOFF.md L416-421 *run printed gpupos twice per row: PMU columns shifted by one*
- gpupos × esplit (periodic splits), 4-phase A/B with 20 s phases: **(gpupos, esplit) = (0,0): 13.35%. (0,1): 13.56%. (1,0): 15.00%. (1,1): 13.26%. 18 rows each. Core 0 own: 18.2, 15.4, 21.0, 14.6%.** (Title from boot) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 15 (= perf_2026-10-11_split4.csv) *Only 18 rows per cell. The 5 s run (next entry) ranks the cells differently. Informs: Whether to split periodically and whether to transform on the GPU*
- gpupos × esplit, 4-phase A/B with 5 s phases, 676 s: **(0,0): 12.28%. (0,1): 13.93%. (1,0): 11.93%. (1,1): 12.23%. 85-87 rows each. esplit without gpupos: gxwait 18.4 → 4.4%, c3d_gpu_ms 17.5 → 6.1.** (Title from boot, 2 s settle) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 16 (= perf_2026-10-11_ab5.csv) *5 s phases are short. This contradicts the 20 s run's best cell (1,0). Informs: Periodic splits +13%. GPU transform 0 to -12%.*
- GSP present rate limit: **flooding the GPU service crashed it; present at most once per 3DS refresh** [device; 2026-10-10] docs/GPU_BACKEND_PLAN.md:61 *Qualitative. Informs: Present pacing*
- GX and GPU work per second and per field, cutscene at 21.5% speed: **Per second: 62.6k draws, 310k vertices, 1,147 GPU calls, 7.1 submits, 92 splits, 46 EFB copies, 6.7 XFB copies, 953 KB of GX FIFO data. Per field: about 4,870 draws, 24k vertices, 89 GPU calls.** (tp850 cutscene) [device; 2026-10-12/13] core/out/relay_83.csv, relay_84.csv, relay_97.csv (0-295 s) *Informs: GX-side capacity a redesign must reach*
- GX command and state rates in the cutscene: **GX FIFO ~1.0-1.35 MB/s; palette uploads ~3.3k/s; draw calls for palette/projection changes ~212/s; TEV program changes 758/s, 33 compiled/s; batch fast path 60.8 %; EFB copies 45-48/s, XFB copies 6.7/s** (clean run) [device; job 157] '# gpu transform', '# tev', '# gx draws' lines and gxbytes_k column, relay_157.csv *Informs: Sizes state-change cost for a GPU redesign*
- GX DMA copy linear -> VRAM (3 MB), with and without cache flush: **886 MB/s DMA alone, verified by readback (gcbench 4: 897). Including GSPGPU_FlushDataCache: 248 MB/s overall (247). GX heap->VRAM refused: GSP takes linear or VRAM sources only** (sys.c, 16 iterations, PPF event wait) [device; 2026-10-10] bench/results/latest.txt L135-137; gcbench4 L256-258; method bench/src/sys.c L215-254 *The flush, not the DMA, dominates (3.6x). Coincides with TIMING_MODEL_PLAN's 'texture copies 230-250 MB/s' (gpuprobe, which does no flush), so that figure is not the DMA rate Informs: Texture and EFB upload path: flush with the SVC and only what changed. DMA itself is about 0.9 GB/s*
- GX side in the cutscene (gx_split, defaults): **GX thread 81.5 % busy, waiting for ring space 23.1 %; worker 60.9 % busy; gxvtx 41.7 %, gxstate 9.6 %, gxtex 5.2 %; 63.4k draws/s, 316k vertices/s; 1,229 GPU calls/s, 95 splits/s; EFB copies 47/s; core 0 GX wait 13.9 %** (cutscene arm with defaults) [device; job 163 (2026-10-15)] core/out/relay_163.csv, gx_split.py --col opt --val 627 *Informs: The cutscene's cap is the vertex worker and ring on cores 2-3*
- GX side in the cutscene (job 163, defaults): **Core 0 waits for GX 14%; GX thread 81.5% busy, ~23% of it waiting for ring space (~5 jobs of ~21 KB in 128 KB), 3.4 jobs in flight; worker 61% busy (gather + colours ~70% of it); ~63k draws/s merged into ~2,900 jobs/s; GPU ~11.6 ms per frame** [device; 2026-10-15] git 03d6bc5 body; c089ecc diff *GPU time here (11.6 ms/frame) is lower than the 10-11 '~20 ms of ~190 ms' figure, measured on an older pipeline. Informs: Where cores 2-3 spend time*
- GX side in the later stretch: **GX thread 42.9 % busy, ring wait 10.4 %; worker 28.5 %; 32.8k draws/s, 165k vertices/s; core 0 GX wait 0.55 %** (defaults) [device; job 163] core/out/relay_163.csv gx_split.py *Informs: Later stretch is purely core-0-bound*
- GX thread (core 2) and vertex worker (core 3) busy share in TP: **Cutscene: GX thread 80-84% (median), vertex worker 60-63%. Later stretch: GX thread ~41%, vertex worker ~28%.** (tp850) [device; 2026-10-15, jobs 171/184/190] relay_184.csv, relay_171.csv, relay_190.csv columns gxthread_pct, vwork_pct *Informs: Headroom on cores 2/3: at 5x speed the GX work would need ~4-5x the current cores*
- GX thread (core 2) time waiting for vertex-job ring space: **22-25% (~23%); 21% in job 125's off arm** (2DS TP heavy scenes) [device; 2026-10-13] docs/research/PERF_LEVERS.md:12-13; REDESIGN_CANDIDATES.md:33,37-38; DL_CACHE_PLAN.md:8-9 *A bigger ring trades this wait for core 0's gxwait (live A/B, job 99). Informs: Whether the vertex worker caps the GX side*
- GX thread breakdown (b439173): **core 0 outside JIT 18%; core 0 waits for GX 17% (was 2%); core 2 busy 75%: vertex path 47% (decode 20%, lighting 4%), state 15%, textures 6%, sync 6%; ~17 us of core 2 per draw; vertex setup cache 99.1% hit** [device; 2026-10-11, b439173] docs/HANDOFF.md L190-194 *Informs: cost is fixed per-draw work*
- GX thread waiting for the GPU without early frame splits: **7-14% of the time** (TP) [device; commit 29a22f7] core/src/gx/pica/pica.c:389-391 *Informs: Split the frame every 8192 arena vertices (SPLIT_VERTS)*
- GX-side frame skip A/B: **speed 20.81 -> 22.42 % (+7.7 % trimmed; medians 22.12 -> 24.92, +12.7 %); core0 23.48 -> 24.04 (noise); core 0 GX wait 9.4 -> 5.0 %; worker 56.6 -> 31.0 % busy; GX thread 78 -> 51 %; GPU 11.1 -> 5.0 ms; 3.7-4.5 frames/s skipped, 31-45k draws/s not made** (300 s cutscene only) [device; job 128 (2026-10-13)] core/out/relay_128.csv, ab_split.py / gx_split.py --col fskip; '# frame skip' lines *REDESIGN_CANDIDATES quotes 19.81 -> 21.80 (+10 %) from the older split Informs: With GX load halved, core 0 (~24 %) is the wall*
- H1: EFB copy to RAM by size (transfer + invalidate + CPU convert): **240x400: 0.484 + 0.474 + 1.980 = 2.94 ms. 256x256: 0.356 + 0.216 + 1.345 = 1.92 ms. 128x128: 0.116 + 0.198 + 0.337 = 0.65 ms. 64x64: 0.070 + 0.043 + 0.083 = 0.20 ms. Pixel verified (40c08020)** (gcbench 6) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L58-66 *CPU conversion dominates at large sizes (about 48 Mpx/s). The display transfer can convert to RGB565 in 0.27 ms instead Informs: EFB->RAM copy path: GPU conversion vs CPU, and how often TP's copies can go to RAM*
- H2: depth buffer (D24S8) readback: **raw TextureCopy returns exact values: quad at z 0.25 -> 0xC00000 (0.75 of full scale); z 0.75 -> 0x400000 (0.25); stencil 00** (gcbench 6) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L37-41 *Mapping is reversed (1 - z). Data is tiled, so untile on the CPU Informs: Z copies and Z peeks (lens flare visibility) are possible*
- H3: synchronous EFB peek (32-byte copy, wait, invalidate): **92.8 us mean, 712.4 us worst** (gcbench 6) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L34-36 *The worst case is 7.7x the mean (wait jitter, consistent with lost or late wake-ups) Informs: EFB access design: a few peeks per frame are fine (about 0.3% of a 33 ms frame each); many need one cached readback per frame*
- Indexed draws (indexed) × gpupos: **indexed off → on (10 s phases): first 295 s 13.74 → 15.81% (+15%), gxwait 17.8 → 4.4%. Whole run 12.96 → 14.42%. By cell (gpupos, indexed): (0,0) 14.04, (0,1) 13.99, (1,0) 12.71, (1,1) 14.47%.** (Title from boot, 542 s) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 17 (= perf_2026-10-11_idx.csv) *The cell split disagrees with the per-flag split when gpupos is off Informs: Indexed draws kept. Most of the gain was with the GPU transform on.*
- Indexed draws x GPU transform A/B (862fe63): **indexed + GPU 14.56% (made default); not indexed/CPU 13.93%; indexed/CPU 13.75%; not indexed/GPU 12.93%; SE ~0.27; core 0 waits core 2 only 2.8%** [device; 2026-10-11, perf_2026-10-11_idx.csv] docs/HANDOFF.md L446-448 *Informs: core 0 became the limit*
- Job-ring sweep (speed_pct, cutscene): **1 MB 18.9%, 256 KB 21.2%, 128 KB 21.4%, 96 KB 21.5%** (tp850 cutscene, one size per job) [device; relay jobs 76-80] core/host3ds/main.c:404; core/src/gx/pica/pica.c:1002 *Each size was a separate job, not interleaved. 128 vs 96 KB differ by 0.1 points. pica.h:44 still documents a 256 KB default (stale). Native mode uses 4 MB. Informs: 128 KB default ring*
- Light content runs at full speed: **triangle.dol: 99.72%, 59.83 fields/s, 120 draws/s, GX thread about 5%. TP light screen (title runs 46, 57, 75, 92, 94): 59.85-59.89 fields/s for about 60 s at about 179 draws/s, core 0 36-45% busy, GX thread 15-16%.** (Title from boot, the light screen at about 20-85 s) [device; 2026-10-12/13] core/out/relay_3.csv; relay_46, 57, 75, 92, 94 (rows with fields_s > 55) *Informs: The whole pipeline holds 60 fields/s when the guest's work is light*
- Merge statistics in the cutscene: **67.7k draws/s, 96.4 % joined an open draw, 2,457 jobs/s (27.5 draws per job); open draws issued for a state change 1.36k/s, could not join 1.0k/s** (clean run) [device; job 157] '# merge 10s' line at ~150 s, relay_157.csv *Later stretch: 35.4k draws/s, 96 % joined, 24.9 per job Informs: Draw-count reduction is already near its limit*
- Native renderer run 227: Aurora's display-list optimizer off (GC strips): **~11,700 draws / ~62,000 vertices per frame; 6.3-8.6 fps; core 2 100% busy on per-draw work** (title demo) [device; job 227, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:153 *Informs: Per-draw overhead dominates with many small draws*
- Native renderer runs 229-234: optimizer on, indexed draws: **~250 draws / ~44,000 vertices per frame; 5.6-7.5 fps; Aurora commands 830-1060 ms/s on core 2** (title demo; pica_draw_indexed) [device; jobs 229-234, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:154 *Core 2 is saturated (more than 830 ms of command work per second). Informs: Converted shapes*
- Per-draw CPU setup costs: **Parsing 7-8 GX lights per draw cost 2-4k cycles; 645k of 652k title draws are shader-lit; a clock read costs 0.25 us; with version-cached state, setup is recomputed for 1.0% of draws** [device; 2026-10-11] git e4fa5a1, 29fce90 bodies *Informs: Cache per-draw state*
- PICA backend per second in TP cutscene: GPU calls / submits / splits / EFB copies / XFB copies / clears; GPU drawing time per frame: **1,216 / 7.3 / 97 / 48 / 6.8 / 8.7; C3D drawing time 10.5 ms median (11.9 mean) per frame at ~13 frames/s** (tp850) [device; 2026-10-15, job 184] relay_184.csv columns gpu_calls_s, gpu_submits_s, gpu_splits_s, efbcopy_s, xfbcopy_s, clears_s, c3d_gpu_ms (30-250 s) *c3d_gpu_ms is C3D_GetDrawingTime: the last frame's GPU time. Informs: The GPU itself is ~14-16% busy and is not TP's limit*
- PICA frame time and GPU lighting: **PICA draws a title frame in ~2.4 ms; lighting was ~a quarter of GX-thread time before shader lighting; shader lighting matches 63,472 lit draws within 1/255** [device; 2026-10-10] git 7b57b16 body *Informs: Shader lighting*
- Re-measure of older switches on current build (jobs 120-124): **merge worth 24%; pld, traces, sched, splitopen within noise** [device; 2026-10-13 night] docs/HANDOFF.md L949-950
- Render to texture + 256x256 readback (FINDINGS): **3.9 ms** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:77; docs/research/PICA200.md:380 *Second-hand from FINDINGS.md. Informs: EFB readback (Z copies, CPU-side effects) cost*
- Render to texture 256x256 RGBA8: **baseline 1.980 ms per cycle; RTT direct 1.976 (switch cost -0.004 ms, free); RTT + TextureCopy 3.976 (+2.0 ms); RTT content 5% of pixels drawn** (gcbench 3/4, 2000 triangles then a quad; +copy = C3D_FrameSplit + GX_TextureCopy VRAM->VRAM) [device; 2026-10-10] bench/results/latest.txt L222-227; method bench/src/gpu.c L748-764 *The +2.0 ms includes a mid-frame command-list split and wait. The DMA of 256 KB alone measures 0.33-0.43 ms elsewhere. The '4 EFB copies/f = 8.0 ms' budget is built on this 2.0 ms and is probably about 4x pessimistic Informs: EFB-to-texture copies stay on the GPU. Avoid frame splits per copy*
- RTT readback to CPU 256x256: **3.92 ms (67 MB/s): transfer 1.50, invalidate 1.66, CPU read 0.76 ms** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L228-229 *gcbench 6 H1 measures the 256x256 transfer at 0.356 ms and invalidate at 0.216 ms, so this run's 1.50 / 1.66 ms are 4-8x higher. Another instance of the slow-sync mode Informs: Cost of EFB copies to RAM*
- Software reference renderer (soft.c) speed on 2DS: **~5% of full speed on 3D test programs** (triangle/NeHe-style test programs, not TP) [device; ~2026-10-10] docs/GPU_BACKEND_PLAN.md:3-4 *Superseded: the PICA200 backend (core/src/gx/pica) now exists. Informs: Moving drawing to the PICA200 (adopted)*
- Splits at recorded draw calls (60d54fa, relay 56-57): **title 18.00 vs 17.56%; cutscene 18.14 vs 17.61%; core 0 wait 12.7 -> 1.6% (title); gxsync 5.9 -> 0.4%; splits 58 -> 78/s** [device; 2026-10-12] docs/HANDOFF.md L676-678 *re-measured later (jobs 120-124) as within noise*
- Splitting only at closed batches with merging on: **splits fell from 67 to 49-55/s; GX wait for GPU rose 1.7% -> 6-7%** (TP, draw merging on) [device; commit 9388639] core/src/gx/pica/pica.c:1293-1298 *Negative result. Informs: Split at recorded draws, open batch stays open*
- Starting citro3d's GPU queue at each split: **Title 12.6% -> 15.1%; GX thread's frame-start wait 15% -> 3%; core 0 then waited for GX 11%** [device; 2026-10-11] git 1d45007, da2aefd bodies *Informs: Overlap GPU with GX recording*
- Starting citro3d's GPU queue at each split (earlyq), in-run A/B: **Speed 12.18% → 15.50% (+27%). gxwait 27.7 → 14.2%. Core 0 own 17.0 → 18.2%.** (Title from boot, heavy rows) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 8 (= perf_2026-10-11_1d45.csv, commit 1d45007) *Informs: GPU overlap with the GX thread is worth a lot. Kept on.*
- The later stretch is GX-light: **From about 300 s into tp850: draws 61k → 30k/s, GX thread 80 → 40%, gxwait 9-12 → 0.5%, valloc 22-25 → 9.5-10.5%, speed 21 → 11.5%** (tp850, 600 s) [device; 2026-10-13] core/out/relay_97.csv; r117b, r125b, r129b-r134b *Informs: Two different limits in one save state. A redesign has to fix core 0 for this stretch.*
- Title scene speed by build: setup cache, slimmer shader, citro3d queue start at split: **29fce90 batch key/vertex setup cache 12.4%; bb18017 slimmer lighting shader 12.6%; 1d45007 citro3d GPU queue started at each split 15.1% (off 12.2%)** [device; 2026-10-11] docs/HANDOFF.md L154-155 *Informs: starting GPU work early is a big lever (+24%)*
- Title scene through the GX-side fixes: **11.6% with worker (9.4% without) after state caching; 12.4% (10.3%) with the slimmer 294-instruction shader (was 406); the GX thread then waited for the GPU 19%** [device; 2026-10-11] git 29fce90, bb18017 bodies *Informs: Shader length vs GPU time*
- TP draw rate and GX-thread cost per draw: **~50,000 draws/s of ~5 vertices; ~16 us of core 2 per draw** (TP, before draw merging) [device; commit 3a1696f] core/src/gx/pica/pica.c:1728-1731 *The per-draw cost is from before merging. Informs: Draw merging*
- TP title from boot with GX inline on core 0 (no GX thread): **Heavy rows: 4.69% speed (n105) and 4.91% (n21). About 13,000 draws/s, 136-151 GPU calls/s.** (Title from boot, JIT on, no GX thread) [device; 2026-10-10] core/out/perf_2026-10-10_run2.csv (2 runs; run1 is its prefix) *Superseded by every later build Informs: Baseline that justifies the GX thread*
- Transform x early-split A/B (eee65ec): **CPU transform + early splits 13.77%; baseline 12.20%; GPU transform 12.01% (no splits) / 11.91% (splits); SE ~0.2; early splits +13% (core 0 wait 20 -> 6%); GPU transform GPU 25 -> 36 ms/frame, vqueue 11%, vpal 6%; winner core 2 vprep 50% (vdec 21%), ~18 us/draw** [device; 2026-10-11, perf_2026-10-11_ab5.csv] docs/HANDOFF.md L435-439 *Informs: early splits kept; GPU transform lost*
- Uniform Z-texture depth resets: **~650 quads per frame** (TP title scene) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:78 *Informs: Correctness fix (pink/white screen)*
- Vertex-shader transform on the GPU (gpupos), first A/B (20 s phases): **Off: 12.79%. On: 11.39% (-11%). Core 0 own 17.9% in both. GPU calls 496 → 555/s. c3d_gpu_ms 28.9 → 36.8.** (Title from boot, heavy rows) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 14 (= perf_2026-10-11_gpupos.csv). The header lacks the esplit column, so I re-parsed it. *Draws/s differ between arms (40.0k vs 34.3k): scene confound Informs: GPU vertex transform loses on the 2DS*
- Vertex-worker job-ring size: **256 KB ring: GX thread waited for ring space 17% while core 3 half idle; a 4 MB ring made GX vertex time grow (L2)** (TP with merging) [device; commit f56d430] core/src/gx/pica/pica.c:994-997 *Superseded by the relay 76-80 sweep (next entry). Informs: Ring size as a run option*
- Write-gather pipe store rate: **~136k stores/s (TP)** [device; 2026-10-11] docs/HANDOFF.md L312-313 *source of rate not stated; likely device perf.csv*
- TEV -> PICA translation differences at field 690 (PC PICA model): **4 draws differ (max channel diff 255, 192, 128, 101) of 345,106 draws** [pc; 2026-10-10] core/out/tp/tevcheck_690.txt *Informs: Combiner translation accuracy*
- TP depth-buffer reset workload: **~650 Z-texture quads per frame** (TP, tp_ztex.sh) [pc] core/src/gx/pica/pica.c:712-715 *A workload count. Informs: Uniform Z texture folded into vertex depth*
- Azahar: draw merge and batching on lesson11: **31,557 draws/s; 98.4% joined an open draw; 62 draws per job; batch path fast 96.8%** (Test DOL) [azahar; 2026-10-15] azahar-sdmc/perf.csv lines ~1896-1899, 1951-1954 *Test DOL, Azahar Informs: Merge path works*
- Azahar: VRAM and linear memory free: **vram_free 2,834 KB; linear_free 21.8-22.2 MB** (Test DOL) [azahar; 2026-10-15] azahar-sdmc/perf.csv data rows (lesson11) *heap_free_mb column reads 4068, an Azahar artefact Informs: VRAM budget for GPU-resident vertex data (C1) and textures*
- citro3d GX queue capacity: **32 entries; runs only at FrameEnd (libctru panics when full)** (citro3d) [azahar; 2026-10-10 evening] memory/tp3ds-gc-wii-on-2ds.md:64 *Library constraint found during Azahar bring-up Informs: GX queue handling in the PICA backend*
- citro3d register writes per C3D_DrawArrays: **~13 GPU registers per draw call (mode toggles + post-vertex cache clear)** [estimate; 2026-10-11] docs/HANDOFF.md L221-222, L567 *from research agents reading citro3d source, not measured on device*
- GX-side rates full speed would need (extrapolated): **About 292k draws/s, 1.44 M vertices/s and 5.3k GPU calls/s at 60 fields/s in the cutscene. In the later stretch: about 265k draws/s.** (tp850) [estimate; 2026-10-12/13] Derived from relay_83 and relay_97 per-field rates × 60 *Linear extrapolation Informs: About 4.7x on core 2 and on the vertex worker, unless merging and caching cut the work*
- PICA freeze pitfalls: **each configured shader output component must be written exactly once or the PICA freezes; two consecutive MOVA freeze it; 4-bit textures in VRAM on TEXUNIT1 can destabilize the GPU** (3dbrew GPU/Pitfalls) [estimate; 2026-10-10] docs/ARCHITECTURE.md:377-378 *Not measured here. Informs: Shader generator and texture placement rules*
- PICA200 shader and pipeline limits: **268 MHz; 3 vertex processors + geometry shader; <=512 instructions, 96 float4 constants, 24-bit float; 6 TexEnv stages; 3 texture units + procedural; power-of-two textures 8-1024** (3dbrew) [estimate; 2026-10-10] docs/ARCHITECTURE.md:349-368 *Spec values. Informs: TEV->combiner mapping (GX has 16 stages, 8 maps, indirect texturing); multipass vs dropping stages*
- Reconciliation of the DMA-engine conflict (0.57 vs 1.67-1.98 ms; 600-900 vs 230-250 MB/s): **Fast set agrees on about 800-900 MB/s: 256 KB DMA 0.33 ms (upload a/b2), 3 MB GX copy 886-897 MB/s, 4b 400x240 0.57 ms and TextureCopy 0.434 ms with overhead, gcbench 6 H1 240x400 transfer 0.484 ms. Slow set at 1.5-2 ms per op: gcbench 4 W2 1.671 ms, gpuprobe jobs 178-180 1.69-1.98 ms and 230-250 MB/s, RTT +copy +2.0 ms, RTT readback xfer 1.50 ms** (Fast-set ops use PPF waits or submit->idle in a fresh state. The slow set comes from C3D_Sync* / FrameSplit waits or a long-running GPU session) [estimate; 2026-10-16 (analysis)] latest.txt L135-136, L214-219, L224-229; gcbench4 L492-494; gcbench4b L58-62; gcbench6 L60; docs/research/TIMING_MODEL_PLAN.md L133, L104-105; core/host3ds/gpuprobe.c L421-446 *Inference: the DMA engine runs at about 0.8-0.9 GB/s, and the 1.5-2 ms figures look like a synchronisation or wait artefact (lost or late PPF wake, frame split), not transfer time. Not proven: needs one probe timing both sync methods back to back Informs: Whether EFB copies and XFB present are cheap (about 0.3-0.5 ms) or cost about 2 ms each. A large swing in the GX frame budget*
- TP draw count and estimated citro3d submission cost: **~450 GPU draws/s, ~85 per frame; ~11 register commands per citro3d draw -> ~1 ms per frame** (TP) [estimate; 2026-10-11] docs/research/PICA200.md:22-24 *Draw count presumably measured; ms cost inferred [I]. With measured 3.25 us CPU/draw, 85 draws = ~0.28 ms. Informs: Raw command lists save little; per-vertex math off CPU saves more*
- XF matrix memory vs PICA uniform space: **256 + 96 + 256 floats of XF matrices vs 96 vec4 uniforms** (derived from specs) [estimate; 2026-10-10] docs/ARCHITECTURE.md:415; docs/GPU_BACKEND_PLAN.md:51-52 *Arithmetic, not a measurement. Informs: Upload only the matrices each draw references (adopted in native converted shapes)*
- Display transfer burst limit to FCRAM: **bursts <=128 bytes; burst setting 3 gives black screen** [unknown; 2026-10-11] docs/research/PICA200.md:402
- EFB copy rate: **~32 EFB copies/s** [unknown; 2026-10-11] docs/HANDOFF.md L564 *source scene/build not stated*
- PCSX ReARMed on New 3DS: software renderer vs GPU: **Unai software renderer 10-20 fps faster in many games** (libretro blog) [unknown; 2026-10-10] docs/ARCHITECTURE.md:444-445 *Third-party, a different console. A counterpoint to 'GPU always wins' on this host. Informs: GPU vs CPU rendering split*
- picasso IFC else-block length limit: **an else block longer than 255 instructions is silently truncated (the IFC holds it in 8 bits)** (picasso shader assembler) [unknown; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:188-189 *Toolchain or hardware constraint. How it was found is not stated. Informs: Shader structure (callc instead of a long ifc)*
- TP render rate: **TP renders at 30 fps (every other field)** (game fact) [unknown; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:119 *Informs: Frame skip (C9) halves GX work*

### Vertex work (106)

- 1 MB job ring (9388639, relay 54-55): **title 17.17 vs 17.31%; cutscene 16.15 vs 17.19%; build slower than f56d430; core 0 waited 13-17%; splits 64 -> 49-55/s; gxsync+gxpres 2 -> 7-12%** [device; 2026-10-12] docs/HANDOFF.md L670-674 *Informs: ring back to 256 KB*
- A/B compact vertex-worker jobs --vin (job 190, vin 0 vs 1): **Cutscene 30-300 s median 21.68 vs 22.02; later 300-475 s 12.71 vs 12.87. valloc 23.4 -> 20.0% (median). KB kept per job ~20 -> ~11, jobs in flight 3.4 -> 5.6, waits for ring space 520-770/s unchanged.** (tp850, 600 s, A/B 20 s) [device; 2026-10-15, job 190] relay_190.csv perf rows split on column 'vin'; '# vworker 10s' lines *Informs: Neutral, left off. Each job still reserves the full ~21 KB before trimming.*
- Attribute count: 2 / 3 / 4 attributes: **gcbench 4: 3 attr (24 B) 6.0, 4 attr (28 B) 3.64 Mtri/s; 4b: 2 attr 6.69, 3 attr 6.67, 4 attr 3.71** (gcbench 4 / 4b) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:10,33; docs/research/GPU_GUIDANCE.md:16-20 *Informs: Pack vertex data into <=3 attributes (~45% loss per extra)*
- C5: share of TP vertices with one shared index for all attributes: **0.6-0.9% of indexed vertices; 4.5-8.9% of draws (small ones) on every vertex** (2DS job 127, # shared index 10s) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:66-68 *Informs: Shared-index fast path rejected for TP*
- Compact vertex-worker jobs (--vin, job 190): **kept per job 20 -> 11 KB; jobs in flight 3.4 -> 5.6; valloc 11.8 -> 10.0%; ring-space waits ~650-700/s unchanged (each job still reserves ~22 KB max); medians cutscene 21.75 / 21.91, later 12.00 / 11.97; core 0 median GX wait 1.1-1.4% (job 184's 11% mean from a few heavy scenes)** [device; 2026-10-15, c93ac0c] docs/HANDOFF.md L1154-1159 *Informs: off; GX side not core 0's limit most of the time*
- Compact vertex-worker jobs (--vin, job 190): **TP VIn 52 vs 108 bytes; job memory ~145 -> ~90 bytes per vertex; per job 20 -> 11 KB, jobs in flight 3.4 -> 5.6, valloc 11.8 -> 10.0%, ring waits ~650-700/s unchanged; medians cutscene 21.75 / 21.91, later 12.00 / 11.97 (neutral). Core 0's median GX wait 1.1-1.4% (job 184's 11% mean comes from a few heavy scenes)** [device; 2026-10-15] git c93ac0c body; docs/HANDOFF.md ~l.1152 *Informs: GX side is not core 0's limit most of the time*
- Compiled vertex path (vfast.c) + draw batching: **Title stayed 3.1%; batching helped, vertex time did not move** (TP title scene) [device; 2026-10-10 late] memory/tp3ds-gc-wii-on-2ds.md:83-84 *Informs: Vertex cost is not per-draw overhead alone*
- Conflict: '4th attribute costs ~45%' vs the 4-attribute 36-byte vertex: **VtxN, 4 loaders (float3 normal, 36B): 6.01 Mtri/s (section 2 'vs simple'). VtxP, 4 loaders (float1 idx, 28B), same use_simple shader: 3.64 / 3.71 Mtri/s** (Both on the same device and build family. Neither shader reads v3) [device; 2026-10-10/11] bench/results/latest.txt L189; gcbench4 L338, L454; gcbench4b L18; bench/src/gpu.c L254-258, gpu_w2.inc L7, L77-81 *The slowdown is not the attribute count. It is specific to the float1 4th attribute (component count or layout). GPU_GUIDANCE rule 2 and the C6 risk line ('4th attribute costs ~45%') are over-generalised Informs: Vertex format design for the PICA backend (whether extra attributes such as normals or texcoords are affordable)*
- Cost per vertex: dlcache hit vs plain gather: **~1.4 us/vertex (hit) vs ~1.35 us/vertex (gather); no speed gain (jobs 111-119)** (2DS job 119) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:22-24,50 *Both RAM-bound: 6 MB record ring never stays in the 2 MB L2. Informs: CPU-side cache rejected; only skipping CPU work (C1) can cash in*
- CPU display-list cache: **Hits cost as much as gathers (RAM latency)** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:18 *Informs: Rejected*
- CPU display-list vertex cache, first build (116-byte records, 2.5 MB ring): **worker hit rate 0.1-1.5 % (ring overwrote every match); speed 21.25 -> 15.18 % (-29 %); core0 23.13 -> 23.65** (372 s) [device; job 111] core/out/relay_111.csv (header from 113), ab_split.py --col dlcache *Informs: Ring sizing*
- CPU lighting / decode / position costs (eb867fd): **lighting 17.8% (~600 cycles per light evaluation); attribute gather 3.9%; position + texgen ~9%** [device; 2026-10-10] docs/HANDOFF.md L379-381
- CPU vertex conversion (100k GC vertices): **to float 24B: 7.32 Mvert/s, 13.67 ms (best 13.59, flush 2.33), about 110 cycles per vertex. To s16 16B: 21.51 Mvert/s, 4.65 ms (best 4.64, flush 3.35)** (gcbench 3; gcbench 4 7.34 / 21.63) [device; 2026-10-10] bench/results/latest.txt L232-238 *'100k verts/f = 16 ms = 48% of frame' uses an assumed GC load Informs: Keep GC integer formats (2.9x cheaper) and let the shader scale. This drives the vertex worker's design*
- CPU vertex conversion rate: **BE GC vertices to floats 7.3 Mvert/s (13.7 ms per 100k); to 16-bit ints 21.5 Mvert/s (4.6 ms per 100k)** (2DS gcbench run 3 (synthetic)) [device; 2026-10-10] docs/FINDINGS.md:203-207 *Synthetic; TP's measured gather is ~1.35 us/vertex (~0.74 Mvert/s), ~10x slower, being RAM-latency bound. Informs: Keep integer formats; gather dominated by memory*
- CPU vertex cost before the GPU moves: **7.1 us per vertex (title scene); position + texture coordinates ~14%, 2.2 us/vertex; lighting 17.8% of 2DS time, ~600 cycles per light evaluation (VFP11 dependent op 8 cycles, divide / sqrt 19)** [device; 2026-10-10] git 4f5b69e, 4e491c4, 3979659 bodies *Informs: Moving lighting and transform to the PICA*
- CPU vertex gathering on the per-frame GX path, verdict: **~44,000 GameCube-indexed vertices per frame gathered on the CPU; 'Tuning this path will not reach 30 fps'** (derived from runs 225-240) [device; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:158-162 *Informs: Convert models once at load into PICA vertex buffers; draw with per-frame matrices only*
- Deferred gather to core-3 worker (6673eb2): **14.2% (slower than 14.8%); vdec on core 2 20% -> 2%, but GX thread waited for worker 18% (vwait); ~445 batch draws/s each a join; worker 35% -> 51% busy; core 0 waited 26%** [device; 2026-10-11, 6673eb2] docs/HANDOFF.md L206-209 *Informs: rejected (kVwGather off): moving work between cores does not help while every flush is a join*
- Display lists in TP: **99.4-99.8 % of draws inside display lists; 1.3-3.7k list calls/s; 124-387 new list addresses/s; sampled calls with the same bytes 85-97 %, same bytes and array bases 78-92 %** (tp850) [device; job 107 (2026-10-13)] '# dlist 10s' lines, relay_107.csv (and 111-124) *Informs: TP replays identical vertex data (basis for C1 GPU-resident vertices)*
- Display-list cache with VIx split, prefetch, final variant: **116: speed 20.93 -> 19.47 (-7.0 %), later 11.44 -> 11.24 (-1.7 %); 118 (prefetch): 21.02 -> 19.28 (-8.3 %), later 11.43 -> 11.24; 119: 21.04 -> 19.35 (-8.0 %); worker hit ~84-85 %, 6.3-7.5k evictions/s, no stale entries** (300-400 s each) [device; jobs 116-119] core/out/relay_116/118/119.csv, ab_split.py --col dlcache *Hit cost ~1.4 us/vertex vs gather ~1.35 us is from REDESIGN_CANDIDATES (not in the logs) Informs: CPU-side vertex cache rejected; a 6 MB ring cannot stay in the 2 MB L2*
- Display-list cache, compact records, 6 MB 2-way: **worker hit 59-61 %, GX-thread hit 64-65 %; speed 21.35 -> 19.00 % (-11 %)** (199 s) [device; job 112] core/out/relay_112.csv (header from 113), ab_split.py --col dlcache
- Display-list cache, missed segments gathered in runs: **worker hit 83.5-84.2 % (47-52k hits/s, 225-254k vertices/s), GX-thread hit 67-68 %; speed 21.42 -> 19.73 % (-7.9 %); core 0 GX wait 9.4 -> 13.2 %** (265 s) [device; job 113] core/out/relay_113.csv, ab_split.py / gx_split.py --col dlcache *Jobs 114/115 (variants): 21.27 -> 19.31 (-9.2 %), 20.84 -> 19.12 (-8.3 %); 2.7-4.6 pages checked per lookup*
- Display-list replay share (D1): **Worker spends 58-62% gathering; ~80% of TP's list calls replay identical bytes over the same arrays (job 107)** [device; 2026-10-13] git 388e9a2 body, 722b643 subject *Informs: GPU-resident vertex data (C1); display-list cache*
- Display-list share and replay rate: **99.6% of TP draws from display lists; ~85% of list calls replay the same bytes over the same arrays; worker spent 58-62% of its time gathering** (TP) [device; job 107] core/src/gx/vfast.c:583-585 *Informs: Display-list vertex cache (DL_CACHE_PLAN)*
- Display-list vertex cache (C3) across jobs 111-116: **Job 111: worker cache ~41k lookups/s, 0.7% hit (GX thread 55%). Job 112: hit 59-61%, speed 16.4% on vs 18.5% off. Job 113: hit 84% (250k vertices/s), 19.30 vs 20.47. Job 115: hits 33-35% of worker, ~1.5 us/vertex (as much as a gather). Job 116: hits 32-36%, ~7 us per hit segment, 6 MB ring > 2 MB L2; off 21.34 +-0.31 vs on 20.13 +-0.19** [device; 2026-10-13] git 35ac5db, 952017e, 52a079e, 9ecfeeb, ec789a0 bodies *User mode cannot enable the L2 prefetcher. Informs: CPU-side caching of gathered vertices loses; the data must live on the GPU (C1)*
- Display-list vertex cache A/B (jobs 111-116): **111 (116-byte records, 2.5 MB ring): 0% hits, 18.77 off / 14.74 on; 112 (compact, 6 MB, 2-way): 60-70%, 20.36 / 18.74; 113 (runs): 84%, 20.47 / 19.30; 116 (VIx split): 85%, 21.34 / 20.13; hits took 32-36% of worker (~7 us per hit segment, ring > L2, RAM latency); PC VCACHE_CHECK 12.36M hits 0 diffs, 0 stale/s** [device; 2026-10-13] docs/HANDOFF.md L925-938 *Informs: rejected (job 119: hits cost as much as gathers)*
- Display-list vertex cache hit rate and exactness: **84-85% of lookups hit; no entry ever invalidated by a page stamp; PC checker 0 differences over 12.36 M hits** (2DS jobs 113-119 (--dlcache on); PC VCACHE_CHECK) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:16-17 *Informs: Repetition is real: evidence for GPU-resident vertices (C1)*
- DL cache hit rate vs gather time: **84% of lookups hit; the gather phase did not shrink** (TP) [device; job 113] core/src/gx/vfast.c:598-599 *Negative. The DL cache defaults to off in runs (main.c RunOpts --dlcache off; g_vcacheOn = g_opt.dlcache at main.c:768). Informs: DL cache left off*
- DL cache hit-segment cost without prefetch: **~7 us per hit segment, as much as gathering it** (6 MB ring larger than the L2) [device; job 116] core/src/gx/vfast.c:800-803 *Negative result. Informs: Lookups run 32 segments ahead with prefetch*
- DL cache hits with VIn lines shared across cores: **hits 33-35% of the worker's time, ~1.5 us per vertex** (TP, deferred jobs) [device; job 115] core/src/gx/vfast.c:285-288 *False sharing between cores 2 and 3. Informs: Matrix indices moved to their own array (VIx)*
- DL cache v1: whole-VIn records: **116-byte VIns filled a 2.5 MB ring in under a frame; hit rate 0.7%** (TP) [device; job 111] core/src/gx/vfast.c:711-712 *Negative result. Informs: Compact cached records*
- DL cache, per-segment gathering: **hit rate 60% and still slower (lost the 4-ahead prefetch)** (TP; segments average ~5 vertices) [device; job 112] core/src/gx/vfast.c:791-792 *Negative result. Informs: Gather missed segments in runs*
- First vertex worker on core 3: **8.83% on vs 8.62% off. Writing culled triangles as degenerate ones tripled GPU frame time (87 vs 30 ms); a 4 MB job ring did not stay in L2. After fixes: 8.3% -> 9.9%** [device; 2026-10-11] git ab635d0, 88de143 bodies *Informs: Ring must fit L2; cull before the GPU*
- Full screen as 32 / 16 / 8 / 4-pixel tiles: **0.72 / 0.85 / 1.20 / 1.82 ms; 50.6 ns per vertex at 4 px** [device; job 181] docs/research/TIMING_MODEL_PLAN.md:168 *Informs: Small triangles vertex-bound; setup cheap*
- G1 shader length +16 / +64 / +128 MAD: **no change, 6.0 Mtri/s at 60k triangles** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:8; docs/research/GPU_GUIDANCE.md:22-23 *Contradicted by gpuprobe job 181 (+32/+64 instructions doubles/quadruples ns per vertex). Unresolved; G1 may have been bound elsewhere.*
- G10 s16 vs float attributes: **6.02 vs 6.01 Mtri/s** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:15 *gpuprobe job 178: s16 at 16 B 56.9 vs float 49.7 ns per vertex (~14% slower) — mild conflict. Informs: Feed GX integer arrays directly, scale in shader*
- G17 post-vertex cache: indexed grid in order vs shuffled: **10.05 vs 3.73 Mtri/s** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:50; docs/research/GPU_GUIDANCE.md:100-101 *Informs: Keep GX index order*
- G17: post-vertex cache, indexed grid in order vs shuffled: **ordered 5.97 ms (10.05 Mtri/s); shuffled 16.07 ms (3.73 Mtri/s), 2.7x slower and slower than non-indexed (6.0)** (gcbench 5, 20k tris x3) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L59-63 *Informs: Converting GC lists to indexed draws helps only with cache-friendly ordering. A bad order loses to plain arrays*
- G1: vertex shader padded with +16/+64/+128 dependent MADs: **base 9.98 ms (6.01 Mtri/s); pad16 9.97 (6.02); pad64 9.99 (6.01); pad128 9.97 (6.02). Printed: 0.00 ns per vertex per extra instruction** (gcbench 4 W2, 24-byte vertices, 3 attributes, non-indexed 60k) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L435-446; shaders bench/src/gpu_pad16.v.pica etc. *Raw result, but see the plausibility check below. GPU_GUIDANCE rule 3 and REDESIGN_CANDIDATES C6 cite it as 'shader length free' Informs: Whether long PICA shaders (texgen, lighting, palette) are free*
- G2 matrix palette (MOVA + 3xDP4) on 4-attribute 28-byte vertex; 1/8/28 matrices: **3.34 vs 3.64 Mtri/s without (-9%); matrix count no effect** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:9 *Superseded by 4b: index in position.w makes it free.*
- G2: matrix palette shader (MOVA + 3xDP4), 4 attributes, 28-byte vertices: **vs pal with 1, 8 or 28 matrices: 17.97 ms, 3.34 Mtri/s each; same vertex format without palette (vs base 28B): 16.46 ms, 3.64 Mtri/s (-8%)** (gcbench 4 W2, idx as a 4th float1 attribute) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L447-454 *Informs: Matrix count does not matter, so the palette is viable*
- G2b: attribute count vs packing (4b): **attr2 16B 8.97 ms (6.69 Mtri/s); attr3 24B 8.99 (6.67); attr4 28B 16.19 (3.71); pal 4attr 28m 16.66 (3.60); pal3 (index packed in position.w, 3 attributes, 28B) 8.97 (6.69)** (gcbench 4b mode f, 60k tris non-indexed) [device; 2026-10-11] bench/results/gcbench4b_2026-10-11.txt L12-22 *The verdict that the palette packed in position.w is free stands Informs: Pack the palette index into position.w (GPU_GUIDANCE rule 1)*
- G6 strips vs triangle list (same 20k triangles): **11.96 vs 5.0 Mtri/s (2.4x)** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:13; docs/research/GPU_GUIDANCE.md:42-43 *Informs: Pass GX strips/fans through with RESTART_PRIMITIVE*
- G6: triangle strips vs list, same 20k triangles: **strips 100x200: 1.67 ms, 11.96 Mtri/s; list 20k: 3.99 ms, 5.01 Mtri/s (2.4x; about 2.8x with frame overhead removed)** (gcbench 4 W2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L471-474 *Informs: Pass GC strips and fans through as PICA strips and fans*
- G9 / G10: VRAM vs FCRAM vertex buffers; s16 vs float vertices: **vertices in VRAM 9.98 ms (6.01 Mtri/s) = FCRAM. s16 16B 9.97 ms (6.02) = float 24B (6.01)** (gcbench 4 W2, 60k tris) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L475-478 *Timingprobe job 178: s16 at 16B 56.9 ns per vertex vs float3 49.7 (s16 14% slower there) Informs: Keep vertex buffers in linear FCRAM. Feed GC integer arrays directly and skip CPU float conversion*
- G9 vertex buffers in VRAM vs FCRAM: **6.01 vs 6.01 Mtri/s** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:14 *Informs: Vertex arenas can stay in linear FCRAM*
- GPU position transform with a matrix palette: **First A/B: 11.3% GPU vs 12.7% CPU (C3D_FrameBegin waited twice as long). 24-bit float precision: terrain up to 1.48 texels off, 0.114 with grid-origin rebasing** [device; 2026-10-11] git 29a22f7, 24e4585 bodies *Informs: Precision handling for GPU transform*
- GPU vertex rate, float3 at 12/16/24-byte strides; s16 at 16: **49.7 / 49.7 / 49.8 / 56.9 ns per vertex (13.3 GPU clocks)** (gpuprobe) [device; job 178] docs/research/TIMING_MODEL_PLAN.md:106,111-114 *Job 178 positions at clip z +0.5, every triangle clipped; vertex/draw cases went through the clipping path. Job 181 +0-instruction shader repeat gave 49.9 ns, consistent. Informs: Vertex stage, not fetch, sets the rate*
- hw.txt synthetic vertex cost: **1.1 us unlit, +0.3 us per light** [device; 2026-10-10/11] docs/HANDOFF.md L352
- Index fetch and vertex reuse: **DrawArrays 9.98 ms (6.01 Mtri/s); u16 elements 1:1 9.99 (6.01, index fetch free); shared-grid elements 5.98 ms (10.03 Mtri/s, 1.67x)** (gcbench 3/4, same 60k tris) [device; 2026-10-10] bench/results/latest.txt L239-245 *Informs: Indexed drawing with vertex sharing helps. Index buffers cost nothing*
- Indexed draws, CPU vs GPU transform: **CPU transform + early splits 13.77% vs baseline 12.20%, GPU transform ~12%; indexed draws with GPU transform became the default at 14.56%; core 0 then 44% stalled on instructions** [device; 2026-10-11] git 862fe63, 3bffa35 bodies *Informs: Core 0 became the limit again*
- Indexed-attribute prefetch (e30b707, relay 81-82): **cutscene 21.00% off / 20.91% on (+-0.15/0.26); title 21.21 / 21.47 (+-0.15/0.16); worker busy 62.2 -> 60.5%; core 0 98% busy (93% compiled code), GX thread 78%** [device; 2026-10-13] docs/HANDOFF.md L789-793 *Informs: kept on, no measurable gain*
- Job ring size: **1 MB 18.9% vs 256 KB 20.8-20.9% (same run); separate runs 96 KB 21.48%, 128 KB 21.41%, 256 KB 21.18%** [device; 2026-10-12] git e30b707, 988911a bodies *Separate runs differ by less than run-to-run noise (~5%). Informs: 128 KB default; bigger rings lengthen core 0's drain waits*
- Job ring size A/B, title (relay 76-77): **1 MB 18.9% vs 256 KB 20.8-20.9% in same runs; GX-thread wait 19% vs 3-7%** [device; 2026-10-13] docs/HANDOFF.md L783-786 *Informs: deep rings lose (each sync waits whole queue)*
- Job ring size, cutscene (relay 78-80, separate runs): **96 KB 21.48%, 128 KB 21.41%, 256 KB 21.18% (+-0.18-0.19); worker wait 19.7 -> 22.1 -> 23.9% as ring shrinks** [device; 2026-10-13] docs/HANDOFF.md L786-788 *Informs: 128 KB chosen*
- Native renderer run 225: DRAW_INDEXED expanded to triangle lists: **~150 draws / ~79,000 vertices per frame; 4.6 fps; core 2 in vertex work** (title demo steady stretch; perf.csv gx column) [device; job 225, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:150-152 *Informs: Indexed drawing*
- Native renderer run 240: job ring 4 MB (VW_MEM), no inline draws: **5.1-6.9 fps; core 2 waiting ~420 ms/s on the vertex worker** (title demo) [device; job 240, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:155 *No fps gain over 229-234: the bottleneck moved to the core-3 worker. Informs: Vertex worker throughput is the next wall*
- Palette with index packed into position.w (3 attributes): **6.69 Mtri/s vs 4 attributes 3.60: free** (gcbench 4b, bench/src/gpu_pal3.v.pica) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:34; docs/research/GPU_GUIDANCE.md:11-15 *Informs: GPU-transform (vfast) design*
- Prefetching indexed attributes (pld), in-run A/B: **Cutscene: 21.03 → 21.56%. Title: 21.26 → 21.65%. The worker's busy share fell about 2 points.** (20 s phases) [device; relay jobs 81-82, 2026-10-13] core/out/relay_81.csv, relay_82.csv *Confounded: draws/s were 65.2k with pld off and 60.1k with it on. HANDOFF reports 21.00/20.91, no change. Job 82's time breakdown was off from 12 s. Informs: Kept on*
- Reference vertex path cost; TP vertex load: **~7.5 us per vertex on the ARM11; TP title scene ~22,000 vertices per field** (reference path (decode, transform, to_screen, PICA fold)) [device] core/src/gx/vfast.c:1-3 *An old baseline that vfast replaced. Informs: vfast fast path*
- Ring 128 KB against 512 KB, changed live: **128 KB: 20.55%, core 0 23.89%, gxwait 14.1%. 512 KB: 19.06%, core 0 27.47%, gxwait 30.5%. vwait 26.6 vs 20.9%.** (tp850 cutscene) [device; relay job 99, 2026-10-13] core/out/relay_99.csv ('# cmd set vwmem' at 68 s and 99.6 s) *Short: 26 rows at 512 KB Informs: A bigger ring doubles core 0's drain wait*
- Ring size in separate cutscene runs: **96 KB: 21.74%. 128 KB: 21.68%. 256 KB: 21.46% (±0.13 each). vwait: 24.7 / 22.9 / 20.5%.** (tp850, 200 s) [device; relay jobs 78-80, 2026-10-13] core/out/relay_78.csv (128), relay_79.csv (256), relay_80.csv (96) *Differences are about 2 SE Informs: 128 KB chosen*
- Shared-index fast path: **<1% of TP's vertices** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:19 *Informs: Rejected*
- Shared-index fast path (C5): **Under 1% of TP's indexed vertices share one index across attributes** [device; 2026-10-13] git eaa3396 subject *Informs: Rejected for TP*
- Shared-index fast path eligibility: **0.3-1.2 % of indexed vertices give all indexed attributes one index; 3.3-18 % of indexed draws qualify on every vertex; 0 % in the later stretch** (sampled 1 draw in 32) [device; jobs 127-157] '# shared index 10s' lines, relay_127-157.csv *Informs: Rejected for TP (C5)*
- Shared-index potential (single PICA index) in TP: **0.85% of indexed vertices give all indexed attributes one index; 9.5% of indexed draws do on every vertex; 0% have a single indexed attribute** (1 draw in 32 sampled) [device; 2026-10-15, job 184] relay_184.csv averaged '# shared index 10s (1 draw in 32)' *Informs: Passing GameCube multi-index vertices straight to the PICA without expansion is rarely possible: a conversion/expansion step is needed*
- Splitting matrix indices out of VIn (VIx, 9ecfeeb): **cache-off speed 20.0-20.8% (jobs 113, 115) -> 21.3-21.4% (jobs 116, 118)** (2DS TP cutscene) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:59-60 *Cross-job comparison, not a same-run A/B. Informs: Worker memory traffic per vertex matters (C4)*
- Title scene draw count and per-draw cost: **~3,200 draws of ~5 vertices per field; ~57 us per draw** (TP title scene, early PICA backend) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:72-73 *Superseded by batching and later draw-path work Informs: Per-draw overhead decides GX speed -> batching + shader transform*
- Title scene time split after 16 MB JIT: **CPU vertices 30%, guest code 54%** (TP title scene at 3%) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:75 *Superseded Informs: Float JIT and GPU vertex work*
- Title-scene split with the CPU vertex path: **3% speed: CPU vertex path 30%, guest 54%; ~4,400 draws of ~5 vertices per field (2DS); PC ~3,200 draws/field** [device; 2026-10-10] git cf15336, 378a47f bodies *Informs: Vertex work was a third of the time*
- Triangle throughput, non-indexed list (24-byte vertices, 3 attributes): **20k tris: 3.99 ms per frame, 5.02 Mtri/s (flat and textured). 60k tris: 9.98 ms, 6.01-6.02 Mtri/s** (gcbench 3/4, ~2.7 px triangles) [device; 2026-10-10] bench/results/latest.txt L180-184 *The 'GC 30-60k tris/frame' budget printed beside it is an assumption Informs: PICA vertex rate is far above TP's assumed 1-2 Mtri/s, so the GPU is not the limit*
- Trimming unused vertex-job reservations (vtrim), in-run A/B: **20.37 → 20.78% (±0.4-0.5). valloc 23.7 → 21.8%. Per job: 22.1 KB reserved, 19.5 KB kept. About 800 waits for ring space/s with 3.9 jobs in flight.** (tp850 cutscene) [device; relay job 98, 2026-10-13] core/out/relay_98.csv; '# vworker 10s' in relay_97-99 *Noisy run Informs: Ring space is not the lever. The worker's cost per job is.*
- Vertex job ring 1 MB against 256 KB, in-run A/B, first test: **Title: 17.08% vs 17.46% (core 0 20.55 vs 20.21%). Cutscene: 16.48% vs 17.27% (core 0 19.78 vs 20.23%). gxwait 17.7 vs 13.7%.** (20 s phases) [device; relay jobs 54-55, 2026-10-12] core/out/relay_54.csv, relay_55.csv *Informs: A deeper ring does not help*
- Vertex job ring 1 MB against 256 KB, in-run A/B, second test: **Cutscene: 19.00% vs 21.24% (core 0 own 23.54 vs 21.87%; gxwait 19.5 vs 2.7%). Title: 19.02% vs 20.93% (core 0 23.83 vs 22.08%; gxwait 19.6 vs 4.3%).** (20 s phases) [device; relay jobs 76-77, 2026-10-13] core/out/relay_76.csv, relay_77.csv *Informs: Deep rings make every sync wait for the whole queue. The ring stays small.*
- Vertex ring occupancy: **2,457 jobs/s, 20.2 KB reserved and 18.1 KB kept per job, 733 waits for ring space/s with 3.5 jobs in flight (128 KB ring); jobs 125-127: 2.0-2.6k jobs/s, 21.9-23.7 KB reserved, 512-748 waits/s** (cutscene) [device; jobs 125-157] '# vworker 10s' lines, relay_125/127/157.csv *Informs: Ring space waits come from per-job reservation size*
- Vertex shader +0 / +32 / +64 instructions: **49.9 / 99.3 / 188.7 ns per vertex** (gpuprobe) [device; job 181 (repeated 183)] docs/research/TIMING_MODEL_PLAN.md:163 *Contradicts gcbench 4 G1 (+16/+64/+128 MAD: no change at 6.0 Mtri/s) and GPU_GUIDANCE rule 3 (shader length free). Unresolved; the later probe says length costs. Informs: Generated GX vertex shaders (lighting/texgen) must be short*
- Vertex shader: minimal vs GC-like 4-light T&L: **vs simple 60k 9.98 ms (6.01 Mtri/s); vs light 60k 13.97 ms (4.29 Mtri/s), 1.40x** (36-byte vertices with normal (4 attribute loaders; the simple shader ignores v3)) [device; 2026-10-10] bench/results/latest.txt L188-192; method bench/src/gpu.c L551-574 *Light adds about 33 instructions and 22 ns per vertex. This contradicts the G1 'shader length is free' result Informs: Cost of moving lighting, texgen and normal transforms into the PICA shader (redesign C6)*
- Vertex worker (core 3) phase breakdown: **62% gathering attributes from guest RAM, 17% positions/normals, 11% texture coordinates, 9% copy/cull/triangles** (2DS, # vworker phases) [device; job 101 (and job 115)] docs/research/PERF_LEVERS.md:14-15; REDESIGN_CANDIDATES.md:83-84 (job 115 repeats 17%/11%); DL_CACHE_PLAN.md:10 *Informs: Gather is memory-latency bound; levers C1, C4, C6*
- Vertex worker busy share: **~60% busy (54% in job 128 off arm)** (2DS TP cutscene) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:33,125 *Informs: Headroom on core 3*
- Vertex worker gathering shader-lit draws (rejected): **14.2% vs 14.8%: gather left the GX thread (vdec 20% -> 2%) but batch draws then waited for the worker (vwait 18%; worker 35% -> 51% busy). Core 0 had been waiting for GX 22%; GPU ~20 ms of a ~190 ms frame** [device; 2026-10-11] git 6673eb2, a1cfba6, 486c32a bodies *Informs: GPU has headroom; vertex work belongs on the GPU*
- Vertex worker on core 3, in-run A/B in five builds: **Speed off/on: 8.73/8.70, 8.38/10.05, 9.04/11.69, 10.27/12.52, 10.51/12.71%. The worker was 24-29% busy. Core 0's wait on GX fell from 45-51% to 28-45%.** (Title from boot, heavy rows) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv runs 3-7 (= perf_2026-10-11_vw1, vw2, e4fa, 29fc, bb18) *The first build showed no gain Informs: Per-vertex work belongs off the GX thread: +20-29% from the second build on*
- Vertex worker per-job cost (3605dfd era): **~220 us per merged job on core 3; 116-byte VIn record mostly unused texture slots; GX thread waits for worker ~19%; core 0 waits core 2 2-4%** [device; 2026-10-12] docs/HANDOFF.md L735-739
- Vertex worker per-vertex cost and throughput: **~2.2 us (~1,750 cycles) per vertex; 60% busy at ~275k vertices/s; GX thread waited for it ~20%; ~3.6 indexed attributes per vertex from scattered guest RAM; PLD 4 vertices ahead neutral (kept)** [device; 2026-10-12] git e30b707 body; 988911a body *Informs: Gather is memory-latency bound*
- Vertex worker phase split in the cutscene: **gather and colours 58-70 %, positions and normals 15-18 %, texture coordinates 11-12 %, rest (copy, cull, triangles) 4-12 % of the worker's time** [device; jobs 101-157] '# vworker phases 10s' lines, relay_101.csv and relay_157.csv *Later stretch 57/18.5/12/12; movie 4-8 % gather, 35-49 % rest Informs: The gather (random guest-RAM reads) is the worker's cost*
- Vertex worker time split (TP cutscene): **gather and colours 61-68%, positions and normals 16.5-18%, texture coordinates 11%, rest (copy, cull, triangles) 4-10%** (tp850) [device; 2026-10-15] relay_184.csv / relay_171.csv '# vworker phases 10s' (averaged over 26 reports: 64.2 / 17.5 / 11.3 / 7.0) *Informs: Gather/colour decode is the vertex worker's dominant cost (target for SIMD-less gather, GPU-side formats, caching)*
- Vertex-attribute count vs PICA vertex rate: **every attribute past three costs ~45% of the vertex rate** (gcbench) [device] core/src/gx/pica/pica.c:2510-2511 (GPU_GUIDANCE rule 2) *Informs: Feed only the inputs a draw uses*
- Vertex-job ring allocation wait (valloc) and ring size, live: **GX thread waits for ring space 22-25% (~3.8 jobs in flight); trimming reservations 23 -> 19.6 KB/job barely moved it; job 99: 512 KB 19.03% vs 128 KB 20.08% (core 0 drain wait 30% vs 14%)** [device; 2026-10-13] docs/HANDOFF.md L889-892
- Vertex-job ring wait after merging: **GX thread waited for space in 256 KB ring 17.6% (vwait) while core 3 47% busy; merged jobs ~30 KB** [device; 2026-10-12, f56d430] docs/HANDOFF.md L665-667
- Vertex-worker gather cost: **~2.2 us (~1,750 cycles) per vertex, mostly waiting for memory; ~3.6 indexed attributes per vertex** (TP, core 3) [device; commit bf169a0] core/src/gx/vfast.c:374-377 *The pld prefetch A/B (--ab pld) result is not in source. Informs: Prefetch indexed attributes 4 vertices ahead*
- Vertex-worker job ring in TP: **21.1 KB reserved and 18.7 KB kept per job; 612 waits for ring space/s with 3.4 jobs in flight** (default ring) [device; 2026-10-15, job 184] relay_184.csv averaged '# vworker 10s' *Informs: Ring sizing and reservation policy*
- vfast + draw batching (0b29e22) on title scene: **3.1% speed; GX 38% (vtx 29.3%, tex 0.5%, gpu 0.9%); guest+other 61%; 8,300 draws/s; vertex share stuck at 29% for ~27,000 vertices/field = ~5.7 us/vertex (~7.5 us/vertex quoted elsewhere); batching cut rest of GX 16% -> 9%** (inline GX) [device; 2026-10-10, 0b29e22] docs/HANDOFF.md L74-79, L88-89 *per-vertex cost is derived (share / vertex count), not directly timed Informs: per-vertex CPU cost did not fall with leaner code -> later found cost is per draw, not per vertex*
- Worker gathers CPU-transformed draws (kVwGather): **speed 14.2% vs 14.8% (worse); vwait 18%** (TP) [device; commit 6673eb2] core/src/gx/pica/pica.c:1027-1031 *Negative result. Every batch flush waits for the worker. Informs: kVwGather = false*
- Worker segment rate; DL cache slots: **worker gathers ~40k segments/s; 16,384 slots evicted 16k live entries/s** (TP) [device; jobs 111, 112] core/src/gx/pica/pica.c:1393-1394 *Informs: 6 MB ring, 32,768 slots*
- Worker writing culled triangles as degenerate (7f2033e): **worker on 8.83% vs off 8.62%; GPU time per frame 87 vs 30 ms (lighting shader still runs per vertex); GX-thread vertex time 51% vs 44% with 4 MB job ring** [device; 2026-10-11, 7f2033e] docs/HANDOFF.md L245-247 *Informs: worker appends only kept triangles; job ring 256 KB*
- Draw merge model (MERGE_CHECK): **96.4% of 4.67M draws join an open draw; 28 draws (140 vertices) per job; CPU-transform vertices bit-identical; GPU transform with merged origin max 0.70 texels on 39 of 12.3M vertices** [pc; 2026-10-12] docs/HANDOFF.md L657-662 *supersedes MERGE_STATS 76%*
- Draw-merge potential estimate (MERGE_STATS): **76% of draws could join previous draw's vertex job; 99.6% of draws are triangle strips** [pc; 2026-10-11] docs/HANDOFF.md L457-459 *superseded by MERGE_CHECK 96.4% (excluded per-vertex matrix-index draws)*
- GPU-transform origin grid: palette reuse and precision: **draws reusing previous palette entries: 86% (no origin), 81% (cells 4-8 eye distances), 70% (1-2), 40% (1/16); largest error 1.04, 0.114, 0.035, 0.028 EFB texels** (TP to field 1000, vfast_check.sh) [pc] core/src/gx/vfast.c:1228-1235 *Folding the position matrix into float coefficients lost up to 8x precision on far geometry (vfast.c:244-246, source unstated). Informs: Origin cells 4-8 eye distances*
- Palette entry reuse between consecutive TP draws: **81% with coarse-grid origin; 86% without origin; 40% with exact per-draw origin** (TP, vfast_check) [pc; 2026-10-11] docs/research/GPU_GUIDANCE.md:118-121 *Informs: Origin grid cell size (4-8 eye distances)*
- TP terrain error with world coordinates in float24 shader: **up to 1.04 EFB texels off; 906 of 4.7 M on-screen vertices over 0.25 texel; with per-draw origin folded on CPU: 0.114 texels max** (float24 model on PC, core/tools/vfast_check.sh, TP model coords ~40,000) [pc; 2026-10-11] docs/research/GPU_GUIDANCE.md:112-121 *Informs: Send positions relative to a coarse-grid origin*
- VIn record size; job ring bytes: **VIn 108 bytes after VIx split (8 texcoord slots = 64 bytes; TP uses 1-2); ~20 KB per job in the ring** (design/build facts) [pc; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:57; DL_CACHE_PLAN.md:47-49 *DL_CACHE_PLAN.md:47 says 116 bytes (before the VIx split): superseded by 108. Informs: C4 shrink VIn*
- Azahar: CPU vertex conversion: **64-vertex draws 2,329 ns/vertex (149 us/draw); 5-vertex draws 3,950 ns/vertex (19.75 us/draw); 0, 1 or 7 lights make no difference** (Azahar hwprobe) [azahar; 2026-10-15] azahar-sdmc/hw.txt:9-13 *Azahar CPU at 267 MHz without caches Informs: None for speed*
- Endianness of Aurora vertex arrays: **183,040 of 183,040 arrays declared little-endian** (title demo stream dump (gx_dump.py)) [azahar; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:133-135 *A content count, not a timing, so Azahar is adequate here. Informs: Little-endian vertex format variants in vfast.c/vertex.c*
- Derived plausibility check of G1 'shader length free': **pad128 at 18 Mvert/s would need about 2.3 G shader instructions per second. 3-4 vertex units at 268 MHz give at most 0.8-1.07 G/s. The vs light shader (+33 instructions) cost 1.40x on the same pipeline** (arithmetic on the raw rows plus the PICA200.md clock and unit count) [estimate; 2026-10-16] bench/results/gcbench4_2026-10-11.txt L437-446; latest.txt L189-191; docs/research/PICA200.md L75, L368, L390 *Strongly suggests the padded MADs were not executed or not on the critical path (dead r3 chain, or assembler or GPU behaviour). The G1 verdict is unproven and should not carry C6. Re-measure with pad results feeding an output Informs: Redesign C6 (texgen and normals in the shader) and GPU_GUIDANCE rule 3*
- Derived: PICA cost per vertex: **about 53-55 ns per vertex = 14-15 GPU clocks at 268 MHz (60k list: 9.98 ms / 180k vertices, 0.39 ms overhead removed). Strips: about 63 ns per vertex; list 20k: 60 ns** (derived from gcbench 3/4 rows) [estimate; 2026-10-16 (derived)] bench/results/latest.txt L183; gcbench4 L472-473 *Agrees with timingprobe job 178 (49.7 ns, 13.3 GPU clocks). Throughput is per vertex, so strips and the post-transform cache win by sending fewer vertices Informs: GPU-side vertex budget: about 18 Mvert/s*
- GX pipeline vertex throughput ceiling, emulator vs native mode: **~300-340k vertices/s in both: TP cutscene 316-330k/s with the GX thread at 81-84%; native title demo ~50.7k vertices/frame x 6.7 fps ≈ 340k/s with the GX thread at 100%** (current GX consumer on cores 2+3) [estimate; 2026-10-15] relay_184.csv verts_s; relay_229.csv-relay_238.csv gx column (derived) *Derived by multiplying per-frame counts by fps across different workloads. Order-of-magnitude only. Informs: The CPU-side vertex path (gather/convert) caps both modes far below the PICA's ~20M vertices/s. A redesign must move vertex conversion out of the per-frame path.*
- Converted-shapes path (native), budget and result: **8 MB linear-memory budget, LRU eviction; a shape dropped 3 times stays on the old path; 2 shapes per present hashed to catch missed flushes. No fps or ms result recorded** (pica.c native shapes, vfast_native_*, gxpass_native.v.pica) [unknown; 2026-10-15 night] docs/research/NATIVE_PORT_PLAN.md:164-185 *A gap: built but unmeasured in this doc. A later relay CSV or HANDOFF may hold the number. Informs: Whether native rendering reaches 30 fps*
- Light parsing per draw: **parsing 7-8 lights per draw cost 2-4k cycles** (vfast_prepare) [unknown] core/src/gx/vfast.c:513-514 *The platform is not stated. Informs: Skip reading lights when the shader lights*

### Textures (34)

- CPU readback of the EFB, 256x256: **~4 ms** (early measurement) [device; ~2026-10-10] docs/GPU_BACKEND_PLAN.md:48 *gcbench 6 (2026-10-11) later measured 256x256 copy-to-RAM at 1.9 ms total including CPU convert (GCWII.md:241). The method may differ; both are kept. Informs: EFB peeks and readbacks only on demand*
- DisplayTransfer format conversion to RGB565: **0.27 ms for 400x240** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:241 *Informs: GPU-side conversion instead of CPU*
- Fill rate on texture-cache-missing textures by format: **1024x1024 minified: RGBA8 17 Mpixel/s, RGB565 27, ETC1 117; anything hitting the texture cache 204** (2DS gpuprobe, job 180) [device; 2026-10-15] docs/research/REDESIGN_CANDIDATES.md:229-231 *Informs: C17 smaller texture formats + mipmaps*
- Fill with 1024x1024 texture minified ~2.5x, no mipmaps: ETC1 / RGB565 / RGBA8: **8.56 / 36.5 / 59.8 ns per pixel = 117 / 27 / 17 Mpx/s** [device; job 180] docs/research/TIMING_MODEL_PLAN.md:123-125,129-131 *Supersedes GPU_GUIDANCE rule 9 / gcbench 4 G12 (all formats 193 Mpx/s) for textures that miss the texture cache. Informs: Texture decode target format (ETC1/16-bit over RGBA8) and mipmaps*
- Fill with RGBA8 64x64 texture (magnified, fits texture cache): **4.91 ns/px, 204 Mpx/s** [device; job 180] docs/research/TIMING_MODEL_PLAN.md:122
- G12 fill by texture format (RGBA8, RGB565, RGBA4, L8, ETC1): **all 193 Mpx/s** (gcbench 4 (texture size not stated)) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:16; docs/research/GPU_GUIDANCE.md:44-45 *Superseded for cache-missing textures by jobs 180/181 (ETC1 117, RGB565 27, RGBA8 17 Mpx/s at 1024 minified).*
- G12: fill rate by texture format (VRAM, 1:1): **RGBA8 = RGB565 = RGBA4 = L8 = ETC1 = 193.4 Mpx/s** (gcbench 4 W2, texture likely cache-resident) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L479-489 *Superseded for real-game textures by timingprobe job 180: minified 1024x1024 without mips gives ETC1 117, RGB565 27, RGBA8 17 Mpx/s. Format matters whenever the texture cache misses. GPU_GUIDANCE rule 9 is too broad Informs: Texture decode target format, and whether to generate mipmaps*
- H1 EFB copy to RAM by size: **240x400: transfer 0.48 + invalidate 0.47 + CPU convert 1.98 = 2.9 ms; 256x256: 1.9 ms; 128x128: 0.65 ms; 64x64: 0.2 ms (no hang)** (bench/results/gcbench6_2026-10-11.txt) [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:241 *CPU conversion dominates. Compare GPU_BACKEND_PLAN's earlier ~4 ms per 256x256. Informs: EFB-to-RAM feasible for the 157 games; let DisplayTransfer convert*
- H4 texture hashing speed: **xxHash32 ~230 MB/s (64 KB in ~0.3 ms); CRC32 ~60 MB/s; 512-sample hash ~23 us per texture** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:244 *Native mode avoids hashing (Aurora ID + version keys). Informs: Texture-cache validation policy: xxHash32 or sampling, never CRC32*
- H4: texture hashing speed: **8 KB: CRC32 62 MB/s, xxHash32 281 MB/s, 512-sample hash 9.4 us. 64 KB: 58 / 232 MB/s, 22.3 us. 512 KB: 58 / 228 MB/s, 23.8 us** (gcbench 6, core 0) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L6-10 *Informs: Texture-cache invalidation: xxHash32 (64 KB in about 0.28 ms) or 512-sample hashing (about 23 us). Never CRC32*
- H5 GameCube texture decode speed: **RGB5A3->RGBA8 17 Mpx/s (3.9 ms per 256x256); CI8 35 Mpx/s (1.9 ms); I4->L4 32 Mpx/s (2.0 ms); CMPR->RGBA8 28 Mpx/s (2.3 ms)** (includes tiling) [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:245 *Informs: Decoded textures must be cached; RGB565/ETC1 targets (C17)*
- H5: GC texture decode to PICA tiled (256x256): **RGB5A3 -> RGBA8 17.0 Mpx/s (3.86 ms); CI8 -> RGBA8 35.3 (1.86 ms); I4 -> L4 32.5 (2.02 ms); CMPR -> RGBA8 28.0 (2.34 ms)** (gcbench 6, core 0) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L11-16 *CMPR decoded to RGBA8, not ETC1. Job 180 shows RGBA8 is the slowest format to sample when the texture cache misses Informs: Decoded textures must be cached, plus the choice of decode target format and decode placement (worker core)*
- RGBA8 256x256 (0.64 texel/pixel): **173 Mpx/s** [device; job 181] docs/research/TIMING_MODEL_PLAN.md:167
- RGBA8 at 0.5 / 1 / 2 / 2.56 texels per pixel: **178 / 114 / 30 / 17 Mpx/s** [device; job 181] docs/research/TIMING_MODEL_PLAN.md:166 *Informs: Mipmaps matter past 1 texel/pixel*
- Sampling FCRAM textures vs VRAM textures: **2.5-3.6x slower from FCRAM** (gcbench) [device; ~2026-10-10] docs/GPU_BACKEND_PLAN.md:38-39 *Informs: Texture cache residency policy (VRAM first, FCRAM fallback)*
- Screenshot BMP written on the GX thread: **stalled graphics for 2.8 s** (perf_shot) [device; 2026-10-11] core/src/gx/pica/pica.c:2583-2584 *An instrument side effect, now fixed. Informs: BMP written by a core 1 thread*
- Texture at 1 texel per pixel: ETC1 / RGB565 / RGBA8: **199 / 165 / 114 Mpx/s** [device; job 181] docs/research/TIMING_MODEL_PLAN.md:165 *Informs: ETC1 free, RGBA8 loses 45%*
- Texture copy bandwidth: **~240 MB/s** (2DS) [device; 2026-10-15] docs/research/REDESIGN_CANDIDATES.md:231 *Compare FINDINGS: GX copy 895 MB/s, 248 MB/s with dcache flush; upload ~130 MB/s per 256 KB. Informs: C17 upload cost*
- Texture copy linear->VRAM / VRAM->linear: **234 / 229 MB/s (230-250 MB/s across jobs 178-180)** (gpuprobe) [device; jobs 178-180] docs/research/TIMING_MODEL_PLAN.md:108,133 *Conflicts with gcbench 4b TextureCopy 256 KB in 0.43 ms (~600 MB/s, GPU_GUIDANCE:69). Unresolved. Informs: Texture upload path cost*
- Texture decode rate: **0-0.1 decodes/s in the cutscene, 0.02 later, 3.4/s in the movie section** (clean run) [device; job 157] texdec_s column, relay_157.csv *Informs: Texture decoding is not a steady-state cost in TP*
- Texture decodes in steady TP scenes: **texdec_s and texdec_kb are 0 in heavy rows of the cutscene and of the later stretch** (tp850) [device; 2026-10-12/13] core/out/relay_7, 83, 84, 97, 99 (texdec_s) *Texture decodes vary up to 10x between title runs (per HANDOFF), not covered here Informs: Texture conversion is not on the critical path in these scenes*
- Texture fetch from FCRAM vs VRAM: **texL (FCRAM) 1:1 29.88 ms, 77.1 Mpx/s (0.40x VRAM); texL 2:1 49.93 ms, 46.1 Mpx/s (0.28x); texV 2:1 13.92 ms, 165.6 Mpx/s** (gcbench 3/4, 2:1 = 2x2 texels per pixel) [device; 2026-10-10] bench/results/latest.txt L196-204 *Informs: Textures must live in VRAM (3 MB max). That forces a texture cache with eviction*
- Texture fill by texel density: RGBA8 at 0.5 / 1 / 2 / 2.56 texels per pixel; at 1 texel per pixel ETC1 / RGB565 / RGBA8; RGBA8 256x256: **178 / 115 / 30 / 17 Mpixel/s; 198 / 163 / 115 Mpixel/s; 256x256 172 Mpixel/s** (400x240 target) [device; 2026-10-15, job 183 (also 181)] relay_183.csv '# gpu fill tex rgba8 1024 span', '# gpu fill tex rgba8 256x256' *Informs: The texture cache gives way past one texel per pixel, so mipmaps matter. ETC1 is free at 1 tpp; RGBA8 loses 45%.*
- Texture fill cost, 1024x1024 minified ~2.5x without mipmaps: RGBA8 / RGB565 / ETC1: **59.8 ns (17 Mpixel/s) / 36.5 ns (27) / 8.55 ns (117)** (400x240 target, 16 layers) [device; 2026-10-15, jobs 179-183] relay_179.csv, relay_180.csv, relay_183.csv '# gpu fill tex ... minified' *Informs: Texture decode target format (ETC1/RGB565 over RGBA8) and generating mipmaps (C17)*
- Texture sampling from FCRAM vs VRAM: **2.5-3.6x slower from FCRAM** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:50 *Informs: Keep textures in VRAM (6 MB budget)*
- Texture sampling: linear FCRAM vs VRAM: **FCRAM textures 2.5-3.6x slower to sample** (gcbench) [device] core/src/gx/pica/pica.c:20 (docs/FINDINGS.md:194) *Textures still live in FCRAM (listed as not done yet). VRAM is only 6 MB. Informs: Texture cache in VRAM (open)*
- Texture upload 256x256 RGBA8 (three paths): **(a) linear + transfer 2.02 ms (130 MB/s): memcpy 0.56, flush 1.13, xfer 0.33. (b1) swizzle to FCRAM 2.00 ms (131 MB/s): swizzle 0.85, flush 1.15. (b2) swizzle + DMA to VRAM 2.03 ms: swizzle 0.87, flush 0.82, DMA 0.33. CPU swizzle matches GPU tiling** (gcbench 3; gcbench 4: b1 swizzle 1.03, flush 0.96; b2 1.05 / 0.64 / 0.33) [device; 2026-10-10] bench/results/latest.txt L213-221; gcbench4 L373-384 *The flush is over half (GSPGPU flush; the svc flush would be cheaper per M3). The 'budget 1 MB new tex/frame = 7.6 ms' line is an assumption Informs: Texture-cache upload path: CPU swizzle and flush dominate, while the DMA engine is about 0.33 ms per 256 KB*
- Texture upload cost: **2 ms per 256 KB RGBA8 texture (~130 MB/s), over half is the data-cache flush** (2DS citro3d) [device; 2026-10-10] docs/FINDINGS.md:195-196 *Informs: Smaller formats (C17); upload batching*
- Texture upload rate (FINDINGS): **~130 MB/s, more than half of it the cache flush** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:76; docs/research/PICA200.md:379 *Second-hand from FINDINGS.md.*
- TextureCopy 256 KB; clear 400x240 colour+depth: **0.43 ms (~600 MB/s); clear 0.17 ms** (gcbench 4b) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:37; docs/research/GPU_GUIDANCE.md:69-70 *gpuprobe jobs 178-180 texture copies 230-250 MB/s; conflict unresolved.*
- TEV plan cache check: **0 of 631,329 cache hits (TP 900-1020) and 0 of 4,414 (boot to 1500) differ from a fresh compile** [pc; 2026-10-14] git 56bd0cd body *Informs: TEV cache is exact*
- EFB as PICA render target, size: **640x528 RGBA8 ~1.35 MB per buffer, fits a 3 MB bank** (arithmetic) [estimate; 2026-10-10] docs/ARCHITECTURE.md:420; docs/GPU_BACKEND_PLAN.md:17 *Native mode renders at Dusklight's 608x448 (NATIVE_PORT_PLAN.md:130). Informs: Native-resolution EFB in VRAM*
- Fog LUT coverage with citro3d z-mapping (near 1, far 1000): **z 10-1000 covers only entries 0-12 of 128** [estimate; 2026-10-11] docs/research/PICA200.md:153-154 *Inferred; confirmed in shape by gcbench 4 G7.*
- Games broken by Dolphin's EFB-copies-to-texture-only: **~40% (approximate, forum); 157 of 1,891 settings files force EFB copies to RAM** (Dolphin forum / GameSettings) [unknown; 2026-10-11] docs/ARCHITECTURE.md:110-114; docs/research/GCWII.md:43 *The two figures disagree widely. Settings files are a lower bound. Informs: Whether to implement the EFB-to-RAM readback path*

### PICA200 hardware (66)

- citro3d GPU vs CPU time per frame: **~20 ms GPU drawing per frame vs ~190 ms CPU work (c3d_gpu_ms / c3d_cpu_ms)** [device; 2026-10-11] docs/HANDOFF.md L211-213, L394 *later analysis (L495) says 6-7 ms per submitted frame; GPU looks idle only because it is fed slowly Informs: GPU has headroom; motivated GPU-transform experiments*
- Combiner (TEV) stage count: **1, 3 and 6 stages each 193.3 Mpx/s (6 vs 1 stage: 1.00)** (gcbench 3/4, textured fill) [device; 2026-10-10] bench/results/latest.txt L208-212 *Informs: Combiner stages are free, so mapping GC TEV stages costs no fill*
- CPU write access to VRAM: **CPU cannot write VRAM** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:51 *The later timing probe has a 'memcpy FCRAM -> VRAM (CPU)' row; its device result is not in my sources (Azahar shows 315 MB/s) Informs: Uploads must go through GPU DMA/texture copy*
- CPU write to VRAM: **app crashed (VRAM is GPU-only for apps)** (.3dsx app) [device; ~2026-10-10] docs/ARCHITECTURE.md:341-343; docs/GPU_BACKEND_PLAN.md:59-60 *Root cause inferred (missing exheader mapping). Informs: All VRAM filling goes through GX DMA / DisplayTransfer / TextureCopy*
- CPU write to VRAM; GX copy to VRAM: **CPU memcpy into VRAM killed the app; GX copy linear->VRAM 895 MB/s, 248 MB/s including data-cache flush** (2DS gcbench) [device; run 2] docs/FINDINGS.md:82-84 *Informs: GPU-resident data must go through GX transfers*
- Data-cache flush call: **svcFlushProcessDataCache 1.5-8x faster than GSPGPU_FlushDataCache** (gcbench) [device] core/src/gx/pica/pica.c:246 (GPU_GUIDANCE rule 14) *Informs: Arena flush via svcFlushProcessDataCache*
- Depth test rejecting every fragment vs drawing: **4.16 vs 4.91 ns per pixel: no real early-Z, hidden pixels cost ~85% of drawn** [device; job 181] docs/research/TIMING_MODEL_PLAN.md:164 *Plain depth test. gcbench 5 G13 with explicit early-depth registers: 11.6x faster front-to-back; the two are compatible only if early depth must be enabled explicitly. Informs: Use early-depth registers for opaque passes*
- Depth test rejecting every fragment vs drawing: **4.16 ns (240 Mpixel/s) vs 4.91 ns per pixel** (GREATER on equal z) [device; 2026-10-15, job 183] relay_183.csv '# gpu fill depth test, all rejected' *Informs: No real early-Z: hidden pixels cost ~85% of drawn ones (overdraw and front-to-back ordering)*
- Display-transfer 2x2 downscale: **Hangs the GPU** (gcbench 4/4b) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:51 *Informs: Avoid the 2x2 downscale mode*
- Draw-call overhead, 1000 one-triangle draws: GPU / CPU per draw; with a combiner change before each: **0.54-0.55 / 3.21-3.29 us; 0.62-0.64 / 3.51-3.57 us** (citro3d) [device; 2026-10-15] relay_178.csv, relay_179.csv, relay_180.csv, relay_183.csv, relay_192.csv, relay_196.csv '# gpu draws' *Informs: Per-draw CPU cost (~3.3 us on the submitting core) motivates merging draws: TP issues ~1,200 GPU calls/s, about 4 ms/s of CPU*
- EFB copy and readback cost: **EFB copy 2 ms each; readback 3.9 ms** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:52 *Informs: Minimise EFB copies/readbacks*
- Fill rate: **193 Mpx/s with textures in VRAM; blending, depth, 1/3/6 combiner stages free; FCRAM textures 2.5-3.6x slower** (2DS citro3d) [device; 2026-10-10] docs/FINDINGS.md:192-194 *Informs: Texture cache in VRAM (6 MB)*
- Fill rate floor: plain / alpha blend / depth test+write / 3 or 6 combiner stages: **4.90-4.91 ns per pixel = 204 Mpixel/s (~1.3 GPU clocks per pixel)** (400x240 RGBA8 target, 16 layers, centre pixels verified) [device; job 180] docs/research/TIMING_MODEL_PLAN.md:116-121,127-128 *Earlier 193 Mpx/s (FINDINGS gcbench 3, gcbench 4 G12). Blending, depth and combiners free at this size. Informs: Fragment state is free; TEV->combiner multipass only costs fill*
- Fill rate floor: plain / alpha blend / depth test+write / 3 or 6 combiner stages / RGBA8 64x64 texture: **4.90-4.93 ns per pixel (~204 Mpixel/s) for all** (400x240 RGBA8 target, 16 layers, centre pixel verified) [device; 2026-10-15, jobs 180/183] relay_180.csv, relay_183.csv '# gpu fill' *Job 178's ~15 Gpixel/s fills are void (everything clipped at z +0.5). Job 179's combiner 3/6-stage figures (8.55 ns) had a texture left bound. Job 180 fixed both and supersedes them. Informs: Blending, depth and TEV stages are free at this size; the rasterizer/framebuffer sets the floor*
- Fill rate with VRAM textures (FINDINGS); FCRAM textures: **193 Mpx/s; FCRAM textures 2.5-3.6x slower** (gcbench run 3) [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:75; docs/research/PICA200.md:377-378 *Second-hand from FINDINGS.md. Floor later measured 204 Mpx/s (job 180). Informs: Texture placement VRAM vs FCRAM (gc3ds uses FCRAM)*
- Fill rate: opaque, blend, depth, VRAM texture 1:1: **193.3 Mpx/s for each (11.92 ms for 24 full-screen 400x240 quads), about 1.39 GPU clocks per pixel** (gcbench 3/4, bilinear, no mips) [device; 2026-10-10] bench/results/latest.txt L193-202 *Timingprobe job 180 measures 204 Mpx/s (4.90 ms per 16 layers) for the same cases, 5% higher Informs: Fill is not a constraint at 400x240 (assumed 4x overdraw = about 2 ms per frame)*
- Fog via W-buffer: **W-buffer depth map gives a linear fog index (dune fix)** (gcbench 4/4b) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:50 *Qualitative Informs: Fog implementation*
- G13 early depth: 24 full-screen layers: **11.98 ms without depth; 0.99 ms with early depth front-to-back (11.6x); no gain back-to-front** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:48; docs/research/GPU_GUIDANCE.md:96-97 *Informs: Early depth for GX zcomploc opaque passes when fill-bound*
- G13: early depth: **no depth 11.98 ms (192.3 Mpx/s); depth, z falling 11.98; depth, z rising 11.48 (200.7); early depth, z falling 11.98; early depth, z rising (front first) 0.99 ms (2331.7 Mpx/s, 12.1x)** (gcbench 5, 24 full-screen layers, GEQUAL) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L43-53 *Informs: Early depth gives a large win only for front-to-back order. Fill is not TP's bottleneck*
- G14 2x2 downscale 240x400 -> 120x200: **hung the GPU (gcbench 4 and 4b, reproducible)** [device; 2026-10-11] docs/research/MEASURE_PLAN.md:17,38; docs/research/GPU_GUIDANCE.md:64-65 *Informs: Avoid this configuration for EFB half-scale copies*
- G18 combiner rounding (12 cases): **all bit-exact: MODULATE x*y/255, x2 scale, ADD saturating, ADD_SIGNED x+y-128 clamped, SUBTRACT clamped, INTERPOLATE, MULTIPLY_ADD, ADD_MULTIPLY** (gcbench 4b) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:35; docs/research/GPU_GUIDANCE.md:46-48 *Informs: TEV mapping can be exact*
- G18: combiner rounding: **all 12 cases bit-exact: mod 80*80 = 40, FF*7F = 7F, x2 scale = 80, add saturating FF, addsigned 80+80 = 80, C0+50 = 90, 40+30 = 00, sub 80-30 = 50, 30-80 = 00 (clamped), lerp = 80, mad 80*80+80 = C0, (40+40)*80 = 40** (gcbench 4b, 8-bit grey) [device; 2026-10-11] bench/results/gcbench4b_2026-10-11.txt L23-47 *Informs: GC TEV ops that map onto PICA combiners can be exact*
- G19 / H10: near-plane clipping: **gcbench 5 G19: crossing triangle drew nothing (test triangle off-screen, inconclusive); visible control triangle drawn. gcbench 6 H10 (one vertex behind the camera): the expected wedge drawn (5x9 sample map, 30 of 45 samples)** (gcbench 5 / 6) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L64-78; gcbench6_2026-10-11.txt L51-57 *GCWII.md says Azahar draws an identical map Informs: No CPU clipping needed for near-plane crossing triangles*
- G19 near-plane clipping: **inconclusive (test triangle off-screen)** [device; 2026-10-11] docs/research/MEASURE_PLAN.md:51 *Open item.*
- G7: fog LUT index vs eye depth (near 1, far 100): **z mode (scale -1): index 85 at d=1.5, 32 at 4, 7 at 16, 0 by 80. W mode with scale 0, offset 0.01: 2 at d=1.5, 6 at 4, 21 at 16, 62 at 48, 123 at 96 (linear in d). W with scale -1: 128 down to 6** (gcbench 4 W2, linear LUT, centre pixel read back) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L421-434 *MEASURE_PLAN says hardware matches Azahar's model Informs: TP dune fog fix: W-buffer depth map with a LUT linear in eye distance*
- gcbench 4 / 4b GPU guidance: **Packed matrix palette free (6.69 Mtri/s); a 4th vertex attribute costs 45% (40% in gcbench 4); +128 shader instructions free; raw draws 0.74 us CPU vs 3.25 through citro3d; a per-draw uniform change stalls the GPU ~2 us; strips 2.4x lists; the 2x2 display-transfer downscale hangs** [device; 2026-10-11] git 67a11d4, 8754245 bodies *Informs: Vertex format and draw submission design*
- gcbench 5 results: **Several draws in one draw window 0.34 us CPU per draw; early depth 11.6x on occluded layers; vertex order changes indexed throughput 2.7x; 'GPU does not slow CPU memory'; a load used at once costs 2 cycles; cross-core kernel wakes lost ~1 in 4; time-stretching 0.66% of a core** [device; 2026-10-11] git 54db0da body *'GPU does not slow CPU memory' is superseded for texture sampling by job 192. Informs: GPU submission, audio stretch cost*
- gcbench 6: GameCube / Wii gaps: **EFB copy to RAM 0.2 ms (64x64) to 2.9 ms (full, CPU conversion dominant); synchronous EFB peek 93 us; depth readback exact; xxHash32 230 MB/s; texture decode 2-4 ms per 256x256; gyro / accel ~90-98 updates/s; software TLB +8 to +39 ns per access; PICA clips near-plane triangles (Azahar matches)** [device; 2026-10-11] git 8c6df6f body; 1fde46f subject *Informs: EFB copies, texture caching, MMU titles*
- GPU busy per submitted frame (2026-10-12 analysis): **6-7 ms per submitted frame (max ~15) -> ~40% busy at full speed (60 fields/s); core 0 feeds it ~7x too slowly** [device; 2026-10-12] docs/HANDOFF.md L495-499, L609-611 *extrapolated to full speed; GPU vertex transform already lost on the 2DS Informs: GPU headroom for picture quality (800x240, supersampling)*
- GPU frame overhead (submit -> idle, 1 triangle): **0.391 ms** (gcbench 3/4, citro3d, offscreen, no vsync) [device; 2026-10-10] bench/results/latest.txt L179; gcbench4 L323 *Not constant across runs: gcbench 4b's 'clear target' (frame overhead included) measured 0.166 ms in total Informs: Fixed per-frame or per-submit cost, and how many command-list flushes per frame are affordable*
- GPU texture sampling slowing core 0's memory: chase idle vs while the GPU samples a 1024x1024 RGBA8 texture in FCRAM: **1 MB chase 34.6 -> 70.1 ns (job 192), 34.6 -> 69.9 ns (job 196); 8 MB chase 193.6 -> 702.6 ns (192), 194.5 -> 704.3 ns (196)** (gpuprobe contention test) [device; 2026-10-15, jobs 192/196] relay_192.csv, relay_196.csv '# gpu contention chase' *The planned VRAM comparison did not run in either job: no line for it is in either log. Informs: Whether to place game textures in VRAM instead of FCRAM (pica.c uses FCRAM via C3D_TexInit)*
- GPU time per frame: **~20 ms of a ~190 ms frame** (2DS TP) [device; 2026-10-14/15] docs/research/REDESIGN_CANDIDATES.md:232 (cites gc3ds-gpu-direction memory) *Second-hand citation; method not in these files. Informs: GPU-side work (C17) won't show until core 0 moves*
- GPU time per frame and frame rates: **c3d_gpu_ms 11.2-11.9 ms (median 10.45) in the cutscene, 8.2 later; GPU submits 7.3/s; fields 13.2/s, shown 8.9/s, dropped 4.35/s in the cutscene; later 7.45 fields/s, none dropped** (clean run) [device; jobs 130-163] relay_157.csv column means; gx_split on relay_130/163 *Derived GPU busy ~9-14 % assumes c3d_gpu_ms is per submitted frame (column meaning not verified) Informs: GPU has headroom for moving vertex work into shaders (C1/C6)*
- GPU time per frame from citro3d (c3d_gpu_ms), heavy rows: **31.8-61.9 ms in the 10-11 morning builds, 21.9 ms with earlyq, 7.6-16 ms afterwards (perf runs 9-18), 4.0-5.9 ms with indexed draws and pipefast. 9.4-12.8 ms in relay jobs 60-99, at about 6.4-7.1 GPU submits/s.** (TP heavy rows) [device; 2026-10-11 to 2026-10-13] core/out/perf_2026-10-12_hle2.csv runs 1-21; relay_60-99 *Exact meaning of c3d_gpu_ms (C3D drawing time) not checked in source Informs: Load on the GPU. Batching and early queue start cut GPU time per frame 3-10x.*
- GPU transfers: texture copy linear->VRAM / VRAM->linear; display transfer 400x240 RGBA8->RGB8; memory fill: **177-253 / 229-248 MB/s; 1.69-1.98 ms; memory fill 2.9-34 GB/s (not usable)** (1 MB copies) [device; 2026-10-15] relay_178.csv-relay_196.csv '# gpu xfer' *The tick around C3D_SyncMemoryFill does not time the fill itself, so the fill numbers are invalid. Informs: EFB/XFB copy and texture upload paths*
- H10 near-plane clipping: **PICA draws the expected visible wedge for a triangle crossing the camera plane; Azahar draws the identical sample map** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:249 *Validates Azahar for clipping checks only, not timing. Informs: No CPU clipping needed*
- H2 depth buffer readback: **raw TextureCopy of D24S8 returns exact values; z 0.25 -> 0xC00000, z 0.75 -> 0x400000 (24-bit, reversed)** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:242 *Tiled data needs untiling on the CPU. Informs: Z copies and Z peeks are possible*
- H3 synchronous single-pixel EFB peek latency: **93 us mean, 712 us worst** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:243 *Informs: A few peeks per frame are fine; many need batching or one cached readback*
- H6 fill rate by render-target format: **RGBA8 = RGB565 = RGB5A1 = RGBA4 = 193.6 Mpx/s** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:246 *Confirms GPU_BACKEND_PLAN's 193 Mpx/s. Informs: EFB pixel-format changes cost nothing in fill*
- H6: fill rate by render-target format: **RGBA8 193.6, RGB565 193.5, RGB5A1 193.6, RGBA4 193.6 Mpx/s (11.90 ms each)** (gcbench 6, 24 full-screen quads, VRAM texture) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L42-49 *Informs: Choose the EFB pixel format for fidelity. Fill is unaffected*
- PICA200 fill rates (job 180): **204 Mpixel/s (4.9 ns/pixel) floor for plain, blend, depth, 3-6 combiner stages and cache-resident textures; 1024x1024 minified without mipmaps: ETC1 117, RGB565 27, RGBA8 17 Mpixel/s; display transfer 1.69-1.98 ms; texture copies 230-250 MB/s; memory-fill timing unusable** [device; 2026-10-15] git 72280da, c908458 diffs *Informs: Texture format and mipmaps (C17) once the GPU matters*
- PICA200 geometry and fill throughput: **6 Mtri/s; 193 Mpx/s** (gcbench) [device; ~2026-10-10] docs/GPU_BACKEND_PLAN.md:4-5 (from FINDINGS.md) *Fill confirmed later as 193.6 Mpx/s for every render-target format (GCWII.md:246). A full 640x528 fill is therefore ~1.75 ms (arithmetic). Informs: The GPU is not the bottleneck; render at native EFB size*
- PICA200 geometry rate: **6.0 Mtri/s (5.0 at 20k tri/frame); 4.3 Mtri/s with GC-style 4-light vertex shader; 10 Mtri/s with shared indexed vertices** (2DS citro3d, off-screen, no vsync) [device; 2026-10-10] docs/FINDINGS.md:188-191 *GC game need of 1-2 Mtri/s is an assumption. Informs: GPU geometry not a limit*
- PICA200 throughput: **~6 Mtri/s and 193 Mpx/s; combiner stages free** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:49 *Informs: GPU has ample headroom*
- PICA200 vertex rate by attribute format: **float3 at 12/16/24-byte stride 49.6-49.9 ns per vertex (~20 M vertices/s, 13.3 GPU clocks); s16x4+u8x4+s16x2 at 16 B 55.6-56.9 ns** (gpuprobe, 60,000 vertices) [device; 2026-10-15, jobs 178-183] relay_178.csv, relay_179.csv, relay_180.csv, relay_183.csv '# gpu vertex' *Job 177's 107.8 ns at 24 B came from a run where triangles were clipped. Job 178 fixed it. Informs: Vertex fetch format barely matters: the vertex stage sets the rate. The GPU is far above TP's ~0.3 M vertices/s.*
- PICA200 vertex shader length / uniform change / attribute count: **+128 MADs: no change (shader length free); a uniform change between draws stalls the GPU ~2 us; a 4th vertex attribute costs ~45% of vertex rate; integer attributes as fast as float** (2DS gcbench 4/4b/5 (GPU_GUIDANCE rules 2,3,4,7)) [device; before 2026-10-13] docs/research/PERF_LEVERS.md:67-71; REDESIGN_CANDIDATES.md:85-87 *Cited from GPU_GUIDANCE.md, not read here. Informs: C6 (texgen/normals in shader), C1 attribute layout*
- Raw draw cost and primitive type: **Raw draws 0.74 us CPU vs 3.25 us via citro3d; strips 2.4x faster than lists** (gcbench 4/4b) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:49 *Informs: Bypass citro3d draw path; prefer strips*
- Render-to-texture (EFB copy) costs: **render into a texture: free; extra 256x256 copy 2 ms; reading 256x256 back to CPU 3.9 ms** (2DS citro3d) [device; 2026-10-10] docs/FINDINGS.md:197-202 *Informs: EFB copies stay on GPU; frame skip (C9) must still produce them*
- Triangle rate (FINDINGS, gcbench run 3): **6.0 Mtri/s; 10 indexed; 4.3 with 4-light vertex shader** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:74; docs/research/PICA200.md:376 *Reported second-hand from FINDINGS.md (not read here). 4-light result suggests shader length does cost, consistent with job 181 over gcbench 4 G1.*
- Triangle setup: full screen as 32/16/8/4-pixel tiles: **0.714 / 0.843 / 1.198 / 1.831 ms (4 px: 50.9 ns per vertex, 152.6 ns per triangle)** (one-layer coverage) [device; 2026-10-15, job 183] relay_183.csv '# gpu setup' *Informs: Small triangles are vertex-bound; setup itself is cheap*
- Vertex attribute and shader costs (gcbench 4/4b): **Matrix palette with index in position.w is free; a 4th attribute costs 45%; shader length is free** (gcbench 4/4b) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:45-48 *Informs: Palette transform in the vertex shader; keep attribute count at 3*
- Vertex shader length cost: +0 / +32 / +64 instructions: **49.6 / 99.2 / 188.6 ns per vertex** (pos float3) [device; 2026-10-15, jobs 181/183] relay_183.csv '# gpu shader +N instructions' (also job 181) *Informs: Generated GX-pass shaders (lighting, texgen, native shapes) should stay short: each instruction adds ~1.4-1.6 ns per vertex*
- WFE in user mode; GPU effect on CPU memory: **WFE does not sleep in user mode; GPU does not slow CPU memory** (2DS (GPU_GUIDANCE rules 17, 25)) [device; before 2026-10-13] docs/research/PERF_LEVERS.md:68-70 *Informs: Cross-core waiting design; GPU-resident data cost*
- Writing culled triangles as degenerate ones: **tripled the GPU's time per frame** (TP) [device; commit 7f2033e] core/src/gx/pica/pica.c:992-994 *Negative result: the vertex shader still runs for every vertex. Informs: Worker culls and appends in queue order*
- Float24 position precision on PICA: **max 0.114 texels error with grid origin vs 1.48 without; depth up to 299 units of 2^24 off (CPU path 220)** (vfast_check.sh 2 1500, TP to field 1,500) [pc; 2026-10-11] docs/HANDOFF.md L408-411
- PICA float24 precision on TP world coordinates: **Up to 1.5 texels off; fixed by grid-origin-relative positions** (GPU position transform, checked on PC (soft.c --pica-tev) and Azahar) [pc; 2026-10-11, 29a22f7] memory/gc3ds-gpu-direction.md:31-32 *Step 1 (GPU position transform) not yet measured on 2DS at time of note Informs: GPU transform needs relative coordinates*
- Azahar: CPU cost per draw and transfer engine: **CPU 7.77 us per one-triangle draw, 8.48 us with a combiner change; memory fill 1 MB VRAM 6,995 MB/s; texture copy 1 MB 1,761 MB/s each way; display transfer 400x240 rgba8->rgb8 0.143 ms** (Azahar) [azahar; 2026-10-15] azahar-sdmc/timing_gpu.txt:26-32 *Device raw draw 0.74 us / citro3d 3.25 us Informs: None for speed*
- Azahar: GPU probe (C3D_GetDrawingTime): **Every section 0.08 ms: vertex 1.40 ns/vertex for all formats; fill 0.055 ns/pixel for every mode, texture and combiner count** (Azahar) [azahar; 2026-10-15 10:48] azahar-sdmc/timing_gpu.txt:3-24 *Only device runs of gpuprobe.c can answer GPU-cost questions Informs: Azahar GPU timing is constant and meaningless*
- Fragment lighting LUT and GX lighting cost estimate in vertex shader: **GX per-vertex lighting (8 lights x 2 channels) ~150-250 instructions; texgen ~6-20 each [I]** [estimate; 2026-10-11] docs/research/PICA200.md:346,349 *Inferred. At job 181's ~1.5 ns per instruction per vertex (+32 -> +49 ns), 200 instructions would be ~350 ns/vertex. Informs: Whether to light on the GPU vertex shader*
- GPU busy share, estimated: **At 21% speed: about 7 submits/s × 11 ms ≈ 8% busy. Extrapolated to full speed: about 35-40% busy.** (tp850 cutscene) [estimate; 2026-10-12/13] Derived from relay_83/84/97 gpu_submits_s and c3d_gpu_ms *Assumes GPU time grows linearly with frames Informs: The GPU has headroom. Moving drawing-shaped work to it is possible.*
- PICA float24 precision: **1.7.16 format: whole-unit precision lost near 65,536** [estimate; 2026-10-11] docs/research/PICA200.md:78
- Vertex shader uniform capacity for matrix palette: **palette of 14 matrices uses 95 of 96 float uniforms** [estimate; 2026-10-11] docs/HANDOFF.md L400-401 *design constraint from gxpass.v.pica, not a timing*
- Vertex shader uniform space for palette: **c0-c95 float uniforms; ~29 3x4 matrices from c8 (28 fit per GPU_GUIDANCE); indices >=96 read (1,1,1,1)** [estimate; 2026-10-11] docs/research/PICA200.md:20-21,65,71,89-91; docs/research/GPU_GUIDANCE.md:14 *Spec-derived; 28 matrices measured working. Informs: Palette batch size*
- PICA fill rate, spec figure: **800 Mpx/s at 200 MHz (DMP); figure at the 3DS clock unverified** (Wikipedia/DMP) [unknown; 2026-10-10] docs/ARCHITECTURE.md:367, :454 *Superseded by the device measurement of 193 Mpx/s (GPU_BACKEND_PLAN.md:4; GCWII.md:246). Informs: GPU headroom*
- Vendor PICA200 peaks (DMP via Wikipedia): **15.3 Mpolygons/s + 800 Mpixels/s at 200 MHz (2010) vs 40 Mtri/s + 400 Mpixels/s at 100 MHz (2008)** [unknown; 2026-10-11] docs/research/PICA200.md:366-367 *Vendor claims conflict; device measures 6-12 Mtri/s, 193-204 Mpx/s.*
- Vertex shader program size / stacks: **<=512 instructions, 128 operand descriptors [?]; CALL 4, IF 8, LOOP 4** [unknown; 2026-10-11] docs/research/PICA200.md:67-68,73-74 *Documentation, not measured.*

### Audio and DSP (21)

- A1 GameCube DSP-ADPCM played by ndsp vs our decoder: **correlation 1.00000; same gain and residual as PCM16** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:23 *Informs: ADPCM decode offload to 3DS DSP*
- A1: GC DSP-ADPCM through the 3DS DSP (ndsp): **ADPCM via ndsp: correlation 1.00000, gain 0.994, max /diff/ 104, against PCM16 baseline correlation 1.00000, gain 0.994, max diff 105** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L602-609 *Informs: GC ADPCM voices can be played natively by the 3DS DSP*
- A2 ndsp cost to core 0 with 24 ADPCM channels: **no measurable cost** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:24
- A2: ndsp ARM-side cost: **core 0 left with 100.0% while 24 ADPCM channels play** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L610-611 *Informs: Offloading AX mixing to the DSP frees core 0 entirely*
- A3 ndsp sample rates: **8-192 kHz all play; effective rate ~97% of requested** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:57
- A3: ndsp effective sample rate: **8000 -> 7950 Hz; 22050 -> 21810; 32000 -> 31197; 48000 -> 46197; 96000 -> 88592; 192000 -> 164391 (16000 samples each)** (gcbench 5) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L18-25 *Includes start and poll overhead (about 97% at 32 kHz). Not a clock error Informs: ndsp plays the GC 32 kHz rate directly*
- A4 ndsp frame: **205 callbacks/s = 4.88 ms per sound frame** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:58
- A4: ndsp sound-frame period; ndsp tone timing: **205 callbacks per second = 4.88 ms per frame. A 0.5 s tone took 0.51 s** (gcbench 3/5) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L26-27; latest.txt L167 *Informs: Audio buffer and latency scheduling against the GC 5 ms AX frame*
- A5 WSOLA time-stretch cost at x1.25, x2, x4: **0.66% of a core** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:59; docs/research/GPU_GUIDANCE.md:109-110 *Informs: Normal-pitch audio below full speed is affordable*
- A5: WSOLA time-stretch cost: **x1.25 6.61 ms CPU per second of output; x2.00 6.57; x4.00 6.57 (0.66% of a core)** (gcbench 5, core 0) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L28-32 *Informs: Time-stretching audio to cover a game running at 20% speed is affordable*
- Audio cost: **DSP-ADPCM decode + resample + mix 0.42% of a core per voice (239 voices per core); ndsp works** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:177-179 *Informs: Audio is cheap*
- GameCube ADPCM through ndsp: **Plays natively** (gcbench 4b) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:52 *Qualitative Informs: Audio path can use ndsp ADPCM directly*
- Software GC DSP-ADPCM decode, resample and mix: **64 voices x 1 s at 32 kHz in 0.268 s (gcbench 4: 0.259 s); 0.42% of a core per voice (0.40%); 239 real-time voices per core (248)** (gcbench 3/4, core 0) [device; 2026-10-10] bench/results/latest.txt L163-166; gcbench4 L299-303 *Informs: Software audio mixing is cheap: at most 64 AX voices = about 27% of a core worst case*
- WSOLA time-stretch cost: **0.66% of a core** (likely gcbench on the 2DS) [device; 2026-10-11] docs/research/GCWII.md:15 *The platform is not stated in this file. Informs: Time-stretching is affordable*
- 3DS DSP/ndsp parameters: **TeakLite ~134 MHz, 24 channels, 32728 Hz output, 160 stereo samples per frame, ndsp sleeps ~4.888 ms per frame, frame callback ~5 ms** (3dbrew/libctru) [estimate; 2026-10-11] docs/ARCHITECTURE.md:382-385; docs/research/AUDIO.md:67-79 *Needs a dumped dspfirm.cdc on SD. Informs: Voice offload to ndsp vs CPU mixing*
- DSP LLE cost estimate: **interpreter at ~20-30 ARM cycles per DSP instruction needs ~2 GHz; a DSP JIT at ~4 cycles per instruction takes a whole 3DS core** (81 MHz GC DSP) [estimate; 2026-10-11] docs/research/GCWII.md:213-217 *Inferred, not measured. Rejected for the 79 LLE titles. Informs: No DSP LLE*
- Guest audio cadence: **GC AX: 32 samples/ms, 5 ms frames (160 samples at 32 kHz), PB updates every 1 ms; Wii AX 3 ms (96 samples); Zelda ucode 80-sample frames** (Dolphin source) [estimate; 2026-10-11] docs/ARCHITECTURE.md:126-128; docs/research/AUDIO.md:21-24 *Third-party model. Informs: Audio HLE scheduling granularity*
- Guest audio timing constants (Dolphin model): **AX empty command list yields 2500 cycles; audio DMA moves 32 B per 4 kHz tick; DMA interrupt ~200 cycles after start (shorter crashed some games); ARAM DMA completion (count/32)*246 ticks** (Dolphin source) [estimate; 2026-10-11] docs/research/AUDIO.md:34-38, :46 *Third-party. Informs: Null-mixer protocol timing so no game stalls*
- Sample generation below full speed: **at 15% speed the game produces 15% of samples per real second; proposal: mute below ~50%, time-stretch 50-100%** [estimate; 2026-10-11] docs/research/AUDIO.md:96-103; docs/research/GCWII.md:14-17 *Owner decision pending. Informs: Audio pacing policy*
- Direct GC ADPCM to ndsp: **unresolved: the two research passes disagree; Test A1 in MEASURE_PLAN.md settles it** [unknown; 2026-10-11] docs/research/AUDIO.md:80-85 *If it passes, mono ADPCM decode costs the ARM11 nothing. ndsp has no stereo ADPCM and no loop points. The A1 result is not in these files. Informs: CPU cost of audio*
- Microcode coverage: **>99% of games use AX [unverified]; TP uses the Zelda family (CRC 0x6ca33a6d); PB_ADDR/PROCESS/OUTPUT/END gives partial audio in ~90% of games** (Dolphin blog / delroth 2012) [unknown; 2026-10-11] docs/research/AUDIO.md:17-18, :51-53 *The >99% figure is from a search snippet. Informs: Which ucode HLE to build first (TP needs Zelda, not AX)*

### Disc and SD I/O (19)

- Disc reads during boot and in play: **boot from disc (no state): disc_pct 40-92 % of core 0 in seconds 2-10, 0.36-1.96 MB/s read, ~0.55-0.84 MB/s while disc_pct is 86-92 %; in tp850 play 91 of 540 rows read the disc, mean 43 KB/s, disc_pct ~2 %** (nkit ISO on SD) [device; jobs 103, 108, 157] core/out/relay_103.csv, relay_108.csv, relay_157.csv *Boot runs were only ~10-30 s Informs: Disc path matters for load times only*
- SD card throughput: **sequential read 10 MB/s, write 6.1 MB/s; random reads 8 ms per 32 KB (3.9 MB/s), 2.4 ms per 2 KB** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:168-172 *Informs: Read cache; GC disc speed (2-3 MB/s) covered*
- SD card: sequential 64 KB reads / random 32 KB read / random 2 KB read: **5.03-5.12 MB/s / 9.37-9.71 ms each / 4.04-4.08 ms each** (sdmc via libctru) [device; 2026-10-15] relay_177.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing sd' *Informs: Disc streaming design (a 4 ms floor per small read; read large and cache)*
- SD random 2 KB reads: **stdio buf 2K 0.82 MB/s, 2.39 ms per read; FSFILE 0.82 MB/s, 2.39 ms per read (gcbench 4: 2.38 / 2.36)** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L148-149 *Conflict: timingprobe job 178 gives 4.0 ms for random 2 KB (method not compared) Informs: Small random disc reads (file system, streamed audio) need batching or caching*
- SD random 32 KB reads: **stdio buf 32K 3.87 MB/s, 8.08 ms per read; FSFILE 3.91 MB/s, 8.00 ms per read (gcbench 4: 8.06 / 7.96)** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L146-147 *Timingprobe job 178 gives 9.7 ms for random 32 KB Informs: Disc read cache design: random seeks cost about 8 ms each*
- SD random 32 KB via stdio with a 1 MB buffer: **0.31 MB/s, 99.92 ms per read (gcbench 4: 98.21 ms)** (gcbench 3/4, setvbuf 1M) [device; 2026-10-10] bench/results/latest.txt L145 *Negative result: a large stdio buffer refills 1 MB per seek, 12x slower Informs: Never use large stdio buffers for random disc access. Use FSFILE or matched buffers*
- SD sequential 1 MB reads: **stdio 9.87 MB/s, FSFILE 10.04 (gcbench 4: 9.92 / 10.20)** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L143-144 *Timingprobe job 178: 64 KB sequential 5.11 MB/s (smaller requests) Informs: Covers GC disc rate (2-3 MB/s) for streaming. Wii is borderline*
- SD write speed: **32 MB: 6.11 MB/s (gcbench 3); 31 MB: 5.05 MB/s (gcbench 4)** (sdmc:/gcbench/tmp.bin) [device; 2026-10-10 / 2026-10-11] bench/results/latest.txt L142; gcbench4 L265 *17% run-to-run spread, the largest of any repeated number Informs: Save states and shader or texture cache writes*
- SD: 64 KB sequential / random 32 KB / random 2 KB: **5.11 MB/s / 9.7 ms / 4.0 ms** [device; job 178] docs/research/TIMING_MODEL_PLAN.md:104 *Informs: Disc streaming / ISO read strategy (prefetch, large reads)*
- Software AES: **10.1 MB/s (61-92% of a core at Wii disc speed); hardware AES unusable (fixed key slots)** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:173-176 *Informs: Decrypt Wii images on PC*
- Software AES and SHA-1 (Wii discs): **AES-128-CBC sw 10.1 MB/s; SHA-1 30.2 MB/s bulk, 27.9 MB/s at 1 KB (gcbench 4: 29.0 / 26.9). Wii 6-9 MB/s needs AES at 61-92% of a core, with H0 SHA-1 83-124%. Hardware AES unusable (PS service has fixed keyslots only)** (mbedtls on core 0) [device; 2026-10-10] bench/results/latest.txt L151-160; gcbench4 L282-296 *Informs: Decrypt Wii images on the PC beforehand (adopted). Not relevant to TP GC*
- Software AES-128-CBC on 2DS (Wii disc decryption): **10.1 MB/s; Wii discs need 6-9 MB/s, so 61-92% of a core** [device; gcbench 4, by 2026-10-11] docs/research/GCWII.md:12-13, :207-211 *Hardware AES works only with console keyslots. Verdict: decrypt offline on the PC or keep a decrypted cache on SD. Informs: Wii disc handling*
- stdio buffer refills on disc reads: **13% of the time while TP loaded** (3DS, buffered fopen) [device] core/src/disc/disc.c:45-47 *Informs: Unbuffered disc reads (setvbuf _IONBF)*
- TP health screen / logos speed; disc-read share while loading: **68-69% speed after polling-loop skip; disc reads up to 57% of time while loading** (bottom-screen profile, inline GX, JIT level 1) [device; 2026-10-10] docs/HANDOFF.md L72 *Informs: disc I/O cost during loads; light scenes are not the bottleneck*
- Wii disc decryption speed: **Software AES 10 MB/s; hardware AES unusable** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:46 *Informs: Decrypt Wii discs offline (owner decision pending)*
- Disc reads during TP and image size: **DI: 1,969 reads / 20.8 MB by field ~900; 2,012 / 21.4 MB by 1400; 2,460 / 26.8 MB by 3300 (~5.3 MB over 900 fields, ~360 KB per emulated second). Image: NKit 1,048,721,408 bytes (71.8% of 1.46 GB), 3,656 files** [pc] core/out/tp/*/log.txt; core/out/tp/info/log.txt *Rate derived by me. Informs: Disc I/O is not a bottleneck*
- Azahar: SD card reads: **Sequential 64 KB 4.98 MB/s; random 32 KB 6.547 ms; random 2 KB 0.925 ms** (Azahar (host disk)) [azahar; 2026-10-15] azahar-sdmc/timing.txt:99-101 *Azahar SD speed is host-dependent Informs: None; device SD figures needed for streaming/load decisions*
- GC disc timing (guest): **2-3 MB/s CAV, 128 ms average access; Dolphin: 300 us command, 600 us read start, 32 MiB/s buffered, 1 MiB read-ahead; seek 0.035 s + 50 s/m*d below 1 mm, else 0.075 s + 4.5 s/m*d; card read 512 KiB/s, write 98 KB/s** (Nintendo spec / Dolphin model) [estimate; 2026-10-11] docs/ARCHITECTURE.md:161-164; docs/research/GCWII.md:152-159 *Instant reads break games (Metroid Prime doors, Sonic Riders, Summoner, Pitfall). Informs: Whether disc latency has to be modelled*
- SD card throughput on 3DS: **unverified; no published benchmark** [unknown; 2026-10-10] docs/ARCHITECTURE.md:389, :425, :455 *A gap in these docs. NATIVE_PORT_PLAN.md:157 shows SD writes per feed caused 5-10 s frames, so SD cost is real but unquantified here. Informs: Disc streaming, Wii decrypted-cache option, debug logging cost*

### Scheduling and timing (23)

- Cross-core handoff: **spinning on a shared flag 0.5 us round trip; LightEvent between cores 0 and 2 timed out on third trip** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:180-183 *LightEvent timeout uninvestigated in these docs. Informs: Lock-free spinning queues for GX FIFO*
- Dispatcher round-trip sources: **cutscene ~3,400 sc/s (PPCSync); Link/Epona stretch ~21k stand-in calls/s, each returning to dispatcher** [device; 2026-10-14] docs/HANDOFF.md L996-998
- Guest exception rates: **Cutscene (jobs 70-99): sc (0xC00) about 3,700/s, external interrupt (0x500) about 170/s, FP unavailable (0x800) about 18/s, decrementer (0x900) about 7/s. Title (job 59): sc 356/s, external interrupt 615/s.** (tp850 / title) [device; 2026-10-12/13] '# exceptions 10s' in relay_52, 59, 70, 76, 83, 91, 97-99 *Informs: sc round trips through the dispatcher evict JIT code (OPT_SYSCALL was later built for this)*
- HLE shadow-scan stand-in A/B (f7e63ab): **cutscene: stand-in covers ~36M guest instr/s; field rate 5.02 -> 3.85/s while cycle-based speed rose 8.39% -> 9.67%; title loop ~5k instr per 10 s (noise)** [device; 2026-10-12, perf_2026-10-12_hle.csv] docs/HANDOFF.md L484-493 *one call ran up to 4M instr vs 1M slices, delaying events; speed_pct misleading, judge by fields_s Informs: hle_run respects caller budget*
- icbi exits in the state-load window (OPT_ICBI): **~622,000 icbi exits in the first 10 s vs 2,451 that dropped code (a loop at 812fffa8 invalidates 81200000-812fffff in the first field, none after). qemu 'other' returns 636,584 -> 23,792 with OPT_ICBI; device jobs 186 / 187 unchanged, speed identical** [device; 2026-10-15] git 308e516, 4b313d7 bodies; docs/HANDOFF.md ~l.1147 *Informs: Load-time only; stays off*
- Kernel thread round trip: same core / core 0<->core 2 / svcSleepThread(0) yield: **7.37-7.91 us / 2.21-2.80 us / 1.11-1.20 us** (signal/wake round trips) [device; 2026-10-15] relay_177.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing kernel' *Informs: Cost of cross-core handoffs (GX thread, vertex worker, job rings) and of yield-based waiting*
- Level-1 JIT dispatcher cost: **~8M guest instructions/s in ~1.3M blocks/s, ~650 host cycles per 6-instruction block (title scene)** [device; 2026-10-11] git 4bf5279 body *Informs: Block linking (level 2)*
- Link requests vs links (job 184 vs 185): **Commit 780c345: 155,000 link requests/s vs ~2,600 links/s, 'part of the dispatcher's ~12% of core 0'. Job 185: 155,000/s only in the state-load window, 10-140/s in play; real dispatcher <1% (the sampler's dispatch bucket is mostly the idle hook's GX wait)** [device; 2026-10-15] git 780c345 body; docs/HANDOFF.md ~l.1146 *The commit body's ~12% dispatcher reading is superseded. Informs: Dispatcher is not a lever*
- OPT_LATCH budget checks only at loop closers: **qemu: hot 9.4 -> 9.3, exit stubs 3.1 -> 2.8 words/guest instr; drift 19k of 7.1G cycles; job 169 core 0 26.06 +- 0.24 vs 25.46 +- 0.23 (+2.4%), I-cache -4%, fetch -3%, game speed unchanged (GX-bound); jobs 170/171 full: later 12.56 vs 12.41 and 12.51 vs 12.44; cutscene 21.86 vs 21.45 and 21.72 vs 21.51** [device; 2026-10-15] docs/HANDOFF.md L1099-1108, L1127-1128 *Informs: default on*
- Real-time pacing catch-up: **logos ran at 144% speed after a slow load** (old pacing that made up lost time) [device] core/host3ds/main.c:850-855 *A bug, now fixed: lag is capped at 50 ms. Informs: Pacing origin follows; at most 50 ms lag*
- svcGetSystemTick cost: **198 cycles** (timingprobe svc) [device; job 176] docs/research/TIMING_MODEL_PLAN.md:92 *Informs: Cost of timing reads in hot paths / event scheduler*
- svcGetSystemTick cost: **251 ns per call** (2DS gcbench run 3) [device; 2026-10-10] docs/FINDINGS.md:165 *Informs: Timing reads in hot paths*
- svcGetSystemTick cost: **198-202 cycles** (core 0) [device; 2026-10-15] relay_176.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing svc GetSystemTick' *Informs: Timing/profiling instrumentation overhead and mftb/timebase emulation via svc*
- svcGetSystemTick cost: **251 ns per call (gcbench 4: 247 ns), about 200 cycles** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L170; gcbench4 L310 *Agrees with timingprobe job 176 (198 cycles) Informs: Do not read the system tick per block or per event. Use cycle counters or downcounts*
- Thread switch: same-core round trip / core 0<->2 round trip / svcSleepThread(0): **7.91 / 2.21 / 1.13 us** (hwprobe/timingprobe) [device; job 178] docs/research/TIMING_MODEL_PLAN.md:103 *Kernel-thread handoff; spin ping-pong is 453 ns (gcbench 4 M6), earlier spin handoff ~0.9 us (FINDINGS). Informs: Spin vs kernel wait for GX/vertex handoffs*
- Unbudgeted stand-in delays events: **The stand-in ran up to 4M instructions per call; field rate fell 5.02 -> 3.85/s while cycle-based speed rose** [device; 2026-10-12] git e7e7cbe body *Informs: Stand-ins must respect the slice budget; cycle speed can mislead*
- Share of TP loading time spent in idle/RAM-polling loops (skipped to next event): **88% of TP loading time (PC)** [pc; 2026-10-10] docs/HANDOFF.md L29-30 *Informs: keep polling-loop skip (gekko_poll_loop_len); polling blocks never linked*
- Dispatcher returns in the TP state-load window (qemu): **'other' returns 640,342 of ~680k dispatches, driven by 622,595 icbi (2,451 dropped all code); link requests 19,381 (opt1) / 28,011 (opt5); indirect misses 13,451 (6,300 slot taken) / 18,328 (5,909 slot taken)** [qemu_model] //wsl$/Debian/root/qtp1/log.txt; qtp5/log.txt *This is a load-time effect (see the OPT_ICBI entries); in play link requests are 10-140/s. Informs: OPT_ICBI and dispatcher work*
- Guest time inside compiled code fix (e689553): **qemu JIT TP to field 500 before fix: 739 draws / 15,262 DSP mails vs interpreter 1,270 / 28,051; after: 1,268 / 28,051; dispatches 3.5M vs 578M; 9.9 vs 23.6 host words per guest instr compiled; qemu wall 23 vs 32 min** [qemu_model; 2026-10-11, e689553] docs/HANDOFF.md L297-307 *Informs: exact event timing requires time passed into helpers*
- Guest time inside JIT runs (bug found under qemu): **TP at field 500 with the JIT: 739 draws and 15,262 DSP mails vs the interpreter's 1,270 and 28,051, until helpers saw exact time** [qemu_model; 2026-10-11] git e689553 body *Informs: Event timing correctness*
- Owner-decision models: budget branch and dead write-backs: **30.3% of block runs in blocks with a backward exit; dropping budget branch elsewhere saves 0.70 hot words/block run (2.5%; earlier '~7%' was wrong); dead write-backs: of 1.56/run at constant exits 0.46 dead (0.66 trusting call convention), at blr/bctr 0.12 of 0.42; ~1.6% exact-enough** (TP 850-880, mask 627) [qemu_model; 2026-10-15] docs/HANDOFF.md L1094-1098
- 3DS thread scheduler behaviour: **strict priority with no time slicing: a spinning thread starves equal or lower priority threads on its core** (3dbrew Multi-threading) [estimate; 2026-10-10] docs/ARCHITECTURE.md:275-276 *Not measured in these docs. Informs: Spin-wait vs. blocking design for GX/worker handoff*
- Guest clocks and timebase: **Gekko 486 MHz, bus 162 MHz, 1T-SRAM 324 MHz, ARAM/DSP 81 MHz; TB/decrementer = CPU/12 (40.5 MHz GC, 60.75 MHz Wii)** (Dolphin SystemTimers.h) [estimate; 2026-10-11] docs/ARCHITECTURE.md:176-177; docs/research/GCWII.md:83 *Informs: Event timing model and timing-drift tolerance*

### Multicore and contention (61)

- --steal (GX thread runs vertex jobs while it would wait): **GX-side waits fell; core 0 slowed 3-5%** (2DS job 164) [device; 2026-10-15] docs/research/REDESIGN_CANDIDATES.md:212-213 *Same L2 pollution explanation as C2. Informs: C15 rejected*
- C1 WFE in user mode: **returns at once (2000 of 2000 early)** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:22; docs/research/GPU_GUIDANCE.md:80 *Negative result. Informs: No event-based sleeping; spin or kernel wait*
- C1: WFE/SEV from user mode: **WFE works in user mode but returns at once: 0 wakes, 2000 of 2000 early (mean 0.00 us)** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L612-614 *Negative result Informs: No event-based low-power waiting. Spin or arbitrate*
- C2 core 1 time limit: **80% gives 79% of a core; 85% and 89% refused under Homebrew Launcher** (gcbench 4, 3dsx) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:21; docs/research/GPU_GUIDANCE.md:81 *PICA200.md:406-407 reports 78% at 80%. Informs: Core 1 usable at ~80%*
- C2: core 1 time limit under the Homebrew Launcher: **80% accepted, core 1 runs at 79% of core 2's rate. 85% and 89% REFUSED (still 79%)** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L597-601 *Informs: Core 1 is capped at about 79% of a core. Plan only background work there*
- C3 svcArbitrateAddress round trip core 0 <-> core 2: **2.5 us; 501 of 2000 wake-ups per side hit the 2 ms timeout (~1 in 4 lost)** (gcbench 5) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:54; docs/research/GPU_GUIDANCE.md:107-108 *Informs: Spin or always wait with timeout for cross-core handoffs*
- C3: svcArbitrateAddress wake-up round trip core 0 <-> core 2: **2.5 us mean; 501 of 2000 waits per side (partner 501, main 501) ended by the 2 ms timeout** (gcbench 5) [device; 2026-10-11] bench/results/gcbench5_2026-10-11.txt L6-8 *Timingprobe job 178: thread switch core 0 <-> 2 round trip 2.21 us. A quarter of kernel cross-core wakes are lost Informs: Kernel sleeping for GX or worker threads costs 5x a spin and is unreliable. Use spin-then-sleep with timeouts*
- Compact vertex-worker jobs (--vin): **Neutral; reservations still full-size; GX wait median ~1%** (tp850) [device; 2026-10-15, job 190] memory/gc3ds-jit-dead-ends.md:32-33 (job 190) *Informs: Rejected; GX side is not what core 0 waits on*
- Core 0 <-> core 2 handoff: **Spin handoff 0.5 us; LightEvent handoff timed out (uninvestigated)** (gcbench) [device; 2026-10-10, run 3] memory/tp3ds-gc-wii-on-2ds.md:54 *Azahar hw.txt also shows LightEvent failing (3/2000 answered), which suggests a probe bug rather than hardware Informs: Use spin handoffs between JIT and GX threads*
- Core 0 chase latency while core 2 streams stores over 4 MB (256 KB / 1 MB / 8 MB chase): **36 / 120 / 604 cycles vs alone 28 / 28 / 156** (timingprobe xcore) [device; job 176] docs/research/TIMING_MODEL_PLAN.md:91 *Shared L2 evicted by other core: stated mechanism behind C2 and C15 losses (second vertex worker etc.). Informs: Work on cores 2-3 must not stream through L2; contention term required in any model*
- Core 0 chase latency while GPU samples 1024x1024 RGBA8 texture in FCRAM (1 MB / 8 MB chase): **1 MB: 34.6 idle vs 70.1 ns; 8 MB: 194 vs 703 ns (job 196: 35 vs 70, 195 vs 704)** (gpuprobe contention) [device; jobs 192, 196] docs/research/TIMING_MODEL_PLAN.md:198-202 *Supersedes GPU_GUIDANCE rule 25 / gcbench 5 M2 ('GPU does not slow CPU memory', memcpy 414 vs 376 MB/s during fill) for FCRAM texture sampling. VRAM comparison not run (no 4 MB VRAM free). Informs: Move game textures from FCRAM (pica.c C3D_TexInit) to VRAM*
- Core 0 time waiting for GX thread (core 2) to drain: **~11% (up to ~20% by scene; gxwait_pct)** (TP tp850, 300 s) [device; job 184] docs/research/TIMING_MODEL_PLAN.md:184 *Informs: Core 2 is a co-limit; GX must scale ~5x with core 0*
- Core 0 to core 2 handoff round trip: **Spin handoff: 907 ns (10-10) and 920 ns (10-11), 20,000 of 20,000 answered. LightEvent handoff: 100,009,831 ns (100 ms), only 3 of 2,000 answered.** (hwprobe) [device; 2026-10-10/11] core/out/hw_2026-10-10.txt, hw_2026-10-11.txt *Informs: Cross-core signalling has to spin. Kernel events are unusable at per-draw or per-job rates.*
- Core 0 wait for core 2 growing as core 0 sped up: **6.9% (morning) -> 9.7% (slow-path stubs) -> 11.5% (budget at exit) -> 12.0% (3a1696f); GX thread ~80% busy at ~50,000 draws/s** [device; 2026-10-12] docs/HANDOFF.md L640-644 *Informs: core 2 co-bottleneck -> draw merging*
- Core 1 (system core) usable fraction with APT_SetAppCpuTimeLimit at 80%: **~78% usable** (New 2DS XL; APT limit 80%; SDK range is 5-89%, cannot go back to 0 once set, >30% helps only on New 3DS) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:269-271 (quoted from FINDINGS.md) *Quoted, not re-derived here. ARCHITECTURE maps AX audio HLE onto core 1 at ~78% (line 423). Informs: What work can move off core 0 onto core 1 (audio mix, helpers)*
- Core 2 parallelism: **fully parallel (no contention measured)** (needs exheader kernel flag 0x2000) [device; quoted from FINDINGS.md (~2026-10-10)] docs/ARCHITECTURE.md:272-273 (quoted from FINDINGS.md) *Qualitative. The GX thread runs here today. Informs: Where the GX thread lives*
- Core 2 speed vs core 0; cross-core handoff: **core 2 runs our code at 95% of core 0; spin-flag handoff ~0.9 us; LightEvent wake-ups across cores unreliable (3 of 2000)** (hwprobe on the New 2DS XL) [device] core/host3ds/gxthread.c:1-5 *timingprobe now has a separate LightEvent round-trip case (kernel_switch). Its results are not in source. Informs: GX thread polls instead of using events*
- Core 2 vs core 0 speed and cross-core handoff: **Core 2 runs our threads at 95% of core 0's speed; spin handoff ~0.9 us; LightEvent cross-core wakes mostly timed out; the title scene split about evenly between GX and CPU emulation** [device; 2026-10-10] git 068ae2f body (hw.txt self-test) *Informs: Use spin handoffs, not kernel events; put GX on core 2*
- Core and thread load in the cutscene, recent builds: **Core 0 about 98% busy. GX thread 78-82% busy. Vertex worker 58-64%. GX thread's wait for the worker (vwait / valloc) 20-26%. gxwait 6-14%. Later stretch: GX thread about 40%, worker 26%, gxwait about 0.5%.** (tp850) [device; 2026-10-12/13] core/out/relay_83-99.csv; r117a-r134b *Informs: Both core 0 and core 2 sit near their limits in the cutscene. Core 0 alone limits the later stretch.*
- Core availability: **core 2 runs JIT code at full speed in parallel with core 0; core 1 at 80% app time limit delivers ~78% of a core** (2DS gcbench) [device; run 2] docs/FINDINGS.md:88-89; REDESIGN_CANDIDATES.md:31 *C2 states ~0.75 of core 0's speed for core 1. Informs: Thread placement*
- Cross-core L2 contention: core 0 chase while core 2 streams stores over 4 MB (cycles per load, alone -> contended): **256 KB 28.4 -> 36.0-36.4; 1 MB 27.7 -> 116.7-123.6; 8 MB 155.6 -> 602.9-605.8** (core 2 writing 4 MB in a loop) [device; 2026-10-15, jobs 176-196] relay_176.csv, relay_177.csv, relay_178.csv, relay_192.csv, relay_196.csv '# timing xcore' *Informs: The shared L2 is evicted by another core's streaming: GX/vertex-worker output streams slow core 0's JIT data (C2/C15)*
- Cross-core spin handoff (FINDINGS): **~0.9 us** [device; before 2026-10-11] docs/research/MEASURE_PLAN.md:80 *Later M6 ping-pong 453 ns.*
- GX on core 2 vs inline (068ae2f): **speed 4.2-4.9% inline -> 6.6-8.2% on core 2; core 2 busy 67-86%; core 0 waiting for GX 15-37%; vertices 49-61% of core 2 wall (lighting 25-30%); PICA draws ~2.4 ms/frame** [device; 2026-10-10/11, 068ae2f] docs/HANDOFF.md L329-336 *Informs: dual-core GX kept*
- GX thread on core 2: **Title scene 4.5% -> 7.5%** [device; 2026-10-10] git 267a391 subject *Informs: Dual-core GX kept*
- GX thread on core 2 (first build): **Heavy speed 7.44% and 7.41%. Core 0's own speed 10.61% and 9.97%. Core 0 waited on GX (gxwait) 29.8% and 24.6%. GX thread 76-77% busy. That is +52-58% over inline GX.** (Title from boot, gx core 2) [device; 2026-10-10] core/out/perf_2026-10-10_run3_gxcore2.csv (2 runs) *Informs: Keep GX on its own core*
- GX thread runs queued vertex jobs while it would wait ('steal') A/B: **core0 cutscene 24.29 -> 23.12 (-4.8 %), later medians 12.31 -> 11.72; ring wait 21.1 -> 16.0 %, core 0 GX wait 10.0 -> 8.8 %; SCU core-2 misses 1.19 -> 1.93 M/s** (600 s) [device; job 164 (2026-10-15)] core/out/relay_164.csv, ab_split.py / gx_split.py --col steal *Later-stretch trimmed means are pulled down by hitches (10.42 vs 9.92) Informs: Rejected: moving gather work between cores does not pay*
- GX thread stealing vertex jobs (C15, job 164): **~400 jobs/s stolen; ring wait 21 -> 16%, core 0's GX wait 10 -> 8.8%; core 0 slower: cutscene 25.37 -> 24.52, later 12.31 -> 11.72 (medians)** [device; 2026-10-15] git c089ecc diff *Informs: Rejected: moving gathers around costs core 0 its shared L2*
- GX thread stealing worker jobs (--steal, job 164): **~400 jobs/s stolen; ring wait 21 -> 16%, core 0 wait 10 -> 8.8%; but core 0 medians cutscene 25.37 -> 24.52, later 12.31 -> 11.72** [device; 2026-10-15] docs/HANDOFF.md L1069-1072 *Informs: rejected; moving gathers costs core 0 its shared L2*
- GX thread waiting for job-ring space vs worker busy: **GX thread waited ~23% of its time for ring space while the worker was 61% busy** (TP) [device; job 163] core/src/gx/pica/pica.h:88-90 *--steal defaults to off. No A/B result is in source. Informs: steal option (GX thread runs queued jobs while waiting)*
- hw.txt cross-core primitives: **tick read 0.25 us; spin handoff core 0 <-> 2: 0.9 us; LightEvent across cores unreliable** [device; 2026-10-10/11] docs/HANDOFF.md L349-351
- JIT int run time per core, and core 1 limit: **core 2 alone 0.27 s; core 0 + core 2 together 0.27 s wall (no interference); core 1 at the 80% limit 0.35 s (77% of core 2)** (gcbench 3, L1-resident JIT int kernel) [device; 2026-10-10] bench/results/latest.txt L118-121; gcbench4 L229-235 *L1-resident only. The shared-L2 interference shown by timingprobe (core 2 streaming makes the core 0 8 MB chase 604 vs 156 cycles) does not appear here Informs: Placement of the GX thread and worker, and whether core 1 is usable*
- LightEvent ping-pong core 0 <-> core 2: **FAILED: timeout at trip 3 (gcbench 3) and at trip 4 (gcbench 4)** (sys.c sync test) [device; 2026-10-10 / 2026-10-11] bench/results/latest.txt L171; gcbench4 L312 *Reproducible. Consistent with C3's lost kernel wake-ups (about 1 in 4) Informs: Do not build cross-core handoffs on LightEvent or kernel wake-ups without a timeout fallback*
- M2 memcpy while GPU fills: **414 MB/s busy vs 376 idle: no contention** (gcbench 5, GPU fill (not FCRAM texture sampling)) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:53; docs/research/GPU_GUIDANCE.md:104 *Superseded/limited by gpuprobe jobs 192/196: GPU sampling an FCRAM texture doubles-to-3.6x core 0 chase latency.*
- M6 cache-line ping-pong core 0 <-> core 2: **453 ns per round trip (about 364 cycles)** (gcbench 4 wave 2) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L590-592 *Consistent with the spin handoff of 0.5 us (latest.txt L172) Informs: Fine-grained cross-core handoff costs about 0.5 us, which limits how often the CPU thread and the GX thread can sync*
- M6 ping-pong round trip core 0 <-> core 2 (spin): **453 ns** (gcbench 4) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:20; docs/research/GPU_GUIDANCE.md:77 *Earlier FINDINGS spin handoff ~0.9 us; kernel thread round trip 2.21 us (job 178). Informs: Handoff granularity between cores*
- M6b false sharing: counters on one 32-byte line vs separate lines: **75 M/s per core vs 160 M/s** (gcbench 4b) [device; 2026-10-11] docs/research/MEASURE_PLAN.md:36; docs/research/GPU_GUIDANCE.md:78-79 *Informs: One cache line per core's hot fields*
- M6b false sharing: counters on one cache line vs separate lines: **same 32-byte line: core 0 75.3 M/s, core 2 75.6 M/s; separate lines: core 0 160.5, core 2 161.1 M/s (2.1x)** (gcbench 4b follow-up mode f) [device; 2026-10-11] bench/results/gcbench4b_2026-10-11.txt L51-55 *Supersedes the gcbench 4 run (below), which was invalid Informs: Redesign C7: put g_vwHead and g_vwTail on separate cache lines*
- Moving vertex gather to the core-3 worker: **14.2% vs 14.8% (slower); every batch flush is a GX-thread/worker join** (TP title scene) [device; 2026-10-11] memory/gc3ds-gpu-direction.md:15-16 *Informs: Core shuffling fails; move work to the GPU instead*
- Relative throughput of cores vs core 0 (work in 50 ms): **core 1 0.69-0.76, core 2 0.87-0.96, core 3 0.85-0.93** (2DS startup probe in every run, jobs 86-94) [device; 2026-10-13] relay_86a..94a.csv '# cores' line *Short probe at startup. Informs: core 1 (system core) gives ~70-76% of a core; cores 2/3 ~90%*
- Relative work of cores 1-3 vs core 0 at startup ('cores (work in 50 ms vs core 0)'): **core 1 0.67-0.75, core 2 0.83-0.96, core 3 0.83-0.92** (startup probe, before the game runs) [device; jobs 101-170] '# cores' header line in ~60 relay runs 101-170 *Spread across runs ~0.1 Informs: Core 1 (OS share) is ~30 % weaker; cores 2-3 ~10 % weaker than core 0*
- Relative work per core in 50 ms vs core 0 (startup calibration line): **core 1 0.67-0.75; core 2 0.85-0.95; core 3 0.83-0.92 (over ~25 jobs)** (measured at start of each run with the app's own threads present) [device; 2026-10-15] '# cores (work in 50 ms vs core 0)' line in every TP-mode relay log 171-196 *Informs: Core 1 (system-shared) gives ~70% of a core; cores 2/3 ~85-95%. Bears on moving work to core 1 (vw2) and on worker placement.*
- SCU traffic during TP: core 0 misses, core 2 misses, reads, writes (per second): **core 0 1.09-1.19 M; core 2 1.13-1.24 M; reads 14.1 M; writes 4.3 M (cutscene); later 10.3 M / 3.0 M** (tp850) [device; 2026-10-15, job 184] relay_184.csv columns scu_c0miss_k, scu_c2miss_k, scu_rd_k, scu_wr_k *Informs: Size of cross-core L2 pressure from the GX side*
- Second vertex worker (C2) and GX-thread job stealing (C15): **Core 0 slower (shared 2 MB L2)** (tp850) [device; 2026-10-15, job 164] memory/gc3ds-jit-dead-ends.md:24-25 (job 164) *Informs: Rejected*
- Second vertex worker on core 1: **Core 0 -6% (evicts core 0's L2)** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:18-19 *Informs: Rejected; shared 2 MB L2 limits multicore scaling*
- Second vertex worker on core 1 (C2): **Job 125: core 0 own speed cutscene 22.77 -> 21.16, core-0 section 11.70 -> 11.36 +-0.05 though GX waited half as much. Job 127 (sleeping helper, padded counters): cutscene 20.29 -> 19.87, core 0 22.66 -> 21.24 (-6%), 11.75 -> 11.19** [device; 2026-10-13] git d1b179b body; 108dde0 diff (REDESIGN_CANDIDATES) *Informs: Rejected: gathers evict core 0's L2 working set*
- Second vertex worker on core 1 (vw2), in-run A/B, two runs: **Cutscene speed: 21.06 → 20.43% and 21.20 → 20.42%. Core 0 own: 23.76 → 22.01% and 23.84 → 22.09% (-7.4%). Later: 11.73 → 11.41% and 11.72 → 11.41% (-2.7%). Worker busy 55 → 44%.** (tp850) [device; relay jobs 125, 127, 2026-10-13] core/out/r125a.csv, r125b.csv, r127a.csv, r127b.csv; checked with ab_split.py on relay_125/127 *Informs: Rejected. The second worker's gathers evict core 0's working set from the shared L2.*
- Second vertex worker on core 1 (vw2), job 125: **helper took ~820 jobs/s (a third); core 0 GX wait 10.0% -> 5.7%; GX ring wait 21% -> 12.5%; but cutscene speed 20.28% -> 19.83%, core 0 22.77% -> 21.16%, core-0-bound section 11.70% -> 11.36% (+-0.05)** (2DS, --ab vw2, 400 s) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:36-41 *Informs: Rejected (see job 127)*
- Second vertex worker on core 1 A/B: **125: core0 cutscene 23.76 -> 22.01 (-7.4 %), later 11.79 -> 11.46 (-2.8 %); 127: 23.84 -> 22.09 (-7.3 %), 11.79 -> 11.47; helper took 814-850 jobs/s; core 0 GX wait 9.0 -> 5.6 %, ring wait 20.7 -> 12.8 % (127)** (400 s each; 127 helper sleeps, counters padded) [device; jobs 125-127 (2026-10-13)] core/out/relay_125.csv, relay_127.csv, ab_split.py / gx_split.py --col vw2 *126 (147 s, log cut by 429): 25.56 -> 23.41 (-8.4 %) Informs: Rejected: more parallel gather costs core 0*
- Second vertex worker on core 1 and shared-index fast path: **second worker: core 0 -6% (gathers evict core 0's working set from shared 2 MB L2), jobs 125/127; shared-index fast path: <1% of indexed vertices qualify (job 127)** [device; 2026-10-13] docs/HANDOFF.md L951-953 *Informs: rejected; L2 contention is a real cross-core cost*
- Second vertex worker on core 1 spinning: **core 0 ran 3-7% slower with the helper spinning; app gets core 1 at 80%** (--vw2) [device; job 125] core/src/gx/pica/pica.c:1230-1231, 1245-1247 *Cache-line ping-pong on the queue counters. The worker now sleeps. vw2 defaults to off and no positive result is in source. Informs: No spinning; vw2 off*
- Second worker that sleeps instead of polling (vw2 variant): **Speed 21.87 vs 22.04%. Core 0 own 25.56 → 23.48% (-8.1%).** (tp850 cutscene, 147 s) [device; relay job 126, 2026-10-13] core/out/r126a.csv (r126b is empty: the 2DS slept) *Interrupted run Informs: The slowdown is not from polling*
- Shared-L2 contention signature: **with vw2 on, core 0's I-misses stay flat (6.42 -> 6.23 M/s) and D-read misses flat (1.09 -> 1.05 M/s), but fetch-stall cycles rise 189 -> 218 M/s (cycles per I-miss ~29.4 -> ~35.0, +19 %); steal (164): 29.8 -> 31.6** (cutscene) [device; jobs 125-164] relay_125/127/164.csv, ab_split.py and per-arm column means *Cycles per miss derived by me; L2 misses are not counted directly Informs: Other cores streaming guest RAM slow core 0's code fetch through the shared L2*
- Spin handoff core 0 <-> core 2: **median 0.5 us (min 0.5, p90 0.5)** (gcbench 3/4) [device; 2026-10-10] bench/results/latest.txt L172; gcbench4 L314 *Informs: Spinning is the only reliable fast cross-core handoff*
- Title scene speed by build (each its own run; A/B halves where given): vertex worker, shader lighting: **ab635d0 vertex worker on core 3: 9.9% (off 8.3%); e4fa5a1 no CPU light setup for shader-lit draws: 11.6% (worker off 9.4%)** [device; 2026-10-11] docs/HANDOFF.md L150-153 *Informs: core 3 worker and shader lighting kept*
- vw2 with sleeping helper and padded counters, job 127: **cutscene 20.29% -> 19.87%; core 0 22.66% -> 21.24% (-6%); core-0-bound 11.75% -> 11.19%** (2DS, --ab vw2) [device; 2026-10-13] docs/research/REDESIGN_CANDIDATES.md:42-45 *Attributed to helper's gathers evicting core 0's working set from the shared 2 MB L2 (inference, not directly measured by an L2 counter). Informs: C2 rejected; same L2 tax likely paid by core-3 worker*
- Work done by threads on cores 1/2/3 relative to core 0: **75% / 95% / 92%** [device; 2026-10-10] docs/HANDOFF.md L359-360 *Informs: core 2 near full speed; core 1 weaker (system)*
- Work each core does in 50 ms, relative to core 0 (# cores line in every run): **Core 1: 0.69-0.75. Core 2: 0.87-0.96. Core 3: 0.85-0.95.** (Measured at the start of each run) [device; 2026-10-12/13] '# cores (work in 50 ms vs core 0)' in relay_3-99 (most common: c1 0.74 c2 0.94 c3 0.91) *Varies by about 5% from run to run Informs: How much work can move to cores 1-3. Core 1 gives about 72% of a core.*
- Azahar: core handoff: **Spin round trip 600,610 ns (333/334 answered); LightEvent 100,037,639 ns per round trip (3/2000 answered)** (Azahar) [azahar; 2026-10-15] azahar-sdmc/hw.txt:7-8 *Device spin handoff is 0.5 us Informs: LightEvent failure is reproduced off-device -> likely a probe/code bug*
- Azahar: work per core in 50 ms vs core 0: **c1 0.80, c2 1.00, c3 1.00** (Azahar) [azahar; 2026-10-15] azahar-sdmc/perf.csv '# cores' lines *Device value not in my sources Informs: Core 1 time limit modelled at 80% in Azahar*
- Core 0 alone vs game speed in cutscene: **core 0 alone ~25%, game ~21.5%; core 0 waits GX ~11%; GX side worth up to ~+15% there; core-0-bound stretch: remaining JIT items ~1% each** [estimate; 2026-10-14] docs/HANDOFF.md L1057-1059 *Informs: owner: GX side next, then JIT rebuild*
- Core 3 availability to applications: **SDK says 'Normal applications cannot create threads on this core'** (libctru thread.h statement) [estimate; 2026-10-10 source fetch] docs/ARCHITECTURE.md:274 *Conflicts with current practice: the vertex worker runs on core 3 (NATIVE_PORT_PLAN.md:44, :125). ARCHITECTURE.md was never updated. These docs give no measured usable fraction for core 3. Informs: Whether vertex work on core 3 is a dependable resource*

### Twilight Princess workload (72)

- Baseline of the 2026-10-13 night defaults (no A/B): **Cutscene (all rows from 25 s): 20.53%, core 0 23.60%, gxwait 10.8%, valloc 21.4%, worker 57%. Later stretch, heavy rows: 11.80%, core 0 11.87%, IPC 0.47 (379 / 804), I-cache misses 4.26 M/s.** (tp850, 600 s, opt=3) [device; relay job 130, 2026-10-13] core/out/r130a.csv, r130b.csv *HANDOFF quotes the cutscene as 20.06 ± 0.26 with core 0 22.83; its later-stretch figures match Informs: The baseline later builds compare against*
- Clean-run baseline with 2026-10-14 defaults (opt 115: hoist, kstore, retstub, syscall, hlelink): **cutscene speed 22.03 +-0.15 % (core0 25.19); later stretch 12.45 +-0.04 % (median 12.73, core0 12.52); movie 4.35 %** (tp850, 600 s, no A/B, new exit counters on) [device; job 157 (2026-10-14 night)] core/out/relay_157.csv (clean windows 30-250 / 265-475 / 500-600 s) *Single run; trimmed means Informs: Current-JIT baseline before NATIVE2/LATCH*
- Clean-run baseline, oldest build in range (900 s): **cutscene 21.39 % (core0 24.25); later 11.63 % (core0 11.69); movie 510-900 s 4.38 %** (tp850, opt=1, 900 s, no A/B) [device; job 101 (2026-10-13)] core/out/relay_101.csv *Informs: Shows build-to-build drift 101->157 is ~+3-4 % core0 cutscene, +7 % later*
- Clean-run baseline, tp850 state, defaults of 2026-10-13 night (no A/B): game speed and core 0's own speed per section: **cutscene (30-250 s) speed 21.83 +-0.15 % (median 22.14), core0 25.06 +-0.12 %, gxwait 12.97 %; later Link/Epona stretch (265-475 s) 11.81 +-0.04 % (core0 11.87, gxwait 0.55 %); movie section (500-600 s) 4.62 %** (TP tp850.state, 600 s, opt=3 (hoist+kstore), no A/B) [device; job 130 (2026-10-13 night)] core/out/relay_130.csv, re-analysed with ab_split.py stat() over clean-run windows *HANDOFF quotes 20.06 % / core0 22.83 % (cutscene) and 11.80 % (later) for this job using 0-300 s / 300-620 s windows; in a clean run the cutscene ends at ~253 s real, so a 300 s window mixes sections. core0 = speed/(1-gxwait). Informs: Reference point every later build/redesign is compared against*
- consoletest and 3D test speed on the 2DS: **consoletest 99.7%; 3D ~5% with the software renderer** (Early JIT, soft.c renderer) [device; 2026-10-10] memory/tp3ds-gc-wii-on-2ds.md:60 *Superseded by the PICA backend (test DOLs ~99% in Azahar) Informs: Motivated the PICA200 backend*
- D1: display-list share and repetition: **99.6% of draws inside display lists; 2,700-3,700 list calls/s; 87-91% of sampled calls same list bytes as last time; 79-85% same bytes and array bases** (2DS job 107, tp850 cutscene, 6 x 10 s, # dlist 10s) [device; job 107] docs/research/PERF_LEVERS.md:91; DL_CACHE_PLAN.md:11-12 *Array contents (CPU skinning) not measured. Informs: Display-list caching (C1/C3)*
- D3: JIT profile flatness: **Top 200 blocks = 5.4% of core 0; hottest TP function J3DUClipper::clip 0.49%; mDoMtx/J3D matrix functions 0.1-0.2% each** (2DS job 107, --profile on, hot_funcs.py with zeldaret/tp symbols) [device; job 107] docs/research/PERF_LEVERS.md:47,93 *Informs: Lever 5 (HLE matrix chain) dropped; also argues little idle-loop time (C10)*
- Device block profile outside shadow scene: **No compiled block holds more than 0.3% of samples** (2DS --profile on) [device; 2026-10-14] docs/research/AOT_PLAN.md:88-90 *Informs: Single-function stand-ins have no target*
- Device profile flatness: **No block >0.3% outside the shadow scene; no TP function >~0.25% outside the shadow scan** (Hot guest code sampler, tp850) [device; 2026-10-14/15] memory/gc3ds-aot-verdict.md:24; memory/gc3ds-jit-dead-ends.md:30 *Informs: Single-function stand-ins have no target; whole-JIT or native approaches needed*
- Device profile flatness: **hottest compiled block 0.2-0.4 % of samples per 10 s; 16-39 % of samples outside compiled code** (tp850 cutscene) [device; job 107] '# hot guest code 10s' lines, relay_107.csv (--profile on) *Informs: No single function is worth a stand-in*
- Display-list behaviour in TP: **3,126 list calls/s; 99.5% of draws inside lists; 263 new list addresses/s; 90% of sampled calls have the same bytes, 83% the same array bases too** (tp850) [device; 2026-10-15, job 184] relay_184.csv averaged '# dlist 10s' lines *Informs: Display-list result caching (dlcache) potential, and converting models once*
- Early TP speed on the 2DS: **triangle 99.7%; TP's first screen 1.4% (f410750). With the polling-loop skip, logos at 68-69%, but the title scene recompiled ~1M blocks (105 flushes of a 4 MB buffer) and ran at 1.9% (378a47f), which led to the 16 MB buffer** [device; 2026-10-10] git f410750, 378a47f bodies *Informs: JIT buffer size*
- End-of-10-11 baseline ('fixed' build), no A/B: **Title heavy, first 295 s: 16.31% (core 0 17.31%, GX thread 78%). After 311 s: 11.73% (core 0 12.36%).** (Title from boot, all A/B options on) [device; 2026-10-11 evening] core/out/perf_2026-10-12_hle2.csv run 21 (= perf_2026-10-11_fixed.csv) *Informs: Starting point for the relay era (about 16%)*
- First perf.csv title scene (GX inline): **4.2-4.9% speed, ~330 ms per field alternating ~650 ms and light fields (TP 30 fps); ~50% GX on CPU (vtx 38%, lighting 19%), ~48% CPU emulation; ~12.8k draws/s -> ~140 C3D_DrawArrays/s; GPU ~2.3 ms/frame** [device; 2026-10-10, perf_2026-10-10_run1.csv] docs/HANDOFF.md L361-366 *c3d_cpu_ms meaningless (frames span a whole field)*
- Guest instructions executed per field: **device: 1.68-1.70 M (ins_k 19,252-21,898 k/s at 11.45-12.92 fields/s, jobs 86/91/93 cutscene); qemu: 1.48 M (50 fields), 1.50 M (100), 1.49 M (30), 1.52 M (20); PC: 1.63 M (900-1020), 1.73 M (900-1499)** (TP gameplay from field 850/900; title/boot stretches lower (~0.66 M/field in job 94)) [device; 2026-10-12 to 2026-10-15] relay_86a/91a/93a.csv (ins_k, fields_s), nohle_on.txt, fromstate_on.txt, dynop.txt, aot/cs/jit.out, aot/hist_900.bin, opmix/out.txt *ins_k counts instructions run by compiled blocks; idle polling loops are skipped, so this is post-idle-skip demand. Informs: full speed needs ~100 M guest instructions/s on core 0 (1.7 M x 60)*
- Guest profile flatness: **top 200 blocks 13.5% (title), 6.2% (cutscene) of core 0 (relay 72-73); no function >~0.5% except DVD Read while loading; D3 job: top 200 blocks 5.4%; job 146: no TP function >~0.25% outside shadow scene** [device; 2026-10-12 to 2026-10-14] docs/HANDOFF.md L771-774, L906-907, L1055-1056 *Informs: native stand-ins for single functions cannot give big steps*
- Guest profile flatness (--profile on): **The 200 hottest blocks are 6.0% of samples in the title (median; 13.2% in the first, loading interval) and 6.3% in the cutscene (4.4-7.1%). Outside compiled code: 5.6-7.5% of samples.** (Title / tp850) [device; relay jobs 72-73, 2026-10-12] core/out/relay_72.csv, relay_73.csv ('# hot guest code 10s', summed) *relay_72 and relay_73 each hold their log twice Informs: No single-function stand-in can give a big step. Per-instruction cost is the lever.*
- Guest work per field, cutscene and title: **Cutscene: 21.9 M guest instructions/s at 21.48% speed, which is 102 M per emulated second (21% of Gekko's 486 MHz) and 1.70 M per field. Title: 1.65-1.67 M per field. Link/Epona scene: 486.0 M per emulated second, 9.89 M per field.** (Heavy rows) [device; 2026-10-12/13] Derived: ins_k / (speed_pct/100) and ins_k / fields_s over relay_83, 84, 46, 8 and perf_2026-10-12_hle2 run 24 (from 437 s) *Derived from device counters Informs: Full speed in the cutscene needs about 102 M guest instructions/s: 4.7x core 0's current 21.9 M*
- GX draw workload in the TP cutscene: **~58-62k GX draws/s and ~300-330k vertices/s. Merged: 96.3% join an open draw, giving 2,303 jobs/s (26.9 draws each). Batch fast path 66%. 5,958 vertex setups per 23,030 batches per 10 s. TEV: 540 program changes/s, 59 compiled/s. Palette uploads ~4,000/s.** (tp850) [device; 2026-10-15, job 184] relay_184.csv draws_s/verts_s columns and averaged '# merge', '# gx draws', '# tev', '# gpu transform' lines (26 reports, 30-300 s) *Informs: Sizing GX/PICA backend redesign (per-draw vs per-job costs)*
- HLE stand-in A/B in the Link/Epona cutscene (from 437 s), two runs: **Run 1: speed 8.52 → 9.69%, but fields/s 5.09 → 3.85. Run 2: 8.65 → 9.70%, fields/s 5.19 → 3.87 (-25%). Title loop: 16.02 → 16.65%, fields 9.60 → 9.98. With the stand-in on: about 14-17k calls/s standing in for 31-37 M guest instructions/s.** (Title from boot, 20 s phases) [device; 2026-10-12] core/out/perf_2026-10-12_hle2.csv runs 23 (= perf_2026-10-12_hle.csv) and 24 (hle2); '# hle 10s' lines *Run 2 is presumably after the budget fix in HANDOFF, yet the field-rate drop remains. Not reconciled. Informs: Judge stand-ins by fields/s, not speed_pct. The stand-in is off by default.*
- HLE stand-in A/B in the tp850 cutscene: **Off: 16.64% speed, 9.97 fields/s, core 0 17.51%. On: 16.11%, 9.65 fields/s, 17.27%.** (tp850 state, about 300 s) [device; relay job 7, 2026-10-12] core/out/relay_7.csv (heavy rows, 4 s settle) *HANDOFF calls this job 6. relay_6.csv is empty. Informs: The stand-in costs about 3% in this scene. Rejected.*
- Hottest guest code: **title scene '# hot guest code' ~30% of samples: dBgWKCol::ShdwDraw (shadow projection, PSVEC calls); later analysis: only the Link/Epona cutscene has a hot spot, ShdwDraw 12.5%** [device; 2026-10-11/12] docs/HANDOFF.md L452-456, L500-502 *L452 and L234/L771 disagree on the title scene (elsewhere top block 0.2-0.3%); 30% may be the share of samples examined, not ShdwDraw alone Informs: HLE stand-in for ShdwDraw*
- Later core-0-bound section of tp850 run: **after ~300 s: ~11% speed with half the draws and gxwait ~0** [device; 2026-10-13, job 97] docs/HANDOFF.md L887-888 *Informs: two regimes: GX-bound cutscene and core-0-bound later stretch*
- Light workloads reach full speed: **light screen (~180 draws/s) ran steady 60 fields/s for a minute (job 46); consoletest 99.7%** [device; 2026-10-10 / 2026-10-12] docs/HANDOFF.md L17, L631-632 *Informs: pipeline overhead is not a floor; cost scales with guest work*
- Presented vs emulated fields in TP cutscene: **Job 184 (30-250 s): 13.3 fields/s emulated, 8.8 shown, 4.4 dropped. Job 185: 12.4 emulated, 6.3 shown, 6.0 dropped.** (tp850) [device; 2026-10-15] relay_184.csv, relay_185.csv columns fields_s, shown_s, dropped_s *Informs: Presentation drops ~1/3-1/2 of fields; the visible frame rate is lower than speed_pct implies*
- Real seconds to reach fixed emulated points from tp850 (end-to-end throughput benchmark): **emu 68.3 s (cutscene end): 258.1 s (job 101), 252.7 (130), 252.7 (157); emu 96.5 s (movie start): 499.1 / 491.8 / 479.5 s. A/B runs: 302.9-305.5 s and 576.6-587.2 s** (same state, same scene sequence) [device; jobs 101-170] core/out/relay_101/130/157/148/151/160/170.csv emu_s column *Derived by me from emu_s; 4 % end-to-end gain from 101 to 157 Informs: A deterministic, low-noise whole-run metric usable alongside per-section A/B*
- Shadow-scan HLE stand-in, first device build: **TP froze. Mean speed 69.7% with about 5,000 draws/s over 147 s: no game progress.** (Title from boot, hle A/B) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv run 22 (= perf_2026-10-11_hle.csv) *Fixed later Informs: An interpreter-only check misses JIT-path bugs (stale g->pc)*
- Shadow-scan share of core 0 (dBgWKCol::ShdwDraw): **~33% of core 0 on the title scene; HLE_CHECK 6,105 calls identical** [device; 2026-10-11] git 9d7c239 body *Informs: Native stand-in*
- Shadow-scan stand-in (hle_tp.c) gain: **Link/Epona stretch +12-17%; stand-in runs ~5 cycles per guest instruction (memory bound, near its limit)** (tp850, default on) [device; 2026-10-14] memory/gc3ds-aot-verdict.md:20-21 *Near its limit Informs: HLE stand-ins pay where a hotspot exists*
- Shadow-scan stand-in A/B (jobs 145-146): **Link / Epona stretch 10.6-10.7% -> 11.9-12.4% (fields 6.36 -> 7.39/s); cutscene within noise (20.2 / 19.6%); the stand-in still ~22% of core 0's samples there, ~5 cycles per guest instruction it replaces (memory-bound); its tight loop changed nothing** [device; 2026-10-14] git 051987d body; git 0903f5f diff (AOT_PLAN.md) *Informs: Stand-in on by default; collision-mesh memory bound*
- Speed progression on the device, tp850 cutscene and title: **Cutscene: 15.09% (job 4, traces-off arm) → 16.0% (jobs 7, 51) → 18.2% (merge, job 53) → 19.5% (job 60) → 21.0-21.7% (jobs 68-83) → core 0 own about 24.9% (jobs 132-134). Title heavy rows: 16.3% (job 8) → 18.9% (job 52) → 21.1% (job 71).** (tp850 / title) [device; 2026-10-12 to 2026-10-13] core/out/relay_4-99, r117-r134 *Mixed run lengths and filters Informs: Tuning gave about +40% in two days. Each remaining lever measured at 1-5%.*
- Title from boot, no A/B, 420 s: **Heavy rows in the first 295 s: 16.32% (core 0 17.51%, gxwait 6.9%, GX thread 78.8%, vertex worker 46%). After 311 s: 15.03%.** (Title from boot, GPU queue at 0) [device; relay job 8, 2026-10-12] core/out/relay_8.csv *Informs: The relay-era title baseline*
- Title-scene speed history on the 2DS: **1.9% (4 MB JIT code space overflowing) -> 3% (16 MB JIT) -> 3.1% (compiled vertex path + draw batching) -> 3.3% -> 5.1% (float JIT b614a7a) -> ~15% (10-11)** (TP title scene, successive builds) [device; 2026-10-10 to 2026-10-11] memory/tp3ds-gc-wii-on-2ds.md:71-92; memory/gc3ds-gpu-direction.md:12 *Each step is a different build; not A/B within one run Informs: Which levers moved the needle (JIT space, float JIT, GPU backend) and which did not (batching)*
- TP clean run speed (no A/B), state tp850: overall and by stretch: **300 s mean 20.3%, median 21.5%; cutscene 30-250 s median 22.2% (13.3 fields/s, field 72-79 ms); after ~250 s median 12.9% (7.7 fields/s)** (JIT on, GX thread core 2, vertex worker core 3, shader lighting, PMU on, current defaults) [device; 2026-10-15, job 184] relay_184.csv perf rows (speed_pct, fields_s, field_ms_avg) *speed_pct = emulated Gekko cycles / 486 MHz. Scene-dependent; tp850 is the only scene measured. Informs: Baseline for the redesign: ~20% in the GX-heavy cutscene, ~12.5% in the later stretch*
- TP logos speed after polling-loop skip: **68-69%** (TP logos, polling-loop skip on) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:70 *Logo scene is light; not representative of 3D scenes Informs: Idle-loop skipping is worth keeping*
- TP overall speed under the JIT (baseline arm of the AOT A/B): **Cutscene 20.2%, later stretch 12.8%** (tp850, 20 s A/B phases, JIT arm) [device; 2026-10-14, job 144] memory/gc3ds-aot-verdict.md:13-14 (job 144) *A/B inside one run; runs of the same build vary ~5% Informs: Baseline from which every later JIT gain is measured*
- TP vertex rate and per-field vertices: **~300k vertices/s at ~12 fields/s; ~25k vertices per field; ~3 MB of VIn per field** (2DS TP) [device; 2026-10-13] docs/DL_CACHE_PLAN.md:47-48; REDESIGN_CANDIDATES.md:21 *Rounded figures. Informs: Memory sizing for vertex caches / GPU-resident buffers*
- AOT coverage of TP executed instructions: **189.7 M of 196.2 M (96.7%) translated; REL module code ~2% untranslated; 13,704 functions found in main.dol without symbols** (PC, TP from field 900, 120 fields) [pc; 2026-10-14] docs/research/AOT_PLAN.md:45-50,104 *Informs: main.dol dominates TP's executed code; RELs ~2%*
- Draws per field and vertices per draw, title scene: **~4,400 draws/field of ~5 vertices (PC: 1.3M draws by field 900); PC 1,500 fields: 4.67M draws, 23.2M vertices; 2DS ~44k draws/s** [pc; 2026-10-10 / 2026-10-11 b439173] docs/HANDOFF.md L88, L192-193 *Informs: per-draw (not per-vertex) costs dominate GX side; draw merging*
- Draws, vertices and EFB copies per field (PC): **900-2500: 9,552,578 draws / 47,131,030 vertices / 6,889 EFB copies over 1,600 fields = 5,970 draws, 29.5k verts, 4.3 copies per field; 900-5800: 4,944 draws, 24.8k verts, 3.7 copies; 900-1499: 7,800 draws, 38.8k verts; strips 99.4-99.7% of primitives** (PC runs hc, li, opmix) [pc; 2026-10-14/15] aot/hc.txt, aot/li/out.txt, opmix/out.txt *Emulated workload, independent of host; scene-dependent. Informs: full speed needs ~300-470k draws/s and 1.5-2.3 M vertices/s through GX*
- Guest idle share (polling-loop skips) by stretch: **9.4% (fields 900-1020), 12.1% (850-1000 qemu), 14.5% (900-1100), 31.4% (900-1499), 43.8% (900-5800), 50.6% (900-2500), 96.6% (boot-1500 title)** (PC and qemu runs) [pc; 2026-10-14/15] aot/fb.txt, aot/ac/out.txt, aot/fa/out.txt, opmix/out.txt, aot/li/out.txt, aot/hc.txt, aot/tc_1.txt *Idle detection is the emulator's polling-loop skip. Informs: CPU demand varies 2x across scenes; benchmarks must name the stretch*
- Guest instruction histogram concentration by TP function (PCHIST, interpreter): **196,183,272 guest instructions, 99,488 distinct words, 2,299 functions; top 10 functions 20.6%, top 50 51.1%, top 100 67.5%, top 200 82.7%, top 500 95.2%, top 1000 99.0%** (PC interpreter, TP from tp_pc_900.state, fields 900-1020 (120 fields; per AOT_PLAN.md), decoded read-only with a scratch script against tp symbols) [pc; 2026-10-14] //wsl$/Debian/root/aot/hist_900.bin (decoded) *Guest instruction counts, not host time. One stretch of the game. Informs: whether per-function translation/HLE can cover the workload: 500 functions are needed for 95%*
- Guest instructions per field, title scene; float share: **~1.55M guest instructions/field, ~15% float/paired-single (later figure: ~1.65M/field)** (PC OPMIX=1 from /root/tp_900.state) [pc; 2026-10-10 (1.55M); 2026-10-12 (1.65M)] docs/HANDOFF.md L81-83, L766 *1.65M figure from a1dd311-era analysis; the two are close but not identical Informs: core-0 throughput needed for full speed*
- Guest opcode mix (dynamic, PC interpreter): **1,037,312,087 instructions: addi 14.56%, lwz 12.85%, bc 8.13%, stw 7.51%, rlwinm 4.24%, b 3.53%, lfs 3.31%, bclr 3.26%, or 3.17%, cmpli 3.01%, stb 2.85%, psq_l 2.22%, cmpi 2.14%, stfs 1.82%, lbz 1.54%, mtspr 1.51%, add 1.33%, addis 1.28%, psq_st 1.27%, stwu 1.23%, lfd 1.19%, fmuls 1.15%** (PC interpreter OPMIX, tp_pc_900.state to field 1499 (599 fields), polling-loop skips 31.4%) [pc; 2026-10-15] //wsl$/Debian/root/opmix/out.txt *opstat/dynop/coldstat/regstat outputs live in //wsl$/Debian/root/*.txt, not under opmix/ (only out.txt is there). Informs: which ops a new JIT must make cheap*
- GX workload in the menus (memory card, name entry, TV settings), fields 1400-2700: **1400-2400: ~353 draws/field (all quads), ~1,411 vertices/field, ~173 KB GX/field. 2400-2700: ~487 draws/field (quads), ~251 KB GX/field** [pc] core/out/tp/card, game, yes logs (cumulative differences); game/_sheet.png, yes/_sheet.png *These menus are unrepresentative of play: 2D quads with few draws but a large stream. Informs: Not a benchmark scene*
- GX workload in the Ordon Spring opening cutscene (Rusl and Link), fields 2700-3300: **~4,433 draws/field, ~23,040 vertices/field, ~120.8 KB GX/field, 4.1 EFB copies/field; line strips (107,712) and points (110,976) appear for the first time (0 before 2700); triangles 104 -> 1,305** [pc] core/out/tp/names/log.txt vs yes/log.txt; names/_sheet.png; cut1/field03300.png *2700-2800 includes black transition frames, so the cutscene proper is denser than this average. gc3ds draws lines and points on the CPU path (git 29a22f7). Informs: A second save state covering a different scene, lines and points*
- GX workload in the title-demo window (Link on Epona), fields 902-960: **~5,090 draws/field, ~25,640 vertices/field (~5.0 per draw), ~90.6 KB GX stream/field, 4.0 EFB copies/field (9 per 30 fps frame: XFB, a 384->192 copy, depth 608x448->304x224, two 608x448->304x224, then 304x224, 152x112 and 76x56 copies), ~60 DSP mails/field** (PC interpreter, deterministic; tp_900.state) [pc] core/out/tp/save900/log.txt and all900/log.txt (cumulative counters subtracted); run5 log (EFB copy list) *I derived these from cumulative end-of-run counters across two runs (902 vs 960 fields). This is the tp850-960 scene all device A/Bs use. Informs: Sizing GX work per frame for any redesign*
- GX workload, fields 690-839 and 875-902: **690-839: ~4,447 draws/field, ~72 KB GX/field. 875-902: ~4,850 draws/field, ~24,450 vertices/field** [pc] core/out/tp/tevcheck_690.txt; run2/log.txt; qtp5/log.txt vs save900/log.txt *Derived across separate runs (the 875 numbers come from the qemu JIT run, 902 from the PC). Event timing can differ slightly between them. Informs: Workload consistency across the tp850 window*
- icbi frequency in TP: **~1.4% of executed instructions; icbi 2.5% of modelled core-0 cycles** (TP (PC/qemu)) [pc; 2026-10-12/13] docs/REGION_JIT_PLAN.md:52; OPT_JIT_PLAN.md:21 *Today any icbi on a page with compiled code flushes everything. Informs: Invalidation granularity*
- libogc example static mix (homebrew, not TP): **branches every 5.6 instructions (median 4, p90 11); lwz/stw 21%; mtmsr/mfmsr 1.4%; float+paired 3.7%** (PC static, 29 libogc examples) [pc; pre 2026-10-10] docs/FINDINGS.md:94-112 *Not a commercial game. Informs: General JIT design*
- Polling-loop share of emulated time, boot to field 690: **168,029 polling-loop skips, 90.8% of emulated time (PC); 89% of loading time in the loop at 0x80341144, skip removes 88% of emulated time (git 09c8bc3)** [pc] core/out/tp/tevcheck_690.txt; git 09c8bc3 body *Informs: Idle and polling skips matter for loads, not for heavy scenes*
- Top TP functions by executed guest instructions: **J3DGDSetLightAttn 3.49%, PSMTXConcat 2.53%, JASTrack::updateSeq 2.32%, J3DGDSetLightPos 2.11%, J3DGDSetLightDir 2.11%, JASChannel::updateDSPChannel 2.06%, cBgS::GroundCross 1.74%, J3DMtxBuffer::calcWeightEnvelopeMtx 1.65%, __save_gpr 1.32%, __restore_gpr 1.32%, J3DGDSetTevColorS10 1.29%, J3DGDSetLightColor 1.24%** (PC interpreter, TP fields 900-1020) [pc; 2026-10-14] aot/hist_900.bin (decoded) *Guest instructions, not ARM cycles. Informs: HLE/native candidates; prologue helpers (__save/__restore_gpr) at 2.6% suggest inlining them in the translator*
- TP guest instruction mix (TP 900, 1.04 G instructions): **loads 22% (lwz 12.9%), stores 15%, conditional branches 8.1%, blr 3.3%, b 3.5%** (PC interpreter) [pc; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:192-193
- TP main.dol static instruction mix: **integer ALU 29.2%, branches 19.8%, integer loads 13.3%, integer stores 10.7%, float load/store 9.7%, float arithmetic 4.5%, compares 4.5%, rotate/shift 3.5%, SPR/MSR 3.4%, paired 0.8%; 900k instructions** (PC static analysis, GZ2E01 rev 0 main.dol; RELs not analysed) [pc; pre 2026-10-10] docs/FINDINGS.md:114-131 *Static, not dynamic. Dynamic data in D2 / OPT_JIT model differ (float ld/st 27.6% of cycles). Informs: Float and memory focus*
- TP static float/call details: **lfs 5.2%, stfs 2.9%, fmuls 0.75%, fsubs 0.68%; paired arithmetic 0.1%; 6,600 psq_l/psq_st (likely FPR save/restore, unchecked); double (lfd/stfd/fmul) ~2%; bl 7.1%, blr 1.9%, bctrl 0.8%; a branch every 5.1 instructions (median 4, p90 9)** (PC static, main.dol) [pc; pre 2026-10-10] docs/FINDINGS.md:133-143 *Informs: C8 single precision (double present: not a free choice); call/return linking*
- TP title GX stream size: **~90 KB of GX commands per field (title scene)** [pc; 2026-10-10] git a3b6498 body *Matches my derived 90.6 KB/field for fields 902-960. Informs: Stream size*
- TP title scene vertex attributes / lights / texgen per vertex: **~5.5 lights evaluated per vertex, 3.6 indexed attributes per vertex, 1.69 texgens per vertex** (PC VFAST_CHECK counts) [pc; 2026-10-10] docs/HANDOFF.md L78-79, L558 *Informs: lighting / texgen on GPU vs CPU*
- What remained untranslated with AOT (hist_rest): **6,489,543 guest instructions (3.3% of 196.2 M); within it: REL module code (no symbol, 80f1e000-80f33000 and 80c5x-80c9x) ~51%, memcpy 6.8%, JASCalc::imixcopy 5.5%, __OSDispatchInterrupt 3.4%, DCInvalidateRange 3.2%, __VIRetraceHandler 2.0%, SelectThread 1.7%** (PC, TP 900-1020 with AOT on) [pc; 2026-10-14] aot/hist_rest.bin (decoded); context AOT_PLAN.md lines 45-50 *Interpretation of hist_rest as 'untranslated remainder' inferred from its total matching AOT_PLAN's 196.2 M - 189.7 M. Informs: coverage limits of ahead-of-time translation from main.dol (REL modules need relocation handling)*
- AOT coverage of executed guest instructions over a long qemu run: **AOT ran 1,431,470,027 guest instructions vs JIT 66,719,117 (95.5% translated); 27,453,341 calls; 7,708,290 fallbacks; 4,544,935 slow accesses** (qemu-arm ARM build, AOT on, tp_arm_850.state to field 1730 (880 fields)) [qemu_model; 2026-10-14] aot/aa/out.txt *Earlier build than fb.txt (fallbacks later cut ~270x); the '20.0% of 7,140,116,923' denominator is cycle-based, not instructions. Informs: AOT coverage achievable; fallback cost*
- Boot-only JIT statistics (not gameplay): **boot to field 150: 210,069,335 instructions, 19,245 blocks, 2 flushes; stfs 24.7% of hot words (15.1 hot + 70.8 cold each); 375 draws** (qemu, opstat) [qemu_model; 2026-10-12] opstat.txt *Informs: boot profiles do not represent gameplay*
- D2: modelled core-0 cycles by class: **float loads/stores 27.6%, float/paired-single maths 15.3% (43% float total), integer loads/stores 29.1%, integer other 17.0%, branches 10.3%** (qemu model, 40 TP fields) [qemu_model; 2026-10-13] docs/research/PERF_LEVERS.md:92 *Informs: Levers 2-3 (single precision, short vectors)*
- Memory instruction share and addressing modes: **memory instructions 40% of TP guest instructions; base: r1+d 21%, r13+d 8%, r2+d 3%, other rA+d 63%, rA+rB 4%; each ~12 ARM instr through stub = ~37% of executed ARM instructions** [qemu_model; 2026-10-12] docs/HANDOFF.md L768-770 *later fault-mode stub estimate is ~9 executed ARM instr*
- Paired-single GQR types executed: **40 fields: psq_l type 0 W=0 1,247,936, W=1 110,878; type 7 W=1 25,404 (98% unquantized float). psq_st type 0 622,933 / 136,952; type 4: 388; type 7: 140. sch7: psq_l type 7 W=1 12,984; psq_st type 0 234,323 / 66,447.** (TP cutscene) [qemu_model; 2026-10-13] stats2.txt lines 49-50; sch7/log.txt lines 153-154 *Informs: A float-only fast path for psq_l/psq_st covers ~98%; dequantization can stay slow*
- Top translated functions by share of AOT ARM instructions (function-level profile): **803464e4 PSMTXConcat 4.9% (14.8 ARM/guest); 80313828 J3DShapeMtxConcatView::loadMtxConcatView_PNGP 2.7% (22.6); 803268d4 J3DMtxBuffer::calcWeightEnvelopeMtx 2.4% (10.6); 8030d364 J3DGDSetLightAttn 2.2% (1.1); 8031419c J3DShapeMtxMultiConcatView::loadNrmMtx 2.0% (23.8); 8019d68c kankyo_color_ratio_set 1.8% (17.9); 80273a44 J3DUClipper::clip 1.8% (13.4); 80310a3c J3DSys::reinitTevStages 1.8% (21.6); top 25 listed functions sum ~39.8%** (qemu, TP 850-870, AOT translated functions only (542 with both counts); names resolved from /root/tp_symbols/symbols.txt) [qemu_model; 2026-10-14] aot/g_meas.txt lines 5-29; symbols //wsl$/Debian/root/tp_symbols/symbols.txt *Only 20 fields of one scene; counts ARM instructions, not cycles; J3DGDSetLightAttn at 1.1 ARM/guest looks anomalous (possibly a constant-folded path) and is unexplained. Informs: which TP functions/families an HLE or native port should target first (matrix concat, J3D shape matrix loads, GD light writers)*
- TP memory instructions by address form: **40 fields: r1+d 5,202,706; r2+d 788,654; r13+d 1,807,342; other rA+d 15,265,328; 0+d 1,158; rA+rB 1,035,970 (24.1 M). sch runs: 1,700,227 / 136,248 / 901,488 / 5,956,406 / 574 / 564,114.** (TP cutscene from 850) [qemu_model; 2026-10-13] stats2.txt line 48; sch7/log.txt line 152 *The two runs differ by more than their field counts (different builds and counting); use the shares Informs: Stack (r1) and small-data (r2/r13) accesses are ~1/3 and could take cheaper or unchecked paths*
- Azahar: test DOL speed: **lesson07 ~99-101%; lesson11 ~25-27% with c3d CPU ~59-61 ms; 3 TP runs in Azahar produced only probe output, no perf rows** (Azahar) [azahar; 2026-10-15] azahar-sdmc/perf.csv rows after lines 1-3, 1935-1966, 1967-2353 *Informs: Azahar is a crash/correctness check only; too slow to show TP*
- Family-level profile of guest instructions (grouped by symbol name): **J3D model/shape/material/joint (non-GD) 19.8%; JAS audio 15.6%; J3DGD display-list writers 14.7%; GX SDK 9.4%; collision cBg/dBg/cM3d 8.6%; PSMTX/PSVEC/math 7.6%; env lighting dKy/kankyo 4.1%; __save/__restore_gpr 2.7%; OS/DC/VI SDK 2.6%; actors 2.3%; REL/no symbol 1.9%; J2D/JUT 1.0%; memcpy/JKR 0.3%; other 9.4%** (PC interpreter, TP fields 900-1020, my regex grouping over symbol names) [estimate; 2026-10-14] aot/hist_900.bin (decoded with scratchpad hist_cat.py) *Grouping is mine (regex), not an established taxonomy; instruction share, not cycles. Informs: redesign targeting: ~44% of guest work is graphics preparation (J3D+GD+GX) and ~16% is audio sequencing, both candidates for native/HLE paths*
- Full-speed guest instruction rate and required core-0 cost: **~104-150 M guest instr/s; ~5-8 core-0 cycles per guest instruction needed vs 25.8 now; core 2 GX work must scale ~5x** [estimate; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:193-194 *Informs: Size of the gap a redesign must close (~3-5x on core 0 and core 2)*
- Full-speed GX demand of the title demo vs measured vertex-worker throughput: **At 60 fields/s about 300k draws/s, 1.5M vertices/s and 5.4 MB/s of GX stream. The device vertex worker measured ~275k vertices/s at 60% busy, i.e. ~460k/s at 100%: about 30% of full-speed demand** [estimate] derived from core/out/tp logs + git e30b707 body *Arithmetic only. ~275k vertices/s at ~20% speed is consistent with 20% x 1.5M = 300k. Informs: Whether CPU vertex gathering can ever reach full speed (it cannot without GPU-resident data or native mode)*
- Guest instruction rate per real second and per emulated second: **cutscene 22.9 M/s real (~103 M per emulated s at 22.1 %); later 60.2 M/s real (~485 M per emulated s at 12.4 %); movie 13.1 M/s** (clean run) [estimate; job 157] core/out/relay_157.csv frame_budget.py, divided by speed_pct *Derived by me; implies the guest is largely idle/waiting in the cutscene (time skipped) and near 1 instruction per Gekko cycle in the later stretch Informs: Idle-loop/timing work (C10) has more room in the cutscene than in the later stretch*
- TP triangle demand: **~1-2 Mtri/s** [estimate; 2026-10-11] docs/research/GPU_GUIDANCE.md:19-20 *Source of figure not given. Informs: GPU vertex rate (3.6-12 Mtri/s) is far from the limit*
- Flipper (GameCube) reference capacity: **162 MHz, 4 pixel pipes ~648 Mpixels/s, ~20 M raw / 6-12 M realistic polys/s, 16 TEV stages / 8 textures, ~2 MB EFB + ~1 MB texture cache** [unknown; 2026-10-11] docs/research/PICA200.md:373-374 *Published spec, not measured.*

### Native mode (43)

- A/B: display lists by copy (job 213/217) vs by reference (job 219): **by reference: busy 53-66% at 30 fps, ~200 KB GX/frame, consumer 2.6 ms; by copy: 80-100% busy, ~900 KB GX/frame, 14.8 fps stretch** (title demo; GX_AURORA 0x50 subcommand 0x0060 (pointer + size)) [device; jobs 213/217/219, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:93-97 *Separate runs, not an in-run A/B. Verdict: adopted. Informs: Aurora-stream design*
- First native picture on device: **0.1-4.6 fps** (title demo) [device; job 225, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:147-148 *Debug stream dump inflated some frames (see SD-write entry). Informs: Renderer path*
- First native picture: DRAW_INDEXED expanded to triangle lists (job 225): **4.61 fps median; ~162 draws and ~78,800 vertices per frame; consumer gx_proc ~194 ms/frame; busy 100%** (native, title demo) [device; 2026-10-15, job 225] relay_225.csv perf rows (gx column deltas; title-demo rows with GX >=100 KB/frame) *Informs: Expanded triangle lists cost vertex work: superseded by indexed drawing*
- Instrument: native-mode perf.csv and gxopt bisection: **Per second: fps, logic_ms, frame_ms, GX bytes per frame, drawdone/s, busy_pct, Link position, GX processing ms, display-list copy ms/KB/calls, plus GX pipeline ms by area. gxopt letters switch pipeline paths off or on.** (relay.sh native) [device] core/native/platform/native_3ds.cpp:1-4, 407; core/native/platform/gx/gx_3ds.c:94-123, 215-225 *No PMU or core 0 sampler in native mode. logic_ms includes the limiter sleep (see the first-run entry). Informs: Native-mode feasibility*
- Memory cost of the GX pipeline in native builds: **GX pipeline takes 5.4-5.5 MB heap and 8 MB linear (linear free 30.5 -> 22.5 MB). The game's largest block after OSInit: 33.5 MB without the pipeline, 26.8 MB with it; 23.8 MB with the 4 MB job ring (job 240); 22.2 MB with native shapes (job 242).** (native builds) [device; 2026-10-15/16] trace lines 'memory after the GX pipeline' / 'memory after OSInit' in relay_221.csv, relay_225.csv, relay_240.csv, relay_242.csv *Informs: Heap budget for any native-mode renderer cache (shape buffers, rings)*
- Native (Dusklight) logo/opening scene, run 2: **steady 60 fps over 133 s; GX 10-18 KB/frame; draw-done twice per frame; logic time equals the frame (16.4 ms) because waitForTick sleeps inside it** (rendering stubbed, audio off) [device; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:79-82 *Nothing drawn. Informs: Native feasibility step 1*
- Native (Dusklight) title/logo scene, no rendering, audio off: fps and game busy share: **Job 204 (476 s): 59.76 fps avg, busy median 8.9% (p90 9.4%, p99 10.8%) of a 16.4 ms frame (~1.5 ms). Job 207 (relay-launched, 117 s): busy median 8.9%, 60 fps. Job 201/202 (133 s): 60 fps, frame 16.4 ms, GX 9.9-18.1 KB per frame, draw-done 120/s.** (native build, logo/opening scene, nothing drawn) [device; 2026-10-15, jobs 201-207] relay_201.csv, relay_202.csv (trace), relay_204.csv, relay_207.csv perf rows *logic_ms equals frame_ms because the game's waitForTick sleeps inside the frame. busy_pct is Dusklight's frameUsagePct. Informs: TP game logic cost on the ARM11 is small in light scenes*
- Native converted shapes (native shapes on), jobs 242 and 244: **Both hung: the GX thread made no progress from frame 241 ('no progress for 2 s' x12, ring head moving, tail stuck at 00347d24); watchdog returned after 20 s with no game frame. Before the hang: 72 shapes drawn converted, 71 conversions, 38,547 vertices, 1,627 KB kept.** (native, title demo, native shapes on) [device; 2026-10-16 ~05:00, jobs 242/244] relay_242.csv, relay_244.csv '# hang', 'GX thread: no progress' and the last perf row 'ns' section *Informs: The converted-shape path has no speed measurement yet: it deadlocks on device*
- Native first-run logic time: **logic equalled the whole frame, 16.4 ms (the game's sleep is inside its frame)** (first native run) [device; 2026-10-15] core/native/platform/native_3ds.cpp:24-26 *A measurement artefact: native logic_ms includes the limiter's sleep unless frameUsagePct is used. Informs: Use Dusklight's frameUsagePct*
- Native GX decoder cost by command class (per second): **Job 238: Aurora commands 921 ms/s for ~17k commands (includes indexed draw vertex work); BP 94 ms for 115k; XF 33 ms for 38.8k; CP 10 ms for 20.8k; other 35 ms for 86.8k; draws 1 ms for 250. Job 240: Aurora 670 ms/s.** (native, title demo) [device; 2026-10-15, jobs 238/240] relay_238.csv, relay_240.csv perf gx column last section (median over title-demo rows) *Informs: Register-write decode is ~0.8 us per BP/XF command. The heavy cost is in the Aurora draw commands' vertex work.*
- Native Hyrule Field (F_SP121) core-0 busy: **164 s at the game's 30 fps; busy median 14.8% of 33.3 ms (~4.9 ms), p90 15.3%, p99 16.0%, max 16.3%; GX ~69 KB/frame** (boot-to-stage, Link idle with no input, nothing drawn, DUSK_AUDIO_SKIP, Link's heaps 2x (PC uses 20x)) [device; relay job 210, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:87-91 *No movement, combat, rendering or audio, so this is a lower bound on gameplay logic cost. Informs: Go decision for the native route (game logic fits core 0)*
- Native Hyrule Field F_SP121, hands-free: game busy at 30 Hz: **busy median 14.8% of the 33.3 ms frame (~4.9 ms), p10 14.1%, p90 15.3-15.4%, max 63% (load); 30 fps; GX 71,328 bytes per frame; repeated with presses (job 211) identically, since Link did not move** (native, boot-to-stage, audio skipped, nothing drawn, no input effect) [device; 2026-10-15, jobs 210/211] relay_210.csv, relay_211.csv perf rows *Informs: Feasibility: gameplay logic fits in ~5 ms of a 33 ms frame on core 0 when idle*
- Native Hyrule Field, hands-free (job 210): **164 s at the game's 30 fps; busy median 14.8% of the 33.3 ms frame (~4.9 ms), p90 15.3%, p99 16.0%, max 16.3%; GX ~69 KB/frame (Azahar ~71 KB)** [device; 2026-10-15] git 5df4f00 diff; 30f671a body *Link stood still, nothing drawn, no audio. --stage spawns were not controllable under scripted input (7fa0e8a), so there is no gameplay (movement / combat) measurement. Informs: Native mode's gameplay CPU headroom (provisional)*
- Native mode (Dusklight) title/opening scene, first device run: **60 fps for 476 s; busy 8.9% of each frame (Dusklight frameUsagePct)** (native.3dsx, before the gc3ds renderer drew; audio off) [device; 2026-10-15 evening] memory/gc3ds-native-mode-plan.md:23-26 *'logic' includes waitForTick's sleep; title scene, not gameplay; no real rendering yet Informs: Go/no-go: game logic is cheap on ARM11*
- Native mode drawing through gc3ds's GX pipeline (job 225): **5-7 fps on the title demo, bound by CPU vertex gathering; 4 MB job ring; display-list vertex cache off** [device; 2026-10-15] git 3c5251a body *Converted shapes (PICA vertex buffers, d7a9948) are unverified: job 242 left the console silent (shader truncated). Informs: GPU-resident vertex data is the next gate for native mode*
- Native mode vertex load and frame rate: **~73,000 vertices per frame on cores 2-3; title demo 5-7 fps** (Dusklight + Aurora through gc3ds GX, before converted shapes) [device] core/src/gx/pica/pica.c:2016-2019 *The game runs a 30 Hz tick (m_Do_main_3ds.cpp:78), so 30 fps is the native target, not 60. Informs: Native converted shapes (vertices in PICA buffers)*
- Native mode with gc3ds GX renderer: **5-7 fps; ~44k GameCube-indexed vertices per frame gathered on CPU (cores 2/3); tuning (optimizer on/off, indexed draws, 4 MB job ring) capped there** (Title demo) [device; 2026-10-15 night, 3c5251a] memory/gc3ds-native-mode-plan.md:28-31 *Informs: Convert J3D models at load time to PICA vertex buffers (draw with matrices only)*
- Native null-pointer crashes in the renderer bring-up: **Data abort with far 00000000 on the main thread ~6-10 s in (jobs 221, 223)** (native with gc3ds GX pipeline) [device; 2026-10-15, jobs 221/223] relay_221.csv, relay_223.csv '# crash:' lines *Informs: Fixed with PA_MEM1_BEGIN = 1 MB (per NATIVE_PORT_PLAN)*
- Native title / opening scene on the 2DS (nothing drawn, audio off): **Run 2: 133 s at a steady 60 fps, GX 10-18 KB/frame; memory 78.5 MB free at start, 33.5 MB after the 45 MB arena. Run 3 (476 s): 59.9 fps average, busy median 8.9% (~1.5 ms of 16.4 ms), p90 9.4%, p99 10.0%; 33-55% while the scene loaded** [device; 2026-10-15] git 1dfb4b2 diff (NATIVE_PORT_PLAN.md); c7d48ee body *Game logic only; no rendering or audio. Informs: Native mode's CPU cost is small*
- Native title demo (Link on Epona), first run: **80-100% busy; ~900 KB of GX per frame; a stretch at 14.8 fps** (heaviest scene so far; one START press) [device; job 213, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:93-95 *Superseded by the by-reference display lists (job 219). Informs: Display-list handling in Aurora's stream*
- Native title demo GX stream (jobs 213, 217, 219): **Job 213: 80-100% busy, up to ~900 KB-1 MB GX per frame, a stretch at 14.8 fps. Job 217: ~388 display lists per frame copied (680 KB, 4.2 ms) + stream scanned again (7.3 ms). Job 219 (lists by reference): busy 53-66% at 30 fps, ~200 KB GX per frame, consumer 2.6 ms; one stretch 17.4 fps at 100% busy (likely waiting on audio sync, unconfirmed). Azahar: 200-250 KB per frame** [device; 2026-10-15] git b6d078f, 474a6a9 bodies; edab796 diff *Informs: Display lists by reference are required*
- Native title demo with display lists by reference (GX_AURORA 0x50 sub 0x0060): **busy median 63.3% at 30 fps; GX 207 KB per frame (from ~840 KB); gx_proc 2.55 ms/frame (from 7.2); dl_copy 0.2 ms; one stretch at ~17 fps with 100% busy** (native, no renderer) [device; 2026-10-15, job 219] relay_219.csv perf rows *Informs: Adopted (DL by reference). The slow stretch is probably the demo waiting on music with audio off (unconfirmed).*
- Native title in-engine demo (Link on Epona), GX stream to Aurora's FIFO, display lists copied: **Job 213: busy median 91% (p90 100%), GX 843 KB per frame median (max 1.09 MB), fps median 29.7 with stretches at 13.9-14.8 fps. Job 215: busy 82.8%, consumer gx_proc 7.26 ms/frame. Job 217: DL copy 4.43 ms/frame, 692 KB/frame, 380 DL calls/frame, gx_proc 7.17 ms, busy 97.7%.** (native, START press at frame 600, renderer not yet attached) [device; 2026-10-15, jobs 213-217] relay_213.csv, relay_215.csv, relay_217.csv perf rows *Informs: Copying display lists inline dominated the producer*
- Native title/opening scene core-0 busy, run 3: **59.9 fps average; busy median 8.9% (~1.5 ms of 16.4 ms), p90 9.4%, p99 10.0%; 33-55% only while loading** (busy = Dusklight frameUsagePct; nothing drawn, audio off) [device; 2026-10-15, commit c7d48ee, 476 s] docs/research/NATIVE_PORT_PLAN.md:83-86 *Not gameplay. Informs: Native game logic cost*
- Native: 4 MB job ring so no draw is done inline on the GX thread (job 240 vs 231-238): **6.0 fps median (vs 6.5-6.7). GX-thread vertex work 637 -> 269 ms/s; GX thread waiting on the worker 88 -> 420 ms/s; worker busy 345 -> 679 ms/s. Game largest block 26.8 -> 23.8 MB.** (native, title demo) [device; 2026-10-15, job 240] relay_240.csv vs relay_236.csv/relay_238.csv perf gx column *Informs: Negative: moving inline work to core 3 made the worker the limit and lowered fps. One vertex worker cannot absorb the load.*
- Native: Aurora display-list optimizer off, GameCube strips (job 227): **7.79 fps median (6.4-8.6); ~10,070 draws and ~50,700 vertices per frame; GX thread 1,026 ms/s (100%), vertex worker 751 ms/s, VWAIT 324 ms/s, VALLOC 310 ms/s** (native, title demo) [device; 2026-10-15, job 227] relay_227.csv perf rows (gx column profile areas) *Informs: Per-draw work saturates the GX thread*
- Native: GX stream dump to SD: **frames took 5-10 s each** (gxopt D) [device; job 229] core/native/platform/gx/gx_3ds.c:141-142 *Debug artefact: runs with dumping are not timing evidence. Informs: Dump only frames 236-241*
- Native: merged draws not fitting the job ring: **GX thread saturated, core 3 at ~30%** (native mode, 1 MB ring) [device; job 238] core/native/platform/gx/gx_3ds.c:90-93 *Informs: 4 MB job ring in native mode*
- Native: optimizer on, DRAW_INDEXED drawn indexed (jobs 229, 231, 234, 236, 238): **6.5-6.7 fps median (p10 5.6-5.7, p90 7.5); ~230 draws and ~50,700 vertices per frame (~340k vertices/s). GX thread 1,025-1,042 ms/s (saturated); its own vertex work 590-700 ms/s; vfast_prepare 185-219 ms/s; worker 316-350 ms/s (~1/3 busy); waiting for GPU 82-116 ms/s. Main-thread gx_proc 112-148 ms/frame (back-pressure).** (native, title demo, steady rows) [device; 2026-10-15, jobs 229-238] relay_229.csv, relay_231.csv, relay_234.csv, relay_236.csv, relay_238.csv perf rows, gx column *Informs: Tuning this path will not reach 30 fps: ~44-50k CPU-gathered GameCube-indexed vertices per frame. This led to converting models once.*
- Unexplained slow stretch in the title demo: **17.4 fps at 100% busy, every second identical, same display lists as normal frames** (audio off) [device; job 219, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:97-99 *Most likely the demo waiting on the music it is synced to; unconfirmed. Informs: Whether audio must exist before native perf numbers are trusted*
- Compile sweep of Aurora's libs (native mode): **15 files failed: 12 on missing SDL3 headers (aurora, card x5, io, main, thread, mouse, pad, si), 2 on nod.h (dvd, fst), 1 on wgpu types (GXFrameBuffer)** [pc; 2026-10-15 08:21] //wsl$/Debian/root/native/asweep/err/*.txt *Superseded by the 3DS platform layer (git b20b01e: nod C API, SDL file shims, pad); Aurora's 20 GX API files compile (492da17). Informs: Native-mode platform layer (done)*
- Compile sweep of Dusklight's mod layer (native mode): **27 files swept, 3 FAIL: queue.cpp (picosha2.h missing), manifest.cpp (zstd.h missing), audio_res/wsys.cpp ('assert' not declared)** [pc; 2026-10-15 08:38] //wsl$/Debian/root/native/sweep/fails.txt, files.txt, err/*.txt *Superseded: native mode later linked with no undefined symbols (git 535ce64) and the mod service layer builds (b20b01e). Earlier sweep per git 492da17: 35 of 46 Dusklight files compiled. Informs: Native-mode bring-up history (resolved)*
- Native compile coverage: **1,308 of 1,309 game files; 35 of 46 of Dusklight's own files; Aurora OS, MTX, GD, VI, THP, AR and all 20 GX API files** [pc; 2026-10-15] git 492da17 body *Informs: Port feasibility*
- GX FIFO bytes per frame in native mode: **Logo frames ~18.2 KB (30+16,764+1,343+31 bytes); early 3D frames ~68.4 KB (frames 248-249)** (Azahar, title demo) [azahar; 2026-10-15 17:48] azahar-sdmc/native/trace.txt lines ~60-307 *Byte counts are workload-determined, likely the same on device Informs: Size of GX command stream to the gc3ds pipeline*
- Native build size and memory (Azahar): **Links with no undefined symbols: 13.0 MB code, 0.8 MB data; Dusklight's PC heaps (~140 MB) cut to ~37 MB of a 44 MB arena (system 8 MB, archive/j2d x1.5, game x3), ARAM 16 MB; 78.5 MB heap and 30.5 MB linear heap at start; 5,200+ frames at 60/s, GX ~18 KB / 2 draw-done markers per frame** [azahar; 2026-10-15] git 535ce64 body *Informs: Native mode fits in memory*
- Native heaps and Azahar progress: **Link's archive heaps at Dusklight's 20x asked for 13.3 MB against a 13 MB game heap (2x now used); in Azahar the logo scene finished and the opening started (8,760 frames in 160 s)** [azahar; 2026-10-15] git 30f671a, e445e85 bodies *Informs: Heap sizing*
- Native mode in Azahar: per-second frame rate and busy share: **Logo: 52.5-60.0 fps, logic_ms 16.6-19.0, busy 11.6-19.3%, gx_proc 1.09-1.43 ms, gx bytes 9.9-13.0 k/frame; at frame 241 (3D starts): 16.4 fps, frame 60.9 ms, gx bytes 18,137/frame, gx_proc 3.50 ms** (--seconds 150 --press START@600-606, Azahar) [azahar; 2026-10-15 17:48] azahar-sdmc/native/perf.csv rows 4.0-8.0 *Azahar timings are not the device's Informs: Crash-test only*
- Native mode memory budget: **Largest malloc block 77.0 MB at start -> 71.5 MB after GX pipeline (5.5 MB arena) -> 26.8 MB after OSInit's 64 MB arena (heap arena 50.3 MB); linear free 30.5 -> 22.5 MB; JKR heaps: archive 13,627 KB, game 13,224 KB, zelda 7,454 KB, J2D 750 KB, command 8 KB; JKRHeap arena 0x2BFC000** (Azahar native run) [azahar; 2026-10-15] azahar-sdmc/native/trace.txt:1-20; azahar-sdmc/native/log.txt:2-18 *Memory layout in Azahar probably matches the device but was not measured there in my sources Informs: Headroom for load-time PICA vertex buffers and textures*
- Native frame budget and render size: **TP logic at 30 Hz = ~33 ms budget; Dusklight renders at 608x448** [estimate; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:66, :130 *Informs: Per-frame budgets for logic, GX and vertex work*
- Native heap arena: **44 MB carved; ~37 MB used with 3DS sizes; core 1 time limit 30%** (native mode) [estimate] core/native/platform/native_3ds.cpp:171-173; core/native/platform/gx/gx_3ds.c:78 *Configured sizes, not measured. Informs: Memory budget for native shapes (8 MB NS_BUDGET)*
- Decomp progress of other candidate games (decomp.dev, undated): **Pikmin 1 100/100; Pikmin 2 99.63/98.09; Melee 100/100 (a listing shows 47.50); Galaxy 100/68.75 (a listing shows 28.75/13.24); Wind Waker 79.58/68.07; Sunshine 48.96/18.28; MKDD 47.09/41.93; Skyward Sword 27.82/15.97; TTYD 12.10/8.12; NSMBW 3.21/3.21; Brawl ~1.0/0.87 (% decompiled / % linked)** (web only) [unknown; captured 2026-10-15, figures undated] docs/research/DECOMP_SURVEY.md:16-46 *Several figures conflict between sources. Informs: Which games could get native mode next*
- Dusklight and TP decomp size: **~970k lines of C++20 in src (1,262 .cpp) plus libs; zeldaret/tp GZ2E01 100.00% decompiled, 87.13% linked** (web sources) [unknown; 2026-10-15 (decomp.dev undated)] docs/research/NATIVE_PORT_PLAN.md:21-25; docs/research/DECOMP_SURVEY.md:22 *Code size feeds the I-fetch and heap risks. Informs: Binary size, -Os vs -O2, heap headroom*
- Existing low-end ports of GC/Wii decomps: **none on 3DS, Vita or Wii U; Dusklight requires D3D12/Vulkan 1.1+/Metal; Aurora has no 3DS backend; no GameCube game has run on a 3DS by any method found** (web survey) [unknown; 2026-10-15] docs/research/DECOMP_SURVEY.md:10-11, :84-88; docs/research/NATIVE_PORT_PLAN.md:31-33 *Informs: A PICA200 consumer for Aurora's GX stream is new work (now built in gc3ds)*

### Instruments and probes (60)

- Branch: predictable vs random direction, per round of 5 instructions; mispredict penalty: **3.44 vs 7.19 cycles per round; penalty ~7.5 cycles; 0.50 misses per random branch (counters)** (timingprobe branch) [device; job 176; counters job 178] docs/research/TIMING_MODEL_PLAN.md:81,96 *SD5113 published prior is 6 cycles (survey); device supersedes. Earlier FINDINGS: branch misprediction under 1% of core 0 in TP. Informs: Branch-prediction modelling is low priority*
- Branches: predictable / random-direction loop round (5 instructions); mispredicts per random branch: **3.44 / 7.19 cycles per round; brmiss 0.50 per random branch, about 7.5 cycles per mispredict** (PMU on) [device; 2026-10-15, jobs 177/178] relay_177.csv, relay_178.csv '# timing branch' *Informs: Value of branch layout / mispredict avoidance in emitted code*
- Cache geometry and memory behaviour (jobs 181 / 183): **L1D 4-way 16 KB, not LRU; L2 16-way 2 MB, not LRU; 32-byte lines; 8 MB sequential stride 32/64/128/4096 = 48/80/126/168 cycles vs random 156; one outstanding miss (156 -> 146 cycles with 2-6 chains); store miss 32.4 / 83.7 cycles (write-allocate, ~2x a load miss); stores 2 registers/cycle; ldr+rev dependent chain 6.0 vs 4.0 cycles** [device; 2026-10-15] git 4349dda diff (TIMING_MODEL_PLAN.md) *Informs: No memory-level parallelism; byte-swapped RAM (C18) ~3-4%*
- Cache geometry: L1 D ways; L2 ways; line size: **L1 D 4-way, 4 KB way (4K stride x4 4.0, x5 7.85, x6 16.7, x8 27.1 cycles); L2 16-way, 128 KB way (128K stride x16 27.1, x17 43.3, x20 91.4, x24 124.6; 64K stride x32 27.1, x33 35.5, x40 94.8); 32-byte lines (1 MB sequential chase stride 4/8/16/32 = 6.86/9.72/15.4/26.9)** (core 0) [device; 2026-10-15, jobs 182/183] relay_183.csv '# timing assoc', '# timing seqchase'; relay_182.csv (partial repeat) *Informs: Alignment and stride of JIT tables and guest RAM layout; the replacement policy is not LRU (one line over gives a partial miss)*
- Call + return chain, depth 1-12 (cycles per call+return): **16.5 (depth 1) -> 18.3 (depth 12), no cliff** (core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing calls depth' *Informs: Use of bl/bx lr for guest call/return mapping*
- Check that the PMU counters read correctly: **Reading 4 MB at one word per 32-byte line (131,072 lines): 131,080 D-cache read misses, 131,086 SCU core-0 misses, 135,728 external reads, 763 external writes, 8 I-cache misses. A 20M-instruction loop had 76 mispredicts.** (hwprobe, PMU on) [device; 2026-10-11] core/out/hw_2026-10-11.txt *Informs: PMU columns in perf.csv can be trusted as absolute counts*
- Clock mode choice: **Use 804 MHz with L2 on** (gcbench run 3) [device; 2026-10-10] memory/tp3ds-gc-wii-on-2ds.md:44 *10-10 note says measured costs ran ~3x arithmetic prediction so clock was suspect; later probes measure the clock Informs: Default run mode*
- Clock modes: speedup on (804 MHz, L2 on) vs off (268 MHz, L2 off): **add_dep 1.321 ns (757 MHz effective), 1 MB chase 34.4 ns; off: 3.963 ns (252 MHz), 229.7-229.9 ns** (osSetSpeedupEnable on/off) [device; 2026-10-15] relay_177.csv, relay_178.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing clock' *Informs: Confirms the speedup/L2 mode is on; the effective clock is ~757 MHz*
- Clock modes: speedup on / off (add_dep; 1 MB chase per load): **757 MHz, 34.4 ns / 252 MHz, 229.7 ns** [device; job 178] docs/research/TIMING_MODEL_PLAN.md:105 *Measured clock reads 757 / 252 MHz not 804 / 268. Informs: Speedup mode (and L2) must be on*
- core -> VFP -> core register moves: **10 cycles** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:79 *Informs: Keeping guest FPRs in VFP registers vs moving through core registers*
- Core clock as measured by the run header and by the PMU: **header: start 279-284 MHz, after speedup 843-852 MHz (ptm 844-852); periodic '# clock' 842-852 MHz; PMU 790-803 Mcycles per 1 s row at cpu_pct 98.5-99.2** (every run) [device; jobs 101-170] run header and '# clock' lines in relay_101-170.csv; pmu_mcyc column *The two sources disagree by ~5 %; nominal New 3DS clock is 804 MHz Informs: Cycle budgets should use the PMU figure (~800 M/s)*
- Core clock: hwprobe's loop timing against PMU cycles per tick: **Loop timing gave core 0 849-852 MHz, core 1 709-710, core 2 852, core 3 826-829. The PMU loop gave 20,013,335 instructions in 50,045,848 cycles, which is 804 MHz by the tick. Perf rows read pmu_mcyc of 804.1 M/s at most.** (hwprobe, standalone) [device; 2026-10-10 / 2026-10-11] core/out/hw_2026-10-10.txt, hw_2026-10-11.txt; relay_68-99 pmu_mcyc column *The loop-derived MHz figures are about 6% high against the PMU. Core 1 reads low because it is shared with the OS. Informs: The cycle budget for any core-0 model: 804 M cycles/s*
- CPU clock: **268 MHz at start, 804 MHz after osSetSpeedupEnable; loop estimate reads 5% high** [device; 2026-10-10] docs/HANDOFF.md L357-358
- Data-abort fault delivery cost (job 85): **1.65 us per fault; exact PC and resume; unaligned ldr correct. qemu TP: 613 faults, 26,373 sites** [device; 2026-10-12] git 73ef32b body *Informs: Fault-backpatched memory is viable on speed*
- Data-abort fault probe (job 85): **fault delivered with exact PC; one fault costs 1.65 us; patch cache sync (svc 0x94) 21 us once per site; 202 faults passed; MEM1 at 0x0800cfe8, all other regions >= 0x40000000 fault; unaligned ldr ok** [device; 2026-10-13, 249f8dc] docs/HANDOFF.md L805-809
- Dependent add latency (cycles): **1.06** (timingprobe op, core 0, 804 MHz, from system tick) [device; relay job 176, 2026-10-15; repeated job 178 within 1%] docs/research/TIMING_MODEL_PLAN.md:71
- Dependent ldr + rev chain (guest big-endian load) vs ldr alone: **6.0 vs 4.0 cycles (+2 per dependent guest load)** (4 KB chase, core 0) [device; 2026-10-15, jobs 181-183] relay_181.csv / relay_182.csv / relay_183.csv '# timing op ldr_rev_dep (BE chase, 4K)' *Informs: C18 byte-swapped guest RAM (removes the rev on loads)*
- Device instruction-fetch cost by straight-line code size: **1-16 KB: 1.01-1.04 cycles/instruction. 20 KB: 2.0. 24 KB: 2.6. 32 KB-1 MB: 3.39 (0.125 misses/instruction, ~19 cycles per sequential line from L2). 2 MB: 3.87. 3 MB: 5.23. 4 MB: 5.64. 8 MB: 5.99.** (timingprobe icode section, sequential code) [device; 2026-10-15 job 183] relay_183.csv lines 46-63 *Sequential code. Job 184 puts TP's real I-misses at ~29 cycles each. Informs: Combined with the ~1.86 MB touched JIT set: any redesign that grows hot code past the 2 MB L2 moves fetch from ~3.4 to ~5-6 cycles per instruction (as traces did)*
- Device load-to-use latency by working-set size (pointer chase, core 0): **4-16 KB: 4.0 cycles. 24 KB: 18.2. 32 KB: 23.0. 64 KB-1.5 MB: 27.7-29.2 (L2 hit). 2 MB: 33.4. 3 MB: 101.6. 4 MB: 128.1. 8 MB: 156.2. 16 MB: 166.0.** (timingprobe, core 0, fewest cycles of five pair runs, clock 804 MHz mode) [device; 2026-10-15 job 183] core/out/relay_183.csv lines 105-121 *Idle machine. GPU contention doubles these (job 192). Informs: Cost per data miss for the model: ~28 cycles from L2, 130-166 from FCRAM*
- Device pipeline latencies that differ from arm11_sched.py's table: **rev dependent chain 2.0 cycles (model LAT_MEDIA 1). ldr->rev->ldr chain 6.0 vs ldr alone 4.0 (model gives 5). mul dependent 5.0 (model 3). vmov core->VFP->core round trip 10.0 (model 4+2 = 6). Matches: ldr_dep 4.0, ldr_use 4.07, vadd/vmul.f32 8.0, vmul.f64 9.0, vdiv/vsqrt.f32 19.0, add 1.06.** (Job 183 timing probe) [device; 2026-10-15] relay_183.csv lines 10-26,103; kernels in core/host3ds/timingprobe.c lines 200-243; model table arm11_sched.py lines 18-22 *vldr_use (8.0) is bound by the vadd chain, so the VFP load latency stays unmeasured. The split of the 10-cycle vmov round trip between directions is unknown. Informs: Calibration of the interlock model (used in the re-score entries below)*
- FPSCR value under the 3DS runtime: **0x23000010: flush-to-zero and default-NaN are set** (hwprobe) [device; 2026-10-10] core/out/hw_2026-10-10.txt, hw_2026-10-11.txt *Fits the 10 float sign-of-zero mismatches in the on-device fuzz (job 29) Informs: Single-precision and float-in-VFP JIT design (C8, C11). Denormals become +0 on the ARM11.*
- hw.txt JIT microbenchmarks, ns per guest instruction linked / unlinked: **ALU loop 4.5 / 66; memory loop 7.6 / 59; call+return 12.2 / 190; compare+branch 7.7 / 130** [device; 2026-10-11, cbff68c] docs/HANDOFF.md L138-139 *Informs: block linking value (~10-15x on dispatch-bound code)*
- Independent loads (cycles per load): **1.06** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:75
- Indirect call cost from one site: 1 target / 2 alternating / 4 / 16 round-robin / 16 random (cycles, including the loop): **28.1 / 29.6 / 30.3 / 30.9 / 30.9; brmiss ~0 in every case** (core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing indirect' *Informs: Dispatcher / blr / bctr design: indirect targets are not predicted, so ~28-31 cycles whatever the target pattern*
- Instruction cache refresh after JIT rewrite: **199 of 200 rewrites ran stale code without refresh, 0 with; full I-cache refresh 2.8 us; 256-byte D-cache flush 4.7 us** (2DS gcbench) [device; run 1/2 (pre 2026-10-10)] docs/FINDINGS.md:10-11 *Informs: Cost of code invalidation (icbi, SMC)*
- Instrument: gpuprobe.c (probe-only jobs): **PICA200 micro-benchmarks timed by C3D_GetDrawingTime: fastest of 5 frames. Cases: vertex by format (60,000 zero-area vertices), fill (ns/pixel by blend/depth/texture/format/size/stages), shader length +32/+64 instructions, setup by tile size, 1,000 one-triangle draws, transfers (MB/s), core 0 chase while the GPU samples FCRAM vs VRAM.** (after timingprobe, no game afterwards) [device] core/host3ds/gpuprobe.c:1-13, 48-93 *GPU time comes from citro3d completion interrupts (frame granularity). Fill numbers are kept only beside a centre-pixel check. Informs: GPU side of the timing model*
- Instrument: hwprobe.c (hw.txt at every startup): **Measures the tick-read cost, MHz of cores 1-3, core 0<->2 spin and LightEvent round trips, vfast on synthetic draws (unlit / 7 lights, 64- and 5-vertex draws), random-read latency (256 KB / 1 MB / 16 MB, 64-byte stride, 200k steps), a PMU self-test, and JIT synthetic benchmarks (alu/mem/call/cmp, straight lines 1k-256k sized around the 16 KB L1 I and 2 MB L2; linking on/off).** (startup, well under a second) [device] core/host3ds/hwprobe.c:1-7, 166-190, 192-348 *Single pass with no repeats. hw.txt is overwritten on every launch. Synthetic code. JIT benchmarks are skipped in Azahar test builds. Informs: Core placement, vertex-path design*
- Instrument: pmu.c / perf.csv PMU columns: **Core 0 cycle counter plus 5 event pairs rotating one per second (inst/brmiss, icmiss/dcrmiss, stall_inst/stall_data, dcwmiss/tlbmiss, branch/stall_lsu) and 4 fixed SCU counters (core 0/2 linefill misses, external read/write)** (svcControlPerformanceCounter) [device] core/host3ds/pmu.c:8-25, 112-129; core/host3ds/pmu.h:2-5 *Each event is sampled only 1 s in 5, so aggregates assume stationarity. Core events are core 0 only, so the GX thread (core 2) and worker (core 3) have no PMU events. pmu.h says four pairs; the code has five. --pmu-split samples 1 JIT run in 8, scaled, each read a system call. Informs: Cycle split for every core-0 decision*
- Instrument: timingprobe.c (--timing-probe on/only): **Core 0 micro-benchmarks. Each case runs ~20 M cycles once per PMU pair (5 pairs, ~25 ms each) and reports cyc = fewest cycles plus 10 events per operation. Cases: icode, chase, tlb, dep/ind, branch, stream, svc, xcore, indirect, calls, btac, install, op forms, assoc, seqchase, mlp, store/load. Also bandwidth (memcpy/memset/read 4 MB, best of 4), kernel switch, SD storage, 268 vs 804 MHz clocks, and DMA (only with --probe-risky).** (runs before the machine boots; MEM1 (24 MB) and JIT buffer as scratch; output as # timing lines) [device] core/host3ds/timingprobe.c:1-25, 82-118, 797-866 *Core 0 only; synthetic kernels. Storage is one pass, kernel switch 2000 rounds. CPU access to VRAM was left out after job 176 stopped in the bandwidth section. DMA contention runs only on request. Informs: Calibrating the 2DS timing model (TIMING_MODEL_PLAN)*
- Integer pipeline: add dependent / mul latency / mul throughput / rev dependent (cycles): **1.06 / 5.0 / 2.1 / 2.0** (timingprobe.c on core 0, 804 MHz, fewest cycles of five runs) [device; 2026-10-15, jobs 176/178/181/183] core/out/relay_176.csv '# timing op' lines; repeated relay_178.csv, relay_181.csv, relay_183.csv *Job 176 ran with PMU events zeroed (counters disagreed with the tick). The cycle counts repeat within 1% in 178/181/183. Informs: Cost model for JIT-emitted ALU code*
- L1 cache geometry (CP15 cache type): **L1 I and D 16 KB each, 4-way, 32-byte lines** (2DS CP15 read, run 3) [device; 2026-10-10] docs/FINDINGS.md:154-155; REGION_JIT_PLAN.md:41-43 *Informs: Footprint and data layout*
- L1 load-to-use (pointer chase) / independent loads / ldrh,ldrb use (cycles): **4.0 / 1.06 / 4.07** (core 0, L1-resident data) [device; 2026-10-15, jobs 176/183] relay_176.csv, relay_183.csv '# timing op ldr_dep, ldr_ind, ldrh_use, ldrb_use' *Informs: Guest load cost floor in the JIT; scheduling around loads*
- ldr+rev dependent chain vs ldr alone: **6.0 cycles vs 4.0 cycles** (2DS timing probe, job 183) [device; 2026-10-15] docs/research/REDESIGN_CANDIDATES.md:244-246 *Informs: C18 byte-swapped guest RAM (2 cycles per dependent guest word load)*
- ldrh / ldrb then use: **4.07 cycles (as ldr)** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:150
- Load latencies, first timing probe (job 176): **L1 load 4 cycles, L2 ~28, memory 100-166; code from L2 3.4 cycles per instruction; core 2 streaming stores raise core 0's L2-resident loads 28 -> 120 cycles** [device; 2026-10-15] git 053c741 body; 559c668 subject (L2 29, FCRAM 126-170) *Informs: Shared-L2 contention explains C2 and C15 losses*
- Measured 2DS cache geometry: **L1 D 16 KB 4-way, L2 2 MB 16-way, 32-byte lines, neither LRU, write-allocate (a store miss costs ~2x a load miss)** (assoc / store probes) [device; timingprobe jobs 181, 183] core/tools/qemu-plugins/dcache.c:5-8 *Informs: qemu cache-simulator parameters*
- Measured ARM11 clock per run: **828-852 MHz ('# clock' lines; 66 of 101 read 851 MHz); start clock 281-284 MHz before speedup** (2DS jobs 86-94) [device; 2026-10-13] relay_*a.csv '# clock' and '# run' lines *Nominal New 3DS clock is 804 MHz; PMU shows ~802 Mcycles/s, so the 851 figure's reference is unclear. Informs: cycle budget per field*
- mul latency / throughput (cycles): **5.0 / 2.1** (timingprobe op, core 0) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:72 *Informs: JIT scheduling of multiplies*
- PMU counter validation: **20M-instruction loop reads 20,013,335 instructions; counters are per core; clock 804 MHz** [device; 2026-10-11] docs/HANDOFF.md L142, L320-325 *Azahar does not emulate the SVC (garbage values) Informs: PMU numbers are trustworthy on device*
- rev dependent (cycles): **2.0** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:73 *Informs: Cost of byte-swapping guest big-endian data*
- SETEND BE data mode: **correct on hardware; 1 cycle per switch; +10% integer, +12% float, +14% paired vs byte-swapping each access** (2DS gcbench run 3; 90 qemu cases pass) [device; 2026-10-10] docs/FINDINGS.md:148-150 *OPT_JIT_PLAN.md:87-88 cites this as '+12% scalar float / +23% paired': conflicts with FINDINGS' +14% paired. In the JIT, OPT_BEFLOAT then measured neutral (job 166). Informs: BE float stubs (C16), C18*
- Stores and multi-register ops: str / strd / stm4 / ldrd / ldm4 / ldm7 / str-then-ldr same word (cycles): **1.13 / 1.13 / 2.06 / 3.0 / 4.0 / 6.0 / 5.07 per trio** (independent ops, core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing op str_ind, strd_ind, stm4_ind, ldrd_ind, ldm4_ind, ldm7_ind, str_then_ldr' *Informs: ldrd is slower than two ldr (2.1); stm stores 2 registers per cycle; no store-to-load forwarding penalty*
- Stores independent / strd / stm of 4: **1.13 / 1.13 / 2.06 cycles: 2 registers per cycle** [device; job 183] docs/research/TIMING_MODEL_PLAN.md:149
- Taken unconditional branches, 16 to 1024 of them 8 bytes apart (cycles per branch): **4.75 (16) -> 4.38 (32) -> 4.12 (128) -> 4.02 (1024)** (core 0) [device; 2026-10-15, job 183] relay_183.csv '# timing btac N' *Informs: Fall-through layout of hot paths (~4 cycles per taken branch, no capacity cliff, no BTAC folding seen)*
- Unaligned word loads: **ARM11 with SCTLR.U set loads unaligned words correctly (probed on the 2DS)** (2DS probe) [device; 2026-10-13] docs/OPT_JIT_PLAN.md:45-47 *Informs: Check hoisting can drop alignment checks*
- VFP single add / mul dependent latency; double add / mul: **8.0 / 8.0; double 8.0 / 9.0 cycles** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:76 *Informs: Float stub design, single-precision JIT work (C8)*
- VFP single add independent throughput: **2.0 cycles** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:77
- VFP single div / sqrt: **19 cycles** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:78
- VFP: single add/mul dependent; double add/mul dependent; single add independent; div/sqrt single; core<->VFP move; vldr then use (cycles): **8.0/8.0; 8.0/9.0; 2.0; 19/19; 10; 8** (VFPv2 on core 0) [device; 2026-10-15, jobs 176/183] relay_176.csv and relay_183.csv '# timing op v*' *Informs: Float/paired-single JIT design: keep values in VFP registers, avoid core<->VFP transfers (10 cycles each)*
- vldr then use: **8 cycles** (timingprobe op) [device; job 176/178] docs/research/TIMING_MODEL_PLAN.md:80
- Azahar: cache/TLB sweeps: **icode flat ~3.0 cycles from 1K to 8192K; pointer chase flat 4.136 cycles from 4K to 16 MB; TLB flat 4.136 at 8-4096 pages; xcore with core2 streaming unchanged** (Azahar) [azahar; 2026-10-15] azahar-sdmc/perf.csv timing probe; azahar-sdmc/timing.txt:40-80 *Device results of these sweeps are not in my sources Informs: Azahar models no caches/TLB/contention*
- Azahar: clock speedup on vs off: **Identical: add_dep 4.216 ns (237 MHz) both ways; chase 1024K 4.7 ns** (Azahar) [azahar; 2026-10-15 10:48] azahar-sdmc/timing.txt:104-106 *Informs: Azahar cannot test clock or L2 modes*
- Azahar: CPU clock per core: **core 0 267 MHz, core 1 211 MHz, cores 2/3 268 MHz; perf.csv 'clock MHz: start 267, speedup 267'** (Azahar) [azahar; 2026-10-15 10:47] azahar-sdmc/hw.txt:3-6; azahar-sdmc/perf.csv run headers *Not the device Informs: Azahar ignores the 804 MHz speedup*
- Azahar: kernel switches and svc costs: **Same-core switch 85.45 us round trip; core 0<->2 21.42 us; svcSleepThread(0) 4.89 us; svcGetSystemTick 1,322 ns (hw.txt) / 1,064.8 cycles; svc icache sync (flush + 0x94) 30,436 cycles** (Azahar) [azahar; 2026-10-15] azahar-sdmc/timing.txt:81-96; azahar-sdmc/hw.txt:1 *Device FINDINGS.md: full I-cache refresh 2.8 us, 256-byte D-cache flush 4.7 us Informs: None (not device)*
- Azahar: memory bandwidth: **memcpy 4 MB FCRAM 315 MB/s; memset 584 MB/s; read 1,168 MB/s; CPU memcpy FCRAM->VRAM 315 MB/s; CPU read VRAM 1,167 MB/s; random reads 14.9 ns at 256 KB and 1 MB** (Azahar) [azahar; 2026-10-15] azahar-sdmc/timing.txt:85-89; azahar-sdmc/perf.csv probe; azahar-sdmc/hw.txt:15-16 *Informs: None (not device)*
- Azahar: PMU readout: **Garbage (20M-instruction loop reads 2 instructions, 8,589,934,594 cycles); 'counters disagree with the tick (8589934594 cycles vs 2360445)'** (Azahar) [azahar; 2026-10-15] azahar-sdmc/hw.txt:17-19; azahar-sdmc/timing.txt:39 *In timing.txt the sanity check fires only after the icode section, so the first ~38 rows of that file are bogus; in perf.csv's copy it fires at the start Informs: Azahar cannot validate PMU-based probes*
- Azahar: probe durations: **hwprobe 1,122 ms; timing probe 8.8-13.2 s; gpu probe 1.8 s** (Azahar) [azahar; 2026-10-15] azahar-sdmc/hw.txt:21; azahar-sdmc/timing.txt:107; azahar-sdmc/timing_gpu.txt:33 *Informs: Probes complete without crashing*
- Azahar: tick-based op latencies: **add_dep 3.38, mul_dep 6.39, ldr_use 6.39, ldr_gap 9.40, vadd_s 3.57, vadd_d/vmul_d 6.58, vdiv_s/vsqrt_s 45.68, vmov_rt 12.41, vldr_use 6.58, branch predictable 13.35, random 14.85 cycles** (Azahar, tick fallback) [azahar; 2026-10-15] azahar-sdmc/perf.csv (TP run timing probe, ~line 1970-1990) *Do not compare with device VFP table Informs: None for speed (crash test of timingprobe.c only)*
- ARM11 branch prediction/fetch facts: **128-entry BTAC indexed by VA[9:3]; 3-entry return stack; mispredict 5-7 cycles; I-cache round-robin, virtually indexed; 2 MB shared L2; no MOVW/MOVT (32-bit constant up to 4 instructions)** (ARM DDI 0360F (manual)) [estimate; 2026-10-12] docs/REGION_JIT_PLAN.md:41-43; PERF_LEVERS.md:45-46 *Manual figures, not measured on device (L2 size consistent with run-3 latency sweep). Informs: Return-stack/call handling; constant folding*
- SD5113 ARM11 priors (7-cpu, single-core part, not MPCore): **L1 16 KB 4-way 32 B lines; L1 hit 4 cycles; L1 TLB 10, L2 TLB 64; mispredict 6 cycles; BTAC 128; return stack 3; stall after 3 misses; RAM ~100 ns per line** [estimate; 2026-10-15] docs/research/BENCHMARK_TOOLING_SURVEY.md:70-77 *Prior only. Device confirms L1 geometry and 4-cycle hit; contradicts mispredict (7.5), BTAC folding (none seen), return-stack cliff (none), and hit-under-miss (one outstanding miss).*

### Methodology and noise (85)

- A/B resolution (10 % trimmed mean standard error per arm): **cutscene SE ~0.18-0.25 points on ~25 % core0 (~1 %), n 84-98 rows/arm; later stretch SE 0.05-0.08 on ~12.4 % (~0.5 %), n 80-99** (600 s A/B runs) [device; jobs 148-170] ab_split.py output over jobs 148-170 *Run-to-run baseline also moves ~0.3 points (core0 cutscene 24.45-25.28 for the same default arm across jobs) Informs: Effects under ~2 % cutscene / ~1 % later are not resolvable in one run*
- A/B switch cost and its contamination of compile share: **Each A/B switch second: speed 0.6-1.25%, ~5,900-6,140 compiles/s (all code recompiled); A/B runs show jitc_pct 6.5-7.3% and compiles 367-515/s vs 0.71% and 32/s in the no-A/B job 93 (2.5%, 174/s in job 94 title)** (2DS jobs 86-94) [device; 2026-10-13] relay_88a.csv per-row speed/compiles; relay_93a.csv, relay_94a.csv *Phase-start rows must be dropped from A/B statistics. Informs: steady-state compile cost on core 0 is <1% in the cutscene; A/B runs inflate it*
- A/B switching overhead vs clean runs: **Cutscene medians in A/B runs 21.2-22.0% (171/190/195) vs 22.2% clean (184, 30-250 s). jitc_pct mean 7-13% in A/B arms vs ~0.8% clean. Whole-run means of 600 s A/B runs (15.2-16.1%) include the 12.5% later stretch.** (tp850) [device; 2026-10-15] relay_171.csv, relay_190.csv, relay_195.csv vs relay_184.csv perf rows *HANDOFF's '15.5% (A/B) vs 20.5% (clean)' seems to compare a 600 s run (with the slow later stretch) against a 300 s cutscene-heavy run. By stretch, the A/B penalty is ~0.5-1 point in medians. Informs: How to read A/B results: compare arms within a run only*
- ab_split.py scene boundaries and spread: **tp850 cutscene ends ~301 s; heavier section (~5%) from ~580 s; fixed-time phases inflated spread +-0.25 vs +-0.07; split at 303 s, drop 8 s around, drop >=576 s, settle 4 s after switches** [device; 2026-10-14] docs/HANDOFF.md L1026-1029
- AOT (C12) predicted vs actual cost: qemu instruction count vs 2DS speed: **qemu ~17% heavier; 2DS ~30% slower** (AOT build vs JIT default, TP) [device; before 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:10-11 *Shows qemu instruction counts under-predict device cost. BEFLOAT and the second vertex worker also lost on hardware for reasons qemu misses (I-cache, shared L2); those losses are not quantified here. Informs: Whether PC/qemu can judge speed: not without a calibrated cache/L2 model*
- Between-run variance of the same build on the 2DS: **~5%** (relay jobs, TP save state) [device; current doc (undated)] docs/REMOTE_RUNS.md:80-82 *An effect under ~5% is visible only within one run. A/B arms alternate every 20 s inside a run (line 74). Informs: A/B design: compare only within a run; minimum detectable effect*
- Build transfer to the 2DS: **~85 KB/s (28 MB took 331 s); relay new-build download ~115 s; restart into the build 11.1 s after download** (Relay) [device; 2026-10-13/14] memory/gc3ds-fast-hardware-loop.md:36-37; memory/gc3ds-remote-runs-relay.md:16-18 (job 114) *Informs: Keep builds small (ppc2c --cover 0.97, -g0)*
- Core-0-own speed definition and A/B statistics: **core0 speed = speed / (1 - gxwait); perf_heavy.py reports mean and standard error over heavy rows split by the A/B column** (perf.csv) [device; current doc] docs/REMOTE_RUNS.md:80-81 *Definitional: it assumes gxwait time is pure stall. Informs: How core-0 cost is separated from GX waiting*
- Corrupted trailer in job 86 log: **core/out/relay_86.csv ends with five appended '{"error":"Too many requests, please try again later."}' strings (270 bytes); relay_86a.csv is clean; relay_91a-94a are byte-identical to core/out originals** (file comparison) [device; 2026-10-13] core/out/relay_86.csv tail; //wsl$/Debian/root/relay_86a.csv *Informs: relay rate-limit responses can land in logs; parsers must tolerate or reject them*
- Duplicated device log for job 88: **core/out/relay_88.csv (640 lines) = the 320-line run twice, byte-identical; //wsl$/Debian/root/relay_88a.csv is the single run (a prefix of the original)** (file comparison (cmp of halves)) [device; 2026-10-13] core/out/relay_88.csv vs //wsl$/Debian/root/relay_88a.csv *HANDOFF's 69/60 heavy-row counts for job 88 cannot be re-derived exactly from either file. Informs: use relay_88a.csv; any statistic over relay_88.csv double-counts rows*
- Early CPU cost vs arithmetic prediction: **Measured costs ~3x what arithmetic predicts (clock suspected)** (Title scene) [device; 2026-10-10 night] memory/tp3ds-gc-wii-on-2ds.md:93 *Later explained by I-fetch bound IPC 0.31 and memory latency Informs: Added clock measurement to the build*
- Fault-mode A/B, job 86 (short): **on 21.00% (n=17) vs off 18.18% (n=16)** (2DS, 41.6 s, one phase each) [device; 2026-10-13] relay_86a.csv *Superseded by job 88. Informs: short single-phase A/B confounded by scene drift (contradicted by the longer job 88)*
- Fault-mode A/B, job 88 (single-run copy): **fault on 20.99% (n=62, sd 1.53) vs off 20.51% (n=57, sd 1.89), switch seconds excluded; gxwait on 16.5% vs off 7.5%; verdict: no speed difference** (2DS, tp850.state cutscene, --ab faultmem, 20 s phases, 147 s) [device; 2026-10-13] relay_88a.csv (my aggregation); HANDOFF.md line 819-827 reports 19.44 +-0.65 on vs 19.53 +-0.60 off (heavy rows) *My row filter differs from tools/perf_heavy.py; HANDOFF line 878 notes core-0 speed rose 21.2 -> 23.6% while gxwait absorbed it. Informs: removing ~7 instructions per access did not move speed; fault mode also crashed the kernel and stays off*
- Four-phase A/B with 20 s phases (24e4585): **inconclusive for speed: phases fell on different parts of title loop (39k-48k draws/s). Mechanisms: early splits cut gxsync to 0.3-2% and core 0 wait ~27% -> 8-12%; deferred gather cut vdec 20% -> 0.2-2% (core 3 48-60%); both on: vwait 10.5%** [device; 2026-10-11, 24e4585] docs/HANDOFF.md L428-432 *Informs: phases shortened to 5 s*
- GPU memory fill VRAM throughput: **2,945 MB/s, then 27 and 34 GB/s in jobs 179-180: not usable** [device; jobs 178-180] docs/research/TIMING_MODEL_PLAN.md:108,132-133 *Tick around C3D_SyncMemoryFill does not time the fill itself. Informs: Still unknown: MemoryFill clear cost*
- GPU probe artefacts (superseded): **Job 178: fills read 15 Gpixel/s (quads outside clip z, nothing drawn); job 179: combiner cases at ETC1's 8.55 ns (texture still bound), colours unnormalised; job 177: a wider vertex stride hung the GPU** [device; 2026-10-15] git d8a59a8, 4f4ea87, 9ec79cd bodies *Superseded by job 180. Informs: Validate probes by reading back pixels*
- gpuprobe invalid results found: **job 178 fill reported 15 Gpixel/s (quads at clip z +0.5, nothing drawn); job 177 hung the GPU with a stride wider than its attributes (Azahar accepted it); job 179 combiner cases all read 8.55 ns (the ETC1 texture left bound)** (gpuprobe) [device; jobs 177-179] core/host3ds/gpuprobe.c:72-74, 188-189, 300-301 *GPU numbers from jobs 177-179 are void for those cases. Informs: Trust of early GPU probe lines*
- gxwait masks core 0 gains: **gxwait 9-16% in TP heavy rows; faster core 0 shows as more gxwait, not speed; use speed / (1 - gxwait) as core 0 speed** [device; 2026-10-13] docs/HANDOFF.md L876-880 *Informs: judge JIT changes by core 0 own speed*
- Heap at start of back-to-back relay jobs on one build: **1.1 MB heap (jobs 145, 150)** (Relay jobs without restart) [device; 2026-10-14] memory/gc3ds-codesize-candidates.md:24 *Informs: Relay restarts between jobs; check '# heap: largest malloc block' in every perf.csv*
- Host-profile sampling and clock cost: **clock read costs 0.25 us on the 2DS. Per-draw areas timed 1 draw in 16 and scaled x16. Interpreter fallbacks timed 1 in 64 and scaled. Vertex-worker phases timed 1 job in 16.** (g_profClock on (X toggles)) [device] core/src/common.h:33-60; core/src/gx/pica/pica.c:1215; core/src/jit/jit.c:314 *_pct columns read 0 when profiling is toggled off. Sampled areas carry sampling noise. Informs: Trust of per-area _pct columns*
- Instrument: jitsample.c (--profile on): **A core 1 thread samples every ~100 us whether core 0 is in compiled code and the last block entered. Reports top 200 blocks and # core0 outside compiled code by WH_ area every 10 s.** (needs one store per block (perturbs code)) [device] core/host3ds/jitsample.c:1-4, 21-33, 61-97 *Attribution is to the last block entered (helpers and stubs count to it). Core 1 is shared with the log writer, console and relay, so the sampling rate is uneven. Distinct PCs are capped at 4096. Informs: Hot guest code (hle_tp, relayout)*
- Job 178 GPU fill numbers: **15 Gpixel/s reported: VOID (all triangles clipped, nothing drawn)** [device; job 178; fixed d8a59a8] docs/research/TIMING_MODEL_PLAN.md:111-113 *Informs: Probes need read-back self-checks*
- M2 memcpy under GPU load (gcbench 4b): **invalid: compiler removed unread memcpy** [device; 2026-10-11] docs/research/MEASURE_PLAN.md:39 *Redone in gcbench 5 M2.*
- M6 false sharing in gcbench 4 (invalid run): **one line: core 0 2.1 M/s, core 2 81.6; separate: core 0 2.2, core 2 89.4 M/s** (gcbench 4 full run) [device; 2026-10-11] bench/results/gcbench4_2026-10-11.txt L593-596 *Core 0 was starved (about 2 M/s in both arms), so this shows nothing about sharing. Superseded by 4b. Not mentioned in MEASURE_PLAN Informs: Treat any cross-core bench as invalid unless both cores report comparable rates*
- Measurement traps seen in these logs: **Title runs from boot land on different scene parts (draws/s 49-65k). --pmu-split costs speed (job 46 15.67% against 16.25%). 5 s A/B phases leave about 1 settled row per phase. A press of X turns the time breakdown off (job 82). Some logs hold their run twice (relay_72, 73, 88). One header lacks a column (the perf gpupos run).** [device; 2026-10-11 to 2026-10-13] relay_46, 52-57, 82, 72, 73, 88; perf_2026-10-12_hle2.csv *Informs: How to run the next measurements*
- Overhead of the A/B mode itself (recompile on every 20 s switch): **~20 % more real time to reach the same emulated point (252.7 s clean vs 303-305 s A/B for emu 68.3 s)** (A/B period 20 s, 2 phases) [device; jobs 130-170] core/out/relay_130/157 vs 148/151/160/170.csv *Arms are compared after a 4 s settle, so within-run deltas stay valid; absolute A/B-run speeds are not clean-run speeds Informs: Compare A/B arms with each other only; section boundaries differ between clean and A/B runs*
- Probe/PMU reliability flags: **Job 176: PMU counters disagreed with the tick (2,870 vs 690 cycles), events zeroed. Jobs 183/192/196: counters disagreed during 1 KB install (983k vs 2,172k cycles) and fell back to the tick. Job 182 log ends mid-probe at 'assoc 64K x40'. Job 177 stopped at the first GPU vertex case.** (timingprobe/gpuprobe) [device; 2026-10-15] relay_176.csv, relay_177.csv, relay_182.csv, relay_183.csv, relay_192.csv, relay_196.csv '# timing: counters disagree' lines and log ends *Informs: PMU event counts around svc calls are unreliable; cycle counts by tick are fine*
- Re-analysis of float lanes (OPT_FCACHE, opt 3 vs 7) with today's ab_split.py: **job 131: core0 cutscene 24.46->24.40, later 11.79->11.67; 132: 24.49->24.97 (+2.0 %), 11.75->11.94 (+1.6 %); 133: 24.44->24.93 (+2.0 %), 11.78->11.86 (+0.7 %); 134: 24.56->24.93 (+1.5 %), 11.86->11.89 (+0.3 %)** (600 s A/B runs, same build 132-134) [device; jobs 131-134 (2026-10-13/14)] core/out/relay_131-134.csv, ab_split.py --col opt *CONFLICTS with REDESIGN_CANDIDATES C11 (+3.0-4.3 % cutscene, -2.7 to -4.4 % later, 'net zero, off'), which was computed before ab_split's split fix. The current tool shows a small gain in both sections, not a loss later Informs: Float lanes may have been rejected on a biased split; re-check before closing C11*
- Re-analysis of the VIx split claim (C4) with today's ab_split.py: dlcache-off arm speed: **113: 21.42, 115: 20.84, 116: 20.93, 118: 21.02, 119: 21.04 % (medians 21.75/21.45/21.88/21.93/22.09)** (cutscene, dlcache=0 arm) [device; jobs 113-119] core/out/relay_113-119.csv, ab_split.py --col dlcache *C4 says splitting matrix indices out of VIn moved cache-off speed 20.0-20.8 -> 21.3-21.4 %; the current tool shows at most +0.3-0.5 points in medians Informs: Weakens the evidence that worker memory traffic per vertex moves game speed*
- Re-analysis of traces in the 2026-10-13 re-measure (job 122): **core0 cutscene 24.51 off vs 23.24 on (-5.2 %, ~5 SE); I-cache misses 6.61->7.41 M/s; fetch stalls 191->241 M/s** (300 s, opt=1 build) [device; job 122] core/out/relay_122.csv, ab_split.py --col traces *HANDOFF filed jobs 120-124 traces as 'within noise'; job 154 later measured -5.5 %. The earlier verdict was a misread Informs: Traces were negative already on 10-13*
- Relay iteration overhead: **build download 168-202 s per job (job 188: two attempts failed after 30-37 s, third took 176 s); restart into the build 12.8-13.3 s (once 22 s, once 42.5 s); job 174: automatic restart unavailable (d88007fa), manual restart after 1,294 s** (relay.sh over the nakama-backend relay) [device; 2026-10-15] '# relay:' lines in relay_171.csv-relay_196.csv *Informs: Each device experiment costs >=3.3 min plus run time: favour PC-side gates and batched A/B runs*
- Relay job turnaround: **~5 minutes per question** [device; 2026-10-13] memory/gc3ds-hardware-first.md:13 *Informs: Build a live two-way relay*
- Relay log integrity: **relay_104/107/144/146 contain the run 2-4 times (144: 1,433 rows for 501 unique); relay_126 and relay_167 end in HTTP 429 'Too many requests' (147 s / 389 s); relay_111/112 have no header row** (relay paging before the 2026-10-15 fix) [device; jobs 104-167] core/out/relay_104,107,111,112,126,144,146,167.csv *For 144 the duplicates inflated n 84->279 and shrank the SE 0.25->0.14 without moving the means; I deduplicated in scratch copies Informs: Dedupe by real_s before trusting SEs on these jobs*
- Relay log integrity: **Jobs 168-171 logs were cut at ~410 s (relay returns at most 200 chunks, ~400 s, per request). Job 182 got a 429 error body in its log. Job 111 lost its CSV header. A full relay ring drops lines (counted). Builds are not byte-reproducible (ELF kept per push).** (relay runs) [device; jobs 111, 168-171, 182] core/tools/relay.sh:34, 59, 65-66; core/host3ds/relay.h:21-22; core/host3ds/relay.c:7-9 *Any analysis of jobs 168-171 past ~410 s, or of job 111, should be rechecked. Header-based parsers skip a header-less file. Informs: Re-fetch and validate old relay CSVs before reuse*
- Relay log truncation: **relay returns at most 200 chunks (~400 s at one chunk / 2 s) per request; analyses of jobs 168-171 before the fix used only first ~400 s** [device; 2026-10-15] docs/HANDOFF.md L1123-1126 *Informs: re-read any long job logged before the fix*
- Relay log truncation: **The relay returns at most 200 chunks (~400 s) per request; jobs 168-171 were first analysed on ~410 s of 600** [device; 2026-10-15] git e2d52e9 body *Any analysis of jobs 168-171 before e2d52e9 is superseded. Informs: Fetch full logs before reading A/Bs*
- Relay measurement loop capability: **14 A/B knobs (opt, kstore, fcache, dlcache, vw2, fskip, merge, traces, sched, pld, splitopen, vtrim, faultmem, hle); --profile hot guest sampling; --fuzz on-device JIT differential fuzzer; console polls every 10 s; job default 600 s; self-restart via hb:ldr** [device; current doc] docs/REMOTE_RUNS.md:58-78 *Save states are portable between qemu and 3DS builds (same EABI layout) until System/GXCore sizes change. Sleep drops Wi-Fi and strands a job. Informs: How fast redesign hypotheses can be tested hands-free*
- Relay turnaround per new build: **build download 167.4-183.2 s (8 jobs with a new build: 148, 149, 151, 152, 155-157, 163-166), restart 12.9-13.3 s; full runs 1,751-1,976 CSV lines; short runs 403-414 lines** (relay jobs 148-166) [device; 2026-10-14/15] //wsl$/Debian/root/jitwork/relay148.txt-relay166.txt *Run duration per line inferred from AOT_PLAN's 600 s runs; jobs 158-159 missing. Informs: a device A/B iteration costs ~3 min download + 13 s restart + run time (~10 min full run)*
- Repeatability of an identical configuration across runs: **The opt=3 arm in jobs 132-134: cutscene core 0 24.44-24.56%, later core 0 11.75-11.86%. Trimmed SE about ±0.2 (cutscene) and ±0.05 (later). Speed spread about ±0.5% between runs.** (tp850, 600 s, 20 s phases) [device; 2026-10-13] core/out/r132a-r134b *Informs: The smallest effect worth chasing is about 1-2% within a run; separate runs need more*
- Run-to-run / within-run spread of speed_pct: **sd 1.5-1.9 percentage points per 1-s row at ~20-21% speed (jobs 88, 91, 93)** (2DS cutscene rows) [device; 2026-10-13] relay_88a.csv, relay_91a.csv, relay_93a.csv *Rows are autocorrelated (scene content), so naive standard errors understate uncertainty. Informs: an A/B needs many rows (~100+) to resolve ~0.5-point effects*
- Run-to-run noise: **OPT_CALLS job 147: cutscene 20.4 -> 19.9%, later 11.0 -> 11.4%, called 'within the ~5% run-to-run noise'** [device; 2026-10-14] git 0903f5f diff *Informs: Within-run A/B is required; cross-run differences under ~5% are noise*
- Run-to-run noise between near-identical TP runs: **186 vs 187 (one inert option): means 19.36 vs 19.33; 191 vs 193: 20.21 vs 20.26; 10 s cutscene samples swing ~±1.5 points around the median** (tp850 from the same state) [device; 2026-10-15] relay_186.csv, relay_187.csv, relay_191.csv, relay_193.csv perf rows *Informs: Effects under ~0.5 points of speed (~2%) cannot be resolved without long A/B runs*
- Run-to-run noise of 2DS speed: **~5% run to run; within-run A/B arms resolve ~+-0.05 percentage points on the core-0-bound section; fskip +10% read as ~3 sigma** (2DS tp850, 600 s, 20 s A/B phases) [device; 2026-10-13..14] docs/research/AOT_PLAN.md:87; REDESIGN_CANDIDATES.md:39,124 *Scene mix matters: first 110 rows of job 128 read +21% vs +10% overall; repeat runs (jobs 132-134) needed to separate a 3% effect. Informs: Minimum effect size detectable; need for replicated A/Bs*
- Run-to-run repeatability (gcbench 3 vs gcbench 4, same tests): **JIT MIPS identical to 0.1; latencies within 0.1 ns; VFP within 0.01 cycles; GPU rates within 0.2%. Larger: SD write 6.11 vs 5.05 MB/s (-17%); ADPCM 0.268 vs 0.259 s (-3%); SHA-1 30.2 vs 29.0; upload b1 split swizzle 0.85 vs 1.03 ms (total the same); LightEvent fails at trip 3 vs 4** (Two device runs a day apart) [device; 2026-10-10 / 2026-10-11] bench/results/latest.txt; gcbench4_2026-10-11.txt L1-416 *CPU and GPU microbenchmarks are highly repeatable. I/O and DMA/sync timings are not (see the G14 conflict) Informs: How large an A/B delta is meaningful: about 1% for CPU and GPU kernels, more than 20% for SD and sync paths*
- Run-to-run repeatability of CPU probes: **job 178 repeated job 176 CPU numbers within 1%; job 181 vs 183 within 1% except code install (10-35%)** [device; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:95,137-138,157 *Informs: CPU microbenchmarks are trustworthy single-run; code-install numbers need replication*
- Run-to-run variance of the same build: **~5%** (tp850) [device; 2026-10-13] memory/gc3ds-region-jit-next.md:20-21 *Informs: A/B only within one run; gains under ~5% need in-run A/B*
- Run-to-run variance of title-from-boot: **texture decodes 10x, compiles 2.5x different between jobs 52 and 54** [device; 2026-10-12] docs/HANDOFF.md L678-679 *Informs: compare builds only by within-run A/B*
- Sampler 'dispatch' bucket: **13-21%, but it is jit_entry with no block running incl. idle-hook busy wait: mostly GX wait** [device; job 184] docs/research/TIMING_MODEL_PLAN.md:191-192 *Informs: Do not read the sampler's dispatch bucket as dispatcher cost*
- Scheduled-code A/B (--ab sched), jobs 91-92: **job 91 cutscene: off 20.98% (n=94) vs on 20.93% (n=102), sd ~1.9; job 92 title heavy rows (draws>20k): off 20.03% vs on 20.84%; verdict within noise** (2DS, 20 s phases) [device; 2026-10-13] relay_91a.csv, relay_92a.csv (my aggregation); HANDOFF.md line 859: 19.89/19.72 and 18.86/19.94 *Row filters differ from the project's perf_heavy.py. Informs: interlock scheduling (model -18% interlock cycles) gave no measurable device gain*
- SD debug writes inflating frame times: **5-10 s frames in runs 225-229 came from the debug stream dump and trace lines (SD writes per feed), not the pipeline** [device; jobs 225-229, 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:157-158 *Instrumentation can dominate a measurement. Informs: Keep SD tracing out of perf runs*
- Section boundaries in tp850 runs: **A/B runs: cutscene to ~301 s, later stretch to ~580 s, then movie; clean runs: cutscene to ~253 s, later to ~480-490 s, movie after** (tp850) [device; jobs 101-170] emu_s/draws_s timelines in relay_130/132/148/157/170.csv *ab_split.py hard-codes 303/576 s, correct only for A/B runs Informs: Any clean-run analysis must use its own windows*
- Single-run spread across builds, title from boot: **Heavy rows: 14.99% (da2a), 13.38% (82a0), 15.23% (b439), 15.07% (5328), 14.43% (6673). Core 0 own: 16.9, 13.7, 18.8, 19.5, 19.7%.** (Title from boot, no A/B) [device; 2026-10-11] core/out/perf_2026-10-12_hle2.csv runs 9-13 (= perf_2026-10-11_da2a, 82a0, b439, 5328, 6673) *82a091b (CR logic ops, 4096-entry indirect table) reads lowest, but there was no A/B to confirm a regression Informs: Comparing separate runs from boot hides changes of 1-2 points. Use in-run A/B.*
- Stack A/B (retstub + syscall + hlelink, job 151): number revised: **Commit and first HANDOFF: core 0 cutscene 24.33 -> 24.95% (+2.5%), later 11.85 -> 12.53% (+5.7%), I-cache -6% / -18%. After the ab_split fix: 24.47 -> 25.00% (+2.2%), 12.04 -> 12.54% (+4.2%). Alone: retstub +0.8 / +1.6, syscall +1.8 / +2.0, hlelink +0.2 / +2.8** [device; 2026-10-14] git ca3793a subject; git b17e546 HANDOFF diff *The first number is superseded; the ca3793a commit subject still carries +2.5% / +5.7%. Informs: Trust only ab_split-corrected numbers*
- timingprobe counter vs tick agreement: **tiny runs read 2,870 counter cycles vs 690 by the tick (job 176). The code-install case read 0.98 M vs 2.1 M (job 181), and the counters were dropped for the rest of that probe. Counters are now dropped only after 3 disagreements in a row (>2x).** (--timing-probe) [device; jobs 176, 181] core/host3ds/timingprobe.c:57-80, 113-117 *In job 181's later cases, events read zero and cycles come from the tick. Azahar refuses the PMU, so its numbers are tick-only. Informs: Which timing lines to trust*
- tp850 scene boundaries in device runs (ab_split fix): **Cutscene ends at ~301 s; a heavier section (~5%) starts at ~580 s; arm spread +-0.25 before the fix vs +-0.07 after (split at 303 s, 8 s gap, end at 576 s, settle 4 s)** [device; 2026-10-14] git b17e546 body + HANDOFF diff *Informs: A/B methodology*
- tp850 scene-change trap in A/B: **Scene changes at ~301 s and ~580 s land in the same arm every job; ab_split.py splits at 303 s, gaps 8 s, ends 576 s, trims 10%** (20 s A/B phases) [device; 2026-10-14] memory/gc3ds-codesize-candidates.md:22-23 *Informs: Earlier A/B verdicts before ab_split.py may be biased*
- Turnaround of one device speed check: **~11 min per relay run; crash-risk runs only when James is home** (relay workflow) [device; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:12 *Informs: Motivation for a PC timing model*
- Determinism of PC runs from save states: **names (from tp_2300) and cut1 (from tp_2900) end at field 3300 with identical counters (5,952,834 draws, 29,688,181 vertices, 408,937,094 GX bytes, 196,198 DSP mails); boot run4/run5 and all900 (from tp_900) match at field 960 (1,603,627 draws, 33,325,939 bytes)** [pc] core/out/tp/names, cut1, run4, run5, all900 logs *Informs: PC states are a valid reference; replay is deterministic*
- PC interpreter check cost: **12-46 MIPS of wall time: 1,000 fields (2300-3300) in 319.6 s; 400 fields (2900-3300) in 156 s; 60 fields from tp_900 with full logging 39 s** [pc] core/out/tp/names, cut1, all900 logs *Informs: Cost of PC correctness gates*
- qemu and PC check costs: **qemu TP check ~0.3 MIPS: ~13 min per 30 fields (plus ARM compile of 14 MB generated C); JIT-only TP under qemu ~20 min for 880 fields; ARM AOT_CHECK under qemu ~6 min for 150 fields; PC GC3DS_NODRAW TP 200 fields in 3.5 s** [pc; 2026-10-14] memory/gc3ds-fast-hardware-loop.md:12-28 *Informs: PC check mode is the correctness gate; skip qemu before relay jobs*
- Save states available for a second test scene: **Device-usable states come from the qemu ARM build: tp_arm_700, tp_arm_850, tp_arm_950. PC states: tp_900 (10-10 23:07), tp_pc_900, and tp_1100 / 2000 / 2300 / 2900 (10-10 15:44-15:59). Each ~45.7 MB. No save state of player-controlled gameplay exists** [pc] //wsl$/Debian/root/*.state listing; git 38bf728 (state v2, 10-10 21:29), 385c1ae (v3), a59e3d9 *tp_1100-2900 predate state version 2. 38bf728 says older TP states 'jumped into the wrong function and spun forever'. Whether they load on the current build or the ARM build is unverified, so a cutscene-at-2900 state for the 2DS would probably have to be regenerated. Informs: Choosing a second benchmark scene*
- Failed/aborted measurement runs: **arma/armj: 'Error while loading build-arm/gcemu: No such file' (8m03s, no data); aj: qemu JIT run terminated after 24m42s with no output beyond state load** (aot directory runs) [qemu_model; 2026-10-14] aot/arma/out.txt, aot/armj/out.txt, aot/aj/out.txt, aot/aj/time.txt *Informs: empty outputs are failures, not results*
- Instrument: qemu D-cache simulator (dcache.c + dcache_report.py): **ARM11 L1 D (16 KB 4-way round-robin) and L2 (2 MB 16-way, LRU approximation) for one thread. Per-line and per-PC misses mapped to JIT hot/cold/stubs, MEM1, JIT state, static symbols, heap, stack.** (qemu-arm with the PC ARM build) [qemu_model] core/tools/qemu-plugins/dcache.c:1-10, 65-68; core/tools/qemu-plugins/dcache_report.py:1-9 *The L2 sees only this thread's data, not code or other cores, so L2 misses are a lower bound. The PC build renders on the same thread (use code= to filter). No timing. build.sh builds only librangecount.so (libdcache.so was built separately). Informs: Data-miss sources behind the 22% data stalls*
- Instrument: qemu I-cache simulator (icache.c + icache_report.py): **16 KB 4-way round-robin L1 I. Each executed TB touches its lines. Misses split into JIT stubs, hot, cold, gcemu C functions, other. With --blockmap/--symbols they map to guest functions. --minus subtracts a deterministic shorter run to isolate steady state.** (qemu-arm) [qemu_model] core/tools/qemu-plugins/icache.c:1-6; core/tools/qemu-plugins/icache_report.py:1-12 *No L2, prefetch or branch predictor model. TB granularity. libicache.so is not present in the plugin directory and build.sh does not build it. Informs: I-fetch attribution (code size, relayout)*
- Instrument: rangecount (qemu plugin + aot/rangecount.sh): **ARM instructions executed inside the JIT buffer vs AOT functions, per guest instruction** (qemu-arm -cpu cortex-a7, TP state tp_arm_850, FR 870) [qemu_model] core/tools/qemu-plugins/rangecount.c:1-6; core/tools/aot/rangecount.sh:1-5, 17 *Counts instructions, not cycles. Helpers are not counted. Runs on a cortex-a7 model (ARMv7), not the ARM11. Informs: JIT vs AOT code quality*
- qemu prediction vs device for AOT: **qemu suggested AOT ~+17% (instruction count); device measured ~-30%** (qemu instruction counts vs 2DS A/B) [qemu_model; 2026-10-14] memory/gc3ds-timing-model-after-decomp.md:22; memory/gc3ds-aot-verdict.md:14 *The sign of the prediction was wrong Informs: Never judge speed ideas by qemu counts; build a PMU-calibrated timing model or test on device*
- Run windows: the qemu runs cover tens of fields, not hundreds: **ic_w850 30 fields; ic_w950 25; ic_smoke and ic_w950s 2 each (warm-ups); dcache 20; dcache2 60 (first 100 M accesses skipped); sch3/sch7/schb7 20; sched/sched2/stats2 40** (486 MHz Gekko, ~8.1 M cycles per field) [qemu_model; 2026-10-13 to 10-15] cycle deltas from 'loaded state ... cycle' to 'ran ... cycles' in each out.txt/log.txt (e.g. 6,891,959,341 -> 7,135,214,444 = 243 M cycles) *'880 fields' etc. in the outputs is the field number since boot Informs: Per-field rates must divide by the window, not the printed field number. All qemu evidence is from one TP cutscene region (fields 850-975).*
- Warm-up share of a state-loaded run: **88% of the 850 run's 26,020 block compiles happen in the first 2 fields (ic_smoke: 22,976). The 2-field warm-up holds 5.54 M of 22.78 M simulated I-misses (24%); the 950 run's: 6.49 M of 23.23 M (28%).** (Same builds as the long runs) [qemu_model; 2026-10-14] ic_smoke/out.txt, ic_w950s/out.txt, icache.txt headers *Informs: Always subtract a warm-up run (icache_report.py --minus) or skip accesses (dcache skip=) before quoting steady-state rates*
- Azahar as a timing source: **Refuses the PMU (cycles from the tick, junk if the counter call is accepted). Runs JIT benchmarks very slowly. Accepted a vertex stride that hung the real GPU (job 177). Crashed binding placeholder textures.** (Azahar test builds) [azahar] core/host3ds/timingprobe.c:57-69; core/host3ds/main.c:1764; core/host3ds/gpuprobe.c:188-189; core/src/gx/pica/pica.c:1666-1668 *Only for correctness and rendering orientation checks. No timings. Informs: Keep all timing on the 2DS*
- Azahar cache modelling: pointer chase and code fetch across sizes: **chase flat ~4.1 cycles 4 KB-16 MB; code fetch flat 3.0 cycles to 8 MB** (timingprobe.c in Azahar) [azahar; 2026-10-15 (job 172 era)] docs/research/TIMING_MODEL_PLAN.md:59-61 *Azahar models no caches; device shows 4 -> 166 cycles. Azahar timing is not a device stand-in. Informs: Azahar cannot be used for speed decisions*
- Azahar: hot-code sampler attribution: **outside compiled code 54.8%; dispatch 48.5% of core 0 time on an idle test DOL** (Test DOL idle) [azahar; 2026-10-15] azahar-sdmc/perf.csv lines ~1900-1901, 1955-1956 *Informs: Same sampler trap as on device: 'dispatch' includes idle/GX wait*
- A/B effect size vs model error: **A/B effects 1-5% are below model error** [estimate; 2026-10-15] memory/gc3ds-timing-model-after-decomp.md:26 *Informs: The 2DS stays the final judge; timing model is a later standalone project*
- Budget lines printed by gcbench (assumptions, not measurements): **GC load 30-60k tris per frame at 30 fps = 0.9-1.8 Mtri/s; overdraw 4 (11.5 Mpx/s, 2.0 ms); 1 MB new textures per frame = 7.6 ms; 4 EFB copies per frame = 8.0 ms; 100k vertices per frame = 16 ms CPU (48%)** (gcbench 3/4) [estimate; 2026-10-10] bench/results/latest.txt L185-186, L205-207, L221, L231, L235-236 *Each one is marked 'assumed' in the raw output. TP's actual tris, vertices, texture uploads and EFB copies per frame are not in these files Informs: Do not size the redesign from these budgets. Replace them with TP per-frame counts from relay perf.csv*
- CPU clock estimate in perf.csv: **17 dependent adds plus a branch, ~18 cycles per iteration, timed by the tick; ~2 ms of core 0 every 10 s** (# clock lines) [estimate] core/host3ds/main.c:114-115, 163-177, 1255 *It can only tell 268 MHz from 804 MHz apart. Informs: Clock sanity per run*
- In-run A/B method: **20 s phases (AB_SECS) alternate one option. Most switches drop all compiled code. Analysis skips the first 4 s of each phase.** (--ab X) [estimate] core/host3ds/main.c:305, 971-1028; core/tools/ab_split.py:1-9 *Arms see different game content. ab_split notes the cutscene tail (~26% of its last rows) and a heavier section at ~580 s always fell in one arm (jobs 148-155). The flush cost is inside every phase. Informs: All JIT/GX A/B verdicts*
- Instrument: ab_split.py statistics: **10% trimmed mean +- winsorized SE and median per arm. Stretches are hard-coded for tp850: skip 25 s, cutscene to 303 s, later stretch to 576 s, gap 8 s, settle 4 s. Core 0 speed = speed / (1 - gxwait).** (relay CSV) [estimate] core/tools/ab_split.py:1-86 *The SE treats 1 s rows as independent, but they are autocorrelated, so the SE is understated. icmiss_k / stall_inst_m exist only in 1 row in 5 (PMU rotation). Stretch bounds apply only to tp850. Informs: Noise floor for 1-2% JIT A/Bs*
- Instrument: arm11_sched.py interlock model: **Latencies: ALU 1, load 3, mul 3, VFP add/mul 8 (double mul 9), VFP load 4, ARM->VFP 4, VFP->ARM 2, div 19/33, +1 early operand** (JIT_SCHEDDUMP from the qemu build) [estimate] core/tools/arm11_sched.py:1-25 *Datasheet values (ARM DDI 0360F). Branches assumed to fall through. No caches or fetch. Its main prediction (SCHED_STUBS) gave no device gain. Informs: Scheduling work*
- Instrument: frame_budget.py: **Per stretch (default from 60 s): core 0 Mcycles per row, IPC, cycles and host instructions per guest instruction, stall split (instruction fetch, data, rest = issue + interlocks + mispredicts), events per 1000 host instructions** (relay CSV with PMU columns) [estimate] core/tools/frame_budget.py:1-80 *Events are scaled as mean-of-measured-rows x n (assumes stationarity across the 5 s rotation). The 'rest' bucket is a remainder, and the stall events may overlap. Core 0 only: it gives no GX-thread budget. Informs: Core 0 cycle budget per guest instruction*
- Instrument: perf_heavy.py / perf_diff.py / gx_split.py: **Heavy rows are draws_s > 1000. Reports mean +- SE of speed and core 0 speed (speed / (1 - gxwait), gxwait < 95). perf_diff lists columns that changed > 5%. gx_split prints GX thread and worker columns with ab_split's selection.** (relay CSV) [estimate] core/tools/perf_heavy.py:1-12, 79-85; core/tools/perf_diff.py:1-7; core/tools/gx_split.py:1-9 *The heavy-row filter drops light screens. Core 0 speed is a derived estimate. Informs: GX vs core 0 bottleneck reading*
- Modelled (not measured) hardware timing in the emulator: **DI: 300 us per command, 600 us plus transfer per read (Dolphin constants, fixed rate). VI: 59.94 fields/s, 263 lines/field. TB at 1/12 of the CPU clock.** (emulated machine) [estimate] core/src/hw/di.c:7-8; core/src/hw/vi.c:1-8; core/src/cpu/gekko.h:59-62 *Disc and VI timing in emulation are models. Informs: Timing model scope*
- perf.csv speed_pct definition: **speed_pct = (guest cycles per real second / 1e6) / 486; guest cycles = 1 per guest instruction plus skipped idle/polling cycles** (all relay runs) [estimate] core/host3ds/main.c:876-878, 910-911; core/src/cpu/gekko.c:388-453 *A CPI=1 Gekko model: speed is emulated-time ratio, not real-Gekko equivalence. Idle skips count as emulated time. fields_s is the matching per-field rate. Informs: Reading every speed figure*
- Size of typical A/B effects vs model error: **A/B effects 1-5%, smaller than any model's error; the 2DS stays final judge** [estimate; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:232 *Informs: Defaults must be decided on the device, not the model*
- Stale or contradictory defaults in source comments: **pica.h:44 says the ring default is 256 KB, but the code uses 128 KB. main.c:360 says --vin defaults on, but opts_default sets it off. main.c:306-309 says the HLE stand-in is off, but it is on since job 145. pmu.h says four pairs; there are five.** (code reading) [estimate] core/src/gx/pica/pica.h:44; core/host3ds/main.c:306-309, 360, 402; core/host3ds/pmu.h:4 *Check the code, not the comments, when reconstructing which arm a run used. perf.csv option columns record the actual state per row. Informs: Reading old runs correctly*
- System tick rate vs CPU clock: **svcGetSystemTick counts at 268,111,856 Hz even in 804 MHz mode; CPU 268 MHz unless the title flag or Home Menu raises it to 804** (libctru os.h) [estimate; 2026-10-10] docs/ARCHITECTURE.md:252-253 *Tick-based timings must be scaled x3 to get 804 MHz cycles. Informs: Converting perf.csv/probe ticks to cycles; guest timebase derivation*
- timingprobe reported max: **max is computed over pair runs 0-2 only, not all 5** (code reading) [estimate] core/host3ds/timingprobe.c:107-112 *An instrument bug. The run-to-run spread is understated. cyc (the minimum) is correct. Informs: Variance reading of probe lines*

### Other (49)

- Boot logs (log_*.txt): **All 9 logs are identical boot transcripts: the apploader finishes in 2,481,563 emulated cycles, plus TEV approximation flags. They hold no performance data.** (TP boot) [device; 2026-10-11/12] core/out/log_2026-10-11_{ab5,fixed,gpupos,hle,idx,memstub,pipefast,split4}.txt, log_2026-10-12_hle.txt *Informs: None. They are only a record that boot was deterministic.*
- CPU clock modes (gcbench): **109 vs 329 MIPS between 268 MHz and 804 MHz** (gcbench) [device] core/host3ds/main.c:1743 *Informs: Explicit PTMSYSM_ConfigureNew3DSCPU(3) at startup*
- Fault-handling probe (fastmem feasibility): **A data abort reaches the handler with pc +0. 200 faults at 1,650 ns each. An instruction-cache sync (svc) costs 21,376 ns. MEM1 maps at host 0x0800cfe8. The uncached mirror, EFB, MMIO, gather pipe, locked cache and guest 0 all fault. Unaligned ldr is correct.** (--probe-faults on) [device; relay job 85, 2026-10-12] core/out/relay_85.csv ('# fault probe') *Informs: Fault-backpatching is feasible per site: about 23 µs once per site*
- Fault-mode crash evidence in the logs: **Luma arm11 dump count 9 -> 10 between jobs 167 and 168; job 170 read dump #9: 332 bytes, values incl. pc fffffffe and 00000094 (svc 0x94); dumps #10/#11 'cannot read'** [device; jobs 167-170] '# luma dump' lines, relay_168-170.csv *My reading of the dump words follows HANDOFF's interpretation (prefetch abort inside svc 0x94) Informs: Root cause of fault mode's kernel crash*
- First hardware run: interpreter + software renderer: **7-10 MIPS (1.5-2% speed) on consoletest / gx_triangle** [device; 2026-10-10] git edd3e22 body *Informs: Baseline before the JIT*
- H7 3DS motion sensor rate: **gyroscope ~90 updates/s; accelerometer ~98/s; poll 0.8 us** [device; gcbench 6, 2026-10-11] docs/research/GCWII.md:247 *Dolphin runs Wii Remotes at 200 Hz: interpolate or repeat. Informs: Wii Remote motion emulation*
- H7: gyroscope and accelerometer update rate: **gyro 90 changes/s, accelerometer 98 changes/s; hidScanInput + reads 0.8 us (545510 polls)** (gcbench 6, device still) [device; 2026-10-11] bench/results/gcbench6_2026-10-11.txt L17-20 *Informs: Wii Remote motion emulation (not TP GC)*
- Heap fragmentation across jobs: **second job without restart starts with malloc's largest block at 1.1 MB (jobs 145, 150) vs 17-20 MB fresh** [device; 2026-10-14] docs/HANDOFF.md L1008-1009, L863 *Informs: relay restarts process when a game already ran*
- Heap left for the relay: **a 28 MB build left too little heap: socInit failed** (large (AOT) build) [device; 2026-10-14] core/host3ds/relay.c:217-219 *Informs: relay_reserve before large allocations*
- Kernel DMA (svcStartInterProcessDma) 4 MB FCRAM->FCRAM, and core 0 interference: **192 MB/s; core 0's 1 MB chase 34.5 ns alone and 34.5 ns while the DMA copies** (default DMA config, no crash) [device; 2026-10-15, job 196 (--probe-risky)] relay_196.csv '# timing dma' *Informs: Offloading bulk copies from core 0 to DMA: slower than memcpy (~300 MB/s) but does not pollute L2*
- Memory headroom in TP emulator mode: **largest malloc block 3.9-4.2 MB (5.7 MB with the smaller indirect table, job 193); linear free 11.6-13 MB; VRAM free 1,266 KB** (tp850 state loaded) [device; 2026-10-15] '# heap: largest malloc block' lines and columns linear_free_mb, vram_free_kb in relay_171.csv-relay_196.csv *Informs: Any new cache or buffer (DL cache, code cache, textures in VRAM) has little room. VRAM has ~1.2 MB free.*
- Memory headroom, early: **VRAM ~1.2 MB free; JIT code buffer 91% full and still growing (full buffer drops all code)** [device; 2026-10-10] docs/HANDOFF.md L366
- Older switches re-measured on the 10-13 build: **pld: core0 23.07 off / 23.81 on (SE ~0.6); sched 0 vs 3: 24.49 / 24.44; splitopen 0 vs 1: 24.04 / 23.76** (300 s each, cutscene) [device; jobs 120, 123, 124] core/out/relay_120.csv, relay_123.csv, relay_124.csv, ab_split.py *120 and 124 have SE 0.5-0.6, so they are inconclusive Informs: Defaults kept*
- On-device JIT fuzzer: **10 failures, all float results the ARM11 flushes to +0.0 where the interpreter gives -0.0; 851 trace side exits matched; qemu does not show it. Later 40,000 cases, 177,598 side exits, no difference** [device; 2026-10-12] git 74ff8b0, 3ef701d bodies *Informs: qemu misses FTZ differences*
- Process9 (ARM9) crash: **Job 102 stopped at 111 s; ARM9 dump read in job 104 (700 bytes, deadc0de deadcafe ... with perf.csv text on the stack per HANDOFF)** (frames read back from the SD card at the time) [device; jobs 102, 104] core/out/relay_102.csv tail; '# luma dump arm9' line in relay_104.csv *Informs: Frames now uploaded from memory; watch arm9 dump count (stayed 2 through job 170)*
- Relay build transfer: **Builds 7.7 -> 4.1 MB gzipped** [device; 2026-10-13] git 56c647d body *Informs: Iteration speed*
- Relay turnaround: **build download 109-388 s per attempt (~110 s for ~4 MB); restart into the new build 10.7-18.5 s after the download (earlier hb:ldr path 113-481 s or unavailable)** [device; jobs 104-170] '# relay:' lines, relay_104-170.csv *Informs: Cost of a device iteration: ~2-6 min plus run length*
- Screenshot / SD write stall: **Row-by-row BMP write froze GX for 2.8 s after each screenshot; perf.csv SD flush on core 0 cost 4.5-4.7%; afterwards perf_log 4.6% -> 3.3-3.7%, console part 2.6%; the 10 s clock line cost ~21 ms of core 0** [device; 2026-10-11/12] git 88de143, d7ac114, 9b96050, afe3fc0 bodies *Informs: Keep I/O and logging off core 0*
- Shadow-scan HLE stand-in (--ab hle): **Link/Epona stretch 10.6-10.7% -> 11.9-12.4%; cutscene within noise; on by default; stand-in ~5 cycles per guest instruction replaced; tight loop (051987d) no measurable change** (2DS jobs 145-146, tp850, 600 s) [device; 2026-10-14] docs/research/AOT_PLAN.md:83-85 *Bound by collision-mesh memory. Informs: HLE pays only where one function dominates*
- THP movie playback speed: **~4.6% (game decodes JPEG in guest code)** [device; 2026-10-13, job 104] docs/HANDOFF.md L908-909 *Informs: native THP decoder as later item*
- AOT correctness check against the interpreter (PC): **4,287,122 calls (161,432,323 instructions) compared: 9 differed (memory byte at 803dacd7 in function 803696e8, plus 1-ulp FPRs), 72 differed only in FPRs** (PC, AOT_CHECK, TP 900-1020) [pc; 2026-10-14] aot/chk/out.txt *Earlier lc run showed paired-single lane-1 FPR differences (e.g. f31 ps1 3ff0... vs 4070...) in 802722b8/8000a604/8006a090 that are not 1-ulp; superseded by later builds (chk/ac show only 1-ulp). Informs: translator correctness bar; the known atan2 stack-store difference*
- HLE stand-in correctness on PC over a long stretch: **5,817 stand-in calls compared, 0 differed** (PC, HLE_CHECK, tp_pc_900.state to field 2500 (1,600 fields), 69.10 s wall, 187.7 MIPS) [pc; 2026-10-14] aot/hc.txt *PC interpreter path. Informs: HLE verification method*
- JIT dispatch hash map (consoletest): **Compiles in 10 emulated seconds 10,711 -> 2,328 with a chained hash map** [pc; 2026-10-10] git 424662b body *Informs: Block lookup*
- PC throughput: AOT vs interpreter over the same 200 fields: **AOT 3.46 s wall (469.2 MIPS) vs interpreter 11.60 s (139.8 MIPS); 19.7% of the cycle-based count translated; 5,707,257 slow accesses** (PC x86 gcemu, tp_pc_900.state to field 1100) [pc; 2026-10-14] aot/fa/out.txt, aot/fi/out.txt *PC host; final state hashes differ (6104d1d3... vs 18a85dfe..., cycles 8,920,240,672 vs 8,919,113,950), i.e. timing differs; not a device prediction. Informs: PC-side speed only; not device*
- TEV shader cache check: **0 of 4,414 cache hits differ from a fresh compile; PC boot-1500 54.7 MIPS (new) vs 53.7 MIPS (old)** (PC, from boot, 1,500 fields) [pc; 2026-10-14] aot/tc_1.txt, aot/tc_old.txt *Title-screen heavy run (96.6% idle). Informs: TEV cache key is safe*
- AOT correctness check under qemu (ARM build): **5,313,619 calls (202,914,381 instructions) compared: 9 differed, 904 differed only in FPRs; 0 not compared** (qemu-arm, AOT_CHECK, tp_arm_850.state to field 1000 (150 fields); 194.69 s wall) [qemu_model; 2026-10-14] aot/ac/out.txt; aot/g_arm.txt (3m15s run, same diff sites) *FPR-only differences are 1 ulp (fast-float model). Informs: ARM back end matches the PC translator*
- HLE shadow-scan stand-in correctness under the qemu JIT path: **1,236 stand-in calls compared with the interpreter, 0 differed (hlejit2); 1,235 calls, 0 differed (hlejit, before the budget fix)** (qemu-arm JIT, boot to field 1100 (1,100 fields from boot), 8,993.69 s / 7,734.89 s wall) [qemu_model; 2026-10-11/12] //wsl$/Debian/root/hlejit2.txt, hlejit.txt *Correctness only, no timing. Informs: HLE stand-ins can be verified bit-exact; supports more native stand-ins*
- qemu ARM instruction counts with and without at-site pipe appends: **8.093G vs 8.125G ARM instructions (TP 850-870)** [qemu_model; 2026-10-14] git 4b313d7 body *Informs: Minor*
- Azahar reliability for native EFB draws, negative: **Azahar itself crashes on the first draw into the native EFB (full-screen fade quad) with the GX thread, worker, merge and shader transform each off; the 2DS renders it** [azahar; 2026-10-15] docs/research/NATIVE_PORT_PLAN.md:140-142 *Unexplained. Informs: Azahar cannot gate native rendering; use the device*
- ARM11 MPCore TRM fetch/branch facts (documented, not measured): **128-entry direct-mapped BTAC indexed VA[9:3] (branches 1 KB apart alias); register-target branches never predicted (5-7 cycles); 3-entry return stack; folded B 0 cycles, unfolded predicted 1, mispredict 5-7; I-cache 16 KB 4-way 32 B lines round-robin, virtually indexed (code 4 KB apart shares a set); L2C-310 prefetch secure-only** [estimate; 2026-10-11/12] docs/HANDOFF.md L161-165, L550-554 *ARM DDI 0360F document values; program-flow prediction enable bit (CP15 Z) not verified for our process*
- ARM11 TRM result latencies (documented): **LDR result 3 (dependent op stalls 2, 3 as shifter operand/base); REV 1; VFP add/mul 8/9; FLDD 4; FMRS 2; FMSR 4; FDIVD 29+ blocking; store buffer forwards to loads** [estimate; 2026-10-11] docs/HANDOFF.md L225-227 *document values used by arm11_sched.py model*
- .cia vs .3dsx dynarec speed (RetroArch): **no difference** (libretro, Luma >= 8.0) [unknown; 2026-10-10] docs/ARCHITECTURE.md:346-347 *Third-party. Informs: Delivery format (speed is not a reason for .cia)*
- Device-measurement gaps still open (from coverage table and survey): **GPU texture-cache geometry, triangle setup rate, lighting/fog/shadow stage costs, NDMA rate, Y2R, DSP per-voice cost, MemoryFill timing, VRAM-texture contention, cache/model not calibrated; geometry shader untested** [unknown; 2026-10-15] docs/research/TIMING_MODEL_PLAN.md:42-57,132,201; docs/research/BENCHMARK_TOOLING_SURVEY.md:123-132; docs/research/GPU_GUIDANCE.md:122-127 *Coverage table marks Model 'no' for every mechanism except I-cache sim and a partial uncalibrated scheduler model. Informs: What a measurement-driven redesign still lacks*
- Dolphin GameSettings feature counts (1,891 INI files): **texture-cache safety 361; EFB copies to RAM 157; single-thread CPU/GPU 98; DSP LLE 79; CPU EFB peek/poke 44; EFB format change 35; defer EFB copies off 33; MMU 32; vertex rounding 25; FPRF 24; XFB-to-texture off 22; SyncOnSkipIdle off 15; bbox 8** (Dolphin Data/Sys/GameSettings) [unknown; 2026-10-11] docs/research/GCWII.md:35-55 *Lower bound; some files only restate defaults. Informs: Which GPU readback and timing features matter beyond TP*
- Dolphin on 32-bit ARM / in-order CPUs: **32-bit ARM JIT dropped; 'really terrible on in-order CPUs' (A53 class)** (press/developer statements) [unknown; 2026-10-10] docs/ARCHITECTURE.md:432-435 *Third-party, qualitative. Informs: Not using Dolphin's design as a template; trading accuracy for speed*
- GAP: array contents changing between display-list replays (CPU skinning): **NOT MEASURED** [unknown] docs/research/PERF_LEVERS.md:91 (D1) *dlcache page stamps never invalidated (jobs 113-119), which suggests little change, but contents were not hashed. Informs: C1 GPU-resident vertex data*
- GAP: block-execution distribution and loop coverage; known vs indirect exit traffic: **NOT MEASURED (open questions)** [unknown] docs/REGION_JIT_PLAN.md:98-101 *Region designs were ruled out by model (~1.3%/0.6%) without these. Informs: Region compilation*
- GAP: Gekko 25-bit frC cut (truncate vs round); fault-mode kernel crash cause: **OPEN / NOT ROOT-CAUSED** [unknown] docs/research/REDESIGN_CANDIDATES.md:107-108 (C8); OPT_JIT_PLAN.md:101-102 *Informs: C8 single precision exactness; fault mode retry (~+11% core 0)*
- GAP: GPU time with headroom; texture decode CPU share: **Only ~20 ms of ~190 ms GPU cited; texture decode share NOT MEASURED** [unknown] docs/research/REDESIGN_CANDIDATES.md:25-26,235-236 *Informs: C1, C17*
- GAP: guest word loads per frame (price of rev, C18): **NOT MEASURED** [unknown] docs/research/REDESIGN_CANDIDATES.md:249-250 *Informs: C18 byte-swapped guest RAM*
- GAP: hot-code footprint per field (distinct hot I-cache lines): **NOT MEASURED — no counter exists** [unknown] docs/research/REDESIGN_CANDIDATES.md:199-200 (C13) *The docs identify footprint as the predictor of I-cache misses but have no way to measure it before an A/B. Informs: Predicting JIT changes before building them*
- GAP: PICA a0 relative addressing for per-vertex matrix choice: **UNVERIFIED on hardware** [unknown] docs/research/REDESIGN_CANDIDATES.md:86-87 (C6) *Informs: C6 texgen/normals in shader; C1 stable palette slots*
- GAP: share of core 0 spent in guest idle/busy-wait loops: **NOT MEASURED (flat profile argues little)** [unknown] docs/research/REDESIGN_CANDIDATES.md:136-139 (C10) *Informs: Idle-loop skipping / underclock (C10, lever 6)*
- Hardware levers ruled out by documentation: **DSP coprocessor (RSA-signed firmware only); ARM9 at run time; MVD (H.264 only); AXI WRAM (kernel-only); large pages for JIT buffer (no SVC); L2C-310 prefetch tuning (Secure-only); PICA vertex texture fetch and per-attribute indices (do not exist)** (3dbrew, ARM DDI 0246H, picasso docs via Haiku research) [unknown; 2026-10-13] docs/research/PERF_LEVERS.md:76-84 *Sourced from documentation, not measured. Informs: Rules out several redesign directions*
- Memory regions available to the app: **VRAM 6 MB; AXI WRAM 512 KB; DSP RAM 512 KB; New 3DS extra RAM 4 MB (2 MB GPU-reachable); APPLICATION region 124 MB (178 MB Dev1 exheader); L2C-310 2 MB** [unknown; 2026-10-11] docs/research/PICA200.md:388-395 *Documentation. 3dsx cannot map VRAM for CPU or pick memory mode; CIA build could. Informs: Whether textures can move to VRAM; CIA vs 3dsx*
- MMU-dependent titles: **Dolphin: 21 GC titles need more than the MMU speedhack; Faster MMU 2 gave ~80% in Rogue Squadron; 32 GameSettings files force MMU** (Dolphin blog / settings files) [unknown; 2026-10-11] docs/ARCHITECTURE.md:60-64; docs/research/GCWII.md:49 *The two counts use different methods. Informs: BAT-only fastmem vs software TLB*
- Published 3DS clocks (3dbrew): **New3DS ARM11 4x MPCore up to 804 MHz, optional 2 MB L2; GPU 268 MHz; DSP TeakLite 134 MHz 24 channels; ARM9 134 MHz; svcGetSystemTick 268 MHz** [unknown; 2026-10-15] docs/research/BENCHMARK_TOOLING_SURVEY.md:21-30 *Spec, not measured; device clock check read 757 / 252 MHz (job 178).*
- Published cycle-level 3DS measurements found by survey: **none (L1/L2 latency, PICA caches, NDMA, Y2R, DSP, cache maintenance: NOT FOUND)** [unknown; 2026-10-15] docs/research/BENCHMARK_TOOLING_SURVEY.md:13-16,235-243 *Informs: Own probes are the only ground truth*
- TP-specific Dolphin settings: **GZ2 (GC) pins only VISkip=False and CPUCull=True; RZD (Wii) also needs CPU EFB access** [unknown; 2026-10-11] docs/research/GCWII.md:57 *Informs: TP needs no EFB-to-RAM or MMU path on GC*

## Sources

Readers and what they covered: docs-hw (142); docs-handoff (148); docs-plans (110); docs-arch (97); relay-early (83); relay-mid (77); relay-late (80); memory-azahar (104); code-comments (94). Follow-up readers covered the
sources the critic found unread. The workflow's full result (assessments, critic, verification votes) is kept outside
the repo; regenerate this file only by re-auditing, and otherwise edit it by hand as results arrive.
