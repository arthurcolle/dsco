/* plan_cache.c — Fuzzy plan cache with LRU ring buffer and plan replay.
 *
 * Priority 4: Cache successful topology plans keyed by 3-gram fingerprint.
 * On cache hit with >=80% Jaccard similarity, reuse cached plan structure.
 *
 * Storage: ~/.dsco/cache/plans.json  (one JSON object per line, no array wrapper)
 * Per-entry plan JSON: ~/.dsco/cache/plan_<16-hex>.json
 * Capacity: 100 entries (LRU eviction)
 * Similarity: Jaccard on 3-grams of lowercased, normalized task string
 */

#include "plan_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define NGRAM_N 3
#define MAX_ENTITIES 16 /* max entity pairs for adapt() */

/* ── Globals ────────────────────────────────────────────────────────────── */

static plan_cache_t s_cache;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static bool s_loaded = false;

/* ── Path helpers ────────────────────────────────────────────────────────── */

static const char *home(void) {
    const char *h = getenv("HOME");
    return h ? h : "/tmp";
}

static void cache_dir(char *buf, size_t len) {
    snprintf(buf, len, "%s/.dsco/cache", home());
}

static void index_path(char *buf, size_t len) {
    snprintf(buf, len, "%s/.dsco/cache/plans.json", home());
}

static void entry_path(char *buf, size_t len, uint64_t task_hash) {
    snprintf(buf, len, "%s/.dsco/cache/plan_%016llx.json", home(), (unsigned long long)task_hash);
}

static void ensure_cache_dir(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.dsco", home());
    mkdir(dir, 0700);
    cache_dir(dir, sizeof(dir));
    mkdir(dir, 0700);
}

/* ── FNV-64 hash ─────────────────────────────────────────────────────────── */

static uint64_t fnv64(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ull;
    }
    return h;
}

/* ── 3-gram construction ─────────────────────────────────────────────────── */

/* Normalize task: lowercase + collapse whitespace. Returns heap string. */
static char *normalize(const char *task) {
    size_t len = strlen(task);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    size_t ni = 0;
    bool last_sp = false;
    for (size_t i = 0; i < len; i++) {
        char c = (char)tolower((unsigned char)task[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_sp && ni > 0) {
                out[ni++] = ' ';
                last_sp = true;
            }
        } else {
            out[ni++] = c;
            last_sp = false;
        }
    }
    out[ni] = '\0';
    return out;
}

/* Build sorted array of 3-gram hashes. Returns count; caller must free *out. */
static int build_ngrams(const char *task, uint32_t **out) {
    *out = NULL;
    if (!task || !task[0])
        return 0;

    char *norm = normalize(task);
    if (!norm)
        return 0;
    size_t ni = strlen(norm);

    if (ni < (size_t)NGRAM_N) {
        free(norm);
        return 0;
    }

    int max_g = (int)(ni - NGRAM_N + 1);
    uint32_t *g = malloc((size_t)max_g * sizeof(uint32_t));
    if (!g) {
        free(norm);
        return 0;
    }

    for (int i = 0; i <= (int)ni - NGRAM_N; i++) {
        uint32_t h = 2166136261u;
        for (int j = 0; j < NGRAM_N; j++) {
            h ^= (uint8_t)norm[i + j];
            h *= 16777619u;
        }
        g[i] = h;
    }
    free(norm);

    /* Insertion sort for Jaccard (n <= few thousand, fast enough) */
    for (int i = 1; i < max_g; i++) {
        uint32_t k = g[i];
        int j = i - 1;
        while (j >= 0 && g[j] > k) {
            g[j + 1] = g[j];
            j--;
        }
        g[j + 1] = k;
    }
    *out = g;
    return max_g;
}

/* ── Jaccard similarity ──────────────────────────────────────────────────── */

static float jaccard(uint32_t *a, int na, uint32_t *b, int nb) {
    if (!a || !b || na == 0 || nb == 0)
        return 0.0f;
    int inter = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            inter++;
            i++;
            j++;
        } else if (a[i] < b[j])
            i++;
        else
            j++;
    }
    int uni = na + nb - inter;
    return (uni > 0) ? (float)inter / (float)uni : 0.0f;
}

/* ── JSON helpers ────────────────────────────────────────────────────────── */

/* Minimal JSON string escaping into dst (len includes NUL). Returns chars written. */
static int json_esc(char *dst, size_t len, const char *src) {
    size_t di = 0;
    for (const char *s = src; *s && di + 4 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') {
            dst[di++] = '\\';
            dst[di++] = '"';
        } else if (c == '\\') {
            dst[di++] = '\\';
            dst[di++] = '\\';
        } else if (c == '\n') {
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c == '\r') {
            dst[di++] = '\\';
            dst[di++] = 'r';
        } else if (c == '\t') {
            dst[di++] = '\\';
            dst[di++] = 't';
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
    return (int)di;
}

/* Parse a JSON string value in place (unescape \", \\, \n etc.).
 * src points just after the opening quote. Writes into dst (dsz).
 * Returns pointer to char after the closing quote, or NULL on error. */
static const char *json_str_parse(const char *src, char *dst, size_t dsz) {
    size_t di = 0;
    while (*src && *src != '"') {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
                case '"':
                    dst[di++] = '"';
                    break;
                case '\\':
                    dst[di++] = '\\';
                    break;
                case 'n':
                    dst[di++] = '\n';
                    break;
                case 'r':
                    dst[di++] = '\r';
                    break;
                case 't':
                    dst[di++] = '\t';
                    break;
                default:
                    dst[di++] = *src;
                    break;
            }
            src++;
        } else {
            if (di + 1 < dsz)
                dst[di++] = *src;
            src++;
        }
    }
    if (di < dsz)
        dst[di] = '\0';
    return (*src == '"') ? src + 1 : NULL;
}

/* Extract string field value from a JSON line: "key":"value" */
static bool jsget_str(const char *line, const char *key, char *dst, size_t dsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(line, needle);
    if (!p)
        return false;
    p += strlen(needle);
    json_str_parse(p, dst, dsz);
    return true;
}

/* Extract numeric field: "key":number */
static bool jsget_num(const char *line, const char *key, double *dst) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(line, needle);
    if (!p)
        return false;
    p += strlen(needle);
    *dst = atof(p);
    return true;
}

/* ── LRU eviction ────────────────────────────────────────────────────────── */

/* Find least-recently-used slot (oldest last_used; ties broken by hit_count). */
static int lru_victim(void) {
    int best = -1;
    time_t oldest = (time_t)INT64_MAX;
    int min_hits = INT32_MAX;
    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        if (!s_cache.entries[i].occupied) {
            return i;
        } /* empty slot preferred */
        plan_cache_entry_t *e = &s_cache.entries[i];
        if (e->last_used < oldest || (e->last_used == oldest && e->hit_count < min_hits)) {
            oldest = e->last_used;
            min_hits = e->hit_count;
            best = i;
        }
    }
    return best;
}

static void evict_entry(int idx) {
    plan_cache_entry_t *e = &s_cache.entries[idx];
    /* Remove per-entry plan file if it exists */
    if (e->task_hash) {
        char path[512];
        entry_path(path, sizeof(path), e->task_hash);
        remove(path);
    }
    free(e->plan_json);
    memset(e, 0, sizeof(*e));
    s_cache.count--;
}

/* ── Disk I/O ────────────────────────────────────────────────────────────── */

static void load_locked(void) {
    if (s_loaded)
        return;
    s_loaded = true;

    char path[512];
    index_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '{')
            continue;
        if (s_cache.count >= PLAN_CACHE_MAX)
            break;

        int slot = lru_victim();
        if (slot < 0)
            break;
        if (s_cache.entries[slot].occupied)
            evict_entry(slot);

        plan_cache_entry_t *e = &s_cache.entries[slot];
        memset(e, 0, sizeof(*e));

        double dval = 0;
        char tmp[256] = {0};

        if (jsget_str(line, "tx", e->task_text, sizeof(e->task_text)))
            e->task_hash = fnv64(e->task_text);
        if (jsget_str(line, "topo", e->topology_name, sizeof(e->topology_name))) {
        }
        if (jsget_str(line, "rat", e->rationale, sizeof(e->rationale))) {
        }
        if (jsget_num(line, "fit", &dval))
            e->fit_score = (float)dval;
        if (jsget_num(line, "hits", &dval))
            e->hit_count = (int)dval;
        if (jsget_num(line, "lu", &dval))
            e->last_used = (time_t)dval;
        if (jsget_num(line, "cr", &dval))
            e->created = (time_t)dval;
        if (jsget_num(line, "th", &dval)) {
        } /* hash also in file; we recompute */

        /* Recompute hash from text if we got a task */
        if (jsget_str(line, "th16", tmp, sizeof(tmp))) {
            /* th16 is hex; parse it */
            e->task_hash = (uint64_t)strtoull(tmp, NULL, 16);
        } else {
            e->task_hash = fnv64(e->task_text);
        }

        if (!e->task_text[0] || !e->topology_name[0])
            continue;

        /* plan_json loaded lazily from per-entry file */
        e->plan_json = NULL;
        e->occupied = true;
        s_cache.count++;
    }
    fclose(f);
}

static void persist_locked(void) {
    ensure_cache_dir();

    char path[512], tmp[600];
    index_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    char esc_tx[512], esc_rat[256], esc_topo[96];

    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        plan_cache_entry_t *e = &s_cache.entries[i];
        if (!e->occupied)
            continue;

        json_esc(esc_tx, sizeof(esc_tx), e->task_text);
        json_esc(esc_rat, sizeof(esc_rat), e->rationale);
        json_esc(esc_topo, sizeof(esc_topo), e->topology_name);

        fprintf(f,
                "{\"th16\":\"%016llx\",\"tx\":\"%s\","
                "\"topo\":\"%s\",\"rat\":\"%s\","
                "\"fit\":%.4f,\"hits\":%d,\"lu\":%lld,\"cr\":%lld}\n",
                (unsigned long long)e->task_hash, esc_tx, esc_topo, esc_rat, e->fit_score,
                e->hit_count, (long long)e->last_used, (long long)e->created);
    }
    fclose(f);
    rename(tmp, path);
}

/* Persist plan_json for an entry to its per-entry file. */
static void persist_plan_json(const plan_cache_entry_t *e) {
    if (!e || !e->plan_json || !e->task_hash)
        return;
    ensure_cache_dir();
    char path[512];
    entry_path(path, sizeof(path), e->task_hash);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs(e->plan_json, f);
    fclose(f);
}

/* Load plan_json from per-entry file. Returns heap string or NULL. */
static char *load_plan_json(uint64_t task_hash) {
    char path[512];
    entry_path(path, sizeof(path), task_hash);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    /* Get file size */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    } /* 4 MB cap */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── Public lifecycle ────────────────────────────────────────────────────── */

void plan_cache_init(void) {
    pthread_mutex_lock(&s_mu);
    load_locked();
    pthread_mutex_unlock(&s_mu);
}

void plan_cache_free(void) {
    pthread_mutex_lock(&s_mu);
    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        free(s_cache.entries[i].plan_json);
        s_cache.entries[i].plan_json = NULL;
        s_cache.entries[i].occupied = false;
    }
    s_cache.head = 0;
    s_cache.count = 0;
    s_loaded = false;
    pthread_mutex_unlock(&s_mu);
}

void plan_cache_load(void) {
    pthread_mutex_lock(&s_mu);
    s_loaded = false; /* force reload */
    load_locked();
    pthread_mutex_unlock(&s_mu);
}

void plan_cache_save(void) {
    pthread_mutex_lock(&s_mu);
    persist_locked();
    pthread_mutex_unlock(&s_mu);
}

void plan_cache_flush(void) {
    plan_cache_save();
}

/* ── Lookup ──────────────────────────────────────────────────────────────── */

bool plan_cache_lookup(const char *task, plan_cache_result_t *result) {
    if (!task || !result)
        return false;

    uint32_t *grams = NULL;
    int ng = build_ngrams(task, &grams);
    if (ng == 0)
        return false;

    pthread_mutex_lock(&s_mu);
    load_locked();

    bool found = false;
    float best = 0.0f;
    int bidx = -1;

    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        plan_cache_entry_t *e = &s_cache.entries[i];
        if (!e->occupied)
            continue;

        float sim;
        uint64_t th = fnv64(task);
        if (th == e->task_hash) {
            sim = 1.0f;
        } else {
            uint32_t *eg = NULL;
            int en = build_ngrams(e->task_text, &eg);
            sim = jaccard(grams, ng, eg, en);
            free(eg);
        }

        if (sim >= PLAN_CACHE_MIN_SIM && sim > best) {
            best = sim;
            bidx = i;
        }
    }

    if (bidx >= 0) {
        plan_cache_entry_t *e = &s_cache.entries[bidx];
        strncpy(result->topology_name, e->topology_name, sizeof(result->topology_name) - 1);
        result->topology_name[sizeof(result->topology_name) - 1] = '\0';
        strncpy(result->rationale, e->rationale, sizeof(result->rationale) - 1);
        result->rationale[sizeof(result->rationale) - 1] = '\0';
        result->similarity = best;
        result->hits_before = e->hit_count;
        e->hit_count++;
        e->last_used = time(NULL);
        found = true;
        persist_locked();
    }

    pthread_mutex_unlock(&s_mu);
    free(grams);
    return found;
}

/* ── Store ───────────────────────────────────────────────────────────────── */

/* Internal: find or create slot for task_hash. Returns index or -1. */
static int find_or_alloc_locked(uint64_t th, const char *task) {
    /* Check existing */
    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        if (s_cache.entries[i].occupied && s_cache.entries[i].task_hash == th)
            return i;
    }
    /* Need a new slot */
    int slot = lru_victim();
    if (slot < 0)
        return -1;
    if (s_cache.entries[slot].occupied)
        evict_entry(slot);

    plan_cache_entry_t *e = &s_cache.entries[slot];
    memset(e, 0, sizeof(*e));
    e->task_hash = th;
    strncpy(e->task_text, task, sizeof(e->task_text) - 1);
    e->created = time(NULL);
    e->last_used = e->created;
    e->occupied = true;
    s_cache.count++;
    return slot;
}

void plan_cache_store(const char *task, const char *topology_name, const char *rationale,
                      float fit_score) {
    if (!task || !topology_name)
        return;

    uint64_t th = fnv64(task);
    pthread_mutex_lock(&s_mu);
    load_locked();

    int slot = find_or_alloc_locked(th, task);
    if (slot >= 0) {
        plan_cache_entry_t *e = &s_cache.entries[slot];
        strncpy(e->topology_name, topology_name, sizeof(e->topology_name) - 1);
        if (rationale)
            strncpy(e->rationale, rationale, sizeof(e->rationale) - 1);
        e->fit_score = fit_score;
        e->last_used = time(NULL);
        persist_locked();
    }

    pthread_mutex_unlock(&s_mu);
}

void plan_cache_store_json(const char *task, const char *plan_json) {
    if (!task || !plan_json)
        return;

    uint64_t th = fnv64(task);
    pthread_mutex_lock(&s_mu);
    load_locked();

    int slot = find_or_alloc_locked(th, task);
    if (slot >= 0) {
        plan_cache_entry_t *e = &s_cache.entries[slot];
        free(e->plan_json);
        e->plan_json = strdup(plan_json);
        e->last_used = time(NULL);
        persist_plan_json(e);
        persist_locked();
    }

    pthread_mutex_unlock(&s_mu);
}

/* ── Find entry (read-only pointer) ─────────────────────────────────────── */

const plan_cache_entry_t *plan_cache_find_entry(const char *task) {
    if (!task)
        return NULL;

    uint32_t *grams = NULL;
    int ng = build_ngrams(task, &grams);
    if (ng == 0)
        return NULL;

    pthread_mutex_lock(&s_mu);
    load_locked();

    float best = 0.0f;
    int bidx = -1;
    uint64_t th = fnv64(task);

    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        plan_cache_entry_t *e = &s_cache.entries[i];
        if (!e->occupied)
            continue;
        float sim;
        if (th == e->task_hash) {
            sim = 1.0f;
        } else {
            uint32_t *eg = NULL;
            int en = build_ngrams(e->task_text, &eg);
            sim = jaccard(grams, ng, eg, en);
            free(eg);
        }
        if (sim >= PLAN_CACHE_MIN_SIM && sim > best) {
            best = sim;
            bidx = i;
        }
    }

    const plan_cache_entry_t *ret = NULL;
    if (bidx >= 0) {
        plan_cache_entry_t *e = &s_cache.entries[bidx];
        /* Lazily load plan_json if available on disk but not in memory */
        if (!e->plan_json && e->task_hash) {
            e->plan_json = load_plan_json(e->task_hash);
        }
        ret = e;
    }

    pthread_mutex_unlock(&s_mu);
    free(grams);
    return ret;
}

/* ── Stats ───────────────────────────────────────────────────────────────── */

int plan_cache_stats_json(char *buf, size_t buflen) {
    pthread_mutex_lock(&s_mu);
    load_locked();

    int total = 0, total_hits = 0, with_json = 0;
    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        if (!s_cache.entries[i].occupied)
            continue;
        total++;
        total_hits += s_cache.entries[i].hit_count;
        if (s_cache.entries[i].plan_json)
            with_json++;
    }
    float hit_rate = (total > 0) ? (float)total_hits / (float)(total + total_hits) : 0.0f;
    int n = snprintf(buf, buflen,
                     "{\"entries\":%d,\"total_hits\":%d,\"with_plan_json\":%d,"
                     "\"max\":%d,\"threshold\":%.2f,\"hit_rate\":%.3f}",
                     total, total_hits, with_json, PLAN_CACHE_MAX, PLAN_CACHE_MIN_SIM, hit_rate);

    pthread_mutex_unlock(&s_mu);
    return n;
}

/* ── plan_similarity_score ───────────────────────────────────────────────── */

float plan_similarity_score(const char *task_a, const char *task_b) {
    if (!task_a || !task_b)
        return 0.0f;
    if (strcmp(task_a, task_b) == 0)
        return 1.0f;

    uint32_t *ga = NULL, *gb = NULL;
    int na = build_ngrams(task_a, &ga);
    int nb = build_ngrams(task_b, &gb);
    float score = jaccard(ga, na, gb, nb);
    free(ga);
    free(gb);
    return score;
}

/* ── plan_cache_adapt ────────────────────────────────────────────────────── */

/* Extract uppercase-only tokens (tickers, acronyms) >= 2 chars from text.
 * Also accepts ALL_CAPS tokens with digits (e.g. "BTC", "S&P500" → "BTC"). */
static int extract_entities(const char *text, char ents[][32], int max) {
    int count = 0;
    const char *p = text;
    while (*p && count < max) {
        while (*p && !isalpha((unsigned char)*p))
            p++;
        if (!*p)
            break;
        /* Collect token: alpha/digit only */
        char tok[64];
        int ti = 0;
        while (*p && (isalnum((unsigned char)*p)) && ti < 63)
            tok[ti++] = *p++;
        tok[ti] = '\0';
        if (ti < 2)
            continue;
        /* Must be ALL uppercase */
        bool all_up = true;
        for (int i = 0; i < ti; i++) {
            if (islower((unsigned char)tok[i])) {
                all_up = false;
                break;
            }
        }
        /* Must have at least one alpha char */
        bool has_alpha = false;
        for (int i = 0; i < ti; i++) {
            if (isalpha((unsigned char)tok[i])) {
                has_alpha = true;
                break;
            }
        }
        if (all_up && has_alpha) {
            /* Deduplicate */
            bool dup = false;
            for (int d = 0; d < count; d++) {
                if (strcmp(ents[d], tok) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup)
                strncpy(ents[count++], tok, 31);
        }
    }
    return count;
}

/* Simple string replace: replace all occurrences of `from` with `to` in `src`.
 * Returns new heap-allocated string. Caller must free. */
static char *str_replace_all(const char *src, const char *from, const char *to) {
    if (!src || !from || !*from)
        return strdup(src ? src : "");
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    /* Count occurrences */
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, from))) {
        count++;
        p += flen;
    }
    if (count == 0)
        return strdup(src);

    size_t srclen = strlen(src);
    size_t newlen = srclen + count * (tlen - flen + 1) + 1;
    char *out = malloc(newlen);
    if (!out)
        return NULL;

    char *w = out;
    p = src;
    while (*p) {
        if (strncmp(p, from, flen) == 0) {
            memcpy(w, to, tlen);
            w += tlen;
            p += flen;
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
    return out;
}

char *plan_cache_adapt(const plan_cache_entry_t *entry, const char *new_task) {
    if (!entry || !new_task)
        return NULL;

    /* Load plan_json lazily if not in memory */
    char *pj = entry->plan_json;
    char *pj_loaded = NULL;
    if (!pj && entry->task_hash) {
        pj = pj_loaded = load_plan_json(entry->task_hash);
    }
    if (!pj)
        return NULL;

    /* Extract entities from both tasks */
    char old_ents[MAX_ENTITIES][32];
    char new_ents[MAX_ENTITIES][32];
    int n_old = extract_entities(entry->task_text, old_ents, MAX_ENTITIES);
    int n_new = extract_entities(new_task, new_ents, MAX_ENTITIES);

    int n_pairs = (n_old < n_new) ? n_old : n_new;

    /* Apply substitutions iteratively */
    char *result = strdup(pj);
    free(pj_loaded);
    if (!result)
        return NULL;

    for (int i = 0; i < n_pairs; i++) {
        if (strcmp(old_ents[i], new_ents[i]) == 0)
            continue;
        char *next = str_replace_all(result, old_ents[i], new_ents[i]);
        free(result);
        if (!next)
            return NULL;
        result = next;
    }

    return result;
}
