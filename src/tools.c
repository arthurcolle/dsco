#include "tools.h"
#include "error.h"
#include "integrations.h"
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
#include "trace.h"
#include "provider.h"
#include "topology.h"
#include "workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include <regex.h>

extern volatile int g_interrupted;

/* ── Global swarm instance (shared across tool calls) ─────────────────── */
static swarm_t g_swarm = {0};
static bool    g_swarm_inited = false;

static void sanitize_tool_result_inplace(char *result) {
    if (!result || !result[0]) return;
    dsco_strip_terminal_controls_inplace(result);
}

static bool dsco_flock(int fd, short type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    int rc;
    do {
        rc = fcntl(fd, F_SETLKW, &fl);
    } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static bool dsco_funlock(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    int rc;
    do {
        rc = fcntl(fd, F_SETLK, &fl);
    } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static char    g_runtime_model[128] = "";
static char   *g_runtime_api_key = NULL;

void tools_set_runtime_api_key(const char *api_key) {
    free(g_runtime_api_key);
    g_runtime_api_key = api_key && api_key[0] ? safe_strdup(api_key) : NULL;
    if (g_swarm_inited) {
        g_swarm.api_key = g_runtime_api_key;
    }
}

void tools_set_runtime_model(const char *model) {
    const char *resolved = model && model[0] ? model_resolve_alias(model) : DEFAULT_MODEL;
    snprintf(g_runtime_model, sizeof(g_runtime_model), "%s", resolved);
    if (g_swarm_inited) {
        g_swarm.default_model = g_runtime_model;
    }
}

const char *tools_runtime_api_key(void) {
    return g_runtime_api_key;
}

const char *tools_runtime_model(void) {
    return g_runtime_model[0] ? g_runtime_model : DEFAULT_MODEL;
}

static double now_sec_helper(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int clamp_timeout_seconds(int value, int def, int min_v, int max_v) {
    if (value <= 0) value = def;
    if (value < min_v) value = min_v;
    if (value > max_v) value = max_v;
    return value;
}

static bool require_regular_file(const char *path, char *result, size_t rlen) {
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(result, rlen, "error: cannot stat %s: %s", path, strerror(errno));
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(result, rlen, "error: %s is not a regular file", path);
        return false;
    }
    return true;
}

static void ensure_swarm(void) {
    if (!g_swarm_inited) {
        const char *model = tools_runtime_model();
        const char *key = g_runtime_api_key;
        if (!key || !key[0]) {
            const char *provider = provider_detect(model, NULL);
            key = provider_resolve_api_key(provider);
        }
        swarm_init(&g_swarm, key, model);
        g_swarm_inited = true;
    }
}

swarm_t *tools_swarm_instance(void) {
    ensure_swarm();
    return &g_swarm;
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
    bool poll_failed = false;
    int poll_err = 0;
    bool first_chunk = true;

    while (1) {
        if (g_interrupted) {
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            break;
        }

        int poll_ms = 200;  /* check every 200ms */
        int ready = poll(&pfd, 1, poll_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                if (g_interrupted) {
                    kill(-pid, SIGTERM);
                    usleep(100000);
                    kill(-pid, SIGKILL);
                    break;
                }
                continue;
            }
            poll_failed = true;
            poll_err = errno;
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            break;
        }

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
    if (poll_failed) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_len - cur, "\n[error: poll failed: %s]",
                 strerror(poll_err ? poll_err : EIO));
        return -1;
    }
    if (g_interrupted) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_len - cur, "\n[interrupted]");
        return 130;
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
#define CTX_MAX_CHUNKS            2048
#define CTX_MAX_TOTAL_BYTES       (24 * 1024 * 1024)
#define CTX_CHUNK_TARGET_CHARS    1200
#define CTX_CHUNK_MIN_CHARS       350
#define CTX_CHUNK_OVERLAP_CHARS   180
#define CTX_SEARCH_DEFAULT_K      5
#define CTX_SEARCH_MAX_K          12
#define CTX_SEARCH_CANDIDATES     40
#define CTX_RESULT_OFFLOAD_BYTES  4096
#define CTX_TURN_PIN_IDS_MAX      2048

typedef struct {
    int id;
    char tool[48];
    char *text;
    size_t text_len;
    bool pinned;
    bool turn_pinned;
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
    bool auto_pin;
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

static void ctx_evict_index(int idx);
static int g_ctx_turn_pins_prev[CTX_TURN_PIN_IDS_MAX];
static int g_ctx_turn_pins_prev_count = 0;
static int g_ctx_turn_pins_curr[CTX_TURN_PIN_IDS_MAX];
static int g_ctx_turn_pins_curr_count = 0;

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

static void ctx_evict_index(int idx);

static int ctx_oldest_evictable_index(void) {
    for (int i = 0; i < g_ctx.count; i++) {
        if (!g_ctx.chunks[i].pinned && !g_ctx.chunks[i].turn_pinned) return i;
    }
    return 0;
}

static void ctx_evict_oldest(void) {
    if (g_ctx.count <= 0) return;
    ctx_evict_index(ctx_oldest_evictable_index());
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
    g_ctx_turn_pins_prev_count = 0;
    g_ctx_turn_pins_curr_count = 0;
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
    c->pinned = false;
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
    bool auto_pin = info ? info->auto_pin : false;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->auto_pin = auto_pin;
    }
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

void tools_context_turn_begin(void) {
    for (int i = 0; i < g_ctx_turn_pins_prev_count; i++) {
        int idx = ctx_find_index_by_id(g_ctx_turn_pins_prev[i]);
        if (idx >= 0) g_ctx.chunks[idx].turn_pinned = false;
    }

    memcpy(g_ctx_turn_pins_prev, g_ctx_turn_pins_curr, sizeof(g_ctx_turn_pins_curr));
    g_ctx_turn_pins_prev_count = g_ctx_turn_pins_curr_count;
    g_ctx_turn_pins_curr_count = 0;
}

static void ctx_turn_pin_range(const ctx_ingest_info_t *info) {
    if (!info || !info->auto_pin || info->chunks_added <= 0) return;
    for (int id = info->first_chunk_id; id <= info->last_chunk_id; id++) {
        int idx = ctx_find_index_by_id(id);
        if (idx < 0) continue;
        g_ctx.chunks[idx].turn_pinned = true;
        if (g_ctx_turn_pins_curr_count < CTX_TURN_PIN_IDS_MAX) {
            g_ctx_turn_pins_curr[g_ctx_turn_pins_curr_count++] = id;
        }
    }
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
    if (!name) return false;
    if (strncmp(name, "context_", 8) == 0) return true;
    if (strcmp(name, "token_audit") == 0) return true;
    return false;
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

    ctx_ingest_info_t info = { .auto_pin = true };
    ctx_ingest_text(tool_name, copy, &info);
    if (info.chunks_added > 0) {
        ctx_turn_pin_range(&info);
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
                     (c->pinned || c->turn_pinned) ? "yes" : "no",
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

typedef struct {
    int idx;
    float fused;
    float best_final;
    int hit_count;
} ctx_fused_hit_t;

static int ctx_cmp_fused_desc(const void *a, const void *b) {
    const ctx_fused_hit_t *x = (const ctx_fused_hit_t *)a;
    const ctx_fused_hit_t *y = (const ctx_fused_hit_t *)b;
    if (y->fused > x->fused) return 1;
    if (y->fused < x->fused) return -1;
    if (y->best_final > x->best_final) return 1;
    if (y->best_final < x->best_final) return -1;
    if (y->hit_count > x->hit_count) return 1;
    if (y->hit_count < x->hit_count) return -1;
    return 0;
}

typedef struct {
    char items[16][256];
    int count;
} ctx_query_list_t;

static void ctx_decode_json_string_token(const char *start, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!start || *start != '"') return;
    size_t j = 0;
    const char *p = start + 1;
    while (*p && *p != '"' && j + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[j++] = '\n'; break;
                case 'r': out[j++] = '\r'; break;
                case 't': out[j++] = '\t'; break;
                case '"': out[j++] = '"'; break;
                case '\\': out[j++] = '\\'; break;
                default: out[j++] = *p; break;
            }
            p++;
            continue;
        }
        out[j++] = *p++;
    }
    out[j] = '\0';
}

static void ctx_collect_query_cb(const char *element_start, void *ctx) {
    ctx_query_list_t *list = (ctx_query_list_t *)ctx;
    if (!list || list->count >= (int)(sizeof(list->items) / sizeof(list->items[0]))) return;
    if (!element_start) return;
    while (*element_start && isspace((unsigned char)*element_start)) element_start++;
    if (*element_start != '"') return;
    ctx_decode_json_string_token(element_start, list->items[list->count], sizeof(list->items[0]));
    if (list->items[list->count][0]) list->count++;
}

static bool ctx_chunk_matches_meta(const ctx_chunk_t *c, int source_id, const char *facet) {
    if (!c) return false;
    if (source_id > 0) {
        char key[64];
        snprintf(key, sizeof(key), "snapshot_id=%d", source_id);
        if (!strstr(c->text, key)) return false;
    }
    if (facet && *facet) {
        char key[64];
        snprintf(key, sizeof(key), "facet=%s", facet);
        if (!strstr(c->text, key)) return false;
    }
    return true;
}

static int ctx_rank_hits_filtered(const char *query,
                                  const char *tool_filter,
                                  int source_id,
                                  const char *facet,
                                  int top_k,
                                  ctx_hit_t *out_hits,
                                  int max_hits) {
    ctx_hit_t raw[CTX_SEARCH_MAX_K];
    int n_raw = ctx_rank_hits(query, tool_filter, CTX_SEARCH_MAX_K, raw, CTX_SEARCH_MAX_K);
    int n = 0;
    for (int i = 0; i < n_raw && n < max_hits; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[raw[i].idx];
        if (!ctx_chunk_matches_meta(c, source_id, facet)) continue;
        out_hits[n++] = raw[i];
        if (n >= top_k) break;
    }
    return n;
}

static int ctx_rank_hits_ladder(const char *query,
                                const char *tool_filter,
                                int source_id,
                                const char *facet,
                                int top_k,
                                ctx_hit_t *out_hits,
                                int max_hits,
                                char *mode_out,
                                size_t mode_len) {
    if (mode_out && mode_len > 0) mode_out[0] = '\0';
    if (!query || !*query) return 0;

    int n = ctx_rank_hits_filtered(query, tool_filter, source_id, facet,
                                   top_k, out_hits, max_hits);
    if (n > 0) {
        if (mode_out && mode_len > 0) snprintf(mode_out, mode_len, "strict");
        return n;
    }

    if (source_id > 0 && facet && *facet) {
        n = ctx_rank_hits_filtered(query, tool_filter, source_id, NULL,
                                   top_k, out_hits, max_hits);
        if (n > 0) {
            if (mode_out && mode_len > 0) snprintf(mode_out, mode_len, "relaxed:source_only");
            return n;
        }
    }

    if (facet && *facet) {
        n = ctx_rank_hits_filtered(query, tool_filter, -1, facet,
                                   top_k, out_hits, max_hits);
        if (n > 0) {
            if (mode_out && mode_len > 0) snprintf(mode_out, mode_len, "relaxed:facet_only");
            return n;
        }
    }

    n = ctx_rank_hits(query, tool_filter, top_k, out_hits, max_hits);
    if (n > 0 && mode_out && mode_len > 0) snprintf(mode_out, mode_len, "relaxed:unfiltered");
    return n;
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

static bool tool_soul_read(const char *input, char *result, size_t rlen) {
    (void)input;

    char path[4096];
    dsco_workspace_doc_path("soul", path, sizeof(path));
    if (path[0] == '\0') {
        snprintf(result, rlen, "error: SOUL document not available");
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(result, rlen, "error: cannot open SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    if (!dsco_flock(fd, F_RDLCK)) {
        close(fd);
        snprintf(result, rlen, "error: failed to lock SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        dsco_funlock(fd);
        close(fd);
        snprintf(result, rlen, "error: cannot stat SOUL document %s: %s", path, strerror(errno));
        return false;
    }
    if ((size_t)st.st_size >= MAX_TOOL_RESULT) {
        dsco_funlock(fd);
        close(fd);
        snprintf(result, rlen, "error: SOUL document too large to read (%lld bytes)", (long long)st.st_size);
        return false;
    }

    char *content = safe_malloc((size_t)st.st_size + 1);
    ssize_t want = (ssize_t)st.st_size;
    ssize_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, content + got, (size_t)(want - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(content);
            dsco_funlock(fd);
            close(fd);
            snprintf(result, rlen, "error: read SOUL document failed: %s", strerror(errno));
            return false;
        }
        if (n == 0) break;
        got += n;
    }
    content[got] = '\0';
    dsco_funlock(fd);
    close(fd);

    snprintf(result, rlen, "%s", content);
    free(content);
    return true;
}

static bool tool_soul_append(const char *input, char *result, size_t rlen) {
    char *content = json_get_str(input, "content");
    if (!content) {
        snprintf(result, rlen, "error: content required");
        return false;
    }

    char path[4096];
    dsco_workspace_doc_path("soul", path, sizeof(path));
    if (path[0] == '\0') {
        free(content);
        snprintf(result, rlen, "error: SOUL document not available");
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        free(content);
        snprintf(result, rlen, "error: cannot open SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    if (!dsco_flock(fd, F_WRLCK)) {
        free(content);
        close(fd);
        snprintf(result, rlen, "error: failed to lock SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    size_t content_len = strlen(content);
    size_t written = 0;
    bool ok = true;
    while (written < content_len) {
        ssize_t n = write(fd, content + written, content_len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        written += (size_t)n;
    }

    dsco_funlock(fd);
    close(fd);
    free(content);

    if (!ok) {
        snprintf(result, rlen, "error: append SOUL document failed: %s", strerror(errno));
        return false;
    }

    snprintf(result, rlen, "appended %zu bytes to SOUL document", written);
    return true;
}

static bool tool_soul_write(const char *input, char *result, size_t rlen) {
    char *content = json_get_str(input, "content");
    if (!content) {
        snprintf(result, rlen, "error: content required");
        return false;
    }

    char path[4096];
    dsco_workspace_doc_path("soul", path, sizeof(path));
    if (path[0] == '\0') {
        free(content);
        snprintf(result, rlen, "error: SOUL document not available");
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(content);
        snprintf(result, rlen, "error: cannot open SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    if (!dsco_flock(fd, F_WRLCK)) {
        free(content);
        close(fd);
        snprintf(result, rlen, "error: failed to lock SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    size_t content_len = strlen(content);
    size_t written = 0;
    bool ok = true;
    while (written < content_len) {
        ssize_t n = write(fd, content + written, content_len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        written += (size_t)n;
    }

    dsco_funlock(fd);
    close(fd);
    free(content);

    if (!ok) {
        snprintf(result, rlen, "error: write SOUL document failed: %s", strerror(errno));
        return false;
    }

    snprintf(result, rlen, "wrote %zu bytes to SOUL document", written);
    return true;
}

static bool tool_soul_replace(const char *input, char *result, size_t rlen) {
    char *old_str = json_get_str(input, "old_string");
    char *new_str = json_get_str(input, "new_string");
    if (!old_str || !new_str) {
        snprintf(result, rlen, "error: old_string and new_string required");
        free(old_str); free(new_str);
        return false;
    }
    if (old_str[0] == '\0') {
        snprintf(result, rlen, "error: old_string must not be empty");
        free(old_str); free(new_str);
        return false;
    }

    char path[4096];
    dsco_workspace_doc_path("soul", path, sizeof(path));
    if (path[0] == '\0') {
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: SOUL document not available");
        return false;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: cannot open SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    if (!dsco_flock(fd, F_WRLCK)) {
        free(old_str); free(new_str);
        close(fd);
        snprintf(result, rlen, "error: failed to lock SOUL document %s: %s", path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: cannot stat SOUL document %s: %s", path, strerror(errno));
        return false;
    }
    if ((size_t)st.st_size >= MAX_TOOL_RESULT) {
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: SOUL document too large to replace (%lld bytes)", (long long)st.st_size);
        return false;
    }

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: failed to read SOUL document: %s", strerror(errno));
        return false;
    }

    char *content = safe_malloc((size_t)st.st_size + 1);
    ssize_t want = (ssize_t)st.st_size;
    ssize_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, content + got, (size_t)(want - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(content);
            dsco_funlock(fd);
            close(fd);
            free(old_str); free(new_str);
            snprintf(result, rlen, "error: read SOUL document failed: %s", strerror(errno));
            return false;
        }
        if (n == 0) break;
        got += n;
    }
    content[got] = '\0';

    char *loc = strstr(content, old_str);
    if (!loc) {
        free(content);
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: old_string not found in SOUL document");
        return false;
    }

    bool replace_all = json_get_bool(input, "replace_all", false);
    char *second = strstr(loc + 1, old_str);
    if (second && !replace_all) {
        free(content);
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen,
                 "error: old_string appears multiple times in SOUL document. "
                 "Provide more context or set replace_all=true");
        return false;
    }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t base_len = (size_t)got;
    jbuf_t out;
    jbuf_init(&out, base_len + new_len + 256);

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

    if (ftruncate(fd, 0) != 0) {
        jbuf_free(&out);
        free(content);
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: truncate SOUL document failed: %s", strerror(errno));
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        jbuf_free(&out);
        free(content);
        dsco_funlock(fd);
        close(fd);
        free(old_str); free(new_str);
        snprintf(result, rlen, "error: failed to rewrite SOUL document: %s", strerror(errno));
        return false;
    }

    size_t pos = 0;
    bool write_ok = true;
    while (pos < out.len) {
        ssize_t n = write(fd, out.data + pos, out.len - pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            write_ok = false;
            break;
        }
        pos += (size_t)n;
    }

    dsco_funlock(fd);
    close(fd);
    free(content);
    jbuf_free(&out);
    free(old_str); free(new_str);
    if (!write_ok) {
        snprintf(result, rlen, "error: write SOUL document failed: %s", strerror(errno));
        return false;
    }

    snprintf(result, rlen, "replaced SOUL document: %d replacement(s)", replacements);
    return true;
}

static bool tool_read_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    if (!require_regular_file(path, result, rlen)) {
        free(path);
        return false;
    }

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
    if (!require_regular_file(path, result, rlen)) {
        free(path);
        return false;
    }

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
    if (old_len == 0) {
        snprintf(result, rlen, "error: old_string must not be empty");
        free(content); free(path); free(old_str); free(new_str);
        return false;
    }
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

    int status = run_cmd(cmd.data, result, rlen);
    if (body_tmpfile[0]) unlink(body_tmpfile);
    jbuf_free(&cmd);
    free(url); free(method); free(headers_str); free(body);
    if (status != 0 && result[0] == '\0') {
        snprintf(result, rlen, "http_request failed with status %d", status);
    }
    return (status == 0);
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

#define MQ_MAX_SYMBOLS 24

typedef struct {
    char symbol[32];
    char requested[32];
    char source[24];
    char source_symbol[40];
    char currency[16];
    char exchange[32];
    char instrument[24];
    char display_name[96];
    char date[16];
    char tstamp[16];
    time_t asof_epoch;
    double open;
    double high;
    double low;
    double close;
    double prev_close;
    long long volume;
    bool has_volume;
    bool has_prev_close;
    bool has_52w;
    bool stale;
    double fifty_two_week_high;
    double fifty_two_week_low;
    double change;
    double change_pct;
    double range_abs;
    double range_pct;
    double day_position_pct;
    double dist_to_52w_high_pct;
    double dist_from_52w_low_pct;
    char change_basis[24];
} market_row_t;

typedef struct {
    char values[MQ_MAX_SYMBOLS][32];
    int count;
} mq_symbol_list_t;

static bool mq_parse_double(const char *s, double *out) {
    if (!s || !*s || strcmp(s, "N/D") == 0) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool mq_parse_i64(const char *s, long long *out) {
    if (!s || !*s || strcmp(s, "N/D") == 0) return false;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool mq_parse_datetime(const char *date_yyyymmdd,
                              const char *time_hhmmss,
                              time_t *out_epoch) {
    if (!out_epoch || !date_yyyymmdd || !time_hhmmss) return false;
    if (strlen(date_yyyymmdd) != 8 || strlen(time_hhmmss) != 6) return false;
    for (int i = 0; i < 8; i++) if (!isdigit((unsigned char)date_yyyymmdd[i])) return false;
    for (int i = 0; i < 6; i++) if (!isdigit((unsigned char)time_hhmmss[i])) return false;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = ((date_yyyymmdd[0] - '0') * 1000 +
                   (date_yyyymmdd[1] - '0') * 100 +
                   (date_yyyymmdd[2] - '0') * 10 +
                   (date_yyyymmdd[3] - '0')) - 1900;
    tmv.tm_mon = ((date_yyyymmdd[4] - '0') * 10 +
                  (date_yyyymmdd[5] - '0')) - 1;
    tmv.tm_mday = ((date_yyyymmdd[6] - '0') * 10 +
                   (date_yyyymmdd[7] - '0'));
    tmv.tm_hour = ((time_hhmmss[0] - '0') * 10 +
                   (time_hhmmss[1] - '0'));
    tmv.tm_min = ((time_hhmmss[2] - '0') * 10 +
                  (time_hhmmss[3] - '0'));
    tmv.tm_sec = ((time_hhmmss[4] - '0') * 10 +
                  (time_hhmmss[5] - '0'));
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t <= 0) return false;
    *out_epoch = t;
    return true;
}

static void mq_fill_datetime_from_epoch(time_t epoch, char *date_out, size_t date_len,
                                        char *time_out, size_t time_len) {
    if (date_out && date_len > 0) date_out[0] = '\0';
    if (time_out && time_len > 0) time_out[0] = '\0';
    if (epoch <= 0 || !date_out || !time_out || date_len == 0 || time_len == 0) return;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
#if defined(__APPLE__) || defined(__linux__)
    if (!gmtime_r(&epoch, &tmv)) return;
#else
    struct tm *tmp = gmtime(&epoch);
    if (!tmp) return;
    tmv = *tmp;
#endif
    (void)strftime(date_out, date_len, "%Y%m%d", &tmv);
    (void)strftime(time_out, time_len, "%H%M%S", &tmv);
}

static void mq_recompute_metrics(market_row_t *row) {
    if (!row) return;

    double basis = row->open;
    snprintf(row->change_basis, sizeof(row->change_basis), "open");
    if (row->has_prev_close && fabs(row->prev_close) > 1e-9) {
        basis = row->prev_close;
        snprintf(row->change_basis, sizeof(row->change_basis), "prev_close");
    }
    if (fabs(basis) <= 1e-9) basis = row->close;

    row->change = row->close - basis;
    row->change_pct = (fabs(basis) > 1e-9) ? ((row->change / basis) * 100.0) : 0.0;

    row->range_abs = row->high - row->low;
    row->range_pct = (fabs(basis) > 1e-9) ? ((row->range_abs / basis) * 100.0) : 0.0;

    if (row->high > row->low + 1e-9) {
        row->day_position_pct = ((row->close - row->low) / (row->high - row->low)) * 100.0;
        if (row->day_position_pct < 0.0) row->day_position_pct = 0.0;
        if (row->day_position_pct > 100.0) row->day_position_pct = 100.0;
    } else {
        row->day_position_pct = 50.0;
    }

    if (row->has_52w && row->fifty_two_week_high > 0.0 && row->fifty_two_week_low > 0.0) {
        row->dist_to_52w_high_pct =
            ((row->close - row->fifty_two_week_high) / row->fifty_two_week_high) * 100.0;
        row->dist_from_52w_low_pct =
            ((row->close - row->fifty_two_week_low) / row->fifty_two_week_low) * 100.0;
    } else {
        row->dist_to_52w_high_pct = 0.0;
        row->dist_from_52w_low_pct = 0.0;
    }
}

static const char *mq_json_find_key(const char *json, const char *key) {
    if (!json || !key || !*key) return NULL;
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
    return strstr(json, needle);
}

static bool mq_json_extract_number(const char *json, const char *key, double *out) {
    if (!json || !key || !out) return false;
    const char *p = mq_json_find_key(json, key);
    if (!p) return false;
    while (*p && *p != ':') p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "null", 4) == 0) return false;

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool mq_json_extract_last_array_number(const char *json, const char *key, double *out) {
    if (!json || !key || !out) return false;
    const char *p = mq_json_find_key(json, key);
    if (!p) return false;
    while (*p && *p != '[') p++;
    if (*p != '[') return false;
    p++;

    bool have = false;
    double last = 0.0;
    while (*p && *p != ']') {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == ']') break;
        if (strncmp(p, "null", 4) == 0) {
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = strtod(p, &end);
        if (end != p && isfinite(v)) {
            last = v;
            have = true;
            p = end;
            continue;
        }
        p++;
    }
    if (!have) return false;
    *out = last;
    return true;
}

static bool mq_json_extract_string(const char *json, const char *key, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    const char *p = mq_json_find_key(json, key);
    if (!p) return false;
    while (*p && *p != ':') p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n' || *p == 'r' || *p == 't') out[o++] = ' ';
            else out[o++] = *p;
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

static bool mq_is_common_crypto_symbol(const char *sym) {
    if (!sym || !*sym) return false;
    return strcmp(sym, "BTC") == 0 ||
           strcmp(sym, "ETH") == 0 ||
           strcmp(sym, "SOL") == 0 ||
           strcmp(sym, "DOGE") == 0 ||
           strcmp(sym, "BNB") == 0 ||
           strcmp(sym, "XRP") == 0 ||
           strcmp(sym, "ADA") == 0 ||
           strcmp(sym, "LTC") == 0 ||
           strcmp(sym, "DOT") == 0 ||
           strcmp(sym, "AVAX") == 0 ||
           strcmp(sym, "LINK") == 0 ||
           strcmp(sym, "MATIC") == 0;
}

static bool mq_normalize_symbol(const char *raw, char *out, size_t out_len) {
    if (!raw || !out || out_len == 0) return false;
    out[0] = '\0';
    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) || c == ',') continue;
        if (isalnum(c) || c == '.' || c == '-' || c == '=' || c == '^') {
            out[o++] = (char)(isalpha(c) ? toupper(c) : c);
        }
    }
    out[o] = '\0';
    return o > 0;
}

static void mq_add_candidate(char candidates[][40], int *count, const char *cand) {
    if (!cand || !*cand || !count) return;
    if (*count >= 8) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(candidates[i], cand) == 0) return;
    }
    snprintf(candidates[*count], 40, "%s", cand);
    (*count)++;
}

static void mq_symbol_list_add(mq_symbol_list_t *list, const char *raw) {
    if (!list || !raw || !*raw) return;
    char norm[32];
    if (!mq_normalize_symbol(raw, norm, sizeof(norm))) return;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->values[i], norm) == 0) return;
    }
    if (list->count < MQ_MAX_SYMBOLS) {
        snprintf(list->values[list->count], sizeof(list->values[0]), "%s", norm);
        list->count++;
    }
}

static void mq_decode_json_token(const char *start, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!start) return;

    const char *p = start;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') {
        size_t o = 0;
        while (*p && *p != ',' && *p != ']' && !isspace((unsigned char)*p) && o + 1 < out_len) {
            out[o++] = *p++;
        }
        out[o] = '\0';
        return;
    }

    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n' || *p == 'r' || *p == 't') out[o++] = ' ';
            else out[o++] = *p;
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static void mq_array_symbol_cb(const char *element_start, void *ctx) {
    mq_symbol_list_t *list = (mq_symbol_list_t *)ctx;
    if (!list || !element_start) return;
    char raw[64];
    mq_decode_json_token(element_start, raw, sizeof(raw));
    if (raw[0]) mq_symbol_list_add(list, raw);
}

static void mq_collect_symbols(const char *input, mq_symbol_list_t *list) {
    char *symbol = json_get_str(input, "symbol");
    char *ticker = json_get_str(input, "ticker");
    char *symbols_str = json_get_str(input, "symbols");

    if (symbol) mq_symbol_list_add(list, symbol);
    if (ticker) mq_symbol_list_add(list, ticker);

    if (symbols_str && *symbols_str) {
        char *copy = safe_strdup(symbols_str);
        char *tok = strtok(copy, ", \t\r\n");
        while (tok) {
            mq_symbol_list_add(list, tok);
            tok = strtok(NULL, ", \t\r\n");
        }
        free(copy);
    }

    (void)json_array_foreach(input, "symbols", mq_array_symbol_cb, list);

    free(symbol);
    free(ticker);
    free(symbols_str);
}

static bool mq_parse_stooq_csv(const char *csv, const char *requested, market_row_t *row) {
    if (!csv || !*csv || !row) return false;
    char *tmp = safe_strdup(csv);
    char *nl = strchr(tmp, '\n');
    if (nl) *nl = '\0';

    char *fields[10] = {0};
    int n = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(tmp, ",", &saveptr);
         tok && n < 10;
         tok = strtok_r(NULL, ",", &saveptr)) {
        fields[n++] = tok;
    }

    bool ok = false;
    do {
        if (n < 7) break;
        if (strcmp(fields[1], "N/D") == 0 || strcmp(fields[6], "N/D") == 0) break;

        memset(row, 0, sizeof(*row));
        snprintf(row->source, sizeof(row->source), "stooq_csv");
        snprintf(row->source_symbol, sizeof(row->source_symbol), "%s", fields[0] ? fields[0] : "");
        snprintf(row->requested, sizeof(row->requested), "%s", requested ? requested : "");
        snprintf(row->symbol, sizeof(row->symbol), "%s",
                 row->source_symbol[0] ? row->source_symbol : row->requested);
        snprintf(row->date, sizeof(row->date), "%s", fields[1] ? fields[1] : "");
        snprintf(row->tstamp, sizeof(row->tstamp), "%s", fields[2] ? fields[2] : "");
        if (!mq_parse_double(fields[3], &row->open)) break;
        if (!mq_parse_double(fields[4], &row->high)) break;
        if (!mq_parse_double(fields[5], &row->low)) break;
        if (!mq_parse_double(fields[6], &row->close)) break;
        row->has_volume = (n > 7 && mq_parse_i64(fields[7], &row->volume));

        row->asof_epoch = 0;
        (void)mq_parse_datetime(row->date, row->tstamp, &row->asof_epoch);
        row->has_prev_close = false;
        row->has_52w = false;
        mq_recompute_metrics(row);
        ok = true;
    } while (0);

    free(tmp);
    return ok;
}

static bool mq_fetch_stooq_quote(const char *normalized, int timeout, market_row_t *row) {
    if (!normalized || !*normalized || !row) return false;

    char lower[32];
    size_t lo = 0;
    for (; normalized[lo] && lo + 1 < sizeof(lower); lo++) {
        lower[lo] = (char)tolower((unsigned char)normalized[lo]);
    }
    lower[lo] = '\0';

    char compact[32];
    size_t co = 0;
    for (size_t i = 0; lower[i] && co + 1 < sizeof(compact); i++) {
        if (isalnum((unsigned char)lower[i]) || lower[i] == '.') compact[co++] = lower[i];
    }
    compact[co] = '\0';

    char no_forex[32];
    snprintf(no_forex, sizeof(no_forex), "%s", lower);
    size_t nfx = strlen(no_forex);
    if (nfx > 2 && no_forex[nfx - 2] == '=' && no_forex[nfx - 1] == 'x') no_forex[nfx - 2] = '\0';

    char us_suffix[40] = "";
    if (!strchr(lower, '.') && !strchr(lower, '-') && !strchr(lower, '=') && strlen(lower) <= 6) {
        snprintf(us_suffix, sizeof(us_suffix), "%s.us", lower);
    }

    char no_us_suffix[32] = "";
    if (strstr(lower, ".us")) {
        snprintf(no_us_suffix, sizeof(no_us_suffix), "%s", lower);
        char *dot = strstr(no_us_suffix, ".us");
        if (dot) *dot = '\0';
    }

    char candidates[8][40];
    int candidate_count = 0;
    mq_add_candidate(candidates, &candidate_count, lower);
    mq_add_candidate(candidates, &candidate_count, no_forex);
    mq_add_candidate(candidates, &candidate_count, compact);
    mq_add_candidate(candidates, &candidate_count, us_suffix);
    mq_add_candidate(candidates, &candidate_count, no_us_suffix);

    char response[4096];
    for (int i = 0; i < candidate_count; i++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "curl -sS -L --max-time %d 'https://stooq.com/q/l/?s=%s&i=5'",
                 timeout, candidates[i]);
        response[0] = '\0';
        int status = run_cmd(cmd, response, sizeof(response));
        if (status != 0 || response[0] == '\0') continue;
        if (mq_parse_stooq_csv(response, normalized, row)) return true;
    }
    return false;
}

static void mq_add_yahoo_alias_candidate(char candidates[][40], int *count, const char *normalized) {
    if (!normalized || !*normalized) return;
    if (strcmp(normalized, "SPX") == 0) mq_add_candidate(candidates, count, "^GSPC");
    if (strcmp(normalized, "NDX") == 0) mq_add_candidate(candidates, count, "^NDX");
    if (strcmp(normalized, "DJI") == 0) mq_add_candidate(candidates, count, "^DJI");
    if (strcmp(normalized, "VIX") == 0) mq_add_candidate(candidates, count, "^VIX");
}

static bool mq_parse_yahoo_chart_json(const char *json, const char *requested, market_row_t *row) {
    if (!json || !*json || !row) return false;
    if (strstr(json, "\"result\":null") || strstr(json, "Too Many Requests")) return false;

    memset(row, 0, sizeof(*row));
    snprintf(row->source, sizeof(row->source), "yahoo_chart");
    snprintf(row->requested, sizeof(row->requested), "%s", requested ? requested : "");

    if (!mq_json_extract_string(json, "symbol", row->symbol, sizeof(row->symbol))) {
        snprintf(row->symbol, sizeof(row->symbol), "%s", row->requested);
    }
    snprintf(row->source_symbol, sizeof(row->source_symbol), "%s", row->symbol);

    (void)mq_json_extract_string(json, "currency", row->currency, sizeof(row->currency));
    if (!mq_json_extract_string(json, "exchangeName", row->exchange, sizeof(row->exchange))) {
        (void)mq_json_extract_string(json, "fullExchangeName", row->exchange, sizeof(row->exchange));
    }
    (void)mq_json_extract_string(json, "instrumentType", row->instrument, sizeof(row->instrument));
    if (!mq_json_extract_string(json, "shortName", row->display_name, sizeof(row->display_name))) {
        (void)mq_json_extract_string(json, "longName", row->display_name, sizeof(row->display_name));
    }

    if (!mq_json_extract_number(json, "regularMarketPrice", &row->close)) {
        if (!mq_json_extract_last_array_number(json, "close", &row->close)) {
            return false;
        }
    }

    if (!mq_json_extract_last_array_number(json, "open", &row->open)) row->open = row->close;
    if (!mq_json_extract_number(json, "regularMarketDayHigh", &row->high)) {
        if (!mq_json_extract_last_array_number(json, "high", &row->high)) row->high = row->close;
    }
    if (!mq_json_extract_number(json, "regularMarketDayLow", &row->low)) {
        if (!mq_json_extract_last_array_number(json, "low", &row->low)) row->low = row->close;
    }
    if (row->high < row->low) {
        double tmp = row->high;
        row->high = row->low;
        row->low = tmp;
    }

    row->has_prev_close = mq_json_extract_number(json, "chartPreviousClose", &row->prev_close);
    if (!row->has_prev_close) {
        row->prev_close = row->open;
    }

    double y_high = 0.0;
    double y_low = 0.0;
    if (mq_json_extract_number(json, "fiftyTwoWeekHigh", &y_high) &&
        mq_json_extract_number(json, "fiftyTwoWeekLow", &y_low) &&
        y_high > 0.0 && y_low > 0.0 && y_high >= y_low) {
        row->has_52w = true;
        row->fifty_two_week_high = y_high;
        row->fifty_two_week_low = y_low;
    }

    double vol = 0.0;
    if (mq_json_extract_number(json, "regularMarketVolume", &vol) ||
        mq_json_extract_last_array_number(json, "volume", &vol)) {
        if (vol >= 0.0) {
            row->volume = (long long)llround(vol);
            row->has_volume = true;
        }
    }

    double market_time = 0.0;
    if (mq_json_extract_number(json, "regularMarketTime", &market_time) && market_time > 0.0) {
        row->asof_epoch = (time_t)llround(market_time);
        mq_fill_datetime_from_epoch(row->asof_epoch, row->date, sizeof(row->date),
                                    row->tstamp, sizeof(row->tstamp));
    }

    mq_recompute_metrics(row);
    return true;
}

static bool mq_fetch_yahoo_quote(const char *normalized, int timeout, market_row_t *row) {
    if (!normalized || !*normalized || !row) return false;

    char candidates[8][40];
    int candidate_count = 0;

    bool alpha_only = true;
    size_t len = strlen(normalized);
    for (size_t i = 0; i < len; i++) {
        if (!isalpha((unsigned char)normalized[i])) {
            alpha_only = false;
            break;
        }
    }
    if (alpha_only && mq_is_common_crypto_symbol(normalized)) {
        char crypto[40];
        snprintf(crypto, sizeof(crypto), "%s-USD", normalized);
        mq_add_candidate(candidates, &candidate_count, crypto);
    }
    if (alpha_only && len == 6) {
        char fx[40];
        snprintf(fx, sizeof(fx), "%s=X", normalized);
        mq_add_candidate(candidates, &candidate_count, fx);
    }

    mq_add_candidate(candidates, &candidate_count, normalized);
    mq_add_yahoo_alias_candidate(candidates, &candidate_count, normalized);

    char stripped_us[40] = "";
    const char *dot_us = strstr(normalized, ".US");
    if (dot_us) {
        size_t n = (size_t)(dot_us - normalized);
        if (n > 0 && n + 1 < sizeof(stripped_us)) {
            memcpy(stripped_us, normalized, n);
            stripped_us[n] = '\0';
            mq_add_candidate(candidates, &candidate_count, stripped_us);
        }
    }

    char response[32768];
    for (int i = 0; i < candidate_count; i++) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "curl -sS -L -H 'User-Agent: Mozilla/5.0' --max-time %d "
                 "'https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=5d'",
                 timeout, candidates[i]);
        response[0] = '\0';
        int status = run_cmd(cmd, response, sizeof(response));
        if (status != 0 || response[0] == '\0') continue;
        if (mq_parse_yahoo_chart_json(response, normalized, row)) return true;
    }

    return false;
}

static void mq_format_volume(long long volume, bool has_volume, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!has_volume) {
        snprintf(out, out_len, "n/a");
        return;
    }
    double v = (double)volume;
    if (v >= 1000000000.0) snprintf(out, out_len, "%.2fB", v / 1000000000.0);
    else if (v >= 1000000.0) snprintf(out, out_len, "%.2fM", v / 1000000.0);
    else if (v >= 1000.0) snprintf(out, out_len, "%.2fK", v / 1000.0);
    else snprintf(out, out_len, "%lld", volume);
}

static long mq_compute_age_seconds(time_t asof_epoch, time_t now_epoch) {
    if (asof_epoch <= 0 || now_epoch <= 0) return -1;
    long diff = (long)(now_epoch - asof_epoch);
    if (diff >= 0) return diff;
    /* Stooq timestamps can appear ahead due to timezone ambiguity. */
    if (diff > -(long)(18 * 60 * 60)) return 0;
    return -1;
}

static bool tool_market_quote(const char *input, char *result, size_t rlen) {
    int timeout = json_get_int(input, "timeout", 12);
    if (timeout < 3) timeout = 3;
    if (timeout > 30) timeout = 30;

    int stale_after_sec = json_get_int(input, "stale_after_sec", 60 * 60 * 48);
    if (stale_after_sec < 60) stale_after_sec = 60;
    if (stale_after_sec > 60 * 60 * 24 * 30) stale_after_sec = 60 * 60 * 24 * 30;

    char *format = json_get_str(input, "format");
    bool json_out = (format && strcasecmp(format, "json") == 0);

    char *source_pref = json_get_str(input, "source_preference");
    bool prefer_yahoo = true;
    bool allow_yahoo = true;
    bool allow_stooq = true;
    const char *pref_label = "auto";
    if (source_pref && *source_pref) {
        if (strcasecmp(source_pref, "stooq") == 0) {
            prefer_yahoo = false;
            pref_label = "stooq";
        } else if (strcasecmp(source_pref, "yahoo") == 0) {
            prefer_yahoo = true;
            pref_label = "yahoo";
        } else if (strcasecmp(source_pref, "stooq_only") == 0) {
            prefer_yahoo = false;
            allow_yahoo = false;
            pref_label = "stooq_only";
        } else if (strcasecmp(source_pref, "yahoo_only") == 0) {
            prefer_yahoo = true;
            allow_stooq = false;
            pref_label = "yahoo_only";
        }
    }

    mq_symbol_list_t symbols = {0};
    mq_collect_symbols(input, &symbols);
    if (symbols.count == 0) {
        snprintf(result, rlen, "error: symbol/ticker/symbols required");
        free(format);
        free(source_pref);
        return false;
    }

    market_row_t rows[MQ_MAX_SYMBOLS];
    char failed[MQ_MAX_SYMBOLS][32];
    int ok_count = 0;
    int fail_count = 0;
    time_t now = time(NULL);
    double sum_change_pct = 0.0;
    int sum_count = 0;
    int source_yahoo_count = 0;
    int source_stooq_count = 0;

    for (int i = 0; i < symbols.count; i++) {
        market_row_t row;
        bool ok = false;
        if (prefer_yahoo) {
            if (allow_yahoo) ok = mq_fetch_yahoo_quote(symbols.values[i], timeout, &row);
            if (!ok && allow_stooq) ok = mq_fetch_stooq_quote(symbols.values[i], timeout, &row);
        } else {
            if (allow_stooq) ok = mq_fetch_stooq_quote(symbols.values[i], timeout, &row);
            if (!ok && allow_yahoo) ok = mq_fetch_yahoo_quote(symbols.values[i], timeout, &row);
        }

        if (ok) {
            row.stale = false;
            long age = mq_compute_age_seconds(row.asof_epoch, now);
            if (age >= 0 && age > stale_after_sec) {
                row.stale = true;
            }
            if (strcmp(row.source, "yahoo_chart") == 0) source_yahoo_count++;
            else if (strcmp(row.source, "stooq_csv") == 0) source_stooq_count++;
            rows[ok_count++] = row;
            sum_change_pct += row.change_pct;
            sum_count++;
        } else {
            snprintf(failed[fail_count], sizeof(failed[fail_count]), "%s", symbols.values[i]);
            fail_count++;
        }
    }

    if (ok_count == 0) {
        snprintf(result, rlen, "market_quote failed requested=%d source_preference=%s", symbols.count, pref_label);
        free(format);
        free(source_pref);
        return false;
    }

    const char *source_label = rows[0].source[0] ? rows[0].source : "unknown";
    bool mixed_source = false;
    for (int i = 1; i < ok_count; i++) {
        const char *s = rows[i].source[0] ? rows[i].source : "unknown";
        if (strcmp(s, source_label) != 0) {
            mixed_source = true;
            break;
        }
    }
    if (mixed_source) source_label = "mixed";

    int best_up = 0;
    int best_down = 0;
    for (int i = 1; i < ok_count; i++) {
        if (rows[i].change_pct > rows[best_up].change_pct) best_up = i;
        if (rows[i].change_pct < rows[best_down].change_pct) best_down = i;
    }
    double avg_change_pct = (sum_count > 0) ? (sum_change_pct / (double)sum_count) : 0.0;

    if (json_out) {
        jbuf_t b;
        jbuf_init(&b, 4096);
        jbuf_append(&b, "{\"source\":");
        jbuf_append_json_str(&b, source_label);
        jbuf_append(&b, ",\"source_preference\":");
        jbuf_append_json_str(&b, pref_label);
        jbuf_append(&b, ",\"source_breakdown\":{\"yahoo_chart\":");
        jbuf_append_int(&b, source_yahoo_count);
        jbuf_append(&b, ",\"stooq_csv\":");
        jbuf_append_int(&b, source_stooq_count);
        jbuf_append(&b, "},\"requested\":");
        jbuf_append_int(&b, symbols.count);
        jbuf_append(&b, ",\"success\":");
        jbuf_append_int(&b, ok_count);
        jbuf_append(&b, ",\"failed\":");
        jbuf_append_int(&b, fail_count);
        jbuf_append(&b, ",\"avg_change_percent\":");
        char nbuf[64];
        snprintf(nbuf, sizeof(nbuf), "%.4f", avg_change_pct);
        jbuf_append(&b, nbuf);
        jbuf_append(&b, ",\"quotes\":[");
        for (int i = 0; i < ok_count; i++) {
            if (i > 0) jbuf_append(&b, ",");
            long age = mq_compute_age_seconds(rows[i].asof_epoch, now);
            jbuf_append(&b, "{\"symbol\":");
            jbuf_append_json_str(&b, rows[i].symbol);
            jbuf_append(&b, ",\"source\":");
            jbuf_append_json_str(&b, rows[i].source);
            jbuf_append(&b, ",\"requested\":");
            jbuf_append_json_str(&b, rows[i].requested);
            jbuf_append(&b, ",\"name\":");
            if (rows[i].display_name[0]) jbuf_append_json_str(&b, rows[i].display_name);
            else jbuf_append(&b, "null");
            jbuf_append(&b, ",\"currency\":");
            if (rows[i].currency[0]) jbuf_append_json_str(&b, rows[i].currency);
            else jbuf_append(&b, "null");
            jbuf_append(&b, ",\"exchange\":");
            if (rows[i].exchange[0]) jbuf_append_json_str(&b, rows[i].exchange);
            else jbuf_append(&b, "null");
            jbuf_append(&b, ",\"instrument\":");
            if (rows[i].instrument[0]) jbuf_append_json_str(&b, rows[i].instrument);
            else jbuf_append(&b, "null");
            jbuf_append(&b, ",\"price\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].close);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"open\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].open);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"previous_close\":");
            if (rows[i].has_prev_close) {
                snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].prev_close);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, "null");
            }
            jbuf_append(&b, ",\"day_high\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].high);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"day_low\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].low);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"change\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].change);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"change_percent\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].change_pct);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"change_basis\":");
            jbuf_append_json_str(&b, rows[i].change_basis);
            jbuf_append(&b, ",\"range\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].range_abs);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"range_percent\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].range_pct);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"close_position_percent\":");
            snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].day_position_pct);
            jbuf_append(&b, nbuf);
            jbuf_append(&b, ",\"fifty_two_week_low\":");
            if (rows[i].has_52w) {
                snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].fifty_two_week_low);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, "null");
            }
            jbuf_append(&b, ",\"fifty_two_week_high\":");
            if (rows[i].has_52w) {
                snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].fifty_two_week_high);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, "null");
            }
            jbuf_append(&b, ",\"distance_to_52w_high_percent\":");
            if (rows[i].has_52w) {
                snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].dist_to_52w_high_pct);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, "null");
            }
            jbuf_append(&b, ",\"distance_from_52w_low_percent\":");
            if (rows[i].has_52w) {
                snprintf(nbuf, sizeof(nbuf), "%.6f", rows[i].dist_from_52w_low_pct);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, "null");
            }
            if (rows[i].has_volume) {
                jbuf_append(&b, ",\"volume\":");
                snprintf(nbuf, sizeof(nbuf), "%lld", rows[i].volume);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, ",\"volume\":null");
            }
            jbuf_append(&b, ",\"as_of_date\":");
            jbuf_append_json_str(&b, rows[i].date);
            jbuf_append(&b, ",\"as_of_time\":");
            jbuf_append_json_str(&b, rows[i].tstamp);
            jbuf_append(&b, ",\"as_of_epoch\":");
            snprintf(nbuf, sizeof(nbuf), "%lld", (long long)rows[i].asof_epoch);
            jbuf_append(&b, nbuf);
            if (age >= 0) {
                jbuf_append(&b, ",\"age_seconds\":");
                snprintf(nbuf, sizeof(nbuf), "%ld", age);
                jbuf_append(&b, nbuf);
            } else {
                jbuf_append(&b, ",\"age_seconds\":null");
            }
            jbuf_append(&b, ",\"stale\":");
            jbuf_append(&b, rows[i].stale ? "true" : "false");
            jbuf_append(&b, "}");
        }
        jbuf_append(&b, "]");

        jbuf_append(&b, ",\"summary\":{\"top_gainer\":");
        jbuf_append_json_str(&b, rows[best_up].symbol);
        jbuf_append(&b, ",\"top_gainer_change_percent\":");
        snprintf(nbuf, sizeof(nbuf), "%.6f", rows[best_up].change_pct);
        jbuf_append(&b, nbuf);
        jbuf_append(&b, ",\"top_loser\":");
        jbuf_append_json_str(&b, rows[best_down].symbol);
        jbuf_append(&b, ",\"top_loser_change_percent\":");
        snprintf(nbuf, sizeof(nbuf), "%.6f", rows[best_down].change_pct);
        jbuf_append(&b, nbuf);
        jbuf_append(&b, "}");

        if (fail_count > 0) {
            jbuf_append(&b, ",\"failed_symbols\":[");
            for (int i = 0; i < fail_count; i++) {
                if (i > 0) jbuf_append(&b, ",");
                jbuf_append_json_str(&b, failed[i]);
            }
            jbuf_append(&b, "]");
        }
        jbuf_append(&b, "}");

        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
        free(format);
        free(source_pref);
        return true;
    }

    if (ok_count == 1) {
        char vol[32];
        char age_buf[32];
        char prev_close_buf[32];
        char w52_low_buf[32];
        char w52_high_buf[32];
        char from_low_buf[32];
        char to_high_buf[32];
        mq_format_volume(rows[0].volume, rows[0].has_volume, vol, sizeof(vol));
        long age = mq_compute_age_seconds(rows[0].asof_epoch, now);
        if (age >= 0) snprintf(age_buf, sizeof(age_buf), "%ld", age);
        else snprintf(age_buf, sizeof(age_buf), "unknown");
        if (rows[0].has_prev_close) snprintf(prev_close_buf, sizeof(prev_close_buf), "%.2f", rows[0].prev_close);
        else snprintf(prev_close_buf, sizeof(prev_close_buf), "n/a");
        if (rows[0].has_52w) {
            snprintf(w52_low_buf, sizeof(w52_low_buf), "%.2f", rows[0].fifty_two_week_low);
            snprintf(w52_high_buf, sizeof(w52_high_buf), "%.2f", rows[0].fifty_two_week_high);
            snprintf(from_low_buf, sizeof(from_low_buf), "%+.2f%%", rows[0].dist_from_52w_low_pct);
            snprintf(to_high_buf, sizeof(to_high_buf), "%+.2f%%", rows[0].dist_to_52w_high_pct);
        } else {
            snprintf(w52_low_buf, sizeof(w52_low_buf), "n/a");
            snprintf(w52_high_buf, sizeof(w52_high_buf), "n/a");
            snprintf(from_low_buf, sizeof(from_low_buf), "n/a");
            snprintf(to_high_buf, sizeof(to_high_buf), "n/a");
        }
        snprintf(result, rlen,
                 "market_quote symbol=%s source=%s\n"
                 "price=%.2f%s%s change=%+.2f change_percent=%+.2f%% basis=%s\n"
                 "open=%.2f prev_close=%s day_high=%.2f day_low=%.2f range=%.2f range_percent=%.2f%% close_position=%.1f%% volume=%s\n"
                 "52w_low=%s 52w_high=%s distance_from_52w_low=%s distance_to_52w_high=%s\n"
                 "exchange=%s instrument=%s name=%s\n"
                 "as_of_date=%s as_of_time=%s as_of_epoch=%lld age_seconds=%s stale=%s",
                 rows[0].symbol,
                 rows[0].source[0] ? rows[0].source : "unknown",
                 rows[0].close,
                 rows[0].currency[0] ? " " : "", rows[0].currency[0] ? rows[0].currency : "",
                 rows[0].change, rows[0].change_pct, rows[0].change_basis,
                 rows[0].open, prev_close_buf, rows[0].high, rows[0].low,
                 rows[0].range_abs, rows[0].range_pct, rows[0].day_position_pct, vol,
                 w52_low_buf, w52_high_buf, from_low_buf, to_high_buf,
                 rows[0].exchange[0] ? rows[0].exchange : "n/a",
                 rows[0].instrument[0] ? rows[0].instrument : "n/a",
                 rows[0].display_name[0] ? rows[0].display_name : "n/a",
                 rows[0].date[0] ? rows[0].date : "n/a",
                 rows[0].tstamp[0] ? rows[0].tstamp : "n/a",
                 (long long)rows[0].asof_epoch,
                 age_buf,
                 rows[0].stale ? "yes" : "no");
        free(format);
        free(source_pref);
        return true;
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "market_quote source=%s requested=%d success=%d failed=%d avg_change_percent=%+.2f%% pref=%s\n"
                     "top_gainer=%s(%+.2f%%) top_loser=%s(%+.2f%%) sources[yahoo=%d stooq=%d]\n",
                     source_label, symbols.count, ok_count, fail_count, avg_change_pct, pref_label,
                     rows[best_up].symbol, rows[best_up].change_pct,
                     rows[best_down].symbol, rows[best_down].change_pct,
                     source_yahoo_count, source_stooq_count);
    if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;

    for (int i = 0; i < ok_count && off + 128 < rlen; i++) {
        char vol[32];
        char age_buf[32];
        char w52_buf[64];
        mq_format_volume(rows[i].volume, rows[i].has_volume, vol, sizeof(vol));
        long age = mq_compute_age_seconds(rows[i].asof_epoch, now);
        if (age >= 0) snprintf(age_buf, sizeof(age_buf), "%lds", age);
        else snprintf(age_buf, sizeof(age_buf), "unknown");
        if (rows[i].has_52w) snprintf(w52_buf, sizeof(w52_buf), "%+.1f/%+.1f%%",
                                      rows[i].dist_from_52w_low_pct, rows[i].dist_to_52w_high_pct);
        else snprintf(w52_buf, sizeof(w52_buf), "n/a");
        n = snprintf(result + off, rlen - off,
                     "- %s price=%.2f%s%s chg=%+.2f(%+.2f%%,%s) range=%.2f(%.2f%%) pos=%.0f%% 52w=%s vol=%s src=%s age=%s stale=%s\n",
                     rows[i].symbol, rows[i].close,
                     rows[i].currency[0] ? " " : "", rows[i].currency[0] ? rows[i].currency : "",
                     rows[i].change, rows[i].change_pct, rows[i].change_basis,
                     rows[i].range_abs, rows[i].range_pct, rows[i].day_position_pct, w52_buf, vol,
                     rows[i].source[0] ? rows[i].source : "unknown", age_buf,
                     rows[i].stale ? "yes" : "no");
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }

    if (fail_count > 0 && off + 48 < rlen) {
        n = snprintf(result + off, rlen - off, "failed_symbols=");
        if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
        for (int i = 0; i < fail_count && off + 4 < rlen; i++) {
            n = snprintf(result + off, rlen - off, "%s%s", (i ? "," : ""), failed[i]);
            if (n < 0 || (size_t)n >= rlen - off) break;
            off += (size_t)n;
        }
    }

    free(format);
    free(source_pref);
    return true;
}

/* ── HTML-to-text extraction ───────────────────────────────────────────── */

/* Strip HTML tags and extract readable text. Returns plain text. */
static void html_to_text(const char *html, char *out, size_t out_len) {
    size_t oi = 0;
    bool in_tag = false;
    bool in_script = false;
    bool in_style = false;
    bool last_was_space = false;
    const char *p = html;

    while (*p && oi < out_len - 1) {
        if (in_script) {
            if (strncasecmp(p, "</script", 8) == 0) in_script = false;
            p++;
            continue;
        }
        if (in_style) {
            if (strncasecmp(p, "</style", 7) == 0) in_style = false;
            p++;
            continue;
        }

        if (*p == '<') {
            in_tag = true;
            if (strncasecmp(p, "<script", 7) == 0) in_script = true;
            if (strncasecmp(p, "<style", 6) == 0) in_style = true;
            /* Block elements get newlines */
            if (strncasecmp(p, "<br", 3) == 0 || strncasecmp(p, "<p", 2) == 0 ||
                strncasecmp(p, "<div", 4) == 0 || strncasecmp(p, "<h1", 3) == 0 ||
                strncasecmp(p, "<h2", 3) == 0 || strncasecmp(p, "<h3", 3) == 0 ||
                strncasecmp(p, "<h4", 3) == 0 || strncasecmp(p, "<h5", 3) == 0 ||
                strncasecmp(p, "<h6", 3) == 0 || strncasecmp(p, "<li", 3) == 0 ||
                strncasecmp(p, "<tr", 3) == 0 || strncasecmp(p, "<td", 3) == 0 ||
                strncasecmp(p, "</p", 3) == 0 || strncasecmp(p, "</div", 5) == 0 ||
                strncasecmp(p, "</tr", 4) == 0) {
                if (oi > 0 && out[oi-1] != '\n') {
                    out[oi++] = '\n';
                    last_was_space = true;
                }
            }
            p++;
            continue;
        }
        if (*p == '>') {
            in_tag = false;
            p++;
            continue;
        }
        if (in_tag) { p++; continue; }

        /* Decode common HTML entities */
        if (*p == '&') {
            if (strncmp(p, "&amp;", 5) == 0) { out[oi++] = '&'; p += 5; continue; }
            if (strncmp(p, "&lt;", 4) == 0) { out[oi++] = '<'; p += 4; continue; }
            if (strncmp(p, "&gt;", 4) == 0) { out[oi++] = '>'; p += 4; continue; }
            if (strncmp(p, "&quot;", 6) == 0) { out[oi++] = '"'; p += 6; continue; }
            if (strncmp(p, "&apos;", 6) == 0) { out[oi++] = '\''; p += 6; continue; }
            if (strncmp(p, "&nbsp;", 6) == 0) { out[oi++] = ' '; p += 6; continue; }
            if (strncmp(p, "&#", 2) == 0) {
                const char *sc = strchr(p, ';');
                if (sc && sc - p < 10) { out[oi++] = ' '; p = sc + 1; continue; }
            }
        }

        /* Collapse whitespace */
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            if (!last_was_space && oi > 0) { out[oi++] = ' '; last_was_space = true; }
            p++;
            continue;
        }
        if (*p == '\n') {
            if (!last_was_space && oi > 0) { out[oi++] = '\n'; last_was_space = true; }
            p++;
            continue;
        }

        out[oi++] = *p;
        last_was_space = false;
        p++;
    }
    out[oi] = '\0';

    /* Collapse multiple blank lines to max 2 */
    char *r = out, *w = out;
    int blank_count = 0;
    while (*r) {
        if (*r == '\n') {
            blank_count++;
            if (blank_count <= 2) *w++ = *r;
        } else {
            blank_count = 0;
            *w++ = *r;
        }
        r++;
    }
    *w = '\0';
}

static bool tool_web_extract(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    if (!url) { snprintf(result, rlen, "error: url required"); return false; }
    int max_chars = json_get_int(input, "max_chars", 8000);
    if (max_chars > (int)rlen - 256) max_chars = (int)rlen - 256;
    if (max_chars < 100) max_chars = 100;

    /* Fetch HTML */
    char *html = safe_malloc(MAX_TOOL_RESULT);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "curl -sS -L --max-time 15 -H 'User-Agent: Mozilla/5.0' '%s'", url);
    run_cmd(cmd, html, MAX_TOOL_RESULT);

    if (strlen(html) == 0) {
        snprintf(result, rlen, "error: failed to fetch %s", url);
        free(html); free(url);
        return false;
    }

    /* Extract text */
    char *text = safe_malloc(max_chars + 1);
    html_to_text(html, text, max_chars);
    free(html);

    /* Build result */
    jbuf_t b;
    jbuf_init(&b, max_chars + 256);
    jbuf_append(&b, "{\"url\":");
    jbuf_append_json_str(&b, url);
    jbuf_append(&b, ",\"chars\":");
    jbuf_append_int(&b, (int)strlen(text));
    jbuf_append(&b, ",\"text\":");
    jbuf_append_json_str(&b, text);
    jbuf_append(&b, "}");

    int wn = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, wn);
    result[wn] = '\0';
    jbuf_free(&b);
    free(text);
    free(url);
    return true;
}

/* ── Screenshot (macOS) ────────────────────────────────────────────────── */

static bool tool_screenshot(const char *input, char *result, size_t rlen) {
    char *output_path = json_get_str(input, "path");
    bool full_screen = json_get_bool(input, "full_screen", true);
    int delay = json_get_int(input, "delay", 0);

    if (!output_path) output_path = safe_strdup("/tmp/dsco_screenshot.png");

    char cmd[1024];
    if (full_screen) {
        if (delay > 0) {
            snprintf(cmd, sizeof(cmd), "screencapture -T %d '%s'", delay, output_path);
        } else {
            snprintf(cmd, sizeof(cmd), "screencapture -x '%s'", output_path);
        }
    } else {
        /* Interactive selection */
        snprintf(cmd, sizeof(cmd), "screencapture -i '%s'", output_path);
    }

    run_cmd(cmd, result, rlen);

    /* Check if file was created */
    struct stat st;
    if (stat(output_path, &st) == 0) {
        snprintf(result, rlen, "{\"path\":\"%s\",\"size\":%lld,\"format\":\"png\"}",
                 output_path, (long long)st.st_size);
    } else {
        snprintf(result, rlen, "{\"error\":\"screenshot failed\"}");
        free(output_path);
        return false;
    }

    free(output_path);
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
            const tui_glyphs_t *gl = tui_glyph();
            const char *icon = gl->circle_open;
            const char *color = TUI_DIM;
            if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING) {
                icon = gl->circle_dot; color = TUI_BCYAN;
            } else if (c->status == SWARM_DONE) {
                icon = gl->ok; color = TUI_GREEN;
            } else if (c->status == SWARM_ERROR) {
                icon = gl->fail; color = TUI_RED;
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
    timeout = clamp_timeout_seconds(timeout, 120, 1, 3600);
    if (id < 0 && g_swarm.child_count == 0) {
        snprintf(result, rlen, "{\"error\":\"no agents to wait for\"}");
        return false;
    }

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

typedef struct {
    char *task;
    char *model;
} swarm_task_spec_t;

typedef struct {
    swarm_task_spec_t specs[SWARM_MAX_CHILDREN];
    int count;
    bool parse_error;
} swarm_task_parse_ctx_t;

static void parse_swarm_task_element(const char *element_start, void *ctx) {
    swarm_task_parse_ctx_t *pctx = (swarm_task_parse_ctx_t *)ctx;
    if (!pctx || !element_start || pctx->parse_error || pctx->count >= SWARM_MAX_CHILDREN)
        return;

    while (*element_start && isspace((unsigned char)*element_start)) element_start++;

    swarm_task_spec_t *spec = &pctx->specs[pctx->count];
    memset(spec, 0, sizeof(*spec));

    if (*element_start == '"') {
        const char *p = element_start + 1;
        jbuf_t task_buf;
        jbuf_init(&task_buf, 128);
        bool closed = false;
        while (*p) {
            if (*p == '\\' && p[1]) {
                p++;
                jbuf_append_char(&task_buf, *p++);
                continue;
            }
            if (*p == '"') {
                closed = true;
                break;
            }
            jbuf_append_char(&task_buf, *p++);
        }
        if (!closed) {
            jbuf_free(&task_buf);
            pctx->parse_error = true;
            return;
        }
        spec->task = safe_strdup(task_buf.data ? task_buf.data : "");
        jbuf_free(&task_buf);
    } else if (*element_start == '{') {
        spec->task = json_get_str(element_start, "task");
        spec->model = json_get_str(element_start, "model");
    } else {
        pctx->parse_error = true;
        return;
    }

    if (!spec->task || !spec->task[0]) {
        free(spec->task);
        free(spec->model);
        spec->task = NULL;
        spec->model = NULL;
        pctx->parse_error = true;
        return;
    }

    pctx->count++;
}

static const char *topology_strategy_label(exec_strategy_t strategy) {
    switch (strategy) {
    case EXEC_LINEAR: return "linear";
    case EXEC_PARALLEL_STAGES: return "parallel_stages";
    case EXEC_FULL_PARALLEL: return "full_parallel";
    case EXEC_ITERATIVE: return "iterative";
    case EXEC_TOURNAMENT: return "tournament";
    case EXEC_CONSENSUS: return "consensus";
    }
    return "unknown";
}

static const char *topology_task_kind_label(topology_task_kind_t kind) {
    switch (kind) {
    case TOPO_TASK_GENERAL: return "general";
    case TOPO_TASK_CODE: return "code";
    case TOPO_TASK_RESEARCH: return "research";
    case TOPO_TASK_REVIEW: return "review";
    case TOPO_TASK_CREATIVE: return "creative";
    case TOPO_TASK_INCIDENT: return "incident";
    }
    return "unknown";
}

static bool topology_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return true;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

typedef enum {
    TOPOLOGY_SORT_NONE = 0,
    TOPOLOGY_SORT_ID,
    TOPOLOGY_SORT_NAME,
    TOPOLOGY_SORT_CATEGORY,
    TOPOLOGY_SORT_STRATEGY,
    TOPOLOGY_SORT_NODE_COUNT,
    TOPOLOGY_SORT_EDGE_COUNT,
    TOPOLOGY_SORT_TOTAL_AGENTS,
    TOPOLOGY_SORT_MAX_ITERATIONS,
    TOPOLOGY_SORT_LATENCY_MULT,
    TOPOLOGY_SORT_EST_COST,
} topology_sort_by_t;

static bool topology_sort_by_from_string(const char *s, topology_sort_by_t *out) {
    if (!s || !out || !s[0]) return false;
    if (!strcasecmp(s, "id") || !strcasecmp(s, "topology_id")) {
        *out = TOPOLOGY_SORT_ID;
    } else if (!strcasecmp(s, "name")) {
        *out = TOPOLOGY_SORT_NAME;
    } else if (!strcasecmp(s, "category")) {
        *out = TOPOLOGY_SORT_CATEGORY;
    } else if (!strcasecmp(s, "strategy")) {
        *out = TOPOLOGY_SORT_STRATEGY;
    } else if (!strcasecmp(s, "node_count") || !strcasecmp(s, "nodes")) {
        *out = TOPOLOGY_SORT_NODE_COUNT;
    } else if (!strcasecmp(s, "edge_count") || !strcasecmp(s, "edges")) {
        *out = TOPOLOGY_SORT_EDGE_COUNT;
    } else if (!strcasecmp(s, "total_agents") || !strcasecmp(s, "agents")) {
        *out = TOPOLOGY_SORT_TOTAL_AGENTS;
    } else if (!strcasecmp(s, "max_iterations") || !strcasecmp(s, "iterations")) {
        *out = TOPOLOGY_SORT_MAX_ITERATIONS;
    } else if (!strcasecmp(s, "latency_mult") || !strcasecmp(s, "latency")) {
        *out = TOPOLOGY_SORT_LATENCY_MULT;
    } else if (!strcasecmp(s, "est_cost_1k") || !strcasecmp(s, "cost")) {
        *out = TOPOLOGY_SORT_EST_COST;
    } else {
        return false;
    }
    return true;
}

static bool topology_order_from_string(const char *s, bool *descending) {
    if (!s || !s[0] || !descending) return false;
    if (!strcasecmp(s, "asc") || !strcasecmp(s, "ascending")) {
        *descending = false;
        return true;
    }
    if (!strcasecmp(s, "desc") || !strcasecmp(s, "descending")) {
        *descending = true;
        return true;
    }
    return false;
}

static topology_sort_by_t g_topology_sort_by = TOPOLOGY_SORT_NONE;
static bool g_topology_sort_desc = false;

static int topology_sort_cmp(const void *left, const void *right) {
    const topology_t *a = *(const topology_t * const *)left;
    const topology_t *b = *(const topology_t * const *)right;
    if (!a && !b) return 0;
    if (!a) return 1;
    if (!b) return -1;

    int cmp = 0;
    switch (g_topology_sort_by) {
    case TOPOLOGY_SORT_ID:
        cmp = a->id - b->id;
        break;
    case TOPOLOGY_SORT_NAME:
        cmp = strcasecmp(a->name, b->name);
        break;
    case TOPOLOGY_SORT_CATEGORY:
        cmp = strcmp(topo_category_label(a->category), topo_category_label(b->category));
        break;
    case TOPOLOGY_SORT_STRATEGY:
        cmp = strcmp(topology_strategy_label(a->strategy), topology_strategy_label(b->strategy));
        break;
    case TOPOLOGY_SORT_NODE_COUNT:
        cmp = a->node_count - b->node_count;
        break;
    case TOPOLOGY_SORT_EDGE_COUNT:
        cmp = a->edge_count - b->edge_count;
        break;
    case TOPOLOGY_SORT_TOTAL_AGENTS:
        cmp = a->total_agents - b->total_agents;
        break;
    case TOPOLOGY_SORT_MAX_ITERATIONS:
        cmp = a->max_iterations - b->max_iterations;
        break;
    case TOPOLOGY_SORT_LATENCY_MULT:
        if (a->est_latency_mult < b->est_latency_mult) cmp = -1;
        else if (a->est_latency_mult > b->est_latency_mult) cmp = 1;
        break;
    case TOPOLOGY_SORT_EST_COST:
        if (a->est_cost_1k < b->est_cost_1k) cmp = -1;
        else if (a->est_cost_1k > b->est_cost_1k) cmp = 1;
        break;
    case TOPOLOGY_SORT_NONE:
    default:
        break;
    }

    if (cmp == 0) cmp = a->id - b->id;
    return g_topology_sort_desc ? -cmp : cmp;
}

static bool topology_category_from_string(const char *s, topo_category_t *out) {
    if (!s || !out) return false;
    if (!strcasecmp(s, "chain")) {
        *out = CAT_CHAIN;
    } else if (!strcasecmp(s, "fanout")) {
        *out = CAT_FANOUT;
    } else if (!strcasecmp(s, "hierarchy")) {
        *out = CAT_HIERARCHY;
    } else if (!strcasecmp(s, "mesh")) {
        *out = CAT_MESH;
    } else if (!strcasecmp(s, "specialist")) {
        *out = CAT_SPECIALIST;
    } else if (!strcasecmp(s, "feedback")) {
        *out = CAT_FEEDBACK;
    } else if (!strcasecmp(s, "competitive")) {
        *out = CAT_COMPETITIVE;
    } else if (!strcasecmp(s, "domain")) {
        *out = CAT_DOMAIN;
    } else {
        return false;
    }
    return true;
}

static bool topology_strategy_from_string(const char *s, exec_strategy_t *out) {
    if (!s || !out) return false;
    if (!strcasecmp(s, "linear")) {
        *out = EXEC_LINEAR;
    } else if (!strcasecmp(s, "parallel_stages") || !strcasecmp(s, "parallel")) {
        *out = EXEC_PARALLEL_STAGES;
    } else if (!strcasecmp(s, "full_parallel")) {
        *out = EXEC_FULL_PARALLEL;
    } else if (!strcasecmp(s, "iterative")) {
        *out = EXEC_ITERATIVE;
    } else if (!strcasecmp(s, "tournament")) {
        *out = EXEC_TOURNAMENT;
    } else if (!strcasecmp(s, "consensus")) {
        *out = EXEC_CONSENSUS;
    } else {
        return false;
    }
    return true;
}

static bool topology_matches_filter(const topology_t *t,
                                   const topo_category_t *category_filter,
                                   bool use_category,
                                   const exec_strategy_t *strategy_filter,
                                   bool use_strategy,
                                   const char *query,
                                   int min_agents,
                                   int max_agents,
                                   bool runnable_only) {
    if (!t) return false;
    if (use_category && t->category != *category_filter) return false;
    if (use_strategy && t->strategy != *strategy_filter) return false;
    if (runnable_only && !topology_is_runnable(t)) return false;
    if (min_agents > 0 && t->total_agents < min_agents) return false;
    if (max_agents > 0 && t->total_agents > max_agents) return false;
    if (query && query[0]) {
        if (!topology_contains_ci(t->name, query) &&
            !topology_contains_ci(t->description, query)) return false;
    }
    return true;
}

static void append_topology_json(jbuf_t *b, const topology_t *t, bool details) {
    if (!b || !t) return;

    jbuf_append(b, "{\"id\":");
    jbuf_append_int(b, t->id);
    jbuf_append(b, ",\"name\":");
    jbuf_append_json_str(b, t->name);
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, t->description);
    jbuf_append(b, ",\"category\":");
    jbuf_append_json_str(b, topo_category_label(t->category));
    jbuf_append(b, ",\"strategy\":");
    jbuf_append_json_str(b, topology_strategy_label(t->strategy));
    jbuf_append(b, ",\"node_count\":");
    jbuf_append_int(b, t->node_count);
    jbuf_append(b, ",\"edge_count\":");
    jbuf_append_int(b, t->edge_count);
    jbuf_append(b, ",\"total_agents\":");
    jbuf_append_int(b, t->total_agents);
    jbuf_append(b, ",\"max_iterations\":");
    jbuf_append_int(b, t->max_iterations);
    {
        char num[64];
        snprintf(num, sizeof(num), "%.3f", t->est_cost_1k);
        jbuf_append(b, ",\"est_cost_1k\":");
        jbuf_append(b, num);
        snprintf(num, sizeof(num), "%.2f", t->est_latency_mult);
        jbuf_append(b, ",\"latency_mult\":");
        jbuf_append(b, num);
    }
    jbuf_append(b, ",\"runnable\":");
    jbuf_append(b, topology_is_runnable(t) ? "true" : "false");

    if (details) {
        char ascii[4096];
        ascii[0] = '\0';
        (void)topology_render_ascii(t, ascii, sizeof(ascii));

        jbuf_append(b, ",\"nodes\":[");
        for (int i = 0; i < t->node_count; i++) {
            if (i > 0) jbuf_append(b, ",");
            jbuf_append(b, "{\"id\":");
            jbuf_append_int(b, t->nodes[i].id);
            jbuf_append(b, ",\"tag\":");
            jbuf_append_json_str(b, t->nodes[i].tag);
            jbuf_append(b, ",\"role\":");
            jbuf_append_json_str(b, node_role_label(t->nodes[i].role));
            jbuf_append(b, ",\"tier\":");
            jbuf_append_json_str(b, tier_label(t->nodes[i].tier));
            jbuf_append(b, ",\"replicas\":");
            jbuf_append_int(b, t->nodes[i].replicas);
            jbuf_append(b, "}");
        }
        jbuf_append(b, "],\"ascii\":");
        jbuf_append_json_str(b, ascii);
    }

    jbuf_append(b, "}");
}

static bool tool_topology_list(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    int topology_id = json_get_int(input, "topology_id", 0);
    char *query = json_get_str(input, "query");
    char *category_name = json_get_str(input, "category");
    char *strategy_name = json_get_str(input, "strategy");
    char *sort_by_name = json_get_str(input, "sort_by");
    char *order = json_get_str(input, "order");
    bool runnable_only = json_get_bool(input, "runnable_only", false);
    int min_agents = json_get_int(input, "min_agents", 0);
    int max_agents = json_get_int(input, "max_agents", -1);
    int offset = json_get_int(input, "offset", 0);
    int limit = json_get_int(input, "limit", 0);
    bool details = json_get_bool(input, "details", name && name[0]);
    bool use_sort = false;
    topology_sort_by_t sort_by = TOPOLOGY_SORT_NONE;
    bool sort_desc = false;
    bool use_category = false;
    bool use_strategy = false;
    topo_category_t category_filter = CAT_CHAIN;
    exec_strategy_t strategy_filter = EXEC_LINEAR;
    const topology_t *selected[TOPOLOGY_COUNT];
    jbuf_t b = { NULL, 0, 0 };
    int match_count = 0;
    int returned = 0;

    if (offset < 0) offset = 0;
    if (limit < 0) limit = 0;
    if (limit > TOPOLOGY_COUNT) limit = TOPOLOGY_COUNT;
    if (max_agents < 0) max_agents = -1;
    if (min_agents < 0) min_agents = 0;
    if (max_agents >= 0 && max_agents < min_agents) {
        snprintf(result, rlen, "error: max_agents cannot be less than min_agents");
        goto failure;
    }
    if (name && name[0] && topology_id > 0) {
        snprintf(result, rlen, "error: provide either 'name' or 'topology_id', not both");
        goto failure;
    }
    if (sort_by_name && sort_by_name[0]) {
        if (!topology_sort_by_from_string(sort_by_name, &sort_by)) {
            snprintf(result, rlen, "error: unknown sort_by '%s'", sort_by_name);
            goto failure;
        }
        use_sort = true;
    }
    if (order && order[0]) {
        if (!topology_order_from_string(order, &sort_desc)) {
            snprintf(result, rlen, "error: unknown order '%s' (use asc|desc)",
                     order);
            goto failure;
        }
    } else {
        sort_desc = false;
    }

    if (category_name && category_name[0]) {
        if (!topology_category_from_string(category_name, &category_filter)) {
            snprintf(result, rlen, "error: unknown category '%s'", category_name);
            goto failure;
        }
        use_category = true;
    }
    if (strategy_name && strategy_name[0]) {
        if (!topology_strategy_from_string(strategy_name, &strategy_filter)) {
            snprintf(result, rlen, "error: unknown strategy '%s'", strategy_name);
            goto failure;
        }
        use_strategy = true;
    }

    jbuf_init(&b, 32768);

    if (name && name[0]) {
        const topology_t *t = topology_find(name);
        if (!t) {
            snprintf(result, rlen, "error: unknown topology '%s'", name);
            goto failure;
        }
        if (!topology_matches_filter(t, &category_filter, use_category,
                                    &strategy_filter, use_strategy,
                                    query, min_agents, max_agents,
                                    runnable_only)) {
            snprintf(result, rlen,
                     "error: topology '%s' does not match requested filters",
                     t->name);
            goto failure;
        }
        append_topology_json(&b, t, details);
    } else if (topology_id > 0) {
        const topology_t *t = topology_get(topology_id);
        if (!t) {
            snprintf(result, rlen, "error: unknown topology id %d", topology_id);
            goto failure;
        }
        if (!topology_matches_filter(t, &category_filter, use_category,
                                    &strategy_filter, use_strategy,
                                    query, min_agents, max_agents,
                                    runnable_only)) {
            snprintf(result, rlen,
                     "error: topology id %d does not match requested filters",
                     topology_id);
            goto failure;
        }
        append_topology_json(&b, t, details);
    } else {
        int count = 0;
        const topology_t *tops = topology_registry(&count);
        for (int i = 0; i < count && match_count < TOPOLOGY_COUNT; i++) {
            if (!topology_matches_filter(&tops[i], &category_filter, use_category,
                                        &strategy_filter, use_strategy,
                                        query, min_agents, max_agents,
                                        runnable_only)) {
                continue;
            }
            selected[match_count++] = &tops[i];
        }

        if (use_sort) {
            g_topology_sort_by = sort_by;
            g_topology_sort_desc = sort_desc;
            qsort(selected, (size_t)match_count, sizeof(*selected),
                  topology_sort_cmp);
        }

        int start = offset > match_count ? match_count : offset;
        int end = match_count;
        if (limit > 0 && limit + start < end) end = start + limit;
        returned = end - start;

        jbuf_append(&b, "{\"offset\":");
        jbuf_append_int(&b, start);
        jbuf_append(&b, ",\"limit\":");
        jbuf_append_int(&b, limit);
        jbuf_append(&b, ",\"count\":");
        jbuf_append_int(&b, match_count);
        jbuf_append(&b, ",\"returned\":");
        jbuf_append_int(&b, returned);
        jbuf_append(&b, ",\"has_more\":");
        jbuf_append(&b, end < match_count ? "true" : "false");
        jbuf_append(&b, ",\"topologies\":[");
        for (int i = start; i < end; i++) {
            if (i > start) jbuf_append(&b, ",");
            append_topology_json(&b, selected[i], details);
        }
        jbuf_append(&b, "]}");
    }

    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    free(name);
    free(query);
    free(category_name);
    free(strategy_name);
    free(sort_by_name);
    free(order);
    jbuf_free(&b);
    return true;

failure:
    free(name);
    free(query);
    free(category_name);
    free(strategy_name);
    free(sort_by_name);
    free(order);
    jbuf_free(&b);
    return false;
}

static bool tool_topology_run(const char *input, char *result, size_t rlen) {
    char *task = json_get_str(input, "task");
    int topology_id = json_get_int(input, "topology_id", 0);
    char *topology = json_get_str(input, "topology");
    bool dry_run = json_get_bool(input, "dry_run", false);
    bool auto_mode = json_get_bool(input, "auto",
                                   !(topology && topology[0]) && topology_id <= 0);
    bool explicit_name = topology && topology[0];
    bool explicit_id = topology_id > 0;

    if (!task || !task[0]) {
        snprintf(result, rlen, "error: task required");
        free(task);
        free(topology);
        return false;
    }
    if (explicit_name && explicit_id) {
        snprintf(result, rlen, "error: provide either topology name or topology_id, not both");
        free(task);
        free(topology);
        return false;
    }

    topology_plan_t plan;
    const topology_t *explicit_topology = NULL;
    const char *preferred = explicit_name ? topology : NULL;

    if (explicit_id) {
        explicit_topology = topology_get(topology_id);
        if (!explicit_topology) {
            snprintf(result, rlen, "error: unknown topology id %d", topology_id);
            free(task);
            free(topology);
            return false;
        }
    } else if (explicit_name) {
        explicit_topology = topology_find(topology);
    }

    if (explicit_topology) {
        topology_profile_task(task, &plan.profile);
        plan.topology = *explicit_topology;
        plan.is_dynamic = false;
        plan.rationale[0] = '\0';
        snprintf(plan.rationale, sizeof(plan.rationale), "explicit topology: %s",
                 explicit_topology->name);
    } else if (!topology_plan_build(preferred, auto_mode, task, &plan)) {
        snprintf(result, rlen, "error: unable to build topology plan");
        free(task);
        free(topology);
        return false;
    }

    jbuf_t b;
    jbuf_init(&b, MAX_TOOL_RESULT + 4096);
    jbuf_append(&b, "{\"ok\":");

    if (dry_run) {
        jbuf_append(&b, "true");
        jbuf_append(&b, ",\"dry_run\":true,\"topology\":");
        append_topology_json(&b, &plan.topology, true);
        jbuf_append(&b, ",\"dynamic\":");
        jbuf_append(&b, plan.is_dynamic ? "true" : "false");
        jbuf_append(&b, ",\"rationale\":");
        jbuf_append_json_str(&b, plan.rationale);
        jbuf_append(&b, ",\"profile\":{\"kind\":");
        jbuf_append_json_str(&b, topology_task_kind_label(plan.profile.kind));
        jbuf_append(&b, ",\"complexity\":");
        jbuf_append_int(&b, plan.profile.complexity);
        jbuf_append(&b, ",\"desired_parallelism\":");
        jbuf_append_int(&b, plan.profile.desired_parallelism);
        jbuf_append(&b, ",\"needs_iteration\":");
        jbuf_append(&b, plan.profile.needs_iteration ? "true" : "false");
        jbuf_append(&b, ",\"needs_validation\":");
        jbuf_append(&b, plan.profile.needs_validation ? "true" : "false");
        jbuf_append(&b, ",\"prefers_breadth\":");
        jbuf_append(&b, plan.profile.prefers_breadth ? "true" : "false");
        jbuf_append(&b, "}}");
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        free(task);
        free(topology);
        jbuf_free(&b);
        return true;
    }

    const char *api_key = tools_runtime_api_key();
    if (!api_key || !api_key[0]) {
        snprintf(result, rlen, "error: no runtime API key configured for topology execution");
        free(task);
        free(topology);
        jbuf_free(&b);
        return false;
    }

    char *topo_result = safe_malloc(MAX_TOOL_RESULT);
    topo_result[0] = '\0';
    topology_run_stats_t stats;
    bool ok = topology_plan_run(&plan, api_key, tools_runtime_model(),
                                task, topo_result, MAX_TOOL_RESULT, &stats);

    jbuf_append(&b, ok ? "true" : "false");
    jbuf_append(&b, ",\"dry_run\":false,\"topology\":");
    append_topology_json(&b, &plan.topology, true);
    jbuf_append(&b, ",\"dynamic\":");
    jbuf_append(&b, plan.is_dynamic ? "true" : "false");
    jbuf_append(&b, ",\"rationale\":");
    jbuf_append_json_str(&b, plan.rationale);
    jbuf_append(&b, ",\"stats\":{\"agents_spawned\":");
    jbuf_append_int(&b, stats.agents_spawned);
    jbuf_append(&b, ",\"iterations\":");
    jbuf_append_int(&b, stats.iterations);
    jbuf_append(&b, ",\"nodes_executed\":");
    jbuf_append_int(&b, stats.nodes_executed);
    {
        char num[64];
        snprintf(num, sizeof(num), "%.4f", stats.est_cost_usd);
        jbuf_append(&b, ",\"est_cost_usd\":");
        jbuf_append(&b, num);
    }
    jbuf_append(&b, ",\"final_node_tag\":");
    jbuf_append_json_str(&b, stats.final_node_tag);
    jbuf_append(&b, "},\"result\":");
    jbuf_append_json_str(&b, topo_result);
    jbuf_append(&b, "}");

    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    free(topo_result);
    free(task);
    free(topology);
    jbuf_free(&b);
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

    /* Parse tasks array. Supports either plain strings or
       objects like {"task":"...", "model":"sonnet"}. */
    char *tasks_raw = json_get_raw(input, "tasks");
    if (!tasks_raw || *tasks_raw != '[') {
        snprintf(result, rlen, "error: tasks array required");
        if (gid == g_swarm.group_count - 1) g_swarm.group_count--;
        else g_swarm.groups[gid].active = false;
        free(name); free(model); free(tasks_raw);
        return false;
    }

    swarm_task_parse_ctx_t parse_ctx;
    memset(&parse_ctx, 0, sizeof(parse_ctx));
    json_array_foreach(input, "tasks", parse_swarm_task_element, &parse_ctx);

    if (parse_ctx.parse_error || parse_ctx.count == 0) {
        for (int i = 0; i < parse_ctx.count; i++) {
            free(parse_ctx.specs[i].task);
            free(parse_ctx.specs[i].model);
        }
        if (gid == g_swarm.group_count - 1) g_swarm.group_count--;
        else g_swarm.groups[gid].active = false;
        free(tasks_raw); free(name); free(model);
        snprintf(result, rlen, "error: malformed or empty tasks array");
        return false;
    }

    int spawned = 0;
    for (int i = 0; i < parse_ctx.count; i++) {
        const char *task_model = (parse_ctx.specs[i].model && parse_ctx.specs[i].model[0])
            ? parse_ctx.specs[i].model : model;
        int cid = swarm_spawn_in_group(&g_swarm, gid, parse_ctx.specs[i].task, task_model);
        if (cid >= 0) spawned++;
    }

    char swarm_detail[256];
    snprintf(swarm_detail, sizeof(swarm_detail),
             "group_id=%d name=%s tasks=%d spawned=%d",
             gid, name, parse_ctx.count, spawned);
    baseline_log("swarm", "create_swarm", swarm_detail, NULL);

    /* TUI feedback */
    fprintf(stderr, "\n  %s⚡%s Swarm %s\"%s\"%s created: %d agents launched\n",
            TUI_BYELLOW, TUI_RESET, TUI_BOLD, name, TUI_RESET, spawned);
    for (int i = 0; i < parse_ctx.count; i++) {
        fprintf(stderr, "    %s◉%s %s%.60s%s\n",
                TUI_BCYAN, TUI_RESET, TUI_DIM, parse_ctx.specs[i].task, TUI_RESET);
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

    for (int i = 0; i < parse_ctx.count; i++) {
        free(parse_ctx.specs[i].task);
        free(parse_ctx.specs[i].model);
    }
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
                                   bool complete, const char *reason) {
    swarm_group_t *g = &sw->groups[gid];
    jbuf_t b;
    jbuf_init(&b, 8192);

    jbuf_append(&b, "{\"group\":");
    jbuf_append_json_str(&b, g->name);
    jbuf_append(&b, ",\"complete\":");
    jbuf_append(&b, complete ? "true" : "false");
    if (reason && reason[0]) {
        jbuf_append(&b, ",\"reason\":");
        jbuf_append_json_str(&b, reason);
    }
    jbuf_append(&b, ",\"active\":");
    jbuf_append_int(&b, swarm_group_active_count(sw, gid));
    jbuf_append(&b, ",\"done\":");
    jbuf_append_int(&b, swarm_group_done_count(sw, gid));
    jbuf_append(&b, ",\"errors\":");
    jbuf_append_int(&b, swarm_group_error_count(sw, gid));
    jbuf_append(&b, ",\"killed\":");
    jbuf_append_int(&b, swarm_group_killed_count(sw, gid));
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
        jbuf_append(&b, ",\"model\":");
        jbuf_append_json_str(&b, c->model);
        jbuf_append(&b, ",\"depth\":");
        jbuf_append_int(&b, c->depth);
        jbuf_append(&b, ",\"exit_code\":");
        jbuf_append_int(&b, c->exit_code);
        double elapsed = (c->end_time > 0 ? c->end_time : now_sec_helper()) - c->start_time;
        char elapsed_str[32];
        snprintf(elapsed_str, sizeof(elapsed_str), "%.1f", elapsed);
        jbuf_append(&b, ",\"elapsed_sec\":");
        jbuf_append(&b, elapsed_str);
        jbuf_append(&b, ",\"est_input_tokens\":");
        jbuf_append_int(&b, c->est_input_tokens);
        jbuf_append(&b, ",\"est_output_tokens\":");
        jbuf_append_int(&b, c->est_output_tokens);
        {
            char cost_str[32];
            snprintf(cost_str, sizeof(cost_str), "%.6f", c->est_cost_usd);
            jbuf_append(&b, ",\"est_cost_usd\":");
            jbuf_append(&b, cost_str);
        }
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

/* ── Live swarm stream callback: prints agent output in real-time ──── */

typedef struct {
    int         group_id;
    swarm_t    *swarm;
} swarm_live_ctx_t;

static void swarm_live_print_line(int child_id, const char *line, size_t line_len) {
    if (!line || line_len == 0) return;

    int display_len = (int)(line_len > 120 ? 120 : line_len);
    fprintf(stderr, "  %s│%s %s[agent %d]%s %.*s%s\n",
            TUI_DIM, TUI_RESET,
            TUI_CYAN, child_id, TUI_RESET,
            display_len, line,
            line_len > 120 ? "..." : "");
}

static void swarm_live_stream_cb(int child_id, const char *data, size_t len, void *ctx) {
    swarm_live_ctx_t *lc = (swarm_live_ctx_t *)ctx;
    if (!lc || !data || len == 0) return;

    swarm_child_t *c = swarm_get(lc->swarm, child_id);
    if (!c || c->group_id != lc->group_id) return;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\r') continue;

        if (ch == '\n') {
            swarm_live_print_line(child_id, c->stream_buf, c->stream_buf_len);
            c->stream_buf_len = 0;
            c->stream_buf[0] = '\0';
            continue;
        }

        if (c->stream_buf_len >= 4095) {
            swarm_live_print_line(child_id, c->stream_buf, c->stream_buf_len);
            c->stream_buf_len = 0;
        }

        c->stream_buf[c->stream_buf_len++] = (char)ch;
        c->stream_buf[c->stream_buf_len] = '\0';
    }
}

static bool tool_swarm_collect(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    int gid = json_get_int(input, "group_id", -1);
    int timeout = json_get_int(input, "timeout", 300);
    timeout = clamp_timeout_seconds(timeout, 300, 1, 3600);

    if (gid < 0 || gid >= g_swarm.group_count) {
        snprintf(result, rlen, "{\"error\":\"invalid group_id\"}");
        return false;
    }

    swarm_group_t *grp = &g_swarm.groups[gid];
    double start = now_sec_helper();
    int last_done = -1;

    /* Set up live streaming context */
    swarm_live_ctx_t live_ctx;
    memset(&live_ctx, 0, sizeof(live_ctx));
    live_ctx.group_id = gid;
    live_ctx.swarm = &g_swarm;

    fprintf(stderr, "\n  %s┌─ swarm \"%s\" ─ %d agents ─ streaming live%s\n",
            TUI_BYELLOW, grp->name, grp->child_count, TUI_RESET);

    /* Poll with short timeout for responsive streaming + Ctrl+C support */
    while (!swarm_group_complete(&g_swarm, gid)) {
        /* Use streaming poll with our live callback — 100ms for responsiveness */
        swarm_poll_stream(&g_swarm, 100, swarm_live_stream_cb, &live_ctx);

        /* Ctrl+C: abort swarm gracefully */
        if (g_interrupted) {
            const char *reason = g_tool_timed_out ? "tool_timeout" : "interrupted";
            fprintf(stderr, "  %s⚠ interrupted — leaving swarm \"%s\" running%s\n",
                    TUI_BRED, grp->name, TUI_RESET);
            swarm_poll_stream(&g_swarm, 100, swarm_live_stream_cb, &live_ctx);
            swarm_collect_results(&g_swarm, gid, result, rlen, false, reason);
            fprintf(stderr, "  %s└─ swarm detached%s\n\n", TUI_BRED, TUI_RESET);
            return false;
        }

        /* Show status line when agent count changes */
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
            if (active > 0) {
                fprintf(stderr, "  %s├─ %d/%d done, %d active (%.0fs)%s\n",
                        TUI_BYELLOW, done_count, grp->child_count, active,
                        now_sec_helper() - start, TUI_RESET);
            }
        }

        double elapsed = now_sec_helper() - start;
        if (elapsed >= timeout) {
            fprintf(stderr, "  %s⚠ swarm \"%s\" timed out after %.0fs — leaving it running%s\n",
                    TUI_BYELLOW, grp->name, elapsed, TUI_RESET);
            swarm_poll_stream(&g_swarm, 100, swarm_live_stream_cb, &live_ctx);
            swarm_collect_results(&g_swarm, gid, result, rlen, false, "timeout");
            fprintf(stderr, "  %s└─ partial results returned%s\n\n", TUI_BYELLOW, TUI_RESET);
            return true;
        }
    }

    swarm_collect_results(&g_swarm, gid, result, rlen, true, NULL);

    /* Final status */
    double elapsed = now_sec_helper() - start;
    int errors = 0;
    for (int i = 0; i < grp->child_count; i++) {
        if (g_swarm.children[grp->child_ids[i]].status == SWARM_ERROR) errors++;
    }
    if (errors > 0) {
        fprintf(stderr, "  %s└─ %d/%d completed, %d errors (%.1fs)%s\n\n",
                TUI_BYELLOW, grp->child_count - errors, grp->child_count,
                errors, elapsed, TUI_RESET);
    } else {
        fprintf(stderr, "  %s└─ all %d agents complete (%.1fs)%s\n\n",
                TUI_GREEN, grp->child_count, elapsed, TUI_RESET);
    }

    tui_agent_rollup(grp->child_count, grp->child_count - errors, 0, errors);
    if (g_tui_features && g_tui_features->swarm_cost) {
        tui_swarm_cost_entry_t entries[SWARM_MAX_CHILDREN];
        int ec = 0;
        double total_cost = 0.0;
        for (int i = 0; i < grp->child_count && ec < SWARM_MAX_CHILDREN; i++) {
            swarm_child_t *c = &g_swarm.children[grp->child_ids[i]];
            entries[ec].name = c->model[0] ? c->model : c->task;
            entries[ec].cost = c->est_cost_usd;
            entries[ec].in_tok = c->est_input_tokens;
            entries[ec].out_tok = c->est_output_tokens;
            total_cost += c->est_cost_usd;
            ec++;
        }
        tui_swarm_cost(entries, ec, total_cost);
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
    if (ok) {
        tui_ipc_message_line(ipc_self_id(), to ? to : "*",
                             topic ? topic : "general", body);
    }
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
        tui_ipc_message_line(msgs[i].from_agent, ipc_self_id(),
                             msgs[i].topic, msgs[i].body);
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
        if (!require_regular_file(file, result, rlen)) {
            free(text); free(file);
            return false;
        }
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
        if (!require_regular_file(file, result, rlen)) {
            free(text); free(file);
            return false;
        }
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
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k = json_get_int(input, "top_k", CTX_SEARCH_DEFAULT_K);
    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        free(query);
        free(tool_filter);
        free(facet);
        return false;
    }

    bool ok = true;
    if (source_id <= 0 && (!facet || !*facet)) {
        ok = ctx_search_render(query, tool_filter, top_k, result, rlen);
    } else {
        ctx_hit_t hits[CTX_SEARCH_MAX_K];
        char mode[64];
        int emit = ctx_rank_hits_ladder(query, tool_filter, source_id, facet,
                                        top_k, hits, CTX_SEARCH_MAX_K,
                                        mode, sizeof(mode));
        if (emit <= 0) {
            snprintf(result, rlen,
                     "no retrieval hits for query=%s with source_id=%d facet=%s",
                     query, source_id, (facet && *facet) ? facet : "*");
        } else {
            size_t off = 0;
            int n = snprintf(result + off, rlen - off,
                             "context search query=%s\n"
                             "reranked=%d tool_filter=%s source_id=%d facet=%s mode=%s\n\n",
                             query, emit,
                             (tool_filter && *tool_filter) ? tool_filter : "*",
                             source_id, (facet && *facet) ? facet : "*",
                             mode[0] ? mode : "strict");
            if (n < 0) n = 0;
            if ((size_t)n < rlen - off) off += (size_t)n;

            for (int i = 0; i < emit && off + 64 < rlen; i++) {
                ctx_chunk_t *c = &g_ctx.chunks[hits[i].idx];
                char preview[300];
                ctx_preview(c->text, 220, preview, sizeof(preview));
                n = snprintf(result + off, rlen - off,
                             "[chunk_id=%d score=%.3f dense=%.3f lexical=%.3f tool=%s bytes=%zu]\n"
                             "%s\n\n",
                             c->id, hits[i].final_score, hits[i].dense_score, hits[i].lexical_score,
                             c->tool, c->text_len, preview[0] ? preview : "(no preview)");
                if (n < 0 || (size_t)n >= rlen - off) break;
                off += (size_t)n;
            }
            snprintf(result + off, rlen - off,
                     "next: context_get {\"chunk_id\":<id>,\"max_chars\":4000}");
        }
    }
    free(query);
    free(tool_filter);
    free(facet);
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
        int oldest = g_ctx.count > 0 ? g_ctx.chunks[0].id : -1;
        int newest = g_ctx.count > 0 ? g_ctx.chunks[g_ctx.count - 1].id : -1;
        snprintf(result, rlen,
                 "error: chunk_id %d not found (likely evicted)\n"
                 "active_chunk_range=%d-%d total_chunks=%d\n"
                 "next: rerun context_search with a focused query, or pin important chunks sooner",
                 chunk_id, oldest, newest, g_ctx.count);
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
    int source_ids[128];
    int source_count = 0;

    tool_stat_t stats[32];
    int scount = 0;
    int pinned_count = 0;
    for (int i = 0; i < g_ctx.count; i++) {
        if (g_ctx.chunks[i].pinned || g_ctx.chunks[i].turn_pinned) pinned_count++;
        const char *sid = strstr(g_ctx.chunks[i].text, "snapshot_id=");
        if (sid) {
            int id = atoi(sid + 12);
            if (id > 0) {
                bool seen = false;
                for (int s = 0; s < source_count; s++) {
                    if (source_ids[s] == id) {
                        seen = true;
                        break;
                    }
                }
                if (!seen && source_count < (int)(sizeof(source_ids) / sizeof(source_ids[0]))) {
                    source_ids[source_count++] = id;
                }
            }
        }
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
                     "context chunks=%d bytes=%zu max_chunks=%d offload_threshold=%d pinned=%d sources=%d\n"
                     "offload_events=%zu offloaded_bytes=%zu reference_bytes=%zu estimated_tokens_saved=%zu\n",
                     g_ctx.count, g_ctx.total_bytes, CTX_MAX_CHUNKS,
                     ctx_offload_threshold_bytes(), pinned_count, source_count,
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
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k = json_get_int(input, "top_k", 4);
    int max_chars = json_get_int(input, "max_chars_per_chunk", 260);

    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        free(query);
        free(tool_filter);
        free(facet);
        return false;
    }
    if (max_chars < 80) max_chars = 80;
    if (max_chars > 1200) max_chars = 1200;

    ctx_hit_t hits[CTX_SEARCH_MAX_K];
    char mode[64];
    int n_hits = ctx_rank_hits_ladder(query, tool_filter, source_id, facet,
                                      top_k, hits, CTX_SEARCH_MAX_K,
                                      mode, sizeof(mode));
    if (n_hits <= 0) {
        snprintf(result, rlen, "no hits for summary query: %s", query);
        free(query);
        free(tool_filter);
        free(facet);
        return true;
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "context summary query=%s tool_filter=%s source_id=%d facet=%s hits=%d mode=%s\n",
                     query,
                     (tool_filter && *tool_filter) ? tool_filter : "*",
                     source_id,
                     (facet && *facet) ? facet : "*",
                     n_hits,
                     mode[0] ? mode : "strict");
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        free(query);
        free(tool_filter);
        free(facet);
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
    free(facet);
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
            if (g_ctx.chunks[i].pinned || g_ctx.chunks[i].turn_pinned) continue;
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

static bool tool_context_pack(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *tool_filter = json_get_str(input, "tool");
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k = json_get_int(input, "top_k", 8);
    int max_total = json_get_int(input, "max_chars_total", 2200);
    int max_per = json_get_int(input, "max_chars_per_chunk", 420);

    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        free(query);
        free(tool_filter);
        free(facet);
        return false;
    }
    if (top_k < 1) top_k = 1;
    if (top_k > CTX_SEARCH_MAX_K) top_k = CTX_SEARCH_MAX_K;
    if (max_total < 400) max_total = 400;
    if (max_total > 24000) max_total = 24000;
    if (max_per < 100) max_per = 100;
    if (max_per > 4000) max_per = 4000;

    ctx_hit_t hits[CTX_SEARCH_MAX_K];
    char mode[64];
    int n_hits = ctx_rank_hits_ladder(query, tool_filter, source_id, facet,
                                      top_k, hits, CTX_SEARCH_MAX_K,
                                      mode, sizeof(mode));
    if (n_hits <= 0) {
        snprintf(result, rlen, "no packable hits for query: %s", query);
        free(query);
        free(tool_filter);
        free(facet);
        return true;
    }

    size_t off = 0;
    size_t used = 0;
    int packed = 0;
    int n = snprintf(result + off, rlen - off,
                     "context_pack query=%s tool=%s source_id=%d facet=%s hits=%d budget_chars=%d mode=%s\n",
                     query,
                     (tool_filter && *tool_filter) ? tool_filter : "*",
                     source_id,
                     (facet && *facet) ? facet : "*",
                     n_hits,
                     max_total,
                     mode[0] ? mode : "strict");
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        free(query);
        free(tool_filter);
        free(facet);
        return true;
    }
    off += (size_t)n;

    for (int i = 0; i < n_hits; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[hits[i].idx];
        if ((int)used >= max_total) break;

        int remain = max_total - (int)used;
        int take = max_per < remain ? max_per : remain;
        if (take <= 0) break;

        char preview[4200];
        size_t pcap = sizeof(preview);
        if ((size_t)take + 8 < pcap) pcap = (size_t)take + 8;
        ctx_preview(c->text, (size_t)take, preview, pcap);
        size_t plen = strlen(preview);
        used += plen;
        packed++;

        n = snprintf(result + off, rlen - off,
                     "\n[citation chunk=%d tool=%s score=%.3f]\n%s\n",
                     c->id, c->tool, hits[i].final_score, preview);
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }

    snprintf(result + off, rlen - off,
             "\npack_summary packed_chunks=%d packed_chars=%zu budget_chars=%d\n"
             "next: context_get on cited chunks for verbatim spans",
             packed, used, max_total);
    free(query);
    free(tool_filter);
    free(facet);
    return true;
}

static bool tool_context_fuse(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *tool_filter = json_get_str(input, "tool");
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k_each = json_get_int(input, "top_k_each", 4);
    int final_k = json_get_int(input, "final_k", 8);

    ctx_query_list_t qlist;
    memset(&qlist, 0, sizeof(qlist));
    json_array_foreach(input, "queries", ctx_collect_query_cb, &qlist);
    if (qlist.count == 0 && query && *query) {
        strncpy(qlist.items[0], query, sizeof(qlist.items[0]) - 1);
        qlist.count = 1;
    }
    if (qlist.count == 0) {
        snprintf(result, rlen, "error: query or queries[] required");
        free(query);
        free(tool_filter);
        free(facet);
        return false;
    }
    if (top_k_each < 1) top_k_each = 1;
    if (top_k_each > CTX_SEARCH_MAX_K) top_k_each = CTX_SEARCH_MAX_K;
    if (final_k < 1) final_k = 1;
    if (final_k > CTX_SEARCH_MAX_K) final_k = CTX_SEARCH_MAX_K;

    float fused[CTX_MAX_CHUNKS];
    float best_final[CTX_MAX_CHUNKS];
    int seen_count[CTX_MAX_CHUNKS];
    int relaxed_queries = 0;
    memset(fused, 0, sizeof(fused));
    memset(best_final, 0, sizeof(best_final));
    memset(seen_count, 0, sizeof(seen_count));

    const float rrf_k = 50.0f;
    for (int qi = 0; qi < qlist.count; qi++) {
        ctx_hit_t hits[CTX_SEARCH_MAX_K];
        char mode[64];
        int n_hits = ctx_rank_hits_ladder(qlist.items[qi], tool_filter, source_id, facet,
                                          top_k_each, hits, CTX_SEARCH_MAX_K,
                                          mode, sizeof(mode));
        if (mode[0] && strncmp(mode, "strict", 6) != 0) relaxed_queries++;
        for (int i = 0; i < n_hits; i++) {
            int idx = hits[i].idx;
            fused[idx] += 1.0f / (rrf_k + (float)(i + 1));
            if (hits[i].final_score > best_final[idx]) best_final[idx] = hits[i].final_score;
            seen_count[idx]++;
        }
    }

    ctx_fused_hit_t ranked[CTX_MAX_CHUNKS];
    int rcount = 0;
    for (int i = 0; i < g_ctx.count && rcount < CTX_MAX_CHUNKS; i++) {
        if (seen_count[i] <= 0) continue;
        ranked[rcount].idx = i;
        ranked[rcount].fused = fused[i];
        ranked[rcount].best_final = best_final[i];
        ranked[rcount].hit_count = seen_count[i];
        rcount++;
    }
    if (rcount == 0) {
        snprintf(result, rlen, "no fused hits");
        free(query);
        free(tool_filter);
        free(facet);
        return true;
    }
    qsort(ranked, (size_t)rcount, sizeof(ranked[0]), ctx_cmp_fused_desc);
    if (final_k > rcount) final_k = rcount;

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "context_fuse queries=%d tool=%s source_id=%d facet=%s top_k_each=%d final_k=%d relaxed_queries=%d\n",
                     qlist.count,
                     (tool_filter && *tool_filter) ? tool_filter : "*",
                     source_id,
                     (facet && *facet) ? facet : "*",
                     top_k_each,
                     final_k,
                     relaxed_queries);
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        free(query);
        free(tool_filter);
        free(facet);
        return true;
    }
    off += (size_t)n;

    for (int i = 0; i < final_k; i++) {
        ctx_chunk_t *c = &g_ctx.chunks[ranked[i].idx];
        char preview[300];
        ctx_preview(c->text, 200, preview, sizeof(preview));
        n = snprintf(result + off, rlen - off,
                     "\n[fused_rank=%d chunk_id=%d fused=%.4f hit_count=%d best=%.3f tool=%s]\n%s\n",
                     i + 1, c->id, ranked[i].fused, ranked[i].hit_count,
                     ranked[i].best_final, c->tool, preview);
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
    }
    snprintf(result + off, rlen - off,
             "\nnext: context_pack or context_get on chunk ids above");
    free(query);
    free(tool_filter);
    free(facet);
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

#define BROWSER_MAX_SNAPSHOTS 64
#define BROWSER_MAX_URL_LEN   768
#define BROWSER_HOST_MEMORY_MAX 128
#define BROWSER_FETCH_STRATEGY_COUNT 5
#define BROWSER_HOST_DB_PATH_MAX 1024

typedef struct {
    int id;
    bool active;
    time_t created_at;
    char url[BROWSER_MAX_URL_LEN];
    char title[256];
    char *visual;
    size_t visual_len;
    char *outline;
    size_t outline_len;
} browser_snapshot_t;

static browser_snapshot_t g_browser_snaps[BROWSER_MAX_SNAPSHOTS];
static int g_browser_next_id = 1;

typedef struct {
    bool used;
    char host[192];
    int attempts[BROWSER_FETCH_STRATEGY_COUNT];
    int successes[BROWSER_FETCH_STRATEGY_COUNT];
    int preferred_strategy;
    int consecutive_failures;
    time_t last_success_at;
} browser_host_profile_t;

static __attribute__((unused)) browser_host_profile_t g_browser_hosts[BROWSER_HOST_MEMORY_MAX];
static bool g_browser_hosts_loaded = false;
static bool g_browser_hosts_loading = false;
static bool g_browser_hosts_dirty = false;
static bool g_browser_hosts_atexit_registered = false;
static time_t g_browser_hosts_last_save_at = 0;
static char g_browser_hosts_path[BROWSER_HOST_DB_PATH_MAX];


static bool ascii_ieq_prefix(const char *s, const char *lit) {
    if (!s || !lit) return false;
    while (*lit) {
        if (!*s) return false;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*lit)) return false;
        s++;
        lit++;
    }
    return true;
}

static const char *ascii_icontains(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return NULL;
    for (const char *p = hay; *p; p++) {
        if (ascii_ieq_prefix(p, needle)) return p;
    }
    return NULL;
}

static bool html_decode_entity_char(const char *p, char *decoded, int *consumed) {
    if (!p || *p != '&' || !decoded || !consumed) return false;
    *decoded = '\0';
    *consumed = 0;
    if (strncmp(p, "&amp;", 5) == 0)  { *decoded = '&'; *consumed = 5; return true; }
    if (strncmp(p, "&lt;", 4) == 0)   { *decoded = '<'; *consumed = 4; return true; }
    if (strncmp(p, "&gt;", 4) == 0)   { *decoded = '>'; *consumed = 4; return true; }
    if (strncmp(p, "&quot;", 6) == 0) { *decoded = '"'; *consumed = 6; return true; }
    if (strncmp(p, "&apos;", 6) == 0) { *decoded = '\''; *consumed = 6; return true; }
    if (strncmp(p, "&nbsp;", 6) == 0) { *decoded = ' '; *consumed = 6; return true; }
    return false;
}

static bool html_extract_attr_ci(const char *tag_start,
                                 const char *tag_end,
                                 const char *attr,
                                 char *out,
                                 size_t out_len) {
    if (!tag_start || !tag_end || !attr || !*attr || !out || out_len == 0) return false;
    out[0] = '\0';
    const char *p = tag_start;
    while (p < tag_end) {
        while (p < tag_end && isspace((unsigned char)*p)) p++;
        if (p >= tag_end || *p == '/' || *p == '>') break;
        const char *k0 = p;
        while (p < tag_end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-')) p++;
        size_t klen = (size_t)(p - k0);
        while (p < tag_end && isspace((unsigned char)*p)) p++;
        if (p >= tag_end || *p != '=') {
            while (p < tag_end && !isspace((unsigned char)*p) && *p != '>') p++;
            continue;
        }
        p++;
        while (p < tag_end && isspace((unsigned char)*p)) p++;
        if (p >= tag_end) break;

        const char *v0 = p;
        const char *v1 = p;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            v0 = p;
            while (p < tag_end && *p != q) p++;
            v1 = p;
            if (p < tag_end) p++;
        } else {
            while (p < tag_end && !isspace((unsigned char)*p) && *p != '>') p++;
            v1 = p;
        }
        if (klen == strlen(attr) && strncasecmp(k0, attr, klen) == 0) {
            size_t n = (size_t)(v1 - v0);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, v0, n);
            out[n] = '\0';
            return n > 0;
        }
    }
    return false;
}

static void html_fragment_to_text(const char *src, size_t len, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!src || len == 0) return;
    size_t j = 0;
    bool prev_space = false;
    for (size_t i = 0; i < len && j + 1 < out_len; i++) {
        char c = src[i];
        if (c == '<') {
            while (i < len && src[i] != '>') i++;
            continue;
        }
        if (c == '&') {
            char dec = 0;
            int consumed = 0;
            if (html_decode_entity_char(src + i, &dec, &consumed) && consumed > 0) {
                c = dec;
                i += (size_t)consumed - 1;
            }
        }
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space && j + 1 < out_len) {
                out[j++] = ' ';
                prev_space = true;
            }
            continue;
        }
        if ((unsigned char)c < 32) continue;
        out[j++] = c;
        prev_space = false;
    }
    if (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

static void html_to_visual_snapshot(const char *html, jbuf_t *out) {
    if (!html || !out) return;
    const char *p = html;
    bool in_script = false;
    bool in_style = false;
    bool prev_space = true;
    while (*p) {
        if (*p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) break;
            bool closing = false;
            const char *t = p + 1;
            if (*t == '/') { closing = true; t++; }
            while (t < gt && isspace((unsigned char)*t)) t++;
            char tag[24];
            int tl = 0;
            while (t < gt && (isalnum((unsigned char)*t) || *t == '-') && tl < (int)sizeof(tag) - 1) {
                tag[tl++] = (char)tolower((unsigned char)*t);
                t++;
            }
            tag[tl] = '\0';

            if (!closing && strcmp(tag, "script") == 0) { in_script = true; p = gt + 1; continue; }
            if (!closing && strcmp(tag, "style") == 0)  { in_style = true; p = gt + 1; continue; }
            if (closing && strcmp(tag, "script") == 0)  { in_script = false; p = gt + 1; continue; }
            if (closing && strcmp(tag, "style") == 0)   { in_style = false; p = gt + 1; continue; }
            if (in_script || in_style) { p = gt + 1; continue; }

            bool heading = (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0');
            if (heading || strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
                strcmp(tag, "section") == 0 || strcmp(tag, "article") == 0 ||
                strcmp(tag, "br") == 0 || strcmp(tag, "tr") == 0) {
                if (out->len == 0 || out->data[out->len - 1] != '\n') jbuf_append_char(out, '\n');
                prev_space = true;
            }
            if (!closing && strcmp(tag, "li") == 0) {
                if (out->len == 0 || out->data[out->len - 1] != '\n') jbuf_append_char(out, '\n');
                jbuf_append(out, "- ");
                prev_space = false;
            }
            if (!closing && heading) {
                char hp[8];
                snprintf(hp, sizeof(hp), "[h%c] ", tag[1]);
                jbuf_append(out, hp);
                prev_space = false;
            }
            p = gt + 1;
            continue;
        }

        if (in_script || in_style) {
            p++;
            continue;
        }

        char c = *p;
        if (c == '&') {
            char dec = 0;
            int consumed = 0;
            if (html_decode_entity_char(p, &dec, &consumed) && consumed > 0) {
                c = dec;
                p += consumed;
            } else {
                p++;
            }
        } else {
            p++;
        }

        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space) {
                jbuf_append_char(out, ' ');
                prev_space = true;
            }
            continue;
        }
        if ((unsigned char)c < 32) continue;
        jbuf_append_char(out, c);
        prev_space = false;
    }
}

static void html_build_outline(const char *html, const char *url, const char *title, jbuf_t *out) {
    if (!html || !out) return;
    int links = 0;
    int headings = 0;
    const char *p = html;
    while ((p = ascii_icontains(p, "<a")) != NULL) { links++; p += 2; }
    p = html;
    while ((p = ascii_icontains(p, "<h")) != NULL) {
        if (p[2] >= '1' && p[2] <= '6') headings++;
        p += 2;
    }

    jbuf_append(out, "outline_summary\nurl: ");
    jbuf_append(out, url ? url : "(unknown)");
    jbuf_append(out, "\ntitle: ");
    jbuf_append(out, (title && *title) ? title : "(none)");
    char stats[128];
    snprintf(stats, sizeof(stats), "\nheadings=%d links=%d\n", headings, links);
    jbuf_append(out, stats);

    jbuf_append(out, "\nheadings:\n");
    int hcount = 0;
    p = html;
    while (hcount < 16 && (p = ascii_icontains(p, "<h")) != NULL) {
        if (!(p[2] >= '1' && p[2] <= '6')) { p += 2; continue; }
        char level = p[2];
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char close[8];
        snprintf(close, sizeof(close), "</h%c", level);
        const char *end = ascii_icontains(gt + 1, close);
        if (!end) { p = gt + 1; continue; }
        char text[320];
        html_fragment_to_text(gt + 1, (size_t)(end - (gt + 1)), text, sizeof(text));
        if (text[0]) {
            char line[420];
            snprintf(line, sizeof(line), "- h%c: %s\n", level, text);
            jbuf_append(out, line);
            hcount++;
        }
        p = end + 3;
    }
    if (hcount == 0) jbuf_append(out, "- (none found)\n");

    jbuf_append(out, "\nkey_links:\n");
    int lcount = 0;
    p = html;
    while (lcount < 12 && (p = ascii_icontains(p, "<a")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char href[420];
        href[0] = '\0';
        html_extract_attr_ci(p + 2, gt, "href", href, sizeof(href));
        const char *end = ascii_icontains(gt + 1, "</a");
        if (!end) { p = gt + 1; continue; }
        char text[260];
        html_fragment_to_text(gt + 1, (size_t)(end - (gt + 1)), text, sizeof(text));
        if (href[0]) {
            char line[760];
            snprintf(line, sizeof(line), "- %s -> %s\n", text[0] ? text : "(link)", href);
            jbuf_append(out, line);
            lcount++;
        }
        p = end + 3;
    }
    if (lcount == 0) jbuf_append(out, "- (none found)\n");
}

static browser_snapshot_t *browser_snapshot_find(int id) {
    for (int i = 0; i < BROWSER_MAX_SNAPSHOTS; i++) {
        if (g_browser_snaps[i].active && g_browser_snaps[i].id == id) return &g_browser_snaps[i];
    }
    return NULL;
}

static void browser_snapshot_clear(browser_snapshot_t *s) {
    if (!s) return;
    free(s->visual);
    free(s->outline);
    memset(s, 0, sizeof(*s));
}

static browser_snapshot_t *browser_snapshot_alloc(void) {
    for (int i = 0; i < BROWSER_MAX_SNAPSHOTS; i++) {
        if (!g_browser_snaps[i].active) {
            memset(&g_browser_snaps[i], 0, sizeof(g_browser_snaps[i]));
            g_browser_snaps[i].active = true;
            g_browser_snaps[i].id = g_browser_next_id++;
            g_browser_snaps[i].created_at = time(NULL);
            return &g_browser_snaps[i];
        }
    }
    int oldest = -1;
    time_t ts = 0;
    for (int i = 0; i < BROWSER_MAX_SNAPSHOTS; i++) {
        if (!g_browser_snaps[i].active) continue;
        if (oldest < 0 || g_browser_snaps[i].created_at < ts) {
            oldest = i;
            ts = g_browser_snaps[i].created_at;
        }
    }
    if (oldest < 0) return NULL;
    browser_snapshot_clear(&g_browser_snaps[oldest]);
    g_browser_snaps[oldest].active = true;
    g_browser_snaps[oldest].id = g_browser_next_id++;
    g_browser_snaps[oldest].created_at = time(NULL);
    return &g_browser_snaps[oldest];
}

static browser_snapshot_t *browser_snapshot_find_latest_by_url(const char *url) {
    if (!url || !*url) return NULL;
    browser_snapshot_t *best = NULL;
    for (int i = 0; i < BROWSER_MAX_SNAPSHOTS; i++) {
        if (!g_browser_snaps[i].active) continue;
        if (strcmp(g_browser_snaps[i].url, url) != 0) continue;
        if (!best || g_browser_snaps[i].created_at > best->created_at) {
            best = &g_browser_snaps[i];
        }
    }
    return best;
}

static browser_snapshot_t *browser_snapshot_find_latest_any(void) {
    browser_snapshot_t *best = NULL;
    for (int i = 0; i < BROWSER_MAX_SNAPSHOTS; i++) {
        if (!g_browser_snaps[i].active) continue;
        if (!best || g_browser_snaps[i].created_at > best->created_at) {
            best = &g_browser_snaps[i];
        }
    }
    return best;
}

enum {
    BFETCH_STRAT_DEFAULT = 0,
    BFETCH_STRAT_HTTP1 = 1,
    BFETCH_STRAT_BROWSER_HEADERS = 2,
    BFETCH_STRAT_INSECURE_TLS = 3,
    BFETCH_STRAT_JINA_PROXY = 4
};

static const char *browser_fetch_strategy_name(int strategy) {
    switch (strategy) {
        case BFETCH_STRAT_DEFAULT: return "default";
        case BFETCH_STRAT_HTTP1: return "http1";
        case BFETCH_STRAT_BROWSER_HEADERS: return "browser_headers";
        case BFETCH_STRAT_INSECURE_TLS: return "insecure_tls";
        case BFETCH_STRAT_JINA_PROXY: return "jina_proxy";
        default: return "unknown";
    }
}

static void browser_extract_host(const char *url, char *host, size_t host_len) {
    if (!host || host_len == 0) return;
    host[0] = '\0';
    if (!url) return;

    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && p[i] != '#' && i + 1 < host_len) {
        host[i] = (char)tolower((unsigned char)p[i]);
        i++;
    }
    host[i] = '\0';
}

static browser_host_profile_t *browser_host_profile_get(const char *host, bool create_if_missing) {
    if (!host || !*host) return NULL;
    for (int i = 0; i < BROWSER_HOST_MEMORY_MAX; i++) {
        if (g_browser_hosts[i].used && strcmp(g_browser_hosts[i].host, host) == 0) {
            return &g_browser_hosts[i];
        }
    }
    if (!create_if_missing) return NULL;

    int free_idx = -1;
    int replace_idx = -1;
    time_t oldest = 0;
    for (int i = 0; i < BROWSER_HOST_MEMORY_MAX; i++) {
        if (!g_browser_hosts[i].used) {
            free_idx = i;
            break;
        }
        if (replace_idx < 0 || g_browser_hosts[i].last_success_at < oldest) {
            replace_idx = i;
            oldest = g_browser_hosts[i].last_success_at;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : replace_idx;
    if (idx < 0) return NULL;
    memset(&g_browser_hosts[idx], 0, sizeof(g_browser_hosts[idx]));
    g_browser_hosts[idx].used = true;
    strncpy(g_browser_hosts[idx].host, host, sizeof(g_browser_hosts[idx].host) - 1);
    g_browser_hosts[idx].preferred_strategy = BFETCH_STRAT_DEFAULT;
    if (!g_browser_hosts_loading) g_browser_hosts_dirty = true;
    return &g_browser_hosts[idx];
}

static void browser_profile_record(browser_host_profile_t *profile, int strategy, bool success) {
    if (!profile || strategy < 0 || strategy >= BROWSER_FETCH_STRATEGY_COUNT) return;
    profile->attempts[strategy]++;
    if (success) {
        profile->successes[strategy]++;
        profile->consecutive_failures = 0;
        profile->last_success_at = time(NULL);

        int best = profile->preferred_strategy;
        int best_successes = (best >= 0 && best < BROWSER_FETCH_STRATEGY_COUNT) ? profile->successes[best] : -1;
        if (profile->successes[strategy] >= best_successes) {
            profile->preferred_strategy = strategy;
        }
    } else {
        profile->consecutive_failures++;
    }
    if (!g_browser_hosts_loading) g_browser_hosts_dirty = true;
}

static const char *browser_host_db_path(void) {
    if (g_browser_hosts_path[0]) return g_browser_hosts_path;
    const char *override = getenv("DSCO_BROWSER_HOST_DB");
    if (override && override[0]) {
        snprintf(g_browser_hosts_path, sizeof(g_browser_hosts_path), "%s", override);
        return g_browser_hosts_path;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(g_browser_hosts_path, sizeof(g_browser_hosts_path), "%s/.dsco/browser_fetch_hosts.tsv", home);
    } else {
        snprintf(g_browser_hosts_path, sizeof(g_browser_hosts_path), ".dsco/browser_fetch_hosts.tsv");
    }
    return g_browser_hosts_path;
}

static bool browser_mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char tmp[BROWSER_HOST_DB_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool browser_ensure_parent_dir(const char *file_path) {
    if (!file_path || !*file_path) return false;
    char tmp[BROWSER_HOST_DB_PATH_MAX];
    size_t n = strlen(file_path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, file_path, n + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    if (tmp[0] == '\0') return true;
    return browser_mkdir_p(tmp);
}

static bool browser_profiles_save(void) {
    const char *path = browser_host_db_path();
    if (!path || !*path) return false;
    if (!browser_ensure_parent_dir(path)) return false;

    char tmp_path[BROWSER_HOST_DB_PATH_MAX + 48];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;

    time_t now = time(NULL);
    fprintf(f, "# dsco browser host strategy memory\n");
    fprintf(f, "# updated_at=%ld\n", (long)now);
    fprintf(f, "# host\\tpref\\tconsecutive_failures\\tlast_success_at\\tattempt_default\\tattempt_http1\\tattempt_browser_headers\\tattempt_insecure_tls\\tattempt_jina_proxy\\tsuccess_default\\tsuccess_http1\\tsuccess_browser_headers\\tsuccess_insecure_tls\\tsuccess_jina_proxy\n");
    for (int i = 0; i < BROWSER_HOST_MEMORY_MAX; i++) {
        browser_host_profile_t *p = &g_browser_hosts[i];
        if (!p->used || !p->host[0]) continue;
        fprintf(f,
                "%s\t%d\t%d\t%lld\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
                p->host,
                p->preferred_strategy,
                p->consecutive_failures,
                (long long)p->last_success_at,
                p->attempts[BFETCH_STRAT_DEFAULT],
                p->attempts[BFETCH_STRAT_HTTP1],
                p->attempts[BFETCH_STRAT_BROWSER_HEADERS],
                p->attempts[BFETCH_STRAT_INSECURE_TLS],
                p->attempts[BFETCH_STRAT_JINA_PROXY],
                p->successes[BFETCH_STRAT_DEFAULT],
                p->successes[BFETCH_STRAT_HTTP1],
                p->successes[BFETCH_STRAT_BROWSER_HEADERS],
                p->successes[BFETCH_STRAT_INSECURE_TLS],
                p->successes[BFETCH_STRAT_JINA_PROXY]);
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    g_browser_hosts_dirty = false;
    g_browser_hosts_last_save_at = now;
    return true;
}

static int browser_int_nonneg(int v) {
    return v < 0 ? 0 : v;
}

static void browser_profiles_load(void) {
    if (g_browser_hosts_loaded) return;
    g_browser_hosts_loaded = true;

    const char *path = browser_host_db_path();
    if (!path || !*path) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    g_browser_hosts_loading = true;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char host[192];
        int pref = 0;
        int cf = 0;
        long long last = 0;
        int a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        int s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
        int got = sscanf(p,
                         "%191[^\t]\t%d\t%d\t%lld\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d",
                         host, &pref, &cf, &last,
                         &a0, &a1, &a2, &a3, &a4,
                         &s0, &s1, &s2, &s3, &s4);
        if (got != 14 || host[0] == '\0') continue;

        browser_host_profile_t *dst = browser_host_profile_get(host, true);
        if (!dst) continue;

        memset(dst, 0, sizeof(*dst));
        dst->used = true;
        strncpy(dst->host, host, sizeof(dst->host) - 1);
        if (pref >= 0 && pref < BROWSER_FETCH_STRATEGY_COUNT) {
            dst->preferred_strategy = pref;
        } else {
            dst->preferred_strategy = BFETCH_STRAT_DEFAULT;
        }
        dst->consecutive_failures = browser_int_nonneg(cf);
        dst->last_success_at = (last > 0) ? (time_t)last : (time_t)0;

        dst->attempts[BFETCH_STRAT_DEFAULT] = browser_int_nonneg(a0);
        dst->attempts[BFETCH_STRAT_HTTP1] = browser_int_nonneg(a1);
        dst->attempts[BFETCH_STRAT_BROWSER_HEADERS] = browser_int_nonneg(a2);
        dst->attempts[BFETCH_STRAT_INSECURE_TLS] = browser_int_nonneg(a3);
        dst->attempts[BFETCH_STRAT_JINA_PROXY] = browser_int_nonneg(a4);

        dst->successes[BFETCH_STRAT_DEFAULT] = browser_int_nonneg(s0);
        dst->successes[BFETCH_STRAT_HTTP1] = browser_int_nonneg(s1);
        dst->successes[BFETCH_STRAT_BROWSER_HEADERS] = browser_int_nonneg(s2);
        dst->successes[BFETCH_STRAT_INSECURE_TLS] = browser_int_nonneg(s3);
        dst->successes[BFETCH_STRAT_JINA_PROXY] = browser_int_nonneg(s4);
    }
    fclose(f);
    g_browser_hosts_loading = false;
    g_browser_hosts_dirty = false;
}

static void browser_profiles_maybe_flush(bool force) {
    if (!g_browser_hosts_loaded || !g_browser_hosts_dirty) return;
    int min_sec = 5;
    const char *env = getenv("DSCO_BROWSER_HOST_FLUSH_SEC");
    if (env && env[0]) {
        int v = atoi(env);
        if (v >= 0 && v <= 300) min_sec = v;
    }
    time_t now = time(NULL);
    if (!force && min_sec > 0 && g_browser_hosts_last_save_at > 0 &&
        (now - g_browser_hosts_last_save_at) < min_sec) {
        return;
    }
    (void)browser_profiles_save();
}

static void browser_profiles_atexit_flush(void) {
    browser_profiles_maybe_flush(true);
}

static void browser_strategy_order(browser_host_profile_t *profile, int order[BROWSER_FETCH_STRATEGY_COUNT]) {
    int base[BROWSER_FETCH_STRATEGY_COUNT] = {
        BFETCH_STRAT_DEFAULT,
        BFETCH_STRAT_HTTP1,
        BFETCH_STRAT_BROWSER_HEADERS,
        BFETCH_STRAT_INSECURE_TLS,
        BFETCH_STRAT_JINA_PROXY
    };
    int n = BROWSER_FETCH_STRATEGY_COUNT;
    for (int i = 0; i < n; i++) order[i] = base[i];

    if (!profile) return;

    /* Stable sort by observed successes, then fewer attempts. */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int si = order[i];
            int sj = order[j];
            int ssi = profile->successes[si];
            int ssj = profile->successes[sj];
            int ai = profile->attempts[si];
            int aj = profile->attempts[sj];
            bool swap = false;
            if (ssj > ssi) swap = true;
            else if (ssj == ssi && aj < ai) swap = true;
            if (swap) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    /* Force preferred strategy to front if valid. */
    int pref = profile->preferred_strategy;
    if (pref >= 0 && pref < BROWSER_FETCH_STRATEGY_COUNT) {
        int at = -1;
        for (int i = 0; i < n; i++) {
            if (order[i] == pref) { at = i; break; }
        }
        if (at > 0) {
            int t = order[0];
            order[0] = order[at];
            order[at] = t;
        }
    }
}

static bool browser_payload_has_content(const char *text) {
    if (!text || !*text) return false;
    int alpha = 0;
    for (const char *p = text; *p; p++) {
        if (isalpha((unsigned char)*p)) {
            alpha++;
            if (alpha >= 48) return true;
        }
    }
    return false;
}

static bool browser_payload_looks_blocked(const char *text) {
    if (!text || !*text) return true;
    const char *flags[] = {
        "enable javascript",
        "are you human",
        "verify you are human",
        "access denied",
        "forbidden",
        "captcha",
        "cloudflare",
        "bot detection",
        "request blocked",
        NULL
    };
    for (int i = 0; flags[i]; i++) {
        if (ascii_icontains(text, flags[i])) return true;
    }
    return false;
}

static int browser_fetch_with_strategy(const char *url,
                                       int timeout,
                                       int strategy,
                                       char *out,
                                       size_t out_len) {
    if (!url || !*url) {
        snprintf(out, out_len, "error: url required");
        return -1;
    }
    if (timeout < 1) timeout = 1;
    if (timeout > 120) timeout = 120;

    char final_url[2048];
    if (strategy == BFETCH_STRAT_JINA_PROXY) {
        snprintf(final_url, sizeof(final_url), "https://r.jina.ai/%s", url);
    } else {
        snprintf(final_url, sizeof(final_url), "%s", url);
    }

    jbuf_t cmd;
    jbuf_init(&cmd, 1536);
    jbuf_append(&cmd, "curl -sS -L --compressed --connect-timeout 10 --retry 2 --retry-delay 1 --retry-all-errors ");
    if (strategy == BFETCH_STRAT_HTTP1 || strategy == BFETCH_STRAT_INSECURE_TLS) {
        jbuf_append(&cmd, "--http1.1 ");
    }
    if (strategy == BFETCH_STRAT_INSECURE_TLS) {
        jbuf_append(&cmd, "-k --tlsv1.2 ");
    }
    if (strategy == BFETCH_STRAT_BROWSER_HEADERS) {
        jbuf_append(&cmd,
            "-H 'Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8' "
            "-H 'Accept-Language: en-US,en;q=0.9' "
            "-H 'Cache-Control: no-cache' "
            "-H 'Pragma: no-cache' "
            "--user-agent 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36' ");
    } else if (strategy == BFETCH_STRAT_JINA_PROXY) {
        jbuf_append(&cmd,
            "--user-agent 'dsco/" DSCO_VERSION " jina-proxy' ");
    } else {
        jbuf_append(&cmd,
            "--user-agent 'Mozilla/5.0 (compatible; dsco/" DSCO_VERSION ")' ");
    }
    jbuf_append(&cmd, "--max-time ");
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", timeout);
    jbuf_append(&cmd, nbuf);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, final_url);

    int status = run_cmd(cmd.data, out, out_len);
    jbuf_free(&cmd);
    return status;
}

static int browser_fetch_url_with_fallbacks(const char *url,
                                            int timeout,
                                            char *out,
                                            size_t out_len,
                                            int max_passes_override,
                                            char *meta,
                                            size_t meta_len) {
    if (meta && meta_len > 0) meta[0] = '\0';
    if (!url || !*url) {
        snprintf(out, out_len, "error: url required");
        return -1;
    }

    char host[192];
    browser_extract_host(url, host, sizeof(host));
    browser_host_profile_t *profile = browser_host_profile_get(host, true);

    int order[BROWSER_FETCH_STRATEGY_COUNT];
    browser_strategy_order(profile, order);

    int max_passes = 3;
    if (max_passes_override >= 1 && max_passes_override <= 8) {
        max_passes = max_passes_override;
    } else {
        const char *passes_env = getenv("DSCO_BROWSER_MAX_PASSES");
        if (passes_env) {
            int v = atoi(passes_env);
            if (v >= 1 && v <= 8) max_passes = v;
        }
    }
    if (max_passes > BROWSER_FETCH_STRATEGY_COUNT) max_passes = BROWSER_FETCH_STRATEGY_COUNT;

    char best_partial[MAX_TOOL_RESULT];
    best_partial[0] = '\0';
    size_t best_partial_len = 0;
    int best_partial_strategy = -1;

    int last_status = -1;
    for (int i = 0; i < max_passes; i++) {
        int strategy = order[i];
        char tmp[MAX_TOOL_RESULT];
        tmp[0] = '\0';
        int pass_timeout = timeout + (i * 8);
        if (pass_timeout > 120) pass_timeout = 120;
        int status = browser_fetch_with_strategy(url, pass_timeout, strategy, tmp, sizeof(tmp));
        last_status = status;
        if (status != 0) {
            browser_profile_record(profile, strategy, false);
            continue;
        }

        size_t tlen = strlen(tmp);
        bool has_content = browser_payload_has_content(tmp);
        bool blocked = browser_payload_looks_blocked(tmp);
        if (tlen > best_partial_len) {
            size_t n = tlen < sizeof(best_partial) - 1 ? tlen : sizeof(best_partial) - 1;
            memcpy(best_partial, tmp, n);
            best_partial[n] = '\0';
            best_partial_len = n;
            best_partial_strategy = strategy;
        }

        if (has_content && !(blocked && strategy != BFETCH_STRAT_JINA_PROXY)) {
            size_t n = tlen < out_len - 1 ? tlen : out_len - 1;
            memcpy(out, tmp, n);
            out[n] = '\0';
            browser_profile_record(profile, strategy, true);
            if (meta && meta_len > 0) {
                snprintf(meta, meta_len, "fetch_strategy=%s passes=%d",
                         browser_fetch_strategy_name(strategy), i + 1);
            }
            browser_profiles_maybe_flush(false);
            return 0;
        }

        /* soft-fail content */
        browser_profile_record(profile, strategy, false);
    }

    if (best_partial_len > 0) {
        size_t n = best_partial_len < out_len - 1 ? best_partial_len : out_len - 1;
        memcpy(out, best_partial, n);
        out[n] = '\0';
        if (meta && meta_len > 0) {
            snprintf(meta, meta_len, "fetch_strategy=%s degraded=partial_content",
                     browser_fetch_strategy_name(best_partial_strategy));
        }
        browser_profiles_maybe_flush(false);
        return 0;
    }

    if (meta && meta_len > 0) {
        snprintf(meta, meta_len, "fetch_failed passes=%d last_status=%d", max_passes, last_status);
    }
    browser_profiles_maybe_flush(false);
    return last_status;
}

static void extract_html_title(const char *html, char *title, size_t title_len) {
    if (!title || title_len == 0) return;
    title[0] = '\0';
    if (!html) return;

    const char *p = ascii_icontains(html, "<title");
    if (!p) return;
    p = strchr(p, '>');
    if (!p) return;
    p++;
    const char *q = ascii_icontains(p, "</title>");
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
    int timeout = json_get_int(input, "timeout", 30);
    int max_chars = json_get_int(input, "max_chars", 28000);
    int max_passes = json_get_int(input, "max_passes", 0);
    if (!url) {
        snprintf(result, rlen, "error: url required");
        return false;
    }
    if (max_chars < 1000) max_chars = 1000;
    if (max_chars > 120000) max_chars = 120000;

    char *html = malloc((size_t)max_chars + 1);
    if (!html) {
        free(url);
        snprintf(result, rlen, "error: out of memory");
        return false;
    }
    html[0] = '\0';
    char fetch_meta[256];
    int status = browser_fetch_url_with_fallbacks(url, timeout, html, (size_t)max_chars + 1,
                                                  max_passes, fetch_meta, sizeof(fetch_meta));
    if (status != 0) {
        browser_snapshot_t *stale = browser_snapshot_find_latest_by_url(url);
        if (stale) {
            snprintf(result, rlen,
                     "browser_snapshot network fetch failed, falling back to cached snapshot\n"
                     "url=%s\nsnapshot_id=%d\ncached_title=%s\nreason=status:%d\n"
                     "next: browser_viewport {\"snapshot_id\":%d,\"offset\":1,\"lines\":30}",
                     url,
                     stale->id,
                     stale->title[0] ? stale->title : "(none)",
                     status,
                     stale->id);
            free(url);
            free(html);
            return true;
        }
        snprintf(result, rlen, "browser_snapshot fetch failed (status %d): %s", status, html);
        free(url);
        free(html);
        return false;
    }

    char title[256];
    extract_html_title(html, title, sizeof(title));

    jbuf_t visual;
    jbuf_t outline;
    jbuf_init(&visual, 4096);
    jbuf_init(&outline, 2048);
    html_to_visual_snapshot(html, &visual);
    if (!visual.data || strlen(visual.data) < 40) {
        char *fallback_txt = malloc((size_t)max_chars + 1);
        if (fallback_txt) {
            html_to_text(html, fallback_txt, (size_t)max_chars + 1);
            if (fallback_txt[0] && strlen(fallback_txt) > strlen(visual.data ? visual.data : "")) {
                jbuf_reset(&visual);
                jbuf_append(&visual, fallback_txt);
            }
            free(fallback_txt);
        }
    }
    html_build_outline(html, url, title, &outline);

    browser_snapshot_t *snap = browser_snapshot_alloc();
    if (!snap) {
        snprintf(result, rlen, "error: snapshot capacity exhausted");
        jbuf_free(&visual);
        jbuf_free(&outline);
        free(url);
        free(html);
        return false;
    }
    strncpy(snap->url, url, sizeof(snap->url) - 1);
    strncpy(snap->title, title, sizeof(snap->title) - 1);
    snap->visual = strdup(visual.data ? visual.data : "");
    snap->outline = strdup(outline.data ? outline.data : "");
    snap->visual_len = snap->visual ? strlen(snap->visual) : 0;
    snap->outline_len = snap->outline ? strlen(snap->outline) : 0;

    size_t raw_len = strlen(html) + strlen(url) + strlen(title) + 96;
    size_t vis_len = (visual.data ? strlen(visual.data) : 0) + strlen(url) + strlen(title) + 96;
    size_t out_len = (outline.data ? strlen(outline.data) : 0) + strlen(url) + strlen(title) + 96;
    char *raw_doc = malloc(raw_len);
    char *visual_doc = malloc(vis_len);
    char *outline_doc = malloc(out_len);
    if (!raw_doc || !visual_doc || !outline_doc) {
        free(raw_doc);
        free(visual_doc);
        free(outline_doc);
        jbuf_free(&visual);
        jbuf_free(&outline);
        free(url);
        free(html);
        snprintf(result, rlen, "error: out of memory");
        return false;
    }
    snprintf(raw_doc, raw_len,
             "snapshot_id=%d\nfacet=raw\nurl=%s\ntitle=%s\n\n%s",
             snap->id, url, title[0] ? title : "(none)", html);
    snprintf(visual_doc, vis_len,
             "snapshot_id=%d\nfacet=visual\nurl=%s\ntitle=%s\n\n%s",
             snap->id, url, title[0] ? title : "(none)", visual.data ? visual.data : "");
    snprintf(outline_doc, out_len,
             "snapshot_id=%d\nfacet=outline\nurl=%s\ntitle=%s\n\n%s",
             snap->id, url, title[0] ? title : "(none)", outline.data ? outline.data : "");

    ctx_ingest_info_t info_raw;
    ctx_ingest_info_t info_visual;
    ctx_ingest_info_t info_outline;
    ctx_ingest_text("browser_snapshot", raw_doc, &info_raw);
    ctx_ingest_text("browser_snapshot", visual_doc, &info_visual);
    ctx_ingest_text("browser_snapshot", outline_doc, &info_outline);

    char preview[260];
    ctx_preview(snap->visual ? snap->visual : "", 200, preview, sizeof(preview));
    snprintf(result, rlen,
             "browser_snapshot url=%s\n"
             "snapshot_id=%d\n"
             "title=%s\n"
             "fetch_meta=%s\n"
             "indexed:\n"
             "  raw: chunks=%d range=%d-%d bytes=%zu\n"
             "  visual: chunks=%d range=%d-%d bytes=%zu\n"
             "  outline: chunks=%d range=%d-%d bytes=%zu\n"
             "visual_preview=%s\n"
             "next:\n"
             "  browser_viewport {\"snapshot_id\":%d,\"offset\":1,\"lines\":30}\n"
             "  browser_extract {\"query\":\"...\",\"source_id\":%d,\"facet\":\"visual\",\"top_k\":5}\n"
             "  context_pack {\"query\":\"...\",\"tool\":\"browser_snapshot\",\"source_id\":%d,\"facet\":\"visual\"}",
             url,
             snap->id,
             title[0] ? title : "(none)",
             fetch_meta[0] ? fetch_meta : "unknown",
             info_raw.chunks_added, info_raw.first_chunk_id, info_raw.last_chunk_id, info_raw.bytes_added,
             info_visual.chunks_added, info_visual.first_chunk_id, info_visual.last_chunk_id, info_visual.bytes_added,
             info_outline.chunks_added, info_outline.first_chunk_id, info_outline.last_chunk_id, info_outline.bytes_added,
             preview[0] ? preview : "(empty)",
             snap->id, snap->id, snap->id);

    free(raw_doc);
    free(visual_doc);
    free(outline_doc);
    jbuf_free(&visual);
    jbuf_free(&outline);
    free(url);
    free(html);
    return true;
}

static bool tool_browser_extract(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k = json_get_int(input, "top_k", 5);
    const char *effective_facet = (facet && *facet) ? facet : "visual";
    if (!query || !*query) {
        snprintf(result, rlen, "error: query required");
        free(query);
        free(facet);
        return false;
    }

    ctx_hit_t hits[CTX_SEARCH_MAX_K];
    char mode[64];
    int emit = ctx_rank_hits_ladder(query, "browser_snapshot", source_id, effective_facet,
                                    top_k, hits, CTX_SEARCH_MAX_K,
                                    mode, sizeof(mode));
    if (emit <= 0) {
        snprintf(result, rlen,
                 "no browser hits for query=%s source_id=%d facet=%s",
                 query, source_id, effective_facet);
    } else {
        size_t off = 0;
        int n = snprintf(result + off, rlen - off,
                         "context search query=%s\n"
                         "reranked=%d tool_filter=browser_snapshot source_id=%d facet=%s mode=%s\n\n",
                         query, emit, source_id, effective_facet,
                         mode[0] ? mode : "strict");
        if (n < 0) n = 0;
        if ((size_t)n < rlen - off) off += (size_t)n;

        for (int i = 0; i < emit && off + 64 < rlen; i++) {
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
                         (c->pinned || c->turn_pinned) ? "yes" : "no",
                         preview[0] ? preview : "(no preview)");
            if (n < 0 || (size_t)n >= rlen - off) break;
            off += (size_t)n;
        }
        snprintf(result + off, rlen - off,
                 "next: context_get {\"chunk_id\":<id>,\"max_chars\":4000}");
    }
    free(query);
    free(facet);
    return true;
}

static bool tool_browser_viewport(const char *input, char *result, size_t rlen) {
    int snapshot_id = json_get_int(input, "snapshot_id", -1);
    int requested_snapshot_id = snapshot_id;
    int offset = json_get_int(input, "offset", 1);
    int lines = json_get_int(input, "lines", 30);
    bool numbered = json_get_bool(input, "numbered", true);

    if (snapshot_id < 0) {
        snprintf(result, rlen, "error: snapshot_id required");
        return false;
    }
    if (offset < 1) offset = 1;
    if (lines < 5) lines = 5;
    if (lines > 160) lines = 160;

    browser_snapshot_t *snap = browser_snapshot_find(snapshot_id);
    if (!snap) {
        browser_snapshot_t *latest = browser_snapshot_find_latest_any();
        if (!latest) {
            snprintf(result, rlen, "error: snapshot_id %d not found", snapshot_id);
            return false;
        }
        snap = latest;
        snapshot_id = snap->id;
    }
    const char *text = snap->visual ? snap->visual : "";
    int total_lines = 0;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') total_lines++;
        if (*p == '\0') break;
    }
    if (offset > total_lines) offset = total_lines;
    if (offset < 1) offset = 1;

    const char *p = text;
    int cur = 1;
    while (*p && cur < offset) {
        if (*p == '\n') cur++;
        p++;
    }

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "browser_viewport snapshot_id=%d requested_snapshot_id=%d title=%s\n"
                     "url=%s\n"
                     "line_window=%d-%d of %d\n\n",
                     snap->id,
                     requested_snapshot_id,
                     snap->title[0] ? snap->title : "(none)",
                     snap->url,
                     offset,
                     (offset + lines - 1 < total_lines) ? (offset + lines - 1) : total_lines,
                     total_lines);
    if (n < 0) n = 0;
    if ((size_t)n >= rlen - off) {
        result[rlen - 1] = '\0';
        return true;
    }
    off += (size_t)n;

    int printed = 0;
    while (*p && printed < lines && off + 8 < rlen) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) len--;
        if (numbered) {
            n = snprintf(result + off, rlen - off, "%5d | ", offset + printed);
            if (n < 0 || (size_t)n >= rlen - off) break;
            off += (size_t)n;
        }
        if (len >= rlen - off) len = rlen - off - 1;
        memcpy(result + off, p, len);
        off += len;
        if (off + 1 >= rlen) break;
        result[off++] = '\n';
        result[off] = '\0';
        printed++;
        if (!eol) break;
        p = eol + 1;
    }

    snprintf(result + off, rlen - off,
             "\nnext: use offset=%d to continue scrolling",
             offset + printed);
    return true;
}

static bool tool_browser_outline(const char *input, char *result, size_t rlen) {
    int snapshot_id = json_get_int(input, "snapshot_id", -1);
    int requested_snapshot_id = snapshot_id;
    int max_chars = json_get_int(input, "max_chars", 5000);
    if (snapshot_id < 0) {
        snprintf(result, rlen, "error: snapshot_id required");
        return false;
    }
    if (max_chars < 400) max_chars = 400;
    if (max_chars > 20000) max_chars = 20000;

    browser_snapshot_t *snap = browser_snapshot_find(snapshot_id);
    if (!snap) {
        browser_snapshot_t *latest = browser_snapshot_find_latest_any();
        if (!latest) {
            snprintf(result, rlen, "error: snapshot_id %d not found", snapshot_id);
            return false;
        }
        snap = latest;
    }
    const char *outline = snap->outline ? snap->outline : "(no outline)";
    size_t off = 0;
    int n = 0;
    if (snap->id != requested_snapshot_id && requested_snapshot_id >= 0) {
        n = snprintf(result + off, rlen - off,
                     "note: requested snapshot_id=%d missing, using latest snapshot_id=%d\n\n",
                     requested_snapshot_id, snap->id);
        if (n < 0) n = 0;
        if ((size_t)n >= rlen - off) {
            result[rlen - 1] = '\0';
            return true;
        }
        off += (size_t)n;
    }

    size_t len = strlen(outline);
    if ((int)len > max_chars) len = (size_t)max_chars;
    if (len >= rlen - off) len = rlen - off - 1;
    memcpy(result + off, outline, len);
    off += len;
    result[off] = '\0';
    if (len < strlen(outline) && off + 40 < rlen) {
        strcat(result + off, "\n... outline truncated ...");
    }
    return true;
}

static bool tool_research_probe(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *query = json_get_str(input, "query");
    int timeout = json_get_int(input, "timeout", 30);
    int max_passes = json_get_int(input, "max_passes", 0);
    int top_k = json_get_int(input, "top_k", 4);
    if (!url) {
        snprintf(result, rlen, "error: url required");
        free(query);
        return false;
    }

    char html[MAX_TOOL_RESULT];
    html[0] = '\0';
    char fetch_meta[256];
    int status = browser_fetch_url_with_fallbacks(url, timeout, html, sizeof(html),
                                                  max_passes,
                                                  fetch_meta, sizeof(fetch_meta));
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
                     "research_probe url=%s fetch_meta=%s stored_chunks=%d chunk_id_range=%d-%d bytes_indexed=%zu\n",
                     url, fetch_meta[0] ? fetch_meta : "unknown",
                     info.chunks_added, info.first_chunk_id, info.last_chunk_id, info.bytes_added);
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
    char *filesystem = json_get_str(input, "filesystem");
    bool network = json_get_bool(input, "network", false);
    if (!command) {
        snprintf(result, rlen, "error: command required");
        free(filesystem);
        free(image);
        return false;
    }
    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;
    if (!filesystem) filesystem = safe_strdup("workspace_rw");
    if (strcmp(filesystem, "workspace_rw") != 0 &&
        strcmp(filesystem, "workspace_ro") != 0) {
        snprintf(result, rlen, "error: filesystem must be workspace_rw or workspace_ro");
        free(command);
        free(image);
        free(filesystem);
        return false;
    }

    char probe[64];
    int has_docker = (run_cmd("command -v docker >/dev/null 2>&1", probe, sizeof(probe)) == 0);
    const char *force_no_docker = getenv("DSCO_SANDBOX_FORCE_NO_DOCKER");
    if (force_no_docker && strcmp(force_no_docker, "1") == 0) {
        has_docker = 0;
    }
    run_opts_t opts = RUN_OPTS_DEFAULT;
    opts.wall_timeout_s = timeout;
    opts.idle_timeout_s = timeout > 120 ? 90 : timeout;
    opts.stream_to_tty = true;
    opts.dim_output = true;
    opts.label = "sandbox_run";

    char *escaped = shell_escape(command);
    jbuf_t cmd;
    jbuf_init(&cmd, 1024);
    if (has_docker) {
        if (!image || !*image) {
            free(image);
            image = safe_strdup("alpine:3.20");
        }
        jbuf_append(&cmd, "docker run --rm ");
        if (!network) jbuf_append(&cmd, "--network none ");
        jbuf_append(&cmd, "-v \"$PWD\":/workspace");
        if (strcmp(filesystem, "workspace_ro") == 0) jbuf_append(&cmd, ":ro");
        jbuf_append(&cmd, " -w /workspace ");
        shell_quote(&cmd, image);
        jbuf_append(&cmd, " sh -lc '");
        jbuf_append(&cmd, escaped);
        jbuf_append(&cmd, "'");
    } else {
        if (!network || strcmp(filesystem, "workspace_ro") == 0) {
            snprintf(result, rlen,
                     "error: strict sandbox policy requires docker "
                     "(network=%s filesystem=%s)",
                     network ? "true" : "false", filesystem);
            jbuf_free(&cmd);
            free(escaped);
            free(command);
            free(image);
            free(filesystem);
            return false;
        }
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
    free(filesystem);
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

static bool tool_plugin_validate(const char *input, char *result, size_t rlen) {
    char *manifest_path = json_get_str(input, "manifest_path");
    char *lock_path = json_get_str(input, "lock_path");
    bool ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, result, rlen);
    free(manifest_path);
    free(lock_path);
    return ok;
}

/* ── View Image (base64 encode for vision) ─────────────────────────────── */

static bool tool_view_image(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 5 * 1024 * 1024) { /* 5MB limit for images */
        snprintf(result, rlen, "error: file size %ld bytes (must be 1B-5MB)", fsize);
        fclose(f); free(path); return false;
    }

    unsigned char *raw = safe_malloc((size_t)fsize);
    size_t nread = fread(raw, 1, (size_t)fsize, f);
    fclose(f);

    /* Base64 encode using crypto.h */
    size_t b64_len = ((nread + 2) / 3) * 4 + 1;
    char *b64 = safe_malloc(b64_len);
    size_t oi = base64_encode(raw, nread, b64, b64_len);
    b64[oi] = '\0';
    free(raw);

    /* Determine media type from extension */
    const char *ext = strrchr(path, '.');
    const char *media_type = "image/png";
    if (ext) {
        if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) media_type = "image/jpeg";
        else if (strcasecmp(ext, ".gif") == 0) media_type = "image/gif";
        else if (strcasecmp(ext, ".webp") == 0) media_type = "image/webp";
        else if (strcasecmp(ext, ".svg") == 0) media_type = "image/svg+xml";
    }

    snprintf(result, rlen,
             "{\"path\":\"%s\",\"size\":%ld,\"media_type\":\"%s\","
             "\"base64_length\":%zu,\"note\":\"Image encoded. "
             "To analyze this image, the content will be included in the next API call.\"}",
             path, fsize, media_type, oi);

    /* Store the base64 data in a temp file for the agent to pick up */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/dsco_img_%d.b64", getpid());
    FILE *tmp = fopen(tmppath, "w");
    if (tmp) {
        fprintf(tmp, "%s\n%s", media_type, b64);
        fclose(tmp);
    }

    free(b64);
    free(path);
    return true;
}

/* ── View PDF (base64 encode for document analysis) ────────────────────── */

static bool tool_view_pdf(const char *input, char *result, size_t rlen) {
    char *path = json_get_str(input, "path");
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        free(path); return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 30 * 1024 * 1024) { /* 30MB limit for PDFs */
        snprintf(result, rlen, "error: file size %ld bytes (must be 1B-30MB)", fsize);
        fclose(f); free(path); return false;
    }

    unsigned char *raw = safe_malloc((size_t)fsize);
    size_t nread = fread(raw, 1, (size_t)fsize, f);
    fclose(f);

    /* Base64 encode using crypto.h */
    size_t b64_len = ((nread + 2) / 3) * 4 + 1;
    char *b64 = safe_malloc(b64_len);
    size_t oi = base64_encode(raw, nread, b64, b64_len);
    b64[oi] = '\0';
    free(raw);

    /* Store for agent loop injection */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/dsco_doc_%d.b64", getpid());
    FILE *tmp = fopen(tmppath, "w");
    if (tmp) {
        fprintf(tmp, "application/pdf\n%s", b64);
        fclose(tmp);
    }

    /* Get title from filename */
    const char *basename_p = strrchr(path, '/');
    basename_p = basename_p ? basename_p + 1 : path;

    snprintf(result, rlen,
             "{\"path\":\"%s\",\"size\":%ld,\"pages\":\"unknown\","
             "\"base64_length\":%zu,\"note\":\"PDF encoded. "
             "Content will be included in the next API call for analysis.\"}",
             path, fsize, oi);

    free(b64);
    free(path);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOOL REGISTRY
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Reusable AV schema strings (keep DRY across 100+ tools) ─────────── */
#define S_SYM  "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol (e.g. AAPL)\"}},\"required\":[\"symbol\"]}"
#define S_SYM_OUT "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact (100 pts) or full (20+ yrs)\"}},\"required\":[\"symbol\"]}"
#define S_INTRA "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min, 5min, 15min, 30min, 60min\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact or full\"},\"adjusted\":{\"type\":\"string\",\"description\":\"true/false (default true)\"},\"month\":{\"type\":\"string\",\"description\":\"YYYY-MM format for historical\"},\"extended_hours\":{\"type\":\"string\",\"description\":\"true/false (default true)\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_NONE "{\"type\":\"object\",\"properties\":{}}"
#define S_NEWS "{\"type\":\"object\",\"properties\":{\"tickers\":{\"type\":\"string\",\"description\":\"Comma-separated tickers\"},\"topics\":{\"type\":\"string\",\"description\":\"Topics: technology, finance, manufacturing, etc.\"},\"limit\":{\"type\":\"string\",\"description\":\"Number of articles (default 50)\"}}}"
#define S_FX "{\"type\":\"object\",\"properties\":{\"from_currency\":{\"type\":\"string\",\"description\":\"Source currency (e.g. USD)\"},\"to_currency\":{\"type\":\"string\",\"description\":\"Target currency (e.g. EUR)\"}},\"required\":[\"from_currency\",\"to_currency\"]}"
#define S_FXP "{\"type\":\"object\",\"properties\":{\"from_symbol\":{\"type\":\"string\",\"description\":\"Source currency (e.g. EUR)\"},\"to_symbol\":{\"type\":\"string\",\"description\":\"Target currency (e.g. USD)\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact or full\"}},\"required\":[\"from_symbol\",\"to_symbol\"]}"
#define S_FXI "{\"type\":\"object\",\"properties\":{\"from_symbol\":{\"type\":\"string\",\"description\":\"Source currency\"},\"to_symbol\":{\"type\":\"string\",\"description\":\"Target currency\"},\"interval\":{\"type\":\"string\",\"description\":\"1min, 5min, 15min, 30min, 60min\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact or full\"}},\"required\":[\"from_symbol\",\"to_symbol\",\"interval\"]}"
#define S_CRYPTO "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Crypto symbol (BTC, ETH, SOL)\"},\"market\":{\"type\":\"string\",\"description\":\"Market currency (default USD)\"}},\"required\":[\"symbol\"]}"
#define S_CRYI "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Crypto symbol\"},\"market\":{\"type\":\"string\",\"description\":\"Market (e.g. USD)\"},\"interval\":{\"type\":\"string\",\"description\":\"1min, 5min, 15min, 30min, 60min\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact or full\"}},\"required\":[\"symbol\",\"market\",\"interval\"]}"
#define S_INTV "{\"type\":\"object\",\"properties\":{\"interval\":{\"type\":\"string\",\"description\":\"daily, weekly, or monthly\"}}}"
#define S_TREAS "{\"type\":\"object\",\"properties\":{\"interval\":{\"type\":\"string\",\"description\":\"daily, weekly, monthly\"},\"maturity\":{\"type\":\"string\",\"description\":\"3month, 2year, 5year, 7year, 10year, 30year\"}}}"
#define S_ECON "{\"type\":\"object\",\"properties\":{\"interval\":{\"type\":\"string\",\"description\":\"annual, quarterly, monthly, semiannual\"}}}"
#define S_IND "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min, 5min, 15min, 30min, 60min, daily, weekly, monthly\"},\"time_period\":{\"type\":\"string\",\"description\":\"Data points for calculation (e.g. 14, 200)\"},\"series_type\":{\"type\":\"string\",\"description\":\"close, open, high, low\"},\"month\":{\"type\":\"string\",\"description\":\"YYYY-MM for intraday history\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_MACD "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"series_type\":{\"type\":\"string\",\"description\":\"close, open, high, low\"},\"fastperiod\":{\"type\":\"string\",\"description\":\"Default 12\"},\"slowperiod\":{\"type\":\"string\",\"description\":\"Default 26\"},\"signalperiod\":{\"type\":\"string\",\"description\":\"Default 9\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_STOCH "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"fastkperiod\":{\"type\":\"string\",\"description\":\"Default 5\"},\"slowkperiod\":{\"type\":\"string\",\"description\":\"Default 3\"},\"slowdperiod\":{\"type\":\"string\",\"description\":\"Default 3\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_BB "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"time_period\":{\"type\":\"string\",\"description\":\"Data points for calculation\"},\"series_type\":{\"type\":\"string\",\"description\":\"close, open, high, low\"},\"nbdevup\":{\"type\":\"string\",\"description\":\"Upper band std dev (default 2)\"},\"nbdevdn\":{\"type\":\"string\",\"description\":\"Lower band std dev (default 2)\"},\"matype\":{\"type\":\"string\",\"description\":\"0=SMA,1=EMA,2=WMA,3=DEMA,4=TEMA,5=TRIMA,6=T3,7=KAMA,8=MAMA\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_SAR "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"acceleration\":{\"type\":\"string\",\"description\":\"Default 0.02\"},\"maximum\":{\"type\":\"string\",\"description\":\"Default 0.20\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_ADOSC "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"fastperiod\":{\"type\":\"string\",\"description\":\"Default 3\"},\"slowperiod\":{\"type\":\"string\",\"description\":\"Default 10\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_ULTOSC "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"timeperiod1\":{\"type\":\"string\",\"description\":\"Default 7\"},\"timeperiod2\":{\"type\":\"string\",\"description\":\"Default 14\"},\"timeperiod3\":{\"type\":\"string\",\"description\":\"Default 28\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_MAMA "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"series_type\":{\"type\":\"string\",\"description\":\"close, open, high, low\"},\"fastlimit\":{\"type\":\"string\",\"description\":\"Default 0.01\"},\"slowlimit\":{\"type\":\"string\",\"description\":\"Default 0.01\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_MACDEXT "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"series_type\":{\"type\":\"string\",\"description\":\"close, open, high, low\"},\"fastperiod\":{\"type\":\"string\"},\"slowperiod\":{\"type\":\"string\"},\"signalperiod\":{\"type\":\"string\"},\"fastmatype\":{\"type\":\"string\"},\"slowmatype\":{\"type\":\"string\"},\"signalmatype\":{\"type\":\"string\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_IND_STOCHF "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min-60min, daily, weekly, monthly\"},\"fastkperiod\":{\"type\":\"string\",\"description\":\"Default 5\"},\"fastdperiod\":{\"type\":\"string\",\"description\":\"Default 3\"},\"fastdmatype\":{\"type\":\"string\",\"description\":\"Default 0 (SMA)\"}},\"required\":[\"symbol\",\"interval\"]}"
#define S_GOLD "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"GOLD/XAU or SILVER/XAG\"}},\"required\":[\"symbol\"]}"
#define S_GOLDH "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"GOLD/XAU or SILVER/XAG\"},\"interval\":{\"type\":\"string\",\"description\":\"daily, weekly, monthly\"}},\"required\":[\"symbol\"]}"
#define S_OPTS "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"contract\":{\"type\":\"string\",\"description\":\"Specific options contract ID (optional)\"},\"require_greeks\":{\"type\":\"string\",\"description\":\"true to include Greeks & IV\"}},\"required\":[\"symbol\"]}"
#define S_OPTSH "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"date\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD (any date after 2008-01-01)\"}},\"required\":[\"symbol\"]}"
#define S_TRANS "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"quarter\":{\"type\":\"string\",\"description\":\"Fiscal quarter in YYYYQN format (e.g. 2024Q1)\"}},\"required\":[\"symbol\",\"quarter\"]}"
#define S_LIST "{\"type\":\"object\",\"properties\":{\"date\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD (optional, default latest)\"},\"state\":{\"type\":\"string\",\"description\":\"active or delisted (default active)\"}}}"
#define S_ECAL "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Ticker (optional, default all)\"},\"horizon\":{\"type\":\"string\",\"description\":\"3month, 6month, or 12month\"}}}"
#define S_BULK "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Up to 100 symbols comma-separated (e.g. AAPL,MSFT,GOOG)\"}},\"required\":[\"symbol\"]}"
#define S_AFIXED "{\"type\":\"object\",\"properties\":{\"symbols\":{\"type\":\"string\",\"description\":\"Comma-separated symbols (up to 50)\"},\"range\":{\"type\":\"string\",\"description\":\"Date range\"},\"interval\":{\"type\":\"string\",\"description\":\"DAILY, WEEKLY, MONTHLY, 1min-60min\"},\"calculations\":{\"type\":\"string\",\"description\":\"Metrics: MEAN, VARIANCE, STDDEV, MAX, MIN, etc.\"},\"ohlc\":{\"type\":\"string\",\"description\":\"open, high, low, close (default close)\"}},\"required\":[\"symbols\",\"range\",\"interval\",\"calculations\"]}"
#define S_ASLIDE "{\"type\":\"object\",\"properties\":{\"symbols\":{\"type\":\"string\",\"description\":\"Comma-separated symbols\"},\"range\":{\"type\":\"string\",\"description\":\"Date range\"},\"interval\":{\"type\":\"string\",\"description\":\"DAILY, WEEKLY, MONTHLY, 1min-60min\"},\"window_size\":{\"type\":\"string\",\"description\":\"Sliding window size (min 10)\"},\"calculations\":{\"type\":\"string\",\"description\":\"Metrics to calculate\"},\"ohlc\":{\"type\":\"string\",\"description\":\"open, high, low, close\"}},\"required\":[\"symbols\",\"range\",\"interval\",\"window_size\",\"calculations\"]}"
#define S_KWD "{\"type\":\"object\",\"properties\":{\"keywords\":{\"type\":\"string\",\"description\":\"Search keywords (company name or partial ticker)\"}},\"required\":[\"keywords\"]}"


/* ═══ CSV PARSE ═══ */
static bool tool_csv_parse(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = json_get_str(input, "file");
    int column = json_get_int(input, "column", -1);
    char *delimiter_str = json_get_str(input, "delimiter");
    char delim = (delimiter_str && delimiter_str[0]) ? delimiter_str[0] : ',';
    bool headers = json_get_bool(input, "headers", true);
    char *data = NULL;
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f) { snprintf(result, rlen, "error: cannot open %s", file); free(text); free(file); free(delimiter_str); return false; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 1024*1024) sz = 1024*1024; /* cap at 1MB */
        data = safe_malloc((size_t)sz + 1);
        size_t n = fread(data, 1, (size_t)sz, f); data[n] = 0;
        fclose(f);
    } else if (text) {
        data = safe_strdup(text);
    } else {
        snprintf(result, rlen, "error: text or file required");
        free(delimiter_str);
        return false;
    }
    jbuf_t b; jbuf_init(&b, strlen(data) + 256);
    jbuf_append(&b, "{\"rows\":[");
    char *line = data, *next;
    int row = 0;
    char **header_names = NULL; int ncols_header = 0;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        /* Trim \r */
        size_t ll = strlen(line); if (ll > 0 && line[ll-1] == '\r') line[ll-1] = 0;
        if (row == 0 && headers) {
            /* Parse header names */
            char *h = line;
            while (h) {
                char *sep = strchr(h, delim);
                if (sep) *sep = 0;
                header_names = realloc(header_names, (ncols_header+1)*sizeof(char*));
                header_names[ncols_header++] = safe_strdup(h);
                h = sep ? sep+1 : NULL;
            }
            row++; line = next; continue;
        }
        if (column >= 0) {
            /* Extract specific column */
            char *col = line; int ci = 0;
            while (ci < column && col) { col = strchr(col, delim); if (col) col++; ci++; }
            if (col) {
                char *end = strchr(col, delim); if (end) *end = 0;
                if (row > (headers ? 1 : 0)) jbuf_append(&b, ",");
                jbuf_append(&b, "\""); jbuf_append_json_str(&b, col); jbuf_append(&b, "\"");
            }
        } else {
            if (row > (headers ? 1 : 0)) jbuf_append(&b, ",");
            jbuf_append(&b, "[");
            char *col = line; int ci = 0;
            while (col) {
                char *sep = strchr(col, delim); if (sep) *sep = 0;
                if (ci > 0) jbuf_append(&b, ",");
                jbuf_append(&b, "\""); jbuf_append_json_str(&b, col); jbuf_append(&b, "\"");
                col = sep ? sep+1 : NULL; ci++;
            }
            jbuf_append(&b, "]");
        }
        row++; line = next;
    }
    jbuf_appendf(&b, "],\"total_rows\":%d", row - (headers ? 1 : 0));
    if (header_names) {
        jbuf_append(&b, ",\"headers\":[");
        for (int i = 0; i < ncols_header; i++) {
            if (i) jbuf_append(&b, ",");
            jbuf_append(&b, "\""); jbuf_append_json_str(&b, header_names[i]); jbuf_append(&b, "\"");
            free(header_names[i]);
        }
        jbuf_append(&b, "]");
        free(header_names);
    }
    jbuf_append(&b, "}");
    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b); free(data); free(text); free(file); free(delimiter_str);
    return true;
}
/* ═══ REGEX MATCH ═══ */
static bool tool_regex_match(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *pattern = json_get_str(input, "pattern");
    bool global = json_get_bool(input, "global", false);
    if (!text || !pattern) {
        snprintf(result, rlen, "{\"error\":\"text and pattern required\"}");
        free(text); free(pattern); return false;
    }
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) {
        snprintf(result, rlen, "{\"error\":\"invalid regex\"}");
        free(text); free(pattern); return false;
    }
    jbuf_t b; jbuf_init(&b, 512);
    jbuf_append(&b, "{\"matches\":[");
    char *p = text; int count = 0; regmatch_t m;
    while (regexec(&re, p, 1, &m, p == text ? 0 : REG_NOTBOL) == 0) {
        if (count > 0) jbuf_append(&b, ",");
        jbuf_append(&b, "\"");
        for (regoff_t i = m.rm_so; i < m.rm_eo; i++) jbuf_appendf(&b, "%c", p[i]);
        jbuf_append(&b, "\"");
        count++;
        p += m.rm_eo;
        if (!global || m.rm_eo == 0) break;
    }
    regfree(&re);
    jbuf_appendf(&b, "],\"count\":%d}", count);
    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b); free(text); free(pattern);
    return true;
}

/* ═══ URL PARSE ═══ */
static bool tool_url_parse(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    if (!url) { snprintf(result, rlen, "{\"error\":\"url required\"}"); return false; }
    char scheme[64]="", host[256]="", path[1024]="", query[1024]="", fragment[256]="";
    int port = 0;
    char *p = url;
    /* scheme */
    char *sep = strstr(p, "://");
    if (sep) {
        size_t slen = (size_t)(sep - p); if (slen >= sizeof(scheme)) slen = sizeof(scheme)-1;
        strncpy(scheme, p, slen); scheme[slen] = 0;
        p = sep + 3;
    }
    /* host:port */
    char *slash = strchr(p, '/');
    char *qmark = strchr(p, '?');
    char *hash  = strchr(p, '#');
    char *hostend = slash ? slash : (qmark ? qmark : (hash ? hash : p + strlen(p)));
    size_t hlen = (size_t)(hostend - p); if (hlen >= sizeof(host)) hlen = sizeof(host)-1;
    strncpy(host, p, hlen); host[hlen] = 0;
    char *colon = strrchr(host, ':');
    if (colon && colon > host) { port = atoi(colon+1); *colon = 0; }
    p = hostend;
    /* path */
    if (*p == '/') {
        char *pe = qmark ? qmark : (hash ? hash : p + strlen(p));
        size_t plen = (size_t)(pe - p); if (plen >= sizeof(path)) plen = sizeof(path)-1;
        strncpy(path, p, plen); path[plen] = 0; p = pe;
    }
    /* query */
    if (*p == '?') {
        p++;
        char *qe = hash ? hash : p + strlen(p);
        size_t qlen = (size_t)(qe - p); if (qlen >= sizeof(query)) qlen = sizeof(query)-1;
        strncpy(query, p, qlen); query[qlen] = 0; p = qe;
    }
    /* fragment */
    if (*p == '#') { strncpy(fragment, p+1, sizeof(fragment)-1); }
    snprintf(result, rlen,
        "{\"scheme\":\"%s\",\"host\":\"%s\",\"port\":%d,\"path\":\"%s\",\"query\":\"%s\",\"fragment\":\"%s\"}",
        scheme, host, port, path, query, fragment);
    free(url);
    return true;
}

/* ═══ SEMVER COMPARE ═══ */
static bool tool_semver(const char *input, char *result, size_t rlen) {
    char *va = json_get_str(input, "version_a");
    char *vb = json_get_str(input, "version_b");
    if (!va || !vb) { snprintf(result, rlen, "{\"error\":\"version_a and version_b required\"}"); free(va); free(vb); return false; }
    int a1=0,a2=0,a3=0, b1=0,b2=0,b3=0;
    sscanf(va, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(vb, "%d.%d.%d", &b1, &b2, &b3);
    int cmp = (a1!=b1) ? (a1>b1?1:-1) : (a2!=b2) ? (a2>b2?1:-1) : (a3!=b3) ? (a3>b3?1:-1) : 0;
    snprintf(result, rlen, "{\"result\":%d,\"version_a\":\"%s\",\"version_b\":\"%s\",\"comparison\":\"%s\"}",
        cmp, va, vb, cmp<0?"less":cmp>0?"greater":"equal");
    free(va); free(vb);
    return true;
}

/* ═══ CRON PARSE ═══ */
static bool tool_cron_parse(const char *input, char *result, size_t rlen) {
    char *expr = json_get_str(input, "expression");
    if (!expr) { snprintf(result, rlen, "{\"error\":\"expression required\"}"); return false; }
    /* Parse 5-field cron: min hour dom mon dow */
    char min[32]="*", hour[32]="*", dom[32]="*", mon[32]="*", dow[32]="*";
    int n = sscanf(expr, "%31s %31s %31s %31s %31s", min, hour, dom, mon, dow);
    const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    jbuf_t b; jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"fields\":%d,\"minute\":\"%s\",\"hour\":\"%s\",\"day_of_month\":\"%s\",\"month\":\"%s\",\"day_of_week\":\"%s\"",
        n, min, hour, dom, mon, dow);
    /* Human description */
    jbuf_append(&b, ",\"description\":\"");
    if (strcmp(min,"*")==0 && strcmp(hour,"*")==0) jbuf_append(&b, "every minute");
    else if (strcmp(min,"0")==0 && strcmp(hour,"*")==0) jbuf_append(&b, "every hour");
    else { jbuf_appendf(&b, "at %s:%s", hour, min); }
    if (strcmp(dom,"*")!=0) jbuf_appendf(&b, " on day %s", dom);
    if (strcmp(mon,"*")!=0) {
        int mi = atoi(mon)-1;
        if (mi>=0 && mi<12) jbuf_appendf(&b, " in %s", mons[mi]);
        else jbuf_appendf(&b, " in month %s", mon);
    }
    if (strcmp(dow,"*")!=0) {
        int di = atoi(dow);
        if (di>=0 && di<7) jbuf_appendf(&b, " on %s", days[di]);
        else jbuf_appendf(&b, " on weekday %s", dow);
    }
    jbuf_append(&b, "\"}");
    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b); free(expr);
    return true;
}

/* ═══ TEMPLATE RENDER ═══ */
static bool tool_template_render(const char *input, char *result, size_t rlen) {
    char *tmpl = json_get_str(input, "template");
    char *vars = json_get_str(input, "variables");
    if (!tmpl) { snprintf(result, rlen, "{\"error\":\"template required\"}"); free(vars); return false; }
    jbuf_t b; jbuf_init(&b, strlen(tmpl) * 2 + 256);
    const char *p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p+2, "}}");
            if (!end) { jbuf_append_char(&b, *p++); continue; }
            size_t klen = (size_t)(end - (p+2));
            char key[256] = {0};
            if (klen < sizeof(key)) { strncpy(key, p+2, klen); key[klen] = 0; }
            /* Trim whitespace */
            char *ks = key; while (*ks == ' ') ks++;
            char *ke = ks + strlen(ks)-1; while (ke > ks && *ke == ' ') *ke-- = 0;
            /* Look up in vars JSON */
            char *val = vars ? json_get_str(vars, ks) : NULL;
            if (val) { jbuf_append_json_str(&b, val); free(val); }
            else jbuf_appendf(&b, "{{%s}}", ks);
            p = end + 2;
        } else {
            jbuf_append_char(&b, *p++);
        }
    }
    snprintf(result, rlen, "%s", b.data ? b.data : "");
    jbuf_free(&b); free(tmpl); free(vars);
    return true;
}

/* ═══ TEXT DIFF ═══ */
static bool tool_text_diff(const char *input, char *result, size_t rlen) {
    char *a = json_get_str(input, "text_a");
    char *b_text = json_get_str(input, "text_b");
    if (!a || !b_text) {
        snprintf(result, rlen, "{\"error\":\"text_a and text_b required\"}");
        free(a); free(b_text); return false;
    }
    /* Write to temp files and diff */
    char fa[] = "/tmp/dsco_diff_a_XXXXXX", fb[] = "/tmp/dsco_diff_b_XXXXXX";
    int fda = mkstemp(fa), fdb = mkstemp(fb);
    if (fda < 0 || fdb < 0) {
        snprintf(result, rlen, "{\"error\":\"failed to create temp files\"}");
        if (fda>=0) close(fda); if (fdb>=0) close(fdb);
        free(a); free(b_text); return false;
    }
    write(fda, a, strlen(a)); close(fda);
    write(fdb, b_text, strlen(b_text)); close(fdb);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "diff -u %s %s 2>/dev/null; true", fa, fb);
    run_opts_t opts = RUN_OPTS_DEFAULT; opts.stream_to_tty = false; opts.wall_timeout_s = 10;
    char diff_out[65536] = {0};
    run_cmd_ex(cmd, diff_out, sizeof(diff_out), &opts);
    unlink(fa); unlink(fb);
    jbuf_t buf; jbuf_init(&buf, strlen(diff_out) + 64);
    jbuf_append(&buf, "{\"diff\":\"");
    jbuf_append_json_str(&buf, diff_out);
    jbuf_append(&buf, "\"}");
    snprintf(result, rlen, "%s", buf.data ? buf.data : "{}");
    jbuf_free(&buf); free(a); free(b_text);
    return true;
}

/* ═══ PROCESS TREE ═══ */
static bool tool_process_tree(const char *input, char *result, size_t rlen) {
    char *filter = json_get_str(input, "filter");
    char cmd[512];
    if (filter && *filter) {
        char *esc = shell_escape(filter);
        snprintf(cmd, sizeof(cmd), "ps -eo pid,ppid,user,pcpu,pmem,comm | head -1; ps -eo pid,ppid,user,pcpu,pmem,comm | grep -i %s 2>/dev/null | head -40", esc);
        free(esc);
    } else {
        snprintf(cmd, sizeof(cmd), "ps -eo pid,ppid,user,pcpu,pmem,comm | head -50");
    }
    run_opts_t opts = RUN_OPTS_DEFAULT; opts.stream_to_tty = false; opts.wall_timeout_s = 10;
    run_cmd_ex(cmd, result, rlen, &opts);
    free(filter);
    return true;
}

/* ═══ SYSTEM PROFILER ═══ */
static bool tool_system_profiler(const char *input, char *result, size_t rlen) {
    char *section = json_get_str(input, "section");
    jbuf_t b; jbuf_init(&b, 8192);
    char buf[4096];
    run_opts_t opts = RUN_OPTS_DEFAULT; opts.stream_to_tty = false; opts.wall_timeout_s = 15;
    bool do_all = !section || !*section || strcmp(section,"all")==0;
    if (do_all || strcmp(section,"cpu")==0) {
        run_cmd_ex("sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize 2>/dev/null || lscpu 2>/dev/null | head -20", buf, sizeof(buf), &opts);
        jbuf_append(&b, "=== CPU/Memory ===\n"); jbuf_append(&b, buf);
    }
    if (do_all || strcmp(section,"disk")==0) {
        run_cmd_ex("df -h 2>/dev/null | head -20", buf, sizeof(buf), &opts);
        jbuf_append(&b, "\n=== Disk Usage ===\n"); jbuf_append(&b, buf);
    }
    if (do_all || strcmp(section,"network")==0) {
        run_cmd_ex("ifconfig 2>/dev/null | grep -A1 'inet ' | head -20 || ip addr 2>/dev/null | grep inet | head -20", buf, sizeof(buf), &opts);
        jbuf_append(&b, "\n=== Network ===\n"); jbuf_append(&b, buf);
    }
    if (do_all || strcmp(section,"load")==0) {
        run_cmd_ex("uptime && vm_stat 2>/dev/null | head -10 || free -h 2>/dev/null", buf, sizeof(buf), &opts);
        jbuf_append(&b, "\n=== Load ===\n"); jbuf_append(&b, buf);
    }
    snprintf(result, rlen, "%s", b.data ? b.data : "no data");
    jbuf_free(&b); free(section);
    return true;
}

/* ═══ STRING OPS ═══ */
static bool tool_string_ops(const char *input, char *result, size_t rlen) {
    char *op  = json_get_str(input, "op");
    char *text = json_get_str(input, "text");
    if (!op || !text) { snprintf(result, rlen, "{\"error\":\"op and text required\"}"); free(op); free(text); return false; }
    jbuf_t b; jbuf_init(&b, strlen(text)+256);
    if (strcmp(op,"upper")==0) {
        for (char *p = text; *p; p++) jbuf_append_char(&b, (char)toupper((unsigned char)*p));
    } else if (strcmp(op,"lower")==0) {
        for (char *p = text; *p; p++) jbuf_append_char(&b, (char)tolower((unsigned char)*p));
    } else if (strcmp(op,"trim")==0) {
        char *s = text; while (*s == ' '||*s=='\t'||*s=='\n'||*s=='\r') s++;
        char *e = s + strlen(s)-1; while (e > s && (*e==' '||*e=='\t'||*e=='\n'||*e=='\r')) e--;
        jbuf_append_len(&b, s, (size_t)(e-s+1));
    } else if (strcmp(op,"reverse")==0) {
        size_t l = strlen(text);
        for (size_t i = l; i > 0; i--) jbuf_append_char(&b, text[i-1]);
    } else if (strcmp(op,"length")==0) {
        snprintf(result, rlen, "{\"length\":%zu}", strlen(text));
        jbuf_free(&b); free(op); free(text); return true;
    } else if (strcmp(op,"base64_encode")==0) {
        static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const unsigned char *s = (const unsigned char *)text; size_t l = strlen(text);
        for (size_t i = 0; i < l; i+=3) {
            unsigned v = (unsigned)s[i]<<16 | (i+1<l?(unsigned)s[i+1]:0)<<8 | (i+2<l?(unsigned)s[i+2]:0);
            jbuf_append_char(&b, b64c[(v>>18)&63]); jbuf_append_char(&b, b64c[(v>>12)&63]);
            jbuf_append_char(&b, i+1<l?b64c[(v>>6)&63]:'=');
            jbuf_append_char(&b, i+2<l?b64c[v&63]:'=');
        }
    } else if (strcmp(op,"word_count")==0) {
        int words=0; bool inw=false;
        for (char *p=text; *p; p++) { if (*p==' '||*p=='\t'||*p=='\n') inw=false; else if(!inw){words++;inw=true;} }
        snprintf(result, rlen, "{\"word_count\":%d}", words);
        jbuf_free(&b); free(op); free(text); return true;
    } else {
        snprintf(result, rlen, "{\"error\":\"unknown op: %s\"}", op);
        jbuf_free(&b); free(op); free(text); return false;
    }
    jbuf_append(&b, "\0");
    snprintf(result, rlen, "{\"result\":\"%s\"}", b.data ? b.data : "");
    jbuf_free(&b); free(op); free(text);
    return true;
}

/* ═══ XML EXTRACT ═══ */
static bool tool_xml_extract(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = json_get_str(input, "file");
    char *tag  = json_get_str(input, "tag");
    char *attr_name = json_get_str(input, "attribute");
    char *data = NULL;
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f) { snprintf(result, rlen, "{\"error\":\"cannot open %s\"}", file); free(text); free(file); free(tag); free(attr_name); return false; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 2*1024*1024) sz = 2*1024*1024;
        data = safe_malloc((size_t)sz + 1);
        size_t n = fread(data, 1, (size_t)sz, f); data[n] = 0; fclose(f);
    } else if (text) {
        data = safe_strdup(text);
    } else {
        snprintf(result, rlen, "{\"error\":\"text or file required\"}");
        free(tag); free(attr_name); return false;
    }
    if (!tag || !*tag) {
        snprintf(result, rlen, "{\"error\":\"tag required\"}");
        free(data); free(text); free(file); free(tag); free(attr_name); return false;
    }
    jbuf_t b; jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"matches\":[");
    char open_tag[256], close_tag[256];
    snprintf(open_tag, sizeof(open_tag), "<%s", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    char *p = data; int count = 0;
    while ((p = strstr(p, open_tag)) != NULL) {
        /* Verify it's actually the tag (not a prefix) */
        char next = p[strlen(open_tag)];
        if (next != '>' && next != ' ' && next != '\t' && next != '\n' && next != '/') { p++; continue; }
        char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        if (attr_name && *attr_name) {
            /* Extract attribute value */
            char attr_search[270]; snprintf(attr_search, sizeof(attr_search), "%s=\"", attr_name);
            char *apos = strstr(p, attr_search);
            if (apos && apos < tag_end) {
                apos += strlen(attr_search);
                char *aend = strchr(apos, '"');
                if (aend && aend <= tag_end) {
                    if (count > 0) jbuf_append(&b, ",");
                    jbuf_append(&b, "\""); jbuf_append_len(&b, apos, (size_t)(aend-apos)); jbuf_append(&b, "\"");
                    count++;
                }
            }
        } else {
            /* Extract inner text */
            char *content_start = tag_end + 1;
            char *content_end = strstr(content_start, close_tag);
            if (!content_end) { p = tag_end; continue; }
            if (count > 0) jbuf_append(&b, ",");
            jbuf_append(&b, "\"");
            jbuf_append_len(&b, content_start, (size_t)(content_end - content_start));
            jbuf_append(&b, "\"");
            count++;
            p = content_end + strlen(close_tag);
            continue;
        }
        p = tag_end;
    }
    jbuf_appendf(&b, "],\"count\":%d}", count);
    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b); free(data); free(text); free(file); free(tag); free(attr_name);
    return true;
}

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
        .name = "soul_read",
        .description = "Read the workspace SOUL.md personality file.",
        .input_schema_json = S_NONE,
        .execute = tool_soul_read
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
        .name = "soul_append",
        .description = "Append content to the workspace SOUL.md personality file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"Text to append to SOUL.md\"}},\"required\":[\"content\"]}",
        .execute = tool_soul_append
    },
    {
        .name = "soul_write",
        .description = "Overwrite the workspace SOUL.md personality file.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"New complete SOUL.md content\"}},\"required\":[\"content\"]}",
        .execute = tool_soul_write
    },
    {
        .name = "soul_replace",
        .description = "Replace text in SOUL.md by exact match; by default, the match must be unique.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to replace\"},\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"},\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace all occurrences when true\"}},\"required\":[\"old_string\",\"new_string\"]}",
        .execute = tool_soul_replace
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
        .description = "Search the chunked retrieval context (dense+lexical reranking) with optional source/facet metadata filters.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Focused retrieval query\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool-name filter\"},\"source_id\":{\"type\":\"integer\",\"description\":\"Optional source/snapshot id\"},\"facet\":{\"type\":\"string\",\"description\":\"Optional facet filter (raw|visual|outline|...)\"},\"top_k\":{\"type\":\"integer\",\"description\":\"How many hits to return (default 5, max 12)\"}},\"required\":[\"query\"]}",
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
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Summary query\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool filter\"},\"source_id\":{\"type\":\"integer\",\"description\":\"Optional source/snapshot id\"},\"facet\":{\"type\":\"string\",\"description\":\"Optional facet filter\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Top chunks to summarize (default 4)\"},\"max_chars_per_chunk\":{\"type\":\"integer\",\"description\":\"Preview size per chunk\"}},\"required\":[\"query\"]}",
        .execute = tool_context_summarize
    },
    {
        .name = "context_pack",
        .description = "Pack retrieval evidence into a strict character/token budget with chunk citations.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Packing query\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool filter\"},\"source_id\":{\"type\":\"integer\",\"description\":\"Optional source/snapshot id\"},\"facet\":{\"type\":\"string\",\"description\":\"Optional facet filter\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Candidate hits before packing\"},\"max_chars_total\":{\"type\":\"integer\",\"description\":\"Total packed char budget\"},\"max_chars_per_chunk\":{\"type\":\"integer\",\"description\":\"Per-chunk cap\"}},\"required\":[\"query\"]}",
        .execute = tool_context_pack
    },
    {
        .name = "context_fuse",
        .description = "Fuse multiple retrieval queries via reciprocal-rank fusion (RRF) before packing/get.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Single fallback query\"},\"queries\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Multiple focused queries\"},\"tool\":{\"type\":\"string\",\"description\":\"Optional tool filter\"},\"source_id\":{\"type\":\"integer\",\"description\":\"Optional source/snapshot id\"},\"facet\":{\"type\":\"string\",\"description\":\"Optional facet filter\"},\"top_k_each\":{\"type\":\"integer\",\"description\":\"Hits per query\"},\"final_k\":{\"type\":\"integer\",\"description\":\"Final fused hit count\"}}}",
        .execute = tool_context_fuse
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
        .description = "Fetch and index a browser-grade snapshot with adaptive multi-pass fallbacks: raw HTML + visual text + DOM outline.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Page URL\"},\"timeout\":{\"type\":\"integer\",\"description\":\"HTTP timeout seconds (default 30)\"},\"max_passes\":{\"type\":\"integer\",\"description\":\"Override adaptive fetch passes (1-8)\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Max response chars to ingest\"}},\"required\":[\"url\"]}",
        .execute = tool_browser_snapshot
    },
    {
        .name = "browser_extract",
        .description = "Query indexed browser snapshots with optional snapshot_id/facet filters (visual facet default).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Extraction query\"},\"source_id\":{\"type\":\"integer\",\"description\":\"Snapshot id filter\"},\"facet\":{\"type\":\"string\",\"description\":\"Facet filter (raw|visual|outline)\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Number of hits (default 5)\"}},\"required\":[\"query\"]}",
        .execute = tool_browser_extract
    },
    {
        .name = "browser_viewport",
        .description = "Scroll a visual snapshot like a browser viewport (line offset + window size).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"snapshot_id\":{\"type\":\"integer\",\"description\":\"Snapshot id from browser_snapshot\"},\"offset\":{\"type\":\"integer\",\"description\":\"1-based start line\"},\"lines\":{\"type\":\"integer\",\"description\":\"Lines per viewport\"},\"numbered\":{\"type\":\"boolean\",\"description\":\"Include line numbers (default true)\"}},\"required\":[\"snapshot_id\"]}",
        .execute = tool_browser_viewport
    },
    {
        .name = "browser_outline",
        .description = "Return the structural DOM outline (headings + key links + layout stats) for a snapshot.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"snapshot_id\":{\"type\":\"integer\",\"description\":\"Snapshot id\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Max chars to return\"}},\"required\":[\"snapshot_id\"]}",
        .execute = tool_browser_outline
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
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Source URL\"},\"query\":{\"type\":\"string\",\"description\":\"Optional focused question\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Hit count for query (default 4)\"},\"timeout\":{\"type\":\"integer\",\"description\":\"HTTP timeout\"},\"max_passes\":{\"type\":\"integer\",\"description\":\"Override adaptive fetch passes (1-8)\"}},\"required\":[\"url\"]}",
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
        .description = "Run a command in a constrained environment with explicit filesystem/network policy.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command to execute\"},\"image\":{\"type\":\"string\",\"description\":\"Optional docker image\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout seconds (default 60)\"},\"network\":{\"type\":\"boolean\",\"description\":\"Allow network egress (default false)\"},\"filesystem\":{\"type\":\"string\",\"description\":\"workspace_rw or workspace_ro (default workspace_rw)\"}},\"required\":[\"command\"]}",
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
        .name = "web_extract",
        .description = "Fetch a URL and extract readable text content, stripping HTML tags. Much more token-efficient than raw HTTP. Good for reading articles, docs, search results.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to fetch and extract text from\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Maximum characters to return (default 8000)\"}},\"required\":[\"url\"]}",
        .execute = tool_web_extract
    },
    {
        .name = "screenshot",
        .description = "Take a screenshot on macOS. Saves as PNG. Use full_screen=false for interactive region selection.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Output file path (default /tmp/dsco_screenshot.png)\"},\"full_screen\":{\"type\":\"boolean\",\"description\":\"Capture full screen (default true)\"},\"delay\":{\"type\":\"integer\",\"description\":\"Delay in seconds before capture\"}}}",
        .execute = tool_screenshot
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
        .name = "market_quote",
        .description = "Fetch market quotes with Yahoo+Stooq fallback, richer analytics, staleness checks, and optional JSON output.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\",\"description\":\"Single ticker symbol (e.g. TSLA, AAPL, BTC-USD, EURUSD=X)\"},\"ticker\":{\"type\":\"string\",\"description\":\"Alias for symbol\"},\"symbols\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Batch symbols as JSON array. Tool also accepts comma-separated string for compatibility.\"},\"timeout\":{\"type\":\"integer\",\"description\":\"HTTP timeout in seconds (default 12)\"},\"stale_after_sec\":{\"type\":\"integer\",\"description\":\"Mark quote stale if older than this many seconds (default 172800)\"},\"source_preference\":{\"type\":\"string\",\"description\":\"auto (default), yahoo, stooq, yahoo_only, or stooq_only\"},\"format\":{\"type\":\"string\",\"description\":\"Output format: text (default) or json\"}}}",
        .execute = tool_market_quote
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
        .name = "topology_list",
        .description = "List advanced swarm topologies or inspect one topology in detail. Use this to discover chain, fanout, hierarchy, mesh, feedback, competitive, and domain orchestration graphs.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Optional topology name to inspect\"},\"topology_id\":{\"type\":\"integer\",\"description\":\"Optional topology ID to inspect\"},\"query\":{\"type\":\"string\",\"description\":\"Substring/contains search against topology name and description\"},\"category\":{\"type\":\"string\",\"description\":\"Filter by category: chain|fanout|hierarchy|mesh|specialist|feedback|competitive|domain\"},\"strategy\":{\"type\":\"string\",\"description\":\"Filter by strategy: linear|parallel_stages|full_parallel|iterative|tournament|consensus\"},\"runnable_only\":{\"type\":\"boolean\",\"description\":\"Only include runnable topologies\"},\"min_agents\":{\"type\":\"integer\",\"description\":\"Minimum total agents in topology\"},\"max_agents\":{\"type\":\"integer\",\"description\":\"Maximum total agents in topology\"},\"offset\":{\"type\":\"integer\",\"description\":\"Pagination offset\"},\"limit\":{\"type\":\"integer\",\"description\":\"Pagination limit\"},\"sort_by\":{\"type\":\"string\",\"description\":\"Sort key: id|name|category|strategy|node_count|edge_count|total_agents|max_iterations|latency_mult|est_cost_1k\"},\"order\":{\"type\":\"string\",\"description\":\"asc|desc\"},\"details\":{\"type\":\"boolean\",\"description\":\"Include node list and ASCII diagram\"}}}",
        .execute = tool_topology_list
    },
    {
        .name = "topology_run",
        .description = "Execute a task through the topology runtime using a named topology or dynamic auto-selection. Use dry_run to preview the chosen execution graph without spending API calls.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"description\":\"Task to execute through the topology runtime\"},\"topology\":{\"type\":\"string\",\"description\":\"Optional explicit topology name\"},\"topology_id\":{\"type\":\"integer\",\"description\":\"Optional explicit topology ID\"},\"auto\":{\"type\":\"boolean\",\"description\":\"Auto-select a topology when true (default if topology omitted)\"},\"dry_run\":{\"type\":\"boolean\",\"description\":\"Preview the plan without executing it\"}},\"required\":[\"task\"]}",
        .execute = tool_topology_run
    },
    {
        .name = "create_swarm",
        .description = "Create a named flat group of sub-agents and dispatch multiple tasks to them simultaneously. Tasks may be strings or objects with per-agent model overrides. For graph-structured orchestration, use topology_run.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Swarm group name\"},\"tasks\":{\"type\":\"array\",\"description\":\"Array of task prompts or objects like {task,model}\",\"items\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"}},\"required\":[\"task\"]}]}},\"model\":{\"type\":\"string\",\"description\":\"Default model for all agents in this swarm\"}},\"required\":[\"name\",\"tasks\"]}",
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
    {
        .name = "plugin_validate",
        .description = "Validate plugin-manifest.json and plugins.lock schema/syntax and pin consistency.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"manifest_path\":{\"type\":\"string\",\"description\":\"Path to plugin-manifest.json\"},\"lock_path\":{\"type\":\"string\",\"description\":\"Path to plugins.lock\"}}}",
        .execute = tool_plugin_validate
    },
    /* ═══════════════════════════════════════════════════════════════════════
     * VISION — Image/document viewing
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        .name = "view_image",
        .description = "Read and encode an image file for vision analysis. Supports PNG, JPEG, GIF, WebP. The image will be included in the next API call for Claude to analyze.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to image file\"}},\"required\":[\"path\"]}",
        .execute = tool_view_image
    },
    {
        .name = "view_pdf",
        .description = "Read and encode a PDF file for analysis. The PDF will be included in the next API call for Claude to read and analyze.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to PDF file\"}},\"required\":[\"path\"]}",
        .execute = tool_view_pdf
    },
    /* ── External Service Integrations ─────────────────────────────────── */
    {
        .name = "tavily_search",
        .description = "Search the web using Tavily API with AI-generated answers and source attribution. Returns relevant results with snippets. Requires TAVILY_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"},\"max_results\":{\"type\":\"integer\",\"description\":\"Max results (default 5)\"},\"include_answer\":{\"type\":\"boolean\",\"description\":\"Include AI answer (default true)\"},\"search_depth\":{\"type\":\"string\",\"enum\":[\"basic\",\"advanced\"],\"description\":\"Search depth\"}},\"required\":[\"query\"]}",
        .execute = tool_tavily_search
    },
    {
        .name = "brave_search",
        .description = "Search the web using Brave Search API. Privacy-focused, returns web results with titles, URLs, and snippets. Requires BRAVE_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"},\"count\":{\"type\":\"integer\",\"description\":\"Number of results (default 5)\"}},\"required\":[\"query\"]}",
        .execute = tool_brave_search
    },
    {
        .name = "github_search",
        .description = "Search GitHub repositories, code, issues, or users. Requires GITHUB_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query (supports GitHub search syntax)\"},\"type\":{\"type\":\"string\",\"enum\":[\"repositories\",\"code\",\"issues\",\"users\"],\"description\":\"Search type (default: repositories)\"}},\"required\":[\"query\"]}",
        .execute = tool_github_search
    },
    {
        .name = "github_issue",
        .description = "Get GitHub issues for a repository. Without number, lists open issues. With number, gets specific issue details. Requires GITHUB_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"repo\":{\"type\":\"string\",\"description\":\"Repository (owner/name)\"},\"number\":{\"type\":\"integer\",\"description\":\"Issue number (optional, lists all if omitted)\"}},\"required\":[\"repo\"]}",
        .execute = tool_github_issue
    },
    {
        .name = "github_pr",
        .description = "Get GitHub pull requests for a repository. Without number, lists open PRs. With number, gets specific PR details. Requires GITHUB_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"repo\":{\"type\":\"string\",\"description\":\"Repository (owner/name)\"},\"number\":{\"type\":\"integer\",\"description\":\"PR number (optional)\"}},\"required\":[\"repo\"]}",
        .execute = tool_github_pr
    },
    {
        .name = "github_repo",
        .description = "Get GitHub repository information including stars, forks, language, description. Requires GITHUB_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"repo\":{\"type\":\"string\",\"description\":\"Repository (owner/name)\"}},\"required\":[\"repo\"]}",
        .execute = tool_github_repo
    },
    {
        .name = "github_create_issue",
        .description = "Create a new GitHub issue. Requires GITHUB_TOKEN with repo write access.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"repo\":{\"type\":\"string\",\"description\":\"Repository (owner/name)\"},\"title\":{\"type\":\"string\",\"description\":\"Issue title\"},\"body\":{\"type\":\"string\",\"description\":\"Issue body (markdown)\"}},\"required\":[\"repo\",\"title\"]}",
        .execute = tool_github_create_issue
    },
    /* ── Alpha Vantage: Core Stock Time Series (7+2) ────────────────────── */
    { .name = "av_intraday", .description = "Intraday OHLCV time series (1min-60min) for any equity. 20+ years history.", .input_schema_json = S_INTRA, .execute = tool_av_time_series_intraday },
    { .name = "av_daily", .description = "Daily OHLCV time series for any equity. 20+ years history.", .input_schema_json = S_SYM_OUT, .execute = tool_av_time_series_daily },
    { .name = "av_daily_adjusted", .description = "Daily adjusted OHLCV (split/dividend adjusted) for any equity.", .input_schema_json = S_SYM_OUT, .execute = tool_av_time_series_daily_adj },
    { .name = "av_weekly", .description = "Weekly OHLCV time series for any equity. 20+ years.", .input_schema_json = S_SYM, .execute = tool_av_time_series_weekly },
    { .name = "av_weekly_adjusted", .description = "Weekly adjusted OHLCV (split/dividend adjusted).", .input_schema_json = S_SYM, .execute = tool_av_time_series_weekly_adj },
    { .name = "av_monthly", .description = "Monthly OHLCV time series for any equity. 20+ years.", .input_schema_json = S_SYM, .execute = tool_av_time_series_monthly },
    { .name = "av_monthly_adjusted", .description = "Monthly adjusted OHLCV (split/dividend adjusted).", .input_schema_json = S_SYM, .execute = tool_av_time_series_monthly_adj },
    { .name = "av_quote", .description = "Latest real-time price, volume, change, change% for a ticker.", .input_schema_json = S_SYM, .execute = tool_av_quote },
    { .name = "av_bulk_quotes", .description = "Bulk real-time quotes for up to 100 US symbols in one call.", .input_schema_json = S_BULK, .execute = tool_av_bulk_quotes },
    /* ── Alpha Vantage: Search & Market Status ────────────────────────── */
    { .name = "av_search", .description = "Search for ticker symbols by name/keyword. Returns symbol, name, type, region.", .input_schema_json = S_KWD, .execute = tool_av_search },
    { .name = "av_market_status", .description = "Current open/closed status of major global exchanges.", .input_schema_json = S_NONE, .execute = tool_av_market_status },
    /* ── Alpha Vantage: Options ───────────────────────────────────────── */
    { .name = "av_realtime_options", .description = "Real-time US options chains sorted by expiration & strike. Full market coverage.", .input_schema_json = S_OPTS, .execute = tool_av_realtime_options },
    { .name = "av_historical_options", .description = "Historical options chain for a symbol on a specific date. 15+ years. Includes Greeks & IV.", .input_schema_json = S_OPTSH, .execute = tool_av_historical_options },
    /* ── Alpha Vantage: News & Sentiment ──────────────────────────────── */
    { .name = "av_news", .description = "Live & historical market news with sentiment scores. Covers stocks, crypto, forex, and topics.", .input_schema_json = S_NEWS, .execute = tool_av_news },
    /* ── Alpha Vantage: Company Fundamentals ──────────────────────────── */
    { .name = "av_overview", .description = "Company overview: sector, market cap, PE, EPS, dividend yield, 52-week range, description.", .input_schema_json = S_SYM, .execute = tool_av_overview },
    { .name = "av_etf", .description = "ETF profile: net assets, expense ratio, holdings, sector allocation.", .input_schema_json = S_SYM, .execute = tool_av_etf },
    { .name = "av_income", .description = "Income statement (annual+quarterly): revenue, gross profit, operating income, net income, EBITDA.", .input_schema_json = S_SYM, .execute = tool_av_income },
    { .name = "av_balance", .description = "Balance sheet (annual+quarterly): assets, liabilities, equity, cash, debt.", .input_schema_json = S_SYM, .execute = tool_av_balance },
    { .name = "av_cashflow", .description = "Cash flow statement (annual+quarterly): operating, investing, financing, FCF, capex.", .input_schema_json = S_SYM, .execute = tool_av_cashflow },
    { .name = "av_earnings", .description = "Earnings (annual+quarterly): reported EPS, estimated EPS, surprise, surprise%.", .input_schema_json = S_SYM, .execute = tool_av_earnings },
    { .name = "av_earnings_estimates", .description = "Analyst EPS & revenue estimates with revision history and analyst count.", .input_schema_json = S_SYM, .execute = tool_av_earnings_estimates },
    { .name = "av_dividends", .description = "Historical and declared future dividend distributions.", .input_schema_json = S_SYM, .execute = tool_av_dividends },
    { .name = "av_splits", .description = "Historical stock split events.", .input_schema_json = S_SYM, .execute = tool_av_splits },
    { .name = "av_insider", .description = "Insider transactions: buys/sells by founders, executives, board members.", .input_schema_json = S_SYM, .execute = tool_av_insider },
    { .name = "av_institutional", .description = "Institutional ownership and holdings (13F filings).", .input_schema_json = S_SYM, .execute = tool_av_institutional },
    /* ── Alpha Vantage: Earnings Call Transcript ──────────────────────── */
    { .name = "av_transcript", .description = "Earnings call transcript for a company in a specific quarter. 15+ years. LLM sentiment signals.", .input_schema_json = S_TRANS, .execute = tool_av_transcript },
    /* ── Alpha Vantage: Corporate Events & Calendar ───────────────────── */
    { .name = "av_movers", .description = "Top 20 gainers, losers, and most actively traded US tickers.", .input_schema_json = S_NONE, .execute = tool_av_movers },
    { .name = "av_listing_status", .description = "Active or delisted US stocks and ETFs. Asset lifecycle & survivorship.", .input_schema_json = S_LIST, .execute = tool_av_listing_status },
    { .name = "av_earnings_calendar", .description = "Upcoming company earnings in next 3/6/12 months.", .input_schema_json = S_ECAL, .execute = tool_av_earnings_calendar },
    { .name = "av_ipo_calendar", .description = "Upcoming IPOs expected in next 3 months.", .input_schema_json = S_NONE, .execute = tool_av_ipo_calendar },
    /* ── Alpha Vantage: Advanced Analytics ─────────────────────────────── */
    { .name = "av_analytics_fixed", .description = "Advanced analytics over fixed window: return, variance, auto-correlation, etc.", .input_schema_json = S_AFIXED, .execute = tool_av_analytics_fixed },
    { .name = "av_analytics_sliding", .description = "Moving analytics over sliding windows: rolling variance, correlation, etc.", .input_schema_json = S_ASLIDE, .execute = tool_av_analytics_sliding },
    /* ── Alpha Vantage: Forex ──────────────────────────────────────────── */
    { .name = "av_forex", .description = "Real-time forex exchange rate for any currency pair (physical or digital).", .input_schema_json = S_FX, .execute = tool_av_forex },
    { .name = "av_fx_intraday", .description = "Intraday FX OHLC (1min-60min) for a currency pair.", .input_schema_json = S_FXI, .execute = tool_av_fx_intraday },
    { .name = "av_fx_daily", .description = "Daily FX OHLC for a currency pair. 20+ years.", .input_schema_json = S_FXP, .execute = tool_av_fx_daily },
    { .name = "av_fx_weekly", .description = "Weekly FX OHLC for a currency pair.", .input_schema_json = S_FXP, .execute = tool_av_fx_weekly },
    { .name = "av_fx_monthly", .description = "Monthly FX OHLC for a currency pair.", .input_schema_json = S_FXP, .execute = tool_av_fx_monthly },
    /* ── Alpha Vantage: Crypto ─────────────────────────────────────────── */
    { .name = "av_crypto", .description = "Daily crypto OHLCV (e.g. BTC, ETH, SOL) in USD or other market.", .input_schema_json = S_CRYPTO, .execute = tool_av_crypto },
    { .name = "av_crypto_intraday", .description = "Intraday crypto OHLCV (1min-60min), real-time.", .input_schema_json = S_CRYI, .execute = tool_av_crypto_intraday },
    { .name = "av_crypto_weekly", .description = "Weekly crypto OHLCV in USD and market currency.", .input_schema_json = S_CRYPTO, .execute = tool_av_crypto_weekly },
    { .name = "av_crypto_monthly", .description = "Monthly crypto OHLCV in USD and market currency.", .input_schema_json = S_CRYPTO, .execute = tool_av_crypto_monthly },
    /* ── Alpha Vantage: Commodities ────────────────────────────────────── */
    { .name = "av_wti", .description = "WTI crude oil prices (daily/weekly/monthly).", .input_schema_json = S_INTV, .execute = tool_av_wti },
    { .name = "av_brent", .description = "Brent crude oil prices (daily/weekly/monthly).", .input_schema_json = S_INTV, .execute = tool_av_brent },
    { .name = "av_natural_gas", .description = "Henry Hub natural gas spot prices.", .input_schema_json = S_INTV, .execute = tool_av_natural_gas },
    { .name = "av_copper", .description = "Global copper prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_copper },
    { .name = "av_aluminum", .description = "Global aluminum prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_aluminum },
    { .name = "av_wheat", .description = "Global wheat prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_wheat },
    { .name = "av_corn", .description = "Global corn prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_corn },
    { .name = "av_cotton", .description = "Global cotton prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_cotton },
    { .name = "av_sugar", .description = "Global sugar prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_sugar },
    { .name = "av_coffee", .description = "Global coffee prices (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_coffee },
    { .name = "av_all_commodities", .description = "Global commodity price index (monthly/quarterly/annual).", .input_schema_json = S_INTV, .execute = tool_av_all_commodities },
    /* ── Alpha Vantage: Precious Metals ────────────────────────────────── */
    { .name = "av_gold_spot", .description = "Live spot price of gold (XAU) or silver (XAG).", .input_schema_json = S_GOLD, .execute = tool_av_gold_spot },
    { .name = "av_gold_history", .description = "Historical gold/silver prices (daily/weekly/monthly).", .input_schema_json = S_GOLDH, .execute = tool_av_gold_history },
    /* ── Alpha Vantage: Economic Indicators ────────────────────────────── */
    { .name = "av_real_gdp", .description = "US Real GDP (annual/quarterly).", .input_schema_json = S_ECON, .execute = tool_av_real_gdp },
    { .name = "av_real_gdp_per_capita", .description = "US Real GDP per capita (quarterly).", .input_schema_json = S_ECON, .execute = tool_av_real_gdp_per_capita },
    { .name = "av_treasury_yield", .description = "US treasury yield (3mo/2yr/5yr/7yr/10yr/30yr) daily/weekly/monthly.", .input_schema_json = S_TREAS, .execute = tool_av_treasury_yield },
    { .name = "av_federal_funds_rate", .description = "Federal funds rate (interest rate) daily/weekly/monthly.", .input_schema_json = S_ECON, .execute = tool_av_federal_funds_rate },
    { .name = "av_cpi", .description = "Consumer Price Index (CPI) monthly/semiannual.", .input_schema_json = S_ECON, .execute = tool_av_cpi },
    { .name = "av_inflation", .description = "Annual inflation rates (consumer prices) US.", .input_schema_json = S_NONE, .execute = tool_av_inflation },
    { .name = "av_retail_sales", .description = "Monthly advance retail sales data.", .input_schema_json = S_NONE, .execute = tool_av_retail_sales },
    { .name = "av_durables", .description = "Monthly manufacturers' new orders of durable goods.", .input_schema_json = S_NONE, .execute = tool_av_durables },
    { .name = "av_unemployment", .description = "Monthly US unemployment rate.", .input_schema_json = S_NONE, .execute = tool_av_unemployment },
    { .name = "av_nonfarm_payroll", .description = "Monthly total nonfarm payroll (jobs added/lost).", .input_schema_json = S_NONE, .execute = tool_av_nonfarm_payroll },
    /* ── Alpha Vantage: Technical Indicators — Moving Averages ─────────── */
    { .name = "av_sma", .description = "Simple Moving Average (SMA).", .input_schema_json = S_IND, .execute = tool_av_sma },
    { .name = "av_ema", .description = "Exponential Moving Average (EMA).", .input_schema_json = S_IND, .execute = tool_av_ema },
    { .name = "av_wma", .description = "Weighted Moving Average (WMA).", .input_schema_json = S_IND, .execute = tool_av_wma },
    { .name = "av_dema", .description = "Double Exponential Moving Average (DEMA).", .input_schema_json = S_IND, .execute = tool_av_dema },
    { .name = "av_tema", .description = "Triple Exponential Moving Average (TEMA).", .input_schema_json = S_IND, .execute = tool_av_tema },
    { .name = "av_trima", .description = "Triangular Moving Average (TRIMA).", .input_schema_json = S_IND, .execute = tool_av_trima },
    { .name = "av_kama", .description = "Kaufman Adaptive Moving Average (KAMA).", .input_schema_json = S_IND, .execute = tool_av_kama },
    { .name = "av_mama", .description = "MESA Adaptive Moving Average (MAMA).", .input_schema_json = S_IND_MAMA, .execute = tool_av_mama },
    { .name = "av_vwap", .description = "Volume Weighted Average Price (VWAP) — intraday only.", .input_schema_json = S_IND, .execute = tool_av_vwap },
    { .name = "av_t3", .description = "Triple Exponential Moving Average T3.", .input_schema_json = S_IND, .execute = tool_av_t3 },
    /* ── Alpha Vantage: Technical Indicators — Oscillators ─────────────── */
    { .name = "av_macd", .description = "Moving Average Convergence/Divergence (MACD).", .input_schema_json = S_IND_MACD, .execute = tool_av_macd },
    { .name = "av_macdext", .description = "MACD with controllable moving average type.", .input_schema_json = S_IND_MACDEXT, .execute = tool_av_macdext },
    { .name = "av_stoch", .description = "Stochastic Oscillator (STOCH).", .input_schema_json = S_IND_STOCH, .execute = tool_av_stoch },
    { .name = "av_stochf", .description = "Stochastic Fast (STOCHF).", .input_schema_json = S_IND_STOCHF, .execute = tool_av_stochf },
    { .name = "av_rsi", .description = "Relative Strength Index (RSI).", .input_schema_json = S_IND, .execute = tool_av_rsi },
    { .name = "av_stochrsi", .description = "Stochastic RSI (STOCHRSI).", .input_schema_json = S_IND, .execute = tool_av_stochrsi },
    { .name = "av_willr", .description = "Williams' %R (WILLR).", .input_schema_json = S_IND, .execute = tool_av_willr },
    { .name = "av_adx", .description = "Average Directional Index (ADX).", .input_schema_json = S_IND, .execute = tool_av_adx },
    { .name = "av_adxr", .description = "Average Directional Index Rating (ADXR).", .input_schema_json = S_IND, .execute = tool_av_adxr },
    { .name = "av_apo", .description = "Absolute Price Oscillator (APO).", .input_schema_json = S_IND, .execute = tool_av_apo },
    { .name = "av_ppo", .description = "Percentage Price Oscillator (PPO).", .input_schema_json = S_IND, .execute = tool_av_ppo },
    { .name = "av_mom", .description = "Momentum (MOM).", .input_schema_json = S_IND, .execute = tool_av_mom },
    { .name = "av_bop", .description = "Balance of Power (BOP).", .input_schema_json = S_IND, .execute = tool_av_bop },
    { .name = "av_cci", .description = "Commodity Channel Index (CCI).", .input_schema_json = S_IND, .execute = tool_av_cci },
    { .name = "av_cmo", .description = "Chande Momentum Oscillator (CMO).", .input_schema_json = S_IND, .execute = tool_av_cmo },
    { .name = "av_roc", .description = "Rate of Change (ROC).", .input_schema_json = S_IND, .execute = tool_av_roc },
    { .name = "av_rocr", .description = "Rate of Change Ratio (ROCR).", .input_schema_json = S_IND, .execute = tool_av_rocr },
    { .name = "av_aroon", .description = "Aroon indicator (AROON).", .input_schema_json = S_IND, .execute = tool_av_aroon },
    { .name = "av_aroonosc", .description = "Aroon Oscillator (AROONOSC).", .input_schema_json = S_IND, .execute = tool_av_aroonosc },
    { .name = "av_mfi", .description = "Money Flow Index (MFI).", .input_schema_json = S_IND, .execute = tool_av_mfi },
    { .name = "av_trix", .description = "Triple smooth EMA rate of change (TRIX).", .input_schema_json = S_IND, .execute = tool_av_trix_ind },
    { .name = "av_ultosc", .description = "Ultimate Oscillator (ULTOSC).", .input_schema_json = S_IND_ULTOSC, .execute = tool_av_ultosc },
    { .name = "av_dx", .description = "Directional Movement Index (DX).", .input_schema_json = S_IND, .execute = tool_av_dx },
    { .name = "av_minus_di", .description = "Minus Directional Indicator (-DI).", .input_schema_json = S_IND, .execute = tool_av_minus_di },
    { .name = "av_plus_di", .description = "Plus Directional Indicator (+DI).", .input_schema_json = S_IND, .execute = tool_av_plus_di },
    { .name = "av_minus_dm", .description = "Minus Directional Movement (-DM).", .input_schema_json = S_IND, .execute = tool_av_minus_dm },
    { .name = "av_plus_dm", .description = "Plus Directional Movement (+DM).", .input_schema_json = S_IND, .execute = tool_av_plus_dm },
    /* ── Alpha Vantage: Technical Indicators — Bands/Range ─────────────── */
    { .name = "av_bbands", .description = "Bollinger Bands (BBANDS).", .input_schema_json = S_IND_BB, .execute = tool_av_bbands },
    { .name = "av_midpoint", .description = "Midpoint (highest+lowest)/2.", .input_schema_json = S_IND, .execute = tool_av_midpoint },
    { .name = "av_midprice", .description = "Midpoint Price (highest high+lowest low)/2.", .input_schema_json = S_IND, .execute = tool_av_midprice },
    { .name = "av_sar", .description = "Parabolic SAR.", .input_schema_json = S_IND_SAR, .execute = tool_av_sar },
    { .name = "av_trange", .description = "True Range (TRANGE).", .input_schema_json = S_IND, .execute = tool_av_trange },
    { .name = "av_atr", .description = "Average True Range (ATR).", .input_schema_json = S_IND, .execute = tool_av_atr },
    { .name = "av_natr", .description = "Normalized Average True Range (NATR).", .input_schema_json = S_IND, .execute = tool_av_natr },
    /* ── Alpha Vantage: Technical Indicators — Volume ──────────────────── */
    { .name = "av_ad", .description = "Chaikin A/D Line.", .input_schema_json = S_IND, .execute = tool_av_ad_line },
    { .name = "av_adosc", .description = "Chaikin A/D Oscillator.", .input_schema_json = S_IND_ADOSC, .execute = tool_av_adosc },
    { .name = "av_obv", .description = "On Balance Volume (OBV).", .input_schema_json = S_IND, .execute = tool_av_obv },
    /* ── Alpha Vantage: Technical Indicators — Hilbert Transform ───────── */
    { .name = "av_ht_trendline", .description = "Hilbert Transform Instantaneous Trendline.", .input_schema_json = S_IND, .execute = tool_av_ht_trendline },
    { .name = "av_ht_sine", .description = "Hilbert Transform Sine Wave.", .input_schema_json = S_IND, .execute = tool_av_ht_sine },
    { .name = "av_ht_trendmode", .description = "Hilbert Transform Trend vs Cycle Mode.", .input_schema_json = S_IND, .execute = tool_av_ht_trendmode },
    { .name = "av_ht_dcperiod", .description = "Hilbert Transform Dominant Cycle Period.", .input_schema_json = S_IND, .execute = tool_av_ht_dcperiod },
    { .name = "av_ht_dcphase", .description = "Hilbert Transform Dominant Cycle Phase.", .input_schema_json = S_IND, .execute = tool_av_ht_dcphase },
    { .name = "av_ht_phasor", .description = "Hilbert Transform Phasor Components.", .input_schema_json = S_IND, .execute = tool_av_ht_phasor },
    {
        .name = "fred_series",
        .description = "Get economic data from FRED (Federal Reserve). Series IDs: GDP, UNRATE (unemployment), CPIAUCSL (CPI), DFF (fed funds rate), T10Y2Y (yield curve), VIXCLS (VIX), DGS10 (10yr treasury), MORTGAGE30US. Requires FRED_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"series_id\":{\"type\":\"string\",\"description\":\"FRED series ID (e.g. GDP, UNRATE, CPIAUCSL)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Number of observations (default 30)\"},\"sort_order\":{\"type\":\"string\",\"enum\":[\"asc\",\"desc\"],\"description\":\"Sort order (default desc = most recent first)\"}},\"required\":[\"series_id\"]}",
        .execute = tool_fred_series
    },
    {
        .name = "slack_post",
        .description = "Post a message to a Slack channel. Requires SLACK_BOT_TOKEN with chat:write scope.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\",\"description\":\"Channel ID or name (e.g. #general, C1234567890)\"},\"text\":{\"type\":\"string\",\"description\":\"Message text (supports Slack markdown)\"}},\"required\":[\"channel\",\"text\"]}",
        .execute = tool_slack_post
    },
    {
        .name = "notion_search",
        .description = "Search across all Notion pages and databases accessible to the integration. Requires NOTION_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query (optional, empty returns recent pages)\"}}}",
        .execute = tool_notion_search
    },
    {
        .name = "notion_page",
        .description = "Read the content blocks of a Notion page by its ID. Requires NOTION_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"page_id\":{\"type\":\"string\",\"description\":\"Notion page or block ID (UUID format)\"}},\"required\":[\"page_id\"]}",
        .execute = tool_notion_page
    },
    {
        .name = "weather",
        .description = "Get current weather for any location worldwide. Returns temperature, conditions, humidity, wind. Requires OPENWEATHERMAP_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"location\":{\"type\":\"string\",\"description\":\"City name, optionally with country code (e.g. 'London,UK', 'New York')\"},\"units\":{\"type\":\"string\",\"enum\":[\"metric\",\"imperial\",\"standard\"],\"description\":\"Units (default: metric)\"}},\"required\":[\"location\"]}",
        .execute = tool_weather
    },
    {
        .name = "firecrawl",
        .description = "Scrape a web page and extract structured content as markdown. Better than raw HTTP for complex pages with JS rendering. Requires FIRECRAWL_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to scrape\"},\"formats\":{\"type\":\"string\",\"description\":\"Output format: markdown (default), html, rawHtml, links, screenshot\"}},\"required\":[\"url\"]}",
        .execute = tool_firecrawl
    },
    {
        .name = "jina_read",
        .description = "Extract readable content from any URL using Jina AI Reader. Returns clean markdown text, stripping navigation and ads. Optionally uses JINA_API_KEY for higher limits.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to read\"}},\"required\":[\"url\"]}",
        .execute = tool_jina_read
    },
    {
        .name = "serpapi",
        .description = "Search Google via SerpAPI. Returns organic results with titles, snippets, links. Requires SERPAPI_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}",
        .execute = tool_serpapi
    },
    {
        .name = "discord_post",
        .description = "Post a message to Discord via webhook URL or bot token. For webhooks, provide webhook_url. For bot mode, provide channel_id and set DISCORD_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Message content\"},\"webhook_url\":{\"type\":\"string\",\"description\":\"Discord webhook URL (easiest method)\"},\"channel_id\":{\"type\":\"string\",\"description\":\"Channel ID for bot mode\"}},\"required\":[\"text\"]}",
        .execute = tool_discord_post
    },
    {
        .name = "twilio_sms",
        .description = "Send an SMS via Twilio. Requires TWILIO_AUTH_TOKEN, TWILIO_ACCOUNT_SID, TWILIO_FROM_NUMBER.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\",\"description\":\"Recipient phone number (+1xxxyyyzzzz)\"},\"body\":{\"type\":\"string\",\"description\":\"SMS message text\"}},\"required\":[\"to\",\"body\"]}",
        .execute = tool_twilio_sms
    },
    {
        .name = "elevenlabs_tts",
        .description = "Convert text to speech audio using ElevenLabs. Saves MP3 file. Requires ELEVENLABS_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to speak\"},\"voice_id\":{\"type\":\"string\",\"description\":\"Voice ID (default: Rachel)\"},\"output\":{\"type\":\"string\",\"description\":\"Output file path (default: /tmp/dsco_tts.mp3)\"}},\"required\":[\"text\"]}",
        .execute = tool_elevenlabs_tts
    },
    {
        .name = "pinecone_query",
        .description = "Query a Pinecone vector database index. Returns top-k nearest neighbors with metadata. Requires PINECONE_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\",\"description\":\"Pinecone index host (e.g. my-index-abc123.svc.pinecone.io)\"},\"top_k\":{\"type\":\"integer\",\"description\":\"Number of results (default 5)\"}},\"required\":[\"host\"]}",
        .execute = tool_pinecone_query
    },
    {
        .name = "stripe",
        .description = "Access Stripe payment data — charges, customers, balance, invoices. Requires STRIPE_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list_charges\",\"list_customers\",\"get_balance\",\"list_invoices\"],\"description\":\"Action to perform\"}},\"required\":[\"action\"]}",
        .execute = tool_stripe
    },
    {
        .name = "supabase_query",
        .description = "Query a Supabase table using PostgREST. Requires SUPABASE_API_KEY and SUPABASE_URL.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\",\"description\":\"Table name\"},\"select\":{\"type\":\"string\",\"description\":\"Columns to select (default: *)\"},\"filter\":{\"type\":\"string\",\"description\":\"PostgREST filter (e.g. name=eq.John)\"}},\"required\":[\"table\"]}",
        .execute = tool_supabase_query
    },
    {
        .name = "huggingface",
        .description = "Run inference on a Hugging Face model. Supports text classification, generation, NER, summarization. Requires HF_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"model\":{\"type\":\"string\",\"description\":\"Model ID (e.g. facebook/bart-large-mnli, gpt2, dslim/bert-base-NER)\"},\"text\":{\"type\":\"string\",\"description\":\"Input text\"}},\"required\":[\"model\",\"text\"]}",
        .execute = tool_huggingface
    },
    {
        .name = "github_actions",
        .description = "View GitHub Actions workflow runs and workflows for a repository. Requires GITHUB_TOKEN.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"repo\":{\"type\":\"string\",\"description\":\"Repository (owner/name)\"},\"action\":{\"type\":\"string\",\"enum\":[\"list_runs\",\"list_workflows\"],\"description\":\"Action (default: list_runs)\"}},\"required\":[\"repo\"]}",
        .execute = tool_github_actions
    },
    {
        .name = "mapbox_geocode",
        .description = "Geocode an address or place name to coordinates using Mapbox. Requires MAPBOX_API_KEY.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Place name or address\"}},\"required\":[\"query\"]}",
        .execute = tool_mapbox_geocode
    },
    {
        .name = "csv_parse",
        .description = "Parse CSV text or file. Returns JSON array of rows. Supports column extraction, custom delimiters, header detection.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"CSV text\"},\"file\":{\"type\":\"string\",\"description\":\"CSV file path\"},\"column\":{\"type\":\"integer\",\"description\":\"Extract specific column (0-based)\"},\"delimiter\":{\"type\":\"string\",\"description\":\"Delimiter (default comma)\"},\"headers\":{\"type\":\"boolean\",\"description\":\"First row is headers (default true)\"}}}",
        .execute = tool_csv_parse
    },
    {
        .name = "regex_match",
        .description = "Match a regex pattern against text. Returns all matches. Supports extended POSIX regex.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Input text\"},\"pattern\":{\"type\":\"string\",\"description\":\"POSIX extended regex pattern\"},\"global\":{\"type\":\"boolean\",\"description\":\"Return all matches (default false)\"}},\"required\":[\"text\",\"pattern\"]}",
        .execute = tool_regex_match
    },
    {
        .name = "url_parse",
        .description = "Parse a URL into scheme, host, port, path, query, fragment components.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to parse\"}},\"required\":[\"url\"]}",
        .execute = tool_url_parse
    },
    {
        .name = "semver_compare",
        .description = "Compare two semantic versions (e.g. 1.2.3 vs 2.0.0). Returns -1, 0, or 1.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"version_a\":{\"type\":\"string\",\"description\":\"First version\"},\"version_b\":{\"type\":\"string\",\"description\":\"Second version\"}},\"required\":[\"version_a\",\"version_b\"]}",
        .execute = tool_semver
    },
    {
        .name = "cron_parse",
        .description = "Parse a cron expression (5-field) and return structured fields with a human-readable description.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Cron expression e.g. '0 9 * * 1'\"}},\"required\":[\"expression\"]}",
        .execute = tool_cron_parse
    },
    {
        .name = "template_render",
        .description = "Render a Mustache-style template with {{variable}} substitution from a JSON variables object.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"template\":{\"type\":\"string\",\"description\":\"Template with {{var}} placeholders\"},\"variables\":{\"type\":\"string\",\"description\":\"JSON object of variable values\"}},\"required\":[\"template\"]}",
        .execute = tool_template_render
    },
    {
        .name = "text_diff",
        .description = "Produce a unified diff between two text strings.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text_a\":{\"type\":\"string\",\"description\":\"First text\"},\"text_b\":{\"type\":\"string\",\"description\":\"Second text\"}},\"required\":[\"text_a\",\"text_b\"]}",
        .execute = tool_text_diff
    },
    {
        .name = "process_tree",
        .description = "Show process list with PID, PPID, user, CPU%, MEM%, command. Optionally filter by name.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\",\"description\":\"Optional process name filter\"}}}",
        .execute = tool_process_tree
    },
    {
        .name = "system_profiler",
        .description = "System profiling: CPU, memory, disk, network, load. Specify section (cpu/disk/network/load/all).",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"description\":\"Section: cpu, disk, network, load, or all (default)\"}}}",
        .execute = tool_system_profiler
    },
    {
        .name = "string_ops",
        .description = "String operations: upper, lower, trim, reverse, length, base64_encode, word_count.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\",\"description\":\"Operation: upper|lower|trim|reverse|length|base64_encode|word_count\"},\"text\":{\"type\":\"string\",\"description\":\"Input text\"}},\"required\":[\"op\",\"text\"]}",
        .execute = tool_string_ops
    },
    {
        .name = "xml_extract",
        .description = "Extract content or attributes from XML/HTML tags. Lightweight tag-based extraction without a full parser.",
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"XML/HTML text\"},\"file\":{\"type\":\"string\",\"description\":\"XML/HTML file path\"},\"tag\":{\"type\":\"string\",\"description\":\"Tag name to extract\"},\"attribute\":{\"type\":\"string\",\"description\":\"Attribute name to extract (omit for inner text)\"}},\"required\":[\"tag\"]}",
        .execute = tool_xml_extract
    },
};

static const int s_tool_count = sizeof(s_tools) / sizeof(s_tools[0]);

/* Forward declarations for hash map */
static unsigned tool_name_hash(const char *s);
static void tool_map_rebuild(void);

void tools_init(void) {
    plugin_init(&g_plugins);
    ctx_store_reset();
    browser_profiles_load();
    if (!g_browser_hosts_atexit_registered) {
        atexit(browser_profiles_atexit_flush);
        g_browser_hosts_atexit_registered = true;
    }

    /* Initialize IPC early so DSCO_IPC_DB is set before any child spawns */
    ipc_init(NULL, NULL);
    const char *depth_s = getenv("DSCO_SWARM_DEPTH");
    const char *parent = getenv("DSCO_PARENT_INSTANCE_ID");
    int depth = depth_s ? atoi(depth_s) : 0;
    ipc_register(parent, depth, getenv("DSCO_SUBAGENT") ? "worker" : "root", "*");

    /* Build hash map for O(1) tool lookup */
    extern void tool_map_rebuild(void);  /* defined below */
    tool_map_rebuild();
}

const tool_def_t *tools_get_all(int *count) {
    *count = s_tool_count + g_plugins.extra_tool_count;
    return s_tools;
}

int tools_builtin_count(void) {
    return s_tool_count;
}

/* ── Tool hash map for O(1) lookup ─────────────────────────────────────── */

tool_map_t g_tool_map = {0};

static unsigned tool_name_hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

void tool_map_init(tool_map_t *m) {
    memset(m, 0, sizeof(*m));
}

void tool_map_free(tool_map_t *m) {
    for (int i = 0; i < TOOL_MAP_BUCKETS; i++) {
        tool_map_entry_t *e = m->buckets[i];
        while (e) {
            tool_map_entry_t *next = e->next;
            free(e);
            e = next;
        }
        m->buckets[i] = NULL;
    }
    m->count = 0;
}

void tool_map_insert(tool_map_t *m, const char *name, int index) {
    unsigned h = tool_name_hash(name) % TOOL_MAP_BUCKETS;
    tool_map_entry_t *e = malloc(sizeof(tool_map_entry_t));
    if (!e) return;
    e->name = name;
    e->index = index;
    e->next = m->buckets[h];
    m->buckets[h] = e;
    m->count++;
}

int tool_map_lookup(tool_map_t *m, const char *name) {
    unsigned h = tool_name_hash(name) % TOOL_MAP_BUCKETS;
    tool_map_entry_t *e = m->buckets[h];
    while (e) {
        if (strcmp(e->name, name) == 0) return e->index;
        e = e->next;
    }
    return -1;
}

/* Build the hash map from all registered tools */
void tool_map_rebuild(void) {
    tool_map_free(&g_tool_map);
    tool_map_init(&g_tool_map);

    /* Builtin tools */
    for (int i = 0; i < s_tool_count; i++) {
        tool_map_insert(&g_tool_map, s_tools[i].name, i);
    }
    /* Plugin tools — index as -(i+1) to distinguish from builtin */
    for (int i = 0; i < g_plugins.extra_tool_count; i++) {
        tool_map_insert(&g_tool_map, g_plugins.extra_tools[i].name, -(i + 1));
    }
    /* External tools (MCP) — index as -(10000+i) */
    for (int i = 0; i < g_external_tool_count; i++) {
        tool_map_insert(&g_tool_map, g_external_tools[i].name, -(10000 + i));
    }
}

/* ── External tool registry (MCP, etc.) ────────────────────────────────── */

external_tool_t g_external_tools[MAX_EXTERNAL_TOOLS];
int             g_external_tool_count = 0;

void tools_register_external(const char *name, const char *description,
                                const char *input_schema_json,
                                external_tool_cb cb, void *ctx) {
    if (g_external_tool_count >= MAX_EXTERNAL_TOOLS) return;
    external_tool_t *t = &g_external_tools[g_external_tool_count++];
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->description, sizeof(t->description), "%s", description);
    t->input_schema_json = safe_strdup(input_schema_json);
    t->cb = cb;
    t->ctx = ctx;

    /* Update hash map */
    tool_map_insert(&g_tool_map, t->name, -(10000 + g_external_tool_count - 1));
}

static bool name_in_list(const char *name, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(name, list[i]) == 0) return true;
    }
    return false;
}

static void sandbox_policy_for_tier(const char *tier,
                                    bool *network,
                                    const char **filesystem) {
    bool out_network = false;
    const char *out_fs = "workspace_rw";
    if (tier && tier[0] && strcasecmp(tier, "trusted") == 0) {
        out_network = true;
        out_fs = "workspace_rw";
    } else if (tier && tier[0] && strcasecmp(tier, "untrusted") == 0) {
        out_network = false;
        out_fs = "workspace_ro";
    }
    if (network) *network = out_network;
    if (filesystem) *filesystem = out_fs;
}

static bool tool_is_untrusted_sandbox_routed(const char *name) {
    return name &&
           (strcmp(name, "bash") == 0 ||
            strcmp(name, "run_command") == 0 ||
            strcmp(name, "python") == 0 ||
            strcmp(name, "node") == 0);
}

static bool json_has_key_raw(const char *json, const char *key) {
    char *raw = json_get_raw(json ? json : "{}", key);
    if (!raw) return false;
    free(raw);
    return true;
}

static char *build_sandbox_input_for_tier(const char *input_json, const char *tier,
                                          char *reason, size_t reason_len) {
    const char *json = input_json ? input_json : "{}";
    char *command = json_get_str(json, "command");
    if (!command) return NULL;

    char *image = json_get_str(json, "image");
    int timeout = json_get_int(json, "timeout", 60);
    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;

    bool network = false;
    const char *filesystem_default = "workspace_rw";
    sandbox_policy_for_tier(tier, &network, &filesystem_default);
    if (json_has_key_raw(json, "network")) {
        network = json_get_bool(json, "network", network);
    }
    char *filesystem = json_get_str(json, "filesystem");
    if (!filesystem) filesystem = safe_strdup(filesystem_default);
    if (strcmp(filesystem, "workspace_rw") != 0 &&
        strcmp(filesystem, "workspace_ro") != 0) {
        if (reason && reason_len > 0) {
            snprintf(reason, reason_len,
                     "invalid sandbox filesystem '%s' (expected workspace_rw/workspace_ro)",
                     filesystem);
        }
        free(command);
        free(image);
        free(filesystem);
        return NULL;
    }

    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"command\":");
    jbuf_append_json_str(&b, command);
    jbuf_append(&b, ",\"timeout\":");
    jbuf_append_int(&b, timeout);
    jbuf_append(&b, ",\"network\":");
    jbuf_append(&b, network ? "true" : "false");
    jbuf_append(&b, ",\"filesystem\":");
    jbuf_append_json_str(&b, filesystem);
    if (image && *image) {
        jbuf_append(&b, ",\"image\":");
        jbuf_append_json_str(&b, image);
    }
    jbuf_append(&b, "}");

    char *out = safe_strdup(b.data ? b.data : "{}");
    jbuf_free(&b);
    free(command);
    free(image);
    free(filesystem);
    return out;
}

static char *build_untrusted_routed_sandbox_input(const char *tool_name,
                                                  const char *input_json,
                                                  const char *tier,
                                                  char *reason,
                                                  size_t reason_len) {
    const char *json = input_json ? input_json : "{}";
    char *command = NULL;
    char *image = NULL;
    int timeout = 120;

    if (strcmp(tool_name, "bash") == 0) {
        char *cmd = json_get_str(json, "command");
        char *cwd = json_get_str(json, "cwd");
        timeout = json_get_int(json, "timeout", 120);
        if (!cmd) {
            if (reason && reason_len > 0) snprintf(reason, reason_len, "error: command required");
            free(cwd);
            return NULL;
        }
        if (cwd && *cwd) {
            char *esc_cwd = shell_escape(cwd);
            size_t need = strlen(cmd) + strlen(esc_cwd) + 16;
            command = safe_malloc(need);
            snprintf(command, need, "cd '%s' && %s", esc_cwd, cmd);
            free(esc_cwd);
        } else {
            command = safe_strdup(cmd);
        }
        image = safe_strdup("alpine:3.20");
        free(cmd);
        free(cwd);
    } else if (strcmp(tool_name, "run_command") == 0) {
        char *cmd = json_get_str(json, "command");
        timeout = json_get_int(json, "timeout", 30);
        if (!cmd) {
            if (reason && reason_len > 0) snprintf(reason, reason_len, "error: command required");
            return NULL;
        }
        command = safe_strdup(cmd);
        image = safe_strdup("alpine:3.20");
        free(cmd);
    } else if (strcmp(tool_name, "python") == 0) {
        char *file = json_get_str(json, "file");
        char *code = json_get_str(json, "code");
        if (file && *file) {
            char *esc = shell_escape(file);
            size_t need = strlen(esc) + 32;
            command = safe_malloc(need);
            snprintf(command, need, "python3 '%s'", esc);
            free(esc);
        } else if (code && *code) {
            jbuf_t cmd;
            jbuf_init(&cmd, strlen(code) + 64);
            jbuf_append(&cmd, "python3 - <<'PY'\n");
            jbuf_append(&cmd, code);
            jbuf_append(&cmd, "\nPY");
            command = safe_strdup(cmd.data);
            jbuf_free(&cmd);
        } else {
            if (reason && reason_len > 0) snprintf(reason, reason_len, "error: code or file required");
            free(file);
            free(code);
            return NULL;
        }
        image = safe_strdup("python:3.12-alpine");
        timeout = 120;
        free(file);
        free(code);
    } else if (strcmp(tool_name, "node") == 0) {
        char *file = json_get_str(json, "file");
        char *code = json_get_str(json, "code");
        if (file && *file) {
            char *esc = shell_escape(file);
            size_t need = strlen(esc) + 24;
            command = safe_malloc(need);
            snprintf(command, need, "node '%s'", esc);
            free(esc);
        } else if (code && *code) {
            jbuf_t cmd;
            jbuf_init(&cmd, strlen(code) + 64);
            jbuf_append(&cmd, "node - <<'JS'\n");
            jbuf_append(&cmd, code);
            jbuf_append(&cmd, "\nJS");
            command = safe_strdup(cmd.data);
            jbuf_free(&cmd);
        } else {
            if (reason && reason_len > 0) snprintf(reason, reason_len, "error: code or file required");
            free(file);
            free(code);
            return NULL;
        }
        image = safe_strdup("node:20-alpine");
        timeout = 120;
        free(file);
        free(code);
    } else {
        if (reason && reason_len > 0) {
            snprintf(reason, reason_len, "tool '%s' is not routable to sandbox", tool_name);
        }
        return NULL;
    }

    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;

    bool network = false;
    const char *filesystem = "workspace_ro";
    sandbox_policy_for_tier(tier, &network, &filesystem);

    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"command\":");
    jbuf_append_json_str(&b, command);
    jbuf_append(&b, ",\"timeout\":");
    jbuf_append_int(&b, timeout);
    jbuf_append(&b, ",\"network\":");
    jbuf_append(&b, network ? "true" : "false");
    jbuf_append(&b, ",\"filesystem\":");
    jbuf_append_json_str(&b, filesystem);
    if (image && *image) {
        jbuf_append(&b, ",\"image\":");
        jbuf_append_json_str(&b, image);
    }
    jbuf_append(&b, "}");

    char *out = safe_strdup(b.data ? b.data : "{}");
    jbuf_free(&b);
    free(command);
    free(image);
    return out;
}

bool tools_is_allowed_for_tier(const char *name, const char *tier,
                               char *reason, size_t reason_len) {
    if (!name || !name[0]) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "invalid tool name");
        return false;
    }

    if (!tier || !tier[0] || strcasecmp(tier, "standard") == 0) {
        static const char *const blocked_standard[] = {
            "ssh_command", "scp", "docker", "docker_compose",
            "git_push", "git_clone", "run_background", "crontab",
            "env_set", "plugin_load", "plugin_reload",
            "kill_process", "port_scan", "chmod",
            NULL
        };
        if (name_in_list(name, blocked_standard)) {
            if (reason && reason_len > 0) {
                snprintf(reason, reason_len,
                         "tool '%s' is blocked in standard tier", name);
            }
            return false;
        }
        return true;
    }

    if (strcasecmp(tier, "trusted") == 0) return true;

    if (strcasecmp(tier, "untrusted") == 0) {
        static const char *const blocked_untrusted[] = {
            /* command/process execution */
            "compile", "run_background", "pkg", "pip", "npm",
            "ssh_command", "scp", "docker", "docker_compose",
            "kill_process", "crontab",
            /* filesystem mutation */
            "write_file", "edit_file", "append_file",
            "move_file", "copy_file", "delete_file",
            "mkdir", "chmod", "symlink", "patch",
            "soul_append", "soul_write", "soul_replace",
            "download_file", "upload_file", "tar", "zip", "xattr",
            /* git/database mutation surfaces */
            "git_add", "git_commit", "git_stash", "git_push",
            "git_pull", "git_clone", "sqlite", "psql",
            /* network/raw request surfaces */
            "http_request", "json_api", "curl_raw", "port_scan",
            "websocket_test",
            /* orchestration/control-plane mutation */
            "spawn_agent", "agent_kill", "create_swarm", "ipc_send",
            "ipc_recv", "ipc_scratch_put", "ipc_task_submit",
            "ipc_set_role", "env_set", "plugin_load", "plugin_reload",
            NULL
        };

        if (name_in_list(name, blocked_untrusted) || strncmp(name, "ipc_", 4) == 0) {
            if (reason && reason_len > 0) {
                snprintf(reason, reason_len,
                         "tool '%s' is blocked in untrusted tier", name);
            }
            return false;
        }

        int idx = tool_map_lookup(&g_tool_map, name);
        if (idx < 0) {
            if (reason && reason_len > 0) {
                snprintf(reason, reason_len,
                         "external/plugin tool '%s' is blocked in untrusted tier", name);
            }
            return false;
        }
        return true;
    }

    /* Unknown tiers fail closed. */
    if (reason && reason_len > 0) {
        snprintf(reason, reason_len, "unknown trust tier '%s'", tier);
    }
    return false;
}

/* ── Tool execution with hash map ──────────────────────────────────────── */

static bool tools_execute_internal(const char *name, const char *input_json,
                                   char *result, size_t result_len) {
    TRACE_INFO("tool_call name=%s", name ? name : "(null)");
    TRACE_DEBUG("tool_input name=%s input=%.*s", name ? name : "(null)",
                (int)(input_json ? (strlen(input_json) < 512 ? strlen(input_json) : 512) : 0),
                input_json ? input_json : "");

    /* O(1) hash map lookup */
    int idx = tool_map_lookup(&g_tool_map, name);

    if (idx >= 0 && idx < s_tool_count) {
        /* Builtin tool */
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        bool ok = s_tools[idx].execute(input_json, result, result_len);
        gettimeofday(&t1, NULL);
        long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
        sanitize_tool_result_inplace(result);
        TRACE_INFO("tool_result name=%s ok=%d elapsed_us=%ld", name, ok, elapsed_us);
        ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
        return ok;
    }

    if (idx < 0 && idx > -10000) {
        /* Plugin tool: index is -(i+1) */
        int pi = -(idx + 1);
        if (pi < g_plugins.extra_tool_count) {
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            bool ok = g_plugins.extra_tools[pi].execute(input_json, result, result_len);
            gettimeofday(&t1, NULL);
            long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
            sanitize_tool_result_inplace(result);
            TRACE_INFO("tool_result name=%s source=plugin ok=%d elapsed_us=%ld", name, ok, elapsed_us);
            ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
            return ok;
        }
    }

    if (idx <= -10000) {
        /* External tool (MCP): index is -(10000+i) */
        int ei = -(idx + 10000);
        if (ei >= 0 && ei < g_external_tool_count && g_external_tools[ei].cb) {
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            char *ext_result = g_external_tools[ei].cb(name, input_json,
                                                         g_external_tools[ei].ctx);
            gettimeofday(&t1, NULL);
            long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
            if (ext_result) {
                snprintf(result, result_len, "%s", ext_result);
                free(ext_result);
                sanitize_tool_result_inplace(result);
                TRACE_INFO("tool_result name=%s source=mcp ok=1 elapsed_us=%ld", name, elapsed_us);
                return true;
            }
            snprintf(result, result_len, "external tool '%s' returned no result", name);
            sanitize_tool_result_inplace(result);
            TRACE_WARN("tool_result name=%s source=mcp ok=0 elapsed_us=%ld", name, elapsed_us);
            return false;
        }
    }

    /* Fallback: linear scan (shouldn't normally reach here if map is correct) */
    TRACE_DEBUG("tool_fallback name=%s using linear scan", name);
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            bool ok = s_tools[i].execute(input_json, result, result_len);
            gettimeofday(&t1, NULL);
            long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
            sanitize_tool_result_inplace(result);
            TRACE_INFO("tool_result name=%s ok=%d elapsed_us=%ld (fallback)", name, ok, elapsed_us);
            ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
            return ok;
        }
    }
    for (int i = 0; i < g_plugins.extra_tool_count; i++) {
        if (strcmp(g_plugins.extra_tools[i].name, name) == 0) {
            bool ok = g_plugins.extra_tools[i].execute(input_json, result, result_len);
            sanitize_tool_result_inplace(result);
            TRACE_INFO("tool_result name=%s source=plugin ok=%d (fallback)", name, ok);
            ctx_maybe_offload_tool_result(name, input_json, ok, result, result_len);
            return ok;
        }
    }

    TRACE_ERROR("tool_unknown name=%s", name);
    snprintf(result, result_len, "unknown tool: %s", name);
    DSCO_SET_ERR(DSCO_ERR_TOOL, "unknown tool: %s", name);
    return false;
}

bool tools_execute(const char *name, const char *input_json,
                   char *result, size_t result_len) {
    return tools_execute_internal(name, input_json, result, result_len);
}

bool tools_execute_for_tier(const char *name, const char *input_json,
                            const char *tier,
                            char *result, size_t result_len) {
    const char *dispatch_name = name;
    const char *dispatch_input = input_json;
    char *owned_input = NULL;
    char route_reason[256];
    route_reason[0] = '\0';

    if (name && strcmp(name, "sandbox_run") == 0) {
        owned_input = build_sandbox_input_for_tier(input_json, tier,
                                                   route_reason, sizeof(route_reason));
        if (!owned_input) {
            snprintf(result, result_len, "%s",
                     route_reason[0] ? route_reason : "error: command required");
            return false;
        }
        dispatch_input = owned_input;
    } else if (tier && strcasecmp(tier, "untrusted") == 0 &&
               tool_is_untrusted_sandbox_routed(name)) {
        owned_input = build_untrusted_routed_sandbox_input(name, input_json, tier,
                                                           route_reason, sizeof(route_reason));
        if (!owned_input) {
            snprintf(result, result_len, "%s",
                     route_reason[0] ? route_reason : "sandbox routing failed");
            return false;
        }
        dispatch_name = "sandbox_run";
        dispatch_input = owned_input;
        baseline_log("security", "sandbox_route", name, NULL);
    }

    bool ok = tools_execute_internal(dispatch_name, dispatch_input, result, result_len);
    free(owned_input);
    return ok;
}

/* ── Concurrency locks ────────────────────────────────────────────────── */

dsco_locks_t g_locks;

void dsco_locks_init(dsco_locks_t *l) {
    pthread_rwlock_init(&l->ctx_lock, NULL);
    pthread_rwlock_init(&l->mcp_lock, NULL);
    pthread_rwlock_init(&l->provider_lock, NULL);
    pthread_rwlock_init(&l->toolmap_lock, NULL);
    pthread_mutex_init(&l->metrics_lock, NULL);
    pthread_mutex_init(&l->cache_lock, NULL);
    pthread_mutex_init(&l->budget_lock, NULL);
    pthread_mutex_init(&l->swarm_lock, NULL);
}

void dsco_locks_destroy(dsco_locks_t *l) {
    pthread_rwlock_destroy(&l->ctx_lock);
    pthread_rwlock_destroy(&l->mcp_lock);
    pthread_rwlock_destroy(&l->provider_lock);
    pthread_rwlock_destroy(&l->toolmap_lock);
    pthread_mutex_destroy(&l->metrics_lock);
    pthread_mutex_destroy(&l->cache_lock);
    pthread_mutex_destroy(&l->budget_lock);
    pthread_mutex_destroy(&l->swarm_lock);
}

/* ── Tool execution watchdog ──────────────────────────────────────────── */

_Thread_local volatile int tl_tool_cancelled = 0;

static double watchdog_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Shared flag between watchdog and main thread (not thread-local).
   Checked in run_cmd_ex poll loop alongside g_interrupted. */
volatile int g_tool_timed_out = 0;

static void *watchdog_thread(void *arg) {
    tool_watchdog_t *wd = (tool_watchdog_t *)arg;
    while (!wd->cancelled) {
        usleep(500000); /* poll every 500ms */
        if (wd->cancelled) break;

        double now = watchdog_now();
        if (now >= wd->deadline && !wd->timed_out) {
            wd->timed_out = 1;
            g_tool_timed_out = 1;
            fprintf(stderr, "  \033[33m\xe2\x8f\xb1 timeout: %s exceeded %ds\033[0m\n",
                    wd->tool_name, wd->timeout_s);
        }
        if (now >= wd->grace_end && wd->timed_out) {
            /* Grace period exhausted. Set g_interrupted as last resort
               to break subprocess poll loops. The agent loop will check
               wd->timed_out and clear g_interrupted to continue. */
            g_interrupted = 1;
            break;
        }
    }
    return NULL;
}

void watchdog_start(tool_watchdog_t *wd, pthread_t target,
                    const char *name, int timeout_s) {
    memset(wd, 0, sizeof(*wd));
    wd->target = target;
    wd->timeout_s = timeout_s;
    snprintf(wd->tool_name, sizeof(wd->tool_name), "%s", name);

    double now = watchdog_now();
    wd->deadline = now + timeout_s;
    wd->grace_end = wd->deadline + TOOL_GRACE_PERIOD_S;
    wd->cancelled = 0;
    wd->timed_out = 0;

    pthread_create(&wd->thread, NULL, watchdog_thread, wd);
}

void watchdog_stop(tool_watchdog_t *wd) {
    wd->cancelled = 1;
    pthread_join(wd->thread, NULL);
}

/* Per-tool timeout overrides (tools that naturally take longer) */
static const tool_timeout_cfg_t s_timeout_overrides[] = {
    { "bash",           120 },
    { "run_command",    120 },
    { "sandbox_run",    120 },
    { "python",         120 },
    { "node",           120 },
    { "compile",         60 },
    { "spawn_agent",    300 },
    { "agent_wait",    3660 },
    { "create_swarm",   300 },
    { "swarm_collect", 3660 },
    { "http_request",    60 },
    { "curl",            60 },
    { "market_quote",    30 },
    { "topology_run",   1800 },
    { NULL, 0 }
};

int tool_timeout_for(const char *name) {
    /* 1. Per-tool env override: DSCO_TOOL_TIMEOUT_BASH, etc. */
    char env_key[128];
    snprintf(env_key, sizeof(env_key), "DSCO_TOOL_TIMEOUT_%s", name);
    for (char *p = env_key + 19; *p; p++) *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    const char *env_val = getenv(env_key);
    if (env_val && env_val[0]) {
        int v = atoi(env_val);
        if (v > 0 && v <= 7200) return v;
    }
    /* 2. Static table */
    for (int i = 0; s_timeout_overrides[i].name; i++) {
        if (strcmp(s_timeout_overrides[i].name, name) == 0)
            return s_timeout_overrides[i].timeout_s;
    }
    /* 3. Global default override */
    const char *g = getenv("DSCO_TOOL_DEFAULT_TIMEOUT");
    if (g && g[0]) {
        int v = atoi(g);
        if (v > 0 && v <= 7200) return v;
    }
    return TOOL_DEFAULT_TIMEOUT_S;
}

/* ── JSON schema validation before tool dispatch ──────────────────────── */

bool tools_validate_input(const char *name, const char *input_json,
                          char *error_buf, size_t error_len) {
    if (!input_json || !name) return true; /* no input to validate */

    /* Find the tool's schema */
    const char *schema = NULL;

    /* Check builtin tools */
    int idx = tool_map_lookup(&g_tool_map, name);
    if (idx >= 0 && idx < s_tool_count) {
        schema = s_tools[idx].input_schema_json;
    } else if (idx <= -10000) {
        int ei = -(idx + 10000);
        if (ei >= 0 && ei < g_external_tool_count) {
            schema = g_external_tools[ei].input_schema_json;
        }
    }

    if (!schema) return true; /* no schema = no validation */

    json_validation_t v = json_validate_schema(input_json, schema);
    if (!v.valid) {
        snprintf(error_buf, error_len, "input validation failed for '%s': %s",
                 name, v.error);
        DSCO_SET_ERR(DSCO_ERR_TOOL, "%s", error_buf);
        return false;
    }
    return true;
}
