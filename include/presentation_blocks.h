#ifndef DSCO_PRESENTATION_BLOCKS_H
#define DSCO_PRESENTATION_BLOCKS_H
#include <stdbool.h>
#define PRESENTATION_COLUMNS 8
#define PRESENTATION_CELL 384
typedef struct { int columns; char headings[PRESENTATION_COLUMNS][PRESENTATION_CELL]; } presentation_table_t;
/* Return zero rather than truncate or reinterpret an incomplete/oversized row. */
bool presentation_table_rule(const char *line);
int presentation_table_cells(const char *line, char cells[PRESENTATION_COLUMNS][PRESENTATION_CELL]);
bool presentation_table_begin(const char *header, const char *separator, presentation_table_t *table);
#endif
