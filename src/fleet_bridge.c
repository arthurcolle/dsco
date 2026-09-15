/* fleet_bridge.c — bounded JSON-stdio adapter to the existing fleet engine.
 * No shell interpolation, implicit peer admission, or alternate tool gate. */
#include "fleet_bridge.h"
#include "json_util.h"
#include "process_capture.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fleet_error(char *out, size_t len, const char *message) {
    if (out && len) {
        jbuf_t b;
        jbuf_init(&b, 256);
        jbuf_append(&b, "{\"ok\":false,\"error\":");
        jbuf_append_json_str(&b, message);
        jbuf_append(&b, "}");
        snprintf(out, len, "%s", b.data);
        jbuf_free(&b);
    }
    return false;
}

static bool locate_adapter(char *script, size_t n, char *python, size_t pn) {
    const char *configured = getenv("DSCO_FLEET_ROOT");
    const char *home = getenv("HOME");
    char roots[3][PATH_MAX];
    memset(roots, 0, sizeof(roots));
    if (configured && *configured) {
        if (configured[0] != '/' || strlen(configured) >= sizeof(roots[0])) return false;
        snprintf(roots[0], sizeof(roots[0]), "%s", configured);
    } else {
        if (!home || !*home) return false;
        snprintf(roots[0], sizeof(roots[0]), "%s/dsco/fleet-mesh", home);
        snprintf(roots[1], sizeof(roots[1]), "%s/bridge/fleet-mesh", home);
        snprintf(roots[2], sizeof(roots[2]), "%s/Dsco/fleet-mesh", home);
    }
    for (size_t i = 0; i < 3; i++) {
        if (!roots[i][0]) continue;
        int written = snprintf(script, n, "%s/fleet_harness.py", roots[i]);
        if (written < 0 || (size_t)written >= n || access(script, R_OK)) continue;
        written = snprintf(python, pn, "%s/.venv/bin/python", roots[i]);
        if (written < 0 || (size_t)written >= pn) return false;
        if (access(python, X_OK)) snprintf(python, pn, "/usr/bin/python3");
        return access(python, X_OK) == 0;
    }
    return false;
}

bool fleet_bridge_execute(const char *input, char *result, size_t result_len) {
    if (!result || result_len < 256) return fleet_error(result, result_len, "result buffer too small");
    if (!input || strlen(input) > 64 * 1024 || !json_is_valid_container(input))
        return fleet_error(result, result_len, "fleet request must be valid JSON, at most 64 KiB");
    json_view_t *request = json_view_open(input);
    if (!request) return fleet_error(result, result_len, "fleet request must be an object");
    const char *action = json_view_str(request, "action");
    bool replication = action && !strcmp(action, "fleet/replicate");
    json_view_close(request);
    char script[PATH_MAX], python[PATH_MAX];
    if (!locate_adapter(script, sizeof(script), python, sizeof(python)))
        return fleet_error(result, result_len,
            "fleet_harness.py not found; set DSCO_FLEET_ROOT to the absolute fleet-mesh directory");
    char *argv[] = {python, "-B", script, NULL};
    process_capture_t capture;
    /* Retry/SSH margins are bounded in Python. Replication has a longer whole-
     * operation ceiling; timeout means ambiguous, never automatic replay. */
    bool ok = process_capture_input(python, argv, input, strlen(input),
                                     replication ? 600000 : 300000, 128 * 1024, &capture);
    if (capture.timed_out) {
        process_capture_free(&capture);
        return fleet_error(result, result_len,
            "fleet operation timed out; completion is ambiguous, inspect receipts before retrying");
    }
    if (capture.spawn_error) {
        process_capture_free(&capture);
        return fleet_error(result, result_len, "could not start fleet adapter");
    }
    if (capture.truncated || capture.length >= result_len) {
        process_capture_free(&capture);
        return fleet_error(result, result_len,
            "fleet response too large; operation may have completed, inspect receipts before retrying");
    }
    if (!capture.output || !json_is_valid_container(capture.output)) {
        process_capture_free(&capture);
        return fleet_error(result, result_len,
            "invalid fleet adapter response; inspect receipts before retrying");
    }
    json_view_t *response = json_view_open(capture.output);
    if (!response) ok = false;
    else if (!json_view_bool(response, "ok", false)) ok = false;
    json_view_close(response);
    snprintf(result, result_len, "%s", capture.output);
    process_capture_free(&capture);
    return ok;
}
