#include "http_pool.h"

#include <pthread.h>
#include <stdbool.h>

/* ── Process-wide cURL share handle ───────────────────────────────────────
 * Pools DNS and TLS session cache across all easy handles. The share handle
 * is reference-protected by per-data-type locks so it is safe to use from
 * multiple worker threads concurrently.
 *
 * NOTE: libcurl explicitly documents the connection pool as NOT thread-safe
 * even with lock callbacks. We deliberately do NOT share CURL_LOCK_DATA_CONNECT.
 * ────────────────────────────────────────────────────────────────────── */

static CURLSH *g_share = NULL;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static pthread_mutex_t g_lock_dns = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_ssl = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock_other = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *lock_for(curl_lock_data data) {
    switch (data) {
    case CURL_LOCK_DATA_DNS:
        return &g_lock_dns;
    case CURL_LOCK_DATA_SSL_SESSION:
        return &g_lock_ssl;
    default:
        return &g_lock_other;
    }
}

static void share_lock_cb(CURL *handle, curl_lock_data data, curl_lock_access access,
                          void *userptr) {
    (void)handle;
    (void)access;
    (void)userptr;
    pthread_mutex_lock(lock_for(data));
}

static void share_unlock_cb(CURL *handle, curl_lock_data data, void *userptr) {
    (void)handle;
    (void)userptr;
    pthread_mutex_unlock(lock_for(data));
}

static void share_init_once(void) {
    CURLSH *sh = curl_share_init();
    if (!sh)
        return;

    bool ok = true;
    ok &= (curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, share_lock_cb) == CURLSHE_OK);
    ok &= (curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, share_unlock_cb) == CURLSHE_OK);
    /* DNS + TLS session only. Connection pool is excluded per libcurl docs. */
    ok &= (curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) == CURLSHE_OK);
    ok &= (curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION) == CURLSHE_OK);

    if (!ok) {
        curl_share_cleanup(sh);
        return;
    }
    g_share = sh;
}

void dsco_http_pool_apply(CURL *easy) {
    if (!easy)
        return;
    pthread_once(&g_once, share_init_once);
    if (g_share)
        curl_easy_setopt(easy, CURLOPT_SHARE, g_share);
}

void dsco_http_pool_cleanup(void) {
    if (g_share) {
        curl_share_cleanup(g_share);
        g_share = NULL;
    }
}
