#include "buffer_cli.h"
#include "buffer_store.h"
#include "tools.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_CLI_INPUT_LIMIT (64u * 1024u)
#define BUFFER_CLI_RESULT_LIMIT (1024u * 1024u)
#define BUFFER_CLI_NAME_LIMIT 160u
static const char *const usage = "dsco buffer ACTION [JSON_OBJECT]";

static char *error_json(const char *code, const char *detail) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = doc ? yyjson_mut_obj(doc) : NULL;
    if (obj) {
        yyjson_mut_doc_set_root(doc, obj);
        yyjson_mut_obj_add_bool(doc, obj, "ok", false);
        yyjson_mut_obj_add_str(doc, obj, "error", code);
        yyjson_mut_obj_add_str(doc, obj, "detail", detail);
        yyjson_mut_obj_add_str(doc, obj, "usage", usage);
    }
    char *line = doc ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    return line;
}

static char *help_json(void) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = doc ? yyjson_mut_obj(doc) : NULL;
    if (obj) {
        yyjson_mut_doc_set_root(doc, obj);
        yyjson_mut_obj_add_bool(doc, obj, "ok", true);
        yyjson_mut_obj_add_str(doc, obj, "usage", usage);
        yyjson_mut_obj_add_str(doc, obj, "actions",
            "new create list inspect read write append rename fork close reopen save edit open view views focus resize layout detach close-view");
        yyjson_mut_obj_add_str(doc, obj, "description",
            "Keep named buffers independently of their terminal views. Close retains content; close-view closes a view. JSON supports workspace, revisions, and all tool options.");
        yyjson_mut_val *examples = yyjson_mut_arr(doc);
        const char *const lines[] = {
            "dsco buffer new notes",
            "dsco buffer append notes 'First thought'",
            "dsco buffer read notes",
            "dsco buffer edit notes",
            "dsco buffer open notes",
            "dsco buffer view notes",
            "dsco buffer views",
            "dsco buffer open '{\"name\":\"notes\",\"mode\":\"follow\"}'",
            "dsco buffer write '{\"name\":\"notes\",\"content\":\"Updated\",\"expected_revision\":\"<revision from read>\"}'",
            "dsco buffer close notes"
        };
        if (examples) {
            for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
                yyjson_mut_arr_add_str(doc, examples, lines[i]);
            yyjson_mut_obj_add_val(doc, obj, "examples", examples);
        }
    }
    char *line = doc ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    return line;
}

static bool storage_action(const char *action) {
    yyjson_doc *schema = yyjson_read(BUFFER_SCHEMA, strlen(BUFFER_SCHEMA), 0);
    if (!schema) return false;
    yyjson_val *props = yyjson_obj_get(yyjson_doc_get_root(schema), "properties");
    yyjson_val *actions = yyjson_obj_get(yyjson_obj_get(props, "action"), "enum");
    size_t i, count;
    yyjson_val *value;
    bool found = false;
    yyjson_arr_foreach(actions, i, count, value)
        if (yyjson_equals_str(value, action)) { found = true; break; }
    yyjson_doc_free(schema);
    return found;
}

static const char *validate_object(yyjson_val *obj, const char *action, bool view) {
    if (!yyjson_is_obj(obj)) return "JSON_OBJECT must be an object";
    if (yyjson_obj_size(obj) > 256) return "JSON_OBJECT has too many fields";
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(obj, i, count, key, value) {
        const char *name = yyjson_get_str(key);
        if (!name || strlen(name) != yyjson_get_len(key)) return "object keys cannot contain NUL";
        size_t j, previous_count;
        yyjson_val *earlier_key, *earlier_value;
        yyjson_obj_foreach(obj, j, previous_count, earlier_key, earlier_value) {
            (void)earlier_value;
            if (j == i) break;
            if (yyjson_equals_str(earlier_key, name)) return "duplicate object field";
        }
        if (!strcmp(name, "action") && !yyjson_equals_str(value, action))
            return "JSON action must match the canonical action (new=create, edit=buffer, view=open, views=list, close-view=close)";
        if (view && !strcmp(name, "mode") && !yyjson_equals_str(value, "view"))
            return "view requires mode view; use open to select another mode";
    }
    return NULL;
}

static bool looks_json(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '{' || *s == '[';
}

/* args begins with ACTION, independent of CLI or slash-command framing. */
static int build_request(int argc, const char *const *args,
                         const char **tool, char **input, char **reply) {
    *tool = NULL; *input = NULL; *reply = NULL;
    if (argc == 0 || (argc == 1 && args[0] &&
        (!strcmp(args[0], "help") || !strcmp(args[0], "--help") || !strcmp(args[0], "-h")))) {
        *reply = help_json();
        return *reply ? 0 : 1;
    }
    const char *error = NULL;
    if (argc < 1 || argc > 3 || !args || !args[0])
        error = "provide ACTION and one JSON object, NAME, or NAME TEXT for append";
    size_t total = 0;
    for (int i = 0; !error && i < argc; i++) {
        if (!args[i]) { error = "argument is missing"; break; }
        size_t n = strnlen(args[i], BUFFER_CLI_INPUT_LIMIT + 1);
        if (n > BUFFER_CLI_INPUT_LIMIT || total > BUFFER_CLI_INPUT_LIMIT - n)
            error = "arguments exceed 64 KiB";
        else total += n;
    }
    if (error) { *reply = error_json("invalid_arguments", error); return 2; }
    const char *action = args[0];
    bool view = !strcmp(action, "view");
    bool is_new = !strcmp(action, "new");
    bool native_edit = !strcmp(action, "edit");
    *tool = "buffer";
    if (is_new) action = "create";
    else if (native_edit) { *tool = "native_window"; action = "buffer"; }
    else if (view || !strcmp(action, "open")) { *tool = "buffer_view"; action = "open"; }
    else if (!strcmp(action, "views")) { *tool = "buffer_view"; action = "list"; }
    else if (!strcmp(action, "close-view")) { *tool = "buffer_view"; action = "close"; }
    else if (!strcmp(action, "focus") || !strcmp(action, "resize") ||
             !strcmp(action, "layout") || !strcmp(action, "detach")) *tool = "buffer_view";
    else if (!storage_action(action)) error = "unsupported ACTION; use dsco buffer help";

    bool short_name = is_new || native_edit || !strcmp(action, "open") ||
        (!strcmp(*tool, "buffer") && (!strcmp(action, "create") || !strcmp(action, "read") ||
         !strcmp(action, "inspect") || !strcmp(action, "close") || !strcmp(action, "reopen")));
    bool short_append = !strcmp(*tool, "buffer") && !strcmp(action, "append");
    bool json = argc == 2 && (!short_name || looks_json(args[1]));
    if (!error && argc == 3 && !short_append) error = "only append accepts NAME TEXT";
    if (!error && argc == 2 && short_append && !looks_json(args[1]))
        error = "append requires NAME TEXT or a JSON object";
    if (!error && argc == 2 && !json && (!args[1][0] || strlen(args[1]) > BUFFER_CLI_NAME_LIMIT))
        error = "NAME must contain 1 to 160 UTF-8 bytes";
    if (!error && argc == 3 && (!args[1][0] || strlen(args[1]) > BUFFER_CLI_NAME_LIMIT))
        error = "NAME must contain 1 to 160 UTF-8 bytes";
    if (error) { *tool = NULL; *reply = error_json("invalid_arguments", error); return 2; }

    yyjson_mut_doc *request = NULL;
    if (json) {
        yyjson_doc *parsed = yyjson_read(args[1], strlen(args[1]), 0);
        error = parsed ? validate_object(yyjson_doc_get_root(parsed), action, view) : "malformed JSON_OBJECT";
        if (!error) request = yyjson_doc_mut_copy(parsed, NULL);
        yyjson_doc_free(parsed);
        if (error) { *tool = NULL; *reply = error_json("invalid_arguments", error); return 2; }
    } else {
        request = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *obj = request ? yyjson_mut_obj(request) : NULL;
        if (obj) yyjson_mut_doc_set_root(request, obj);
    }
    yyjson_mut_val *obj = request ? yyjson_mut_doc_get_root(request) : NULL;
    bool ready = obj != NULL;
    if (ready && !yyjson_mut_obj_get(obj, "action"))
        ready = yyjson_mut_obj_add_str(request, obj, "action", action);
    if (ready && !json && argc >= 2)
        ready = yyjson_mut_obj_add_str(request, obj, "name", args[1]);
    if (ready && !json && argc == 3)
        ready = yyjson_mut_obj_add_str(request, obj, "content", args[2]);
    if (ready && is_new && !yyjson_mut_obj_get(obj, "kind"))
        ready = yyjson_mut_obj_add_str(request, obj, "kind", "scratch");
    if (ready && view && !yyjson_mut_obj_get(obj, "mode"))
        ready = yyjson_mut_obj_add_str(request, obj, "mode", "view");
    if (ready) *input = yyjson_mut_write(request, 0, NULL);
    yyjson_mut_doc_free(request);
    if (!*input) { *tool = NULL; *reply = error_json("out_of_memory", "could not encode request"); return 1; }
    if (strlen(*input) > BUFFER_CLI_INPUT_LIMIT) {
        free(*input); *input = NULL; *tool = NULL;
        *reply = error_json("invalid_arguments", "encoded request exceeds 64 KiB");
        return 2;
    }
    return 0;
}

static bool copy_reply(char *result, size_t cap, const char *line) {
    if (!result || !cap) return false;
    if (!line || strlen(line) >= cap) {
        const char *error = "{\"ok\":false,\"error\":\"output_too_small\"}";
        if (strlen(error) < cap) memcpy(result, error, strlen(error) + 1);
        else result[0] = '\0';
        return false;
    }
    memcpy(result, line, strlen(line) + 1);
    return true;
}

static char *execute_request(const char *tool, const char *input, const char *tier, bool *ok) {
    *ok = false;
    char *raw = calloc(BUFFER_CLI_RESULT_LIMIT, 1);
    if (!raw) return error_json("out_of_memory", "could not allocate tool result");
    *ok = tools_execute_for_tier(tool, input, tier, raw, BUFFER_CLI_RESULT_LIMIT);
    size_t n = strnlen(raw, BUFFER_CLI_RESULT_LIMIT);
    yyjson_doc *doc = n < BUFFER_CLI_RESULT_LIMIT ? yyjson_read(raw, n, 0) : NULL;
    char *line;
    if (doc && yyjson_is_obj(yyjson_doc_get_root(doc))) {
        line = yyjson_write(doc, 0, NULL);
    } else {
        *ok = false;
        line = error_json("buffer_failed", n == BUFFER_CLI_RESULT_LIMIT ?
            "tool result is not terminated" : n ? raw : "buffer returned an empty result");
    }
    yyjson_doc_free(doc);
    free(raw);
    if (!line) *ok = false;
    return line;
}

static int print_reply(char *line, int status) {
    if (!line) { line = error_json("out_of_memory", "could not encode response"); status = 1; }
    const char *output = line ? line : "{\"ok\":false,\"error\":\"out_of_memory\"}";
    bool written = fputs(output, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    free(line);
    return written ? status : 1;
}

int buffer_cli(int argc, char **argv) {
    if (!argv || argc < 2 || !argv[1] || strcmp(argv[1], "buffer"))
        return print_reply(error_json("invalid_arguments", "expected the buffer subcommand"), 2);
    const char *tool; char *input, *reply;
    int status = build_request(argc - 2, (const char *const *)(argv + 2), &tool, &input, &reply);
    if (!tool) return print_reply(reply, status);
    tools_init_local_only();
    bool ok;
    reply = execute_request(tool, input, "trusted", &ok);
    free(input);
    return print_reply(reply, ok ? 0 : 1);
}

/* Literal quoting only: no shell, expansion, command substitution, or globbing. */
static bool tokenize_tail(char *text, const char **args, int *argc) {
    *argc = 0;
    char *read = text, *write = text;
    while (*read) {
        while (*read && isspace((unsigned char)*read)) read++;
        if (!*read) break;
        if (*argc >= 3) return false;
        if (*argc == 1 && (*read == '{' || *read == '[')) {
            args[(*argc)++] = read;
            return true;
        }
        args[(*argc)++] = write;
        char quote = 0;
        while (*read && (quote || !isspace((unsigned char)*read))) {
            char c = *read++;
            if (c == '\'' || c == '"') {
                if (!quote) { quote = c; continue; }
                if (quote == c) { quote = 0; continue; }
            }
            if (c == '\\' && quote != '\'') {
                if (!*read) return false;
                c = *read++;
            }
            *write++ = c;
        }
        if (quote) return false;
        /* Consume whitespace before writing NUL when read and write coincide. */
        while (*read && isspace((unsigned char)*read)) read++;
        *write++ = '\0';
    }
    return true;
}

bool buffer_command_execute(const char *arguments, const char *tier, char *result, size_t cap) {
    if (!result || !cap) return false;
    result[0] = '\0';
    size_t n = arguments ? strnlen(arguments, BUFFER_CLI_INPUT_LIMIT + 1) : 0;
    char *reply = NULL;
    if (!tier || !*tier || n > BUFFER_CLI_INPUT_LIMIT) {
        reply = error_json("invalid_arguments", "tier is required and command must not exceed 64 KiB");
        copy_reply(result, cap, reply); free(reply); return false;
    }
    char *text = malloc(n + 1);
    if (!text) return false;
    memcpy(text, arguments ? arguments : "", n + 1);
    const char *args[3]; int argc;
    if (!tokenize_tail(text, args, &argc)) {
        free(text);
        reply = error_json("invalid_arguments", "invalid quoting or too many arguments; quote NAME and TEXT containing spaces");
        copy_reply(result, cap, reply); free(reply); return false;
    }
    const char *tool; char *input;
    int status = build_request(argc, args, &tool, &input, &reply);
    bool ok = status == 0;
    if (tool) { reply = execute_request(tool, input, tier, &ok); free(input); }
    free(text);
    bool copied = copy_reply(result, cap, reply);
    free(reply);
    return ok && copied;
}
