#include "tool_grounding.h"
#include "tools.h"
#include "pixel_tui.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_NAMES = 96, MAX_NAME_BYTES = 255, MAX_WIRE_BYTES = 16 * 1024 * 1024 };

static bool usable_name(yyjson_val *value) {
    const char *name = yyjson_get_str(value);
    size_t n = yyjson_get_len(value);
    if (!name || !n || n > MAX_NAME_BYTES)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("_-.:/", c)) || c == 0)
            return false;
    }
    return true;
}

static bool contains(const char *const *names, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(names[i], name) == 0)
            return true;
    return false;
}

static int compare_names(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

void tool_grounding_append(jbuf_t *prompt, const char *tools_json) {
    if (!prompt || !tools_json)
        return;
    size_t length = strnlen(tools_json, MAX_WIRE_BYTES + 1u);
    if (!length || length > MAX_WIRE_BYTES)
        return;
    const char *start = tools_json;
    while (isspace((unsigned char)*start)) start++;
    if (*start == ',') start++;
    while (isspace((unsigned char)*start)) start++;

    jbuf_t wrapped = {0};
    if (*start == '"') {
        jbuf_init(&wrapped, length + 3);
        jbuf_append(&wrapped, "{");
        jbuf_append(&wrapped, start);
        jbuf_append(&wrapped, "}");
        start = wrapped.data;
    }
    yyjson_doc *doc = yyjson_read(start, strlen(start), 0);
    jbuf_free(&wrapped);
    if (!doc)
        return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *array = yyjson_is_arr(root) ? root : yyjson_obj_get(root, "tools");
    if (!yyjson_is_arr(array)) {
        yyjson_doc_free(doc);
        return;
    }

    /* Sort borrowed names once, so even repeated names beyond the display cap
     * are counted exactly without quadratic scans of large tool arrays. */
    size_t entries = yyjson_arr_size(array);
    const char **all_names = entries ? safe_malloc(entries * sizeof(*all_names)) : NULL;
    size_t valid_count = 0;
    const char *names[MAX_NAMES];
    size_t count = 0, total = 0, index, size;
    bool bash = false, python = false, discover = false, load = false, invoke = false;
    bool native_search = false, native_window_wire = false;
    yyjson_val *item;
    yyjson_arr_foreach(array, index, size, item) {
        /* Never mistake a property named 'name' inside an input schema or a
         * description for a directly callable function. */
        yyjson_val *function = yyjson_obj_get(item, "function");
        yyjson_val *name = yyjson_obj_get(yyjson_is_obj(function) ? function : item, "name");
        /* Responses names its provider-owned search tool by type, without a
         * function name. These known types are not custom function calls. */
        if (!name) {
            yyjson_val *type = yyjson_obj_get(item, "type");
            const char *text = yyjson_get_str(type);
            if (text && yyjson_get_len(type) == strlen(text) &&
                (!strcmp(text, "web_search") || !strcmp(text, "web_search_preview"))) {
                name = type;
                native_search = true;
            }
        }
        if (!usable_name(name))
            continue;
        all_names[valid_count++] = yyjson_get_str(name);
    }
    if (valid_count > 1)
        qsort(all_names, valid_count, sizeof(*all_names), compare_names);
    for (size_t i = 0; i < valid_count; i++) {
        const char *text = all_names[i];
        if (i && strcmp(all_names[i - 1], text) == 0) continue;
        bash |= strcmp(text, "bash") == 0;
        python |= strcmp(text, "dsco-python-3x") == 0;
        discover |= strcmp(text, "discover_tools") == 0;
        load |= strcmp(text, "load_tools") == 0;
        invoke |= strcmp(text, "invoke_tool") == 0;
        native_window_wire |= strcmp(text, "native_window") == 0;
        if (count < MAX_NAMES)
            names[count++] = text;
        total++;
    }
    free(all_names);
    if (!count) {
        yyjson_doc_free(doc);
        return;
    }

    int builtin_count = 0;
    const tool_def_t *builtins = tools_get_all(&builtin_count);
    int external_count = tools_external_count();
    bool native_workspace = false;
    if (pixel_tui_session_active() && (native_window_wire || discover || load || invoke)) {
        for (int i = 0; builtins && i < builtin_count; i++) {
            if (builtins[i].name && !strcmp(builtins[i].name, "native_window") && tools_profile_allows_index(i)) {
                native_workspace = true;
                break;
            }
        }
    }
    jbuf_append(prompt, "\n\nLIVE TOOLS — CURRENT REQUEST\n");
    jbuf_appendf(prompt, "Directly advertised tool names (%zu): ", total);
    for (size_t i = 0; i < count; i++) {
        if (i) jbuf_append(prompt, ", ");
        jbuf_append(prompt, names[i]);
    }
    if (total > count)
        jbuf_appendf(prompt, " (+%zu more in the attached tools array)", total - count);
    jbuf_append(prompt, ".\n");
    if (native_search)
        jbuf_append(prompt, "web_search/web_search_preview entries identified by type are provider-native "
                            "search tools; use their provider-defined protocol, not invoke_tool.\n");
    jbuf_appendf(prompt, "Registered local catalog: %d builtin tools and %d external tools. "
                        "Registration is not a permission grant; calls retain all execution gates.\n",
                 builtin_count, external_count);
    if (native_workspace)
        jbuf_append(prompt, "The native Kitty graphics workspace is active, but visual surfaces are explicit opt-in only. "
                            "Do not open or show desktop widgets, panels, overlays, dashboards, or extra Kitty windows "
                            "unless the human explicitly requests the relevant surface. Native-mode availability, "
                            "multi-step work, tests, and agent suggestions are not consent. Honor a disabled presentation "
                            "preference until the human specifically re-enables it; use the existing conversation and "
                            "headless checks instead. Never undo saved macOS widget-hiding settings automatically. "
                            "When explicitly requested, retrieve native_window's schema to open, update, arrange and "
                            "attach buffer panels. For requested editing of an AI-managed buffer, use native_window "
                            "action=buffer with its buffer_id/name, not a note or copied text; it is editable in place. "
                            "Tell the human to click the text, type, Ctrl-S to save, Esc to return to chat; "
                            "/buffer edit NAME opens the same editor. For native UI glitches use ui_trace "
                            "action=start with duration_ms to record component deltas and frame timing; "
                            "inspect its returned JSONL path after completion with read_file. The shortcut is /ui trace 10s. "
                            "The native composer also has Record UI issue, followed by Ask AI about trace after recording. "
                            "That explicit diagnostic request includes the completed local path; read it and investigate "
                            "instead of asking for screenshots or asking the human to remember trace commands. "
                            "Use plain human titles and concise progress messages with an outcome, "
                            "evidence and concrete next_step. Mark done only with real evidence. Human workflow buttons "
                            "deliver explicit steering; moving or closing a panel is not task approval or completion. "
                            "Preserve the human's draft, buffers, window arrangement, and running work; hiding a view "
                            "must not terminate its task.\n");

    if (bash)
        jbuf_append(prompt, "bash({\"command\":\"...\",\"timeout\":30}) runs real shell commands: "
                            "use it for local work and authorized curl/python3 lookups.\n");
    if (python)
        jbuf_append(prompt, "dsco-python-3x({\"code\":\"...\"}) runs real Python, or accepts "
                            "{\"file\":\"/path/script.py\"}. Use this exact callable name for Python.\n");

    if (discover || load || invoke) {
        /* Concrete names supply useful orientation even when the wire is a
         * deliberately small stable proxy set. Check current registration and
         * profile, and label them separately from the actual wire functions. */
        static const char *const examples[] = {
            "weather", "weather_batch", "http_request", "curl_raw", "browser", "browser_session",
            "read_file", "write_file", "edit_file", "append_file", "list_directory", "find_files",
            "grep_files", "bash", "dsco-python-3x", "surface", "buffer", "buffer_view", "native_window", "ui_render",
            "swarm", "context_recall", "context_status", "context_compact", "git_status",
        };
        bool wrote = false;
        for (size_t e = 0; e < sizeof(examples) / sizeof(examples[0]); e++) {
            if (contains(names, count, examples[e])) continue;
            for (int i = 0; builtins && i < builtin_count; i++) {
                if (!builtins[i].name || strcmp(builtins[i].name, examples[e]) != 0 ||
                    !tools_profile_allows_index(i)) continue;
                jbuf_append(prompt, wrote ? ", " : "Additional registered builtin names (retrieve their schemas): ");
                jbuf_append(prompt, examples[e]);
                wrote = true;
                break;
            }
        }
        if (wrote) jbuf_append(prompt, ".\n");
    }
    if (discover)
        jbuf_append(prompt, "discover_tools({\"query\":\"task keywords\",\"limit\":5}) searches the "
                            "live catalog and returns exact names/schemas. For weather try "
                            "{\"query\":\"weather current forecast\",\"limit\":5}. "
                            "Use category/offset/limit to browse more names.\n");
    if (load)
        jbuf_append(prompt, "load_tools({\"tools\":[\"exact_name\"]}) retrieves and retains schemas.\n");
    if (invoke)
        jbuf_append(prompt, "invoke_tool({\"name\":\"exact_name\",\"input\":{...}}) executes a registered "
                            "tool using its retrieved schema, even when its name is absent from the wire "
                            "array. A discovery result with a full schema is already sufficient; loading "
                            "is optional for retaining it. Discovery is not task completion: execute "
                            "the selected capability next.\n");
    jbuf_append(prompt, "Use the appropriate tools now when the task needs execution or live facts. "
                        "Do not answer with an unverified claim of no tools or hand the user a URL "
                        "instead of performing an available lookup. ");
    if (discover)
        jbuf_append(prompt, "Before reporting a missing capability, search this live catalog and inspect "
                            "the returned schemas. An empty search is not proof that Bash/Python or "
                            "another authorized route cannot do the task. ");
    jbuf_append(prompt, "Report concrete errors or policy restrictions accurately; never bypass a denial. "
                        "This inventory is refreshed on every request, including after tool results and "
                        "compaction; earlier assistant claims of no tools are not runtime evidence.\n");
    yyjson_doc_free(doc);
}
