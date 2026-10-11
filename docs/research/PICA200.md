# The 3DS GPU (PICA200): what it can do for gc3ds

Research digest, 2026-10-11. It comes from six web-research passes (vertex shader, geometry shader and primitive
engine, fragment pipeline, textures and DMA, command submission, other projects), plus the ARM11 notes in
HANDOFF.md.

Tags:
- **[S]**: read in the cited source.
- **[I]**: inferred.
- **[?]**: unresolved or found only in a search snippet.

Most pages were read through a summarising fetch, so verify bit layouts against the raw source before writing
register code. The main sources are listed at the end.

**Why this matters:** on the TP title scene the GPU draws for about 20 ms of each ~190 ms frame. The CPU side is
the limit.

## 0. What to take from this, in order

1. **Transform on the GPU with a matrix palette.** Indexed uniform reads exist (MOVA + a0.x) and there is room for
   ~29 3x4 matrices. Section 1 has the traps. No 3DS project was found doing this, so it is new ground.
2. **Submission is cheap; vertex work is not.** We send ~450 GPU draws/s, ~85 per frame. At ~11 register commands
   per citro3d draw that is ~1 ms per frame [I]. Writing raw command lists would save little. Moving per-vertex
   math off the CPU saves a lot.
3. **Native strips, fans, index buffers and primitive restart** [S]. GameCube strips and fans could go to the GPU as
   they are. RESTART_PRIMITIVE lets many tiny draws share one draw call [I].
4. **EFB to XFB, scaling and flips through the display-transfer engine** [S]. One DMA call does tile-to-linear,
   format conversion, 2x1 or 2x2 downscale and vertical flip.
5. **Fog (the dunes):** the fog LUT has 128 entries indexed affinely in 1/z, so distant terrain gets very few
   entries [I]. There is also an open question whether fog reads z or w. Section 3 gives a test procedure.
6. **Avoid the freezes:**
   - Two consecutive MOVA freeze the GPU.
   - A wrong attribute stride, or an output component not written exactly once, freezes it.
   - NaN in a command or uniform hangs it.
   - A missing FINALIZE hangs it.

## 1. Vertex shader

**Instruction set** [S] (3dbrew GPU/Shader_Instruction_Set):

| Op | Code | Op | Code | Op | Code |
|---|---|---|---|---|---|
| ADD | 00 | MUL | 08 | DPHI | 18 |
| DP3 | 01 | SGE | 09 | DSTI | 19 |
| DP4 | 02 | SLT | 0A | SGEI | 1A |
| DPH | 03 | FLR | 0B | SLTI | 1B |
| DST | 04 | MAX | 0C | BREAK | 20 |
| EX2 | 05 | MIN | 0D | NOP | 21 |
| LG2 | 06 | RCP | 0E | END | 22 |
| LITP | 07 | RSQ | 0F | BREAKC | 23 |
| MOVA | 12 | MOV | 13 | CALL | 24 |
| CALLC | 25 | CALLU | 26 | IFU | 27 |
| IFC | 28 | LOOP | 29 | EMIT | 2A |
| SETEMIT | 2B | JMPC | 2C | JMPU | 2D |
| CMP | 2E–2F | MADI | 30–37 | MAD | 38–3F |

- Operand descriptors carry swizzles, a negate per source, and a write mask.
- **Only one source per instruction may be a float uniform** [S].
- The "I" (inverted) forms move the wide register field to src2 [S] (Azahar shader_interpreter.cpp).

**Relative addressing** [S]:
- a0.x and a0.y are written by MOVA; aL by LOOP.
- They index **float uniforms only**: index = register + 7-bit constant, masked to 0x7F. Offsets outside
  -128..127 count as 0.
- **Indices of 96 or more read (1,1,1,1).** Clamp with MIN/MAX first.
- MOVA truncates toward zero.
- **Two consecutive MOVA freeze the PICA200** (picasso manual).
- Stacks: CALL 4, IF 8, LOOP 4. A BREAK with an empty loop stack hangs.

**Limits:**
- Float uniforms c0–c95 (vec4) [S]; integer uniforms i0–i3; boolean uniforms b0–b15 (3dbrew text says b0–b7 [?]).
- Temporaries r0–r15, inputs v0–v15, outputs o0–o15 (o7–o15 are vertex-shader only).
- Program size "≤ 512 instructions, 128 operand descriptors" [?] (debug info; code space is 4096 words by register
  width).
- 3 or 4 vertex units in parallel; the 4th is the primitive engine when no geometry shader runs [?].

**Precision** [S]:
- float24 is 1.7.16, so positions lose whole-unit precision near 65,536 [I].
- Uniforms can be uploaded as float32 words (bit 31 of FLOATUNIFORM_INDEX; citro3d does this).
- Infinity times 0 gives 0 on the PICA.

**Attributes** [S]:
- Types: signed byte, unsigned byte, short, float, each with 1–4 components.
- 12 attributes and 12 buffers (citro3d).
- Fixed (constant) attributes through FIXEDATTRIB_INDEX 0–11; 0xF is immediate mode.
- Permutation registers map attribute to vN.
- **The stride must equal the vertex size, or the GPU freezes or draws nothing** (3dbrew GPU/Pitfalls).

**Matrix palette sketch** [I]; the design is unverified, so test it in Azahar, then hardware:
- **Uniform layout:** c0–c3 projection; palette from c8, matrix k in c[8+3k .. 10+3k] (~29 matrices). The light
  uniforms the shader already uses take some of this space: budget it.
- **Per vertex:** one attribute holding 3k as an exact float (or a u8; check how it arrives in the shader).
- **Shader:** `MOVA a0.x, v1.x` (never two MOVA in a row), then `DP4 r0.x/y/z, pal[a0.x+0/1/2], v0`, then
  `DPH o0.x..w` against c0..c3. Normals: DP3 with the same rows (rigid or uniform-scale matrices).
- **Uploads:** only dirty matrices; draws inside the palette window stay in one batch.
- **Not measured anywhere:** cycles per instruction and uniform-upload cost. Measure both.

## 2. Geometry shader, primitive engine, clipping

**Geometry shader** [S]:
- It runs on the primitive engine and takes away one vertex unit.
- Enabled with GEOSTAGE_CONFIG 0x229 (bits 0–1 = 2), VSH_COM_MODE 0x244, and its own register block 0x280–0x2DF
  with separate uniforms.
- libctru: `shaderProgramSetGsh(prog, &dvlb->DVLE[1], stride)` and draws with `GPU_GEOMETRY_PRIM`. The devkitPro
  geoshader example is the reference.
- EMIT and SETEMIT (winding, primitive-emit flag, vertex id). Output is triangles; the buffer holds 4 vertices [?].
- Variable-size and subdivision modes are poorly documented: 3dbrew says the primitive engine is not fully reverse
  engineered.
- **Bug:** while GEOSTAGE_CONFIG2 bit 0 is set, register writes outside 0x200–0x254 and 0x280–0x2DF go wrong.
- Possible use: expanding GX points and lines into quads [I]. It needs a hardware test. Splitting quads with it is
  of low value.

**Primitive engine** [S]:
- PRIMITIVE_CONFIG 0x25E bits 8–9: 0 triangles, 1 strip, 2 fan, 3 geometry.
- RESTART_PRIMITIVE 0x25F starts a new strip or fan within one draw.
- INDEXBUFFER_CONFIG 0x227: bit 31 selects u8 or u16 indices.
- POST_VERTEX_CACHE_NUM 0x22D, "typically 0x4 or 0x84".

**Culling and clipping** [S]:
- FACECULLING_CONFIG 0x040: none, front, or back (counter-clockwise).
- One user clip plane: FRAGOP_CLIP 0x047, coefficients in 0x048–0x04B.
- Guard band, near-plane hardware behaviour and point/line size: not found.

## 3. Fragment pipeline

**Texture combiners** [S]:
- 6 stages; stages 4 and 5 sit at 0xF0/0xF8.
- Sources: primary colour, fragment primary and secondary (lighting), textures 0–3, previous buffer, constant,
  previous.
- Functions: REPLACE, MODULATE, ADD, ADD_SIGNED, INTERPOLATE, SUBTRACT, DOT3_RGB(A), MULTIPLY_ADD, ADD_MULTIPLY.
- Scale 1x, 2x or 4x.
- Combiner buffer: inputs for stages 1–4 via TEXENV_UPDATE_BUFFER 0xE0; buffer colour at 0xFD.
- Constant colour per stage.

**GameCube TEV features with no PICA equivalent** [I]:
- Compare ops: none.
- Bias ±0.5: use ADD or SUBTRACT with a 0.5 constant.
- Divide by 2: MODULATE by 0.5.
- 16 stages: fold, or use multiple passes.

**Fragment lighting** [S]:
- 8 lights; 256-entry LUTs (D0, D1, spot, Fresnel, reflection, distance attenuation).
- The vertex shader outputs a normal *quaternion*.
- Bump and tangent maps; per-light shadow and distance-attenuation enables.
- citro3d packs light products into 10-bit fields without clamping, so clamp first.
- Per-pixel results differ from GX per-vertex lighting [I]: A/B test before switching.

**Fog** [S]:
- Mode is set in TEXENV_UPDATE_BUFFER bits 0–2 (5 fog, 7 gas); bit 16 is zFlip.
- Colour at 0xE1.
- LUT: 128 entries through index 0xE6 and data 0xE8–0xEF. Each entry holds an 11-bit value (visibility; 1 means no
  fog) and a 13-bit difference.
- citro3d samples the LUT at eye z = far·near / (d·(far−near) + near) for d = i/128. **The index is affine in
  1/z** [I]: with near 1 and far 1000, z from 10 to 1000 covers only entries 0–12.
- Whether the hardware reads z or w for fog: not found.
- GX range adjust (per-X scaling) has no PICA equivalent.
- **Dunes test:**
  1. Draw a known depth ramp with an identity LUT, to settle z or w, DEPTHMAP_ENABLE and zFlip.
  2. Build the LUT by sampling GX_SetFog's function at those eye-z values.
  3. Pick near and far to spend the 128 entries where the fog changes.

**Per-fragment operations** [S]:
- Alpha test (one reference) at 0x104. GX's two-reference compare needs two passes with stencil [I].
- Stencil at 0x105/0x106, with the 8 usual ops.
- Depth test and colour/depth write masks at 0x107. **Conflict:** citro3d's effect.c writes some depth fields
  through the gas register. Check against hardware before writing depth code.
- Blend at 0x100–0x103: ADD, SUB, REVERSE_SUB, MIN, MAX; constant colour. All 16 logic ops are available when
  blending is off.
- Early depth at 0x61–0x63 and 0x6A (descriptions conflict).
- Depth map: scale 0x4D, offset 0x4E, enable 0x6D (w-buffer versus z).
- Shadow mode: FRAGOP mode 3 and shadow texture types.

## 4. Textures, render targets, DMA

**Texture formats** [S]:

| Code | Format | Code | Format |
|---|---|---|---|
| 0 | RGBA8888 | 7 | I8 |
| 1 | RGB888 | 8 | A8 |
| 2 | RGBA5551 | 9 | IA44 |
| 3 | RGB565 | A | I4 |
| 4 | RGBA4444 | B | A4 |
| 5 | IA8 | C | ETC1 |
| 6 | HILO8 | D | ETC1A4 |

- **No palette and no S3TC.**
- Layout: 8x8 tiles, Morton order inside each tile. citro3d does not tile for you.
- Sizes are powers of two from 8 to 1024 (citro3d; the registers allow more).

**GameCube mapping** [I]:
- Re-tile only: I4, I8, IA8, IA4 (as IA44), RGB565.
- Convert: RGB5A3; reorder RGBA8.
- Expand on the CPU: CI4, CI8, CMPR. CMPR could also be transcoded to ETC1, with loss.

**Texture units** [S]:
- 3 units plus the procedural texture unit; cube, shadow and projection modes on unit 0 only.
- Wrap modes: edge, border, repeat, mirror.
- LOD bias, minimum and maximum level.
- Bit 16 of 0x080 clears the texture cache.
- Framebuffer cache: flush with 0x111, then invalidate with 0x110.

**Procedural texture unit** [S]: noise plus 128- and 256-entry LUTs; it reads no memory. Whether its colour table
could act as a palette is untested.

**Render targets** [S]:
- Colour formats RGBA8, RGB5A1, RGB565, RGBA4; depth formats D16, D24, D24S8.
- Up to 4095 pixels; 8x8 tiles, with a 32x32 block mode (0x11B).
- Render to texture with `C3D_FrameBufTex`.

**Display-transfer engine** [S] (registers at 0x10400C00):
- Flags: vertical flip, linear-to-tiled, raw copy, tiled-to-tiled.
- Input and output formats: RGBA8, RGB8, RGB565, RGB5A1, RGBA4.
- 32x32 blocks; 2x1 or 2x2 box downscale.
- Signals completion with PPF.
- TextureCopy copies sub-rectangles (line width and gap in 16-byte units).
- MemoryFill (PSC0/1) works on VRAM only.
- No speed figures found.
- Possibly a 64x64 minimum output size, and a 32x32 RGB5A1→RGBA4 transfer that never signals [?].

**Memory:** VRAM is 6 MB. citro3d flushes linear textures and skips VRAM ones. No data on GPU read speed from VRAM
versus FCRAM.

**Offload ranking** [I]:
1. EFB→XFB as one display transfer. YUV conversion stays on the CPU.
2. EFB copy to texture by rendering into a texture-backed buffer, or by a tiled-to-tiled copy. The engine has no
   depth formats.
3. Re-tile textures once and cache them by hash.
4. Clears through MemoryFill.
5. Antialiasing downscale flags.

## 5. Command submission

**Command format** [S]:
- A command is a parameter word, then a header: register id in bits 0–15, byte mask in 16–19, extra-word count in
  20–27, consecutive writing in bit 31. Then the extra words, padded to 8 bytes.
- The buffer is 16-byte aligned and must end with FINALIZE (0x0010).
- NaN parameters hang the GPU.

**Draws** [S]:
- `C3D_DrawArrays` writes ~11 commands on every call.
- Config writes made in draw mode (START_DRAW_FUNC0 = 0) apply incorrectly.
- Minimal per-draw set [I]: GEOSTAGE_CONFIG2 on, START_DRAW_FUNC0 = 0, NUMVERTICES, VERTEX_OFFSET, DRAWARRAYS,
  START_DRAW_FUNC0 = 1, GEOSTAGE_CONFIG2 off. Add VTX_FUNC when vertex data changes. About 64 bytes.

**Command-list jumps** [S]: CMDBUF_JUMP0/1 (0x23C/0x23D, with ADDR and SIZE). Return semantics are undocumented,
citro3d has no wrapper, and a bad list hangs the GPU. High risk, low need.

**Immediate mode** [S]:
- `C3D_ImmDrawBegin`, `C3D_ImmSendAttrib`, `C3D_ImmDrawEnd`.
- Three words per 4-float attribute, ~9 commands of overhead.
- Only worth it for small draws.

**GSP queue** [S]: 15 entries; libctru keeps up to 3 in flight; citro3d allocates 32. Flush flags make GSP call
svcFlushProcessDataCache.

## 6. Other projects

- **Citra/Azahar on PC** [S]: moving all PICA emulation onto the host GPU gave ~2x. A switch-based shader
  interpreter was slower than the software renderer; generated shader code won. Lesson: generate fixed shader
  variants; don't interpret.
- **RetroArch ctr_gfx.c** [S]:
  - One sprite shader with a per-quad uniform.
  - One vertex per quad, which the geometry shader expands.
  - A ring-buffer vertex cache.
  - Font atlases swizzled to L8 and uploaded as dirty rectangles.
- **ScummVM 3DS** [S]: uploads go in a separate frame, only when something changed. Cave Story's 3DS port syncs a
  transfer per glyph, which is the pattern to avoid.
- **Not found:** any 3DS project doing a matrix palette or GPU vertex loading, and any measured draw-call budget
  (one unverified forum report: ~350 citro2d sprites before lag on an original 3DS). picaGL's internals could not
  be read.

## Sources (main)

- 3dbrew:
  - GPU/Internal_Registers, External_Registers, Shader_Instruction_Set, Primitive_Engine, Fragment_Lighting,
    Programming_Guide, Pitfalls
  - GX_Commands, GSP_Shared_Memory, Nintendo_OpenGL
- citro3d (`source/`): drawArrays.c, immediate.c, base.c, texture.c, framebuffer.c, texenv.c, fog.c, effect.c,
  light.c, uniforms.h
- libctru: gpu.c, gpu.h, gx.c, gx.h, gxqueue.c, enums.h
- picasso manual
- Azahar: shader_interpreter.cpp, etc1.cpp, utils.h
- libogc gx.h
- RetroArch gfx/drivers/ctr_gfx.c
- citra.azahar-emu.org "Improvements to hardware renderer"
