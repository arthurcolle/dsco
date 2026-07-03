/*
 * Scratchpad regression test: verifies write → read → list → delete → clear
 * lifecycle within a single process (in-memory store is per-process).
 *
 * Build: cc -Iinclude -c -o /tmp/sp_test.o tests/scratchpad_test.c
 *        cc -o /tmp/sp_test /tmp/sp_test.o build/obj/tools.o [plus deps]
 *
 * Or simpler: run via the test runner in test.c
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Forward declarations from tools.c */
extern bool tools_execute(const char *tool_name, const char *input_json, char *result, size_t rlen);

static int passes = 0, fails = 0;

static void check(const char *label, const char *input, const char *must_contain) {
    char result[8192];
    bool ok = tools_execute("scratchpad", input, result, sizeof(result));
    if (ok && strstr(result, must_contain)) {
        printf("  ✓ %s → %s\n", label, result);
        passes++;
    } else {
        printf("  ✗ %s → %s (expected to contain \"%s\")\n", label, result, must_contain);
        fails++;
    }
}

int main(void) {
    printf("=== Scratchpad Regression Test ===\n\n");

    /* Clear any prior state */
    tools_execute("scratchpad", "{\"action\":\"clear\"}", (char[256]){0}, 256);

    printf("[1] Write via action+key+value\n");
    check("write", "{\"action\":\"write\",\"key\":\"alpha\",\"value\":\"hello\"}",
          "\"op\":\"created\"");
    check("update", "{\"action\":\"write\",\"key\":\"alpha\",\"value\":\"world\"}",
          "\"op\":\"updated\"");

    printf("\n[2] Read via action+key\n");
    check("read", "{\"action\":\"read\",\"key\":\"alpha\"}", "\"value\":\"world\"");

    printf("\n[3] Write via content=key=value\n");
    check("write_content", "{\"action\":\"write\",\"content\":\"beta=42\"}", "\"op\":\"created\"");

    printf("\n[4] Read via content=key\n");
    check("read_content", "{\"action\":\"read\",\"content\":\"beta\"}", "\"value\":\"42\"");

    printf("\n[5] List\n");
    check("list", "{\"action\":\"list\"}", "\"entries\":2");

    printf("\n[6] Delete\n");
    check("delete", "{\"action\":\"delete\",\"key\":\"alpha\"}", "\"op\":\"deleted\"");

    printf("\n[7] Legacy op=set/get\n");
    check("legacy_set", "{\"op\":\"set\",\"key\":\"gamma\",\"value\":\"99\"}",
          "\"op\":\"created\"");
    check("legacy_get", "{\"op\":\"get\",\"key\":\"gamma\"}", "\"value\":\"99\"");

    printf("\n[8] Clear\n");
    check("clear", "{\"action\":\"clear\"}", "\"op\":\"cleared\"");

    printf("\n[9] Error cases (these should return errors = ok:false)\n");
    {
        char result[8192];
        const char *cases[] = {
            "{\"action\":\"read\"}",
            "{\"action\":\"write\",\"key\":\"x\"}",
            "{\"action\":\"read\",\"key\":\"nope\"}",
        };
        const char *labels[] = {"read_missing_key", "write_missing_val", "read_nonexistent"};
        for (int i = 0; i < 3; i++) {
            bool ok = tools_execute("scratchpad", cases[i], result, sizeof(result));
            if (!ok && strstr(result, "error")) {
                printf("  ✓ %s → %s\n", labels[i], result);
                passes++;
            } else {
                printf("  ✗ %s → %s (expected ok=false with error)\n", labels[i], result);
                fails++;
            }
        }
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
