/* usage.c — per-session cost receipts (Wave B Plan #12 observability).
 *
 * Durable, append-only usage ledger at ~/.dsco/usage/usage.jsonl
 * (DSCO_USAGE_DIR overrides the directory). One JSON line per receipt:
 *
 *   {"schema":"dsco.usage_receipt.v1","session_id":...,"timestamp_ms":...,
 *    "tokens_in":...,"tokens_out":...,"cache_read_tokens":...,
 *    "cache_write_tokens":...,"cost_usd":...,"provider_cost_usd":...,
 *    "locally_derived_usd":...,"turns":...,"model":...,"provider":...}
 *
 * The CLI is strictly read-only: it never creates the ledger, a Chronicle
 * session, a provider connection, or a network call. It parses the file,
 * renders recent receipts plus totals, and can filter by session id.
 */

#include "usage.h"
#include "json_util.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

bool usage_session_id_valid(const char *session_id) {
    if (!session_id || !session_id[0]) return false;
    size_t n = strlen(session_id);
    if (n >= sizeof(((usage_receipt_t *)0)->session_id)) return false;
    for (size_t i = 0; i < n; i++) {
        char c = session_id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool usage_ledger_dir(char *out, size_t cap) {
    const char *override = getenv("DSCO_USAGE_DIR");
    if (override && override[0] && override[0] != '/') {
        fprintf(stderr, "usage: DSCO_USAGE_DIR must be an absolute path\n");
        return false;
    }
    if (override && override[0]) {
        snprintf(out, cap, "%s", override);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    snprintf(out, cap, "%s/.dsco/usage", home);
    return true;
}

bool usage_ledger_path(char *out, size_t cap) {
    char dir[PATH_MAX];
    if (!usage_ledger_dir(dir, sizeof(dir))) return false;
    snprintf(out, cap, "%s/%s", dir, USAGE_LEDGER_FILENAME);
    return true;
}

/* mkdir -p equivalent for the usage directory: walks path components and
 * creates each missing prefix (EEXIST tolerated). A single mkdir(2) fails
 * with ENOENT when ~/.dsco itself does not exist yet. */
static bool usage_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return false;
    memcpy(tmp, path, len + 1);
    /* Preserve a leading '/' so the first component is not empty. */
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool usage_dir_ensure(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode);
    if (!usage_mkdir_p(dir)) return false;
    /* Tolerate a lost race between stat and mkdir. */
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static void usage_receipt_json(jbuf_t *b, const usage_receipt_t *r) {
    jbuf_init(b, 512);
    jbuf_append(b, "{\"schema\":");
    jbuf_append_json_str(b, USAGE_RECEIPT_SCHEMA);
    jbuf_append(b, ",\"session_id\":");
    jbuf_append_json_str(b, r->session_id[0] ? r->session_id : "unknown");
    jbuf_appendf(b, ",\"timestamp_ms\":%lld", r->timestamp_ms);
    jbuf_appendf(b, ",\"tokens_in\":%lld", r->tokens_in);
    jbuf_appendf(b, ",\"tokens_out\":%lld", r->tokens_out);
    jbuf_appendf(b, ",\"cache_read_tokens\":%lld", r->cache_read_tokens);
    jbuf_appendf(b, ",\"cache_write_tokens\":%lld", r->cache_write_tokens);
    jbuf_appendf(b, ",\"cost_usd\":%.8f", r->cost_usd);
    jbuf_appendf(b, ",\"provider_cost_usd\":%.8f", r->provider_cost_usd);
    jbuf_appendf(b, ",\"locally_derived_usd\":%.8f", r->locally_derived_usd);
    jbuf_appendf(b, ",\"turns\":%lld", r->turns);
    jbuf_append(b, ",\"model\":");
    jbuf_append_json_str(b, r->model[0] ? r->model : "unknown");
    jbuf_append(b, ",\"provider\":");
    jbuf_append_json_str(b, r->provider[0] ? r->provider : "unknown");
    jbuf_append(b, "}\n");
}

bool usage_receipt_record(const usage_receipt_t *receipt) {
    if (!receipt || !usage_session_id_valid(receipt->session_id)) return false;

    char dir[PATH_MAX], path[PATH_MAX + 32];
    if (!usage_ledger_dir(dir, sizeof(dir)) ||
        !usage_ledger_path(path, sizeof(path)))
        return false;
    if (!usage_dir_ensure(dir)) return false;

    jbuf_t b;
    usage_receipt_json(&b, receipt);

    /* O_APPEND keeps writes atomic at the record level even if multiple
     * processes record concurrently. The caller is responsible for not
     * recording a partial receipt. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        jbuf_free(&b);
        return false;
    }
    bool ok = true;
    size_t off = 0;
    while (off < b.len) {
        ssize_t w = write(fd, b.data + off, b.len - off);
        if (w < 0) {
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    jbuf_free(&b);
    return ok;
}

/* ── Read-only CLI ──────────────────────────────────────────────────── */

typedef struct {
    long long receipts;
    long long tokens_in;
    long long tokens_out;
    long long cache_read_tokens;
    long long cache_write_tokens;
    long long turns;
    double cost_usd;
    double provider_cost_usd;
    double locally_derived_usd;
} usage_totals_t;

static void usage_totals_add(usage_totals_t *t, const usage_receipt_t *r) {
    t->receipts++;
    t->tokens_in += r->tokens_in;
    t->tokens_out += r->tokens_out;
    t->cache_read_tokens += r->cache_read_tokens;
    t->cache_write_tokens += r->cache_write_tokens;
    t->turns += r->turns;
    t->cost_usd += r->cost_usd;
    t->provider_cost_usd += r->provider_cost_usd;
    t->locally_derived_usd += r->locally_derived_usd;
}

static void usage_format_time(long long ts_ms, char *out, size_t cap) {
    time_t secs = (time_t)(ts_ms / 1000);
    struct tm tmv;
    if (secs > 0 && localtime_r(&secs, &tmv) &&
        strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv))
        return;
    snprintf(out, cap, "epoch-ms=%lld", ts_ms);
}

static void usage_print_totals(const usage_totals_t *t) {
    long long cache_total = t->cache_read_tokens + t->cache_write_tokens;
    printf("\n%zu receipt(s) | tokens in %lld, out %lld, cache r/w %lld/%lld | "
           "turns %lld | est total $%.6f (provider $%.6f, local $%.6f)\n",
           (size_t)t->receipts, t->tokens_in, t->tokens_out,
           t->cache_read_tokens, t->cache_write_tokens, t->turns,
           t->cost_usd, t->provider_cost_usd, t->locally_derived_usd);
    if (cache_total == 0)
        printf("ledger: %s (no cache traffic recorded)\n", USAGE_LEDGER_FILENAME);
}

static void usage_print_row(const usage_receipt_t *r) {
    char when[64];
    usage_format_time(r->timestamp_ms, when, sizeof(when));
    printf("%-38s %s  in=%-9lld out=%-9lld cache=%-9lld turns=%-5lld "
           "$%.6f  %s/%s\n",
           r->session_id, when, r->tokens_in, r->tokens_out,
           r->cache_read_tokens, r->turns, r->cost_usd,
           r->provider[0] ? r->provider : "?",
           r->model[0] ? r->model : "?");
}

/* Parse one JSONL line into a receipt. Tolerates missing optional fields;
 * a line without a session_id is treated as corrupt. */
static bool usage_parse_line(const char *line, usage_receipt_t *r) {
    memset(r, 0, sizeof(*r));
    char *sid = json_get_str(line, "session_id");
    if (!sid || !sid[0]) {
        free(sid);
        return false;
    }
    snprintf(r->session_id, sizeof(r->session_id), "%s", sid);
    free(sid);

    r->timestamp_ms = json_get_i64(line, "timestamp_ms", 0);
    r->tokens_in = json_get_i64(line, "tokens_in", 0);
    r->tokens_out = json_get_i64(line, "tokens_out", 0);
    r->cache_read_tokens = json_get_i64(line, "cache_read_tokens", 0);
    r->cache_write_tokens = json_get_i64(line, "cache_write_tokens", 0);
    r->turns = json_get_i64(line, "turns", 0);
    r->cost_usd = json_get_double(line, "cost_usd", 0.0);
    r->provider_cost_usd = json_get_double(line, "provider_cost_usd", 0.0);
    r->locally_derived_usd = json_get_double(line, "locally_derived_usd", 0.0);

    char *m = json_get_str(line, "model");
    if (m) {
        snprintf(r->model, sizeof(r->model), "%s", m);
        free(m);
    }
    char *p = json_get_str(line, "provider");
    if (p) {
        snprintf(r->provider, sizeof(r->provider), "%s", p);
        free(p);
    }
    return true;
}

/* Newest-first list: read the whole ledger, keep the most recent
 * USAGE_MAX_ROWS receipts in a ring buffer, then print them in order.
 * Totals always cover every matching receipt, not just the shown window. */
static int usage_cli_list(const char *filter_id) {
    char path[PATH_MAX + 32];
    if (!usage_ledger_path(path, sizeof(path))) {
        fprintf(stderr, "usage: cannot resolve ledger path (set HOME?)\n");
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        /* No ledger is an empty result, not an error: no run has ever
         * recorded a receipt on this installation. */
        printf("no usage receipts recorded yet (expected %s)\n", path);
        return 0;
    }

    enum { USAGE_MAX_ROWS = 20 };
    usage_receipt_t ring[USAGE_MAX_ROWS];
    long long row_count = 0;
    usage_totals_t all = {0};
    long long corrupt = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t n;

    while ((n = getline(&line, &line_cap, f)) > 0) {
        /* Trim trailing newline / CR so trailing whitespace never breaks
         * JSON parsing. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        /* Skip blank lines silently (append-only logs may accrue them). */
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p) continue;

        usage_receipt_t r;
        if (!usage_parse_line(p, &r)) {
            corrupt++;
            continue;
        }
        if (filter_id && strcmp(r.session_id, filter_id) != 0) continue;

        usage_totals_add(&all, &r);
        ring[row_count % USAGE_MAX_ROWS] = r; /* overwrites oldest when full */
        row_count++;
    }
    bool read_failed = ferror(f) != 0;
    free(line);
    fclose(f);

    if (read_failed) {
        fprintf(stderr, "usage: I/O error while reading %s\n", path);
        return 1;
    }

    if (corrupt > 0)
        fprintf(stderr, "usage: skipped %lld corrupt line(s) in %s\n",
                corrupt, path);

    if (row_count == 0) {
        if (filter_id)
            printf("no receipts recorded for session %s (ledger %s)\n",
                   filter_id, path);
        else
            printf("ledger is empty (%s)\n", path);
        return 0;
    }

    if (filter_id)
        printf("usage receipts for session %s (ledger %s):\n", filter_id, path);
    else
        printf("recent usage receipts (newest first, ledger %s):\n", path);

    long long shown_rows = row_count < USAGE_MAX_ROWS ? row_count : USAGE_MAX_ROWS;
    for (long long i = 0; i < shown_rows; i++) {
        /* slot = (row_count - shown_rows + i) mod max walks the ring in
         * oldest-to-newest order both before and after it wraps. */
        long long slot = (row_count - shown_rows + i) % USAGE_MAX_ROWS;
        usage_print_row(&ring[slot]);
    }

    if (filter_id) {
        printf("\ntotal for %s: %lld receipt(s), tokens in %lld, out %lld, "
               "cache r/w %lld/%lld, turns %lld, est $%.6f "
               "(provider $%.6f, local $%.6f)\n",
               filter_id, all.receipts, all.tokens_in, all.tokens_out,
               all.cache_read_tokens, all.cache_write_tokens, all.turns,
               all.cost_usd, all.provider_cost_usd, all.locally_derived_usd);
    } else {
        if (row_count > shown_rows)
            printf("... %lld more receipt(s) not shown\n",
                   row_count - shown_rows);
        usage_print_totals(&all);
    }
    return 0;
}

static int usage_cli_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s usage [session-id]\n"
            "\n"
            "Per-session cost receipts (Wave B #12).\n"
            "  dsco usage                 list recent receipts plus totals\n"
            "  dsco usage <session-id>    receipts and totals for one session\n"
            "\n"
            "Ledger: %s (append-only JSONL, DSCO_USAGE_DIR overrides)\n",
            argv0, USAGE_LEDGER_FILENAME);
    return 2;
}

int usage_cli(int argc, char **argv) {
    /* argv is the normalized subcommand vector: argv[1] == "usage". */
    if (argc == 2)
        return usage_cli_list(NULL);
    if (argc == 3) {
        if (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)
            return usage_cli_usage(argv[0]);
        const char *id = argv[2];
        if (!usage_session_id_valid(id)) {
            fprintf(stderr,
                    "usage: invalid session id '%s' "
                    "(allowed: [A-Za-z0-9_-], max 127 chars)\n",
                    id ? id : "");
            return 2;
        }
        return usage_cli_list(id);
    }
    return usage_cli_usage(argv[0]);
}
