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

/* Full-session Kitty workspace. State and content are rendered as native RGB
 * surfaces; an interruptible compositor animates active phases, streaming
 * cursors, and eased transitions at an oh-my-pi-compatible 80 ms cadence. */
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

#endif /* DSCO_PIXEL_TUI_H */
