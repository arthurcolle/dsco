#include "lingo_autobot.h"
#include "toolmgmt.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define INPUT_LIMIT 4096u
#define OUTPUT_LIMIT (128u * 1024u)

const char lingo_autobot_schema[] =
    "{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"enum\":[\"tool_management\"],\"default\":\"tool_mana"
    "gement\"},\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512,\"pattern\":\"[^ ]\",\"not\":{\"pattern\":\"[\\\\u0"
    "000-\\\\u001f\\\\u007f]\"},\"description\":\"Visible text without ASCII control characters; native runtime enfor"
    "ces at most 512 UTF-8 bytes.\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16,\"default\":8}},\"require"
    "d\":[\"query\"],\"additionalProperties\":false}";

const char lingo_autobot_output_schema[] =
    "{\"oneOf\":[{\"type\":\"object\",\"properties\":{\"source\":{\"const\":\"tool_management_api\"},\"query\":{\"type\":\"strin"
    "g\",\"minLength\":1,\"maxLength\":512,\"pattern\":\"[^ ]\",\"not\":{\"pattern\":\"[\\\\u0000-\\\\u001f\\\\u007f]\"},\"descript"
    "ion\":\"Visible text without ASCII control characters; native runtime enforces at most 512 UTF-8 bytes.\"},"
    "\"matches\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\""
    "minLength\":5,\"maxLength\":63,\"pattern\":\"^tm__[A-Za-z0-9_]+$\",\"not\":{\"pattern\":\"[^A-Za-z0-9_]\"}},\"descript"
    "ion\":{\"type\":\"string\"},\"input_schema\":{\"type\":\"object\"},\"output_schema\":{\"type\":\"object\"},\"source\":{\"con"
    "st\":\"tool_management_api\"},\"remote_tool_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512,\"not\":{\"patte"
    "rn\":\"\\\\u0000\"},\"description\":\"Opaque remote identity; native limit is 512 UTF-8 bytes.\"}},\"required\":[\"n"
    "ame\",\"description\",\"input_schema\",\"output_schema\",\"source\",\"remote_tool_id\"],\"additionalProperties\":fals"
    "e}},\"matched\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16},\"showing\":{\"type\":\"integer\",\"minimum\":0,\"maxim"
    "um\":16},\"has_more\":{\"const\":false},\"note\":{\"type\":\"string\"}},\"required\":[\"source\",\"query\",\"matches\",\"mat"
    "ched\",\"showing\",\"has_more\",\"note\"],\"additionalProperties\":false},{\"type\":\"object\",\"properties\":{\"ok\":{\"c"
    "onst\":false},\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\",\"enum\":[\"response_buffer_too"
    "_small\",\"invalid_request\",\"discovery_disabled\",\"credentials_unavailable\",\"discovery_unavailable\",\"respon"
    "se_too_large\",\"invalid_discovery_response\"]},\"message\":{\"type\":\"string\",\"minLength\":1}},\"required\":[\"cod"
    "e\",\"message\"],\"additionalProperties\":false}},\"required\":[\"ok\",\"error\"],\"additionalProperties\":false}]}";

static bool fail(char *result, size_t capacity, const char *code, const char *message) {
    if (result && capacity) {
        int written = snprintf(result, capacity,
            "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
        if (written < 0 || (size_t)written >= capacity)
            result[0] = '\0';
    }
    return false;
}

static bool text_is(yyjson_val *value, const char *expected) {
    return yyjson_is_str(value) && yyjson_get_len(value) == strlen(expected) &&
           memcmp(yyjson_get_str(value), expected, strlen(expected)) == 0;
}

static bool bounded_text(yyjson_val *value, size_t minimum, size_t maximum) {
    return yyjson_is_str(value) && yyjson_get_len(value) >= minimum &&
           yyjson_get_len(value) <= maximum &&
           strlen(yyjson_get_str(value)) == yyjson_get_len(value);
}

static bool registered_name(yyjson_val *value) {
    if (!bounded_text(value, 5, 63) || strncmp(yyjson_get_str(value), "tm__", 4))
        return false;
    const unsigned char *name = (const unsigned char *)yyjson_get_str(value);
    for (size_t i = 4; i < yyjson_get_len(value); ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return false;
    return true;
}

/* This tool only discovers/registers schemas. Execution remains a separate
 * lingo.call of the exact registered tool, through tools_execute_for_tier. */
bool lingo_autobot_execute(const char *input, char *result, size_t capacity) {
    if (result && capacity)
        result[0] = '\0';
    if (!result || capacity < 160)
        return fail(result, capacity, "response_buffer_too_small", "Result buffer too small");
    size_t length = input ? strnlen(input, INPUT_LIMIT + 1) : 0;
    if (!length || length > INPUT_LIMIT)
        return fail(result, capacity, "invalid_request", "Expected a bounded argument object");
    yyjson_doc *request = yyjson_read(input, length, 0);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    yyjson_val *query = yyjson_obj_get(root, "query");
    yyjson_val *source = yyjson_obj_get(root, "source");
    yyjson_val *limit_value = yyjson_obj_get(root, "limit");
    unsigned limit = 8;
    bool valid = yyjson_is_obj(root) && bounded_text(query, 1, 512) &&
                 (!source || text_is(source, "tool_management"));
    if (valid && limit_value) {
        double n = yyjson_get_num(limit_value);
        valid = yyjson_is_num(limit_value) && isfinite(n) && floor(n) == n && n >= 1 && n <= 16;
        if (valid)
            limit = (unsigned)n;
    }
    size_t i, count;
    yyjson_val *key, *value;
    if (valid) {
        yyjson_obj_foreach(root, i, count, key, value) {
            const char *name = yyjson_get_str(key);
            if (strlen(name) != yyjson_get_len(key) || yyjson_obj_get(root, name) != value ||
                (strcmp(name, "query") && strcmp(name, "source") && strcmp(name, "limit"))) {
                valid = false;
                break;
            }
        }
    }
    if (valid) {
        bool visible = false;
        const unsigned char *q = (const unsigned char *)yyjson_get_str(query);
        for (i = 0; i < yyjson_get_len(query); ++i) {
            if (q[i] < 32 || q[i] == 127)
                valid = false;
            if (!isspace(q[i]))
                visible = true;
        }
        valid = valid && visible;
    }
    if (!valid) {
        yyjson_doc_free(request);
        return fail(result, capacity, "invalid_request",
                    "Expected source tool_management, query of 1..512 bytes, and integer limit 1..16");
    }
    const char *enabled = getenv("DSCO_TOOLMGMT");
    if (enabled && (!strcmp(enabled, "0") || !strcasecmp(enabled, "false") ||
                    !strcasecmp(enabled, "off"))) {
        yyjson_doc_free(request);
        return fail(result, capacity, "discovery_disabled", "Tool Management discovery is disabled");
    }
    if (!toolmgmt_token()) {
        yyjson_doc_free(request);
        return fail(result, capacity, "credentials_unavailable", "Tool Management credentials are not configured");
    }
    int matched = 0;
    char *body = toolmgmt_discover_tools(yyjson_get_str(query), (int)limit, &matched);
    if (!body) {
        yyjson_doc_free(request);
        return fail(result, capacity, "discovery_unavailable", "Tool Management discovery failed");
    }
    size_t body_length = strnlen(body, OUTPUT_LIMIT + 1);
    if (body_length > OUTPUT_LIMIT || body_length >= capacity) {
        free(body);
        yyjson_doc_free(request);
        return fail(result, capacity, "response_too_large", "Discovery response exceeds the result limit");
    }
    yyjson_doc *response = yyjson_read(body, body_length, 0);
    yyjson_val *data = response ? yyjson_doc_get_root(response) : NULL;
    yyjson_val *matches = yyjson_obj_get(data, "matches");
    valid = yyjson_is_obj(data) && text_is(yyjson_obj_get(data, "source"), "tool_management_api") &&
            text_is(yyjson_obj_get(data, "query"), yyjson_get_str(query)) &&
            yyjson_is_arr(matches) && yyjson_arr_size(matches) <= limit && matched >= 0 &&
            (size_t)matched == yyjson_arr_size(matches);
    if (valid) {
        yyjson_arr_foreach(matches, i, count, value) {
            yyjson_val *name = yyjson_obj_get(value, "name");
            valid = registered_name(name) &&
                    text_is(yyjson_obj_get(value, "source"), "tool_management_api") &&
                    bounded_text(yyjson_obj_get(value, "remote_tool_id"), 1, 512) &&
                    yyjson_is_obj(yyjson_obj_get(value, "input_schema")) &&
                    yyjson_is_obj(yyjson_obj_get(value, "output_schema"));
            if (!valid)
                break;
            for (size_t prior = 0; prior < i; ++prior)
                if (text_is(yyjson_obj_get(yyjson_arr_get(matches, prior), "name"), yyjson_get_str(name))) {
                    valid = false;
                    break;
                }
            if (!valid)
                break;
        }
    }
    if (valid)
        memcpy(result, body, body_length + 1);
    yyjson_doc_free(response);
    yyjson_doc_free(request);
    free(body);
    return valid ? true : fail(result, capacity, "invalid_discovery_response",
                              "Tool Management returned invalid or ambiguous discovery metadata");
}
