/* Regression for object-overreading JSON scans. Run: make test-json-scan-bounds */
#define _DARWIN_C_SOURCE 1
#include "json_util.h"
#include "simd.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void check_scan(char *p, size_t len) {
    memset(p, 'a', len);
    p[len] = '\0';
    assert(dsco_simd_json_unescaped_run(p) == len);
    for (size_t i = 0; i < len; ++i) {
        p[i] = '"';
        assert(dsco_simd_json_unescaped_run(p) == i);
        p[i] = '\\';
        assert(dsco_simd_json_unescaped_run(p) == i);
        p[i] = '\0';
        assert(dsco_simd_json_unescaped_run(p) == i);
        p[i] = 'a';
    }
}

int main(void) {
    /* Exact allocations exercise every vector tail and starting alignment. */
    for (size_t offset = 0; offset < 16; ++offset) {
        for (size_t len = 0; len <= 96; ++len) {
            char *allocation = malloc(offset + len + 1);
            assert(allocation);
            check_scan(allocation + offset, len);
            free(allocation);
        }
    }
    char stack_empty[] = "";
    char stack_short[] = "abcd";
    check_scan(stack_empty, 0);
    check_scan(stack_short, 4);
    assert(dsco_simd_json_unescaped_run("hello\\nworld") == 5);

    /* The terminator is the last readable byte, before a guard page. */
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    char *mapping = mmap(NULL, (size_t)page * 3, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(mprotect(mapping + page, (size_t)page, PROT_READ | PROT_WRITE) == 0);
    for (size_t len = 0; len <= 96; ++len)
        check_scan(mapping + 2 * page - len - 1, len);
    check_scan(mapping + page, 96);
    assert(munmap(mapping, (size_t)page * 3) == 0);

    /* Exercise the actual JSON decoder, including a fresh run after escape. */
    for (size_t len = 0; len <= 96; ++len) {
        const char *prefix = "{\"msg\":\"";
        const char *suffix = "\\nworld\"}";
        size_t size = strlen(prefix) + len + strlen(suffix) + 1;
        char *json = malloc(size);
        assert(json);
        memcpy(json, prefix, strlen(prefix));
        memset(json + strlen(prefix), 'a', len);
        strcpy(json + strlen(prefix) + len, suffix);
        char *value = json_get_str(json, "msg");
        assert(value && strlen(value) == len + 6);
        for (size_t i = 0; i < len; ++i) assert(value[i] == 'a');
        assert(strcmp(value + len, "\nworld") == 0);
        free(value);
        free(json);
    }
    puts("JSON scan bounds: PASS (heap alignments, stack/global, guard pages, decoder)");
    return 0;
}
