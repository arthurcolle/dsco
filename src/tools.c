#include "tools.h"
#include "net_server.h"
#include "mesh.h"
#include "peer_bootstrap.h"
#include "vfs.h"
#include "self_improve.h"
#include "error.h"
#include "integrations.h"
#include "trading.h"
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
#include "task_profile.h"
#include "plan_optimizer.h"
#include "plan_cache.h"
#include "cost_model.h"
#include "mcp_names.h"
#include "workspace.h"
#include "governance.h"
#include "memory_tier.h"
#include "talons.h"
#include "vm.h"
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
#include <limits.h>
#include <pwd.h>
#include <curl/curl.h>
#include <regex.h>
#include <pthread.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

extern volatile int g_interrupted;

/* ── Forward declaration: net_tool.c ─────────────────────────────────── */
extern bool tool_net_dispatch(const char *input, char *result, size_t rlen);
extern void dsco_net_routes_register(void *srv_opaque);


static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ── Agent self-exit flag (set by self_exit tool) ─────────────────────── */
volatile int g_agent_exit_requested = 0;

/* ── §8: VFS-backed tool result cache for deterministic tools ──────────── */
static vfs_db_t *g_tools_vfs = NULL;

void tools_set_vfs(vfs_db_t *vfs) { g_tools_vfs = vfs; }

static bool tool_is_cacheable(const char *name) {
    static const char *cacheable[] = {
        "sha256", "md5", "base64_encode", "base64_decode",
        "hmac_sha256", "hex_encode", "hex_decode", "eval",
        "hkdf_sha256", NULL
    };
    for (int i = 0; cacheable[i]; i++)
        if (strcmp(name, cacheable[i]) == 0) return true;
    return false;
}

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

/* ── Virtual context window: context-aware offload + register pressure ── */

static int g_ctx_window_tokens = 0;  /* 0 = unknown/unset */
static int g_ctx_used_input_tokens = 0;
static int g_ctx_used_output_tokens = 0;

void tools_set_context_window(int tokens) {
    g_ctx_window_tokens = tokens > 0 ? tokens : 0;
}

void tools_set_context_usage(int input_tokens, int output_tokens) {
    g_ctx_used_input_tokens = input_tokens;
    g_ctx_used_output_tokens = output_tokens;
}

int tools_context_window(void) {
    return g_ctx_window_tokens;
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

/* Default swarm stream callback — streams child tokens to stderr in real-time.
 * This is used by swarm_wait_any (agent_race), agent_wait, etc. so
 * the user sees tokens from sub-agents as they arrive, not just at the end. */
static void default_swarm_stream_cb(int child_id, const char *data, size_t len, void *ctx) {
    (void)ctx;
    if (!data || len == 0) return;

    /* Buffer partial lines per child — use the child's stream_buf */
    swarm_child_t *c = swarm_get(&g_swarm, child_id);
    if (!c) return;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\r') continue;

        if (ch == '\n') {
            /* Flush line */
            if (c->stream_buf_len > 0) {
                int display_len = (int)(c->stream_buf_len > 160 ? 160 : c->stream_buf_len);
                fprintf(stderr, "  \033[2m│\033[0m \033[36m[agent %d]\033[0m %.*s%s\n",
                        child_id, display_len, c->stream_buf,
                        c->stream_buf_len > 160 ? "..." : "");
                c->stream_buf_len = 0;
                c->stream_buf[0] = '\0';
            }
            continue;
        }

        if (c->stream_buf_len >= 4095) {
            int display_len = (int)(c->stream_buf_len > 160 ? 160 : c->stream_buf_len);
            fprintf(stderr, "  \033[2m│\033[0m \033[36m[agent %d]\033[0m %.*s...\n",
                    child_id, display_len, c->stream_buf);
            c->stream_buf_len = 0;
        }

        c->stream_buf[c->stream_buf_len++] = (char)ch;
        c->stream_buf[c->stream_buf_len] = '\0';
    }
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
        swarm_detect_executors(&g_swarm);
        /* Wire up default streaming so all poll paths emit tokens live */
        g_swarm.stream_cb = default_swarm_stream_cb;
        g_swarm.stream_ctx = NULL;
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

static bool mkdir_p_local(const char *path, mode_t mode) {
    if (!path || !path[0]) return false;

    char tmp[4096];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (tmp[0] != '\0' && mkdir(tmp, mode) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return false;
    return true;
}

static bool ensure_parent_dir_local(const char *path) {
    if (!path || !path[0]) return false;

    char tmp[4096];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);

    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    if (tmp[0] == '\0') return true;
    return mkdir_p_local(tmp, 0755);
}

static bool write_all_fd(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static bool split_path_dir_base(const char *path,
                                char *dir, size_t dir_len,
                                const char **base_out) {
    if (!path || !path[0] || !dir || dir_len == 0 || !base_out) return false;

    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dir, dir_len, ".");
        *base_out = path;
        return path[0] != '\0';
    }

    *base_out = slash + 1;
    if (**base_out == '\0') return false;

    size_t n = (slash == path) ? 1 : (size_t)(slash - path);
    if (n >= dir_len) return false;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return true;
}

static void best_effort_fsync_parent_dir(const char *path) {
    char dir[4096];
    const char *base = NULL;
    if (!split_path_dir_base(path, dir, sizeof(dir), &base)) return;
    (void)base;

    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

static bool file_content_matches(const char *path, const char *content, size_t len) {
    if (len == 0) return true;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    char buf[8192];
    size_t off = 0;
    while (off < len) {
        size_t want = len - off;
        if (want > sizeof(buf)) want = sizeof(buf);
        ssize_t n = read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0 || memcmp(buf, content + off, (size_t)n) != 0) {
            close(fd);
            return false;
        }
        off += (size_t)n;
    }

    close(fd);
    return true;
}

static bool file_region_matches(const char *path, off_t start,
                                const char *content, size_t len) {
    if (len == 0) return true;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    char buf[8192];
    size_t off = 0;
    while (off < len) {
        size_t want = len - off;
        if (want > sizeof(buf)) want = sizeof(buf);
        ssize_t n = pread(fd, buf, want, start + (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0 || memcmp(buf, content + off, (size_t)n) != 0) {
            close(fd);
            return false;
        }
        off += (size_t)n;
    }

    close(fd);
    return true;
}

static bool verified_write_file(const char *path, const char *content,
                                size_t len, char *err, size_t err_len,
                                mode_t *out_mode) {
    if (err && err_len > 0) err[0] = '\0';
    if (out_mode) *out_mode = 0644;
    if (!path || !path[0]) {
        snprintf(err, err_len, "empty path");
        return false;
    }

    struct stat existing;
    mode_t mode = 0644;
    if (stat(path, &existing) == 0) {
        if (!S_ISREG(existing.st_mode)) {
            snprintf(err, err_len, "target is not a regular file");
            return false;
        }
        mode = existing.st_mode & 0777;
    } else if (errno != ENOENT) {
        snprintf(err, err_len, "stat failed: %s", strerror(errno));
        return false;
    }

    char dir[4096];
    const char *base = NULL;
    if (!split_path_dir_base(path, dir, sizeof(dir), &base)) {
        snprintf(err, err_len, "invalid path");
        return false;
    }

    char tmp_path[4096];
    int n;
    if (strcmp(dir, "/") == 0)
        n = snprintf(tmp_path, sizeof(tmp_path), "/.%s.dsco-tmp-%ld-XXXXXX",
                     base, (long)getpid());
    else
        n = snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.dsco-tmp-%ld-XXXXXX",
                     dir, base, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        snprintf(err, err_len, "temporary path too long");
        return false;
    }

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        snprintf(err, err_len, "mkstemp failed: %s", strerror(errno));
        return false;
    }

    bool ok = false;
    do {
        if (fchmod(fd, mode) != 0) {
            snprintf(err, err_len, "chmod temp failed: %s", strerror(errno));
            break;
        }
        if (!write_all_fd(fd, content, len)) {
            snprintf(err, err_len, "write failed after partial write: %s", strerror(errno));
            break;
        }
        if (fsync(fd) != 0) {
            snprintf(err, err_len, "fsync temp failed: %s", strerror(errno));
            break;
        }
        if (close(fd) != 0) {
            fd = -1;
            snprintf(err, err_len, "close temp failed: %s", strerror(errno));
            break;
        }
        fd = -1;
        if (rename(tmp_path, path) != 0) {
            snprintf(err, err_len, "rename temp failed: %s", strerror(errno));
            break;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            snprintf(err, err_len, "post-write stat failed: %s", strerror(errno));
            break;
        }
        if (!S_ISREG(st.st_mode)) {
            snprintf(err, err_len, "post-write target is not a regular file");
            break;
        }
        if (st.st_size != (off_t)len) {
            snprintf(err, err_len, "post-write size mismatch: expected %zu got %lld",
                     len, (long long)st.st_size);
            break;
        }
        if (!file_content_matches(path, content, len)) {
            snprintf(err, err_len, "post-write readback verification failed");
            break;
        }

        best_effort_fsync_parent_dir(path);
        if (out_mode) *out_mode = st.st_mode & 0777;
        ok = true;
    } while (0);

    if (fd >= 0) close(fd);
    if (!ok) unlink(tmp_path);
    return ok;
}

static bool verified_append_file(const char *path, const char *content,
                                 size_t len, char *err, size_t err_len,
                                 off_t *out_size) {
    if (err && err_len > 0) err[0] = '\0';
    if (out_size) *out_size = 0;
    if (!path || !path[0]) {
        snprintf(err, err_len, "empty path");
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        snprintf(err, err_len, "open failed: %s", strerror(errno));
        return false;
    }

    bool ok = false;
    off_t start = 0;
    do {
        struct stat before;
        if (fstat(fd, &before) != 0) {
            snprintf(err, err_len, "pre-append stat failed: %s", strerror(errno));
            break;
        }
        if (!S_ISREG(before.st_mode)) {
            snprintf(err, err_len, "target is not a regular file");
            break;
        }
        start = before.st_size;

        if (!write_all_fd(fd, content, len)) {
            snprintf(err, err_len, "append failed after partial write: %s", strerror(errno));
            break;
        }
        if (fsync(fd) != 0) {
            snprintf(err, err_len, "fsync append failed: %s", strerror(errno));
            break;
        }
        if (close(fd) != 0) {
            fd = -1;
            snprintf(err, err_len, "close append failed: %s", strerror(errno));
            break;
        }
        fd = -1;

        struct stat after;
        if (stat(path, &after) != 0) {
            snprintf(err, err_len, "post-append stat failed: %s", strerror(errno));
            break;
        }
        if (after.st_size < start + (off_t)len) {
            snprintf(err, err_len, "post-append size mismatch: expected at least %lld got %lld",
                     (long long)(start + (off_t)len), (long long)after.st_size);
            break;
        }
        if (!file_region_matches(path, start, content, len)) {
            snprintf(err, err_len, "post-append readback verification failed");
            break;
        }

        best_effort_fsync_parent_dir(path);
        if (out_size) *out_size = after.st_size;
        ok = true;
    } while (0);

    if (fd >= 0) close(fd);
    return ok;
}

static bool shell_fragment_has_forbidden_chars(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case ';':
            case '&':
            case '|':
            case '<':
            case '>':
            case '`':
            case '$':
            case '\\':
            case '\'':
            case '"':
            case '\n':
            case '\r':
                return true;
            default:
                break;
        }
    }
    return false;
}

static bool chmod_mode_is_safe(const char *mode) {
    if (!mode || !mode[0]) return false;
    for (const unsigned char *p = (const unsigned char *)mode; *p; p++) {
        if ((*p >= '0' && *p <= '7') ||
            strchr("rwxXstugo=,+-", (int)*p) != NULL) {
            continue;
        }
        return false;
    }
    return true;
}

static bool http_method_is_safe(const char *method) {
    if (!method || !method[0]) return false;
    for (const unsigned char *p = (const unsigned char *)method; *p; p++) {
        if (!isalpha(*p)) return false;
    }
    return true;
}

static bool ssh_target_atom_is_safe(const char *s) {
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == ':')
            continue;
        return false;
    }
    return true;
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
#define CTX_RESULT_OFFLOAD_BYTES  16384
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
    if (bits) memset(bits, 0, sizeof(uint64_t) * CTX_EMBED_WORDS);

    if (!text || !*text) {
        if (norm_out) *norm_out = 0.0f;
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
                if (bits) bits[b / 64] |= (1ULL << (b % 64));
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
    if (env && env[0]) {
        int n = atoi(env);
        if (n < 4096) n = 4096;
        if (n > 131072) n = 131072;
        return n;
    }
    /* Models with ≥200K context: disable chunk-store offloading entirely.
     * The hash-histogram "embeddings" can't distinguish structured JSON
     * (same field names everywhere), causing 7-12 turn retrieval loops.
     * Instead, results are persisted to VFS and truncated inline. */
    if (g_ctx_window_tokens >= 200000) {
        return MAX_TOOL_RESULT;  /* 128KB — offloading never fires */
    }
    /* Smaller models: 10% of context window, floor 32KB */
    if (g_ctx_window_tokens > 0) {
        int threshold = (g_ctx_window_tokens * 4) / 10;  /* 10% */
        if (threshold < 32768) threshold = 32768;
        if (threshold > 131072) threshold = 131072;
        return threshold;
    }
    return CTX_RESULT_OFFLOAD_BYTES;
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
    int nchunks = info ? info->chunks_added : 0;
    int first   = info ? info->first_chunk_id : -1;
    int last    = info ? info->last_chunk_id : -1;
    size_t orig_len = original_result ? strlen(original_result) : 0;

    /* Inline as much of the original result as possible.
     * Reserve space for header (~200 bytes) + footer (~200 bytes).
     * The rest of the result buffer is for actual content. */
    size_t header_reserve = 256;
    size_t footer_reserve = 256;
    size_t inline_budget = 0;
    if (result_len > header_reserve + footer_reserve + 512) {
        inline_budget = result_len - header_reserve - footer_reserve;
    }
    /* Cap inline to ~60% of original to leave room for the model to work */
    if (inline_budget > orig_len * 6 / 10) {
        inline_budget = orig_len * 6 / 10;
    }
    /* But always inline at least 4KB if the buffer allows */
    if (inline_budget < 4096 && result_len > header_reserve + footer_reserve + 4096) {
        inline_budget = 4096;
    }

    /* Build chunk_ids array for batch retrieval */
    char ids_buf[512];
    ids_buf[0] = '\0';
    if (nchunks > 0 && nchunks <= 64) {
        size_t off = 0;
        off += (size_t)snprintf(ids_buf + off, sizeof(ids_buf) - off, "[");
        for (int i = first; i <= last && off + 16 < sizeof(ids_buf); i++) {
            if (i > first) off += (size_t)snprintf(ids_buf + off, sizeof(ids_buf) - off, ",");
            off += (size_t)snprintf(ids_buf + off, sizeof(ids_buf) - off, "%d", i);
        }
        snprintf(ids_buf + off, sizeof(ids_buf) - off, "]");
    }

    /* Write header */
    size_t off = 0;
    int n = snprintf(result + off, result_len - off,
                     "[offloaded %zu bytes → %d chunks, tool=%s]\n",
                     orig_len, nchunks,
                     tool_name ? tool_name : "unknown");
    if (n > 0 && (size_t)n < result_len - off) off += (size_t)n;

    /* Inline truncated content - the actual data, not a preview */
    if (inline_budget > 0 && original_result && orig_len > 0) {
        size_t copy = inline_budget;
        if (copy > orig_len) copy = orig_len;
        /* Try to break at a clean line boundary */
        if (copy < orig_len) {
            size_t scan = copy;
            while (scan > copy * 3 / 4) {
                if (original_result[scan] == '\n') { copy = scan + 1; break; }
                scan--;
            }
        }
        if (off + copy + 1 < result_len) {
            memcpy(result + off, original_result, copy);
            off += copy;
            result[off] = '\0';
        }
    }

    /* Footer with retrieval instructions */
    if (orig_len > inline_budget) {
        snprintf(result + off, result_len - off,
                 "\n[truncated — %zu/%zu bytes shown. "
                 "Remaining in chunks %s. "
                 "Use context_get_batch to retrieve.]",
                 inline_budget, orig_len, ids_buf);
    }
}

static __attribute__((unused)) void ctx_maybe_offload_tool_result(const char *tool_name,
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

/* ── Phase 1d: JSON structure-aware inline truncation ────────────────── */

static int ctx_inline_budget(void) {
    /* Budget = 20% of *remaining* context bytes, floor 4KB, cap MAX_TOOL_RESULT */
    if (g_ctx_window_tokens > 0 && g_ctx_used_input_tokens > 0) {
        int remaining = g_ctx_window_tokens - g_ctx_used_input_tokens;
        if (remaining < 1000) remaining = 1000;
        int budget = (remaining * 4) / 5;  /* 20% of remaining bytes */
        if (budget < 4096) budget = 4096;
        if (budget > (int)MAX_TOOL_RESULT) budget = (int)MAX_TOOL_RESULT;
        return budget;
    }
    /* Fallback: 32KB default */
    return 32768;
}

/* Extract JSON structural skeleton: keys + types + first 2 array elements */
static size_t ctx_truncate_json(const char *json, size_t json_len,
                                char *out, size_t out_len) {
    if (!json || json_len == 0 || !out || out_len < 128) return 0;

    size_t budget = out_len - 64;  /* reserve for footer */
    size_t wr = 0;
    int depth = 0;
    int array_elem_count[32] = {0};  /* track per nesting level */
    bool in_string = false;
    bool skip_value = false;
    int skip_depth = 0;

    for (size_t i = 0; i < json_len && wr < budget; i++) {
        char c = json[i];

        /* Handle string literals */
        if (in_string) {
            if (c == '\\' && i + 1 < json_len) {
                if (!skip_value) {
                    if (wr + 2 < budget) { out[wr++] = c; out[wr++] = json[i+1]; }
                }
                i++;
                continue;
            }
            if (c == '"') {
                in_string = false;
                if (!skip_value && wr < budget) out[wr++] = c;
            } else {
                if (!skip_value && wr < budget) out[wr++] = c;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            if (!skip_value && wr < budget) out[wr++] = c;
            continue;
        }

        /* Track array element counts for truncation */
        if (c == '[') {
            depth++;
            if (depth < 32) array_elem_count[depth] = 0;
            if (!skip_value && wr < budget) out[wr++] = c;
            continue;
        }
        if (c == '{') {
            depth++;
            if (!skip_value && wr < budget) out[wr++] = c;
            continue;
        }
        if (c == ']' || c == '}') {
            if (skip_value && depth <= skip_depth) {
                skip_value = false;
            }
            depth--;
            if (!skip_value && wr < budget) out[wr++] = c;
            continue;
        }
        if (c == ',') {
            /* Check if we're in an array and past 2 elements */
            if (depth > 0 && depth < 32) {
                array_elem_count[depth]++;
                if (array_elem_count[depth] >= 2) {
                    /* Skip remaining array elements */
                    if (!skip_value && wr + 16 < budget) {
                        wr += (size_t)snprintf(out + wr, budget - wr, ",\"...\"]");
                        /* Skip until matching close bracket */
                        int skip = 1;
                        for (size_t j = i + 1; j < json_len && skip > 0; j++) {
                            if (json[j] == '[' || json[j] == '{') skip++;
                            else if (json[j] == ']' || json[j] == '}') {
                                skip--;
                                if (skip == 0) { i = j; break; }
                            } else if (json[j] == '"') {
                                for (j++; j < json_len && json[j] != '"'; j++) {
                                    if (json[j] == '\\') j++;
                                }
                            }
                        }
                        depth--;
                        continue;
                    }
                }
            }
            if (!skip_value && wr < budget) out[wr++] = c;
            continue;
        }

        if (!skip_value && wr < budget) out[wr++] = c;
    }
    out[wr] = '\0';
    return wr;
}

/* Generic truncation: first 40% + truncation marker + last 20% */
static size_t ctx_truncate_generic(const char *text, size_t text_len,
                                   const char *vfs_key,
                                   char *out, size_t out_len) {
    if (!text || text_len == 0 || !out || out_len < 128) return 0;

    size_t budget = out_len - 128;  /* reserve for marker */
    size_t head = budget * 2 / 5;   /* 40% */
    size_t tail = budget / 5;       /* 20% */

    if (head + tail >= text_len) {
        /* Fits entirely */
        size_t n = text_len < budget ? text_len : budget;
        memcpy(out, text, n);
        out[n] = '\0';
        return n;
    }

    /* Try to break head at a newline */
    size_t head_actual = head;
    for (size_t s = head; s > head * 3 / 4; s--) {
        if (text[s] == '\n') { head_actual = s + 1; break; }
    }

    memcpy(out, text, head_actual);
    size_t wr = head_actual;

    wr += (size_t)snprintf(out + wr, out_len - wr,
                           "\n[... %zu bytes truncated", text_len - head_actual - tail);
    if (vfs_key && vfs_key[0]) {
        wr += (size_t)snprintf(out + wr, out_len - wr, " | key=%s for full result", vfs_key);
    }
    wr += (size_t)snprintf(out + wr, out_len - wr, " ...]\n");

    /* Tail */
    size_t tail_start = text_len - tail;
    size_t tail_actual = text_len - tail_start;
    if (wr + tail_actual < out_len) {
        memcpy(out + wr, text + tail_start, tail_actual);
        wr += tail_actual;
    }
    out[wr] = '\0';
    return wr;
}

/* ── Phase 2b: VFS-backed persist and truncate ───────────────────────── */

static void ctx_persist_and_truncate(const char *tool_name,
                                     const char *input_json,
                                     bool ok,
                                     char *result,
                                     size_t result_len) {
    if (!ok || !tool_name || !result) return;
    if (ctx_is_internal_tool(tool_name)) return;
    if (strncmp(result, "error:", 6) == 0) return;

    size_t rlen = strlen(result);
    if (rlen < 4096) return;  /* don't bother for small results */

    /* Build VFS key: {tool}:{sha256(input)[:16]} */
    char vfs_key[128];
    vfs_key[0] = '\0';
    if (g_tools_vfs && input_json) {
        char hash[65];
        sha256_hex((const uint8_t *)input_json, strlen(input_json), hash);
        snprintf(vfs_key, sizeof(vfs_key), "%s:%.16s", tool_name, hash);

        /* Persist full result to VFS (TTL: 1 hour) */
        vfs_result_put(g_tools_vfs, tool_name, hash, result, 3600);
    }

    /* Decide inline budget */
    int budget = ctx_inline_budget();
    if ((int)rlen <= budget) return;  /* fits inline, no truncation needed */

    /* Detect JSON and use structure-aware truncation */
    char *truncated = malloc(budget + 256);
    if (!truncated) return;

    /* Always preserve the first line (breadcrumb) */
    const char *first_nl = strchr(result, '\n');
    size_t first_line = first_nl ? (size_t)(first_nl - result + 1) : 0;
    size_t wr = 0;
    if (first_line > 0 && first_line < 256) {
        memcpy(truncated, result, first_line);
        wr = first_line;
    }

    /* Check if result looks like JSON */
    const char *body = result + first_line;
    size_t body_len = rlen - first_line;
    bool is_json = false;
    for (size_t i = 0; i < body_len && i < 64; i++) {
        if (body[i] == '{' || body[i] == '[') { is_json = true; break; }
        if (!isspace((unsigned char)body[i])) break;
    }

    if (is_json) {
        wr += ctx_truncate_json(body, body_len, truncated + wr, (size_t)budget - wr);
    } else {
        wr += ctx_truncate_generic(body, body_len, vfs_key, truncated + wr, (size_t)budget - wr);
    }

    /* Add VFS key footer */
    if (vfs_key[0]) {
        snprintf(truncated + wr, (size_t)budget + 256 - wr,
                 "\n[truncated %zu→%zu bytes | key=%s for full result]",
                 rlen, wr, vfs_key);
    } else {
        snprintf(truncated + wr, (size_t)budget + 256 - wr,
                 "\n[truncated %zu→%zu bytes]",
                 rlen, wr);
    }

    /* Replace result */
    snprintf(result, result_len, "%s", truncated);
    free(truncated);
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
             "use context_get_batch with chunk_ids to retrieve multiple chunks in one call");
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

/* Expand leading ~ / ~user in a user-supplied path. Takes ownership of `raw`
 * (frees it and returns a newly allocated replacement when expansion occurs)
 * so call sites stay one line:  path = path_normalize(path);
 * Returns `raw` unchanged if it does not start with ~ or if HOME/passwd lookup
 * fails. Returns NULL iff `raw` is NULL. Safe to chain directly on
 * json_get_str(). */
static char *path_normalize(char *raw) {
    if (!raw || raw[0] != '~') return raw;

    const char *home = NULL;
    const char *rest = NULL;
    char user[128];

    if (raw[1] == '\0' || raw[1] == '/') {
        home = getenv("HOME");
        if (!home || !home[0]) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        rest = raw + 1;
    } else {
        /* ~user or ~user/rest */
        const char *slash = strchr(raw + 1, '/');
        size_t ulen = slash ? (size_t)(slash - (raw + 1)) : strlen(raw + 1);
        if (ulen == 0 || ulen >= sizeof(user)) return raw;
        memcpy(user, raw + 1, ulen);
        user[ulen] = '\0';
        struct passwd *pw = getpwnam(user);
        if (!pw || !pw->pw_dir) return raw;
        home = pw->pw_dir;
        rest = slash ? slash : "";
    }

    if (!home || !home[0]) return raw;

    size_t need = strlen(home) + strlen(rest) + 1;
    char *out = malloc(need);
    if (!out) return raw;
    snprintf(out, need, "%s%s", home, rest);
    free(raw);
    return out;
}

static char *json_get_path_or_file_path(const char *input) {
    char *path = json_get_str(input, "path");
    if (!path) path = json_get_str(input, "file_path");
    return path_normalize(path);
}

static bool tool_write_file(const char *input, char *result, size_t rlen) {
    char *path = json_get_path_or_file_path(input);
    char *content = json_get_str(input, "content");
    if (!path || !content) {
        snprintf(result, rlen, "error: path/file_path and content required");
        free(path); free(content);
        return false;
    }
    if (!ensure_parent_dir_local(path)) {
        snprintf(result, rlen, "error: cannot create parent directories for %s: %s",
                 path, strerror(errno));
        free(path); free(content);
        return false;
    }

    size_t len = strlen(content);
    char err[256];
    mode_t mode = 0644;
    if (!verified_write_file(path, content, len, err, sizeof(err), &mode)) {
        snprintf(result, rlen, "error: verified write failed for %s: %s",
                 path, err[0] ? err : strerror(errno));
        free(path); free(content);
        return false;
    }
    snprintf(result, rlen,
             "verified write: path=%s bytes=%zu mode=%03o",
             path, len, mode & 0777);
    free(path); free(content);
    return true;
}

static __attribute__((unused)) bool tool_soul_read(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_soul_append(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_soul_write(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_soul_replace(const char *input, char *result, size_t rlen) {
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
    char *path = json_get_path_or_file_path(input);
    if (!path) { snprintf(result, rlen, "error: path/file_path required"); return false; }
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
    char *path = json_get_path_or_file_path(input);
    if (!path) { snprintf(result, rlen, "error: path/file_path required"); return false; }
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
    char *path = json_get_path_or_file_path(input);
    char *old_str = json_get_str(input, "old_string");
    char *new_str = json_get_str(input, "new_string");

    if (!path || !old_str || !new_str) {
        snprintf(result, rlen, "error: path/file_path, old_string, and new_string required");
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
    char *path = path_normalize(json_get_str(input, "path"));
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
    char *path = path_normalize(json_get_str(input, "path"));
    if (!pattern) { snprintf(result, rlen, "error: pattern required"); free(path); return false; }

    jbuf_t cmd;
    jbuf_init(&cmd, 256 + strlen(pattern) + (path ? strlen(path) : 1));
    jbuf_append(&cmd, "find ");
    shell_quote(&cmd, path ? path : ".");
    jbuf_append(&cmd, " -name ");
    shell_quote(&cmd, pattern);
    jbuf_append(&cmd, " -type f 2>/dev/null | head -100");
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(pattern); free(path);
    return true;
}

static bool tool_grep(const char *input, char *result, size_t rlen) {
    char *pattern = json_get_str(input, "pattern");
    char *path = path_normalize(json_get_str(input, "path"));
    if (!pattern) { snprintf(result, rlen, "error: pattern required"); free(path); return false; }
    char *include = json_get_str(input, "include");
    if (!include) include = json_get_str(input, "glob");
    char *mode = json_get_str(input, "output_mode");
    int head_limit = json_get_int(input, "head_limit", 100);
    if (head_limit < 0) head_limit = 100;
    if (head_limit > 1000) head_limit = 1000;

    jbuf_t cmd;
    jbuf_init(&cmd, 384 + strlen(pattern) + (path ? strlen(path) : 1) + (include ? strlen(include) : 0));
    jbuf_append(&cmd, "grep -rn");
    if (mode && strcmp(mode, "files_with_matches") == 0) jbuf_append(&cmd, " -l");
    else if (mode && strcmp(mode, "count") == 0) jbuf_append(&cmd, " -c");
    if (include && include[0]) {
        jbuf_append(&cmd, " --include ");
        shell_quote(&cmd, include);
    }
    jbuf_append(&cmd, " -- ");
    shell_quote(&cmd, pattern);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, path ? path : ".");
    if (head_limit > 0) {
        char limit[32];
        snprintf(limit, sizeof(limit), " 2>/dev/null | head -%d", head_limit);
        jbuf_append(&cmd, limit);
    } else {
        jbuf_append(&cmd, " 2>/dev/null");
    }
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(pattern); free(path); free(include); free(mode);
    return true;
}

static bool tool_file_info(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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
    char *path = path_normalize(json_get_str(input, "path"));
    char *content = json_get_str(input, "content");
    if (!path || !content) {
        snprintf(result, rlen, "error: path and content required");
        free(path); free(content); return false;
    }
    if (!ensure_parent_dir_local(path)) {
        snprintf(result, rlen, "error: cannot create parent directories for %s: %s",
                 path, strerror(errno));
        free(path); free(content); return false;
    }

    size_t len = strlen(content);
    char err[256];
    off_t final_size = 0;
    if (!verified_append_file(path, content, len, err, sizeof(err), &final_size)) {
        snprintf(result, rlen, "error: verified append failed for %s: %s",
                 path, err[0] ? err : strerror(errno));
        free(path); free(content); return false;
    }
    snprintf(result, rlen,
             "verified append: path=%s bytes_appended=%zu final_size=%lld",
             path, len, (long long)final_size);
    free(path); free(content);
    return true;
}

/* ── move_file ────────────────────────────────────────────────────────── */
static bool tool_move_file(const char *input, char *result, size_t rlen) {
    char *src = path_normalize(json_get_str(input, "source"));
    char *dst = path_normalize(json_get_str(input, "destination"));
    if (!dst) dst = path_normalize(json_get_str(input, "dest"));
    if (!src || !dst) {
        snprintf(result, rlen, "error: source and destination/dest required");
        free(src); free(dst); return false;
    }
    if (rename(src, dst) != 0) {
        /* Try mv for cross-device */
        jbuf_t cmd;
        jbuf_init(&cmd, 64 + strlen(src) + strlen(dst));
        jbuf_append(&cmd, "mv ");
        shell_quote(&cmd, src);
        jbuf_append(&cmd, " ");
        shell_quote(&cmd, dst);
        int status = run_cmd(cmd.data, result, rlen);
        jbuf_free(&cmd);
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
    char *src = path_normalize(json_get_str(input, "source"));
    char *dst = path_normalize(json_get_str(input, "destination"));
    if (!dst) dst = path_normalize(json_get_str(input, "dest"));
    if (!src || !dst) {
        snprintf(result, rlen, "error: source and destination/dest required");
        free(src); free(dst); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(src) + strlen(dst));
    jbuf_append(&cmd, "cp -r ");
    shell_quote(&cmd, src);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, dst);
    int status = run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    if (status == 0 && strlen(result) == 0)
        snprintf(result, rlen, "copied %s -> %s", src, dst);
    free(src); free(dst);
    return (status == 0);
}

/* ── delete_file ──────────────────────────────────────────────────────── */
static bool tool_delete_file(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    bool recursive = json_get_bool(input, "recursive", false);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }

    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(path));
    jbuf_append(&cmd, recursive ? "rm -rf " : "rm -f ");
    shell_quote(&cmd, path);
    int status = run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    if (status == 0) snprintf(result, rlen, "deleted %s", path);
    free(path);
    return (status == 0);
}

/* ── mkdir ────────────────────────────────────────────────────────────── */
static bool tool_mkdir(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    bool ok = mkdir_p_local(path, 0755);
    if (ok) snprintf(result, rlen, "created directory %s", path);
    else snprintf(result, rlen, "error: mkdir %s: %s", path, strerror(errno));
    free(path);
    return ok;
}

/* ── chmod ────────────────────────────────────────────────────────────── */
static __attribute__((unused)) bool tool_chmod(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    char *mode = json_get_str(input, "mode");
    if (!path || !mode) {
        snprintf(result, rlen, "error: path and mode required");
        free(path); free(mode); return false;
    }
    if (!chmod_mode_is_safe(mode)) {
        snprintf(result, rlen, "error: unsafe chmod mode");
        free(path); free(mode); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(path) + strlen(mode));
    jbuf_append(&cmd, "chmod ");
    shell_quote(&cmd, mode);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, path);
    int status = run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    if (status == 0) snprintf(result, rlen, "chmod %s %s", mode, path);
    free(path); free(mode);
    return (status == 0);
}

/* ── tree: Directory tree ─────────────────────────────────────────────── */
static __attribute__((unused)) bool tool_tree(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    int depth = json_get_int(input, "max_depth", 3);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "find '%s' -maxdepth %d -print 2>/dev/null | head -200 | sort",
             path ? path : ".", depth);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── wc: Word/line count ──────────────────────────────────────────────── */
static __attribute__((unused)) bool tool_wc(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "wc -l -w -c '%s'", path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── head/tail ────────────────────────────────────────────────────────── */
static __attribute__((unused)) bool tool_head(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    int lines = json_get_int(input, "lines", 20);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "head -n %d '%s'", lines, path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

static __attribute__((unused)) bool tool_tail(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    int lines = json_get_int(input, "lines", 20);
    if (!path) { snprintf(result, rlen, "error: path required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "tail -n %d '%s'", lines, path);
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

/* ── symlink ──────────────────────────────────────────────────────────── */
static __attribute__((unused)) bool tool_symlink(const char *input, char *result, size_t rlen) {
    char *target = path_normalize(json_get_str(input, "target"));
    char *link_path = path_normalize(json_get_str(input, "link_path"));
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
    char *source = path_normalize(json_get_str(input, "source"));
    char *output = path_normalize(json_get_str(input, "output"));
    char *flags = json_get_str(input, "flags");
    if (!source) { snprintf(result, rlen, "error: source required"); free(output); free(flags); return false; }
    if (!output) output = safe_strdup("a.out");

    if (flags && flags[0] && shell_fragment_has_forbidden_chars(flags)) {
        snprintf(result, rlen, "error: unsafe compile flags");
        free(source); free(output); free(flags);
        return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 128 + strlen(source) + strlen(output) + (flags ? strlen(flags) : 0));
    jbuf_append(&cmd, "cc -Wall -Wextra");
    if (flags && flags[0]) {
        jbuf_append(&cmd, " ");
        jbuf_append(&cmd, flags);
    }
    jbuf_append(&cmd, " -o ");
    shell_quote(&cmd, output);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, source);
    int status = run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
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

static bool shell_command_may_write_artifact(const char *cmd) {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (const char *p = cmd ? cmd : ""; *p; p++) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (*p == '\\' && !in_single) {
            escaped = true;
            continue;
        }
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (*p == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (in_single || in_double || *p != '>') continue;

        const char *q = p + 1;
        if (*q == '>') q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '&') continue;                 /* e.g. 2>&1 */
        if (strncmp(q, "/dev/null", 9) == 0) continue;
        return true;
    }

    return false;
}

static bool shell_command_mentions_artifact_verifier(const char *cmd) {
    static const char *needles[] = {
        "test -e", "test -f", "[ -e", "[ -f", "stat ",
        "wc -c", "cmp ", "diff ", "sha256", "shasum", NULL
    };
    for (int i = 0; needles[i]; i++) {
        if (strstr(cmd ? cmd : "", needles[i])) return true;
    }
    return false;
}

static void append_artifact_check_warning(const char *cmd, char *result, size_t rlen) {
    if (!result || rlen == 0) return;
    if (!shell_command_may_write_artifact(cmd)) return;
    if (shell_command_mentions_artifact_verifier(cmd)) return;

    const char *warning =
        "[artifact-check: shell redirection may have written files; "
        "verify expected paths with read_file, file_info, stat, or cmp before claiming success]";
    size_t cur = strlen(result);
    if (cur >= rlen - 1) return;
    snprintf(result + cur, rlen - cur, "%s%s", cur ? "\n" : "", warning);
}

#define ARTIFACT_CONTRACT_MAX_PATHS 8

typedef struct {
    char *paths[ARTIFACT_CONTRACT_MAX_PATHS];
    int count;
    int min_bytes;
    char *contains;
    char *sha256;
    bool malformed;
    char error[256];
} artifact_contract_t;

static void append_result_line(char *result, size_t rlen, const char *line) {
    if (!result || rlen == 0 || !line) return;
    size_t cur = strlen(result);
    if (cur >= rlen - 1) return;
    snprintf(result + cur, rlen - cur, "%s%s", cur ? "\n" : "", line);
}

static bool path_is_absolute_local(const char *path) {
    return path && path[0] == '/';
}

static char *resolve_cwd_path(const char *cwd) {
    if (!cwd || !cwd[0]) return NULL;

    char *norm = path_normalize(safe_strdup(cwd));
    if (!norm) return NULL;
    if (path_is_absolute_local(norm)) return norm;

    char here[PATH_MAX];
    if (!getcwd(here, sizeof(here))) return norm;

    size_t need = strlen(here) + 1 + strlen(norm) + 1;
    char *joined = safe_malloc(need);
    snprintf(joined, need, "%s/%s", here, norm);
    free(norm);
    return joined;
}

static char *normalize_artifact_path(char *raw, const char *cwd) {
    char *path = path_normalize(raw);
    if (!path || path_is_absolute_local(path)) return path;

    char *base = resolve_cwd_path(cwd);
    if (!base) return path;

    size_t need = strlen(base) + 1 + strlen(path) + 1;
    char *joined = safe_malloc(need);
    snprintf(joined, need, "%s/%s", base, path);
    free(base);
    free(path);
    return joined;
}

static void artifact_contract_add_path(artifact_contract_t *c,
                                       char *raw_path,
                                       const char *cwd) {
    if (!c || !raw_path) return;
    if (c->count >= ARTIFACT_CONTRACT_MAX_PATHS) {
        free(raw_path);
        c->malformed = true;
        snprintf(c->error, sizeof(c->error),
                 "too many artifact paths (max %d)", ARTIFACT_CONTRACT_MAX_PATHS);
        return;
    }

    char *path = normalize_artifact_path(raw_path, cwd);
    if (!path || !path[0]) {
        free(path);
        c->malformed = true;
        snprintf(c->error, sizeof(c->error), "empty artifact path");
        return;
    }

    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->paths[i], path) == 0) {
            free(path);
            return;
        }
    }
    c->paths[c->count++] = path;
}

typedef struct {
    artifact_contract_t *contract;
    const char *cwd;
} artifact_path_array_ctx_t;

static void artifact_path_array_cb(const char *element_start, void *ctx) {
    artifact_path_array_ctx_t *ac = (artifact_path_array_ctx_t *)ctx;
    if (!ac || !ac->contract || !element_start) return;

    char decoded[4096];
    ctx_decode_json_string_token(element_start, decoded, sizeof(decoded));
    if (!decoded[0]) return;
    artifact_contract_add_path(ac->contract, safe_strdup(decoded), ac->cwd);
}

static void artifact_contract_init(artifact_contract_t *c,
                                   const char *input,
                                   const char *cwd) {
    memset(c, 0, sizeof(*c));
    c->min_bytes = json_get_int(input, "verify_min_bytes", -1);
    if (c->min_bytes < 0) c->min_bytes = -1;
    c->contains = json_get_str(input, "verify_contains");
    c->sha256 = json_get_str(input, "verify_sha256");

    char *path = json_get_str(input, "verify_path");
    if (!path) path = json_get_str(input, "artifact_path");
    if (!path) path = json_get_str(input, "output_path");
    artifact_contract_add_path(c, path, cwd);

    artifact_path_array_ctx_t array_ctx = { .contract = c, .cwd = cwd };
    json_array_foreach(input ? input : "{}", "verify_paths",
                       artifact_path_array_cb, &array_ctx);

    if ((c->min_bytes >= 0 || c->contains || c->sha256) && c->count == 0) {
        c->malformed = true;
        snprintf(c->error, sizeof(c->error),
                 "artifact verification constraints require verify_path or verify_paths");
    }
}

static void artifact_contract_free(artifact_contract_t *c) {
    if (!c) return;
    for (int i = 0; i < c->count; i++) free(c->paths[i]);
    free(c->contains);
    free(c->sha256);
    memset(c, 0, sizeof(*c));
}

static bool bytes_contains_local(const char *haystack, size_t hay_len,
                                 const char *needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (hay_len < needle_len) return false;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static bool file_contains_string(const char *path, const char *needle,
                                 bool *found, char *err, size_t err_len) {
    if (found) *found = false;
    if (!needle || !needle[0]) {
        if (found) *found = true;
        return true;
    }

    size_t needle_len = strlen(needle);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, err_len, "open for contains check failed: %s", strerror(errno));
        return false;
    }

    size_t tail_cap = needle_len > 0 ? needle_len - 1 : 0;
    char *tail = tail_cap ? safe_malloc(tail_cap) : NULL;
    size_t tail_len = 0;
    char buf[8192];
    char *window = safe_malloc(sizeof(buf) + tail_cap);

    bool ok = true;
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            snprintf(err, err_len, "read for contains check failed: %s", strerror(errno));
            ok = false;
            break;
        }
        if (n == 0) break;

        if (tail_len > 0) memcpy(window, tail, tail_len);
        memcpy(window + tail_len, buf, (size_t)n);
        size_t window_len = tail_len + (size_t)n;
        if (bytes_contains_local(window, window_len, needle, needle_len)) {
            if (found) *found = true;
            break;
        }

        tail_len = tail_cap < window_len ? tail_cap : window_len;
        if (tail_len > 0) memcpy(tail, window + window_len - tail_len, tail_len);
    }

    free(window);
    free(tail);
    close(fd);
    return ok;
}

static bool file_sha256_hex(const char *path, char hex[65],
                            char *err, size_t err_len) {
    hex[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, err_len, "open for sha256 check failed: %s", strerror(errno));
        return false;
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t buf[8192];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            snprintf(err, err_len, "read for sha256 check failed: %s", strerror(errno));
            close(fd);
            return false;
        }
        if (n == 0) break;
        sha256_update(&ctx, buf, (size_t)n);
    }
    close(fd);

    uint8_t digest[32];
    sha256_final(&ctx, digest);
    hex_encode(digest, sizeof(digest), hex);
    return true;
}

static bool verify_artifact_contract(const artifact_contract_t *c,
                                     char *result, size_t rlen,
                                     char *err, size_t err_len) {
    if (err && err_len > 0) err[0] = '\0';
    if (!c || c->count == 0) return true;

    for (int i = 0; i < c->count; i++) {
        const char *path = c->paths[i];
        struct stat st;
        if (stat(path, &st) != 0) {
            snprintf(err, err_len, "missing artifact path=%s: %s",
                     path, strerror(errno));
            return false;
        }

        const char *type = S_ISDIR(st.st_mode) ? "directory" :
                           S_ISREG(st.st_mode) ? "file" : "other";
        if (c->min_bytes >= 0 && (!S_ISREG(st.st_mode) || st.st_size < c->min_bytes)) {
            snprintf(err, err_len,
                     "artifact path=%s expected at least %d bytes, got %lld",
                     path, c->min_bytes, (long long)st.st_size);
            return false;
        }

        char actual_sha[65] = "";
        if (c->sha256 && c->sha256[0]) {
            if (!S_ISREG(st.st_mode)) {
                snprintf(err, err_len, "artifact path=%s is not a regular file for sha256 check", path);
                return false;
            }
            if (!file_sha256_hex(path, actual_sha, err, err_len)) return false;
            if (strcasecmp(actual_sha, c->sha256) != 0) {
                snprintf(err, err_len,
                         "artifact path=%s sha256 mismatch: expected %s got %s",
                         path, c->sha256, actual_sha);
                return false;
            }
        }

        if (c->contains && c->contains[0]) {
            if (!S_ISREG(st.st_mode)) {
                snprintf(err, err_len, "artifact path=%s is not a regular file for contains check", path);
                return false;
            }
            bool found = false;
            if (!file_contains_string(path, c->contains, &found, err, err_len)) return false;
            if (!found) {
                snprintf(err, err_len,
                         "artifact path=%s does not contain required text", path);
                return false;
            }
        }

        char line[1024];
        if (actual_sha[0]) {
            snprintf(line, sizeof(line),
                     "[artifact-verified: path=%s type=%s size=%lld sha256=%s]",
                     path, type, (long long)st.st_size, actual_sha);
        } else {
            snprintf(line, sizeof(line),
                     "[artifact-verified: path=%s type=%s size=%lld]",
                     path, type, (long long)st.st_size);
        }
        append_result_line(result, rlen, line);
    }

    return true;
}

static bool tool_run_command(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }

    artifact_contract_t artifacts;
    artifact_contract_init(&artifacts, input, NULL);
    if (artifacts.malformed) {
        snprintf(result, rlen, "error: %s", artifacts.error);
        artifact_contract_free(&artifacts);
        free(command);
        return false;
    }

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
    bool ok = (status == 0);
    if (ok && artifacts.count > 0) {
        char err[512];
        if (!verify_artifact_contract(&artifacts, result, rlen, err, sizeof(err))) {
            char line[768];
            snprintf(line, sizeof(line), "[artifact-verification-failed: %s]", err);
            append_result_line(result, rlen, line);
            ok = false;
        }
    } else if (ok) {
        append_artifact_check_warning(command, result, rlen);
    }
    artifact_contract_free(&artifacts);
    free(cmd);
    free(escaped);
    free(command);
    return ok;
}

/* bash tool — streaming subprocess with live output deltas */
static bool tool_bash(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }

    int timeout = json_get_int(input, "timeout", 120);
    if (timeout <= 0) timeout = 120;
    if (timeout > 600) timeout = 600;
    char *cwd = path_normalize(json_get_str(input, "cwd"));

    artifact_contract_t artifacts;
    artifact_contract_init(&artifacts, input, cwd);
    if (artifacts.malformed) {
        snprintf(result, rlen, "error: %s", artifacts.error);
        artifact_contract_free(&artifacts);
        free(command);
        free(cwd);
        return false;
    }

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
    bool ok = (status == 0);
    if (ok && artifacts.count > 0) {
        char err[512];
        if (!verify_artifact_contract(&artifacts, result, rlen, err, sizeof(err))) {
            char line[768];
            snprintf(line, sizeof(line), "[artifact-verification-failed: %s]", err);
            append_result_line(result, rlen, line);
            ok = false;
        }
    } else if (ok) {
        append_artifact_check_warning(command, result, rlen);
    }
    artifact_contract_free(&artifacts);
    free(cmd);
    free(escaped);
    free(command);
    free(cwd);
    return ok;
}

/* ── run_background: Run a command in background ──────────────────────── */
static __attribute__((unused)) bool tool_run_background(const char *input, char *result, size_t rlen) {
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
    char *dir = path_normalize(json_get_str(input, "directory"));
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

static __attribute__((unused)) bool tool_kill_process(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_env_set(const char *input, char *result, size_t rlen) {
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
    char *path = path_normalize(json_get_str(input, "path"));
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "du -sh '%s' 2>/dev/null && echo '---' && df -h '%s'",
             path ? path : ".", path ? path : ".");
    run_cmd(cmd, result, rlen);
    free(path);
    return true;
}

static __attribute__((unused)) bool tool_which(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    if (!name) { snprintf(result, rlen, "error: name required"); return false; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "which '%s' 2>/dev/null && '%s' --version 2>/dev/null | head -1", name, name);
    run_cmd(cmd, result, rlen);
    free(name);
    return true;
}

static bool tool_cwd(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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

static __attribute__((unused)) bool tool_sed(const char *input, char *result, size_t rlen) {
    char *pattern = json_get_str(input, "expression");
    char *file = path_normalize(json_get_str(input, "file"));
    if (!pattern || !file) {
        snprintf(result, rlen, "error: expression and file required");
        free(pattern); free(file); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(pattern) + strlen(file));
    jbuf_append(&cmd, "sed ");
    shell_quote(&cmd, pattern);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, file);
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(pattern); free(file);
    return true;
}

static __attribute__((unused)) bool tool_awk(const char *input, char *result, size_t rlen) {
    char *program = json_get_str(input, "program");
    char *file = path_normalize(json_get_str(input, "file"));
    if (!program) {
        snprintf(result, rlen, "error: program required");
        free(file); return false;
    }
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(program) + (file ? strlen(file) : 0));
    if (file) {
        jbuf_append(&cmd, "awk ");
        shell_quote(&cmd, program);
        jbuf_append(&cmd, " ");
        shell_quote(&cmd, file);
    } else {
        jbuf_append(&cmd, "printf '' | awk ");
        shell_quote(&cmd, program);
    }
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(program); free(file);
    return true;
}

static __attribute__((unused)) bool tool_sort_uniq(const char *input, char *result, size_t rlen) {
    char *file = path_normalize(json_get_str(input, "file"));
    bool unique = json_get_bool(input, "unique", false);
    bool count = json_get_bool(input, "count", false);
    if (!file) { snprintf(result, rlen, "error: file required"); return false; }
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(file));
    if (count) {
        jbuf_append(&cmd, "sort ");
        shell_quote(&cmd, file);
        jbuf_append(&cmd, " | uniq -c | sort -rn | head -50");
    } else if (unique) {
        jbuf_append(&cmd, "sort -u ");
        shell_quote(&cmd, file);
        jbuf_append(&cmd, " | head -200");
    } else {
        jbuf_append(&cmd, "sort ");
        shell_quote(&cmd, file);
        jbuf_append(&cmd, " | head -200");
    }
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
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
    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(file1) + strlen(file2));
    jbuf_append(&cmd, "diff -u ");
    shell_quote(&cmd, file1);
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, file2);
    run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    free(file1); free(file2);
    return true;
}

static __attribute__((unused)) bool tool_patch(const char *input, char *result, size_t rlen) {
    char *file = path_normalize(json_get_str(input, "file"));
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

    jbuf_t cmd;
    jbuf_init(&cmd, 64 + strlen(file) + strlen(tmpfile));
    jbuf_append(&cmd, "patch ");
    shell_quote(&cmd, file);
    jbuf_append(&cmd, " -i ");
    shell_quote(&cmd, tmpfile);
    int status = run_cmd(cmd.data, result, rlen);
    jbuf_free(&cmd);
    unlink(tmpfile);
    free(file); free(patch_content);
    return (status == 0);
}

static bool tool_jq(const char *input, char *result, size_t rlen) {
    char *filter = json_get_str(input, "filter");
    char *json_input = json_get_str(input, "input");
    char *file = path_normalize(json_get_str(input, "file"));
    if (!filter) {
        snprintf(result, rlen, "error: filter required");
        free(json_input); free(file); return false;
    }
    if (file) {
        jbuf_t cmd;
        jbuf_init(&cmd, 64 + strlen(filter) + strlen(file));
        jbuf_append(&cmd, "jq ");
        shell_quote(&cmd, filter);
        jbuf_append(&cmd, " ");
        shell_quote(&cmd, file);
        run_cmd(cmd.data, result, rlen);
        jbuf_free(&cmd);
    } else if (json_input) {
        char tmpfile[] = "/tmp/dsco_jq_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd >= 0) { write(fd, json_input, strlen(json_input)); close(fd); }
        jbuf_t cmd;
        jbuf_init(&cmd, 64 + strlen(filter) + strlen(tmpfile));
        jbuf_append(&cmd, "jq ");
        shell_quote(&cmd, filter);
        jbuf_append(&cmd, " ");
        shell_quote(&cmd, tmpfile);
        run_cmd(cmd.data, result, rlen);
        jbuf_free(&cmd);
        unlink(tmpfile);
        free(filter); free(json_input); free(file);
        return true;
    } else {
        snprintf(result, rlen, "error: input or file required");
        free(filter); free(json_input); free(file); return false;
    }
    free(filter); free(json_input); free(file);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ENCODING & HASHING TOOLS
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool read_entire_file_bytes(const char *path, uint8_t **out_buf,
                                   size_t *out_len, char *result, size_t rlen) {
    if (!path || !out_buf || !out_len) return false;
    *out_buf = NULL;
    *out_len = 0;

    if (!require_regular_file(path, result, rlen)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(result, rlen, "error: cannot open %s: %s", path, strerror(errno));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(result, rlen, "error: cannot seek %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        snprintf(result, rlen, "error: cannot size %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        snprintf(result, rlen, "error: cannot rewind %s: %s", path, strerror(errno));
        fclose(f);
        return false;
    }

    size_t alloc = (size_t)fsize + 1;
    uint8_t *buf = malloc(alloc);
    if (!buf) {
        snprintf(result, rlen, "error: out of memory");
        fclose(f);
        return false;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, f);
    if (nread < (size_t)fsize && ferror(f)) {
        snprintf(result, rlen, "error: cannot read %s: %s", path, strerror(errno));
        free(buf);
        fclose(f);
        return false;
    }
    buf[nread] = '\0';
    fclose(f);

    *out_buf = buf;
    *out_len = nread;
    return true;
}

static bool base64_encode_result(const uint8_t *src, size_t src_len, bool url_safe,
                                 char *result, size_t rlen) {
    size_t needed = ((src_len + 2) / 3) * 4 + 1;
    if (needed > rlen) {
        snprintf(result, rlen, "error: result too large");
        return false;
    }

    size_t out = url_safe
        ? base64url_encode(src, src_len, result, rlen)
        : base64_encode(src, src_len, result, rlen);
    if (out + 1 > rlen) {
        snprintf(result, rlen, "error: result too large");
        return false;
    }
    result[out] = '\0';
    return true;
}

static bool base64_decode_result(const char *text, bool url_safe,
                                 char *result, size_t rlen) {
    if (!text) {
        snprintf(result, rlen, "error: input required");
        return false;
    }

    size_t src_len = strlen(text);
    uint8_t *decoded = malloc(src_len + 1);
    if (!decoded) {
        snprintf(result, rlen, "error: out of memory");
        return false;
    }

    size_t out = url_safe
        ? base64url_decode(text, src_len, decoded, src_len + 1)
        : base64_decode(text, src_len, decoded, src_len + 1);
    if (out + 1 > rlen) {
        free(decoded);
        snprintf(result, rlen, "error: result too large");
        return false;
    }

    memcpy(result, decoded, out);
    result[out] = '\0';
    free(decoded);
    return true;
}

static bool tool_base64(const char *input, char *result, size_t rlen) {
    char *data = json_get_str(input, "data");
    if (!data) data = json_get_str(input, "text");
    if (!data) data = json_get_str(input, "input");
    char *file = path_normalize(json_get_str(input, "file"));
    bool decode = json_get_bool(input, "decode", false);

    if (file) {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (!read_entire_file_bytes(file, &buf, &len, result, rlen)) {
            free(data); free(file);
            return false;
        }

        bool ok = decode
            ? base64_decode_result((const char *)buf, false, result, rlen)
            : base64_encode_result(buf, len, false, result, rlen);
        free(buf);
        free(data); free(file);
        return ok;
    } else if (data) {
        bool ok = decode
            ? base64_decode_result(data, false, result, rlen)
            : base64_encode_result((const uint8_t *)data, strlen(data), false, result, rlen);
        free(data); free(file);
        return ok;
    } else {
        snprintf(result, rlen, "error: data or file required");
        free(data); free(file); return false;
    }
}

static __attribute__((unused)) bool tool_hash(const char *input, char *result, size_t rlen) {
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_tar(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_zip(const char *input, char *result, size_t rlen) {
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
    if (!http_method_is_safe(method)) {
        snprintf(result, rlen, "error: unsafe http method");
        free(url); free(method); free(headers_str); free(body);
        return false;
    }

    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "curl -sS");
    if (include_headers) jbuf_append(&cmd, " -i");
    jbuf_append(&cmd, " -X ");
    shell_quote(&cmd, method);
    jbuf_append(&cmd, " --max-time ");
    char timeout_str[16]; snprintf(timeout_str, sizeof(timeout_str), "%d", timeout);
    jbuf_append(&cmd, timeout_str);

    if (headers_str) {
        char *hcopy = safe_strdup(headers_str);
        char *hdr = strtok(hcopy, "\n");
        while (hdr) {
            jbuf_append(&cmd, " -H ");
            shell_quote(&cmd, hdr);
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
            jbuf_append(&cmd, " --data-binary @");
            shell_quote(&cmd, body_tmpfile);
        }
    }

    jbuf_append(&cmd, " ");
    shell_quote(&cmd, url);

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
    char *output = path_normalize(json_get_str(input, "output"));
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

static __attribute__((unused)) bool tool_upload(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_http_headers(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    if (!url) { snprintf(result, rlen, "error: url required"); return false; }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "curl -sS -I '%s'", url);
    run_cmd(cmd, result, rlen);
    free(url);
    return true;
}

static __attribute__((unused)) bool tool_ws_test(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_market_quote(const char *input, char *result, size_t rlen) {
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
    jbuf_t cmd;
    jbuf_init(&cmd, 256 + strlen(url));
    jbuf_append(&cmd, "curl -sS -L --max-time 15 -H 'User-Agent: Mozilla/5.0' ");
    shell_quote(&cmd, url);
    run_cmd(cmd.data, html, MAX_TOOL_RESULT);
    jbuf_free(&cmd);

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

static __attribute__((unused)) bool tool_screenshot(const char *input, char *result, size_t rlen) {
    char *output_path = path_normalize(json_get_str(input, "path"));
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

/* ── Computer use: native desktop control (mouse/keyboard/screen) ──────────
 *
 * A single dispatch tool that drives the local desktop the way a human would:
 * move/click the mouse, type, press key combos, scroll, and capture the
 * screen. On macOS this uses CoreGraphics CGEvent APIs directly (no external
 * binaries, no Anthropic-specific computer-use tool type — it's an ordinary
 * custom tool). On Linux it shells out to xdotool/scrot.
 *
 * Screenshots are fed back to the model through the existing
 * /tmp/dsco_img_<pid>.b64 hook that the agent loop drains after every tool
 * batch, so the model sees the result of each action — the visual feedback
 * loop that makes computer use work.
 *
 * Coordinates are in *logical points* (the screenshot is resized to match),
 * so a click at (x,y) lands where the model sees it, Retina or not. */

/* Logical (point) display size — the coordinate space for clicks. */
static void cu_display_size(int *w, int *h) {
    *w = 1280; *h = 800;  /* safe default if detection fails */
#ifdef __APPLE__
    CGDirectDisplayID disp = CGMainDisplayID();
    CGRect b = CGDisplayBounds(disp);
    if (b.size.width > 0 && b.size.height > 0) {
        *w = (int)b.size.width;
        *h = (int)b.size.height;
    }
#else
    char out[128] = "";
    if (run_cmd("xdotool getdisplaygeometry 2>/dev/null", out, sizeof(out)) == 0) {
        int gw = 0, gh = 0;
        if (sscanf(out, "%d %d", &gw, &gh) == 2 && gw > 0 && gh > 0) {
            *w = gw; *h = gh;
        }
    }
#endif
}

/* Parse a "coordinate":[x,y] array (or fall back to x/y scalar fields). */
static bool cu_get_coord(const char *input, int *x, int *y) {
    char *raw = json_get_raw(input, "coordinate");
    if (raw) {
        int ok = (sscanf(raw, " [ %d , %d ]", x, y) == 2);
        free(raw);
        if (ok) return true;
    }
    if (json_get_raw(input, "x") || json_get_raw(input, "y")) {
        *x = json_get_int(input, "x", 0);
        *y = json_get_int(input, "y", 0);
        return true;
    }
    return false;
}

/* Emit a PNG to the agent's image pickup file so it is attached to the next
 * turn (same mechanism as view_image). */
static void cu_emit_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 8 * 1024 * 1024) { fclose(f); return; }
    unsigned char *raw = safe_malloc((size_t)fsize);
    size_t nread = fread(raw, 1, (size_t)fsize, f);
    fclose(f);
    size_t b64_len = ((nread + 2) / 3) * 4 + 1;
    char *b64 = safe_malloc(b64_len);
    size_t oi = base64_encode(raw, nread, b64, b64_len);
    b64[oi] = '\0';
    free(raw);
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/dsco_img_%d.b64", getpid());
    FILE *tmp = fopen(tmppath, "w");
    if (tmp) { fprintf(tmp, "image/png\n%s", b64); fclose(tmp); }
    free(b64);
}

/* Capture the full screen to a PNG resized to logical-point dimensions so the
 * screenshot pixel space matches the click coordinate space. */
static bool cu_screenshot(char *path_out, size_t path_len) {
    int w = 0, h = 0;
    cu_display_size(&w, &h);
    snprintf(path_out, path_len, "/tmp/dsco_computer_%d.png", getpid());
    char cmd[512], scratch[1024];
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd), "screencapture -x '%s' 2>/dev/null", path_out);
    run_cmd(cmd, scratch, sizeof(scratch));
    /* Resize to logical points so coordinates align (Retina-safe). */
    snprintf(cmd, sizeof(cmd),
             "sips -z %d %d '%s' >/dev/null 2>&1", h, w, path_out);
    run_cmd(cmd, scratch, sizeof(scratch));
#else
    snprintf(cmd, sizeof(cmd),
             "(scrot -o '%s' || import -window root '%s') 2>/dev/null",
             path_out, path_out);
    run_cmd(cmd, scratch, sizeof(scratch));
#endif
    struct stat st;
    return stat(path_out, &st) == 0 && st.st_size > 0;
}

#ifdef __APPLE__
/* macOS virtual keycodes for named keys (xdotool-style names accepted). */
typedef struct { const char *name; CGKeyCode code; } cu_keymap_t;
static const cu_keymap_t CU_KEYS[] = {
    {"return",36},{"enter",36},{"tab",48},{"space",49},{"delete",51},
    {"backspace",51},{"escape",53},{"esc",53},{"forwarddelete",117},
    {"left",123},{"right",124},{"down",125},{"up",126},
    {"home",115},{"end",119},{"pageup",116},{"pagedown",121},
    {"f1",122},{"f2",120},{"f3",99},{"f4",118},{"f5",96},{"f6",97},
    {"f7",98},{"f8",100},{"f9",101},{"f10",109},{"f11",103},{"f12",111},
    {"minus",27},{"equal",24},{"grave",50},{"semicolon",41},{"quote",39},
    {"comma",43},{"period",47},{"slash",44},{"backslash",42},
    {"a",0},{"s",1},{"d",2},{"f",3},{"h",4},{"g",5},{"z",6},{"x",7},
    {"c",8},{"v",9},{"b",11},{"q",12},{"w",13},{"e",14},{"r",15},{"y",16},
    {"t",17},{"o",31},{"u",32},{"i",34},{"p",35},{"l",37},{"j",38},
    {"k",40},{"n",45},{"m",46},
    {"1",18},{"2",19},{"3",20},{"4",21},{"5",23},{"6",22},{"7",26},
    {"8",28},{"9",25},{"0",29},
};

static bool cu_keycode(const char *name, CGKeyCode *out) {
    char low[64];
    size_t i = 0;
    for (; name[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';
    for (size_t k = 0; k < sizeof(CU_KEYS) / sizeof(CU_KEYS[0]); k++) {
        if (strcmp(CU_KEYS[k].name, low) == 0) { *out = CU_KEYS[k].code; return true; }
    }
    return false;
}

static CGEventFlags cu_modifier(const char *name) {
    if (!strcasecmp(name, "cmd") || !strcasecmp(name, "command") ||
        !strcasecmp(name, "super") || !strcasecmp(name, "win")) return kCGEventFlagMaskCommand;
    if (!strcasecmp(name, "ctrl") || !strcasecmp(name, "control")) return kCGEventFlagMaskControl;
    if (!strcasecmp(name, "alt") || !strcasecmp(name, "option") ||
        !strcasecmp(name, "opt")) return kCGEventFlagMaskAlternate;
    if (!strcasecmp(name, "shift")) return kCGEventFlagMaskShift;
    if (!strcasecmp(name, "fn")) return kCGEventFlagMaskSecondaryFn;
    return 0;
}

static void cu_post_mouse(CGEventType type, int x, int y, CGMouseButton btn, int clicks) {
    CGPoint pt = CGPointMake((double)x, (double)y);
    CGEventRef ev = CGEventCreateMouseEvent(NULL, type, pt, btn);
    if (clicks > 1) CGEventSetIntegerValueField(ev, kCGMouseEventClickState, clicks);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

static void cu_click(int x, int y, CGMouseButton btn, int clicks) {
    CGEventType down = (btn == kCGMouseButtonRight) ? kCGEventRightMouseDown :
                       (btn == kCGMouseButtonCenter) ? kCGEventOtherMouseDown : kCGEventLeftMouseDown;
    CGEventType up   = (btn == kCGMouseButtonRight) ? kCGEventRightMouseUp :
                       (btn == kCGMouseButtonCenter) ? kCGEventOtherMouseUp : kCGEventLeftMouseUp;
    CGWarpMouseCursorPosition(CGPointMake((double)x, (double)y));
    for (int c = 1; c <= clicks; c++) {
        cu_post_mouse(down, x, y, btn, c);
        cu_post_mouse(up, x, y, btn, c);
    }
}

static bool cu_key_combo(const char *spec, char *err, size_t errlen) {
    /* spec like "cmd+shift+t" or "Return" — last token is the key. */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    CGEventFlags flags = 0;
    char *tokens[8]; int nt = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+- ", &save); t && nt < 8; t = strtok_r(NULL, "+- ", &save))
        tokens[nt++] = t;
    if (nt == 0) { snprintf(err, errlen, "empty key spec"); return false; }
    CGKeyCode code = 0;
    bool have_code = false;
    for (int i = 0; i < nt; i++) {
        CGEventFlags m = cu_modifier(tokens[i]);
        if (m && i < nt - 1) { flags |= m; continue; }
        if (cu_keycode(tokens[i], &code)) have_code = true;
        else if (m) { flags |= m; }  /* trailing modifier alone */
    }
    if (!have_code) { snprintf(err, errlen, "unknown key: %s", spec); return false; }
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventRef kd = CGEventCreateKeyboardEvent(src, code, true);
    CGEventSetFlags(kd, flags);
    CGEventPost(kCGHIDEventTap, kd);
    CFRelease(kd);
    CGEventRef ku = CGEventCreateKeyboardEvent(src, code, false);
    CGEventSetFlags(ku, flags);
    CGEventPost(kCGHIDEventTap, ku);
    CFRelease(ku);
    if (src) CFRelease(src);
    return true;
}

static void cu_type_text(const char *text) {
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CFStringRef s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!s) { if (src) CFRelease(src); return; }
    CFIndex n = CFStringGetLength(s);
    for (CFIndex i = 0; i < n; i++) {
        UniChar ch = CFStringGetCharacterAtIndex(s, i);
        CGEventRef kd = CGEventCreateKeyboardEvent(src, 0, true);
        CGEventKeyboardSetUnicodeString(kd, 1, &ch);
        CGEventPost(kCGHIDEventTap, kd);
        CFRelease(kd);
        CGEventRef ku = CGEventCreateKeyboardEvent(src, 0, false);
        CGEventKeyboardSetUnicodeString(ku, 1, &ch);
        CGEventPost(kCGHIDEventTap, ku);
        CFRelease(ku);
        usleep(8000);
    }
    CFRelease(s);
    if (src) CFRelease(src);
}

static void cu_scroll(const char *dir, int amount) {
    int dy = 0, dx = 0;
    if (!strcasecmp(dir, "up")) dy = amount;
    else if (!strcasecmp(dir, "down")) dy = -amount;
    else if (!strcasecmp(dir, "left")) dx = amount;
    else if (!strcasecmp(dir, "right")) dx = -amount;
    CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2,
                                                  dy, dx);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}
#endif /* __APPLE__ */

static bool tool_computer(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen, "missing: action (screenshot|cursor_position|mouse_move|"
                 "left_click|right_click|middle_click|double_click|triple_click|"
                 "left_click_drag|key|type|scroll|wait)");
        return false;
    }

    int w = 0, h = 0;
    cu_display_size(&w, &h);
    bool ok = true;
    bool want_shot = true;   /* most actions return a fresh screenshot */
    char scratch[1024] __attribute__((unused));
    if (rlen) result[0] = '\0';

#ifdef __APPLE__
    if (!strcmp(action, "screenshot")) {
        /* handled below via want_shot */
    } else if (!strcmp(action, "cursor_position")) {
        CGEventRef e = CGEventCreate(NULL);
        CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        snprintf(result, rlen, "{\"x\":%d,\"y\":%d,\"display\":[%d,%d]}",
                 (int)p.x, (int)p.y, w, h);
        free(action);
        return true;  /* no screenshot needed */
    } else if (!strcmp(action, "mouse_move")) {
        int x, y;
        if (!cu_get_coord(input, &x, &y)) { snprintf(result, rlen, "mouse_move needs coordinate [x,y]"); free(action); return false; }
        CGWarpMouseCursorPosition(CGPointMake(x, y));
        cu_post_mouse(kCGEventMouseMoved, x, y, kCGMouseButtonLeft, 0);
    } else if (!strcmp(action, "left_click") || !strcmp(action, "right_click") ||
               !strcmp(action, "middle_click") || !strcmp(action, "double_click") ||
               !strcmp(action, "triple_click")) {
        int x, y;
        if (!cu_get_coord(input, &x, &y)) {
            CGEventRef e = CGEventCreate(NULL); CGPoint p = CGEventGetLocation(e); CFRelease(e);
            x = (int)p.x; y = (int)p.y;
        }
        CGMouseButton btn = !strcmp(action, "right_click") ? kCGMouseButtonRight :
                            !strcmp(action, "middle_click") ? kCGMouseButtonCenter : kCGMouseButtonLeft;
        int clicks = !strcmp(action, "double_click") ? 2 :
                     !strcmp(action, "triple_click") ? 3 : 1;
        cu_click(x, y, btn, clicks);
    } else if (!strcmp(action, "left_click_drag")) {
        int sx, sy;
        if (!cu_get_coord(input, &sx, &sy)) { snprintf(result, rlen, "drag needs start coordinate [x,y]"); free(action); return false; }
        int ex = json_get_int(input, "x2", sx), ey = json_get_int(input, "y2", sy);
        char *to = json_get_raw(input, "to");
        if (to) { sscanf(to, " [ %d , %d ]", &ex, &ey); free(to); }
        CGWarpMouseCursorPosition(CGPointMake(sx, sy));
        cu_post_mouse(kCGEventLeftMouseDown, sx, sy, kCGMouseButtonLeft, 1);
        cu_post_mouse(kCGEventLeftMouseDragged, ex, ey, kCGMouseButtonLeft, 1);
        cu_post_mouse(kCGEventLeftMouseUp, ex, ey, kCGMouseButtonLeft, 1);
    } else if (!strcmp(action, "key")) {
        char *text = json_get_str(input, "text");
        if (!text) { snprintf(result, rlen, "key needs text (e.g. \"cmd+a\", \"Return\")"); free(action); return false; }
        char err[128] = "";
        ok = cu_key_combo(text, err, sizeof(err));
        if (!ok) snprintf(result, rlen, "%s", err);
        free(text);
    } else if (!strcmp(action, "type")) {
        char *text = json_get_str(input, "text");
        if (!text) { snprintf(result, rlen, "type needs text"); free(action); return false; }
        cu_type_text(text);
        free(text);
    } else if (!strcmp(action, "scroll")) {
        char *dir = json_get_str(input, "scroll_direction");
        int amt = json_get_int(input, "scroll_amount", 3);
        cu_scroll(dir ? dir : "down", amt);
        free(dir);
    } else if (!strcmp(action, "wait")) {
        int ms = json_get_int(input, "duration", 1000);
        if (ms > 10000) ms = 10000;
        usleep((useconds_t)ms * 1000);
    } else {
        snprintf(result, rlen, "unknown computer action: %s", action);
        free(action);
        return false;
    }
#else
    /* Linux: drive xdotool. */
    char cmd[1024];
    if (!strcmp(action, "screenshot")) {
        /* handled below */
    } else if (!strcmp(action, "cursor_position")) {
        run_cmd("xdotool getmouselocation --shell 2>/dev/null", scratch, sizeof(scratch));
        int x = 0, y = 0; char *xp = strstr(scratch, "X="); char *yp = strstr(scratch, "Y=");
        if (xp) x = atoi(xp + 2); if (yp) y = atoi(yp + 2);
        snprintf(result, rlen, "{\"x\":%d,\"y\":%d,\"display\":[%d,%d]}", x, y, w, h);
        free(action); return true;
    } else if (!strcmp(action, "mouse_move")) {
        int x, y;
        if (!cu_get_coord(input, &x, &y)) { snprintf(result, rlen, "mouse_move needs coordinate [x,y]"); free(action); return false; }
        snprintf(cmd, sizeof(cmd), "xdotool mousemove %d %d", x, y);
        run_cmd(cmd, scratch, sizeof(scratch));
    } else if (!strcmp(action, "left_click") || !strcmp(action, "right_click") ||
               !strcmp(action, "middle_click") || !strcmp(action, "double_click") ||
               !strcmp(action, "triple_click")) {
        int x, y, btn = !strcmp(action, "right_click") ? 3 : !strcmp(action, "middle_click") ? 2 : 1;
        int clicks = !strcmp(action, "double_click") ? 2 : !strcmp(action, "triple_click") ? 3 : 1;
        if (cu_get_coord(input, &x, &y)) {
            snprintf(cmd, sizeof(cmd), "xdotool mousemove %d %d click --repeat %d %d", x, y, clicks, btn);
        } else {
            snprintf(cmd, sizeof(cmd), "xdotool click --repeat %d %d", clicks, btn);
        }
        run_cmd(cmd, scratch, sizeof(scratch));
    } else if (!strcmp(action, "left_click_drag")) {
        int sx, sy;
        if (!cu_get_coord(input, &sx, &sy)) { snprintf(result, rlen, "drag needs start coordinate [x,y]"); free(action); return false; }
        int ex = json_get_int(input, "x2", sx), ey = json_get_int(input, "y2", sy);
        char *to = json_get_raw(input, "to");
        if (to) { sscanf(to, " [ %d , %d ]", &ex, &ey); free(to); }
        snprintf(cmd, sizeof(cmd), "xdotool mousemove %d %d mousedown 1 mousemove %d %d mouseup 1",
                 sx, sy, ex, ey);
        run_cmd(cmd, scratch, sizeof(scratch));
    } else if (!strcmp(action, "key")) {
        char *text = json_get_str(input, "text");
        if (!text) { snprintf(result, rlen, "key needs text"); free(action); return false; }
        /* xdotool uses + for combos already; map common aliases. */
        snprintf(cmd, sizeof(cmd), "xdotool key '%s'", text);
        run_cmd(cmd, scratch, sizeof(scratch));
        free(text);
    } else if (!strcmp(action, "type")) {
        char *text = json_get_str(input, "text");
        if (!text) { snprintf(result, rlen, "type needs text"); free(action); return false; }
        char qtmp[64] = "/tmp/dsco_type_XXXXXX";
        int fd = mkstemp(qtmp);
        if (fd >= 0) { write(fd, text, strlen(text)); close(fd);
            snprintf(cmd, sizeof(cmd), "xdotool type --clearmodifiers --file '%s'", qtmp);
            run_cmd(cmd, scratch, sizeof(scratch)); unlink(qtmp);
        }
        free(text);
    } else if (!strcmp(action, "scroll")) {
        char *dir = json_get_str(input, "scroll_direction");
        int amt = json_get_int(input, "scroll_amount", 3);
        int btn = (dir && !strcasecmp(dir, "up")) ? 4 : (dir && !strcasecmp(dir, "down")) ? 5 :
                  (dir && !strcasecmp(dir, "left")) ? 6 : 7;
        snprintf(cmd, sizeof(cmd), "xdotool click --repeat %d %d", amt, btn);
        run_cmd(cmd, scratch, sizeof(scratch));
        free(dir);
    } else if (!strcmp(action, "wait")) {
        int ms = json_get_int(input, "duration", 1000);
        if (ms > 10000) ms = 10000;
        usleep((useconds_t)ms * 1000);
    } else {
        snprintf(result, rlen, "unknown computer action: %s", action);
        free(action);
        return false;
    }
#endif

    if (want_shot && ok) {
        usleep(120000);  /* let the UI settle before capturing */
        char shot[256];
        if (cu_screenshot(shot, sizeof(shot))) {
            cu_emit_image(shot);
            struct stat st; stat(shot, &st);
            snprintf(result, rlen,
                     "{\"action\":\"%s\",\"display\":[%d,%d],\"screenshot\":\"%s\","
                     "\"note\":\"Screenshot attached for the next turn. "
                     "Coordinates are in display points.\"}",
                     action, w, h, shot);
        } else if (result[0] == '\0' || !strcmp(action, "screenshot")) {
            snprintf(result, rlen, "{\"action\":\"%s\",\"error\":\"screen capture failed "
                     "(check screen-recording permission)\"}", action);
            ok = false;
        }
    } else if (result[0] == '\0') {
        snprintf(result, rlen, "{\"action\":\"%s\",\"display\":[%d,%d],\"ok\":true}",
                 action, w, h);
    }

    free(action);
    return ok;
}

static __attribute__((unused)) bool tool_json_api(const char *input, char *result, size_t rlen) {
    char *url = json_get_str(input, "url");
    char *method = json_get_str(input, "method");
    char *body = json_get_str(input, "body");
    char *auth = json_get_str(input, "auth_header");
    if (!url) {
        snprintf(result, rlen, "error: url required");
        free(method); free(body); free(auth); return false;
    }
    if (!method) method = safe_strdup("GET");
    if (!http_method_is_safe(method)) {
        snprintf(result, rlen, "error: unsafe http method");
        free(url); free(method); free(body); free(auth); return false;
    }

    jbuf_t cmd;
    jbuf_init(&cmd, 4096);
    jbuf_append(&cmd, "curl -sS -X ");
    shell_quote(&cmd, method);
    jbuf_append(&cmd, " -H 'Content-Type: application/json' -H 'Accept: application/json'");
    if (auth) {
        jbuf_t auth_hdr;
        jbuf_init(&auth_hdr, strlen(auth) + 32);
        jbuf_append(&auth_hdr, "Authorization: ");
        jbuf_append(&auth_hdr, auth);
        jbuf_append(&cmd, " -H ");
        shell_quote(&cmd, auth_hdr.data ? auth_hdr.data : "Authorization:");
        jbuf_free(&auth_hdr);
    }
    char json_tmpfile[32] = "";
    if (body) {
        strcpy(json_tmpfile, "/tmp/dsco_json_XXXXXX");
        int fd = mkstemp(json_tmpfile);
        if (fd >= 0) {
            write(fd, body, strlen(body));
            close(fd);
            jbuf_append(&cmd, " --data-binary @");
            shell_quote(&cmd, json_tmpfile);
        }
    }
    jbuf_append(&cmd, " ");
    shell_quote(&cmd, url);

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

static __attribute__((unused)) bool tool_docker_compose(const char *input, char *result, size_t rlen) {
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
    if (!ssh_target_atom_is_safe(host) || (user && user[0] && !ssh_target_atom_is_safe(user))) {
        snprintf(result, rlen, "error: unsafe ssh host/user");
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

static __attribute__((unused)) bool tool_scp(const char *input, char *result, size_t rlen) {
    /* path_normalize only expands a leading ~ so remote scp endpoints like
     * user@host:~/file (which do not start with ~) are passed through untouched. */
    char *source = path_normalize(json_get_str(input, "source"));
    char *destination = path_normalize(json_get_str(input, "destination"));
    char *key = path_normalize(json_get_str(input, "key"));
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

static __attribute__((unused)) bool tool_psql(const char *input, char *result, size_t rlen) {
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
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_node(const char *input, char *result, size_t rlen) {
    char *code = json_get_str(input, "code");
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_clipboard(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_pkg(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_pip(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) { snprintf(result, rlen, "error: command required"); return false; }
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "pip3 %s", command);
    run_cmd(cmd, result, rlen);
    free(command);
    return true;
}

static __attribute__((unused)) bool tool_npm(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_crontab(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_xattr(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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

static __attribute__((unused)) bool tool_self_inspect(const char *input, char *result, size_t rlen) {
    char *dir = path_normalize(json_get_str(input, "project_dir"));
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

static __attribute__((unused)) bool tool_inspect_file(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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

static __attribute__((unused)) bool tool_call_graph(const char *input, char *result, size_t rlen) {
    char *func = json_get_str(input, "function");
    char *dir = path_normalize(json_get_str(input, "project_dir"));
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

static __attribute__((unused)) bool tool_dependency_graph(const char *input, char *result, size_t rlen) {
    char *dir = path_normalize(json_get_str(input, "project_dir"));
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

    /* Poll for latest data with live streaming */
    swarm_poll_stream(&g_swarm, 100, default_swarm_stream_cb, NULL);

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

    /* Poll loop with live streaming */
    while (1) {
        swarm_poll_stream(&g_swarm, 500, default_swarm_stream_cb, NULL);

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

/* agent_race — spawn N agents with the same task on different providers/models,
 * return the FIRST one to complete successfully, kill the rest.
 * This is the fundamental speed primitive: race models and take the fastest. */
static bool tool_agent_race(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    char *task = json_get_str(input, "task");
    if (!task) {
        snprintf(result, rlen, "{\"error\":\"task is required\"}");
        return false;
    }

    int timeout = json_get_int(input, "timeout", 60);
    timeout = clamp_timeout_seconds(timeout, 60, 5, 300);

    /* Parse contestants array: [{provider, model}] or ["model1", "model2"] */
    char *contestants_raw = json_get_raw(input, "contestants");
    if (!contestants_raw) {
        snprintf(result, rlen, "{\"error\":\"contestants array required\"}");
        free(task);
        return false;
    }

    /* Spawn all contestants */
    int ids[SWARM_MAX_CHILDREN];
    char *models[SWARM_MAX_CHILDREN];
    char *providers[SWARM_MAX_CHILDREN];
    int n = 0;

    /* Parse each contestant — either a string (model via openrouter) or {provider, model} */
    const char *p = contestants_raw;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    while (*p && n < SWARM_MAX_CHILDREN) {
        while (*p && (*p == ' ' || *p == '\n' || *p == ',')) p++;
        if (*p == ']' || !*p) break;

        if (*p == '{') {
            /* Object: {provider, model} */
            int depth = 0;
            const char *start = p;
            do { if (*p == '{') depth++; else if (*p == '}') depth--; p++; } while (*p && depth > 0);
            size_t olen = (size_t)(p - start);
            char *obj = safe_malloc(olen + 1);
            memcpy(obj, start, olen);
            obj[olen] = '\0';

            providers[n] = json_get_str(obj, "provider");
            models[n] = json_get_str(obj, "model");
            if (!providers[n]) providers[n] = safe_strdup("openrouter");
            free(obj);
        } else if (*p == '"') {
            /* String: just a model name, use openrouter */
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            size_t slen = (size_t)(p - start);
            models[n] = safe_malloc(slen + 1);
            memcpy(models[n], start, slen);
            models[n][slen] = '\0';
            providers[n] = safe_strdup("openrouter");
            if (*p == '"') p++;
        } else {
            p++;
            continue;
        }

        /* Spawn this contestant */
        int cid = swarm_spawn_provider(&g_swarm, -1, task, models[n], providers[n]);
        if (cid >= 0) {
            ids[n] = cid;
            n++;
            fprintf(stderr, "  \033[2m🏁 racer #%d: %s/%s\033[0m\n",
                    n, providers[n-1], models[n-1] ? models[n-1] : "default");
        }
    }
    free(contestants_raw);

    if (n == 0) {
        snprintf(result, rlen, "{\"error\":\"no contestants spawned\"}");
        free(task);
        return false;
    }

    fprintf(stderr, "  \033[1m🏁 Racing %d models...\033[0m\n", n);

    /* Wait for first SUCCESSFUL completion — skip errors */
    int winner_id = -1;
    int errors = 0;
    double deadline = (double)timeout;
    double race_start = 0;
    { struct timeval tv; gettimeofday(&tv, NULL); race_start = tv.tv_sec + tv.tv_usec / 1e6; }

    while (winner_id < 0) {
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        double elapsed = (now_tv.tv_sec + now_tv.tv_usec / 1e6) - race_start;
        int remaining = (int)((deadline - elapsed) * 1000);
        if (remaining <= 0) break;

        int cid = swarm_wait_any(&g_swarm, remaining);
        if (cid < 0) break; /* timeout */

        swarm_child_t *c = swarm_get(&g_swarm, cid);
        if (c && c->status == SWARM_DONE) {
            winner_id = cid;
        } else {
            /* Error — log and keep waiting for others */
            errors++;
            fprintf(stderr, "  \033[33m✗ racer %d errored (%s)\033[0m\n",
                    cid, c ? (c->output ? c->output : "unknown") : "null");
            if (errors >= n) break; /* all errored */
        }
    }

    if (winner_id < 0) {
        /* Timeout or all errored — kill all */
        for (int i = 0; i < n; i++) swarm_kill(&g_swarm, ids[i]);
        snprintf(result, rlen, "{\"error\":\"race: %s after %ds (%d errors, %d contestants)\"}",
                 errors >= n ? "all contestants errored" : "timed out", timeout, errors, n);
        free(task);
        for (int i = 0; i < n; i++) { free(models[i]); free(providers[i]); }
        return false;
    }

    /* We have a winner — kill all losers */
    swarm_child_t *winner = swarm_get(&g_swarm, winner_id);
    int killed = 0;
    for (int i = 0; i < n; i++) {
        if (ids[i] != winner_id) {
            swarm_kill(&g_swarm, ids[i]);
            killed++;
        }
    }

    /* Find which contestant index won */
    int winner_idx = -1;
    for (int i = 0; i < n; i++) if (ids[i] == winner_id) { winner_idx = i; break; }

    double elapsed = winner->end_time - winner->start_time;

    fprintf(stderr, "  \033[1;32m🏆 Winner: %s/%s in %.1fs (killed %d losers)\033[0m\n",
            providers[winner_idx], models[winner_idx] ? models[winner_idx] : "default", elapsed, killed);

    /* Build result with winner output */
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"winner\":{\"agent_id\":");
    jbuf_append_int(&b, winner_id);
    jbuf_append(&b, ",\"provider\":");
    jbuf_append_json_str(&b, providers[winner_idx]);
    jbuf_append(&b, ",\"model\":");
    jbuf_append_json_str(&b, models[winner_idx] ? models[winner_idx] : "");
    jbuf_appendf(&b, ",\"elapsed_sec\":%.3f", elapsed);
    jbuf_appendf(&b, ",\"status\":\"%s\"", swarm_status_str(winner->status));
    jbuf_append(&b, ",\"output\":");
    jbuf_append_json_str(&b, winner->output ? winner->output : "");
    jbuf_appendf(&b, "},\"total_contestants\":%d,\"killed\":%d}", n, killed);

    int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, written);
    result[written] = '\0';
    jbuf_free(&b);

    free(task);
    for (int i = 0; i < n; i++) { free(models[i]); free(providers[i]); }
    return winner->status == SWARM_DONE;
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

/* topology_solve — portfolio solver. Runs the task across several diverse
 * topologies, each anchored on a different model from the heterogeneous pool
 * (glm-5.2 / kimi-k2.7-code / step-3.7-flash), then a judge synthesizes the
 * strongest answer. This is the "leverage all topologies with diff models"
 * primitive: model diversity both ACROSS topologies (rotating coordinators)
 * and WITHIN each (DSCO_TOPO_HETERO tier pool). */
static bool tool_topology_solve(const char *input, char *result, size_t rlen) {
    char *task = json_get_str(input, "task");
    if (!task || !task[0]) {
        snprintf(result, rlen, "{\"error\":\"task required\"}");
        free(task);
        return false;
    }
    int timeout = clamp_timeout_seconds(json_get_int(input, "timeout", 300),
                                        300, 30, 3600);

    const char *api_key = tools_runtime_api_key();
    if (!api_key || !api_key[0]) {
        snprintf(result, rlen, "{\"error\":\"no runtime API key for topology execution\"}");
        free(task);
        return false;
    }

    /* Topology set: explicit names or a diverse default spanning categories. */
    const char *default_topos[] = { "trident", "debate", "tournament" };
    const char *topo_names[8];
    int ntopo = 0;
    char *topos_raw = json_get_raw(input, "topologies");
    if (topos_raw && *topos_raw == '[') {
        swarm_task_parse_ctx_t pc;
        memset(&pc, 0, sizeof(pc));
        json_array_foreach(input, "topologies", parse_swarm_task_element, &pc);
        for (int i = 0; i < pc.count && ntopo < 8; i++)
            topo_names[ntopo++] = pc.specs[i].task;  /* freed below via pc */
        /* defer free of pc until after run */
        if (ntopo == 0) {
            for (int i = 0; i < 3; i++) topo_names[ntopo++] = default_topos[i];
        }
        /* Rotating model anchors — one distinct model per topology slot. */
        const char *anchors[] = {
            "z-ai/glm-5.2", "moonshotai/kimi-k2.7-code", "stepfun/step-3.7-flash"
        };
        /* Force the heterogeneous tier pool for the duration of the solve. */
        char *prev_hetero = getenv("DSCO_TOPO_HETERO");
        char saved[8] = "";
        if (prev_hetero) snprintf(saved, sizeof(saved), "%s", prev_hetero);
        setenv("DSCO_TOPO_HETERO", "1", 1);

        jbuf_t synth;
        jbuf_init(&synth, 16384);
        jbuf_append(&synth, "You are the judge of a topology portfolio. The same "
                    "task was solved by multiple agent topologies, each anchored "
                    "on a different model. Pick the strongest answer, merge any "
                    "complementary insights, and produce one final result. "
                    "Note which topology/model each kept idea came from.\n\nTASK:\n");
        jbuf_append(&synth, task);

        int ran = 0;
        for (int i = 0; i < ntopo; i++) {
            const topology_t *t = topology_find(topo_names[i]);
            if (!t || !topology_is_runnable(t)) {
                fprintf(stderr, "  %s⚠ topology '%s' not runnable — skipping%s\n",
                        TUI_BYELLOW, topo_names[i], TUI_RESET);
                continue;
            }
            const char *anchor = anchors[i % 3];
            fprintf(stderr, "\n  %s⚡%s solve %d/%d: topology \"%s\" anchored on %s%s\n",
                    TUI_BCYAN, TUI_RESET, i + 1, ntopo, t->name, anchor, TUI_RESET);

            char *tr = safe_malloc(MAX_TOOL_RESULT);
            tr[0] = '\0';
            topology_run_stats_t st;
            bool ok = topology_run(t, api_key, anchor, task, tr, MAX_TOOL_RESULT, &st);
            char hdr[128];
            snprintf(hdr, sizeof(hdr),
                     "\n\n===== topology \"%s\" [%s] (%s) =====\n",
                     t->name, anchor, ok ? "ok" : "error");
            jbuf_append(&synth, hdr);
            /* Bound each topology's contribution to keep the judge prompt sane. */
            size_t trl = strlen(tr);
            const size_t CAP = 6000;
            if (trl > CAP) { jbuf_append(&synth, "[...tail...]\n"); jbuf_append(&synth, tr + trl - CAP); }
            else jbuf_append(&synth, tr);
            free(tr);
            ran++;
        }

        /* Restore hetero env. */
        if (saved[0]) setenv("DSCO_TOPO_HETERO", saved, 1);
        else unsetenv("DSCO_TOPO_HETERO");

        /* Judge/synthesize via a glm-5.2 coordinator sub-agent. */
        char *judge_out = NULL;
        int judge_id = -1;
        if (ran > 0) {
            ensure_swarm();
            fprintf(stderr, "\n  %s┌─ judge ─ glm-5.2 synthesizing %d topology results%s\n",
                    TUI_BYELLOW, ran, TUI_RESET);
            judge_id = swarm_spawn(&g_swarm, synth.data ? synth.data : task, "glm52");
            if (judge_id >= 0) {
                double js = now_sec_helper();
                while (1) {
                    swarm_poll_stream(&g_swarm, 100, default_swarm_stream_cb, NULL);
                    swarm_child_t *jc = swarm_get(&g_swarm, judge_id);
                    if (!jc) break;
                    if (jc->status == SWARM_DONE || jc->status == SWARM_ERROR ||
                        jc->status == SWARM_KILLED) break;
                    if (g_interrupted || now_sec_helper() - js >= timeout) break;
                }
                swarm_child_t *jc = swarm_get(&g_swarm, judge_id);
                if (jc && jc->output) judge_out = jc->output;
            }
        }
        jbuf_free(&synth);

        jbuf_t b;
        jbuf_init(&b, 16384);
        jbuf_append(&b, "{\"task_solved\":");
        jbuf_append(&b, ran > 0 ? "true" : "false");
        jbuf_append(&b, ",\"topologies_run\":");
        jbuf_append_int(&b, ran);
        jbuf_append(&b, ",\"final\":");
        if (judge_out) {
            size_t jl = strlen(judge_out);
            if (jl > 16384) {
                char tr2[16448];
                snprintf(tr2, sizeof(tr2), "[...truncated %zu bytes...]\n%s",
                         jl - 16384, judge_out + jl - 16384);
                jbuf_append_json_str(&b, tr2);
            } else jbuf_append_json_str(&b, judge_out);
        } else jbuf_append(&b, "null");
        jbuf_append(&b, "}");
        int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
        memcpy(result, b.data, written);
        result[written] = '\0';
        jbuf_free(&b);

        baseline_log("swarm", "topology_solve", task, NULL);
        for (int i = 0; i < pc.count; i++) { free(pc.specs[i].task); free(pc.specs[i].model); }
        free(topos_raw);
        free(task);
        return true;
    }

    /* No explicit topologies → use the diverse default set via recursion-free path. */
    free(topos_raw);
    /* Re-dispatch with defaults injected: build a synthetic input. */
    {
        jbuf_t inj;
        jbuf_init(&inj, 512);
        jbuf_append(&inj, "{\"task\":");
        jbuf_append_json_str(&inj, task);
        jbuf_append(&inj, ",\"topologies\":[\"trident\",\"debate\",\"tournament\"],\"timeout\":");
        jbuf_append_int(&inj, timeout);
        jbuf_append(&inj, "}");
        bool ok = tool_topology_solve(inj.data, result, rlen);
        jbuf_free(&inj);
        free(task);
        return ok;
    }
}


/* plan_analyze — Priority 1: show ranked topology options with cost/latency before execution */
static bool tool_plan_analyze(const char *input, char *result, size_t rlen) {
    char *task   = json_get_str(input, "task");
    int  budget  = json_get_int(input, "budget_cents", 0);

    if (!task || !task[0]) {
        free(task);
        snprintf(result, rlen, "{\"error\":\"task required\"}");
        return false;
    }

    plan_options_t *opts = plan_analyze(task, budget);
    free(task);

    if (!opts) {
        snprintf(result, rlen, "{\"error\":\"plan_analyze failed\"}");
        return false;
    }

    plan_options_json(opts, result, rlen);
    plan_options_free(opts);
    return true;
}

/* plan_cache_stats — show cache stats */
static bool tool_plan_cache_stats(const char *input, char *result, size_t rlen) {
    (void)input;
    plan_cache_stats_json(result, rlen);
    return true;
}

/* cost_model_stats — show learned cost model */
static bool tool_cost_model_stats(const char *input, char *result, size_t rlen) {
    (void)input;
    cost_model_stats_json(result, rlen);
    return true;
}

/* plan_cache_lookup — check cache for a task */
static bool tool_plan_cache_lookup(const char *input, char *result, size_t rlen) {
    char *task = json_get_str(input, "task");
    if (!task || !task[0]) {
        free(task);
        snprintf(result, rlen, "{\"error\":\"task required\"}");
        return false;
    }
    plan_cache_result_t hit;
    bool ok = plan_cache_lookup(task, &hit);
    free(task);
    if (ok) {
        snprintf(result, rlen,
            "{\"hit\":true,\"topology\":\"%s\",\"similarity\":%.3f,\"hits_before\":%d,\"rationale\":\"%s\"}",
            hit.topology_name, hit.similarity, hit.hits_before, hit.rationale);
    } else {
        snprintf(result, rlen, "{\"hit\":false}");
    }
    return true;
}

/* task_profile — Analyze a task string and recommend the best topology.
 * Exposes the Phase 1 dynamic topology selection engine as a tool. */
static bool tool_task_profile(const char *input, char *result, size_t rlen) {
    char *task = json_get_str(input, "task");
    if (!task || !task[0]) {
        snprintf(result, rlen, "{\"error\":\"task string is required\"}");
        free(task);
        return false;
    }

    bool explain = json_get_bool(input, "explain", false);

    task_profile_t *tp = task_profile(task, NULL);
    if (!tp) {
        snprintf(result, rlen, "{\"error\":\"profiling failed\"}");
        free(task);
        return false;
    }

    if (explain) {
        char explain_buf[2048];
        task_profile_explain(tp, explain_buf, sizeof(explain_buf));

        /* Wrap explanation + JSON into a combined result */
        jbuf_t b;
        jbuf_init(&b, 4096);
        jbuf_append(&b, "{\"explanation\":");
        jbuf_append_json_str(&b, explain_buf);
        jbuf_append(&b, ",\"profile\":");

        char json_buf[2048];
        task_profile_json(tp, json_buf, sizeof(json_buf));
        jbuf_append(&b, json_buf);

        jbuf_append(&b, "}");
        int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
        memcpy(result, b.data, written);
        result[written] = '\0';
        jbuf_free(&b);
    } else {
        task_profile_json(tp, result, rlen);
    }

    task_profile_free(tp);
    free(task);
    return true;
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

/* ── Hierarchical map-reduce swarm ─────────────────────────────────────────
 * Fan out N worker tasks in parallel (MAP), barrier-wait for them, then spawn a
 * single coordinator sub-agent that reduces the workers' outputs into one
 * synthesized answer (REDUCE). Each worker is itself a full dsco process, so it
 * can recursively spawn its own swarm — the tree is bounded by SWARM_MAX_DEPTH.
 * This is the declarative primitive for hierarchical, parallelizable swarms:
 * one tool call replaces the manual create → collect → synthesize dance. */

/* Stream-wait a group to completion. Returns 1=complete, 0=timeout, -1=interrupt. */
static int swarm_barrier_wait(int gid, int timeout, double start) {
    swarm_group_t *grp = &g_swarm.groups[gid];
    swarm_live_ctx_t live_ctx;
    memset(&live_ctx, 0, sizeof(live_ctx));
    live_ctx.group_id = gid;
    live_ctx.swarm = &g_swarm;
    int last_done = -1;
    while (!swarm_group_complete(&g_swarm, gid)) {
        swarm_poll_stream(&g_swarm, 100, swarm_live_stream_cb, &live_ctx);
        if (g_interrupted) return -1;
        int done_count = 0;
        for (int i = 0; i < grp->child_count; i++) {
            swarm_child_t *c = &g_swarm.children[grp->child_ids[i]];
            if (c->status == SWARM_DONE || c->status == SWARM_ERROR ||
                c->status == SWARM_KILLED)
                done_count++;
        }
        if (done_count > last_done) {
            last_done = done_count;
            int active = grp->child_count - done_count;
            if (active > 0)
                fprintf(stderr, "  %s├─ %d/%d done, %d active (%.0fs)%s\n",
                        TUI_BYELLOW, done_count, grp->child_count, active,
                        now_sec_helper() - start, TUI_RESET);
        }
        if (now_sec_helper() - start >= timeout) return 0;
    }
    return 1;
}

static bool tool_swarm_map_reduce(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    /* Depth guard — the coordinator + its workers form one extra level. */
    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen,
                 "{\"error\":\"max swarm depth %d reached (current depth: %d). "
                 "Execute tasks directly instead of nesting more swarms.\"}",
                 SWARM_MAX_DEPTH, depth);
        return false;
    }

    char *name        = json_get_str(input, "name");
    char *model       = json_get_str(input, "model");
    char *coordinator = json_get_str(input, "coordinator");
    char *coord_model = json_get_str(input, "coordinator_model");
    int   timeout     = clamp_timeout_seconds(json_get_int(input, "timeout", 300),
                                              300, 5, 3600);
    double budget     = json_get_double(input, "budget", 0);

    if (!name || !name[0] || !coordinator || !coordinator[0]) {
        snprintf(result, rlen,
                 "{\"error\":\"map_reduce requires 'name', 'tasks' (array) and "
                 "'coordinator' (synthesis instruction)\"}");
        free(name); free(model); free(coordinator); free(coord_model);
        return false;
    }

    char *tasks_raw = json_get_raw(input, "tasks");
    if (!tasks_raw || *tasks_raw != '[') {
        snprintf(result, rlen, "{\"error\":\"tasks array required\"}");
        free(name); free(model); free(coordinator); free(coord_model);
        free(tasks_raw);
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
        snprintf(result, rlen, "{\"error\":\"malformed or empty tasks array\"}");
        free(name); free(model); free(coordinator); free(coord_model);
        free(tasks_raw);
        return false;
    }

    int gid = swarm_group_create(&g_swarm, name);
    if (gid < 0) {
        for (int i = 0; i < parse_ctx.count; i++) {
            free(parse_ctx.specs[i].task);
            free(parse_ctx.specs[i].model);
        }
        snprintf(result, rlen, "{\"error\":\"max groups reached\"}");
        free(name); free(model); free(coordinator); free(coord_model);
        free(tasks_raw);
        return false;
    }

    /* Wire the (previously dormant) coordinator field for status visibility. */
    snprintf(g_swarm.groups[gid].coordinator_task,
             sizeof(g_swarm.groups[gid].coordinator_task), "%s", coordinator);

    /* Optional budget partition across the whole map-reduce op. */
    if (budget > 0) swarm_set_budget(&g_swarm, budget);

    /* ── MAP: spawn workers in parallel ── */
    int spawned = 0;
    for (int i = 0; i < parse_ctx.count; i++) {
        const char *tm = (parse_ctx.specs[i].model && parse_ctx.specs[i].model[0])
            ? parse_ctx.specs[i].model : model;
        if (swarm_spawn_in_group(&g_swarm, gid, parse_ctx.specs[i].task, tm) >= 0)
            spawned++;
    }

    fprintf(stderr, "\n  %s⚡%s map-reduce \"%s\": %d workers (depth %d) → "
            "1 coordinator%s\n",
            TUI_BYELLOW, TUI_RESET, name, spawned, depth + 1, TUI_RESET);
    fprintf(stderr, "  %s┌─ map phase ─ streaming live%s\n", TUI_BYELLOW, TUI_RESET);

    /* ── BARRIER ── */
    double start = now_sec_helper();
    int wait_st = swarm_barrier_wait(gid, timeout, start);
    if (wait_st < 0) {
        swarm_collect_results(&g_swarm, gid, result, rlen, false, "interrupted");
        fprintf(stderr, "  %s└─ interrupted — workers left running%s\n\n",
                TUI_BRED, TUI_RESET);
        for (int i = 0; i < parse_ctx.count; i++) {
            free(parse_ctx.specs[i].task);
            free(parse_ctx.specs[i].model);
        }
        free(name); free(model); free(coordinator); free(coord_model);
        free(tasks_raw);
        return false;
    }
    bool map_timed_out = (wait_st == 0);
    {
        swarm_group_t *g = &g_swarm.groups[gid];
        int done = swarm_group_done_count(&g_swarm, gid);
        int errs = swarm_group_error_count(&g_swarm, gid);
        fprintf(stderr, "  %s└─ map %s: %d/%d done, %d errors (%.1fs)%s\n",
                map_timed_out ? TUI_BYELLOW : TUI_GREEN,
                map_timed_out ? "timed out (partial)" : "complete",
                done, g->child_count, errs, now_sec_helper() - start, TUI_RESET);
    }

    /* ── REDUCE: assemble a bounded synthesis prompt from worker outputs ── */
    swarm_group_t *grp = &g_swarm.groups[gid];
    jbuf_t rp;
    jbuf_init(&rp, 16384);
    jbuf_append(&rp, coordinator);
    jbuf_append(&rp, "\n\nYou are the coordinator of a worker swarm. Synthesize "
                     "the worker outputs below into a single coherent result. "
                     "Reconcile disagreements, drop dead-ends, and cite which "
                     "worker each conclusion came from.\n");
    /* Per-worker output cap keeps the prompt well under ARG_MAX (execl). */
    const size_t PER_WORKER_CAP = 6000;
    for (int i = 0; i < grp->child_count; i++) {
        swarm_child_t *c = &g_swarm.children[grp->child_ids[i]];
        char hdr[SWARM_LABEL_LEN + 64];
        snprintf(hdr, sizeof(hdr), "\n\n===== worker %d [%s] (%s) =====\n",
                 c->id, swarm_status_str(c->status), c->task);
        jbuf_append(&rp, hdr);
        const char *out = c->output ? c->output : "(no output)";
        size_t olen = strlen(out);
        if (olen > PER_WORKER_CAP) {
            jbuf_append(&rp, "[...truncated, tail follows...]\n");
            jbuf_append(&rp, out + olen - PER_WORKER_CAP);
        } else {
            jbuf_append(&rp, out);
        }
    }

    const char *cm = (coord_model && coord_model[0]) ? coord_model
                   : ((model && model[0]) ? model : NULL);
    fprintf(stderr, "  %s┌─ reduce phase ─ coordinator synthesizing%s\n",
            TUI_BYELLOW, TUI_RESET);

    int coord_id = swarm_spawn(&g_swarm, rp.data ? rp.data : coordinator, cm);
    jbuf_free(&rp);

    char *coord_out = NULL;
    if (coord_id < 0) {
        fprintf(stderr, "  %s└─ coordinator spawn failed — returning raw map%s\n\n",
                TUI_BRED, TUI_RESET);
    } else {
        /* Single-child barrier wait for the coordinator. */
        double cstart = now_sec_helper();
        swarm_live_ctx_t cctx;
        memset(&cctx, 0, sizeof(cctx));
        cctx.group_id = g_swarm.children[coord_id].group_id; /* -1: ungrouped */
        cctx.swarm = &g_swarm;
        while (1) {
            swarm_poll_stream(&g_swarm, 100, default_swarm_stream_cb, NULL);
            swarm_child_t *cc = swarm_get(&g_swarm, coord_id);
            if (!cc) break;
            if (cc->status == SWARM_DONE || cc->status == SWARM_ERROR ||
                cc->status == SWARM_KILLED)
                break;
            if (g_interrupted) break;
            if (now_sec_helper() - cstart >= timeout) {
                fprintf(stderr, "  %s⚠ coordinator timed out%s\n",
                        TUI_BYELLOW, TUI_RESET);
                break;
            }
        }
        swarm_child_t *cc = swarm_get(&g_swarm, coord_id);
        if (cc && cc->output) coord_out = cc->output;
        fprintf(stderr, "  %s└─ reduce complete (%.1fs)%s\n\n",
                TUI_GREEN, now_sec_helper() - cstart, TUI_RESET);
    }

    /* ── Build result: synthesized answer + per-worker summary ── */
    jbuf_t b;
    jbuf_init(&b, 16384);
    jbuf_append(&b, "{\"group_id\":");
    jbuf_append_int(&b, gid);
    jbuf_append(&b, ",\"name\":");
    jbuf_append_json_str(&b, name);
    jbuf_append(&b, ",\"workers\":");
    jbuf_append_int(&b, grp->child_count);
    jbuf_append(&b, ",\"map_complete\":");
    jbuf_append(&b, map_timed_out ? "false" : "true");
    jbuf_append(&b, ",\"coordinator_output\":");
    if (coord_out) {
        size_t col = strlen(coord_out);
        if (col > 16384) {
            char trunc[16448];
            snprintf(trunc, sizeof(trunc), "[...truncated %zu bytes...]\n%s",
                     col - 16384, coord_out + col - 16384);
            jbuf_append_json_str(&b, trunc);
        } else {
            jbuf_append_json_str(&b, coord_out);
        }
    } else {
        jbuf_append(&b, "null");
    }
    jbuf_append(&b, ",\"worker_results\":[");
    for (int i = 0; i < grp->child_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        swarm_child_t *c = &g_swarm.children[grp->child_ids[i]];
        jbuf_append(&b, "{\"id\":");
        jbuf_append_int(&b, c->id);
        jbuf_append(&b, ",\"task\":");
        jbuf_append_json_str(&b, c->task);
        jbuf_append(&b, ",\"status\":");
        jbuf_append_json_str(&b, swarm_status_str(c->status));
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, written);
    result[written] = '\0';
    jbuf_free(&b);

    char mr_detail[256];
    snprintf(mr_detail, sizeof(mr_detail),
             "group_id=%d name=%s workers=%d coordinator=%d depth=%d",
             gid, name, grp->child_count, coord_id, depth + 1);
    baseline_log("swarm", "map_reduce", mr_detail, NULL);

    for (int i = 0; i < parse_ctx.count; i++) {
        free(parse_ctx.specs[i].task);
        free(parse_ctx.specs[i].model);
    }
    free(name); free(model); free(coordinator); free(coord_model);
    free(tasks_raw);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * OPENROUTER LIVE MODEL REGISTRY — query & dynamic selection
 * ═══════════════════════════════════════════════════════════════════════════ */

static size_t jbuf_curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    jbuf_t *b = (jbuf_t *)userdata;
    size_t need = b->len + total + 1;
    if (need > b->cap) {
        size_t newcap = b->cap * 2;
        if (newcap < need) newcap = need;
        b->data = realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* Fetch and filter OpenRouter's /api/v1/models endpoint.
 * Supports filtering by: capability (chat/image/code), min context length,
 * max price, and text search. Returns up to `limit` models sorted by price. */
static __attribute__((unused)) bool tool_openrouter_models(const char *input, char *result, size_t rlen) {
    const char *or_key = getenv("OPENROUTER_API_KEY");
    if (!or_key || !or_key[0]) {
        snprintf(result, rlen, "{\"error\":\"OPENROUTER_API_KEY not set\"}");
        return false;
    }

    /* Parse filter params */
    char *search = json_get_str(input, "search");
    int min_ctx = json_get_int(input, "min_context", 0);
    double max_price = json_get_double(input, "max_price_per_million", 0);
    int limit = json_get_int(input, "limit", 20);
    bool free_only = json_get_bool(input, "free_only", false);
    bool chat_only = json_get_bool(input, "chat_only", true);
    if (limit <= 0) limit = 20;
    if (limit > 100) limit = 100;

    /* Fetch models list via curl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(result, rlen, "{\"error\":\"curl init failed\"}");
        free(search);
        return false;
    }

    jbuf_t resp;
    jbuf_init(&resp, 64 * 1024);

    struct curl_slist *hdrs = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", or_key);
    hdrs = curl_slist_append(hdrs, auth);

    curl_easy_setopt(curl, CURLOPT_URL, "https://openrouter.ai/api/v1/models");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)jbuf_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        snprintf(result, rlen, "{\"error\":\"HTTP %d: %s\"}",
                 (int)http_code, curl_easy_strerror(res));
        jbuf_free(&resp);
        free(search);
        return false;
    }

    /* Parse the "data" array from response */
    char *data_arr = json_get_raw(resp.data, "data");
    jbuf_free(&resp);

    if (!data_arr) {
        snprintf(result, rlen, "{\"error\":\"no data array in response\"}");
        free(search);
        return false;
    }

    /* Build filtered result — iterate JSON array elements */
    jbuf_t out;
    jbuf_init(&out, 8192);
    jbuf_append(&out, "{\"models\":[");

    int count = 0;
    int total_scanned = 0;
    const char *p = data_arr;

    /* Skip opening '[' */
    while (*p && *p != '{') p++;

    while (*p == '{' && count < limit) {
        /* Find matching closing brace (simple depth tracking) */
        int depth = 0;
        const char *start = p;
        do {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        } while (*p && depth > 0);

        /* Extract this model object */
        size_t obj_len = (size_t)(p - start);
        char *obj = safe_malloc(obj_len + 1);
        memcpy(obj, start, obj_len);
        obj[obj_len] = '\0';
        total_scanned++;

        /* Apply filters */
        bool pass = true;

        /* Context length filter */
        if (min_ctx > 0) {
            int ctx = json_get_int(obj, "context_length", 0);
            if (ctx < min_ctx) pass = false;
        }

        /* Price filter */
        if (pass && (max_price > 0 || free_only)) {
            char *pricing = json_get_raw(obj, "pricing");
            if (pricing) {
                char *prompt_price = json_get_str(pricing, "prompt");
                double pp = prompt_price ? atof(prompt_price) : 0;
                free(prompt_price);
                if (free_only && pp > 0) pass = false;
                if (max_price > 0 && pp * 1000000.0 > max_price) pass = false;
                free(pricing);
            }
        }

        /* Chat capability filter */
        if (pass && chat_only) {
            char *arch = json_get_raw(obj, "architecture");
            if (arch) {
                char *modality = json_get_str(arch, "modality");
                if (modality && !strstr(modality, "text")) pass = false;
                free(modality);
                free(arch);
            }
        }

        /* Text search filter (case-insensitive via tolower) */
        if (pass && search && search[0]) {
            char *id = json_get_str(obj, "id");
            char *name_str = json_get_str(obj, "name");
            bool found = false;
            /* Simple case-insensitive substring: lowercase both and strstr */
            char sl[128];
            size_t slen = strlen(search);
            if (slen >= sizeof(sl)) slen = sizeof(sl) - 1;
            for (size_t si = 0; si < slen; si++) sl[si] = (char)tolower((unsigned char)search[si]);
            sl[slen] = '\0';
            if (id) {
                char buf[256];
                size_t ilen = strlen(id);
                if (ilen >= sizeof(buf)) ilen = sizeof(buf) - 1;
                for (size_t si = 0; si < ilen; si++) buf[si] = (char)tolower((unsigned char)id[si]);
                buf[ilen] = '\0';
                if (strstr(buf, sl)) found = true;
            }
            if (!found && name_str) {
                char buf[256];
                size_t nlen = strlen(name_str);
                if (nlen >= sizeof(buf)) nlen = sizeof(buf) - 1;
                for (size_t si = 0; si < nlen; si++) buf[si] = (char)tolower((unsigned char)name_str[si]);
                buf[nlen] = '\0';
                if (strstr(buf, sl)) found = true;
            }
            if (!found) pass = false;
            free(id);
            free(name_str);
        }

        if (pass) {
            /* Extract compact model info */
            char *id = json_get_str(obj, "id");
            char *name = json_get_str(obj, "name");
            int ctx = json_get_int(obj, "context_length", 0);
            char *pricing = json_get_raw(obj, "pricing");
            char *prompt_p = pricing ? json_get_str(pricing, "prompt") : NULL;
            char *comp_p = pricing ? json_get_str(pricing, "completion") : NULL;
            char *top_prov = json_get_raw(obj, "top_provider");
            int max_comp = top_prov ? json_get_int(top_prov, "max_completion_tokens", 0) : 0;

            if (count > 0) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"id\":");
            jbuf_append_json_str(&out, id ? id : "");
            jbuf_append(&out, ",\"name\":");
            jbuf_append_json_str(&out, name ? name : "");
            jbuf_appendf(&out, ",\"context\":%d,\"max_output\":%d", ctx, max_comp);
            jbuf_appendf(&out, ",\"price_in\":%s,\"price_out\":%s",
                         prompt_p ? prompt_p : "\"0\"",
                         comp_p ? comp_p : "\"0\"");
            jbuf_append(&out, "}");
            count++;

            free(id); free(name); free(pricing);
            free(prompt_p); free(comp_p); free(top_prov);
        }

        free(obj);

        /* Skip comma/whitespace between objects */
        while (*p && *p != '{') p++;
    }

    jbuf_appendf(&out, "],\"count\":%d,\"total_scanned\":%d}", count, total_scanned);
    free(data_arr);
    free(search);

    int written = (int)out.len < (int)rlen - 1 ? (int)out.len : (int)rlen - 1;
    memcpy(result, out.data, written);
    result[written] = '\0';
    jbuf_free(&out);

    fprintf(stderr, "  \033[2m%d models (scanned %d)\033[0m\n", count, total_scanned);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EXTERNAL EXECUTOR TOOLS — Claude Code / Codex CLI integration
 * ═══════════════════════════════════════════════════════════════════════════ */

static executor_type_t parse_executor_type(const char *name) {
    if (!name || !name[0] || strcmp(name, "dsco") == 0 || strcmp(name, "auto") == 0)
        return EXECUTOR_DSCO;
    if (strcmp(name, "claude") == 0) return EXECUTOR_CLAUDE;
    if (strcmp(name, "codex") == 0)  return EXECUTOR_CODEX;
    return EXECUTOR_DSCO;
}

static bool tool_executor_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_swarm();
    executor_registry_t *e = &g_swarm.executors;

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"executors\":{");
    jbuf_append(&b, "\"dsco\":{\"available\":true,\"path\":");
    jbuf_append_json_str(&b, g_swarm.dsco_path ? g_swarm.dsco_path : "dsco");
    jbuf_append(&b, "}");

    jbuf_appendf(&b, ",\"claude\":{\"available\":%s", e->claude_available ? "true" : "false");
    if (e->claude_available) {
        jbuf_append(&b, ",\"path\":");
        jbuf_append_json_str(&b, e->claude_path);
        jbuf_append(&b, ",\"model\":");
        jbuf_append_json_str(&b, e->claude_model);
    }
    jbuf_append(&b, "}");

    jbuf_appendf(&b, ",\"codex\":{\"available\":%s", e->codex_available ? "true" : "false");
    if (e->codex_available) {
        jbuf_append(&b, ",\"path\":");
        jbuf_append_json_str(&b, e->codex_path);
        jbuf_append(&b, ",\"model\":");
        jbuf_append_json_str(&b, e->codex_model);
    }
    jbuf_append(&b, "}");

    /* Budget info */
    if (g_swarm.swarm_budget_usd > 0) {
        char bud[32], spent[32];
        snprintf(bud, sizeof(bud), "%.6f", g_swarm.swarm_budget_usd);
        snprintf(spent, sizeof(spent), "%.6f", g_swarm.spent_usd);
        jbuf_appendf(&b, "},\"budget\":{\"total_usd\":%s,\"spent_usd\":%s,"
                     "\"remaining_usd\":%.6f}",
                     bud, spent, swarm_budget_remaining(&g_swarm));
    } else {
        jbuf_append(&b, "},\"budget\":\"unlimited\"");
    }
    jbuf_append(&b, "}");

    int written = (int)b.len < (int)rlen - 1 ? (int)b.len : (int)rlen - 1;
    memcpy(result, b.data, written);
    result[written] = '\0';
    jbuf_free(&b);

    /* TUI feedback */
    fprintf(stderr, "  %s🔌%s executors: dsco=%s✓%s",
            TUI_BMAGENTA, TUI_RESET, TUI_GREEN, TUI_RESET);
    if (e->claude_available)
        fprintf(stderr, " claude=%s✓%s(%s)", TUI_GREEN, TUI_RESET, e->claude_model);
    else
        fprintf(stderr, " claude=%s✗%s", TUI_RED, TUI_RESET);
    if (e->codex_available)
        fprintf(stderr, " codex=%s✓%s(%s)", TUI_GREEN, TUI_RESET, e->codex_model);
    else
        fprintf(stderr, " codex=%s✗%s", TUI_RED, TUI_RESET);
    fprintf(stderr, "\n");

    return true;
}

static bool tool_spawn_executor(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen,
                 "{\"error\":\"max swarm depth %d reached\"}",
                 SWARM_MAX_DEPTH);
        return false;
    }

    char *task = json_get_str(input, "task");
    char *model = json_get_str(input, "model");
    char *exec_name = json_get_str(input, "executor");
    double budget = json_get_double(input, "budget", 0.0);

    if (!task) {
        snprintf(result, rlen, "error: task required");
        free(model); free(exec_name);
        return false;
    }

    executor_type_t exec_type = parse_executor_type(exec_name);

    /* Validate executor availability */
    if (exec_type == EXECUTOR_CLAUDE && !g_swarm.executors.claude_available) {
        snprintf(result, rlen, "{\"error\":\"claude CLI not available — "
                 "install Claude Code and sign in, or set ANTHROPIC_API_KEY\"}");
        free(task); free(model); free(exec_name);
        return false;
    }
    if (exec_type == EXECUTOR_CODEX && !g_swarm.executors.codex_available) {
        snprintf(result, rlen, "{\"error\":\"codex CLI not available — "
                 "install OpenAI Codex and authenticate with `codex auth`\"}");
        free(task); free(model); free(exec_name);
        return false;
    }

    /* Budget check before spawn */
    if (g_swarm.swarm_budget_usd > 0) {
        double remaining = swarm_budget_remaining(&g_swarm);
        double est = swarm_estimate_task_cost(&g_swarm, model ? model : g_swarm.default_model);
        if (est > remaining) {
            snprintf(result, rlen,
                     "{\"error\":\"insufficient swarm budget: estimated $%.4f > remaining $%.4f\"}",
                     est, remaining);
            free(task); free(model); free(exec_name);
            return false;
        }
    }

    int id = swarm_spawn_executor(&g_swarm, -1, task, model, exec_type);
    if (id < 0) {
        snprintf(result, rlen, "{\"error\":\"failed to spawn %s executor\"}",
                 executor_type_name(exec_type));
        free(task); free(model); free(exec_name);
        return false;
    }

    /* Set per-child budget if specified */
    swarm_child_t *c = swarm_get(&g_swarm, id);
    if (budget > 0) c->budget_usd = budget;

    /* Pre-estimate cost */
    c->est_cost_usd = swarm_estimate_task_cost(&g_swarm,
        model ? model : (exec_type == EXECUTOR_CLAUDE ? g_swarm.executors.claude_model
                                                       : g_swarm.executors.codex_model));

    snprintf(result, rlen,
             "{\"agent_id\":%d,\"pid\":%d,\"executor\":\"%s\",\"model\":\"%s\","
             "\"est_cost_usd\":%.6f,\"status\":\"running\","
             "\"hint\":\"Use agent_status to monitor\"}",
             id, (int)c->pid, executor_type_name(exec_type), c->model,
             c->est_cost_usd);

    fprintf(stderr, "  %s⚡%s spawned %s%s%s agent #%d: %s%.60s%s\n",
            TUI_BCYAN, TUI_RESET,
            TUI_BOLD, executor_type_name(exec_type), TUI_RESET,
            id, TUI_DIM, task, TUI_RESET);

    free(task); free(model); free(exec_name);
    return true;
}

/* spawn_provider — spawn a dsco sub-agent forced to a specific native API provider.
 * This is the dynamic decoupling primitive: parent on Anthropic can spawn children
 * on OpenAI, Groq, DeepSeek, etc. Each child is a full dsco instance routed through
 * that provider's API. */
static bool tool_spawn_provider(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen, "{\"error\":\"max swarm depth %d reached\"}", SWARM_MAX_DEPTH);
        return false;
    }

    char *task = json_get_str(input, "task");
    char *model = json_get_str(input, "model");
    char *provider = json_get_str(input, "provider");
    double budget = json_get_double(input, "budget", 0.0);

    if (!task || !provider) {
        snprintf(result, rlen, "{\"error\":\"task and provider are required\"}");
        free(task); free(model); free(provider);
        return false;
    }

    /* Validate provider has an API key */
    const char *pkey = provider_resolve_api_key(provider);
    if (!pkey || !pkey[0]) {
        snprintf(result, rlen,
                 "{\"error\":\"no API key for provider '%s' — set the env var\"}", provider);
        free(task); free(model); free(provider);
        return false;
    }

    /* Budget check */
    if (g_swarm.swarm_budget_usd > 0) {
        double remaining = swarm_budget_remaining(&g_swarm);
        double est = swarm_estimate_task_cost(&g_swarm, model ? model : g_swarm.default_model);
        if (est > remaining) {
            snprintf(result, rlen,
                     "{\"error\":\"insufficient budget: est $%.4f > remaining $%.4f\"}",
                     est, remaining);
            free(task); free(model); free(provider);
            return false;
        }
    }

    int id = swarm_spawn_provider(&g_swarm, -1, task, model, provider);
    if (id < 0) {
        snprintf(result, rlen, "{\"error\":\"spawn failed for provider '%s'\"}", provider);
        free(task); free(model); free(provider);
        return false;
    }

    swarm_child_t *c = swarm_get(&g_swarm, id);
    if (budget > 0) c->budget_usd = budget;
    c->est_cost_usd = swarm_estimate_task_cost(&g_swarm,
        model ? model : g_swarm.default_model);

    snprintf(result, rlen,
             "{\"agent_id\":%d,\"pid\":%d,\"provider\":\"%s\",\"model\":\"%s\","
             "\"est_cost_usd\":%.6f,\"status\":\"running\","
             "\"hint\":\"Use agent_status to monitor\"}",
             id, (int)c->pid, provider, c->model, c->est_cost_usd);

    fprintf(stderr, "  %s⚡%s spawned %s%s%s→%s%s%s agent #%d: %s%.60s%s\n",
            TUI_BCYAN, TUI_RESET,
            TUI_BOLD, "dsco", TUI_RESET,
            TUI_BGREEN, provider, TUI_RESET,
            id, TUI_DIM, task, TUI_RESET);

    free(task); free(model); free(provider);
    return true;
}

static bool tool_create_executor_swarm(const char *input, char *result, size_t rlen) {
    ensure_swarm();

    int depth = current_swarm_depth();
    if (depth >= SWARM_MAX_DEPTH) {
        snprintf(result, rlen, "{\"error\":\"max swarm depth reached\"}");
        return false;
    }

    char *name = json_get_str(input, "name");
    char *default_executor = json_get_str(input, "executor");
    char *default_model = json_get_str(input, "model");
    double total_budget = json_get_double(input, "budget", 0.0);

    if (!name) {
        snprintf(result, rlen, "error: name required");
        free(default_executor); free(default_model);
        return false;
    }

    int gid = swarm_group_create(&g_swarm, name);
    if (gid < 0) {
        snprintf(result, rlen, "{\"error\":\"max groups reached\"}");
        free(name); free(default_executor); free(default_model);
        return false;
    }

    /* Parse tasks array — each task can specify its own executor and model */
    char *tasks_raw = json_get_raw(input, "tasks");
    if (!tasks_raw || *tasks_raw != '[') {
        snprintf(result, rlen, "error: tasks array required");
        free(name); free(default_executor); free(default_model); free(tasks_raw);
        return false;
    }

    /* Simple JSON array parsing — tasks can be strings or objects */
    swarm_task_parse_ctx_t parse_ctx;
    memset(&parse_ctx, 0, sizeof(parse_ctx));
    json_array_foreach(input, "tasks", parse_swarm_task_element, &parse_ctx);

    if (parse_ctx.parse_error || parse_ctx.count == 0) {
        for (int i = 0; i < parse_ctx.count; i++) {
            free(parse_ctx.specs[i].task);
            free(parse_ctx.specs[i].model);
        }
        free(tasks_raw); free(name); free(default_executor); free(default_model);
        snprintf(result, rlen, "error: malformed or empty tasks array");
        return false;
    }

    /* Compute per-child budget partition */
    double per_child_budget = 0;
    if (total_budget > 0) {
        per_child_budget = total_budget / (double)parse_ctx.count;
    }

    int spawned = 0;
    for (int i = 0; i < parse_ctx.count; i++) {
        const char *task_model = (parse_ctx.specs[i].model && parse_ctx.specs[i].model[0])
            ? parse_ctx.specs[i].model : default_model;

        /* Check if task object has an executor field (stored in model as "exec:name" hack) */
        executor_type_t exec_type = parse_executor_type(default_executor);

        /* Try to extract executor from the task spec — check for "executor" key in the raw JSON */
        /* For now use the default executor */

        int cid = swarm_spawn_executor(&g_swarm, gid, parse_ctx.specs[i].task,
                                        task_model, exec_type);
        if (cid >= 0) {
            swarm_child_t *c = swarm_get(&g_swarm, cid);
            if (per_child_budget > 0) c->budget_usd = per_child_budget;
            c->est_cost_usd = swarm_estimate_task_cost(&g_swarm,
                task_model ? task_model : g_swarm.default_model);
            spawned++;
        }
    }

    /* TUI feedback */
    fprintf(stderr, "\n  %s⚡%s Swarm %s\"%s\"%s (%s): %d agents\n",
            TUI_BYELLOW, TUI_RESET, TUI_BOLD, name, TUI_RESET,
            executor_type_name(parse_executor_type(default_executor)), spawned);
    for (int i = 0; i < parse_ctx.count; i++) {
        fprintf(stderr, "    %s◉%s %s%.60s%s\n",
                TUI_BCYAN, TUI_RESET, TUI_DIM, parse_ctx.specs[i].task, TUI_RESET);
    }
    if (total_budget > 0) {
        fprintf(stderr, "    %s$%s budget: $%.4f total ($%.4f/agent)\n",
                TUI_BYELLOW, TUI_RESET, total_budget, per_child_budget);
    }
    fprintf(stderr, "\n");

    /* Build result */
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"group_id\":");
    jbuf_append_int(&b, gid);
    jbuf_append(&b, ",\"name\":");
    jbuf_append_json_str(&b, name);
    jbuf_append(&b, ",\"executor\":");
    jbuf_append_json_str(&b, executor_type_name(parse_executor_type(default_executor)));
    jbuf_append(&b, ",\"agents_spawned\":");
    jbuf_append_int(&b, spawned);
    if (total_budget > 0) {
        char bud[32];
        snprintf(bud, sizeof(bud), "%.6f", total_budget);
        jbuf_append(&b, ",\"budget_usd\":");
        jbuf_append(&b, bud);
    }
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
    free(tasks_raw); free(name); free(default_executor); free(default_model);
    return true;
}

static bool tool_swarm_budget(const char *input, char *result, size_t rlen) {
    ensure_swarm();
    double budget = json_get_double(input, "budget_usd", -1.0);
    if (budget >= 0) {
        swarm_set_budget(&g_swarm, budget);
        snprintf(result, rlen,
                 "{\"budget_usd\":%.6f,\"remaining_usd\":%.6f,\"status\":\"set\"}",
                 budget, swarm_budget_remaining(&g_swarm));
        fprintf(stderr, "  %s💰%s swarm budget set: $%.4f\n",
                TUI_BYELLOW, TUI_RESET, budget);
    } else {
        double remaining = swarm_budget_remaining(&g_swarm);
        snprintf(result, rlen,
                 "{\"budget_usd\":%.6f,\"spent_usd\":%.6f,\"remaining_usd\":%.6f,"
                 "\"children\":%d,\"active\":%d}",
                 g_swarm.swarm_budget_usd, g_swarm.spent_usd, remaining,
                 g_swarm.child_count, swarm_active_count(&g_swarm));
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
    char *file = path_normalize(json_get_str(input, "file"));
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
    char *file = path_normalize(json_get_str(input, "file"));
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
    if (!text) text = json_get_str(input, "text");
    if (!text) text = json_get_str(input, "data");
    char *action = json_get_str(input, "action");
    bool url_safe = json_get_bool(input, "url_safe", false);

    if (!text) {
        snprintf(result, rlen, "error: input required");
        free(action);
        return false;
    }

    if (action && strcmp(action, "decode") == 0) {
        if (!base64_decode_result(text, url_safe, result, rlen)) {
            free(text); free(action);
            return false;
        }
    } else {
        if (!base64_encode_result((const uint8_t *)text, strlen(text), url_safe, result, rlen)) {
            free(text); free(action);
            return false;
        }
    }
    free(text); free(action);
    return true;
}

static __attribute__((unused)) bool tool_jwt_decode(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_hkdf(const char *input, char *result, size_t rlen) {
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
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_big_factorial(const char *input, char *result, size_t rlen) {
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
                     "use context_get_batch to retrieve multiple chunks in one call");
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

/* ── context_get_batch: fetch multiple chunks in one call ─────────── */
static bool tool_context_get_batch(const char *input, char *result, size_t rlen) {
    /* Parse chunk_ids array from JSON: {"chunk_ids":[1,2,3]} */
    const char *arr = strstr(input, "\"chunk_ids\"");
    if (!arr) {
        snprintf(result, rlen, "error: chunk_ids array required");
        return false;
    }
    const char *bracket = strchr(arr, '[');
    if (!bracket) {
        snprintf(result, rlen, "error: chunk_ids must be a JSON array");
        return false;
    }

    int ids[64];
    int id_count = 0;
    const char *p = bracket + 1;
    while (*p && *p != ']' && id_count < 64) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        ids[id_count++] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
    if (id_count == 0) {
        snprintf(result, rlen, "error: chunk_ids array is empty");
        return false;
    }

    int max_chars_each = json_get_int(input, "max_chars_each", 2000);
    if (max_chars_each < 200) max_chars_each = 200;
    if (max_chars_each > 8000) max_chars_each = 8000;

    size_t off = 0;
    int found = 0, missing = 0;
    for (int i = 0; i < id_count && off + 128 < rlen; i++) {
        int idx = ctx_find_index_by_id(ids[i]);
        if (idx < 0) {
            missing++;
            continue;
        }
        ctx_chunk_t *c = &g_ctx.chunks[idx];
        size_t copy_len = c->text_len;
        if (copy_len > (size_t)max_chars_each) copy_len = (size_t)max_chars_each;

        int n = snprintf(result + off, rlen - off,
                         "%s[chunk_id=%d tool=%s bytes=%zu]\n",
                         found > 0 ? "\n---\n" : "", c->id, c->tool, c->text_len);
        if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;

        if (off + copy_len + 64 < rlen) {
            memcpy(result + off, c->text, copy_len);
            off += copy_len;
            result[off] = '\0';
            if (copy_len < c->text_len) {
                n = snprintf(result + off, rlen - off,
                             "\n(truncated %zu/%zu)", copy_len, c->text_len);
                if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
            }
        }
        found++;
    }
    if (found == 0) {
        snprintf(result, rlen, "error: none of %d chunk_ids found (likely evicted)", id_count);
        return false;
    }
    int n = snprintf(result + off, rlen - off,
                     "\n\n--- batch: %d/%d chunks returned, %d missing ---",
                     found, id_count, missing);
    if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
    result[off] = '\0';
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
             "\nUse context_get only if you need verbatim details from the cited chunk ids.");
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

/* ── Phase 2c: VFS-backed context_recall tool ────────────────────────── */

static bool tool_context_recall(const char *input, char *result, size_t rlen) {
    char *key = json_get_str(input, "key");
    int list_mode = json_get_int(input, "list", 0);

    if (list_mode) {
        /* List available persisted results */
        if (!g_tools_vfs) {
            snprintf(result, rlen, "error: VFS not initialized");
            free(key);
            return false;
        }
        int count = 0;
        char **keys = vfs_result_list(g_tools_vfs, &count);
        if (!keys || count == 0) {
            snprintf(result, rlen, "no persisted tool results available");
            free(key);
            free(keys);
            return true;
        }
        size_t off = 0;
        int n = snprintf(result, rlen, "persisted_results count=%d\n", count);
        if (n > 0 && (size_t)n < rlen) off = (size_t)n;
        for (int i = 0; i < count && off + 64 < rlen; i++) {
            n = snprintf(result + off, rlen - off, "  %s\n", keys[i]);
            if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
            free(keys[i]);
        }
        free(keys);
        free(key);
        return true;
    }

    if (!key || !*key) {
        /* "No args = list keys" — auto-list when called without key */
        free(key);
        key = NULL;
        if (!g_tools_vfs) {
            snprintf(result, rlen, "no persisted results (VFS not initialized)");
            return true;
        }
        int count = 0;
        char **keys = vfs_result_list(g_tools_vfs, &count);
        if (!keys || count == 0) {
            snprintf(result, rlen, "no persisted tool results available");
            free(keys);
            return true;
        }
        size_t off = 0;
        int n = snprintf(result, rlen, "persisted_results count=%d\n", count);
        if (n > 0 && (size_t)n < rlen) off = (size_t)n;
        for (int i = 0; i < count && off + 64 < rlen; i++) {
            n = snprintf(result + off, rlen - off, "  %s\n", keys[i]);
            if (n > 0 && (size_t)n < rlen - off) off += (size_t)n;
            free(keys[i]);
        }
        free(keys);
        return true;
    }

    if (!g_tools_vfs) {
        snprintf(result, rlen, "error: VFS not initialized");
        free(key);
        return false;
    }

    char *full_result = vfs_result_get(g_tools_vfs, key);
    if (!full_result) {
        snprintf(result, rlen, "error: key '%s' not found or expired", key);
        free(key);
        return false;
    }

    size_t full_len = strlen(full_result);
    if (full_len + 64 < rlen) {
        snprintf(result, rlen, "[key=%s len=%zu]\n%s", key, full_len, full_result);
    } else {
        /* Truncate if even the full result is huge for the buffer */
        snprintf(result, rlen, "[key=%s len=%zu (buffer-truncated to %zu)]\n",
                 key, full_len, rlen - 128);
        size_t off = strlen(result);
        size_t copy = rlen - off - 1;
        if (copy > full_len) copy = full_len;
        memcpy(result + off, full_result, copy);
        result[off + copy] = '\0';
    }

    free(full_result);
    free(key);
    return true;
}

static bool tool_context_pack(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *tool_filter = json_get_str(input, "tool");
    char *facet = json_get_str(input, "facet");
    int source_id = json_get_int(input, "source_id", -1);
    int top_k = json_get_int(input, "top_k", 8);
    /* Scale pack budget with context window: use ~3% of context as chars.
     * 200K tokens → ~24K chars budget; 1M tokens → ~120K, capped at 48K.
     * Fallback: 8000 chars total, 1600 per chunk. */
    int default_total = 8000;
    int default_per   = 1600;
    if (g_ctx_window_tokens > 0) {
        default_total = g_ctx_window_tokens * 4 / 35;  /* ~3% of ctx bytes */
        if (default_total < 4000)  default_total = 4000;
        if (default_total > 48000) default_total = 48000;
        default_per = default_total / 5;
        if (default_per < 800)  default_per = 800;
        if (default_per > 8000) default_per = 8000;
    }
    int max_total = json_get_int(input, "max_chars_total", default_total);
    int max_per = json_get_int(input, "max_chars_per_chunk", default_per);

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

        char preview[8200];
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
             "answer from this evidence. Do not call context_get individually.",
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
             "\nnext: context_pack on the fused hits above; use context_get only for specific verbatim spans");
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
    WF_STEP_BLOCKED,
    WF_STEP_FAILED
} wf_step_status_t;

typedef struct {
    int id;
    char name[96];
    int step_count;
    char steps[WF_MAX_STEPS][WF_STEP_TEXT_MAX];
    wf_step_status_t status[WF_MAX_STEPS];
    char notes[WF_MAX_STEPS][WF_NOTE_MAX];
    char business_key[96];
    char contract_version[32];
    char root_cause[64];
    int retry_count[WF_MAX_STEPS];
    int max_retries;
    time_t created_at;
    time_t updated_at;
    time_t heartbeat_at;
    bool dead_lettered;
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

static wf_plan_t *wf_find_business_key(const char *business_key) {
    if (!business_key || !*business_key) return NULL;
    for (int i = 0; i < WF_MAX_PLANS; i++) {
        if (g_wf_plans[i].active &&
            strcmp(g_wf_plans[i].business_key, business_key) == 0) {
            return &g_wf_plans[i];
        }
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
            g_wf_plans[i].updated_at = g_wf_plans[i].created_at;
            g_wf_plans[i].heartbeat_at = g_wf_plans[i].created_at;
            g_wf_plans[i].max_retries = 3;
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
        case WF_STEP_FAILED: return "failed";
        default: return "unknown";
    }
}

static wf_step_status_t wf_parse_status(const char *s) {
    if (!s) return WF_STEP_PENDING;
    if (strcmp(s, "pending") == 0) return WF_STEP_PENDING;
    if (strcmp(s, "in_progress") == 0 || strcmp(s, "inprogress") == 0) return WF_STEP_IN_PROGRESS;
    if (strcmp(s, "done") == 0 || strcmp(s, "complete") == 0) return WF_STEP_DONE;
    if (strcmp(s, "blocked") == 0) return WF_STEP_BLOCKED;
    if (strcmp(s, "failed") == 0 || strcmp(s, "failure") == 0) return WF_STEP_FAILED;
    return WF_STEP_PENDING;
}

static void wf_format_time(time_t t, char *buf, size_t len) {
    if (!buf || len == 0) return;
    struct tm tmv;
    if (t <= 0 || !gmtime_r(&t, &tmv)) {
        snprintf(buf, len, "unknown");
        return;
    }
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int wf_done_count(const wf_plan_t *wf) {
    int done = 0;
    if (!wf) return 0;
    for (int i = 0; i < wf->step_count; i++) {
        if (wf->status[i] == WF_STEP_DONE) done++;
    }
    return done;
}

static int wf_failed_count(const wf_plan_t *wf) {
    int failed = 0;
    if (!wf) return 0;
    for (int i = 0; i < wf->step_count; i++) {
        if (wf->status[i] == WF_STEP_FAILED || wf->status[i] == WF_STEP_BLOCKED)
            failed++;
    }
    return failed;
}

static bool tool_workflow_plan(const char *input, char *result, size_t rlen) {
    char *name = json_get_str(input, "name");
    char *steps_raw = json_get_str(input, "steps");
    char *business_key = json_get_str(input, "business_key");
    char *contract_version = json_get_str(input, "contract_version");
    int max_retries = json_get_int(input, "max_retries", 3);
    if (!steps_raw || !*steps_raw) {
        snprintf(result, rlen, "error: steps required (newline/semicolon separated)");
        free(name);
        free(steps_raw);
        free(business_key);
        free(contract_version);
        return false;
    }
    if (business_key && *business_key) {
        wf_plan_t *existing = wf_find_business_key(business_key);
        if (existing) {
            snprintf(result, rlen,
                     "error: duplicate workflow business_key=%s existing_id=%d status=%d/%d",
                     business_key, existing->id, wf_done_count(existing), existing->step_count);
            free(name);
            free(steps_raw);
            free(business_key);
            free(contract_version);
            return false;
        }
    }

    wf_plan_t *wf = wf_alloc();
    if (!wf) {
        snprintf(result, rlen, "error: workflow capacity reached (%d)", WF_MAX_PLANS);
        free(name);
        free(steps_raw);
        free(business_key);
        free(contract_version);
        return false;
    }
    strncpy(wf->name, (name && *name) ? name : "workflow", sizeof(wf->name) - 1);
    if (business_key && *business_key) {
        strncpy(wf->business_key, business_key, sizeof(wf->business_key) - 1);
        wf->business_key[sizeof(wf->business_key) - 1] = '\0';
    }
    strncpy(wf->contract_version,
            (contract_version && *contract_version) ? contract_version : "v1",
            sizeof(wf->contract_version) - 1);
    wf->contract_version[sizeof(wf->contract_version) - 1] = '\0';
    if (max_retries < 0) max_retries = 0;
    if (max_retries > 25) max_retries = 25;
    wf->max_retries = max_retries;
    wf->step_count = wf_parse_steps(steps_raw, wf->steps);
    for (int i = 0; i < wf->step_count; i++) wf->status[i] = WF_STEP_PENDING;

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "workflow created id=%d name=%s steps=%d contract=%s business_key=%s max_retries=%d\n",
                     wf->id, wf->name, wf->step_count, wf->contract_version,
                     wf->business_key[0] ? wf->business_key : "-", wf->max_retries);
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
    free(business_key);
    free(contract_version);
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
        char created[32], updated[32], heartbeat[32];
        wf_format_time(wf->created_at, created, sizeof(created));
        wf_format_time(wf->updated_at, updated, sizeof(updated));
        wf_format_time(wf->heartbeat_at, heartbeat, sizeof(heartbeat));
        int n = snprintf(result + off, rlen - off,
                         "workflow id=%d name=%s steps=%d progress=%d/%d contract=%s business_key=%s dead_lettered=%s root_cause=%s created=%s updated=%s heartbeat=%s\n",
                         wf->id, wf->name, wf->step_count, wf_done_count(wf), wf->step_count,
                         wf->contract_version[0] ? wf->contract_version : "v1",
                         wf->business_key[0] ? wf->business_key : "-",
                         wf->dead_lettered ? "true" : "false",
                         wf->root_cause[0] ? wf->root_cause : "-",
                         created, updated, heartbeat);
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
        int n = snprintf(result + off, rlen - off,
                         "id=%d name=%s progress=%d/%d failed=%d contract=%s business_key=%s dead_lettered=%s\n",
                         g_wf_plans[i].id, g_wf_plans[i].name,
                         wf_done_count(&g_wf_plans[i]), g_wf_plans[i].step_count,
                         wf_failed_count(&g_wf_plans[i]),
                         g_wf_plans[i].contract_version[0] ? g_wf_plans[i].contract_version : "v1",
                         g_wf_plans[i].business_key[0] ? g_wf_plans[i].business_key : "-",
                         g_wf_plans[i].dead_lettered ? "true" : "false");
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
    char *root_cause = json_get_str(input, "root_cause");
    bool non_retryable = json_get_bool(input, "non_retryable", false);
    if (id < 0 || step < 1) {
        snprintf(result, rlen, "error: id and step (1-based) required");
        free(status);
        free(note);
        free(root_cause);
        return false;
    }
    wf_plan_t *wf = wf_find(id);
    if (!wf) {
        snprintf(result, rlen, "error: workflow id %d not found", id);
        free(status);
        free(note);
        free(root_cause);
        return false;
    }
    if (step > wf->step_count) {
        snprintf(result, rlen, "error: step out of range (1-%d)", wf->step_count);
        free(status);
        free(note);
        free(root_cause);
        return false;
    }
    int idx = step - 1;
    wf_step_status_t next = wf_parse_status(status);
    wf->status[idx] = next;
    if (note) {
        strncpy(wf->notes[idx], note, WF_NOTE_MAX - 1);
        wf->notes[idx][WF_NOTE_MAX - 1] = '\0';
    }
    if (root_cause && *root_cause) {
        strncpy(wf->root_cause, root_cause, sizeof(wf->root_cause) - 1);
        wf->root_cause[sizeof(wf->root_cause) - 1] = '\0';
    }
    if (next == WF_STEP_FAILED) {
        wf->retry_count[idx]++;
        if (non_retryable || wf->retry_count[idx] > wf->max_retries) {
            wf->dead_lettered = true;
        }
    }
    wf->updated_at = time(NULL);
    snprintf(result, rlen,
             "workflow %d step %d set to %s retry=%d/%d dead_lettered=%s root_cause=%s",
             id, step, wf_status_name(wf->status[idx]), wf->retry_count[idx],
             wf->max_retries, wf->dead_lettered ? "true" : "false",
             wf->root_cause[0] ? wf->root_cause : "-");
    free(status);
    free(note);
    free(root_cause);
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

static bool tool_workflow_heartbeat(const char *input, char *result, size_t rlen) {
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
    wf->heartbeat_at = time(NULL);
    wf->updated_at = wf->heartbeat_at;
    char heartbeat[32];
    wf_format_time(wf->heartbeat_at, heartbeat, sizeof(heartbeat));
    snprintf(result, rlen, "workflow %d heartbeat=%s liveness=ok", wf->id, heartbeat);
    return true;
}

static bool tool_workflow_dead_letter(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    size_t off = 0;
    int listed = 0;
    for (int i = 0; i < WF_MAX_PLANS && off < rlen - 80; i++) {
        wf_plan_t *wf = &g_wf_plans[i];
        if (!wf->active) continue;
        if (id >= 0 && wf->id != id) continue;
        if (!wf->dead_lettered && id < 0) continue;
        int n = snprintf(result + off, rlen - off,
                         "dead_letter id=%d name=%s business_key=%s root_cause=%s failed_steps=%d\n",
                         wf->id, wf->name,
                         wf->business_key[0] ? wf->business_key : "-",
                         wf->root_cause[0] ? wf->root_cause : "-",
                         wf_failed_count(wf));
        if (n < 0 || (size_t)n >= rlen - off) break;
        off += (size_t)n;
        listed++;
    }
    if (listed == 0) snprintf(result, rlen, "no dead-lettered workflows");
    return true;
}

static bool tool_workflow_reprocess(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    char *business_key = json_get_str(input, "business_key");
    wf_plan_t *wf = id >= 0 ? wf_find(id) : wf_find_business_key(business_key);
    if (!wf) {
        snprintf(result, rlen, "error: workflow not found");
        free(business_key);
        return false;
    }
    int reset = 0;
    for (int i = 0; i < wf->step_count; i++) {
        if (wf->status[i] == WF_STEP_FAILED || wf->status[i] == WF_STEP_BLOCKED) {
            wf->status[i] = WF_STEP_PENDING;
            wf->retry_count[i] = 0;
            reset++;
        }
    }
    wf->dead_lettered = false;
    wf->root_cause[0] = '\0';
    wf->updated_at = time(NULL);
    snprintf(result, rlen, "workflow %d reprocess reset_steps=%d business_key=%s",
             wf->id, reset, wf->business_key[0] ? wf->business_key : "-");
    free(business_key);
    return true;
}

static bool tool_workflow_validate(const char *input, char *result, size_t rlen) {
    int id = json_get_int(input, "id", -1);
    char *steps_raw = json_get_str(input, "steps");
    char tmp_steps[WF_MAX_STEPS][WF_STEP_TEXT_MAX];
    int step_count = 0;
    const char *contract = "v1";

    if (id >= 0) {
        wf_plan_t *wf = wf_find(id);
        if (!wf) {
            snprintf(result, rlen, "error: workflow id %d not found", id);
            free(steps_raw);
            return false;
        }
        step_count = wf->step_count;
        contract = wf->contract_version[0] ? wf->contract_version : "v1";
    } else {
        step_count = wf_parse_steps(steps_raw, tmp_steps);
    }

    if (step_count <= 0) {
        snprintf(result, rlen, "workflow validation failed: no steps");
        free(steps_raw);
        return false;
    }
    if (step_count > WF_MAX_STEPS) {
        snprintf(result, rlen, "workflow validation failed: too many steps");
        free(steps_raw);
        return false;
    }
    snprintf(result, rlen,
             "workflow validation ok steps=%d contract=%s schema=input_output:v1 retries=deterministic dead_letter=enabled",
             step_count, contract);
    free(steps_raw);
    return true;
}

static bool tool_workflow_smoke(const char *input, char *result, size_t rlen) {
    (void)input;
    int active = 0, dead = 0, invalid = 0;
    for (int i = 0; i < WF_MAX_PLANS; i++) {
        wf_plan_t *wf = &g_wf_plans[i];
        if (!wf->active) continue;
        active++;
        if (wf->dead_lettered) dead++;
        if (wf->step_count <= 0 || !wf->contract_version[0]) invalid++;
    }
    snprintf(result, rlen,
             "workflow smoke ok active=%d dead_lettered=%d invalid=%d contracts=pinned liveness=heartbeat_supported dedupe=business_key",
             active, dead, invalid);
    return invalid == 0;
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

    /* Persist to VFS instead of chunk store */
    char vfs_key_raw[128] = "", vfs_key_vis[128] = "", vfs_key_out[128] = "";
    if (g_tools_vfs) {
        char url_hash[65];
        sha256_hex((const uint8_t *)url, strlen(url), url_hash);
        snprintf(vfs_key_raw, sizeof(vfs_key_raw), "browser_raw:%.16s", url_hash);
        snprintf(vfs_key_vis, sizeof(vfs_key_vis), "browser_vis:%.16s", url_hash);
        snprintf(vfs_key_out, sizeof(vfs_key_out), "browser_out:%.16s", url_hash);
        vfs_result_put(g_tools_vfs, "browser_snapshot", url_hash, raw_doc, 7200);
        vfs_result_put(g_tools_vfs, "browser_snapshot", url_hash, visual_doc, 7200);
        vfs_result_put(g_tools_vfs, "browser_snapshot", url_hash, outline_doc, 7200);
    }

    /* Also ingest into chunk store for legacy browser_extract compatibility */
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
             "vfs_keys: raw=%s visual=%s outline=%s\n"
             "indexed:\n"
             "  raw: chunks=%d range=%d-%d bytes=%zu\n"
             "  visual: chunks=%d range=%d-%d bytes=%zu\n"
             "  outline: chunks=%d range=%d-%d bytes=%zu\n"
             "visual_preview=%s\n"
             "next:\n"
             "  browser_viewport {\"snapshot_id\":%d,\"offset\":1,\"lines\":30}\n"
             "  context_recall {\"key\":\"%s\"}\n"
             "  browser_extract {\"query\":\"...\",\"source_id\":%d,\"facet\":\"visual\",\"top_k\":5}",
             url,
             snap->id,
             title[0] ? title : "(none)",
             fetch_meta[0] ? fetch_meta : "unknown",
             vfs_key_raw, vfs_key_vis, vfs_key_out,
             info_raw.chunks_added, info_raw.first_chunk_id, info_raw.last_chunk_id, info_raw.bytes_added,
             info_visual.chunks_added, info_visual.first_chunk_id, info_visual.last_chunk_id, info_visual.bytes_added,
             info_outline.chunks_added, info_outline.first_chunk_id, info_outline.last_chunk_id, info_outline.bytes_added,
             preview[0] ? preview : "(empty)",
             snap->id, vfs_key_vis, snap->id);

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
                 "next: prefer context_pack with the same query/filter before calling context_get on individual chunk_ids");
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

    /* Persist to VFS */
    char vfs_key_probe[128] = "";
    if (g_tools_vfs) {
        char url_hash[65];
        sha256_hex((const uint8_t *)url, strlen(url), url_hash);
        snprintf(vfs_key_probe, sizeof(vfs_key_probe), "research_probe:%.16s", url_hash);
        vfs_result_put(g_tools_vfs, "research_probe", url_hash, html, 3600);
    }

    ctx_ingest_info_t info;
    ctx_ingest_text("research_probe", html, &info);

    size_t off = 0;
    int n = snprintf(result + off, rlen - off,
                     "research_probe url=%s fetch_meta=%s vfs_key=%s stored_chunks=%d chunk_id_range=%d-%d bytes_indexed=%zu\n",
                     url, fetch_meta[0] ? fetch_meta : "unknown",
                     vfs_key_probe[0] ? vfs_key_probe : "none",
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

static __attribute__((unused)) bool tool_research_compare(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_code_index(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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

static __attribute__((unused)) bool tool_privacy_filter(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_secret_scan(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = path_normalize(json_get_str(input, "file"));
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

static __attribute__((unused)) bool tool_risk_gate(const char *input, char *result, size_t rlen) {
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

static __attribute__((unused)) bool tool_plugin_list(const char *input, char *result, size_t rlen) {
    (void)input;
    plugin_list(&g_plugins, result, rlen);
    return true;
}

static void tool_map_rebuild(void);  /* forward decl */
static int build_compact_params(const char *schema, char *out, size_t outlen);  /* forward decl */

static __attribute__((unused)) bool tool_plugin_reload(const char *input, char *result, size_t rlen) {
    (void)input;
    plugin_reload(&g_plugins);
    tool_map_rebuild();
    snprintf(result, rlen, "Plugins reloaded. %d plugins loaded, %d extra tools.",
             g_plugins.count, g_plugins.extra_tool_count);
    return true;
}

static __attribute__((unused)) bool tool_plugin_load_file(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
    if (!path) {
        snprintf(result, rlen, "error: path required");
        return false;
    }
    bool ok = plugin_load(&g_plugins, path);
    if (ok) {
        tool_map_rebuild();
        snprintf(result, rlen, "Plugin loaded: %s (%d tools)",
                 g_plugins.plugins[g_plugins.count - 1].name,
                 g_plugins.plugins[g_plugins.count - 1].tool_count);
    } else
        snprintf(result, rlen, "error: failed to load plugin from %s", path);
    free(path);
    return ok;
}

static __attribute__((unused)) bool tool_plugin_validate(const char *input, char *result, size_t rlen) {
    char *manifest_path = path_normalize(json_get_str(input, "manifest_path"));
    char *lock_path = path_normalize(json_get_str(input, "lock_path"));
    bool ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, result, rlen);
    free(manifest_path);
    free(lock_path);
    return ok;
}

/* ── View Image (base64 encode for vision) ─────────────────────────────── */

static __attribute__((unused)) bool tool_view_image(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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

static __attribute__((unused)) bool tool_view_pdf(const char *input, char *result, size_t rlen) {
    char *path = path_normalize(json_get_str(input, "path"));
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
static __attribute__((unused)) bool tool_csv_parse(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = path_normalize(json_get_str(input, "file"));
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
static __attribute__((unused)) bool tool_regex_match(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_template_render(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_text_diff(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_process_tree(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_system_profiler(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_string_ops(const char *input, char *result, size_t rlen) {
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
static __attribute__((unused)) bool tool_xml_extract(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    char *file = path_normalize(json_get_str(input, "file"));
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Wings + Talons Tool Implementations
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Global governance engine — singleton for this process */
static governance_engine_t g_governance;  /* immune system */
static memory_store_t g_memory;           /* wings: cognitive memory */
static talons_engine_t g_talons;          /* talons: competitive execution */
static bool g_wt_initialized = false;

static void ensure_wt_init(void) {
    if (!g_wt_initialized) {
        governance_init(&g_governance);
        memory_store_init(&g_memory);
        talons_init(&g_talons);
        /* Register self as agent */
        const char *self_id = getenv("DSCO_AGENT_ID");
        if (!self_id) self_id = "root";
        governance_register_agent(&g_governance, self_id, PRINCIPAL_TIER_2, 10000);
        g_wt_initialized = true;
    }
}

/* ── Pheromone Tools ──────────────────────────────────────────────────── */

static bool tool_pheromone_deposit(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *type_s = json_get_str(input,"type");
    char *region = json_get_str(input,"region");
    char *source = json_get_str(input,"source");
    char *meta = json_get_str(input,"meta");
    double concentration = json_get_double(input, "concentration", 0);
    if (concentration <= 0) concentration = 0.5;

    pheromone_type_t type = PHERO_PROGRESS;
    if (type_s) {
        if (strcasecmp(type_s, "attraction") == 0) type = PHERO_ATTRACTION;
        else if (strcasecmp(type_s, "warning") == 0) type = PHERO_WARNING;
        else if (strcasecmp(type_s, "success") == 0) type = PHERO_SUCCESS;
        else if (strcasecmp(type_s, "help_needed") == 0) type = PHERO_HELP_NEEDED;
        else if (strcasecmp(type_s, "capacity") == 0) type = PHERO_CAPACITY;
    }

    int id = pheromone_deposit(&g_governance.pheromones, type, concentration,
                                region ? region : "global",
                                source ? source : "agent", meta);
    snprintf(result, rlen,
             "{\"ok\":true,\"signal_id\":%d,\"type\":\"%s\",\"concentration\":%.3f,\"region\":\"%s\"}",
             id, pheromone_type_name(type), concentration, region ? region : "global");
    free(type_s); free(region); free(source); free(meta);
    return true;
}

static bool tool_pheromone_sense(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *type_s = json_get_str(input,"type");
    char *region = json_get_str(input,"region");

    if (!type_s) {
        /* Sense all types */
        pheromone_gradient_t grads[PHERO_TYPE_COUNT];
        int n = pheromone_sense_all(&g_governance.pheromones,
                                     region ? region : "global",
                                     PHERO_AGG_SUM, grads, PHERO_TYPE_COUNT);
        jbuf_t b = {0};
        jbuf_appendf(&b, "{\"region\":\"%s\",\"gradients\":[", region ? region : "global");
        for (int i = 0; i < n; i++) {
            jbuf_appendf(&b, "%s{\"type\":\"%s\",\"concentration\":%.4f,\"signals\":%d}",
                         i ? "," : "", pheromone_type_name(grads[i].type),
                         grads[i].concentration, grads[i].signal_count);
        }
        jbuf_appendf(&b, "]}");
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
    } else {
        pheromone_type_t type = PHERO_PROGRESS;
        if (strcasecmp(type_s, "attraction") == 0) type = PHERO_ATTRACTION;
        else if (strcasecmp(type_s, "warning") == 0) type = PHERO_WARNING;
        else if (strcasecmp(type_s, "success") == 0) type = PHERO_SUCCESS;
        else if (strcasecmp(type_s, "help_needed") == 0) type = PHERO_HELP_NEEDED;
        else if (strcasecmp(type_s, "capacity") == 0) type = PHERO_CAPACITY;

        pheromone_gradient_t g;
        pheromone_gradient(&g_governance.pheromones, type,
                           region ? region : "global", PHERO_AGG_SUM, &g);
        snprintf(result, rlen,
                 "{\"type\":\"%s\",\"concentration\":%.4f,\"signals\":%d,"
                 "\"strongest_source\":\"%s\",\"strongest_age\":%.1f}",
                 pheromone_type_name(type), g.concentration, g.signal_count,
                 g.strongest_source, g.strongest_age);
    }
    free(type_s); free(region);
    return true;
}

static bool tool_pheromone_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    pheromone_status_json(&g_governance.pheromones, result, rlen);
    return true;
}

/* ── OODA Tools ───────────────────────────────────────────────────────── */

static bool tool_ooda_begin(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    int id = ooda_begin(&g_governance.ooda);
    snprintf(result, rlen, "{\"ok\":true,\"cycle_id\":%d,\"phase\":\"observe\"}", id);
    return true;
}

static bool tool_ooda_observe(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *content = json_get_str(input,"observation");
    char *source = json_get_str(input,"source");
    double confidence = json_get_double(input, "confidence", 0);
    if (confidence <= 0) confidence = 0.7;

    bool ok = ooda_observe(&g_governance.ooda, content ? content : "",
                           source ? source : "tool", confidence);
    snprintf(result, rlen, "{\"ok\":%s,\"observations\":%d}",
             ok ? "true" : "false", g_governance.ooda.current.observation_count);
    free(content); free(source);
    return true;
}

static bool tool_ooda_orient(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *factor = json_get_str(input,"factor");
    double weight = json_get_double(input, "weight", 0);
    if (weight <= 0) weight = 0.5;
    bool constraint = json_get_bool(input, "constraint", false);

    bool ok = ooda_orient_add(&g_governance.ooda, factor ? factor : "",
                               weight, constraint);

    /* Also set context if provided */
    double budget = json_get_double(input, "budget", 0);
    int tier = (int)json_get_double(input, "tier", 0);
    bool safety = json_get_bool(input, "safety_critical", false);
    if (budget > 0 || tier > 0 || safety)
        ooda_orient_context(&g_governance.ooda, budget, tier, safety);

    snprintf(result, rlen, "{\"ok\":%s,\"factors\":%d,\"phase\":\"%s\"}",
             ok ? "true" : "false", g_governance.ooda.current.factor_count,
             ooda_phase_name(g_governance.ooda.current.phase));
    free(factor);
    return true;
}

static bool tool_ooda_decide(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    ooda_action_t action = ooda_decide(&g_governance.ooda);
    const ooda_decision_t *d = ooda_current_decision(&g_governance.ooda);
    if (d) {
        snprintf(result, rlen,
                 "{\"action\":\"%s\",\"confidence\":%.3f,"
                 "\"capability\":\"%s\",\"reason\":\"%s\","
                 "\"requires_confirmation\":%s}",
                 ooda_action_name(action), d->confidence,
                 ooda_capability_name(d->capability), d->reason,
                 d->requires_confirmation ? "true" : "false");
    } else {
        snprintf(result, rlen, "{\"action\":\"%s\",\"error\":\"no decision\"}", ooda_action_name(action));
    }
    return true;
}

static bool tool_ooda_complete(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    bool success = json_get_bool(input, "success", false);
    char *res = json_get_str(input,"result");

    ooda_act_result(&g_governance.ooda, success, res ? res : "");
    bool ok = ooda_complete(&g_governance.ooda);
    snprintf(result, rlen,
             "{\"ok\":%s,\"total_cycles\":%d,\"success_rate\":%.2f}",
             ok ? "true" : "false", g_governance.ooda.total_cycles,
             ooda_success_rate(&g_governance.ooda, 10));
    free(res);
    return true;
}

static bool tool_ooda_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    ooda_to_json(&g_governance.ooda, result, rlen);
    return true;
}

/* ── Kill Switch Tools ────────────────────────────────────────────────── */

static bool tool_killswitch_trigger(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *level_s = json_get_str(input,"level");
    char *target = json_get_str(input,"target");
    char *reason = json_get_str(input,"reason");
    double timeout = json_get_double(input, "timeout", 0);
    bool cascade = json_get_bool(input, "cascade", false);

    kill_level_t level = KILL_AGENT;
    if (level_s) {
        if (strcasecmp(level_s, "workflow") == 0) level = KILL_WORKFLOW;
        else if (strcasecmp(level_s, "service") == 0) level = KILL_SERVICE;
        else if (strcasecmp(level_s, "pheromone") == 0) level = KILL_PHEROMONE;
        else if (strcasecmp(level_s, "system") == 0) level = KILL_SYSTEM;
    }

    int id = killswitch_trigger(&g_governance.killswitches, level,
                                 target ? target : "*",
                                 reason ? reason : "manual trigger",
                                 KILL_TRIGGER_MANUAL, 1, timeout, cascade);
    snprintf(result, rlen,
             "{\"ok\":%s,\"kill_id\":%d,\"level\":\"%s\",\"target\":\"%s\"}",
             id >= 0 ? "true" : "false", id,
             killswitch_level_name(level), target ? target : "*");
    free(level_s); free(target); free(reason);
    return true;
}

static bool tool_killswitch_resolve(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    int kill_id = (int)json_get_double(input, "kill_id", 0);
    bool ok = killswitch_resolve(&g_governance.killswitches, kill_id, 1);
    snprintf(result, rlen, "{\"ok\":%s,\"kill_id\":%d}", ok ? "true" : "false", kill_id);
    return true;
}

static bool tool_killswitch_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    killswitch_status_json(&g_governance.killswitches, result, rlen);
    return true;
}

/* ── Governance Tools ─────────────────────────────────────────────────── */

static bool tool_governance_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    governance_status_json(&g_governance, result, rlen);
    return true;
}

static bool tool_governance_authorize(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *agent_id = json_get_str(input,"agent_id");
    char *action = json_get_str(input,"action");
    double gsu_cost = json_get_double(input, "gsu_cost", 0);

    bool ok = governance_authorize(&g_governance,
                                    agent_id ? agent_id : "root",
                                    action ? action : "unknown", gsu_cost);
    snprintf(result, rlen,
             "{\"authorized\":%s,\"agent\":\"%s\",\"action\":\"%s\",\"gsu_cost\":%.2f}",
             ok ? "true" : "false", agent_id ? agent_id : "root",
             action ? action : "unknown", gsu_cost);
    free(agent_id); free(action);
    return true;
}

static bool tool_governance_checkpoint(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *agent_id = json_get_str(input,"agent_id");
    char *action = json_get_str(input,"action");
    double gsu_cost = json_get_double(input, "gsu_cost", 0);
    char *context = json_get_str(input,"context");

    bool ok = governance_checkpoint(&g_governance,
                                     agent_id ? agent_id : "root",
                                     action ? action : "unknown",
                                     gsu_cost, context);
    double remaining = governance_remaining_gsu(&g_governance,
                                                 agent_id ? agent_id : "root");
    snprintf(result, rlen,
             "{\"permitted\":%s,\"agent\":\"%s\",\"action\":\"%s\","
             "\"gsu_charged\":%.2f,\"gsu_remaining\":%.2f}",
             ok ? "true" : "false", agent_id ? agent_id : "root",
             action ? action : "unknown", gsu_cost, remaining);
    free(agent_id); free(action); free(context);
    return true;
}

static bool tool_governance_budget(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *agent_id = json_get_str(input,"agent_id");
    const char *aid = agent_id ? agent_id : "root";

    const agent_envelope_t *a = governance_get_agent(&g_governance, aid);
    if (a) {
        snprintf(result, rlen,
                 "{\"agent\":\"%s\",\"tier\":\"%s\","
                 "\"allocated\":%.2f,\"consumed\":%.2f,\"remaining\":%.2f,"
                 "\"capability\":\"%s\",\"can_spawn\":%s,\"can_kill\":%s}",
                 aid, governance_tier_name(a->tier),
                 a->budget.allocated, a->budget.consumed,
                 a->budget.allocated - a->budget.consumed - a->budget.reserved,
                 ooda_capability_name(a->capability),
                 a->can_spawn ? "true" : "false",
                 a->can_kill ? "true" : "false");
    } else {
        snprintf(result, rlen, "{\"error\":\"agent not found\",\"agent\":\"%s\"}", aid);
    }
    free(agent_id);
    return true;
}

static bool tool_governance_audit(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    int last_n = (int)json_get_double(input, "last_n", 0);
    if (last_n <= 0) last_n = 20;
    governance_audit_json(&g_governance, result, rlen, last_n);
    return true;
}

static bool tool_governance_param(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *name = json_get_str(input,"name");
    double value = json_get_double(input, "value", 0);

    if (!name) {
        /* List all params */
        jbuf_t b = {0};
        jbuf_appendf(&b, "{\"params\":[");
        for (int i = 0; i < g_governance.softcoded_count; i++) {
            jbuf_appendf(&b, "%s{\"name\":\"%s\",\"value\":%.4f,\"min\":%.4f,"
                         "\"max\":%.4f,\"default\":%.4f}",
                         i ? "," : "",
                         g_governance.softcoded[i].name,
                         g_governance.softcoded[i].value,
                         g_governance.softcoded[i].min_value,
                         g_governance.softcoded[i].max_value,
                         g_governance.softcoded[i].default_value);
        }
        jbuf_appendf(&b, "]}");
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
    } else if (value != 0) {
        bool ok = governance_set_param(&g_governance, name, value, 1);
        snprintf(result, rlen, "{\"ok\":%s,\"name\":\"%s\",\"value\":%.4f}",
                 ok ? "true" : "false", name, governance_get_param(&g_governance, name));
    } else {
        double v = governance_get_param(&g_governance, name);
        snprintf(result, rlen, "{\"name\":\"%s\",\"value\":%.4f}", name, v);
    }
    free(name);
    return true;
}

/* ── Memory Tier Tools ────────────────────────────────────────────────── */

static bool tool_memory_store(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *key = json_get_str(input,"key");
    char *value = json_get_str(input,"value");
    char *tier_s = json_get_str(input,"tier");
    double importance = json_get_double(input, "importance", 0);
    if (importance <= 0) importance = 0.5;

    if (!key || !value) {
        snprintf(result, rlen, "{\"error\":\"key and value required\"}");
        free(key); free(value); free(tier_s);
        return false;
    }

    memory_tier_t tier = MEM_WORKING;
    if (tier_s) {
        if (strcasecmp(tier_s, "episodic") == 0) tier = MEM_EPISODIC;
        else if (strcasecmp(tier_s, "semantic") == 0) tier = MEM_SEMANTIC;
    }

    int id = memory_store(&g_memory, tier, key, value, importance);
    snprintf(result, rlen,
             "{\"ok\":true,\"id\":%d,\"key\":\"%s\",\"tier\":\"%s\","
             "\"importance\":%.2f,\"halflife\":%.0f}",
             id, key, memory_tier_name(tier), importance,
             memory_tier_halflife(tier));
    free(key); free(value); free(tier_s);
    return true;
}

static bool tool_memory_recall(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *key = json_get_str(input,"key");
    char *query = json_get_str(input,"query");
    char *tag = json_get_str(input,"tag");

    if (key) {
        const memory_entry_t *e = memory_recall(&g_memory, key);
        if (e) {
            snprintf(result, rlen,
                     "{\"found\":true,\"key\":\"%s\",\"value\":\"%.*s\","
                     "\"tier\":\"%s\",\"strength\":%.4f,\"importance\":%.2f,"
                     "\"accesses\":%d,\"pinned\":%s}",
                     e->key, (int)(rlen > 512 ? rlen - 512 : 256), e->value,
                     memory_tier_name(e->tier), e->strength, e->importance,
                     e->access_count, e->pinned ? "true" : "false");
        } else {
            snprintf(result, rlen, "{\"found\":false,\"key\":\"%s\"}", key);
        }
    } else if (query) {
        const memory_entry_t *hits[16];
        int n = memory_search(&g_memory, query, hits, 16);
        jbuf_t b = {0};
        jbuf_appendf(&b, "{\"results\":[");
        for (int i = 0; i < n; i++) {
            jbuf_appendf(&b, "%s{\"key\":\"%s\",\"tier\":\"%s\","
                         "\"strength\":%.4f,\"importance\":%.2f}",
                         i ? "," : "", hits[i]->key,
                         memory_tier_name(hits[i]->tier),
                         hits[i]->strength, hits[i]->importance);
        }
        jbuf_appendf(&b, "],\"count\":%d}", n);
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
    } else if (tag) {
        const memory_entry_t *hits[16];
        int n = memory_recall_by_tag(&g_memory, tag, hits, 16);
        jbuf_t b = {0};
        jbuf_appendf(&b, "{\"tag\":\"%s\",\"results\":[", tag);
        for (int i = 0; i < n; i++) {
            jbuf_appendf(&b, "%s{\"key\":\"%s\",\"tier\":\"%s\"}",
                         i ? "," : "", hits[i]->key,
                         memory_tier_name(hits[i]->tier));
        }
        jbuf_appendf(&b, "],\"count\":%d}", n);
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
    } else {
        snprintf(result, rlen, "{\"error\":\"provide key, query, or tag\"}");
    }
    free(key); free(query); free(tag);
    return true;
}

static bool tool_memory_promote(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *key = json_get_str(input,"key");
    if (!key) {
        snprintf(result, rlen, "{\"error\":\"key required\"}");
        return false;
    }
    bool ok = memory_promote(&g_memory, key);
    if (ok) {
        const memory_entry_t *e = memory_recall(&g_memory, key);
        snprintf(result, rlen, "{\"ok\":true,\"key\":\"%s\",\"new_tier\":\"%s\"}",
                 key, e ? memory_tier_name(e->tier) : "unknown");
    } else {
        snprintf(result, rlen, "{\"ok\":false,\"key\":\"%s\",\"reason\":\"not found or already semantic\"}", key);
    }
    free(key);
    return true;
}

static bool tool_memory_forget(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *key = json_get_str(input,"key");
    if (!key) {
        snprintf(result, rlen, "{\"error\":\"key required\"}");
        return false;
    }
    bool ok = memory_forget(&g_memory, key);
    snprintf(result, rlen, "{\"ok\":%s,\"key\":\"%s\"}", ok ? "true" : "false", key);
    free(key);
    return true;
}

static bool tool_memory_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    memory_status_json(&g_memory, result, rlen);
    return true;
}

/* ── Talons Tools (Competitive Execution) ─────────────────────────────── */

static bool tool_talons_goal_create(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *desc = json_get_str(input, "description");
    char *grip_s = json_get_str(input, "grip");
    char *strategy_s = json_get_str(input, "strategy");
    double priority = json_get_double(input, "priority", 0);
    double deadline = json_get_double(input, "deadline", 0);
    if (priority <= 0) priority = 0.5;

    grip_strength_t grip = GRIP_HOLDING;
    if (grip_s) {
        if (strcasecmp(grip_s, "tentative") == 0) grip = GRIP_TENTATIVE;
        else if (strcasecmp(grip_s, "locked") == 0) grip = GRIP_LOCKED;
        else if (strcasecmp(grip_s, "death_grip") == 0) grip = GRIP_DEATH_GRIP;
    }

    strategy_type_t strategy = STRATEGY_DIRECT;
    if (strategy_s) {
        if (strcasecmp(strategy_s, "flanking") == 0) strategy = STRATEGY_FLANKING;
        else if (strcasecmp(strategy_s, "tournament") == 0) strategy = STRATEGY_TOURNAMENT;
        else if (strcasecmp(strategy_s, "escalation") == 0) strategy = STRATEGY_ESCALATION;
        else if (strcasecmp(strategy_s, "divide") == 0) strategy = STRATEGY_DIVIDE;
        else if (strcasecmp(strategy_s, "ambush") == 0) strategy = STRATEGY_AMBUSH;
    }

    int id = talons_goal_create(&g_talons, desc, priority, grip, strategy, deadline);
    snprintf(result, rlen,
             "{\"ok\":true,\"goal_id\":%d,\"description\":\"%.*s\","
             "\"grip\":\"%s\",\"strategy\":\"%s\",\"priority\":%.2f,"
             "\"max_attempts\":%d}",
             id, 80, desc ? desc : "",
             talons_grip_name(grip), talons_strategy_name(strategy),
             priority, grip == GRIP_TENTATIVE ? 1 : grip == GRIP_HOLDING ? 3 : grip == GRIP_LOCKED ? 7 : 20);
    free(desc); free(grip_s); free(strategy_s);
    return true;
}

static bool tool_talons_goal_advance(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    int goal_id = (int)json_get_double(input, "goal_id", 0);
    char *action = json_get_str(input, "action");
    char *res_text = json_get_str(input, "result");
    double cost = json_get_double(input, "cost", 0);
    double confidence = json_get_double(input, "confidence", 0);

    bool ok = false;
    const char *new_state = "unknown";

    if (!action) {
        snprintf(result, rlen, "{\"error\":\"action required: stalk|strike|grip|capture|escaped|abandon\"}");
        free(action); free(res_text);
        return false;
    }

    if (strcasecmp(action, "stalk") == 0) {
        ok = talons_goal_stalk(&g_talons, goal_id);
        new_state = "stalking";
    } else if (strcasecmp(action, "strike") == 0) {
        ok = talons_goal_strike(&g_talons, goal_id);
        new_state = "striking";
    } else if (strcasecmp(action, "grip") == 0) {
        ok = talons_goal_grip(&g_talons, goal_id);
        new_state = "gripping";
    } else if (strcasecmp(action, "capture") == 0) {
        ok = talons_goal_capture(&g_talons, goal_id, res_text, cost);
        new_state = "captured";
    } else if (strcasecmp(action, "escaped") == 0) {
        bool retrying = talons_goal_escaped(&g_talons, goal_id, res_text, cost);
        new_state = retrying ? "stalking (retry)" : "escaped";
        ok = true;
    } else if (strcasecmp(action, "abandon") == 0) {
        ok = talons_goal_abandon(&g_talons, goal_id, res_text);
        new_state = "abandoned";
    }

    if (confidence > 0) talons_goal_update_confidence(&g_talons, goal_id, confidence);

    const goal_t *g = talons_goal_get(&g_talons, goal_id);
    snprintf(result, rlen,
             "{\"ok\":%s,\"goal_id\":%d,\"new_state\":\"%s\","
             "\"attempts\":%d,\"confidence\":%.3f}",
             ok ? "true" : "false", goal_id, new_state,
             g ? g->attempts : 0, g ? g->confidence : 0);
    free(action); free(res_text);
    return true;
}

static bool tool_talons_tournament(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    char *action = json_get_str(input, "action");
    char *objective = json_get_str(input, "objective");
    int tid = (int)json_get_double(input, "tournament_id", 0);
    int cid = (int)json_get_double(input, "competitor_id", 0);
    char *label = json_get_str(input, "label");
    double score = json_get_double(input, "score", 0);
    double cost = json_get_double(input, "cost", 0);
    char *res_text = json_get_str(input, "result");

    if (!action) {
        snprintf(result, rlen, "{\"error\":\"action required: begin|add|result|decide\"}");
        goto done;
    }

    if (strcasecmp(action, "begin") == 0) {
        double wq = json_get_double(input, "weight_quality", 0);
        double ws = json_get_double(input, "weight_speed", 0);
        double wc = json_get_double(input, "weight_cost", 0);
        double deadline = json_get_double(input, "deadline", 0);
        int id = talons_tournament_begin(&g_talons, objective, deadline,
                                          wq > 0 ? wq : 0.5,
                                          ws > 0 ? ws : 0.3,
                                          wc > 0 ? wc : 0.2);
        snprintf(result, rlen, "{\"ok\":true,\"tournament_id\":%d,\"objective\":\"%.*s\"}",
                 id, 80, objective ? objective : "");
    } else if (strcasecmp(action, "add") == 0) {
        char *strat_s = json_get_str(input, "strategy");
        strategy_type_t strat = STRATEGY_DIRECT;
        if (strat_s && strcasecmp(strat_s, "flanking") == 0) strat = STRATEGY_FLANKING;
        int id = talons_tournament_add(&g_talons, tid, label, strat);
        snprintf(result, rlen, "{\"ok\":%s,\"competitor_id\":%d}", id >= 0 ? "true" : "false", id);
        free(strat_s);
    } else if (strcasecmp(action, "result") == 0) {
        bool ok = talons_tournament_result(&g_talons, tid, cid, score, cost, res_text);
        const tournament_t *tr = talons_tournament_get(&g_talons, tid);
        snprintf(result, rlen,
                 "{\"ok\":%s,\"decided\":%s,\"winner\":%d}",
                 ok ? "true" : "false",
                 (tr && tr->decided) ? "true" : "false",
                 tr ? tr->winner_id : -1);
    } else if (strcasecmp(action, "decide") == 0) {
        int winner = talons_tournament_decide(&g_talons, tid);
        snprintf(result, rlen, "{\"winner_id\":%d}", winner);
    } else {
        snprintf(result, rlen, "{\"error\":\"unknown action: %s\"}", action);
    }

done:
    free(action); free(objective); free(label); free(res_text);
    return true;
}

static bool tool_talons_recommend(const char *input, char *result, size_t rlen) {
    ensure_wt_init();
    double time_pressure = json_get_double(input, "time_pressure", 0);
    double budget = json_get_double(input, "resource_budget", 0);
    double complexity = json_get_double(input, "complexity", 0);
    if (time_pressure <= 0) time_pressure = 0.5;
    if (budget <= 0) budget = 0.5;
    if (complexity <= 0) complexity = 0.5;

    strategy_type_t rec = talons_recommend_strategy(&g_talons, time_pressure, budget, complexity);
    snprintf(result, rlen,
             "{\"recommended\":\"%s\",\"context\":{\"time_pressure\":%.2f,"
             "\"resource_budget\":%.2f,\"complexity\":%.2f},"
             "\"best_overall\":\"%s\",\"win_rate\":%.3f}",
             talons_strategy_name(rec), time_pressure, budget, complexity,
             talons_strategy_name(talons_best_strategy(&g_talons)),
             talons_win_rate(&g_talons, 20));
    return true;
}

static bool tool_talons_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    talons_status_json(&g_talons, result, rlen);
    return true;
}

/* ── Wings+Talons+Immune unified meta tool ────────────────────────────── */

static bool tool_wings_talons_status(const char *input, char *result, size_t rlen) {
    (void)input;
    ensure_wt_init();
    governance_tick(&g_governance);
    memory_tick(&g_memory);

    jbuf_t b = {0};
    jbuf_appendf(&b, "{\"version\":\"v0.9.0\",\"wings\":{\"pheromones\":");

    char pbuf[4096];
    pheromone_status_json(&g_governance.pheromones, pbuf, sizeof(pbuf));
    jbuf_appendf(&b, "%s,\"memory\":", pbuf);

    char mbuf[4096];
    memory_status_json(&g_memory, mbuf, sizeof(mbuf));
    jbuf_appendf(&b, "%s},\"talons\":", mbuf);

    char tbuf[4096];
    talons_status_json(&g_talons, tbuf, sizeof(tbuf));
    jbuf_appendf(&b, "%s,\"immune_system\":", tbuf);

    char ibuf[8192];
    governance_status_json(&g_governance, ibuf, sizeof(ibuf));
    jbuf_appendf(&b, "%s}", ibuf);

    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}

/* ── Legion: Angel/Demon agent system ─────────────────────────────── */

#include "legion.h"

static bool tool_legion_status(const char *input, char *result, size_t rlen) {
    (void)input;
    legion_init();
    int total = 0;
    legion_registry(&total);
    int angels = legion_angel_count();
    int demons = legion_demon_count();

    int off = 0;
    off += snprintf(result + off, rlen - off,
        "{\"total\":%d,\"angels\":%d,\"demons\":%d,\"roles\":%d,\"variants_by_role\":[",
        total, angels, demons, LEGION_ROLE_COUNT);

    for (int r = 0; r < LEGION_ROLE_COUNT && (size_t)off < rlen - 100; r++) {
        if (r > 0) off += snprintf(result + off, rlen - off, ",");
        off += snprintf(result + off, rlen - off,
            "{\"role\":\"%s\",\"count\":%d}",
            legion_role_name((legion_role_t)r), legion_count_by_role((legion_role_t)r));
    }
    off += snprintf(result + off, rlen - off, "]}");
    return true;
}

static bool tool_legion_spawn(const char *input, char *result, size_t rlen) {
    if (!input) { snprintf(result, rlen, "{\"error\":\"missing input\"}"); return false; }

    /* Parse task and class/role/variant_id */
    char task[2048] = "";
    char class_str[16] = "auto";
    char role_str[32] = "";
    char variant_name[64] = "";
    int variant_id = -1;

    /* Extract fields */
    const char *t = strstr(input, "\"task\"");
    if (t) {
        t = strchr(t + 6, '"'); if (t) { t++;
        const char *te = strchr(t, '"');
        if (te && (size_t)(te - t) < sizeof(task)) { memcpy(task, t, te - t); task[te - t] = '\0'; }
        }
    }
    const char *c = strstr(input, "\"class\"");
    if (c) {
        c = strchr(c + 7, '"'); if (c) { c++;
        const char *ce = strchr(c, '"');
        if (ce && (size_t)(ce - c) < sizeof(class_str)) { memcpy(class_str, c, ce - c); class_str[ce - c] = '\0'; }
        }
    }
    const char *r = strstr(input, "\"role\"");
    if (r) {
        r = strchr(r + 6, '"'); if (r) { r++;
        const char *re = strchr(r, '"');
        if (re && (size_t)(re - r) < sizeof(role_str)) { memcpy(role_str, r, re - r); role_str[re - r] = '\0'; }
        }
    }
    const char *vn = strstr(input, "\"variant\"");
    if (vn) {
        vn = strchr(vn + 9, '"'); if (vn) { vn++;
        const char *ve = strchr(vn, '"');
        if (ve && (size_t)(ve - vn) < sizeof(variant_name)) { memcpy(variant_name, vn, ve - vn); variant_name[ve - vn] = '\0'; }
        }
    }
    const char *vi = strstr(input, "\"variant_id\"");
    if (vi) {
        vi = strchr(vi + 12, ':');
        if (vi) variant_id = atoi(vi + 1);
    }

    if (!task[0]) { snprintf(result, rlen, "{\"error\":\"task is required\"}"); return false; }

    const legion_variant_t *v = NULL;

    /* Resolution order: variant_id → variant name → auto-select by class+role */
    if (variant_id >= 0) {
        v = legion_get(variant_id);
    } else if (variant_name[0]) {
        v = legion_find(variant_name);
    } else {
        agent_class_t cls = AGENT_CLASS_ANGEL;
        if (strcmp(class_str, "demon") == 0) cls = AGENT_CLASS_DEMON;
        v = legion_auto_select(task, cls);
    }

    if (!v) {
        snprintf(result, rlen, "{\"error\":\"no matching legion variant found\"}");
        return false;
    }

    int agent_id = legion_spawn(v->id, task);
    if (agent_id < 0) {
        snprintf(result, rlen, "{\"error\":\"spawn failed\"}");
        return false;
    }

    int off = 0;
    off += snprintf(result + off, rlen - off, "{\"agent_id\":%d,", agent_id);
    off += snprintf(result + off, rlen - off, "\"variant\":");
    off += legion_variant_json(v, result + off, rlen - off);
    off += snprintf(result + off, rlen - off, "}");
    return true;
}

static bool tool_legion_find(const char *input, char *result, size_t rlen) {
    if (!input) { snprintf(result, rlen, "{\"error\":\"missing input\"}"); return false; }

    char class_str[16] = "";
    char role_str[32] = "";
    int limit = 10;

    const char *c = strstr(input, "\"class\"");
    if (c) { c = strchr(c+7,'"'); if(c){c++; const char *e=strchr(c,'"'); if(e&&(size_t)(e-c)<sizeof(class_str)){memcpy(class_str,c,e-c);class_str[e-c]='\0';}}}
    const char *r = strstr(input, "\"role\"");
    if (r) { r = strchr(r+6,'"'); if(r){r++; const char *e=strchr(r,'"'); if(e&&(size_t)(e-r)<sizeof(role_str)){memcpy(role_str,r,e-r);role_str[e-r]='\0';}}}
    const char *l = strstr(input, "\"limit\"");
    if (l) { l = strchr(l+7,':'); if(l) limit = atoi(l+1); }
    if (limit < 1) limit = 1;
    if (limit > 50) limit = 50;

    legion_init();
    int off = 0;
    off += snprintf(result + off, rlen - off, "{\"variants\":[");

    int found = 0;
    int total = 0;
    const legion_variant_t *all = legion_registry(&total);

    for (int i = 0; i < total && found < limit; i++) {
        const legion_variant_t *v = &all[i];
        /* Filter by class */
        if (class_str[0]) {
            if (strcmp(class_str, "angel") == 0 && v->cls != AGENT_CLASS_ANGEL) continue;
            if (strcmp(class_str, "demon") == 0 && v->cls != AGENT_CLASS_DEMON) continue;
        }
        /* Filter by role */
        if (role_str[0] && strcmp(role_str, legion_role_name(v->role)) != 0) continue;

        if (found > 0) off += snprintf(result + off, rlen - off, ",");
        off += legion_variant_json(v, result + off, rlen - off);
        found++;
    }

    off += snprintf(result + off, rlen - off, "],\"found\":%d,\"total\":%d}", found, total);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ACE PLAYBOOK: Agentic Context Engineering (Zhang et al. 2026)
 *
 * Evolving playbook of itemized bullets with:
 *   - Unique IDs (pb-NNNNN)
 *   - Helpful/harmful/neutral counters for adaptive scoring
 *   - Section-based organization (strategies, mistakes, patterns, etc.)
 *   - Incremental delta updates (never monolithic rewrite)
 *   - Grow-and-refine GC with semantic deduplication
 *
 * Prevents context collapse by accumulating structured knowledge that
 * persists across turns and can be searched/pruned independently of
 * the conversation history.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PB_MAX_BULLETS       512
#define PB_MAX_CONTENT       2048
#define PB_MAX_SECTION       48
#define PB_MAX_TAGS          4
#define PB_TAG_LEN           32
#define PB_DEDUP_THRESHOLD   0.85f  /* cosine similarity for dedup */

typedef struct {
    int   id;
    char  section[PB_MAX_SECTION];      /* strategies, common_mistakes, api_schemas, etc. */
    char  content[PB_MAX_CONTENT];
    char  tags[PB_MAX_TAGS][PB_TAG_LEN];
    int   tag_count;
    int   helpful;
    int   harmful;
    int   neutral;
    int   created_turn;
    int   last_accessed_turn;
    float embed[CTX_EMBED_DIM];
    float embed_norm;
    uint64_t content_hash;
} pb_bullet_t;

typedef struct {
    pb_bullet_t bullets[PB_MAX_BULLETS];
    int   count;
    int   next_id;
    int   current_turn;
} pb_store_t;

static pb_store_t g_playbook = {0};

/* ACE bullet score: helpful - 2*harmful, with recency boost */
static float pb_score(const pb_bullet_t *b) {
    float base = (float)b->helpful - 2.0f * (float)b->harmful;
    int age = g_playbook.current_turn - b->last_accessed_turn;
    if (age < 1) age = 1;
    float recency = 1.0f / (1.0f + 0.1f * (float)age);
    return base * recency;
}

/* ── context_status: full context window self-awareness ───────────── */

static bool tool_context_status(const char *input, char *result, size_t rlen) {
    (void)input;
    int off = 0;
    int window = g_ctx_window_tokens > 0 ? g_ctx_window_tokens : 200000;
    int used = g_ctx_used_input_tokens + g_ctx_used_output_tokens;
    int remaining = window - used;
    float pct = 100.0f * (float)used / (float)window;

    off += snprintf(result + off, rlen - off,
        "{\"context_window\":{\"total_tokens\":%d,\"used_tokens\":%d,"
        "\"remaining_tokens\":%d,\"usage_pct\":%.1f,"
        "\"input_tokens\":%d,\"output_tokens\":%d},",
        window, used, remaining, pct,
        g_ctx_used_input_tokens, g_ctx_used_output_tokens);

    /* Tool schema overhead estimate: ~200 tokens per active tool */
    int tool_count;
    tools_get_all(&tool_count);
    int active_estimate = TOOL_REGISTER_CAP < tool_count ? TOOL_REGISTER_CAP : tool_count;
    int schema_tokens = active_estimate * 200;

    off += snprintf(result + off, rlen - off,
        "\"tool_overhead\":{\"active_tools\":%d,\"est_schema_tokens\":%d},",
        active_estimate, schema_tokens);

    /* Playbook stats */
    int pb_helpful = 0, pb_harmful = 0;
    size_t pb_bytes = 0;
    for (int i = 0; i < g_playbook.count; i++) {
        pb_helpful += g_playbook.bullets[i].helpful;
        pb_harmful += g_playbook.bullets[i].harmful;
        pb_bytes += strlen(g_playbook.bullets[i].content);
    }
    int pb_est_tokens = (int)(pb_bytes / 4);  /* ~4 chars per token */

    off += snprintf(result + off, rlen - off,
        "\"playbook\":{\"bullets\":%d,\"helpful_total\":%d,\"harmful_total\":%d,"
        "\"est_tokens\":%d},",
        g_playbook.count, pb_helpful, pb_harmful, pb_est_tokens);

    /* Context store stats */
    off += snprintf(result + off, rlen - off,
        "\"context_store\":{\"chunks\":%d,\"bytes\":%zu,"
        "\"offloaded_events\":%zu,\"offloaded_bytes\":%zu},",
        g_ctx.count, g_ctx.total_bytes,
        g_ctx_offload_events, g_ctx_offloaded_bytes);

    /* Recommendations */
    off += snprintf(result + off, rlen - off, "\"recommendations\":[");
    bool first = true;
    if (pct > 85.0f) {
        off += snprintf(result + off, rlen - off,
            "\"CRITICAL: context >85%% full — use context_compact or playbook_gc\"");
        first = false;
    } else if (pct > 70.0f) {
        off += snprintf(result + off, rlen - off,
            "\"WARNING: context >70%% — consider compacting old tool results\"");
        first = false;
    }
    if (g_playbook.count > 0) {
        int harmful_bullets = 0;
        for (int i = 0; i < g_playbook.count; i++)
            if (g_playbook.bullets[i].harmful > g_playbook.bullets[i].helpful)
                harmful_bullets++;
        if (harmful_bullets > 0) {
            if (!first) off += snprintf(result + off, rlen - off, ",");
            off += snprintf(result + off, rlen - off,
                "\"%d playbook bullets have more harmful than helpful tags — consider playbook_gc\"",
                harmful_bullets);
        }
    }
    off += snprintf(result + off, rlen - off, "]}");
    return true;
}

/* ── playbook_read: list all bullets with scores ─────────────────── */

static bool tool_playbook_read(const char *input, char *result, size_t rlen) {
    /* Optional section filter */
    char section_filter[PB_MAX_SECTION] = "";
    if (input) {
        const char *s = strstr(input, "\"section\"");
        if (s) {
            s = strchr(s + 9, '"'); if (s) { s++;
            const char *e = strchr(s, '"');
            if (e && (size_t)(e - s) < sizeof(section_filter)) {
                memcpy(section_filter, s, e - s);
                section_filter[e - s] = '\0';
            }}
        }
    }

    int off = 0;
    off += snprintf(result + off, rlen - off,
        "{\"total_bullets\":%d,\"current_turn\":%d,\"bullets\":[",
        g_playbook.count, g_playbook.current_turn);

    bool first = true;
    for (int i = 0; i < g_playbook.count && (size_t)off < rlen - 512; i++) {
        pb_bullet_t *b = &g_playbook.bullets[i];
        if (section_filter[0] && strcasecmp(section_filter, b->section) != 0) continue;

        b->last_accessed_turn = g_playbook.current_turn;
        if (!first) off += snprintf(result + off, rlen - off, ",");
        first = false;

        off += snprintf(result + off, rlen - off,
            "{\"id\":\"pb-%05d\",\"section\":\"%s\","
            "\"helpful\":%d,\"harmful\":%d,\"neutral\":%d,"
            "\"score\":%.2f,\"content\":",
            b->id, b->section, b->helpful, b->harmful, b->neutral,
            pb_score(b));

        /* JSON-escape content */
        off += snprintf(result + off, rlen - off, "\"");
        for (int j = 0; b->content[j] && (size_t)off < rlen - 64; j++) {
            char c = b->content[j];
            if (c == '"') { result[off++] = '\\'; result[off++] = '"'; }
            else if (c == '\\') { result[off++] = '\\'; result[off++] = '\\'; }
            else if (c == '\n') { result[off++] = '\\'; result[off++] = 'n'; }
            else if (c == '\t') { result[off++] = '\\'; result[off++] = 't'; }
            else result[off++] = c;
        }
        off += snprintf(result + off, rlen - off, "\"}");
    }
    off += snprintf(result + off, rlen - off, "]}");
    return true;
}

/* ── playbook_add: incremental delta update ──────────────────────── */

static bool tool_playbook_add(const char *input, char *result, size_t rlen) {
    if (!input) {
        snprintf(result, rlen, "{\"error\":\"input required\"}");
        return false;
    }

    char section[PB_MAX_SECTION] = "strategies";
    char content[PB_MAX_CONTENT] = "";
    char tags[PB_MAX_TAGS][PB_TAG_LEN] = {{""}};
    int tag_count = 0;

    /* Parse section */
    const char *s = strstr(input, "\"section\"");
    if (s) {
        s = strchr(s + 9, '"'); if (s) { s++;
        const char *e = strchr(s, '"');
        if (e && (size_t)(e - s) < sizeof(section)) {
            memcpy(section, s, e - s);
            section[e - s] = '\0';
        }}
    }

    /* Parse content */
    s = strstr(input, "\"content\"");
    if (s) {
        s = strchr(s + 9, '"'); if (s) { s++;
        int ci = 0;
        while (*s && ci < PB_MAX_CONTENT - 1) {
            if (*s == '\\' && *(s+1)) {
                s++;
                if (*s == 'n') content[ci++] = '\n';
                else if (*s == 't') content[ci++] = '\t';
                else if (*s == '"') content[ci++] = '"';
                else if (*s == '\\') content[ci++] = '\\';
                else content[ci++] = *s;
            } else if (*s == '"') break;
            else content[ci++] = *s;
            s++;
        }
        content[ci] = '\0';
        }
    }

    /* Parse tags array */
    s = strstr(input, "\"tags\"");
    if (s) {
        s = strchr(s, '[');
        if (s) {
            s++;
            while (*s && *s != ']' && tag_count < PB_MAX_TAGS) {
                const char *q1 = strchr(s, '"');
                if (!q1 || *q1 == ']') break;
                q1++;
                const char *q2 = strchr(q1, '"');
                if (!q2) break;
                size_t len = q2 - q1;
                if (len < PB_TAG_LEN) {
                    memcpy(tags[tag_count], q1, len);
                    tags[tag_count][len] = '\0';
                    tag_count++;
                }
                s = q2 + 1;
            }
        }
    }

    if (!content[0]) {
        snprintf(result, rlen, "{\"error\":\"content is required\"}");
        return false;
    }

    /* Check for near-duplicate by content hash */
    uint64_t hash = ctx_hash_bytes(content, strlen(content));
    for (int i = 0; i < g_playbook.count; i++) {
        if (g_playbook.bullets[i].content_hash == hash) {
            /* Exact duplicate — reinforce instead of adding */
            g_playbook.bullets[i].helpful++;
            g_playbook.bullets[i].last_accessed_turn = g_playbook.current_turn;
            snprintf(result, rlen,
                "{\"action\":\"reinforced\",\"id\":\"pb-%05d\",\"helpful\":%d,"
                "\"note\":\"Duplicate content — reinforced existing bullet instead of adding.\"}",
                g_playbook.bullets[i].id, g_playbook.bullets[i].helpful);
            return true;
        }
    }

    if (g_playbook.count >= PB_MAX_BULLETS) {
        /* Evict lowest-scoring bullet */
        int min_i = 0;
        float min_s = pb_score(&g_playbook.bullets[0]);
        for (int i = 1; i < g_playbook.count; i++) {
            float sc = pb_score(&g_playbook.bullets[i]);
            if (sc < min_s) { min_s = sc; min_i = i; }
        }
        if (min_i < g_playbook.count - 1)
            memmove(&g_playbook.bullets[min_i], &g_playbook.bullets[min_i + 1],
                     (size_t)(g_playbook.count - min_i - 1) * sizeof(pb_bullet_t));
        g_playbook.count--;
    }

    pb_bullet_t *b = &g_playbook.bullets[g_playbook.count];
    memset(b, 0, sizeof(*b));
    b->id = ++g_playbook.next_id;
    snprintf(b->section, sizeof(b->section), "%s", section);
    snprintf(b->content, sizeof(b->content), "%s", content);
    for (int i = 0; i < tag_count; i++)
        snprintf(b->tags[i], PB_TAG_LEN, "%s", tags[i]);
    b->tag_count = tag_count;
    b->helpful = 0;
    b->harmful = 0;
    b->neutral = 0;
    b->created_turn = g_playbook.current_turn;
    b->last_accessed_turn = g_playbook.current_turn;
    b->content_hash = hash;
    /* Compute embedding for dedup/search */
    ctx_build_embedding(content, b->embed, NULL, &b->embed_norm);
    g_playbook.count++;

    snprintf(result, rlen,
        "{\"action\":\"added\",\"id\":\"pb-%05d\",\"section\":\"%s\","
        "\"total_bullets\":%d}",
        b->id, b->section, g_playbook.count);
    return true;
}

/* ── playbook_tag: mark bullet as helpful/harmful/neutral ────────── */

static bool tool_playbook_tag(const char *input, char *result, size_t rlen) {
    if (!input) {
        snprintf(result, rlen, "{\"error\":\"input required\"}");
        return false;
    }

    /* Parse bullet_id (number after "pb-") */
    int target_id = -1;
    const char *s = strstr(input, "\"bullet_id\"");
    if (s) {
        s = strchr(s + 11, '"');
        if (s) {
            s++;
            /* Skip "pb-" prefix if present */
            if (strncmp(s, "pb-", 3) == 0) s += 3;
            target_id = atoi(s);
        }
    }

    /* Parse tag */
    char tag[32] = "";
    s = strstr(input, "\"tag\"");
    if (s) {
        s = strchr(s + 4, '"'); if (s) { s++;
        const char *e = strchr(s, '"');
        if (e && (size_t)(e - s) < sizeof(tag)) {
            memcpy(tag, s, e - s);
            tag[e - s] = '\0';
        }}
    }

    if (target_id < 0 || !tag[0]) {
        snprintf(result, rlen, "{\"error\":\"bullet_id and tag required\"}");
        return false;
    }

    for (int i = 0; i < g_playbook.count; i++) {
        if (g_playbook.bullets[i].id == target_id) {
            pb_bullet_t *b = &g_playbook.bullets[i];
            if (strcasecmp(tag, "helpful") == 0) b->helpful++;
            else if (strcasecmp(tag, "harmful") == 0) b->harmful++;
            else b->neutral++;
            b->last_accessed_turn = g_playbook.current_turn;

            snprintf(result, rlen,
                "{\"id\":\"pb-%05d\",\"helpful\":%d,\"harmful\":%d,"
                "\"neutral\":%d,\"score\":%.2f}",
                b->id, b->helpful, b->harmful, b->neutral, pb_score(b));
            return true;
        }
    }

    snprintf(result, rlen, "{\"error\":\"bullet pb-%05d not found\"}", target_id);
    return false;
}

/* ── playbook_remove: delete a bullet by ID ──────────────────────── */

static bool tool_playbook_remove(const char *input, char *result, size_t rlen) {
    if (!input) {
        snprintf(result, rlen, "{\"error\":\"input required\"}");
        return false;
    }

    int target_id = -1;
    const char *s = strstr(input, "\"bullet_id\"");
    if (s) {
        s = strchr(s + 11, '"');
        if (s) {
            s++;
            if (strncmp(s, "pb-", 3) == 0) s += 3;
            target_id = atoi(s);
        }
    }

    if (target_id < 0) {
        snprintf(result, rlen, "{\"error\":\"bullet_id required\"}");
        return false;
    }

    for (int i = 0; i < g_playbook.count; i++) {
        if (g_playbook.bullets[i].id == target_id) {
            if (i < g_playbook.count - 1)
                memmove(&g_playbook.bullets[i], &g_playbook.bullets[i + 1],
                         (size_t)(g_playbook.count - i - 1) * sizeof(pb_bullet_t));
            g_playbook.count--;
            snprintf(result, rlen,
                "{\"removed\":\"pb-%05d\",\"remaining\":%d}", target_id, g_playbook.count);
            return true;
        }
    }

    snprintf(result, rlen, "{\"error\":\"bullet pb-%05d not found\"}", target_id);
    return false;
}

/* ── playbook_search: semantic search over bullets ───────────────── */

static bool tool_playbook_search(const char *input, char *result, size_t rlen) {
    if (!input) {
        snprintf(result, rlen, "{\"error\":\"query required\"}");
        return false;
    }

    char query[1024] = "";
    int top_k = 5;
    const char *s = strstr(input, "\"query\"");
    if (s) {
        s = strchr(s + 7, '"'); if (s) { s++;
        int qi = 0;
        while (*s && *s != '"' && qi < 1023) {
            if (*s == '\\' && *(s+1)) { s++; query[qi++] = *s; }
            else query[qi++] = *s;
            s++;
        }
        query[qi] = '\0';
        }
    }
    s = strstr(input, "\"top_k\"");
    if (s) {
        s += 7;
        while (*s && (*s < '0' || *s > '9')) s++;
        top_k = atoi(s);
        if (top_k < 1) top_k = 1;
        if (top_k > 20) top_k = 20;
    }

    if (!query[0]) {
        snprintf(result, rlen, "{\"error\":\"query is required\"}");
        return false;
    }

    /* Build query embedding */
    float qembed[CTX_EMBED_DIM];
    float qnorm;
    ctx_build_embedding(query, qembed, NULL, &qnorm);

    /* Score all bullets */
    typedef struct { int idx; float score; } hit_t;
    hit_t *hits = safe_malloc(g_playbook.count * sizeof(hit_t));
    int nhits = 0;

    for (int i = 0; i < g_playbook.count; i++) {
        pb_bullet_t *b = &g_playbook.bullets[i];
        float cosine = ctx_cosine(qembed, qnorm, b->embed, b->embed_norm);
        /* Blend semantic similarity with bullet quality score */
        float quality = pb_score(b);
        float final = cosine * 0.7f + (quality > 0 ? 0.3f : 0.0f);
        hits[nhits].idx = i;
        hits[nhits].score = final;
        nhits++;
    }

    /* Sort descending */
    for (int i = 0; i < nhits - 1; i++)
        for (int j = i + 1; j < nhits; j++)
            if (hits[j].score > hits[i].score) {
                hit_t tmp = hits[i]; hits[i] = hits[j]; hits[j] = tmp;
            }

    int show = nhits < top_k ? nhits : top_k;
    int off = 0;
    off += snprintf(result + off, rlen - off,
        "{\"query\":\"%s\",\"hits\":%d,\"bullets\":[", query, show);

    for (int i = 0; i < show && (size_t)off < rlen - 512; i++) {
        pb_bullet_t *b = &g_playbook.bullets[hits[i].idx];
        b->last_accessed_turn = g_playbook.current_turn;
        if (i > 0) off += snprintf(result + off, rlen - off, ",");

        off += snprintf(result + off, rlen - off,
            "{\"id\":\"pb-%05d\",\"section\":\"%s\","
            "\"score\":%.3f,\"helpful\":%d,\"harmful\":%d,\"content\":",
            b->id, b->section, hits[i].score, b->helpful, b->harmful);

        off += snprintf(result + off, rlen - off, "\"");
        for (int j = 0; b->content[j] && (size_t)off < rlen - 64; j++) {
            char c = b->content[j];
            if (c == '"') { result[off++] = '\\'; result[off++] = '"'; }
            else if (c == '\\') { result[off++] = '\\'; result[off++] = '\\'; }
            else if (c == '\n') { result[off++] = '\\'; result[off++] = 'n'; }
            else result[off++] = c;
        }
        off += snprintf(result + off, rlen - off, "\"}");
    }
    off += snprintf(result + off, rlen - off, "]}");
    free(hits);
    return true;
}

/* ── playbook_gc: grow-and-refine — dedup + prune low scorers ────── */

static bool tool_playbook_gc(const char *input, char *result, size_t rlen) {
    int max_bullets = PB_MAX_BULLETS;
    float prune_threshold = -1.0f;  /* remove bullets scoring below this */

    if (input) {
        const char *s = strstr(input, "\"max_bullets\"");
        if (s) {
            s += 13;
            while (*s && (*s < '0' || *s > '9') && *s != '-') s++;
            max_bullets = atoi(s);
            if (max_bullets < 10) max_bullets = 10;
        }
        s = strstr(input, "\"prune_below\"");
        if (s) {
            s += 13;
            while (*s && *s != '-' && *s != '.' && (*s < '0' || *s > '9')) s++;
            prune_threshold = (float)atof(s);
        }
    }

    int deduped = 0, pruned = 0;

    /* Pass 1: Semantic deduplication — merge near-duplicates */
    for (int i = 0; i < g_playbook.count; i++) {
        for (int j = i + 1; j < g_playbook.count; j++) {
            float sim = ctx_cosine(g_playbook.bullets[i].embed,
                                    g_playbook.bullets[i].embed_norm,
                                    g_playbook.bullets[j].embed,
                                    g_playbook.bullets[j].embed_norm);
            if (sim >= PB_DEDUP_THRESHOLD) {
                /* Merge: keep higher-scoring bullet, absorb counters */
                float si = pb_score(&g_playbook.bullets[i]);
                float sj = pb_score(&g_playbook.bullets[j]);
                int keep, drop;
                if (si >= sj) { keep = i; drop = j; }
                else           { keep = j; drop = i; }
                g_playbook.bullets[keep].helpful += g_playbook.bullets[drop].helpful;
                g_playbook.bullets[keep].harmful += g_playbook.bullets[drop].harmful;
                g_playbook.bullets[keep].neutral += g_playbook.bullets[drop].neutral;
                /* Remove dropped bullet */
                if (drop < g_playbook.count - 1)
                    memmove(&g_playbook.bullets[drop], &g_playbook.bullets[drop + 1],
                             (size_t)(g_playbook.count - drop - 1) * sizeof(pb_bullet_t));
                g_playbook.count--;
                deduped++;
                if (drop == i) {
                    /* We dropped i (kept j which shifted down) — restart outer */
                    i--;
                    break;
                }
                /* We dropped j — re-check this j position */
                j--;
            }
        }
    }

    /* Pass 2: Prune low-scoring bullets (iterate backward for safe removal) */
    for (int i = g_playbook.count - 1; i >= 0; i--) {
        float sc = pb_score(&g_playbook.bullets[i]);
        bool should_prune = (sc < prune_threshold) ||
                            (g_playbook.count > max_bullets && sc <= 0.0f);
        if (!should_prune) continue;
        if (i < g_playbook.count - 1)
            memmove(&g_playbook.bullets[i], &g_playbook.bullets[i + 1],
                     (size_t)(g_playbook.count - i - 1) * sizeof(pb_bullet_t));
        g_playbook.count--;
        pruned++;
    }

    snprintf(result, rlen,
        "{\"deduped\":%d,\"pruned\":%d,\"remaining\":%d,"
        "\"dedup_threshold\":%.2f,\"prune_threshold\":%.2f}",
        deduped, pruned, g_playbook.count,
        PB_DEDUP_THRESHOLD, prune_threshold);
    return true;
}

/* ── scratchpad: KV working memory (separate from conversation) ──── */

#define SP_MAX_ENTRIES   64
#define SP_KEY_LEN       128
#define SP_VALUE_LEN     8192

typedef struct {
    char key[SP_KEY_LEN];
    char value[SP_VALUE_LEN];
    int  turn_written;
} sp_entry_t;

static sp_entry_t g_scratchpad[SP_MAX_ENTRIES];
static int g_sp_count = 0;

static bool tool_scratchpad(const char *input, char *result, size_t rlen) {
    if (!input) {
        snprintf(result, rlen, "{\"error\":\"input required\"}");
        return false;
    }

    /* Parse operation: get, set, delete, list, clear */
    char op[16] = "get";
    char key[SP_KEY_LEN] = "";
    char value[SP_VALUE_LEN] = "";

    const char *s = strstr(input, "\"op\"");
    if (!s) s = strstr(input, "\"operation\"");
    if (s) {
        s = strchr(s, ':');
        if (s) {
            s = strchr(s, '"'); if (s) { s++;
            const char *e = strchr(s, '"');
            if (e && (size_t)(e - s) < sizeof(op)) {
                memcpy(op, s, e - s);
                op[e - s] = '\0';
            }}
        }
    }

    s = strstr(input, "\"key\"");
    if (s) {
        s = strchr(s + 5, '"'); if (s) { s++;
        const char *e = strchr(s, '"');
        if (e && (size_t)(e - s) < SP_KEY_LEN) {
            memcpy(key, s, e - s);
            key[e - s] = '\0';
        }}
    }

    s = strstr(input, "\"value\"");
    if (s) {
        s = strchr(s + 7, '"'); if (s) { s++;
        int vi = 0;
        while (*s && vi < SP_VALUE_LEN - 1) {
            if (*s == '\\' && *(s+1)) {
                s++;
                if (*s == 'n') value[vi++] = '\n';
                else if (*s == 't') value[vi++] = '\t';
                else if (*s == '"') value[vi++] = '"';
                else if (*s == '\\') value[vi++] = '\\';
                else value[vi++] = *s;
            } else if (*s == '"') break;
            else value[vi++] = *s;
            s++;
        }
        value[vi] = '\0';
        }
    }

    if (strcasecmp(op, "set") == 0 || strcasecmp(op, "write") == 0) {
        if (!key[0] || !value[0]) {
            snprintf(result, rlen, "{\"error\":\"key and value required for set\"}");
            return false;
        }
        /* Update existing or add new */
        for (int i = 0; i < g_sp_count; i++) {
            if (strcmp(g_scratchpad[i].key, key) == 0) {
                snprintf(g_scratchpad[i].value, SP_VALUE_LEN, "%s", value);
                g_scratchpad[i].turn_written = g_playbook.current_turn;
                snprintf(result, rlen, "{\"op\":\"updated\",\"key\":\"%s\"}", key);
                return true;
            }
        }
        if (g_sp_count >= SP_MAX_ENTRIES) {
            snprintf(result, rlen, "{\"error\":\"scratchpad full (max %d entries)\"}", SP_MAX_ENTRIES);
            return false;
        }
        sp_entry_t *e = &g_scratchpad[g_sp_count++];
        snprintf(e->key, SP_KEY_LEN, "%s", key);
        snprintf(e->value, SP_VALUE_LEN, "%s", value);
        e->turn_written = g_playbook.current_turn;
        snprintf(result, rlen, "{\"op\":\"created\",\"key\":\"%s\",\"entries\":%d}", key, g_sp_count);
        return true;

    } else if (strcasecmp(op, "get") == 0 || strcasecmp(op, "read") == 0) {
        if (!key[0]) {
            snprintf(result, rlen, "{\"error\":\"key required for get\"}");
            return false;
        }
        for (int i = 0; i < g_sp_count; i++) {
            if (strcmp(g_scratchpad[i].key, key) == 0) {
                int off = 0;
                off += snprintf(result + off, rlen - off,
                    "{\"key\":\"%s\",\"turn_written\":%d,\"value\":\"",
                    key, g_scratchpad[i].turn_written);
                for (int j = 0; g_scratchpad[i].value[j] && (size_t)off < rlen - 16; j++) {
                    char c = g_scratchpad[i].value[j];
                    if (c == '"') { result[off++] = '\\'; result[off++] = '"'; }
                    else if (c == '\\') { result[off++] = '\\'; result[off++] = '\\'; }
                    else if (c == '\n') { result[off++] = '\\'; result[off++] = 'n'; }
                    else result[off++] = c;
                }
                off += snprintf(result + off, rlen - off, "\"}");
                return true;
            }
        }
        snprintf(result, rlen, "{\"error\":\"key '%s' not found\"}", key);
        return false;

    } else if (strcasecmp(op, "delete") == 0 || strcasecmp(op, "del") == 0) {
        for (int i = 0; i < g_sp_count; i++) {
            if (strcmp(g_scratchpad[i].key, key) == 0) {
                if (i < g_sp_count - 1)
                    memmove(&g_scratchpad[i], &g_scratchpad[i + 1],
                             (size_t)(g_sp_count - i - 1) * sizeof(sp_entry_t));
                g_sp_count--;
                snprintf(result, rlen, "{\"op\":\"deleted\",\"key\":\"%s\",\"remaining\":%d}", key, g_sp_count);
                return true;
            }
        }
        snprintf(result, rlen, "{\"error\":\"key '%s' not found\"}", key);
        return false;

    } else if (strcasecmp(op, "list") == 0 || strcasecmp(op, "keys") == 0) {
        int off = 0;
        off += snprintf(result + off, rlen - off, "{\"entries\":%d,\"keys\":[", g_sp_count);
        for (int i = 0; i < g_sp_count && (size_t)off < rlen - 128; i++) {
            if (i > 0) off += snprintf(result + off, rlen - off, ",");
            off += snprintf(result + off, rlen - off,
                "{\"key\":\"%s\",\"turn\":%d,\"len\":%zu}",
                g_scratchpad[i].key, g_scratchpad[i].turn_written,
                strlen(g_scratchpad[i].value));
        }
        off += snprintf(result + off, rlen - off, "]}");
        return true;

    } else if (strcasecmp(op, "clear") == 0) {
        int cleared = g_sp_count;
        g_sp_count = 0;
        snprintf(result, rlen, "{\"op\":\"cleared\",\"removed\":%d}", cleared);
        return true;
    }

    snprintf(result, rlen, "{\"error\":\"unknown op '%s' — use get/set/delete/list/clear\"}", op);
    return false;
}

/* ── context_compact: trigger conversation history compression ───── */

/* Forward: conv_compact_recent_tool_turn / conv_trim_old_results defined in llm.c */
extern bool conv_compact_recent_tool_turn(conversation_t *c, int max_chars, int protect_tail);
extern void conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);

/* We need access to the active conversation — set by agent loop */
static conversation_t *g_active_conv = NULL;

void tools_set_active_conversation(void *c) {
    g_active_conv = (conversation_t *)c;
}

void tools_playbook_advance_turn(void) {
    g_playbook.current_turn++;
}

/* ── plot: render data as Unicode charts (inline / tool result / artifact) ── */
#include "plot.h"
static bool tool_plot(const char *input, char *result, size_t rlen) {
    if (!input || !input[0]) {
        snprintf(result, rlen,
            "{\"error\":\"plot needs JSON: {\\\"type\\\":\\\"line|bar|column|area|"
            "scatter|hist|heatmap|box|candlestick|gauge|sparkline\\\",\\\"data\\\":[...]}\"}");
        return false;
    }
    int n = plot_dispatch(input, result, rlen);
    return n > 0;
}

/* ── pets: companion sprites for background agents ───────────────────────── */
#include "pets.h"
static bool tool_pets(const char *input, char *result, size_t rlen) {
    char *action = input ? json_get_str(input, "action") : NULL;
    const char *act = action ? action : "roster";

    char *buf = NULL;
    size_t bsz = 0;
    FILE *m = open_memstream(&buf, &bsz);
    if (!m) { snprintf(result, rlen, "pets: out of memory"); free(action); return false; }

    pet_roster_t *r = pet_roster_global();
    int frame = r->frame;  /* advanced by the anim clock; 0 is fine when idle */

    if (strcmp(act, "gallery") == 0) {
        pet_gallery_print(m, frame);
    } else if (strcmp(act, "roll") == 0) {
        char *seed = json_get_str(input, "seed");
        pet_t p; memset(&p, 0, sizeof(p));
        pet_roll(seed ? seed : "anon", &p.bones);
        snprintf(p.name, sizeof(p.name), "%s",
                 seed ? seed : pet_species_name(p.bones.species));
        p.status = PET_ST_IDLE;
        pet_card_print(m, &p, frame);
        free(seed);
    } else {
        pet_roster_render(m, r, tui_term_width(), 0);
    }

    fclose(m);
    snprintf(result, rlen, "%s", buf ? buf : "");
    free(buf);
    free(action);
    return true;
}

static bool tool_context_compact(const char *input, char *result, size_t rlen) {
    int keep_recent = 6;   /* keep last N messages uncompacted */
    int max_chars = 800;    /* truncate old tool results to this */
    int aggressive = 0;

    if (input) {
        const char *s = strstr(input, "\"keep_recent\"");
        if (s) {
            s += 13;
            while (*s && (*s < '0' || *s > '9')) s++;
            keep_recent = atoi(s);
            if (keep_recent < 2) keep_recent = 2;
        }
        s = strstr(input, "\"max_result_chars\"");
        if (s) {
            s += 18;
            while (*s && (*s < '0' || *s > '9')) s++;
            max_chars = atoi(s);
            if (max_chars < 100) max_chars = 100;
        }
        s = strstr(input, "\"aggressive\"");
        if (s) aggressive = 1;
    }

    if (!g_active_conv) {
        snprintf(result, rlen,
            "{\"error\":\"no active conversation — context_compact only works during agent loop\"}");
        return false;
    }

    int before_count = g_active_conv->count;

    /* Step 1: Trim old tool results */
    conv_trim_old_results(g_active_conv, keep_recent, max_chars);

    /* Step 2: Compact recent tool turns if aggressive */
    int compacted = 0;
    if (aggressive) {
        while (conv_compact_recent_tool_turn(g_active_conv, max_chars, keep_recent)) {
            compacted++;
            if (compacted > 10) break;  /* safety cap */
        }
    }

    int after_count = g_active_conv->count;

    snprintf(result, rlen,
        "{\"messages_before\":%d,\"messages_after\":%d,"
        "\"tool_turns_compacted\":%d,\"keep_recent\":%d,"
        "\"max_result_chars\":%d}",
        before_count, after_count, compacted, keep_recent, max_chars);
    return true;
}

/* ── playbook_inject: inject playbook into system context ────────── */

static bool tool_playbook_inject(const char *input, char *result, size_t rlen) {
    (void)input;
    /* Build a compact playbook string from all bullets, organized by section */
    char sections[16][PB_MAX_SECTION];
    int section_count = 0;

    /* Collect unique sections */
    for (int i = 0; i < g_playbook.count; i++) {
        bool found = false;
        for (int j = 0; j < section_count; j++)
            if (strcmp(sections[j], g_playbook.bullets[i].section) == 0) { found = true; break; }
        if (!found && section_count < 16)
            snprintf(sections[section_count++], PB_MAX_SECTION, "%s", g_playbook.bullets[i].section);
    }

    int off = 0;
    off += snprintf(result + off, rlen - off, "{\"injected_sections\":%d,\"playbook\":\"", section_count);

    for (int s = 0; s < section_count && (size_t)off < rlen - 256; s++) {
        off += snprintf(result + off, rlen - off, "## %s\\n", sections[s]);
        for (int i = 0; i < g_playbook.count && (size_t)off < rlen - 256; i++) {
            if (strcmp(g_playbook.bullets[i].section, sections[s]) != 0) continue;
            float sc = pb_score(&g_playbook.bullets[i]);
            if (sc < -1.0f) continue;  /* skip heavily penalized */

            off += snprintf(result + off, rlen - off, "- [pb-%05d] ", g_playbook.bullets[i].id);
            /* Escape content inline */
            for (int j = 0; g_playbook.bullets[i].content[j] && (size_t)off < rlen - 64; j++) {
                char c = g_playbook.bullets[i].content[j];
                if (c == '"') { result[off++] = '\\'; result[off++] = '"'; }
                else if (c == '\\') { result[off++] = '\\'; result[off++] = '\\'; }
                else if (c == '\n') { result[off++] = '\\'; result[off++] = 'n'; }
                else result[off++] = c;
            }
            off += snprintf(result + off, rlen - off, "\\n");
        }
    }
    off += snprintf(result + off, rlen - off, "\"}");
    return true;
}

/* ── Agent self-exit tool ──────────────────────────────────────────── */

static bool tool_self_exit(const char *input, char *result, size_t rlen) {
    (void)input;
    g_agent_exit_requested = 1;
    snprintf(result, rlen, "{\"status\":\"exit_scheduled\",\"message\":\"Process will terminate after this turn completes.\"}");
    return true;
}

static bool tool_self_exiting(const char *input, char *result, size_t rlen) {
    return tool_self_exit(input, result, rlen);
}

/* ── Live agent loop constructs ─────────────────────────────────────── */

#define LOOP_STACK_MAX 16
#define LOOP_LABEL_MAX 64
#define LOOP_COND_MAX 2048
#define LOOP_EXPR_MAX 1024
#define LOOP_META_MAX 16
#define LOOP_DYAD_MAX 8
#define LOOP_REFINE_MAX 8
#define LOOP_REWRITE_MAX 8
#define LOOP_MAPREDUCE_MAX 8
#define LOOP_SRM_MAX 12
#define LOOP_MEASUREMENT_MAX 12
#define LOOP_SRM_OPERATION_MAX 12
#define LOOP_GRAPH_NODE_MAX 24
#define LOOP_GRAPH_EDGE_MAX 32
#define LOOP_META_KIND_MAX 32
#define LOOP_META_NAME_MAX 48
#define LOOP_META_VALUE_MAX 128

typedef struct {
    char   kind[LOOP_META_KIND_MAX];
    char   name[LOOP_META_NAME_MAX];
    char   value[LOOP_META_VALUE_MAX];
    double weight;
} loop_meta_entry_t;

typedef struct {
    char from[LOOP_META_NAME_MAX];
    char to[LOOP_META_NAME_MAX];
    char relation[LOOP_META_KIND_MAX];
} loop_dyad_t;

typedef struct {
    char   name[LOOP_META_NAME_MAX];
    char   type[LOOP_META_NAME_MAX];
    char   state[LOOP_META_VALUE_MAX];
    double weight;
} loop_graph_node_t;

typedef struct {
    char   from[LOOP_META_NAME_MAX];
    char   to[LOOP_META_NAME_MAX];
    char   relation[LOOP_META_KIND_MAX];
    double weight;
} loop_graph_edge_t;

typedef struct {
    char   target[LOOP_META_NAME_MAX];
    char   op[3];
    double value;
    char   when[LOOP_EXPR_MAX];
    bool   fired;
} loop_refine_rule_t;

typedef struct {
    char action[LOOP_COND_MAX];
    char when[LOOP_EXPR_MAX];
    bool fired;
} loop_rewrite_rule_t;

typedef struct {
    char name[LOOP_META_NAME_MAX];
    char source[LOOP_META_NAME_MAX];
    char mapper[LOOP_META_NAME_MAX];
    char reducer[LOOP_META_NAME_MAX];
    char key[LOOP_META_NAME_MAX];
    char target[LOOP_META_NAME_MAX];
    int  partitions;
    bool mapped;
    bool shuffled;
    bool reduced;
} loop_mapreduce_job_t;

typedef struct {
    char   id[LOOP_META_NAME_MAX];
    char   name[LOOP_META_NAME_MAX];
    char   matrix[LOOP_META_VALUE_MAX];
    char   property[LOOP_META_VALUE_MAX];
    char   certificate[LOOP_META_NAME_MAX];
    char   report[LOOP_META_NAME_MAX];
    char   sds[LOOP_META_NAME_MAX];
    double assigned_value;
    double uncertainty;
    double price;
    char   destination[LOOP_META_NAME_MAX];
    char   distributor[LOOP_META_NAME_MAX];
    char   store[LOOP_META_NAME_MAX];
    bool   certificate_current;
    bool   sds_available;
    bool   traceable;
    bool   available;
    bool   orderable;
    bool   shipping_blocked;
    bool   product_search_found;
    bool   archived_certificate;
} loop_srm_entry_t;

typedef struct {
    char   name[LOOP_META_NAME_MAX];
    char   material[LOOP_META_NAME_MAX];
    char   property[LOOP_META_VALUE_MAX];
    char   unit[LOOP_META_KIND_MAX];
    char   method[LOOP_META_NAME_MAX];
    double value;
    double uncertainty;
    bool   calibrated;
    bool   has_uncertainty_budget;
} loop_measurement_entry_t;

typedef struct {
    char   kind[LOOP_META_KIND_MAX];
    char   name[LOOP_META_NAME_MAX];
    char   value[LOOP_META_VALUE_MAX];
    double amount;
    bool   flag;
} loop_srm_operation_t;

typedef struct {
    bool active;
    int  id;
    char label[LOOP_LABEL_MAX];
    char conditions[LOOP_COND_MAX];
    char continue_expr[LOOP_EXPR_MAX];
    char break_expr[LOOP_EXPR_MAX];
    char continue_prompt[DSCO_LOOP_PROMPT_MAX];
    int  max_iterations;
    int  iterations;
    int  max_turns;
    bool recursive;
    bool override_done;
    bool override_max_turns;
    bool dsl_enabled;
    loop_meta_entry_t meta[LOOP_META_MAX];
    int  meta_count;
    loop_dyad_t dyads[LOOP_DYAD_MAX];
    int  dyad_count;
    loop_graph_node_t graph_nodes[LOOP_GRAPH_NODE_MAX];
    int  graph_node_count;
    loop_graph_edge_t graph_edges[LOOP_GRAPH_EDGE_MAX];
    int  graph_edge_count;
    char traverse_from[LOOP_META_NAME_MAX];
    int  traverse_depth;
    int  traverse_hits;
    loop_refine_rule_t refine_rules[LOOP_REFINE_MAX];
    int  refine_count;
    int  refinements_applied;
    loop_rewrite_rule_t rewrite_rules[LOOP_REWRITE_MAX];
    int  rewrite_count;
    int  rewrites_applied;
    loop_mapreduce_job_t mapreduce_jobs[LOOP_MAPREDUCE_MAX];
    int  mapreduce_job_count;
    int  map_count;
    int  shuffle_count;
    int  reduce_count;
    int  partition_count;
    loop_srm_entry_t srm_entries[LOOP_SRM_MAX];
    int  srm_count;
    loop_measurement_entry_t measurements[LOOP_MEASUREMENT_MAX];
    int  measurement_count;
    int  uncertainty_budget_count;
    loop_srm_operation_t srm_operations[LOOP_SRM_OPERATION_MAX];
    int  srm_operation_count;
    double effect_conversational;
    double effect_tool;
    double effect_world;
    double effect_meta;
    double reward;
    double curiosity;
    double empowerment;
    double confidence;
    double uncertainty;
    double learning_rate;
    double valence;
    double intensity;
    double exploration_rate;
    double credit;
    double pruning_threshold;
    double basin_temperature;
    char policy[LOOP_META_NAME_MAX];
    char decision[LOOP_META_VALUE_MAX];
    char attractor[LOOP_META_NAME_MAX];
    char prompt_game[LOOP_META_NAME_MAX];
} loop_construct_t;

static loop_construct_t g_loop_stack[LOOP_STACK_MAX];
static int              g_loop_depth = 0;
static int              g_loop_next_id = 1;
static pthread_mutex_t  g_loop_lock = PTHREAD_MUTEX_INITIALIZER;

static void loop_copy(char *dst, size_t dst_len, const char *src,
                      const char *fallback) {
    if (!dst || dst_len == 0) return;
    const char *s = (src && src[0]) ? src : (fallback ? fallback : "");
    snprintf(dst, dst_len, "%s", s);
}

static int loop_clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int loop_find_locked(const char *label) {
    if (!label || !label[0]) return g_loop_depth - 1;
    for (int i = g_loop_depth - 1; i >= 0; i--) {
        if (strcmp(g_loop_stack[i].label, label) == 0) return i;
    }
    return -1;
}

static bool loop_ident_char(int ch) {
    return isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '$';
}

static const char *loop_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p ? p : "";
}

static char *loop_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static bool loop_stmt_prefix(char *s, const char *prefix, char **after) {
    s = loop_trim(s);
    size_t n = strlen(prefix);
    if (strncasecmp(s, prefix, n) != 0) return false;
    if (loop_ident_char((unsigned char)prefix[n - 1]) &&
        loop_ident_char((unsigned char)s[n])) {
        return false;
    }
    if (after) *after = loop_trim(s + n);
    return true;
}

static bool loop_stmt_assignment(char *s, const char *name, char **value) {
    s = loop_trim(s);
    size_t n = strlen(name);
    if (strncasecmp(s, name, n) != 0 || loop_ident_char((unsigned char)s[n]))
        return false;
    char *p = s + n;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '=' || *p == ':') p++;
    else if (*p && !isspace((unsigned char)*p)) return false;
    if (value) *value = loop_trim(p);
    return true;
}

static char *loop_unquote_inplace(char *s) {
    s = loop_trim(s);
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

static void loop_copy_unquoted(char *dst, size_t dst_len, const char *src,
                               const char *fallback) {
    char tmp[LOOP_COND_MAX];
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    loop_copy(dst, dst_len, loop_unquote_inplace(tmp), fallback);
}

static bool loop_parse_bool_value(const char *s, bool def) {
    s = loop_skip_ws(s);
    if (strncasecmp(s, "true", 4) == 0 || strncasecmp(s, "yes", 3) == 0 ||
        strncasecmp(s, "on", 2) == 0 || strcmp(s, "1") == 0) {
        return true;
    }
    if (strncasecmp(s, "false", 5) == 0 || strncasecmp(s, "no", 2) == 0 ||
        strncasecmp(s, "off", 3) == 0 || strcmp(s, "0") == 0) {
        return false;
    }
    return def;
}

static char *loop_strcasestr_local(char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return NULL;
    size_t n = strlen(needle);
    for (char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static double loop_clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double loop_parse_double_value(const char *s, double def) {
    s = loop_skip_ws(s);
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) return def;
    return v;
}

static int loop_graph_find_node(const loop_construct_t *c, const char *name) {
    if (!c || !name || !name[0]) return -1;
    for (int i = 0; i < c->graph_node_count; i++) {
        if (strcasecmp(c->graph_nodes[i].name, name) == 0) return i;
    }
    return -1;
}

static int loop_graph_find_edge(const loop_construct_t *c, const char *from,
                                const char *to, const char *relation) {
    if (!c || !from || !to || !from[0] || !to[0]) return -1;
    for (int i = 0; i < c->graph_edge_count; i++) {
        loop_graph_edge_t *e = (loop_graph_edge_t *)&c->graph_edges[i];
        if (strcasecmp(e->from, from) != 0 || strcasecmp(e->to, to) != 0)
            continue;
        if (!relation || !relation[0] || strcasecmp(e->relation, relation) == 0)
            return i;
    }
    return -1;
}

static double loop_graph_density(const loop_construct_t *c) {
    if (!c || c->graph_node_count <= 1) return 0.0;
    double possible = (double)c->graph_node_count * (double)(c->graph_node_count - 1);
    return (double)c->graph_edge_count / possible;
}

static int loop_graph_count_relation(const loop_construct_t *c,
                                     const char *relation) {
    if (!c || !relation || !relation[0]) return 0;
    int n = 0;
    for (int i = 0; i < c->graph_edge_count; i++) {
        if (strcasecmp(c->graph_edges[i].relation, relation) == 0) n++;
    }
    return n;
}

static void loop_graph_add_node(loop_construct_t *c, const char *name,
                                const char *type, const char *state,
                                double weight) {
    if (!c || !name || !name[0]) return;
    char clean_name[LOOP_META_NAME_MAX];
    char clean_type[LOOP_META_NAME_MAX];
    char clean_state[LOOP_META_VALUE_MAX];
    loop_copy_unquoted(clean_name, sizeof(clean_name), name, "node");
    loop_copy_unquoted(clean_type, sizeof(clean_type), type, "");
    loop_copy_unquoted(clean_state, sizeof(clean_state), state, "");

    int idx = loop_graph_find_node(c, clean_name);
    if (idx < 0) {
        if (c->graph_node_count >= LOOP_GRAPH_NODE_MAX) return;
        idx = c->graph_node_count++;
        memset(&c->graph_nodes[idx], 0, sizeof(c->graph_nodes[idx]));
        loop_copy(c->graph_nodes[idx].name, sizeof(c->graph_nodes[idx].name),
                  clean_name, "node");
        loop_copy(c->graph_nodes[idx].type, sizeof(c->graph_nodes[idx].type),
                  clean_type[0] ? clean_type : "object", "object");
    }
    if (clean_type[0])
        loop_copy(c->graph_nodes[idx].type, sizeof(c->graph_nodes[idx].type),
                  clean_type, "object");
    if (clean_state[0])
        loop_copy(c->graph_nodes[idx].state, sizeof(c->graph_nodes[idx].state),
                  clean_state, NULL);
    c->graph_nodes[idx].weight = weight;
    c->dsl_enabled = true;
}

static void loop_graph_remove_node(loop_construct_t *c, const char *name) {
    if (!c || !name || !name[0]) return;
    char clean_name[LOOP_META_NAME_MAX];
    loop_copy_unquoted(clean_name, sizeof(clean_name), name, NULL);
    int idx = loop_graph_find_node(c, clean_name);
    if (idx >= 0) {
        for (int i = idx + 1; i < c->graph_node_count; i++)
            c->graph_nodes[i - 1] = c->graph_nodes[i];
        c->graph_node_count--;
        memset(&c->graph_nodes[c->graph_node_count], 0,
               sizeof(c->graph_nodes[c->graph_node_count]));
    }
    for (int i = 0; i < c->graph_edge_count; ) {
        loop_graph_edge_t *e = &c->graph_edges[i];
        if (strcasecmp(e->from, clean_name) == 0 ||
            strcasecmp(e->to, clean_name) == 0) {
            for (int j = i + 1; j < c->graph_edge_count; j++)
                c->graph_edges[j - 1] = c->graph_edges[j];
            c->graph_edge_count--;
            memset(&c->graph_edges[c->graph_edge_count], 0,
                   sizeof(c->graph_edges[c->graph_edge_count]));
        } else {
            i++;
        }
    }
    c->dsl_enabled = true;
}

static void loop_graph_replace_node(loop_construct_t *c, const char *old_name,
                                    const char *new_name) {
    if (!c || !old_name || !new_name || !old_name[0] || !new_name[0]) return;
    char old_clean[LOOP_META_NAME_MAX];
    char new_clean[LOOP_META_NAME_MAX];
    loop_copy_unquoted(old_clean, sizeof(old_clean), old_name, NULL);
    loop_copy_unquoted(new_clean, sizeof(new_clean), new_name, NULL);
    int idx = loop_graph_find_node(c, old_clean);
    if (idx < 0) {
        loop_graph_add_node(c, new_clean, "object", NULL, 0.0);
    } else {
        loop_copy(c->graph_nodes[idx].name, sizeof(c->graph_nodes[idx].name),
                  new_clean, NULL);
    }
    for (int i = 0; i < c->graph_edge_count; i++) {
        if (strcasecmp(c->graph_edges[i].from, old_clean) == 0)
            loop_copy(c->graph_edges[i].from, sizeof(c->graph_edges[i].from),
                      new_clean, NULL);
        if (strcasecmp(c->graph_edges[i].to, old_clean) == 0)
            loop_copy(c->graph_edges[i].to, sizeof(c->graph_edges[i].to),
                      new_clean, NULL);
    }
    c->dsl_enabled = true;
}

static void loop_graph_add_edge(loop_construct_t *c, const char *from,
                                const char *to, const char *relation,
                                double weight) {
    if (!c || !from || !to || !from[0] || !to[0]) return;
    char clean_from[LOOP_META_NAME_MAX];
    char clean_to[LOOP_META_NAME_MAX];
    char clean_rel[LOOP_META_KIND_MAX];
    loop_copy_unquoted(clean_from, sizeof(clean_from), from, "object");
    loop_copy_unquoted(clean_to, sizeof(clean_to), to, "object");
    loop_copy_unquoted(clean_rel, sizeof(clean_rel), relation, "relates");
    if (loop_graph_find_node(c, clean_from) < 0)
        loop_graph_add_node(c, clean_from, "object", NULL, 0.0);
    if (loop_graph_find_node(c, clean_to) < 0)
        loop_graph_add_node(c, clean_to, "object", NULL, 0.0);

    int idx = loop_graph_find_edge(c, clean_from, clean_to, clean_rel);
    if (idx < 0) {
        if (c->graph_edge_count >= LOOP_GRAPH_EDGE_MAX) return;
        idx = c->graph_edge_count++;
        memset(&c->graph_edges[idx], 0, sizeof(c->graph_edges[idx]));
    }
    loop_copy(c->graph_edges[idx].from, sizeof(c->graph_edges[idx].from),
              clean_from, NULL);
    loop_copy(c->graph_edges[idx].to, sizeof(c->graph_edges[idx].to),
              clean_to, NULL);
    loop_copy(c->graph_edges[idx].relation, sizeof(c->graph_edges[idx].relation),
              clean_rel, "relates");
    c->graph_edges[idx].weight = weight;
    c->dsl_enabled = true;
}

static void loop_graph_remove_edge(loop_construct_t *c, const char *from,
                                   const char *to, const char *relation) {
    if (!c || !from || !to || !from[0] || !to[0]) return;
    char clean_from[LOOP_META_NAME_MAX];
    char clean_to[LOOP_META_NAME_MAX];
    char clean_rel[LOOP_META_KIND_MAX];
    loop_copy_unquoted(clean_from, sizeof(clean_from), from, NULL);
    loop_copy_unquoted(clean_to, sizeof(clean_to), to, NULL);
    loop_copy_unquoted(clean_rel, sizeof(clean_rel), relation, "");
    for (int i = 0; i < c->graph_edge_count; ) {
        loop_graph_edge_t *e = &c->graph_edges[i];
        bool rel_match = !clean_rel[0] || strcasecmp(e->relation, clean_rel) == 0;
        if (strcasecmp(e->from, clean_from) == 0 &&
            strcasecmp(e->to, clean_to) == 0 && rel_match) {
            for (int j = i + 1; j < c->graph_edge_count; j++)
                c->graph_edges[j - 1] = c->graph_edges[j];
            c->graph_edge_count--;
            memset(&c->graph_edges[c->graph_edge_count], 0,
                   sizeof(c->graph_edges[c->graph_edge_count]));
        } else {
            i++;
        }
    }
    c->dsl_enabled = true;
}

static int loop_graph_traverse_count(const loop_construct_t *c,
                                     const char *start, int depth) {
    if (!c || !start || !start[0] || depth < 0) return 0;
    char clean_start[LOOP_META_NAME_MAX];
    loop_copy_unquoted(clean_start, sizeof(clean_start), start, NULL);
    int start_idx = loop_graph_find_node(c, clean_start);
    if (start_idx < 0) return 0;

    bool seen[LOOP_GRAPH_NODE_MAX] = { false };
    int frontier[LOOP_GRAPH_NODE_MAX];
    int next[LOOP_GRAPH_NODE_MAX];
    int fcount = 1;
    int hits = 1;
    frontier[0] = start_idx;
    seen[start_idx] = true;

    for (int level = 0; level < depth && fcount > 0; level++) {
        int ncount = 0;
        for (int fi = 0; fi < fcount; fi++) {
            const char *from = c->graph_nodes[frontier[fi]].name;
            for (int e = 0; e < c->graph_edge_count; e++) {
                if (strcasecmp(c->graph_edges[e].from, from) != 0) continue;
                int ni = loop_graph_find_node(c, c->graph_edges[e].to);
                if (ni < 0 || seen[ni]) continue;
                seen[ni] = true;
                hits++;
                if (ncount < LOOP_GRAPH_NODE_MAX) next[ncount++] = ni;
            }
        }
        for (int i = 0; i < ncount; i++) frontier[i] = next[i];
        fcount = ncount;
    }
    return hits;
}

static int loop_meta_count_kind(const loop_construct_t *c, const char *kind) {
    if (!c || !kind) return 0;
    int n = 0;
    for (int i = 0; i < c->meta_count; i++) {
        if (strcasecmp(c->meta[i].kind, kind) == 0) n++;
    }
    return n;
}

static void loop_meta_add(loop_construct_t *c, const char *kind,
                          const char *name, const char *value,
                          double weight) {
    if (!c || c->meta_count >= LOOP_META_MAX) return;
    loop_meta_entry_t *m = &c->meta[c->meta_count++];
    memset(m, 0, sizeof(*m));
    loop_copy(m->kind, sizeof(m->kind), kind, "meta");
    loop_copy_unquoted(m->name, sizeof(m->name), name, "item");
    loop_copy_unquoted(m->value, sizeof(m->value), value, "");
    if (strcasecmp(m->kind, "define") == 0 || strcasecmp(m->kind, "object") == 0) {
        loop_graph_add_node(c, m->name, m->value[0] ? m->value : m->kind,
                            NULL, weight);
    }
    m->weight = weight;
    c->dsl_enabled = true;
}

static bool loop_name_matches(const char *name, const char *a, const char *b,
                              const char *c) {
    return (a && strcasecmp(name, a) == 0) ||
           (b && strcasecmp(name, b) == 0) ||
           (c && strcasecmp(name, c) == 0);
}

static void loop_set_effect(loop_construct_t *c, const char *name, double value) {
    if (!c || !name) return;
    value = loop_clamp01(value);
    if (loop_name_matches(name, "conversational", "conversation", "dialogue"))
        c->effect_conversational = value;
    else if (loop_name_matches(name, "tool", "tools", "tool_calling"))
        c->effect_tool = value;
    else if (loop_name_matches(name, "world", "world_model", "world_modeling"))
        c->effect_world = value;
    else if (loop_name_matches(name, "meta", "meta_learning", "reflection"))
        c->effect_meta = value;
    c->dsl_enabled = true;
}

static void loop_set_signal(loop_construct_t *c, const char *name, double value) {
    if (!c || !name) return;
    if (loop_name_matches(name, "reward", "external_reward", "value"))
        c->reward = value;
    else if (loop_name_matches(name, "curiosity", "intrinsic", "intrinsic_reward"))
        c->curiosity = value;
    else if (loop_name_matches(name, "empowerment", "agency", "control"))
        c->empowerment = value;
    else if (loop_name_matches(name, "confidence", "belief_confidence", "certainty"))
        c->confidence = loop_clamp01(value);
    else if (loop_name_matches(name, "uncertainty", "entropy", "risk"))
        c->uncertainty = loop_clamp01(value);
    else if (loop_name_matches(name, "learning_rate", "learn_rate", "rate"))
        c->learning_rate = loop_clamp01(value);
    else if (loop_name_matches(name, "valence", "reward_valence", NULL))
        c->valence = value;
    else if (loop_name_matches(name, "intensity", "reward_intensity", NULL))
        c->intensity = loop_clamp01(value);
    else if (loop_name_matches(name, "exploration_rate", "explore_rate", "epsilon"))
        c->exploration_rate = loop_clamp01(value);
    else if (loop_name_matches(name, "credit", "credit_assignment", NULL))
        c->credit = value;
    else if (loop_name_matches(name, "pruning_threshold", "prune_threshold", NULL))
        c->pruning_threshold = loop_clamp01(value);
    else if (loop_name_matches(name, "basin_temperature", "basin_hop", "temperature"))
        c->basin_temperature = loop_clamp01(value);
    c->dsl_enabled = true;
}

static void loop_program_meta_reset(loop_construct_t *c) {
    if (!c) return;
    memset(c->meta, 0, sizeof(c->meta));
    c->meta_count = 0;
    memset(c->dyads, 0, sizeof(c->dyads));
    c->dyad_count = 0;
    memset(c->graph_nodes, 0, sizeof(c->graph_nodes));
    c->graph_node_count = 0;
    memset(c->graph_edges, 0, sizeof(c->graph_edges));
    c->graph_edge_count = 0;
    c->traverse_from[0] = '\0';
    c->traverse_depth = 0;
    c->traverse_hits = 0;
    memset(c->refine_rules, 0, sizeof(c->refine_rules));
    c->refine_count = 0;
    c->refinements_applied = 0;
    memset(c->rewrite_rules, 0, sizeof(c->rewrite_rules));
    c->rewrite_count = 0;
    c->rewrites_applied = 0;
    memset(c->mapreduce_jobs, 0, sizeof(c->mapreduce_jobs));
    c->mapreduce_job_count = 0;
    c->map_count = 0;
    c->shuffle_count = 0;
    c->reduce_count = 0;
    c->partition_count = 0;
    memset(c->srm_entries, 0, sizeof(c->srm_entries));
    c->srm_count = 0;
    memset(c->measurements, 0, sizeof(c->measurements));
    c->measurement_count = 0;
    c->uncertainty_budget_count = 0;
    memset(c->srm_operations, 0, sizeof(c->srm_operations));
    c->srm_operation_count = 0;
    c->effect_conversational = 0.0;
    c->effect_tool = 0.0;
    c->effect_world = 0.0;
    c->effect_meta = 0.0;
    c->reward = 0.0;
    c->curiosity = 0.0;
    c->empowerment = 0.0;
    c->confidence = 0.0;
    c->uncertainty = 0.0;
    c->learning_rate = 0.0;
    c->valence = 0.0;
    c->intensity = 0.0;
    c->exploration_rate = 0.0;
    c->credit = 0.0;
    c->pruning_threshold = 0.0;
    c->basin_temperature = 0.0;
    c->policy[0] = '\0';
    c->decision[0] = '\0';
    c->attractor[0] = '\0';
    c->prompt_game[0] = '\0';
}

static void loop_pop_from_locked(int idx) {
    if (idx < 0) return;
    if (idx > g_loop_depth) idx = g_loop_depth;
    for (int i = idx; i < g_loop_depth; i++)
        memset(&g_loop_stack[i], 0, sizeof(g_loop_stack[i]));
    g_loop_depth = idx;
}

typedef struct {
    const char             *p;
    const loop_construct_t *c;
    int                     current_turn;
    int                     depth;
    bool                    model_done;
    bool                    has_followup;
    bool                    ok;
    char                    error[128];
} loop_expr_parser_t;

static void loop_expr_error(loop_expr_parser_t *ps, const char *msg) {
    if (!ps || !ps->ok) return;
    ps->ok = false;
    snprintf(ps->error, sizeof(ps->error), "%s", msg ? msg : "invalid expression");
}

static bool loop_expr_consume(loop_expr_parser_t *ps, const char *lit) {
    ps->p = loop_skip_ws(ps->p);
    size_t n = strlen(lit);
    if (strncmp(ps->p, lit, n) != 0) return false;
    ps->p += n;
    return true;
}

static bool loop_expr_keyword(loop_expr_parser_t *ps, const char *kw) {
    ps->p = loop_skip_ws(ps->p);
    size_t n = strlen(kw);
    if (strncasecmp(ps->p, kw, n) != 0 || loop_ident_char((unsigned char)ps->p[n]))
        return false;
    ps->p += n;
    return true;
}

static double loop_expr_parse_or(loop_expr_parser_t *ps);
static int loop_srm_certificate_count(const loop_construct_t *c);
static int loop_srm_current_certificate_count(const loop_construct_t *c);
static int loop_srm_sds_count(const loop_construct_t *c);
static int loop_srm_traceability_count(const loop_construct_t *c);
static int loop_srm_available_count(const loop_construct_t *c);
static int loop_srm_orderable_count(const loop_construct_t *c);
static int loop_srm_shipping_block_count(const loop_construct_t *c);
static int loop_srm_product_search_count(const loop_construct_t *c);
static int loop_srm_archived_certificate_count(const loop_construct_t *c);
static double loop_srm_total_price(const loop_construct_t *c);
static int loop_srm_operation_count_kind(const loop_construct_t *c,
                                         const char *kind);
static bool loop_srm_operation_has(const loop_construct_t *c,
                                   const char *kind, const char *name);
static double loop_srm_mean_uncertainty(const loop_construct_t *c);
static double loop_srm_max_uncertainty(const loop_construct_t *c);
static int loop_measurement_calibration_count(const loop_construct_t *c);

static double loop_expr_variable(loop_expr_parser_t *ps) {
    ps->p = loop_skip_ws(ps->p);
    const char *start = ps->p;
    if (*start == '$') start++;
    if (!isalpha((unsigned char)*start) && *start != '_') {
        loop_expr_error(ps, "expected variable");
        return 0.0;
    }
    const char *p = start;
    while (loop_ident_char((unsigned char)*p)) p++;
    char name[80];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)tolower((unsigned char)start[i]);
    name[n] = '\0';
    ps->p = p;

    if (strcmp(name, "true") == 0) return 1.0;
    if (strcmp(name, "false") == 0) return 0.0;
    if (strcmp(name, "iteration") == 0 || strcmp(name, "iterations") == 0 ||
        strcmp(name, "c.iteration") == 0 || strcmp(name, "c.iterations") == 0)
        return ps->c ? ps->c->iterations : 0.0;
    if (strcmp(name, "next_iteration") == 0 || strcmp(name, "c.next_iteration") == 0)
        return ps->c ? ps->c->iterations + 1 : 1.0;
    if (strcmp(name, "remaining") == 0 || strcmp(name, "c.remaining") == 0)
        return ps->c ? ps->c->max_iterations - ps->c->iterations : 0.0;
    if (strcmp(name, "max_iterations") == 0 || strcmp(name, "c.max_iterations") == 0)
        return ps->c ? ps->c->max_iterations : 0.0;
    if (strcmp(name, "max_turns") == 0 || strcmp(name, "c.max_turns") == 0)
        return ps->c ? ps->c->max_turns : 0.0;
    if (strcmp(name, "turn") == 0 || strcmp(name, "current_turn") == 0 ||
        strcmp(name, "c.turn") == 0)
        return ps->current_turn;
    if (strcmp(name, "depth") == 0 || strcmp(name, "c.depth") == 0)
        return ps->depth;
    if (strcmp(name, "model_done") == 0 || strcmp(name, "done") == 0)
        return ps->model_done ? 1.0 : 0.0;
    if (strcmp(name, "has_followup") == 0 || strcmp(name, "followup") == 0)
        return ps->has_followup ? 1.0 : 0.0;
    if (strcmp(name, "override_done") == 0 || strcmp(name, "c.override_done") == 0)
        return ps->c && ps->c->override_done ? 1.0 : 0.0;
    if (strcmp(name, "override_max_turns") == 0 ||
        strcmp(name, "c.override_max_turns") == 0)
        return ps->c && ps->c->override_max_turns ? 1.0 : 0.0;
    if (strcmp(name, "recursive") == 0 || strcmp(name, "c.recursive") == 0)
        return ps->c && ps->c->recursive ? 1.0 : 0.0;
    if (strcmp(name, "meta_count") == 0 || strcmp(name, "c.meta_count") == 0)
        return ps->c ? ps->c->meta_count : 0.0;
    if (strcmp(name, "definition_count") == 0 || strcmp(name, "define_count") == 0 ||
        strcmp(name, "definitions") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "define") : 0.0;
    if (strcmp(name, "reward_object_count") == 0 ||
        strcmp(name, "reward_objects") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "reward_object") : 0.0;
    if (strcmp(name, "goal_count") == 0 || strcmp(name, "goals") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "goal") : 0.0;
    if (strcmp(name, "task_count") == 0 || strcmp(name, "tasks") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "task") : 0.0;
    if (strcmp(name, "belief_count") == 0 || strcmp(name, "beliefs") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "belief") : 0.0;
    if (strcmp(name, "object_count") == 0 || strcmp(name, "objects") == 0)
        return ps->c ? loop_meta_count_kind(ps->c, "object") : 0.0;
    if (strcmp(name, "dyad_count") == 0 || strcmp(name, "dyads") == 0 ||
        strcmp(name, "interaction_count") == 0)
        return ps->c ? ps->c->dyad_count : 0.0;
    if (strcmp(name, "node_count") == 0 || strcmp(name, "nodes") == 0 ||
        strcmp(name, "graph_node_count") == 0)
        return ps->c ? ps->c->graph_node_count : 0.0;
    if (strcmp(name, "edge_count") == 0 || strcmp(name, "edges") == 0 ||
        strcmp(name, "graph_edge_count") == 0)
        return ps->c ? ps->c->graph_edge_count : 0.0;
    if (strcmp(name, "causal_link_count") == 0 || strcmp(name, "causal_links") == 0)
        return ps->c ? loop_graph_count_relation(ps->c, "causal") : 0.0;
    if (strcmp(name, "message_count") == 0 || strcmp(name, "messages") == 0 ||
        strcmp(name, "message_link_count") == 0)
        return ps->c ? loop_graph_count_relation(ps->c, "message") : 0.0;
    if (strcmp(name, "graph_density") == 0 || strcmp(name, "density") == 0)
        return ps->c ? loop_graph_density(ps->c) : 0.0;
    if (strcmp(name, "traverse_count") == 0 || strcmp(name, "traversal_count") == 0 ||
        strcmp(name, "traverse_hits") == 0)
        return ps->c ? ps->c->traverse_hits : 0.0;
    if (strcmp(name, "traverse_depth") == 0 || strcmp(name, "traversal_depth") == 0)
        return ps->c ? ps->c->traverse_depth : 0.0;
    if (strcmp(name, "mapreduce_count") == 0 ||
        strcmp(name, "mapreduce_job_count") == 0 ||
        strcmp(name, "map_reduce_count") == 0)
        return ps->c ? ps->c->mapreduce_job_count : 0.0;
    if (strcmp(name, "map_count") == 0 || strcmp(name, "maps") == 0 ||
        strcmp(name, "mapped_count") == 0)
        return ps->c ? ps->c->map_count : 0.0;
    if (strcmp(name, "shuffle_count") == 0 || strcmp(name, "shuffles") == 0 ||
        strcmp(name, "partition_stage_count") == 0)
        return ps->c ? ps->c->shuffle_count : 0.0;
    if (strcmp(name, "reduce_count") == 0 || strcmp(name, "reduces") == 0 ||
        strcmp(name, "reduced_count") == 0)
        return ps->c ? ps->c->reduce_count : 0.0;
    if (strcmp(name, "partition_count") == 0 || strcmp(name, "partitions") == 0)
        return ps->c ? ps->c->partition_count : 0.0;
    if (strcmp(name, "srm_count") == 0 ||
        strcmp(name, "reference_material_count") == 0 ||
        strcmp(name, "rm_count") == 0)
        return ps->c ? ps->c->srm_count : 0.0;
    if (strcmp(name, "certificate_count") == 0 ||
        strcmp(name, "cert_count") == 0)
        return ps->c ? loop_srm_certificate_count(ps->c) : 0.0;
    if (strcmp(name, "current_certificate_count") == 0 ||
        strcmp(name, "current_cert_count") == 0)
        return ps->c ? loop_srm_current_certificate_count(ps->c) : 0.0;
    if (strcmp(name, "sds_count") == 0 ||
        strcmp(name, "safety_data_sheet_count") == 0)
        return ps->c ? loop_srm_sds_count(ps->c) : 0.0;
    if (strcmp(name, "traceability_count") == 0 ||
        strcmp(name, "traceable_count") == 0 ||
        strcmp(name, "metrological_traceability") == 0)
        return ps->c ? loop_srm_traceability_count(ps->c) : 0.0;
    if (strcmp(name, "available_count") == 0 ||
        strcmp(name, "srm_available_count") == 0 ||
        strcmp(name, "availability_count") == 0)
        return ps->c ? loop_srm_available_count(ps->c) : 0.0;
    if (strcmp(name, "orderable_count") == 0 ||
        strcmp(name, "srm_orderable_count") == 0)
        return ps->c ? loop_srm_orderable_count(ps->c) : 0.0;
    if (strcmp(name, "shipping_block_count") == 0 ||
        strcmp(name, "shipping_restriction_count") == 0 ||
        strcmp(name, "restricted_shipping_count") == 0)
        return ps->c ? loop_srm_shipping_block_count(ps->c) : 0.0;
    if (strcmp(name, "product_search_count") == 0 ||
        strcmp(name, "store_search_count") == 0 ||
        strcmp(name, "search_result_count") == 0)
        return ps->c ? loop_srm_product_search_count(ps->c) : 0.0;
    if (strcmp(name, "archived_certificate_count") == 0 ||
        strcmp(name, "archive_count") == 0)
        return ps->c ? loop_srm_archived_certificate_count(ps->c) : 0.0;
    if (strcmp(name, "catalog_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "catalog") : 0.0;
    if (strcmp(name, "annual_catalog_count") == 0 ||
        strcmp(name, "annual_product_list_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "annual_catalog") : 0.0;
    if (strcmp(name, "licensed_distributor_count") == 0 ||
        strcmp(name, "distributor_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "licensed_distributor") : 0.0;
    if (strcmp(name, "order_policy_count") == 0 ||
        strcmp(name, "policy_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "order_policy") : 0.0;
    if (strcmp(name, "paper_checks_blocked") == 0 ||
        strcmp(name, "paper_check_blocked") == 0 ||
        strcmp(name, "no_paper_checks") == 0)
        return ps->c && loop_srm_operation_has(ps->c, "order_policy", "no_paper_checks")
                   ? 1.0 : 0.0;
    if (strcmp(name, "registration_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "registration") : 0.0;
    if (strcmp(name, "survey_count") == 0)
        return ps->c ? loop_srm_operation_count_kind(ps->c, "survey") : 0.0;
    if (strcmp(name, "price_total") == 0 ||
        strcmp(name, "srm_price_total") == 0)
        return ps->c ? loop_srm_total_price(ps->c) : 0.0;
    if (strcmp(name, "measurement_count") == 0 ||
        strcmp(name, "measurements") == 0)
        return ps->c ? ps->c->measurement_count : 0.0;
    if (strcmp(name, "calibration_count") == 0 ||
        strcmp(name, "calibrations") == 0)
        return ps->c ? loop_measurement_calibration_count(ps->c) : 0.0;
    if (strcmp(name, "uncertainty_budget_count") == 0 ||
        strcmp(name, "uncertainty_budgets") == 0)
        return ps->c ? ps->c->uncertainty_budget_count : 0.0;
    if (strcmp(name, "mean_uncertainty") == 0 ||
        strcmp(name, "average_uncertainty") == 0)
        return ps->c ? loop_srm_mean_uncertainty(ps->c) : 0.0;
    if (strcmp(name, "max_uncertainty") == 0 ||
        strcmp(name, "uncertainty_max") == 0)
        return ps->c ? loop_srm_max_uncertainty(ps->c) : 0.0;
    if (strcmp(name, "refine_count") == 0 || strcmp(name, "refinements") == 0)
        return ps->c ? ps->c->refine_count : 0.0;
    if (strcmp(name, "refinements_applied") == 0 || strcmp(name, "refine_applied") == 0)
        return ps->c ? ps->c->refinements_applied : 0.0;
    if (strcmp(name, "rewrite_count") == 0 || strcmp(name, "rewrites") == 0 ||
        strcmp(name, "schema_rewrite_count") == 0)
        return ps->c ? ps->c->rewrite_count : 0.0;
    if (strcmp(name, "rewrites_applied") == 0 || strcmp(name, "rewrite_applied") == 0 ||
        strcmp(name, "schema_rewrites_applied") == 0)
        return ps->c ? ps->c->rewrites_applied : 0.0;
    if (strcmp(name, "effect.conversational") == 0 ||
        strcmp(name, "effect.conversation") == 0)
        return ps->c ? ps->c->effect_conversational : 0.0;
    if (strcmp(name, "effect.tool") == 0 ||
        strcmp(name, "effect.tools") == 0 ||
        strcmp(name, "effect.tool_calling") == 0)
        return ps->c ? ps->c->effect_tool : 0.0;
    if (strcmp(name, "effect.world") == 0 ||
        strcmp(name, "effect.world_modeling") == 0)
        return ps->c ? ps->c->effect_world : 0.0;
    if (strcmp(name, "effect.meta") == 0 ||
        strcmp(name, "effect.meta_learning") == 0 ||
        strcmp(name, "effect.reflection") == 0)
        return ps->c ? ps->c->effect_meta : 0.0;
    if (strcmp(name, "reward") == 0 || strcmp(name, "external_reward") == 0)
        return ps->c ? ps->c->reward : 0.0;
    if (strcmp(name, "curiosity") == 0 || strcmp(name, "intrinsic_reward") == 0)
        return ps->c ? ps->c->curiosity : 0.0;
    if (strcmp(name, "empowerment") == 0 || strcmp(name, "agency") == 0)
        return ps->c ? ps->c->empowerment : 0.0;
    if (strcmp(name, "confidence") == 0 || strcmp(name, "certainty") == 0)
        return ps->c ? ps->c->confidence : 0.0;
    if (strcmp(name, "uncertainty") == 0 || strcmp(name, "entropy") == 0)
        return ps->c ? ps->c->uncertainty : 0.0;
    if (strcmp(name, "learning_rate") == 0 || strcmp(name, "learn_rate") == 0)
        return ps->c ? ps->c->learning_rate : 0.0;
    if (strcmp(name, "valence") == 0 || strcmp(name, "reward_valence") == 0)
        return ps->c ? ps->c->valence : 0.0;
    if (strcmp(name, "intensity") == 0 || strcmp(name, "reward_intensity") == 0)
        return ps->c ? ps->c->intensity : 0.0;
    if (strcmp(name, "exploration_rate") == 0 || strcmp(name, "explore_rate") == 0 ||
        strcmp(name, "epsilon") == 0)
        return ps->c ? ps->c->exploration_rate : 0.0;
    if (strcmp(name, "credit") == 0 || strcmp(name, "credit_assignment") == 0)
        return ps->c ? ps->c->credit : 0.0;
    if (strcmp(name, "pruning_threshold") == 0 || strcmp(name, "prune_threshold") == 0)
        return ps->c ? ps->c->pruning_threshold : 0.0;
    if (strcmp(name, "basin_temperature") == 0 || strcmp(name, "basin_hop") == 0)
        return ps->c ? ps->c->basin_temperature : 0.0;

    loop_expr_error(ps, "unknown variable");
    return 0.0;
}

static double loop_expr_primary(loop_expr_parser_t *ps) {
    ps->p = loop_skip_ws(ps->p);
    if (loop_expr_consume(ps, "(")) {
        double v = loop_expr_parse_or(ps);
        if (!loop_expr_consume(ps, ")")) loop_expr_error(ps, "expected ')'");
        return v;
    }

    char *end = NULL;
    double v = strtod(ps->p, &end);
    if (end && end != ps->p) {
        ps->p = end;
        return v;
    }
    return loop_expr_variable(ps);
}

static double loop_expr_unary(loop_expr_parser_t *ps) {
    if (loop_expr_consume(ps, "!") || loop_expr_keyword(ps, "not"))
        return loop_expr_unary(ps) == 0.0 ? 1.0 : 0.0;
    if (loop_expr_consume(ps, "-"))
        return -loop_expr_unary(ps);
    return loop_expr_primary(ps);
}

static double loop_expr_mul(loop_expr_parser_t *ps) {
    double v = loop_expr_unary(ps);
    while (ps->ok) {
        if (loop_expr_consume(ps, "*")) {
            v *= loop_expr_unary(ps);
        } else if (loop_expr_consume(ps, "/")) {
            double rhs = loop_expr_unary(ps);
            if (rhs == 0.0) { loop_expr_error(ps, "division by zero"); return 0.0; }
            v /= rhs;
        } else if (loop_expr_consume(ps, "%")) {
            double rhs = loop_expr_unary(ps);
            if (rhs == 0.0) { loop_expr_error(ps, "modulo by zero"); return 0.0; }
            v = fmod(v, rhs);
        } else {
            break;
        }
    }
    return v;
}

static double loop_expr_add(loop_expr_parser_t *ps) {
    double v = loop_expr_mul(ps);
    while (ps->ok) {
        if (loop_expr_consume(ps, "+")) v += loop_expr_mul(ps);
        else if (loop_expr_consume(ps, "-")) v -= loop_expr_mul(ps);
        else break;
    }
    return v;
}

static double loop_expr_compare(loop_expr_parser_t *ps) {
    double v = loop_expr_add(ps);
    while (ps->ok) {
        if (loop_expr_consume(ps, "<=")) v = v <= loop_expr_add(ps) ? 1.0 : 0.0;
        else if (loop_expr_consume(ps, ">=")) v = v >= loop_expr_add(ps) ? 1.0 : 0.0;
        else if (loop_expr_consume(ps, "==")) v = fabs(v - loop_expr_add(ps)) < 1e-9 ? 1.0 : 0.0;
        else if (loop_expr_consume(ps, "!=")) v = fabs(v - loop_expr_add(ps)) >= 1e-9 ? 1.0 : 0.0;
        else if (loop_expr_consume(ps, "<")) v = v < loop_expr_add(ps) ? 1.0 : 0.0;
        else if (loop_expr_consume(ps, ">")) v = v > loop_expr_add(ps) ? 1.0 : 0.0;
        else break;
    }
    return v;
}

static double loop_expr_and(loop_expr_parser_t *ps) {
    double v = loop_expr_compare(ps);
    while (ps->ok) {
        if (loop_expr_consume(ps, "&&") || loop_expr_keyword(ps, "and")) {
            double rhs = loop_expr_compare(ps);
            v = (v != 0.0 && rhs != 0.0) ? 1.0 : 0.0;
        } else {
            break;
        }
    }
    return v;
}

static double loop_expr_parse_or(loop_expr_parser_t *ps) {
    double v = loop_expr_and(ps);
    while (ps->ok) {
        if (loop_expr_consume(ps, "||") || loop_expr_keyword(ps, "or")) {
            double rhs = loop_expr_and(ps);
            v = (v != 0.0 || rhs != 0.0) ? 1.0 : 0.0;
        } else {
            break;
        }
    }
    return v;
}

static bool loop_eval_expr_bool(const loop_construct_t *c, const char *expr,
                                int current_turn, bool model_done,
                                bool has_followup, int depth,
                                bool *valid, char *err, size_t err_len) {
    loop_expr_parser_t ps = {
        .p = expr ? expr : "",
        .c = c,
        .current_turn = current_turn,
        .depth = depth,
        .model_done = model_done,
        .has_followup = has_followup,
        .ok = true
    };
    double v = loop_expr_parse_or(&ps);
    ps.p = loop_skip_ws(ps.p);
    if (ps.ok && *ps.p && *ps.p != '#') loop_expr_error(&ps, "unexpected trailing input");
    if (valid) *valid = ps.ok;
    if (!ps.ok && err && err_len > 0)
        snprintf(err, err_len, "%s", ps.error[0] ? ps.error : "invalid expression");
    return ps.ok && v != 0.0;
}

static void loop_meta_from_remainder(loop_construct_t *c, const char *kind,
                                     char *rem) {
    if (!c || !kind || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    double weight = 0.0;
    char *weight_kw = loop_strcasestr_local(rem, " weight ");
    if (!weight_kw) weight_kw = loop_strcasestr_local(rem, " priority ");
    if (!weight_kw) weight_kw = loop_strcasestr_local(rem, " confidence ");
    if (weight_kw) {
        char *num = weight_kw;
        while (*num && !isdigit((unsigned char)*num) && *num != '-' && *num != '.')
            num++;
        weight = loop_parse_double_value(num, 0.0);
        *weight_kw = '\0';
        rem = loop_trim(rem);
    }

    char *name = rem;
    char *value = NULL;
    char *sep = loop_strcasestr_local(rem, " as ");
    if (!sep) sep = loop_strcasestr_local(rem, " from ");
    if (!sep) sep = loop_strcasestr_local(rem, " = ");
    if (!sep) sep = loop_strcasestr_local(rem, " : ");
    if (sep) {
        *sep = '\0';
        value = loop_trim(sep + 4);
    } else {
        char *p = rem;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            value = loop_trim(p + 1);
        }
    }
    name = loop_unquote_inplace(loop_trim(name));
    value = loop_unquote_inplace(loop_trim(value ? value : ""));
    loop_meta_add(c, kind, name, value, weight);

    if (strcasecmp(kind, "belief") == 0) {
        double v = loop_parse_double_value(value, NAN);
        if (!isnan(v)) loop_set_signal(c, name, v);
    }
}

static bool loop_parse_define_call(loop_construct_t *c, char *stmt) {
    char *open = strchr(stmt, '(');
    char *close = strrchr(stmt, ')');
    if (!open || !close || close <= open) return false;
    *close = '\0';
    char *a = loop_trim(open + 1);
    char *b = strchr(a, ',');
    if (!b) return false;
    *b = '\0';
    b = loop_trim(b + 1);
    loop_meta_add(c, "define", loop_unquote_inplace(a), loop_unquote_inplace(b), 0.0);
    return true;
}

static void loop_parse_dyad(loop_construct_t *c, char *rem) {
    if (!c || c->dyad_count >= LOOP_DYAD_MAX || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;
    loop_dyad_t *d = &c->dyads[c->dyad_count++];
    memset(d, 0, sizeof(*d));

    char *arrow = strstr(rem, "->");
    char *rel = loop_strcasestr_local(rem, " relation ");
    if (!rel) rel = loop_strcasestr_local(rem, " via ");
    if (!rel) rel = loop_strcasestr_local(rem, " as ");
    if (rel) {
        char *rval = rel;
        while (*rval && !isspace((unsigned char)*rval)) rval++;
        rval = loop_trim(rval);
        loop_copy(d->relation, sizeof(d->relation), loop_unquote_inplace(rval), "interacts");
        *rel = '\0';
    } else {
        loop_copy(d->relation, sizeof(d->relation), "interacts", NULL);
    }

    if (arrow) {
        *arrow = '\0';
        loop_copy(d->from, sizeof(d->from), loop_unquote_inplace(loop_trim(rem)), "object");
        loop_copy(d->to, sizeof(d->to), loop_unquote_inplace(loop_trim(arrow + 2)), "object");
    } else {
        char from[LOOP_META_NAME_MAX] = "";
        char to[LOOP_META_NAME_MAX] = "";
        if (sscanf(rem, "%47s %47s", from, to) >= 2) {
            loop_copy(d->from, sizeof(d->from), loop_unquote_inplace(from), "object");
            loop_copy(d->to, sizeof(d->to), loop_unquote_inplace(to), "object");
        } else {
            loop_copy(d->from, sizeof(d->from), loop_unquote_inplace(rem), "object");
            loop_copy(d->to, sizeof(d->to), "self", NULL);
        }
    }
    loop_graph_add_edge(c, d->from, d->to, d->relation, 0.0);
    c->dsl_enabled = true;
}

static char *loop_take_clause(char *rem, const char *clause) {
    char *p = loop_strcasestr_local(rem, clause);
    if (!p) return NULL;
    *p = '\0';
    return loop_trim(p + strlen(clause));
}

static char *loop_split_relation_clause(char *rem) {
    char *rel = loop_take_clause(rem, " relation ");
    if (!rel) rel = loop_take_clause(rem, " via ");
    if (!rel) rel = loop_take_clause(rem, " as ");
    return rel;
}

static void loop_parse_graph_node(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    double weight = 0.0;
    char *weight_v = loop_take_clause(rem, " weight ");
    if (weight_v) weight = loop_parse_double_value(weight_v, 0.0);
    char *state = loop_take_clause(rem, " state ");

    char *name = rem;
    char *type = NULL;
    char *sep = loop_strcasestr_local(rem, " as ");
    int skip = 4;
    if (!sep) { sep = loop_strcasestr_local(rem, " type "); skip = 6; }
    if (!sep) { sep = strstr(rem, "->"); skip = 2; }
    if (!sep) { sep = strchr(rem, ':'); skip = 1; }
    if (!sep) { sep = strchr(rem, '='); skip = 1; }
    if (sep) {
        *sep = '\0';
        type = loop_trim(sep + skip);
    } else {
        char *p = rem;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            type = loop_trim(p + 1);
        }
    }
    loop_graph_add_node(c, loop_trim(name), type, state, weight);
}

static void loop_parse_graph_update(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *state = loop_take_clause(rem, " state ");
    char *type = loop_take_clause(rem, " type ");
    if (!type) type = loop_take_clause(rem, " as ");

    char *name = rem;
    if (!state && !type) {
        char *p = rem;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            state = loop_trim(p + 1);
        }
    }
    loop_graph_add_node(c, loop_trim(name), type, state, 0.0);
}

static void loop_parse_graph_replace(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *sep = loop_strcasestr_local(rem, " with ");
    int skip = 6;
    if (!sep) { sep = loop_strcasestr_local(rem, " as "); skip = 4; }
    if (!sep) { sep = strstr(rem, "->"); skip = 2; }
    if (!sep) return;
    *sep = '\0';
    loop_graph_replace_node(c, loop_trim(rem), loop_trim(sep + skip));
}

static void loop_parse_graph_edge_parts(char *rem, char *from, size_t from_len,
                                        char *to, size_t to_len,
                                        char *relation, size_t rel_len,
                                        double *weight) {
    if (from && from_len) from[0] = '\0';
    if (to && to_len) to[0] = '\0';
    if (relation && rel_len) relation[0] = '\0';
    if (weight) *weight = 0.0;
    rem = loop_trim(rem);

    char *weight_v = loop_take_clause(rem, " weight ");
    if (weight_v && weight) *weight = loop_parse_double_value(weight_v, 0.0);
    char *rel = loop_split_relation_clause(rem);
    if (rel && relation && rel_len) loop_copy_unquoted(relation, rel_len, rel, "relates");

    char *arrow = strstr(rem, "->");
    if (arrow) {
        *arrow = '\0';
        loop_copy_unquoted(from, from_len, loop_trim(rem), NULL);
        loop_copy_unquoted(to, to_len, loop_trim(arrow + 2), NULL);
    } else {
        char a[LOOP_META_NAME_MAX] = "";
        char b[LOOP_META_NAME_MAX] = "";
        if (sscanf(rem, "%47s %47s", a, b) >= 2) {
            loop_copy_unquoted(from, from_len, a, NULL);
            loop_copy_unquoted(to, to_len, b, NULL);
        }
    }
    if (relation && rel_len && !relation[0])
        loop_copy(relation, rel_len, "relates", NULL);
}

static void loop_parse_graph_edge(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    char from[LOOP_META_NAME_MAX];
    char to[LOOP_META_NAME_MAX];
    char relation[LOOP_META_KIND_MAX];
    double weight = 0.0;
    loop_parse_graph_edge_parts(rem, from, sizeof(from), to, sizeof(to),
                                relation, sizeof(relation), &weight);
    loop_graph_add_edge(c, from, to, relation, weight);
}

static void loop_parse_graph_remove_edge(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    char from[LOOP_META_NAME_MAX];
    char to[LOOP_META_NAME_MAX];
    char relation[LOOP_META_KIND_MAX];
    double weight = 0.0;
    loop_parse_graph_edge_parts(rem, from, sizeof(from), to, sizeof(to),
                                relation, sizeof(relation), &weight);
    (void)weight;
    loop_graph_remove_edge(c, from, to,
                           strcmp(relation, "relates") == 0 ? "" : relation);
}

static void loop_parse_graph_traverse(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (strncasecmp(rem, "from ", 5) == 0) rem = loop_trim(rem + 5);
    int depth = 1;
    char *depth_v = loop_take_clause(rem, " depth ");
    if (depth_v) depth = loop_clamp_int(atoi(depth_v), 0, LOOP_GRAPH_NODE_MAX);
    loop_copy_unquoted(c->traverse_from, sizeof(c->traverse_from), rem, NULL);
    c->traverse_depth = depth;
    c->traverse_hits = loop_graph_traverse_count(c, c->traverse_from, depth);
    c->dsl_enabled = true;
}

static int loop_mapreduce_find_job(const loop_construct_t *c, const char *name) {
    if (!c || !name || !name[0]) return -1;
    for (int i = 0; i < c->mapreduce_job_count; i++) {
        if (strcasecmp(c->mapreduce_jobs[i].name, name) == 0) return i;
    }
    return -1;
}

static loop_mapreduce_job_t *loop_mapreduce_get_job(loop_construct_t *c,
                                                    const char *name) {
    if (!c || !name || !name[0]) return NULL;
    char clean[LOOP_META_NAME_MAX];
    loop_copy_unquoted(clean, sizeof(clean), name, "mapreduce");
    int idx = loop_mapreduce_find_job(c, clean);
    if (idx < 0) {
        if (c->mapreduce_job_count >= LOOP_MAPREDUCE_MAX) return NULL;
        idx = c->mapreduce_job_count++;
        memset(&c->mapreduce_jobs[idx], 0, sizeof(c->mapreduce_jobs[idx]));
        loop_copy(c->mapreduce_jobs[idx].name,
                  sizeof(c->mapreduce_jobs[idx].name), clean, "mapreduce");
    }
    return &c->mapreduce_jobs[idx];
}

static void loop_mapreduce_set_partitions(loop_construct_t *c,
                                          loop_mapreduce_job_t *job,
                                          int partitions) {
    if (!c || !job || partitions <= 0) return;
    partitions = loop_clamp_int(partitions, 1, 1000000);
    c->partition_count += partitions - job->partitions;
    if (c->partition_count < 0) c->partition_count = 0;
    job->partitions = partitions;
}

static void loop_mapreduce_mark_map(loop_construct_t *c,
                                    loop_mapreduce_job_t *job) {
    if (!c || !job) return;
    if (!job->mapped) {
        job->mapped = true;
        c->map_count++;
    }
}

static void loop_mapreduce_mark_shuffle(loop_construct_t *c,
                                        loop_mapreduce_job_t *job) {
    if (!c || !job) return;
    if (!job->shuffled) {
        job->shuffled = true;
        c->shuffle_count++;
    }
}

static void loop_mapreduce_mark_reduce(loop_construct_t *c,
                                       loop_mapreduce_job_t *job) {
    if (!c || !job) return;
    if (!job->reduced) {
        job->reduced = true;
        c->reduce_count++;
    }
}

static void loop_mapreduce_update_graph(loop_construct_t *c,
                                        const loop_mapreduce_job_t *job) {
    if (!c || !job || !job->name[0]) return;
    char state[64] = "";
    snprintf(state, sizeof(state), "%s%s%s",
             job->mapped ? "mapped" : "",
             job->shuffled ? "|shuffled" : "",
             job->reduced ? "|reduced" : "");
    loop_graph_add_node(c, job->name, "mapreduce", state[0] ? state : "planned", 0.0);
    if (job->source[0])
        loop_graph_add_edge(c, job->source, job->name, "map", 0.0);
    if (job->key[0]) {
        loop_graph_add_node(c, job->key, "partition_key", "", 0.0);
        loop_graph_add_edge(c, job->name, job->key, "shuffle", 0.0);
    }
    if (job->reducer[0]) {
        loop_graph_add_node(c, job->reducer, "reducer", "", 0.0);
        loop_graph_add_edge(c, job->name, job->reducer, "reduce", 0.0);
    }
    if (job->target[0] && job->reducer[0])
        loop_graph_add_edge(c, job->reducer, job->target, "emits", 0.0);
}

static void loop_parse_map_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *partitions = loop_take_clause(rem, " partitions ");
    char *mapper = loop_take_clause(rem, " using ");
    if (!mapper) mapper = loop_take_clause(rem, " mapper ");
    if (!mapper) mapper = loop_take_clause(rem, " map ");
    char *source = loop_take_clause(rem, " over ");
    if (!source) source = loop_take_clause(rem, " from ");
    loop_mapreduce_job_t *job = loop_mapreduce_get_job(c, loop_trim(rem));
    if (!job) return;
    if (source) loop_copy_unquoted(job->source, sizeof(job->source), source, NULL);
    if (mapper) loop_copy_unquoted(job->mapper, sizeof(job->mapper), mapper, NULL);
    if (partitions)
        loop_mapreduce_set_partitions(c, job, atoi(partitions));
    loop_mapreduce_mark_map(c, job);
    loop_meta_add(c, "map", job->name, job->source, 0.0);
    loop_mapreduce_update_graph(c, job);
    c->dsl_enabled = true;
}

static void loop_parse_shuffle_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *partitions = loop_take_clause(rem, " partitions ");
    char *key = loop_take_clause(rem, " by ");
    if (!key) key = loop_take_clause(rem, " key ");
    if (!key) key = loop_take_clause(rem, " partition ");
    loop_mapreduce_job_t *job = loop_mapreduce_get_job(c, loop_trim(rem));
    if (!job) return;
    if (key) loop_copy_unquoted(job->key, sizeof(job->key), key, NULL);
    if (partitions)
        loop_mapreduce_set_partitions(c, job, atoi(partitions));
    loop_mapreduce_mark_shuffle(c, job);
    loop_meta_add(c, "shuffle", job->name, job->key, job->partitions);
    loop_mapreduce_update_graph(c, job);
    c->dsl_enabled = true;
}

static void loop_parse_reduce_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *reducer = loop_take_clause(rem, " using ");
    if (!reducer) reducer = loop_take_clause(rem, " reducer ");
    if (!reducer) reducer = loop_take_clause(rem, " reduce ");
    char *target = loop_take_clause(rem, " into ");
    if (!target) target = loop_take_clause(rem, " target ");
    loop_mapreduce_job_t *job = loop_mapreduce_get_job(c, loop_trim(rem));
    if (!job) return;
    if (reducer) loop_copy_unquoted(job->reducer, sizeof(job->reducer), reducer, NULL);
    if (target) loop_copy_unquoted(job->target, sizeof(job->target), target, NULL);
    loop_mapreduce_mark_reduce(c, job);
    loop_meta_add(c, "reduce", job->name, job->reducer, 0.0);
    loop_mapreduce_update_graph(c, job);
    c->dsl_enabled = true;
}

static void loop_parse_mapreduce_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *partitions = loop_take_clause(rem, " partitions ");
    char *key = loop_take_clause(rem, " by ");
    if (!key) key = loop_take_clause(rem, " key ");
    char *target = loop_take_clause(rem, " into ");
    if (!target) target = loop_take_clause(rem, " target ");
    char *reducer = loop_take_clause(rem, " reduce ");
    if (!reducer) reducer = loop_take_clause(rem, " reducer ");
    char *mapper = loop_take_clause(rem, " map ");
    if (!mapper) mapper = loop_take_clause(rem, " mapper ");
    if (!mapper) mapper = loop_take_clause(rem, " using ");
    char *source = loop_take_clause(rem, " over ");
    if (!source) source = loop_take_clause(rem, " from ");

    loop_mapreduce_job_t *job = loop_mapreduce_get_job(c, loop_trim(rem));
    if (!job) return;
    if (source) loop_copy_unquoted(job->source, sizeof(job->source), source, NULL);
    if (mapper) loop_copy_unquoted(job->mapper, sizeof(job->mapper), mapper, NULL);
    if (reducer) loop_copy_unquoted(job->reducer, sizeof(job->reducer), reducer, NULL);
    if (key) loop_copy_unquoted(job->key, sizeof(job->key), key, NULL);
    if (target) loop_copy_unquoted(job->target, sizeof(job->target), target, NULL);
    if (partitions)
        loop_mapreduce_set_partitions(c, job, atoi(partitions));
    loop_mapreduce_mark_map(c, job);
    if (job->key[0] || job->partitions > 0)
        loop_mapreduce_mark_shuffle(c, job);
    if (job->reducer[0])
        loop_mapreduce_mark_reduce(c, job);
    loop_meta_add(c, "mapreduce", job->name, job->source, job->partitions);
    loop_mapreduce_update_graph(c, job);
    c->dsl_enabled = true;
}

static const char *loop_strcasestr_const(const char *haystack,
                                         const char *needle) {
    if (!haystack || !needle || !needle[0]) return NULL;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static bool loop_ci_contains_const(const char *haystack, const char *needle) {
    return loop_strcasestr_const(haystack, needle) != NULL;
}

static const char *loop_clause_terms[] = {
    " matrix ", " property ", " certificate ", " report ", " sds ",
    " value ", " uncertainty ", " unit ", " method ", " target ",
    " using ", " on ", " from ", " by ", " partitions ", " current ",
    " available ", " unavailable ", " orderable ", " price ",
    " distributor ", " store ", " search ", " query ", " destination ",
    " blocked ", " restricted ", " found ", " archived ", " annual ",
    " traceable ", " to ", " policy ", " payment ", NULL
};

static void loop_copy_span_unquoted(char *dst, size_t dst_len,
                                    const char *start, const char *end) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!start) return;
    if (!end || end < start) end = start + strlen(start);
    size_t n = (size_t)(end - start);
    if (n >= LOOP_COND_MAX) n = LOOP_COND_MAX - 1;
    char tmp[LOOP_COND_MAX];
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    loop_copy_unquoted(dst, dst_len, loop_trim(tmp), NULL);
}

static bool loop_extract_clause_value(const char *src, const char *clause,
                                      char *out, size_t out_len) {
    if (!src || !clause || !out || out_len == 0) return false;
    out[0] = '\0';
    const char *p = loop_strcasestr_const(src, clause);
    if (!p) return false;
    p += strlen(clause);
    p = loop_skip_ws(p);
    const char *end = src + strlen(src);
    for (int i = 0; loop_clause_terms[i]; i++) {
        const char *q = loop_strcasestr_const(p, loop_clause_terms[i]);
        if (q && q > p && q < end) end = q;
    }
    loop_copy_span_unquoted(out, out_len, p, end);
    return out[0] != '\0';
}

static void loop_extract_subject(const char *src, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!src) return;
    const char *end = src + strlen(src);
    for (int i = 0; loop_clause_terms[i]; i++) {
        const char *q = loop_strcasestr_const(src, loop_clause_terms[i]);
        if (q && q > src && q < end) end = q;
    }
    const char *eq = strchr(src, '=');
    if (eq && eq > src && eq < end) end = eq;
    const char *colon = strchr(src, ':');
    if (colon && colon > src && colon < end) end = colon;
    loop_copy_span_unquoted(out, out_len, src, end);
    char *space = out;
    while (*space && !isspace((unsigned char)*space)) space++;
    if (*space) *space = '\0';
}

static int loop_srm_find(const loop_construct_t *c, const char *id) {
    if (!c || !id || !id[0]) return -1;
    for (int i = 0; i < c->srm_count; i++) {
        if (strcasecmp(c->srm_entries[i].id, id) == 0 ||
            strcasecmp(c->srm_entries[i].name, id) == 0) {
            return i;
        }
    }
    return -1;
}

static loop_srm_entry_t *loop_srm_get(loop_construct_t *c, const char *id) {
    if (!c || !id || !id[0]) return NULL;
    char clean[LOOP_META_NAME_MAX];
    loop_copy_unquoted(clean, sizeof(clean), id, "reference_material");
    int idx = loop_srm_find(c, clean);
    if (idx < 0) {
        if (c->srm_count >= LOOP_SRM_MAX) return NULL;
        idx = c->srm_count++;
        memset(&c->srm_entries[idx], 0, sizeof(c->srm_entries[idx]));
        loop_copy(c->srm_entries[idx].id, sizeof(c->srm_entries[idx].id),
                  clean, "reference_material");
        loop_copy(c->srm_entries[idx].name, sizeof(c->srm_entries[idx].name),
                  clean, "reference_material");
    }
    return &c->srm_entries[idx];
}

static void loop_srm_update_graph(loop_construct_t *c,
                                  const loop_srm_entry_t *srm) {
    if (!c || !srm || !srm->id[0]) return;
    const char *state = srm->orderable ? "orderable" :
                        srm->available ? "available" :
                        srm->certificate_current ? "current" : "registered";
    loop_graph_add_node(c, srm->id, "reference_material", state,
                        srm->uncertainty);
    if (srm->certificate[0]) {
        loop_graph_add_node(c, srm->certificate, "certificate",
                            srm->certificate_current ? "current" : "", 0.0);
        loop_graph_add_edge(c, srm->id, srm->certificate, "certified_by", 0.0);
    }
    if (srm->report[0]) {
        loop_graph_add_node(c, srm->report, "report", "", 0.0);
        loop_graph_add_edge(c, srm->id, srm->report, "reported_by", 0.0);
    }
    if (srm->sds[0]) {
        loop_graph_add_node(c, srm->sds, "safety_data_sheet",
                            srm->sds_available ? "available" : "", 0.0);
        loop_graph_add_edge(c, srm->id, srm->sds, "sds", 0.0);
    }
    if (srm->traceable)
        loop_graph_add_edge(c, srm->id, "nist_traceability", "traceable_to", 0.0);
    if (srm->store[0]) {
        loop_graph_add_node(c, srm->store, "srm_store", "", 0.0);
        loop_graph_add_edge(c, srm->store, srm->id, "lists", 0.0);
    }
    if (srm->distributor[0]) {
        loop_graph_add_node(c, srm->distributor, "licensed_distributor", "", 0.0);
        loop_graph_add_edge(c, srm->distributor, srm->id, "distributes", 0.0);
    }
    if (srm->shipping_blocked)
        loop_graph_add_edge(c, srm->id, "shipping_restriction", "blocked_to", 0.0);
}

static int loop_srm_certificate_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].certificate[0]) n++;
    return n;
}

static int loop_srm_current_certificate_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].certificate_current) n++;
    return n;
}

static int loop_srm_sds_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].sds_available || c->srm_entries[i].sds[0]) n++;
    return n;
}

static int loop_srm_traceability_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].traceable) n++;
    return n;
}

static int loop_srm_available_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].available) n++;
    return n;
}

static int loop_srm_orderable_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].orderable) n++;
    return n;
}

static int loop_srm_shipping_block_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].shipping_blocked) n++;
    return n;
}

static int loop_srm_product_search_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].product_search_found) n++;
    return n;
}

static int loop_srm_archived_certificate_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].archived_certificate) n++;
    return n;
}

static double loop_srm_total_price(const loop_construct_t *c) {
    if (!c) return 0.0;
    double total = 0.0;
    for (int i = 0; i < c->srm_count; i++)
        if (c->srm_entries[i].price > 0.0) total += c->srm_entries[i].price;
    return total;
}

static int loop_srm_operation_count_kind(const loop_construct_t *c,
                                         const char *kind) {
    if (!c || !kind) return 0;
    int n = 0;
    for (int i = 0; i < c->srm_operation_count; i++)
        if (strcasecmp(c->srm_operations[i].kind, kind) == 0) n++;
    return n;
}

static bool loop_srm_operation_has(const loop_construct_t *c,
                                   const char *kind, const char *name) {
    if (!c || !kind) return false;
    for (int i = 0; i < c->srm_operation_count; i++) {
        loop_srm_operation_t *op = (loop_srm_operation_t *)&c->srm_operations[i];
        if (strcasecmp(op->kind, kind) != 0) continue;
        if (!name || !name[0] || strcasecmp(op->name, name) == 0)
            return true;
    }
    return false;
}

static void loop_srm_operation_add(loop_construct_t *c, const char *kind,
                                   const char *name, const char *value,
                                   bool flag, double amount) {
    if (!c || !kind || c->srm_operation_count >= LOOP_SRM_OPERATION_MAX) return;
    loop_srm_operation_t *op = &c->srm_operations[c->srm_operation_count++];
    memset(op, 0, sizeof(*op));
    loop_copy(op->kind, sizeof(op->kind), kind, "operation");
    loop_copy_unquoted(op->name, sizeof(op->name), name, "");
    loop_copy_unquoted(op->value, sizeof(op->value), value, "");
    op->flag = flag;
    op->amount = amount;
    loop_meta_add(c, kind, op->name[0] ? op->name : kind,
                  op->value, amount);
}

static double loop_srm_mean_uncertainty(const loop_construct_t *c) {
    if (!c) return 0.0;
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < c->srm_count; i++) {
        if (c->srm_entries[i].uncertainty > 0.0) {
            sum += c->srm_entries[i].uncertainty;
            n++;
        }
    }
    return n > 0 ? sum / (double)n : 0.0;
}

static double loop_srm_max_uncertainty(const loop_construct_t *c) {
    if (!c) return 0.0;
    double max_v = 0.0;
    for (int i = 0; i < c->srm_count; i++) {
        if (c->srm_entries[i].uncertainty > max_v)
            max_v = c->srm_entries[i].uncertainty;
    }
    for (int i = 0; i < c->measurement_count; i++) {
        if (c->measurements[i].uncertainty > max_v)
            max_v = c->measurements[i].uncertainty;
    }
    return max_v;
}

static int loop_measurement_find(const loop_construct_t *c, const char *name) {
    if (!c || !name || !name[0]) return -1;
    for (int i = 0; i < c->measurement_count; i++) {
        if (strcasecmp(c->measurements[i].name, name) == 0) return i;
    }
    return -1;
}

static loop_measurement_entry_t *loop_measurement_get(loop_construct_t *c,
                                                      const char *name) {
    if (!c || !name || !name[0]) return NULL;
    char clean[LOOP_META_NAME_MAX];
    loop_copy_unquoted(clean, sizeof(clean), name, "measurement");
    int idx = loop_measurement_find(c, clean);
    if (idx < 0) {
        if (c->measurement_count >= LOOP_MEASUREMENT_MAX) return NULL;
        idx = c->measurement_count++;
        memset(&c->measurements[idx], 0, sizeof(c->measurements[idx]));
        loop_copy(c->measurements[idx].name,
                  sizeof(c->measurements[idx].name), clean, "measurement");
    }
    return &c->measurements[idx];
}

static int loop_measurement_calibration_count(const loop_construct_t *c) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->measurement_count; i++)
        if (c->measurements[i].calibrated) n++;
    return n;
}

static void loop_measurement_update_graph(loop_construct_t *c,
                                          const loop_measurement_entry_t *m) {
    if (!c || !m || !m->name[0]) return;
    loop_graph_add_node(c, m->name,
                        m->calibrated ? "calibration" : "measurement",
                        m->unit, m->uncertainty);
    if (m->material[0])
        loop_graph_add_edge(c, m->material, m->name,
                            m->calibrated ? "calibrates" : "measures", 0.0);
    if (m->method[0]) {
        loop_graph_add_node(c, m->method, "method", "", 0.0);
        loop_graph_add_edge(c, m->name, m->method, "uses_method", 0.0);
    }
}

static void loop_parse_srm_statement(loop_construct_t *c, char *rem,
                                     const char *kind) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;

    char value[LOOP_META_VALUE_MAX];
    if (loop_extract_clause_value(rem, " name ", value, sizeof(value)))
        loop_copy_unquoted(srm->name, sizeof(srm->name), value, NULL);
    if (loop_extract_clause_value(rem, " matrix ", value, sizeof(value)))
        loop_copy_unquoted(srm->matrix, sizeof(srm->matrix), value, NULL);
    if (loop_extract_clause_value(rem, " property ", value, sizeof(value)))
        loop_copy_unquoted(srm->property, sizeof(srm->property), value, NULL);
    if (loop_extract_clause_value(rem, " certificate ", value, sizeof(value))) {
        loop_copy_unquoted(srm->certificate, sizeof(srm->certificate),
                           value, "certificate");
        if (loop_ci_contains_const(value, "current"))
            srm->certificate_current = true;
    }
    if (loop_extract_clause_value(rem, " report ", value, sizeof(value)))
        loop_copy_unquoted(srm->report, sizeof(srm->report), value, "report");
    if (loop_extract_clause_value(rem, " sds ", value, sizeof(value))) {
        loop_copy_unquoted(srm->sds, sizeof(srm->sds), value, "sds");
        if (loop_ci_contains_const(value, "available"))
            srm->sds_available = true;
    }
    if (loop_extract_clause_value(rem, " value ", value, sizeof(value)))
        srm->assigned_value = loop_parse_double_value(value, srm->assigned_value);
    if (loop_extract_clause_value(rem, " uncertainty ", value, sizeof(value)))
        srm->uncertainty = loop_parse_double_value(value, srm->uncertainty);
    if (loop_ci_contains_const(rem, " current"))
        srm->certificate_current = true;
    if (loop_ci_contains_const(rem, " available") && srm->sds[0])
        srm->sds_available = true;
    if (loop_ci_contains_const(rem, " traceable"))
        srm->traceable = true;
    loop_meta_add(c, kind ? kind : "reference_material", srm->id,
                  srm->property[0] ? srm->property : srm->matrix,
                  srm->uncertainty);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_catalog_statement(loop_construct_t *c, char *rem,
                                         bool annual) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) rem = (char *)"catalog";

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    if (!subject[0]) loop_copy(subject, sizeof(subject),
                               annual ? "annual_product_list" : "catalog", NULL);
    char value[LOOP_META_VALUE_MAX] = "";
    if (!loop_extract_clause_value(rem, " store ", value, sizeof(value)) &&
        !loop_extract_clause_value(rem, " from ", value, sizeof(value))) {
        loop_copy(value, sizeof(value),
                  loop_ci_contains_const(rem, "current") ? "current" : "", NULL);
    }
    loop_srm_operation_add(c, annual ? "annual_catalog" : "catalog",
                           subject, value, true, 0.0);
    loop_graph_add_node(c, subject, annual ? "annual_catalog" : "catalog",
                        value, 0.0);
    c->dsl_enabled = true;
}

static void loop_parse_product_search_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;

    char value[LOOP_META_VALUE_MAX] = "";
    if (loop_extract_clause_value(rem, " store ", value, sizeof(value)))
        loop_copy_unquoted(srm->store, sizeof(srm->store), value, NULL);
    else if (loop_ci_contains_const(rem, "shop.nist.gov"))
        loop_copy(srm->store, sizeof(srm->store), "shop.nist.gov", NULL);
    srm->product_search_found = !loop_ci_contains_const(rem, "missing") &&
                                !loop_ci_contains_const(rem, "not_found");
    if (loop_ci_contains_const(rem, " current"))
        srm->certificate_current = true;
    loop_srm_operation_add(c, "product_search", srm->id,
                           srm->store[0] ? srm->store : "store",
                           srm->product_search_found, 0.0);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_availability_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;

    char value[LOOP_META_VALUE_MAX];
    if (loop_extract_clause_value(rem, " price ", value, sizeof(value)))
        srm->price = loop_parse_double_value(value, srm->price);
    if (loop_extract_clause_value(rem, " store ", value, sizeof(value)))
        loop_copy_unquoted(srm->store, sizeof(srm->store), value, NULL);
    if (loop_extract_clause_value(rem, " distributor ", value, sizeof(value)))
        loop_copy_unquoted(srm->distributor, sizeof(srm->distributor), value, NULL);
    if (loop_ci_contains_const(rem, " unavailable")) {
        srm->available = false;
        srm->orderable = false;
    } else if (loop_ci_contains_const(rem, " available")) {
        srm->available = true;
    }
    if (loop_ci_contains_const(rem, " orderable"))
        srm->orderable = true;
    loop_srm_operation_add(c, "availability", srm->id,
                           srm->available ? "available" : "unknown",
                           srm->available, srm->price);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_order_policy_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    bool no_paper = loop_ci_contains_const(rem, "no_paper") ||
                    loop_ci_contains_const(rem, "no paper") ||
                    loop_ci_contains_const(rem, "paper_checks false") ||
                    loop_ci_contains_const(rem, "paper checks false");
    if (no_paper)
        loop_copy(subject, sizeof(subject), "no_paper_checks", NULL);
    if (!subject[0]) loop_copy(subject, sizeof(subject), "order_policy", NULL);
    loop_srm_operation_add(c, "order_policy", subject, rem, true, 0.0);
    loop_graph_add_node(c, subject, "order_policy", no_paper ? "blocked" : "", 0.0);
    c->dsl_enabled = true;
}

static void loop_parse_distributor_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    if (!subject[0]) loop_copy(subject, sizeof(subject), "distributor", NULL);
    loop_srm_operation_add(c, "licensed_distributor", subject, rem, true, 0.0);
    loop_graph_add_node(c, subject, "licensed_distributor", "licensed", 0.0);
    c->dsl_enabled = true;
}

static void loop_parse_shipping_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    char value[LOOP_META_VALUE_MAX];
    if (loop_extract_clause_value(rem, " to ", value, sizeof(value)) ||
        loop_extract_clause_value(rem, " destination ", value, sizeof(value))) {
        loop_copy_unquoted(srm->destination, sizeof(srm->destination), value, NULL);
        const char *status_terms[] = {
            " blocked", " restricted", " allowed", " ceased", NULL
        };
        for (int i = 0; status_terms[i]; i++) {
            char *status = loop_strcasestr_local(srm->destination, status_terms[i]);
            if (!status) continue;
            char *after = status + strlen(status_terms[i]);
            if (*after == '\0' || isspace((unsigned char)*after)) {
                *status = '\0';
                char clean[LOOP_META_NAME_MAX];
                loop_copy(clean, sizeof(clean), loop_trim(srm->destination), NULL);
                loop_copy(srm->destination, sizeof(srm->destination), clean, NULL);
                break;
            }
        }
    }
    srm->shipping_blocked = loop_ci_contains_const(rem, " blocked") ||
                            loop_ci_contains_const(rem, " restricted") ||
                            loop_ci_contains_const(rem, " ceased");
    loop_srm_operation_add(c, "shipping", srm->id,
                           srm->destination, srm->shipping_blocked, 0.0);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_registration_statement(loop_construct_t *c, char *rem,
                                              const char *kind) {
    if (!c || !kind) return;
    rem = loop_trim(rem ? rem : "");
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    if (!subject[0]) loop_copy(subject, sizeof(subject), kind, NULL);
    loop_srm_operation_add(c, kind, subject, rem, true, 0.0);
    c->dsl_enabled = true;
}

static void loop_parse_archived_certificate_statement(loop_construct_t *c,
                                                      char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    srm->archived_certificate = true;
    loop_srm_operation_add(c, "archived_certificate", srm->id, rem, true, 0.0);
    loop_graph_add_edge(c, srm->id, "archived_documents", "archived_in", 0.0);
    c->dsl_enabled = true;
}

static void loop_parse_certificate_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    char cert[LOOP_META_NAME_MAX];
    if (loop_extract_clause_value(rem, " certificate ", cert, sizeof(cert)))
        loop_copy_unquoted(srm->certificate, sizeof(srm->certificate), cert, "certificate");
    else
        loop_copy(srm->certificate, sizeof(srm->certificate), "certificate", NULL);
    if (loop_ci_contains_const(rem, " current"))
        srm->certificate_current = true;
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_report_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    char report[LOOP_META_NAME_MAX];
    if (loop_extract_clause_value(rem, " report ", report, sizeof(report)))
        loop_copy_unquoted(srm->report, sizeof(srm->report), report, "report");
    else
        loop_copy(srm->report, sizeof(srm->report), "report", NULL);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_sds_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    char sds[LOOP_META_NAME_MAX];
    if (loop_extract_clause_value(rem, " sds ", sds, sizeof(sds)))
        loop_copy_unquoted(srm->sds, sizeof(srm->sds), sds, "sds");
    else
        loop_copy(srm->sds, sizeof(srm->sds), "sds", NULL);
    if (loop_ci_contains_const(rem, " available"))
        srm->sds_available = true;
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_traceability_statement(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_srm_entry_t *srm = loop_srm_get(c, subject);
    if (!srm) return;
    srm->traceable = true;
    loop_meta_add(c, "traceability", srm->id, "NIST", 1.0);
    loop_srm_update_graph(c, srm);
    c->dsl_enabled = true;
}

static void loop_parse_measurement_statement(loop_construct_t *c, char *rem,
                                             bool calibrated) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    loop_measurement_entry_t *m = loop_measurement_get(c, subject);
    if (!m) return;
    m->calibrated = m->calibrated || calibrated;

    char value[LOOP_META_VALUE_MAX];
    if (loop_extract_clause_value(rem, " on ", value, sizeof(value)) ||
        loop_extract_clause_value(rem, " using ", value, sizeof(value)) ||
        loop_extract_clause_value(rem, " from ", value, sizeof(value))) {
        loop_copy_unquoted(m->material, sizeof(m->material), value, NULL);
    }
    if (loop_extract_clause_value(rem, " property ", value, sizeof(value)))
        loop_copy_unquoted(m->property, sizeof(m->property), value, NULL);
    if (loop_extract_clause_value(rem, " unit ", value, sizeof(value)))
        loop_copy_unquoted(m->unit, sizeof(m->unit), value, NULL);
    if (loop_extract_clause_value(rem, " method ", value, sizeof(value)))
        loop_copy_unquoted(m->method, sizeof(m->method), value, NULL);
    if (loop_extract_clause_value(rem, " value ", value, sizeof(value)))
        m->value = loop_parse_double_value(value, m->value);
    if (loop_extract_clause_value(rem, " uncertainty ", value, sizeof(value)))
        m->uncertainty = loop_parse_double_value(value, m->uncertainty);
    loop_meta_add(c, calibrated ? "calibration" : "measurement", m->name,
                  m->material, m->uncertainty);
    loop_measurement_update_graph(c, m);
    c->dsl_enabled = true;
}

static void loop_parse_uncertainty_budget(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char subject[LOOP_META_NAME_MAX];
    loop_extract_subject(rem, subject, sizeof(subject));
    if (!subject[0]) loop_copy(subject, sizeof(subject), "uncertainty_budget", NULL);
    loop_measurement_entry_t *m = loop_measurement_get(c, subject);
    if (!m) return;
    char value[LOOP_META_VALUE_MAX];
    if (loop_extract_clause_value(rem, " uncertainty ", value, sizeof(value))) {
        m->uncertainty = loop_parse_double_value(value, m->uncertainty);
    } else {
        char *eq = strchr(rem, '=');
        if (!eq) eq = strchr(rem, ':');
        if (eq)
            m->uncertainty = loop_parse_double_value(eq + 1, m->uncertainty);
    }
    if (!m->has_uncertainty_budget) {
        m->has_uncertainty_budget = true;
        c->uncertainty_budget_count++;
    }
    loop_meta_add(c, "uncertainty_budget", m->name, "", m->uncertainty);
    loop_measurement_update_graph(c, m);
    c->dsl_enabled = true;
}

static void loop_parse_reward_object(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    if (!rem[0]) return;

    char *target = loop_take_clause(rem, " target ");
    if (!target) target = loop_take_clause(rem, " for ");
    char *intensity_v = loop_take_clause(rem, " intensity ");
    char *valence_v = loop_take_clause(rem, " valence ");
    double valence = valence_v ? loop_parse_double_value(valence_v, c->valence) : c->valence;
    double intensity = intensity_v ? loop_parse_double_value(intensity_v, 1.0) : 1.0;
    intensity = loop_clamp01(intensity);

    char *name = loop_trim(rem);
    if (!name[0]) name = (char *)"reward";
    double strength = valence * intensity;
    c->valence = valence;
    c->intensity = intensity;
    c->reward += strength;
    loop_meta_add(c, "reward_object", name, target ? target : "", strength);
    loop_graph_add_node(c, name, "reward_object", target ? target : "", strength);
    if (target && loop_trim(target)[0])
        loop_graph_add_edge(c, name, target, "rewards", strength);
    c->dsl_enabled = true;
}

static void loop_parse_relation_edge(loop_construct_t *c, char *rem,
                                     const char *default_relation,
                                     const char *meta_kind) {
    if (!c || !rem) return;
    char from[LOOP_META_NAME_MAX];
    char to[LOOP_META_NAME_MAX];
    char relation[LOOP_META_KIND_MAX];
    double weight = 0.0;
    loop_parse_graph_edge_parts(rem, from, sizeof(from), to, sizeof(to),
                                relation, sizeof(relation), &weight);
    if (strcmp(relation, "relates") == 0 && default_relation)
        loop_copy(relation, sizeof(relation), default_relation, NULL);
    loop_graph_add_edge(c, from, to, relation, weight);
    if (from[0] && to[0] && meta_kind)
        loop_meta_add(c, meta_kind, from, to, weight);
}

static void loop_parse_explore(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *rate = loop_take_clause(rem, " rate ");
    if (!rate) rate = loop_take_clause(rem, " epsilon ");
    if (!rate) {
        char *v = NULL;
        if (loop_stmt_assignment(rem, "rate", &v) ||
            loop_stmt_assignment(rem, "exploration_rate", &v) ||
            loop_stmt_assignment(rem, "epsilon", &v)) {
            rate = v;
        }
    }
    double r = loop_parse_double_value(rate ? rate : rem, c->exploration_rate);
    c->exploration_rate = loop_clamp01(r);
    if (c->curiosity < c->exploration_rate)
        c->curiosity = c->exploration_rate;
    loop_meta_add(c, "explore", rem && rem[0] ? rem : "objects", rate ? rate : "", c->exploration_rate);
    c->dsl_enabled = true;
}

static void loop_prune_edges_below(loop_construct_t *c, double threshold) {
    if (!c) return;
    threshold = loop_clamp01(threshold);
    c->pruning_threshold = threshold;
    for (int i = 0; i < c->graph_edge_count; ) {
        if (c->graph_edges[i].weight < threshold) {
            for (int j = i + 1; j < c->graph_edge_count; j++)
                c->graph_edges[j - 1] = c->graph_edges[j];
            c->graph_edge_count--;
            memset(&c->graph_edges[c->graph_edge_count], 0,
                   sizeof(c->graph_edges[c->graph_edge_count]));
        } else {
            i++;
        }
    }
    c->dsl_enabled = true;
}

static void loop_parse_prune(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *below = NULL;
    if (strncasecmp(rem, "below ", 6) == 0)
        below = loop_trim(rem + 6);
    else if (strncasecmp(rem, "threshold ", 10) == 0)
        below = loop_trim(rem + 10);
    if (!below) below = loop_strcasestr_local(rem, " below ");
    if (!below) below = loop_strcasestr_local(rem, " threshold ");
    if (below) {
        while (*below && !isdigit((unsigned char)*below) &&
               *below != '-' && *below != '.') {
            below++;
        }
    }
    double threshold = loop_parse_double_value(below ? below : rem,
                                              c->pruning_threshold);
    loop_prune_edges_below(c, threshold);
}

static void loop_parse_credit(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *value = NULL;
    char *name = rem;
    char *sep = strchr(rem, '=');
    if (!sep) sep = strchr(rem, ':');
    if (sep) {
        *sep = '\0';
        value = loop_trim(sep + 1);
    } else {
        char *p = rem;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            value = loop_trim(p + 1);
        }
    }
    c->credit = loop_parse_double_value(value ? value : name, c->credit);
    loop_meta_add(c, "credit", loop_trim(name)[0] ? loop_trim(name) : "assignment",
                  value ? value : "", c->credit);
}

static void loop_parse_attractor(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *basin = loop_take_clause(rem, " basin ");
    if (!basin) basin = loop_take_clause(rem, " temperature ");
    if (basin)
        c->basin_temperature = loop_clamp01(loop_parse_double_value(basin, 0.0));
    loop_copy_unquoted(c->attractor, sizeof(c->attractor), rem, "attractor");
    loop_meta_add(c, "attractor", c->attractor, basin ? basin : "", c->basin_temperature);
    loop_graph_add_node(c, c->attractor, "attractor", basin ? basin : "", c->basin_temperature);
}

static void loop_parse_prompt_game(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    loop_copy_unquoted(c->prompt_game, sizeof(c->prompt_game), rem, "prompt_game");
    loop_meta_add(c, "prompt_game", c->prompt_game, "", 0.0);
    loop_graph_add_node(c, c->prompt_game, "prompt_game", "", 0.0);
}

static void loop_parse_effect(loop_construct_t *c, char *rem) {
    if (!c || !rem) return;
    rem = loop_trim(rem);
    char *name = rem;
    char *value = NULL;
    char *sep = strchr(rem, '=');
    if (!sep) sep = strchr(rem, ':');
    if (sep) {
        *sep = '\0';
        value = loop_trim(sep + 1);
    } else {
        char *p = rem;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            value = loop_trim(p + 1);
        }
    }
    loop_set_effect(c, loop_trim(name), loop_parse_double_value(value, 0.0));
}

static bool loop_refine_add(loop_construct_t *c, char *rem) {
    if (!c || c->refine_count >= LOOP_REFINE_MAX || !rem) return false;
    rem = loop_trim(rem);
    char *when = loop_strcasestr_local(rem, " when ");
    if (!when) return false;
    *when = '\0';
    when = loop_trim(when + 6);
    loop_refine_rule_t *r = &c->refine_rules[c->refine_count];
    memset(r, 0, sizeof(*r));
    if (sscanf(rem, "%47s %2s %lf", r->target, r->op, &r->value) != 3)
        return false;
    loop_copy(r->when, sizeof(r->when), when, NULL);
    c->refine_count++;
    c->dsl_enabled = true;
    return true;
}

static bool loop_rewrite_add(loop_construct_t *c, char *rem) {
    if (!c || c->rewrite_count >= LOOP_REWRITE_MAX || !rem) return false;
    rem = loop_trim(rem);
    char *when = loop_strcasestr_local(rem, " when ");
    if (!when) return false;
    *when = '\0';
    when = loop_trim(when + 6);
    rem = loop_trim(rem);
    if (!rem[0] || !when[0]) return false;
    char *semi = strchr(rem, ';');
    char *nl = strchr(rem, '\n');
    if (semi && (!nl || semi < nl)) *semi = '\0';
    else if (nl) *nl = '\0';
    rem = loop_trim(rem);
    if (!rem[0]) return false;
    loop_rewrite_rule_t *r = &c->rewrite_rules[c->rewrite_count];
    memset(r, 0, sizeof(*r));
    loop_copy(r->action, sizeof(r->action), rem, NULL);
    loop_copy(r->when, sizeof(r->when), when, NULL);
    c->rewrite_count++;
    c->dsl_enabled = true;
    return true;
}

static void loop_apply_numeric_target(loop_construct_t *c, const char *target,
                                      const char *op, double value) {
    if (!c || !target || !op) return;
    double current = 0.0;
    double *slot = NULL;
    bool is_int = false;

    if (loop_name_matches(target, "max_iterations", "max_iters", NULL)) {
        current = c->max_iterations; is_int = true;
    } else if (loop_name_matches(target, "max_turns", "turn_limit", NULL)) {
        current = c->max_turns; is_int = true;
    } else if (loop_name_matches(target, "effect.conversational", "effect.conversation", NULL)) {
        slot = &c->effect_conversational; current = *slot;
    } else if (loop_name_matches(target, "effect.tool", "effect.tools", "effect.tool_calling")) {
        slot = &c->effect_tool; current = *slot;
    } else if (loop_name_matches(target, "effect.world", "effect.world_modeling", NULL)) {
        slot = &c->effect_world; current = *slot;
    } else if (loop_name_matches(target, "effect.meta", "effect.meta_learning", "effect.reflection")) {
        slot = &c->effect_meta; current = *slot;
    } else if (loop_name_matches(target, "reward", "external_reward", NULL)) {
        slot = &c->reward; current = *slot;
    } else if (loop_name_matches(target, "curiosity", "intrinsic_reward", NULL)) {
        slot = &c->curiosity; current = *slot;
    } else if (loop_name_matches(target, "empowerment", "agency", NULL)) {
        slot = &c->empowerment; current = *slot;
    } else if (loop_name_matches(target, "confidence", "certainty", NULL)) {
        slot = &c->confidence; current = *slot;
    } else if (loop_name_matches(target, "uncertainty", "entropy", NULL)) {
        slot = &c->uncertainty; current = *slot;
    } else if (loop_name_matches(target, "learning_rate", "learn_rate", NULL)) {
        slot = &c->learning_rate; current = *slot;
    } else if (loop_name_matches(target, "valence", "reward_valence", NULL)) {
        slot = &c->valence; current = *slot;
    } else if (loop_name_matches(target, "intensity", "reward_intensity", NULL)) {
        slot = &c->intensity; current = *slot;
    } else if (loop_name_matches(target, "exploration_rate", "explore_rate", "epsilon")) {
        slot = &c->exploration_rate; current = *slot;
    } else if (loop_name_matches(target, "credit", "credit_assignment", NULL)) {
        slot = &c->credit; current = *slot;
    } else if (loop_name_matches(target, "pruning_threshold", "prune_threshold", NULL)) {
        slot = &c->pruning_threshold; current = *slot;
    } else if (loop_name_matches(target, "basin_temperature", "basin_hop", NULL)) {
        slot = &c->basin_temperature; current = *slot;
    } else {
        return;
    }

    double next = value;
    if (strcmp(op, "+=") == 0) next = current + value;
    else if (strcmp(op, "-=") == 0) next = current - value;
    else if (strcmp(op, "*=") == 0) next = current * value;
    else if (strcmp(op, "/=") == 0 && value != 0.0) next = current / value;

    if (is_int) {
        int iv = (int)llround(next);
        if (loop_name_matches(target, "max_iterations", "max_iters", NULL))
            c->max_iterations = loop_clamp_int(iv, 1, 10000);
        else
            c->max_turns = loop_clamp_int(iv, 1, 999999);
    } else if (slot) {
        if (strncasecmp(target, "effect.", 7) == 0 ||
            loop_name_matches(target, "confidence", "certainty", NULL) ||
            loop_name_matches(target, "uncertainty", "entropy", NULL) ||
            loop_name_matches(target, "learning_rate", "learn_rate", NULL) ||
            loop_name_matches(target, "intensity", "reward_intensity", NULL) ||
            loop_name_matches(target, "exploration_rate", "explore_rate", "epsilon") ||
            loop_name_matches(target, "pruning_threshold", "prune_threshold", NULL) ||
            loop_name_matches(target, "basin_temperature", "basin_hop", NULL)) {
            *slot = loop_clamp01(next);
        } else {
            *slot = next;
        }
    }
}

static bool loop_apply_refinements_locked(loop_construct_t *c, int current_turn,
                                          bool model_done, bool has_followup,
                                          int depth, char *err,
                                          size_t err_len) {
    if (!c) return true;
    for (int i = 0; i < c->refine_count; i++) {
        loop_refine_rule_t *r = &c->refine_rules[i];
        if (r->fired || !r->when[0]) continue;
        bool valid = false;
        char local_err[96] = "";
        bool fire = loop_eval_expr_bool(c, r->when, current_turn, model_done,
                                        has_followup, depth, &valid,
                                        local_err, sizeof(local_err));
        if (!valid) {
            if (err && err_len > 0)
                snprintf(err, err_len, "refine '%s' invalid: %s",
                         r->target, local_err);
            return false;
        }
        if (fire) {
            loop_apply_numeric_target(c, r->target, r->op, r->value);
            r->fired = true;
            c->refinements_applied++;
        }
    }
    return true;
}

static char *loop_extract_program_body(char *buf) {
    char *body = loop_trim(buf);
    char *brace = strchr(body, '{');
    if (brace) {
        char *end = strrchr(brace + 1, '}');
        if (end) *end = '\0';
        body = brace + 1;
    }
    body = loop_trim(body);
    if (strncmp(body, "|c|", 3) == 0) body = loop_trim(body + 3);
    return body;
}

static bool loop_apply_program_statement(loop_construct_t *c, char *stmt) {
    char *v = NULL;
    stmt = loop_trim(stmt);
    if (!stmt || !stmt[0] || stmt[0] == '#') return false;

    if (loop_stmt_prefix(stmt, "continue when", &v) ||
        loop_stmt_prefix(stmt, "continue while", &v)) {
        loop_copy(c->continue_expr, sizeof(c->continue_expr), v, NULL);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "break when", &v) ||
        loop_stmt_prefix(stmt, "break if", &v) ||
        loop_stmt_prefix(stmt, "exit when", &v) ||
        loop_stmt_prefix(stmt, "complete when", &v)) {
        loop_copy(c->break_expr, sizeof(c->break_expr), v, NULL);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "add_node", &v) ||
        loop_stmt_prefix(stmt, "add node", &v) ||
        loop_stmt_prefix(stmt, "node", &v)) {
        loop_parse_graph_node(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "remove_node", &v) ||
        loop_stmt_prefix(stmt, "remove node", &v) ||
        loop_stmt_prefix(stmt, "delete_node", &v) ||
        loop_stmt_prefix(stmt, "delete node", &v)) {
        loop_graph_remove_node(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "replace_node", &v) ||
        loop_stmt_prefix(stmt, "replace node", &v) ||
        loop_stmt_prefix(stmt, "replace", &v)) {
        loop_parse_graph_replace(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "update_node", &v) ||
        loop_stmt_prefix(stmt, "update node", &v) ||
        loop_stmt_prefix(stmt, "update", &v)) {
        loop_parse_graph_update(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "add_edge", &v) ||
        loop_stmt_prefix(stmt, "add edge", &v) ||
        loop_stmt_prefix(stmt, "edge", &v)) {
        loop_parse_graph_edge(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "remove_edge", &v) ||
        loop_stmt_prefix(stmt, "remove edge", &v) ||
        loop_stmt_prefix(stmt, "delete_edge", &v) ||
        loop_stmt_prefix(stmt, "delete edge", &v)) {
        loop_parse_graph_remove_edge(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "find", &v)) {
        loop_copy_unquoted(c->traverse_from, sizeof(c->traverse_from), v, NULL);
        c->traverse_depth = 0;
        c->traverse_hits = loop_graph_find_node(c, c->traverse_from) >= 0 ? 1 : 0;
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "traverse", &v) ||
        loop_stmt_prefix(stmt, "traverse from", &v)) {
        loop_parse_graph_traverse(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "mapreduce", &v) ||
        loop_stmt_prefix(stmt, "map_reduce", &v) ||
        loop_stmt_prefix(stmt, "map reduce", &v)) {
        loop_parse_mapreduce_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "map", &v)) {
        loop_parse_map_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "shuffle", &v) ||
        loop_stmt_prefix(stmt, "partition", &v)) {
        loop_parse_shuffle_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "reduce", &v) ||
        loop_stmt_prefix(stmt, "fold", &v)) {
        loop_parse_reduce_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "annual_product_list", &v) ||
        loop_stmt_prefix(stmt, "annual product list", &v) ||
        loop_stmt_prefix(stmt, "annual_catalog", &v) ||
        loop_stmt_prefix(stmt, "annual catalog", &v)) {
        loop_parse_catalog_statement(c, v, true);
        return true;
    }
    if (loop_stmt_prefix(stmt, "catalog", &v) ||
        loop_stmt_prefix(stmt, "online_catalog", &v) ||
        loop_stmt_prefix(stmt, "online catalog", &v)) {
        loop_parse_catalog_statement(c, v, false);
        return true;
    }
    if (loop_stmt_prefix(stmt, "product_search", &v) ||
        loop_stmt_prefix(stmt, "product search", &v) ||
        loop_stmt_prefix(stmt, "store_search", &v) ||
        loop_stmt_prefix(stmt, "store search", &v)) {
        loop_parse_product_search_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "availability", &v) ||
        loop_stmt_prefix(stmt, "available", &v) ||
        loop_stmt_prefix(stmt, "orderable", &v)) {
        loop_parse_availability_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "order_policy", &v) ||
        loop_stmt_prefix(stmt, "order policy", &v) ||
        loop_stmt_prefix(stmt, "payment_policy", &v) ||
        loop_stmt_prefix(stmt, "payment policy", &v)) {
        loop_parse_order_policy_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "licensed_distributor", &v) ||
        loop_stmt_prefix(stmt, "licensed distributor", &v) ||
        loop_stmt_prefix(stmt, "distributor", &v)) {
        loop_parse_distributor_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "shipping", &v) ||
        loop_stmt_prefix(stmt, "shipping_restriction", &v) ||
        loop_stmt_prefix(stmt, "shipping restriction", &v)) {
        loop_parse_shipping_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "registration", &v)) {
        loop_parse_registration_statement(c, v, "registration");
        return true;
    }
    if (loop_stmt_prefix(stmt, "survey", &v) ||
        loop_stmt_prefix(stmt, "surveys", &v)) {
        loop_parse_registration_statement(c, v, "survey");
        return true;
    }
    if (loop_stmt_prefix(stmt, "archived_certificate", &v) ||
        loop_stmt_prefix(stmt, "archived certificate", &v) ||
        loop_stmt_prefix(stmt, "archived_report", &v) ||
        loop_stmt_prefix(stmt, "archived report", &v)) {
        loop_parse_archived_certificate_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "standard_reference_material", &v) ||
        loop_stmt_prefix(stmt, "standard reference material", &v) ||
        loop_stmt_prefix(stmt, "reference_material", &v) ||
        loop_stmt_prefix(stmt, "reference material", &v) ||
        loop_stmt_prefix(stmt, "srm", &v) ||
        loop_stmt_prefix(stmt, "rm", &v)) {
        loop_parse_srm_statement(c, v, "reference_material");
        return true;
    }
    if (loop_stmt_prefix(stmt, "certificate", &v) ||
        loop_stmt_prefix(stmt, "cert", &v)) {
        loop_parse_certificate_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "report", &v) ||
        loop_stmt_prefix(stmt, "report_of_investigation", &v) ||
        loop_stmt_prefix(stmt, "report of investigation", &v)) {
        loop_parse_report_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "safety_data_sheet", &v) ||
        loop_stmt_prefix(stmt, "safety data sheet", &v) ||
        loop_stmt_prefix(stmt, "sds", &v)) {
        loop_parse_sds_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "traceability", &v) ||
        loop_stmt_prefix(stmt, "metrological_traceability", &v) ||
        loop_stmt_prefix(stmt, "metrological traceability", &v)) {
        loop_parse_traceability_statement(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "measurement", &v) ||
        loop_stmt_prefix(stmt, "measure", &v)) {
        loop_parse_measurement_statement(c, v, false);
        return true;
    }
    if (loop_stmt_prefix(stmt, "calibration", &v) ||
        loop_stmt_prefix(stmt, "calibrate", &v)) {
        loop_parse_measurement_statement(c, v, true);
        return true;
    }
    if (loop_stmt_prefix(stmt, "uncertainty_budget", &v) ||
        loop_stmt_prefix(stmt, "uncertainty budget", &v)) {
        loop_parse_uncertainty_budget(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "quality_system", &v) ||
        loop_stmt_prefix(stmt, "quality system", &v)) {
        loop_meta_from_remainder(c, "quality_system", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "balance", &v)) {
        (void)v;
        double density = loop_graph_density(c);
        if (c->graph_edge_count > 0 && c->effect_world < density)
            c->effect_world = loop_clamp01(density);
        if (c->graph_node_count > 0 && c->confidence <= 0.0)
            c->confidence = loop_clamp01(1.0 - density);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "reward_object", &v) ||
        loop_stmt_prefix(stmt, "reward object", &v) ||
        loop_stmt_prefix(stmt, "reward mechanism", &v)) {
        loop_parse_reward_object(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "causal_link", &v) ||
        loop_stmt_prefix(stmt, "causal link", &v) ||
        loop_stmt_prefix(stmt, "causal", &v)) {
        loop_parse_relation_edge(c, v, "causal", "causal_link");
        return true;
    }
    if (loop_stmt_prefix(stmt, "message_passing", &v) ||
        loop_stmt_prefix(stmt, "message passing", &v) ||
        loop_stmt_prefix(stmt, "message", &v)) {
        loop_parse_relation_edge(c, v, "message", "message");
        return true;
    }
    if (loop_stmt_prefix(stmt, "explore", &v) ||
        loop_stmt_prefix(stmt, "stochastic_prompting", &v) ||
        loop_stmt_prefix(stmt, "stochastic prompting", &v)) {
        loop_parse_explore(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "prune_edges", &v) ||
        loop_stmt_prefix(stmt, "prune chains", &v) ||
        loop_stmt_prefix(stmt, "prune", &v)) {
        loop_parse_prune(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "credit_assignment", &v) ||
        loop_stmt_prefix(stmt, "credit assignment", &v) ||
        loop_stmt_prefix(stmt, "credit", &v)) {
        loop_parse_credit(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "attractor", &v)) {
        loop_parse_attractor(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "prompt_game", &v) ||
        loop_stmt_prefix(stmt, "prompt game", &v)) {
        loop_parse_prompt_game(c, v);
        return true;
    }
    if (loop_stmt_assignment(stmt, "basin_hop", &v) ||
        loop_stmt_assignment(stmt, "basin_temperature", &v) ||
        loop_stmt_assignment(stmt, "basin", &v)) {
        c->basin_temperature = loop_clamp01(loop_parse_double_value(v, 0.0));
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "define", &v)) {
        if (!loop_parse_define_call(c, stmt))
            loop_meta_from_remainder(c, "define", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "object", &v)) {
        loop_meta_from_remainder(c, "object", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "goal", &v)) {
        loop_meta_from_remainder(c, "goal", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "task", &v)) {
        loop_meta_from_remainder(c, "task", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "belief", &v)) {
        loop_meta_from_remainder(c, "belief", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "infer", &v)) {
        loop_meta_from_remainder(c, "infer", v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "learn", &v)) {
        char *learn_v = v;
        if (loop_stmt_assignment(learn_v, "rate", &learn_v) ||
            loop_stmt_assignment(v, "learning_rate", &learn_v)) {
            c->learning_rate = loop_clamp01(loop_parse_double_value(learn_v, 0.0));
        } else {
            loop_meta_from_remainder(c, "learn", v);
        }
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "decide", &v)) {
        loop_copy(c->decision, sizeof(c->decision), loop_unquote_inplace(v), NULL);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_prefix(stmt, "dyad", &v) ||
        loop_stmt_prefix(stmt, "interaction", &v) ||
        loop_stmt_prefix(stmt, "spawn dyad", &v)) {
        loop_parse_dyad(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "effect", &v)) {
        loop_parse_effect(c, v);
        return true;
    }
    if (loop_stmt_prefix(stmt, "schema_rewrite", &v) ||
        loop_stmt_prefix(stmt, "schema rewrite", &v) ||
        loop_stmt_prefix(stmt, "adapt_schema", &v) ||
        loop_stmt_prefix(stmt, "adapt schema", &v) ||
        loop_stmt_prefix(stmt, "rewrite_when", &v) ||
        loop_stmt_prefix(stmt, "rewrite", &v)) {
        return loop_rewrite_add(c, v);
    }
    if (loop_stmt_prefix(stmt, "refine", &v)) {
        return loop_refine_add(c, v);
    }
    if (loop_stmt_assignment(stmt, "policy", &v)) {
        loop_copy(c->policy, sizeof(c->policy), loop_unquote_inplace(v), NULL);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "reward", &v) ||
        loop_stmt_assignment(stmt, "curiosity", &v) ||
        loop_stmt_assignment(stmt, "empowerment", &v) ||
        loop_stmt_assignment(stmt, "confidence", &v) ||
        loop_stmt_assignment(stmt, "uncertainty", &v) ||
        loop_stmt_assignment(stmt, "learning_rate", &v) ||
        loop_stmt_assignment(stmt, "valence", &v) ||
        loop_stmt_assignment(stmt, "intensity", &v) ||
        loop_stmt_assignment(stmt, "exploration_rate", &v) ||
        loop_stmt_assignment(stmt, "credit", &v) ||
        loop_stmt_assignment(stmt, "pruning_threshold", &v) ||
        loop_stmt_assignment(stmt, "basin_temperature", &v)) {
        char key[LOOP_META_NAME_MAX] = "";
        char *eq = strchr(stmt, '=');
        if (!eq) eq = strchr(stmt, ':');
        size_t n = eq ? (size_t)(eq - stmt) : strcspn(stmt, " \t");
        if (n >= sizeof(key)) n = sizeof(key) - 1;
        memcpy(key, stmt, n);
        key[n] = '\0';
        loop_set_signal(c, loop_trim(key), loop_parse_double_value(v, 0.0));
        return true;
    }
    if (loop_stmt_assignment(stmt, "max_iterations", &v) ||
        loop_stmt_assignment(stmt, "max_iters", &v)) {
        c->max_iterations = loop_clamp_int(atoi(v), 1, 10000);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "max_turns", &v)) {
        c->max_turns = loop_clamp_int(atoi(v), 1, 999999);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "recursive", &v)) {
        c->recursive = loop_parse_bool_value(v, c->recursive);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "override_done", &v)) {
        c->override_done = loop_parse_bool_value(v, c->override_done);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "override_max_turns", &v)) {
        c->override_max_turns = loop_parse_bool_value(v, c->override_max_turns);
        c->dsl_enabled = true;
        return true;
    }
    if (loop_stmt_assignment(stmt, "prompt", &v) ||
        loop_stmt_assignment(stmt, "continue_prompt", &v)) {
        loop_copy(c->continue_prompt, sizeof(c->continue_prompt),
                  loop_unquote_inplace(v), NULL);
        c->dsl_enabled = true;
        return true;
    }
    return false;
}

static void loop_apply_program_locked(loop_construct_t *c, const char *program) {
    if (!c || !program || !program[0]) return;
    c->continue_expr[0] = '\0';
    c->break_expr[0] = '\0';
    c->dsl_enabled = false;
    loop_program_meta_reset(c);

    char buf[LOOP_COND_MAX];
    snprintf(buf, sizeof(buf), "%s", program);
    char *body = loop_extract_program_body(buf);
    char *p = body;
    while (*p) {
        char *start = p;
        while (*p && *p != ';' && *p != '\n') p++;
        char saved = *p;
        *p = '\0';
        loop_apply_program_statement(c, start);
        if (!saved) break;
        p++;
    }
}

static bool loop_apply_rewrites_locked(loop_construct_t *c, int current_turn,
                                       bool model_done, bool has_followup,
                                       int depth, char *err,
                                       size_t err_len) {
    if (!c) return true;
    int initial_count = c->rewrite_count;
    for (int i = 0; i < initial_count; i++) {
        loop_rewrite_rule_t *r = &c->rewrite_rules[i];
        if (r->fired || !r->when[0] || !r->action[0]) continue;
        bool valid = false;
        char local_err[96] = "";
        bool fire = loop_eval_expr_bool(c, r->when, current_turn, model_done,
                                        has_followup, depth, &valid,
                                        local_err, sizeof(local_err));
        if (!valid) {
            if (err && err_len > 0)
                snprintf(err, err_len, "rewrite invalid: %s", local_err);
            return false;
        }
        if (!fire) continue;

        char action[LOOP_COND_MAX];
        loop_copy(action, sizeof(action), r->action, NULL);
        if (!loop_apply_program_statement(c, action)) {
            if (err && err_len > 0)
                snprintf(err, err_len, "rewrite action not understood: %.64s",
                         r->action);
            return false;
        }
        r->fired = true;
        c->rewrites_applied++;
    }
    return true;
}

void tools_loop_control_reset(void) {
    pthread_mutex_lock(&g_loop_lock);
    memset(g_loop_stack, 0, sizeof(g_loop_stack));
    g_loop_depth = 0;
    g_loop_next_id = 1;
    pthread_mutex_unlock(&g_loop_lock);
}

bool tools_loop_control_has_active(void) {
    pthread_mutex_lock(&g_loop_lock);
    bool active = g_loop_depth > 0;
    pthread_mutex_unlock(&g_loop_lock);
    return active;
}

int tools_loop_control_effective_max_turns(int default_max_turns) {
    int limit = default_max_turns;
    pthread_mutex_lock(&g_loop_lock);
    for (int i = 0; i < g_loop_depth; i++) {
        loop_construct_t *c = &g_loop_stack[i];
        if (c->override_max_turns && c->max_turns > limit) {
            limit = c->max_turns;
        }
    }
    pthread_mutex_unlock(&g_loop_lock);
    return limit;
}

void tools_loop_control_decide(int current_turn, bool model_done,
                               bool has_followup,
                               loop_control_decision_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_loop_lock);
    if (g_loop_depth <= 0) {
        pthread_mutex_unlock(&g_loop_lock);
        return;
    }

    loop_construct_t *c = &g_loop_stack[g_loop_depth - 1];
    if (c->override_max_turns && c->max_turns > 0)
        out->effective_max_turns = c->max_turns;

    bool boundary = model_done && !has_followup;
    if (boundary && c->refine_count > 0) {
        char refine_err[128] = "";
        if (!loop_apply_refinements_locked(c, current_turn, model_done,
                                           has_followup, g_loop_depth,
                                           refine_err, sizeof(refine_err))) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' %s", c->label, refine_err);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
            pthread_mutex_unlock(&g_loop_lock);
            return;
        }
        if (c->override_max_turns && c->max_turns > 0)
            out->effective_max_turns = c->max_turns;
    }

    if (boundary && c->rewrite_count > 0) {
        char rewrite_err[128] = "";
        if (!loop_apply_rewrites_locked(c, current_turn, model_done,
                                        has_followup, g_loop_depth,
                                        rewrite_err, sizeof(rewrite_err))) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' %s", c->label, rewrite_err);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
            pthread_mutex_unlock(&g_loop_lock);
            return;
        }
        if (c->override_max_turns && c->max_turns > 0)
            out->effective_max_turns = c->max_turns;
    }

    if (boundary && c->break_expr[0]) {
        bool valid = false;
        char err[96] = "";
        bool should_break = loop_eval_expr_bool(c, c->break_expr, current_turn,
                                                model_done, has_followup,
                                                g_loop_depth, &valid,
                                                err, sizeof(err));
        if (!valid) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' break expression invalid: %s", c->label, err);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
            pthread_mutex_unlock(&g_loop_lock);
            return;
        }
        if (should_break) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' break condition matched: %s",
                     c->label, c->break_expr);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
            pthread_mutex_unlock(&g_loop_lock);
            return;
        }
    }

    if (boundary && c->override_done) {
        bool should_continue = true;
        bool expr_valid = true;
        char expr_err[96] = "";
        if (c->continue_expr[0]) {
            should_continue = loop_eval_expr_bool(c, c->continue_expr,
                                                  current_turn, model_done,
                                                  has_followup, g_loop_depth,
                                                  &expr_valid,
                                                  expr_err, sizeof(expr_err));
        }

        if (!expr_valid) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' continue expression invalid: %s",
                     c->label, expr_err);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
        } else if (!should_continue) {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' continue condition false: %s",
                     c->label, c->continue_expr);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
        } else if (c->iterations < c->max_iterations) {
            c->iterations++;
            out->force_continue = true;
            snprintf(out->reason, sizeof(out->reason),
                     c->continue_expr[0]
                         ? "loop '%s' iteration %d/%d continued by DSL"
                         : "loop '%s' iteration %d/%d kept turn alive",
                     c->label, c->iterations, c->max_iterations);
            snprintf(out->prompt, sizeof(out->prompt),
                     "[StartOfLoopConstruct label=%s turn=%d iteration=%d/%d]\n"
                     "Conditions: %s\n"
                     "%s%s%s"
                     "%s\n"
                     "Re-evaluate the conditions. Continue recursively if useful, "
                     "or call EndOfLoopConstruct with action=break or action=complete "
                     "to exit this construct.",
                     c->label, current_turn, c->iterations, c->max_iterations,
                     c->conditions,
                     c->continue_expr[0] ? "ContinueWhen: " : "",
                     c->continue_expr[0] ? c->continue_expr : "",
                     c->continue_expr[0] ? "\n" : "",
                     c->continue_prompt[0] ? c->continue_prompt
                                           : "Continue the active loop construct.");
        } else {
            snprintf(out->reason, sizeof(out->reason),
                     "loop '%s' reached max_iterations=%d",
                     c->label, c->max_iterations);
            out->force_done = true;
            loop_pop_from_locked(g_loop_depth - 1);
        }
    }

    pthread_mutex_unlock(&g_loop_lock);
}

static bool tool_start_loop_construct(const char *input, char *result,
                                      size_t rlen) {
    char *label = json_get_str(input, "label");
    char *program = json_get_str(input, "program");
    if (!program) program = json_get_str(input, "construct");
    char *conditions = json_get_str(input, "conditions");
    if (!conditions) conditions = json_get_str(input, "condition");
    char *continue_prompt = json_get_str(input, "continue_prompt");

    int max_iterations = json_get_int(input, "max_iterations", 8);
    max_iterations = loop_clamp_int(max_iterations, 1, 10000);
    int max_turns = json_get_int(input, "max_turns", 0);
    if (max_turns <= 0) {
        max_turns = dsco_max_agent_turns() + max_iterations + 2;
    }
    max_turns = loop_clamp_int(max_turns, 1, 999999);

    bool recursive = json_get_bool(input, "recursive", true);
    bool override_done = json_get_bool(input, "override_done", true);
    bool override_max_turns = json_get_bool(input, "override_max_turns", true);

    pthread_mutex_lock(&g_loop_lock);
    if (g_loop_depth > 0 && !g_loop_stack[g_loop_depth - 1].recursive) {
        pthread_mutex_unlock(&g_loop_lock);
        snprintf(result, rlen,
                 "{\"ok\":false,\"error\":\"active loop is not recursive\"}");
        free(label); free(program); free(conditions); free(continue_prompt);
        return false;
    }
    if (g_loop_depth >= LOOP_STACK_MAX) {
        pthread_mutex_unlock(&g_loop_lock);
        snprintf(result, rlen,
                 "{\"ok\":false,\"error\":\"loop stack capacity reached\"}");
        free(label); free(program); free(conditions); free(continue_prompt);
        return false;
    }

    loop_construct_t *c = &g_loop_stack[g_loop_depth++];
    memset(c, 0, sizeof(*c));
    c->active = true;
    c->id = g_loop_next_id++;
    c->max_iterations = max_iterations;
    c->max_turns = max_turns;
    c->recursive = recursive;
    c->override_done = override_done;
    c->override_max_turns = override_max_turns;
    loop_copy(c->label, sizeof(c->label), label, "loop");
    loop_copy(c->conditions, sizeof(c->conditions),
              program && program[0] ? program : conditions,
              "continue until the task is complete or explicitly broken");
    loop_copy(c->continue_prompt, sizeof(c->continue_prompt), continue_prompt,
              "Continue executing this loop construct.");
    loop_apply_program_locked(c, c->conditions);

    int depth = g_loop_depth;
    jbuf_t out;
    jbuf_init(&out, 256);
    jbuf_append(&out, "{\"ok\":true,\"construct\":\"StartOfLoopConstruct\"");
    jbuf_appendf(&out, ",\"id\":%d,\"label\":", c->id);
    jbuf_append_json_str(&out, c->label);
    jbuf_appendf(&out, ",\"depth\":%d,\"max_iterations\":%d,\"max_turns\":%d",
                 depth, c->max_iterations, c->max_turns);
    jbuf_appendf(&out, ",\"override_done\":%s,\"override_max_turns\":%s,"
                 "\"recursive\":%s,\"dsl\":%s",
                 c->override_done ? "true" : "false",
                 c->override_max_turns ? "true" : "false",
                 c->recursive ? "true" : "false",
                 c->dsl_enabled ? "true" : "false");
    if (c->continue_expr[0]) {
        jbuf_append(&out, ",\"continue_when\":");
        jbuf_append_json_str(&out, c->continue_expr);
    }
    if (c->break_expr[0]) {
        jbuf_append(&out, ",\"break_when\":");
        jbuf_append_json_str(&out, c->break_expr);
    }
    jbuf_appendf(&out, ",\"meta_count\":%d,\"dyad_count\":%d,"
                 "\"node_count\":%d,\"edge_count\":%d,"
                 "\"refine_count\":%d,\"rewrite_count\":%d,"
                 "\"mapreduce_count\":%d,\"map_count\":%d,"
                 "\"shuffle_count\":%d,\"reduce_count\":%d,"
                 "\"partition_count\":%d,"
                 "\"srm_count\":%d,\"certificate_count\":%d,"
                 "\"current_certificate_count\":%d,\"sds_count\":%d,"
                 "\"traceability_count\":%d,\"measurement_count\":%d,"
                 "\"calibration_count\":%d,\"uncertainty_budget_count\":%d,"
                 "\"available_count\":%d,\"orderable_count\":%d,"
                 "\"shipping_block_count\":%d,\"product_search_count\":%d,"
                 "\"catalog_count\":%d,\"annual_catalog_count\":%d,"
                 "\"licensed_distributor_count\":%d,"
                 "\"order_policy_count\":%d,\"paper_checks_blocked\":%s,"
                 "\"effects\":{\"conversational\":%.3f,"
                 "\"tool\":%.3f,\"world\":%.3f,\"meta\":%.3f}",
                 c->meta_count, c->dyad_count, c->graph_node_count,
                 c->graph_edge_count, c->refine_count, c->rewrite_count,
                 c->mapreduce_job_count, c->map_count, c->shuffle_count,
                 c->reduce_count, c->partition_count,
                 c->srm_count, loop_srm_certificate_count(c),
                 loop_srm_current_certificate_count(c), loop_srm_sds_count(c),
                 loop_srm_traceability_count(c), c->measurement_count,
                 loop_measurement_calibration_count(c),
                 c->uncertainty_budget_count,
                 loop_srm_available_count(c), loop_srm_orderable_count(c),
                 loop_srm_shipping_block_count(c),
                 loop_srm_product_search_count(c),
                 loop_srm_operation_count_kind(c, "catalog"),
                 loop_srm_operation_count_kind(c, "annual_catalog"),
                 loop_srm_operation_count_kind(c, "licensed_distributor"),
                 loop_srm_operation_count_kind(c, "order_policy"),
                 loop_srm_operation_has(c, "order_policy", "no_paper_checks")
                     ? "true" : "false",
                 c->effect_conversational, c->effect_tool,
                 c->effect_world, c->effect_meta);
    jbuf_append(&out, "}");
    snprintf(result, rlen, "%s", out.data ? out.data : "{}");
    jbuf_free(&out);
    pthread_mutex_unlock(&g_loop_lock);

    free(label); free(program); free(conditions); free(continue_prompt);
    return true;
}

static bool tool_end_loop_construct(const char *input, char *result,
                                    size_t rlen) {
    char *label = json_get_str(input, "label");
    char *action = json_get_str(input, "action");
    char *reason = json_get_str(input, "reason");
    char *program = json_get_str(input, "program");
    if (!program) program = json_get_str(input, "construct");
    char *conditions = json_get_str(input, "conditions");
    char *continue_prompt = json_get_str(input, "continue_prompt");
    bool all = json_get_bool(input, "all", false);
    bool exit_break_conditions = json_get_bool(input, "exit_break_conditions", false);

    const char *act = (action && action[0]) ? action : "complete";

    pthread_mutex_lock(&g_loop_lock);
    int idx = all ? 0 : loop_find_locked(label);
    if (idx < 0 || g_loop_depth <= 0) {
        pthread_mutex_unlock(&g_loop_lock);
        snprintf(result, rlen,
                 "{\"ok\":false,\"error\":\"loop construct not found\"}");
        free(label); free(action); free(reason); free(program);
        free(conditions); free(continue_prompt);
        return false;
    }

    if (strcmp(act, "continue") == 0 || strcmp(act, "recur") == 0) {
        loop_construct_t *c = &g_loop_stack[idx];
        const char *new_program = (program && program[0]) ? program : conditions;
        if (new_program && new_program[0]) {
            loop_copy(c->conditions, sizeof(c->conditions), new_program, NULL);
            loop_apply_program_locked(c, c->conditions);
        }
        if (continue_prompt && continue_prompt[0])
            loop_copy(c->continue_prompt, sizeof(c->continue_prompt),
                      continue_prompt, NULL);
        if (exit_break_conditions) {
            c->iterations = 0;
            c->override_done = true;
            c->override_max_turns = true;
        }
        jbuf_t out;
        jbuf_init(&out, 256);
        jbuf_append(&out,
                    "{\"ok\":true,\"construct\":\"EndOfLoopConstruct\","
                    "\"action\":\"continue\",\"label\":");
        jbuf_append_json_str(&out, c->label);
        jbuf_appendf(&out, ",\"depth\":%d,\"exit_break_conditions\":%s",
                     g_loop_depth, exit_break_conditions ? "true" : "false");
        if (c->continue_expr[0]) {
            jbuf_append(&out, ",\"continue_when\":");
            jbuf_append_json_str(&out, c->continue_expr);
        }
        if (c->break_expr[0]) {
            jbuf_append(&out, ",\"break_when\":");
            jbuf_append_json_str(&out, c->break_expr);
        }
        jbuf_appendf(&out, ",\"meta_count\":%d,\"dyad_count\":%d,"
                     "\"node_count\":%d,\"edge_count\":%d,"
                     "\"refine_count\":%d,\"rewrite_count\":%d,"
                     "\"mapreduce_count\":%d,\"map_count\":%d,"
                     "\"shuffle_count\":%d,\"reduce_count\":%d,"
                     "\"partition_count\":%d,"
                     "\"srm_count\":%d,\"certificate_count\":%d,"
                     "\"current_certificate_count\":%d,\"sds_count\":%d,"
                     "\"traceability_count\":%d,\"measurement_count\":%d,"
                     "\"calibration_count\":%d,\"uncertainty_budget_count\":%d,"
                     "\"available_count\":%d,\"orderable_count\":%d,"
                     "\"shipping_block_count\":%d,\"product_search_count\":%d,"
                     "\"catalog_count\":%d,\"annual_catalog_count\":%d,"
                     "\"licensed_distributor_count\":%d,"
                     "\"order_policy_count\":%d,\"paper_checks_blocked\":%s,"
                     "\"refinements_applied\":%d,\"rewrites_applied\":%d}",
                     c->meta_count, c->dyad_count, c->graph_node_count,
                     c->graph_edge_count, c->refine_count,
                     c->rewrite_count, c->mapreduce_job_count, c->map_count,
                     c->shuffle_count, c->reduce_count, c->partition_count,
                     c->srm_count, loop_srm_certificate_count(c),
                     loop_srm_current_certificate_count(c), loop_srm_sds_count(c),
                     loop_srm_traceability_count(c), c->measurement_count,
                     loop_measurement_calibration_count(c),
                     c->uncertainty_budget_count,
                     loop_srm_available_count(c), loop_srm_orderable_count(c),
                     loop_srm_shipping_block_count(c),
                     loop_srm_product_search_count(c),
                     loop_srm_operation_count_kind(c, "catalog"),
                     loop_srm_operation_count_kind(c, "annual_catalog"),
                     loop_srm_operation_count_kind(c, "licensed_distributor"),
                     loop_srm_operation_count_kind(c, "order_policy"),
                     loop_srm_operation_has(c, "order_policy", "no_paper_checks")
                         ? "true" : "false",
                     c->refinements_applied,
                     c->rewrites_applied);
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
    } else {
        int removed = g_loop_depth - idx;
        char removed_label[LOOP_LABEL_MAX];
        snprintf(removed_label, sizeof(removed_label), "%s",
                 g_loop_stack[idx].label);
        for (int i = idx; i < g_loop_depth; i++)
            memset(&g_loop_stack[i], 0, sizeof(g_loop_stack[i]));
        g_loop_depth = idx;
        jbuf_t out;
        jbuf_init(&out, 256);
        jbuf_append(&out,
                    "{\"ok\":true,\"construct\":\"EndOfLoopConstruct\","
                    "\"action\":");
        jbuf_append_json_str(&out, act);
        jbuf_appendf(&out, ",\"removed\":%d,\"label\":", removed);
        jbuf_append_json_str(&out, removed_label);
        jbuf_appendf(&out, ",\"depth\":%d,\"reason\":", g_loop_depth);
        jbuf_append_json_str(&out, reason ? reason : "");
        jbuf_append(&out, "}");
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
    }
    pthread_mutex_unlock(&g_loop_lock);

    free(label); free(action); free(reason); free(program);
    free(conditions); free(continue_prompt);
    return true;
}

static bool tool_loop_construct_status(const char *input, char *result,
                                       size_t rlen) {
    (void)input;
    pthread_mutex_lock(&g_loop_lock);
    jbuf_t out;
    jbuf_init(&out, 512);
    jbuf_appendf(&out, "{\"active\":%s,\"depth\":%d,\"stack\":[",
                 g_loop_depth > 0 ? "true" : "false", g_loop_depth);
    for (int i = 0; i < g_loop_depth; i++) {
        loop_construct_t *c = &g_loop_stack[i];
        if (i) jbuf_append(&out, ",");
        jbuf_appendf(&out, "{\"id\":%d,\"label\":", c->id);
        jbuf_append_json_str(&out, c->label);
        jbuf_appendf(&out, ",\"iteration\":%d,\"max_iterations\":%d,"
                     "\"max_turns\":%d,\"override_done\":%s,"
                     "\"override_max_turns\":%s,\"recursive\":%s,"
                     "\"dsl\":%s,\"conditions\":",
                     c->iterations, c->max_iterations, c->max_turns,
                     c->override_done ? "true" : "false",
                     c->override_max_turns ? "true" : "false",
                     c->recursive ? "true" : "false",
                     c->dsl_enabled ? "true" : "false");
        jbuf_append_json_str(&out, c->conditions);
        if (c->continue_expr[0]) {
            jbuf_append(&out, ",\"continue_when\":");
            jbuf_append_json_str(&out, c->continue_expr);
        }
        if (c->break_expr[0]) {
            jbuf_append(&out, ",\"break_when\":");
            jbuf_append_json_str(&out, c->break_expr);
        }
        jbuf_appendf(&out,
                     ",\"effects\":{\"conversational\":%.3f,\"tool\":%.3f,"
                     "\"world\":%.3f,\"meta\":%.3f}",
                     c->effect_conversational, c->effect_tool,
                     c->effect_world, c->effect_meta);
        jbuf_appendf(&out,
                     ",\"signals\":{\"reward\":%.3f,\"curiosity\":%.3f,"
                     "\"empowerment\":%.3f,\"confidence\":%.3f,"
                     "\"uncertainty\":%.3f,\"learning_rate\":%.3f,"
                     "\"valence\":%.3f,\"intensity\":%.3f,"
                     "\"exploration_rate\":%.3f,\"credit\":%.3f,"
                     "\"pruning_threshold\":%.3f,\"basin_temperature\":%.3f}",
                     c->reward, c->curiosity, c->empowerment, c->confidence,
                     c->uncertainty, c->learning_rate, c->valence,
                     c->intensity, c->exploration_rate, c->credit,
                     c->pruning_threshold, c->basin_temperature);
        if (c->policy[0]) {
            jbuf_append(&out, ",\"policy\":");
            jbuf_append_json_str(&out, c->policy);
        }
        if (c->decision[0]) {
            jbuf_append(&out, ",\"decision\":");
            jbuf_append_json_str(&out, c->decision);
        }
        if (c->attractor[0]) {
            jbuf_append(&out, ",\"attractor\":");
            jbuf_append_json_str(&out, c->attractor);
        }
        if (c->prompt_game[0]) {
            jbuf_append(&out, ",\"prompt_game\":");
            jbuf_append_json_str(&out, c->prompt_game);
        }
        jbuf_append(&out, ",\"meta\":[");
        for (int m = 0; m < c->meta_count; m++) {
            loop_meta_entry_t *e = &c->meta[m];
            if (m) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"kind\":");
            jbuf_append_json_str(&out, e->kind);
            jbuf_append(&out, ",\"name\":");
            jbuf_append_json_str(&out, e->name);
            jbuf_append(&out, ",\"value\":");
            jbuf_append_json_str(&out, e->value);
            jbuf_appendf(&out, ",\"weight\":%.3f}", e->weight);
        }
        jbuf_append(&out, "],\"dyads\":[");
        for (int d = 0; d < c->dyad_count; d++) {
            loop_dyad_t *dy = &c->dyads[d];
            if (d) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"from\":");
            jbuf_append_json_str(&out, dy->from);
            jbuf_append(&out, ",\"to\":");
            jbuf_append_json_str(&out, dy->to);
            jbuf_append(&out, ",\"relation\":");
            jbuf_append_json_str(&out, dy->relation);
            jbuf_append(&out, "}");
        }
        jbuf_appendf(&out,
                     "],\"graph\":{\"node_count\":%d,\"edge_count\":%d,"
                     "\"density\":%.3f,\"traverse_from\":",
                     c->graph_node_count, c->graph_edge_count,
                     loop_graph_density(c));
        jbuf_append_json_str(&out, c->traverse_from);
        jbuf_appendf(&out, ",\"traverse_depth\":%d,\"traverse_hits\":%d,"
                     "\"nodes\":[", c->traverse_depth, c->traverse_hits);
        for (int n = 0; n < c->graph_node_count; n++) {
            loop_graph_node_t *gn = &c->graph_nodes[n];
            if (n) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"name\":");
            jbuf_append_json_str(&out, gn->name);
            jbuf_append(&out, ",\"type\":");
            jbuf_append_json_str(&out, gn->type);
            jbuf_append(&out, ",\"state\":");
            jbuf_append_json_str(&out, gn->state);
            jbuf_appendf(&out, ",\"weight\":%.3f}", gn->weight);
        }
        jbuf_append(&out, "],\"edges\":[");
        for (int e = 0; e < c->graph_edge_count; e++) {
            loop_graph_edge_t *ge = &c->graph_edges[e];
            if (e) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"from\":");
            jbuf_append_json_str(&out, ge->from);
            jbuf_append(&out, ",\"to\":");
            jbuf_append_json_str(&out, ge->to);
            jbuf_append(&out, ",\"relation\":");
            jbuf_append_json_str(&out, ge->relation);
            jbuf_appendf(&out, ",\"weight\":%.3f}", ge->weight);
        }
        jbuf_appendf(&out,
                     "]},\"mapreduce\":{\"job_count\":%d,\"map_count\":%d,"
                     "\"shuffle_count\":%d,\"reduce_count\":%d,"
                     "\"partition_count\":%d,\"jobs\":[",
                     c->mapreduce_job_count, c->map_count, c->shuffle_count,
                     c->reduce_count, c->partition_count);
        for (int mr = 0; mr < c->mapreduce_job_count; mr++) {
            loop_mapreduce_job_t *job = &c->mapreduce_jobs[mr];
            if (mr) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"name\":");
            jbuf_append_json_str(&out, job->name);
            jbuf_append(&out, ",\"source\":");
            jbuf_append_json_str(&out, job->source);
            jbuf_append(&out, ",\"mapper\":");
            jbuf_append_json_str(&out, job->mapper);
            jbuf_append(&out, ",\"reducer\":");
            jbuf_append_json_str(&out, job->reducer);
            jbuf_append(&out, ",\"key\":");
            jbuf_append_json_str(&out, job->key);
            jbuf_append(&out, ",\"target\":");
            jbuf_append_json_str(&out, job->target);
            jbuf_appendf(&out,
                         ",\"partitions\":%d,\"mapped\":%s,"
                         "\"shuffled\":%s,\"reduced\":%s}",
                         job->partitions,
                         job->mapped ? "true" : "false",
                         job->shuffled ? "true" : "false",
                         job->reduced ? "true" : "false");
        }
        jbuf_appendf(&out,
                     "]},\"srm\":{\"material_count\":%d,"
                     "\"certificate_count\":%d,"
                     "\"current_certificate_count\":%d,"
                     "\"sds_count\":%d,\"traceability_count\":%d,"
                     "\"measurement_count\":%d,\"calibration_count\":%d,"
                     "\"uncertainty_budget_count\":%d,"
                     "\"available_count\":%d,\"orderable_count\":%d,"
                     "\"shipping_block_count\":%d,"
                     "\"product_search_count\":%d,"
                     "\"archived_certificate_count\":%d,"
                     "\"catalog_count\":%d,\"annual_catalog_count\":%d,"
                     "\"licensed_distributor_count\":%d,"
                     "\"order_policy_count\":%d,"
                     "\"registration_count\":%d,\"survey_count\":%d,"
                     "\"paper_checks_blocked\":%s,"
                     "\"price_total\":%.6f,"
                     "\"mean_uncertainty\":%.6f,\"max_uncertainty\":%.6f,"
                     "\"materials\":[",
                     c->srm_count, loop_srm_certificate_count(c),
                     loop_srm_current_certificate_count(c),
                     loop_srm_sds_count(c), loop_srm_traceability_count(c),
                     c->measurement_count, loop_measurement_calibration_count(c),
                     c->uncertainty_budget_count,
                     loop_srm_available_count(c), loop_srm_orderable_count(c),
                     loop_srm_shipping_block_count(c),
                     loop_srm_product_search_count(c),
                     loop_srm_archived_certificate_count(c),
                     loop_srm_operation_count_kind(c, "catalog"),
                     loop_srm_operation_count_kind(c, "annual_catalog"),
                     loop_srm_operation_count_kind(c, "licensed_distributor"),
                     loop_srm_operation_count_kind(c, "order_policy"),
                     loop_srm_operation_count_kind(c, "registration"),
                     loop_srm_operation_count_kind(c, "survey"),
                     loop_srm_operation_has(c, "order_policy", "no_paper_checks")
                         ? "true" : "false",
                     loop_srm_total_price(c),
                     loop_srm_mean_uncertainty(c),
                     loop_srm_max_uncertainty(c));
        for (int s = 0; s < c->srm_count; s++) {
            loop_srm_entry_t *sr = &c->srm_entries[s];
            if (s) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"id\":");
            jbuf_append_json_str(&out, sr->id);
            jbuf_append(&out, ",\"name\":");
            jbuf_append_json_str(&out, sr->name);
            jbuf_append(&out, ",\"matrix\":");
            jbuf_append_json_str(&out, sr->matrix);
            jbuf_append(&out, ",\"property\":");
            jbuf_append_json_str(&out, sr->property);
            jbuf_append(&out, ",\"certificate\":");
            jbuf_append_json_str(&out, sr->certificate);
            jbuf_append(&out, ",\"report\":");
            jbuf_append_json_str(&out, sr->report);
            jbuf_append(&out, ",\"sds\":");
            jbuf_append_json_str(&out, sr->sds);
            jbuf_appendf(&out,
                         ",\"assigned_value\":%.6f,\"uncertainty\":%.6f,"
                         "\"price\":%.6f,\"destination\":",
                         sr->assigned_value, sr->uncertainty, sr->price);
            jbuf_append_json_str(&out, sr->destination);
            jbuf_append(&out, ",\"distributor\":");
            jbuf_append_json_str(&out, sr->distributor);
            jbuf_append(&out, ",\"store\":");
            jbuf_append_json_str(&out, sr->store);
            jbuf_appendf(&out,
                         ",\"certificate_current\":%s,\"sds_available\":%s,"
                         "\"traceable\":%s,\"available\":%s,"
                         "\"orderable\":%s,\"shipping_blocked\":%s,"
                         "\"product_search_found\":%s,"
                         "\"archived_certificate\":%s}",
                         sr->certificate_current ? "true" : "false",
                         sr->sds_available ? "true" : "false",
                         sr->traceable ? "true" : "false",
                         sr->available ? "true" : "false",
                         sr->orderable ? "true" : "false",
                         sr->shipping_blocked ? "true" : "false",
                         sr->product_search_found ? "true" : "false",
                         sr->archived_certificate ? "true" : "false");
        }
        jbuf_append(&out, "],\"measurements\":[");
        for (int m = 0; m < c->measurement_count; m++) {
            loop_measurement_entry_t *me = &c->measurements[m];
            if (m) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"name\":");
            jbuf_append_json_str(&out, me->name);
            jbuf_append(&out, ",\"material\":");
            jbuf_append_json_str(&out, me->material);
            jbuf_append(&out, ",\"property\":");
            jbuf_append_json_str(&out, me->property);
            jbuf_append(&out, ",\"unit\":");
            jbuf_append_json_str(&out, me->unit);
            jbuf_append(&out, ",\"method\":");
            jbuf_append_json_str(&out, me->method);
            jbuf_appendf(&out,
                         ",\"value\":%.6f,\"uncertainty\":%.6f,"
                         "\"calibrated\":%s,\"uncertainty_budget\":%s}",
                         me->value, me->uncertainty,
                         me->calibrated ? "true" : "false",
                         me->has_uncertainty_budget ? "true" : "false");
        }
        jbuf_append(&out, "],\"operations\":[");
        for (int opi = 0; opi < c->srm_operation_count; opi++) {
            loop_srm_operation_t *op = &c->srm_operations[opi];
            if (opi) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"kind\":");
            jbuf_append_json_str(&out, op->kind);
            jbuf_append(&out, ",\"name\":");
            jbuf_append_json_str(&out, op->name);
            jbuf_append(&out, ",\"value\":");
            jbuf_append_json_str(&out, op->value);
            jbuf_appendf(&out, ",\"amount\":%.6f,\"flag\":%s}",
                         op->amount, op->flag ? "true" : "false");
        }
        jbuf_appendf(&out, "]},\"refinements_applied\":%d,\"refinements\":[",
                     c->refinements_applied);
        for (int r = 0; r < c->refine_count; r++) {
            loop_refine_rule_t *rr = &c->refine_rules[r];
            if (r) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"target\":");
            jbuf_append_json_str(&out, rr->target);
            jbuf_append(&out, ",\"op\":");
            jbuf_append_json_str(&out, rr->op);
            jbuf_appendf(&out, ",\"value\":%.3f,\"when\":", rr->value);
            jbuf_append_json_str(&out, rr->when);
            jbuf_appendf(&out, ",\"fired\":%s}", rr->fired ? "true" : "false");
        }
        jbuf_appendf(&out, "],\"rewrites_applied\":%d,\"rewrites\":[",
                     c->rewrites_applied);
        for (int r = 0; r < c->rewrite_count; r++) {
            loop_rewrite_rule_t *rw = &c->rewrite_rules[r];
            if (r) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"action\":");
            jbuf_append_json_str(&out, rw->action);
            jbuf_append(&out, ",\"when\":");
            jbuf_append_json_str(&out, rw->when);
            jbuf_appendf(&out, ",\"fired\":%s}", rw->fired ? "true" : "false");
        }
        jbuf_append(&out, "]");
        jbuf_append(&out, "}");
    }
    jbuf_append(&out, "]}");
    snprintf(result, rlen, "%s", out.data ? out.data : "{}");
    jbuf_free(&out);
    pthread_mutex_unlock(&g_loop_lock);
    return true;
}

/* ── Discover tools (meta-tool for lazy loading) ───────────────────── */

/* Forward declaration — defined after assign_group() below */
static int build_compact_params(const char *schema, char *out, size_t outlen);

static bool tools_ci_contains_local(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static bool tool_matches_query_local(const char *name, const char *desc,
                                     const char *query) {
    if (!query || !query[0]) return true;
    if (tools_ci_contains_local(name, query) ||
        tools_ci_contains_local(desc ? desc : "", query)) {
        return true;
    }

    char token[64];
    int tlen = 0;
    int matched = 0;
    int total = 0;
    for (const char *p = query; ; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_' || c == '-') {
            if (tlen < (int)sizeof(token) - 1)
                token[tlen++] = (char)c;
            continue;
        }
        if (tlen > 0) {
            token[tlen] = '\0';
            total++;
            if (tools_ci_contains_local(name, token) ||
                tools_ci_contains_local(desc ? desc : "", token)) {
                matched++;
            }
            tlen = 0;
        }
        if (!*p) break;
    }
    return total > 0 && matched == total;
}

static void discover_append_tool_detail(jbuf_t *b, const char *name,
                                        const char *desc, const char *schema,
                                        const char *source) {
    char params[256];
    build_compact_params(schema, params, sizeof(params));
    jbuf_append(b, "{\"name\":");
    jbuf_append_json_str(b, name ? name : "");
    jbuf_append(b, ",\"source\":");
    jbuf_append_json_str(b, source ? source : "builtin");
    jbuf_append(b, ",\"signature\":");
    jbuf_append_json_str(b, params);
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, desc ? desc : "");
    jbuf_append(b, ",\"input_schema\":");
    jbuf_append(b, schema ? schema : "{\"type\":\"object\",\"properties\":{}}");
    jbuf_append(b, "}");
}

static bool tool_discover_tools(const char *input, char *result, size_t rlen) {
    /* Parse optional category filter */
    char category[64] = "";
    char *query = input ? json_get_str(input, "query") : NULL;
    char *category_owned = input ? json_get_str(input, "category") : NULL;
    if (category_owned && category_owned[0]) {
        snprintf(category, sizeof(category), "%s", category_owned);
    }
    if (input) {
        const char *c = strstr(input, "\"category\"");
        if (c) {
            c = strchr(c + 10, '"');
            if (c) {
                c++;
                const char *end = strchr(c, '"');
                if (end && (size_t)(end - c) < sizeof(category)) {
                    memcpy(category, c, end - c);
                    category[end - c] = '\0';
                }
            }
        }
    }

    int total;
    const tool_def_t *tools = tools_get_all(&total);
    int grand_total = total + g_external_tool_count;

    if (query && query[0]) {
        jbuf_t b;
        jbuf_init(&b, 4096);
        jbuf_append(&b, "{\"query\":");
        jbuf_append_json_str(&b, query);
        jbuf_appendf(&b, ",\"total_tools\":%d,\"matches\":[", grand_total);

        int emitted = 0;
        int matched = 0;
        const int limit = 8;
        bool first = true;

        /* MCP tools first: integration queries usually need exact external schemas. */
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < g_external_tool_count; i++) {
                external_tool_t *t = &g_external_tools[i];
                bool exact = strcasecmp(t->name, query) == 0;
                if ((pass == 0) != exact) continue;
                if (!exact && !tool_matches_query_local(t->name, t->description, query))
                    continue;
                matched++;
                if (emitted >= limit) continue;
                if (!first) jbuf_append(&b, ",");
                first = false;
                discover_append_tool_detail(&b, t->name, t->description,
                                            t->input_schema_json, "mcp");
                emitted++;
            }
        }

        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < total; i++) {
                bool exact = strcasecmp(tools[i].name, query) == 0;
                if ((pass == 0) != exact) continue;
                if (!exact && !tool_matches_query_local(tools[i].name,
                                                        tools[i].description,
                                                        query)) {
                    continue;
                }
                matched++;
                if (emitted >= limit) continue;
                if (!first) jbuf_append(&b, ",");
                first = false;
                discover_append_tool_detail(&b, tools[i].name, tools[i].description,
                                            tools[i].input_schema_json, "builtin");
                emitted++;
            }
        }

        jbuf_appendf(&b,
            "],\"matched\":%d,\"showing\":%d,\"truncated\":%s,"
            "\"note\":\"Full input_schema returned for matches. Use load_tools with exact names to pin MCP tools into the active tool set.\"}",
            matched, emitted, matched > emitted ? "true" : "false");
        snprintf(result, rlen, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
        free(query);
        free(category_owned);
        return true;
    }

    /* Build JSON array of tool summaries grouped by category */
    int off = 0;
    off += snprintf(result + off, rlen - off,
        "{\"total_tools\":%d,\"builtin_tools\":%d,\"mcp_tools\":%d,"
        "\"note\":\"Use query for exact MCP schemas, then load_tools to pin names.\",\"tools\":[",
        grand_total, total, g_external_tool_count);

    /* Group names for display */
    const char *group_names[] = {
        "file_io", "git", "network", "shell", "code", "crypto",
        "swarm", "ast", "pipeline", "math", "search", "general",
        "finance", "prediction", "memory"
    };
    int group_count = 15;

    /* Simple categorization: list tools by group */
    for (int g = 0; g < group_count && (size_t)off < rlen - 100; g++) {
        /* Filter by category if specified */
        if (category[0] && strcasecmp(category, group_names[g]) != 0) continue;

        if (off > 80) off += snprintf(result + off, rlen - off, ",");
        off += snprintf(result + off, rlen - off, "{\"category\":\"%s\",\"tools\":[", group_names[g]);

        bool first = true;
        for (int i = 0; i < total && (size_t)off < rlen - 200; i++) {
            int gid = -1;
            const char *n = tools[i].name;
            /* Quick group assignment matching assign_group() */
            if (strncmp(n, "git_", 4) == 0 || strncmp(n, "github_", 7) == 0) gid = 1;
            else if (strncmp(n, "av_", 3) == 0 || strncmp(n, "fred_", 5) == 0 ||
                     strstr(n, "market") || strstr(n, "stripe")) gid = 12;
            else if (strstr(n, "polymarket") || strstr(n, "kalshi") || strstr(n, "prediction")) gid = 13;
            else if (strncmp(n, "ipc_", 4) == 0 || strstr(n, "swarm") || strstr(n, "spawn") ||
                     strstr(n, "agent") || strstr(n, "topology") || strstr(n, "ooda") ||
                     strstr(n, "pheromone") || strstr(n, "talons") || strstr(n, "governance") ||
                     strstr(n, "killswitch") || strstr(n, "executor") || strstr(n, "openrouter")) gid = 6;
            else if (strstr(n, "memory_") || strstr(n, "soul_")) gid = 14;
            else if (strstr(n, "hash") || strstr(n, "hmac") || strstr(n, "jwt") ||
                     strstr(n, "uuid") || strstr(n, "random") || strstr(n, "cert")) gid = 5;
            else if (strstr(n, "http") || strstr(n, "curl") || strstr(n, "dns") ||
                     strstr(n, "ping") || strstr(n, "web") || strstr(n, "jina") ||
                     strstr(n, "slack") || strstr(n, "discord") || strstr(n, "notion") ||
                     strstr(n, "weather") || strstr(n, "firecrawl") || strstr(n, "serpapi") ||
                     strstr(n, "browser")) gid = 2;
            else if (strncmp(n, "read_", 5) == 0 || strncmp(n, "write_", 6) == 0 ||
                     strncmp(n, "edit_", 5) == 0 || strstr(n, "file") || strstr(n, "dir") ||
                     strstr(n, "tree") || strstr(n, "symlink") || strstr(n, "chmod") ||
                     strstr(n, "mkdir") || strstr(n, "copy") || strstr(n, "move") ||
                     strstr(n, "delete") || strstr(n, "append")) gid = 0;
            else if (strstr(n, "bash") || strstr(n, "run_") || strstr(n, "compile") ||
                     strstr(n, "sandbox")) gid = 3;
            else if (strstr(n, "code_") || strstr(n, "ast_") || strstr(n, "grep") ||
                     strstr(n, "find") || strstr(n, "search")) gid = 4;
            else gid = 11; /* general */

            if (gid != g) continue;
            if (!first) off += snprintf(result + off, rlen - off, ",");
            first = false;
            /* Include compact signature: name(params) — description */
            char params[256];
            build_compact_params(tools[i].input_schema_json, params, sizeof(params));
            const char *d = tools[i].description;
            size_t dlen = strlen(d);
            if (dlen > 60)
                off += snprintf(result + off, rlen - off,
                    "\"%s%s — %.57s...\"", n, params, d);
            else
                off += snprintf(result + off, rlen - off,
                    "\"%s%s — %s\"", n, params, d);
        }
        off += snprintf(result + off, rlen - off, "]}");
    }

    if ((!category[0] || strcasecmp(category, "mcp") == 0) &&
        g_external_tool_count > 0 && (size_t)off < rlen - 256) {
        if (off > 80) off += snprintf(result + off, rlen - off, ",");
        off += snprintf(result + off, rlen - off, "{\"category\":\"mcp\",\"tools\":[");
        int shown = 0;
        for (int i = 0; i < g_external_tool_count && shown < 64 &&
             (size_t)off < rlen - 300; i++) {
            if (shown > 0) off += snprintf(result + off, rlen - off, ",");
            char params[256];
            build_compact_params(g_external_tools[i].input_schema_json, params, sizeof(params));
            off += snprintf(result + off, rlen - off, "\"%s%s\"",
                            g_external_tools[i].name, params);
            shown++;
        }
        off += snprintf(result + off, rlen - off,
                        "],\"showing\":%d,\"total\":%d,"
                        "\"hint\":\"Call discover_tools with query for full MCP schemas.\"}",
                        shown, g_external_tool_count);
    }

    off += snprintf(result + off, rlen - off, "]}");
    free(query);
    free(category_owned);
    return true;
}

/* ── Load tools (dynamic tool activation via hint-pinning) ─────────── */

/* Forward declarations — defined later in this file */
static int assign_group(const char *name, const char *desc);
void tools_mark_hot(int tool_idx);

static void load_tools_add_name(char names[][64], int *name_count, const char *start, size_t len) {
    while (len > 0 && isspace((unsigned char)*start)) { start++; len--; }
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    if (len == 0 || *name_count >= 32) return;
    if ((start[0] == '"' || start[0] == '\'') && len >= 2 && start[len - 1] == start[0]) {
        start++;
        len -= 2;
    }
    if (len == 0) return;
    if (len >= 64) len = 63;
    memcpy(names[*name_count], start, len);
    names[*name_count][len] = '\0';
    (*name_count)++;
}

static bool tool_load_tools(const char *input, char *result, size_t rlen) {
    int total;
    const tool_def_t *tools = tools_get_all(&total);

    /* Parse "names": "tool_a,tool_b" and/or "tools": ["tool_a","tool_b"]. */
    char names[32][64];
    int name_count = 0;

    /* Also support "category" for bulk loading */
    char category[64] = "";

    if (input) {
        /* Parse category */
        const char *cc = strstr(input, "\"category\"");
        if (cc) {
            cc = strchr(cc + 10, '"');
            if (cc) {
                cc++;
                const char *end = strchr(cc, '"');
                if (end && (size_t)(end - cc) < sizeof(category)) {
                    memcpy(category, cc, end - cc);
                    category[end - cc] = '\0';
                }
            }
        }

        /* Parse names string (the public schema advertised this from day one). */
        const char *ns = strstr(input, "\"names\"");
        if (ns) {
            ns = strchr(ns + 7, '"');
            if (ns) {
                ns++;
                const char *end = ns;
                bool esc = false;
                while (*end) {
                    if (esc) { esc = false; end++; continue; }
                    if (*end == '\\') { esc = true; end++; continue; }
                    if (*end == '"') break;
                    end++;
                }
                const char *part = ns;
                for (const char *p = ns; p <= end && name_count < 32; p++) {
                    if (p == end || *p == ',') {
                        load_tools_add_name(names, &name_count, part, (size_t)(p - part));
                        part = p + 1;
                    }
                }
            }
        }

        /* Parse tools array */
        const char *arr = strstr(input, "\"tools\"");
        if (arr) {
            arr = strchr(arr, '[');
            if (arr) {
                arr++;
                while (*arr && *arr != ']' && name_count < 32) {
                    const char *q1 = strchr(arr, '"');
                    if (!q1 || *q1 == ']') break;
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (!q2) break;
                    size_t len = q2 - q1;
                    load_tools_add_name(names, &name_count, q1, len);
                    arr = q2 + 1;
                }
            }
        }
    }

    /* If category given, resolve all tools in that category */
    if (category[0] && name_count == 0) {
        const char *group_names[] = {
            "file_io", "git", "network", "shell", "code", "crypto",
            "swarm", "ast", "pipeline", "math", "search", "general",
            "finance", "prediction", "memory"
        };
        int target_group = -1;
        for (int g = 0; g < 15; g++) {
            if (strcasecmp(category, group_names[g]) == 0) {
                target_group = g;
                break;
            }
        }
        if (target_group >= 0) {
            for (int i = 0; i < total && name_count < 32; i++) {
                int gid = assign_group(tools[i].name, tools[i].description);
                if (gid == target_group) {
                    snprintf(names[name_count], 64, "%s", tools[i].name);
                    name_count++;
                }
            }
        }
    }

    if (name_count == 0) {
        snprintf(result, rlen,
            "{\"error\":\"No tools specified. Provide \\\"names\\\":\\\"name1,name2\\\", "
            "\\\"tools\\\":[\\\"name1\\\",\\\"name2\\\",...] "
            "or \\\"category\\\":\\\"file_io\\\" to load a group.\"}");
        return false;
    }

    /* Hint-pin each requested tool */
    int loaded = 0;
    int not_found = 0;
    char not_found_names[512] = "";
    int nf_off = 0;

    /* Build a single hint with all requested tools (up to HINT_MAX_TOOLS per hint) */
    for (int batch = 0; batch < name_count; batch += HINT_MAX_TOOLS) {
        tool_hint_t hint = {0};
        snprintf(hint.domain, sizeof(hint.domain), "load_tools_%d", batch);
        hint.weight = 1.5f;
        hint.ttl_turns = 20;  /* persist for 20 turns */
        hint.source = HINT_USER;
        hint.turn_created = 0;  /* will be set by hint_add */

        int this_batch = name_count - batch;
        if (this_batch > HINT_MAX_TOOLS) this_batch = HINT_MAX_TOOLS;

        for (int i = 0; i < this_batch; i++) {
            int idx = tool_map_lookup(&g_tool_map, names[batch + i]);
            if (idx >= 0 && idx < total) {
                snprintf(hint.tools[hint.tool_count], 64, "%s", names[batch + i]);
                hint.tool_count++;
                /* Also mark in hot cache so it auto-admits to working set */
                tools_mark_hot(idx);
                loaded++;
            } else if (idx <= -10000) {
                int ei = -(idx + 10000);
                if (ei >= 0 && ei < g_external_tool_count) {
                    g_external_tools[ei].loaded = true;
                    if (hint.tool_count < HINT_MAX_TOOLS) {
                        snprintf(hint.tools[hint.tool_count], 64, "%s", names[batch + i]);
                        hint.tool_count++;
                    }
                    loaded++;
                } else {
                    if (nf_off > 0)
                        nf_off += snprintf(not_found_names + nf_off,
                                           sizeof(not_found_names) - nf_off, ",");
                    nf_off += snprintf(not_found_names + nf_off,
                                       sizeof(not_found_names) - nf_off,
                                       "\"%s\"", names[batch + i]);
                    not_found++;
                }
            } else {
                if (nf_off > 0)
                    nf_off += snprintf(not_found_names + nf_off,
                                       sizeof(not_found_names) - nf_off, ",");
                nf_off += snprintf(not_found_names + nf_off,
                                   sizeof(not_found_names) - nf_off,
                                   "\"%s\"", names[batch + i]);
                not_found++;
            }
        }

        if (hint.tool_count > 0)
            tools_hint_add(&hint);
    }

    /* Build result: include full schemas for loaded tools so the LLM can use them immediately */
    int off = 0;
    off += snprintf(result + off, rlen - off,
        "{\"loaded\":%d,\"not_found\":%d", loaded, not_found);
    if (not_found > 0)
        off += snprintf(result + off, rlen - off,
            ",\"unknown_tools\":[%s]", not_found_names);

    off += snprintf(result + off, rlen - off, ",\"tools\":[");
    bool first = true;
    for (int i = 0; i < name_count && (size_t)off < rlen - 512; i++) {
        int idx = tool_map_lookup(&g_tool_map, names[i]);
        const char *tool_name = NULL;
        const char *tool_desc = NULL;
        const char *tool_schema = NULL;
        if (idx >= 0 && idx < total) {
            tool_name = tools[idx].name;
            tool_desc = tools[idx].description;
            tool_schema = tools[idx].input_schema_json;
        } else if (idx <= -10000) {
            int ei = -(idx + 10000);
            if (ei >= 0 && ei < g_external_tool_count) {
                tool_name = g_external_tools[ei].name;
                tool_desc = g_external_tools[ei].description;
                tool_schema = g_external_tools[ei].input_schema_json;
            }
        }
        if (!tool_name || !tool_schema) continue;
        if (!first) off += snprintf(result + off, rlen - off, ",");
        first = false;
        /* Return name + description + full input_schema so the model knows the params */
        off += snprintf(result + off, rlen - off,
            "{\"name\":\"%s\",\"description\":", tool_name);
        /* JSON-escape description */
        off += snprintf(result + off, rlen - off, "\"");
        const char *d = tool_desc ? tool_desc : "";
        for (size_t j = 0; d[j] && (size_t)off < rlen - 100; j++) {
            if (d[j] == '"') { result[off++] = '\\'; result[off++] = '"'; }
            else if (d[j] == '\\') { result[off++] = '\\'; result[off++] = '\\'; }
            else if (d[j] == '\n') { result[off++] = '\\'; result[off++] = 'n'; }
            else result[off++] = d[j];
        }
        off += snprintf(result + off, rlen - off, "\",\"input_schema\":%s}",
                        tool_schema);
    }
    off += snprintf(result + off, rlen - off,
        "],\"note\":\"These tools are now pinned in your tool list for the next 20 turns. "
        "You can call them directly.\"}");

    return true;
}

/* ── §1-§8: Post-LLM Virtual OS subsystem tools ───────────────────── */

#include "arena_alloc.h"
#include "event_loop.h"
#include "scheduler.h"
#include "vfs.h"

static bool tool_vos_status(const char *input_json, char *result, size_t rlen) {
    (void)input_json;
    arena_stats_t as = arena_get_stats();
    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"subsystems\":{");
    jbuf_appendf(&b, "\"arena\":{\"scratch_allocated\":%zu,\"session_allocated\":%zu,"
                      "\"scratch_resets\":%zu,\"temp_scopes\":%zu},",
                 as.scratch_bytes_allocated, as.session_bytes_allocated,
                 as.scratch_resets, as.temp_scopes);
    jbuf_append(&b, "\"event_loop\":{\"status\":\"initialized\"},");
    jbuf_append(&b, "\"vm\":{\"status\":\"initialized\"},");
    jbuf_append(&b, "\"scheduler\":{\"status\":\"initialized\"},");
    jbuf_append(&b, "\"vfs\":{\"status\":\"initialized\"}");
    jbuf_append(&b, "},\"reading_list_coverage\":{");
    jbuf_append(&b, "\"s1_coroutines\":\"active\",");
    jbuf_append(&b, "\"s2_arena_allocator\":\"active\",");
    jbuf_append(&b, "\"s3_bytecode_vm\":\"active\",");
    jbuf_append(&b, "\"s4_ast_introspection\":\"active\",");
    jbuf_append(&b, "\"s6_event_loop\":\"active\",");
    jbuf_append(&b, "\"s7_scheduler\":\"active\",");
    jbuf_append(&b, "\"s8_persistence\":\"active\",");
    jbuf_append(&b, "\"s9_crypto\":\"active\",");
    jbuf_append(&b, "\"s10_llm_inference\":\"api_only\",");
    jbuf_append(&b, "\"s11_metaprogramming\":\"partial\",");
    jbuf_append(&b, "\"s12_zero_dep\":\"vendor_libs\",");
    jbuf_append(&b, "\"s13_parsers\":\"active\",");
    jbuf_append(&b, "\"s14_tui\":\"active\"");
    jbuf_append(&b, "}}");
    snprintf(result, rlen, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}


/* ══════════════════════════════════════════════════════════════════════════
 *  CONSOLIDATED DISPATCHERS — system-level tool groups
 * ══════════════════════════════════════════════════════════════════════════ */

/* Forward-declare integrations.c dispatchers */
extern bool tool_alpha_vantage(const char *input, char *result, size_t rlen);
extern bool tool_kalshi(const char *input, char *result, size_t rlen);
extern bool tool_polymarket(const char *input, char *result, size_t rlen);
extern bool tool_synoptic(const char *input, char *result, size_t rlen);
extern bool tool_nws(const char *input, char *result, size_t rlen);
extern bool tool_contract_ingest(const char *input, char *result, size_t rlen);
extern bool tool_contract_search(const char *input, char *result, size_t rlen);
extern bool tool_contract_lookup(const char *input, char *result, size_t rlen);
extern bool tool_contract_ingest_all(const char *input, char *result, size_t rlen);
extern bool tool_contract_new_issues(const char *input, char *result, size_t rlen);
extern bool tool_contract_landscape(const char *input, char *result, size_t rlen);

/* ── Claude Code compatibility shims ──────────────────────────────────── */

static pthread_mutex_t g_cc_todo_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_cc_todos_json = NULL;
static bool g_cc_plan_mode = false;

static int cc_todo_item_count(const char *todos_json) {
    int count = 0;
    const char *p = todos_json ? todos_json : "";
    while ((p = strstr(p, "\"content\"")) != NULL) {
        count++;
        p += 9;
    }
    return count;
}

static void cc_map_optional_string_field(jbuf_t *mapped,
                                         const char *input,
                                         const char *field) {
    char *value = json_get_str(input, field);
    if (!value) return;
    jbuf_append(mapped, ",\"");
    jbuf_append(mapped, field);
    jbuf_append(mapped, "\":");
    jbuf_append_json_str(mapped, value);
    free(value);
}

static bool tool_bash_compat(const char *input, char *result, size_t rlen) {
    char *command = json_get_str(input, "command");
    if (!command) {
        snprintf(result, rlen, "error: command required");
        return false;
    }

    int timeout = json_get_int(input, "timeout", 120);
    if (timeout > 1000) timeout = (timeout + 999) / 1000; /* Claude Code uses ms */
    if (timeout <= 0) timeout = 120;
    if (timeout > 600) timeout = 600;
    char *cwd = json_get_str(input, "cwd");

    jbuf_t mapped;
    jbuf_init(&mapped, strlen(command) + (cwd ? strlen(cwd) : 0) + 128);
    jbuf_append(&mapped, "{\"command\":");
    jbuf_append_json_str(&mapped, command);
    jbuf_append(&mapped, ",\"timeout\":");
    jbuf_append_int(&mapped, timeout);
    if (cwd && cwd[0]) {
        jbuf_append(&mapped, ",\"cwd\":");
        jbuf_append_json_str(&mapped, cwd);
    }
    cc_map_optional_string_field(&mapped, input, "verify_path");
    cc_map_optional_string_field(&mapped, input, "artifact_path");
    cc_map_optional_string_field(&mapped, input, "output_path");
    cc_map_optional_string_field(&mapped, input, "verify_contains");
    cc_map_optional_string_field(&mapped, input, "verify_sha256");
    int verify_min_bytes = json_get_int(input, "verify_min_bytes", -1);
    if (verify_min_bytes >= 0) {
        jbuf_append(&mapped, ",\"verify_min_bytes\":");
        jbuf_append_int(&mapped, verify_min_bytes);
    }
    char *verify_paths = json_get_raw(input, "verify_paths");
    if (verify_paths) {
        jbuf_append(&mapped, ",\"verify_paths\":");
        jbuf_append(&mapped, verify_paths);
        free(verify_paths);
    }
    jbuf_append(&mapped, "}");

    bool ok = tool_bash(mapped.data, result, rlen);
    jbuf_free(&mapped);
    free(command);
    free(cwd);
    return ok;
}

static bool tool_agent_compat(const char *input, char *result, size_t rlen) {
    char *task = json_get_str(input, "prompt");
    if (!task) task = json_get_str(input, "task");
    if (!task) task = json_get_str(input, "description");
    if (!task) {
        snprintf(result, rlen, "error: prompt/task required");
        return false;
    }

    char *model = json_get_str(input, "model");
    jbuf_t mapped;
    jbuf_init(&mapped, strlen(task) + (model ? strlen(model) : 0) + 96);
    jbuf_append(&mapped, "{\"task\":");
    jbuf_append_json_str(&mapped, task);
    if (model && model[0]) {
        jbuf_append(&mapped, ",\"model\":");
        jbuf_append_json_str(&mapped, model);
    }
    jbuf_append(&mapped, "}");

    bool ok = tool_spawn_agent(mapped.data, result, rlen);
    jbuf_free(&mapped);
    free(task);
    free(model);
    return ok;
}

static bool tool_todo_write_compat(const char *input, char *result, size_t rlen) {
    char *todos = json_get_raw(input, "todos");
    if (!todos) {
        snprintf(result, rlen, "error: todos array required");
        return false;
    }

    pthread_mutex_lock(&g_cc_todo_lock);
    char *old = g_cc_todos_json ? safe_strdup(g_cc_todos_json) : safe_strdup("[]");
    free(g_cc_todos_json);
    g_cc_todos_json = safe_strdup(todos);
    int count = cc_todo_item_count(todos);
    pthread_mutex_unlock(&g_cc_todo_lock);

    jbuf_t out;
    jbuf_init(&out, strlen(old) + strlen(todos) + 256);
    jbuf_append(&out, "{\"ok\":true,\"message\":\"Todos have been modified successfully. Continue using the todo list to track progress.\",\"count\":");
    jbuf_append_int(&out, count);
    jbuf_append(&out, ",\"oldTodos\":");
    jbuf_append(&out, old);
    jbuf_append(&out, ",\"newTodos\":");
    jbuf_append(&out, todos);
    jbuf_append(&out, "}");
    snprintf(result, rlen, "%s", out.data ? out.data : "{\"ok\":true}");

    jbuf_free(&out);
    free(old);
    free(todos);
    return true;
}

static bool tool_task_list_compat(const char *input, char *result, size_t rlen) {
    (void)input;
    pthread_mutex_lock(&g_cc_todo_lock);
    const char *todos = g_cc_todos_json ? g_cc_todos_json : "[]";
    int count = cc_todo_item_count(todos);
    snprintf(result, rlen, "{\"count\":%d,\"todos\":%s}", count, todos);
    pthread_mutex_unlock(&g_cc_todo_lock);
    return true;
}

static bool tool_enter_plan_mode_compat(const char *input, char *result, size_t rlen) {
    (void)input;
    g_cc_plan_mode = true;
    snprintf(result, rlen,
             "Plan mode entered. DSCO records this as advisory state; continue by presenting a plan or asking specific questions.");
    return true;
}

static bool tool_exit_plan_mode_compat(const char *input, char *result, size_t rlen) {
    char *plan = json_get_str(input, "plan");
    g_cc_plan_mode = false;
    if (plan && plan[0]) {
        snprintf(result, rlen, "Plan submitted for user approval:\n%s", plan);
    } else {
        snprintf(result, rlen, "Plan mode exited.");
    }
    free(plan);
    return true;
}

/* ── AskUserQuestion: dynamic dialog engine ──────────────────────────────
 *
 * Renders a full interactive multi-question modal (see tui_ask_questions):
 *   - tab strip with per-question completion checkboxes
 *   - per-question option lists with descriptions
 *   - computed options (options_cmd runs a shell command at build time)
 *   - conditional branching (show_if gates a question on another's answer)
 *   - "Type something" free-text + "Chat about this" escape on every question
 *   - a review/submit screen
 *
 * Hybrid drive model: a single call carries a rich spec; passing the same
 * session_id on a later call REOPENS the session, preserving prior answers by
 * id and appending any new (model-generated) follow-up questions.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    char id[64];
    bool active;
    tui_ask_question_t q[TUI_ASK_MAX_QUESTIONS];
    int  n;
} ask_session_t;

static ask_session_t s_ask_sessions[8];

static ask_session_t *ask_session_find(const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < (int)(sizeof s_ask_sessions / sizeof s_ask_sessions[0]); i++)
        if (s_ask_sessions[i].active && strcmp(s_ask_sessions[i].id, id) == 0)
            return &s_ask_sessions[i];
    return NULL;
}

static ask_session_t *ask_session_alloc(const char *id) {
    int n = (int)(sizeof s_ask_sessions / sizeof s_ask_sessions[0]);
    for (int i = 0; i < n; i++)
        if (!s_ask_sessions[i].active) {
            memset(&s_ask_sessions[i], 0, sizeof(ask_session_t));
            s_ask_sessions[i].active = true;
            snprintf(s_ask_sessions[i].id, sizeof s_ask_sessions[i].id, "%s", id ? id : "");
            return &s_ask_sessions[i];
        }
    /* pool full — evict slot 0 */
    memset(&s_ask_sessions[0], 0, sizeof(ask_session_t));
    s_ask_sessions[0].active = true;
    snprintf(s_ask_sessions[0].id, sizeof s_ask_sessions[0].id, "%s", id ? id : "");
    return &s_ask_sessions[0];
}

/* options: array of objects {value?,label,description?} or bare strings */
static void ask_option_cb(const char *el, void *ctx) {
    tui_ask_question_t *q = (tui_ask_question_t *)ctx;
    if (!el || q->n_options >= TUI_ASK_MAX_OPTIONS) return;
    tui_ask_option_t *o = &q->options[q->n_options];
    while (*el && isspace((unsigned char)*el)) el++;
    if (*el == '"') {
        ctx_decode_json_string_token(el, o->label, sizeof o->label);
        if (!o->label[0]) return;
        snprintf(o->value, sizeof o->value, "%s", o->label);
        q->n_options++;
        return;
    }
    char *label = json_get_str(el, "label");
    char *value = json_get_str(el, "value");
    char *desc  = json_get_str(el, "description");
    if (label && label[0]) {
        snprintf(o->label, sizeof o->label, "%s", label);
        snprintf(o->value, sizeof o->value, "%s",
                 (value && value[0]) ? value : label);
        if (desc) snprintf(o->description, sizeof o->description, "%s", desc);
        q->n_options++;
    }
    free(label); free(value); free(desc);
}

/* show_if "in": [ "...", ... ] — bare string values */
static void ask_gateval_cb(const char *el, void *ctx) {
    tui_ask_question_t *q = (tui_ask_question_t *)ctx;
    if (!el || q->n_gate_vals >= TUI_ASK_MAX_GATEVALS) return;
    char buf[128];
    ctx_decode_json_string_token(el, buf, sizeof buf);
    if (buf[0]) snprintf(q->gate_vals[q->n_gate_vals++], 128, "%s", buf);
}

/* Fill options from the line-oriented output of a shell command. */
static void ask_fill_computed_options(tui_ask_question_t *q, const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[512];
    while (q->n_options < TUI_ASK_MAX_OPTIONS && fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        tui_ask_option_t *o = &q->options[q->n_options++];
        snprintf(o->value, sizeof o->value, "%s", line);
        snprintf(o->label, sizeof o->label, "%s", line);
    }
    pclose(fp);
}

typedef struct {
    ask_session_t *s;
    char gate_id[TUI_ASK_MAX_QUESTIONS][64];
} ask_parse_ctx_t;

static void ask_question_cb(const char *el, void *ctx) {
    ask_parse_ctx_t *pc = (ask_parse_ctx_t *)ctx;
    ask_session_t *s = pc->s;
    if (!el || s->n >= TUI_ASK_MAX_QUESTIONS) return;
    int idx = s->n;
    tui_ask_question_t *q = &s->q[idx];
    memset(q, 0, sizeof(*q));
    q->gate_q = -1;

    char *id       = json_get_str(el, "id");
    char *header   = json_get_str(el, "header");
    char *question = json_get_str(el, "question");
    q->multi_select = json_get_bool(el, "multiSelect",
                          json_get_bool(el, "multi_select", false));
    q->allow_custom = json_get_bool(el, "allow_custom",
                          json_get_bool(el, "allowCustom", true));
    q->allow_chat   = json_get_bool(el, "allow_chat",
                          json_get_bool(el, "allowChat", true));
    if (id)       snprintf(q->id, sizeof q->id, "%s", id);
    if (header)   snprintf(q->header, sizeof q->header, "%s", header);
    else if (id)  snprintf(q->header, sizeof q->header, "%s", id);
    if (question) snprintf(q->question, sizeof q->question, "%s", question);
    free(id); free(header); free(question);

    json_array_foreach(el, "options", ask_option_cb, q);

    char *ocmd = json_get_str(el, "options_cmd");
    if (!ocmd) ocmd = json_get_str(el, "optionsCmd");
    if (ocmd && ocmd[0]) ask_fill_computed_options(q, ocmd);
    free(ocmd);

    char *showif = json_get_raw(el, "show_if");
    if (!showif) showif = json_get_raw(el, "showIf");
    if (showif && showif[0] && showif[0] == '{') {
        char *gq = json_get_str(showif, "q");
        if (!gq) gq = json_get_str(showif, "question");
        if (gq) snprintf(pc->gate_id[idx], 64, "%s", gq);
        free(gq);
        char *eq = json_get_str(showif, "equals");
        if (eq && eq[0] && q->n_gate_vals < TUI_ASK_MAX_GATEVALS)
            snprintf(q->gate_vals[q->n_gate_vals++], 128, "%s", eq);
        free(eq);
        json_array_foreach(showif, "in", ask_gateval_cb, q);
    }
    free(showif);

    s->n++;
}

/* Copy prior answer state from an old question with the same id (reopen). */
static void ask_merge_prior(tui_ask_question_t *dst, const ask_session_t *old) {
    if (!dst->id[0]) return;
    for (int i = 0; i < old->n; i++) {
        if (strcmp(old->q[i].id, dst->id) != 0) continue;
        const tui_ask_question_t *src = &old->q[i];
        dst->answered = src->answered;
        snprintf(dst->custom, sizeof dst->custom, "%s", src->custom);
        /* re-map selections by option value so reordered options still match */
        for (int a = 0; a < dst->n_options; a++) {
            for (int b = 0; b < src->n_options; b++) {
                if (src->selected[b] &&
                    strcmp(src->options[b].value, dst->options[a].value) == 0) {
                    dst->selected[a] = true;
                    break;
                }
            }
        }
        return;
    }
}

bool dsco_run_ask_dialog(const char *input, char *result, size_t rlen) {
    if (!input) input = "{}";
    char *session_id = json_get_str(input, "session_id");
    if (!session_id) session_id = json_get_str(input, "sessionId");
    char *intro = json_get_str(input, "intro");

    /* Parse incoming spec into a temp session. */
    ask_session_t incoming;
    memset(&incoming, 0, sizeof incoming);
    ask_parse_ctx_t pc;
    memset(&pc, 0, sizeof pc);
    pc.s = &incoming;

    char *questions = json_get_raw(input, "questions");
    if (questions && questions[0] == '[') {
        json_array_foreach(input, "questions", ask_question_cb, &pc);
    }
    /* Fallback: a bare "question" string becomes one free-text question. */
    if (incoming.n == 0) {
        char *q1 = json_get_str(input, "question");
        tui_ask_question_t *q = &incoming.q[0];
        memset(q, 0, sizeof(*q));
        q->gate_q = -1;
        q->allow_custom = true;
        q->allow_chat = true;
        snprintf(q->id, sizeof q->id, "q1");
        snprintf(q->header, sizeof q->header, "Question");
        snprintf(q->question, sizeof q->question, "%s",
                 (q1 && q1[0]) ? q1 : "Please provide input");
        incoming.n = 1;
        free(q1);
    }

    /* Resolve branching gates (id → index). */
    for (int i = 0; i < incoming.n; i++) {
        if (!pc.gate_id[i][0]) continue;
        for (int j = 0; j < incoming.n; j++)
            if (incoming.q[j].id[0] && strcmp(incoming.q[j].id, pc.gate_id[i]) == 0) {
                incoming.q[i].gate_q = j;
                break;
            }
    }

    /* Reopen: merge prior answers by id, then persist merged spec. */
    ask_session_t *sess = ask_session_find(session_id);
    if (sess) {
        for (int i = 0; i < incoming.n; i++)
            ask_merge_prior(&incoming.q[i], sess);
    } else if (session_id && session_id[0]) {
        sess = ask_session_alloc(session_id);
    }

    /* Non-interactive: degrade gracefully — emit the spec so the model can
     * fall back to asking in plain text. */
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
        jbuf_t out; jbuf_init(&out, 256);
        jbuf_append(&out, "{\"status\":\"no_tty\",\"questions\":[");
        for (int i = 0; i < incoming.n; i++) {
            if (i) jbuf_append(&out, ",");
            jbuf_append(&out, "{\"id\":");
            jbuf_append_json_str(&out, incoming.q[i].id);
            jbuf_append(&out, ",\"question\":");
            jbuf_append_json_str(&out, incoming.q[i].question);
            jbuf_append(&out, "}");
        }
        jbuf_append(&out, "]}");
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
        if (sess) { memcpy(sess->q, incoming.q, sizeof incoming.q); sess->n = incoming.n; }
        free(session_id); free(intro); free(questions);
        return true;
    }

    char chat[1280] = "";
    tui_ask_status_t st = tui_ask_questions(incoming.q, incoming.n,
                                            intro, chat, sizeof chat);

    /* Persist merged + answered state back into the session for reopen. */
    if (sess) { memcpy(sess->q, incoming.q, sizeof incoming.q); sess->n = incoming.n; }

    const char *status_str = st == TUI_ASK_SUBMIT ? "submit" :
                             st == TUI_ASK_CHAT   ? "chat"   : "cancel";

    jbuf_t out; jbuf_init(&out, 512);
    jbuf_append(&out, "{\"status\":");
    jbuf_append_json_str(&out, status_str);
    if (session_id && session_id[0]) {
        jbuf_append(&out, ",\"session_id\":");
        jbuf_append_json_str(&out, session_id);
    }
    if (st == TUI_ASK_CHAT && chat[0]) {
        jbuf_append(&out, ",\"chat\":");
        jbuf_append_json_str(&out, chat);
    }
    jbuf_append(&out, ",\"answers\":[");
    bool first = true;
    char val[1280];
    for (int i = 0; i < incoming.n; i++) {
        if (!tui_ask_question_visible(incoming.q, incoming.n, i)) continue;
        tui_ask_question_t *q = &incoming.q[i];
        if (!first) jbuf_append(&out, ",");
        first = false;
        jbuf_append(&out, "{\"id\":");
        jbuf_append_json_str(&out, q->id);
        jbuf_append(&out, ",\"header\":");
        jbuf_append_json_str(&out, q->header);
        jbuf_append(&out, ",\"question\":");
        jbuf_append_json_str(&out, q->question);
        jbuf_append(&out, ",\"answered\":");
        jbuf_append(&out, q->answered ? "true" : "false");
        tui_ask_answer_value(q, val, sizeof val);
        jbuf_append(&out, ",\"value\":");
        jbuf_append_json_str(&out, val);
        if (q->custom[0]) {
            jbuf_append(&out, ",\"custom\":");
            jbuf_append_json_str(&out, q->custom);
        }
        jbuf_append(&out, ",\"selected\":[");
        bool sf = true;
        for (int o = 0; o < q->n_options; o++) {
            if (!q->selected[o]) continue;
            if (!sf) jbuf_append(&out, ",");
            sf = false;
            jbuf_append_json_str(&out, q->options[o].value[0]
                                       ? q->options[o].value
                                       : q->options[o].label);
        }
        jbuf_append(&out, "]}");
    }
    jbuf_append(&out, "]}");
    snprintf(result, rlen, "%s", out.data ? out.data : "{}");
    jbuf_free(&out);

    free(session_id); free(intro); free(questions);
    return true;
}

static __attribute__((unused)) bool tool_context_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (recall, search, get, batch_get, stats, summarize, pack, fuse, pin, gc)"); return false; }
    bool ok = false;
    if      (strcmp(action, "recall") == 0)    ok = tool_context_recall(input, result, rlen);
    else if (strcmp(action, "search") == 0)    ok = tool_context_search(input, result, rlen);
    else if (strcmp(action, "get") == 0)       ok = tool_context_get(input, result, rlen);
    else if (strcmp(action, "batch_get") == 0) ok = tool_context_get_batch(input, result, rlen);
    else if (strcmp(action, "stats") == 0)     ok = tool_context_stats(input, result, rlen);
    else if (strcmp(action, "summarize") == 0) ok = tool_context_summarize(input, result, rlen);
    else if (strcmp(action, "pack") == 0)      ok = tool_context_pack(input, result, rlen);
    else if (strcmp(action, "fuse") == 0)      ok = tool_context_fuse(input, result, rlen);
    else if (strcmp(action, "pin") == 0)       ok = tool_context_pin(input, result, rlen);
    else if (strcmp(action, "gc") == 0)        ok = tool_context_gc(input, result, rlen);
    else snprintf(result, rlen, "unknown context action: %s", action);
    free(action); return ok;
}

static bool tool_playbook_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (read, add, tag, remove, search, gc, inject)"); return false; }
    bool ok = false;
    if      (strcmp(action, "read") == 0)   ok = tool_playbook_read(input, result, rlen);
    else if (strcmp(action, "add") == 0)    ok = tool_playbook_add(input, result, rlen);
    else if (strcmp(action, "tag") == 0)    ok = tool_playbook_tag(input, result, rlen);
    else if (strcmp(action, "remove") == 0) ok = tool_playbook_remove(input, result, rlen);
    else if (strcmp(action, "search") == 0) ok = tool_playbook_search(input, result, rlen);
    else if (strcmp(action, "gc") == 0)     ok = tool_playbook_gc(input, result, rlen);
    else if (strcmp(action, "inject") == 0) ok = tool_playbook_inject(input, result, rlen);
    else snprintf(result, rlen, "unknown playbook action: %s", action);
    free(action); return ok;
}

static bool tool_browser_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (snapshot, extract, viewport, outline)"); return false; }
    bool ok = false;
    if      (strcmp(action, "snapshot") == 0) ok = tool_browser_snapshot(input, result, rlen);
    else if (strcmp(action, "extract") == 0)  ok = tool_browser_extract(input, result, rlen);
    else if (strcmp(action, "viewport") == 0) ok = tool_browser_viewport(input, result, rlen);
    else if (strcmp(action, "outline") == 0)  ok = tool_browser_outline(input, result, rlen);
    else snprintf(result, rlen, "unknown browser action: %s", action);
    free(action); return ok;
}

static bool tool_workflow_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (plan, status, checkpoint, resume, heartbeat, dead_letter, reprocess, validate, smoke)"); return false; }
    bool ok = false;
    if      (strcmp(action, "plan") == 0)       ok = tool_workflow_plan(input, result, rlen);
    else if (strcmp(action, "status") == 0)     ok = tool_workflow_status(input, result, rlen);
    else if (strcmp(action, "checkpoint") == 0) ok = tool_workflow_checkpoint(input, result, rlen);
    else if (strcmp(action, "resume") == 0)     ok = tool_workflow_resume(input, result, rlen);
    else if (strcmp(action, "heartbeat") == 0)  ok = tool_workflow_heartbeat(input, result, rlen);
    else if (strcmp(action, "dead_letter") == 0) ok = tool_workflow_dead_letter(input, result, rlen);
    else if (strcmp(action, "reprocess") == 0)  ok = tool_workflow_reprocess(input, result, rlen);
    else if (strcmp(action, "validate") == 0)   ok = tool_workflow_validate(input, result, rlen);
    else if (strcmp(action, "smoke") == 0)      ok = tool_workflow_smoke(input, result, rlen);
    else snprintf(result, rlen, "unknown workflow action: %s", action);
    free(action); return ok;
}

static bool tool_git_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (status, diff, log, commit, add, branch, stash, clone, push, pull)"); return false; }
    bool ok = false;
    if      (strcmp(action, "status") == 0) ok = tool_git_status(input, result, rlen);
    else if (strcmp(action, "diff") == 0)   ok = tool_git_diff(input, result, rlen);
    else if (strcmp(action, "log") == 0)    ok = tool_git_log(input, result, rlen);
    else if (strcmp(action, "commit") == 0) ok = tool_git_commit(input, result, rlen);
    else if (strcmp(action, "add") == 0)    ok = tool_git_add(input, result, rlen);
    else if (strcmp(action, "branch") == 0) ok = tool_git_branch(input, result, rlen);
    else if (strcmp(action, "stash") == 0)  ok = tool_git_stash(input, result, rlen);
    else if (strcmp(action, "clone") == 0)  ok = tool_git_clone(input, result, rlen);
    else if (strcmp(action, "push") == 0)   ok = tool_git_push(input, result, rlen);
    else if (strcmp(action, "pull") == 0)   ok = tool_git_pull(input, result, rlen);
    else snprintf(result, rlen, "unknown git action: %s", action);
    free(action); return ok;
}

static bool tool_ipc_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (send, recv, agents, scratch_put, scratch_get, task_submit, task_list, set_role)"); return false; }
    bool ok = false;
    if      (strcmp(action, "send") == 0)        ok = tool_ipc_send(input, result, rlen);
    else if (strcmp(action, "recv") == 0)        ok = tool_ipc_recv(input, result, rlen);
    else if (strcmp(action, "agents") == 0)      ok = tool_ipc_agents(input, result, rlen);
    else if (strcmp(action, "scratch_put") == 0) ok = tool_ipc_scratch_put(input, result, rlen);
    else if (strcmp(action, "scratch_get") == 0) ok = tool_ipc_scratch_get(input, result, rlen);
    else if (strcmp(action, "task_submit") == 0) ok = tool_ipc_task_submit(input, result, rlen);
    else if (strcmp(action, "task_list") == 0)   ok = tool_ipc_task_list(input, result, rlen);
    else if (strcmp(action, "set_role") == 0)    ok = tool_ipc_set_role(input, result, rlen);
    else snprintf(result, rlen, "unknown ipc action: %s", action);
    free(action); return ok;
}

static bool tool_agent_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (spawn, status, output, wait, race, kill)"); return false; }
    bool ok = false;
    if      (strcmp(action, "spawn") == 0)  ok = tool_spawn_agent(input, result, rlen);
    else if (strcmp(action, "status") == 0) ok = tool_agent_status(input, result, rlen);
    else if (strcmp(action, "output") == 0) ok = tool_agent_output(input, result, rlen);
    else if (strcmp(action, "wait") == 0)   ok = tool_agent_wait(input, result, rlen);
    else if (strcmp(action, "race") == 0)   ok = tool_agent_race(input, result, rlen);
    else if (strcmp(action, "kill") == 0)   ok = tool_agent_kill(input, result, rlen);
    else snprintf(result, rlen, "unknown agent action: %s", action);
    free(action); return ok;
}

static bool tool_swarm_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (create, map_reduce, status, collect, budget, spawn_executor, spawn_provider, create_executor_swarm, executor_status, topology_list, topology_run)"); return false; }
    bool ok = false;
    if      (strcmp(action, "create") == 0)               ok = tool_create_swarm(input, result, rlen);
    else if (strcmp(action, "map_reduce") == 0)           ok = tool_swarm_map_reduce(input, result, rlen);
    else if (strcmp(action, "status") == 0)               ok = tool_swarm_status(input, result, rlen);
    else if (strcmp(action, "collect") == 0)              ok = tool_swarm_collect(input, result, rlen);
    else if (strcmp(action, "budget") == 0)               ok = tool_swarm_budget(input, result, rlen);
    else if (strcmp(action, "spawn_executor") == 0)       ok = tool_spawn_executor(input, result, rlen);
    else if (strcmp(action, "spawn_provider") == 0)       ok = tool_spawn_provider(input, result, rlen);
    else if (strcmp(action, "create_executor_swarm") == 0) ok = tool_create_executor_swarm(input, result, rlen);
    else if (strcmp(action, "executor_status") == 0)      ok = tool_executor_status(input, result, rlen);
    else if (strcmp(action, "topology_list") == 0)        ok = tool_topology_list(input, result, rlen);
    else if (strcmp(action, "topology_run") == 0)         ok = tool_topology_run(input, result, rlen);
    else if (strcmp(action, "topology_solve") == 0)       ok = tool_topology_solve(input, result, rlen);
    else if (strcmp(action, "task_profile") == 0)         ok = tool_task_profile(input, result, rlen);
    else if (strcmp(action, "plan_analyze") == 0)         ok = tool_plan_analyze(input, result, rlen);
    else if (strcmp(action, "plan_cache_stats") == 0)     ok = tool_plan_cache_stats(input, result, rlen);
    else if (strcmp(action, "plan_cache_lookup") == 0)    ok = tool_plan_cache_lookup(input, result, rlen);
    else if (strcmp(action, "cost_model_stats") == 0)     ok = tool_cost_model_stats(input, result, rlen);
    else snprintf(result, rlen, "unknown swarm action: %s", action);
    free(action); return ok;
}

static bool tool_pheromone_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (deposit, sense, status)"); return false; }
    bool ok = false;
    if      (strcmp(action, "deposit") == 0) ok = tool_pheromone_deposit(input, result, rlen);
    else if (strcmp(action, "sense") == 0)   ok = tool_pheromone_sense(input, result, rlen);
    else if (strcmp(action, "status") == 0)  ok = tool_pheromone_status(input, result, rlen);
    else snprintf(result, rlen, "unknown pheromone action: %s", action);
    free(action); return ok;
}

static bool tool_ooda_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (begin, observe, orient, decide, complete, status)"); return false; }
    bool ok = false;
    if      (strcmp(action, "begin") == 0)    ok = tool_ooda_begin(input, result, rlen);
    else if (strcmp(action, "observe") == 0)  ok = tool_ooda_observe(input, result, rlen);
    else if (strcmp(action, "orient") == 0)   ok = tool_ooda_orient(input, result, rlen);
    else if (strcmp(action, "decide") == 0)   ok = tool_ooda_decide(input, result, rlen);
    else if (strcmp(action, "complete") == 0) ok = tool_ooda_complete(input, result, rlen);
    else if (strcmp(action, "status") == 0)   ok = tool_ooda_status(input, result, rlen);
    else snprintf(result, rlen, "unknown ooda action: %s", action);
    free(action); return ok;
}

static bool tool_killswitch_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (trigger, resolve, status)"); return false; }
    bool ok = false;
    if      (strcmp(action, "trigger") == 0) ok = tool_killswitch_trigger(input, result, rlen);
    else if (strcmp(action, "resolve") == 0) ok = tool_killswitch_resolve(input, result, rlen);
    else if (strcmp(action, "status") == 0)  ok = tool_killswitch_status(input, result, rlen);
    else snprintf(result, rlen, "unknown killswitch action: %s", action);
    free(action); return ok;
}

static bool tool_governance_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (status, authorize, checkpoint, budget, audit, param)"); return false; }
    bool ok = false;
    if      (strcmp(action, "status") == 0)     ok = tool_governance_status(input, result, rlen);
    else if (strcmp(action, "authorize") == 0)  ok = tool_governance_authorize(input, result, rlen);
    else if (strcmp(action, "checkpoint") == 0) ok = tool_governance_checkpoint(input, result, rlen);
    else if (strcmp(action, "budget") == 0)     ok = tool_governance_budget(input, result, rlen);
    else if (strcmp(action, "audit") == 0)      ok = tool_governance_audit(input, result, rlen);
    else if (strcmp(action, "param") == 0)      ok = tool_governance_param(input, result, rlen);
    else snprintf(result, rlen, "unknown governance action: %s", action);
    free(action); return ok;
}

static bool tool_memory_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (store, recall, promote, forget, status)"); return false; }
    bool ok = false;
    if      (strcmp(action, "store") == 0)   ok = tool_memory_store(input, result, rlen);
    else if (strcmp(action, "recall") == 0)  ok = tool_memory_recall(input, result, rlen);
    else if (strcmp(action, "promote") == 0) ok = tool_memory_promote(input, result, rlen);
    else if (strcmp(action, "forget") == 0)  ok = tool_memory_forget(input, result, rlen);
    else if (strcmp(action, "status") == 0)  ok = tool_memory_status(input, result, rlen);
    else snprintf(result, rlen, "unknown memory action: %s", action);
    free(action); return ok;
}

static bool tool_talons_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (goal, advance, tournament, recommend, status)"); return false; }
    bool ok = false;
    if      (strcmp(action, "goal") == 0)       ok = tool_talons_goal_create(input, result, rlen);
    else if (strcmp(action, "advance") == 0)    ok = tool_talons_goal_advance(input, result, rlen);
    else if (strcmp(action, "tournament") == 0) ok = tool_talons_tournament(input, result, rlen);
    else if (strcmp(action, "recommend") == 0)  ok = tool_talons_recommend(input, result, rlen);
    else if (strcmp(action, "status") == 0)     ok = tool_talons_status(input, result, rlen);
    else snprintf(result, rlen, "unknown talons action: %s", action);
    free(action); return ok;
}

static bool tool_legion_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (spawn, status, find)"); return false; }
    bool ok = false;
    if      (strcmp(action, "spawn") == 0)  ok = tool_legion_spawn(input, result, rlen);
    else if (strcmp(action, "status") == 0) ok = tool_legion_status(input, result, rlen);
    else if (strcmp(action, "find") == 0)   ok = tool_legion_find(input, result, rlen);
    else snprintf(result, rlen, "unknown legion action: %s", action);
    free(action); return ok;
}

static bool tool_kb_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (ingest, search, deep_search, list, get, delete, arxiv_search, arxiv_ingest)"); return false; }
    bool ok = false;
    if      (strcmp(action, "ingest") == 0)       ok = tool_kb_ingest(input, result, rlen);
    else if (strcmp(action, "search") == 0)       ok = tool_kb_search(input, result, rlen);
    else if (strcmp(action, "deep_search") == 0)  ok = tool_kb_deep_search(input, result, rlen);
    else if (strcmp(action, "list") == 0)         ok = tool_kb_list(input, result, rlen);
    else if (strcmp(action, "get") == 0)          ok = tool_kb_get(input, result, rlen);
    else if (strcmp(action, "delete") == 0)       ok = tool_kb_delete(input, result, rlen);
    else if (strcmp(action, "arxiv_search") == 0) ok = tool_arxiv_search(input, result, rlen);
    else if (strcmp(action, "arxiv_ingest") == 0) ok = tool_arxiv_ingest(input, result, rlen);
    else snprintf(result, rlen, "unknown kb action: %s", action);
    free(action); return ok;
}

static bool tool_network_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (dns, ping, port_check, port_scan, netstat, cert, traceroute, whois, interfaces, websocket)"); return false; }
    bool ok = false;
    if      (strcmp(action, "dns") == 0)        ok = tool_dns_lookup(input, result, rlen);
    else if (strcmp(action, "ping") == 0)       ok = tool_ping(input, result, rlen);
    else if (strcmp(action, "port_check") == 0) ok = tool_port_check(input, result, rlen);
    else if (strcmp(action, "port_scan") == 0)  ok = tool_port_scan(input, result, rlen);
    else if (strcmp(action, "netstat") == 0)    ok = tool_netstat(input, result, rlen);
    else if (strcmp(action, "cert") == 0)       ok = tool_cert_info(input, result, rlen);
    else if (strcmp(action, "traceroute") == 0) ok = tool_traceroute(input, result, rlen);
    else if (strcmp(action, "whois") == 0)      ok = tool_whois(input, result, rlen);
    else if (strcmp(action, "interfaces") == 0) ok = tool_net_interfaces(input, result, rlen);
    else if (strcmp(action, "websocket") == 0)  { snprintf(result, rlen, "websocket test: use curl_raw"); ok = true; }
    else snprintf(result, rlen, "unknown network action: %s", action);
    free(action); return ok;
}

static bool tool_prediction_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (scan, weather, snapshot, arb, semantic_match, cross_delta, movers, cache_refresh, cache_query, historical)"); return false; }
    bool ok = false;
    if      (strcmp(action, "scan") == 0)           ok = tool_prediction_scan(input, result, rlen);
    else if (strcmp(action, "weather") == 0)        ok = tool_prediction_weather(input, result, rlen);
    else if (strcmp(action, "snapshot") == 0)       ok = tool_prediction_snapshot(input, result, rlen);
    else if (strcmp(action, "arb") == 0)            ok = tool_prediction_arb(input, result, rlen);
    else if (strcmp(action, "semantic_match") == 0) ok = tool_prediction_semantic_match(input, result, rlen);
    else if (strcmp(action, "cross_delta") == 0)    ok = tool_cross_platform_delta(input, result, rlen);
    else if (strcmp(action, "movers") == 0)         ok = tool_market_movers(input, result, rlen);
    else if (strcmp(action, "cache_refresh") == 0)  ok = tool_market_cache_refresh(input, result, rlen);
    else if (strcmp(action, "cache_query") == 0)    ok = tool_market_cache_query(input, result, rlen);
    else if (strcmp(action, "historical") == 0)     ok = tool_historical_cross_platform(input, result, rlen);
    else snprintf(result, rlen, "unknown prediction action: %s", action);
    free(action); return ok;
}

static bool tool_strategy_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (completeness, binary_fade, stale_snipe, kelly, spread_scan)"); return false; }
    bool ok = false;
    if      (strcmp(action, "completeness") == 0) ok = tool_strat_completeness(input, result, rlen);
    else if (strcmp(action, "binary_fade") == 0)  ok = tool_strat_binary_fade(input, result, rlen);
    else if (strcmp(action, "stale_snipe") == 0)  ok = tool_strat_stale_snipe(input, result, rlen);
    else if (strcmp(action, "kelly") == 0)        ok = tool_strat_kelly(input, result, rlen);
    else if (strcmp(action, "spread_scan") == 0)  ok = tool_strat_spread_scan(input, result, rlen);
    else snprintf(result, rlen, "unknown strategy action: %s", action);
    free(action); return ok;
}

static bool tool_systematic_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (ingest_polymarket, ingest_kalshi, analytics, signals)"); return false; }
    bool ok = false;
    if      (strcmp(action, "ingest_polymarket") == 0) ok = tool_systematic_ingest_polymarket(input, result, rlen);
    else if (strcmp(action, "ingest_kalshi") == 0)     ok = tool_systematic_ingest_kalshi(input, result, rlen);
    else if (strcmp(action, "analytics") == 0)         ok = tool_systematic_analytics(input, result, rlen);
    else if (strcmp(action, "signals") == 0)           ok = tool_systematic_signals(input, result, rlen);
    else snprintf(result, rlen, "unknown systematic action: %s", action);
    free(action); return ok;
}

static bool tool_trading_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (arb_execute, arb_monitor, portfolio, risk_check, risk_configure)"); return false; }
    bool ok = false;
    if      (strcmp(action, "arb_execute") == 0)    ok = tool_arb_execute(input, result, rlen);
    else if (strcmp(action, "arb_monitor") == 0)    ok = tool_arb_monitor(input, result, rlen);
    else if (strcmp(action, "portfolio") == 0)      ok = tool_portfolio_cross(input, result, rlen);
    else if (strcmp(action, "risk_check") == 0)     ok = tool_risk_check(input, result, rlen);
    else if (strcmp(action, "risk_configure") == 0) ok = tool_risk_configure(input, result, rlen);
    else snprintf(result, rlen, "unknown trading action: %s", action);
    free(action); return ok;
}

typedef struct {
    const char *id;
    const char *aliases;
    const char *name;
    const char *layer;
    const char *standard_type;
    const char *transport;
    const char *scope;
    const char *payment_rails;
    const char *state;
    const char *next_adapter;
    const char *source_url;
    const char *notes;
    bool open_standard;
    bool live_adapter;
} agentic_commerce_protocol_t;

static const agentic_commerce_protocol_t s_agentic_commerce_protocols[] = {
    {
        "acp", "agentic_commerce_protocol,agentic-commerce-protocol",
        "Agentic Commerce Protocol", "checkout", "open_standard",
        "REST/MCP", "buyer-agent-business checkout and payment credential sharing",
        "PSP-neutral payment credential handoff; Stripe Shared Payment Tokens",
        "registry_ready_live_adapter_pending",
        "checkout session adapter, payment token exchange, order approval webhook",
        "https://www.agenticcommerce.dev/",
        "OpenAI and Stripe protocol for agent-led checkout flows.",
        true, false
    },
    {
        "ucp", "universal_commerce_protocol,universal-commerce-protocol",
        "Universal Commerce Protocol", "commerce_lifecycle", "open_standard",
        "REST/JSON-RPC with MCP/A2A/AP2 support",
        "catalog search, cart building, identity linking, checkout, order management",
        "AP2 mandates plus merchant PSP rails",
        "registry_ready_live_adapter_pending",
        "catalog/cart/order client, OAuth identity linking, checkout/order webhooks",
        "https://ucp.dev/",
        "Google and ecosystem protocol spanning discovery through post-purchase.",
        true, false
    },
    {
        "ap2", "agent_payments_protocol,agent-payments-protocol",
        "Agent Payments Protocol", "authorization_payment", "open_standard",
        "A2A/UCP extension with MCP tooling",
        "verifiable user intent for human-present and human-not-present purchases",
        "cards, x402, wallets, bank rails, digital currency",
        "registry_ready_live_adapter_pending",
        "Checkout Mandate and Payment Mandate signer/verifier with durable audit log",
        "https://ap2-protocol.org/",
        "Mandate layer for authorization, authenticity, and accountability.",
        true, false
    },
    {
        "x402", "http_402,payment_required",
        "x402 Payment Required", "http_payment", "open_standard",
        "HTTP 402 with PAYMENT-* headers and facilitator verify/settle APIs",
        "pay-per-resource API access and machine-to-machine payments",
        "stablecoins/onchain settlement with extensible schemes",
        "registry_ready_live_adapter_pending",
        "402 challenge parser, PAYMENT-SIGNATURE builder, /verify and /settle client",
        "https://www.x402.org/",
        "HTTP-native payment negotiation for agents and services.",
        true, false
    },
    {
        "mpp", "machine_payment_protocol,stripe_mpp",
        "Stripe Machine Payment Protocol", "machine_payment", "vendor_protocol",
        "Stripe Agentic Commerce APIs",
        "machine and agent resource payments",
        "Stripe payment methods, Link, and shared payment tokens",
        "registry_ready_live_adapter_pending",
        "Stripe test-mode client, scoped credentials, idempotent payment execution",
        "https://docs.stripe.com/agentic-commerce",
        "Stripe rail for machine payments alongside x402 support.",
        false, false
    },
    {
        "stripe_spt", "shared_payment_tokens,stripe_shared_payment_tokens,link",
        "Stripe Shared Payment Tokens", "credential_token", "vendor_protocol",
        "Stripe token APIs",
        "delegated payment credentials for agent checkout",
        "cards, wallets, Link, and Stripe-managed credentials",
        "registry_ready_live_adapter_pending",
        "token vault, consent scope model, checkout credential exchange",
        "https://docs.stripe.com/agentic-commerce",
        "Credential bridge used by Stripe's agentic commerce suite.",
        false, false
    },
    {
        "visa_tap", "visa_ic,visa_intelligent_commerce,trusted_agent_protocol",
        "Visa Intelligent Commerce / Trusted Agent Protocol", "network_trust",
        "network_protocol",
        "network tokenization plus trusted-agent attestation",
        "agent identity, merchant bot trust, tokenized Visa credentials, user controls",
        "Visa network tokens",
        "watchlist_partner_adapter_pending",
        "partner sandbox adapter once public protocol docs and credentials are available",
        "https://www.axios.com/2025/10/14/visa-ai-shopping-agent-protocol-bot",
        "Network-led trust and tokenization layer for agent-originated card payments.",
        false, false
    },
    {
        "mastercard_agent_pay", "agent_pay,mastercard_agent_suite",
        "Mastercard Agent Pay", "network_trust", "network_protocol",
        "Mastercard tokenization, authentication, and Agent Pay APIs",
        "authenticated agentic card transactions and merchant trust",
        "Mastercard network tokens and authentication rails",
        "watchlist_partner_adapter_pending",
        "partner sandbox adapter once API contracts and regulatory scope are available",
        "https://www.axios.com/2026/01/20/mastercard-ai-checkout-agentic-commerce",
        "Network-led agentic payment infrastructure; public SDK surface is still emerging.",
        false, false
    },
    {
        "rails", "real_time_agent_integrity_ledger_settlement,agentic_clearing",
        "RAILS Clearing Protocol", "clearing_verification", "research_protocol",
        "verification and clearing objects with rail-agnostic settlement adapters",
        "obligation evidence, verification mesh, clearing decision, settlement instruction",
        "rail-agnostic; consumes x402, card, wallet, and escrow settlement adapters",
        "research_watchlist_adapter_pending",
        "obligation object, evidence envelope, clearing decision, settlement adapter hooks",
        "https://arxiv.org/abs/2606.08790",
        "Emerging clearing layer for whether delegated work satisfied the obligation.",
        true, false
    },
    {0}
};

static bool commerce_token_eq(const char *a, const char *b, size_t b_len) {
    if (!a || !b || strlen(a) != b_len) return false;
    for (size_t i = 0; i < b_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool commerce_alias_matches(const char *aliases, const char *probe) {
    if (!aliases || !probe || !probe[0]) return false;
    const char *p = aliases;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        if (commerce_token_eq(probe, start, (size_t)(p - start)))
            return true;
    }
    return false;
}

static const agentic_commerce_protocol_t *commerce_find_protocol(const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; s_agentic_commerce_protocols[i].id; i++) {
        const agentic_commerce_protocol_t *p = &s_agentic_commerce_protocols[i];
        if (commerce_token_eq(id, p->id, strlen(p->id)) ||
            commerce_alias_matches(p->aliases, id))
            return p;
    }
    return NULL;
}

static void commerce_json_field(jbuf_t *b, const char *key, const char *value) {
    jbuf_append(b, "\"");
    jbuf_append(b, key);
    jbuf_append(b, "\":");
    jbuf_append_json_str(b, value ? value : "");
}

static void commerce_append_protocol_json(jbuf_t *b,
                                           const agentic_commerce_protocol_t *p) {
    jbuf_append(b, "{");
    commerce_json_field(b, "id", p->id);
    jbuf_append(b, ",");
    commerce_json_field(b, "name", p->name);
    jbuf_append(b, ",");
    commerce_json_field(b, "layer", p->layer);
    jbuf_append(b, ",");
    commerce_json_field(b, "standard_type", p->standard_type);
    jbuf_append(b, ",");
    commerce_json_field(b, "transport", p->transport);
    jbuf_append(b, ",");
    commerce_json_field(b, "scope", p->scope);
    jbuf_append(b, ",");
    commerce_json_field(b, "payment_rails", p->payment_rails);
    jbuf_append(b, ",");
    commerce_json_field(b, "state", p->state);
    jbuf_append(b, ",");
    commerce_json_field(b, "next_adapter", p->next_adapter);
    jbuf_append(b, ",");
    commerce_json_field(b, "source_url", p->source_url);
    jbuf_append(b, ",");
    commerce_json_field(b, "notes", p->notes);
    jbuf_append(b, ",\"open_standard\":");
    jbuf_append(b, p->open_standard ? "true" : "false");
    jbuf_append(b, ",\"live_adapter\":");
    jbuf_append(b, p->live_adapter ? "true" : "false");
    jbuf_append(b, "}");
}

static void commerce_append_step(jbuf_t *b, int *count, const char *step) {
    if ((*count)++ > 0) jbuf_append(b, ",");
    jbuf_append_json_str(b, step);
}

static void commerce_append_plan_steps(jbuf_t *b,
                                       const agentic_commerce_protocol_t *p) {
    int n = 0;
    jbuf_append(b, "[");
    if (strcmp(p->id, "acp") == 0) {
        commerce_append_step(b, &n, "Model merchant checkout sessions and itemized order state.");
        commerce_append_step(b, &n, "Implement payment credential exchange using scoped shared payment tokens.");
        commerce_append_step(b, &n, "Add idempotent order approval hooks with timeout and retry telemetry.");
    } else if (strcmp(p->id, "ucp") == 0) {
        commerce_append_step(b, &n, "Add catalog search and lookup actions with machine-readable offers.");
        commerce_append_step(b, &n, "Implement cart mutation, OAuth identity linking, checkout, and order webhooks.");
        commerce_append_step(b, &n, "Route UCP payment authorization through AP2 mandate storage.");
    } else if (strcmp(p->id, "ap2") == 0) {
        commerce_append_step(b, &n, "Persist Checkout Mandate and Payment Mandate objects with signatures.");
        commerce_append_step(b, &n, "Verify mandate authenticity before payment or order placement.");
        commerce_append_step(b, &n, "Attach mandate IDs to tool audit records for accountability.");
    } else if (strcmp(p->id, "x402") == 0) {
        commerce_append_step(b, &n, "Parse 402 PAYMENT-REQUIRED challenges and advertised schemes.");
        commerce_append_step(b, &n, "Create PAYMENT-SIGNATURE headers for exact-payment requests.");
        commerce_append_step(b, &n, "Call facilitator /verify and /settle endpoints before releasing tool output.");
    } else if (strcmp(p->id, "mpp") == 0) {
        commerce_append_step(b, &n, "Gate Stripe Machine Payment Protocol calls behind explicit test/live credentials.");
        commerce_append_step(b, &n, "Map machine resource pricing into payment intents with idempotency keys.");
        commerce_append_step(b, &n, "Record payment, refund, and failure states in the workflow ledger.");
    } else if (strcmp(p->id, "stripe_spt") == 0) {
        commerce_append_step(b, &n, "Create a scoped token vault for delegated payment credentials.");
        commerce_append_step(b, &n, "Bind token use to merchant, amount, category, expiry, and user approval policy.");
        commerce_append_step(b, &n, "Expose checkout credential handoff to ACP and Stripe MPP adapters.");
    } else if (strcmp(p->id, "visa_tap") == 0) {
        commerce_append_step(b, &n, "Wait for public/partner Trusted Agent Protocol contracts before live execution.");
        commerce_append_step(b, &n, "Represent trusted-agent attestation, merchant allowlists, spend controls, and token IDs.");
        commerce_append_step(b, &n, "Add network-token audit records once a Visa sandbox is available.");
    } else if (strcmp(p->id, "mastercard_agent_pay") == 0) {
        commerce_append_step(b, &n, "Wait for Agent Pay API/sandbox contracts before live execution.");
        commerce_append_step(b, &n, "Represent authenticated agent credentials and merchant trust assertions.");
        commerce_append_step(b, &n, "Map approval, authentication, and token lifecycle events into the audit ledger.");
    } else if (strcmp(p->id, "rails") == 0) {
        commerce_append_step(b, &n, "Model Obligation Object and Evidence Envelope records.");
        commerce_append_step(b, &n, "Add verification policy hooks that produce a Clearing Decision.");
        commerce_append_step(b, &n, "Route Settlement Instructions to x402, card, wallet, or escrow adapters.");
    } else {
        commerce_append_step(b, &n, p->next_adapter);
    }
    jbuf_append(b, "]");
}

static bool tool_agentic_commerce(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
                 "missing: action (list, status, coverage, plan)");
        return false;
    }

    if (strcmp(action, "list") == 0) {
        jbuf_t out;
        jbuf_init(&out, 8192);
        int count = 0;
        jbuf_append(&out, "{\"ok\":true,\"protocol_count\":");
        for (int i = 0; s_agentic_commerce_protocols[i].id; i++) count++;
        jbuf_append_int(&out, count);
        jbuf_append(&out, ",\"protocols\":[");
        for (int i = 0; s_agentic_commerce_protocols[i].id; i++) {
            if (i > 0) jbuf_append(&out, ",");
            commerce_append_protocol_json(&out, &s_agentic_commerce_protocols[i]);
        }
        jbuf_append(&out, "]}");
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
        free(action);
        return true;
    }

    if (strcmp(action, "coverage") == 0) {
        int total = 0, open = 0, live = 0, watch = 0;
        for (int i = 0; s_agentic_commerce_protocols[i].id; i++) {
            const agentic_commerce_protocol_t *p = &s_agentic_commerce_protocols[i];
            total++;
            if (p->open_standard) open++;
            if (p->live_adapter) live++;
            if (strstr(p->state, "watchlist")) watch++;
        }

        jbuf_t out;
        jbuf_init(&out, 4096);
        jbuf_append(&out, "{\"ok\":true,\"registry_protocol_count\":");
        jbuf_append_int(&out, total);
        jbuf_append(&out, ",\"open_standard_count\":");
        jbuf_append_int(&out, open);
        jbuf_append(&out, ",\"live_adapter_count\":");
        jbuf_append_int(&out, live);
        jbuf_append(&out, ",\"watchlist_count\":");
        jbuf_append_int(&out, watch);
        jbuf_append(&out, ",\"missing_live_adapters\":[");
        int missing = 0;
        for (int i = 0; s_agentic_commerce_protocols[i].id; i++) {
            const agentic_commerce_protocol_t *p = &s_agentic_commerce_protocols[i];
            if (p->live_adapter) continue;
            if (missing++ > 0) jbuf_append(&out, ",");
            jbuf_append_json_str(&out, p->id);
        }
        jbuf_append(&out, "],\"next_priority\":[\"x402\",\"ap2\",\"acp\",\"ucp\"],");
        commerce_json_field(&out, "summary",
                            "protocol registry is integrated; live commerce adapters are pending");
        jbuf_append(&out, "}");
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
        free(action);
        return true;
    }

    if (strcmp(action, "status") == 0 || strcmp(action, "plan") == 0) {
        char *protocol = json_get_str(input, "protocol");
        if (!protocol || !protocol[0]) {
            free(protocol);
            free(action);
            snprintf(result, rlen, "missing: protocol");
            return false;
        }
        const agentic_commerce_protocol_t *p = commerce_find_protocol(protocol);
        if (!p) {
            snprintf(result, rlen, "unknown agentic commerce protocol: %s", protocol);
            free(protocol);
            free(action);
            return false;
        }

        jbuf_t out;
        jbuf_init(&out, 4096);
        jbuf_append(&out, "{\"ok\":true,\"protocol\":");
        commerce_append_protocol_json(&out, p);
        if (strcmp(action, "plan") == 0) {
            jbuf_append(&out, ",\"implementation_plan\":");
            commerce_append_plan_steps(&out, p);
        }
        jbuf_append(&out, "}");
        snprintf(result, rlen, "%s", out.data ? out.data : "{}");
        jbuf_free(&out);
        free(protocol);
        free(action);
        return true;
    }

    snprintf(result, rlen, "unknown agentic_commerce action: %s", action);
    free(action);
    return false;
}

/* ── Unified schema constants for consolidated tools ── */

#define S_ACTION "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Action to perform\"}},\"required\":[\"action\"]}"

static const tool_def_t s_tools[] = {
    /* ══════════════════════════════════════════════════════════════════════
     *  CLAUDE CODE COMPATIBILITY ALIASES
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "Read", .description = "Claude-compatible alias for read_file.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"file_path\"]}", .execute = tool_read_file, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "Write", .description = "Claude-compatible alias for verified atomic write_file.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"content\"]}", .execute = tool_write_file, .core = true },
    { .name = "Edit", .description = "Claude-compatible alias for edit_file.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}", .execute = tool_edit_file, .core = true },
    { .name = "Bash", .description = "Claude-compatible shell runner. Use Write/write_file for durable artifacts; declare verify_path/verify_paths when shell creates files.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\"},\"description\":{\"type\":\"string\"},\"run_in_background\":{\"type\":\"boolean\"},\"cwd\":{\"type\":\"string\"},\"verify_path\":{\"type\":\"string\"},\"artifact_path\":{\"type\":\"string\"},\"output_path\":{\"type\":\"string\"},\"verify_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"verify_min_bytes\":{\"type\":\"integer\"},\"verify_contains\":{\"type\":\"string\"},\"verify_sha256\":{\"type\":\"string\"}},\"required\":[\"command\"]}", .execute = tool_bash_compat, .core = true },
    { .name = "Glob", .description = "Claude-compatible file glob search.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}", .execute = tool_find_files, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "Grep", .description = "Claude-compatible content search with glob/output_mode/head_limit support.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"include\":{\"type\":\"string\"},\"output_mode\":{\"type\":\"string\"},\"head_limit\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}", .execute = tool_grep, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "WebFetch", .description = "Claude-compatible URL fetch/extract.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"},\"max_chars\":{\"type\":\"integer\"}},\"required\":[\"url\"]}", .execute = tool_web_extract, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "WebSearch", .description = "Claude-compatible web search alias.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"num\":{\"type\":\"integer\"}},\"required\":[\"query\"]}", .execute = tool_parallel_search, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "Agent", .description = "Claude-compatible sub-agent task alias.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"task\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"}},\"required\":[\"prompt\"]}", .execute = tool_agent_compat },
    { .name = "Task", .description = "Claude-compatible task agent alias.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"task\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"}},\"required\":[\"prompt\"]}", .execute = tool_agent_compat },
    { .name = "TodoWrite", .description = "Claude-compatible todo list state writer.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"todos\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"todos\"]}", .execute = tool_todo_write_compat },
    { .name = "TaskList", .description = "Return the Claude-compatible todo list state.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_task_list_compat, .is_read_only = true, .is_concurrent = true },
    { .name = "EnterPlanMode", .description = "Enter Claude-compatible advisory plan mode.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_enter_plan_mode_compat },
    { .name = "ExitPlanMode", .description = "Exit Claude-compatible advisory plan mode.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"plan\":{\"type\":\"string\"}}}", .execute = tool_exit_plan_mode_compat },
    { .name = "AskUserQuestion", .description = "Show an interactive multi-question dialog to collect structured input from the user. Use when a response merits clarification. Supports option lists with descriptions, conditional branching (show_if), computed options (options_cmd), free-text + 'chat about this' escape hatches, and reopen-by-id to append follow-up questions while preserving prior answers. Returns {status:submit|cancel|chat|no_tty, answers:[{id,header,value,selected[],custom,answered}]}.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"session_id\":{\"type\":\"string\",\"description\":\"Reuse to reopen a prior dialog and append follow-ups; prior answers persist by question id.\"},\"intro\":{\"type\":\"string\"},\"question\":{\"type\":\"string\",\"description\":\"Shorthand for a single free-text question.\"},\"questions\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"header\":{\"type\":\"string\",\"description\":\"Short tab label.\"},\"question\":{\"type\":\"string\"},\"multiSelect\":{\"type\":\"boolean\"},\"allow_custom\":{\"type\":\"boolean\",\"description\":\"Offer 'Type something' free-text (default true).\"},\"allow_chat\":{\"type\":\"boolean\",\"description\":\"Offer 'Chat about this' deferral (default true).\"},\"options\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"}}}},\"options_cmd\":{\"type\":\"string\",\"description\":\"Shell command whose output lines become options.\"},\"show_if\":{\"type\":\"object\",\"description\":\"Branch: show only if question {q} answered {equals} or value in {in:[...]}.\",\"properties\":{\"q\":{\"type\":\"string\"},\"equals\":{\"type\":\"string\"},\"in\":{\"type\":\"array\"}}}},\"required\":[\"question\"]}}}}", .execute = dsco_run_ask_dialog },

    /* ══════════════════════════════════════════════════════════════════════
     *  CORE FILE TOOLS (13)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "write_file", .description = "Create/overwrite a file atomically, fsync it, and verify bytes on disk. Creates parent dirs.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}", .execute = tool_write_file, .core = true },
    { .name = "read_file", .description = "Read file with line numbers. Use offset/limit for large files.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"description\":\"Start line (1-based)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines\"}},\"required\":[\"path\"]}", .execute = tool_read_file, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "page_file", .description = "Page through a large file.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"page\":{\"type\":\"integer\"},\"page_size\":{\"type\":\"integer\"}},\"required\":[\"path\"]}", .execute = tool_page_file, .is_read_only = true, .is_concurrent = true },
    { .name = "edit_file", .description = "Edit file by replacing old_string with new_string.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}", .execute = tool_edit_file, .core = true },
    { .name = "append_file", .description = "Append content, fsync it, and verify appended bytes on disk.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}", .execute = tool_append_file },
    { .name = "list_directory", .description = "List directory contents with file info.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}", .execute = tool_list_dir, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "find_files", .description = "Find files by name pattern (glob).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}", .execute = tool_find_files, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "grep_files", .description = "Search file contents with regex.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"include\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}", .execute = tool_grep, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "file_info", .description = "Get file metadata (size, permissions, timestamps).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", .execute = tool_file_info, .is_read_only = true, .is_concurrent = true },
    { .name = "move_file", .description = "Move or rename a file/directory.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"dest\":{\"type\":\"string\"}},\"required\":[\"source\",\"dest\"]}", .execute = tool_move_file },
    { .name = "copy_file", .description = "Copy a file or directory.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"dest\":{\"type\":\"string\"}},\"required\":[\"source\",\"dest\"]}", .execute = tool_copy_file },
    { .name = "delete_file", .description = "Delete a file or empty directory.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", .execute = tool_delete_file },
    { .name = "mkdir", .description = "Create directory (with parents).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", .execute = tool_mkdir },

    /* ══════════════════════════════════════════════════════════════════════
     *  EXECUTION (3)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "compile", .description = "Compile source code.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Build command\"}}}", .execute = tool_compile },
    { .name = "bash", .description = "Run a shell command. Use write_file/append_file for durable artifacts; declare verify_path/verify_paths when shell creates files.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds\"},\"cwd\":{\"type\":\"string\"},\"verify_path\":{\"type\":\"string\"},\"artifact_path\":{\"type\":\"string\"},\"output_path\":{\"type\":\"string\"},\"verify_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"verify_min_bytes\":{\"type\":\"integer\"},\"verify_contains\":{\"type\":\"string\"},\"verify_sha256\":{\"type\":\"string\"}},\"required\":[\"command\"]}", .execute = tool_bash, .core = true },
    { .name = "python", .description = "Run Python code.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"args\":{\"type\":\"string\"}},\"required\":[\"code\"]}", .execute = tool_python, .core = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  CONTEXT & RETRIEVAL (4)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "context_recall", .description = "Retrieve persisted tool results. No args = list available keys.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Result key (e.g. python:a3f2...) from [key=...] footer\"},\"list\":{\"type\":\"boolean\",\"description\":\"List all available keys\"}}}", .execute = tool_context_recall, .is_read_only = true, .is_concurrent = true },
    { .name = "token_audit", .description = "Audit token usage across conversation.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_token_audit, .is_read_only = true, .is_concurrent = true },
    { .name = "context_status", .description = "Context window self-awareness: tokens, schema overhead, recommendations.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_context_status, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "context_compact", .description = "Compress old conversation history to reclaim tokens.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"aggressive\":{\"type\":\"boolean\"}}}", .execute = tool_context_compact, .core = true },
    { .name = "plot", .description = "Render data as a Unicode chart (returns ANSI/Unicode art). Types: line, bar, column, area, scatter, hist, heatmap, box, candlestick, gauge, sparkline, pie, waterfall, bullet, lollipop, slope, ecdf, calendar, ridgeline, violin, bignum. Uses subpixel Braille (2x4 dots/cell) for line/scatter/area/ridgeline, eighth-block bars, and 256-color heatmaps/calendars. Inline-printable and usable as a display artifact.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"description\":\"line|bar|column|area|scatter|hist|heatmap|box|candlestick|gauge|sparkline|pie|waterfall|bullet|lollipop|slope|ecdf|calendar|ridgeline|violin|bignum\"},\"series\":{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"description\":\"ridgeline: array of series\"},\"left\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"right\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"ranges\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"target\":{\"type\":\"number\"},\"data\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"description\":\"primary series\"},\"x\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"open\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"high\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"low\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"close\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"title\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},\"bins\":{\"type\":\"integer\"},\"rows\":{\"type\":\"integer\"},\"cols\":{\"type\":\"integer\"},\"value\":{\"type\":\"number\"},\"min\":{\"type\":\"number\"},\"max\":{\"type\":\"number\"},\"color\":{\"type\":\"boolean\"},\"axes\":{\"type\":\"boolean\"}},\"required\":[\"type\",\"data\"]}", .execute = tool_plot, .is_read_only = true, .is_concurrent = true },
    { .name = "pets", .description = "Companion sprites for background agents. action=roster shows live background-agent pets (face, status, cost, activity sparkline); gallery shows a species sampler; roll shows a single deterministic pet for a seed string. Each agent deterministically hatches the same pet from its id/task.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"roster|gallery|roll\"},\"seed\":{\"type\":\"string\",\"description\":\"seed for action=roll\"}}}", .execute = tool_pets, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  BROWSER & PERCEPTION (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "browser", .description = "Browser operations: snapshot, extract, viewport, outline.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"snapshot|extract|viewport|outline\"},\"url\":{\"type\":\"string\"},\"selector\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_browser_dispatch, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  WORKFLOW & RESEARCH (3)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "workflow", .description = "Workflow management: plan, status, checkpoint, resume, heartbeat, dead-letter, reprocess, validate, smoke.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"plan|status|checkpoint|resume|heartbeat|dead_letter|reprocess|validate|smoke\"},\"name\":{\"type\":\"string\"},\"steps\":{\"type\":\"string\"},\"id\":{\"type\":\"integer\"},\"step\":{\"type\":\"integer\"},\"status\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"},\"business_key\":{\"type\":\"string\"},\"contract_version\":{\"type\":\"string\"},\"max_retries\":{\"type\":\"integer\"},\"root_cause\":{\"type\":\"string\"},\"non_retryable\":{\"type\":\"boolean\"}},\"required\":[\"action\"]}", .execute = tool_workflow_dispatch },
    { .name = "research_probe", .description = "Deep research probe on a topic.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}", .execute = tool_research_probe, .is_read_only = true, .is_concurrent = true },
    { .name = "code_search", .description = "Search codebase by symbol or pattern.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"query\"]}", .execute = tool_code_search, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  GIT (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "git", .description = "Git operations: status, diff, log, commit, add, branch, stash, clone, push, pull.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"status|diff|log|commit|add|branch|stash|clone|push|pull\"},\"args\":{\"type\":\"string\",\"description\":\"Arguments (files, message, etc)\"},\"message\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_git_dispatch, .core = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  SYSTEM & PROCESS (4)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "ps", .description = "List running processes.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_ps, .is_read_only = true, .is_concurrent = true },
    { .name = "env_get", .description = "Get environment variable.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}", .execute = tool_env_get, .is_read_only = true, .is_concurrent = true },
    { .name = "sysinfo", .description = "System info: CPU, memory, OS.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_sysinfo, .is_read_only = true, .is_concurrent = true },
    { .name = "disk_usage", .description = "Disk usage for a path.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", .execute = tool_disk_usage, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  TEXT & DATA PROCESSING (2)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "jq", .description = "Process JSON with jq expressions.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"}},\"required\":[\"filter\"]}", .execute = tool_jq, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "diff", .description = "Compare two files or strings.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"file1\":{\"type\":\"string\"},\"file2\":{\"type\":\"string\"},\"text1\":{\"type\":\"string\"},\"text2\":{\"type\":\"string\"}}}", .execute = tool_diff, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  NETWORK & HTTP (4)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "http_request", .description = "Make HTTP requests (GET/POST/PUT/DELETE).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"method\":{\"type\":\"string\"},\"headers\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"url\"]}", .execute = tool_http_request, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "download_file", .description = "Download a file from URL.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"url\",\"path\"]}", .execute = tool_download },
    { .name = "curl_raw", .description = "Raw curl command execution.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\"}},\"required\":[\"args\"]}", .execute = tool_curl_raw },
    { .name = "network", .description = "Network diagnostics: dns, ping, port_check, port_scan, netstat, cert, traceroute, whois, interfaces, websocket.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"dns|ping|port_check|port_scan|netstat|cert|traceroute|whois|interfaces|websocket\"},\"host\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"}},\"required\":[\"action\"]}", .execute = tool_network_dispatch, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  INFRASTRUCTURE (5)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "docker", .description = "Docker operations.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}", .execute = tool_docker },
    { .name = "ssh_command", .description = "Run command on remote host via SSH.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\"},\"command\":{\"type\":\"string\"}},\"required\":[\"host\",\"command\"]}", .execute = tool_ssh_command },
    { .name = "sqlite", .description = "Execute SQLite queries.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"db\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"db\",\"query\"]}", .execute = tool_sqlite },
    { .name = "date", .description = "Get current date/time or parse dates.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"format\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"}}}", .execute = tool_date, .is_read_only = true, .is_concurrent = true },
    { .name = "calc", .description = "Evaluate math expressions.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expr\":{\"type\":\"string\"}},\"required\":[\"expr\"]}", .execute = tool_calc, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  SEARCH & EXTERNAL (4)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "tavily_search", .description = "Web search via Tavily.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}", .execute = tool_tavily_search, .is_read_only = true, .is_concurrent = true },
    { .name = "jina_search", .description = "AI-powered web search via Jina AI. Returns structured results with titles, URLs, and descriptions.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"num\":{\"type\":\"integer\",\"description\":\"Number of results (1-10, default 5)\"}},\"required\":[\"query\"]}", .execute = tool_jina_search, .is_read_only = true, .is_concurrent = true },
    { .name = "jina_embed", .description = "Compute embeddings via Jina v4 API. Returns 1024d float vectors for semantic similarity.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"task\":{\"type\":\"string\",\"description\":\"retrieval.passage|retrieval.query|classification|separation\"},\"dimensions\":{\"type\":\"integer\",\"description\":\"64-1024 (default 1024)\"}},\"required\":[\"text\"]}", .execute = tool_jina_embed, .is_read_only = true, .is_concurrent = true },
    { .name = "parallel_search", .description = "Fan out web search to multiple providers (Jina, Tavily, Brave) concurrently. Returns merged results from all available providers.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"num\":{\"type\":\"integer\",\"description\":\"Results per provider (1-10, default 5)\"}},\"required\":[\"query\"]}", .execute = tool_parallel_search, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "weather", .description = "Get weather data for a location.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}", .execute = tool_weather, .is_read_only = true, .is_concurrent = true },
    { .name = "slack_post", .description = "Post message to Slack.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"required\":[\"channel\",\"text\"]}", .execute = tool_slack_post },
    { .name = "github_search", .description = "Search GitHub repos, code, issues.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"description\":\"repos|code|issues\"}},\"required\":[\"query\"]}", .execute = tool_github_search, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  KNOWLEDGE BASE (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "knowledge_base", .description = "KB operations: ingest, search, deep_search, list, get, delete, arxiv_search, arxiv_ingest.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"ingest|search|deep_search|list|get|delete|arxiv_search|arxiv_ingest\"},\"query\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"doc_id\":{\"type\":\"integer\"},\"arxiv_id\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"action\"]}", .execute = tool_kb_dispatch },  /* mixed: search=RO, ingest/delete=write */

    /* ══════════════════════════════════════════════════════════════════════
     *  ALPHA VANTAGE — UNIFIED (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "alpha_vantage", .description = "Alpha Vantage financial data API. Supports 100+ functions: time series (TIME_SERIES_DAILY, TIME_SERIES_INTRADAY), technical indicators (SMA, EMA, RSI, MACD, BBANDS, STOCH, ADX, CCI, OBV, ATR, VWAP), fundamentals (OVERVIEW, INCOME_STATEMENT, BALANCE_SHEET, EARNINGS), macro (CPI, REAL_GDP, UNEMPLOYMENT, TREASURY_YIELD), commodities (WTI, BRENT, NATURAL_GAS, GOLD_SILVER_SPOT), forex (CURRENCY_EXCHANGE_RATE, FX_DAILY), crypto (DIGITAL_CURRENCY_DAILY), options (REALTIME_OPTIONS), news (NEWS_SENTIMENT).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"function\":{\"type\":\"string\",\"description\":\"AV API function name\"},\"symbol\":{\"type\":\"string\",\"description\":\"Ticker symbol\"},\"interval\":{\"type\":\"string\",\"description\":\"1min|5min|15min|30min|60min|daily|weekly|monthly\"},\"time_period\":{\"type\":\"string\",\"description\":\"Data points (e.g. 14, 200)\"},\"series_type\":{\"type\":\"string\",\"description\":\"close|open|high|low\"},\"outputsize\":{\"type\":\"string\",\"description\":\"compact|full\"},\"from_currency\":{\"type\":\"string\"},\"to_currency\":{\"type\":\"string\"},\"from_symbol\":{\"type\":\"string\"},\"to_symbol\":{\"type\":\"string\"},\"market\":{\"type\":\"string\"},\"maturity\":{\"type\":\"string\"},\"keywords\":{\"type\":\"string\"},\"tickers\":{\"type\":\"string\"},\"quarter\":{\"type\":\"string\"}},\"required\":[\"function\"]}", .execute = tool_alpha_vantage, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  KALSHI — UNIFIED (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "kalshi", .description = "Kalshi prediction market. Actions: markets, events, search, orderbook, trades, series, candlesticks, weather, snapshot, event_detail, daily (read); positions, balance, portfolio, fills, open_orders (account); create_order, batch_create, cancel_order, cancel_all, amend_order (trade); historical_markets, historical_trades, historical_cutoff (history).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Action to perform\"},\"ticker\":{\"type\":\"string\"},\"event_ticker\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"},\"series_ticker\":{\"type\":\"string\"},\"status\":{\"type\":\"string\"},\"side\":{\"type\":\"string\",\"description\":\"yes|no\"},\"yes_price\":{\"type\":\"integer\",\"description\":\"Price in cents 1-99\"},\"count\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"city\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\"},\"order_id\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_kalshi },

    /* ══════════════════════════════════════════════════════════════════════
     *  POLYMARKET — UNIFIED (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "polymarket", .description = "Polymarket prediction market. Actions: markets, events, categories, prices, book, trades, search, resolved, resolved_events, whale_trades, leaderboard, history (read); balance, positions, open_orders, api_keys, derive_api_key (account); create_order, cancel_order, cancel_all (trade); relayer_deploy, relayer_approve, relayer_execute, relayer_status (relayer).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"Action to perform\"},\"query\":{\"type\":\"string\"},\"tag\":{\"type\":\"string\"},\"token_id\":{\"type\":\"string\"},\"condition_id\":{\"type\":\"string\"},\"side\":{\"type\":\"string\",\"description\":\"buy|sell\"},\"price\":{\"type\":\"number\",\"description\":\"0.01-0.99\"},\"size\":{\"type\":\"number\"},\"order_id\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"},\"offset\":{\"type\":\"integer\"}},\"required\":[\"action\"]}", .execute = tool_polymarket },

    /* ══════════════════════════════════════════════════════════════════════
     *  WEATHER DATA — Synoptic (ASOS/METAR) + NWS (forecasts/alerts)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "synoptic", .description = "Synoptic Data real-time weather station observations (ASOS/METAR). Actions: latest (current obs), timeseries (historical), nearesttime, metadata, precip, kalshi_stations (all 29 Kalshi cities). Requires SYNOPTIC_API_TOKEN.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"latest|timeseries|nearesttime|metadata|precip|kalshi_stations\"},\"stid\":{\"type\":\"string\",\"description\":\"Station IDs comma-separated (KORD,KLAX)\"},\"vars\":{\"type\":\"string\",\"description\":\"Variables: air_temp,wind_speed,relative_humidity,precip_accum,dew_point_temperature\"},\"start\":{\"type\":\"string\",\"description\":\"Start time YYYYmmddHHMM\"},\"end\":{\"type\":\"string\",\"description\":\"End time YYYYmmddHHMM\"},\"recent\":{\"type\":\"integer\",\"description\":\"Minutes of recent data\"},\"within\":{\"type\":\"integer\",\"description\":\"Minutes window (default 60)\"},\"state\":{\"type\":\"string\",\"description\":\"US state abbreviation\"},\"attime\":{\"type\":\"string\",\"description\":\"Target time for nearesttime (YYYYmmddHHMM)\"}},\"required\":[\"action\"]}", .execute = tool_synoptic, .is_read_only = true, .is_concurrent = true },
    { .name = "nws", .description = "NWS API: forecast (lat/lon), hourly, station_obs (METAR station), alerts (by state), stations (near lat/lon), discussion (NWS office AFD). Free, no auth.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"forecast|hourly|station_obs|alerts|stations|discussion\"},\"lat\":{\"type\":\"string\"},\"lon\":{\"type\":\"string\"},\"stid\":{\"type\":\"string\",\"description\":\"Station ID (e.g. KORD)\"},\"state\":{\"type\":\"string\",\"description\":\"State abbrev for alerts\"},\"office\":{\"type\":\"string\",\"description\":\"NWS office for discussion (OKX,LOT,FWD,LAX,MFL)\"},\"severity\":{\"type\":\"string\",\"description\":\"Alert severity filter\"}},\"required\":[\"action\"]}", .execute = tool_nws, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  PREDICTION & CROSS-PLATFORM (3)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "prediction", .description = "Cross-platform prediction market ops: scan, weather, snapshot, arb, semantic_match, cross_delta, movers, cache_refresh, cache_query, historical.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"},\"topic\":{\"type\":\"string\"},\"city\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"},\"category\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_prediction_dispatch, .is_read_only = true, .is_concurrent = true },
    { .name = "strategy", .description = "Trading strategies: completeness, binary_fade, stale_snipe, kelly, spread_scan.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"series\":{\"type\":\"string\"},\"true_prob\":{\"type\":\"number\"},\"market_price\":{\"type\":\"number\"},\"bankroll\":{\"type\":\"number\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"action\"]}", .execute = tool_strategy_dispatch, .is_read_only = true, .is_concurrent = true },
    { .name = "systematic", .description = "Systematic trading: ingest_polymarket, ingest_kalshi, analytics, signals.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"platform\":{\"type\":\"string\"},\"pages\":{\"type\":\"integer\"}},\"required\":[\"action\"]}", .execute = tool_systematic_dispatch },  /* mixed: ingest=write */

    /* ══════════════════════════════════════════════════════════════════════
     *  TRADING OPERATIONS (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "trading", .description = "Trading ops: arb_execute, arb_monitor, portfolio, risk_check, risk_configure.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"platform\":{\"type\":\"string\"},\"amount_usd\":{\"type\":\"number\"},\"topic\":{\"type\":\"string\"},\"kalshi_ticker\":{\"type\":\"string\"},\"poly_token_id\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_trading_dispatch },

    /* ══════════════════════════════════════════════════════════════════════
     *  AGENTIC COMMERCE PROTOCOLS (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "agentic_commerce", .description = "Agentic commerce protocol registry: list/status/coverage/plan for ACP, UCP, AP2, x402, Stripe MPP/SPT, Visa, Mastercard, and clearing watchlist protocols.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"list|status|coverage|plan\"},\"protocol\":{\"type\":\"string\",\"description\":\"Protocol id or alias, e.g. acp, ucp, ap2, x402, mpp, stripe_spt, visa_tap, mastercard_agent_pay, rails\"}},\"required\":[\"action\"]}", .execute = tool_agentic_commerce, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  CONTRACT METADATA — ingest, search, lookup (3)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "contract_ingest", .description = "Bulk-fetch all open Kalshi events+markets into contracts.db. Persists title, settlement_date, strike, underlying, YES/NO meanings, prices. Run before searching.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"description\":\"Max events to fetch (default 200)\"},\"series_ticker\":{\"type\":\"string\",\"description\":\"Filter by series (KXBTC, KXFED, KXTEMP, etc)\"},\"status\":{\"type\":\"string\",\"description\":\"open|closed|settled\"}}}", .execute = tool_contract_ingest },
    { .name = "contract_search", .description = "Semantic search over persisted contracts. Natural language queries: 'Bitcoin above 90000', 'Fed rate cut March', 'Chicago temperature'. Uses FTS5 full-text search.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Natural language search\"},\"date\":{\"type\":\"string\",\"description\":\"Filter by settlement date YYYY-MM-DD\"},\"underlying\":{\"type\":\"string\",\"description\":\"Filter by asset: BTC, ETH, SPY, TEMP, FED_RATE, CPI, OIL, KORD, etc\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max results (default 20)\"}},\"required\":[\"query\"]}", .execute = tool_contract_search, .is_read_only = true, .is_concurrent = true },
    { .name = "contract_lookup", .description = "Get full contract context for a ticker or all markets in an event. Returns title, YES/NO meanings, settlement_date, strike, underlying, close_time, prices.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"ticker\":{\"type\":\"string\",\"description\":\"Market ticker for single contract\"},\"event_ticker\":{\"type\":\"string\",\"description\":\"Event ticker for full bracket view\"}}}", .execute = tool_contract_lookup, .is_read_only = true, .is_concurrent = true },
    { .name = "contract_ingest_all", .description = "Exhaustive historical ingestion: fetch ALL settled Kalshi markets via cursor pagination into contracts.db. Can take minutes for full history. Use max_pages to control depth.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"max_pages\":{\"type\":\"integer\",\"description\":\"Max pages to fetch, 200 markets/page (default 500)\"},\"series_ticker\":{\"type\":\"string\",\"description\":\"Filter by series\"}}}", .execute = tool_contract_ingest_all },
    { .name = "contract_new_issues", .description = "Detect NEW contracts not yet in contracts.db. Fetches current open events, diffs against stored contracts, returns only new issues. Run periodically (e.g. every hour) to catch new market listings.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"description\":\"Max events to scan (default 200)\"},\"series_ticker\":{\"type\":\"string\",\"description\":\"Filter by series\"}}}", .execute = tool_contract_new_issues, .is_read_only = true, .is_concurrent = true },
    { .name = "contract_landscape", .description = "Contract database summary: total/open/settled counts, breakdown by underlying asset, settlement date distribution, newest contracts.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_contract_landscape, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  AGENT & SWARM (2)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "net", .description = "Native networking: mesh P2P (libsodium encrypted), HTTP/TLS server/client (mbedTLS), bridge fleet ops, remote tool invocation. Actions: mesh/status, mesh/peers, mesh/send, mesh/broadcast, mesh/connect, http/post, http/status, bridge/fleet, bridge/exec, bridge/send, bridge/bus_put, bridge/bus_get, remote.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"mesh/status|mesh/peers|mesh/send|mesh/broadcast|mesh/connect|http/post|http/status|bridge/fleet|bridge/exec|bridge/send|bridge/bus_put|bridge/bus_get|remote\"},\"host\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},\"peer\":{\"type\":\"string\",\"description\":\"Fleet peer name or IP for bridge/exec and remote\"},\"peer_pubkey\":{\"type\":\"string\",\"description\":\"Hex pubkey for mesh/send\"},\"data\":{\"type\":\"string\",\"description\":\"Payload for mesh/send or mesh/broadcast\"},\"tool\":{\"type\":\"string\",\"description\":\"Tool name for remote invocation\"},\"params\":{\"type\":\"string\",\"description\":\"JSON params for remote tool\"},\"message\":{\"type\":\"string\",\"description\":\"Message for bridge/send\"},\"kind\":{\"type\":\"string\",\"description\":\"Kind for bus_put/bus_get\"},\"body\":{\"type\":\"string\",\"description\":\"Body for bus_put or http/post\"},\"since\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"},\"tls\":{\"type\":\"boolean\"},\"cmd\":{\"type\":\"string\",\"description\":\"Shell command for bridge/exec\"}},\"required\":[\"action\"]}", .execute = tool_net_dispatch },
    { .name = "agent", .description = "Agent management: spawn, status, output, wait, race, kill.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"spawn|status|output|wait|race|kill\"},\"task\":{\"type\":\"string\",\"description\":\"Task for action=spawn or race\"},\"model\":{\"type\":\"string\",\"description\":\"Model override for spawned agent\"},\"id\":{\"type\":\"integer\",\"description\":\"Agent ID for output|wait|kill\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds for wait or race timeout\"},\"contestants\":{\"type\":\"array\",\"description\":\"For action=race: array of model strings or {provider,model} objects\"}},\"required\":[\"action\"]}", .execute = tool_agent_dispatch },
    { .name = "swarm", .description = "Swarm orchestration: create, map_reduce, status, collect, budget, spawn_executor, spawn_provider, create_executor_swarm, executor_status, topology_list, topology_run, task_profile. map_reduce fans out 'tasks' as parallel workers then spawns a 'coordinator' sub-agent that synthesizes their outputs into one result (hierarchical map→reduce; workers may recurse).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"create|map_reduce|status|collect|budget|spawn_executor|spawn_provider|create_executor_swarm|executor_status|topology_list|topology_run|topology_solve|task_profile\"},\"topologies\":{\"type\":\"array\",\"description\":\"topology_solve: names of topologies to run the task across (default: trident,debate,tournament — each anchored on a different model)\"},\"name\":{\"type\":\"string\",\"description\":\"Swarm/group name for create|map_reduce\"},\"group_id\":{\"type\":\"integer\",\"description\":\"Group ID for status|collect\"},\"task\":{\"type\":\"string\",\"description\":\"Single task for spawn_executor|spawn_provider\"},\"tasks\":{\"type\":\"array\",\"description\":\"Task array (strings or {task,model}) for create|map_reduce|create_executor_swarm\"},\"coordinator\":{\"type\":\"string\",\"description\":\"map_reduce: synthesis instruction for the coordinator sub-agent that reduces worker outputs\"},\"coordinator_model\":{\"type\":\"string\",\"description\":\"map_reduce: model for the coordinator (defaults to 'model')\"},\"model\":{\"type\":\"string\",\"description\":\"Default model for spawned workers or topology\"},\"provider\":{\"type\":\"string\",\"description\":\"Native provider name for spawn_provider\"},\"executor\":{\"type\":\"string\",\"description\":\"dsco|claude|codex for executor-based actions\"},\"budget\":{\"type\":\"number\",\"description\":\"Budget (USD) partitioned across workers for create|map_reduce\"},\"budget_usd\":{\"type\":\"number\",\"description\":\"Budget for action=budget\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds per phase for collect|map_reduce\"},\"topology\":{\"type\":\"string\",\"description\":\"Topology name for topology_run\"}},\"required\":[\"action\"]}", .execute = tool_swarm_dispatch },

    /* ══════════════════════════════════════════════════════════════════════
     *  IPC (1)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "ipc", .description = "Inter-process communication: send, recv, agents, scratch_put, scratch_get, task_submit, task_list, set_role.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_ipc_dispatch },

    /* ══════════════════════════════════════════════════════════════════════
     *  WINGS + TALONS + IMMUNE (7)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "pheromone", .description = "Pheromone coordination (Wings): deposit, sense, status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"deposit|sense|status\"},\"trail\":{\"type\":\"string\"},\"strength\":{\"type\":\"number\"}},\"required\":[\"action\"]}", .execute = tool_pheromone_dispatch },
    { .name = "ooda", .description = "OODA loop discipline (Talons): begin, observe, orient, decide, complete, status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"begin|observe|orient|decide|complete|status\"},\"observation\":{\"type\":\"string\"},\"decision\":{\"type\":\"string\"},\"goal\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_ooda_dispatch },
    { .name = "killswitch", .description = "Kill switch control: trigger, resolve, status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"trigger|resolve|status\"},\"reason\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_killswitch_dispatch },
    { .name = "governance", .description = "Governance controls: status, authorize, checkpoint, budget, audit, param.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"status|authorize|checkpoint|budget|audit|param\"},\"operation\":{\"type\":\"string\"},\"amount\":{\"type\":\"number\"},\"param\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_governance_dispatch },
    { .name = "memory_tier", .description = "Three-tier memory: store, recall, promote, forget, status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"store|recall|promote|forget|status\"},\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_memory_dispatch },
    { .name = "talons", .description = "Competitive execution (Talons): goal, advance, tournament, recommend, status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"goal|advance|tournament|recommend|status\"},\"goal\":{\"type\":\"string\"},\"step\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_talons_dispatch },
    { .name = "wings_talons_status", .description = "Unified Wings+Talons+Immune system status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_wings_talons_status, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  PLAYBOOK & META (6)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "playbook", .description = "ACE playbook: read, add, tag, remove, search, gc, inject.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"read|add|tag|remove|search|gc|inject\"},\"text\":{\"type\":\"string\"},\"section\":{\"type\":\"string\"},\"id\":{\"type\":\"integer\"},\"query\":{\"type\":\"string\"},\"tag\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_playbook_dispatch },
    { .name = "scratchpad", .description = "Read/write scratchpad for temporary data.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"read|write|clear\"},\"content\":{\"type\":\"string\"}}}", .execute = tool_scratchpad, .core = true },  /* mixed: read=RO, write/clear=write */
    { .name = "self_exit", .description = "Gracefully exit the agent loop.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\"}}}", .execute = tool_self_exit, .core = true },
    { .name = "self_exiting", .description = "Legacy alias for self_exit.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\"}}}", .execute = tool_self_exiting, .core = true },
    { .name = "self_improve", .description = "Run the self-improvement loop: summary, consolidate, acknowledge suggestions, load/save history. Actions: summary|consolidate|acknowledge|history|save.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"summary|consolidate|acknowledge|history|save\"},\"suggestion_id\":{\"type\":\"integer\",\"description\":\"1-based suggestion ID for acknowledge\"}},\"required\":[\"action\"]}", .execute = tool_self_improve, .is_read_only = true, .is_concurrent = false },
    { .name = "self_assess", .description = "Quick self-evaluation of current session performance. Returns efficiency score, top issues, and recommendations. No input required.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_self_assess, .is_read_only = true, .is_concurrent = true },
    {
        .name = "StartOfLoopConstruct",
        .description =
            "Start a live recursive agent loop construct. Accepts a bounded "
            "MetaConstruct/OORL DSL: continue/break expressions, max controls, "
            "DEFINE/GOAL/TASK/BELIEF/INFER/DECIDE/LEARN metadata, mutable "
            "ontology graph nodes/edges, dyad object interactions, reward "
            "objects, valence/intensity, causal/message links, stochastic "
            "exploration, pruning, credit assignment, attractors, prompt games, "
            "basin hopping, effect weights, traversal/find/balance operations, "
            "MapReduce map/shuffle/reduce job state, "
            "SRM catalog/store search, availability/orderability, licensed "
            "distributors, order policies, shipping restrictions, "
            "standard reference material records, certificates/reports/SDS, "
            "metrological traceability, calibration measurements, uncertainty "
            "budgets, "
            "one-shot REFINE rules, and bounded schema_rewrite rules. Example: "
            "define(sensor,state); reward_object success valence 0.8 intensity "
            "0.5 target state; causal_link state -> action weight 0.7; "
            "schema_rewrite add_edge state -> policy relation optimized weight "
            "0.9 when credit >= 0.8; continue when rewrites_applied >= 1. "
            "Expressions support loop variables plus meta_count, belief_count, "
            "goal_count, task_count, dyad_count, reward_object_count, "
            "causal_link_count, message_count, node_count, edge_count, "
            "graph_density, traverse_hits, mapreduce_count, map_count, "
            "shuffle_count, reduce_count, partition_count, rewrite_count, "
            "rewrites_applied, srm_count, current_certificate_count, "
            "sds_count, traceability_count, measurement_count, "
            "calibration_count, uncertainty_budget_count, mean_uncertainty, "
            "max_uncertainty, available_count, orderable_count, "
            "product_search_count, catalog_count, annual_catalog_count, "
            "licensed_distributor_count, order_policy_count, "
            "paper_checks_blocked, shipping_block_count, price_total, "
            "effect.tool, effect.world, effect.meta, reward, valence, "
            "intensity, exploration_rate, credit, curiosity, empowerment, "
            "confidence, uncertainty, learning_rate, pruning_threshold, "
            "basin_temperature.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"label\":{\"type\":\"string\"},"
            "\"program\":{\"type\":\"string\",\"description\":\"MetaConstruct "
            "DSL program. Statements: continue when <expr>; break when <expr>; "
            "max_iterations=<n>; max_turns=<n>; recursive=<bool>; "
            "override_done=<bool>; override_max_turns=<bool>; prompt=<text>; "
            "define(a,b); object x as type; goal g weight n; task t priority n; "
            "belief b=n; infer x from y; decide policy; learn rate=n; "
            "add_node n as type state s weight w; update_node n state s; "
            "replace_node a with b; remove_node n; add_edge a -> b relation r "
            "weight w; remove_edge a -> b; find n; traverse from n depth d; "
            "balance graph; mapreduce job over source map mapper reduce reducer "
            "by key partitions n; map job over source using mapper; shuffle "
            "job by key partitions n; reduce job using reducer; reward_object "
            "r valence v intensity i target x; "
            "srm id matrix m property p certificate current sds available "
            "traceable uncertainty n; measurement x on id value n uncertainty "
            "n unit u; calibration tool using id uncertainty n; "
            "uncertainty_budget x=n; "
            "annual_product_list name current; catalog name store shop.nist.gov; "
            "product_search id store shop.nist.gov found; availability id "
            "available orderable price n; order_policy no_paper_checks; "
            "licensed_distributor name; shipping id to dest blocked|allowed; "
            "registration x; survey x; "
            "causal_link a -> b weight w; message a -> b weight w; explore "
            "objects rate e; credit x=n; prune_edges below n; attractor name "
            "basin n; prompt_game name; dyad a -> b relation r; effect "
            "tool|world|meta|conversational=n; refine target += n when <expr>; "
            "schema_rewrite <single DSL statement> when <expr>\"},"
            "\"construct\":{\"type\":\"string\",\"description\":\"Alias for "
            "program\"},"
            "\"conditions\":{\"type\":\"string\",\"description\":\"Legacy "
            "conditions or MetaConstruct DSL, e.g. |c| continue when iteration "
            "< 3; break when turn >= 20\"},"
            "\"condition\":{\"type\":\"string\"},"
            "\"continue_prompt\":{\"type\":\"string\"},"
            "\"max_iterations\":{\"type\":\"integer\"},"
            "\"max_turns\":{\"type\":\"integer\"},"
            "\"recursive\":{\"type\":\"boolean\"},"
            "\"override_done\":{\"type\":\"boolean\"},"
            "\"override_max_turns\":{\"type\":\"boolean\"}}}",
        .execute = tool_start_loop_construct,
        .core = true
    },
    { .name = "EndOfLoopConstruct", .description = "Continue, modify, break, complete, or unwind live loop constructs. action=continue/recur can replace the active MetaConstruct DSL program; action=break/complete exits. exit_break_conditions=true resets iteration and restores done/max-turn overrides.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"action\":{\"type\":\"string\",\"description\":\"continue|recur|break|complete|exit|pop\"},\"reason\":{\"type\":\"string\"},\"program\":{\"type\":\"string\",\"description\":\"Replacement MetaConstruct DSL program for action=continue/recur\"},\"construct\":{\"type\":\"string\",\"description\":\"Alias for program\"},\"conditions\":{\"type\":\"string\",\"description\":\"Legacy conditions or replacement DSL program\"},\"continue_prompt\":{\"type\":\"string\"},\"exit_break_conditions\":{\"type\":\"boolean\"},\"all\":{\"type\":\"boolean\"}}}", .execute = tool_end_loop_construct, .core = true },
    { .name = "LoopConstructStatus", .description = "Inspect the live recursive MetaConstruct stack, parsed continue/break expressions, counters, override flags, ontology metadata, mutable graph nodes/edges, traversal state, dyads, MapReduce jobs, SRM/metrology and catalog/order state, effects, reward dynamics, learning signals, policies, decisions, attractors, prompt games, refinement rules, and schema rewrite rules.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_loop_construct_status, .core = true, .is_read_only = true },
    { .name = "discover_tools", .description = "List available tools by category or search.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"category\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"}}}", .execute = tool_discover_tools, .core = true, .is_read_only = true, .is_concurrent = true },
    { .name = "load_tools", .description = "Dynamically load tools into the active register file. Provide at least one of: names (comma-separated), tools (array), or category.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"names\":{\"type\":\"string\",\"description\":\"Comma-separated tool names to load\"},\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tool names to load\"},\"category\":{\"type\":\"string\",\"description\":\"Tool category to bulk load\"}}}", .execute = tool_load_tools, .core = true },
    { .name = "legion", .description = "Legion agent system: spawn, status, find.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"spawn|status|find\"},\"class\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"}},\"required\":[\"action\"]}", .execute = tool_legion_dispatch },

    /* ══════════════════════════════════════════════════════════════════════
     *  VOS & PIPELINE (2)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "computer", .description = "Control the local desktop like a human: screenshot, mouse_move, left_click, right_click, middle_click, double_click, triple_click, left_click_drag, key (combos like cmd+a), type, scroll, cursor_position, wait. Coordinates are display points [x,y]; a fresh screenshot is attached after each action so you can see the result.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"description\":\"screenshot|cursor_position|mouse_move|left_click|right_click|middle_click|double_click|triple_click|left_click_drag|key|type|scroll|wait\"},\"coordinate\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"[x,y] in display points\"},\"to\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"[x,y] drag destination\"},\"text\":{\"type\":\"string\",\"description\":\"text to type, or key combo for action=key\"},\"scroll_direction\":{\"type\":\"string\",\"description\":\"up|down|left|right\"},\"scroll_amount\":{\"type\":\"integer\"},\"duration\":{\"type\":\"integer\",\"description\":\"wait milliseconds\"}},\"required\":[\"action\"]}", .execute = tool_computer },
    { .name = "vos_status", .description = "Virtual OS subsystem status.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_vos_status, .is_read_only = true, .is_concurrent = true },
    { .name = "pipeline", .description = "Pipeline execution and chaining.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"steps\":{\"type\":\"string\",\"description\":\"Pipeline steps as JSON array\"}},\"required\":[\"steps\"]}", .execute = tool_pipeline },

    /* ══════════════════════════════════════════════════════════════════════
     *  CRYPTO & UTILITY
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "sha256", .description = "Compute SHA-256 hash of text.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", .execute = tool_sha256, .is_read_only = true, .is_concurrent = true },
    { .name = "md5", .description = "Compute MD5 hash of text.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", .execute = tool_md5, .is_read_only = true, .is_concurrent = true },
    { .name = "hmac", .description = "Compute HMAC-SHA256.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"key\",\"message\"]}", .execute = tool_hmac, .is_read_only = true, .is_concurrent = true },
    { .name = "uuid", .description = "Generate a UUID v4.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_uuid, .is_read_only = true, .is_concurrent = true },
    { .name = "random_bytes", .description = "Generate random bytes (hex).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\"}},\"required\":[\"count\"]}", .execute = tool_random_bytes, .is_read_only = true, .is_concurrent = true },
    { .name = "base64_tool", .description = "Base64 encode/decode.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"},\"action\":{\"type\":\"string\",\"description\":\"encode|decode\"}},\"required\":[\"action\"]}", .execute = tool_base64_tool, .is_read_only = true, .is_concurrent = true },
    { .name = "base64", .description = "Base64 encode/decode (legacy).", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"data\":{\"type\":\"string\"}},\"required\":[\"data\"]}", .execute = tool_base64, .is_read_only = true, .is_concurrent = true },
    { .name = "eval", .description = "Evaluate a math expression.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\"}},\"required\":[\"expression\"]}", .execute = tool_eval, .is_read_only = true, .is_concurrent = true },
    { .name = "cwd", .description = "Get current working directory.", .input_schema_json = "{\"type\":\"object\",\"properties\":{}}", .execute = tool_cwd, .core = true, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  EXECUTION ALIASES & SANDBOX
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "run_command", .description = "Run a shell command with optional artifact verification.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\"},\"verify_path\":{\"type\":\"string\"},\"artifact_path\":{\"type\":\"string\"},\"output_path\":{\"type\":\"string\"},\"verify_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"verify_min_bytes\":{\"type\":\"integer\"},\"verify_contains\":{\"type\":\"string\"},\"verify_sha256\":{\"type\":\"string\"}},\"required\":[\"command\"]}", .execute = tool_run_command },
    { .name = "sandbox_run", .description = "Run command in sandboxed container.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"image\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\"},\"network\":{\"type\":\"boolean\"},\"filesystem\":{\"type\":\"string\"}},\"required\":[\"command\"]}", .execute = tool_sandbox_run },
    { .name = "agent_wait", .description = "Wait for agent(s) to complete.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"timeout\":{\"type\":\"integer\"}}}", .execute = tool_agent_wait },

    /* ══════════════════════════════════════════════════════════════════════
     *  PLAYBOOK SHORTCUTS (WARM tier)
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "playbook_add", .description = "Add entry to ACE playbook.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"section\":{\"type\":\"string\"},\"tag\":{\"type\":\"string\"}},\"required\":[\"text\"]}", .execute = tool_playbook_add },
    { .name = "playbook_search", .description = "Search ACE playbook.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}", .execute = tool_playbook_search, .is_read_only = true, .is_concurrent = true },

    /* ══════════════════════════════════════════════════════════════════════
     *  DATA PARSING & COMPARISON
     * ══════════════════════════════════════════════════════════════════════ */
    { .name = "semver_compare", .description = "Compare two semantic versions.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"version_a\":{\"type\":\"string\"},\"version_b\":{\"type\":\"string\"}},\"required\":[\"version_a\",\"version_b\"]}", .execute = tool_semver, .is_read_only = true, .is_concurrent = true },
    { .name = "cron_parse", .description = "Parse a cron expression.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\"}},\"required\":[\"expression\"]}", .execute = tool_cron_parse, .is_read_only = true, .is_concurrent = true },
    { .name = "url_parse", .description = "Parse a URL into components.", .input_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}", .execute = tool_url_parse, .is_read_only = true, .is_concurrent = true },
};


static const int s_tool_count = sizeof(s_tools) / sizeof(s_tools[0]);

/* ── tools_invoke_by_name: used by HTTP /tool route ─────────────────────── */
bool tools_invoke_by_name(const char *name, const char *input,
                           char *result, size_t rlen) {
    if (!name || !result || rlen == 0) return false;
    result[0] = '\0';
    for (int i = 0; i < s_tool_count; i++) {
        if (!s_tools[i].name || !s_tools[i].execute) continue;
        if (strcmp(s_tools[i].name, name) == 0) {
            return s_tools[i].execute(input ? input : "{}", result, rlen);
        }
    }
    snprintf(result, rlen, "{\"error\":\"unknown tool: %s\"}", name);
    return false;
}

static tools_init_profile_t g_tools_init_profile = TOOLS_FULL;

/* Forward declarations for hash map */
static unsigned tool_name_hash(const char *s);
static void tool_map_rebuild(void);

/* ── Agent profile tool filter ──────────────────────────────────────── */

static tool_profile_filter_fn_t g_profile_filter = NULL;

void tools_set_profile_filter(tool_profile_filter_fn_t fn) {
    g_profile_filter = fn;
}

void tools_clear_profile_filter(void) {
    g_profile_filter = NULL;
}

void tools_init_local_only(void) {
    /* Fast path for local metadata/direct-tool commands. Keep this free of
     * subsystem startup: no plugins, browser profiles, IPC, MCP, or VFS. */
    g_tools_init_profile = TOOLS_AGENT;
    tool_map_rebuild();
}

void tools_init(void) {
    tools_init_profile(TOOLS_FULL);
}

void tools_init_profile(tools_init_profile_t profile) {
    g_tools_init_profile = profile;
    if (profile == TOOLS_CORE || profile == TOOLS_AGENT) {
        tool_map_rebuild();
        return;
    }

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
    tool_map_rebuild();
}

tools_init_profile_t tools_current_profile(void) {
    return g_tools_init_profile;
}

bool tools_profile_allows_index(int index) {
    if (index < 0 || index >= s_tool_count) return false;
    if (g_tools_init_profile == TOOLS_FULL ||
        g_tools_init_profile == TOOLS_AGENT)
        return true;
    return s_tools[index].core;
}

void tools_register_vm_dispatch(vm_t *vm) {
    /* §3: Populate bytecode VM dispatch table from tool registry.
       This enables computed-goto O(1) tool dispatch via FNV-1a hashing. */
    for (int i = 0; i < s_tool_count; i++) {
        if (s_tools[i].name && s_tools[i].execute) {
            vm_register_tool(vm, s_tools[i].name, s_tools[i].execute, i);
        }
    }
    vm_build_dispatch_index(vm);
}

const tool_def_t *tools_get_all(int *count) {
    *count = s_tool_count + g_plugins.extra_tool_count;
    return s_tools;
}

int tools_builtin_count(void) {
    return s_tool_count;
}

int tools_get_core_count(void) {
    int n = 0;
    for (int i = 0; i < s_tool_count; i++)
        if (s_tools[i].core) n++;
    return n;
}

/* ── Tool retrieval: BM25 + TF-IDF semantic index ──────────────────── */

#include "semantic.h"

/* ── Jina v4 embeddings: static vectors + runtime query embedding ───── */
#include "tool_embeddings.h"

/* Static embeddings loaded from .bin file (1024d × 364 tools) */
static float *g_emb_vectors = NULL;  /* flat array: [TOOL_EMB_COUNT * TOOL_EMB_DIM] */
static int    g_emb_count = 0;
static int    g_emb_dim = 0;

static void tool_embeddings_expand_path(char *out, size_t out_len, const char *path) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(out, out_len, "%s/%s", home, path + 2);
            return;
        }
    }

    snprintf(out, out_len, "%s", path);
}

static bool tool_embeddings_try_open(FILE **out_fp, char *loaded_path, size_t loaded_len,
                                     const char *path) {
    if (!out_fp || !path || !path[0]) return false;

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    *out_fp = fp;
    if (loaded_path && loaded_len > 0)
        snprintf(loaded_path, loaded_len, "%s", path);
    return true;
}

static void ensure_embeddings_loaded(void) {
    if (g_emb_vectors) return;

    FILE *fp = NULL;
    char loaded_path[PATH_MAX] = {0};

    const char *override = getenv("DSCO_TOOL_EMBEDDINGS_FILE");
    if (override && override[0]) {
        char expanded[PATH_MAX];
        tool_embeddings_expand_path(expanded, sizeof(expanded), override);
        tool_embeddings_try_open(&fp, loaded_path, sizeof(loaded_path), expanded);
    }

    char exe_dir[PATH_MAX] = {0};
    {
        char self[PATH_MAX] = {0};
#ifdef __APPLE__
        uint32_t sz = sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0)
            self[0] = '\0';
#else
        ssize_t got = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (got >= 0) self[got] = '\0';
#endif
        if (self[0]) {
            char *sl = strrchr(self, '/');
            if (sl) *sl = '\0';
            snprintf(exe_dir, sizeof(exe_dir), "%s", self);
        }
    }

    if (!fp) {
        const char *cwd_paths[] = {
            "include/tool_embeddings.bin",
            "../include/tool_embeddings.bin",
            NULL
        };
        for (int i = 0; cwd_paths[i] && !fp; i++)
            tool_embeddings_try_open(&fp, loaded_path, sizeof(loaded_path), cwd_paths[i]);
    }

    if (!fp && exe_dir[0]) {
        char candidate[PATH_MAX];
        const char *suffixes[] = {
            "include/tool_embeddings.bin",
            "../include/tool_embeddings.bin",
            "../share/dsco/tool_embeddings.bin",
            "tool_embeddings.bin",
            NULL
        };
        for (int i = 0; suffixes[i] && !fp; i++) {
            snprintf(candidate, sizeof(candidate), "%s/%s", exe_dir, suffixes[i]);
            tool_embeddings_try_open(&fp, loaded_path, sizeof(loaded_path), candidate);
        }
    }

    if (!fp) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char candidate[PATH_MAX];
            snprintf(candidate, sizeof(candidate), "%s/.dsco/tool_embeddings.bin", home);
            tool_embeddings_try_open(&fp, loaded_path, sizeof(loaded_path), candidate);
        }
    }

    if (!fp) {
        fprintf(stderr, "  \033[33mno tool_embeddings.bin found\033[0m\n");
        return;
    }

    uint32_t header[2];
    if (fread(header, sizeof(uint32_t), 2, fp) != 2) { fclose(fp); return; }
    g_emb_count = (int)header[0];
    g_emb_dim = (int)header[1];
    if (g_emb_dim > 1024) g_emb_dim = 1024; /* clamp to centroid array size */

    size_t total_floats = (size_t)g_emb_count * g_emb_dim;
    g_emb_vectors = safe_malloc(total_floats * sizeof(float));
    size_t read_n = fread(g_emb_vectors, sizeof(float), total_floats, fp);
    fclose(fp);

    if ((int)read_n != (int)total_floats) {
        free(g_emb_vectors); g_emb_vectors = NULL;
        g_emb_count = 0;
        fprintf(stderr, "  \033[31mtool_embeddings.bin truncated\033[0m\n");
        return;
    }

    fprintf(stderr, "  \033[2memb: %d tools × %dd loaded\033[0m\n", g_emb_count, g_emb_dim);
    if (provider_debug_auth_enabled() && loaded_path[0])
        fprintf(stderr, "  \033[2m[emb] path=%s\033[0m\n", loaded_path);
}

/* Embed a query via Jina v4. Returns malloc'd float[g_emb_dim] or NULL. */
static float *embed_query_jina(const char *text) {
    const char *api_key = getenv("JINA_API_KEY");
    if (!api_key || !api_key[0]) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    jbuf_t req;
    jbuf_init(&req, 512);
    jbuf_appendf(&req, "{\"model\":\"jina-embeddings-v4\",\"task\":\"retrieval.query\","
                 "\"dimensions\":%d,\"embedding_type\":\"float\",\"input\":[", g_emb_dim);
    jbuf_append_json_str(&req, text);
    jbuf_append(&req, "]}");

    jbuf_t resp;
    jbuf_init(&resp, 8192);

    struct curl_slist *hdrs = NULL;
    char auth[256];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "User-Agent: dsco/0.9.0");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.jina.ai/v1/embeddings");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jbuf_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    jbuf_free(&req);

    if (res != CURLE_OK || http_code != 200 || !resp.data || !resp.data[0]) {
        jbuf_free(&resp);
        return NULL;
    }

    /* Parse: data[0].embedding = [f, f, f, ...] */
    float *vec = safe_malloc(g_emb_dim * sizeof(float));
    memset(vec, 0, g_emb_dim * sizeof(float));
    char *data_arr = json_get_raw(resp.data, "data");
    if (!data_arr) { jbuf_free(&resp); free(vec); return NULL; }

    char *emb_str = json_get_raw(data_arr, "embedding");
    free(data_arr);
    if (!emb_str) { jbuf_free(&resp); free(vec); return NULL; }

    const char *p = emb_str;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    for (int i = 0; i < g_emb_dim && *p; i++) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n')) p++;
        if (*p == ']') break;
        vec[i] = (float)strtod(p, (char **)&p);
    }
    free(emb_str);
    jbuf_free(&resp);
    return vec;
}

/* Cosine similarity — float vectors */
static float cosine_sim_f(const float *a, const float *b, int dim) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = sqrtf(na) * sqrtf(nb);
    return denom > 1e-8f ? dot / denom : 0.0f;
}

/* ── Public embedding wrapper (used by memory_tier, agent) ────────────── */

float *tools_embed_text(const char *text, int *out_dim) {
    ensure_embeddings_loaded();
    if (out_dim) *out_dim = g_emb_dim > 0 ? g_emb_dim : 1024;
    return embed_query_jina(text);
}

/* ── Agent context for context-aware tool selection ──────────────────── */

static char g_agent_ctx_results[2048] = "";
static char g_agent_ctx_memory[2048] = "";

void tools_set_agent_context(const char *recent_results,
                             const char *working_memory_summary) {
    if (recent_results)
        snprintf(g_agent_ctx_results, sizeof(g_agent_ctx_results), "%s", recent_results);
    else
        g_agent_ctx_results[0] = '\0';

    if (working_memory_summary)
        snprintf(g_agent_ctx_memory, sizeof(g_agent_ctx_memory), "%s", working_memory_summary);
    else
        g_agent_ctx_memory[0] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HIERARCHICAL TOOL RETRIEVAL WITH SLOT ALLOCATION
 *
 * Architecture (scales to 10K+ tools with constant query cost):
 *
 *   Level 0: SLOT TABLE — fixed budget (e.g. 48 slots), LRU hot cache
 *   Level 1: GROUP CENTROIDS — K clusters, cosine sim vs K centroids O(K)
 *   Level 2: INTRA-GROUP RANK — only search top 3 groups O(k) where k~50
 *   Level 3: EMBEDDING REFINE — Jina query vec for precision within winners
 *
 * At 10K tools with 20 groups of ~500 each:
 *   Old: O(10000) cosine sims per query
 *   New: O(20) centroid sims + O(1500) intra-group sims = 15x faster
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Cluster / Group definitions ──────────────────────────────────────── */

#define MAX_GROUPS        32
#define MAX_GROUP_TOOLS  512   /* up to 512 tools per group — scales to 10K+ total */
#define HOT_CACHE_SIZE    16   /* LRU: recently-used tools get priority slots */

typedef struct {
    const char *name;
    int         tool_indices[MAX_GROUP_TOOLS];
    int         count;
    float       centroid[1024]; /* mean embedding of all tools in group */
    bool        has_centroid;
} tool_group_t;

static tool_group_t g_tool_groups[MAX_GROUPS];
static int g_group_count = 0;
static bool g_groups_built = false;

/* ── Hot cache: LRU of recently-executed tools ────────────────────────── */

static int  g_hot_cache[HOT_CACHE_SIZE];
static int  g_hot_count = 0;

void tools_mark_hot(int tool_idx) {
    /* Move to front of LRU */
    for (int i = 0; i < g_hot_count; i++) {
        if (g_hot_cache[i] == tool_idx) {
            /* Shift left */
            for (int j = i; j > 0; j--) g_hot_cache[j] = g_hot_cache[j-1];
            g_hot_cache[0] = tool_idx;
            return;
        }
    }
    /* Not in cache — insert at front, evict oldest */
    if (g_hot_count < HOT_CACHE_SIZE) g_hot_count++;
    for (int j = g_hot_count - 1; j > 0; j--) g_hot_cache[j] = g_hot_cache[j-1];
    g_hot_cache[0] = tool_idx;
}

/* ── Core tool set: register-file model ─────────────────────────────────
 * ALWAYS:          Hardwired registers, never evicted. Minimum viable set
 *                  for file I/O, search, execution, and context management.
 * WARM:            Default-loaded but evictable under budget pressure.
 *                  Swarm/agent orchestration, secondary I/O, soul access.
 * Together they define "core" for backward compat, but the register file
 * can shrink the warm bank under cost pressure. */

static const char *CORE_ALWAYS[] = {
    "bash", "python",
    "discover_tools", "load_tools", "self_exit",
    "StartOfLoopConstruct", "EndOfLoopConstruct",
    NULL  /* minimal core: execution + dynamic loading + loop control */
};

static const char *CORE_WARM[] = {
    "read_file", "write_file", "edit_file", "list_directory",
    "find_files", "grep_files", "run_command",
    "context_status", "scratchpad", "playbook_add", "playbook_search",
    NULL  /* 11 tools → fills R5-R15 when budget allows */
};

static bool is_core_always(const char *name) {
    for (int i = 0; CORE_ALWAYS[i]; i++)
        if (strcmp(name, CORE_ALWAYS[i]) == 0) return true;
    return false;
}

static bool is_core_warm(const char *name) {
    for (int i = 0; CORE_WARM[i]; i++)
        if (strcmp(name, CORE_WARM[i]) == 0) return true;
    return false;
}

static bool is_core_tool(const char *name) {
    return is_core_always(name) || is_core_warm(name);
}

/* ── Group assignment (rule-based, fast) ──────────────────────────────── */

static int assign_group(const char *name, const char *desc) {
    /* Returns group index. Groups are: file_io=0, git=1, network=2, shell=3,
       code=4, crypto=5, swarm=6, ast=7, pipeline=8, math=9, search=10, general=11,
       market=12, prediction=13, memory=14 */
    if (strncmp(name, "git_", 4) == 0 || strncmp(name, "github_", 7) == 0) return 1;
    if (strncmp(name, "av_", 3) == 0 || strncmp(name, "fred_", 5) == 0 ||
        strstr(name, "market") || strstr(name, "stripe")) return 12;
    if (strstr(name, "polymarket") || strstr(name, "kalshi") || strstr(name, "prediction")) return 13;
    if (strncmp(name, "ipc_", 4) == 0 || strstr(name, "swarm") || strstr(name, "spawn") ||
        strstr(name, "agent") || strstr(name, "topology") || strstr(name, "ooda") ||
        strstr(name, "pheromone") || strstr(name, "talons") || strstr(name, "governance") ||
        strstr(name, "killswitch") || strstr(name, "executor") || strstr(name, "openrouter")) return 6;
    if (strstr(name, "memory_") || strstr(name, "soul_")) return 14;
    if (strstr(name, "hash") || strstr(name, "hmac") || strstr(name, "hkdf") ||
        strstr(name, "jwt") || strstr(name, "uuid") || strstr(name, "random") ||
        strstr(name, "secret") || strstr(name, "cert") || strstr(name, "token_audit")) return 5;
    if (strstr(name, "http") || strstr(name, "curl") || strstr(name, "url") ||
        strstr(name, "dns") || strstr(name, "ping") || strstr(name, "port") ||
        strstr(name, "websocket") || strstr(name, "web_") || strstr(name, "download") ||
        strstr(name, "upload") || strstr(name, "weather") || strstr(name, "whois") ||
        strstr(name, "brave") || strstr(name, "tavily") || strstr(name, "serpapi") ||
        strstr(name, "jina") || strstr(name, "firecrawl") || strstr(name, "browser") ||
        strstr(name, "slack") || strstr(name, "notion") || strstr(name, "discord") ||
        strstr(name, "twilio") || strstr(name, "elevenlabs") || strstr(name, "pinecone") ||
        strstr(name, "supabase") || strstr(name, "huggingface")) return 2;
    if (strstr(name, "inspect") || strstr(name, "call_graph") || strstr(name, "dependency") ||
        strstr(name, "code_") || strstr(name, "compile") || strstr(name, "self_inspect")) return 7;
    if (strncmp(name, "docker", 6) == 0 || strncmp(name, "npm", 3) == 0 ||
        strncmp(name, "node", 4) == 0 || strncmp(name, "pip", 3) == 0 ||
        strncmp(name, "pkg", 3) == 0 || strstr(name, "sandbox") ||
        strstr(name, "crontab") || strstr(name, "process") || strstr(name, "sysinfo") ||
        strstr(name, "system_profiler")) return 3;
    if (strstr(name, "csv") || strstr(name, "xml") || strstr(name, "jq") ||
        strstr(name, "awk") || strstr(name, "sed") || strstr(name, "sort_") ||
        strstr(name, "regex") || strstr(name, "template") || strstr(name, "text_diff") ||
        strstr(name, "string_")) return 8;
    if (strstr(name, "calc") || strstr(name, "eval") || strstr(name, "factorial") ||
        strstr(name, "semver") || strstr(name, "cron_parse")) return 9;
    if (strstr(name, "file") || strstr(name, "dir") || strstr(name, "mkdir") ||
        strstr(name, "chmod") || strstr(name, "symlink") || strstr(name, "xattr") ||
        strstr(name, "tar") || strstr(name, "zip") || strstr(name, "disk") ||
        strstr(name, "tree") || strstr(name, "page_") || strstr(name, "view_")) return 0;
    if (desc) {
        if (strstr(desc, "HTTP") || strstr(desc, "API") || strstr(desc, "endpoint")) return 2;
        if (strstr(desc, "encrypt") || strstr(desc, "hash")) return 5;
    }
    return 11; /* general */
}

static const char *GROUP_NAMES[] = {
    "file_io", "git", "network", "shell", "code", "crypto",
    "swarm", "ast", "pipeline", "math", "search", "general",
    "market", "prediction", "memory", NULL
};

/* ── Compact parameter extraction from JSON schema ──────────────────── */

/* Parse a tool's input_schema_json and build a compact params string.
 * Input:  {"type":"object","properties":{"a":{"type":"string"},"b":...},"required":["a"]}
 * Output: "(a*, b)" where * marks required params.
 * Returns number of chars written. */
static int build_compact_params(const char *schema, char *out, size_t outlen) {
    if (!schema || !out || outlen < 3) { if (out) out[0] = 0; return 0; }

    /* 1. Extract required field names */
    char req[16][64];
    int nreq = 0;
    const char *rp = strstr(schema, "\"required\":[");
    if (rp) {
        rp += 12;
        while (*rp && *rp != ']' && nreq < 16) {
            if (*rp == '"') {
                rp++;
                const char *e = strchr(rp, '"');
                if (!e || (e - rp) >= 64) break;
                memcpy(req[nreq], rp, (size_t)(e - rp));
                req[nreq][e - rp] = '\0';
                nreq++;
                rp = e + 1;
            } else rp++;
        }
    }

    /* 2. Find properties object */
    const char *pp = strstr(schema, "\"properties\":{");
    if (!pp) { out[0] = '('; out[1] = ')'; out[2] = '\0'; return 2; }
    pp += 14;

    /* 3. Extract property names at depth 0 inside properties */
    char names[20][64];
    int nprop = 0;
    int depth = 0;
    const char *p = pp;

    while (*p && nprop < 20) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') {
            if (depth == 0) break;
            depth--; p++; continue;
        }
        if (depth == 0 && *p == '"') {
            p++;
            const char *e = strchr(p, '"');
            if (!e || (e - p) >= 64) break;
            memcpy(names[nprop], p, (size_t)(e - p));
            names[nprop][e - p] = '\0';
            nprop++;
            p = e + 1;
            while (*p && *p != ':') p++;
            if (*p == ':') p++;
            continue;
        }
        p++;
    }

    /* 4. Build output string */
    int pos = 0;
    pos += snprintf(out + pos, outlen - (size_t)pos, "(");
    for (int i = 0; i < nprop && (size_t)pos < outlen - 10; i++) {
        if (i > 0) pos += snprintf(out + pos, outlen - (size_t)pos, ", ");
        bool is_req = false;
        for (int j = 0; j < nreq; j++) {
            if (strcmp(names[i], req[j]) == 0) { is_req = true; break; }
        }
        pos += snprintf(out + pos, outlen - (size_t)pos, "%s%s",
                        names[i], is_req ? "*" : "");
    }
    pos += snprintf(out + pos, outlen - (size_t)pos, ")");
    return pos;
}

/* Build a compact text catalog of all tools for the system prompt.
 * Format: one line per tool with signature and short description.
 * Returns a malloc'd string. Caller frees. */
char *tools_build_compact_catalog(void) {
    int total;
    const tool_def_t *tools = tools_get_all(&total);
    int core_n = tools_get_core_count();

    /* Two-section catalog: CORE (always pageable) + LOADABLE (via load_tools).
     * Only tool names — full schemas loaded dynamically.
     * Saves ~15KB vs old per-tool signature+description catalog. */
    size_t buflen = (size_t)total * 40 + 4096;
    char *buf = safe_malloc(buflen);
    int pos = 0;

    pos += snprintf(buf + pos, buflen - (size_t)pos,
        "\nTOOLS: %d core + %d loadable (use discover_tools/load_tools for extended)\n",
        core_n, total - core_n);

    /* Core tools: listed by name */
    pos += snprintf(buf + pos, buflen - (size_t)pos, "CORE: ");
    int first = 1;
    for (int i = 0; i < total && (size_t)pos < buflen - 64; i++) {
        if (!tools[i].core) continue;
        if (!first) pos += snprintf(buf + pos, buflen - (size_t)pos, ", ");
        pos += snprintf(buf + pos, buflen - (size_t)pos, "%s", tools[i].name);
        first = 0;
    }
    pos += snprintf(buf + pos, buflen - (size_t)pos, "\n");

    /* Loadable tools: grouped by category */
    static const char *headers[] = {
        "file_io", "git", "network", "shell", "code", "crypto",
        "swarm", "ast", "pipeline", "math", "search", "general",
        "finance", "prediction", "memory"
    };

    pos += snprintf(buf + pos, buflen - (size_t)pos, "LOADABLE: ");
    first = 1;
    for (int g = 0; g < 15; g++) {
        for (int i = 0; i < total && (size_t)pos < buflen - 64; i++) {
            if (tools[i].core) continue;
            int ig = assign_group(tools[i].name, tools[i].description);
            if (ig != g) continue;
            if (!first) pos += snprintf(buf + pos, buflen - (size_t)pos, ", ");
            pos += snprintf(buf + pos, buflen - (size_t)pos, "%s", tools[i].name);
            first = 0;
        }
    }
    (void)headers; /* suppress unused warning */
    pos += snprintf(buf + pos, buflen - (size_t)pos, "\n");

    /* External tools summary */
    if (g_external_tool_count > 0) {
        pos += snprintf(buf + pos, buflen - (size_t)pos, "EXTERNAL: ");
        for (int i = 0; i < g_external_tool_count && (size_t)pos < buflen - 64; i++) {
            if (i > 0) pos += snprintf(buf + pos, buflen - (size_t)pos, ", ");
            pos += snprintf(buf + pos, buflen - (size_t)pos, "%s", g_external_tools[i].name);
        }
        pos += snprintf(buf + pos, buflen - (size_t)pos, "\n");
    }

    return buf;
}

/* ── Build groups + centroids lazily ──────────────────────────────────── */

static void ensure_groups(void) {
    if (g_groups_built) return;

    int total;
    const tool_def_t *tools = tools_get_all(&total);

    /* Initialize groups */
    int max_gid = 0;
    for (int i = 0; GROUP_NAMES[i]; i++) max_gid = i + 1;
    g_group_count = max_gid;
    for (int g = 0; g < g_group_count; g++) {
        g_tool_groups[g].name = GROUP_NAMES[g];
        g_tool_groups[g].count = 0;
        g_tool_groups[g].has_centroid = false;
        memset(g_tool_groups[g].centroid, 0, sizeof(float) * (g_emb_dim > 0 ? g_emb_dim : 1024));
    }

    /* Assign tools to groups */
    for (int i = 0; i < total; i++) {
        if (is_core_tool(tools[i].name)) continue;
        int gid = assign_group(tools[i].name, tools[i].description);
        if (gid < 0 || gid >= g_group_count) gid = 11;
        tool_group_t *g = &g_tool_groups[gid];
        if (g->count < MAX_GROUP_TOOLS)
            g->tool_indices[g->count++] = i;
    }

    /* Compute centroids from embeddings (mean of member vectors) */
    if (g_emb_vectors && g_emb_dim > 0) {
        for (int gid = 0; gid < g_group_count; gid++) {
            tool_group_t *g = &g_tool_groups[gid];
            if (g->count == 0) continue;
            float *c = g->centroid;
            memset(c, 0, g_emb_dim * sizeof(float));
            int valid = 0;
            for (int j = 0; j < g->count; j++) {
                int ti = g->tool_indices[j];
                if (ti < g_emb_count) {
                    float *v = &g_emb_vectors[ti * g_emb_dim];
                    for (int d = 0; d < g_emb_dim; d++) c[d] += v[d];
                    valid++;
                }
            }
            if (valid > 0) {
                float inv = 1.0f / valid;
                for (int d = 0; d < g_emb_dim; d++) c[d] *= inv;
                g->has_centroid = true;
            }
        }
    }

    g_groups_built = true;
    fprintf(stderr, "  \033[2mgroups:");
    for (int g = 0; g < g_group_count; g++)
        if (g_tool_groups[g].count > 0)
            fprintf(stderr, " %s=%d", g_tool_groups[g].name, g_tool_groups[g].count);
    fprintf(stderr, "\033[0m\n");
}

/* TF-IDF fallback */
static tfidf_index_t g_tool_index;
static bool g_tool_index_built = false;

static void ensure_tool_index(void) {
    if (g_tool_index_built) return;
    int total;
    const tool_def_t *tools = tools_get_all(&total);
    if (total == 0) return;
    int n = total < SEM_MAX_DOCS ? total : SEM_MAX_DOCS;
    const char **names = safe_malloc(n * sizeof(char *));
    const char **descs = safe_malloc(n * sizeof(char *));
    for (int i = 0; i < n; i++) { names[i] = tools[i].name; descs[i] = tools[i].description; }
    sem_tools_index_build(&g_tool_index, names, descs, n);
    free(names); free(descs);
    g_tool_index_built = true;
}

/* ── Hierarchical retrieval ───────────────────────────────────────────── */

typedef struct { int idx; float score; } hrt_score_t;

static int hrt_cmp(const void *a, const void *b) {
    float sa = ((const hrt_score_t *)b)->score - ((const hrt_score_t *)a)->score;
    return sa > 0 ? 1 : sa < 0 ? -1 : 0;
}

int tools_retrieve(const char *context, int *out_indices, int max_tools) {
    int total;
    const tool_def_t *tools = tools_get_all(&total);
    if (total == 0 || max_tools <= 0) return 0;

    int n = 0;
    /* Heap-allocate included[] — scales to 10K+ tools without stack overflow */
    bool *included = safe_malloc(total * sizeof(bool));
    memset(included, 0, total * sizeof(bool));

    /* ── Slot 0: Core tools (always loaded, ~27 tools) ────────────────── */
    for (int i = 0; i < total; i++) {
        if (is_core_tool(tools[i].name)) {
            out_indices[n++] = i;
            included[i] = true;
            if (n >= max_tools) { free(included); return n; }
        }
    }

    /* ── Slot 1: Hot cache (recently used tools, LRU) ─────────────────── */
    for (int h = 0; h < g_hot_count && n < max_tools; h++) {
        int hi = g_hot_cache[h];
        if (hi >= 0 && hi < total && !included[hi]) {
            out_indices[n++] = hi;
            included[hi] = true;
        }
    }

    if (!context || !context[0]) { free(included); return n; }

    /* ── Level 1: Centroid ranking (O(K) where K=15 groups) ───────────── */
    ensure_embeddings_loaded();
    ensure_groups();

    float *qvec = NULL;
    int top_groups[MAX_GROUPS];
    int n_top_groups = 0;

    if (g_emb_vectors && g_emb_dim > 0) {
        qvec = embed_query_jina(context);
    }

    if (qvec) {
        /* Score each group centroid against query — O(K) not O(N) */
        hrt_score_t gscore[MAX_GROUPS];
        for (int g = 0; g < g_group_count; g++) {
            gscore[g].idx = g;
            gscore[g].score = g_tool_groups[g].has_centroid
                ? cosine_sim_f(qvec, g_tool_groups[g].centroid, g_emb_dim)
                : 0.0f;
        }
        qsort(gscore, g_group_count, sizeof(hrt_score_t), hrt_cmp);

        /* Take top 3 groups (or more if budget allows) */
        for (int i = 0; i < g_group_count && n_top_groups < 3; i++) {
            if (gscore[i].score > 0.05f) {
                top_groups[n_top_groups++] = gscore[i].idx;
            }
        }
        /* Always include general group if we have room */
        bool has_general = false;
        for (int i = 0; i < n_top_groups; i++) if (top_groups[i] == 11) has_general = true;
        if (!has_general && n_top_groups < 5) top_groups[n_top_groups++] = 11;

        /* ── Level 2: Intra-group embedding rank (O(k) per group) ─────── */
        int max_cand = 0;
        for (int gi = 0; gi < n_top_groups; gi++)
            max_cand += g_tool_groups[top_groups[gi]].count;
        hrt_score_t *candidates = safe_malloc(max_cand * sizeof(hrt_score_t));
        int n_cand = 0;

        for (int gi = 0; gi < n_top_groups; gi++) {
            tool_group_t *g = &g_tool_groups[top_groups[gi]];
            for (int j = 0; j < g->count; j++) {
                int ti = g->tool_indices[j];
                if (ti < 0 || ti >= total || included[ti] || ti >= g_emb_count) continue;
                candidates[n_cand].idx = ti;
                candidates[n_cand].score = cosine_sim_f(qvec, &g_emb_vectors[ti * g_emb_dim], g_emb_dim);
                n_cand++;
            }
        }

        qsort(candidates, n_cand, sizeof(hrt_score_t), hrt_cmp);

        /* Fill remaining slots */
        float threshold = 0.12f;
        for (int i = 0; i < n_cand && n < max_tools; i++) {
            int idx = candidates[i].idx;
            if (candidates[i].score > threshold && idx >= 0 && idx < total && !included[idx]) {
                out_indices[n++] = idx;
                included[idx] = true;
            }
        }

        free(candidates);
        free(qvec);
    }

    /* ── Level 3: TF-IDF fallback (no Jina key, or sparse results) ────── */
    if (n < max_tools / 2) {
        ensure_tool_index();
        int capped = total < SEM_MAX_DOCS ? total : SEM_MAX_DOCS;
        tool_score_t *ranked = safe_malloc(capped * sizeof(tool_score_t));
        int ranked_count = sem_tools_rank(&g_tool_index, context, ranked, capped, capped);
        for (int i = 0; i < ranked_count && n < max_tools; i++) {
            int idx = ranked[i].tool_index;
            if (idx >= 0 && idx < total && !included[idx] && ranked[i].score > 0.05) {
                out_indices[n++] = idx;
                included[idx] = true;
            }
        }
        free(ranked);
    }

    free(included);
    return n;
}

const tool_def_t **tools_get_filtered(const char *context, int max_tools, int *out_count) {
    int cap = max_tools > 0 ? max_tools : 128;
    int *indices = safe_malloc(cap * sizeof(int));
    int n = tools_retrieve(context, indices, cap);

    int total;
    const tool_def_t *all = tools_get_all(&total);

    const tool_def_t **result = safe_malloc((n > 0 ? n : 1) * sizeof(tool_def_t *));
    for (int i = 0; i < n; i++) {
        result[i] = &all[indices[i]];
    }
    free(indices);
    *out_count = n;
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DYNAMIC TOOL PAGING — Hints, Co-occurrence, Tiered Retrieval
 *
 * Three-tier architecture for cache-aware tool serialization:
 *   Tier 0 (Pinned):    Core + hint-pinned. Stable across turns → cached.
 *   Tier 1 (Working):   Hot LRU + cooc-predicted + centroid-matched. Slow-evolving.
 *   Tier 2 (Discovery): Fresh TF-IDF matches. Volatile each turn.
 *
 * Cache breakpoint between Tier 1 and Tier 2 ensures the stable prefix
 * (70+ tools) gets prompt-cached while the volatile tail (≤10 tools) is cheap.
 *
 * Research basis:
 *   - Pichay 2026: demand paging → 37% token reduction
 *   - W&D 2026: parallel tool calls → 36% cost, 41% latency
 *   - Lost-in-Middle: position-aware ordering → 20% accuracy
 *   - Prompt Caching study: stable prefix → 78% cost reduction
 *   - BAVT 2026: budget-adaptive exploitation as resources deplete
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Hint accumulator ─────────────────────────────────────────────────── */

static tool_hint_t g_hints[MAX_HINTS];
static int g_hint_count = 0;
static int g_hint_turn = 0;

void tools_hint_init(void) {
    memset(g_hints, 0, sizeof(g_hints));
    g_hint_count = 0;
    g_hint_turn = 0;
}

/* LCFU eviction score: lower = more evictable.
 * LCFU = log(freq+1) * weight * source_priority / age
 * Captures: how often reinforced, current weight, source importance, staleness */
static float hint_lcfu_score(const tool_hint_t *h) {
    int age = g_hint_turn - h->turn_created;
    if (age < 1) age = 1;
    /* Source priority: USER > PLAN > SWARM > CONV > TOOL */
    static const float src_pri[] = {
        [HINT_USER]  = 2.0f,
        [HINT_CONV]  = 0.8f,
        [HINT_PLAN]  = 1.5f,
        [HINT_TOOL]  = 0.6f,
        [HINT_SWARM] = 1.2f,
    };
    float pri = (h->source <= HINT_SWARM) ? src_pri[h->source] : 1.0f;
    return logf((float)(h->group_count + h->tool_count + 1)) * h->weight * pri / (float)age;
}

void tools_hint_add(const tool_hint_t *h) {
    /* Check for merge: if same domain exists, reinforce instead of adding */
    for (int i = 0; i < g_hint_count; i++) {
        if (h->domain[0] && g_hints[i].domain[0] &&
            strcasecmp(g_hints[i].domain, h->domain) == 0) {
            /* Reinforce: boost weight, merge groups */
            g_hints[i].weight = fminf(g_hints[i].weight + h->weight * 0.5f, 2.0f);
            for (int gi = 0; gi < h->group_count; gi++) {
                bool dup = false;
                for (int j = 0; j < g_hints[i].group_count; j++)
                    if (g_hints[i].groups[j] == h->groups[gi]) { dup = true; break; }
                if (!dup && g_hints[i].group_count < HINT_MAX_GROUPS)
                    g_hints[i].groups[g_hints[i].group_count++] = h->groups[gi];
            }
            for (int ti = 0; ti < h->tool_count; ti++) {
                bool dup = false;
                for (int j = 0; j < g_hints[i].tool_count; j++)
                    if (strcmp(g_hints[i].tools[j], h->tools[ti]) == 0) { dup = true; break; }
                if (!dup && g_hints[i].tool_count < HINT_MAX_TOOLS)
                    snprintf(g_hints[i].tools[g_hints[i].tool_count++], 64, "%s", h->tools[ti]);
            }
            return;
        }
    }

    if (g_hint_count >= MAX_HINTS) {
        /* LCFU eviction: evict hint with lowest composite score */
        int min_i = 0;
        float min_score = hint_lcfu_score(&g_hints[0]);
        for (int i = 1; i < g_hint_count; i++) {
            float s = hint_lcfu_score(&g_hints[i]);
            if (s < min_score) { min_score = s; min_i = i; }
        }
        /* Only evict if new hint scores higher */
        if (hint_lcfu_score(h) > min_score)
            g_hints[min_i] = *h;
        return;
    }
    g_hints[g_hint_count++] = *h;
}

void tools_hint_decay(void) {
    g_hint_turn++;
    int alive = 0;
    for (int i = 0; i < g_hint_count; i++) {
        int age = g_hint_turn - g_hints[i].turn_created;
        if (g_hints[i].ttl_turns > 0 && age >= g_hints[i].ttl_turns) continue;
        /* Momentum decay: high-weight hints (reinforced) decay slower.
         * Base rate 0.85, but hints >1.0 weight use 0.92 (stickier). */
        float decay = (g_hints[i].weight > 1.0f) ? 0.92f : 0.85f;
        /* Source-dependent: USER hints are stickier than auto-generated */
        if (g_hints[i].source == HINT_USER) decay = 0.95f;
        g_hints[i].weight *= decay;
        if (g_hints[i].weight < 0.05f) continue;
        if (alive != i) g_hints[alive] = g_hints[i];
        alive++;
    }
    g_hint_count = alive;
}

void tools_hint_clear(void) { g_hint_count = 0; }
int  tools_hint_count(void)  { return g_hint_count; }

/* Group name → index lookup for hint matching (used by /hint command) */
static int group_name_to_idx(const char *name) __attribute__((unused));
static int group_name_to_idx(const char *name) {
    for (int i = 0; GROUP_NAMES[i]; i++)
        if (strcasecmp(name, GROUP_NAMES[i]) == 0) return i;
    return -1;
}

void tools_hint_add_user(const char *input) {
    if (!input || !input[0]) return;

    tool_hint_t h = {0};
    h.weight = 0.8f;
    h.ttl_turns = HINT_DEFAULT_TTL;
    h.source = HINT_CONV;
    h.turn_created = g_hint_turn;

    static const struct { const char *kw; int group; } KW_MAP[] = {
        {"trading", 12}, {"trade", 12}, {"stock", 12}, {"kalshi", 13},
        {"polymarket", 13}, {"predict", 13}, {"bet", 13}, {"wager", 13},
        {"alpha vantage", 12}, {"financial", 12}, {"portfolio", 12},
        {"kubernetes", 3}, {"k8s", 3}, {"docker", 3}, {"container", 3},
        {"deploy", 3}, {"terraform", 3},
        {"encrypt", 5}, {"decrypt", 5}, {"signing", 5}, {"certificate", 5},
        {"hash", 5}, {"jwt", 5},
        {"github", 1}, {"pull request", 1}, {"commit", 1}, {"merge", 1},
        {"branch", 1}, {"rebase", 1},
        {"swarm", 6}, {"agent", 6}, {"topology", 6}, {"spawn", 6},
        {"memory", 14}, {"soul", 14}, {"remember", 14},
        {"scrape", 2}, {"crawl", 2}, {"api", 2}, {"webhook", 2},
        {"slack", 2}, {"discord", 2}, {"notion", 2},
        {NULL, 0}
    };

    bool found = false;
    for (int k = 0; KW_MAP[k].kw; k++) {
        const char *kw = KW_MAP[k].kw;
        size_t kwlen = strlen(kw);
        for (const char *p = input; *p; p++) {
            if (strncasecmp(p, kw, kwlen) == 0) {
                /* Word boundary check */
                if (p > input && ((unsigned char)p[-1] >= 'a' && (unsigned char)p[-1] <= 'z'))
                    continue;
                char after = p[kwlen];
                if ((unsigned char)after >= 'a' && (unsigned char)after <= 'z')
                    continue;
                int gid = KW_MAP[k].group;
                bool dup = false;
                for (int i = 0; i < h.group_count; i++)
                    if (h.groups[i] == gid) { dup = true; break; }
                if (!dup && h.group_count < HINT_MAX_GROUPS) {
                    h.groups[h.group_count++] = gid;
                    found = true;
                }
                if (!h.domain[0])
                    snprintf(h.domain, sizeof(h.domain), "%s", kw);
                break;
            }
        }
    }

    /* Also check group names directly */
    for (int g = 0; GROUP_NAMES[g]; g++) {
        const char *gn = GROUP_NAMES[g];
        size_t gnlen = strlen(gn);
        for (const char *p = input; *p; p++) {
            if (strncasecmp(p, gn, gnlen) == 0) {
                bool dup = false;
                for (int i = 0; i < h.group_count; i++)
                    if (h.groups[i] == g) { dup = true; break; }
                if (!dup && h.group_count < HINT_MAX_GROUPS) {
                    h.groups[h.group_count++] = g;
                    found = true;
                }
                break;
            }
        }
    }

    if (found) tools_hint_add(&h);
}

/* ── Co-occurrence matrix ─────────────────────────────────────────────── */

#define COOC_MAX_TOOLS 512
#define COOC_MAGIC     0x434F4F43u  /* "COOC" */
#define COOC_VERSION   1

typedef struct {
    uint16_t matrix[COOC_MAX_TOOLS][COOC_MAX_TOOLS];
    char     names[COOC_MAX_TOOLS][64];
    int      name_count;
} tool_cooc_internal_t;

static tool_cooc_internal_t *g_cooc = NULL;

void tools_cooc_init(void) {
    if (g_cooc) return;
    g_cooc = safe_malloc(sizeof(tool_cooc_internal_t));
    memset(g_cooc, 0, sizeof(tool_cooc_internal_t));
}

void tools_cooc_free(void) {
    free(g_cooc);
    g_cooc = NULL;
}

static int cooc_name_idx(const char *name) {
    if (!g_cooc) return -1;
    for (int i = 0; i < g_cooc->name_count; i++)
        if (strcmp(g_cooc->names[i], name) == 0) return i;
    if (g_cooc->name_count >= COOC_MAX_TOOLS) return -1;
    int idx = g_cooc->name_count++;
    snprintf(g_cooc->names[idx], 64, "%s", name);
    return idx;
}

void tools_cooc_update(const char **tool_names, int n) {
    if (!g_cooc || n <= 1) return;
    int indices[64];
    int valid = 0;
    for (int i = 0; i < n && valid < 64; i++) {
        int idx = cooc_name_idx(tool_names[i]);
        if (idx >= 0) indices[valid++] = idx;
    }
    for (int i = 0; i < valid; i++)
        for (int j = 0; j < valid; j++) {
            if (i == j) continue;
            if (g_cooc->matrix[indices[i]][indices[j]] < UINT16_MAX)
                g_cooc->matrix[indices[i]][indices[j]]++;
        }
}

/* Predict top-K successor tools from co-occurrence row.
 * Returns count of predicted s_tools[] indices written to out. */
static int cooc_predict_internal(const char *tool_name, int *out_tool_indices, int max) {
    if (!g_cooc || max <= 0) return 0;
    int ri = -1;
    for (int i = 0; i < g_cooc->name_count; i++)
        if (strcmp(g_cooc->names[i], tool_name) == 0) { ri = i; break; }
    if (ri < 0) return 0;

    /* Find top-K by count in this row */
    typedef struct { int ci; uint16_t count; } cpair_t;
    cpair_t top[16];
    int nt = 0;
    int cap = max < 16 ? max : 16;

    for (int j = 0; j < g_cooc->name_count; j++) {
        uint16_t c = g_cooc->matrix[ri][j];
        if (c == 0) continue;
        if (nt < cap) {
            top[nt].ci = j;
            top[nt].count = c;
            nt++;
        } else {
            int mi = 0;
            for (int k = 1; k < nt; k++)
                if (top[k].count < top[mi].count) mi = k;
            if (c > top[mi].count) {
                top[mi].ci = j;
                top[mi].count = c;
            }
        }
    }

    /* Map cooc names back to s_tools[] indices */
    int result = 0;
    for (int i = 0; i < nt; i++) {
        int ti = tool_map_lookup(&g_tool_map, g_cooc->names[top[i].ci]);
        if (ti >= 0) out_tool_indices[result++] = ti;
    }
    return result;
}

void tools_cooc_persist(void) {
    if (!g_cooc || g_cooc->name_count == 0) return;
    char path[256];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.dsco/tool_cooc.bin", home);

    FILE *f = fopen(path, "wb");
    if (!f) return;

    uint32_t magic = COOC_MAGIC;
    uint32_t version = COOC_VERSION;
    uint32_t nc = (uint32_t)g_cooc->name_count;

    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&nc, 4, 1, f);
    fwrite(g_cooc->names, 64, nc, f);
    for (uint32_t i = 0; i < nc; i++)
        fwrite(g_cooc->matrix[i], 2, nc, f);
    fclose(f);
}

void tools_cooc_load(void) {
    tools_cooc_init();
    char path[256];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.dsco/tool_cooc.bin", home);

    FILE *f = fopen(path, "rb");
    if (!f) return;

    uint32_t magic, version, nc;
    if (fread(&magic, 4, 1, f) != 1 || magic != COOC_MAGIC) { fclose(f); return; }
    if (fread(&version, 4, 1, f) != 1 || version != COOC_VERSION) { fclose(f); return; }
    if (fread(&nc, 4, 1, f) != 1 || nc > COOC_MAX_TOOLS) { fclose(f); return; }

    if (fread(g_cooc->names, 64, nc, f) != nc) { fclose(f); return; }
    g_cooc->name_count = (int)nc;
    for (uint32_t i = 0; i < nc; i++) {
        if (fread(g_cooc->matrix[i], 2, nc, f) != nc) break;
    }
    fclose(f);
    fprintf(stderr, "  \033[2mcooc: loaded %d tool pairs\033[0m\n", (int)nc);
}

/* ── Tiered retrieval: register-file model with multi-signal quorum ──── */

quorum_telemetry_t g_quorum_telemetry = {0};

tool_page_result_t tools_get_paged(const char *context, int max_tools,
                                    float budget_ratio) {
    tool_page_result_t r = {0};
    double t0 = now_ms();
    int cooc_added = 0, centroid_added = 0;
    int total;
    const tool_def_t *tools = tools_get_all(&total);
    if (total == 0 || max_tools <= 0) return r;

    /* Hard cap: register file is 64 slots max, period. */
    if (max_tools > TOOL_REGISTER_CAP) max_tools = TOOL_REGISTER_CAP;

    /* --cheap mode: force critical budget -> only ALWAYS core tools */
    if (g_cheap_mode) {
        budget_ratio = 0.0f;
    }

    /* Virtual context pressure: if the context window is known, penalize
     * the budget ratio when context is filling up. Each tool schema costs
     * ~200 tokens; at 64 tools that's ~12,800 tokens (6.4% of 200k).
     * When context is >70% full, tool schemas are competing with actual
     * conversation — tighten the register file proportionally.
     * This is the unified "page allocator" — cost AND context pressure
     * both drive the same eviction curve. */
    float effective_ratio = budget_ratio;
    if (g_ctx_window_tokens > 0) {
        /* Estimate context usage from page telemetry or session state.
         * We don't have direct access to session here, so use a heuristic:
         * each tool at ~200 tokens, and the context window is known. */
        float tool_schema_fraction = (max_tools * 200.0f) / g_ctx_window_tokens;
        /* If tools alone would eat >10% of context, start tightening */
        if (tool_schema_fraction > 0.10f) {
            float pressure = (tool_schema_fraction - 0.10f) / 0.10f;
            if (pressure > 1.0f) pressure = 1.0f;
            effective_ratio *= (1.0f - pressure * 0.5f);  /* up to 50% penalty */
        }
    }

    /* Register-file slot allocation:
     * ALWAYS:   bash, python, discover/load, exit, loop constructs
     * WARM:     file I/O, evictable under budget pressure
     * WORKING:  quorum-scored / hint-pinned, turn-volatile
     * DISCOVERY: progressive schema, ephemeral
     *
     * Budget-adaptive shrinking (uses effective_ratio = cost + context pressure):
     *   Full   (>0.4):    7 + 11 + 10 + 4 = 32
     *   Mid  (0.15-0.4):  7 +  8 +  8 + 3 = 26
     *   Low  (0.05-0.15): 7 +  4 +  4 + 0 = 15 (~17 with hint-pins)
     *   Critical (<0.05): 7 +  0 +  0 + 0 =  7 */
    int always_budget = TOOL_REG_ALWAYS;
    int warm_budget, working_budget, discovery_budget;

    if (always_budget > max_tools) always_budget = max_tools;

    if (effective_ratio < 0.05f) {
        /* Critical: ALWAYS core only (bash + python + meta) */
        warm_budget = 0;
        working_budget = 0;
        discovery_budget = 0;
    } else if (effective_ratio < 0.15f) {
        /* Low: some file I/O + small working set */
        warm_budget = 4;
        working_budget = 4;
        discovery_budget = 0;
    } else if (effective_ratio < 0.4f) {
        /* Mid: most warm + working, small discovery */
        warm_budget = 8;
        working_budget = 8;
        discovery_budget = 3;
    } else {
        /* Full: all register banks active */
        warm_budget = TOOL_REG_WARM;
        working_budget = TOOL_REG_WORKING;
        discovery_budget = TOOL_REG_DISCOVERY;
    }

    /* Clamp total to max_tools — shrink discovery first, then working, then warm */
    int total_budget = always_budget + warm_budget + working_budget + discovery_budget;
    if (total_budget > max_tools) {
        int excess = total_budget - max_tools;
        int d_cut = excess < discovery_budget ? excess : discovery_budget;
        discovery_budget -= d_cut; excess -= d_cut;
        int w_cut = excess < working_budget ? excess : working_budget;
        working_budget -= w_cut; excess -= w_cut;
        if (excess > 0) warm_budget -= excess;
    }
    if (warm_budget < 0) warm_budget = 0;
    if (working_budget < 0) working_budget = 0;
    if (discovery_budget < 0) discovery_budget = 0;

    /* Reserve extra pinned slots for hint-pinned tools (steal from working) */
    int hint_extra = (g_hint_count > 0 && working_budget > 0) ? 2 : 0;
    if (hint_extra > working_budget) hint_extra = working_budget;
    working_budget -= hint_extra;
    int pinned_budget = always_budget + warm_budget + hint_extra;

    bool *included = safe_malloc(total * sizeof(bool));
    memset(included, 0, total * sizeof(bool));

    /* Multi-signal quorum: track how many independent scoring methods
     * recommend each tool. Tools need >= QUORUM_MIN_SIGNALS to enter
     * the working set. This prevents speculative single-signal tools
     * from wasting register slots. */
    int *tool_signals = safe_malloc(total * sizeof(int));
    memset(tool_signals, 0, total * sizeof(int));

    /* ── Tier 0: Pinned (ALWAYS core + WARM core + hint-pinned) ────────── */
    int *pidx = safe_malloc((pinned_budget + 1) * sizeof(int));
    int pn = 0;

    /* ALWAYS core: hardwired, never evicted */
    for (int i = 0; i < total && pn < always_budget; i++) {
        if (is_core_always(tools[i].name)) {
            pidx[pn++] = i;
            included[i] = true;
        }
    }

    /* WARM core: evictable under budget pressure */
    for (int i = 0; i < total && pn < always_budget + warm_budget; i++) {
        if (is_core_warm(tools[i].name) && !included[i]) {
            pidx[pn++] = i;
            included[i] = true;
        }
    }

    /* Hint-pinned: specific tools named in active hints */
    for (int hi = 0; hi < g_hint_count && pn < pinned_budget; hi++) {
        for (int t = 0; t < g_hints[hi].tool_count && pn < pinned_budget; t++) {
            int idx = tool_map_lookup(&g_tool_map, g_hints[hi].tools[t]);
            if (idx >= 0 && idx < total && !included[idx]) {
                pidx[pn++] = idx;
                included[idx] = true;
            }
        }
    }

    /* ── Tier 1: Working set with multi-signal quorum ──────────────────── *
     * Four independent signals vote on each candidate:
     *   1. Hot cache (recency — tool was recently executed)
     *   2. Co-occurrence (usage pattern — frequently follows current tools)
     *   3. Embedding similarity (semantic match to conversation context)
     *   4. Hint-group membership (user/plan/conversation intent)
     *
     * Hot cache tools are auto-admitted (they were literally just used).
     * All other candidates need >= QUORUM_MIN_SIGNALS to enter. */
    int *widx = safe_malloc((working_budget + 1) * sizeof(int));
    int wn = 0;
    int hot_admitted = 0;
    double q0 = now_ms();

    /* Signal 1: Hot cache — auto-admit (recently executed = always relevant) */
    for (int h = 0; h < g_hot_count && wn < working_budget; h++) {
        int hi = g_hot_cache[h];
        if (hi >= 0 && hi < total && !included[hi]) {
            tool_signals[hi] += 10; /* sentinel: hot cache bypasses quorum */
            widx[wn++] = hi;
            included[hi] = true;
            hot_admitted++;
        }
    }

    /* Signal 2: Co-occurrence predictions — count signal, tentatively add.
     * Only consider core tools (non-core need explicit load_tools). */
    if (g_cooc) {
        for (int h = 0; h < g_hot_count; h++) {
            int hi = g_hot_cache[h];
            if (hi < 0 || hi >= total) continue;
            int predicted[8];
            int np = cooc_predict_internal(tools[hi].name, predicted, 8);
            for (int p = 0; p < np; p++) {
                int ti = predicted[p];
                if (ti >= 0 && ti < total && tools[ti].core) {
                    tool_signals[ti]++;
                    if (!included[ti] && wn < working_budget) {
                        widx[wn++] = ti;
                        included[ti] = true;
                        cooc_added++;
                    }
                }
            }
        }
    }

    /* Signal 3: Embedding/centroid or TF-IDF — count signal, tentatively add */
    if (context && context[0]) {
        ensure_embeddings_loaded();
        ensure_groups();

        float *qvec = NULL;
        if (g_emb_vectors && g_emb_dim > 0)
            qvec = embed_query_jina(context);

        if (qvec) {
            hrt_score_t gscore[MAX_GROUPS];
            for (int g = 0; g < g_group_count; g++) {
                gscore[g].idx = g;
                gscore[g].score = g_tool_groups[g].has_centroid
                    ? cosine_sim_f(qvec, g_tool_groups[g].centroid, g_emb_dim)
                    : 0.0f;
                /* Hint boost: active hints bias group scores */
                for (int hi = 0; hi < g_hint_count; hi++)
                    for (int gi = 0; gi < g_hints[hi].group_count; gi++)
                        if (g_hints[hi].groups[gi] == g)
                            gscore[g].score += g_hints[hi].weight * 0.5f;
            }
            qsort(gscore, g_group_count, sizeof(hrt_score_t), hrt_cmp);

            int top_groups[MAX_GROUPS];
            int n_top = 0;
            for (int i = 0; i < g_group_count && n_top < 5; i++)
                if (gscore[i].score > 0.05f)
                    top_groups[n_top++] = gscore[i].idx;
            /* Always include general group */
            bool has_gen = false;
            for (int i = 0; i < n_top; i++) if (top_groups[i] == 11) has_gen = true;
            if (!has_gen && n_top < 6) top_groups[n_top++] = 11;

            /* Intra-group embedding rank */
            int max_cand = 0;
            for (int gi = 0; gi < n_top; gi++)
                max_cand += g_tool_groups[top_groups[gi]].count;
            hrt_score_t *cands = safe_malloc((max_cand + 1) * sizeof(hrt_score_t));
            int nc = 0;

            for (int gi = 0; gi < n_top; gi++) {
                tool_group_t *grp = &g_tool_groups[top_groups[gi]];
                for (int j = 0; j < grp->count; j++) {
                    int ti = grp->tool_indices[j];
                    if (ti < 0 || ti >= total || ti >= g_emb_count) continue;
                    if (!tools[ti].core) continue; /* only core tools for embedding paging */
                    float sim = cosine_sim_f(qvec, &g_emb_vectors[ti * g_emb_dim], g_emb_dim);
                    if (sim > 0.12f) {
                        tool_signals[ti]++;
                        cands[nc].idx = ti;
                        cands[nc].score = sim;
                        nc++;
                    }
                }
            }
            qsort(cands, nc, sizeof(hrt_score_t), hrt_cmp);

            for (int i = 0; i < nc && wn < working_budget; i++) {
                if (!included[cands[i].idx]) {
                    widx[wn++] = cands[i].idx;
                    included[cands[i].idx] = true;
                    centroid_added++;
                }
            }

            free(cands);
            free(qvec);
        } else {
            /* No Jina key — TF-IDF fallback for working set */
            ensure_tool_index();
            int capped = total < SEM_MAX_DOCS ? total : SEM_MAX_DOCS;
            tool_score_t *ranked = safe_malloc(capped * sizeof(tool_score_t));
            int ranked_count = sem_tools_rank(&g_tool_index, context, ranked, capped, capped);
            for (int i = 0; i < ranked_count; i++) {
                int idx = ranked[i].tool_index;
                if (idx >= 0 && idx < total && ranked[i].score > 0.05 && tools[idx].core) {
                    tool_signals[idx]++;
                    if (!included[idx] && wn < working_budget) {
                        widx[wn++] = idx;
                        included[idx] = true;
                    }
                }
            }
            free(ranked);
        }
    }

    /* Signal 4: Hint-group membership — add signal for tools whose group
     * matches an active hint (user intent, plan, conversation context) */
    for (int i = 0; i < wn; i++) {
        int ti = widx[i];
        int grp = assign_group(tools[ti].name, tools[ti].description);
        for (int hi = 0; hi < g_hint_count; hi++) {
            for (int gi = 0; gi < g_hints[hi].group_count; gi++) {
                if (g_hints[hi].groups[gi] == grp) {
                    tool_signals[ti]++;
                    goto hint_counted;
                }
            }
        }
        hint_counted:;
    }

    /* ── Quorum eviction: remove tools with < QUORUM_MIN_SIGNALS ──────── *
     * Hot cache tools are exempt (sentinel value >= 10).
     * This is the key cost control: a tool recommended by ONLY one method
     * (e.g. only TF-IDF, or only co-occurrence) gets evicted. Tools need
     * agreement from multiple independent signals to justify a register slot. */
    int quorum_vetoed = 0;
    {
        int new_wn = 0;
        for (int i = 0; i < wn; i++) {
            int ti = widx[i];
            if (tool_signals[ti] >= 10 ||  /* hot cache: always keep */
                tool_signals[ti] >= QUORUM_MIN_SIGNALS) {
                widx[new_wn++] = widx[i];
            } else {
                included[ti] = false;  /* un-include: available for discovery */
                quorum_vetoed++;
            }
        }
        wn = new_wn;
    }

    double quorum_ms = now_ms() - q0;

    /* Record quorum telemetry */
    g_quorum_telemetry.candidates_scored = hot_admitted + cooc_added + centroid_added;
    g_quorum_telemetry.quorum_admitted = wn;
    g_quorum_telemetry.quorum_vetoed = quorum_vetoed;
    g_quorum_telemetry.signal_hot = hot_admitted;
    g_quorum_telemetry.signal_cooc = cooc_added;
    g_quorum_telemetry.signal_embed = centroid_added;
    g_quorum_telemetry.signal_hint = g_hint_count;
    g_quorum_telemetry.quorum_ms = quorum_ms;

    free(tool_signals);

    /* ── Tier 2: Discovery (volatile TF-IDF matches, progressive schema) ─ */
    int *didx = safe_malloc((discovery_budget + 1) * sizeof(int));
    int dn = 0;

    if (discovery_budget > 0 && context && context[0]) {
        ensure_tool_index();
        int capped = total < SEM_MAX_DOCS ? total : SEM_MAX_DOCS;
        tool_score_t *ranked = safe_malloc(capped * sizeof(tool_score_t));
        int ranked_count = sem_tools_rank(&g_tool_index, context, ranked, capped, capped);
        for (int i = 0; i < ranked_count && dn < discovery_budget; i++) {
            int idx = ranked[i].tool_index;
            if (idx >= 0 && idx < total && !included[idx] && ranked[i].score > 0.05 && tools[idx].core) {
                didx[dn++] = idx;
                included[idx] = true;
            }
        }
        free(ranked);
    }

    free(included);

    /* Build result — position-aware: most relevant first/last,
     * least relevant in middle (Lost-in-Middle mitigation).
     * Apply agent profile filter if active. */
    r.pinned = safe_malloc((pn > 0 ? pn : 1) * sizeof(tool_def_t *));
    r.pinned_count = 0;
    for (int i = 0; i < pn; i++) {
        if (!g_profile_filter || g_profile_filter(tools[pidx[i]].name, NULL))
            r.pinned[r.pinned_count++] = &tools[pidx[i]];
    }

    r.working = safe_malloc((wn > 0 ? wn : 1) * sizeof(tool_def_t *));
    r.working_count = 0;
    for (int i = 0; i < wn; i++) {
        if (!g_profile_filter || g_profile_filter(tools[widx[i]].name, NULL))
            r.working[r.working_count++] = &tools[widx[i]];
    }

    r.discovery = safe_malloc((dn > 0 ? dn : 1) * sizeof(tool_def_t *));
    r.discovery_count = 0;
    for (int i = 0; i < dn; i++) {
        if (!g_profile_filter || g_profile_filter(tools[didx[i]].name, NULL))
            r.discovery[r.discovery_count++] = &tools[didx[i]];
    }

    free(pidx);
    free(widx);
    free(didx);

    /* Record paging telemetry */
    g_page_telemetry.pinned_count = r.pinned_count;
    g_page_telemetry.working_count = r.working_count;
    g_page_telemetry.discovery_count = r.discovery_count;
    g_page_telemetry.hint_count = g_hint_count;
    g_page_telemetry.cooc_predictions = cooc_added;
    g_page_telemetry.centroid_matches = centroid_added;
    g_page_telemetry.budget_ratio = budget_ratio;
    g_page_telemetry.retrieval_ms = now_ms() - t0;
    /* Token savings: progressive schema (~200/tool) + quorum eviction (~200/tool) */
    g_page_telemetry.schema_tokens_saved =
        r.discovery_count * 200 + quorum_vetoed * 200;

    return r;
}

void tool_page_result_free(tool_page_result_t *r) {
    free((void *)r->pinned);
    free((void *)r->working);
    free((void *)r->discovery);
    memset(r, 0, sizeof(*r));
}

/* ── Per-turn paging telemetry ─────────────────────────────────────────── */

page_telemetry_t g_page_telemetry = {0};

/* ── API quorum gate (opt-in: DSCO_QUORUM_GATE=1) ─────────────────────── *
 * Fires a synchronous request to a cheap model (haiku) asking which tool
 * groups are relevant for the current context. Results injected as
 * HINT_PLAN hints, which boost those groups in the next tools_get_paged()
 * call. Cost: ~$0.0004/call at haiku pricing. Saves $0.05-0.15/turn on
 * the main model by reducing tools loaded. */

/* GROUP_NAMES[] already defined above in ensure_groups() */

static size_t quorum_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    jbuf_t *b = (jbuf_t *)userdata;
    size_t n = size * nmemb;
    jbuf_append_len(b, (const char *)ptr, n);
    return n;
}

void tool_quorum_gate_api(const char *context, const char *api_key) {
    if (!context || !context[0] || !api_key || !api_key[0]) return;
    const char *enabled = getenv("DSCO_QUORUM_GATE");
    if (!enabled || enabled[0] != '1') return;

    double t0 = now_ms();

    /* Build a minimal prompt: just group names, ask for relevant ones */
    jbuf_t req;
    jbuf_init(&req, 2048);
    jbuf_append(&req, "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":256,\"messages\":[{\"role\":\"user\",\"content\":");
    char prompt[2048];
    snprintf(prompt, sizeof(prompt),
        "Given this task context, return ONLY a JSON array of relevant tool group names. "
        "Available groups: file_io, git, network, shell, code, crypto, swarm, ast, "
        "pipeline, math, search, general, market, prediction, memory.\\n\\n"
        "Context: %.1024s\\n\\nReturn ONLY the JSON array, e.g. [\\\"file_io\\\",\\\"git\\\"]",
        context);
    jbuf_append_json_str(&req, prompt);
    jbuf_append(&req, "}]}");

    /* Fire synchronous request to haiku */
    CURL *curl = curl_easy_init();
    if (!curl) { jbuf_free(&req); return; }

    jbuf_t resp;
    jbuf_init(&resp, 4096);

    struct curl_slist *hdrs = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, API_URL_ANTHROPIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, quorum_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);  /* fast or nothing */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    jbuf_free(&req);

    if (res != CURLE_OK || http_code != 200 || !resp.data) {
        jbuf_free(&resp);
        return;
    }

    /* Parse response: find the text content, extract group names */
    const char *text = strstr(resp.data, "\"text\":\"");
    if (!text) { jbuf_free(&resp); return; }
    text += 8;

    /* Inject matching groups as HINT_PLAN hints */
    tool_hint_t h = {0};
    snprintf(h.domain, sizeof(h.domain), "quorum_gate");
    h.weight = 0.6f;
    h.ttl_turns = 3;
    h.source = HINT_PLAN;
    h.turn_created = g_hint_turn;

    for (int g = 0; GROUP_NAMES[g]; g++) {
        if (strstr(text, GROUP_NAMES[g])) {
            if (h.group_count < HINT_MAX_GROUPS)
                h.groups[h.group_count++] = g;
        }
    }

    if (h.group_count > 0) {
        tools_hint_add(&h);
        double gate_ms = now_ms() - t0;
        fprintf(stderr, "  \033[2mquorum gate: %d groups selected in %.0fms\033[0m\n",
                h.group_count, gate_ms);
    }

    jbuf_free(&resp);
}

/* ── Co-occurrence → Hint bridge ───────────────────────────────────────── */

void tools_cooc_inject_hints(const char **tool_names, int n) {
    if (!g_cooc || n < 1) return;

    /* For each tool just executed, predict top-3 successors */
    for (int i = 0; i < n; i++) {
        int predicted[3];
        int np = cooc_predict_internal(tool_names[i], predicted, 3);
        if (np == 0) continue;

        /* Build a HINT_TOOL hint with predicted tool names */
        int total;
        const tool_def_t *all = tools_get_all(&total);

        tool_hint_t h = {0};
        snprintf(h.domain, sizeof(h.domain), "cooc:%s", tool_names[i]);
        h.weight = 0.4f; /* Lower than user/conv hints — speculative */
        h.ttl_turns = 3;  /* Short-lived: only relevant for next few turns */
        h.source = HINT_TOOL;
        h.turn_created = g_hint_turn;

        for (int j = 0; j < np && h.tool_count < HINT_MAX_TOOLS; j++) {
            if (predicted[j] >= 0 && predicted[j] < total)
                snprintf(h.tools[h.tool_count++], 64, "%s", all[predicted[j]].name);
        }

        if (h.tool_count > 0) tools_hint_add(&h);
    }
}

/* ── Co-occurrence temporal decay ──────────────────────────────────────── */

void tools_cooc_decay(float factor) {
    if (!g_cooc || factor <= 0.0f || factor >= 1.0f) return;
    for (int i = 0; i < g_cooc->name_count; i++) {
        for (int j = 0; j < g_cooc->name_count; j++) {
            uint16_t v = g_cooc->matrix[i][j];
            g_cooc->matrix[i][j] = (uint16_t)(v * factor);
        }
    }
}

/* ── Progressive schema check ──────────────────────────────────────────── */

bool tool_is_progressive_schema(const tool_def_t *t, const tool_page_result_t *r) {
    /* Only Tier 2 (discovery) tools get compact schema */
    if (!t || !r) return false;
    for (int i = 0; i < r->discovery_count; i++)
        if (r->discovery[i] == t) return true;
    return false;
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
    for (tool_map_entry_t *cur = m->buckets[h]; cur; cur = cur->next) {
        if (strcmp(cur->name, name) == 0) {
            cur->index = index;
            cur->name = name;
            return;
        }
    }
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
        if (!tools_profile_allows_index(i)) continue;
        tool_map_insert(&g_tool_map, s_tools[i].name, i);
    }
    if (g_tools_init_profile != TOOLS_FULL) return;
    /* Plugin tools — index as -(i+1) to distinguish from builtin */
    for (int i = 0; i < g_plugins.extra_tool_count; i++) {
        tool_map_insert(&g_tool_map, g_plugins.extra_tools[i].name, -(i + 1));
    }
    /* External tools (MCP) — index as -(10000+i) */
    for (int i = 0; i < g_external_tool_count; i++) {
        tool_map_insert(&g_tool_map, g_external_tools[i].name, -(10000 + i));
    }
}

static int tool_map_lookup_with_mcp_alias(const char *name,
                                          char *resolved_name,
                                          size_t resolved_len) {
    if (resolved_name && resolved_len > 0)
        snprintf(resolved_name, resolved_len, "%s", name ? name : "");

    int idx = tool_map_lookup(&g_tool_map, name);
    if (idx != -1 || !name) return idx;

    if (strncmp(name, "mcp_", 4) == 0 && strncmp(name, "mcp__", 5) != 0) {
        for (int i = 0; i < g_external_tool_count; i++) {
            const char *candidate = g_external_tools[i].name;
            if (!dsco_mcp_is_canonical_tool_name(candidate)) continue;
            char legacy[128];
            dsco_mcp_legacy_alias_from_canonical(candidate, legacy, sizeof(legacy));
            if (strcmp(legacy, name) == 0) {
                if (resolved_name && resolved_len > 0)
                    snprintf(resolved_name, resolved_len, "%s", candidate);
                return -(10000 + i);
            }
        }
        return -1;
    }

    if (!dsco_mcp_is_canonical_tool_name(name)) return idx;

    char legacy[128];
    dsco_mcp_legacy_alias_from_canonical(name, legacy, sizeof(legacy));
    idx = tool_map_lookup(&g_tool_map, legacy);
    if (idx != -1 && resolved_name && resolved_len > 0)
        snprintf(resolved_name, resolved_len, "%s", legacy);
    return idx;
}

/* ── External tool registry (MCP, etc.) ────────────────────────────────── */

external_tool_t g_external_tools[MAX_EXTERNAL_TOOLS];
int             g_external_tool_count = 0;

/* Serializes writers (tools_register_external, reset_external_tools).
 * Readers iterate g_external_tool_count without locking — the release-store
 * below pairs with a natural acquire-load on aligned ints, so any reader that
 * sees the new count is guaranteed to see a fully-initialized entry. */
static pthread_mutex_t g_external_tools_mu = PTHREAD_MUTEX_INITIALIZER;

void tools_register_external(const char *name, const char *description,
                                const char *input_schema_json,
                                external_tool_cb cb, void *ctx) {
    if (!name || !name[0]) return;
    pthread_mutex_lock(&g_external_tools_mu);
    int current = g_external_tool_count;
    for (int i = 0; i < current; i++) {
        if (strcmp(g_external_tools[i].name, name) == 0) {
            external_tool_t *existing = &g_external_tools[i];
            snprintf(existing->description, sizeof(existing->description),
                     "%s", description ? description : "");
            char *old_schema = existing->input_schema_json;
            existing->input_schema_json = safe_strdup(input_schema_json ? input_schema_json :
                                                      "{\"type\":\"object\",\"properties\":{}}");
            existing->cb = cb;
            existing->ctx = ctx;
            tool_map_insert(&g_tool_map, existing->name, -(10000 + i));
            pthread_mutex_unlock(&g_external_tools_mu);
            free(old_schema);
            return;
        }
    }
    if (current >= MAX_EXTERNAL_TOOLS) {
        pthread_mutex_unlock(&g_external_tools_mu);
        return;
    }
    /* Fully populate entry at slot `current` BEFORE publishing the new count.
     * Readers loop `for (i = 0; i < g_external_tool_count; i++)`, so we must
     * not advertise the slot until its contents are valid. */
    external_tool_t *t = &g_external_tools[current];
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->description, sizeof(t->description), "%s", description ? description : "");
    t->input_schema_json = safe_strdup(input_schema_json ? input_schema_json :
                                       "{\"type\":\"object\",\"properties\":{}}");
    t->cb = cb;
    t->ctx = ctx;
    t->loaded = false;
    tool_map_insert(&g_tool_map, t->name, -(10000 + current));
    __atomic_store_n(&g_external_tool_count, current + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_external_tools_mu);
}

void tools_reset_external(void) {
    pthread_mutex_lock(&g_external_tools_mu);
    int n = g_external_tool_count;
    for (int i = 0; i < n; i++) {
        free(g_external_tools[i].input_schema_json);
        g_external_tools[i].input_schema_json = NULL;
    }
    __atomic_store_n(&g_external_tool_count, 0, __ATOMIC_RELEASE);
    tool_map_rebuild();
    pthread_mutex_unlock(&g_external_tools_mu);
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
            strcmp(name, "Bash") == 0 ||
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

    if (strcmp(tool_name, "bash") == 0 || strcmp(tool_name, "Bash") == 0) {
        char *cmd = json_get_str(json, "command");
        char *cwd = json_get_str(json, "cwd");
        timeout = json_get_int(json, "timeout", 120);
        if (timeout > 1000) timeout = (timeout + 999) / 1000;
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
            "Write", "Edit", "TodoWrite",
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
            "spawn_agent", "agent_kill", "create_swarm",
            "Agent", "Task", "EnterPlanMode", "ExitPlanMode",
            "StartOfLoopConstruct", "EndOfLoopConstruct",
            "spawn_executor", "spawn_provider", "create_executor_swarm", "swarm_budget",
            "ipc_send",
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

    /* §8: VFS tool result cache for deterministic tools */
    if (g_tools_vfs && name && input_json && tool_is_cacheable(name)) {
        char hash[65];
        sha256_hex((const uint8_t *)input_json, strlen(input_json), hash);
        pthread_mutex_lock(&g_locks.cache_lock);
        char *cached = vfs_cache_get(g_tools_vfs, name, hash);
        pthread_mutex_unlock(&g_locks.cache_lock);
        if (cached) {
            snprintf(result, result_len, "%s", cached);
            free(cached);
            TRACE_INFO("tool_cache_hit name=%s", name);
            return true;
        }
    }

    /* §3: Try VM computed-goto dispatch first (FNV-1a hash, O(1)).
       This is the hot path — bypasses the chained hash map entirely
       for builtin tools via the pre-built dispatch table. */
    extern vm_t g_vm;
    if (g_vm.dispatch_len > 0) {
        uint32_t h = 2166136261u;
        for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
        uint32_t bucket = h & 511;
        for (int probe = 0; probe < 512; probe++) {
            int di = g_vm.hash_buckets[bucket];
            if (di < 0) break;
            if (g_vm.dispatch[di].hash == h &&
                strcmp(g_vm.dispatch[di].name, name) == 0) {
                /* Mark hot for tool filtering (O(1) hash map lookup) */
                int hot_idx = tool_map_lookup(&g_tool_map, name);
                if (hot_idx >= 0) tools_mark_hot(hot_idx);
                struct timeval t0, t1;
                gettimeofday(&t0, NULL);
                bool ok = g_vm.dispatch[di].func(input_json, result, result_len);
                gettimeofday(&t1, NULL);
                long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
                sanitize_tool_result_inplace(result);
                g_vm.dispatches++;
                g_vm.cache_hits++;
                TRACE_INFO("tool_result name=%s ok=%d elapsed_us=%ld (vm_dispatch)", name, ok, elapsed_us);
                ctx_persist_and_truncate(name, input_json, ok, result, result_len);
                /* §8: store deterministic result in VFS cache */
                if (ok && g_tools_vfs && input_json && tool_is_cacheable(name)) {
                    char ch[65];
                    sha256_hex((const uint8_t *)input_json, strlen(input_json), ch);
                    pthread_mutex_lock(&g_locks.cache_lock);
                    vfs_cache_put(g_tools_vfs, name, ch, result, 86400);
                    pthread_mutex_unlock(&g_locks.cache_lock);
                }
                return ok;
            }
            bucket = (bucket + 1) & 511;
        }
    }

    /* Fallback: O(1) chained hash map lookup (for plugins, MCP, late-registered) */
    char resolved_name[128];
    int idx = tool_map_lookup_with_mcp_alias(name, resolved_name,
                                             sizeof(resolved_name));
    const char *exec_name = resolved_name[0] ? resolved_name : name;

    if (idx >= 0 && idx < s_tool_count) {
        /* Builtin tool */
        tools_mark_hot(idx);
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        bool ok = s_tools[idx].execute(input_json, result, result_len);
        gettimeofday(&t1, NULL);
        long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
        sanitize_tool_result_inplace(result);
        TRACE_INFO("tool_result name=%s ok=%d elapsed_us=%ld", name, ok, elapsed_us);
        ctx_persist_and_truncate(name, input_json, ok, result, result_len);
        /* §8: store deterministic result in VFS cache */
        if (ok && g_tools_vfs && input_json && tool_is_cacheable(name)) {
            char ch[65];
            sha256_hex((const uint8_t *)input_json, strlen(input_json), ch);
            pthread_mutex_lock(&g_locks.cache_lock);
            vfs_cache_put(g_tools_vfs, name, ch, result, 86400);
            pthread_mutex_unlock(&g_locks.cache_lock);
        }
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
            ctx_persist_and_truncate(name, input_json, ok, result, result_len);
            return ok;
        }
    }

    if (idx <= -10000) {
        /* External tool (MCP): index is -(10000+i) */
        int ei = -(idx + 10000);
        if (ei >= 0 && ei < g_external_tool_count && g_external_tools[ei].cb) {
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            char *ext_result = g_external_tools[ei].cb(exec_name, input_json,
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
            ctx_persist_and_truncate(name, input_json, ok, result, result_len);
            /* §8: store deterministic result in VFS cache */
            if (ok && g_tools_vfs && input_json && tool_is_cacheable(name)) {
                char ch[65];
                sha256_hex((const uint8_t *)input_json, strlen(input_json), ch);
                pthread_mutex_lock(&g_locks.cache_lock);
                vfs_cache_put(g_tools_vfs, name, ch, result, 86400);
                pthread_mutex_unlock(&g_locks.cache_lock);
            }
            return ok;
        }
    }
    for (int i = 0; i < g_plugins.extra_tool_count; i++) {
        if (strcmp(g_plugins.extra_tools[i].name, name) == 0) {
            bool ok = g_plugins.extra_tools[i].execute(input_json, result, result_len);
            sanitize_tool_result_inplace(result);
            TRACE_INFO("tool_result name=%s source=plugin ok=%d (fallback)", name, ok);
            ctx_persist_and_truncate(name, input_json, ok, result, result_len);
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
    char *normalized = tools_normalize_input(name, input_json);
    bool ok = tools_execute_internal(name, normalized ? normalized : input_json,
                                     result, result_len);
    free(normalized);
    return ok;
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

    char *normalized_input = tools_normalize_input(dispatch_name, dispatch_input);
    if (normalized_input) dispatch_input = normalized_input;

    bool ok = tools_execute_internal(dispatch_name, dispatch_input, result, result_len);
    free(normalized_input);
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
    { "agent_race",     300 },
    { "create_swarm",   300 },
    { "swarm_collect", 3660 },
    { "spawn_executor",         300 },
    { "spawn_provider",         300 },
    { "create_executor_swarm",  300 },
    { "executor_status",         30 },
    { "swarm_budget",            30 },
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

/* ── JSON schema normalization + validation before tool dispatch ──────── */

static const char *tools_schema_for_name(const char *name) {
    if (!name) return NULL;
    int idx = tool_map_lookup_with_mcp_alias(name, NULL, 0);
    if (idx >= 0 && idx < s_tool_count) {
        return s_tools[idx].input_schema_json;
    }
    if (idx <= -10000) {
        int ei = -(idx + 10000);
        if (ei >= 0 && ei < g_external_tool_count) {
            return g_external_tools[ei].input_schema_json;
        }
    }
    return NULL;
}

static const char *tools_json_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p ? p : "";
}

static const char *tools_json_skip_string(const char *p) {
    if (!p || *p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *tools_json_skip_value(const char *p) {
    p = tools_json_ws(p);
    if (!*p) return p;
    if (*p == '"') return tools_json_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p = tools_json_skip_string(p);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           !isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *tools_json_parse_string(const char *p, char **out) {
    if (out) *out = NULL;
    if (!p || *p != '"') return NULL;
    p++;
    jbuf_t b;
    jbuf_init(&b, 64);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case '"':  jbuf_append_char(&b, '"'); break;
            case '\\': jbuf_append_char(&b, '\\'); break;
            case '/':  jbuf_append_char(&b, '/'); break;
            case 'b':  jbuf_append_char(&b, '\b'); break;
            case 'f':  jbuf_append_char(&b, '\f'); break;
            case 'n':  jbuf_append_char(&b, '\n'); break;
            case 'r':  jbuf_append_char(&b, '\r'); break;
            case 't':  jbuf_append_char(&b, '\t'); break;
            default:   jbuf_append_char(&b, *p); break;
            }
            p++;
            continue;
        }
        jbuf_append_char(&b, *p++);
    }
    if (*p != '"') {
        jbuf_free(&b);
        return NULL;
    }
    if (out) *out = b.data;
    else jbuf_free(&b);
    return p + 1;
}

typedef enum {
    TOOL_SCHEMA_SCALAR_NONE = 0,
    TOOL_SCHEMA_SCALAR_INTEGER,
    TOOL_SCHEMA_SCALAR_NUMBER,
    TOOL_SCHEMA_SCALAR_BOOLEAN
} tool_schema_scalar_t;

static tool_schema_scalar_t tools_schema_scalar_for_property(const char *schema,
                                                            const char *key) {
    if (!schema || !key || !key[0]) return TOOL_SCHEMA_SCALAR_NONE;
    char *props = json_get_raw(schema, "properties");
    if (!props) return TOOL_SCHEMA_SCALAR_NONE;
    char *prop = json_get_raw(props, key);
    free(props);
    if (!prop) return TOOL_SCHEMA_SCALAR_NONE;

    tool_schema_scalar_t kind = TOOL_SCHEMA_SCALAR_NONE;
    char *type = json_get_str(prop, "type");
    if (type) {
        if (strcmp(type, "integer") == 0) kind = TOOL_SCHEMA_SCALAR_INTEGER;
        else if (strcmp(type, "number") == 0) kind = TOOL_SCHEMA_SCALAR_NUMBER;
        else if (strcmp(type, "boolean") == 0) kind = TOOL_SCHEMA_SCALAR_BOOLEAN;
        free(type);
    } else {
        char *raw = json_get_raw(prop, "type");
        if (raw) {
            if (strstr(raw, "\"integer\"")) kind = TOOL_SCHEMA_SCALAR_INTEGER;
            else if (strstr(raw, "\"number\"")) kind = TOOL_SCHEMA_SCALAR_NUMBER;
            else if (strstr(raw, "\"boolean\"")) kind = TOOL_SCHEMA_SCALAR_BOOLEAN;
            free(raw);
        }
    }
    free(prop);
    return kind;
}

static bool tools_trim_copy(const char *s, char *out, size_t outlen) {
    if (!s || !out || outlen == 0) return false;
    while (isspace((unsigned char)*s)) s++;
    const char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t n = (size_t)(e - s);
    if (n == 0 || n >= outlen) return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

static bool tools_string_to_schema_literal(tool_schema_scalar_t kind,
                                           const char *decoded,
                                           char *literal,
                                           size_t literal_len) {
    char tmp[160];
    if (!tools_trim_copy(decoded, tmp, sizeof(tmp))) return false;

    if (kind == TOOL_SCHEMA_SCALAR_BOOLEAN) {
        if (strcasecmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0 ||
            strcasecmp(tmp, "yes") == 0) {
            snprintf(literal, literal_len, "true");
            return true;
        }
        if (strcasecmp(tmp, "false") == 0 || strcmp(tmp, "0") == 0 ||
            strcasecmp(tmp, "no") == 0) {
            snprintf(literal, literal_len, "false");
            return true;
        }
        return false;
    }

    if (kind == TOOL_SCHEMA_SCALAR_INTEGER) {
        const char *p = tmp;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (*p) {
            if (!isdigit((unsigned char)*p)) return false;
            p++;
        }
        errno = 0;
        char *end = NULL;
        long long v = strtoll(tmp, &end, 10);
        if (errno == ERANGE || !end || *end) return false;
        snprintf(literal, literal_len, "%lld", v);
        return true;
    }

    if (kind == TOOL_SCHEMA_SCALAR_NUMBER) {
        errno = 0;
        char *end = NULL;
        double v = strtod(tmp, &end);
        if (errno == ERANGE || !end || *tools_json_ws(end) || !isfinite(v)) return false;
        snprintf(literal, literal_len, "%.17g", v);
        return true;
    }

    return false;
}

char *tools_normalize_input(const char *name, const char *input_json) {
    const char *schema = tools_schema_for_name(name);
    if (!schema || !input_json) return NULL;

    const char *p = tools_json_ws(input_json);
    if (*p != '{') return NULL;
    p++;

    jbuf_t out;
    jbuf_init(&out, strlen(input_json) + 32);
    jbuf_append_char(&out, '{');
    bool first = true;
    bool changed = false;

    while (*p) {
        p = tools_json_ws(p);
        if (*p == '}') {
            p++;
            break;
        }
        if (*p != '"') {
            jbuf_free(&out);
            return NULL;
        }

        const char *key_start = p;
        char *key = NULL;
        const char *key_end = tools_json_parse_string(p, &key);
        if (!key_end || !key) {
            free(key);
            jbuf_free(&out);
            return NULL;
        }

        p = tools_json_ws(key_end);
        if (*p != ':') {
            free(key);
            jbuf_free(&out);
            return NULL;
        }
        p++;
        const char *value_start = tools_json_ws(p);
        const char *value_end = tools_json_skip_value(value_start);
        if (value_end <= value_start && *value_start) {
            free(key);
            jbuf_free(&out);
            return NULL;
        }

        if (!first) jbuf_append_char(&out, ',');
        first = false;
        jbuf_append_len(&out, key_start, (size_t)(key_end - key_start));
        jbuf_append_char(&out, ':');

        bool emitted = false;
        if (*value_start == '"') {
            tool_schema_scalar_t kind = tools_schema_scalar_for_property(schema, key);
            if (kind != TOOL_SCHEMA_SCALAR_NONE) {
                char *decoded = NULL;
                const char *string_end = tools_json_parse_string(value_start, &decoded);
                if (string_end && string_end == value_end && decoded) {
                    char literal[160];
                    if (tools_string_to_schema_literal(kind, decoded,
                                                       literal, sizeof(literal))) {
                        jbuf_append(&out, literal);
                        changed = true;
                        emitted = true;
                    }
                }
                free(decoded);
            }
        }

        if (!emitted)
            jbuf_append_len(&out, value_start, (size_t)(value_end - value_start));

        free(key);
        p = tools_json_ws(value_end);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        if (*p) {
            jbuf_free(&out);
            return NULL;
        }
    }

    jbuf_append_char(&out, '}');
    if (!changed) {
        jbuf_free(&out);
        return NULL;
    }
    return out.data;
}

bool tools_validate_input(const char *name, const char *input_json,
                          char *error_buf, size_t error_len) {
    if (!input_json || !name) return true; /* no input to validate */

    const char *schema = tools_schema_for_name(name);
    if (!schema) return true; /* no schema = no validation */

    char *normalized = tools_normalize_input(name, input_json);
    json_validation_t v = json_validate_schema(normalized ? normalized : input_json,
                                               schema);
    free(normalized);
    if (!v.valid) {
        snprintf(error_buf, error_len, "input validation failed for '%s': %s",
                 name, v.error);
        DSCO_SET_ERR(DSCO_ERR_TOOL, "%s", error_buf);
        return false;
    }
    return true;
}
