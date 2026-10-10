#pragma once
// Guest programs shared by the 3DS benchmark and the qemu correctness test.
#include <string.h>
#include "ppc.h"

#define D_(op, d, a, i) (((u32)(op) << 26) | ((u32)(d) << 21) | ((u32)(a) << 16) | ((u32)(i) & 0xFFFF))
#define ADDI(d, a, i) D_(14, d, a, i)
#define LIS(d, i) D_(15, d, 0, i)
#define ORI(a, s, i) D_(24, s, a, i)
#define ADDIC_(d, a, i) D_(13, d, a, i)
#define LWZ(d, a, o) D_(32, d, a, o)
#define STW(s, a, o) D_(36, s, a, o)
#define LFS(d, a, o) D_(48, d, a, o)
#define STFS(s, a, o) D_(52, s, a, o)
#define ADD(d, a, b) ((31u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | (266u << 1))
#define XOR(a, s, b) ((31u << 26) | ((s) << 21) | ((a) << 16) | ((b) << 11) | (316u << 1))
#define RLWINM(a, s, sh, mb, me) ((21u << 26) | ((s) << 21) | ((a) << 16) | ((sh) << 11) | ((mb) << 6) | ((me) << 1))
#define BC(bo, bi, off) ((16u << 26) | ((bo) << 21) | ((bi) << 16) | ((u32)(off) & 0xFFFC))
#define FADDS(d, a, b) ((59u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | (21u << 1))
#define FMULS(d, a, c) ((59u << 26) | ((d) << 21) | ((a) << 16) | ((c) << 6) | (25u << 1))
#define FMADDS(d, a, c, b) ((59u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | ((c) << 6) | (29u << 1))
#define PS_ADD(d, a, b) ((4u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | (21u << 1))
#define PS_SUB(d, a, b) ((4u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | (20u << 1))
#define PS_MUL(d, a, c) ((4u << 26) | ((d) << 21) | ((a) << 16) | ((c) << 6) | (25u << 1))
#define PS_MADD(d, a, c, b) ((4u << 26) | ((d) << 21) | ((a) << 16) | ((b) << 11) | ((c) << 6) | (29u << 1))
#define PSQ_(op, d, a, off, w, q) (((u32)(op) << 26) | ((d) << 21) | ((a) << 16) | ((w) << 15) | ((q) << 12) | ((u32)(off) & 0xFFF))
#define PSQ_L(d, a, off, w, q) PSQ_(56, d, a, off, w, q)
#define PSQ_ST(s, a, off, w, q) PSQ_(60, s, a, off, w, q)

typedef struct { const char* name; u32 code[40]; int n; int setup, body; } Program;

// Integer: load, add, shift, xor, store, pointer step and wrap, counted loop. 9 instructions per iteration.
static void make_int(Program* p, u32 iters) {
    u32 c[] = {
        LIS(4, iters >> 16), ORI(4, 4, iters & 0xFFFF), ADDI(3, 0, 0), ADDI(5, 0, 0), ADDI(7, 0, 0),
        LWZ(6, 5, 0), ADD(3, 3, 6), RLWINM(7, 3, 3, 0, 28), XOR(3, 3, 7), STW(3, 5, 4),
        ADDI(5, 5, 8), RLWINM(5, 5, 0, 19, 31), ADDIC_(4, 4, -1), BC(4, 2, (5 - 13) * 4),
        0,
    };
    p->name = "int"; memcpy(p->code, c, sizeof c); p->n = sizeof c / 4; p->setup = 5; p->body = 9;
}

// Scalar float: multiply-add, add, multiply, store, reload as int, accumulate, counted loop. 8 per iteration.
static void make_float(Program* p, u32 iters) {
    u32 c[] = {
        LIS(4, iters >> 16), ORI(4, 4, iters & 0xFFFF), LIS(5, 0x8000), ORI(5, 5, 0x1000),
        LFS(1, 5, 0), LFS(4, 5, 4), LFS(2, 5, 8), LFS(3, 5, 8), ADDI(7, 0, 0),
        FMADDS(2, 2, 1, 4), FADDS(3, 3, 2), FMULS(5, 3, 1), STFS(5, 5, 12), LWZ(6, 5, 12), ADD(7, 7, 6),
        ADDIC_(4, 4, -1), BC(4, 2, (9 - 16) * 4),
        0,
    };
    p->name = "float"; memcpy(p->code, c, sizeof c); p->n = sizeof c / 4; p->setup = 9; p->body = 8;
}

// Paired singles, shaped like a game's vertex transform: s16 vertices dequantised with psq_l, a 2x2 pair matrix
// plus translation, results stored as s16 and saturated u8 with psq_st, then reloaded to take the quantisation
// error. Source 0x0000-0x0FFF, matrix 0x1F00, destination 0x2000-0x3FFF. 16 instructions per iteration.
// GQR2 = load s16 >>8, GQR3 = store s16 <<4, GQR4 = store u8 <<2, GQR5 = load s16 >>4.
static void make_ps(Program* p, u32 iters) {
    u32 c[] = {
        LIS(4, iters >> 16), ORI(4, 4, iters & 0xFFFF), ADDI(5, 0, 0), ADDI(7, 0, 0x1F00),
        PSQ_L(5, 7, 0, 0, 0), PSQ_L(6, 7, 8, 0, 0), PSQ_L(7, 7, 16, 0, 0),
        PSQ_L(1, 5, 0, 0, 2), PSQ_L(2, 5, 4, 0, 2),
        PS_MUL(3, 5, 1), PS_MADD(3, 6, 2, 3), PS_ADD(3, 3, 7),
        RLWINM(6, 5, 1, 0, 30), ADDI(6, 6, 0x2000),
        PSQ_ST(3, 6, 0, 0, 3), PSQ_ST(3, 6, 4, 1, 4), PSQ_L(4, 6, 0, 0, 5), PS_SUB(3, 3, 4), PSQ_ST(3, 6, 8, 0, 0),
        ADDI(5, 5, 8), RLWINM(5, 5, 0, 20, 31), ADDIC_(4, 4, -1), BC(4, 2, (7 - 22) * 4),
        0,
    };
    p->name = "paired"; memcpy(p->code, c, sizeof c); p->n = sizeof c / 4; p->setup = 7; p->body = 16;
}

// Two vertices per iteration with the same work, either one after the other (each a dependent chain) or with
// their instructions interleaved, as a scheduler would order them. The gap is what instruction scheduling buys
// against VFP latency. 18 instructions per iteration (two vertices).
static void make_ps2(Program* p, u32 iters, int interleaved) {
    u32 a[] = { PSQ_L(1, 5, 0, 0, 2), PSQ_L(2, 5, 4, 0, 2), PS_MUL(3, 5, 1), PS_MADD(3, 6, 2, 3), PS_ADD(3, 3, 7), PSQ_ST(3, 6, 0, 0, 3) };
    u32 b[] = { PSQ_L(8, 5, 8, 0, 2), PSQ_L(9, 5, 12, 0, 2), PS_MUL(10, 5, 8), PS_MADD(10, 6, 9, 10), PS_ADD(10, 10, 7), PSQ_ST(10, 6, 16, 0, 3) };
    u32 c[40];
    int n = 0;
    u32 head[] = { LIS(4, iters >> 16), ORI(4, 4, iters & 0xFFFF), ADDI(5, 0, 0), ADDI(7, 0, 0x1F00),
                   PSQ_L(5, 7, 0, 0, 0), PSQ_L(6, 7, 8, 0, 0), PSQ_L(7, 7, 16, 0, 0),
                   RLWINM(6, 5, 1, 0, 30), ADDI(6, 6, 0x2000) };
    for (unsigned k = 0; k < 7; k++) c[n++] = head[k];
    int loop = n;
    c[n++] = head[7]; c[n++] = head[8];
    if (interleaved) for (int k = 0; k < 6; k++) { c[n++] = a[k]; c[n++] = b[k]; }
    else { for (int k = 0; k < 6; k++) c[n++] = a[k]; for (int k = 0; k < 6; k++) c[n++] = b[k]; }
    c[n++] = ADDI(5, 5, 16); c[n++] = RLWINM(5, 5, 0, 20, 31); c[n++] = ADDIC_(4, 4, -1);
    c[n] = BC(4, 2, (loop - n) * 4); n++;
    c[n++] = 0;
    p->name = interleaved ? "paired ilp" : "paired seq"; memcpy(p->code, c, n * 4); p->n = n; p->setup = 7; p->body = 18;
}
static void make_pseq(Program* p, u32 iters) { make_ps2(p, iters, 0); }
static void make_pilp(Program* p, u32 iters) { make_ps2(p, iters, 1); }

#define GUEST_TOUCHED 0x4000u

static void put_be32(u8* ram, u32 at, u32 v) { v = __builtin_bswap32(v); memcpy(ram + at, &v, 4); }

static void reset_guest(PPCState* s, u8* ram, int layout) {
    memset(s, 0, sizeof *s);
    memset(ram, 0, GUEST_TOUCHED);
    s->gqr[2] = (8u << 24) | ((u32)GQR_S16 << 16);
    s->gqr[3] = (4u << 8) | GQR_S16;
    s->gqr[4] = (2u << 8) | GQR_U8;
    s->gqr[5] = (4u << 24) | ((u32)GQR_S16 << 16);
    // Scalar float program: 0.5f, 1.0f, 0.0f.
    put_be32(ram, 0x1000, 0x3F000000u); put_be32(ram, 0x1004, 0x3F800000u); put_be32(ram, 0x1008, 0);
    // Paired program: matrix (0.75, 1.25), (-0.5, 0.25), translation (10, -3).
    static const u32 m[6] = { 0x3F400000u, 0x3FA00000u, 0xBF000000u, 0x3E800000u, 0x41200000u, 0xC0400000u };
    for (int i = 0; i < 6; i++) put_be32(ram, 0x1F00 + 4 * i, m[i]);
    // Paired program vertices: big-endian s16 from a fixed LCG.
    u32 x = 12345;
    for (u32 at = 0; at < 0x1000; at += 2) {
        x = x * 1103515245u + 12345u;
        u16 v = (u16)(x >> 16);
        ram[at] = (u8)(v >> 8); ram[at + 1] = (u8)v;
    }
    ram_to_layout(ram, GUEST_TOUCHED, layout);
}
