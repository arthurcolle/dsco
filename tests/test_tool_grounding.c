/* Standalone runtime grounding regression: link tool_grounding.c/json_util.c
 * and the JSON parser, without the real tool registry or provider execution. */
#include "tool_grounding.h"
#include "tools.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static int registry_reads, external_reads, profile_reads;
static int registry_count, external_count;
static bool profile_allowed[32];
static tool_def_t registry[32];
static bool native_active;
bool pixel_tui_session_active(void) { return native_active; }

const tool_def_t *tools_get_all(int *count) {
    registry_reads++;
    *count = registry_count;
    return registry;
}
int tools_external_count(void) { external_reads++; return external_count; }
bool tools_profile_allows_index(int index) {
    profile_reads++;
    return index >= 0 && index < registry_count && profile_allowed[index];
}

static void check(bool pass, const char *message) {
    checks++;
    if (!pass) { fprintf(stderr, "FAIL %u: %s\n", checks, message); exit(1); }
}

static void reset_registry(void) {
    native_active = false;
    memset(registry, 0, sizeof(registry));
    memset(profile_allowed, 1, sizeof(profile_allowed));
    static const char *const names[] = {
        "Bash", "bash", "discover_tools", "load_tools", "invoke_tool",
        "weather", "web_search", "read_file", "buffer", "registry_only_decoy"
    };
    registry_count = (int)(sizeof(names) / sizeof(names[0]));
    for (int i = 0; i < registry_count; i++) {
        registry[i].name = names[i];
        registry[i].description = "registry_description_decoy";
        registry[i].input_schema_json = "{\"type\":\"object\"}";
    }
    external_count = 17;
    registry_reads = external_reads = profile_reads = 0;
}

static jbuf_t render(const char *tools) {
    jbuf_t prompt;
    jbuf_init(&prompt, 128);
    jbuf_append(&prompt, "EXISTING SYSTEM PROMPT\n");
    tool_grounding_append(&prompt, tools);
    check(!strncmp(prompt.data, "EXISTING SYSTEM PROMPT\n", 23), "existing prompt is preserved");
    check(prompt.len == strlen(prompt.data), "prompt remains NUL terminated with correct length");
    return prompt;
}

static void absent(const char *tools) {
    jbuf_t prompt = render(tools);
    check(!strcmp(prompt.data, "EXISTING SYSTEM PROMPT\n"), "empty/malformed schemas append no availability claims");
    jbuf_free(&prompt);
}

static bool name_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' || c == '/';
}

static bool contains_name(const char *text, const char *name) {
    size_t n = strlen(name);
    for (const char *at = text; (at = strstr(at, name)); at++) {
        bool sentence_end = at[n] == '.' && (!at[n + 1] || isspace((unsigned char)at[n + 1]));
        if ((at == text || !name_char((unsigned char)at[-1])) &&
            (!name_char((unsigned char)at[n]) || sentence_end)) return true;
    }
    return false;
}

static void has(const jbuf_t *prompt, const char *name) {
    if (!contains_name(prompt->data, name)) fprintf(stderr, "Grounding output missing %s:\n%s\n", name, prompt->data);
    check(contains_name(prompt->data, name), name);
}
static void lacks(const jbuf_t *prompt, const char *name) {
    check(!contains_name(prompt->data, name), name);
}

static char *line_after(const jbuf_t *prompt, const char *label) {
    const char *line = strstr(prompt->data, label);
    if (!line) return strdup("");
    const char *end = strchr(line, '\n');
    return strndup(line, end ? (size_t)(end - line) : strlen(line));
}

static unsigned advertised_count(const jbuf_t *prompt) {
    unsigned count = 0;
    const char *label = strstr(prompt->data, "Directly advertised tool names (");
    check(label && sscanf(label, "Directly advertised tool names (%u)", &count) == 1,
          "advertised-name count is reported");
    return count;
}

static void catalog_counts(const jbuf_t *prompt, int builtin, int external) {
    int actual_builtin = -1, actual_external = -1;
    const char *line = strstr(prompt->data, "Registered local catalog:");
    check(line && sscanf(line, "Registered local catalog: %d builtin tools and %d external tools",
                         &actual_builtin, &actual_external) == 2,
          "builtin and external catalog counts are distinguished");
    check(actual_builtin == builtin && actual_external == external,
          "catalog counts reflect current registry values");
}

static void test_empty_and_invalid(void) {
    const char *inputs[] = {
        NULL, "", " ", "[]", "{}", "{\"tools\":[]}", ",\"tools\":[]",
        "null", "true", "1", "\"Bash\"", "[", "[{\"name\":\"bad_json\"}",
        "[{\"name\":\"trailing_json\"}] garbage", "{\"tools\":\"Bash\"}",
        "[null,1,\"string_name_decoy\",[],{}]",
        "[{\"description\":\"description_only_decoy\",\"input_schema\":{\"name\":\"schema_only_decoy\"}}]",
        "[{\"name\":null},{\"name\":3},{\"name\":true},{\"name\":[]},{\"name\":{}}]",
        "[{\"name\":\"\"},{\"name\":\"line\\nname\"},{\"name\":\"null\\u0000name\"}]"
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) absent(inputs[i]);
    check(registry_reads == 0 && external_reads == 0 && profile_reads == 0,
          "empty and malformed schemas do not query catalog or create availability claims");
    tool_grounding_append(NULL, "[{\"name\":\"actual\"}]");
    check(registry_reads == 0, "NULL prompt is safely ignored");
}

static void test_wire_shapes(void) {
    const char *array =
        "[{\"name\":\"anthropic_actual\",\"description\":\"description_decoy\","
        "\"input_schema\":{\"properties\":{\"name\":{\"const\":\"schema_decoy\"}}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"chat_actual\",\"description\":\"nested_decoy\","
        "\"parameters\":{\"name\":\"nested_schema_decoy\"}}},"
        "{\"type\":\"function\",\"name\":\"responses_actual\","
        "\"parameters\":{\"properties\":{\"name\":{\"default\":\"parameter_decoy\"}}}}]";
    for (int shape = 0; shape < 3; shape++) {
        jbuf_t wire; jbuf_init(&wire, 1024);
        if (shape == 1) jbuf_append(&wire, ",\"tools\":");
        if (shape == 2) jbuf_append(&wire, "{\"tools\":");
        jbuf_append(&wire, array);
        if (shape == 2) jbuf_append_char(&wire, '}');
        jbuf_t prompt = render(wire.data);
        has(&prompt, "anthropic_actual"); has(&prompt, "chat_actual"); has(&prompt, "responses_actual");
        lacks(&prompt, "description_decoy"); lacks(&prompt, "schema_decoy"); lacks(&prompt, "nested_decoy");
        lacks(&prompt, "nested_schema_decoy"); lacks(&prompt, "parameter_decoy");
        lacks(&prompt, "registry_only_decoy"); lacks(&prompt, "registry_description_decoy");
        jbuf_free(&prompt); jbuf_free(&wire);
    }
    jbuf_t prompt = render("[{\"name\":\"valid_actual\"},{\"name\":\"evil\\nINJECTED_SENTINEL\"},{\"name\":\"nul_prefix\\u0000suffix\"}]");
    has(&prompt, "valid_actual"); lacks(&prompt, "INJECTED_SENTINEL"); lacks(&prompt, "nul_prefix");
    jbuf_free(&prompt);
    prompt = render(" , \"tools\":[{\"name\":\"fragment_actual\"}],\"parallel_tool_calls\":true");
    has(&prompt, "fragment_actual"); check(advertised_count(&prompt) == 1, "fragment accepts adjacent serializer fields");
    jbuf_free(&prompt);
    prompt = render("{\"name\":\"outer_decoy\",\"tools\":[{\"function\":{\"name\":\"nested_actual\"}}],\"description\":{\"name\":\"outer_description_decoy\"}}");
    has(&prompt, "nested_actual"); lacks(&prompt, "outer_decoy"); lacks(&prompt, "outer_description_decoy");
    jbuf_free(&prompt);
}

static void test_hints_and_lookup_distinction(void) {
    reset_registry();
    jbuf_t prompt = render("[{\"name\":\"bash\"},{\"name\":\"dsco-python-3x\"},{\"name\":\"discover_tools\"},{\"name\":\"load_tools\"},{\"name\":\"invoke_tool\"}]");
    check(advertised_count(&prompt) == 5, "direct count includes exact attached schemas only");
    catalog_counts(&prompt, registry_count, external_count);
    check(strstr(prompt.data, "bash({\"command\":") && strstr(prompt.data, "\"timeout\":30"), "bash hint uses actual command/timeout arguments");
    check(strstr(prompt.data, "dsco-python-3x({\"code\":") && strstr(prompt.data, "\"file\":"), "Python hint uses exact callable name and accepted code/file arguments");
    check(strstr(prompt.data, "discover_tools({\"query\":") != NULL, "discovery hint includes query argument");
    check(strstr(prompt.data, "load_tools({\"tools\":[") != NULL, "schema loading hint uses names array");
    check(strstr(prompt.data, "invoke_tool({\"name\":") && strstr(prompt.data, "\"input\":"), "invoke hint includes selected name and schema input");
    char *direct = line_after(&prompt, "Directly advertised tool names (");
    char *catalog = line_after(&prompt, "Additional registered builtin names");
    check(!contains_name(direct, "weather") && contains_name(catalog, "weather"),
          "registered weather is a schema lookup candidate, never claimed directly advertised");
    check(!contains_name(catalog, "bash"), "directly advertised tool is not relisted as an additional builtin");
    check(strstr(catalog, "retrieve their schemas") != NULL, "additional registry entries require schema retrieval");
    free(direct); free(catalog); jbuf_free(&prompt);

    prompt = render("[{\"name\":\"unrelated_actual\"}]");
    check(!strstr(prompt.data, "bash({") && !strstr(prompt.data, "dsco-python-3x({"),
          "registered shell/Python availability does not invent direct call hints");
    check(!strstr(prompt.data, "discover_tools({") && !strstr(prompt.data, "load_tools({") && !strstr(prompt.data, "invoke_tool({"),
          "non-advertised workflow tools receive no direct call hints");
    lacks(&prompt, "weather"); jbuf_free(&prompt);

    /* Counts and profile filtering must be recomputed after a tool result can
     * register/remove tools or alter which builtin names are pageable. */
    const char *wire = "[{\"name\":\"discover_tools\"}]";
    external_count = 29; registry_count = 6;
    prompt = render(wire); catalog_counts(&prompt, 6, 29);
    catalog = line_after(&prompt, "Additional registered builtin names");
    check(contains_name(catalog, "weather") && !contains_name(catalog, "read_file"),
          "removed builtin entries disappear from additional lookup names");
    free(catalog); jbuf_free(&prompt);
    registry[5].name = "replacement_not_curated"; external_count = 3;
    prompt = render(wire); catalog_counts(&prompt, 6, 3);
    catalog = line_after(&prompt, "Additional registered builtin names");
    check(!contains_name(catalog, "weather"), "weather lookup hint is not a stale registry snapshot");
    free(catalog); jbuf_free(&prompt);
    registry[5].name = "weather"; profile_allowed[5] = false;
    prompt = render(wire);
    catalog = line_after(&prompt, "Additional registered builtin names");
    check(!contains_name(catalog, "weather"), "profile-disallowed builtin is not offered for lookup");
    check(profile_reads > 0, "lookup candidates pass current runtime profile filtering");
    free(catalog); jbuf_free(&prompt);
    prompt = render("[{\"name\":\"weather\"}]");
    direct = line_after(&prompt, "Directly advertised tool names (");
    check(contains_name(direct, "weather"), "attached schemas remain authoritative for directly advertised names");
    free(direct); jbuf_free(&prompt);
}

static void test_bounds_and_duplicates(void) {
    reset_registry();
    jbuf_t prompt = render("[{\"name\":\"unique_actual\"},{\"name\":\"unique_actual\"},{\"type\":\"function\",\"function\":{\"name\":\"unique_actual\"}}]");
    check(advertised_count(&prompt) == 1, "repeated wire-name aliases are deduplicated");
    jbuf_free(&prompt);
    char accepted[256], rejected[257];
    memset(accepted, 'a', sizeof(accepted) - 1); accepted[255] = 0;
    memset(rejected, 'r', sizeof(rejected) - 1); rejected[256] = 0;
    jbuf_t wire; jbuf_init(&wire, 1024);
    jbuf_append(&wire, "[{\"name\":"); jbuf_append_json_str(&wire, accepted);
    jbuf_append(&wire, "},{\"name\":"); jbuf_append_json_str(&wire, rejected);
    jbuf_append(&wire, "},{\"name\":\"slash/path:tool.name-1_ok\"},{\"name\":\"white space\"},{\"name\":\"tab\\tname\"},{\"name\":\"雪\"}]");
    prompt = render(wire.data); has(&prompt, accepted); lacks(&prompt, rejected);
    has(&prompt, "slash/path:tool.name-1_ok"); check(advertised_count(&prompt) == 2, "name byte limit and ASCII allowlist are enforced");
    jbuf_free(&prompt); jbuf_free(&wire);

    jbuf_init(&wire, 32768); jbuf_append_char(&wire, '[');
    char first[256] = "", last[256] = "", overflow[256] = "";
    for (int i = 0; i < 100; i++) {
        char name[256]; memset(name, 'n', 255); name[255] = 0;
        char prefix[16]; snprintf(prefix, sizeof(prefix), "bounded_%03d_", i < 97 ? i : 96);
        memcpy(name, prefix, strlen(prefix));
        if (i == 0) strcpy(first, name);
        if (i == 95) strcpy(last, name);
        if (i == 96) strcpy(overflow, name);
        if (i) jbuf_append_char(&wire, ',');
        jbuf_append(&wire, "{\"name\":"); jbuf_append_json_str(&wire, name); jbuf_append_char(&wire, '}');
    }
    jbuf_append_char(&wire, ']'); prompt = render(wire.data);
    check(prompt.len - strlen("EXISTING SYSTEM PROMPT\n") <= 32768, "maximum name inventory stays under 32 KiB");
    char *direct = line_after(&prompt, "Directly advertised tool names (");
    check(contains_name(direct, first) && contains_name(direct, last) && !contains_name(direct, overflow),
          "inventory enumerates at most the first 96 unique names");
    check(advertised_count(&prompt) == 97, "overflow duplicates do not inflate exact advertised-name count");
    free(direct); jbuf_free(&prompt); jbuf_free(&wire);

    size_t huge_size = 16u * 1024u * 1024u + 1u;
    char *huge = malloc(huge_size + 1); check(huge != NULL, "allocate oversized wire fixture");
    memset(huge, ' ', huge_size); huge[huge_size] = 0;
    absent(huge); free(huge);
}

static void test_current_request_not_snapshot(void) {
    jbuf_t first = render("[{\"name\":\"before_tool_result\"}]");
    has(&first, "before_tool_result"); lacks(&first, "after_tool_result");
    jbuf_t second = render("[{\"name\":\"after_tool_result\"}]");
    has(&second, "after_tool_result"); lacks(&second, "before_tool_result");
    jbuf_free(&first); jbuf_free(&second);
    /* Fresh prompt builders stand in for cheap/full rebuilds and another loop
     * iteration. Availability must never depend on a one-time catalog snapshot. */
    const char *wire = "[{\"name\":\"persistent_named_tool\"},{\"name\":\"bash\"}]";
    for (int iteration = 0; iteration < 4; iteration++) {
        jbuf_t prompt = render(wire);
        has(&prompt, "persistent_named_tool"); has(&prompt, "bash");
        jbuf_free(&prompt);
    }
}

static void test_provider_native_search(void) {
    reset_registry();
    jbuf_t prompt = render("[{\"type\":\"web_search\"},{\"type\":\"web_search_preview\"},{\"type\":\"web_search\"}]");
    char *direct = line_after(&prompt, "Directly advertised tool names (");
    check(contains_name(direct, "web_search") && contains_name(direct, "web_search_preview"),
          "known provider search types are grounded without fictional name fields");
    check(advertised_count(&prompt) == 2, "native search types are deduplicated");
    check(strstr(prompt.data, "provider-native") && strstr(prompt.data, "not invoke_tool"),
          "native search uses provider protocol rather than custom function invocation");
    free(direct); jbuf_free(&prompt);
    absent("[{\"type\":\"arbitrary_type_decoy\"},{\"type\":\"function\"},{\"type\":\"web_search\\u0000suffix\"},{\"type\":\"web_search\\n\"}]");
    absent("[{\"type\":\"web_search\",\"name\":null},{\"type\":\"web_search_preview\",\"name\":\"bad\\nname\"}]");
    absent("[{\"function\":{\"description\":\"decoy\",\"type\":\"web_search\"}}]");
    prompt = render("[{\"type\":\"function\",\"name\":\"web_search\",\"parameters\":{}}]");
    has(&prompt, "web_search");
    check(!strstr(prompt.data, "provider-native"), "explicitly named search function is not mistaken for native search protocol");
    jbuf_free(&prompt);
    prompt = render("{\"tools\":[{\"type\":\"web_search_preview\",\"description\":\"native_description_decoy\",\"parameters\":{\"name\":\"native_schema_decoy\"}}]}");
    has(&prompt, "web_search_preview"); lacks(&prompt, "native_description_decoy"); lacks(&prompt, "native_schema_decoy");
    jbuf_free(&prompt);
}

static bool has_native_guidance(const jbuf_t *prompt) {
    return strstr(prompt->data, "native Kitty graphics workspace is active") != NULL;
}

static void test_native_workspace_guidance(void) {
    reset_registry();
    int native_index = registry_count++;
    registry[native_index] = (tool_def_t){.name = "native_window", .input_schema_json = "{}"};
    native_active = true;
    const char *variants[] = {"[{\"name\":\"native_window\"}]", "[{\"name\":\"discover_tools\"}]",
        "[{\"name\":\"load_tools\"}]", "[{\"name\":\"invoke_tool\"}]"};
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        jbuf_t prompt = render(variants[i]);
        check(has_native_guidance(&prompt), "active registered available native window gets current workflow guidance");
        check(strstr(prompt.data, "next_step") && strstr(prompt.data, "evidence") && strstr(prompt.data, "draft"),
              "native guidance preserves workflow evidence and human draft boundaries");
        check(strstr(prompt.data, "visual surfaces are explicit opt-in only") != NULL,
              "native mode does not imply presentation consent");
        check(strstr(prompt.data, "until the human specifically re-enables it") != NULL,
              "disabled presentation persists until explicit re-enable");
        check(strstr(prompt.data, "headless checks instead") != NULL,
              "disabled surfaces retain a headless execution path");
        check(strstr(prompt.data, "Never undo saved macOS widget-hiding settings automatically") != NULL,
              "saved desktop preference is preserved");
        check(strstr(prompt.data, "hiding a view must not terminate its task") != NULL,
              "hiding preserves running work");
        check(strstr(prompt.data, "For work with multiple steps, keep") == NULL,
              "native guidance never mandates unsolicited workflow panels");
        jbuf_free(&prompt);
    }
    native_active = false;
    jbuf_t prompt = render(variants[0]);
    check(!has_native_guidance(&prompt), "inactive compositor never claims active native workflow"); jbuf_free(&prompt);
    native_active = true; profile_allowed[native_index] = false;
    prompt = render(variants[0]); check(!has_native_guidance(&prompt), "restricted profile suppresses unavailable native guidance"); jbuf_free(&prompt);
    profile_allowed[native_index] = true; registry_count--;
    prompt = render(variants[1]); check(!has_native_guidance(&prompt), "removed native registration cannot survive in guidance"); jbuf_free(&prompt);
    registry_count++;
    prompt = render("[{\"name\":\"unrelated_actual\"}]");
    check(!has_native_guidance(&prompt), "native registration alone does not invent a callable or discoverable tool path"); jbuf_free(&prompt);
    /* Direct advertisement beyond the display cap is still real capability;
     * display truncation must not alter behavioral guidance. */
    jbuf_t wire; jbuf_init(&wire, 4096); jbuf_append_char(&wire, '[');
    for (int i = 0; i < 96; i++) jbuf_appendf(&wire, "%s{\"name\":\"a_before_native_%03d\"}", i ? "," : "", i);
    jbuf_append(&wire, ",{\"name\":\"native_window\"}]");
    prompt = render(wire.data);
    check(advertised_count(&prompt) == 97 && has_native_guidance(&prompt), "display cap does not hide directly available native workflow capability");
    jbuf_free(&prompt); jbuf_free(&wire);
    native_active = false;
    prompt = render(variants[0]); check(!has_native_guidance(&prompt), "later request refreshes actual native session state"); jbuf_free(&prompt);
}

int main(void) {
    reset_registry();
    test_empty_and_invalid();
    test_wire_shapes();
    test_hints_and_lookup_distinction();
    test_bounds_and_duplicates();
    test_current_request_not_snapshot();
    test_provider_native_search();
    test_native_workspace_guidance();
    printf("tool_grounding: %u checks passed\n", checks);
    return 0;
}
