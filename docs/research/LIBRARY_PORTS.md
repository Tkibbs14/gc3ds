# Native library ports

Owner decision (2026-10-18): port shared game libraries natively, for as many games as possible, as long as each port
makes every game that uses it faster. When it was taken, core 0 (the game's own code) was the limit in every Twilight
Princess scene (MEASUREMENTS.md, "Frames ended early, and the command buffer"). Since the first ports, gpbulk and ring
batching the GX thread looked like the limit in gameplay; since overlapped GPU frames (2026-10-19) the joint states show
core 0 the limit in every scene again (MEASUREMENTS.md "Who waits on whom"). Measure ports with separate whole runs:
an A/B of a port recompiles all JIT code at each switch and reads the on arm ~2% low.

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

- **Ported** (core/src/cpu/port_j2d.c; MEASUREMENTS.md, "The first library port"), each checked exact on the device
  (`--port check` runs on the 2DS as well as the PC; `--port plant` proves it reports a difference):
  J2DMaterial::setGX (Ordon +3.6-5.9%), J2DPictureEx::drawSelf (Ordon +4.5% more), J3DShapeMtxConcatView's matrix
  load (Ordon +5.8%; the title neutral), all on by default (`--port-hooks 7`; `--ab port|pichook|j3dhook`). Their
  heavy-area A/Bs read -0.5 to -0.7%, but separate runs give the three together +2.0% there (ports off 20.71%
  against 21.13%, jobs 539-541). Not yet:
  J2DTevBlock1/4/8/16 (the title screen uses 16), indirect stages.
- **Ported, off (2026-10-19, MEASUREMENTS.md "What limits the heavy area, first reading"):**
  `__GXSendFlushPrim` (`--port-hooks` bit 3) and `J3DColorBlockLightOn::diffLightObj` (bit 4: the lights' XF loads
  appended to `__GDCurrentDL`; the context now carries a run of GD display-list bytes, committed and checked byte by
  byte). Both exact on the PC and the device; no measured gain (separate runs: neutral and -0.2% in the heavy area).
  The flush prim is 48 guest instructions a call, too little to win back the port's set-up; the light port's
  byte-by-byte commit (8.7 us a call) leaves little of its saving, so it would need word stores to pay. Measure ports
  with separate runs, not A/Bs (MEASUREMENTS.md "The frame-rate line was broken").
- **Next candidates** (jobs 503-504 profiles): J2DPane::draw (~1.2% of Ordon's core 0), the J3D paths still calling
  GXLoadPosMtxImm (1.4%) and __GXSendFlushPrim (1.1%), JUTResFont::drawChar_scale; for the title's later stretch, the ShdwDraw
  stand-in (~38%) is the lever, not a new port.
- **The J2D pane walk, scoped (2026-10-20; jobs 630-631 profiles, 683-684 `--panestat on`):** J2DPane::draw is 25.4% of
  Ordon's core 0 inclusive (dMeter2Draw_c::draw 14.6%, J2DScreen::draw 13.1%), but its own work is ~3% self; the rest
  is what it draws (J2DPictureEx's port, text, GX calls). Gameplay draws five pane classes, per 10 s in Ordon: J2DPictureEx
  20,090, J2DPane (no drawSelf work) 14,504, J2DTextBoxEx 3,430, J2DScreen 490, J2DPicture 392 (~400 panes a frame;
  heavy area the same mix, 0.7x). A native walk could take every subtree without text boxes (pre-scanned at each pane's
  hook, J2DPictureEx's port inside one shared context, pane stores written through with an undo list), worth about the
  walk's ~3% plus the hook exits; text (J2DTextBoxEx: J2DPrint, JUTResFont) would be the next piece. Not started:
  the per-pane port give-up rate (~2% of J2DPictureEx calls, float edge cases) would abort large subtrees, so the
  give-ups' cause comes first.
- **The give-ups' cause (2026-10-22, job 812, Ordon walk, `# port float give-ups`):**
  - 2,108 of 2,134 float give-ups are `plain()` refusing a **denormal (or NaN) input**: matrix elements or quad
    coordinates of PSMTXConcat and the quad. The other 26 are the FPU off (the OS's lazy switch).
  - The refusal is right. With FPSCR[NI] the interpreter flushes denormal *results* but uses denormal *inputs*
    (gekko_softfp.c round_pack), while the 2DS's VFP (flush-to-zero) zeroes the inputs too. A denormal times a large
    factor is normal on Gekko and zero on the VFP.
  - **For the pane walk:** a pane with such an input should be computed exactly through the interpreter's soft float
    (as hle_ps.c's SOP / SMADD do), for that pane only, rather than aborting the subtree. It is ~2% of panes, so the
    slow path's cost is small.
- **The pane walk's design, read from the machine code (2026-10-20; J2DPane::draw 0x802F67E0, 334 instructions):**
  - Per pane: visible = param && 176(p); the graf context's getGrafType (vtable +32: J2DOrthoGraph 0x802E9C88 `li r3,1;
    blr`, J2DGrafContext 0x802E9664 `li r3,0`); parent = 12(236(p)) when 236(p); bounds valid when 40 >= 32 and 44 >= 36
    (fcmpo + cror: `>=`, NaN fails), else return without children; global bounds 48..60 = bounds 32..44 + translate
    (212, 216) by fadds; visible: clip 64..76 = bounds, rewriteAlpha (vtable +88: J2DPane / J2DPicture / J2DScreen a
    bare `blr`; J2DPictureEx 0x80306A0C: 178(p) = 19(336(p)) when 336(p)). With a parent: + (parent 48 - 32, 52 - 36),
    MTXConcat(parent + 128, p + 80, p + 128); visible and ortho: clip = global bounds intersected with the parent's
    (each `<=` / `>=` test stores the parent's bits); 179 = 178, or 178 * parent 179 / 255 (divw) when 180(p). Without a
    parent: makeMatrix (vtable +68, 0x80053C00, the same for all five classes) and MTXCopy: leave roots to guest code.
  - Visible and ortho: J2DOrthoGraph::scissorBounds 0x802E987C (~50 float operations: the clip through the ortho
    box, clamped); then if the clip is valid or not ortho: a J2DGrafContext copy on the stack (only its scissor box and
    posMtx matter here), scissor 0x802E90C0 (four words) and setScissor 0x802E8E20 (clamps, orders, fctiwz, then
    GXSetScissor), GXSetCullMode(177(p)) (port_j2d.c has it), drawSelf (vtable +56: J2DPane 0x802F83C8 `blr`; J2DPictureEx
    the existing port, with the graf context's posMtx at +124); children through 220(p) / 24(child), each `- 12`,
    called directly (not virtually) with x = y = 0.
  - What it needs that the ports do not have: one context for a whole subtree (pipe ~600 bytes a drawn J2DPictureEx:
    PIPE_CAP 8192 holds ~13), a store log that is not scanned linearly on every read (gx_load_tex_obj logs ~7 words a
    texture and reads them back; LOG_MAX 64), pane fields written through with an undo list (children read their
    parent's just-written fields), check mode undoing those before the interpreter runs and comparing them as log
    entries, and a pre-scan that takes a subtree only when every pane is a J2DPane or J2DPictureEx and the hooked pane
    has a parent. Expected: the walk's ~6,600 cycles a pane (Ordon: ~3,760 panes a second, 3.1% self) plus its GX calls,
    ~+3-3.5% in Ordon, ~2.5% in the heavy area.
- **The pane walk, built (2026-10-23; PORT_PANEWALK, `--port-hooks 39`, off by default):** J2DPane::draw hooked;
  a pane with a parent whose whole subtree draws as J2DPane / J2DPictureEx (drawSelf and rewriteAlpha either class's,
  pre-scanned) is walked natively: bounds, global bounds and the parent's offset, PSMTXConcat, clip against the
  parent's, alpha, J2DOrthoGraph::scissorBounds, J2DGrafContext::setScissor (its ceil and __cvt_fp2unsigned) and
  GXSetScissor, GXSetCullMode, J2DPictureEx's existing drawSelf port, then the children. Pane stores are written
  through with an undo list (children read their parent's fields; a walk that gives up restores memory); the walk's
  pipe bytes and undo list live in linear memory (64 KB, 96 KB). Comparisons follow fcmpo on the singles' bits (the
  VFP flushes denormal inputs). 12 more guest functions in the hash table.
  - **Check mode (jobs 1078-1079, Ordon / heavy area):** ~770 / 580 walks checked per 10 s, ~13,400 interpreter
    instructions each (port estimate ~10,400), ~2,200 pipe bytes each. Every reported difference was at pipe byte
    16,384 exactly: the interpreter's capture buffer was 16 KB on the 3DS (the walk sends up to ~19 KB); all bytes
    before it matched. The buffer is 128 KB in linear memory now (allocated at the first check); its re-check (jobs
    1080-1081) did not run: the 2DS stopped polling during job 1079 (at 38.6 s of 60, cause unknown; no Luma check
    possible remotely).
  - **Re-check (jobs 1080-1081, full runs):** 0 differed (Ordon ~780 walks / 10 s, heavy ~550).
  - **Speed (jobs 1082-1089, two pairs each, same session):** Ordon 31.03 / 30.99 -> 31.72 / 31.69% (**+2.2%**,
    frames 9.26 -> 9.46 a second), heavy area 22.39 / 22.40 -> 22.49 / 22.49% (+0.4%: the GX thread is its limit).
    ~3,070 walks per 10 s in Ordon at ~189 us each. **On by default** (James, 2026-10-23); `--port-hooks 7` without.
  - **Next in J2D:** J2DTextBoxEx (J2DPrint, JUTResFont) so subtrees with text are walked too (~1,800 walks a 10 s
    in Ordon are refused, most for a text box or a root).
- **Guest effects a port must copy, found by the check:** registers the ABI does not promise but the guest code leaves
  changed (PSMTXConcat's stfd / lfd keep ps0 only, so ps1 of f14, f15, f31 change), and exceptions the guest code
  would take (MSR.FP off: the OS's lazy FP switch).
- **Lesson: a port that repeats the SDK's work step by step costs what the JIT's translation does** (~35,600 cycles and
  ~16,300 ARM instructions a call against the guest's 1,920 instructions). What pays is doing less: fields read once
  without a checked read each, pipe bytes stored as words into a buffer the size is known for, the SDK's per-call
  bookkeeping (bpSentNot, dirty bits) folded into one final value, and the pipe path itself (one copy per burst: +3.2% /
  +4.5% for every game). The next ports are written that way from the start.
- **Regenerating hle_ps_gen.inc (checked byte-identical 2026-10-20):** the inputs are objdump listings of the game's
  functions in `/root/psfns` (WSL Debian; game code, never committed), made with the session's `dis.py START END`
  (main.dol bytes through devkitPPC's objdump). In order: `python3 core/tools/ps_translate.py --helpers 803621A0,803621EC`
  PSMTXConcat PSMTXCopy PSMTXMultVec PSMTXMultVecSR PSVECAdd PSVECSubtract PSVECScale PSVECDotProduct
  PSVECCrossProduct PSVECSquareMag PSVECSquareDistance PSMTXIdentity PSMTXScaleApply PSMTXTransApply
  J3DFifoLoadPosMtxImm__FPA4_fUl J3DFifoLoadNrmMtxImm__FPA4_fUl J3DFifoLoadNrmMtxImm3x3__FPA3_fUl GXLoadLightObjImm
  `calcWeightEnvelopeMtx__12J3DMtxBufferFv!` GXSetTevColorIn GXSetTevAlphaIn GXSetTevColorOp GXSetTevAlphaOp
  GXSetTevOrder GXSetTevSwapMode GXSetTevKColorSel GXSetTevKAlphaSel GXSetTevIndirect GXSetChanCtrl GXSetArray
  __GXSetMatrixIndex GXSetTevColor `__GXSendFlushPrim!`, then (10-20) `__pl__4cXyzCFRC3Vec!` `__mi__4cXyzCFRC3Vec!`
  `__ml__4cXyzCFf!` (each NAME.dis; `!` = direct mode). The cXyz operators are the `--psxyz` group: off, heavy area -0.9% (jobs 660-663).
  Then (2026-10-22) other SDK generations' copies, from a PC `MEMDUMP` of each game cut at its symbol addresses
  (mariopartyrd/marioparty4 GMPE01_01, zeldaret/tww GZLE01) into `/root/psfns_mp4` and `/root/psfns_ww`: Mario Party 4's
  `PSMTXROMultVecArray!` `PSMTXConcat` `PSMTXMultVecArray!` `PSMTXRotTrig` `PSMTXIdentity` `PSMTXTrans` `PSMTXScale`
  `PSMTXReorder`, then Wind Waker's `PSMTXConcat` `PSMTXIdentity` `PSMTXRotTrig` `PSMTXScale` `PSMTXTrans` (a repeated
  name gets `_2`, `_3`; recognition is by the bytes' hash). **Not in the table (measured slower, below):** the estimate
  functions, all in direct mode (`!`): TP's
  `PSVECMag` `PSVECNormalize` (`/root/psfns`), Mario Party 4's `PSVECMag` `PSVECNormalize` `PSMTXInvXpose`, Wind
  Waker's `PSVECMag` `PSVECNormalize`. The translator handles frsqrte and fres in direct mode through the interpreter's
  estimate routines (stopping before zero, negative, infinite or out-of-range inputs), fsel, and ps_muls0 by such a
  double. Single arithmetic with a double operand goes through the interpreter's soft float (SOP / SMADD in hle_ps.c),
  whose multiplies round frC to 25 bits as Gekko does. Functions identical to TP's were left out; PSMTXInverse
  (ps_nmadd) and PSMTXRotRad (a call) do not translate usefully. Check with `HLE_FTZ=1 HLE_CHECK=1`.
  The estimate functions were exact (TP 8.6 M, Wind Waker 20.9 M, Mario Party 4 74k calls, 0 differed) but slower:
  the soft-float calls cost more than the JIT's code for these short functions (2DS, jobs 806-809 against 795-799:
  Mario Party 4 13.92 -> 13.17%, Wind Waker 18.87 -> 18.75%, TP heavy area 22.80 -> 22.53%, Ordon 32.02 -> 31.83%).
  The translator keeps the support for an exact path that is also fast.
- **Tools:** core/tools/porthash.py makes the code-hash table from main.dol and the symbol map; `--port-parts on` times
  each part of a call; `--pmu-split port` / `portcommit` count cycles and events around each half.

Sources: zeldaret/tp `libs/JSystem/src/J2DGraph` (18 files, ~11,600 lines), `libs/JSystem/src/JUtility`,
`libs/dolphin/src/gx` (sparse clone at `/root/tpdecomp` in WSL Debian).
