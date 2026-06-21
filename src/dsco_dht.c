/* Distributed peer discovery over a private Kademlia/BitTorrent DHT overlay,
 * bridged to the encrypted mesh via ~/.dsco/peers.txt + peer_bootstrap_reseed.
 * See include/dsco_dht.h. Wraps vendor/dht.c (compiled in dht_impl.c). */

#include "dsco_dht.h"

#ifdef HAVE_LIBSODIUM

#include "peer_bootstrap.h"
#include "../vendor/dht.h"

#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>

#define DHT_DEFAULT_UDP_PORT   7600
#define DHT_SEARCH_INTERVAL_S  300   /* re-announce / re-search cadence */
#define DHT_ID_LEN             20

struct dsco_dht {
    int             sock;
    pthread_t       thread;
    volatile bool   running;
    bool            dirty;        /* new peers written, reseed pending */
    unsigned char   id[DHT_ID_LEN];
    unsigned char   infohash[DHT_ID_LEN];
    uint16_t        mesh_port;    /* announced port peers dial over the mesh */
    time_t          next_search;
    dsco_dht_stats_t stats;
    pthread_mutex_t lock;         /* serializes all dht_* calls + stat fields */
};

static dsco_dht_t *g_dht = NULL;

/* ── User callbacks required by vendor/dht.c ───────────────────────────────
 * dht.c is single-threaded; we serialize every dht_* entry point under
 * d->lock, so these run with that lock already held. */

int dht_sendto(int sockfd, const void *buf, int len, int flags,
               const struct sockaddr *to, int tolen) {
    return (int)sendto(sockfd, buf, (size_t)len, flags, to, (socklen_t)tolen);
}

int dht_blacklisted(const struct sockaddr *sa, int salen) {
    (void)sa; (void)salen;
    return 0;  /* private overlay: trust configured bootstrap graph */
}

void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    if (v1 && len1 > 0) crypto_hash_sha256_update(&st, v1, (size_t)len1);
    if (v2 && len2 > 0) crypto_hash_sha256_update(&st, v2, (size_t)len2);
    if (v3 && len3 > 0) crypto_hash_sha256_update(&st, v3, (size_t)len3);
    unsigned char out[crypto_hash_sha256_BYTES];  /* 32 */
    crypto_hash_sha256_final(&st, out);
    if (hash_size > (int)sizeof(out)) {
        memset((char *)hash_return + sizeof(out), 0,
               (size_t)hash_size - sizeof(out));
        hash_size = (int)sizeof(out);
    }
    memcpy(hash_return, out, (size_t)hash_size);
}

int dht_random_bytes(void *buf, size_t size) {
    randombytes_buf(buf, size);
    return (int)size;
}

/* ── Bridge: record a discovered peer for the mesh ─────────────────────────
 * Append "ip:port" to ~/.dsco/peers.txt (deduped) so the existing
 * peer_bootstrap layer dials it over the encrypted mesh. */

static const char *dht_home_path(char *buf, size_t sz, const char *leaf) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, sz, "%s/.dsco/%s", home, leaf);
    return buf;
}

static void dht_record_peer(dsco_dht_t *d, const char *ip, uint16_t port) {
    char path[1024];
    if (!dht_home_path(path, sizeof(path), "peers.txt")) return;

    char line[80];
    snprintf(line, sizeof(line), "%s:%u", ip, (unsigned)port);

    /* Dedup against existing entries. */
    FILE *rf = fopen(path, "r");
    if (rf) {
        char buf[256];
        while (fgets(buf, sizeof(buf), rf)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            if (strcmp(buf, line) == 0) { fclose(rf); return; }
        }
        fclose(rf);
    }

    /* Ensure ~/.dsco exists, then append. */
    char dir[1024];
    if (dht_home_path(dir, sizeof(dir), "")) {
        size_t n = strlen(dir);
        if (n && dir[n - 1] == '/') dir[n - 1] = '\0';
        mkdir(dir, 0700);
    }
    FILE *af = fopen(path, "a");
    if (!af) return;
    fprintf(af, "%s\n", line);
    fclose(af);

    d->stats.peers_found++;   /* caller holds d->lock */
    d->dirty = true;
}

/* dht.c invokes this from inside dht_periodic/dht_search (d->lock held). */
static void dht_event_cb(void *closure, int event,
                         const unsigned char *info_hash,
                         const void *data, size_t data_len) {
    (void)info_hash;
    dsco_dht_t *d = (dsco_dht_t *)closure;
    if (event != DHT_EVENT_VALUES) return;  /* IPv4 compact peers only (phase 1) */

    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i + 6 <= data_len; i += 6) {
        struct in_addr a;
        memcpy(&a, p + i, 4);
        uint16_t port = (uint16_t)((p[i + 4] << 8) | p[i + 5]);
        if (port == 0) continue;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &a, ip, sizeof(ip)))
            dht_record_peer(d, ip, port);
    }
}

/* ── Node identity (persistent) ────────────────────────────────────────── */

static void dht_load_or_make_id(unsigned char id[DHT_ID_LEN]) {
    char path[1024];
    if (dht_home_path(path, sizeof(path), "dht_id")) {
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(id, 1, DHT_ID_LEN, f);
            fclose(f);
            if (n == DHT_ID_LEN) return;
        }
    }
    randombytes_buf(id, DHT_ID_LEN);
    char dir[1024];
    if (dht_home_path(dir, sizeof(dir), "")) {
        size_t n = strlen(dir);
        if (n && dir[n - 1] == '/') dir[n - 1] = '\0';
        mkdir(dir, 0700);
    }
    if (dht_home_path(path, sizeof(path), "dht_id")) {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(id, 1, DHT_ID_LEN, f); fclose(f); }
    }
}

/* ── Poll thread ───────────────────────────────────────────────────────── */

static void *dht_poll_thread(void *arg) {
    dsco_dht_t *d = (dsco_dht_t *)arg;
    time_t tosleep = 0;
    unsigned char buf[4096];

    while (d->running) {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(d->sock, &rfd);
        struct timeval tv;
        tv.tv_sec  = (tosleep > 0 && tosleep < 5) ? tosleep : 1;
        tv.tv_usec = 0;
        int rc = select(d->sock + 1, &rfd, NULL, NULL, &tv);
        if (!d->running) break;

        ssize_t n = -1;
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        if (rc > 0 && FD_ISSET(d->sock, &rfd)) {
            n = recvfrom(d->sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        }

        pthread_mutex_lock(&d->lock);
        time_t now = time(NULL);
        if (now >= d->next_search) {
            dht_search(d->infohash, (int)d->mesh_port, AF_INET, dht_event_cb, d);
            d->stats.searches++;
            d->next_search = now + DHT_SEARCH_INTERVAL_S;
        }
        if (n >= 0) {
            buf[n] = '\0';
            dht_periodic(buf, (size_t)n, (struct sockaddr *)&from, (int)fromlen,
                         &tosleep, dht_event_cb, d);
        } else {
            dht_periodic(NULL, 0, NULL, 0, &tosleep, dht_event_cb, d);
        }
        bool dirty = d->dirty;
        d->dirty = false;
        pthread_mutex_unlock(&d->lock);

        if (dirty) peer_bootstrap_reseed();  /* dial new peers over the mesh */
    }
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool dsco_dht_bootstrap(dsco_dht_t *d, const char *host, uint16_t port) {
    if (!d || !host || !*host) return false;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return false;

    bool ok = false;
    pthread_mutex_lock(&d->lock);
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (dht_ping_node(ai->ai_addr, (int)ai->ai_addrlen) >= 0) { ok = true; break; }
    }
    pthread_mutex_unlock(&d->lock);
    freeaddrinfo(res);
    return ok;
}

static void dht_seed_from_config(dsco_dht_t *d) {
    /* DSCO_DHT_BOOTSTRAP="host:port,host:port" */
    const char *env = getenv("DSCO_DHT_BOOTSTRAP");
    char tmp[2048];
    if (env && *env) {
        snprintf(tmp, sizeof(tmp), "%s", env);
        for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
            char *colon = strrchr(tok, ':');
            if (!colon) continue;
            *colon = '\0';
            dsco_dht_bootstrap(d, tok, (uint16_t)atoi(colon + 1));
        }
    }
    /* ~/.dsco/dht_bootstrap.txt — one "host:port" (or "host port") per line. */
    char path[1024];
    if (dht_home_path(path, sizeof(path), "dht_bootstrap.txt")) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0] == '\0' || line[0] == '#') continue;
                char *sep = strpbrk(line, ": \t");
                if (!sep) continue;
                *sep = '\0';
                dsco_dht_bootstrap(d, line, (uint16_t)atoi(sep + 1));
            }
            fclose(f);
        }
    }
}

dsco_dht_t *dsco_dht_start(const dsco_dht_config_t *cfg) {
    if (g_dht) return g_dht;
    if (!cfg || !cfg->swarm_key || !*cfg->swarm_key) return NULL;
    if (sodium_init() < 0) return NULL;

    dsco_dht_t *d = (dsco_dht_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);
    d->mesh_port = cfg->mesh_port;

    uint16_t udp_port = cfg->udp_port ? cfg->udp_port : DHT_DEFAULT_UDP_PORT;
    d->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (d->sock < 0) { pthread_mutex_destroy(&d->lock); free(d); return NULL; }
    int one = 1;
    setsockopt(d->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(udp_port);
    if (bind(d->sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(d->sock);
        pthread_mutex_destroy(&d->lock);
        free(d);
        return NULL;
    }

    dht_load_or_make_id(d->id);
    crypto_hash_sha256(d->infohash, (const unsigned char *)cfg->swarm_key,
                       strlen(cfg->swarm_key));  /* first 20 bytes used as id */

    pthread_mutex_lock(&d->lock);
    int ir = dht_init(d->sock, -1, d->id, NULL);
    pthread_mutex_unlock(&d->lock);
    if (ir < 0) {
        close(d->sock);
        pthread_mutex_destroy(&d->lock);
        free(d);
        return NULL;
    }

    d->running = true;
    d->next_search = 0;          /* search immediately on first tick */
    d->stats.running = true;
    g_dht = d;

    dht_seed_from_config(d);

    if (pthread_create(&d->thread, NULL, dht_poll_thread, d) != 0) {
        d->running = false;
        g_dht = NULL;
        dht_uninit();
        close(d->sock);
        pthread_mutex_destroy(&d->lock);
        free(d);
        return NULL;
    }
    return d;
}

void dsco_dht_find_peers(dsco_dht_t *d) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    d->next_search = 0;  /* poll thread fires a fresh search next tick */
    pthread_mutex_unlock(&d->lock);
}

void dsco_dht_get_stats(dsco_dht_t *d, dsco_dht_stats_t *out) {
    if (!d || !out) return;
    pthread_mutex_lock(&d->lock);
    dht_nodes(AF_INET, &d->stats.good, &d->stats.dubious,
              &d->stats.cached, &d->stats.incoming);
    *out = d->stats;
    pthread_mutex_unlock(&d->lock);
}

void dsco_dht_stop(dsco_dht_t *d) {
    if (!d) return;
    d->running = false;
    pthread_join(d->thread, NULL);
    pthread_mutex_lock(&d->lock);
    dht_uninit();
    pthread_mutex_unlock(&d->lock);
    close(d->sock);
    pthread_mutex_destroy(&d->lock);
    if (g_dht == d) g_dht = NULL;
    free(d);
}

dsco_dht_t *dsco_dht_global(void) { return g_dht; }

#else  /* !HAVE_LIBSODIUM — no-op stubs */

dsco_dht_t *dsco_dht_start(const dsco_dht_config_t *cfg) { (void)cfg; return NULL; }
bool dsco_dht_bootstrap(dsco_dht_t *d, const char *host, uint16_t port) {
    (void)d; (void)host; (void)port; return false;
}
void dsco_dht_find_peers(dsco_dht_t *d) { (void)d; }
void dsco_dht_get_stats(dsco_dht_t *d, dsco_dht_stats_t *out) { (void)d; (void)out; }
void dsco_dht_stop(dsco_dht_t *d) { (void)d; }
dsco_dht_t *dsco_dht_global(void) { return NULL; }

#endif /* HAVE_LIBSODIUM */
