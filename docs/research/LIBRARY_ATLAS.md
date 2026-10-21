# Library atlas: the GameCube library beyond Twilight Princess (2026-10-21)

A reference for planning work that should pay across many games: which libraries GameCube games share, which games
stress which hardware features, how library code can be recognised automatically, and what other emulators learned
doing it. It extends GAMES.md (the 12 games on James's discs, what TP's stand-ins match in each) and DECOMP_SURVEY.md
(decompilation projects); read those for per-game detail.

**Sources.** Five web research passes (Haiku agents, 2026-10-21), with the load-bearing facts checked here against
primary sources: Dolphin's repository at `cf5cb7e` (2026-10-19; its per-game settings, audio microcode table and
signature database, read directly), GameTDB's title list, the GitHub API for the decompilation repositories, and
decomp-toolkit's signature directory. Tags: **verified** (read from the primary source here), **sourced** (an agent
read it at the cited page; not re-checked), **unverified** (search snippet or secondary page only), **inferred**.
Several pages (Dolphin wiki and blog, MobyGames, some GitHub raw files) refused automated fetches, so the gaps listed
in section 9 are gaps in what could be read, not evidence of absence.

## 1. The layers a GameCube executable is made of

| Layer | Shared by | Versioning | gc3ds lever |
|---|---|---|---|
| Nintendo's SDK ("Dolphin SDK": OS, GX, DVD, AX, MTX, ...) | every licensed game | per library, by build date (a game can mix library builds) | the most general: GX, MTX / PSVEC, OS |
| CodeWarrior runtime and MSL (memcpy, memset, printf, `__savegpr`, ...) | every game built with CodeWarrior | per compiler version (GC/1.2.5, 1.3.2, ...) | small leaves: inline (X3 showed `_savegpr` inlining pays) |
| Middleware (JSystem, HAL's HSD, RenderWare, MusyX, CRI ADX / Sofdec, Bink, Havok) | a family of games each | per middleware version | a subtree port pays per family |
| Audio microcode on the DSP (AX, Zelda, CARD, GBA, IPL) | families of games | per microcode CRC | high-level DSP emulation per family |
| The game's own code | one game | - | the JIT only |

## 2. The SDK

- **Libraries** (verified from the decompilation READMEs): the 2001 SDK has 31 libraries: ai, amcnotstub, amcstubs,
  ar, ax, axfx, base, card, db, demo, dolformat, dsp, dtk, dvd, fileCache, G2D, gx, hio, mcc, mix, mtx, odemustubs,
  odenotstub, os, pad, perf, seq, support, syn, texPalette, vi. Later SDKs add am, axart, exi, gd, si, sp. Wind Waker
  also links `gba` and `gf` (sourced, tww config).
- **Decompilations of the SDK itself** (verified via the GitHub API):
  - `doldecomp/dolsdk2001` decompiles the 5-23-2001 SDK (README: all libraries matched, "not a complete copy");
  - `doldecomp/dolsdk2001-dec` decompiles 12-17-2001 (12 of 38 libraries matched);
  - `doldecomp/dolsdk2004` decompiles 4-20-2004 (about 33 of 36 matched; gx, vi and axfx in progress).

  Game decompilations also carry matched SDK code: TP, TWW and Melee (their configs list the SDK libraries), Resident
  Evil 4 (built from dolsdk2004 sources with GC/1.2.5n), and Metroid Prime, Mario Kart: Double Dash and Pikmin 2 (all
  credited by the Ocarina of Time GC decomp for SDK code) (sourced). Animal Crossing, Mario Party 4 and Pikmin 1 likely
  do too (inferred, not checked).
- **Identifying the version in a disc:** each library prints a banner such as
  `<< Dolphin SDK - GX release build: Feb 7 2003 ... (0x2301) >>`, and the OS prints `$Revision: NN $` (unverified: one
  Dolphin bug-tracker boot log, apparently F-Zero GX). In that log one game links libraries built Sep 2002 to Mar 2003.
  **Recognition must therefore work per library, not per SDK release** (inferred). GAMES.md's "OS rev 37 / 47 / 54"
  generations come from the same kind of string in James's discs.
- **How much changes between revisions:** no published diff was found. gc3ds's own evidence (GAMES.md): TP's 40
  stand-ins match 3-17 functions exactly in each other game and more with masking, falling to 0 exact in Pikmin, the
  oldest SDK. So exact matches cover a game sharing TP's SDK generation well, and older generations need masked or
  regenerated signatures.

## 3. Middleware and engines

| Family | Games (tag) | Notes |
|---|---|---|
| JSystem (J3D, J2D, JAudio, JKernel, JUtility) | TP, Wind Waker, Sunshine, Luigi's Mansion, Pikmin, Animal Crossing (JKernel and JUtility, no J3D), likely Mario Kart: Double Dash and Pikmin 2 (inferred) | Nintendo EAD's engine; one engine across versions. JAudio pairs with the Zelda microcode (section 4) |
| HAL's HSD ("sysdolphin") | Melee (verified in its build config); Kirby Air Ride (inferred: same studio and era) | Melee is the best-selling GameCube game |
| Retro's engine | Metroid Prime, Prime 2: Echoes (sourced) | own renderer; Dolphin culls on the CPU for both (section 5) |
| RenderWare (Criterion) | licensed "over 200 times" across platforms; GameCube titles not listed by any page read (Killer7 credited by one catalogue, unverified) | a large unknown for third-party games |
| MusyX (Factor 5, audio) | Metroid Prime, Echoes, Star Fox Adventures (unverified, one forum list) | |
| CRI ADX / Sofdec | F-Zero GX, Resident Evil 4, Star Fox: Assault, Code: Veronica X (unverified); SA2: Battle (GAMES.md) | Sofdec is MPEG video plus ADX audio: a CPU decode load like THP |
| Havok | From Russia with Love, The Ant Bully (sourced, incomplete fan list) | late-life titles |
| THP (Nintendo's movie format) | the main GameCube movie format (sourced) | gc3ds already has a THP IDCT stand-in |

No per-engine game counts were found anywhere readable. MobyGames' middleware pages are the next source to try.

## 4. Audio microcode families (verified: Dolphin `UCodes.cpp`)

- **Zelda family** (Dolphin's ZeldaUCode, by CRC): Wind Waker, Twilight Princess (GC and Wii), Four Swords Adventures,
  Pikmin 1, Luigi's Mansion, Super Mario Sunshine, Super Mario Galaxy, and the GameCube IPL (both regions).
- **AX family:** everything else Dolphin lists, including Paper Mario: TTYD, Naruto, Alien Hominid, Crazy Taxi, 18
  Wheeler, Monkey Ball 1/2, Cubivore, Capcom vs. SNK 2, Lost Kingdoms, Star Fox, Mario Party 4 and 5, Mortal Kombat,
  Ikaruga, F-Zero GX, Robotech, Ocarina of Time GC, Tony Hawk, Viewtiful Joe, Billy Hatcher, Dragon Ball Z, TMNT and
  1080° Avalanche.
- **CARD, GBA and ROM (IPL)** have their own entries. Dolphin's AX HLE rewrite is described as one of its biggest
  performance wins (sourced, Dolphin's 2015 blog), and the Zelda microcode's hazard is CPU-DSP synchronisation by
  interrupts (sourced).
- **For gc3ds** (inferred): a high-level AX implementation covers most of the library, and the Zelda family covers the
  EAD games. gc3ds's AUDIO.md plan (high-level AX into ndsp) fits; TP itself needs the Zelda family.

## 5. Which games stress what: Dolphin's per-game settings (verified, derived counts)

Dolphin ships settings for **588 GameCube titles** (`Data/Sys/GameSettings`, IDs starting G or D, titles from
GameTDB). A setting there means Dolphin needs it for that game, so each count is a list of games that exercise the
feature. The names are Dolphin's; gc3ds's equivalent behaviour is noted.

| Setting (what it means) | Games | Notable titles |
|---|---|---|
| `ImmediateXFBEnable=False` (needs real XFB timing) | 112 | 007: Agent Under Fire, Beyond Good & Evil, Baten Kaitos Origins, Batman titles, Crash Nitro Kart, ... |
| `CPUThread=False` (timing-sensitive with Dolphin's dual core) | 43 | F-Zero GX, Metroid Prime 2, Pokémon XD, LEGO Star Wars, Skies of Arcadia, SSX Tricky, PSO, Second Sight |
| `EFBToTextureEnable=False` (**EFB copies must reach RAM**) | 35 | Super Mario Sunshine, Wind Waker, Paper Mario: TTYD, FF Crystal Chronicles, Mario Party 5, Star Fox: Assault, Metroid Prime 2, Skies of Arcadia, Baten Kaitos, Code: Veronica X, Splinter Cell DA |
| `MMU=True` (full address translation) | 25 | Rogue Squadron II and III, The Clone Wars, Spider-Man 2, Ultimate Spider-Man, TimeSplitters: Future Perfect, Call of Duty 2, Pokémon XD, Gun, Vexx |
| `FPRF=True` (exact float result flags) | 20 | F-Zero GX, Super Monkey Ball 1 and 2, Star Fox: Assault, Beach Spikers, Virtua Striker, Resident Evil 2, Fight Night Round 2 |
| `VertexRounding=True` | 16 | Sonic Heroes, Shadow the Hedgehog, SpongeBob titles, The Incredibles, TY, Mortal Kombat: Deception |
| `EFBAccessEnable=True` (**the CPU reads the EFB**) | 15 | Super Mario Sunshine, Wind Waker, Killer7, Code: Veronica X, TY, Jimmy Neutron |
| `DeferEFBCopies=False` | 14 | Paper Mario: TTYD, Baten Kaitos, SA2: Battle, Tomb Raider: Legend |
| `CPUCull=True` (cull on the CPU: heavy geometry) | 8 | **Metroid Prime, Prime 2, Rogue Squadron II and III, Pokémon Colosseum and XD, Twilight Princess** |
| `SyncOnSkipIdle=False` | 8 | Battalion Wars, Alien Hominid, Hulk, Tetris Worlds |
| `BBoxEnable=True` (GPU bounding box readback) | 6 | **Paper Mario: TTYD**, Hide & Sneak, Magical Mirror, Ultimate Spider-Man |
| `EFBEmulateFormatChanges=True` | 5 | FF Crystal Chronicles, Mario Party 6, Godzilla |

What this means for breadth (inferred): beyond TP's path, the features that gate whole groups of popular games are
**EFB copies to RAM and CPU EFB reads** (Sunshine, Wind Waker, TTYD), **bounding box** (TTYD; no PICA equivalent, so
it needs a CPU-side estimate), **MMU / BAT** (Rogue Squadron, licensed titles), and **exact FPRF** (Monkey Ball,
F-Zero GX). CPUCull marks geometry-heavy games, the same class as TP.

## 6. The best sellers and what they are built on

Wikipedia's best-selling list (sourced; sales in millions). Layers from sections 3 and 4.

| Game | Sales | Layers (tag) |
|---|---|---|
| Super Smash Bros. Melee | 7.41 | HSD (verified), AX (inferred), 60 fps |
| Mario Kart: Double Dash | 6.88 | JSystem (inferred) |
| Super Mario Sunshine | 5.91 | JSystem, Zelda microcode, EFB to RAM, CPU EFB reads |
| The Wind Waker | 4.43 | JSystem, Zelda microcode, EFB to RAM, CPU EFB reads |
| Luigi's Mansion | 3.33 | JSystem, Zelda microcode |
| Metroid Prime | 2.84 | Retro engine, MusyX (unverified), CPU culling |
| Animal Crossing | 2.70 | JSystem (no J3D) |
| Mario Party 4 / 5 / 6 / 7 | 2.46 / 2.17 / 1.63 / 2.08 | Hudson engine, AX; MP5 needs EFB to RAM |
| Pokémon Colosseum / XD | 2.41 / 1.42 | Genius Sonority; CPU culling; XD also MMU and timing |
| Paper Mario: TTYD | 1.91 | AX; EFB to RAM, bounding box |
| Star Fox Adventures | 1.82 | Rare; MusyX (unverified) |
| Sonic Adventure 2: Battle | 1.73 | CRI ADX / Sofdec |
| Pikmin | 1.60 | JSystem, Zelda microcode |
| Resident Evil 4 | 1.60 | CRI, AX, SDK 4-20-2004 |
| Twilight Princess | 1.43 | JSystem, Zelda microcode, CPU culling |

The middleware share across this list (inferred): JSystem about 7 of the top 20, HSD 1 (the top seller), the rest in
studio engines on the plain SDK. **Work at the SDK level reaches all of them; JSystem work reaches the Nintendo EAD
core.**

## 7. Recognising library code automatically

- **Dolphin's `totaldb.dsy`** (verified): 10,549 named functions. By name prefix: J3D 592, J2D 427, JAS 383, GX 279,
  JKR 271, OS 238, JUT 210, JAI 113, CARD 97, DVD 94, VI 45, AR 42, SI 40, EXI 40, THP 37, AI 36, PAD 29, PSMTX 24,
  DSP 23, GD 10, AX 9; about 7,400 others. Matching is by a lossy opcode-skeleton checksum: immediates and branch targets
  are ignored and colliding entries overwrite each other. The file stores names only, so there is nothing to verify a
  match against. Useful as a candidate finder, not as proof.
- **decomp-toolkit (dtk)** (verified: 140 signatures): exact bytes with relocated fields masked (Rel24, Rel14, Hi / Lo,
  SDA21), hashed with SHA-1, plus required relocations. Coverage is the boot and runtime set (OSInit, OSReport, DVD,
  GXInit, memset); no memcpy and no GX setters.
- **No GameCube FLIRT or Ghidra Function ID database exists** that any pass found. Ghidra-GameCube-Loader loads DOL / REL
  and `.map` files. decomp.dev is closed to new projects (sourced).
- **Academic work** (Jacobson et al. 2011, LibSig 2026) says byte fingerprints break across compiler and library
  versions. None of it covers PowerPC (sourced).
- **gc3ds already has its own:** ps_translate.py recognises functions by a hash of their bytes, with masked variants
  (GAMES.md counts).

**The recognition gc3ds needs, by inference from the above:** generate dtk-style masked signatures from compiled objects
of the SDK decompilations (dolsdk2001, dolsdk2004) and of the game decompilations' matched SDK and JSystem objects,
per library build. Use a skeleton hash (Dolphin's style) only to find candidates, then confirm with size, masked bytes
and relocations before replacing anything.

## 8. What other emulators learned

- **Cxbx-Reloaded (original Xbox)** replaced the XDK libraries (D3D8, DirectSound) found by OOVPA signatures, with one
  table per XDK build (sourced). Build-specific calling conventions forced split signatures (sourced, a progress
  report). xemu emulates the hardware instead and is far more compatible (unverified: ~80% against ~16%).
- **PPSSPP (PSP)** replaces game functions (memcpy, strlen, trig, a matrix multiply) found by a hash of the code with
  immediates masked, plus the size (sourced, ReplaceTables.cpp). It drops a hook when the hash changes or the game
  overwrites the code, disables entries whose hashes collide (asinf / acosf), and gates some hooks per game. No speed
  figures are published.
- **Dolphin** hooks only a few OS functions, by symbol name, mostly for logging (sourced, HLE.cpp). The GPU is emulated
  at the FIFO level, never by replacing GX. Its high-level AX was a large speed win; the Zelda microcode's CPU-DSP
  synchronisation is the hazard (sourced).
- **RPCS3, Citra, PPSSPP** each keep a per-module switch between high-level emulation and the original code (sourced).
- **N64 emulators:** high-level graphics microcode (GLideN64) runs much faster and almost as accurately as low-level
  emulation; full accuracy needs low-level (sourced, GameTechWiki and libretro).

**Lessons for gc3ds** (inferred from the above):
- confirm every match by hash plus size and relocations;
- keep the guest code as the fallback, per library and per game;
- drop a replacement when the game writes its code (gc3ds's icbi page marks already do this);
- expect the accuracy failures where the CPU reads back state: EFB reads, FIFO and token state, DSP interrupts.

Library replacement by signature is common, but **no project publishes a measured speedup** from it: gc3ds's own
numbers (MEASUREMENTS.md) are the only ones.

## 9. What this changes for gc3ds, and the gaps

- **Second-game choice (GENERAL_ARCHITECTURE X6):** pick games that cover different layers and stresses:
  - **Mario Party 4** (Hudson engine, AX, early SDK, a near-complete decomp: the plain-SDK case);
  - **Wind Waker** (JSystem 2002, EFB to RAM, CPU EFB reads, Zelda microcode: TP's nearest relative);
  - **Melee** (HSD, 60 fps, the best seller);
  - **Super Monkey Ball** (AX, exact FPRF, 60 fps);
  - **Metroid Prime** (own engine, CPU culling: heavy geometry).
- **Accuracy features that gate breadth:**
  - EFB copies to RAM and CPU EFB reads;
  - bounding box (TTYD);
  - MMU / BAT (Rogue Squadron and licensed titles);
  - exact FPRF.

  Each needs a gc3ds status check before the second game is chosen.
- **Recognition work, in order:**
  1. scan James's discs for the per-library banners (date each library in each game);
  2. build masked signatures from dolsdk2004 objects;
  3. measure exact and masked coverage per game, as GAMES.md did for TP's 40 functions.
- **Gaps** (not found in any readable page):
  - per-engine game counts;
  - a RenderWare GameCube list;
  - a MusyX title list;
  - the SDK version of each game;
  - any published diff between SDK revisions (for example the `__GXData` layout);
  - native frame rates for Metroid Prime, Double Dash and Rogue Squadron;
  - per-frame polygon counts (only Rogue Leader's 12-15 M polygons a second, sourced from a 2001 developer chat).
