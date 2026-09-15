#ifndef DSCO_NATIVE_DISPLAY_H
#define DSCO_NATIVE_DISPLAY_H
#include <stdbool.h>
/* 0 means automatic; explicit multipliers may be any finite positive value. */
bool native_display_zoom_parse(const char *value, double *zoom);
#endif
