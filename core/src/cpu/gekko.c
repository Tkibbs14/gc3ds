// Gekko core: lifetime, run loop, exceptions, MSR/SPR side effects, timebase and decrementer.
//
// References: IBM PowerPC 750CL RISC Microprocessor User's Manual (chapter 4, Exceptions; chapter 2, registers),
// PowerPC Programming Environments Manual for 32-bit implementations (PEM; 6.x exception processing).
#include "gekko_internal.h"
#include <stdio.h>
#include <stdlib.h>

void gk_map_init(Gekko* g);
const u8* gk_fetch_host(Gekko* g, u32 blk);

#define GEKKO_PVR 0x00083214u  // Gekko (as reported on retail GameCube units)
#define TB_DIV 12u

// ---- Timebase / decrementer ----
// TB = cycles / 12 + tbOffset. DEC counts down on the same ticks: DEC = decZeroAt/12 - cycles/12 (mod 2^32), where
// decZeroAt is always a multiple of 12. The decrementer exception is signalled when DEC bit 0 goes 0 -> 1 (PEM 6.4.9),
// i.e. on the tick after DEC reaches 0 and, if nobody rewrites DEC, again every 2^32 ticks.
u64 gk_timebase(const Gekko* g) { return g->cycles / TB_DIV + g->tbOffset; }

static u32 get_dec(const Gekko* g) { return (u32)(g->decZeroAt / TB_DIV - g->cycles / TB_DIV); }

static void recompute_dec_edge(Gekko* g) {
    u32 d = get_dec(g);
    u32 k = d + 1;  // ticks until DEC reads 0xFFFFFFFF
    GKP(g)->nextDecEdge = (g->cycles / TB_DIV + (k ? (u64)k : (1ull << 32))) * TB_DIV;
}

static void set_dec(Gekko* g, u32 v) {
    u32 old = get_dec(g);
    g->decZeroAt = (g->cycles / TB_DIV + v) * TB_DIV;
    recompute_dec_edge(g);
    if (!(old & 0x80000000u) && (v & 0x80000000u)) GKP(g)->decPending = true;  // software-made 0 -> 1 transition
    GKP(g)->brk = true;
}

static void check_dec_edge(Gekko* g) {
    GkPriv* p = GKP(g);
    if (g->cycles >= p->nextDecEdge) {
        p->decPending = true;
        recompute_dec_edge(g);
    }
}

// ---- Lifetime ----
bool gekko_init(Gekko* g, const GekkoBus* bus) {
    memset(g, 0, sizeof *g);
    g->bus = bus;
    g->priv = calloc(1, sizeof(GkPriv));
    if (!g->priv) return false;
    gk_decode_init();
    gekko_reset(g);
    return true;
}

void gekko_free(Gekko* g) {
    free(g->priv);
    g->priv = NULL;
}

void gekko_reset(Gekko* g) {
    GkPriv* p = GKP(g);
    const GekkoBus* bus = g->bus;
    u32 compat = p->compat;
    GekkoJitFn jit = p->jit;  // an attached JIT stays attached across resets (it drops its own blocks)
    void* jitCtx = p->jitCtx;
    memset(g->gpr, 0, sizeof g->gpr);
    memset(g->fpr, 0, sizeof g->fpr);
    memset(g->sr, 0, sizeof g->sr);
    memset(g->spr, 0, sizeof g->spr);
    g->cr = g->xer = g->lr = g->ctr = g->fpscr = 0;
    g->msr = MSR_IP;  // 750CL 4.6.1: hard reset sets MSR[IP], everything else cleared; vector 0xFFF00100
    g->pc = 0xFFF00100u;
    g->cycles = 0;
    g->downcount = 0;
    g->tbOffset = 0;
    g->decZeroAt = 0xFFFFFFFFull * TB_DIV;  // DEC = 0xFFFFFFFF (undefined at power-on; chosen to fire late)
    g->extIrq = false;
    g->spr[SPR_PVR] = GEKKO_PVR;
    memset(p, 0, sizeof *p);
    p->compat = compat;
    p->jit = jit;
    p->jitCtx = jitCtx;
    p->fetchBlk = 0xFFFFFFFFu;
    g->bus = bus;
    gk_map_init(g);
    recompute_dec_edge(g);
}

void gekko_set_compat(Gekko* g, u32 flags) { GKP(g)->compat = flags; }

// ---- Exceptions ----
// 750CL Table 4-? / PEM 6.4: SRR0 gets the restart address; SRR1[16-31] gets MSR[16-31] and SRR1[0-15] carries
// exception-specific bits. The new MSR clears POW, EE, PR, FP, FE0, SE, BE, FE1, IR, DR, PM and RI, keeps ME and IP,
// copies ILE into LE; machine check also clears ME. The vector base is 0xFFF00000 when MSR[IP] = 1.
void gk_exception(Gekko* g, u32 vector, u32 srr0, u32 srr1Bits) {
    GkPriv* p = GKP(g);
    g->spr[SPR_SRR0] = srr0;
    g->spr[SPR_SRR1] = (g->msr & 0x0000FFFFu) | srr1Bits;
    u32 msr = g->msr & (MSR_ME | MSR_IP | MSR_ILE);
    if (msr & MSR_ILE) msr |= MSR_LE;
    if (vector == EXC_MACHINE_CHECK) msr &= ~MSR_ME;
    gk_set_msr(g, msr);
    p->npc = ((msr & MSR_IP) ? 0xFFF00000u : 0) | vector;
    p->brk = true;
}

void gk_program_exception(Gekko* g, u32 reason) { gk_exception(g, EXC_PROGRAM, GKP(g)->curPc, reason); }

void gk_illegal(Gekko* g, u32 inst) {
    char buf[96], msg[160];
    gekko_disasm(GKP(g)->curPc, inst, buf, sizeof buf);
    snprintf(msg, sizeof msg, "illegal/unimplemented instruction %08x at %08x: %s", inst, GKP(g)->curPc, buf);
    gekko_on_fatal(g, msg);
    gk_program_exception(g, SRR1_ILLEGAL);
}

void gk_set_msr(Gekko* g, u32 msr) {
    u32 old = g->msr;
    g->msr = msr;
    if ((old ^ msr) & (MSR_IR | MSR_DR | MSR_PR)) gk_update_maps(g);
    if ((msr & MSR_LE) && !(old & MSR_LE)) gekko_on_fatal(g, "MSR[LE] set: little-endian mode is not supported");
    GKP(g)->brk = true;  // EE may have been enabled: re-check pending interrupts before the next instruction
}

void gekko_set_msr(Gekko* g, u32 msr) { gk_set_msr(g, msr); }

void gekko_set_external_irq(Gekko* g, bool level) {
    g->extIrq = level;
    if (level) GKP(g)->brk = true;
}

void gekko_stop(Gekko* g) {
    GKP(g)->stopReq = true;
    GKP(g)->brk = true;
}

bool gekko_translate_data(Gekko* g, u32 ea, u32* pa) { return gk_translate(g, ea, false, true, pa, false); }

// ---- SPRs ----
static void lc_dma(Gekko* g) {
    // 750CL "Locked cache DMA": DMA_U = MEM_ADDR[0:26] | DMA_LEN_U[27:31]; DMA_L = LC_ADDR[0:26] | DMA_LD[27] |
    // DMA_LEN_L[28:29] | DMA_T[30] | DMA_F[31]. Length is in 32-byte lines, 0 meaning 128. LD = 1 loads the locked cache
    // from memory. Transfers complete immediately here, so the queue length in HID2 always reads 0.
    u32 u = g->spr[SPR_DMAU], l = g->spr[SPR_DMAL];
    u32 lines = ((u & 0x1F) << 2) | ((l >> 2) & 3);
    if (!lines) lines = 128;
    bool load = (l & 0x10) != 0;
    if (g->bus->lc_dma && (g->spr[SPR_HID2] & HID2_LCE))
        g->bus->lc_dma(g->bus->ctx, !load, u & ~0x1Fu, l & ~0x1Fu, lines * 32);
    else
        emu_log(LOG_CPU, "locked-cache DMA ignored (HID2.LCE=0 or no bus hook) DMA_U=%08x DMA_L=%08x\n", u, l);
    g->spr[SPR_DMAL] = l & ~3u;
}

u32 gk_mfspr(Gekko* g, u32 spr) {
    switch (spr) {
    case SPR_XER: return g->xer;
    case SPR_LR: return g->lr;
    case SPR_CTR: return g->ctr;
    case SPR_DEC: return get_dec(g);
    case SPR_TBL_R: case SPR_TBL_W: return (u32)gk_timebase(g);
    case SPR_TBU_R: case SPR_TBU_W: return (u32)(gk_timebase(g) >> 32);
    case SPR_HID2: return g->spr[SPR_HID2] & ~HID2_DMAQL;
    case SPR_WPAR: return g->spr[SPR_WPAR] & ~1u;  // BNE: the gather pipe is drained by the system immediately
    default: return g->spr[spr & 1023];
    }
}

u32 gekko_get_spr(Gekko* g, u32 spr) { return gk_mfspr(g, spr); }

void gk_mtspr(Gekko* g, u32 spr, u32 v) {
    spr &= 1023;
    switch (spr) {
    case SPR_XER: g->xer = v & XER_MASK; return;
    case SPR_LR: g->lr = v; return;
    case SPR_CTR: g->ctr = v; return;
    case SPR_DEC: set_dec(g, v); return;
    case SPR_TBL_W: {
        u64 tb = (gk_timebase(g) & 0xFFFFFFFF00000000ull) | v;
        g->tbOffset = tb - g->cycles / TB_DIV;
        return;
    }
    case SPR_TBU_W: {
        u64 tb = (gk_timebase(g) & 0xFFFFFFFFull) | ((u64)v << 32);
        g->tbOffset = tb - g->cycles / TB_DIV;
        return;
    }
    case SPR_PVR: case SPR_TBL_R: case SPR_TBU_R: return;  // read-only
    case SPR_HID2: g->spr[SPR_HID2] = v & ~HID2_DMAQL; return;
    case SPR_DMAL:
        g->spr[SPR_DMAL] = v;
        if (v & 2) lc_dma(g);
        else if (v & 1) g->spr[SPR_DMAL] = v & ~1u;  // flush with nothing queued
        return;
    case SPR_L2CR: g->spr[SPR_L2CR] = v & ~1u; return;  // L2IP (invalidate in progress) always reads 0
    default: break;
    }
    g->spr[spr] = v;
    if (spr >= SPR_IBAT0U && spr < SPR_IBAT0U + 8) {
        gk_map_invalidate(g, true, false);
        gk_update_maps(g);
    } else if (spr >= SPR_DBAT0U && spr < SPR_DBAT0U + 8) {
        gk_map_invalidate(g, false, true);
        gk_update_maps(g);
    }
}

void gekko_set_spr(Gekko* g, u32 spr, u32 v) { gk_mtspr(g, spr, v); }

// ---- Run loop ----
static void take_interrupts(Gekko* g) {
    GkPriv* p = GKP(g);
    check_dec_edge(g);
    if (!(g->msr & MSR_EE)) return;
    // PEM 6.1.2 priority: external interrupt before decrementer.
    if (g->extIrq) {
        gk_exception(g, EXC_EXTERNAL, g->pc, 0);
        g->pc = p->npc;
    } else if (p->decPending) {
        p->decPending = false;
        gk_exception(g, EXC_DECREMENTER, g->pc, 0);
        g->pc = p->npc;
    }
}

static bool fetch_refill(Gekko* g, u32 pc) {
    GkPriv* p = GKP(g);
    u32 pa;
    p->curPc = pc;
    if (!gk_translate(g, pc, false, false, &pa, true)) return false;
    p->fetchBlk = pc >> GK_BLK_SHIFT;
    p->fetchHost = gk_fetch_host(g, p->fetchBlk);
    p->fetchPa = pa & ~GK_BLK_MASK;
    return true;
}

void gekko_set_jit(Gekko* g, GekkoJitFn fn, void* ctx) {
    GKP(g)->jit = fn;
    GKP(g)->jitCtx = ctx;
}

u32 gekko_interp_one(Gekko* g, u32 pc, u32 inst, bool* stop) {
    GkPriv* p = GKP(g);
    p->brk = false;  // report only what this instruction causes (callers leave compiled code whenever brk is set)
    p->curPc = pc;
    p->npc = pc + 4;
    gk_prim[inst >> 26](g, inst);
    *stop = p->brk || p->stopReq;
    return p->npc;
}

u32 gekko_run(Gekko* g, u32 cycles) {
    GkPriv* p = GKP(g);
    u64 start = g->cycles;
    g->downcount = cycles;
    p->stopReq = false;
    recompute_dec_edge(g);
    while (g->downcount > 0 && !p->stopReq) {
        p->brk = false;
        take_interrupts(g);
        u64 limit = p->nextDecEdge;
        for (;;) {
            if (p->jit) {
                // The JIT runs compiled blocks starting at g->pc, at most `budget` cycles' worth (a block may run up
                // to one block past it), and returns how many instructions it executed; 0 means "interpret this one".
                s64 budget = g->downcount;
                if ((s64)(limit - g->cycles) < budget) budget = (s64)(limit - g->cycles);
                u32 n = p->jit(g, budget, p->jitCtx);
                if (n) {
                    g->cycles += n;
                    g->downcount -= n;
                    if (g->downcount <= 0 || p->brk || p->stopReq || g->cycles >= limit) break;
                    continue;
                }
            }
            u32 pc = g->pc;
            u32 inst;
            if ((pc >> GK_BLK_SHIFT) != p->fetchBlk && !fetch_refill(g, pc)) {
                g->pc = p->npc;  // ISI taken
                g->cycles++;
                g->downcount--;
                break;
            }
            if (p->fetchHost) inst = gk_ld32(p->fetchHost + (pc & GK_BLK_MASK));
            else inst = g->bus->fetch(g->bus->ctx, p->fetchPa | (pc & GK_BLK_MASK));
            // Idle loop: "b ." with interrupts enabled can only be left by an interrupt, and nothing changes until
            // the next scheduled event or decrementer edge, so time jumps there instead of spinning.
            if (inst == 0x48000000u && (g->msr & MSR_EE)) {
                s64 skip = g->downcount;
                if ((s64)(limit - g->cycles) < skip) skip = (s64)(limit - g->cycles);
                if (skip < 1) skip = 1;
                g->cycles += (u64)skip;
                g->downcount -= skip;
                break;
            }
            p->curPc = pc;
            p->npc = pc + 4;
            gk_prim[inst >> 26](g, inst);
            g->pc = p->npc;
            g->cycles++;
            if (--g->downcount <= 0 || p->brk || g->cycles >= limit) break;
        }
        check_dec_edge(g);
    }
    return (u32)(g->cycles - start);
}
