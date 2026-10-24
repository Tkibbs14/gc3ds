# G2: translated display lists (design, 2026-10-23)

EMULATOR_CEILING.md G2 proposed caching each repeated display list's translation on the GX thread. This is the design
that the first measurement (MEASUREMENTS.md "G2-0", jobs 1127-1146) supports, and the order to build it in. Every stage
sits behind `--g2` (off by default) and is scored with separate whole runs in one session.

## 1. What the measurement settled

| Question | Answer (2DS, four scenes) | Consequence |
|---|---|---|
| Do draw-carrying lists repeat? | Their structural key repeats from one picture to the next on **99.9-100% of draws** (TP heavy, Ordon, Mario Party 4, Wind Waker). | The cache can cover nearly all list draws. |
| One entry per list? | No: against the same list's last use only 51-98% of calls repeat; the same list is drawn under 2+ states in one picture (bp00 / bp40 / bp41 / TEV stage words differ). | **Several variants per list**, keyed by state. |
| Which state is the key? | The list's own read-before-write words: ~11-38 structural words a call. A whole-state signature (every register a draw could read) repeats 0% in Mario Party 4 and 21% in Wind Waker. | **Guard = the list's structural read set**, compared word by word (~15-40 compares a call). |
| What changes every picture? | Matrices (~50% of calls), the projection and TEV colour registers (Ordon: with them in the key 14.9% of draws repeat, without 99.9%). | **Parametric words are applied live, never guarded**: projection / viewport, TEV colour registers and konsts, ambient / material colours, fog, lights, matrices. |
| Side effects? | PE signals, copies, palette loads: 0-0.4% of calls in TP and Mario Party 4, 4.5% in Wind Waker. | Replayable (the replay calls the same functions) but never compared in check mode. |

What it does not change: list calls are ~32-34% of TP's GX thread (X5), so G2 alone moves TP's GX wall by at most
that share; Mario Party 4 spends ~86% of its GX thread in list calls. TP's heavy area is limited by core 0 today (the
joint line: core 0 waits on the GX thread ~0.5% of the time, job 1124; the GX thread is idle ~13.5%): a GX-thread
saving there shows up as a lower `gx thread where` busy share before it shows up as speed.

## 2. Where the GX thread's list time goes now (heavy area, job 1124, `--profile on`)

- **Converted shapes (G1) already turned each run of ~43 draws into one draw call** (2,030 of 2,660 runs a second;
  ~22% are CPU-deformed and stay on the old path). Their remaining cost is per run, in `ns_draw`: ~117 ms/s (~13% of
  the GX thread, ~55 us a run; job 1162, `# shapes phases 10s`): **batch state 54 ms/s** (flush, `update_tev`, the
  batch key built a second time, texture lookups and setters, the lighting set-up a second time, `apply_frag`,
  uniforms), **the draw call 25** (`C3D_DrawElements`: citro3d's own path, not C25's raw one), **the batch check 15**
  (batch key, TEV key and lookup, the lighting set-up), matrices and attributes 10, the shape lookup 7, entry 6.5.
  When a batch opens, the TEV key is built up to four times and the lighting set-up twice.
- **Decoding inside lists:** `dec_list` 2.5%, `dec_run` 2.1%, part of `dec_op` 4.2%, `dec_bp` 2.6%, `dec_xf` 0.9%.
- **The old path** for changing runs: vertex preparation and the job ring (`vprep` 9.3%, `vwalloc` 5.6%, `palassign`
  3.2%): G2 cannot skip vertex work, only state work.

## 3. The design

### 3.1 Entry, variants, validity

- **Hook:** `exec_one_`'s list call (0x40) at the top level (fifo.c). Nested calls and native mode are untouched.
- **Entry per address** (open-addressed table): size, its decodings, and up to four **variants**, each a guard list of
  `(word, value)` pairs (the structural words the list reads before it writes them) and the fragment made under it.
- **Byte validity:** page stamps over the list's pages (g_memStamps), as G1 does, now including the two writes that
  were never stamped: write-gather bytes held in RAM while the GP is unlinked (GXBeginDisplayList) and a save state's
  RAM. A sampled hash of the bytes (1 replay in 64; every checked replay) counts any change the stamps missed.
- **Variant choice:** compare each variant's guards against the live registers (most recently used first); none
  holds: run the decoder and record a new variant (evicting the oldest). A list decoded too often (bytes rewritten
  every frame: the state-only lists that miss) is left to the decoder.

### 3.2 The fragment, by stage

Each stage is a switch bit of `--g2`, measured on its own.

- **G2a, pre-decoded lists (built 2026-10-23, `--g2 on`):** the list's commands as ops (state writes, runs of draws,
  single draws, NOPs) replayed through the decoder's own state functions (`bp_write`, `cp_write`, `xf_write`,
  `load_indexed`, `draw`, the backend's `draw_list`), statistics included. Removes the parse: opcode dispatch, length
  checks, byte-order reads, vertex sizes, run scans. Its only guard is the vertex formats the parse reads (VCD / VAT
  words read before written: a draw's length depends on them). Exact by construction: the state functions run with
  the same arguments in the same order. Expected: the decode share inside lists, ~3-6% of TP's GX thread.
- **G2b, per-run batch memo:** for a replayed run under a variant, the backend's batch decision is a function of the
  variant's structural words and the run's index: keep the batch key words, the TEV program slot, the texture objects
  (with the texture cache's own validity: address, format, `tlutVersion`, `texVersion`, the copy list), the fragment
  state words and the lighting selection. On replay `ns_draw` takes them instead of building keys and looking them up;
  the open-batch check becomes one pointer compare. Applied live every time: matrix rows, texture matrix rows,
  projection, light uniforms. Expected: half of `ns_draw`'s ~12%, ~5-6% of TP's GX thread. Checked on the device like
  the ports (`--g2 check`: 1 replay in 8 also builds the decision fresh and compares; `plant` must differ).
- **G2c, TEV colours as parameters:** today the TEV compiler folds colour register and konst values into each stage's
  constant (`tev_pica.c`, `konst` byte slots), so an animated colour is a new program: a new key, a cache lookup and
  often a compile every picture (Ordon's tevreg1). Compile keyed by structure, with a per-slot record of which
  register channel feeds each constant byte; a slot folded from several values keeps the program value-keyed. This is
  what makes Ordon's variants hold, and it helps the old path too.
- **G2d, recorded state words:** per memo entry, the raw state words C25 writes (combiner stages, effect, texture
  units) kept ready, so a replayed batch skips `update_tev`, `apply_frag` and `bind_tex`'s code. C25's lesson: those
  paths cost through cold I-cache on core 2, not through their stores.
- **G2e, the old path's state:** CPU-deformed runs still prepare vertices each picture, but their batch decision can
  come from the same memo.

### 3.3 Exactness checks

- **PC:** with `G2=1`, the state hash and the last frame equal G2 off on the title spans (the soft renderer has no
  `draw_list`, so runs replay draw by draw there); `G2=check` compares 1 replay in 8 with the decoder run on a copy of
  the GX state; `G2=plant` skips one state write on checked replays and must report differences.
- **2DS:** `--g2 check` and `--g2 plant` the same way (the copy runs without a backend; a difference is counted, the
  decoder's result kept, and G2 switched off for the run). G2b onwards add their own compare of the memo's decision.

### 3.4 Measuring

Separate whole runs in one session, `--profile on` without `--gxdiag`, scored with `core/tools/runs.py`, on TP heavy
(`tp_area2.state`), the Ordon walk, Mario Party 4 and Wind Waker. Read speed and the GX thread's busy share
(`# gx thread where`'s idle). TP's heavy area is core-0-bound today, so a GX-side gain may read as speed only in the
GX-bound scenes (Mario Party 4's minigame, the title cutscene).

## 4. Order and status

1. G2-0 census: done (MEASUREMENTS.md "G2-0").
2. **G2a: built, exact (PC and 2DS check mode), measured (MEASUREMENTS.md "G2a"):** the GX thread 1.3-2.5 points less
   busy in all four scenes, **speed unchanged** (TP and Wind Waker are core-0-bound, Mario Party 4 waits on its vertex
   worker). Off by default.
3. Next, by the phase profile (TP heavy, ms/s of ~117 in `ns_draw`), cheapest first:
   - **the batch-open work done once:** the batch key, TEV key and lookup and the lighting set-up computed in the check
     are reused by the state step (today built again); exact by construction, no G2 machinery (part of 15 + 54);
   - **the converted draw call through C25's raw path** (part of 25; C25 cut a call 8.15 -> 5.3 us);
   - **G2b's memo** keyed by variant and run (the rest of the check and the key / lookup work in the state step).
   Then G2c (TEV colours as parameters) and G2d (recorded state words). Each GX-side stage turns into speed only where
   the GX thread is the limit: after core-0 gains, in Mario Party 4's minigame and in the title cutscene.
   **Done 2026-10-24 (MEASUREMENTS.md "The GX thread's batch-state steps"):** the batch-open work done once
   (`--nsonce`, `--drawonce`: exact, GX -0.1-1.1 points, speed even); the raw converted draw call (`--nsraw`: exact,
   no gain, fixed attributes keep the draws on citro3d); and, found by `# shapes state 10s`, the texture re-hash by
   page stamps (`--texfast 2`: **TP heavy +0.6-0.8%, Ordon +0.9%, Mario Party 4 +4.2%, Wind Waker +0.4%**, 0 misses
   in check mode; candidate default). G2b's memo is next only if the GX thread becomes the limit: after these the
   state step's remaining keys and lookups are a few ms/s.
4. Outside G2, found by the census: Mario Party 4's converted shapes made again after stamps whose bytes did not
   change (~180 ms/s of its GX thread; a G1 fix, its own task).
