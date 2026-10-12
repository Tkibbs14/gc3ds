# gc3ds

A GameCube (and later Wii) emulator for homebrewed New Nintendo 3DS / New 2DS XL systems, designed around what
that hardware can actually do. Early development: nothing runs games yet.

**Bring your own games.** gc3ds contains no game data, BIOS or firmware, and never will. Use dumps of discs you
own.

## Layout

| Path | What |
|---|---|
| `core/` | The emulator core, portable C11: Gekko CPU, hardware blocks, GX graphics pipeline, DOL loader. Builds for the PC (development and reference) and the 3DS. |
| `bench/` | `gcbench`, the 3DS benchmark used to measure the hardware (CPU JIT shapes, memory, SD, crypto, audio, GPU). |
| `gc-tests/` | Builds libogc's open-source GameCube examples as test programs, and a static instruction-mix analyser. |
| `docs/FINDINGS.md` | Everything measured on a New 2DS XL, and what it means for the design. |
| `docs/REMOTE_RUNS.md` | Testing on the 2DS from anywhere: builds, jobs and perf logs through the dev relay. |
| `docs/ARCHITECTURE.md` | GameCube, Wii and New 3DS hardware reference with sources. |

## Building (WSL / Linux)

```bash
make -C core                     # PC build: core/build/gcemu
core/build/gcemu program.dol --seconds 2 --out out   # run a DOL, save the screen as PNG
bash bench/build.sh              # 3DS benchmark (devkitARM)
bash gc-tests/build_examples.sh  # GameCube test programs (devkitPPC)
```

## Licence

MIT. Dolphin, YAGCD, WiiBrew, 3dbrew and the IBM/ARM manuals were read as documentation; no code is copied from
GPL projects.
