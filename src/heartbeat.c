#include "heartbeat.h"
#include "watchdog.h"
#include "sealed_store.h"
#include "audit_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#ifdef HAVE_LIBSODIUM
#  include <sodium.h>
#endif

#ifdef HAVE_MBEDTLS
#  include "net_server.h"   /* netsrv_client_post */
#endif

/* ── state ──────────────────────────────────────────────────────────────── */

static pthread_t  s_thread;
static atomic_int s_running  = 0;
static atomic_int s_poke     = 0;
static uint64_t   s_seq      = 0;
static time_t     s_start_ts = 0;

/* ── helpers ────────────────────────────────────────────────────────────── */

static void get_node_id(char *buf, size_t len) {
    if (sealed_store_get("DSCO_NODE_ID", buf, len) > 0 && buf[0]) return;
    if (gethostname(buf, len) != 0) snprintf(buf, len, "unknown");
}

static int get_interval(void) {
    char buf[16];
    if (sealed_store_get("DSCO_BEACON_SECS", buf, sizeof(buf)) > 0 && buf[0])
        return atoi(buf);
    const char *e = getenv("DSCO_BEACON_SECS");
    return (e && e[0]) ? atoi(e) : 60;
}

static void sign_beacon(const char *node, int64_t ts, uint64_t seq,
                         char *sig_hex, size_t sig_hex_len) {
#ifdef HAVE_LIBSODIUM
    uint8_t key[32] = {0};
    char keybuf[512] = {0};
    sealed_store_get("DSCO_NET_AUTH_KEY", keybuf, sizeof(keybuf));
    if (!keybuf[0]) sealed_store_get("DSCO_MESH_SECRET", keybuf, sizeof(keybuf));
    if (keybuf[0])
        crypto_generichash(key, sizeof(key),
                           (const uint8_t *)keybuf, strlen(keybuf), NULL, 0);

    uint8_t hmac[32];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, sizeof(key));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)&ts,  sizeof(ts));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)&seq, sizeof(seq));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)node, strlen(node));
    crypto_auth_hmacsha256_final(&st, hmac);

    /* hex-encode first 16 bytes for a compact sig */
    for (int i = 0; i < 16 && (size_t)(i*2+3) < sig_hex_len; i++)
        snprintf(sig_hex + i*2, 3, "%02x", hmac[i]);
    sodium_memzero(key, sizeof(key));
    sodium_memzero(keybuf, sizeof(keybuf));
#else
    (void)node; (void)ts; (void)seq;
    snprintf(sig_hex, sig_hex_len, "00000000000000000000000000000000");
#endif
}

static void emit_beacon(void) {
    watchdog_ping();

    char node[256];
    get_node_id(node, sizeof(node));
    int64_t ts      = (int64_t)time(NULL);
    uint64_t seq    = s_seq++;
    long uptime     = (long)(ts - s_start_ts);

    char sig[64] = {0};
    sign_beacon(node, ts, seq, sig, sizeof(sig));

    char json[1024];
    snprintf(json, sizeof(json),
        "{\"node\":\"%s\",\"ts\":%lld,\"seq\":%llu,"
        "\"uptime_s\":%ld,\"sig\":\"%s\"}",
        node, (long long)ts, (unsigned long long)seq, uptime, sig);

    audit_log("heartbeat", json);

    /* phone home if URL configured */
    char url[512] = {0};
    sealed_store_get("DSCO_BEACON_URL", url, sizeof(url));
    if (!url[0]) {
        const char *e = getenv("DSCO_BEACON_URL");
        if (e) snprintf(url, sizeof(url), "%s", e);
    }

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    if (url[0]) {
        /* parse host / port / path from URL */
        char host[256] = {0}, path[256] = "/beacon";
        int port = 443;
        int use_tls = 1;
        const char *p = url;
        if (strncmp(p, "https://", 8) == 0) { p += 8; use_tls = 1; port = 443; }
        else if (strncmp(p, "http://", 7) == 0) { p += 7; use_tls = 0; port = 80; }

        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');
        if (colon && (!slash || colon < slash)) {
            snprintf(host, (size_t)(colon - p + 1), "%s", p);
            port = atoi(colon + 1);
        } else if (slash) {
            snprintf(host, (size_t)(slash - p + 1), "%s", p);
        } else {
            snprintf(host, sizeof(host), "%s", p);
        }
        if (slash) snprintf(path, sizeof(path), "%s", slash);

        uint8_t auth_key[32] = {0};
        char akbuf[512] = {0};
        sealed_store_get("DSCO_NET_AUTH_KEY", akbuf, sizeof(akbuf));
        if (akbuf[0])
            crypto_generichash(auth_key, sizeof(auth_key),
                               (const uint8_t *)akbuf, strlen(akbuf), NULL, 0);

        char *resp = netsrv_client_post(host, (uint16_t)port, path, json,
                                        auth_key, sizeof(auth_key), use_tls);
        free(resp);
    }
#endif
}

/* ── thread ─────────────────────────────────────────────────────────────── */

static void *beacon_thread(void *arg) {
    (void)arg;
    while (atomic_load(&s_running)) {
        emit_beacon();
        int secs = get_interval();
        for (int i = 0; i < secs && atomic_load(&s_running); i++) {
            if (atomic_exchange(&s_poke, 0)) break;
            sleep(1);
        }
    }
    return NULL;
}

/* ── public API ─────────────────────────────────────────────────────────── */

void heartbeat_start(void) {
    if (atomic_load(&s_running)) return;
    s_start_ts = time(NULL);
    s_seq      = 0;
    atomic_store(&s_running, 1);
    pthread_create(&s_thread, NULL, beacon_thread, NULL);
}

void heartbeat_stop(void) {
    if (!atomic_load(&s_running)) return;
    atomic_store(&s_running, 0);
    atomic_store(&s_poke, 1);
    pthread_join(s_thread, NULL);
}

bool heartbeat_running(void) {
    return atomic_load(&s_running) != 0;
}

void heartbeat_poke(void) {
    atomic_store(&s_poke, 1);
}
