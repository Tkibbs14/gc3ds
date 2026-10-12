// Dev relay client. The 2DS can sit on any network: it polls nakama-backend's /api/devrelay for jobs queued from the
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
#define POST_TICKS (5ull * SYSCLOCK_ARM11)  // a chunk at least every 5 s while lines arrive

static char g_url[256], g_token[256];
static bool g_auto, g_started, g_sslc;
static u32* g_socBuf;
static CURL* g_curl;
static LightLock g_curlLock;
static char g_selfId[17];

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
    LightLock_Unlock(&g_curlLock);
    curl_slist_free_all(h);
    if (rc != CURLE_OK && err) snprintf(err, en, "%s", ebuf[0] ? ebuf : curl_easy_strerror(rc));
    else if (status >= 400 && err) snprintf(err, en, "HTTP %ld: %.80s", status, out && out->buf ? out->buf : "");
    return status;
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
bool relay_start(char* err, size_t n) {
    if (g_started) return true;
    if (!relay_configured()) { snprintf(err, n, "no url/token in sdmc:/gc3ds/relay.txt"); return false; }
    g_socBuf = memalign(0x1000, SOC_BUF_SIZE);
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

bool relay_fetch_build(const char* id, const char* path, char* err, size_t n) {
    char tmp[300], url[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { snprintf(err, n, "cannot write %s", tmp); return false; }
    snprintf(url, sizeof url, "/build/%s", id);
    Sink s = { .file = f };
    long st = http("GET", url, NULL, 0, &s, 300, err, n);
    bool ok = fclose(f) == 0 && st == 200;
    if (!ok) { remove(tmp); return false; }
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

void relay_job_begin(unsigned id) {
    g_head = g_tail = 0;
    g_dropped = 0;
    g_seq = 0;
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

FILE* relay_tee(FILE* f) {
    if (!g_started || !g_jobId) return f;
    FILE* t = funopen(f, NULL, tee_write, NULL, tee_close);
    return t ? t : f;
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
