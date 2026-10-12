#ifndef DSCO_NATIVE_TABLE_BLOCKS_H
#define DSCO_NATIVE_TABLE_BLOCKS_H
#include "presentation_blocks.h"
#include <stdbool.h>
#include <stddef.h>
/* Find a bounded GFM table in prose, skipping fenced code. Offsets refer to
 * the original immutable message. Invalid/incomplete tables remain prose. */
typedef struct {
    size_t start, data_start, end;
    presentation_table_t schema;
} native_table_span_t;
bool native_table_find(const char *text, size_t from, native_table_span_t *out);
#endif
