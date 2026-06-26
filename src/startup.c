#include "startup.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool dsco_profile_parse(const char *name, dsco_profile_t *out) {
    if (!name || !name[0])
        return false;
    dsco_profile_t p;
    if (strcasecmp(name, "full") == 0) {
        p = DSCO_PROFILE_FULL;
    } else if (strcasecmp(name, "lite") == 0) {
        p = DSCO_PROFILE_LITE;
    } else if (strcasecmp(name, "worker") == 0 || strcasecmp(name, "worker-lite") == 0) {
        p = DSCO_PROFILE_WORKER;
    } else {
        return false;
    }
    if (out)
        *out = p;
    return true;
}

const char *dsco_profile_name(dsco_profile_t profile) {
    switch (profile) {
        case DSCO_PROFILE_FULL:
            return "full";
        case DSCO_PROFILE_LITE:
            return "lite";
        case DSCO_PROFILE_WORKER:
            return "worker";
    }
    return "full";
}

const char *dsco_caps_to_string(dsco_caps_t caps, char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return "";
    buf[0] = '\0';
    struct cap_name {
        dsco_caps_t bit;
        const char *name;
    };
    static const struct cap_name names[] = {
        {DSCO_CAP_SECURITY, "security"}, {DSCO_CAP_PROVIDER, "provider"}, {DSCO_CAP_TOOLS, "tools"},
        {DSCO_CAP_TUI, "tui"},           {DSCO_CAP_VOS, "vos"},           {DSCO_CAP_IPC, "ipc"},
        {DSCO_CAP_MCP, "mcp"},           {DSCO_CAP_MEMORY, "memory"},     {DSCO_CAP_ACCEL, "accel"},
        {DSCO_CAP_TRUST, "trust"},       {DSCO_CAP_TRACE, "trace"},
    };
    size_t pos = 0;
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(caps & names[i].bit))
            continue;
        int n = snprintf(buf + pos, buflen - pos, "%s%s", first ? "" : ",", names[i].name);
        if (n < 0)
            break;
        size_t wrote = (size_t)n;
        if (pos + wrote >= buflen) {
            buf[buflen - 1] = '\0';
            break;
        }
        pos += wrote;
        first = false;
    }
    if (first)
        snprintf(buf, buflen, "none");
    return buf;
}
