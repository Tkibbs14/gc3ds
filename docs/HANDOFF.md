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

## Next work, ordered by speed gained per token

1. **GPU backend, on hardware:** run the new build on the 2DS (triangle, NeHe, texturetest, then TP) and read
   `sdmc:/gc3ds/log.txt` for `pica:` lines. Then GPU_BACKEND_PLAN.md step 5 (transform in the vertex shader), the
   gaps above, and textures in VRAM.
2. **JIT level 2 (Sonnet, fuzzer-checked):**
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
