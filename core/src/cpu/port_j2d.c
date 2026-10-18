// Native library ports (docs/research/LIBRARY_PORTS.md): JSystem J2D's material set-up (J2DMaterial::setGX) with the
// Dolphin GX SDK functions it reaches, as one native call. The guest code makes ~30-100 small SDK calls per pane, each
// a call and return around a few bit inserts into the SDK's shadow state (__GXData) and a register write to the gather
// pipe; here they share one context: __GXData copied in once, the pipe bytes collected in one buffer, the other stores
// (texture objects and cache regions) logged, all committed at the end. Anything not handled (another block type, a
// palette texture, a callback that is not the SDK's default, an address outside MEM1) gives up before the commit and
// the guest code runs instead.
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
#endif
}
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx) { g_ppipe = write; g_pctx = ctx; }
PortStats g_portStats;

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
            fprintf(stderr, "port: guest function %08lx differs: the J2D port stays off\n", (unsigned long)kFns[i].addr);
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
    u32 gx[GXD_BYTES / 4]; // its words (working copy) and as found
    u32 gx0[GXD_BYTES / 4];
    u8 pipe[PIPE_CAP];
    u32 pn;
    u32 log[LOG_MAX][2];   // other stores: address, value (in order; reads see the newest)
    u32 nlog;
    u32 n;                 // guest instructions the call stands for (an estimate: timing only)
    u32 fail;              // why it gives up (the first reason; PortStats.refusedBy)
} Pc;

// Reasons (PortStats.refusedBy): 1 memory or buffers, 2 floats, 3 texture region, 4 palette / TLUT, 5 TEV block type,
// 6 texgens, 7 indirect stages; 0 the set-up.
static inline void give_up(Pc* c, u32 why) { if (!c->fail) c->fail = why; }
static inline bool in_mem1(u32 a, u32 len) { return a >= 0x80000000u && a - 0x80000000u + len <= g_psz; }
static u32 rd8(Pc* c, u32 a);
static u32 rd32(Pc* c, u32 a) {
    if (a & 3) return rd8(c, a) << 24 | rd8(c, a + 1) << 16 | rd8(c, a + 2) << 8 | rd8(c, a + 3);  // (lwz at J2DTevBlock2 + 18)
    if (a - c->gxd < GXD_BYTES) return c->gx[(a - c->gxd) >> 2];
    for (u32 i = c->nlog; i-- > 0;) if (c->log[i][0] == a) return c->log[i][1];
    if (!in_mem1(a, 4)) { give_up(c, 1); return 0; }
    return ram_rd32(g_pm, a - 0x80000000u);
}
static u32 rd8(Pc* c, u32 a) { return (rd32(c, a & ~3u) >> (8 * (3 - (a & 3)))) & 0xFF; }
static u32 rd16(Pc* c, u32 a) {
    if (a & 1) { give_up(c, 1); return 0; }
    return (rd32(c, a & ~3u) >> (a & 2 ? 0 : 16)) & 0xFFFF;
}
static void wr32(Pc* c, u32 a, u32 v) {
    if (a - c->gxd < GXD_BYTES && !(a & 3)) { c->gx[(a - c->gxd) >> 2] = v; return; }
    if ((a & 3) || !in_mem1(a, 4) || c->nlog >= LOG_MAX) { give_up(c, 1); return; }
    c->log[c->nlog][0] = a;
    c->log[c->nlog][1] = v;
    c->nlog++;
}
// __GXData fields by offset.
#define GX(off) (c->gx[(off) >> 2])
static inline void bp_sent_not(Pc* c, u32 v) { GX(0) = (GX(0) & 0xFFFF0000u) | (v & 0xFFFF); }  // sth at +2
#define DIRTY 1452
static inline void p8(Pc* c, u32 v) { if (c->pn < PIPE_CAP) c->pipe[c->pn++] = (u8)v; else give_up(c, 1); }
static inline void p32(Pc* c, u32 v) { p8(c, v >> 24); p8(c, v >> 16); p8(c, v >> 8); p8(c, v); }
static inline void bp(Pc* c, u32 v) { p8(c, 0x61); p32(c, v); }          // GX_WRITE_RAS_REG
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
    GX(DIRTY) |= 4;
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
    GX(DIRTY) |= 4;
    c->n += 10;
}
static void gx_set_num_tex_gens(Pc* c, u32 n) {  // 0x8035BDFC
    GX(516) = rlwimi(GX(516), n, 0, 28, 31);
    xf(c, 4159, n & 0xFF);
    GX(DIRTY) |= 4;
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
    for (u32 i = 0; i < words; i++) p32(c, psq_copy(c, rd32(c, mtx + 4 * i)));
    c->n += 30;
}
static void gx_set_tev_order(Pc* c, u32 stage, u32 coord, u32 map, u32 color) {  // 0x8035F6F4
    u32 r10 = map & ~0x100u;
    GX(1364 + 4 * stage) = map;
    u32 tref = 256 + 4 * (stage >> 1);
    u32 r9 = r10 < 8 ? r10 : 0;
    if ((s32)coord >= 8) { GX(1432) &= ~(1u << stage); coord = 0; }
    else GX(1432) |= 1u << stage;
    u32 crc = color == 255 ? 7 : rd32(c, A_TEV_C2R + 4 * color);
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
    GX(DIRTY) |= 1;
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
    GX(DIRTY) |= 4;
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
    GX(DIRTY) |= 6;
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
    u32 fmt = rd32(c, obj + 20), mip = rd8(c, obj + 31) & 1, i8 = id & 7;
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
    static const s32 kTab[6] = { -31472, -31464, -31456, -31448, -31440, -31432 };
    u32 t[7];
    for (int k = 0; k < 6; k++) t[k] = rd8(c, r13 + (u32)kTab[k] + id);
    wr32(c, obj + 0, rlwimi(rd32(c, obj + 0), t[0], 24, 0, 7));
    wr32(c, obj + 4, rlwimi(rd32(c, obj + 4), t[1], 24, 0, 7));
    wr32(c, obj + 8, rlwimi(rd32(c, obj + 8), t[2], 24, 0, 7));
    wr32(c, reg + 0, rlwimi(rd32(c, reg + 0), t[3], 24, 0, 7));
    wr32(c, reg + 4, rlwimi(rd32(c, reg + 4), t[4], 24, 0, 7));
    wr32(c, obj + 12, rlwimi(rd32(c, obj + 12), t[5], 24, 0, 7));
    bp(c, rd32(c, obj + 0)); bp(c, rd32(c, obj + 4)); bp(c, rd32(c, obj + 8));
    bp(c, rd32(c, reg + 0)); bp(c, rd32(c, reg + 4)); bp(c, rd32(c, obj + 12));
    if (!(rd8(c, obj + 31) & 2)) {  // a palette: the default TLUT region
        if (GX(1228) != A_GXDEFAULT_TLUTREGION) { give_up(c, 4); return; }
        u32 tl = rd32(c, obj + 24);
        if (tl >= 20) { give_up(c, 4); return; }  // (the guest would use a NULL region)
        u32 tr = c->gxd + 904 + tl * 16;
        u32 t6 = rd8(c, r13 + (u32)(-31424) + id);
        wr32(c, tr + 4, rlwimi(rd32(c, tr + 4), t6, 24, 0, 7));
        bp(c, rd32(c, tr + 4));
    }
    GX(1300 + 4 * id) = rd32(c, obj + 8);
    GX(1332 + 4 * id) = rd32(c, obj + 0);
    GX(DIRTY) |= 1;
    bp_sent_not(c, 0);
    c->n += 110;
}

// ---- J2D (libs/JSystem/src/J2DGraph) ----
static void jut_texture_load(Pc* c, u32 tex, u32 id) {  // JUTTexture::load 0x802DE840
    if (rd32(c, tex + 44)) { give_up(c, 4); return; }  // (an embedded palette: JUTPalette::load, not ported)
    gx_load_tex_obj(c, tex, id);
    c->n += 20;
}
static void tev_block2_set_gx(Pc* c, u32 b) {  // J2DTevBlock2::setGX 0x802ED584
    u32 vt = rd32(c, b);
    if (rd32(c, vt + 16) != A_TEVBLOCK2_LOADTEX) { give_up(c, 5); return; }
    for (u32 i = 0; i < 2; i++) {  // loadTexture 0x802ED874
        u32 tex = rd32(c, b + 100 + 4 * i);
        if (tex && rd32(c, tex + 32)) jut_texture_load(c, tex, i);
        c->n += 12;
    }
    u32 ns = rd8(c, b + 50);
    for (u32 i = 0; i < ns; i++) gx_set_tev_order(c, i, rd8(c, b + 10 + 4 * i), rd8(c, b + 11 + 4 * i), rd8(c, b + 12 + 4 * i));
    for (u32 i = 0; i < 4; i++) gx_set_tev_color_s10(c, i == 3 ? 0 : i + 1, rd32(c, b + 18 + 8 * i), rd32(c, b + 22 + 8 * i));
    gx_set_num_tev_stages(c, ns);
    for (u32 i = 0; i < ns; i++) {
        u32 s = b + 8 * i;
        u32 b53 = rd8(c, s + 53), b54 = rd8(c, s + 54), b57 = rd8(c, s + 57), b58 = rd8(c, s + 58);
        gx_set_tev_color_in(c, i, b53 >> 4, b53 & 15, b54 >> 4, b54 & 15);
        gx_set_tev_alpha_in(c, i, (b57 & 0xE0) >> 5, (b57 >> 2) & 7, ((b57 & 3) << 1) | (b58 >> 7), (b58 >> 4) & 7);
        for (int a = 0; a < 2; a++) {
            u32 v = rd8(c, s + (a ? 56 : 52));
            u32 bias = v & 3, scale = (v & 0x30) >> 4, clamp = (v >> 3) & 1, reg = (v >> 6) & 3, r9 = (v >> 2) & 1;
            u32 op = bias == 3 ? ((r9 + ((v >> 3) & 6) + 8) & 0xFF) : r9;
            gx_set_tev_op(c, a ? 368 : 304, i, op, bias, scale, clamp, reg);
        }
        u32 k = rd8(c, b + 84 + i);
        gx_set_tev_ksel(c, i, k == 255 ? 0 : k, false);
        k = rd8(c, b + 86 + i);
        gx_set_tev_ksel(c, i, k == 255 ? 0 : k, true);
        c->n += 60;
    }
    for (u32 i = 0; i < 4; i++) gx_set_tev_kcolor(c, i, rd32(c, b + 68 + 4 * i));
    for (u32 i = 0; i < ns; i++) { u32 v = rd8(c, b + 58 + 8 * i); gx_set_tev_swap_mode(c, i, v & 3, (v >> 2) & 3); }
    for (u32 i = 0; i < 4; i++) {
        u32 v = rd8(c, b + 88 + i);
        gx_set_tev_swap_mode_table(c, i, (v >> 6) & 3, (v >> 4) & 3, (v >> 2) & 3, v & 3);
    }
    for (u32 i = 0; i < ns; i++) {  // J2DIndTevStage::load 0x802EA044
        u32 w = rd32(c, b + 92 + 4 * i);
        gx_set_tev_indirect(c, i & 0xFF, w & 3, (w >> 2) & 3, (w >> 4) & 7, (w >> 16) & 15, (w >> 8) & 7, (w >> 11) & 7,
                            (w >> 20) & 1, (w >> 21) & 1, (w >> 22) & 3);
        c->n += 20;
    }
    c->n += 80;
}
static void material_set_gx(Pc* c, u32 m) {  // J2DMaterial::setGX 0x802EA38C
    c->n += 15;
    if (!rd8(c, m + 14)) return;
    {  // J2DColorBlock::setGX 0x802EB424 (this = m + 16)
        u32 b = m + 16;
        for (u32 i = 0; i < 2; i++) gx_set_chan_mat_color(c, 4 + i, rd32(c, b + 4 * i));
        gx_set_num_chans(c, rd8(c, b + 8));
        u32 nc = rd8(c, b + 8) << 1;
        for (u32 i = 0; i < nc; i++)
            gx_set_chan_ctrl(c, rd32(c, A_CHAN_MAPPING + 4 * i), 0, 0, rd16(c, b + 10 + 2 * i) & 1, 0, 0, 2);
        gx_set_cull_mode(c, rd8(c, b + 18));
        c->n += 40;
    }
    {  // J2DTexGenBlock::setGX 0x802EB570 (this = m + 40)
        u32 b = m + 40, n = rd32(c, b);
        gx_set_num_tex_gens(c, n & 0xFF);
        if (n > 8) { give_up(c, 6); return; }
        for (u32 i = 0; i < n; i++)
            gx_set_tex_coord_gen2(c, i, rd8(c, b + 4 + 4 * i), rd8(c, b + 5 + 4 * i), rd8(c, b + 6 + 4 * i), 0, 125);
        for (u32 i = 0; i < 8; i++) {
            u32 tm = rd32(c, b + 36 + 4 * i);
            if (tm) gx_load_tex_mtx_imm(c, tm + 36, i * 3 + 30, rd8(c, tm));  // J2DTexMtx::load 0x802E9C90
        }
        c->n += 30;
    }
    u32 tev = rd32(c, m + 112);
    if (tev) {
        u32 fn = rd32(c, rd32(c, tev) + 12);
        if (fn == A_TEVBLOCK2_SETGX) tev_block2_set_gx(c, tev);
        else { give_up(c, 5); g_portStats.otherTev = fn; return; }
    }
    u32 ind = rd32(c, m + 116);
    if (ind) {
        u32 fn = rd32(c, rd32(c, ind) + 12);
        if (fn == A_INDBLOCKFULL_SETGX) {
            u32 ns = rd8(c, ind + 4);
            if (ns) { give_up(c, 7); return; }  // (indirect stages: not ported)
            gx_set_num_ind_stages(c, 0);
        } else if (fn != A_INDBLOCKNULL_SETGX && fn != A_INDBLOCK_SETGX) {
            give_up(c, 7);
            return;
        }
    }
    {  // J2DPEBlock::setGX 0x802F1840 (this = m + 120)
        u32 b = m + 120, w = rd16(c, b);
        gx_set_alpha_compare(c, (w >> 5) & 7, rd8(c, b + 2), (w >> 3) & 3, w & 7, rd8(c, b + 3));
        gx_set_blend_mode(c, rd8(c, b + 4), rd8(c, b + 5), rd8(c, b + 6), rd8(c, b + 7));
        gx_set_dither(c, rd8(c, b + 8));
    }
}

// ---- entry ----
static Pc g_pc;

// Runs the port into a fresh context (nothing written to the guest). false: not taken (c->fail).
static bool port_run(Pc* c, Gekko* g) {
    c->g = g;
    c->pn = c->nlog = c->n = 0;
    c->fail = 0;
    if (!in_mem1(A_GXDATA_PTR, 4)) return false;
    c->gxd = ram_rd32(g_pm, A_GXDATA_PTR - 0x80000000u);
    if (c->gxd != ram_rd32(g_pm, g->gpr[2] - 13440u - 0x80000000u) || !in_mem1(c->gxd, GXD_BYTES) || (c->gxd & 3)) return false;
    for (u32 i = 0; i < GXD_BYTES / 4; i++) c->gx[i] = c->gx0[i] = ram_rd32(g_pm, c->gxd - 0x80000000u + 4 * i);
    material_set_gx(c, g->gpr[3]);
    return !c->fail;
}
static void port_commit(Pc* c) {
    for (u32 i = 0; i < GXD_BYTES / 4; i++)
        if (c->gx[i] != c->gx0[i]) ram_wr32(g_pm, c->gxd - 0x80000000u + 4 * i, c->gx[i]);
    for (u32 i = 0; i < c->nlog; i++) ram_wr32(g_pm, c->log[i][0] - 0x80000000u, c->log[i][1]);
    if (c->pn && g_ppipe) g_ppipe(g_pctx, c->pipe, c->pn);
}

bool port_is_hook(u32 pc) { return g_portMode && pc == A_MATERIAL_SETGX && g_pm; }

// ---- check mode (--port check, PORT_CHECK=1 on the PC): the port's intended effects against the interpreter's ----
// The port runs into its context without committing; the interpreter then runs the call for real (its effects are the
// ones kept), its stores and pipe bytes recorded, and the two are compared: every word the interpreter wrote above the
// stack pointer but its LR save word, every word the port would have written, the pipe bytes, the registers the ABI
// keeps.
extern void (*gp_capture)(const u8* data, u32 len);
static u8 g_icap[PIPE_CAP * 2];
static u32 g_icapN;
static void icap_put(const u8* d, u32 len) { for (u32 i = 0; i < len && g_icapN < sizeof g_icap; i++) g_icap[g_icapN++] = d[i]; }
static void chk_bad(Gekko* g0, Pc* c, u32 what, u32 a, u32 want, u32 mine) {
    PortStats* s = &g_portStats;
    if (s->bad++) return;
    s->badMat = g0->gpr[3];
    s->badWhat = what;
    s->badAddr = a;
    s->badWant = want;
    s->badMine = mine;
    s->badPipe[0] = c->pn;
    s->badPipe[1] = g_icapN;
#ifndef __3DS__
    static const char* kWhat[] = { "", "the call did not return", "pipe bytes", "memory", "__GXData", "logged stores", "registers" };
    fprintf(stderr, "PORT_CHECK material %08lx: %s differ at %08lx: interpreter %08lx port %08lx (pipe bytes: port %u interpreter %u)\n",
            (unsigned long)g0->gpr[3], kWhat[what], (unsigned long)a, (unsigned long)want, (unsigned long)mine, c->pn, g_icapN);
#endif
}
static u32 port_check(Gekko* g) {
    Pc* c = &g_pc;
    Gekko g0 = *g;
    bool took = port_run(c, g);
    *g = g0;
    if (!took) { g_portStats.refused++; g_portStats.refusedBy[c->fail & 7]++; return 0; }  // (the guest code runs as usual)
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
    g_portStats.checked++;
    g_portStats.pipe += c->pn;
    if (why) { chk_bad(&g0, c, 1, g->pc, 0, 0); return n ? n : 1; }
    if (g_icapN != c->pn || memcmp(g_icap, c->pipe, c->pn)) {
        u32 i = 0;
        while (i < c->pn && i < g_icapN && g_icap[i] == c->pipe[i]) i++;
        u32 want = 0, mine = 0;
        for (u32 k = i & ~3u; k < (i & ~3u) + 4; k++) {
            want = want << 8 | (k < g_icapN ? g_icap[k] : 0);
            mine = mine << 8 | (k < c->pn ? c->pipe[k] : 0);
        }
        chk_bad(&g0, c, 2, i, want, mine);
        return n ? n : 1;
    }
    for (u32 i = 0; i < g_chkN; i++) {
        u32 a = g_chkW[i][0];
        if (a < sp || a == sp + 4) continue;
        u32 want = ram_rd32(g_pm, a - 0x80000000u), mine = g_chkW[i][1];
        if (a - c->gxd < GXD_BYTES) mine = c->gx[(a - c->gxd) >> 2];
        else for (u32 k = 0; k < c->nlog; k++) if (c->log[k][0] == a) mine = c->log[k][1];
        if (want != mine) { chk_bad(&g0, c, 3, a, want, mine); return n ? n : 1; }
    }
    for (u32 i = 0; i < GXD_BYTES / 4; i++) {
        u32 want = ram_rd32(g_pm, c->gxd - 0x80000000u + 4 * i);
        if (c->gx[i] != c->gx0[i] && want != c->gx[i]) { chk_bad(&g0, c, 4, c->gxd + 4 * i, want, c->gx[i]); return n ? n : 1; }
    }
    for (u32 i = 0; i < c->nlog; i++) {
        bool later = false;  // (a later store to the same word decides)
        for (u32 k = i + 1; k < c->nlog; k++) later |= c->log[k][0] == c->log[i][0];
        u32 want = ram_rd32(g_pm, c->log[i][0] - 0x80000000u);
        if (!later && want != c->log[i][1]) { chk_bad(&g0, c, 5, c->log[i][0], want, c->log[i][1]); return n ? n : 1; }
    }
    bool regs = g->pc == ret && g->gpr[1] == sp && g->gpr[2] == g0.gpr[2] && !memcmp(&g->gpr[13], &g0.gpr[13], 19 * 4) &&
                !memcmp(&g->fpr[14], &g0.fpr[14], 18 * sizeof(FPR)) && (g->cr & 0x00FFF000u) == (g0.cr & 0x00FFF000u);
    if (!regs) chk_bad(&g0, c, 6, g->pc, 0, 0);
    return n ? n : 1;
}

#ifndef __3DS__
static void port_report(void) {
    PortStats* s = &g_portStats;
    fprintf(stderr, "PORT: J2DMaterial::setGX %llu taken, %llu checked (%llu differed), %llu not taken; %llu pipe bytes\n",
            (unsigned long long)s->calls, (unsigned long long)s->checked, (unsigned long long)s->bad,
            (unsigned long long)s->refused, (unsigned long long)s->pipe);
}
#endif
u32 port_run_hook(Gekko* g) {
    if (!port_valid() || !g_ppipe) return 0;
#ifndef __3DS__
    static bool reg;
    if (!reg) { reg = true; atexit(port_report); }
#endif
    if (g_portMode >= 2) return port_check(g);
    Pc* c = &g_pc;
    if (!port_run(c, g)) { g_portStats.refused++; g_portStats.refusedBy[c->fail & 7]++; return 0; }
    port_commit(c);
    g_portStats.calls++;
    g_portStats.pipe += c->pn;
    g->pc = g->lr;
    return c->n ? c->n : 1;
}
