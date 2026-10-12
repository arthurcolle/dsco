#ifndef DSCO_TOOL_CONTENT_H
#define DSCO_TOOL_CONTENT_H
#include <stdbool.h>
typedef struct tool_content {
    char *mime_type;
    char *data;
    struct tool_content *next;
} tool_content_t;
/* Thread-owned content queue; callers transfer it when collecting worker results. */
bool tool_content_add_image_base64(const char *data, const char *mime);
bool tool_content_add_image_file(const char *path, const char *mime);
tool_content_t *tool_content_take(void);
bool tool_content_merge(tool_content_t *items);
void tool_content_free(tool_content_t *items);
void tool_content_clear(void);
#endif
