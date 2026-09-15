#ifndef DSCO_NATIVE_BUFFER_EDITOR_H
#define DSCO_NATIVE_BUFFER_EDITOR_H

#include <stdbool.h>
#include <stddef.h>

/* Small, bounded UTF-8 editing core used by retained native buffer panels.
 * The caller owns the storage and decides which control bytes are commands.
 * A partial terminal codepoint is retained in `pending` until the next byte.
 * History is bounded both by entries and by snapshot bytes.
 * Initialize with {0} before first reset; dispose before discarding an owner.
 * Copies (including compositor snapshots) borrow history and must not dispose it. */
#define NATIVE_BUFFER_EDITOR_HISTORY_LIMIT 64U
#define NATIVE_BUFFER_EDITOR_HISTORY_BYTES (512U * 1024U)
#define NATIVE_BUFFER_EDITOR_HISTORY_MAGIC 0x4e424548U

typedef struct native_buffer_editor_history native_buffer_editor_history_t;

typedef struct {
    /* All offsets are byte offsets at UTF-8 codepoint boundaries. */
    size_t cursor;
    size_t anchor;
    unsigned char pending[4];
    unsigned pending_len;
    unsigned history_magic;
    native_buffer_editor_history_t *history;
} native_buffer_editor_t;

void native_buffer_editor_reset(native_buffer_editor_t *editor, size_t text_len);
void native_buffer_editor_dispose(native_buffer_editor_t *editor);
/* Returns [start, end) in bytes. On a valid editor with no selection, writes
 * start == end == cursor and returns false. */
bool native_buffer_editor_get_selected_range(const native_buffer_editor_t *editor,
                                             const char *text, size_t *start, size_t *end);
void native_buffer_editor_clear_selection(native_buffer_editor_t *editor);
bool native_buffer_editor_set_caret(native_buffer_editor_t *editor, const char *text,
                                    size_t cursor);
bool native_buffer_editor_extend_caret(native_buffer_editor_t *editor, const char *text,
                                       size_t cursor);
bool native_buffer_editor_select_all(native_buffer_editor_t *editor, const char *text);
/* Selects the current line's content, excluding its '\n' delimiter. */
bool native_buffer_editor_select_line(native_buffer_editor_t *editor, const char *text);
bool native_buffer_editor_delete_selection(native_buffer_editor_t *editor, char *text);

bool native_buffer_editor_insert(native_buffer_editor_t *editor, char *text, size_t cap,
                                 const char *bytes, size_t len);
bool native_buffer_editor_feed(native_buffer_editor_t *editor, char *text, size_t cap,
                               unsigned char byte);
bool native_buffer_editor_backspace(native_buffer_editor_t *editor, char *text);
bool native_buffer_editor_left(native_buffer_editor_t *editor, const char *text);
bool native_buffer_editor_right(native_buffer_editor_t *editor, const char *text);
bool native_buffer_editor_undo(native_buffer_editor_t *editor, char *text, size_t cap);
bool native_buffer_editor_redo(native_buffer_editor_t *editor, char *text, size_t cap);
bool native_buffer_editor_cursor_valid(const native_buffer_editor_t *editor, const char *text);

#endif
