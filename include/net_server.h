#ifndef DSCO_NET_SERVER_H
#define DSCO_NET_SERVER_H

/* ── Secure HTTP-like JSON server ─────────────────────────────────────────
 *
 * Transport : POSIX TCP via mbedTLS net_sockets helpers
 * TLS       : mbedTLS 3.x (optional; falls back to plaintext if no cert)
 * Auth      : libsodium HMAC-SHA512256 on request body via X-DSCO-Auth header
 *
 * Protocol  : HTTP/1.0 subset — GET/POST, Content-Length, one header per line
 * Default port: 7547
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETSRV_DEFAULT_PORT   7547
#define NETSRV_MAX_HANDLERS   32
#define NETSRV_MAX_BODY       (256 * 1024)
#define NETSRV_MAX_HDRLINE    1024

typedef struct {
    const char *method;
    const char *path;
    const char *body;
    size_t      body_len;
    const char *auth_token;  /* value of X-DSCO-Auth header, or NULL */
} netsrv_request_t;

typedef struct {
    int   status;
    char *body;
    bool  heap_body;   /* if true, body is freed after response is sent */
} netsrv_response_t;

typedef netsrv_response_t (*netsrv_handler_fn)(const netsrv_request_t *req,
                                               void *ctx);

typedef struct dsco_net_server dsco_net_server_t;

/* Create a server instance.  cert_pem_path / key_pem_path may be NULL to
 * disable TLS; use_tls is ignored in that case. */
dsco_net_server_t *netsrv_create(uint16_t port, bool use_tls,
                                 const char *cert_pem_path,
                                 const char *key_pem_path);
void netsrv_destroy(dsco_net_server_t *s);

/* Register a handler for METHOD /path.  Last-registered wins on conflicts. */
bool netsrv_route(dsco_net_server_t *s, const char *method, const char *path,
                  netsrv_handler_fn fn, void *ctx);

/* Optional: require HMAC-SHA512256(key, body) on every request. */
void netsrv_set_auth_key(dsco_net_server_t *s,
                         const uint8_t *key, size_t key_len);

bool     netsrv_start(dsco_net_server_t *s);
void     netsrv_stop(dsco_net_server_t *s);
uint16_t netsrv_port(dsco_net_server_t *s);

/* Generate a self-signed EC P-256 cert + key and write PEM files to disk.
 * Validity: 2025-01-01 → 2035-01-01.  cn = Common Name (e.g. "dsco-node"). */
bool netsrv_gen_tls_cert(const char *cert_path, const char *key_path,
                          const char *cn);

/* Client helper: POST JSON body to a remote dsco server.
 * Returns malloc'd response body on success, NULL on failure.
 * If auth_key != NULL it adds X-DSCO-Auth with HMAC over the body.
 * If use_tls, skips server cert verification (TOFU model). */
char *netsrv_client_post(const char *host, uint16_t port, const char *path,
                         const char *json_body,
                         const uint8_t *auth_key, size_t auth_key_len,
                         bool use_tls);

#endif /* DSCO_NET_SERVER_H */
