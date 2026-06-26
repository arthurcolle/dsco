#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "trust.h"
#include "http_pool.h"
#include "fingerprint.h"
#include "crypto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef DSCO_VERSION
#define DSCO_VERSION "0.0.0"
#endif

/* ──────────────────────────────────────────────────────────────────────────
 *  Module state
 * ────────────────────────────────────────────────────────────────────────── */

static struct {
    dsco_trust_config_t cfg;
    bool inited;
    pthread_t sender;
    atomic_int run;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    /* in-memory queue (event_json strings) */
    char **queue;
    int q_head, q_tail, q_size;
    int q_cap;

    dsco_trust_stats_t stats;
    pthread_mutex_t stats_mu;
} g = {0};

/* ──────────────────────────────────────────────────────────────────────────
 *  Helpers
 * ────────────────────────────────────────────────────────────────────────── */

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/tmp";
}

static void spool_dir(char *out, size_t cap) {
    snprintf(out, cap, "%s/.dsco/trust/pending", home_dir());
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    return 0;
}

static int64_t now_unix(void) {
    return (int64_t)time(NULL);
}
static __attribute__((unused)) int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static __attribute__((unused)) void stats_update_err(const char *msg) {
    pthread_mutex_lock(&g.stats_mu);
    snprintf(g.stats.last_error, sizeof(g.stats.last_error), "%s", msg ? msg : "");
    pthread_mutex_unlock(&g.stats_mu);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Config
 * ────────────────────────────────────────────────────────────────────────── */

void dsco_trust_default_config(dsco_trust_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    const char *url = getenv("DSCO_TRUST_URL");
    snprintf(cfg->endpoint_url, sizeof(cfg->endpoint_url), "%s",
             url ? url : DSCO_TRUST_DEFAULT_URL);
    cfg->opt_out = getenv("DSCO_TRUST_OPT_OUT") != NULL;
    cfg->include_uuid = getenv("DSCO_TRUST_INCLUDE_UUID") != NULL;
    cfg->dry_run = getenv("DSCO_TRUST_DRY_RUN") != NULL;
    cfg->queue_max = 256;
    cfg->retry_max_seconds = 3600;

    /* HMAC key resolution order:
     *   1. DSCO_TRUST_KEY env       (operator-provided shared secret)
     *   2. ~/.dsco/trust/key file   (auto-generated on first run, 0600)
     *   3. derived from fingerprint (last-resort, less secure)
     */
    const char *k = getenv("DSCO_TRUST_KEY");
    if (k && *k) {
        snprintf(cfg->hmac_key_hex, sizeof(cfg->hmac_key_hex), "%s", k);
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.dsco/trust/key", home_dir());
    FILE *kf = fopen(path, "r");
    if (kf) {
        if (fgets(cfg->hmac_key_hex, sizeof(cfg->hmac_key_hex), kf)) {
            size_t n = strlen(cfg->hmac_key_hex);
            while (n > 0 && (cfg->hmac_key_hex[n - 1] == '\n' || cfg->hmac_key_hex[n - 1] == '\r' ||
                             cfg->hmac_key_hex[n - 1] == ' ')) {
                cfg->hmac_key_hex[--n] = '\0';
            }
        }
        fclose(kf);
        if (cfg->hmac_key_hex[0])
            return;
    }
    /* generate a fresh 256-bit key and persist it */
    uint8_t fresh[32];
    if (crypto_random_bytes(fresh, sizeof(fresh))) {
        hex_encode(fresh, 32, cfg->hmac_key_hex);
        cfg->hmac_key_hex[64] = '\0';
        memset(fresh, 0, sizeof(fresh));
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/.dsco/trust", home_dir());
        mkdir_p(dir);
        kf = fopen(path, "w");
        if (kf) {
            fchmod(fileno(kf), 0600);
            fprintf(kf, "%s\n", cfg->hmac_key_hex);
            fclose(kf);
        }
    } else {
        /* fallback: derive from fingerprint (low entropy but deterministic) */
        const dsco_fingerprint_t *fp = dsco_fingerprint_get();
        if (fp) {
            uint8_t derived[32];
            hkdf_sha256((const uint8_t *)fp->fingerprint_id, strlen(fp->fingerprint_id), NULL, 0,
                        (const uint8_t *)"dsco-trust-v1", 13, derived, sizeof(derived));
            hex_encode(derived, 32, cfg->hmac_key_hex);
            cfg->hmac_key_hex[64] = '\0';
            memset(derived, 0, sizeof(derived));
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Queue ops
 * ────────────────────────────────────────────────────────────────────────── */

static int queue_push(char *json_owned) {
    pthread_mutex_lock(&g.mu);
    if (g.q_size >= g.q_cap) {
        pthread_mutex_lock(&g.stats_mu);
        g.stats.dropped++;
        pthread_mutex_unlock(&g.stats_mu);
        pthread_mutex_unlock(&g.mu);
        free(json_owned);
        return -1;
    }
    g.queue[g.q_tail] = json_owned;
    g.q_tail = (g.q_tail + 1) % g.q_cap;
    g.q_size++;
    pthread_mutex_lock(&g.stats_mu);
    g.stats.queued++;
    pthread_mutex_unlock(&g.stats_mu);
    pthread_cond_signal(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return 0;
}

static char *queue_pop_blocking(void) {
    pthread_mutex_lock(&g.mu);
    while (g.q_size == 0 && atomic_load_explicit(&g.run, memory_order_acquire))
        pthread_cond_wait(&g.cv, &g.mu);
    if (g.q_size == 0) {
        pthread_mutex_unlock(&g.mu);
        return NULL;
    }
    char *s = g.queue[g.q_head];
    g.q_head = (g.q_head + 1) % g.q_cap;
    g.q_size--;
    pthread_mutex_unlock(&g.mu);
    return s;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Signing + POST
 * ────────────────────────────────────────────────────────────────────────── */

/* curl write callback that discards the body */
static size_t curl_discard(void *p, size_t s, size_t n, void *u) {
    (void)p;
    (void)u;
    return s * n;
}

static int post_signed(const char *json, char *err, size_t err_cap) {
    if (g.cfg.dry_run) {
        fprintf(stderr, "[trust dry-run %s]\n%s\n", g.cfg.endpoint_url, json);
        return 0;
    }
    if (!g.cfg.endpoint_url[0]) {
        snprintf(err, err_cap, "no endpoint configured");
        return -1;
    }

    /* timestamp + nonce */
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)now_unix());
    char nonce[33];
    crypto_random_hex(16, nonce);

    /* signing input: ts || "." || nonce || "." || body */
    size_t blen = strlen(json);
    size_t slen = strlen(ts) + 1 + strlen(nonce) + 1 + blen;
    char *sigbuf = (char *)malloc(slen + 1);
    if (!sigbuf) {
        snprintf(err, err_cap, "oom");
        return -1;
    }
    int n = snprintf(sigbuf, slen + 1, "%s.%s.", ts, nonce);
    memcpy(sigbuf + n, json, blen);
    sigbuf[slen] = '\0';

    /* HMAC */
    uint8_t key[64];
    size_t klen = hex_decode(g.cfg.hmac_key_hex, strlen(g.cfg.hmac_key_hex), key, sizeof(key));
    if (klen == 0) {
        /* fall back to raw key bytes (treat hex_key_hex as utf-8 secret) */
        klen = strlen(g.cfg.hmac_key_hex);
        if (klen > sizeof(key))
            klen = sizeof(key);
        memcpy(key, g.cfg.hmac_key_hex, klen);
    }
    char sig_hex[65];
    hmac_sha256_hex(key, klen, (const uint8_t *)sigbuf, slen, sig_hex);
    memset(key, 0, sizeof(key));
    free(sigbuf);

    CURL *c = curl_easy_init();
    dsco_http_pool_apply(c);
    if (!c) {
        snprintf(err, err_cap, "curl init");
        return -1;
    }
    struct curl_slist *h = NULL;
    char buf[512];
    h = curl_slist_append(h, "Content-Type: application/json");
    snprintf(buf, sizeof(buf), "X-DSCO-Version: %s", "1.0.0");
    h = curl_slist_append(h, buf);
    snprintf(buf, sizeof(buf), "X-DSCO-Timestamp: %s", ts);
    h = curl_slist_append(h, buf);
    snprintf(buf, sizeof(buf), "X-DSCO-Nonce: %s", nonce);
    h = curl_slist_append(h, buf);
    snprintf(buf, sizeof(buf), "X-DSCO-Signature: %s", sig_hex);
    h = curl_slist_append(h, buf);

    curl_easy_setopt(c, CURLOPT_URL, g.cfg.endpoint_url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)blen);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "dsco-trust/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    /* TLS hygiene: enforce certificate validation. Endpoint operator may
     * choose to pin via CURLOPT_PINNEDPUBLICKEY in the future. */
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        snprintf(err, err_cap, "curl: %s", curl_easy_strerror(rc));
        return -1;
    }
    if (http_code < 200 || http_code >= 300) {
        snprintf(err, err_cap, "http %ld", http_code);
        return -1;
    }
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Sender thread
 * ────────────────────────────────────────────────────────────────────────── */

static void *sender_main(void *arg) {
    (void)arg;
    int backoff_ms = 0;
    while (atomic_load_explicit(&g.run, memory_order_acquire)) {
        char *json = queue_pop_blocking();
        if (!json)
            break;
        char err[128] = {0};
        int rc = post_signed(json, err, sizeof(err));
        if (rc == 0) {
            pthread_mutex_lock(&g.stats_mu);
            g.stats.sent_ok++;
            g.stats.last_send_unix = now_unix();
            pthread_mutex_unlock(&g.stats_mu);
            free(json);
            backoff_ms = 0;
        } else {
            pthread_mutex_lock(&g.stats_mu);
            g.stats.sent_failed++;
            snprintf(g.stats.last_error, sizeof(g.stats.last_error), "%s", err);
            pthread_mutex_unlock(&g.stats_mu);
            /* requeue at head, sleep, retry with exponential backoff */
            pthread_mutex_lock(&g.mu);
            g.q_head = (g.q_head + g.q_cap - 1) % g.q_cap;
            g.queue[g.q_head] = json;
            g.q_size++;
            pthread_mutex_unlock(&g.mu);
            if (backoff_ms == 0)
                backoff_ms = 500;
            else
                backoff_ms = backoff_ms * 2;
            if (backoff_ms > g.cfg.retry_max_seconds * 1000)
                backoff_ms = g.cfg.retry_max_seconds * 1000;
            struct timespec ts = {backoff_ms / 1000, (backoff_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Lifecycle
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_trust_init(const dsco_trust_config_t *cfg_in) {
    if (g.inited)
        return 0;
    memset(&g, 0, sizeof(g));
    if (cfg_in)
        g.cfg = *cfg_in;
    else
        dsco_trust_default_config(&g.cfg);

    if (g.cfg.opt_out)
        return 0; /* silently do nothing */

    pthread_mutex_init(&g.mu, NULL);
    pthread_mutex_init(&g.stats_mu, NULL);
    pthread_cond_init(&g.cv, NULL);

    g.q_cap = g.cfg.queue_max > 0 ? g.cfg.queue_max : 256;
    g.queue = (char **)calloc(g.q_cap, sizeof(char *));
    if (!g.queue)
        return -1;

    char dir[PATH_MAX];
    spool_dir(dir, sizeof(dir));
    mkdir_p(dir);

    atomic_store(&g.run, 1);
    if (pthread_create(&g.sender, NULL, sender_main, NULL) != 0) {
        atomic_store(&g.run, 0);
        free(g.queue);
        g.queue = NULL;
        return -1;
    }
    g.inited = true;
    return 0;
}

void dsco_trust_shutdown(void) {
    if (!g.inited)
        return;
    atomic_store(&g.run, 0);
    pthread_mutex_lock(&g.mu);
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
    pthread_join(g.sender, NULL);
    for (int i = 0; i < g.q_size; i++) {
        free(g.queue[(g.q_head + i) % g.q_cap]);
    }
    free(g.queue);
    pthread_mutex_destroy(&g.mu);
    pthread_mutex_destroy(&g.stats_mu);
    pthread_cond_destroy(&g.cv);
    memset(&g, 0, sizeof(g));
}

const char *dsco_trust_endpoint(void) {
    return g.inited ? g.cfg.endpoint_url : NULL;
}
bool dsco_trust_is_active(void) {
    return g.inited && !g.cfg.opt_out;
}

void dsco_trust_stats(dsco_trust_stats_t *out) {
    if (!out)
        return;
    pthread_mutex_lock(&g.stats_mu);
    *out = g.stats;
    pthread_mutex_unlock(&g.stats_mu);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Emit helpers
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_trust_emit(const char *event_type, const char *json_payload) {
    if (!g.inited || g.cfg.opt_out)
        return 0;
    if (!event_type || !json_payload)
        return -1;
    /* wrap payload into envelope */
    size_t need = strlen(event_type) + strlen(json_payload) + 128;
    char *env = (char *)malloc(need);
    if (!env)
        return -1;
    snprintf(env, need, "{\"event\":\"%s\",\"emitted_at\":%lld,\"data\":%s}", event_type,
             (long long)now_unix(), json_payload);
    return queue_push(env);
}

int dsco_trust_emit_attest(void) {
    if (!g.inited || g.cfg.opt_out)
        return 0;
    const dsco_fingerprint_t *fp = dsco_fingerprint_get();
    if (!fp)
        return -1;
    char buf[8192];
    size_t n = dsco_fingerprint_to_json_compat(fp, g.cfg.include_uuid, buf, sizeof(buf));
    if (n == 0)
        return -1;
    return dsco_trust_emit("attest", buf);
}

int dsco_trust_emit_heartbeat(const dsco_trust_runtime_t *r) {
    if (!g.inited || g.cfg.opt_out)
        return 0;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"projects_active\":%d,\"projects_total\":%d,"
                     "\"tokens_in\":%llu,\"tokens_out\":%llu,"
                     "\"bytes_in\":%llu,\"bytes_out\":%llu,"
                     "\"cents_spent\":%d}",
                     r ? r->projects_active : 0, r ? r->projects_total : 0,
                     r ? (unsigned long long)r->tokens_in : 0ULL,
                     r ? (unsigned long long)r->tokens_out : 0ULL,
                     r ? (unsigned long long)r->bytes_in : 0ULL,
                     r ? (unsigned long long)r->bytes_out : 0ULL, r ? r->cents_spent : 0);
    if (n < 0)
        return -1;
    return dsco_trust_emit("heartbeat", buf);
}
