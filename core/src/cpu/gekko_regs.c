// Which guest registers an instruction reads and writes, for analysing guest code before it is compiled (the JIT's
// block entry states, docs/REGION_JIT_PLAN.md). Ids: GPRs 0-31, then GR_CR, GR_XER, GR_LR, GR_CTR (gekko.h). The sets
// are conservative: everything the instruction may read is in `reads`, everything it may write in `writes` (FPRs,
// FPSCR, GQRs and other SPRs are not tracked). Instructions that change state compiled code depends on (MSR, segment
// registers, HID2, caches), traps, string loads and stores, and anything not listed return false: analysis stops there.
// Checked against the interpreter by the JIT fuzzer (jit_fuzz.c, check_regs): changing registers outside `reads` must
// not change the outcome, and registers outside `writes` must keep their values.
#include "gekko_internal.h"

#define R(x) (1ull << (x))

bool gekko_regs(u32 inst, u64* reads, u64* writes) {
    u32 op = inst >> 26, rd = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31;
    u64 r = 0, w = 0, ra0 = ra ? R(ra) : 0;  // ra0: (rA|0)
    // The record bit (CR0, or CR1 for floats) exists only in these forms; in D forms bit 0 is part of the immediate.
    bool rc = (op == 4 || op == 20 || op == 21 || op == 23 || op == 31 || op == 59 || op == 63) && (inst & 1);
    const u64 CR = R(GR_CR), XER = R(GR_XER), LR = R(GR_LR), CTR = R(GR_CTR);
    switch (op) {
    case 7: r = R(ra); w = R(rd); break;                                          // mulli
    case 8: r = R(ra) | XER; w = R(rd) | XER; break;                              // subfic (CA)
    case 10: case 11: r = R(ra) | CR | XER; w = CR; break;                        // cmpli, cmpi (SO into the field)
    case 12: r = R(ra) | XER; w = R(rd) | XER; break;                             // addic
    case 13: r = R(ra) | XER | CR; w = R(rd) | XER | CR; break;                   // addic.
    case 14: case 15: r = ra0; w = R(rd); break;                                  // addi, addis
    case 16:                                                                       // bc
        if (!(rd & 4)) { r |= CTR; w |= CTR; }
        if (!(rd & 16)) r |= CR;
        if (inst & 1) w |= LR;
        break;
    case 18: if (inst & 1) w = LR; break;                                         // b
    case 19: {
        u32 xo = (inst >> 1) & 0x3FF;
        switch (xo) {
        case 0: case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: r = CR; w = CR; break;  // mcrf, cr*
        case 16:                                                                   // bclr
            r = LR;
            if (!(rd & 4)) { r |= CTR; w |= CTR; }
            if (!(rd & 16)) r |= CR;
            if (inst & 1) w |= LR;
            break;
        case 528:                                                                  // bcctr
            if (!(rd & 4)) return false;
            r = CTR;
            if (!(rd & 16)) r |= CR;
            if (inst & 1) w |= LR;
            break;
        default: return false;                                                     // rfi, isync
        }
        break;
    }
    case 20: r = R(rd) | R(ra); w = R(ra); break;                                 // rlwimi
    case 21: r = R(rd); w = R(ra); break;                                         // rlwinm
    case 23: r = R(rd) | R(rb); w = R(ra); break;                                 // rlwnm
    case 24: case 25: case 26: case 27: r = R(rd); w = R(ra); break;  // ori, oris, xori, xoris
    case 28: case 29: r = R(rd) | CR | XER; w = R(ra) | CR; break;    // andi., andis.
    case 32: case 34: case 40: case 42: r = ra0; w = R(rd); break;    // lwz, lbz, lhz, lha
    case 33: case 35: case 41: case 43: r = R(ra); w = R(rd) | R(ra); break;  // with update
    case 36: case 38: case 44: r = R(rd) | ra0; break;                // stw, stb, sth
    case 37: case 39: case 45: r = R(rd) | R(ra); w = R(ra); break;   // with update
    case 46: r = ra0; for (u32 k = rd; k < 32; k++) w |= R(k); break; // lmw
    case 47: r = ra0; for (u32 k = rd; k < 32; k++) r |= R(k); break; // stmw
    case 48: case 50: case 52: case 54: case 56: case 60: r = ra0; break;  // lfs lfd stfs stfd psq_l psq_st
    case 49: case 51: case 53: case 55: case 57: case 61: r = R(ra); w = R(ra); break;  // with update
    case 59: break;                                                                // single-precision arithmetic (CR1 with Rc)
    case 63: {
        u32 xo = (inst >> 1) & 0x3FF;
        if (xo == 38 || xo == 70 || xo == 134 || xo == 711) return false;          // FPSCR rounding mode: ends the block
        if (xo == 0 || xo == 32 || xo == 64) { r = CR; w = CR; }                  // fcmpu, fcmpo, mcrfs
        break;
    }
    case 4: {
        u32 xo = (inst >> 1) & 0x3FF, xo6 = (inst >> 1) & 0x3F;
        if (xo6 == 6 || xo6 == 7) { r = ra0 | R(rb); rc = false; break; }         // psq_lx, psq_stx
        if (xo6 == 38 || xo6 == 39) { r = R(ra) | R(rb); w = R(ra); rc = false; break; }  // psq_lux, psq_stux
        if (xo == 1014) { r = ra0 | R(rb); rc = false; break; }                   // dcbz_l
        if (xo == 0 || xo == 32 || xo == 64 || xo == 96) { r = CR; w = CR; }      // ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1
        break;
    }
    case 31: {
        u32 xo = (inst >> 1) & 0x3FF, xo9 = xo & 0x1FF;
        bool oe = xo & 0x200;
        switch (xo9) {  // XO-form arithmetic (OE in bit 9 of the extended opcode)
        case 266: case 40: case 235: case 491: case 459:                           // add subf mullw divw divwu
            r = R(ra) | R(rb); w = R(rd); goto xo_form;
        case 10: case 8: case 138: case 136:                                       // addc subfc adde subfe
            r = R(ra) | R(rb) | XER; w = R(rd) | XER; goto xo_form;
        case 202: case 200: case 234: case 232:                                    // addze subfze addme subfme
            r = R(ra) | XER; w = R(rd) | XER; goto xo_form;
        case 104: r = R(ra); w = R(rd); goto xo_form;                              // neg
        case 75: case 11:                                                          // mulhw mulhwu (no OE form)
            if (oe) return false;
            r = R(ra) | R(rb); w = R(rd); goto xo_form;
        default: break;
        }
        switch (xo) {
        case 0: case 32: r = R(ra) | R(rb) | CR | XER; w = CR; rc = false; break;  // cmp, cmpl
        case 19: r = CR; w = R(rd); rc = false; break;                             // mfcr
        case 20: case 23: case 87: case 279: case 343: case 534: case 790:         // lwarx lwzx lbzx lhzx lhax lwbrx lhbrx
            r = ra0 | R(rb); w = R(rd); rc = false; break;
        case 55: case 119: case 311: case 375:                                     // lwzux lbzux lhzux lhaux
            r = R(ra) | R(rb); w = R(rd) | R(ra); rc = false; break;
        case 151: case 215: case 407: case 662: case 918:                          // stwx stbx sthx stwbrx sthbrx
            r = R(rd) | ra0 | R(rb); rc = false; break;
        case 183: case 247: case 439:                                              // stwux stbux sthux
            r = R(rd) | R(ra) | R(rb); w = R(ra); rc = false; break;
        case 150: r = R(rd) | ra0 | R(rb) | XER | CR; w = CR; rc = false; break;   // stwcx.
        case 24: case 536: case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
            r = R(rd) | R(rb); w = R(ra); break;                                   // slw srw and andc nor eqv xor orc or nand
        case 792: r = R(rd) | R(rb) | XER; w = R(ra) | XER; break;                 // sraw
        case 824: r = R(rd) | XER; w = R(ra) | XER; break;                         // srawi
        case 26: case 922: case 954: r = R(rd); w = R(ra); break;                  // cntlzw extsh extsb
        case 144: r = R(rd) | CR; w = CR; rc = false; break;                       // mtcrf
        case 83: w = R(rd); rc = false; break;                                     // mfmsr
        case 339: {                                                                // mfspr
            u32 spr = ((inst >> 16) & 31) | ((inst >> 6) & 0x3E0);
            r = spr == 1 ? XER : spr == 8 ? LR : spr == 9 ? CTR : 0;
            w = R(rd);
            rc = false;
            break;
        }
        case 371: w = R(rd); rc = false; break;                                    // mftb
        case 467: {                                                                // mtspr
            u32 spr = ((inst >> 16) & 31) | ((inst >> 6) & 0x3E0);
            r = R(rd);
            rc = false;
            if (spr == 1) w = XER;
            else if (spr == 8) w = LR;
            else if (spr == 9) w = CTR;
            else if (!((spr >= 912 && spr <= 919) || (spr >= 272 && spr <= 275) || spr == 26 || spr == 27 || spr == 18 ||
                       spr == 19 || spr == 1008)) return false;                    // only GQRs, SPRGs, SRR0/1, DSISR, DAR, HID0
            break;
        }
        case 595: w = R(rd); rc = false; break;                                    // mfsr
        case 659: r = R(rb); w = R(rd); rc = false; break;                         // mfsrin
        case 512: r = XER | CR; w = XER | CR; rc = false; break;                   // mcrxr
        case 54: case 86: case 470: case 278: case 246: case 1014: r = ra0 | R(rb); rc = false; break;  // dcbst dcbf dcbi dcbt dcbtst dcbz
        case 598: case 854: rc = false; break;                                     // sync, eieio
        case 535: case 599: case 663: case 727: case 983: r = ra0 | R(rb); rc = false; break;  // lfsx lfdx stfsx stfdx stfiwx
        case 567: case 631: case 695: case 759: r = R(ra) | R(rb); w = R(ra); rc = false; break;  // lfsux lfdux stfsux stfdux
        default: return false;
        }
        break;
    xo_form:
        if (oe) { r |= XER; w |= XER; }
        break;
    }
    default: return false;
    }
    if (rc) { r |= CR | XER; w |= CR; }
    *reads = r;
    *writes = w;
    return true;
}
