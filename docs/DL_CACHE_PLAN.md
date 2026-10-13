# Display-list vertex cache: design (lever 1 of research/PERF_LEVERS.md)

Status: built 2026-10-13 (vfast.c gather_cached, page stamps in gx.h), off by default: `--dlcache on`, `--ab dlcache`,
live `set dlcache 0|1`. Checked on the PC with `VCACHE_CHECK=1` (`VCACHE_PLANT=1` for its self-test); measured on the 2DS.

## Why

- TP's GX side (the GX thread on core 2 + the vertex worker on core 3) caps its speed today: core 2 waits for job-ring
  space 22-25% of its time and core 0 waits for the GX side 9-16% at its polling syncs (HANDOFF 2026-10-13).
- The vertex worker spends 62% of its time gathering attributes from guest RAM (`# vworker phases`, job 101).
- 99.6% of TP's draws come from display lists, and ~80% of list calls replay the same list bytes over the same array
  bases as last time (`# dlist`, job 107). TP's models are recorded once at load (`J3DShapeDraw::draw` replays
  `mDisplayList`; zeldaret/tp).

## What is cached

The gathered vertex input (VIn: positions, normals, colours, raw texture coordinates, matrix indices) of each draw
inside a display list, keyed by:

- the list's physical address, size and a hash of its bytes (the bytes are hashed only when the entry is new or the
  memory was flushed, see below);
- the draw's offset inside the list;
- the vertex format (VCD, the VAT of the draw) and the array bases and strides it reads.

What stays per frame: matrix selection and composition, positions relative to the job's origin, normals, texgen and
colours that depend on lights and matrices (17% + 11% of the worker), and everything on the GPU.

## When an entry is invalid

On a GameCube the GPU reads vertex arrays and display lists from main memory, so a game that changes them with the CPU
must flush them out of the CPU's data cache first (`dcbf`, `dcbst`; SDK `DCFlushRange` / `DCStoreRange`), or the real
GPU reads stale data. Every way memory can change under the cache is visible to us:

- `dcbf`, `dcbst`, `dcbi`, `dcbz` / `dcbz_l` (the JIT compiles or interprets each: mark the line's page dirty);
- stores through the uncached mirror (0xC0000000; they take the slow path today: mark dirty there);
- DMA into MEM1: disc reads (DI), ARAM to MEM1 (DSP DMA), EXI DMA: mark the range dirty.

Dirty tracking is a bitmap of 4 KB pages (24 MB / 4 KB = 6144 bits), each with a generation counter. An entry records
the pages its list and arrays cover and the generations it saw; a lookup compares them. A dirty page drops only the
entries over it.

Exactness: equal to the hardware's own coherency rule. A game that writes arrays and never flushes would show stale data
on a real GameCube too (rare; such a game would need `--dlcache off`).

## Memory

TP draws ~300k vertices/s at ~12 fields/s, ~25k vertices per field. VIn is 116 bytes, so ~3 MB per field's worth of
distinct draws; cap the cache at 4-6 MB with LRU by field, and shrink VIn (the unused texture-coordinate slots: 64 of the
116 bytes) first: that also cuts the job ring's bytes (`# vworker`: ~20 KB per job).

## Checks

- PC: VFAST_CHECK-style comparison, cached vs gathered VIn, bit-identical for every draw of TP to field 3000 (soft.c).
- PC: a planted bug (an entry kept across a flushed page) must be caught.
- 2DS: `--ab dlcache`; `# vworker phases` (gather share), `valloc_pct`, `gxwait_pct`, speed and core 0 speed.
- Count entries hit / missed / invalidated per second in a `# dlcache 10s` line.
