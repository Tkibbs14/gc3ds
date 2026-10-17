// Native stand-ins for J3D's display-list writers (JSystem J3DGD, the hottest guest functions of Twilight Princess:
// J3DGDSetLightAttn 3.49%, SetLightPos 2.11%, SetLightDir 2.11% of its guest instructions; hist_900, PC profile).
// Each is straight-line code that packs a GX command into the current display list (__GDCurrentDL) one byte at a time:
// ~50-190 guest instructions for 17-29 bytes. The stand-in writes the same bytes, advances the list's write pointer
// and returns to LR, counting the guest instructions it stood in for.
//
// Semantics from the decompilation (zeldaret/tp, libs/JSystem J3DGD.cpp/.h and the Dolphin SDK's GDBase.h / GDLight.h):
// an XF command header is 0x10, u16 count - 1, u16 XF address; floats are stored as stfs stores them (same conversion
// as the interpreter); __GDWrite has no overflow check. A light's index is 31 - cntlzw(id) in u16, and the compiled
// code (not the decompilation's header) clamps an index above 7 to 0: cmplwi 7, ble +8, li 0 at +0x10..+0x18.
// What a stand-in does not reproduce is dead state: the ABI's volatile registers and the callee's own stack frame
// (owner decision 2026-10-15, docs: gc3ds-timing-drift-approved). HLE_CHECK=1 (PC) compares everything else with the
// interpreter: the list's bytes, its write pointer, the non-volatile registers, LR, PC and the instruction count.
//
// A function is recognised by a hash of its bytes, and only if it is straight-line apart from the index clamp's one
// forward branch (ble +8 at +0x14, over one instruction) and its final blr: the count is its length less one when the
// clamp is skipped. The address of __GDCurrentDL is the first r13-relative lwz in it (the game's small data).
#include "../ramlayout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gekko_internal.h"
#include "gekko_softfp.h"
#include "hle.h"

typedef struct {
    const char* name;
    u32 pc, size;     // Twilight Princess (GZ2E01) addresses, zeldaret/tp config/GZ2E01/symbols.txt
    u64 hash;         // FNV-1a 64 of the function's bytes (0: print it with HLE_PRINT_HASH and refuse)
    u16 xf;           // XF address of light 0
    u8 nf;            // floats written (f1..f(nf))
    s8 known;         // -1 not checked, 0 refused, 1 recognised
    u32 insns;        // guest instructions it stands in for (its length / 4)
    s32 sda;          // offset of __GDCurrentDL from r13
} GdFn;

static GdFn g_fn[] = {
    { "J3DGDSetLightAttn", 0x8030D364u, 0x2F8u, 0xF3E59AD98A299B8Dull, 0x0604, 6, -1, 0, 0 },
    { "J3DGDSetLightPos", 0x8030D76Cu, 0x1CCu, 0x633C24DEB6C3AFA9ull, 0x060A, 3, -1, 0, 0 },
    { "J3DGDSetLightDir", 0x8030D938u, 0x1CCu, 0xE204DE6EB1034630ull, 0x060D, 3, -1, 0, 0 },
};
#define NFN (sizeof g_fn / sizeof g_fn[0])

static u8* g_mem;
static u32 g_memSize;
u64 g_hleGdRuns, g_hleGdInsns;

static u32 rd_be32(u32 pa) { return ram_rd32(g_mem, pa); }  // (guest memory in its layout, ramlayout.h)

static bool check_fn(GdFn* f) {
    u32 pa = f->pc & 0x0FFFFFFFu;
    if (!f->pc || !f->size || (f->size & 3) || !g_mem || pa + f->size > g_memSize) return false;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < f->size; i++) { h ^= ram_rd8(g_mem, pa + i); h *= 0x100000001B3ull; }
#ifndef __3DS__
    if (getenv("HLE_PRINT_HASH")) fprintf(stderr, "HLE hash of %s %08x+%x: %016llx\n", f->name, f->pc, f->size, (unsigned long long)h);
#endif
    if (!f->hash || h != f->hash) return false;  // another build of the code (or another game)
    f->sda = 0x7FFFFFFF;
    for (u32 i = 0; i < f->size; i += 4) {
        u32 in = rd_be32(pa + i), op = in >> 26;
        bool last = i + 4 == f->size;
        if (last) { if (in != 0x4E800020u) return false; continue; }  // blr
        if (in == 0x40810008u && i == 0x14) continue;                    // the index clamp's ble +8
        if (op == 16 || op == 18) return false;                          // bc, b
        if (op == 19 && (((in >> 1) & 0x3FF) == 16 || ((in >> 1) & 0x3FF) == 528)) return false;  // bclr, bcctr
        if (op == 32 && ((in >> 16) & 31) == 13 && f->sda == 0x7FFFFFFF) f->sda = (s16)(in & 0xFFFF);  // lwz rD, d(r13)
    }
    if (f->sda == 0x7FFFFFFF) return false;
    f->insns = f->size / 4;
#ifndef __3DS__
    if (getenv("HLE_CHECK")) fprintf(stderr, "HLE_CHECK %s recognised: %u instructions, __GDCurrentDL at r13%+d\n", f->name, f->insns, f->sda);
#endif
    return true;
}

static GdFn* find(u32 pc) {
    for (u32 k = 0; k < NFN; k++) {
        GdFn* f = &g_fn[k];
        if (f->pc != pc) continue;
        if (f->known < 0) {
            f->known = check_fn(f) ? 1 : 0;
#ifndef __3DS__
            if (!f->known && getenv("HLE_CHECK")) fprintf(stderr, "HLE_CHECK %s at %08x NOT recognised (bytes or shape differ)\n", f->name, f->pc);
#endif
        }
        return f->known == 1 ? f : NULL;
    }
    return NULL;
}

void hle_gd_set_memory(u8* mem1, u32 size) {
    g_mem = mem1;
    g_memSize = size;
    for (u32 k = 0; k < NFN; k++) g_fn[k].known = -1;
}
void hle_gd_invalidate(u32 pa) {
    for (u32 k = 0; k < NFN; k++) {
        u32 lo = g_fn[k].pc & 0x0FFFFFFFu, hi = lo + g_fn[k].size;
        if (pa >> 12 >= lo >> 12 && pa >> 12 <= (hi - 1) >> 12) g_fn[k].known = -1;
    }
}
static bool g_gdOn = true;
void hle_gd_set_enabled(bool on) { g_gdOn = on; }
bool hle_gd_enabled(void) { return g_gdOn; }
bool hle_gd_is_hook(u32 pc) { return g_gdOn && find(pc) != NULL; }

// Guest RAM through the standard BAT mappings (0x80000000 cached, 0xC0000000 uncached, both physical 0).
static bool ram(u32 ea, u32 len, u32* pa) {
    u32 top = ea >> 28;
    *pa = ea & 0x0FFFFFFFu;
    return (top == 0x8 || top == 0xC) && *pa + len <= g_memSize && *pa + len >= *pa;
}

static u32 gd_native(Gekko* g);

#ifndef __3DS__
// HLE_CHECK=1: every call also runs in the interpreter from the same state for the same instruction count; compared:
// PC, LR, r1, r2, r13-r31, f14-f31, the list's write pointer and the bytes written. The interpreter's result stands.
static struct { u64 calls, bad; } g_gchk;
static void gchk_report(void) {
    fprintf(stderr, "HLE_CHECK (J3DGD): %llu stand-in calls compared with the interpreter, %llu differed\n",
            (unsigned long long)g_gchk.calls, (unsigned long long)g_gchk.bad);
}
static u32 gd_checked(Gekko* g) {
    GdFn* f = find(g->pc);
    if (!f) return 0;
    u32 dlPa, objPa, outPa;
    if (!ram(g->gpr[13] + (u32)f->sda, 4, &dlPa) || !ram(rd_be32(dlPa), 16, &objPa)) return 0;
    u32 ptr = rd_be32(objPa + 8), len = 5 + 4u * f->nf;
    if (!ram(ptr, len, &outPa)) return 0;
    static u8 before[64], native[64];
    Gekko g0 = *g;
    u8 obj0[16];
    memcpy(obj0, g_mem + objPa, 16);
    ram_to_be(before, g_mem, outPa, len);  // (the output, unaligned: as the guest's bytes)
    u32 n = gd_native(g);
    if (!n) return 0;
    Gekko gn = *g;
    u8 objN[16];
    memcpy(objN, g_mem + objPa, 16);
    ram_to_be(native, g_mem, outPa, len);
    *g = g0;  // the interpreter from the same state (its own stack frame writes are not compared: dead state)
    memcpy(g_mem + objPa, obj0, 16);
    ram_from_be(g_mem, outPa, before, len);
    u32 steps = 0;
    while (steps < n) {
        u32 pa = g->pc & 0x0FFFFFFFu;
        if (pa + 4 > g_memSize) break;
        bool stop;
        g->pc = gekko_interp_one(g, g->pc, rd_be32(pa), &stop);
        steps++;
    }
    if (!g_gchk.calls++) atexit(gchk_report);
    u8 now[64];
    ram_to_be(now, g_mem, outPa, len);
    bool same = steps == n && g->pc == gn.pc && g->lr == gn.lr && g->gpr[1] == gn.gpr[1] && g->gpr[2] == gn.gpr[2] &&
                !memcmp(&g->gpr[13], &gn.gpr[13], 19 * 4) && !memcmp(&g->fpr[14], &gn.fpr[14], 18 * sizeof(FPR)) &&
                !memcmp(g_mem + objPa, objN, 16) && !memcmp(now, native, len);
    if (!same && g_gchk.bad++ < 20) {
        fprintf(stderr, "HLE_CHECK %s call %llu: native %u insns to %08x, interpreter %u to %08x\n", f->name,
                (unsigned long long)g_gchk.calls, n, gn.pc, steps, g->pc);
        for (u32 k = 0; k < len; k++)
            if (now[k] != native[k]) fprintf(stderr, "  byte %u: native %02x interpreter %02x\n", k, native[k], now[k]);
        if (memcmp(g_mem + objPa, objN, 16)) fprintf(stderr, "  list object differs\n");
    }
    return steps;
}
#endif

u32 hle_gd_run(Gekko* g) {
    if (!g_gdOn) return 0;
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) chk = getenv("HLE_CHECK") != NULL;
    if (chk) return gd_checked(g);
#endif
    return gd_native(g);
}

static u32 gd_native(Gekko* g) {
    GdFn* f = find(g->pc);
    if (!f) return 0;
    u32 dlEa = g->gpr[13] + (u32)f->sda, dlPa, objPa;
    if (!ram(dlEa, 4, &dlPa)) return 0;
    u32 obj = rd_be32(dlPa);
    if (!ram(obj, 16, &objPa)) return 0;
    u32 ptr = rd_be32(objPa + 8), len = 5 + 4u * f->nf, out;
    if (!ram(ptr, len, &out)) return 0;
    u32 id = g->gpr[3];
    u32 idx = (u16)(0x1F - (u32)(id ? __builtin_clz(id) : 32));
    u32 count = f->insns - 1;            // ble taken over li 0
    if (idx > 7) { idx = 0; count++; }  // cmplwi 7; ble not taken; li r0,0
    u16 addr = (u16)(f->xf + (u16)((idx << 4) & 0xFFF0));
    ram_wr8(g_mem, out, 0x10);
    ram_wr16(g_mem, out + 1, (u32)(f->nf - 1));
    ram_wr16(g_mem, out + 3, addr);
    bool tiny = (GKP(g)->compat & GK_COMPAT_STFS_TINY_ZERO) != 0;
    for (u32 k = 0; k < f->nf; k++) {
        u32 w = sf_double_to_single(fpr_get0(g, 1 + (int)k), tiny);
        ram_wr32(g_mem, out + 5 + 4 * k, w);
    }
    u32 np = ptr + len;
    ram_wr32(g_mem, objPa + 8, np);
    g->pc = g->lr;
    g_hleGdRuns++;
    g_hleGdInsns += count;
    return count;
}
