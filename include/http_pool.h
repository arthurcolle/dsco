#ifndef DSCO_HTTP_POOL_H
#define DSCO_HTTP_POOL_H

/* ── Shared cURL connection pool ──────────────────────────────────────────
 * A process-wide CURLSH share handle that pools DNS resolutions, TLS
 * session tickets, and the connection cache across every easy handle in
 * the process. Without this, each of the ~125 curl_easy_init() sites pays a
 * fresh TCP + TLS handshake (~100-300ms) on every request even though
 * CURLOPT_TCP_KEEPALIVE is set — the keepalive is wasted because the
 * connection dies with the handle.
 *
 * Usage:
 *   CURL *c = curl_easy_init();
 *   dsco_http_pool_apply(c);   // attach the shared cache (no-op on failure)
 *   ... set options, perform ...
 *   curl_easy_cleanup(c);      // connection returns to the shared cache
 *
 * Thread-safe: the share handle uses internal locks, initialized exactly
 * once via pthread_once. Safe to call dsco_http_pool_apply() from worker
 * threads (MCP pool, codex/openrouter cache workers, etc.).
 * ────────────────────────────────────────────────────────────────────── */

#include <curl/curl.h>

/* Attach the process-wide share handle to an easy handle. Idempotent and
 * lazy: initializes the shared pool on first call. On any failure the easy
 * handle is left unchanged (requests still work, just without pooling). */
void dsco_http_pool_apply(CURL *easy);

/* Release the shared pool. Call once at process shutdown, after all easy
 * handles using it have been cleaned up. Safe to call multiple times. */
void dsco_http_pool_cleanup(void);

#endif /* DSCO_HTTP_POOL_H */
