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
                            const char *result_preview, double elapsed_ms);

/* ── Batch Spinner (multi-tool) ───────────────────────────────────────── */

#define TUI_BATCH_MAX 32

typedef struct {
    char     name[64];
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

/* ── Live Status Bar ──────────────────────────────────────────────────── */

typedef struct {
    pthread_mutex_t mutex;
    bool     enabled;
    bool     visible;
    bool     show_clock;
    char     model[64];
    int      input_tokens;
    int      output_tokens;
    double   cost;
    int      turn;
    int      tools_used;
} tui_status_bar_t;

void tui_status_bar_init(tui_status_bar_t *sb, const char *model);
void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok,
                           double cost, int turn, int tools);
void tui_status_bar_enable(tui_status_bar_t *sb);
void tui_status_bar_disable(tui_status_bar_t *sb);
void tui_status_bar_render(tui_status_bar_t *sb);

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
void tui_section_divider(int turn, int tools, double cost, const char *model);

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

typedef struct {
    char   buf[TUI_CADENCE_BUF_SIZE];
    int    len;
    double last_flush;     /* timestamp of last flush */
    double interval;       /* flush interval in seconds (default 0.016) */
    void  *md_renderer;    /* md_renderer_t* to feed into */
} tui_cadence_t;

void tui_cadence_init(tui_cadence_t *c, void *md_renderer);
void tui_cadence_feed(tui_cadence_t *c, const char *text);
void tui_cadence_flush(tui_cadence_t *c);

/* ── F4: Collapsible Thinking ─────────────────────────────────────────── */
typedef struct {
    int    char_count;
    double start_time;
    bool   active;
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

#endif
