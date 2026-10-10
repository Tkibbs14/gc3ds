# GX → PICA200 backend: plan

The 3DS frontend currently renders GX draws with the software reference renderer (`core/src/gx/soft.c`) on the
ARM11. On the New 2DS XL that holds 3D test programs at about 5% speed. The PICA200 measured 6 Mtri/s and
193 Mpx/s (docs/FINDINGS.md), far more than GameCube games need, so moving drawing to it is the largest available
speed-up for anything 3D.

The backend plugs in through `GXBackend.draw` (`core/src/gx/gx.h`), exactly where `soft.c` does. It lives in
`core/src/gx/pica/`, is built only for the 3DS, and uses citro3d.

## Steps, each verified against the reference renderer before the next

1. **Geometry, transformed on the CPU.**
   - Reuse `soft.c`'s transform: XF matrices, projection, lighting and texgen. Move it into a shared `gx/xform.c` so
     both backends call the same verified code.
   - Hand the PICA clip-space positions, both colours and texture coordinates through a pass-through vertex shader.
   - Render into a 640×528 RGBA8 + D24S8 render target in VRAM (EFB size; the fill rate allows native size).
   - **Check:** triangle, NeHe 5, NeHe 7 against `soft.c` frames, compared as images.
   - **Depth convention:** GX clip space and the PICA's depth range differ. Derive the mapping from the GX projection
     formulas in `soft.c::transform` and the PICA's documented depth range; verify with NeHe 5 (overlapping cube
     and pyramid).

2. **TEV → PICA combiners.**
   - A GX stage computes `(d + lerp(a, b, c) + bias) * scale` with clamping. The PICA has 6 stages, the combiner
     buffer, and one constant colour per stage.
   - Common stages map to one PICA stage:
     - only `d` → REPLACE;
     - `a = 0, d = 0` → MODULATE(b, c);
     - `d + lerp` → INTERPOLATE then ADD;
     - konst inputs → that stage's constant colour.
   - The general case takes two PICA stages. When a game needs more than 6, drop the stages that contribute least
     and log it (accepted approximation; GX has 16 stages).
   - **Check:** `texturetest` and NeHe lessons; per-stage unit tests comparing the combiner formula to `soft.c::run_tev`.

3. **Textures.**
   - `gx_decode_texture` (shared, verified) to RGBA8, swizzle to the PICA tiled layout (gcbench verified the swizzle
     byte for byte), then DMA to VRAM.
   - Texture cache keyed by address, format, size, TLUT and `tlutVersion`/`texVersion`, holding VRAM up to its 3 MB
     per-allocation limit, with FCRAM fallback (2.5–3.6× slower to sample).
   - The PICA needs power-of-two sizes from 8 to 1024: pad and scale the texture coordinates.
   - Map wrap and filter modes.

4. **EFB copies on the GPU.**
   - Copy to XFB: DisplayTransfer the render target straight to the top-screen framebuffer, scaled 640×480 →
     400×240 with filtering. The XFB in emulated RAM is not written ("virtual XFB"; programs that read the XFB
     back are rare).
   - Copy to texture: a GPU-side copy into a texture registered in the texture cache under the destination address.
   - CPU readback (EFB peeks) only on demand, about 4 ms per 256×256 as measured, never by default.

5. **Then speed:**
   - Move the transform into the vertex shader (matrices as uniforms; the PICA has 96 vec4 constants, so upload only
     the matrices each draw uses).
   - Keep GameCube integer vertex formats where the PICA accepts them, byte-swapped only.
   - Run vertex conversion on core 2.

## Not changing

- `soft.c` stays the PC reference and the source of truth for comparisons.
- Never write VRAM from the CPU: it is not accessible to apps, and the app crashed when it tried. Everything goes
  through GX DMA/DisplayTransfer.
- Present at most once per 3DS refresh; flooding the GPU service crashed it (FINDINGS, 2026-10-10).
