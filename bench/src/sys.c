// gcbench system module: memory, SD storage, crypto, audio mixing and cross-core sync costs, each framed by the
// GameCube/Wii emulator question it answers. Every test checks its allocations and Results, prints "SKIPPED: why"
// when it cannot run, is time-bounded, and frees what it allocated (the GX test deliberately leaks its buffers if the
// GPU never confirms completion, because freeing memory the GPU may still write is worse than a leak).
#include <3ds.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include "bench.h"

#define MiB (1024u * 1024u)
#define TICKS_PER_US (SYSCLOCK_ARM11 / 1e6)

static volatile u32 g_sink;  // timed results are folded in here so the compiler cannot drop the work
static u32 g_rng = 0x2545F491u;

static inline u32 rnd(void) {
    u32 x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}
static inline u32 rnd_below(u32 n) { return (u32)(((u64)rnd() * n) >> 32); }
static void fill_random(void* p, size_t bytes) {
    u32* w = p;
    for (size_t i = 0; i < bytes / 4; i++) w[i] = rnd();
}
// Tells the compiler that memory behind p is read here, so stores into it are not dead.
static inline void clobber(void* p) { __asm__ volatile("" : : "r"(p) : "memory"); }

// ============================================================================================================
// 1. Memory
// ============================================================================================================
typedef void* (*AllocFn)(size_t);
typedef void (*FreeFn)(void*);

// Largest size in [0, hi) that allocates, to `gran` precision. Each probe is freed at once.
static size_t largest_alloc(AllocFn a, FreeFn f, size_t hi, size_t gran) {
    size_t lo = 0;
    while (hi - lo > gran) {
        size_t mid = lo + (hi - lo) / 2;
        mid -= mid % gran;
        if (mid <= lo) mid = lo + gran;
        void* p = a(mid);
        if (p) { f(p); lo = mid; } else hi = mid;
    }
    return lo;
}

static void mem_limits(void) {
    step("mem max malloc");
    P("app region free: %lu KB\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
    size_t m = largest_alloc(malloc, free, 512u * MiB, 256u * 1024);
    P("max malloc: %.1f MB\n", m / (double)MiB);

    step("mem max linearAlloc");
    unsigned long lf = linearSpaceFree();
    size_t l = largest_alloc(linearAlloc, linearFree, 512u * MiB, 64u * 1024);
    P("max linearAlloc: %.2f MB (free %.2f)\n", l / (double)MiB, lf / (double)MiB);

    step("mem max vramAlloc");
    unsigned long vf = vramSpaceFree();
    size_t v = largest_alloc(vramAlloc, vramFree, 16u * MiB, 16u * 1024);
    P("max vramAlloc: %.2f MB (free %.2f)\n", v / (double)MiB, vf / (double)MiB);
}

static void bswap_copy(u32* restrict d, const u32* restrict s, size_t nwords) {
    for (size_t i = 0; i < nwords; i += 4) {
        d[i] = __builtin_bswap32(s[i]);
        d[i + 1] = __builtin_bswap32(s[i + 1]);
        d[i + 2] = __builtin_bswap32(s[i + 2]);
        d[i + 3] = __builtin_bswap32(s[i + 3]);
    }
}

static void mem_bandwidth(void) {
    const size_t sz = 4u * MiB;
    u8* a = malloc(sz);
    u8* b = malloc(sz);
    step("mem memcpy 4MB");
    if (!a || !b) {
        P("memcpy: SKIPPED: malloc 2x4MB failed\n");
        P("bswap copy: SKIPPED: malloc failed\n");
        free(a); free(b);
        return;
    }
    fill_random(a, sz);
    memcpy(b, a, sz);  // warm both buffers (page faults/TLB) before timing
    u32 n = 0;
    double t0 = now_s(), t;
    do {
        a[(n * 64) & (sz - 1)] ^= (u8)n;
        memcpy(b, a, sz);
        clobber(b);
        n++;
        t = now_s() - t0;
    } while (t < 0.5 && n < 512);
    g_sink += b[sz / 3];
    P("memcpy heap 4MB: %.0f MB/s (%lu x)\n", n * (double)sz / MiB / t, (unsigned long)n);

    step("mem bswap copy 4MB");
    n = 0;
    t0 = now_s();
    do {
        a[(n * 64) & (sz - 1)] ^= (u8)n;
        bswap_copy((u32*)b, (const u32*)a, sz / 4);
        clobber(b);
        n++;
        t = now_s() - t0;
    } while (t < 0.5 && n < 512);
    // Verify one word so a broken copy cannot report a fast number.
    u32 w0, w1;
    memcpy(&w0, a + 128, 4);
    memcpy(&w1, b + 128, 4);
    g_sink += w1;
    P("bswap32 copy 4MB: %.0f MB/s %s\n", n * (double)sz / MiB / t, w1 == __builtin_bswap32(w0) ? "ok" : "WRONG");
    free(a);
    free(b);
}

// Random single-cycle permutation over the 32-byte lines of the first `bytes` of buf (Sattolo's algorithm);
// word 0 of each line holds the word index of the next line to visit.
static void build_chain(u32* buf, size_t bytes) {
    u32 n = bytes / 32;
    for (u32 i = 0; i < n; i++) buf[i * 8] = i;
    for (u32 i = n - 1; i > 0; i--) {
        u32 j = rnd_below(i);
        u32 t = buf[i * 8]; buf[i * 8] = buf[j * 8]; buf[j * 8] = t;
    }
    for (u32 i = 0; i < n; i++) buf[i * 8] *= 8;
}

// Dependent loads: latency per access.
static double chase_ns(const u32* buf, u32 accesses) {
    u32 idx = 0;
    double t = now_s();
    for (u32 i = 0; i < accesses; i += 8) {
        idx = buf[idx]; idx = buf[idx]; idx = buf[idx]; idx = buf[idx];
        idx = buf[idx]; idx = buf[idx]; idx = buf[idx]; idx = buf[idx];
    }
    t = now_s() - t;
    g_sink += idx;
    return t * 1e9 / accesses;
}

// Independent loads at random addresses (an emulated CPU's loads mostly do not depend on the previous load).
static double random_ns(const u32* buf, u32 nwords, u32 accesses) {
    u32 x = 12345, s = 0;
    double t = now_s();
    for (u32 i = 0; i < accesses; i++) {
        x = x * 1664525u + 1013904223u;
        s += buf[((u64)x * nwords) >> 32];
    }
    t = now_s() - t;
    g_sink += s;
    return t * 1e9 / accesses;
}

static void mem_random(void) {
    step("mem random reads");
    const size_t big = 24u * MiB;
    u32* buf = malloc(big);
    if (!buf) { P("random reads: SKIPPED: malloc 24MB failed\n"); return; }
    memset(buf, 0, big);
    static const size_t sizes[3] = { 8u * 1024, 256u * 1024, 24u * MiB };
    static const char* names[3] = { "8K", "256K", "24M" };
    const u32 acc = 1u << 20;
    double ch[3], rn[3];
    for (int k = 0; k < 3; k++) {
        build_chain(buf, sizes[k]);
        ch[k] = chase_ns(buf, acc);
        rn[k] = random_ns(buf, sizes[k] / 4, acc);
    }
    P("chase ns: %s %.1f %s %.1f %s %.1f\n", names[0], ch[0], names[1], ch[1], names[2], ch[2]);
    P("random ns: %s %.1f %s %.1f %s %.1f\n", names[0], rn[0], names[1], rn[1], names[2], rn[2]);
    P("24M vs 256K miss cost: +%.0f ns dependent\n", ch[2] - ch[1]);
    free(buf);
}

static LightEvent g_ppfEv;
static void ppf_cb(void* arg) { LightEvent_Signal((LightEvent*)arg); }

// One raw GX TextureCopy, waiting for the PPF interrupt with a timeout.
// 1 = done, 0 = command refused (nothing in flight), -1 = timeout (the GPU may still touch the buffers).
static int gx_copy(void* src, void* dst, u32 size, double* secs) {
    LightEvent_Clear(&g_ppfEv);
    gspSetEventCallback(GSPGPU_EVENT_PPF, ppf_cb, &g_ppfEv, true);
    double t = now_s();
    // Line width 0x100 x 16 bytes, gap 0: one contiguous block.
    Result r = GX_TextureCopy((u32*)src, GX_BUFFER_DIM(0x100, 0), (u32*)dst, GX_BUFFER_DIM(0x100, 0), size,
                              GX_TRANSFER_RAW_COPY(1));
    if (R_FAILED(r)) {
        gspSetEventCallback(GSPGPU_EVENT_PPF, NULL, NULL, false);
        P("GX copy: SKIPPED: TextureCopy %08lx\n", (unsigned long)r);
        return 0;
    }
    if (LightEvent_WaitTimeout(&g_ppfEv, 1000000000LL)) {
        gspSetEventCallback(GSPGPU_EVENT_PPF, NULL, NULL, false);
        return -1;
    }
    *secs = now_s() - t;
    return 1;
}

static void mem_gx(void) {
    step("mem GX copy to VRAM");
    P("GX heap->VRAM: SKIPPED: GSP takes linear/VRAM only\n");
    const u32 sz = MiB;
    u8* src = linearAlloc(sz);
    u8* rb = linearAlloc(sz);
    u8* vr = vramAlloc(sz);
    if (!src || !rb || !vr) {
        P("GX copy: SKIPPED: alloc lin %d/%d vram %d\n", src != NULL, rb != NULL, vr != NULL);
        if (src) linearFree(src);
        if (rb) linearFree(rb);
        if (vr) vramFree(vr);
        return;
    }
    LightEvent_Init(&g_ppfEv, RESET_ONESHOT);
    fill_random(src, sz);
    double flushT = 0, copyT = 0, s = 0;
    int n = 0, st = 1;
    for (; n < 16; n++) {
        src[n * 4096] ^= 0xA5;  // dirty a line so each flush has work
        double t = now_s();
        Result fr = GSPGPU_FlushDataCache(src, sz);
        flushT += now_s() - t;
        if (R_FAILED(fr)) { P("GX copy: SKIPPED: flush %08lx\n", (unsigned long)fr); st = 0; break; }
        st = gx_copy(src, vr, sz, &s);
        if (st != 1) break;
        copyT += s;
    }
    if (st == -1) {
        // Probably a GX command queue is bound (citro3d active), so the copy was queued, not run.
        P("GX copy: SKIPPED: no PPF in 1 s; 3 MB leaked\n");
        return;
    }
    if (st == 1) {
        // Read back VRAM through the GPU and compare, so a silently dropped copy cannot report a speed.
        memset(rb, 0, sz);
        Result fr = GSPGPU_FlushDataCache(rb, sz);
        st = R_SUCCEEDED(fr) ? gx_copy(vr, rb, sz, &s) : 0;
        if (st == -1) { P("GX copy: readback no PPF in 1 s; 3 MB leaked\n"); return; }
        int ok = 0;
        if (st == 1) {
            GSPGPU_InvalidateDataCache(rb, sz);
            ok = memcmp(src, rb, sz) == 0;
        }
        P("GX copy lin->VRAM: %.0f MB/s %s\n", n * (double)sz / MiB / copyT, st != 1 ? "unverified" : ok ? "verified" : "MISMATCH");
        P("  +cache flush: %.0f MB/s overall\n", n * (double)sz / MiB / (copyT + flushT));
    }

    // A CPU memcpy into vramAlloc() memory killed the app on the 2DS (gcbench 2, 2026-10-10): VRAM is reachable
    // only through GPU transfers, so that test is gone.
    P("CPU->VRAM: not CPU-accessible (crashed run 2)\n");
    linearFree(src);
    linearFree(rb);
    vramFree(vr);
}

// ============================================================================================================
// 2. SD storage
// ============================================================================================================
#define SD_DIR "sdmc:/gcbench"
#define SD_TMP "sdmc:/gcbench/tmp.bin"
#define FS_TMP "/gcbench/tmp.bin"

typedef struct { u32 n; u64 bytes; double secs; int eno; Result rc; } ReadRes;

static Result fs_open_tmp(Handle* h) {
    return FSUSER_OpenFileDirectly(h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, FS_TMP),
                                   FS_OPEN_READ, 0);
}

static void print_read(const char* name, const ReadRes* r, int perRead) {
    if (r->n == 0 || r->secs <= 0) {
        if (r->rc) P("%s: FAILED %08lx\n", name, (unsigned long)r->rc);
        else P("%s: FAILED errno %d\n", name, r->eno);
        return;
    }
    double mbs = r->bytes / (double)MiB / r->secs;
    if (perRead) P("%s: %.2f MB/s %.2f ms/rd\n", name, mbs, r->secs * 1e3 / r->n);
    else P("%s: %.2f MB/s\n", name, mbs);
    if (r->rc) P("  (stopped early: %08lx)\n", (unsigned long)r->rc);
    else if (r->eno) P("  (stopped early: errno %d)\n", r->eno);
}

static ReadRes sd_seq_stdio(u8* buf, u8* vbuf, double cap) {
    ReadRes r = { 0 };
    errno = 0;
    FILE* f = fopen(SD_TMP, "rb");
    if (!f) { r.eno = errno ? errno : -1; return r; }
    setvbuf(f, (char*)vbuf, _IOFBF, MiB);
    double t0 = now_s();
    for (;;) {
        size_t got = fread(buf, 1, MiB, f);
        r.bytes += got;
        if (got) { r.n++; g_sink += buf[got / 2]; }
        if (got != MiB) { if (ferror(f)) r.eno = errno ? errno : -1; break; }
        if (now_s() - t0 > cap) break;
    }
    r.secs = now_s() - t0;
    fclose(f);
    return r;
}

static ReadRes sd_seq_native(u8* buf, u32 fsize, double cap) {
    ReadRes r = { 0 };
    Handle h;
    Result rc = fs_open_tmp(&h);
    if (R_FAILED(rc)) { r.rc = rc; return r; }
    double t0 = now_s();
    for (u64 off = 0; off < fsize; off += MiB) {
        u32 got = 0;
        rc = FSFILE_Read(h, &got, off, buf, MiB);
        if (R_FAILED(rc)) { r.rc = rc; break; }
        r.bytes += got;
        r.n++;
        g_sink += buf[MiB / 2];
        if (got != MiB || now_s() - t0 > cap) break;
    }
    r.secs = now_s() - t0;
    FSFILE_Close(h);
    return r;
}

static ReadRes sd_rand_stdio(u32 fsize, u32 blk, u8* buf, u8* vbuf, size_t vsize, u32 maxn, double cap) {
    ReadRes r = { 0 };
    errno = 0;
    FILE* f = fopen(SD_TMP, "rb");
    if (!f) { r.eno = errno ? errno : -1; return r; }
    setvbuf(f, (char*)vbuf, _IOFBF, vsize);
    u32 nblk = fsize / blk;
    double t0 = now_s();
    while (r.n < maxn) {
        long off = (long)(rnd_below(nblk) * blk);
        if (fseek(f, off, SEEK_SET) != 0 || fread(buf, 1, blk, f) != blk) { r.eno = errno ? errno : -1; break; }
        g_sink += buf[blk / 2];
        r.n++;
        r.bytes += blk;
        if (now_s() - t0 > cap) break;
    }
    r.secs = now_s() - t0;
    fclose(f);
    return r;
}

static ReadRes sd_rand_native(u32 fsize, u32 blk, u8* buf, u32 maxn, double cap) {
    ReadRes r = { 0 };
    Handle h;
    Result rc = fs_open_tmp(&h);
    if (R_FAILED(rc)) { r.rc = rc; return r; }
    u32 nblk = fsize / blk;
    double t0 = now_s();
    while (r.n < maxn) {
        u32 got = 0;
        rc = FSFILE_Read(h, &got, (u64)rnd_below(nblk) * blk, buf, blk);
        if (R_FAILED(rc) || got != blk) { r.rc = R_FAILED(rc) ? rc : (Result)0xFFFFFFFF; break; }
        g_sink += buf[blk / 2];
        r.n++;
        r.bytes += blk;
        if (now_s() - t0 > cap) break;
    }
    r.secs = now_s() - t0;
    FSFILE_Close(h);
    return r;
}

static void bench_storage(void) {
    P("disc rates: GC ~2-3 MB/s, Wii ~6-9 MB/s (approx.)\n");
    step("sd setup");
    u8* buf = malloc(MiB);
    u8* vbuf = malloc(MiB);
    if (!buf || !vbuf) {
        P("SD: SKIPPED: malloc 2x1MB failed\n");
        free(buf); free(vbuf);
        return;
    }
    if (mkdir(SD_DIR, 0777) != 0 && errno != EEXIST) {
        P("SD: SKIPPED: mkdir %s errno %d\n", SD_DIR, errno);
        free(buf); free(vbuf);
        return;
    }

    step("sd write 32MB");
    u32 written = 0;
    FILE* f = fopen(SD_TMP, "wb");
    if (!f) {
        P("SD write: SKIPPED: fopen errno %d\n", errno);
        goto cleanup;
    }
    setvbuf(f, (char*)vbuf, _IOFBF, MiB);
    fill_random(buf, MiB);
    double t0 = now_s();
    int werr = 0;
    errno = 0;
    while (written < 32u * MiB) {
        buf[0] = (u8)(written >> 20);
        if (fwrite(buf, 1, MiB, f) != MiB) { werr = errno ? errno : -1; break; }
        written += MiB;
        if (now_s() - t0 > 6.0) break;  // bound the test on a slow card
    }
    if (fclose(f) != 0 && !werr) werr = errno ? errno : -1;
    double wt = now_s() - t0;
    if (werr) P("SD write: error errno %d after %lu MB\n", werr, (unsigned long)(written >> 20));
    if (written) P("SD write %luMB: %.2f MB/s\n", (unsigned long)(written >> 20), written / (double)MiB / wt);
    if (written < 4u * MiB || werr) {
        P("SD reads: SKIPPED: test file not written\n");
        goto cleanup;
    }

    {
        ReadRes r;
        step("sd seq read stdio");
        r = sd_seq_stdio(buf, vbuf, 4.0);
        print_read("seq 1M stdio", &r, 0);
        step("sd seq read native");
        r = sd_seq_native(buf, written, 4.0);
        print_read("seq 1M FSFILE", &r, 0);

        step("sd rand 32K stdio buf1M");
        r = sd_rand_stdio(written, 32768, buf, vbuf, MiB, 200, 2.0);
        print_read("rnd 32K stdio buf1M", &r, 1);
        step("sd rand 32K stdio buf32K");
        r = sd_rand_stdio(written, 32768, buf, vbuf, 32768, 300, 2.0);
        print_read("rnd 32K stdio buf32K", &r, 1);
        step("sd rand 32K native");
        r = sd_rand_native(written, 32768, buf, 300, 2.0);
        print_read("rnd 32K FSFILE", &r, 1);

        step("sd rand 2K stdio buf2K");
        r = sd_rand_stdio(written, 2048, buf, vbuf, 2048, 1000, 2.0);
        print_read("rnd 2K stdio buf2K", &r, 1);
        step("sd rand 2K native");
        r = sd_rand_native(written, 2048, buf, 1000, 2.0);
        print_read("rnd 2K FSFILE", &r, 1);
    }

cleanup:
    step("sd cleanup");
    if (remove(SD_TMP) != 0 && errno != ENOENT) P("SD: remove tmp.bin failed errno %d\n", errno);
    free(buf);
    free(vbuf);
}

// ============================================================================================================
// 3. Crypto
// ============================================================================================================
static void bench_crypto(void) {
    const size_t sz = MiB;
    u8* in = malloc(sz);
    u8* out = malloc(sz);
    step("crypto setup");
    if (!in || !out) {
        P("crypto: SKIPPED: malloc 2x1MB failed\n");
        free(in); free(out);
        return;
    }
    fill_random(in, sz);

    // Known-answer tests first, so a broken library cannot report a speed.
    step("crypto KAT");
    static const u8 fipsKey[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const u8 fipsPt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const u8 fipsCt[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    static const u8 sha1Abc[20] = { 0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d };
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    u8 tmp[20];
    int aesOk = mbedtls_aes_setkey_dec(&ctx, fipsKey, 128) == 0 &&
                mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, fipsCt, tmp) == 0 && memcmp(tmp, fipsPt, 16) == 0;
    int shaOk = mbedtls_sha1_ret((const unsigned char*)"abc", 3, tmp) == 0 && memcmp(tmp, sha1Abc, 20) == 0;
    P("mbedtls KAT: AES %s, SHA-1 %s\n", aesOk ? "ok" : "FAILED", shaOk ? "ok" : "FAILED");

    // Wii cluster layout: 0x400 hash block (IV 0) + 0x7C00 data (IV = encrypted hash block bytes 0x3D0..0x3DF).
    step("crypto AES-128-CBC sw 8MB");
    double aesMBs = 0;
    if (!aesOk) {
        P("AES sw: SKIPPED: known-answer test failed\n");
    } else {
        u8 key[16];
        fill_random(key, sizeof key);
        int rc = mbedtls_aes_setkey_dec(&ctx, key, 128);
        double t0 = now_s();
        for (int pass = 0; pass < 8 && rc == 0; pass++) {
            for (u32 base = 0; base < sz && rc == 0; base += 0x8000) {
                u8 iv[16] = { 0 };
                rc |= mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, 0x400, iv, in + base, out + base);
                memcpy(iv, in + base + 0x3D0, 16);
                rc |= mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, 0x7C00, iv, in + base + 0x400, out + base + 0x400);
            }
        }
        double t = now_s() - t0;
        g_sink += out[sz / 2];
        if (rc) P("AES sw: FAILED mbedtls rc %d\n", rc);
        else { aesMBs = 8.0 / t; P("AES-128-CBC sw (Wii clusters): %.1f MB/s\n", aesMBs); }
    }
    mbedtls_aes_free(&ctx);

    step("crypto AES hw PS");
    Result pr = psInit();
    if (R_FAILED(pr)) P("PS service: psInit %08lx\n", (unsigned long)pr);
    else { P("PS service: psInit ok\n"); psExit(); }
    P("AES hw: not measured: PS_EncryptDecryptAes only\n");
    P("  takes fixed keyslots, no arbitrary/Wii key\n");

    step("crypto SHA-1 8MB");
    double shaBulk = 0, shaH0 = 0;
    if (!shaOk) {
        P("SHA-1: SKIPPED: known-answer test failed\n");
    } else {
        int rc = 0;
        u8 dig[20];
        double t0 = now_s();
        for (int pass = 0; pass < 8 && rc == 0; pass++) {
            in[pass] ^= 1;
            rc |= mbedtls_sha1_ret(in, sz, dig);
            g_sink += dig[pass];
        }
        double t = now_s() - t0;
        if (!rc) shaBulk = 8.0 / t;
        // Wii H0: one SHA-1 per 1 KB block of decrypted data.
        t0 = now_s();
        for (int pass = 0; pass < 8 && rc == 0; pass++) {
            for (u32 off = 0; off < sz && rc == 0; off += 1024) {
                rc |= mbedtls_sha1_ret(in + off, 1024, dig);
                g_sink += dig[0];
            }
        }
        t = now_s() - t0;
        if (rc) P("SHA-1: FAILED mbedtls rc %d\n", rc);
        else { shaH0 = 8.0 / t; P("SHA-1 sw: %.1f MB/s bulk, %.1f MB/s 1KB\n", shaBulk, shaH0); }
    }

    // Budget: raw cluster bytes = user bytes * 32/31, all of which are AES-decrypted.
    const double lo = 6.0 * 32 / 31, hi = 9.0 * 32 / 31;
    P("Wii 6-9 MB/s needs AES %.1f-%.1f MB/s\n", lo, hi);
    if (aesMBs > 0) P("  -> AES %.0f-%.0f%% of one core\n", 100 * lo / aesMBs, 100 * hi / aesMBs);
    if (aesMBs > 0 && shaH0 > 0)
        P("  -> AES+H0 SHA-1 %.0f-%.0f%% of one core\n", 100 * (lo / aesMBs + 6.0 / shaH0),
          100 * (hi / aesMBs + 9.0 / shaH0));
    free(in);
    free(out);
}

// ============================================================================================================
// 4. Audio: DSP-ADPCM decode + linear resample + stereo mix, as the GameCube DSP's AX microcode does
// ============================================================================================================
#define OUT_RATE 32000
#define BLOCK 160  // 5 ms, the AX frame size at 32 kHz
#define VOICES 64

typedef struct {
    const u8* data;
    u32 nframes, frame, nib;
    s16 coef[16];
    s32 c1, c2, shift;  // predictor of the current frame
    s32 h1, h2;
    u32 frac, step;  // 16.16 source position step per output sample
    s32 s0, s1;      // interpolation pair
    s32 volL, volR;  // Q15
} Voice;

static inline s32 adpcm_next(Voice* v) {
    const u8* fr = v->data + v->frame * 8;
    if (v->nib == 0) {
        u8 ps = fr[0];
        int pi = (ps >> 4) & 7;
        v->c1 = v->coef[pi * 2];
        v->c2 = v->coef[pi * 2 + 1];
        v->shift = ps & 0xF;
    }
    u8 b = fr[1 + (v->nib >> 1)];
    s32 n = (v->nib & 1) ? (b & 0xF) : (b >> 4);
    n = (n ^ 8) - 8;
    // s32 like Dolphin; synthetic coefficients and shifts below keep this from overflowing.
    s32 s = (v->c1 * v->h1 + v->c2 * v->h2 + n * (2048 << v->shift) + 1024) >> 11;
    if (s > 32767) s = 32767;
    else if (s < -32768) s = -32768;
    v->h2 = v->h1;
    v->h1 = s;
    if (++v->nib == 14) {
        v->nib = 0;
        if (++v->frame == v->nframes) v->frame = 0;
    }
    return s;
}

static void mix_voice(Voice* v, s32* accL, s32* accR, int n) {
    u32 frac = v->frac, step = v->step;
    s32 s0 = v->s0, s1 = v->s1, vl = v->volL, vr = v->volR;
    for (int i = 0; i < n; i++) {
        s32 smp = s0 + (((s1 - s0) * (s32)(frac >> 4)) >> 12);
        accL[i] += (smp * vl) >> 15;
        accR[i] += (smp * vr) >> 15;
        frac += step;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            s0 = s1;
            s1 = adpcm_next(v);
        }
    }
    v->frac = frac;
    v->s0 = s0;
    v->s1 = s1;
}

static void audio_mix(void) {
    step("audio ADPCM mix 64 voices");
    const u32 poolFrames = 8192;  // 64 KB of ADPCM, ~3.6 s at 32 kHz
    u8* pool = malloc(poolFrames * 8);
    Voice* vs = calloc(VOICES, sizeof(Voice));
    s16* out = malloc(OUT_RATE * 2 * sizeof(s16));
    if (!pool || !vs || !out) {
        P("ADPCM mix: SKIPPED: malloc failed\n");
        free(pool); free(vs); free(out);
        return;
    }
    fill_random(pool, poolFrames * 8);
    for (u32 f = 0; f < poolFrames; f++) pool[f * 8] = (u8)(((rnd() & 7) << 4) | rnd_below(12));
    double stepSum = 0;
    for (int i = 0; i < VOICES; i++) {
        Voice* v = &vs[i];
        v->data = pool;
        v->nframes = poolFrames;
        v->frame = rnd_below(poolFrames);
        for (int p = 0; p < 8; p++) {
            // Stable 2nd-order predictors (Q11): c2 in -0.9..-0.1, 0 <= c1 < 1 + |c2|.
            s32 c2 = -(s32)(205 + rnd_below(1638));
            s32 c1 = (s32)rnd_below((u32)(2048 - c2));
            v->coef[p * 2] = (s16)c1;
            v->coef[p * 2 + 1] = (s16)c2;
        }
        // Source rates 22.05..48 kHz.
        u32 srcRate = 22050 + rnd_below(48000 - 22050);
        v->step = (u32)(((u64)srcRate << 16) / OUT_RATE);
        stepSum += v->step / 65536.0;
        v->volL = 4096 + rnd_below(12288);
        v->volR = 4096 + rnd_below(12288);
        v->s0 = adpcm_next(v);
        v->s1 = adpcm_next(v);
    }
    s32 accL[BLOCK], accR[BLOCK];
    const int blocks = OUT_RATE / BLOCK;
    int done = 0;
    double t0 = now_s(), t = 0;
    for (; done < blocks; done++) {
        memset(accL, 0, sizeof accL);
        memset(accR, 0, sizeof accR);
        for (int i = 0; i < VOICES; i++) mix_voice(&vs[i], accL, accR, BLOCK);
        s16* o = out + done * BLOCK * 2;
        for (int k = 0; k < BLOCK; k++) {
            s32 l = accL[k], r = accR[k];
            o[2 * k] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            o[2 * k + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        t = now_s() - t0;
        if (t > 5.0) { done++; break; }
    }
    u32 sum = 0;
    for (int k = 0; k < done * BLOCK * 2; k++) sum = sum * 31 + (u16)out[k];
    g_sink += sum;
    double audioSecs = done * (double)BLOCK / OUT_RATE;
    double load = t / audioSecs;  // core-seconds per second of audio, all voices
    P("ADPCM %d voices, %.2f s @32k: %.3f s (sum %08lx)\n", VOICES, audioSecs, t, (unsigned long)sum);
    P("  avg src/out ratio %.2f\n", stepSum / VOICES);
    P("  per voice: %.2f%% of one core\n", 100.0 * load / VOICES);
    P("  real-time voices per core: %.0f (AX max 64)\n", VOICES / load);
    free(pool);
    free(vs);
    free(out);
}

static void audio_ndsp(void) {
    step("audio ndspInit");
    Result r = ndspInit();
    if (R_FAILED(r)) {
        P("ndsp: SKIPPED: ndspInit %08lx\n", (unsigned long)r);
        P("  (sdmc:/3ds/dspfirm.cdc missing? dump it)\n");
        return;
    }
    step("audio ndsp tone");
    const u32 n = OUT_RATE / 2;  // 0.5 s mono
    s16* pcm = linearAlloc(n * sizeof(s16));
    if (!pcm) {
        P("ndsp: available; tone SKIPPED: linearAlloc\n");
        ndspExit();
        return;
    }
    for (u32 i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < 800) env = i / 800.0f;                // 25 ms fade in/out, no clicks
        else if (i > n - 800) env = (n - i) / 800.0f;
        pcm[i] = (s16)(2000.0f * env * sinf(2.0f * 3.14159265f * 440.0f * i / OUT_RATE));
    }
    Result fr = DSP_FlushDataCache(pcm, n * sizeof(s16));
    if (R_FAILED(fr)) {
        P("ndsp: available; tone SKIPPED: flush %08lx\n", (unsigned long)fr);
        linearFree(pcm);
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, OUT_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0 };
    mix[0] = mix[1] = 0.5f;
    ndspChnSetMix(0, mix);
    ndspWaveBuf wb;
    memset(&wb, 0, sizeof wb);
    wb.data_vaddr = pcm;
    wb.nsamples = n;
    ndspChnWaveBufAdd(0, &wb);
    double t0 = now_s(), t;
    do {
        svcSleepThread(10 * 1000 * 1000);
        t = now_s() - t0;
    } while (wb.status != NDSP_WBUF_DONE && t < 2.0);
    int done = wb.status == NDSP_WBUF_DONE;
    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    svcSleepThread(20 * 1000 * 1000);  // let the DSP drop the buffer before freeing it
    ndspExit();
    linearFree(pcm);
    if (done) P("ndsp: available; 0.5 s tone took %.2f s\n", t);
    else P("ndsp: available; tone NOT done after 2 s\n");
}

// ============================================================================================================
// 5. Timer and cross-core sync
// ============================================================================================================
typedef struct {
    LightEvent req, resp;
    volatile int quit;
    u32 spinReq, spinResp;  // accessed with __atomic builtins
} Sync;
static Sync g_sync;  // static, so a worker abandoned on timeout never points into a dead stack frame

static void ev_worker(void* arg) {
    Sync* s = arg;
    for (;;) {
        LightEvent_Wait(&s->req);
        if (s->quit) break;
        LightEvent_Signal(&s->resp);
    }
}

static void spin_worker(void* arg) {
    Sync* s = arg;
    u32 last = 0;
    while (!s->quit) {
        u32 r = __atomic_load_n(&s->spinReq, __ATOMIC_ACQUIRE);
        if (r != last) {
            last = r;
            __atomic_store_n(&s->spinResp, r, __ATOMIC_RELEASE);
        }
    }
}

static int cmp_u32(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

static void report_rt(const char* name, u32* ticks, u32 n) {
    qsort(ticks, n, sizeof(u32), cmp_u32);
    P("%s: med %.1f us (min %.1f p90 %.1f)\n", name, ticks[n / 2] / TICKS_PER_US, ticks[0] / TICKS_PER_US,
      ticks[n * 9 / 10] / TICKS_PER_US);
}

// Returns 1 if the worker exited and was freed; otherwise the thread is leaked (it only touches static g_sync).
static int stop_worker(Thread th, const char* name) {
    g_sync.quit = 1;
    LightEvent_Signal(&g_sync.req);
    Result jr = threadJoin(th, 1000000000ULL);
    if (R_SUCCEEDED(jr)) { threadFree(th); return 1; }
    P("%s: worker did not exit (%08lx); leaked\n", name, (unsigned long)jr);
    return 0;
}

static void bench_sync(void) {
    step("sync svcGetSystemTick");
    {
        const u32 n = 200000;
        u64 acc = 0;
        u64 t0 = svcGetSystemTick();
        for (u32 i = 0; i < n; i++) acc += svcGetSystemTick();
        u64 t1 = svcGetSystemTick();
        g_sink += (u32)acc;
        P("svcGetSystemTick: %.0f ns/call\n", (t1 - t0) * 1000.0 / TICKS_PER_US / n);
    }

    enum { RT = 1000 };
    u32* ticks = malloc(RT * sizeof(u32));
    if (!ticks) { P("core handoff: SKIPPED: malloc failed\n"); return; }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const u64 timeoutTicks = (u64)(SYSCLOCK_ARM11 / 10);  // 100 ms per round trip

    step("sync LightEvent core0->core2");
    memset(&g_sync, 0, sizeof g_sync);
    LightEvent_Init(&g_sync.req, RESET_ONESHOT);
    LightEvent_Init(&g_sync.resp, RESET_ONESHOT);
    int evFreed = 1;
    Thread th = threadCreate(ev_worker, &g_sync, 16 * 1024, prio, 2, false);
    if (!th) {
        P("LightEvent c0<->c2: SKIPPED: threadCreate core 2 failed\n");
    } else {
        u32 i = 0;
        for (; i < RT; i++) {
            u64 t0 = svcGetSystemTick();
            LightEvent_Signal(&g_sync.req);
            if (LightEvent_WaitTimeout(&g_sync.resp, 100000000LL)) break;
            ticks[i] = (u32)(svcGetSystemTick() - t0);
        }
        evFreed = stop_worker(th, "LightEvent");
        if (i < RT) P("LightEvent c0<->c2: FAILED: timeout at trip %lu\n", (unsigned long)i);
        else report_rt("LightEvent c0<->c2", ticks, RT);
    }

    step("sync spin core0->core2");
    if (!evFreed) {
        P("spin c0<->c2: SKIPPED: previous worker stuck\n");
        free(ticks);
        return;
    }
    memset(&g_sync, 0, sizeof g_sync);
    LightEvent_Init(&g_sync.req, RESET_ONESHOT);
    LightEvent_Init(&g_sync.resp, RESET_ONESHOT);
    th = threadCreate(spin_worker, &g_sync, 16 * 1024, prio, 2, false);
    if (!th) {
        P("spin c0<->c2: SKIPPED: threadCreate core 2 failed\n");
    } else {
        u32 i = 0;
        int timedOut = 0;
        for (; i < RT && !timedOut; i++) {
            u32 want = i + 1;
            u64 t0 = svcGetSystemTick();
            __atomic_store_n(&g_sync.spinReq, want, __ATOMIC_RELEASE);
            u32 spins = 0;
            while (__atomic_load_n(&g_sync.spinResp, __ATOMIC_ACQUIRE) != want) {
                if ((++spins & 0xFFF) == 0 && svcGetSystemTick() - t0 > timeoutTicks) { timedOut = 1; break; }
            }
            ticks[i] = (u32)(svcGetSystemTick() - t0);
        }
        stop_worker(th, "spin");  // on failure the thread is leaked; it only touches static g_sync
        if (timedOut) P("spin c0<->c2: FAILED: timeout at trip %lu\n", (unsigned long)i);
        else report_rt("spin c0<->c2", ticks, RT);
    }
    free(ticks);
}

// ============================================================================================================
void bench_sys(void) {
    double t0 = now_s();
    P("\n-- memory --\n");
    mem_limits();
    mem_bandwidth();
    mem_random();
    mem_gx();
    P("\n-- SD storage --\n");
    bench_storage();
    P("\n-- crypto (Wii discs) --\n");
    bench_crypto();
    P("\n-- audio (GC DSP) --\n");
    audio_mix();
    audio_ndsp();
    P("\n-- timer / cross-core sync --\n");
    bench_sync();
    P("sys module: %.1f s (sink %08lx)\n", now_s() - t0, (unsigned long)g_sink);
}
