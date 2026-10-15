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

## Plan, with a go / no-go before each large step

1. **Feasibility (go / no-go for the whole route):** build Dusklight's game code with devkitARM (ARMv6K, VFP, C++20)
   with rendering and audio stubbed; boot from the disc image on SD; measure core 0's milliseconds per game frame
   (TP's logic runs at 30 Hz: ~33 ms budget) in the title scene and in gameplay. Also: code size and heap headroom
   (gc3ds's malloc-starvation lesson: check `# heap: largest malloc block`).
2. **Renderer:** Aurora GX API -> gc3ds GX (native address mode) -> PICA200; THP movies; EFB copies as gc3ds does them.
3. **Audio** (Dusklight's JAudio / DSP path onto the 3DS DSP or a mixer on a free core), input, saves (CARD on SD).
4. **Generalise:** the platform layer as a library other decomp ports (Wind Waker, Sunshine, ...) build against.

## Risks

- Unmeasured: TP's game logic on an 804 MHz ARM11 (no NEON; VFP11 doubles are slow). Step 1 answers it.
- Memory: the game's heaps (sized for 24 MB MEM1 + ARAM use) plus a native binary of ~970k lines of C++ within the
  3DS application region; build-size growth shrinks gc3ds's malloc heap (AOT lesson, 10-14).
- Dusklight's own additions (mods, Luau scripting, RmlUi menus, Discord presence) must be compiled out.
- Never distribute game data or a build containing it; the user's own disc only. Never download ROMs.
