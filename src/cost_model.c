/* cost_model.c — Learned per-topology cost model with EMA smoothing.
 *
 * Priority 3 from the roadmap: after each topology_run, call
 * cost_model_learn(topology_name, actual_tokens, actual_cost_usd)
 * to improve future cost_model_predict() accuracy.
 *
 * Storage: ~/.dsco/cost_model.jsonl  (append-only, loaded on init)
 * Algorithm: Exponential Moving Average (alpha=0.2) per topology
 */

#include "cost_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_ENTRIES   128
#define EMA_ALPHA     0.20          /* weight given to new observation */
#define COST_MODEL_VERSION 1

typedef struct {
    char   name[48];               /* topology name */
    double cost_per_1k_ema;        /* EMA of cost / 1K tokens */
    double cost_per_1k_var;        /* EMA of variance for CI computation */
    double latency_ema;            /* EMA of latency (seconds) */
    int    observations;
    double last_actual_cost;
    time_t last_updated;
} cost_entry_t;

/* Adaptive EMA alpha: high early (fast learning), stabilises after 10 obs */
static double adaptive_alpha(int observations) {
    if (observations <= 0)  return 1.0;
    if (observations <= 3)  return 0.60;
    if (observations <= 7)  return 0.35;
    if (observations <= 15) return 0.20;
    return 0.10;                        /* stable long-run */
}

static cost_entry_t s_entries[MAX_ENTRIES];
static int          s_count    = 0;
static bool         s_loaded   = false;
static bool         s_dirty    = false;
static pthread_mutex_t s_mu    = PTHREAD_MUTEX_INITIALIZER;

/* ── File path ─────────────────────────────────────────────────────────── */

static void model_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.dsco/cost_model.jsonl", home ? home : "/tmp");
}

/* ── Load ──────────────────────────────────────────────────────────────── */

static void load_locked(void) {
    if (s_loaded) return;
    s_loaded = true;

    char path[512];
    model_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f) && s_count < MAX_ENTRIES) {
        /* Parse: {"name":"...","cost_per_1k":0.001,"lat":5.0,"obs":3} */
        cost_entry_t *e = &s_entries[s_count];
        char name_buf[48] = {0};
        double cper1k = 0, lat = 0;
        int obs = 0;
        long long ts = 0;

        /* Minimal JSON extraction — no full parser dependency */
        char *p;
#define JSGET_STR(key, dst, dsz) do { \
    p = strstr(line, "\"" key "\":\""); \
    if (p) { p += strlen("\"" key "\":\""); \
              char *end = strchr(p, '"'); \
              if (end) { size_t n = (size_t)(end-p); if(n>=dsz) n=dsz-1; \
                         memcpy(dst, p, n); dst[n]='\0'; } } \
} while(0)
#define JSGET_DBL(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) { p += strlen("\"" key "\":"); dst = atof(p); } \
} while(0)
#define JSGET_INT(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) { p += strlen("\"" key "\":"); dst = atoi(p); } \
} while(0)

        JSGET_STR("name", name_buf, sizeof(name_buf));
        JSGET_DBL("cost_per_1k", cper1k);
        JSGET_DBL("lat", lat);
        JSGET_INT("obs", obs);
        JSGET_INT("ts", ts);

        if (!name_buf[0] || cper1k <= 0.0) continue;

        strncpy(e->name, name_buf, sizeof(e->name)-1);
        e->cost_per_1k_ema  = cper1k;
        e->latency_ema      = lat;
        e->observations     = obs;
        e->last_updated     = (time_t)ts;
        s_count++;
    }
    fclose(f);
}

/* ── Persist ───────────────────────────────────────────────────────────── */

static void persist_locked(void) {
    if (!s_dirty) return;

    char path[512];
    model_path(path, sizeof(path));

    /* Ensure ~/.dsco exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.dsco", getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(dir, 0700);

    /* Write atomically: write to .tmp then rename */
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;

    for (int i = 0; i < s_count; i++) {
        const cost_entry_t *e = &s_entries[i];
        fprintf(f,
            "{\"v\":%d,\"name\":\"%s\",\"cost_per_1k\":%.8f,"
            "\"lat\":%.3f,\"obs\":%d,\"ts\":%lld}\n",
            COST_MODEL_VERSION,
            e->name, e->cost_per_1k_ema,
            e->latency_ema, e->observations,
            (long long)e->last_updated);
    }
    fclose(f);
    rename(tmp, path);
    s_dirty = false;
}

/* ── Find or create entry ─────────────────────────────────────────────── */

static cost_entry_t *find_or_create(const char *name) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) return &s_entries[i];
    }
    if (s_count >= MAX_ENTRIES) return NULL;
    cost_entry_t *e = &s_entries[s_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name)-1);
    return e;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void cost_model_init(void) {
    pthread_mutex_lock(&s_mu);
    load_locked();
    pthread_mutex_unlock(&s_mu);
}

void cost_model_learn(const char *topology_name,
                      int total_tokens,
                      double actual_cost_usd,
                      double actual_latency_s) {
    if (!topology_name || total_tokens <= 0 || actual_cost_usd < 0) return;

    double new_per_1k = (total_tokens > 0)
        ? actual_cost_usd / (total_tokens / 1000.0) : 0.0;

    pthread_mutex_lock(&s_mu);
    load_locked();

    cost_entry_t *e = find_or_create(topology_name);
    if (!e) { pthread_mutex_unlock(&s_mu); return; }

    if (e->observations == 0) {
        /* First observation — seed directly, zero variance */
        e->cost_per_1k_ema = new_per_1k;
        e->cost_per_1k_var = 0.0;
        e->latency_ema     = actual_latency_s;
    } else {
        /* Adaptive EMA: converges fast early, stabilises after 10 obs */
        double alpha = adaptive_alpha(e->observations);
        double delta = new_per_1k - e->cost_per_1k_ema;
        e->cost_per_1k_ema += alpha * delta;
        /* Welford-style EMA variance */
        e->cost_per_1k_var  = (1.0 - alpha) * (e->cost_per_1k_var + alpha * delta * delta);
        e->latency_ema      = alpha * actual_latency_s
                            + (1.0 - alpha) * e->latency_ema;
    }

    e->last_actual_cost = actual_cost_usd;
    e->observations++;
    e->last_updated = time(NULL);
    s_dirty = true;

    persist_locked();
    pthread_mutex_unlock(&s_mu);
}

double cost_model_predict(const char *topology_name,
                          int input_tokens, int output_tokens) {
    if (!topology_name) return -1.0;

    pthread_mutex_lock(&s_mu);
    load_locked();

    double result = -1.0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].name, topology_name) == 0 &&
            s_entries[i].observations >= 2) {
            double units = (input_tokens + output_tokens) / 1000.0;
            if (units < 1.0) units = 1.0;
            result = s_entries[i].cost_per_1k_ema * units;
            break;
        }
    }

    pthread_mutex_unlock(&s_mu);
    return result; /* -1.0 = no learned data */
}

double cost_model_predict_latency(const char *topology_name) {
    if (!topology_name) return -1.0;

    pthread_mutex_lock(&s_mu);
    load_locked();

    double result = -1.0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].name, topology_name) == 0 &&
            s_entries[i].observations >= 2) {
            result = s_entries[i].latency_ema;
            break;
        }
    }

    pthread_mutex_unlock(&s_mu);
    return result;
}

int cost_model_stats_json(char *buf, size_t buflen) {
    pthread_mutex_lock(&s_mu);
    load_locked();

    char *p = buf;
    size_t rem = buflen;
    int n = snprintf(p, rem, "{\"entries\":%d,\"models\":[", s_count);
    p += n; rem -= (size_t)n;

    for (int i = 0; i < s_count && rem > 8; i++) {
        const cost_entry_t *e = &s_entries[i];
        n = snprintf(p, rem,
            "%s{\"name\":\"%s\",\"cost_per_1k\":%.6f,\"cost_stddev\":%.6f,\"lat_s\":%.2f,\"obs\":%d}",
            i ? "," : "", e->name, e->cost_per_1k_ema, sqrt(e->cost_per_1k_var), e->latency_ema, e->observations);
        p += n; rem -= (size_t)n;
    }
    snprintf(p, rem, "]}");

    pthread_mutex_unlock(&s_mu);
    return (int)(buflen - rem);
}


/* cost_model_predict_full — point estimate + 80% confidence interval.
 *
 * CI uses ±1.28σ (80%) around the EMA mean.  For early observations
 * (< 5) the interval is widened by an uncertainty multiplier so callers
 * can signal "we're not sure yet."
 */
bool cost_model_predict_full(const char *topology_name,
                             int input_tokens, int output_tokens,
                             cost_prediction_t *out) {
    if (!topology_name || !out) return false;

    pthread_mutex_lock(&s_mu);
    load_locked();

    bool found = false;
    for (int i = 0; i < s_count; i++) {
        cost_entry_t *e = &s_entries[i];
        if (strcmp(e->name, topology_name) != 0) continue;
        if (e->observations < 1) break;

        double units   = (input_tokens + output_tokens) / 1000.0;
        if (units < 1.0) units = 1.0;

        double mean    = e->cost_per_1k_ema * units;
        double stddev  = sqrt(e->cost_per_1k_var) * units;

        /* Widen CI if few observations */
        double k = 1.28;   /* 80% CI */
        if (e->observations < 5) k *= (1.0 + (5.0 - e->observations) * 0.30);

        out->cost_usd    = mean;
        out->cost_lo     = mean - k * stddev;
        if (out->cost_lo < 0.0) out->cost_lo = 0.0;
        out->cost_hi     = mean + k * stddev;
        out->latency_s   = (e->observations >= 2) ? e->latency_ema : -1.0;
        out->confidence  = (e->observations >= 10) ? 0.90
                         : (e->observations >= 5)  ? 0.70
                         : (e->observations >= 2)  ? 0.50
                         :                           0.20;
        out->observations = e->observations;
        found = true;
        break;
    }

    pthread_mutex_unlock(&s_mu);
    return found;
}

void cost_model_flush(void) {
    pthread_mutex_lock(&s_mu);
    persist_locked();
    pthread_mutex_unlock(&s_mu);
}
