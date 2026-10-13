# Redesign candidates: ideas parked with their evidence, re-evaluated later

Owner direction (2026-10-13): anything with redesign potential is logged here and re-evaluated later, never dropped.
Each entry says what it is, the evidence for and against, what it would cost, and what would make it worth another
look ("re-evaluate when"). Review this list whenever a bottleneck moves (a new cap, a new measurement); update the
status line instead of deleting entries. Ranked levers that are already being built live in PERF_LEVERS.md.

Status values: **parked** (not started, waiting for its trigger), **next** (chosen for upcoming work), **built**
(done; the entry stays with its result), **rejected** (measured and refuted; the entry keeps why).

## C1. GPU-resident vertex data for repeated display lists

- **Status:** parked (2026-10-13).
- **What:** keep the gathered vertices of a display-list draw in GPU-readable linear memory, so a repeated draw is
  issued straight from that buffer with no per-vertex CPU work (no gather, no copy into the arena).
- **For:** TP replays identical vertex data: the display-list cache hit 84-85% of lookups on the 2DS with no entry ever
  invalidated by a page stamp (jobs 113-119), and the PC checker found 0 differences over 12.36M hits. TP's heavy draws
  are already GPU-transformed (palette matrices) and shader-lit.
- **Against / cost:** each vertex carries per-draw data today: its palette slot (`slot3[pn]`, chosen per draw),
  colours from lighting, and texture coordinates after texgen. A persistent buffer needs per-draw palette slots to be
  stable or moved into an index the shader resolves, and texgen in the shader. Memory: ~25k vertices a field.
- **Why parked:** the CPU-side cache (DL_CACHE_PLAN.md) measured a hit at ~1.4 us a vertex, the same as a plain gather
  (~1.35 us), because both wait for RAM: the 6 MB record ring never stays in the 2 MB L2 (job 119). Only skipping the
  CPU entirely can cash in on the repetition.
- **Re-evaluate when:** the vertex worker stops being the cap (e.g. the second worker, C2, lifts it) and the GPU's
  time per frame is known with headroom; or a design for stable palette slots appears.

## C2. A second vertex worker on core 1

- **Status:** next: built 2026-10-13 (722caa0, `--vw2 on`, `--ab vw2`), first measurement job 125.
- **What:** core 1 (the app gets 80%, ~0.75 of core 0's speed) takes indexed and fixed-place jobs from the same queue;
  jobs finish out of order and g_vwTail moves over finished jobs in order.
- **For:** the worker on core 3 is the GX side's busiest part (~60% busy, the GX thread waiting for ring space ~23%).
- **Against:** core 1 also hosts the log writer and samplers; L2 and RAM bandwidth are shared, and the gather is
  memory-latency bound, so two workers may contend.
- **Re-evaluate when:** job 125's result is in.

## C3. Display-list vertex cache on the CPU

- **Status:** built, off by default (`--dlcache on`). Result: no gain (jobs 111-119); kept as the evidence for C1.
- **Re-evaluate when:** records could live in fast memory (they cannot: AXI WRAM is kernel-only, PERF_LEVERS), or if a
  much smaller record (positions only, the stream's own format) fits the L2 with the rest of the working set.

## C4. Shrink VIn (the worker's per-vertex record)

- **Status:** parked (2026-10-13).
- **What:** VIn is 108 bytes (8 texture-coordinate slots, 64 bytes, though TP uses one or two). Pack only the used
  coordinates, and keep the per-draw fields out of it (already done for the matrix indices: VIx, 9ecfeeb).
- **For:** splitting the matrix indices out of VIn alone moved cache-off speed from 20.0-20.8% (jobs 113, 115) to
  21.3-21.4% (116, 118): the worker's memory traffic per vertex matters.
- **Cost:** texgen and the finish phases index `t[unit]`; a packed layout needs a per-job map.
- **Re-evaluate when:** after C2's result (the same bottleneck); cheap enough to try next if C2 does not lift the cap.
