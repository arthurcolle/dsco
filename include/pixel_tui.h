#ifndef DSCO_PIXEL_TUI_H
#define DSCO_PIXEL_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Native-pixel terminal surfaces. The current transport is Kitty's graphics
 * protocol; callers keep their existing ANSI/text fallback for other TTYs. */
bool pixel_tui_available(FILE *out);

typedef enum {
    PIXEL_TUI_IDLE = 0,
    PIXEL_TUI_REASONING,
    PIXEL_TUI_EXECUTING,
    PIXEL_TUI_RESPONDING,
} pixel_tui_state_t;

/* Shared command shape for native help surfaces. Callers should pass their
 * canonical registry rather than maintaining a second pixel-only list. */
typedef struct {
    const char *command;
    const char *description;
} pixel_tui_command_t;

typedef enum {
    PIXEL_TUI_MENU_NONE = 0,
    PIXEL_TUI_MENU_COMMANDS,
    PIXEL_TUI_MENU_IMAGES,
} pixel_tui_menu_kind_t;

typedef struct {
    const char *label;
    const char *detail;
    bool disabled;
} pixel_tui_menu_item_t;

typedef enum {
    PIXEL_TUI_NOTICE_INFO = 0,
    PIXEL_TUI_NOTICE_SUCCESS,
    PIXEL_TUI_NOTICE_WARNING,
    PIXEL_TUI_NOTICE_ERROR,
    PIXEL_TUI_NOTICE_ACTIVITY,
} pixel_tui_notice_level_t;

typedef enum {
    PIXEL_TUI_MODAL_INFO = 0,
    PIXEL_TUI_MODAL_PERMISSION,
    PIXEL_TUI_MODAL_QUESTION,
    PIXEL_TUI_MODAL_MENU,
} pixel_tui_modal_kind_t;

/* Full-session Kitty workspace. State and content are rendered as native RGB
 * surfaces; an interruptible compositor coalesces streaming deltas at 30 Hz
 * and animates active phases, cursors, and eased transitions. */
bool pixel_tui_session_begin(FILE *out, const char *model);
bool pixel_tui_session_active(void);
void pixel_tui_session_set_state(FILE *out, pixel_tui_state_t state);
/* Native workspace content. In Kitty mode these are the authoritative
 * transcript and composer surfaces; ANSI rendering remains the fallback. */
void pixel_tui_session_begin_message(FILE *out, const char *role, const char *detail);
void pixel_tui_session_append_text(FILE *out, const char *text);
void pixel_tui_session_end_message(FILE *out);
void pixel_tui_session_add_message(FILE *out, const char *role, const char *text);
void pixel_tui_session_show_commands(FILE *out, const pixel_tui_command_t *commands,
                                     int count);
void pixel_tui_session_set_input(FILE *out, const char *text, size_t cursor, bool active);
/* Atomically update input plus its live command/image picker. This preserves
 * the established cell composer's editing semantics while the native backend
 * owns presentation. */
void pixel_tui_session_set_composer(FILE *out, const char *text, size_t cursor,
                                    bool active, pixel_tui_menu_kind_t menu_kind,
                                    const pixel_tui_menu_item_t *items, int item_count,
                                    int selected);
void pixel_tui_session_set_model(FILE *out, const char *model, const char *slot_name);
void pixel_tui_session_set_usage(FILE *out, int input_tokens, int output_tokens,
                                 double cost_usd, int turn, int tools_used);
void pixel_tui_session_set_budget(FILE *out, double limit_usd, double burn_rate,
                                  double percent, double runway_seconds);
void pixel_tui_session_set_clock(FILE *out, bool show_clock);

/* Pixel theme selection (see px_theme.h for the registry). Setting or
 * cycling repaints the live session; both are safe with no session active
 * (they still switch the process-wide theme for the next surface). */
const char *pixel_tui_theme_name(void);
bool pixel_tui_session_set_theme(FILE *out, const char *name);
const char *pixel_tui_session_cycle_theme(FILE *out, int direction);
void pixel_tui_session_notify(FILE *out, pixel_tui_notice_level_t level,
                              const char *text);
void pixel_tui_session_clear_notifications(FILE *out);
void pixel_tui_session_show_modal(FILE *out, pixel_tui_modal_kind_t kind,
                                  const char *title, const char *subtitle,
                                  const pixel_tui_menu_item_t *items, int item_count,
                                  int selected, const char *footer);
void pixel_tui_session_clear_modal(FILE *out);
void pixel_tui_session_scroll(FILE *out, int lines);
void pixel_tui_session_set_turn(FILE *out, int turn);
void pixel_tui_session_set_runtime_metrics(FILE *out, double cost_usd,
                                           double context_percent);
void pixel_tui_session_set_queue_depth(FILE *out, int depth, int capacity);
/* Structured live-operation telemetry. These hooks are fed by the governed
 * tool gate and swarm lifecycle, so native motion represents real work rather
 * than parsing transcript text. */
uint64_t pixel_tui_session_tool_begin(FILE *out, const char *name, const char *input_json);
void pixel_tui_session_tool_end(FILE *out, uint64_t operation_id, const char *name,
                                bool ok, double elapsed_ms);
void pixel_tui_session_swarm_update(FILE *out, int child_id, const char *status,
                                    const char *task, const char *model,
                                    size_t output_bytes, double cost_usd);
/* Track reasoning activity without displaying private chain-of-thought text. */
void pixel_tui_session_note_thinking(FILE *out, const char *delta);
/* Send terminal-mode controls through the preserved Kitty TTY while ordinary
 * stdout/stderr are suppressed behind the native compositor. */
void pixel_tui_session_terminal_control(FILE *out, const char *sequence);
/* Temporarily hand the real TTY to an interactive tool, then rebuild the
 * native workspace without losing transcript/composer state. */
void pixel_tui_session_suspend_terminal(FILE *out);
void pixel_tui_session_resume_terminal(FILE *out);
bool pixel_tui_session_terminal_suspended(void);
/* Re-query cell + pixel geometry; changed dimensions regenerate and atomically
 * swap the active surface, preserving the current agent phase. */
void pixel_tui_session_refresh(FILE *out);
void pixel_tui_session_end(FILE *out);

typedef enum {
    PIXEL_PLAN_VIEW_TREE = 0,
    PIXEL_PLAN_VIEW_ACTIONS,
} pixel_plan_view_t;

/* Draw a plan as a native RGB surface at the current cursor. The tree view
 * shows goal/step/atom hierarchy; the actions view adds dependency edges,
 * ready-frontier state, policy gates, and execution lanes. Returns the number
 * of terminal rows occupied, or 0 when unavailable/error. */
int pixel_tui_render_plan(FILE *out, int plan_id);
int pixel_tui_render_plan_view(FILE *out, int plan_id, pixel_plan_view_t view);

/* Deterministic artifact path used by visual tests and design iteration. */
bool pixel_tui_write_plan_ppm(const char *path, int plan_id, int width);
bool pixel_tui_write_plan_view_ppm(const char *path, int plan_id, int width,
                                   pixel_plan_view_t view);

/* Headless artifact path for the complete live-session surface. This is the
 * cross-transport regression seam: Kitty and future native windows consume
 * the same RGB canvas rendered here. */
bool pixel_tui_write_session_ppm(const char *path, int width, int height,
                                 const char *model, pixel_tui_state_t state);

/* Deterministic populated-session seam for parity corpora and density tests.
 * It uses the same renderer as a live session, but never opens a TTY or starts
 * compositor threads. */
typedef struct {
    const char *role;
    const char *detail;
    const char *text;
} pixel_tui_fixture_message_t;

/* Live-operation telemetry for fixtures: mirrors what the governed tool gate
 * feeds pixel_tui_session_tool_begin/_end, so running/resolved operations
 * (exec ticker, masthead activity ring) render deterministically headless. */
typedef struct {
    const char *name;
    const char *preview;
    int status;              /* 0 running, 1 done, 2 error */
    double elapsed_ms;       /* for resolved ops */
    double started_offset_s; /* how long ago the op began */
} pixel_tui_fixture_tool_t;

typedef struct {
    const char *model;
    const char *slot_name;
    pixel_tui_state_t state;
    const pixel_tui_fixture_message_t *messages;
    int message_count;
    const char *input;
    size_t input_cursor;
    bool input_active;
    int turn;
    int input_tokens;
    int output_tokens;
    int tools_used;
    double cost_usd;
    double context_percent;
    const pixel_tui_fixture_tool_t *tools;
    int tool_count;
} pixel_tui_fixture_t;

typedef struct {
    int logical_width;
    int logical_height;
    int transcript_width;
    int transcript_height;
    int wrap_columns;
    int line_capacity;
    int wrapped_lines;
    int visible_lines;
    int visible_messages;
    size_t source_chars;
    size_t visible_chars;
} pixel_tui_density_metrics_t;

bool pixel_tui_write_fixture_ppm(const char *path, int width, int height,
                                 const pixel_tui_fixture_t *fixture,
                                 pixel_tui_density_metrics_t *metrics);

/* Headless cadence probe: evaluates the compositor thread's real wait/repaint
 * decision for `running_tools` live operations on a scratch session. Returns
 * the chosen sleep in ms (full rate for motion, ~66ms tool tick, 250ms
 * reduced-motion transient, 500ms parked), or -1 while a live session owns
 * the compositor. `fast_out` reports animation-rate frames; `transient_out`
 * reports the reduced-motion tick path. */
int pixel_tui_animation_cadence_probe(int running_tools,
                                      bool animation_enabled,
                                      bool *fast_out, bool *transient_out);

/* ── Generative UI ───────────────────────────────────────────────────────
 * Render a declarative native_ui JSON scene (see native_ui_scene_from_json)
 * as a native RGB overlay above the session — or at the cursor outside a
 * session. This is how agent output materializes ad-hoc panels, dashboards,
 * and inspectors without owning pixels. Returns rows occupied, 0 on error. */
int pixel_tui_render_scene_json(FILE *out, const char *scene_json);

/* Headless scene render for tests and design iteration. */
bool pixel_tui_write_scene_ppm(const char *path, const char *scene_json,
                               int width, int height);

/* Governed agent-facing tool: input {"spec": {…}, "ppm_path"?: "…"}.
 * Renders the spec as a native overlay (and/or a PPM artifact). */
bool tool_ui_render(const char *input_json, char *result, size_t result_len);

/* ── Capture stream parser ───────────────────────────────────────────────
 * Turns a raw captured stdout/stderr byte stream into transcript lines,
 * honoring terminal overwrite semantics: erase-line / cursor-motion CSI
 * sequences and bare '\r' mark in-place repaint frames (spinners, progress
 * bars) that a real terminal would overwrite — those never publish. Exposed
 * so the compatibility boundary is regression-testable headlessly. */
typedef struct {
    char line[4096];
    size_t len;
    int state;         /* capture_parse_state_t */
    bool line_repaint; /* current line is an in-place repaint frame */
    bool cr_pending;   /* saw '\r'; next byte decides overwrite vs CRLF */
} pixel_capture_parser_t;

typedef bool (*pixel_capture_publish_fn)(const char *line, size_t len, void *ctx);

void pixel_capture_parser_init(pixel_capture_parser_t *p);
/* Feed bytes; invokes publish for each completed durable line. Returns true
 * if any publish call returned true (content changed). */
bool pixel_capture_parser_feed(pixel_capture_parser_t *p, const char *bytes,
                               size_t n, pixel_capture_publish_fn publish,
                               void *ctx);
/* End-of-stream: publish a trailing unterminated durable line, if any. */
bool pixel_capture_parser_finish(pixel_capture_parser_t *p,
                                 pixel_capture_publish_fn publish, void *ctx);

#endif /* DSCO_PIXEL_TUI_H */
