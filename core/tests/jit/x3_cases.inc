// Synthetic CodeWarrior-shaped tails, not game words. Exercise every suffix, cache pressure, faults and icbi.
#ifndef __3DS__
#include "x3_native.h"
#include "../../src/cpu/gekko_internal.h"
static u32 x3_cases(int plant) {
    u32 saved_opt = jit_opt(jit), saved_max = jit_debug_max_insns, saved_plant = jit_debug_plant;
    bool saved_trace = jit_traces(jit);
    u32 cases = 0, fails = 0;
    if (!alias_unchecked(0x7fffd800u) || alias_unchecked(0x80001000u) || alias_unchecked(0xc0001000u)) {
        printf("X3 alias-classification self-test failed\n"); fails++;
    }
    jit_set_traces(jit, false);
    for (u32 mode = 0; mode < 2; mode++) for (u32 load = 0; load < 2; load++)
    for (u32 first = 14; first < 32; first++) for (u32 form = 0; form < 7; form++) {
        St state = {0};
        for (u32 r = 0; r < 32; r++) state.gpr[r] = 0xaabb0000u+r;
        state.gpr[11] = form == 2 ? 0x70000000u : form == 3 || form == 6 ? 0xe0000200u :
                         (form == 1 ? 0xc0000000u : 0x80000000u) | (WIN_PA+0x200);
        state.cr = 0x12345678; state.xer = 0xa0000000u;
        CODE_PA = 0x3000;
        u32 hpa = form == 4 ? 0x7ffc : 0x7000, target = 0x80000000u+hpa;
        u32 program[] = {0x3a940001u, 0x38630001u, // addi r20,r20,1; addi r3,r3,1
            0x48000001u | ((target-(CODE_EA+8)) & 0x03fffffcu), 0x3ab40007u, 0x48000000u};
        for (u32 i = 0; i < WIN_SIZE; i++) A.mem1[WIN_PA+i] = B.mem1[WIN_PA+i] = (u8)(i*37);
        memset(A.lockedCache, 0x5a, LOCKED_CACHE_SIZE); memset(B.lockedCache, 0x5a, LOCKED_CACHE_SIZE);
        System* machines[] = {&A, &B};
        for (u32 m = 0; m < 2; m++) {
            System* s = machines[m];
            gekko_set_spr(&s->cpu, SPR_DBAT0U+4, form == 6 ? 0 : 0xe0000003u);
            put_state(s, &state);
            for (u32 i = 0; i < 5; i++) mem1_write32(s, CODE_PA+4*i, program[i]);
            for (u32 r = first; r < 32; r++) mem1_write32(s, hpa+4*(r-first),
                ((load ? 32u : 36u) << 26) | (r << 21) | (11u << 16) | (u16)(r*4-128));
            mem1_write32(s, hpa+4*(32-first), form == 5 ? 0x60000000u : 0x4e800020u);
        }
        hle_ps_set_memory(A.mem1, MEM1_SIZE, A.lockedCache);
        jit_set_opt(jit, (saved_opt & ~(OPT_GPRINLINE | OPT_ZGPR)) | (mode ? OPT_ZGPR : OPT_GPRINLINE));
        jit_invalidate_all(jit);
        jit_debug_max_insns = 5;
        jit_debug_plant = plant == 13 && !mode ? 13 : plant == 14 && mode ? 14 : plant == 19 ? 19 : 0;
        u32 n = jit_run_one_block(jit);
        interp_steps((int)n);
        bool same = !memcmp(A.cpu.gpr, B.cpu.gpr, sizeof A.cpu.gpr) && A.cpu.pc == B.cpu.pc &&
            A.cpu.lr == B.cpu.lr && A.cpu.cr == B.cpu.cr && A.cpu.xer == B.cpu.xer && A.cpu.msr == B.cpu.msr &&
            A.cpu.spr[SPR_SRR0] == B.cpu.spr[SPR_SRR0] && A.cpu.spr[SPR_DAR] == B.cpu.spr[SPR_DAR] &&
            !memcmp(A.mem1+WIN_PA, B.mem1+WIN_PA, WIN_SIZE) &&
            !memcmp(A.lockedCache, B.lockedCache, LOCKED_CACHE_SIZE);
        if (form == 5) same &= n == 3 && A.cpu.pc == target;
        if ((form == 3 || form == 6) && mode) same &= n == 3 && A.cpu.pc == target;
        if (form == 0) same &= n == 5+(33-first); // disabling the new mechanism must not masquerade as a pass
        if (form == 0 && !plant) {
            // A callee page invalidation must invalidate the caller which copied/recognised its words.
            u64 flushes = jit_stats(jit)->icbiFlushes;
            jit_invalidate(jit, hpa);
            same &= jit_stats(jit)->icbiFlushes > flushes;
        }
        if (!same && fails++ < 4) printf("X3 MISMATCH mode=%u load=%u first=%u form=%u ran=%u pc=%08x/%08x lr=%08x/%08x\n",
            mode, load, first, form, n, A.cpu.pc, B.cpu.pc, A.cpu.lr, B.cpu.lr);
        cases++;
    }
    gekko_set_spr(&A.cpu, SPR_DBAT0U+4, 0xe0000003u);
    gekko_set_spr(&B.cpu, SPR_DBAT0U+4, 0xe0000003u);
#ifdef JIT_FUZZ_X3
    for (u32 form = 0; form < 4; form++) {
        St state = {0}; state.cr = 0x12345678; state.xer = 0xa0000000u;
        for (u32 r = 0; r < 32; r++) state.gpr[r] = 0x12340000u+r;
        state.gpr[1] = 0x80000000u | WIN_PA;
        state.gpr[4] = form == 1 ? 0x817ffffcu : form == 2 ? 0x70000000u : 0x80000000u | (WIN_PA+32);
        state.fpOff = form == 3;
        u32 program[] = {0xc0210000u, 0x3bff0001u, 0x38630001u, // lfs f1,0(r1); dirty r31 and r3
            0x48000001u | ((X3_TEST_PC-(CODE_EA+12)) & 0x03fffffcu), 0xd0210008u,
            0x3a9f0007u, 0x48000000u}; // use f1 and r31 after the native call
        System* machines[] = {&A, &B};
        for (u32 m = 0; m < 2; m++) {
            System* s = machines[m]; put_state(s, &state);
            memset(s->mem1+WIN_PA, 0, WIN_SIZE); memset(s->mem1+MEM1_SIZE-16, 0, 16);
            mem1_write32(s, WIN_PA, 0x40400000u); mem1_write32(s, WIN_PA+36, 0x3f800000u);
            for (u32 i = 0; i < 7; i++) mem1_write32(s, CODE_PA+4*i, program[i]);
            for (u32 i = 0; i < sizeof x3_native_words/4; i++) mem1_write32(s, (X3_TEST_PC & 0xffffffu)+4*i, x3_native_words[i]);
        }
        // FP unavailable is tested at the callee's first FP instruction, not in the caller setup.
        if (state.fpOff) for (u32 m = 0; m < 2; m++) mem1_write32(machines[m], CODE_PA, 0x60000000u);
        hle_ps_set_memory(A.mem1, MEM1_SIZE, A.lockedCache);
        jit_set_opt(jit, saved_opt | OPT_ZXYZ | OPT_FCACHE); jit_invalidate_all(jit);
        jit_debug_max_insns = 7; jit_debug_plant = plant == 14 ? 14 : 0;
        u32 n = jit_run_one_block(jit); interp_steps((int)n);
        bool same = !memcmp(A.cpu.gpr, B.cpu.gpr, sizeof A.cpu.gpr) && A.cpu.pc == B.cpu.pc &&
            A.cpu.lr == B.cpu.lr && A.cpu.xer == B.cpu.xer && !memcmp(A.cpu.fpr, B.cpu.fpr, sizeof A.cpu.fpr) &&
            !memcmp(A.mem1+WIN_PA, B.mem1+WIN_PA, WIN_SIZE) &&
            !memcmp(A.mem1+MEM1_SIZE-16, B.mem1+MEM1_SIZE-16, 16);
        same &= form == 0 ? n == 14 : form == 2 ? n == 4 : n == 6;
        if (!same && fails++ < 4) printf("X3 ABI MISMATCH form=%u count=%u pc=%08x/%08x f1=%g/%g r31=%08x/%08x\n",
            form, n, A.cpu.pc, B.cpu.pc, A.cpu.fpr[1].ps0, B.cpu.fpr[1].ps0, A.cpu.gpr[31], B.cpu.gpr[31]);
        cases++;
    }
#endif
    for (u32 mode = 0; mode < 2; mode++) for (u32 load = 0; load < 2; load++)
    for (u32 budget = 1; budget <= 32; budget++) {
        St state = {0};
        for (u32 r = 0; r < 32; r++) state.gpr[r] = 0x12340000u+r;
        state.gpr[11] = 0x80000000u | (WIN_PA+256);
        u32 program[] = {0x3a940001u, 0x38630001u, 0x48004001u-8, 0x3ab40007u, 0x48000000u};
        u32 old_count = 0, old_pc = 0, old_gpr[32], old_lr = 0;
        u8 old_memory[256];
        for (u32 pass = 0; pass < 2; pass++) {
            put_state(&A, &state);
            memset(A.mem1+WIN_PA, 0x5a, 512);
            for (u32 i = 0; i < 5; i++) mem1_write32(&A, CODE_PA+4*i, program[i]);
            for (u32 r = 14; r < 32; r++) mem1_write32(&A, 0x7000+4*(r-14),
                ((load ? 32u : 36u) << 26) | (r << 21) | (11u << 16) | (u16)(r*4-128));
            mem1_write32(&A, 0x7000+4*18, 0x4e800020u);
            hle_ps_set_memory(A.mem1, MEM1_SIZE, A.lockedCache);
            jit_set_opt(jit, (saved_opt & ~(OPT_GPRINLINE | OPT_ZGPR)) |
                (pass ? (mode ? OPT_ZGPR : OPT_GPRINLINE) : 0));
            jit_invalidate_all(jit); jit_debug_max_insns = 0;
            jit_debug_plant = pass && plant == 18 ? 18 : 0;
            GKP(&A.cpu)->brk = GKP(&A.cpu)->stopReq = false;
            u32 n = jit_run(jit, budget);
            if (!pass) {
                old_count = n; old_pc = A.cpu.pc; old_lr = A.cpu.lr;
                memcpy(old_gpr, A.cpu.gpr, sizeof old_gpr); memcpy(old_memory, A.mem1+WIN_PA, sizeof old_memory);
            } else if (n != old_count || A.cpu.pc != old_pc || A.cpu.lr != old_lr ||
                       memcmp(old_gpr, A.cpu.gpr, sizeof old_gpr) ||
                       memcmp(old_memory, A.mem1+WIN_PA, sizeof old_memory)) {
                if (fails++ < 4) printf("X3 budget MISMATCH mode=%u load=%u budget=%u count=%u/%u pc=%08x/%08x\n",
                    mode, load, budget, n, old_count, A.cpu.pc, old_pc);
            }
        }
        cases++;
    }
    jit_debug_plant = saved_plant; jit_debug_max_insns = saved_max;
    jit_set_opt(jit, saved_opt); jit_set_traces(jit, saved_trace); jit_invalidate_all(jit);
    printf("X3 directed cases=%u fails=%u (inline/native; all suffixes, mirror, locked cache, DSI, cross-page, rejection, icbi)\n", cases, fails);
    return fails;
}
#endif
