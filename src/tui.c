#include "tui.h"
#include "config.h"
#include "presence.h"
#include "touchid.h"
#include "dist_logo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Box character sets ───────────────────────────────────────────────── */

static const tui_box_chars_t BOX_ROUND_CHARS = {"╭", "╮", "╰", "╯", "─", "│", "├", "┤"};
static const tui_box_chars_t BOX_SINGLE_CHARS = {"┌", "┐", "└", "┘", "─", "│", "├", "┤"};
static const tui_box_chars_t BOX_DOUBLE_CHARS = {"╔", "╗", "╚", "╝", "═", "║", "╠", "╣"};
static const tui_box_chars_t BOX_HEAVY_CHARS = {"┏", "┓", "┗", "┛", "━", "┃", "┣", "┫"};
static const tui_box_chars_t BOX_ASCII_CHARS = {"+", "+", "+", "+", "-", "|", "+", "+"};

const tui_box_chars_t *tui_box_chars(tui_box_style_t style) {
    switch (style) {
        case BOX_ROUND:
            return &BOX_ROUND_CHARS;
        case BOX_SINGLE:
            return &BOX_SINGLE_CHARS;
        case BOX_DOUBLE:
            return &BOX_DOUBLE_CHARS;
        case BOX_HEAVY:
            return &BOX_HEAVY_CHARS;
        case BOX_ASCII:
            return &BOX_ASCII_CHARS;
    }
    return &BOX_ROUND_CHARS;
}

/* ── Terminal utilities ───────────────────────────────────────────────── */

int tui_term_width(void) {
    struct winsize w;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

int tui_term_height(void) {
    struct winsize w;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return w.ws_row;
    return 24;
}

/* ── Terminal output mutex — serializes cursor-positioned writes ──────── */
static pthread_mutex_t g_term_mutex = PTHREAD_MUTEX_INITIALIZER;

void tui_term_lock(void) {
    pthread_mutex_lock(&g_term_mutex);
}
void tui_term_unlock(void) {
    pthread_mutex_unlock(&g_term_mutex);
}

void tui_cursor_hide(void) {
    fprintf(stderr, "\033[?25l");
}
void tui_cursor_show(void) {
    fprintf(stderr, "\033[?25h");
}
void tui_cursor_move(int r, int c) {
    fprintf(stderr, "\033[%d;%dH", r, c);
}
void tui_clear_screen(void) {
    fprintf(stderr, "\033[2J\033[H");
}
void tui_clear_line(void) {
    fprintf(stderr, "\033[2K\r");
}

/* Full reset for a clean first paint: reset SGR, show cursor, clear the
 * visible screen *and* the scrollback buffer (\033[3J), then home. This wipes
 * any content left by the previous program so the banner doesn't bleed
 * through a stale screen. No-op when stderr isn't a tty, or when the caller
 * sets DSCO_NO_CLEAR=1 (useful when piping/embedding). */
void tui_screen_reset_full(void) {
    if (!isatty(STDERR_FILENO))
        return;
    const char *no_clear = getenv("DSCO_NO_CLEAR");
    if (no_clear && no_clear[0] == '1')
        return;
    fputs("\033[0m\033[?25h\033[3J\033[2J\033[H", stderr);
    fflush(stderr);
}
void tui_save_cursor(void) {
    fprintf(stderr, "\033[s");
}
void tui_restore_cursor(void) {
    fprintf(stderr, "\033[u");
}

/* ── Visible string length (strip ANSI) ───────────────────────────────── */

static int visible_len(const char *s) {
    return tui_str_display_width(s);
}

/* ── Repeat a string n times to stderr ────────────────────────────────── */

static void repeat_str(const char *s, int n) {
    for (int i = 0; i < n; i++)
        fprintf(stderr, "%s", s);
}

/* ── Box drawing ──────────────────────────────────────────────────────── */

void tui_box(const char *title, const char *body, tui_box_style_t style, const char *border_color,
             int width) {
    if (width <= 0)
        width = tui_term_width();
    const tui_box_chars_t *bc = tui_box_chars(style);
    const char *col = border_color ? border_color : "";
    const char *rst = border_color ? TUI_RESET : "";

    /* Top border */
    fprintf(stderr, "%s%s", col, bc->tl);
    if (title && *title) {
        fprintf(stderr, "%s ", bc->h);
        fprintf(stderr, "%s%s%s%s ", TUI_BOLD, title, rst, col);
        int title_vis = visible_len(title);
        int remaining = width - 4 - title_vis;
        if (remaining > 0)
            repeat_str(bc->h, remaining);
    } else {
        repeat_str(bc->h, width - 2);
    }
    fprintf(stderr, "%s%s\n", bc->tr, rst);

    /* Body lines */
    if (body) {
        const char *p = body;
        while (*p) {
            const char *nl = strchr(p, '\n');
            int line_len = nl ? (int)(nl - p) : (int)strlen(p);

            fprintf(stderr, "%s%s%s ", col, bc->v, rst);

            /* Print line content */
            char line_buf[2048];
            int copy_len =
                line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
            memcpy(line_buf, p, copy_len);
            line_buf[copy_len] = '\0';
            fprintf(stderr, "%s", line_buf);

            /* Pad to width */
            int vis = visible_len(line_buf);
            int pad = width - 3 - vis;
            if (pad > 0)
                for (int i = 0; i < pad; i++)
                    fputc(' ', stderr);

            fprintf(stderr, "%s%s%s\n", col, bc->v, rst);

            p = nl ? nl + 1 : p + line_len;
        }
    }

    /* Bottom border */
    fprintf(stderr, "%s%s", col, bc->bl);
    repeat_str(bc->h, width - 2);
    fprintf(stderr, "%s%s\n", bc->br, rst);
}

/* ── Divider ──────────────────────────────────────────────────────────── */

void tui_divider(tui_box_style_t style, const char *color, int width) {
    if (width <= 0)
        width = tui_term_width();
    const tui_box_chars_t *bc = tui_box_chars(style);
    const char *col = color ? color : "";
    const char *rst = color ? TUI_RESET : "";
    fprintf(stderr, "%s", col);
    repeat_str(bc->h, width);
    fprintf(stderr, "%s\n", rst);
}

/* ── Panel ────────────────────────────────────────────────────────────── */

void tui_panel(const tui_panel_t *p) {
    tui_box(p->title, p->body, p->style, p->color ? p->color : TUI_DIM,
            p->width > 0 ? p->width : tui_term_width());
}

/* ── Spinners ─────────────────────────────────────────────────────────── */

/* Spinner frames — resolved from glyph tier at runtime */
static void spinner_get_frames(tui_spinner_type_t type, const char *const **out_frames,
                               int *out_count) {
    const tui_glyphs_t *g = tui_glyph();
    switch (type) {
        case SPINNER_DOTS:
            *out_frames = g->spin_dots;
            *out_count = g->spin_dots_n;
            break;
        case SPINNER_BRAILLE:
            *out_frames = g->spin_thick;
            *out_count = g->spin_thick_n;
            break;
        case SPINNER_LINE:
            *out_frames = g->spin_line;
            *out_count = g->spin_line_n;
            break;
        case SPINNER_ARROW:
            *out_frames = g->spin_arrow;
            *out_count = g->spin_arrow_n;
            break;
        case SPINNER_STAR:
            *out_frames = g->spin_star;
            *out_count = g->spin_star_n;
            break;
        case SPINNER_PULSE:
            *out_frames = g->spin_pulse;
            *out_count = g->spin_pulse_n;
            break;
        default:
            *out_frames = g->spin_dots;
            *out_count = g->spin_dots_n;
            break;
    }
}

void tui_spinner_init(tui_spinner_t *s, tui_spinner_type_t type, const char *label,
                      const char *color) {
    s->type = type;
    s->frame = 0;
    s->label = label;
    s->color = color ? color : tui_named_fg("ui.spinner");
    s->active = true;
}

void tui_spinner_tick(tui_spinner_t *s) {
    if (!s->active)
        return;
    const char *const *frames;
    int count;
    spinner_get_frames(s->type, &frames, &count);
    if (count <= 0)
        return;

    tui_clear_line();
    fprintf(stderr, "  %s%s%s %s%s%s", s->color, frames[s->frame % count], TUI_RESET, TUI_DIM,
            s->label ? s->label : "", TUI_RESET);
    fflush(stderr);
    s->frame++;
}

void tui_spinner_done(tui_spinner_t *s, const char *final_label) {
    s->active = false;
    tui_clear_line();
    fprintf(stderr, "  %s%s%s %s%s%s\n", tui_named_fg("ui.spinner.done"), tui_glyph()->florette,
            TUI_RESET, TUI_DIM, final_label ? final_label : "done", TUI_RESET);
}

/* ── Progress bar ─────────────────────────────────────────────────────── */

void tui_progress(const char *label, double pct, int width, const char *fill_color,
                  const char *empty_color) {
    if (width <= 0)
        width = tui_term_width() - 20;
    if (pct < 0)
        pct = 0;
    if (pct > 1)
        pct = 1;

    int label_len = label ? visible_len(label) : 0;
    int bar_width = width - label_len - 10;
    if (bar_width < 10)
        bar_width = 10;

    int filled = (int)(pct * bar_width);
    int empty = bar_width - filled;

    const char *fc = fill_color ? fill_color : tui_named_fg("ui.progress.fill");
    const char *ec = empty_color ? empty_color : TUI_DIM;
    (void)filled;
    (void)empty;

    tui_clear_line();
    if (label)
        fprintf(stderr, "  %s ", label);
    /* Subpixel: resolve the leading edge to 1/8 of a cell instead of snapping
     * to whole cells — 8× the effective length resolution. */
    tui_subpixel_hbar(stderr, pct, bar_width, fc, "░", ec);
    fprintf(stderr, " %3.0f%%\n", pct * 100);
}

/* ── Table ────────────────────────────────────────────────────────────── */

void tui_table_init(tui_table_t *t, int cols, const char *header_color) {
    memset(t, 0, sizeof(*t));
    t->col_count = cols < TUI_TABLE_MAX_COLS ? cols : TUI_TABLE_MAX_COLS;
    t->header_color = header_color ? header_color : tui_named_fg("ui.table.header");
    t->border_color = tui_named_fg("ui.table.border");
    t->style = BOX_ROUND;
}

void tui_table_header(tui_table_t *t, ...) {
    va_list args;
    va_start(args, t);
    for (int i = 0; i < t->col_count; i++) {
        t->headers[i] = va_arg(args, const char *);
    }
    va_end(args);
}

void tui_table_row(tui_table_t *t, ...) {
    if (t->row_count >= TUI_TABLE_MAX_ROWS)
        return;
    va_list args;
    va_start(args, t);
    for (int i = 0; i < t->col_count; i++) {
        t->rows[t->row_count][i] = va_arg(args, const char *);
    }
    t->row_count++;
    va_end(args);
}

void tui_table_render(const tui_table_t *t, int width) {
    if (width <= 0)
        width = tui_term_width();

    /* Calculate column widths */
    int col_widths[TUI_TABLE_MAX_COLS] = {0};
    for (int c = 0; c < t->col_count; c++) {
        if (t->headers[c]) {
            int w = visible_len(t->headers[c]);
            if (w > col_widths[c])
                col_widths[c] = w;
        }
        for (int r = 0; r < t->row_count; r++) {
            if (t->rows[r][c]) {
                int w = visible_len(t->rows[r][c]);
                if (w > col_widths[c])
                    col_widths[c] = w;
            }
        }
    }

    const tui_box_chars_t *bc = tui_box_chars(t->style);
    const char *dcol = t->border_color ? t->border_color : "";
    const char *rst = TUI_RESET;

    /* Header */
    fprintf(stderr, "  %s", dcol);
    for (int c = 0; c < t->col_count; c++) {
        fprintf(stderr, "%s%-*s%s", t->header_color ? t->header_color : "", col_widths[c] + 2,
                t->headers[c] ? t->headers[c] : "", rst);
        if (c < t->col_count - 1)
            fprintf(stderr, " %s%s%s ", dcol, bc->v, rst);
    }
    fprintf(stderr, "\n");

    /* Separator */
    fprintf(stderr, "  %s", dcol);
    for (int c = 0; c < t->col_count; c++) {
        repeat_str(bc->h, col_widths[c] + 2);
        if (c < t->col_count - 1) {
            fprintf(stderr, "%s", bc->h);
            repeat_str(bc->h, 1);
            fprintf(stderr, "%s", bc->h);
        }
    }
    fprintf(stderr, "%s\n", rst);

    /* Rows */
    for (int r = 0; r < t->row_count; r++) {
        fprintf(stderr, "  ");
        for (int c = 0; c < t->col_count; c++) {
            const char *cell = t->rows[r][c] ? t->rows[r][c] : "";
            int vis = visible_len(cell);
            fprintf(stderr, "%s", cell);
            int pad = col_widths[c] + 2 - vis;
            if (pad > 0)
                for (int i = 0; i < pad; i++)
                    fputc(' ', stderr);
            if (c < t->col_count - 1)
                fprintf(stderr, " %s%s%s ", dcol, bc->v, rst);
        }
        fprintf(stderr, "\n");
    }
}

/* ── Badges & tags ────────────────────────────────────────────────────── */

void tui_badge(const char *text, const char *fg, const char *bg) {
    fprintf(stderr, " %s%s %s %s ", bg ? bg : TUI_BG_BLUE, fg ? fg : TUI_WHITE, text, TUI_RESET);
}

void tui_tag(const char *text, const char *color) {
    fprintf(stderr, "%s[%s]%s", color ? color : TUI_CYAN, text, TUI_RESET);
}

/* ── Convenience output ───────────────────────────────────────────────── */

void tui_header(const char *text, const char *color) {
    bool use_rgb = tui_detect_color_level() >= TUI_COLOR_256;
    if (use_rgb) {
        /* ⏺ in warm orange, text in caller's color */
        fprintf(stderr, "\n\033[38;2;255;95;0m" TUI_RECORD "\033[0m %s%s%s%s\n",
                color ? color : TUI_BWHITE, TUI_BOLD, text, TUI_RESET);
    } else {
        fprintf(stderr, "\n%s%s" TUI_RECORD " %s%s\n", color ? color : TUI_BWHITE, TUI_BOLD, text,
                TUI_RESET);
    }
}

void tui_subheader(const char *text) {
    fprintf(stderr, "  %s%s%s\n", TUI_DIM, text, TUI_RESET);
}

void tui_info(const char *text) {
    fprintf(stderr, "  %sℹ%s %s\n", tui_named_fg("ui.info"), TUI_RESET, text);
}

void tui_success(const char *text) {
    fprintf(stderr, "  %s✓%s %s\n", tui_named_fg("ui.success"), TUI_RESET, text);
}

void tui_warning(const char *text) {
    fprintf(stderr, "  %s⚠%s %s\n", tui_named_fg("ui.warning"), TUI_RESET, text);
}

void tui_error(const char *text) {
    fprintf(stderr, "  %s✗%s %s\n", tui_named_fg("ui.error"), TUI_RESET, text);
}

/* Forward declarations for functions defined later */
static void welcome_animated(const char *model, int core_count, int total_count,
                             const char *version);
static void fg_color_auto(tui_rgb_t c);

/* ── Welcome banner ───────────────────────────────────────────────────── */

void tui_welcome(const char *model, int core_count, int total_count, const char *version) {
    /* DSCO_SPLASH=off  → silent (no banner at all)
       DSCO_SPLASH=compact → single status line, no animation
       anything else   → full animated logo (default) */
    const char *splash_env = getenv("DSCO_SPLASH");
    if (splash_env && strcasecmp(splash_env, "off") == 0)
        return; /* silent startup */

    if (splash_env && strcasecmp(splash_env, "compact") == 0) {
        /* One-line compact header: dsco v1.0.1 · model · N tools (M loadable) */
        const tui_glyphs_t *gl = tui_glyph();
        fprintf(stderr, "%s%s%s dsco %sv%s%s  %s·%s  %s%s%s  %s·%s  %s%d tools%s %s(%d loadable)%s\n",
                TUI_BMAGENTA, gl->diamond, TUI_RESET,
                TUI_BOLD, version, TUI_RESET,
                TUI_DIM, TUI_RESET,
                TUI_CYAN, model, TUI_RESET,
                TUI_DIM, TUI_RESET,
                TUI_GREEN, core_count, TUI_RESET,
                TUI_DIM, total_count - core_count, TUI_RESET);
        return;
    }

    /* Use animated gradient version when truecolor or 256-color is available */
    if (tui_detect_color_level() >= TUI_COLOR_256) {
        welcome_animated(model, core_count, total_count, version);
        return;
    }

    /* Fallback: original 16-color instant print */
    int w = tui_term_width();

    const char *logo = "     %s██████╗%s  %s███████╗%s  %s██████╗%s  %s██████╗%s \n"
                       "     %s██╔══██╗%s %s██╔════╝%s %s██╔════╝%s %s██╔═══██╗%s\n"
                       "     %s██║  ██║%s %s███████╗%s %s██║%s      %s██║   ██║%s\n"
                       "     %s██║  ██║%s %s╚════██║%s %s██║%s      %s██║   ██║%s\n"
                       "     %s██████╔╝%s %s███████║%s %s╚██████╗%s %s╚██████╔╝%s\n"
                       "     %s╚═════╝%s  %s╚══════╝%s  %s╚═════╝%s  %s╚═════╝%s \n";

    /* Full-width top border */
    fprintf(stderr, "\n%s", TUI_DIM);
    for (int i = 0; i < w; i++)
        fprintf(stderr, "━");
    fprintf(stderr, "%s\n\n", TUI_RESET);

    fprintf(stderr, logo, TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET,
            TUI_BGREEN, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE,
            TUI_RESET, TUI_BGREEN, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET,
            TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BCYAN,
            TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET, TUI_BMAGENTA, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET, TUI_BMAGENTA,
            TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET);
    fprintf(stderr, "\n");

    char info[512];
    snprintf(info, sizeof(info),
             "%s✦%s %sv%s%s  %s·%s  %s%s%s  %s·%s  %s%d tools%s %s(%d loadable)%s  %s·%s  "
             "%sstreaming%s  %s·%s  %sswarm-ready%s %s✦%s",
             TUI_BMAGENTA, TUI_RESET, TUI_BOLD, version, TUI_RESET, TUI_DIM, TUI_RESET, TUI_CYAN,
             model, TUI_RESET, TUI_DIM, TUI_RESET, TUI_GREEN, core_count, TUI_RESET, TUI_DIM,
             total_count - core_count, TUI_RESET, TUI_DIM, TUI_RESET, TUI_BMAGENTA, TUI_RESET,
             TUI_DIM, TUI_RESET, TUI_BYELLOW, TUI_RESET, TUI_BMAGENTA, TUI_RESET);
    fprintf(stderr, "     %s\n\n", info);

    fprintf(stderr,
            "     %s◆%s %sAST introspection%s  %s·%s  %s◆%s %sSub-agent swarms%s  %s·%s  %s◆%s "
            "%sStreaming I/O%s\n",
            TUI_BMAGENTA, TUI_RESET, TUI_DIM, TUI_RESET, TUI_DIM, TUI_RESET, TUI_BCYAN, TUI_RESET,
            TUI_DIM, TUI_RESET, TUI_DIM, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr,
            "     %s◆%s %sCrypto toolkit%s     %s·%s  %s◆%s %sCoroutine pipelines%s %s·%s %s◆%s "
            "%sPlugin system%s\n",
            TUI_BRED, TUI_RESET, TUI_DIM, TUI_RESET, TUI_DIM, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_DIM, TUI_RESET, TUI_DIM, TUI_RESET, TUI_BYELLOW, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr, "\n");

    /* Full-width bottom border */
    fprintf(stderr, "%s", TUI_DIM);
    for (int i = 0; i < w; i++)
        fprintf(stderr, "━");
    fprintf(stderr, "%s\n\n", TUI_RESET);
}

/* ── Streaming wrappers ───────────────────────────────────────────────── */

static bool s_stream_active = false;

void tui_stream_start(void) {
    s_stream_active = true;
}

void tui_stream_text(const char *text) {
    tui_term_lock();
    fputs(text, stderr);
    fflush(stderr);
    tui_term_unlock();
}

void tui_stream_tool(const char *name, const char *id) {
    (void)id;
    if (s_stream_active) {
        fprintf(stderr, "\n");
        s_stream_active = false;
    }
    fprintf(stderr, "  %s%s⚡%s %s%s%s\n", TUI_BOLD, TUI_CYAN, TUI_RESET, TUI_DIM, name, TUI_RESET);
}

void tui_stream_tool_result(const char *name, bool ok, const char *preview) {
    (void)name;
    const tui_glyphs_t *gl = tui_glyph();
    const char *icon = ok ? gl->ok : gl->fail;
    const char *color = ok ? TUI_GREEN : TUI_RED;

    char trunc[128];
    if (preview) {
        const char *nl = strchr(preview, '\n');
        int len = nl ? (int)(nl - preview) : (int)strlen(preview);
        if (len > 100)
            len = 100;
        memcpy(trunc, preview, len);
        trunc[len] = '\0';
    } else {
        trunc[0] = '\0';
    }

    fprintf(stderr, "    %s%s%s %s%s%s\n", color, icon, TUI_RESET, TUI_DIM, trunc, TUI_RESET);
}

void tui_stream_end(void) {
    if (s_stream_active) {
        printf("\n");
        s_stream_active = false;
    }
}

/* ── Swarm panel ──────────────────────────────────────────────────────── */

void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width) {
    if (width <= 0)
        width = tui_term_width();

    fprintf(stderr, "\n");
    tui_header("Swarm Status", TUI_BYELLOW);

    for (int i = 0; i < count; i++) {
        tui_swarm_entry_t *e = &entries[i];

        const tui_glyphs_t *gl = tui_glyph();
        const char *status_color = TUI_DIM;
        const char *status_icon = gl->circle_open;

        if (strcmp(e->status, "running") == 0) {
            status_color = TUI_BCYAN;
            status_icon = gl->circle_dot;
        } else if (strcmp(e->status, "done") == 0) {
            status_color = TUI_GREEN;
            status_icon = gl->ok;
        } else if (strcmp(e->status, "error") == 0) {
            status_color = TUI_RED;
            status_icon = gl->fail;
        }

        fprintf(stderr, "  %s%s%s %s#%d%s %s%s%s", status_color, status_icon, TUI_RESET, TUI_DIM,
                e->id, TUI_RESET, TUI_BOLD, e->task, TUI_RESET);

        if (e->progress > 0 && e->progress < 1.0) {
            int bar_w = 20;
            int filled = (int)(e->progress * bar_w);
            fprintf(stderr, " %s", TUI_GREEN);
            for (int j = 0; j < filled; j++)
                fprintf(stderr, "%s", tui_glyph()->block_med);
            fprintf(stderr, "%s", TUI_DIM);
            for (int j = filled; j < bar_w; j++)
                fprintf(stderr, "▯");
            fprintf(stderr, "%s", TUI_RESET);
        }
        fprintf(stderr, "\n");

        if (e->last_output && strlen(e->last_output) > 0) {
            /* Show last line of output */
            const char *last = e->last_output;
            const char *scan = last;
            while (*scan) {
                if (*scan == '\n' && *(scan + 1))
                    last = scan + 1;
                scan++;
            }
            int out_len = (int)strlen(last);
            if (out_len > width - 8)
                out_len = width - 8;
            fprintf(stderr, "    %s%.*s%s\n", TUI_DIM, out_len, last, TUI_RESET);
        }
    }
    fprintf(stderr, "\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * GLYPH TIER SYSTEM — embedded glyph tables with ASCII/Unicode/Full tiers
 * ══════════════════════════════════════════════════════════════════════════ */

static tui_glyph_tier_t s_glyph_tier = (tui_glyph_tier_t)-1;

/* Nerd Font codepoints as UTF-8 byte sequences.
 * Nerd Font PUA glyphs are in U+E000–U+F8FF (3-byte UTF-8)
 * and U+F0000–U+10FFFF (4-byte UTF-8 for Material Design). */
#define NF(hex) hex /* just documentation — we use raw UTF-8 bytes */

/* ── Nerd Font tier: modern 2026 terminals with patched fonts ──────── */
static const tui_glyphs_t GLYPHS_NERD = {
    /* Status — Nerd Font codicons/FA */
    .ok = "\xef\x80\x8c",   /* nf-fa-check        U+F00C */
    .fail = "\xef\x80\x8d", /* nf-fa-times        U+F00D */
    .warn = "\xef\x81\xb1", /* nf-fa-warning      U+F071 */
    .info = "\xef\x84\xa9", /* nf-fa-info_circle  U+F129 */
    /* Bullets — Nerd Font */
    .bullet = "\xef\x84\x92",       /* nf-fa-circle       U+F111 */
    .circle_open = "\xef\x84\x90",  /* nf-fa-circle_o     U+F10C */
    .circle_dot = "\xef\x84\x92",   /* nf-fa-circle       U+F111 */
    .circle_ring = "\xef\x84\x90",  /* nf-fa-circle_o     U+F10C */
    .diamond = "\xef\x88\x99",      /* nf-fa-diamond      U+F219 */
    .diamond_open = "\xef\x88\x99", /* nf-fa-diamond      U+F219 */
    .sparkle = "\xef\x83\xab",      /* nf-fa-star         U+F0EB -> lightbulb */
    .florette = "\xef\x80\x8c",     /* nf-fa-check        U+F00C */
    /* Arrows — Nerd Font */
    .arrow_right = "\xef\x81\xa1", /* nf-fa-arrow_right  U+F061 */
    .arrow_left = "\xef\x81\xa0",  /* nf-fa-arrow_left   U+F060 */
    .arrow_up = "\xef\x81\xa2",    /* nf-fa-arrow_up     U+F062 */
    .arrow_down = "\xef\x81\xa3",  /* nf-fa-arrow_down   U+F063 */
    .arrow_cycle = "\xef\x80\xa1", /* nf-fa-refresh      U+F021 */
    /* Blocks — standard Unicode (universally supported) */
    .block_full = "\xe2\x96\x88",
    .block_med = "\xe2\x96\xae",
    .block_light = "\xe2\x96\x91",
    .block_dark = "\xe2\x96\x93",
    .vblock = {" ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85",
               "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"},
    /* Spinners — braille (universally supported with nerd fonts) */
    .spin_dots = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
                  "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    .spin_thick = {"\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf", "\xe2\xa1\xbf",
                   "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    .spin_orbit = {"\xe2\x97\x9c", "\xe2\x97\x9d", "\xe2\x97\x9e", "\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0", "\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {"\xe2\x97\x90", "\xe2\x97\x93", "\xe2\x97\x91", "\xe2\x97\x92"},
    .spin_pulse_n = 4,
    .spin_line = {"-", "\\", "|", "/"},
    .spin_line_n = 4,
    .spin_arrow = {"\xe2\x86\x90", "\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97", "\xe2\x86\x92",
                   "\xe2\x86\x98", "\xe2\x86\x93", "\xe2\x86\x99"},
    .spin_arrow_n = 8,
    .spin_star = {"\xe2\x9c\xb6", "\xe2\x9c\xb8", "\xe2\x9c\xb9", "\xe2\x9c\xba", "\xe2\x9c\xb9",
                  "\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* Icons — Nerd Font */
    .icon_think = "\xef\x83\xab",     /* nf-fa-lightbulb_o  U+F0EB */
    .icon_lightning = "\xef\x83\xa7", /* nf-fa-bolt         U+F0E7 */
    .icon_gear = "\xef\x80\x93",      /* nf-fa-cog          U+F013 */
    .icon_timer = "\xef\x80\x97",     /* nf-fa-clock_o      U+F017 */
    .icon_lock = "\xef\x80\xa3",      /* nf-fa-lock         U+F023 */
    .icon_money = "\xef\x85\x95",     /* nf-fa-money        U+F155 */
    .icon_globe = "\xef\x82\xac",     /* nf-fa-globe        U+F0AC */
    .icon_rocket = "\xef\x84\xb5",    /* nf-fa-rocket       U+F135 */
    .icon_fire = "\xef\x81\xad",      /* nf-fa-fire         U+F06D */
    .icon_link = "\xef\x83\x81",      /* nf-fa-link         U+F0C1 */
    .icon_eyes = "\xef\x81\xae",      /* nf-fa-eye          U+F06E */
    /* Nerd Font extras */
    .icon_folder = "\xef\x81\xbc",   /* nf-fa-folder       U+F07C */
    .icon_file = "\xef\x85\x9b",     /* nf-fa-file_text_o  U+F15B (really U+F0F6) */
    .icon_code = "\xef\x84\xa1",     /* nf-fa-code         U+F121 */
    .icon_terminal = "\xef\x84\xa0", /* nf-fa-terminal     U+F120 */
    .icon_git = "\xef\x84\xa6",      /* nf-fa-code_fork    U+F126 */
    .icon_database = "\xef\x87\x80", /* nf-fa-database     U+F1C0 */
    .icon_cloud = "\xef\x83\x82",    /* nf-fa-cloud        U+F0C2 */
    .icon_bug = "\xef\x86\x88",      /* nf-fa-bug          U+F188 */
    .icon_cpu = "\xef\x88\x9b",      /* nf-fa-microchip    U+F21B (close) */
    .icon_network = "\xef\x83\xa0",  /* nf-fa-sitemap      U+F0E8 */
    .icon_key = "\xef\x82\x84",      /* nf-fa-key          U+F084 */
    .icon_shield = "\xef\x84\xb2",   /* nf-fa-shield       U+F132 */
    .icon_search = "\xef\x80\x82",   /* nf-fa-search       U+F002 */
    .icon_download = "\xef\x80\x99", /* nf-fa-download     U+F019 */
    .icon_upload = "\xef\x82\x93",   /* nf-fa-upload       U+F093 */
    .icon_sync = "\xef\x80\xa1",     /* nf-fa-refresh      U+F021 */
    .icon_play = "\xef\x81\x8b",     /* nf-fa-play         U+F04B */
    .icon_pause = "\xef\x81\x8c",    /* nf-fa-pause        U+F04C */
    .icon_stop = "\xef\x81\x8d",     /* nf-fa-stop         U+F04D */
    .icon_skip = "\xef\x81\x8e",     /* nf-fa-forward      U+F04E */
    .icon_chat = "\xef\x81\xb5",     /* nf-fa-comment      U+F075 */
    .icon_robot = "\xef\x84\xa4",    /* nf-fa-android      U+F17B (close) */
    .icon_brain = "\xef\x83\xab",    /* nf-fa-lightbulb_o  U+F0EB */
    .icon_wand = "\xef\x83\x90",     /* nf-fa-magic        U+F0D0 */
    .icon_graph = "\xef\x83\xa0",    /* nf-fa-sitemap      U+F0E8 */
    /* Powerline separators */
    .pl_right = "\xee\x82\xb0",       /* U+E0B0 */
    .pl_right_thin = "\xee\x82\xb1",  /* U+E0B1 */
    .pl_left = "\xee\x82\xb2",        /* U+E0B2 */
    .pl_left_thin = "\xee\x82\xb3",   /* U+E0B3 */
    .pl_round_right = "\xee\x82\xb4", /* U+E0B4 */
    .pl_round_left = "\xee\x82\xb6",  /* U+E0B6 */
    /* Box drawing */
    .hline = "\xe2\x94\x80",
    .hline_heavy = "\xe2\x94\x81",
    .vline = "\xe2\x94\x82",
    .corner_tl = "\xe2\x95\xad",
    .corner_tr = "\xe2\x95\xae",
    .corner_bl = "\xe2\x95\xb0",
    .corner_br = "\xe2\x95\xaf",
    /* Trail dots */
    .dot_large = "\xe2\x80\xa2",
    .dot_medium = "\xc2\xb7",
    .dot_small = ".",
};

/* ── Full tier: emoji + all Unicode ──────────────────────────────────── */
static const tui_glyphs_t GLYPHS_FULL = {
    /* Status */
    .ok = "\xe2\x9c\x93",   /* ✓ */
    .fail = "\xe2\x9c\x97", /* ✗ */
    .warn = "\xe2\x9a\xa0", /* ⚠ */
    .info = "\xe2\x84\xb9", /* ℹ */
    /* Bullets */
    .bullet = "\xe2\x97\x8f",       /* ● */
    .circle_open = "\xe2\x97\x8b",  /* ○ */
    .circle_dot = "\xe2\x97\x89",   /* ◉ */
    .circle_ring = "\xe2\x97\x8e",  /* ◎ */
    .diamond = "\xe2\x97\x86",      /* ◆ */
    .diamond_open = "\xe2\x97\x87", /* ◇ */
    .sparkle = "\xe2\x9c\xa6",      /* ✦ */
    .florette = "\xe2\x9c\xbf",     /* ✿ */
    /* Arrows */
    .arrow_right = "\xe2\x86\x92", /* → */
    .arrow_left = "\xe2\x86\x90",  /* ← */
    .arrow_up = "\xe2\x96\xb2",    /* ▲ */
    .arrow_down = "\xe2\x96\xbc",  /* ▼ */
    .arrow_cycle = "\xe2\x86\xbb", /* ↻ */
    /* Blocks */
    .block_full = "\xe2\x96\x88",  /* █ */
    .block_med = "\xe2\x96\xae",   /* ▮ */
    .block_light = "\xe2\x96\x91", /* ░ */
    .block_dark = "\xe2\x96\x93",  /* ▓ */
    .vblock = {" ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85",
               "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"},
    /* Spinners — braille dots */
    .spin_dots = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
                  "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    /* Spinners — thick braille */
    .spin_thick = {"\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf", "\xe2\xa1\xbf",
                   "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    /* Spinners — orbital arcs */
    .spin_orbit = {"\xe2\x97\x9c", "\xe2\x97\x9d", "\xe2\x97\x9e", "\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0", "\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    /* Spinners — pulse */
    .spin_pulse = {"\xe2\x97\x90", "\xe2\x97\x93", "\xe2\x97\x91", "\xe2\x97\x92"},
    .spin_pulse_n = 4,
    /* Spinners — line */
    .spin_line = {"-", "\\", "|", "/"},
    .spin_line_n = 4,
    /* Spinners — arrow */
    .spin_arrow = {"\xe2\x86\x90", "\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97", "\xe2\x86\x92",
                   "\xe2\x86\x98", "\xe2\x86\x93", "\xe2\x86\x99"},
    .spin_arrow_n = 8,
    /* Spinners — star */
    .spin_star = {"\xe2\x9c\xb6", "\xe2\x9c\xb8", "\xe2\x9c\xb9", "\xe2\x9c\xba", "\xe2\x9c\xb9",
                  "\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* Icons — emoji */
    .icon_think = "\xf0\x9f\xa7\xa0",  /* 🧠 */
    .icon_lightning = "\xe2\x9a\xa1",  /* ⚡ */
    .icon_gear = "\xe2\x9a\x99",       /* ⚙ */
    .icon_timer = "\xe2\x8f\xb1",      /* ⏱ */
    .icon_lock = "\xf0\x9f\x94\x92",   /* 🔒 */
    .icon_money = "\xf0\x9f\x92\xb0",  /* 💰 */
    .icon_globe = "\xf0\x9f\x8c\x90",  /* 🌐 */
    .icon_rocket = "\xf0\x9f\x9a\x80", /* 🚀 */
    .icon_fire = "\xf0\x9f\x94\xa5",   /* 🔥 */
    .icon_link = "\xf0\x9f\x94\x97",   /* 🔗 */
    .icon_eyes = "\xf0\x9f\x91\x80",   /* 👀 */
    /* Box drawing */
    .hline = "\xe2\x94\x80",       /* ─ */
    .hline_heavy = "\xe2\x94\x81", /* ━ */
    .vline = "\xe2\x94\x82",       /* │ */
    .corner_tl = "\xe2\x95\xad",   /* ╭ */
    .corner_tr = "\xe2\x95\xae",   /* ╮ */
    .corner_bl = "\xe2\x95\xb0",   /* ╰ */
    .corner_br = "\xe2\x95\xaf",   /* ╯ */
    /* Trail dots */
    .dot_large = "\xe2\x80\xa2", /* • */
    .dot_medium = "\xc2\xb7",    /* · */
    .dot_small = ".",
};

/* ── Unicode tier: BMP only, no emoji ────────────────────────────────── */
static const tui_glyphs_t GLYPHS_UNICODE = {
    .ok = "\xe2\x9c\x93",
    .fail = "\xe2\x9c\x97",
    .warn = "(!)",
    .info = "(i)",
    .bullet = "\xe2\x97\x8f",
    .circle_open = "\xe2\x97\x8b",
    .circle_dot = "\xe2\x97\x89",
    .circle_ring = "\xe2\x97\x8e",
    .diamond = "\xe2\x97\x86",
    .diamond_open = "\xe2\x97\x87",
    .sparkle = "\xe2\x9c\xa6",
    .florette = "\xe2\x9c\xbf",
    .arrow_right = "\xe2\x86\x92",
    .arrow_left = "\xe2\x86\x90",
    .arrow_up = "\xe2\x96\xb2",
    .arrow_down = "\xe2\x96\xbc",
    .arrow_cycle = "\xe2\x86\xbb",
    .block_full = "\xe2\x96\x88",
    .block_med = "\xe2\x96\xae",
    .block_light = "\xe2\x96\x91",
    .block_dark = "\xe2\x96\x93",
    .vblock = {" ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84", "\xe2\x96\x85",
               "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"},
    .spin_dots = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
                  "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    .spin_thick = {"\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf", "\xe2\xa1\xbf",
                   "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    .spin_orbit = {"\xe2\x97\x9c", "\xe2\x97\x9d", "\xe2\x97\x9e", "\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0", "\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {"\xe2\x97\x90", "\xe2\x97\x93", "\xe2\x97\x91", "\xe2\x97\x92"},
    .spin_pulse_n = 4,
    .spin_line = {"-", "\\", "|", "/"},
    .spin_line_n = 4,
    .spin_arrow = {"\xe2\x86\x90", "\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97", "\xe2\x86\x92",
                   "\xe2\x86\x98", "\xe2\x86\x93", "\xe2\x86\x99"},
    .spin_arrow_n = 8,
    .spin_star = {"\xe2\x9c\xb6", "\xe2\x9c\xb8", "\xe2\x9c\xb9", "\xe2\x9c\xba", "\xe2\x9c\xb9",
                  "\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* No emoji — use BMP symbols instead */
    .icon_think = "\xc2\xa7",         /* § */
    .icon_lightning = "\xe2\x9a\xa1", /* ⚡ (BMP) */
    .icon_gear = "\xe2\x9a\x99",      /* ⚙ (BMP) */
    .icon_timer = "\xe2\x97\x8b",     /* ○ (fallback) */
    .icon_lock = "#",
    .icon_money = "$",
    .icon_globe = "\xe2\x97\x89",  /* ◉ */
    .icon_rocket = "\xe2\x96\xb2", /* ▲ */
    .icon_fire = "~",
    .icon_link = "=",
    .icon_eyes = "\xe2\x97\x8b", /* ○ */
    .hline = "\xe2\x94\x80",
    .hline_heavy = "\xe2\x94\x81",
    .vline = "\xe2\x94\x82",
    .corner_tl = "\xe2\x95\xad",
    .corner_tr = "\xe2\x95\xae",
    .corner_bl = "\xe2\x95\xb0",
    .corner_br = "\xe2\x95\xaf",
    .dot_large = "\xe2\x80\xa2",
    .dot_medium = "\xc2\xb7",
    .dot_small = ".",
};

/* ── ASCII tier: pure 7-bit ASCII ────────────────────────────────────── */
static const tui_glyphs_t GLYPHS_ASCII = {
    .ok = "+",
    .fail = "x",
    .warn = "!",
    .info = "i",
    .bullet = "*",
    .circle_open = "o",
    .circle_dot = "@",
    .circle_ring = "O",
    .diamond = "*",
    .diamond_open = "<>",
    .sparkle = "*",
    .florette = "*",
    .arrow_right = "->",
    .arrow_left = "<-",
    .arrow_up = "^",
    .arrow_down = "v",
    .arrow_cycle = "~",
    .block_full = "#",
    .block_med = "=",
    .block_light = ".",
    .block_dark = "#",
    .vblock = {" ", "_", "_", ".", ".", ":", ":", "#", "#"},
    .spin_dots = {"-", "\\", "|", "/", "-", "\\", "|", "/", "-", "\\"},
    .spin_dots_n = 4,
    .spin_thick = {"-", "\\", "|", "/", "-", "\\", "|", "/"},
    .spin_thick_n = 4,
    .spin_orbit = {"-", "\\", "|", "/"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"~", "~"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {"-", "\\", "|", "/"},
    .spin_pulse_n = 4,
    .spin_line = {"-", "\\", "|", "/"},
    .spin_line_n = 4,
    .spin_arrow = {"<", "\\", "^", "/", ">", "\\", "v", "/"},
    .spin_arrow_n = 8,
    .spin_star = {"*", "+", "*", "+", "*", "+"},
    .spin_star_n = 6,
    .icon_think = "?",
    .icon_lightning = "!",
    .icon_gear = "*",
    .icon_timer = "@",
    .icon_lock = "#",
    .icon_money = "$",
    .icon_globe = "@",
    .icon_rocket = "^",
    .icon_fire = "~",
    .icon_link = "=",
    .icon_eyes = "o",
    .hline = "-",
    .hline_heavy = "=",
    .vline = "|",
    .corner_tl = "+",
    .corner_tr = "+",
    .corner_bl = "+",
    .corner_br = "+",
    .dot_large = "*",
    .dot_medium = ".",
    .dot_small = ".",
};

tui_glyph_tier_t tui_detect_glyph_tier(void) {
    if ((int)s_glyph_tier != -1)
        return s_glyph_tier;

    /* Allow explicit override via DSCO_GLYPH env var */
    const char *override = getenv("DSCO_GLYPH");
    if (override) {
        if (strcmp(override, "ascii") == 0) {
            s_glyph_tier = TUI_GLYPH_ASCII;
            return s_glyph_tier;
        }
        if (strcmp(override, "unicode") == 0) {
            s_glyph_tier = TUI_GLYPH_UNICODE;
            return s_glyph_tier;
        }
        if (strcmp(override, "full") == 0) {
            s_glyph_tier = TUI_GLYPH_FULL;
            return s_glyph_tier;
        }
        if (strcmp(override, "nerd") == 0) {
            s_glyph_tier = TUI_GLYPH_NERD;
            return s_glyph_tier;
        }
    }

    /* Not a TTY → ASCII only */
    if (!isatty(STDERR_FILENO)) {
        s_glyph_tier = TUI_GLYPH_ASCII;
        return s_glyph_tier;
    }

    /* Check locale for UTF-8 support */
    const char *lang = getenv("LANG");
    const char *lc_all = getenv("LC_ALL");
    const char *lc_ctype = getenv("LC_CTYPE");
    bool has_utf8 = false;

    const char *locale_vars[] = {lc_all, lc_ctype, lang};
    for (int li = 0; li < 3 && !has_utf8; li++) {
        if (locale_vars[li] &&
            (strstr(locale_vars[li], "UTF-8") || strstr(locale_vars[li], "utf-8") ||
             strstr(locale_vars[li], "utf8") || strstr(locale_vars[li], "UTF8"))) {
            has_utf8 = true;
        }
    }

    if (!has_utf8) {
        s_glyph_tier = TUI_GLYPH_ASCII;
        return s_glyph_tier;
    }

    /* Check for terminals known to support emoji / full Unicode */
    const char *term_prog = getenv("TERM_PROGRAM");
    const char *term_prog_v = getenv("TERM_PROGRAM_VERSION");
    (void)term_prog_v;

    /* Detect Nerd Font: DSCO_NERD_FONT=1 or NERD_FONT=1 env var */
    const char *nerd_flag = getenv("DSCO_NERD_FONT");
    if (!nerd_flag)
        nerd_flag = getenv("NERD_FONT");
    bool has_nerd = (nerd_flag && (strcmp(nerd_flag, "1") == 0 || strcmp(nerd_flag, "true") == 0 ||
                                   strcmp(nerd_flag, "yes") == 0));

    /* These modern terminals render emoji and full Unicode well */
    if (term_prog && (strcmp(term_prog, "iTerm.app") == 0 || strcmp(term_prog, "WezTerm") == 0 ||
                      strcmp(term_prog, "Hyper") == 0 || strcmp(term_prog, "vscode") == 0)) {
        s_glyph_tier = has_nerd ? TUI_GLYPH_NERD : TUI_GLYPH_FULL;
        return s_glyph_tier;
    }

    /* kitty always supports full Unicode, often has Nerd Fonts */
    const char *term = getenv("TERM");
    if (term && strstr(term, "kitty")) {
        s_glyph_tier = has_nerd ? TUI_GLYPH_NERD : TUI_GLYPH_FULL;
        return s_glyph_tier;
    }

    /* Alacritty: good Unicode but emoji rendering varies */
    if (term && strstr(term, "alacritty")) {
        s_glyph_tier = TUI_GLYPH_UNICODE;
        return s_glyph_tier;
    }

    /* macOS Terminal.app: decent Unicode, patchy emoji */
    if (term_prog && strcmp(term_prog, "Apple_Terminal") == 0) {
        s_glyph_tier = TUI_GLYPH_UNICODE;
        return s_glyph_tier;
    }

    /* Linux console: very limited */
    if (term && strcmp(term, "linux") == 0) {
        s_glyph_tier = TUI_GLYPH_ASCII;
        return s_glyph_tier;
    }

    /* Default: if we have UTF-8 locale, assume Unicode BMP is safe */
    s_glyph_tier = TUI_GLYPH_UNICODE;
    return s_glyph_tier;
}

const tui_glyphs_t *tui_glyph(void) {
    tui_glyph_tier_t tier = tui_detect_glyph_tier();
    switch (tier) {
        case TUI_GLYPH_NERD:
            return &GLYPHS_NERD;
        case TUI_GLYPH_FULL:
            return &GLYPHS_FULL;
        case TUI_GLYPH_UNICODE:
            return &GLYPHS_UNICODE;
        case TUI_GLYPH_ASCII:
        default:
            return &GLYPHS_ASCII;
    }
}

void tui_set_glyph_tier(tui_glyph_tier_t tier) {
    s_glyph_tier = tier;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TRUE COLOR FOUNDATION
 * ══════════════════════════════════════════════════════════════════════════ */

static tui_color_level_t s_color_level = (tui_color_level_t)-1; /* uncached sentinel */

tui_color_level_t tui_detect_color_level(void) {
    if ((int)s_color_level != -1)
        return s_color_level;

    if (!isatty(STDERR_FILENO)) {
        s_color_level = TUI_COLOR_NONE;
        return s_color_level;
    }

    const char *colorterm = getenv("COLORTERM");
    if (colorterm && (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0)) {
        s_color_level = TUI_COLOR_TRUECOLOR;
        return s_color_level;
    }

    /* iTerm2, WezTerm, kitty, Alacritty all set COLORTERM but also check TERM_PROGRAM */
    const char *term_prog = getenv("TERM_PROGRAM");
    if (term_prog && (strcmp(term_prog, "iTerm.app") == 0 || strcmp(term_prog, "WezTerm") == 0 ||
                      strcmp(term_prog, "Hyper") == 0)) {
        s_color_level = TUI_COLOR_TRUECOLOR;
        return s_color_level;
    }

    const char *term = getenv("TERM");
    if (term && strstr(term, "256color")) {
        s_color_level = TUI_COLOR_256;
        return s_color_level;
    }

    s_color_level = TUI_COLOR_16;
    return s_color_level;
}

bool tui_supports_truecolor(void) {
    return tui_detect_color_level() == TUI_COLOR_TRUECOLOR;
}

uint64_t tui_color_name_hash(const char *name) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)(name && name[0] ? name : "dsco.color.default");
    while (*p) {
        unsigned char ch = *p++;
        if (isspace(ch))
            ch = '-';
        else
            ch = (unsigned char)tolower(ch);
        h ^= (uint64_t)ch;
        h *= 1099511628211ULL;
    }
    return h ? h : 1469598103934665603ULL;
}

tui_rgb_t tui_hsv_to_rgb(float h, float s, float v) {
    float c = v * s;
    float hp = fmodf(h / 60.0f, 6.0f);
    if (hp < 0)
        hp += 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;

    if (hp < 1) {
        r1 = c;
        g1 = x;
        b1 = 0;
    } else if (hp < 2) {
        r1 = x;
        g1 = c;
        b1 = 0;
    } else if (hp < 3) {
        r1 = 0;
        g1 = c;
        b1 = x;
    } else if (hp < 4) {
        r1 = 0;
        g1 = x;
        b1 = c;
    } else if (hp < 5) {
        r1 = x;
        g1 = 0;
        b1 = c;
    } else {
        r1 = c;
        g1 = 0;
        b1 = x;
    }

    tui_rgb_t rgb;
    rgb.r = (unsigned char)((r1 + m) * 255.0f + 0.5f);
    rgb.g = (unsigned char)((g1 + m) * 255.0f + 0.5f);
    rgb.b = (unsigned char)((b1 + m) * 255.0f + 0.5f);
    return rgb;
}

void tui_fg_rgb(tui_rgb_t c) {
    fprintf(stderr, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b);
}

/* Approximate RGB to nearest 256-color index */
int tui_rgb_to_256(tui_rgb_t c) {
    /* Check greyscale ramp first (232-255) */
    if (c.r == c.g && c.g == c.b) {
        if (c.r < 8)
            return 16;
        if (c.r > 248)
            return 231;
        return (int)roundf((c.r - 8.0f) / 247.0f * 24.0f) + 232;
    }
    /* 6x6x6 color cube (16-231) */
    int ri = (int)roundf(c.r / 255.0f * 5.0f);
    int gi = (int)roundf(c.g / 255.0f * 5.0f);
    int bi = (int)roundf(c.b / 255.0f * 5.0f);
    return 16 + 36 * ri + 6 * gi + bi;
}

static void fg_color_auto(tui_rgb_t c) {
    if (tui_supports_truecolor()) {
        fprintf(stderr, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b);
    } else if (tui_detect_color_level() >= TUI_COLOR_256) {
        fprintf(stderr, "\033[38;5;%dm", tui_rgb_to_256(c));
    }
    /* 16-color: caller should use ANSI constants instead */
}

static float hash_unit(uint64_t h, int shift) {
    return (float)((h >> shift) & 0xFFFFu) / 65535.0f;
}

static tui_rgb_t rgb_blend(tui_rgb_t a, tui_rgb_t b, float t) {
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    tui_rgb_t out;
    out.r = (unsigned char)(a.r + (b.r - a.r) * t + 0.5f);
    out.g = (unsigned char)(a.g + (b.g - a.g) * t + 0.5f);
    out.b = (unsigned char)(a.b + (b.b - a.b) * t + 0.5f);
    return out;
}

static bool str_contains_ci_local(const char *s, const char *needle) {
    if (!s || !needle || !needle[0])
        return false;
    size_t n = strlen(needle);
    for (const char *p = s; *p; p++) {
        if (strncasecmp(p, needle, n) == 0)
            return true;
    }
    return false;
}

static void rgb_hex(tui_rgb_t rgb, char out[8]) {
    snprintf(out, 8, "#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
}

tui_rgb_t tui_named_color_rgb(const char *name) {
    uint64_t h = tui_color_name_hash(name);
    float hue = hash_unit(h, 0) * 360.0f;
    float sat = 0.52f + hash_unit(h, 16) * 0.38f;
    float val = 0.68f + hash_unit(h, 32) * 0.28f;

    if (str_contains_ci_local(name, "muted") || str_contains_ci_local(name, "comment")) {
        sat *= 0.42f;
        val *= 0.74f;
    } else if (str_contains_ci_local(name, "danger") || str_contains_ci_local(name, "error") ||
               str_contains_ci_local(name, "blocked")) {
        hue = 358.0f + hash_unit(h, 48) * 16.0f;
        sat = 0.72f + hash_unit(h, 8) * 0.20f;
        val = 0.82f + hash_unit(h, 24) * 0.14f;
    } else if (str_contains_ci_local(name, "success") || str_contains_ci_local(name, "done") ||
               str_contains_ci_local(name, "complete")) {
        hue = 118.0f + hash_unit(h, 48) * 42.0f;
        sat = 0.58f + hash_unit(h, 8) * 0.24f;
        val = 0.74f + hash_unit(h, 24) * 0.20f;
    } else if (str_contains_ci_local(name, "warn") || str_contains_ci_local(name, "pending")) {
        hue = 35.0f + hash_unit(h, 48) * 30.0f;
        sat = 0.66f + hash_unit(h, 8) * 0.22f;
        val = 0.80f + hash_unit(h, 24) * 0.16f;
    }

    return tui_hsv_to_rgb(hue, sat, val);
}

void tui_named_color_sample(const char *name, tui_named_color_t *out) {
    if (!out)
        return;
    const char *key = (name && name[0]) ? name : "dsco.color.default";
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", key);
    out->hash = tui_color_name_hash(key);
    out->rgb = tui_named_color_rgb(key);
    out->ansi256 = tui_rgb_to_256(out->rgb);
    rgb_hex(out->rgb, out->hex);
}

void tui_construct_color_sample(const char *kind, const char *name, const char *state,
                                double weight, tui_named_color_t *out) {
    char key[512];
    const char *k = (kind && kind[0]) ? kind : "item";
    const char *n = (name && name[0]) ? name : "unnamed";
    const char *s = (state && state[0]) ? state : "neutral";
    snprintf(key, sizeof(key), "construct.%s.%s.%s", k, n, s);
    tui_named_color_sample(key, out);
    if (!out)
        return;

    tui_rgb_t status = out->rgb;
    float blend = 0.0f;
    if (str_contains_ci_local(s, "error") || str_contains_ci_local(s, "blocked") ||
        str_contains_ci_local(s, "fail") || str_contains_ci_local(s, "danger")) {
        status = tui_hsv_to_rgb(2.0f + hash_unit(out->hash, 8) * 18.0f, 0.86f, 0.94f);
        blend = 0.55f;
    } else if (str_contains_ci_local(s, "done") || str_contains_ci_local(s, "complete") ||
               str_contains_ci_local(s, "available") || str_contains_ci_local(s, "current") ||
               str_contains_ci_local(s, "calibrated") || str_contains_ci_local(s, "ok")) {
        status = tui_hsv_to_rgb(126.0f + hash_unit(out->hash, 8) * 34.0f, 0.70f, 0.88f);
        blend = 0.45f;
    } else if (str_contains_ci_local(s, "active") || str_contains_ci_local(s, "running") ||
               str_contains_ci_local(s, "continue")) {
        status = tui_hsv_to_rgb(190.0f + hash_unit(out->hash, 8) * 45.0f, 0.72f, 0.92f);
        blend = 0.42f;
    } else if (str_contains_ci_local(s, "warn") || str_contains_ci_local(s, "uncertain") ||
               str_contains_ci_local(s, "pending")) {
        status = tui_hsv_to_rgb(38.0f + hash_unit(out->hash, 8) * 24.0f, 0.78f, 0.92f);
        blend = 0.42f;
    }
    if (blend > 0.0f)
        out->rgb = rgb_blend(out->rgb, status, blend);

    if (isfinite(weight) && fabs(weight) > 0.0001) {
        float w = (float)fabs(weight);
        if (w > 1.0f)
            w = 1.0f;
        tui_rgb_t white = {255, 255, 255};
        tui_rgb_t black = {0, 0, 0};
        out->rgb = weight >= 0 ? rgb_blend(out->rgb, white, 0.06f * w)
                               : rgb_blend(out->rgb, black, 0.18f * w);
    }
    out->ansi256 = tui_rgb_to_256(out->rgb);
    rgb_hex(out->rgb, out->hex);
}

static int rgb_to_ansi16_code(tui_rgb_t c, bool bg) {
    static const struct {
        unsigned char r, g, b;
        int fg;
        int bg;
    } pal[] = {{0, 0, 0, 30, 40},       {128, 0, 0, 31, 41},     {0, 128, 0, 32, 42},
               {128, 128, 0, 33, 43},   {0, 0, 128, 34, 44},     {128, 0, 128, 35, 45},
               {0, 128, 128, 36, 46},   {192, 192, 192, 37, 47}, {128, 128, 128, 90, 100},
               {255, 0, 0, 91, 101},    {0, 255, 0, 92, 102},    {255, 255, 0, 93, 103},
               {0, 0, 255, 94, 104},    {255, 0, 255, 95, 105},  {0, 255, 255, 96, 106},
               {255, 255, 255, 97, 107}};
    long best = LONG_MAX;
    int best_code = bg ? 40 : 37;
    for (size_t i = 0; i < sizeof(pal) / sizeof(pal[0]); i++) {
        long dr = (long)c.r - pal[i].r;
        long dg = (long)c.g - pal[i].g;
        long db = (long)c.b - pal[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (dist < best) {
            best = dist;
            best_code = bg ? pal[i].bg : pal[i].fg;
        }
    }
    return best_code;
}

static const char *rgb_escape(tui_rgb_t c, bool bg) {
    static _Thread_local char ring[32][40];
    static _Thread_local unsigned idx;
    tui_color_level_t level = tui_detect_color_level();
    if (level == TUI_COLOR_NONE)
        return "";
    char *buf = ring[idx++ % 32];
    if (level == TUI_COLOR_TRUECOLOR) {
        snprintf(buf, 40, "\033[%d;2;%d;%d;%dm", bg ? 48 : 38, c.r, c.g, c.b);
    } else if (level == TUI_COLOR_256) {
        snprintf(buf, 40, "\033[%d;5;%dm", bg ? 48 : 38, tui_rgb_to_256(c));
    } else {
        snprintf(buf, 40, "\033[%dm", rgb_to_ansi16_code(c, bg));
    }
    return buf;
}

const char *tui_rgb_fg_escape(tui_rgb_t c) {
    return rgb_escape(c, false);
}

const char *tui_rgb_bg_escape(tui_rgb_t c) {
    return rgb_escape(c, true);
}

const char *tui_named_fg(const char *name) {
    return tui_rgb_fg_escape(tui_named_color_rgb(name));
}

const char *tui_named_bg(const char *name) {
    return tui_rgb_bg_escape(tui_named_color_rgb(name));
}

void tui_write_fg(FILE *out, tui_rgb_t c) {
    fputs(tui_rgb_fg_escape(c), out ? out : stderr);
}

void tui_write_bg(FILE *out, tui_rgb_t c) {
    fputs(tui_rgb_bg_escape(c), out ? out : stderr);
}

void tui_gradient_text(const char *text, float h_start, float h_end, float s, float v) {
    if (!text || !*text)
        return;

    /* Count visible characters */
    int vis_count = 0;
    for (const char *p = text; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80)
            vis_count++;
    }
    if (vis_count <= 1) {
        tui_rgb_t c = tui_hsv_to_rgb(h_start, s, v);
        fg_color_auto(c);
        fprintf(stderr, "%s" TUI_RESET, text);
        return;
    }

    int idx = 0;
    for (const char *p = text; *p;) {
        /* Determine byte length of this UTF-8 character */
        int clen = 1;
        unsigned char uc = (unsigned char)*p;
        if (uc >= 0xF0)
            clen = 4;
        else if (uc >= 0xE0)
            clen = 3;
        else if (uc >= 0xC0)
            clen = 2;

        float t = (float)idx / (float)(vis_count - 1);
        float h = h_start + t * (h_end - h_start);
        tui_rgb_t c = tui_hsv_to_rgb(h, s, v);
        fg_color_auto(c);
        fwrite(p, 1, clen, stderr);

        p += clen;
        idx++;
    }
    fprintf(stderr, TUI_RESET);
}

void tui_gradient_divider(int width, float h_start, float h_end) {
    if (width <= 0)
        width = tui_term_width();
    for (int i = 0; i < width; i++) {
        float t = (float)i / (float)(width - 1);
        float h = h_start + t * (h_end - h_start);
        tui_rgb_t c = tui_hsv_to_rgb(h, 0.6f, 0.7f);
        fg_color_auto(c);
        fprintf(stderr, "─");
    }
    fprintf(stderr, TUI_RESET "\n");
}

void tui_transition_divider(void) {
    bool use_rgb = tui_detect_color_level() >= TUI_COLOR_256;
    if (use_rgb) {
        /* Subtle center-fading dot trail */
        int w = tui_term_width();
        int dots = w / 3;
        if (dots < 8)
            dots = 8;
        if (dots > 40)
            dots = 40;
        int pad = (w - dots) / 2;
        fprintf(stderr, "%*s", pad, "");
        for (int i = 0; i < dots; i++) {
            float t = (float)i / (float)(dots - 1);
            /* Fade from edges: bright center, dim edges */
            float brightness = 0.25f + 0.35f * (1.0f - fabsf(2.0f * t - 1.0f));
            float hue = 260.0f + t * 40.0f;
            tui_rgb_t c = tui_hsv_to_rgb(hue, 0.2f, brightness);
            fprintf(stderr, "\033[38;2;%d;%d;%dm·", c.r, c.g, c.b);
        }
        fprintf(stderr, TUI_RESET "\n");
    } else {
        fprintf(stderr, "\n");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * TOOL TYPE CLASSIFICATION & COLORS
 * ══════════════════════════════════════════════════════════════════════════ */

tui_tool_type_t tui_classify_tool(const char *name) {
    if (!name)
        return TUI_TOOL_OTHER;
    if (strcmp(name, "Read") == 0 || strcmp(name, "Glob") == 0 || strcmp(name, "TaskList") == 0)
        return TUI_TOOL_READ;
    if (strcmp(name, "Grep") == 0)
        return TUI_TOOL_DATA;
    if (strcmp(name, "Write") == 0 || strcmp(name, "Edit") == 0 || strcmp(name, "TodoWrite") == 0 ||
        strcmp(name, "EnterPlanMode") == 0 || strcmp(name, "ExitPlanMode") == 0)
        return TUI_TOOL_WRITE;
    if (strcmp(name, "Bash") == 0 || strcmp(name, "Agent") == 0 || strcmp(name, "Task") == 0)
        return TUI_TOOL_EXEC;
    if (strcmp(name, "WebFetch") == 0 || strcmp(name, "WebSearch") == 0)
        return TUI_TOOL_WEB;
    if (strcmp(name, "AskUserQuestion") == 0)
        return TUI_TOOL_OTHER;
    /* Crypto — must match before generic "hash" */
    if (strstr(name, "sha") || strstr(name, "md5") || strstr(name, "hmac") ||
        strstr(name, "encrypt") || strstr(name, "decrypt") || strstr(name, "hash") ||
        strstr(name, "base64") || strstr(name, "uuid") || strstr(name, "jwt") ||
        strstr(name, "hkdf") || strstr(name, "random_bytes") || strstr(name, "crypto"))
        return TUI_TOOL_CRYPTO;
    /* Math/eval */
    if (strstr(name, "eval") || strstr(name, "calc") || strstr(name, "math") ||
        strstr(name, "factorial") || strstr(name, "compute"))
        return TUI_TOOL_MATH;
    /* Data/search/query */
    if (strstr(name, "search") || strstr(name, "find") || strstr(name, "query") ||
        strstr(name, "sql") || strstr(name, "database") || strstr(name, "market") ||
        strstr(name, "quote") || strstr(name, "context_search") || strstr(name, "context_get") ||
        strstr(name, "grep"))
        return TUI_TOOL_DATA;
    /* Read */
    if (strstr(name, "read") || strstr(name, "list") || strstr(name, "get") ||
        strstr(name, "cat") || strstr(name, "tree") || strstr(name, "ls") || strstr(name, "head") ||
        strstr(name, "tail") || strstr(name, "stat") || strstr(name, "sysinfo") ||
        strstr(name, "date"))
        return TUI_TOOL_READ;
    /* Write */
    if (strstr(name, "write") || strstr(name, "create") || strstr(name, "edit") ||
        strstr(name, "patch") || strstr(name, "update") || strstr(name, "delete") ||
        strstr(name, "mkdir") || strstr(name, "mv") || strstr(name, "rm") || strstr(name, "save") ||
        strstr(name, "append"))
        return TUI_TOOL_WRITE;
    /* Exec */
    if (strstr(name, "exec") || strstr(name, "run") || strstr(name, "shell") ||
        strstr(name, "bash") || strstr(name, "cmd") || strstr(name, "python") ||
        strstr(name, "compile") || strstr(name, "build") || strstr(name, "pipeline"))
        return TUI_TOOL_EXEC;
    /* Web */
    if (strstr(name, "http") || strstr(name, "fetch") || strstr(name, "curl") ||
        strstr(name, "web") || strstr(name, "api") || strstr(name, "request") ||
        strstr(name, "download") || strstr(name, "url") || strstr(name, "browse"))
        return TUI_TOOL_WEB;
    return TUI_TOOL_OTHER;
}

const char *tui_tool_color(tui_tool_type_t type) {
    switch (type) {
        case TUI_TOOL_READ:
            return tui_named_fg("tool.read");
        case TUI_TOOL_WRITE:
            return tui_named_fg("tool.write");
        case TUI_TOOL_EXEC:
            return tui_named_fg("tool.exec");
        case TUI_TOOL_WEB:
            return tui_named_fg("tool.web");
        case TUI_TOOL_CRYPTO:
            return tui_named_fg("tool.crypto");
        case TUI_TOOL_MATH:
            return tui_named_fg("tool.math");
        case TUI_TOOL_DATA:
            return tui_named_fg("tool.data");
        case TUI_TOOL_OTHER:
            return tui_named_fg("tool.other");
    }
    return tui_named_fg("tool.other");
}

tui_rgb_t tui_tool_rgb(tui_tool_type_t type) {
    switch (type) {
        case TUI_TOOL_READ:
            return (tui_rgb_t){100, 149, 237}; /* cornflower blue */
        case TUI_TOOL_WRITE:
            return (tui_rgb_t){255, 193, 37}; /* gold */
        case TUI_TOOL_EXEC:
            return (tui_rgb_t){178, 102, 255}; /* purple */
        case TUI_TOOL_WEB:
            return (tui_rgb_t){80, 220, 120}; /* green */
        case TUI_TOOL_CRYPTO:
            return (tui_rgb_t){255, 100, 150}; /* hot pink */
        case TUI_TOOL_MATH:
            return (tui_rgb_t){255, 165, 50}; /* orange */
        case TUI_TOOL_DATA:
            return (tui_rgb_t){0, 200, 200}; /* teal */
        case TUI_TOOL_OTHER:
            return (tui_rgb_t){0, 210, 230}; /* cyan */
    }
    return (tui_rgb_t){0, 210, 230};
}

/* ══════════════════════════════════════════════════════════════════════════
 * ASYNC SPINNER (single tool)
 * ══════════════════════════════════════════════════════════════════════════ */

static double tui_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void *async_spinner_thread(void *arg) {
    tui_async_spinner_t *s = (tui_async_spinner_t *)arg;
    int frame = 0;
    bool truecolor = tui_supports_truecolor();

    while (1) {
        pthread_mutex_lock(&s->mutex);
        bool running = s->running;
        const char *label = s->label;
        double elapsed = tui_now_sec() - s->start_time;
        pthread_mutex_unlock(&s->mutex);

        if (!running)
            break;

        char buf[1024];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[2K\r  ");

        /* ── Claude Code-style activity indicator ── */
        /* ⏺ tool_name(args) ... Xs                   */
        static const char *dot_frames[] = {"   ", ".  ", ".. ", "..."};
        const char *dots = dot_frames[frame % 4];

        char es[16];
        if (elapsed < 10.0)
            snprintf(es, sizeof(es), "%.1fs", elapsed);
        else
            snprintf(es, sizeof(es), "%.0fs", elapsed);

        if (truecolor) {
            /* Pulse the orange slightly */
            float pulse = 0.85f + 0.15f * sinf((float)elapsed * 3.0f);
            int or_ = (int)(255 * pulse);
            int og = (int)(95 * pulse);
            /* ⏺ in warm orange */
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[38;2;%d;%d;0m" TUI_RECORD "\033[0m",
                            or_, og);
            /* tool name in white */
            pos += snprintf(buf + pos, sizeof(buf) - pos, " \033[1m%s\033[0m", label ? label : "");
            /* animated dots in dim */
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[2m%s\033[0m", dots);
            /* elapsed in muted */
            pos += snprintf(buf + pos, sizeof(buf) - pos, " \033[38;2;100;100;100m%s\033[0m", es);
        } else {
            /* Fallback: ⏺ tool_name... Xs */
            pos += snprintf(buf + pos, sizeof(buf) - pos, TUI_ORANGE TUI_RECORD TUI_RESET);
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%s%s", TUI_BOLD, label ? label : "",
                            TUI_RESET);
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s%s %s%s%s", TUI_DIM, dots, TUI_RESET,
                            TUI_DIM, es, TUI_RESET);
        }

        fwrite(buf, 1, pos, stderr);
        fflush(stderr);

        frame++;
        usleep(80000); /* 12.5fps for smoother animation */
    }
    return NULL;
}

void tui_async_spinner_start(tui_async_spinner_t *s, const char *label, tui_tool_type_t tool_type) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mutex, NULL);
    s->running = true;
    s->label = label;
    s->start_time = tui_now_sec();

    if (tui_detect_color_level() >= TUI_COLOR_256) {
        s->use_rgb = true;
        s->rgb = tui_tool_rgb(tool_type);
    } else {
        s->use_rgb = false;
        s->color = tui_tool_color(tool_type);
    }

    tui_cursor_hide();
    pthread_create(&s->thread, NULL, async_spinner_thread, s);
}

/* Tools whose result IS visual art — shown in full with their own ANSI color
 * instead of a dimmed one-line preview. */
bool tui_tool_is_display_art(const char *name) {
    return name && strcmp(name, "plot") == 0;
}

/* If `name` is a display-art tool, print `result` in full (color preserved,
 * 2-space indent) to stderr and return true; otherwise return false so the
 * caller falls back to its usual dim preview. Shared by every tool-result
 * display path so art renders identically wherever a tool completes. */
bool tui_print_tool_art(const char *name, const char *result) {
    if (!tui_tool_is_display_art(name) || !result || !*result)
        return false;
    for (const char *p = result; *p;) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(stderr, "  %.*s\n", len, p);
        if (!nl)
            break;
        p = nl + 1;
    }
    fflush(stderr);
    return true;
}

void tui_async_spinner_stop(tui_async_spinner_t *s, bool ok, const char *result_preview,
                            double elapsed_ms, const char *suffix) {
    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_mutex_unlock(&s->mutex);

    pthread_join(s->thread, NULL);
    pthread_mutex_destroy(&s->mutex);

    tui_clear_line();
    tui_cursor_show();

    /* Format elapsed nicely */
    char elapsed_str[32];
    if (elapsed_ms < 1000.0)
        snprintf(elapsed_str, sizeof(elapsed_str), "%.0fms", elapsed_ms);
    else
        snprintf(elapsed_str, sizeof(elapsed_str), "%.1fs", elapsed_ms / 1000.0);

    /* Truncate preview to first line, max ~80 chars */
    char preview[128] = "";
    if (result_preview && *result_preview) {
        const char *nl = strchr(result_preview, '\n');
        int len = nl ? (int)(nl - result_preview) : (int)strlen(result_preview);
        if (len > 80)
            len = 80;
        memcpy(preview, result_preview, len);
        preview[len] = '\0';
    }

    /* Size info for large results */
    char size_str[32] = "";
    if (result_preview) {
        size_t total = strlen(result_preview);
        if (total > 1024) {
            snprintf(size_str, sizeof(size_str), " [%.1fKB]", total / 1024.0);
        }
    }

    bool use_rgb = tui_detect_color_level() >= TUI_COLOR_256;

    /* ── Chat-log style completion line ─────────────────────────────────
     *   ▌ tool_response  name · Xs · NKB  [suffix]
     *     <preview line, dim>
     * ──────────────────────────────────────────────────────────────── */
    int br = ok ? 80 : 255, bg = ok ? 220 : 80, bb = ok ? 120 : 80;
    if (use_rgb) {
        fprintf(stderr, "\n  \033[38;2;%d;%d;%dm▌\033[0m \033[1mtool_response\033[0m", br, bg, bb);
        fprintf(stderr, "\033[2m %s\033[0m", s->label ? s->label : "");
        fprintf(stderr, " \033[2m" TUI_SEP " %s\033[0m", elapsed_str);
        if (size_str[0])
            fprintf(stderr, " \033[2m" TUI_SEP "%s\033[0m", size_str);
        if (!ok)
            fprintf(stderr, " \033[38;2;255;80;80m\xe2\x9c\x97\033[0m");
        if (suffix && suffix[0])
            fprintf(stderr, "  \033[2m%s\033[0m", suffix);
    } else {
        const char *bar_color = ok ? TUI_GREEN : TUI_RED;
        fprintf(stderr, "\n  %s▌%s %stool_response%s", bar_color, TUI_RESET, TUI_BOLD, TUI_RESET);
        fprintf(stderr, " %s%s%s", TUI_DIM, s->label ? s->label : "", TUI_RESET);
        fprintf(stderr, " %s" TUI_SEP " %s%s%s", TUI_DIM, elapsed_str, size_str[0] ? " " : "",
                size_str[0] ? size_str : "");
        if (!ok)
            fprintf(stderr, " \xe2\x9c\x97");
        fprintf(stderr, TUI_RESET);
        if (suffix && suffix[0])
            fprintf(stderr, "  %s%s%s", TUI_DIM, suffix, TUI_RESET);
    }
    fprintf(stderr, "\n");
    /* Display-art tools render their FULL colored output; everything else
     * shows the usual dim first-line preview. */
    if (!(ok && tui_print_tool_art(s->label, result_preview)) && preview[0])
        fprintf(stderr, "  %s%s%s\n", TUI_DIM, preview, TUI_RESET);
    fflush(stderr);
}

/* ══════════════════════════════════════════════════════════════════════════
 * BATCH SPINNER (multi-tool)
 * ══════════════════════════════════════════════════════════════════════════ */

static void *batch_spinner_thread(void *arg) {
    tui_batch_spinner_t *bs = (tui_batch_spinner_t *)arg;
    int frame = 0;

    /* Large buffer for atomic writes — avoids interleaving with stdout */
    char buf[8192];

    while (1) {
        pthread_mutex_lock(&bs->mutex);
        bool running = bs->running;
        int count = bs->count;
        double now = tui_now_sec();

        if (!running) {
            pthread_mutex_unlock(&bs->mutex);
            break;
        }

        int pos = 0;

        /* Move cursor up to first line */
        if (count > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[%dA", count);
        }

        for (int i = 0; i < count; i++) {
            tui_batch_entry_t *e = &bs->entries[i];
            /* Clear line */
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[2K\r");

            if (e->done) {
                char elapsed_str[32];
                if (e->elapsed_ms < 1000.0)
                    snprintf(elapsed_str, sizeof(elapsed_str), "%.0fms", e->elapsed_ms);
                else
                    snprintf(elapsed_str, sizeof(elapsed_str), "%.1fs", e->elapsed_ms / 1000.0);

                /* Speed-based elapsed color */
                float speed_hue =
                    e->elapsed_ms < 500    ? 120.0f
                    : e->elapsed_ms < 2000 ? 120.0f - (float)(e->elapsed_ms - 500) / 1500.0f * 60.0f
                    : e->elapsed_ms < 5000 ? 60.0f - (float)(e->elapsed_ms - 2000) / 3000.0f * 60.0f
                                           : 0.0f;
                tui_rgb_t er = tui_hsv_to_rgb(speed_hue, 0.5f, 0.75f);
                tui_rgb_t ir = e->ok ? (tui_rgb_t){80, 220, 120} : (tui_rgb_t){255, 80, 80};
                tui_rgb_t nr = tui_tool_rgb(e->type);

                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "  \033[38;2;%d;%d;%dm%s\033[0m "
                                "\033[38;2;%d;%d;%dm%s\033[0m "
                                "\033[38;2;%d;%d;%dm(%s)\033[0m",
                                ir.r, ir.g, ir.b, e->ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97", nr.r,
                                nr.g, nr.b, e->name, er.r, er.g, er.b, elapsed_str);
                if (e->preview[0]) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%.60s%s", TUI_DIM, e->preview,
                                    TUI_RESET);
                }
            } else {
                double elapsed = now - bs->start_time;
                tui_rgb_t rgb = tui_tool_rgb(e->type);
                const tui_glyphs_t *gl = tui_glyph();

                /* Hue-rotating orbital spinner per entry */
                float base_h = (float)(i * 45) + 270.0f;
                float spin_h = fmodf(base_h + (float)frame * 9.0f, 360.0f);
                tui_rgb_t sc = tui_hsv_to_rgb(spin_h, 0.7f, 0.95f);
                int oc = gl->spin_orbit_n > 0 ? gl->spin_orbit_n : 1;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  \033[38;2;%d;%d;%dm%s\033[0m",
                                sc.r, sc.g, sc.b, gl->spin_orbit[frame % oc]);

                /* Tool name in type color */
                pos += snprintf(buf + pos, sizeof(buf) - pos, " \033[38;2;%d;%d;%dm%s\033[0m",
                                rgb.r, rgb.g, rgb.b, e->name);

                /* Mini 4-char progress pulse */
                {
                    float progress = fmodf((float)elapsed / 8.0f, 1.0f);
                    int pw = 4;
                    int filled = (int)(progress * pw);
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
                    for (int b = 0; b < pw; b++) {
                        float bh = fmodf(spin_h + (float)b * 20.0f, 360.0f);
                        tui_rgb_t bc = tui_hsv_to_rgb(bh, 0.4f, b < filled ? 0.75f : 0.2f);
                        pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[38;2;%d;%d;%dm%s", bc.r,
                                        bc.g, bc.b, b < filled ? gl->vblock[7] : gl->vblock[1]);
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[0m");
                }

                if (e->args_preview[0]) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%.50s%s", TUI_DIM,
                                    e->args_preview, TUI_RESET);
                }

                /* Breathing elapsed */
                {
                    float breath = 0.45f + 0.3f * sinf((float)elapsed * 2.0f);
                    tui_rgb_t tc = tui_hsv_to_rgb(210.0f, 0.15f, breath);
                    pos +=
                        snprintf(buf + pos, sizeof(buf) - pos, " \033[38;2;%d;%d;%dm(%.1fs)\033[0m",
                                 tc.r, tc.g, tc.b, elapsed);
                }
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");

            if (pos >= (int)sizeof(buf) - 256)
                break; /* safety */
        }

        pthread_mutex_unlock(&bs->mutex);

        /* Single atomic write */
        fwrite(buf, 1, pos, stderr);
        fflush(stderr);
        frame++;
        usleep(100000); /* 10fps */
    }
    return NULL;
}

void tui_batch_spinner_start(tui_batch_spinner_t *bs, const char **names, int count) {
    memset(bs, 0, sizeof(*bs));
    pthread_mutex_init(&bs->mutex, NULL);
    bs->running = true;
    bs->start_time = tui_now_sec();
    bs->count = count < TUI_BATCH_MAX ? count : TUI_BATCH_MAX;

    for (int i = 0; i < bs->count; i++) {
        snprintf(bs->entries[i].name, sizeof(bs->entries[i].name), "%s",
                 names[i] ? names[i] : "unknown");
        bs->entries[i].type = tui_classify_tool(names[i]);
        bs->entries[i].done = false;
    }

    tui_cursor_hide();

    /* Reserve N blank lines */
    for (int i = 0; i < bs->count; i++)
        fprintf(stderr, "\n");

    pthread_create(&bs->thread, NULL, batch_spinner_thread, bs);
}

void tui_batch_spinner_complete(tui_batch_spinner_t *bs, int idx, bool ok, const char *preview,
                                double elapsed_ms) {
    pthread_mutex_lock(&bs->mutex);
    if (idx >= 0 && idx < bs->count) {
        bs->entries[idx].done = true;
        bs->entries[idx].ok = ok;
        bs->entries[idx].elapsed_ms = elapsed_ms;
        if (preview) {
            const char *nl = strchr(preview, '\n');
            int len = nl ? (int)(nl - preview) : (int)strlen(preview);
            if (len > 80)
                len = 80;
            memcpy(bs->entries[idx].preview, preview, len);
            bs->entries[idx].preview[len] = '\0';
        }
    }
    pthread_mutex_unlock(&bs->mutex);
}

void tui_batch_spinner_stop(tui_batch_spinner_t *bs) {
    pthread_mutex_lock(&bs->mutex);
    bs->running = false;
    pthread_mutex_unlock(&bs->mutex);

    pthread_join(bs->thread, NULL);
    pthread_mutex_destroy(&bs->mutex);

    tui_cursor_show();
}

void tui_batch_summary(const tui_batch_spinner_t *bs, const char *cost_suffix) {
    if (bs->count < 2)
        return;

    int ok_count = 0, fail_count = 0, cached = 0;
    double total_ms = 0, max_ms = 0;
    for (int i = 0; i < bs->count; i++) {
        const tui_batch_entry_t *e = &bs->entries[i];
        if (e->ok)
            ok_count++;
        else
            fail_count++;
        if (e->preview[0] && strncmp(e->preview, "cached", 6) == 0) {
            cached++;
        } else {
            total_ms += e->elapsed_ms;
            if (e->elapsed_ms > max_ms)
                max_ms = e->elapsed_ms;
        }
    }
    (void)ok_count;
    int executed = bs->count - cached;
    double avg_ms = executed > 0 ? total_ms / executed : 0;

    const tui_glyphs_t *gl = tui_glyph();

    /* Summary: ✓ 5 tools (42ms avg, 120ms max) [2 cached]  [in:N out:N $cost] */
    fprintf(stderr, "  %s%s%s %s%d tool%s%s", fail_count == 0 ? TUI_GREEN : TUI_YELLOW,
            fail_count == 0 ? gl->ok : gl->warn, TUI_RESET, TUI_BOLD, bs->count,
            bs->count == 1 ? "" : "s", TUI_RESET);

    if (executed > 0) {
        char avg_str[32], max_str[32];
        if (avg_ms < 1000.0)
            snprintf(avg_str, sizeof(avg_str), "%.0fms", avg_ms);
        else
            snprintf(avg_str, sizeof(avg_str), "%.1fs", avg_ms / 1000.0);
        if (max_ms < 1000.0)
            snprintf(max_str, sizeof(max_str), "%.0fms", max_ms);
        else
            snprintf(max_str, sizeof(max_str), "%.1fs", max_ms / 1000.0);
        fprintf(stderr, " %s(%s avg, %s max)%s", TUI_DIM, avg_str, max_str, TUI_RESET);
    }
    if (cached > 0) {
        fprintf(stderr, " %s[%d cached]%s", TUI_DIM, cached, TUI_RESET);
    }
    if (fail_count > 0) {
        fprintf(stderr, " %s[%d failed]%s", TUI_RED, fail_count, TUI_RESET);
    }
    if (cost_suffix && cost_suffix[0]) {
        fprintf(stderr, "  %s%s%s", TUI_DIM, cost_suffix, TUI_RESET);
    }
    fprintf(stderr, "\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * BRAILLE WORDMARK
 *
 * Renders the real "Distributed" brand wordmark — sampled from the source PNG
 * into a 1bpp master bitmap (include/dist_logo.h) — using Unicode Braille
 * glyphs. Each cell packs a 2×4 grid of dots, so one character cell carries
 * EIGHT sub-pixels: roughly 4× the density of half-blocks. The master is
 * box-downsampled to whatever width the terminal allows, then coloured with a
 * chrome/iridescent hue gradient and a sheen highlight that sweeps across on
 * reveal. Below it we print a letter-spaced "S Y S T E M S" subtitle.
 * ══════════════════════════════════════════════════════════════════════════ */

#define BRL_MAXW 256 /* max sub-pixel width we render (128 cells) */
#define BRL_MAXH 64  /* max sub-pixel height (16 cells)           */

static __attribute__((unused)) inline int dist_logo_bit(int x, int y) {
    /* dist_logo.h now ships an 8-bit grayscale master (DIST_LOGO_GRAY) instead
     * of a packed 1-bit bitmap; threshold it to preserve the on/off contract. */
    return DIST_LOGO_GRAY[y * DIST_LOGO_W + x] > 127;
}

static tui_rgb_t brl_lerp_white(tui_rgb_t c, float f) {
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    tui_rgb_t o;
    o.r = (unsigned char)(c.r + (255 - c.r) * f);
    o.g = (unsigned char)(c.g + (255 - c.g) * f);
    o.b = (unsigned char)(c.b + (255 - c.b) * f);
    return o;
}

static void brl_set_fg(tui_rgb_t c) {
    if (tui_supports_truecolor())
        fprintf(stderr, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b);
    else
        fprintf(stderr, "\033[38;5;%dm", tui_rgb_to_256(c));
}

static __attribute__((unused)) tui_rgb_t brl_scale_rgb(tui_rgb_t c, float scale) {
    if (scale < 0.0f)
        scale = 0.0f;
    if (scale > 1.0f)
        scale = 1.0f;
    tui_rgb_t o;
    o.r = (unsigned char)lroundf((float)c.r * scale);
    o.g = (unsigned char)lroundf((float)c.g * scale);
    o.b = (unsigned char)lroundf((float)c.b * scale);
    return o;
}

/* Splash palettes — full-bright letter colour for horizontal position nx (0..1)
 * and vertical ny (0..1). Selected via DSCO_SPLASH={iris,chrome,neon,gold,fire,
 * rainbow,ice}. `iris` (vivid violet→magenta→peach) is the default. */
enum {
    PAL_IRIS,
    PAL_CHROME,
    PAL_NEON,
    PAL_GOLD,
    PAL_FIRE,
    PAL_RAINBOW,
    PAL_ICE,
    PAL_INDUSTRIAL,
    PAL_STEEL,
    PAL_MONO
};

static int splash_palette_id(void) {
    const char *e = getenv("DSCO_SPLASH");
    if (!e || !*e)
        return PAL_INDUSTRIAL;
    if (!strcasecmp(e, "iris"))
        return PAL_IRIS;
    if (!strcasecmp(e, "chrome"))
        return PAL_CHROME;
    if (!strcasecmp(e, "neon"))
        return PAL_NEON;
    if (!strcasecmp(e, "gold"))
        return PAL_GOLD;
    if (!strcasecmp(e, "fire"))
        return PAL_FIRE;
    if (!strcasecmp(e, "rainbow"))
        return PAL_RAINBOW;
    if (!strcasecmp(e, "ice"))
        return PAL_ICE;
    if (!strcasecmp(e, "industrial"))
        return PAL_INDUSTRIAL;
    if (!strcasecmp(e, "steel"))
        return PAL_STEEL;
    if (!strcasecmp(e, "mono"))
        return PAL_MONO;
    return PAL_INDUSTRIAL;
}

static tui_rgb_t splash_palette(int pal, float nx, float ny) {
    float hue, sat = 0.85f, val = 1.0f;
    switch (pal) {
        case PAL_CHROME:
            hue = 250.0f + nx * 120.0f;
            sat = 0.18f;
            val = 0.82f + 0.18f * sinf(ny * 3.14159f);
            break;
        case PAL_NEON:
            hue = 172.0f + nx * 80.0f;
            sat = 0.88f;
            break;
        case PAL_GOLD:
            hue = 34.0f + nx * 24.0f;
            sat = 0.85f;
            break;
        case PAL_FIRE:
            hue = 2.0f + nx * 48.0f - ny * 12.0f;
            sat = 0.92f;
            break;
        case PAL_RAINBOW:
            hue = nx * 330.0f;
            sat = 0.85f;
            break;
        case PAL_ICE:
            hue = 190.0f + nx * 70.0f;
            sat = 0.55f;
            break;
        /* Brushed white→grey industrial: cool steel tint, brushed vertical
         * metallic sheen (bright band across the middle), brightening L→R. */
        case PAL_INDUSTRIAL:
            hue = 212.0f;
            sat = 0.07f;
            val = 0.50f + 0.30f * sinf(ny * 3.14159f) + 0.20f * nx;
            break;
        /* Cooler, more saturated steel-blue metal. */
        case PAL_STEEL:
            hue = 205.0f + nx * 24.0f;
            sat = 0.24f;
            val = 0.55f + 0.30f * sinf(ny * 3.14159f) + 0.15f * nx;
            break;
        /* Pure greyscale gradient, grey → white. */
        case PAL_MONO:
            hue = 0.0f;
            sat = 0.0f;
            val = 0.45f + 0.55f * nx;
            break;
        default: /*IRIS*/
            hue = 256.0f + nx * 98.0f;
            sat = 0.84f;
            break;
    }
    if (val > 1.0f)
        val = 1.0f;
    return tui_hsv_to_rgb(hue, sat, val);
}

static void welcome_pixel_logo(void) {
    int w = tui_term_width();
    int pal = splash_palette_id();

    /* Choose a sub-pixel target width that fits the terminal (cells = TW/2). */
    int avail = (w - 4) * 2; /* leave a small margin   */
    int TW = avail;
    if (TW > BRL_MAXW)
        TW = BRL_MAXW;
    if (TW > DIST_LOGO_W)
        TW = DIST_LOGO_W;
    if (TW < 80)
        TW = 80; /* keep it legible        */
    TW &= ~1;    /* even for 2-wide cells  */
    int TH = (int)lroundf((float)DIST_LOGO_H * (float)TW / (float)DIST_LOGO_W);
    TH = (TH + 3) & ~3; /* multiple of 4          */
    if (TH > BRL_MAXH)
        TH = BRL_MAXH;

    /* Box-downsample the master into a per-sub-pixel coverage grid (0..255). */
    static unsigned char cov[BRL_MAXH][BRL_MAXW];
    for (int y = 0; y < TH; y++) {
        int sy0 = y * DIST_LOGO_H / TH;
        int sy1 = (y + 1) * DIST_LOGO_H / TH;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < TW; x++) {
            int sx0 = x * DIST_LOGO_W / TW;
            int sx1 = (x + 1) * DIST_LOGO_W / TW;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            int sum = 0, tot = 0;
            for (int yy = sy0; yy < sy1 && yy < DIST_LOGO_H; yy++)
                for (int xx = sx0; xx < sx1 && xx < DIST_LOGO_W; xx++) {
                    sum += DIST_LOGO_GRAY[yy * DIST_LOGO_W + xx];
                    tot++;
                }
            cov[y][x] = tot ? (unsigned char)(sum / tot) : 0;
        }
    }

    int rows = TH / 4, cols = TW / 2;
    int pad = (w - cols) / 2;
    if (pad < 1)
        pad = 1;

    /* Dot bit layout: [row][col] within the 2×4 cell. */
    static const int dotbit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    const int DOT_THR = 112; /* coverage → dot lit         */

    const int NF = 22;                       /* reveal frames              */
    const float sig = (float)TW * 0.11f + 5; /* sheen half-width (subpx)   */

    for (int f = 0; f < NF; f++) {
        float prog = (float)f / (float)(NF - 1);
        /* Sheen sweeps left→right and exits, leaving the final frame settled. */
        float sx = -2.0f * sig + prog * ((float)TW + 4.0f * sig);

        if (f > 0)
            fprintf(stderr, "\033[%dA", rows);

        for (int cy = 0; cy < rows; cy++) {
            fputc('\r', stderr);
            for (int p = 0; p < pad; p++)
                fputc(' ', stderr);

            tui_rgb_t cur = {0, 0, 0};
            int have = 0;
            for (int cx = 0; cx < cols; cx++) {
                int gx = cx * 2, gy = cy * 4;
                int dots = 0, csum = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 2; i++) {
                        int c = cov[gy + j][gx + i];
                        csum += c;
                        if (c >= DOT_THR)
                            dots |= dotbit[j][i];
                    }

                if (!dots) { /* transparent cell */
                    if (have) {
                        fprintf(stderr, "\033[0m");
                        have = 0;
                    }
                    fputc(' ', stderr);
                    continue;
                }

                float nx = (float)gx / (float)(TW - 1);
                float ny = (float)gy / (float)(TH - 1);
                float covavg = (float)csum / (8.0f * 255.0f); /* 0..1, AA */
                float d = (float)gx - sx;
                float hl = expf(-(d * d) / (2.0f * sig * sig)); /* specular */

                tui_rgb_t base = splash_palette(pal, nx, ny);
                base = brl_lerp_white(base, hl * 0.6f); /* shimmer, keep hue */
                float bright = 0.42f + 0.58f * covavg;  /* edge AA */
                bright += hl * 0.18f;                   /* sheen lift */
                if (bright > 1.0f)
                    bright = 1.0f;
                base.r = (unsigned char)(base.r * bright);
                base.g = (unsigned char)(base.g * bright);
                base.b = (unsigned char)(base.b * bright);

                if (!have || base.r != cur.r || base.g != cur.g || base.b != cur.b) {
                    brl_set_fg(base);
                    cur = base;
                    have = 1;
                }
                /* Braille U+2800+dots, emitted as raw UTF-8 (locale-independent). */
                unsigned int cp = 0x2800u + (unsigned)dots;
                fputc(0xE0 | (cp >> 12), stderr);
                fputc(0x80 | ((cp >> 6) & 0x3F), stderr);
                fputc(0x80 | (cp & 0x3F), stderr);
            }
            fprintf(stderr, "\033[0m\033[K\n");
        }
        fflush(stderr);
        if (f < NF - 1)
            usleep(18000);
    }

    /* Letter-spaced subtitle in the same palette, crisp text. */
    {
        const char *sub = "S Y S T E M S";
        int slen = (int)strlen(sub);
        int spad = (w - slen) / 2;
        if (spad < 1)
            spad = 1;
        for (int i = 0; i < spad; i++)
            fputc(' ', stderr);
        tui_rgb_t a = splash_palette(pal, 0.0f, 0.5f);
        tui_rgb_t b = splash_palette(pal, 1.0f, 0.5f);
        int n = 0;
        for (const char *p = sub; *p; p++)
            if (((unsigned char)*p & 0xC0) != 0x80)
                n++;
        int idx = 0;
        for (const char *p = sub; *p;) {
            int clen = 1;
            unsigned char uc = (unsigned char)*p;
            if (uc >= 0xF0)
                clen = 4;
            else if (uc >= 0xE0)
                clen = 3;
            else if (uc >= 0xC0)
                clen = 2;
            float t = n > 1 ? (float)idx / (float)(n - 1) : 0;
            tui_rgb_t c = {(unsigned char)(a.r + (b.r - a.r) * t),
                           (unsigned char)(a.g + (b.g - a.g) * t),
                           (unsigned char)(a.b + (b.b - a.b) * t)};
            brl_set_fg(c);
            fwrite(p, 1, clen, stderr);
            p += clen;
            idx++;
        }
        fprintf(stderr, "\033[0m\n");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * ANIMATED WELCOME BANNER
 * ══════════════════════════════════════════════════════════════════════════ */

/* Overrides the original tui_welcome with animated version */
/* Note: we redefine tui_welcome at the bottom so the linker picks this up.
   Actually, we just modify the existing function above. We'll do it via
   a new internal helper called from the original. */

static void tui_logo_compact_mark(FILE *out, int subdued) {
    static const char *dense[] = {"⣀⣤⣶⣶⣶⣤⣀", "⣿⡇⢀⣀⢸⣿⣿", "⣿⣷⣾⣿⣾⣿⣿"};
    static const char *soft[] = {"⣀⣤⣶⣶⣤⣀", "⣿⡇⢀⡀⢸⣿", "⣿⣷⣾⣿⣾⣿"};
    const char **rows = subdued ? soft : dense;
    const char *fg = subdued ? "\033[38;5;245m" : "\033[38;5;252m";
    fputs(fg, out);
    fputs(rows[0], out);
    fputs(" ", out);
    fputs(TUI_DIM "dsco" TUI_RESET, out);
}

static void welcome_animated(const char *model, int core_count, int total_count,
                             const char *version) {
    int w = tui_term_width();

/* Left margin for banner content (left-aligned) */
#define CENTER_PAD(content_w) 2

    fprintf(stderr, "\n");

    /* Full-width soft gradient top border */
    for (int i = 0; i < w; i++) {
        float t = (float)i / (float)(w > 1 ? w - 1 : 1);
        float h = 280.0f + t * 80.0f; /* purple → pink → peach */
        tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.65f);
        fg_color_auto(c);
        fprintf(stderr, "━");
    }
    fprintf(stderr, TUI_RESET "\n\n");

    /* Hyper-dense sub-cell pixel reveal of the DSCO logo. */
    welcome_pixel_logo();

    fprintf(stderr, "\n");

    /* Info line with sparkle decorations */
    char info_plain[256];
    snprintf(info_plain, sizeof(info_plain),
             "v%s  %s  %d tools (%d loadable)  streaming  swarm-ready", version, model, core_count,
             total_count - core_count);
    int info_pad = CENTER_PAD(0);
    for (int i = 0; i < info_pad; i++)
        fputc(' ', stderr);
    tui_gradient_text(tui_glyph()->sparkle, 300.0f, 300.0f, 0.5f, 1.0f);
    fprintf(stderr, "  ");
    tui_gradient_text(info_plain, 270.0f, 350.0f, 0.35f, 0.9f);
    fprintf(stderr, "  ");
    tui_gradient_text(tui_glyph()->sparkle, 340.0f, 340.0f, 0.5f, 1.0f);
    fprintf(stderr, "\n\n");

    /* Capabilities in a cute centered pill layout with dot separators */
    const char *caps[] = {"AST introspection", "Sub-agent swarms",    "Streaming I/O",
                          "Crypto toolkit",    "Coroutine pipelines", "Plugin system"};
    float cap_hues[] = {270.0f, 290.0f, 310.0f, 330.0f, 350.0f, 280.0f};
    int ncaps = 6;

    /* Row 1: first 3 caps */
    {
        int rpad = CENTER_PAD(0);
        for (int i = 0; i < rpad; i++)
            fputc(' ', stderr);
        for (int i = 0; i < 3; i++) {
            if (i > 0) {
                fprintf(stderr, "  ");
                tui_gradient_text("·", cap_hues[i], cap_hues[i], 0.4f, 0.7f);
                fprintf(stderr, "  ");
            }
            tui_gradient_text(tui_glyph()->diamond, cap_hues[i], cap_hues[i], 0.5f, 1.0f);
            fprintf(stderr, " %s%s%s", TUI_DIM, caps[i], TUI_RESET);
        }
        fprintf(stderr, "\n");
    }

    /* Row 2: last 3 caps */
    {
        int rpad = CENTER_PAD(0);
        for (int i = 0; i < rpad; i++)
            fputc(' ', stderr);
        for (int i = 3; i < ncaps; i++) {
            if (i > 3) {
                fprintf(stderr, "  ");
                tui_gradient_text("·", cap_hues[i], cap_hues[i], 0.4f, 0.7f);
                fprintf(stderr, "  ");
            }
            tui_gradient_text(tui_glyph()->diamond, cap_hues[i], cap_hues[i], 0.5f, 1.0f);
            fprintf(stderr, " %s%s%s", TUI_DIM, caps[i], TUI_RESET);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\n");

    /* Full-width gradient bottom border */
    for (int i = 0; i < w; i++) {
        float t = (float)i / (float)(w > 1 ? w - 1 : 1);
        float h = 280.0f + t * 80.0f;
        tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.65f);
        fg_color_auto(c);
        fprintf(stderr, "━");
    }
    fprintf(stderr, TUI_RESET "\n\n");

#undef CENTER_PAD
}

/* ══════════════════════════════════════════════════════════════════════════
 * LIVE STATUS BAR
 * ══════════════════════════════════════════════════════════════════════════ */

void tui_status_bar_init(tui_status_bar_t *sb, const char *model) {
    memset(sb, 0, sizeof(*sb));
    pthread_mutex_init(&sb->mutex, NULL);
    if (model)
        snprintf(sb->model, sizeof(sb->model), "%s", model);
    sb->enabled = false;
    sb->visible = false;
    sb->panel_active = false;
    sb->panel_rows = TUI_COMPOSER_PANEL_ROWS; /* 3: top rule + input + status */
    sb->splash_started_at = (double)time(NULL);
}

void tui_status_bar_set_model(tui_status_bar_t *sb, const char *model, const char *slot_name) {
    if (!sb)
        return;
    pthread_mutex_lock(&sb->mutex);
    if (model)
        snprintf(sb->model, sizeof(sb->model), "%s", model);
    if (slot_name)
        snprintf(sb->slot_name, sizeof(sb->slot_name), "%s", slot_name);
    pthread_mutex_unlock(&sb->mutex);
    /* No paint here — status bar paints only when the panel is shown. */
}

void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok, double cost, int turn,
                           int tools) {
    pthread_mutex_lock(&sb->mutex);
    sb->input_tokens = in_tok;
    sb->output_tokens = out_tok;
    sb->cost = cost;
    sb->turn = turn;
    sb->tools_used = tools;
    pthread_mutex_unlock(&sb->mutex);
    /* No paint here — status bar paints only when the panel is shown. */
}

void tui_status_bar_enable(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    sb->enabled = true;
    sb->visible = true;
    pthread_mutex_unlock(&sb->mutex);
    /* No DECSTBM, no painting. The panel is ephemeral — it renders when
     * we're about to read input and erases when input is submitted. */
}

void tui_status_bar_disable(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    bool was_visible = sb->visible;
    int panel = sb->panel_rows > 0 ? sb->panel_rows : TUI_COMPOSER_PANEL_ROWS;
    sb->enabled = false;
    sb->visible = false;
    pthread_mutex_unlock(&sb->mutex);

    if (!was_visible)
        return;

    int rows = tui_term_height();
    tui_term_lock();
    tui_save_cursor();
    for (int i = 0; i < panel; i++) {
        tui_cursor_move(rows - i, 1);
        tui_clear_line();
    }
    tui_restore_cursor();
    fflush(stderr);
    tui_term_unlock();
}

/* ── Helpers for status bar context (user, host, branch, cwd) ──────────── */

static const char *sb_username(void) {
    static char buf[64] = {0};
    if (buf[0])
        return buf;
    const char *u = getenv("USER");
    if (!u || !u[0])
        u = getenv("LOGNAME");
    snprintf(buf, sizeof(buf), "%s", (u && u[0]) ? u : "user");
    return buf;
}

static const char *sb_short_cwd(void) {
    static char buf[128];
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        snprintf(buf, sizeof(buf), "?");
        return buf;
    }
    const char *home = getenv("HOME");
    if (home && home[0] && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(buf, sizeof(buf), "~%s", cwd + strlen(home));
    } else {
        snprintf(buf, sizeof(buf), "%s", cwd);
    }
    /* Trim to last 28 chars with ellipsis */
    size_t len = strlen(buf);
    if (len > 28) {
        char tail[32];
        snprintf(tail, sizeof(tail), "…%s", buf + len - 27);
        snprintf(buf, sizeof(buf), "%s", tail);
    }
    return buf;
}

static const char *sb_git_branch(void) {
    static char branch[64] = {0};
    static time_t last_check = 0;
    time_t now = time(NULL);
    /* Refresh every 5s to avoid forking on every status update */
    if (branch[0] && (now - last_check) < 5)
        return branch;
    last_check = now;
    branch[0] = '\0';
    FILE *fp = popen("git symbolic-ref --short -q HEAD 2>/dev/null", "r");
    if (!fp)
        return branch;
    if (fgets(branch, sizeof(branch), fp)) {
        size_t n = strlen(branch);
        while (n > 0 && (branch[n - 1] == '\n' || branch[n - 1] == '\r'))
            branch[--n] = '\0';
        if (n > 18) {
            branch[15] = '\0';
            snprintf(branch + strlen(branch), sizeof(branch) - strlen(branch), "…");
        }
    }
    pclose(fp);
    return branch;
}

/* Compose the persistent status bar in the dsco "powerline" style.
 * Layout (left → right):
 *   [user@cwd][branch][model][in/out][cost][turn][tools]                [clock]
 */
void tui_status_bar_render(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    if (!sb->visible) {
        pthread_mutex_unlock(&sb->mutex);
        return;
    }

    int rows = tui_term_height();
    int cols = tui_term_width();

    /* Snapshot fields under lock so we can release before slow git call */
    char model[64];
    char slot_name[64];
    snprintf(model, sizeof(model), "%s", sb->model);
    snprintf(slot_name, sizeof(slot_name), "%s", sb->slot_name);
    int in_tok = sb->input_tokens;
    int out_tok = sb->output_tokens;
    double cost = sb->cost;
    int turn = sb->turn;
    int tools = sb->tools_used;
    bool show_clock = sb->show_clock;
    double splash_started_at = sb->splash_started_at;
    pthread_mutex_unlock(&sb->mutex);

    /* Format token counts with K suffix */
    char in_str[16], out_str[16];
    if (in_tok >= 1000)
        snprintf(in_str, sizeof(in_str), "%.1fk", in_tok / 1000.0);
    else
        snprintf(in_str, sizeof(in_str), "%d", in_tok);
    if (out_tok >= 1000)
        snprintf(out_str, sizeof(out_str), "%.1fk", out_tok / 1000.0);
    else
        snprintf(out_str, sizeof(out_str), "%d", out_tok);

    /* Shorten model alias for display */
    char short_model[40];
    {
        const char *slash = strrchr(model, '/');
        const char *m = slash ? slash + 1 : model;
        snprintf(short_model, sizeof(short_model), "%s", m);
        if (strlen(short_model) > 22)
            short_model[22] = '\0';
    }

    const char *user = sb_username();
    const char *cwd = sb_short_cwd();
    const char *branch = sb_git_branch();

    tui_term_lock();
    tui_save_cursor();
    /* Hide cursor while painting to avoid flicker, restore after */
    fprintf(stderr, "\033[?25l");

    /* The composer box (top rule / input / bottom rule / hint) is drawn
     * by tui_input_panel_render. Here we only repaint the bottom-most
     * powerline status row. */
    int status_row = rows;
    (void)short_model;

    /* ── Status bar row (rows): powerline-style segments ────────────── */
    tui_cursor_move(status_row, 1);
    fprintf(stderr, "\033[2K");

    /* Segment colors (256-color) */
    /* user@cwd  bg=24 fg=255 ; branch bg=22 fg=255 ; metrics bg=236 fg=252 */
    char left_buf[512];
    int li = 0;
    double logo_age = difftime(time(NULL), (time_t)splash_started_at);
    bool logo_subdued = (logo_age >= 2.0);
    li += snprintf(left_buf + li, sizeof(left_buf) - li, "\033[48;5;24m\033[38;5;255m ");
    {
        char logo_buf[96] = {0};
        FILE *mf = fmemopen(logo_buf, sizeof(logo_buf), "w");
        if (mf) {
            tui_logo_compact_mark(mf, logo_subdued);
            fclose(mf);
            li += snprintf(left_buf + li, sizeof(left_buf) - li, "%s  ", logo_buf);
        }
    }
    li += snprintf(left_buf + li, sizeof(left_buf) - li, "%s@%s ", user, cwd);
    if (branch[0]) {
        li += snprintf(left_buf + li, sizeof(left_buf) - li, "\033[48;5;22m\033[38;5;255m  %s ",
                       branch);
    }
    /* Slot segment: bg=54 (purple) fg=255, only when a named slot is active */
    if (slot_name[0]) {
        li += snprintf(left_buf + li, sizeof(left_buf) - li, "\033[48;5;54m\033[38;5;255m  %s ",
                       slot_name);
    }
    li += snprintf(left_buf + li, sizeof(left_buf) - li,
                   "\033[48;5;236m\033[38;5;252m"
                   " in:%s out:%s "
                   "\033[38;5;120m$%.2f\033[38;5;252m "
                   "│ t%d │ %d⚙ ",
                   in_str, out_str, cost, turn, tools);
    fputs(left_buf, stderr);

    /* Compute visible width of left segment (rough — count printable runs).
     * For simplicity we count chars not in ESC sequences. */
    int vis = 0;
    for (const char *p = left_buf; *p;) {
        if (*p == '\033') {
            while (*p && *p != 'm')
                p++;
            if (*p)
                p++;
            continue;
        }
        if ((unsigned char)*p < 0x80) {
            vis++;
            p++;
        } else {
            /* UTF-8 continuation: count as 1 visual cell */
            vis++;
            p++;
            while (((unsigned char)*p & 0xC0) == 0x80)
                p++;
        }
    }

    /* Pad with grey to fill remainder */
    fprintf(stderr, "\033[48;5;236m");
    int pad = cols - vis;
    if (show_clock)
        pad -= 7; /* reserve right cluster width */
    for (int i = 0; i < pad; i++)
        fputc(' ', stderr);

    /* Right cluster: clock */
    if (show_clock) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        fprintf(stderr, "\033[38;5;255m %02d:%02d ", tm->tm_hour, tm->tm_min);
    }
    fprintf(stderr, "\033[0m");

    /* Restore cursor + visibility */
    fprintf(stderr, "\033[?25h");
    tui_restore_cursor();
    fflush(stderr);
    tui_term_unlock();
}

/* ── Input Panel / Composer (persistent bottom panel) ────────────────── */
/*
 * Layout (bottom 7 rows of the terminal, fixed by scroll-region trick):
 *   row N-6: top horizontal rule ─────────────
 *   row N-5: notification row 1 (middle whitespace / realtime notifications)
 *   row N-4: notification row 2 (middle whitespace / realtime notifications)
 *   row N-3: "❯ " + input (dual-state caret: colored when active, grey idle)
 *   row N-2: bottom horizontal rule ─────────────
 *   row N-1: dim hint footer ("↵ send · ⌥↵ newline · esc interrupt · /help")
 *   row N  : powerline status bar (drawn by tui_status_bar_render)
 *
 * Multi-line input: the composer keeps a byte buffer, word-wraps and
 * scrolls internally so the cursor line is always visible on row N-3.
 * (Growing the box requires shrinking the scroll region, so we keep the
 * visible input fixed at 1 row for the persistent layout and scroll
 * the buffer instead.)
 *
 * Notifications: tui_panel_notify() pushes a (level, text, timestamp)
 * record into a tiny ring. The two middle rows render newest-first and
 * entries auto-expire after TUI_PANEL_NOTIFY_TTL_S seconds so idle state
 * shows whitespace.
 */

#include <termios.h>
#include <sys/select.h>
#include <errno.h>

/* ── Panel notification ring ──────────────────────────────────────────── */
typedef struct {
    tui_panel_note_level_t level;
    double ts; /* monotonic seconds; 0 = empty */
    char text[200];
} panel_note_t;

static pthread_mutex_t g_panel_note_mu = PTHREAD_MUTEX_INITIALIZER;
static panel_note_t g_panel_notes[TUI_PANEL_NOTIFY_SLOTS];

static double panel_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *panel_note_color(tui_panel_note_level_t lvl) {
    switch (lvl) {
        case TUI_PANEL_NOTE_INFO:
            return "\033[38;5;110m"; /* soft blue */
        case TUI_PANEL_NOTE_OK:
            return "\033[38;5;114m"; /* green */
        case TUI_PANEL_NOTE_WARN:
            return "\033[38;5;215m"; /* amber */
        case TUI_PANEL_NOTE_ERROR:
            return "\033[1;38;5;203m"; /* bold red */
        case TUI_PANEL_NOTE_ACTIVITY:
            return "\033[38;5;213m"; /* pink */
    }
    return "\033[38;5;244m";
}

static const char *panel_note_glyph(tui_panel_note_level_t lvl) {
    switch (lvl) {
        case TUI_PANEL_NOTE_INFO:
            return "·";
        case TUI_PANEL_NOTE_OK:
            return "✓";
        case TUI_PANEL_NOTE_WARN:
            return "▲";
        case TUI_PANEL_NOTE_ERROR:
            return "✗";
        case TUI_PANEL_NOTE_ACTIVITY:
            return "◆";
    }
    return "·";
}

void tui_panel_set_active(tui_status_bar_t *sb, bool active) {
    if (!sb)
        return;
    pthread_mutex_lock(&sb->mutex);
    sb->panel_active = active;
    pthread_mutex_unlock(&sb->mutex);
}

void tui_panel_notify(tui_status_bar_t *sb, tui_panel_note_level_t level, const char *text) {
    if (!text)
        text = "";
    pthread_mutex_lock(&g_panel_note_mu);
    /* Shift entries down (newest at slot 0) */
    for (int i = TUI_PANEL_NOTIFY_SLOTS - 1; i > 0; i--) {
        g_panel_notes[i] = g_panel_notes[i - 1];
    }
    g_panel_notes[0].level = level;
    g_panel_notes[0].ts = panel_now_s();
    snprintf(g_panel_notes[0].text, sizeof(g_panel_notes[0].text), "%s", text);
    pthread_mutex_unlock(&g_panel_note_mu);

    /* Repaint the middle rows only. Full chrome repaint is cheap enough. */
    if (sb)
        tui_input_panel_render(sb, NULL);
}

void tui_panel_notify_clear(tui_status_bar_t *sb) {
    pthread_mutex_lock(&g_panel_note_mu);
    memset(g_panel_notes, 0, sizeof(g_panel_notes));
    pthread_mutex_unlock(&g_panel_note_mu);
    if (sb)
        tui_input_panel_render(sb, NULL);
}

/* Draw the two notification rows. Called from composer_draw_chrome.
 * Expected to run with tui_term_lock() held by caller. */
static void composer_draw_notifications(int rows_total, int cols) {
    double now = panel_now_s();
    /* Copy under lock, render without lock */
    panel_note_t snap[TUI_PANEL_NOTIFY_SLOTS];
    pthread_mutex_lock(&g_panel_note_mu);
    memcpy(snap, g_panel_notes, sizeof(snap));
    pthread_mutex_unlock(&g_panel_note_mu);

    for (int slot = 0; slot < TUI_PANEL_NOTIFY_SLOTS; slot++) {
        /* slot 0 (newest) -> row N-5, slot 1 -> row N-4 */
        int row = rows_total - 5 + slot;
        tui_cursor_move(row, 1);
        fprintf(stderr, "\033[2K");

        const panel_note_t *n = &snap[slot];
        bool alive = n->ts > 0.0 && (now - n->ts) < TUI_PANEL_NOTIFY_TTL_S;
        if (!alive) {
            /* Leave blank — middle whitespace when no active notification. */
            continue;
        }

        /* Prefix: " <color>glyph<reset> " then the text, dim-truncated to fit. */
        fprintf(stderr, " %s%s\033[0m ", panel_note_color(n->level), panel_note_glyph(n->level));
        int overhead = 3; /* space + glyph + space */
        int avail = cols - overhead - 1;
        if (avail < 4)
            avail = 4;

        /* Fade older notifications slightly. */
        double age = now - n->ts;
        bool dim = age > (TUI_PANEL_NOTIFY_TTL_S * 0.6);
        if (dim)
            fprintf(stderr, "\033[2;38;5;245m");
        else
            fprintf(stderr, "\033[38;5;252m");

        /* Truncate to avail cells, counting bytes naively (mostly ASCII).
         * For longer strings with wide chars, rely on terminal clipping. */
        int written = 0;
        for (const char *p = n->text; *p && written < avail; p++) {
            fputc((unsigned char)*p, stderr);
            if (((unsigned char)*p & 0xC0) != 0x80)
                written++;
        }
        fprintf(stderr, "\033[0m");
    }
}

/* Draw the top divider for the 3-row panel. The input row (rows-1) and the
 * status row (rows) are drawn elsewhere. Does NOT draw the input text. */
__attribute__((unused)) static void composer_draw_chrome(const char *model) {
    int rows = tui_term_height();
    int cols = tui_term_width();
    if (cols < 4)
        cols = 4;

    int r_top = rows - 2;

    /* Top rule: ─[ model ]──────────── */
    tui_cursor_move(r_top, 1);
    fprintf(stderr, "\033[2K\033[38;5;240m");
    int used = 0;
    if (model && *model) {
        fprintf(stderr, "─[ \033[1;38;5;213m%s\033[0;38;5;240m ]", model);
        used = 5 + (int)strlen(model);
    }
    for (int i = used; i < cols; i++)
        fputs("─", stderr);
    fprintf(stderr, "\033[0m");

    (void)composer_draw_notifications; /* legacy notification renderer — disabled */
}

/* Count visible utf-8 cells in first `nbytes` bytes of s. */
static int composer_cells(const char *s, size_t nbytes) {
    int cells = 0;
    for (size_t i = 0; i < nbytes;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            i++;
            continue;
        }
        if (c < 0x80) {
            cells++;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cells++;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cells++;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cells++;
            i += 4;
        } else {
            i += 1;
        }
    }
    return cells;
}

/* Caret escape strings for dual-state caret. Active = bold pink,
 * inactive = dim grey. Kept as literals so they can be used directly
 * in fprintf formatting without extra branching. */
#define CARET_ACTIVE "\033[1;38;5;213m❯\033[0m "
#define CARET_INACTIVE "\033[2;38;5;240m❯\033[0m "

/* ── Inline bordered input box (Claude Code style) ────────────────────
 * Full-width rounded box at row r_top growing downward:
 *   r_top              ╭──────────────────╮
 *   r_top+1..r_top+V   │ ❯ content        │   (V = visible content rows)
 *   r_top+1+V          ╰──────────────────╯
 *   r_top+2+V          dim hint footer
 *
 * Visible content rows grow with logical lines, capped at TUI_INBOX_MAX_VISIBLE.
 * If logical lines exceed the cap, the window slides to keep the cursor line in
 * view; a small ▴/▾ marker on the borders signals hidden lines. */
#define TUI_INBOX_MAX_VISIBLE 8

static int inbox_visible_rows(const char *buf, size_t len) {
    int logical = 1;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n')
            logical++;
    return logical > TUI_INBOX_MAX_VISIBLE ? TUI_INBOX_MAX_VISIBLE : logical;
}

static int inbox_total_rows(const char *buf, size_t len) {
    return inbox_visible_rows(buf, len) + 3; /* top + content + bottom + hint */
}

static void inbox_clear(int r_top, int rows) {
    for (int i = 0; i < rows; i++) {
        tui_cursor_move(r_top + i, 1);
        fprintf(stderr, "\033[2K");
    }
}

/* Render the inline box at row r_top. Sets cur_row/cur_col (out params) to
 * the screen coordinates where the terminal cursor should sit. Returns box
 * height (borders + visible content + hint row). */
static int inbox_render(int r_top, const char *buf, size_t len, size_t cur, bool active,
                        int *cur_row, int *cur_col) {
    int cols = tui_term_width();
    if (cols < 12)
        cols = 12;

    /* Count logical lines + locate cursor line */
    int total_lines = 1;
    int cur_line = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            total_lines++;
            if (i < cur)
                cur_line++;
        }
    }
    int visible = total_lines < TUI_INBOX_MAX_VISIBLE ? total_lines : TUI_INBOX_MAX_VISIBLE;

    /* Vertical scroll: slide window so cur_line is in view (biased toward bottom) */
    int vscroll = 0;
    if (total_lines > visible) {
        int desired_view_row = visible - 2;
        if (desired_view_row < 0)
            desired_view_row = 0;
        vscroll = cur_line - desired_view_row;
        if (vscroll < 0)
            vscroll = 0;
        if (vscroll > total_lines - visible)
            vscroll = total_lines - visible;
    }

    const char *border_col = "\033[38;5;240m";
    const char *reset = "\033[0m";

    /* Top border */
    tui_cursor_move(r_top, 1);
    fprintf(stderr, "\033[2K%s╭", border_col);
    for (int i = 1; i < cols - 1; i++)
        fputs("─", stderr);
    fprintf(stderr, "╮%s", reset);

    /* Content rows */
    size_t line_start = 0;
    int line_idx = 0;
    *cur_row = r_top + 1;
    *cur_col = 5;
    for (size_t i = 0; i <= len; i++) {
        bool eol = (i == len) || (buf[i] == '\n');
        if (!eol)
            continue;

        size_t line_end = i;
        if (line_idx >= vscroll && line_idx < vscroll + visible) {
            int draw_row = r_top + 1 + (line_idx - vscroll);
            tui_cursor_move(draw_row, 1);
            fprintf(stderr, "\033[2K");

            /* Left border + caret/continuation marker. Visible cells = 4. */
            if (line_idx == 0) {
                fprintf(stderr, "%s│%s %s", border_col, reset,
                        active ? "\033[1;38;5;213m❯\033[0m " : "\033[2;38;5;240m❯\033[0m ");
            } else {
                fprintf(stderr, "%s│%s   ", border_col, reset);
            }
            int prefix_cells = 4;

            int avail = cols - prefix_cells - 2; /* leave " │" on the right */
            if (avail < 4)
                avail = 4;

            size_t line_len = line_end - line_start;
            int line_cells = composer_cells(buf + line_start, line_len);

            /* Horizontal scroll: only the cursor-containing line scrolls */
            int hscroll = 0;
            int cur_cells = 0;
            if (line_idx == cur_line) {
                cur_cells = composer_cells(buf + line_start, cur - line_start);
                int target = avail - 4;
                if (target < 0)
                    target = 0;
                if (cur_cells > target)
                    hscroll = cur_cells - target;
            } else if (line_cells > avail) {
                /* Truncate non-cursor lines from the right */
            }

            /* Skip hscroll cells from line_start */
            size_t render_j = line_start;
            int skipped = 0;
            while (render_j < line_end && skipped < hscroll) {
                unsigned char c = (unsigned char)buf[render_j];
                if (c < 0x80)
                    render_j += 1;
                else if ((c & 0xE0) == 0xC0)
                    render_j += 2;
                else if ((c & 0xF0) == 0xE0)
                    render_j += 3;
                else if ((c & 0xF8) == 0xF0)
                    render_j += 4;
                else
                    render_j += 1;
                skipped++;
            }

            if (hscroll > 0) {
                fprintf(stderr, "%s…%s", border_col, reset);
            }

            int written = (hscroll > 0) ? 1 : 0;
            while (render_j < line_end && written < avail) {
                unsigned char c = (unsigned char)buf[render_j];
                size_t clen = 1;
                if (c < 0x80)
                    clen = 1;
                else if ((c & 0xE0) == 0xC0)
                    clen = 2;
                else if ((c & 0xF0) == 0xE0)
                    clen = 3;
                else if ((c & 0xF8) == 0xF0)
                    clen = 4;
                if (render_j + clen > line_end)
                    clen = 1;
                fwrite(buf + render_j, 1, clen, stderr);
                render_j += clen;
                written++;
            }

            /* Truncation indicator if more text to the right */
            if (render_j < line_end && written < avail) {
                fprintf(stderr, "%s…%s", border_col, reset);
                written++;
            }

            /* Pad to right edge then right border */
            for (int p = written; p < avail; p++)
                fputc(' ', stderr);
            fprintf(stderr, " %s│%s", border_col, reset);

            if (line_idx == cur_line) {
                *cur_row = draw_row;
                *cur_col = 1 + prefix_cells + (cur_cells - hscroll);
                if (*cur_col < 1 + prefix_cells)
                    *cur_col = 1 + prefix_cells;
                if (*cur_col > cols - 2)
                    *cur_col = cols - 2;
            }
        }

        line_idx++;
        line_start = i + 1;
        if (i == len)
            break;
    }

    /* Bottom border */
    int r_bot = r_top + 1 + visible;
    tui_cursor_move(r_bot, 1);
    fprintf(stderr, "\033[2K%s╰", border_col);
    for (int i = 1; i < cols - 1; i++)
        fputs("─", stderr);
    fprintf(stderr, "╯%s", reset);

    /* Scroll indicators on borders */
    if (vscroll > 0) {
        tui_cursor_move(r_top, cols - 3);
        fprintf(stderr, "%s\xe2\x96\xb4%s", border_col, reset); /* ▴ */
    }
    if (vscroll + visible < total_lines) {
        tui_cursor_move(r_bot, cols - 3);
        fprintf(stderr, "%s\xe2\x96\xbe%s", border_col, reset); /* ▾ */
    }

    /* Hint footer */
    int r_hint = r_bot + 1;
    tui_cursor_move(r_hint, 1);
    fprintf(stderr,
            "\033[2K\033[2;38;5;245m  ↵ send · ⌥↵ newline · ctrl+c interrupt · /help\033[0m");

    return visible + 3;
}

/* Draw the active input line given current buffer + cursor byte offset.
 * `active` selects caret color when no explicit prompt is supplied.
 * Shows the last visible slice of text if wider than available cells,
 * placing the cursor so it remains in view. Returns the screen column
 * (1-indexed) where the terminal cursor should sit. */
__attribute__((unused)) static int composer_draw_input(const char *prompt, const char *buf,
                                                       size_t len, size_t cur, bool active) {
    int rows = tui_term_height();
    int cols = tui_term_width();
    int r_in = rows - 1;

    tui_cursor_move(r_in, 1);
    fprintf(stderr, "\033[2K");

    /* Render prompt: use a static "❯ " if caller passed NULL.
     * We strip readline \001/\002 no-echo wrappers if present. */
    const char *p = prompt && *prompt ? prompt : (active ? CARET_ACTIVE : CARET_INACTIVE);
    char clean[320];
    {
        size_t oi = 0;
        for (const char *q = p; *q && oi + 1 < sizeof(clean); q++) {
            if (*q == '\001' || *q == '\002')
                continue;
            clean[oi++] = *q;
        }
        clean[oi] = '\0';
    }

    /* Visible cells of the prompt (skipping ANSI escapes) */
    int prompt_cells = 0;
    for (const char *q = clean; *q;) {
        if (*q == '\033') {
            while (*q && *q != 'm')
                q++;
            if (*q)
                q++;
            continue;
        }
        if ((unsigned char)*q < 0x80) {
            prompt_cells++;
            q++;
        } else {
            prompt_cells++;
            q++;
            while (((unsigned char)*q & 0xC0) == 0x80)
                q++;
        }
    }

    fputs(clean, stderr);

    int avail = cols - prompt_cells - 1;
    if (avail < 4)
        avail = 4;

    /* Find the start of the logical line containing the cursor (for
     * multi-line buffers). Scroll within that line if too wide. */
    size_t line_start = 0;
    for (size_t i = 0; i < cur; i++)
        if (buf[i] == '\n')
            line_start = i + 1;
    size_t line_end = cur;
    while (line_end < len && buf[line_end] != '\n')
        line_end++;

    int cur_cells_from_start = composer_cells(buf + line_start, cur - line_start);
    int line_total_cells = composer_cells(buf + line_start, line_end - line_start);

    size_t render_start = line_start;
    int render_skip_cells = 0;
    int cur_col_in_view = cur_cells_from_start;
    if (line_total_cells > avail) {
        /* Scroll so cursor stays 4 cells from the right */
        int target_view_col = avail - 4;
        if (target_view_col < 0)
            target_view_col = 0;
        if (cur_cells_from_start > target_view_col) {
            render_skip_cells = cur_cells_from_start - target_view_col;
            /* Advance render_start by render_skip_cells visual cells */
            size_t i = line_start;
            int skipped = 0;
            while (i < line_end && skipped < render_skip_cells) {
                unsigned char c = (unsigned char)buf[i];
                if (c < 0x80)
                    i += 1;
                else if ((c & 0xE0) == 0xC0)
                    i += 2;
                else if ((c & 0xF0) == 0xE0)
                    i += 3;
                else if ((c & 0xF8) == 0xF0)
                    i += 4;
                else
                    i += 1;
                skipped++;
            }
            render_start = i;
            cur_col_in_view = cur_cells_from_start - render_skip_cells;
        }
    }

    /* Write text from render_start up to min(line_end, render_start+avail cells).
     * Also: if we have multi-line buffer, prefix with "⏎ " continuation marker
     * on the left if there is content before line_start. */
    int multi_prefix_cells = 0;
    if (line_start > 0) {
        fprintf(stderr, "\033[38;5;240m… \033[0m");
        multi_prefix_cells = 2;
    }

    int cells_written = 0;
    size_t j = render_start;
    while (j < line_end && cells_written + multi_prefix_cells < avail) {
        unsigned char c = (unsigned char)buf[j];
        if (c == '\n')
            break;
        size_t clen = 1;
        if (c < 0x80)
            clen = 1;
        else if ((c & 0xE0) == 0xC0)
            clen = 2;
        else if ((c & 0xF0) == 0xE0)
            clen = 3;
        else if ((c & 0xF8) == 0xF0)
            clen = 4;
        if (j + clen > line_end)
            clen = 1;
        fwrite(buf + j, 1, clen, stderr);
        j += clen;
        cells_written++;
    }

    /* Continuation ellipsis if more text beyond view */
    if (line_end > j) {
        fprintf(stderr, "\033[38;5;240m…\033[0m");
    }

    /* Also: if the whole buffer has additional logical lines after the
     * current cursor-line, hint at them */
    if (line_end < len) {
        fprintf(stderr, "\033[38;5;240m ⏎\033[0m");
    }

    int cursor_col = 1 + prompt_cells + multi_prefix_cells + cur_col_in_view;
    return cursor_col;
}

/* Query current cursor row via DSR (ESC[6n). Returns row, or `fallback` if
 * the query fails or times out. Briefly enters raw mode if stdin is a tty
 * in cooked mode; restores the original termios on exit. */
static int tui_query_cursor_row(int fallback) {
    int fd = STDIN_FILENO;
    if (!isatty(fd) || !isatty(STDERR_FILENO))
        return fallback;

    struct termios saved, raw;
    bool restore = false;
    if (tcgetattr(fd, &saved) == 0) {
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 2; /* 200ms per read */
        if (tcsetattr(fd, TCSANOW, &raw) == 0)
            restore = true;
    }

    fputs("\033[6n", stderr);
    fflush(stderr);

    char buf[32];
    size_t n = 0;
    int deadline_ms = 300;
    struct timeval start, now;
    gettimeofday(&start, NULL);
    while (n + 1 < sizeof(buf)) {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        int rv = select(fd + 1, &rfd, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(fd, &rfd)) {
            char c;
            ssize_t r = read(fd, &c, 1);
            if (r <= 0)
                break;
            buf[n++] = c;
            if (c == 'R')
                break;
        }
        gettimeofday(&now, NULL);
        int elapsed_ms =
            (int)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000);
        if (elapsed_ms >= deadline_ms)
            break;
    }
    buf[n] = '\0';

    if (restore)
        tcsetattr(fd, TCSANOW, &saved);

    const char *p = buf;
    while (*p && *p != '\033')
        p++;
    if (*p != '\033')
        return fallback;
    int row = 0, col = 0;
    if (sscanf(p, "\033[%d;%dR", &row, &col) == 2 && row > 0)
        return row;
    return fallback;
}

void tui_pad_to_panel_anchor(void) {
    int rows = tui_term_height();
    int anchor = rows - 3; /* row just above the 3-row panel area */
    if (anchor < 1)
        return;

    tui_term_lock();
    int cur = tui_query_cursor_row(anchor);
    if (cur < anchor) {
        for (int i = cur; i < anchor; i++)
            fputc('\n', stderr);
        fflush(stderr);
    }
    tui_term_unlock();
}

/* The input panel is now rendered inline by tui_composer_read (Claude-Code
 * style bordered box at the current cursor position). These hooks become
 * no-ops so legacy callers (notification ring, etc.) don't paint over the
 * scrollback. Notifications are still recorded — call sites can be reworked
 * to print inline if visibility is needed. */
void tui_input_panel_render(tui_status_bar_t *sb, const char *prompt_hint) {
    (void)sb;
    (void)prompt_hint;
}

void tui_input_panel_clear(tui_status_bar_t *sb) {
    (void)sb;
}

void tui_bottom_panel_refresh(tui_status_bar_t *sb, const char *prompt_hint) {
    (void)sb;
    (void)prompt_hint;
}

/* ── Composer read: raw-termios multi-line input box ──────────────────── */

static ssize_t composer_read_byte(int fd, int timeout_ms, unsigned char *out) {
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(fd, &rfd);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(fd + 1, &rfd, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (r <= 0)
        return r;
    return read(fd, out, 1);
}

/* Insert `s` (len bytes) at buf[cur], shifting the rest. Grows cursor. */
static void composer_insert(char *buf, size_t cap, size_t *len, size_t *cur, const char *s,
                            size_t slen) {
    if (*len + slen >= cap - 1)
        return;
    memmove(buf + *cur + slen, buf + *cur, *len - *cur);
    memcpy(buf + *cur, s, slen);
    *len += slen;
    *cur += slen;
    buf[*len] = '\0';
}

/* Delete one utf-8 codepoint before cursor. */
static void composer_backspace(char *buf, size_t *len, size_t *cur) {
    if (*cur == 0)
        return;
    size_t start = *cur - 1;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80)
        start--;
    size_t removed = *cur - start;
    memmove(buf + start, buf + *cur, *len - *cur);
    *len -= removed;
    *cur = start;
    buf[*len] = '\0';
}

/* Delete one utf-8 codepoint at cursor. */
static void composer_delete(char *buf, size_t *len, size_t *cur) {
    if (*cur >= *len)
        return;
    size_t end = *cur + 1;
    while (end < *len && ((unsigned char)buf[end] & 0xC0) == 0x80)
        end++;
    size_t removed = end - *cur;
    memmove(buf + *cur, buf + end, *len - end);
    *len -= removed;
    buf[*len] = '\0';
}

/* Cursor left by one codepoint. */
static void composer_cur_left(const char *buf, size_t *cur) {
    if (*cur == 0)
        return;
    (*cur)--;
    while (*cur > 0 && ((unsigned char)buf[*cur] & 0xC0) == 0x80)
        (*cur)--;
}

/* Cursor right by one codepoint. */
static void composer_cur_right(const char *buf, size_t len, size_t *cur) {
    if (*cur >= len)
        return;
    (*cur)++;
    while (*cur < len && ((unsigned char)buf[*cur] & 0xC0) == 0x80)
        (*cur)++;
}

/* Delete previous word (whitespace-delimited). */
static void composer_kill_word(char *buf, size_t *len, size_t *cur) {
    size_t c = *cur;
    while (c > 0 && (buf[c - 1] == ' ' || buf[c - 1] == '\t'))
        c--;
    while (c > 0 && buf[c - 1] != ' ' && buf[c - 1] != '\t' && buf[c - 1] != '\n')
        c--;
    size_t removed = *cur - c;
    memmove(buf + c, buf + *cur, *len - *cur);
    *len -= removed;
    *cur = c;
    buf[*len] = '\0';
}

/* Move cursor to start of current logical line. */
static void composer_home(const char *buf, size_t *cur) {
    while (*cur > 0 && buf[*cur - 1] != '\n')
        (*cur)--;
}

/* Move cursor to end of current logical line. */
static void composer_end(const char *buf, size_t len, size_t *cur) {
    while (*cur < len && buf[*cur] != '\n')
        (*cur)++;
}

/* Try to pull an image from the system clipboard and save it to a temp file.
 * Returns true and fills out_path on success.  macOS-only via osascript. */
static bool composer_clipboard_grab_image(char *out_path, size_t out_sz) {
#ifdef __APPLE__
    char png_path[512];
    snprintf(png_path, sizeof(png_path), "/tmp/dsco_clip_%d.png", getpid());

    /* Write a tiny AppleScript to a temp .scpt and execute it.
     * «class PNGf» = 0xC2 0xAB class PNGf 0xC2 0xBB in UTF-8.
     * We also try «class JPEG» (JPEG clipboard data) as a fallback.      */
    char scpt[512];
    snprintf(scpt, sizeof(scpt), "/tmp/dsco_clip_%d.scpt", getpid());

    FILE *sf = fopen(scpt, "w");
    if (!sf)
        return false;

    /* AppleScript: try PNG, then JPEG.
     * The \xAB / \xBB bytes are « and » (U+00AB, U+00BB) encoded as UTF-8
     * two-byte sequences.  We split the string literals so the hex escapes
     * do not bleed into the following ASCII characters.                     */
    static const char lq[] = {(char)0xC2, (char)0xAB, '\0'}; /* « */
    static const char rq[] = {(char)0xC2, (char)0xBB, '\0'}; /* » */
    fprintf(sf,
            "set outPath to \"%s\"\n"
            "try\n"
            "  set d to the clipboard as %sclass PNGf%s\n"
            "  set fh to open for access POSIX file outPath with write permission\n"
            "  set eof of fh to 0\n"
            "  write d to fh\n"
            "  close access fh\n"
            "on error\n"
            "  try\n"
            "    set d to the clipboard as %sclass JPEG%s\n"
            "    set fh to open for access POSIX file outPath with write permission\n"
            "    set eof of fh to 0\n"
            "    write d to fh\n"
            "    close access fh\n"
            "  on error\n"
            "    return 1\n"
            "  end try\n"
            "end try\n",
            png_path, lq, rq, lq, rq);
    fclose(sf);

    char cmd[640];
    snprintf(cmd, sizeof(cmd), "osascript %s >/dev/null 2>&1", scpt);
    int rc = system(cmd);
    unlink(scpt);

    struct stat st;
    if (rc == 0 && stat(png_path, &st) == 0 && st.st_size > 16) {
        snprintf(out_path, out_sz, "%s", png_path);
        return true;
    }
    unlink(png_path);
    return false;
#else
    (void)out_path;
    (void)out_sz;
    return false;
#endif
}

/* ── Slash-command dropdown ───────────────────────────────────────────────
 * Registered by agent.c at startup; consumed by tui_composer_read to render a
 * live, prefix-filtered popup that expands beneath the input line. */
#define TUI_SLASHMENU_MAX 8   /* max rows shown at once (scrolls beyond) */
#define TUI_SLASHMENU_CAP 128 /* max matches tracked per keystroke */

/* ═══ @ Image-picker ═══════════════════════════════════════════════════ */
#define TUI_IMGPICK_MAX   10
#define TUI_IMGPICK_CAP  256  /* picker scan; actual send cap is IMG_MAX_PER_MSG=100 */
#define TUI_IMGPICK_NAMEW 38
static const char *s_imgpick_exts[]={
    ".png",".jpg",".jpeg",".gif",".webp",
    ".bmp",".tif",".tiff",".heic",".heif",".avif",
    ".PNG",".JPG",".JPEG",".GIF",".WEBP",
    ".BMP",".TIF",".TIFF",".HEIC",".HEIF",".AVIF",NULL};
static bool imgpick_has_ext(const char *n){
    size_t nl=strlen(n);
    for(int i=0;s_imgpick_exts[i];i++){size_t el=strlen(s_imgpick_exts[i]);
        if(nl>=el&&strcasecmp(n+nl-el,s_imgpick_exts[i])==0)return true;}
    return false;}
static char s_imgpick_paths[TUI_IMGPICK_CAP][4096];
static int  s_imgpick_count=0;
static void imgpick_scan(const char *dir,const char *pfx){
    s_imgpick_count=0;
    DIR *dp=opendir((dir&&*dir)?dir:".");if(!dp)return;
    struct dirent *de;size_t pl=pfx?strlen(pfx):0;
    char cwd[4096];if(getcwd(cwd,sizeof(cwd))==NULL)strcpy(cwd,".");
    const char *base=(dir&&*dir)?dir:cwd;
    while((de=readdir(dp))!=NULL&&s_imgpick_count<TUI_IMGPICK_CAP){
        const char *nm=de->d_name;if(nm[0]=='.')continue;
        if(!imgpick_has_ext(nm))continue;
        if(pl>0&&strncasecmp(nm,pfx,pl)!=0)continue;
        snprintf(s_imgpick_paths[s_imgpick_count],sizeof(s_imgpick_paths[0]),
                 "%s/%s",base,nm);s_imgpick_count++;}
    closedir(dp);}
static int imgpick_render(int r_first,int count,int sel){
    int shown=count<TUI_IMGPICK_MAX?count:TUI_IMGPICK_MAX;if(shown==0)return 0;
    int cols=tui_term_width();if(cols<30)cols=30;
    int top=0;if(sel>=shown)top=sel-shown+1;
    if(top>count-shown)top=count-shown;if(top<0)top=0;
    for(int r=0;r<shown;r++){
        int gi=top+r;bool on=(gi==sel);
        const char *path=s_imgpick_paths[gi];
        const char *fname=strrchr(path,'/');fname=fname?fname+1:path;
        tui_cursor_move(r_first+r,1);fprintf(stderr,"\033[2K");
        const char *mark=on?"\033[38;5;45m\xe2\x96\xb8\033[0m":" ";
        const char *nc=on?"\033[1;38;5;45m":"\033[38;5;250m";
        char nbuf[TUI_IMGPICK_NAMEW+4];snprintf(nbuf,sizeof(nbuf),"%s",fname);
        char tmp[4096];snprintf(tmp,sizeof(tmp),"%s",path);
        char *sl=strrchr(tmp,'/');if(sl)*sl='\0';
        int used=2+1+1+3+1+TUI_IMGPICK_NAMEW+2;int dbud=cols-used-1;if(dbud<0)dbud=0;
        char dbuf[4100];snprintf(dbuf,sizeof(dbuf),"%s",tmp);
        if((int)strlen(dbuf)>dbud&&dbud>4){char el[4100];
            snprintf(el,sizeof(el),"\xe2\x80\xa6%s",dbuf+(int)strlen(dbuf)-(dbud-1));
            snprintf(dbuf,sizeof(dbuf),"%s",el);}
        else if((int)strlen(dbuf)>dbud)dbuf[dbud]='\0';
        fprintf(stderr,"  %s \xf0\x9f\x96\xbc %s%-*s\033[0m  \033[2;38;5;245m%s\033[0m",
                mark,nc,TUI_IMGPICK_NAMEW,nbuf,dbuf);}
    return shown;}
static bool imgpick_parse_at(const char *buf,size_t cur,
                              char *dir_out,size_t dir_sz,
                              char *pfx_out,size_t pfx_sz){
    if(cur==0)return false;size_t p=cur;
    while(p>0&&buf[p-1]!=' '&&buf[p-1]!='\n')p--;
    if(p>=cur||buf[p]!='@')return false;
    const char *after=buf+p+1;size_t alen=cur-p-1;
    const char *sl=NULL;
    for(size_t i=0;i<alen;i++)if(after[i]=='/')sl=after+i;
    if(sl){size_t dlen=(size_t)(sl-after);if(dlen>=dir_sz)dlen=dir_sz-1;
        memcpy(dir_out,after,dlen);dir_out[dlen]='\0';
        snprintf(pfx_out,pfx_sz,"%s",sl+1);
    }else{char _cwd[4096];if(getcwd(_cwd,sizeof(_cwd))==NULL)strcpy(_cwd,".");
        size_t dlen2=strlen(_cwd);if(dlen2>=dir_sz)dlen2=dir_sz-1;
        memcpy(dir_out,_cwd,dlen2);dir_out[dlen2]='\0';
        size_t plen=alen<pfx_sz-1?alen:pfx_sz-1;
        memcpy(pfx_out,after,plen);pfx_out[plen]='\0';}
    return true;}


static const tui_cmd_entry_t *s_slash_cmds = NULL;
static int s_slash_cmds_n = 0;

void tui_composer_set_slash_commands(const tui_cmd_entry_t *cmds, int count) {
    s_slash_cmds = cmds;
    s_slash_cmds_n = (cmds && count > 0) ? count : 0;
}

/* Collect indices of registered commands whose name is prefixed by the current
 * input. Only fires when the buffer is a single "/<token>" with no space or
 * newline yet — i.e. the user is still typing the command word. Writes up to
 * `max` match indices into `idx` and sets `*out_count`. */
static void composer_slash_match(const char *buf, size_t len, int *idx, int *out_count, int max) {
    *out_count = 0;
    if (!s_slash_cmds || s_slash_cmds_n == 0)
        return;
    if (len == 0 || buf[0] != '/')
        return;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ' ' || buf[i] == '\n')
            return; /* past the command token */
    }
    for (int i = 0; i < s_slash_cmds_n && *out_count < max; i++) {
        const char *name = s_slash_cmds[i].name;
        if (!name || name[0] != '/')
            continue;
        if (strncmp(name, buf, len) == 0)
            idx[(*out_count)++] = i;
    }
}

/* Rows the dropdown will occupy for `count` matches (0 when closed). */
static int slashmenu_rows(int count) {
    if (count <= 0)
        return 0;
    return count < TUI_SLASHMENU_MAX ? count : TUI_SLASHMENU_MAX;
}

/* Draw the dropdown starting at screen row r_first. Returns rows drawn.
 * `idx`/`count` come from composer_slash_match; `sel` is the highlighted row. */
static int slashmenu_render(int r_first, const int *idx, int count, int sel) {
    int shown = slashmenu_rows(count);
    if (shown == 0)
        return 0;

    int cols = tui_term_width();
    if (cols < 20)
        cols = 20;

    /* Slide a window so the selection stays visible. */
    int top = 0;
    if (sel >= shown)
        top = sel - shown + 1;
    if (top > count - shown)
        top = count - shown;
    if (top < 0)
        top = 0;

    /* Widest visible command name → column width for the description gutter. */
    int namew = 0;
    for (int r = 0; r < shown; r++) {
        int l = (int)strlen(s_slash_cmds[idx[top + r]].name);
        if (l > namew)
            namew = l;
    }
    if (namew > 22)
        namew = 22;

    for (int r = 0; r < shown; r++) {
        int gi = idx[top + r];
        bool on = (top + r == sel);
        const char *name = s_slash_cmds[gi].name;
        const char *desc = s_slash_cmds[gi].desc ? s_slash_cmds[gi].desc : "";

        tui_cursor_move(r_first + r, 1);
        fprintf(stderr, "\033[2K");

        /* "  ▸ /name        description" — pink for the active row, dim else. */
        const char *mark = on ? "\033[38;5;213m\xe2\x96\xb8\033[0m" : " ";
        const char *ncol = on ? "\033[1;38;5;213m" : "\033[38;5;250m";

        /* Truncate description to the remaining cells. */
        int used = 2 /*indent*/ + 1 /*mark*/ + 1 /*sp*/ + namew + 2 /*gap*/;
        int dbudget = cols - used - 1;
        if (dbudget < 0)
            dbudget = 0;
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf), "%s", desc);
        if ((int)strlen(dbuf) > dbudget)
            dbuf[dbudget] = '\0';

        fprintf(stderr, "  %s %s%-*s\033[0m  \033[2;38;5;245m%s\033[0m", mark, ncol, namew, name,
                dbuf);
    }
    return shown;
}

/* Paint the input box plus (if open) the slash dropdown beneath it. Cursor
 * coordinates (always inside the box) are returned via cur_r/cur_c. Returns
 * the combined row count so the caller can anchor/scroll/clear correctly. */
static int composer_paint(int r_top, const char *buf, size_t len, size_t cur, const int *sug_idx,
                          int sug_count, int sug_sel, int *cur_r, int *cur_c,
                          int imgpick_cnt, int imgpick_s) {
    int h = inbox_render(r_top, buf, len, cur, true, cur_r, cur_c);
    int m = slashmenu_render(r_top + h, sug_idx, sug_count, sug_sel);
    int q = imgpick_render(r_top + h + m, imgpick_cnt, imgpick_s);
    return h + m + q;
}

char *tui_composer_read(tui_status_bar_t *sb, const char *prompt, char *out, size_t out_sz) {
    if (!out || out_sz == 0)
        return NULL;
    if (!isatty(STDIN_FILENO)) {
        /* Fallback: just fgets a line */
        if (!fgets(out, (int)out_sz, stdin))
            return NULL;
        size_t l = strlen(out);
        while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r'))
            out[--l] = '\0';
        return out;
    }

    /* Save + switch to raw mode */
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        if (!fgets(out, (int)out_sz, stdin))
            return NULL;
        size_t l = strlen(out);
        while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r'))
            out[--l] = '\0';
        return out;
    }
    raw = saved;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    /* Enable bracketed paste */
    fprintf(stderr, "\033[?2004h");
    fflush(stderr);

    char *buf = (char *)calloc(1, TUI_COMPOSER_BUF_CAP);
    if (!buf) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        return NULL;
    }
    size_t len = 0, cur = 0;

    /* Slash-command dropdown state. `sug_idx` holds indices into the registered
     * command table for the current prefix matches; `sug_sel` is the highlight;
     * `sug_suppress` is set when the user dismisses the menu (Esc) or completes
     * a command (Tab) so it stays closed until the token changes again. */
    int sug_idx[TUI_SLASHMENU_CAP];
    int sug_count = 0;
    int sug_sel = 0;
    bool sug_suppress = false;

    /* @-image-picker state */
    bool imgpick_open  = false;
    int  imgpick_count = 0;
    int  imgpick_sel   = 0;

    /* History snapshot pointer for up/down */
    /* (We don't integrate with readline history here — keep composer
     *  self-contained. History nav is handled by the agent loop via
     *  the returned string being pushed into readline history.) */

    /* Initial paint: chrome + empty input */
    pthread_mutex_lock(&sb->mutex);
    char model_copy[64];
    snprintf(model_copy, sizeof(model_copy), "%s", sb->model);
    pthread_mutex_unlock(&sb->mutex);
    const char *slash = strrchr(model_copy, '/');
    const char *short_model = slash ? slash + 1 : model_copy;
    char sm[40];
    snprintf(sm, sizeof(sm), "%s", short_model);
    if (strlen(sm) > 22)
        sm[22] = '\0';

    (void)sm;
    (void)prompt;
    int rows = tui_term_height();

    /* Mark panel active — caret renders in its colored state while we're
     * reading keystrokes. Cleared on exit below. */
    tui_panel_set_active(sb, true);

    /* Anchor the inline box at the current cursor row. If the box would
     * extend past the bottom of the terminal, scroll up first so it fits.
     * We track r_top across keystrokes so resize/growth can re-anchor. */
    int needed = inbox_total_rows(buf, len);
    int r_top = tui_query_cursor_row(rows - needed);
    if (r_top < 1)
        r_top = 1;
    if (r_top + needed - 1 > rows) {
        int overflow = (r_top + needed - 1) - rows;
        tui_term_lock();
        tui_cursor_move(rows, 1);
        for (int i = 0; i < overflow; i++)
            fputc('\n', stderr);
        fflush(stderr);
        tui_term_unlock();
        r_top -= overflow;
        if (r_top < 1)
            r_top = 1;
    }
    int prev_height = 0;

    tui_term_lock();
    fprintf(stderr, "\033[?25l");
    int cur_r = r_top + 1, cur_c = 5;
    prev_height = composer_paint(r_top, buf, len, cur, sug_idx, sug_count, sug_sel, &cur_r, &cur_c, imgpick_count, imgpick_sel);
    tui_cursor_move(cur_r, cur_c);
    fprintf(stderr, "\033[?25h");
    fflush(stderr);
    tui_term_unlock();

    bool done = false;
    bool cancelled = false;
    bool in_paste = false;
    size_t paste_chars = 0;  /* bytes received during current bracketed paste */
    int paste_lines = 0;     /* newlines received during current bracketed paste */
    char paste_match_buf[8]; /* for matching \e[201~ */
    (void)paste_match_buf;

    bool was_locked = false;
    while (!done) {
        /* Cheap, non-blocking check first: if presence has engaged the lock
         * overlay, do not race it for the user's keystrokes. Block here until
         * Touch ID clears the lock, then force a full composer redraw because
         * the lock screen wiped the alt-screen buffer behind us. */
        if (presence_is_locked()) {
            was_locked = true;
            while (presence_is_locked()) {
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            /* Drop any keystrokes the user typed at the lock screen before
             * touching the sensor — they aren't input for this prompt. */
            tcflush(STDIN_FILENO, TCIFLUSH);
        }
        if (was_locked) {
            was_locked = false;
            goto redraw;
        }

        unsigned char c;
        /* Use a 200 ms select so we wake periodically to re-check the lock
         * state — keeps perceived input latency invisible while still pausing
         * promptly when the screen locks under us. */
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(STDIN_FILENO, &rfd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
        int sr = select(STDIN_FILENO + 1, &rfd, NULL, NULL, &tv);
        if (sr == 0)
            continue; /* timeout — re-check presence_is_locked */
        if (sr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0) {
            cancelled = true;
            break;
        }

        /* Bracketed paste: bytes are queued raw until "\e[201~" */
        if (in_paste) {
            if (c == '\033') {
                /* Look ahead for "[201~" */
                unsigned char seq[5] = {0};
                ssize_t need = 5;
                ssize_t got = 0;
                while (got < need) {
                    ssize_t m = read(STDIN_FILENO, seq + got, 1);
                    if (m <= 0)
                        break;
                    got++;
                }
                if (got == 5 && memcmp(seq, "[201~", 5) == 0) {
                    in_paste = false;
                    /* Notify on non-trivial pastes */
                    if (paste_chars > 50) {
                        char pnote[128];
                        if (paste_lines > 0)
                            snprintf(pnote, sizeof(pnote),
                                     "\xf0\x9f\x93\x8b pasted %zu chars \xc2\xb7 %d line%s",
                                     paste_chars, paste_lines, paste_lines == 1 ? "" : "s");
                        else
                            snprintf(pnote, sizeof(pnote), "\xf0\x9f\x93\x8b pasted %zu chars",
                                     paste_chars);
                        tui_panel_notify(sb, TUI_PANEL_NOTE_INFO, pnote);
                    }
                    paste_chars = 0;
                    paste_lines = 0;
                    goto redraw;
                }
                /* Not the end-paste marker — insert raw */
                composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, "\033", 1);
                for (ssize_t k = 0; k < got; k++) {
                    if (seq[k] == '\r')
                        seq[k] = '\n';
                    composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, (char *)&seq[k], 1);
                }
                goto redraw;
            }
            if (c == '\r')
                c = '\n';
            if (c == '\n')
                paste_lines++;
            paste_chars++;
            composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, (char *)&c, 1);
            /* Live indicator every 500 bytes for large pastes */
            if (paste_chars > 0 && paste_chars % 500 == 0) {
                char pnote[128];
                snprintf(pnote, sizeof(pnote), "\xf0\x9f\x93\x8b pasting\xe2\x80\xa6 %zu chars",
                         paste_chars);
                tui_panel_notify(sb, TUI_PANEL_NOTE_ACTIVITY, pnote);
            }
            goto redraw;
        }

        /* Control keys */
        if (c == 0x03) { /* Ctrl+C */
            cancelled = true;
            break;
        }
        if (c == 0x04) { /* Ctrl+D */
            if (len == 0) {
                cancelled = true;
                break;
            }
            composer_delete(buf, &len, &cur);
            goto redraw;
        }
        if (c == 0x01) {
            composer_home(buf, &cur);
            goto redraw;
        } /* Ctrl+A */
        if (c == 0x05) {
            composer_end(buf, len, &cur);
            goto redraw;
        } /* Ctrl+E */
        if (c == 0x17) {
            composer_kill_word(buf, &len, &cur);
            goto redraw;
        } /* Ctrl+W */
        if (c == 0x15) { /* Ctrl+U: kill to start of line */
            size_t ls = cur;
            while (ls > 0 && buf[ls - 1] != '\n')
                ls--;
            size_t removed = cur - ls;
            memmove(buf + ls, buf + cur, len - cur);
            len -= removed;
            cur = ls;
            buf[len] = '\0';
            goto redraw;
        }
        if (c == 0x0C) { /* Ctrl+L clear screen redraw */
            fprintf(stderr, "\033[2J\033[H");
            goto redraw;
        }
        if (c == '\r' || c == '\n') {
            /* @-image-picker: Enter inserts selected path (keeps picker open
             * so user can append another @ for multi-image).             */
            if (imgpick_open && imgpick_count > 0) {
                const char *sel_path = s_imgpick_paths[imgpick_sel];
                size_t p2 = cur;
                while (p2 > 0 && buf[p2-1] != ' ' && buf[p2-1] != '\n') p2--;
                memmove(buf + p2, buf + cur, len - cur + 1);
                len -= (cur - p2); cur = p2;
                char ins2[4098];
                snprintf(ins2, sizeof(ins2), "%s ", sel_path);
                composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, ins2, strlen(ins2));
                imgpick_open = false; imgpick_count = 0; imgpick_sel = 0;
                goto redraw;
            }
            /* Enter with the dropdown open → complete the highlighted command,
             * then submit it. (Use Tab if you need to add arguments first.)
             * Require at least one char after "/" so a bare "/" + Enter never
             * silently fires the first command (e.g. /clear). */
            if (sug_count > 0 && len > 1) {
                const char *name = s_slash_cmds[sug_idx[sug_sel]].name;
                size_t nl = strlen(name);
                if (nl < TUI_COMPOSER_BUF_CAP) {
                    memcpy(buf, name, nl);
                    buf[nl] = '\0';
                    len = nl;
                    cur = nl;
                }
            }
            done = true;
            break;
        }
        if (c == 0x0A) {
            /* Ctrl+J → literal newline in buffer */
            composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, "\n", 1);
            sug_sel = 0;
            sug_suppress = false;
            goto redraw;
        }
        if (c == 0x7F || c == 0x08) { /* Backspace */
            composer_backspace(buf, &len, &cur);
            sug_sel = 0;
            sug_suppress = false;
            imgpick_sel = 0; /* reset picker highlight on token change */
            goto redraw;
        }
        if (c == 0x09) { /* Tab */
            /* @-image-picker: Tab inserts selected path */
            if (imgpick_open && imgpick_count > 0) {
                const char *sel_path = s_imgpick_paths[imgpick_sel];
                size_t p = cur;
                while (p > 0 && buf[p-1] != ' ' && buf[p-1] != '\n') p--;
                memmove(buf + p, buf + cur, len - cur + 1);
                len -= (cur - p); cur = p;
                char ins[4098];
                snprintf(ins, sizeof(ins), "%s ", sel_path);
                composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, ins, strlen(ins));
                imgpick_open = false; imgpick_count = 0; imgpick_sel = 0;
                goto redraw;
            }
            /* With the dropdown open, Tab completes the highlighted command into
             * the buffer and dismisses the menu so you can type arguments. */
            if (sug_count > 0) {
                const char *name = s_slash_cmds[sug_idx[sug_sel]].name;
                size_t nl = strlen(name);
                if (nl < TUI_COMPOSER_BUF_CAP) {
                    memcpy(buf, name, nl);
                    buf[nl] = '\0';
                    len = nl;
                    cur = nl;
                }
                sug_suppress = true;
                goto redraw;
            }
            composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, "  ", 2);
            goto redraw;
        }

        /* ESC sequences: \e[... or Alt+key */
        if (c == 0x1B) {
            unsigned char n1 = 0;
            if (composer_read_byte(STDIN_FILENO, 30, &n1) <= 0) {
                /* Standalone ESC → dismiss pickers, else cancel. */
                if (imgpick_open) {
                    imgpick_open = false; imgpick_count = 0; imgpick_sel = 0;
                    goto redraw;
                }
                if (sug_count > 0) {
                    sug_suppress = true;
                    goto redraw;
                }
                cancelled = true;
                break;
            }
            if (n1 == '\r' || n1 == '\n') {
                /* Alt/Option+Enter → newline */
                composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, "\n", 1);
                goto redraw;
            }
            if (n1 == 0x7F || n1 == 0x08) {
                composer_kill_word(buf, &len, &cur);
                goto redraw;
            }
            if (n1 != '[' && n1 != 'O') {
                /* Alt+I / Alt+i — paste image from system clipboard */
                if (n1 == 'i' || n1 == 'I') {
                    char img_path[512] = {0};
                    if (composer_clipboard_grab_image(img_path, sizeof(img_path))) {
                        /* Insert the path — agent.c's extract_image_path picks it up */
                        if (len > 0 && buf[len - 1] != ' ' && buf[len - 1] != '\n')
                            composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, " ", 1);
                        composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, img_path,
                                        strlen(img_path));
                        const char *fname = strrchr(img_path, '/');
                        char inote[256];
                        snprintf(inote, sizeof(inote), "\xf0\x9f\x96\xbc  image attached: %s",
                                 fname ? fname + 1 : img_path);
                        tui_panel_notify(sb, TUI_PANEL_NOTE_OK, inote);
                    } else {
                        tui_panel_notify(sb, TUI_PANEL_NOTE_WARN,
                                         "no image in clipboard — copy an image first");
                    }
                }
                /* Unknown Alt+key — ignore */
                goto redraw;
            }
            unsigned char n2 = 0;
            if (composer_read_byte(STDIN_FILENO, 30, &n2) <= 0)
                goto redraw;
            if (n2 == '2') {
                unsigned char n3 = 0, n4 = 0;
                if (composer_read_byte(STDIN_FILENO, 30, &n3) > 0 &&
                    composer_read_byte(STDIN_FILENO, 30, &n4) > 0) {
                    if (n3 == '0' && n4 == '0') {
                        /* Start bracketed paste: consume trailing '~' */
                        unsigned char tilde = 0;
                        composer_read_byte(STDIN_FILENO, 30, &tilde);
                        in_paste = true;
                        paste_chars = 0;
                        paste_lines = 0;
                        goto redraw;
                    }
                }
                goto redraw;
            }
            /* When the dropdown is open, ↑/↓ move the highlight rather than the
             * text cursor (wrapping at the ends, like Claude Code / Codex). */
            if (imgpick_open && imgpick_count > 0 && (n2 == 'A' || n2 == 'B')) {
                if (n2 == 'A')
                    imgpick_sel = (imgpick_sel - 1 + imgpick_count) % imgpick_count;
                else
                    imgpick_sel = (imgpick_sel + 1) % imgpick_count;
                goto redraw;
            }
            if (sug_count > 0 && (n2 == 'A' || n2 == 'B')) {
                if (n2 == 'A')
                    sug_sel = (sug_sel - 1 + sug_count) % sug_count;
                else
                    sug_sel = (sug_sel + 1) % sug_count;
                goto redraw;
            }
            switch (n2) {
                case 'A': /* Up — move cursor to previous line */
                    if (cur > 0) {
                        /* Find start of current line */
                        size_t ls = cur;
                        while (ls > 0 && buf[ls - 1] != '\n')
                            ls--;
                        if (ls > 0) {
                            int col_in = composer_cells(buf + ls, cur - ls);
                            /* Jump to previous line end */
                            size_t prev_end = ls - 1; /* at '\n' */
                            size_t prev_start = prev_end;
                            while (prev_start > 0 && buf[prev_start - 1] != '\n')
                                prev_start--;
                            /* Advance col_in cells into prev line */
                            size_t i = prev_start;
                            int k = 0;
                            while (i < prev_end && k < col_in) {
                                unsigned char cc = (unsigned char)buf[i];
                                if (cc < 0x80)
                                    i += 1;
                                else if ((cc & 0xE0) == 0xC0)
                                    i += 2;
                                else if ((cc & 0xF0) == 0xE0)
                                    i += 3;
                                else if ((cc & 0xF8) == 0xF0)
                                    i += 4;
                                else
                                    i += 1;
                                k++;
                            }
                            cur = i;
                        }
                    }
                    goto redraw;
                case 'B': /* Down */
                    if (cur < len) {
                        /* Find end of current line */
                        size_t le = cur;
                        while (le < len && buf[le] != '\n')
                            le++;
                        if (le < len) {
                            size_t ls = cur;
                            while (ls > 0 && buf[ls - 1] != '\n')
                                ls--;
                            int col_in = composer_cells(buf + ls, cur - ls);
                            size_t next_start = le + 1;
                            size_t next_end = next_start;
                            while (next_end < len && buf[next_end] != '\n')
                                next_end++;
                            size_t i = next_start;
                            int k = 0;
                            while (i < next_end && k < col_in) {
                                unsigned char cc = (unsigned char)buf[i];
                                if (cc < 0x80)
                                    i += 1;
                                else if ((cc & 0xE0) == 0xC0)
                                    i += 2;
                                else if ((cc & 0xF0) == 0xE0)
                                    i += 3;
                                else if ((cc & 0xF8) == 0xF0)
                                    i += 4;
                                else
                                    i += 1;
                                k++;
                            }
                            cur = i;
                        }
                    }
                    goto redraw;
                case 'C':
                    composer_cur_right(buf, len, &cur);
                    goto redraw; /* Right */
                case 'D':
                    composer_cur_left(buf, &cur);
                    goto redraw; /* Left */
                case 'H':
                    composer_home(buf, &cur);
                    goto redraw; /* Home */
                case 'F':
                    composer_end(buf, len, &cur);
                    goto redraw; /* End */
                case '3': {      /* Delete: \e[3~ */
                    unsigned char tilde = 0;
                    composer_read_byte(STDIN_FILENO, 30, &tilde);
                    composer_delete(buf, &len, &cur);
                    goto redraw;
                }
                default:
                    goto redraw;
            }
        }

        /* Printable byte or UTF-8 start */
        if (c >= 0x20 || c >= 0x80) {
            /* Collect continuation bytes for UTF-8 */
            unsigned char utf[4];
            utf[0] = c;
            int want = 0;
            if ((c & 0x80) == 0)
                want = 0;
            else if ((c & 0xE0) == 0xC0)
                want = 1;
            else if ((c & 0xF0) == 0xE0)
                want = 2;
            else if ((c & 0xF8) == 0xF0)
                want = 3;
            for (int k = 0; k < want; k++) {
                if (read(STDIN_FILENO, &utf[1 + k], 1) != 1)
                    break;
            }
            composer_insert(buf, TUI_COMPOSER_BUF_CAP, &len, &cur, (char *)utf, (size_t)(1 + want));
            /* Editing the token resets the highlight and revives a menu that
             * was dismissed with Esc/Tab. */
            sug_sel = 0;
            sug_suppress = false;
            goto redraw;
        }
        continue;

    redraw:
        /* Repaint the inline box. Recompute height — may have changed due to
         * newlines / deletions / resize. Scroll the terminal if growth would
         * push the bottom of the box past the last row. */
        {
            int nrows = tui_term_height();
            /* Refresh the dropdown match set for the current token, then size
             * the repaint to include however many rows it will occupy. */
            sug_count = 0;
            if (!sug_suppress)
                composer_slash_match(buf, len, sug_idx, &sug_count, TUI_SLASHMENU_CAP);
            if (sug_sel >= sug_count)
                sug_sel = sug_count > 0 ? sug_count - 1 : 0;
            if (sug_sel < 0)
                sug_sel = 0;
            /* Refresh @-image-picker */
            { char ip_dir[4096], ip_pfx[256];
              imgpick_open = imgpick_parse_at(buf, cur, ip_dir, sizeof(ip_dir), ip_pfx, sizeof(ip_pfx));
              if (imgpick_open) {
                  imgpick_scan(ip_dir, ip_pfx);
                  imgpick_count = s_imgpick_count;
              } else { imgpick_count = 0; }
              if (imgpick_sel >= imgpick_count)
                  imgpick_sel = imgpick_count > 0 ? imgpick_count - 1 : 0;
              if (imgpick_sel < 0) imgpick_sel = 0; }
            int need = inbox_total_rows(buf, len) + slashmenu_rows(sug_count)
                       + (imgpick_count < TUI_IMGPICK_MAX ? imgpick_count : TUI_IMGPICK_MAX);
            if (r_top + need - 1 > nrows) {
                int overflow = (r_top + need - 1) - nrows;
                tui_term_lock();
                fprintf(stderr, "\033[?25l");
                tui_cursor_move(nrows, 1);
                for (int i = 0; i < overflow; i++)
                    fputc('\n', stderr);
                fflush(stderr);
                tui_term_unlock();
                r_top -= overflow;
                if (r_top < 1)
                    r_top = 1;
            }
            if (nrows != rows)
                rows = nrows;

            tui_term_lock();
            fprintf(stderr, "\033[?25l");
            /* Clear any rows from the previous render that the new one won't
             * overwrite (shrinking buffer). */
            if (prev_height > need) {
                for (int i = need; i < prev_height; i++) {
                    tui_cursor_move(r_top + i, 1);
                    fprintf(stderr, "\033[2K");
                }
            }
            int cr = r_top + 1, cc = 5;
            prev_height =
                composer_paint(r_top, buf, len, cur, sug_idx, sug_count, sug_sel, &cr, &cc, imgpick_count, imgpick_sel);
            tui_cursor_move(cr, cc);
            fprintf(stderr, "\033[?25h");
            fflush(stderr);
            tui_term_unlock();
        }
    }

    /* Cleanup — disable bracketed paste, restore termios */
    fprintf(stderr, "\033[?2004l");
    fflush(stderr);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);

    /* Panel is no longer reading keystrokes — caret returns to idle grey
     * on the next repaint. */
    tui_panel_set_active(sb, false);

    /* Erase the inline box so the echoed input + streamed text can flow
     * naturally below. Cursor lands at row r_top, col 1 — the next text
     * emit overwrites where the box used to be. */
    tui_term_lock();
    fprintf(stderr, "\033[?25l");
    inbox_clear(r_top, prev_height);
    tui_cursor_move(r_top, 1);
    fprintf(stderr, "\033[?25h");
    fflush(stderr);
    tui_term_unlock();

    if (cancelled) {
        free(buf);
        if (len == 0) {
            /* Treat Ctrl+C / Ctrl+D-on-empty as EOF */
            return NULL;
        }
        /* Return empty string to let caller skip this cycle */
        out[0] = '\0';
        return out;
    }

    /* Echo the submitted input into scrollback. Truecolor gradient on the
     * chevron, bright text, and a dim continuation glyph for multi-line input.
     * Built into one stack buffer and emitted with a single fwrite — keeps the
     * commit-to-scroll transition crisp and tear-free even on slow PTYs. */
    if (len > 0) {
        bool truecolor = tui_detect_color_level() >= TUI_COLOR_TRUECOLOR;
        char outbuf[TUI_COMPOSER_BUF_CAP + 1024];
        int op = 0;

        const char *opener = truecolor ? "\033[38;2;255;110;199m❯\033[0m \033[38;2;245;245;245m"
                                       : "\033[1;38;5;213m❯\033[0m \033[1;37m";
        const char *cont = truecolor ? "\033[0m\033[38;2;90;90;90m│\033[0m \033[38;2;235;235;235m"
                                     : "\033[2;37m│\033[0m \033[37m";
        const char *reset_nl = "\033[0m\n";
        int ol = (int)strlen(opener);
        int cl = (int)strlen(cont);
        int rl = (int)strlen(reset_nl);

        if (op + ol < (int)sizeof(outbuf)) {
            memcpy(outbuf + op, opener, (size_t)ol);
            op += ol;
        }
        for (size_t i = 0; i < (size_t)len; i++) {
            if (buf[i] == '\n') {
                if (op + rl < (int)sizeof(outbuf)) {
                    memcpy(outbuf + op, reset_nl, (size_t)rl);
                    op += rl;
                }
                if (i + 1 < (size_t)len && op + cl < (int)sizeof(outbuf)) {
                    memcpy(outbuf + op, cont, (size_t)cl);
                    op += cl;
                }
            } else if (op + 1 < (int)sizeof(outbuf)) {
                outbuf[op++] = buf[i];
            }
        }
        if (op + rl < (int)sizeof(outbuf)) {
            memcpy(outbuf + op, reset_nl, (size_t)rl);
            op += rl;
        }

        fwrite(outbuf, 1, (size_t)op, stderr);
        fflush(stderr);
    }

    /* Copy into output buffer */
    size_t copy_n = len < out_sz - 1 ? len : out_sz - 1;
    memcpy(out, buf, copy_n);
    out[copy_n] = '\0';
    free(buf);

    return out;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 40 Strategic UI Features — Implementation
 * ══════════════════════════════════════════════════════════════════════════ */

#include "config.h"
#include <ctype.h>
#include <termios.h>

/* Global feature flags pointer */
tui_features_t *g_tui_features = NULL;

static double tui_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── F11: Retry Pulse ─────────────────────────────────────────────────── */

void tui_retry_pulse(const char *label, int attempt, int max, double wait_sec) {
    if (g_tui_features && !g_tui_features->retry_pulse)
        return;
    const char *frames[] = {"◉◎○◎◉", "◎◉◎○◎", "○◎◉◎○", "◎○◎◉◎", "◉◎○◎◉"};
    int nframes = 5;
    double end = tui_now() + wait_sec;
    int frame = 0;
    tui_cursor_hide();
    while (tui_now() < end) {
        double remaining = end - tui_now();
        if (remaining < 0)
            remaining = 0;
        tui_clear_line();
        fprintf(stderr, "  %s%s%s %s%s%s retry %d/%d %.1fs", TUI_YELLOW, frames[frame % nframes],
                TUI_RESET, TUI_DIM, label ? label : "", TUI_RESET, attempt, max, remaining);
        fflush(stderr);
        usleep(150000);
        frame++;
    }
    tui_clear_line();
    tui_cursor_show();
    fflush(stderr);
}

/* ── F12: Result Sparkline ────────────────────────────────────────────── */

/* ── Subpixel rendering primitives ──────────────────────────────────────── */

/* Left-anchored eighth blocks: index 1..7 = 1/8..7/8 of a cell filled from
 * the left; 0 = empty, 8 = full handled by caller via "█". */
static const char *const k_eighths_left[8] = {
    "", "▏", "▎", "▍", "▌", "▋", "▊", "▉",
};

int tui_subpixel_hbar(FILE *out, double frac, int cells, const char *fill_color,
                      const char *empty_glyph, const char *empty_color) {
    if (!out || cells <= 0)
        return 0;
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    const char *eg = empty_glyph ? empty_glyph : " ";

    double exact = frac * cells;
    int full = (int)exact;
    int eighth = (int)((exact - full) * 8.0 + 0.5); /* 0..8, rounded */
    if (eighth >= 8) {
        full++;
        eighth = 0;
    }
    if (full > cells) {
        full = cells;
        eighth = 0;
    }

    if (fill_color)
        fputs(fill_color, out);
    int used = 0;
    for (; used < full; used++)
        fputs("█", out);
    if (used < cells && eighth > 0) {
        fputs(k_eighths_left[eighth], out);
        used++;
    }
    if (used < cells) {
        if (empty_color)
            fputs(empty_color, out);
        for (; used < cells; used++)
            fputs(eg, out);
    }
    fputs(TUI_RESET, out);
    return cells;
}

/* Braille dot bit for sub-cell coord (col 0..1, row 0..3). Standard Unicode
 * braille layout: dots 1-2-3-7 down the left column, 4-5-6-8 down the right. */
static const unsigned char k_braille_bit[4][2] = {
    {0x01, 0x08}, /* row 0 (top)    */
    {0x02, 0x10}, /* row 1          */
    {0x04, 0x20}, /* row 2          */
    {0x40, 0x80}, /* row 3 (bottom) */
};

void tui_braille_init(tui_braille_t *b, int px_w, int px_h) {
    if (!b)
        return;
    if (px_w < 1)
        px_w = 1;
    if (px_h < 1)
        px_h = 1;
    b->px_w = px_w;
    b->px_h = px_h;
    b->w_cells = (px_w + 1) / 2;
    b->h_cells = (px_h + 3) / 4;
    b->cells = calloc((size_t)b->w_cells * b->h_cells, 1);
}

void tui_braille_free(tui_braille_t *b) {
    if (!b || !b->cells)
        return;
    free(b->cells);
    b->cells = NULL;
}

void tui_braille_clear(tui_braille_t *b) {
    if (b && b->cells)
        memset(b->cells, 0, (size_t)b->w_cells * b->h_cells);
}

void tui_braille_set(tui_braille_t *b, int x, int y) {
    if (!b || !b->cells)
        return;
    if (x < 0 || y < 0 || x >= b->px_w || y >= b->px_h)
        return;
    int cx = x / 2, cy = y / 4;
    b->cells[cy * b->w_cells + cx] |= k_braille_bit[y % 4][x % 2];
}

void tui_braille_line(tui_braille_t *b, int x0, int y0, int x1, int y1) {
    if (!b)
        return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        tui_braille_set(b, x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void tui_braille_plot(tui_braille_t *b, const double *values, int n) {
    if (!b || !values || n <= 0 || b->px_w <= 0 || b->px_h <= 0)
        return;
    double mn = values[0], mx = values[0];
    for (int i = 1; i < n; i++) {
        if (values[i] < mn)
            mn = values[i];
        if (values[i] > mx)
            mx = values[i];
    }
    double range = mx - mn;
    if (range < 1e-9)
        range = 1.0;

    int prev_x = 0, prev_y = 0;
    for (int i = 0; i < n; i++) {
        int x = (n == 1) ? 0 : (int)((double)i / (n - 1) * (b->px_w - 1) + 0.5);
        double norm = (values[i] - mn) / range;            /* 0..1 */
        int y = (int)((1.0 - norm) * (b->px_h - 1) + 0.5); /* invert: high=top */
        if (i == 0)
            tui_braille_set(b, x, y);
        else
            tui_braille_line(b, prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }
}

void tui_braille_render(const tui_braille_t *b, FILE *out, const char *color) {
    if (!b || !b->cells || !out)
        return;
    if (color)
        fputs(color, out);
    for (int cy = 0; cy < b->h_cells; cy++) {
        if (cy > 0)
            fputc('\n', out);
        for (int cx = 0; cx < b->w_cells; cx++) {
            unsigned int cp = 0x2800u + b->cells[cy * b->w_cells + cx];
            char u[4] = {
                (char)(0xE0 | (cp >> 12)),
                (char)(0x80 | ((cp >> 6) & 0x3F)),
                (char)(0x80 | (cp & 0x3F)),
                0,
            };
            fputs(u, out);
        }
    }
    if (color)
        fputs(TUI_RESET, out);
}

void tui_sparkline_braille(const double *values, int count, int rows, const char *color) {
    if (!values || count <= 0)
        return;
    if (rows <= 0)
        rows = 1;
    /* Braille is BMP Unicode; degrade to the eighth-block sparkline if the
     * terminal can't render it. */
    if (tui_detect_glyph_tier() == TUI_GLYPH_ASCII) {
        tui_sparkline(values, count, color);
        return;
    }
    /* 2 samples per character column → px width = ceil(count/?)*… keep it
     * compact: one column per sample gives the densest readable line. */
    int px_w = count * 2; /* 2 dot-columns per cell, 1 sample/dot-col */
    if (px_w < 2)
        px_w = 2;
    int px_h = rows * 4;
    tui_braille_t b;
    tui_braille_init(&b, px_w, px_h);
    tui_braille_plot(&b, values, count);
    tui_braille_render(&b, stderr, color);
    tui_braille_free(&b);
}

void tui_sparkline(const double *values, int count, const char *color) {
    if (g_tui_features && !g_tui_features->result_sparkline)
        return;
    if (!values || count <= 0)
        return;
    const char *bars = "▁▂▃▄▅▆▇█";
    /* Each bar char is 3 bytes UTF-8 */
    double mn = values[0], mx = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < mn)
            mn = values[i];
        if (values[i] > mx)
            mx = values[i];
    }
    double range = mx - mn;
    if (range < 1e-9)
        range = 1.0;
    if (color)
        fprintf(stderr, "%s", color);
    for (int i = 0; i < count; i++) {
        int idx = (int)((values[i] - mn) / range * 7.0);
        if (idx < 0)
            idx = 0;
        if (idx > 7)
            idx = 7;
        /* Write the UTF-8 bar character (3 bytes each) */
        fwrite(bars + idx * 3, 1, 3, stderr);
    }
    if (color)
        fprintf(stderr, "%s", TUI_RESET);
}

bool tui_try_sparkline(const char *text) {
    if (!text)
        return false;
    /* Try to detect comma-separated numbers */
    const char *p = text;
    double values[128];
    int count = 0;
    while (*p && count < 128) {
        while (*p == ' ' || *p == ',' || *p == '[' || *p == ']')
            p++;
        if (!*p)
            break;
        char *end;
        double v = strtod(p, &end);
        if (end == p)
            break;
        values[count++] = v;
        p = end;
    }
    if (count >= 3) {
        tui_sparkline(values, count, TUI_CYAN);
        return true;
    }
    return false;
}

/* ── F14: Cached Badge ────────────────────────────────────────────────── */

void tui_cached_badge(const char *tool_name) {
    if (g_tui_features && !g_tui_features->cached_badge) {
        fprintf(stderr, "    %s⚡ cached%s\n", TUI_DIM, TUI_RESET);
        return;
    }
    fprintf(stderr, "    ");
    tui_badge("CACHED", TUI_BLACK, TUI_BG_GREEN);
    fprintf(stderr, " %s%s%s\n", TUI_DIM, tool_name ? tool_name : "", TUI_RESET);
}

/* ── F15: Context Pressure Gauge ──────────────────────────────────────── */

void tui_context_gauge(int used, int max_tok, int width) {
    if (g_tui_features && !g_tui_features->context_gauge)
        return;
    if (max_tok <= 0)
        return;
    if (width <= 0)
        width = 30;
    double pct = (double)used / max_tok;

    const char *color;
    if (pct < 0.5)
        color = TUI_GREEN;
    else if (pct < 0.75)
        color = TUI_YELLOW;
    else if (pct < 0.90)
        color = TUI_BYELLOW;
    else
        color = TUI_RED;

    fprintf(stderr, "  [");
    /* Subpixel edge: 1/8-cell precision so small context deltas are visible. */
    tui_subpixel_hbar(stderr, pct, width, color, "░", TUI_DIM);
    fprintf(stderr, " %s%.0f%%%s]", color, pct * 100, TUI_RESET);
    if (used >= 1000)
        fprintf(stderr, " %s%.1fk/%.0fk%s", TUI_DIM, used / 1000.0, max_tok / 1000.0, TUI_RESET);
    else
        fprintf(stderr, " %s%d/%d%s", TUI_DIM, used, max_tok, TUI_RESET);
    fprintf(stderr, "\n");
}

/* ── F17: Auto-Compact Notification ───────────────────────────────────── */

void tui_compact_flash(int before, int after) {
    if (g_tui_features && !g_tui_features->compact_flash)
        return;
    fprintf(stderr, "  %s⟳ compacted %d→%d messages%s\n", TUI_DIM, before, after, TUI_RESET);
}

/* ── F22: Prompt Token Counter ────────────────────────────────────────── */

int tui_estimate_tokens(const char *text) {
    if (!text)
        return 0;
    return ((int)strlen(text) + 3) / 4;
}

void tui_prompt_token_display(int est, int remaining) {
    if (g_tui_features && !g_tui_features->prompt_tokens)
        return;
    const char *color = (remaining > 0 && est > remaining * 0.8) ? TUI_YELLOW : TUI_DIM;
    fprintf(stderr, "  %s~%d tokens%s", color, est, TUI_RESET);
    if (remaining > 0)
        fprintf(stderr, " %s(%d remaining)%s", TUI_DIM, remaining, TUI_RESET);
    fprintf(stderr, "\n");
}

/* ── F26: IPC Message Line ────────────────────────────────────────────── */

void tui_ipc_message_line(const char *from, const char *to, const char *topic,
                          const char *preview) {
    if (g_tui_features && !g_tui_features->ipc_message_line)
        return;
    fprintf(stderr, "  %s%s → %s [%s]%s", TUI_DIM, from ? from : "?", to ? to : "?",
            topic ? topic : "", TUI_RESET);
    if (preview && preview[0]) {
        char trunc[60];
        snprintf(trunc, sizeof(trunc), "%.56s", preview);
        fprintf(stderr, " %s\"%s\"%s", TUI_DIM, trunc, TUI_RESET);
    }
    fprintf(stderr, "\n");
}

/* ── F27: Agent Progress Roll-up ──────────────────────────────────────── */

void tui_agent_rollup(int total, int done, int running, int errored) {
    if (g_tui_features && !g_tui_features->agent_rollup)
        return;
    fprintf(stderr, "  %sagents: %s%d done%s", TUI_DIM, TUI_GREEN, done, TUI_RESET);
    if (running > 0)
        fprintf(stderr, " %s%d running%s", TUI_CYAN, running, TUI_RESET);
    if (errored > 0)
        fprintf(stderr, " %s%d errors%s", TUI_RED, errored, TUI_RESET);
    fprintf(stderr, " %s(%d total)%s\n", TUI_DIM, total, TUI_RESET);
}

/* ── F29: Adaptive Color Theme ────────────────────────────────────────── */

static tui_theme_t s_current_theme = TUI_THEME_DARK;

tui_theme_t tui_detect_theme(void) {
    /* Check COLORFGBG env var (format: "fg;bg") */
    const char *cfg = getenv("COLORFGBG");
    if (cfg) {
        const char *semi = strrchr(cfg, ';');
        if (semi) {
            int bg = atoi(semi + 1);
            /* bg > 8 typically means light background */
            if (bg > 8 || bg == 7)
                return TUI_THEME_LIGHT;
        }
    }
    /* Check TERM_BACKGROUND */
    const char *tb = getenv("TERM_BACKGROUND");
    if (tb && strcmp(tb, "light") == 0)
        return TUI_THEME_LIGHT;
    return TUI_THEME_DARK;
}

void tui_apply_theme(tui_theme_t theme) {
    s_current_theme = theme;
}

const char *tui_theme_dim(void) {
    return s_current_theme == TUI_THEME_LIGHT ? "\033[38;5;240m" : TUI_DIM;
}

const char *tui_theme_bright(void) {
    return s_current_theme == TUI_THEME_LIGHT ? TUI_BLACK : TUI_BWHITE;
}

const char *tui_theme_accent(void) {
    return s_current_theme == TUI_THEME_LIGHT ? TUI_BLUE : TUI_BCYAN;
}

/* ── F30: Section Dividers with Context ───────────────────────────────── */

void tui_section_divider(int turn, int tools, double cost, const char *model, double tok_per_sec) {
    (void)model;
    if (g_tui_features && !g_tui_features->section_dividers)
        return;
    int w = tui_term_width();
    char info[256];
    if (tok_per_sec > 0) {
        snprintf(info, sizeof(info), " turn %d · %d tool%s · $%.3f · %.0f tok/s ", turn, tools,
                 tools == 1 ? "" : "s", cost, tok_per_sec);
    } else {
        snprintf(info, sizeof(info), " turn %d · %d tool%s · $%.3f ", turn, tools,
                 tools == 1 ? "" : "s", cost);
    }
    int info_len = (int)strlen(info);
    int left = (w - info_len - 2) / 2;
    int right = w - info_len - left - 2;
    if (left < 2)
        left = 2;
    if (right < 2)
        right = 2;

    bool use_gradient = tui_detect_color_level() >= TUI_COLOR_256;
    if (use_gradient) {
        /* Left gradient: purple → cyan */
        for (int i = 0; i < left; i++) {
            float t = (float)i / (float)(left > 1 ? left - 1 : 1);
            float h = 280.0f + t * (190.0f - 280.0f + 360.0f);
            if (h >= 360.0f)
                h -= 360.0f;
            tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.55f);
            fprintf(stderr, "\033[38;2;%d;%d;%dm─", c.r, c.g, c.b);
        }
        fprintf(stderr, TUI_RESET);
        /* Info text with subtle accent */
        {
            tui_rgb_t tc = tui_hsv_to_rgb(210.0f, 0.20f, 0.65f);
            fprintf(stderr, "\033[38;2;%d;%d;%dm%s" TUI_RESET, tc.r, tc.g, tc.b, info);
        }
        /* Right gradient: cyan → purple */
        for (int i = 0; i < right; i++) {
            float t = (float)i / (float)(right > 1 ? right - 1 : 1);
            float h = 190.0f + t * (280.0f - 190.0f);
            tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.55f);
            fprintf(stderr, "\033[38;2;%d;%d;%dm─", c.r, c.g, c.b);
        }
        fprintf(stderr, TUI_RESET "\n");
    } else {
        fprintf(stderr, "%s", TUI_DIM);
        for (int i = 0; i < left; i++)
            fprintf(stderr, "─");
        fprintf(stderr, "%s%s%s%s", TUI_RESET, TUI_DIM, info, TUI_RESET);
        fprintf(stderr, "%s", TUI_DIM);
        for (int i = 0; i < right; i++)
            fprintf(stderr, "─");
        fprintf(stderr, "%s\n", TUI_RESET);
    }
}

/* ── F30 Enhanced: Section Divider with success/fail/cache/context ───── */

void tui_section_divider_ex(int turn, int tools_ok, int tools_fail, int cache_hits, double cost,
                            const char *model, double tok_per_sec, double ctx_pct,
                            const char *git_branch) {
    (void)model;
    if (g_tui_features && !g_tui_features->section_dividers)
        return;
    int w = tui_term_width();
    char info[384];
    int pos = 0;

    pos += snprintf(info + pos, sizeof(info) - pos, " t%d", turn);

    int total_tools = tools_ok + tools_fail;
    if (total_tools > 0) {
        pos += snprintf(info + pos, sizeof(info) - pos, " · %d tool%s", total_tools,
                        total_tools == 1 ? "" : "s");
        if (tools_fail > 0)
            pos += snprintf(info + pos, sizeof(info) - pos, " (%d fail)", tools_fail);
    }

    if (cache_hits > 0)
        pos += snprintf(info + pos, sizeof(info) - pos, " · %d cached", cache_hits);

    pos += snprintf(info + pos, sizeof(info) - pos, " · $%.3f", cost);

    if (tok_per_sec > 0)
        pos += snprintf(info + pos, sizeof(info) - pos, " · %.0f tok/s", tok_per_sec);

    /* Context pressure */
    const char *ctx_indicator = ctx_pct < 60 ? "●" : (ctx_pct < 85 ? "◐" : "◉");
    pos += snprintf(info + pos, sizeof(info) - pos, " · %.0f%% %s", ctx_pct, ctx_indicator);

    if (git_branch && git_branch[0] && g_tui_features && g_tui_features->branch_indicator)
        pos += snprintf(info + pos, sizeof(info) - pos, " · %s", git_branch);

    snprintf(info + pos, sizeof(info) - pos, " ");

    int info_len = (int)strlen(info);
    int left = (w - info_len - 2) / 2;
    int right = w - info_len - left - 2;
    if (left < 2)
        left = 2;
    if (right < 2)
        right = 2;

    bool use_gradient = tui_detect_color_level() >= TUI_COLOR_256;
    if (use_gradient) {
        for (int i = 0; i < left; i++) {
            float t = (float)i / (float)(left > 1 ? left - 1 : 1);
            float h = 280.0f + t * (190.0f - 280.0f + 360.0f);
            if (h >= 360.0f)
                h -= 360.0f;
            tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.55f);
            fprintf(stderr, "\033[38;2;%d;%d;%dm─", c.r, c.g, c.b);
        }
        fprintf(stderr, TUI_RESET);
        tui_rgb_t tc = tui_hsv_to_rgb(210.0f, 0.20f, 0.65f);
        fprintf(stderr, "\033[38;2;%d;%d;%dm%s" TUI_RESET, tc.r, tc.g, tc.b, info);
        for (int i = 0; i < right; i++) {
            float t = (float)i / (float)(right > 1 ? right - 1 : 1);
            float h = 190.0f + t * (280.0f - 190.0f);
            tui_rgb_t c = tui_hsv_to_rgb(h, 0.35f, 0.55f);
            fprintf(stderr, "\033[38;2;%d;%d;%dm─", c.r, c.g, c.b);
        }
        fprintf(stderr, TUI_RESET "\n");
    } else {
        fprintf(stderr, "%s", TUI_DIM);
        for (int i = 0; i < left; i++)
            fprintf(stderr, "─");
        fprintf(stderr, "%s%s%s%s", TUI_RESET, TUI_DIM, info, TUI_RESET);
        fprintf(stderr, "%s", TUI_DIM);
        for (int i = 0; i < right; i++)
            fprintf(stderr, "─");
        fprintf(stderr, "%s\n", TUI_RESET);
    }
}

/* ── F31: Status Bar Clock ────────────────────────────────────────────── */

void tui_status_bar_set_clock(tui_status_bar_t *sb, bool show) {
    pthread_mutex_lock(&sb->mutex);
    sb->show_clock = show;
    pthread_mutex_unlock(&sb->mutex);
}

/* ── F32: Error Severity Levels ───────────────────────────────────────── */

void tui_error_typed(tui_err_type_t type, const char *msg) {
    if (g_tui_features && !g_tui_features->error_severity) {
        tui_error(msg);
        return;
    }
    const tui_glyphs_t *gl = tui_glyph();
    const char *icons[] = {gl->icon_globe, gl->icon_lightning, gl->warn,
                           gl->icon_timer, gl->icon_lock,      gl->icon_money};
    const char *labels[] = {"NETWORK", "API", "VALIDATION", "TIMEOUT", "AUTH", "BUDGET"};
    const char *colors[] = {TUI_BRED, TUI_BMAGENTA, TUI_BYELLOW, TUI_BCYAN, TUI_RED, TUI_YELLOW};
    int idx = (int)type;
    if (idx < 0 || idx > 5)
        idx = 1;
    fprintf(stderr, "  %s %s%s%s%s %s%s%s\n", icons[idx], colors[idx], TUI_BOLD, labels[idx],
            TUI_RESET, msg ? msg : "", TUI_RESET, "");
}

/* ── F34: Notification Bell ───────────────────────────────────────────── */

void tui_notify(const char *title, const char *body) {
    if (g_tui_features && !g_tui_features->notify_bell)
        return;
    /* BEL character */
    fprintf(stderr, "\a");
    /* OSC 9 (iTerm2 notification) */
    if (title)
        fprintf(stderr, "\033]9;%s\007", title);
    /* OSC 777 (generic terminal notification) */
    if (title && body)
        fprintf(stderr, "\033]777;notify;%s;%s\007", title, body);
    fflush(stderr);
}

/* ── F2: Typing Cadence ───────────────────────────────────────────────── */

void tui_cadence_init(tui_cadence_t *c, tui_cadence_flush_cb cb, void *ctx) {
    memset(c, 0, sizeof(*c));
    c->interval = 0.016; /* 16ms */
    c->last_flush = tui_now();
    c->flush_cb = cb;
    c->flush_ctx = ctx;
}

void tui_cadence_feed(tui_cadence_t *c, const char *text) {
    if (!text)
        return;
    int tlen = (int)strlen(text);
    int i = 0;
    while (i < tlen) {
        int room = TUI_CADENCE_BUF_SIZE - 1 - c->len;
        if (room <= 0) {
            tui_cadence_flush(c);
            room = TUI_CADENCE_BUF_SIZE - 1 - c->len;
            if (room <= 0)
                return; /* shouldn't happen, defensive */
        }
        int take = tlen - i;
        if (take > room)
            take = room;
        memcpy(c->buf + c->len, text + i, (size_t)take);
        c->len += take;
        i += take;
    }
    double now = tui_now();
    if (now - c->last_flush >= c->interval || c->len >= TUI_CADENCE_BUF_SIZE - 1) {
        tui_cadence_flush(c);
    }
}

void tui_cadence_flush(tui_cadence_t *c) {
    if (c->len == 0) {
        c->last_flush = tui_now();
        return;
    }
    /* Don't split a multi-byte UTF-8 sequence at the tail — hold the
     * trailing partial char for the next flush so the renderer never
     * sees half a character. */
    int emit = c->len;
    while (emit > 0) {
        unsigned char b = (unsigned char)c->buf[emit - 1];
        if ((b & 0x80) == 0)
            break;                /* ASCII — safe boundary */
        if ((b & 0xC0) == 0xC0) { /* lead byte: back up one */
            int need;
            if ((b & 0xE0) == 0xC0)
                need = 2;
            else if ((b & 0xF0) == 0xE0)
                need = 3;
            else if ((b & 0xF8) == 0xF0)
                need = 4;
            else
                need = 1; /* malformed; drop */
            int have = c->len - (emit - 1);
            if (have >= need)
                break; /* full sequence present */
            emit--;    /* incomplete — hold it */
            break;
        }
        /* continuation byte at tail — walk back until lead */
        emit--;
    }
    if (emit > 0) {
        int rem = c->len - emit;
        char held[TUI_CADENCE_BUF_SIZE];
        if (rem > 0)
            memcpy(held, c->buf + emit, (size_t)rem);
        c->buf[emit] = '\0';
        if (c->flush_cb)
            c->flush_cb(c->buf, emit, c->flush_ctx);
        /* shift remaining bytes (the held partial UTF-8) to the front */
        if (rem > 0)
            memcpy(c->buf, held, (size_t)rem);
        c->len = rem;
        c->buf[c->len] = '\0';
    }
    c->last_flush = tui_now();
}

void tui_cadence_drain(tui_cadence_t *c) {
    if (c->len == 0) {
        c->last_flush = tui_now();
        return;
    }
    c->buf[c->len] = '\0';
    if (c->flush_cb)
        c->flush_cb(c->buf, c->len, c->flush_ctx);
    c->len = 0;
    c->last_flush = tui_now();
}

/* ── F4: Collapsible Thinking ─────────────────────────────────────────── */

void tui_thinking_init(tui_thinking_state_t *t) {
    memset(t, 0, sizeof(*t));
}

void tui_thinking_feed(tui_thinking_state_t *t, const char *text) {
    if (!text)
        return;
    if (!t->active) {
        t->active = true;
        t->start_time = tui_now();
        t->char_count = 0;
        t->summary_len = 0;
        t->summary_done = false;
        t->summary[0] = '\0';
    }
    t->char_count += (int)strlen(text);

    /* Capture first sentence/line for summary display */
    if (!t->summary_done) {
        for (const char *c = text; *c && !t->summary_done; c++) {
            if (*c == '\n' || *c == '.' || *c == '!' || *c == '?') {
                /* Include the punctuation, skip newlines */
                if (*c != '\n' && t->summary_len < TUI_THINKING_SUMMARY_MAX - 1) {
                    t->summary[t->summary_len++] = *c;
                }
                t->summary[t->summary_len] = '\0';
                t->summary_done = true;
            } else if (t->summary_len < TUI_THINKING_SUMMARY_MAX - 4) {
                t->summary[t->summary_len++] = *c;
                t->summary[t->summary_len] = '\0';
            } else {
                /* Truncate with ellipsis */
                t->summary[t->summary_len] = '\0';
                strncat(t->summary, "...", TUI_THINKING_SUMMARY_MAX - t->summary_len - 1);
                t->summary_len = (int)strlen(t->summary);
                t->summary_done = true;
            }
        }
    }
}

void tui_thinking_end(tui_thinking_state_t *t) {
    if (!t->active)
        return;
    double elapsed = tui_now() - t->start_time;
    int est_tokens = (t->char_count + 3) / 4;
    bool truecolor = tui_supports_truecolor();
    const tui_glyphs_t *gl = tui_glyph();
    const char *think_icon = (gl && gl->icon_think) ? gl->icon_think : "?";
    const char *brain_icon = (gl && gl->icon_brain) ? gl->icon_brain : think_icon;
    const char *pl_right = (gl && gl->pl_right) ? gl->pl_right : "";

    if (truecolor && t->summary[0]) {
        /* Gradient thinking badge: brain icon + summary */
        tui_rgb_t badge_bg = tui_hsv_to_rgb(275.0f, 0.35f, 0.30f);
        tui_rgb_t badge_fg = tui_hsv_to_rgb(275.0f, 0.20f, 0.90f);
        tui_rgb_t meta_c = tui_hsv_to_rgb(260.0f, 0.15f, 0.50f);
        fprintf(stderr, "  \033[48;2;%d;%d;%dm\033[38;2;%d;%d;%dm %s thinking \033[0m", badge_bg.r,
                badge_bg.g, badge_bg.b, badge_fg.r, badge_fg.g, badge_fg.b, brain_icon);
        fprintf(stderr, "\033[38;2;%d;%d;%dm%s\033[0m", badge_bg.r, badge_bg.g, badge_bg.b,
                pl_right);
        /* Summary as italic dim */
        fprintf(stderr, " %s%s%s\n", TUI_DIM TUI_ITALIC, t->summary, TUI_RESET);
        /* Token/time metadata on second line */
        fprintf(stderr, "  \033[38;2;%d;%d;%dm  ~%d tokens, %.1fs\033[0m\n", meta_c.r, meta_c.g,
                meta_c.b, est_tokens, elapsed);
    } else if (t->summary[0]) {
        fprintf(stderr, "  %s%s[thinking]%s %s%s%s\n", TUI_DIM, think_icon, TUI_RESET,
                TUI_DIM TUI_ITALIC, t->summary, TUI_RESET);
        fprintf(stderr, "  %s~%d tokens, %.1fs%s\n", TUI_DIM, est_tokens, elapsed, TUI_RESET);
    } else {
        fprintf(stderr, "  %s[thinking: ~%d tokens, %.1fs]%s\n", TUI_DIM, est_tokens, elapsed,
                TUI_RESET);
    }
    t->active = false;
    t->char_count = 0;
}

/* ── F5: Live Word Count ──────────────────────────────────────────────── */

void tui_word_counter_init(tui_word_counter_t *w) {
    memset(w, 0, sizeof(*w));
    w->start_time = tui_now();
}

void tui_word_counter_feed(tui_word_counter_t *w, const char *text) {
    if (!text)
        return;
    for (const char *p = text; *p; p++) {
        w->chars++;
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            /* Check if previous was non-space */
            if (p > text && *(p - 1) != ' ' && *(p - 1) != '\n' && *(p - 1) != '\t')
                w->words++;
        }
    }
}

void tui_word_counter_render(tui_word_counter_t *w) {
    /* Disabled during streaming — cursor jumps cause rendering corruption.
       Word count is still shown at end via tui_word_counter_end(). */
    (void)w;
}

void tui_word_counter_end(tui_word_counter_t *w) {
    /* Final count — rendered inline */
    if (g_tui_features && !g_tui_features->live_word_count)
        return;
    double elapsed = tui_now() - w->start_time;
    fprintf(stderr, "  %s%d words, %d chars (%.1fs)%s\n", TUI_DIM, w->words, w->chars, elapsed,
            TUI_RESET);
}

/* ── F39: Streaming Throughput Graph ──────────────────────────────────── */

void tui_throughput_init(tui_throughput_t *t) {
    memset(t, 0, sizeof(*t));
    t->last_sample_time = tui_now();
}

void tui_throughput_tick(tui_throughput_t *t, int tokens) {
    t->tokens_since_last += tokens;
    double now = tui_now();
    double dt = now - t->last_sample_time;
    if (dt >= 0.25) { /* sample every 250ms */
        double tps = t->tokens_since_last / dt;
        t->samples[t->head % TUI_THROUGHPUT_SAMPLES] = tps;
        t->head++;
        if (t->count < TUI_THROUGHPUT_SAMPLES)
            t->count++;
        t->tokens_since_last = 0;
        t->last_sample_time = now;
    }
}

void tui_throughput_render(tui_throughput_t *t) {
    if (g_tui_features && !g_tui_features->throughput_graph)
        return;
    if (t->count < 2)
        return;
    /* Compute average */
    double sum = 0;
    int start = t->head - t->count;
    if (start < 0)
        start = 0;
    double vals[TUI_THROUGHPUT_SAMPLES];
    int n = 0;
    for (int i = start; i < t->head && n < TUI_THROUGHPUT_SAMPLES; i++) {
        vals[n++] = t->samples[i % TUI_THROUGHPUT_SAMPLES];
        sum += vals[n - 1];
    }
    double avg = n > 0 ? sum / n : 0;
    fprintf(stderr, "  %sthroughput: %s", TUI_DIM, TUI_RESET);
    /* Maximal-subpixel line graph: a 2-row Braille plot gives 8 vertical dots
     * and 2 samples per column — far smoother than 8-level eighth blocks.
     * tui_sparkline_braille degrades to eighth blocks on ASCII terminals. */
    tui_sparkline_braille(vals, n, 1, TUI_BCYAN);
    fprintf(stderr, " %savg %.0f tok/s%s\n", TUI_DIM, avg, TUI_RESET);
}

/* ── F8: Flame Timeline ───────────────────────────────────────────────── */

void tui_flame_init(tui_flame_t *f) {
    memset(f, 0, sizeof(*f));
}

void tui_flame_add(tui_flame_t *f, const char *name, double start_ms, double end_ms, bool ok,
                   tui_tool_type_t type) {
    if (f->count >= TUI_FLAME_MAX)
        return;
    tui_flame_entry_t *e = &f->entries[f->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->start_ms = start_ms;
    e->end_ms = end_ms;
    e->ok = ok;
    e->type = type;
    if (f->count == 1)
        f->epoch_ms = start_ms;
}

void tui_flame_render(tui_flame_t *f) {
    if (g_tui_features && !g_tui_features->flame_timeline)
        return;
    if (f->count < 2)
        return;

    /* Find total span */
    double total_end = 0;
    for (int i = 0; i < f->count; i++)
        if (f->entries[i].end_ms > total_end)
            total_end = f->entries[i].end_ms;
    double total_span = total_end - f->epoch_ms;
    if (total_span <= 0)
        total_span = 1;

    int bar_width = tui_term_width() - 24;
    if (bar_width < 20)
        bar_width = 20;

    fprintf(stderr, "\n  %s%sflame timeline:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < f->count; i++) {
        tui_flame_entry_t *e = &f->entries[i];
        int start_col = (int)((e->start_ms - f->epoch_ms) / total_span * bar_width);
        int end_col = (int)((e->end_ms - f->epoch_ms) / total_span * bar_width);
        if (end_col <= start_col)
            end_col = start_col + 1;
        if (end_col > bar_width)
            end_col = bar_width;

        const char *color = tui_tool_color(e->type);
        char name_trunc[16];
        snprintf(name_trunc, sizeof(name_trunc), "%.14s", e->name);

        fprintf(stderr, "  %s%-14s%s ", TUI_DIM, name_trunc, TUI_RESET);
        for (int c = 0; c < bar_width; c++) {
            if (c >= start_col && c < end_col)
                fprintf(stderr, "%s%s█%s", color, e->ok ? "" : TUI_BOLD, TUI_RESET);
            else
                fprintf(stderr, "%s░%s", TUI_DIM, TUI_RESET);
        }
        fprintf(stderr, " %s%.0fms%s\n", TUI_DIM, e->end_ms - e->start_ms, TUI_RESET);
    }
    fprintf(stderr, "\n");
}

/* ── F10: Tool Dependency Graph ───────────────────────────────────────── */

void tui_dag_init(tui_dag_t *d) {
    memset(d, 0, sizeof(*d));
}

int tui_dag_add_node(tui_dag_t *d, const char *name) {
    /* Check if exists */
    for (int i = 0; i < d->node_count; i++)
        if (strcmp(d->nodes[i], name) == 0)
            return i;
    if (d->node_count >= TUI_DAG_MAX_NODES)
        return -1;
    int idx = d->node_count++;
    snprintf(d->nodes[idx], sizeof(d->nodes[idx]), "%s", name);
    return idx;
}

void tui_dag_add_edge(tui_dag_t *d, int from, int to) {
    if (d->edge_count >= TUI_DAG_MAX_EDGES)
        return;
    /* Deduplicate */
    for (int i = 0; i < d->edge_count; i++)
        if (d->edges[i].from == from && d->edges[i].to == to)
            return;
    d->edges[d->edge_count].from = from;
    d->edges[d->edge_count].to = to;
    d->edge_count++;
}

void tui_dag_render(tui_dag_t *d) {
    if (g_tui_features && !g_tui_features->tool_dep_graph)
        return;
    if (d->node_count < 2 || d->edge_count == 0)
        return;

    fprintf(stderr, "  %s%stool chain:%s ", TUI_BOLD, TUI_DIM, TUI_RESET);

    /* Simple linear chain rendering: follow edges */
    bool printed[TUI_DAG_MAX_NODES];
    memset(printed, 0, sizeof(printed));
    /* Find roots (nodes with no incoming edges) */
    bool has_incoming[TUI_DAG_MAX_NODES];
    memset(has_incoming, 0, sizeof(has_incoming));
    for (int i = 0; i < d->edge_count; i++)
        has_incoming[d->edges[i].to] = true;

    bool first = true;
    for (int n = 0; n < d->node_count; n++) {
        if (!has_incoming[n]) {
            /* Walk chain from this root */
            int cur = n;
            while (cur >= 0 && !printed[cur]) {
                if (!first)
                    fprintf(stderr, " %s→%s ", TUI_DIM, TUI_RESET);
                fprintf(stderr, "%s%s%s", TUI_CYAN, d->nodes[cur], TUI_RESET);
                printed[cur] = true;
                first = false;
                /* Find next */
                int next = -1;
                for (int e = 0; e < d->edge_count; e++)
                    if (d->edges[e].from == cur) {
                        next = d->edges[e].to;
                        break;
                    }
                cur = next;
            }
        }
    }
    /* Print any remaining unprinted nodes */
    for (int n = 0; n < d->node_count; n++) {
        if (!printed[n]) {
            if (!first)
                fprintf(stderr, " %s→%s ", TUI_DIM, TUI_RESET);
            fprintf(stderr, "%s%s%s", TUI_CYAN, d->nodes[n], TUI_RESET);
            first = false;
        }
    }
    fprintf(stderr, "\n");
}

/* ── F13: Tool Cost Annotations ───────────────────────────────────────── */

void tui_tool_cost(const char *name, int in_tok, int out_tok, const char *model) {
    if (g_tui_features && !g_tui_features->tool_cost)
        return;
    (void)name;
    /* Lookup model pricing */
    double in_price = 15.0, out_price = 75.0; /* default: opus */
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        if (model && (strcmp(model, MODEL_REGISTRY[i].model_id) == 0 ||
                      strcmp(model, MODEL_REGISTRY[i].alias) == 0)) {
            in_price = MODEL_REGISTRY[i].input_price;
            out_price = MODEL_REGISTRY[i].output_price;
            break;
        }
    }
    double cost = in_tok * in_price / 1e6 + out_tok * out_price / 1e6;
    if (cost >= 0.0001)
        fprintf(stderr, " %s($%.4f)%s", TUI_DIM, cost, TUI_RESET);
}

/* ── F35: Inline ASCII Charts ─────────────────────────────────────────── */

static void tui_chart_hbar_render(const char **labels, const double *values, int count, int width) {
    double mx = values[0];
    for (int i = 1; i < count; i++)
        if (values[i] > mx)
            mx = values[i];
    if (mx <= 0)
        mx = 1;

    int max_label = 0;
    for (int i = 0; i < count; i++) {
        int l = labels && labels[i] ? tui_str_display_width(labels[i]) : 0;
        if (l > max_label)
            max_label = l;
    }
    if (max_label > 20)
        max_label = 20;

    int bar_max = width - max_label - 12;
    if (bar_max < 10)
        bar_max = 10;

    for (int i = 0; i < count; i++) {
        const char *lbl = labels && labels[i] ? labels[i] : "";
        fprintf(stderr, "  %*s │", max_label, lbl);
        int bar_len = (int)(values[i] / mx * bar_max);
        const char *color = TUI_BCYAN;
        if (values[i] == mx)
            color = TUI_BGREEN;
        for (int b = 0; b < bar_len; b++)
            fprintf(stderr, "%s█%s", color, TUI_RESET);
        fprintf(stderr, " %.1f\n", values[i]);
    }
}

static void tui_chart_vbar_render(const char **labels, const double *values, int count, int width,
                                  int height) {
    if (height <= 0)
        height = 8;
    if (height > 20)
        height = 20;

    int max_cols = width > 12 ? (width - 6) / 2 : count;
    if (max_cols < 1)
        max_cols = 1;
    int cols = count < max_cols ? count : max_cols;

    double mx = values[0];
    for (int i = 1; i < count; i++)
        if (values[i] > mx)
            mx = values[i];
    if (mx <= 0)
        mx = 1;

    int h[256];
    if (cols > (int)(sizeof(h) / sizeof(h[0])))
        cols = (int)(sizeof(h) / sizeof(h[0]));
    for (int c = 0; c < cols; c++) {
        int src = (int)((long long)c * count / cols);
        double v = values[src];
        if (v < 0)
            v = 0;
        h[c] = (int)(v / mx * height + 0.5);
        if (h[c] > height)
            h[c] = height;
    }

    for (int row = height; row >= 1; row--) {
        fprintf(stderr, "  ");
        for (int c = 0; c < cols; c++) {
            if (h[c] >= row)
                fprintf(stderr, "%s█%s ", TUI_BCYAN, TUI_RESET);
            else
                fprintf(stderr, "  ");
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  ");
    for (int c = 0; c < cols; c++)
        fprintf(stderr, "─ ");
    fprintf(stderr, "\n");

    if (labels) {
        fprintf(stderr, "  ");
        for (int c = 0; c < cols; c++) {
            int src = (int)((long long)c * count / cols);
            const char *lbl = labels[src] ? labels[src] : "";
            unsigned int cp = 0;
            int n = tui_utf8_decode(lbl, &cp);
            if (n <= 0 || !*lbl)
                fprintf(stderr, "· ");
            else
                fprintf(stderr, "%.*s ", n, lbl);
        }
        fprintf(stderr, "\n");
    }
}

static void tui_chart_spark_render(const double *values, int count, int width) {
    const char *bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int max_points = width > 14 ? width - 12 : count;
    if (max_points < 4)
        max_points = 4;
    int points = count < max_points ? count : max_points;

    double mn = values[0], mx = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < mn)
            mn = values[i];
        if (values[i] > mx)
            mx = values[i];
    }
    double span = mx - mn;
    if (span < 1e-12)
        span = 1.0;

    fprintf(stderr, "  spark │");
    for (int i = 0; i < points; i++) {
        int src = (int)((long long)i * count / points);
        double v = values[src];
        int idx = (int)((v - mn) / span * 7.0);
        if (idx < 0)
            idx = 0;
        if (idx > 7)
            idx = 7;
        fprintf(stderr, "%s%s%s", TUI_BCYAN, bars[idx], TUI_RESET);
    }
    fprintf(stderr, "│ %.2f..%.2f\n", mn, mx);
}

static void tui_chart_heat_render(const double *values, int count, int width) {
    int max_points = width > 14 ? width - 10 : count;
    if (max_points < 4)
        max_points = 4;
    int points = count < max_points ? count : max_points;

    double mn = values[0], mx = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < mn)
            mn = values[i];
        if (values[i] > mx)
            mx = values[i];
    }
    double span = mx - mn;
    if (span < 1e-12)
        span = 1.0;

    fprintf(stderr, "  heat  │");
    for (int i = 0; i < points; i++) {
        int src = (int)((long long)i * count / points);
        double t = (values[src] - mn) / span;
        const char *color = TUI_BLUE;
        if (t >= 0.80)
            color = TUI_BRED;
        else if (t >= 0.60)
            color = TUI_BYELLOW;
        else if (t >= 0.40)
            color = TUI_BGREEN;
        else if (t >= 0.20)
            color = TUI_BCYAN;
        fprintf(stderr, "%s█%s", color, TUI_RESET);
    }
    fprintf(stderr, "│\n");
}

typedef enum {
    TUI_CHART_FAMILY_HBAR,
    TUI_CHART_FAMILY_VBAR,
    TUI_CHART_FAMILY_SPARK,
    TUI_CHART_FAMILY_HEAT,
} tui_chart_family_t;

static tui_chart_family_t tui_chart_family(tui_chart_type_t type) {
    switch (type) {
        case TUI_CHART_BAR:
        case TUI_CHART_HBAR:
        case TUI_CHART_HBAR_THIN:
        case TUI_CHART_HBAR_BLOCK:
        case TUI_CHART_HBAR_SHADE:
        case TUI_CHART_HBAR_DOT:
        case TUI_CHART_HBAR_TICK:
        case TUI_CHART_HBAR_STEP:
        case TUI_CHART_HBAR_STACKED:
        case TUI_CHART_HBAR_NEGPOS:
        case TUI_CHART_HBAR_DELTA:
        case TUI_CHART_HBAR_CANDLE:
        case TUI_CHART_HBAR_RANGE:
        case TUI_CHART_HBAR_MEDIAN:
        case TUI_CHART_HBAR_PCTL:
        case TUI_CHART_HBAR_LOG:
        case TUI_CHART_HBAR_SQRT:
            return TUI_CHART_FAMILY_HBAR;
        case TUI_CHART_VBAR:
        case TUI_CHART_VBAR_THIN:
        case TUI_CHART_VBAR_BLOCK:
        case TUI_CHART_VBAR_SHADE:
        case TUI_CHART_VBAR_DOT:
        case TUI_CHART_VBAR_TICK:
        case TUI_CHART_VBAR_STEP:
        case TUI_CHART_VBAR_STACKED:
        case TUI_CHART_VBAR_NEGPOS:
        case TUI_CHART_VBAR_DELTA:
        case TUI_CHART_VBAR_CANDLE:
        case TUI_CHART_VBAR_RANGE:
        case TUI_CHART_VBAR_MEDIAN:
        case TUI_CHART_VBAR_PCTL:
        case TUI_CHART_VBAR_LOG:
        case TUI_CHART_VBAR_SQRT:
            return TUI_CHART_FAMILY_VBAR;
        case TUI_CHART_SPARK:
        case TUI_CHART_SPARK_THIN:
        case TUI_CHART_SPARK_DOT:
        case TUI_CHART_SPARK_BLOCK:
        case TUI_CHART_SPARK_SHADE:
        case TUI_CHART_SPARK_STEP:
        case TUI_CHART_SPARK_WAVE:
        case TUI_CHART_SPARK_DENSE:
        case TUI_CHART_SPARK_SMOOTH:
        case TUI_CHART_SPARK_DELTA:
        case TUI_CHART_SPARK_RANGE:
        case TUI_CHART_SPARK_MEDIAN:
        case TUI_CHART_SPARK_PCTL:
        case TUI_CHART_SPARK_LOG:
        case TUI_CHART_SPARK_SQRT:
        case TUI_CHART_SPARK_ZIGZAG:
            return TUI_CHART_FAMILY_SPARK;
        case TUI_CHART_HEAT:
        case TUI_CHART_HEAT_BLOCK:
        case TUI_CHART_HEAT_DOT:
        case TUI_CHART_HEAT_SHADE:
        case TUI_CHART_HEAT_BWR:
        case TUI_CHART_HEAT_GYR:
        case TUI_CHART_HEAT_VIRIDIS:
        case TUI_CHART_HEAT_MAGMA:
        case TUI_CHART_HEAT_PLASMA:
        case TUI_CHART_HEAT_COOLWARM:
        case TUI_CHART_HEAT_BINARY:
        case TUI_CHART_HEAT_STEPS:
        case TUI_CHART_HEAT_DENSE:
        case TUI_CHART_HEAT_RANGE:
        case TUI_CHART_HEAT_LOG:
        case TUI_CHART_HEAT_SQRT:
            return TUI_CHART_FAMILY_HEAT;
        default:
            return TUI_CHART_FAMILY_HBAR;
    }
}

void tui_chart(tui_chart_type_t type, const char **labels, const double *values, int count,
               int width, int height) {
    if (g_tui_features && !g_tui_features->ascii_charts)
        return;
    if (!values || count <= 0)
        return;
    if (width <= 0)
        width = tui_term_width();
    if (height <= 0)
        height = 8;

    switch (tui_chart_family(type)) {
        case TUI_CHART_FAMILY_HBAR:
            tui_chart_hbar_render(labels, values, count, width);
            break;
        case TUI_CHART_FAMILY_VBAR:
            tui_chart_vbar_render(labels, values, count, width, height);
            break;
        case TUI_CHART_FAMILY_SPARK:
            tui_chart_spark_render(values, count, width);
            break;
        case TUI_CHART_FAMILY_HEAT:
            tui_chart_heat_render(values, count, width);
            break;
        default:
            tui_chart_hbar_render(labels, values, count, width);
            break;
    }
}

/* ── F7: Citation Footnotes ───────────────────────────────────────────── */

void tui_citation_init(tui_citation_t *c) {
    memset(c, 0, sizeof(*c));
}

int tui_citation_add(tui_citation_t *c, const char *tool_name, const char *tool_id,
                     const char *preview, double elapsed_ms) {
    if (c->count >= TUI_CITATION_MAX)
        return c->count;
    tui_citation_entry_t *e = &c->entries[c->count];
    snprintf(e->tool_name, sizeof(e->tool_name), "%s", tool_name ? tool_name : "");
    snprintf(e->tool_id, sizeof(e->tool_id), "%s", tool_id ? tool_id : "");
    if (preview) {
        const char *nl = strchr(preview, '\n');
        int len = nl ? (int)(nl - preview) : (int)strlen(preview);
        if (len > 120)
            len = 120;
        memcpy(e->preview, preview, len);
        e->preview[len] = '\0';
    }
    e->elapsed_ms = elapsed_ms;
    e->index = c->count + 1;
    return ++c->count;
}

void tui_citation_render(tui_citation_t *c) {
    if (g_tui_features && !g_tui_features->citation_footnotes)
        return;
    if (c->count == 0)
        return;
    fprintf(stderr, "\n  %s%sfootnotes:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < c->count; i++) {
        tui_citation_entry_t *e = &c->entries[i];
        fprintf(stderr, "  %s[%d]%s %s%s%s", TUI_DIM, e->index, TUI_RESET, TUI_CYAN, e->tool_name,
                TUI_RESET);
        if (e->elapsed_ms > 0)
            fprintf(stderr, " %s(%.0fms)%s", TUI_DIM, e->elapsed_ms, TUI_RESET);
        if (e->preview[0])
            fprintf(stderr, " %s%s%s", TUI_DIM, e->preview, TUI_RESET);
        fprintf(stderr, "\n");
    }
}

/* ── F3: Inline Diff Rendering ────────────────────────────────────────── */

bool tui_is_diff(const char *text) {
    if (!text)
        return false;
    /* Check for unified diff markers */
    const char *p = text;
    bool has_minus = false, has_plus = false, has_at = false;
    while (*p) {
        if (p == text || *(p - 1) == '\n') {
            if (strncmp(p, "--- ", 4) == 0)
                has_minus = true;
            else if (strncmp(p, "+++ ", 4) == 0)
                has_plus = true;
            else if (strncmp(p, "@@ ", 3) == 0)
                has_at = true;
        }
        p++;
    }
    return (has_minus && has_plus) || has_at;
}

void tui_render_diff(const char *text, FILE *out) {
    if (!text)
        return;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);

        if (*p == '+' && (len < 4 || strncmp(p, "+++", 3) != 0))
            fprintf(out, "%s", TUI_GREEN);
        else if (*p == '-' && (len < 4 || strncmp(p, "---", 3) != 0))
            fprintf(out, "%s", TUI_RED);
        else if (*p == '@' && len >= 2 && p[1] == '@')
            fprintf(out, "%s%s", TUI_BOLD, TUI_CYAN);
        else if (strncmp(p, "---", 3) == 0 || strncmp(p, "+++", 3) == 0)
            fprintf(out, "%s", TUI_BOLD);
        else
            fprintf(out, "%s", TUI_DIM);

        fwrite(p, 1, len, out);
        fprintf(out, "%s\n", TUI_RESET);

        if (!eol)
            break;
        p = eol + 1;
    }
}

/* ── F36: Table Sort Indicators ───────────────────────────────────────── */

void tui_table_render_sorted(const tui_table_t *t, int width, int sort_col, bool ascending) {
    if (!t || sort_col < 0 || sort_col >= t->col_count) {
        tui_table_render(t, width);
        return;
    }
    /* Create a modified copy with sort indicator in header */
    tui_table_t tmp = *t;
    static char modified_header[256];
    snprintf(modified_header, sizeof(modified_header), "%s %s", t->headers[sort_col],
             ascending ? "▲" : "▼");
    tmp.headers[sort_col] = modified_header;
    tui_table_render(&tmp, width);
}

/* ── F37: JSON Tree View ──────────────────────────────────────────────── */

static void json_tree_recurse(const char **p, int depth, int max_depth, bool color, bool last,
                              const char *prefix) {
    (void)last;
    if (!p || !*p)
        return;
    while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t')
        (*p)++;

    if (depth > max_depth) {
        fprintf(stderr, "%s...%s", color ? TUI_DIM : "", color ? TUI_RESET : "");
        /* Skip until matching bracket */
        int nesting = 0;
        while (**p) {
            if (**p == '{' || **p == '[')
                nesting++;
            else if (**p == '}' || **p == ']') {
                if (nesting == 0)
                    break;
                nesting--;
            }
            (*p)++;
        }
        return;
    }

    if (**p == '{') {
        (*p)++;
        while (**p && **p != '}') {
            while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t' || **p == ',')
                (*p)++;
            if (**p == '}')
                break;

            /* Parse key */
            if (**p == '"') {
                (*p)++;
                const char *key_start = *p;
                while (**p && **p != '"')
                    (*p)++;
                int key_len = (int)(*p - key_start);
                if (**p == '"')
                    (*p)++;
                while (**p == ' ' || **p == ':')
                    (*p)++;

                /* Determine if last key */
                bool is_last_key = false;
                {
                    const char *scan = *p;
                    int nest = 0;
                    if (*scan == '"') {
                        scan++;
                        while (*scan && !(*scan == '"' && *(scan - 1) != '\\'))
                            scan++;
                        if (*scan)
                            scan++;
                    } else if (*scan == '{' || *scan == '[') {
                        nest = 1;
                        scan++;
                        while (*scan && nest > 0) {
                            if (*scan == '{' || *scan == '[')
                                nest++;
                            if (*scan == '}' || *scan == ']')
                                nest--;
                            scan++;
                        }
                    } else {
                        while (*scan && *scan != ',' && *scan != '}')
                            scan++;
                    }
                    while (*scan == ' ' || *scan == '\n' || *scan == '\r' || *scan == '\t')
                        scan++;
                    if (*scan == ',' || *scan == '}') {
                        while (*scan == ' ' || *scan == ',' || *scan == '\n' || *scan == '\r' ||
                               *scan == '\t')
                            scan++;
                        is_last_key = (*scan == '}');
                    }
                }

                const char *connector = is_last_key ? "└── " : "├── ";
                const char *child_prefix_add = is_last_key ? "    " : "│   ";

                fprintf(stderr, "%s%s%s%.*s%s: ", prefix, connector, color ? TUI_BCYAN : "",
                        key_len, key_start, color ? TUI_RESET : "");

                char child_prefix[256];
                snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, child_prefix_add);
                json_tree_recurse(p, depth + 1, max_depth, color, is_last_key, child_prefix);
                fprintf(stderr, "\n");
            } else {
                break;
            }
        }
        if (**p == '}')
            (*p)++;
    } else if (**p == '[') {
        (*p)++;
        fprintf(stderr, "[");
        int idx = 0;
        while (**p && **p != ']') {
            while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t' || **p == ',')
                (*p)++;
            if (**p == ']')
                break;
            if (idx > 0)
                fprintf(stderr, ", ");
            json_tree_recurse(p, depth + 1, max_depth, color, false, prefix);
            idx++;
        }
        fprintf(stderr, "]");
        if (**p == ']')
            (*p)++;
    } else if (**p == '"') {
        (*p)++;
        const char *start = *p;
        while (**p && **p != '"') {
            if (**p == '\\')
                (*p)++;
            (*p)++;
        }
        fprintf(stderr, "%s\"%.*s\"%s", color ? TUI_GREEN : "", (int)(*p - start), start,
                color ? TUI_RESET : "");
        if (**p == '"')
            (*p)++;
    } else if (**p == 't' || **p == 'f') {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n')
            (*p)++;
        fprintf(stderr, "%s%.*s%s", color ? TUI_YELLOW : "", (int)(*p - start), start,
                color ? TUI_RESET : "");
    } else if (**p == 'n') {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n')
            (*p)++;
        fprintf(stderr, "%s%.*s%s", color ? TUI_RED : "", (int)(*p - start), start,
                color ? TUI_RESET : "");
    } else if (**p == '-' || (**p >= '0' && **p <= '9')) {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n')
            (*p)++;
        fprintf(stderr, "%s%.*s%s", color ? TUI_BCYAN : "", (int)(*p - start), start,
                color ? TUI_RESET : "");
    } else {
        (*p)++;
    }
}

void tui_json_tree(const char *json, int max_depth, bool color) {
    if (g_tui_features && !g_tui_features->json_tree)
        return;
    if (!json)
        return;
    const char *p = json;
    json_tree_recurse(&p, 0, max_depth > 0 ? max_depth : 5, color, true, "  ");
    fprintf(stderr, "\n");
}

/* ── F16: Conversation Minimap ────────────────────────────────────────── */

void tui_minimap_render(const tui_minimap_entry_t *entries, int count, int height) {
    if (g_tui_features && !g_tui_features->conv_minimap)
        return;
    if (!entries || count == 0)
        return;
    if (height <= 0)
        height = tui_term_height() - 4;
    if (height < 4)
        height = 4;

    /* Map entries to rows proportionally */
    int total_tokens = 0;
    for (int i = 0; i < count; i++)
        total_tokens += entries[i].tokens > 0 ? entries[i].tokens : 1;
    if (total_tokens == 0)
        total_tokens = 1;

    fprintf(stderr, "  %s%sminimap:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    int row = 0;
    for (int i = 0; i < count && row < height; i++) {
        int tok = entries[i].tokens > 0 ? entries[i].tokens : 1;
        int rows_for_entry = (int)((double)tok / total_tokens * height + 0.5);
        if (rows_for_entry < 1)
            rows_for_entry = 1;

        const char *block, *color;
        switch (entries[i].type) {
            case 'u':
                block = "█";
                color = TUI_BBLUE;
                break;
            case 'a':
                block = "░";
                color = TUI_BGREEN;
                break;
            case 't':
                block = "▓";
                color = TUI_BYELLOW;
                break;
            default:
                block = "·";
                color = TUI_DIM;
                break;
        }
        for (int r = 0; r < rows_for_entry && row < height; r++, row++) {
            fprintf(stderr, "  %s%s%s", color, block, TUI_RESET);
        }
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "  %s█%s user  %s░%s assistant  %s▓%s tool%s\n", TUI_BBLUE, TUI_RESET,
            TUI_BGREEN, TUI_RESET, TUI_BYELLOW, TUI_RESET, TUI_RESET);
}

/* ── F19: Branch Indicator ────────────────────────────────────────────── */

void tui_branch_init(tui_branch_t *b) {
    memset(b, 0, sizeof(*b));
}

void tui_branch_push(tui_branch_t *b, const char *prompt) {
    if (!prompt)
        return;
    int idx = b->head % TUI_BRANCH_HISTORY;
    snprintf(b->prompts[idx], sizeof(b->prompts[idx]), "%.255s", prompt);
    b->head++;
    if (b->count < TUI_BRANCH_HISTORY)
        b->count++;
}

/* Jaccard similarity on word sets */
static double word_jaccard(const char *a, const char *b) {
    char words_a[32][32], words_b[32][32];
    int na = 0, nb = 0;
    const char *p;

    p = a;
    while (*p && na < 32) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        int len = (int)(p - start);
        if (len > 0 && len < 32) {
            memcpy(words_a[na], start, len);
            words_a[na][len] = '\0';
            na++;
        }
    }
    p = b;
    while (*p && nb < 32) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        int len = (int)(p - start);
        if (len > 0 && len < 32) {
            memcpy(words_b[nb], start, len);
            words_b[nb][len] = '\0';
            nb++;
        }
    }

    if (na == 0 || nb == 0)
        return 0;

    int intersection = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (strcasecmp(words_a[i], words_b[j]) == 0) {
                intersection++;
                break;
            }

    int total_union = na + nb - intersection;
    return total_union > 0 ? (double)intersection / total_union : 0;
}

bool tui_branch_detect(tui_branch_t *b, const char *prompt) {
    if (g_tui_features && !g_tui_features->branch_indicator)
        return false;
    if (!prompt || b->count < 2)
        return false;
    for (int i = 0; i < b->count; i++) {
        int idx = (b->head - 1 - i + TUI_BRANCH_HISTORY * 2) % TUI_BRANCH_HISTORY;
        double sim = word_jaccard(prompt, b->prompts[idx]);
        if (sim > 0.6) {
            fprintf(stderr, "  %s↩ branch (%.0f%% similar to recent prompt)%s\n", TUI_DIM,
                    sim * 100, TUI_RESET);
            return true;
        }
    }
    return false;
}

/* ── F21: Ghost Suggestions ───────────────────────────────────────────── */

void tui_ghost_init(tui_ghost_t *g) {
    memset(g, 0, sizeof(*g));
}

void tui_ghost_push(tui_ghost_t *g, const char *cmd) {
    if (!cmd || !cmd[0])
        return;
    if (g->count > 0 && strcmp(g->history[g->count - 1], cmd) == 0)
        return;
    if (g->count < TUI_GHOST_MAX) {
        snprintf(g->history[g->count], sizeof(g->history[g->count]), "%.255s", cmd);
        g->count++;
    } else {
        memmove(g->history[0], g->history[1], sizeof(g->history[0]) * (TUI_GHOST_MAX - 1));
        snprintf(g->history[TUI_GHOST_MAX - 1], sizeof(g->history[0]), "%.255s", cmd);
    }
}

const char *tui_ghost_match(tui_ghost_t *g, const char *prefix) {
    if (!prefix || !prefix[0])
        return NULL;
    int plen = (int)strlen(prefix);
    for (int i = g->count - 1; i >= 0; i--) {
        if (strncmp(g->history[i], prefix, plen) == 0 && g->history[i][plen] != '\0')
            return g->history[i];
    }
    return NULL;
}

/* ── F20: Multi-line Input Syntax Highlighting ────────────────────────── */

bool tui_is_code_paste(const char *text) {
    if (!text)
        return false;
    bool has_newline = (strchr(text, '\n') != NULL);
    if (!has_newline)
        return false;
    bool has_brace = (strchr(text, '{') || strchr(text, '}'));
    bool has_semi = (strchr(text, ';') != NULL);
    bool has_kw = (strstr(text, "def ") || strstr(text, "func ") || strstr(text, "function ") ||
                   strstr(text, "class ") || strstr(text, "import ") || strstr(text, "#include"));
    return has_brace || has_semi || has_kw;
}

void tui_highlight_input(const char *text, FILE *out) {
    if (!text || !out)
        return;
    static const char *keywords[] = {"if",    "else",     "for",   "while", "return", "def",
                                     "class", "function", "const", "let",   "var",    "import",
                                     "from",  "struct",   "enum",  "int",   "void",   "char",
                                     "bool",  "true",     "false", "None",  "null",   NULL};
    const char *p = text;
    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            int wlen = (int)(p - start);
            bool is_kw = false;
            for (int k = 0; keywords[k]; k++) {
                if ((int)strlen(keywords[k]) == wlen && strncmp(start, keywords[k], wlen) == 0) {
                    is_kw = true;
                    break;
                }
            }
            if (is_kw)
                fprintf(out, "%s%.*s%s", TUI_BMAGENTA, wlen, start, TUI_RESET);
            else
                fwrite(start, 1, wlen, out);
        } else if (*p == '"' || *p == '\'') {
            char q = *p;
            const char *start = p;
            p++;
            while (*p && *p != q) {
                if (*p == '\\')
                    p++;
                p++;
            }
            if (*p == q)
                p++;
            fprintf(out, "%s%.*s%s", TUI_GREEN, (int)(p - start), start, TUI_RESET);
        } else if (*p >= '0' && *p <= '9') {
            const char *start = p;
            while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'x'))
                p++;
            fprintf(out, "%s%.*s%s", TUI_BYELLOW, (int)(p - start), start, TUI_RESET);
        } else if (*p == '/' && *(p + 1) == '/') {
            const char *start = p;
            while (*p && *p != '\n')
                p++;
            fprintf(out, "%s%.*s%s", TUI_DIM, (int)(p - start), start, TUI_RESET);
        } else {
            fputc(*p, out);
            p++;
        }
    }
}

/* ── F23: Drag-Drop Preview ───────────────────────────────────────────── */

void tui_image_preview_badge(const char *path, const char *media_type, long size, int w, int h) {
    if (g_tui_features && !g_tui_features->drag_drop_preview)
        return;
    fprintf(stderr, "  ╭─ %s%simage%s ─╮\n", TUI_BOLD, TUI_CYAN, TUI_RESET);
    if (path) {
        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        fprintf(stderr, "  │ %s%s%s\n", TUI_BWHITE, basename, TUI_RESET);
    }
    fprintf(stderr, "  │ %s%s%s", TUI_DIM, media_type ? media_type : "unknown", TUI_RESET);
    if (size > 0) {
        if (size >= 1024 * 1024)
            fprintf(stderr, " %.1fMB", size / (1024.0 * 1024.0));
        else if (size >= 1024)
            fprintf(stderr, " %.1fKB", size / 1024.0);
        else
            fprintf(stderr, " %ldB", size);
    }
    if (w > 0 && h > 0)
        fprintf(stderr, " %dx%d", w, h);
    fprintf(stderr, "\n");
    fprintf(stderr, "  ╰──────────╯\n");
}

/* ── F24: Slash Command Palette ───────────────────────────────────────── */

static bool subsequence_match(const char *str, const char *filter) {
    if (!filter || !filter[0])
        return true;
    const char *s = str, *f = filter;
    while (*s && *f) {
        if (tolower((unsigned char)*s) == tolower((unsigned char)*f))
            f++;
        s++;
    }
    return *f == '\0';
}

void tui_command_palette(const tui_cmd_entry_t *cmds, int count, const char *filter) {
    if (g_tui_features && !g_tui_features->command_palette)
        return;
    if (!cmds || count == 0)
        return;

    int matches = 0;
    for (int i = 0; i < count; i++) {
        if (!filter || !filter[0] || subsequence_match(cmds[i].name, filter)) {
            if (matches == 0) {
                fprintf(stderr, "  ╭─ %s%scommands%s ─╮\n", TUI_BOLD, TUI_CYAN, TUI_RESET);
            }
            fprintf(stderr, "  │ %s%-16s%s %s%s%s\n", TUI_BCYAN, cmds[i].name, TUI_RESET, TUI_DIM,
                    cmds[i].desc ? cmds[i].desc : "", TUI_RESET);
            matches++;
        }
    }
    if (matches > 0)
        fprintf(stderr, "  ╰──────────────────╯\n");
    else if (filter && filter[0])
        fprintf(stderr, "  %sno matching commands%s\n", TUI_DIM, TUI_RESET);
}

/* ── F25: Agent Topology ──────────────────────────────────────────────── */

static void topo_print_children(const tui_agent_node_t *agents, int count, int parent_id,
                                const char *prefix, bool last) {
    (void)last;
    for (int i = 0; i < count; i++) {
        if (agents[i].parent_id == parent_id) {
            bool is_last = true;
            for (int j = i + 1; j < count; j++)
                if (agents[j].parent_id == parent_id) {
                    is_last = false;
                    break;
                }

            const char *connector = is_last ? "└─" : "├─";
            const char *child_add = is_last ? "  " : "│ ";
            const char *status_color = TUI_DIM;
            if (agents[i].status) {
                if (strcmp(agents[i].status, "running") == 0)
                    status_color = TUI_BCYAN;
                else if (strcmp(agents[i].status, "done") == 0)
                    status_color = TUI_GREEN;
                else if (strcmp(agents[i].status, "error") == 0)
                    status_color = TUI_RED;
            }

            fprintf(stderr, "%s%s %s[%d]%s %s%s%s", prefix, connector, status_color, agents[i].id,
                    TUI_RESET, TUI_DIM, agents[i].task ? agents[i].task : "", TUI_RESET);
            if (agents[i].status)
                fprintf(stderr, " %s(%s)%s", status_color, agents[i].status, TUI_RESET);
            fprintf(stderr, "\n");

            char child_prefix[256];
            snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, child_add);
            topo_print_children(agents, count, agents[i].id, child_prefix, is_last);
        }
    }
}

void tui_agent_topology(const tui_agent_node_t *agents, int count) {
    if (g_tui_features && !g_tui_features->agent_topology)
        return;
    if (!agents || count == 0)
        return;
    fprintf(stderr, "  %s%sagent topology:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < count; i++) {
        if (agents[i].parent_id <= 0) {
            const char *sc = TUI_BCYAN;
            if (agents[i].status && strcmp(agents[i].status, "done") == 0)
                sc = TUI_GREEN;
            fprintf(stderr, "  %s[%d]%s %s%s%s\n", sc, agents[i].id, TUI_RESET, TUI_DIM,
                    agents[i].task ? agents[i].task : "root", TUI_RESET);
            topo_print_children(agents, count, agents[i].id, "  ", true);
        }
    }
}

/* ── F28: Swarm Cost Aggregation ──────────────────────────────────────── */

void tui_swarm_cost(const tui_swarm_cost_entry_t *agents, int count, double total) {
    if (g_tui_features && !g_tui_features->swarm_cost)
        return;
    if (!agents || count == 0)
        return;
    fprintf(stderr, "  %s%sswarm cost:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "    %s%-20s%s $%.4f %s(%d in, %d out)%s\n", TUI_CYAN,
                agents[i].name ? agents[i].name : "?", TUI_RESET, agents[i].cost, TUI_DIM,
                agents[i].in_tok, agents[i].out_tok, TUI_RESET);
    }
    fprintf(stderr, "    %s%s──────────────────────%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    fprintf(stderr, "    %s%-20s%s %s$%.4f%s\n", TUI_BWHITE, "total", TUI_RESET, TUI_BOLD, total,
            TUI_RESET);
}

/* ── F33: Smooth Scroll ───────────────────────────────────────────────── */

void tui_scroller_init(tui_scroller_t *s, const char **lines, int count) {
    s->lines = lines;
    s->line_count = count;
    s->offset = 0;
    s->page_size = tui_term_height() - 4;
    if (s->page_size < 5)
        s->page_size = 5;
}

void tui_scroller_render(tui_scroller_t *s) {
    if (!s->lines)
        return;
    int end = s->offset + s->page_size;
    if (end > s->line_count)
        end = s->line_count;

    for (int i = s->offset; i < end; i++) {
        fprintf(stderr, "%s\n", s->lines[i] ? s->lines[i] : "");
    }
    fprintf(stderr, "%s── %d-%d of %d (j/k/q) ──%s\n", TUI_DIM, s->offset + 1, end, s->line_count,
            TUI_RESET);
}

bool tui_scroller_handle_key(tui_scroller_t *s, int ch) {
    switch (ch) {
        case 'j':
        case 'J':
            if (s->offset + s->page_size < s->line_count)
                s->offset++;
            return true;
        case 'k':
        case 'K':
            if (s->offset > 0)
                s->offset--;
            return true;
        case ' ':
            s->offset += s->page_size;
            if (s->offset > s->line_count - s->page_size)
                s->offset = s->line_count - s->page_size;
            if (s->offset < 0)
                s->offset = 0;
            return true;
        case 'q':
        case 'Q':
        case 27:
            return false;
    }
    return true;
}

/* ── F40: Latency Budget Breakdown ────────────────────────────────────── */

void tui_latency_waterfall(const tui_latency_breakdown_t *b) {
    if (g_tui_features && !g_tui_features->latency_waterfall)
        return;
    if (!b || b->total_ms <= 0)
        return;

    int bar_width = tui_term_width() - 30;
    if (bar_width < 20)
        bar_width = 20;

    double phases[] = {
        b->dns_ms,
        b->connect_ms - b->dns_ms,
        b->tls_ms - b->connect_ms,
        b->ttfb_ms - b->tls_ms,
        b->total_ms - b->ttfb_ms,
    };
    const char *labels[] = {"DNS", "Connect", "TLS", "TTFB", "Transfer"};
    const char *colors[] = {TUI_BCYAN, TUI_BBLUE, TUI_BMAGENTA, TUI_BYELLOW, TUI_BGREEN};

    fprintf(stderr, "  %s%slatency waterfall:%s (%.0fms total)\n", TUI_BOLD, TUI_DIM, TUI_RESET,
            b->total_ms);

    for (int i = 0; i < 5; i++) {
        if (phases[i] < 0)
            phases[i] = 0;
        int bar_len = (int)(phases[i] / b->total_ms * bar_width);
        if (bar_len < 0)
            bar_len = 0;
        fprintf(stderr, "  %-10s ", labels[i]);
        for (int c = 0; c < bar_len; c++)
            fprintf(stderr, "%s█%s", colors[i], TUI_RESET);
        fprintf(stderr, " %s%.0fms%s\n", TUI_DIM, phases[i], TUI_RESET);
    }
}

/* ── F18: Session Diff on Load ────────────────────────────────────────── */

void tui_session_diff(int msg_count, int tool_calls, int est_tokens, const char *model) {
    if (g_tui_features && !g_tui_features->session_diff)
        return;
    char body[512];
    snprintf(body, sizeof(body), "%d messages · %d tool calls · ~%d tokens\nmodel: %s", msg_count,
             tool_calls, est_tokens, model ? model : "unknown");
    tui_box("session loaded", body, BOX_ROUND, TUI_CYAN, 0);
}

/* ── F1: Token Heatmap ────────────────────────────────────────────────── */

void tui_heatmap_word(const char *word, int len, FILE *out) {
    if (g_tui_features && !g_tui_features->token_heatmap) {
        fwrite(word, 1, len, out);
        return;
    }
    float hue;
    if (len <= 3)
        hue = 120.0f;
    else if (len <= 5)
        hue = 80.0f;
    else if (len <= 7)
        hue = 50.0f;
    else
        hue = 30.0f;

    tui_rgb_t rgb = tui_hsv_to_rgb(hue, 0.5f, 0.9f);
    if (tui_supports_truecolor()) {
        fprintf(out, "\033[38;2;%d;%d;%dm", rgb.r, rgb.g, rgb.b);
    } else {
        int idx = 16 + (int)(rgb.r / 51.0) * 36 + (int)(rgb.g / 51.0) * 6 + (int)(rgb.b / 51.0);
        fprintf(out, "\033[38;5;%dm", idx);
    }
    fwrite(word, 1, len, out);
    fprintf(out, "%s", TUI_RESET);
}

/* ── Features listing / toggle ────────────────────────────────────────── */

void tui_features_list(const tui_features_t *f) {
    if (!f)
        return;
    const bool *flags = (const bool *)f;
    const tui_glyphs_t *gl = tui_glyph();
    fprintf(stderr, "  %s%sUI Features:%s\n", TUI_BOLD, TUI_BCYAN, TUI_RESET);
    for (int i = 0; i < TUI_FEATURE_COUNT; i++) {
        fprintf(stderr, "    %s[%s]%s F%-2d %s\n", flags[i] ? TUI_GREEN : TUI_RED,
                flags[i] ? gl->ok : gl->fail, TUI_RESET, i + 1, tui_feature_name(i));
    }
}

bool tui_features_toggle(tui_features_t *f, const char *name) {
    if (!f || !name)
        return false;
    bool *flags = (bool *)f;

    for (int i = 0; i < TUI_FEATURE_COUNT; i++) {
        if (strcasecmp(tui_feature_name(i), name) == 0) {
            flags[i] = !flags[i];
            {
                const tui_glyphs_t *gl = tui_glyph();
                fprintf(stderr, "  %s%s%s %s %s %s%s%s\n", flags[i] ? TUI_GREEN : TUI_RED,
                        flags[i] ? gl->ok : gl->fail, TUI_RESET, tui_feature_name(i),
                        gl->arrow_right, flags[i] ? TUI_GREEN : TUI_RED, flags[i] ? "on" : "off",
                        TUI_RESET);
            }
            return true;
        }
    }

    int num = 0;
    if (name[0] == 'F' || name[0] == 'f')
        num = atoi(name + 1);
    else
        num = atoi(name);
    if (num >= 1 && num <= TUI_FEATURE_COUNT) {
        int idx = num - 1;
        flags[idx] = !flags[idx];
        {
            const tui_glyphs_t *gl = tui_glyph();
            fprintf(stderr, "  %s%s%s F%d %s %s %s%s%s\n", flags[idx] ? TUI_GREEN : TUI_RED,
                    flags[idx] ? gl->ok : gl->fail, TUI_RESET, num, tui_feature_name(idx),
                    gl->arrow_right, flags[idx] ? TUI_GREEN : TUI_RED, flags[idx] ? "on" : "off",
                    TUI_RESET);
        }
        return true;
    }

    fprintf(stderr, "  %sunknown feature: %s%s\n", TUI_RED, name, TUI_RESET);
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ADVANCED STATEFUL ABSTRACTIONS — IMPLEMENTATIONS
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Notification Queue ──────────────────────────────────────────────── */

void tui_notif_queue_init(tui_notif_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    q->next_id = 1;
}

static const char *notif_level_icon(tui_notif_level_t level) {
    switch (level) {
        case TUI_NOTIF_DEBUG:
            return "\xE2\x80\xA2"; /* • */
        case TUI_NOTIF_INFO:
            return "\xE2\x84\xB9"; /* ℹ */
        case TUI_NOTIF_SUCCESS:
            return "\xE2\x9C\x93"; /* ✓ */
        case TUI_NOTIF_WARNING:
            return "\xE2\x9A\xA0"; /* ⚠ */
        case TUI_NOTIF_ERROR:
            return "\xE2\x9C\x97"; /* ✗ */
        case TUI_NOTIF_CRITICAL:
            return "\xE2\x9C\x98"; /* ✘ */
        default:
            return " ";
    }
}

static const char *notif_level_color(tui_notif_level_t level) {
    switch (level) {
        case TUI_NOTIF_DEBUG:
            return TUI_DIM;
        case TUI_NOTIF_INFO:
            return TUI_CYAN;
        case TUI_NOTIF_SUCCESS:
            return TUI_GREEN;
        case TUI_NOTIF_WARNING:
            return TUI_YELLOW;
        case TUI_NOTIF_ERROR:
            return TUI_RED;
        case TUI_NOTIF_CRITICAL:
            return TUI_BRED;
        default:
            return TUI_RESET;
    }
}

static double notif_default_ttl(tui_notif_level_t level) {
    switch (level) {
        case TUI_NOTIF_DEBUG:
            return 5.0;
        case TUI_NOTIF_INFO:
            return 15.0;
        case TUI_NOTIF_SUCCESS:
            return 10.0;
        case TUI_NOTIF_WARNING:
            return 30.0;
        case TUI_NOTIF_ERROR:
            return 0.0; /* persist */
        case TUI_NOTIF_CRITICAL:
            return 0.0; /* persist */
        default:
            return 10.0;
    }
}

int tui_notif_push(tui_notif_queue_t *q, tui_notif_level_t level, const char *tag, const char *fmt,
                   ...) {
    pthread_mutex_lock(&q->mutex);

    /* Coalesce: if same tag+level+msg already exists, increment count */
    char msg[TUI_NOTIF_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (int i = 0; i < q->count; i++) {
        if (!q->queue[i].dismissed && q->queue[i].level == level && tag && q->queue[i].tag[0] &&
            strcmp(q->queue[i].tag, tag) == 0 && strcmp(q->queue[i].msg, msg) == 0) {
            q->queue[i].count++;
            q->queue[i].created_at = tui_now();
            int id = q->queue[i].id;
            pthread_mutex_unlock(&q->mutex);
            return id;
        }
    }

    /* Evict oldest if full */
    if (q->count >= TUI_NOTIF_QUEUE_MAX) {
        /* Remove oldest dismissed or lowest priority */
        int victim = 0;
        for (int i = 1; i < q->count; i++) {
            if (q->queue[i].dismissed && !q->queue[victim].dismissed) {
                victim = i;
                break;
            }
            if (q->queue[i].created_at < q->queue[victim].created_at)
                victim = i;
        }
        if (victim < q->count - 1)
            memmove(&q->queue[victim], &q->queue[victim + 1],
                    (q->count - victim - 1) * sizeof(tui_notif_t));
        q->count--;
    }

    tui_notif_t *n = &q->queue[q->count];
    memset(n, 0, sizeof(*n));
    n->id = q->next_id++;
    n->level = level;
    strncpy(n->msg, msg, TUI_NOTIF_MSG_MAX - 1);
    if (tag)
        strncpy(n->tag, tag, TUI_NOTIF_TAG_MAX - 1);
    n->created_at = tui_now();
    n->ttl_sec = notif_default_ttl(level);
    n->count = 1;
    q->count++;
    q->unread++;

    /* Bell for critical */
    if (level == TUI_NOTIF_CRITICAL) {
        fprintf(stderr, "\a");
    }

    int id = n->id;
    pthread_mutex_unlock(&q->mutex);
    return id;
}

void tui_notif_dismiss(tui_notif_queue_t *q, int id) {
    pthread_mutex_lock(&q->mutex);
    for (int i = 0; i < q->count; i++) {
        if (q->queue[i].id == id) {
            q->queue[i].dismissed = true;
            break;
        }
    }
    pthread_mutex_unlock(&q->mutex);
}

void tui_notif_dismiss_tag(tui_notif_queue_t *q, const char *tag) {
    pthread_mutex_lock(&q->mutex);
    for (int i = 0; i < q->count; i++) {
        if (tag && q->queue[i].tag[0] && strcmp(q->queue[i].tag, tag) == 0) {
            q->queue[i].dismissed = true;
        }
    }
    pthread_mutex_unlock(&q->mutex);
}

void tui_notif_gc(tui_notif_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    double now = tui_now();
    int dst = 0;
    for (int i = 0; i < q->count; i++) {
        bool expired =
            (q->queue[i].ttl_sec > 0 && (now - q->queue[i].created_at) > q->queue[i].ttl_sec);
        if (!q->queue[i].dismissed && !expired) {
            if (dst != i)
                q->queue[dst] = q->queue[i];
            dst++;
        }
    }
    q->count = dst;
    pthread_mutex_unlock(&q->mutex);
}

void tui_notif_render(tui_notif_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return;
    }

    double now = tui_now();
    bool any = false;
    for (int i = 0; i < q->count; i++) {
        tui_notif_t *n = &q->queue[i];
        if (n->dismissed)
            continue;
        if (n->ttl_sec > 0 && (now - n->created_at) > n->ttl_sec)
            continue;

        if (!any) {
            fprintf(stderr, "  %s\xe2\x94\x80\xe2\x94\x80 notifications %s\n", TUI_DIM, TUI_RESET);
            any = true;
        }

        const char *icon = notif_level_icon(n->level);
        const char *color = notif_level_color(n->level);
        double age = now - n->created_at;

        fprintf(stderr, "  %s%s%s %s%s%s", color, icon, TUI_RESET, color, n->msg, TUI_RESET);
        if (n->count > 1)
            fprintf(stderr, " %s(\xC3\x97%d)%s", TUI_DIM, n->count, TUI_RESET);
        if (n->tag[0])
            fprintf(stderr, " %s[%s]%s", TUI_DIM, n->tag, TUI_RESET);
        if (age > 60)
            fprintf(stderr, " %s%.0fm ago%s", TUI_DIM, age / 60, TUI_RESET);

        fprintf(stderr, "\n");
        n->seen = true;
    }
    q->unread = 0;
    pthread_mutex_unlock(&q->mutex);
}

int tui_notif_unread(tui_notif_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    int n = q->unread;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

void tui_notif_clear_all(tui_notif_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->count = 0;
    q->unread = 0;
    pthread_mutex_unlock(&q->mutex);
}

/* ── Toast System ────────────────────────────────────────────────────── */

void tui_toast_init(tui_toast_t *t) {
    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->mutex, NULL);
}

void tui_toast_show(tui_toast_t *t, tui_notif_level_t level, double duration_sec, const char *fmt,
                    ...) {
    pthread_mutex_lock(&t->mutex);

    /* Find empty slot or overwrite oldest */
    int slot = -1;
    double oldest = 1e18;
    int oldest_idx = 0;
    for (int i = 0; i < TUI_TOAST_MAX; i++) {
        if (!t->toasts[i].active) {
            slot = i;
            break;
        }
        if (t->toasts[i].expire_at < oldest) {
            oldest = t->toasts[i].expire_at;
            oldest_idx = i;
        }
    }
    if (slot < 0)
        slot = oldest_idx;

    tui_toast_entry_t *e = &t->toasts[slot];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, TUI_TOAST_MSG_MAX, fmt, ap);
    va_end(ap);

    e->level = level;
    e->expire_at = tui_now() + duration_sec;
    e->active = true;

    const char *icon = notif_level_icon(level);
    snprintf(e->icon, sizeof(e->icon), "%s", icon);

    if (slot >= t->count)
        t->count = slot + 1;

    /* Render immediately */
    const char *color = notif_level_color(level);
    fprintf(stderr, "  %s%s %s%s\n", color, e->icon, e->msg, TUI_RESET);
    fflush(stderr);

    pthread_mutex_unlock(&t->mutex);
}

void tui_toast_tick(tui_toast_t *t) {
    pthread_mutex_lock(&t->mutex);
    double now = tui_now();
    for (int i = 0; i < t->count; i++) {
        if (t->toasts[i].active && now >= t->toasts[i].expire_at) {
            t->toasts[i].active = false;
        }
    }
    pthread_mutex_unlock(&t->mutex);
}

void tui_toast_destroy(tui_toast_t *t) {
    pthread_mutex_destroy(&t->mutex);
}

/* ── State Machine Framework ─────────────────────────────────────────── */

void tui_fsm_init(tui_fsm_t *fsm, const char *name, void *ctx) {
    memset(fsm, 0, sizeof(*fsm));
    if (name)
        strncpy(fsm->name, name, TUI_FSM_NAME_MAX - 1);
    fsm->ctx = ctx;
    fsm->current = -1;
}

int tui_fsm_add_state(tui_fsm_t *fsm, const char *name, tui_fsm_action_fn on_enter,
                      tui_fsm_action_fn on_exit, tui_fsm_action_fn on_tick) {
    if (fsm->state_count >= TUI_FSM_MAX_STATES)
        return -1;
    int idx = fsm->state_count++;
    tui_fsm_state_t *s = &fsm->states[idx];
    if (name)
        strncpy(s->name, name, TUI_FSM_NAME_MAX - 1);
    s->on_enter = on_enter;
    s->on_exit = on_exit;
    s->on_tick = on_tick;

    /* First state added becomes initial state */
    if (fsm->current < 0) {
        fsm->current = idx;
        fsm->state_entered_at = tui_now();
        if (s->on_enter)
            s->on_enter(fsm->ctx);
    }
    return idx;
}

void tui_fsm_add_transition(tui_fsm_t *fsm, int from, int to, int event, tui_fsm_guard_fn guard,
                            tui_fsm_action_fn action) {
    if (fsm->trans_count >= TUI_FSM_MAX_TRANS)
        return;
    tui_fsm_trans_t *t = &fsm->transitions[fsm->trans_count++];
    t->from = from;
    t->to = to;
    t->event = event;
    t->guard = guard;
    t->action = action;
}

bool tui_fsm_send(tui_fsm_t *fsm, int event) {
    for (int i = 0; i < fsm->trans_count; i++) {
        tui_fsm_trans_t *t = &fsm->transitions[i];
        if (t->from == fsm->current && t->event == event) {
            /* Check guard */
            if (t->guard && !t->guard(fsm->ctx))
                continue;

            /* Exit current state */
            if (fsm->states[fsm->current].on_exit)
                fsm->states[fsm->current].on_exit(fsm->ctx);

            /* Record history */
            if (fsm->history_len < TUI_FSM_HISTORY_SIZE)
                fsm->history[fsm->history_len++] = fsm->current;

            /* Transition action */
            if (t->action)
                t->action(fsm->ctx);

            /* Enter new state */
            fsm->current = t->to;
            fsm->state_entered_at = tui_now();
            if (fsm->states[fsm->current].on_enter)
                fsm->states[fsm->current].on_enter(fsm->ctx);

            return true;
        }
    }
    return false;
}

void tui_fsm_tick(tui_fsm_t *fsm) {
    if (fsm->current >= 0 && fsm->current < fsm->state_count) {
        if (fsm->states[fsm->current].on_tick)
            fsm->states[fsm->current].on_tick(fsm->ctx);
    }
}

const char *tui_fsm_current_name(const tui_fsm_t *fsm) {
    if (fsm->current >= 0 && fsm->current < fsm->state_count)
        return fsm->states[fsm->current].name;
    return "(none)";
}

double tui_fsm_time_in_state(const tui_fsm_t *fsm) {
    return tui_now() - fsm->state_entered_at;
}

void tui_fsm_debug(const tui_fsm_t *fsm) {
    fprintf(stderr, "  %sFSM '%s': state=%s (%.1fs)%s\n", TUI_DIM, fsm->name,
            tui_fsm_current_name(fsm), tui_fsm_time_in_state(fsm), TUI_RESET);
    if (fsm->history_len > 0) {
        fprintf(stderr, "  %shistory: ", TUI_DIM);
        for (int i = 0; i < fsm->history_len; i++) {
            if (i > 0)
                fprintf(stderr, " \xe2\x86\x92 "); /* → */
            fprintf(stderr, "%s", fsm->states[fsm->history[i]].name);
        }
        fprintf(stderr, " \xe2\x86\x92 %s%s\n", tui_fsm_current_name(fsm), TUI_RESET);
    }
}

/* ── Render Context ──────────────────────────────────────────────────── */

void tui_render_ctx_init(tui_render_ctx_t *rc) {
    memset(rc, 0, sizeof(*rc));
    pthread_mutex_init(&rc->mutex, NULL);
    rc->term_width = tui_term_width();
    rc->term_height = tui_term_height();
}

int tui_render_slot_alloc(tui_render_ctx_t *rc, tui_slot_type_t type, int z_order) {
    pthread_mutex_lock(&rc->mutex);
    if (rc->slot_count >= TUI_RENDER_SLOTS_MAX) {
        pthread_mutex_unlock(&rc->mutex);
        return -1;
    }
    int idx = rc->slot_count++;
    tui_render_slot_t *s = &rc->slots[idx];
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->row = -1;
    s->z_order = z_order;
    s->dirty = true;
    rc->layout_dirty = true;
    pthread_mutex_unlock(&rc->mutex);
    return idx;
}

void tui_render_slot_update(tui_render_ctx_t *rc, int slot_id, const char *content) {
    if (slot_id < 0 || slot_id >= rc->slot_count)
        return;
    pthread_mutex_lock(&rc->mutex);
    tui_render_slot_t *s = &rc->slots[slot_id];
    if (strcmp(s->content, content) != 0) {
        strncpy(s->content, content, TUI_RENDER_CONTENT_MAX - 1);
        s->dirty = true;
    }
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_slot_free(tui_render_ctx_t *rc, int slot_id) {
    if (slot_id < 0 || slot_id >= rc->slot_count)
        return;
    pthread_mutex_lock(&rc->mutex);
    rc->slots[slot_id].type = TUI_SLOT_EMPTY;
    rc->slots[slot_id].dirty = false;
    rc->layout_dirty = true;
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_slot_dirty(tui_render_ctx_t *rc, int slot_id) {
    if (slot_id < 0 || slot_id >= rc->slot_count)
        return;
    pthread_mutex_lock(&rc->mutex);
    rc->slots[slot_id].dirty = true;
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_flush(tui_render_ctx_t *rc) {
    pthread_mutex_lock(&rc->mutex);
    double now = tui_now();

    /* Sort by z_order for rendering */
    for (int i = 0; i < rc->slot_count; i++) {
        tui_render_slot_t *s = &rc->slots[i];
        if (s->type == TUI_SLOT_EMPTY || !s->dirty)
            continue;
        if (s->content[0]) {
            fprintf(stderr, "%s", s->content);
        }
        s->dirty = false;
        s->last_render = now;
    }
    fflush(stderr);
    rc->layout_dirty = false;
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_ctx_destroy(tui_render_ctx_t *rc) {
    pthread_mutex_destroy(&rc->mutex);
}

/* ── Multi-Phase Progress ────────────────────────────────────────────── */

void tui_multi_progress_init(tui_multi_progress_t *mp, const char *title) {
    memset(mp, 0, sizeof(*mp));
    pthread_mutex_init(&mp->mutex, NULL);
    if (title)
        strncpy(mp->title, title, TUI_PROGRESS_NAME_MAX - 1);
    mp->start_time = tui_now();
    mp->ema_alpha = 0.3;
    mp->current_phase = -1;
}

int tui_multi_progress_add_phase(tui_multi_progress_t *mp, const char *name, double weight) {
    pthread_mutex_lock(&mp->mutex);
    if (mp->phase_count >= TUI_PROGRESS_PHASES_MAX) {
        pthread_mutex_unlock(&mp->mutex);
        return -1;
    }
    int idx = mp->phase_count++;
    tui_progress_phase_t *p = &mp->phases[idx];
    memset(p, 0, sizeof(*p));
    if (name)
        strncpy(p->name, name, TUI_PROGRESS_NAME_MAX - 1);
    p->weight = weight > 0 ? weight : 1.0;
    pthread_mutex_unlock(&mp->mutex);
    return idx;
}

void tui_multi_progress_start_phase(tui_multi_progress_t *mp, int phase_idx) {
    pthread_mutex_lock(&mp->mutex);
    if (phase_idx >= 0 && phase_idx < mp->phase_count) {
        /* Complete previous phase if still active */
        if (mp->current_phase >= 0 && mp->phases[mp->current_phase].active) {
            mp->phases[mp->current_phase].progress = 1.0;
            mp->phases[mp->current_phase].complete = true;
            mp->phases[mp->current_phase].active = false;
            mp->phases[mp->current_phase].end_time = tui_now();
        }
        mp->current_phase = phase_idx;
        mp->phases[phase_idx].active = true;
        mp->phases[phase_idx].start_time = tui_now();
    }
    pthread_mutex_unlock(&mp->mutex);
}

void tui_multi_progress_update(tui_multi_progress_t *mp, double progress) {
    pthread_mutex_lock(&mp->mutex);
    if (mp->current_phase >= 0 && mp->current_phase < mp->phase_count) {
        double prev = mp->phases[mp->current_phase].progress;
        mp->phases[mp->current_phase].progress = progress > 1.0 ? 1.0 : progress;

        /* Update EMA rate */
        double dt = tui_now() - mp->phases[mp->current_phase].start_time;
        if (dt > 0.1 && progress > prev) {
            double rate = progress / dt;
            if (mp->ema_rate <= 0)
                mp->ema_rate = rate;
            else
                mp->ema_rate = mp->ema_alpha * rate + (1 - mp->ema_alpha) * mp->ema_rate;
        }
    }
    pthread_mutex_unlock(&mp->mutex);
}

void tui_multi_progress_complete_phase(tui_multi_progress_t *mp) {
    pthread_mutex_lock(&mp->mutex);
    if (mp->current_phase >= 0 && mp->current_phase < mp->phase_count) {
        mp->phases[mp->current_phase].progress = 1.0;
        mp->phases[mp->current_phase].complete = true;
        mp->phases[mp->current_phase].active = false;
        mp->phases[mp->current_phase].end_time = tui_now();
    }
    pthread_mutex_unlock(&mp->mutex);
}

double tui_multi_progress_total(tui_multi_progress_t *mp) {
    pthread_mutex_lock(&mp->mutex);
    double total_weight = 0, weighted_progress = 0;
    for (int i = 0; i < mp->phase_count; i++) {
        total_weight += mp->phases[i].weight;
        weighted_progress += mp->phases[i].weight * mp->phases[i].progress;
    }
    double result = total_weight > 0 ? weighted_progress / total_weight : 0;
    pthread_mutex_unlock(&mp->mutex);
    return result;
}

double tui_multi_progress_eta_sec(tui_multi_progress_t *mp) {
    pthread_mutex_lock(&mp->mutex);
    double total = 0.0;
    double weighted_progress = 0.0;
    for (int i = 0; i < mp->phase_count; i++) {
        total += mp->phases[i].weight;
        weighted_progress += mp->phases[i].weight * mp->phases[i].progress;
    }
    total = total > 0.0 ? weighted_progress / total : 0.0;
    double remaining = 1.0 - total;
    double eta = -1;
    if (mp->ema_rate > 0 && remaining > 0) {
        eta = remaining / mp->ema_rate;
    }
    pthread_mutex_unlock(&mp->mutex);
    return eta;
}

void tui_multi_progress_render(tui_multi_progress_t *mp) {
    pthread_mutex_lock(&mp->mutex);
    if (mp->phase_count == 0) {
        pthread_mutex_unlock(&mp->mutex);
        return;
    }

    int width = tui_term_width() - 8;
    if (width < 20)
        width = 20;
    if (width > 80)
        width = 80;

    double total_pct = 0;
    double total_weight = 0;
    for (int i = 0; i < mp->phase_count; i++)
        total_weight += mp->phases[i].weight;

    fprintf(stderr, "  %s%s%s\n", TUI_BOLD, mp->title, TUI_RESET);

    for (int i = 0; i < mp->phase_count; i++) {
        tui_progress_phase_t *p = &mp->phases[i];
        const char *icon = p->complete ? "\xe2\x9c\x93"
                           : p->active ? "\xe2\x97\x89"
                                       : "\xe2\x97\x8b"; /* ✓ ◉ ○ */
        const char *color = p->complete ? TUI_GREEN : p->active ? TUI_CYAN : TUI_DIM;

        int bar_w = (int)(width * p->weight / total_weight);
        if (bar_w < 5)
            bar_w = 5;
        int filled = (int)(bar_w * p->progress);

        fprintf(stderr, "  %s%s%s %s%-12s%s ", color, icon, TUI_RESET, color, p->name, TUI_RESET);

        /* Bar */
        fprintf(stderr, "%s", color);
        for (int j = 0; j < bar_w; j++) {
            fprintf(stderr, "%s", j < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); /* █ ░ */
        }
        fprintf(stderr, "%s", TUI_RESET);

        fprintf(stderr, " %s%.0f%%%s", TUI_DIM, p->progress * 100, TUI_RESET);

        if (p->complete && p->end_time > p->start_time) {
            fprintf(stderr, " %s(%.1fs)%s", TUI_DIM, p->end_time - p->start_time, TUI_RESET);
        }
        fprintf(stderr, "\n");

        total_pct += p->progress * p->weight / total_weight;
    }

    /* Total bar */
    fprintf(stderr, "  %s", TUI_BOLD);
    int total_bar = width - 10;
    int total_filled = (int)(total_bar * total_pct);
    for (int j = 0; j < total_bar; j++) {
        fprintf(stderr, "%s", j < total_filled ? "\xe2\x96\x88" : "\xe2\x96\x91");
    }
    fprintf(stderr, " %.0f%%%s", total_pct * 100, TUI_RESET);

    double eta = mp->ema_rate > 0 ? (1.0 - total_pct) / mp->ema_rate : -1;
    if (eta > 0) {
        if (eta < 60)
            fprintf(stderr, " %sETA %.0fs%s", TUI_DIM, eta, TUI_RESET);
        else
            fprintf(stderr, " %sETA %.1fm%s", TUI_DIM, eta / 60, TUI_RESET);
    }
    fprintf(stderr, "\n");

    pthread_mutex_unlock(&mp->mutex);
}

void tui_multi_progress_destroy(tui_multi_progress_t *mp) {
    pthread_mutex_destroy(&mp->mutex);
}

/* ── Event Bus ───────────────────────────────────────────────────────── */

void tui_event_bus_init(tui_event_bus_t *bus) {
    memset(bus, 0, sizeof(*bus));
    pthread_mutex_init(&bus->mutex, NULL);
}

int tui_event_subscribe(tui_event_bus_t *bus, tui_event_type_t type, tui_event_handler_fn handler,
                        void *ctx) {
    pthread_mutex_lock(&bus->mutex);
    if (bus->sub_count >= TUI_EVT_SUBS_MAX) {
        pthread_mutex_unlock(&bus->mutex);
        return -1;
    }
    int idx = bus->sub_count++;
    bus->subs[idx].type = type;
    bus->subs[idx].handler = handler;
    bus->subs[idx].ctx = ctx;
    bus->subs[idx].active = true;
    pthread_mutex_unlock(&bus->mutex);
    return idx;
}

void tui_event_unsubscribe(tui_event_bus_t *bus, int sub_id) {
    pthread_mutex_lock(&bus->mutex);
    if (sub_id >= 0 && sub_id < bus->sub_count) {
        bus->subs[sub_id].active = false;
    }
    pthread_mutex_unlock(&bus->mutex);
}

void tui_event_emit(tui_event_bus_t *bus, const tui_event_t *event) {
    pthread_mutex_lock(&bus->mutex);

    /* Record in history ring buffer */
    int h = bus->history_head;
    bus->history[h] = *event;
    bus->history[h].timestamp = tui_now();
    bus->history_head = (h + 1) % 64;
    if (bus->history_count < 64)
        bus->history_count++;

    /* Dispatch to subscribers */
    for (int i = 0; i < bus->sub_count; i++) {
        if (bus->subs[i].active && bus->subs[i].type == event->type) {
            bus->subs[i].handler(event, bus->subs[i].ctx);
        }
    }
    pthread_mutex_unlock(&bus->mutex);
}

void tui_event_emit_simple(tui_event_bus_t *bus, tui_event_type_t type, const char *source) {
    tui_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.source = source;
    e.timestamp = tui_now();
    tui_event_emit(bus, &e);
}

void tui_event_bus_dump(const tui_event_bus_t *bus, int max_events) {
    static const char *evt_names[] = {
        "stream_start", "stream_text",  "stream_end",    "thinking_start",
        "thinking_end", "tool_start",   "tool_complete", "tool_error",
        "turn_start",   "turn_end",     "progress",      "context_pressure",
        "cost_update",  "session_load", "session_save",  "compact",
        "retry",        "error",        "custom"};

    fprintf(stderr,
            "  %s\xe2\x94\x80\xe2\x94\x80 event bus "
            "(%d subs, %d events) \xe2\x94\x80\xe2\x94\x80%s\n",
            TUI_DIM, bus->sub_count, bus->history_count, TUI_RESET);

    int start = bus->history_count < 64 ? 0 : bus->history_head;
    int total = bus->history_count < max_events ? bus->history_count : max_events;

    for (int i = 0; i < total; i++) {
        int idx = (start + bus->history_count - total + i) % 64;
        const tui_event_t *e = &bus->history[idx];
        const char *ename = (e->type < TUI_EVT__COUNT) ? evt_names[e->type] : "?";
        fprintf(stderr, "  %s%.3f%s %s%-18s%s %s%s%s\n", TUI_DIM,
                e->timestamp - bus->history[start].timestamp, TUI_RESET, TUI_CYAN, ename, TUI_RESET,
                TUI_DIM, e->source ? e->source : "", TUI_RESET);
    }
}

void tui_event_bus_destroy(tui_event_bus_t *bus) {
    pthread_mutex_destroy(&bus->mutex);
}

/* ── Streaming Display Pipeline ──────────────────────────────────────── */

void tui_stream_state_init(tui_stream_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
    ss->phase = TUI_STREAM_IDLE;
}

void tui_stream_state_transition(tui_stream_state_t *ss, tui_stream_phase_t new_phase) {
    ss->phase = new_phase;
    ss->phase_start = tui_now();
}

void tui_stream_state_token(tui_stream_state_t *ss, int count) {
    ss->text_tokens += count;
    ss->tokens_this_second += count;

    double now = tui_now();
    if (now - ss->last_second >= 1.0) {
        double tps = ss->tokens_this_second / (now - ss->last_second);
        if (tps > ss->peak_tok_per_sec)
            ss->peak_tok_per_sec = tps;
        ss->avg_tok_per_sec =
            (ss->avg_tok_per_sec * ss->sample_count + tps) / (ss->sample_count + 1);
        ss->sample_count++;
        ss->tokens_this_second = 0;
        ss->last_second = now;
    }
}

const char *tui_stream_phase_name(tui_stream_phase_t phase) {
    switch (phase) {
        case TUI_STREAM_IDLE:
            return "idle";
        case TUI_STREAM_THINKING:
            return "thinking";
        case TUI_STREAM_TEXT:
            return "text";
        case TUI_STREAM_TOOL_PENDING:
            return "tool_pending";
        case TUI_STREAM_TOOL_RUNNING:
            return "tool_running";
        case TUI_STREAM_TOOL_COMPLETE:
            return "tool_complete";
        case TUI_STREAM_DONE:
            return "done";
        case TUI_STREAM_ERROR:
            return "error";
        default:
            return "?";
    }
}

tui_stream_phase_t tui_stream_state_phase(const tui_stream_state_t *ss) {
    return ss->phase;
}

void tui_stream_state_render_badge(const tui_stream_state_t *ss) {
    const char *name = tui_stream_phase_name(ss->phase);
    const char *color = TUI_DIM;
    switch (ss->phase) {
        case TUI_STREAM_THINKING:
            color = TUI_MAGENTA;
            break;
        case TUI_STREAM_TEXT:
            color = TUI_GREEN;
            break;
        case TUI_STREAM_TOOL_PENDING:
            color = TUI_YELLOW;
            break;
        case TUI_STREAM_TOOL_RUNNING:
            color = TUI_CYAN;
            break;
        case TUI_STREAM_ERROR:
            color = TUI_RED;
            break;
        default:
            break;
    }

    double elapsed = tui_now() - ss->phase_start;
    fprintf(stderr, "  %s[%s %.1fs]%s", color, name, elapsed, TUI_RESET);

    if (ss->text_tokens > 0) {
        fprintf(stderr, " %s%d tok%s", TUI_DIM, ss->text_tokens, TUI_RESET);
    }
    if (ss->tool_count > 0) {
        fprintf(stderr, " %s%d tools%s", TUI_DIM, ss->tool_count, TUI_RESET);
        if (ss->tool_errors > 0) {
            fprintf(stderr, " %s(%d err)%s", TUI_RED, ss->tool_errors, TUI_RESET);
        }
    }
    if (ss->peak_tok_per_sec > 0) {
        fprintf(stderr, " %speak:%.0f tok/s%s", TUI_DIM, ss->peak_tok_per_sec, TUI_RESET);
    }
    fprintf(stderr, "\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Stream Heartbeat — anti-hang visual feedback
 *  Gorgeous animated indicator that auto-detects silent streams.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Phase icons — resolved from glyph tier ──────────────────────────── */
static const char *hb_phase_icon(const char *phase) {
    const tui_glyphs_t *g = tui_glyph();
    if (!phase || !*phase)
        return g->diamond;
    if (strstr(phase, "think"))
        return g->icon_think;
    if (strstr(phase, "fallback"))
        return g->arrow_cycle;
    if (strstr(phase, "receiv"))
        return g->icon_lightning;
    if (strstr(phase, "tool"))
        return g->icon_gear;
    return g->diamond;
}

/* ── Format bytes nicely ────────────────────────────────────────────── */
static void hb_format_bytes(char *out, int outsz, unsigned long bytes) {
    if (bytes < 1024)
        snprintf(out, outsz, "%luB", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, outsz, "%.1fKB", (double)bytes / 1024.0);
    else
        snprintf(out, outsz, "%.1fMB", (double)bytes / (1024.0 * 1024.0));
}

/* ── Format elapsed with sub-second precision ───────────────────────── */
static void hb_format_elapsed(char *out, int outsz, double secs) {
    if (secs < 10.0)
        snprintf(out, outsz, "%.1fs", secs);
    else if (secs < 60.0)
        snprintf(out, outsz, "%.0fs", secs);
    else if (secs < 3600.0)
        snprintf(out, outsz, "%dm%02ds", (int)(secs / 60.0), (int)secs % 60);
    else
        snprintf(out, outsz, "%dh%02dm", (int)(secs / 3600.0), ((int)secs % 3600) / 60);
}

static void *stream_heartbeat_thread(void *arg) {
    tui_stream_heartbeat_t *hb = (tui_stream_heartbeat_t *)arg;
    int frame = 0;

    while (1) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000000}; /* 200ms */
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&hb->mutex);
        bool running = hb->running;
        double now = tui_now_sec();
        double silence = now - hb->last_poke;
        double elapsed = now - hb->start_time;
        double thresh = hb->silence_thresh;
        unsigned long bytes = hb->bytes_recv;
        const char *phase = hb->phase_label;
        bool was_visible = hb->visible;
        pthread_mutex_unlock(&hb->mutex);

        if (!running) {
            if (was_visible) {
                tui_term_lock();
                fprintf(stderr, "\r\033[2K");
                fflush(stderr);
                tui_term_unlock();
            }
            break;
        }

        if (silence >= thresh) {
            /* Resolve label */
            const char *icon = hb_phase_icon(phase);
            const char *label;
            if (phase && *phase)
                label = phase;
            else if (bytes > 0)
                label = "streaming";
            else
                label = "connecting";

            /* Animated dots */
            const char *dots[] = {".", "..", "..."};

            char elapsed_str[32];
            hb_format_elapsed(elapsed_str, sizeof(elapsed_str), elapsed);

            /* Render inline on current line */
            tui_term_lock();
            fprintf(stderr, "\r\033[2K");
            fprintf(stderr, "  " TUI_DIM "%s %s%s  %s", icon, label, dots[frame % 3], elapsed_str);
            if (bytes > 0) {
                char bytes_str[32];
                hb_format_bytes(bytes_str, sizeof(bytes_str), bytes);
                fprintf(stderr, " · %s", bytes_str);
            }
            fprintf(stderr, TUI_RESET);
            fflush(stderr);
            tui_term_unlock();

            pthread_mutex_lock(&hb->mutex);
            hb->visible = true;
            pthread_mutex_unlock(&hb->mutex);

            frame++;
        } else if (was_visible) {
            /* Content resumed — clear heartbeat from current line */
            tui_term_lock();
            fprintf(stderr, "\r\033[2K");
            fflush(stderr);
            tui_term_unlock();

            pthread_mutex_lock(&hb->mutex);
            hb->visible = false;
            pthread_mutex_unlock(&hb->mutex);
        }
    }
    return NULL;
}

void tui_stream_heartbeat_start(tui_stream_heartbeat_t *hb) {
    memset(hb, 0, sizeof(*hb));
    pthread_mutex_init(&hb->mutex, NULL);
    hb->running = true;
    hb->visible = false;
    double now = tui_now_sec();
    hb->last_poke = now;
    hb->start_time = now;
    hb->silence_thresh = 2.0;
    hb->bytes_recv = 0;
    hb->phase_label = NULL;
    hb->poke_count = 0;
    hb->phase_changes = 0;
    pthread_create(&hb->thread, NULL, stream_heartbeat_thread, hb);
}

void tui_stream_heartbeat_poke(tui_stream_heartbeat_t *hb, const char *phase) {
    pthread_mutex_lock(&hb->mutex);
    hb->last_poke = tui_now_sec();
    bool was_visible = hb->visible;
    if (was_visible)
        hb->visible = false;
    if (phase != hb->phase_label) {
        hb->phase_changes++;
        hb->phase_label = phase;
    }
    hb->poke_count++;
    pthread_mutex_unlock(&hb->mutex);

    /* Clear inline heartbeat before caller writes content */
    if (was_visible) {
        tui_term_lock();
        fprintf(stderr, "\r\033[2K");
        fflush(stderr);
        tui_term_unlock();
    }
}

void tui_stream_heartbeat_recv(tui_stream_heartbeat_t *hb, size_t bytes) {
    pthread_mutex_lock(&hb->mutex);
    hb->bytes_recv += bytes;
    pthread_mutex_unlock(&hb->mutex);
}

void tui_stream_heartbeat_stop(tui_stream_heartbeat_t *hb) {
    pthread_mutex_lock(&hb->mutex);
    hb->running = false;
    pthread_mutex_unlock(&hb->mutex);

    pthread_join(hb->thread, NULL);
    pthread_mutex_destroy(&hb->mutex);
    hb->visible = false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SUBSTRUCTURAL IMPROVEMENTS — Output Serialization & Coordination
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Global instances ───────────────────────────────────────────────── */

tui_output_queue_t *g_outq = NULL;
tui_anim_clock_t *g_anim_clock = NULL;

/* ── Serialized Output Queue ────────────────────────────────────────── */

static void *outq_render_thread(void *arg) {
    tui_output_queue_t *q = (tui_output_queue_t *)arg;

    while (1) {
        /* Wait for entries or shutdown */
        pthread_mutex_lock(&q->mutex);
        while (q->count == 0 && q->running) {
            /* Wait with timeout for periodic flush */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += TUI_OUTQ_TICK_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
        }

        if (!q->running && q->count == 0) {
            pthread_mutex_unlock(&q->mutex);
            break;
        }

        /* Drain all entries under lock */
        double flush_start = tui_now_sec();
        int flushed = 0;

        while (q->count > 0) {
            tui_out_entry_t *e = &q->ring[q->tail];

            switch (e->type) {
                case TUI_OUT_TEXT:
                    fwrite(e->data, 1, e->len, q->out);
                    break;
                case TUI_OUT_CLEAR_LINE:
                    fprintf(q->out, "\r\033[K");
                    break;
                case TUI_OUT_CURSOR_MOVE:
                    fprintf(q->out, "%s", e->data);
                    break;
                case TUI_OUT_STYLE:
                    fprintf(q->out, "%s", e->data);
                    break;
                case TUI_OUT_TITLE:
                    fprintf(q->out, "\033]2;%s\a", e->data);
                    break;
                case TUI_OUT_BELL:
                    fprintf(q->out, "\a");
                    break;
                case TUI_OUT_FLUSH:
                    break; /* just triggers the drain */
            }

            q->tail = (q->tail + 1) % q->capacity;
            q->count--;
            flushed++;
        }

        if (flushed > 0) {
            fflush(q->out);
            q->total_flushes++;
            q->total_writes += flushed;

            double flush_ms = (tui_now_sec() - flush_start) * 1000.0;
            q->total_flush_ms += flush_ms;
            if (flush_ms > q->max_flush_ms)
                q->max_flush_ms = flush_ms;
        }

        pthread_mutex_unlock(&q->mutex);
    }
    return NULL;
}

void tui_outq_init(tui_output_queue_t *q, FILE *out) {
    memset(q, 0, sizeof(*q));
    q->capacity = TUI_OUTQ_SIZE;
    q->ring = calloc(q->capacity, sizeof(tui_out_entry_t));
    q->out = out ? out : stderr;
    q->running = true;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    pthread_create(&q->render_thread, NULL, outq_render_thread, q);
}

void tui_outq_destroy(tui_output_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->running = false;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    pthread_join(q->render_thread, NULL);

    /* Drain any remaining entries directly */
    while (q->count > 0) {
        tui_out_entry_t *e = &q->ring[q->tail];
        if (e->type == TUI_OUT_TEXT)
            fwrite(e->data, 1, e->len, q->out);
        q->tail = (q->tail + 1) % q->capacity;
        q->count--;
    }
    fflush(q->out);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
    free(q->ring);
    memset(q, 0, sizeof(*q));
}

static void outq_enqueue(tui_output_queue_t *q, tui_out_type_t type, int priority, const char *data,
                         int len) {
    pthread_mutex_lock(&q->mutex);
    if (q->count >= q->capacity) {
        q->dropped++;
        pthread_mutex_unlock(&q->mutex);
        return;
    }
    tui_out_entry_t *e = &q->ring[q->head];
    e->type = type;
    e->priority = priority;
    if (data && len > 0) {
        int n = len < TUI_OUTQ_MSG_MAX - 1 ? len : TUI_OUTQ_MSG_MAX - 1;
        memcpy(e->data, data, n);
        e->data[n] = '\0';
        e->len = n;
    } else {
        e->data[0] = '\0';
        e->len = 0;
    }
    q->head = (q->head + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void tui_outq_write(tui_output_queue_t *q, const char *text) {
    if (!q || !text)
        return;
    outq_enqueue(q, TUI_OUT_TEXT, 0, text, (int)strlen(text));
}

void tui_outq_writef(tui_output_queue_t *q, const char *fmt, ...) {
    if (!q || !fmt)
        return;
    char buf[TUI_OUTQ_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        outq_enqueue(q, TUI_OUT_TEXT, 0, buf, n);
}

void tui_outq_write_pri(tui_output_queue_t *q, int priority, const char *text) {
    if (!q || !text)
        return;
    outq_enqueue(q, TUI_OUT_TEXT, priority, text, (int)strlen(text));
}

void tui_outq_clear_line(tui_output_queue_t *q) {
    if (!q)
        return;
    outq_enqueue(q, TUI_OUT_CLEAR_LINE, 10, NULL, 0);
}

void tui_outq_flush_sync(tui_output_queue_t *q) {
    if (!q)
        return;
    outq_enqueue(q, TUI_OUT_FLUSH, 100, NULL, 0);
    /* Spin until drained (simple busy-wait, typically <1ms) */
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&q->mutex);
        int cnt = q->count;
        pthread_mutex_unlock(&q->mutex);
        if (cnt == 0)
            break;
        usleep(100);
    }
}

void tui_outq_stats(const tui_output_queue_t *q, int *total_writes, int *total_flushes,
                    int *dropped, double *avg_flush_ms) {
    if (!q)
        return;
    if (total_writes)
        *total_writes = q->total_writes;
    if (total_flushes)
        *total_flushes = q->total_flushes;
    if (dropped)
        *dropped = q->dropped;
    if (avg_flush_ms) {
        *avg_flush_ms = q->total_flushes > 0 ? q->total_flush_ms / q->total_flushes : 0.0;
    }
}

/* ── Streaming FSM Wiring ───────────────────────────────────────────── */

void tui_streaming_fsm_create(tui_fsm_t *fsm, void *ctx) {
    tui_fsm_init(fsm, "streaming", ctx);

    /* States: idle, thinking, text, tool_pending, tool_running, tool_complete, done, error */
    int s_idle = tui_fsm_add_state(fsm, "idle", NULL, NULL, NULL);
    int s_thinking = tui_fsm_add_state(fsm, "thinking", NULL, NULL, NULL);
    int s_text = tui_fsm_add_state(fsm, "text", NULL, NULL, NULL);
    int s_tool_p = tui_fsm_add_state(fsm, "tool_pending", NULL, NULL, NULL);
    int s_tool_r = tui_fsm_add_state(fsm, "tool_running", NULL, NULL, NULL);
    int s_tool_c = tui_fsm_add_state(fsm, "tool_complete", NULL, NULL, NULL);
    int s_done = tui_fsm_add_state(fsm, "done", NULL, NULL, NULL);
    int s_error = tui_fsm_add_state(fsm, "error", NULL, NULL, NULL);

    /* Transitions — idle */
    tui_fsm_add_transition(fsm, s_idle, s_thinking, TUI_FSM_EVT_THINKING_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_idle, s_text, TUI_FSM_EVT_TEXT_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_idle, s_tool_p, TUI_FSM_EVT_TOOL_START, NULL, NULL);

    /* Transitions — thinking */
    tui_fsm_add_transition(fsm, s_thinking, s_text, TUI_FSM_EVT_THINKING_END, NULL, NULL);
    tui_fsm_add_transition(fsm, s_thinking, s_text, TUI_FSM_EVT_TEXT_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_thinking, s_tool_p, TUI_FSM_EVT_TOOL_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_thinking, s_done, TUI_FSM_EVT_STREAM_END, NULL, NULL);

    /* Transitions — text */
    tui_fsm_add_transition(fsm, s_text, s_thinking, TUI_FSM_EVT_THINKING_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_text, s_tool_p, TUI_FSM_EVT_TOOL_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_text, s_done, TUI_FSM_EVT_STREAM_END, NULL, NULL);

    /* Transitions — tool states */
    tui_fsm_add_transition(fsm, s_tool_p, s_tool_r, TUI_FSM_EVT_TOOL_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_p, s_text, TUI_FSM_EVT_TEXT_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_p, s_thinking, TUI_FSM_EVT_THINKING_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_p, s_done, TUI_FSM_EVT_STREAM_END, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_r, s_tool_c, TUI_FSM_EVT_TOOL_COMPLETE, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_r, s_done, TUI_FSM_EVT_STREAM_END, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_c, s_text, TUI_FSM_EVT_TEXT_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_c, s_tool_p, TUI_FSM_EVT_TOOL_START, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_c, s_done, TUI_FSM_EVT_STREAM_END, NULL, NULL);

    /* Transitions — done (reset for next turn) */
    tui_fsm_add_transition(fsm, s_done, s_idle, TUI_FSM_EVT_STREAM_START, NULL, NULL);

    /* Error transitions from any state */
    for (int i = 0; i < fsm->state_count; i++) {
        if (i != s_error)
            tui_fsm_add_transition(fsm, i, s_error, TUI_FSM_EVT_ERROR, NULL, NULL);
    }
    tui_fsm_add_transition(fsm, s_error, s_idle, TUI_FSM_EVT_RESUME, NULL, NULL);

    /* Interrupt transitions from active states */
    tui_fsm_add_transition(fsm, s_thinking, s_idle, TUI_FSM_EVT_INTERRUPT, NULL, NULL);
    tui_fsm_add_transition(fsm, s_text, s_idle, TUI_FSM_EVT_INTERRUPT, NULL, NULL);
    tui_fsm_add_transition(fsm, s_tool_r, s_idle, TUI_FSM_EVT_INTERRUPT, NULL, NULL);

    (void)s_idle;
    (void)s_thinking;
    (void)s_text;
    (void)s_tool_p;
    (void)s_tool_r;
    (void)s_tool_c;
    (void)s_done;
    (void)s_error;
}

/* ── Animation Clock ────────────────────────────────────────────────── */

static void *anim_clock_thread(void *arg) {
    tui_anim_clock_t *clk = (tui_anim_clock_t *)arg;

    while (1) {
        pthread_mutex_lock(&clk->mutex);
        if (!clk->running) {
            pthread_mutex_unlock(&clk->mutex);
            break;
        }

        /* Count active keep_alive subscribers */
        int active = 0;
        for (int i = 0; i < clk->sub_count; i++) {
            if (clk->subs[i].active && clk->subs[i].keep_alive)
                active++;
        }
        clk->active_count = active;

        if (active == 0) {
            /* No active subscribers — sleep longer to save CPU */
            pthread_mutex_unlock(&clk->mutex);
            usleep(100000); /* 100ms when idle */
            continue;
        }

        /* Update synchronized tick time */
        clk->tick_time = (tui_now_sec() - clk->start_time) * 1000.0;

        /* Dispatch to all active subscribers */
        double tick_start = tui_now_sec();
        for (int i = 0; i < clk->sub_count; i++) {
            tui_anim_sub_t *s = &clk->subs[i];
            if (s->active && s->callback) {
                s->callback(clk->tick_time, s->ctx);
            }
        }

        double tick_ms = (tui_now_sec() - tick_start) * 1000.0;
        if (tick_ms > clk->max_tick_ms)
            clk->max_tick_ms = tick_ms;
        clk->total_ticks++;

        pthread_mutex_unlock(&clk->mutex);

        /* Sleep for interval, subtracting tick duration */
        int sleep_us = clk->interval_ms * 1000 - (int)(tick_ms * 1000);
        if (sleep_us < 1000)
            sleep_us = 1000; /* min 1ms */
        usleep(sleep_us);
    }
    return NULL;
}

void tui_anim_clock_init(tui_anim_clock_t *clk, int interval_ms) {
    memset(clk, 0, sizeof(*clk));
    clk->interval_ms = interval_ms > 0 ? interval_ms : TUI_OUTQ_TICK_MS;
    clk->start_time = tui_now_sec();
    clk->running = true;
    pthread_mutex_init(&clk->mutex, NULL);
    pthread_create(&clk->thread, NULL, anim_clock_thread, clk);
}

void tui_anim_clock_destroy(tui_anim_clock_t *clk) {
    pthread_mutex_lock(&clk->mutex);
    clk->running = false;
    pthread_mutex_unlock(&clk->mutex);
    pthread_join(clk->thread, NULL);
    pthread_mutex_destroy(&clk->mutex);
}

int tui_anim_subscribe(tui_anim_clock_t *clk, tui_anim_cb callback, void *ctx, bool keep_alive) {
    pthread_mutex_lock(&clk->mutex);
    if (clk->sub_count >= TUI_ANIM_MAX_SUBS) {
        pthread_mutex_unlock(&clk->mutex);
        return -1;
    }
    int id = clk->sub_count++;
    clk->subs[id].callback = callback;
    clk->subs[id].ctx = ctx;
    clk->subs[id].active = true;
    clk->subs[id].keep_alive = keep_alive;
    if (keep_alive)
        clk->active_count++;
    pthread_mutex_unlock(&clk->mutex);
    return id;
}

void tui_anim_unsubscribe(tui_anim_clock_t *clk, int sub_id) {
    pthread_mutex_lock(&clk->mutex);
    if (sub_id >= 0 && sub_id < clk->sub_count) {
        if (clk->subs[sub_id].active && clk->subs[sub_id].keep_alive)
            clk->active_count--;
        clk->subs[sub_id].active = false;
        clk->subs[sub_id].keep_alive = false;
        clk->subs[sub_id].callback = NULL;
    }
    pthread_mutex_unlock(&clk->mutex);
}

void tui_anim_set_active(tui_anim_clock_t *clk, int sub_id, bool active) {
    pthread_mutex_lock(&clk->mutex);
    if (sub_id >= 0 && sub_id < clk->sub_count) {
        bool was_active = clk->subs[sub_id].active;
        clk->subs[sub_id].active = active;
        if (clk->subs[sub_id].keep_alive) {
            if (!was_active && active)
                clk->active_count++;
            else if (was_active && !active)
                clk->active_count--;
        }
    }
    pthread_mutex_unlock(&clk->mutex);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Claude Code-inspired UI systems — pure C / ANSI escapes
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Style Intern Table ─────────────────────────────────────────────── */

static tui_style_entry_t s_styles[TUI_MAX_STYLES];
static int s_style_count = 1; /* 0 = default (reset) */

static void style_intern_init(void) {
    memset(&s_styles[0], 0, sizeof(s_styles[0]));
    snprintf(s_styles[0].ansi, sizeof(s_styles[0].ansi), "%s", TUI_RESET);
    s_styles[0].len = (int)strlen(s_styles[0].ansi);
}

int tui_style_intern(const char *ansi_seq) {
    if (!ansi_seq || !ansi_seq[0])
        return 0;
    /* Search existing */
    for (int i = 0; i < s_style_count; i++) {
        if (strcmp(s_styles[i].ansi, ansi_seq) == 0)
            return i;
    }
    /* Add new */
    if (s_style_count >= TUI_MAX_STYLES)
        return 0;
    int id = s_style_count++;
    snprintf(s_styles[id].ansi, sizeof(s_styles[id].ansi), "%s", ansi_seq);
    s_styles[id].len = (int)strlen(s_styles[id].ansi);
    return id;
}

const char *tui_style_get(int style_id) {
    if (style_id < 0 || style_id >= s_style_count)
        return TUI_RESET;
    return s_styles[style_id].ansi;
}

/* ── UTF-8 Decode ───────────────────────────────────────────────────── */

int tui_utf8_decode(const char *s, unsigned int *codepoint) {
    if (!s || !s[0]) {
        *codepoint = 0;
        return 0;
    }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *codepoint = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        *codepoint = (c & 0x1F) << 6;
        if ((s[1] & 0xC0) != 0x80) {
            *codepoint = 0xFFFD;
            return 1;
        }
        *codepoint |= (s[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0) {
        *codepoint = (c & 0x0F) << 12;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
            *codepoint = 0xFFFD;
            return 1;
        }
        *codepoint |= ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0) {
        *codepoint = (c & 0x07) << 18;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *codepoint = 0xFFFD;
            return 1;
        }
        *codepoint |= ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *codepoint = 0xFFFD;
    return 1;
}

/* ── Wide Character Width Detection ─────────────────────────────────── */
/* Based on Unicode East Asian Width + emoji properties.
 * Covers CJK Unified Ideographs, Hangul, fullwidth forms, and emoji. */

int tui_char_width(unsigned int cp) {
    /* C0/C1 control characters */
    if (cp == 0)
        return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;

    /* Combining marks (zero width) */
    if ((cp >= 0x0300 && cp <= 0x036F) ||   /* Combining Diacriticals */
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||   /* Combining Diacriticals Extended */
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||   /* Combining Diacriticals Supplement */
        (cp >= 0x20D0 && cp <= 0x20FF) ||   /* Combining for Symbols */
        (cp >= 0xFE00 && cp <= 0xFE0F) ||   /* Variation Selectors */
        (cp >= 0xFE20 && cp <= 0xFE2F) ||   /* Combining Half Marks */
        (cp >= 0xE0100 && cp <= 0xE01EF) || /* Variation Selectors Supplement */
        cp == 0x200B || cp == 0x200C ||     /* Zero-width space, ZWNJ */
        cp == 0x200D || cp == 0xFEFF)       /* ZWJ, BOM */
        return 0;

    /* Soft hyphen */
    if (cp == 0x00AD)
        return 1;

    /* East Asian Wide & Fullwidth */
    if ((cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo */
        cp == 0x2329 || cp == 0x232A ||     /* Angle brackets */
        (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK Radicals Supplement..Ideographic Description */
        (cp >= 0x3040 && cp <= 0x33BF) ||   /* Hiragana..CJK Compatibility */
        (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK Unified Ideographs Extension A */
        (cp >= 0x4E00 && cp <= 0xA4CF) ||   /* CJK Unified Ideographs..Yi Radicals */
        (cp >= 0xA960 && cp <= 0xA97F) ||   /* Hangul Jamo Extended-A */
        (cp >= 0xAC00 && cp <= 0xD7FF) ||   /* Hangul Syllables + Jamo Extended-B */
        (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK Compatibility Ideographs */
        (cp >= 0xFE10 && cp <= 0xFE19) ||   /* Vertical Forms */
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   /* CJK Compatibility Forms */
        (cp >= 0xFF01 && cp <= 0xFF60) ||   /* Fullwidth Forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   /* Fullwidth Signs */
        (cp >= 0x20000 && cp <= 0x2FFFD) || /* CJK Unified Ideographs Extension B-F */
        (cp >= 0x30000 && cp <= 0x3FFFD))   /* CJK Extension G+ */
        return 2;

    /* Emoji that are typically rendered wide */
    if ((cp >= 0x1F300 &&
         cp <= 0x1F9FF) || /* Miscellaneous Symbols & Pictographs..Supplemental Symbols */
        (cp >= 0x1FA00 && cp <= 0x1FA6F) || /* Chess Symbols */
        (cp >= 0x1FA70 && cp <= 0x1FAFF) || /* Symbols and Pictographs Extended-A */
        (cp >= 0x231A && cp <= 0x231B) ||   /* Watch, Hourglass */
        (cp >= 0x23E9 && cp <= 0x23F3) ||   /* Various play/pause/etc */
        (cp >= 0x2600 && cp <= 0x27BF) ||   /* Misc symbols, Dingbats */
        (cp >= 0x2934 && cp <= 0x2935) ||   /* Arrows */
        (cp >= 0x2B05 && cp <= 0x2B07) ||   /* Arrows */
        cp == 0x2B1B ||
        cp == 0x2B1C || cp == 0x2B50 || cp == 0x2B55 || cp == 0x3030 || cp == 0x303D ||
        cp == 0x3297 || cp == 0x3299)
        return 2;

    return 1;
}

int tui_str_display_width(const char *s) {
    if (!s)
        return 0;
    int width = 0;
    while (*s) {
        /* Skip ANSI escape sequences */
        if (*s == '\033') {
            s++;
            if (*s == '[') {
                s++;
                while (*s && !(*s >= 'A' && *s <= 'Z') && *s != 'm' && !(*s >= 'a' && *s <= 'z'))
                    s++;
                if (*s)
                    s++;
            } else if (*s == ']') {
                /* OSC sequence — skip to ST (BEL or ESC\) */
                s++;
                while (*s && *s != '\a' && !(*s == '\033' && *(s + 1) == '\\'))
                    s++;
                if (*s == '\a')
                    s++;
                else if (*s == '\033')
                    s += 2;
            }
            continue;
        }
        unsigned int cp;
        int bytes = tui_utf8_decode(s, &cp);
        width += tui_char_width(cp);
        s += bytes;
    }
    return width;
}

int tui_utf8_truncate(const char *src, char *dst, size_t dst_len, int max_width) {
    if (!src || !dst || dst_len == 0)
        return 0;
    int width = 0;
    const char *p = src;
    char *d = dst;
    char *d_end = dst + dst_len - 4; /* reserve for ellipsis */

    while (*p && d < d_end) {
        /* Pass through ANSI escape sequences */
        if (*p == '\033') {
            const char *esc_start = p;
            p++;
            if (*p == '[') {
                p++;
                while (*p && !(*p >= 'A' && *p <= 'Z') && *p != 'm' && !(*p >= 'a' && *p <= 'z'))
                    p++;
                if (*p)
                    p++;
            }
            int esc_len = (int)(p - esc_start);
            if (d + esc_len < dst + dst_len) {
                memcpy(d, esc_start, esc_len);
                d += esc_len;
            }
            continue;
        }

        unsigned int cp;
        int bytes = tui_utf8_decode(p, &cp);
        int cw = tui_char_width(cp);
        if (width + cw > max_width) {
            /* Add ellipsis */
            if (d + 3 < dst + dst_len) {
                *d++ = '.';
                *d++ = '.';
                *d++ = '.';
            }
            break;
        }
        if (d + bytes < dst + dst_len) {
            memcpy(d, p, bytes);
            d += bytes;
        }
        width += cw;
        p += bytes;
    }
    *d = '\0';
    return width;
}

/* ── Screen Buffer ──────────────────────────────────────────────────── */

static void cell_clear(tui_cell_t *c) {
    c->ch[0] = ' ';
    c->ch[1] = '\0';
    c->style_id = 0;
    c->width = 1;
}

void tui_screenbuf_init(tui_screenbuf_t *sb, int width, int height, FILE *out) {
    if (width > SCRBUF_MAX_WIDTH)
        width = SCRBUF_MAX_WIDTH;
    if (height > SCRBUF_MAX_HEIGHT)
        height = SCRBUF_MAX_HEIGHT;
    sb->width = width;
    sb->height = height;
    sb->out = out;
    sb->cursor_x = sb->cursor_y = 0;

    int total = width * height;
    sb->cells = calloc(total, sizeof(tui_cell_t));
    sb->prev = calloc(total, sizeof(tui_cell_t));
    sb->dirty = calloc(total, sizeof(bool));
    for (int i = 0; i < total; i++) {
        cell_clear(&sb->cells[i]);
        cell_clear(&sb->prev[i]);
        sb->dirty[i] = true;
    }
    style_intern_init();
}

void tui_screenbuf_free(tui_screenbuf_t *sb) {
    free(sb->cells);
    free(sb->prev);
    free(sb->dirty);
    memset(sb, 0, sizeof(*sb));
}

void tui_screenbuf_resize(tui_screenbuf_t *sb, int width, int height) {
    tui_screenbuf_free(sb);
    tui_screenbuf_init(sb, width, height, sb->out ? sb->out : stderr);
}

void tui_screenbuf_clear(tui_screenbuf_t *sb) {
    int total = sb->width * sb->height;
    for (int i = 0; i < total; i++) {
        cell_clear(&sb->cells[i]);
        sb->dirty[i] = true;
    }
}

void tui_screenbuf_put(tui_screenbuf_t *sb, int x, int y, const char *ch, int style_id,
                       int char_width) {
    if (x < 0 || x >= sb->width || y < 0 || y >= sb->height)
        return;
    int idx = y * sb->width + x;
    tui_cell_t *c = &sb->cells[idx];
    snprintf(c->ch, sizeof(c->ch), "%s", ch ? ch : " ");
    c->style_id = style_id;
    c->width = (unsigned char)char_width;
    sb->dirty[idx] = true;

    /* For wide characters, mark continuation cell */
    if (char_width == 2 && x + 1 < sb->width) {
        tui_cell_t *c2 = &sb->cells[idx + 1];
        c2->ch[0] = '\0';
        c2->style_id = style_id;
        c2->width = 0; /* continuation */
        sb->dirty[idx + 1] = true;
    }
}

void tui_screenbuf_write(tui_screenbuf_t *sb, int x, int y, const char *text, int style_id) {
    if (!text)
        return;
    int col = x;
    const char *p = text;
    while (*p && col < sb->width) {
        /* Skip ANSI sequences in the text */
        if (*p == '\033') {
            while (*p && *p != 'm')
                p++;
            if (*p)
                p++;
            continue;
        }
        unsigned int cp;
        int bytes = tui_utf8_decode(p, &cp);
        int cw = tui_char_width(cp);
        if (col + cw > sb->width)
            break;
        char ch[8];
        int n = bytes < 7 ? bytes : 7;
        memcpy(ch, p, n);
        ch[n] = '\0';
        tui_screenbuf_put(sb, col, y, ch, style_id, cw);
        col += cw;
        p += bytes;
    }
}

void tui_screenbuf_flush(tui_screenbuf_t *sb) {
    int total = sb->width * sb->height;
    int last_style = -1;

    for (int y = 0; y < sb->height; y++) {
        bool row_dirty = false;
        for (int x = 0; x < sb->width; x++) {
            int idx = y * sb->width + x;
            if (sb->dirty[idx]) {
                row_dirty = true;
                break;
            }
        }
        if (!row_dirty)
            continue;

        for (int x = 0; x < sb->width; x++) {
            int idx = y * sb->width + x;
            if (!sb->dirty[idx])
                continue;

            tui_cell_t *c = &sb->cells[idx];
            tui_cell_t *p = &sb->prev[idx];

            /* Skip if same as previous frame */
            if (c->style_id == p->style_id && c->width == p->width && strcmp(c->ch, p->ch) == 0) {
                sb->dirty[idx] = false;
                continue;
            }

            /* Move cursor to position */
            fprintf(sb->out, "\033[%d;%dH", y + 1, x + 1);

            /* Apply style if changed */
            if (c->style_id != last_style) {
                fprintf(sb->out, "%s%s", TUI_RESET, tui_style_get(c->style_id));
                last_style = c->style_id;
            }

            /* Write character */
            if (c->width > 0 && c->ch[0]) {
                fprintf(sb->out, "%s", c->ch);
            }

            sb->dirty[idx] = false;
        }
    }

    /* Reset style */
    if (last_style != 0)
        fprintf(sb->out, "%s", TUI_RESET);
    fflush(sb->out);

    /* Copy current to previous */
    memcpy(sb->prev, sb->cells, total * sizeof(tui_cell_t));
}

/* ── OSC 8 Hyperlinks ───────────────────────────────────────────────── */

static int s_hyperlinks_detected = -1; /* -1 = not checked */

bool tui_supports_hyperlinks(void) {
    if (s_hyperlinks_detected >= 0)
        return s_hyperlinks_detected;
    /* Detect by terminal type. Most modern terminals support OSC 8. */
    const char *term = getenv("TERM_PROGRAM");
    const char *term_emu = getenv("TERM");
    s_hyperlinks_detected = 0;
    if (term) {
        if (strstr(term, "iTerm") || strstr(term, "WezTerm") || strstr(term, "Hyper") ||
            strstr(term, "kitty") || strstr(term, "Alacritty") || strstr(term, "ghostty") ||
            strstr(term, "vscode"))
            s_hyperlinks_detected = 1;
    }
    if (!s_hyperlinks_detected && term_emu) {
        if (strstr(term_emu, "xterm-256color") || strstr(term_emu, "tmux"))
            s_hyperlinks_detected = 1;
    }
    /* env override */
    const char *force = getenv("DSCO_HYPERLINKS");
    if (force)
        s_hyperlinks_detected = atoi(force);
    return s_hyperlinks_detected;
}

void tui_hyperlink(FILE *out, const char *url, const char *display_text) {
    if (!out || !display_text)
        return;
    if (tui_supports_hyperlinks() && url) {
        fprintf(out, "\033]8;;%s\a%s\033]8;;\a", url, display_text);
    } else {
        fprintf(out, "%s", display_text);
    }
}

void tui_file_link(FILE *out, const char *path, const char *display) {
    if (!path)
        return;
    char url[1024];
    snprintf(url, sizeof(url), "file://%s", path);
    tui_hyperlink(out, url, display ? display : path);
}

void tui_file_line_link(FILE *out, const char *path, int line, const char *display) {
    if (!path)
        return;
    char url[1024], disp[512];
    snprintf(url, sizeof(url), "file://%s#%d", path, line);
    if (!display) {
        snprintf(disp, sizeof(disp), "%s:%d", path, line);
        display = disp;
    }
    tui_hyperlink(out, url, display);
}

/* ── Terminal Title ─────────────────────────────────────────────────── */

void tui_set_title(const char *title) {
    if (!title)
        return;
    /* OSC 2: Set window title */
    fprintf(stderr, "\033]2;%s\a", title);
    fflush(stderr);
}

void tui_set_title_fmt(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_set_title(buf);
}

void tui_reset_title(void) {
    fprintf(stderr, "\033]2;\a");
    fflush(stderr);
}

/* ── Shimmer / Gradient Animation ───────────────────────────────────── */

static tui_rgb_t rgb_lerp(tui_rgb_t a, tui_rgb_t b, float t) {
    tui_rgb_t r;
    r.r = (unsigned char)(a.r + (b.r - a.r) * t);
    r.g = (unsigned char)(a.g + (b.g - a.g) * t);
    r.b = (unsigned char)(a.b + (b.b - a.b) * t);
    return r;
}

void tui_shimmer_text(FILE *out, const char *text, tui_rgb_t color_a, tui_rgb_t color_b) {
    if (!text || !out)
        return;
    int len = tui_str_display_width(text);
    if (len == 0)
        return;

    const char *p = text;
    int col = 0;
    while (*p) {
        if (*p == '\033') {
            while (*p && *p != 'm')
                fputc(*p++, out);
            if (*p)
                fputc(*p++, out);
            continue;
        }
        unsigned int cp;
        int bytes = tui_utf8_decode(p, &cp);
        float t = len > 1 ? (float)col / (float)(len - 1) : 0.5f;
        tui_rgb_t c = rgb_lerp(color_a, color_b, t);
        fprintf(out, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b);
        fwrite(p, 1, bytes, out);
        col += tui_char_width(cp);
        p += bytes;
    }
    fprintf(out, "%s", TUI_RESET);
}

static void *shimmer_thread_fn(void *arg) {
    tui_shimmer_t *sh = (tui_shimmer_t *)arg;
    const char *blocks[] = {"░", "▒", "▓", "█", "▓", "▒"};
    int nblocks = 6;

    while (1) {
        pthread_mutex_lock(&sh->mutex);
        bool running = sh->running;
        pthread_mutex_unlock(&sh->mutex);
        if (!running)
            break;

        /* Render shimmer line */
        double elapsed = tui_now_sec() - sh->start_time;
        int phase = (int)(elapsed * 8.0) % nblocks;

        fprintf(stderr, "\r  ");
        /* Shimmer blocks */
        for (int i = 0; i < 3; i++) {
            int bi = (phase + i) % nblocks;
            float t = (float)i / 2.0f;
            tui_rgb_t c = rgb_lerp(sh->color_a, sh->color_b, t);
            fprintf(stderr, "\033[38;2;%d;%d;%dm%s", c.r, c.g, c.b, blocks[bi]);
        }
        fprintf(stderr, "%s ", TUI_RESET);

        /* Label with gradient */
        if (sh->label)
            tui_shimmer_text(stderr, sh->label, sh->color_a, sh->color_b);

        /* Elapsed time */
        fprintf(stderr, " %s%.1fs%s ", TUI_DIM, elapsed, TUI_RESET);

        /* Trailing shimmer */
        for (int i = 2; i >= 0; i--) {
            int bi = (phase + 3 + i) % nblocks;
            float t = (float)i / 2.0f;
            tui_rgb_t c = rgb_lerp(sh->color_b, sh->color_a, t);
            fprintf(stderr, "\033[38;2;%d;%d;%dm%s", c.r, c.g, c.b, blocks[bi]);
        }
        fprintf(stderr, "%s\033[K", TUI_RESET); /* clear to end of line */
        fflush(stderr);

        usleep(80000); /* ~12.5 fps */
    }
    return NULL;
}

void tui_shimmer_start(tui_shimmer_t *sh, const char *label, tui_rgb_t color_a, tui_rgb_t color_b) {
    memset(sh, 0, sizeof(*sh));
    sh->label = label;
    sh->color_a = color_a;
    sh->color_b = color_b;
    sh->start_time = tui_now_sec();
    sh->running = true;
    pthread_mutex_init(&sh->mutex, NULL);
    pthread_create(&sh->thread, NULL, shimmer_thread_fn, sh);
}

void tui_shimmer_stop(tui_shimmer_t *sh) {
    pthread_mutex_lock(&sh->mutex);
    sh->running = false;
    pthread_mutex_unlock(&sh->mutex);
    pthread_join(sh->thread, NULL);
    pthread_mutex_destroy(&sh->mutex);
    /* Clear shimmer line */
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
}

/* ── Permission Prompt Dialog ───────────────────────────────────────── */

#include <termios.h>

static int read_single_char(void) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 1;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    int ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

tui_perm_result_t tui_permission_prompt(const char *tool_name, const char *description,
                                        const char *detail) {
    int w = tui_term_width();
    if (w > 80)
        w = 80;
    const tui_glyphs_t *g = tui_glyph();

    /* Top border */
    fprintf(stderr, "\n  %s%s╭", TUI_YELLOW, TUI_BOLD);
    for (int i = 0; i < w - 6; i++)
        fprintf(stderr, "─");
    fprintf(stderr, "╮%s\n", TUI_RESET);

    /* Title line */
    fprintf(stderr, "  %s%s│%s %s%s Tool Permission %s", TUI_YELLOW, TUI_BOLD, TUI_RESET, TUI_BOLD,
            TUI_YELLOW, TUI_RESET);
    int title_vis = 18; /* " Tool Permission " */
    for (int i = 0; i < w - 6 - title_vis; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "%s%s│%s\n", TUI_YELLOW, TUI_BOLD, TUI_RESET);

    /* Separator */
    fprintf(stderr, "  %s│", TUI_YELLOW);
    for (int i = 0; i < w - 6; i++)
        fprintf(stderr, "─");
    fprintf(stderr, "│%s\n", TUI_RESET);

    /* Tool name */
    fprintf(stderr, "  %s│%s  %s%s %s%s%s", TUI_YELLOW, TUI_RESET, g->icon_gear, TUI_BOLD,
            tool_name ? tool_name : "unknown", TUI_RESET, "");
    int name_vis = 4 + (int)strlen(tool_name ? tool_name : "unknown");
    for (int i = 0; i < w - 6 - name_vis; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "%s│%s\n", TUI_YELLOW, TUI_RESET);

    /* Description */
    if (description && description[0]) {
        fprintf(stderr, "  %s│%s  %s%s%s", TUI_YELLOW, TUI_RESET, TUI_DIM, description, TUI_RESET);
        int desc_vis = 2 + (int)strlen(description);
        if (desc_vis > w - 6)
            desc_vis = w - 6;
        for (int i = 0; i < w - 6 - desc_vis; i++)
            fprintf(stderr, " ");
        fprintf(stderr, "%s│%s\n", TUI_YELLOW, TUI_RESET);
    }

    /* Detail */
    if (detail && detail[0]) {
        fprintf(stderr, "  %s│%s  %s%s%s", TUI_YELLOW, TUI_RESET, TUI_DIM, detail, TUI_RESET);
        int det_vis = 2 + (int)strlen(detail);
        if (det_vis > w - 6)
            det_vis = w - 6;
        for (int i = 0; i < w - 6 - det_vis; i++)
            fprintf(stderr, " ");
        fprintf(stderr, "%s│%s\n", TUI_YELLOW, TUI_RESET);
    }

    /* Blank line */
    fprintf(stderr, "  %s│", TUI_YELLOW);
    for (int i = 0; i < w - 6; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "│%s\n", TUI_RESET);

    /* Options */
    fprintf(stderr, "  %s│%s  %s[y]%s Allow  %s[n]%s Deny  %s[a]%s Always  %s[esc]%s Cancel",
            TUI_YELLOW, TUI_RESET, TUI_BGREEN, TUI_RESET, TUI_BRED, TUI_RESET, TUI_BCYAN, TUI_RESET,
            TUI_DIM, TUI_RESET);
    int opts_vis = 48;
    for (int i = 0; i < w - 6 - opts_vis; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "%s│%s\n", TUI_YELLOW, TUI_RESET);

    /* Bottom border */
    fprintf(stderr, "  %s%s╰", TUI_YELLOW, TUI_BOLD);
    for (int i = 0; i < w - 6; i++)
        fprintf(stderr, "─");
    fprintf(stderr, "╯%s\n", TUI_RESET);
    fflush(stderr);

    /* Read response */
    while (1) {
        int ch = read_single_char();
        if (ch == 'y' || ch == 'Y')
            return TUI_PERM_ALLOW;
        if (ch == 'n' || ch == 'N')
            return TUI_PERM_DENY;
        if (ch == 'a' || ch == 'A')
            return TUI_PERM_ALWAYS;
        if (ch == 27 || ch == 'q' || ch == 'Q')
            return TUI_PERM_CANCEL;
    }
}

bool tui_confirm(const char *question) {
    const tui_glyphs_t *g = tui_glyph();
    fprintf(stderr, "  %s%s %s%s %s[y/n]%s ", TUI_YELLOW, g->warn, TUI_RESET,
            question ? question : "Continue?", TUI_DIM, TUI_RESET);
    fflush(stderr);
    int ch = read_single_char();
    fprintf(stderr, "%c\n", ch);
    return (ch == 'y' || ch == 'Y');
}

/* ── Dynamic Question Dialog (AskUserQuestion) ──────────────────────── */

enum {
    DLG_K_UP = 1000,
    DLG_K_DOWN,
    DLG_K_LEFT,
    DLG_K_RIGHT,
    DLG_K_TAB,
    DLG_K_BTAB,
    DLG_K_ENTER,
    DLG_K_ESC,
    DLG_K_SPACE,
};

/* Raw key reader with escape-sequence + lone-ESC handling (VTIME timeout so a
 * bare ESC doesn't block waiting for a sequence tail). */
static int dlg_read_key(void) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 1;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    int c = getchar();
    int ret;
    if (c == '\r' || c == '\n')
        ret = DLG_K_ENTER;
    else if (c == '\t')
        ret = DLG_K_TAB;
    else if (c == ' ')
        ret = DLG_K_SPACE;
    else if (c == 27) {
        /* possible escape sequence; read tail with a short timeout */
        newt.c_cc[VMIN] = 0;
        newt.c_cc[VTIME] = 1; /* 0.1s */
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        int c2 = getchar();
        if (c2 == '[' || c2 == 'O') {
            int c3 = getchar();
            switch (c3) {
                case 'A':
                    ret = DLG_K_UP;
                    break;
                case 'B':
                    ret = DLG_K_DOWN;
                    break;
                case 'C':
                    ret = DLG_K_RIGHT;
                    break;
                case 'D':
                    ret = DLG_K_LEFT;
                    break;
                case 'Z':
                    ret = DLG_K_BTAB;
                    break;
                default:
                    ret = DLG_K_ESC;
                    break;
            }
        } else {
            ret = DLG_K_ESC;
        }
    } else {
        ret = c;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ret;
}

const char *tui_ask_answer_value(const tui_ask_question_t *q, char *out, size_t out_len) {
    if (!out || !out_len)
        return out;
    out[0] = '\0';
    if (!q)
        return out;
    if (q->custom[0]) {
        snprintf(out, out_len, "%s", q->custom);
        return out;
    }
    size_t pos = 0;
    bool first = true;
    for (int i = 0; i < q->n_options && pos < out_len; i++) {
        if (!q->selected[i])
            continue;
        const char *v = q->options[i].value[0] ? q->options[i].value : q->options[i].label;
        int w = snprintf(out + pos, out_len - pos, "%s%s", first ? "" : ", ", v);
        if (w < 0)
            break;
        pos += (size_t)w;
        first = false;
    }
    return out;
}

bool tui_ask_question_visible(const tui_ask_question_t *qs, int n, int qi) {
    if (!qs || qi < 0 || qi >= n)
        return false;
    const tui_ask_question_t *q = &qs[qi];
    if (q->gate_q < 0 || q->gate_q >= n || q->gate_q == qi)
        return true;
    /* the gating question must itself be visible and answered */
    if (!tui_ask_question_visible(qs, n, q->gate_q))
        return false;
    const tui_ask_question_t *g = &qs[q->gate_q];
    if (!g->answered)
        return false;
    for (int k = 0; k < q->n_gate_vals; k++) {
        if (g->custom[0] && strcmp(g->custom, q->gate_vals[k]) == 0)
            return true;
        for (int o = 0; o < g->n_options; o++) {
            if (!g->selected[o])
                continue;
            const char *ov = g->options[o].value[0] ? g->options[o].value : g->options[o].label;
            if (strcmp(ov, q->gate_vals[k]) == 0)
                return true;
        }
    }
    return false;
}

/* number of selectable rows in a question tab (options + escape hatches) */
static int dlg_row_count(const tui_ask_question_t *q) {
    int n = q->n_options;
    if (q->allow_custom)
        n++;
    if (q->allow_chat)
        n++;
    return n;
}
/* row index of the "Type something" / "Chat about this" pseudo-rows, or -1 */
static int dlg_custom_row(const tui_ask_question_t *q) {
    return q->allow_custom ? q->n_options : -1;
}
static int dlg_chat_row(const tui_ask_question_t *q) {
    if (!q->allow_chat)
        return -1;
    return q->n_options + (q->allow_custom ? 1 : 0);
}

/* Read a free-text line in canonical mode (for "Type something"). */
static void dlg_read_line(const char *prompt, char *buf, size_t buflen) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    tui_cursor_show();
    fprintf(stderr, "\033[2J\033[H\n  %s%s%s\n  %s> %s", TUI_BOLD,
            prompt ? prompt : "Type your answer:", TUI_RESET, TUI_BCYAN, TUI_RESET);
    fflush(stderr);
    if (fgets(buf, (int)buflen, stdin)) {
        buf[strcspn(buf, "\r\n")] = '\0';
    } else {
        buf[0] = '\0';
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    tui_cursor_hide();
}

static void dlg_render(tui_ask_question_t *qs, int n, const char *intro, const int *vis, int vc,
                       int tabpos, int row) {
    (void)n;
    int w = tui_term_width();
    if (w > 100)
        w = 100;
    fprintf(stderr, "\033[2J\033[H");

    /* Intro */
    if (intro && intro[0]) {
        fprintf(stderr, "  %s%s%s\n\n", TUI_DIM, intro, TUI_RESET);
    }

    /* Tab strip: visible questions + Submit */
    fprintf(stderr, "  %s\xe2\x86\x90%s ", TUI_DIM, TUI_RESET); /* ← */
    for (int i = 0; i < vc; i++) {
        int qi = vis[i];
        const char *box = qs[qi].answered ? "\xe2\x98\x92"  /* ☒ */
                                          : "\xe2\x98\x90"; /* ☐ */
        const char *hdr = qs[qi].header[0] ? qs[qi].header : "Q";
        if (tabpos == i)
            fprintf(stderr, "%s%s %s %s  ", TUI_REVERSE, box, hdr, TUI_RESET);
        else
            fprintf(stderr, "%s %s  ", box, hdr);
    }
    /* Submit tab */
    if (tabpos == vc)
        fprintf(stderr, "%s\xe2\x9c\x94 Submit %s ", TUI_REVERSE, TUI_RESET);
    else
        fprintf(stderr, "\xe2\x9c\x94 Submit ");
    fprintf(stderr, "%s\xe2\x86\x92%s\n", TUI_DIM, TUI_RESET); /* → */

    /* Divider */
    fprintf(stderr, "  %s", TUI_DIM);
    for (int i = 0; i < w - 4; i++)
        fprintf(stderr, "\xe2\x94\x80"); /* ─ */
    fprintf(stderr, "%s\n\n", TUI_RESET);

    if (tabpos < vc) {
        /* ── Question body ── */
        tui_ask_question_t *q = &qs[vis[tabpos]];
        fprintf(stderr, "  %s%s%s\n\n", TUI_BOLD, q->question, TUI_RESET);

        int num = 1;
        for (int i = 0; i < q->n_options; i++, num++) {
            bool cur = (row == i);
            fprintf(stderr, "  %s%s%d.%s %s%s%s%s\n", cur ? TUI_BCYAN : " ",
                    cur ? "\xe2\x80\xba " : "  ", num, TUI_RESET,
                    cur ? TUI_BBLUE TUI_BOLD : TUI_BLUE, q->options[i].label, TUI_RESET,
                    q->selected[i] ? "  \033[92m\xe2\x9c\x94\033[0m" : "");
            if (q->options[i].description[0])
                fprintf(stderr, "       %s%s%s\n", TUI_DIM, q->options[i].description, TUI_RESET);
        }
        int crow = dlg_custom_row(q);
        if (crow >= 0) {
            bool cur = (row == crow);
            fprintf(stderr, "  %s%s%d.%s %sType something.%s\n", cur ? TUI_BCYAN : " ",
                    cur ? "\xe2\x80\xba " : "  ", num++, TUI_RESET, TUI_DIM, TUI_RESET);
        }
        int hrow = dlg_chat_row(q);
        if (hrow >= 0) {
            bool cur = (row == hrow);
            fprintf(stderr, "  %s%s%d.%s %sChat about this%s\n", cur ? TUI_BCYAN : " ",
                    cur ? "\xe2\x80\xba " : "  ", num++, TUI_RESET, TUI_DIM, TUI_RESET);
        }
        if (q->multi_select)
            fprintf(stderr, "\n  %s(multi-select: Space toggles, then \xe2\x86\x92/Submit)%s\n",
                    TUI_DIM, TUI_RESET);
    } else {
        /* ── Review / Submit body ── */
        fprintf(stderr, "  %sReview your answers%s\n\n", TUI_BOLD, TUI_RESET);
        char val[1280];
        for (int i = 0; i < vc; i++) {
            tui_ask_question_t *q = &qs[vis[i]];
            fprintf(stderr, "  %s\xe2\x97\x8f%s %s%s%s\n", TUI_BBLUE, TUI_RESET, TUI_BOLD,
                    q->question[0] ? q->question : q->header, TUI_RESET);
            tui_ask_answer_value(q, val, sizeof val);
            fprintf(stderr, "     %s\xe2\x86\x92 %s%s\n", q->answered ? TUI_BBLUE : TUI_DIM,
                    q->answered && val[0] ? val : "(unanswered)", TUI_RESET);
        }
        fprintf(stderr, "\n  %sReady to submit your answers?%s\n", TUI_BOLD, TUI_RESET);
        fprintf(stderr, "  %s%s1.%s Submit answers\n", row == 0 ? TUI_BCYAN : " ",
                row == 0 ? "\xe2\x80\xba " : "  ", TUI_RESET);
        fprintf(stderr, "  %s%s2.%s Cancel\n", row == 1 ? TUI_BCYAN : " ",
                row == 1 ? "\xe2\x80\xba " : "  ", TUI_RESET);
    }

    /* Footer */
    fprintf(stderr, "\n  %s", TUI_DIM);
    for (int i = 0; i < w - 4; i++)
        fprintf(stderr, "\xe2\x94\x80");
    fprintf(stderr, "%s\n", TUI_RESET);
    fprintf(stderr,
            "  %sEnter/Space select \xc2\xb7 Tab/\xe2\x86\x90\xe2\x86\x92 tabs \xc2\xb7 "
            "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 1-9 jump \xc2\xb7 Esc cancel%s\n",
            TUI_DIM, TUI_RESET);
    fflush(stderr);
}

/* Activate the highlighted row of a question tab. Returns true if the dialog
 * should auto-advance to the next tab (single-select choice made). */
static bool dlg_activate(tui_ask_question_t *q, int row, bool *out_chat) {
    *out_chat = false;
    int crow = dlg_custom_row(q);
    int hrow = dlg_chat_row(q);
    if (row >= 0 && row < q->n_options) {
        if (q->multi_select) {
            q->selected[row] = !q->selected[row];
            q->custom[0] = '\0';
            q->answered = false;
            for (int i = 0; i < q->n_options; i++)
                if (q->selected[i]) {
                    q->answered = true;
                    break;
                }
            return false; /* stay; multi-select keeps choosing */
        }
        for (int i = 0; i < q->n_options; i++)
            q->selected[i] = false;
        q->selected[row] = true;
        q->custom[0] = '\0';
        q->answered = true;
        return true; /* advance */
    }
    if (row == crow) {
        char buf[1024];
        dlg_read_line(q->question[0] ? q->question : "Type your answer:", buf, sizeof buf);
        if (buf[0]) {
            for (int i = 0; i < q->n_options; i++)
                q->selected[i] = false;
            snprintf(q->custom, sizeof q->custom, "%s", buf);
            q->answered = true;
            return true;
        }
        return false;
    }
    if (row == hrow) {
        *out_chat = true;
        return false;
    }
    return false;
}

tui_ask_status_t tui_ask_questions(tui_ask_question_t *qs, int n_questions, const char *intro,
                                   char *chat_out, size_t chat_len) {
    if (chat_out && chat_len)
        chat_out[0] = '\0';
    if (n_questions <= 0)
        return TUI_ASK_CANCEL;
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))
        return TUI_ASK_CANCEL;

    int vis[TUI_ASK_MAX_QUESTIONS];
    int tabpos = 0, row = 0;

    tui_cursor_hide();
    tui_ask_status_t status = TUI_ASK_CANCEL;

    for (;;) {
        /* Recompute visible-question list each iteration (branching is live). */
        int vc = 0;
        for (int i = 0; i < n_questions && vc < TUI_ASK_MAX_QUESTIONS; i++)
            if (tui_ask_question_visible(qs, n_questions, i))
                vis[vc++] = i;
        if (tabpos > vc)
            tabpos = vc;

        int maxrow = (tabpos < vc) ? dlg_row_count(&qs[vis[tabpos]]) - 1 : 1;
        if (maxrow < 0)
            maxrow = 0;
        if (row > maxrow)
            row = maxrow;
        if (row < 0)
            row = 0;

        dlg_render(qs, n_questions, intro, vis, vc, tabpos, row);

        int k = dlg_read_key();
        if (k == DLG_K_ESC) {
            status = TUI_ASK_CANCEL;
            break;
        }
        if (k == DLG_K_TAB || k == DLG_K_RIGHT) {
            if (tabpos < vc)
                tabpos++;
            row = 0;
            continue;
        }
        if (k == DLG_K_BTAB || k == DLG_K_LEFT) {
            if (tabpos > 0)
                tabpos--;
            row = 0;
            continue;
        }
        if (k == DLG_K_UP) {
            row = (row > 0) ? row - 1 : maxrow;
            continue;
        }
        if (k == DLG_K_DOWN) {
            row = (row < maxrow) ? row + 1 : 0;
            continue;
        }

        if (k >= '1' && k <= '9') {
            int want = k - '1';
            if (tabpos < vc) {
                if (want <= maxrow)
                    row = want;
                else
                    continue;
            } else {
                if (want <= 1)
                    row = want;
                else
                    continue;
            }
            k = DLG_K_ENTER; /* fall through to activate */
        }

        if (k == DLG_K_ENTER || k == DLG_K_SPACE) {
            if (tabpos == vc) {
                status = (row == 0) ? TUI_ASK_SUBMIT : TUI_ASK_CANCEL;
                break;
            }
            bool want_chat = false;
            bool advance = dlg_activate(&qs[vis[tabpos]], row, &want_chat);
            if (want_chat) {
                if (chat_out && chat_len)
                    snprintf(chat_out, chat_len, "%s", qs[vis[tabpos]].question);
                status = TUI_ASK_CHAT;
                break;
            }
            if (advance) {
                if (tabpos < vc)
                    tabpos++;
                row = 0;
            }
            continue;
        }
    }

    tui_cursor_show();
    fprintf(stderr, "\033[2J\033[H");
    fflush(stderr);
    return status;
}

/* ── Structured Diff Display ────────────────────────────────────────── */

void tui_diff_init(tui_diff_t *d) {
    memset(d, 0, sizeof(*d));
    d->cap = 256;
    d->lines = calloc(d->cap, sizeof(tui_diff_line_t));
}

void tui_diff_free(tui_diff_t *d) {
    for (int i = 0; i < d->count; i++)
        free(d->lines[i].text);
    free(d->lines);
    memset(d, 0, sizeof(*d));
}

static void diff_add_line(tui_diff_t *d, char type, int old_l, int new_l, const char *text) {
    if (d->count >= d->cap) {
        d->cap *= 2;
        d->lines = realloc(d->lines, d->cap * sizeof(tui_diff_line_t));
    }
    tui_diff_line_t *l = &d->lines[d->count++];
    l->type = type;
    l->old_line = old_l;
    l->new_line = new_l;
    l->text = strdup(text ? text : "");
}

bool tui_diff_parse(tui_diff_t *d, const char *unified_diff) {
    if (!d || !unified_diff)
        return false;
    const char *p = unified_diff;
    int old_line = 0, new_line = 0;

    while (*p) {
        const char *eol = strchr(p, '\n');
        int line_len = eol ? (int)(eol - p) : (int)strlen(p);
        char line_buf[4096];
        int n = line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
        memcpy(line_buf, p, n);
        line_buf[n] = '\0';

        if (strncmp(line_buf, "--- ", 4) == 0) {
            snprintf(d->old_file, sizeof(d->old_file), "%s", line_buf + 4);
        } else if (strncmp(line_buf, "+++ ", 4) == 0) {
            snprintf(d->new_file, sizeof(d->new_file), "%s", line_buf + 4);
        } else if (strncmp(line_buf, "@@ ", 3) == 0) {
            /* Parse hunk header: @@ -old,count +new,count @@ */
            if (sscanf(line_buf, "@@ -%d", &old_line) != 1)
                old_line = 1;
            char *plus = strchr(line_buf + 3, '+');
            if (plus && sscanf(plus, "+%d", &new_line) != 1)
                new_line = 1;
            diff_add_line(d, '@', old_line, new_line, line_buf);
        } else if (line_buf[0] == '+') {
            diff_add_line(d, '+', -1, new_line++, line_buf + 1);
        } else if (line_buf[0] == '-') {
            diff_add_line(d, '-', old_line++, -1, line_buf + 1);
        } else if (line_buf[0] == ' ') {
            diff_add_line(d, ' ', old_line++, new_line++, line_buf + 1);
        } else {
            diff_add_line(d, ' ', old_line++, new_line++, line_buf);
        }

        p = eol ? eol + 1 : p + line_len;
        if (!eol)
            break;
    }
    return d->count > 0;
}

void tui_diff_render(const tui_diff_t *d, FILE *out, int context_lines) {
    if (!d || !out)
        return;
    (void)context_lines; /* render all lines */

    /* File header */
    if (d->old_file[0] || d->new_file[0]) {
        fprintf(out, "  %s%s--- %s%s\n", TUI_DIM, TUI_RED, d->old_file, TUI_RESET);
        fprintf(out, "  %s%s+++ %s%s\n", TUI_DIM, TUI_GREEN, d->new_file, TUI_RESET);
    }

    /* Max line number width for alignment */
    int max_old = 0, max_new = 0;
    for (int i = 0; i < d->count; i++) {
        if (d->lines[i].old_line > max_old)
            max_old = d->lines[i].old_line;
        if (d->lines[i].new_line > max_new)
            max_new = d->lines[i].new_line;
    }
    int lw = 1;
    int mx = max_old > max_new ? max_old : max_new;
    while (mx >= 10) {
        lw++;
        mx /= 10;
    }

    for (int i = 0; i < d->count; i++) {
        const tui_diff_line_t *l = &d->lines[i];
        switch (l->type) {
            case '@':
                fprintf(out, "  %s%s%s%s\n", TUI_BCYAN, TUI_BOLD, l->text, TUI_RESET);
                break;
            case '+':
                fprintf(out, "  %s%*s %s%*d%s │%s+%s%s\n", TUI_DIM, lw, "", TUI_GREEN, lw,
                        l->new_line, TUI_RESET, TUI_GREEN, l->text, TUI_RESET);
                break;
            case '-':
                fprintf(out, "  %s%*d %s%*s%s │%s-%s%s\n", TUI_DIM, lw, l->old_line, "", lw, "",
                        TUI_RESET, TUI_RED, l->text, TUI_RESET);
                break;
            default:
                fprintf(out, "  %s%*d %*d%s │ %s\n", TUI_DIM, lw, l->old_line, lw, l->new_line,
                        TUI_RESET, l->text);
                break;
        }
    }
}

void tui_diff_render_inline(const tui_diff_t *d, FILE *out) {
    /* Simplified inline diff: just highlight +/- lines */
    tui_diff_render(d, out, 0);
}

/* ── Full-Screen Pager ──────────────────────────────────────────────── */

void tui_pager_init(tui_pager_t *p, const char **lines, int count, const char *title) {
    memset(p, 0, sizeof(*p));
    p->lines = lines;
    p->line_count = count;
    p->offset = 0;
    p->page_size = tui_term_height() - 3; /* title + status */
    p->term_width = tui_term_width();
    p->show_line_numbers = true;
    p->wrap_lines = false;
    p->search_hit = -1;
    p->title = title;
}

static void pager_render(const tui_pager_t *p) {
    /* Clear screen */
    fprintf(stderr, "\033[2J\033[H");

    /* Title bar */
    fprintf(stderr, "%s%s%s", TUI_REVERSE, TUI_BOLD, " ");
    if (p->title)
        fprintf(stderr, "%s", p->title);
    int title_len = p->title ? (int)strlen(p->title) + 1 : 1;
    for (int i = title_len; i < p->term_width; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "%s\n", TUI_RESET);

    /* Content */
    int lw = 1;
    int n = p->line_count;
    while (n >= 10) {
        lw++;
        n /= 10;
    }

    for (int i = 0; i < p->page_size && (p->offset + i) < p->line_count; i++) {
        int li = p->offset + i;
        const char *line = p->lines[li];

        /* Search highlight */
        bool is_hit = (p->search[0] && p->search_hit == li);

        if (p->show_line_numbers) {
            fprintf(stderr, "%s%*d%s │ ", TUI_DIM, lw, li + 1, TUI_RESET);
        }
        if (is_hit) {
            fprintf(stderr, "%s%s%s", TUI_REVERSE, line ? line : "", TUI_RESET);
        } else {
            fprintf(stderr, "%s", line ? line : "");
        }
        fprintf(stderr, "\n");
    }

    /* Status bar */
    fprintf(stderr, "\033[%d;1H", tui_term_height());
    fprintf(stderr, "%s%s", TUI_REVERSE, " ");
    int pct = p->line_count > 0 ? (int)(100.0 * (p->offset + p->page_size) / p->line_count) : 100;
    if (pct > 100)
        pct = 100;
    fprintf(stderr, "Line %d-%d of %d (%d%%)  q:quit  /:search  n:next", p->offset + 1,
            p->offset + p->page_size < p->line_count ? p->offset + p->page_size : p->line_count,
            p->line_count, pct);
    int sbar_vis = 60; /* approximate */
    for (int i = sbar_vis; i < p->term_width; i++)
        fprintf(stderr, " ");
    fprintf(stderr, "%s", TUI_RESET);
    fflush(stderr);
}

static void pager_search_next(tui_pager_t *p) {
    if (!p->search[0])
        return;
    int start = p->search_hit >= 0 ? p->search_hit + 1 : p->offset;
    for (int i = 0; i < p->line_count; i++) {
        int li = (start + i) % p->line_count;
        if (p->lines[li] && strstr(p->lines[li], p->search)) {
            p->search_hit = li;
            p->offset = li - p->page_size / 2;
            if (p->offset < 0)
                p->offset = 0;
            return;
        }
    }
    p->search_hit = -1;
}

void tui_pager_run(tui_pager_t *p) {
    tui_cursor_hide();
    pager_render(p);

    while (1) {
        int ch = read_single_char();
        if (ch == 'q' || ch == 'Q')
            break;
        if (ch == 'j' || ch == 'e') {
            if (p->offset < p->line_count - p->page_size)
                p->offset++;
        }
        if (ch == 'k' || ch == 'y') {
            if (p->offset > 0)
                p->offset--;
        }
        if (ch == 'd' || ch == ' ') {
            p->offset += p->page_size / 2;
            if (p->offset > p->line_count - p->page_size)
                p->offset = p->line_count - p->page_size;
            if (p->offset < 0)
                p->offset = 0;
        }
        if (ch == 'u') {
            p->offset -= p->page_size / 2;
            if (p->offset < 0)
                p->offset = 0;
        }
        if (ch == 'g')
            p->offset = 0;
        if (ch == 'G') {
            p->offset = p->line_count - p->page_size;
            if (p->offset < 0)
                p->offset = 0;
        }
        if (ch == 'n')
            pager_search_next(p);
        if (ch == '/') {
            /* Enter search mode — read query from user */
            fprintf(stderr, "\033[%d;1H\033[K/", tui_term_height());
            fflush(stderr);
            /* Restore canonical mode briefly for line input */
            struct termios oldt, newt;
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_lflag |= (ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            if (fgets(p->search, sizeof(p->search), stdin)) {
                int sl = (int)strlen(p->search);
                if (sl > 0 && p->search[sl - 1] == '\n')
                    p->search[sl - 1] = '\0';
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            p->search_hit = -1;
            pager_search_next(p);
        }
        if (ch == 27) { /* ESC — check for arrow keys */
            int c2 = read_single_char();
            if (c2 == '[') {
                int c3 = read_single_char();
                if (c3 == 'A') {
                    if (p->offset > 0)
                        p->offset--;
                } /* Up */
                if (c3 == 'B') {
                    if (p->offset < p->line_count - p->page_size)
                        p->offset++;
                } /* Down */
                if (c3 == '5') {
                    read_single_char();
                    p->offset -= p->page_size;
                    if (p->offset < 0)
                        p->offset = 0;
                } /* PgUp */
                if (c3 == '6') {
                    read_single_char();
                    p->offset += p->page_size;
                    if (p->offset > p->line_count - p->page_size)
                        p->offset = p->line_count - p->page_size;
                    if (p->offset < 0)
                        p->offset = 0;
                } /* PgDn */
            } else if (c2 == 27) {
                break; /* Double ESC = quit */
            }
        }
        pager_render(p);
    }

    tui_cursor_show();
    fprintf(stderr, "\033[2J\033[H"); /* clear screen */
    fflush(stderr);
}

/* ── Inline Code Block ──────────────────────────────────────────────── */

static void tui_fprint_safe_span(FILE *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\t') {
            fputc('\t', out);
        } else if (ch < 0x20 || ch == 0x7F) {
            fprintf(out, "\\x%02X", ch);
        } else {
            fputc((char)ch, out);
        }
    }
}

static int tui_count_code_lines(const char *code) {
    if (!code || !*code)
        return 0;
    int n = 0;
    const char *p = code;
    while (*p) {
        n++;
        const char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    return n;
}

static int tui_int_width(int n) {
    int w = 1;
    while (n >= 10) {
        n /= 10;
        w++;
    }
    return w;
}

void tui_code_block(FILE *out, const char *code, const char *language, int start_line,
                    bool show_line_numbers) {
    if (!out || !code)
        return;
    int w = tui_term_width();
    if (w > 120)
        w = 120;
    int total_lines = tui_count_code_lines(code);
    int max_line_num = (start_line > 0 ? start_line : 1) + (total_lines > 0 ? total_lines - 1 : 0);
    int lineno_w = tui_int_width(max_line_num);
    if (lineno_w < 3)
        lineno_w = 3;
    bool fallback_trunc = false;

    /* Top border with language tag */
    fprintf(out, "  %s╭", TUI_DIM);
    if (language && language[0]) {
        fprintf(out, "─ %s%s", TUI_RESET, TUI_BCYAN);
        tui_fprint_safe_span(out, language, strlen(language));
        fprintf(out, "%s ", TUI_DIM);
        int tag_len = tui_str_display_width(language) + 4;
        for (int i = tag_len; i < w - 6; i++)
            fprintf(out, "─");
    } else {
        for (int i = 0; i < w - 6; i++)
            fprintf(out, "─");
    }
    fprintf(out, "╮%s\n", TUI_RESET);

    /* Code lines */
    int line_num = start_line > 0 ? start_line : 1;
    const char *p = code;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int ll = eol ? (int)(eol - p) : (int)strlen(p);
        char stack_line[2048];
        char *line_buf = stack_line;
        int copy_n = ll;
        bool heap_line = false;
        if (ll >= (int)sizeof(stack_line)) {
            line_buf = malloc((size_t)ll + 1);
            if (line_buf) {
                heap_line = true;
            } else {
                copy_n = (int)sizeof(stack_line) - 1;
                fallback_trunc = true;
                line_buf = stack_line;
            }
        }
        memcpy(line_buf, p, (size_t)copy_n);
        line_buf[copy_n] = '\0';

        fprintf(out, "  %s│%s", TUI_DIM, TUI_RESET);
        if (show_line_numbers) {
            fprintf(out, " %s%*d%s │ ", TUI_DIM, lineno_w, line_num, TUI_RESET);
        } else {
            fprintf(out, " ");
        }
        tui_fprint_safe_span(out, line_buf, strlen(line_buf));
        fprintf(out, "\n");
        if (heap_line)
            free(line_buf);
        line_num++;
        p = eol ? eol + 1 : p + ll;
        if (!eol)
            break;
    }

    if (fallback_trunc) {
        fprintf(out, "  %s│%s %s[code block truncated]%s\n", TUI_DIM, TUI_RESET, TUI_DIM,
                TUI_RESET);
    }

    /* Bottom border */
    fprintf(out, "  %s╰", TUI_DIM);
    for (int i = 0; i < w - 6; i++)
        fprintf(out, "─");
    fprintf(out, "╯%s\n", TUI_RESET);
}

/* ── Breadcrumb Path Display ────────────────────────────────────────── */

void tui_breadcrumb(FILE *out, const char *path, int max_width) {
    if (!out || !path)
        return;
    int plen = (int)strlen(path);

    /* If fits, render with styled separators */
    if (plen <= max_width) {
        const char *p = path;
        while (*p) {
            if (*p == '/') {
                fprintf(out, "%s/%s", TUI_DIM, TUI_RESET);
            } else {
                fputc(*p, out);
            }
            p++;
        }
        return;
    }

    /* Truncate: show .../<last_two_components> */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        /* No slashes — just truncate */
        fprintf(out, "...%.*s", max_width - 3, path + (plen - max_width + 3));
        return;
    }
    const char *second_last = last_slash - 1;
    while (second_last > path && *second_last != '/')
        second_last--;
    if (*second_last == '/')
        second_last++;

    int tail_len = (int)(path + plen - second_last);
    if (tail_len + 4 <= max_width) {
        fprintf(out, "%s...%s", TUI_DIM, TUI_RESET);
        fprintf(out, "%s/%s", TUI_DIM, TUI_RESET);
        const char *t = second_last;
        while (*t) {
            if (*t == '/')
                fprintf(out, "%s/%s", TUI_DIM, TUI_RESET);
            else
                fputc(*t, out);
            t++;
        }
    } else {
        /* Even the tail is too long */
        fprintf(out, "...%.*s", max_width - 3, last_slash + 1);
    }
}

/* ── Terminal Lock Overlay ───────────────────────────────────────────────────
 *
 * Called by the presence module when the idle threshold fires.
 * Clears the terminal, draws a centered lock screen, and blocks in a tight
 * kqueue loop that only wakes on:
 *   - EVFILT_TIMER (1s tick to refresh the clock)
 *   - EVFILT_USER  (Touch ID success from background thread → unlock)
 *   - EVFILT_READ  (any keypress → retrigger Touch ID prompt)
 *
 * Output is batched into a single write() per frame to minimize latency.
 * ─────────────────────────────────────────────────────────────────────────── */

#ifdef __APPLE__
#include <sys/event.h>
#endif

static atomic_int s_lock_kq = -1;                  /* kqueue fd for the lock loop */
static atomic_bool s_lock_touchid_pending = false; /* an LAContext dialog is up */

/* Wake the lock loop from the Touch ID callback thread. */
static void tui_lock_wake(void) {
    int kq = atomic_load(&s_lock_kq);
    if (kq < 0)
        return;
#ifdef __APPLE__
    struct kevent wake;
    EV_SET(&wake, 99, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(kq, &wake, 1, NULL, 0, NULL);
#endif
}

static void touchid_lock_cb(bool success, const char *err_msg, void *ctx) {
    (void)err_msg;
    (void)ctx;
    if (success)
        presence_mark_unlocked();
    /* Clear the in-flight flag on BOTH success and failure/cancel, then wake
     * the loop. On success the loop sees !locked and exits; on failure it
     * stays locked but the flag is now clear, so a keystroke or the next timer
     * tick can re-pop the prompt. Without clearing on failure the flag stuck
     * true forever and the user was trapped behind a dead dialog. */
    atomic_store(&s_lock_touchid_pending, false);
    tui_lock_wake();
}

/* Draw the full lock overlay into a stack buffer; return bytes written.
 *
 * Strategy: explicitly fill every cell of the visible terminal with a pure-black
 * background. Don't trust `\033[2J` alone — some terminals (and most terminal-
 * in-terminal scenarios) don't honor it after switching to the alternate screen
 * buffer, which would leave the previous TUI content readable behind the lock.
 * Painting every cell ourselves guarantees a hard blackout regardless of
 * emulator quirks. */
static size_t tui_lock_render(char *buf, size_t cap) {
    int cols = tui_term_width();
    int rows = tui_term_height();
    if (cols < 20)
        cols = 20;
    if (rows < 8)
        rows = 8;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[32], date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
    strftime(date_str, sizeof(date_str), "%a %b %d, %Y", tm);

    /* Box dimensions */
    const int BOX_W = 44;
    const int BOX_H = 9;
    int box_row = (rows - BOX_H) / 2;
    int box_col = (cols - BOX_W) / 2;
    if (box_col < 1)
        box_col = 1;
    if (box_row < 1)
        box_row = 1;

    size_t n = 0;
#define APPEND(s)                                                                                  \
    do {                                                                                           \
        size_t _l = strlen(s);                                                                     \
        if (n + _l < cap) {                                                                        \
            memcpy(buf + n, s, _l);                                                                \
            n += _l;                                                                               \
        }                                                                                          \
    } while (0)
#define APPENDF(...)                                                                               \
    do {                                                                                           \
        char _tmp[256];                                                                            \
        int _r = snprintf(_tmp, sizeof(_tmp), __VA_ARGS__);                                        \
        if (_r > 0 && n + (size_t)_r < cap) {                                                      \
            memcpy(buf + n, _tmp, _r);                                                             \
            n += _r;                                                                               \
        }                                                                                          \
    } while (0)

    /* Reset, hide cursor, go home */
    APPEND("\033[0m\033[?25l\033[H");
    /* Pure black background + matching foreground so any glyphs that leak in
     * from a racing thread are invisible */
    APPEND("\033[48;2;0;0;0m\033[38;2;0;0;0m");
    /* First a coarse clear, then explicit per-cell fill to guarantee opacity */
    APPEND("\033[2J");
    for (int r = 1; r <= rows; r++) {
        APPENDF("\033[%d;1H", r);
        for (int c = 0; c < cols; c++)
            APPEND(" ");
    }

    /* Border colour reset before drawing the box */
    APPEND("\033[0m\033[48;2;14;14;22m"); /* very dark navy box interior */

    /* Top border */
    APPENDF("\033[%d;%dH", box_row, box_col);
    APPEND("\033[38;2;100;100;160m"); /* muted purple border */
    APPEND("╔");
    for (int i = 0; i < BOX_W - 2; i++)
        APPEND("═");
    APPEND("╗");

    /* Title row */
    APPENDF("\033[%d;%dH", box_row + 1, box_col);
    APPEND("\033[38;2;100;100;160m║");
    APPEND("\033[38;2;220;220;255m\033[1m");
    int pad = (BOX_W - 2 - 4) / 2;
    for (int i = 0; i < pad; i++)
        APPEND(" ");
    APPEND("dsco");
    for (int i = pad + 4; i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("\033[22m\033[38;2;100;100;160m║");

    /* Divider */
    APPENDF("\033[%d;%dH", box_row + 2, box_col);
    APPEND("╠");
    for (int i = 0; i < BOX_W - 2; i++)
        APPEND("═");
    APPEND("╣");

    /* Clock row */
    APPENDF("\033[%d;%dH", box_row + 3, box_col);
    APPEND("\033[38;2;100;100;160m║");
    APPEND("\033[38;2;255;210;80m");
    int tpad = (BOX_W - 2 - (int)strlen(time_str)) / 2;
    for (int i = 0; i < tpad; i++)
        APPEND(" ");
    APPEND(time_str);
    for (int i = tpad + (int)strlen(time_str); i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("\033[38;2;100;100;160m║");

    /* Date row */
    APPENDF("\033[%d;%dH", box_row + 4, box_col);
    APPEND("║");
    APPEND("\033[38;2;150;150;180m");
    int dpad = (BOX_W - 2 - (int)strlen(date_str)) / 2;
    for (int i = 0; i < dpad; i++)
        APPEND(" ");
    APPEND(date_str);
    for (int i = dpad + (int)strlen(date_str); i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("\033[38;2;100;100;160m║");

    /* Empty row */
    APPENDF("\033[%d;%dH", box_row + 5, box_col);
    APPEND("║");
    for (int i = 0; i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("║");

    /* Touch ID prompt — slow blink so it's obvious the screen is alive */
    const char *prompt = " 󰈷  Touch ID to unlock ";
    APPENDF("\033[%d;%dH", box_row + 6, box_col);
    APPEND("\033[38;2;100;100;160m║");
    APPEND("\033[5m\033[38;2;120;220;255m"); /* slow-blink cyan */
    int prompt_cells = 23;                   /* visual width incl. NF glyph and padding */
    int ppad = (BOX_W - 2 - prompt_cells) / 2;
    for (int i = 0; i < ppad; i++)
        APPEND(" ");
    APPEND(prompt);
    for (int i = ppad + prompt_cells; i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("\033[25m\033[38;2;100;100;160m║");

    /* Hint row */
    const char *hint = "context blacked out";
    APPENDF("\033[%d;%dH", box_row + 7, box_col);
    APPEND("║");
    APPEND("\033[2m\033[38;2;110;110;140m"); /* dim grey */
    int hpad = (BOX_W - 2 - (int)strlen(hint)) / 2;
    for (int i = 0; i < hpad; i++)
        APPEND(" ");
    APPEND(hint);
    for (int i = hpad + (int)strlen(hint); i < BOX_W - 2; i++)
        APPEND(" ");
    APPEND("\033[22m\033[38;2;100;100;160m║");

    /* Bottom border */
    APPENDF("\033[%d;%dH", box_row + 8, box_col);
    APPEND("╚");
    for (int i = 0; i < BOX_W - 2; i++)
        APPEND("═");
    APPEND("╝");
    APPEND("\033[0m");

    /* Park cursor far off-screen so the OS draw caret doesn't sit in the box */
    APPENDF("\033[%d;%dH", rows, cols);
    APPEND("\033[?25l");

#undef APPEND
#undef APPENDF
    return n;
}

void tui_lock_engage(void) {
    /* Hold the global terminal mutex for the entire lock duration so no other
     * thread (status bar, spinner, composer redraw, panel notifier) can punch
     * a hole through the lock overlay. Anything that drew via tui_term_lock()
     * blocks until we release on unlock. */
    tui_term_lock();

    /* Save terminal state and enter raw mode */
    struct termios saved, raw;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    /* Enter the alternate screen buffer so the existing TUI is preserved
     * underneath the lock overlay. On exit (`\033[?1049l`), the terminal
     * restores the prior screen automatically — without this, clearing the
     * screen on unlock would leave the composer thread staring at a blank
     * terminal until the next keystroke. */
    write(STDOUT_FILENO, "\033[?1049h", 8);
    /* Drain anything sitting in the kernel input buffer so a stray key from
     * before the lock can't be replayed straight back into the composer. */
    tcflush(STDIN_FILENO, TCIFLUSH);

    /* Initial render — frame needs to be large enough to hold one space per
     * cell of the visible terminal (up to ~400 cols × 200 rows × overhead). */
    char frame[262144];
    size_t n = tui_lock_render(frame, sizeof(frame));
    write(STDOUT_FILENO, frame, n);

#ifdef __APPLE__
    int kq = kqueue();
    atomic_store(&s_lock_kq, kq);

    struct kevent changes[3];
    /* 1s clock tick */
    EV_SET(&changes[0], 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_SECONDS, 1, NULL);
    /* Touch ID wake */
    EV_SET(&changes[1], 99, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    /* Keypress */
    EV_SET(&changes[2], STDIN_FILENO, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(kq, changes, 3, NULL, 0, NULL);

    /* Kick off initial Touch ID request */
    bool have_touchid = touchid_available();
    atomic_store(&s_lock_touchid_pending, false);
    if (have_touchid) {
        atomic_store(&s_lock_touchid_pending, true);
        touchid_authenticate("Unlock dsco", touchid_lock_cb, NULL);
    }

    struct kevent ev;
    while (presence_is_locked()) {
        int r = kevent(kq, NULL, 0, &ev, 1, NULL);
        if (r <= 0)
            break;

        if (ev.filter == EVFILT_USER) {
            /* Touch ID callback fired. If it unlocked us, leave the loop.
             * Otherwise the attempt failed/cancelled — the flag is already
             * clear, so a keypress or the next timer tick will re-pop it.
             * We deliberately do NOT re-arm here: an instant-fail policy
             * (e.g. biometry lockout) would otherwise spin popping dialogs. */
            if (!presence_is_locked())
                break;
            continue;
        }

        if (ev.filter == EVFILT_READ) {
            /* Drain ALL pending bytes so they can't trickle back to the
             * composer. */
            unsigned char discard[64];
            while (read(STDIN_FILENO, discard, sizeof(discard)) > 0) {
            }
            if (have_touchid) {
                /* Re-pop the prompt on any key if no dialog is currently up. */
                if (!atomic_load(&s_lock_touchid_pending)) {
                    atomic_store(&s_lock_touchid_pending, true);
                    touchid_authenticate("Unlock dsco", touchid_lock_cb, NULL);
                }
            } else {
                /* No biometric available at all — fall back to keypress
                 * unlock so the user is never trapped behind a screen that
                 * has no way to authenticate. */
                presence_mark_unlocked();
                break;
            }
        }

        if (ev.filter == EVFILT_TIMER) {
            /* Tick: redraw clock + reassert blackout in case anything raced
             * through before we acquired the term mutex. */
            n = tui_lock_render(frame, sizeof(frame));
            write(STDOUT_FILENO, frame, n);
            /* Safety net: if we're still locked and no dialog is up, re-pop so
             * there's always a live prompt the user can interact with. */
            if (have_touchid && !atomic_load(&s_lock_touchid_pending)) {
                atomic_store(&s_lock_touchid_pending, true);
                touchid_authenticate("Unlock dsco", touchid_lock_cb, NULL);
            }
        }
    }

    atomic_store(&s_lock_touchid_pending, false);

    close(kq);
    atomic_store(&s_lock_kq, -1);
#else
    /* Non-Apple: block until Enter (no Touch ID) */
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) > 0 && c != '\n' && c != '\r') {
    }
    presence_mark_unlocked();
#endif

    /* Drain anything the user typed at the lock screen before they touched
     * the sensor — those bytes have nothing to do with the next prompt. */
    tcflush(STDIN_FILENO, TCIFLUSH);

    /* Restore terminal; leaving the alt screen restores the prior TUI. */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    write(STDOUT_FILENO, "\033[0m\033[?25h\033[?1049l", 15);

    /* Release the global mutex so the composer can resume drawing. */
    tui_term_unlock();
}

/* ── Interactive Hierarchical Menu ──────────────────────────────────────── */

void tui_menu_init(tui_menu_t *m, const char *title, const char *subtitle) {
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    if (title)
        snprintf(m->title, sizeof(m->title), "%s", title);
    if (subtitle)
        snprintf(m->subtitle, sizeof(m->subtitle), "%s", subtitle);
    m->items = calloc(TUI_MENU_MAX_NODES, sizeof(tui_menu_item_t));
    m->item_count = 0;
    m->accent = NULL;
    m->max_visible = 0;
    m->selected = 0;
}

static void menu_free_items(tui_menu_item_t *items, int count) {
    if (!items)
        return;
    for (int i = 0; i < count; i++) {
        if (items[i].children) {
            menu_free_items(items[i].children, items[i].child_count);
            free(items[i].children);
            items[i].children = NULL;
        }
    }
}

void tui_menu_free(tui_menu_t *m) {
    if (!m || !m->items)
        return;
    menu_free_items(m->items, m->item_count);
    free(m->items);
    m->items = NULL;
    m->item_count = 0;
}

/* Append a fresh node to a level array (cap TUI_MENU_MAX_NODES). Returns it. */
static tui_menu_item_t *menu_level_add(tui_menu_item_t *arr, int *count, const char *label,
                                       tui_menu_kind_t kind, int id) {
    if (!arr || *count >= TUI_MENU_MAX_NODES)
        return NULL;
    tui_menu_item_t *it = &arr[(*count)++];
    memset(it, 0, sizeof(*it));
    if (label)
        snprintf(it->label, sizeof(it->label), "%s", label);
    it->kind = kind;
    it->id = id;
    it->badge = TUI_MENU_BADGE_NONE;
    if (kind == TUI_MENU_GROUP || kind == TUI_MENU_SEPARATOR)
        it->disabled = true;
    return it;
}

/* Lazily allocate a parent's children array, then append. */
static tui_menu_item_t *menu_child_add(tui_menu_item_t *parent, const char *label,
                                       tui_menu_kind_t kind, int id) {
    if (!parent)
        return NULL;
    if (!parent->children) {
        parent->children = calloc(TUI_MENU_MAX_NODES, sizeof(tui_menu_item_t));
        if (!parent->children)
            return NULL;
    }
    return menu_level_add(parent->children, &parent->child_count, label, kind, id);
}

tui_menu_item_t *tui_menu_add_group(tui_menu_t *m, const char *label) {
    return m ? menu_level_add(m->items, &m->item_count, label, TUI_MENU_GROUP, -1) : NULL;
}
tui_menu_item_t *tui_menu_add_item(tui_menu_t *m, const char *label, int id) {
    return m ? menu_level_add(m->items, &m->item_count, label, TUI_MENU_ITEM, id) : NULL;
}
tui_menu_item_t *tui_menu_add_separator(tui_menu_t *m) {
    return m ? menu_level_add(m->items, &m->item_count, "", TUI_MENU_SEPARATOR, -1) : NULL;
}
tui_menu_item_t *tui_menu_add_child(tui_menu_item_t *parent, const char *label, int id) {
    return menu_child_add(parent, label, TUI_MENU_ITEM, id);
}
tui_menu_item_t *tui_menu_add_submenu(tui_menu_item_t *parent, const char *label, int id) {
    return menu_child_add(parent, label, TUI_MENU_SUBMENU, id);
}
tui_menu_item_t *tui_menu_add_action(tui_menu_item_t *parent, const char *label, int id) {
    return menu_child_add(parent, label, TUI_MENU_ACTION, id);
}

void tui_menu_set_badge(tui_menu_item_t *it, tui_menu_badge_t badge, const char *text) {
    if (!it)
        return;
    it->badge = badge;
    if (text)
        snprintf(it->badge_text, sizeof(it->badge_text), "%s", text);
    else
        it->badge_text[0] = '\0';
}
void tui_menu_set_detail(tui_menu_item_t *it, const char *detail) {
    if (it && detail)
        snprintf(it->detail, sizeof(it->detail), "%s", detail);
}
void tui_menu_set_hint(tui_menu_item_t *it, const char *hint) {
    if (it && hint)
        snprintf(it->hint, sizeof(it->hint), "%s", hint);
}
void tui_menu_set_disabled(tui_menu_item_t *it, bool disabled) {
    if (it)
        it->disabled = disabled;
}
void tui_menu_set_expanded(tui_menu_item_t *it, bool expanded) {
    if (it && it->kind == TUI_MENU_SUBMENU)
        it->expanded = expanded;
}

/* ── Flattening: walk the tree into the visible-row list ──────────────── */

typedef struct {
    tui_menu_item_t *item;
    int depth;
    bool selectable;
    bool is_hint; /* synthetic dim sub-line row carrying item->hint */
} menu_row_t;

#define MENU_ROWS_MAX 512

static void menu_flatten_level(tui_menu_item_t *arr, int count, int depth, menu_row_t *rows,
                               int *nrows) {
    for (int i = 0; i < count && *nrows < MENU_ROWS_MAX; i++) {
        tui_menu_item_t *it = &arr[i];
        bool sel = !it->disabled && (it->kind == TUI_MENU_ITEM || it->kind == TUI_MENU_SUBMENU ||
                                     it->kind == TUI_MENU_ACTION);
        rows[*nrows].item = it;
        rows[*nrows].depth = depth;
        rows[*nrows].selectable = sel;
        rows[*nrows].is_hint = false;
        (*nrows)++;
        if (it->hint[0] && *nrows < MENU_ROWS_MAX) {
            rows[*nrows].item = it;
            rows[*nrows].depth = depth;
            rows[*nrows].selectable = false;
            rows[*nrows].is_hint = true;
            (*nrows)++;
        }
        /* Groups always show their children (section bodies); submenus only
         * when expanded. A top-level group's children render at the same
         * indent so the section reads flush-left like the `/mcp` picker. */
        if (it->children &&
            (it->kind == TUI_MENU_GROUP || (it->kind == TUI_MENU_SUBMENU && it->expanded))) {
            int cd = it->kind == TUI_MENU_GROUP ? depth : depth + 1;
            menu_flatten_level(it->children, it->child_count, cd, rows, nrows);
        }
    }
}

static const char *menu_badge_glyph(const tui_glyphs_t *g, tui_menu_badge_t b) {
    switch (b) {
        case TUI_MENU_BADGE_OK:
            return g->ok;
        case TUI_MENU_BADGE_FAIL:
            return g->fail;
        case TUI_MENU_BADGE_WARN:
            return g->warn;
        case TUI_MENU_BADGE_DISABLED:
            return g->circle_open;
        case TUI_MENU_BADGE_ACTIVE:
            return g->bullet;
        default:
            return NULL;
    }
}

static const char *menu_badge_color(tui_menu_badge_t b) {
    switch (b) {
        case TUI_MENU_BADGE_OK:
            return TUI_BBLUE;
        case TUI_MENU_BADGE_FAIL:
            return TUI_BRED;
        case TUI_MENU_BADGE_WARN:
            return TUI_BYELLOW;
        case TUI_MENU_BADGE_DISABLED:
            return "\033[38;5;244m";
        case TUI_MENU_BADGE_ACTIVE:
            return TUI_BGREEN;
        default:
            return TUI_RESET;
    }
}

/* Render one content row to stderr (no trailing newline). */
static void menu_render_row(const menu_row_t *r, bool is_selected, const char *accent,
                            const tui_glyphs_t *g) {
    tui_menu_item_t *it = r->item;
    int indent = 2 + r->depth * 2;

    if (r->is_hint) {
        fprintf(stderr, "%*s  %s%s%s", indent, "", "\033[38;5;240m", it->hint, TUI_RESET);
        return;
    }
    if (it->kind == TUI_MENU_SEPARATOR)
        return; /* blank line */

    if (it->kind == TUI_MENU_GROUP) {
        fprintf(stderr, "%*s%s%s%s%s", indent, "", TUI_BOLD, TUI_BWHITE, it->label, TUI_RESET);
        return;
    }

    /* selection caret */
    if (is_selected)
        fprintf(stderr, "%*s%s%s%s ", r->depth * 2, "", accent,
                g->arrow_right ? "\xe2\x80\xba" : ">", TUI_RESET);
    else
        fprintf(stderr, "%*s  ", r->depth * 2, "");

    /* expand caret for submenus */
    if (it->kind == TUI_MENU_SUBMENU) {
        const char *car = it->expanded ? "\xe2\x96\xbe" : "\xe2\x96\xb8"; /* ▾ / ▸ */
        fprintf(stderr, "%s%s%s ", TUI_DIM, car, TUI_RESET);
    } else if (it->kind == TUI_MENU_ACTION) {
        fprintf(stderr, "%s%s%s ", TUI_DIM, g->arrow_right ? g->arrow_right : "->", TUI_RESET);
    }

    /* label */
    const char *lcol = it->disabled ? "\033[38;5;244m" : is_selected ? accent : TUI_RESET;
    fprintf(stderr, "%s%s%s", lcol, it->label, TUI_RESET);

    /* badge */
    const char *bg = menu_badge_glyph(g, it->badge);
    if (bg) {
        const char *bc = menu_badge_color(it->badge);
        fprintf(stderr, " %s%s%s%s", TUI_DIM TUI_SEP " " TUI_RESET, bc, bg, TUI_RESET);
        if (it->badge_text[0])
            fprintf(stderr, " %s%s%s", bc, it->badge_text, TUI_RESET);
    }

    /* trailing detail */
    if (it->detail[0])
        fprintf(stderr, " %s%s %s%s", TUI_DIM TUI_SEP " ", it->detail, TUI_RESET, "");
}

/* Erase `n` previously drawn lines, leaving cursor at the block's top column 0. */
static void menu_erase_block(int n) {
    if (n <= 0)
        return;
    /* cursor is just below the block; move up n lines, clearing each */
    for (int i = 0; i < n; i++)
        fprintf(stderr, "\033[1A\033[2K\r");
}

int tui_menu_run(tui_menu_t *m, tui_menu_item_t **out_item) {
    if (out_item)
        *out_item = NULL;
    if (!m || !m->items || m->item_count == 0)
        return TUI_MENU_CANCELLED;

    const tui_glyphs_t *g = tui_glyph();
    const char *accent = m->accent ? m->accent : TUI_BCYAN;
    bool tty = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);

    static menu_row_t rows[MENU_ROWS_MAX];
    int nrows = 0;

    /* Establish initial selection on the first selectable row. */
    int sel = m->selected;

    int prev_lines = 0; /* lines drawn last frame (for in-place redraw) */
    int result = TUI_MENU_CANCELLED;
    bool first = true;

    tui_term_lock();
    tui_cursor_hide();

    while (1) {
        /* (Re)flatten — submenu expansion changes the row set each frame. */
        nrows = 0;
        menu_flatten_level(m->items, m->item_count, 0, rows, &nrows);

        /* Clamp / snap selection to a selectable row. */
        if (sel < 0)
            sel = 0;
        if (sel >= nrows)
            sel = nrows - 1;
        if (!rows[sel].selectable) {
            int s = -1;
            for (int i = sel; i < nrows; i++)
                if (rows[i].selectable) {
                    s = i;
                    break;
                }
            if (s < 0)
                for (int i = sel; i >= 0; i--)
                    if (rows[i].selectable) {
                        s = i;
                        break;
                    }
            if (s >= 0)
                sel = s;
        }

        /* Viewport: header + rows + footer must fit; scroll around selection. */
        int term_h = tui_term_height();
        int chrome = (m->title[0] ? 1 : 0) + (m->subtitle[0] ? 1 : 0) + 2 /*footer+gap*/;
        int vis = m->max_visible > 0 ? m->max_visible : (term_h - chrome - 2);
        if (vis < 3)
            vis = 3;
        if (vis > nrows)
            vis = nrows;

        int top = 0;
        if (nrows > vis) {
            top = sel - vis / 2;
            if (top < 0)
                top = 0;
            if (top > nrows - vis)
                top = nrows - vis;
        }

        /* Redraw in place: erase the previous frame first. */
        if (!first)
            menu_erase_block(prev_lines);
        first = false;

        int lines = 0;
        if (m->title[0]) {
            fprintf(stderr, "%s%s%s%s\r\n", TUI_BOLD, TUI_BWHITE, m->title, TUI_RESET);
            lines++;
        }
        if (m->subtitle[0]) {
            fprintf(stderr, "%s%s%s\r\n", TUI_DIM, m->subtitle, TUI_RESET);
            lines++;
        }
        if (top > 0) {
            fprintf(stderr, "  %s\xe2\x96\xb4 %d more%s\r\n", TUI_DIM, top, TUI_RESET);
            lines++;
        }
        for (int i = top; i < top + vis && i < nrows; i++) {
            menu_render_row(&rows[i], i == sel, accent, g);
            fprintf(stderr, "\r\n");
            lines++;
        }
        if (top + vis < nrows) {
            fprintf(stderr, "  %s\xe2\x96\xbe %d more%s\r\n", TUI_DIM, nrows - (top + vis),
                    TUI_RESET);
            lines++;
        }
        fprintf(stderr, "%s%s/%s navigate %s %s/%s expand %s Enter select %s Esc cancel%s\r\n",
                TUI_DIM, "\xe2\x86\x91", "\xe2\x86\x93", TUI_SEP, "\xe2\x86\x90", "\xe2\x86\x92",
                TUI_SEP, TUI_SEP, TUI_RESET);
        lines++;
        fflush(stderr);
        prev_lines = lines;

        if (!tty) {
            result = TUI_MENU_CANCELLED;
            break;
        }

        /* ── Input ── */
        int ch = read_single_char();
        if (ch == 27) {
            int c2 = read_single_char();
            if (c2 == '[') {
                int c3 = read_single_char();
                if (c3 == 'A')
                    ch = 'k'; /* up */
                else if (c3 == 'B')
                    ch = 'j'; /* down */
                else if (c3 == 'C')
                    ch = 'l'; /* right */
                else if (c3 == 'D')
                    ch = 'h'; /* left */
                else
                    continue;
            } else {
                result = TUI_MENU_CANCELLED;
                break;
            } /* bare ESC */
        }

        if (ch == 'q' || ch == 'Q') {
            result = TUI_MENU_CANCELLED;
            break;
        }

        if (ch == 'k') { /* prev selectable */
            for (int i = sel - 1; i >= 0; i--)
                if (rows[i].selectable) {
                    sel = i;
                    break;
                }
        } else if (ch == 'j') { /* next selectable */
            for (int i = sel + 1; i < nrows; i++)
                if (rows[i].selectable) {
                    sel = i;
                    break;
                }
        } else if (ch == 'g') {
            for (int i = 0; i < nrows; i++)
                if (rows[i].selectable) {
                    sel = i;
                    break;
                }
        } else if (ch == 'G') {
            for (int i = nrows - 1; i >= 0; i--)
                if (rows[i].selectable) {
                    sel = i;
                    break;
                }
        } else if (ch == 'l') { /* expand submenu / right */
            tui_menu_item_t *it = rows[sel].item;
            if (it->kind == TUI_MENU_SUBMENU)
                it->expanded = true;
        } else if (ch == 'h') { /* collapse / jump to parent */
            tui_menu_item_t *it = rows[sel].item;
            if (it->kind == TUI_MENU_SUBMENU && it->expanded) {
                it->expanded = false;
            } else if (rows[sel].depth > 0) {
                int d = rows[sel].depth;
                for (int i = sel - 1; i >= 0; i--)
                    if (rows[i].depth < d && rows[i].selectable) {
                        sel = i;
                        break;
                    }
            }
        } else if (ch == '\r' || ch == '\n' || ch == ' ') {
            tui_menu_item_t *it = rows[sel].item;
            if (it->kind == TUI_MENU_SUBMENU) {
                it->expanded = !it->expanded; /* toggle */
            } else if (it->kind == TUI_MENU_ITEM || it->kind == TUI_MENU_ACTION) {
                result = it->id;
                if (out_item)
                    *out_item = it;
                break;
            }
        }
    }

    /* Erase the menu block so the surrounding scrollback stays clean. */
    menu_erase_block(prev_lines);
    m->selected = sel;
    tui_cursor_show();
    fflush(stderr);
    tui_term_unlock();
    return result;
}
