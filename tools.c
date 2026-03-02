#include "tools.h"
#include "json_util.h"
#include "config.h"
#include "ast.h"
#include "swarm.h"
#include "ipc.h"
#include "tui.h"
#include "baseline.h"
#include "crypto.h"
#include "pipeline.h"
#include "eval.h"
#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

/* ── Global swarm instance (shared across tool calls) ─────────────────── */
static swarm_t g_swarm = {0};
static bool    g_swarm_inited = false;

static double now_sec_helper(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void ensure_swarm(void) {
    if (!g_swarm_inited) {
        const char *key = getenv("ANTHROPIC_API_KEY");
        const char *model = getenv("DSCO_MODEL");
        if (!model) model = DEFAULT_MODEL;
        swarm_init(&g_swarm, key, model);
        g_swarm_inited = true;
    }
}

/* ── Streaming subprocess runner ────────────────────────────────────────
 *
 * Replaces blocking popen+fgets with fork/exec/pipe/poll so that:
 *   1) Output streams to stderr in real-time as the command runs
 *   2) Full output is accumulated in the result buffer for the LLM
 *   3) An idle-timeout fires when the process stalls (no output)
 *   4) A wall-clock timeout is the hard upper bound
 *
 * The stream_opts control how output is presented.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    int   wall_timeout_s;     /* hard wall-clock limit (0 = 120s default)    */
    int   idle_timeout_s;     /* kill after N seconds with no output (0=30)  */
    bool  stream_to_tty;      /* echo deltas to stderr in real time          */
    bool  dim_output;         /* wrap streamed output in TUI_DIM             */
    const char *label;        /* label for event logging (tool name)         */
} run_opts_t;

static const run_opts_t RUN_OPTS_DEFAULT = {
    .wall_timeout_s = 120,
    .idle_timeout_s = 30,
    .stream_to_tty  = true,
    .dim_output     = true,
    .label          = NULL,
};

/* Low-level streaming runner. Returns exit status (or -1 on error). */
static int run_cmd_ex(const char *cmd, char *out, size_t out_len,
                      const run_opts_t *opts) {
    if (!opts) opts = &RUN_OPTS_DEFAULT;

    int wall_timeout = opts->wall_timeout_s > 0 ? opts->wall_timeout_s : 120;
    int idle_timeout = opts->idle_timeout_s > 0 ? opts->idle_timeout_s : 30;

    /* Pipe for child stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(out, out_len, "pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(out, out_len, "fork failed: %s", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* ── Child ── */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* New process group so we can kill the whole tree */
        setpgid(0, 0);

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* ── Parent ── */
    close(pipefd[1]);

    /* Guard against zero-length output buffer */
    if (out_len == 0) {
        close(pipefd[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    size_t total = 0;
    out[0] = '\0';

    struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
    struct timeval tv_start, tv_now, tv_last_data;
    gettimeofday(&tv_start, NULL);
    gettimeofday(&tv_last_data, NULL);

    bool timed_out = false;
    bool idle_killed = false;
    bool first_chunk = true;

    while (1) {
        int poll_ms = 200;  /* check every 200ms */
        int ready = poll(&pfd, 1, poll_ms);

        gettimeofday(&tv_now, NULL);
        double elapsed = (tv_now.tv_sec - tv_start.tv_sec)
                       + (tv_now.tv_usec - tv_start.tv_usec) / 1e6;
        double since_data = (tv_now.tv_sec - tv_last_data.tv_sec)
                          + (tv_now.tv_usec - tv_last_data.tv_usec) / 1e6;

        /* Wall-clock timeout */
        if (elapsed > wall_timeout) {
            timed_out = true;
            kill(-pid, SIGTERM);  /* kill process group */
            usleep(100000);
            kill(-pid, SIGKILL);
            break;
        }

        /* Idle timeout — no output for too long */
        if (since_data > idle_timeout && total > 0) {
            idle_killed = true;
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            break;
        }

        if (ready > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n <= 0) break;  /* EOF or error */

            buf[n] = '\0';
            gettimeofday(&tv_last_data, NULL);

            /* Stream delta to terminal */
            if (opts->stream_to_tty) {
                if (first_chunk && opts->dim_output) {
                    fprintf(stderr, "%s", TUI_DIM);
                    first_chunk = false;
                }
                fwrite(buf, 1, (size_t)n, stderr);
                fflush(stderr);
            }

            /* Accumulate in result buffer */
            if (total + (size_t)n < out_len - 1) {
                memcpy(out + total, buf, (size_t)n);
                total += (size_t)n;
            } else if (total < out_len - 1) {
                size_t room = out_len - 1 - total;
                memcpy(out + total, buf, room);
                total += room;
            }
        } else if (ready > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            /* Drain remaining data */
            char buf[4096];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                if (opts->stream_to_tty) {
                    if (first_chunk && opts->dim_output) {
                        fprintf(stderr, "%s", TUI_DIM);
                        first_chunk = false;
                    }
                    fwrite(buf, 1, (size_t)n, stderr);
                    fflush(stderr);
                }
                if (total + (size_t)n < out_len - 1) {
                    memcpy(out + total, buf, (size_t)n);
                    total += (size_t)n;
                }
            }
            break;
        }
    }

    out[total] = '\0';
    close(pipefd[0]);

    /* Reset terminal style if we were streaming dim output */
    if (opts->stream_to_tty && !first_chunk && opts->dim_output) {
        fprintf(stderr, "%s", TUI_RESET);
    }

    /* Reap child */
    int status = 0;
    int wpid = waitpid(pid, &status, WNOHANG);
    if (wpid == 0) {
        /* Still running — give it a moment */
        usleep(100000);
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == 0) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }

    /* Append status info for non-zero exits */
    if (timed_out) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_len - cur, "\n[killed: wall timeout %ds]", wall_timeout);
        return 124;
    }
    if (idle_killed) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_len - cur, "\n[killed: idle timeout %ds]", idle_timeout);
        return 124;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

/* Simple wrapper for tools that don't need streaming (internal helpers) */
static int run_cmd(const char *cmd, char *out, size_t out_len) {
    run_opts_t opts = RUN_OPTS_DEFAULT;
    opts.stream_to_tty = false;  /* quiet for internal helpers */
    return run_cmd_ex(cmd, out, out_len, &opts);
}

/* Safe shell quoting helper — wraps a string in single quotes */
static void shell_quote(jbuf_t *b, const char *s) {
    jbuf_append_char(b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            jbuf_append(b, "'\\''");
        } else {
            jbuf_append_char(b, *p);
        }
    }
    jbuf_append_char(b, '\'');
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RETRIEVAL CONTEXT STORE (for large tool outputs)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CTX_EMBED_DIM             256
#define CTX_EMBED_WORDS           (CTX_EMBED_DIM / 64)
#define CTX_MAX_CHUNKS            480
#define CTX_MAX_TOTAL_BYTES       (6 * 1024 * 1024)
#define CTX_CHUNK_TARGET_CHARS    1200
#define CTX_CHUNK_MIN_CHARS       350
#define CTX_CHUNK_OVERLAP_CHARS   180
#define CTX_SEARCH_DEFAULT_K      5
#define CTX_SEARCH_MAX_K          12
#define CTX_SEARCH_CANDIDATES     40
#define CTX_RESULT_OFFLOAD_BYTES  4096

typedef struct {
    int id;
    char tool[48];
    char *text;
    size_t text_len;
    bool pinned;
    uint64_t hash;
    float embed[CTX_EMBED_DIM];
    float embed_norm;
    uint64_t bucket_bits[CTX_EMBED_WORDS];
    time_t created_at;
} ctx_chunk_t;

typedef struct {
    ctx_chunk_t chunks[CTX_MAX_CHUNKS];
    int count;
    int next_id;
    size_t total_bytes;
    uint32_t bucket_df[CTX_EMBED_DIM];
} ctx_store_t;

typedef struct {
    int chunks_added;
    int first_chunk_id;
    int last_chunk_id;
    size_t bytes_added;
} ctx_ingest_info_t;

typedef struct {
    int idx;
    float dense_score;
    float lexical_score;
    float recency_score;
    float final_score;
} ctx_hit_t;

static ctx_store_t g_ctx = {0};
static size_t g_ctx_offload_events = 0;
static size_t g_ctx_offloaded_bytes = 0;
static size_t g_ctx_reference_bytes = 0;

static uint64_t ctx_hash_bytes(const char *s, size_t len) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint32_t ctx_hash_token(const char *s, int len) {
    uint32_t h = 2166136261u; /* FNV-1a 32 */
    for (int i = 0; i < len; i++) {
        h ^= (uint32_t)(unsigned char)tolower((unsigned char)s[i]);
        h *= 16777619u;
    }
    return h;
}

static bool ctx_is_token_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' ||
           c == ':' || c == '#' || c == '@';
}

static void ctx_build_embedding(const char *text,
                                float vec[CTX_EMBED_DIM],
                                uint64_t bits[CTX_EMBED_WORDS],
                                float *norm_out) {
    memset(vec, 0, sizeof(float) * CTX_EMBED_DIM);
    memset(bits, 0, sizeof(uint64_t) * CTX_EMBED_WORDS);

    if (!text || !*text) {
        *norm_out = 0.0f;
        return;
    }

    char tok[64];
    int tlen = 0;
    const char *p = text;
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (ctx_is_token_char(c)) {
            if (tlen < (int)sizeof(tok) - 1) {
                tok[tlen++] = (char)tolower(c);
            }
        } else {
            if (tlen >= 2) {
                uint32_t h = ctx_hash_token(tok, tlen);
                int b = (int)(h % CTX_EMBED_DIM);
                vec[b] += 1.0f;
                bits[b / 64] |= (1ULL << (b % 64));
            }
            tlen = 0;
            if (c == '\0') break;
        }
        p++;
    }

    float n2 = 0.0f;
    for (int i = 0; i < CTX_EMBED_DIM; i++) {
        if (vec[i] > 0.0f) {
            vec[i] = 1.0f + logf(vec[i]); /* damp frequent buckets */
            n2 += vec[i] * vec[i];
        }
    }
    *norm_out = (n2 > 0.0f) ? sqrtf(n2) : 0.0f;
}

static float ctx_cosine(const float a[CTX_EMBED_DIM], float anorm,
                        const float b[CTX_EMBED_DIM], float bnorm) {
    if (anorm <= 1e-8f || bnorm <= 1e-8f) return 0.0f;
    float dot = 0.0f;
    for (int i = 0; i < CTX_EMBED_DIM; i++) {
        dot += a[i] * b[i];
    }
    return dot / (anorm * bnorm);
}

static void ctx_preview(const char *text, size_t max_chars, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!text || !*text) return;

    size_t j = 0;
    bool prev_space = false;
    for (size_t i = 0; text[i] && j + 2 < out_len && j < max_chars; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space) out[j++] = ' ';
            prev_space = true;
            continue;
        }
        if (c < 32 && c != '\0') continue;
        out[j++] = (char)c;
        prev_space = false;
    }
    if (text[strlen(text) > max_chars ? max_chars : strlen(text)] != '\0' &&
        j + 4 < out_len) {
        out[j++] = '.';
        out[j++] = '.';
        out[j++] = '.';
    }
    out[j] = '\0';
}

static void ctx_recompute_df(void) {
    memset(g_ctx.bucket_df, 0, sizeof(g_ctx.bucket_df));
    for (int i = 0; i < g_ctx.count; i++) {
        for (int b = 0; b < CTX_EMBED_DIM; b++) {
            if (g_ctx.chunks[i].bucket_bits[b / 64] & (1ULL << (b % 64))) {
                g_ctx.bucket_df[b]++;
            }
        }
    }
}

static void ctx_evict_oldest(void) {
    if (g_ctx.count <= 0) return;
    if (g_ctx.chunks[0].text) {
        if (g_ctx.total_bytes >= g_ctx.chunks[0].text_len + 1) {
            g_ctx.total_bytes -= g_ctx.chunks[0].text_len + 1;
        } else {
            g_ctx.total_bytes = 0;
        }
        free(g_ctx.chunks[0].text);
    }
    if (g_ctx.count > 1) {
        memmove(&g_ctx.chunks[0], &g_ctx.chunks[1],
                (size_t)(g_ctx.count - 1) * sizeof(ctx_chunk_t));
    }
    g_ctx.count--;
}

static void ctx_evict_index(int idx) {
    if (idx < 0 || idx >= g_ctx.count) return;
    if (g_ctx.chunks[idx].text) {
        if (g_ctx.total_bytes >= g_ctx.chunks[idx].text_len + 1) {
            g_ctx.total_bytes -= g_ctx.chunks[idx].text_len + 1;
        } else {
            g_ctx.total_bytes = 0;
        }
        free(g_ctx.chunks[idx].text);
    }
    if (idx < g_ctx.count - 1) {
        memmove(&g_ctx.chunks[idx], &g_ctx.chunks[idx + 1],
                (size_t)(g_ctx.count - idx - 1) * sizeof(ctx_chunk_t));
    }
    g_ctx.count--;
}

static void ctx_store_reset(void) {
    for (int i = 0; i < g_ctx.count; i++) {
        free(g_ctx.chunks[i].text);
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.next_id = 1;
    g_ctx_offload_events = 0;
    g_ctx_offloaded_bytes = 0;
    g_ctx_reference_bytes = 0;
}

static bool ctx_chunk_exists(uint64_t hash, const char *text, size_t len) {
    for (int i = 0; i < g_ctx.count; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[i];
        if (c->hash != hash || c->text_len != len) continue;
        if (memcmp(c->text, text, len) == 0) return true;
    }
    return false;
}

static bool ctx_store_chunk(const char *tool, const char *text, size_t len,
                            ctx_ingest_info_t *info) {
    while (len > 0 && isspace((unsigned char)*text)) {
        text++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        len--;
    }
    if (len < 2) return false;

    uint64_t h = ctx_hash_bytes(text, len);
    if (ctx_chunk_exists(h, text, len)) return false;

    while (g_ctx.count >= CTX_MAX_CHUNKS ||
           g_ctx.total_bytes + len + 1 > CTX_MAX_TOTAL_BYTES) {
        if (g_ctx.count <= 0) break;
        ctx_evict_oldest();
    }
    if (g_ctx.count >= CTX_MAX_CHUNKS ||
        g_ctx.total_bytes + len + 1 > CTX_MAX_TOTAL_BYTES) {
        return false;
    }

    ctx_chunk_t *c = &g_ctx.chunks[g_ctx.count++];
    memset(c, 0, sizeof(*c));
    c->id = g_ctx.next_id++;
    c->hash = h;
    c->created_at = time(NULL);
    strncpy(c->tool, tool ? tool : "unknown", sizeof(c->tool) - 1);
    c->text = malloc(len + 1);
    if (!c->text) {
        g_ctx.count--;
        return false;
    }
    memcpy(c->text, text, len);
    c->text[len] = '\0';
    c->text_len = len;
    ctx_build_embedding(c->text, c->embed, c->bucket_bits, &c->embed_norm);
    g_ctx.total_bytes += len + 1;

    if (info) {
        if (info->chunks_added == 0) info->first_chunk_id = c->id;
        info->last_chunk_id = c->id;
        info->chunks_added++;
        info->bytes_added += len;
    }
    return true;
}

static void ctx_ingest_text(const char *tool, const char *text, ctx_ingest_info_t *info) {
    if (info) memset(info, 0, sizeof(*info));
    if (!text || !*text) return;

    size_t n = strlen(text);
    size_t pos = 0;
    while (pos < n) {
        size_t end = pos + CTX_CHUNK_TARGET_CHARS;
        if (end > n) end = n;

        size_t split = end;
        if (end < n) {
            size_t min_split = pos + CTX_CHUNK_MIN_CHARS;
            if (min_split > end) min_split = end;

            size_t best = 0;
            for (size_t i = end; i > min_split; i--) {
                if (i < n && text[i - 1] == '\n' && text[i] == '\n') {
                    best = i + 1;
                    break;
                }
            }
            if (!best) {
                for (size_t i = end; i > min_split; i--) {
                    if (text[i - 1] == '\n') {
                        best = i;
                        break;
                    }
                }
            }
            if (!best) {
                for (size_t i = end; i > min_split; i--) {
                    if (text[i - 1] == ' ') {
                        best = i;
                        break;
                    }
                }
            }
            if (best > pos) split = best;
        }
        if (split <= pos) split = (end > pos) ? end : (pos + 1);

        size_t len = split - pos;
        ctx_store_chunk(tool, text + pos, len, info);
        if (split >= n) break;

        size_t next_pos = (split > CTX_CHUNK_OVERLAP_CHARS)
                        ? (split - CTX_CHUNK_OVERLAP_CHARS)
                        : split;
        if (next_pos <= pos) next_pos = split;
        pos = next_pos;
    }

    ctx_recompute_df();
}

static int ctx_find_index_by_id(int chunk_id) {
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.chunks[i].id == chunk_id) return i;
    }
    return -1;
}

static float ctx_lexical_overlap(const uint64_t query_bits[CTX_EMBED_WORDS],
                                 const ctx_chunk_t *chunk) {
    if (!chunk || g_ctx.count <= 0) return 0.0f;
    float num = 0.0f, den = 0.0f;
    int N = g_ctx.count;
    for (int b = 0; b < CTX_EMBED_DIM; b++) {
        if (!(query_bits[b / 64] & (1ULL << (b % 64)))) continue;
        float idf = logf(((float)N + 1.0f) / ((float)g_ctx.bucket_df[b] + 1.0f)) + 1.0f;
        den += idf;
        if (chunk->bucket_bits[b / 64] & (1ULL << (b % 64))) {
            num += idf;
        }
    }
    return (den > 1e-6f) ? (num / den) : 0.0f;
}

static int ctx_extract_terms(const char *text, char terms[][32], int max_terms) {
    if (!text || !*text || max_terms <= 0) return 0;
    int count = 0;
    char tok[32];
    int tlen = 0;
    for (const char *p = text;; p++) {
        unsigned char c = (unsigned char)*p;
        if (ctx_is_token_char(c)) {
            if (tlen < (int)sizeof(tok) - 1) {
                tok[tlen++] = (char)tolower(c);
            }
        } else {
            if (tlen >= 2) {
                tok[tlen] = '\0';
                bool exists = false;
                for (int i = 0; i < count; i++) {
                    if (strcmp(terms[i], tok) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    strncpy(terms[count], tok, 31);
                    terms[count][31] = '\0';
                    count++;
                    if (count >= max_terms) break;
                }
            }
            tlen = 0;
            if (c == '\0') break;
        }
    }
    return count;
}

static bool ctx_chunk_contains_term(const char *text, const char *term) {
    if (!text || !term || !*term) return false;
    size_t tlen = strlen(term);
    if (tlen < 2) return false;

    char tok[64];
    int k = 0;
    for (const char *p = text;; p++) {
        unsigned char c = (unsigned char)*p;
        if (ctx_is_token_char(c)) {
            if (k < (int)sizeof(tok) - 1) {
                tok[k++] = (char)tolower(c);
            }
        } else {
            if (k > 0) {
                tok[k] = '\0';
                if (strcmp(tok, term) == 0) return true;
            }
            k = 0;
            if (c == '\0') break;
        }
    }
    return false;
}

static float ctx_exact_overlap_score(const char *query, const char *chunk_text) {
    char terms[24][32];
    int tcount = ctx_extract_terms(query, terms, 24);
    if (tcount <= 0) return 0.0f;

    float hit = 0.0f;
    for (int i = 0; i < tcount; i++) {
        if (ctx_chunk_contains_term(chunk_text, terms[i])) hit += 1.0f;
    }
    return hit / (float)tcount;
}

static int ctx_cmp_dense_desc(const void *a, const void *b) {
    const ctx_hit_t *x = (const ctx_hit_t *)a;
    const ctx_hit_t *y = (const ctx_hit_t *)b;
    if (y->dense_score > x->dense_score) return 1;
    if (y->dense_score < x->dense_score) return -1;
    return 0;
}

static int ctx_cmp_final_desc(const void *a, const void *b) {
    const ctx_hit_t *x = (const ctx_hit_t *)a;
    const ctx_hit_t *y = (const ctx_hit_t *)b;
    if (y->final_score > x->final_score) return 1;
    if (y->final_score < x->final_score) return -1;
    return 0;
}

static bool ctx_is_internal_tool(const char *name) {
    return name &&
           (strcmp(name, "context_search") == 0 ||
            strcmp(name, "context_get") == 0 ||
            strcmp(name, "context_stats") == 0);
}

static int ctx_offload_threshold_bytes(void) {
    const char *env = getenv("DSCO_CONTEXT_OFFLOAD_BYTES");
    int n = env ? atoi(env) : CTX_RESULT_OFFLOAD_BYTES;
    if (n < 1024) n = 1024;
    if (n > 32768) n = 32768;
    return n;
}

static bool ctx_should_offload_result(const char *tool_name, const char *result, bool ok) {
    if (!ok || !tool_name || !result) return false;
    if (ctx_is_internal_tool(tool_name)) return false;
    if (strncmp(result, "error:", 6) == 0) return false;
    return strlen(result) >= (size_t)ctx_offload_threshold_bytes();
}

static void ctx_rewrite_result_as_reference(const char *tool_name,
                                            const char *original_result,
                                            char *result,
                                            size_t result_len,
                                            const ctx_ingest_info_t *info) {
    char preview[340];
    ctx_preview(original_result, 260, preview, sizeof(preview));
    snprintf(result, result_len,
             "large tool output offloaded for retrieval\n"
             "tool: %s\n"
             "stored_chunks: %d\n"
             "chunk_id_range: %d-%d\n"
             "bytes_indexed: %zu\n"
             "preview: %s\n"
             "next: use context_search with a focused query, then context_get with chunk_id",
             tool_name ? tool_name : "unknown",
             info ? info->chunks_added : 0,
             info ? info->first_chunk_id : -1,
             info ? info->last_chunk_id : -1,
             info ? info->bytes_added : 0,
             preview[0] ? preview : "(empty)");
}

static void ctx_maybe_offload_tool_result(const char *tool_name,
                                          const char *input_json,
                                          bool ok,
                                          char *result,
                                          size_t result_len) {
    (void)input_json;
    if (!ctx_should_offload_result(tool_name, result, ok)) return;

    char *copy = strdup(result);
    if (!copy) return;
    size_t old_len = strlen(copy);

    ctx_ingest_info_t info;
    ctx_ingest_text(tool_name, copy, &info);
    if (info.chunks_added > 0) {
        ctx_rewrite_result_as_reference(tool_name, copy, result, result_len, &info);
        g_ctx_offload_events++;
        g_ctx_offloaded_bytes += old_len;
        g_ctx_reference_bytes += strlen(result);
    }
    free(copy);
}

static int ctx_rank_hits(const char *query,
                         const char *tool_filter,
                         int top_k,
                         ctx_hit_t *out_hits,
                         int max_hits) {
    if (!query || !*query || g_ctx.count <= 0 || max_hits <= 0) return 0;
    if (top_k < 1) top_k = 1;
    if (top_k > CTX_SEARCH_MAX_K) top_k = CTX_SEARCH_MAX_K;

    float qvec[CTX_EMBED_DIM];
    uint64_t qbits[CTX_EMBED_WORDS];
    float qnorm = 0.0f;
    ctx_build_embedding(query, qvec, qbits, &qnorm);

    ctx_hit_t hits[CTX_MAX_CHUNKS];
    int hit_count = 0;
    int scanned = 0;
    for (int i = 0; i < g_ctx.count && hit_count < CTX_MAX_CHUNKS; i++) {
        if (tool_filter && *tool_filter &&
            strcmp(g_ctx.chunks[i].tool, tool_filter) != 0) {
            continue;
        }
        scanned++;
        float dense = ctx_cosine(qvec, qnorm, g_ctx.chunks[i].embed, g_ctx.chunks[i].embed_norm);
        if (dense <= 0.0001f && qnorm > 0.0f) continue;
        hits[hit_count].idx = i;
        hits[hit_count].dense_score = dense;
        hits[hit_count].lexical_score = 0.0f;
        hits[hit_count].recency_score = 0.0f;
        hits[hit_count].final_score = dense;
        hit_count++;
    }

    if (scanned == 0) return 0;

    if (hit_count == 0) {
        int start = g_ctx.count > CTX_SEARCH_CANDIDATES ? g_ctx.count - CTX_SEARCH_CANDIDATES : 0;
        for (int i = start; i < g_ctx.count && hit_count < CTX_MAX_CHUNKS; i++) {
            if (tool_filter && *tool_filter &&
                strcmp(g_ctx.chunks[i].tool, tool_filter) != 0) {
                continue;
            }
            hits[hit_count].idx = i;
            hits[hit_count].dense_score = 0.0f;
            hits[hit_count].lexical_score = 0.0f;
            hits[hit_count].recency_score = 0.0f;
            hits[hit_count].final_score = 0.0f;
            hit_count++;
        }
    }

    qsort(hits, (size_t)hit_count, sizeof(ctx_hit_t), ctx_cmp_dense_desc);
    int rerank_n = hit_count;
    if (rerank_n > CTX_SEARCH_CANDIDATES) rerank_n = CTX_SEARCH_CANDIDATES;
    if (rerank_n < top_k) rerank_n = top_k < hit_count ? top_k : hit_count;

    for (int i = 0; i < rerank_n; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[hits[i].idx];
        float bucket_lex = ctx_lexical_overlap(qbits, c);
        float exact_lex = ctx_exact_overlap_score(query, c->text);
        hits[i].lexical_score = 0.4f * bucket_lex + 0.6f * exact_lex;
        hits[i].recency_score = (float)(hits[i].idx + 1) / (float)(g_ctx.count + 1);
        hits[i].final_score =
            0.55f * hits[i].dense_score +
            0.35f * hits[i].lexical_score +
            0.10f * hits[i].recency_score;
    }
    qsort(hits, (size_t)rerank_n, sizeof(ctx_hit_t), ctx_cmp_final_desc);

    int emit = top_k < rerank_n ? top_k : rerank_n;
    if (emit > max_hits) emit = max_hits;
    memcpy(out_hits, hits, (size_t)emit * sizeof(ctx_hit_t));
    return emit;
}

static bool ctx_search_render(const char *query,
                              const char *tool_filter,
                              int top_k,
                              char *result,
                              size_t rlen) {
    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        return false;
    }
    if (g_ctx.count == 0) {
        snprintf(result, rlen, "context store is empty");
        return true;
    }

    int scanned = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (tool_filter && *tool_filter &&
            strcmp(g_ctx.chunks[i].tool, tool_filter) != 0) {
            continue;
        }
        scanned++;
    }

    if (scanned == 0) {
        snprintf(result, rlen, "no chunks match tool filter: %s", tool_filter ? tool_filter : "");
        return true;
    }

    ctx_hit_t hits[CTX_SEARCH_MAX_K];
    int emit = ctx_rank_hits(query, tool_filter, top_k, hits, CTX_SEARCH_MAX_K);
    if (emit <= 0) {
        snprintf(result, rlen, "no retrieval hits for query: %s", query);
        return true;
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "context search query=%s\n"
                     "chunks=%d total_bytes=%zu reranked=%d tool_filter=%s\n\n",
                     query, g_ctx.count, g_ctx.total_bytes, emit,
                     (tool_filter && *tool_filter) ? tool_filter : "*");
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        return true;
    }
    off += (size_t)n;

    for (int i = 0; i < emit; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[hits[i].idx];
        char preview[300];
        ctx_preview(c->text, 220, preview, sizeof(preview));

        n = snprintf(result + off, rlen - off,
                     "[chunk_id=%d score=%.3f dense=%.3f lexical=%.3f tool=%s bytes=%zu pinned=%s]\n"
                     "%s\n\n",
                     c->id,
                     hits[i].final_score,
                     hits[i].dense_score,
                     hits[i].lexical_score,
                     c->tool,
                     c->text_len,
                     c->pinned ? "yes" : "no",
                     preview[0] ? preview : "(no preview)");
        if (n < 0 || (size_t)n >= rlen - off) {
            break;
        }
        off += (size_t)n;
    }

    snprintf(result + off, rlen - off,
             "next: context_get {\"chunk_id\":<id>,\"max_chars\":4000}");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FILE TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_write_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char *content = json_get_str(input, "content");
    if (!path || !content) {
        snprintf(result, rlen, "error: path and content required");
        free(path); free(content);
        return false;
    }
    char *pathcopy = strdup(path);
    char *dir = dirname(pathcopy);
    if (dir && strlen(dir) > 0 && strcmp(dir, ".") != 0) {
        char mkdir_cmd[4096];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", dir);
        system(mkdir_cmd);
    }
    free(pathcopy);

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); free(content);
        return false;
    }
    size_t n = fwrite(content, 1, strlen(content), f);
    fclose(f);
    snprintf(result, rlen, "wrote %zu bytes to %s", n, path);
    free(path); free(content);
    return true;
}

static bool tool_read_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    int offset = json_get_int(input, "offset", 0);
    int limit = json_get_int(input, "limit", 0);

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    int line_num = 0;
    int lines_written = 0;
    size_t total = 0;
    char line[4096];

    int hdr = snprintf(result, rlen, "=== %s (%ld bytes) ===\n", path, fsize);
    total = (size_t)hdr;

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        if (offset > 0 && line_num <= offset) continue;
        if (limit > 0 && lines_written >= limit) break;

        int n = snprintf(result + total, rlen - total, "%4d | %s", line_num, line);
        if (n < 0 || total + (size_t)n >= rlen - 64) {
            snprintf(result + total, rlen - total, "\n... truncated at line %d ...\n", line_num);
            break;
        }
        total += (size_t)n;
        lines_written++;
    }
    fclose(f);
    free(path);
    return true;
}

static bool tool_page_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    int page = json_get_int(input, "page", 1);
    int page_size = json_get_int(input, "page_size", 50);
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 50;
    if (page_size > 200) page_size = 200;

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); return false;
    }

    int total_lines = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) total_lines++;
    fseek(f, 0, SEEK_SET);

    int total_pages = (total_lines + page_size - 1) / page_size;
    if (total_pages < 1) total_pages = 1;
    if (page > total_pages) page = total_pages;

    int start_line = (page - 1) * page_size + 1;
    int end_line = start_line + page_size - 1;

    size_t total = 0;
    int hdr = snprintf(result, rlen, "=== %s | page %d/%d (lines %d-%d of %d) ===\n",
                       path, page, total_pages, start_line,
                       end_line > total_lines ? total_lines : end_line, total_lines);
    total = (size_t)hdr;

    int cur = 0;
    while (fgets(line, sizeof(line), f)) {
        cur++;
        if (cur < start_line) continue;
        if (cur > end_line) break;
        int n = snprintf(result + total, rlen - total, "%4d | %s", cur, line);
        if (n < 0 || total + (size_t)n >= rlen - 64) {
            snprintf(result + total, rlen - total, "\n... truncated ...\n");
            break;
        }
        total += (size_t)n;
    }
    fclose(f);
    free(path);
    return true;
}

static bool tool_edit_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char *old_str = json_get_str(input, "old_string");
    char *new_str = json_get_str(input, "new_string");

    if (!path || !old_str || !new_str) {
        snprintf(result, rlen, "error: path, old_string, and new_string required");
        free(path); free(old_str); free(new_str);
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); free(old_str); free(new_str);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc(fsize + 1);
    fread(content, 1, fsize, f);
    content[fsize] = '\0';
    fclose(f);

    char *loc = strstr(content, old_str);
    if (!loc) {
        snprintf(result, rlen, "error: old_string not found in %s", path);
        free(content); free(path); free(old_str); free(new_str);
        return false;
    }

    char *second = strstr(loc + 1, old_str);
    if (second && !json_get_bool(input, "replace_all", false)) {
        snprintf(result, rlen, "error: old_string appears multiple times in %s. "
                 "Provide more context or set replace_all=true", path);
        free(content); free(path); free(old_str); free(new_str);
        return false;
    }

    bool replace_all = json_get_bool(input, "replace_all", false);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    jbuf_t out;
    jbuf_init(&out, fsize + new_len + 256);

    int replacements = 0;
    char *p = content;
    while (*p) {
        char *found = strstr(p, old_str);
        if (found && (replacements == 0 || replace_all)) {
            jbuf_append_len(&out, p, (size_t)(found - p));
            jbuf_append_len(&out, new_str, new_len);
            p = found + old_len;
            replacements++;
        } else {
            jbuf_append_char(&out, *p);
            p++;
        }
    }

    f = fopen(path, "w");
    if (!f) {
        snprintf(result, rlen, "error: cannot write %s: %s", path, strerror(errno));
        jbuf_free(&out);
        free(content); free(path); free(old_str); free(new_str);
        return false;
    }
    fwrite(out.data, 1, out.len, f);
    fclose(f);

    snprintf(result, rlen, "edited %s: %d replacement(s)", path, replacements);
    jbuf_free(&out);
    free(content); free(path); free(old_str); free(new_str);
    return true;
}

static bool tool_list_dir(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) path = strdup(".");

    DIR *d = opendir(path);
    if (!d) {
        snprintf(result, rlen, "error: cannot open directory %s: %s", path, strerror(errno));
        free(path); return false;
    }

    size_t total = 0;
    int hdr = snprintf(result, rlen, "=== %s ===\n", path);
    total = (size_t)hdr;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        const char *suffix = "";
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) suffix = "/";

        int n = snprintf(result + total, rlen - total, "  %s%s\n", ent->d_name, suffix);
        if (n < 0 || total + (size_t)n >= rlen - 32) break;
        total += (size_t)n;
    }
    closedir(d);
    free(path);
    return true;
}

static bool tool_find_files(const char *input, char *result, size_t rlen) {
    char *pattern = json_get_str(input, "pattern");
    char *path = json_get_str(input, "path");
    if (!pattern) { snprintf(result, rlen, "error: pattern required"); free(path); return false; }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "find '%s' -name '%s' -type f 2>/dev/null | head -100",
             path ? path : ".", pattern);
    run_cmd(cmd, result, rlen);
    free(pattern); free(path);
    return true;
}

static bool tool_grep(const char *input, char *result, size_t rlen) {
    char *pattern = json_get_str(input, "pattern");
    char *path = json_get_str(input, "path");
    if (!pattern) { snprintf(result, rlen, "error: pattern required"); free(path); return false; }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "grep -rn '%s' '%s' 2>/dev/null | head -100",
             pattern, path ? path : ".");
    run_cmd(cmd, result, rlen);
    free(pattern); free(path);
    return true;
}

static bool tool_file_info(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(result, rlen, "error: stat %s: %s", path, strerror(errno));
        free(path); return false;
    }
    snprintf(result, rlen,
             "path: %s\nsize: %lld bytes\ntype: %s\nmode: %o\n",
             path, (long long)st.st_size,
             S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "file" : "other",
             st.st_mode & 0777);
    free(path);
    return true;
}

/* ── append_file: Append content to a file ────────────────────────────── */
static bool tool_append_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char *content = json_get_str(input, "content");
    if (!path || !content) {
        snprintf(result, rlen, "error: path and content required");
        free(path); free(content); return false;
    }
    FILE *f = fopen(path, "a");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); free(content); return false;
    }
    size_t n = fwrite(content, 1, strlen(content), f);
    fclose(f);
    snprintf(result, rlen, "appended %zu bytes to %s", n, path);
    free(path); free(content);
    return true;
}

/* ── move_file ────────────────────────────────────────────────────────── */
static bool tool_move_file(const char *input, char *result, size_t rlen) {
    char *src = json_get_str(input, "source");
    char *dst = json_get_str(input, "destination");
    if (!src || !dst) {
        snprintf(result, rlen, "error: source and destination required");
        free(src); free(dst); return false;
    }
    if (rename(src, dst) != 0) {
        /* Try mv for cross-device */
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", src, dst);
        int status = run_cmd(cmd, result, rlen);
        if (status == 0) snprintf(result, rlen, "moved %s -> %s", src, dst);
        free(src); free(dst);
        return (status == 0);
    }
    snprintf(result, rlen, "moved %s -> %s", src, dst);
    free(src); free(dst);
    return true;
}

/* ── copy_file ────────────────────────────────────────────────────────── */
static bool tool_copy_file(const char *input, char *result, size_t rlen) {
    char *src = json_get_str(input, "source");
    char *dst = json_get_str(input, "destination");
    if (!src || !dst) {
        snprintf(result, rlen, "error: source and destination required");
        free(src); free(dst); return false;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", src, dst);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0 && strlen(result) == 0)
        snprintf(result, rlen, "copied %s -> %s", src, dst);
    free(src); free(dst);
    return (status == 0);
}

/* ── delete_file ──────────────────────────────────────────────────────── */
static bool tool_delete_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    bool recursive = json_get_bool(input, "recursive", false);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    char cmd[4096];
    if (recursive)
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    else
        snprintf(cmd, sizeof(cmd), "rm -f '%s'", path);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0) snprintf(result, rlen, "deleted %s", path);
    free(path);
    return (status == 0);
}

/* ── mkdir ────────────────────────────────────────────────────────────── */
static bool tool_mkdir(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0) snprintf(result, rlen, "created directory %s", path);
    free(path);
    return (status == 0);
}

/* ── chmod ────────────────────────────────────────────────────────────── */
static bool tool_chmod(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char *mode = json_get_str(input, "mode");
    if (!path || !mode) {
        snprintf(result, rlen, "error: path and mode required");
        free(path); free(mode); return false;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "chmod %s '%s'", mode, path);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0) snprintf(result, rlen, "chmod %s %s", mode, path);
    free(path); free(mode);
    return (status == 0);
}

/* ── tree: Directory tree ─────────────────────────────────────────────── */
static bool tool_tree(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    int depth = json_get_int(input, "max_depth", 3);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "find '%s' -maxdepth %d -print 2>/dev/null | head -200 | sort",
             path ? path : ".", depth);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── wc: Word/line count ──────────────────────────────────────────────── */
static bool tool_wc(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "wc -l -w -c '%s'", path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── head/tail ────────────────────────────────────────────────────────── */
static bool tool_head(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    int lines = json_get_int(input, "lines", 20);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "head -n %d '%s'", lines, path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

static bool tool_tail(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    int lines = json_get_int(input, "lines", 20);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "tail -n %d '%s'", lines, path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── symlink ──────────────────────────────────────────────────────────── */
static bool tool_symlink(const char *input, char *result, size_t rlen) {
    char *target = json_get_str(input, "target");
    char *link_path = json_get_str(input, "link_path");
    if (!target || !link_path) {
        snprintf(result, rlen, "error: target and link_path required");
        free(target); free(link_path); return false;
    }
    if (symlink(target, link_path) != 0) {
        snprintf(result, rlen, "error: symlink failed: %s", strerror(errno));
        free(target); free(link_path); return false;
    }
    snprintf(result, rlen, "created symlink %s -> %s", link_path, target);
    free(target); free(link_path);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * COMPILE & RUN TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_compile(const char *input, char *result, size_t rlen) {
    char *source = json_get_str(input, "source");
    char *output = json_get_str(input, "output");
    char *flags = json_get_str(input, "flags");
    if (!source) { snprintf(result, rlen, "error: source required"); free(output); free(flags); return false; }
    if (!output) output = safe_strdup("a.out");

    char cmd[8192];
    int n = snprintf(cmd, sizeof(cmd), "cc -Wall -Wextra %s -o '%s' '%s'",
             flags ? flags : "", output, source);
    if (n >= (int)sizeof(cmd)) {
        snprintf(result, rlen, "error: compile command too long (truncated)");
        free(source); free(output); free(flags);
        return false;
    }
    int status = run_cmd(cmd, result, rlen);
    if (status == 0 && strlen(result) == 0) {
        snprintf(result, rlen, "compiled %s -> %s (success)", source, output);
    }
    free(source); free(output); free(flags);
    return (status == 0);
}

/* Shell-escape a string for use inside single quotes: replace ' with '\'' */
static char *shell_escape(const char *s) {
    if (!s) return safe_strdup("");
    size_t n = 0;
    for (const char *p = s; *p; p++) {
        n += (*p == '\'') ? 4 : 1;
    }
    char *out = safe_malloc(n + 1);
    char *d = out;
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            *d++ = '\''; *d++ = '\\'; *d++ = '\''; *d++ = '\'';
        } else {
            *d++ = *p;
        }
    }
    *d = '\0';
    return out;
}

static bool tool_run_command(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }

    int timeout = json_get_int(input, "timeout", 30);
    if (timeout <= 0) timeout = 30;
    if (timeout > 600) timeout = 600;

    run_opts_t opts = RUN_OPTS_DEFAULT;
    opts.wall_timeout_s = timeout;
    opts.idle_timeout_s = timeout;  /* for short commands, idle=wall */
    opts.stream_to_tty  = true;
    opts.label          = "run_command";

    char *escaped = shell_escape(command);
    size_t cmd_len = strlen(escaped) + 64;
    char *cmd = safe_malloc(cmd_len);
    snprintf(cmd, cmd_len, "sh -c '%s'", escaped);
    int status = run_cmd_ex(cmd, result, rlen, &opts);
    if (status != 0 && strlen(result) == 0) {
        snprintf(result, rlen, "command exited with status %d", status);
    }
    free(cmd);
    free(escaped);
    free(command);
    return (status == 0);
}

/* bash tool — streaming subprocess with live output deltas */
static bool tool_bash(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }

    int timeout = json_get_int(input, "timeout", 120);
    if (timeout <= 0) timeout = 120;
    if (timeout > 600) timeout = 600;
    char *cwd = json_get_str(input, "cwd");

    run_opts_t opts = RUN_OPTS_DEFAULT;
    opts.wall_timeout_s = timeout;
    opts.idle_timeout_s = 60;   /* idle=60s for long-running scripts */
    opts.stream_to_tty  = true;
    opts.dim_output     = true;
    opts.label          = "bash";

    char *escaped = shell_escape(command);
    size_t cmd_len = strlen(escaped) + (cwd ? strlen(cwd) + 32 : 0) + 64;
    char *cmd = safe_malloc(cmd_len);
    if (cwd) {
        char *esc_cwd = shell_escape(cwd);
        snprintf(cmd, cmd_len, "cd '%s' && bash -c '%s'", esc_cwd, escaped);
        free(esc_cwd);
    } else {
        snprintf(cmd, cmd_len, "bash -c '%s'", escaped);
    }
    int status = run_cmd_ex(cmd, result, rlen, &opts);
    if (status != 0 && strlen(result) == 0) {
        snprintf(result, rlen, "command exited with status %d", status);
    }
    free(cmd);
    free(escaped);
    free(command);
    free(cwd);
    return (status == 0);
}

/* ── run_background: Run a command in background ──────────────────────── */
static bool tool_run_background(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }

    char *escaped = shell_escape(command);
    size_t cmd_len = strlen(escaped) + 128;
    char *cmd = safe_malloc(cmd_len);
    snprintf(cmd, cmd_len, "nohup sh -c '%s' > /tmp/dsco_bg_$$.log 2>&1 & echo $!", escaped);
    run_cmd(cmd, result, rlen);
    free(cmd);
    free(escaped);
    free(command);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GIT TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_git_status(const char *input, char *result, size_t rlen) {
    (void)input;
    run_cmd("git status --short --branch", result, rlen);
    return true;
}

static bool tool_git_diff(const char *input, char *result, size_t rlen) {
    char *args = json_get_str(input, "args");
    char cmd[4096];
    int n = snprintf(cmd, sizeof(cmd), "git diff %s", args ? args : "");
    if (n >= (int)sizeof(cmd)) {
        snprintf(result, rlen, "error: git diff args too long (truncated)");
        free(args);
        return false;
    }
    run_cmd(cmd, result, rlen);
    free(args);
    return true;
}

static bool tool_git_log(const char *input, char *result, size_t rlen) {
    int count = json_get_int(input, "count", 10);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "git log --oneline -n %d", count);
    run_cmd(cmd, result, rlen);
    return true;
}

static bool tool_git_commit(const char *input, char *result, size_t rlen) {
    char *message = json_get_str(input, "message");
    bool all = json_get_bool(input, "all", false);
    if (!message) { snprintf(result, rlen, "error: message required"); return false; }

    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "git commit");
    if (all) jbuf_append(&cmd, " -a");
    jbuf_append(&cmd, " -m ");
    shell_quote(&cmd, message);
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(message);
    return true;
}

static bool tool_git_add(const char *input, char *result, size_t rlen) {
    char *files = json_get_str(input, "files");
    if (!files) { snprintf(result, rlen, "error: files required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "git add %s", files);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0 && strlen(result) == 0)
        snprintf(result, rlen, "staged: %s", files);
    free(files);
    return (status == 0);
}

static bool tool_git_branch(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    bool create = json_get_bool(input, "create", false);
    char cmd[4096];
    if (name && create)
        snprintf(cmd, sizeof(cmd), "git checkout -b '%s'", name);
    else if (name)
        snprintf(cmd, sizeof(cmd), "git checkout '%s'", name);
    else
        snprintf(cmd, sizeof(cmd), "git branch -a");
    run_cmd(cmd, result, rlen);
    free(name);
    return true;
}

static bool tool_git_stash(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char cmd[4096];
    if (action && strcmp(action, "pop") == 0)
        snprintf(cmd, sizeof(cmd), "git stash pop");
    else if (action && strcmp(action, "list") == 0)
        snprintf(cmd, sizeof(cmd), "git stash list");
    else if (action && strcmp(action, "drop") == 0)
        snprintf(cmd, sizeof(cmd), "git stash drop");
    else
        snprintf(cmd, sizeof(cmd), "git stash");
    run_cmd(cmd, result, rlen);
    free(action);
    return true;
}

static bool tool_git_clone(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *dir = json_get_str(input, "directory");
    if (!url) { snprintf(result, rlen, "error: url required"); free(dir); return false; }
    char cmd[8192];
    if (dir)
        snprintf(cmd, sizeof(cmd), "git clone '%s' '%s'", url, dir);
    else
        snprintf(cmd, sizeof(cmd), "git clone '%s'", url);
    int status = run_cmd(cmd, result, rlen);
    free(url); free(dir);
    return (status == 0);
}

static bool tool_git_push(const char *input, char *result, size_t rlen) {
    char *remote = json_get_str(input, "remote");
    char *branch = json_get_str(input, "branch");
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "git push %s %s",
             remote ? remote : "origin",
             branch ? branch : "");
    int status = run_cmd(cmd, result, rlen);
    free(remote); free(branch);
    return (status == 0);
}

static bool tool_git_pull(const char *input, char *result, size_t rlen) {
    char *remote = json_get_str(input, "remote");
    char *branch = json_get_str(input, "branch");
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "git pull %s %s",
             remote ? remote : "",
             branch ? branch : "");
    int status = run_cmd(cmd, result, rlen);
    free(remote); free(branch);
    return (status == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PROCESS & SYSTEM TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_ps(const char *input, char *result, size_t rlen) {
    char *filter = json_get_str(input, "filter");
    char cmd[4096];
    if (filter)
        snprintf(cmd, sizeof(cmd), "ps aux | grep -i '%s' | grep -v grep", filter);
    else
        snprintf(cmd, sizeof(cmd), "ps aux | head -30");
    run_cmd(cmd, result, rlen);
    free(filter);
    return true;
}

static bool tool_kill_process(const char *input, char *result, size_t rlen) {
    int pid = json_get_int(input, "pid", 0);
    int sig = json_get_int(input, "signal", 15);
    if (pid <= 0) { snprintf(result, rlen, "error: pid required"); return false; }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "kill -%d %d", sig, pid);
    int status = run_cmd(cmd, result, rlen);
    if (status == 0) snprintf(result, rlen, "sent signal %d to pid %d", sig, pid);
    return (status == 0);
}

static bool tool_env_get(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    if (!name) {
        run_cmd("env | sort | head -80", result, rlen);
        return true;
    }
    const char *val = getenv(name);
    if (val)
        snprintf(result, rlen, "%s=%s", name, val);
    else
        snprintf(result, rlen, "%s is not set", name);
    free(name);
    return true;
}

static bool tool_env_set(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    char *value = json_get_str(input, "value");
    if (!name || !value) {
        snprintf(result, rlen, "error: name and value required");
        free(name); free(value); return false;
    }
    setenv(name, value, 1);
    snprintf(result, rlen, "set %s=%s", name, value);
    free(name); free(value);
    return true;
}

static bool tool_sysinfo(const char *input, char *result, size_t rlen) {
    (void)input;
    run_cmd("uname -a && echo '---' && sw_vers 2>/dev/null || cat /etc/os-release 2>/dev/null && echo '---' && df -h / && echo '---' && uptime", result, rlen);
    return true;
}

static bool tool_disk_usage(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "du -sh '%s' 2>/dev/null && echo '---' && df -h '%s'",
             path ? path : ".", path ? path : ".");
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

static bool tool_which(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    if (!name) { snprintf(result, rlen, "error: name required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "which '%s' 2>/dev/null && '%s' --version 2>/dev/null | head -1", name, name);
    run_cmd(cmd, result, rlen);
    free(name);
    return true;
}

static bool tool_cwd(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (path) {
        if (chdir(path) != 0) {
            snprintf(result, rlen, "error: chdir %s: %s", path, strerror(errno));
            free(path); return false;
        }
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        snprintf(result, rlen, "%s", cwd);
    else
        snprintf(result, rlen, "error: getcwd failed");
    free(path);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TEXT PROCESSING TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_sed(const char *input, char *result, size_t rlen) {
    char *pattern = json_get_str(input, "expression");
    char *file = json_get_str(input, "file");
    if (!pattern || !file) {
        snprintf(result, rlen, "error: expression and file required");
        free(pattern); free(file); return false;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "sed '%s' '%s'", pattern, file);
    run_cmd(cmd, result, rlen);
    free(pattern); free(file);
    return true;
}

static bool tool_awk(const char *input, char *result, size_t rlen) {
    char *program = json_get_str(input, "program");
    char *file = json_get_str(input, "file");
    if (!program) {
        snprintf(result, rlen, "error: program required");
        free(file); return false;
    }
    char cmd[8192];
    if (file)
        snprintf(cmd, sizeof(cmd), "awk '%s' '%s'", program, file);
    else
        snprintf(cmd, sizeof(cmd), "echo '' | awk '%s'", program);
    run_cmd(cmd, result, rlen);
    free(program); free(file);
    return true;
}

static bool tool_sort_uniq(const char *input, char *result, size_t rlen) {
    char *file = json_get_str(input, "file");
    bool unique = json_get_bool(input, "unique", false);
    bool count = json_get_bool(input, "count", false);
    if (!file) { snprintf(result, rlen, "error: file required"); return false; }
    char cmd[4096];
    if (count)
        snprintf(cmd, sizeof(cmd), "sort '%s' | uniq -c | sort -rn | head -50", file);
    else if (unique)
        snprintf(cmd, sizeof(cmd), "sort -u '%s' | head -200", file);
    else
        snprintf(cmd, sizeof(cmd), "sort '%s' | head -200", file);
    run_cmd(cmd, result, rlen);
    free(file);
    return true;
}

static bool tool_diff(const char *input, char *result, size_t rlen) {
    char *file1 = json_get_str(input, "file1");
    char *file2 = json_get_str(input, "file2");
    if (!file1 || !file2) {
        snprintf(result, rlen, "error: file1 and file2 required");
        free(file1); free(file2); return false;
    }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "diff -u '%s' '%s'", file1, file2);
    run_cmd(cmd, result, rlen);
    free(file1); free(file2);
    return true;
}

static bool tool_patch(const char *input, char *result, size_t rlen) {
    char *file = json_get_str(input, "file");
    char *patch_content = json_get_str(input, "patch");
    if (!file || !patch_content) {
        snprintf(result, rlen, "error: file and patch required");
        free(file); free(patch_content); return false;
    }
    /* Write patch to temp file */
    char tmpfile[] = "/tmp/dsco_patch_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        snprintf(result, rlen, "error: cannot create temp file");
        free(file); free(patch_content); return false;
    }
    write(fd, patch_content, strlen(patch_content));
    close(fd);

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "patch '%s' < '%s'", file, tmpfile);
    int status = run_cmd(cmd, result, rlen);
    unlink(tmpfile);
    free(file); free(patch_content);
    return (status == 0);
}

static bool tool_jq(const char *input, char *result, size_t rlen) {
    char *filter = json_get_str(input, "filter");
    char *json_input = json_get_str(input, "input");
    char *file = json_get_str(input, "file");
    if (!filter) {
        snprintf(result, rlen, "error: filter required");
        free(json_input); free(file); return false;
    }
    char cmd[8192];
    if (file) {
        snprintf(cmd, sizeof(cmd), "jq '%s' '%s'", filter, file);
    } else if (json_input) {
        char tmpfile[] = "/tmp/dsco_jq_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd >= 0) { write(fd, json_input, strlen(json_input)); close(fd); }
        snprintf(cmd, sizeof(cmd), "jq '%s' '%s'", filter, tmpfile);
        run_cmd(cmd, result, rlen);
        unlink(tmpfile);
        free(filter); free(json_input); free(file);
        return true;
    } else {
        snprintf(result, rlen, "error: input or file required");
        free(filter); free(json_input); free(file); return false;
    }
    run_cmd(cmd, result, rlen);
    free(filter); free(json_input); free(file);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ENCODING & HASHING TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_base64(const char *input, char *result, size_t rlen) {
    char *data = json_get_str(input, "data");
    char *file = json_get_str(input, "file");
    bool decode = json_get_bool(input, "decode", false);
    char cmd[8192];

    if (file) {
        if (decode)
            snprintf(cmd, sizeof(cmd), "base64 -d < '%s'", file);
        else
            snprintf(cmd, sizeof(cmd), "base64 < '%s'", file);
    } else if (data) {
        char tmpfile[] = "/tmp/dsco_b64_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd >= 0) { write(fd, data, strlen(data)); close(fd); }
        if (decode)
            snprintf(cmd, sizeof(cmd), "base64 -d < '%s'", tmpfile);
        else
            snprintf(cmd, sizeof(cmd), "base64 < '%s'", tmpfile);
        run_cmd(cmd, result, rlen);
        unlink(tmpfile);
        free(data); free(file);
        return true;
    } else {
        snprintf(result, rlen, "error: data or file required");
        free(data); free(file); return false;
    }
    run_cmd(cmd, result, rlen);
    free(data); free(file);
    return true;
}

static bool tool_hash(const char *input, char *result, size_t rlen) {
    char *file = json_get_str(input, "file");
    char *data = json_get_str(input, "data");
    char *algo = json_get_str(input, "algorithm");
    if (!algo) algo = strdup("sha256");

    char cmd[8192];
    if (file) {
        if (strcmp(algo, "md5") == 0)
            snprintf(cmd, sizeof(cmd), "md5sum '%s' 2>/dev/null || md5 '%s'", file, file);
        else if (strcmp(algo, "sha1") == 0)
            snprintf(cmd, sizeof(cmd), "sha1sum '%s' 2>/dev/null || shasum '%s'", file, file);
        else
            snprintf(cmd, sizeof(cmd), "sha256sum '%s' 2>/dev/null || shasum -a 256 '%s'", file, file);
    } else if (data) {
        if (strcmp(algo, "md5") == 0)
            snprintf(cmd, sizeof(cmd), "echo -n '%s' | md5sum 2>/dev/null || echo -n '%s' | md5", data, data);
        else if (strcmp(algo, "sha1") == 0)
            snprintf(cmd, sizeof(cmd), "echo -n '%s' | sha1sum 2>/dev/null || echo -n '%s' | shasum", data, data);
        else
            snprintf(cmd, sizeof(cmd), "echo -n '%s' | sha256sum 2>/dev/null || echo -n '%s' | shasum -a 256", data, data);
    } else {
        snprintf(result, rlen, "error: file or data required");
        free(algo); return false;
    }
    run_cmd(cmd, result, rlen);
    free(file); free(data); free(algo);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ARCHIVE TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_tar(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char *archive = json_get_str(input, "archive");
    char *files = json_get_str(input, "files");
    if (!action || !archive) {
        snprintf(result, rlen, "error: action and archive required");
        free(action); free(archive); free(files); return false;
    }
    char cmd[8192];
    if (strcmp(action, "create") == 0) {
        if (!files) { snprintf(result, rlen, "error: files required for create"); free(action); free(archive); return false; }
        snprintf(cmd, sizeof(cmd), "tar czf '%s' %s", archive, files);
    } else if (strcmp(action, "extract") == 0) {
        snprintf(cmd, sizeof(cmd), "tar xzf '%s'", archive);
    } else if (strcmp(action, "list") == 0) {
        snprintf(cmd, sizeof(cmd), "tar tzf '%s' | head -100", archive);
    } else {
        snprintf(result, rlen, "error: action must be create, extract, or list");
        free(action); free(archive); free(files); return false;
    }
    int status = run_cmd(cmd, result, rlen);
    if (status == 0 && strlen(result) == 0) snprintf(result, rlen, "done");
    free(action); free(archive); free(files);
    return (status == 0);
}

static bool tool_zip(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char *archive = json_get_str(input, "archive");
    char *files = json_get_str(input, "files");
    if (!action || !archive) {
        snprintf(result, rlen, "error: action and archive required");
        free(action); free(archive); free(files); return false;
    }
    char cmd[8192];
    if (strcmp(action, "create") == 0) {
        if (!files) { snprintf(result, rlen, "error: files required"); free(action); free(archive); return false; }
        snprintf(cmd, sizeof(cmd), "zip -r '%s' %s", archive, files);
    } else if (strcmp(action, "extract") == 0) {
        snprintf(cmd, sizeof(cmd), "unzip -o '%s'", archive);
    } else if (strcmp(action, "list") == 0) {
        snprintf(cmd, sizeof(cmd), "unzip -l '%s' | head -100", archive);
    } else {
        snprintf(result, rlen, "error: action must be create, extract, or list");
        free(action); free(archive); free(files); return false;
    }
    int status = run_cmd(cmd, result, rlen);
    free(action); free(archive); free(files);
    return (status == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * NETWORK / CURL TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_http_request(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *method = json_get_str(input, "method");
    char *headers_str = json_get_str(input, "headers");
    char *body = json_get_str(input, "body");
    bool include_headers = json_get_bool(input, "include_headers", false);
    int timeout = json_get_int(input, "timeout", 30);

    if (!url) {
        snprintf(result, rlen, "error: url required");
        free(method); free(headers_str); free(body);
        return false;
    }
    if (!method) method = safe_strdup("GET");

    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "curl -sS");
    if (include_headers) jbuf_append(&cmd, " -i");
    jbuf_append(&cmd, " -X ");
    jbuf_append(&cmd, method);
    jbuf_append(&cmd, " --max-time ");
    char timeout_str[16]; snprintf(timeout_str, sizeof(timeout_str), "%d", timeout);
    jbuf_append(&cmd, timeout_str);

    if (headers_str) {
        char *hcopy = safe_strdup(headers_str);
        char *hdr = strtok(hcopy, "\n");
        while (hdr) {
            jbuf_append(&cmd, " -H '");
            jbuf_append(&cmd, hdr);
            jbuf_append(&cmd, "'");
            hdr = strtok(NULL, "\n");
        }
        free(hcopy);
    }

    char body_tmpfile[32] = "";
    if (body) {
        strcpy(body_tmpfile, "/tmp/dsco_body_XXXXXX");
        int fd = mkstemp(body_tmpfile);
        if (fd >= 0) {
            write(fd, body, strlen(body));
            close(fd);
            jbuf_append(&cmd, " -d @");
            jbuf_append(&cmd, body_tmpfile);
        }
    }

    jbuf_append(&cmd, " '");
    jbuf_append(&cmd, url);
    jbuf_append(&cmd, "'");

    run_cmd(cmd.data, result, rlen);
    if (body_tmpfile[0]) unlink(body_tmpfile);
    jbuf_free(&cmd);
    free(url); free(method); free(headers_str); free(body);
    return true;
}

static bool tool_download(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *output = json_get_str(input, "output");
    if (!url || !output) {
        snprintf(result, rlen, "error: url and output required");
        free(url); free(output); return false;
    }

    char cmd[4096];
    int n = snprintf(cmd, sizeof(cmd), "curl -sS -L -o '%s' '%s'", output, url);
    if (n >= (int)sizeof(cmd)) {
        snprintf(result, rlen, "error: download command too long (truncated)");
        free(url); free(output);
        return false;
    }
    int status = run_cmd(cmd, result, rlen);
    if (status == 0) {
        struct stat st;
        if (stat(output, &st) == 0) {
            snprintf(result, rlen, "downloaded %s -> %s (%lld bytes)", url, output, (long long)st.st_size);
        }
    }
    free(url); free(output);
    return (status == 0);
}

static bool tool_upload(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *file = json_get_str(input, "file");
    char *field = json_get_str(input, "field_name");
    if (!url || !file) {
        snprintf(result, rlen, "error: url and file required");
        free(url); free(file); free(field); return false;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "curl -sS -X POST -F '%s=@%s' '%s'",
             field ? field : "file", file, url);
    run_cmd(cmd, result, rlen);
    free(url); free(file); free(field);
    return true;
}

static bool tool_dns_lookup(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "hostname");
    if (!host) { snprintf(result, rlen, "error: hostname required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "host '%s'", host);
    run_cmd(cmd, result, rlen);
    free(host);
    return true;
}

static bool tool_ping(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    int count = json_get_int(input, "count", 4);
    if (!host) { snprintf(result, rlen, "error: host required"); return false; }
    if (count > 20) count = 20;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "ping -c %d '%s'", count, host);
    run_cmd(cmd, result, rlen);
    free(host);
    return true;
}

static bool tool_curl_raw(const char *input, char *result, size_t rlen) {
    char *args = json_get_str(input, "args");
    if (!args) { snprintf(result, rlen, "error: args required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "curl %s", args);
    run_cmd(cmd, result, rlen);
    free(args);
    return true;
}

static bool tool_http_headers(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    if (!url) { snprintf(result, rlen, "error: url required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "curl -sS -I '%s'", url);
    run_cmd(cmd, result, rlen);
    free(url);
    return true;
}

static bool tool_ws_test(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *message = json_get_str(input, "message");
    if (!url) { snprintf(result, rlen, "error: url required"); free(message); return false; }
    char cmd[4096];
    if (message) {
        snprintf(cmd, sizeof(cmd),
                 "echo '%s' | timeout 5 curl -sS --no-buffer -H 'Connection: Upgrade' "
                 "-H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' "
                 "-H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' '%s'",
                 message, url);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "timeout 5 curl -sS -i -H 'Connection: Upgrade' "
                 "-H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' "
                 "-H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' '%s'", url);
    }
    run_cmd(cmd, result, rlen);
    free(url); free(message);
    return true;
}

static bool tool_port_check(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    int port = json_get_int(input, "port", 0);
    if (!host || port <= 0) {
        snprintf(result, rlen, "error: host and port required");
        free(host); return false;
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "nc -z -w 3 '%s' %d && echo 'OPEN' || echo 'CLOSED'", host, port);
    run_cmd(cmd, result, rlen);
    free(host);
    return true;
}

static bool tool_net_interfaces(const char *input, char *result, size_t rlen) {
    (void)input;
    run_cmd("ifconfig 2>/dev/null || ip addr 2>/dev/null", result, rlen);
    return true;
}

static bool tool_whois(const char *input, char *result, size_t rlen) {
    char *domain = json_get_str(input, "domain");
    if (!domain) { snprintf(result, rlen, "error: domain required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "whois '%s' 2>/dev/null | head -80", domain);
    run_cmd(cmd, result, rlen);
    free(domain);
    return true;
}

static bool tool_cert_info(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    int port = json_get_int(input, "port", 443);
    if (!host) { snprintf(result, rlen, "error: host required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "echo | openssl s_client -connect '%s:%d' -servername '%s' 2>/dev/null | "
             "openssl x509 -noout -subject -issuer -dates -ext subjectAltName 2>/dev/null",
             host, port, host);
    run_cmd(cmd, result, rlen);
    free(host);
    return true;
}

static bool tool_traceroute(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    if (!host) { snprintf(result, rlen, "error: host required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "traceroute -m 15 '%s' 2>&1", host);
    run_cmd(cmd, result, rlen);
    free(host);
    return true;
}

static bool tool_json_api(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *method = json_get_str(input, "method");
    char *body = json_get_str(input, "body");
    char *auth = json_get_str(input, "auth_header");
    if (!url) {
        snprintf(result, rlen, "error: url required");
        free(method); free(body); free(auth); return false;
    }
    if (!method) method = safe_strdup("GET");

    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "curl -sS -X ");
    jbuf_append(&cmd, method);
    jbuf_append(&cmd, " -H 'Content-Type: application/json' -H 'Accept: application/json'");
    if (auth) {
        jbuf_append(&cmd, " -H 'Authorization: ");
        jbuf_append(&cmd, auth);
        jbuf_append(&cmd, "'");
    }
    char json_tmpfile[32] = "";
    if (body) {
        strcpy(json_tmpfile, "/tmp/dsco_json_XXXXXX");
        int fd = mkstemp(json_tmpfile);
        if (fd >= 0) {
            write(fd, body, strlen(body));
            close(fd);
            jbuf_append(&cmd, " -d @");
            jbuf_append(&cmd, json_tmpfile);
        }
    }
    jbuf_append(&cmd, " '");
    jbuf_append(&cmd, url);
    jbuf_append(&cmd, "'");

    run_cmd(cmd.data, result, rlen);
    if (json_tmpfile[0]) unlink(json_tmpfile);
    jbuf_free(&cmd);
    free(url); free(method); free(body); free(auth);
    return true;
}

static bool tool_port_scan(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    char *ports = json_get_str(input, "ports");
    if (!host) { snprintf(result, rlen, "error: host required"); free(ports); return false; }
    char cmd[4096];
    if (ports)
        snprintf(cmd, sizeof(cmd), "for p in %s; do nc -z -w 1 '%s' $p 2>/dev/null && echo \"$p OPEN\" || echo \"$p CLOSED\"; done", ports, host);
    else
        snprintf(cmd, sizeof(cmd), "for p in 22 80 443 8080 8443 3000 5000 5432 3306 6379 27017; do nc -z -w 1 '%s' $p 2>/dev/null && echo \"$p OPEN\" || echo \"$p CLOSED\"; done", host);
    run_cmd(cmd, result, rlen);
    free(host); free(ports);
    return true;
}

static bool tool_netstat(const char *input, char *result, size_t rlen) {
    (void)input;
    run_cmd("netstat -an 2>/dev/null | head -60 || ss -tuln 2>/dev/null | head -60", result, rlen);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DOCKER TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_docker(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "docker %s", command);
    run_cmd(cmd, result, rlen);
    free(command);
    return true;
}

static bool tool_docker_compose(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "docker compose %s 2>/dev/null || docker-compose %s", command, command);
    run_cmd(cmd, result, rlen);
    free(command);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SSH / REMOTE TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_ssh_command(const char *input, char *result, size_t rlen) {
    char *host = json_get_str(input, "host");
    char *command = json_get_str(input, "command");
    char *user = json_get_str(input, "user");
    char *key = json_get_str(input, "key");
    if (!host || !command) {
        snprintf(result, rlen, "error: host and command required");
        free(host); free(command); free(user); free(key); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10");
    if (key) {
        jbuf_append(&cmd, " -i ");
        shell_quote(&cmd, key);
    }
    jbuf_append(&cmd, " ");
    if (user) {
        jbuf_append(&cmd, user);
        jbuf_append(&cmd, "@");
    }
    jbuf_append(&cmd, host);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, command);

    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(host); free(command); free(user); free(key);
    return true;
}

static bool tool_scp(const char *input, char *result, size_t rlen) {
    char *source = json_get_str(input, "source");
    char *destination = json_get_str(input, "destination");
    char *key = json_get_str(input, "key");
    if (!source || !destination) {
        snprintf(result, rlen, "error: source and destination required");
        free(source); free(destination); free(key); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "scp -o StrictHostKeyChecking=no");
    if (key) {
        jbuf_append(&cmd, " -i ");
        shell_quote(&cmd, key);
    }
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, source);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, destination);

    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(source); free(destination); free(key);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DATABASE TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_sqlite(const char *input, char *result, size_t rlen) {
    char *db = json_get_str(input, "database");
    char *query = json_get_str(input, "query");
    if (!db || !query) {
        snprintf(result, rlen, "error: database and query required");
        free(db); free(query); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 8192);
    jbuf_append(&cmd, "sqlite3 -header -column ");
    shell_quote(&cmd, db);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, query);
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(db); free(query);
    return true;
}

static bool tool_psql(const char *input, char *result, size_t rlen) {
    char *connstr = json_get_str(input, "connection");
    char *query = json_get_str(input, "query");
    if (!query) {
        snprintf(result, rlen, "error: query required");
        free(connstr); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 8192);
    jbuf_append(&cmd, "psql");
    if (connstr) {
        jbuf_append(&cmd, " ");
        shell_quote(&cmd, connstr);
    }
    jbuf_append(&cmd, " -c ");
    shell_quote(&cmd, query);
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(connstr); free(query);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PYTHON / SCRIPTING TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_python(const char *input, char *result, size_t rlen) {
    char *code = json_get_str(input, "code");
    char *file = json_get_str(input, "file");
    if (!code && !file) {
        snprintf(result, rlen, "error: code or file required");
        return false;
    }
    char cmd[MAX_TOOL_RESULT];
    if (file) {
        snprintf(cmd, sizeof(cmd), "python3 '%s'", file);
    } else {
        char tmpfile[] = "/tmp/dsco_py_XXXXXX";
        int fd = mkstemp(tmpfile);
        /* Rename to .py so python recognizes it */
        char pyfile[256];
        snprintf(pyfile, sizeof(pyfile), "%s.py", tmpfile);
        rename(tmpfile, pyfile);
        if (fd >= 0) { write(fd, code, strlen(code)); close(fd); }
        snprintf(cmd, sizeof(cmd), "python3 '%s'", pyfile);
        int status = run_cmd(cmd, result, rlen);
        unlink(pyfile);
        free(code); free(file);
        return (status == 0);
    }
    int status = run_cmd(cmd, result, rlen);
    free(code); free(file);
    return (status == 0);
}

static bool tool_node(const char *input, char *result, size_t rlen) {
    char *code = json_get_str(input, "code");
    char *file = json_get_str(input, "file");
    if (!code && !file) {
        snprintf(result, rlen, "error: code or file required");
        return false;
    }
    char cmd[MAX_TOOL_RESULT];
    if (file) {
        snprintf(cmd, sizeof(cmd), "node '%s'", file);
    } else {
        char tmpfile[] = "/tmp/dsco_js_XXXXXX";
        int fd = mkstemp(tmpfile);
        char jsfile[256];
        snprintf(jsfile, sizeof(jsfile), "%s.js", tmpfile);
        rename(tmpfile, jsfile);
        if (fd >= 0) { write(fd, code, strlen(code)); close(fd); }
        snprintf(cmd, sizeof(cmd), "node '%s'", jsfile);
        int status = run_cmd(cmd, result, rlen);
        unlink(jsfile);
        free(code); free(file);
        return (status == 0);
    }
    int status = run_cmd(cmd, result, rlen);
    free(code); free(file);
    return (status == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DATE/TIME & MATH TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_date(const char *input, char *result, size_t rlen) {
    char *format = json_get_str(input, "format");
    char *tz = json_get_str(input, "timezone");
    char cmd[1024];
    if (tz)
        snprintf(cmd, sizeof(cmd), "TZ='%s' date '%s'", tz, format ? format : "+%Y-%m-%d %H:%M:%S %Z");
    else
        snprintf(cmd, sizeof(cmd), "date '%s'", format ? format : "+%Y-%m-%d %H:%M:%S %Z");
    run_cmd(cmd, result, rlen);
    free(format); free(tz);
    return true;
}

static bool tool_calc(const char *input, char *result, size_t rlen) {
    char *expression = json_get_str(input, "expression");
    if (!expression) { snprintf(result, rlen, "error: expression required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "echo '%s' | bc -l 2>/dev/null || python3 -c 'print(%s)'", expression, expression);
    run_cmd(cmd, result, rlen);
    free(expression);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CLIPBOARD
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_clipboard(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char *content = json_get_str(input, "content");

    if (!action) action = strdup("read");

    if (strcmp(action, "write") == 0 || strcmp(action, "copy") == 0) {
        if (!content) { snprintf(result, rlen, "error: content required for write"); free(action); return false; }
        char tmpfile[] = "/tmp/dsco_clip_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd >= 0) { write(fd, content, strlen(content)); close(fd); }
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "cat '%s' | pbcopy 2>/dev/null || cat '%s' | xclip -selection clipboard 2>/dev/null || cat '%s' | xsel --clipboard 2>/dev/null", tmpfile, tmpfile, tmpfile);
        run_cmd(cmd, result, rlen);
        unlink(tmpfile);
        if (strlen(result) == 0) snprintf(result, rlen, "copied %zu bytes to clipboard", strlen(content));
    } else {
        run_cmd("pbpaste 2>/dev/null || xclip -selection clipboard -o 2>/dev/null || xsel --clipboard -o 2>/dev/null", result, rlen);
    }
    free(action); free(content);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PACKAGE MANAGEMENT
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_pkg(const char *input, char *result, size_t rlen) {
    char *manager = json_get_str(input, "manager");
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); free(manager); return false; }
    char cmd[8192];
    if (manager)
        snprintf(cmd, sizeof(cmd), "%s %s", manager, command);
    else
        snprintf(cmd, sizeof(cmd), "brew %s 2>/dev/null || apt %s 2>/dev/null || yum %s 2>/dev/null", command, command, command);
    run_cmd(cmd, result, rlen);
    free(manager); free(command);
    return true;
}

static bool tool_pip(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "pip3 %s", command);
    run_cmd(cmd, result, rlen);
    free(command);
    return true;
}

static bool tool_npm(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "npm %s", command);
    run_cmd(cmd, result, rlen);
    free(command);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CRON / SCHEDULING
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_crontab(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char *entry = json_get_str(input, "entry");
    if (!action) action = strdup("list");

    if (strcmp(action, "list") == 0) {
        run_cmd("crontab -l 2>/dev/null || echo 'no crontab'", result, rlen);
    } else if (strcmp(action, "add") == 0 && entry) {
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "(crontab -l 2>/dev/null; echo '%s') | crontab -", entry);
        run_cmd(cmd, result, rlen);
    } else {
        snprintf(result, rlen, "error: action must be list or add (with entry)");
        free(action); free(entry); return false;
    }
    free(action); free(entry);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * XATTR / EXTENDED ATTRIBUTES (macOS)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_xattr(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    char *action = json_get_str(input, "action");
    if (!path) { snprintf(result, rlen, "error: path required"); free(action); return false; }
    char cmd[4096];
    if (action && strcmp(action, "clear") == 0)
        snprintf(cmd, sizeof(cmd), "xattr -cr '%s'", path);
    else
        snprintf(cmd, sizeof(cmd), "xattr -l '%s'", path);
    run_cmd(cmd, result, rlen);
    free(path); free(action);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * AST SELF-INTROSPECTION TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_self_inspect(const char *input, char *result, size_t rlen) {
    char *dir = json_get_str(input, "project_dir");
    if (!dir) {
        /* Default: find our own source directory */
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)))
            dir = strdup(cwd);
        else
            dir = strdup(".");
    }

    ast_project_t *proj = ast_introspect(dir);
    if (!proj || proj->file_count == 0) {
        snprintf(result, rlen, "{\"error\":\"no C/H files found in %s\"}", dir);
        free(dir);
        ast_free_project(proj);
        return false;
    }

    ast_summary_json(proj, result, rlen);
    ast_free_project(proj);
    free(dir);
    return true;
}

static bool tool_inspect_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) {
        snprintf(result, rlen, "error: path required");
        return false;
    }

    ast_file_t *f = ast_parse_file(path);
    if (!f) {
        snprintf(result, rlen, "{\"error\":\"cannot parse %s\"}", path);
        free(path);
        return false;
    }

    ast_file_summary_json(f, result, rlen);
    ast_free_file(f);
    free(path);
    return true;
}

static bool tool_call_graph(const char *input, char *result, size_t rlen) {
    char *func = json_get_str(input, "function");
    char *dir = json_get_str(input, "project_dir");
    if (!func) {
        snprintf(result, rlen, "error: function name required");
        free(dir);
        return false;
    }
    if (!dir) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) dir = strdup(cwd);
        else dir = strdup(".");
    }

    ast_project_t *proj = ast_introspect(dir);
    ast_call_graph(proj, func, result, rlen);
    ast_free_project(proj);
    free(func);
    free(dir);
    return true;
}

static bool tool_dependency_graph(const char *input, char *result, size_t rlen) {
    char *dir = json_get_str(input, "project_dir");
    if (!dir) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) dir = strdup(cwd);
        else dir = strdup(".");
    }

    ast_project_t *proj = ast_introspect(dir);
    ast_dependency_graph(proj, result, rlen);
    ast_free_project(proj);
    free(dir);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SUB-AGENT SWARM TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static int current_swarm_depth(void) {
    const char *d = getenv("DSCO_SWARM_DEPTH");
    return d ? atoi(d) : 0;
}

static bool tool_spawn_agent(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    /* Check hierarchical depth limit */
    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen,
                 "{\"error\":\"max swarm depth %d reached (current depth: %d). "
                 "Execute the task directly instead of delegating.\"}",
                 SWARM_MAX_DEPTH, depth);
        return false;
    }

    char *task = json_get_str(input, "task");
    char *model = json_get_str(input, "model");
    if (!task) {
        snprintf(result, rlen, "error: task required");
        free(model);
        return false;
    }

    int id = swarm_spawn(&g_swarm, task, model);
    if (id < 0) {
        snprintf(result, rlen, "{\"error\":\"failed to spawn agent (max %d)\"}",
                 SWARM_MAX_CHILDREN);
        free(task); free(model);
        return false;
    }

    swarm_child_t *c = swarm_get(&g_swarm, id);
    snprintf(result, rlen,
             "{\"agent_id\":%d,\"pid\":%d,\"task\":\"%s\",\"status\":\"running\","
             "\"hint\":\"Use agent_status to monitor, agent_output to get results\"}",
             id, (int)c->pid, task);

    char spawn_detail[256];
    snprintf(spawn_detail, sizeof(spawn_detail), "agent_id=%d pid=%d task=%s",
             id, (int)c->pid, task);
    baseline_log("swarm", "spawn_agent", spawn_detail, NULL);

    /* TUI feedback */
    int spawn_depth = current_swarm_depth() + 1;
    fprintf(stderr, "  %s⚡%s spawned agent #%d (depth %d): %s%.60s%s\n",
            TUI_BCYAN, TUI_RESET, id, spawn_depth, TUI_DIM, task, TUI_RESET);

    free(task); free(model);
    return true;
}

static bool tool_agent_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_swarm();

    /* Poll for updates first */
    swarm_poll(&g_swarm, 0);

    swarm_status_json(&g_swarm, result, rlen);

    /* TUI feedback */
    int active = swarm_active_count(&g_swarm);
    int total = g_swarm.child_count;
    if (total > 0) {
        fprintf(stderr, "  %s📊%s swarm: %d/%d active\n",
                TUI_BYELLOW, TUI_RESET, active, total);
        for (int i = 0; i < total; i++) {
            swarm_child_t *c = swarm_get(&g_swarm, i);
            const char *icon = "○";
            const char *color = TUI_DIM;
            if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING) {
                icon = "◉"; color = TUI_BCYAN;
            } else if (c->status == SWARM_DONE) {
                icon = "✓"; color = TUI_GREEN;
            } else if (c->status == SWARM_ERROR) {
                icon = "✗"; color = TUI_RED;
            }
            fprintf(stderr, "    %s%s%s #%d %s%.50s%s\n",
                    color, icon, TUI_RESET, i, TUI_DIM, c->task, TUI_RESET);
        }
    }

    return true;
}

static bool tool_agent_output(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int id = json_get_int(input, "id", -1);
    if (id < 0) {
        snprintf(result, rlen, "error: agent id required");
        return false;
    }

    /* Poll for latest data */
    swarm_poll(&g_swarm, 100);

    swarm_child_output(&g_swarm, id, result, rlen);
    return true;
}

static bool tool_agent_wait(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int id = json_get_int(input, "id", -1);
    int timeout = json_get_int(input, "timeout", 120);

    double deadline = 0;
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        deadline = tv.tv_sec + tv.tv_usec / 1e6 + timeout;
    }

    /* Poll loop with streaming output */
    while (1) {
        swarm_poll(&g_swarm, 500);

        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        double now = now_tv.tv_sec + now_tv.tv_usec / 1e6;
        if (now >= deadline) {
            snprintf(result, rlen, "{\"error\":\"timeout after %d seconds\"}", timeout);
            return false;
        }

        if (id >= 0) {
            swarm_child_t *c = swarm_get(&g_swarm, id);
            if (!c) {
                snprintf(result, rlen, "{\"error\":\"invalid agent id\"}");
                return false;
            }
            if (c->status == SWARM_DONE || c->status == SWARM_ERROR || c->status == SWARM_KILLED) {
                swarm_child_output(&g_swarm, id, result, rlen);
                return c->status == SWARM_DONE;
            }
        } else {
            /* Wait for all */
            if (swarm_active_count(&g_swarm) == 0 && g_swarm.child_count > 0) {
                swarm_status_json(&g_swarm, result, rlen);
                return true;
            }
        }
    }
}

static bool tool_agent_kill(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int id = json_get_int(input, "id", -1);
    if (id < 0) {
        snprintf(result, rlen, "error: agent id required");
        return false;
    }

    bool ok = swarm_kill(&g_swarm, id);
    if (ok)
        snprintf(result, rlen, "{\"killed\":true,\"agent_id\":%d}", id);
    else
        snprintf(result, rlen, "{\"error\":\"agent %d not running\"}", id);
    baseline_log(ok ? "swarm" : "swarm_error",
                 ok ? "agent_kill" : "agent_kill_failed", result, NULL);
    return ok;
}

static bool tool_create_swarm(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    /* Check hierarchical depth limit */
    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen,
                 "{\"error\":\"max swarm depth %d reached (current depth: %d). "
                 "Execute tasks directly instead of spawning more sub-agents.\"}",
                 SWARM_MAX_DEPTH, depth);
        return false;
    }

    char *name = json_get_str(input, "name");
    char *model = json_get_str(input, "model");
    if (!name) {
        snprintf(result, rlen, "error: name required");
        free(model);
        return false;
    }

    int gid = swarm_group_create(&g_swarm, name);
    if (gid < 0) {
        snprintf(result, rlen, "{\"error\":\"max groups reached\"}");
        free(name); free(model);
        return false;
    }

    /* Parse tasks array - we need to iterate the JSON array */
    char *tasks_raw = json_get_raw(input, "tasks");
    if (!tasks_raw || *tasks_raw != '[') {
        snprintf(result, rlen, "error: tasks array required");
        free(name); free(model); free(tasks_raw);
        return false;
    }

    /* Simple array parsing: collect task strings */
    const char *task_ptrs[SWARM_MAX_CHILDREN];
    char *task_strs[SWARM_MAX_CHILDREN];
    int task_count = 0;

    const char *p = tasks_raw + 1; /* skip [ */
    while (*p && *p != ']' && task_count < SWARM_MAX_CHILDREN) {
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') {
                if (*p == '\\') p++;
                p++;
            }
            int len = (int)(p - start);
            task_strs[task_count] = malloc(len + 1);
            memcpy(task_strs[task_count], start, len);
            task_strs[task_count][len] = '\0';
            task_ptrs[task_count] = task_strs[task_count];
            task_count++;
            if (*p == '"') p++;
        } else if (*p != ']') {
            p++;
        }
    }

    int spawned = swarm_group_dispatch(&g_swarm, gid, task_ptrs, task_count, model);

    char swarm_detail[256];
    snprintf(swarm_detail, sizeof(swarm_detail),
             "group_id=%d name=%s tasks=%d spawned=%d",
             gid, name, task_count, spawned);
    baseline_log("swarm", "create_swarm", swarm_detail, NULL);

    /* TUI feedback */
    fprintf(stderr, "\n  %s⚡%s Swarm %s\"%s\"%s created: %d agents launched\n",
            TUI_BYELLOW, TUI_RESET, TUI_BOLD, name, TUI_RESET, spawned);
    for (int i = 0; i < task_count; i++) {
        fprintf(stderr, "    %s◉%s %s%.60s%s\n",
                TUI_BCYAN, TUI_RESET, TUI_DIM, task_strs[i], TUI_RESET);
    }
    fprintf(stderr, "\n");

    /* Build result */
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"group_id\":");
    jbuf_append_int(&b, gid);
    jbuf_append(&b, ",\"name\":");
    jbuf_append_json_str(&b, name);
    jbuf_append(&b, ",\"agents_spawned\":");
    jbuf_append_int(&b, spawned);
    jbuf_append(&b, ",\"agent_ids\":[");
    swarm_group_t *g = &g_swarm.groups[gid];
    for (int i = 0; i < g->child_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        jbuf_append_int(&b, g->child_ids[i]);
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, written);
    result[written] = '\0';
    jbuf_free(&b);

    for (int i = 0; i < task_count; i++) free(task_strs[i]);
    free(tasks_raw); free(name); free(model);
    return true;
}

static bool tool_swarm_status(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int gid = json_get_int(input, "group_id", -1);
    if (gid < 0) {
        snprintf(result, rlen, "error: group_id required");
        return false;
    }

    swarm_group_status_json(&g_swarm, gid, result, rlen);
    return true;
}

static void swarm_collect_results(swarm_t *sw, int gid, char *result, size_t rlen,
                                   bool complete) {
    swarm_group_t *g = &sw->groups[gid];
    jbuf_t b;
    jbuf_init(&b, 8192);

    jbuf_append(&b, "{\"group\":");
    jbuf_append_json_str(&b, g->name);
    jbuf_append(&b, ",\"complete\":");
    jbuf_append(&b, complete ? "true" : "false");
    jbuf_append(&b, ",\"results\":[");

    for (int i = 0; i < g->child_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        swarm_child_t *c = &sw->children[g->child_ids[i]];
        jbuf_append(&b, "{\"id\":");
        jbuf_append_int(&b, c->id);
        jbuf_append(&b, ",\"task\":");
        jbuf_append_json_str(&b, c->task);
        jbuf_append(&b, ",\"status\":");
        jbuf_append_json_str(&b, swarm_status_str(c->status));
        jbuf_append(&b, ",\"exit_code\":");
        jbuf_append_int(&b, c->exit_code);
        double elapsed = (c->end_time > 0 ? c->end_time : now_sec_helper()) - c->start_time;
        char elapsed_str[32];
        snprintf(elapsed_str, sizeof(elapsed_str), "%.1f", elapsed);
        jbuf_append(&b, ",\"elapsed_sec\":");
        jbuf_append(&b, elapsed_str);
        jbuf_append(&b, ",\"output\":");
        /* Truncate very long outputs to keep the result under rlen */
        const char *out = c->output ? c->output : "";
        size_t olen = strlen(out);
        if (olen > 8192) {
            /* Last 8k chars for long outputs */
            char truncated[8256];
            snprintf(truncated, sizeof(truncated), "[...truncated %zu bytes...]\n%s",
                     olen - 8192, out + olen - 8192);
            jbuf_append_json_str(&b, truncated);
        } else {
            jbuf_append_json_str(&b, out);
        }
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, written);
    result[written] = '\0';
    jbuf_free(&b);
}

static bool tool_swarm_collect(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int gid = json_get_int(input, "group_id", -1);
    int timeout = json_get_int(input, "timeout", 300);

    if (gid < 0 || gid >= g_swarm.group_count) {
        snprintf(result, rlen, "{\"error\":\"invalid group_id\"}");
        return false;
    }

    swarm_group_t *grp = &g_swarm.groups[gid];
    double start = now_sec_helper();
    int last_done = -1;

    /* Wait for completion with live TUI status updates */
    while (!swarm_group_complete(&g_swarm, gid)) {
        swarm_poll(&g_swarm, 500);

        /* Show progress when agents finish */
        int done_count = 0;
        for (int i = 0; i < grp->child_count; i++) {
            swarm_child_t *c = &g_swarm.children[grp->child_ids[i]];
            if (c->status == SWARM_DONE || c->status == SWARM_ERROR ||
                c->status == SWARM_KILLED) {
                done_count++;
            }
        }
        if (done_count > last_done) {
            last_done = done_count;
            int active = grp->child_count - done_count;
            fprintf(stderr, "  %s⏳%s swarm \"%s\": %d/%d done, %d active (%.0fs)\n",
                    TUI_BYELLOW, TUI_RESET, grp->name,
                    done_count, grp->child_count, active,
                    now_sec_helper() - start);
        }

        double elapsed = now_sec_helper() - start;
        if (elapsed >= timeout) {
            fprintf(stderr, "  %s⚠%s swarm \"%s\" timed out after %.0fs\n",
                    TUI_BYELLOW, TUI_RESET, grp->name, elapsed);
            swarm_collect_results(&g_swarm, gid, result, rlen, false);
            return false;
        }
    }

    swarm_collect_results(&g_swarm, gid, result, rlen, true);

    /* TUI feedback */
    double elapsed = now_sec_helper() - start;
    int errors = 0;
    for (int i = 0; i < grp->child_count; i++) {
        if (g_swarm.children[grp->child_ids[i]].status == SWARM_ERROR) errors++;
    }
    if (errors > 0) {
        fprintf(stderr, "\n  %s⚠%s Swarm \"%s\": %d/%d completed, %d errors (%.1fs)\n",
                TUI_BYELLOW, TUI_RESET, grp->name,
                grp->child_count - errors, grp->child_count, errors, elapsed);
    } else {
        fprintf(stderr, "\n  %s✓%s Swarm \"%s\" complete (%d agents, %.1fs)\n",
                TUI_GREEN, TUI_RESET, grp->name, grp->child_count, elapsed);
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IPC TOOLS — inter-agent communication
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ensure_ipc(void) {
    static bool inited = false;
    if (!inited) {
        ipc_init(NULL, NULL);
        const char *depth_s = getenv("DSCO_SWARM_DEPTH");
        const char *parent = getenv("DSCO_PARENT_INSTANCE_ID");
        int depth = depth_s ? atoi(depth_s) : 0;
        ipc_register(parent, depth, "", "*");
        inited = true;
    }
}

static bool tool_ipc_send(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *to = json_get_str(input, "to");
    char *topic = json_get_str(input, "topic");
    char *body = json_get_str(input, "body");
    if (!body) {
        snprintf(result, rlen, "error: body required");
        free(to); free(topic); return false;
    }
    bool ok = ipc_send(to, topic ? topic : "general", body);
    if (ok) snprintf(result, rlen, "{\"sent\":true,\"to\":\"%s\",\"topic\":\"%s\"}",
                     to ? to : "(broadcast)", topic ? topic : "general");
    else snprintf(result, rlen, "{\"error\":\"send failed\"}");
    free(to); free(topic); free(body);
    return ok;
}

static bool tool_ipc_recv(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *topic = json_get_str(input, "topic");

    ipc_message_t msgs[32];
    int count = topic ? ipc_recv_topic(topic, msgs, 32) : ipc_recv(msgs, 32);

    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"count\":"); jbuf_append_int(&b, count);
    jbuf_append(&b, ",\"messages\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        jbuf_append(&b, "{\"from\":"); jbuf_append_json_str(&b, msgs[i].from_agent);
        jbuf_append(&b, ",\"topic\":"); jbuf_append_json_str(&b, msgs[i].topic);
        jbuf_append(&b, ",\"body\":"); jbuf_append_json_str(&b, msgs[i].body);
        jbuf_append(&b, "}");
        free(msgs[i].body);
    }
    jbuf_append(&b, "]}");
    int w = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, w); result[w] = '\0';
    jbuf_free(&b); free(topic);
    return true;
}

static bool tool_ipc_agents(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_ipc();
    ipc_status_json(result, rlen);
    return true;
}

static bool tool_ipc_scratch_put(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *key = json_get_str(input, "key");
    char *value = json_get_str(input, "value");
    if (!key || !value) {
        snprintf(result, rlen, "error: key and value required");
        free(key); free(value); return false;
    }
    bool ok = ipc_scratch_put(key, value);
    snprintf(result, rlen, ok ? "{\"stored\":\"%s\"}" : "{\"error\":\"write failed\"}", key);
    free(key); free(value);
    return ok;
}

static bool tool_ipc_scratch_get(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *key = json_get_str(input, "key");
    if (!key) { snprintf(result, rlen, "error: key required"); return false; }
    char *val = ipc_scratch_get(key);
    if (val) {
        jbuf_t b; jbuf_init(&b, 1024);
        jbuf_append(&b, "{\"key\":"); jbuf_append_json_str(&b, key);
        jbuf_append(&b, ",\"value\":"); jbuf_append_json_str(&b, val);
        jbuf_append(&b, "}");
        int w = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
        memcpy(result, b.data, w); result[w] = '\0';
        jbuf_free(&b); free(val);
    } else {
        snprintf(result, rlen, "{\"key\":\"%s\",\"value\":null}", key);
    }
    free(key);
    return true;
}

static bool tool_ipc_task_submit(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *desc = json_get_str(input, "description");
    int prio = json_get_int(input, "priority", 0);
    int parent = json_get_int(input, "parent_task_id", 0);
    if (!desc) { snprintf(result, rlen, "error: description required"); return false; }
    int id = ipc_task_submit(desc, prio, parent);
    if (id >= 0) snprintf(result, rlen, "{\"task_id\":%d,\"status\":\"pending\"}", id);
    else snprintf(result, rlen, "{\"error\":\"submit failed\"}");
    free(desc);
    return (id >= 0);
}

static bool tool_ipc_task_list(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *filter = json_get_str(input, "assigned_to");
    ipc_task_t tasks[64];
    int count = ipc_task_list(filter, tasks, 64);

    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"count\":"); jbuf_append_int(&b, count);
    jbuf_append(&b, ",\"tasks\":[");
    static const char *st_names[] = {"pending","assigned","running","done","failed"};
    for (int i = 0; i < count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        jbuf_append(&b, "{\"id\":"); jbuf_append_int(&b, tasks[i].id);
        jbuf_append(&b, ",\"status\":"); jbuf_append_json_str(&b, st_names[tasks[i].status]);
        jbuf_append(&b, ",\"priority\":"); jbuf_append_int(&b, tasks[i].priority);
        jbuf_append(&b, ",\"assigned_to\":"); jbuf_append_json_str(&b, tasks[i].assigned_to);
        jbuf_append(&b, ",\"description\":"); jbuf_append_json_str(&b, tasks[i].description);
        if (tasks[i].result) {
            jbuf_append(&b, ",\"result\":"); jbuf_append_json_str(&b, tasks[i].result);
            free(tasks[i].result);
        }
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");
    int w = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, w); result[w] = '\0';
    jbuf_free(&b); free(filter);
    return true;
}

static bool tool_ipc_set_role(const char *input, char *result, size_t rlen) {
    ensure_ipc();
    char *role = json_get_str(input, "role");
    if (!role) { snprintf(result, rlen, "error: role required"); return false; }
    ipc_set_status(IPC_AGENT_WORKING, role);
    snprintf(result, rlen, "{\"agent\":\"%s\",\"role\":\"%s\"}", ipc_self_id(), role);
    free(role);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CRYPTO TOOL IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_sha256(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = json_get_str(input, "file");
    char hex[65];

    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            snprintf(result, rlen, "error: cannot open %s", file);
            free(text); free(file);
            return false;
        }
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            sha256_update(&ctx, buf, n);
        fclose(f);
        uint8_t hash[32];
        sha256_final(&ctx, hash);
        hex_encode(hash, 32, hex);
        snprintf(result, rlen, "%s  %s", hex, file);
    } else if (text) {
        sha256_hex((const uint8_t *)text, strlen(text), hex);
        snprintf(result, rlen, "%s", hex);
    } else {
        snprintf(result, rlen, "error: provide text or file");
        return false;
    }
    free(text); free(file);
    return true;
}

static bool tool_md5(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = json_get_str(input, "file");
    char hex[33];

    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            snprintf(result, rlen, "error: cannot open %s", file);
            free(text); free(file);
            return false;
        }
        md5_ctx_t ctx;
        md5_init(&ctx);
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            md5_update(&ctx, buf, n);
        fclose(f);
        uint8_t hash[16];
        md5_final(&ctx, hash);
        hex_encode(hash, 16, hex);
        snprintf(result, rlen, "%s  %s", hex, file);
    } else if (text) {
        md5_hex((const uint8_t *)text, strlen(text), hex);
        snprintf(result, rlen, "%s", hex);
    } else {
        snprintf(result, rlen, "error: provide text or file");
        return false;
    }
    free(text); free(file);
    return true;
}

static bool tool_hmac(const char *input, char *result, size_t rlen) {
    char *key = json_get_str(input, "key");
    char *message = json_get_str(input, "message");
    if (!key || !message) {
        snprintf(result, rlen, "error: key and message required");
        free(key); free(message);
        return false;
    }
    char hex[65];
    hmac_sha256_hex((const uint8_t *)key, strlen(key),
                    (const uint8_t *)message, strlen(message), hex);
    snprintf(result, rlen, "%s", hex);
    free(key); free(message);
    return true;
}

static bool tool_uuid(const char *input, char *result, size_t rlen) {
    int count = json_get_int(input, "count", 1);
    if (count < 1) count = 1;
    if (count > 100) count = 100;
    size_t pos = 0;
    for (int i = 0; i < count && pos < rlen - 40; i++) {
        char u[37];
        uuid_v4(u);
        int n = snprintf(result + pos, rlen - pos, "%s%s", i > 0 ? "\n" : "", u);
        if (n > 0) pos += (size_t)n;
    }
    return true;
}

static bool tool_random_bytes(const char *input, char *result, size_t rlen) {
    int nbytes = json_get_int(input, "bytes", 32);
    if (nbytes < 1) nbytes = 1;
    if (nbytes > 1024) nbytes = 1024;
    char *format = json_get_str(input, "format");

    uint8_t *buf = malloc((size_t)nbytes);
    if (!crypto_random_bytes(buf, (size_t)nbytes)) {
        snprintf(result, rlen, "error: failed to generate random bytes");
        free(buf); free(format);
        return false;
    }

    if (format && strcmp(format, "base64") == 0) {
        base64_encode(buf, (size_t)nbytes, result, rlen);
    } else {
        hex_encode(buf, (size_t)nbytes, result);
    }
    free(buf); free(format);
    return true;
}

static bool tool_base64_tool(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "input");
    char *action = json_get_str(input, "action");
    bool url_safe = json_get_bool(input, "url_safe", false);

    if (!text) {
        snprintf(result, rlen, "error: input required");
        free(action);
        return false;
    }

    if (action && strcmp(action, "decode") == 0) {
        uint8_t *decoded = malloc(strlen(text) + 4);
        size_t len;
        if (url_safe)
            len = base64url_decode(text, strlen(text), decoded, strlen(text) + 4);
        else
            len = base64_decode(text, strlen(text), decoded, strlen(text) + 4);
        if (len < rlen) {
            memcpy(result, decoded, len);
            result[len] = '\0';
        }
        free(decoded);
    } else {
        if (url_safe)
            base64url_encode((const uint8_t *)text, strlen(text), result, rlen);
        else
            base64_encode((const uint8_t *)text, strlen(text), result, rlen);
    }
    free(text); free(action);
    return true;
}

static bool tool_jwt_decode(const char *input, char *result, size_t rlen) {
    char *token = json_get_str(input, "token");
    if (!token) {
        snprintf(result, rlen, "error: token required");
        return false;
    }
    char header[4096], payload[4096];
    if (!jwt_decode(token, header, sizeof(header), payload, sizeof(payload))) {
        snprintf(result, rlen, "error: invalid JWT format");
        free(token);
        return false;
    }
    snprintf(result, rlen, "Header:\n%s\n\nPayload:\n%s", header, payload);
    free(token);
    return true;
}

static bool tool_hkdf(const char *input, char *result, size_t rlen) {
    char *ikm_hex = json_get_str(input, "ikm");
    char *salt_hex = json_get_str(input, "salt");
    char *info_str = json_get_str(input, "info");
    int length = json_get_int(input, "length", 32);
    if (length > 256) length = 256;

    if (!ikm_hex) {
        snprintf(result, rlen, "error: ikm required");
        free(salt_hex); free(info_str);
        return false;
    }

    size_t ikm_len = strlen(ikm_hex) / 2;
    uint8_t *ikm = malloc(ikm_len + 1);
    hex_decode(ikm_hex, strlen(ikm_hex), ikm, ikm_len + 1);

    uint8_t *salt = NULL;
    size_t salt_len = 0;
    if (salt_hex) {
        salt_len = strlen(salt_hex) / 2;
        salt = malloc(salt_len + 1);
        hex_decode(salt_hex, strlen(salt_hex), salt, salt_len + 1);
    }

    uint8_t *okm = malloc((size_t)length);
    hkdf_sha256(ikm, ikm_len,
                salt, salt_len,
                (const uint8_t *)(info_str ? info_str : ""), info_str ? strlen(info_str) : 0,
                okm, (size_t)length);

    hex_encode(okm, (size_t)length, result);

    free(ikm); free(salt); free(okm);
    free(ikm_hex); free(salt_hex); free(info_str);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PIPELINE TOOL IMPLEMENTATION
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_pipeline(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "input");
    char *file = json_get_str(input, "file");
    char *spec = json_get_str(input, "spec");

    if (!spec) {
        snprintf(result, rlen, "error: spec required");
        free(text); free(file);
        return false;
    }

    /* Get input from text or file */
    char *data = NULL;
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f) {
            snprintf(result, rlen, "error: cannot open %s", file);
            free(text); free(file); free(spec);
            return false;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize > PIPE_MAX_OUTPUT) fsize = PIPE_MAX_OUTPUT;
        data = malloc((size_t)fsize + 1);
        size_t n = fread(data, 1, (size_t)fsize, f);
        data[n] = '\0';
        fclose(f);
    } else if (text) {
        data = text;
        text = NULL; /* prevent double-free */
    } else {
        snprintf(result, rlen, "error: input or file required");
        free(spec);
        return false;
    }

    char *output = pipeline_run(data, spec);
    if (output) {
        strncpy(result, output, rlen - 1);
        result[rlen - 1] = '\0';
        free(output);
    } else {
        snprintf(result, rlen, "(empty)");
    }

    free(data); free(text); free(file); free(spec);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EVAL TOOL IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_eval(const char *input, char *result, size_t rlen) {
    char *expr = json_get_str(input, "expression");
    if (!expr) {
        snprintf(result, rlen, "error: expression required");
        return false;
    }
    eval_ctx_t ctx;
    eval_init(&ctx);
    eval_multi(&ctx, expr, result, rlen);
    free(expr);
    return !ctx.has_error;
}

static bool tool_big_factorial(const char *input, char *result, size_t rlen) {
    int n = json_get_int(input, "n", 0);
    if (n < 0 || n > 500) {
        snprintf(result, rlen, "error: n must be 0-500");
        return false;
    }
    bigint_t r;
    bigint_factorial(n, &r);
    bigint_to_str(&r, result, rlen);
    return true;
}

static bool tool_context_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *tool_filter = json_get_str(input, "tool");
    int top_k = json_get_int(input, "top_k", CTX_SEARCH_DEFAULT_K);
    bool ok = ctx_search_render(query, tool_filter, top_k, result, rlen);
    free(query);
    free(tool_filter);
    return ok;
}

static bool tool_context_get(const char *input, char *result, size_t rlen) {
    int chunk_id = json_get_int(input, "chunk_id", -1);
    int max_chars = json_get_int(input, "max_chars", 4000);
    if (chunk_id < 0) {
        snprintf(result, rlen, "error: chunk_id required");
        return false;
    }
    if (max_chars < 200) max_chars = 200;
    if (max_chars > 24000) max_chars = 24000;

    int idx = ctx_find_index_by_id(chunk_id);
    if (idx < 0) {
        snprintf(result, rlen, "error: chunk_id %d not found", chunk_id);
        return false;
    }

    ctx_chunk_t *c = &g_ctx.chunks[idx];
    size_t copy_len = c->text_len;
    if (copy_len > (size_t)max_chars) copy_len = (size_t)max_chars;

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "chunk_id=%d tool=%s bytes=%zu\n---\n",
                     c->id, c->tool, c->text_len);
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        return true;
    }
    off += (size_t)n;

    if (off + copy_len + 64 >= rlen) {
        if (rlen > off + 64) {
            copy_len = rlen - off - 64;
        } else {
            copy_len = 0;
        }
    }
    if (copy_len > 0) {
        memcpy(result + off, c->text, copy_len);
        off += copy_len;
        result[off] = '\0';
    }
    if (copy_len < c->text_len) {
        snprintf(result + off, rlen - off, "\n\n... truncated (%zu/%zu chars shown)",
                 copy_len, c->text_len);
    }
    return true;
}

static bool tool_context_stats(const char *input, char *result, size_t rlen) {
    (void)input;
    if (g_ctx.count == 0) {
        snprintf(result, rlen, "context store is empty\n"
                               "offload_events=%zu offloaded_bytes=%zu reference_bytes=%zu estimated_tokens_saved=%zu",
                 g_ctx_offload_events, g_ctx_offloaded_bytes, g_ctx_reference_bytes,
                 (g_ctx_offloaded_bytes > g_ctx_reference_bytes)
                    ? (g_ctx_offloaded_bytes - g_ctx_reference_bytes) / 4
                    : 0);
        return true;
    }

    typedef struct {
        char tool[48];
        int chunks;
        size_t bytes;
    } tool_stat_t;

    tool_stat_t stats[32];
    int scount = 0;
    int pinned_count = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.chunks[i].pinned) pinned_count++;
        int found = -1;
        for (int j = 0; j < scount; j++) {
            if (strcmp(stats[j].tool, g_ctx.chunks[i].tool) == 0) {
                found = j;
                break;
            }
        }
        if (found < 0 && scount < (int)(sizeof(stats) / sizeof(stats[0]))) {
            found = scount++;
            memset(&stats[found], 0, sizeof(stats[found]));
            strncpy(stats[found].tool, g_ctx.chunks[i].tool, sizeof(stats[found].tool) - 1);
        }
        if (found >= 0) {
            stats[found].chunks++;
            stats[found].bytes += g_ctx.chunks[i].text_len;
        }
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "context chunks=%d bytes=%zu max_chunks=%d offload_threshold=%d pinned=%d\n"
                     "offload_events=%zu offloaded_bytes=%zu reference_bytes=%zu estimated_tokens_saved=%zu\n",
                     g_ctx.count, g_ctx.total_bytes, CTX_MAX_CHUNKS,
                     ctx_offload_threshold_bytes(), pinned_count,
                     g_ctx_offload_events, g_ctx_offloaded_bytes, g_ctx_reference_bytes,
                     (g_ctx_offloaded_bytes > g_ctx_reference_bytes)
                        ? (g_ctx_offloaded_bytes - g_ctx_reference_bytes) / 4
                        : 0);
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        return true;
    }
    off += (size_t)n;

    for (int i = 0; i < scount; i++) {
        n = snprintf(result + off, rlen - off, "  %s: %d chunks, %zu bytes\n",
                     stats[i].tool, stats[i].chunks, stats[i].bytes);
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }
    return true;
}

static bool tool_context_summarize(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *tool_filter = json_get_str(input, "tool");
    int top_k = json_get_int(input, "top_k", 4);
    int max_chars = json_get_int(input, "max_chars_per_chunk", 260);

    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        free(query);
        free(tool_filter);
        return false;
    }
    if (max_chars < 80) max_chars = 80;
    if (max_chars > 1200) max_chars = 1200;

    ctx_hit_t hits[CTX_SEARCH_MAX_K];
    int n_hits = ctx_rank_hits(query, tool_filter, top_k, hits, CTX_SEARCH_MAX_K);
    if (n_hits <= 0) {
        snprintf(result, rlen, "no hits for summary query: %s", query);
        free(query);
        free(tool_filter);
        return true;
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "context summary query=%s tool_filter=%s hits=%d\n",
                     query, (tool_filter && *tool_filter) ? tool_filter : "*", n_hits);
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        free(query);
        free(tool_filter);
        return true;
    }
    off += (size_t)n;

    for (int i = 0; i < n_hits; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[hits[i].idx];
        char preview[1400];
        ctx_preview(c->text, (size_t)max_chars, preview, sizeof(preview));
        n = snprintf(result + off, rlen - off,
                     "- [chunk:%d tool:%s score:%.3f] %s\n",
                     c->id, c->tool, hits[i].final_score, preview);
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }

    snprintf(result + off, rlen - off,
             "\nUse context_get on chunk ids above for verbatim details.");
    free(query);
    free(tool_filter);
    return true;
}

static bool tool_context_pin(const char *input, char *result, size_t rlen) {
    int chunk_id = json_get_int(input, "chunk_id", -1);
    bool pin = json_get_bool(input, "pin", true);
    if (chunk_id < 0) {
        snprintf(result, rlen, "error: chunk_id required");
        return false;
    }
    int idx = ctx_find_index_by_id(chunk_id);
    if (idx < 0) {
        snprintf(result, rlen, "error: chunk_id %d not found", chunk_id);
        return false;
    }
    g_ctx.chunks[idx].pinned = pin;
    snprintf(result, rlen, "chunk %d %s", chunk_id, pin ? "pinned" : "unpinned");
    return true;
}

static bool tool_context_gc(const char *input, char *result, size_t rlen) {
    int max_chunks = json_get_int(input, "max_chunks", CTX_MAX_CHUNKS);
    int max_bytes = json_get_int(input, "max_bytes", (int)CTX_MAX_TOTAL_BYTES);
    int keep_recent = json_get_int(input, "keep_recent", 64);

    if (max_chunks < 8) max_chunks = 8;
    if (max_chunks > CTX_MAX_CHUNKS) max_chunks = CTX_MAX_CHUNKS;
    if (max_bytes < 4096) max_bytes = 4096;
    if (max_bytes > (int)CTX_MAX_TOTAL_BYTES) max_bytes = (int)CTX_MAX_TOTAL_BYTES;
    if (keep_recent < 0) keep_recent = 0;

    int removed = 0;
    bool changed = true;
    while (changed &&
           (g_ctx.count > max_chunks || (int)g_ctx.total_bytes > max_bytes)) {
        changed = false;
        int cutoff = g_ctx.count - keep_recent;
        if (cutoff < 0) cutoff = 0;

        for (int i = 0; i < g_ctx.count; i++) {
            if (g_ctx.count <= max_chunks && (int)g_ctx.total_bytes <= max_bytes) break;
            if (i >= cutoff && keep_recent > 0) continue;
            if (g_ctx.chunks[i].pinned) continue;
            ctx_evict_index(i);
            removed++;
            changed = true;
            i--;
        }
    }

    ctx_recompute_df();
    snprintf(result, rlen,
             "context_gc removed=%d chunks=%d bytes=%zu (targets: max_chunks=%d max_bytes=%d keep_recent=%d)",
             removed, g_ctx.count, g_ctx.total_bytes, max_chunks, max_bytes, keep_recent);
    return true;
}

static bool tool_token_audit(const char *input, char *result, size_t rlen) {
    (void)input;
    size_t saved_bytes = (g_ctx_offloaded_bytes > g_ctx_reference_bytes)
                       ? (g_ctx_offloaded_bytes - g_ctx_reference_bytes)
                       : 0;
    size_t saved_tokens = saved_bytes / 4;
    snprintf(result, rlen,
             "token_audit\n"
             "offload_events=%zu\n"
             "offloaded_bytes=%zu\n"
             "reference_bytes=%zu\n"
             "estimated_bytes_saved=%zu\n"
             "estimated_tokens_saved=%zu\n"
             "context_chunks=%d\n"
             "context_bytes=%zu\n",
             g_ctx_offload_events,
             g_ctx_offloaded_bytes,
             g_ctx_reference_bytes,
             saved_bytes,
             saved_tokens,
             g_ctx.count,
             g_ctx.total_bytes);
    return true;
}

/* ── Workflow Graph Toolkit (lightweight DAG/checkpoint memory) ───────── */

#define WF_MAX_PLANS        64
#define WF_MAX_STEPS        40
#define WF_STEP_TEXT_MAX    220
#define WF_NOTE_MAX         180

typedef enum {
    WF_STEP_PENDING = 0,
    WF_STEP_IN_PROGRESS,
    WF_STEP_DONE,
    WF_STEP_BLOCKED
} wf_step_status_t;

typedef struct {
    int id;
    char name[96];
    int step_count;
    char steps[WF_MAX_STEPS][WF_STEP_TEXT_MAX];
    wf_step_status_t status[WF_MAX_STEPS];
    char notes[WF_MAX_STEPS][WF_NOTE_MAX];
    time_t created_at;
    bool active;
} wf_plan_t;

static wf_plan_t g_wf_plans[WF_MAX_PLANS];
static int g_wf_next_id = 1;

static wf_plan_t *wf_find(int id) {
    for (int i = 0; i < WF_MAX_PLANS; i++) {
        if (g_wf_plans[i].active && g_wf_plans[i].id == id) return &g_wf_plans[i];
    }
    return NULL;
}

static wf_plan_t *wf_alloc(void) {
    for (int i = 0; i < WF_MAX_PLANS; i++) {
        if (!g_wf_plans[i].active) {
            memset(&g_wf_plans[i], 0, sizeof(g_wf_plans[i]));
            g_wf_plans[i].active = true;
            g_wf_plans[i].id = g_wf_next_id++;
            g_wf_plans[i].created_at = time(NULL);
            return &g_wf_plans[i];
        }
    }
    return NULL;
}

static int wf_parse_steps(const char *raw, char out[WF_MAX_STEPS][WF_STEP_TEXT_MAX]) {
    if (!raw || !*raw) return 0;
    int count = 0;
    char tmp[8192];
    strncpy(tmp, raw, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *p = tmp;
    while (*p && count < WF_MAX_STEPS) {
        while (*p == '\n' || *p == ';' || *p == '\r' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '\n' && *p != ';') p++;
        char save = *p;
        *p = '\0';

        while (*start == ' ' || *start == '\t') start++;
        size_t len = strlen(start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r')) {
            start[--len] = '\0';
        }
        if (len > 0) {
            strncpy(out[count], start, WF_STEP_TEXT_MAX - 1);
            out[count][WF_STEP_TEXT_MAX - 1] = '\0';
            count++;
        }
        if (!save) break;
        p++;
    }
    return count;
}

static const char *wf_status_name(wf_step_status_t s) {
    switch (s) {
        case WF_STEP_PENDING: return "pending";
        case WF_STEP_IN_PROGRESS: return "in_progress";
        case WF_STEP_DONE: return "done";
        case WF_STEP_BLOCKED: return "blocked";
        default: return "unknown";
    }
}

static wf_step_status_t wf_parse_status(const char *s) {
    if (!s) return WF_STEP_PENDING;
    if (strcmp(s, "pending") == 0) return WF_STEP_PENDING;
    if (strcmp(s, "in_progress") == 0 || strcmp(s, "inprogress") == 0) return WF_STEP_IN_PROGRESS;
    if (strcmp(s, "done") == 0 || strcmp(s, "complete") == 0) return WF_STEP_DONE;
    if (strcmp(s, "blocked") == 0) return WF_STEP_BLOCKED;
    return WF_STEP_PENDING;
}

static bool tool_workflow_plan(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    char *steps_raw = json_get_str(input, "steps");
    if (!steps_raw || !*steps_raw) {
        snprintf(result, rlen, "error: steps required (newline/semicolon separated)");
        free(name);
        free(steps_raw);
        return false;
    }

    wf_plan_t *wf = wf_alloc();
    if (!wf) {
        snprintf(result, rlen, "error: workflow capacity reached (%d)", WF_MAX_PLANS);
        free(name);
        free(steps_raw);
        return false;
    }
    strncpy(wf->name, (name && *name) ? name : "workflow", sizeof(wf->name) - 1);
    wf->step_count = wf_parse_steps(steps_raw, wf->steps);
    for (int i = 0; i < wf->step_count; i++) wf->status[i] = WF_STEP_PENDING;

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "workflow created id=%d name=%s steps=%d\n",
                     wf->id, wf->name, wf->step_count);
    if (n < 0) n = 0;
    if ((size_t)n < rlen - off) off += (size_t)n;
    for (int i = 0; i < wf->step_count && off < rlen - 4; i++) {
        n = snprintf(result + off, rlen - off, "  %d. %s [%s]\n",
                     i + 1, wf->steps[i], wf_status_name(wf->status[i]));
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }

    free(name);
    free(steps_raw);
    return true;
}

static bool tool_workflow_status(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    size_t off = 0;
    if (id >= 0) {
        wf_plan_t *wf = wf_find(id);
        if (!wf) {
            snprintf(result, rlen, "error: workflow id %d not found", id);
            return false;
        }
        int n = snprintf(result + off, rlen - off,
                         "workflow id=%d name=%s steps=%d\n",
                         wf->id, wf->name, wf->step_count);
        if (n < 0) n = 0;
        if ((size_t)n < rlen - off) off += (size_t)n;
        for (int i = 0; i < wf->step_count && off < rlen - 8; i++) {
            n = snprintf(result + off, rlen - off, "  %d. [%s] %s%s%s\n",
                         i + 1,
                         wf_status_name(wf->status[i]),
                         wf->steps[i],
                         wf->notes[i][0] ? " - note: " : "",
                         wf->notes[i][0] ? wf->notes[i] : "");
            if (n < 0 || (size_t)n >= rlen - off) break;
            off += (size_t)n;
        }
        return true;
    }

    int listed = 0;
    for (int i = 0; i < WF_MAX_PLANS && off < rlen - 64; i++) {
        if (!g_wf_plans[i].active) continue;
        listed++;
        int done = 0;
        for (int s = 0; s < g_wf_plans[i].step_count; s++) {
            if (g_wf_plans[i].status[s] == WF_STEP_DONE) done++;
        }
        int n = snprintf(result + off, rlen - off,
                         "id=%d name=%s progress=%d/%d\n",
                         g_wf_plans[i].id, g_wf_plans[i].name, done, g_wf_plans[i].step_count);
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }
    if (listed == 0) snprintf(result, rlen, "no workflows");
    return true;
}

static bool tool_workflow_checkpoint(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    int step = json_get_int(input, "step", -1);
    char *status = json_get_str(input, "status");
    char *note = json_get_str(input, "note");
    if (id < 0 || step < 1) {
        snprintf(result, rlen, "error: id and step (1-based) required");
        free(status);
        free(note);
        return false;
    }
    wf_plan_t *wf = wf_find(id);
    if (!wf) {
        snprintf(result, rlen, "error: workflow id %d not found", id);
        free(status);
        free(note);
        return false;
    }
    if (step > wf->step_count) {
        snprintf(result, rlen, "error: step out of range (1-%d)", wf->step_count);
        free(status);
        free(note);
        return false;
    }
    int idx = step - 1;
    wf->status[idx] = wf_parse_status(status);
    if (note) {
        strncpy(wf->notes[idx], note, WF_NOTE_MAX - 1);
        wf->notes[idx][WF_NOTE_MAX - 1] = '\0';
    }
    snprintf(result, rlen, "workflow %d step %d set to %s",
             id, step, wf_status_name(wf->status[idx]));
    free(status);
    free(note);
    return true;
}

static bool tool_workflow_resume(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    if (id < 0) {
        snprintf(result, rlen, "error: id required");
        return false;
    }
    wf_plan_t *wf = wf_find(id);
    if (!wf) {
        snprintf(result, rlen, "error: workflow id %d not found", id);
        return false;
    }
    for (int i = 0; i < wf->step_count; i++) {
        if (wf->status[i] == WF_STEP_PENDING || wf->status[i] == WF_STEP_BLOCKED) {
            snprintf(result, rlen, "resume workflow %d at step %d: %s [%s]",
                     wf->id, i + 1, wf->steps[i], wf_status_name(wf->status[i]));
            return true;
        }
    }
    snprintf(result, rlen, "workflow %d has no pending steps", wf->id);
    return true;
}

/* ── Browser + Research + Code + Sandbox + Policy Toolkit slice ───────── */

static int fetch_url_to_buffer(const char *url, int timeout, char *out, size_t out_len) {
    if (!url || !*url) {
        snprintf(out, out_len, "error: url required");
        return -1;
    }
    if (timeout < 1) timeout = 1;
    if (timeout > 90) timeout = 90;

    jbuf_t cmd;
    jbuf_init(&cmd, 512);
    jbuf_append(&cmd, "curl -sS -L --max-time ");
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", timeout);
    jbuf_append(&cmd, nbuf);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, url);

    int status = run_cmd(cmd.data, out, out_len);
    jbuf_free(&cmd);
    return status;
}

static void extract_html_title(const char *html, char *title, size_t title_len) {
    if (!title || title_len == 0) return;
    title[0] = '\0';
    if (!html) return;

    const char *p = strstr(html, "<title");
    if (!p) p = strstr(html, "<TITLE");
    if (!p) return;
    p = strchr(p, '>');
    if (!p) return;
    p++;
    const char *q = strstr(p, "</title>");
    if (!q) q = strstr(p, "</TITLE>");
    if (!q || q <= p) return;

    size_t n = (size_t)(q - p);
    if (n >= title_len) n = title_len - 1;
    memcpy(title, p, n);
    title[n] = '\0';
    for (size_t i = 0; title[i]; i++) {
        if (title[i] == '\n' || title[i] == '\r' || title[i] == '\t') title[i] = ' ';
    }
}

static bool tool_browser_snapshot(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    int timeout = json_get_int(input, "timeout", 20);
    int max_chars = json_get_int(input, "max_chars", 14000);
    if (!url) {
        snprintf(result, rlen, "error: url required");
        return false;
    }
    if (max_chars < 1000) max_chars = 1000;
    if (max_chars > 60000) max_chars = 60000;

    char *html = malloc((size_t)max_chars + 1);
    if (!html) {
        free(url);
        snprintf(result, rlen, "error: out of memory");
        return false;
    }
    html[0] = '\0';
    int status = fetch_url_to_buffer(url, timeout, html, (size_t)max_chars + 1);
    if (status != 0) {
        snprintf(result, rlen, "browser_snapshot fetch failed (status %d): %s", status, html);
        free(url);
        free(html);
        return false;
    }

    ctx_ingest_info_t info;
    ctx_ingest_text("browser_snapshot", html, &info);

    char title[256];
    extract_html_title(html, title, sizeof(title));
    char preview[260];
    ctx_preview(html, 200, preview, sizeof(preview));
    snprintf(result, rlen,
             "browser_snapshot url=%s\n"
             "title=%s\n"
             "stored_chunks=%d chunk_id_range=%d-%d bytes_indexed=%zu\n"
             "preview=%s\n"
             "next: browser_extract or context_search {\"tool\":\"browser_snapshot\",...}",
             url,
             title[0] ? title : "(none)",
             info.chunks_added,
             info.first_chunk_id,
             info.last_chunk_id,
             info.bytes_added,
             preview[0] ? preview : "(empty)");

    free(url);
    free(html);
    return true;
}

static bool tool_browser_extract(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    int top_k = json_get_int(input, "top_k", 5);
    bool ok = ctx_search_render(query, "browser_snapshot", top_k, result, rlen);
    free(query);
    return ok;
}

static bool tool_research_probe(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *query = json_get_str(input, "query");
    int timeout = json_get_int(input, "timeout", 25);
    int top_k = json_get_int(input, "top_k", 4);
    if (!url) {
        snprintf(result, rlen, "error: url required");
        free(query);
        return false;
    }

    char html[MAX_TOOL_RESULT];
    html[0] = '\0';
    int status = fetch_url_to_buffer(url, timeout, html, sizeof(html));
    if (status != 0) {
        snprintf(result, rlen, "research_probe fetch failed (status %d): %s", status, html);
        free(url);
        free(query);
        return false;
    }

    ctx_ingest_info_t info;
    ctx_ingest_text("research_probe", html, &info);

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "research_probe url=%s stored_chunks=%d chunk_id_range=%d-%d bytes_indexed=%zu\n",
                     url, info.chunks_added, info.first_chunk_id, info.last_chunk_id, info.bytes_added);
    if (n < 0) n = 0;
    if ((size_t)n < rlen - off) off += (size_t)n;

    if (query && *query && off < rlen - 64) {
        char sub[MAX_TOOL_RESULT];
        sub[0] = '\0';
        ctx_search_render(query, "research_probe", top_k, sub, sizeof(sub));
        n = snprintf(result + off, rlen - off, "\n%s", sub);
        if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
    }

    free(url);
    free(query);
    return true;
}

static bool tool_research_compare(const char *input, char *result, size_t rlen) {
    char *a = json_get_str(input, "text_a");
    char *b = json_get_str(input, "text_b");
    if (!a || !b) {
        snprintf(result, rlen, "error: text_a and text_b required");
        free(a);
        free(b);
        return false;
    }

    char ta[64][32];
    char tb[64][32];
    int na = ctx_extract_terms(a, ta, 64);
    int nb = ctx_extract_terms(b, tb, 64);
    int inter = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (strcmp(ta[i], tb[j]) == 0) {
                inter++;
                break;
            }
        }
    }
    int uni = na + nb - inter;
    double jacc = (uni > 0) ? ((double)inter / (double)uni) : 1.0;

    snprintf(result, rlen,
             "research_compare\n"
             "tokens_a=%d tokens_b=%d intersection=%d union=%d jaccard=%.3f\n"
             "length_a=%zu length_b=%zu\n",
             na, nb, inter, uni, jacc, strlen(a), strlen(b));
    free(a);
    free(b);
    return true;
}

static bool file_has_binary_nul(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') return true;
    }
    return false;
}

static bool tool_code_index(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    int max_files = json_get_int(input, "max_files", 200);
    int max_chars = json_get_int(input, "max_chars_per_file", 6000);
    if (!path) path = strdup(".");
    if (max_files < 1) max_files = 1;
    if (max_files > 800) max_files = 800;
    if (max_chars < 400) max_chars = 400;
    if (max_chars > 20000) max_chars = 20000;

    jbuf_t cmd;
    jbuf_init(&cmd, 512);
    jbuf_append(&cmd, "cd ");
    shell_quote(&cmd, path);
    jbuf_append(&cmd, " && ((rg --files 2>/dev/null) || (find . -type f 2>/dev/null)) | head -");
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", max_files);
    jbuf_append(&cmd, nbuf);

    char listing[MAX_TOOL_RESULT];
    listing[0] = '\0';
    int status = run_cmd(cmd.data, listing, sizeof(listing));
    jbuf_free(&cmd);
    if (status != 0 || listing[0] == '\0') {
        snprintf(result, rlen, "code_index failed to enumerate files in %s", path);
        free(path);
        return false;
    }

    int files_indexed = 0;
    int chunks_indexed = 0;
    size_t bytes_indexed = 0;
    char *line = listing;
    while (*line && files_indexed < max_files) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';
        char *rel = line;
        while (*rel == ' ' || *rel == '\t') rel++;
        if (rel[0] == '.' && rel[1] == '/') rel += 2;

        if (*rel) {
            char full[4096];
            if (strcmp(path, ".") == 0) {
                snprintf(full, sizeof(full), "%s", rel);
            } else {
                snprintf(full, sizeof(full), "%s/%s", path, rel);
            }

            FILE *f = fopen(full, "rb");
            if (f) {
                char *buf = malloc((size_t)max_chars + 1);
                if (buf) {
                    size_t nread = fread(buf, 1, (size_t)max_chars, f);
                    buf[nread] = '\0';
                    if (nread > 0 && !file_has_binary_nul(buf, nread)) {
                        char *chunk = malloc(nread + strlen(full) + 32);
                        if (chunk) {
                            snprintf(chunk, nread + strlen(full) + 32, "file: %s\n%s", full, buf);
                            ctx_ingest_info_t info;
                            ctx_ingest_text("code_index", chunk, &info);
                            if (info.chunks_added > 0) {
                                files_indexed++;
                                chunks_indexed += info.chunks_added;
                                bytes_indexed += info.bytes_added;
                            }
                            free(chunk);
                        }
                    }
                    free(buf);
                }
                fclose(f);
            }
        }

        if (!eol) break;
        line = eol + 1;
    }

    snprintf(result, rlen,
             "code_index path=%s files_indexed=%d chunks_indexed=%d bytes_indexed=%zu\n"
             "next: code_search with a focused query",
             path, files_indexed, chunks_indexed, bytes_indexed);
    free(path);
    return true;
}

static bool tool_code_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    int top_k = json_get_int(input, "top_k", 6);
    bool ok = ctx_search_render(query, "code_index", top_k, result, rlen);
    free(query);
    return ok;
}

static bool tool_sandbox_run(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    char *image = json_get_str(input, "image");
    int timeout = json_get_int(input, "timeout", 60);
    if (!command) {
        snprintf(result, rlen, "error: command required");
        free(image);
        return false;
    }
    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;

    char probe[64];
    int has_docker = (run_cmd("command -v docker >/dev/null 2>&1", probe, sizeof(probe)) == 0);
    run_opts_t opts = RUN_OPTS_DEFAULT;
    opts.wall_timeout_s = timeout;
    opts.idle_timeout_s = timeout > 120 ? 90 : timeout;
    opts.stream_to_tty = true;
    opts.dim_output = true;
    opts.label = "sandbox_run";

    char *escaped = shell_escape(command);
    jbuf_t cmd;
    jbuf_init(&cmd, 1024);
    if (image && *image && has_docker) {
        jbuf_append(&cmd, "docker run --rm --network none -v \"$PWD\":/workspace -w /workspace ");
        shell_quote(&cmd, image);
        jbuf_append(&cmd, " sh -lc '");
        jbuf_append(&cmd, escaped);
        jbuf_append(&cmd, "'");
    } else {
        jbuf_append(&cmd, "env -i HOME=\"$HOME\" PATH=\"/usr/bin:/bin:/usr/sbin:/sbin\" sh -lc '");
        jbuf_append(&cmd, escaped);
        jbuf_append(&cmd, "'");
    }

    int status = run_cmd_ex(cmd.data, result, rlen, &opts);
    if (status != 0 && strlen(result) == 0) {
        snprintf(result, rlen, "sandbox_run failed with status %d", status);
    }
    jbuf_free(&cmd);
    free(escaped);
    free(command);
    free(image);
    return (status == 0);
}

static bool token_is_email(const char *tok, size_t len) {
    if (len < 5 || len > 255) return false;
    const char *at = memchr(tok, '@', len);
    if (!at || at == tok || at == tok + len - 1) return false;
    const char *dot = memchr(at + 1, '.', (size_t)((tok + len) - (at + 1)));
    if (!dot || dot == at + 1 || dot == tok + len - 1) return false;
    return true;
}

static bool token_is_phone_like(const char *tok, size_t len) {
    int digits = 0;
    for (size_t i = 0; i < len; i++) {
        if (isdigit((unsigned char)tok[i])) digits++;
        else if (tok[i] == '+' || tok[i] == '-' || tok[i] == '(' || tok[i] == ')' || tok[i] == ' ' || tok[i] == '.') {}
        else return false;
    }
    return digits >= 10;
}

static bool tool_privacy_filter(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    if (!text) {
        snprintf(result, rlen, "error: text required");
        return false;
    }
    size_t off = 0;
    const char *p = text;
    while (*p && off + 1 < rlen) {
        if (isspace((unsigned char)*p)) {
            result[off++] = *p++;
            continue;
        }
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        bool redact_email = token_is_email(start, len);
        bool redact_phone = !redact_email && token_is_phone_like(start, len);
        const char *rep = redact_email ? "[REDACTED_EMAIL]"
                        : redact_phone ? "[REDACTED_PHONE]" : NULL;
        if (rep) {
            size_t rl = strlen(rep);
            if (off + rl >= rlen) break;
            memcpy(result + off, rep, rl);
            off += rl;
        } else {
            if (off + len >= rlen) break;
            memcpy(result + off, start, len);
            off += len;
        }
    }
    result[off] = '\0';
    free(text);
    return true;
}

static bool line_has_secret_pattern(const char *line) {
    if (!line) return false;
    if (strstr(line, "BEGIN PRIVATE KEY")) return true;
    if (strstr(line, "AKIA") && strlen(line) >= 20) return true;
    if (strstr(line, "sk-") && strlen(line) >= 18) return true;
    if (strstr(line, "xoxb-") || strstr(line, "xoxp-")) return true;
    if (strstr(line, "password=") || strstr(line, "passwd=")) return true;
    if (strstr(line, "api_key") || strstr(line, "secret_key")) return true;
    return false;
}

static bool tool_secret_scan(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = json_get_str(input, "file");
    char *owned = NULL;
    const char *scan = text;

    if (!scan && file) {
        FILE *f = fopen(file, "rb");
        if (!f) {
            snprintf(result, rlen, "error: cannot open file %s", file);
            free(text);
            free(file);
            return false;
        }
        owned = malloc(MAX_TOOL_RESULT);
        if (!owned) {
            fclose(f);
            snprintf(result, rlen, "error: out of memory");
            free(text);
            free(file);
            return false;
        }
        size_t n = fread(owned, 1, MAX_TOOL_RESULT - 1, f);
        owned[n] = '\0';
        fclose(f);
        scan = owned;
    }

    if (!scan) {
        snprintf(result, rlen, "error: text or file required");
        free(text);
        free(file);
        free(owned);
        return false;
    }

    int findings = 0;
    size_t off = 0;
    int line_no = 1;
    const char *line = scan;
    while (*line && off + 64 < rlen) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char tmp[1024];
        size_t cpy = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        memcpy(tmp, line, cpy);
        tmp[cpy] = '\0';
        if (line_has_secret_pattern(tmp)) {
            findings++;
            char prev[220];
            ctx_preview(tmp, 160, prev, sizeof(prev));
            int n = snprintf(result + off, rlen - off,
                             "line %d: %s\n", line_no, prev);
            if (n < 0 || (size_t)n >= rlen - off) break;
            off += (size_t)n;
        }
        if (!eol) break;
        line = eol + 1;
        line_no++;
    }

    if (findings == 0) {
        snprintf(result, rlen, "secret_scan: no obvious secrets detected");
    } else {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "secret_scan findings=%d\n", findings);
        size_t hlen = strlen(hdr);
        if (hlen + strlen(result) + 1 < rlen) {
            memmove(result + hlen, result, strlen(result) + 1);
            memcpy(result, hdr, hlen);
        }
    }

    free(text);
    free(file);
    free(owned);
    return true;
}

static bool tool_risk_gate(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    char *content = json_get_str(input, "content");
    if (!action) {
        snprintf(result, rlen, "error: action required");
        free(content);
        return false;
    }

    int score = 0;
    char reasons[512];
    reasons[0] = '\0';
    if (strstr(action, "rm -rf") || strstr(action, "delete") || strstr(action, "drop ")) {
        score += 5; strncat(reasons, "destructive_action;", sizeof(reasons) - strlen(reasons) - 1);
    }
    if (strstr(action, "sudo") || strstr(action, "ssh") || strstr(action, "docker")) {
        score += 2; strncat(reasons, "privileged_execution;", sizeof(reasons) - strlen(reasons) - 1);
    }
    if (content && *content) {
        if (strstr(content, "BEGIN PRIVATE KEY") || strstr(content, "AKIA") || strstr(content, "sk-")) {
            score += 4; strncat(reasons, "possible_secret_material;", sizeof(reasons) - strlen(reasons) - 1);
        }
        if (strstr(content, "@") && strstr(content, ".")) {
            score += 1; strncat(reasons, "possible_pii;", sizeof(reasons) - strlen(reasons) - 1);
        }
    }

    const char *decision = (score >= 7) ? "deny" : (score >= 4) ? "review" : "allow";
    snprintf(result, rlen, "risk_gate action=%s score=%d decision=%s reasons=%s",
             action, score, decision, reasons[0] ? reasons : "none");
    free(action);
    free(content);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PLUGIN TOOL IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool tool_plugin_list(const char *input, char *result, size_t rlen) {
    (void)input;
    plugin_list(&g_plugins, result, rlen);
    return true;
}

static bool tool_plugin_reload(const char *input, char *result, size_t rlen) {
    (void)input;
    plugin_reload(&g_plugins);
    snprintf(result, rlen, "Plugins reloaded. %d plugins loaded, %d extra tools.",
             g_plugins.count, g_plugins.extra_tool_count);
    return true;
}

static bool tool_plugin_load_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) {
        snprintf(result, rlen, "error: path required");
        return false;
    }
    bool ok = plugin_load(&g_plugins, path);
    if (ok)
        snprintf(result, rlen, "Plugin loaded: %s (%d tools)",
                 g_plugins.plugins[g_plugins.count - 1].name,
                 g_plugins.plugins[g_plugins.count - 1].tool_count);
    else
        snprintf(result, rlen, "error: failed to load plugin from %s", path);
    free(path);
    return ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOOL REGISTRY
 * ═══════════════════════════════════════════════════════════════════════════ */

static const tool_def_t s_tools[] = {
    /* ── File Tools ──────────────────────────────────────────────────────── */
    {
        .name = "write_file",
        .description = "Create or overwrite a file with the given content. Creates parent directories automatically.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path to write\"},\"content\":{\"type\":\"string\",\"description\":\"File content\"}},\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file
    },
    {
        .name = "read_file",
        .description = "Read a file with line numbers. Use offset/limit for large files.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"offset\":{\"type\":\"integer\",\"description\":\"Start at this line number (1-based)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to read (0=all)\"}},\"required\":[\"path\"]}",
        .execute = tool_read_file
    },
    {
        .name = "page_file",
        .description = "Page through a file. Returns one page at a time with line numbers and page info.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"page\":{\"type\":\"integer\",\"description\":\"Page number (1-based, default 1)\"},\"page_size\":{\"type\":\"integer\",\"description\":\"Lines per page (default 50, max 200)\"}},\"required\":[\"path\"]}",
        .execute = tool_page_file
    },
    {
        .name = "edit_file",
        .description = "Edit a file by replacing old_string with new_string. The old_string must uniquely match unless replace_all is true.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to find\"},\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"},\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace all occurrences (default false)\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file
    },
    {
        .name = "append_file",
        .description = "Append content to the end of a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"Content to append\"}},\"required\":[\"path\",\"content\"]}",
        .execute = tool_append_file
    },
    {
        .name = "list_directory",
        .description = "List files and subdirectories in a directory.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path (default: current directory)\"}}}",
        .execute = tool_list_dir
    },
    {
        .name = "find_files",
        .description = "Recursively find files matching a name pattern (glob).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Filename glob pattern (e.g. *.c)\"},\"path\":{\"type\":\"string\",\"description\":\"Root directory (default: .)\"}},\"required\":[\"pattern\"]}",
        .execute = tool_find_files
    },
    {
        .name = "grep_files",
        .description = "Search for a pattern in file contents recursively.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Search pattern (regex)\"},\"path\":{\"type\":\"string\",\"description\":\"Root directory (default: .)\"}},\"required\":[\"pattern\"]}",
        .execute = tool_grep
    },
    {
        .name = "file_info",
        .description = "Get file metadata (size, type, permissions).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},\"required\":[\"path\"]}",
        .execute = tool_file_info
    },
    {
        .name = "move_file",
        .description = "Move or rename a file or directory.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Source path\"},\"destination\":{\"type\":\"string\",\"description\":\"Destination path\"}},\"required\":[\"source\",\"destination\"]}",
        .execute = tool_move_file
    },
    {
        .name = "copy_file",
        .description = "Copy a file or directory (recursive).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Source path\"},\"destination\":{\"type\":\"string\",\"description\":\"Destination path\"}},\"required\":[\"source\",\"destination\"]}",
        .execute = tool_copy_file
    },
    {
        .name = "delete_file",
        .description = "Delete a file or directory.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to delete\"},\"recursive\":{\"type\":\"boolean\",\"description\":\"Delete directories recursively (default false)\"}},\"required\":[\"path\"]}",
        .execute = tool_delete_file
    },
    {
        .name = "mkdir",
        .description = "Create a directory (and parents).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path\"}},\"required\":[\"path\"]}",
        .execute = tool_mkdir
    },
    {
        .name = "chmod",
        .description = "Change file permissions.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"mode\":{\"type\":\"string\",\"description\":\"Permission mode (e.g. 755, +x, u+rw)\"}},\"required\":[\"path\",\"mode\"]}",
        .execute = tool_chmod
    },
    {
        .name = "tree",
        .description = "Show directory tree structure.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Root path (default .)\"},\"max_depth\":{\"type\":\"integer\",\"description\":\"Max depth (default 3)\"}}}",
        .execute = tool_tree
    },
    {
        .name = "wc",
        .description = "Count lines, words, and bytes in a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},\"required\":[\"path\"]}",
        .execute = tool_wc
    },
    {
        .name = "head",
        .description = "Show the first N lines of a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"lines\":{\"type\":\"integer\",\"description\":\"Number of lines (default 20)\"}},\"required\":[\"path\"]}",
        .execute = tool_head
    },
    {
        .name = "tail",
        .description = "Show the last N lines of a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"lines\":{\"type\":\"integer\",\"description\":\"Number of lines (default 20)\"}},\"required\":[\"path\"]}",
        .execute = tool_tail
    },
    {
        .name = "symlink",
        .description = "Create a symbolic link.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"Target path\"},\"link_path\":{\"type\":\"string\",\"description\":\"Link path\"}},\"required\":[\"target\",\"link_path\"]}",
        .execute = tool_symlink
    },
    /* ── Compile & Run ───────────────────────────────────────────────────── */
    {
        .name = "compile",
        .description = "Compile a C source file. Uses cc (system compiler).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Source file path\"},\"output\":{\"type\":\"string\",\"description\":\"Output binary name (default: a.out)\"},\"flags\":{\"type\":\"string\",\"description\":\"Additional compiler flags (e.g. -lm -lpthread)\"}},\"required\":[\"source\"]}",
        .execute = tool_compile
    },
    {
        .name = "bash",
        .description = "Execute a bash command and return stdout+stderr. Supports multi-line scripts, pipes, redirections. Preferred for all shell operations.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Bash command or script to run. Supports pipes, redirections, multi-line.\"},\"cwd\":{\"type\":\"string\",\"description\":\"Working directory (optional)\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 120, max 600)\"}},\"required\":[\"command\"]}",
        .execute = tool_bash
    },
    {
        .name = "run_command",
        .description = "Execute a shell command and return stdout+stderr. Use for running compiled programs, scripts, or any system command.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}},\"required\":[\"command\"]}",
        .execute = tool_run_command
    },
    {
        .name = "run_background",
        .description = "Run a command in the background. Returns the PID.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command to run in background\"}},\"required\":[\"command\"]}",
        .execute = tool_run_background
    },
    /* ── Retrieval Context ───────────────────────────────────────────────── */
    {
        .name = "context_search",
        .description = "Search the chunked retrieval context (built automatically from large tool outputs) using dense embedding retrieval + lexical reranking.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Focused retrieval query\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool-name filter\"},\"top_k\":{\"type\":\"integer\",\"description\":\"How many hits to return (default 5, max 12)\"}},\"required\":[\"query\"]}",
        .execute = tool_context_search
    },
    {
        .name = "context_get",
        .description = "Fetch full text of a previously indexed retrieval chunk by chunk_id.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"chunk_id\":{\"type\":\"integer\",\"description\":\"Chunk ID from context_search\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Max chars to return (default 4000, max 24000)\"}},\"required\":[\"chunk_id\"]}",
        .execute = tool_context_get
    },
    {
        .name = "context_stats",
        .description = "Show retrieval context store stats and chunk counts by source tool.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_context_stats
    },
    {
        .name = "context_summarize",
        .description = "Build a compact evidence summary from retrieval hits for a query (with chunk citations).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Summary query\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool filter\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Top chunks to summarize (default 4)\"},\"max_chars_per_chunk\":{\"type\":\"integer\",\"description\":\"Preview size per chunk\"}},\"required\":[\"query\"]}",
        .execute = tool_context_summarize
    },
    {
        .name = "context_pin",
        .description = "Pin or unpin a context chunk to protect it from context_gc pruning.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"chunk_id\":{\"type\":\"integer\",\"description\":\"Chunk ID\"},\"pin\":{\"type\":\"boolean\",\"description\":\"true to pin, false to unpin (default true)\"}},\"required\":[\"chunk_id\"]}",
        .execute = tool_context_pin
    },
    {
        .name = "context_gc",
        .description = "Garbage-collect retrieval context by max_chunks/max_bytes while preserving pinned and recent chunks.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"max_chunks\":{\"type\":\"integer\",\"description\":\"Target max chunks\"},\"max_bytes\":{\"type\":\"integer\",\"description\":\"Target max bytes\"},\"keep_recent\":{\"type\":\"integer\",\"description\":\"Always keep newest N chunks\"}}}",
        .execute = tool_context_gc
    },
    {
        .name = "token_audit",
        .description = "Report token-efficiency metrics from large-result offloading and retrieval context.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_token_audit
    },
    /* ── Browser-Grade Perception (foundation slice) ────────────────────── */
    {
        .name = "browser_snapshot",
        .description = "Fetch and index a web page snapshot for retrieval-first browsing workflows.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Page URL\"},\"timeout\":{\"type\":\"integer\",\"description\":\"HTTP timeout seconds (default 20)\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Max response chars to ingest\"}},\"required\":[\"url\"]}",
        .execute = tool_browser_snapshot
    },
    {
        .name = "browser_extract",
        .description = "Query previously indexed browser snapshots using retrieval + reranking.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Extraction query\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Number of hits (default 5)\"}},\"required\":[\"query\"]}",
        .execute = tool_browser_extract
    },
    /* ── Workflow Graph Toolkit ──────────────────────────────────────────── */
    {
        .name = "workflow_plan",
        .description = "Create a workflow plan with newline/semicolon-separated steps.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Workflow name\"},\"steps\":{\"type\":\"string\",\"description\":\"Step list (newline or semicolon separated)\"}},\"required\":[\"steps\"]}",
        .execute = tool_workflow_plan
    },
    {
        .name = "workflow_status",
        .description = "Show workflow status (one workflow by id, or all).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Workflow ID (omit to list all)\"}}}",
        .execute = tool_workflow_status
    },
    {
        .name = "workflow_checkpoint",
        .description = "Update a workflow step status with an optional note.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Workflow ID\"},\"step\":{\"type\":\"integer\",\"description\":\"Step number (1-based)\"},\"status\":{\"type\":\"string\",\"description\":\"pending|in_progress|done|blocked\"},\"note\":{\"type\":\"string\",\"description\":\"Optional step note\"}},\"required\":[\"id\",\"step\",\"status\"]}",
        .execute = tool_workflow_checkpoint
    },
    {
        .name = "workflow_resume",
        .description = "Return the next actionable workflow step.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Workflow ID\"}},\"required\":[\"id\"]}",
        .execute = tool_workflow_resume
    },
    /* ── Research Toolkit ────────────────────────────────────────────────── */
    {
        .name = "research_probe",
        .description = "Fetch and index a source, then optionally run focused retrieval against that source.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Source URL\"},\"query\":{\"type\":\"string\",\"description\":\"Optional focused question\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Hit count for query (default 4)\"},\"timeout\":{\"type\":\"integer\",\"description\":\"HTTP timeout\"}},\"required\":[\"url\"]}",
        .execute = tool_research_probe
    },
    {
        .name = "research_compare",
        .description = "Compare two passages and report lexical overlap metrics.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text_a\":{\"type\":\"string\",\"description\":\"Passage A\"},\"text_b\":{\"type\":\"string\",\"description\":\"Passage B\"}},\"required\":[\"text_a\",\"text_b\"]}",
        .execute = tool_research_compare
    },
    /* ── Code Intelligence Toolkit ───────────────────────────────────────── */
    {
        .name = "code_index",
        .description = "Index repository files into retrieval context for semantic code search.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Project path (default .)\"},\"max_files\":{\"type\":\"integer\",\"description\":\"Max files to index\"},\"max_chars_per_file\":{\"type\":\"integer\",\"description\":\"Max chars read per file\"}}}",
        .execute = tool_code_index
    },
    {
        .name = "code_search",
        .description = "Search indexed code chunks using retrieval + reranking.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Code query\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Number of hits\"}},\"required\":[\"query\"]}",
        .execute = tool_code_search
    },
    /* ── Sandbox + Policy Toolkit ───────────────────────────────────────── */
    {
        .name = "sandbox_run",
        .description = "Run a command in a constrained environment (Docker no-network if available, otherwise minimal env shell).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command to execute\"},\"image\":{\"type\":\"string\",\"description\":\"Optional docker image\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout seconds (default 60)\"}},\"required\":[\"command\"]}",
        .execute = tool_sandbox_run
    },
    {
        .name = "privacy_filter",
        .description = "Redact common PII patterns (email and phone-like tokens) from text.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Input text\"}},\"required\":[\"text\"]}",
        .execute = tool_privacy_filter
    },
    {
        .name = "secret_scan",
        .description = "Scan text or file content for common secret/key leakage patterns.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to scan\"},\"file\":{\"type\":\"string\",\"description\":\"File path to scan\"}}}",
        .execute = tool_secret_scan
    },
    {
        .name = "risk_gate",
        .description = "Score action/content risk and return allow/review/deny decision.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Planned action\"},\"content\":{\"type\":\"string\",\"description\":\"Optional payload/context\"}},\"required\":[\"action\"]}",
        .execute = tool_risk_gate
    },
    /* ── Git ─────────────────────────────────────────────────────────────── */
    {
        .name = "git_status",
        .description = "Show git status (short format with branch info).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_git_status
    },
    {
        .name = "git_diff",
        .description = "Show git diff. Pass args like '--staged' or 'HEAD~1'.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\",\"description\":\"Additional git diff arguments\"}}}",
        .execute = tool_git_diff
    },
    {
        .name = "git_log",
        .description = "Show recent git commits (oneline format).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"description\":\"Number of commits to show (default 10)\"}}}",
        .execute = tool_git_log
    },
    {
        .name = "git_commit",
        .description = "Create a git commit with a message.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":\"Commit message\"},\"all\":{\"type\":\"boolean\",\"description\":\"Stage all modified files (-a)\"}},\"required\":[\"message\"]}",
        .execute = tool_git_commit
    },
    {
        .name = "git_add",
        .description = "Stage files for git commit.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"string\",\"description\":\"Files to add (space-separated, or . for all)\"}},\"required\":[\"files\"]}",
        .execute = tool_git_add
    },
    {
        .name = "git_branch",
        .description = "List, create, or switch git branches.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Branch name (omit to list all)\"},\"create\":{\"type\":\"boolean\",\"description\":\"Create and switch to new branch\"}}}",
        .execute = tool_git_branch
    },
    {
        .name = "git_stash",
        .description = "Git stash operations: push, pop, list, drop.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Action: push (default), pop, list, drop\"}}}",
        .execute = tool_git_stash
    },
    {
        .name = "git_clone",
        .description = "Clone a git repository.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Repository URL\"},\"directory\":{\"type\":\"string\",\"description\":\"Target directory\"}},\"required\":[\"url\"]}",
        .execute = tool_git_clone
    },
    {
        .name = "git_push",
        .description = "Push commits to remote.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"remote\":{\"type\":\"string\",\"description\":\"Remote name (default origin)\"},\"branch\":{\"type\":\"string\",\"description\":\"Branch name\"}}}",
        .execute = tool_git_push
    },
    {
        .name = "git_pull",
        .description = "Pull changes from remote.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"remote\":{\"type\":\"string\",\"description\":\"Remote name\"},\"branch\":{\"type\":\"string\",\"description\":\"Branch name\"}}}",
        .execute = tool_git_pull
    },
    /* ── Process & System ────────────────────────────────────────────────── */
    {
        .name = "ps",
        .description = "List running processes. Optionally filter by name.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"description\":\"Process name filter\"}}}",
        .execute = tool_ps
    },
    {
        .name = "kill_process",
        .description = "Send a signal to a process.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\",\"description\":\"Process ID\"},\"signal\":{\"type\":\"integer\",\"description\":\"Signal number (default 15/SIGTERM)\"}},\"required\":[\"pid\"]}",
        .execute = tool_kill_process
    },
    {
        .name = "env_get",
        .description = "Get environment variable(s). Without name, lists all.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Variable name (omit to list all)\"}}}",
        .execute = tool_env_get
    },
    {
        .name = "env_set",
        .description = "Set an environment variable for this session.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Variable name\"},\"value\":{\"type\":\"string\",\"description\":\"Variable value\"}},\"required\":[\"name\",\"value\"]}",
        .execute = tool_env_set
    },
    {
        .name = "sysinfo",
        .description = "Show system information (OS, disk, uptime).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_sysinfo
    },
    {
        .name = "disk_usage",
        .description = "Show disk usage for a path.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to check (default .)\"}}}",
        .execute = tool_disk_usage
    },
    {
        .name = "which",
        .description = "Find a command's location and version.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Command name\"}},\"required\":[\"name\"]}",
        .execute = tool_which
    },
    {
        .name = "cwd",
        .description = "Get or change the current working directory.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory to change to (omit to just print cwd)\"}}}",
        .execute = tool_cwd
    },
    /* ── Text Processing ─────────────────────────────────────────────────── */
    {
        .name = "sed",
        .description = "Run a sed expression on a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Sed expression (e.g. s/old/new/g)\"},\"file\":{\"type\":\"string\",\"description\":\"Input file\"}},\"required\":[\"expression\",\"file\"]}",
        .execute = tool_sed
    },
    {
        .name = "awk",
        .description = "Run an awk program on a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"program\":{\"type\":\"string\",\"description\":\"Awk program\"},\"file\":{\"type\":\"string\",\"description\":\"Input file\"}},\"required\":[\"program\"]}",
        .execute = tool_awk
    },
    {
        .name = "sort_uniq",
        .description = "Sort a file, optionally unique or with counts.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\",\"description\":\"Input file\"},\"unique\":{\"type\":\"boolean\",\"description\":\"Remove duplicates\"},\"count\":{\"type\":\"boolean\",\"description\":\"Count occurrences\"}},\"required\":[\"file\"]}",
        .execute = tool_sort_uniq
    },
    {
        .name = "diff",
        .description = "Show unified diff between two files.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file1\":{\"type\":\"string\",\"description\":\"First file\"},\"file2\":{\"type\":\"string\",\"description\":\"Second file\"}},\"required\":[\"file1\",\"file2\"]}",
        .execute = tool_diff
    },
    {
        .name = "patch",
        .description = "Apply a unified diff patch to a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\",\"description\":\"File to patch\"},\"patch\":{\"type\":\"string\",\"description\":\"Patch content (unified diff)\"}},\"required\":[\"file\",\"patch\"]}",
        .execute = tool_patch
    },
    {
        .name = "jq",
        .description = "Process JSON with jq. Provide input as string or file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"description\":\"jq filter expression\"},\"input\":{\"type\":\"string\",\"description\":\"JSON string input\"},\"file\":{\"type\":\"string\",\"description\":\"JSON file path\"}},\"required\":[\"filter\"]}",
        .execute = tool_jq
    },
    /* ── Encoding & Hashing ──────────────────────────────────────────────── */
    {
        .name = "base64",
        .description = "Base64 encode or decode data or a file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"data\":{\"type\":\"string\",\"description\":\"String to encode/decode\"},\"file\":{\"type\":\"string\",\"description\":\"File to encode/decode\"},\"decode\":{\"type\":\"boolean\",\"description\":\"Decode instead of encode\"}}}",
        .execute = tool_base64
    },
    {
        .name = "hash",
        .description = "Compute hash (md5, sha1, sha256) of data or file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\",\"description\":\"File to hash\"},\"data\":{\"type\":\"string\",\"description\":\"String to hash\"},\"algorithm\":{\"type\":\"string\",\"description\":\"Hash algorithm: md5, sha1, sha256 (default)\"}}}",
        .execute = tool_hash
    },
    /* ── Archives ────────────────────────────────────────────────────────── */
    {
        .name = "tar",
        .description = "Create, extract, or list tar.gz archives.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"create, extract, or list\"},\"archive\":{\"type\":\"string\",\"description\":\"Archive file path\"},\"files\":{\"type\":\"string\",\"description\":\"Files to include (for create)\"}},\"required\":[\"action\",\"archive\"]}",
        .execute = tool_tar
    },
    {
        .name = "zip",
        .description = "Create, extract, or list zip archives.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"create, extract, or list\"},\"archive\":{\"type\":\"string\",\"description\":\"Archive file path\"},\"files\":{\"type\":\"string\",\"description\":\"Files to include (for create)\"}},\"required\":[\"action\",\"archive\"]}",
        .execute = tool_zip
    },
    /* ── Network / Curl ──────────────────────────────────────────────────── */
    {
        .name = "http_request",
        .description = "Make an HTTP request. Supports GET, POST, PUT, DELETE, PATCH with custom headers and body.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Request URL\"},\"method\":{\"type\":\"string\",\"description\":\"HTTP method (default GET)\"},\"headers\":{\"type\":\"string\",\"description\":\"Headers, one per line\"},\"body\":{\"type\":\"string\",\"description\":\"Request body\"},\"include_headers\":{\"type\":\"boolean\",\"description\":\"Include response headers\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}},\"required\":[\"url\"]}",
        .execute = tool_http_request
    },
    {
        .name = "json_api",
        .description = "Call a JSON API with Content-Type and Accept set to application/json.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"API URL\"},\"method\":{\"type\":\"string\",\"description\":\"HTTP method (default GET)\"},\"body\":{\"type\":\"string\",\"description\":\"JSON request body\"},\"auth_header\":{\"type\":\"string\",\"description\":\"Authorization header value\"}},\"required\":[\"url\"]}",
        .execute = tool_json_api
    },
    {
        .name = "download_file",
        .description = "Download a URL to a local file. Follows redirects.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to download\"},\"output\":{\"type\":\"string\",\"description\":\"Local file path\"}},\"required\":[\"url\",\"output\"]}",
        .execute = tool_download
    },
    {
        .name = "upload_file",
        .description = "Upload a file to a URL via multipart POST.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Upload URL\"},\"file\":{\"type\":\"string\",\"description\":\"Local file path\"},\"field_name\":{\"type\":\"string\",\"description\":\"Form field name (default: file)\"}},\"required\":[\"url\",\"file\"]}",
        .execute = tool_upload
    },
    {
        .name = "curl_raw",
        .description = "Run curl with arbitrary arguments.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\",\"description\":\"Raw curl arguments\"}},\"required\":[\"args\"]}",
        .execute = tool_curl_raw
    },
    {
        .name = "http_headers",
        .description = "Fetch only HTTP response headers for a URL.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to probe\"}},\"required\":[\"url\"]}",
        .execute = tool_http_headers
    },
    {
        .name = "dns_lookup",
        .description = "Resolve a hostname to IP addresses.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"hostname\":{\"type\":\"string\",\"description\":\"Hostname to resolve\"}},\"required\":[\"hostname\"]}",
        .execute = tool_dns_lookup
    },
    {
        .name = "ping",
        .description = "Ping a host to check connectivity and latency.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Host to ping\"},\"count\":{\"type\":\"integer\",\"description\":\"Number of pings (default 4, max 20)\"}},\"required\":[\"host\"]}",
        .execute = tool_ping
    },
    {
        .name = "port_check",
        .description = "Check if a TCP port is open on a host.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Target host\"},\"port\":{\"type\":\"integer\",\"description\":\"TCP port number\"}},\"required\":[\"host\",\"port\"]}",
        .execute = tool_port_check
    },
    {
        .name = "port_scan",
        .description = "Scan common ports on a host (or specify custom ports).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Target host\"},\"ports\":{\"type\":\"string\",\"description\":\"Space-separated port list (default: common ports)\"}},\"required\":[\"host\"]}",
        .execute = tool_port_scan
    },
    {
        .name = "netstat",
        .description = "Show active network connections and listening ports.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_netstat
    },
    {
        .name = "cert_info",
        .description = "Show TLS certificate information for a host.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Hostname\"},\"port\":{\"type\":\"integer\",\"description\":\"Port (default 443)\"}},\"required\":[\"host\"]}",
        .execute = tool_cert_info
    },
    {
        .name = "traceroute",
        .description = "Trace the network route to a host.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Target host\"}},\"required\":[\"host\"]}",
        .execute = tool_traceroute
    },
    {
        .name = "whois_lookup",
        .description = "Look up WHOIS information for a domain.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"domain\":{\"type\":\"string\",\"description\":\"Domain name\"}},\"required\":[\"domain\"]}",
        .execute = tool_whois
    },
    {
        .name = "net_interfaces",
        .description = "Show network interfaces and their IP addresses.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_net_interfaces
    },
    {
        .name = "websocket_test",
        .description = "Test a WebSocket connection.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"WebSocket URL\"},\"message\":{\"type\":\"string\",\"description\":\"Message to send\"}},\"required\":[\"url\"]}",
        .execute = tool_ws_test
    },
    /* ── Docker ──────────────────────────────────────────────────────────── */
    {
        .name = "docker",
        .description = "Run a docker command (e.g. ps, images, run, build, exec).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Docker subcommand and args (e.g. 'ps -a', 'run -d nginx')\"}},\"required\":[\"command\"]}",
        .execute = tool_docker
    },
    {
        .name = "docker_compose",
        .description = "Run docker compose commands.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Compose subcommand (e.g. 'up -d', 'down', 'logs')\"}},\"required\":[\"command\"]}",
        .execute = tool_docker_compose
    },
    /* ── SSH / Remote ────────────────────────────────────────────────────── */
    {
        .name = "ssh_command",
        .description = "Execute a command on a remote host via SSH.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Remote host\"},\"command\":{\"type\":\"string\",\"description\":\"Command to run\"},\"user\":{\"type\":\"string\",\"description\":\"SSH user\"},\"key\":{\"type\":\"string\",\"description\":\"Path to SSH key\"}},\"required\":[\"host\",\"command\"]}",
        .execute = tool_ssh_command
    },
    {
        .name = "scp",
        .description = "Copy files over SSH (local->remote or remote->local).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Source path (user@host:path or local path)\"},\"destination\":{\"type\":\"string\",\"description\":\"Destination path\"},\"key\":{\"type\":\"string\",\"description\":\"SSH key path\"}},\"required\":[\"source\",\"destination\"]}",
        .execute = tool_scp
    },
    /* ── Database ────────────────────────────────────────────────────────── */
    {
        .name = "sqlite",
        .description = "Execute a SQL query against a SQLite database.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"database\":{\"type\":\"string\",\"description\":\"Database file path\"},\"query\":{\"type\":\"string\",\"description\":\"SQL query\"}},\"required\":[\"database\",\"query\"]}",
        .execute = tool_sqlite
    },
    {
        .name = "psql",
        .description = "Execute a SQL query against PostgreSQL.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"connection\":{\"type\":\"string\",\"description\":\"Connection string (e.g. postgresql://user:pass@host/db)\"},\"query\":{\"type\":\"string\",\"description\":\"SQL query\"}},\"required\":[\"query\"]}",
        .execute = tool_psql
    },
    /* ── Scripting ───────────────────────────────────────────────────────── */
    {
        .name = "python",
        .description = "Execute Python code (inline or from file).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"Python code to execute\"},\"file\":{\"type\":\"string\",\"description\":\"Python file to run\"}}}",
        .execute = tool_python
    },
    {
        .name = "node",
        .description = "Execute JavaScript/Node.js code (inline or from file).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute\"},\"file\":{\"type\":\"string\",\"description\":\"JS file to run\"}}}",
        .execute = tool_node
    },
    /* ── Date/Time & Math ────────────────────────────────────────────────── */
    {
        .name = "date",
        .description = "Get current date/time, optionally in a specific timezone or format.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"format\":{\"type\":\"string\",\"description\":\"Date format string (strftime)\"},\"timezone\":{\"type\":\"string\",\"description\":\"Timezone (e.g. America/New_York, UTC)\"}}}",
        .execute = tool_date
    },
    {
        .name = "calc",
        .description = "Evaluate a mathematical expression using bc or python.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Math expression (e.g. '2^32', 'sqrt(144)', '3.14*2')\"}},\"required\":[\"expression\"]}",
        .execute = tool_calc
    },
    /* ── Clipboard ───────────────────────────────────────────────────────── */
    {
        .name = "clipboard",
        .description = "Read from or write to the system clipboard.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"read or write (default read)\"},\"content\":{\"type\":\"string\",\"description\":\"Content to copy (for write)\"}}}",
        .execute = tool_clipboard
    },
    /* ── Package Management ──────────────────────────────────────────────── */
    {
        .name = "pkg",
        .description = "Run a package manager command (auto-detects brew/apt/yum).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"manager\":{\"type\":\"string\",\"description\":\"Package manager (brew, apt, yum, etc.)\"},\"command\":{\"type\":\"string\",\"description\":\"Package command (e.g. 'install jq', 'list')\"}},\"required\":[\"command\"]}",
        .execute = tool_pkg
    },
    {
        .name = "pip",
        .description = "Run pip3 commands (install, list, freeze, etc.).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"pip command (e.g. 'install requests', 'list')\"}},\"required\":[\"command\"]}",
        .execute = tool_pip
    },
    {
        .name = "npm",
        .description = "Run npm commands (install, list, init, etc.).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"npm command (e.g. 'install express', 'list')\"}},\"required\":[\"command\"]}",
        .execute = tool_npm
    },
    /* ── Scheduling ──────────────────────────────────────────────────────── */
    {
        .name = "crontab",
        .description = "List or add crontab entries.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"list or add\"},\"entry\":{\"type\":\"string\",\"description\":\"Cron entry to add (e.g. '0 * * * * /path/to/script')\"}}}",
        .execute = tool_crontab
    },
    /* ── macOS Extended Attributes ────────────────────────────────────────── */
    {
        .name = "xattr",
        .description = "List or clear extended attributes on a file (macOS).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"action\":{\"type\":\"string\",\"description\":\"list (default) or clear\"}},\"required\":[\"path\"]}",
        .execute = tool_xattr
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * AST SELF-INTROSPECTION TOOLS
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "self_inspect",
        .description = "Introspect dsco's own source code. Returns AST-level analysis: files, functions, structs, tools, line counts, complexity scores, and dependency graph. Use this to understand dsco's architecture before modifying it.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"project_dir\":{\"type\":\"string\",\"description\":\"Project directory (default: directory containing dsco binary)\"}}}",
        .execute = tool_self_inspect
    },
    {
        .name = "inspect_file",
        .description = "Deep AST analysis of a single C/H file. Returns every function (with params, return type, complexity, line range), every struct/typedef/enum, includes, defines. Use for targeted code understanding.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to .c or .h file\"}},\"required\":[\"path\"]}",
        .execute = tool_inspect_file
    },
    {
        .name = "call_graph",
        .description = "Show what functions a given function calls (from body analysis). Useful for understanding control flow and planning refactors.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"function\":{\"type\":\"string\",\"description\":\"Function name to analyze\"},\"project_dir\":{\"type\":\"string\",\"description\":\"Project directory\"}},\"required\":[\"function\"]}",
        .execute = tool_call_graph
    },
    {
        .name = "dependency_graph",
        .description = "Show the include dependency graph between source files. Returns which files depend on which headers.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"project_dir\":{\"type\":\"string\",\"description\":\"Project directory\"}}}",
        .execute = tool_dependency_graph
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * SUB-AGENT SWARM TOOLS
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "spawn_agent",
        .description = "Spawn a sub-dsco agent to work on a task autonomously. The agent runs as a separate process with its own conversation with Claude. Returns immediately with an agent ID — use agent_status/agent_output to monitor. Great for parallelizing: spawn multiple agents for different subtasks.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"description\":\"The task/prompt for the sub-agent (be specific and self-contained)\"},\"model\":{\"type\":\"string\",\"description\":\"Model override for this agent\"}},\"required\":[\"task\"]}",
        .execute = tool_spawn_agent
    },
    {
        .name = "agent_status",
        .description = "Check status of all spawned sub-agents. Shows running/done/error state, elapsed time, output size. Use to monitor progress of spawned agents.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_agent_status
    },
    {
        .name = "agent_output",
        .description = "Get the accumulated output from a specific sub-agent. Polls for latest data. Returns the agent's streamed stdout including Claude's responses and tool usage.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Agent ID (from spawn_agent)\"}},\"required\":[\"id\"]}",
        .execute = tool_agent_output
    },
    {
        .name = "agent_wait",
        .description = "Wait for a specific agent or all agents to complete. Streams status updates while waiting. Returns final output when done.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Agent ID to wait for (-1 or omit for all)\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Max seconds to wait (default 120)\"}}}",
        .execute = tool_agent_wait
    },
    {
        .name = "agent_kill",
        .description = "Kill a running sub-agent by ID.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"description\":\"Agent ID to kill\"}},\"required\":[\"id\"]}",
        .execute = tool_agent_kill
    },
    {
        .name = "create_swarm",
        .description = "Create a named group of sub-agents and dispatch multiple tasks to them simultaneously. Each task gets its own agent. Returns group ID for monitoring. Use for parallel work: e.g. create_swarm with tasks=['write backend API', 'write frontend UI', 'write tests'].",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Swarm group name\"},\"tasks\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of task prompts, one per agent\"},\"model\":{\"type\":\"string\",\"description\":\"Model for all agents in this swarm\"}},\"required\":[\"name\",\"tasks\"]}",
        .execute = tool_create_swarm
    },
    {
        .name = "swarm_status",
        .description = "Check status of a swarm group. Shows each agent's status and output preview.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"group_id\":{\"type\":\"integer\",\"description\":\"Swarm group ID (from create_swarm)\"}},\"required\":[\"group_id\"]}",
        .execute = tool_swarm_status
    },
    {
        .name = "swarm_collect",
        .description = "Wait for all agents in a swarm to complete and collect their outputs. Returns all results aggregated.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"group_id\":{\"type\":\"integer\",\"description\":\"Swarm group ID\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Max seconds to wait (default 300)\"}},\"required\":[\"group_id\"]}",
        .execute = tool_swarm_collect
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * IPC — INTER-AGENT COMMUNICATION TOOLS
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "ipc_send",
        .description = "Send a message to another agent (point-to-point or broadcast). For inter-agent coordination, task delegation, and status sharing.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\",\"description\":\"Target agent ID (omit for broadcast)\"},\"topic\":{\"type\":\"string\",\"description\":\"Message topic/channel\"},\"body\":{\"type\":\"string\",\"description\":\"Message content\"}},\"required\":[\"body\"]}",
        .execute = tool_ipc_send
    },
    {
        .name = "ipc_recv",
        .description = "Read unread messages from other agents. Returns messages addressed to this agent plus broadcasts.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"topic\":{\"type\":\"string\",\"description\":\"Filter by topic (optional)\"}}}",
        .execute = tool_ipc_recv
    },
    {
        .name = "ipc_agents",
        .description = "List all agents in the hierarchy with their status, role, depth, and liveness. Shows the full agent tree.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_ipc_agents
    },
    {
        .name = "ipc_scratch_put",
        .description = "Write a key-value pair to the shared scratchpad. All agents in the hierarchy can read/write this shared state.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Key name\"},\"value\":{\"type\":\"string\",\"description\":\"Value to store\"}},\"required\":[\"key\",\"value\"]}",
        .execute = tool_ipc_scratch_put
    },
    {
        .name = "ipc_scratch_get",
        .description = "Read a value from the shared scratchpad by key. Returns the value and which agent wrote it.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Key to read\"}},\"required\":[\"key\"]}",
        .execute = tool_ipc_scratch_get
    },
    {
        .name = "ipc_task_submit",
        .description = "Submit a task to the shared task queue. Any idle agent can claim and execute it. Use for work distribution.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"description\":{\"type\":\"string\",\"description\":\"Task description\"},\"priority\":{\"type\":\"integer\",\"description\":\"Priority (higher=more urgent, default 0)\"},\"parent_task_id\":{\"type\":\"integer\",\"description\":\"Parent task ID for sub-tasks\"}},\"required\":[\"description\"]}",
        .execute = tool_ipc_task_submit
    },
    {
        .name = "ipc_task_list",
        .description = "List tasks in the shared queue with status, priority, and results. Filter by assigned agent optionally.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"assigned_to\":{\"type\":\"string\",\"description\":\"Filter by agent ID (optional)\"}}}",
        .execute = tool_ipc_task_list
    },
    {
        .name = "ipc_set_role",
        .description = "Set this agent's role/specialization (e.g., 'researcher', 'coder', 'reviewer'). Visible to other agents.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\",\"description\":\"Role name\"}},\"required\":[\"role\"]}",
        .execute = tool_ipc_set_role
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * CRYPTOGRAPHIC TOOLS
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "sha256",
        .description = "Compute SHA-256 hash of text or file. Pure C implementation — no external dependencies.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to hash\"},\"file\":{\"type\":\"string\",\"description\":\"File path to hash\"}}}",
        .execute = tool_sha256
    },
    {
        .name = "md5",
        .description = "Compute MD5 hash of text or file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to hash\"},\"file\":{\"type\":\"string\",\"description\":\"File path to hash\"}}}",
        .execute = tool_md5
    },
    {
        .name = "hmac",
        .description = "Compute HMAC-SHA256 of a message with a given key.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"HMAC key\"},\"message\":{\"type\":\"string\",\"description\":\"Message to authenticate\"}},\"required\":[\"key\",\"message\"]}",
        .execute = tool_hmac
    },
    {
        .name = "uuid",
        .description = "Generate a UUID v4 (cryptographically random).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"description\":\"Number of UUIDs to generate (default 1)\"}}}",
        .execute = tool_uuid
    },
    {
        .name = "random_bytes",
        .description = "Generate cryptographically random bytes (hex-encoded).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"bytes\":{\"type\":\"integer\",\"description\":\"Number of random bytes (default 32)\"},\"format\":{\"type\":\"string\",\"description\":\"Output format: hex (default), base64\"}}}",
        .execute = tool_random_bytes
    },
    {
        .name = "base64_tool",
        .description = "Base64 encode or decode text.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\",\"description\":\"Input text\"},\"action\":{\"type\":\"string\",\"description\":\"encode (default) or decode\"},\"url_safe\":{\"type\":\"boolean\",\"description\":\"Use URL-safe base64 (default false)\"}},\"required\":[\"input\"]}",
        .execute = tool_base64_tool
    },
    {
        .name = "jwt_decode",
        .description = "Decode a JWT token (header + payload, no signature verification).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"token\":{\"type\":\"string\",\"description\":\"JWT token string\"}},\"required\":[\"token\"]}",
        .execute = tool_jwt_decode
    },
    {
        .name = "hkdf",
        .description = "Derive keys using HKDF-SHA256 (RFC 5869).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"ikm\":{\"type\":\"string\",\"description\":\"Input key material (hex)\"},\"salt\":{\"type\":\"string\",\"description\":\"Salt (hex, optional)\"},\"info\":{\"type\":\"string\",\"description\":\"Context info string\"},\"length\":{\"type\":\"integer\",\"description\":\"Output key length in bytes (default 32)\"}},\"required\":[\"ikm\"]}",
        .execute = tool_hkdf
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * PIPELINE TOOLS — Coroutine-powered data pipelines
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "pipeline",
        .description = "Run a streaming data pipeline on text. Chains transform stages using coroutines. "
                       "Spec format: 'stage:arg|stage:arg|...' "
                       "Available stages: filter/grep, filter_v, map/sed, sort, sort_n, sort_r, "
                       "uniq, uniq_c, head:N, tail:N, reverse, count, trim, upper, lower, "
                       "prefix:str, suffix:str, number/nl, join:sep, split:delim, cut:delim:N, "
                       "regex:pattern, replace:old/new, take_while, drop_while, flatten, "
                       "compact/blank_remove, length, hash/sha256, jq/json_extract:key, "
                       "csv_column:N, stats. "
                       "Example: 'filter:error|sort|uniq|head:20'",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\",\"description\":\"Input text (multi-line)\"},\"spec\":{\"type\":\"string\",\"description\":\"Pipeline spec (stage:arg|stage:arg|...)\"},\"file\":{\"type\":\"string\",\"description\":\"Read input from file instead\"}},\"required\":[\"spec\"]}",
        .execute = tool_pipeline
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * EXPRESSION EVALUATOR — Recursive descent math engine
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "eval",
        .description = "Evaluate mathematical expressions. Supports: arithmetic (+,-,*,/,%,^/**), "
                       "comparison (==,!=,<,>,<=,>=), bitwise (&,|,~,<<,>>), ternary (?:), "
                       "variables (x=42; x*2), functions (sqrt, sin, cos, tan, log, ln, exp, "
                       "abs, ceil, floor, round, min, max, gcd, lcm, fib, factorial, pow, "
                       "deg, rad, hex, oct, bin), constants (pi, e, tau, phi), "
                       "hex/oct/bin literals (0xFF, 0o77, 0b1010), factorial (5!). "
                       "Multiple expressions separated by semicolons.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Math expression(s) to evaluate (semicolon-separated)\"}},\"required\":[\"expression\"]}",
        .execute = tool_eval
    },
    {
        .name = "big_factorial",
        .description = "Compute exact factorial of large numbers using arbitrary-precision arithmetic. "
                       "Returns all digits (e.g. 100! has 158 digits).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\",\"description\":\"Number to compute factorial of\"}},\"required\":[\"n\"]}",
        .execute = tool_big_factorial
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * PLUGIN SYSTEM — Dynamic tool loading
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "plugin_list",
        .description = "List loaded plugins from ~/.dsco/plugins/. Shows plugin names, versions, "
                       "tool counts, and build instructions.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_plugin_list
    },
    {
        .name = "plugin_reload",
        .description = "Hot-reload all plugins from ~/.dsco/plugins/.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_plugin_reload
    },
    {
        .name = "plugin_load",
        .description = "Load a specific plugin from a file path.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to .dylib/.so plugin file\"}},\"required\":[\"path\"]}",
        .execute = tool_plugin_load_file
    },
};

static const int s_tool_count = sizeof(s_tools) / sizeof(s_tools[0]);

void tools_init(void) {
    plugin_init(&g_plugins);
    ctx_store_reset();

    /* Initialize IPC early so DSCO_IPC_DB is set before any child spawns */
    ipc_init(NULL, NULL);
    const char *depth_s = getenv("DSCO_SWARM_DEPTH");
    const char *parent = getenv("DSCO_PARENT_INSTANCE_ID");
    int depth = depth_s ? atoi(depth_s) : 0;
    ipc_register(parent, depth, getenv("DSCO_SUBAGENT") ? "worker" : "root", "*");
}

const tool_def_t *tools_get_all(int *count) {
    *count = s_tool_count + g_plugins.extra_tool_count;
    return s_tools;
}

int tools_builtin_count(void) {
    return s_tool_count;
}

bool tools_execute(const char *name, const char *input_json,
                   char *result, size_t result_len) {
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            bool ok = s_tools[i].execute(input_json, result, result_len);
            ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
            return ok;
        }
    }
    /* Check plugin tools */
    for (int i = 0; i < g_plugins.extra_tool_count; i++) {
        if (strcmp(g_plugins.extra_tools[i].name, name) == 0) {
            bool ok = g_plugins.extra_tools[i].execute(input_json, result, result_len);
            ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
            return ok;
        }
    }
    snprintf(result, result_len, "unknown tool: %s", name);
    return false;
}
