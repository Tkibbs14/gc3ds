#pragma once
// Native library ports (port_j2d.c; docs/research/LIBRARY_PORTS.md).
#include "../common.h"

typedef struct Gekko Gekko;
enum { PORT_MATERIAL, PORT_PICTUREEX, PORT_J3DPNGP, PORT_FLUSHPRIM, PORT_LIGHTS, PORT_PANEWALK, PORT_MATDIFF, PORT_REINITGX, PORT_DECO, PORT_J3DMULTI, PORT_TEXT, PORT_J3DDRAW, PORT_N };  // the hooked guest functions:
// J2DMaterial::setGX, J2DPictureEx::drawSelf, J3DShapeMtxConcatView::loadMtxConcatView_PNGP, __GXSendFlushPrim,
// J3DColorBlockLightOn::diffLightObj, J2DPane::draw (the pane walk over a subtree), J3DMaterial::diff (the material's
// display-list diff, every block), J3DSys::reinitGX, the minimap's renderingDecoration,
// J3DShapeMtxMultiConcatView::load (a skinned shape's matrices), J3DDrawBuffer::drawHead (the J3D packet-draw
// subtree, C56 - **the scan only so far: it tests whether the subtree is the port's and then declines**, so the
// coverage is measured through the port's own read path before any drawing code is written)
typedef struct {
    u64 calls, refused, pipe, checked;
    u64 refusedBy[8];     // by reason (port_j2d.c give_up)
    u64 insReal, insEst;  // check mode: guest instructions the interpreter ran for the checked calls, and the port's estimate
    u64 timed, tRun, tCommit;  // one call in 16 timed (g_profClock ticks): building the context, and committing it
} PortHook;
typedef struct {
    PortHook h[PORT_N];
    u64 bad;
    u32 badHook, badMat, badWhat, badAddr, badWant, badMine, badPipe[2];  // the first difference check mode found
    u32 badHash;   // a guest function whose code differs from the one ported (the ports stay off)
    u32 otherTev;  // the last TEV block setGX not ported
    u64 tSub[8];   // the timed calls' building by part: set-up, colour, texgens, TEV textures, TEV stages, indirect + PE
    u64 mdWhy[8];  // J3DMaterial::diff not taken: texture numbers, texgens, PE blend, TEV stages, block class, room, floats, other
    u64 txWhy[4];  // J2DTextBoxEx::drawSelf not taken: font class or encoding, a byte outside ASCII text, font page change, gradient
} PortStats;
extern PortStats g_portStats;
// Which line of port_j2d.c each give-up came from, per reason (port_j2d.c why_line): the reason codes are coarse -
// reason 5 alone has 21 sites and its old "TEV block type" label was sizing the top open direction from a bucket of
// unrelated causes. `g_portWhyLine[r][i]` is a line number (0: unused), `g_portWhyLineN` its count, and
// `g_portWhyOther[r]` the give-ups past the slots a reason has. Global, not per hook.
#ifndef __3DS__
bool port_whyline_selftest(void);   // GC3DS_PORTWHY_SELFTEST=1 (port_j2d.c): the give-up histogram's own check
#endif
// Why walk_scan refuses a pane subtree, by line (port_j2d.c walk_no): the pane walk is refused more often than it
// runs in TP's heavy area and the give-up histogram traced all of it to that one call, but walk_scan has five ways
// to say no and only some are worth extending.
// The unrecognised panes' drawSelf addresses (port_j2d.c walk_ds), since 100% of walk_scan's refusals are that one
// test. Guest addresses: name them with core/tools/stack_funcs.py.
// --port-picture (ON by default since 2026-11-01): the pane walk accepts J2DPicture panes (C53). Measured with
// --port-walktext as one change: Ordon +3.46% and +3.17% on two builds, heavy area +2.31%. The *drawing* path for a
// visible picture is --port-picture-draw and is still unexercised, so the walk refuses at one.
extern bool g_portPicture;
// --port-picture-draw on: draw a *visible* J2DPicture pane natively instead of refusing the subtree at it. Its own
// switch because `--port-picture`'s value is scan acceptance (covered by every check) while this code has never run
// in one: `drew picture` is 0 in every checked state on both machines, every J2DPicture reached being hidden.
extern bool g_portPictureDraw;
// --port-walktext (ON by default since 2026-11-01): the pane walk accepts J2DTextBoxEx panes and draws them with
// port bit 10's own text_draw_self (C53's other half, and the one that unblocks it: every *visible* J2DPicture in
// TP's HUD shares a subtree with a J2DTextBoxEx, and a subtree is all-or-nothing). Still a separate switch from
// `--port-hooks` bit 10: that bit is the standalone entry hook, which measured Ordon -4.3%.
extern bool g_portWalkText;
extern u64 g_portWalkDrew[2];
// C56's scan: calls, would-be-taken, refused on a packet class, on a matrix class, on a texture-matrix object; then
// what the drawing actually did (taken calls, mat packets, shape draws) and the one refusal the drawing adds.
extern u64 g_j3dDrawScan[5], g_j3dDrawn[3], g_j3dNbtRefused;
// --port-size-refused MASK (check mode only, measurement): interpreter instructions summed over refused calls whose
// reason is in MASK, per hook and reason, so a refusal's cost is the subtree it abandons rather than a count of
// calls. A mask and not a flag because sizing a refusal means executing it, which hides its children from the hook:
// see the comment in port_j2d.c. The figure is an upper bound on the prize, never the prize.
extern u32 g_portSizeRefused;
extern u64 g_portRefusedIns[PORT_N][8];
// --port-softfloat (ON by default since 2026-11-01): PSMTXConcat through the interpreter's exact soft float when an
// input is a denormal, a NaN or an infinity, rather than giving up. Heavy area +0.20% (four runs an arm, complete
// rank separation), static Ordon +0.31%; exact, and the walk's float refusals go from 60 a window to 0.
extern bool g_portSoftFloat;  // panes the walk drew through the C53 halves: [0] J2DPicture, [1] J2DTextBoxEx
extern u64 g_portFloatSite[6];
extern u32 g_portWalkDs[4];
extern u64 g_portWalkDsN[4], g_portWalkDsOther;
extern u32 g_portWalkLine[6];
extern u64 g_portWalkLineN[6], g_portWalkOther;
extern u32 g_portWhyLine[8][6];
extern u64 g_portWhyLineN[8][6], g_portWhyOther[8];
extern const char* const g_portNames[PORT_N];
extern int g_portMode;  // --port: 0 off, 1 on, 2 check (each call compared with the interpreter, whose result is kept), 3 plant
extern bool g_portParts;  // --port-parts on: PortStats.tSub
extern u32 g_portHooks;   // --port-hooks MASK: the hooked functions taken (bit 1 << PORT_*)
void port_set_memory(u8* mem1, u32 size);
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx);
bool port_is_hook(u32 pc);
u32 port_run_hook(Gekko* g);  // guest instructions stood for (0: not taken, the guest code runs)
// --port-charge PCT (100): every port's charge on the emulated clock scaled by PCT percent. Measurement only: the
// charge is an estimate of the guest instructions a call stood for, and C41 found that changing it moves speed more
// than the work it removes. With a knob the sweep is phases of one run instead of a build each.
extern u32 g_portCharge;
// --pmu-split port / portcommit (host3ds): brackets around building the context / committing it
extern void (*g_portPmuBegin)(void), (*g_portPmuEnd)(void), (*g_portCommitBegin)(void), (*g_portCommitEnd)(void);
extern u32 g_portPmuHooks;  // --pmu-port-hooks MASK: the hooks those brackets go around (all)
