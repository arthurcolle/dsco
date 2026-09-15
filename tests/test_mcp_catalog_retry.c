/* Narrow offline test: include the implementation to exercise its private
 * adapter without exposing a bypass API. Unused MCP transport is dead-stripped.
 * cc -std=c11 -D_DARWIN_C_SOURCE -ffunction-sections -fdata-sections -Iinclude
 * tests/test_mcp_catalog_retry.c src/json_util.c src/json_fast.c
 * -Wl,-dead_strip -lcurl -lm -o build/test_mcp_catalog_retry */
#include "../src/mcp.c"

static int checks, failures, calls;
static bool deny;
static mcp_registry_t registry;
static const char *schema = "{\"type\":\"object\",\"properties\":{\"tool_name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"tool_name\",\"arguments\"]}";
static const char *rejection = "{\"error\":\"Unknown meta-tool: accuweather_weather_current\",\"available_meta_tools\":[\"search_tools\",\"execute_tool\"]}";
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

const char *tools_execution_tier(void) { return "untrusted"; }
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *result, size_t cap) {
    calls++;
    CHECK(strcmp(name, "mcp__configured__execute_tool") == 0);
    CHECK(strcmp(tier, "untrusted") == 0);
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    CHECK(yyjson_obj_size(root) == 2);
    CHECK(yyjson_equals_str(yyjson_obj_get(root, "tool_name"), "accuweather_weather_current"));
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(root, "arguments"), "id"), "DC π 🦉"));
    yyjson_doc_free(doc);
    CHECK(mcp_retry_catalog_leaf(&registry, &registry.tools[0], "{}", rejection) == NULL);
    snprintf(result, cap, "%s", deny ? "denied: DSCO_ALLOW_NET=0" : "{\"temperature\":24}");
    return !deny;
}

static void reset(void) {
    memset(registry.servers, 0, 2 * sizeof(registry.servers[0]));
    memset(registry.tools, 0, 3 * sizeof(registry.tools[0]));
    registry.server_count = 1;
    registry.tool_count = 2;
    registry.servers[0].initialized = true;
    registry.servers[0].transport = MCP_TRANSPORT_HTTP;
    strcpy(registry.servers[0].url, "https://tools.distributed.systems/mcp");
    strcpy(registry.tools[0].name, "mcp__configured__accuweather_weather_current");
    strcpy(registry.tools[0].remote_name, "accuweather_weather_current");
    strcpy(registry.tools[1].name, "mcp__configured__execute_tool");
    strcpy(registry.tools[1].remote_name, "execute_tool");
    strcpy(registry.tools[1].input_schema, schema);
    calls = 0;
    deny = false;
}

static char *retry(const char *error, const char *arguments) {
    return mcp_retry_catalog_leaf(&registry, &registry.tools[0], arguments, error);
}

int main(void) {
    reset();
    char *result = retry(rejection, "{\"id\":\"DC π 🦉\"}");
    CHECK(result && strcmp(result, "{\"temperature\":24}") == 0);
    CHECK(calls == 1);
    free(result);

    reset(); deny = true;
    result = retry(rejection, "{\"id\":\"DC π 🦉\"}");
    CHECK(result && strcmp(result, "denied: DSCO_ALLOW_NET=0") == 0);
    CHECK(calls == 1);
    free(result);

    const char *errors[] = {
        NULL, "timeout", "{}", "{\"error\":\"permission denied\"}",
        "{\"error\":\"Unknown meta-tool: another_tool\",\"available_meta_tools\":[\"execute_tool\"]}",
        "{\"error\":\"Unknown meta-tool: accuweather_weather_current\"}",
        "{\"error\":\"Unknown meta-tool: accuweather_weather_current\\u0000extra\",\"available_meta_tools\":[\"execute_tool\"]}",
        "{\"error\":\"Unknown meta-tool: accuweather_weather_current\",\"error\":\"timeout\",\"available_meta_tools\":[\"execute_tool\"]}",
        "{\"error\":\"Unknown meta-tool: accuweather_weather_current\",\"available_meta_tools\":\"execute_tool\"}"
    };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        reset(); CHECK(retry(errors[i], "{}") == NULL); CHECK(calls == 0);
    }
    const char *bad_schemas[] = {
        "{}", "not JSON",
        "{\"type\":\"object\",\"properties\":{\"tool_name\":{\"type\":\"integer\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"tool_name\",\"arguments\"]}",
        "{\"type\":\"object\",\"properties\":{\"tool_name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"tool_name\",\"arguments\",\"approval\"]}",
        "{\"type\":\"object\",\"properties\":{\"tool_name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"tool_name\",\"tool_name\"]}"
    };
    for (size_t i = 0; i < sizeof(bad_schemas) / sizeof(bad_schemas[0]); i++) {
        reset(); strcpy(registry.tools[1].input_schema, bad_schemas[i]);
        CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    }
    reset(); registry.tools[1].server_idx = 1;
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); strcpy(registry.servers[0].url, "https://example.org/mcp");
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); registry.servers[0].transport = MCP_TRANSPORT_STDIO;
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); registry.tool_count = 1;
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); registry.tools[2] = registry.tools[1]; registry.tool_count = 3;
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); strcpy(registry.tools[0].remote_name, "execute_tool");
    CHECK(retry(rejection, "{}") == NULL); CHECK(calls == 0);
    reset(); CHECK(retry(rejection, "[]") == NULL); CHECK(calls == 0);
    reset(); CHECK(retry(rejection, "{bad}") == NULL); CHECK(calls == 0);
    printf("MCP catalog retry: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
