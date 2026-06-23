#ifndef DSCO_TOOLMGMT_H
#define DSCO_TOOLMGMT_H

#include <stdbool.h>
#include <stddef.h>

/* ── Tool Management API client ───────────────────────────────────────────
 * Drives an external HTTP "Tool Management API" (the dsco-autobot registry)
 * so dsco can discover, execute, and fan out across many remote tools.
 *
 * Configuration is resolved at runtime, in priority order:
 *   base URL : explicit override → TOOLS_API_URL → TOOL_MANAGEMENT_API_URL
 *              → TOOLMGMT_API_URL_DEFAULT
 *   token    : explicit override → TOOLS_API_TOKEN → AUTH_TOKEN → none
 * These env names match the existing `do` shell client so the two stay in
 * sync. Tools are also registered dynamically: the remote catalog is fetched
 * and each entry is surfaced as a dsco external tool the agent can call. */

/* Resolved configuration (read-only views; do not free). */
const char *toolmgmt_base_url(void);
const char *toolmgmt_token(void);     /* may return NULL when unauthenticated */

/* Explicit overrides (e.g. from --tm-url / --tm-token flags). Copied. */
void toolmgmt_set_base_url(const char *url);
void toolmgmt_set_token(const char *token);

/* Low-level request. `path` is appended to the base URL (e.g. "/api/v1/...").
 * Returns the HTTP status code (-1 on transport failure). On success *out is
 * set to a malloc'd, NUL-terminated response body the caller must free; on
 * failure *out may still hold an error body (free it if non-NULL). */
long toolmgmt_request(const char *method, const char *path,
                      const char *body, char **out);

/* High-level operations. Each returns a malloc'd response body (caller frees)
 * or NULL on transport/HTTP error. */
char *toolmgmt_list_tools(int limit);                 /* GET catalog */
char *toolmgmt_list_tools_paginated(int offset, int limit); /* GET catalog page */
char *toolmgmt_list_tools_all(int page_limit);        /* GET full catalog when API supports pagination */
char *toolmgmt_execute(const char *tool,
                       const char *args_json,         /* JSON object, may be NULL → {} */
                       int timeout_ms);               /* 0 = server default */
char *toolmgmt_batch(const char *calls_json,          /* JSON array of {tool,inputs} */
                     bool parallel);
char *toolmgmt_recommend(const char *intent,          /* may be NULL */
                         const char *query,
                         int max_steps);               /* <=0 → default */

/* Parallel fan-out: execute N tool calls concurrently (one curl handle per
 * worker thread, bounded by max_concurrency). Fills result/status per call.
 * Returns the number of calls that returned HTTP 2xx. */
typedef struct {
    const char *tool;       /* in  */
    const char *args_json;  /* in  : JSON object, may be NULL */
    int         timeout_ms; /* in  : 0 = default */
    char       *result;     /* out : malloc'd body, caller frees */
    long        status;     /* out : HTTP status (-1 transport error) */
} tm_call_t;

int toolmgmt_parallel(tm_call_t *calls, int n, int max_concurrency);

/* Fetch the remote catalog and register every tool as a dsco external tool,
 * routing execution through /api/v1/execute. Returns the number registered,
 * or -1 if the catalog could not be fetched. Safe to call once at startup. */
int toolmgmt_register_tools(void);

/* CLI entry point for the `dsco tools …` subcommand. Returns a process exit
 * code. Recognizes: list, run, batch, plan, register, -h/--help, plus global
 * --tm-url / --tm-token overrides. */
int toolmgmt_cli(int argc, char **argv);

#endif /* DSCO_TOOLMGMT_H */
