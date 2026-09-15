#include "net_server.h"
#include <sodium.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/version.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* mbedTLS 2.x compatibility (ubuntu 24.04 ships 2.28): map the 3.x-only
 * spellings onto their 2.x equivalents. */
#if MBEDTLS_VERSION_NUMBER < 0x03000000
#include <mbedtls/bignum.h>
#define mbedtls_ssl_conf_min_tls_version(conf, ver)                                                \
    mbedtls_ssl_conf_min_version((conf), MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3)
#define mbedtls_pk_parse_keyfile(ctx, path, pwd, f_rng, p_rng)                                     \
    mbedtls_pk_parse_keyfile((ctx), (path), (pwd))
static int netsrv_compat_set_serial_raw(mbedtls_x509write_cert *ctx, unsigned char *serial,
                                        size_t len) {
    mbedtls_mpi mpi;
    mbedtls_mpi_init(&mpi);
    int r = mbedtls_mpi_read_binary(&mpi, serial, len);
    if (r == 0)
        r = mbedtls_x509write_crt_set_serial(ctx, &mpi);
    mbedtls_mpi_free(&mpi);
    return r;
}
#define mbedtls_x509write_crt_set_serial_raw netsrv_compat_set_serial_raw
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

static void netsrv_tune_socket(int fd, bool listener) {
    if (fd < 0)
        return;

    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0)
        (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);

    int one = 1;
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (!listener) {
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_NODELAY
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    }
}

/* ── Route table ──────────────────────────────────────────────────────── */
typedef struct {
    char method[8];
    char path[128];
    netsrv_handler_fn fn;       /* buffered handler (NULL if streaming) */
    netsrv_stream_fn stream_fn; /* streaming handler (NULL if buffered) */
    void *ctx;
} route_t;

/* ── Server struct ────────────────────────────────────────────────────── */
struct dsco_net_server {
    uint16_t port;
    bool use_tls;
    char cert_path[512];
    char key_path[512];

    /* mbedTLS shared context (TLS mode) */
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt srvcert;
    mbedtls_pk_context pkey;
    mbedtls_ssl_config conf;
    bool tls_ready;

    mbedtls_net_context listen_fd;

    pthread_t accept_thread;
    volatile bool running;

    route_t routes[NETSRV_MAX_HANDLERS];
    int route_count;

    uint8_t auth_key[crypto_auth_hmacsha512256_KEYBYTES];
    bool auth_enabled;
    char bind_host[256];
    pthread_mutex_t replay_mu;
    struct { uint64_t ts; char nonce[65]; } replay[1024];
};

/* ── Per-connection handler arg ───────────────────────────────────────── */
typedef struct {
    dsco_net_server_t *srv;
    mbedtls_net_context client_fd;
    mbedtls_ssl_context ssl;
} conn_arg_t;

/* ── HTTP helpers ─────────────────────────────────────────────────────── */
static void trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' '))
        s[--len] = '\0';
}

/* Read one CRLF-terminated line from mbedTLS (plain or TLS) */
static bool read_line(dsco_net_server_t *srv, mbedtls_ssl_context *ssl, mbedtls_net_context *fd,
                      char *buf, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        unsigned char ch;
        int ret;
        if (srv->tls_ready && ssl)
            ret = mbedtls_ssl_read(ssl, &ch, 1);
        else
            ret = mbedtls_net_recv(fd, &ch, 1);
        if (ret <= 0)
            return false;
        buf[pos++] = (char)ch;
        if (ch == '\n')
            break;
    }
    buf[pos] = '\0';
    trim_right(buf);
    return true;
}

/* Read exactly len bytes */
static bool read_exact(dsco_net_server_t *srv, mbedtls_ssl_context *ssl, mbedtls_net_context *fd,
                       void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        int ret;
        if (srv->tls_ready && ssl)
            ret = mbedtls_ssl_read(ssl, p, len);
        else
            ret = mbedtls_net_recv(fd, p, len);
        if (ret <= 0)
            return false;
        p += ret;
        len -= (size_t)ret;
    }
    return true;
}

/* Write all bytes */
static bool write_all(dsco_net_server_t *srv, mbedtls_ssl_context *ssl, mbedtls_net_context *fd,
                      const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        int ret;
        if (srv->tls_ready && ssl)
            ret = mbedtls_ssl_write(ssl, p, len);
        else
            ret = mbedtls_net_send(fd, p, len);
        if (ret <= 0)
            return false;
        p += ret;
        len -= (size_t)ret;
    }
    return true;
}

/* ── Streaming responder ──────────────────────────────────────────────────
 * Handed to a streaming route so it can write the response incrementally over
 * the live connection.  ssl is NULL in plaintext mode. */
struct netsrv_stream {
    dsco_net_server_t *srv;
    mbedtls_ssl_context *ssl;
    mbedtls_net_context *fd;
};

int netsrv_stream_send(netsrv_stream_t *s, const char *buf, size_t len) {
    if (!s || !buf)
        return -1;
    return write_all(s->srv, s->ssl, s->fd, buf, len) ? 0 : -1;
}

static bool client_write_all(bool use_tls, mbedtls_ssl_context *ssl, mbedtls_net_context *fd,
                             const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        int ret = use_tls ? mbedtls_ssl_write(ssl, p, len) : mbedtls_net_send(fd, p, len);
        if (ret <= 0)
            return false;
        p += ret;
        len -= (size_t)ret;
    }
    return true;
}

/* ── HMAC auth ─────────────────────────────────────────────────────────── */
#define NETSRV_AUTH_WINDOW 300
#define NETSRV_NONCE_HEX 32
static bool hex_decode_key(const char *in, uint8_t out[crypto_auth_hmacsha512256_KEYBYTES]) {
    if (!in || strlen(in) != crypto_auth_hmacsha512256_KEYBYTES * 2) return false;
    for (size_t i = 0; i < crypto_auth_hmacsha512256_KEYBYTES; i++) {
        int a = isdigit((unsigned char)in[i*2]) ? in[i*2]-'0' :
                (in[i*2] >= 'a' && in[i*2] <= 'f') ? in[i*2]-'a'+10 :
                (in[i*2] >= 'A' && in[i*2] <= 'F') ? in[i*2]-'A'+10 : -1;
        int b = isdigit((unsigned char)in[i*2+1]) ? in[i*2+1]-'0' :
                (in[i*2+1] >= 'a' && in[i*2+1] <= 'f') ? in[i*2+1]-'a'+10 :
                (in[i*2+1] >= 'A' && in[i*2+1] <= 'F') ? in[i*2+1]-'A'+10 : -1;
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}
static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i=0; i<n; i++) { out[i*2]=h[in[i]>>4]; out[i*2+1]=h[in[i]&15]; }
    out[n*2]='\0';
}
static uint64_t netsrv_now(void) { return (uint64_t)time(NULL); }
static bool replay_seen(dsco_net_server_t *s, uint64_t ts, const char *nonce) {
    /* Never evict an unexpired nonce: saturation fails closed, not replay-open. */
    bool reject = false;
    size_t slot = sizeof(s->replay) / sizeof(s->replay[0]);
    uint64_t now = netsrv_now();
    pthread_mutex_lock(&s->replay_mu);
    for (size_t i = 0; i < sizeof(s->replay) / sizeof(s->replay[0]); i++) {
        if (!s->replay[i].ts || now > s->replay[i].ts + NETSRV_AUTH_WINDOW) {
            if (slot == sizeof(s->replay) / sizeof(s->replay[0])) slot = i;
        } else if (!strcmp(s->replay[i].nonce, nonce)) { reject = true; break; }
    }
    if (!reject) {
        if (slot == sizeof(s->replay) / sizeof(s->replay[0])) reject = true;
        else {
            s->replay[slot].ts = ts;
            snprintf(s->replay[slot].nonce, sizeof(s->replay[slot].nonce), "%s", nonce);
        }
    }
    pthread_mutex_unlock(&s->replay_mu);
    return reject;
}
static bool verify_auth(dsco_net_server_t *s, const char *token, const char *method,
                        const char *path, const char *body, size_t body_len) {
    if (!s->auth_enabled || !token || strncmp(token, "v1:", 3) != 0) return false;
    char tsbuf[32], nonce[65], machex[crypto_auth_hmacsha512256_BYTES*2+1];
    if (sscanf(token+3, "%31[^:]:%64[^:]:%64s", tsbuf, nonce, machex) != 3 || strlen(nonce) != NETSRV_NONCE_HEX || strlen(machex) != 64) return false;
    for (size_t i = 0; tsbuf[i]; i++) if (!isdigit((unsigned char)tsbuf[i])) return false;
    char *end=NULL; errno=0; unsigned long long t=strtoull(tsbuf,&end,10);
    uint64_t now=netsrv_now(); if (errno || !end || *end || t > now + NETSRV_AUTH_WINDOW || (t < now && now - t > NETSRV_AUTH_WINDOW)) return false;
    for (size_t i=0;i<strlen(nonce);i++) if (!isxdigit((unsigned char)nonce[i])) return false;
    size_t n = strlen(tsbuf)+1+strlen(nonce)+1+strlen(method)+1+strlen(path)+1+body_len;
    char *msg=malloc(n + 1); if (!msg) return false;
    int used=snprintf(msg,n+1,"%s\n%s\n%s\n%s\n",tsbuf,nonce,method,path);
    if (used < 0 || (size_t)used + body_len != n) { free(msg); return false; }
    if (body_len) memcpy(msg+used,body,body_len);
    uint8_t mac[crypto_auth_hmacsha512256_BYTES], supplied[sizeof(mac)];
    if (!hex_decode_key(machex, supplied)) { free(msg); return false; }
    crypto_auth_hmacsha512256(mac,(const uint8_t*)msg,n,s->auth_key); free(msg);
    bool ok = sodium_memcmp(mac,supplied,sizeof(mac)) == 0;
    if (ok && replay_seen(s,(uint64_t)t,nonce)) ok=false;
    sodium_memzero(supplied,sizeof(supplied)); sodium_memzero(mac,sizeof(mac));
    return ok;
}

/* ── Connection handler thread ─────────────────────────────────────────── */
static void *handle_conn(void *arg) {
    conn_arg_t *ca = arg;
    dsco_net_server_t *srv = ca->srv;

    /* TLS handshake if applicable */
    if (srv->tls_ready) {
        int ret;
        while ((ret = mbedtls_ssl_handshake(&ca->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                goto cleanup;
        }
    }

    /* ── Parse HTTP/1.0 request line ───────────────────────────────── */
    char line[NETSRV_MAX_HDRLINE];
    if (!read_line(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, line, sizeof(line)))
        goto cleanup;

    char method[16] = {0}, path[256] = {0};
    if (sscanf(line, "%15s %255s", method, path) != 2)
        goto cleanup;

    /* ── Parse headers until blank line ────────────────────────────── */
    long content_length = 0;
    char auth_token[256] = {0};

    while (read_line(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, line, sizeof(line))) {
        if (line[0] == '\0')
            break; /* blank line = end of headers */
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *ep = NULL; errno = 0;
            content_length = strtol(line + 15, &ep, 10);
            while (ep && isspace((unsigned char)*ep)) ep++;
            if (errno || !ep || ep == line + 15 || *ep || content_length < 0 || content_length > NETSRV_MAX_BODY) {
                const char *bad = "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                write_all(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, bad, strlen(bad));
                goto cleanup;
            }
        } else if (strncasecmp(line, "X-DSCO-Auth:", 12) == 0) {
            const char *v = line + 12;
            while (*v == ' ')
                v++;
            snprintf(auth_token, sizeof(auth_token), "%s", v);
        }
    }

    /* ── Read body ──────────────────────────────────────────────────── */
    char *body = NULL;
    if (content_length > 0 && content_length <= NETSRV_MAX_BODY) {
        body = malloc((size_t)content_length + 1);
        if (!body)
            goto cleanup;
        if (!read_exact(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, body,
                        (size_t)content_length)) {
            free(body);
            goto cleanup;
        }
        body[content_length] = '\0';
    }

    /* ── Auth check ─────────────────────────────────────────────────── */
    const char *tok = auth_token[0] ? auth_token : NULL;
    bool public_health = (strcasecmp(method, "GET") == 0 && strcmp(path, "/health") == 0);
    if (!public_health && !verify_auth(srv, tok, method, path, body ? body : "", (size_t)content_length)) {
        const char *resp = "HTTP/1.0 401 Unauthorized\r\n"
                           "Content-Length: 0\r\n\r\n";
        write_all(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, resp, strlen(resp));
        free(body);
        goto cleanup;
    }

    /* ── Route dispatch ─────────────────────────────────────────────── */
    netsrv_response_t result = {
        .status = 404, .body = (char *)"{\"error\":\"not found\"}", .heap_body = false};

    for (int i = 0; i < srv->route_count; i++) {
        route_t *r = &srv->routes[i];
        if (strcasecmp(r->method, method) == 0 && strcmp(r->path, path) == 0) {
            netsrv_request_t req = {
                .method = method,
                .path = path,
                .body = body ? body : "",
                .body_len = (size_t)(body ? content_length : 0),
                .auth_token = tok,
            };
            if (r->stream_fn) {
                /* Streaming route owns the socket: it writes status + headers +
                 * body itself.  Skip the buffered response writer entirely. */
                netsrv_stream_t st = {
                    .srv = srv, .ssl = srv->tls_ready ? &ca->ssl : NULL, .fd = &ca->client_fd};
                r->stream_fn(&req, &st, r->ctx);
                free(body);
                goto cleanup;
            }
            result = r->fn(&req, r->ctx);
            break;
        }
    }

    /* ── Write response ─────────────────────────────────────────────── */
    const char *rbody = result.body ? result.body : "";
    size_t rbody_len = strlen(rbody);

    char status_line[64];
    const char *phrase = result.status == 200   ? "OK"
                         : result.status == 201 ? "Created"
                         : result.status == 400 ? "Bad Request"
                         : result.status == 401 ? "Unauthorized"
                         : result.status == 404 ? "Not Found"
                                                : "Error";
    snprintf(status_line, sizeof(status_line), "HTTP/1.0 %d %s\r\n", result.status, phrase);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n",
             rbody_len);

    write_all(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, status_line,
              strlen(status_line));
    write_all(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, hdr, strlen(hdr));
    write_all(srv, srv->tls_ready ? &ca->ssl : NULL, &ca->client_fd, rbody, rbody_len);

    if (result.heap_body)
        free(result.body);
    free(body);

cleanup:
    if (srv->tls_ready)
        mbedtls_ssl_close_notify(&ca->ssl);
    mbedtls_ssl_free(&ca->ssl);
    mbedtls_net_free(&ca->client_fd);
    free(ca);
    return NULL;
}

/* ── Accept loop ───────────────────────────────────────────────────────── */
static void *accept_loop(void *arg) {
    dsco_net_server_t *srv = arg;

    while (srv->running) {
        conn_arg_t *ca = calloc(1, sizeof(*ca));
        if (!ca)
            break;
        ca->srv = srv;
        mbedtls_net_init(&ca->client_fd);

        int ret = mbedtls_net_accept(&srv->listen_fd, &ca->client_fd, NULL, 0, NULL);
        if (ret != 0) {
            free(ca);
            break;
        }
        netsrv_tune_socket(ca->client_fd.fd, false);

        if (srv->tls_ready) {
            mbedtls_ssl_init(&ca->ssl);
            mbedtls_ssl_setup(&ca->ssl, &srv->conf);
            mbedtls_ssl_set_bio(&ca->ssl, &ca->client_fd, mbedtls_net_send, mbedtls_net_recv, NULL);
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&(pthread_t){0}, &attr, handle_conn, ca);
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

dsco_net_server_t *netsrv_create(uint16_t port, bool use_tls, const char *cert_pem_path,
                                 const char *key_pem_path) {
    if (sodium_init() < 0)
        return NULL;

    dsco_net_server_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->port = port;
    s->use_tls = use_tls;
    snprintf(s->bind_host, sizeof(s->bind_host), "%s", getenv("DSCO_NET_BIND") ? getenv("DSCO_NET_BIND") : "127.0.0.1");
    pthread_mutex_init(&s->replay_mu, NULL);
    const char *envkey = getenv("DSCO_NET_AUTH_KEY");
    if (envkey && hex_decode_key(envkey, s->auth_key)) s->auth_enabled = true;
    if (cert_pem_path)
        snprintf(s->cert_path, sizeof(s->cert_path), "%s", cert_pem_path);
    if (key_pem_path)
        snprintf(s->key_path, sizeof(s->key_path), "%s", key_pem_path);

    mbedtls_net_init(&s->listen_fd);
    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    mbedtls_x509_crt_init(&s->srvcert);
    mbedtls_pk_init(&s->pkey);
    mbedtls_ssl_config_init(&s->conf);

    const char *pers = "dsco-netsrv";
    mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func, &s->entropy,
                          (const unsigned char *)pers, strlen(pers));
    return s;
}

void netsrv_destroy(dsco_net_server_t *s) {
    if (!s)
        return;
    netsrv_stop(s);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->srvcert);
    mbedtls_pk_free(&s->pkey);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_entropy_free(&s->entropy);
    mbedtls_net_free(&s->listen_fd);
    sodium_memzero(s->auth_key, sizeof(s->auth_key));
    pthread_mutex_destroy(&s->replay_mu);
    free(s);
}

bool netsrv_route(dsco_net_server_t *s, const char *method, const char *path, netsrv_handler_fn fn,
                  void *ctx) {
    if (!s || s->route_count >= NETSRV_MAX_HANDLERS)
        return false;
    route_t *r = &s->routes[s->route_count++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->fn = fn;
    r->stream_fn = NULL;
    r->ctx = ctx;
    return true;
}

bool netsrv_route_stream(dsco_net_server_t *s, const char *method, const char *path,
                         netsrv_stream_fn fn, void *ctx) {
    if (!s || s->route_count >= NETSRV_MAX_HANDLERS)
        return false;
    route_t *r = &s->routes[s->route_count++];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->fn = NULL;
    r->stream_fn = fn;
    r->ctx = ctx;
    return true;
}

void netsrv_set_auth_key(dsco_net_server_t *s, const uint8_t *key, size_t key_len) {
    if (!s)
        return;
    if (!key || key_len != crypto_auth_hmacsha512256_KEYBYTES) { s->auth_enabled = false; return; }
    memcpy(s->auth_key, key, crypto_auth_hmacsha512256_KEYBYTES);
    s->auth_enabled = true;
}

bool netsrv_start(dsco_net_server_t *s) {
    const char *ak = getenv("DSCO_NET_AUTH_KEY");
    if (!s->auth_enabled && ak) s->auth_enabled = hex_decode_key(ak, s->auth_key);
    bool loopback = strcmp(s->bind_host,"localhost")==0 || strcmp(s->bind_host,"::1")==0 ||
                    (strncmp(s->bind_host,"127.",4)==0);
    if (!loopback && !s->auth_enabled) { fprintf(stderr,"[netsrv] non-loopback bind requires DSCO_NET_AUTH_KEY (64 hex chars)\n"); return false; }
    if (s->use_tls) {
        if (!s->cert_path[0] || !s->key_path[0]) { fprintf(stderr,"[netsrv] TLS requires certificate and key\n"); return false; }
        int r;
        r = mbedtls_x509_crt_parse_file(&s->srvcert, s->cert_path);
        if (r != 0) {
            char errbuf[128];
            mbedtls_strerror(r, errbuf, sizeof(errbuf));
            fprintf(stderr, "[netsrv] cert parse failed: %s\n", errbuf);
            return false;
        }
        r = mbedtls_pk_parse_keyfile(&s->pkey, s->key_path, NULL, mbedtls_ctr_drbg_random,
                                     &s->ctr_drbg);
        if (r != 0) {
            char errbuf[128];
            mbedtls_strerror(r, errbuf, sizeof(errbuf));
            fprintf(stderr, "[netsrv] key parse failed: %s\n", errbuf);
            return false;
        }
        r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (r != 0)
            return false;

        mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);
        mbedtls_ssl_conf_ca_chain(&s->conf, s->srvcert.next, NULL);
        r = mbedtls_ssl_conf_own_cert(&s->conf, &s->srvcert, &s->pkey);
        if (r != 0)
            return false;

        mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        s->tls_ready = true;
    }

/* Bind the configured port; if it's already taken (e.g. another dsco
     * instance owns it), walk a small range of fallbacks rather than failing
     * loudly. Running a second instance is normal, so a wedged default port
     * shouldn't dump an mbedtls error into the TUI. */
    enum { NETSRV_BIND_TRIES = 8 };
    uint16_t want = s->port;
    int r = MBEDTLS_ERR_NET_BIND_FAILED;
    for (int i = 0; i < NETSRV_BIND_TRIES; i++) {
        uint16_t try_port = (uint16_t)(want + i);
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", try_port);
        r = mbedtls_net_bind(&s->listen_fd, s->bind_host, port_str, MBEDTLS_NET_PROTO_TCP);
        if (r == 0) {
            s->port = try_port;
            break;
        }
        mbedtls_net_free(&s->listen_fd);
        mbedtls_net_init(&s->listen_fd);
    }
    if (r != 0) {
        /* Every candidate was taken — run without the HTTP API, quietly. */
        fprintf(stderr,
                "[netsrv] :%u-%u all in use; HTTP API disabled "
                "(another dsco instance likely owns it)\n",
                want, (uint16_t)(want + NETSRV_BIND_TRIES - 1));
        return false;
    }
    if (s->port != want)
        fprintf(stderr, "[netsrv] :%u in use; HTTP API on :%u\n", want, s->port);

    /* Keep the listener out of fork+exec'd MCP subprocesses and apply
     * platform TCP hygiene in one place. */
    netsrv_tune_socket(s->listen_fd.fd, true);

    s->running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&s->accept_thread, &attr, accept_loop, s) == 0;
    pthread_attr_destroy(&attr);
    if (!ok)
        s->running = false;
    return ok;
}

void netsrv_stop(dsco_net_server_t *s) {
    if (!s)
        return;
    s->running = false;
    mbedtls_net_free(&s->listen_fd);
    mbedtls_net_init(&s->listen_fd);
}

uint16_t netsrv_port(dsco_net_server_t *s) {
    return s ? s->port : 0;
}

/* ── TLS cert generation ────────────────────────────────────────────────── */
bool netsrv_gen_tls_cert(const char *cert_path, const char *key_path, const char *cn) {
    bool ok = false;
    mbedtls_pk_context pkey;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_pk_init(&pkey);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    const char *pers = "dsco-certgen";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        goto done;

    /* Generate EC P-256 key */
    if (mbedtls_pk_setup(&pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
        goto done;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pkey), mbedtls_ctr_drbg_random,
                            &ctr_drbg) != 0)
        goto done;

    /* Build self-signed cert */
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pkey);

    char subject[128];
    snprintf(subject, sizeof(subject), "CN=%s", cn ? cn : "dsco-node");
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0)
        goto done;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0)
        goto done;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000") != 0)
        goto done;

    /* Serial = random 8 bytes */
    uint8_t serial_bytes[8];
    mbedtls_ctr_drbg_random(&ctr_drbg, serial_bytes, sizeof(serial_bytes));
    serial_bytes[0] &= 0x7F; /* ensure positive */
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial_bytes, sizeof(serial_bytes)) != 0)
        goto done;

    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

    /* Write cert PEM */
    unsigned char cert_buf[8192];
    if (mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf), mbedtls_ctr_drbg_random,
                                  &ctr_drbg) != 0)
        goto done;

    /* Write key PEM */
    unsigned char key_buf[4096];
    if (mbedtls_pk_write_key_pem(&pkey, key_buf, sizeof(key_buf)) != 0)
        goto done;

    /* Save to disk */
    FILE *f = fopen(cert_path, "w");
    if (!f)
        goto done;
    fputs((const char *)cert_buf, f);
    fclose(f);

    f = fopen(key_path, "w");
    if (!f)
        goto done;
    fputs((const char *)key_buf, f);
    fclose(f);

    ok = true;
    fprintf(stderr, "[netsrv] TLS cert written: %s  key: %s\n", cert_path, key_path);

done:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

/* ── Client: POST JSON to a remote dsco server ─────────────────────────── */
char *netsrv_client_post(const char *host, uint16_t port, const char *path, const char *json_body,
                         const uint8_t *auth_key, size_t auth_key_len, bool use_tls) {
    char *result = NULL;

    mbedtls_net_context fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt cacert;

    mbedtls_net_init(&fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&cacert);

    const char *pers = "dsco-client";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        goto done;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (mbedtls_net_connect(&fd, host, port_str, MBEDTLS_NET_PROTO_TCP) != 0)
        goto done;
    netsrv_tune_socket(fd.fd, false);

    if (use_tls) {
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            goto done;
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
        const char *ca_file = getenv("DSCO_NET_CA_FILE");
        if (ca_file && *ca_file) {
            if (mbedtls_x509_crt_parse_file(&cacert, ca_file) != 0) goto done;
        } else if (mbedtls_x509_crt_parse_file(&cacert, "/etc/ssl/cert.pem") < 0 &&
                   mbedtls_x509_crt_parse_file(&cacert, "/etc/ssl/certs/ca-certificates.crt") < 0) {
            goto done;
        }
        mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0)
            goto done;
        mbedtls_ssl_set_hostname(&ssl, host);
        mbedtls_ssl_set_bio(&ssl, &fd, mbedtls_net_send, mbedtls_net_recv, NULL);
        int ret;
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                goto done;
        }
    }

    /* Build request */
    size_t body_len = json_body ? strlen(json_body) : 0;

    /* Signed request: v1:timestamp:nonce:HMAC(timestamp\nnonce\nmethod\npath\nbody).
     * A NULL key means the process-scoped DSCO_NET_AUTH_KEY. */
    uint8_t effective_key[crypto_auth_hmacsha512256_KEYBYTES];
    const uint8_t *sign_key = auth_key;
    if (!sign_key) { const char *ek=getenv("DSCO_NET_AUTH_KEY");
        if (ek && hex_decode_key(ek,effective_key)) { sign_key=effective_key; auth_key_len=sizeof(effective_key); } }
    char auth_hdr[256] = {0};
    if (sign_key && auth_key_len == crypto_auth_hmacsha512256_KEYBYTES) {
        char ts[32], nonce[NETSRV_NONCE_HEX+1];
        char *msg = malloc(body_len + 512);
        if (!msg) goto done;
        uint8_t nonce_raw[NETSRV_NONCE_HEX/2], mac[crypto_auth_hmacsha512256_BYTES];
        randombytes_buf(nonce_raw, sizeof(nonce_raw)); hex_encode(nonce_raw,sizeof(nonce_raw),nonce);
        snprintf(ts,sizeof(ts),"%llu",(unsigned long long)netsrv_now());
        int ml=snprintf(msg,body_len + 512,"%s\n%s\nPOST\n%s\n",ts,nonce,path ? path : "/");
        if (ml < 0 || (size_t)ml >= 512) { free(msg); goto done; }
        if (body_len) memcpy(msg+ml,json_body,body_len);
        crypto_auth_hmacsha512256(mac,(const uint8_t*)msg,(size_t)ml+body_len,sign_key);
        free(msg);
        char mh[sizeof(mac)*2+1]; hex_encode(mac,sizeof(mac),mh);
        snprintf(auth_hdr,sizeof(auth_hdr),"X-DSCO-Auth: v1:%s:%s:%s\r\n",ts,nonce,mh);
        sodium_memzero(effective_key,sizeof(effective_key));
    }

    char req_hdr[1024];
    snprintf(req_hdr, sizeof(req_hdr),
             "POST %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "%s\r\n",
             path, host, body_len, auth_hdr);

    /* Send */
    bool wok = client_write_all(use_tls, &ssl, &fd, req_hdr, strlen(req_hdr));
    wok &= body_len == 0 || client_write_all(use_tls, &ssl, &fd, json_body, body_len);
    if (!wok)
        goto done;

    /* Read response into growing buffer */
    size_t rpos = 0, rcap = 4096;
    char *rbuf = malloc(rcap);
    if (!rbuf)
        goto done;

    while (1) {
        if (rpos + 1024 >= 2 * NETSRV_MAX_BODY) { free(rbuf); goto done; }
        if (rpos + 1024 >= rcap) {
            rcap *= 2;
            char *tmp = realloc(rbuf, rcap);
            if (!tmp) {
                free(rbuf);
                goto done;
            }
            rbuf = tmp;
        }
        int n;
        if (use_tls)
            n = mbedtls_ssl_read(&ssl, (unsigned char *)(rbuf + rpos), rcap - rpos - 1);
        else
            n = mbedtls_net_recv(&fd, (unsigned char *)(rbuf + rpos), rcap - rpos - 1);
        if (n <= 0)
            break;
        rpos += (size_t)n;
    }
    rbuf[rpos] = '\0';

    /* Strip HTTP headers: find double CRLF */
    char *body_start = strstr(rbuf, "\r\n\r\n");
    if (body_start)
        body_start += 4;
    else
        body_start = rbuf;

    result = strdup(body_start);
    free(rbuf);

done:
    if (use_tls)
        mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&fd);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}
