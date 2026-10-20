// Synthetic input exercises the real census, without a disc or a save state.
#include "../../src/host/gxcensus.c"
#include <assert.h>
bool hle_enabled(void) { return false; }
static void putf(GXCore* gx, u32 r, float v) { memcpy(gx->xf + r, &v, 4); }
int main(int argc, char** argv) {
    assert(argc == 2);
    static System s;
    s.mem1 = calloc(MEM1_SIZE, 1);
    assert(s.mem1);
    gxcensus_start(&s, argv[1]);
    ram_wr32(s.mem1, gxptr, gxbase | 0x80000000);
    GXCore* gx = &s.gx.core;
    gx->cp[0x50] = 0; // no direct attributes: one default vertex, matrix zero
    gx->xf[0x1009] = 1; gx->xf[0x100e] = 2 | 4;
    gx->xf[0x100a] = 0x102030ff; gx->xf[0x100c] = 0xffffffff;
    putf(gx, 0, 1); putf(gx, 5, 1); putf(gx, 10, 1);
    putf(gx, 0x60a, 4); putf(gx, 0x60b, 5); putf(gx, 0x60c, 6);
    putf(gx, 0x60d, 0); putf(gx, 0x60e, 1); putf(gx, 0x60f, 0);
    // An inverse must handle translation and a non-orthonormal matrix.
    double inv[3][3], t[3];
    putf(gx, 0, 2); putf(gx, 3, 10);
    assert(inverse(gx, 0, inv, t) && inv[0][0] == .5 && t[0] == 10);
    putf(gx, 0, 1); putf(gx, 3, 0);
    bool ok;
    u64 light0 = lighting_key(gx, 0, 1, 123, &ok); assert(ok);
    putf(gx, 3, 10); putf(gx, 0x60a, 14);
    assert(lighting_key(gx, 0, 1, 123, &ok) == light0 && ok);
    putf(gx, 3, 0); putf(gx, 0x60a, 4);
    const u8* list = s.mem1 + 0x1000;
    u32 sdkpc = 0;
    for (u32 i=0; i<nsym; i++) if (symbols[i].gx) { sdkpc=symbols[i].lo; break; }
    assert(sdkpc);
    for (unsigned f=0; f<3; f++) {
        gxcensus_reg(0, 0xc0, 7, 7); gxcensus_reg(0, 0x80, 8, 9);
        gxcensus_reg(1, 0xa0, 3, 3); gxcensus_reg(2, 0, 0, 0);
        gxcensus_reg(2, 0x600, 0, 0); gxcensus_reg(1, 0x50, 0, 0);
        gxcensus_list(gx, list, 32);
        gxcensus_draw(&s, gx, 0, list, 4, 0);
        gxcensus_list_end();
        gx->xf[0x100e] |= 1u << 9;
        gxcensus_draw(&s, gx, 0, list, 4, 0);
        gx->xf[0x100e] &= ~(1u << 9);
        gxcensus_mem(sdkpc, 0x0c008000, 8, true);
        gxcensus_mem(0x80003100, 0x0c008000, 4, true);
        gxcensus_field(f+1);
    }
    if (!plant) { assert(total.same[0]==3 && total.upper==2 && total.lower==2 && total.hit==8); }
    else { assert(!total.same[0] && !total.upper && !total.lower && !total.hit); }
    assert(total.spec == 12);
    assert(total.fifo[plant ? 1 : 0] == 24);
    assert(total.outside[0] == (plant ? 3 : 0));
    gxcensus_finish(); free(s.mem1);
    return 0;
}
