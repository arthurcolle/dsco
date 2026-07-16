#ifndef DSCO_HTTP_POOL_H
#define DSCO_HTTP_POOL_H

/* ── Shared cURL resolver/session cache ───────────────────────────────────
 * A process-wide CURLSH share handle that shares DNS resolutions and TLS
 * session tickets across easy handles. libcurl's connection cache is not
 * safe to share between worker threads, so live HTTP connections are reused
 * only by providers that retain their own easy handle.
 *
 * Usage:
 *   CURL *c = curl_easy_init();
 *   dsco_http_pool_apply(c);   // attach the shared cache (no-op on failure)
 *   ... set options, perform ...
 *   curl_easy_cleanup(c);      // DNS + TLS session cache remain shared
 *
 * Thread-safe: the share handle uses internal locks, initialized exactly
 * once via pthread_once. Safe to call dsco_http_pool_apply() from worker
 * threads (MCP pool, codex/openrouter cache workers, etc.).
 * ────────────────────────────────────────────────────────────────────── */

#include <curl/curl.h>

/* Initialize libcurl's process-wide state exactly once. Call this before
 * starting threads that may create easy handles. Safe to call repeatedly. */
void dsco_http_global_init(void);

/* Release libcurl's process-wide state after every easy/share handle is gone.
 * Normal binaries also register this cleanup at exit for early-return paths. */
void dsco_http_global_cleanup(void);

/* Attach the process-wide share handle to an easy handle. Idempotent and
 * lazy: initializes the shared pool on first call. On any failure the easy
 * handle is left unchanged (requests still work, just without pooling). */
void dsco_http_pool_apply(CURL *easy);

/* Release the shared pool. Call once at process shutdown, after all easy
 * handles using it have been cleaned up. Safe to call multiple times. */
void dsco_http_pool_cleanup(void);

#endif /* DSCO_HTTP_POOL_H */
