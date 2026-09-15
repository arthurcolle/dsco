/* Standalone JSON-lines fixture for the production owned-browser adapter. */
#include "browser_session.h"
#include "json_util.h"
#include "tool_content.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t image_size;
bool tool_content_add_image_base64(const char *data, const char *mime) {
    if (!data || strcmp(mime, "image/png") || strncmp(data, "iVBORw0KGgo", 11)) return false;
    image_size = strlen(data); return true;
}

int main(void) {
    char *line = NULL; size_t capacity = 0;
    char *result = malloc(256 * 1024);
    while (getline(&line, &capacity, stdin) > 0) {
        image_size = 0;
        bool ok = tool_browser_session(line, result, 256 * 1024);
        printf("{\"returned\":%s,\"image_base64_bytes\":%zu,\"response\":%s}\n",
               ok ? "true" : "false", image_size, result);
        fflush(stdout);
    }
    free(line); free(result); return 0;
}
