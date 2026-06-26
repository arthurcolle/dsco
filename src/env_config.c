#include "env_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *env_nonempty(const char *name) {
    if (!name || !name[0])
        return NULL;
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static int ascii_ieq(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

bool dsco_env_truthy_str(const char *v) {
    return v && (ascii_ieq(v, "1") || ascii_ieq(v, "true") || ascii_ieq(v, "yes") ||
                 ascii_ieq(v, "on") || ascii_ieq(v, "y") || ascii_ieq(v, "t"));
}

bool dsco_env_falsy_str(const char *v) {
    return v && (ascii_ieq(v, "0") || ascii_ieq(v, "false") || ascii_ieq(v, "no") ||
                 ascii_ieq(v, "off") || ascii_ieq(v, "n") || ascii_ieq(v, "f"));
}

bool dsco_env_bool(const char *name, bool def) {
    const char *v = env_nonempty(name);
    if (!v)
        return def;
    if (dsco_env_truthy_str(v))
        return true;
    if (dsco_env_falsy_str(v))
        return false;
    return def;
}

long dsco_env_long(const char *name, long def, long min_v, long max_v) {
    const char *v = env_nonempty(name);
    if (!v)
        return def;
    char *end = NULL;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (errno || end == v)
        return def;
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (end && *end)
        return def;
    if (min_v <= max_v) {
        if (x < min_v)
            return min_v;
        if (x > max_v)
            return max_v;
    }
    return x;
}

int dsco_env_int(const char *name, int def, int min_v, int max_v) {
    long x = dsco_env_long(name, def, min_v, max_v);
    if (x < INT_MIN)
        return INT_MIN;
    if (x > INT_MAX)
        return INT_MAX;
    return (int)x;
}

size_t dsco_env_size(const char *name, size_t def, size_t min_v, size_t max_v) {
    const char *v = env_nonempty(name);
    if (!v)
        return def;
    char *end = NULL;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (errno || end == v)
        return def;
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (end && *end)
        return def;
    size_t sx = (x > (unsigned long long)SIZE_MAX) ? SIZE_MAX : (size_t)x;
    if (min_v <= max_v) {
        if (sx < min_v)
            return min_v;
        if (sx > max_v)
            return max_v;
    }
    return sx;
}

double dsco_env_double(const char *name, double def, double min_v, double max_v) {
    const char *v = env_nonempty(name);
    if (!v)
        return def;
    char *end = NULL;
    errno = 0;
    double x = strtod(v, &end);
    if (errno || end == v || !isfinite(x))
        return def;
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (end && *end)
        return def;
    if (isfinite(min_v) && isfinite(max_v) && min_v <= max_v) {
        if (x < min_v)
            return min_v;
        if (x > max_v)
            return max_v;
    }
    return x;
}

const char *dsco_env_str(const char *name, const char *def) {
    const char *v = env_nonempty(name);
    return v ? v : def;
}
