#include "dsco_c.h"

#include <stdio.h>

static const dsco_c_capabilities_t capabilities = {
    .standard_version = DSCO_C_VERSION,
    .standard_name = DSCO_C_STANDARD_NAME,
    .compiler_name = DSCO_COMPILER_NAME,
    .compiler_version = DSCO_COMPILER_VERSION,
    .mask = (DSCO_C99 ? DSCO_C_FEATURE_C99 : 0) | (DSCO_C11 ? DSCO_C_FEATURE_C11 : 0) |
            (DSCO_C17 ? DSCO_C_FEATURE_C17 : 0) | (DSCO_C23 ? DSCO_C_FEATURE_C23 : 0) |
            (DSCO_C2Y ? DSCO_C_FEATURE_C2Y : 0) | (DSCO_HAS_GENERIC ? DSCO_C_FEATURE_GENERIC : 0) |
            (DSCO_HAS_ATOMICS ? DSCO_C_FEATURE_ATOMICS : 0) |
            (DSCO_HAS_THREADS ? DSCO_C_FEATURE_THREADS : 0) |
            (DSCO_HAS_TYPEOF ? DSCO_C_FEATURE_TYPEOF : 0) |
            (DSCO_HAS_VA_OPT ? DSCO_C_FEATURE_VA_OPT : 0) |
            (DSCO_HAS_ATTRIBUTES ? DSCO_C_FEATURE_ATTRIBUTES : 0) |
            (DSCO_HAS_BITINT ? DSCO_C_FEATURE_BITINT : 0) |
            (DSCO_HAS_AUTO_INFERENCE ? DSCO_C_FEATURE_AUTO : 0) |
            (DSCO_HAS_NULLPTR ? DSCO_C_FEATURE_NULLPTR : 0) |
            (DSCO_HAS_CONSTEXPR ? DSCO_C_FEATURE_CONSTEXPR : 0) |
            (DSCO_HAS_EMBED ? DSCO_C_FEATURE_EMBED : 0) |
            (DSCO_HAS_STDCKDINT ? DSCO_C_FEATURE_CHECKED_INT : 0) |
            (DSCO_HAS_CLEANUP ? DSCO_C_FEATURE_CLEANUP : 0),
    .bitint_maxwidth = DSCO_BITINT_MAXWIDTH,
};

const dsco_c_capabilities_t *dsco_c_capabilities(void) {
    return &capabilities;
}

int dsco_c_format_manifest(char *buffer, size_t capacity) {
    if (!buffer || capacity == 0)
        return -1;
    return snprintf(buffer, capacity,
                    "{\"standard\":\"%s\",\"standard_version\":%ld,"
                    "\"compiler\":\"%s\",\"compiler_version\":%d,"
                    "\"feature_mask\":%llu,\"bitint_maxwidth\":%u,"
                    "\"generic\":%s,\"atomics\":%s,\"threads\":%s,"
                    "\"typeof\":%s,\"va_opt\":%s,\"attributes\":%s,"
                    "\"bitint\":%s,\"auto\":%s,\"nullptr\":%s,"
                    "\"constexpr\":%s,\"embed\":%s,\"checked_int\":%s,"
                    "\"cleanup\":%s}",
                    capabilities.standard_name, capabilities.standard_version,
                    capabilities.compiler_name, capabilities.compiler_version,
                    (unsigned long long)capabilities.mask, capabilities.bitint_maxwidth,
                    DSCO_HAS_GENERIC ? "true" : "false", DSCO_HAS_ATOMICS ? "true" : "false",
                    DSCO_HAS_THREADS ? "true" : "false", DSCO_HAS_TYPEOF ? "true" : "false",
                    DSCO_HAS_VA_OPT ? "true" : "false", DSCO_HAS_ATTRIBUTES ? "true" : "false",
                    DSCO_HAS_BITINT ? "true" : "false", DSCO_HAS_AUTO_INFERENCE ? "true" : "false",
                    DSCO_HAS_NULLPTR ? "true" : "false", DSCO_HAS_CONSTEXPR ? "true" : "false",
                    DSCO_HAS_EMBED ? "true" : "false", DSCO_HAS_STDCKDINT ? "true" : "false",
                    DSCO_HAS_CLEANUP ? "true" : "false");
}
