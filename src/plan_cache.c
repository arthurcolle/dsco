/* plan_cache.c — Fuzzy plan cache with fingerprint matching.
 *
 * Priority 4: Cache successful topology plans keyed by task fingerprint.
 * On cache hit with >=85% similarity, reuse the plan structure directly.
 *
 * Storage: ~/.dsco/plan_cache.jsonl  (LRU, max 256 entries)
 * Similarity: Jaccard on 3-grams of lowercased task string
 */

#include "plan_cache.h"
#include "topology.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#define CACHE_MAX     256
#define NGRAM_N       3
#define HIT_THRESHOLD 0.72f

typedef struct {
    char     fingerprint[64];        /* hex of task 3-gram set hash */
    char     task_excerpt[128];      /* first 127 chars of task */
    char     topology_name[48];
    char     rationale[128];
    float    last_fit;
    int      hits;
    time_t   created;
    time_t   last_hit;
    bool     occupied;
} cache_entry_t;

static cache_entry_t  s_cache[CACHE_MAX];
static int            s_count    = 0;
static bool           s_loaded   = false;
static pthread_mutex_t s_mu      = PTHREAD_MUTEX_INITIALIZER;

/* ── 3-gram fingerprint ────────────────────────────────────────────────── */

/* Build sorted array of 3-gram hashes from normalized task string.
 * Returns count; caller must free *out. */
static int build_ngrams(const char *task, uint32_t **out) {
    if (!task || !task[0]) { *out = NULL; return 0; }

    /* Normalize: lowercase, collapse spaces */
    size_t len = strlen(task);
    char *norm = malloc(len + 1);
    if (!norm) { *out = NULL; return 0; }
    size_t ni = 0;
    bool last_space = false;
    for (size_t i = 0; i < len; i++) {
        char c = (char)tolower((unsigned char)task[i]);
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!last_space && ni > 0) { norm[ni++] = ' '; last_space = true; }
        } else {
            norm[ni++] = c; last_space = false;
        }
    }
    norm[ni] = '\0';

    if (ni < (size_t)NGRAM_N) { free(norm); *out = NULL; return 0; }

    int max_grams = (int)(ni - NGRAM_N + 1);
    uint32_t *grams = malloc((size_t)max_grams * sizeof(uint32_t));
    if (!grams) { free(norm); *out = NULL; return 0; }

    for (int i = 0; i <= (int)ni - NGRAM_N; i++) {
        /* FNV-1a 3-gram hash */
        uint32_t h = 2166136261u;
        for (int j = 0; j < NGRAM_N; j++) {
            h ^= (uint8_t)norm[i+j];
            h *= 16777619u;
        }
        grams[i] = h;
    }
    free(norm);

    /* Sort for Jaccard */
    for (int i = 1; i < max_grams; i++) {
        uint32_t key = grams[i]; int j = i - 1;
        while (j >= 0 && grams[j] > key) { grams[j+1] = grams[j]; j--; }
        grams[j+1] = key;
    }

    *out = grams;
    return max_grams;
}

static float jaccard(uint32_t *a, int na, uint32_t *b, int nb) {
    if (!a || !b || na == 0 || nb == 0) return 0.0f;
    int intersect = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        if      (a[i] == b[j]) { intersect++; i++; j++; }
        else if (a[i]  < b[j]) i++;
        else                   j++;
    }
    int union_sz = na + nb - intersect;
    return (union_sz > 0) ? (float)intersect / (float)union_sz : 0.0f;
}

/* Compact fingerprint: FNV-1a over the ngram array */
static void ngrams_fingerprint(uint32_t *grams, int n, char *out64) {
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < n; i++) {
        uint8_t *b = (uint8_t*)&grams[i];
        for (int j = 0; j < 4; j++) { h ^= b[j]; h *= 1099511628211ull; }
    }
    snprintf(out64, 64, "%016llx", (unsigned long long)h);
}

/* ── File I/O ──────────────────────────────────────────────────────────── */

static void cache_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.dsco/plan_cache.jsonl", home ? home : "/tmp");
}

static void load_locked(void) {
    if (s_loaded) return;
    s_loaded = true;

    char path[512];
    cache_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f) && s_count < CACHE_MAX) {
        cache_entry_t *e = &s_cache[s_count];
        memset(e, 0, sizeof(*e));

        char *p;
#define JSGET_STR2(key, dst, dsz) do { \
    p = strstr(line, "\"" key "\":\""); \
    if (p) { p += strlen("\"" key "\":\""); \
              char *end = strchr(p, '"'); \
              if (end) { size_t n2 = (size_t)(end-p); if(n2>=dsz) n2=dsz-1; \
                         memcpy(dst, p, n2); dst[n2]='\0'; } } \
} while(0)
#define JSGET_FLT2(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) { p += strlen("\"" key "\":"); dst = (float)atof(p); } \
} while(0)
#define JSGET_INT2(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) { p += strlen("\"" key "\":"); dst = atoi(p); } \
} while(0)

        JSGET_STR2("fp",   e->fingerprint,   sizeof(e->fingerprint));
        JSGET_STR2("ex",   e->task_excerpt,  sizeof(e->task_excerpt));
        JSGET_STR2("topo", e->topology_name, sizeof(e->topology_name));
        JSGET_STR2("rat",  e->rationale,     sizeof(e->rationale));
        JSGET_FLT2("fit",  e->last_fit);
        JSGET_INT2("hits", e->hits);

        if (!e->fingerprint[0] || !e->topology_name[0]) continue;
        e->occupied = true;
        s_count++;
    }
    fclose(f);
}

static void persist_locked(void) {
    char path[512];
    cache_path(path, sizeof(path));

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.dsco", getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(dir, 0700);

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;

    /* LRU eviction: if over limit, drop oldest (low hits + old last_hit) */
    int count = 0;
    for (int i = 0; i < CACHE_MAX; i++) {
        if (!s_cache[i].occupied) continue;
        /* Escape rationale for JSON */
        char esc_rat[256] = {0};
        size_t ri = 0;
        for (const char *c = s_cache[i].rationale; *c && ri < sizeof(esc_rat)-4; c++) {
            if (*c == '"') { esc_rat[ri++] = '\\'; esc_rat[ri++] = '"'; }
            else            esc_rat[ri++] = *c;
        }
        fprintf(f,
            "{\"fp\":\"%s\",\"ex\":\"%s\",\"topo\":\"%s\","
            "\"rat\":\"%s\",\"fit\":%.4f,\"hits\":%d,\"ts\":%lld}\n",
            s_cache[i].fingerprint,
            s_cache[i].task_excerpt,
            s_cache[i].topology_name,
            esc_rat,
            s_cache[i].last_fit,
            s_cache[i].hits,
            (long long)s_cache[i].last_hit);
        if (++count >= CACHE_MAX) break;
    }
    fclose(f);
    rename(tmp, path);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void plan_cache_init(void) {
    pthread_mutex_lock(&s_mu);
    load_locked();
    pthread_mutex_unlock(&s_mu);
}

bool plan_cache_lookup(const char *task, plan_cache_result_t *result) {
    if (!task || !result) return false;

    uint32_t *grams = NULL;
    int ng = build_ngrams(task, &grams);
    if (ng == 0) return false;

    char fp[64];
    ngrams_fingerprint(grams, ng, fp);

    pthread_mutex_lock(&s_mu);
    load_locked();

    bool found = false;
    float best_score = 0.0f;
    int best_idx = -1;

    for (int i = 0; i < CACHE_MAX; i++) {
        if (!s_cache[i].occupied) continue;

        /* Fast path: exact fingerprint match */
        float sim;
        if (strcmp(s_cache[i].fingerprint, fp) == 0) {
            sim = 1.0f;
        } else {
            /* Fuzzy: re-compute ngrams from stored excerpt */
            uint32_t *eg = NULL;
            int en = build_ngrams(s_cache[i].task_excerpt, &eg);
            sim = jaccard(grams, ng, eg, en);
            free(eg);
        }

        if (sim >= HIT_THRESHOLD && sim > best_score) {
            best_score = sim;
            best_idx   = i;
        }
    }

    if (best_idx >= 0) {
        cache_entry_t *e = &s_cache[best_idx];
        strncpy(result->topology_name, e->topology_name, sizeof(result->topology_name)-1);
        strncpy(result->rationale,     e->rationale,     sizeof(result->rationale)-1);
        result->similarity = best_score;
        result->hits_before = e->hits;

        e->hits++;
        e->last_hit = time(NULL);
        found = true;
        persist_locked();
    }

    pthread_mutex_unlock(&s_mu);
    free(grams);
    return found;
}

void plan_cache_store(const char *task, const char *topology_name,
                      const char *rationale, float fit_score) {
    if (!task || !topology_name) return;

    uint32_t *grams = NULL;
    int ng = build_ngrams(task, &grams);
    if (ng == 0) return;

    char fp[64];
    ngrams_fingerprint(grams, ng, fp);
    free(grams);

    pthread_mutex_lock(&s_mu);
    load_locked();

    /* Check if already exists */
    for (int i = 0; i < CACHE_MAX; i++) {
        if (s_cache[i].occupied && strcmp(s_cache[i].fingerprint, fp) == 0) {
            /* Update existing */
            strncpy(s_cache[i].topology_name, topology_name, sizeof(s_cache[i].topology_name)-1);
            s_cache[i].last_fit  = fit_score;
            s_cache[i].last_hit  = time(NULL);
            persist_locked();
            pthread_mutex_unlock(&s_mu);
            return;
        }
    }

    /* Find empty slot or evict LRU */
    int slot = -1;
    for (int i = 0; i < CACHE_MAX; i++) {
        if (!s_cache[i].occupied) { slot = i; break; }
    }
    if (slot < 0) {
        /* Evict: find entry with fewest hits and oldest last_hit */
        int min_hits = INT32_MAX;
        time_t oldest = time(NULL);
        for (int i = 0; i < CACHE_MAX; i++) {
            if (s_cache[i].hits < min_hits ||
                (s_cache[i].hits == min_hits && s_cache[i].last_hit < oldest)) {
                min_hits = s_cache[i].hits;
                oldest   = s_cache[i].last_hit;
                slot     = i;
            }
        }
    }
    if (slot < 0) { pthread_mutex_unlock(&s_mu); return; }

    cache_entry_t *e = &s_cache[slot];
    memset(e, 0, sizeof(*e));
    strncpy(e->fingerprint,   fp,            sizeof(e->fingerprint)-1);
    strncpy(e->task_excerpt,  task,          sizeof(e->task_excerpt)-1);
    strncpy(e->topology_name, topology_name, sizeof(e->topology_name)-1);
    if (rationale) strncpy(e->rationale, rationale, sizeof(e->rationale)-1);
    e->last_fit  = fit_score;
    e->hits      = 0;
    e->created   = time(NULL);
    e->last_hit  = e->created;
    e->occupied  = true;
    if (slot >= s_count) s_count = slot + 1;

    persist_locked();
    pthread_mutex_unlock(&s_mu);
}

int plan_cache_stats_json(char *buf, size_t buflen) {
    pthread_mutex_lock(&s_mu);
    load_locked();

    int total = 0, total_hits = 0;
    for (int i = 0; i < CACHE_MAX; i++) {
        if (!s_cache[i].occupied) continue;
        total++;
        total_hits += s_cache[i].hits;
    }
    int n = snprintf(buf, buflen,
        "{\"entries\":%d,\"total_hits\":%d,\"max\":%d,\"threshold\":%.2f}",
        total, total_hits, CACHE_MAX, HIT_THRESHOLD);

    pthread_mutex_unlock(&s_mu);
    return n;
}

void plan_cache_flush(void) {
    pthread_mutex_lock(&s_mu);
    persist_locked();
    pthread_mutex_unlock(&s_mu);
}
