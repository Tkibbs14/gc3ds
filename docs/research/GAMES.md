# The other games: what they link, what carries over, and the plan (2026-10-17)

Twilight Princess comes first (full speed); this is the groundwork for the other twelve GameCube games on James's test
list. Everything here comes from reading each disc's `main.dol` (streamed from the archive, nothing kept) and from public
sources; no game code is in this file.

## What each game links

`<< Dolphin SDK - LIB release build: DATE (0x2301) >>` strings in the DOL give each SDK library's build; the oldest SDKs
carry no such strings, only `Dolphin OS $Revision: N $`. Middleware from names in the DOL.

| Game | ID / rev | SDK generation | Middleware | Public symbol map (matching revision) |
|---|---|---|---|---|
| Twilight Princess | GZ2E01 / 0 | Apr 2004 (GX, OS Nov 2004) | JSystem (J3D, JAudio) | zeldaret/tp `config/GZ2E01` |
| Paper Mario: TTYD | G8ME01 / 0 | Apr 2004 | (Intelligent Systems) | none found |
| Resident Evil 4 | G4BE08 / 0 | Apr 2004 | CRI ADX / Sofdec, Capcom sound on AX | adonis-singh/re4 `config/G4BE08` (a Nov 2004 debug build: check against retail) |
| The Wind Waker | GZLE01 / 0 | Sep 2002 | JSystem (shares much engine code with TP) | zeldaret/tww `config/GZLE01` |
| Metroid Prime | GM8E01 / 2 | Sep 2002 (GX Feb 2003), THP | Retro's own engine | PrimeDecomp/prime `config/GM8E01_02` |
| F-Zero GX | GFZE01 / 0 | Sep 2002 (GX, OS, AX 2003) | CRI ADX; Amusement Vision engine | fzero-gx-decomp `config/GFZE01` (early: 2.7% decompiled) |
| Super Mario Sunshine | GMSE01 / 0 | early (OS rev 54) | JSystem (J3D) | doldecomp/sms `config/GMSE01` |
| Animal Crossing | GAFE01 / 0 | early (OS rev 54) | JSystem (JKernel, JUtility; no J3D) | ACreTeam/ac-decomp `config/GAFE01_00` |
| Mario Party 4 | GMPE01 / 1 | early (OS rev 54) | (Hudson) | mariopartyrd/marioparty4 `config/GMPE01_01` |
| Super Smash Bros. Melee | GALE01 / 2 (1.02) | early (OS rev 47) | HAL's HSD / sysdolphin | doldecomp/melee `config/GALE01` |
| Sonic Adventure 2: Battle | GSNE8P / 0 | early (OS rev 47) | CRI ADX / Sofdec; JAudio-like JAS names | none found |
| Luigi's Mansion | GLME01 / 0 | early (OS rev 37) | JSystem (J3D) | none found |
| Pikmin | GPIE01 / 1 | early (OS rev 37) | (Nintendo EAD) | projectPiki/pikmin `config/GPIE01_01` |

## What carries over today (TP's 40 stand-in functions)

Exact: the bytes match, so the stand-in already applies. Masked: they match once the game-specific small-data offsets
and constant addresses are read from the code at recognition (not yet done).

| Game | Exact | Masked | Notes |
|---|---|---|---|
| Paper Mario: TTYD | 16 | 15 | same SDK as TP: GX setters, PS library, GPR helpers |
| The Wind Waker | 17 | 12 | J3D FIFO / GD writers too |
| Resident Evil 4 | 13 | 1 | PS library, GXSetArray, GXLoadPosMtxImm |
| Mario Party 4 | 12 | 2 | |
| Metroid Prime | 8 | 8 | GX setters masked |
| Super Mario Sunshine | 9 | 2 | |
| Super Smash Bros. Melee | 8 | 2 | |
| F-Zero GX | 3 | 7 | GX setters masked |
| Luigi's Mansion, Sonic Adventure 2, Animal Crossing | 5-6 | 2 | |
| Pikmin | 0 | 2 | oldest SDK |

The paired-single maths library (PSMTX / PSVEC: hand-written assembly) is the same nearly everywhere; `__save_gpr` /
`__restore_gpr` (the CodeWarrior runtime) are exact in 8 games. GX setters follow the SDK generation.

## Plan

1. **Masked recognition** (next): read small-data offsets and `lis` / `addi` constants from each game's code, as the THP
   stand-in already reads its module's bases. Extends today's stand-ins to the 2002 and 2004 games (TTYD, Wind Waker,
   Prime, F-Zero GX, RE4) without new translations.
2. **A save state per game** from James's discs (the way `tp850.state` was made), at a scene the Dolphin wiki or play
   marks as heavy: Sunshine's Delfino Plaza and goop, Melee's four-player stages, Pikmin with 50+ Pikmin, F-Zero GX's
   Sand Ocean, Prime's water and bosses, Wind Waker's Forest Haven.
3. **A 2DS profile per game** (`--profile on`, `hot_funcs.py` with the game's symbol map): its hot functions, by
   library family. The three without a map (TTYD, Luigi's Mansion, SA2: Battle) are profiled by address; TTYD shares
   TP's SDK, so its SDK functions are named by hash.
4. **Translations per SDK generation**: the early SDK (seven games) from one early game's code (Melee or Sunshine:
   both have full maps), then middleware families by reach: JSystem (TP, Wind Waker, Sunshine, Luigi's Mansion, Animal
   Crossing), CRI ADX / Sofdec (RE4, F-Zero GX, SA2: Battle; Sofdec is MPEG-1 video, like THP a decoder worth a
   stand-in), HAL's HSD (Melee), Retro (Prime).

Dolphin wiki notes on CPU-heavy scenes (sources in the research reports): Pikmin with 31+ / 51+ Pikmin needs a higher
emulated CPU clock; F-Zero GX lags on Sand Ocean and Split Oval; Sunshine drops below 20 FPS at the Delfino statue and
with goop; Melee slows at Fountain of Dreams and in four-player matches; Wind Waker drops in heat-effect areas.
