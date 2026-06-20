#ifndef DSCO_TUI_H
#define DSCO_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>

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

/* ── Activity indicator (Claude Code aesthetic) ───────────────────────── */
/* ⏺  U+23FA — the primary action bullet */
#define TUI_RECORD        "\xe2\x8f\xba"
/* ⎿  U+23BF — indent/result line leader */
#define TUI_INDENT_LEAD   "\xe2\x8e\xbf"
/* 256-color orange (approx #FF5F00) */
#define TUI_ORANGE        "\033[38;5;202m"
/* Truecolor orange used for the record glyph */
#define TUI_ORANGE_RGB    "\033[38;2;255;95;0m"
/* Middle dot separator · */
#define TUI_SEP           "\xc2\xb7"

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

/* Terminal output mutex — serialize cursor-positioned writes between threads */
void tui_term_lock(void);
void tui_term_unlock(void);

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
void tui_welcome(const char *model, int core_count, int total_count, const char *version);

/* ── Streaming block wrappers ─────────────────────────────────────────── */
void tui_stream_start(void);
void tui_stream_text(const char *text);
void tui_stream_tool(const char *name, const char *id);
void tui_stream_tool_result(const char *name, bool ok, const char *preview);
void tui_stream_end(void);

/* ── Glyph Tier System ─────────────────────────────────────────────────── */
/* Four rendering tiers based on terminal + font capabilities.
 * All glyphs used in the binary are defined here with fallbacks.
 * Detection runs once at startup and caches the result.
 *
 * 2026 standard: Nerd Font v3+ assumed for modern terminals.
 * Nerd Font codepoints live in the Private Use Area:
 *   - Powerline:       U+E0A0–U+E0D4
 *   - Powerline Extra: U+E0B0–U+E0D4
 *   - Devicons:        U+E700–U+E7C5
 *   - Font Awesome:    U+F000–U+F2E0
 *   - Octicons:        U+F400–U+F532
 *   - Material:        U+F0001–U+F1AF0
 *   - Weather:         U+E300–U+E3E3
 *   - Codicons:        U+EA60–U+EC00
 */

typedef enum {
    TUI_GLYPH_ASCII,    /* pure ASCII — works everywhere */
    TUI_GLYPH_UNICODE,  /* BMP Unicode — box drawing, braille, symbols */
    TUI_GLYPH_FULL,     /* full Unicode — emoji, supplementary planes */
    TUI_GLYPH_NERD,     /* Nerd Font v3+ — devicons, powerline, FA, etc. */
} tui_glyph_tier_t;

typedef struct {
    /* ── Status indicators ──────────────────────────────────────────── */
    const char *ok;              /* nf:  / full: ✓  / uni: ✓  / ascii: + */
    const char *fail;            /* nf:  / full: ✗  / uni: ✗  / ascii: x */
    const char *warn;            /* nf:  / full: ⚠  / uni: (!) / ascii: ! */
    const char *info;            /* nf:  / full: ℹ  / uni: (i) / ascii: i */

    /* ── Bullets & markers ──────────────────────────────────────────── */
    const char *bullet;          /* nf:  / full: ●  / ascii: * */
    const char *circle_open;     /* nf:  / full: ○  / ascii: o */
    const char *circle_dot;      /* nf:  / full: ◉  / ascii: @ */
    const char *circle_ring;     /* nf:  / full: ◎  / ascii: O */
    const char *diamond;         /* nf:  / full: ◆  / ascii: * */
    const char *diamond_open;    /* nf:  / full: ◇  / ascii: <> */
    const char *sparkle;         /* nf:  / full: ✦  / ascii: * */
    const char *florette;        /* nf:  / full: ✿  / ascii: * */

    /* ── Arrows & motion ────────────────────────────────────────────── */
    const char *arrow_right;     /* nf:  / full: →  / ascii: -> */
    const char *arrow_left;      /* nf:  / full: ←  / ascii: <- */
    const char *arrow_up;        /* nf:  / full: ▲  / ascii: ^ */
    const char *arrow_down;      /* nf:  / full: ▼  / ascii: v */
    const char *arrow_cycle;     /* nf:  / full: ↻  / ascii: ~ */

    /* ── Progress blocks ────────────────────────────────────────────── */
    const char *block_full;
    const char *block_med;
    const char *block_light;
    const char *block_dark;
    const char *vblock[9];       /* [0]=space [1]=▁ .. [8]=█ */

    /* ── Spinners ───────────────────────────────────────────────────── */
    const char *spin_dots[10];
    int         spin_dots_n;
    const char *spin_thick[8];
    int         spin_thick_n;
    const char *spin_orbit[4];
    int         spin_orbit_n;
    const char *spin_orbit_inner[2];
    int         spin_orbit_inner_n;
    const char *spin_pulse[4];
    int         spin_pulse_n;
    const char *spin_line[4];
    int         spin_line_n;
    const char *spin_arrow[8];
    int         spin_arrow_n;
    const char *spin_star[6];
    int         spin_star_n;

    /* ── Contextual icons ───────────────────────────────────────────── */
    const char *icon_think;      /* nf:  brain/lightbulb */
    const char *icon_lightning;  /* nf:  */
    const char *icon_gear;       /* nf:  */
    const char *icon_timer;      /* nf:  */
    const char *icon_lock;       /* nf:  */
    const char *icon_money;      /* nf:  */
    const char *icon_globe;      /* nf:  */
    const char *icon_rocket;     /* nf:  */
    const char *icon_fire;       /* nf:  */
    const char *icon_link;       /* nf:  */
    const char *icon_eyes;       /* nf:  */

    /* ── Nerd Font extras (NULL on lower tiers) ─────────────────────── */
    const char *icon_folder;     /* nf:  */
    const char *icon_file;       /* nf:  */
    const char *icon_code;       /* nf:  */
    const char *icon_terminal;   /* nf:  */
    const char *icon_git;        /* nf:  */
    const char *icon_database;   /* nf:  */
    const char *icon_cloud;      /* nf:  */
    const char *icon_bug;        /* nf:  */
    const char *icon_cpu;        /* nf:  */
    const char *icon_network;    /* nf: 󰛳  */
    const char *icon_key;        /* nf:  */
    const char *icon_shield;     /* nf: 󰒃  */
    const char *icon_search;     /* nf:  */
    const char *icon_download;   /* nf:  */
    const char *icon_upload;     /* nf:  */
    const char *icon_sync;       /* nf:  */
    const char *icon_play;       /* nf:  */
    const char *icon_pause;      /* nf:  */
    const char *icon_stop;       /* nf:  */
    const char *icon_skip;       /* nf:  */
    const char *icon_chat;       /* nf:  */
    const char *icon_robot;      /* nf: 󰚩  */
    const char *icon_brain;      /* nf: 󰧑  */
    const char *icon_wand;       /* nf:  */
    const char *icon_graph;      /* nf:  */

    /* ── Powerline separators ───────────────────────────────────────── */
    const char *pl_right;        /* nf:  U+E0B0 */
    const char *pl_right_thin;   /* nf:  U+E0B1 */
    const char *pl_left;         /* nf:  U+E0B2 */
    const char *pl_left_thin;    /* nf:  U+E0B3 */
    const char *pl_round_right;  /* nf:  U+E0B4 */
    const char *pl_round_left;   /* nf:  U+E0B6 */

    /* ── Box-drawing ────────────────────────────────────────────────── */
    const char *hline;
    const char *hline_heavy;
    const char *vline;
    const char *corner_tl;
    const char *corner_tr;
    const char *corner_bl;
    const char *corner_br;

    /* ── Dot trail for heartbeat ────────────────────────────────────── */
    const char *dot_large;
    const char *dot_medium;
    const char *dot_small;
} tui_glyphs_t;

/* Detect glyph tier and cache */
tui_glyph_tier_t  tui_detect_glyph_tier(void);

/* Get the active glyph set (singleton, initialized on first call) */
const tui_glyphs_t *tui_glyph(void);

/* Override tier (e.g. from env var DSCO_GLYPH=ascii|unicode|full) */
void tui_set_glyph_tier(tui_glyph_tier_t tier);

/* ── True Color Foundation ─────────────────────────────────────────────── */

typedef struct { unsigned char r, g, b; } tui_rgb_t;

/* Terminal capability levels */
typedef enum {
    TUI_COLOR_NONE,      /* no color */
    TUI_COLOR_16,        /* basic 16 ANSI colors */
    TUI_COLOR_256,       /* 256-color xterm palette */
    TUI_COLOR_TRUECOLOR, /* 24-bit RGB */
} tui_color_level_t;

/* Detect and cache terminal color capabilities */
tui_color_level_t tui_detect_color_level(void);
bool              tui_supports_truecolor(void);

/* HSV (h=0-360, s/v=0-1) → RGB conversion */
tui_rgb_t tui_hsv_to_rgb(float h, float s, float v);

/* Write a foreground truecolor escape to stderr */
void tui_fg_rgb(tui_rgb_t c);

/* Print text with horizontal hue gradient (h_start→h_end degrees) */
void tui_gradient_text(const char *text, float h_start, float h_end, float s, float v);

/* Print a gradient-colored horizontal rule */
void tui_gradient_divider(int width, float h_start, float h_end);

/* Print dim transition divider between content blocks */
void tui_transition_divider(void);

/* ── Tool Type Colors ─────────────────────────────────────────────────── */

typedef enum {
    TUI_TOOL_READ,    /* blue — file reading */
    TUI_TOOL_WRITE,   /* gold/yellow — file writing */
    TUI_TOOL_EXEC,    /* purple — command execution */
    TUI_TOOL_WEB,     /* green — web/network */
    TUI_TOOL_CRYPTO,  /* pink/magenta — crypto/hash operations */
    TUI_TOOL_MATH,    /* orange — math/eval/compute */
    TUI_TOOL_DATA,    /* teal — search/query/database */
    TUI_TOOL_OTHER,   /* cyan — default */
} tui_tool_type_t;

tui_tool_type_t tui_classify_tool(const char *name);
const char     *tui_tool_color(tui_tool_type_t type);
tui_rgb_t       tui_tool_rgb(tui_tool_type_t type);

/* ── Async Spinner (single tool) ──────────────────────────────────────── */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    volatile bool   running;
    const char     *label;
    const char     *color;
    tui_rgb_t       rgb;
    bool            use_rgb;
    double          start_time;
} tui_async_spinner_t;

void tui_async_spinner_start(tui_async_spinner_t *s, const char *label,
                             tui_tool_type_t tool_type);
void tui_async_spinner_stop(tui_async_spinner_t *s, bool ok,
                            const char *result_preview, double elapsed_ms,
                            const char *suffix);

/* ── Batch Spinner (multi-tool) ───────────────────────────────────────── */

#define TUI_BATCH_MAX 32

typedef struct {
    char     name[64];
    char     args_preview[128];   /* tool args shown inline while spinning */
    bool     done;
    bool     ok;
    char     preview[128];
    double   elapsed_ms;
    tui_tool_type_t type;
} tui_batch_entry_t;

typedef struct {
    pthread_t        thread;
    pthread_mutex_t  mutex;
    volatile bool    running;
    tui_batch_entry_t entries[TUI_BATCH_MAX];
    int              count;
    double           start_time;
} tui_batch_spinner_t;

void tui_batch_spinner_start(tui_batch_spinner_t *bs, const char **names, int count);
void tui_batch_spinner_complete(tui_batch_spinner_t *bs, int idx, bool ok,
                                const char *preview, double elapsed_ms);
void tui_batch_spinner_stop(tui_batch_spinner_t *bs);

/* Aggregate summary line after batch completion */
void tui_batch_summary(const tui_batch_spinner_t *bs, const char *cost_suffix);

/* ── Live Status Bar ──────────────────────────────────────────────────── */

typedef struct {
    pthread_mutex_t mutex;
    bool     enabled;
    bool     visible;
    bool     show_clock;
    bool     panel_active;  /* true while tui_composer_read is actively reading
                             * keystrokes — drives dual-state caret color */
    char     model[64];
    char     slot_name[64]; /* active workspace slot, empty = default */
    int      input_tokens;
    int      output_tokens;
    double   cost;
    int      turn;
    int      tools_used;
    int      panel_rows;    /* bottom panel rows: top rule + input + status (3) */
} tui_status_bar_t;

void tui_status_bar_init(tui_status_bar_t *sb, const char *model);
void tui_status_bar_set_model(tui_status_bar_t *sb, const char *model, const char *slot_name);
void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok,
                           double cost, int turn, int tools);
void tui_status_bar_enable(tui_status_bar_t *sb);
void tui_status_bar_disable(tui_status_bar_t *sb);
void tui_status_bar_render(tui_status_bar_t *sb);

/* ── Input Panel (ephemeral bottom panel) ─────────────────────────────── */
/* The bottom panel is 3 rows, painted only when reading user input:
 *   row N-2 : top horizontal rule with model badge ─[ model ]──────
 *   row N-1 : "❯ " + current input
 *   row N   : powerline status bar (drawn by tui_status_bar_render)
 *
 * When the user submits, the panel is erased and the input is echoed into
 * scrollback, so the agent's response streams freely through the terminal.
 * The panel re-renders before the next read. No DECSTBM scroll region:
 * text rendering uses the whole terminal during streaming.
 */
#define TUI_COMPOSER_PANEL_ROWS   3
#define TUI_COMPOSER_BUF_CAP      16384
#define TUI_PANEL_NOTIFY_SLOTS    2
#define TUI_PANEL_NOTIFY_TTL_S    8.0

/* Severity levels for panel notifications. Drives the color. */
typedef enum {
    TUI_PANEL_NOTE_INFO,     /* dim cyan */
    TUI_PANEL_NOTE_OK,       /* green */
    TUI_PANEL_NOTE_WARN,     /* yellow */
    TUI_PANEL_NOTE_ERROR,    /* red */
    TUI_PANEL_NOTE_ACTIVITY, /* bright pink, for realtime activity */
} tui_panel_note_level_t;

/* Push a notification into the middle whitespace of the input panel.
 * Entries live in a fixed-size ring; newest wins. Safe to call from any
 * thread. text is copied. Auto-expires after TUI_PANEL_NOTIFY_TTL_S seconds.
 * Triggers a repaint of the notification rows.
 */
void tui_panel_notify(tui_status_bar_t *sb, tui_panel_note_level_t level,
                      const char *text);

/* Clear all panel notifications. */
void tui_panel_notify_clear(tui_status_bar_t *sb);

/* Set the panel_active flag (controls caret color: colored vs grey). */
void tui_panel_set_active(tui_status_bar_t *sb, bool active);

void tui_input_panel_render(tui_status_bar_t *sb, const char *prompt_hint);
void tui_input_panel_clear(tui_status_bar_t *sb);
void tui_bottom_panel_refresh(tui_status_bar_t *sb, const char *prompt_hint);

/* Push cursor down with newlines until it sits just above the input panel
 * area (row `rows - 3`). No-op if cursor already at/past that row. Queries
 * cursor row via DSR (ESC[6n); briefly enters raw mode if stdin is a tty.
 * Call once after the startup banner + notes finish printing so the bottom
 * panel sits flush against them on tall terminals. */
void tui_pad_to_panel_anchor(void);

/* Persistent multi-line input box. Draws a 5-row box above the status bar
 * and reads input with raw termios. Supports:
 *   • multi-line editing (Alt+Enter / Opt+Enter / Ctrl+J = newline)
 *   • plain Enter submits
 *   • backspace, left/right/home/end, up/down history
 *   • Ctrl+A/E/U/W, Ctrl+C cancels, Ctrl+D on empty returns NULL
 *   • bracketed paste (multi-line paste preserved)
 * Returns `out` on success, NULL on EOF/quit.
 * `prompt` is drawn on the first input row before the text; pass NULL for "❯ ".
 */
char *tui_composer_read(tui_status_bar_t *sb, const char *prompt,
                        char *out, size_t out_sz);

/* ── Swarm UI ─────────────────────────────────────────────────────────── */
typedef struct {
    int id;
    const char *task;
    const char *status;     /* "running", "done", "error" */
    double progress;
    const char *last_output;
} tui_swarm_entry_t;

void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width);

/* ══════════════════════════════════════════════════════════════════════════
 * 40 Strategic UI Features — New structs, enums, and function signatures
 * ══════════════════════════════════════════════════════════════════════════ */

#include "config.h"  /* tui_features_t */

/* Global feature flags pointer (set by agent.c) */
extern tui_features_t *g_tui_features;

/* ── F11: Retry Pulse ─────────────────────────────────────────────────── */
void tui_retry_pulse(const char *label, int attempt, int max, double wait_sec);

/* ── F12: Result Sparkline ────────────────────────────────────────────── */
void tui_sparkline(const double *values, int count, const char *color);
bool tui_try_sparkline(const char *text);

/* ── F14: Cached Badge ────────────────────────────────────────────────── */
void tui_cached_badge(const char *tool_name);

/* ── F15: Context Pressure Gauge ──────────────────────────────────────── */
void tui_context_gauge(int used, int max_tok, int width);

/* ── F17: Auto-Compact Notification ───────────────────────────────────── */
void tui_compact_flash(int before, int after);

/* ── F22: Prompt Token Counter ────────────────────────────────────────── */
int  tui_estimate_tokens(const char *text);
void tui_prompt_token_display(int est, int remaining);

/* ── F26: IPC Message Line ────────────────────────────────────────────── */
void tui_ipc_message_line(const char *from, const char *to,
                           const char *topic, const char *preview);

/* ── F27: Agent Progress Roll-up ──────────────────────────────────────── */
void tui_agent_rollup(int total, int done, int running, int errored);

/* ── F29: Adaptive Color Theme ────────────────────────────────────────── */
typedef enum {
    TUI_THEME_DARK,
    TUI_THEME_LIGHT,
} tui_theme_t;

tui_theme_t tui_detect_theme(void);
void        tui_apply_theme(tui_theme_t theme);

/* Theme-aware color accessors */
const char *tui_theme_dim(void);
const char *tui_theme_bright(void);
const char *tui_theme_accent(void);

/* ── F30: Section Dividers with Context ───────────────────────────────── */
void tui_section_divider(int turn, int tools, double cost, const char *model,
                         double tok_per_sec);
/* Enhanced section divider with success/fail/cache/context stats */
void tui_section_divider_ex(int turn, int tools_ok, int tools_fail,
                            int cache_hits, double cost, const char *model,
                            double tok_per_sec, double ctx_pct,
                            const char *git_branch);

/* ── F31: Status Bar Clock ────────────────────────────────────────────── */
/* show_clock field added to tui_status_bar_t below */

/* ── F32: Error Severity Levels ───────────────────────────────────────── */
typedef enum {
    TUI_ERR_NETWORK,
    TUI_ERR_API,
    TUI_ERR_VALIDATION,
    TUI_ERR_TIMEOUT,
    TUI_ERR_AUTH,
    TUI_ERR_BUDGET,
} tui_err_type_t;

void tui_error_typed(tui_err_type_t type, const char *msg);

/* ── F34: Notification Bell ───────────────────────────────────────────── */
void tui_notify(const char *title, const char *body);

/* ── F2: Typing Cadence ───────────────────────────────────────────────── */
#define TUI_CADENCE_BUF_SIZE 4096

typedef void (*tui_cadence_flush_cb)(const char *buf, int len, void *ctx);

typedef struct {
    char   buf[TUI_CADENCE_BUF_SIZE];
    int    len;
    double last_flush;     /* timestamp of last flush */
    double interval;       /* flush interval in seconds (default 0.016) */
    tui_cadence_flush_cb flush_cb;
    void  *flush_ctx;
} tui_cadence_t;

void tui_cadence_init(tui_cadence_t *c, tui_cadence_flush_cb cb, void *ctx);
void tui_cadence_feed(tui_cadence_t *c, const char *text);
void tui_cadence_flush(tui_cadence_t *c);  /* throttled — holds trailing partial UTF-8 */
void tui_cadence_drain(tui_cadence_t *c);  /* unconditional — emits all buffered bytes */

/* ── F4: Collapsible Thinking ─────────────────────────────────────────── */
#define TUI_THINKING_SUMMARY_MAX 120
typedef struct {
    int    char_count;
    double start_time;
    bool   active;
    char   summary[TUI_THINKING_SUMMARY_MAX]; /* first sentence excerpt */
    int    summary_len;
    bool   summary_done;  /* stop capturing after first sentence */
} tui_thinking_state_t;

void tui_thinking_init(tui_thinking_state_t *t);
void tui_thinking_feed(tui_thinking_state_t *t, const char *text);
void tui_thinking_end(tui_thinking_state_t *t);

/* ── F5: Live Word Count ──────────────────────────────────────────────── */
typedef struct {
    int    words;
    int    chars;
    double start_time;
    double last_render;
} tui_word_counter_t;

void tui_word_counter_init(tui_word_counter_t *w);
void tui_word_counter_feed(tui_word_counter_t *w, const char *text);
void tui_word_counter_render(tui_word_counter_t *w);
void tui_word_counter_end(tui_word_counter_t *w);

/* ── F39: Streaming Throughput Graph ──────────────────────────────────── */
#define TUI_THROUGHPUT_SAMPLES 64

typedef struct {
    double samples[TUI_THROUGHPUT_SAMPLES];
    int    count;
    int    head;
    double last_sample_time;
    int    tokens_since_last;
} tui_throughput_t;

void tui_throughput_init(tui_throughput_t *t);
void tui_throughput_tick(tui_throughput_t *t, int tokens);
void tui_throughput_render(tui_throughput_t *t);

/* ── F8: Flame Timeline ───────────────────────────────────────────────── */
#define TUI_FLAME_MAX 32

typedef struct {
    char   name[64];
    double start_ms;
    double end_ms;
    bool   ok;
    tui_tool_type_t type;
} tui_flame_entry_t;

typedef struct {
    tui_flame_entry_t entries[TUI_FLAME_MAX];
    int count;
    double epoch_ms;  /* reference start time */
} tui_flame_t;

void tui_flame_init(tui_flame_t *f);
void tui_flame_add(tui_flame_t *f, const char *name, double start_ms,
                    double end_ms, bool ok, tui_tool_type_t type);
void tui_flame_render(tui_flame_t *f);

/* ── F10: Tool Dependency Graph ───────────────────────────────────────── */
#define TUI_DAG_MAX_NODES 32
#define TUI_DAG_MAX_EDGES 64

typedef struct {
    char nodes[TUI_DAG_MAX_NODES][64];
    int  node_count;
    struct { int from; int to; } edges[TUI_DAG_MAX_EDGES];
    int  edge_count;
} tui_dag_t;

void tui_dag_init(tui_dag_t *d);
int  tui_dag_add_node(tui_dag_t *d, const char *name);
void tui_dag_add_edge(tui_dag_t *d, int from, int to);
void tui_dag_render(tui_dag_t *d);

/* ── F13: Tool Cost Annotations ───────────────────────────────────────── */
void tui_tool_cost(const char *name, int in_tok, int out_tok, const char *model);

/* ── F35: Inline ASCII Charts ─────────────────────────────────────────── */
typedef enum {
    TUI_CHART_BAR,
    TUI_CHART_HBAR,
    TUI_CHART_VBAR,
    TUI_CHART_SPARK,
    TUI_CHART_HEAT,
    /* 60 additional chart type identifiers */
    TUI_CHART_HBAR_THIN,
    TUI_CHART_HBAR_BLOCK,
    TUI_CHART_HBAR_SHADE,
    TUI_CHART_HBAR_DOT,
    TUI_CHART_HBAR_TICK,
    TUI_CHART_HBAR_STEP,
    TUI_CHART_HBAR_STACKED,
    TUI_CHART_HBAR_NEGPOS,
    TUI_CHART_HBAR_DELTA,
    TUI_CHART_HBAR_CANDLE,
    TUI_CHART_HBAR_RANGE,
    TUI_CHART_HBAR_MEDIAN,
    TUI_CHART_HBAR_PCTL,
    TUI_CHART_HBAR_LOG,
    TUI_CHART_HBAR_SQRT,
    TUI_CHART_VBAR_THIN,
    TUI_CHART_VBAR_BLOCK,
    TUI_CHART_VBAR_SHADE,
    TUI_CHART_VBAR_DOT,
    TUI_CHART_VBAR_TICK,
    TUI_CHART_VBAR_STEP,
    TUI_CHART_VBAR_STACKED,
    TUI_CHART_VBAR_NEGPOS,
    TUI_CHART_VBAR_DELTA,
    TUI_CHART_VBAR_CANDLE,
    TUI_CHART_VBAR_RANGE,
    TUI_CHART_VBAR_MEDIAN,
    TUI_CHART_VBAR_PCTL,
    TUI_CHART_VBAR_LOG,
    TUI_CHART_VBAR_SQRT,
    TUI_CHART_SPARK_THIN,
    TUI_CHART_SPARK_DOT,
    TUI_CHART_SPARK_BLOCK,
    TUI_CHART_SPARK_SHADE,
    TUI_CHART_SPARK_STEP,
    TUI_CHART_SPARK_WAVE,
    TUI_CHART_SPARK_DENSE,
    TUI_CHART_SPARK_SMOOTH,
    TUI_CHART_SPARK_DELTA,
    TUI_CHART_SPARK_RANGE,
    TUI_CHART_SPARK_MEDIAN,
    TUI_CHART_SPARK_PCTL,
    TUI_CHART_SPARK_LOG,
    TUI_CHART_SPARK_SQRT,
    TUI_CHART_SPARK_ZIGZAG,
    TUI_CHART_HEAT_BLOCK,
    TUI_CHART_HEAT_DOT,
    TUI_CHART_HEAT_SHADE,
    TUI_CHART_HEAT_BWR,
    TUI_CHART_HEAT_GYR,
    TUI_CHART_HEAT_VIRIDIS,
    TUI_CHART_HEAT_MAGMA,
    TUI_CHART_HEAT_PLASMA,
    TUI_CHART_HEAT_COOLWARM,
    TUI_CHART_HEAT_BINARY,
    TUI_CHART_HEAT_STEPS,
    TUI_CHART_HEAT_DENSE,
    TUI_CHART_HEAT_RANGE,
    TUI_CHART_HEAT_LOG,
    TUI_CHART_HEAT_SQRT,
} tui_chart_type_t;

void tui_chart(tui_chart_type_t type, const char **labels, const double *values,
               int count, int width, int height);

/* ── F7: Citation Footnotes ───────────────────────────────────────────── */
#define TUI_CITATION_MAX 32

typedef struct {
    char tool_name[64];
    char tool_id[64];
    char preview[128];
    double elapsed_ms;
    int  index;   /* footnote number */
} tui_citation_entry_t;

typedef struct {
    tui_citation_entry_t entries[TUI_CITATION_MAX];
    int count;
} tui_citation_t;

void tui_citation_init(tui_citation_t *c);
int  tui_citation_add(tui_citation_t *c, const char *tool_name,
                       const char *tool_id, const char *preview, double elapsed_ms);
void tui_citation_render(tui_citation_t *c);

/* ── F3: Inline Diff Rendering ────────────────────────────────────────── */
void tui_render_diff(const char *text, FILE *out);
bool tui_is_diff(const char *text);

/* ── F36: Table Sort Indicators ───────────────────────────────────────── */
void tui_table_render_sorted(const tui_table_t *t, int width, int sort_col, bool ascending);

/* ── F37: JSON Tree View ──────────────────────────────────────────────── */
void tui_json_tree(const char *json, int max_depth, bool color);

/* ── F16: Conversation Minimap ────────────────────────────────────────── */
typedef struct {
    char type;    /* 'u'=user, 'a'=assistant, 't'=tool */
    int  tokens;  /* estimated tokens */
} tui_minimap_entry_t;

void tui_minimap_render(const tui_minimap_entry_t *entries, int count, int height);

/* ── F19: Branch Indicator ────────────────────────────────────────────── */
#define TUI_BRANCH_HISTORY 8

typedef struct {
    char prompts[TUI_BRANCH_HISTORY][256];
    int  count;
    int  head;
} tui_branch_t;

void tui_branch_init(tui_branch_t *b);
void tui_branch_push(tui_branch_t *b, const char *prompt);
bool tui_branch_detect(tui_branch_t *b, const char *prompt);

/* ── F21: Ghost Suggestions ───────────────────────────────────────────── */
#define TUI_GHOST_MAX 32

typedef struct {
    char history[TUI_GHOST_MAX][256];
    int  count;
} tui_ghost_t;

void        tui_ghost_init(tui_ghost_t *g);
void        tui_ghost_push(tui_ghost_t *g, const char *cmd);
const char *tui_ghost_match(tui_ghost_t *g, const char *prefix);

/* ── F20: Multi-line Input Syntax Highlighting ────────────────────────── */
bool tui_is_code_paste(const char *text);
void tui_highlight_input(const char *text, FILE *out);

/* ── F23: Drag-Drop Preview ───────────────────────────────────────────── */
void tui_image_preview_badge(const char *path, const char *media_type,
                              long size, int w, int h);

/* ── F24: Slash Command Palette ───────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *desc;
} tui_cmd_entry_t;

void tui_command_palette(const tui_cmd_entry_t *cmds, int count, const char *filter);

/* ── F25: Agent Topology ──────────────────────────────────────────────── */
typedef struct {
    int         id;
    int         parent_id;
    const char *task;
    const char *status;
} tui_agent_node_t;

void tui_agent_topology(const tui_agent_node_t *agents, int count);

/* ── F28: Swarm Cost Aggregation ──────────────────────────────────────── */
typedef struct {
    const char *name;
    double      cost;
    int         in_tok;
    int         out_tok;
} tui_swarm_cost_entry_t;

void tui_swarm_cost(const tui_swarm_cost_entry_t *agents, int count, double total);

/* ── F33: Smooth Scroll ───────────────────────────────────────────────── */
typedef struct {
    const char **lines;
    int  line_count;
    int  offset;      /* current scroll position */
    int  page_size;   /* lines per page */
} tui_scroller_t;

void tui_scroller_init(tui_scroller_t *s, const char **lines, int count);
void tui_scroller_render(tui_scroller_t *s);
bool tui_scroller_handle_key(tui_scroller_t *s, int ch);

/* ── F40: Latency Budget Breakdown ────────────────────────────────────── */
typedef struct {
    double dns_ms;          /* CURLINFO_NAMELOOKUP_TIME */
    double connect_ms;      /* CURLINFO_CONNECT_TIME */
    double tls_ms;          /* CURLINFO_APPCONNECT_TIME */
    double ttfb_ms;         /* CURLINFO_STARTTRANSFER_TIME */
    double total_ms;        /* CURLINFO_TOTAL_TIME */
} tui_latency_breakdown_t;

void tui_latency_waterfall(const tui_latency_breakdown_t *b);

/* ── F18: Session Diff on Load ────────────────────────────────────────── */
void tui_session_diff(int msg_count, int tool_calls, int est_tokens,
                       const char *model);

/* ── F1: Token Heatmap ────────────────────────────────────────────────── */
void tui_heatmap_word(const char *word, int len, FILE *out);

/* ── Features listing / toggle ────────────────────────────────────────── */
void tui_features_list(const tui_features_t *f);
bool tui_features_toggle(tui_features_t *f, const char *name);

/* ── Extend status bar with clock ─────────────────────────────────────── */
/* Added show_clock field — use tui_status_bar_set_clock() */
void tui_status_bar_set_clock(tui_status_bar_t *sb, bool show);

/* ═══════════════════════════════════════════════════════════════════════
 *  ADVANCED STATEFUL ABSTRACTIONS
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Notification Queue ──────────────────────────────────────────────── */
/* Priority-based, auto-dismissing, stackable notification system.
 * Notifications can persist across turns or auto-dismiss after TTL. */

typedef enum {
    TUI_NOTIF_DEBUG,       /* dim, auto-dismiss fast */
    TUI_NOTIF_INFO,        /* cyan, standard TTL */
    TUI_NOTIF_SUCCESS,     /* green, standard TTL */
    TUI_NOTIF_WARNING,     /* yellow, longer TTL */
    TUI_NOTIF_ERROR,       /* red, persists until dismissed */
    TUI_NOTIF_CRITICAL,    /* bold red + bell, persists */
} tui_notif_level_t;

#define TUI_NOTIF_QUEUE_MAX  32
#define TUI_NOTIF_MSG_MAX    256
#define TUI_NOTIF_TAG_MAX    32

typedef struct {
    int               id;
    tui_notif_level_t level;
    char              msg[TUI_NOTIF_MSG_MAX];
    char              tag[TUI_NOTIF_TAG_MAX];     /* grouping tag (e.g. "api", "tool") */
    double            created_at;                  /* epoch seconds */
    double            ttl_sec;                     /* 0 = persist forever */
    bool              dismissed;
    bool              seen;                        /* rendered at least once */
    int               count;                       /* coalesced duplicate count */
} tui_notif_t;

typedef struct {
    pthread_mutex_t  mutex;
    tui_notif_t      queue[TUI_NOTIF_QUEUE_MAX];
    int              count;
    int              next_id;
    int              unread;
} tui_notif_queue_t;

void tui_notif_queue_init(tui_notif_queue_t *q);
int  tui_notif_push(tui_notif_queue_t *q, tui_notif_level_t level,
                     const char *tag, const char *fmt, ...);
void tui_notif_dismiss(tui_notif_queue_t *q, int id);
void tui_notif_dismiss_tag(tui_notif_queue_t *q, const char *tag);
void tui_notif_gc(tui_notif_queue_t *q);           /* expire TTL'd entries */
void tui_notif_render(tui_notif_queue_t *q);        /* render visible stack */
int  tui_notif_unread(tui_notif_queue_t *q);
void tui_notif_clear_all(tui_notif_queue_t *q);

/* ── Toast System ────────────────────────────────────────────────────── */
/* Ephemeral single-line messages that appear briefly and vanish.
 * Uses async thread for timed display. */

#define TUI_TOAST_MAX 8
#define TUI_TOAST_MSG_MAX 128

typedef struct {
    char   msg[TUI_TOAST_MSG_MAX];
    char   icon[8];             /* emoji/unicode prefix */
    double expire_at;           /* epoch seconds */
    bool   active;
    tui_notif_level_t level;
} tui_toast_entry_t;

typedef struct {
    pthread_mutex_t   mutex;
    tui_toast_entry_t toasts[TUI_TOAST_MAX];
    int               count;
    pthread_t         thread;
    volatile bool     running;
} tui_toast_t;

void tui_toast_init(tui_toast_t *t);
void tui_toast_show(tui_toast_t *t, tui_notif_level_t level,
                     double duration_sec, const char *fmt, ...);
void tui_toast_tick(tui_toast_t *t);   /* expire old toasts, render active */
void tui_toast_destroy(tui_toast_t *t);

/* ── State Machine Framework ─────────────────────────────────────────── */
/* Generic finite state machine for UI components.
 * Supports enter/exit callbacks, guarded transitions, and state history. */

#define TUI_FSM_MAX_STATES    16
#define TUI_FSM_MAX_TRANS     64
#define TUI_FSM_HISTORY_SIZE  32
#define TUI_FSM_NAME_MAX      32

typedef void (*tui_fsm_action_fn)(void *ctx);
typedef bool (*tui_fsm_guard_fn)(void *ctx);

typedef struct {
    char              name[TUI_FSM_NAME_MAX];
    tui_fsm_action_fn on_enter;
    tui_fsm_action_fn on_exit;
    tui_fsm_action_fn on_tick;   /* called every render cycle while in this state */
} tui_fsm_state_t;

typedef struct {
    int               from;       /* state index */
    int               to;         /* state index */
    int               event;      /* event ID */
    tui_fsm_guard_fn  guard;      /* NULL = always allowed */
    tui_fsm_action_fn action;     /* transition action */
} tui_fsm_trans_t;

typedef struct {
    char              name[TUI_FSM_NAME_MAX];   /* FSM name for debugging */
    tui_fsm_state_t   states[TUI_FSM_MAX_STATES];
    int               state_count;
    tui_fsm_trans_t   transitions[TUI_FSM_MAX_TRANS];
    int               trans_count;
    int               current;                    /* current state index */
    int               history[TUI_FSM_HISTORY_SIZE];
    int               history_len;
    void             *ctx;                        /* user context */
    double            state_entered_at;           /* for time-in-state queries */
} tui_fsm_t;

void tui_fsm_init(tui_fsm_t *fsm, const char *name, void *ctx);
int  tui_fsm_add_state(tui_fsm_t *fsm, const char *name,
                        tui_fsm_action_fn on_enter, tui_fsm_action_fn on_exit,
                        tui_fsm_action_fn on_tick);
void tui_fsm_add_transition(tui_fsm_t *fsm, int from, int to, int event,
                             tui_fsm_guard_fn guard, tui_fsm_action_fn action);
bool tui_fsm_send(tui_fsm_t *fsm, int event);   /* returns true if transition occurred */
void tui_fsm_tick(tui_fsm_t *fsm);               /* calls current state's on_tick */
const char *tui_fsm_current_name(const tui_fsm_t *fsm);
double tui_fsm_time_in_state(const tui_fsm_t *fsm);
void tui_fsm_debug(const tui_fsm_t *fsm);        /* print state + history */

/* ── Render Context ──────────────────────────────────────────────────── */
/* Tracks what's currently displayed on the terminal for smart redraws.
 * Enables partial updates without full clear/repaint. */

#define TUI_RENDER_SLOTS_MAX 16
#define TUI_RENDER_CONTENT_MAX 512

typedef enum {
    TUI_SLOT_EMPTY,
    TUI_SLOT_SPINNER,
    TUI_SLOT_PROGRESS,
    TUI_SLOT_NOTIFICATION,
    TUI_SLOT_TOAST,
    TUI_SLOT_STATUS,
    TUI_SLOT_PANEL,
    TUI_SLOT_TEXT,
} tui_slot_type_t;

typedef struct {
    tui_slot_type_t type;
    int             row;          /* terminal row (-1 = floating) */
    int             height;       /* lines occupied */
    char            content[TUI_RENDER_CONTENT_MAX]; /* last rendered snapshot */
    bool            dirty;        /* needs redraw */
    double          last_render;  /* timestamp */
    int             z_order;      /* stacking priority */
} tui_render_slot_t;

typedef struct {
    tui_render_slot_t slots[TUI_RENDER_SLOTS_MAX];
    int               slot_count;
    int               term_width;
    int               term_height;
    bool              layout_dirty;
    pthread_mutex_t   mutex;
} tui_render_ctx_t;

void tui_render_ctx_init(tui_render_ctx_t *rc);
int  tui_render_slot_alloc(tui_render_ctx_t *rc, tui_slot_type_t type, int z_order);
void tui_render_slot_update(tui_render_ctx_t *rc, int slot_id, const char *content);
void tui_render_slot_free(tui_render_ctx_t *rc, int slot_id);
void tui_render_slot_dirty(tui_render_ctx_t *rc, int slot_id);
void tui_render_flush(tui_render_ctx_t *rc); /* redraw all dirty slots */
void tui_render_ctx_destroy(tui_render_ctx_t *rc);

/* ── Multi-Phase Progress ────────────────────────────────────────────── */
/* Named phases with ETA estimation, throughput tracking, and phase transitions. */

#define TUI_PROGRESS_PHASES_MAX 8
#define TUI_PROGRESS_NAME_MAX   48

typedef struct {
    char   name[TUI_PROGRESS_NAME_MAX];
    double weight;     /* relative weight for total progress */
    double progress;   /* 0.0 - 1.0 within this phase */
    double start_time;
    double end_time;   /* 0 = not finished */
    bool   active;
    bool   complete;
} tui_progress_phase_t;

typedef struct {
    char                  title[TUI_PROGRESS_NAME_MAX];
    tui_progress_phase_t  phases[TUI_PROGRESS_PHASES_MAX];
    int                   phase_count;
    int                   current_phase;
    double                start_time;
    double                last_render;
    /* ETA estimation via exponential moving average */
    double                ema_rate;     /* units per second */
    double                ema_alpha;    /* smoothing factor */
    pthread_mutex_t       mutex;
} tui_multi_progress_t;

void   tui_multi_progress_init(tui_multi_progress_t *mp, const char *title);
int    tui_multi_progress_add_phase(tui_multi_progress_t *mp, const char *name, double weight);
void   tui_multi_progress_start_phase(tui_multi_progress_t *mp, int phase_idx);
void   tui_multi_progress_update(tui_multi_progress_t *mp, double progress);
void   tui_multi_progress_complete_phase(tui_multi_progress_t *mp);
void   tui_multi_progress_render(tui_multi_progress_t *mp);
double tui_multi_progress_total(tui_multi_progress_t *mp);   /* 0.0 - 1.0 */
double tui_multi_progress_eta_sec(tui_multi_progress_t *mp); /* estimated seconds remaining */
void   tui_multi_progress_destroy(tui_multi_progress_t *mp);

/* ── Event Bus ───────────────────────────────────────────────────────── */
/* Lightweight pub/sub for decoupled UI updates. Components subscribe
 * to event types and receive callbacks when events fire. */

typedef enum {
    TUI_EVT_STREAM_START,
    TUI_EVT_STREAM_TEXT,
    TUI_EVT_STREAM_END,
    TUI_EVT_THINKING_START,
    TUI_EVT_THINKING_END,
    TUI_EVT_TOOL_START,
    TUI_EVT_TOOL_COMPLETE,
    TUI_EVT_TOOL_ERROR,
    TUI_EVT_TURN_START,
    TUI_EVT_TURN_END,
    TUI_EVT_PROGRESS,
    TUI_EVT_CONTEXT_PRESSURE,
    TUI_EVT_COST_UPDATE,
    TUI_EVT_SESSION_LOAD,
    TUI_EVT_SESSION_SAVE,
    TUI_EVT_COMPACT,
    TUI_EVT_RETRY,
    TUI_EVT_ERROR,
    TUI_EVT_CUSTOM,
    TUI_EVT__COUNT
} tui_event_type_t;

typedef struct {
    tui_event_type_t type;
    const char      *source;     /* component name */
    double           timestamp;
    union {
        struct { const char *text; int len; }        text;
        struct { const char *name; const char *id; } tool;
        struct { double pct; const char *label; }    progress;
        struct { int used; int max; }                context;
        struct { double total; double delta; }       cost;
        struct { int code; const char *msg; }        error;
        struct { int key; void *data; }              custom;
    } data;
} tui_event_t;

typedef void (*tui_event_handler_fn)(const tui_event_t *event, void *ctx);

#define TUI_EVT_SUBS_MAX 64

typedef struct {
    tui_event_type_t      type;
    tui_event_handler_fn  handler;
    void                 *ctx;
    bool                  active;
} tui_evt_sub_t;

typedef struct {
    tui_evt_sub_t   subs[TUI_EVT_SUBS_MAX];
    int             sub_count;
    pthread_mutex_t mutex;
    /* Ring buffer for recent events (debugging/replay) */
    tui_event_t     history[64];
    int             history_head;
    int             history_count;
} tui_event_bus_t;

void tui_event_bus_init(tui_event_bus_t *bus);
int  tui_event_subscribe(tui_event_bus_t *bus, tui_event_type_t type,
                          tui_event_handler_fn handler, void *ctx);
void tui_event_unsubscribe(tui_event_bus_t *bus, int sub_id);
void tui_event_emit(tui_event_bus_t *bus, const tui_event_t *event);
void tui_event_emit_simple(tui_event_bus_t *bus, tui_event_type_t type,
                            const char *source);
void tui_event_bus_dump(const tui_event_bus_t *bus, int max_events);
void tui_event_bus_destroy(tui_event_bus_t *bus);

/* ── Streaming Display Pipeline ──────────────────────────────────────── */
/* Formalizes the streaming state machine with proper phase tracking,
 * composable middleware, and metrics collection. */

typedef enum {
    TUI_STREAM_IDLE,
    TUI_STREAM_THINKING,
    TUI_STREAM_TEXT,
    TUI_STREAM_TOOL_PENDING,    /* tool announced, input accumulating */
    TUI_STREAM_TOOL_RUNNING,    /* tool executing */
    TUI_STREAM_TOOL_COMPLETE,
    TUI_STREAM_DONE,
    TUI_STREAM_ERROR,
} tui_stream_phase_t;

typedef struct {
    tui_stream_phase_t phase;
    double             phase_start;
    int                thinking_tokens;
    int                text_tokens;
    int                tool_count;
    int                tool_errors;
    double             total_tool_ms;
    char               current_tool[64];
    /* Throughput tracking */
    int                tokens_this_second;
    double             last_second;
    double             peak_tok_per_sec;
    double             avg_tok_per_sec;
    int                sample_count;
} tui_stream_state_t;

void              tui_stream_state_init(tui_stream_state_t *ss);
void              tui_stream_state_transition(tui_stream_state_t *ss,
                                              tui_stream_phase_t new_phase);
void              tui_stream_state_token(tui_stream_state_t *ss, int count);
void              tui_stream_state_render_badge(const tui_stream_state_t *ss);
const char       *tui_stream_phase_name(tui_stream_phase_t phase);
tui_stream_phase_t tui_stream_state_phase(const tui_stream_state_t *ss);

/* ── Stream Heartbeat (anti-hang visual feedback) ─────────────────────── */
/* Background thread that detects when a stream is active but no visible
 * output has reached the terminal.  After a configurable silence threshold
 * (default 2 s) it shows a subtle pulsing indicator on stderr so the user
 * knows the process hasn't frozen.  The indicator auto-hides as soon as
 * any visible output fires (text, thinking, tool start). */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    volatile bool   running;        /* false → thread should exit */
    volatile bool   visible;        /* heartbeat indicator currently on screen */
    double          last_poke;      /* timestamp of last visible output */
    double          start_time;     /* when stream started */
    double          silence_thresh; /* seconds before showing indicator (def 2.0) */
    unsigned long   bytes_recv;     /* total bytes received from curl */
    const char     *phase_label;    /* current phase hint (nullable) */
    int             poke_count;     /* total pokes received */
    int             phase_changes;  /* number of phase transitions */
} tui_stream_heartbeat_t;

void tui_stream_heartbeat_start(tui_stream_heartbeat_t *hb);
void tui_stream_heartbeat_poke(tui_stream_heartbeat_t *hb, const char *phase);
void tui_stream_heartbeat_recv(tui_stream_heartbeat_t *hb, size_t bytes);
void tui_stream_heartbeat_stop(tui_stream_heartbeat_t *hb);

/* ══════════════════════════════════════════════════════════════════════════
 * SUBSTRUCTURAL IMPROVEMENTS — Output Serialization & Coordination
 *
 * Problem: 807+ unprotected stderr writes across 4+ threads (agent main,
 * heartbeat, ESC poller, md_feed) with no single coordination point.
 *
 * Solution: A serialized output queue that all writers enqueue into,
 * flushed by a single render thread on a 16ms tick (like Claude Code's
 * throttled pipeline). Also wires the existing FSM, Event Bus, and
 * Render Context into the actual streaming pipeline.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Serialized Output Queue ─────────────────────────────────────────── */
/* All terminal writes go through this queue. A single render thread
 * drains it on a 16ms tick, preventing race conditions between
 * heartbeat, markdown streaming, spinners, and the agent loop.
 *
 * Inspired by Claude Code's "throttled render" pattern:
 * leading: true (react immediately), trailing: true (coalesce). */

#define TUI_OUTQ_SIZE        4096  /* ring buffer entries */
#define TUI_OUTQ_MSG_MAX     2048  /* max bytes per queued message */
#define TUI_OUTQ_TICK_MS     16    /* render tick interval (~60 fps) */

typedef enum {
    TUI_OUT_TEXT,          /* raw text to stderr */
    TUI_OUT_CLEAR_LINE,    /* \r\033[K */
    TUI_OUT_CURSOR_MOVE,   /* ESC[row;colH */
    TUI_OUT_STYLE,         /* ESC[...m */
    TUI_OUT_TITLE,         /* OSC 2 title */
    TUI_OUT_BELL,          /* \a */
    TUI_OUT_FLUSH,         /* force immediate flush */
} tui_out_type_t;

typedef struct {
    tui_out_type_t type;
    int            priority;     /* higher = rendered first within tick */
    int            len;
    char           data[TUI_OUTQ_MSG_MAX];
} tui_out_entry_t;

typedef struct {
    tui_out_entry_t *ring;         /* ring buffer */
    int              head;         /* write position */
    int              tail;         /* read position */
    int              capacity;
    int              count;        /* current entries */
    int              dropped;      /* overflow drops (diagnostic) */

    pthread_mutex_t  mutex;
    pthread_cond_t   cond;         /* signal render thread */
    pthread_t        render_thread;
    volatile bool    running;

    FILE            *out;          /* output target (stderr) */

    /* Statistics */
    int              total_writes;
    int              total_flushes;
    double           total_flush_ms;
    double           max_flush_ms;
} tui_output_queue_t;

/* Initialize the output queue and start the render thread */
void tui_outq_init(tui_output_queue_t *q, FILE *out);

/* Stop the render thread and destroy the queue */
void tui_outq_destroy(tui_output_queue_t *q);

/* Enqueue output (thread-safe, non-blocking). All TUI writes should
 * go through this instead of direct fprintf(stderr, ...). */
void tui_outq_write(tui_output_queue_t *q, const char *text);
void tui_outq_writef(tui_output_queue_t *q, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Enqueue with priority (higher = rendered first in batch) */
void tui_outq_write_pri(tui_output_queue_t *q, int priority, const char *text);

/* Enqueue a clear-line operation */
void tui_outq_clear_line(tui_output_queue_t *q);

/* Force immediate flush (bypass tick coalescing) */
void tui_outq_flush_sync(tui_output_queue_t *q);

/* Get queue statistics */
void tui_outq_stats(const tui_output_queue_t *q,
                     int *total_writes, int *total_flushes,
                     int *dropped, double *avg_flush_ms);

/* Global output queue (initialized in agent_run, available everywhere) */
extern tui_output_queue_t *g_outq;

/* ── Streaming FSM Wiring ────────────────────────────────────────────── */
/* Pre-built FSM for the streaming pipeline. Replaces the ad-hoc
 * boolean flags (s_in_thinking_block, s_in_text_block, etc.) with
 * proper state transitions and callbacks. */

/* FSM event IDs for streaming */
#define TUI_FSM_EVT_STREAM_START    1
#define TUI_FSM_EVT_THINKING_START  2
#define TUI_FSM_EVT_THINKING_END    3
#define TUI_FSM_EVT_TEXT_START      4
#define TUI_FSM_EVT_TOOL_START      5
#define TUI_FSM_EVT_TOOL_COMPLETE   6
#define TUI_FSM_EVT_STREAM_END      7
#define TUI_FSM_EVT_ERROR           8
#define TUI_FSM_EVT_INTERRUPT       9
#define TUI_FSM_EVT_RESUME          10

/* State indices — match tui_streaming_fsm_create() order */
#define TUI_STREAM_ST_IDLE          0
#define TUI_STREAM_ST_THINKING      1
#define TUI_STREAM_ST_TEXT          2
#define TUI_STREAM_ST_TOOL_PENDING  3
#define TUI_STREAM_ST_TOOL_RUNNING  4
#define TUI_STREAM_ST_TOOL_COMPLETE 5
#define TUI_STREAM_ST_DONE          6
#define TUI_STREAM_ST_ERROR         7

/* Create a pre-wired streaming FSM with all states and transitions.
 * ctx is passed to all callbacks (typically agent's callback context). */
void tui_streaming_fsm_create(tui_fsm_t *fsm, void *ctx);

/* ── Animation Clock ─────────────────────────────────────────────────── */
/* Global demand-driven animation clock. Only ticks when subscribers
 * exist. All animated components (spinners, shimmer, heartbeat)
 * synchronize through this clock. Inspired by Claude Code's global
 * shared clock pattern.
 *
 * The clock is demand-driven: starts when first subscriber registers,
 * stops when last subscriber unregisters. This means zero CPU overhead
 * when no animations are active. */

#define TUI_ANIM_MAX_SUBS 16

typedef void (*tui_anim_cb)(double elapsed_ms, void *ctx);

typedef struct {
    tui_anim_cb  callback;
    void        *ctx;
    bool         active;
    bool         keep_alive;  /* false = pause when offscreen */
} tui_anim_sub_t;

typedef struct {
    pthread_t        thread;
    pthread_mutex_t  mutex;
    volatile bool    running;

    tui_anim_sub_t   subs[TUI_ANIM_MAX_SUBS];
    int              sub_count;
    int              active_count;   /* subs with keep_alive=true */

    double           start_time;     /* epoch of clock start */
    double           tick_time;      /* current synchronized tick time */
    int              interval_ms;    /* tick interval (default 16ms) */

    /* Diagnostics */
    int              total_ticks;
    double           max_tick_ms;
} tui_anim_clock_t;

void tui_anim_clock_init(tui_anim_clock_t *clk, int interval_ms);
void tui_anim_clock_destroy(tui_anim_clock_t *clk);

/* Subscribe a callback. Returns subscriber ID. */
int  tui_anim_subscribe(tui_anim_clock_t *clk, tui_anim_cb callback,
                          void *ctx, bool keep_alive);
void tui_anim_unsubscribe(tui_anim_clock_t *clk, int sub_id);

/* Pause/resume a subscriber (e.g., when offscreen) */
void tui_anim_set_active(tui_anim_clock_t *clk, int sub_id, bool active);

/* Global animation clock */
extern tui_anim_clock_t *g_anim_clock;

/* ══════════════════════════════════════════════════════════════════════════
 * Claude Code-inspired UI systems — implemented in pure C / ANSI escapes
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Screen Buffer with Damage Tracking ──────────────────────────────── */
/* Avoids full redraws. Only writes cells that changed since last frame.
 * Inspired by Claude Code's output.ts screen buffer system. */

#define SCRBUF_MAX_WIDTH   512
#define SCRBUF_MAX_HEIGHT  128

typedef struct {
    char     ch[8];       /* UTF-8 character (up to 4 bytes + null) */
    int      style_id;    /* interned style index (0 = default) */
    unsigned char width;  /* display width: 1 = narrow, 2 = wide, 0 = continuation */
} tui_cell_t;

typedef struct {
    tui_cell_t *cells;        /* current frame: width × height */
    tui_cell_t *prev;         /* previous frame for diff */
    bool       *dirty;        /* per-cell dirty flags */
    int         width;
    int         height;
    int         cursor_x;     /* logical cursor position */
    int         cursor_y;
    FILE       *out;
} tui_screenbuf_t;

void tui_screenbuf_init(tui_screenbuf_t *sb, int width, int height, FILE *out);
void tui_screenbuf_free(tui_screenbuf_t *sb);
void tui_screenbuf_resize(tui_screenbuf_t *sb, int width, int height);
void tui_screenbuf_clear(tui_screenbuf_t *sb);
void tui_screenbuf_put(tui_screenbuf_t *sb, int x, int y,
                        const char *ch, int style_id, int char_width);
void tui_screenbuf_write(tui_screenbuf_t *sb, int x, int y,
                          const char *text, int style_id);
void tui_screenbuf_flush(tui_screenbuf_t *sb);  /* diff & output only changed cells */

/* Style intern table: maps style_id ↔ ANSI sequence */
#define TUI_MAX_STYLES 64

typedef struct {
    char ansi[64];   /* full ANSI SGR sequence */
    int  len;
} tui_style_entry_t;

int  tui_style_intern(const char *ansi_seq); /* returns style_id */
const char *tui_style_get(int style_id);     /* returns ANSI sequence */

/* ── OSC 8 Hyperlinks ────────────────────────────────────────────────── */
/* Clickable file paths and URLs in terminals that support OSC 8.
 * Falls back to plain text in unsupported terminals. */

bool tui_supports_hyperlinks(void);

/* Write a clickable hyperlink to FILE */
void tui_hyperlink(FILE *out, const char *url, const char *display_text);

/* Format a file:// URI from a local path */
void tui_file_link(FILE *out, const char *path, const char *display);

/* Format a clickable file path with line number */
void tui_file_line_link(FILE *out, const char *path, int line,
                         const char *display);

/* ── Wide Character Width Detection ──────────────────────────────────── */
/* Handles emoji, CJK, combining marks, and other multi-width characters.
 * Inspired by Claude Code's grapheme cluster detection. */

/* Returns display width of a Unicode codepoint (0, 1, or 2) */
int  tui_char_width(unsigned int codepoint);

/* Returns display width of a UTF-8 string */
int  tui_str_display_width(const char *s);

/* Decode one UTF-8 codepoint from string. Returns bytes consumed. */
int  tui_utf8_decode(const char *s, unsigned int *codepoint);

/* Truncate UTF-8 string to fit within max_width display columns.
 * Writes to dst. Returns actual display width used. */
int  tui_utf8_truncate(const char *src, char *dst, size_t dst_len,
                        int max_width);

/* ── Terminal Title (OSC 0/2) ────────────────────────────────────────── */

void tui_set_title(const char *title);
void tui_set_title_fmt(const char *fmt, ...);
void tui_reset_title(void);

/* ── Shimmer / Gradient Animation ────────────────────────────────────── */
/* Animated gradient text for thinking state, similar to Claude Code's
 * shimmer effect using block characters and color interpolation. */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    volatile bool   running;
    const char     *label;
    double          start_time;
    int             hue_offset;  /* current hue offset for animation */
    tui_rgb_t       color_a;     /* gradient start */
    tui_rgb_t       color_b;     /* gradient end */
} tui_shimmer_t;

void tui_shimmer_start(tui_shimmer_t *sh, const char *label,
                        tui_rgb_t color_a, tui_rgb_t color_b);
void tui_shimmer_stop(tui_shimmer_t *sh);

/* One-shot shimmer text (non-animated) */
void tui_shimmer_text(FILE *out, const char *text,
                       tui_rgb_t color_a, tui_rgb_t color_b);

/* ── Permission Prompt Dialog ────────────────────────────────────────── */
/* Styled interactive Y/N dialog with box drawing, similar to Claude Code's
 * tool permission UI. Pure ANSI — no ncurses. */

typedef enum {
    TUI_PERM_ALLOW,
    TUI_PERM_DENY,
    TUI_PERM_ALWAYS,
    TUI_PERM_CANCEL,
} tui_perm_result_t;

/* Show a permission prompt and wait for user input.
 * Returns the user's choice. */
tui_perm_result_t tui_permission_prompt(const char *tool_name,
                                         const char *description,
                                         const char *detail);

/* Lightweight yes/no prompt with styled box */
bool tui_confirm(const char *question);

/* ── Structured Diff Display ─────────────────────────────────────────── */
/* Proper unified diff renderer with line numbers, context lines,
 * and side-by-side support. Inspired by Claude Code's StructuredDiff. */

typedef struct {
    char   type;         /* ' ' = context, '+' = add, '-' = remove, '@' = hunk header */
    int    old_line;     /* original line number (-1 if added) */
    int    new_line;     /* new line number (-1 if removed) */
    char  *text;         /* line content (no newline) */
} tui_diff_line_t;

typedef struct {
    tui_diff_line_t *lines;
    int              count;
    int              cap;
    char             old_file[256];
    char             new_file[256];
} tui_diff_t;

void tui_diff_init(tui_diff_t *d);
void tui_diff_free(tui_diff_t *d);
bool tui_diff_parse(tui_diff_t *d, const char *unified_diff);
void tui_diff_render(const tui_diff_t *d, FILE *out, int context_lines);
void tui_diff_render_inline(const tui_diff_t *d, FILE *out); /* word-level changes */

/* ── Full-Screen Pager ───────────────────────────────────────────────── */
/* Interactive pager with search, line numbers, and scrolling.
 * Replaces basic scroller with Claude Code-like screen management. */

typedef struct {
    const char **lines;
    int          line_count;
    int          offset;         /* first visible line */
    int          page_size;      /* terminal height - chrome */
    int          term_width;
    bool         show_line_numbers;
    bool         wrap_lines;
    char         search[128];    /* active search query */
    int          search_hit;     /* current match line (-1 = none) */
    const char  *title;
} tui_pager_t;

void tui_pager_init(tui_pager_t *p, const char **lines, int count,
                     const char *title);
void tui_pager_run(tui_pager_t *p);  /* blocks until user quits */

/* ── Inline Code Block ───────────────────────────────────────────────── */
/* Renders a code block with border, language tag, and line numbers.
 * Similar to Claude Code's fenced code block component. */

void tui_code_block(FILE *out, const char *code, const char *language,
                     int start_line, bool show_line_numbers);

/* ── Breadcrumb Path Display ─────────────────────────────────────────── */
/* Renders file paths with directory separators styled, truncated to fit.
 * Clickable via OSC 8 if supported. */

void tui_breadcrumb(FILE *out, const char *path, int max_width);

#endif
