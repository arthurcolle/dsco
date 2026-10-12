#include "presentation_blocks.h"
#include <ctype.h>
#include <string.h>

int presentation_table_cells(const char *line, char cells[PRESENTATION_COLUMNS][PRESENTATION_CELL]) {
    if (!line) return 0;
    const char *end = line;
    while (*end && *end != '\n' && *end != '\r') end++;
    while (line < end && isspace((unsigned char)*line)) line++;
    while (end > line && isspace((unsigned char)end[-1])) end--;
    if (line == end) return 0;
    if (*line == '|') line++;
    if (end > line && end[-1] == '|' && (end-line < 2 || end[-2] != '\\')) end--;
    int columns = 0;
    const char *start = line;
    size_t fence = 0;
    for (const char *p = line; p <= end; p++) {
        if (p < end && *p == '\\' && p + 1 < end) { p++; continue; }
        if (p < end && *p == '`') {
            size_t run = 1;
            while (p + run < end && p[run] == '`') run++;
            if (!fence) fence = run;
            else if (fence == run) fence = 0;
            p += run - 1;
            continue;
        }
        if (p != end && (*p != '|' || fence)) continue;
        if (columns == PRESENTATION_COLUMNS) return 0;
        const char *a = start, *b = p;
        while (a < b && isspace((unsigned char)*a)) a++;
        while (b > a && isspace((unsigned char)b[-1])) b--;
        size_t len = (size_t)(b-a);
        if (len >= PRESENTATION_CELL) return 0;
        memcpy(cells[columns], a, len); cells[columns++][len] = 0;
        start = p + 1;
    }
    return fence || columns < 2 ? 0 : columns;
}

bool presentation_table_rule(const char *line) {
    int dashes = 0, joins = 0;
    for (const char *p = line; p && *p && *p != '\n' && *p != '\r'; p++) {
        if (*p == '-') dashes++;
        else if (*p == '|' || *p == '+') joins++;
        else if (*p != ':' && !isspace((unsigned char)*p)) return false;
    }
    return dashes >= 3 && joins >= 2;
}

bool presentation_table_begin(const char *header, const char *separator, presentation_table_t *table) {
    if (!header || !separator || !table) return false;
    char normalized[4096];
    size_t length = 0;
    for (const char *p = separator; *p && *p != '\n' && *p != '\r'; p++) {
        if (length + 1 >= sizeof(normalized)) return false;
        normalized[length++] = *p == '+' ? '|' : *p;
    }
    normalized[length] = 0;
    separator = normalized;
    char heads[PRESENTATION_COLUMNS][PRESENTATION_CELL], cells[PRESENTATION_COLUMNS][PRESENTATION_CELL];
    int count = presentation_table_cells(header, heads);
    if (!count || presentation_table_cells(separator, cells) != count) return false;
    for (int i = 0; i < count; i++) {
        const char *p = cells[i];
        if (*p == ':') p++;
        int dashes = 0;
        while (*p == '-') { dashes++; p++; }
        if (*p == ':') p++;
        if (*p || dashes < 3) return false;
    }
    table->columns = count;
    memcpy(table->headings, heads, sizeof(heads));
    return true;
}
