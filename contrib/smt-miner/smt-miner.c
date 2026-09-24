/*
 * smt-miner: native solo CPU miner for Smartiecoin (Yespower; N=256, r=8 after the v0.5.0 fork).
 *
 * Talks to a local Smartiecoin node over JSON-RPC (getblocktemplate / submitblock) and hashes with the
 * node's own yespower_hash(), so a nonce found here is valid for the node's consensus code.
 * Nothing in this program changes consensus; it only builds and submits ordinary blocks.
 *
 * The node picks its Yespower parameters from the block's nTime: nTime >= SMT_V050_FORK_TIME (1790528400,
 * consensus.nSMTv050PowTime) uses the cache-tuned v0.5.0 parameters (N=256, r=8, no personalization), anything
 * older uses the legacy ones (N=2048, r=32, or yespower 0.5 for pre-2018 timestamps). This miner tells its own
 * copy of yespower.c about that switch (yespower_set_v050_fork_time), so it always hashes exactly like the node.
 *
 * Build: see build.sh.   Usage: smt-miner <payout address> [--threads N] [--conf FILE] [--rpc URL]
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <jansson.h>
#include <math.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <sched.h>
#endif

/* ---- Windows port (compiled out on POSIX builds; Linux behaviour unchanged) ---- */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define close closesocket
#define poll WSAPoll
static void smt_usleep_win(unsigned us) { Sleep(us / 1000); }
#define usleep smt_usleep_win
static unsigned smt_sleep_win(unsigned s) { Sleep(s * 1000); return 0; }
#define sleep smt_sleep_win
static long smt_ncpu_win(void) { return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }
#define smt_ncpu() smt_ncpu_win()
#else
#define smt_ncpu() sysconf(_SC_NPROCESSORS_ONLN)
#endif
/* ------------------------------------------------------------------------------- */


extern int yespower_hash(const char *input, char *output);
extern void yespower_set_v050_fork_time(uint32_t fork_time);

/* nTime at which the node switches to the v0.5.0 Yespower parameters (src/chainparams.cpp:
 * consensus.nSMTv050PowTime). The copy of yespower.c built into this miner must be told about it: without this
 * call it would keep using the pre-fork parameters for post-fork headers and every hash would be wrong. */
#define SMT_V050_FORK_TIME 1790528400u
static void init_pow_params(void)
{
    yespower_set_v050_fork_time(SMT_V050_FORK_TIME);
}

/* Optional two-way interleaved kernel (smt-yp2.c). Built with -DSMT_NO_YP2 it is replaced by a stub. */
#ifdef SMT_NO_YP2
static int smt_yp2_supported(void) { return 0; }
static int smt_yespower_hash2(const uint8_t *a, const uint8_t *b, uint8_t *oa, uint8_t *ob)
{
    (void)a; (void)b; (void)oa; (void)ob;
    return -1;
}
#else
#include "smt-yp2.h"
#endif

#define TEMPLATE_MAX_AGE 30 /* seconds before the template is refreshed */
#define TIP_CHECK_EVERY 2   /* seconds between best-block checks */
#define STATS_EVERY 30
#define BATCH 64            /* hashes between checks for a new job */
#define MAX_SHARES_PER_TURN 32 /* Stratum: shares sent per loop turn (a sane pool difficulty never reaches this) */

/* ---------------------------------------------------------------- utilities */
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex2bin(const char *hex, uint8_t *out, size_t maxlen)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > maxlen) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

static void bin2hex(const uint8_t *in, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

static void reverse(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n / 2; i++) { uint8_t t = p[i]; p[i] = p[n - 1 - i]; p[n - 1 - i] = t; }
}

static void dsha(const uint8_t *in, size_t n, uint8_t out[32])
{
    uint8_t t[32];
    SHA256(in, n, t);
    SHA256(t, 32, out);
}

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* growable byte buffer */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static void buf_add(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void buf_u8(buf_t *b, uint8_t v) { buf_add(b, &v, 1); }
static void buf_varint(buf_t *b, uint64_t v)
{
    uint8_t t[9];
    if (v < 0xfd) { buf_u8(b, (uint8_t)v); return; }
    if (v <= 0xffff) { t[0] = 0xfd; put_le32(t + 1, (uint32_t)v); buf_add(b, t, 3); return; }
    if (v <= 0xffffffffu) { t[0] = 0xfe; put_le32(t + 1, (uint32_t)v); buf_add(b, t, 5); return; }
    t[0] = 0xff; put_le64(t + 1, v); buf_add(b, t, 9);
}
static void buf_push(buf_t *b, const void *d, size_t n) { buf_u8(b, (uint8_t)n); buf_add(b, d, n); }

/* ------------------------------------------------------------------ address */
static int b58check_p2pkh(const char *addr, uint8_t h160[20])
{
    static const char *A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    uint8_t raw[25] = {0};
    for (const char *c = addr; *c; c++) {
        const char *q = strchr(A, *c);
        if (!q || !*c) return -1;
        int carry = (int)(q - A);
        for (int i = 24; i >= 0; i--) {
            carry += 58 * raw[i];
            raw[i] = (uint8_t)(carry & 0xff);
            carry >>= 8;
        }
        if (carry) return -1;
    }
    uint8_t chk[32];
    dsha(raw, 21, chk);
    if (memcmp(chk, raw + 21, 4)) return -1;
    memcpy(h160, raw + 1, 20);
    return 0;
}

/* ---------------------------------------------------------------------- RPC */
static char g_url[256], g_userpwd[256];

typedef struct { char *p; size_t n; } mem_t;
static size_t on_data(char *d, size_t s, size_t n, void *u)
{
    mem_t *m = u;
    m->p = realloc(m->p, m->n + s * n + 1);
    memcpy(m->p + m->n, d, s * n);
    m->n += s * n;
    m->p[m->n] = 0;
    return s * n;
}

/* returns the "result" object (caller must json_decref) or NULL; error text in err */
static json_t *rpc(CURL *c, const char *method, json_t *params, char *err, size_t errlen)
{
    json_t *req = json_pack("{s:s,s:o,s:i}", "method", method, "params", params ? params : json_array(), "id", 1);
    char *body = json_dumps(req, 0);
    json_decref(req);
    mem_t m = {0};
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, g_url);
    curl_easy_setopt(c, CURLOPT_USERPWD, g_userpwd);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    free(body);
    json_t *res = NULL;
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "connection: %s", curl_easy_strerror(rc));
    } else {
        json_error_t je;
        json_t *r = json_loads(m.p ? m.p : "", 0, &je);
        if (!r) {
            snprintf(err, errlen, "bad JSON from node");
        } else {
            json_t *e = json_object_get(r, "error");
            if (e && !json_is_null(e)) {
                char *s = json_dumps(e, 0);
                snprintf(err, errlen, "%s", s);
                free(s);
            } else {
                res = json_incref(json_object_get(r, "result"));
                if (!res) res = json_null();
            }
            json_decref(r);
        }
    }
    free(m.p);
    return res;
}

/* --------------------------------------------------------------------- job */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_header[80];
static uint8_t g_target[32]; /* little-endian uint256 */
static atomic_uint g_job = 0;
static uint32_t g_found_job = 0, g_found_nonce = 0;

/* Stratum mode: keep scanning after a share and queue every share for the main thread. */
#define QMAX 256
typedef struct { unsigned job; uint32_t nonce; } share_t;
static share_t g_q[QMAX];
static int g_qn = 0;
static int g_continue = 0;
static atomic_int g_paused = 0;

typedef struct { uint64_t n; char pad[56]; } counter_t;
static counter_t *g_hashes;
static int g_threads = 1;
static int g_pin = 1;

static int le_leq(const uint8_t *h, const uint8_t *t)
{
    for (int i = 31; i >= 0; i--) {
        if (h[i] < t[i]) return 1;
        if (h[i] > t[i]) return 0;
    }
    return 1;
}

/* 1 = reference kernel, one hash at a time; 2 = interleaved kernel, two hashes at a time (see --ways) */
static int g_ways = 1;

/* Hash the header with nonce n (and, when g_ways == 2, nonce n2) into out[0] (and out[1]). */
static void hash_nonces(uint8_t h[80], uint32_t n, uint32_t n2, uint8_t out[2][32])
{
    if (g_ways == 2) {
        uint8_t h2[80];
        memcpy(h2, h, 80);
        put_le32(h + 76, n);
        put_le32(h2 + 76, n2);
        if (smt_yespower_hash2(h, h2, out[0], out[1]) == 0) return;
        yespower_hash((const char *)h2, (char *)out[1]);   /* unsupported input: reference path for both */
        yespower_hash((const char *)h, (char *)out[0]);
        return;
    }
    put_le32(h + 76, n);
    yespower_hash((const char *)h, (char *)out[0]);
}

/* Compare the interleaved kernel with the reference on pseudo-random headers. Returns 1 when identical. */
static int yp2_check(int pairs)
{
    if (!smt_yp2_supported()) return 0;
    uint32_t seed = 0x9e3779b9u;
    for (int p = 0; p < pairs; p++) {
        uint8_t a[80], b[80], oa[32], ob[32], ra[32], rb[32];
        for (int i = 0; i < 80; i++) {
            seed = seed * 1664525u + 1013904223u;
            a[i] = (uint8_t)(seed >> 24);
            seed = seed * 1664525u + 1013904223u;
            b[i] = (uint8_t)(seed >> 24);
        }
        put_le32(a + 68, SMT_V050_FORK_TIME + 1u + (uint32_t)p);      /* after the v0.5.0 fork: (N=256, r=8) */
        put_le32(b + 68, SMT_V050_FORK_TIME + 1u + (uint32_t)p * 7u);
        if (smt_yespower_hash2(a, b, oa, ob) != 0) return 0;
        yespower_hash((const char *)a, (char *)ra);
        yespower_hash((const char *)b, (char *)rb);
        if (memcmp(oa, ra, 32) || memcmp(ob, rb, 32)) return 0;
    }
    return 1;
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    if (g_pin) {
#ifdef _WIN32
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (id % smt_ncpu())); /* best effort */
#elif defined(__linux__)
        long ncpu = smt_ncpu();
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(id % ncpu, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set); /* best effort */
#else
        (void)id; /* no CPU affinity API on this OS (macOS): the scheduler decides */
#endif
    }
    uint8_t h[80], target[32], outs[2][32];
    unsigned mine = 0;
    uint32_t nonce = (uint32_t)id;
    while (!g_stop) {
        unsigned j = atomic_load(&g_job);
        if (j == 0 || atomic_load(&g_paused)) { usleep(20000); continue; }
        if (j != mine) { /* new job: take a consistent snapshot of header + target */
            pthread_mutex_lock(&g_mu);
            memcpy(h, g_header, 80);
            memcpy(target, g_target, 32);
            mine = atomic_load(&g_job);
            pthread_mutex_unlock(&g_mu);
            nonce = (uint32_t)id;
        }
        int done = 0;
        for (int i = 0; i < BATCH && !done; i += g_ways) {
            uint32_t nonces[2] = {nonce, nonce + (uint32_t)g_threads};
            hash_nonces(h, nonces[0], nonces[1], outs);
            g_hashes[id].n += (uint64_t)g_ways;
            for (int c = 0; c < g_ways; c++) {
                if (!le_leq(outs[c], target)) continue;
                pthread_mutex_lock(&g_mu);
                if (g_continue) {
                    if (g_qn < QMAX) g_q[g_qn++] = (share_t){mine, nonces[c]};
                } else if (g_found_job != mine) {
                    g_found_job = mine;
                    g_found_nonce = nonces[c];
                }
                pthread_mutex_unlock(&g_mu);
                if (!g_continue) {
                    while (!g_stop && atomic_load(&g_job) == mine) usleep(5000); /* wait for the next job */
                    done = 1;
                    break;
                }
            }
            nonce += (uint32_t)g_ways * (uint32_t)g_threads;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ block building */
typedef struct {
    uint8_t header[80];
    buf_t txs; /* varint(count) + coinbase + template txs */
    int height;
    char prevhash[65];
    uint8_t target[32];
} block_t;

static int build_block(json_t *t, const uint8_t spk[25], uint64_t extranonce, block_t *b, char *err, size_t errlen)
{
    int height = (int)json_integer_value(json_object_get(t, "height"));
    json_t *coinbasevalue = json_object_get(t, "coinbasevalue");
    if (!json_is_integer(coinbasevalue) || json_integer_value(coinbasevalue) < 0) {
        snprintf(err, errlen, "bad coinbasevalue");
        return -1;
    }
    int64_t value = (int64_t)json_integer_value(coinbasevalue);
    uint32_t version = (uint32_t)json_integer_value(json_object_get(t, "version"));
    uint32_t curtime = (uint32_t)json_integer_value(json_object_get(t, "curtime"));
    const char *bits = json_string_value(json_object_get(t, "bits"));
    const char *prev = json_string_value(json_object_get(t, "previousblockhash"));
    const char *tgt = json_string_value(json_object_get(t, "target"));
    const char *payload_hex = json_string_value(json_object_get(t, "coinbase_payload"));
    if (!bits || !prev || !tgt || strlen(prev) != 64 || strlen(tgt) != 64) { snprintf(err, errlen, "malformed template"); return -1; }

    uint8_t payload[512];
    int plen = payload_hex && *payload_hex ? hex2bin(payload_hex, payload, sizeof payload) : 0;
    if (plen < 0) { snprintf(err, errlen, "bad coinbase_payload"); return -1; }

    /* GBT coinbasevalue is GetValueOut(), including ALL required payouts.
     * Preserve the node's exact scripts, amounts and MN-then-superblock order;
     * only the remainder belongs to the miner. Never infer payouts from payee
     * addresses or the payments_started flags (an enforced output may predate
     * those flags on a test chain). */
    buf_t payments = {0};
    size_t nout = 1;
    const char *keys[] = {"masternode", "superblock"};
    for (int k = 0; k < 2; k++) {
        json_t *list = json_object_get(t, keys[k]);
        if (!list) continue;
        if (!json_is_array(list)) {
            snprintf(err, errlen, "bad %s payment list", keys[k]);
            free(payments.p);
            return -1;
        }
        for (size_t i = 0; i < json_array_size(list); i++) {
            json_t *payment = json_array_get(list, i);
            json_t *amount = json_object_get(payment, "amount");
            const char *script = json_string_value(json_object_get(payment, "script"));
            uint8_t rawscript[10000], w[8];
            int slen = script ? hex2bin(script, rawscript, sizeof rawscript) : -1;
            if (!json_is_integer(amount) || json_integer_value(amount) < 0 ||
                json_integer_value(amount) > value || slen <= 0) {
                snprintf(err, errlen, "bad %s payment (amount/script or exceeds coinbasevalue)", keys[k]);
                free(payments.p);
                return -1;
            }
            int64_t paid = json_integer_value(amount);
            value -= paid;
            put_le64(w, (uint64_t)paid);
            buf_add(&payments, w, 8);
            buf_varint(&payments, (uint64_t)slen);
            buf_add(&payments, rawscript, (size_t)slen);
            nout++;
        }
    }

    /* coinbase scriptSig: height (BIP34) + extranonce + tag */
    buf_t sig = {0};
    if (height >= 1 && height <= 16) buf_u8(&sig, (uint8_t)(0x50 + height));
    else {
        uint8_t hb[5]; int n = 0; uint32_t hv = (uint32_t)height;
        while (hv) { hb[n++] = hv & 0xff; hv >>= 8; }
        if (hb[n - 1] & 0x80) hb[n++] = 0;
        buf_push(&sig, hb, n);
    }
    uint8_t en[8];
    put_le64(en, extranonce);
    buf_push(&sig, en, 8);
    buf_push(&sig, "smartiecoin", 5);

    buf_t cb = {0};
    uint32_t cbver = plen ? (3u | (5u << 16)) : 1u;
    uint8_t w[8];
    put_le32(w, cbver); buf_add(&cb, w, 4);
    buf_u8(&cb, 1);
    uint8_t zero[32] = {0};
    buf_add(&cb, zero, 32);
    memset(w, 0xff, 4); buf_add(&cb, w, 4);
    buf_varint(&cb, sig.n); buf_add(&cb, sig.p, sig.n);
    memset(w, 0xff, 4); buf_add(&cb, w, 4);
    buf_varint(&cb, nout);
    put_le64(w, (uint64_t)value); buf_add(&cb, w, 8);
    buf_varint(&cb, 25); buf_add(&cb, spk, 25);
    if (payments.n) buf_add(&cb, payments.p, payments.n);
    free(payments.p);
    put_le32(w, 0); buf_add(&cb, w, 4);
    if (plen) { buf_varint(&cb, (uint64_t)plen); buf_add(&cb, payload, (size_t)plen); }
    free(sig.p);

    json_t *txs = json_object_get(t, "transactions");
    size_t ntx = json_is_array(txs) ? json_array_size(txs) : 0;
    uint8_t (*layer)[32] = malloc((ntx + 2) * 32);
    dsha(cb.p, cb.n, layer[0]);
    b->txs = (buf_t){0};
    buf_varint(&b->txs, ntx + 1);
    buf_add(&b->txs, cb.p, cb.n);
    free(cb.p);
    for (size_t i = 0; i < ntx; i++) {
        json_t *tx = json_array_get(txs, i);
        const char *txid = json_string_value(json_object_get(tx, "txid"));
        if (!txid) txid = json_string_value(json_object_get(tx, "hash")); /* no segwit: hash == txid */
        const char *data = json_string_value(json_object_get(tx, "data"));
        if (!txid || !data || hex2bin(txid, layer[i + 1], 32) != 32) { snprintf(err, errlen, "bad template tx"); free(layer); return -1; }
        reverse(layer[i + 1], 32);
        size_t dl = strlen(data) / 2;
        uint8_t *raw = malloc(dl);
        if (hex2bin(data, raw, dl) < 0) { snprintf(err, errlen, "bad tx data"); free(raw); free(layer); return -1; }
        buf_add(&b->txs, raw, dl);
        free(raw);
    }
    size_t cnt = ntx + 1;
    while (cnt > 1) { /* merkle root */
        if (cnt % 2) { memcpy(layer[cnt], layer[cnt - 1], 32); cnt++; }
        for (size_t i = 0; i < cnt; i += 2) {
            uint8_t cat[64];
            memcpy(cat, layer[i], 32); memcpy(cat + 32, layer[i + 1], 32);
            dsha(cat, 64, layer[i / 2]);
        }
        cnt /= 2;
    }
    uint8_t prevb[32];
    hex2bin(prev, prevb, 32); reverse(prevb, 32);
    put_le32(b->header, version);
    memcpy(b->header + 4, prevb, 32);
    memcpy(b->header + 36, layer[0], 32);
    put_le32(b->header + 68, curtime);
    put_le32(b->header + 72, (uint32_t)strtoul(bits, NULL, 16));
    put_le32(b->header + 76, 0);
    free(layer);
    hex2bin(tgt, b->target, 32); reverse(b->target, 32);
    b->height = height;
    snprintf(b->prevhash, sizeof b->prevhash, "%s", prev);
    return 0;
}

/* --------------------------------------------------------------------- conf */
static int read_conf(const char *path, char *user, size_t ul, char *pass, size_t pl, char *port, size_t ptl)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        while (*k == ' ' || *k == '\t') k++;
        if (!strcmp(k, "rpcuser")) snprintf(user, ul, "%s", v);
        else if (!strcmp(k, "rpcpassword")) snprintf(pass, pl, "%s", v);
        else if (!strcmp(k, "rpcport")) snprintf(port, ptl, "%s", v);
    }
    fclose(f);
    return 0;
}

static uint64_t total_hashes(void)
{
    uint64_t s = 0;
    for (int i = 0; i < g_threads; i++) s += g_hashes[i].n;
    return s;
}

/* ============================================================ Stratum (v1) */
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#ifndef _WIN32
#include <netdb.h>
#endif
#ifndef _WIN32
#include <poll.h>
#endif
#ifndef _WIN32
#include <sys/socket.h>
#endif

/*
 * Difficulty convention: share target = diff1 / difficulty, where diff1 is
 *   scrypt-style  0x0000ffff00..00  (default, what Yespower/scrypt coins and cpuminer use), or
 *   bitcoin-style 0x00000000ffff00..00  (--diff1 bitcoin).
 * A pool must use the same convention. Block validity is decided by the node, never by this value.
 */
static int g_diff1_bitcoin = 0;

#define NL 12 /* 384-bit little-endian big-number, 32-bit limbs */

static void bn_shl(uint32_t *a, int bits)
{
    int w = bits / 32, b = bits % 32;
    for (int i = NL - 1; i >= 0; i--) {
        uint64_t v = 0;
        if (i - w >= 0) v = (uint64_t)a[i - w] << b;
        if (b && i - w - 1 >= 0) v |= (uint64_t)a[i - w - 1] >> (32 - b);
        a[i] = (uint32_t)v;
    }
}

static void bn_shr(uint32_t *a, int bits)
{
    int w = bits / 32, b = bits % 32;
    for (int i = 0; i < NL; i++) {
        uint64_t v = 0;
        if (i + w < NL) v = (uint64_t)a[i + w] >> b;
        if (b && i + w + 1 < NL) v |= (uint64_t)a[i + w + 1] << (32 - b);
        a[i] = (uint32_t)v;
    }
}

static void bn_divsmall(uint32_t *a, uint64_t d)
{
    unsigned __int128 rem = 0;
    for (int i = NL - 1; i >= 0; i--) {
        unsigned __int128 cur = (rem << 32) | a[i];
        a[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
}

/* out = diff1 / diff as a little-endian uint256, saturating at 2^256-1 */
static void diff_to_target(double diff, uint8_t out[32])
{
    if (!(diff > 0)) diff = 1e-12;
    uint32_t a[NL] = {0};
    a[0] = 0xffff;
    bn_shl(a, g_diff1_bitcoin ? 208 : 224);
    int e;
    double m = frexp(diff, &e); /* diff = m * 2^e, m in [0.5,1) */
    uint64_t M = (uint64_t)ldexp(m, 53);
    bn_shl(a, 53);
    if (e < 0) bn_shl(a, -e); else if (e > 0) bn_shr(a, e);
    bn_divsmall(a, M);
    int over = 0;
    for (int i = 8; i < NL; i++) if (a[i]) over = 1;
    for (int i = 0; i < 8; i++) put_le32(out + 4 * i, over ? 0xffffffffu : a[i]);
}

typedef struct {
    int fd;
    char buf[1 << 16];
    size_t len;
} conn_t;

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res, *p;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_json(conn_t *c, json_t *obj)
{
    char *s = json_dumps(obj, 0);
    json_decref(obj);
    size_t n = strlen(s), off = 0;
    char *line = malloc(n + 2);
    memcpy(line, s, n);
    line[n] = '\n';
    free(s);
    n++;
    while (off < n) {
        ssize_t w = send(c->fd, line + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) { free(line); return -1; }
        off += (size_t)w;
    }
    free(line);
    return 0;
}

/* returns 1 with a malloc'ed line, 0 on timeout, -1 on error/EOF */
static int read_line(conn_t *c, int timeout_ms, char **line)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            *line = malloc(n + 1);
            memcpy(*line, c->buf, n);
            (*line)[n] = 0;
            memmove(c->buf, nl + 1, c->len - n - 1);
            c->len -= n + 1;
            return 1;
        }
        if (c->len == sizeof c->buf) return -1; /* line too long */
        struct pollfd p = {c->fd, POLLIN, 0};
        int r = poll(&p, 1, timeout_ms);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1; /* signal (Ctrl-C): caller checks g_stop */
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        if (n <= 0) return -1;
        c->len += (size_t)n;
        timeout_ms = 0; /* drain what is there, then report */
    }
}

typedef struct {
    int have;
    unsigned serial;      /* g_job value this job was published under */
    char id[128];
    uint8_t header[80];
    uint8_t en2[16];
    int en2_len;
    uint32_t ntime;
} sjob_t;

static void swap4(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i += 4) { uint8_t t; t = p[i]; p[i] = p[i + 3]; p[i + 3] = t; t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t; }
}

static unsigned publish(const uint8_t hdr[80], const uint8_t tgt[32])
{
    pthread_mutex_lock(&g_mu);
    memcpy(g_header, hdr, 80);
    memcpy(g_target, tgt, 32);
    g_found_job = 0;
    g_qn = 0;
    unsigned s = atomic_fetch_add(&g_job, 1) + 1;
    pthread_mutex_unlock(&g_mu);
    return s;
}

/* Build header from a mining.notify (params array) for our extranonce. Returns 0 on success. */
static int stratum_job(json_t *p, const uint8_t *en1, int en1_len, int en2_len, uint64_t en2ctr, sjob_t *j)
{
    if (!json_is_array(p) || json_array_size(p) < 8) return -1;
    const char *id = json_string_value(json_array_get(p, 0));
    const char *prev = json_string_value(json_array_get(p, 1));
    const char *cb1 = json_string_value(json_array_get(p, 2));
    const char *cb2 = json_string_value(json_array_get(p, 3));
    json_t *br = json_array_get(p, 4);
    const char *ver = json_string_value(json_array_get(p, 5));
    const char *nbits = json_string_value(json_array_get(p, 6));
    const char *ntime = json_string_value(json_array_get(p, 7));
    if (!id || !prev || !cb1 || !cb2 || !ver || !nbits || !ntime || strlen(prev) != 64 || en2_len < 1 || en2_len > 16) return -1;

    buf_t cb = {0};
    size_t l1 = strlen(cb1) / 2, l2 = strlen(cb2) / 2;
    uint8_t *t1 = malloc(l1 + 1), *t2 = malloc(l2 + 1);
    if (hex2bin(cb1, t1, l1) < 0 || hex2bin(cb2, t2, l2) < 0) { free(t1); free(t2); return -1; }
    memset(j->en2, 0, sizeof j->en2);
    for (int i = 0; i < en2_len && i < 8; i++) j->en2[i] = (uint8_t)(en2ctr >> (8 * i));
    j->en2_len = en2_len;
    buf_add(&cb, t1, l1);
    buf_add(&cb, en1, (size_t)en1_len);
    buf_add(&cb, j->en2, (size_t)en2_len);
    buf_add(&cb, t2, l2);
    free(t1); free(t2);

    uint8_t root[32];
    dsha(cb.p, cb.n, root);
    free(cb.p);
    size_t nb = json_is_array(br) ? json_array_size(br) : 0;
    for (size_t i = 0; i < nb; i++) {
        uint8_t cat[64];
        const char *bh = json_string_value(json_array_get(br, i));
        memcpy(cat, root, 32);
        if (!bh || hex2bin(bh, cat + 32, 32) != 32) return -1;
        dsha(cat, 64, root);
    }
    uint8_t prevb[32];
    hex2bin(prev, prevb, 32);
    swap4(prevb, 32); /* stratum sends the previous hash with every 4-byte word byte-swapped */
    put_le32(j->header, (uint32_t)strtoul(ver, NULL, 16));
    memcpy(j->header + 4, prevb, 32);
    memcpy(j->header + 36, root, 32);
    j->ntime = (uint32_t)strtoul(ntime, NULL, 16);
    put_le32(j->header + 68, j->ntime);
    put_le32(j->header + 72, (uint32_t)strtoul(nbits, NULL, 16));
    put_le32(j->header + 76, 0);
    snprintf(j->id, sizeof j->id, "%s", id);
    j->have = 1;
    return 0;
}

static int parse_url(const char *url, char *host, size_t hl, char *port, size_t pl)
{
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s) return -1;
    snprintf(host, hl, "%.*s", (int)(colon - s), s);
    snprintf(port, pl, "%s", colon + 1);
    port[strcspn(port, "/")] = 0;
    return 0;
}

/* Runs until g_stop. Reconnects automatically. */
static int run_stratum(const char *url, const char *user, const char *pass)
{
    char host[256], port[16];
    if (parse_url(url, host, sizeof host, port, sizeof port)) { fprintf(stderr, "bad stratum URL (expected stratum+tcp://host:port)\n"); return 2; }
    g_continue = 1;
    unsigned long acc = 0, rej = 0, sub = 0;
    double t0 = now_s(), last_stats = t0;
    uint64_t en2ctr = (uint64_t)time(NULL);
    long next_id = 100;

    while (!g_stop) {
        atomic_store(&g_paused, 1);
        conn_t *c = calloc(1, sizeof *c);
        c->fd = tcp_connect(host, port);
        if (c->fd < 0) {
            printf("cannot connect to %s:%s (retry in 10s)\n", host, port);
            fflush(stdout);
            free(c);
            for (int i = 0; i < 100 && !g_stop; i++) usleep(100000);
            continue;
        }
        printf("connected to %s:%s\n", host, port);
        fflush(stdout);
        uint8_t en1[32];
        int en1_len = 0, en2_len = 4, authorized = 0;
        double diff = 1e-6;
        uint8_t share_target[32];
        diff_to_target(diff, share_target);
        sjob_t cur = {0};
        int ok = send_json(c, json_pack("{s:i,s:s,s:[s]}", "id", 1, "method", "mining.subscribe", "params", "smt-miner/1.0")) == 0;
        double last_rx = now_s();
        int burst = 0;

        while (ok && !g_stop) {
            char *line = NULL;
            int r = read_line(c, 100, &line);
            if (r < 0) { printf("connection lost\n"); fflush(stdout); break; }
            if (r == 1) {
                last_rx = now_s();
                json_error_t je;
                json_t *m = json_loads(line, 0, &je);
                free(line);
                if (!m) continue;
                json_t *method = json_object_get(m, "method");
                json_t *idv = json_object_get(m, "id");
                if (json_is_string(method)) {
                    const char *mn = json_string_value(method);
                    json_t *p = json_object_get(m, "params");
                    if (!strcmp(mn, "mining.notify")) {
                        if (stratum_job(p, en1, en1_len, en2_len, ++en2ctr, &cur) == 0) {
                            cur.serial = publish(cur.header, share_target);
                            atomic_store(&g_paused, 0);
                        } else { printf("ignoring malformed mining.notify\n"); fflush(stdout); }
                    } else if (!strcmp(mn, "mining.set_difficulty") && json_is_array(p)) {
                        diff = json_number_value(json_array_get(p, 0));
                        diff_to_target(diff, share_target);
                        printf("difficulty set to %g\n", diff);
                        fflush(stdout);
                        if (cur.have) cur.serial = publish(cur.header, share_target);
                    } else if (!strcmp(mn, "mining.set_extranonce") && json_is_array(p)) {
                        const char *e1 = json_string_value(json_array_get(p, 0));
                        if (e1 && strlen(e1) / 2 <= sizeof en1) en1_len = hex2bin(e1, en1, sizeof en1);
                        en2_len = (int)json_integer_value(json_array_get(p, 1));
                    } else if (!strcmp(mn, "client.reconnect")) {
                        printf("pool asked to reconnect\n");
                        fflush(stdout);
                        json_decref(m);
                        break;
                    }
                } else if (json_is_integer(idv)) {
                    long id = (long)json_integer_value(idv);
                    json_t *res = json_object_get(m, "result");
                    json_t *er = json_object_get(m, "error");
                    if (id == 1) {
                        if (!res || !json_is_array(res)) { printf("subscribe failed\n"); fflush(stdout); json_decref(m); break; }
                        const char *e1 = json_string_value(json_array_get(res, 1));
                        en1_len = e1 && strlen(e1) / 2 <= sizeof en1 ? hex2bin(e1, en1, sizeof en1) : 0;
                        en2_len = (int)json_integer_value(json_array_get(res, 2));
                        if (en1_len < 0 || en2_len < 1 || en2_len > 16) { printf("bad extranonce from pool\n"); fflush(stdout); json_decref(m); break; }
                        send_json(c, json_pack("{s:i,s:s,s:[s,s]}", "id", 2, "method", "mining.authorize", "params", user, pass));
                    } else if (id == 2) {
                        authorized = json_is_true(res);
                        printf("authorize: %s\n", authorized ? "ok" : "REJECTED by pool");
                        fflush(stdout);
                        if (!authorized) { json_decref(m); break; }
                    } else if (id >= 100) {
                        if (json_is_true(res)) { acc++; }
                        else {
                            rej++;
                            char *s = er && !json_is_null(er) ? json_dumps(er, 0) : NULL;
                            printf("share rejected%s%s\n", s ? ": " : "", s ? s : "");
                            fflush(stdout);
                            free(s);
                        }
                    }
                }
                json_decref(m);
                /* Keep draining the socket before sending more shares. Otherwise, with a pool difficulty far too low
                 * for our hashrate (every hash is a share), the replies to our own submissions pile up and a new job
                 * notification waits behind them, so we would keep mining a stale job. */
                if (++burst < 64) continue;
            }
            burst = 0;

            /* forward any shares found by the workers for the current job */
            share_t q[QMAX];
            int qn = 0;
            pthread_mutex_lock(&g_mu);
            for (int i = 0; i < g_qn; i++) if (cur.have && g_q[i].job == cur.serial) q[qn++] = g_q[i];
            g_qn = 0;
            pthread_mutex_unlock(&g_mu);
            for (int i = 0; i < qn && i < MAX_SHARES_PER_TURN && ok; i++) {   /* extra shares are dropped */
                char en2hex[40], ntimehex[16], noncehex[16];
                bin2hex(cur.en2, (size_t)cur.en2_len, en2hex);
                snprintf(ntimehex, sizeof ntimehex, "%08x", cur.ntime);
                snprintf(noncehex, sizeof noncehex, "%08x", q[i].nonce);
                ok = send_json(c, json_pack("{s:i,s:s,s:[s,s,s,s,s]}", "id", (int)next_id++, "method", "mining.submit", "params",
                                            user, cur.id, en2hex, ntimehex, noncehex)) == 0;
                sub++;
            }

            double t = now_s();
            if (t - last_stats >= STATS_EVERY) {
                last_stats = t;
                printf("rate=%.1f kH/s shares: submitted=%lu accepted=%lu rejected=%lu\n", (double)total_hashes() / 1000.0 / (t - t0), sub, acc, rej);
                fflush(stdout);
            }
            if (t - last_rx > 300) { printf("pool silent for 300s, reconnecting\n"); fflush(stdout); break; }
        }
        atomic_store(&g_paused, 1);
        close(c->fd);
        free(c);
        for (int i = 0; i < 50 && !g_stop; i++) usleep(100000);
    }
    printf("stopped; shares submitted=%lu accepted=%lu rejected=%lu\n", sub, acc, rej);
    return 0;
}

/* ------------------------------------------------------- benchmark and self-test (no node needed) */
static volatile int g_bench_stop = 0;

/* header with a timestamp after the v0.5.0 fork, so the post-fork (N=256, r=8) parameters are measured */
static void bench_header(uint8_t h[80])
{
    memset(h, 0, 80);
    memcpy(h + 4, "smt-benchmark-header", sizeof("smt-benchmark-header") - 1);
    put_le32(h + 68, SMT_V050_FORK_TIME + 60u);
}

static void pin_thread(int id)
{
    if (!g_pin) return;
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << (id % smt_ncpu())); /* best effort */
#elif defined(__linux__)
    long ncpu = smt_ncpu();
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(id % ncpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
    (void)id; /* no CPU affinity API on this OS (macOS): the scheduler decides */
#endif
}

static void *bench_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    pin_thread(id);
    uint8_t h[80], outs[2][32];
    bench_header(h);
    uint32_t n = (uint32_t)id;
    while (!g_bench_stop) {
        for (int i = 0; i < 16; i += g_ways) {
            hash_nonces(h, n, n + (uint32_t)g_threads, outs);
            n += (uint32_t)g_ways * (uint32_t)g_threads;
        }
        g_hashes[id].n += 16;
    }
    return NULL;
}

static long meminfo_kb(const char *file, const char *key)
{
    FILE *f = fopen(file, "r");
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtol(line + kl + 1, NULL, 10); break; }
    }
    if (f) fclose(f);
    return v;
}

/* Deterministic digest of the hashes of 64 fixed inputs: identical for every build that agrees with consensus. */
static void selftest_digest(char hex[65])
{
    uint8_t h[80], out[32], all[64 * 32], d[32];
    bench_header(h);
    for (uint32_t n = 0; n < 64; n++) {
        put_le32(h + 76, n);
        yespower_hash((const char *)h, (char *)out);
        memcpy(all + 32 * n, out, 32);
    }
    SHA256(all, sizeof all, d);
    bin2hex(d, 32, hex);
}

static int run_bench(int seconds)
{
    char digest[65];
    selftest_digest(digest);
    g_hashes = calloc((size_t)g_threads, sizeof(counter_t));
    pthread_t *th = calloc((size_t)g_threads, sizeof *th);
    long hp_free0 = meminfo_kb("/proc/meminfo", "HugePages_Free");
    double t0 = now_s();
    for (int i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, bench_worker, (void *)(intptr_t)i);
    sleep((unsigned)seconds);
    long hp_free1 = meminfo_kb("/proc/meminfo", "HugePages_Free");
    long thp = meminfo_kb("/proc/self/smaps_rollup", "AnonHugePages");
    g_bench_stop = 1;
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);
    double el = now_s() - t0;
    uint64_t total = total_hashes();
    printf("threads=%d ways=%d seconds=%.1f hashrate=%.1f kH/s (%.2f kH/s per thread)\n", g_threads, g_ways, el, total / 1000.0 / el, total / 1000.0 / el / g_threads);
    if (hp_free0 < 0 && hp_free1 < 0 && thp < 0)
        printf("hugepages: not applicable on this OS (Linux-only accounting)\n");
    else
        printf("hugepages: reserved-free before=%ld during=%ld (2 MB pages), transparent huge pages in use by this process=%ld kB\n",
               hp_free0, hp_free1, thp);
    printf("selftest digest: %s\n", digest);
    return 0;
}

/* One-thread timed probe of a kernel, used to auto-pick the faster of the two when g_ways was not forced.
 * The two kernels are proven identical before this runs; this only decides which one to mine with. */
static double speed_probe(int ways, double seconds)
{
    int saved_threads = g_threads, saved_ways = g_ways;
    uint8_t h[80], outs[2][32];
    g_threads = 1;
    g_ways = ways;
    bench_header(h);
    uint32_t n = 1 + (uint32_t)ways;
    uint64_t cnt = 0;
    double t0 = now_s();
    while (now_s() - t0 < seconds) {
        for (int i = 0; i < 16; i += g_ways) {
            hash_nonces(h, n, n + 1, outs);
            n += (uint32_t)g_ways;
        }
        cnt += 16;
    }
    double el = now_s() - t0;
    g_threads = saved_threads;
    g_ways = saved_ways;
    return el > 0 ? cnt / 1000.0 / el : 0;
}

/* --------------------------------------------------------------------- main */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "smt-miner - Smartiecoin (Yespower) CPU miner\n"
        "\n"
        "usage:\n"
        "  solo:    %s <payout address> [--threads N] [--conf FILE] [--rpc URL] [--no-pin]\n"
        "  stratum: %s --stratum stratum+tcp://host:port --user <address[.worker]> [--pass x]\n"
        "                    [--threads N] [--diff1 scrypt|bitcoin] [--no-pin]\n"
        "  bench:   %s --bench [seconds] [--threads N]\n"
        "  verify:  %s --selftest\n"
        "\n"
        "quick start:\n"
        "  pool mining (no local node needed):\n"
        "    %s --stratum stratum+tcp://POOL:PORT --user YOUR_ADDRESS.rig1\n"
        "\n"
        "  solo mining (needs a local node with RPC enabled):\n"
        "    %s YOUR_ADDRESS --threads 8\n"
        "    (RPC credentials are read from ~/.smartiecoin/smartiecoin.conf;\n"
        "     default RPC port is 8282 - give YOUR_ADDRESS from: smartiecoin-cli getnewaddress)\n"
        "\n"
        "options:\n"
        "  --threads N        number of threads (default: CPU cores - 2)\n"
        "  --conf FILE        smartiecoin.conf to read RPC credentials from (solo)\n"
        "  --rpc URL          RPC endpoint (default http://127.0.0.1:8282/)\n"
        "  --ways 1|2         force kernel: 1 = reference, 2 = two-way (default: auto-tuned)\n"
        "  --bench [secs]     measure this machine's hashrate (default 10 seconds)\n"
        "  --selftest         print the consensus self-test digest and exit\n"
        "  --no-pin           don't pin threads to CPU cores\n"
        "  -h, --help         show this help\n"
        "\n"
        "Full documentation: contrib/smt-miner/README.md in the Smartiecoin repository\n"
        "(https://github.com/SmartiesCoin/Smartiecoin).\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);   /* sockets need this on Windows; leaked on purpose (process exit cleans up) */
#endif
    init_pow_params();   /* this binary's yespower.c must follow the node across the v0.5.0 fork */
    const char *addr = NULL, *conf = NULL, *rpc_url = NULL;
    const char *surl = NULL, *suser = NULL, *spass = "x";
    int bench = 0, selftest = 0, ways_opt = 0;
    g_threads = (int)smt_ncpu() - 2;
    if (g_threads < 1) g_threads = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "--rpc") && i + 1 < argc) rpc_url = argv[++i];
        else if (!strcmp(argv[i], "--stratum") && i + 1 < argc) surl = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) suser = argv[++i];
        else if (!strcmp(argv[i], "--pass") && i + 1 < argc) spass = argv[++i];
        else if (!strcmp(argv[i], "--diff1") && i + 1 < argc) g_diff1_bitcoin = !strcmp(argv[++i], "bitcoin");
        else if (!strcmp(argv[i], "--bench")) bench = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 10;
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--ways") && i + 1 < argc) ways_opt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-pin")) g_pin = 0;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { print_usage(argv[0]); return 0; }
        else if (argv[i][0] != '-') addr = argv[i];
        else { fprintf(stderr, "unknown option %s (try --help)\n", argv[i]); return 2; }
    }
    if (selftest) {
        char digest[65];
        selftest_digest(digest);
        printf("%s\n", digest);   /* stdout: digest only (scripts compare it) */
        if (smt_yp2_supported()) {
            int ok = yp2_check(64);
            fprintf(stderr, "two-way kernel: %s\n", ok ? "identical to the reference on 64 pairs of random headers" : "MISMATCH with the reference");
            if (!ok) return 1;
        } else {
            fprintf(stderr, "two-way kernel: not available in this build\n");
        }
        return 0;
    }
    /* choose the kernel: the interleaved one only when it is proven identical to the reference right now */
    if (ways_opt == 1) {
        g_ways = 1;
    } else if (smt_yp2_supported() && yp2_check(16)) {
        if (ways_opt == 2 || bench > 0) {
            g_ways = 2;   /* explicit --ways 2, or a bench run (keep bench numbers comparable) */
        } else {
            /* both kernels are byte-identical; the faster one depends on the CPU (two-way wins on
             * parts with a big enough L2, the plain kernel on small-L2 parts) -> measure and pick */
            double s1 = speed_probe(1, 0.35), s2 = speed_probe(2, 0.35);
            g_ways = (s2 >= s1) ? 2 : 1;
            fprintf(stderr, "kernel auto-tune: reference %.1f kH/s vs two-way %.1f kH/s on this CPU -> mining %d-way (force with --ways)\n",
                    s1, s2, g_ways);
        }
    } else {
        g_ways = 1;
        if (ways_opt == 2) fprintf(stderr, "warning: two-way kernel unavailable or different from the reference; using one hash at a time\n");
    }
    if (bench > 0) return run_bench(bench);
    if ((!addr && !surl) || (surl && !suser) || g_threads < 1) {
        print_usage(argv[0]);
        return 2;
    }

    if (surl) { /* pool mining: no local node needed */
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        #ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        g_hashes = calloc((size_t)g_threads, sizeof(counter_t));
        pthread_t *ths = calloc((size_t)g_threads, sizeof *ths);
        for (int i = 0; i < g_threads; i++) pthread_create(&ths[i], NULL, worker, (void *)(intptr_t)i);
        printf("smt-miner: %d threads, %d-way kernel -> %s (stratum, %s difficulty-1)\n", g_threads, g_ways, surl, g_diff1_bitcoin ? "bitcoin" : "scrypt");
        fflush(stdout);
        int rc = run_stratum(surl, suser, spass);
        g_stop = 1;
        atomic_fetch_add(&g_job, 1);
        for (int i = 0; i < g_threads; i++) pthread_join(ths[i], NULL);
        return rc;
    }

    uint8_t h160[20];
    if (b58check_p2pkh(addr, h160)) { fprintf(stderr, "invalid payout address\n"); return 2; }
    uint8_t spk[25] = {0x76, 0xa9, 0x14};
    memcpy(spk + 3, h160, 20);
    spk[23] = 0x88; spk[24] = 0xac;

    char user[128] = "", pass[128] = "", port[16] = "8282";
    const char *home = getenv("HOME");
    char path[512];
    const char *cand[2] = {"%s/.smartiecoin/smartiecoin.conf", "%s/.smartiecoincore/smartiecoin.conf"};
    int ok = 0;
    if (conf) ok = read_conf(conf, user, sizeof user, pass, sizeof pass, port, sizeof port) == 0;
    for (int i = 0; !conf && !ok && i < 2; i++) {
        snprintf(path, sizeof path, cand[i], home ? home : "");
        ok = read_conf(path, user, sizeof user, pass, sizeof pass, port, sizeof port) == 0;
    }
    if (!ok) { fprintf(stderr, "no smartiecoin.conf found; use --conf\n"); return 2; }
    snprintf(g_url, sizeof g_url, "%s", rpc_url ? rpc_url : "http://127.0.0.1:8282/");
    if (!rpc_url) snprintf(g_url, sizeof g_url, "http://127.0.0.1:%s/", port);
    snprintf(g_userpwd, sizeof g_userpwd, "%s:%s", user, pass);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    curl_global_init(CURL_GLOBAL_ALL);
    CURL *c = curl_easy_init();

    g_hashes = calloc((size_t)g_threads, sizeof(counter_t));
    pthread_t *th = calloc((size_t)g_threads, sizeof *th);
    for (int i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    printf("smt-miner: %d threads, %d-way kernel -> %s\n", g_threads, g_ways, addr);
    fflush(stdout);

    uint64_t extranonce = (uint64_t)time(NULL) << 8;
    unsigned blocks = 0;
    double t0 = now_s(), last_stats = t0;
    char err[512];

    while (!g_stop) {
        json_t *params = json_pack("[{s:[s]}]", "rules", "segwit");
        json_t *tpl = rpc(c, "getblocktemplate", params, err, sizeof err);
        if (!tpl) {
            printf("template error: %s (retrying)\n", err);
            fflush(stdout);
            for (int i = 0; i < 50 && !g_stop; i++) usleep(100000);
            continue;
        }
        block_t blk;
        extranonce++;
        if (build_block(tpl, spk, extranonce, &blk, err, sizeof err)) {
            printf("cannot build block: %s (retrying)\n", err);
            fflush(stdout);
            json_decref(tpl);
            sleep(5);
            continue;
        }
        json_decref(tpl);

        pthread_mutex_lock(&g_mu);
        memcpy(g_header, blk.header, 80);
        memcpy(g_target, blk.target, 32);
        g_found_job = 0;
        atomic_fetch_add(&g_job, 1);
        unsigned job = atomic_load(&g_job);
        pthread_mutex_unlock(&g_mu);

        double born = now_s(), last_tip = born;
        while (!g_stop) {
            usleep(100000);
            uint32_t fj, fn;
            pthread_mutex_lock(&g_mu);
            fj = g_found_job; fn = g_found_nonce;
            pthread_mutex_unlock(&g_mu);
            if (fj == job) {
                uint8_t hdr[80];
                memcpy(hdr, blk.header, 80);
                put_le32(hdr + 76, fn);
                size_t total = 80 + blk.txs.n;
                uint8_t *raw = malloc(total);
                memcpy(raw, hdr, 80);
                memcpy(raw + 80, blk.txs.p, blk.txs.n);
                char *hex = malloc(total * 2 + 1);
                bin2hex(raw, total, hex);
                json_t *sp = json_pack("[s]", hex);
                json_t *res = rpc(c, "submitblock", sp, err, sizeof err);
                if (res && json_is_null(res)) { blocks++; printf("height=%d nonce=%u submitblock=accepted\n", blk.height, fn); }
                else if (res) { char *s = json_dumps(res, JSON_ENCODE_ANY); printf("height=%d nonce=%u submitblock=%s\n", blk.height, fn, s); free(s); }
                else printf("height=%d nonce=%u submitblock error: %s\n", blk.height, fn, err);
                fflush(stdout);
                if (res) json_decref(res);
                free(hex); free(raw);
                break;
            }
            double t = now_s();
            if (t - born > TEMPLATE_MAX_AGE) break;
            if (t - last_tip > TIP_CHECK_EVERY) { /* never keep mining on a stale tip */
                last_tip = t;
                json_t *bb = rpc(c, "getbestblockhash", NULL, err, sizeof err);
                int stale = !bb || !json_is_string(bb) || strcmp(json_string_value(bb), blk.prevhash) != 0;
                if (bb) json_decref(bb);
                if (stale) break;
            }
            if (t - last_stats >= STATS_EVERY) {
                last_stats = t;
                printf("rate=%.1f kH/s blocks=%u\n", (double)total_hashes() / 1000.0 / (t - t0), blocks);
                fflush(stdout);
            }
        }
        free(blk.txs.p);
    }
    printf("stopped; blocks found: %u\n", blocks);
    atomic_fetch_add(&g_job, 1);
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);
    curl_easy_cleanup(c);
    curl_global_cleanup();
    return 0;
}
