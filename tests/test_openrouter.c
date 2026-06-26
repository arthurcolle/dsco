/* tests/test_openrouter.c — Daily OpenRouter frontier model eval harness.
 *
 * Build:   make openrouter_tester
 * Run:     OPENROUTER_API_KEY=sk-or-... ./openrouter_tester [options]
 *
 * Options:
 *   --refresh        Force re-fetch model list even if fetched today
 *   --report         Only print report, no new evals
 *   --filter <str>   Only test models whose ID contains <str>
 *   --db <path>      Override DB path (default: ~/.dsco/openrouter_evals.db)
 *
 * Behavior:
 *  1. Fetch live model list from OpenRouter /api/v1/models (once per day)
 *  2. Filter out top 15% most expensive + bottom 15% cheapest by combined price
 *  3. Filter out models created more than 18 months ago (stale/deprecated)
 *  4. For each remaining model run 3 eval tiers: 1-token, 30-token, 300-token
 *  5. Persist everything to SQLite under ~/.dsco/openrouter_evals.db
 *  6. Print inverted map report: model-family → {provider, cost, latency per tier}
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <sqlite3.h>

#include "json_util.h"

/* Required by llm.c / tools.c when linked against lib objects */
volatile int g_interrupted = 0;

/* ── Constants ──────────────────────────────────────────────────────────── */

#define OR_MODELS_URL "https://openrouter.ai/api/v1/models"
#define OR_CHAT_URL "https://openrouter.ai/api/v1/chat/completions"
#define MAX_MODELS 2048
#define PRICE_PERCENTILE 0.15 /* cut top 15% and bottom 15% */
/* No age cutoff — OpenRouter manages model lifecycle via the live API */
#define CONNECT_TIMEOUT 20L
#define EVAL_TIMEOUT 90L /* per-request timeout */

/* Eval tier definitions */
typedef struct {
    int target_tokens; /* max_tokens to request */
    const char *prompt;
} eval_tier_t;

static const eval_tier_t TIERS[] = {
    {32, "Respond with exactly the single letter A, nothing else."},
    {64, "List the days of the week separated by commas. Nothing else."},
    {384, "Describe the process of photosynthesis in 3 short paragraphs."},
};
#define NTIERS 3

/* ── Data structures ────────────────────────────────────────────────────── */

typedef struct {
    char id[256];
    char name[256];
    char provider[64]; /* derived from id prefix before '/' */
    long created;      /* Unix timestamp from API */
    int context_length;
    double prompt_price;     /* per token, USD */
    double completion_price; /* per token, USD */
    double combined_price;   /* prompt + completion, for sorting */
    bool include;            /* passed percentile filter */
} or_model_t;

typedef struct {
    char model_id[256];
    char run_date[16]; /* YYYY-MM-DD */
    int tier;          /* 0/1/2 */
    int target_tokens;
    int actual_tokens;
    double latency_ms;
    int http_status;
    bool ok;
    char snippet[128];
    char error_msg[256];
} eval_result_t;

/* ── HTTP buffer ────────────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buf_t;

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t total = size * nmemb;
    http_buf_t *b = (http_buf_t *)ud;
    if (b->len + total + 1 > b->cap) {
        b->cap = (b->len + total + 1) * 2 + 4096;
        char *tmp = realloc(b->data, b->cap);
        if (!tmp)
            return 0;
        b->data = tmp;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static http_buf_t http_buf_new(void) {
    http_buf_t b;
    b.cap = 65536;
    b.len = 0;
    b.data = malloc(b.cap);
    if (b.data)
        b.data[0] = '\0';
    return b;
}

/* ── SQLite helpers ─────────────────────────────────────────────────────── */

static sqlite3 *g_db = NULL;

static bool db_open(const char *path) {
    /* Ensure directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "error: cannot open DB %s: %s\n", path, sqlite3_errmsg(g_db));
        return false;
    }
    sqlite3_busy_timeout(g_db, 5000);
    return true;
}

static bool db_exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static bool db_init(void) {
    return db_exec("PRAGMA journal_mode=WAL;"

                   "CREATE TABLE IF NOT EXISTS models ("
                   "  id               TEXT PRIMARY KEY,"
                   "  name             TEXT,"
                   "  provider         TEXT,"
                   "  created          INTEGER,"
                   "  context_length   INTEGER,"
                   "  prompt_price     REAL,"
                   "  completion_price REAL,"
                   "  last_seen        TEXT" /* YYYY-MM-DD */
                   ");"

                   "CREATE TABLE IF NOT EXISTS evals ("
                   "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "  model_id         TEXT,"
                   "  run_date         TEXT,"
                   "  tier             INTEGER,"
                   "  target_tokens    INTEGER,"
                   "  actual_tokens    INTEGER,"
                   "  latency_ms       REAL,"
                   "  http_status      INTEGER,"
                   "  ok               INTEGER,"
                   "  snippet          TEXT,"
                   "  error_msg        TEXT"
                   ");"

                   "CREATE INDEX IF NOT EXISTS idx_evals_model  ON evals(model_id);"
                   "CREATE INDEX IF NOT EXISTS idx_evals_date   ON evals(run_date);");
}

static void db_upsert_model(const or_model_t *m, const char *today) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO models(id,name,provider,created,context_length,"
                      "prompt_price,completion_price,last_seen) VALUES(?,?,?,?,?,?,?,?)"
                      " ON CONFLICT(id) DO UPDATE SET"
                      "  name=excluded.name, provider=excluded.provider,"
                      "  created=excluded.created, context_length=excluded.context_length,"
                      "  prompt_price=excluded.prompt_price,"
                      "  completion_price=excluded.completion_price,"
                      "  last_seen=excluded.last_seen;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, m->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, m->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, m->provider, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, m->created);
    sqlite3_bind_int(st, 5, m->context_length);
    sqlite3_bind_double(st, 6, m->prompt_price);
    sqlite3_bind_double(st, 7, m->completion_price);
    sqlite3_bind_text(st, 8, today, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void db_insert_eval(const eval_result_t *e) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO evals(model_id,run_date,tier,target_tokens,actual_tokens,"
                      "latency_ms,http_status,ok,snippet,error_msg) VALUES(?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, e->model_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, e->run_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, e->tier);
    sqlite3_bind_int(st, 4, e->target_tokens);
    sqlite3_bind_int(st, 5, e->actual_tokens);
    sqlite3_bind_double(st, 6, e->latency_ms);
    sqlite3_bind_int(st, 7, e->http_status);
    sqlite3_bind_int(st, 8, e->ok ? 1 : 0);
    sqlite3_bind_text(st, 9, e->snippet, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, e->error_msg, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Returns true if we already ran evals for this model+date+tier */
static bool db_already_ran(const char *model_id, const char *today, int tier) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT 1 FROM evals WHERE model_id=? AND run_date=? AND tier=? LIMIT 1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, model_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, today, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, tier);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* Check if models were fetched today */
static bool db_models_fresh(const char *today) {
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT 1 FROM models WHERE last_seen=? LIMIT 1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, today, -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* Remove models whose last_seen is not today (no longer in live list) */
static void db_prune_stale(const char *today) {
    sqlite3_stmt *st = NULL;
    const char *sql = "DELETE FROM models WHERE last_seen != ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, today, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    int removed = sqlite3_changes(g_db);
    sqlite3_finalize(st);
    if (removed > 0)
        fprintf(stderr, "  pruned %d stale models from DB\n", removed);
}

/* ── Model fetching & parsing ───────────────────────────────────────────── */

static char *fetch_url(const char *url, const char *api_key) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;

    http_buf_t buf = http_buf_new();
    if (!buf.data) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    struct curl_slist *hdrs = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "HTTP-Referer: https://github.com/dsco-cli");
    hdrs = curl_slist_append(hdrs, "X-Title: dsco-cli-eval");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http != 200) {
        fprintf(stderr, "error: fetch %s → HTTP %ld curl=%d\n", url, http, res);
        free(buf.data);
        return NULL;
    }
    return buf.data; /* caller must free */
}

/* Per-element callback context for json_array_foreach */
typedef struct {
    or_model_t *models;
    int count;
    int cap;
    long cutoff_created; /* reject models older than this */
} parse_ctx_t;

static void parse_model_elem(const char *elem, void *ctx_ptr) {
    parse_ctx_t *ctx = (parse_ctx_t *)ctx_ptr;
    if (ctx->count >= ctx->cap)
        return;

    or_model_t *m = &ctx->models[ctx->count];
    memset(m, 0, sizeof(*m));

    char *id = json_get_str(elem, "id");
    if (!id || !id[0]) {
        free(id);
        return;
    }
    snprintf(m->id, sizeof(m->id), "%s", id);
    free(id);

    /* Skip openrouter/ meta-routers — routing abstractions, not real models */
    if (strncmp(m->id, "openrouter/", 11) == 0)
        return;

    /* Derive provider from id prefix ("openai/gpt-4o" → "openai") */
    char *slash = strchr(m->id, '/');
    if (slash) {
        size_t plen = (size_t)(slash - m->id);
        if (plen >= sizeof(m->provider))
            plen = sizeof(m->provider) - 1;
        memcpy(m->provider, m->id, plen);
        m->provider[plen] = '\0';
    } else {
        snprintf(m->provider, sizeof(m->provider), "%s", m->id);
    }

    char *name = json_get_str(elem, "name");
    if (name) {
        snprintf(m->name, sizeof(m->name), "%s", name);
        free(name);
    } else {
        snprintf(m->name, sizeof(m->name), "%s", m->id);
    }

    m->created = (long)json_get_int(elem, "created", 0);
    m->context_length = json_get_int(elem, "context_length", 0);

    /* Parse pricing sub-object */
    char *pricing = json_get_raw(elem, "pricing");
    if (pricing) {
        char *pp = json_get_str(pricing, "prompt");
        char *cp = json_get_str(pricing, "completion");
        if (pp) {
            m->prompt_price = atof(pp);
            free(pp);
        }
        if (cp) {
            m->completion_price = atof(cp);
            free(cp);
        }
        free(pricing);
    }
    m->combined_price = m->prompt_price + m->completion_price;

    /* Age filter: skip if older than cutoff */
    if (ctx->cutoff_created > 0 && m->created > 0 && m->created < ctx->cutoff_created)
        return;

    ctx->count++;
}

/* qsort comparator by combined_price ascending */
static int cmp_price_asc(const void *a, const void *b) {
    const or_model_t *ma = (const or_model_t *)a;
    const or_model_t *mb = (const or_model_t *)b;
    if (ma->combined_price < mb->combined_price)
        return -1;
    if (ma->combined_price > mb->combined_price)
        return 1;
    return 0;
}

/* Parse models JSON, filter by percentile, mark include=true for middle 70% */
static int parse_and_filter(const char *json, or_model_t *models, int cap, const char *today) {
    parse_ctx_t ctx = {
        .models = models, .count = 0, .cap = cap, .cutoff_created = 0}; /* no age cutoff */
    json_array_foreach(json, "data", parse_model_elem, &ctx);
    int total = ctx.count;

    if (total == 0)
        return 0;

    /* Sort by combined price to apply percentile cuts */
    qsort(models, (size_t)total, sizeof(or_model_t), cmp_price_asc);

    int lo = (int)floor(total * PRICE_PERCENTILE);
    int hi = (int)ceil(total * (1.0 - PRICE_PERCENTILE));

    for (int i = 0; i < total; i++)
        models[i].include = (i >= lo && i < hi);

    /* Free/zero price is always excluded (usually image-only or moderation) */
    for (int i = 0; i < total; i++)
        if (models[i].combined_price <= 0.0)
            models[i].include = false;

    (void)today;
    return total;
}

/* ── JSON helpers ───────────────────────────────────────────────────────── */

/* Return pointer to first object element inside a JSON array "[{...},...]" */
static const char *array_first_elem(const char *arr) {
    if (!arr)
        return NULL;
    const char *p = arr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '[') {
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
    }
    return (*p == '{') ? p : NULL;
}

/* ── Single model eval ──────────────────────────────────────────────────── */

static eval_result_t run_eval(const char *model_id, const char *api_key, int tier,
                              const char *today) {
    eval_result_t r = {0};
    snprintf(r.model_id, sizeof(r.model_id), "%s", model_id);
    snprintf(r.run_date, sizeof(r.run_date), "%s", today);
    r.tier = tier;
    r.target_tokens = TIERS[tier].target_tokens;

    /* Build request */
    char body[2048];
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\","
             "\"max_tokens\":%d,"
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             model_id, TIERS[tier].target_tokens, TIERS[tier].prompt);

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(r.error_msg, sizeof(r.error_msg), "curl init");
        return r;
    }

    http_buf_t buf = http_buf_new();
    if (!buf.data) {
        curl_easy_cleanup(curl);
        return r;
    }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "HTTP-Referer: https://github.com/dsco-cli");
    hdrs = curl_slist_append(hdrs, "X-Title: dsco-cli-eval");

    curl_easy_setopt(curl, CURLOPT_URL, OR_CHAT_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, EVAL_TIMEOUT);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    r.latency_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    r.http_status = (int)http_code;

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(r.error_msg, sizeof(r.error_msg), "curl: %s", curl_easy_strerror(res));
        free(buf.data);
        return r;
    }

    if (http_code != 200) {
        char *emsg = json_get_str(buf.data, "message");
        if (!emsg) {
            char *eraw = json_get_raw(buf.data, "error");
            if (eraw) {
                emsg = json_get_str(eraw, "message");
                free(eraw);
            }
        }
        snprintf(r.error_msg, sizeof(r.error_msg), "HTTP %d: %.180s", (int)http_code,
                 emsg ? emsg : "");
        free(emsg);
        free(buf.data);
        return r;
    }

    /* Parse choices[0].message.content */
    char *choices = json_get_raw(buf.data, "choices");
    if (choices) {
        const char *first = array_first_elem(choices);
        if (first) {
            char *msg = json_get_raw(first, "message");
            if (msg) {
                char *content = json_get_str(msg, "content");
                if (content) {
                    char *s = content;
                    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
                        s++;
                    snprintf(r.snippet, sizeof(r.snippet), "%.126s", s);
                    free(content);
                }
                free(msg);
            }
        }
        free(choices);
    }

    /* Parse usage.completion_tokens */
    char *usage = json_get_raw(buf.data, "usage");
    if (usage) {
        r.actual_tokens = json_get_int(usage, "completion_tokens", 0);
        free(usage);
    }

    r.ok = (r.snippet[0] != '\0');
    free(buf.data);
    return r;
}

/* ── Terminal helpers ───────────────────────────────────────────────────── */

#define C_GREEN "\033[32m"
#define C_YELLOW "\033[33m"
#define C_RED "\033[31m"
#define C_CYAN "\033[36m"
#define C_BLUE "\033[34m"
#define C_BOLD "\033[1m"
#define C_DIM "\033[2m"
#define C_RST "\033[0m"

/* ── Report: inverted model-level view ──────────────────────────────────── */

/*
 * Normal map:  provider → [model list]
 * Inverted:    model → {provider, prompt_price, completion_price,
 *                       tier0_latency, tier1_latency, tier2_latency,
 *                       tier0_ok, tier1_ok, tier2_ok}
 *
 * We load today's eval results from SQLite and print sorted by
 * median latency across tiers (fastest first).
 */

typedef struct {
    char model_id[256];
    char provider[64];
    double prompt_price;
    double completion_price;
    double lat[NTIERS]; /* avg latency per tier, -1 if not run */
    int tok[NTIERS];    /* actual tokens returned */
    int ok_count;       /* how many tiers succeeded */
    int total_tiers;    /* how many tiers were attempted */
    int context_length; /* max context tokens from model metadata */
} report_row_t;

static int cmp_report_rows(const void *a, const void *b) {
    const report_row_t *ra = (const report_row_t *)a;
    const report_row_t *rb = (const report_row_t *)b;
    /* Sort by ok_count desc, then median latency asc */
    if (ra->ok_count != rb->ok_count)
        return rb->ok_count - ra->ok_count;
    double la = 0, lb = 0;
    int ca = 0, cb = 0;
    for (int i = 0; i < NTIERS; i++) {
        if (ra->lat[i] >= 0) {
            la += ra->lat[i];
            ca++;
        }
        if (rb->lat[i] >= 0) {
            lb += rb->lat[i];
            cb++;
        }
    }
    if (ca)
        la /= ca;
    if (cb)
        lb /= cb;
    if (la < lb)
        return -1;
    if (la > lb)
        return 1;
    return 0;
}

/* Match model ID or provider against a filter string.
 * Filter may be a plain substring OR a comma-separated list of substrings,
 * e.g. "grok-4.20-beta,kimi-k2.5,glm-4.7".
 * Any token that matches (case-sensitive substring) is a hit. */
static bool filter_matches(const char *filter, const char *id, const char *provider) {
    if (!filter)
        return true;
    /* Fast path: no comma → plain strstr */
    if (!strchr(filter, ',')) {
        return strstr(id, filter) || (provider && strstr(provider, filter));
    }
    /* Comma-separated list */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", filter);
    char *tok = strtok(buf, ",");
    while (tok) {
        while (*tok == ' ')
            tok++;
        if (*tok && (strstr(id, tok) || (provider && strstr(provider, tok))))
            return true;
        tok = strtok(NULL, ",");
    }
    return false;
}

static const char *REPORT_SQL = "SELECT e.model_id, m.provider, m.prompt_price, m.completion_price,"
                                "       e.tier, AVG(e.latency_ms), AVG(e.actual_tokens),"
                                "       SUM(e.ok), COUNT(*), COALESCE(m.context_length,0)"
                                " FROM evals e"
                                " LEFT JOIN models m ON m.id = e.model_id"
                                " WHERE e.run_date = ?"
                                " GROUP BY e.model_id, e.tier"
                                " ORDER BY e.model_id, e.tier;";

static int load_report_rows(const char *today, report_row_t *rows, int cap) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, REPORT_SQL, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, today, -1, SQLITE_TRANSIENT);
    int nrows = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *mid = (const char *)sqlite3_column_text(st, 0);
        const char *prov = (const char *)sqlite3_column_text(st, 1);
        double pp = sqlite3_column_double(st, 2);
        double cp = sqlite3_column_double(st, 3);
        int tier = sqlite3_column_int(st, 4);
        double avg_lat = sqlite3_column_double(st, 5);
        int avg_tok = (int)sqlite3_column_double(st, 6);
        int ok = sqlite3_column_int(st, 7);
        (void)sqlite3_column_int(st, 8);
        int ctx = sqlite3_column_int(st, 9);
        if (!mid)
            continue;
        report_row_t *row = NULL;
        for (int i = 0; i < nrows; i++) {
            if (strcmp(rows[i].model_id, mid) == 0) {
                row = &rows[i];
                break;
            }
        }
        if (!row) {
            if (nrows >= cap)
                continue;
            row = &rows[nrows++];
            memset(row, 0, sizeof(*row));
            snprintf(row->model_id, sizeof(row->model_id), "%s", mid);
            snprintf(row->provider, sizeof(row->provider), "%s", prov ? prov : "?");
            row->prompt_price = pp;
            row->completion_price = cp;
            row->context_length = ctx;
            for (int i = 0; i < NTIERS; i++)
                row->lat[i] = -1;
        }
        if (tier >= 0 && tier < NTIERS) {
            row->lat[tier] = avg_lat;
            row->tok[tier] = avg_tok;
            row->total_tiers++;
            if (ok > 0)
                row->ok_count++;
        }
    }
    sqlite3_finalize(st);
    return nrows;
}

static void print_report(const char *today, const char *filter) {
    static report_row_t rows[MAX_MODELS];
    int nrows = load_report_rows(today, rows, MAX_MODELS);
    /* Apply filter in-place */
    if (filter) {
        int w = 0;
        for (int i = 0; i < nrows; i++) {
            if (filter_matches(filter, rows[i].model_id, rows[i].provider))
                rows[w++] = rows[i];
        }
        nrows = w;
    }

    if (nrows == 0) {
        printf("  No eval results for %s yet.\n", today);
        return;
    }

    qsort(rows, (size_t)nrows, sizeof(report_row_t), cmp_report_rows);

    /* Header */
    printf(C_BOLD "\n  OpenRouter Eval Report — %s\n" C_RST, today);
    printf("  %d models with results%s\n\n", nrows, filter ? " (filtered)" : "");

    /* Column header */
    printf(C_BOLD "  %-42s %-14s  %8s %8s    T1     T2     T3\n" C_RST, "Model", "Provider",
           "$/1Mtok-in", "$/1Mtok-out");
    printf("  %s\n", "─────────────────────────────────────────────────────────────────"
                     "──────────────────────────────");

    /* Group by provider for context, but sort by performance (inverted view) */
    char last_provider[64] = "";
    for (int i = 0; i < nrows; i++) {
        report_row_t *r = &rows[i];

        /* Provider break line */
        if (strcmp(r->provider, last_provider) != 0) {
            printf(C_CYAN "  ▸ %s\n" C_RST, r->provider);
            snprintf(last_provider, sizeof(last_provider), "%s", r->provider);
        }

        /* Model ID — strip provider prefix for display */
        const char *display_id = r->model_id;
        const char *sl = strchr(r->model_id, '/');
        if (sl)
            display_id = sl + 1;

        /* Success indicator */
        const char *status_color = r->ok_count == r->total_tiers ? C_GREEN
                                   : r->ok_count > 0             ? C_YELLOW
                                                                 : C_RED;

        printf("    %s%-40s" C_RST " %-14s  %8.2f %8.2f   ", status_color, display_id, r->provider,
               r->prompt_price * 1e6, /* per 1M tokens */
               r->completion_price * 1e6);

        /* Latency per tier */
        for (int t = 0; t < NTIERS; t++) {
            if (r->lat[t] >= 0)
                printf(C_DIM "%5.0f" C_RST " ", r->lat[t]);
            else
                printf("    - ");
        }
        printf("\n");
    }

    /* Summary stats */
    int total_ok = 0, total_fail = 0;
    double total_lat = 0.0;
    int lat_cnt = 0;
    for (int i = 0; i < nrows; i++) {
        total_ok += rows[i].ok_count;
        total_fail += rows[i].total_tiers - rows[i].ok_count;
        for (int t = 0; t < NTIERS; t++) {
            if (rows[i].lat[t] >= 0) {
                total_lat += rows[i].lat[t];
                lat_cnt++;
            }
        }
    }
    printf("  %s\n", "─────────────────────────────────────────────────────────────────"
                     "──────────────────────────────");
    printf("  " C_BOLD "Total: " C_RST C_GREEN "%d passed" C_RST ", " C_RED "%d failed" C_RST
           " — avg latency %.0fms\n\n",
           total_ok, total_fail, lat_cnt ? total_lat / lat_cnt : 0.0);
}

/* ── Select: model recommendation engine ───────────────────────────────── */

/* ── Advanced select: weighted multi-dimensional scoring ────────────────── */

/*
 * Scoring dimensions (5 total):
 *   [0] T0 latency   — cold start / TTFT
 *   [1] T1 latency   — short generation
 *   [2] T2 latency   — sustained throughput
 *   [3] cost_in      — $/token input
 *   [4] cost_out     — $/token output
 *
 * Each dimension is percentile-ranked within the candidate pool (1.0 = best).
 * Final score = weighted sum; range [0, 100].
 */
#define SDIM 5

typedef struct {
    double w[SDIM]; /* weights, must sum to 1.0 */
    const char *name;
    const char *description;
} score_profile_t;

/* clang-format off */
static const score_profile_t PROFILES[] = {
/*            T0     T1     T2    cin   cout  name        description */
    { {0.70, 0.15, 0.05, 0.07, 0.03}, "fast",     "Lowest TTFT — interactive chat, tool calls, autocomplete"    },
    { {0.03, 0.07, 0.05, 0.70, 0.15}, "cheap",    "Lowest input cost — large-scale batch, classification"       },
    { {0.20, 0.20, 0.20, 0.25, 0.15}, "balanced", "Equal latency + cost weight — general-purpose default"       },
    { {0.03, 0.10, 0.60, 0.15, 0.12}, "smart",    "Best T2 throughput — long-form output, essays, summaries"    },
    { {0.03, 0.10, 0.60, 0.12, 0.15}, "coding",   "High T2 + output cost aware — code gen, review, refactor"   },
    { {0.55, 0.20, 0.10, 0.10, 0.05}, "chat",     "Ultra-low TTFT, cheap — conversational apps, assistants"    },
    { {0.05, 0.18, 0.35, 0.30, 0.12}, "rag",      "Mid-tier throughput + low cost — retrieval-augmented tasks" },
    { {0.02, 0.05, 0.58, 0.25, 0.10}, "batch",    "Max throughput at lowest total cost — offline pipelines"    },
};
/* clang-format on */
#define NPROFILES ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))

typedef struct {
    double w[SDIM];           /* active weights */
    const char *profile_name; /* "fast", "coding", etc. */
    double budget;            /* max $/1M input (0 = unlimited) */
    int min_ctx;              /* min context length (0 = any) */
    long volume_tok_day;      /* tokens/day for cost projection */
    const char *provider;     /* NULL = all */
    int top_n;                /* how many to show (default 5) */
} sel_params_t;

static const score_profile_t *profile_find(const char *name) {
    for (int i = 0; i < NPROFILES; i++)
        if (strcmp(PROFILES[i].name, name) == 0)
            return &PROFILES[i];
    return NULL;
}

/* Parse "T0=0.4,T1=0.2,T2=0.2,cin=0.1,cout=0.1" into weights */
static bool parse_weights(const char *s, double w[SDIM]) {
    static const char *keys[] = {"T0", "T1", "T2", "cin", "cout"};
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    char *tok = strtok(buf, ",");
    while (tok) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            tok = strtok(NULL, ",");
            continue;
        }
        *eq = '\0';
        double val = atof(eq + 1);
        for (int i = 0; i < SDIM; i++) {
            if (strcmp(tok, keys[i]) == 0) {
                w[i] = val;
                break;
            }
        }
        tok = strtok(NULL, ",");
    }
    /* Normalize so weights sum to 1 */
    double sum = 0;
    for (int i = 0; i < SDIM; i++)
        sum += w[i];
    if (sum <= 0)
        return false;
    for (int i = 0; i < SDIM; i++)
        w[i] /= sum;
    return true;
}

/* ASCII block bar: rank in [0,1] → "████░░░░" of length `width` */
static void bar_str(double rank, int width, char *out, size_t out_len) {
    int filled = (int)round(rank * width);
    if (filled > width)
        filled = width;
    int pos = 0;
    for (int i = 0; i < filled && pos + 4 < (int)out_len; i++) {
        /* UTF-8 FULL BLOCK █ = E2 96 88 */
        out[pos++] = '\xe2';
        out[pos++] = '\x96';
        out[pos++] = '\x88';
    }
    for (int i = filled; i < width && pos + 4 < (int)out_len; i++) {
        /* UTF-8 LIGHT SHADE ░ = E2 96 91 */
        out[pos++] = '\xe2';
        out[pos++] = '\x96';
        out[pos++] = '\x91';
    }
    out[pos] = '\0';
}

/* Format context length: 128000 → "128K", 1000000 → "1M" */
static void fmt_ctx(int ctx, char *out, size_t n) {
    if (ctx <= 0)
        snprintf(out, n, "  ?K");
    else if (ctx >= 1000000)
        snprintf(out, n, "%3.0fM", ctx / 1e6);
    else if (ctx >= 1000)
        snprintf(out, n, "%3.0fK", ctx / 1e3);
    else
        snprintf(out, n, "%4d", ctx);
}

static void print_recommendations(const char *today, const sel_params_t *p) {
    static report_row_t rows[MAX_MODELS];
    int nrows = load_report_rows(today, rows, MAX_MODELS);
    if (nrows == 0) {
        printf("  No eval data for %s — run evals first.\n", today);
        return;
    }

    /* Header */
    printf(C_BOLD "\n  ┌─ Model Recommendations: %s · %s", p->profile_name, today);
    if (p->budget > 0)
        printf(" · budget ≤$%.2f/1M", p->budget);
    if (p->min_ctx > 0)
        printf(" · ctx≥%dK", p->min_ctx / 1000);
    printf(" ─" C_RST "\n");

    /* Show profile description if named */
    const score_profile_t *prof = profile_find(p->profile_name);
    if (prof)
        printf("  %s│%s %s%s%s\n", C_DIM, C_RST, C_DIM, prof->description, C_RST);

    /* Show active weights */
    static const char *dim_names[] = {"T0", "T1", "T2", "cost_in", "cost_out"};
    printf("  %s│%s Weights:", C_DIM, C_RST);
    for (int d = 0; d < SDIM; d++) {
        if (p->w[d] >= 0.01)
            printf("  %s=%.0f%%", dim_names[d], p->w[d] * 100);
    }
    printf("\n\n");

    /* Collect fully-passing candidates */
    report_row_t *cands[MAX_MODELS];
    int ncands = 0;
    for (int i = 0; i < nrows; i++) {
        report_row_t *r = &rows[i];
        if (r->ok_count < NTIERS)
            continue;
        if (p->budget > 0 && r->prompt_price * 1e6 > p->budget)
            continue;
        if (p->min_ctx > 0 && r->context_length > 0 && r->context_length < p->min_ctx)
            continue;
        if (p->provider && r->provider[0] && !strstr(r->provider, p->provider))
            continue;
        cands[ncands++] = r;
    }

    if (ncands == 0) {
        printf("  No models passed all tiers with the given filters.\n\n");
        return;
    }

    /* ── Percentile ranking per dimension ── */
    /* raw[d][i] = raw value for candidate i on dimension d (lower=better) */
    double raw[SDIM][MAX_MODELS];
    for (int i = 0; i < ncands; i++) {
        report_row_t *r = cands[i];
        raw[0][i] = r->lat[0] >= 0 ? r->lat[0] : 1e9; /* T0 */
        raw[1][i] = r->lat[1] >= 0 ? r->lat[1] : 1e9; /* T1 */
        raw[2][i] = r->lat[2] >= 0 ? r->lat[2] : 1e9; /* T2 */
        raw[3][i] = r->prompt_price;                  /* cost_in */
        raw[4][i] = r->completion_price;              /* cost_out */
    }

    /* rank[d][i] ∈ [0,1]: 1.0 = best on this dimension (lowest raw value) */
    double rank[SDIM][MAX_MODELS];
    for (int d = 0; d < SDIM; d++) {
        /* count models with lower raw value → position in ascending order */
        for (int i = 0; i < ncands; i++) {
            int pos = 0;
            for (int j = 0; j < ncands; j++)
                if (raw[d][j] < raw[d][i])
                    pos++;
            /* rank: 0 = worst (highest raw), 1 = best (lowest raw) */
            rank[d][i] = ncands > 1 ? 1.0 - (double)pos / (double)(ncands - 1) : 1.0;
        }
    }

    /* composite score [0,100] */
    double scores[MAX_MODELS];
    double dim_scores[SDIM][MAX_MODELS]; /* for breakdown */
    for (int i = 0; i < ncands; i++) {
        double s = 0;
        for (int d = 0; d < SDIM; d++) {
            dim_scores[d][i] = rank[d][i];
            s += p->w[d] * rank[d][i];
        }
        scores[i] = s * 100.0;
    }

    /* Sort all candidates by score desc */
    for (int i = 0; i < ncands - 1; i++) {
        for (int j = i + 1; j < ncands; j++) {
            if (scores[j] > scores[i]) {
                report_row_t *tr = cands[i];
                cands[i] = cands[j];
                cands[j] = tr;
                double ts = scores[i];
                scores[i] = scores[j];
                scores[j] = ts;
                for (int d = 0; d < SDIM; d++) {
                    double td = dim_scores[d][i];
                    dim_scores[d][i] = dim_scores[d][j];
                    dim_scores[d][j] = td;
                }
            }
        }
    }

    int top = ncands < p->top_n ? ncands : p->top_n;
    const char *medals[] = {"🥇", "🥈", "🥉", " 4.", " 5.", " 6.", " 7.", " 8.", " 9.", "10."};

    for (int i = 0; i < top; i++) {
        report_row_t *r = cands[i];
        const char *disp = r->model_id;
        const char *sl = strchr(r->model_id, '/');
        if (sl)
            disp = sl + 1;

        char ctx_str[8];
        fmt_ctx(r->context_length, ctx_str, sizeof(ctx_str));

        /* Line 1: rank + model name + score pill */
        const char *medal = (i < 10) ? medals[i] : "   ";
        printf("  %s " C_BOLD C_GREEN "%-38s" C_RST "  %s%-10s%s"
               "  %sScore %s%.0f/100%s\n",
               medal, disp, C_DIM, r->provider, C_RST, C_DIM, C_RST C_CYAN, scores[i], C_RST);

        /* Line 2: pricing + context */
        printf("       %s$%.3f%s / %s$%.3f%s per 1M tok"
               "  ctx %s%s%s",
               C_GREEN, r->prompt_price * 1e6, C_RST, C_DIM, r->completion_price * 1e6, C_RST,
               C_CYAN, ctx_str, C_RST);

        /* T0/T1/T2 latencies inline */
        for (int t = 0; t < NTIERS; t++) {
            if (r->lat[t] >= 0)
                printf("  T%d=%s%.0fms%s", t, C_DIM, r->lat[t], C_RST);
        }
        printf("\n");

        /* Line 3: dimension score bars */
        printf("       ");
        static const char *bar_labels[] = {"T0", "T1", "T2", "CIN", "OUT"};
        for (int d = 0; d < SDIM; d++) {
            if (p->w[d] < 0.01)
                continue; /* skip near-zero weight dims */
            char bar[64];
            bar_str(dim_scores[d][i], 6, bar, sizeof(bar));
            printf("%s%s%s:%s%s%s  ", C_DIM, bar_labels[d], C_RST, C_CYAN, bar, C_RST);
        }
        printf("\n\n");
    }

    /* Footer */
    printf("  %s%d candidates · %d passed all tiers%s", C_DIM, ncands, ncands, C_RST);
    if (p->budget > 0 || p->min_ctx > 0 || p->provider)
        printf(" %s(filtered)%s", C_DIM, C_RST);
    printf("\n");

    /* Volume projection */
    if (p->volume_tok_day > 0) {
        printf("\n" C_BOLD "  Cost projection at %ldK tokens/day:\n" C_RST,
               p->volume_tok_day / 1000);
        printf("  %-38s  %8s/day  %8s/mo   %8s/yr\n", "Model", "$", "$", "$");
        printf("  %s\n", "──────────────────────────────────────────────────────────────");
        for (int i = 0; i < top; i++) {
            report_row_t *r = cands[i];
            const char *disp = r->model_id;
            const char *sl = strchr(r->model_id, '/');
            if (sl)
                disp = sl + 1;
            /* Assume 70% input, 30% output token split */
            double in_tok = p->volume_tok_day * 0.70;
            double out_tok = p->volume_tok_day * 0.30;
            double day = in_tok * r->prompt_price + out_tok * r->completion_price;
            double mo = day * 30;
            double yr = day * 365;
            printf("  %-38s  %8.2f     %8.2f    %8.0f\n", disp, day, mo, yr);
        }
        printf("\n");
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

static void today_str(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    strftime(buf, n, "%Y-%m-%d", tm);
}

static void usage(void) {
    puts("usage: openrouter_tester [options]");
    puts("");
    puts("Eval options:");
    puts("  --refresh              Force re-fetch model list");
    puts("  --report               Show today's full eval report");
    puts("  --filter <str>         Eval/show only models matching <str>");
    puts("  --db <path>            Override DB path (default ~/.dsco/openrouter_evals.db)");
    puts("");
    puts("Select options (requires prior eval run):");
    puts("  --select <task>        Recommend models for a task:");
    puts("                         fast | cheap | balanced | smart | coding | chat | rag | batch");
    puts("  --weight <spec>        Custom dimension weights, e.g. T0=0.5,T2=0.3,cin=0.2");
    puts("                         Dimensions: T0 T1 T2 cin cout  (auto-normalized to sum=1)");
    puts("  --budget <$/1M>        Max input price per 1M tokens (e.g. 2.0)");
    puts("  --min-ctx <tokens>     Min context length required (e.g. 32000)");
    puts("  --provider <name>      Only show models from this provider");
    puts("  --volume <tok/day>     Show cost projection at this daily token volume");
    puts("  --top <n>              Number of results to show (default 5)");
    puts("  --profiles             List all built-in task profiles");
}

int main(int argc, char **argv) {
    const char *api_key = getenv("OPENROUTER_API_KEY");
    if (!api_key || !*api_key) {
        fprintf(stderr, "error: OPENROUTER_API_KEY not set\n");
        return 1;
    }

    /* Parse args */
    bool force_refresh = false;
    bool report_only = false;
    bool list_profiles = false;
    const char *filter = NULL;
    const char *select_arg = NULL;
    const char *weight_arg = NULL;
    char db_path[512];
    const char *home = getenv("HOME");
    snprintf(db_path, sizeof(db_path), "%s/.dsco/openrouter_evals.db", home ? home : ".");

    sel_params_t sel = {
        .w = {0},
        .profile_name = "balanced",
        .budget = 0.0,
        .min_ctx = 0,
        .volume_tok_day = 0,
        .provider = NULL,
        .top_n = 5,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--refresh") == 0)
            force_refresh = true;
        else if (strcmp(argv[i], "--report") == 0)
            report_only = true;
        else if (strcmp(argv[i], "--profiles") == 0)
            list_profiles = true;
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            select_arg = argv[++i];
        else if (strcmp(argv[i], "--weight") == 0 && i + 1 < argc)
            weight_arg = argv[++i];
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)
            sel.budget = atof(argv[++i]);
        else if (strcmp(argv[i], "--min-ctx") == 0 && i + 1 < argc)
            sel.min_ctx = atoi(argv[++i]);
        else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
            sel.provider = argv[++i];
        else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc)
            sel.volume_tok_day = atol(argv[++i]);
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc)
            sel.top_n = atoi(argv[++i]);
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            filter = argv[++i];
        else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            snprintf(db_path, sizeof(db_path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
    }

    if (list_profiles) {
        printf(C_BOLD "\n  Built-in task profiles:\n\n" C_RST);
        printf("  %-12s  %-50s  Weights (T0 T1 T2 cin cout)\n", "Name", "Description");
        printf("  %s\n", "─────────────────────────────────────────────────────────────────────────"
                         "─────────────────────────");
        for (int i = 0; i < NPROFILES; i++) {
            const score_profile_t *pr = &PROFILES[i];
            printf("  " C_CYAN "%-12s" C_RST "  %-50s  ", pr->name, pr->description);
            for (int d = 0; d < SDIM; d++)
                printf("%.0f%% ", pr->w[d] * 100);
            printf("\n");
        }
        printf("\n");
        return 0;
    }

    char today[16];
    today_str(today, sizeof(today));

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!db_open(db_path)) {
        curl_global_cleanup();
        return 1;
    }
    if (!db_init()) {
        sqlite3_close(g_db);
        curl_global_cleanup();
        return 1;
    }

    /* ── Step 1: Fetch & store model list ── */
    or_model_t *models = calloc(MAX_MODELS, sizeof(or_model_t));
    if (!models) {
        fprintf(stderr, "OOM\n");
        return 1;
    }
    int total_models = 0;

    bool need_fetch = force_refresh || !db_models_fresh(today);
    if (need_fetch) {
        printf(C_BOLD "  Fetching model list from OpenRouter…" C_RST "\n");
        char *json = fetch_url(OR_MODELS_URL, api_key);
        if (!json) {
            fprintf(stderr, "error: failed to fetch model list\n");
            free(models);
            sqlite3_close(g_db);
            curl_global_cleanup();
            return 1;
        }

        total_models = parse_and_filter(json, models, MAX_MODELS, today);
        free(json);

        printf("  Found %d models (after age filter)", total_models);
        int included = 0;
        for (int i = 0; i < total_models; i++)
            if (models[i].include)
                included++;
        printf(", %d in middle 70%% by price\n\n", included);

        /* Upsert all into DB */
        for (int i = 0; i < total_models; i++)
            db_upsert_model(&models[i], today);

        /* Remove models no longer in live list */
        db_prune_stale(today);
    } else {
        printf(C_DIM "  Model list already fresh for %s (use --refresh to re-fetch)\n" C_RST,
               today);
        /* Load from DB so we can run evals */
        sqlite3_stmt *st = NULL;
        const char *sql =
            "SELECT id,name,provider,created,context_length,prompt_price,completion_price"
            " FROM models ORDER BY provider, id;";
        if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW && total_models < MAX_MODELS) {
                or_model_t *m = &models[total_models++];
                const char *id = (const char *)sqlite3_column_text(st, 0);
                if (!id)
                    continue;
                snprintf(m->id, sizeof(m->id), "%s", id);
                const char *name = (const char *)sqlite3_column_text(st, 1);
                const char *prov = (const char *)sqlite3_column_text(st, 2);
                snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
                snprintf(m->provider, sizeof(m->provider), "%s", prov ? prov : "");
                m->created = sqlite3_column_int64(st, 3);
                m->context_length = sqlite3_column_int(st, 4);
                m->prompt_price = sqlite3_column_double(st, 5);
                m->completion_price = sqlite3_column_double(st, 6);
                m->combined_price = m->prompt_price + m->completion_price;
                m->include = (m->combined_price > 0.0);
            }
            sqlite3_finalize(st);
        }

        /* Re-apply percentile filter */
        or_model_t *tmp = calloc((size_t)total_models, sizeof(or_model_t));
        if (tmp) {
            memcpy(tmp, models, (size_t)total_models * sizeof(or_model_t));
            qsort(tmp, (size_t)total_models, sizeof(or_model_t), cmp_price_asc);
            int lo = (int)floor(total_models * PRICE_PERCENTILE);
            int hi = (int)ceil(total_models * (1.0 - PRICE_PERCENTILE));
            /* Map back: mark include by position */
            for (int i = 0; i < total_models; i++)
                models[i].include = false;
            for (int i = lo; i < hi; i++) {
                for (int j = 0; j < total_models; j++) {
                    if (strcmp(models[j].id, tmp[i].id) == 0 && tmp[i].combined_price > 0) {
                        models[j].include = true;
                        break;
                    }
                }
            }
            free(tmp);
        }
    }

    if (report_only) {
        print_report(today, filter);
        free(models);
        sqlite3_close(g_db);
        curl_global_cleanup();
        return 0;
    }

    if (select_arg || weight_arg) {
        /* Resolve profile */
        const char *pname = select_arg ? select_arg : "balanced";
        const score_profile_t *prof = profile_find(pname);
        if (!prof && !weight_arg) {
            fprintf(stderr,
                    "error: unknown task profile '%s'\n"
                    "       use --profiles to list available profiles\n",
                    pname);
            free(models);
            sqlite3_close(g_db);
            curl_global_cleanup();
            return 1;
        }
        if (prof) {
            for (int d = 0; d < SDIM; d++)
                sel.w[d] = prof->w[d];
            sel.profile_name = prof->name;
        } else {
            /* Start with balanced as base if custom weights */
            const score_profile_t *base = profile_find("balanced");
            if (base)
                for (int d = 0; d < SDIM; d++)
                    sel.w[d] = base->w[d];
            sel.profile_name = "custom";
        }
        if (weight_arg) {
            if (!parse_weights(weight_arg, sel.w)) {
                fprintf(stderr,
                        "error: invalid --weight spec '%s'\n"
                        "       format: T0=0.4,T2=0.4,cin=0.1,cout=0.1\n",
                        weight_arg);
                free(models);
                sqlite3_close(g_db);
                curl_global_cleanup();
                return 1;
            }
            if (!select_arg)
                sel.profile_name = "custom";
        }
        print_recommendations(today, &sel);
        free(models);
        sqlite3_close(g_db);
        curl_global_cleanup();
        return 0;
    }

    /* ── Step 2: Run evals ── */
    int included = 0;
    for (int i = 0; i < total_models; i++) {
        bool fm = filter_matches(filter, models[i].id, models[i].provider);
        if ((models[i].include || fm) && (!filter || fm))
            included++;
    }

    printf(C_BOLD "  Running %d × %d tier evals…\n" C_RST, included, NTIERS);
    printf("  %-44s  %s\n", "Model", "T1      T2      T3");
    printf("  %s\n", "─────────────────────────────────────────────────────────────────────");

    int eval_pass = 0, eval_fail = 0;

    for (int i = 0; i < total_models; i++) {
        or_model_t *m = &models[i];
        /* --filter bypasses the price-percentile include gate so you can
           force-eval any specific model regardless of cost tier */
        bool filter_match = filter_matches(filter, m->id, m->provider);
        if (!m->include && !filter_match)
            continue;
        if (filter && !filter_match)
            continue;

        printf("  %-44s  ", m->id);
        fflush(stdout);

        for (int t = 0; t < NTIERS; t++) {
            if (db_already_ran(m->id, today, t)) {
                printf(C_DIM "  skip" C_RST "  ");
                fflush(stdout);
                continue;
            }

            eval_result_t r = run_eval(m->id, api_key, t, today);
            db_insert_eval(&r);

            if (r.ok) {
                eval_pass++;
                printf(C_GREEN "%5.0f" C_RST "ms ", r.latency_ms);
            } else {
                eval_fail++;
                printf(C_RED " FAIL" C_RST "   ");
            }
            fflush(stdout);
        }
        printf("\n");
    }

    printf("  %s\n", "─────────────────────────────────────────────────────────────────────");
    printf("  Evals: " C_GREEN "%d passed" C_RST ", " C_RED "%d failed" C_RST "\n\n", eval_pass,
           eval_fail);

    /* ── Step 3: Print inverted report ── */
    print_report(today, filter);

    free(models);
    sqlite3_close(g_db);
    curl_global_cleanup();
    return (eval_fail > 0) ? 1 : 0;
}
