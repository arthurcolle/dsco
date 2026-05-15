#define _DARWIN_C_SOURCE 1
#include "env_guard.h"
#include "audit_log.h"
#include "tamper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* ── dangerous environment variables ───────────────────────────────────── */

static const char *s_dangerous[] = {
    "LD_PRELOAD",
    "LD_AUDIT",
    "LD_DEBUG",
    "LD_PROFILE",
    "DYLD_INSERT_LIBRARIES",
    "DYLD_FORCE_FLAT_NAMESPACE",
    "DYLD_IMAGE_SUFFIX",
    "DYLD_FRAMEWORK_PATH",
    "DYLD_LIBRARY_PATH",
    "MALLOC_CHECK_",
    "MALLOC_PERTURB_",
    "GLIBC_TUNABLES",
    "LIBC_FATAL_STDERR_",
    NULL
};

/* ── baseline dylib count captured at env_guard_init() ─────────────────── */

static uint32_t s_baseline_img_count = 0;

/* ── implementation ─────────────────────────────────────────────────────── */

uint32_t env_guard_init(void) {
    uint32_t flags = 0;
    char report[2048] = {0};
    int  roff = 0;

    /* 1. strip dangerous env vars and record which were set */
    for (int i = 0; s_dangerous[i]; i++) {
        const char *val = getenv(s_dangerous[i]);
        if (val && val[0]) {
            flags |= ENVG_PRELOAD_FOUND;
            roff += snprintf(report + roff, sizeof(report) - (size_t)roff,
                             "%s=%s; ", s_dangerous[i], val);
            unsetenv(s_dangerous[i]);
        }
    }

    if (flags & ENVG_PRELOAD_FOUND) {
        char msg[2200];
        snprintf(msg, sizeof(msg), "stripped dangerous env vars: %s", report);
        audit_log("env_guard", msg);
        fprintf(stderr, "[env_guard] WARNING: %s\n", msg);
    }

#ifdef __APPLE__
    /* 2. capture baseline dylib count after startup is done */
    s_baseline_img_count = _dyld_image_count();

    /* 3. scan images for suspicious paths
     * Anything not under /usr, /System, /Library, /opt/homebrew, the app
     * bundle path, or the executable path is considered suspicious. */
    const char *exe = NULL;
    {
        static char exepath[4096];
        uint32_t sz = sizeof(exepath);
        extern int _NSGetExecutablePath(char *, uint32_t *);
        if (_NSGetExecutablePath(exepath, &sz) == 0) exe = exepath;
    }

    for (uint32_t i = 0; i < s_baseline_img_count; i++) {
        const char *img = _dyld_get_image_name(i);
        if (!img) continue;
        /* known-safe prefixes */
        if (strncmp(img, "/usr/",    5) == 0) continue;
        if (strncmp(img, "/System/", 8) == 0) continue;
        if (strncmp(img, "/Library/",9) == 0) continue;
        if (strncmp(img, "/opt/homebrew/", 14) == 0) continue;
        if (strncmp(img, "/usr/local/", 11) == 0) continue;
        if (exe && strncmp(img, exe, strlen(exe)) == 0) continue;
        /* anything else — could be injected */
        flags |= ENVG_INJECTED_DYLIB;
        char msg[1024];
        snprintf(msg, sizeof(msg), "unexpected dylib at startup: %s", img);
        audit_log("env_guard", msg);
        fprintf(stderr, "[env_guard] WARNING: %s\n", msg);
    }
#endif

    return flags;
}

bool env_guard_check_dylibs(void) {
#ifdef __APPLE__
    uint32_t current = _dyld_image_count();
    if (current <= s_baseline_img_count) return true;

    /* new images appeared — enumerate them */
    for (uint32_t i = s_baseline_img_count; i < current; i++) {
        const char *img = _dyld_get_image_name(i);
        char msg[1024];
        snprintf(msg, sizeof(msg), "dylib injected after init: %s",
                 img ? img : "(null)");
        audit_log("env_guard", msg);
        fprintf(stderr, "[env_guard] ALERT: %s\n", msg);
    }
    s_baseline_img_count = current;
    return false;
#else
    return true;
#endif
}
