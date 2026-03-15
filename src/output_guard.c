#include "output_guard.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define OG_DEFAULT_REPEAT_LIMIT       40
#define OG_DEFAULT_REPEAT_MIN_BYTES   4096
#define OG_DEFAULT_MAX_TOTAL_BYTES    (64u * 1024u * 1024u)

#define OG_FRAME_MAX    2048
#define OG_NORM_MAX      256
#define OG_PREVIEW_MAX   256

typedef struct {
    int out_fd;              /* 1 or 2 (process fd replaced with pipe writer) */
    int mirror_fd;           /* duplicate of original fd (real terminal/file) */
    int read_fd;             /* read side of guard pipe */
    const char *name;        /* stdout / stderr */
    pthread_t thread;
    bool active;

    char frame_buf[OG_FRAME_MAX];
    size_t frame_len;

    char last_norm[OG_NORM_MAX];
    char last_preview[OG_PREVIEW_MAX];
    int repeat_count;
    size_t repeat_bytes;
    size_t total_bytes;
} og_stream_t;

typedef struct {
    bool initialized;
    int repeat_limit;
    size_t repeat_min_bytes;
    size_t motif_min_bytes;
    bool motif_skip_path_like;
    size_t max_total_bytes;
    volatile int tripped;
    og_stream_t streams[2];
} og_state_t;

static og_state_t g_og = {0};

static int env_int_clamped(const char *name, int def_val, int min_v, int max_v) {
    const char *s = getenv(name);
    if (!s || !*s) return def_val;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') return def_val;
    if (v < min_v) v = min_v;
    if (v > max_v) v = max_v;
    return (int)v;
}

static size_t env_size_clamped(const char *name, size_t def_val, size_t min_v, size_t max_v) {
    const char *s = getenv(name);
    if (!s || !*s) return def_val;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0') return def_val;
    if (v < min_v) v = min_v;
    if (v > max_v) v = max_v;
    return (size_t)v;
}

static void write_all_fd(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

static void escape_preview(const char *src, size_t len, char *out, size_t out_len) {
    size_t o = 0;
    bool prev_space = false;
    for (size_t i = 0; i < len && o + 2 < out_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_space) out[o++] = ' ';
            prev_space = true;
            continue;
        }
        if (!isprint(c)) continue;
        out[o++] = (char)c;
        prev_space = false;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static void normalize_frame(const char *src, size_t len, char *out, size_t out_len) {
    size_t o = 0;
    bool prev_space = false;

    for (size_t i = 0; i < len && o + 2 < out_len; i++) {
        unsigned char c = (unsigned char)src[i];

        /* Strip ANSI CSI escape sequences: ESC [ ... letter */
        if (c == 0x1b && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len) {
                unsigned char t = (unsigned char)src[i];
                if ((t >= '@' && t <= '~')) break;
                i++;
            }
            continue;
        }

        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_space) out[o++] = ' ';
            prev_space = true;
            continue;
        }

        if (c >= '0' && c <= '9') {
            out[o++] = '#';
            prev_space = false;
            continue;
        }

        if (c < 32 || c == 127) continue;

        if (c >= 128) {
            /* Normalize non-ASCII glyphs (spinner frames etc.). */
            out[o++] = '@';
            prev_space = false;
            continue;
        }

        out[o++] = (char)tolower(c);
        prev_space = false;
    }

    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static int repeated_prefix_count(const char *s) {
    int n = (int)strlen(s);
    if (n < 8) return 1;

    int best = 1;
    int max_period = n / 2;
    if (max_period > 96) max_period = 96;

    for (int period = 2; period <= max_period; period++) {
        int reps = 1;
        int pos = period;
        while (pos + period <= n) {
            if (memcmp(s, s + pos, (size_t)period) != 0) break;
            reps++;
            pos += period;
        }
        if (reps > best) best = reps;
    }
    return best;
}

static bool frame_is_path_like(const char *s) {
    if (!s || s[0] == '\0') return false;

    size_t len = strlen(s);
    if (len < 128) return false;

    int slash_count = 0;
    int non_path_chars = 0;
    int space_count = 0;
    int alpha_count = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\') slash_count++;
        else if (isspace(c)) space_count++;
        else if (isalnum(c) || c == '_' || c == '-' || c == '.') alpha_count++;
        else non_path_chars++;
    }

    if (space_count > 0) return false;
    if (slash_count < 3) return false;
    if (alpha_count == 0) return false;

    int core_len = (int)len - space_count;
    if (core_len <= 0) return false;

    if (non_path_chars * 10 > core_len) return false;
    if (slash_count * 100 < core_len * 15) return false;
    return true;
}

static int diagnostic_fd(void) {
    og_stream_t *err = &g_og.streams[1];
    if (err->active && err->mirror_fd >= 0) return err->mirror_fd;
    og_stream_t *out = &g_og.streams[0];
    if (out->active && out->mirror_fd >= 0) return out->mirror_fd;
    return STDERR_FILENO;
}

static void output_guard_trip(const og_stream_t *s, const char *reason) {
    if (__sync_lock_test_and_set(&g_og.tripped, 1)) {
        return;  /* Already tripped — just suppress. */
    }

    int fd = diagnostic_fd();
    char msg[1400];
    int n = snprintf(
        msg, sizeof(msg),
        "\n\n[output-guard] suppressing runaway display output\n"
        "[output-guard] stream=%s reason=%s\n"
        "[output-guard] repeat_limit=%d repeat_count=%d repeat_bytes=%zu total_bytes=%zu\n"
        "[output-guard] last_frame=\"%s\"\n"
        "[output-guard] Output suppressed until stream settles. "
        "Set DSCO_OUTPUT_GUARD=0 to disable.\n\n",
        s && s->name ? s->name : "unknown",
        reason ? reason : "repeat flood",
        g_og.repeat_limit,
        s ? s->repeat_count : 0,
        s ? s->repeat_bytes : 0,
        s ? s->total_bytes : 0,
        (s && s->last_preview[0]) ? s->last_preview : "(empty)");
    if (n > 0) write_all_fd(fd, msg, (size_t)n);

    /* Don't _exit — keep draining pipes to prevent deadlock.
     * The LLM-level repdet will abort the stream gracefully. */
}

static void finalize_frame(og_stream_t *s, char delim) {
    if (!s || s->frame_len == 0) return;

    size_t len = s->frame_len;
    while (len > 0 &&
           (s->frame_buf[len - 1] == '\n' || s->frame_buf[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) {
        s->frame_len = 0;
        return;
    }

    char norm[OG_NORM_MAX];
    char preview[OG_PREVIEW_MAX];
    normalize_frame(s->frame_buf, len, norm, sizeof(norm));
    escape_preview(s->frame_buf, len, preview, sizeof(preview));

    if (norm[0] == '\0') {
        s->frame_len = 0;
        return;
    }

    bool track_repeats = !(delim == '\r' && len < 256);
    if (track_repeats) {
        if (strcmp(norm, s->last_norm) == 0) {
            s->repeat_count++;
            s->repeat_bytes += len;
        } else {
            snprintf(s->last_norm, sizeof(s->last_norm), "%s", norm);
            snprintf(s->last_preview, sizeof(s->last_preview), "%s", preview);
            s->repeat_count = 1;
            s->repeat_bytes = len;
        }

        if (s->repeat_count > g_og.repeat_limit &&
            s->repeat_bytes >= g_og.repeat_min_bytes) {
            output_guard_trip(s, "repeated frame flood");
        }
    }

    /* Catch repeated motifs inside one long frame, e.g. "pin ON -> pin OFF" spam.
     * Require a large payload so long but finite path-like lines don't trigger
     * on normal legitimate output (for example a long source-path fragment).
     */
    size_t motif_min_bytes = g_og.motif_min_bytes > 0 ? g_og.motif_min_bytes : OG_DEFAULT_REPEAT_MIN_BYTES;
    if (motif_min_bytes < g_og.repeat_min_bytes) motif_min_bytes = g_og.repeat_min_bytes;
    if (motif_min_bytes < 256) motif_min_bytes = 256;

    if (len >= motif_min_bytes && !(g_og.motif_skip_path_like && frame_is_path_like(norm))) {
        int in_frame_reps = repeated_prefix_count(norm);
        if (in_frame_reps > g_og.repeat_limit) {
            output_guard_trip(s, "in-frame repeated motif flood");
        }
    }

    s->frame_len = 0;
}

static void process_bytes(og_stream_t *s, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (s->frame_len + 1 < sizeof(s->frame_buf)) {
            s->frame_buf[s->frame_len++] = c;
            s->frame_buf[s->frame_len] = '\0';
        } else {
            /* Extremely long frame without separator: segment it. */
            finalize_frame(s, '\0');
            s->frame_buf[s->frame_len++] = c;
            s->frame_buf[s->frame_len] = '\0';
        }

        if (c == '\n' || c == '\r') {
            finalize_frame(s, c);
        }
    }
}

static void *stream_thread(void *arg) {
    og_stream_t *s = (og_stream_t *)arg;
    char buf[8192];

    for (;;) {
        ssize_t n = read(s->read_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        s->total_bytes += (size_t)n;

        if (g_og.tripped) {
            /* Keep draining pipe to prevent writer deadlock,
             * but don't mirror or process — output is suppressed. */
            continue;
        }

        if (s->total_bytes > g_og.max_total_bytes) {
            output_guard_trip(s, "total output byte budget exceeded");
            continue;
        }

        write_all_fd(s->mirror_fd, buf, (size_t)n);
        process_bytes(s, buf, (size_t)n);
    }

    finalize_frame(s, '\0');
    return NULL;
}

static void uninstall_stream(og_stream_t *s) {
    if (!s || !s->active) return;
    (void)dup2(s->mirror_fd, s->out_fd);
    if (s->read_fd >= 0) close(s->read_fd);
    if (s->mirror_fd >= 0) close(s->mirror_fd);
    s->read_fd = -1;
    s->mirror_fd = -1;
    s->active = false;
}

static bool install_stream(og_stream_t *s, int out_fd, const char *name) {
    memset(s, 0, sizeof(*s));
    s->out_fd = out_fd;
    s->name = name;
    s->mirror_fd = -1;
    s->read_fd = -1;

    int mirror_fd = dup(out_fd);
    if (mirror_fd < 0) return false;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        close(mirror_fd);
        return false;
    }

    if (dup2(pipefd[1], out_fd) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(mirror_fd);
        return false;
    }

    close(pipefd[1]);
    s->mirror_fd = mirror_fd;
    s->read_fd = pipefd[0];
    s->active = true;
    return true;
}

void output_guard_reset(void) {
    if (!g_og.initialized) return;
    __sync_lock_release(&g_og.tripped);
    for (int i = 0; i < 2; i++) {
        og_stream_t *s = &g_og.streams[i];
        if (!s->active) continue;
        s->repeat_count = 0;
        s->repeat_bytes = 0;
        s->last_norm[0] = '\0';
        s->last_preview[0] = '\0';
        s->frame_len = 0;
    }
}

bool output_guard_init(void) {
    if (g_og.initialized) return true;

    const char *enabled_env = getenv("DSCO_OUTPUT_GUARD");
    if (enabled_env && (strcmp(enabled_env, "0") == 0 ||
                        strcasecmp(enabled_env, "false") == 0 ||
                        strcasecmp(enabled_env, "off") == 0)) {
        return false;
    }

    g_og.repeat_limit = env_int_clamped("DSCO_OUTPUT_REPEAT_LIMIT",
                                        OG_DEFAULT_REPEAT_LIMIT, 1, 1000000);
    g_og.repeat_min_bytes = env_size_clamped("DSCO_OUTPUT_REPEAT_MIN_BYTES",
                                             OG_DEFAULT_REPEAT_MIN_BYTES, 1, (size_t)1 << 30);
    g_og.motif_min_bytes = env_size_clamped("DSCO_OUTPUT_MOTIF_MIN_BYTES",
                                            OG_DEFAULT_REPEAT_MIN_BYTES, 256,
                                            (size_t)1 << 30);
    g_og.motif_skip_path_like = env_int_clamped("DSCO_OUTPUT_MOTIF_SKIP_PATHLIKE",
                                               1, 0, 1) != 0;
    g_og.max_total_bytes = env_size_clamped("DSCO_OUTPUT_MAX_BYTES",
                                            OG_DEFAULT_MAX_TOTAL_BYTES, 1024, (size_t)1 << 32);
    g_og.tripped = 0;

    bool out_ok = install_stream(&g_og.streams[0], STDOUT_FILENO, "stdout");
    bool err_ok = install_stream(&g_og.streams[1], STDERR_FILENO, "stderr");

    if (!out_ok && !err_ok) return false;
    if (!out_ok || !err_ok) {
        /* Require both streams for complete coverage. Roll back partial setup. */
        uninstall_stream(&g_og.streams[0]);
        uninstall_stream(&g_og.streams[1]);
        return false;
    }

    /* Keep ordering/streaming deterministic once fds are redirected. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (pthread_create(&g_og.streams[0].thread, NULL, stream_thread, &g_og.streams[0]) != 0) {
        uninstall_stream(&g_og.streams[0]);
        uninstall_stream(&g_og.streams[1]);
        return false;
    }
    if (pthread_create(&g_og.streams[1].thread, NULL, stream_thread, &g_og.streams[1]) != 0) {
        uninstall_stream(&g_og.streams[0]);
        uninstall_stream(&g_og.streams[1]);
        return false;
    }

    pthread_detach(g_og.streams[0].thread);
    pthread_detach(g_og.streams[1].thread);
    g_og.initialized = true;
    return true;
}
