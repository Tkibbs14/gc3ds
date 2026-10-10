// Compact Gekko disassembler for traces and error messages. Output uses base (non-simplified) mnemonics and the
// operand order of the PowerPC manuals, in the same style as GNU objdump -M gekko,raw so the two can be compared.
#include "gekko_internal.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct { char* p; size_t n; } Out;

static void out(Out* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(Out* o, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->p, o->n, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    size_t w = (size_t)k < o->n ? (size_t)k : (o->n ? o->n - 1 : 0);
    o->p += w;
    o->n -= w;
}

static void mn(Out* o, const char* name, const char* suffix) {
    char m[24];
    snprintf(m, sizeof m, "%s%s", name, suffix);
    out(o, "%-7s ", m);
}

static const char* crbit_name(u32 b, char* buf) {
    static const char* n[4] = {"lt", "gt", "eq", "so"};
    if (b < 4) return n[b];
    snprintf(buf, 16, "4*cr%u+%s", b >> 2, n[b & 3]);
    return buf;
}

static const char* ra0(u32 a, char* buf) {
    if (!a) return "0";
    snprintf(buf, 8, "r%u", a);
    return buf;
}

static const char* rc(u32 i) { return (i & 1) ? "." : ""; }
static const char* oerc(u32 i) {
    static const char* s[4] = {"", ".", "o", "o."};
    return s[((i >> 9) & 2) | (i & 1)];
}

typedef struct { u16 xo; const char* name; u8 form; } Ent;
enum {
    F_XO3,     // rD,rA,rB with OE/Rc
    F_XO2,     // rD,rA with OE/Rc
    F_MULH,    // rD,rA,rB with Rc
    F_LOG3,    // rA,rS,rB with Rc
    F_LOG2,    // rA,rS with Rc
    F_LX,      // rD,rA,rB
    F_SX,      // rS,rA,rB
    F_CACHE,   // rA,rB
    F_FLX,     // fD,rA,rB
    F_FSX,     // fS,rA,rB
    F_NONE,
};

static const Ent op31[] = {
    {266, "add", F_XO3}, {10, "addc", F_XO3}, {138, "adde", F_XO3}, {234, "addme", F_XO2}, {202, "addze", F_XO2},
    {40, "subf", F_XO3}, {8, "subfc", F_XO3}, {136, "subfe", F_XO3}, {232, "subfme", F_XO2}, {200, "subfze", F_XO2},
    {104, "neg", F_XO2}, {235, "mullw", F_XO3}, {491, "divw", F_XO3}, {459, "divwu", F_XO3},
    {75, "mulhw", F_MULH}, {11, "mulhwu", F_MULH},
    {28, "and", F_LOG3}, {60, "andc", F_LOG3}, {444, "or", F_LOG3}, {412, "orc", F_LOG3}, {316, "xor", F_LOG3},
    {476, "nand", F_LOG3}, {124, "nor", F_LOG3}, {284, "eqv", F_LOG3}, {24, "slw", F_LOG3}, {536, "srw", F_LOG3},
    {792, "sraw", F_LOG3}, {954, "extsb", F_LOG2}, {922, "extsh", F_LOG2}, {26, "cntlzw", F_LOG2},
    {23, "lwzx", F_LX}, {55, "lwzux", F_LX}, {87, "lbzx", F_LX}, {119, "lbzux", F_LX}, {279, "lhzx", F_LX},
    {311, "lhzux", F_LX}, {343, "lhax", F_LX}, {375, "lhaux", F_LX}, {534, "lwbrx", F_LX}, {790, "lhbrx", F_LX},
    {20, "lwarx", F_LX}, {533, "lswx", F_LX}, {310, "eciwx", F_LX},
    {151, "stwx", F_SX}, {183, "stwux", F_SX}, {215, "stbx", F_SX}, {247, "stbux", F_SX}, {407, "sthx", F_SX},
    {439, "sthux", F_SX}, {662, "stwbrx", F_SX}, {918, "sthbrx", F_SX}, {661, "stswx", F_SX}, {438, "ecowx", F_SX},
    {54, "dcbst", F_CACHE}, {86, "dcbf", F_CACHE}, {470, "dcbi", F_CACHE}, {246, "dcbtst", F_CACHE},
    {278, "dcbt", F_CACHE}, {982, "icbi", F_CACHE}, {1014, "dcbz", F_CACHE},
    {535, "lfsx", F_FLX}, {567, "lfsux", F_FLX}, {599, "lfdx", F_FLX}, {631, "lfdux", F_FLX},
    {663, "stfsx", F_FSX}, {695, "stfsux", F_FSX}, {727, "stfdx", F_FSX}, {759, "stfdux", F_FSX},
    {983, "stfiwx", F_FSX},
    {598, "sync", F_NONE}, {854, "eieio", F_NONE}, {566, "tlbsync", F_NONE},
};

static const char* dname[64] = {
    [7] = "mulli", [8] = "subfic", [12] = "addic", [13] = "addic.", [14] = "addi", [15] = "addis",
    [24] = "ori", [25] = "oris", [26] = "xori", [27] = "xoris", [28] = "andi.", [29] = "andis.",
    [32] = "lwz", [33] = "lwzu", [34] = "lbz", [35] = "lbzu", [36] = "stw", [37] = "stwu", [38] = "stb",
    [39] = "stbu", [40] = "lhz", [41] = "lhzu", [42] = "lha", [43] = "lhau", [44] = "sth", [45] = "sthu",
    [46] = "lmw", [47] = "stmw", [48] = "lfs", [49] = "lfsu", [50] = "lfd", [51] = "lfdu", [52] = "stfs",
    [53] = "stfsu", [54] = "stfd", [55] = "stfdu",
};

static bool dis31(Out* o, u32 i) {
    u32 xo = (i >> 1) & 0x3FF, xo9 = xo & 0x1FF;
    u32 d = I_RD(i), a = I_RA(i), b = I_RB(i);
    char t1[16];
    for (size_t k = 0; k < sizeof op31 / sizeof op31[0]; k++) {
        const Ent* e = &op31[k];
        bool isXO = e->form == F_XO3 || e->form == F_XO2;
        if (isXO ? (xo9 != e->xo) : (xo != e->xo)) continue;
        switch (e->form) {
        case F_XO3: mn(o, e->name, oerc(i)); out(o, "r%u,r%u,r%u", d, a, b); return true;
        case F_XO2: mn(o, e->name, oerc(i)); out(o, "r%u,r%u", d, a); return true;
        case F_MULH: mn(o, e->name, rc(i)); out(o, "r%u,r%u,r%u", d, a, b); return true;
        case F_LOG3: mn(o, e->name, rc(i)); out(o, "r%u,r%u,r%u", a, d, b); return true;
        case F_LOG2: mn(o, e->name, rc(i)); out(o, "r%u,r%u", a, d); return true;
        case F_LX: mn(o, e->name, ""); out(o, "r%u,%s,r%u", d, ra0(a, t1), b); return true;
        case F_SX: mn(o, e->name, ""); out(o, "r%u,%s,r%u", d, ra0(a, t1), b); return true;
        case F_CACHE: mn(o, e->name, ""); out(o, "%s,r%u", ra0(a, t1), b); return true;
        case F_FLX: case F_FSX: mn(o, e->name, ""); out(o, "f%u,%s,r%u", d, ra0(a, t1), b); return true;
        default: out(o, "%s", e->name); return true;
        }
    }
    switch (xo) {
    case 0: mn(o, "cmp", ""); out(o, "cr%u,%u,r%u,r%u", I_CRFD(i), (i >> 21) & 1, a, b); return true;
    case 32: mn(o, "cmpl", ""); out(o, "cr%u,%u,r%u,r%u", I_CRFD(i), (i >> 21) & 1, a, b); return true;
    case 4: mn(o, "tw", ""); out(o, "%u,r%u,r%u", d, a, b); return true;
    case 19: mn(o, "mfcr", ""); out(o, "r%u", d); return true;
    case 83: mn(o, "mfmsr", ""); out(o, "r%u", d); return true;
    case 146: mn(o, "mtmsr", ""); out(o, "r%u", d); return true;
    case 144: mn(o, "mtcrf", ""); out(o, "%u,r%u", (i >> 12) & 0xFF, d); return true;
    case 150: mn(o, "stwcx.", ""); out(o, "r%u,%s,r%u", d, ra0(a, t1), b); return true;
    case 210: mn(o, "mtsr", ""); out(o, "%u,r%u", a & 15, d); return true;
    case 595: mn(o, "mfsr", ""); out(o, "r%u,%u", d, a & 15); return true;
    case 242: mn(o, "mtsrin", ""); out(o, "r%u,r%u", d, b); return true;
    case 659: mn(o, "mfsrin", ""); out(o, "r%u,r%u", d, b); return true;
    case 306: mn(o, "tlbie", ""); out(o, "r%u", b); return true;
    case 339: mn(o, "mfspr", ""); out(o, "r%u,%u", d, I_SPR(i)); return true;
    case 467: mn(o, "mtspr", ""); out(o, "%u,r%u", I_SPR(i), d); return true;
    case 371: mn(o, "mftb", ""); out(o, "r%u,%u", d, I_SPR(i)); return true;
    case 512: mn(o, "mcrxr", ""); out(o, "cr%u", I_CRFD(i)); return true;
    case 597: mn(o, "lswi", ""); out(o, "r%u,%s,%u", d, ra0(a, t1), b); return true;
    case 725: mn(o, "stswi", ""); out(o, "r%u,%s,%u", d, ra0(a, t1), b); return true;
    case 824: mn(o, "srawi", rc(i)); out(o, "r%u,r%u,%u", a, d, b); return true;
    default: return false;
    }
}

static bool dis19(Out* o, u32 pc, u32 i) {
    u32 xo = (i >> 1) & 0x3FF;
    char t1[16], t2[16], t3[16];
    static const struct { u16 xo; const char* n; } cr[] = {
        {257, "crand"}, {129, "crandc"}, {289, "creqv"}, {225, "crnand"}, {33, "crnor"}, {449, "cror"},
        {417, "crorc"}, {193, "crxor"},
    };
    (void)pc;
    for (size_t k = 0; k < sizeof cr / sizeof cr[0]; k++)
        if (cr[k].xo == xo) {
            mn(o, cr[k].n, "");
            out(o, "%s,%s,%s", crbit_name(I_RD(i), t1), crbit_name(I_RA(i), t2), crbit_name(I_RB(i), t3));
            return true;
        }
    switch (xo) {
    case 0: mn(o, "mcrf", ""); out(o, "cr%u,cr%u", I_CRFD(i), I_CRFS(i)); return true;
    case 16: mn(o, (i & 1) ? "bclrl" : "bclr", ""); out(o, "%u,%s", I_RD(i), crbit_name(I_RA(i), t1)); return true;
    case 528: mn(o, (i & 1) ? "bcctrl" : "bcctr", ""); out(o, "%u,%s", I_RD(i), crbit_name(I_RA(i), t1)); return true;
    case 50: out(o, "rfi"); return true;
    case 150: out(o, "isync"); return true;
    default: return false;
    }
}

static bool dis63(Out* o, u32 i) {
    u32 xo = (i >> 1) & 0x3FF, d = I_RD(i), a = I_RA(i), b = I_RB(i), c = I_RC(i);
    switch (xo & 31) {
    case 18: mn(o, "fdiv", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 20: mn(o, "fsub", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 21: mn(o, "fadd", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 23: mn(o, "fsel", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 25: mn(o, "fmul", rc(i)); out(o, "f%u,f%u,f%u", d, a, c); return true;
    case 26: mn(o, "frsqrte", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 28: mn(o, "fmsub", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 29: mn(o, "fmadd", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 30: mn(o, "fnmsub", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 31: mn(o, "fnmadd", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    default: break;
    }
    switch (xo) {
    case 0: mn(o, "fcmpu", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 32: mn(o, "fcmpo", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 12: mn(o, "frsp", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 14: mn(o, "fctiw", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 15: mn(o, "fctiwz", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 40: mn(o, "fneg", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 72: mn(o, "fmr", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 136: mn(o, "fnabs", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 264: mn(o, "fabs", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 583: mn(o, "mffs", rc(i)); out(o, "f%u", d); return true;
    case 711: mn(o, "mtfsf", rc(i)); out(o, "%u,f%u", (i >> 17) & 0xFF, b); return true;
    case 134: mn(o, "mtfsfi", rc(i)); out(o, "%u,%u", I_CRFD(i), (i >> 12) & 0xF); return true;
    case 38: mn(o, "mtfsb1", rc(i)); out(o, "%u", d); return true;
    case 70: mn(o, "mtfsb0", rc(i)); out(o, "%u", d); return true;
    case 64: mn(o, "mcrfs", ""); out(o, "cr%u,cr%u", I_CRFD(i), I_CRFS(i)); return true;
    default: return false;
    }
}

static bool dis59(Out* o, u32 i) {
    u32 d = I_RD(i), a = I_RA(i), b = I_RB(i), c = I_RC(i);
    switch ((i >> 1) & 31) {
    case 18: mn(o, "fdivs", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 20: mn(o, "fsubs", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 21: mn(o, "fadds", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 24: mn(o, "fres", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 25: mn(o, "fmuls", rc(i)); out(o, "f%u,f%u,f%u", d, a, c); return true;
    case 28: mn(o, "fmsubs", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 29: mn(o, "fmadds", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 30: mn(o, "fnmsubs", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    case 31: mn(o, "fnmadds", rc(i)); out(o, "f%u,f%u,f%u,f%u", d, a, c, b); return true;
    default: return false;
    }
}

static bool dis4(Out* o, u32 i) {
    u32 xo = (i >> 1) & 0x3FF, d = I_RD(i), a = I_RA(i), b = I_RB(i), c = I_RC(i);
    static const char* a5[32] = {
        [10] = "ps_sum0", [11] = "ps_sum1", [12] = "ps_muls0", [13] = "ps_muls1", [14] = "ps_madds0",
        [15] = "ps_madds1", [18] = "ps_div", [20] = "ps_sub", [21] = "ps_add", [23] = "ps_sel", [24] = "ps_res",
        [25] = "ps_mul", [26] = "ps_rsqrte", [28] = "ps_msub", [29] = "ps_madd", [30] = "ps_nmsub", [31] = "ps_nmadd",
    };
    u32 x5 = xo & 31;
    if (a5[x5]) {
        mn(o, a5[x5], rc(i));
        switch (x5) {
        case 18: case 20: case 21: out(o, "f%u,f%u,f%u", d, a, b); break;
        case 24: case 26: out(o, "f%u,f%u", d, b); break;
        case 12: case 13: case 25: out(o, "f%u,f%u,f%u", d, a, c); break;
        default: out(o, "f%u,f%u,f%u,f%u", d, a, c, b); break;
        }
        return true;
    }
    switch (xo & 63) {
    case 6: case 7: case 38: case 39: {
        static const char* n[] = {"psq_lx", "psq_stx", "psq_lux", "psq_stux"};
        u32 k = ((xo & 63) == 6) ? 0 : ((xo & 63) == 7) ? 1 : ((xo & 63) == 38) ? 2 : 3;
        mn(o, n[k], "");
        char t1[8];
        out(o, "f%u,%s,r%u,%u,%u", d, ra0(a, t1), b, (i >> 10) & 1, (i >> 7) & 7);
        return true;
    }
    default: break;
    }
    switch (xo) {
    case 0: mn(o, "ps_cmpu0", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 32: mn(o, "ps_cmpo0", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 64: mn(o, "ps_cmpu1", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 96: mn(o, "ps_cmpo1", ""); out(o, "cr%u,f%u,f%u", I_CRFD(i), a, b); return true;
    case 40: mn(o, "ps_neg", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 72: mn(o, "ps_mr", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 136: mn(o, "ps_nabs", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 264: mn(o, "ps_abs", rc(i)); out(o, "f%u,f%u", d, b); return true;
    case 528: mn(o, "ps_merge00", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 560: mn(o, "ps_merge01", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 592: mn(o, "ps_merge10", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 624: mn(o, "ps_merge11", rc(i)); out(o, "f%u,f%u,f%u", d, a, b); return true;
    case 1014: {
        char t1[8];
        mn(o, "dcbz_l", "");
        out(o, "%s,r%u", ra0(a, t1), b);
        return true;
    }
    default: return false;
    }
}

void gekko_disasm(u32 pc, u32 i, char* buf, size_t len) {
    Out o = {buf, len};
    u32 op = i >> 26, d = I_RD(i), a = I_RA(i);
    s32 simm = (s32)(s16)(i & 0xFFFF);
    char t1[16];
    bool ok = true;
    if (len) buf[0] = 0;
    switch (op) {
    case 3: mn(&o, "twi", ""); out(&o, "%u,r%u,%d", d, a, simm); break;
    case 4: ok = dis4(&o, i); break;
    case 7: case 8: case 12: case 13: mn(&o, dname[op], ""); out(&o, "r%u,r%u,%d", d, a, simm); break;
    case 14: case 15: mn(&o, dname[op], ""); out(&o, "r%u,%s,%d", d, ra0(a, t1), simm); break;
    case 10: mn(&o, "cmpli", ""); out(&o, "cr%u,%u,r%u,%u", I_CRFD(i), (i >> 21) & 1, a, i & 0xFFFF); break;
    case 11: mn(&o, "cmpi", ""); out(&o, "cr%u,%u,r%u,%d", I_CRFD(i), (i >> 21) & 1, a, simm); break;
    case 16: {
        u32 t = (u32)(s32)(s16)(i & 0xFFFC);
        static const char* sfx[4] = {"", "l", "a", "la"};
        mn(&o, "bc", sfx[i & 3]);
        out(&o, "%u,%s,0x%x", d, crbit_name(a, t1), (i & 2) ? t : pc + t);
        break;
    }
    case 17: out(&o, "sc"); break;
    case 18: {
        u32 t = (u32)(((s32)(i << 6)) >> 6) & ~3u;
        static const char* sfx[4] = {"", "l", "a", "la"};
        mn(&o, "b", sfx[i & 3]);
        out(&o, "0x%x", (i & 2) ? t : pc + t);
        break;
    }
    case 19: ok = dis19(&o, pc, i); break;
    case 20: mn(&o, "rlwimi", rc(i)); out(&o, "r%u,r%u,%u,%u,%u", a, d, I_RB(i), (i >> 6) & 31, (i >> 1) & 31); break;
    case 21: mn(&o, "rlwinm", rc(i)); out(&o, "r%u,r%u,%u,%u,%u", a, d, I_RB(i), (i >> 6) & 31, (i >> 1) & 31); break;
    case 23: mn(&o, "rlwnm", rc(i)); out(&o, "r%u,r%u,r%u,%u,%u", a, d, I_RB(i), (i >> 6) & 31, (i >> 1) & 31); break;
    case 24: case 25: case 26: case 27: case 28: case 29:
        mn(&o, dname[op], ""); out(&o, "r%u,r%u,%u", a, d, i & 0xFFFF); break;
    case 31: ok = dis31(&o, i); break;
    case 59: ok = dis59(&o, i); break;
    case 63: ok = dis63(&o, i); break;
    case 56: case 57: case 60: case 61: {
        static const char* n[] = {"psq_l", "psq_lu", "", "", "psq_st", "psq_stu"};
        mn(&o, n[op - 56], "");
        out(&o, "f%u,%d(%s),%u,%u", d, (s32)(i << 20) >> 20, ra0(a, t1), (i >> 15) & 1, (i >> 12) & 7);
        break;
    }
    default:
        if (op >= 32 && op <= 55) {
            mn(&o, dname[op], "");
            out(&o, "%s%u,%d(%s)", op >= 48 ? "f" : "r", d, simm, ra0(a, t1));
        } else {
            ok = false;
        }
        break;
    }
    if (!ok) {
        o.p = buf;
        o.n = len;
        out(&o, ".long 0x%08x", i);
    }
    // Trim the padding left by a mnemonic without operands.
    size_t n = strlen(buf);
    while (n && buf[n - 1] == ' ') buf[--n] = 0;
}
