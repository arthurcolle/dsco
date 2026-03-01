#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

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

static const char *SPINNER_FRAMES[][12] = {
    [SPINNER_DOTS]    = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏",NULL},
    [SPINNER_BRAILLE] = {"⣾","⣽","⣻","⢿","⡿","⣟","⣯","⣷",NULL},
    [SPINNER_LINE]    = {"-","\\","|","/",NULL},
    [SPINNER_ARROW]   = {"←","↖","↑","↗","→","↘","↓","↙",NULL},
    [SPINNER_STAR]    = {"✶","✸","✹","✺","✹","✷",NULL},
    [SPINNER_PULSE]   = {"◐","◓","◑","◒",NULL},
};

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
    const char **frames = SPINNER_FRAMES[s->type];
    int count = 0;
    while (frames[count]) count++;

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
    fprintf(stderr, "  %s✓%s %s%s%s\n",
            TUI_GREEN, TUI_RESET,
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
            color ? color : TUI_BWHITE, TUI_BOLD, "●", text, TUI_RESET);
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

/* ── Welcome banner ───────────────────────────────────────────────────── */

void tui_welcome(const char *model, int tool_count, const char *version) {
    int w = tui_term_width();
    if (w > 100) w = 100;

    const char *logo =
        "     %s██████%s  %s███████%s  %s██████%s  %s██████%s \n"
        "     %s██   ██%s %s██%s      %s██%s      %s██    ██%s\n"
        "     %s██   ██%s %s███████%s %s██%s      %s██    ██%s\n"
        "     %s██   ██%s      %s██%s %s██%s      %s██    ██%s\n"
        "     %s██████%s  %s███████%s  %s██████%s  %s██████%s \n";

    fprintf(stderr, "\n");
    fprintf(stderr, logo,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BGREEN, TUI_RESET,
            TUI_BCYAN, TUI_RESET, TUI_BBLUE, TUI_RESET, TUI_BMAGENTA, TUI_RESET, TUI_BGREEN, TUI_RESET);
    fprintf(stderr, "\n");

    /* Info line */
    char info[512];
    snprintf(info, sizeof(info),
             "%sv%s%s  %s•%s  %s%s%s  %s•%s  %s%d tools%s  %s•%s  %sstreaming%s  %s•%s  %sswarm-ready%s",
             TUI_BOLD, version, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_CYAN, model, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_GREEN, tool_count, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_BMAGENTA, TUI_RESET,
             TUI_DIM, TUI_RESET,
             TUI_BYELLOW, TUI_RESET);
    fprintf(stderr, "     %s\n\n", info);

    /* Capabilities — two rows */
    fprintf(stderr, "     %s%s┃%s %sAST introspection%s  %s%s┃%s %sSub-agent swarms%s  %s%s┃%s %sStreaming I/O%s\n",
            TUI_BCYAN, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_BBLUE, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_BMAGENTA, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr, "     %s%s┃%s %sCrypto toolkit%s     %s%s┃%s %sCoroutine pipelines%s %s%s┃%s %sPlugin system%s\n",
            TUI_BRED, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_BGREEN, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_BYELLOW, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET);
    fprintf(stderr, "\n");

    tui_divider(BOX_ROUND, TUI_DIM, w);
    fprintf(stderr, "\n");
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
    const char *icon = ok ? "✓" : "✗";
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

        const char *status_color = TUI_DIM;
        const char *status_icon = "○";

        if (strcmp(e->status, "running") == 0) {
            status_color = TUI_BCYAN;
            status_icon = "◉";
        } else if (strcmp(e->status, "done") == 0) {
            status_color = TUI_GREEN;
            status_icon = "✓";
        } else if (strcmp(e->status, "error") == 0) {
            status_color = TUI_RED;
            status_icon = "✗";
        }

        fprintf(stderr, "  %s%s%s %s#%d%s %s%s%s",
                status_color, status_icon, TUI_RESET,
                TUI_DIM, e->id, TUI_RESET,
                TUI_BOLD, e->task, TUI_RESET);

        if (e->progress > 0 && e->progress < 1.0) {
            int bar_w = 20;
            int filled = (int)(e->progress * bar_w);
            fprintf(stderr, " %s", TUI_GREEN);
            for (int j = 0; j < filled; j++) fprintf(stderr, "▮");
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
