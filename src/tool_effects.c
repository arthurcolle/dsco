#include "tool_effects.h"
#include "../vendor/yyjson.h"
#include <string.h>

/* Registry metadata stays in tools.c; this module owns invocation semantics. */
bool tools_meta_is_read_only(const char *name, bool *found);

bool tool_http_request_is_read_only(const char *input_json) {
    if (!input_json)
        return false;
    yyjson_doc *doc = yyjson_read(input_json, strlen(input_json), 0);
    if (!doc)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool read_only = yyjson_is_obj(root);
    unsigned methods = 0;
    if (read_only) {
        yyjson_obj_iter iter = yyjson_obj_iter_with(root);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            /* Payload-bearing GETs may have endpoint-specific side effects. */
            if (yyjson_equals_str(key, "body"))
                read_only = false;
            if (!yyjson_equals_str(key, "method"))
                continue;
            yyjson_val *method = yyjson_obj_iter_get_val(key);
            if (++methods > 1 ||
                !(yyjson_equals_str(method, "GET") || yyjson_equals_str(method, "HEAD")))
                read_only = false;
        }
    }
    yyjson_doc_free(doc);
    return read_only;
}

bool tools_call_is_read_only(const char *name, const char *input_json) {
    if (name && strcmp(name, "http_request") == 0)
        return tool_http_request_is_read_only(input_json);
    bool found = false;
    bool read_only = tools_meta_is_read_only(name, &found);
    return found && read_only;
}
