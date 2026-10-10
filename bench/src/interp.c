#include <stdio.h>
#include <string.h>
#include "ppc.h"

static u32 ppc_mask(int mb, int me) {
    u32 hi = 0xFFFFFFFFu >> mb;
    u32 lo = me == 31 ? 0 : 0xFFFFFFFFu >> (me + 1);
    return mb <= me ? hi & ~lo : hi | ~lo;
}

int ppc_decode(const u32* code, int n, Decoded* out, char* err, int errlen) {
    for (int i = 0; i < n; i++) {
        u32 w = code[i];
        Decoded* x = &out[i];
        memset(x, 0, sizeof *x);
        x->d = (w >> 21) & 31;
        x->a = (w >> 16) & 31;
        x->b = (w >> 11) & 31;
        x->c = (w >> 6) & 31;
        s32 simm = (s16)(w & 0xFFFF);
        u32 primary = w >> 26;
        if (w == 0) { x->op = OP_HALT; continue; }
        switch (primary) {
        case 14: x->op = OP_ADDI; x->imm = simm; break;
        case 15: x->op = OP_ADDIS; x->imm = (s32)((u32)simm << 16); break;
        case 24: x->op = OP_ORI; x->imm = w & 0xFFFF; break;
        case 13: x->op = OP_ADDIC_RC; x->imm = simm; break;
        case 32: x->op = OP_LWZ; x->imm = simm; break;
        case 36: x->op = OP_STW; x->imm = simm; break;
        case 48: x->op = OP_LFS; x->imm = simm; break;
        case 52: x->op = OP_STFS; x->imm = simm; break;
        case 56:
        case 60:
            x->op = primary == 56 ? OP_PSQ_L : OP_PSQ_ST;
            x->sh = (w >> 15) & 1;
            x->c = (w >> 12) & 7;
            x->imm = ((s32)(w << 20)) >> 20;
            break;
        case 21:
            if (w & 1) goto bad;
            x->op = OP_RLWINM; x->sh = x->b; x->mask = ppc_mask(x->c, (w >> 1) & 31);
            break;
        case 16:
            if (w & 3) goto bad;
            if (!(x->d & 4)) goto bad;  // CTR-decrementing forms are not in the benchmark
            x->op = OP_BC; x->bo = x->d; x->bi = x->a; x->target = i + (s16)(w & 0xFFFC) / 4;
            break;
        case 31: {
            if (w & 1) goto bad;
            u32 xo = (w >> 1) & 0x3FF;
            if (xo == 266) x->op = OP_ADD;
            else if (xo == 316) x->op = OP_XOR;
            else goto bad;
            break;
        }
        case 59: {
            if (w & 1) goto bad;
            u32 xo = (w >> 1) & 31;
            if (xo == 21) x->op = OP_FADDS;
            else if (xo == 25) x->op = OP_FMULS;
            else if (xo == 29) x->op = OP_FMADDS;
            else goto bad;
            break;
        }
        case 4: {
            if (w & 1) goto bad;
            u32 xo = (w >> 1) & 31;
            if (xo == 21) x->op = OP_PS_ADD;
            else if (xo == 20) x->op = OP_PS_SUB;
            else if (xo == 25) x->op = OP_PS_MUL;
            else if (xo == 29) x->op = OP_PS_MADD;
            else goto bad;
            break;
        }
        default:
        bad:
            snprintf(err, errlen, "decode: unsupported %08lx at %d", (unsigned long)w, i);
            return -1;
        }
    }
    return n;
}

// Guest byte `ea` lives at host offset (ea & mask) in the BE layout and at (ea & mask) ^ 3 in the swapped one. The
// swapped path is written byte by byte: it is the reference the JIT is checked against, not a fast path.
static inline u8* byte_at(u8* ram, u32 ea, int lay) { return ram + ((ea & GC_ADDR_MASK) ^ (lay == RAM_SWAPPED ? 3u : 0u)); }
static inline u32 load32(u8* ram, u32 ea, int lay) {
    if (lay == RAM_BE) { u32 v; memcpy(&v, ram + (ea & GC_ADDR_MASK), 4); return __builtin_bswap32(v); }
    return ((u32)*byte_at(ram, ea, lay) << 24) | ((u32)*byte_at(ram, ea + 1, lay) << 16) |
           ((u32)*byte_at(ram, ea + 2, lay) << 8) | *byte_at(ram, ea + 3, lay);
}
static inline void store32(u8* ram, u32 ea, u32 v, int lay) {
    if (lay == RAM_BE) { v = __builtin_bswap32(v); memcpy(ram + (ea & GC_ADDR_MASK), &v, 4); return; }
    for (int i = 0; i < 4; i++) *byte_at(ram, ea + i, lay) = (u8)(v >> (24 - 8 * i));
}
static inline u16 load16(u8* ram, u32 ea, int lay) { return (u16)((*byte_at(ram, ea, lay) << 8) | *byte_at(ram, ea + 1, lay)); }
static inline void store16(u8* ram, u32 ea, u16 v, int lay) { *byte_at(ram, ea, lay) = (u8)(v >> 8); *byte_at(ram, ea + 1, lay) = (u8)v; }

void ram_to_layout(u8* ram, u32 bytes, int layout) {
    if (layout != RAM_SWAPPED) return;
    for (u32 i = 0; i + 4 <= bytes; i += 4) { u32 v; memcpy(&v, ram + i, 4); v = __builtin_bswap32(v); memcpy(ram + i, &v, 4); }
}
static inline float u2f(u32 v) { float f; memcpy(&f, &v, 4); return f; }
static inline u32 f2u(float f) { u32 v; memcpy(&v, &f, 4); return v; }

// 2^k for k in [-32, 31], built exactly from the exponent bits.
static float pow2f(int k) { return u2f((u32)(127 + k) << 23); }
static int gqr_scale(u32 field6) { return field6 >= 32 ? (int)field6 - 64 : (int)field6; }
static int gqr_size(u32 type) { return type == GQR_FLOAT ? 4 : (type == GQR_U16 || type == GQR_S16) ? 2 : 1; }

static float psq_load_one(u8* ram, u32 ea, u32 type, float dq, int lay) {
    switch (type) {
    case GQR_U8: return (float)*byte_at(ram, ea, lay) * dq;
    case GQR_S8: return (float)(s8)*byte_at(ram, ea, lay) * dq;
    case GQR_U16: return (float)load16(ram, ea, lay) * dq;
    case GQR_S16: return (float)(s16)load16(ram, ea, lay) * dq;
    default: return u2f(load32(ram, ea, lay));
    }
}

static void psq_store_one(u8* ram, u32 ea, u32 type, float v, float q, int lay) {
    float lo, hi;
    switch (type) {
    case GQR_U8: lo = 0; hi = 255; break;
    case GQR_S8: lo = -128; hi = 127; break;
    case GQR_U16: lo = 0; hi = 65535; break;
    case GQR_S16: lo = -32768; hi = 32767; break;
    default: store32(ram, ea, f2u(v), lay); return;
    }
    float t = v * q;
    t = t < lo ? lo : t > hi ? hi : t;
    s32 iv = (s32)t;  // truncation, like the hardware
    if (gqr_size(type) == 1) *byte_at(ram, ea, lay) = (u8)iv;
    else store16(ram, ea, (u16)iv, lay);
}

// Cached interpreter: instructions are decoded once, then dispatched by a switch. This is the fastest non-JIT shape.
void ppc_interp(PPCState* s, u8* ram, const Decoded* prog, int lay) {
    u32* g = s->gpr;
    float* p0 = s->ps0;
    float* p1 = s->ps1;
    int i = 0;
    for (;;) {
        const Decoded* x = &prog[i];
        switch (x->op) {
        case OP_HALT: s->pc = i; return;
        case OP_ADDI:
        case OP_ADDIS: g[x->d] = (x->a ? g[x->a] : 0) + (u32)x->imm; i++; break;
        case OP_ORI: g[x->a] = g[x->d] | (u32)x->imm; i++; break;
        case OP_ADD: g[x->d] = g[x->a] + g[x->b]; i++; break;
        case OP_XOR: g[x->a] = g[x->d] ^ g[x->b]; i++; break;
        case OP_RLWINM: {
            u32 v = g[x->d];
            u32 r = x->sh ? (v << x->sh) | (v >> (32 - x->sh)) : v;
            g[x->a] = r & x->mask; i++; break;
        }
        case OP_LWZ: g[x->d] = load32(ram, (x->a ? g[x->a] : 0) + (u32)x->imm, lay); i++; break;
        case OP_STW: store32(ram, (x->a ? g[x->a] : 0) + (u32)x->imm, g[x->d], lay); i++; break;
        case OP_ADDIC_RC: {
            u64 wide = (u64)g[x->a] + (u32)x->imm;
            u32 r = (u32)wide;
            g[x->d] = r;
            s->xer = (s->xer & ~(1u << 29)) | ((u32)(wide >> 32) << 29);
            u32 so = s->xer >> 31;
            u32 lt = (s32)r < 0, gt = (s32)r > 0, eq = r == 0;
            s->cr = (s->cr & 0x0FFFFFFF) | (lt << 31) | (gt << 30) | (eq << 29) | (so << 28);
            i++; break;
        }
        case OP_BC: {
            u32 bit = (s->cr >> (31 - x->bi)) & 1;
            int taken = (x->bo & 16) || bit == ((x->bo >> 3) & 1u);
            i = taken ? x->target : i + 1;
            break;
        }
        // Single-precision scalar ops write the result to both lanes, as Gekko does.
        case OP_LFS: p0[x->d] = p1[x->d] = u2f(load32(ram, (x->a ? g[x->a] : 0) + (u32)x->imm, lay)); i++; break;
        case OP_STFS: store32(ram, (x->a ? g[x->a] : 0) + (u32)x->imm, f2u(p0[x->d]), lay); i++; break;
        case OP_FADDS: p0[x->d] = p1[x->d] = p0[x->a] + p0[x->b]; i++; break;
        case OP_FMULS: p0[x->d] = p1[x->d] = p0[x->a] * p0[x->c]; i++; break;
        case OP_FMADDS: { float t = p0[x->a] * p0[x->c]; p0[x->d] = p1[x->d] = t + p0[x->b]; i++; break; }
        case OP_PS_ADD: { float r0 = p0[x->a] + p0[x->b], r1 = p1[x->a] + p1[x->b]; p0[x->d] = r0; p1[x->d] = r1; i++; break; }
        case OP_PS_SUB: { float r0 = p0[x->a] - p0[x->b], r1 = p1[x->a] - p1[x->b]; p0[x->d] = r0; p1[x->d] = r1; i++; break; }
        case OP_PS_MUL: { float r0 = p0[x->a] * p0[x->c], r1 = p1[x->a] * p1[x->c]; p0[x->d] = r0; p1[x->d] = r1; i++; break; }
        case OP_PS_MADD: {
            float t0 = p0[x->a] * p0[x->c], t1 = p1[x->a] * p1[x->c];
            float r0 = t0 + p0[x->b], r1 = t1 + p1[x->b];
            p0[x->d] = r0; p1[x->d] = r1; i++; break;
        }
        case OP_PSQ_L: {
            u32 gq = s->gqr[x->c];
            u32 type = (gq >> 16) & 7;
            float dq = pow2f(-gqr_scale((gq >> 24) & 63));
            u32 ea = (x->a ? g[x->a] : 0) + (u32)x->imm;
            float v0 = psq_load_one(ram, ea, type, dq, lay);
            float v1 = x->sh ? 1.0f : psq_load_one(ram, ea + gqr_size(type), type, dq, lay);
            p0[x->d] = v0; p1[x->d] = v1; i++; break;
        }
        case OP_PSQ_ST: {
            u32 gq = s->gqr[x->c];
            u32 type = gq & 7;
            float q = pow2f(gqr_scale((gq >> 8) & 63));
            u32 ea = (x->a ? g[x->a] : 0) + (u32)x->imm;
            psq_store_one(ram, ea, type, p0[x->d], q, lay);
            if (!x->sh) psq_store_one(ram, ea + gqr_size(type), type, p1[x->d], q, lay);
            i++; break;
        }
        }
    }
}
