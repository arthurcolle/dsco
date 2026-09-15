#include "lingo_workbench.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COMMAND_LIMIT (64u * 1024u)
#define RESPONSE_LIMIT (512u * 1024u)
#define SESSION_LIMIT 256u
#define DISPLAY_LIMIT (32u * 1024u)

typedef struct {
    char id[SESSION_LIMIT + 1];
    bool json;
    bool closed;
    bool failed;
} workbench_t;

static bool text_is(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
           !memcmp(yyjson_get_str(v), s, strlen(s));
}

static bool bounded_string(yyjson_val *v, size_t max) {
    return yyjson_is_str(v) && yyjson_get_len(v) > 0 && yyjson_get_len(v) <= max &&
           strlen(yyjson_get_str(v)) == yyjson_get_len(v);
}

/* Reject duplicate keys, embedded-NUL keys and excessive nesting before copying
 * command fields. Otherwise session_id could be ambiguous between parsers. */
static bool unique_json(yyjson_val *v, unsigned depth) {
    if (depth > 64) return false;
    size_t i, n;
    yyjson_val *key, *value;
    if (yyjson_is_obj(v)) {
        yyjson_obj_foreach(v, i, n, key, value) {
            const char *s = yyjson_get_str(key);
            if (strlen(s) != yyjson_get_len(key) || yyjson_obj_get(v, s) != value ||
                !unique_json(value, depth + 1)) return false;
        }
    } else if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v, i, n, value) {
            if (!unique_json(value, depth + 1)) return false;
        }
    }
    return true;
}

static yyjson_doc *error_doc(const char *code, const char *message) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"error\":{\"code\":");
    jbuf_append_json_str(&b, code);
    jbuf_append(&b, ",\"message\":");
    jbuf_append_json_str(&b, message);
    jbuf_append(&b, "}}");
    yyjson_doc *doc = yyjson_read(b.data, b.len, 0);
    jbuf_free(&b);
    return doc;
}

/* ASCII JSON quoting escapes terminal controls, including Unicode controls.
 * The complete machine response is never truncated into invalid JSON. */
static void emit_json(yyjson_doc *doc) {
    char *s = yyjson_write(doc, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
    puts(s ? s : "{\"error\":{\"code\":\"serialization_failed\",\"message\":\"Cannot encode response\"}}");
    free(s);
    fflush(stdout);
}

static void display_value(jbuf_t *b, yyjson_val *v, size_t maximum) {
    if (b->len >= DISPLAY_LIMIT) return;
    char *s = v ? yyjson_val_write(v, YYJSON_WRITE_ESCAPE_UNICODE, NULL) : NULL;
    if (!s) {
        jbuf_append(b, "null");
        return;
    }
    size_t n = strlen(s);
    size_t remaining = DISPLAY_LIMIT - b->len;
    if (maximum > remaining) maximum = remaining;
    jbuf_append_len(b, s, n < maximum ? n : maximum);
    if (n > maximum) jbuf_append(b, "...");
    free(s);
}

static void display_field(jbuf_t *b, const char *label, yyjson_val *v, size_t maximum) {
    jbuf_append(b, label);
    display_value(b, v, maximum);
    jbuf_append_char(b, '\n');
}

/* Human labels omit JSON's outer quotes but retain its escaped contents. */
static void display_label(jbuf_t *b, yyjson_val *v, size_t maximum) {
    if (!yyjson_is_str(v)) { display_value(b, v, maximum); return; }
    char *s = yyjson_val_write(v, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
    if (!s) return;
    size_t n = strlen(s);
    if (n >= 2 && b->len < DISPLAY_LIMIT) {
        n -= 2;
        size_t remaining = DISPLAY_LIMIT - b->len;
        if (maximum > remaining) maximum = remaining;
        jbuf_append_len(b, s + 1, n < maximum ? n : maximum);
        if (n > maximum) jbuf_append(b, "...");
    }
    free(s);
}

static bool has_arguments(yyjson_val *args) {
    return args && !yyjson_is_null(args) &&
           !(yyjson_is_obj(args) && yyjson_obj_size(args) == 0) &&
           !(yyjson_is_arr(args) && yyjson_arr_size(args) == 0);
}

static void display_arguments(jbuf_t *b, yyjson_val *args) {
    if (!has_arguments(args)) return;
    jbuf_append_char(b, '(');
    if (yyjson_is_obj(args)) {
        size_t i, n;
        yyjson_val *key, *value;
        yyjson_obj_foreach(args, i, n, key, value) {
            if (i) jbuf_append(b, ", ");
            if (i >= 8) { jbuf_append(b, "..."); break; }
            display_label(b, key, 48);
            jbuf_append_char(b, '=');
            display_value(b, value, 96);
        }
    } else display_value(b, args, 256);
    jbuf_append_char(b, ')');
}

static void display_address(jbuf_t *b, yyjson_val *address) {
    yyjson_val *object = yyjson_obj_get(address, "object_id");
    if (!object) object = yyjson_obj_get(address, "object");
    display_label(b, object, 128);
    jbuf_append_char(b, '.');
    display_label(b, yyjson_obj_get(address, "field"), 96);
    display_arguments(b, yyjson_obj_get(address, "args"));
}

static void display_type(jbuf_t *b, yyjson_val *type, unsigned depth) {
    if (!yyjson_is_obj(type) || depth >= 3) { display_label(b, type, 96); return; }
    yyjson_val *kind = yyjson_obj_get(type, "kind");
    if (text_is(kind, "list") || text_is(kind, "optional")) {
        display_label(b, kind, 24);
        jbuf_append_char(b, '<');
        display_type(b, yyjson_obj_get(type, "of"), depth + 1);
        jbuf_append_char(b, '>');
    } else if (text_is(kind, "ref")) {
        jbuf_append(b, "ref<");
        display_label(b, yyjson_obj_get(type, "class"), 64);
        jbuf_append_char(b, '>');
    } else if (text_is(kind, "record")) {
        /* Field details remain available in the complete JSON inspector. */
        jbuf_appendf(b, "record{%zu fields}", yyjson_obj_size(yyjson_obj_get(type, "fields")));
    } else display_label(b, type, 96);
}

static void display_controls(jbuf_t *b, yyjson_val *controls) {
    jbuf_append(b, "Controls:\n");
    if (!yyjson_is_arr(controls)) {
        display_value(b, controls, 4096);
        jbuf_append_char(b, '\n');
        return;
    }
    size_t i, n;
    yyjson_val *control;
    yyjson_arr_foreach(controls, i, n, control) {
        if (i >= 32 || b->len > DISPLAY_LIMIT - 2048) {
            jbuf_append(b, "  ... more controls available in JSON mode\n"); break;
        }
        jbuf_appendf(b, "  %zu: ", i + 1);
        yyjson_val *label = yyjson_obj_get(control, "label");
        display_label(b, label ? label : yyjson_obj_get(control, "field"), 128);
        jbuf_append(b, " = ");
        display_value(b, yyjson_obj_get(control, "current"), 160);
        if (yyjson_is_true(yyjson_obj_get(control, "overridden"))) {
            jbuf_append(b, " (base=");
            display_value(b, yyjson_obj_get(control, "base"), 160);
            jbuf_append_char(b, ')');
        }
        yyjson_val *choices = yyjson_obj_get(control, "choices");
        if (yyjson_is_arr(choices) && yyjson_arr_size(choices)) {
            jbuf_append(b, "  choices: ");
            size_t j, total;
            yyjson_val *choice;
            yyjson_arr_foreach(choices, j, total, choice) {
                if (j) jbuf_append(b, " | ");
                if (j >= 8) { jbuf_append(b, "..."); break; }
                display_value(b, choice, 64);
            }
        }
        jbuf_append_char(b, '\n');
    }
    if (!yyjson_arr_size(controls)) jbuf_append(b, "  (none)\n");
}

static void display_why(jbuf_t *b, yyjson_val *why) {
    jbuf_append(b, "Why: ");
    display_address(b, why);
    jbuf_append(b, " [");
    display_label(b, yyjson_obj_get(why, "state"), 80);
    jbuf_append(b, "]\n");
    yyjson_val *error = yyjson_obj_get(why, "error");
    if (error && !yyjson_is_null(error)) display_field(b, "  Error: ", error, 1024);
    yyjson_val *reason = yyjson_obj_get(why, "reason");
    if (reason && !yyjson_is_null(reason)) display_field(b, "  Reason: ", reason, 1024);
    yyjson_val *deps = yyjson_obj_get(why, "dependencies");
    if (!yyjson_is_arr(deps)) {
        jbuf_append(b, "  No dependency reads recorded.\n");
        return;
    }
    jbuf_appendf(b, "  Dependencies (%zu):\n", yyjson_arr_size(deps));
    size_t i, n;
    yyjson_val *dep;
    yyjson_arr_foreach(deps, i, n, dep) {
        if (i >= 64 || b->len > DISPLAY_LIMIT - 2048) {
            jbuf_append(b, "    ... more dependencies available in JSON mode\n"); break;
        }
        jbuf_append(b, "    ");
        display_address(b, dep);
        jbuf_append_char(b, '\n');
    }
}

static void display_inspector(jbuf_t *b, yyjson_val *object) {
    jbuf_append(b, "Inspector: ");
    display_label(b, yyjson_obj_get(object, "id"), 128);
    jbuf_append(b, " : ");
    yyjson_val *class = yyjson_obj_get(object, "class");
    display_label(b, class, 96);
    jbuf_append(b, "  revision ");
    display_value(b, yyjson_obj_get(object, "revision"), 32);
    if (yyjson_is_true(yyjson_obj_get(object, "immutable"))) jbuf_append(b, "  immutable");
    jbuf_append(b, "\nFields:\n");
    yyjson_val *fields = yyjson_obj_get(object, "fields");
    size_t i, n;
    yyjson_val *field;
    yyjson_arr_foreach(fields, i, n, field) {
        if (i >= 64 || b->len > DISPLAY_LIMIT - 2048) {
            jbuf_append(b, "  ... more fields available in JSON mode\n"); break;
        }
        yyjson_val *name = yyjson_obj_get(field, "name");
        yyjson_val *kind = yyjson_obj_get(field, "kind");
        jbuf_append(b, "  ");
        display_label(b, name, 96);
        yyjson_val *params = yyjson_obj_get(field, "params");
        if (yyjson_obj_size(params)) {
            jbuf_append_char(b, '(');
            size_t j, count;
            yyjson_val *key, *type;
            yyjson_obj_foreach(params, j, count, key, type) {
                if (j) jbuf_append(b, ", ");
                if (j >= 6) { jbuf_append(b, "..."); break; }
                display_label(b, key, 48);
                jbuf_append(b, ": ");
                display_type(b, type, 0);
            }
            jbuf_append_char(b, ')');
        }
        jbuf_append(b, " : ");
        display_type(b, yyjson_obj_get(field, "type"), 0);
        jbuf_append(b, "  [");
        display_label(b, kind, 24);
        jbuf_append_char(b, ']');
        if (text_is(kind, "derived")) {
            jbuf_append(b, "  formula ");
            display_label(b, class, 96);
            jbuf_append_char(b, '.');
            display_label(b, name, 96);
        }
        jbuf_append_char(b, '\n');
    }
}

static void display_comparison(jbuf_t *b, const char *label, yyjson_val *value) {
    jbuf_append(b, label);
    if (yyjson_is_arr(yyjson_obj_get(value, "queue"))) {
        jbuf_append(b, "status=");
        display_label(b, yyjson_obj_get(value, "status"), 64);
        jbuf_append(b, ", selected=");
        display_label(b, yyjson_obj_get(value, "selected"), 96);
    } else display_value(b, value, 512);
    jbuf_append_char(b, '\n');
}

static void display_response(yyjson_doc *doc, const char *action) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *error = yyjson_obj_get(root, "error");
    jbuf_t b;
    jbuf_init(&b, 2048);
    if (error || yyjson_is_false(yyjson_obj_get(root, "ok"))) {
        display_field(&b, "Error: ", error ? error : root, 8192);
    } else {
        yyjson_val *view = yyjson_obj_get(root, "view");
        if (!strcmp(action, "why")) display_why(&b, view);
        else if (!strcmp(action, "controls")) display_controls(&b, view);
        else if (!strcmp(action, "inspect")) display_inspector(&b, view);
        else if (!strcmp(action, "save")) display_field(&b, "Saved: ", yyjson_obj_get(view, "path"), 4096);
        else if (!strcmp(action, "close")) jbuf_append(&b, "Session closed.\n");
        else {
            display_field(&b, "Title: ", yyjson_obj_get(view, "title"), 256);
            display_field(&b, "Context: ", yyjson_obj_get(view, "context"), 2048);
            jbuf_append(&b, "Target: ");
            display_address(&b, yyjson_obj_get(view, "target"));
            jbuf_append_char(&b, '\n');
            yyjson_val *value = yyjson_obj_get(view, "value");
            yyjson_val *baseline = yyjson_obj_get(view, "baseline");
            if (baseline) {
                display_comparison(&b, "Baseline: ", baseline);
                display_comparison(&b, "Scenario: ", value);
            }
            yyjson_val *baseline_error = yyjson_obj_get(view, "baseline_error");
            if (baseline_error) display_field(&b, "Baseline unavailable: ", baseline_error, 1024);
            yyjson_val *queue = yyjson_obj_get(value, "queue");
            if (yyjson_is_arr(queue)) {
                display_field(&b, "Status: ", yyjson_obj_get(value, "status"), 128);
                display_field(&b, "Selected: ", yyjson_obj_get(value, "selected"), 256);
                jbuf_append(&b, "Value: review queue\n  NAME | SOURCE CHANGES | CONFLICTS | NEXT ACTION\n");
                size_t i, n;
                yyjson_val *row;
                yyjson_arr_foreach(queue, i, n, row) {
                    if (i >= 40) { jbuf_append(&b, "  ... more rows available in JSON mode\n"); break; }
                    jbuf_append(&b, "  ");
                    display_value(&b, yyjson_obj_get(row, "name"), 48);
                    jbuf_append(&b, " | ");
                    display_value(&b, yyjson_obj_get(row, "source_changes"), 32);
                    jbuf_append(&b, " | ");
                    display_value(&b, yyjson_obj_get(row, "conflicts"), 32);
                    jbuf_append(&b, " | ");
                    display_value(&b, yyjson_obj_get(row, "next_action"), 120);
                    jbuf_append_char(&b, '\n');
                }
                if (yyjson_arr_size(yyjson_obj_get(value, "needs")))
                    display_field(&b, "Needs evidence: ", yyjson_obj_get(value, "needs"), 2048);
            } else display_field(&b, "Value: ", value, 8192);
            display_controls(&b, yyjson_obj_get(view, "controls"));
        }
    }
    if (b.len > DISPLAY_LIMIT) {
        fwrite(b.data, 1, DISPLAY_LIMIT, stdout);
        fputs("\n[Display truncated; use --json for complete records.]\n", stdout);
    } else fputs(b.data, stdout);
    fflush(stdout);
    jbuf_free(&b);
}

static void emit_response(workbench_t *w, yyjson_doc *doc, const char *action) {
    if (w->json) emit_json(doc);
    else display_response(doc, action);
}

static void local_error(workbench_t *w, const char *code, const char *message) {
    yyjson_doc *doc = error_doc(code, message);
    emit_response(w, doc, "error");
    yyjson_doc_free(doc);
    w->failed = true;
}

/* Every semantic operation, including cleanup, goes through the public gate. */
static yyjson_doc *execute_request(yyjson_mut_doc *request, bool *ok) {
    size_t length = 0;
    char *input = yyjson_mut_write(request, 0, &length);
    if (!input || length > COMMAND_LIMIT) {
        free(input);
        *ok = false;
        return error_doc("request_too_large", "Workbench requests must fit 64 KiB");
    }
    char *output = calloc(1, RESPONSE_LIMIT);
    if (!output) {
        free(input);
        *ok = false;
        return error_doc("allocation_failed", "Cannot allocate session response");
    }
    *ok = tools_execute("lingo_session", input, output, RESPONSE_LIMIT);
    free(input);
    size_t used = strnlen(output, RESPONSE_LIMIT);
    yyjson_doc *response = used < RESPONSE_LIMIT ? yyjson_read(output, used, 0) : NULL;
    yyjson_val *root = response ? yyjson_doc_get_root(response) : NULL;
    if (!yyjson_is_obj(root) || !unique_json(root, 0)) {
        yyjson_doc_free(response);
        if (!*ok && used > 0 && used < 2048 && output[0] != '{')
            response = error_doc("session_failed", output);
        else response = error_doc("invalid_response", "Session tool returned invalid or oversized JSON");
        *ok = false;
    } else if (yyjson_obj_get(root, "error") || yyjson_is_false(yyjson_obj_get(root, "ok"))) {
        *ok = false;
    } else if (!*ok) {
        yyjson_doc_free(response);
        response = error_doc("session_failed", "Session tool rejected the operation");
    } else if (*ok && (!bounded_string(yyjson_obj_get(root, "session_id"), SESSION_LIMIT) ||
                       !yyjson_obj_get(root, "view"))) {
        yyjson_doc_free(response);
        response = error_doc("invalid_response", "Session response lacks identity or semantic view");
        *ok = false;
    }
    free(output);
    return response;
}

static yyjson_mut_doc *request_doc(const char *action, const char *session) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "action", action);
    if (session) yyjson_mut_obj_add_strcpy(doc, root, "session_id", session);
    return doc;
}

static bool allowed_action(const char *s) {
    static const char *actions[] = {"read", "why", "controls", "set", "reset", "select",
                                    "inspect", "save", "close"};
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (!strcmp(s, actions[i])) return true;
    return false;
}

static yyjson_mut_doc *json_command(workbench_t *w, const char *line, char action[16]) {
    yyjson_doc *input = yyjson_read(line, strlen(line), 0);
    yyjson_val *root = input ? yyjson_doc_get_root(input) : NULL;
    yyjson_val *verb = yyjson_obj_get(root, "action");
    if (!yyjson_is_obj(root) || !unique_json(root, 0) || !bounded_string(verb, 15) ||
        !allowed_action(yyjson_get_str(verb))) {
        yyjson_doc_free(input);
        local_error(w, "invalid_command", "Expected a unique-key JSON object with an allowed session action");
        return NULL;
    }
    snprintf(action, 16, "%s", yyjson_get_str(verb));
    yyjson_mut_doc *request = request_doc(action, w->id);
    if (!request) {
        yyjson_doc_free(input);
        local_error(w, "allocation_failed", "Cannot allocate command");
        return NULL;
    }
    size_t i, n;
    yyjson_val *key, *value;
    bool valid = true;
    yyjson_obj_foreach(root, i, n, key, value) {
        const char *name = yyjson_get_str(key);
        if (!strcmp(name, "action")) continue;
        bool allowed = (!strcmp(action, "set") && (!strcmp(name, "control") || !strcmp(name, "value"))) ||
                       (!strcmp(action, "select") && (!strcmp(name, "object") || !strcmp(name, "field") || !strcmp(name, "args"))) ||
                       (!strcmp(action, "inspect") && !strcmp(name, "object")) ||
                       (!strcmp(action, "save") && !strcmp(name, "path"));
        if (!allowed) { valid = false; break; }
        yyjson_mut_obj_add(yyjson_mut_doc_get_root(request),
                           yyjson_mut_strcpy(request, name), yyjson_val_mut_copy(request, value));
    }
    yyjson_doc_free(input);
    if (!valid) {
        yyjson_mut_doc_free(request);
        local_error(w, "invalid_command", "Unknown command field; session_id is owned by this workbench");
        return NULL;
    }
    return request;
}

static char *skip_space(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    return s;
}

static char *word(char **cursor) {
    char *start = skip_space(*cursor), *end = start;
    while (*end && !isspace((unsigned char)*end)) ++end;
    if (*end) *end++ = '\0';
    *cursor = end;
    return start;
}

static void help(void) {
    puts("show | why | controls | set INDEX JSON | reset\n"
         "select OBJECT FIELD | inspect [OBJECT] | save PATH | quit\n"
         "Controls are 1-based. Example: set 1 \"smallest\"\n"
         "Changes are session scenarios; save retains a reconstructable session.\n"
         "Use --json for structured commands, including parameterized selections.");
}

static yyjson_mut_doc *terminal_command(workbench_t *w, char *line, char action[16]) {
    char *cursor = line;
    char *verb = word(&cursor);
    char *rest = skip_space(cursor);
    if (!*verb) return NULL;
    if (!strcmp(verb, "help") || !strcmp(verb, "?")) { help(); return NULL; }
    if (!strcmp(verb, "show")) verb = "read";
    if (!strcmp(verb, "quit") || !strcmp(verb, "q")) verb = "close";
    if (!allowed_action(verb)) {
        local_error(w, "invalid_command", "Unknown command; enter help");
        return NULL;
    }
    snprintf(action, 16, "%s", verb);
    yyjson_mut_doc *doc = request_doc(action, w->id);
    if (!doc) { local_error(w, "allocation_failed", "Cannot allocate command"); return NULL; }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    bool valid = true;
    if (!strcmp(action, "set")) {
        errno = 0;
        char *end;
        unsigned long index = strtoul(rest, &end, 10);
        valid = *rest >= '0' && *rest <= '9' && !errno && index >= 1 && index <= 2147483647UL &&
                end != rest && isspace((unsigned char)*end);
        yyjson_doc *value = valid ? yyjson_read(skip_space(end), strlen(skip_space(end)), 0) : NULL;
        valid = value && unique_json(yyjson_doc_get_root(value), 0);
        if (valid) {
            yyjson_mut_obj_add_uint(doc, root, "control", index);
            yyjson_mut_obj_add_val(doc, root, "value", yyjson_val_mut_copy(doc, yyjson_doc_get_root(value)));
        }
        yyjson_doc_free(value);
    } else if (!strcmp(action, "select")) {
        char *object = word(&rest), *field = word(&rest);
        valid = *object && *field && !*skip_space(rest);
        if (valid) {
            yyjson_mut_obj_add_strcpy(doc, root, "object", object);
            yyjson_mut_obj_add_strcpy(doc, root, "field", field);
        }
    } else if (!strcmp(action, "inspect")) {
        if (*rest) yyjson_mut_obj_add_strcpy(doc, root, "object", rest);
    } else if (!strcmp(action, "save")) {
        valid = *rest != '\0';
        if (valid) yyjson_mut_obj_add_strcpy(doc, root, "path", rest);
    } else valid = *rest == '\0';
    if (!valid) {
        yyjson_mut_doc_free(doc);
        local_error(w, "invalid_command", "Invalid arguments; enter help for command syntax");
        return NULL;
    }
    return doc;
}

/* Drain an oversized/binary line so its suffix cannot become another command. */
static int read_line(char *line, size_t capacity) {
    size_t length = 0;
    bool invalid = false;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (c == 0 || length >= capacity - 1) invalid = true;
        else line[length++] = (char)c;
    }
    if (ferror(stdin)) return -2;
    if (c == EOF && length == 0 && !invalid) return 0;
    if (length && line[length - 1] == '\r') --length;
    line[length] = '\0';
    return invalid ? -1 : 1;
}

static void close_session(workbench_t *w) {
    if (w->closed || !*w->id) return;
    yyjson_mut_doc *request = request_doc("close", w->id);
    bool ok = false;
    yyjson_doc *response = execute_request(request, &ok);
    yyjson_mut_doc_free(request);
    yyjson_doc_free(response);
    if (!ok) {
        fputs("lingo workbench: owned session cleanup failed\n", stderr);
        w->failed = true;
    }
    w->closed = true;
}

static void usage(void) {
    fputs("usage: dsco lingo open PROGRAM [ARGS_JSON] [--restore SESSION_FILE] [--json]\n", stderr);
}

int lingo_workbench_cli(int argc, char **argv) {
    workbench_t w = {0};
    for (int i = 3; i < argc; ++i) if (!strcmp(argv[i], "--json")) w.json = true;
    if (argc == 4 && !strcmp(argv[3], "--help")) { usage(); return 0; }
    if (argc < 4 || strcmp(argv[2], "open") || !*argv[3] || strlen(argv[3]) > 4096) {
        usage(); return 2;
    }
    const char *args = NULL, *restore = NULL;
    bool json_seen = false;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--json") && !json_seen) json_seen = true;
        else if (!strcmp(argv[i], "--restore") && !restore && i + 1 < argc &&
                 strncmp(argv[i + 1], "--", 2)) restore = argv[++i];
        else if (!args && strncmp(argv[i], "--", 2)) args = argv[i];
        else { usage(); return 2; }
    }
    w.json = json_seen;
    if (restore && (!*restore || strlen(restore) > 4096)) {
        local_error(&w, "invalid_arguments", "Restore path must contain 1..4096 bytes"); return 2;
    }
    yyjson_doc *arguments = args && strlen(args) <= COMMAND_LIMIT ? yyjson_read(args, strlen(args), 0) : NULL;
    if (args && (!arguments || !yyjson_is_obj(yyjson_doc_get_root(arguments)) ||
                 !unique_json(yyjson_doc_get_root(arguments), 0))) {
        yyjson_doc_free(arguments);
        local_error(&w, "invalid_arguments", "ARGS_JSON must be a bounded, unique-key JSON object");
        return 2;
    }
    yyjson_mut_doc *open = request_doc("open", NULL);
    if (!open) { yyjson_doc_free(arguments); return 1; }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(open);
    yyjson_mut_obj_add_strcpy(open, root, "path", argv[3]);
    if (restore) yyjson_mut_obj_add_strcpy(open, root, "restore_path", restore);
    if (arguments) yyjson_mut_obj_add_val(open, root, "args", yyjson_val_mut_copy(open, yyjson_doc_get_root(arguments)));
    yyjson_doc_free(arguments);
    tools_init_scripting();
    bool ok = false;
    yyjson_doc *response = execute_request(open, &ok);
    yyjson_mut_doc_free(open);
    if (ok) snprintf(w.id, sizeof(w.id), "%s", yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(response), "session_id")));
    if (!w.json) puts("Lingo workbench — enter help for commands.");
    emit_response(&w, response, "open");
    yyjson_doc_free(response);
    if (!ok) return 1;

    char *line = malloc(COMMAND_LIMIT + 1);
    if (!line) { w.failed = true; close_session(&w); return 1; }
    while (!w.closed) {
        if (!w.json && isatty(STDIN_FILENO)) { fputs("lingo> ", stdout); fflush(stdout); }
        int status = read_line(line, COMMAND_LIMIT + 1);
        if (!status) break;
        if (status < 0) {
            local_error(&w, status == -2 ? "input_failed" : "invalid_command",
                        status == -2 ? "Cannot read command input" : "Command contains NUL or exceeds 64 KiB");
            if (status == -2) break;
            continue;
        }
        if (!w.json) {
            size_t n = strlen(line);
            while (n && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
        }
        char action[16] = {0};
        yyjson_mut_doc *request = w.json ? json_command(&w, line, action) : terminal_command(&w, line, action);
        if (!request) continue;
        response = execute_request(request, &ok);
        yyjson_mut_doc_free(request);
        if (ok && !text_is(yyjson_obj_get(yyjson_doc_get_root(response), "session_id"), w.id)) {
            yyjson_doc_free(response);
            response = error_doc("invalid_response", "Session response identity changed");
            ok = false;
        }
        emit_response(&w, response, action);
        yyjson_doc_free(response);
        if (!ok) w.failed = true;
        if (ok && !strcmp(action, "close")) w.closed = true;
    }
    free(line);
    close_session(&w);
    return w.failed ? 1 : 0;
}
