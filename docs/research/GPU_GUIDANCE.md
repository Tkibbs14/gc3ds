# Guidance for the GPU work, from measurements on the 2DS (2026-10-11)

These rules come from gcbench 4 and 4b, run on James's New 2DS XL. The raw tables are in MEASURE_PLAN.md
("Results"); the reasoning behind them is in PICA200.md.

Each rule is a measured result, not a guess. Where a rule changes the plan in HANDOFF.md ("NEXT SESSION: move
everything we can onto the GPU"), it says so.

## The vertex shader and its inputs

1. **The matrix palette works, and with its index packed into position.w it is free.**
   - Measured: 6.69 Mtri/s, the same as a plain 3-attribute shader with no palette (6.67).
   - The design: `MOVA a0.x, v0.w` (never two MOVA in a row), then `DP4 r.x/y/z, pal[a0.x+0/1/2], pos`.
   - 28 matrices fit; 1, 8 or 28 distinct matrices measure the same.
   - Reference shader: `bench/src/gpu_pal3.v.pica`.
2. **Every vertex attribute beyond three costs ~45% of the vertex rate, so pack.**
   - Measured: 2 or 3 attributes 6.7 Mtri/s; 4 attributes 3.6–3.7 Mtri/s. The cost is the attribute count, not the
     stride: 28-byte vertices with 3 attributes run at full speed.
   - Carry the palette index in position.w, and combine other small inputs where possible.
   - Keep this in proportion: TP needs ~1–2 Mtri/s, so even 4+ attributes fit. This matters only once the GPU is
     the limit, which it is far from.
3. **Shader length is free at these sizes.** +128 MAD instructions changed nothing (6.0 Mtri/s). Do lighting,
   texgen and normalisation in the shader without worrying about instruction count (limit ~512).
4. **Integer attributes are as fast as float** (s16: 6.02 vs 6.01 Mtri/s).
   - Feed GameCube s16 and u8 arrays as they are (byte-swapped) and apply the fixed-point scale in the shader. The
     PICA does not normalise integer attributes.
   - This removes the CPU's float conversion.
5. **Vertex buffers in FCRAM are as fast as in VRAM** (6.01 vs 6.01). Keep them in the linear heap.

## Draws and state

6. **Write draws as raw register commands, not `C3D_DrawArrays`.**
   - CPU per draw: 3.25 µs through citro3d against **0.74 µs** for the minimal sequence.
   - The sequence: RESTART_PRIMITIVE, NUMVERTICES, VERTEX_OFFSET, GEOSTAGE_CONFIG2 on, START_DRAW_FUNC0=0, DRAWARRAYS,
     START_DRAW_FUNC0=1, GEOSTAGE_CONFIG2 off, VTX_FUNC. Config writes go before the draw window, never inside it.
   - Reference: `bench/src/gpu_w2.inc`, `draw_many`.
   - The GPU side is ~0.7–1.3 µs per draw either way.
7. **A uniform change between draws stalls the GPU about 2 µs** (one matrix: GPU 0.73 → 2.8 µs per draw; CPU
   +0.7 µs).
   - Upload the palette when matrices change, not per draw.
   - Fill palette slots incrementally and only start a new batch when the palette is full.
8. **Triangle strips are 2.4× faster than lists** (11.96 vs 5.0 Mtri/s for the same triangles). Pass GameCube strips
   and fans through as GPU strips/fans, with RESTART_PRIMITIVE between them.
9. **Texture format does not change fill rate**: RGBA8, RGB565, RGBA4, L8 and ETC1 all fill at 193 Mpx/s. Choose
   formats for memory and upload size only.
10. **The combiners are bit-exact 8-bit** with the expected formulas: MODULATE x·y/255, ADD_SIGNED x+y−128,
    SUBTRACT, INTERPOLATE, MULTIPLY_ADD and ADD_MULTIPLY all saturate; scale ×2 is exact. GameCube TEV operations
    that map onto them can be exact.

## Fog (the dunes)

11. **Use W-buffer depth mapping and build the fog LUT linear in eye distance.**
    - With citro3d's default z-buffer mapping, the fog index is 128·(z/w). With near 1 and far 100, everything past
      24 units falls into indices 0–5.
    - With `C3D_DepthMap(false, 0.0f, 1.0f / far)` the index is 128·d/far, linear in distance (measured 2 at d=1.5,
      123 at d=96).
    - Fill entry i from GX's fog function at eye depth d = (i/128)·far, using GX's A, B, C and fog type (formulas in
      PICA200.md §8).
    - The depth test runs on the same w-based value, which is monotonic, so depth ordering still works. **Check
      z-fighting on TP's terrain after the change.**

## Copies and memory

12. **Never use the display transfer's 2×2 downscale as configured here** (240×400 → 120×200). It hung the GPU in
    both runs. Do EFB half-scale copies another way until a working configuration is found.
13. **DMA engine timings** (submit to idle, small fixed overhead included):
    - 400×240 RGBA8 tiled→linear: 0.57 ms.
    - The same with a convert to RGB565: 0.27 ms.
    - TextureCopy: ~600 MB/s.
    - Clearing a 400×240 colour + depth target: 0.17 ms.
14. **Flush with `svcFlushProcessDataCache`, and only what changed.**
    - Measured: 26 µs for 4 KB, 195 µs for 256 KB, 610 µs for 1 MB.
    - `GSPGPU_FlushDataCache` is 1.5–8× slower.

## Cores (for the threading design)

15. **A cross-core round trip costs ~0.45 µs** (core 0 ↔ core 2 ping-pong).
16. **Two cores writing one 32-byte cache line halves both their speeds** (75 vs 160 M increments/s each). Give
    every core's hot fields (queue heads, counters) their own cache line.
17. **WFE does not sleep in user mode** (it returns at once), so spin, or wait in the kernel.
18. **Core 1 stays at 80%** (85% and 89% are refused under the Homebrew Launcher).

## Audio (later)

19. **GameCube DSP-ADPCM plays on the 3DS DSP directly** (matched our decoder exactly), and 24 channels cost the ARM11
    nothing measurable. See AUDIO.md for the plan.

## Added after gcbench 5

20. **Several draws can share one draw window.**
    - Open GEOSTAGE_CONFIG2 / START_DRAW_FUNC0=0 once, then per draw write only RESTART_PRIMITIVE, NUMVERTICES,
      VERTEX_OFFSET and DRAWARRAYS; close the window after the last draw.
    - Measured: **0.34 µs CPU per draw** (raw 0.74, citro3d 3.25), GPU 0.66 µs. Drawn correctly, no hang.
    - Reference: `draw_onewindow` in `bench/src/gpu_w2.inc`.
    - It does not cover changes of state between draws (uniforms, textures): those still close the window.
21. **Early depth works and is large:** 11.6× less fill time when occluded layers are rejected early (front layers
    first). It matters only when fill is the limit; consider it for GX's early-Z (zcomploc) opaque passes.
22. **A framebuffer flush + invalidate between draws costs ~0.8 µs GPU.** An EFB copy mid-frame is cheap on the GPU
    side.
23. **Vertex order matters for indexed draws:** 10.1 Mtri/s with good reuse order against 3.7 shuffled. Keep GX's
    index order (strips and fans already have good locality); never reorder indices randomly.
24. **The 2×2 downscale works from a 2× target** (480×800 → 240×400, 0.15 ms). Only the small 240×400 → 120×200
    case hangs. For EFB half-scale copies, test the exact dimensions first.
25. **The GPU does not slow CPU memory:** memcpy ran at 414 MB/s with the GPU filling, against 376 idle.
26. **For the JIT:** a load whose result is used by the very next instruction costs 2 extra cycles. Emit loads
    (register-cache fills, memory fast paths) two instructions before their use where possible.
27. **Kernel cross-core wake-ups are lost about 1 in 4** (svcArbitrateAddress, as LightEvent before). Spin for
    cross-core handoffs, or always wait with a timeout.
28. **Audio:** time-stretching (WSOLA) costs 0.66% of a core, so normal-pitch audio below full speed is affordable.
    ndsp plays 8–192 kHz; its sound frame is 4.88 ms.

## Added with the vertex-shader transform (core, 2026-10-11; a float24 model on the PC, not a 2DS measurement)

29. **Never send world coordinates to the shader as they are.** The PICA computes in 24-bit floats (16 mantissa
    bits): Twilight Princess's terrain at model coordinates ~40,000 lands up to 1.04 EFB texels off (906 of 4.7 M
    on-screen vertices over 0.25; `core/tools/vfast_check.sh`, `VFAST_GPU` lines). Send positions relative to an
    origin and fold the origin into the matrix translation in f32 on the CPU (`vfast_job_origin`): 0.114 texels at
    most. Round the origin to a coarse grid (cells 4-8 eye distances) so draws that share a matrix still share a
    palette entry: 81% of TP's draws reuse the previous draw's entries, against 86% without an origin and 40% with
    an exact per-draw origin.

## Not yet measured

- CPU memcpy speed while the GPU fills: the test was invalid, because the compiler removed the copies.
- A working configuration for the display transfer's 2×2 downscale.
- The geometry shader, and several draws inside one draw window. Both are deliberately untested because they can
  hang the GPU.
