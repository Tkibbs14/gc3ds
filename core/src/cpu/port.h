#pragma once
// Native library ports (port_j2d.c; docs/research/LIBRARY_PORTS.md).
#include "../common.h"

typedef struct Gekko Gekko;
enum { PORT_MATERIAL, PORT_PICTUREEX, PORT_J3DPNGP, PORT_FLUSHPRIM, PORT_LIGHTS, PORT_PANEWALK, PORT_MATDIFF, PORT_REINITGX, PORT_DECO, PORT_J3DMULTI, PORT_N };  // the hooked guest functions:
// J2DMaterial::setGX, J2DPictureEx::drawSelf, J3DShapeMtxConcatView::loadMtxConcatView_PNGP, __GXSendFlushPrim,
// J3DColorBlockLightOn::diffLightObj, J2DPane::draw (the pane walk over a subtree), J3DMaterial::diff (the material's
// display-list diff, every block), J3DSys::reinitGX, the minimap's renderingDecoration,
// J3DShapeMtxMultiConcatView::load (a skinned shape's matrices)
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
} PortStats;
extern PortStats g_portStats;
extern const char* const g_portNames[PORT_N];
extern int g_portMode;  // --port: 0 off, 1 on, 2 check (each call compared with the interpreter, whose result is kept), 3 plant
extern bool g_portParts;  // --port-parts on: PortStats.tSub
extern u32 g_portHooks;   // --port-hooks MASK: the hooked functions taken (bit 1 << PORT_*)
void port_set_memory(u8* mem1, u32 size);
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx);
bool port_is_hook(u32 pc);
u32 port_run_hook(Gekko* g);  // guest instructions stood for (0: not taken, the guest code runs)
// --pmu-split port / portcommit (host3ds): brackets around building the context / committing it
extern void (*g_portPmuBegin)(void), (*g_portPmuEnd)(void), (*g_portCommitBegin)(void), (*g_portCommitEnd)(void);
extern u32 g_portPmuHooks;  // --pmu-port-hooks MASK: the hooks those brackets go around (all)
