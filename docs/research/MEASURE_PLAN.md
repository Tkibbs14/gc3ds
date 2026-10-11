# Hardware measurement plan (the 2DS is the ground truth)

Research (PICA200.md, AUDIO.md) answers what the hardware *can* do. This plan covers what nobody has published:
each test settles one open question on James's New 2DS XL.

**Home:** the tests go into `bench/` (gcbench; build with `build.sh`, check with `test/run_qemu.sh`, results in
`sdmc:/gcbench/results.txt`), or into the hwprobe section of gc3ds where they need emulator state.

**Rules:**
- Every test logs its raw numbers and a pass/fail against an expectation.
- Every test runs in Azahar (software renderer) before it goes on the card.
- A test that can hang the GPU runs last, behind a watchdog.

**Already measured** (FINDINGS.md), not repeated here:
- triangle rate (6 / 10 indexed / 4.3 lit Mtri/s);
- fill rate (193 Mpixels/s, VRAM textures);
- texture upload (~130 MB/s);
- render to texture, and 256×256 readback (3.9 ms);
- memcpy (~307 MB/s) and memory latency;
- core speeds and the core 1 limit;
- cross-core spin handoff (~0.9 µs);
- branch misprediction cost (under 1% of core 0);
- the I-cache stall share (~40% of core 0 in TP).

## GPU

| # | Question | Test | Decides |
|---|---|---|---|
| G1 | Vertex-shader cost per instruction | the same 20k-triangle draw with shaders padded by 0/16/64/128 MAD instructions; triangles/s | how long the palette + texgen + lighting shader may be |
| G2 | Matrix-palette shader | palette shader (MOVA + 3×DP4 + DPH) vs the plain shader, with 1/8/29 distinct matrices | the GPU-transform design (other session, step 1) |
| G3 | Uniform upload cost | upload 4/12/48/96 vec4 per draw for 1000 draws; CPU µs and GPU time | how often the palette may change |
| G4 | CPU cost per draw call | 1000 draws of 6 vertices: C3D_DrawArrays vs a hand-written minimal register sequence (PICA200.md §5); CPU µs per draw | whether raw command writing is worth it |
| G5 | One draw window, several draws | several DRAWARRAYS between one START_DRAW_FUNC0=0/1 pair; correctness + speed (can hang: watchdog) | cheaper batching |
| G6 | Strips/fans + primitive restart | the same mesh as triangle lists vs strips with restart vs indexed; triangles/s | whether to pass GX strips/fans through |
| G7 | **Fog depth source (the dunes)** | a depth ramp with an identity fog LUT, in z-buffer and W-buffer modes, with and without zFlip; read back pixels | the fog LUT construction; likely the dune fix |
| G8 | Depth-register conflicts | write 0x107 and the gas-depth register separately; check the depth test and write mask | which citro3d/3dbrew description is right |
| G9 | Vertex fetch: VRAM vs FCRAM | vertex buffers in each; triangles/s | where vertex arenas live |
| G10 | Integer attributes | u8/s16 attributes with a scale uniform vs float attributes; triangles/s and CPU conversion time | skipping CPU float conversion |
| G11 | Fragment lighting cost | 1/2/4/8 lights per pixel at full screen; Mpixels/s | per-pixel lighting as an option |
| G12 | Texture units and formats | fill with 1, 2 and 3 units; RGBA8 vs RGB565 vs ETC1 vs I8 | texture format choice |
| G13 | Early depth | front-to-back overdraw with early depth on and off | terrain overdraw |
| G14 | Display transfer and TextureCopy speed | 400×240 and 640×480 tiled→linear, format conversions, 2×2 downscale; µs, async with PPF | EFB→XFB and EFB copies on the DMA engine |
| G15 | Framebuffer and texture cache flush cost | time the 0x111/0x110 and texture-cache-clear writes inside command lists | per-copy overheads |
| G16 | Geometry shader | points → quads with a GS vs CPU-expanded quads; triangles/s | GX points and lines |
| G17 | Post-vertex cache | indexed meshes with varying index locality | index ordering |
| G18 | Combiner rounding | known inputs through ADD_SIGNED, MODULATE, INTERPOLATE; read back | exactness of the TEV mapping |
| G19 | Clip space | triangles crossing z = 0 and z = −w, and crossing w = 0 | near-plane handling (Azahar says z ∈ [−w, 0]) |

## Memory

| # | Question | Test | Decides |
|---|---|---|---|
| M1 | Bandwidth by heap | read/write/copy MB/s for app heap vs linear heap, 4 KB to 32 MB | where the emulator's hot buffers live |
| M2 | **Contention** | M1 while the GPU renders a fill-heavy scene and a display transfer runs | real bandwidth during a frame |
| M3 | Cache-flush cost | svcFlushProcessDataCache / GSPGPU_FlushDataCache for 4 KB to 4 MB | upload and command-buffer flush strategy |
| M4 | L2 effect | latency sweep with the speed-up mode on/off (if switchable at run time) | working-set sizing |
| M5 | Prefetch (PLD) | strided and random reads with and without PLD ahead | vertex gather and texture decode loops |
| M6 | Cross-core cache-line cost | a flag ping-pong on one line vs separate lines; ns per round trip | handoff design |

## CPU

| # | Question | Test | Decides |
|---|---|---|---|
| C1 | WFE/SEV from user mode | a thread waits with WFE; another core issues SEV; wake latency vs spinning (crash-guarded) | sleeping instead of spinning between cores |
| C2 | Core 1 limit above 80% | APT_SetAppCpuTimeLimit at 85 and 89; measured share | a fuller fourth core |
| C3 | svcArbitrateAddress round trip | wake latency across cores | backoff after spinning |
| C4 | Load-use scheduling | JIT-style sequences with the load result used next vs 2 instructions later | JIT instruction ordering (manual: load latency 3) |

## Audio

| # | Question | Test | Decides |
|---|---|---|---|
| A1 | **GameCube DSP-ADPCM straight into ndsp** | a known DSP-ADPCM clip (generated by our own encoder) played by ndsp with its coefficients and context, vs a CPU decode | ADPCM decoding offload |
| A2 | ndsp CPU cost | ARM time with 0, 8 and 24 active channels | how many voices go to the DSP |
| A3 | ndsp rate range and interpolation cost | channels at 8–48 kHz, polyphase vs linear | resampling on the DSP |
| A4 | Buffer latency | smallest wave buffer without underruns at 32728 Hz | audio latency |
| A5 | Time-stretch cost | a WSOLA stretch of PCM16 on core 1; µs per 5 ms frame | sound below full speed |

## Order (most decision value first)

1. **G7** (fog: a likely visible fix).
2. **G2, G1, G3** (the GPU transform the other session is building).
3. **G4, G6** (draw batching).
4. **A1** (audio offload).
5. **M2, M3** (real memory budget).
6. **C1** (core wake-ups).
7. **G14** (copies on the DMA engine).

Then the rest.
