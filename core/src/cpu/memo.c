// Function memoization, probe stage (measurement only: nothing is replayed yet). A function the game calls with the
// same inputs gives the same result: the same stores, the same bytes to the GX pipe, the same registers on return. So a
// call can be recorded once (every memory word it reads before writing it, with its value; every word it writes; the
// pipe bytes) and, while every recorded input still holds, its effects applied without running it. The probe runs each
// call of the hooked function (--memo ADDR) in the interpreter with the data accesses recorded, compares the inputs with
// the previous call's for the same r3 (the object), and reports how often they matched: whether replay would pay.
//   Inputs are taken at word granularity (an access of any size reads its whole aligned words); a word written before
// it is read is the call's own (stack temporaries); a sub-word write to a word not seen yet takes the word as an input
// first. A read of anything that is not RAM (hardware registers) makes the call unmemoizable; stores to the gather pipe
// are its output bytes. The argument registers, r1, r2, r13, the GQRs, FPSCR and MSR are part of the key.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gekko_internal.h"
#include "memo.h"

void* g_memoRec;   // non-NULL while a call is recorded (gekko_internal.h's accessors call memo_access)
u32 g_memoAddr;    // the hooked function's entry (0: off)
MemoStats g_memoStats;

#define SEEN_BITS 15
#define SEEN_N (1u << SEEN_BITS)
#define REC_MAX 65536u
typedef struct { u32 w, state; } Seen;  // state: 1 read first (an input), 2 written first (the call's own)
static Seen g_seen[SEEN_N];
static u32 g_seenUsed[REC_MAX], g_nSeenUsed;
static u32 g_rd[REC_MAX][2], g_nrd;  // inputs: word address, value, in first-read order
static u32 g_nwr, g_pipe, g_flags, g_sp;
enum { MF_MMIO = 1, MF_FULL = 2 };

static Seen* seen_slot(u32 w) {
    u32 h = (w >> 2) * 2654435761u >> (32 - SEEN_BITS);
    for (u32 k = 0; k < SEEN_N; k++, h = (h + 1) & (SEEN_N - 1)) {
        Seen* s = &g_seen[h];
        if (s->state == 0) {
            if (g_nSeenUsed >= REC_MAX) { g_flags |= MF_FULL; return NULL; }
            s->w = w;
            g_seenUsed[g_nSeenUsed++] = h;
            return s;
        }
        if (s->w == w) return s;
    }
    g_flags |= MF_FULL;
    return NULL;
}

static void note_word(Gekko* g, u32 w, bool write, bool whole) {
    Seen* s = seen_slot(w);
    if (!s) return;
    if (s->state == 0) {
        if (!write || !whole) {  // an input: its value now (before a partial write changes part of it)
            u8* h = GKP(g)->dmap->rhost[w >> GK_BLK_SHIFT];
            if (!h) { g_flags |= MF_MMIO; return; }
            if (g_nrd >= REC_MAX) { g_flags |= MF_FULL; return; }
            g_rd[g_nrd][0] = w;
            g_rd[g_nrd][1] = ram_rd32(h, w & GK_BLK_MASK);
            g_nrd++;
            s->state = 1;
        } else {
            s->state = 2;
        }
    }
    if (write && w >= g_sp && !(s->state & 4)) { s->state |= 4; g_nwr++; }  // (an output: above the stack pointer)
}

// The port checker's recorder (port_j2d.c, PORT_CHECK on the PC; g_memoRec == 2): each word the interpreter writes,
// with its value before, once.
u32 g_chkW[CHK_MAX][2];
u32 g_chkN;
bool g_chkOn;
static u32 g_chkSet[CHK_MAX * 2], g_chkSetGen[CHK_MAX * 2], g_chkGen = 1;
static void chk_note(Gekko* g, u32 ea, u32 size) {
    if (!GKP(g)->dmap->rhost[ea >> GK_BLK_SHIFT]) return;  // (the gather pipe: its bytes are captured)
    if (g_chkN == 0) g_chkGen++;
    for (u32 w = ea & ~3u, last = (ea + size - 1) & ~3u; w <= last; w += 4) {
        u32 k = (w >> 2) * 2654435761u & (CHK_MAX * 2 - 1);
        while (g_chkSetGen[k] == g_chkGen && g_chkSet[k] != w) k = (k + 1) & (CHK_MAX * 2 - 1);
        if (g_chkSetGen[k] == g_chkGen || g_chkN >= CHK_MAX) continue;
        g_chkSetGen[k] = g_chkGen;
        g_chkSet[k] = w;
        u8* h = GKP(g)->dmap->rhost[w >> GK_BLK_SHIFT];
        g_chkW[g_chkN][0] = w;
        g_chkW[g_chkN][1] = h ? ram_rd32(h, w & GK_BLK_MASK) : 0;
        g_chkN++;
    }
}

void memo_access(Gekko* g, u32 ea, u32 size, bool write) {
    if (g_memoRec == (void*)2) { if (write) chk_note(g, ea, size); return; }
    u32 blk = ea >> GK_BLK_SHIFT;
    if (!GKP(g)->dmap->rhost[blk]) {
        u32 pa;
        if (write && gk_translate(g, ea, true, true, &pa, false) && (pa & ~31u) == 0x0C008000u) { g_pipe += size; return; }
        g_flags |= MF_MMIO;
        return;
    }
    u32 first = ea & ~3u, last = (ea + size - 1) & ~3u;
    for (u32 w = first; w <= last; w += 4) note_word(g, w, write, write && w >= ea && w + 4 <= ea + size);
}

// The previous call's inputs per object (r3), kept whole for the diagnosis of misses.
#define KEEP 16
#define PREV_MAX 8192u
typedef struct { u32 r3; u64 hash, key; u32 n; u32 rd[PREV_MAX][2]; u32 regs[12]; } Prev;
static Prev g_prev[KEEP];
static u32 g_prevNext;
// Misses diagnosed: calls with the key registers different (per register), with another input count, and per input
// word that changed: how often, by address (a small table of the most frequent).
u64 g_memoDiag[16];  // [0] key regs differ, [1] input count differs, [2] same count but words differ, [3] words differing (sum)
                     // [4..15] per key register differing: r1..r10, r13, other (f1-f8, GQRs, FPSCR, MSR)
#define DIFF_N 64
u32 g_memoDiff[DIFF_N][3];  // address, count, last value

static u64 fnv(u64 h, u32 v) { return (h ^ v) * 0x100000001B3ull; }

u32 memo_probe(Gekko* g) {
    MemoStats* st = &g_memoStats;
    st->calls++;
    u32 ret = g->lr, r3 = g->gpr[3];
    // The key's registers: arguments, the stack and small-data pointers, the quantizers, FPSCR, MSR.
    u32 entryRegs[14];
    for (int r = 0; r < 14; r++) entryRegs[r] = g->gpr[r];
    u64 keyOther = 0xCBF29CE484222325ull;
    for (int f = 1; f <= 8; f++) {
        u64 a, b;
        memcpy(&a, &g->fpr[f].ps0, 8);
        memcpy(&b, &g->fpr[f].ps1, 8);
        keyOther = fnv(fnv(fnv(fnv(keyOther, (u32)a), (u32)(a >> 32)), (u32)b), (u32)(b >> 32));
    }
    for (int q = 0; q < 8; q++) keyOther = fnv(keyOther, g->spr[SPR_GQR0 + q]);
    keyOther = fnv(fnv(keyOther, g->fpscr), g->msr);
    u64 key = 0xCBF29CE484222325ull;
    for (int r = 1; r <= 10; r++) key = fnv(key, g->gpr[r]);
    key = fnv(key, g->gpr[13]);
    for (int f = 1; f <= 8; f++) {
        u64 a, b;
        memcpy(&a, &g->fpr[f].ps0, 8);
        memcpy(&b, &g->fpr[f].ps1, 8);
        key = fnv(fnv(fnv(fnv(key, (u32)a), (u32)(a >> 32)), (u32)b), (u32)(b >> 32));
    }
    for (int q = 0; q < 8; q++) key = fnv(key, g->spr[SPR_GQR0 + q]);
    key = fnv(fnv(key, g->fpscr), g->msr);
    g_nrd = g_nwr = g_pipe = g_flags = 0;
    g_sp = g->gpr[1];
    g_memoRec = (void*)1;
    int why = 0;
    u32 n = gekko_run_call(g, ret, g_sp, 4000000u, &why);
    g_memoRec = NULL;
    for (u32 i = 0; i < g_nSeenUsed; i++) g_seen[g_seenUsed[i]].state = 0;
    g_nSeenUsed = 0;
    st->insns += n;
    if (why) { st->abort[why < 4 ? why : 3]++; return n ? n : 1; }
    if (g_flags & MF_MMIO) { st->abort[0]++; return n; }
    if (g_flags & MF_FULL) { st->abort[3]++; return n; }
    st->recorded++;
    st->reads += g_nrd;
    st->writes += g_nwr;
    st->pipe += g_pipe;
    u64 h = key;
    for (u32 i = 0; i < g_nrd; i++) h = fnv(fnv(h, g_rd[i][0]), g_rd[i][1]);
    u32 regs[12];
    for (int r = 1; r <= 10; r++) regs[r - 1] = entryRegs[r];
    regs[10] = entryRegs[13];
    regs[11] = (u32)(keyOther ^ (keyOther >> 32));
    Prev* p = NULL;
    for (u32 i = 0; i < KEEP; i++) if (g_prev[i].r3 == r3 && g_prev[i].n) p = &g_prev[i];
    if (p && p->hash == h && p->n == g_nrd) st->hits++;
    else if (p) {
        st->misses++;
        bool regDiff = false;
        for (int k = 0; k < 12; k++) if (p->regs[k] != regs[k]) { g_memoDiag[4 + k]++; regDiff = true; }
        if (regDiff) g_memoDiag[0]++;
        if (p->n != g_nrd) g_memoDiag[1]++;
        else {
            u32 diff = 0;
            for (u32 i = 0; i < g_nrd && i < PREV_MAX; i++) {
                if (p->rd[i][0] == g_rd[i][0] && p->rd[i][1] == g_rd[i][1]) continue;
                diff++;
                u32 a = g_rd[i][0], k;
                for (k = 0; k < DIFF_N && g_memoDiff[k][1] && g_memoDiff[k][0] != a; k++) {}
                if (k < DIFF_N) { g_memoDiff[k][0] = a; g_memoDiff[k][1]++; g_memoDiff[k][2] = g_rd[i][1]; }
            }
            if (diff) { g_memoDiag[2]++; g_memoDiag[3] += diff; }
        }
    } else {
        p = &g_prev[g_prevNext++ % KEEP];
        p->r3 = r3;
        st->firsts++;
    }
    p->hash = h;
    p->n = g_nrd;
    memcpy(p->regs, regs, sizeof regs);
    memcpy(p->rd, g_rd, (g_nrd < PREV_MAX ? g_nrd : PREV_MAX) * 8);
    return n;
}
