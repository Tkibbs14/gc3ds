// Synthetic lists go through the real decoder. Including the observer exposes its read/write masks to assertions.
#include "../../src/host/gxcensus.c"
#include <assert.h>

static void putf(GXCore* gx, u32 r, float f) { memcpy(gx->xf+r, &f, 4); }
static u32 next_field;
static void boundary(void) { gxcensus_field(++next_field); }
static void command(System* s, const u8* p, u32 n) { gxcore_feed(s, &s->gx.core, p, n); }
static void call_list(System* s, u32 address, const u8* list, u32 n) {
    ram_from_be(s->mem1, address, list, n);
    u8 call[9] = {0x40};
    for (unsigned i=0;i<4;i++) { call[1+i]=(u8)(address >> (24-8*i)); call[5+i]=(u8)(n >> (24-8*i)); }
    command(s, call, sizeof call);
}
static void draw(System* s) { const u8 p[3] = {0x90,0,4}; command(s,p,sizeof p); }
static void picture(System* s) { const u8 p[5]={0x61,0x52,0,0x40,0}; command(s,p,sizeof p); }
static u64 sampled_backend_ticks;
static volatile unsigned busy;
static void slow_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* p, u32 n, u32 stride) {
    (void)ctx; (void)s; (void)prim; (void)vat; (void)p; (void)n; (void)stride;
    u64 a=tick();
    for (unsigned i=0;i<50000;i++) busy = busy*1664525u + 1013904223u;
    if (timers[timer_depth-1].sample) sampled_backend_ticks += tick()-a;
}
int main(int argc, char** argv) {
    assert(argc==2);
    System s; assert(sys_init(&s)); hle_set_enabled(false);
    s.gx.core.backend=NULL;
    gxcensus_start(&s,argv[1]);
    GXCore* gx=&s.gx.core;
    putf(gx,0,1); putf(gx,5,1); putf(gx,10,1);
    const u8 list[32]={
        0x08,0x70,0,0,0,0, // VAT written before the draw
        0x10,0,0,0,0,0x3f,0x80,0,0, // XF[0] written before the draw
        0x61,0,0,0,0, // GEN_MODE written before the draw
        0x90,0,1
    };
    call_list(&s,0x1000,list,sizeof list);
    assert(!dep_read[0] && !dep_read[D_CP+0x70] && !dep_read[D_XF]);
    assert(dep_read[D_XF+0x1020] && !dep_read[D_XF+0x300] && !dep_read[0xa8]);
    boundary(); boundary(); // empty VI field must not evict the previous field with list calls
    gx->bp[0]=0xffff; gx->bp[0xa8]=0x123456; gx->cp[0x70]=0xabcdef;
    gx->xf[0]=0x778899; gx->xf[0x300]=0xaabbcc;
    call_list(&s,0x1000,list,sizeof list);
    assert(row.b[B_DEP_ACTIVE]==(plant ? 0 : 1) && row.b[B_DEP_LAST]==(plant ? 0 : 1));
    boundary(); boundary();
    gx->xf[0x1020]++;
    call_list(&s,0x1000,list,sizeof list);
    assert(!row.b[B_DEP_ACTIVE] && !row.b[B_DEP_LAST]);
    boundary(); boundary();
    call_list(&s,0x1000,list,sizeof list);
    assert(row.b[B_DEP_ACTIVE]==(plant ? 0 : 1) && row.b[B_DEP_LAST]==(plant ? 0 : 1));
    boundary();

    const u8 read_then_write[16]={0x90,0,1, 0x10,0,0,0x10,0x20,0,0,0,0};
    call_list(&s,0x1100,read_then_write,sizeof read_then_write);
    assert(dep_read[D_XF+0x1020] && dep_written[D_XF+0x1020]);
    const u8 masked[16]={0x61,0xfe,0,0,0xff, 0x61,0x41,0,0,0x12};
    call_list(&s,0x1200,masked,sizeof masked);
    assert(dep_read[0x41] && !dep_read[D_MASK]);
    const u8 indexed[8]={0x20,0,0,2,0};
    gx->cp[0xac]=0x3000; gx->cp[0xbc]=4;
    call_list(&s,0x1300,indexed,sizeof indexed);
    assert(dep_read[D_CP+0xac] && dep_read[D_CP+0xbc] && dep_written[D_XF+0x200]);
    const u8 masked_copy[10]={0x61,0xfe,0,0,0, 0x61,0x52,0,0x40,0};
    call_list(&s,0x1500,masked_copy,sizeof masked_copy);
    assert(row.b[B_PICTURES]==1 && !dep_read[0x43]); // raw copy command still creates an XFB picture
    const u8 guarded[8]={0x90,0,1,0};
    gx->cp[0x50]=0; call_list(&s,0x1400,guarded,sizeof guarded);
    gx->cp[0x50]=1; call_list(&s,0x1400,guarded,sizeof guarded);
    assert(row.b[B_PLAN_CHANGED]>=1 && !dep_read[D_CP+0x30]);
    gx->cp[0x50]=0; boundary();

    gx->xf[0x1009]=1; gx->xf[0x100e]=2|4;
    putf(gx,0x60b,2); putf(gx,0x60c,3);
    u64 before1=total.b[B_LIGHT1], before2=total.b[B_LIGHT2], before4=total.b[B_LIGHT4];
    const unsigned positions[]={1,2,3,1,4,1,1};
    for (unsigned i=0;i<7;i++) { putf(gx,0x60a,(float)positions[i]); draw(&s); picture(&s); boundary(); boundary(); }
    assert(total.b[B_LIGHT1]-before1==(plant ? 0 : 4));
    assert(total.b[B_LIGHT2]-before2==(plant ? 0 : 8));
    assert(total.b[B_LIGHT4]-before4==(plant ? 0 : 12));
    gx->xf[0x1009]=0;
    for (unsigned i=0;i<3;i++) { draw(&s); picture(&s); boundary(); }
    gx->xf[0x1009]=1; draw(&s);
    assert(!row.b[B_LIGHT1] && !row.b[B_LIGHT2] && row.b[B_LIGHT4]==(plant ? 0 : 4));
    picture(&s); boundary(); gx->xf[0x1009]=0;
    for (unsigned i=0;i<4;i++) { draw(&s); picture(&s); boundary(); }
    gx->xf[0x1009]=1; draw(&s); assert(!row.b[B_LIGHT4]); picture(&s); boundary();

    const u8 equals[32]={0x61,0xc0,0,0,0, 0x08,0x70,0,0,0,0,
        0x10,0,0,0,0,0x3f,0x80,0,0, 0x20,0,0,2,0};
    for (unsigned i=0;i<4096;i++) command(&s,equals,sizeof equals);
    for (unsigned c=0;c<=DEC_IDX_EQUAL;c+=2) {
        assert(row.dec_n[c]==(plant ? 0 : 4096));
        if (!plant) assert(row.dec_s[c] && row.dec_raw[c]);
    }
    static const GXBackend backend={.draw=slow_draw};
    gx->backend=&backend;
    u64 dstart=row.dec_ticks[DEC_DRAW], nstart=row.dec_s[DEC_DRAW];
    for (unsigned i=0;row.dec_s[DEC_DRAW]-nstart<8;i++) { assert(i<10000); draw(&s); }
    assert(sampled_backend_ticks && row.dec_ticks[DEC_DRAW]-dstart < sampled_backend_ticks/8);
    gx->backend=NULL;
    gxcensus_finish(); sys_free(&s);
    puts("X1b decoder/dependency/retention/backend-exclusion oracles passed");
    return 0;
}
