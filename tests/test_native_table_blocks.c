#include "native_table_blocks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *sample = "intro\n\n| Tool | State |\n| --- | --- |\n| weather | ready |\n\nafter\n\n| X | Y |\n|---|---|\n| 1 | 2 |\n";
    native_table_span_t s;
    assert(native_table_find(sample, 0, &s));
    assert(s.start == (size_t)(strstr(sample, "| Tool") - sample));
    assert(s.schema.columns == 2 && !strcmp(s.schema.headings[0], "Tool"));
    assert(strstr(sample + s.data_start, "weather") == sample + s.data_start + 2);
    assert(native_table_find(sample, s.end, &s));
    assert(!strcmp(s.schema.headings[1], "Y"));
    assert(!native_table_find(sample, s.end, &s));
    assert(!native_table_find("```md\n| A | B |\n|---|---|\n|1|2|\n```\n", 0, &s));
    assert(!native_table_find("| A | B |\n| bad | separator |\n", 0, &s));
    puts("native table blocks: ok");
    return 0;
}
