/* introspect.c — Codebase self-stats + swarm-of-selves runner.
 *
 * This is the agent looking at its own body. Pure C, zero LLM cost for stats.
 * Selves uses fork/exec of dsco itself (the binary that contains us) so any
 * model/auth already configured is inherited.
 */
#include "introspect.h"
#include "config.h"
#include "topology.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <dirent.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef INTROSPECT_MAX_SELVES
#define INTROSPECT_MAX_SELVES 16
#endif

#ifndef INTROSPECT_CHILD_BUF
#define INTROSPECT_CHILD_BUF (64 * 1024)
#endif

/* ── Helpers ──────────────────────────────────────────────────────────── */

static long file_loc(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n')
            n++;
    fclose(f);
    return n;
}

/* Walk dir; for each regular file matching one of `exts`, add LOC. */
static void walk_loc(const char *dir, const char *const *exts, int nexts, long *loc_out,
                     long *files_out) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    char path[4096];
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk_loc(path, exts, nexts, loc_out, files_out);
            continue;
        }
        const char *dot = strrchr(de->d_name, '.');
        if (!dot)
            continue;
        for (int i = 0; i < nexts; i++) {
            if (strcmp(dot, exts[i]) == 0) {
                *loc_out += file_loc(path);
                (*files_out)++;
                break;
            }
        }
    }
    closedir(d);
}

static int self_exe_path(char *buf, size_t len) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)len;
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n > 0) {
        buf[n] = 0;
        return 0;
    }
#endif
    /* Fallback: look up "dsco" on PATH at runtime via /usr/bin/which */
    FILE *p = popen("command -v dsco 2>/dev/null", "r");
    if (!p)
        return -1;
    if (!fgets(buf, len, p)) {
        pclose(p);
        return -1;
    }
    pclose(p);
    size_t k = strlen(buf);
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r'))
        buf[--k] = 0;
    return buf[0] ? 0 : -1;
}

/* Find the dsco-cli repo root by walking upward from candidate dirs looking
 * for include/config.h. Returns 0 on success. */
static int find_repo_root(char *out, size_t len) {
    const char *seeds[] = {getenv("DSCO_REPO_ROOT"), ".", getenv("HOME") ? getenv("HOME") : "/",
                           NULL};
    char cand[4096];
    for (int i = 0; seeds[i]; i++) {
        if (!seeds[i][0])
            continue;
        /* try seed and seed/dsco-cli */
        const char *suffixes[] = {"", "/dsco-cli", NULL};
        for (int j = 0; suffixes[j]; j++) {
            snprintf(cand, sizeof(cand), "%s%s", seeds[i], suffixes[j]);
            char probe[4200];
            snprintf(probe, sizeof(probe), "%s/include/config.h", cand);
            if (access(probe, R_OK) == 0) {
                snprintf(out, len, "%s", cand);
                return 0;
            }
        }
    }
    return -1;
}

/* ── Public: codebase stats ───────────────────────────────────────────── */

int introspect_print_codebase_stats(FILE *out) {
    if (!out)
        out = stdout;

    /* Tool catalog (always available — compiled in) */
    int tool_count = 0;
    (void)tools_get_all(&tool_count);

    /* Topology count */
    int topos = TOPOLOGY_COUNT;

    fprintf(out, "═══ DSCO Self-Introspection ═══\n");
    fprintf(out, "  binary  : ");
    char exe[4096] = {0};
    if (self_exe_path(exe, sizeof(exe)) == 0)
        fprintf(out, "%s\n", exe);
    else
        fprintf(out, "(unknown)\n");

    struct stat st;
    if (exe[0] && stat(exe, &st) == 0)
        fprintf(out, "  size    : %lld bytes (%.2f MB)\n", (long long)st.st_size,
                (double)st.st_size / (1024.0 * 1024.0));

    fprintf(out, "  tools   : %d registered\n", tool_count);
    fprintf(out, "  topos   : %d topologies\n", topos);
    fprintf(out, "  reg cap : %d slots (always=%d warm=%d working=%d disc=%d)\n", TOOL_REGISTER_CAP,
            TOOL_REG_ALWAYS, TOOL_REG_WARM, TOOL_REG_WORKING, TOOL_REG_DISCOVERY);

    char root[4096] = {0};
    if (find_repo_root(root, sizeof(root)) == 0) {
        fprintf(out, "  repo    : %s\n", root);
        char dir[4200];
        const char *cexts[] = {".c", ".h"};
        long c_loc = 0, c_files = 0;
        snprintf(dir, sizeof(dir), "%s/src", root);
        walk_loc(dir, cexts, 2, &c_loc, &c_files);
        snprintf(dir, sizeof(dir), "%s/include", root);
        walk_loc(dir, cexts, 2, &c_loc, &c_files);
        fprintf(out, "  C body  : %ld LOC across %ld files\n", c_loc, c_files);
    } else {
        fprintf(out, "  repo    : (not found — set DSCO_REPO_ROOT)\n");
    }
    fprintf(out, "═══════════════════════════════\n");
    return 0;
}

/* ── Selves runner ────────────────────────────────────────────────────── */

/* Predefined exploration angles. The angle is prefixed to the user prompt
 * so each child takes a different stance on the same task. */
static const char *SELF_ANGLES[INTROSPECT_MAX_SELVES] = {
    "[ARCHITECT] Focus on structure, abstractions, and seams. ",
    "[CRITIC]    Identify weaknesses, foot-guns, and brittle assumptions. ",
    "[PERF]      Identify performance levers, hot paths, and cost. ",
    "[SECURITY]  Surface attack surface, leaks, and unsafe code paths. ",
    "[UX]        Consider the human at the CLI: clarity and ergonomics. ",
    "[GROWTH]    Find revenue, leverage, and capability multipliers. ",
    "[INVERT]    Apply inversion: what would make this fail/regress? ",
    "[SIMPLIFY]  Identify what to delete, merge, or constant-fold. ",
    "[SCALE]     Consider 10x and 100x load, depth, breadth. ",
    "[OBSERVE]   Ask what we currently cannot see; propose telemetry. ",
    "[TEST]      Propose decisive tests and verification. ",
    "[EVOLVE]    Suggest the next self-modification with positive ROI. ",
    "[META]      Reason about the reasoning; audit the framing itself. ",
    "[BOUNDARY]  Map the edges, contracts, and failure modes. ",
    "[COMPOSE]   Recombine existing primitives into a new capability. ",
    "[DOCTRINE]  Cite or evolve the relevant doctrine and rituals. "};

typedef struct {
    pid_t pid;
    int fd;
    int exit_code;
    char *buf;
    size_t buf_len;
    size_t buf_cap;
    int angle_idx;
} self_child_t;

static int spawn_self(const char *exe, const char *angle, const char *prompt, self_child_t *child) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    /* Build prefixed prompt: angle + prompt */
    size_t plen = strlen(angle) + strlen(prompt) + 4;
    char *full = (char *)malloc(plen);
    if (!full) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    snprintf(full, plen, "%s%s", angle, prompt);

    pid_t pid = fork();
    if (pid < 0) {
        free(full);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        /* Avoid recursive selves explosion */
        setenv("DSCO_IN_SELVES", "1", 1);
        /* Run dsco non-interactively with cheap mode to keep cost bounded */
        char *const argv[] = {(char *)exe, (char *)"-C", (char *)full, NULL};
        execv(exe, argv);
        fprintf(stderr, "[self] execv failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    child->pid = pid;
    child->fd = pipefd[0];
    child->exit_code = -1;
    child->buf_cap = 4096;
    child->buf_len = 0;
    child->buf = (char *)malloc(child->buf_cap);
    if (child->buf)
        child->buf[0] = 0;
    free(full);
    return 0;
}

static void child_append(self_child_t *c, const char *data, size_t n) {
    if (!c->buf)
        return;
    if (c->buf_len + n + 1 > c->buf_cap) {
        size_t nc = c->buf_cap * 2;
        while (nc < c->buf_len + n + 1)
            nc *= 2;
        if (nc > INTROSPECT_CHILD_BUF)
            nc = INTROSPECT_CHILD_BUF;
        if (c->buf_len + n + 1 > nc) {
            /* truncate quietly */
            n = nc - c->buf_len - 1;
            if ((ssize_t)n <= 0)
                return;
        }
        char *nb = (char *)realloc(c->buf, nc);
        if (!nb)
            return;
        c->buf = nb;
        c->buf_cap = nc;
    }
    memcpy(c->buf + c->buf_len, data, n);
    c->buf_len += n;
    c->buf[c->buf_len] = 0;
}

int introspect_run_selves(FILE *out, int n, const char *prompt) {
    if (!out)
        out = stdout;
    if (!prompt || !*prompt) {
        fprintf(out, "[selves] error: empty prompt\n");
        return -1;
    }
    if (getenv("DSCO_IN_SELVES")) {
        fprintf(out, "[selves] refusing to nest (DSCO_IN_SELVES set)\n");
        return -1;
    }
    if (n < 1)
        n = 3;
    if (n > INTROSPECT_MAX_SELVES)
        n = INTROSPECT_MAX_SELVES;

    char exe[4096] = {0};
    if (self_exe_path(exe, sizeof(exe)) != 0) {
        fprintf(out, "[selves] cannot resolve self executable\n");
        return -1;
    }

    fprintf(out, "═══ dsco selves: spawning %d angles ═══\n", n);
    fprintf(out, "  exe   : %s\n", exe);
    fprintf(out, "  task  : %s\n", prompt);
    fprintf(out, "─────────────────────────────────────\n");
    fflush(out);

    self_child_t children[INTROSPECT_MAX_SELVES];
    memset(children, 0, sizeof(children));
    int live = 0;
    for (int i = 0; i < n; i++) {
        children[i].angle_idx = i;
        if (spawn_self(exe, SELF_ANGLES[i], prompt, &children[i]) == 0) {
            live++;
            fprintf(out, "  ✓ spawned self #%d %.*s (pid=%d)\n", i, 20, SELF_ANGLES[i],
                    (int)children[i].pid);
        } else {
            fprintf(out, "  ✗ failed to spawn self #%d\n", i);
            children[i].pid = -1;
        }
    }
    fflush(out);

    /* Drain pipes until all children exit. Use simple polling loop. */
    while (live > 0) {
        for (int i = 0; i < n; i++) {
            self_child_t *c = &children[i];
            if (c->pid <= 0)
                continue;
            char buf[2048];
            ssize_t r = read(c->fd, buf, sizeof(buf));
            if (r > 0)
                child_append(c, buf, (size_t)r);
            else if (r == 0) {
                /* EOF — child closed pipe */
                close(c->fd);
                c->fd = -1;
                int status = 0;
                waitpid(c->pid, &status, 0);
                c->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                c->pid = -1;
                live--;
            } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                close(c->fd);
                c->fd = -1;
                int status = 0;
                waitpid(c->pid, &status, 0);
                c->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                c->pid = -1;
                live--;
            }
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    /* Synthesis */
    fprintf(out, "\n═══ self reports ═══\n");
    for (int i = 0; i < n; i++) {
        self_child_t *c = &children[i];
        fprintf(out, "\n── self #%d %.*s (exit=%d, %zu bytes) ──\n", i, 20,
                SELF_ANGLES[c->angle_idx], c->exit_code, c->buf_len);
        if (c->buf && c->buf_len > 0) {
            /* Truncate to last 2KB per self to keep terminal usable */
            const size_t cap = 2048;
            if (c->buf_len > cap) {
                fprintf(out, "[…%zu earlier bytes elided…]\n", c->buf_len - cap);
                fwrite(c->buf + (c->buf_len - cap), 1, cap, out);
            } else {
                fwrite(c->buf, 1, c->buf_len, out);
            }
            if (c->buf_len && c->buf[c->buf_len - 1] != '\n')
                fputc('\n', out);
        }
        free(c->buf);
        c->buf = NULL;
    }
    fprintf(out, "\n═══ end selves ═══\n");
    return 0;
}
