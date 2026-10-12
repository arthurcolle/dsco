/* Offline harness JSON scan benchmark. Build/run: make bench-json-skip
 * Compare the same executable source/flags against a saved json_util.c baseline.
 * Timings include extraction/allocation/free, not just an isolated scan loop. */
#define _POSIX_C_SOURCE 200809L
#include "json_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static volatile uint64_t sink;

static void run_case(size_t bytes, bool escaped, bool response) {
    char *text = safe_malloc(bytes + 1);
    memset(text, 'a', bytes);
    if (escaped)
        for (size_t i = 31; i < bytes; i += 32) text[i] = '\n';
    text[bytes] = '\0';
    jbuf_t b;
    jbuf_init(&b, bytes + 256);
    if (response) {
        jbuf_append(&b, "{\"content\":[{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, text);
        jbuf_append(&b, "},{\"type\":\"tool_use\",\"id\":\"tool_1\",\"name\":\"read_file\","
                       "\"input\":{\"path\":\"src/agent.c\"}}],\"stop_reason\":\"tool_use\"}");
    } else {
        jbuf_append(&b, "{\"payload\":{\"items\":[");
        jbuf_append_json_str(&b, text);
        jbuf_append(&b, "]},\"count\":42,\"ok\":true}");
    }
    int iters = bytes < 128 ? 300000 : (bytes < 8192 ? 10000 : 1000);
    for (int sample = -1; sample < 7; sample++) {
        uint64_t start = now_ns();
        for (int i = 0; i < iters; i++) {
            if (response) {
                parsed_response_t r;
                assert(json_parse_response(b.data, &r));
                assert(r.count == 2 && r.blocks[0].text && r.blocks[1].tool_input);
                assert(strcmp(r.stop_reason, "tool_use") == 0);
                sink += (unsigned char)r.blocks[0].text[0] + strlen(r.blocks[0].text);
                json_free_response(&r);
            } else {
                int n = json_get_int(b.data, "count", -1);
                bool ok = json_get_bool(b.data, "ok", false);
                assert(n == 42 && ok);
                sink += (uint64_t)n + ok;
            }
        }
        uint64_t elapsed = now_ns() - start;
        if (sample >= 0)
            printf("{\"case\":\"%s-%zu-%s\",\"sample\":%d,\"iterations\":%d,\"ns_per_op\":%.2f}\n",
                   response ? "response" : "late-fields", bytes,
                   escaped ? "escaped" : "plain", sample, iters, (double)elapsed / iters);
    }
    free(text);
    jbuf_free(&b);
}

int main(void) {
    const size_t sizes[] = {16, 128, 4096, 65536};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (int escaped = 0; escaped < 2; escaped++)
            for (int response = 0; response < 2; response++)
                run_case(sizes[i], escaped != 0, response != 0);
    fprintf(stderr, "sink=%llu\n", (unsigned long long)sink);
    return 0;
}
