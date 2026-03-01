#ifndef DSCO_TUI_H
#define DSCO_TUI_H

#include <stdbool.h>
#include <stddef.h>

/* ── ANSI Colors & Styles ─────────────────────────────────────────────── */
#define TUI_RESET     "\033[0m"
#define TUI_BOLD      "\033[1m"
#define TUI_DIM       "\033[2m"
#define TUI_ITALIC    "\033[3m"
#define TUI_UNDERLINE "\033[4m"
#define TUI_BLINK     "\033[5m"
#define TUI_REVERSE   "\033[7m"
#define TUI_STRIKE    "\033[9m"

/* Foreground */
#define TUI_BLACK     "\033[30m"
#define TUI_RED       "\033[31m"
#define TUI_GREEN     "\033[32m"
#define TUI_YELLOW    "\033[33m"
#define TUI_BLUE      "\033[34m"
#define TUI_MAGENTA   "\033[35m"
#define TUI_CYAN      "\033[36m"
#define TUI_WHITE     "\033[37m"

/* Bright foreground */
#define TUI_BRED      "\033[91m"
#define TUI_BGREEN    "\033[92m"
#define TUI_BYELLOW   "\033[93m"
#define TUI_BBLUE     "\033[94m"
#define TUI_BMAGENTA  "\033[95m"
#define TUI_BCYAN     "\033[96m"
#define TUI_BWHITE    "\033[97m"

/* Background */
#define TUI_BG_BLACK    "\033[40m"
#define TUI_BG_RED      "\033[41m"
#define TUI_BG_GREEN    "\033[42m"
#define TUI_BG_YELLOW   "\033[43m"
#define TUI_BG_BLUE     "\033[44m"
#define TUI_BG_MAGENTA  "\033[45m"
#define TUI_BG_CYAN     "\033[46m"
#define TUI_BG_WHITE    "\033[47m"

/* 256 color */
#define TUI_FG256(n)  "\033[38;5;" #n "m"
#define TUI_BG256(n)  "\033[48;5;" #n "m"

/* ── Box styles ───────────────────────────────────────────────────────── */
typedef enum {
    BOX_ROUND,      /* ╭─╮│╰─╯ */
    BOX_SINGLE,     /* ┌─┐│└─┘ */
    BOX_DOUBLE,     /* ╔═╗║╚═╝ */
    BOX_HEAVY,      /* ┏━┓┃┗━┛ */
    BOX_ASCII,      /* +-+|+-+ */
} tui_box_style_t;

typedef struct {
    const char *tl, *tr, *bl, *br;
    const char *h, *v;
    const char *lj, *rj;  /* left/right junction for dividers */
} tui_box_chars_t;

const tui_box_chars_t *tui_box_chars(tui_box_style_t style);

/* ── Terminal utilities ───────────────────────────────────────────────── */
int  tui_term_width(void);
int  tui_term_height(void);
void tui_cursor_hide(void);
void tui_cursor_show(void);
void tui_cursor_move(int row, int col);
void tui_clear_screen(void);
void tui_clear_line(void);
void tui_save_cursor(void);
void tui_restore_cursor(void);

/* ── Box drawing ──────────────────────────────────────────────────────── */

/* Draw a box with optional title, width auto-detected from terminal */
void tui_box(const char *title, const char *body, tui_box_style_t style,
             const char *border_color, int width);

/* Draw a horizontal divider */
void tui_divider(tui_box_style_t style, const char *color, int width);

/* ── Panels ───────────────────────────────────────────────────────────── */
typedef struct {
    const char *title;
    const char *body;
    const char *color;       /* border color */
    const char *title_color; /* title color (NULL = same as border) */
    tui_box_style_t style;
    int width;               /* 0 = auto */
    int padding;             /* internal horizontal padding */
} tui_panel_t;

void tui_panel(const tui_panel_t *p);

/* ── Spinners ─────────────────────────────────────────────────────────── */
typedef enum {
    SPINNER_DOTS,       /* ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏ */
    SPINNER_BRAILLE,    /* ⣾⣽⣻⢿⡿⣟⣯⣷ */
    SPINNER_LINE,       /* -\|/ */
    SPINNER_ARROW,      /* ←↖↑↗→↘↓↙ */
    SPINNER_STAR,       /* ✶✸✹✺✹✷ */
    SPINNER_PULSE,      /* ◐◓◑◒ */
} tui_spinner_type_t;

typedef struct {
    tui_spinner_type_t type;
    int frame;
    const char *label;
    const char *color;
    bool active;
} tui_spinner_t;

void tui_spinner_init(tui_spinner_t *s, tui_spinner_type_t type,
                      const char *label, const char *color);
void tui_spinner_tick(tui_spinner_t *s);
void tui_spinner_done(tui_spinner_t *s, const char *final_label);

/* ── Progress bar ─────────────────────────────────────────────────────── */
void tui_progress(const char *label, double pct, int width,
                  const char *fill_color, const char *empty_color);

/* ── Table rendering ──────────────────────────────────────────────────── */
#define TUI_TABLE_MAX_COLS 16
#define TUI_TABLE_MAX_ROWS 256

typedef struct {
    const char *headers[TUI_TABLE_MAX_COLS];
    const char *rows[TUI_TABLE_MAX_ROWS][TUI_TABLE_MAX_COLS];
    int col_count;
    int row_count;
    const char *header_color;
    const char *border_color;
    tui_box_style_t style;
} tui_table_t;

void tui_table_init(tui_table_t *t, int cols, const char *header_color);
void tui_table_header(tui_table_t *t, ...);
void tui_table_row(tui_table_t *t, ...);
void tui_table_render(const tui_table_t *t, int width);

/* ── Status badges ────────────────────────────────────────────────────── */
void tui_badge(const char *text, const char *fg, const char *bg);
void tui_tag(const char *text, const char *color);

/* ── Convenience: styled output ───────────────────────────────────────── */
void tui_header(const char *text, const char *color);
void tui_subheader(const char *text);
void tui_info(const char *text);
void tui_success(const char *text);
void tui_warning(const char *text);
void tui_error(const char *text);

/* ── Welcome banner ───────────────────────────────────────────────────── */
void tui_welcome(const char *model, int tool_count, const char *version);

/* ── Streaming block wrappers ─────────────────────────────────────────── */
void tui_stream_start(void);
void tui_stream_text(const char *text);
void tui_stream_tool(const char *name, const char *id);
void tui_stream_tool_result(const char *name, bool ok, const char *preview);
void tui_stream_end(void);

/* ── Swarm UI ─────────────────────────────────────────────────────────── */
typedef struct {
    int id;
    const char *task;
    const char *status;     /* "running", "done", "error" */
    double progress;
    const char *last_output;
} tui_swarm_entry_t;

void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width);

#endif
