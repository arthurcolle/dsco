#include "native_buffer_editor.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void assert_range(native_buffer_editor_t *editor, const char *text,
                         size_t expected_start, size_t expected_end, bool active) {
    size_t start = SIZE_MAX, end = SIZE_MAX;
    bool got = native_buffer_editor_get_selected_range(editor, text, &start, &end);
    assert(got == active);
    assert(start == expected_start && end == expected_end);
}

static void test_ranges_and_commands(void) {
    char text[64] = "ab雪cd\nlast";
    native_buffer_editor_t editor = {0};
    native_buffer_editor_reset(&editor, strlen(text));

    assert(native_buffer_editor_set_caret(&editor, text, 2));
    assert_range(&editor, text, 2, 2, false);
    assert(native_buffer_editor_extend_caret(&editor, text, 7));
    assert_range(&editor, text, 2, 7, true);
    assert(native_buffer_editor_extend_caret(&editor, text, 5));
    assert_range(&editor, text, 2, 5, true);
    assert(native_buffer_editor_set_caret(&editor, text, 5));
    assert_range(&editor, text, 5, 5, false);

    /* Byte offsets inside 雪 are not legal caret positions. */
    assert(!native_buffer_editor_set_caret(&editor, text, 3));
    assert(!native_buffer_editor_extend_caret(&editor, text, 4));
    assert_range(&editor, text, 5, 5, false);

    assert(native_buffer_editor_select_all(&editor, text));
    assert_range(&editor, text, 0, strlen(text), true);
    native_buffer_editor_clear_selection(&editor);
    assert_range(&editor, text, strlen(text), strlen(text), false);

    assert(native_buffer_editor_set_caret(&editor, text, 6));
    assert(native_buffer_editor_select_line(&editor, text));
    assert_range(&editor, text, 0, 7, true); /* excludes the newline */
    assert(native_buffer_editor_set_caret(&editor, text, 9));
    assert(native_buffer_editor_select_line(&editor, text));
    assert_range(&editor, text, 8, strlen(text), true);

    native_buffer_editor_dispose(&editor);
}

static void test_replace_and_history(void) {
    char text[64] = "hello";
    native_buffer_editor_t editor = {0};
    native_buffer_editor_reset(&editor, strlen(text));
    assert(native_buffer_editor_set_caret(&editor, text, 1));
    assert(native_buffer_editor_extend_caret(&editor, text, 4));
    assert_range(&editor, text, 1, 4, true);

    assert(native_buffer_editor_insert(&editor, text, sizeof(text), "雪", 3));
    assert(!strcmp(text, "h雪o"));
    assert(editor.cursor == 4 && editor.anchor == 4);
    assert_range(&editor, text, 4, 4, false);

    /* The edit snapshot restores both text and the original selection. */
    assert(native_buffer_editor_undo(&editor, text, sizeof(text)));
    assert(!strcmp(text, "hello"));
    assert_range(&editor, text, 1, 4, true);
    assert(native_buffer_editor_redo(&editor, text, sizeof(text)));
    assert(!strcmp(text, "h雪o"));
    assert_range(&editor, text, 4, 4, false);

    native_buffer_editor_dispose(&editor);
}

static void test_delete_and_backspace(void) {
    char text[64] = "ab雪cd";
    native_buffer_editor_t editor = {0};
    native_buffer_editor_reset(&editor, strlen(text));
    assert(native_buffer_editor_set_caret(&editor, text, 2));
    assert(native_buffer_editor_extend_caret(&editor, text, 5));
    assert(native_buffer_editor_delete_selection(&editor, text));
    assert(!strcmp(text, "abcd") && editor.cursor == 2 && editor.anchor == 2);
    assert_range(&editor, text, 2, 2, false);
    assert(!native_buffer_editor_delete_selection(&editor, text));
    assert(native_buffer_editor_undo(&editor, text, sizeof(text)));
    assert(!strcmp(text, "ab雪cd"));
    assert_range(&editor, text, 2, 5, true);

    native_buffer_editor_reset(&editor, strlen(text));
    assert(native_buffer_editor_set_caret(&editor, text, 5));
    assert(native_buffer_editor_extend_caret(&editor, text, 2));
    assert(native_buffer_editor_backspace(&editor, text));
    assert(!strcmp(text, "abcd") && editor.cursor == 2 && editor.anchor == 2);
    assert(native_buffer_editor_undo(&editor, text, sizeof(text)));
    assert(!strcmp(text, "ab雪cd"));
    assert_range(&editor, text, 2, 5, true);

    native_buffer_editor_dispose(&editor);
}

static void test_invalid_edits_and_utf8(void) {
    char text[16] = "🙂x";
    native_buffer_editor_t editor = {0};
    native_buffer_editor_reset(&editor, strlen(text));
    assert(native_buffer_editor_set_caret(&editor, text, 4));
    assert(!native_buffer_editor_set_caret(&editor, text, 1));
    assert(!native_buffer_editor_extend_caret(&editor, text, 2));
    assert_range(&editor, text, 4, 4, false);

    assert(!native_buffer_editor_insert(&editor, text, sizeof(text), "\xed\xa0\x80", 3));
    assert(!strcmp(text, "🙂x") && editor.cursor == 4 && editor.anchor == 4);

    char bounded[5] = "abcd";
    native_buffer_editor_reset(&editor, strlen(bounded));
    assert(native_buffer_editor_set_caret(&editor, bounded, 1));
    assert(native_buffer_editor_extend_caret(&editor, bounded, 3));
    assert(!native_buffer_editor_insert(&editor, bounded, sizeof(bounded), "雪", 3));
    assert(!strcmp(bounded, "abcd"));
    assert_range(&editor, bounded, 1, 3, true);

    native_buffer_editor_dispose(&editor);
}

int main(void) {
    test_ranges_and_commands();
    test_replace_and_history();
    test_delete_and_backspace();
    test_invalid_edits_and_utf8();
    puts("PASS: native UTF-8 selection ranges, commands, replacement, deletion, history, and rejection");
    return 0;
}
