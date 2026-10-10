// Runs the interpreter, the JIT and the micro-kernels on an emulated ARM1176 (qemu-arm -cpu arm1176) and checks
// them against each other, so encoding mistakes show up before anything is sent to the 3DS. Timing here means
// nothing; only results are checked. Freestanding: Linux EABI syscalls, no libc.
#include "../src/kernels.h"
#include "../src/progs.h"

void* memcpy(void* d, const void* s, unsigned n) { u8* a = d; const u8* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned n) { u8* a = d; while (n--) *a++ = (u8)c; return d; }
int memcmp(const void* x, const void* y, unsigned n) {
    const u8* a = x; const u8* b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
unsigned strlen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
// The decoder and JIT only call snprintf to report errors; keep the format string as the message.
int snprintf(char* out, unsigned n, const char* fmt, ...) {
    unsigned i = 0;
    for (; fmt[i] && i + 1 < n; i++) out[i] = fmt[i];
    if (n) out[i] = 0;
    return (int)i;
}

static void sys_write(const char* s, unsigned n) {
    register int r0 __asm__("r0") = 1;
    register const char* r1 __asm__("r1") = s;
    register unsigned r2 __asm__("r2") = n;
    register int r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
static void __attribute__((noreturn)) sys_exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" : : "r"(r0), "r"(r7));
    for (;;) {}
}
static void* sys_mmap_rwx(unsigned size) {
    register u32 r0 __asm__("r0") = 0;
    register u32 r1 __asm__("r1") = size;
    register u32 r2 __asm__("r2") = 7;      // PROT_READ | PROT_WRITE | PROT_EXEC
    register u32 r3 __asm__("r3") = 0x22;   // MAP_PRIVATE | MAP_ANONYMOUS
    register s32 r4 __asm__("r4") = -1;
    register u32 r5 __asm__("r5") = 0;
    register u32 r7 __asm__("r7") = 192;    // mmap2
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7) : "memory");
    return (void*)r0;
}
static void out(const char* s) { sys_write(s, strlen(s)); }
static void hex(u32 v) {
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    out(b);
}
static u32 f2u(float f) { u32 v; memcpy(&v, &f, 4); return v; }
static u32 read_fpscr(void) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }

#define JIT_WORDS (64 * 1024)
#define RAM_BYTES (64 * 1024)
static u8* ramA;
static u8* ramB;
static u32* jitBuf;
static Decoded dec[48];
static int plantDifference;  // self-test: the comparison must catch a single flipped bit

static int check(Program* p, int mode, int lay, const char* label) {
    char err[128];
    PPCState a, b;
    if (ppc_decode(p->code, p->n, dec, err, sizeof err) < 0) { out(err); out("\n"); return 0; }
    reset_guest(&a, ramA, lay); ppc_interp(&a, ramA, dec, lay);
    reset_guest(&b, ramB, lay);
    int words = jit_compile(dec, p->n, jitBuf, JIT_WORDS, mode, lay, b.gqr, err, sizeof err);
    if (words < 0) { out(err); out("\n"); return 0; }
    u32 fpscrBefore = read_fpscr();
    ((JitBlock)jitBuf)(&b, ramB);
    if (plantDifference) b.gpr[7] ^= 1;
    int ok = 1;
    for (int r = 0; r < 32; r++) if (a.gpr[r] != b.gpr[r]) { out("  r"); hex(r); out(" interp "); hex(a.gpr[r]); out(" jit "); hex(b.gpr[r]); out("\n"); ok = 0; }
    for (int r = 0; r < 32; r++) {
        if (f2u(a.ps0[r]) != f2u(b.ps0[r]) || f2u(a.ps1[r]) != f2u(b.ps1[r])) {
            out("  f"); hex(r); out(" interp "); hex(f2u(a.ps0[r])); out(","); hex(f2u(a.ps1[r]));
            out(" jit "); hex(f2u(b.ps0[r])); out(","); hex(f2u(b.ps1[r])); out("\n"); ok = 0;
        }
    }
    if (a.cr != b.cr) { out("  cr interp "); hex(a.cr); out(" jit "); hex(b.cr); out("\n"); ok = 0; }
    if (a.xer != b.xer) { out("  xer interp "); hex(a.xer); out(" jit "); hex(b.xer); out("\n"); ok = 0; }
    if (memcmp(ramA, ramB, GUEST_TOUCHED)) {
        for (u32 i = 0; i < GUEST_TOUCHED; i++) if (ramA[i] != ramB[i]) { out("  RAM differs at "); hex(i); out("\n"); break; }
        ok = 0;
    }
    if (read_fpscr() != fpscrBefore) { out("  FPSCR not restored\n"); ok = 0; }
    if (b.pc == PC_BAILED) { out("  bailed\n"); ok = 0; }
    out(label); out(ok ? ": OK r3=" : ": MISMATCH r3="); hex(a.gpr[3]); out(" f3="); hex(f2u(a.ps0[3]));
    out(","); hex(f2u(a.ps1[3])); out("\n");
    return ok;
}

static int check_kernels(void) {
    int ok = 1;
    // Linked and dispatched block chains.
    for (u32 K = 64; K <= 2048; K *= 32) {
        int w = kern_linked(jitBuf, JIT_WORDS, K, 16);
        u32 r = w < 0 ? 0 : ((u32 (*)(u32))jitBuf)(5000);
        out("linked K="); hex(K); out(r == 20000 ? " OK\n" : " BAD\n"); ok &= r == 20000;
        static u32 words[2048];
        static KernEntry tab[2048];
        w = kern_dispatch(jitBuf, JIT_WORDS, K, 16, words);
        for (u32 i = 0; i < K; i++) { tab[i].pc = kern_block_pc(i); tab[i].fn = (u32 (*)(void))(jitBuf + words[i]); }
        u32 expect = 0;
        for (u32 i = 0; i < 5000; i++) expect = kern_next(expect, K);
        u32 pc = w < 0 ? 0 : kern_run_dispatch(tab, K, 5000);
        out("dispatch K="); hex(K); out(pc == kern_block_pc(expect) ? " OK\n" : " BAD\n"); ok &= pc == kern_block_pc(expect);
    }
    // Memory paths agree with each other and with a plain C sum.
    for (u32 i = 0; i < RAM_BYTES; i++) ramA[i] = (u8)(i * 7 + (i >> 8));
    kern_set_ram(ramA);
    const u32 count = RAM_BYTES / 4;
    u32 expect = 0;
    for (u32 i = 0; i < count; i++) expect += ((u32)ramA[4 * i] << 24) | ((u32)ramA[4 * i + 1] << 16) | ((u32)ramA[4 * i + 2] << 8) | ramA[4 * i + 3];
    for (int v = 0; v < KMEM_COUNT; v++) {
        int w = kern_mem(jitBuf, JIT_WORDS, v);
        u32 sum = w < 0 ? 0 : ((u32 (*)(u32, u8*))jitBuf)(count, ramA);
        out("mem "); out(kern_mem_names[v]); out(sum == expect ? " OK\n" : " BAD\n"); ok &= sum == expect;
    }
    // Every VFP kernel runs to completion and restores FPSCR.
    for (int k = 0; k < KFP_COUNT; k++) {
        for (int dep = 0; dep < 2; dep++) {
            u32 before = read_fpscr();
            int w = kern_fp(jitBuf, JIT_WORDS, k, dep);
            if (w >= 0) ((void (*)(u32))jitBuf)(10);
            int good = w >= 0 && read_fpscr() == before;
            if (!good) { out("fp "); out(kern_fp_names[k]); out(" BAD\n"); }
            ok &= good;
        }
    }
    out(ok ? "kernels OK\n" : "kernels FAILED\n");
    return ok;
}

void _start(void) {
    jitBuf = sys_mmap_rwx(JIT_WORDS * 4);
    ramA = sys_mmap_rwx(RAM_BYTES);
    ramB = sys_mmap_rwx(RAM_BYTES);
    if ((u32)jitBuf >= 0xFFFFF000u || (u32)ramA >= 0xFFFFF000u || (u32)ramB >= 0xFFFFF000u) { out("mmap failed\n"); sys_exit(2); }
    Program p;
    plantDifference = 1;
    make_int(&p, 10);
    out("self-test (expect MISMATCH): ");
    if (check(&p, JIT_PS_SCALAR, RAM_BE, "int")) { out("comparison is blind\n"); sys_exit(3); }
    plantDifference = 0;
    int ok = 1;
    static void (*const makes[])(Program*, u32) = { make_int, make_float, make_ps, make_pseq, make_pilp };
    static const char* const names[3][2] = { { "BE", "BE vec" }, { "sw", "sw vec" }, { "setend", "setend vec" } };
    u32 iters[3] = { 1, 1000, 70000 };  // 70000 needs the LIS+ORI pair to carry a non-zero upper half
    for (int k = 0; k < 3; k++) {
        for (unsigned m = 0; m < sizeof makes / sizeof makes[0]; m++) {
            for (int lay = 0; lay < 3; lay++) {
                for (int mode = 0; mode < 2; mode++) {
                    makes[m](&p, iters[k]);
                    out(p.name); out(" "); out(names[lay][mode]); out(" ");
                    ok &= check(&p, mode, lay, "");
                }
            }
        }
    }
    ok &= check_kernels();
    out(ok ? "ALL OK\n" : "FAILED\n");
    sys_exit(ok ? 0 : 1);
}
