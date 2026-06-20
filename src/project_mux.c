#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "project_mux.h"
#include "project.h"
#include "project_grid.h"
#include "simd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ──────────────────────────────────────────────────────────────────────────
 *  Module state — kept small. The mux struct is the source of truth.
 * ────────────────────────────────────────────────────────────────────────── */

static dsco_mux_t  *g_mux = NULL;
static struct termios g_saved_tio;
static bool         g_tio_saved = false;

#define MUX_PREFIX        0x02   /* Ctrl+B */
#define ANSI_CLEAR        "\x1b[2J\x1b[H"
#define ANSI_CLEAR_LINE   "\x1b[2K"
#define ANSI_HOME         "\x1b[H"
#define ANSI_HIDE_CURSOR  "\x1b[?25l"
#define ANSI_SHOW_CURSOR  "\x1b[?25h"
#define ANSI_ALT_ON       "\x1b[?1049h"
#define ANSI_ALT_OFF      "\x1b[?1049l"
#define ANSI_RESET        "\x1b[0m"
#define ANSI_BOLD         "\x1b[1m"
#define ANSI_REV          "\x1b[7m"
#define ANSI_DIM          "\x1b[2m"
#define ANSI_FG_CYAN      "\x1b[36m"
#define ANSI_FG_YELLOW    "\x1b[33m"
#define ANSI_FG_GREEN     "\x1b[32m"
#define ANSI_FG_RED       "\x1b[31m"
#define ANSI_FG_GREY      "\x1b[90m"

/* ──────────────────────────────────────────────────────────────────────────
 *  Terminal control
 * ────────────────────────────────────────────────────────────────────────── */

static void tio_restore(void) {
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        write(STDOUT_FILENO, ANSI_SHOW_CURSOR ANSI_ALT_OFF, sizeof(ANSI_SHOW_CURSOR ANSI_ALT_OFF) - 1);
    }
}

static int tio_raw(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return -1;
    g_tio_saved = true;
    atexit(tio_restore);

    struct termios raw = g_saved_tio;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    write(STDOUT_FILENO, ANSI_ALT_ON ANSI_HIDE_CURSOR, sizeof(ANSI_ALT_ON ANSI_HIDE_CURSOR) - 1);
    return 0;
}

static void term_size(int *rows, int *cols) {
    struct winsize w = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row && w.ws_col) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static volatile sig_atomic_t g_resized = 0;
static void on_winch(int sig) { (void)sig; g_resized = 1; }

static volatile sig_atomic_t g_term = 0;
static void on_term(int sig) { (void)sig; g_term = 1; }

/* ──────────────────────────────────────────────────────────────────────────
 *  Worker spawn — child process exec'ing this binary in --worker mode.
 *  Implementation note: we re-exec the same binary with `--worker <id>` so
 *  the child runs the full agent loop wired against the project's root +
 *  workspace. The actual worker entry is `dsco_mux_worker_main()`.
 * ────────────────────────────────────────────────────────────────────────── */

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

extern char **environ;

static char g_self_exe[PATH_MAX] = {0};

/* ──────────────────────────────────────────────────────────────────────────
 *  Per-project drain thread
 *
 *  Owns blocking reads on worker_out_fd → ring buffer. Wakes the main loop
 *  via the mux wake-pipe (1 byte per non-empty drain). Slow/bursty workers
 *  no longer block render.
 * ────────────────────────────────────────────────────────────────────────── */

static void *drain_thread_main(void *arg) {
    dsco_project_t *p = (dsco_project_t *)arg;
    char buf[16384];
    while (atomic_load_explicit(&p->drain_run, memory_order_acquire)) {
        ssize_t n = read(p->worker_out_fd, buf, sizeof(buf));
        if (n > 0) {
            dsco_ring_write(&p->scrollback, buf, (size_t)n);
            atomic_store_explicit(&p->has_unread, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&p->activity_bytes,
                                      (unsigned long long)n,
                                      memory_order_relaxed);
            if (p->wake_fd_w >= 0) {
                char wk = 1;
                ssize_t w = write(p->wake_fd_w, &wk, 1);
                (void)w; /* best-effort; pipe full just means the loop is busy */
            }
        } else if (n == 0) {
            /* EOF — worker exited */
            p->state = DSCO_PROJECT_DEAD;
            p->epoch++;
            if (p->wake_fd_w >= 0) { char wk = 1; ssize_t w = write(p->wake_fd_w, &wk, 1); (void)w; }
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* drain_run set non-blocking somewhere; sleep briefly */
                struct timespec ts = { 0, 5 * 1000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
    }
    return NULL;
}

static int start_drain_thread(dsco_project_t *p, int wake_fd_w) {
    if (!p || p->worker_out_fd < 0) return -1;
    /* Drain thread does blocking reads — undo any nonblock that spawn set. */
    int fl = fcntl(p->worker_out_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(p->worker_out_fd, F_SETFL, fl & ~O_NONBLOCK);
    p->wake_fd_w = wake_fd_w;
    atomic_store_explicit(&p->drain_run, 1, memory_order_release);
    if (pthread_create(&p->drain_thread, NULL, drain_thread_main, p) != 0) {
        atomic_store_explicit(&p->drain_run, 0, memory_order_release);
        return -1;
    }
    return 0;
}

static void stop_drain_thread(dsco_project_t *p) {
    if (!p) return;
    if (atomic_load_explicit(&p->drain_run, memory_order_acquire)) {
        atomic_store_explicit(&p->drain_run, 0, memory_order_release);
        /* closing worker_out_fd will unblock the read() */
        if (p->worker_out_fd >= 0) { close(p->worker_out_fd); p->worker_out_fd = -1; }
        pthread_join(p->drain_thread, NULL);
    }
}

static const char *self_exe(void) {
    if (g_self_exe[0]) return g_self_exe;
    /* /proc/self/exe on Linux; fall back to argv[0] surrogate via dladdr-free path */
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        snprintf(g_self_exe, sizeof(g_self_exe), "%s", buf);
        return g_self_exe;
    }
#ifdef __APPLE__
    /* _NSGetExecutablePath equivalent without including the header */
    extern int _NSGetExecutablePath(char *buf, unsigned *bufsize);
    unsigned sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        snprintf(g_self_exe, sizeof(g_self_exe), "%s", buf);
        return g_self_exe;
    }
#endif
    snprintf(g_self_exe, sizeof(g_self_exe), "dsco");
    return g_self_exe;
}

int dsco_mux_spawn_worker(dsco_project_t *p, const char *api_key) {
    if (!p) return -1;

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe)  != 0) return -1;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* child */
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(in_pipe[0]);
        close(out_pipe[1]);

        if (chdir(p->id.root) != 0) {
            fprintf(stderr, "[worker] chdir(%s) failed: %s\n", p->id.root, strerror(errno));
        }
        if (api_key && *api_key) setenv("ANTHROPIC_API_KEY", api_key, 1);
        setenv("DSCO_PROJECT_ID",   p->id.id, 1);
        setenv("DSCO_PROJECT_NAME", p->id.name, 1);
        setenv("DSCO_WORKER",       "1", 1);

        char *argv[] = { (char *)self_exe(), "--worker", p->id.id, NULL };
        execve(self_exe(), argv, environ);
        fprintf(stderr, "[worker] execve failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    /* drain thread does blocking reads — leave fd in default (blocking) mode */

    p->worker_pid    = pid;
    p->worker_in_fd  = in_pipe[1];
    p->worker_out_fd = out_pipe[0];

    /* Start the per-project drain thread. wake_fd_w comes from the mux. */
    int wake = (p->mux && ((dsco_mux_t *)p->mux)->wake_fd_w >= 0)
               ? ((dsco_mux_t *)p->mux)->wake_fd_w : -1;
    start_drain_thread(p, wake);
    return 0;
}

int dsco_mux_kill_worker(dsco_project_t *p) {
    if (!p) return -1;
    /* tear down the drain thread first — closing worker_out_fd unblocks read() */
    stop_drain_thread(p);
    if (p->worker_pid > 0) {
        kill(p->worker_pid, SIGTERM);
        /* give it 200ms to exit */
        for (int i = 0; i < 20; i++) {
            int status = 0;
            pid_t r = waitpid(p->worker_pid, &status, WNOHANG);
            if (r == p->worker_pid) { p->worker_pid = -1; break; }
            struct timespec ts = { 0, 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
        if (p->worker_pid > 0) {
            kill(p->worker_pid, SIGKILL);
            int status = 0;
            waitpid(p->worker_pid, &status, 0);
            p->worker_pid = -1;
        }
    }
    if (p->worker_in_fd  >= 0) { close(p->worker_in_fd);  p->worker_in_fd  = -1; }
    if (p->worker_out_fd >= 0) { close(p->worker_out_fd); p->worker_out_fd = -1; }
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Rendering — deep grid with truecolor borders + heatmap shading
 * ────────────────────────────────────────────────────────────────────────── */

static void out_write(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void out_writen(const char *s, size_t n) { write(STDOUT_FILENO, s, n); }
static void move_to(int row, int col) {
    char b[32];
    int n = snprintf(b, sizeof(b), "\x1b[%d;%dH", row, col);
    write(STDOUT_FILENO, b, n);
}

static void ansi_fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um", r, g, b);
    write(STDOUT_FILENO, buf, n);
}
static void ansi_bg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "\x1b[48;2;%u;%u;%um", r, g, b);
    write(STDOUT_FILENO, buf, n);
}

/* Compose the border + background color for a tile based on the current
 * grid color mode. Returns whether anything was emitted (so the caller
 * knows to reset afterwards). */
static int apply_tile_colors(dsco_mux_t *m, dsco_project_t *p, bool focused,
                              dsco_rgb_t *out_border) {
    dsco_grid_t *g = &m->grid;
    dsco_rgb_t border = {200, 200, 200};
    bool emitted = false;

    switch (g->color_mode) {
        case DSCO_COLOR_MODE_OFF:
            border.r = focused ? 240 : 100;
            border.g = focused ? 240 : 100;
            border.b = focused ? 240 : 100;
            break;
        case DSCO_COLOR_MODE_STATE: {
            border = dsco_grid_state_color((int)p->state);
            break;
        }
        case DSCO_COLOR_MODE_IDENTITY: {
            border = dsco_grid_project_color(p->id.id);
            if (!focused) {
                /* dim unfocused by lerping toward grey */
                dsco_rgb_t grey = {80, 80, 88};
                border = dsco_grid_lerp(border, grey, 0.55f);
            }
            break;
        }
        case DSCO_COLOR_MODE_HEATMAP: {
            double bps = dsco_project_activity_bps(p);
            float t = (float)(bps / 8192.0);
            if (t > 1.0f) t = 1.0f;
            dsco_rgb_t cold = {30, 30, 40};
            dsco_rgb_t hot  = {255, 170, 60};
            border = dsco_grid_lerp(cold, hot, t);
            break;
        }
        case DSCO_COLOR_MODE_FULL: {
            border = dsco_grid_project_color(p->id.id);
            if (!focused) {
                dsco_rgb_t grey = {80, 80, 88};
                border = dsco_grid_lerp(border, grey, 0.5f);
            }
            double bps = dsco_project_activity_bps(p);
            float t = (float)(bps / 16384.0);
            if (t > 0.4f) t = 0.4f;
            dsco_rgb_t base = {18, 18, 22};
            dsco_rgb_t accent = dsco_grid_project_color(p->id.id);
            dsco_rgb_t bg = dsco_grid_lerp(base, accent, t);
            ansi_bg_rgb(bg.r, bg.g, bg.b);
            emitted = true;
            break;
        }
        default: break;
    }
    ansi_fg_rgb(border.r, border.g, border.b);
    if (focused) out_write("\x1b[1m");
    if (out_border) *out_border = border;
    return emitted ? 1 : 0;
}

/* Draw a tile's border at its cached rect, then render the project's
 * scrollback inside. `focused` controls bold + bright border. */
static void draw_tile(dsco_mux_t *m, const dsco_tile_t *t, bool focused) {
    int r0 = t->rect.row0;
    int c0 = t->rect.col0;
    int rows = t->rect.rows;
    int cols = t->rect.cols;
    if (rows < 2 || cols < 2) return;

    dsco_project_t *p = NULL;
    if (t->project_idx >= 0 && t->project_idx < m->count) {
        p = m->projects[t->project_idx];
    }

    dsco_border_glyphs_t bg = dsco_grid_border_glyphs(m->grid.border);
    bool bg_painted = false;
    if (p) bg_painted = apply_tile_colors(m, p, focused, NULL) != 0;
    else { ansi_fg_rgb(70, 70, 80); }

    /* top border with embedded title (Unicode sigil + sparkline) */
    move_to(r0, c0);
    out_write(bg.tl);

    /* Build title: " ⚙ name " — sigil reflects state */
    static const char *k_spark[8] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    const char *sigil = "◌";
    if (p) {
        if (p->state == DSCO_PROJECT_RUNNING)         sigil = "⚙";
        else if (p->state == DSCO_PROJECT_PAUSED)     sigil = "⏸";
        else if (p->state == DSCO_PROJECT_DEAD)       sigil = "☠";
        else if (p->state == DSCO_PROJECT_QUARANTINED) sigil = "⚠";
    }

    char title[160];
    if (p) snprintf(title, sizeof(title), " %s %s ", sigil, p->id.name);
    else   snprintf(title, sizeof(title), " (empty) ");
    int tlen = (int)strlen(title);
    if (tlen > cols - 4) tlen = cols - 4;

    int left = 2;
    for (int i = 0; i < left && i < cols - 2; i++) out_write(bg.h);
    if (tlen > 0) {
        if (focused) out_write("\x1b[7m");
        out_writen(title, tlen);
        out_write("\x1b[27m");
        if (focused) out_write("\x1b[1m");
    }
    /* sparkline: 8 most-recent samples, mapped to 8 block glyphs.
     * Drawn at the right end of the title bar, before tr corner. */
    int right_pad = cols - 2 - left - tlen;
    int spark_cells = 0;
    if (p && right_pad >= 12) {
        double ring[16];
        dsco_project_activity_ring(p, ring);
        double peak = 1.0;
        for (int i = 0; i < 16; i++) if (ring[i] > peak) peak = ring[i];
        spark_cells = 8;
        int border_before = right_pad - spark_cells - 2;
        for (int i = 0; i < border_before; i++) out_write(bg.h);
        out_write(" ");
        for (int i = 8; i < 16; i++) {
            double v = ring[i];
            int idx = (int)(v / peak * 7.0);
            if (idx < 0) idx = 0; if (idx > 7) idx = 7;
            out_write(k_spark[idx]);
        }
        out_write(" ");
    } else {
        for (int i = 0; i < right_pad; i++) out_write(bg.h);
    }
    out_write(bg.tr);

    /* side borders */
    for (int r = 1; r < rows - 1; r++) {
        move_to(r0 + r, c0);
        out_write(bg.v);
        move_to(r0 + r, c0 + cols - 1);
        out_write(bg.v);
    }
    /* bottom */
    move_to(r0 + rows - 1, c0);
    out_write(bg.bl);
    for (int i = 0; i < cols - 2; i++) out_write(bg.h);
    out_write(bg.br);

    out_write(ANSI_RESET);

    if (!p) return;

    /* paint the inner background again (without border color) so heatmap is
     * visible across the interior */
    if (m->grid.color_mode == DSCO_COLOR_MODE_FULL || m->grid.color_mode == DSCO_COLOR_MODE_HEATMAP) {
        double bps = dsco_project_activity_bps(p);
        float t = (float)(bps / 16384.0);
        if (t > 0.35f) t = 0.35f;
        dsco_rgb_t base = {12, 12, 16};
        dsco_rgb_t accent = (m->grid.color_mode == DSCO_COLOR_MODE_FULL)
                            ? dsco_grid_project_color(p->id.id)
                            : (dsco_rgb_t){255, 170, 60};
        dsco_rgb_t bgc = dsco_grid_lerp(base, accent, t);
        ansi_bg_rgb(bgc.r, bgc.g, bgc.b);
        for (int r = 1; r < rows - 1; r++) {
            move_to(r0 + r, c0 + 1);
            for (int c = 0; c < cols - 2; c++) out_write(" ");
        }
        out_write(ANSI_RESET);
    }
    (void)bg_painted;

    /* render scrollback inside */
    static __thread char snap[DSCO_PROJECT_RING_BYTES + 1];
    size_t n = dsco_project_snapshot(p, snap, sizeof(snap));
    atomic_store_explicit(&p->has_unread, 0, memory_order_relaxed);

    int avail = rows - 2;
    int inner_w = cols - 2;
    if (avail < 1 || inner_w < 1) return;

    const char *lines[256];
    int line_count = 0;
    size_t pos = n;
    size_t cap = sizeof(lines) / sizeof(lines[0]);
    while (pos > 0 && line_count < (int)cap) {
        ssize_t nl = dsco_simd_rfind_byte(snap, pos, '\n');
        if (nl < 0) { lines[line_count++] = snap; break; }
        if ((size_t)nl == n - 1 && line_count == 0) { pos = (size_t)nl; continue; }
        lines[line_count++] = snap + nl + 1;
        pos = (size_t)nl;
    }

    int draw_lines = line_count < avail ? line_count : avail;
    int row = r0 + 1 + (avail - draw_lines);
    const char *end = snap + n;
    for (int i = draw_lines - 1; i >= 0; i--) {
        const char *ls = lines[i];
        const char *le = (i == 0) ? end : (lines[i - 1] - 1);
        if (le < ls) le = ls;
        int len = (int)(le - ls);
        if (len > inner_w) len = inner_w;
        move_to(row, c0 + 1);
        out_writen(ls, len);
        row++;
    }
    out_write(ANSI_RESET);
}

static void draw_tab_strip(dsco_mux_t *m, int cols) {
    move_to(1, 1);
    out_write(ANSI_CLEAR_LINE);
    int x = 1;
    for (int i = 0; i < m->count && x < cols - 4; i++) {
        dsco_project_t *p = m->projects[i];
        bool active = (i == m->focused);
        int unread_flag = atomic_load_explicit(&p->has_unread, memory_order_relaxed);

        char tab[160];
        const char *dot = (unread_flag && !active) ? "•" : " ";
        /* hue from identity, brightness from focus, state shown as suffix glyph */
        dsco_rgb_t hue = dsco_grid_project_color(p->id.id);
        if (!active) { dsco_rgb_t grey = {90, 90, 100}; hue = dsco_grid_lerp(hue, grey, 0.55f); }
        const char *state_glyph = " ";
        if (p->state == DSCO_PROJECT_RUNNING)      state_glyph = "*";
        else if (p->state == DSCO_PROJECT_PAUSED)  state_glyph = "~";
        else if (p->state == DSCO_PROJECT_DEAD)    state_glyph = "x";

        if (active) {
            ansi_bg_rgb(hue.r / 3, hue.g / 3, hue.b / 3);
            ansi_fg_rgb(hue.r, hue.g, hue.b);
            out_write("\x1b[1m");
        } else {
            ansi_fg_rgb(hue.r, hue.g, hue.b);
        }
        int n = snprintf(tab, sizeof(tab), " %d %s %s%s ",
                         i + 1, p->id.name, state_glyph, dot);
        if (x + n > cols - 4) n = cols - 4 - x;
        if (n > 0) out_writen(tab, n);
        out_write(ANSI_RESET);
        x += n;
    }
    if (m->count == 0) {
        out_write(ANSI_DIM "  (no projects — Ctrl+B c to create)  " ANSI_RESET);
    }
}

static const char *color_mode_name(dsco_color_mode_t m) {
    switch (m) {
        case DSCO_COLOR_MODE_OFF:      return "mono";
        case DSCO_COLOR_MODE_STATE:    return "state";
        case DSCO_COLOR_MODE_IDENTITY: return "id-hue";
        case DSCO_COLOR_MODE_HEATMAP:  return "heat";
        case DSCO_COLOR_MODE_FULL:     return "full";
        default: return "?";
    }
}
static const char *border_name(dsco_border_t b) {
    switch (b) {
        case DSCO_BORDER_NONE:   return "none";
        case DSCO_BORDER_SINGLE: return "single";
        case DSCO_BORDER_DOUBLE: return "double";
        case DSCO_BORDER_HEAVY:  return "heavy";
        case DSCO_BORDER_DASHED: return "dashed";
        default: return "?";
    }
}

static void draw_prefix_hud(dsco_mux_t *m, int rows, int cols) {
    /* When prefix is pending, show a single-row HUD of next-key options. */
    if (!m->prefix_pending) return;
    if (rows < 6) return;
    int row = rows - 2;
    move_to(row, 1);
    out_write(ANSI_CLEAR_LINE);
    ansi_bg_rgb(40, 40, 55);
    ansi_fg_rgb(220, 220, 230);
    out_write("\x1b[1m");
    const char *hud =
        " c:new x:close 1-9:focus h/j/k/l:nav z:zoom s/v:split "
        "G:grid C:color B:border T:theme d:detach ?:help ";
    int n = (int)strlen(hud);
    if (n > cols) n = cols;
    out_writen(hud, n);
    out_write(ANSI_RESET);
}

static void draw_status_bar(dsco_mux_t *m, int rows, int cols) {
    move_to(rows, 1);
    out_write(ANSI_CLEAR_LINE ANSI_REV);
    char buf[512];
    if (m->focused >= 0 && m->focused < m->count) {
        dsco_project_t *p = m->projects[m->focused];
        double bps = p->activity_ewma;
        int n = snprintf(buf, sizeof(buf),
            " dsco-mux │ %s [%s] │ %d/%d │ col:%s bord:%s │ %.0f B/s │ Ctrl+B ? ",
            p->id.name, dsco_project_state_name(p->state),
            m->focused + 1, m->count,
            color_mode_name(m->grid.color_mode),
            border_name(m->grid.border),
            bps);
        if (n > cols) n = cols;
        out_writen(buf, n);
    } else {
        snprintf(buf, sizeof(buf), " dsco-mux │ empty │ Ctrl+B c to create │ Ctrl+B ? help ");
        out_write(buf);
    }
    if (m->prefix_pending) {
        move_to(rows, cols - 18);
        out_write(" [PREFIX ACTIVE] ");
    }
    out_write(ANSI_RESET);
}

/* Legacy single-pane renderer — kept for help overlay / fallback only. */
static void draw_pane_legacy(dsco_project_t *p, int row0, int col0, int h, int w, bool focused) {
    /* border */
    for (int r = row0; r < row0 + h; r++) {
        move_to(r, col0);
        for (int c = 0; c < w; c++) out_write(" ");
    }
    /* title row */
    move_to(row0, col0);
    if (focused) out_write(ANSI_REV);
    char title[128];
    int tn = snprintf(title, sizeof(title), "  %s  ", p ? p->id.name : "(empty)");
    if (tn > w) tn = w;
    out_writen(title, tn);
    out_write(ANSI_RESET);

    if (!p) return;

    /* snapshot scrollback */
    static __thread char snap[DSCO_PROJECT_RING_BYTES + 1];
    size_t n = dsco_project_snapshot(p, snap, sizeof(snap));
    atomic_store_explicit(&p->has_unread, 0, memory_order_relaxed);

    /* find the last (h-2) lines that fit width w. SIMD'd via include/simd.h
     * — the backward scan goes 16 bytes per cycle on NEON/SSE2 instead of
     * one byte per cycle. */
    int avail = h - 2;
    if (avail < 1) return;
    const char *lines[256];
    int line_count = 0;
    const char *end = snap + n;
    size_t pos = n;
    size_t cap = sizeof(lines) / sizeof(lines[0]);
    while (pos > 0 && line_count < (int)cap) {
        ssize_t nl = dsco_simd_rfind_byte(snap, pos, '\n');
        if (nl < 0) {
            lines[line_count++] = snap;
            break;
        }
        /* skip a trailing newline at the very end so we don't record an empty
         * line for it. */
        if ((size_t)nl == n - 1 && line_count == 0) { pos = (size_t)nl; continue; }
        lines[line_count++] = snap + nl + 1;
        pos = (size_t)nl;
    }
    /* draw newest at bottom, so iterate from end of `lines` back to 0 */
    int draw_lines = line_count < avail ? line_count : avail;
    int row = row0 + 1 + (avail - draw_lines);
    for (int i = draw_lines - 1; i >= 0; i--) {
        const char *ls = lines[i];
        const char *le = (i == 0) ? end : (lines[i - 1] - 1);
        if (le < ls) le = ls;
        int len = (int)(le - ls);
        if (len > w - 1) len = w - 1;
        move_to(row, col0 + 1);
        /* sanitize control chars except color escape */
        out_writen(ls, len);
        row++;
    }
}

/* Mini-map: a single-row chip strip at the bottom showing every project
 * as a colored cell sized proportional to its recent activity. */
static void draw_minimap(dsco_mux_t *m, int row, int cols) {
    if (m->count == 0) return;
    move_to(row, 1);
    out_write(ANSI_CLEAR_LINE);
    int per = (cols - 2) / m->count;
    if (per < 1) per = 1;
    int x = 1;
    for (int i = 0; i < m->count && x < cols - per; i++) {
        dsco_project_t *p = m->projects[i];
        dsco_rgb_t c = dsco_grid_project_color(p->id.id);
        double bps = p->activity_ewma;
        float t = (float)(bps / 4096.0); if (t > 1.0f) t = 1.0f;
        dsco_rgb_t dim = {30, 30, 35};
        dsco_rgb_t bg = dsco_grid_lerp(dim, c, 0.4f + 0.6f * t);
        ansi_bg_rgb(bg.r, bg.g, bg.b);
        ansi_fg_rgb(c.r, c.g, c.b);
        for (int k = 0; k < per; k++) {
            if (k == per / 2 && i == m->focused) out_write("●");
            else                                 out_write(" ");
        }
        out_write(ANSI_RESET);
        x += per;
    }
}

static void render(dsco_mux_t *m) {
    out_write(ANSI_CLEAR);
    int rows = m->term_rows, cols = m->term_cols;
    draw_tab_strip(m, cols);

    /* status bar = row `rows`, minimap = row `rows-1`, body = rows 2..rows-2 */
    int body_row0 = 2;
    int body_rows = rows - 3;
    if (body_rows < 4) body_rows = rows - 2;  /* skip minimap if too small */

    if (m->count == 0) {
        move_to(body_row0 + body_rows / 2, cols / 2 - 12);
        out_write(ANSI_DIM "no projects — Ctrl+B c to create" ANSI_RESET);
        draw_status_bar(m, rows, cols);
        return;
    }

    /* Bind grid leaves to projects and layout */
    dsco_grid_assign_projects(&m->grid, m->count);
    if (m->grid.focused_id < 0 ||
        !m->grid.tiles[m->grid.focused_id].in_use ||
        m->grid.tiles[m->grid.focused_id].kind != DSCO_TILE_LEAF) {
        /* refocus on the mux's focused project */
        int tid = dsco_grid_find_leaf(&m->grid, m->focused);
        if (tid >= 0) m->grid.focused_id = tid;
    }
    dsco_grid_layout(&m->grid, body_row0, 1, body_rows, cols);

    /* Render every visible leaf */
    if (m->grid.zoomed_id >= 0) {
        const dsco_tile_t *t = &m->grid.tiles[m->grid.zoomed_id];
        draw_tile(m, t, true);
    } else {
        for (int i = 0; i < DSCO_GRID_MAX_TILES; i++) {
            const dsco_tile_t *t = &m->grid.tiles[i];
            if (!t->in_use || t->kind != DSCO_TILE_LEAF) continue;
            bool focused = (i == m->grid.focused_id);
            draw_tile(m, t, focused);
        }
    }

    /* sync m->focused with grid focus */
    if (m->grid.focused_id >= 0) {
        const dsco_tile_t *ft = &m->grid.tiles[m->grid.focused_id];
        if (ft->project_idx >= 0 && ft->project_idx < m->count) {
            m->focused = ft->project_idx;
        }
    }

    if (rows >= 5) draw_minimap(m, rows - 1, cols);
    draw_prefix_hud(m, rows, cols);
    draw_status_bar(m, rows, cols);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Mux helpers
 * ────────────────────────────────────────────────────────────────────────── */

static int mux_add(dsco_mux_t *m, dsco_project_t *p) {
    if (m->count >= DSCO_PROJECT_MAX) return -1;
    p->mux = m;
    m->projects[m->count++] = p;
    return m->count - 1;
}

static void mux_remove_at(dsco_mux_t *m, int idx) {
    if (idx < 0 || idx >= m->count) return;
    dsco_project_t *p = m->projects[idx];
    dsco_project_close(p);
    for (int i = idx; i < m->count - 1; i++) m->projects[i] = m->projects[i + 1];
    m->count--;
    if (m->focused >= m->count) m->focused = m->count - 1;
    if (m->focused < 0 && m->count > 0) m->focused = 0;
    m->needs_redraw = true;
}

static int mux_create_new(dsco_mux_t *m, const char *root, const char *name) {
    dsco_project_t *p = NULL;
    if (dsco_project_create(root, name, &p) != 0) return -1;
    int idx = mux_add(m, p);
    if (idx < 0) { dsco_project_close(p); return -1; }
    if (dsco_project_start(p, m->api_key) != 0) {
        /* still keep it in the list, just IDLE */
    }
    m->focused = idx;
    m->needs_redraw = true;
    return idx;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Input prompts — a minimal modal input over the alt screen
 * ────────────────────────────────────────────────────────────────────────── */

static int read_modal_line(const char *prompt, char *out, size_t out_cap) {
    int rows = g_mux->term_rows, cols = g_mux->term_cols;
    move_to(rows - 1, 1);
    out_write(ANSI_CLEAR_LINE ANSI_REV);
    char b[256];
    int n = snprintf(b, sizeof(b), " %s ", prompt);
    if (n > cols) n = cols;
    out_writen(b, n);
    out_write(ANSI_RESET " ");
    out_write(ANSI_SHOW_CURSOR);

    /* temporarily restore cooked-ish mode for line input */
    struct termios cooked = g_saved_tio;
    tcsetattr(STDIN_FILENO, TCSANOW, &cooked);

    size_t i = 0;
    char c;
    while (i < out_cap - 1) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == 127 || c == 8) { if (i > 0) { i--; write(STDOUT_FILENO, "\b \b", 3); } continue; }
        if (c == 27) { /* esc — cancel */
            tcsetattr(STDIN_FILENO, TCSANOW, &cooked);
            out_write(ANSI_HIDE_CURSOR);
            /* re-enter raw */
            struct termios raw = g_saved_tio;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
            raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
            raw.c_oflag &= ~(OPOST);
            raw.c_cflag |= CS8;
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
            return -1;
        }
        out[i++] = c;
        write(STDOUT_FILENO, &c, 1);
    }
    out[i] = '\0';

    /* back to raw */
    struct termios raw = g_saved_tio;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    out_write(ANSI_HIDE_CURSOR);
    return (int)i;
}

static void show_help(dsco_mux_t *m) {
    int rows = m->term_rows, cols = m->term_cols;
    int boxw = 56, boxh = 16;
    int r0 = (rows - boxh) / 2;
    int c0 = (cols - boxw) / 2;
    for (int r = 0; r < boxh; r++) {
        move_to(r0 + r, c0);
        for (int c = 0; c < boxw; c++) out_write(" ");
    }
    const char *lines[] = {
        " dsco-mux — deep grid console",
        " ",
        " Prefix: Ctrl+B then:",
        "   c           new project           x   close active",
        "   1..9        focus tab N           n/p next / previous",
        "   h j k l     navigate cells        z   zoom toggle",
        "   s / v       split horiz / vert",
        "   G           cycle grid preset",
        "   C           cycle color mode (mono/state/id/heat/full)",
        "   B           cycle border style",
        "   d           detach               ?   this help",
        " ",
        " Any other key → focused project's stdin",
        " Press any key to dismiss",
    };
    int n = (int)(sizeof(lines)/sizeof(lines[0]));
    boxh = n + 1; (void)boxh;
    for (int i = 0; i < n; i++) {
        move_to(r0 + i, c0 + 1);
        out_write(lines[i]);
    }
    /* wait for one key */
    char c;
    while (read(STDIN_FILENO, &c, 1) <= 0) {
        struct timespec ts = { 0, 20 * 1000000 }; nanosleep(&ts, NULL);
    }
    m->needs_redraw = true;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Key dispatch
 * ────────────────────────────────────────────────────────────────────────── */

static void handle_prefix_command(dsco_mux_t *m, char c) {
    m->prefix_pending = 0;
    if (c >= '1' && c <= '9') {
        int idx = c - '1';
        if (idx < m->count) {
            m->focused = idx;
            dsco_grid_focus_project(&m->grid, idx);
            m->needs_redraw = true;
        }
        return;
    }
    switch (c) {
        case 'c': {
            char root[PATH_MAX];
            if (read_modal_line("new project root path (absolute or relative)", root, sizeof(root)) > 0) {
                mux_create_new(m, root, NULL);
            }
            m->needs_redraw = true;
            return;
        }
        case 'x': {
            if (m->focused >= 0 && m->focused < m->count) {
                char yn[8];
                if (read_modal_line("close active project? [y/N]", yn, sizeof(yn)) > 0
                    && (yn[0] == 'y' || yn[0] == 'Y')) {
                    mux_remove_at(m, m->focused);
                }
            }
            m->needs_redraw = true;
            return;
        }
        case 'n':
            if (m->count > 0) {
                m->focused = (m->focused + 1) % m->count;
                dsco_grid_focus_project(&m->grid, m->focused);
            }
            m->needs_redraw = true; return;
        case 'p':
            if (m->count > 0) {
                m->focused = (m->focused - 1 + m->count) % m->count;
                dsco_grid_focus_project(&m->grid, m->focused);
            }
            m->needs_redraw = true; return;

        /* vim-style cell navigation */
        case 'h': dsco_grid_focus_dir(&m->grid, -1,  0); m->needs_redraw = true; return;
        case 'l': dsco_grid_focus_dir(&m->grid, +1,  0); m->needs_redraw = true; return;
        case 'k': dsco_grid_focus_dir(&m->grid,  0, -1); m->needs_redraw = true; return;
        case 'j': dsco_grid_focus_dir(&m->grid,  0, +1); m->needs_redraw = true; return;

        /* zoom toggle */
        case 'z': dsco_grid_zoom_toggle(&m->grid); m->needs_redraw = true; return;

        /* splits */
        case 's': dsco_grid_split(&m->grid, m->grid.focused_id, DSCO_TILE_HSPLIT); m->needs_redraw = true; return;
        case 'v': dsco_grid_split(&m->grid, m->grid.focused_id, DSCO_TILE_VSPLIT); m->needs_redraw = true; return;

        /* cycle visual modes */
        case 'G': dsco_grid_cycle_preset(&m->grid);     m->needs_redraw = true; return;
        case 'C': dsco_grid_cycle_color_mode(&m->grid); m->needs_redraw = true; return;
        case 'B': dsco_grid_cycle_border(&m->grid);     m->needs_redraw = true; return;
        case 'T': dsco_grid_cycle_theme(&m->grid);      m->needs_redraw = true; return;

        case 'd': m->running = false; return;
        case '?': show_help(m); return;
        default: return;
    }
}

static void handle_byte(dsco_mux_t *m, char c) {
    if (m->prefix_pending) {
        handle_prefix_command(m, c);
        return;
    }
    if (c == MUX_PREFIX) {
        m->prefix_pending = 1;
        m->needs_redraw = true;
        return;
    }
    /* forward to focused project */
    if (m->focused >= 0 && m->focused < m->count) {
        dsco_project_t *p = m->projects[m->focused];
        if (p->worker_in_fd >= 0)
            write(p->worker_in_fd, &c, 1);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Main loop
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_mux_run(const char *api_key, const char *initial_root) {
    dsco_project_registry_ensure();

    dsco_mux_t mux = {0};
    g_mux = &mux;
    mux.count = 0;
    mux.focused = -1;
    dsco_grid_init(&mux.grid);
    mux.running = true;
    mux.needs_redraw = true;
    if (api_key) snprintf(mux.api_key, sizeof(mux.api_key), "%s", api_key);
    term_size(&mux.term_rows, &mux.term_cols);

    /* wake-pipe: drain threads write 1 byte to nudge the render loop. */
    int wake_pipe[2] = { -1, -1 };
    if (pipe(wake_pipe) != 0) { fprintf(stderr, "dsco-mux: pipe(): %s\n", strerror(errno)); return 1; }
    set_nonblock(wake_pipe[0]);
    set_nonblock(wake_pipe[1]);
    mux.wake_fd_r = wake_pipe[0];
    mux.wake_fd_w = wake_pipe[1];

    if (tio_raw() != 0) {
        fprintf(stderr, "dsco-mux: requires a TTY\n");
        return 1;
    }
    signal(SIGWINCH, on_winch);
    signal(SIGTERM,  on_term);
    signal(SIGINT,   on_term);
    signal(SIGPIPE,  SIG_IGN);

    if (initial_root && *initial_root) {
        mux_create_new(&mux, initial_root, NULL);
    }

    int64_t last_render_ms = 0;
    const int64_t frame_budget_ms = 33;  /* ~30 fps cap */
    while (mux.running && !g_term) {
        if (g_resized) {
            g_resized = 0;
            term_size(&mux.term_rows, &mux.term_cols);
            mux.needs_redraw = true;
        }

        if (mux.needs_redraw) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now_ms_v = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now_ms_v - last_render_ms >= frame_budget_ms) {
                render(&mux);
                mux.needs_redraw = false;
                last_render_ms = now_ms_v;
            }
            /* otherwise: defer render to the next poll-wake cycle */
        }

        /* Hyper-parallel architecture: drain threads own all worker I/O.
         * Main loop polls just two fds — stdin and the wake-pipe.
         * Throughput across N projects no longer depends on N. */
        struct pollfd fds[2];
        fds[0].fd = STDIN_FILENO;       fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = mux.wake_fd_r;      fds[1].events = POLLIN; fds[1].revents = 0;

        /* If a redraw is pending but frame budget isn't elapsed yet, wake up
         * exactly when it elapses so we can render. Otherwise idle. */
        int poll_timeout = 1000;
        if (mux.needs_redraw) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now_ms_v = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            int64_t wait = frame_budget_ms - (now_ms_v - last_render_ms);
            poll_timeout = wait < 1 ? 1 : (int)wait;
        }
        int rc = poll(fds, 2, poll_timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            char buf[128];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; i++) handle_byte(&mux, buf[i]);
        }

        if (fds[1].revents & POLLIN) {
            /* drain coalesced wakeup bytes */
            char drain[256];
            while (read(mux.wake_fd_r, drain, sizeof(drain)) > 0) { /* drain */ }
            mux.needs_redraw = true;
        }

        /* reap dead workers */
        for (int i = 0; i < mux.count; i++) {
            if (mux.projects[i]->worker_pid > 0) {
                int status = 0;
                pid_t r = waitpid(mux.projects[i]->worker_pid, &status, WNOHANG);
                if (r == mux.projects[i]->worker_pid) {
                    mux.projects[i]->worker_pid = -1;
                    mux.projects[i]->state = DSCO_PROJECT_DEAD;
                    mux.projects[i]->epoch++;
                    mux.needs_redraw = true;
                }
            }
        }
    }

    /* shutdown */
    for (int i = 0; i < mux.count; i++) dsco_project_close(mux.projects[i]);
    if (mux.wake_fd_r >= 0) close(mux.wake_fd_r);
    if (mux.wake_fd_w >= 0) close(mux.wake_fd_w);
    tio_restore();
    g_mux = NULL;
    return 0;
}
