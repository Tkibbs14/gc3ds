// Native library ports (docs/research/LIBRARY_PORTS.md): JSystem functions with the Dolphin GX SDK functions they
// reach, each as one native call hooked at the guest function's entry (PORT_* in port.h): J2D's material set-up
// (J2DMaterial::setGX), J2DPictureEx::drawSelf (the material, the vertex formats, GXBegin with the SDK's dirty-state
// flush, the quad) and J3D's concat-view matrix load (J3DShapeMtxConcatView::loadMtxConcatView_PNGP with either
// class's loadNrmMtx); off by default, __GXSendFlushPrim and J3D's light loads into the GD display list
// (J3DColorBlockLightOn::diffLightObj). The guest code makes tens of small SDK calls for each, each a call and return around a few bit
// inserts into the SDK's shadow state (__GXData) and a register write to the gather pipe; here they share one context:
// __GXData's words loaded at first touch, the pipe bytes collected in one buffer, the other stores (texture objects)
// logged, all committed at the end. Anything not handled (another TEV block type, indirect stages, a callback that is
// not the SDK's default, an address outside MEM1, floating point off) gives up before the commit and the guest code
// runs instead.
//   Each function follows the game's machine code (Twilight Princess, GZ2E01: addresses below), so integer results are
// exact; the code bytes of every guest function this replaces are hashed at first use and the port stays off unless
// they all match. Check mode (--port check; PORT_CHECK=1 on the PC) runs the guest code in the interpreter after each
// port call and compares every word it wrote above the stack pointer, the pipe bytes and the registers the ABI keeps.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gekko_internal.h"
#include "port.h"
#include "memo.h"
#define HLE_EXACTFLOAT  // (the multiply-adds rounded once, as the interpreter: check mode compares bits)
#include "hle_psops.h"

int g_portMode;  // --port off|on|check (main.c; GC3DS_PORT=1 / PORT_CHECK=1 on the PC)
static u8* g_pm;  // MEM1 (guest physical 0)
static u32 g_psz;
static void (*g_ppipe)(void* ctx, const u8* data, u32 len);
static void* g_pctx;
void port_set_memory(u8* mem1, u32 size) {
    g_pm = mem1;
    g_psz = size;
#ifndef __3DS__
    if (getenv("GC3DS_PORT")) g_portMode = 1;
    if (getenv("PORT_CHECK")) g_portMode = !strcmp(getenv("PORT_CHECK"), "plant") ? 3 : 2;  // (plant: --port plant)
    if (getenv("GC3DS_PORT_HOOKS")) g_portHooks = (u32)strtoul(getenv("GC3DS_PORT_HOOKS"), NULL, 0);
#endif
}
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx) { g_ppipe = write; g_pctx = ctx; }
PortStats g_portStats;
void (*g_portPmuBegin)(void), (*g_portPmuEnd)(void), (*g_portCommitBegin)(void), (*g_portCommitEnd)(void);
u32 g_portPmuHooks = ~0u;

// ---- the guest functions (GZ2E01) ----
enum {
    A_MATERIAL_SETGX = 0x802EA38C, A_COLORBLOCK_SETGX = 0x802EB424, A_TEXGENBLOCK_SETGX = 0x802EB570,
    A_TEVBLOCK2_SETGX = 0x802ED584, A_TEVBLOCK2_LOADTEX = 0x802ED874, A_PEBLOCK_SETGX = 0x802F1840,
    A_INDBLOCK_SETGX = 0x802EB1CC, A_INDBLOCKNULL_SETGX = 0x802EB1E0, A_INDBLOCKFULL_SETGX = 0x802F1730,
    A_JUTTEXTURE_LOAD = 0x802DE840, A_TEXMTX_LOAD = 0x802E9C90, A_INDTEVSTAGE_LOAD = 0x802EA044,
    A_GXDEFAULT_TEXREGION = 0x8035921C, A_GXDEFAULT_TLUTREGION = 0x80359318,
    A_GXDATA_PTR = 0x80456580,  // __GXData (r2 - 13440)
    A_CHAN_MAPPING = 0x803A1C00,  // J2DColorBlock::setGX's {GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1}
    A_TEV_C2R = 0x803D2810,       // GXSetTevOrder's channel -> rasterizer colour table
    A_PICEX_DRAWSELF = 0x80303AB4, A_PICEX_DRAWFULLSET = 0x80303B80, A_PICEX_DRAWTEXCOORD = 0x80303BDC,
    A_J3D_PNGP = 0x80313828, A_J3DCONCATVIEW_LOADNRM = 0x80313D28, A_J3DMULTICONCATVIEW_LOADNRM = 0x8031419C,
    A_J3DSYS = 0x80434AC8,  // (j3dSys)
    A_GX_SENDFLUSHPRIM = 0x8035C834, A_J3D_DIFFLIGHTOBJ = 0x8031A8E0, A_J3D_MATDIFF = 0x803167D8,
    A_GDCURRENTDL = 0x80451980,  // (__GDCurrentDL: the GDLObj J3D's display-list writers append to)
    A_PANE_DRAW = 0x802F67E0, A_PANE_DRAWSELF = 0x802F83C8, A_PANE_REWRITEALPHA = 0x802F83CC,
    A_PICEX_REWRITEALPHA = 0x80306A0C, A_GRAFTYPE_ORTHO = 0x802E9C88, A_GRAFTYPE_CONTEXT = 0x802E9664,
};
typedef struct { u32 addr, size; u64 hash; } PortFn;
static const PortFn kFns[] = {
#include "port_j2d_hashes.inc"
};
static int g_portValid = -1;  // -1: not checked yet
static u64 code_hash(u32 addr, u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < size; i += 4) h = (h ^ ram_rd32(g_pm, addr - 0x80000000u + i)) * 0x100000001B3ull;
    return h;
}
static bool port_valid(void) {
    if (g_portValid >= 0) return g_portValid;
    if (!g_pm) return false;
    g_portValid = 1;
    for (u32 i = 0; i < sizeof kFns / sizeof kFns[0]; i++)
        if (code_hash(kFns[i].addr, kFns[i].size) != kFns[i].hash) {
            g_portValid = 0;
            if (!g_portStats.badHash) g_portStats.badHash = kFns[i].addr;
            fprintf(stderr, "port: guest function %08lx differs: the ports stay off\n", (unsigned long)kFns[i].addr);
        }
    return g_portValid;
}

// ---- the context ----
#define GXD_BYTES 0x5B0
#define PIPE_CAP 8192
#define LOG_MAX 64
#define WALK_PIPE (64u << 10)  // the pane walk's pipe bytes (~600 a drawn J2DPictureEx) and stores with their old words
#define WALK_UNDO 8192
static void* walk_alloc(u32 n) {
#ifdef __3DS__
    extern void* linearAlloc(size_t size);
    void* p = linearAlloc(n);
    if (p) return p;
#endif
    return malloc(n);
}
typedef struct {
    Gekko* g;
    u32 gxd;               // __GXData's guest address
    u32 gx[GXD_BYTES / 4]; // its words the call touches (working copy) and as found: loaded at first touch (gxw)
    u32 gx0[GXD_BYTES / 4];
    u8 have[GXD_BYTES / 4];
    u16 touched[GXD_BYTES / 4];
    u32 nt;
    u8 pipe[PIPE_CAP];
    u8* pp;                // the next pipe byte
    u32 pn;                // bytes (when the call has run)
    u32 bpsn, dirty;       // bpSentNot's last value (~0: not set) and the dirty bits set
    u32 log[LOG_MAX][2];   // other stores: address, value (in order; reads see the newest)
    u32 nlog;
    u32 n;                 // guest instructions the call stands for (an estimate: timing only)
    u32 fail;              // why it gives up (the first reason; PortStats.refusedBy)
    // FPR lanes the guest code leaves changed: PSMTXConcat saves f14, f15 and f31 with stfd and restores them with lfd,
    // which loads ps0 only, so their ps1 keep its last values (f31 by its caller too unless the caller saved it whole).
    bool leak;
    float l14, l15, l31;
    u32 gdA, gdN, gdRoom;  // bytes written at a GD display list's write pointer (J3D's writers), from gdA; its room
    u8 gd[2048];
    u32 flushA, flushLen;  // J3DMaterial::diff: the buffer DCFlushRange flushes after the commit (md_flush)
    // The pane walk (PORT_PANEWALK): its stores go straight to guest memory (children read what their parent just
    // wrote), each with the word it replaced, so a walk that gives up puts everything back (walk_undo); its pipe bytes
    // go to a larger buffer. pbase: where this call's pipe bytes start (pipe, or the walk's buffer).
    u8* pbase;
    u32 pcap;
    bool wt;
    u32 (*undo)[3];        // address, old word, new word
    u32 nundo, undoCap;
} Pc;

// Reasons (PortStats.refusedBy): 1 memory or buffers, 2 floats, 3 texture region, 4 TLUT region, 5 TEV block type,
// 6 texgens, 7 indirect stages; 0 the set-up.
static inline void give_up(Pc* c, u32 why) { if (!c->fail) c->fail = why; }
u64 g_portFloatSite[5];  // which float check gave up (reports): NaN/inf, psq GQR, denormal or NaN, drawSelf f7, FPU off
static inline bool in_mem1(u32 a, u32 len) { return a >= 0x80000000u && a - 0x80000000u + len <= g_psz; }
static inline u32* gxw(Pc* c, u32 i) {
    if (!c->have[i]) {
        c->have[i] = 1;
        c->gx[i] = c->gx0[i] = ram_rd32(g_pm, c->gxd - 0x80000000u + 4 * i);
        c->touched[c->nt++] = (u16)i;
    }
    return &c->gx[i];
}
static u32 rd8(Pc* c, u32 a);
static u32 rd32(Pc* c, u32 a) {
    if (a & 3) return rd8(c, a) << 24 | rd8(c, a + 1) << 16 | rd8(c, a + 2) << 8 | rd8(c, a + 3);  // (lwz at J2DTevBlock2 + 18)
    if (a - c->gxd < GXD_BYTES) return *gxw(c, (a - c->gxd) >> 2);
    for (u32 i = c->nlog; i-- > 0;) if (c->log[i][0] == a) return c->log[i][1];
    if (!in_mem1(a, 4)) { give_up(c, 1); return 0; }
    return ram_rd32(g_pm, a - 0x80000000u);
}
static u32 rd8(Pc* c, u32 a) { return (rd32(c, a & ~3u) >> (8 * (3 - (a & 3)))) & 0xFF; }
static void wr32(Pc* c, u32 a, u32 v) {
    if (a - c->gxd < GXD_BYTES && !(a & 3)) { *gxw(c, (a - c->gxd) >> 2) = v; return; }
    if (c->wt) {  // the walk: written through, the old word kept
        if ((a & 3) || !in_mem1(a, 4) || c->nundo >= c->undoCap) { give_up(c, 1); return; }
        u32* u = c->undo[c->nundo++];
        u[0] = a; u[1] = ram_rd32(g_pm, a - 0x80000000u); u[2] = v;
        ram_wr32(g_pm, a - 0x80000000u, v);
        return;
    }
    if ((a & 3) || !in_mem1(a, 4) || c->nlog >= LOG_MAX) { give_up(c, 1); return; }
    c->log[c->nlog][0] = a;
    c->log[c->nlog][1] = v;
    c->nlog++;
}
// Fields of the structures the port only reads (materials, blocks, matrices, the SDK's tables): plain loads after one
// range check per structure (span).
static inline u32 f8(u32 a) { return ram_rd8(g_pm, a - 0x80000000u); }
static inline u32 f16(u32 a) { return ram_rd16(g_pm, a - 0x80000000u); }
static inline u32 f32(u32 a) { return ram_rd32(g_pm, a - 0x80000000u); }
static inline bool span(Pc* c, u32 a, u32 len) {
    if (in_mem1(a, len)) return true;
    give_up(c, 1);
    return false;
}
// __GXData fields by offset. bpSentNot (the u16 at +2) and the dirty bits (DIRTY) only ever take a value or gain bits
// in these functions, so they are kept aside and applied once at the end (port_run).
#define GX(off) (*gxw(c, (off) >> 2))
static inline void bp_sent_not(Pc* c, u32 v) { c->bpsn = v & 0xFFFF; }
#define DIRTY 1452
// The pipe: bytes stored straight into the buffer (a call's counts are bounded before it starts: PIPE_CAP holds it).
static inline void p8(Pc* c, u32 v) { *c->pp++ = (u8)v; }
static inline void p32(Pc* c, u32 v) {
    u8* p = c->pp;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
    c->pp = p + 4;
}
static inline void bp(Pc* c, u32 v) {  // GX_WRITE_RAS_REG
    u8* p = c->pp;
    p[0] = 0x61; p[1] = (u8)(v >> 24); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 8); p[4] = (u8)v;
    c->pp = p + 5;
}
static inline void xf(Pc* c, u32 addr, u32 v) { p8(c, 0x10); p32(c, addr); p32(c, v); }  // one XF register
// rlwimi rA,rS,sh,mb,me (IBM bit numbering): rotate rS left by sh, insert under the mask mb..me.
static inline u32 rlwimi(u32 a, u32 s, u32 sh, u32 mb, u32 me) {
    u32 r = sh ? (s << sh) | (s >> (32 - sh)) : s;
    u32 m = mb <= me ? (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)) : (0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me));
    return (a & ~m) | (r & m);
}

// ---- GX SDK (libs/dolphin/src/gx; each as the game's code at its address) ----
static void gx_set_chan_mat_color(Pc* c, u32 chan, u32 color) {  // 0x8035DA48 (color: the GXColor word)
    u32 r5, r7;
    switch (chan) {
    case 0: r5 = 0; r7 = rlwimi(GX(176), color, 0, 0, 23); break;
    case 1: r5 = 1; r7 = rlwimi(GX(180), color, 0, 0, 23); break;
    case 2: r5 = 0; r7 = rlwimi(GX(176), color & 0xFF, 0, 24, 31); break;
    case 3: r5 = 1; r7 = rlwimi(GX(180), color & 0xFF, 0, 24, 31); break;
    case 4: r5 = 0; r7 = color; break;
    case 5: r5 = 1; r7 = color; break;
    default: return;
    }
    xf(c, 4108 + r5, r7);
    bp_sent_not(c, 1);
    GX(176 + 4 * r5) = r7;
    c->n += 20;
}
static void gx_set_num_chans(Pc* c, u32 n) {  // 0x8035DB30
    GX(516) = rlwimi(GX(516), n, 4, 25, 27);
    xf(c, 4105, n & 0xFF);
    c->dirty |= 4;
    c->n += 15;
}
static void gx_set_chan_ctrl(Pc* c, u32 chan, u32 enable, u32 amb, u32 mat, u32 mask, u32 diff, u32 attn) {  // 0x8035DB6C
    u32 r10 = rlwimi(0, enable & 0xFF, 1, 30, 30);
    r10 = rlwimi(r10, mat, 0, 31, 31);
    r10 = rlwimi(r10, amb, 6, 25, 25);
    if (attn == 0) diff = 0;
    r10 = rlwimi(r10, diff, 7, 23, 24);
    r10 = rlwimi(r10, (2 - attn) != 0, 9, 22, 22);
    r10 = rlwimi(r10, (0u - attn) != 0, 10, 21, 21);
    r10 = rlwimi(r10, mask, 2, 26, 29);
    r10 = rlwimi(r10, mask, 7, 17, 20);
    xf(c, (chan & 3) + 4110, r10);
    if (chan == 4) xf(c, 4112, r10);
    else if (chan == 5) xf(c, 4113, r10);
    bp_sent_not(c, 1);
    c->n += 40;
}
static void gx_set_cull_mode(Pc* c, u32 mode) {  // 0x8035C984
    u32 r5 = rlwimi((mode >> 1) & 1, mode, 1, 30, 30);
    GX(516) = rlwimi(GX(516), r5, 14, 16, 17);
    c->dirty |= 4;
    c->n += 10;
}
static void gx_set_num_tex_gens(Pc* c, u32 n) {  // 0x8035BDFC
    GX(516) = rlwimi(GX(516), n, 0, 28, 31);
    xf(c, 4159, n & 0xFF);
    c->dirty |= 4;
    c->n += 15;
}
static void gx_set_matrix_index(Pc* c, u32 attr) {  // __GXSetMatrixIndex, 0x803605F8
    if ((s32)attr < 5) {
        p8(c, 0x08); p8(c, 0x30); p32(c, GX(128));
        xf(c, 0x1018, GX(128));
    } else {
        p8(c, 0x08); p8(c, 0x40); p32(c, GX(132));
        xf(c, 0x1019, GX(132));
    }
    bp_sent_not(c, 1);
    c->n += 20;
}
static void gx_set_tex_coord_gen2(Pc* c, u32 dst, u32 func, u32 src, u32 mtx, u32 norm, u32 pt) {  // 0x8035BB7C
    u32 row = 5, form = 0, reg = 0;
    if (src <= 20) {
        static const u8 kRow[21] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 5, 5, 5, 5, 5, 5, 5, 2, 2 };
        row = kRow[src];
        form = src <= 3;
    }
    if (func == 1) {
        reg = rlwimi(reg, 0, 1, 30, 30); reg = rlwimi(reg, form, 2, 29, 29);
        reg = rlwimi(reg, 0, 4, 25, 27); reg = rlwimi(reg, row, 7, 20, 24);
    } else if (func == 0) {
        reg = rlwimi(reg, 1, 1, 30, 30); reg = rlwimi(reg, form, 2, 29, 29);
        reg = rlwimi(reg, 0, 4, 25, 27); reg = rlwimi(reg, row, 7, 20, 24);
    } else if (func >= 2 && func <= 9) {
        reg = rlwimi(reg, 0, 1, 30, 30); reg = rlwimi(reg, form, 2, 29, 29);
        reg = rlwimi(reg, 1, 4, 25, 27); reg = rlwimi(reg, row, 7, 20, 24);
        reg = rlwimi(reg, src - 12, 12, 17, 19); reg = rlwimi(reg, func - 2, 15, 14, 16);
    } else if (func == 10) {
        reg = rlwimi(reg, 0, 1, 30, 30); reg = rlwimi(reg, form, 2, 29, 29);
        reg = rlwimi(reg, src == 19 ? 2 : 3, 4, 25, 27); reg = rlwimi(reg, 2, 7, 20, 24);
    }
    xf(c, dst + 4160, reg);
    u32 r8 = rlwimi(0, pt - 64, 0, 26, 31);
    r8 = rlwimi(r8, norm, 8, 23, 23);
    xf(c, dst + 4176, r8);
    switch (dst) {
    case 0: GX(128) = rlwimi(GX(128), mtx, 6, 20, 25); break;
    case 1: GX(128) = rlwimi(GX(128), mtx, 12, 14, 19); break;
    case 2: GX(128) = rlwimi(GX(128), mtx, 18, 8, 13); break;
    case 3: GX(128) = rlwimi(GX(128), mtx, 24, 2, 7); break;
    case 4: GX(132) = rlwimi(GX(132), mtx, 0, 26, 31); break;
    case 5: GX(132) = rlwimi(GX(132), mtx, 6, 20, 25); break;
    case 6: GX(132) = rlwimi(GX(132), mtx, 12, 14, 19); break;
    default: GX(132) = rlwimi(GX(132), mtx, 18, 8, 13); break;
    }
    gx_set_matrix_index(c, dst + 1);
    c->n += 60;
}
// psq_l / psq_st through GQR0 (float, no scale): the interpreter stores a single denormal as a signed zero.
static bool psq_float_ok(Pc* c) { return c->g->spr[SPR_GQR0] == 0 && (c->g->msr & MSR_FP) && (c->g->spr[SPR_HID2] & 0x20000000u); }
static u32 psq_copy(Pc* c, u32 bits) {
    u32 e = (bits >> 23) & 0xFF;
    if (e == 0) return bits & 0x80000000u;
    if (e == 0xFF) { if (!c->fail) g_portFloatSite[0]++; give_up(c, 2); }  // (NaN / infinity: not taken here)
    return bits;
}
static void gx_load_tex_mtx_imm(Pc* c, u32 mtx, u32 id, u32 type) {  // 0x80360320
    u32 addr = id >= 64 ? (id - 64) * 4 + 1280 : id * 4;
    u32 count = type == 1 ? 8 : 12;
    if (!psq_float_ok(c)) { if (!c->fail) g_portFloatSite[1]++; give_up(c, 2); return; }
    p8(c, 0x10);
    p32(c, addr | ((count - 1) << 16));
    u32 words = type == 0 ? 12 : 8;
    for (u32 i = 0; i < words; i++) p32(c, psq_copy(c, f32(mtx + 4 * i)));
    c->n += 30;
}
static void gx_set_tev_order(Pc* c, u32 stage, u32 coord, u32 map, u32 color) {  // 0x8035F6F4
    u32 r10 = map & ~0x100u;
    GX(1364 + 4 * stage) = map;
    u32 tref = 256 + 4 * (stage >> 1);
    u32 r9 = r10 < 8 ? r10 : 0;
    if ((s32)coord >= 8) { GX(1432) &= ~(1u << stage); coord = 0; }
    else GX(1432) |= 1u << stage;
    u32 crc = color == 255 ? 7 : f32(A_TEV_C2R + 4 * color);
    u32 en = (map != 255 && !(map & 0x100)) ? 1 : 0;
    u32 t = GX(tref);
    if (stage & 1) {
        t = rlwimi(t, r9, 12, 17, 19); t = rlwimi(t, coord, 15, 14, 16);
        t = rlwimi(t, crc, 19, 10, 12); t = rlwimi(t, en, 18, 13, 13);
    } else {
        t = rlwimi(t, r9, 0, 29, 31); t = rlwimi(t, coord, 3, 26, 28);
        t = rlwimi(t, crc, 7, 22, 24); t = rlwimi(t, en, 6, 25, 25);
    }
    GX(tref) = t;
    bp(c, t);
    bp_sent_not(c, 0);
    c->dirty |= 1;
    c->n += 60;
}
static void gx_set_tev_color_s10(Pc* c, u32 id, u32 w0, u32 w1) {  // 0x8035F3DC (w0 = r<<16 | g, w1 = b<<16 | a)
    u32 r7 = (id * 2 + 224) << 24;
    r7 = rlwimi(r7, w0, 16, 21, 31);
    r7 = rlwimi(r7, w1, 12, 9, 19);
    bp(c, r7);
    u32 r6 = (id * 2 + 225) << 24;
    r6 = rlwimi(r6, w1, 16, 21, 31);
    r6 = rlwimi(r6, w0, 12, 9, 19);
    bp(c, r6); bp(c, r6); bp(c, r6);
    bp_sent_not(c, 0);
    c->n += 25;
}
static void gx_set_num_tev_stages(Pc* c, u32 n) {  // 0x8035F890
    GX(516) = rlwimi(GX(516), (n & 0xFF) - 1, 10, 18, 21);
    c->dirty |= 4;
    c->n += 10;
}
static void gx_set_tev_color_in(Pc* c, u32 s, u32 a, u32 b, u32 cc, u32 d) {  // 0x8035F224
    u32 r = GX(304 + 4 * s);
    r = rlwimi(r, a, 12, 16, 19); r = rlwimi(r, b, 8, 20, 23); r = rlwimi(r, cc, 4, 24, 27); r = rlwimi(r, d, 0, 28, 31);
    bp(c, r);
    GX(304 + 4 * s) = r;
    bp_sent_not(c, 0);
    c->n += 17;
}
static void gx_set_tev_alpha_in(Pc* c, u32 s, u32 a, u32 b, u32 cc, u32 d) {  // 0x8035F268
    u32 r = GX(368 + 4 * s);
    r = rlwimi(r, a, 13, 16, 18); r = rlwimi(r, b, 10, 19, 21); r = rlwimi(r, cc, 7, 22, 24); r = rlwimi(r, d, 4, 25, 27);
    bp(c, r);
    GX(368 + 4 * s) = r;
    bp_sent_not(c, 0);
    c->n += 17;
}
static void gx_set_tev_op(Pc* c, u32 base, u32 s, u32 op, u32 bias, u32 scale, u32 clamp, u32 reg) {  // 0x8035F2AC / 0x8035F314
    u32 r = rlwimi(GX(base + 4 * s), op, 18, 13, 13);
    if ((s32)op <= 1) { r = rlwimi(r, scale, 20, 10, 11); r = rlwimi(r, bias, 16, 14, 15); }
    else { r = rlwimi(r, op, 19, 10, 11); r = rlwimi(r, 3, 16, 14, 15); }
    r = rlwimi(r, clamp, 19, 12, 12);
    r = rlwimi(r, reg, 22, 8, 9);
    bp(c, r);
    GX(base + 4 * s) = r;
    bp_sent_not(c, 0);
    c->n += 26;
}
static void gx_set_tev_ksel(Pc* c, u32 s, u32 sel, bool alpha) {  // GXSetTevKColorSel 0x8035F4A4 / KAlphaSel 0x8035F500
    u32 off = 432 + 4 * (u32)((s32)s >> 1);
    u32 r = GX(off);
    if (s & 1) r = alpha ? rlwimi(r, sel, 19, 8, 12) : rlwimi(r, sel, 14, 13, 17);
    else r = alpha ? rlwimi(r, sel, 9, 18, 22) : rlwimi(r, sel, 4, 23, 27);
    GX(off) = r;
    bp(c, r);
    bp_sent_not(c, 0);
    c->n += 23;
}
static void gx_set_tev_kcolor(Pc* c, u32 id, u32 col) {  // 0x8035F440 (col: the GXColor word)
    u32 r8 = rlwimi((id * 2 + 224) << 24, col, 8, 24, 31);
    u32 r5 = rlwimi((id * 2 + 225) << 24, col, 24, 24, 31);
    r8 = rlwimi(r8, col, 12, 12, 19); r8 = rlwimi(r8, 8, 20, 8, 11);
    bp(c, r8);
    r5 = rlwimi(r5, col, 28, 12, 19); r5 = rlwimi(r5, 8, 20, 8, 11);
    bp(c, r5);
    bp_sent_not(c, 0);
    c->n += 25;
}
static void gx_set_tev_swap_mode(Pc* c, u32 s, u32 ras, u32 tex) {  // 0x8035F55C
    u32 off = 368 + 4 * s;
    GX(off) = rlwimi(GX(off), ras, 0, 30, 31);
    GX(off) = rlwimi(GX(off), tex, 2, 28, 29);
    bp(c, GX(off));
    bp_sent_not(c, 0);
    c->n += 18;
}
static void gx_set_tev_swap_mode_table(Pc* c, u32 t, u32 r, u32 g, u32 b, u32 a) {  // 0x8035F5A4
    u32 o0 = 432 + 8 * t, o1 = 432 + 4 * (2 * t + 1);
    GX(o0) = rlwimi(GX(o0), r, 0, 30, 31);
    GX(o0) = rlwimi(GX(o0), g, 2, 28, 29);
    bp(c, GX(o0));
    GX(o1) = rlwimi(GX(o1), b, 0, 30, 31);
    GX(o1) = rlwimi(GX(o1), a, 2, 28, 29);
    bp(c, GX(o1));
    bp_sent_not(c, 0);
    c->n += 32;
}
static void gx_set_tev_indirect(Pc* c, u32 stage, u32 ind, u32 fmt, u32 bias, u32 mtx, u32 ws, u32 wt, u32 add,
                                u32 utc, u32 asel) {  // 0x8035ECC0
    u32 r = rlwimi(0, ind, 0, 30, 31);
    r = rlwimi(r, fmt, 2, 28, 29); r = rlwimi(r, bias, 4, 25, 27); r = rlwimi(r, asel, 7, 23, 24);
    r = rlwimi(r, mtx, 9, 19, 22); r = rlwimi(r, ws, 13, 16, 18); r = rlwimi(r, wt, 16, 13, 15);
    r = rlwimi(r, utc, 19, 12, 12); r = rlwimi(r, add, 20, 11, 11); r = rlwimi(r, stage + 16, 24, 0, 7);
    bp(c, r);
    bp_sent_not(c, 0);
    c->n += 27;
}
static void gx_set_num_ind_stages(Pc* c, u32 n) {  // 0x8035F0D4
    GX(516) = rlwimi(GX(516), n & 0xFF, 16, 13, 15);
    c->dirty |= 6;
    c->n += 10;
}
static void gx_set_alpha_compare(Pc* c, u32 comp0, u32 ref0, u32 op, u32 comp1, u32 ref1) {  // 0x8035F624
    u32 r = rlwimi(0xF3000000u, ref0 & 0xFF, 0, 24, 31);
    r = rlwimi(r, ref1 & 0xFF, 8, 16, 23); r = rlwimi(r, comp0, 16, 13, 15); r = rlwimi(r, comp1, 19, 10, 12);
    r = rlwimi(r, op, 22, 8, 9);
    bp(c, r);
    bp_sent_not(c, 0);
    c->n += 17;
}
static u32 cntlzw(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static void gx_set_blend_mode(Pc* c, u32 type, u32 src, u32 dst, u32 op) {  // 0x8035FBF0
    u32 r = rlwimi(GX(464), cntlzw(3 - type), 6, 20, 20);
    r = rlwimi(r, type, 0, 31, 31);
    r = rlwimi(r, cntlzw(2 - type), 28, 30, 30);
    r = rlwimi(r, op, 12, 16, 19);
    r = rlwimi(r, src, 8, 21, 23);
    r = rlwimi(r, dst, 5, 24, 26);
    bp(c, r);
    GX(464) = r;
    bp_sent_not(c, 0);
    c->n += 21;
}
static void gx_set_dither(Pc* c, u32 d) {  // 0x8035FDD8
    GX(464) = rlwimi(GX(464), d, 2, 29, 29);
    bp(c, GX(464));
    bp_sent_not(c, 0);
    c->n += 11;
}
// GXLoadTexObj with the SDK's default region callbacks.
static u32 gx_default_tex_region(Pc* c, u32 obj, u32 id) {  // 0x8035921C
    u32 fmt = f32(obj + 20), mip = f8(obj + 31) & 1, i8 = id & 7;
    u32 base;
    if (fmt == 6) base = mip ? 776 : 648;
    else if (fmt >= 8 && fmt <= 10) base = 520;
    else base = mip ? 648 : 520;
    c->n += 40;
    return c->gxd + base + i8 * 16;
}
static void gx_load_tex_obj(Pc* c, u32 obj, u32 id) {  // GXLoadTexObj 0x8035E414 + GXLoadTexObjPreLoaded 0x8035E298
    if (GX(1224) != A_GXDEFAULT_TEXREGION || id > 7) { give_up(c, 3); return; }
    u32 reg = gx_default_tex_region(c, obj, id);
    u32 r13 = c->g->gpr[13];
    if (!span(c, r13 - 31472u, 56)) return;
    static const s32 kTab[6] = { -31472, -31464, -31456, -31448, -31440, -31432 };
    u32 t[7];
    for (int k = 0; k < 6; k++) t[k] = f8(r13 + (u32)kTab[k] + id);
    wr32(c, obj + 0, rlwimi(rd32(c, obj + 0), t[0], 24, 0, 7));
    wr32(c, obj + 4, rlwimi(rd32(c, obj + 4), t[1], 24, 0, 7));
    wr32(c, obj + 8, rlwimi(rd32(c, obj + 8), t[2], 24, 0, 7));
    wr32(c, reg + 0, rlwimi(rd32(c, reg + 0), t[3], 24, 0, 7));
    wr32(c, reg + 4, rlwimi(rd32(c, reg + 4), t[4], 24, 0, 7));
    wr32(c, obj + 12, rlwimi(rd32(c, obj + 12), t[5], 24, 0, 7));
    bp(c, rd32(c, obj + 0)); bp(c, rd32(c, obj + 4)); bp(c, rd32(c, obj + 8));
    bp(c, rd32(c, reg + 0)); bp(c, rd32(c, reg + 4)); bp(c, rd32(c, obj + 12));
    if (!(f8(obj + 31) & 2)) {  // a palette: the default TLUT region
        if (GX(1228) != A_GXDEFAULT_TLUTREGION) { give_up(c, 4); return; }
        u32 tl = f32(obj + 24);
        if (tl >= 20) { give_up(c, 4); return; }  // (the guest would use a NULL region)
        u32 tr = c->gxd + 904 + tl * 16;
        u32 t6 = f8(r13 + (u32)(-31424) + id);
        wr32(c, tr + 4, rlwimi(rd32(c, tr + 4), t6, 24, 0, 7));
        bp(c, rd32(c, tr + 4));
    }
    GX(1300 + 4 * id) = rd32(c, obj + 8);
    GX(1332 + 4 * id) = rd32(c, obj + 0);
    c->dirty |= 1;
    bp_sent_not(c, 0);
    c->n += 110;
}

static void gx_flush_texture_state(Pc* c) {  // __GXFlushTextureState 0x8035F174
    bp(c, GX(292));
    bp_sent_not(c, 0);
    c->n += 9;
}
static void gx_load_tlut(Pc* c, u32 obj, u32 name) {  // GXLoadTlut 0x8035E4A0, the default TLUT region callback
    if (GX(1228) != A_GXDEFAULT_TLUTREGION || name >= 20) { give_up(c, 4); return; }  // (the guest would use a NULL region)
    u32 r = c->gxd + 904 + name * 16;
    gx_flush_texture_state(c);
    bp(c, rd32(c, obj + 4));
    bp(c, rd32(c, r));
    gx_flush_texture_state(c);
    wr32(c, obj, rlwimi(rd32(c, obj), rd32(c, r), 0, 22, 31));
    wr32(c, r + 4, rd32(c, obj));
    wr32(c, r + 8, rd32(c, obj + 4));
    wr32(c, r + 12, rd32(c, obj + 8));
    c->n += 40;
}

// ---- J2D (libs/JSystem/src/J2DGraph) ----
static void jut_texture_load(Pc* c, u32 tex, u32 id) {  // JUTTexture::load 0x802DE840
    u32 pal = f32(tex + 44);
    if (pal && !span(c, pal, 24)) return;
    if (pal) {  // JUTPalette::load 0x802DE95C
        if (f16(pal + 20)) gx_load_tlut(c, pal, f8(pal + 12));
        c->n += 12;
    }
    gx_load_tex_obj(c, tex, id);
    c->n += 20;
}
// Part timers (one call in 16 when timed, port_run_hook): the time since the last mark goes to tSub[k].
static bool g_tOn;
bool g_portParts;  // --port-parts on: the timed calls also time each part (five more clock reads each)
static u64 g_tm;
static inline void tmark(int k) {
    if (!g_tOn) return;
    u64 t = g_profClock();
    g_portStats.tSub[k] += t - g_tm;
    g_tm = t;
}
static void tev_block2_set_gx(Pc* c, u32 b) {  // J2DTevBlock2::setGX 0x802ED584
    if (!span(c, b, 108)) return;
    u32 vt = f32(b);
    if (!span(c, vt + 16, 4)) return;
    if (f32(vt + 16) != A_TEVBLOCK2_LOADTEX) { give_up(c, 5); return; }
    for (u32 i = 0; i < 2; i++) {  // loadTexture 0x802ED874
        u32 tex = f32(b + 100 + 4 * i);
        if (tex && !span(c, tex, 48)) return;
        if (tex && f32(tex + 32)) jut_texture_load(c, tex, i);
        c->n += 12;
    }
    tmark(3);
    u32 ns = f8(b + 50);
    if (ns > 2) { give_up(c, 1); return; }  // (a J2DTevBlock2 has two stages)
    for (u32 i = 0; i < ns; i++) gx_set_tev_order(c, i, f8(b + 10 + 4 * i), f8(b + 11 + 4 * i), f8(b + 12 + 4 * i));
    for (u32 i = 0; i < 4; i++) gx_set_tev_color_s10(c, i == 3 ? 0 : i + 1, f32(b + 18 + 8 * i), f32(b + 22 + 8 * i));
    gx_set_num_tev_stages(c, ns);
    for (u32 i = 0; i < ns; i++) {
        u32 s = b + 8 * i;
        u32 b53 = f8(s + 53), b54 = f8(s + 54), b57 = f8(s + 57), b58 = f8(s + 58);
        gx_set_tev_color_in(c, i, b53 >> 4, b53 & 15, b54 >> 4, b54 & 15);
        gx_set_tev_alpha_in(c, i, (b57 & 0xE0) >> 5, (b57 >> 2) & 7, ((b57 & 3) << 1) | (b58 >> 7), (b58 >> 4) & 7);
        for (int a = 0; a < 2; a++) {
            u32 v = f8(s + (a ? 56 : 52));
            u32 bias = v & 3, scale = (v & 0x30) >> 4, clamp = (v >> 3) & 1, reg = (v >> 6) & 3, r9 = (v >> 2) & 1;
            u32 op = bias == 3 ? ((r9 + ((v >> 3) & 6) + 8) & 0xFF) : r9;
            gx_set_tev_op(c, a ? 368 : 304, i, op, bias, scale, clamp, reg);
        }
        u32 k = f8(b + 84 + i);
        gx_set_tev_ksel(c, i, k == 255 ? 0 : k, false);
        k = f8(b + 86 + i);
        gx_set_tev_ksel(c, i, k == 255 ? 0 : k, true);
        c->n += 60;
    }
    for (u32 i = 0; i < 4; i++) gx_set_tev_kcolor(c, i, f32(b + 68 + 4 * i));
    for (u32 i = 0; i < ns; i++) { u32 v = f8(b + 58 + 8 * i); gx_set_tev_swap_mode(c, i, v & 3, (v >> 2) & 3); }
    for (u32 i = 0; i < 4; i++) {
        u32 v = f8(b + 88 + i);
        gx_set_tev_swap_mode_table(c, i, (v >> 6) & 3, (v >> 4) & 3, (v >> 2) & 3, v & 3);
    }
    for (u32 i = 0; i < ns; i++) {  // J2DIndTevStage::load 0x802EA044
        u32 w = f32(b + 92 + 4 * i);
        gx_set_tev_indirect(c, i & 0xFF, w & 3, (w >> 2) & 3, (w >> 4) & 7, (w >> 16) & 15, (w >> 8) & 7, (w >> 11) & 7,
                            (w >> 20) & 1, (w >> 21) & 1, (w >> 22) & 3);
        c->n += 20;
    }
    c->n += 80;
}
static void material_set_gx(Pc* c, u32 m) {  // J2DMaterial::setGX 0x802EA38C
    c->n += 15;
    if (!span(c, m, 132)) return;
    if (!f8(m + 14)) return;
    {  // J2DColorBlock::setGX 0x802EB424 (this = m + 16)
        u32 b = m + 16;
        for (u32 i = 0; i < 2; i++) gx_set_chan_mat_color(c, 4 + i, f32(b + 4 * i));
        gx_set_num_chans(c, f8(b + 8));
        u32 nc = f8(b + 8) << 1;
        if (nc > 4) { give_up(c, 1); return; }  // (past the four-entry channel table: not taken)
        for (u32 i = 0; i < nc; i++)
            gx_set_chan_ctrl(c, f32(A_CHAN_MAPPING + 4 * i), 0, 0, f16(b + 10 + 2 * i) & 1, 0, 0, 2);
        gx_set_cull_mode(c, f8(b + 18));
        c->n += 40;
    }
    tmark(1);
    {  // J2DTexGenBlock::setGX 0x802EB570 (this = m + 40)
        u32 b = m + 40, n = f32(b);
        gx_set_num_tex_gens(c, n & 0xFF);
        if (n > 8) { give_up(c, 6); return; }
        for (u32 i = 0; i < n; i++)
            gx_set_tex_coord_gen2(c, i, f8(b + 4 + 4 * i), f8(b + 5 + 4 * i), f8(b + 6 + 4 * i), 0, 125);
        for (u32 i = 0; i < 8; i++) {
            u32 tm = f32(b + 36 + 4 * i);
            if (tm && !span(c, tm, 84)) return;
            if (tm) gx_load_tex_mtx_imm(c, tm + 36, i * 3 + 30, f8(tm));  // J2DTexMtx::load 0x802E9C90
        }
        c->n += 30;
    }
    tmark(2);
    u32 tev = f32(m + 112);
    if (tev) {
        if (!span(c, tev, 4) || !span(c, f32(tev) + 12, 4)) return;
        u32 fn = f32(f32(tev) + 12);
        if (fn == A_TEVBLOCK2_SETGX) tev_block2_set_gx(c, tev);
        else { give_up(c, 5); g_portStats.otherTev = fn; return; }
    }
    tmark(4);
    u32 ind = f32(m + 116);
    if (ind) {
        if (!span(c, ind, 8) || !span(c, f32(ind) + 12, 4)) return;
        u32 fn = f32(f32(ind) + 12);
        if (fn == A_INDBLOCKFULL_SETGX) {
            u32 ns = f8(ind + 4);
            if (ns) { give_up(c, 7); return; }  // (indirect stages: not ported)
            gx_set_num_ind_stages(c, 0);
        } else if (fn != A_INDBLOCKNULL_SETGX && fn != A_INDBLOCK_SETGX) {
            give_up(c, 7);
            return;
        }
    }
    {  // J2DPEBlock::setGX 0x802F1840 (this = m + 120)
        u32 b = m + 120, w = f16(b);
        gx_set_alpha_compare(c, (w >> 5) & 7, f8(b + 2), (w >> 3) & 3, w & 7, f8(b + 3));
        gx_set_blend_mode(c, f8(b + 4), f8(b + 5), f8(b + 6), f8(b + 7));
        gx_set_dither(c, f8(b + 8));
    }
}

// ---- vertex formats and GXBegin (GX SDK) ----
// A byte of __GXData (the u8 fields at 1236, 1237, 1451).
static inline u32 gxb(Pc* c, u32 off) { return (GX(off & ~3u) >> (8 * (3 - (off & 3)))) & 0xFF; }
static inline void gxb_set(Pc* c, u32 off, u32 v) {
    u32 sh = 8 * (3 - (off & 3));
    GX(off & ~3u) = (GX(off & ~3u) & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
}
static inline void p16(Pc* c, u32 v) { p8(c, v >> 8); p8(c, v); }
// bpSentNot and the dirty bits made current (gx_begin reads both).
static void gx_sync(Pc* c) {
    if (c->bpsn != ~0u) { GX(0) = (GX(0) & 0xFFFF0000u) | c->bpsn; c->bpsn = ~0u; }
    if (c->dirty) { GX(DIRTY) |= c->dirty; c->dirty = 0; }
}
static void gx_clear_vtx_desc(Pc* c) {  // 0x8035B58C
    GX(20) = rlwimi(0, 1, 9, 21, 22);
    GX(24) = 0;
    gxb_set(c, 1236, 0);
    gxb_set(c, 1237, 0);
    c->dirty |= 8;
    c->n += 14;
}
static void gx_set_vtx_desc(Pc* c, u32 attr, u32 type) {  // 0x8035AEB8 (POS, CLR0, TEX0: the ones J2D uses)
    if (attr == 9) GX(20) = rlwimi(GX(20), type, 9, 21, 22);
    else if (attr == 11) GX(20) = rlwimi(GX(20), type, 13, 17, 18);
    else if (attr == 13) GX(24) = rlwimi(GX(24), type, 0, 30, 31);
    else { give_up(c, 1); return; }
    GX(20) = rlwimi(GX(20), gxb(c, 1236) || gxb(c, 1237) ? GX(1232) : 0, 11, 19, 20);
    c->dirty |= 8;
    c->n += 22;
}
static void gx_set_vtx_attr_fmt(Pc* c, u32 fmt, u32 attr, u32 cnt, u32 type, u32 frac) {  // 0x8035B5C4 (POS, TEX0)
    if (fmt > 7) { give_up(c, 1); return; }
    u32 a = 28 + 4 * fmt;
    if (attr == 9) {
        GX(a) = rlwimi(GX(a), cnt, 0, 31, 31);
        GX(a) = rlwimi(GX(a), type, 1, 28, 30);
        GX(a) = rlwimi(GX(a), frac, 4, 23, 27);
    } else if (attr == 13) {
        GX(a) = rlwimi(GX(a), cnt, 21, 10, 10);
        GX(a) = rlwimi(GX(a), type, 22, 7, 9);
        GX(a) = rlwimi(GX(a), frac, 25, 2, 6);
    } else {
        give_up(c, 1);
        return;
    }
    c->dirty |= 16;
    gxb_set(c, 1451, gxb(c, 1451) | ((1u << fmt) & 0xFF));
    c->n += 26;
}
static void gx_set_su_regs(Pc* c, u32 map, u32 tc) {  // __SetSURegs 0x8035E750
    if (map > 7 || tc > 7) { give_up(c, 1); return; }
    u32 img = GX(1300 + 4 * map), mode = GX(1332 + 4 * map);
    u32 s0 = 184 + 4 * tc, s1 = 216 + 4 * tc;
    GX(s0) = rlwimi(GX(s0), img & 0x3FF, 0, 16, 31);
    GX(s1) = rlwimi(GX(s1), (img >> 10) & 0x3FF, 0, 16, 31);
    GX(s0) = rlwimi(GX(s0), (mode & 3) == 1, 16, 15, 15);
    GX(s1) = rlwimi(GX(s1), ((mode >> 2) & 3) == 1, 16, 15, 15);
    bp(c, GX(s0));
    bp(c, GX(s1));
    bp_sent_not(c, 0);
    c->n += 38;
}
static void gx_set_su_tex_regs(Pc* c) {  // __GXSetSUTexRegs 0x8035E7F0
    c->n += 12;
    if (GX(1428) == 255) return;
    u32 gm = GX(516), ns = ((gm >> 10) & 15) + 1, ni = (gm >> 16) & 7;
    if (ni > 4) { give_up(c, 1); return; }  // (the guest code would use registers left from before)
    for (u32 i = 0; i < ni; i++) {
        u32 ir = GX(288), map = (ir >> (6 * i)) & 7, tc = (ir >> (6 * i + 3)) & 7;
        if (!(GX(1428) & (1u << tc))) gx_set_su_regs(c, map, tc);
        c->n += 14;
    }
    for (u32 i = 0; i < ns; i++) {
        u32 map = GX(1364 + 4 * i) & ~0x100u, tr = GX(256 + 4 * (i >> 1));
        u32 tc = i & 1 ? (tr >> 15) & 7 : (tr >> 3) & 7;
        if (map != 255 && !(GX(1428) & (1u << tc)) && (GX(1432) & (1u << i))) gx_set_su_regs(c, map, tc);
        c->n += 20;
    }
}
static void gx_set_vcd(Pc* c) {  // __GXSetVCD 0x8035B3AC
    p8(c, 0x08); p8(c, 0x50); p32(c, GX(20));
    p8(c, 0x08); p8(c, 0x60); p32(c, GX(24));
    u32 nrm = gxb(c, 1237) ? 2 : gxb(c, 1236) ? 1 : 0;
    u32 clr = (33 - cntlzw((GX(20) >> 13) & 15)) >> 1, tex = ((33 - cntlzw(GX(24) & 0xFFFF)) << 3) & ~15u;
    xf(c, 4104, clr | nrm << 2 | tex);
    bp_sent_not(c, 1);
    c->n += 47;
}
static void gx_set_vat(Pc* c) {  // __GXSetVAT 0x8035BAA0
    for (u32 vd = gxb(c, 1451), i = 0; vd; vd >>= 1, i++) {
        if (vd & 1) {
            p8(c, 0x08); p8(c, 0x70 | i); p32(c, GX(28 + 4 * i));
            p8(c, 0x08); p8(c, 0x80 | i); p32(c, GX(60 + 4 * i));
            p8(c, 0x08); p8(c, 0x90 | i); p32(c, GX(92 + 4 * i));
            c->n += 16;
        }
        c->n += 4;
    }
    gxb_set(c, 1451, 0);
    c->n += 8;
}
static void gx_calculate_vlim(Pc* c) {  // __GXCalculateVLim 0x8035B468 (its tables at r13-31488, -31484, -31480)
    c->n += 4;
    if (!(GX(4) >> 16)) return;
    u32 t = c->g->gpr[13] - 31488u;
    if (!span(c, t, 12)) return;
    u32 lo = GX(20), a0 = GX(28), hi = GX(24), v = 0;
    for (u32 k = 0; k < 9; k++) v += (lo >> k) & 1;
    v += f8(t + 8 + ((lo >> 9) & 3));
    v += f8(t + 8 + ((lo >> 11) & 3)) * (((a0 >> 9) & 1) == 1 ? 3 : 1);
    v += f8(t + ((lo >> 13) & 3)) + f8(t + ((lo >> 15) & 3));
    for (u32 k = 0; k < 8; k++) v += f8(t + 4 + ((hi >> (2 * k)) & 3));
    GX(4) = (GX(4) & 0xFFFF0000u) | (v & 0xFFFF);
    c->n += 69;
}
static void gx_send_flush_prim(Pc* c) {  // __GXSendFlushPrim 0x8035C834: a strip of vNum vertices of vLim bytes, all zero
    u32 num = GX(4) >> 16, lim = GX(4) & 0xFFFF, bytes = num * lim, words = (bytes + 3) >> 2;
    p8(c, 0x98);
    p16(c, num);
    if (bytes) {
        if (words > 256) { give_up(c, 1); return; }
        for (u32 i = 0; i < words; i++) p32(c, 0);
    }
    bp_sent_not(c, 1);
    c->n += 20 + words;
}
static void gx_begin(Pc* c, u32 type, u32 fmt, u32 n) {  // GXBegin 0x8035C764 (with __GXSetDirtyState's work)
    gx_sync(c);
    u32 d = GX(DIRTY);
    if (d) {
        if (d & 1) gx_set_su_tex_regs(c);
        if (d & 4) { bp(c, GX(516)); bp_sent_not(c, 0); c->n += 9; }  // __GXSetGenMode (bit 2, __GXUpdateBPMask, is empty)
        if (d & 8) gx_set_vcd(c);
        if (d & 16) gx_set_vat(c);
        if (d & 24) gx_calculate_vlim(c);
        GX(DIRTY) = 0;
        gx_sync(c);
    }
    if (!GX(0)) { gx_send_flush_prim(c); gx_sync(c); }
    p8(c, fmt | type);
    p16(c, n);
    c->n += 30;
}
static void gx_load_pos_mtx_imm(Pc* c, const u32* m, u32 id) {  // 0x8036024C (psq_l / psq_st through GQR0)
    p8(c, 0x10);
    p32(c, (id << 2) | 0xB0000u);
    for (u32 i = 0; i < 12; i++) p32(c, psq_copy(c, m[i]));
    c->n += 19;
}

// ---- J2DPictureEx's quad (libs/JSystem/src/J2DGraph/J2DPictureEx.cpp) ----
static inline float fbits(u32 v) { float f; memcpy(&f, &v, 4); return f; }
static inline u32 bitsf(float f) { u32 v; memcpy(&v, &f, 4); return v; }
// A single the VFP and the Gekko treat alike: zero or normal (denormals, infinities and NaNs are not taken).
static inline bool plain(Pc* c, u32 v) {
    u32 e = (v >> 23) & 0xFF;
    if ((e == 0 && (v & 0x7FFFFF)) || e == 0xFF) { if (!c->fail) g_portFloatSite[2]++; give_up(c, 2); return false; }
    return true;
}
// PSMTXConcat 0x803464E4: ab = a * b (3x4), as its paired-single code computes it (multiply-adds rounded once).
static void psmtx_concat(Pc* c, u32 a, u32 b, u32* out) {
    if (!span(c, a, 48) || !span(c, b, 48) || !span(c, 0x804509D0u, 8)) return;
    P m[12];
    for (u32 i = 0; i < 6; i++) {
        u32 x0 = f32(a + 8 * i), x1 = f32(a + 8 * i + 4), y0 = f32(b + 8 * i), y1 = f32(b + 8 * i + 4);
        if (!plain(c, x0) || !plain(c, x1) || !plain(c, y0) || !plain(c, y1)) return;
        m[i] = (P){ fbits(x0), fbits(x1) };
        m[6 + i] = (P){ fbits(y0), fbits(y1) };
    }
    P f0 = m[0], f1 = m[1], f2 = m[2], f3 = m[3], f4 = m[4], f5 = m[5];  // a's rows as register pairs
    P f6 = m[6], f7 = m[7], f8_ = m[8], f9 = m[9], f10 = m[10], f11 = m[11];
    P f31 = { fbits(f32(0x804509D0u)), fbits(f32(0x804509D4u)) };  // {0, 1}
    P f12 = PMULS0(f6, f0), f13 = PMULS0(f7, f0), f14 = PMULS0(f6, f2), f15 = PMULS0(f7, f2);
    f12 = PMADDS1(f8_, f0, f12); f14 = PMADDS1(f8_, f2, f14); f13 = PMADDS1(f9, f0, f13); f15 = PMADDS1(f9, f2, f15);
    f12 = PMADDS0(f10, f1, f12); f13 = PMADDS0(f11, f1, f13); f14 = PMADDS0(f10, f3, f14); f15 = PMADDS0(f11, f3, f15);
    P g2 = PMULS0(f6, f4);
    f13 = PMADDS1(f31, f1, f13);
    P g0 = PMULS0(f7, f4);
    f15 = PMADDS1(f31, f3, f15);
    g2 = PMADDS1(f8_, f4, g2); g0 = PMADDS1(f9, f4, g0); g2 = PMADDS0(f10, f5, g2); g0 = PMADDS0(f11, f5, g0);
    g0 = PMADDS1(f31, f5, g0);
    c->leak = true;
    c->l14 = f14.p1;
    c->l15 = f15.p1;
    c->l31 = f31.p1;
    P r[6] = { f12, f13, f14, f15, g2, g0 };
    for (u32 i = 0; i < 6; i++) {  // psq_st to the caller's temporary
        out[2 * i] = psq_copy(c, bitsf(r[i].p0));
        out[2 * i + 1] = psq_copy(c, bitsf(r[i].p1));
    }
    c->n += 52;
}
// J2DPictureEx::drawTexCoord 0x80303BDC as drawFullSet 0x80303B80 calls it: the box's corner, its size (w, h: single
// bits), the texture coordinates at +266..+280, and the matrix the pane was given.
static void picex_draw_tex_coord(Pc* c, u32 p, u32 w, u32 h, u32 mtx) {
    u32 x0 = f32(p + 32), y0 = f32(p + 36), z = 0;
    if (!plain(c, x0) || !plain(c, y0)) return;
    u32 x1 = bitsf(fbits(x0) + fbits(w)), y1 = bitsf(fbits(y0) + fbits(h));  // fadds
    if (!plain(c, x1) || !plain(c, y1)) return;
    u32 m[12];
    psmtx_concat(c, mtx, p + 128, m);
    if (c->fail) return;
    u32 mat = f32(p + 336);
    if (mat && !f8(mat + 14)) { c->n += 40; return; }
    gx_load_pos_mtx_imm(c, m, 0);
    u32 col[4];
    for (u32 i = 0; i < 4; i++) col[i] = f32(p + 312 + 4 * i);
    if (mat) {
        if ((f16(mat + 28) & 1) == 1) {
            if (f8(mat + 15) == 1) {
                u32 al = f8(p + 179);
                for (u32 i = 0; i < 4; i++) col[i] = (col[i] & ~0xFFu) | (((s32)((col[i] & 0xFF) * al) / 255) & 0xFF);
                c->n += 20;
            }
        } else if (f8(p + 180)) {
            gx_set_chan_mat_color(c, 2, f8(p + 179));
        }
    }
    gx_set_vtx_attr_fmt(c, 0, 9, 1, 4, 0);
    gx_set_vtx_attr_fmt(c, 0, 13, 1, 3, 8);
    u32 zr = c->g->gpr[2] - 14140u;
    if (!span(c, zr, 4)) return;
    z = f32(zr);
    if (!plain(c, z)) return;
    gx_begin(c, 128, 0, 4);
    if (c->fail) return;
    static const u8 kCorner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    static const u8 kCol[4] = { 0, 1, 3, 2 };
    static const u16 kTc[4] = { 266, 270, 278, 274 };
    for (u32 v = 0; v < 4; v++) {
        p32(c, kCorner[v][0] ? x1 : x0);
        p32(c, kCorner[v][1] ? y1 : y0);
        p32(c, z);
        p32(c, col[kCol[v]]);
        p16(c, f16(p + kTc[v]));
        p16(c, f16(p + kTc[v] + 2));
    }
    gx_set_vtx_attr_fmt(c, 0, 13, 1, 2, 15);
    gx_set_vtx_attr_fmt(c, 0, 9, 1, 3, 0);
    c->n += 150;
}
static void picex_draw_self(Pc* c, u32 p, u32 mtx) {  // J2DPictureEx::drawSelf 0x80303AB4
    c->n += 20;
    if (!span(c, p, 344)) return;
    u32 mat = f32(p + 336);
    if (!mat) return;
    material_set_gx(c, mat);
    if (c->fail) return;
    gx_clear_vtx_desc(c);
    gx_set_vtx_desc(c, 9, 1);
    gx_set_vtx_desc(c, 11, 1);
    gx_set_vtx_desc(c, 13, 1);
    u32 x0 = f32(p + 32), y0 = f32(p + 36), x1 = f32(p + 40), y1 = f32(p + 44);
    if (!plain(c, x0) || !plain(c, y0) || !plain(c, x1) || !plain(c, y1)) return;
    u32 w = bitsf(fbits(x1) - fbits(x0)), h = bitsf(fbits(y1) - fbits(y0));  // fsubs
    if (!plain(c, w) || !plain(c, h)) return;
    u32 vt = f32(p);
    if (!span(c, vt + 320, 8)) return;
    if (f32(vt + 320) != A_PICEX_DRAWFULLSET || f32(vt + 324) != A_PICEX_DRAWTEXCOORD) { give_up(c, 5); return; }
    picex_draw_tex_coord(c, p, w, h, mtx);
    c->n += 40;
}

// ---- the pane walk: J2DPane::draw 0x802F67E0 over a whole subtree (libs/JSystem/src/J2DGraph/J2DPane.cpp) ----
// Taken when the hooked pane has a parent and every pane below it draws as J2DPane or J2DPictureEx (drawSelf and
// rewriteAlpha either class's); otherwise the guest code runs, and its calls to each child's draw come back here.
// Comparisons follow fcmpo + cror on the singles' bits (the VFP flushes denormal inputs, the Gekko does not); every
// float operation's inputs and result are plain() (zero or normal), so the VFP's single result is the Gekko's.
static inline bool fnan(u32 v) { return ((v >> 23) & 0xFF) == 0xFF && (v & 0x7FFFFF); }
static inline s32 fkey(u32 v) { return v & 0x80000000u ? -(s32)(v & 0x7FFFFFFFu) : (s32)v; }  // (-0 and +0 equal)
static inline bool fge(u32 a, u32 b) { return !fnan(a) && !fnan(b) && fkey(a) >= fkey(b); }  // fcmpo; cror eq,gt,eq
static inline bool fle(u32 a, u32 b) { return !fnan(a) && !fnan(b) && fkey(a) <= fkey(b); }  // fcmpo; cror eq,lt,eq
static u32 fop(Pc* c, u32 a, u32 b, int op) {  // fadds / fsubs / fmuls / fdivs on plain singles
    if (!plain(c, a) || !plain(c, b)) return 0;
    float x = fbits(a), y = fbits(b), r = op == 0 ? x + y : op == 1 ? x - y : op == 2 ? x * y : x / y;
    u32 v = bitsf(r);
    plain(c, v);
    return v;
}
#define FADD(a, b) fop(c, a, b, 0)
#define FSUB(a, b) fop(c, a, b, 1)
#define FMUL(a, b) fop(c, a, b, 2)
#define FDIV(a, b) fop(c, a, b, 3)
static inline u32 sda2(Pc* c, s32 off) { return f32(c->g->gpr[2] + (u32)off); }  // a constant at r2+off
static void pw8(Pc* c, u32 a, u32 v) {  // stb through the walk's undo list
    u32 w = rd32(c, a & ~3u), sh = 8 * (3 - (a & 3));
    wr32(c, a & ~3u, (w & ~(0xFFu << sh)) | ((v & 0xFF) << sh));
}
// J2DOrthoGraph::scissorBounds 0x802E987C (g: the graph, q: the pane's clip box): the box in screen units, clamped to
// the graph's scissor box.
static void ortho_scissor_bounds(Pc* c, u32 g, u32* box, u32 q) {
    u32 sx = FDIV(FSUB(f32(g + 12), f32(g + 4)), FSUB(f32(g + 196), f32(g + 188)));
    u32 sy = FDIV(FSUB(f32(g + 16), f32(g + 8)), FSUB(f32(g + 200), f32(g + 192)));
    u32 lo = sda2(c, -14496), x = f32(g + 4), y = f32(g + 8), ox = f32(g + 188), oy = f32(g + 192);
    if (!fge(x, lo)) x = lo;
    if (!fge(y, lo)) y = lo;
    box[2] = FADD(x, FMUL(sx, FSUB(f32(q + 8), ox)));
    box[1] = FADD(y, FMUL(sy, FSUB(f32(q + 4), oy)));
    box[3] = FADD(y, FMUL(sy, FSUB(f32(q + 12), oy)));
    box[0] = FADD(x, FMUL(sx, FSUB(f32(q + 0), ox)));
    if (fle(box[0], f32(g + 20))) box[0] = f32(g + 20);
    if (fle(box[1], f32(g + 24))) box[1] = f32(g + 24);
    if (fge(box[2], f32(g + 28))) box[2] = f32(g + 28);
    if (fge(box[3], f32(g + 32))) box[3] = f32(g + 32);
    c->n += 71;
}
static u32 cvt_fp2unsigned(Pc* c, u32 v) {  // __cvt_fp2unsigned 0x803620AC on a plain single
    plain(c, v);
    float f = fbits(v);
    c->n += 14;
    return f < 0.0f ? 0 : f >= 4294967296.0f ? 0xFFFFFFFFu : (u32)f;
}
static void gx_set_scissor(Pc* c, u32 left, u32 top, u32 w, u32 h) {  // GXSetScissor 0x803604D0
    u32 t = top + 342, l = left + 342;
    GX(248) = rlwimi(GX(248), t, 0, 21, 31);
    GX(248) = rlwimi(GX(248), l, 12, 9, 19);
    GX(252) = rlwimi(GX(252), t + (h - 1), 0, 21, 31);
    GX(252) = rlwimi(GX(252), l + (w - 1), 12, 9, 19);
    bp(c, GX(248));
    bp(c, GX(252));
    bp_sent_not(c, 0);
    c->n += 30;
}
// J2DGrafContext::setScissor 0x802E8E20 on the box s (the stack copy's scissor box; its own clamping of the copy's
// fields is left out: the copy is not read again).
static void graf_set_scissor(Pc* c, const u32* s) {
    u32 x0 = s[0], y0 = s[1], x1 = s[2], y1 = s[3], lo = sda2(c, -14520), hi = sda2(c, -14512);
    if (!(fge(x1, x0) && fge(y1, y0))) {
        u32 a = x0, b = y0, cc = x1, d = y1;
        if (fge(x0, x1)) x0 = x1;
        if (fge(y0, y1)) y0 = y1;
        if (fle(x1, a)) x1 = a;
        if (fle(y1, b)) y1 = b;
        if (fle(x1, cc)) x1 = cc;
        if (fle(y1, d)) y1 = d;
    }
    if (fle(x0, lo)) x0 = lo;
    if (fle(y0, lo)) y0 = lo;
    if (fge(x1, hi)) x1 = hi;
    if (fge(y1, hi)) y1 = hi;
    c->n += 60;
    if (!(fge(x1, x0) && fge(y1, y0))) { gx_set_scissor(c, 0, 0, 0, 0); return; }
    u32 left = cvt_fp2unsigned(c, x0), top = cvt_fp2unsigned(c, y0);
    if (left >= (1u << 24) || top >= (1u << 24) || !plain(c, x1) || !plain(c, y1)) { give_up(c, 2); return; }
    u32 fl = bitsf((float)left), ft = bitsf((float)top);  // (the magic-number conversion, exact below 2^24)
    u32 cx = bitsf((float)ceil((double)fbits(x1))), cy = bitsf((float)ceil((double)fbits(y1)));  // ceil 0x8036BEBC, frsp
    u32 h = cvt_fp2unsigned(c, FSUB(cy, ft)), w = cvt_fp2unsigned(c, FSUB(cx, fl));
    c->n += 2 * 60;
    gx_set_scissor(c, left, cvt_fp2unsigned(c, ft), w, h);
}
// Whether the subtree at p (depth d) is the walk's: J2DPane or J2DPictureEx drawing throughout; counts the panes.
#define WALK_DEPTH 24
static bool walk_scan(Pc* c, u32 p, u32 d, u32* panes) {
    if (d > WALK_DEPTH || !in_mem1(p, 344)) return false;
    u32 vt = f32(p);
    if (!in_mem1(vt, 92)) return false;
    u32 ds = f32(vt + 56), ra = f32(vt + 88);
    if ((ds != A_PANE_DRAWSELF && ds != A_PICEX_DRAWSELF) || (ra != A_PANE_REWRITEALPHA && ra != A_PICEX_REWRITEALPHA)) return false;
    if (++*panes > 512) return false;
    for (u32 l = f32(p + 220); l; l = f32(l + 12)) {
        if (!in_mem1(l, 16) || !walk_scan(c, f32(l), d + 1, panes)) return false;
    }
    return true;
}
// J2DPane::draw (p, x, y, graph, clip, visible) for a pane with a parent; x and y are only read without one.
static void walk_pane(Pc* c, u32 p, u32 g, bool clip, bool vis, u32 d) {
    if (c->fail || !span(c, p, 344)) return;
    c->n += 40;
    vis = vis && f8(p + 176);
    u32 node = f32(p + 236), par = node && in_mem1(node, 16) ? f32(node + 12) : 0;
    if (!par || !span(c, par, 240)) { give_up(c, 1); return; }
    if (!(fge(f32(p + 40), f32(p + 32)) && fge(f32(p + 44), f32(p + 36)))) return;  // no box: nothing drawn, no children
    u32 b0 = f32(p + 32), b1 = f32(p + 36), b2 = f32(p + 40), b3 = f32(p + 44), tx = f32(p + 212), ty = f32(p + 216);
    u32 gb[4] = { FADD(b0, tx), FADD(b1, ty), FADD(b2, tx), FADD(b3, ty) };
    if (vis) {
        u32 vt = f32(p);
        if (f32(vt + 88) == A_PICEX_REWRITEALPHA) {  // J2DPictureEx::rewriteAlpha: 178 = the material's alpha
            u32 mat = f32(p + 336);
            if (mat && span(c, mat, 20)) pw8(c, p + 178, f8(mat + 19));
            c->n += 6;
        }
    }
    u32 dx = FSUB(f32(par + 48), f32(par + 32)), dy = FSUB(f32(par + 52), f32(par + 36));
    gb[0] = FADD(gb[0], dx); gb[1] = FADD(gb[1], dy); gb[2] = FADD(gb[2], dx); gb[3] = FADD(gb[3], dy);
    for (u32 k = 0; k < 4; k++) wr32(c, p + 48 + 4 * k, gb[k]);
    u32 m[12];
    psmtx_concat(c, par + 128, p + 80, m);
    if (c->fail) return;
    for (u32 k = 0; k < 12; k++) wr32(c, p + 128 + 4 * k, m[k]);
    c->n += 60;
    if (vis) {
        u32 cl[4] = { b0, b1, b2, b3 };
        if (clip) {
            for (u32 k = 0; k < 4; k++) cl[k] = gb[k];
            u32 pc0 = f32(par + 64), pc1 = f32(par + 68), pc2 = f32(par + 72), pc3 = f32(par + 76);
            if (fle(cl[0], pc0)) cl[0] = pc0;
            if (fle(cl[1], pc1)) cl[1] = pc1;
            if (fge(cl[2], pc2)) cl[2] = pc2;
            if (fge(cl[3], pc3)) cl[3] = pc3;
        }
        for (u32 k = 0; k < 4; k++) wr32(c, p + 64 + 4 * k, cl[k]);
        u32 a = rd32(c, p + 176) >> 8 & 0xFF;  // 178 (rewriteAlpha may have just written it)
        pw8(c, p + 179, a);
        if (f8(p + 180)) pw8(c, p + 179, (u32)((s32)(a * f8(par + 179)) / 255));
        c->n += 30;
    }
    if (c->fail) return;
    if (vis) {
        u32 box[4], z = sda2(c, -14352);
        box[0] = box[1] = box[2] = box[3] = z;
        if (clip) ortho_scissor_bounds(c, g, box, p + 64);
        u32 c0 = rd32(c, p + 64), c1 = rd32(c, p + 68), c2 = rd32(c, p + 72), c3 = rd32(c, p + 76);
        if ((fge(c2, c0) && fge(c3, c1)) || !clip) {
            if (clip) graf_set_scissor(c, box);
            gx_set_cull_mode(c, f8(p + 177));
            if (f32(f32(p) + 56) == A_PICEX_DRAWSELF) {
                if ((u32)(c->pp - c->pbase) + 2048 > c->pcap) { give_up(c, 1); return; }
                picex_draw_self(c, p, g + 128);  // (the stack copy's matrix is the graph's at +128)
            }
            c->n += 60;
        }
    }
    for (u32 l = f32(p + 220); l && !c->fail; l = f32(l + 12)) walk_pane(c, f32(l), g, clip, vis, d + 1);
}
u64 g_walkPanes;
static void pane_walk(Pc* c, Gekko* g) {
    u32 p = g->gpr[3], gr = g->gpr[4], panes = 0;
    bool clip = g->gpr[5] & 0xFF, vis = g->gpr[6] & 0xFF;
    u32 node = in_mem1(p, 240) ? f32(p + 236) : 0;
    if (!node || !in_mem1(node, 16) || !f32(node + 12)) { give_up(c, 1); return; }  // a root: the guest code
    if (!walk_scan(c, p, 0, &panes)) { give_up(c, 5); return; }
    if (!span(c, gr, 204) || !span(c, f32(gr), 36)) return;
    u32 gt = f32(f32(gr) + 32);
    if (gt != A_GRAFTYPE_ORTHO && gt != A_GRAFTYPE_CONTEXT) { give_up(c, 5); return; }
    if (gt != A_GRAFTYPE_ORTHO) clip = false;
    if (!span(c, g->gpr[2] - 14520u, 180)) return;
    c->wt = true;
    walk_pane(c, p, gr, clip, vis, 0);
    if (!c->fail) g_walkPanes += panes;
}
static void walk_undo(Pc* c) {
    for (u32 i = c->nundo; i-- > 0;) ram_wr32(g_pm, c->undo[i][0] - 0x80000000u, c->undo[i][1]);
}

// ---- J3D: a shape's matrices (libs/JSystem/src/J3DGraphBase/J3DShapeMtx.cpp) ----
// J3DFifoLoadPosMtxImm 0x8030FAE0 / J3DFifoLoadNrmMtxImm 0x8030FB60 / ...Imm3x3 0x8030FBCC: the words as they are.
static void j3d_load_pos_mtx(Pc* c, const u32* m, u32 id) {
    p8(c, 0x10); p16(c, 11); p16(c, (id << 2) & 0xFFFC);
    for (u32 i = 0; i < 12; i++) p32(c, m[i]);
    c->n += 31;
}
static void j3d_load_nrm_mtx(Pc* c, const u32* m, u32 stride, u32 id) {  // stride 4: from a 3x4 matrix, 3: a 3x3
    p8(c, 0x10); p16(c, 8); p16(c, id * 3 + 1024);
    for (u32 r = 0; r < 3; r++) for (u32 k = 0; k < 3; k++) p32(c, m[r * stride + k]);
    c->n += 25;
}
// J3DPSCalcInverseTranspose 0x8031189C: the 3x3 part's inverse transpose (cofactors over the determinant, whose
// reciprocal is fres refined by two Newton steps), as its paired-single code computes it. false: not taken (a zero
// determinant leaves the guest's output untouched, so the guest code loads whatever was there).
static bool j3d_inverse_transpose(Pc* c, const u32* s, u32* d) {
    P f0 = { fbits(s[0]), 1.0f }, f1 = { fbits(s[1]), fbits(s[2]) }, f2 = { fbits(s[4]), 1.0f };
    P f3 = { fbits(s[5]), fbits(s[6]) }, f4 = { fbits(s[8]), 1.0f }, f5 = { fbits(s[9]), fbits(s[10]) };
    P f6 = PMERGE10(f1, f0), f7 = PMERGE10(f3, f2), f8 = PMERGE10(f5, f4);
    P f11 = PMUL(f3, f6), f13 = PMUL(f5, f7);
    f11 = PMSUB(f1, f7, f11);
    P f12 = PMUL(f1, f8);
    f13 = PMSUB(f3, f8, f13);
    f12 = PMSUB(f5, f6, f12);
    P f10 = PMUL(f3, f4), f9 = PMUL(f0, f5);
    f8 = PMUL(f1, f2);
    f10 = PMSUB(f2, f5, f10);
    f9 = PMSUB(f1, f4, f9);
    f8 = PMSUB(f0, f3, f8);
    f7 = PMUL(f0, f13);
    f1 = PSUB(f1, f1);
    f7 = PMADD(f2, f12, f7);
    f7 = PMADD(f4, f11, f7);
    if (!plain(c, bitsf(f7.p0)) || f7.p0 == f1.p0) { if (!c->fail) g_portFloatSite[3]++; give_up(c, 2); return false; }
    f0 = PS(1.0f / f7.p0);  // fres: the correctly rounded single reciprocal (gekko_softfp.c sf_recip_estimate)
    f6 = PADD(f0, f0); f5 = PMUL(f0, f0); f0 = PNMSUB(f7, f5, f6);
    f6 = PADD(f0, f0); f5 = PMUL(f0, f0); f0 = PNMSUB(f7, f5, f6);
    f13 = PMULS0(f13, f0); f12 = PMULS0(f12, f0); f11 = PMULS0(f11, f0);
    f10 = PMULS0(f10, f0); f9 = PMULS0(f9, f0); f8 = PMULS0(f8, f0);
    const float out[9] = { f13.p0, f13.p1, f10.p0, f12.p0, f12.p1, f9.p0, f11.p0, f11.p1, f8.p0 };
    for (u32 i = 0; i < 9; i++) d[i] = psq_copy(c, bitsf(out[i]));  // psq_st (w = 1 for the third column)
    c->n += 52;
    return true;
}
// J3DShapeMtxConcatView::loadMtxConcatView_PNGP 0x80313828 (this, slot, draw matrix index) with the loadNrmMtx of
// J3DShapeMtxConcatView 0x80313D28 or J3DShapeMtxMultiConcatView 0x8031419C: the view matrix times the draw matrix,
// loaded as the slot's position matrix and as a normal matrix.
static void j3d_load_mtx_concat_view_pngp(Pc* c, u32 self, u32 slot, u32 idx) {
    const u32 sys = A_J3DSYS, r13 = c->g->gpr[13];
    if (!span(c, sys, 284) || !span(c, r13 - 28628u, 24) || !span(c, self, 4)) return;
    if (f32(r13 - 28608u)) { give_up(c, 7); return; }  // (a differed texture matrix: J3DDifferedTexMtx::loadExecute)
    u32 vt = f32(self);
    if (!span(c, vt + 36, 4)) return;
    u32 nrm = f32(vt + 36);  // J3DShapeMtxConcatView's loadNrmMtx loads normal matrix 0, J3DShapeMtxMultiConcatView's the slot's
    if (nrm != A_J3DCONCATVIEW_LOADNRM && nrm != A_J3DMULTICONCATVIEW_LOADNRM) { give_up(c, 5); return; }
    u32 nid = nrm == A_J3DMULTICONCATVIEW_LOADNRM ? slot * 3 : 0;
    u32 m0 = f32(sys + 64);
    if (!span(c, m0 + 48, 4)) return;
    u32 m[12], n3[9];
    psmtx_concat(c, f32(m0 + 48), f32(sys + 260) + (idx & 0xFFFF) * 48, m);
    if (c->fail) return;
    j3d_load_pos_mtx(c, m, slot * 3);
    u32 aff = f32(r13 - 28628u);
    if (!span(c, aff + (idx & 0xFFFF), 1)) return;
    if (f32(r13 - 28620u) == 8192 || f8(r13 - 28624u)) { give_up(c, 7); return; }  // (to a texture matrix; scaled normals)
    if (f8(aff + (idx & 0xFFFF)) == 1) j3d_load_nrm_mtx(c, m, 4, nid);
    else if (j3d_inverse_transpose(c, m, n3)) j3d_load_nrm_mtx(c, n3, 3, nid);
    c->n += 90;
}

// ---- J3D lights: J3DColorBlockLightOn::diffLightObj 0x8031A8E0 (J3DLightObj::load 0x80323590 per light) ----
// Each light set appends four XF loads to __GDCurrentDL: position, attenuation (cos then distance), colour, direction
// (72 bytes, J3DGDSetLight* 0x8030D364-0x8030DB04). The guest code checks for room once per light and calls GDOverflowed
// when there is none: the port is not taken then.
// (each loads the cursor once: c->gd is bytes, so a store through it may alias c->gdN and byte by byte the compiler
// reloaded it after every byte, ~20 instructions a word, 2DS job 1350)
static inline void gd8(Pc* c, u32 v) { c->gd[c->gdN++] = (u8)v; }
static inline void gd16(Pc* c, u32 v) {
    u32 n = c->gdN;
    u8* p = c->gd + n;
    p[0] = (u8)(v >> 8); p[1] = (u8)v;
    c->gdN = n + 2;
}
static inline void gd32(Pc* c, u32 v) {
    u32 n = c->gdN;
    u8* p = c->gd + n;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
    c->gdN = n + 4;
}
static void gd_xf(Pc* c, u32 addr, u32 n, u32 src) {  // J3DGDWriteXFCmdHdr and n words (the floats' bits as stored)
    u32 k = c->gdN;
    u8* p = c->gd + k;
    p[0] = 0x10; p[1] = (u8)((n - 1) >> 8); p[2] = (u8)(n - 1); p[3] = (u8)(addr >> 8); p[4] = (u8)addr;
    p += 5;
    for (u32 i = 0; i < n; i++, p += 4) {
        u32 v = f32(src + 4 * i);
        p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
    }
    c->gdN = k + 5 + 4 * n;
}
// J3DLightObj::load 0x80323590 for each light of the block (n of them): the GD bytes after c->gdN.
static void light_bytes(Pc* c, u32 blk, u32 n) {
    for (u32 i = 0; i < n; i++) {
        u32 l = f32(blk + 0x20 + 4 * i), o = 16 * i;
        c->n += 6;
        if (!l) continue;
        if (!span(c, l, 0x34)) return;
        gd_xf(c, 0x060A + o, 3, l);         // position
        gd_xf(c, 0x0604 + o, 6, l + 0x1C);  // cos attenuation, distance attenuation
        gd8(c, 0x10); gd16(c, 0); gd16(c, 0x0603 + o); gd32(c, f32(l + 0x18));  // colour
        gd_xf(c, 0x060D + o, 3, l + 0x0C);  // direction
        c->n += 160;
    }
}
static void j3d_diff_light_obj(Pc* c, u32 blk, u32 n) {
    if (n > 8 || !span(c, blk + 0x20, 32) || !span(c, A_GDCURRENTDL, 4)) { give_up(c, 1); return; }
    u32 dl = f32(A_GDCURRENTDL), k = 0;
    if (!span(c, dl, 16)) return;
    for (u32 i = 0; i < n; i++) k += f32(blk + 0x20 + 4 * i) != 0;
    u32 ptr = f32(dl + 8), top = f32(dl + 12);
    if (ptr + 72 * k > top || !span(c, ptr, 72 * k)) { give_up(c, 1); return; }
    c->gdA = ptr;
    light_bytes(c, blk, n);
    wr32(c, dl + 8, ptr + c->gdN);
    c->n += 12;
}

// ---- J3D: J3DMaterial::diff 0x803167D8 (this, diff flags): the material's blocks append their changed state to the
// packet's display list between J3DMatPacket::beginDiff and endDiff (J3DDisplayListObj::beginDL / endDL) ----
// Census (jobs 1324-1327): every material in TP's heavy area and Ordon has J3DColorBlockLightOn, J3DTexGenBlockPatched,
// J3DTevBlock2 or 4, J3DIndBlockFull and J3DPEBlockFull; the flags used are ambient colour, channels, lights, texture
// matrices, texgens, texture numbers, gen mode, TEV registers and fog, all ported; any other class, flag or vtable entry
// gives up and the guest code runs (PortStats.mdWhy).
enum {
    A_MD_COLORDIFF = 0x8031A13C, A_MD_AMB = 0x8031A1DC, A_MD_MAT = 0x8031A358, A_MD_CHAN = 0x8031A4D4,
    A_MD_TEXGENDIFF = 0x8031AC68, A_MD_TEXMTXDIFF = 0x8031ACD0, A_MD_TEXGENGEN = 0x8031AD30, A_MD_TEXGENNUM = 0x80322D48,
    A_MD_TEVDIFF = 0x8031CD44, A_MD_TEV2REG = 0x8031D434, A_MD_TEV4REG = 0x8031D7BC,
    A_MD_TEV2STAGES = 0x8032258C, A_MD_TEV4STAGES = 0x803221C8, A_MD_TEV2TEXNO = 0x8031D3D0, A_MD_TEV4TEXNO = 0x8031D758,
    A_MD_INDDIFF = 0x8031E328, A_MD_INDSTAGES = 0x803233A4, A_MD_PEDIFF = 0x8031FCA0, A_MD_FOG = 0x8031F940,
    A_MD_GDLOBJ = 0x80434C70,    // (beginDL's GDLObj, __GDCurrentDL while the diff runs)
    A_MD_FLUSH_SC = 0x8033B5D4,  // DCFlushRange's sc (its handler runs after the port, md_flush)
};
// Why J3DMaterial::diff was not taken: texture numbers, texgens, PE blend, TEV stages / indirect / texcoord scale, a block
// class or vtable entry not ported, the display list's room, floats, other.
enum { MDW_TEXNO, MDW_TEXGEN, MDW_BLEND, MDW_STAGES, MDW_CLASS, MDW_ROOM, MDW_FLOAT, MDW_OTHER };
static inline void md_why(Pc* c, u32 why) {
    if (!c->fail) g_portStats.mdWhy[why]++;
    give_up(c, why == MDW_ROOM ? 1 : why == MDW_FLOAT ? 2 : 5);
}
static inline u32 rlwinm(u32 v, u32 sh, u32 mb, u32 me) { return rlwimi(0, v, sh, mb, me); }
static inline u32 vfn(u32 obj, u32 off) {  // a virtual function's address (0: not readable)
    u32 vt = in_mem1(obj, 4) ? f32(obj) : 0;
    return vt && in_mem1(vt + off, 4) ? f32(vt + off) : 0;
}
// GDOverflowCheck(size) at the current write pointer: the guest calls GDOverflowed when it fails; the port gives up.
static inline bool gd_room(Pc* c, u32 size) {
    if (c->gdN + size > c->gdRoom || c->gdN + size > sizeof c->gd) { md_why(c, MDW_ROOM); return false; }
    return true;
}
static inline void bp_word(Pc* c, u32 w) {  // J3DGDWriteBPCmd
    u32 n = c->gdN;
    u8* p = c->gd + n;
    p[0] = 0x61; p[1] = (u8)(w >> 24); p[2] = (u8)(w >> 16); p[3] = (u8)(w >> 8); p[4] = (u8)w;
    c->gdN = n + 5;
}
// J3DGDSetTevColorS10 0x8030F3FC (reg 1-3, the colour's s16 fields at a): RA once, BG three times.
static void md_tev_color_s10(Pc* c, u32 id, u32 a) {
    u32 r = (u32)(s32)(s16)f16(a), g = (u32)(s32)(s16)f16(a + 2), b = (u32)(s32)(s16)f16(a + 4), al = (u32)(s32)(s16)f16(a + 6);
    u32 ra = rlwimi(rlwimi(rlwinm(al, 12, 9, 19), r, 0, 21, 31), id * 2 + 224, 24, 0, 7);
    u32 bg = rlwimi(rlwimi(rlwinm(g, 12, 9, 19), b, 0, 21, 31), id * 2 + 225, 24, 0, 7);
    if (!gd_room(c, 20)) return;
    bp_word(c, ra); bp_word(c, bg); bp_word(c, bg); bp_word(c, bg);
    c->n += 150;
}
// J3DGDSetTevKColor 0x8030F294 (konst colour 0-3, the GXColor's bytes at a).
static void md_tev_kcolor(Pc* c, u32 id, u32 a) {
    u32 w1 = (rlwimi(f8(a), f8(a + 3), 12, 12, 19) | 0x800000u) | (id * 2 + 224) << 24;
    u32 w2 = (rlwimi(f8(a + 2), f8(a + 1), 12, 12, 19) | 0x800000u) | (id * 2 + 225) << 24;
    if (!gd_room(c, 10)) return;
    bp_word(c, w1); bp_word(c, w2);
    c->n += 90;
}
// J3DTevBlock2::diffTevReg 0x8031D434 / J3DTevBlock4::diffTevReg 0x8031D7BC: colours 1-3 and the four konst colours.
static void md_tev_reg(Pc* c, u32 tev, u32 colors, u32 kcolors) {
    if (!span(c, tev + colors, 24) || !span(c, tev + kcolors, 16)) return;
    for (u32 i = 0; i < 3 && !c->fail; i++) md_tev_color_s10(c, i + 1, tev + colors + 8 * i);
    for (u32 i = 0; i < 4 && !c->fail; i++) md_tev_kcolor(c, i, tev + kcolors + 4 * i);
    c->n += 40;
}
// A constant the guest loads from its small-data area (r2 + off): the expected bits, else not taken.
static bool sda_is(Pc* c, s32 off, u32 bits) {
    if (f32(c->g->gpr[2] + (u32)off) == bits) return true;
    md_why(c, MDW_OTHER);
    return false;
}
static bool sda_is_d(Pc* c, s32 off, u32 hi, u32 lo) { return sda_is(c, off, hi) && sda_is(c, off + 4, lo); }
// J3DGDSetFog 0x8030F630 then J3DGDSetFogRangeAdj 0x8030F994, as J3DPEBlockFull::diffFog 0x8031F940 calls them (fog at pe + 4).
static void md_fog(Pc* c, u32 pe) {
    if (!span(c, pe + 4, 44) || !gd_room(c, 55)) return;
    if (!sda_is(c, -14028, 0) || !sda_is(c, -14024, 0x3F000000u) || !sda_is_d(c, -14016, 0x3FF00000u, 0) ||
        !sda_is_d(c, -14000, 0x3FE00000u, 0) || !sda_is(c, -14008, 0x40000000u) ||
        !sda_is_d(c, -13984, 0x43300000u, 0x80000000u) || !sda_is(c, -13992, 0x4B00001Eu))
        return;
    u32 type = f8(pe + 4), sz = f32(pe + 8), ez = f32(pe + 12), nz = f32(pe + 16), fz = f32(pe + 20);
    if (!plain(c, sz) || !plain(c, ez) || !plain(c, nz) || !plain(c, fz)) { g_portStats.mdWhy[MDW_FLOAT]++; return; }
    u32 A, B, C;
    if (fbits(fz) == fbits(nz) || fbits(ez) == fbits(sz)) { A = 0; B = 0x3F000000u; C = 0; }  // (fcmpu: -0 == +0)
    else {
        u32 fn = FMUL(fz, nz), d = FSUB(fz, nz), e = FSUB(ez, sz);
        A = FDIV(fn, FMUL(d, e));
        B = FDIV(fz, d);
        C = FDIV(sz, e);
    }
    if (c->fail) { g_portStats.mdWhy[MDW_FLOAT]++; return; }
    u32 expn = 1;  // (B_mant > 1.0 and 0.0f < B_mant < 0.5: compares against doubles, exact for a single)
    for (u32 k = 0; fbits(B) > 1.0f && k < 300 && !c->fail; k++) { B = FMUL(B, 0x3F000000u); expn++; }
    for (u32 k = 0; fbits(B) > 0.0f && fbits(B) < 0.5f && k < 300 && !c->fail; k++) { B = FMUL(B, 0x40000000u); expn--; }
    u32 sh = expn & 63, p = sh < 32 ? 1u << sh : 0;
    if (!p || c->fail) { md_why(c, MDW_FLOAT); return; }
    u32 Af = FDIV(A, bitsf((float)(s32)p));  // (the int-to-double trick, then fsubs: exact for a power of two)
    u32 bm = cvt_fp2unsigned(c, FMUL(0x4B00001Eu, B));
    if (c->fail) { g_portStats.mdWhy[MDW_FLOAT]++; return; }
    bp_word(c, (Af >> 12) | 0xEE000000u);
    bp_word(c, bm | 0xEF000000u);
    bp_word(c, expn | 0xF0000000u);
    bp_word(c, rlwimi(type << 21, C, 20, 12, 31) | 0xF1000000u);
    u32 col = pe + 24;
    bp_word(c, rlwimi(rlwimi(f8(col + 2), f8(col + 1), 8, 16, 23), f8(col), 16, 8, 15) | 0xF2000000u);
    u32 en = f8(pe + 5), tbl = pe + 28;
    if (en)
        for (u32 k = 0; k < 5; k++) bp_word(c, (k + 233) << 24 | f16(tbl + 4 * k + 2) << 12 | f16(tbl + 4 * k));
    bp_word(c, ((f16(pe + 6) + 342) | 0xE8000000u) | rlwinm(en, 10, 14, 21));
    c->n += 260;
}
// J3DColorBlockLightOn::diffAmbColor / diffMatColor: XF 0x100A / 0x100C, two colours (at blk + off), room from the SDA.
static void md_two_colors(Pc* c, u32 blk, u32 off, u32 xf, s32 sizeAt) {
    if (!gd_room(c, f32(c->g->gpr[13] + (u32)sizeAt))) return;
    gd8(c, 0x10); gd16(c, 1); gd16(c, xf); gd32(c, f32(blk + off)); gd32(c, f32(blk + off + 4));
    c->n += 95;
}
// J3DColorBlockLightOn::diffColorChan 0x8031A4D4: XF 0x100E, the four channel controls (in the order 0, 2, 1, 3).
static void md_color_chan(Pc* c, u32 blk) {
    if (!gd_room(c, f32(c->g->gpr[13] - 31768u))) return;
    u32 tbl = c->g->gpr[2] - 13912u;
    if (!span(c, tbl, 4)) return;
    gd8(c, 0x10); gd16(c, 3); gd16(c, 0x100E);
    static const u32 kAt[4] = { 22, 26, 24, 28 };
    for (u32 q = 0; q < 4; q++) {  // J3DColorChan::load's word, instruction by instruction
        u32 v = f16(blk + kAt[q]);
        u32 r11 = f8(tbl + rlwinm(v, 23, 30, 31)), r10 = rlwinm(v, 25, 30, 31);
        u32 r3 = rlwimi(rlwinm(v, 25, 24, 27), v, 30, 28, 31), r9 = r3 & 0xFF;
        u32 r6 = rlwinm(r3, 7, 17, 20), r5 = rlwinm(r11 != 0, 10, 14, 21), r4 = rlwinm(2 - r11 != 0, 9, 15, 22);
        u32 r8 = rlwimi(rlwimi(rlwimi(v & 1, v, 0, 30, 30), r9, 2, 26, 29), v, 0, 25, 25);
        if (!r11) r10 = 0;
        gd32(c, r6 | r4 | r5 | r8 | r10 << 7);
    }
    c->n += 210;
}
// J3DGDSetTexCoordGen 0x8030E064 (texgen function, source): the XF word, as its two switches leave it (a function out of
// range keeps the source's value as the type, the register the code reuses).
static u32 md_texcoord_gen(u32 func, u32 src) {
    u32 form = 0, proj = 0, row = 5, eRow = 5, eLit = 0, type = src;
    if (src <= 3) { static const u8 kRow[4] = { 0, 1, 3, 4 }; row = kRow[src]; form = 1; }
    else if (src <= 11) row = src + 1;           // GX_TG_TEX0-7: rows 5-12
    else if (src <= 18) eRow = src - 12;         // GX_TG_TEXCOORD0-6
    else if (src <= 20) row = 2;                 // GX_TG_COLOR0 / COLOR1
    if (func == 1) type = 0;                     // GX_TG_MTX2x4
    else if (func == 0) { type = 0; proj = 1; }  // GX_TG_MTX3x4
    else if (func >= 2 && func <= 9) { type = 1; eLit = func - 2; }  // GX_TG_BUMP0-7
    else if (func == 10) type = src == 19 ? 2 : 3;                   // GX_TG_SRTG
    else if ((s32)func < 0 || func > 10) type = src;
    return eLit << 15 | eRow << 12 | row << 7 | type << 4 | form << 2 | proj << 1;
}
// J3DTexGenBlockPatched::diffTexGen 0x8031AD30 with loadTexCoordGens 0x80323644 (count and J3DTexCoords of 6 bytes at tg + 4).
static void md_texgens(Pc* c, u32 tg, bool post) {
    u32 n = f32(tg + 4);
    c->n += 8;
    if (!n) return;
    if (n > 8 || !span(c, tg + 8, 6 * n)) { md_why(c, MDW_OTHER); return; }
    if (!gd_room(c, n * 8 + 10)) return;
    u32 cnt = ((n & 0xFF) - 1) & 0xFFFF;
    gd8(c, 0x10); gd16(c, cnt); gd16(c, 0x1040);
    for (u32 i = 0; i < n; i++) gd32(c, md_texcoord_gen(f8(tg + 8 + 6 * i), f8(tg + 9 + 6 * i)));
    gd8(c, 0x10); gd16(c, cnt); gd16(c, 0x1050);
    for (u32 i = 0; i < n; i++) gd32(c, post ? (f8(tg + 10 + 6 * i) == 60 ? 61 : i * 3) : 61);
    c->n += 40 + 45 * n;
}
// fctiwz: toward zero, saturating (a plain single here).
static u32 fctiwz(u32 v) {
    float f = fbits(v);
    return f >= 2147483648.0f ? 0x7FFFFFFFu : f <= -2147483649.0f ? 0x80000000u : (u32)(s32)f;
}
// (float)(s32)v as the guest makes it: the int-to-double trick with sda2(-13896) = 0x4330000080000000, then fsubs (exact
// for the small integers here).
static u32 int_to_single(s32 v) { return bitsf((float)v); }
// loadTexNo 0x80323F94 (texture map i, ResTIMG index): the texcoord scale table, then J3DGDSetTexImgPtr / ImgAttr /
// LookupMode and, for an index texture, J3DGDLoadTlut and J3DGDSetTexTlut.
static void md_texno(Pc* c, u32 i, u32 no) {
    u32 sda = c->g->gpr[13], tex = f32(A_J3DSYS + 88);
    if (!span(c, tex, 8) || !span(c, f32(tex + 4) + (no << 5), 32) || !span(c, sda - 31860u, 48)) return;
    u32 t = f32(tex + 4) + (no << 5);
    u32 w = f16(t + 2), h = f16(t + 4);
    wr32(c, 0x80434C2Cu + 8 * i, w << 16 | h);  // J3DSys::sTexCoordScaleTable[i] (two halfword stores, one word)
    if (!gd_room(c, 20)) return;
    if (!sda_is_d(c, -13896, 0x43300000u, 0x80000000u)) return;
    u32 img = t + f32(t + 28);
    bp_word(c, ((img - 0x80000000u) >> 5) | f8(sda - 31836u + i) << 24);                                           // ImgPtr
    bp_word(c, f8(sda - 31844u + i) << 24 | (f8(t) & 15) << 20 | (((h - 1) << 10) | (w - 1)));                        // ImgAttr
    u32 k8 = sda2(c, -13904), k01 = sda2(c, -13900), kb = sda2(c, -14040), k16 = sda2(c, -14036);
    u32 minL = FMUL(k8, int_to_single((s8)f8(t + 22))), maxL = FMUL(k8, int_to_single((s8)f8(t + 23)));
    u32 bias = FMUL(k01, int_to_single((s16)f16(t + 26)));
    u32 bi = fctiwz(FMUL(kb, bias)), mi = fctiwz(FMUL(k16, minL)), ma = fctiwz(FMUL(k16, maxL));
    if (c->fail) { g_portStats.mdWhy[MDW_FLOAT]++; return; }
    u32 wrapS = f8(t + 6), wrapT = f8(t + 7), minF = f8(t + 20), magF = f8(t + 21), clamp = f8(t + 18), edge = f8(t + 17);
    u32 aniso = f8(t + 19);
    u32 clzEdge = edge ? (u32)__builtin_clz(edge) : 32, d = 1 - magF, clzMag = d ? (u32)__builtin_clz(d) : 32;
    u32 m0 = wrapS | wrapT << 2 | rlwinm(clzMag, 31, 20, 27) | f8(sda - 31820u + minF) << 5 | rlwinm(clzEdge, 3, 16, 23) |
             rlwinm(bi, 9, 15, 22) | aniso << 19 | rlwinm(clamp, 21, 3, 10) | f8(sda - 31860u + i) << 24;
    bp_word(c, m0);                                                                                                 // LookupMode
    bp_word(c, rlwimi(rlwimi(rlwinm(ma, 8, 16, 23), mi, 0, 24, 31), f8(sda - 31852u + i), 24, 0, 7));
    c->n += 160;
    if (f8(t + 8) != 1) return;
    u32 size = f16(t + 10) > 16 ? 16 : 1, tmem = (i << 13) + 0xF0000u, pal = t + f32(t + 12);
    if (!gd_room(c, 20)) return;
    bp_word(c, 0xFEFFFF00u); bp_word(c, 0x0F000000u);                                                             // LoadTlut
    bp_word(c, ((pal - 0x80000000u) >> 5) | 0x64000000u);
    bp_word(c, (((tmem - 0x80000u) >> 9) | size << 10) | 0x65000000u);
    bp_word(c, 0xFEFFFF00u); bp_word(c, 0x0F000000u);
    bp_word(c, f8(sda - 31828u + i) << 24 | (((tmem - 0x80000u) >> 9) | f8(t + 9) << 10));                         // TexTlut
    c->n += 120;
}
static void j3d_material_diff(Pc* c, u32 m, u32 flags) {
    Gekko* g = c->g;
    if (!span(c, A_J3DSYS, 64) || !span(c, m + 36, 20)) return;
    u32 pk = f32(A_J3DSYS + 60);
    if (!span(c, pk + 40, 4) || !span(c, f32(pk + 40) + 32, 4)) return;
    u32 dlo = f32(f32(pk + 40) + 32);
    c->n += 30;
    if (!dlo) return;  // (isEnabled_Diff false: nothing happens)
    u32 col = f32(m + 36), tg = f32(m + 40), tev = f32(m + 44), ind = f32(m + 48), pe = f32(m + 52);
    if (!span(c, dlo, 16) || !span(c, col, 48) || !span(c, tg, 92) || !span(c, tev, 120) || !span(c, ind, 8) || !span(c, pe, 48))
        return;
    u32 tevReg = vfn(tev, 24), tevStages = vfn(tev, 152);
    u32 tevTexNo = vfn(tev, 20);
    bool tev2 = tevReg == A_MD_TEV2REG && tevStages == A_MD_TEV2STAGES && tevTexNo == A_MD_TEV2TEXNO;
    bool tev4 = tevReg == A_MD_TEV4REG && tevStages == A_MD_TEV4STAGES && tevTexNo == A_MD_TEV4TEXNO;
    if (vfn(col, 28) != A_MD_COLORDIFF || vfn(col, 32) != A_MD_AMB || vfn(col, 36) != A_MD_MAT || vfn(col, 40) != A_MD_CHAN ||
        vfn(col, 44) != A_J3D_DIFFLIGHTOBJ || vfn(tg, 36) != A_MD_TEXGENDIFF || vfn(tg, 40) != A_MD_TEXMTXDIFF ||
        vfn(tg, 44) != A_MD_TEXGENGEN || vfn(tg, 64) != A_MD_TEXGENNUM || vfn(tev, 16) != A_MD_TEVDIFF || (!tev2 && !tev4) || vfn(ind, 12) != A_MD_INDDIFF ||
        vfn(ind, 32) != A_MD_INDSTAGES || vfn(pe, 20) != A_MD_PEDIFF || vfn(pe, 24) != A_MD_FOG) {
        md_why(c, MDW_CLASS);
        return;
    }
    if (flags & 0x20000000u) { md_why(c, MDW_BLEND); return; }
    if ((flags & 0x0C000000u) || ((flags >> 20) & 15)) { md_why(c, MDW_STAGES); return; }
    // beginDL 0x80312644: the two buffers swapped, interrupts off (their state kept at r13 - 28640), GDInitGDLObj
    u32 buf = f32(dlo + 4), len = f32(dlo + 12), sda = g->gpr[13];
    if (!in_mem1(buf, len ? len : 1) || !span(c, sda - 28640u, 4) || sda - 27648u != A_GDCURRENTDL || !span(c, A_MD_GDLOBJ, 16)) {
        md_why(c, MDW_OTHER);
        return;
    }
    wr32(c, dlo, buf);
    wr32(c, dlo + 4, f32(dlo));
    wr32(c, sda - 28640u, (g->msr >> 15) & 1);
    c->gdA = buf;
    c->gdN = 0;
    c->gdRoom = len;
    // J3DTevBlock::diff 0x8031CD44: texture numbers (diffTexNo: each map's index at tev + 8 unless 0xFFFF), then TEV registers
    // (texcoord scale and stages gave up above)
    if ((flags >> 16) & 15)
        for (u32 i = 0; i < (tev2 ? 2u : 4u) && !c->fail; i++) {
            u32 no = f16(tev + 8 + 2 * i);
            c->n += 7;
            if (no != 0xFFFF) md_texno(c, i, no);
        }
    if ((flags & 0x01000000u) && !c->fail) md_tev_reg(c, tev, tev2 ? 20 : 66, tev2 ? 69 : 98);
    // J3DIndBlockFull::diff: only with the indirect TEV stage flag (gave up above). J3DPEBlockFull::diff: fog.
    if ((flags & 0x10000000u) && !c->fail) md_fog(c, pe);
    if ((flags & 0x02000000u) && !c->fail) {  // J3DGDSetGenMode_3Param 0x8030D210, then GX_NUMTEXGENS through XF 0x103F
        u32 ntg = f32(tg + 4) & 0xFF, ntev = f8(tev + (tev2 ? 52 : 32)), nind = f8(ind + 4);  // (the getters: a u32 at +4, bytes)
        if (gd_room(c, 10)) {
            bp_word(c, 0xFE073C0Fu);
            bp_word(c, rlwinm(nind, 16, 8, 15) | rlwimi((ntev - 1) << 10, ntg, 0, 24, 31));
            gd8(c, 0x10); gd16(c, 0); gd16(c, 0x103F); gd32(c, ntg);
            c->n += 120;
        }
    }
    bool post = f32(A_J3DSYS + 52) & 0x40000000u;  // (J3DSys flag 0x40000000: post-texture matrices)
    if (((flags >> 8) & 15) && !c->fail) {  // J3DTexGenBlockPatched::diffTexMtx 0x8031ACD0: J3DTexMtx::load per matrix
        for (u32 i = 0; i < 8 && !c->fail; i++) {
            u32 tm = f32(tg + 56 + 4 * i);
            c->n += 8;
            if (!tm) continue;
            if (!span(c, tm, 148) || !gd_room(c, 53)) return;
            u32 mtx = tm + 100, rows = post || f8(tm) != 1 ? 3 : 2;  // (loadPostTexMtx: always 3x4; loadTexMtx: GX_MTX2x4 is 1)
            for (u32 k = 0; k < 4 * rows; k++)
                if (!plain(c, f32(mtx + 4 * k))) { g_portStats.mdWhy[MDW_FLOAT]++; return; }  // (through lfs / stfs)
            u32 addr = post ? (i * 3) * 4 + 1280 : ((i * 3 + 30) << 2) & 0xFFFC;
            gd8(c, 0x10); gd16(c, 4 * rows - 1); gd16(c, addr);
            for (u32 k = 0; k < 4 * rows; k++) gd32(c, f32(mtx + 4 * k));
            c->n += 30 + 25 * rows;
        }
        if ((flags & 0x1000) && !c->fail) md_texgens(c, tg, post);  // (then diffTexGen)
    }
    if (!c->fail) {  // J3DColorBlockLightOn::diff 0x8031A13C
        if (flags & 4) md_two_colors(c, col, 12, 0x100A, -31772);
        if ((flags & 1) && !c->fail) md_two_colors(c, col, 4, 0x100C, -31776);
        if ((flags & 2) && !c->fail) md_color_chan(c, col);
        u32 nl = (flags >> 4) & 15, k = 0;
        if (nl && !c->fail) {
            if (nl > 8) { md_why(c, MDW_OTHER); return; }
            for (u32 i = 0; i < nl; i++) k += f32(col + 0x20 + 4 * i) != 0;
            if (gd_room(c, 72 * k)) light_bytes(c, col, nl);
        }
    }
    if (c->fail) return;
    // endDL 0x80312698: GDPadCurr32, interrupts as they were, the length, GDFlushCurrToMem (DCFlushRange over the whole
    // buffer: dcbf per line, then sc), __GDCurrentDL cleared
    while ((buf + c->gdN) & 31) gd8(c, 0);
    if (c->gdN > c->gdRoom || !span(c, buf, c->gdN ? c->gdN : 1)) { md_why(c, MDW_ROOM); return; }
    wr32(c, A_MD_GDLOBJ, buf);
    wr32(c, A_MD_GDLOBJ + 4, len);
    wr32(c, A_MD_GDLOBJ + 8, buf + c->gdN);
    wr32(c, A_MD_GDLOBJ + 12, buf + len);
    wr32(c, dlo + 8, c->gdN);
    wr32(c, sda - 27648u, 0);
    c->flushA = buf;
    c->flushLen = len;
    c->n += 60 + (len ? ((buf & 31) + len + 31) / 32 * 3 + 12 : 0);
}
// After a taken J3DMaterial::diff: DCFlushRange's dcbf per line (page stamps, as the interpreter's cache op does) and its
// sc with the OS's handler, run by the interpreter as the JIT does (jit_sc_handler), so SRR0 / SRR1, HID0 and the
// handler's registers end as the guest code leaves them.
static void md_flush(Pc* c, Gekko* g) {
    u32 a = c->flushA, len = c->flushLen;
    if (!len) return;
    // (one cache op per 4 KB page, not per line: the bus stamps the line's page, so the pages end stamped alike; a call
    // per line was most of the commit's 2,400 instructions, 2DS job 1348)
    u32 first = (a & ~31u) - 0x80000000u, last = ((a & ~31u) + ((((a & 31) + len + 31) >> 5) - 1) * 32) - 0x80000000u;
    for (u32 pg = first; g->bus->cache_op; pg = (pg & ~0xFFFu) + 0x1000) {
        g->bus->cache_op(g->bus->ctx, CACHE_DCBF, pg);
        if ((pg & ~0xFFFu) >= (last & ~0xFFFu)) break;
    }
    bool stop;
    u32 npc = gekko_interp_one(g, A_MD_FLUSH_SC, 0x44000002u, &stop);
    for (u32 k = 0; k < 32 && npc != A_MD_FLUSH_SC + 4; k++) {
        u32 pa, w = 0;
        if (gk_translate(g, npc, false, false, &pa, false) && pa + 4 <= g_psz) w = ram_rd32(g_pm, pa);
        npc = gekko_interp_one(g, npc, w, &stop);
    }
    if (npc != A_MD_FLUSH_SC + 4) g_portStats.mdWhy[MDW_OTHER] += 1000000;  // (cannot happen with the OS's handler: shows loudly)
}

// ---- entry ----
static Pc g_pc;

// Runs the hooked function's port into a fresh context (nothing written to the guest). false: not taken (c->fail).
static bool port_run(Pc* c, Gekko* g, u32 hook) {
    c->g = g;
    c->pn = c->nlog = c->n = c->dirty = c->gdN = c->nundo = c->flushLen = 0;
    c->wt = false;
    c->pbase = c->pipe;
    c->pcap = PIPE_CAP;
    if (hook == PORT_PANEWALK) {  // the walk's buffers: linear memory on the 3DS (the heap is nearly full in gameplay)
        static u8* walkPipe;
        static u32 (*walkUndo)[3];
        if (!walkPipe) walkPipe = walk_alloc(WALK_PIPE);
        if (!walkUndo) walkUndo = walk_alloc(WALK_UNDO * sizeof *walkUndo);
        if (!walkPipe || !walkUndo) return false;
        c->pbase = walkPipe;
        c->pcap = WALK_PIPE;
        c->undo = walkUndo;
        c->undoCap = WALK_UNDO;
    }
    c->pp = c->pbase;
    c->bpsn = ~0u;
    c->fail = 0;
    c->leak = false;
    if (!in_mem1(A_GXDATA_PTR, 4)) return false;
    c->gxd = ram_rd32(g_pm, A_GXDATA_PTR - 0x80000000u);
    if (c->gxd != ram_rd32(g_pm, g->gpr[2] - 13440u - 0x80000000u) || !in_mem1(c->gxd, GXD_BYTES) || (c->gxd & 3)) return false;
    for (u32 k = 0; k < c->nt; k++) c->have[c->touched[k]] = 0;
    c->nt = 0;
    tmark(0);
    // (Floating point off: the FPRs are another thread's until the OS takes the FP-unavailable exception, which the
    // guest code raises at its first float instruction: the float ports are not taken then.)
    if (hook != PORT_MATERIAL && hook != PORT_FLUSHPRIM && !(g->msr & MSR_FP)) { if (!c->fail) g_portFloatSite[4]++; give_up(c, 2); return false; }
    if (hook == PORT_MATDIFF) j3d_material_diff(c, g->gpr[3], g->gpr[4]);
    else if (hook == PORT_PANEWALK) pane_walk(c, g);
    else if (hook == PORT_FLUSHPRIM) gx_send_flush_prim(c);
    else if (hook == PORT_LIGHTS) j3d_diff_light_obj(c, g->gpr[3], g->gpr[4]);
    else if (hook == PORT_J3DPNGP) j3d_load_mtx_concat_view_pngp(c, g->gpr[3], g->gpr[4], g->gpr[5]);
    else if (hook == PORT_PICTUREEX) picex_draw_self(c, g->gpr[3], g->gpr[4]);
    else material_set_gx(c, g->gpr[3]);
    tmark(5);
    gx_sync(c);
    c->pn = (u32)(c->pp - c->pbase);
    c->wt = false;
    if (c->fail && c->nundo) walk_undo(c);  // (a walk that gave up leaves memory as it found it)
    return !c->fail;
}
static void port_commit(Pc* c) {
    for (u32 k = 0; k < c->nt; k++) {
        u32 i = c->touched[k];
        if (c->gx[i] != c->gx0[i]) ram_wr32(g_pm, c->gxd - 0x80000000u + 4 * i, c->gx[i]);
    }
    for (u32 i = 0; i < c->nlog; i++) ram_wr32(g_pm, c->log[i][0] - 0x80000000u, c->log[i][1]);
    // (one copy: byte by byte it took 8.7 us a call, job 553, as long as building the bytes)
    if (c->gdN) ram_from_be(g_pm, c->gdA - 0x80000000u, c->gd, c->gdN);
    if (c->pn && g_ppipe) g_ppipe(g_pctx, c->pbase, c->pn);  // (the walk's stores are in memory already)
}

// The registers the guest code would leave other than the ABI promises (PSMTXConcat's leaked lanes, Pc.leak).
static void port_regs(Pc* c, Gekko* g, u32 hook) {
    if (!c->leak) return;
    g->fpr[14].ps1 = c->l14;
    g->fpr[15].ps1 = c->l15;
    if (hook != PORT_PICTUREEX && hook != PORT_PANEWALK) g->fpr[31].ps1 = c->l31;  // (drawTexCoord and J2DPane::draw save f31 whole)
}

const char* const g_portNames[PORT_N] = { "J2DMaterial::setGX", "J2DPictureEx::drawSelf",
                                          "J3DShapeMtxConcatView::loadMtxConcatView_PNGP", "__GXSendFlushPrim",
                                          "J3DColorBlockLightOn::diffLightObj", "J2DPane::draw (walk)",
                                          "J3DMaterial::diff" };
static inline int hook_of(u32 pc) {
    return pc == A_MATERIAL_SETGX ? PORT_MATERIAL : pc == A_PICEX_DRAWSELF ? PORT_PICTUREEX : pc == A_J3D_PNGP ? PORT_J3DPNGP :
           pc == A_GX_SENDFLUSHPRIM ? PORT_FLUSHPRIM : pc == A_J3D_DIFFLIGHTOBJ ? PORT_LIGHTS : pc == A_PANE_DRAW ? PORT_PANEWALK :
           pc == A_J3D_MATDIFF ? PORT_MATDIFF : -1;
}
// --port-hooks MASK (bit per PORT_*). drawSelf: Ordon +4.5%, heavy area -0.5 +- 0.35% (GX-thread bound; jobs 492-493).
// J3D: Ordon +5.8%, heavy area -0.7 +- 0.4%, the title's cutscene and later stretch neutral (jobs 496-498).
// __GXSendFlushPrim (the dummy strip GXBegin and GXCallDisplayList send after BP writes, one guest store a word): off
// until measured (--ab flushhook). diffLightObj (3.6% of the heavy area's core 0 inclusive, job 512): off until
// measured (--ab lighthook); on since 2026-10-24 with its one-copy commit (heavy area +2.7%, jobs 1315-1322).
// The pane walk (bit 5): Ordon +2.2%, heavy area +0.4%, exact in check mode (jobs 1080-1089; James 2026-10-23: on).
u32 g_portHooks = 1u << PORT_MATERIAL | 1u << PORT_PICTUREEX | 1u << PORT_J3DPNGP | 1u << PORT_LIGHTS | 1u << PORT_PANEWALK |
                  1u << PORT_MATDIFF;  // (J3DMaterial::diff: heavy area and Ordon +3.0%, jobs 1340-1347; on 2026-10-24)
bool port_is_hook(u32 pc) {
    int h = hook_of(pc);
    return g_portMode && g_pm && h >= 0 && (g_portHooks >> h & 1);
}

// ---- check mode (--port check, PORT_CHECK=1 on the PC): the port's intended effects against the interpreter's ----
// The port runs into its context without committing; the interpreter then runs the call for real (its effects are the
// ones kept), its stores and pipe bytes recorded, and the two are compared: every word the interpreter wrote above the
// stack pointer but its LR save word, every word the port would have written, the pipe bytes, the registers the ABI
// keeps.
extern void (*gp_capture)(const u8* data, u32 len);
// The interpreter's pipe bytes (allocated at the first check: a walk sends ~19 KB, more than the 16 KB this held, which
// cut the walk's checks short at byte 16,384, jobs 1078-1079).
#define ICAP_CAP (WALK_PIPE * 2)
static u8* g_icap;
static u32 g_icapN;
static void icap_put(const u8* d, u32 len) { for (u32 i = 0; i < len && g_icapN < ICAP_CAP; i++) g_icap[g_icapN++] = d[i]; }
static void chk_bad(Gekko* g0, Pc* c, u32 hook, u32 what, u32 a, u32 want, u32 mine) {
    PortStats* s = &g_portStats;
    if (s->bad++) return;
    s->badHook = hook;
    s->badMat = g0->gpr[3];
    s->badWhat = what;
    s->badAddr = a;
    s->badWant = want;
    s->badMine = mine;
    s->badPipe[0] = c->pn;
    s->badPipe[1] = g_icapN;
#ifndef __3DS__
    static const char* kWhat[] = { "", "the call did not return", "pipe bytes", "memory", "__GXData", "logged stores", "registers" };
    fprintf(stderr, "PORT_CHECK %s %08lx: %s differ at %08lx: interpreter %08lx port %08lx (pipe bytes: port %u interpreter %u)\n",
            g_portNames[hook], (unsigned long)g0->gpr[3], kWhat[what], (unsigned long)a, (unsigned long)want, (unsigned long)mine,
            c->pn, g_icapN);
#endif
}
static u32 port_check(Gekko* g, u32 hook) {
    Pc* c = &g_pc;
    if (!g_icap && !(g_icap = walk_alloc(ICAP_CAP))) return 0;
    PortHook* hs = &g_portStats.h[hook];
    Gekko g0 = *g;
    bool took = port_run(c, g, hook);
    *g = g0;
    if (!took) { hs->refused++; hs->refusedBy[c->fail & 7]++; return 0; }  // (the guest code runs as usual)
    walk_undo(c);  // (the walk's stores: undone for the interpreter, compared below)
    if (g_portMode == 3 && c->pn) c->pbase[c->pn - 1] ^= 1;  // (--port plant: a known difference, the checker must report it)
    else if (g_portMode == 3 && c->gdN) c->gd[0] ^= 1;  // (no pipe bytes: the first GD byte)
    u32 sp = g->gpr[1], ret = g->lr;
    g_chkN = 0;
    g_icapN = 0;
    gp_capture = icap_put;
    g_memoRec = (void*)2;
    int why = 0;
    u32 n = gekko_run_call(g, ret, sp, 10000000u, &why, true);
    g_memoRec = NULL;
    gp_capture = NULL;
    if (g_icapN && g_ppipe) g_ppipe(g_pctx, g_icap, g_icapN);
    hs->checked++;
    hs->insReal += n;
    hs->insEst += c->n;
    hs->pipe += c->pn;
    if (why) { chk_bad(&g0, c, hook, 1, g->pc, 0, 0); return n ? n : 1; }
    if (g_icapN != c->pn || memcmp(g_icap, c->pbase, c->pn)) {
        u32 i = 0;
        while (i < c->pn && i < g_icapN && g_icap[i] == c->pbase[i]) i++;
        u32 want = 0, mine = 0;
        for (u32 k = i & ~3u; k < (i & ~3u) + 4; k++) {
            want = want << 8 | (k < g_icapN ? g_icap[k] : 0);
            mine = mine << 8 | (k < c->pn ? c->pbase[k] : 0);
        }
        chk_bad(&g0, c, hook, 2, i, want, mine);
        return n ? n : 1;
    }
    for (u32 i = 0; i < g_chkN; i++) {
        u32 a = g_chkW[i][0];
        if (a < sp || a == sp + 4) continue;
        u32 want = ram_rd32(g_pm, a - 0x80000000u), mine = g_chkW[i][1];
        if (a - c->gxd < GXD_BYTES) { if (c->have[(a - c->gxd) >> 2]) mine = c->gx[(a - c->gxd) >> 2]; }
        else for (u32 k = 0; k < c->nlog; k++) if (c->log[k][0] == a) mine = c->log[k][1];
        for (u32 k = 0; k < c->nundo; k++) if (c->undo[k][0] == a) mine = c->undo[k][2];  // (the walk's, newest last)
        for (u32 b = 0; b < 4; b++)  // (the GD bytes over the word as it was)
            if (a + b - c->gdA < c->gdN) mine = (mine & ~(0xFF000000u >> 8 * b)) | (u32)c->gd[a + b - c->gdA] << (24 - 8 * b);
        if (want != mine) {
#ifndef __3DS__
            static int dumped;  // PORT_DUMP=1: the first difference's GD bytes, port then interpreter (debugging)
            if (getenv("PORT_DUMP") && !dumped++) {
                fprintf(stderr, "PORT_DUMP r4 %08lx at %08lx (+%lu):\n port:", (unsigned long)g0.gpr[4], (unsigned long)c->gdA, (unsigned long)(a - c->gdA));
                for (u32 k = 0; k < c->gdN; k++) fprintf(stderr, "%s%02x", k % 32 ? " " : "\n  ", c->gd[k]);
                fprintf(stderr, "\n guest:");
                for (u32 k = 0; k < c->gdN + 32; k++) fprintf(stderr, "%s%02x", k % 32 ? " " : "\n  ", ram_rd8(g_pm, c->gdA - 0x80000000u + k));
                fputc('\n', stderr);
            }
#endif
            chk_bad(&g0, c, hook, 3, a, want, mine);
            return n ? n : 1;
        }
    }
    for (u32 k = 0; k < c->nt; k++) {
        u32 i = c->touched[k];
        u32 want = ram_rd32(g_pm, c->gxd - 0x80000000u + 4 * i);
        if (c->gx[i] != c->gx0[i] && want != c->gx[i]) { chk_bad(&g0, c, hook, 4, c->gxd + 4 * i, want, c->gx[i]); return n ? n : 1; }
    }
    for (u32 i = 0; i < c->nlog; i++) {
        bool later = false;  // (a later store to the same word decides)
        for (u32 k = i + 1; k < c->nlog; k++) later |= c->log[k][0] == c->log[i][0];
        u32 want = ram_rd32(g_pm, c->log[i][0] - 0x80000000u);
        if (!later && want != c->log[i][1]) { chk_bad(&g0, c, hook, 5, c->log[i][0], want, c->log[i][1]); return n ? n : 1; }
    }
    for (u32 i = 0; i < c->nundo; i++) {  // the walk's stores (the last to each word decides)
        bool later = false;
        for (u32 k = i + 1; k < c->nundo; k++) later |= c->undo[k][0] == c->undo[i][0];
        u32 want = ram_rd32(g_pm, c->undo[i][0] - 0x80000000u);
        if (!later && want != c->undo[i][2]) { chk_bad(&g0, c, hook, 5, c->undo[i][0], want, c->undo[i][2]); return n ? n : 1; }
    }
    for (u32 i = 0; i < c->gdN; i++) {
        u32 want = ram_rd8(g_pm, c->gdA - 0x80000000u + i);
        if (want != c->gd[i]) { chk_bad(&g0, c, hook, 3, c->gdA + i, want, c->gd[i]); return n ? n : 1; }
    }
    Gekko gw = g0;  // the registers as the port leaves them
    port_regs(c, &gw, hook);
    bool regs = g->pc == ret && g->gpr[1] == sp && g->gpr[2] == g0.gpr[2] && !memcmp(&g->gpr[13], &g0.gpr[13], 19 * 4) &&
                !memcmp(&g->fpr[14], &gw.fpr[14], 18 * sizeof(FPR)) && (g->cr & 0x00FFF000u) == (g0.cr & 0x00FFF000u);
    if (!regs) chk_bad(&g0, c, hook, 6, g->pc, 0, 0);
    return n ? n : 1;
}

#ifndef __3DS__
static void port_report(void) {
    for (int k = 0; k < PORT_N; k++) {
        PortHook* s = &g_portStats.h[k];
        fprintf(stderr, "PORT: %s %llu taken, %llu checked, %llu not taken; %llu pipe bytes\n", g_portNames[k],
                (unsigned long long)s->calls, (unsigned long long)s->checked, (unsigned long long)s->refused, (unsigned long long)s->pipe);
        if (s->checked)
            fprintf(stderr, "  guest instructions per checked call: interpreter %.1f, port estimate %.1f (kInsScale %.0f)\n",
                    (double)s->insReal / s->checked, (double)s->insEst / s->checked, 100.0 * s->insReal / (s->insEst ? s->insEst : 1));
        fprintf(stderr, "  not taken by reason (setup, memory, floats, texregion, palette, block type, texgens, other):");
        for (int q = 0; q < 8; q++) fprintf(stderr, " %llu", (unsigned long long)s->refusedBy[q]);
        fputc('\n', stderr);
    }
    const u64* w = g_portStats.mdWhy;
    fprintf(stderr, "PORT: J3DMaterial::diff not taken because of texture numbers %llu, texgens %llu, blend %llu, TEV stages %llu, "
            "block class %llu, room %llu, floats %llu, other %llu\n", (unsigned long long)w[0], (unsigned long long)w[1],
            (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5],
            (unsigned long long)w[6], (unsigned long long)w[7]);
    fprintf(stderr, "PORT: %llu checked calls differed\n", (unsigned long long)g_portStats.bad);
}
#endif
// The estimates scaled to what the interpreter ran for the same calls (the material: 1,920 against 1,510 a call, job
// 470; drawSelf: 2,739 against 2,203, jobs 490-491; J3D PNGP: 209 against 224, jobs 494-495; flush prim: 29.0 against
// 24.0 on the PC, 741 calls; diffLightObj: 3,562.7 against 1,107.8, 18,699 calls on the PC), so that the emulated clock
// advances as the guest code would have.
static const u32 kInsScale[PORT_N] = { 127, 124, 93, 121, 322, 129, 220 };  // (the walk: 13,377 against 10,400, job 1080; the material diff 6,151.5 against 2,795.3, 4,427 calls on the PC)
u32 port_run_hook(Gekko* g) {
    int hook = hook_of(g->pc);
    if (hook < 0 || !port_valid() || !g_ppipe) return 0;
#ifndef __3DS__
    static bool reg;
    if (!reg) { reg = true; atexit(port_report); }
#endif
    if (g_portMode >= 2) return port_check(g, (u32)hook);
    Pc* c = &g_pc;
    PortHook* hs = &g_portStats.h[hook];
    u64 t0 = prof_begin();
    g_tOn = t0 && g_portParts && hook == PORT_MATERIAL;
    g_tm = t0;
    bool pmu = g_portPmuHooks >> hook & 1;
    if (g_portPmuBegin && pmu) g_portPmuBegin();
    bool took = port_run(c, g, (u32)hook);
    if (g_portPmuEnd && pmu) g_portPmuEnd();
    g_tOn = false;
    if (!took) { hs->refused++; hs->refusedBy[c->fail & 7]++; return 0; }
    u64 t1 = t0 ? g_profClock() : 0;
    if (g_portCommitBegin && pmu) g_portCommitBegin();
    port_commit(c);
    port_regs(c, g, (u32)hook);
    if (hook == PORT_MATDIFF) md_flush(c, g);
    if (g_portCommitEnd && pmu) g_portCommitEnd();
    if (t0) {
        hs->timed++;
        hs->tRun += t1 - t0;
        hs->tCommit += g_profClock() - t1;
    }
    hs->calls++;
    hs->pipe += c->pn;
    g->pc = g->lr;
    return c->n ? c->n * kInsScale[hook] / 100 : 1;
}
