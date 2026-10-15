# Native mode: decompilation ports run by gc3ds (2026-10-15)

Owner direction (James, 2026-10-15): full speed is the goal, for as many games as possible, and everything goes through
gc3ds. Emulation stays for every game; a game with a decompilation port gets a **native mode**: its own source
compiled for the 3DS, running on gc3ds's platform layer (GX -> PICA200, disc, input, crash logging).

## Why this and not more JIT work, or binary recompilation

- The emulator is ~22% (cutscene) / ~12.6% (later stretch) of full speed on TP after the 10-14 / 10-15 work; each JIT
  step is worth a few percent (HANDOFF). Core 0 emulating the Gekko is the wall.
- **Binary static recompilation was built and lost (C12, AOT_PLAN.md, job 144):** TP's PowerPC code translated to C and
  compiled with GCC ran ~30% slower than the JIT on the 2DS (exact on the PC). The output keeps the GameCube's memory
  image, byte order and float semantics per instruction, and its size made core 0's instruction-fetch stalls worse.
  Do not propose PowerPC-to-C recompilers (DolRecomp, XenonRecomp-style) for the 2DS again without a new argument.
- A decompilation port compiles the game's **source**: native types and layouts, the compiler optimizes across whole
  functions with real types, no address translation, no per-access checks, no Gekko float emulation. That is a
  different thing from C12, and it is how existing ports reach full speed.

## What exists (checked 2026-10-15 against the primary sources)

- **zeldaret/tp:** GZ2E01 100.00% decompiled, 87.13% linked (decomp.dev/zeldaret/tp); README: "The code for the
  GameCube releases is completely matching"; it "is not, and will not, produce a port".
- **Dusklight (github.com/TwilitRealm/dusk, CC0-1.0, twilitrealm.dev):** "a reverse-engineered reimplementation of
  Twilight Princess" based on the decompilation, on PC, Android and iOS, from the user's own disc (no assets shipped).
  C++20, ~970k lines in src (1,262 .cpp), plus libs/ (decompiled JSystem, Dolphin SDK, MSL). Its contribution policy
  rejects AI-generated pull requests: our 3DS work stays a private fork, never proposed upstream.
- **Aurora (github.com/encounter/aurora, submodule of dusk):** source-level GameCube / Wii SDK layer (OS, DVD, PAD, VI,
  GX, GD, MTX, THP, CARD). Its GX API (`lib/dolphin/gx`) encodes **GX FIFO commands** into a buffer
  (`lib/gx/fifo.hpp`), and a command processor (`lib/gx/command_processor.cpp`, `process(data, size)`) turns them into
  WebGPU (Dawn: D3D12 / Vulkan / Metal). No 3DS backend exists anywhere.
- **Other decomp ports:** Super Mario Sunshine (PC), Animal Crossing (PC, own GX -> OpenGL layer), Mario Party 4 (alpha),
  Wind Waker decomp 79.58% decompiled (shares TP's engine). On the 3DS only Super Mario 64 (N64) has a decomp port.
  No GameCube game has run on a 3DS by any method we could find (research agent report, 2026-10-15).

## Fit with gc3ds

- **Data stays big-endian.** Dusklight keeps resources in their GameCube form and swaps fields on access (`BE<T>`,
  `include/helpers/endian.h`), so vertex arrays, display lists and textures in memory are the format gc3ds's GX side
  already reads. Patch needed: endian.h detects little-endian only for x86 / aarch64, not 32-bit ARM (`__arm__`).
- **Pointers are 32-bit** on the 3DS, as on the GameCube; the decompilation's original assumption.
- **Renderer:** keep Aurora's GX API (API -> FIFO) and replace its consumer with gc3ds's GX thread, vertex worker and
  PICA200 pipeline (core/src/gx). Change needed there: addresses in the FIFO are host pointers, not guest physical
  addresses (a native address mode in gc3ds's memory reads for arrays, display lists, textures).
- **CPU side:** core 0 runs the game natively; cores 2-3 keep GX as now. The emulator's lessons still apply to the
  native code (I-cache footprint: -Os vs -O2 is a measurement, not a guess).
- **Delivery:** gc3ds recognises the disc and relaunches into the game's native build (it already relaunches itself into
  new builds for the relay), sharing gc3ds's platform code as a library. Game assets come from the user's disc on SD.

## Owner requirement: the disc image on the SD card is all a player provides (James, 2026-10-15)

"As long as it makes it easy and as long as user puts an ISO on the SD card. It can work." Native mode is invisible:

- The player copies their own disc image to the SD card and picks it in gc3ds; nothing else to install or choose.
- gc3ds reads the disc's ID and revision. A native port built for exactly that disc runs native; every other disc
  (other regions, revisions, games without a port) runs on the JIT as now. Never a native port on a disc it does not
  match.
- One app from the player's view: native ports inside gc3ds or relaunched by it (hb:ldr, as for relay builds).
- Every image format gc3ds accepts works natively too (the owner's own image is .nkit.iso: check nod's reader in the
  shim before the first device run).
- No game data ships with gc3ds: the decompilations are code only; models, textures and sounds come from the disc.

## Plan, with a go / no-go before each large step

1. **Feasibility (go / no-go for the whole route):** build Dusklight's game code with devkitARM (ARMv6K, VFP, C++20)
   with rendering and audio stubbed; boot from the disc image on SD; measure core 0's milliseconds per game frame
   (TP's logic runs at 30 Hz: ~33 ms budget) in the title scene and in gameplay. Also: code size and heap headroom
   (gc3ds's malloc-starvation lesson: check `# heap: largest malloc block`).
2. **Renderer:** Aurora GX API -> gc3ds GX (native address mode) -> PICA200; THP movies; EFB copies as gc3ds does them.
3. **Audio** (Dusklight's JAudio / DSP path onto the 3DS DSP or a mixer on a free core), input, saves (CARD on SD).
4. **Generalise:** the platform layer as a library other decomp ports (Wind Waker, Sunshine, ...) build against.

## First runs on the New 2DS XL (2026-10-15)

Delivered through the relay (`relay.sh send core/out/native.3dsx sdmc:/3ds/gc3ds_native.3dsx`), launched from the
Homebrew Launcher by James; logs back with `relay.sh cat sdmc:/gc3ds/native/perf.csv`.

1. **First run:** a data abort reading 0xF8 in `Z2SceneMgr::loadSeWave` from the logo scene's `loadStaticWaves`: with
   audio off no `JAUSectionHeap` exists (asserts compiled out). Guarded in patch_dusk.sh (e445e85); Azahar had not
   reached that point.
2. **Second run (133 s):** logo and opening scene at a steady 60 fps, GX 10-18 KB a frame, draw-done twice a frame,
   clean exit; memory 78.5 MB free at start, 33.5 MB after the 45 MB arena. The "logic" time equalled the frame (16.4
   ms): JFWDisplay's `waitForTick` sleeps inside the game's frame.
3. **Third run (476 s, c7d48ee, busy = Dusklight's `frameUsagePct`):** 59.9 fps average; the game worked **8.9% of each
   frame (median; ~1.5 ms of 16.4 ms)**, p90 9.4%, p99 10.0%; 33-55% only in the first seconds while the scene loaded.
   This is the title / opening scene with nothing drawn and audio off: gameplay is the measurement that decides
   feasibility (next: an input script to reach it), then the renderer.
4. **Hyrule Field, hands-free (relay job 210, `relay.sh native 180 - F_SP121,0,0`):** gc3ds started native mode, which
   booted straight into F_SP121 (Dusklight's boot-to-stage; audio skipped through DUSK_AUDIO_SKIP; Link's heaps 2x
   instead of the PC's 20x), ran 180 s and handed back to gc3ds, which posted the logs. **164 s at the game's 30 fps,
   busy median 14.8% of the 33.3 ms frame (~4.9 ms), p90 15.3%, p99 16.0%, max 16.3%**; GX ~69 KB a frame. Link stood
   still (no input), nothing drawn, no audio: movement and combat are next (`--press`), then the renderer.

5. **The title screen's in-engine demo** (Link on Epona, Midna; one START press, `relay.sh native 120 START@600-606`),
   the heaviest scene so far. Job 213: 80-100% busy with ~900 KB of GX a frame, a stretch at 14.8 fps. Job 217 split
   it: ~388 display lists a frame were **copied** into Aurora's FIFO (680 KB, 4.2 ms) and the stream scanned again
   (7.3 ms). With display lists **by reference** (job 219: GX_AURORA 0x50 subcommand 0x0060, pointer + size): busy
   **53-66% at 30 fps**, ~200 KB of GX a frame, consumer 2.6 ms. One stretch stays slow (17.4 fps, 100% busy, every
   second identical, same display lists as normal frames): most likely the demo waiting on the music it is synced to
   (audio off), unconfirmed.
6. **Not yet controllable:** the --stage field spawn never moved Link under scripted input (no event running), though
   the same presses walk the title to file select: the blank-save spawn is the suspect.

## Renderer: Aurora's stream into gc3ds's GX pipeline (design, 2026-10-15)

Aurora's GX API writes the GameCube's FIFO format (BP / CP / XF register writes, draws) plus an extension opcode
(`GX_WRITE_AURORA`, include/dolphin/gx/GXAurora.h) for what cannot be a GameCube address on a host:

- `LOAD_ARRAYBASE | n`: a vertex array's host pointer (u64), size and endianness, in place of CP's array base.
- `LOAD_TEXOBJ` / `LOAD_TLUT`: texture data pointer, width, height, format, TLUT, mips, an object ID and a data
  **version** (a texture cache keys on ID + version: no hashing of texture data). BP image registers still follow.
- `LOAD_COPY_SRC / DST / DEST`, `REQUEST_DEPTH_SNAPSHOT`, `BEGIN / END_OFFSCREEN`, `DESTROY_*`: EFB copies, render to
  texture, object lifetimes.
- `LOAD_VIEWPORT_RENDER`, `LOAD_SCISSOR_RENDER`, `LOAD_PROJECTION_FULL`, `DRAW_SIZED`, `DRAW_INDEXED`.
- Display lists are copied inline (`GXCallDisplayList` writes the list's bytes): no list addresses.

Plan: gc3ds's GX decoder (core/src/gx, its GX thread on core 2, vertex worker on core 3, PICA200 backend) gets an
Aurora mode: the opcode's commands set host-pointer arrays (gathers read host memory instead of guest MEM1), texture
objects (decode from the pointer, cached by ID + version, into the formats the 2DS measured as fast: RGB565 / ETC1,
REDESIGN_CANDIDATES C17) and copy targets; everything else is the existing GameCube path. The stream reaches the GX
thread's ring from aurora_3ds.cpp's process() (today it only finds draw-done markers). First milestone: Hyrule Field's
geometry on the top screen with flat shading, then TEV and textures.

## Risks

- Unmeasured: TP's game logic on an 804 MHz ARM11 (no NEON; VFP11 doubles are slow). Step 1 answers it.
- Memory: the game's heaps (sized for 24 MB MEM1 + ARAM use) plus a native binary of ~970k lines of C++ within the
  3DS application region; build-size growth shrinks gc3ds's malloc heap (AOT lesson, 10-14).
- Dusklight's own additions (mods, Luau scripting, RmlUi menus, Discord presence) must be compiled out.
- Never distribute game data or a build containing it; the user's own disc only. Never download ROMs.

## After this: the 2DS timing model

Once step 1 has been tested on the 2DS, a standalone project models the 2DS's speed on the PC and measures every
modelled mechanism against the device: docs/research/TIMING_MODEL_PLAN.md (owner direction 2026-10-15).
