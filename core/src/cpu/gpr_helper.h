#pragma once
#include "../ramlayout.h"

// CodeWarrior's suffix entries: rN..r31, contiguous word slots through r11, then plain blr.
// Match instructions, never a title address. No update forms or writes to the base are admitted.
typedef struct { u32 first, words; s32 displacement; bool load; } GprHelper;
static inline bool gpr_helper_match(const u8* ram, u32 size, u32 pa, GprHelper* h) {
    if ((pa & 3) || pa > size || size - pa < 8) return false;
    u32 w = ram_rd32(ram, pa), op = w >> 26, first = (w >> 21) & 31;
    if ((op != 32 && op != 36) || ((w >> 16) & 31) != 11 || first < 14) return false;
    u32 n = 32 - first;
    if (size - pa < 4*(n+1) || (w & 3)) return false;
    s32 d = (s16)w;
    for (u32 k = 0; k < n; k++) {
        s32 off = d + (s32)(4*k);
        if (off > 32767 || ram_rd32(ram, pa+4*k) !=
            ((op << 26) | ((first+k) << 21) | (11u << 16) | (u16)off)) return false;
    }
    if (ram_rd32(ram, pa+4*n) != 0x4e800020u) return false;
    *h = (GprHelper){first, n+1, d, op == 32};
    return true;
}
