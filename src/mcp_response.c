#include "mcp_response.h"
#include "../vendor/yyjson.h"
#include <stdlib.h>
#include <string.h>

char *mcp_response_match(const char *json, size_t len, const char *request) {
    yyjson_doc *d = yyjson_read(json, len, 0);
    yyjson_doc *r = request ? yyjson_read(request, strlen(request), 0) : NULL;
    yyjson_val *o = d ? yyjson_doc_get_root(d) : NULL;
    yyjson_val *q = r ? yyjson_doc_get_root(r) : NULL;
    yyjson_val *id = yyjson_obj_get(o, "id"), *wanted = yyjson_obj_get(q, "id");
    bool result = yyjson_obj_get(o, "result") != NULL;
    bool error = yyjson_obj_get(o, "error") != NULL;
    char *out = NULL;
    if (yyjson_is_obj(o) && yyjson_equals_str(yyjson_obj_get(o, "jsonrpc"), "2.0") &&
        id && wanted && yyjson_equals(id, wanted) && result != error && !yyjson_obj_get(o,"method"))
        out = yyjson_val_write(o, 0, NULL);
    yyjson_doc_free(d); yyjson_doc_free(r); return out;
}

char *mcp_response_sse(const char *body, size_t len, size_t *offset, const char *request) {
    if (!body || !offset || *offset > len) return NULL;
    size_t start = *offset;
    while (start < len) {
        size_t line = start, end = start, next = start;
        bool complete = false;
        while (line < len) {
            const char *nl = memchr(body + line, '\n', len - line);
            if (!nl) break;
            size_t e = (size_t)(nl - body), width = e - line;
            if (width && body[e-1] == '\r') width--;
            if (!width) { end = line; next = e + 1; complete = true; break; }
            line = e + 1;
        }
        if (!complete) return NULL;
        char *data = malloc(end - start + 1);
        if (!data) return NULL;
        size_t used = 0;
        for (line = start; line < end;) {
            const char *nl = memchr(body + line, '\n', end - line);
            size_t e = nl ? (size_t)(nl - body) : end;
            size_t content_end = e;
            if (content_end > line && body[content_end-1] == '\r') content_end--;
            if (content_end - line >= 5 && !memcmp(body + line, "data:", 5)) {
                size_t value = line + 5;
                if (value < content_end && body[value] == ' ') value++;
                if (used) data[used++] = '\n';
                memcpy(data + used, body + value, content_end - value);
                used += content_end - value;
            }
            line = e + 1;
        }
        data[used] = 0;
        char *match = used ? mcp_response_match(data, used, request) : NULL;
        free(data); *offset = next;
        if (match) return match;
        start = next;
    }
    return NULL;
}
