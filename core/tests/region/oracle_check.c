// Supplemental synthetic checks of the fixed X4 assembly, using code only from the user's external MEM1 dump.
// No commercial code or captured state is embedded here. Device real-call checks gate performance results.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/sys/system.h"
#include "../../src/cpu/gekko_internal.h"
#include "../../src/jit/jit.h"

extern void x4_pos(Gekko*, u8*), x4_tex(Gekko*, u8*), x4_pane(Gekko*, u8*);
extern void (*gp_capture)(const u8*, u32);
static System s;
static u8 pipebuf[262144];
static u32 pn;

static void cap(const u8* p, u32 n) {
    if (n > sizeof pipebuf - pn) exit(2);
    memcpy(pipebuf + pn, p, n);
    pn += n;
}
void x4_pipe_word(Gekko* g, u32 v, u32 n) {
    (void)g;
    u8 a[4];
    for (u32 i = 0; i < n; i++) a[i] = v >> (8 * (n - i - 1));
    cap(a, n);
}
void x4_call(Gekko* g, u32 target, u32 ret) {
    int why;
    g->pc = target;
    g->lr = ret;
    gekko_run_call(g, ret, g->gpr[1], 1000000, &why);
    if (why) {
        fprintf(stderr, "callee %08x failed %d pc %08x\n", target, why, g->pc);
        exit(2);
    }
}
static void word(u32 a, u32 v) { ram_wr32(s.mem1, a & 0x3fffffff, v); }

static void setup(Gekko* g, int which, bool full, bool parent, bool children) {
    gekko_set_spr(g, SPR_IBAT0U, 0x80001fff);
    gekko_set_spr(g, SPR_IBAT0U + 1, 2);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001fff);
    gekko_set_spr(g, SPR_DBAT0U + 1, 2);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xc0001fff);
    gekko_set_spr(g, SPR_DBAT0U + 3, 0x2a);
    gekko_set_msr(g, MSR_FP | MSR_IR | MSR_DR);
    gekko_set_spr(g, SPR_HID2, HID2_LSQE | HID2_PSE);
    jit_host_fpscr_init();
    for (int r = 0; r < 32; r++) g->gpr[r] = r;
    g->gpr[1] = 0x81000000;
    g->gpr[2] = 0x80450000;
    g->gpr[13] = 0x80460000;
    g->lr = 0x80001000;
    g->pc = which == 3 ? 0x8036024c : which == 2 ? 0x8035e298 : 0x802f67e0;
    g->gpr[3] = 0x80500000;
    g->gpr[4] = which == 3 ? 3 : 0x80501000;
    g->gpr[5] = 2;
    g->gpr[6] = 1;
    for (int i = 0; i < 16; i++) word(0x80500000 + i * 4, 0x3f800000 + i * 0x1000);
    word(g->gpr[2] - 13440, 0x80502000);
    s.mem1[0x50001f] = 2;
    if (which != 1) return;
    word(0x80500000, 0x80501000);
    word(0x80501000, 0x80503000);
    word(0x80503020, 0x80003000);
    word(0x80003000, 0x38600001);  // synthetic li r3,1; blr leaf
    word(0x80003004, 0x4e800020);
    word(0x805000ec, 0);
    word(0x80500020, 0x40000000);
    word(0x80500028, 0x3f800000);  // inverted bounds exercise the complete early-return region
    if (!full) return;
    word(0x80500028, 0x40800000);
    word(0x80500024, 0);
    word(0x8050002c, 0x40800000);
    word(0x805000d4, 0);
    word(0x805000d8, 0);
    word(0x805000dc, 0);
    s.mem1[0x5000b0] = 1;
    s.mem1[0x5000b2] = 128;
    g->gpr[5] = 0;
    word(0x80501044, 0x80003010);
    word(0x80501038, 0x80003010);
    word(0x80501058, 0x80003010);
    word(0x80003010, 0x4e800020);  // synthetic empty virtual methods
    word(g->gpr[2] - 14352, 0);
    for (int i = 0; i < 12; i++) word(0x80500050 + i * 4, 0x3f800000);
    if (!parent) return;
    word(0x805000ec, 0x80504000);
    word(0x8050400c, 0x80505000);
    for (int i = 0; i < 12; i++) word(0x80505080 + i * 4, 0x3f800000 + i * 0x2000);
    for (int i = 0x20; i < 0x50; i += 4) word(0x80505000 + i, 0);
    if (children) {
        word(0x805000dc, 0x8050600c);
        word(0x8050600c, 0x80507000);
        word(0x80506018, 0);
        memset(s.mem1 + 0x507000, 0, 256);
        word(0x80507000, 0x80501000);
        word(0x80507020, 0x40000000);
        word(0x80507028, 0x3f800000);
    }
}
int main(int argc, char** argv) {
    if (argc < 3 || !sys_init(&s)) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f || fread(s.mem1, 1, 0x1800000, f) != 0x1800000) return 2;
    fclose(f);
    Gekko* g = &s.cpu;
    int which = atoi(argv[2]);
    setup(g, which, argc > 3, argc > 4, argc > 5);
    GkPriv* priv = malloc(sizeof *priv);
    u8 *before = malloc(0x1800000), *after = malloc(0x1800000), *wantPipe = malloc(sizeof pipebuf);
    if (!priv || !before || !after || !wantPipe) return 2;
    *priv = *GKP(g);
    Gekko in = *g;
    memcpy(before, s.mem1, 0x1800000);
    gp_capture = cap;
    GKP(g)->stopReq = GKP(g)->brk = false;
    int why;
    u32 n = gekko_run_call(g, g->lr, g->gpr[1], 1000000, &why);
    Gekko want = *g;
    memcpy(after, s.mem1, 0x1800000);
    u32 wantN = pn;
    memcpy(wantPipe, pipebuf, pn);
    memcpy(s.mem1, before, 0x1800000);
    *g = in;
    *GKP(g) = *priv;
    GKP(g)->stopReq = false;
    pn = 0;
    (which == 3 ? x4_pos : which == 2 ? x4_tex : x4_pane)(g, s.mem1);
    printf("case %d guest %u why %d pc %08x/%08x sp %08x/%08x pipe %u/%u\n",
           which, n, why, g->pc, want.pc, g->gpr[1], want.gpr[1], pn, wantN);
    int bad = why || g->pc != want.pc || pn != wantN || memcmp(wantPipe, pipebuf, pn) ||
              memcmp(g->gpr + 13, want.gpr + 13, 19 * 4) ||
              memcmp(g->fpr + 14, want.fpr + 14, 18 * sizeof(FPR));
    for (u32 a = 0; a < 0x1800000; a++) {
        u32 sp = in.gpr[1] & 0x3fffffff;
        u32 dead = which == 1 ? (argc > 5 ? 544 : 336) : which == 2 ? 64 : 0;
        if (a >= sp - dead && a < sp) continue;  // dead callee stack, as in port_check
        if (s.mem1[a] == after[a]) continue;
        printf("mem %08x got %02x want %02x\n", a, s.mem1[a], after[a]);
        if (++bad > 10) break;
    }
    printf("bad %d\n", bad);
    if (!bad) {
        g->gpr[14] ^= 1;
        bool caught = memcmp(g->gpr + 13, want.gpr + 13, 19 * 4) != 0;
        printf("plant %s\n", caught ? "CAUGHT" : "MISSED");
        if (!caught) bad++;
    }
    return bad ? 1 : 0;
}
