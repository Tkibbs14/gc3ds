// Dev relay client. The 2DS can sit on any network: it polls a relay server (docs/REMOTE_RUNS.md) for jobs queued from the
// PC (core/tools/relay.sh), downloads a build it does not have, runs the job and streams perf.csv lines back, so runs
// need no card swap and no PC on the same network. HTTPS through libcurl + mbedTLS (portlibs), the server's roots in
// romfs:/relay_ca.pem. sdmc:/gc3ds/relay.txt holds "url <base>", "token <secret>" and optionally "auto 1"; the token
// file is never part of the repository or the build.
//
// Log lines: the emulation thread writes perf.csv through relay_tee(), which copies each write into a ring; a thread
// on core 1 posts the ring in chunks every few seconds (one curl handle, kept alive). A full ring drops lines rather
// than stall emulation (counted, reported at the end).
#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <string.h>
#include "relay.h"

#define SOC_BUF_SIZE 0x80000
#define RING_SIZE (512u * 1024)
#define CHUNK_MAX (256u * 1024)
#define POST_TICKS (2ull * SYSCLOCK_ARM11)  // a chunk at least every 2 s while lines arrive (live commands ride on it)

static char g_url[256], g_token[256];
static bool g_auto, g_started, g_sslc;
static u32* g_socBuf;
static CURL* g_curl;
static LightLock g_curlLock;
static char g_selfId[17];
static int tee_write(void* cookie, const char* p, _READ_WRITE_BUFSIZE_TYPE len);

static char* g_ring;
static volatile u32 g_head, g_tail;  // bytes ever written / ever posted (wrap at RING_SIZE)
static volatile u32 g_dropped;
static volatile unsigned g_jobId;
static int g_seq;
static volatile bool g_flushReq, g_quit;
static LightEvent g_flushed;
static Thread g_thread;

static void load_config(void) {
    if (g_url[0]) return;
    FILE* f = fopen("sdmc:/gc3ds/relay.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "url ", 4)) snprintf(g_url, sizeof g_url, "%s", line + 4);
        else if (!strncmp(line, "token ", 6)) snprintf(g_token, sizeof g_token, "%s", line + 6);
        else if (!strncmp(line, "auto ", 5)) g_auto = line[5] == '1';
    }
    fclose(f);
}

bool relay_configured(void) { load_config(); return g_url[0] && strlen(g_token) >= 32; }
bool relay_auto(void) { load_config(); return relay_configured() && g_auto; }

// ---- HTTP ----
typedef struct { char* buf; size_t len, cap; FILE* file; } Sink;

static size_t on_data(char* p, size_t sz, size_t n, void* user) {
    Sink* s = user;
    size_t k = sz * n;
    if (s->file) return fwrite(p, 1, k, s->file);
    if (s->len + k + 1 > s->cap) {
        size_t cap = (s->len + k + 1) * 2;
        char* b = realloc(s->buf, cap);
        if (!b) return 0;
        s->buf = b; s->cap = cap;
    }
    memcpy(s->buf + s->len, p, k);
    s->len += k;
    s->buf[s->len] = 0;
    return k;
}

// Network recovery: once the 2DS's sockets fail, every later request on the same session failed too ("Failed sending
// HTTP request", retrying every 10 s for good, jobs 301-307), until gc3ds restarted. After NET_FAILS failed requests
// in a row the sockets service and the curl handle are started again (call with g_curlLock held). g_netIp: the 2DS's
// address at the last failure (0: no network, so Wi-Fi dropped rather than the sockets service).
// The ballast: BALLAST_SIZE of heap taken at startup and given up at the first failed request, for the TLS reconnect
// to allocate in (the emulator leaves the heap nearly empty: "Out of memory" from curl, 2DS 2026-10-16; mbedTLS here
// cannot take its own allocator); taken back between jobs (relay_next).
#define NET_FAILS 3
#define BALLAST_SIZE (384u * 1024)
static void* g_ballast;
static u32 g_netFails;
u32 g_netResets, g_netIp;
static void net_failed(CURLcode rc) {
    if (rc == CURLE_OK) { g_netFails = 0; return; }
    if (g_ballast) { free(g_ballast); g_ballast = NULL; }
    g_netIp = (u32)gethostid();
    if (++g_netFails < NET_FAILS) return;
    g_netFails = 0;
    g_netResets++;
    curl_easy_cleanup(g_curl);
    socExit();
    socInit(g_socBuf, SOC_BUF_SIZE);
    g_curl = curl_easy_init();
}

// One request on the shared handle. Returns the HTTP status (0 when the request failed; err says why).
static long http(const char* method, const char* path, const void* body, size_t len, Sink* out, long timeout,
                 char* err, size_t en) {
    char url[384], tok[300];
    snprintf(url, sizeof url, "%s%s", g_url, path);
    snprintf(tok, sizeof tok, "x-relay-token: %s", g_token);
    struct curl_slist* h = curl_slist_append(NULL, tok);
    h = curl_slist_append(h, "Content-Type: text/plain");
    char ebuf[CURL_ERROR_SIZE] = "";
    LightLock_Lock(&g_curlLock);
    if (!g_curl && !(g_curl = curl_easy_init())) { LightLock_Unlock(&g_curlLock); curl_slist_free_all(h); if (err) snprintf(err, en, "curl handle lost"); return 0; }
    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(g_curl, CURLOPT_CAINFO, "romfs:/relay_ca.pem");
    curl_easy_setopt(g_curl, CURLOPT_USERAGENT, "gc3ds-relay/1");
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(g_curl, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, out);
    if (!strcmp(method, "POST")) {
        curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE, (long)len);
    }
    CURLcode rc = curl_easy_perform(g_curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    net_failed(rc);
    LightLock_Unlock(&g_curlLock);
    curl_slist_free_all(h);
    if (rc != CURLE_OK && err) snprintf(err, en, "%s (ip %08lx, network resets %lu)", ebuf[0] ? ebuf : curl_easy_strerror(rc), (unsigned long)g_netIp, (unsigned long)g_netResets);
    else if (status >= 400 && err) snprintf(err, en, "HTTP %ld: %.80s", status, out && out->buf ? out->buf : "");
    return status;
}

// ---- live commands and frames ----
// Commands for the running job arrive in the replies to log posts ("cmd <text>" lines) and wait here for the emulation
// thread (relay_take_cmd). Frames are BMP files the emulation thread saved; this thread uploads them.
#define CMD_N 16
static char g_cmd[CMD_N][128];
static volatile u32 g_cmdHead, g_cmdTail;
// One frame waits at a time, copied from memory (2DS, 2026-10-13: a Luma exception in Process9, the ARM9's file system
// server, died writing perf.csv while this thread read frames back from the card; nothing here touches the card now).
static u8* g_frameBuf;
static u32 g_frameLen;
static volatile u32 g_frameHead, g_frameTail;
static u32 g_frameNo;

static void take_cmds(const char* reply) {
    for (const char* p = reply ? strstr(reply, "cmd ") : NULL; p; p = strstr(p + 1, "\ncmd ")) {
        if (*p == '\n') p++;
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (g_cmdHead - g_cmdTail < CMD_N && len > 4) {
            snprintf(g_cmd[g_cmdHead % CMD_N], sizeof g_cmd[0], "%.*s", (int)(len - 4), p + 4);
            __sync_synchronize();
            g_cmdHead = g_cmdHead + 1;
        }
        if (!e) break;
    }
}

bool relay_take_cmd(char* out, size_t n) {
    if (g_cmdTail == g_cmdHead) return false;
    __sync_synchronize();
    snprintf(out, n, "%s", g_cmd[g_cmdTail % CMD_N]);
    g_cmdTail = g_cmdTail + 1;
    return true;
}

void relay_frame(const void* bmp, unsigned bytes) {
    if (!g_started || !g_jobId || g_frameHead != g_frameTail || !bytes) return;  // one at a time: busy, dropped
    if (!g_frameBuf || g_frameLen < bytes) {
        u8* b = realloc(g_frameBuf, bytes);
        if (!b) return;
        g_frameBuf = b;
    }
    memcpy(g_frameBuf, bmp, bytes);
    g_frameLen = bytes;
    __sync_synchronize();
    g_frameHead = g_frameHead + 1;
}

static void post_frame(void) {
    if (g_frameTail == g_frameHead || !g_jobId) return;
    __sync_synchronize();
    char url[64];
    snprintf(url, sizeof url, "/jobs/%u/frame?n=%lu", g_jobId, (unsigned long)g_frameNo++);
    Sink s = { 0 };
    http("POST", url, g_frameBuf, g_frameLen, &s, 120, NULL, 0);
    free(s.buf);
    __sync_synchronize();
    g_frameTail = g_frameTail + 1;  // sent or not: a frame is not worth holding up the next
}

// ---- log thread ----
static bool post_chunk(void) {
    u32 head = g_head, tail = g_tail;
    __sync_synchronize();
    u32 n = head - tail;
    if (!n || !g_jobId) return true;
    if (n > CHUNK_MAX) n = CHUNK_MAX;
    char* buf = malloc(n);
    if (!buf) return false;
    for (u32 i = 0; i < n; i++) buf[i] = g_ring[(tail + i) % RING_SIZE];
    char path[64];
    snprintf(path, sizeof path, "/jobs/%u/log?seq=%d", g_jobId, g_seq);
    Sink s = { 0 };
    long st = http("POST", path, buf, n, &s, 60, NULL, 0);
    free(buf);
    if (st == 200) take_cmds(s.buf);
    free(s.buf);
    if (st != 200) return false;
    g_seq++;
    __sync_synchronize();
    g_tail = tail + n;
    return true;
}

static void log_thread(void* arg) {
    (void)arg;
    u64 last = svcGetSystemTick();
    while (!g_quit) {
        svcSleepThread(200000000LL);
        post_frame();
        bool flush = g_flushReq;
        u32 pending = g_head - g_tail;
        if (!flush && (!pending || (pending < 64 * 1024 && svcGetSystemTick() - last < POST_TICKS))) continue;
        // A flush sends everything (several chunks if needed); a failed post is retried on the next pass, and a
        // flush gives up after a few failures so the run can end.
        for (int tries = 0; g_head != g_tail && tries < (flush ? 6 : 1);) {
            if (post_chunk()) continue;
            tries++;
            if (flush) svcSleepThread(2000000000LL);
        }
        last = svcGetSystemTick();
        if (flush) { g_flushReq = false; LightEvent_Signal(&g_flushed); }
    }
}

// ---- public ----
// The sockets buffer, taken at startup before the emulator's large allocations (MEM1, ARAM, the JIT's code): a 28 MB
// build left too little heap afterwards and the relay failed with "socInit failed" (2DS, 2026-10-14).
void relay_reserve(void) {
    if (relay_configured() && !g_socBuf) g_socBuf = memalign(0x1000, SOC_BUF_SIZE);
    if (relay_configured() && !g_ballast) g_ballast = malloc(BALLAST_SIZE);
}

bool relay_start(char* err, size_t n) {
    if (g_started) return true;
    if (!relay_configured()) { snprintf(err, n, "no url/token in sdmc:/gc3ds/relay.txt"); return false; }
    if (!g_socBuf) g_socBuf = memalign(0x1000, SOC_BUF_SIZE);
    if (!g_socBuf || R_FAILED(socInit(g_socBuf, SOC_BUF_SIZE))) { snprintf(err, n, "socInit failed"); return false; }
    g_sslc = R_SUCCEEDED(sslcInit(0));  // mbedTLS's entropy source (sslcGenerateRandomData)
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || !(g_curl = curl_easy_init())) { snprintf(err, n, "curl init failed"); return false; }
    LightLock_Init(&g_curlLock);
    LightEvent_Init(&g_flushed, RESET_ONESHOT);
    g_ring = malloc(RING_SIZE);
    if (!g_ring) { snprintf(err, n, "out of memory"); return false; }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(log_thread, NULL, 16 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, 1, false);
    if (!g_thread) g_thread = threadCreate(log_thread, NULL, 16 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, 0, false);
    if (!g_thread) { snprintf(err, n, "no log thread"); return false; }
    g_started = true;
    return true;
}

const char* relay_self_id(const char* selfPath) {
    if (g_selfId[0]) return g_selfId;
    FILE* f = selfPath ? fopen(selfPath, "rb") : NULL;
    if (!f) return "-";
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts_ret(&c, 0);
    static u8 buf[64 * 1024];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update_ret(&c, buf, k);
    fclose(f);
    u8 d[32];
    mbedtls_sha256_finish_ret(&c, d);
    mbedtls_sha256_free(&c);
    for (int i = 0; i < 8; i++) snprintf(g_selfId + 2 * i, 3, "%02x", d[i]);
    return g_selfId;
}

int relay_next(RelayJob* j, char* err, size_t n) {
    if (!g_ballast) g_ballast = malloc(BALLAST_SIZE);  // between jobs: take the ballast back (net_failed)
    Sink s = { 0 };
    long st = http("GET", "/next", NULL, 0, &s, 30, err, n);
    int r = -1;
    if (st == 204) r = 0;
    else if (st == 200 && s.buf) {
        memset(j, 0, sizeof *j);
        const char* p = strstr(s.buf, "job ");
        const char* b = strstr(s.buf, "\nbuild ");
        const char* a = strstr(s.buf, "\nargs ");
        if (p && b && a) {
            j->id = (unsigned)strtoul(p + 4, NULL, 10);
            snprintf(j->build, sizeof j->build, "%.16s", b + 7);
            j->build[strcspn(j->build, "\r\n")] = 0;
            snprintf(j->args, sizeof j->args, "%s", a + 6);
            j->args[strcspn(j->args, "\r\n")] = 0;
            r = 1;
        } else snprintf(err, n, "bad reply from /next");
    }
    free(s.buf);
    return r;
}

// Download progress on the bottom screen, at most every half second.
static u64 g_dlShown;
static int on_progress(void* user, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)user; (void)ut; (void)un;
    u64 t = svcGetSystemTick();
    if (t - g_dlShown < SYSCLOCK_ARM11 / 2) return 0;
    g_dlShown = t;
    if (total > 0) printf("\r  %lu / %lu KB   ", (unsigned long)(now >> 10), (unsigned long)(total >> 10));
    else printf("\r  %lu KB   ", (unsigned long)(now >> 10));
    return 0;
}

// A build into `f`: gzipped on the wire (the relay compresses it, curl inflates it), progress shown, and given up
// when it moves less than 512 bytes/s for 30 s (a stall, instead of waiting out the whole timeout). The status, or 0.
static long download(const char* path, FILE* f, char* err, size_t en) {
    char url[384], tok[300];
    snprintf(url, sizeof url, "%s%s", g_url, path);
    snprintf(tok, sizeof tok, "x-relay-token: %s", g_token);
    struct curl_slist* h = curl_slist_append(NULL, tok);
    char ebuf[CURL_ERROR_SIZE] = "";
    Sink s = { .file = f };
    LightLock_Lock(&g_curlLock);
    if (!g_curl) g_curl = curl_easy_init();
    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(g_curl, CURLOPT_CAINFO, "romfs:/relay_ca.pem");
    curl_easy_setopt(g_curl, CURLOPT_USERAGENT, "gc3ds-relay/1");
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 900L);
    curl_easy_setopt(g_curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(g_curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(g_curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(g_curl, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    g_dlShown = 0;
    CURLcode rc = curl_easy_perform(g_curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    net_failed(rc);
    LightLock_Unlock(&g_curlLock);
    curl_slist_free_all(h);
    printf("\n");
    if (rc != CURLE_OK) snprintf(err, en, "%s", ebuf[0] ? ebuf : curl_easy_strerror(rc));
    else if (status != 200) snprintf(err, en, "HTTP %ld", status);
    return status;
}

bool relay_fetch_build(const char* id, const char* path, char* err, size_t n) {
    char tmp[300], url[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    snprintf(url, sizeof url, "/build/%s", id);
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        FILE* f = fopen(tmp, "wb");
        if (!f) { snprintf(err, n, "cannot write %s", tmp); return false; }
        u64 t0 = svcGetSystemTick();
        long st = download(url, f, err, n);
        ok = fclose(f) == 0 && st == 200;
        double secs = (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11;
        char note[300];
        snprintf(note, sizeof note, "# relay: build %s download attempt %d %s after %.1f s%s%s\n", id, attempt,
                 ok ? "done" : "failed", secs, ok ? "" : ": ", ok ? "" : err);
        tee_write(NULL, note, (int)strlen(note));
        if (!ok) { remove(tmp); printf("download failed (%s), attempt %d of 3\n", err, attempt); }
    }
    if (!ok) return false;
    printf("verifying...\n");
    // Check what arrived before replacing the running build.
    char was[17];
    snprintf(was, sizeof was, "%s", g_selfId);
    g_selfId[0] = 0;
    const char* got = relay_self_id(tmp);
    bool match = !strcmp(got, id);
    snprintf(g_selfId, sizeof g_selfId, "%s", was);
    if (!match) { remove(tmp); snprintf(err, n, "download does not match %s", id); return false; }
    // RomFS reads from our own .3dsx and keeps it open, so it must be closed before the file can be replaced (2DS,
    // 2026-10-12: "cannot replace"). The caller leaves right after a success; on failure RomFS comes back (the
    // relay's CA file lives there).
    printf("installing...\n");
    romfsExit();
    remove(path);
    if (rename(tmp, path)) {
        // Fallback: copy the bytes over.
        FILE* in = fopen(tmp, "rb");
        FILE* out = in ? fopen(path, "wb") : NULL;
        bool copied = in && out;
        static u8 buf[64 * 1024];
        size_t k;
        while (copied && (k = fread(buf, 1, sizeof buf, in)) > 0) copied = fwrite(buf, 1, k, out) == k;
        if (in) fclose(in);
        if (out && fclose(out)) copied = false;
        if (!copied) { romfsInit(); snprintf(err, n, "cannot replace %s (new build left at %s)", path, tmp); return false; }
        remove(tmp);
    }
    return true;
}

void relay_job_begin(unsigned id, int firstSeq) {
    g_cmdTail = g_cmdHead;
    g_frameTail = g_frameHead;
    g_frameNo = 0;
    g_head = g_tail = 0;
    g_dropped = 0;
    g_seq = firstSeq;  // the relay upserts chunks by number: a resumed job must not reuse the numbers it already sent
    __sync_synchronize();
    g_jobId = id;
}

static int tee_write(void* cookie, const char* p, _READ_WRITE_BUFSIZE_TYPE len) {
    if (cookie) fwrite(p, 1, (size_t)len, cookie);
    if (g_jobId && len > 0) {
        u32 head = g_head, tail = g_tail;
        if ((u32)len <= RING_SIZE - (head - tail)) {
            for (u32 i = 0; i < (u32)len; i++) g_ring[(head + i) % RING_SIZE] = p[i];
            __sync_synchronize();
            g_head = head + (u32)len;
        } else g_dropped += (u32)len;
    }
    return len;
}
static int tee_close(void* cookie) { return cookie ? fclose(cookie) : 0; }
void relay_ring_note(const char* s) { tee_write(NULL, s, (_READ_WRITE_BUFSIZE_TYPE)strlen(s)); }

FILE* relay_tee(FILE* f) {
    if (!g_started || !g_jobId) return f;
    FILE* t = funopen(f, NULL, tee_write, NULL, tee_close);
    return t ? t : f;
}

void relay_flush(void) {
    if (!g_started || !g_jobId) return;
    g_flushReq = true;
    LightEvent_WaitTimeout(&g_flushed, 30000000000LL);
}

void relay_job_end(const char* status) {
    if (!g_started || !g_jobId) return;
    if (g_dropped) {
        char line[96];
        int k = snprintf(line, sizeof line, "# relay: %lu bytes of log dropped (ring full)\n", (unsigned long)g_dropped);
        tee_write(NULL, line, k);
    }
    g_flushReq = true;
    LightEvent_WaitTimeout(&g_flushed, 60000000000LL);
    char path[48];
    snprintf(path, sizeof path, "/jobs/%u/done", g_jobId);
    Sink s = { 0 };
    http("POST", path, status, strlen(status), &s, 30, NULL, 0);
    free(s.buf);
    g_jobId = 0;
}

void relay_stop(void) {
    if (!g_started) return;
    g_quit = true;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    curl_easy_cleanup(g_curl);
    curl_global_cleanup();
    if (g_sslc) sslcExit();
    socExit();
    g_started = false;
}
