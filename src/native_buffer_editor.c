#include "native_buffer_editor.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;
    size_t len;
    size_t cursor;
    size_t anchor;
} editor_state_t;

struct native_buffer_editor_history {
    editor_state_t undo[NATIVE_BUFFER_EDITOR_HISTORY_LIMIT];
    editor_state_t redo[NATIVE_BUFFER_EDITOR_HISTORY_LIMIT];
    unsigned undo_count;
    unsigned redo_count;
    size_t bytes;
};

static bool continuation(unsigned char c) { return (c & 0xc0U) == 0x80U; }

static size_t decode(const unsigned char *p, size_t available) {
    if (!p || !available) return 0;
    unsigned char c = p[0];
    if (c < 0x80U) return 1;
    size_t width = c < 0xe0U ? 2 : c < 0xf0U ? 3 : c < 0xf8U ? 4 : 0;
    if (!width || available < width) return 0;
    if ((width == 2 && c < 0xc2U) || (width == 3 && c < 0xe0U) ||
        (width == 4 && c < 0xf0U)) return 0;
    for (size_t i = 1; i < width; ++i)
        if (!continuation(p[i])) return 0;
    if (width == 3 && c == 0xe0U && p[1] < 0xa0U) return 0;
    if (width == 3 && c == 0xedU && p[1] >= 0xa0U) return 0;
    if (width == 4 && c == 0xf0U && p[1] < 0x90U) return 0;
    if (width == 4 && c == 0xf4U && p[1] > 0x8fU) return 0;
    if (width == 4 && c > 0xf4U) return 0;
    return width;
}

static bool valid_span(const char *bytes, size_t len) {
    size_t at = 0;
    while (at < len) {
        size_t width = decode((const unsigned char *)bytes + at, len - at);
        if (!width || bytes[at] == '\0') return false;
        at += width;
    }
    return true;
}

static void state_free(editor_state_t *state, native_buffer_editor_history_t *history) {
    if (!state || !state->text) return;
    free(state->text);
    if (history && history->bytes >= state->len + 1) history->bytes -= state->len + 1;
    state->text = NULL;
    state->len = 0;
    state->cursor = 0;
}

static void stack_drop_oldest(editor_state_t *stack, unsigned *count,
                              native_buffer_editor_history_t *history) {
    if (!stack || !count || !*count) return;
    state_free(&stack[0], history);
    if (*count > 1)
        memmove(stack, stack + 1, (size_t)(*count - 1) * sizeof(stack[0]));
    memset(&stack[--*count], 0, sizeof(stack[0]));
}

static void history_clear_stack(editor_state_t *stack, unsigned *count,
                                native_buffer_editor_history_t *history) {
    if (!stack || !count) return;
    while (*count) stack_drop_oldest(stack, count, history);
}

static void history_destroy(native_buffer_editor_history_t *history) {
    if (!history) return;
    history_clear_stack(history->undo, &history->undo_count, history);
    history_clear_stack(history->redo, &history->redo_count, history);
    free(history);
}

static native_buffer_editor_history_t *history_get(native_buffer_editor_t *editor) {
    if (!editor) return NULL;
    /* Owners must be zero-initialized before their first reset. */
    if (editor->history_magic != NATIVE_BUFFER_EDITOR_HISTORY_MAGIC) {
        editor->history = NULL;
        editor->history_magic = NATIVE_BUFFER_EDITOR_HISTORY_MAGIC;
    }
    if (editor->history) return editor->history;
    editor->history = calloc(1, sizeof(*editor->history));
    return editor->history;
}

static bool capture_state(const native_buffer_editor_t *editor, const char *text,
                          editor_state_t *out) {
    if (!editor || !text || !out) return false;
    size_t len = strlen(text);
    if (len >= NATIVE_BUFFER_EDITOR_HISTORY_BYTES ||
        !native_buffer_editor_cursor_valid(editor, text)) return false;
    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, text, len + 1);
    out->text = copy;
    out->len = len;
    out->cursor = editor->cursor;
    out->anchor = editor->anchor;
    return true;
}

static void clear_redo(native_buffer_editor_history_t *history) {
    if (!history) return;
    history_clear_stack(history->redo, &history->redo_count, history);
}

static bool make_room(native_buffer_editor_history_t *history, bool undo_stack,
                      size_t needed) {
    if (!history || needed > NATIVE_BUFFER_EDITOR_HISTORY_BYTES) return false;
    while (history->bytes + needed > NATIVE_BUFFER_EDITOR_HISTORY_BYTES) {
        if (undo_stack) {
            if (history->undo_count) stack_drop_oldest(history->undo, &history->undo_count, history);
            else if (history->redo_count) stack_drop_oldest(history->redo, &history->redo_count, history);
            else return false;
        } else if (history->redo_count) {
            stack_drop_oldest(history->redo, &history->redo_count, history);
        } else if (history->undo_count) {
            stack_drop_oldest(history->undo, &history->undo_count, history);
        } else {
            return false;
        }
    }
    return true;
}

static void stack_push_owned(editor_state_t *stack, unsigned *count,
                             editor_state_t state, native_buffer_editor_history_t *history) {
    if (*count == NATIVE_BUFFER_EDITOR_HISTORY_LIMIT)
        stack_drop_oldest(stack, count, history);
    stack[*count] = state;
    (*count)++;
    history->bytes += state.len + 1;
}

/* Prepare an edit before changing caller-owned text. A failed allocation or
 * oversized snapshot therefore leaves both the document and its history intact. */
static bool record_edit(native_buffer_editor_t *editor, const char *text) {
    native_buffer_editor_history_t *history = history_get(editor);
    if (!history) return false;
    editor_state_t before = {0};
    if (!capture_state(editor, text, &before)) return false;
    clear_redo(history);
    if (!make_room(history, true, before.len + 1)) {
        state_free(&before, history);
        return false;
    }
    stack_push_owned(history->undo, &history->undo_count, before, history);
    return true;
}

static bool offset_valid(const char *text, size_t len, size_t offset) {
    return offset <= len && (!offset || !continuation((unsigned char)text[offset]));
}

static bool state_valid(const editor_state_t *state, size_t cap) {
    return state && state->text && state->len < cap && valid_span(state->text, state->len) &&
           offset_valid(state->text, state->len, state->cursor) &&
           offset_valid(state->text, state->len, state->anchor);
}

static bool restore_state(native_buffer_editor_t *editor, char *text, size_t cap,
                          const editor_state_t *state) {
    if (!editor || !text || !state_valid(state, cap)) return false;
    memcpy(text, state->text, state->len + 1);
    editor->cursor = state->cursor;
    editor->anchor = state->anchor;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

static bool history_step(native_buffer_editor_t *editor, char *text, size_t cap,
                         bool redo) {
    if (!editor || !text || !editor->history || editor->history_magic != NATIVE_BUFFER_EDITOR_HISTORY_MAGIC)
        return false;
    if (strnlen(text, cap) >= cap) return false;
    native_buffer_editor_history_t *history = editor->history;
    editor_state_t *source_stack = redo ? history->redo : history->undo;
    unsigned *source_count = redo ? &history->redo_count : &history->undo_count;
    editor_state_t *target_stack = redo ? history->undo : history->redo;
    unsigned *target_count = redo ? &history->undo_count : &history->redo_count;
    if (!*source_count) return false;

    editor_state_t current = {0};
    if (!capture_state(editor, text, &current)) return false;
    editor_state_t target = source_stack[*source_count - 1];
    if (!state_valid(&target, cap)) {
        state_free(&current, history);
        return false;
    }

    size_t target_bytes = target.len + 1;
    (*source_count)--;
    memset(&source_stack[*source_count], 0, sizeof(source_stack[0]));
    if (history->bytes >= target_bytes) history->bytes -= target_bytes;
    if (!make_room(history, redo, current.len + 1)) {
        source_stack[*source_count] = target;
        (*source_count)++;
        history->bytes += target_bytes;
        state_free(&current, history);
        return false;
    }
    if (!restore_state(editor, text, cap, &target)) {
        source_stack[*source_count] = target;
        (*source_count)++;
        history->bytes += target_bytes;
        state_free(&current, history);
        return false;
    }
    free(target.text);
    if (*target_count == NATIVE_BUFFER_EDITOR_HISTORY_LIMIT)
        stack_drop_oldest(target_stack, target_count, history);
    stack_push_owned(target_stack, target_count, current, history);
    return true;
}

void native_buffer_editor_dispose(native_buffer_editor_t *editor) {
    if (!editor) return;
    if (editor->history_magic == NATIVE_BUFFER_EDITOR_HISTORY_MAGIC)
        history_destroy(editor->history);
    editor->history = NULL;
    editor->history_magic = NATIVE_BUFFER_EDITOR_HISTORY_MAGIC;
    editor->cursor = 0;
    editor->anchor = 0;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
}

void native_buffer_editor_reset(native_buffer_editor_t *editor, size_t text_len) {
    if (!editor) return;
    native_buffer_editor_dispose(editor);
    editor->cursor = text_len;
    editor->anchor = text_len;
}

bool native_buffer_editor_cursor_valid(const native_buffer_editor_t *editor, const char *text) {
    if (!editor || !text) return false;
    size_t len = strlen(text);
    return valid_span(text, len) && offset_valid(text, len, editor->cursor) &&
           offset_valid(text, len, editor->anchor);
}

static bool selection_bounds(const native_buffer_editor_t *editor, const char *text,
                             size_t *start, size_t *end) {
    if (!editor || !text || !start || !end || !native_buffer_editor_cursor_valid(editor, text))
        return false;
    if (editor->anchor < editor->cursor) {
        *start = editor->anchor;
        *end = editor->cursor;
    } else {
        *start = editor->cursor;
        *end = editor->anchor;
    }
    return true;
}

bool native_buffer_editor_get_selected_range(const native_buffer_editor_t *editor,
                                             const char *text, size_t *start, size_t *end) {
    if (!selection_bounds(editor, text, start, end)) return false;
    return *start != *end;
}

void native_buffer_editor_clear_selection(native_buffer_editor_t *editor) {
    if (editor) editor->anchor = editor->cursor;
}

bool native_buffer_editor_set_caret(native_buffer_editor_t *editor, const char *text,
                                    size_t cursor) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t len = strlen(text);
    if (!offset_valid(text, len, cursor)) return false;
    bool changed = editor->cursor != cursor || editor->anchor != cursor;
    editor->cursor = cursor;
    editor->anchor = cursor;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return changed;
}

bool native_buffer_editor_extend_caret(native_buffer_editor_t *editor, const char *text,
                                       size_t cursor) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t len = strlen(text);
    if (!offset_valid(text, len, cursor)) return false;
    bool changed = editor->cursor != cursor;
    editor->cursor = cursor;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return changed;
}

bool native_buffer_editor_select_all(native_buffer_editor_t *editor, const char *text) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t len = strlen(text);
    bool changed = editor->anchor != 0 || editor->cursor != len;
    editor->anchor = 0;
    editor->cursor = len;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return changed;
}

bool native_buffer_editor_select_line(native_buffer_editor_t *editor, const char *text) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t len = strlen(text);
    size_t start = editor->cursor;
    while (start > 0 && text[start - 1] != '\n') --start;
    size_t end = editor->cursor;
    while (end < len && text[end] != '\n') ++end;
    bool changed = editor->anchor != start || editor->cursor != end;
    editor->anchor = start;
    editor->cursor = end;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return changed;
}

bool native_buffer_editor_delete_selection(native_buffer_editor_t *editor, char *text) {
    if (!editor || !text) return false;
    size_t start = 0, end = 0;
    if (!selection_bounds(editor, text, &start, &end) || start == end) return false;
    size_t old_len = strlen(text);
    if (!record_edit(editor, text)) return false;
    memmove(text + start, text + end, old_len - end + 1);
    editor->cursor = start;
    editor->anchor = start;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

bool native_buffer_editor_insert(native_buffer_editor_t *editor, char *text, size_t cap,
                                 const char *bytes, size_t len) {
    if (!editor || !text || !bytes || !cap || !len) return false;
    size_t old_len = strnlen(text, cap);
    size_t start = 0, end = 0;
    if (old_len >= cap || !selection_bounds(editor, text, &start, &end) ||
        !valid_span(bytes, len)) return false;
    size_t remaining = old_len - (end - start);
    if (len > cap - 1 - remaining || !record_edit(editor, text)) return false;
    memmove(text + start + len, text + end, old_len - end + 1);
    memmove(text + start, bytes, len);
    editor->cursor = start + len;
    editor->anchor = editor->cursor;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

bool native_buffer_editor_feed(native_buffer_editor_t *editor, char *text, size_t cap,
                               unsigned char byte) {
    if (!editor || !text) return false;
    if (!editor->pending_len && byte < 0x80U) {
        if (byte == '\0') return false;
        return native_buffer_editor_insert(editor, text, cap, (const char *)&byte, 1);
    }
    if (!editor->pending_len) {
        if (byte < 0xc2U || byte > 0xf4U) return false;
        editor->pending[0] = byte;
        editor->pending_len = 1;
        return true;
    }
    if (!continuation(byte) || editor->pending_len >= sizeof(editor->pending)) {
        editor->pending_len = 0;
        memset(editor->pending, 0, sizeof(editor->pending));
        return false;
    }
    editor->pending[editor->pending_len++] = byte;
    size_t expected = editor->pending[0] < 0xe0U ? 2 : editor->pending[0] < 0xf0U ? 3 : 4;
    if (editor->pending_len < expected) return true;
    bool ok = decode(editor->pending, editor->pending_len) == expected &&
              native_buffer_editor_insert(editor, text, cap, (const char *)editor->pending, expected);
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return ok;
}

bool native_buffer_editor_backspace(native_buffer_editor_t *editor, char *text) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    if (editor->pending_len) {
        editor->pending_len = 0;
        memset(editor->pending, 0, sizeof(editor->pending));
        return true;
    }
    size_t start = 0, end = 0;
    if (!selection_bounds(editor, text, &start, &end)) return false;
    if (start != end) return native_buffer_editor_delete_selection(editor, text);
    if (!editor->cursor) return false;
    if (!record_edit(editor, text)) return false;
    size_t at = editor->cursor - 1;
    while (at > 0 && continuation((unsigned char)text[at])) --at;
    memmove(text + at, text + editor->cursor, strlen(text) - editor->cursor + 1);
    editor->cursor = at;
    editor->anchor = at;
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

bool native_buffer_editor_left(native_buffer_editor_t *editor, const char *text) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t start = 0, end = 0;
    if (!selection_bounds(editor, text, &start, &end)) return false;
    if (start != end) {
        editor->cursor = editor->anchor = start;
    } else if (editor->cursor) {
        --editor->cursor;
        while (editor->cursor > 0 && continuation((unsigned char)text[editor->cursor])) --editor->cursor;
        editor->anchor = editor->cursor;
    } else {
        return false;
    }
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

bool native_buffer_editor_right(native_buffer_editor_t *editor, const char *text) {
    if (!editor || !text || !native_buffer_editor_cursor_valid(editor, text)) return false;
    size_t start = 0, end = 0;
    if (!selection_bounds(editor, text, &start, &end)) return false;
    size_t len = strlen(text);
    if (start != end) {
        editor->cursor = editor->anchor = end;
    } else {
        if (editor->cursor >= len) return false;
        size_t width = decode((const unsigned char *)text + editor->cursor, len - editor->cursor);
        if (!width) return false;
        editor->cursor += width;
        editor->anchor = editor->cursor;
    }
    editor->pending_len = 0;
    memset(editor->pending, 0, sizeof(editor->pending));
    return true;
}

bool native_buffer_editor_undo(native_buffer_editor_t *editor, char *text, size_t cap) {
    return history_step(editor, text, cap, false);
}

bool native_buffer_editor_redo(native_buffer_editor_t *editor, char *text, size_t cap) {
    return history_step(editor, text, cap, true);
}
