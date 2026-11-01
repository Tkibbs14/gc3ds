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

## Status (2026-10-25; default `--port-hooks 503` = bits 0-2, 4-8)

- **Ported** (core/src/cpu/port_j2d.c; MEASUREMENTS.md, "The first library port"), each checked exact on the device
  (`--port check` runs on the 2DS as well as the PC; `--port plant` proves it reports a difference):
  J2DMaterial::setGX (Ordon +3.6-5.9%), J2DPictureEx::drawSelf (Ordon +4.5% more), J3DShapeMtxConcatView's matrix
  load (Ordon +5.8%; the title neutral), all on by default (bits 0-2; `--ab port|pichook|j3dhook`). Their
  heavy-area A/Bs read -0.5 to -0.7%, but separate runs give the three together +2.0% there (ports off 20.71%
  against 21.13%, jobs 539-541). Not yet:
  J2DTevBlock1/4/8/16 (the title screen uses 16), indirect stages.
- **Ported, off (2026-10-19, MEASUREMENTS.md "What limits the heavy area, first reading"):**
  `__GXSendFlushPrim` (`--port-hooks` bit 3) and `J3DColorBlockLightOn::diffLightObj` (bit 4: the lights' XF loads
  appended to `__GDCurrentDL`; the context now carries a run of GD display-list bytes, committed and checked byte by
  byte). Both exact on the PC and the device; no measured gain (separate runs: neutral and -0.2% in the heavy area).
  The flush prim is 48 guest instructions a call, too little to win back the port's set-up; the light port's
  byte-by-byte commit (8.7 us a call) leaves little of its saving, so it would need word stores to pay. **Done 2026-10-24: one copy
  per call, and it pays: heavy area +2.7%, Ordon even (jobs 1315-1322, MEASUREMENTS.md "The J3D light port with a
  one-copy commit"; on by default since 2026-10-24, `--port-hooks 55`).** **Then all of J3DMaterial::diff as one port
  (bit 6, on by default since 2026-10-24, `--port-hooks 119`): heavy area and Ordon +3.0% each, exact (MEASUREMENTS.md "J3DMaterial::diff as one native
  port").** It confirms the rule: the whole subtree pays, the one leaf alone did not. **J3DSys::reinitGX then too (bit 7,
  on by default): Ordon +3.9%, heavy area even (MEASUREMENTS.md "J3DSys::reinitGX as one native port").** The minimap's
  renderingDecoration (bit 8, on: Ordon +3.1%, heavy area -0.4%). **J3DShapeMtxMultiConcatView::load (bit 9) lost
  Ordon 2.9% although the ports' own time fell: off.** **HUD text, J2DTextBoxEx::drawSelf with J2DPrint and
  JUTResFont (bit 10, 2026-10-25): exact, Ordon -4.3%, off;** its cycle charge alone moves Ordon between 27.9% and
  32.7% (REDESIGN_CANDIDATES C41, MEASUREMENTS.md "HUD text as a port, and the ports' cycle charge"), which likely
  explains bit 9 too. Measure ports with separate runs, not A/Bs (MEASUREMENTS.md "The frame-rate line was broken").
- **Next candidates** (jobs 503-504 profiles; J2DPane::draw and text since done, above): first C41, then the J3D paths still calling
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

> **Correction, 2026-10-31 (jobs 1753-1754, build `04824e949fd12861`): there are no denormals. Every one of these
> give-ups is a NaN or an infinity.** The 2026-10-22 counter merged the two cases into one slot, so "denormal (or
> NaN)" was a guess that the instrument could not settle; the parenthesis turned into "denormal" everywhere it was
> quoted afterwards. `plain()` now counts them separately (`# port float give-ups`: `denormal` and `plain nan/inf`)
> and reads, over whole runs at the current default hooks: static Ordon **denormal 0, plain nan/inf 13,216** (864 per
> 10 s, all at `port_j2d.c:892`, `psmtx_concat`'s matrix elements) plus 85 for the FPU off; heavy area **denormal 0,
> plain nan/inf 9,544** (640 per 10 s) plus 69. Floats are ~11% of all give-ups in both scenes; the pane-class test
> is 82% (C53).
> **What this changes.** The *remedy* is unaffected - softfp is exact for NaNs too - but its **cost is not what was
> written**: flushing a denormal input is one line, while matching Gekko's NaN propagation means routing the whole
> `ps_madd` chain through `gekko_softfp.c`, because the VFP runs with default-NaN on (FPSCR 0x03000010, DN+FZ) and
> throws every input payload away. And the *prize* is 11%, not the "prerequisite for big ports" it was treated as:
> C53's one class test is the blocker, and its missing component is already built (port bit 10). So this is no longer
> a prerequisite for anything - it is an 11% tail to pick up after C53, and worth a second counter split (NaN against
> infinity) before anyone builds it, since infinities need no payload matching.
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
    in Ordon are refused, most for a text box or a root). Built 2026-10-25 as bit 10, off until C41 is understood.
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

## C56: the J3D packet draw, designed from zeldaret's sources (2026-11-01)

**Why this subtree.** `J3DDrawBuffer::draw` is **9.88%** of core 0 in TP's heavy area at the current defaults (job
1821), the largest unexplored subtree once C53 is taken out of the J2D containers. The census (`--j3dstat`, jobs
1822-1823) says the class dispatch is *narrower* than the pane walk's: **one packet class of four** and **two matrix
loads of six**, with the other seven entry points reading exactly 0 in both scenes - and both surviving matrix loads
are already written as port bits 9 and 2, the two that measure **-0.46%** and **-0.03%** as standalone entry hooks.
Absorbing them is the whole point: one `drawHead` call covers ~6.3 material loads, ~6.9 shape draws and ~12.2 matrix
loads in the heavy area.

**Hook: `J3DDrawBuffer::drawHead` 0x80325500** (not `draw` 0x803254AC, which dispatches through
`drawFuncTable[mDrawMode]`; the census shows the two called equally often, so `mDrawMode` is always Head in these
scenes, and hooking `drawHead` means a `drawTail` run simply never enters the port).

**The walk.** `for (i < mEntryTableSize) for (packet = mpBuffer[i]; packet; packet = packet->mpNextPacket)
packet->draw()`, taking the subtree only when **every** packet's vtable is `J3DMatPacket`'s - all-or-nothing, as C53
established.

**Per J3DMatPacket (`draw` 0x80312A9C, 132 bytes):**
`mpMaterial->load()`; `callDL()`; `getShapePacket()->getShape()->loadPreDrawSetting()`; then
`while (sp) { if (sp->mpDisplayListObj) sp->mpDisplayListObj->callDL(); sp->drawFast(); sp = sp->mpNextPacket; }`;
then `J3DShape::resetVcdVatCache()`.
`J3DMaterial::load` (0x8031668C, 80 bytes) is only `j3dSys.setMaterialMode(mMaterialMode)` and, unless `j3dSys`
flag 2 is set, `loadNBTScale(*mTexGenBlock->getNBTScale())`.

**Per J3DShapePacket (`drawFast` 0x80312FBC, 136 bytes):** skip when `J3DShpFlag_Hidden` or `!mpShape`; else
`prepareDraw()` (0x80312E08, 284 bytes), then set `J3DDifferedTexMtx::sTexGenBlock` / `sTexMtxObj` from
`mpTexMtxObj` (or clear the first), then `mpShape->drawFast()`.

**`J3DShape::drawFast` (0x8031544C, 404 bytes)** - the core:
`if (sOldVcdVatCmd != mVcdVatCmd) { GXCallDisplayList(mVcdVatCmd, kVcdVatDLSize); sOldVcdVatCmd = mVcdVatCmd; }`;
`if (sEnvelopeFlag && !mHasPNMTXIdx) mCurrentMtx.load()`; `setArrayAndBindPipeline()`; then, unless
`J3DShpFlag_NoMtx`, `for (i < mMtxGroupNum) { mShapeMtx[i]->load(); mShapeDraw[i]->draw(); }` (with
`J3DShapeMtx::resetMtxLoadCache()` first when the LOD flag is set), else two `J3DFifoLoad*MtxImm` of the base matrix
followed by the same `mShapeDraw[i]->draw()` loop.
`setArrayAndBindPipeline` (0x80315398, 180 bytes) is seven stores plus `loadVtxArray`, which is three
`J3DLoadArrayBasePtr` calls - FIFO writes gc3ds already stands in for.

**Offsets, from the decompilation's headers:**
- `J3DDrawBuffer`: +0x00 `mpBuffer`, +0x04 `mEntryTableSize`, +0x08 `mDrawMode`
- `J3DPacket`: +0x00 vtable, +0x04 `mpNextPacket`, +0x08 `mpFirstChild`, +0x0C `mpUserArea`
- `J3DDrawPacket`: +0x10 `mFlags`, +0x20 `mpDisplayListObj`, +0x24 `mpTexMtxObj`
- `J3DMatPacket`: +0x28 `mpInitShapePacket`, +0x2C `mpShapePacket`, +0x30 `mpMaterial`, +0x34 `mDiffFlag`
- `J3DShapePacket`: +0x28 `mpShape`, +0x2C `mpMtxBuffer`, +0x30 `mpBaseMtxPtr`, +0x34 `mDiffFlag`, +0x38 `mpModel`
- `J3DShape`: +0x04 `mMaterial`, +0x0A `mMtxGroupNum`, +0x0C `mFlags`, +0x2C `mVcdVatCmd`, +0x34 `mHasNBT`,
  +0x38 `mShapeMtx`, +0x3C `mShapeDraw`, +0x40 `mCurrentMtx`, +0x48 `mHasPNMTXIdx`, +0x54 `mScaleFlagArray`,
  +0x58 `mDrawMtx`, +0x5C `mNrmMtx`, +0x60 `mCurrentViewNo`
- `J3DMaterial`: +0x10 `mMaterialMode`, +0x20 `mDiffFlag`, +0x24 `mColorBlock`, +0x28 `mTexGenBlock`,
  +0x2C `mTevBlock`, +0x34 `mPEBlock`

**Statics, from the symbol map:** `j3dSys` 0x80434AC8 (already `A_J3DSYS` in `port_j2d.c`),
`J3DShapeMtx::sCurrentPipeline` 0x804515A8, `sCurrentScaleFlag` 0x804515AC, `sNBTFlag` 0x804515B0,
`sTexMtxLoadType` 0x804515B4, `J3DShape::sOldVcdVatCmd` 0x804515D0, `sEnvelopeFlag` 0x804515D4.

**The one gap:** `J3DDifferedTexMtx::sTexGenBlock` and `sTexMtxObj` are not in zeldaret's map under that mangling
(local statics, or named otherwise), so their addresses have to come from the machine code - which needs the disc
image and therefore a machine that has one. Everything else above is settled.

**Viability, checked before writing anything (2026-11-01) - the port can emit what the subtree emits.**
- Most of what the subtree does to the GX side is `callDL()` on guest display lists, and the standard GX opcode
  **`0x40`** (call display list: opcode, u32 address masked `& 0x03FFFFE0`, u32 size - 9 bytes) is fully decoded by
  `fifo.c`, which only ignores such a call when already *inside* a list - and the port's pipe is not. So
  `p8(0x40); p32(addr); p32(size)` is the emitter, and `gx_load_pos_mtx_imm` already exists in `port_j2d.c` for the
  matrix loads. (Do not confuse this with `fifo.c`'s `0x0060`, a gc3ds extension for another path.)
- **`prepareDraw` (284 bytes, the biggest single piece) is pure state plumbing, not GX**: stores into `j3dSys`,
  flags on and off the shape, and pointers copied out of the matrix buffer into the shape, plus
  `J3DVertexBuffer::setArray` (36 bytes) and `J3DModelData::syncJ3DSysFlags` (60 bytes). It **mutates guest memory**,
  so those writes go through the undo list exactly as the pane walk writes pane fields through - the framework
  already does this.
- So the whole port is **a lot of guest-memory stores plus a handful of GX emissions** (display-list calls, array
  base pointers, matrix loads). That is the pane walk's shape, which is the strongest reason to expect it to behave
  the same way.

**What was left to write** was the spine only, ~1.2 KB of guest behaviour: the two loops, `J3DMatPacket::draw`,
`J3DShapePacket::drawFast`, `prepareDraw`, `J3DShape::drawFast`, `loadPreDrawSetting`, `J3DMaterial::load`,
`setArrayAndBindPipeline`, `J3DShapeDraw::draw`, and a `J3DShapeMtxConcatView::load` wrapper around port bit 2's
existing `loadMtxConcatView_PNGP`. **Write it with `--port check` and a `drew N` counter from the first commit:**
two ports in this session reported "0 differed" over zero executed calls, and one of them took a peer's run to catch.

### Written, 1 November 2026 - port bit 11, off, not yet checked or measured

The drawing is in `port_j2d.c` behind `--port-hooks` bit 11 (2048), and it absorbs **both** matrix loads: a
`J3DShapeMtxConcatView` matrix goes through the new single-matrix `j3d_concat_view_load`, which wraps bit 2's
`j3d_load_mtx_concat_view_pngp`, and a `J3DShapeMtxMultiConcatView` matrix goes straight through bit 9's existing
`j3d_multi_concat_load`. That is the absorption the entry hooks could not pay for: bit 2 measures **-0.94%** and
bit 9 **+0.08%** on their own (jobs 1840-1848), and inside a subtree neither pays a hook entry or a commit.

**The gap the design left is closed.** `J3DDifferedTexMtx::sTexGenBlock` and `sTexMtxObj` *are* in zeldaret's map,
at **0x804515C0** and **0x804515C4**; the earlier search missed them by mangling the class name as `16J3D...`
when it is 17 characters. The find is self-checking: port bit 2 already refuses on `f32(r13 - 28608)`, and
r13 - 28608 is 0x804515C0 exactly, so the symbol map and code written months earlier agree on the address. Every
other static resolves the same way, and `j3d_draw_head` now compares `r13` against the value the symbol map
implies (0x80458580) once per call, so the absolute form used by C56 and the r13-relative form used by bits 2 and
9 can never drift apart silently.

**Three things the port refuses, each counted, because each was a prediction rather than a reading:**
- **A shape packet with a texture-matrix object** (`mpTexMtxObj != 0`). It makes `drawFast` set `sTexGenBlock`,
  and the concat-view load then takes its differed path - the one bit 2 has always refused. Refused in the *scan*
  rather than three functions deep, so the reason stays legible and no half-written subtree needs undoing. This is
  new since the scan measured 1,558 of 1,722 calls takeable, so **that figure is now an upper bound** and the gap
  between the two numbers is this class's share.
- **`J3DMaterial::load`'s NBT-scale half**, when `j3dSys` flag 2 is clear. `J3DTexGenBlockPatched` - the class the
  material census found on every material in both scenes - does **not** override `getNBTScale`, so it inherits the
  base class's `return NULL` and the guest would be dereferencing it. That says flag 2 must be set whenever this
  runs, which is a prediction from the headers, so the port declines and `nbt scale N` settles it in one run
  instead.
- **The pipeline being anything but `loadMtxConcatView_PNGP`**, and `sMtxPtrTbl` holding more than two entries -
  both inherited from bit 9, unchanged.

**One bug the writing produced, and what caught it:** `J3DShape::drawFast`'s no-matrix branch loads
`*j3dSys.getShapePacket()->getBaseMtxPtr()`, and a `Mtx` decays straight back to its pointer, so the twelve floats
are *at* `mpBaseMtxPtr` and not at the word it points to. The first draft dereferenced twice. It was caught by
differencing against bit 2, which reads the same field as `psmtx_concat(c, f32(m0 + 48), ...)` - rule 8 applied to
a field offset rather than to an instrument.

**The four new guest functions are recorded with hash 0 on purpose.** `porthash.py` needs the disc image and a
cloud session has none, so a guessed hash would be worse than none. `port_valid()` treats a zero row exactly as a
mismatch - **every** port off, so C53 and the soft-float concat cannot be silently re-enabled by a bad row - and
publishes the hash the console computed as a paste-ready `# port hashes` line. One console run fills them in. The
rows are `loadVtxArray` 0x80314EEC, `J3DLoadCPCmd` 0x80314E98, `resetMtxLoadCache` 0x803130A8 and
`GXCallDisplayList` 0x8035FEF0.

**So the next three steps are, in order:** harvest the four hashes from one short console run (the build has every
port off until then, so it must not be used for speed); then `--port check` with `--port-hooks 2291`, which must
report 0 differed **and** a non-zero `drew` (the J2DPicture port and the soft-float concat both reported "0
differed" over zero executed calls, which is why `drew` is in the report line at all); then calibrate `kInsScale`
for the hook from check mode's instructions per call, which is still the placeholder 100; and only then measure.
