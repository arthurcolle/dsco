/* Link src/llm.c + src/json_util.c with dead-code stripping, so these tests
 * exercise the real cache functions without providers or live tool execution. */
#include "llm.h"
#include "tool_effects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Isolate the real cache/effect implementation from the megafile registry.
 * Binary scheduler tests cover the production registry and dispatch path. */
bool tools_meta_is_read_only(const char *name, bool *found) {
    bool known_read = name && (!strcmp(name, "sha256") || !strcmp(name, "read_file"));
    if (found)
        *found = known_read;
    return known_read;
}

int main(void) {
    const char *const dynamic[] = {
        "surface", "buffer", "buffer_view", "pty_session", "desktop", "browser_session", "computer",
        "kitty_remote", "kitten", "view_image", "browser"
    };
    const char *input = "{\"action\":\"read\"}";
    int checks = 0;
    for (size_t i = 0; i < sizeof(dynamic) / sizeof(dynamic[0]); ++i) {
        tool_cache_t cache;
        tool_cache_init(&cache);
        char result[128] = "unchanged";
        bool success = false;
        tool_cache_put(&cache, dynamic[i], input, "stale observation", true, 60.0);
        assert(cache.count == 0);
        assert(!tool_cache_get(&cache, dynamic[i], input, result, sizeof(result), &success));
        assert(!strcmp(result, "unchanged") && !success && cache.hits == 0);

        /* Seed a pre-existing entry using the cache's own key generation, then
         * change only the tool name. Lookup must still refuse the stale entry. */
        tool_cache_put(&cache, "sha256", input, "stale observation", true, 60.0);
        assert(cache.count == 1);
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "%s", strchr(cache.entries[0].key, ':'));
        snprintf(cache.entries[0].key, sizeof(cache.entries[0].key), "%s%s", dynamic[i], suffix);
        assert(!tool_cache_get(&cache, dynamic[i], input, result, sizeof(result), &success));
        assert(!strcmp(result, "unchanged") && !success && cache.hits == 0 && cache.misses == 2);
        tool_cache_free(&cache);
        ++checks;
    }

    tool_cache_t cache;
    tool_cache_init(&cache);
    char result[128];
    bool success = false;
    tool_cache_put(&cache, "sha256", "{}", "digest", true, 60.0);
    assert(tool_cache_get(&cache, "sha256", "{}", result, sizeof(result), &success));
    assert(success && !strcmp(result, "digest") && cache.hits == 1);
    tool_cache_put(&cache, "sha256", "{}", "updated", false, 60.0);
    assert(cache.count == 1);
    assert(tool_cache_get(&cache, "sha256", "{}", result, sizeof(result), &success));
    assert(!success && !strcmp(result, "updated"));
    cache.entries[0].timestamp = 0;
    assert(!tool_cache_get(&cache, "sha256", "{}", result, sizeof(result), &success));
    assert(cache.count == 0);
    tool_cache_free(&cache);
    ++checks;

    const struct {
        const char *input;
        bool read_only;
    } http_cases[] = {
        {"{}", true},
        {"{\"url\":\"http://localhost/\"}", true},
        {"{\"method\":\"GET\"}", true},
        {"{\"method\":\"HEAD\"}", true},
        {"{\"method\":\"GET\",\"body\":\"payload\"}", false},
        {"{\"body\":null}", false},
        {"{\"method\":\"POST\"}", false},
        {"{\"method\":\"PUT\"}", false},
        {"{\"method\":\"DELETE\"}", false},
        {"{\"method\":\"PATCH\"}", false},
        {"{\"method\":\"UNKNOWN\"}", false},
        {"{\"method\":\"get\"}", false},
        {"{\"method\":null}", false},
        {"{\"method\":12}", false},
        {"{\"method\":\"GET\",\"method\":\"POST\"}", false},
        {"{\"method\":\"GET\",\"method\":\"GET\"}", false},
        {"{\"method\":\"GET\\u0000POST\"}", false},
        {"{\"method\":\"GET\"} trailing", false},
        {"[\"GET\"]", false},
        {"{", false},
        {NULL, false},
    };
    for (size_t i = 0; i < sizeof(http_cases) / sizeof(http_cases[0]); ++i) {
        const char *http_input = http_cases[i].input;
        bool read_only = http_cases[i].read_only;
        assert(tool_http_request_is_read_only(http_input) == read_only);
        tool_cache_init(&cache);
        tool_cache_put(&cache, "http_request", http_input, "response", true, 60.0);
        assert(cache.count == (read_only ? 1 : 0));
        assert(tool_cache_get(&cache, "http_request", http_input, result,
                              sizeof(result), &success) == read_only);
        tool_cache_free(&cache);
        if (!read_only) {
            /* Refuse entries written by a previous eligibility policy too. */
            tool_cache_init(&cache);
            tool_cache_put(&cache, "sha256", http_input, "old mutation", true, 60.0);
            char suffix[64];
            snprintf(suffix, sizeof(suffix), "%s", strchr(cache.entries[0].key, ':'));
            snprintf(cache.entries[0].key, sizeof(cache.entries[0].key), "http_request%s", suffix);
            assert(!tool_cache_get(&cache, "http_request", http_input, result,
                                   sizeof(result), &success));
            tool_cache_free(&cache);
        }
        ++checks;
    }
    printf("dynamic tool cache: %d behavior checks passed\n", checks);
    return 0;
}
