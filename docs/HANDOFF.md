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
