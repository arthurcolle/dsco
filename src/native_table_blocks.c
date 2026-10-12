#include "native_table_blocks.h"
#include <string.h>

static const char *next_line(const char *line) {
    const char *p = strchr(line, '\n');
    return p ? p + 1 : line + strlen(line);
}

bool native_table_find(const char *text, size_t from, native_table_span_t *out) {
    if (!text || !out) return false;
    size_t length = strlen(text);
    if (from > length) return false;
    bool fenced = false;
    char fence = 0;
    for (const char *line = text; *line; line = next_line(line)) {
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if ((*s == '`' || *s == '~') && s[1] == *s && s[2] == *s) {
            if (!fenced) { fenced = true; fence = *s; }
            else if (fence == *s) fenced = false;
            continue;
        }
        if (fenced || (size_t)(line - text) < from) continue;
        const char *rule = next_line(line);
        if (!*rule || !strchr(line, '|')) continue;
        presentation_table_t schema = {0};
        if (!presentation_table_begin(line, rule, &schema)) continue;
        const char *row = next_line(rule);
        char cells[PRESENTATION_COLUMNS][PRESENTATION_CELL];
        const char *end = row;
        while (*end && presentation_table_cells(end, cells) == schema.columns)
            end = next_line(end);
        *out = (native_table_span_t){
            .start = (size_t)(line - text), .data_start = (size_t)(row - text),
            .end = (size_t)(end - text), .schema = schema
        };
        return true;
    }
    return false;
}
