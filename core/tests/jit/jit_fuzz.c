// JIT differential fuzzer: random single instructions of every kind the JIT translates, executed by the JIT on one
// machine and by the interpreter on another from identical random state; registers and memory must match exactly.
// Runs as an ARM Linux program under qemu-arm (tests/jit/run_jit_fuzz.sh).
//   jit_fuzz [cases] [seed]        JIT_FUZZ_PLANT=1 corrupts one JIT result to prove the comparison can fail.
#include <stdio.h>
#include <stdlib.h>
#include "../../src/jit/jit.h"
#include "../../src/sys/system.h"

static System A, B;
static Jit* jit;
static u64 rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 16); }
static u32 rbits(u32 n) { return rnd() & ((1u << n) - 1); }

// The instruction is placed at a random word in this page, so PC-derived encodings (bit 2 and up) vary.
static u32 CODE_PA = 0x3000u;
#define CODE_EA (0x80000000u | CODE_PA)
#define WIN_PA 0x100000u
#define WIN_SIZE 0x10000u

static u32 interesting(void) {
    static const u32 v[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x8000, 0x7FFF, 0x10000, 31, 32, 33, 63 };
    switch (rnd() % 4) {
    case 0: return v[rnd() % (sizeof v / sizeof v[0])];
    case 1: return rnd() & 0xFF;
    case 2: return (u32)-(s32)(rnd() & 0xFF);
    default: return rnd() << 16 | rnd();
    }
}

static void boot_like(System* s) {
    Gekko* g = &s->cpu;
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF); gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);
    gekko_set_msr(g, MSR_FP | MSR_IR | MSR_DR | MSR_RI);
}

// Random instruction from the translated set; may set up registers it needs (base addresses) in the state.
typedef struct { u32 gpr[32], cr, xer, lr, ctr; } St;

static u32 load_base(St* st, u32 ra, u32 size) {
    u32 off = rnd() % (WIN_SIZE - 0x400) + 0x200;
    if (rnd() % 8) off &= ~(size - 1);       // mostly aligned (fast path), sometimes not (slow path)
    st->gpr[ra] = ((rnd() & 1) ? 0x80000000u : 0xC0000000u) | (WIN_PA + off);
    return st->gpr[ra];
}

static u32 gen(St* st) {
    u32 rd = rbits(5), ra = rbits(5), rb = rbits(5), rc = rbits(1);
    u32 imm = rnd() & 0xFFFF;
    for (;;) {
        switch (rnd() % 16) {
        case 0: {  // D-form arithmetic / logic / compare
            static const u32 ops[] = { 14, 15, 12, 13, 8, 7, 24, 25, 26, 27, 28, 29, 10, 11 };
            u32 op = ops[rnd() % 14];
            if (op == 10 || op == 11) rd = rbits(3) << 2;  // crfD, L = 0
            return (op << 26) | (rd << 21) | (ra << 16) | imm;
        }
        case 1: case 2: {  // XO / X-form group 31
            static const u32 xos[] = { 266, 10, 138, 202, 40, 8, 136, 104, 28, 60, 444, 412, 316, 124, 476, 284, 24, 536,
                                       824, 26, 954, 922, 0, 32, 235, 75, 11, 19, 144 };
            u32 xo = xos[rnd() % (sizeof xos / sizeof xos[0])];
            if (xo == 0 || xo == 32) { rd = rbits(3) << 2; rc = 0; }
            if (xo == 19) { ra = rb = 0; rc = 0; }
            if (xo == 144) return (31u << 26) | (rd << 21) | (rbits(8) << 12) | (144u << 1);
            if (xo == 104 || xo == 202 || xo == 26 || xo == 954 || xo == 922) rb = 0;
            return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
        }
        case 3: {  // rotates
            static const u32 ops[] = { 21, 20, 23 };
            u32 op = ops[rnd() % 3];
            return (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (rbits(5) << 6) | (rbits(5) << 1) | rc;
        }
        case 4: case 5: {  // D-form loads / stores
            static const u32 ops[] = { 32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45 };
            u32 op = ops[rnd() % 14];
            u32 size = op <= 33 || op == 36 || op == 37 ? 4 : (op <= 35 || op == 38 || op == 39) ? 1 : 2;
            ra = 1 + rnd() % 31;
            if ((op & 1) && rd == ra) rd = (ra + 1) & 31;
            load_base(st, ra, size);
            u32 d = (u32)(s32)((s32)(rnd() % 512) - 256);
            if (rnd() % 8) d &= ~(size - 1);
            return (op << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF);
        }
        case 6: {  // indexed loads / stores
            static const u32 xos[] = { 23, 87, 279, 343, 55, 119, 311, 375, 151, 215, 407, 183, 247, 439 };
            u32 xo = xos[rnd() % 14];
            ra = 1 + rnd() % 31;
            rb = 1 + rnd() % 31;
            if (rb == ra) rb = (ra % 31) + 1;
            bool upd = xo == 55 || xo == 119 || xo == 311 || xo == 375 || xo == 183 || xo == 247 || xo == 439;
            if (upd && rd == ra) rd = (ra + 2) & 31;
            load_base(st, ra, 4);
            st->gpr[rb] = rnd() % 256;
            if (rnd() % 8) st->gpr[rb] &= ~3u;
            return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1);
        }
        case 7: {  // mfspr / mtspr LR, CTR, XER
            static const u32 sprs[] = { 1, 8, 9 };
            u32 spr = sprs[rnd() % 3];
            u32 xo = (rnd() & 1) ? 339 : 467;
            return (31u << 26) | (rd << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (xo << 1);
        }
        case 8: case 9: {  // conditional branch (bc) with every BO class
            static const u32 bos[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20 };
            u32 bo = bos[rnd() % 11], bi = rbits(5);
            if (rnd() % 3 == 0) st->ctr = rnd() % 3;
            s32 bd = ((s32)(rnd() % 64) - 32) * 4;
            return (16u << 26) | (bo << 21) | (bi << 16) | ((u32)bd & 0xFFFC) | (rnd() & 1);
        }
        case 10: {  // b / bl / ba
            s32 li = ((s32)(rnd() % 4096) - 2048) * 4;
            return (18u << 26) | ((u32)li & 0x03FFFFFC) | (rnd() & 1) | ((rnd() % 8 == 0) ? 2u : 0u);
        }
        case 11: {  // bclr / bcctr
            static const u32 bos[] = { 4, 12, 20, 0, 2, 8, 16 };
            bool toCtr = rnd() & 1;
            u32 bo = bos[rnd() % (toCtr ? 3 : 7)], bi = rbits(5);
            if (rnd() % 3 == 0) st->ctr = rnd() % 3;
            return (19u << 26) | (bo << 21) | (bi << 16) | ((toCtr ? 528u : 16u) << 1) | (rnd() & 1);
        }
        default:
            continue;
        }
    }
}

static bool compare(u32 inst, int n) {
    Gekko* a = &A.cpu;
    Gekko* b = &B.cpu;
    bool ok = true;
    char dis[96];
    gekko_disasm(CODE_EA, inst, dis, sizeof dis);
    for (int r = 0; r < 32; r++)
        if (a->gpr[r] != b->gpr[r]) { if (ok) printf("MISMATCH %08x  %s\n", inst, dis); printf("  r%d jit %08x interp %08x\n", r, a->gpr[r], b->gpr[r]); ok = false; }
#define CHK(f) if (a->f != b->f) { if (ok) printf("MISMATCH %08x  %s\n", inst, dis); printf("  " #f " jit %08x interp %08x\n", a->f, b->f); ok = false; }
    CHK(cr) CHK(xer) CHK(lr) CHK(ctr) CHK(pc)
#undef CHK
    if (memcmp(A.mem1 + WIN_PA, B.mem1 + WIN_PA, WIN_SIZE)) {
        if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
        for (u32 i = 0; i < WIN_SIZE; i++) if (A.mem1[WIN_PA + i] != B.mem1[WIN_PA + i]) { printf("  mem %08x jit %02x interp %02x\n", WIN_PA + i, A.mem1[WIN_PA + i], B.mem1[WIN_PA + i]); break; }
        ok = false;
    }
    if (n != 1) { if (ok) printf("MISMATCH %08x  %s\n", inst, dis); printf("  JIT executed %d instructions\n", n); ok = false; }
    return ok;
}

int main(int argc, char** argv) {
    u32 cases = argc > 1 ? (u32)atoi(argv[1]) : 20000;
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 0) * 0x2545F4914F6CDD1Dull;
    bool plant = getenv("JIT_FUZZ_PLANT") != NULL;
    if (!sys_init(&A) || !sys_init(&B)) { printf("init failed\n"); return 2; }
    boot_like(&A);
    boot_like(&B);
    jit = jit_create(&A.cpu, A.mem1, MEM1_SIZE, 4u << 20);
    if (!jit) { printf("jit_create failed\n"); return 2; }
    jit_debug_max_insns = 1;
    u32 fails = 0;
    for (u32 k = 0; k < cases && fails < 10; k++) {
        St st;
        for (int r = 0; r < 32; r++) st.gpr[r] = interesting();
        st.cr = rnd() << 16 | rnd();
        st.xer = (rnd() << 16 | rnd()) & 0xE000007Fu;
        st.lr = (rnd() << 16 | rnd()) & ~3u;
        st.ctr = rnd() << 16 | rnd();
        u32 inst = gen(&st);
        CODE_PA = 0x3000u + 4 * (rnd() % 1024);
        for (u32 i = 0; i < WIN_SIZE; i++) A.mem1[WIN_PA + i] = B.mem1[WIN_PA + i] = (u8)rnd();
        mem1_write32(&A, CODE_PA, inst);
        mem1_write32(&B, CODE_PA, inst);
        System* sys[2] = { &A, &B };
        for (int m = 0; m < 2; m++) {
            Gekko* g = &sys[m]->cpu;
            memcpy(g->gpr, st.gpr, sizeof st.gpr);
            g->cr = st.cr; g->xer = st.xer; g->lr = st.lr; g->ctr = st.ctr; g->pc = CODE_EA;
        }
        jit_invalidate_all(jit);
        int n = (int)jit_run_one_block(jit);
        if (plant && k == cases / 2) A.cpu.gpr[3] ^= 1;
        bool stop;
        B.cpu.pc = gekko_interp_one(&B.cpu, CODE_EA, inst, &stop);
        if (!compare(inst, n)) {
            if (!fails && !plant) {  // keep the first failing block for disassembly
                FILE* d = fopen("/tmp/jit_fail.bin", "wb");
                if (d) { fwrite(jit_debug_last_code, 4, jit_debug_last_words, d); fclose(d); }
            }
            fails++;
        }
    }
    printf("JIT FUZZ cases=%u fails=%u%s\n", cases, fails, plant ? " (planted)" : "");
    return fails ? 1 : 0;
}
