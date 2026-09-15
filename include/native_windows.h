#ifndef DSCO_NATIVE_WINDOWS_H
#define DSCO_NATIVE_WINDOWS_H
#include "native_ui.h"
#include "native_buffer_editor.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_WINDOWS_MAX 12
#define NATIVE_WINDOW_TEXT_CAP 8192
#define NATIVE_WINDOW_TITLE_CAP 96
#define NATIVE_WINDOW_EVIDENCE_CAP 1024
#define NATIVE_WINDOWS_EVENTS_MAX 64
#define NATIVE_WINDOW_TITLE_HEIGHT 28
#define NATIVE_WINDOW_FOOTER_HEIGHT 34
#define NATIVE_WINDOWS_TOOLBAR_HEIGHT 32

typedef enum { NATIVE_WINDOW_NOTE, NATIVE_WINDOW_BUFFER, NATIVE_WINDOW_WORKFLOW } native_window_kind_t;
typedef enum { NATIVE_WINDOW_READY, NATIVE_WINDOW_RUNNING, NATIVE_WINDOW_BLOCKED, NATIVE_WINDOW_DONE } native_window_status_t;
typedef enum { NATIVE_WINDOW_CONTINUE, NATIVE_WINDOW_REVISE, NATIVE_WINDOW_RETRY, NATIVE_WINDOW_INSPECT } native_window_action_kind_t;
typedef struct {
    uint64_t id, revision;
    native_ui_rect_t rect, restore_rect;
    native_window_kind_t kind;
    native_window_status_t status;
    bool zoomed, sensitive;
    int scroll;
    char title[NATIVE_WINDOW_TITLE_CAP];
    char text[NATIVE_WINDOW_TEXT_CAP];
    char evidence[NATIVE_WINDOW_EVIDENCE_CAP];
    char next_step[NATIVE_WINDOW_EVIDENCE_CAP];
    char buffer_id[37], buffer_revision[65], workspace[64];
    native_buffer_editor_t editor;
    char search_text[256];
    bool searching;
    bool editor_active, dirty, save_in_flight;
    uint64_t edit_generation;
} native_window_t;
typedef struct {
    uint64_t event_id, window_id;
    native_window_action_kind_t kind;
    char title[NATIVE_WINDOW_TITLE_CAP];
    char objective[1024], next_step[NATIVE_WINDOW_EVIDENCE_CAP];
} native_window_action_t;
typedef struct {
    uint64_t revision, focused_id, captured_id;
    bool visible, keyboard_focus;
    native_ui_rect_t work_area;
    int count;
    native_window_t windows[NATIVE_WINDOWS_MAX]; /* back to front */
    char feedback[192];
} native_windows_snapshot_t;

enum {
    NATIVE_WINDOW_KEY_TAB = 256, NATIVE_WINDOW_KEY_ESCAPE,
    NATIVE_WINDOW_KEY_UP, NATIVE_WINDOW_KEY_DOWN, NATIVE_WINDOW_KEY_LEFT,
    NATIVE_WINDOW_KEY_RIGHT, NATIVE_WINDOW_KEY_PAGEUP, NATIVE_WINDOW_KEY_PAGEDOWN,
    NATIVE_WINDOW_KEY_TOGGLE_FOCUS, NATIVE_WINDOW_KEY_BACKSPACE, NATIVE_WINDOW_KEY_SAVE,
    NATIVE_WINDOW_KEY_HOME, NATIVE_WINDOW_KEY_END, NATIVE_WINDOW_KEY_DELETE
};
enum { NATIVE_WINDOW_MOD_SHIFT = 1, NATIVE_WINDOW_MOD_ALT = 2, NATIVE_WINDOW_MOD_CTRL = 4 };

/* Retained, mutex-protected model. No rendering, terminal I/O, buffer I/O or
 * tool execution while holding its lock. IDs are session-local and never reused.
 * Calls into the renderer must follow, never surround, these model operations. */
void native_windows_reset(void);
bool native_windows_clipboard_key(int key);
bool native_windows_reuse_key(int key);
void native_windows_editor_feedback(const char *message);
bool native_windows_replace_region(uint64_t id,uint64_t generation,size_t anchor,size_t cursor,const char *text,size_t len);

void native_windows_set_work_area(native_ui_rect_t rect);
void native_windows_snapshot(native_windows_snapshot_t *out);
bool native_windows_visible(void);
bool native_windows_focused(void);
bool native_windows_sensitive(void);
bool native_windows_command(const char *json, char *result, size_t cap);
/* Validates the public schema without changing state. */
const char *native_windows_validate(const char *json);
bool native_windows_bind_buffer(uint64_t id, const char *title, const char *text,
    const char *buffer_id, const char *revision, const char *workspace, bool sensitive,
    char *result, size_t cap);

typedef struct {
    uint64_t window_id, edit_generation;
    char buffer_id[37], expected_revision[65], workspace[64];
    char content[NATIVE_WINDOW_TEXT_CAP];
} native_window_save_request_t;
/* Take a bounded copy of an explicit Ctrl-S request. The model mutex is
 * released before the governed adapter performs the write. */
uint64_t native_windows_paste_target(void);
bool native_windows_insert_paste(uint64_t id, const char *text, size_t len);
bool native_windows_take_save_request(native_window_save_request_t *out);
/* Reconcile a save result without discarding edits made while the tool ran. */
void native_windows_finish_save(const native_window_save_request_t *request, bool ok,
    const char *revision, const char *error);
/* Adapter entrypoint: calls tools_execute_for_tier("buffer", ...) only after
 * take_save_request has released the retained-window model mutex. */
bool native_windows_save_pending(bool *attempted, char *result, size_t cap);
/* Return the editor caret's local body row/column in fixed compositor cells. */
bool native_window_editor_position(const native_window_t *window, int columns,
    int *row, int *column);
bool native_windows_pointer(int button, int x, int y, bool released);
void native_windows_set_pointer_cell(int width, int height);
bool native_windows_key(int key, unsigned modifiers);
void native_windows_cancel_gesture(void);
/* Only terminal input handlers enqueue actions; this JSON API cannot. Input
 * may also come from terminal automation and never grants extra authority. */
bool native_windows_action_pending(void);
bool native_windows_pop_action(native_window_action_t *out);
void native_windows_format_action(const native_window_action_t *action, char *out, size_t cap);
void native_windows_format_result(const char *json, bool ok, char *out, size_t cap);
/* Shared word wrapping for rendering and exact scroll bounds. Slices borrow
 * valid UTF-8 input; columns are bounded to 1..240. Explicit blank lines stay. */
bool native_window_text_next(const char **cursor, int columns, const char **start, size_t *bytes);
const char *native_window_kind_name(native_window_kind_t kind);
const char *native_window_status_name(native_window_status_t status);
const char *native_window_action_name(native_window_action_kind_t kind);

/* Governed tool adapter. Buffer reads go through tools_execute_for_tier. */
bool tool_native_window(const char *input, char *result, size_t cap);
#define NATIVE_WINDOW_DESCRIPTION \
    "Manage retained draggable windows inside the active DSCO native Kitty graphics compositor. " \
    "Separate from terminal PTYs, tabs and OS windows (use surface/kitty_remote for those). " \
    "Open note/workflow panels; open an editable persistent buffer with action=buffer and buffer_id or name. " \
    "For requested buffer editing use action=buffer, not a note or copied text. The human clicks the text to edit, " \
    "Ctrl-S saves to the canonical buffer with revision-conflict protection, and Esc returns to chat. " \
    "The slash shortcut is /buffer edit NAME. Refresh explicitly after external changes; unsaved edits are retained. " \
    "Move/resize use logical pixel coordinates returned by list. " \
    "Actions list/events are read-only; events returns retained human button receipts and never consumes them. " \
    "Workflow text is the objective; next_step is the proposed action; evidence records observed results. " \
    "status=done requires evidence; status is agent-reported, not independently verified. " \
    "Continue/Revise/Retry/Inspect terminal inputs steer the current task without granting new authority; " \
    "this JSON API cannot forge input receipts. Terminal input may be physical or automated. " \
    "Closing a panel retains its underlying buffer. Up to 12 panels, text up to 8191 UTF-8 bytes. " \
    "Use short human titles, plain language and concrete next steps."
#define NATIVE_WINDOW_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"open\",\"update\",\"buffer\",\"refresh\",\"move\",\"resize\",\"focus\",\"zoom\",\"tile\",\"cascade\",\"close\",\"show\",\"hide\",\"scroll\",\"events\"]}," \
    "\"id\":{\"type\":\"integer\",\"minimum\":1}," \
    "\"title\":{\"type\":\"string\",\"maxLength\":95}," \
    "\"kind\":{\"type\":\"string\",\"enum\":[\"note\",\"workflow\"]}," \
    "\"text\":{\"type\":\"string\",\"maxLength\":8191}," \
    "\"status\":{\"type\":\"string\",\"enum\":[\"ready\",\"running\",\"blocked\",\"done\"]}," \
    "\"evidence\":{\"type\":\"string\",\"maxLength\":1023}," \
    "\"next_step\":{\"type\":\"string\",\"maxLength\":1023}," \
    "\"x\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16384}," \
    "\"y\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16384}," \
    "\"width\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384}," \
    "\"height\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384}," \
    "\"lines\":{\"type\":\"integer\",\"minimum\":-100000,\"maximum\":100000}," \
    "\"since\":{\"type\":\"integer\",\"minimum\":0}," \
    "\"workspace\":{\"type\":\"string\",\"maxLength\":63}," \
    "\"buffer_id\":{\"type\":\"string\",\"maxLength\":36}," \
    "\"name\":{\"type\":\"string\",\"maxLength\":160}" \
    "},\"required\":[\"action\"]}"
#endif
