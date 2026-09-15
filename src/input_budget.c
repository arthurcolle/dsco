#include "input_budget.h"
#include "../vendor/yyjson.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_UNKNOWN_WINDOW 32768
#define REQUEST_MAX_BYTES (64u * 1024u * 1024u)
#define ESTIMATE_MAX (INT_MAX / 2)

int input_budget_configured_limit(void) {
    const char *v = getenv("DSCO_MAX_INPUT_TOKENS");
    if (!v || !*v || !strcmp(v, "auto")) return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    /* Invalid settings cannot silently disable the guard. */
    return end && !*end && n >= 512 && n <= 2000000 ? (int)n : -1;
}

int input_budget_effective_limit(int window, int reserve) {
    int configured = input_budget_configured_limit();
    if (configured < 0) return -1;
    int limit = configured > 0 ? configured : INPUT_UNKNOWN_WINDOW;
    if (window > 0) {
        long long room = (long long)window - (reserve > 0 ? reserve : 0) - 1024;
        int available = room > 0 ? (int)room : 0;
        if (!configured || available < limit) limit = available;
    }
    return limit;
}
static int add(int a, int b) {
    return a >= ESTIMATE_MAX - b ? ESTIMATE_MAX : a + b;
}
static int text_tokens(const char *s, size_t n) {
    if (!s) return 0;
    size_t ascii = 0, other = 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 128) ascii++; else other++;
    }
    size_t value = (ascii + 2) / 3 + other;
    return value >= ESTIMATE_MAX ? ESTIMATE_MAX : (int)value;
}
static bool eq(yyjson_val *v, const char *s) { return yyjson_equals_str(v, s); }
static int estimate(yyjson_val *v, int depth) {
    if (!v || depth > 96) return ESTIMATE_MAX;
    if (yyjson_is_str(v)) return add(2, text_tokens(yyjson_get_str(v), yyjson_get_len(v)));
    if (yyjson_is_obj(v)) {
        yyjson_val *type = yyjson_obj_get(v, "type");
        /* Image tokens depend on model, dimensions and detail, not base64 size.
         * This is a reserve, not a universal upper bound for all image models. */
        if ((eq(type, "image") && yyjson_obj_get(v, "source")) ||
            (eq(type, "image_url") && yyjson_obj_get(v, "image_url")) ||
            eq(type, "input_image")) return 4096;
        yyjson_val *inline_data = yyjson_obj_get(v, "inlineData");
        const char *mime = yyjson_get_str(yyjson_obj_get(inline_data, "mimeType"));
        if (mime && !strncmp(mime, "image/", 6)) return 4096;
        int n = 8;
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) {
            n = add(n, text_tokens(yyjson_get_str(key), yyjson_get_len(key)));
            n = add(n, estimate(yyjson_obj_iter_get_val(key), depth + 1));
        }
        return n;
    }
    if (yyjson_is_arr(v)) {
        int n = 8; size_t i, max; yyjson_val *item;
        yyjson_arr_foreach(v, i, max, item) n = add(n, estimate(item, depth + 1));
        return n;
    }
    return 4;
}
int input_budget_estimate(const char *request) {
    if (!request || strnlen(request, REQUEST_MAX_BYTES + 1u) > REQUEST_MAX_BYTES) return -1;
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc); return -1;
    }
    int n = add(512, estimate(yyjson_doc_get_root(doc), 0));
    yyjson_doc_free(doc);
    return n;
}
static bool meq(yyjson_mut_val *v, const char *s) { return yyjson_mut_equals_str(v, s); }
static bool shorten(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, int cap) {
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    if (!yyjson_mut_is_str(v) || yyjson_mut_get_len(v) <= (size_t)cap + 256) return false;
    const char *s = yyjson_mut_get_str(v);
    size_t len = yyjson_mut_get_len(v), head = (size_t)cap * 3 / 4, tail = (size_t)cap - head;
    while (head && ((unsigned char)s[head] & 0xc0) == 0x80) head--;
    size_t end = len - tail;
    while (end < len && ((unsigned char)s[end] & 0xc0) == 0x80) end++;
    char *out = malloc((size_t)cap + 256);
    if (!out) return false;
    int written = snprintf(out, (size_t)cap + 256,
        "%.*s\n[request input budget: omitted %zu bytes; full content retained in local conversation history]\n%.*s",
        (int)head, s, end - head, (int)(len - end), s + end);
    bool ok = written > 0 && yyjson_mut_obj_put(obj, yyjson_mut_str(doc, key),
                                               yyjson_mut_strcpy(doc, out));
    free(out); return ok;
}
static int shorten_text(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, int cap) {
    if (shorten(doc, obj, key, cap)) return 1;
    yyjson_mut_val *content = yyjson_mut_obj_get(obj, key);
    int changed = 0; size_t i, max; yyjson_mut_val *block;
    yyjson_mut_arr_foreach(content, i, max, block) {
        yyjson_mut_val *type = yyjson_mut_obj_get(block, "type");
        if (meq(type, "text") || meq(type, "output_text") || !type)
            changed += shorten(doc, block, "text", cap);
    }
    return changed;
}
static bool user_input(yyjson_mut_val *m) {
    if (!meq(yyjson_mut_obj_get(m, "role"), "user")) return false;
    yyjson_mut_val *c = yyjson_mut_obj_get(m, "content");
    if (yyjson_mut_is_str(c)) return true;
    size_t i, max; yyjson_mut_val *b;
    yyjson_mut_arr_foreach(c, i, max, b)
        if (!meq(yyjson_mut_obj_get(b, "type"), "tool_result")) return true;
    return yyjson_mut_obj_get(m, "parts") != NULL;
}
static int reduce(yyjson_mut_doc *doc, int tool_cap, int assistant_cap) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *messages = yyjson_mut_obj_get(root, "messages");
    if (!messages) messages = yyjson_mut_obj_get(root, "input");
    if (!messages) messages = yyjson_mut_obj_get(root, "contents");
    if (!yyjson_mut_is_arr(messages)) return 0;
    size_t last_user = 0, i, max; yyjson_mut_val *m;
    yyjson_mut_arr_foreach(messages, i, max, m) if (user_input(m)) last_user = i;
    int changed = 0;
    yyjson_mut_arr_foreach(messages, i, max, m) {
        yyjson_mut_val *role = yyjson_mut_obj_get(m, "role");
        if (meq(role, "tool")) changed += shorten_text(doc, m, "content", tool_cap);
        if (meq(yyjson_mut_obj_get(m, "type"), "function_call_output"))
            changed += shorten(doc, m, "output", tool_cap);
        if (assistant_cap && i < last_user && (meq(role, "assistant") || meq(role, "model")))
            changed += shorten_text(doc, m, "content", assistant_cap);
        size_t j, count; yyjson_mut_val *b;
        yyjson_mut_arr_foreach(yyjson_mut_obj_get(m, "content"), j, count, b)
            if (meq(yyjson_mut_obj_get(b, "type"), "tool_result"))
                changed += shorten_text(doc, b, "content", tool_cap);
        yyjson_mut_arr_foreach(yyjson_mut_obj_get(m, "parts"), j, count, b) {
            yyjson_mut_val *response = yyjson_mut_obj_get(yyjson_mut_obj_get(b, "functionResponse"), "response");
            changed += shorten(doc, response, "result", tool_cap);
            if (assistant_cap && i < last_user && meq(role, "model"))
                changed += shorten(doc, b, "text", assistant_cap);
        }
    }
    return changed;
}
bool input_budget_apply(char **request, int window, int reserve, input_budget_result_t *r) {
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    r->limit = input_budget_configured_limit();
    if (r->limit < 0) {
        snprintf(r->reason, sizeof(r->reason), "DSCO_MAX_INPUT_TOKENS must be an integer from 512 to 2000000");
        return false;
    }
    if (!request || !*request || (r->before_tokens = input_budget_estimate(*request)) < 0) {
        snprintf(r->reason, sizeof(r->reason), "invalid or oversized request envelope"); return false;
    }
    yyjson_doc *parsed = yyjson_read(*request, strlen(*request), 0);
    if (!parsed) { snprintf(r->reason, sizeof(r->reason), "request allocation failed"); return false; }
    yyjson_val *root = yyjson_doc_get_root(parsed);
    const char *output_keys[] = {"max_tokens", "max_completion_tokens", "max_output_tokens"};
    for (size_t i = 0; i < sizeof(output_keys)/sizeof(*output_keys); i++) {
        yyjson_val *v = yyjson_obj_get(root, output_keys[i]);
        if (yyjson_is_int(v) && yyjson_get_sint(v) > 0 && yyjson_get_sint(v) < INT_MAX)
            reserve = (int)yyjson_get_sint(v);
    }
    yyjson_val *google_output = yyjson_obj_get(yyjson_obj_get(root, "generationConfig"), "maxOutputTokens");
    if (yyjson_is_int(google_output) && yyjson_get_sint(google_output) > 0 && yyjson_get_sint(google_output) < INT_MAX)
        reserve = (int)yyjson_get_sint(google_output);
    if (reserve < 0) reserve = 0;
    r->limit = input_budget_effective_limit(window, reserve);
    r->after_tokens = r->before_tokens;
    if (r->limit == 0) {
        snprintf(r->reason, sizeof(r->reason),
            "output reserve %d leaves no input room in %d-token context; reduce DSCO_MAX_TOKENS", reserve, window);
        yyjson_doc_free(parsed); return false;
    }
    if (r->before_tokens <= r->limit) {
        r->admitted = true; yyjson_doc_free(parsed); return true;
    }
    yyjson_mut_doc *doc = yyjson_doc_mut_copy(parsed, NULL);
    yyjson_doc_free(parsed);
    if (!doc) { snprintf(r->reason, sizeof(r->reason), "request projection allocation failed"); return false; }
    const int tool_caps[] = {8192, 2048, 512, 512};
    const int assistant_caps[] = {0, 0, 4096, 1024};
    char *candidate = NULL;
    for (int stage = 0; stage < 4; stage++) {
        r->reduced_fields += reduce(doc, tool_caps[stage], assistant_caps[stage]);
        free(candidate);
        candidate = yyjson_mut_write(doc, 0, NULL);
        if (!candidate) break;
        r->after_tokens = input_budget_estimate(candidate);
        if (r->after_tokens >= 0 && r->after_tokens <= r->limit) {
            free(*request); *request = candidate; candidate = NULL; r->admitted = true; break;
        }
    }
    free(candidate); yyjson_mut_doc_free(doc);
    if (!r->admitted)
        snprintf(r->reason, sizeof(r->reason),
            "protected input exceeds request budget (%d estimated tokens > %d); history retained; use /evict, /compact or /input-budget <tokens>",
            r->after_tokens, r->limit);
    return r->admitted;
}
