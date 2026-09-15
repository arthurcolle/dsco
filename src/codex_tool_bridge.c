#include "codex_tool_bridge.h"
#include "../vendor/yyjson.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *codex_tool_bridge_schema(void) {
    return "{\"type\":\"object\",\"additionalProperties\":false,"
           "\"properties\":{\"text\":{\"type\":\"string\"},"
           "\"tool_name\":{\"type\":\"string\"},\"arguments_json\":{\"type\":\"string\"}},"
           "\"required\":[\"text\",\"tool_name\",\"arguments_json\"]}";
}

static bool plain_string(yyjson_val *value) {
    return yyjson_is_str(value) && strlen(yyjson_get_str(value)) == yyjson_get_len(value);
}

bool codex_tool_bridge_parse(const char *answer, const char *request, parsed_response_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    yyjson_doc *doc = answer ? yyjson_read(answer, strlen(answer), 0) : NULL;
    yyjson_doc *req = request ? yyjson_read(request, strlen(request), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *text = yyjson_obj_get(root, "text");
    yyjson_val *name = yyjson_obj_get(root, "tool_name");
    yyjson_val *args = yyjson_obj_get(root, "arguments_json");
    bool valid = yyjson_is_obj(root) && yyjson_obj_size(root) == 3 &&
                 plain_string(text) && plain_string(name) && plain_string(args);
    yyjson_doc *input = NULL;
    if (valid && yyjson_get_len(name)) {
        valid = false;
        yyjson_val *items = yyjson_obj_get(req ? yyjson_doc_get_root(req) : NULL, "tools");
        size_t i, count; yyjson_val *item;
        yyjson_arr_foreach(items, i, count, item) {
            yyjson_val *fn = yyjson_obj_get(item, "function");
            if (yyjson_equals_str(yyjson_obj_get(fn, "name"), yyjson_get_str(name))) {
                valid = true;
                break;
            }
        }
        input = yyjson_read(yyjson_get_str(args), yyjson_get_len(args), 0);
        valid = valid && input && yyjson_is_obj(yyjson_doc_get_root(input));
    } else if (valid) {
        /* A final answer must not smuggle an unexecuted request. */
        valid = yyjson_get_len(text) > 0 && yyjson_equals_str(args, "{}");
    }
    if (valid) {
        bool call = yyjson_get_len(name) > 0;
        bool has_text = yyjson_get_len(text) > 0;
        out->count = (int)has_text + (int)call;
        out->blocks = safe_malloc((size_t)out->count * sizeof(*out->blocks));
        memset(out->blocks, 0, (size_t)out->count * sizeof(*out->blocks));
        if (has_text) {
            out->blocks[0].type = safe_strdup("text");
            out->blocks[0].text = safe_strdup(yyjson_get_str(text));
        }
        if (call) {
            static _Atomic unsigned long sequence;
            char id[64];
            snprintf(id, sizeof(id), "dsco_cli_%lu", atomic_fetch_add(&sequence, 1) + 1);
            content_block_t *block = &out->blocks[has_text ? 1 : 0];
            block->type = safe_strdup("tool_use");
            block->tool_id = safe_strdup(id);
            block->tool_name = safe_strdup(yyjson_get_str(name));
            block->tool_input = safe_strdup(yyjson_get_str(args));
        }
        out->stop_reason = safe_strdup(call ? "tool_use" : "end_turn");
    }
    yyjson_doc_free(input);
    yyjson_doc_free(req);
    yyjson_doc_free(doc);
    return valid;
}
