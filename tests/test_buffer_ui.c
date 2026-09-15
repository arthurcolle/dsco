/* Standalone with src/buffer_ui.c and src/json_fast.c; no terminal or tools. */
#include "buffer_ui.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char out[4096];
    buffer_ui_format_result("{\"ok\":true,\"buffers\":[]}", true, out, sizeof(out));
    assert(strstr(out, "Buffers (0)") && strstr(out, "/buffer new NAME"));
    assert(strstr(out, "/buffer edit NAME") && strstr(out, "Ctrl-S"));
    buffer_ui_format_result("{\"ok\":true,\"windows\":[{\"id\":1,\"kind\":\"buffer\"}]}", true, out, sizeof(out));
    assert(strstr(out, "Click its text to edit") && strstr(out, "Ctrl-S saves"));
    buffer_ui_format_result("{\"ok\":true,\"buffers\":[{\"name\":\"notes\",\"kind\":\"scratch\",\"bytes\":42,\"closed\":true,\"buffer_id\":\"owned-id\"}]}",
        true, out, sizeof(out));
    assert(strstr(out, "notes (scratch, 42 bytes, closed)") && strstr(out, "ID: owned-id"));
    buffer_ui_format_result("{\"ok\":true,\"buffer\":{\"name\":\"notes\",\"kind\":\"scratch\",\"bytes\":42,\"buffer_id\":\"owned-id\",\"revision\":\"actual-revision\"},\"text\":\"First line\\n雪\",\"base64\":\"do-not-print-encoded-copy\",\"truncated\":true,\"next_offset\":4096}",
        true, out, sizeof(out));
    assert(strstr(out, "First line\n雪") && strstr(out, "actual-revision") && strstr(out, "4096"));
    assert(!strstr(out, "do-not-print-encoded-copy"));
    buffer_ui_format_result("{\"ok\":false,\"error\":\"buffer_failed\",\"detail\":\"DSCO_ALLOW_RUN=0\\npermission denied\"}", false, out, sizeof(out));
    assert(strstr(out, "DSCO_ALLOW_RUN=0\npermission denied"));
    buffer_ui_format_result("{\"ok\":true,\"usage\":\"dsco buffer ACTION [JSON]\",\"examples\":[\"dsco buffer new notes\"]}", true, out, sizeof(out));
    assert(strstr(out, "/buffer new notes"));
    buffer_ui_format_result("{\"ok\":true,\"view_id\":\"view-owned\"}", true, out, sizeof(out));
    assert(strstr(out, "view-owned"));
    buffer_ui_format_result("{\"ok\":true,\"workspace\":\"main\",\"buffer\":{\"name\":\"notes\",\"revision\":\"current-revision\"},\"views\":[{\"surface_id\":\"owned-surface\",\"buffer_id\":\"owned-buffer\",\"mode\":\"edit\",\"columns\":80,\"rows\":24,\"focused\":true}]}", true, out, sizeof(out));
    assert(strstr(out, "current-revision") && strstr(out, "owned-surface") && strstr(out, "owned-buffer"));
    assert(strstr(out, "edit · 80x24 · focused") && strstr(out, "/buffer focus"));
    buffer_ui_format_result(NULL, false, out, sizeof(out));
    assert(strstr(out, "failed"));
    char large[2048];
    strcpy(large, "{\"ok\":false,\"detail\":\"");
    for (int i = 0; i < 400; i++) strcat(large, "雪");
    strcat(large, "\"}");
    for (size_t cap = 1; cap < 257; cap++) {
        memset(out, 'X', sizeof(out));
        buffer_ui_format_result(large, false, out, cap);
        assert(strlen(out) < cap && out[cap] == 'X');
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); assert(doc);
        yyjson_mut_doc_set_root(doc, yyjson_mut_str(doc, out));
        char *encoded = yyjson_mut_write(doc, 0, NULL);
        assert(encoded); /* Reject an incomplete UTF-8 prefix. */
        free(encoded); yyjson_mut_doc_free(doc);
        if (cap > 128) assert(strstr(out, "Preview truncated"));
    }
    buffer_ui_format_result(large, false, NULL, 0);
    puts("buffer UI: list/read/help/error/view and 256 UTF-8 preview bounds passed");
    return 0;
}
