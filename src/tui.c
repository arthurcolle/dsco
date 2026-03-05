#include "tui.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* ── Box character sets ───────────────────────────────────────────────── */

static const tui_box_chars_t BOX_ROUND_CHARS = {
    "╭", "╮", "╰", "╯", "─", "│", "├", "┤"
};
static const tui_box_chars_t BOX_SINGLE_CHARS = {
    "┌", "┐", "└", "┘", "─", "│", "├", "┤"
};
static const tui_box_chars_t BOX_DOUBLE_CHARS = {
    "╔", "╗", "╚", "╝", "═", "║", "╠", "╣"
};
static const tui_box_chars_t BOX_HEAVY_CHARS = {
    "┏", "┓", "┗", "┛", "━", "┃", "┣", "┫"
};
static const tui_box_chars_t BOX_ASCII_CHARS = {
    "+", "+", "+", "+", "-", "|", "+", "+"
};

const tui_box_chars_t *tui_box_chars(tui_box_style_t style) {
    switch (style) {
        case BOX_ROUND:  return &BOX_ROUND_CHARS;
        case BOX_SINGLE: return &BOX_SINGLE_CHARS;
        case BOX_DOUBLE: return &BOX_DOUBLE_CHARS;
        case BOX_HEAVY:  return &BOX_HEAVY_CHARS;
        case BOX_ASCII:  return &BOX_ASCII_CHARS;
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

void tui_cursor_hide(void)    { fprintf(stderr, "\033[?25l"); }
void tui_cursor_show(void)    { fprintf(stderr, "\033[?25h"); }
void tui_cursor_move(int r, int c) { fprintf(stderr, "\033[%d;%dH", r, c); }
void tui_clear_screen(void)   { fprintf(stderr, "\033[2J\033[H"); }
void tui_clear_line(void)     { fprintf(stderr, "\033[2K\r"); }
void tui_save_cursor(void)    { fprintf(stderr, "\033[s"); }
void tui_restore_cursor(void) { fprintf(stderr, "\033[u"); }

/* ── Visible string length (strip ANSI) ───────────────────────────────── */

static int visible_len(const char *s) {
    int len = 0;
    bool in_esc = false;
    for (const char *p = s; *p; p++) {
        if (*p == '\033') { in_esc = true; continue; }
        if (in_esc) { if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) in_esc = false; continue; }
        /* Skip UTF-8 continuation bytes */
        if (((unsigned char)*p & 0xC0) == 0x80) continue;
        len++;
    }
    return len;
}

/* ── Repeat a string n times to stderr ────────────────────────────────── */

static void repeat_str(const char *s, int n) {
    for (int i = 0; i < n; i++) fprintf(stderr, "%s", s);
}

/* ── Box drawing ──────────────────────────────────────────────────────── */

void tui_box(const char *title, const char *body, tui_box_style_t style,
             const char *border_color, int width) {
    if (width <= 0) width = tui_term_width();
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
        if (remaining > 0) repeat_str(bc->h, remaining);
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
            int copy_len = line_len < (int)sizeof(line_buf) - 1 ? line_len : (int)sizeof(line_buf) - 1;
            memcpy(line_buf, p, copy_len);
            line_buf[copy_len] = '\0';
            fprintf(stderr, "%s", line_buf);

            /* Pad to width */
            int vis = visible_len(line_buf);
            int pad = width - 3 - vis;
            if (pad > 0) for (int i = 0; i < pad; i++) fputc(' ', stderr);

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
    if (width <= 0) width = tui_term_width();
    const tui_box_chars_t *bc = tui_box_chars(style);
    const char *col = color ? color : "";
    const char *rst = color ? TUI_RESET : "";
    fprintf(stderr, "%s", col);
    repeat_str(bc->h, width);
    fprintf(stderr, "%s\n", rst);
}

/* ── Panel ────────────────────────────────────────────────────────────── */

void tui_panel(const tui_panel_t *p) {
    tui_box(p->title, p->body, p->style,
            p->color ? p->color : TUI_DIM,
            p->width > 0 ? p->width : tui_term_width());
}

/* ── Spinners ─────────────────────────────────────────────────────────── */

/* Spinner frames — resolved from glyph tier at runtime */
static void spinner_get_frames(tui_spinner_type_t type,
                               const char *const **out_frames, int *out_count) {
    const tui_glyphs_t *g = tui_glyph();
    switch (type) {
    case SPINNER_DOTS:
        *out_frames = g->spin_dots; *out_count = g->spin_dots_n; break;
    case SPINNER_BRAILLE:
        *out_frames = g->spin_thick; *out_count = g->spin_thick_n; break;
    case SPINNER_LINE:
        *out_frames = g->spin_line; *out_count = g->spin_line_n; break;
    case SPINNER_ARROW:
        *out_frames = g->spin_arrow; *out_count = g->spin_arrow_n; break;
    case SPINNER_STAR:
        *out_frames = g->spin_star; *out_count = g->spin_star_n; break;
    case SPINNER_PULSE:
        *out_frames = g->spin_pulse; *out_count = g->spin_pulse_n; break;
    default:
        *out_frames = g->spin_dots; *out_count = g->spin_dots_n; break;
    }
}

void tui_spinner_init(tui_spinner_t *s, tui_spinner_type_t type,
                      const char *label, const char *color) {
    s->type = type;
    s->frame = 0;
    s->label = label;
    s->color = color ? color : TUI_CYAN;
    s->active = true;
}

void tui_spinner_tick(tui_spinner_t *s) {
    if (!s->active) return;
    const char *const *frames; int count;
    spinner_get_frames(s->type, &frames, &count);
    if (count <= 0) return;

    tui_clear_line();
    fprintf(stderr, "  %s%s%s %s%s%s",
            s->color, frames[s->frame % count], TUI_RESET,
            TUI_DIM, s->label ? s->label : "", TUI_RESET);
    fflush(stderr);
    s->frame++;
}

void tui_spinner_done(tui_spinner_t *s, const char *final_label) {
    s->active = false;
    tui_clear_line();
    fprintf(stderr, "  %s%s%s %s%s%s\n",
            TUI_GREEN, tui_glyph()->florette, TUI_RESET,
            TUI_DIM, final_label ? final_label : "done", TUI_RESET);
}

/* ── Progress bar ─────────────────────────────────────────────────────── */

void tui_progress(const char *label, double pct, int width,
                  const char *fill_color, const char *empty_color) {
    if (width <= 0) width = tui_term_width() - 20;
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;

    int label_len = label ? visible_len(label) : 0;
    int bar_width = width - label_len - 10;
    if (bar_width < 10) bar_width = 10;

    int filled = (int)(pct * bar_width);
    int empty = bar_width - filled;

    const char *fc = fill_color ? fill_color : TUI_GREEN;
    const char *ec = empty_color ? empty_color : TUI_DIM;

    tui_clear_line();
    if (label) fprintf(stderr, "  %s ", label);
    fprintf(stderr, "%s", fc);
    for (int i = 0; i < filled; i++) fprintf(stderr, "█");
    fprintf(stderr, "%s", ec);
    for (int i = 0; i < empty; i++) fprintf(stderr, "░");
    fprintf(stderr, "%s %3.0f%%\n", TUI_RESET, pct * 100);
}

/* ── Table ────────────────────────────────────────────────────────────── */

void tui_table_init(tui_table_t *t, int cols, const char *header_color) {
    memset(t, 0, sizeof(*t));
    t->col_count = cols < TUI_TABLE_MAX_COLS ? cols : TUI_TABLE_MAX_COLS;
    t->header_color = header_color ? header_color : TUI_CYAN;
    t->border_color = TUI_DIM;
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
    if (t->row_count >= TUI_TABLE_MAX_ROWS) return;
    va_list args;
    va_start(args, t);
    for (int i = 0; i < t->col_count; i++) {
        t->rows[t->row_count][i] = va_arg(args, const char *);
    }
    t->row_count++;
    va_end(args);
}

void tui_table_render(const tui_table_t *t, int width) {
    if (width <= 0) width = tui_term_width();

    /* Calculate column widths */
    int col_widths[TUI_TABLE_MAX_COLS] = {0};
    for (int c = 0; c < t->col_count; c++) {
        if (t->headers[c]) {
            int w = visible_len(t->headers[c]);
            if (w > col_widths[c]) col_widths[c] = w;
        }
        for (int r = 0; r < t->row_count; r++) {
            if (t->rows[r][c]) {
                int w = visible_len(t->rows[r][c]);
                if (w > col_widths[c]) col_widths[c] = w;
            }
        }
    }

    const tui_box_chars_t *bc = tui_box_chars(t->style);
    const char *dcol = t->border_color ? t->border_color : "";
    const char *rst = TUI_RESET;

    /* Header */
    fprintf(stderr, "  %s", dcol);
    for (int c = 0; c < t->col_count; c++) {
        fprintf(stderr, "%s%-*s%s",
                t->header_color ? t->header_color : "",
                col_widths[c] + 2,
                t->headers[c] ? t->headers[c] : "",
                rst);
        if (c < t->col_count - 1) fprintf(stderr, " %s%s%s ", dcol, bc->v, rst);
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
            if (pad > 0) for (int i = 0; i < pad; i++) fputc(' ', stderr);
            if (c < t->col_count - 1)
                fprintf(stderr, " %s%s%s ", dcol, bc->v, rst);
        }
        fprintf(stderr, "\n");
    }
}

/* ── Badges & tags ────────────────────────────────────────────────────── */

void tui_badge(const char *text, const char *fg, const char *bg) {
    fprintf(stderr, " %s%s %s %s ", bg ? bg : TUI_BG_BLUE, fg ? fg : TUI_WHITE,
            text, TUI_RESET);
}

void tui_tag(const char *text, const char *color) {
    fprintf(stderr, "%s[%s]%s", color ? color : TUI_CYAN, text, TUI_RESET);
}

/* ── Convenience output ───────────────────────────────────────────────── */

void tui_header(const char *text, const char *color) {
    fprintf(stderr, "\n%s%s%s %s%s\n",
            color ? color : TUI_BWHITE, TUI_BOLD, tui_glyph()->bullet, text, TUI_RESET);
}

void tui_subheader(const char *text) {
    fprintf(stderr, "  %s%s%s\n", TUI_DIM, text, TUI_RESET);
}

void tui_info(const char *text) {
    fprintf(stderr, "  %sℹ%s %s\n", TUI_BLUE, TUI_RESET, text);
}

void tui_success(const char *text) {
    fprintf(stderr, "  %s✓%s %s\n", TUI_GREEN, TUI_RESET, text);
}

void tui_warning(const char *text) {
    fprintf(stderr, "  %s⚠%s %s\n", TUI_YELLOW, TUI_RESET, text);
}

void tui_error(const char *text) {
    fprintf(stderr, "  %s✗%s %s\n", TUI_RED, TUI_RESET, text);
}

/* Forward declarations for functions defined later */
static void welcome_animated(const char *model, int tool_count, const char *version);
static void fg_color_auto(tui_rgb_t c);

/* ── Welcome banner ───────────────────────────────────────────────────── */

void tui_welcome(const char *model, int tool_count, const char *version) {
    /* Use animated gradient version when truecolor or 256-color is available */
    if (tui_detect_color_level() >= TUI_COLOR_256) {
        welcome_animated(model, tool_count, version);
        return;
    }

    /* Fallback: original 16-color instant print */
    int w = tui_term_width();

    const char *logo =
        "     %s██████╗%s  %s███████╗%s  %s██████╗%s  %s██████╗%s \n"
        "     %s██╔══██╗%s %s██╔════╝%s %s██╔════╝%s %s██╔═══██╗%s\n"
        "     %s██║  ██║%s %s███████╗%s %s██║%s      %s██║   ██║%s\n"
        "     %s██║  ██║%s %s╚════██║%s %s██║%s      %s██║   ██║%s\n"
        "     %s██████╔╝%s %s███████║%s %s╚██████╗%s %s╚██████╔╝%s\n"
        "     %s╚═════╝%s  %s╚══════╝%s  %s╚═════╝%s  %s╚═════╝%s \n";

    /* Full-width top border */
    fprintf(stderr, "\n%s", TUI_DIM);
    for (int i = 0; i < w; i++) fprintf(stderr, "━");
    fprintf(stderr, "%s\n\n", TUI_RESET);

    fprintf(stderr, logo,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BMAGENTA, TUI_RESET, TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET);
    fprintf(stderr, "\n");

    char info[512];
    snprintf(info, sizeof(info),
             "%s✦%s %sv%s%s  %s·%s  %s%s%s  %s·%s  %s%d tools%s  %s·%s  %sstreaming%s  %s·%s  %sswarm-ready%s %s✦%s",
             TUI_BMAGENTA, TUI_RESET,
             TUI_BOLD, version, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_CYAN, model, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_GREEN, tool_count, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_BMAGENTA, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_BYELLOW, TUI_RESET,
             TUI_BMAGENTA, TUI_RESET);
    fprintf(stderr, "     %s\n\n", info);

    fprintf(stderr, "     %s◆%s %sAST introspection%s  %s·%s  %s◆%s %sSub-agent swarms%s  %s·%s  %s◆%s %sStreaming I/O%s\n",
            TUI_BMAGENTA, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_DIM, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_DIM, TUI_RESET,
            TUI_BBLUE, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr, "     %s◆%s %sCrypto toolkit%s     %s·%s  %s◆%s %sCoroutine pipelines%s %s·%s %s◆%s %sPlugin system%s\n",
            TUI_BRED, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_DIM, TUI_RESET,
            TUI_BGREEN, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_DIM, TUI_RESET,
            TUI_BYELLOW, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr, "\n");

    /* Full-width bottom border */
    fprintf(stderr, "%s", TUI_DIM);
    for (int i = 0; i < w; i++) fprintf(stderr, "━");
    fprintf(stderr, "%s\n\n", TUI_RESET);
}

/* ── Streaming wrappers ───────────────────────────────────────────────── */

static bool s_stream_active = false;

void tui_stream_start(void) {
    s_stream_active = true;
}

void tui_stream_text(const char *text) {
    fputs(text, stdout);
    fflush(stdout);
}

void tui_stream_tool(const char *name, const char *id) {
    (void)id;
    if (s_stream_active) {
        printf("\n");
        s_stream_active = false;
    }
    fprintf(stderr, "  %s%s⚡%s %s%s%s\n",
            TUI_BOLD, TUI_CYAN, TUI_RESET,
            TUI_DIM, name, TUI_RESET);
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
        if (len > 100) len = 100;
        memcpy(trunc, preview, len);
        trunc[len] = '\0';
    } else {
        trunc[0] = '\0';
    }

    fprintf(stderr, "    %s%s%s %s%s%s\n",
            color, icon, TUI_RESET,
            TUI_DIM, trunc, TUI_RESET);
}

void tui_stream_end(void) {
    if (s_stream_active) {
        printf("\n");
        s_stream_active = false;
    }
}

/* ── Swarm panel ──────────────────────────────────────────────────────── */

void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width) {
    if (width <= 0) width = tui_term_width();

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

        fprintf(stderr, "  %s%s%s %s#%d%s %s%s%s",
                status_color, status_icon, TUI_RESET,
                TUI_DIM, e->id, TUI_RESET,
                TUI_BOLD, e->task, TUI_RESET);

        if (e->progress > 0 && e->progress < 1.0) {
            int bar_w = 20;
            int filled = (int)(e->progress * bar_w);
            fprintf(stderr, " %s", TUI_GREEN);
            for (int j = 0; j < filled; j++) fprintf(stderr, "%s", tui_glyph()->block_med);
            fprintf(stderr, "%s", TUI_DIM);
            for (int j = filled; j < bar_w; j++) fprintf(stderr, "▯");
            fprintf(stderr, "%s", TUI_RESET);
        }
        fprintf(stderr, "\n");

        if (e->last_output && strlen(e->last_output) > 0) {
            /* Show last line of output */
            const char *last = e->last_output;
            const char *scan = last;
            while (*scan) {
                if (*scan == '\n' && *(scan + 1)) last = scan + 1;
                scan++;
            }
            int out_len = (int)strlen(last);
            if (out_len > width - 8) out_len = width - 8;
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
#define NF(hex) hex  /* just documentation — we use raw UTF-8 bytes */

/* ── Nerd Font tier: modern 2026 terminals with patched fonts ──────── */
static const tui_glyphs_t GLYPHS_NERD = {
    /* Status — Nerd Font codicons/FA */
    .ok   = "\xef\x80\x8c",             /* nf-fa-check        U+F00C */
    .fail = "\xef\x80\x8d",             /* nf-fa-times        U+F00D */
    .warn = "\xef\x81\xb1",             /* nf-fa-warning      U+F071 */
    .info = "\xef\x84\xa9",             /* nf-fa-info_circle  U+F129 */
    /* Bullets — Nerd Font */
    .bullet      = "\xef\x84\x92",      /* nf-fa-circle       U+F111 */
    .circle_open = "\xef\x84\x90",      /* nf-fa-circle_o     U+F10C */
    .circle_dot  = "\xef\x84\x92",      /* nf-fa-circle       U+F111 */
    .circle_ring = "\xef\x84\x90",      /* nf-fa-circle_o     U+F10C */
    .diamond     = "\xef\x88\x99",      /* nf-fa-diamond      U+F219 */
    .diamond_open= "\xef\x88\x99",      /* nf-fa-diamond      U+F219 */
    .sparkle     = "\xef\x83\xab",      /* nf-fa-star         U+F0EB -> lightbulb */
    .florette    = "\xef\x80\x8c",      /* nf-fa-check        U+F00C */
    /* Arrows — Nerd Font */
    .arrow_right = "\xef\x81\xa1",      /* nf-fa-arrow_right  U+F061 */
    .arrow_left  = "\xef\x81\xa0",      /* nf-fa-arrow_left   U+F060 */
    .arrow_up    = "\xef\x81\xa2",      /* nf-fa-arrow_up     U+F062 */
    .arrow_down  = "\xef\x81\xa3",      /* nf-fa-arrow_down   U+F063 */
    .arrow_cycle = "\xef\x80\xa1",      /* nf-fa-refresh      U+F021 */
    /* Blocks — standard Unicode (universally supported) */
    .block_full  = "\xe2\x96\x88",
    .block_med   = "\xe2\x96\xae",
    .block_light = "\xe2\x96\x91",
    .block_dark  = "\xe2\x96\x93",
    .vblock = {" ",
        "\xe2\x96\x81","\xe2\x96\x82","\xe2\x96\x83","\xe2\x96\x84",
        "\xe2\x96\x85","\xe2\x96\x86","\xe2\x96\x87","\xe2\x96\x88"},
    /* Spinners — braille (universally supported with nerd fonts) */
    .spin_dots = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
        "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
        "\xe2\xa0\x87","\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    .spin_thick = {
        "\xe2\xa3\xbe","\xe2\xa3\xbd","\xe2\xa3\xbb","\xe2\xa2\xbf",
        "\xe2\xa1\xbf","\xe2\xa3\x9f","\xe2\xa3\xaf","\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    .spin_orbit = {
        "\xe2\x97\x9c","\xe2\x97\x9d","\xe2\x97\x9e","\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0","\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {
        "\xe2\x97\x90","\xe2\x97\x93","\xe2\x97\x91","\xe2\x97\x92"},
    .spin_pulse_n = 4,
    .spin_line = {"-","\\","|","/"},
    .spin_line_n = 4,
    .spin_arrow = {
        "\xe2\x86\x90","\xe2\x86\x96","\xe2\x86\x91","\xe2\x86\x97",
        "\xe2\x86\x92","\xe2\x86\x98","\xe2\x86\x93","\xe2\x86\x99"},
    .spin_arrow_n = 8,
    .spin_star = {
        "\xe2\x9c\xb6","\xe2\x9c\xb8","\xe2\x9c\xb9",
        "\xe2\x9c\xba","\xe2\x9c\xb9","\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* Icons — Nerd Font */
    .icon_think     = "\xef\x83\xab",   /* nf-fa-lightbulb_o  U+F0EB */
    .icon_lightning  = "\xef\x83\xa7",   /* nf-fa-bolt         U+F0E7 */
    .icon_gear       = "\xef\x80\x93",   /* nf-fa-cog          U+F013 */
    .icon_timer      = "\xef\x80\x97",   /* nf-fa-clock_o      U+F017 */
    .icon_lock       = "\xef\x80\xa3",   /* nf-fa-lock         U+F023 */
    .icon_money      = "\xef\x85\x95",   /* nf-fa-money        U+F155 */
    .icon_globe      = "\xef\x82\xac",   /* nf-fa-globe        U+F0AC */
    .icon_rocket     = "\xef\x84\xb5",   /* nf-fa-rocket       U+F135 */
    .icon_fire       = "\xef\x81\xad",   /* nf-fa-fire         U+F06D */
    .icon_link       = "\xef\x83\x81",   /* nf-fa-link         U+F0C1 */
    .icon_eyes       = "\xef\x81\xae",   /* nf-fa-eye          U+F06E */
    /* Nerd Font extras */
    .icon_folder     = "\xef\x81\xbc",   /* nf-fa-folder       U+F07C */
    .icon_file       = "\xef\x85\x9b",   /* nf-fa-file_text_o  U+F15B (really U+F0F6) */
    .icon_code       = "\xef\x84\xa1",   /* nf-fa-code         U+F121 */
    .icon_terminal   = "\xef\x84\xa0",   /* nf-fa-terminal     U+F120 */
    .icon_git        = "\xef\x84\xa6",   /* nf-fa-code_fork    U+F126 */
    .icon_database   = "\xef\x87\x80",   /* nf-fa-database     U+F1C0 */
    .icon_cloud      = "\xef\x83\x82",   /* nf-fa-cloud        U+F0C2 */
    .icon_bug        = "\xef\x86\x88",   /* nf-fa-bug          U+F188 */
    .icon_cpu        = "\xef\x88\x9b",   /* nf-fa-microchip    U+F21B (close) */
    .icon_network    = "\xef\x83\xa0",   /* nf-fa-sitemap      U+F0E8 */
    .icon_key        = "\xef\x82\x84",   /* nf-fa-key          U+F084 */
    .icon_shield     = "\xef\x84\xb2",   /* nf-fa-shield       U+F132 */
    .icon_search     = "\xef\x80\x82",   /* nf-fa-search       U+F002 */
    .icon_download   = "\xef\x80\x99",   /* nf-fa-download     U+F019 */
    .icon_upload     = "\xef\x82\x93",   /* nf-fa-upload       U+F093 */
    .icon_sync       = "\xef\x80\xa1",   /* nf-fa-refresh      U+F021 */
    .icon_play       = "\xef\x81\x8b",   /* nf-fa-play         U+F04B */
    .icon_pause      = "\xef\x81\x8c",   /* nf-fa-pause        U+F04C */
    .icon_stop       = "\xef\x81\x8d",   /* nf-fa-stop         U+F04D */
    .icon_skip       = "\xef\x81\x8e",   /* nf-fa-forward      U+F04E */
    .icon_chat       = "\xef\x81\xb5",   /* nf-fa-comment      U+F075 */
    .icon_robot      = "\xef\x84\xa4",   /* nf-fa-android      U+F17B (close) */
    .icon_brain      = "\xef\x83\xab",   /* nf-fa-lightbulb_o  U+F0EB */
    .icon_wand       = "\xef\x83\x90",   /* nf-fa-magic        U+F0D0 */
    .icon_graph      = "\xef\x83\xa0",   /* nf-fa-sitemap      U+F0E8 */
    /* Powerline separators */
    .pl_right      = "\xee\x82\xb0",     /* U+E0B0 */
    .pl_right_thin = "\xee\x82\xb1",     /* U+E0B1 */
    .pl_left       = "\xee\x82\xb2",     /* U+E0B2 */
    .pl_left_thin  = "\xee\x82\xb3",     /* U+E0B3 */
    .pl_round_right= "\xee\x82\xb4",     /* U+E0B4 */
    .pl_round_left = "\xee\x82\xb6",     /* U+E0B6 */
    /* Box drawing */
    .hline = "\xe2\x94\x80", .hline_heavy = "\xe2\x94\x81",
    .vline = "\xe2\x94\x82",
    .corner_tl = "\xe2\x95\xad", .corner_tr = "\xe2\x95\xae",
    .corner_bl = "\xe2\x95\xb0", .corner_br = "\xe2\x95\xaf",
    /* Trail dots */
    .dot_large = "\xe2\x80\xa2", .dot_medium = "\xc2\xb7", .dot_small = ".",
};

/* ── Full tier: emoji + all Unicode ──────────────────────────────────── */
static const tui_glyphs_t GLYPHS_FULL = {
    /* Status */
    .ok = "\xe2\x9c\x93",             /* ✓ */
    .fail = "\xe2\x9c\x97",           /* ✗ */
    .warn = "\xe2\x9a\xa0",           /* ⚠ */
    .info = "\xe2\x84\xb9",           /* ℹ */
    /* Bullets */
    .bullet = "\xe2\x97\x8f",         /* ● */
    .circle_open = "\xe2\x97\x8b",    /* ○ */
    .circle_dot = "\xe2\x97\x89",     /* ◉ */
    .circle_ring = "\xe2\x97\x8e",    /* ◎ */
    .diamond = "\xe2\x97\x86",        /* ◆ */
    .diamond_open = "\xe2\x97\x87",   /* ◇ */
    .sparkle = "\xe2\x9c\xa6",        /* ✦ */
    .florette = "\xe2\x9c\xbf",       /* ✿ */
    /* Arrows */
    .arrow_right = "\xe2\x86\x92",    /* → */
    .arrow_left = "\xe2\x86\x90",     /* ← */
    .arrow_up = "\xe2\x96\xb2",       /* ▲ */
    .arrow_down = "\xe2\x96\xbc",     /* ▼ */
    .arrow_cycle = "\xe2\x86\xbb",    /* ↻ */
    /* Blocks */
    .block_full = "\xe2\x96\x88",     /* █ */
    .block_med = "\xe2\x96\xae",      /* ▮ */
    .block_light = "\xe2\x96\x91",    /* ░ */
    .block_dark = "\xe2\x96\x93",     /* ▓ */
    .vblock = {" ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"},
    /* Spinners — braille dots */
    .spin_dots = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
        "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
        "\xe2\xa0\x87","\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    /* Spinners — thick braille */
    .spin_thick = {
        "\xe2\xa3\xbe","\xe2\xa3\xbd","\xe2\xa3\xbb","\xe2\xa2\xbf",
        "\xe2\xa1\xbf","\xe2\xa3\x9f","\xe2\xa3\xaf","\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    /* Spinners — orbital arcs */
    .spin_orbit = {
        "\xe2\x97\x9c","\xe2\x97\x9d","\xe2\x97\x9e","\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0","\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    /* Spinners — pulse */
    .spin_pulse = {
        "\xe2\x97\x90","\xe2\x97\x93","\xe2\x97\x91","\xe2\x97\x92"},
    .spin_pulse_n = 4,
    /* Spinners — line */
    .spin_line = {"-","\\","|","/"},
    .spin_line_n = 4,
    /* Spinners — arrow */
    .spin_arrow = {
        "\xe2\x86\x90","\xe2\x86\x96","\xe2\x86\x91","\xe2\x86\x97",
        "\xe2\x86\x92","\xe2\x86\x98","\xe2\x86\x93","\xe2\x86\x99"},
    .spin_arrow_n = 8,
    /* Spinners — star */
    .spin_star = {
        "\xe2\x9c\xb6","\xe2\x9c\xb8","\xe2\x9c\xb9",
        "\xe2\x9c\xba","\xe2\x9c\xb9","\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* Icons — emoji */
    .icon_think = "\xf0\x9f\xa7\xa0",     /* 🧠 */
    .icon_lightning = "\xe2\x9a\xa1",      /* ⚡ */
    .icon_gear = "\xe2\x9a\x99",           /* ⚙ */
    .icon_timer = "\xe2\x8f\xb1",          /* ⏱ */
    .icon_lock = "\xf0\x9f\x94\x92",       /* 🔒 */
    .icon_money = "\xf0\x9f\x92\xb0",      /* 💰 */
    .icon_globe = "\xf0\x9f\x8c\x90",      /* 🌐 */
    .icon_rocket = "\xf0\x9f\x9a\x80",     /* 🚀 */
    .icon_fire = "\xf0\x9f\x94\xa5",        /* 🔥 */
    .icon_link = "\xf0\x9f\x94\x97",        /* 🔗 */
    .icon_eyes = "\xf0\x9f\x91\x80",        /* 👀 */
    /* Box drawing */
    .hline = "\xe2\x94\x80",              /* ─ */
    .hline_heavy = "\xe2\x94\x81",        /* ━ */
    .vline = "\xe2\x94\x82",              /* │ */
    .corner_tl = "\xe2\x95\xad",          /* ╭ */
    .corner_tr = "\xe2\x95\xae",          /* ╮ */
    .corner_bl = "\xe2\x95\xb0",          /* ╰ */
    .corner_br = "\xe2\x95\xaf",          /* ╯ */
    /* Trail dots */
    .dot_large = "\xe2\x80\xa2",           /* • */
    .dot_medium = "\xc2\xb7",             /* · */
    .dot_small = ".",
};

/* ── Unicode tier: BMP only, no emoji ────────────────────────────────── */
static const tui_glyphs_t GLYPHS_UNICODE = {
    .ok = "\xe2\x9c\x93", .fail = "\xe2\x9c\x97",
    .warn = "(!)", .info = "(i)",
    .bullet = "\xe2\x97\x8f", .circle_open = "\xe2\x97\x8b",
    .circle_dot = "\xe2\x97\x89", .circle_ring = "\xe2\x97\x8e",
    .diamond = "\xe2\x97\x86", .diamond_open = "\xe2\x97\x87",
    .sparkle = "\xe2\x9c\xa6", .florette = "\xe2\x9c\xbf",
    .arrow_right = "\xe2\x86\x92", .arrow_left = "\xe2\x86\x90",
    .arrow_up = "\xe2\x96\xb2", .arrow_down = "\xe2\x96\xbc",
    .arrow_cycle = "\xe2\x86\xbb",
    .block_full = "\xe2\x96\x88", .block_med = "\xe2\x96\xae",
    .block_light = "\xe2\x96\x91", .block_dark = "\xe2\x96\x93",
    .vblock = {" ",
        "\xe2\x96\x81","\xe2\x96\x82","\xe2\x96\x83","\xe2\x96\x84",
        "\xe2\x96\x85","\xe2\x96\x86","\xe2\x96\x87","\xe2\x96\x88"},
    .spin_dots = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8",
        "\xe2\xa0\xbc","\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7",
        "\xe2\xa0\x87","\xe2\xa0\x8f"},
    .spin_dots_n = 10,
    .spin_thick = {
        "\xe2\xa3\xbe","\xe2\xa3\xbd","\xe2\xa3\xbb","\xe2\xa2\xbf",
        "\xe2\xa1\xbf","\xe2\xa3\x9f","\xe2\xa3\xaf","\xe2\xa3\xb7"},
    .spin_thick_n = 8,
    .spin_orbit = {
        "\xe2\x97\x9c","\xe2\x97\x9d","\xe2\x97\x9e","\xe2\x97\x9f"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"\xe2\x97\xa0","\xe2\x97\xa1"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {
        "\xe2\x97\x90","\xe2\x97\x93","\xe2\x97\x91","\xe2\x97\x92"},
    .spin_pulse_n = 4,
    .spin_line = {"-","\\","|","/"},
    .spin_line_n = 4,
    .spin_arrow = {
        "\xe2\x86\x90","\xe2\x86\x96","\xe2\x86\x91","\xe2\x86\x97",
        "\xe2\x86\x92","\xe2\x86\x98","\xe2\x86\x93","\xe2\x86\x99"},
    .spin_arrow_n = 8,
    .spin_star = {
        "\xe2\x9c\xb6","\xe2\x9c\xb8","\xe2\x9c\xb9",
        "\xe2\x9c\xba","\xe2\x9c\xb9","\xe2\x9c\xb7"},
    .spin_star_n = 6,
    /* No emoji — use BMP symbols instead */
    .icon_think = "\xc2\xa7",              /* § */
    .icon_lightning = "\xe2\x9a\xa1",      /* ⚡ (BMP) */
    .icon_gear = "\xe2\x9a\x99",           /* ⚙ (BMP) */
    .icon_timer = "\xe2\x97\x8b",          /* ○ (fallback) */
    .icon_lock = "#",
    .icon_money = "$",
    .icon_globe = "\xe2\x97\x89",          /* ◉ */
    .icon_rocket = "\xe2\x96\xb2",         /* ▲ */
    .icon_fire = "~",
    .icon_link = "=",
    .icon_eyes = "\xe2\x97\x8b",           /* ○ */
    .hline = "\xe2\x94\x80", .hline_heavy = "\xe2\x94\x81",
    .vline = "\xe2\x94\x82",
    .corner_tl = "\xe2\x95\xad", .corner_tr = "\xe2\x95\xae",
    .corner_bl = "\xe2\x95\xb0", .corner_br = "\xe2\x95\xaf",
    .dot_large = "\xe2\x80\xa2", .dot_medium = "\xc2\xb7", .dot_small = ".",
};

/* ── ASCII tier: pure 7-bit ASCII ────────────────────────────────────── */
static const tui_glyphs_t GLYPHS_ASCII = {
    .ok = "+", .fail = "x", .warn = "!", .info = "i",
    .bullet = "*", .circle_open = "o", .circle_dot = "@", .circle_ring = "O",
    .diamond = "*", .diamond_open = "<>", .sparkle = "*", .florette = "*",
    .arrow_right = "->", .arrow_left = "<-", .arrow_up = "^", .arrow_down = "v",
    .arrow_cycle = "~",
    .block_full = "#", .block_med = "=", .block_light = ".", .block_dark = "#",
    .vblock = {" ", "_", "_", ".", ".", ":", ":", "#", "#"},
    .spin_dots = {"-","\\","|","/","-","\\","|","/","-","\\"},
    .spin_dots_n = 4,
    .spin_thick = {"-","\\","|","/","-","\\","|","/"},
    .spin_thick_n = 4,
    .spin_orbit = {"-","\\","|","/"},
    .spin_orbit_n = 4,
    .spin_orbit_inner = {"~","~"},
    .spin_orbit_inner_n = 2,
    .spin_pulse = {"-","\\","|","/"},
    .spin_pulse_n = 4,
    .spin_line = {"-","\\","|","/"},
    .spin_line_n = 4,
    .spin_arrow = {"<","\\","^","/",">","\\","v","/"},
    .spin_arrow_n = 8,
    .spin_star = {"*","+","*","+","*","+"},
    .spin_star_n = 6,
    .icon_think = "?", .icon_lightning = "!", .icon_gear = "*",
    .icon_timer = "@", .icon_lock = "#", .icon_money = "$",
    .icon_globe = "@", .icon_rocket = "^", .icon_fire = "~",
    .icon_link = "=", .icon_eyes = "o",
    .hline = "-", .hline_heavy = "=", .vline = "|",
    .corner_tl = "+", .corner_tr = "+", .corner_bl = "+", .corner_br = "+",
    .dot_large = "*", .dot_medium = ".", .dot_small = ".",
};

tui_glyph_tier_t tui_detect_glyph_tier(void) {
    if ((int)s_glyph_tier != -1) return s_glyph_tier;

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

    const char *locale_vars[] = { lc_all, lc_ctype, lang };
    for (int li = 0; li < 3 && !has_utf8; li++) {
        if (locale_vars[li] &&
            (strstr(locale_vars[li], "UTF-8") || strstr(locale_vars[li], "utf-8") ||
             strstr(locale_vars[li], "utf8")  || strstr(locale_vars[li], "UTF8"))) {
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
    if (!nerd_flag) nerd_flag = getenv("NERD_FONT");
    bool has_nerd = (nerd_flag && (strcmp(nerd_flag, "1") == 0 ||
                                   strcmp(nerd_flag, "true") == 0 ||
                                   strcmp(nerd_flag, "yes") == 0));

    /* These modern terminals render emoji and full Unicode well */
    if (term_prog && (
            strcmp(term_prog, "iTerm.app") == 0 ||
            strcmp(term_prog, "WezTerm") == 0 ||
            strcmp(term_prog, "Hyper") == 0 ||
            strcmp(term_prog, "vscode") == 0)) {
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
    case TUI_GLYPH_NERD:    return &GLYPHS_NERD;
    case TUI_GLYPH_FULL:    return &GLYPHS_FULL;
    case TUI_GLYPH_UNICODE: return &GLYPHS_UNICODE;
    case TUI_GLYPH_ASCII:
    default:                return &GLYPHS_ASCII;
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
    if ((int)s_color_level != -1) return s_color_level;

    if (!isatty(STDERR_FILENO)) {
        s_color_level = TUI_COLOR_NONE;
        return s_color_level;
    }

    const char *colorterm = getenv("COLORTERM");
    if (colorterm && (strcmp(colorterm, "truecolor") == 0 ||
                      strcmp(colorterm, "24bit") == 0)) {
        s_color_level = TUI_COLOR_TRUECOLOR;
        return s_color_level;
    }

    /* iTerm2, WezTerm, kitty, Alacritty all set COLORTERM but also check TERM_PROGRAM */
    const char *term_prog = getenv("TERM_PROGRAM");
    if (term_prog && (strcmp(term_prog, "iTerm.app") == 0 ||
                      strcmp(term_prog, "WezTerm") == 0 ||
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

tui_rgb_t tui_hsv_to_rgb(float h, float s, float v) {
    float c = v * s;
    float hp = fmodf(h / 60.0f, 6.0f);
    if (hp < 0) hp += 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;

    if      (hp < 1) { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5) { r1 = x; g1 = 0; b1 = c; }
    else             { r1 = c; g1 = 0; b1 = x; }

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
static int rgb_to_256(tui_rgb_t c) {
    /* Check greyscale ramp first (232-255) */
    if (c.r == c.g && c.g == c.b) {
        if (c.r < 8) return 16;
        if (c.r > 248) return 231;
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
        fprintf(stderr, "\033[38;5;%dm", rgb_to_256(c));
    }
    /* 16-color: caller should use ANSI constants instead */
}

void tui_gradient_text(const char *text, float h_start, float h_end, float s, float v) {
    if (!text || !*text) return;

    /* Count visible characters */
    int vis_count = 0;
    for (const char *p = text; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80) vis_count++;
    }
    if (vis_count <= 1) {
        tui_rgb_t c = tui_hsv_to_rgb(h_start, s, v);
        fg_color_auto(c);
        fprintf(stderr, "%s" TUI_RESET, text);
        return;
    }

    int idx = 0;
    for (const char *p = text; *p; ) {
        /* Determine byte length of this UTF-8 character */
        int clen = 1;
        unsigned char uc = (unsigned char)*p;
        if      (uc >= 0xF0) clen = 4;
        else if (uc >= 0xE0) clen = 3;
        else if (uc >= 0xC0) clen = 2;

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
    if (width <= 0) width = tui_term_width();
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
    int w = tui_term_width();
    fprintf(stderr, "  %s", TUI_DIM);
    for (int i = 0; i < w - 4; i++) fprintf(stderr, "·");
    fprintf(stderr, "%s\n", TUI_RESET);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TOOL TYPE CLASSIFICATION & COLORS
 * ══════════════════════════════════════════════════════════════════════════ */

tui_tool_type_t tui_classify_tool(const char *name) {
    if (!name) return TUI_TOOL_OTHER;
    if (strstr(name, "read") || strstr(name, "list") || strstr(name, "get") ||
        strstr(name, "search") || strstr(name, "find") || strstr(name, "cat") ||
        strstr(name, "tree") || strstr(name, "ls") || strstr(name, "stat"))
        return TUI_TOOL_READ;
    if (strstr(name, "write") || strstr(name, "create") || strstr(name, "edit") ||
        strstr(name, "patch") || strstr(name, "update") || strstr(name, "delete") ||
        strstr(name, "mkdir") || strstr(name, "mv") || strstr(name, "rm"))
        return TUI_TOOL_WRITE;
    if (strstr(name, "exec") || strstr(name, "run") || strstr(name, "shell") ||
        strstr(name, "bash") || strstr(name, "cmd") || strstr(name, "eval"))
        return TUI_TOOL_EXEC;
    if (strstr(name, "http") || strstr(name, "fetch") || strstr(name, "curl") ||
        strstr(name, "web") || strstr(name, "api") || strstr(name, "request") ||
        strstr(name, "download"))
        return TUI_TOOL_WEB;
    return TUI_TOOL_OTHER;
}

const char *tui_tool_color(tui_tool_type_t type) {
    switch (type) {
        case TUI_TOOL_READ:  return TUI_BBLUE;
        case TUI_TOOL_WRITE: return TUI_BYELLOW;
        case TUI_TOOL_EXEC:  return TUI_BMAGENTA;
        case TUI_TOOL_WEB:   return TUI_BGREEN;
        case TUI_TOOL_OTHER: return TUI_BCYAN;
    }
    return TUI_BCYAN;
}

tui_rgb_t tui_tool_rgb(tui_tool_type_t type) {
    switch (type) {
        case TUI_TOOL_READ:  return (tui_rgb_t){100, 149, 237}; /* cornflower blue */
        case TUI_TOOL_WRITE: return (tui_rgb_t){255, 193,  37}; /* gold */
        case TUI_TOOL_EXEC:  return (tui_rgb_t){178, 102, 255}; /* purple */
        case TUI_TOOL_WEB:   return (tui_rgb_t){ 80, 220, 120}; /* green */
        case TUI_TOOL_OTHER: return (tui_rgb_t){  0, 210, 230}; /* cyan */
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

    while (1) {
        pthread_mutex_lock(&s->mutex);
        bool running = s->running;
        const char *label = s->label;
        double elapsed = tui_now_sec() - s->start_time;
        bool use_rgb = s->use_rgb;
        tui_rgb_t rgb = s->rgb;
        const char *color = s->color;
        pthread_mutex_unlock(&s->mutex);

        if (!running) break;

        /* Build the entire spinner line in a buffer, then write atomically
           to avoid interleaving with stdout streaming output. */
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[2K\r  ");

        /* Spinner character with color */
        if (use_rgb) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "\033[38;2;%d;%d;%dm", rgb.r, rgb.g, rgb.b);
        } else if (color) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", color);
        }
        {
            const tui_glyphs_t *g = tui_glyph();
            int fc = g->spin_dots_n > 0 ? g->spin_dots_n : 1;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s" TUI_RESET,
                            g->spin_dots[frame % fc]);
        }

        /* Label + elapsed */
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%s%s %s(%.1fs)%s",
                TUI_BOLD, label ? label : "", TUI_RESET,
                TUI_DIM, elapsed, TUI_RESET);

        /* Single write to stderr */
        fwrite(buf, 1, pos, stderr);
        fflush(stderr);

        frame++;
        usleep(100000); /* 10fps */
    }
    return NULL;
}

void tui_async_spinner_start(tui_async_spinner_t *s, const char *label,
                             tui_tool_type_t tool_type) {
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

void tui_async_spinner_stop(tui_async_spinner_t *s, bool ok,
                            const char *result_preview, double elapsed_ms) {
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
        if (len > 80) len = 80;
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

    const tui_glyphs_t *gl = tui_glyph();
    const char *icon = ok ? gl->ok : gl->fail;
    const char *icon_color = ok ? TUI_GREEN : TUI_RED;

    fprintf(stderr, "  %s%s%s %s%s%s%s %s(%s)%s",
            icon_color, icon, TUI_RESET,
            TUI_BOLD, s->label ? s->label : "", TUI_RESET,
            size_str,
            TUI_DIM, elapsed_str, TUI_RESET);

    if (preview[0]) {
        fprintf(stderr, " %s%s%s", TUI_DIM, preview, TUI_RESET);
    }
    fprintf(stderr, "\n");
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
                const char *icon = e->ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
                const char *color = e->ok ? TUI_GREEN : TUI_RED;
                char elapsed_str[32];
                if (e->elapsed_ms < 1000.0)
                    snprintf(elapsed_str, sizeof(elapsed_str), "%.0fms", e->elapsed_ms);
                else
                    snprintf(elapsed_str, sizeof(elapsed_str), "%.1fs", e->elapsed_ms / 1000.0);

                pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "  %s%s%s %s%s%s %s(%s)%s",
                        color, icon, TUI_RESET,
                        tui_tool_color(e->type), e->name, TUI_RESET,
                        TUI_DIM, elapsed_str, TUI_RESET);
                if (e->preview[0]) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                            " %s%.60s%s", TUI_DIM, e->preview, TUI_RESET);
                }
            } else {
                double elapsed = now - bs->start_time;
                tui_rgb_t rgb = tui_tool_rgb(e->type);
                {
                    const tui_glyphs_t *gl = tui_glyph();
                    int fc = gl->spin_dots_n > 0 ? gl->spin_dots_n : 1;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "  \033[38;2;%d;%d;%dm%s" TUI_RESET " %s%s%s %s(%.1fs)%s",
                            rgb.r, rgb.g, rgb.b,
                            gl->spin_dots[frame % fc],
                            tui_tool_color(e->type), e->name, TUI_RESET,
                            TUI_DIM, elapsed, TUI_RESET);
                }
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");

            if (pos >= (int)sizeof(buf) - 256) break; /* safety */
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
    for (int i = 0; i < bs->count; i++) fprintf(stderr, "\n");

    pthread_create(&bs->thread, NULL, batch_spinner_thread, bs);
}

void tui_batch_spinner_complete(tui_batch_spinner_t *bs, int idx, bool ok,
                                const char *preview, double elapsed_ms) {
    pthread_mutex_lock(&bs->mutex);
    if (idx >= 0 && idx < bs->count) {
        bs->entries[idx].done = true;
        bs->entries[idx].ok = ok;
        bs->entries[idx].elapsed_ms = elapsed_ms;
        if (preview) {
            const char *nl = strchr(preview, '\n');
            int len = nl ? (int)(nl - preview) : (int)strlen(preview);
            if (len > 80) len = 80;
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

/* ══════════════════════════════════════════════════════════════════════════
 * ANIMATED WELCOME BANNER
 * ══════════════════════════════════════════════════════════════════════════ */

/* Overrides the original tui_welcome with animated version */
/* Note: we redefine tui_welcome at the bottom so the linker picks this up.
   Actually, we just modify the existing function above. We'll do it via
   a new internal helper called from the original. */

static void welcome_animated(const char *model, int tool_count, const char *version) {
    int w = tui_term_width();

    /* Left margin for banner content (left-aligned) */
    #define CENTER_PAD(content_w) 2

    /* ASCII art lines (plain text, no ANSI) */
    const char *art[] = {
        "  ██████╗  ███████╗  ██████╗  ██████╗ ",
        "  ██╔══██╗ ██╔════╝ ██╔════╝ ██╔═══██╗",
        "  ██║  ██║ ███████╗ ██║      ██║   ██║",
        "  ██║  ██║ ╚════██║ ██║      ██║   ██║",
        "  ██████╔╝ ███████║ ╚██████╗ ╚██████╔╝",
        "  ╚═════╝  ╚══════╝  ╚═════╝  ╚═════╝ ",
    };
    int art_lines = 6;
    /* art_width unused after left-align change */

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

    /* Pastel gradient reveal: lavender(270°) → rose(340°) → peach(20°) */
    float h_start = 270.0f, h_end = 350.0f;

    int pad = CENTER_PAD(art_width);

    for (int line = 0; line < art_lines; line++) {
        const char *row = art[line];
        int row_len = (int)strlen(row);

        /* Center padding */
        for (int i = 0; i < pad; i++) fputc(' ', stderr);

        /* Count visible (non-space) characters for gradient */
        int vis_chars = 0;
        for (const char *p = row; *p; ) {
            unsigned char uc = (unsigned char)*p;
            int clen = 1;
            if      (uc >= 0xF0) clen = 4;
            else if (uc >= 0xE0) clen = 3;
            else if (uc >= 0xC0) clen = 2;
            vis_chars++;
            p += clen;
        }

        int char_idx = 0;
        for (int ci = 0; ci < row_len; ) {
            unsigned char uc = (unsigned char)row[ci];
            int clen = 1;
            if      (uc >= 0xF0) clen = 4;
            else if (uc >= 0xE0) clen = 3;
            else if (uc >= 0xC0) clen = 2;

            if (row[ci] == ' ') {
                fputc(' ', stderr);
                ci += clen;
                char_idx++;
                continue;
            }
            float t = (float)char_idx / (float)(vis_chars > 1 ? vis_chars - 1 : 1);
            float h = h_start + t * (h_end - h_start);
            /* Softer pastel: high value, moderate saturation */
            tui_rgb_t c = tui_hsv_to_rgb(h, 0.45f, 1.0f);
            fg_color_auto(c);
            fwrite(&row[ci], 1, clen, stderr);
            fflush(stderr);
            usleep(1500); /* slightly faster reveal */
            ci += clen;
            char_idx++;
        }
        fprintf(stderr, TUI_RESET "\n");
    }

    fprintf(stderr, "\n");

    /* Info line with sparkle decorations */
    char info_plain[256];
    snprintf(info_plain, sizeof(info_plain), "v%s  %s  %d tools  streaming  swarm-ready",
             version, model, tool_count);
    int info_pad = CENTER_PAD(0);
    for (int i = 0; i < info_pad; i++) fputc(' ', stderr);
    tui_gradient_text(tui_glyph()->sparkle, 300.0f, 300.0f, 0.5f, 1.0f);
    fprintf(stderr, "  ");
    tui_gradient_text(info_plain, 270.0f, 350.0f, 0.35f, 0.9f);
    fprintf(stderr, "  ");
    tui_gradient_text(tui_glyph()->sparkle, 340.0f, 340.0f, 0.5f, 1.0f);
    fprintf(stderr, "\n\n");

    /* Capabilities in a cute centered pill layout with dot separators */
    const char *caps[] = {
        "AST introspection", "Sub-agent swarms", "Streaming I/O",
        "Crypto toolkit", "Coroutine pipelines", "Plugin system"
    };
    float cap_hues[] = { 270.0f, 290.0f, 310.0f, 330.0f, 350.0f, 280.0f };
    int ncaps = 6;

    /* Row 1: first 3 caps */
    {
        int rpad = CENTER_PAD(0);
        for (int i = 0; i < rpad; i++) fputc(' ', stderr);
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
        for (int i = 0; i < rpad; i++) fputc(' ', stderr);
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
    if (model) snprintf(sb->model, sizeof(sb->model), "%s", model);
    sb->enabled = false;
    sb->visible = false;
}

void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok,
                           double cost, int turn, int tools) {
    pthread_mutex_lock(&sb->mutex);
    sb->input_tokens = in_tok;
    sb->output_tokens = out_tok;
    sb->cost = cost;
    sb->turn = turn;
    sb->tools_used = tools;
    pthread_mutex_unlock(&sb->mutex);

    if (sb->visible) tui_status_bar_render(sb);
}

void tui_status_bar_enable(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    sb->enabled = true;
    sb->visible = true;
    pthread_mutex_unlock(&sb->mutex);

    int rows = tui_term_height();
    /* Set scroll region: rows 1 to (rows-1), pinning last line */
    fprintf(stderr, "\033[1;%dr", rows - 1);
    tui_status_bar_render(sb);
}

void tui_status_bar_disable(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    bool was_visible = sb->visible;
    sb->enabled = false;
    sb->visible = false;
    pthread_mutex_unlock(&sb->mutex);

    if (!was_visible) return;  /* Don't touch scroll region if never shown */

    int rows = tui_term_height();
    /* Reset scroll region to full terminal */
    fprintf(stderr, "\033[r");  /* reset to default */
    /* Clear the status bar line */
    tui_save_cursor();
    tui_cursor_move(rows, 1);
    tui_clear_line();
    tui_restore_cursor();
    fflush(stderr);
}

void tui_status_bar_render(tui_status_bar_t *sb) {
    pthread_mutex_lock(&sb->mutex);
    if (!sb->visible) {
        pthread_mutex_unlock(&sb->mutex);
        return;
    }

    int rows = tui_term_height();
    int cols = tui_term_width();

    char bar[512];
    char in_str[32], out_str[32];

    /* Format token counts with K suffix */
    if (sb->input_tokens >= 1000)
        snprintf(in_str, sizeof(in_str), "%.1fk", sb->input_tokens / 1000.0);
    else
        snprintf(in_str, sizeof(in_str), "%d", sb->input_tokens);

    if (sb->output_tokens >= 1000)
        snprintf(out_str, sizeof(out_str), "%.1fk", sb->output_tokens / 1000.0);
    else
        snprintf(out_str, sizeof(out_str), "%d", sb->output_tokens);

    snprintf(bar, sizeof(bar), " %s | in:%s out:%s | $%.2f | turn %d | %d tools ",
             sb->model, in_str, out_str, sb->cost, sb->turn, sb->tools_used);

    pthread_mutex_unlock(&sb->mutex);

    tui_save_cursor();
    tui_cursor_move(rows, 1);

    /* Dark background bar */
    fprintf(stderr, "\033[48;5;236m\033[38;5;252m");
    int bar_vis = (int)strlen(bar);
    fprintf(stderr, "%s", bar);
    /* Pad remaining width */
    for (int i = bar_vis; i < cols; i++) fputc(' ', stderr);

    /* F31: Status bar clock */
    if (sb->show_clock) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char clock_str[8];
        snprintf(clock_str, sizeof(clock_str), " %02d:%02d", tm->tm_hour, tm->tm_min);
        /* Move cursor back to write clock at right edge */
        int clock_len = (int)strlen(clock_str);
        fprintf(stderr, "\033[%dG%s", cols - clock_len, clock_str);
    }

    fprintf(stderr, TUI_RESET);

    tui_restore_cursor();
    fflush(stderr);
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
    if (g_tui_features && !g_tui_features->retry_pulse) return;
    const char *frames[] = {"◉◎○◎◉", "◎◉◎○◎", "○◎◉◎○", "◎○◎◉◎", "◉◎○◎◉"};
    int nframes = 5;
    double end = tui_now() + wait_sec;
    int frame = 0;
    tui_cursor_hide();
    while (tui_now() < end) {
        double remaining = end - tui_now();
        if (remaining < 0) remaining = 0;
        tui_clear_line();
        fprintf(stderr, "  %s%s%s %s%s%s retry %d/%d %.1fs",
                TUI_YELLOW, frames[frame % nframes], TUI_RESET,
                TUI_DIM, label ? label : "", TUI_RESET,
                attempt, max, remaining);
        fflush(stderr);
        usleep(150000);
        frame++;
    }
    tui_clear_line();
    tui_cursor_show();
    fflush(stderr);
}

/* ── F12: Result Sparkline ────────────────────────────────────────────── */

void tui_sparkline(const double *values, int count, const char *color) {
    if (g_tui_features && !g_tui_features->result_sparkline) return;
    if (!values || count <= 0) return;
    const char *bars = "▁▂▃▄▅▆▇█";
    /* Each bar char is 3 bytes UTF-8 */
    double mn = values[0], mx = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < mn) mn = values[i];
        if (values[i] > mx) mx = values[i];
    }
    double range = mx - mn;
    if (range < 1e-9) range = 1.0;
    if (color) fprintf(stderr, "%s", color);
    for (int i = 0; i < count; i++) {
        int idx = (int)((values[i] - mn) / range * 7.0);
        if (idx < 0) idx = 0;
        if (idx > 7) idx = 7;
        /* Write the UTF-8 bar character (3 bytes each) */
        fwrite(bars + idx * 3, 1, 3, stderr);
    }
    if (color) fprintf(stderr, "%s", TUI_RESET);
}

bool tui_try_sparkline(const char *text) {
    if (!text) return false;
    /* Try to detect comma-separated numbers */
    const char *p = text;
    double values[128];
    int count = 0;
    while (*p && count < 128) {
        while (*p == ' ' || *p == ',' || *p == '[' || *p == ']') p++;
        if (!*p) break;
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
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
    if (g_tui_features && !g_tui_features->context_gauge) return;
    if (max_tok <= 0) return;
    if (width <= 0) width = 30;
    double pct = (double)used / max_tok;
    int filled = (int)(pct * width);
    if (filled > width) filled = width;

    const char *color;
    if (pct < 0.5) color = TUI_GREEN;
    else if (pct < 0.75) color = TUI_YELLOW;
    else if (pct < 0.90) color = TUI_BYELLOW;
    else color = TUI_RED;

    fprintf(stderr, "  [");
    for (int i = 0; i < width; i++) {
        if (i < filled)
            fprintf(stderr, "%s█%s", color, TUI_RESET);
        else
            fprintf(stderr, "%s░%s", TUI_DIM, TUI_RESET);
    }
    fprintf(stderr, " %s%.0f%%%s]", color, pct * 100, TUI_RESET);
    if (used >= 1000)
        fprintf(stderr, " %s%.1fk/%.0fk%s", TUI_DIM, used / 1000.0, max_tok / 1000.0, TUI_RESET);
    else
        fprintf(stderr, " %s%d/%d%s", TUI_DIM, used, max_tok, TUI_RESET);
    fprintf(stderr, "\n");
}

/* ── F17: Auto-Compact Notification ───────────────────────────────────── */

void tui_compact_flash(int before, int after) {
    if (g_tui_features && !g_tui_features->compact_flash) return;
    fprintf(stderr, "  %s⟳ compacted %d→%d messages%s\n",
            TUI_DIM, before, after, TUI_RESET);
}

/* ── F22: Prompt Token Counter ────────────────────────────────────────── */

int tui_estimate_tokens(const char *text) {
    if (!text) return 0;
    return ((int)strlen(text) + 3) / 4;
}

void tui_prompt_token_display(int est, int remaining) {
    if (g_tui_features && !g_tui_features->prompt_tokens) return;
    const char *color = (remaining > 0 && est > remaining * 0.8) ? TUI_YELLOW : TUI_DIM;
    fprintf(stderr, "  %s~%d tokens%s", color, est, TUI_RESET);
    if (remaining > 0)
        fprintf(stderr, " %s(%d remaining)%s", TUI_DIM, remaining, TUI_RESET);
    fprintf(stderr, "\n");
}

/* ── F26: IPC Message Line ────────────────────────────────────────────── */

void tui_ipc_message_line(const char *from, const char *to,
                           const char *topic, const char *preview) {
    if (g_tui_features && !g_tui_features->ipc_message_line) return;
    fprintf(stderr, "  %s%s → %s [%s]%s",
            TUI_DIM, from ? from : "?", to ? to : "?",
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
    if (g_tui_features && !g_tui_features->agent_rollup) return;
    fprintf(stderr, "  %sagents: %s%d done%s",
            TUI_DIM, TUI_GREEN, done, TUI_RESET);
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
            if (bg > 8 || bg == 7) return TUI_THEME_LIGHT;
        }
    }
    /* Check TERM_BACKGROUND */
    const char *tb = getenv("TERM_BACKGROUND");
    if (tb && strcmp(tb, "light") == 0) return TUI_THEME_LIGHT;
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

void tui_section_divider(int turn, int tools, double cost, const char *model) {
    (void)model;
    if (g_tui_features && !g_tui_features->section_dividers) return;
    int w = tui_term_width();
    char info[256];
    snprintf(info, sizeof(info), " turn %d · %d tool%s · $%.3f ",
             turn, tools, tools == 1 ? "" : "s", cost);
    int info_len = (int)strlen(info);
    int left = (w - info_len - 2) / 2;
    int right = w - info_len - left - 2;
    if (left < 2) left = 2;
    if (right < 2) right = 2;

    fprintf(stderr, "\n%s", TUI_DIM);
    for (int i = 0; i < left; i++) fprintf(stderr, "─");
    fprintf(stderr, "%s%s%s%s", TUI_RESET, TUI_DIM, info, TUI_RESET);
    fprintf(stderr, "%s", TUI_DIM);
    for (int i = 0; i < right; i++) fprintf(stderr, "─");
    fprintf(stderr, "%s\n\n", TUI_RESET);
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
    const char *icons[] = {gl->icon_globe, gl->icon_lightning, gl->warn, gl->icon_timer, gl->icon_lock, gl->icon_money};
    const char *labels[] = {"NETWORK", "API", "VALIDATION", "TIMEOUT", "AUTH", "BUDGET"};
    const char *colors[] = {TUI_BRED, TUI_BMAGENTA, TUI_BYELLOW, TUI_BCYAN, TUI_RED, TUI_YELLOW};
    int idx = (int)type;
    if (idx < 0 || idx > 5) idx = 1;
    fprintf(stderr, "  %s %s%s%s%s %s%s%s\n",
            icons[idx], colors[idx], TUI_BOLD, labels[idx], TUI_RESET,
            msg ? msg : "", TUI_RESET, "");
}

/* ── F34: Notification Bell ───────────────────────────────────────────── */

void tui_notify(const char *title, const char *body) {
    if (g_tui_features && !g_tui_features->notify_bell) return;
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

void tui_cadence_init(tui_cadence_t *c, void *md_renderer) {
    memset(c, 0, sizeof(*c));
    c->interval = 0.016; /* 16ms */
    c->last_flush = tui_now();
    c->md_renderer = md_renderer;
}

void tui_cadence_feed(tui_cadence_t *c, const char *text) {
    if (!text) return;
    int tlen = (int)strlen(text);
    for (int i = 0; i < tlen; i++) {
        if (c->len < TUI_CADENCE_BUF_SIZE - 1)
            c->buf[c->len++] = text[i];
    }
    double now = tui_now();
    if (now - c->last_flush >= c->interval || c->len >= TUI_CADENCE_BUF_SIZE - 1) {
        tui_cadence_flush(c);
    }
}

void tui_cadence_flush(tui_cadence_t *c) {
    if (c->len == 0) return;
    c->buf[c->len] = '\0';
    if (c->md_renderer) {
        /* We need to call md_feed_str but avoid circular includes.
           The caller (agent.c) sets md_renderer to the correct pointer
           and calls md_feed_str directly. We just buffer here. */
        /* Actual feeding is done by the integration layer */
    }
    c->len = 0;
    c->last_flush = tui_now();
}

/* ── F4: Collapsible Thinking ─────────────────────────────────────────── */

void tui_thinking_init(tui_thinking_state_t *t) {
    memset(t, 0, sizeof(*t));
}

void tui_thinking_feed(tui_thinking_state_t *t, const char *text) {
    if (!text) return;
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
    if (!t->active) return;
    double elapsed = tui_now() - t->start_time;
    int est_tokens = (t->char_count + 3) / 4;
    if (t->summary[0]) {
        fprintf(stderr, "  %s[thinking] %s%s\n", TUI_DIM, t->summary, TUI_RESET);
        fprintf(stderr, "  %s~%d tokens, %.1fs%s\n", TUI_DIM, est_tokens, elapsed, TUI_RESET);
    } else {
        fprintf(stderr, "  %s[thinking: ~%d tokens, %.1fs]%s\n",
                TUI_DIM, est_tokens, elapsed, TUI_RESET);
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
    if (!text) return;
    for (const char *p = text; *p; p++) {
        w->chars++;
        if (*p == ' ' || *p == '\n' || *p == '\t') {
            /* Check if previous was non-space */
            if (p > text && *(p-1) != ' ' && *(p-1) != '\n' && *(p-1) != '\t')
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
    if (g_tui_features && !g_tui_features->live_word_count) return;
    double elapsed = tui_now() - w->start_time;
    fprintf(stderr, "  %s%d words, %d chars (%.1fs)%s\n",
            TUI_DIM, w->words, w->chars, elapsed, TUI_RESET);
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
        if (t->count < TUI_THROUGHPUT_SAMPLES) t->count++;
        t->tokens_since_last = 0;
        t->last_sample_time = now;
    }
}

void tui_throughput_render(tui_throughput_t *t) {
    if (g_tui_features && !g_tui_features->throughput_graph) return;
    if (t->count < 2) return;
    /* Compute average */
    double sum = 0;
    int start = t->head - t->count;
    if (start < 0) start = 0;
    double vals[TUI_THROUGHPUT_SAMPLES];
    int n = 0;
    for (int i = start; i < t->head && n < TUI_THROUGHPUT_SAMPLES; i++) {
        vals[n++] = t->samples[i % TUI_THROUGHPUT_SAMPLES];
        sum += vals[n-1];
    }
    double avg = n > 0 ? sum / n : 0;
    fprintf(stderr, "  %sthroughput: ", TUI_DIM);
    /* Inline sparkline without feature check (already checked) */
    {
        const char *spark_bars = "▁▂▃▄▅▆▇█";
        double mn = vals[0], mx2 = vals[0];
        for (int i = 1; i < n; i++) {
            if (vals[i] < mn) mn = vals[i];
            if (vals[i] > mx2) mx2 = vals[i];
        }
        double range = mx2 - mn;
        if (range < 1e-9) range = 1.0;
        fprintf(stderr, "%s", TUI_BCYAN);
        for (int i = 0; i < n; i++) {
            int si = (int)((vals[i] - mn) / range * 7.0);
            if (si < 0) si = 0;
            if (si > 7) si = 7;
            fwrite(spark_bars + si * 3, 1, 3, stderr);
        }
        fprintf(stderr, "%s", TUI_RESET);
    }
    fprintf(stderr, " avg %.0f tok/s%s\n", avg, TUI_RESET);
}

/* ── F8: Flame Timeline ───────────────────────────────────────────────── */

void tui_flame_init(tui_flame_t *f) {
    memset(f, 0, sizeof(*f));
}

void tui_flame_add(tui_flame_t *f, const char *name, double start_ms,
                    double end_ms, bool ok, tui_tool_type_t type) {
    if (f->count >= TUI_FLAME_MAX) return;
    tui_flame_entry_t *e = &f->entries[f->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->start_ms = start_ms;
    e->end_ms = end_ms;
    e->ok = ok;
    e->type = type;
    if (f->count == 1) f->epoch_ms = start_ms;
}

void tui_flame_render(tui_flame_t *f) {
    if (g_tui_features && !g_tui_features->flame_timeline) return;
    if (f->count < 2) return;

    /* Find total span */
    double total_end = 0;
    for (int i = 0; i < f->count; i++)
        if (f->entries[i].end_ms > total_end) total_end = f->entries[i].end_ms;
    double total_span = total_end - f->epoch_ms;
    if (total_span <= 0) total_span = 1;

    int bar_width = tui_term_width() - 24;
    if (bar_width < 20) bar_width = 20;

    fprintf(stderr, "\n  %s%sflame timeline:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < f->count; i++) {
        tui_flame_entry_t *e = &f->entries[i];
        int start_col = (int)((e->start_ms - f->epoch_ms) / total_span * bar_width);
        int end_col = (int)((e->end_ms - f->epoch_ms) / total_span * bar_width);
        if (end_col <= start_col) end_col = start_col + 1;
        if (end_col > bar_width) end_col = bar_width;

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
        if (strcmp(d->nodes[i], name) == 0) return i;
    if (d->node_count >= TUI_DAG_MAX_NODES) return -1;
    int idx = d->node_count++;
    snprintf(d->nodes[idx], sizeof(d->nodes[idx]), "%s", name);
    return idx;
}

void tui_dag_add_edge(tui_dag_t *d, int from, int to) {
    if (d->edge_count >= TUI_DAG_MAX_EDGES) return;
    /* Deduplicate */
    for (int i = 0; i < d->edge_count; i++)
        if (d->edges[i].from == from && d->edges[i].to == to) return;
    d->edges[d->edge_count].from = from;
    d->edges[d->edge_count].to = to;
    d->edge_count++;
}

void tui_dag_render(tui_dag_t *d) {
    if (g_tui_features && !g_tui_features->tool_dep_graph) return;
    if (d->node_count < 2 || d->edge_count == 0) return;

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
                if (!first) fprintf(stderr, " %s→%s ", TUI_DIM, TUI_RESET);
                fprintf(stderr, "%s%s%s", TUI_CYAN, d->nodes[cur], TUI_RESET);
                printed[cur] = true;
                first = false;
                /* Find next */
                int next = -1;
                for (int e = 0; e < d->edge_count; e++)
                    if (d->edges[e].from == cur) { next = d->edges[e].to; break; }
                cur = next;
            }
        }
    }
    /* Print any remaining unprinted nodes */
    for (int n = 0; n < d->node_count; n++) {
        if (!printed[n]) {
            if (!first) fprintf(stderr, " %s→%s ", TUI_DIM, TUI_RESET);
            fprintf(stderr, "%s%s%s", TUI_CYAN, d->nodes[n], TUI_RESET);
            first = false;
        }
    }
    fprintf(stderr, "\n");
}

/* ── F13: Tool Cost Annotations ───────────────────────────────────────── */

void tui_tool_cost(const char *name, int in_tok, int out_tok, const char *model) {
    if (g_tui_features && !g_tui_features->tool_cost) return;
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

void tui_chart(tui_chart_type_t type, const char **labels, const double *values,
               int count, int width, int height) {
    if (g_tui_features && !g_tui_features->ascii_charts) return;
    if (!values || count <= 0) return;
    (void)height; /* height used for vertical charts, not implemented yet */

    double mx = values[0];
    for (int i = 1; i < count; i++)
        if (values[i] > mx) mx = values[i];
    if (mx <= 0) mx = 1;

    int max_label = 0;
    for (int i = 0; i < count; i++) {
        int l = labels && labels[i] ? (int)strlen(labels[i]) : 0;
        if (l > max_label) max_label = l;
    }
    if (max_label > 20) max_label = 20;

    int bar_max = width - max_label - 12;
    if (bar_max < 10) bar_max = 10;

    (void)type; /* both types render horizontal for now */
    for (int i = 0; i < count; i++) {
        const char *lbl = labels && labels[i] ? labels[i] : "";
        fprintf(stderr, "  %*s │", max_label, lbl);
        int bar_len = (int)(values[i] / mx * bar_max);
        const char *color = TUI_BCYAN;
        if (values[i] == mx) color = TUI_BGREEN;
        for (int b = 0; b < bar_len; b++)
            fprintf(stderr, "%s█%s", color, TUI_RESET);
        fprintf(stderr, " %.1f\n", values[i]);
    }
}

/* ── F7: Citation Footnotes ───────────────────────────────────────────── */

void tui_citation_init(tui_citation_t *c) {
    memset(c, 0, sizeof(*c));
}

int tui_citation_add(tui_citation_t *c, const char *tool_name,
                      const char *tool_id, const char *preview, double elapsed_ms) {
    if (c->count >= TUI_CITATION_MAX) return c->count;
    tui_citation_entry_t *e = &c->entries[c->count];
    snprintf(e->tool_name, sizeof(e->tool_name), "%s", tool_name ? tool_name : "");
    snprintf(e->tool_id, sizeof(e->tool_id), "%s", tool_id ? tool_id : "");
    if (preview) {
        const char *nl = strchr(preview, '\n');
        int len = nl ? (int)(nl - preview) : (int)strlen(preview);
        if (len > 120) len = 120;
        memcpy(e->preview, preview, len);
        e->preview[len] = '\0';
    }
    e->elapsed_ms = elapsed_ms;
    e->index = c->count + 1;
    return ++c->count;
}

void tui_citation_render(tui_citation_t *c) {
    if (g_tui_features && !g_tui_features->citation_footnotes) return;
    if (c->count == 0) return;
    fprintf(stderr, "\n  %s%sfootnotes:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < c->count; i++) {
        tui_citation_entry_t *e = &c->entries[i];
        fprintf(stderr, "  %s[%d]%s %s%s%s",
                TUI_DIM, e->index, TUI_RESET,
                TUI_CYAN, e->tool_name, TUI_RESET);
        if (e->elapsed_ms > 0)
            fprintf(stderr, " %s(%.0fms)%s", TUI_DIM, e->elapsed_ms, TUI_RESET);
        if (e->preview[0])
            fprintf(stderr, " %s%s%s", TUI_DIM, e->preview, TUI_RESET);
        fprintf(stderr, "\n");
    }
}

/* ── F3: Inline Diff Rendering ────────────────────────────────────────── */

bool tui_is_diff(const char *text) {
    if (!text) return false;
    /* Check for unified diff markers */
    const char *p = text;
    bool has_minus = false, has_plus = false, has_at = false;
    while (*p) {
        if (p == text || *(p-1) == '\n') {
            if (strncmp(p, "--- ", 4) == 0) has_minus = true;
            else if (strncmp(p, "+++ ", 4) == 0) has_plus = true;
            else if (strncmp(p, "@@ ", 3) == 0) has_at = true;
        }
        p++;
    }
    return (has_minus && has_plus) || has_at;
}

void tui_render_diff(const char *text, FILE *out) {
    if (!text) return;
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

        if (!eol) break;
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
    snprintf(modified_header, sizeof(modified_header), "%s %s",
             t->headers[sort_col], ascending ? "▲" : "▼");
    tmp.headers[sort_col] = modified_header;
    tui_table_render(&tmp, width);
}

/* ── F37: JSON Tree View ──────────────────────────────────────────────── */

static void json_tree_recurse(const char **p, int depth, int max_depth,
                                bool color, bool last, const char *prefix) {
    (void)last;
    if (!p || !*p) return;
    while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++;

    if (depth > max_depth) {
        fprintf(stderr, "%s...%s", color ? TUI_DIM : "", color ? TUI_RESET : "");
        /* Skip until matching bracket */
        int nesting = 0;
        while (**p) {
            if (**p == '{' || **p == '[') nesting++;
            else if (**p == '}' || **p == ']') {
                if (nesting == 0) break;
                nesting--;
            }
            (*p)++;
        }
        return;
    }

    if (**p == '{') {
        (*p)++;
        while (**p && **p != '}') {
            while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t' || **p == ',') (*p)++;
            if (**p == '}') break;

            /* Parse key */
            if (**p == '"') {
                (*p)++;
                const char *key_start = *p;
                while (**p && **p != '"') (*p)++;
                int key_len = (int)(*p - key_start);
                if (**p == '"') (*p)++;
                while (**p == ' ' || **p == ':') (*p)++;

                /* Determine if last key */
                bool is_last_key = false;
                {
                    const char *scan = *p;
                    int nest = 0;
                    if (*scan == '"') { scan++; while (*scan && !(*scan == '"' && *(scan-1) != '\\')) scan++; if (*scan) scan++; }
                    else if (*scan == '{' || *scan == '[') { nest = 1; scan++; while (*scan && nest > 0) { if (*scan == '{' || *scan == '[') nest++; if (*scan == '}' || *scan == ']') nest--; scan++; } }
                    else { while (*scan && *scan != ',' && *scan != '}') scan++; }
                    while (*scan == ' ' || *scan == '\n' || *scan == '\r' || *scan == '\t') scan++;
                    if (*scan == ',' || *scan == '}') {
                        while (*scan == ' ' || *scan == ',' || *scan == '\n' || *scan == '\r' || *scan == '\t') scan++;
                        is_last_key = (*scan == '}');
                    }
                }

                const char *connector = is_last_key ? "└── " : "├── ";
                const char *child_prefix_add = is_last_key ? "    " : "│   ";

                fprintf(stderr, "%s%s%s%.*s%s: ",
                        prefix, connector,
                        color ? TUI_BCYAN : "", key_len, key_start,
                        color ? TUI_RESET : "");

                char child_prefix[256];
                snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, child_prefix_add);
                json_tree_recurse(p, depth + 1, max_depth, color, is_last_key, child_prefix);
                fprintf(stderr, "\n");
            } else {
                break;
            }
        }
        if (**p == '}') (*p)++;
    } else if (**p == '[') {
        (*p)++;
        fprintf(stderr, "[");
        int idx = 0;
        while (**p && **p != ']') {
            while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t' || **p == ',') (*p)++;
            if (**p == ']') break;
            if (idx > 0) fprintf(stderr, ", ");
            json_tree_recurse(p, depth + 1, max_depth, color, false, prefix);
            idx++;
        }
        fprintf(stderr, "]");
        if (**p == ']') (*p)++;
    } else if (**p == '"') {
        (*p)++;
        const char *start = *p;
        while (**p && **p != '"') {
            if (**p == '\\') (*p)++;
            (*p)++;
        }
        fprintf(stderr, "%s\"%.*s\"%s",
                color ? TUI_GREEN : "", (int)(*p - start), start, color ? TUI_RESET : "");
        if (**p == '"') (*p)++;
    } else if (**p == 't' || **p == 'f') {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n') (*p)++;
        fprintf(stderr, "%s%.*s%s",
                color ? TUI_YELLOW : "", (int)(*p - start), start, color ? TUI_RESET : "");
    } else if (**p == 'n') {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n') (*p)++;
        fprintf(stderr, "%s%.*s%s",
                color ? TUI_RED : "", (int)(*p - start), start, color ? TUI_RESET : "");
    } else if (**p == '-' || (**p >= '0' && **p <= '9')) {
        const char *start = *p;
        while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\n') (*p)++;
        fprintf(stderr, "%s%.*s%s",
                color ? TUI_BCYAN : "", (int)(*p - start), start, color ? TUI_RESET : "");
    } else {
        (*p)++;
    }
}

void tui_json_tree(const char *json, int max_depth, bool color) {
    if (g_tui_features && !g_tui_features->json_tree) return;
    if (!json) return;
    const char *p = json;
    json_tree_recurse(&p, 0, max_depth > 0 ? max_depth : 5, color, true, "  ");
    fprintf(stderr, "\n");
}

/* ── F16: Conversation Minimap ────────────────────────────────────────── */

void tui_minimap_render(const tui_minimap_entry_t *entries, int count, int height) {
    if (g_tui_features && !g_tui_features->conv_minimap) return;
    if (!entries || count == 0) return;
    if (height <= 0) height = tui_term_height() - 4;
    if (height < 4) height = 4;

    /* Map entries to rows proportionally */
    int total_tokens = 0;
    for (int i = 0; i < count; i++) total_tokens += entries[i].tokens > 0 ? entries[i].tokens : 1;
    if (total_tokens == 0) total_tokens = 1;

    fprintf(stderr, "  %s%sminimap:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    int row = 0;
    for (int i = 0; i < count && row < height; i++) {
        int tok = entries[i].tokens > 0 ? entries[i].tokens : 1;
        int rows_for_entry = (int)((double)tok / total_tokens * height + 0.5);
        if (rows_for_entry < 1) rows_for_entry = 1;

        const char *block, *color;
        switch (entries[i].type) {
            case 'u': block = "█"; color = TUI_BBLUE; break;
            case 'a': block = "░"; color = TUI_BGREEN; break;
            case 't': block = "▓"; color = TUI_BYELLOW; break;
            default:  block = "·"; color = TUI_DIM; break;
        }
        for (int r = 0; r < rows_for_entry && row < height; r++, row++) {
            fprintf(stderr, "  %s%s%s", color, block, TUI_RESET);
        }
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "  %s█%s user  %s░%s assistant  %s▓%s tool%s\n",
            TUI_BBLUE, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BYELLOW, TUI_RESET, TUI_RESET);
}

/* ── F19: Branch Indicator ────────────────────────────────────────────── */

void tui_branch_init(tui_branch_t *b) {
    memset(b, 0, sizeof(*b));
}

void tui_branch_push(tui_branch_t *b, const char *prompt) {
    if (!prompt) return;
    int idx = b->head % TUI_BRANCH_HISTORY;
    snprintf(b->prompts[idx], sizeof(b->prompts[idx]), "%.255s", prompt);
    b->head++;
    if (b->count < TUI_BRANCH_HISTORY) b->count++;
}

/* Jaccard similarity on word sets */
static double word_jaccard(const char *a, const char *b) {
    char words_a[32][32], words_b[32][32];
    int na = 0, nb = 0;
    const char *p;

    p = a;
    while (*p && na < 32) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len > 0 && len < 32) { memcpy(words_a[na], start, len); words_a[na][len] = '\0'; na++; }
    }
    p = b;
    while (*p && nb < 32) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len > 0 && len < 32) { memcpy(words_b[nb], start, len); words_b[nb][len] = '\0'; nb++; }
    }

    if (na == 0 || nb == 0) return 0;

    int intersection = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (strcasecmp(words_a[i], words_b[j]) == 0) { intersection++; break; }

    int total_union = na + nb - intersection;
    return total_union > 0 ? (double)intersection / total_union : 0;
}

bool tui_branch_detect(tui_branch_t *b, const char *prompt) {
    if (g_tui_features && !g_tui_features->branch_indicator) return false;
    if (!prompt || b->count < 2) return false;
    for (int i = 0; i < b->count; i++) {
        int idx = (b->head - 1 - i + TUI_BRANCH_HISTORY * 2) % TUI_BRANCH_HISTORY;
        double sim = word_jaccard(prompt, b->prompts[idx]);
        if (sim > 0.6) {
            fprintf(stderr, "  %s↩ branch (%.0f%% similar to recent prompt)%s\n",
                    TUI_DIM, sim * 100, TUI_RESET);
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
    if (!cmd || !cmd[0]) return;
    if (g->count > 0 && strcmp(g->history[g->count - 1], cmd) == 0) return;
    if (g->count < TUI_GHOST_MAX) {
        snprintf(g->history[g->count], sizeof(g->history[g->count]), "%.255s", cmd);
        g->count++;
    } else {
        memmove(g->history[0], g->history[1], sizeof(g->history[0]) * (TUI_GHOST_MAX - 1));
        snprintf(g->history[TUI_GHOST_MAX - 1], sizeof(g->history[0]), "%.255s", cmd);
    }
}

const char *tui_ghost_match(tui_ghost_t *g, const char *prefix) {
    if (!prefix || !prefix[0]) return NULL;
    int plen = (int)strlen(prefix);
    for (int i = g->count - 1; i >= 0; i--) {
        if (strncmp(g->history[i], prefix, plen) == 0 && g->history[i][plen] != '\0')
            return g->history[i];
    }
    return NULL;
}

/* ── F20: Multi-line Input Syntax Highlighting ────────────────────────── */

bool tui_is_code_paste(const char *text) {
    if (!text) return false;
    bool has_newline = (strchr(text, '\n') != NULL);
    if (!has_newline) return false;
    bool has_brace = (strchr(text, '{') || strchr(text, '}'));
    bool has_semi = (strchr(text, ';') != NULL);
    bool has_kw = (strstr(text, "def ") || strstr(text, "func ") ||
                   strstr(text, "function ") || strstr(text, "class ") ||
                   strstr(text, "import ") || strstr(text, "#include"));
    return has_brace || has_semi || has_kw;
}

void tui_highlight_input(const char *text, FILE *out) {
    if (!text || !out) return;
    static const char *keywords[] = {
        "if", "else", "for", "while", "return", "def", "class", "function",
        "const", "let", "var", "import", "from", "struct", "enum", "int",
        "void", "char", "bool", "true", "false", "None", "null", NULL
    };
    const char *p = text;
    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
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
            while (*p && *p != q) { if (*p == '\\') p++; p++; }
            if (*p == q) p++;
            fprintf(out, "%s%.*s%s", TUI_GREEN, (int)(p - start), start, TUI_RESET);
        } else if (*p >= '0' && *p <= '9') {
            const char *start = p;
            while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'x')) p++;
            fprintf(out, "%s%.*s%s", TUI_BYELLOW, (int)(p - start), start, TUI_RESET);
        } else if (*p == '/' && *(p+1) == '/') {
            const char *start = p;
            while (*p && *p != '\n') p++;
            fprintf(out, "%s%.*s%s", TUI_DIM, (int)(p - start), start, TUI_RESET);
        } else {
            fputc(*p, out);
            p++;
        }
    }
}

/* ── F23: Drag-Drop Preview ───────────────────────────────────────────── */

void tui_image_preview_badge(const char *path, const char *media_type,
                              long size, int w, int h) {
    if (g_tui_features && !g_tui_features->drag_drop_preview) return;
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
    if (!filter || !filter[0]) return true;
    const char *s = str, *f = filter;
    while (*s && *f) {
        if (tolower((unsigned char)*s) == tolower((unsigned char)*f)) f++;
        s++;
    }
    return *f == '\0';
}

void tui_command_palette(const tui_cmd_entry_t *cmds, int count, const char *filter) {
    if (g_tui_features && !g_tui_features->command_palette) return;
    if (!cmds || count == 0) return;

    int matches = 0;
    for (int i = 0; i < count; i++) {
        if (!filter || !filter[0] || subsequence_match(cmds[i].name, filter)) {
            if (matches == 0) {
                fprintf(stderr, "  ╭─ %s%scommands%s ─╮\n", TUI_BOLD, TUI_CYAN, TUI_RESET);
            }
            fprintf(stderr, "  │ %s%-16s%s %s%s%s\n",
                    TUI_BCYAN, cmds[i].name, TUI_RESET,
                    TUI_DIM, cmds[i].desc ? cmds[i].desc : "", TUI_RESET);
            matches++;
        }
    }
    if (matches > 0)
        fprintf(stderr, "  ╰──────────────────╯\n");
    else if (filter && filter[0])
        fprintf(stderr, "  %sno matching commands%s\n", TUI_DIM, TUI_RESET);
}

/* ── F25: Agent Topology ──────────────────────────────────────────────── */

static void topo_print_children(const tui_agent_node_t *agents, int count,
                                  int parent_id, const char *prefix, bool last) {
    (void)last;
    for (int i = 0; i < count; i++) {
        if (agents[i].parent_id == parent_id) {
            bool is_last = true;
            for (int j = i + 1; j < count; j++)
                if (agents[j].parent_id == parent_id) { is_last = false; break; }

            const char *connector = is_last ? "└─" : "├─";
            const char *child_add = is_last ? "  " : "│ ";
            const char *status_color = TUI_DIM;
            if (agents[i].status) {
                if (strcmp(agents[i].status, "running") == 0) status_color = TUI_BCYAN;
                else if (strcmp(agents[i].status, "done") == 0) status_color = TUI_GREEN;
                else if (strcmp(agents[i].status, "error") == 0) status_color = TUI_RED;
            }

            fprintf(stderr, "%s%s %s[%d]%s %s%s%s",
                    prefix, connector,
                    status_color, agents[i].id, TUI_RESET,
                    TUI_DIM, agents[i].task ? agents[i].task : "", TUI_RESET);
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
    if (g_tui_features && !g_tui_features->agent_topology) return;
    if (!agents || count == 0) return;
    fprintf(stderr, "  %s%sagent topology:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < count; i++) {
        if (agents[i].parent_id <= 0) {
            const char *sc = TUI_BCYAN;
            if (agents[i].status && strcmp(agents[i].status, "done") == 0) sc = TUI_GREEN;
            fprintf(stderr, "  %s[%d]%s %s%s%s\n",
                    sc, agents[i].id, TUI_RESET,
                    TUI_DIM, agents[i].task ? agents[i].task : "root", TUI_RESET);
            topo_print_children(agents, count, agents[i].id, "  ", true);
        }
    }
}

/* ── F28: Swarm Cost Aggregation ──────────────────────────────────────── */

void tui_swarm_cost(const tui_swarm_cost_entry_t *agents, int count, double total) {
    if (g_tui_features && !g_tui_features->swarm_cost) return;
    if (!agents || count == 0) return;
    fprintf(stderr, "  %s%sswarm cost:%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "    %s%-20s%s $%.4f %s(%d in, %d out)%s\n",
                TUI_CYAN, agents[i].name ? agents[i].name : "?", TUI_RESET,
                agents[i].cost,
                TUI_DIM, agents[i].in_tok, agents[i].out_tok, TUI_RESET);
    }
    fprintf(stderr, "    %s%s──────────────────────%s\n", TUI_BOLD, TUI_DIM, TUI_RESET);
    fprintf(stderr, "    %s%-20s%s %s$%.4f%s\n",
            TUI_BWHITE, "total", TUI_RESET,
            TUI_BOLD, total, TUI_RESET);
}

/* ── F33: Smooth Scroll ───────────────────────────────────────────────── */

void tui_scroller_init(tui_scroller_t *s, const char **lines, int count) {
    s->lines = lines;
    s->line_count = count;
    s->offset = 0;
    s->page_size = tui_term_height() - 4;
    if (s->page_size < 5) s->page_size = 5;
}

void tui_scroller_render(tui_scroller_t *s) {
    if (!s->lines) return;
    int end = s->offset + s->page_size;
    if (end > s->line_count) end = s->line_count;

    for (int i = s->offset; i < end; i++) {
        fprintf(stderr, "%s\n", s->lines[i] ? s->lines[i] : "");
    }
    fprintf(stderr, "%s── %d-%d of %d (j/k/q) ──%s\n",
            TUI_DIM, s->offset + 1, end, s->line_count, TUI_RESET);
}

bool tui_scroller_handle_key(tui_scroller_t *s, int ch) {
    switch (ch) {
        case 'j': case 'J':
            if (s->offset + s->page_size < s->line_count)
                s->offset++;
            return true;
        case 'k': case 'K':
            if (s->offset > 0)
                s->offset--;
            return true;
        case ' ':
            s->offset += s->page_size;
            if (s->offset > s->line_count - s->page_size)
                s->offset = s->line_count - s->page_size;
            if (s->offset < 0) s->offset = 0;
            return true;
        case 'q': case 'Q': case 27:
            return false;
    }
    return true;
}

/* ── F40: Latency Budget Breakdown ────────────────────────────────────── */

void tui_latency_waterfall(const tui_latency_breakdown_t *b) {
    if (g_tui_features && !g_tui_features->latency_waterfall) return;
    if (!b || b->total_ms <= 0) return;

    int bar_width = tui_term_width() - 30;
    if (bar_width < 20) bar_width = 20;

    double phases[] = {
        b->dns_ms,
        b->connect_ms - b->dns_ms,
        b->tls_ms - b->connect_ms,
        b->ttfb_ms - b->tls_ms,
        b->total_ms - b->ttfb_ms,
    };
    const char *labels[] = {"DNS", "Connect", "TLS", "TTFB", "Transfer"};
    const char *colors[] = {TUI_BCYAN, TUI_BBLUE, TUI_BMAGENTA, TUI_BYELLOW, TUI_BGREEN};

    fprintf(stderr, "  %s%slatency waterfall:%s (%.0fms total)\n",
            TUI_BOLD, TUI_DIM, TUI_RESET, b->total_ms);

    for (int i = 0; i < 5; i++) {
        if (phases[i] < 0) phases[i] = 0;
        int bar_len = (int)(phases[i] / b->total_ms * bar_width);
        if (bar_len < 0) bar_len = 0;
        fprintf(stderr, "  %-10s ", labels[i]);
        for (int c = 0; c < bar_len; c++)
            fprintf(stderr, "%s█%s", colors[i], TUI_RESET);
        fprintf(stderr, " %s%.0fms%s\n", TUI_DIM, phases[i], TUI_RESET);
    }
}

/* ── F18: Session Diff on Load ────────────────────────────────────────── */

void tui_session_diff(int msg_count, int tool_calls, int est_tokens,
                       const char *model) {
    if (g_tui_features && !g_tui_features->session_diff) return;
    char body[512];
    snprintf(body, sizeof(body),
             "%d messages · %d tool calls · ~%d tokens\nmodel: %s",
             msg_count, tool_calls, est_tokens, model ? model : "unknown");
    tui_box("session loaded", body, BOX_ROUND, TUI_CYAN, 0);
}

/* ── F1: Token Heatmap ────────────────────────────────────────────────── */

void tui_heatmap_word(const char *word, int len, FILE *out) {
    if (g_tui_features && !g_tui_features->token_heatmap) {
        fwrite(word, 1, len, out);
        return;
    }
    float hue;
    if (len <= 3) hue = 120.0f;
    else if (len <= 5) hue = 80.0f;
    else if (len <= 7) hue = 50.0f;
    else hue = 30.0f;

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
    if (!f) return;
    const bool *flags = (const bool *)f;
    const tui_glyphs_t *gl = tui_glyph();
    fprintf(stderr, "  %s%sUI Features:%s\n", TUI_BOLD, TUI_BCYAN, TUI_RESET);
    for (int i = 0; i < TUI_FEATURE_COUNT; i++) {
        fprintf(stderr, "    %s[%s]%s F%-2d %s\n",
                flags[i] ? TUI_GREEN : TUI_RED,
                flags[i] ? gl->ok : gl->fail,
                TUI_RESET,
                i + 1, tui_feature_name(i));
    }
}

bool tui_features_toggle(tui_features_t *f, const char *name) {
    if (!f || !name) return false;
    bool *flags = (bool *)f;

    for (int i = 0; i < TUI_FEATURE_COUNT; i++) {
        if (strcasecmp(tui_feature_name(i), name) == 0) {
            flags[i] = !flags[i];
            {
                const tui_glyphs_t *gl = tui_glyph();
                fprintf(stderr, "  %s%s%s %s %s %s%s%s\n",
                        flags[i] ? TUI_GREEN : TUI_RED,
                        flags[i] ? gl->ok : gl->fail, TUI_RESET,
                        tui_feature_name(i), gl->arrow_right,
                        flags[i] ? TUI_GREEN : TUI_RED,
                        flags[i] ? "on" : "off", TUI_RESET);
            }
            return true;
        }
    }

    int num = 0;
    if (name[0] == 'F' || name[0] == 'f') num = atoi(name + 1);
    else num = atoi(name);
    if (num >= 1 && num <= TUI_FEATURE_COUNT) {
        int idx = num - 1;
        flags[idx] = !flags[idx];
        {
            const tui_glyphs_t *gl = tui_glyph();
            fprintf(stderr, "  %s%s%s F%d %s %s %s%s%s\n",
                    flags[idx] ? TUI_GREEN : TUI_RED,
                    flags[idx] ? gl->ok : gl->fail, TUI_RESET,
                    num, tui_feature_name(idx), gl->arrow_right,
                    flags[idx] ? TUI_GREEN : TUI_RED,
                    flags[idx] ? "on" : "off", TUI_RESET);
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
    case TUI_NOTIF_DEBUG:    return "\xE2\x80\xA2";  /* • */
    case TUI_NOTIF_INFO:     return "\xE2\x84\xB9";  /* ℹ */
    case TUI_NOTIF_SUCCESS:  return "\xE2\x9C\x93";  /* ✓ */
    case TUI_NOTIF_WARNING:  return "\xE2\x9A\xA0";  /* ⚠ */
    case TUI_NOTIF_ERROR:    return "\xE2\x9C\x97";  /* ✗ */
    case TUI_NOTIF_CRITICAL: return "\xE2\x9C\x98";  /* ✘ */
    default: return " ";
    }
}

static const char *notif_level_color(tui_notif_level_t level) {
    switch (level) {
    case TUI_NOTIF_DEBUG:    return TUI_DIM;
    case TUI_NOTIF_INFO:     return TUI_CYAN;
    case TUI_NOTIF_SUCCESS:  return TUI_GREEN;
    case TUI_NOTIF_WARNING:  return TUI_YELLOW;
    case TUI_NOTIF_ERROR:    return TUI_RED;
    case TUI_NOTIF_CRITICAL: return TUI_BRED;
    default: return TUI_RESET;
    }
}

static double notif_default_ttl(tui_notif_level_t level) {
    switch (level) {
    case TUI_NOTIF_DEBUG:    return 5.0;
    case TUI_NOTIF_INFO:     return 15.0;
    case TUI_NOTIF_SUCCESS:  return 10.0;
    case TUI_NOTIF_WARNING:  return 30.0;
    case TUI_NOTIF_ERROR:    return 0.0;  /* persist */
    case TUI_NOTIF_CRITICAL: return 0.0;  /* persist */
    default: return 10.0;
    }
}

int tui_notif_push(tui_notif_queue_t *q, tui_notif_level_t level,
                    const char *tag, const char *fmt, ...) {
    pthread_mutex_lock(&q->mutex);

    /* Coalesce: if same tag+level+msg already exists, increment count */
    char msg[TUI_NOTIF_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (int i = 0; i < q->count; i++) {
        if (!q->queue[i].dismissed && q->queue[i].level == level &&
            tag && q->queue[i].tag[0] && strcmp(q->queue[i].tag, tag) == 0 &&
            strcmp(q->queue[i].msg, msg) == 0) {
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
            if (q->queue[i].dismissed && !q->queue[victim].dismissed) { victim = i; break; }
            if (q->queue[i].created_at < q->queue[victim].created_at) victim = i;
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
    if (tag) strncpy(n->tag, tag, TUI_NOTIF_TAG_MAX - 1);
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
        bool expired = (q->queue[i].ttl_sec > 0 &&
                       (now - q->queue[i].created_at) > q->queue[i].ttl_sec);
        if (!q->queue[i].dismissed && !expired) {
            if (dst != i) q->queue[dst] = q->queue[i];
            dst++;
        }
    }
    q->count = dst;
    pthread_mutex_unlock(&q->mutex);
}

void tui_notif_render(tui_notif_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) { pthread_mutex_unlock(&q->mutex); return; }

    double now = tui_now();
    bool any = false;
    for (int i = 0; i < q->count; i++) {
        tui_notif_t *n = &q->queue[i];
        if (n->dismissed) continue;
        if (n->ttl_sec > 0 && (now - n->created_at) > n->ttl_sec) continue;

        if (!any) {
            fprintf(stderr, "  %s\xe2\x94\x80\xe2\x94\x80 notifications %s\n",
                    TUI_DIM, TUI_RESET);
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

void tui_toast_show(tui_toast_t *t, tui_notif_level_t level,
                     double duration_sec, const char *fmt, ...) {
    pthread_mutex_lock(&t->mutex);

    /* Find empty slot or overwrite oldest */
    int slot = -1;
    double oldest = 1e18;
    int oldest_idx = 0;
    for (int i = 0; i < TUI_TOAST_MAX; i++) {
        if (!t->toasts[i].active) { slot = i; break; }
        if (t->toasts[i].expire_at < oldest) {
            oldest = t->toasts[i].expire_at;
            oldest_idx = i;
        }
    }
    if (slot < 0) slot = oldest_idx;

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

    if (slot >= t->count) t->count = slot + 1;

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
    if (name) strncpy(fsm->name, name, TUI_FSM_NAME_MAX - 1);
    fsm->ctx = ctx;
    fsm->current = -1;
}

int tui_fsm_add_state(tui_fsm_t *fsm, const char *name,
                       tui_fsm_action_fn on_enter, tui_fsm_action_fn on_exit,
                       tui_fsm_action_fn on_tick) {
    if (fsm->state_count >= TUI_FSM_MAX_STATES) return -1;
    int idx = fsm->state_count++;
    tui_fsm_state_t *s = &fsm->states[idx];
    if (name) strncpy(s->name, name, TUI_FSM_NAME_MAX - 1);
    s->on_enter = on_enter;
    s->on_exit = on_exit;
    s->on_tick = on_tick;

    /* First state added becomes initial state */
    if (fsm->current < 0) {
        fsm->current = idx;
        fsm->state_entered_at = tui_now();
        if (s->on_enter) s->on_enter(fsm->ctx);
    }
    return idx;
}

void tui_fsm_add_transition(tui_fsm_t *fsm, int from, int to, int event,
                             tui_fsm_guard_fn guard, tui_fsm_action_fn action) {
    if (fsm->trans_count >= TUI_FSM_MAX_TRANS) return;
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
            if (t->guard && !t->guard(fsm->ctx)) continue;

            /* Exit current state */
            if (fsm->states[fsm->current].on_exit)
                fsm->states[fsm->current].on_exit(fsm->ctx);

            /* Record history */
            if (fsm->history_len < TUI_FSM_HISTORY_SIZE)
                fsm->history[fsm->history_len++] = fsm->current;

            /* Transition action */
            if (t->action) t->action(fsm->ctx);

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
    fprintf(stderr, "  %sFSM '%s': state=%s (%.1fs)%s\n",
            TUI_DIM, fsm->name, tui_fsm_current_name(fsm),
            tui_fsm_time_in_state(fsm), TUI_RESET);
    if (fsm->history_len > 0) {
        fprintf(stderr, "  %shistory: ", TUI_DIM);
        for (int i = 0; i < fsm->history_len; i++) {
            if (i > 0) fprintf(stderr, " \xe2\x86\x92 ");  /* → */
            fprintf(stderr, "%s", fsm->states[fsm->history[i]].name);
        }
        fprintf(stderr, " \xe2\x86\x92 %s%s\n",
                tui_fsm_current_name(fsm), TUI_RESET);
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
    if (slot_id < 0 || slot_id >= rc->slot_count) return;
    pthread_mutex_lock(&rc->mutex);
    tui_render_slot_t *s = &rc->slots[slot_id];
    if (strcmp(s->content, content) != 0) {
        strncpy(s->content, content, TUI_RENDER_CONTENT_MAX - 1);
        s->dirty = true;
    }
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_slot_free(tui_render_ctx_t *rc, int slot_id) {
    if (slot_id < 0 || slot_id >= rc->slot_count) return;
    pthread_mutex_lock(&rc->mutex);
    rc->slots[slot_id].type = TUI_SLOT_EMPTY;
    rc->slots[slot_id].dirty = false;
    rc->layout_dirty = true;
    pthread_mutex_unlock(&rc->mutex);
}

void tui_render_slot_dirty(tui_render_ctx_t *rc, int slot_id) {
    if (slot_id < 0 || slot_id >= rc->slot_count) return;
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
        if (s->type == TUI_SLOT_EMPTY || !s->dirty) continue;
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
    if (title) strncpy(mp->title, title, TUI_PROGRESS_NAME_MAX - 1);
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
    if (name) strncpy(p->name, name, TUI_PROGRESS_NAME_MAX - 1);
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
            if (mp->ema_rate <= 0) mp->ema_rate = rate;
            else mp->ema_rate = mp->ema_alpha * rate + (1 - mp->ema_alpha) * mp->ema_rate;
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
    double total = tui_multi_progress_total(mp);
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
    if (mp->phase_count == 0) { pthread_mutex_unlock(&mp->mutex); return; }

    int width = tui_term_width() - 8;
    if (width < 20) width = 20;
    if (width > 80) width = 80;

    double total_pct = 0;
    double total_weight = 0;
    for (int i = 0; i < mp->phase_count; i++) total_weight += mp->phases[i].weight;

    fprintf(stderr, "  %s%s%s\n", TUI_BOLD, mp->title, TUI_RESET);

    for (int i = 0; i < mp->phase_count; i++) {
        tui_progress_phase_t *p = &mp->phases[i];
        const char *icon = p->complete ? "\xe2\x9c\x93" :
                          p->active   ? "\xe2\x97\x89" : "\xe2\x97\x8b"; /* ✓ ◉ ○ */
        const char *color = p->complete ? TUI_GREEN :
                           p->active   ? TUI_CYAN : TUI_DIM;

        int bar_w = (int)(width * p->weight / total_weight);
        if (bar_w < 5) bar_w = 5;
        int filled = (int)(bar_w * p->progress);

        fprintf(stderr, "  %s%s%s %s%-12s%s ", color, icon, TUI_RESET,
                color, p->name, TUI_RESET);

        /* Bar */
        fprintf(stderr, "%s", color);
        for (int j = 0; j < bar_w; j++) {
            fprintf(stderr, "%s", j < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); /* █ ░ */
        }
        fprintf(stderr, "%s", TUI_RESET);

        fprintf(stderr, " %s%.0f%%%s", TUI_DIM, p->progress * 100, TUI_RESET);

        if (p->complete && p->end_time > p->start_time) {
            fprintf(stderr, " %s(%.1fs)%s", TUI_DIM,
                    p->end_time - p->start_time, TUI_RESET);
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

int tui_event_subscribe(tui_event_bus_t *bus, tui_event_type_t type,
                         tui_event_handler_fn handler, void *ctx) {
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
    if (bus->history_count < 64) bus->history_count++;

    /* Dispatch to subscribers */
    for (int i = 0; i < bus->sub_count; i++) {
        if (bus->subs[i].active && bus->subs[i].type == event->type) {
            bus->subs[i].handler(event, bus->subs[i].ctx);
        }
    }
    pthread_mutex_unlock(&bus->mutex);
}

void tui_event_emit_simple(tui_event_bus_t *bus, tui_event_type_t type,
                            const char *source) {
    tui_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.source = source;
    e.timestamp = tui_now();
    tui_event_emit(bus, &e);
}

void tui_event_bus_dump(const tui_event_bus_t *bus, int max_events) {
    static const char *evt_names[] = {
        "stream_start", "stream_text", "stream_end",
        "thinking_start", "thinking_end",
        "tool_start", "tool_complete", "tool_error",
        "turn_start", "turn_end",
        "progress", "context_pressure", "cost_update",
        "session_load", "session_save", "compact",
        "retry", "error", "custom"
    };

    fprintf(stderr, "  %s\xe2\x94\x80\xe2\x94\x80 event bus "
            "(%d subs, %d events) \xe2\x94\x80\xe2\x94\x80%s\n",
            TUI_DIM, bus->sub_count, bus->history_count, TUI_RESET);

    int start = bus->history_count < 64 ? 0 : bus->history_head;
    int total = bus->history_count < max_events ? bus->history_count : max_events;

    for (int i = 0; i < total; i++) {
        int idx = (start + bus->history_count - total + i) % 64;
        const tui_event_t *e = &bus->history[idx];
        const char *ename = (e->type < TUI_EVT__COUNT) ? evt_names[e->type] : "?";
        fprintf(stderr, "  %s%.3f%s %s%-18s%s %s%s%s\n",
                TUI_DIM, e->timestamp - bus->history[start].timestamp, TUI_RESET,
                TUI_CYAN, ename, TUI_RESET,
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

void tui_stream_state_transition(tui_stream_state_t *ss,
                                  tui_stream_phase_t new_phase) {
    ss->phase = new_phase;
    ss->phase_start = tui_now();
}

void tui_stream_state_token(tui_stream_state_t *ss, int count) {
    ss->text_tokens += count;
    ss->tokens_this_second += count;

    double now = tui_now();
    if (now - ss->last_second >= 1.0) {
        double tps = ss->tokens_this_second / (now - ss->last_second);
        if (tps > ss->peak_tok_per_sec) ss->peak_tok_per_sec = tps;
        ss->avg_tok_per_sec = (ss->avg_tok_per_sec * ss->sample_count + tps) /
                              (ss->sample_count + 1);
        ss->sample_count++;
        ss->tokens_this_second = 0;
        ss->last_second = now;
    }
}

const char *tui_stream_phase_name(tui_stream_phase_t phase) {
    switch (phase) {
    case TUI_STREAM_IDLE:          return "idle";
    case TUI_STREAM_THINKING:      return "thinking";
    case TUI_STREAM_TEXT:          return "text";
    case TUI_STREAM_TOOL_PENDING:  return "tool_pending";
    case TUI_STREAM_TOOL_RUNNING:  return "tool_running";
    case TUI_STREAM_TOOL_COMPLETE: return "tool_complete";
    case TUI_STREAM_DONE:          return "done";
    case TUI_STREAM_ERROR:         return "error";
    default: return "?";
    }
}

tui_stream_phase_t tui_stream_state_phase(const tui_stream_state_t *ss) {
    return ss->phase;
}

void tui_stream_state_render_badge(const tui_stream_state_t *ss) {
    const char *name = tui_stream_phase_name(ss->phase);
    const char *color = TUI_DIM;
    switch (ss->phase) {
    case TUI_STREAM_THINKING:     color = TUI_MAGENTA; break;
    case TUI_STREAM_TEXT:         color = TUI_GREEN;   break;
    case TUI_STREAM_TOOL_PENDING: color = TUI_YELLOW;  break;
    case TUI_STREAM_TOOL_RUNNING: color = TUI_CYAN;    break;
    case TUI_STREAM_ERROR:        color = TUI_RED;     break;
    default: break;
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
        fprintf(stderr, " %speak:%.0f tok/s%s", TUI_DIM,
                ss->peak_tok_per_sec, TUI_RESET);
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
    if (!phase || !*phase) return g->diamond;
    if (strstr(phase, "think"))    return g->icon_think;
    if (strstr(phase, "fallback")) return g->arrow_cycle;
    if (strstr(phase, "receiv"))   return g->icon_lightning;
    if (strstr(phase, "tool"))     return g->icon_gear;
    return g->diamond;
}

/* ── Breathing wave — 8 block elements that pulse in a sine wave ──── */
static void hb_render_wave(char *buf, int *pos, int bufsz,
                           double t, bool use_rgb) {
    const tui_glyphs_t *g = tui_glyph();
    int wave_width = 12;

    for (int i = 0; i < wave_width; i++) {
        /* Sine wave with phase offset per column */
        double phase = t * 3.0 + (double)i * 0.55;
        double val = (sin(phase) + 1.0) * 0.5; /* 0..1 */
        int block_idx = (int)(val * 8.0);
        if (block_idx < 0) block_idx = 0;
        if (block_idx > 8) block_idx = 8;

        if (use_rgb) {
            /* Gradient hue: shift through purple→cyan→pink over time */
            float hue = fmodf(270.0f + (float)i * 10.0f + (float)t * 25.0f, 360.0f);
            float sat = 0.50f + (float)val * 0.30f;
            float bri = 0.40f + (float)val * 0.55f;
            tui_rgb_t c = tui_hsv_to_rgb(hue, sat, bri);
            *pos += snprintf(buf + *pos, bufsz - *pos,
                             "\033[38;2;%d;%d;%dm%s",
                             c.r, c.g, c.b, g->vblock[block_idx]);
        } else {
            /* 256-color fallback — cycle through purples */
            int color = 129 + (i % 6);
            *pos += snprintf(buf + *pos, bufsz - *pos,
                             "\033[38;5;%dm%s", color, g->vblock[block_idx]);
        }
    }
    *pos += snprintf(buf + *pos, bufsz - *pos, TUI_RESET);
}

/* ── Trailing dots that fade ────────────────────────────────────────── */
static void hb_render_trail(char *buf, int *pos, int bufsz,
                            int frame, bool use_rgb) {
    const tui_glyphs_t *g = tui_glyph();
    const char *dot_strs[] = { g->dot_large, g->dot_medium, g->dot_small };
    int n = 3;
    for (int i = 0; i < n; i++) {
        (void)frame;
        float brightness = 1.0f - (float)i * 0.3f;
        if (brightness < 0.2f) brightness = 0.2f;
        if (use_rgb) {
            float hue = 280.0f + (float)i * 20.0f;
            tui_rgb_t c = tui_hsv_to_rgb(hue, 0.4f, brightness);
            *pos += snprintf(buf + *pos, bufsz - *pos,
                             "\033[38;2;%d;%d;%dm%s",
                             c.r, c.g, c.b, dot_strs[i]);
        } else {
            int gray = 240 + (int)(brightness * 15.0f);
            if (gray > 255) gray = 255;
            *pos += snprintf(buf + *pos, bufsz - *pos,
                             "\033[38;5;%dm%s", gray, dot_strs[i]);
        }
    }
    *pos += snprintf(buf + *pos, bufsz - *pos, TUI_RESET);
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
    bool use_rgb = tui_detect_color_level() >= TUI_COLOR_256;
    bool truecolor = tui_supports_truecolor();

    while (1) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 80000000 }; /* 80ms — smooth */
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&hb->mutex);
        bool running      = hb->running;
        double now         = tui_now_sec();
        double silence     = now - hb->last_poke;
        double elapsed     = now - hb->start_time;
        double thresh      = hb->silence_thresh;
        unsigned long bytes = hb->bytes_recv;
        const char *phase  = hb->phase_label;
        bool was_visible   = hb->visible;
        pthread_mutex_unlock(&hb->mutex);

        if (!running) {
            if (was_visible) {
                fprintf(stderr, "\033[2K\r");
                fflush(stderr);
            }
            break;
        }

        if (silence >= thresh) {
            char buf[1024];
            int pos = 0;

            /* Clear line and indent */
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[2K\r  ");

            /* ── Spinner glyph — orbital or braille depending on color level ── */
            {
                const tui_glyphs_t *g = tui_glyph();
                if (truecolor) {
                    float spin_hue = fmodf(270.0f + (float)frame * 8.0f, 360.0f);
                    tui_rgb_t sc = tui_hsv_to_rgb(spin_hue, 0.6f, 0.95f);
                    int oc = g->spin_orbit_n > 0 ? g->spin_orbit_n : 1;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "\033[38;2;%d;%d;%dm%s" TUI_RESET " ",
                        sc.r, sc.g, sc.b,
                        g->spin_orbit[frame % oc]);
                } else if (use_rgb) {
                    int color = 129 + (frame % 6);
                    int dc = g->spin_dots_n > 0 ? g->spin_dots_n : 1;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "\033[38;5;%dm%s" TUI_RESET " ",
                        color, g->spin_dots[frame % dc]);
                } else {
                    int dc = g->spin_dots_n > 0 ? g->spin_dots_n : 1;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        TUI_MAGENTA "%s" TUI_RESET " ",
                        g->spin_dots[frame % dc]);
                }
            }

            /* ── Phase icon + label ───────────────────────────────────── */
            const char *icon = hb_phase_icon(phase);
            const char *label;
            if (phase && *phase)
                label = phase;
            else if (bytes > 0)
                label = "streaming";
            else
                label = "connecting";

            if (truecolor) {
                /* Subtle gradient on the label text */
                float lh = fmodf(290.0f + (float)frame * 2.0f, 360.0f);
                tui_rgb_t lc = tui_hsv_to_rgb(lh, 0.25f, 0.75f);
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s \033[38;2;%d;%d;%dm%s" TUI_RESET,
                    icon, lc.r, lc.g, lc.b, label);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s " TUI_DIM "%s" TUI_RESET, icon, label);
            }

            /* ── Trailing dots ────────────────────────────────────────── */
            pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
            hb_render_trail(buf, &pos, sizeof(buf), frame, truecolor);

            /* ── Separator ────────────────────────────────────────────── */
            pos += snprintf(buf + pos, sizeof(buf) - pos, "  ");

            /* ── Breathing wave ───────────────────────────────────────── */
            if (use_rgb && silence >= thresh + 1.0) {
                /* Only show wave after an extra second of silence */
                hb_render_wave(buf, &pos, sizeof(buf), elapsed, truecolor);
                pos += snprintf(buf + pos, sizeof(buf) - pos, "  ");
            }

            /* ── Stats: bytes + elapsed ───────────────────────────────── */
            if (truecolor) {
                tui_rgb_t dim_c = tui_hsv_to_rgb(260.0f, 0.15f, 0.55f);
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "\033[38;2;%d;%d;%dm", dim_c.r, dim_c.g, dim_c.b);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos, TUI_DIM);
            }

            if (bytes > 0) {
                char bytes_str[32];
                hb_format_bytes(bytes_str, sizeof(bytes_str), bytes);
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s ", bytes_str);
            }

            char elapsed_str[32];
            hb_format_elapsed(elapsed_str, sizeof(elapsed_str), elapsed);
            {
                const tui_glyphs_t *g = tui_glyph();
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%s %s" TUI_RESET, g->icon_timer, elapsed_str);

                /* ── Inner orbit glyph (truecolor only, gentle accent) ──── */
                if (truecolor && silence >= thresh + 2.0) {
                    float ih = fmodf(320.0f + (float)frame * 12.0f, 360.0f);
                    tui_rgb_t ic = tui_hsv_to_rgb(ih, 0.4f, 0.8f);
                    int oinc = g->spin_orbit_inner_n > 0 ? g->spin_orbit_inner_n : 1;
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        " \033[38;2;%d;%d;%dm%s" TUI_RESET,
                        ic.r, ic.g, ic.b,
                        g->spin_orbit_inner[frame % oinc]);
                }
            }

            /* Write atomically */
            fwrite(buf, 1, pos, stderr);
            fflush(stderr);

            pthread_mutex_lock(&hb->mutex);
            hb->visible = true;
            pthread_mutex_unlock(&hb->mutex);

            frame++;
        } else if (was_visible) {
            /* Output resumed — clear indicator */
            fprintf(stderr, "\033[2K\r");
            fflush(stderr);

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
    if (phase != hb->phase_label) {
        hb->phase_changes++;
        hb->phase_label = phase;
    }
    hb->poke_count++;
    pthread_mutex_unlock(&hb->mutex);
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

    if (hb->visible) {
        fprintf(stderr, "\033[2K\r");
        fflush(stderr);
        hb->visible = false;
    }
}
