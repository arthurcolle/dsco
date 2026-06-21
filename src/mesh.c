#include "mesh.h"
#include <sodium.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Wire constants ───────────────────────────────────────────────────── */
#define NONCE_LEN        24   /* crypto_box_NONCEBYTES */
#define MAC_LEN          16   /* crypto_box_MACBYTES */
#define HANDSHAKE_LEN    36   /* magic(4) + pubkey(32) */
#define MSG_HDR_LEN      4    /* uint32_t wire_body_len */
#define INNER_HDR_LEN    5    /* type(1) + payload_len(4) */
#define SHARED_KEY_LEN   32   /* crypto_box_BEFORENMBYTES */

typedef enum {
    WIRE_DATA  = 0x01,
    WIRE_PING  = 0x02,
    WIRE_PONG  = 0x03,
    WIRE_PEERS = 0x04,
} wire_type_t;

/* ── Per-connection ───────────────────────────────────────────────────── */
typedef struct mesh_conn {
    int           sock;
    pthread_t     thread;
    struct mesh_node *node;

    uint8_t peer_pubkey[MESH_PUBKEY_LEN];
    uint8_t shared_key[SHARED_KEY_LEN];

    char    addr[48];
    bool    outbound;
    bool    authed;
    volatile bool dead;

    pthread_mutex_t wlock;
} mesh_conn_t;

/* ── Node ─────────────────────────────────────────────────────────────── */
struct mesh_node {
    uint16_t      port;
    int           server_sock;
    pthread_t     accept_thread;
    volatile bool running;

    uint8_t pubkey[MESH_PUBKEY_LEN];
    uint8_t seckey[crypto_box_SECRETKEYBYTES];

    pthread_mutex_t lock;
    mesh_conn_t    *conns[MESH_MAX_PEERS];
    int             conn_count;

    mesh_on_message_fn on_message;
    void              *on_message_ctx;
    mesh_on_peer_fn    on_connect;
    void              *on_connect_ctx;
    mesh_on_peer_fn    on_disconnect;
    void              *on_disconnect_ctx;
};

/* ── Endian helpers ───────────────────────────────────────────────────── */
static uint32_t u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF; p[3] = v          & 0xFF;
}

/* ── I/O helpers ──────────────────────────────────────────────────────── */
static bool recv_exact(int sock, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = recv(sock, p, len, MSG_WAITALL);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}
static bool send_exact(int sock, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(sock, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

/* ── Encrypted send (must hold conn->wlock externally or use this) ─── */
static bool conn_send_enc(mesh_conn_t *c, wire_type_t type,
                          const void *payload, size_t plen) {
    if (!c->authed || c->dead) return false;
    if (plen > MESH_MAX_PAYLOAD) return false;

    size_t plain_len  = INNER_HDR_LEN + plen;
    size_t cipher_len = plain_len + MAC_LEN;
    size_t body_len   = NONCE_LEN + cipher_len;
    size_t wire_len   = MSG_HDR_LEN + body_len;

    uint8_t *wire = malloc(wire_len);
    if (!wire) return false;

    uint8_t *nonce      = wire + MSG_HDR_LEN;
    uint8_t *ciphertext = nonce + NONCE_LEN;
    uint8_t  plain[INNER_HDR_LEN + MESH_MAX_PAYLOAD];

    plain[0] = (uint8_t)type;
    put_u32be(plain + 1, (uint32_t)plen);
    if (plen) memcpy(plain + INNER_HDR_LEN, payload, plen);

    randombytes_buf(nonce, NONCE_LEN);
    put_u32be(wire, (uint32_t)body_len);
    crypto_box_easy_afternm(ciphertext, plain, plain_len, nonce, c->shared_key);

    pthread_mutex_lock(&c->wlock);
    bool ok = send_exact(c->sock, wire, wire_len);
    pthread_mutex_unlock(&c->wlock);

    free(wire);
    return ok;
}

/* ── Remove connection from node list and free ────────────────────────── */
static void conn_remove(struct mesh_node *n, mesh_conn_t *c) {
    pthread_mutex_lock(&n->lock);
    for (int i = 0; i < n->conn_count; i++) {
        if (n->conns[i] == c) {
            n->conns[i] = n->conns[--n->conn_count];
            break;
        }
    }
    pthread_mutex_unlock(&n->lock);
    pthread_mutex_destroy(&c->wlock);
    free(c);
}

/* ── Reader thread (one per connection) ───────────────────────────────── */
static void *conn_reader(void *arg) {
    mesh_conn_t      *c = arg;
    struct mesh_node *n = c->node;

    /* Snapshot callbacks so we can call them after removal */
    mesh_on_message_fn   on_msg  = n->on_message;
    void                *msg_ctx = n->on_message_ctx;
    mesh_on_peer_fn      on_con  = n->on_connect;
    void                *con_ctx = n->on_connect_ctx;
    mesh_on_peer_fn      on_dis  = n->on_disconnect;
    void                *dis_ctx = n->on_disconnect_ctx;

    /* ── Send handshake ─────────────────────────────────────────────── */
    uint8_t hs[HANDSHAKE_LEN];
    put_u32be(hs, MESH_MAGIC);
    memcpy(hs + 4, n->pubkey, MESH_PUBKEY_LEN);
    pthread_mutex_lock(&c->wlock);
    bool ok = send_exact(c->sock, hs, HANDSHAKE_LEN);
    pthread_mutex_unlock(&c->wlock);
    if (!ok) goto done;

    /* ── Read peer handshake ─────────────────────────────────────────── */
    uint8_t peer_hs[HANDSHAKE_LEN];
    if (!recv_exact(c->sock, peer_hs, HANDSHAKE_LEN)) goto done;
    if (u32be(peer_hs) != MESH_MAGIC) goto done;
    memcpy(c->peer_pubkey, peer_hs + 4, MESH_PUBKEY_LEN);

    /* Compute shared key once — used for all messages on this connection */
    (void)crypto_box_beforenm(c->shared_key, c->peer_pubkey, n->seckey);
    c->authed = true;

    if (on_con) {
        mesh_peer_info_t info;
        memcpy(info.pubkey, c->peer_pubkey, MESH_PUBKEY_LEN);
        snprintf(info.addr, sizeof(info.addr), "%s", c->addr);
        info.outbound = c->outbound;
        on_con(&info, con_ctx);
    }

    /* ── Gossip: send our known peers to the newly connected node ─────── */
    if (n->conn_count > 1) {
        mesh_peer_info_t peers[MESH_MAX_PEERS];
        int npc = mesh_node_peers(n, peers, MESH_MAX_PEERS);
        if (npc > 0) {
            size_t peer_entry_len = 48; /* host(40) + port(2) + pad(6) */
            size_t gossip_sz = 2 + (size_t)npc * peer_entry_len;
            if (gossip_sz <= MESH_MAX_PAYLOAD) {
                uint8_t *gossip = calloc(1, gossip_sz);
                if (gossip) {
                    gossip[0] = (uint8_t)(npc >> 8);
                    gossip[1] = (uint8_t)(npc & 0xFF);
                    int written = 0;
                    for (int i = 0; i < npc && written < npc; i++) {
                        /* Skip the peer we're talking to */
                        if (memcmp(peers[i].pubkey, c->peer_pubkey, MESH_PUBKEY_LEN) == 0)
                            continue;
                        uint8_t *entry = gossip + 2 + (size_t)written * peer_entry_len;
                        memset(entry, 0, peer_entry_len);
                        /* Extract host from addr (strip port if present) */
                        char host[41] = {0};
                        snprintf(host, sizeof(host), "%s", peers[i].addr);
                        char *colon = strrchr(host, ':');
                        if (colon) *colon = '\0';
                        if (host[0] == '\0') continue;
                        memcpy(entry, host, strnlen(host, 40));
                        /* Use the mesh port (we don't track per-peer; use our own) */
                        uint16_t port = n->port;
                        entry[40] = (uint8_t)(port >> 8);
                        entry[41] = (uint8_t)(port & 0xFF);
                        written++;
                    }
                    /* Update count to actually-written entries */
                    gossip[0] = (uint8_t)(written >> 8);
                    gossip[1] = (uint8_t)(written & 0xFF);
                    conn_send_enc(c, WIRE_PEERS, gossip, gossip_sz);
                    free(gossip);
                }
            }
        }
    }

    /* ── Message loop ────────────────────────────────────────────────── */
    while (!c->dead && n->running) {
        /* Read 4-byte wire_body length */
        uint8_t hdr[MSG_HDR_LEN];
        if (!recv_exact(c->sock, hdr, MSG_HDR_LEN)) break;

        uint32_t body_len = u32be(hdr);
        if (body_len < (uint32_t)(NONCE_LEN + MAC_LEN + INNER_HDR_LEN)) break;
        if (body_len > (uint32_t)(NONCE_LEN + MAC_LEN + INNER_HDR_LEN + MESH_MAX_PAYLOAD)) break;

        uint8_t *body = malloc(body_len);
        if (!body) break;
        if (!recv_exact(c->sock, body, body_len)) { free(body); break; }

        uint8_t *nonce      = body;
        uint8_t *ciphertext = body + NONCE_LEN;
        size_t   cipher_len = body_len - NONCE_LEN;
        size_t   plain_len  = cipher_len - MAC_LEN;

        uint8_t *plain = malloc(plain_len);
        if (!plain) { free(body); break; }

        if (crypto_box_open_easy_afternm(plain, ciphertext, cipher_len,
                                         nonce, c->shared_key) != 0) {
            free(plain); free(body);
            fprintf(stderr, "[mesh] auth failure from %s — dropping\n", c->addr);
            break;
        }
        free(body);

        if (plain_len < INNER_HDR_LEN) { free(plain); break; }
        wire_type_t type    = (wire_type_t)plain[0];
        uint32_t    pay_len = u32be(plain + 1);
        if ((size_t)(INNER_HDR_LEN + pay_len) != plain_len) { free(plain); break; }
        const uint8_t *pay = plain + INNER_HDR_LEN;

        switch (type) {
        case WIRE_DATA:
            if (on_msg) on_msg(c->peer_pubkey, pay, pay_len, msg_ctx);
            break;
        case WIRE_PING:
            conn_send_enc(c, WIRE_PONG, pay, pay_len);
            break;
        case WIRE_PONG:
            break;
        case WIRE_PEERS: {
            /* Gossip protocol: [count:2][host:40 port:2 padding:6 each]
             * Parse advertised peers and dial any we don't already know. */
            if (pay_len >= 2 && c->node) {
                uint16_t pcount = (uint16_t)((pay[0] << 8) | pay[1]);
                const size_t peer_entry_len = 48; /* host(40) + port(2) + pad(6) */
                size_t offset = 2;
                for (uint16_t i = 0; i < pcount && offset + peer_entry_len <= pay_len; i++) {
                    char host[41];
                    memcpy(host, pay + offset, 40);
                    host[40] = '\0';
                    /* Trim trailing NULs / whitespace */
                    size_t hlen = strnlen(host, 40);
                    host[hlen] = '\0';
                    uint16_t port = (uint16_t)((pay[offset + 40] << 8) | pay[offset + 41]);
                    offset += peer_entry_len;
                    if (host[0] == '\0' || port == 0) continue;
                    /* Attempt connection if not already connected */
                    mesh_node_connect(c->node, host, port);
                }
            }
            break;
        }
        }
        free(plain);
    }

done:
    c->dead = true;
    shutdown(c->sock, SHUT_RDWR);
    close(c->sock);

    if (c->authed && on_dis) {
        mesh_peer_info_t info;
        memcpy(info.pubkey, c->peer_pubkey, MESH_PUBKEY_LEN);
        snprintf(info.addr, sizeof(info.addr), "%s", c->addr);
        info.outbound = c->outbound;
        on_dis(&info, dis_ctx);
    }

    conn_remove(n, c);
    return NULL;
}

/* ── Accept thread ────────────────────────────────────────────────────── */
static void *accept_loop(void *arg) {
    struct mesh_node *n = arg;

    while (n->running) {
        struct sockaddr_storage cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int fd = accept(n->server_sock, (struct sockaddr *)&cli_addr, &cli_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        pthread_mutex_lock(&n->lock);
        bool full = n->conn_count >= MESH_MAX_PEERS;
        pthread_mutex_unlock(&n->lock);
        if (full) { close(fd); continue; }

        mesh_conn_t *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->sock     = fd;
        c->node     = n;
        c->outbound = false;
        pthread_mutex_init(&c->wlock, NULL);

        /* Peer address string */
        char host[INET6_ADDRSTRLEN] = "?";
        uint16_t pport = 0;
        if (cli_addr.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&cli_addr;
            inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
            pport = ntohs(a->sin_port);
        } else if (cli_addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&cli_addr;
            inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
            pport = ntohs(a->sin6_port);
        }
        snprintf(c->addr, sizeof(c->addr), "%s:%u", host, pport);

        pthread_mutex_lock(&n->lock);
        n->conns[n->conn_count++] = c;
        pthread_mutex_unlock(&n->lock);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&c->thread, &attr, conn_reader, c);
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

mesh_node_t *mesh_node_create(uint16_t port) {
    if (sodium_init() < 0) return NULL;

    mesh_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;

    n->port        = port;
    n->server_sock = -1;
    crypto_box_keypair(n->pubkey, n->seckey);
    pthread_mutex_init(&n->lock, NULL);
    return n;
}

void mesh_node_destroy(mesh_node_t *n) {
    if (!n) return;
    mesh_node_stop(n);
    /* Wait up to 2 s for reader threads to drain */
    for (int i = 0; i < 40; i++) {
        pthread_mutex_lock(&n->lock);
        int rem = n->conn_count;
        pthread_mutex_unlock(&n->lock);
        if (rem == 0) break;
        usleep(50000);
    }
    sodium_memzero(n->seckey, sizeof(n->seckey));
    pthread_mutex_destroy(&n->lock);
    free(n);
}

bool mesh_node_start(mesh_node_t *n) {
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    int ipv4only = 0;
    if (s >= 0) {
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &ipv4only, sizeof(ipv4only));
    } else {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
    }

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in6 addr6 = {0};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port   = htons(n->port);
    addr6.sin6_addr   = in6addr_any;

    if (bind(s, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
        /* Try IPv4 fallback */
        close(s);
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr4 = {0};
        addr4.sin_family      = AF_INET;
        addr4.sin_port        = htons(n->port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            close(s); return false;
        }
    }

    if (listen(s, 32) < 0) { close(s); return false; }
    n->server_sock = s;
    n->running     = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&n->accept_thread, &attr, accept_loop, n) == 0;
    pthread_attr_destroy(&attr);

    if (!ok) {
        n->running = false;
        close(s); n->server_sock = -1;
    }
    return ok;
}

void mesh_node_stop(mesh_node_t *n) {
    if (!n) return;
    n->running = false;
    if (n->server_sock >= 0) {
        shutdown(n->server_sock, SHUT_RDWR);
        close(n->server_sock);
        n->server_sock = -1;
    }
    pthread_mutex_lock(&n->lock);
    for (int i = 0; i < n->conn_count; i++) {
        n->conns[i]->dead = true;
        shutdown(n->conns[i]->sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&n->lock);
}

const uint8_t *mesh_node_pubkey(mesh_node_t *n) {
    return n ? n->pubkey : NULL;
}

bool mesh_node_connect(mesh_node_t *n, const char *host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return false;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    pthread_mutex_lock(&n->lock);
    if (n->conn_count >= MESH_MAX_PEERS) {
        pthread_mutex_unlock(&n->lock);
        close(fd); return false;
    }
    mesh_conn_t *c = calloc(1, sizeof(*c));
    if (!c) { pthread_mutex_unlock(&n->lock); close(fd); return false; }
    c->sock     = fd;
    c->node     = n;
    c->outbound = true;
    pthread_mutex_init(&c->wlock, NULL);
    snprintf(c->addr, sizeof(c->addr), "%s:%u", host, port);
    n->conns[n->conn_count++] = c;
    pthread_mutex_unlock(&n->lock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&c->thread, &attr, conn_reader, c);
    pthread_attr_destroy(&attr);
    return true;
}

bool mesh_node_send_to(mesh_node_t *n, const uint8_t *peer_pk,
                       const void *data, size_t len) {
    mesh_conn_t *target = NULL;
    pthread_mutex_lock(&n->lock);
    for (int i = 0; i < n->conn_count; i++) {
        mesh_conn_t *c = n->conns[i];
        if (c->authed && !c->dead &&
            memcmp(c->peer_pubkey, peer_pk, MESH_PUBKEY_LEN) == 0) {
            target = c; break;
        }
    }
    pthread_mutex_unlock(&n->lock);
    return target ? conn_send_enc(target, WIRE_DATA, data, len) : false;
}

int mesh_node_broadcast(mesh_node_t *n, const void *data, size_t len) {
    mesh_conn_t *snap[MESH_MAX_PEERS];
    int count = 0;
    pthread_mutex_lock(&n->lock);
    for (int i = 0; i < n->conn_count && count < MESH_MAX_PEERS; i++) {
        if (n->conns[i]->authed && !n->conns[i]->dead)
            snap[count++] = n->conns[i];
    }
    pthread_mutex_unlock(&n->lock);

    int sent = 0;
    for (int i = 0; i < count; i++)
        if (conn_send_enc(snap[i], WIRE_DATA, data, len)) sent++;
    return sent;
}

int mesh_node_peers(mesh_node_t *n, mesh_peer_info_t *out, int max) {
    int count = 0;
    pthread_mutex_lock(&n->lock);
    for (int i = 0; i < n->conn_count && count < max; i++) {
        mesh_conn_t *c = n->conns[i];
        if (!c->authed || c->dead) continue;
        memcpy(out[count].pubkey, c->peer_pubkey, MESH_PUBKEY_LEN);
        snprintf(out[count].addr, sizeof(out[count].addr), "%s", c->addr);
        out[count].outbound = c->outbound;
        count++;
    }
    pthread_mutex_unlock(&n->lock);
    return count;
}

void mesh_node_set_on_message(mesh_node_t *n, mesh_on_message_fn cb, void *ctx) {
    n->on_message = cb; n->on_message_ctx = ctx;
}
void mesh_node_set_on_connect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx) {
    n->on_connect = cb; n->on_connect_ctx = ctx;
}
void mesh_node_set_on_disconnect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx) {
    n->on_disconnect = cb; n->on_disconnect_ctx = ctx;
}

void mesh_pubkey_to_hex(const uint8_t *pk, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < MESH_PUBKEY_LEN; i++) {
        out[i*2]   = hex[pk[i] >> 4];
        out[i*2+1] = hex[pk[i] & 0xF];
    }
    out[64] = '\0';
}

bool mesh_pubkey_from_hex(const char *hex, uint8_t pk[MESH_PUBKEY_LEN]) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < MESH_PUBKEY_LEN; i++) {
        unsigned int hi, lo;
        char c = hex[i*2];
        if      (c >= '0' && c <= '9') hi = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = (unsigned)(c - 'A' + 10);
        else return false;
        c = hex[i*2+1];
        if      (c >= '0' && c <= '9') lo = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = (unsigned)(c - 'A' + 10);
        else return false;
        pk[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
