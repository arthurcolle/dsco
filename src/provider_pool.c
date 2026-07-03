#include "provider_pool.h"
#include "provider_profiles.h"
#include "json_util.h"
#include "http_pool.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* Circuit breaker: after this many consecutive failures a slot is tripped and
 * routing should avoid it until tripped_until passes. */
#define POOL_TRIP_THRESHOLD 3
#define POOL_TRIP_BASE_SECS 15
#define POOL_TRIP_MAX_SECS 300

static provider_pool_t g_pool;

provider_pool_t *provider_pool(void) {
    return &g_pool;
}

static const char *pool_canon(const char *name) {
    if (!name || !name[0])
        return name;
    const char *c = provider_profile_canonical_name(name);
    return (c && c[0]) ? c : name;
}

provider_slot_t *provider_pool_slot(const char *name) {
    if (!name || !name[0])
        return NULL;
    const char *canon = pool_canon(name);
    for (int i = 0; i < g_pool.count; i++) {
        if (strcmp(g_pool.slots[i].name, canon) == 0)
            return &g_pool.slots[i];
    }
    return NULL;
}

/* Register a slot (without building transport) if not already present. Returns
 * the slot, or NULL if the pool is full. */
static provider_slot_t *pool_register(const char *name, bool is_subscription) {
    provider_slot_t *existing = provider_pool_slot(name);
    if (existing) {
        if (is_subscription)
            existing->is_subscription = true;
        return existing;
    }
    if (g_pool.count >= PROVIDER_POOL_MAX)
        return NULL;
    provider_slot_t *s = &g_pool.slots[g_pool.count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", pool_canon(name));
    s->is_subscription = is_subscription;
    s->state = POOL_SLOT_EMPTY;
    const provider_profile_t *prof = provider_profile_find(s->name);
    if (prof && prof->default_model)
        snprintf(s->default_model, sizeof(s->default_model), "%s", prof->default_model);
    return s;
}

static bool pool_limits_path(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out || out_len == 0)
        return false;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.dsco", home);
    mkdir(dir, 0700);
    snprintf(out, out_len, "%s/provider_limits.json", dir);
    return true;
}

static char *pool_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || n > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void pool_limits_load(void) {
    char path[1024];
    if (!pool_limits_path(path, sizeof(path)))
        return;
    char *json = pool_read_file(path);
    if (!json)
        return;
    char *providers = json_get_raw(json, "providers");
    const char *root = providers ? providers : json;
    time_t now = time(NULL);
    for (int i = 0; i < g_pool.count; i++) {
        provider_slot_t *s = &g_pool.slots[i];
        char *entry = json_get_raw(root, s->name);
        if (!entry)
            continue;
        char *raw_until = json_get_raw(entry, "exhausted_until");
        if (raw_until) {
            time_t until = (time_t)atoll(raw_until);
            s->exhausted_until = until > now ? until : 0;
            free(raw_until);
        }
        free(entry);
    }
    free(providers);
    free(json);
}

static void pool_limits_save(void) {
    char path[1024];
    if (!pool_limits_path(path, sizeof(path)))
        return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    time_t now = time(NULL);
    fprintf(f, "{\"version\":1,\"updated_at\":%lld,\"providers\":{", (long long)now);
    bool first = true;
    for (int i = 0; i < g_pool.count; i++) {
        provider_slot_t *s = &g_pool.slots[i];
        if (s->exhausted_until <= now)
            continue;
        fprintf(f, "%s\"%s\":{\"exhausted_until\":%lld}", first ? "" : ",", s->name,
                (long long)s->exhausted_until);
        first = false;
    }
    fprintf(f, "}}\n");
    fclose(f);
    rename(tmp, path);
}

/* Build + warm the transport for a slot. Safe to call repeatedly. */
static void pool_warm(provider_slot_t *s) {
    if (!s)
        return;
    const char *sk = g_pool.session_key[0] ? g_pool.session_key : NULL;
    bool have_key = provider_has_usable_key(s->name, sk);
    if (!s->provider) {
        s->provider = provider_create(s->name);
        if (s->provider)
            provider_prepare(s->provider); /* warm DNS/TCP/TLS; no-op if unsupported */
    }
    if (!s->provider) {
        s->state = POOL_SLOT_NOKEY; /* uncreatable — treat as unusable */
        return;
    }
    if (s->state == POOL_SLOT_TRIPPED)
        return; /* leave tripped state to report()/healthy() to clear */
    s->state = have_key ? POOL_SLOT_UP : POOL_SLOT_NOKEY;
}

static bool pool_slot_has_usable_key(const provider_slot_t *s) {
    if (!s)
        return false;
    const char *sk = g_pool.session_key[0] ? g_pool.session_key : NULL;
    return provider_has_usable_key(s->name, sk);
}

static bool pool_refresh_slot(provider_slot_t *s, time_t now, bool warm_if_keyed) {
    if (!s)
        return false;
    bool changed = false;
    bool allocation_reset = s->is_subscription && s->exhausted_until && now >= s->exhausted_until;
    bool breaker_reset =
        s->state == POOL_SLOT_TRIPPED && s->tripped_until && now >= s->tripped_until;

    if (!allocation_reset && !breaker_reset)
        return false;

    bool have_key = pool_slot_has_usable_key(s);
    if (allocation_reset) {
        s->exhausted_until = 0;
        changed = true;
    }
    if (allocation_reset || breaker_reset) {
        s->consec_failures = 0;
        s->tripped_until = 0;
        if (!have_key) {
            s->state = POOL_SLOT_NOKEY;
        } else {
            if (warm_if_keyed && !s->provider) {
                s->state = POOL_SLOT_EMPTY;
                pool_warm(s);
            }
            s->state = s->provider ? POOL_SLOT_UP : POOL_SLOT_EMPTY;
        }
        changed = true;
    }
    return changed;
}

void provider_pool_init(const char *session_key) {
    if (session_key && session_key[0])
        snprintf(g_pool.session_key, sizeof(g_pool.session_key), "%s", session_key);

    /* The flat-rate/core subscription lanes are always registered so they show
     * up in /providers even before first use; they are warmed when a credential
     * is available. openai-codex covers the ChatGPT subscription path. */
    struct {
        const char *name;
        bool is_sub;
    } core[] = {
        {"sakana", provider_sakana_current_key_is_subscription()},
        {"anthropic", true},
        {"openai", true},
        {"openai-codex", true},
        {"zai", true},
        /* Common metered fallbacks — registered lazily-warm only if keyed. */
        {"openrouter", false},
        {"xai", false},
        {"moonshot", false},
        {"google", false},
    };

    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
        const char *sk = g_pool.session_key[0] ? g_pool.session_key : NULL;
        bool keyed = provider_has_usable_key(core[i].name, sk);
        /* Always register the core subscriptions; register metered providers
         * only when they actually have a key (keeps the table meaningful). */
        if (!core[i].is_sub && !keyed)
            continue;
        provider_slot_t *s = pool_register(core[i].name, core[i].is_sub);
        if (s && keyed)
            pool_warm(s); /* warm the ones we can authenticate now */
        else if (s)
            s->state = POOL_SLOT_NOKEY;
    }

    pool_limits_load();
    g_pool.initialized = true;
}

/* ── Background connection pre-warming ──────────────────────────────────────
 * A cold first request per provider pays a full DNS + TCP + TLS handshake
 * (~50-150ms) that lands squarely in the first turn's time-to-first-token.
 * We hide it: snapshot the endpoint URLs now (main thread), then on a detached
 * worker open a CONNECT_ONLY TLS connection to each host. That populates the
 * process-wide shared DNS cache and TLS session tickets (both mutex-protected
 * in http_pool.c), so the first real request resolves DNS from cache and
 * resumes TLS in one round trip instead of two. The worker uses its own
 * thread-local easy handles and never touches g_pool or the providers'
 * persistent handles, so there is no data race. */
/* Open a CONNECT_ONLY TLS session to `url`, populating the shared DNS + TLS
 * cache. Thread-local handle; touches only the mutex-protected shared cache. */
static void pool_warm_one_url(const char *url) {
    CURL *c = curl_easy_init();
    if (!c)
        return;
    dsco_http_pool_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_perform(c); /* best-effort; failure just skips the warm */
    curl_easy_cleanup(c);
}

/* Snapshot the warm providers' endpoint URLs (dedup by exact URL), scored
 * worst-first so the connections most at risk of having gone cold get priority.
 * Runs on the calling thread — no cross-thread pool read. Caller frees the
 * returned array and each element. Returns the count via *out_n. */
static char **pool_snapshot_warm_urls(int *out_n) {
    *out_n = 0;
    if (g_pool.count <= 0)
        return NULL;
    char **urls = calloc((size_t)g_pool.count, sizeof(char *));
    double *scores = calloc((size_t)g_pool.count, sizeof(double));
    if (!urls || !scores) {
        free(urls);
        free(scores);
        return NULL;
    }
    int n = 0;
    for (int i = 0; i < g_pool.count; i++) {
        provider_slot_t *s = &g_pool.slots[i];
        if (s->state != POOL_SLOT_UP || !s->provider)
            continue;
        const char *url = s->provider->api_url;
        if (!url || !url[0] || strncmp(url, "http", 4) != 0)
            continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(urls[j], url) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        char *dup_url = strdup(url);
        if (!dup_url)
            continue;
        urls[n] = dup_url;
        /* Higher score (worse health/latency) warms first. */
        scores[n] = provider_pool_score_components(s->lat_ewma_ms, s->success_ewma,
                                                   s->is_subscription, true);
        n++;
    }
    /* Insertion sort by descending score (worst-first). */
    for (int i = 1; i < n; i++) {
        char *uk = urls[i];
        double sk = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < sk) {
            urls[j + 1] = urls[j];
            scores[j + 1] = scores[j];
            j--;
        }
        urls[j + 1] = uk;
        scores[j + 1] = sk;
    }
    free(scores);
    *out_n = n;
    return urls;
}

typedef struct {
    char **urls;
    int count;
} prewarm_batch_t;

static void *pool_prewarm_thread(void *arg) {
    prewarm_batch_t *batch = (prewarm_batch_t *)arg;
    if (!batch)
        return NULL;
    for (int i = 0; i < batch->count; i++) {
        pool_warm_one_url(batch->urls[i]);
        free(batch->urls[i]);
    }
    free(batch->urls);
    free(batch);
    return NULL;
}

void provider_pool_prewarm_async(void) {
    if (getenv("DSCO_NO_PREWARM"))
        return;
    int n = 0;
    char **urls = pool_snapshot_warm_urls(&n);
    if (n == 0) {
        free(urls);
        return;
    }
    prewarm_batch_t *batch = malloc(sizeof(*batch));
    if (!batch) {
        for (int i = 0; i < n; i++)
            free(urls[i]);
        free(urls);
        return;
    }
    batch->urls = urls;
    batch->count = n;
    pthread_t t;
    if (pthread_create(&t, NULL, pool_prewarm_thread, batch) == 0) {
        pthread_detach(t);
    } else {
        pool_prewarm_thread(batch); /* couldn't spawn — clean up rather than leak */
    }
}

/* ── Connection keep-warm heartbeat ─────────────────────────────────────────
 * Prewarm hides the first turn's handshake; this keeps it hidden for the whole
 * session. Idle connections die to NAT/proxy timeouts (~60-120s), so a quiet
 * gap makes the next turn pay a cold handshake again. This detached thread
 * re-primes the DNS+TLS cache on an interval. It re-snapshots the pool each
 * cycle on its own thread — this reads g_pool concurrently with the main
 * thread's health updates, but every field it touches is a plain scalar or a
 * stable pointer (api_url is set once at provider creation), so a torn read at
 * worst warms a stale URL; it never dereferences freed memory because pooled
 * providers live for the process lifetime. */
static pthread_once_t g_keepwarm_once = PTHREAD_ONCE_INIT;

static void *pool_keepwarm_thread(void *arg) {
    (void)arg;
    long interval_s = 45;
    const char *env = getenv("DSCO_KEEPWARM_SECS");
    if (env && env[0]) {
        long v = atol(env);
        if (v >= 5 && v <= 3600)
            interval_s = v;
    }
    for (;;) {
        for (long slept = 0; slept < interval_s; slept++)
            sleep(1);
        int n = 0;
        char **urls = pool_snapshot_warm_urls(&n);
        for (int i = 0; i < n; i++) {
            pool_warm_one_url(urls[i]);
            free(urls[i]);
        }
        free(urls);
    }
    return NULL;
}

static void keepwarm_spawn_once(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, pool_keepwarm_thread, NULL) == 0)
        pthread_detach(t);
}

void provider_pool_keepwarm_start(void) {
    if (getenv("DSCO_NO_PREWARM"))
        return;
    pthread_once(&g_keepwarm_once, keepwarm_spawn_once);
}

/* ── Latency-aware scoring ──────────────────────────────────────────────── */

double provider_pool_score_components(double lat_ewma_ms, double success_ewma, bool is_subscription,
                                      bool available) {
    if (!available)
        return 1e9;
    /* Neutral latency prior until we have data, so a never-used provider isn't
     * unfairly ranked ahead of or behind a measured one. */
    double lat = lat_ewma_ms > 0 ? lat_ewma_ms : 1500.0;
    double sr = success_ewma;
    if (sr < 0.0)
        sr = 0.0;
    if (sr > 1.0)
        sr = 1.0;
    /* A provider that fails half its requests scores as if ~2.5s slower — real
     * enough to demote it below a healthy but nominally slower lane. */
    double failure_penalty = (1.0 - sr) * 5000.0;
    double score = lat + failure_penalty;
    /* Zero-marginal-cost subscription lanes get a mild discount so they stay
     * preferred while healthy, but a tripped/slow subscription still loses to a
     * fast metered provider (this is what makes fallback order dynamic). */
    if (is_subscription)
        score *= 0.85;
    return score;
}

double provider_pool_score(const char *name) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        return 1e9;
    bool available = provider_pool_healthy(name);
    return provider_pool_score_components(s->lat_ewma_ms, s->success_ewma, s->is_subscription,
                                          available);
}

const char *provider_pool_fastest_healthy(const char *const *names, int n) {
    const char *best = NULL;
    double best_score = 1e9;
    for (int i = 0; i < n; i++) {
        if (!names[i] || !names[i][0])
            continue;
        double sc = provider_pool_score(names[i]);
        if (sc < best_score) {
            best_score = sc;
            best = names[i];
        }
    }
    return best_score >= 1e9 ? NULL : best;
}

#define POOL_RERANK_MAX 16

void provider_pool_rerank_fallbacks(char out_models[][128], int n) {
    /* Dynamic fallback ordering: reorder a statically-built fallback chain by
     * each model's provider's live score (latency EWMA + failure penalty,
     * subscription-discounted). A no-op before the pool is initialized or when
     * every candidate scores equally (no runtime data) — so the static prior
     * from provider_build_default_fallback_models() stands until real signal
     * accumulates. Stable: equal scores keep their original relative order. */
    if (!g_pool.initialized || n <= 1 || n > POOL_RERANK_MAX)
        return;

    double sc[POOL_RERANK_MAX];
    int idx[POOL_RERANK_MAX];
    bool any_diff = false;
    for (int i = 0; i < n; i++) {
        idx[i] = i;
        const char *model = out_models[i][0] ? out_models[i] : NULL;
        const char *pv = model ? provider_detect(model, NULL) : NULL;
        sc[i] = pv ? provider_pool_score(pv) : 1e9;
        if (i > 0 && sc[i] != sc[0])
            any_diff = true;
    }
    if (!any_diff)
        return; /* all equal — nothing to gain, preserve original order */

    /* Stable insertion sort of indices by ascending score. */
    for (int i = 1; i < n; i++) {
        int cur = idx[i];
        int j = i - 1;
        while (j >= 0 && sc[idx[j]] > sc[cur]) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = cur;
    }

    char tmp[POOL_RERANK_MAX][128];
    for (int i = 0; i < n; i++)
        memcpy(tmp[i], out_models[idx[i]], 128);
    for (int i = 0; i < n; i++)
        memcpy(out_models[i], tmp[i], 128);
}

provider_t *provider_pool_acquire(const char *name) {
    if (!name || !name[0])
        return NULL;
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        s = pool_register(name, false);
    if (!s)
        return NULL; /* pool full (unreachable in practice): caller keeps current
                      * provider rather than receiving a non-pool instance */
    time_t now = time(NULL);
    bool changed = pool_refresh_slot(s, now, true);
    if (changed)
        pool_limits_save();
    if (!s->provider)
        pool_warm(s);
    s->last_used = now;
    return s->provider;
}

void provider_pool_report(const char *name, bool ok, double latency_ms) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        return;
    s->total_requests++;
    if (latency_ms > 0) {
        s->last_latency_ms = latency_ms;
        /* EWMA (α=0.3): responsive to recent shifts without single-sample noise. */
        s->lat_ewma_ms = s->lat_ewma_ms > 0 ? 0.7 * s->lat_ewma_ms + 0.3 * latency_ms : latency_ms;
    }
    /* Success-rate EWMA (α=0.25), optimistic prior (1.0) so a fresh provider
     * isn't penalized before it has run anything. */
    if (s->success_ewma <= 0.0 && s->total_requests == 1)
        s->success_ewma = 1.0;
    s->success_ewma = 0.75 * s->success_ewma + 0.25 * (ok ? 1.0 : 0.0);
    if (ok) {
        s->consec_failures = 0;
        s->tripped_until = 0;
        bool clears_subscription_exhaustion = true;
        if (strcmp(s->name, "sakana") == 0 && !provider_sakana_current_key_is_subscription())
            clears_subscription_exhaustion = false;
        if (s->is_subscription && s->exhausted_until && clears_subscription_exhaustion) {
            s->exhausted_until = 0;
            pool_limits_save();
        }
        if (s->provider)
            s->state = POOL_SLOT_UP;
    } else {
        s->total_failures++;
        s->consec_failures++;
        if (s->consec_failures >= POOL_TRIP_THRESHOLD) {
            long backoff = (long)POOL_TRIP_BASE_SECS * s->consec_failures;
            if (backoff > POOL_TRIP_MAX_SECS)
                backoff = POOL_TRIP_MAX_SECS;
            s->state = POOL_SLOT_TRIPPED;
            s->tripped_until = time(NULL) + backoff;
        }
    }
}

void provider_pool_mark_subscription_exhausted(const char *name, time_t exhausted_until) {
    if (!name || !name[0] || exhausted_until <= 0)
        return;
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        s = pool_register(name, true);
    if (!s)
        return;
    if (exhausted_until <= time(NULL))
        return;
    if (exhausted_until <= s->exhausted_until)
        return;
    s->exhausted_until = exhausted_until;
    pool_limits_save();
}

time_t provider_pool_subscription_exhausted_until(const char *name) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s || !s->is_subscription || !s->exhausted_until)
        return 0;
    time_t now = time(NULL);
    if (now >= s->exhausted_until) {
        pool_refresh_slot(s, now, true);
        pool_limits_save();
        return 0;
    }
    return s->exhausted_until;
}

bool provider_pool_healthy(const char *name) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        return false;
    time_t now = time(NULL);
    if (pool_refresh_slot(s, now, true))
        pool_limits_save();
    if (!s->provider)
        return false;
    if (s->is_subscription && s->exhausted_until) {
        if (now < s->exhausted_until)
            return false;
    }
    if (s->state == POOL_SLOT_TRIPPED) {
        return false;
    }
    return s->state == POOL_SLOT_UP;
}

static const char *pool_state_str(pool_slot_state_t st) {
    switch (st) {
        case POOL_SLOT_UP:
            return "up";
        case POOL_SLOT_NOKEY:
            return "no-key";
        case POOL_SLOT_TRIPPED:
            return "tripped";
        case POOL_SLOT_EMPTY:
        default:
            return "idle";
    }
}

void provider_pool_render(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t pos = 0;
    int n =
        snprintf(out + pos, out_len - pos, "  %-14s %-4s %-8s %-16s %8s %6s %7s %7s  %s\n",
                 "provider", "sub", "state", "reset", "lat(ms)", "ok%", "score", "fails", "model");
    if (n > 0)
        pos += (size_t)n;
    for (int i = 0; i < g_pool.count && pos < out_len; i++) {
        provider_slot_t *s = &g_pool.slots[i];
        char fails[32];
        char reset[32] = "-";
        if (pool_refresh_slot(s, time(NULL), false))
            pool_limits_save();
        snprintf(fails, sizeof(fails), "%ld/%ld", s->total_failures, s->total_requests);
        if (s->exhausted_until > time(NULL)) {
            struct tm tmv;
            localtime_r(&s->exhausted_until, &tmv);
            strftime(reset, sizeof(reset), "%m-%d %H:%M", &tmv);
        }
        /* EWMA latency (falls back to last sample), success rate, and the live
         * routing score that drives dynamic fallback ordering. */
        double lat_disp = s->lat_ewma_ms > 0 ? s->lat_ewma_ms : s->last_latency_ms;
        double ok_pct = s->total_requests > 0 ? s->success_ewma * 100.0 : 100.0;
        double score = provider_pool_score(s->name);
        char score_buf[16];
        if (score >= 1e9)
            snprintf(score_buf, sizeof(score_buf), "%s", "-");
        else
            snprintf(score_buf, sizeof(score_buf), "%.0f", score);
        n = snprintf(out + pos, out_len - pos,
                     "  %-14s %-4s %-8s %-16s %8.0f %5.0f%% %7s %7s  %s\n", s->name,
                     s->is_subscription ? "yes" : "-", pool_state_str(s->state), reset, lat_disp,
                     ok_pct, score_buf, fails, s->default_model[0] ? s->default_model : "-");
        if (n <= 0)
            break;
        pos += (size_t)n;
    }
    if (pos < out_len)
        out[pos] = '\0';
    else if (out_len > 0)
        out[out_len - 1] = '\0';
}

void provider_pool_shutdown(void) {
    for (int i = 0; i < g_pool.count; i++) {
        if (g_pool.slots[i].provider) {
            provider_free(g_pool.slots[i].provider);
            g_pool.slots[i].provider = NULL;
        }
    }
    g_pool.count = 0;
    g_pool.initialized = false;
    g_pool.session_key[0] = '\0';
}
