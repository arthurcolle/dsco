#include "surface_cli.h"
#include "surface_registry.h"
#include "tools.h"
#include "../vendor/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SURFACE_CLI_INPUT_LIMIT (64u * 1024u)
#define SURFACE_CLI_RESULT_LIMIT (1024u * 1024u)
static const char *const usage = "dsco surface ACTION [JSON_OBJECT]";

static int emit_mutable(yyjson_mut_doc *doc, int status) {
    char *line = doc ? yyjson_mut_write(doc, 0, NULL) : NULL;
    if (!line) {
        fputs("{\"ok\":false,\"error\":\"out_of_memory\"}\n", stdout);
        yyjson_mut_doc_free(doc);
        return 1;
    }
    bool written = fputs(line, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    free(line);
    yyjson_mut_doc_free(doc);
    return written ? status : 1;
}

static int cli_error(const char *code, const char *detail, int status) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = doc ? yyjson_mut_obj(doc) : NULL;
    if (obj) {
        yyjson_mut_doc_set_root(doc, obj);
        yyjson_mut_obj_add_bool(doc, obj, "ok", false);
        yyjson_mut_obj_add_str(doc, obj, "error", code);
        yyjson_mut_obj_add_str(doc, obj, "detail", detail);
        yyjson_mut_obj_add_str(doc, obj, "usage", usage);
    }
    return emit_mutable(doc, status);
}

static int cli_help(void) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = doc ? yyjson_mut_obj(doc) : NULL;
    if (obj) {
        yyjson_mut_doc_set_root(doc, obj);
        yyjson_mut_obj_add_bool(doc, obj, "ok", true);
        yyjson_mut_obj_add_str(doc, obj, "usage", usage);
        yyjson_mut_obj_add_str(doc, obj, "actions",
            "status start list inspect create focus resize layout detach read send_text send_key close");
        yyjson_mut_obj_add_str(doc, obj, "description",
            "Manage an owned Kitty workspace. Use surface_id returned by start, create, or list.");
        yyjson_mut_val *examples = yyjson_mut_arr(doc);
        const char *const lines[] = {
            "dsco surface start '{\"visible\":true}'",
            "dsco surface list",
            "dsco surface create '{\"type\":\"window\",\"location\":\"vsplit\",\"command\":\"/bin/sh\"}'",
            "dsco surface focus '{\"surface_id\":\"view-...\"}'",
            "dsco surface read '{\"surface_id\":\"view-...\",\"extent\":\"screen\"}'"
        };
        if (examples) {
            for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
                yyjson_mut_arr_add_str(doc, examples, lines[i]);
            yyjson_mut_obj_add_val(doc, obj, "examples", examples);
        }
    }
    return emit_mutable(doc, 0);
}

/* Use the tool's own action enum, so CLI validation follows registry changes. */
static bool action_exists(const char *action) {
    yyjson_doc *schema = yyjson_read(SURFACE_SCHEMA, strlen(SURFACE_SCHEMA), 0);
    if (!schema) return false;
    yyjson_val *properties = yyjson_obj_get(yyjson_doc_get_root(schema), "properties");
    yyjson_val *actions = yyjson_obj_get(yyjson_obj_get(properties, "action"), "enum");
    size_t i, count;
    yyjson_val *item;
    bool found = false;
    yyjson_arr_foreach(actions, i, count, item)
        if (yyjson_equals_str(item, action)) { found = true; break; }
    yyjson_doc_free(schema);
    return found;
}

static const char *validate_object(yyjson_val *obj, const char *action) {
    if (!yyjson_is_obj(obj)) return "JSON_OBJECT must be an object";
    if (yyjson_obj_size(obj) > 256) return "JSON_OBJECT has too many fields";
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(obj, i, count, key, value) {
        const char *name = yyjson_get_str(key);
        if (!name || strlen(name) != yyjson_get_len(key)) return "object keys cannot contain NUL";
        size_t previous, previous_count;
        yyjson_val *earlier_key, *earlier_value;
        yyjson_obj_foreach(obj, previous, previous_count, earlier_key, earlier_value) {
            (void)earlier_value;
            if (previous == i) break;
            if (yyjson_equals_str(earlier_key, name)) return "duplicate object field";
        }
        if (!strcmp(name, "action") && !yyjson_equals_str(value, action))
            return "JSON action must exactly match ACTION";
    }
    return NULL;
}

int surface_cli(int argc, char **argv) {
    if (!argv || argc < 2 || !argv[1] || strcmp(argv[1], "surface"))
        return cli_error("invalid_arguments", "expected the surface subcommand", 2);
    if (argc == 2 || (argc == 3 && argv[2] &&
        (!strcmp(argv[2], "--help") || !strcmp(argv[2], "-h") || !strcmp(argv[2], "help"))))
        return cli_help();
    if (argc < 3 || argc > 4 || !argv[2] || !action_exists(argv[2]))
        return cli_error("invalid_arguments", "provide a supported ACTION and at most one JSON object", 2);
    const char *options = argc == 4 ? argv[3] : "{}";
    if (!options || strlen(options) > SURFACE_CLI_INPUT_LIMIT)
        return cli_error("invalid_arguments", "JSON_OBJECT is missing or exceeds 64 KiB", 2);
    yyjson_doc *parsed = yyjson_read(options, strlen(options), 0);
    if (!parsed) return cli_error("invalid_arguments", "malformed JSON_OBJECT", 2);
    const char *error = validate_object(yyjson_doc_get_root(parsed), argv[2]);
    if (error) {
        yyjson_doc_free(parsed);
        return cli_error("invalid_arguments", error, 2);
    }
    yyjson_mut_doc *request = yyjson_doc_mut_copy(parsed, NULL);
    yyjson_doc_free(parsed);
    if (!request) return cli_error("out_of_memory", "could not allocate request", 1);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(request);
    if (!yyjson_mut_obj_get(root, "action") &&
        !yyjson_mut_obj_add_str(request, root, "action", argv[2])) {
        yyjson_mut_doc_free(request);
        return cli_error("out_of_memory", "could not set action", 1);
    }
    char *input = yyjson_mut_write(request, 0, NULL);
    yyjson_mut_doc_free(request);
    char *result = calloc(SURFACE_CLI_RESULT_LIMIT, 1);
    if (!input || !result) {
        free(input); free(result);
        return cli_error("out_of_memory", "could not allocate tool buffers", 1);
    }
    /* Preserve operator configuration. The tool entrypoint applies capability
     * grants independently of the selected governance experiment model. */
    tools_init_local_only();
    bool ok = tools_execute_for_tier("surface", input, "trusted", result, SURFACE_CLI_RESULT_LIMIT);
    free(input);
    yyjson_doc *response = yyjson_read(result, strlen(result), 0);
    if (!response || !yyjson_is_obj(yyjson_doc_get_root(response))) {
        yyjson_doc_free(response);
        int status = cli_error("surface_failed", result[0] ? result : "surface returned an empty result", 1);
        free(result);
        return status;
    }
    free(result);
    char *line = yyjson_write(response, 0, NULL);
    yyjson_doc_free(response);
    if (!line) return cli_error("out_of_memory", "could not encode surface result", 1);
    bool written = fputs(line, stdout) >= 0 && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
    free(line);
    return ok && written ? 0 : 1;
}
