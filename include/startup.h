#ifndef DSCO_STARTUP_H
#define DSCO_STARTUP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    DSCO_PROFILE_FULL = 0,
    DSCO_PROFILE_LITE,
    DSCO_PROFILE_WORKER,
} dsco_profile_t;

typedef uint32_t dsco_caps_t;

enum {
    DSCO_CAP_SECURITY = 1u << 0,
    DSCO_CAP_PROVIDER = 1u << 1,
    DSCO_CAP_TOOLS    = 1u << 2,
    DSCO_CAP_TUI      = 1u << 3,
    DSCO_CAP_VOS      = 1u << 4,
    DSCO_CAP_IPC      = 1u << 5,
    DSCO_CAP_MCP      = 1u << 6,
    DSCO_CAP_MEMORY   = 1u << 7,
    DSCO_CAP_ACCEL    = 1u << 8,
    DSCO_CAP_TRUST    = 1u << 9,
    DSCO_CAP_TRACE    = 1u << 10,
};

#define DSCO_CAP_FULL (DSCO_CAP_SECURITY | DSCO_CAP_PROVIDER | DSCO_CAP_TOOLS | \
                       DSCO_CAP_TUI | DSCO_CAP_VOS | DSCO_CAP_IPC | \
                       DSCO_CAP_MCP | DSCO_CAP_MEMORY | DSCO_CAP_ACCEL | \
                       DSCO_CAP_TRUST | DSCO_CAP_TRACE)

bool dsco_profile_parse(const char *name, dsco_profile_t *out);
const char *dsco_profile_name(dsco_profile_t profile);
const char *dsco_caps_to_string(dsco_caps_t caps, char *buf, size_t buflen);

void dsco_startup_init(dsco_profile_t profile, dsco_caps_t caps);

#endif /* DSCO_STARTUP_H */
