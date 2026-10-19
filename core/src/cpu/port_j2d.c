// Native library ports (docs/research/LIBRARY_PORTS.md): JSystem functions with the Dolphin GX SDK functions they
// reach, each as one native call hooked at the guest function's entry (PORT_* in port.h): J2D's material set-up
// (J2DMaterial::setGX), J2DPictureEx::drawSelf (the material, the vertex formats, GXBegin with the SDK's dirty-state
// flush, the quad) and J3D's concat-view matrix load (J3DShapeMtxConcatView::loadMtxConcatView_PNGP with either
// class's loadNrmMtx). The guest code makes tens of small SDK calls for each, each a call and return around a few bit
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
    if (getenv("PORT_CHECK")) g_portMode = 2;
    if (getenv("GC3DS_PORT_HOOKS")) g_portHooks = (u32)strtoul(getenv("GC3DS_PORT_HOOKS"), NULL, 0);
#endif
}
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx) { g_ppipe = write; g_pctx = ctx; }
PortStats g_portStats;
void (*g_portPmuBegin)(void), (*g_portPmuEnd)(void), (*g_portCommitBegin)(void), (*g_portCommitEnd)(void);

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
} Pc;

// Reasons (PortStats.refusedBy): 1 memory or buffers, 2 floats, 3 texture region, 4 TLUT region, 5 TEV block type,
// 6 texgens, 7 indirect stages; 0 the set-up.
static inline void give_up(Pc* c, u32 why) { if (!c->fail) c->fail = why; }
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
    if (e == 0xFF) give_up(c, 2);  // (NaN / infinity: not taken here)
    return bits;
}
static void gx_load_tex_mtx_imm(Pc* c, u32 mtx, u32 id, u32 type) {  // 0x80360320
    u32 addr = id >= 64 ? (id - 64) * 4 + 1280 : id * 4;
    u32 count = type == 1 ? 8 : 12;
    if (!psq_float_ok(c)) { give_up(c, 2); return; }
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
    if ((e == 0 && (v & 0x7FFFFF)) || e == 0xFF) { give_up(c, 2); return false; }
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
    if (!plain(c, bitsf(f7.p0)) || f7.p0 == f1.p0) { give_up(c, 2); return false; }
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

// ---- entry ----
static Pc g_pc;

// Runs the hooked function's port into a fresh context (nothing written to the guest). false: not taken (c->fail).
static bool port_run(Pc* c, Gekko* g, u32 hook) {
    c->g = g;
    c->pn = c->nlog = c->n = c->dirty = 0;
    c->pp = c->pipe;
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
    if (hook != PORT_MATERIAL && !(g->msr & MSR_FP)) { give_up(c, 2); return false; }
    if (hook == PORT_J3DPNGP) j3d_load_mtx_concat_view_pngp(c, g->gpr[3], g->gpr[4], g->gpr[5]);
    else if (hook == PORT_PICTUREEX) picex_draw_self(c, g->gpr[3], g->gpr[4]);
    else material_set_gx(c, g->gpr[3]);
    tmark(5);
    gx_sync(c);
    c->pn = (u32)(c->pp - c->pipe);
    return !c->fail;
}
static void port_commit(Pc* c) {
    for (u32 k = 0; k < c->nt; k++) {
        u32 i = c->touched[k];
        if (c->gx[i] != c->gx0[i]) ram_wr32(g_pm, c->gxd - 0x80000000u + 4 * i, c->gx[i]);
    }
    for (u32 i = 0; i < c->nlog; i++) ram_wr32(g_pm, c->log[i][0] - 0x80000000u, c->log[i][1]);
    if (c->pn && g_ppipe) g_ppipe(g_pctx, c->pipe, c->pn);
}

// The registers the guest code would leave other than the ABI promises (PSMTXConcat's leaked lanes, Pc.leak).
static void port_regs(Pc* c, Gekko* g, u32 hook) {
    if (!c->leak) return;
    g->fpr[14].ps1 = c->l14;
    g->fpr[15].ps1 = c->l15;
    if (hook != PORT_PICTUREEX) g->fpr[31].ps1 = c->l31;  // (drawTexCoord saves and restores f31 whole)
}

const char* const g_portNames[PORT_N] = { "J2DMaterial::setGX", "J2DPictureEx::drawSelf",
                                          "J3DShapeMtxConcatView::loadMtxConcatView_PNGP" };
static inline int hook_of(u32 pc) {
    return pc == A_MATERIAL_SETGX ? PORT_MATERIAL : pc == A_PICEX_DRAWSELF ? PORT_PICTUREEX : pc == A_J3D_PNGP ? PORT_J3DPNGP : -1;
}
// --port-hooks MASK (bit per PORT_*). drawSelf: Ordon +4.5%, heavy area -0.5 +- 0.35% (GX-thread bound; jobs 492-493).
// J3D: Ordon +5.8%, the title cutscene +7.3%, heavy area -0.7 +- 0.4% (jobs 496-498).
u32 g_portHooks = 1u << PORT_MATERIAL | 1u << PORT_PICTUREEX | 1u << PORT_J3DPNGP;
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
static u8 g_icap[PIPE_CAP * 2];
static u32 g_icapN;
static void icap_put(const u8* d, u32 len) { for (u32 i = 0; i < len && g_icapN < sizeof g_icap; i++) g_icap[g_icapN++] = d[i]; }
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
    PortHook* hs = &g_portStats.h[hook];
    Gekko g0 = *g;
    bool took = port_run(c, g, hook);
    *g = g0;
    if (!took) { hs->refused++; hs->refusedBy[c->fail & 7]++; return 0; }  // (the guest code runs as usual)
    if (g_portMode == 3 && c->pn) c->pipe[c->pn - 1] ^= 1;  // (--port plant: a known difference, the checker must report it)
    u32 sp = g->gpr[1], ret = g->lr;
    g_chkN = 0;
    g_icapN = 0;
    gp_capture = icap_put;
    g_memoRec = (void*)2;
    int why = 0;
    u32 n = gekko_run_call(g, ret, sp, 10000000u, &why);
    g_memoRec = NULL;
    gp_capture = NULL;
    if (g_icapN && g_ppipe) g_ppipe(g_pctx, g_icap, g_icapN);
    hs->checked++;
    hs->insReal += n;
    hs->insEst += c->n;
    hs->pipe += c->pn;
    if (why) { chk_bad(&g0, c, hook, 1, g->pc, 0, 0); return n ? n : 1; }
    if (g_icapN != c->pn || memcmp(g_icap, c->pipe, c->pn)) {
        u32 i = 0;
        while (i < c->pn && i < g_icapN && g_icap[i] == c->pipe[i]) i++;
        u32 want = 0, mine = 0;
        for (u32 k = i & ~3u; k < (i & ~3u) + 4; k++) {
            want = want << 8 | (k < g_icapN ? g_icap[k] : 0);
            mine = mine << 8 | (k < c->pn ? c->pipe[k] : 0);
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
        if (want != mine) { chk_bad(&g0, c, hook, 3, a, want, mine); return n ? n : 1; }
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
        fprintf(stderr, "  not taken by reason (setup, memory, floats, texregion, palette, block type, texgens, other):");
        for (int q = 0; q < 8; q++) fprintf(stderr, " %llu", (unsigned long long)s->refusedBy[q]);
        fputc('\n', stderr);
    }
    fprintf(stderr, "PORT: %llu checked calls differed\n", (unsigned long long)g_portStats.bad);
}
#endif
// The estimates scaled to what the interpreter ran for the same calls (the material: 1,920 against 1,510 a call, job
// 470; drawSelf: 2,739 against 2,203, jobs 490-491; J3D PNGP: 209 against 224, jobs 494-495), so that the emulated
// clock advances as the guest code would have.
static const u32 kInsScale[PORT_N] = { 127, 124, 93 };
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
    if (g_portPmuBegin) g_portPmuBegin();
    bool took = port_run(c, g, (u32)hook);
    if (g_portPmuEnd) g_portPmuEnd();
    g_tOn = false;
    if (!took) { hs->refused++; hs->refusedBy[c->fail & 7]++; return 0; }
    u64 t1 = t0 ? g_profClock() : 0;
    if (g_portCommitBegin) g_portCommitBegin();
    port_commit(c);
    port_regs(c, g, (u32)hook);
    if (g_portCommitEnd) g_portCommitEnd();
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
