/* JSON value-skip equivalence and object bounds; no providers/network. */
#define _DARWIN_C_SOURCE 1
#include "json_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(const char *json, int expected) {
    assert(json_get_int(json, "target", -1) == expected);
}

int main(void) {
    const char *tokens[] = {"a", "\\\"", "\\\\", "\\n", "{}[]", "é", "\\u0041"};
    const char *opens[] = {"", "[", "{\"nested\":["};
    const char *closes[] = {"", "]", "]}"};
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    char *map = mmap(NULL, (size_t)page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(map != MAP_FAILED);
    assert(mprotect(map, (size_t)page, PROT_READ | PROT_WRITE) == 0);
    unsigned cases = 0;
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned len = 0; len <= 160; len++) {
            for (unsigned token = 0; token < sizeof(tokens)/sizeof(tokens[0]); token++) {
                jbuf_t b; jbuf_init(&b, 256);
                jbuf_append(&b, "{\"payload\":"); jbuf_append(&b, opens[kind]);
                jbuf_append_char(&b, '"');
                for (unsigned i = 0; i < len; i++) jbuf_append_char(&b, 'x');
                jbuf_append(&b, tokens[token]);
                /* Every prefix ends at the heap boundary and a guard page.
                 * Includes a trailing escape and unterminated nested strings. */
                for (size_t cut = 0; cut <= b.len; cut++) {
                    char *exact = malloc(cut + 1); assert(exact);
                    memcpy(exact, b.data, cut); exact[cut] = 0;
                    check(exact, -1);
                    char *guard = map + page - cut - 1;
                    memcpy(guard, exact, cut + 1); check(guard, -1);
                    free(exact); cases++;
                }
                jbuf_append_char(&b, '"'); jbuf_append(&b, closes[kind]);
                jbuf_append(&b, ",\"target\":42}");
                check(b.data, 42);
                char *raw = json_get_raw(b.data, "payload"); assert(raw); free(raw);
                jbuf_free(&b);
            }
        }
    }
    assert(munmap(map, (size_t)page * 2) == 0);
    printf("JSON skip: PASS (%u truncated prefixes, heap/guard-page checks, nested/escaped values)\n", cases);
    return 0;
}
