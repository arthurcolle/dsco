#include "native_display.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>

bool native_display_zoom_parse(const char *value, double *zoom) {
    if (!value || !*value || !strcasecmp(value, "auto") || !strcasecmp(value, "display")) {
        *zoom = 0.0;
        return true;
    }
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || !isfinite(parsed)) return false;
    if (*end == '%') { parsed /= 100.0; end++; }
    while (isspace((unsigned char)*end)) end++;
    if (*end || parsed <= 0.0) return false;
    *zoom = parsed;
    return true;
}

