# Test games

Which games gc3ds is tested on, and why: chosen for coverage of engines, libraries and load (CPU, GPU, movies, audio),
so each speed-up can be judged general or game-specific. Images come only from the owner's own discs (dumped with
CleanRip on a Wii); gc3ds never downloads or ships game data.

Status: `-` not tested, `boots`, `in game`, `playable` (stable, most scenes over ~50% speed), `full` (full speed).
Record each device measurement in docs/research/MEASUREMENTS.md and link the relay job here.

## GameCube

| Game | Why | Engine / libraries | Status | Device runs |
|---|---|---|---|---|
| The Legend of Zelda: Twilight Princess (GZ2E01) | the baseline every measurement so far uses | Zelda engine, JSystem | in game (20.5% cutscene) | jobs 2-267 |
| The Legend of Zelda: The Wind Waker | does TP's tuning carry over (shared engine family)? | Zelda engine, JSystem | - | |
| Super Mario Sunshine | JSystem; decompilation exists (native-mode candidate) | JSystem | - | |
| Super Smash Bros. Melee | widely played; light but timing-sensitive | HAL | - | |
| Animal Crossing | light load; decompilation exists | N64-derived | - | |
| Pikmin | early JSystem; THP movies | JSystem, THP | - | |
| Luigi's Mansion | early JSystem; THP movies | JSystem, THP | - | |
| Metroid Prime | float-heavy CPU, demanding | Retro engine | - | |
| F-Zero GX | very heavy CPU and GPU | Sega (Amusement Vision) | - | |
| Paper Mario: The Thousand-Year Door | 2D on 3D, menu-heavy | Intelligent Systems | - | |
| Resident Evil 4 | heavy streaming and movies | Capcom | - | |
| Sonic Adventure 2: Battle | third-party use of the SDK | Sega | - | |
| Mario Party 4 | decompilation exists | Hudson | - | |

## Wii (after GameCube works)

The Wii's CPU runs at 729 MHz (the GameCube's 486 MHz) with more memory: full speed on the 2DS is a much bigger step.
Start with lighter Wii games once GameCube games are playable; the list is to be chosen then.

## What each game should be measured for

- Speed by section (frame_budget.py on a 600 s relay run from a save state), with the section named.
- The device profile by guest function (`--profile on`, core/tools/hot_funcs.py with the game's symbol map if one exists):
  which library replacements (SDK, JSystem, THP) apply and which hot spots are the game's own.
- Whether each replacement is recognised (byte hash) and exact (PC HLE_CHECK) in that game.
