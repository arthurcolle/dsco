#ifndef DSCO_PRESENTATION_JSON_H
#define DSCO_PRESENTATION_JSON_H
#include <stdbool.h>
/* Display-only allocation. NULL means retain the original bytes unchanged. */
char *presentation_json(const char *text, bool expanded);
#endif
