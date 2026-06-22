/* learned_cost.c — k-NN learned cost model for topology-aware prediction.
 *
 * Algorithm: for a query (task, topology), find the k nearest historical
 * executions (same topology, closest task_len + atom_count), then return
 * a weighted inverse-distance average of their actual costs.
 *
 * Storage: ~/.dsco/cost_history.json  (JSONL; override with DSCO_COST_HISTORY)
 * Thread safety: none — callers must serialize if needed.
 */

#include "learned_cost.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LEARNED_COST_VERSION 1
#define DEFAULT_K            5

/* Normalization denominators for the distance metric.
 * These cap the per-feature contribution to [0, 1] for typical tasks. */
#define NORM_TASK_LEN    512.0
#define NORM_ATOM_COUNT   20.0

/* Feature weights must sum to 1.0 */
#define W_TASK_LEN    0.65
#define W_ATOM_COUNT  0.35

/* ── Path helpers ──────────────────────────────────────────────────────── */

static void history_path(char *buf, size_t len) {
    const char *override = getenv("DSCO_COST_HISTORY");
    if (override && override[0]) { snprintf(buf, len, "%s", override); return; }
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.dsco/cost_history.json", home ? home : "/tmp");
}

static void ensure_dsco_dir(void) {
    const char *home = getenv("HOME");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.dsco", home ? home : "/tmp");
    mkdir(dir, S_IRWXU);
}

/* ── Feature extraction ────────────────────────────────────────────────── */

/* Estimate sub-task count from task string using conjunction heuristics. */
static int infer_atom_count(const char *task) {
    if (!task || !task[0]) return 1;
    int count = 1;
    const char *p = task;
    while (*p) {
        if (*p == ';') {
            count++;
        } else if (strncasecmp(p, " and ", 5) == 0 ||
                   strncasecmp(p, " then ", 6) == 0 ||
                   strncasecmp(p, " also ", 6) == 0 ||
                   strncasecmp(p, " plus ", 6) == 0 ||
                   strncasecmp(p, " next ", 6) == 0) {
            count++;
        } else if (*p == ',' && p[1] == ' ' && isupper((unsigned char)p[2])) {
            /* "Foo, Bar, Baz" style enumeration */
            count++;
        }
        p++;
    }
    return count < 1 ? 1 : (count > 20 ? 20 : count);
}

/* ── Distance metric ───────────────────────────────────────────────────── */

/* Returns normalized distance in [0, ~sqrt(2)]. Topology must already match. */
static double feature_distance(const cost_record_t *r, int task_len, int atom_count) {
    double dt = (r->task_len   - task_len)   / NORM_TASK_LEN;
    double da = (r->atom_count - atom_count) / NORM_ATOM_COUNT;
    return sqrt(W_TASK_LEN * dt * dt + W_ATOM_COUNT * da * da);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void cost_db_init(cost_db_t *db) {
    if (!db) return;
    memset(db, 0, sizeof(*db));
}

/* ── Load ──────────────────────────────────────────────────────────────── */

bool cost_db_load(cost_db_t *db) {
    if (!db || db->loaded) return true;
    db->loaded = true;

    char path[512];
    history_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return true; /* no history yet is fine */

    char line[512];
    bool meta_read = false;
    int  fill = 0;

    while (fgets(line, sizeof(line), f)) {
        if (!meta_read) {
            meta_read = true;
            /* First line: {"__meta__":1,"version":N,"count":N,"head":N} — ignored
             * for actual data; we just fill entries in file order. */
            continue;
        }

        if (fill >= COST_DB_CAPACITY) break;

        char *p;
        int tl = 0, ac = 0, tk = 0;
        double c = 0.0;
        long long ts = 0;
        char tp[48] = {0};

#define JSGET_INT(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) dst = atoi(p + (int)strlen("\"" key "\":"));  \
} while (0)
#define JSGET_DBL(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) dst = atof(p + (int)strlen("\"" key "\":"));  \
} while (0)
#define JSGET_LL(key, dst) do { \
    p = strstr(line, "\"" key "\":"); \
    if (p) dst = atoll(p + (int)strlen("\"" key "\":"));  \
} while (0)

        JSGET_INT("tl", tl);
        JSGET_INT("ac", ac);
        JSGET_INT("tk", tk);
        JSGET_DBL("c",  c);
        JSGET_LL ("ts", ts);
        p = strstr(line, "\"tp\":\"");
        if (p) {
            p += 6;
            char *end = strchr(p, '"');
            if (end) {
                size_t n = (size_t)(end - p);
                if (n >= sizeof(tp)) n = sizeof(tp) - 1;
                memcpy(tp, p, n);
                tp[n] = '\0';
            }
        }

        if (!tp[0] || c <= 0.0 || tk <= 0) continue;

        cost_record_t *r = &db->entries[fill];
        r->task_len   = tl;
        r->atom_count = (ac > 0) ? ac : 1;
        r->tokens     = tk;
        r->cost       = c;
        r->timestamp  = (time_t)ts;
        strncpy(r->topology, tp, sizeof(r->topology) - 1);
        r->topology[sizeof(r->topology) - 1] = '\0';
        fill++;
    }
    fclose(f);

#undef JSGET_INT
#undef JSGET_DBL
#undef JSGET_LL

    db->count = fill;
    db->head  = fill % COST_DB_CAPACITY;
    return true;
}

/* ── Save ──────────────────────────────────────────────────────────────── */

bool cost_db_save(cost_db_t *db) {
    if (!db || !db->dirty) return true;

    ensure_dsco_dir();

    char path[512], tmp[600];
    history_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    /* Metadata header line */
    fprintf(f, "{\"__meta__\":1,\"version\":%d,\"count\":%d,\"head\":%d}\n",
            LEARNED_COST_VERSION, db->count, db->head);

    /* Records in oldest-first order */
    int cap = (db->count < COST_DB_CAPACITY) ? db->count : COST_DB_CAPACITY;
    for (int i = 0; i < cap; i++) {
        int idx = (db->count < COST_DB_CAPACITY)
                ? i
                : (db->head + i) % COST_DB_CAPACITY;
        const cost_record_t *r = &db->entries[idx];
        fprintf(f,
            "{\"v\":%d,\"tl\":%d,\"ac\":%d,\"tk\":%d,"
            "\"c\":%.8f,\"tp\":\"%s\",\"ts\":%lld}\n",
            LEARNED_COST_VERSION,
            r->task_len, r->atom_count, r->tokens,
            r->cost, r->topology, (long long)r->timestamp);
    }

    fclose(f);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    db->dirty = false;
    return true;
}

/* ── Learn ──────────────────────────────────────────────────────────────── */

void learn_from_execution(cost_db_t *db,
                          const char *task,
                          const char *topology,
                          int actual_tokens,
                          double actual_cost) {
    if (!db || !task || !topology || actual_tokens <= 0 || actual_cost < 0.0) return;

    if (!db->loaded) cost_db_load(db);

    cost_record_t *r = &db->entries[db->head];
    r->task_len   = (int)strlen(task);
    r->atom_count = infer_atom_count(task);
    r->tokens     = actual_tokens;
    r->cost       = actual_cost;
    r->timestamp  = time(NULL);
    strncpy(r->topology, topology, sizeof(r->topology) - 1);
    r->topology[sizeof(r->topology) - 1] = '\0';

    db->head = (db->head + 1) % COST_DB_CAPACITY;
    if (db->count < COST_DB_CAPACITY) db->count++;
    db->dirty = true;
    cost_db_save(db);
}

/* ── Find similar ──────────────────────────────────────────────────────── */

int find_similar_executions(const cost_db_t *db,
                            const char *task,
                            const char *topology,
                            int k,
                            cost_record_t *out_records) {
    if (!db || !task || !topology || k <= 0 || !out_records) return 0;
    if (k > LEARNED_COST_K_MAX) k = LEARNED_COST_K_MAX;

    int    task_len   = (int)strlen(task);
    int    atom_count = infer_atom_count(task);

    typedef struct { double dist; int idx; } scored_t;
    /* Stack-allocate the scored list; COST_DB_CAPACITY is 1000 */
    scored_t scored[COST_DB_CAPACITY];
    int n_scored = 0;

    for (int i = 0; i < db->count; i++) {
        const cost_record_t *r = &db->entries[i];
        if (strcmp(r->topology, topology) != 0) continue;
        scored[n_scored].dist = feature_distance(r, task_len, atom_count);
        scored[n_scored].idx  = i;
        n_scored++;
    }

    if (n_scored == 0) return 0;

    int actual_k = (k < n_scored) ? k : n_scored;

    /* Partial selection sort — O(n*k) but k≤20 and n≤1000, so fine. */
    for (int i = 0; i < actual_k; i++) {
        int   min_j = i;
        for (int j = i + 1; j < n_scored; j++) {
            if (scored[j].dist < scored[min_j].dist) min_j = j;
        }
        if (min_j != i) {
            scored_t tmp = scored[i]; scored[i] = scored[min_j]; scored[min_j] = tmp;
        }
        out_records[i] = db->entries[scored[i].idx];
    }

    return actual_k;
}

/* ── Predict ────────────────────────────────────────────────────────────── */

bool predict_cost(const cost_db_t *db,
                  const char *task,
                  const char *topology,
                  cost_prediction_result_t *out) {
    if (!db || !task || !topology || !out) return false;

    cost_record_t neighbors[LEARNED_COST_K_MAX];
    int n = find_similar_executions(db, task, topology, DEFAULT_K, neighbors);

    if (n == 0) {
        out->predicted_cost = -1.0;
        out->confidence     = 0.0;
        out->k_used         = 0;
        return false;
    }

    int task_len   = (int)strlen(task);
    int atom_count = infer_atom_count(task);

    double sum_w  = 0.0;
    double sum_wc = 0.0;
    double sum_d  = 0.0;

    for (int i = 0; i < n; i++) {
        double d = feature_distance(&neighbors[i], task_len, atom_count);
        double w = 1.0 / (d + 1e-9);  /* inverse distance weight */
        sum_w  += w;
        sum_wc += w * neighbors[i].cost;
        sum_d  += d;
    }

    out->predicted_cost = sum_wc / sum_w;
    out->k_used         = n;

    /* Confidence rises with k and falls with average distance.
     * exp(-3*avg_dist): avg_dist=0 → 1.0, avg_dist=0.5 → 0.22 */
    double avg_d    = sum_d / n;
    double k_frac   = (double)n / 10.0;  /* saturates at n=10 */
    if (k_frac > 1.0) k_frac = 1.0;
    double dist_fac = exp(-3.0 * avg_d);

    out->confidence = 0.95 * (0.50 * k_frac + 0.50 * dist_fac);
    if (out->confidence > 0.95) out->confidence = 0.95;

    return true;
}
