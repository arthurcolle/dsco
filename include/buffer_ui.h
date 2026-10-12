#ifndef DSCO_BUFFER_UI_H
#define DSCO_BUFFER_UI_H
#include <stdbool.h>
#include <stddef.h>

/* Format a bounded transcript preview. No tool calls, input, or terminal I/O.
 * The caller applies its terminal-control sanitizer before rendering. */
void buffer_ui_format_result(const char *json, bool ok, char *out, size_t cap);
#endif
