# Native library ports

Owner decision (2026-10-18): port shared game libraries natively, for as many games as possible, as long as each port
makes every game that uses it faster. Core 0 (the game's own code) is now the limit in every Twilight Princess scene
(MEASUREMENTS.md, "Frames ended early, and the command buffer"); the GX thread, the vertex worker and the GPU have
headroom.

## Why ports, and what kind

- **Where core 0 goes (job 451, heavy Ordon area, inclusive):** rendering 39%, actors' draw 28.6%, the 2D UI
  (J2DPane::draw) 22%, the HUD meter 12.5%, game logic 19%, J3D model work ~10-20%. Self time is flat (top function 3.4%).
- **Mechanical translation does not pay** (Translator v4, jobs 340-347): ps_translate.py's stand-ins are exact per
  instruction, but each syncs guest registers at entry, exit and every call, so call-heavy code ran slower than the
  JIT's. FP-heavy code ran only ~1.3x faster (calcWeightEnvelopeMtx, job 333).
- **Memoization does not apply to the UI** (C28, jobs 454-456): J2DScreen::draw is ~115,000 instructions against ~3,800
  input words, but ~11 animated words change every frame.
- **What has paid:** native code that is structurally smarter than the guest's (ShdwDraw's walk). So a port is
  hand-written from the decompilation (zeldaret/tp, libs/JSystem, libs/dolphin), with native functions calling each
  other directly (no guest register state between them) and the output written the cheapest way (GX command bytes
  straight into the pipe buffer, not through the GX SDK's per-call bookkeeping).

## Design

- **Entry:** an HLE hook at a subtree's root (J2DScreen::draw first). Arguments from the guest registers; on return the
  guest registers the ABI promises (callee-saved, r1, r2, r13), r3 / f1 results, LR.
- **Recognition:** each ported function names its guest function by size and byte hash (as hle_ps.c does), per game
  build; a port runs only where the hashes match.
- **Virtual calls:** J2D dispatches drawSelf / setGX through guest vtables. Before running natively, the root pre-scans
  the tree: every vtable entry it will call must be a recognised port, else the guest code runs (no native-to-guest
  calls).
- **Effects:** guest memory stores (pane matrices, the GX SDK's shadow state `__GXData`, anything else the guest code
  writes and later code reads) and the pipe bytes, in the guest's order where order is observable.
- **Exactness:** the PC check runs the port and the interpreter from the same state and compares stores, pipe bytes and
  registers (the HLE_CHECK machinery). Integer and control results must match exactly. Floats: the game's code was
  compiled by CodeWarrior, whose fused multiply-adds and operation order the decompiled C does not fix; a port either
  follows the instruction stream for its float math (exact) or is checked with a stated tolerance (relaxed, an option
  like `--fastfloat`; the owner approved non-bit-exact floats as an option, 2026-10-17). Each port records which.
- **Measurement:** an `--ab` switch per port; keep it only if the 2DS shows a gain in a scene that uses it.

## Order

1. **J2DPane::draw's walk and J2DPicture** (the HUD's icons and meters): the matrix chain, alpha inheritance, scissor,
   the quad emission. GX SDK pieces they reach: GXSetTev*, GXLoadTexObj, GXSetVtxDesc / GXSetVtxAttrFmt, GXBegin and the
   vertex writes, GXLoadPosMtxImm.
2. **J2DTextBox / J2DPrint / JUTResFont** (message windows, the talk prompt: dMsgScrnBase_c::draw was 14.8% of core 0 in
   job 374).
3. **J2DPictureEx / J2DTextBoxEx / J2DWindow(Ex) and J2DMaterial::setGX** (material-driven panes).
4. Then J3D's packet draw (J3DDrawBuffer, J3DShapePacket::draw, J3DMaterial) and the matrix work, and the same JSystem
   libraries in other EAD games (GAMES.md: Wind Waker, Sunshine, Pikmin share JSystem).

## Status (2026-10-18)

- **J2DMaterial::setGX is ported** (core/src/cpu/port_j2d.c; MEASUREMENTS.md, "The first library port"): exact on the
  device over ~200,000 calls (`--port check`, which runs on the 2DS as well as the PC; `--port plant` proves it reports
  a difference), on by default, heavy area +1.6-1.7%, Ordon +0.5%. Not yet: J2DTevBlock1/4/8/16 (the title screen uses
  16), indirect stages.
- **Lesson: a port that repeats the SDK's work step by step costs what the JIT's translation does** (~35,600 cycles and
  ~16,300 ARM instructions a call against the guest's 1,920 instructions). What pays is doing less: fields read once
  without a checked read each, pipe bytes stored as words into a buffer the size is known for, the SDK's per-call
  bookkeeping (bpSentNot, dirty bits) folded into one final value, and the pipe path itself (one copy per burst: +3.2% /
  +4.5% for every game). The next ports are written that way from the start.
- **Tools:** core/tools/porthash.py makes the code-hash table from main.dol and the symbol map; `--port-parts on` times
  each part of a call; `--pmu-split port` / `portcommit` count cycles and events around each half.

Sources: zeldaret/tp `libs/JSystem/src/J2DGraph` (18 files, ~11,600 lines), `libs/JSystem/src/JUtility`,
`libs/dolphin/src/gx` (sparse clone at `/root/tpdecomp` in WSL Debian).
