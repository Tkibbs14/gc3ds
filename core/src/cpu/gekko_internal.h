#pragma once
// Private state and helpers shared by the Gekko interpreter translation units. Nothing here is part of the public
// interface in gekko.h; the system side must not include this file.
#include "gekko.h"
#include <string.h>

// ---- Address translation tables ----
// BATs map blocks of at least 128 KB, so translation is uniform inside every 128 KB effective-address block and one
// table entry per block gives O(1) lookup (32768 entries cover the 4 GB space).
#define GK_BLK_SHIFT 17
#define GK_BLK_SIZE (1u << GK_BLK_SHIFT)
#define GK_BLK_MASK (GK_BLK_SIZE - 1)
#define GK_NBLK (1u << (32 - GK_BLK_SHIFT))

// trans[] entry: physical block base (high 15 bits) | flags.
#define TR_VALID 1u   // a BAT (or real mode) maps this block
#define TR_WRITE 2u   // stores allowed (BAT PP = 0b10, or real mode)
#define TR_HOSTQ 4u   // host_ptr() was queried for this block (rhost/whost are final)

typedef struct GkMap {
    u32 trans[GK_NBLK];
    u8* rhost[GK_NBLK];  // host pointer to the start of the block for loads/fetch, or NULL (use the bus)
    u8* whost[GK_NBLK];  // same for stores (NULL when the block is read-only or not RAM)
} GkMap;

typedef void (*GkOp)(Gekko* g, u32 inst);

// Test-only behaviour switches (see gekko_set_compat). Each one replaces a documented Gekko behaviour by the one qemu's
// 750 model implements, so the differential fuzzer can compare everything else bit for bit.
enum {
    GK_COMPAT_EXACT_SINGLE_MUL = 1u << 0,  // do not round frC to 25 bits in single-precision multiplies
    GK_COMPAT_MFFS_UPPER_ZERO = 1u << 1,   // mffs: upper word 0 instead of 0xFFF80000
    GK_COMPAT_FCTIW_QEMU = 1u << 2,        // fctiw[z]: upper word = sign extension (qemu) instead of 0xFFF80000/-0 quirk
    GK_COMPAT_STFS_TINY_ZERO = 1u << 3,    // stfs of a value below the single denormal range stores signed zero
    GK_COMPAT_NEG_THEN_ROUND = 1u << 4,    // fnmadd/fnmsub: round -(a*c +- b) instead of negating the rounded result
    GK_COMPAT_FR_QEMU = 1u << 5,           // FPSCR[FR] is never set by rounding (qemu does not implement FR)
    GK_COMPAT_FX_ALWAYS = 1u << 6,         // FX set by every signalled exception, not only on a 0 -> 1 transition
    GK_COMPAT_FCMP_QEMU = 1u << 7,         // fcmpu/fcmpo clear FI; fcmpo raising VXVC sets FPRF[C]
    GK_COMPAT_DIV_QEMU = 1u << 8,          // divw/divwu undefined cases (x/0, INT_MIN/-1) return the dividend
    GK_COMPAT_MTFSB1_QEMU = 1u << 9,       // mtfsb1 does not set FX
    GK_COMPAT_FPRF_DOUBLE = 1u << 10,      // single-precision results classified in double format (no single denormals)
};

typedef struct GkPriv {
    GkMap realMap;   // MSR.IR/DR = 0: identity (shared by the instruction and data sides)
    GkMap ibatMap;   // MSR.IR = 1
    GkMap dbatMap;   // MSR.DR = 1
    GkMap* imap;     // current instruction-side map
    GkMap* dmap;     // current data-side map
    u32 batPR;       // MSR.PR value the BAT maps were built for (Vs/Vp selection)

    u32 npc;         // next PC; handlers overwrite it to branch or to vector to an exception
    u32 curPc;       // address of the executing instruction
    bool brk;        // leave the inner loop after this instruction (state that affects interrupts changed)
    bool stopReq;    // gekko_stop() was called

    u64 nextDecEdge; // cycle at which DEC bit 0 next goes 0 -> 1
    bool decPending; // decrementer exception pending (delivered when MSR.EE = 1)

    bool resv;       // lwarx reservation
    u32 resvAddr;

    u32 fetchBlk;    // EA >> 17 of the cached fetch block, or 0xFFFFFFFF
    const u8* fetchHost;  // host pointer to that block, or NULL (fetch through bus->fetch)
    u32 fetchPa;     // physical base of that block

    u32 compat;      // GK_COMPAT_* (0 in the emulator)

    GekkoJitFn jit;  // optional block executor (see gekko_set_jit)
    void* jitCtx;
} GkPriv;

#define GKP(g) ((GkPriv*)(g)->priv)

// ---- Register / field helpers ----
#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u
#define XER_MASK 0xE000FF7Fu  // SO, OV, CA, byte-compare (bits 16-23), byte count (bits 25-31)

#define MSR_POW (1u << 18)
#define MSR_ILE (1u << 16)
#define MSR_ME (1u << 12)
#define MSR_FE0 (1u << 11)
#define MSR_FE1 (1u << 8)
#define MSR_LE (1u << 0)

// HID2 (750CL manual, HID2 register description). IBM bit 0 = MSB.
#define HID2_LSQE 0x80000000u
#define HID2_WPE 0x40000000u
#define HID2_PSE 0x20000000u
#define HID2_LCE 0x10000000u
#define HID2_DMAQL 0x0F000000u

// SRR1 program-exception reason bits.
#define SRR1_FPEXC 0x00100000u
#define SRR1_ILLEGAL 0x00080000u
#define SRR1_PRIV 0x00040000u
#define SRR1_TRAP 0x00020000u

// FPSCR bits.
#define FPSCR_FX 0x80000000u
#define FPSCR_FEX 0x40000000u
#define FPSCR_VX 0x20000000u
#define FPSCR_OX 0x10000000u
#define FPSCR_UX 0x08000000u
#define FPSCR_ZX 0x04000000u
#define FPSCR_XX 0x02000000u
#define FPSCR_VXSNAN 0x01000000u
#define FPSCR_VXISI 0x00800000u
#define FPSCR_VXIDI 0x00400000u
#define FPSCR_VXZDZ 0x00200000u
#define FPSCR_VXIMZ 0x00100000u
#define FPSCR_VXVC 0x00080000u
#define FPSCR_FR 0x00040000u
#define FPSCR_FI 0x00020000u
#define FPSCR_FPRF 0x0001F000u
#define FPSCR_FPCC 0x0000F000u
#define FPSCR_RES20 0x00000800u
#define FPSCR_VXSOFT 0x00000400u
#define FPSCR_VXSQRT 0x00000200u
#define FPSCR_VXCVI 0x00000100u
#define FPSCR_VE 0x00000080u
#define FPSCR_OE 0x00000040u
#define FPSCR_UE 0x00000020u
#define FPSCR_ZE 0x00000010u
#define FPSCR_XE 0x00000008u
#define FPSCR_NI 0x00000004u
#define FPSCR_RN 0x00000003u
#define FPSCR_VX_ALL (FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | FPSCR_VXVC | \
                      FPSCR_VXSOFT | FPSCR_VXSQRT | FPSCR_VXCVI)
#define FPSCR_EXC_ALL (FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ALL)

// Instruction fields.
#define I_RD(i) (((i) >> 21) & 31)
#define I_RS(i) (((i) >> 21) & 31)
#define I_RA(i) (((i) >> 16) & 31)
#define I_RB(i) (((i) >> 11) & 31)
#define I_RC(i) (((i) >> 6) & 31)
#define I_SIMM(i) ((u32)(s32)(s16)((i) & 0xFFFF))
#define I_UIMM(i) ((i) & 0xFFFF)
#define I_OE(i) (((i) >> 10) & 1)
#define I_RCB(i) ((i) & 1)
#define I_CRFD(i) (((i) >> 23) & 7)
#define I_CRFS(i) (((i) >> 18) & 7)
#define I_SPR(i) ((((i) >> 16) & 31) | (((i) >> 6) & 0x3E0))

// ---- Byte order ----
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GK_HOST_BE 1
#else
#define GK_HOST_BE 0
#endif
static inline u16 gk_ld16(const u8* p) { u16 v; memcpy(&v, p, 2); return GK_HOST_BE ? v : bswap16(v); }
static inline u32 gk_ld32(const u8* p) { u32 v; memcpy(&v, p, 4); return GK_HOST_BE ? v : bswap32(v); }
static inline u64 gk_ld64(const u8* p) { u64 v; memcpy(&v, p, 8); return GK_HOST_BE ? v : bswap64(v); }
static inline void gk_st16(u8* p, u16 v) { if (!GK_HOST_BE) v = bswap16(v); memcpy(p, &v, 2); }
static inline void gk_st32(u8* p, u32 v) { if (!GK_HOST_BE) v = bswap32(v); memcpy(p, &v, 4); }
static inline void gk_st64(u8* p, u64 v) { if (!GK_HOST_BE) v = bswap64(v); memcpy(p, &v, 8); }

// FPR lanes are held as doubles (gekko.h) but always moved as raw bits so signalling NaNs and payloads survive.
static inline u64 fpr_get0(const Gekko* g, int r) { u64 v; memcpy(&v, &g->fpr[r].ps0, 8); return v; }
static inline u64 fpr_get1(const Gekko* g, int r) { u64 v; memcpy(&v, &g->fpr[r].ps1, 8); return v; }
static inline void fpr_set0(Gekko* g, int r, u64 v) { memcpy(&g->fpr[r].ps0, &v, 8); }
static inline void fpr_set1(Gekko* g, int r, u64 v) { memcpy(&g->fpr[r].ps1, &v, 8); }

// ---- Core services (gekko.c) ----
void gk_exception(Gekko* g, u32 vector, u32 srr0, u32 srr1Bits);
void gk_program_exception(Gekko* g, u32 reason);  // SRR0 = current instruction
void gk_illegal(Gekko* g, u32 inst);              // reports through gekko_on_fatal, then program exception
void gk_set_msr(Gekko* g, u32 msr);               // MSR write with all side effects (maps, interrupt re-check)
u32 gk_mfspr(Gekko* g, u32 spr);
void gk_mtspr(Gekko* g, u32 spr, u32 v);
u64 gk_timebase(const Gekko* g);

// ---- Data memory (gekko_mem.c). Each returns false after raising DSI; the caller must then stop. ----
bool gk_read_slow(Gekko* g, u32 ea, u32 size, u64* out);
bool gk_write_slow(Gekko* g, u32 ea, u32 size, u64 v);
bool gk_translate(Gekko* g, u32 ea, bool write, bool data, u32* pa, bool raise);
void gk_map_invalidate(Gekko* g, bool ibat, bool dbat);
void gk_update_maps(Gekko* g);

static inline bool gk_rd8(Gekko* g, u32 ea, u32* v) {
    u8* h = GKP(g)->dmap->rhost[ea >> GK_BLK_SHIFT];
    if (h) { *v = h[ea & GK_BLK_MASK]; return true; }
    u64 t; if (!gk_read_slow(g, ea, 1, &t)) return false;
    *v = (u32)t; return true;
}
static inline bool gk_rd16(Gekko* g, u32 ea, u32* v) {
    u8* h = GKP(g)->dmap->rhost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 2) { *v = gk_ld16(h + o); return true; }
    u64 t; if (!gk_read_slow(g, ea, 2, &t)) return false;
    *v = (u32)t; return true;
}
static inline bool gk_rd32(Gekko* g, u32 ea, u32* v) {
    u8* h = GKP(g)->dmap->rhost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 4) { *v = gk_ld32(h + o); return true; }
    u64 t; if (!gk_read_slow(g, ea, 4, &t)) return false;
    *v = (u32)t; return true;
}
static inline bool gk_rd64(Gekko* g, u32 ea, u64* v) {
    u8* h = GKP(g)->dmap->rhost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 8) { *v = gk_ld64(h + o); return true; }
    return gk_read_slow(g, ea, 8, v);
}
static inline bool gk_wr8(Gekko* g, u32 ea, u32 v) {
    u8* h = GKP(g)->dmap->whost[ea >> GK_BLK_SHIFT];
    if (h) { h[ea & GK_BLK_MASK] = (u8)v; return true; }
    return gk_write_slow(g, ea, 1, v);
}
static inline bool gk_wr16(Gekko* g, u32 ea, u32 v) {
    u8* h = GKP(g)->dmap->whost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 2) { gk_st16(h + o, (u16)v); return true; }
    return gk_write_slow(g, ea, 2, v);
}
static inline bool gk_wr32(Gekko* g, u32 ea, u32 v) {
    u8* h = GKP(g)->dmap->whost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 4) { gk_st32(h + o, v); return true; }
    return gk_write_slow(g, ea, 4, v);
}
static inline bool gk_wr64(Gekko* g, u32 ea, u64 v) {
    u8* h = GKP(g)->dmap->whost[ea >> GK_BLK_SHIFT];
    u32 o = ea & GK_BLK_MASK;
    if (h && o <= GK_BLK_SIZE - 8) { gk_st64(h + o, v); return true; }
    return gk_write_slow(g, ea, 8, v);
}

// ---- Decoder tables (gekko_decode.c) and instruction groups ----
void gk_decode_init(void);
extern GkOp gk_prim[64];
extern GkOp gk_op4[1024], gk_op19[1024], gk_op31[1024], gk_op59[32], gk_op63[1024];
void gk_register_int(void);  // gekko_int.c fills its entries
void gk_register_fpu(void);  // gekko_fpu.c fills its entries

// ---- Test hooks (used by core/tests/cpu only) ----
void gekko_set_compat(Gekko* g, u32 flags);
