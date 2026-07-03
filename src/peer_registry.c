#include "peer_registry.h"
#include "net_server.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Offload requires the secure net server (netsrv_client_post) — only built
 * when libsodium + mbedTLS are present. Without them the whole feature compiles
 * to a disabled no-op. */
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
#define PEER_NET_AVAILABLE 1
#else
#define PEER_NET_AVAILABLE 0
#endif

#define PEER_REGISTRY_MAX 32
#define PEER_TRIP_THRESHOLD 3
#define PEER_TRIP_BASE_SECS 15
#define PEER_TRIP_MAX_SECS 300

typedef struct {
    char name[64];
    char addr[128];
    int port;
    double rtt_ewma_ms;  /* 0 = no data */
    double success_ewma; /* 1.0 = optimistic prior */
    int inflight;
    int consec_failures;
    long total_calls;
    long total_failures;
    time_t tripped_until; /* 0 = healthy */
} peer_slot_t;

static peer_slot_t g_peers[PEER_REGISTRY_MAX];
static int g_peer_count = 0;
static pthread_mutex_t g_peer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_hb_once = PTHREAD_ONCE_INIT;
static int g_initialized = 0;

bool peer_registry_enabled(void) {
    if (!PEER_NET_AVAILABLE)
        return false;
    const char *v = getenv("DSCO_OFFLOAD");
    return v && v[0] && v[0] != '0';
}

static int peer_default_port(void) {
    const char *p = getenv("DSCO_OFFLOAD_PORT");
    if (p && p[0]) {
        int v = atoi(p);
        if (v > 0 && v < 65536)
            return v;
    }
    return NETSRV_DEFAULT_PORT;
}

/* Parse KEY="VALUE" out of a fleet .host line into out; returns true on match. */
static bool host_kv(const char *line, const char *key, char *out, size_t n) {
    size_t klen = strlen(key);
    const char *s = line;
    while (*s == ' ' || *s == '\t')
        s++;
    if (strncmp(s, key, klen) != 0)
        return false;
    s += klen;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s != '=')
        return false;
    s++;
    while (*s == ' ' || *s == '\t' || *s == '"')
        s++;
    size_t i = 0;
    while (*s && *s != '"' && *s != '\n' && *s != '\r' && i + 1 < n)
        out[i++] = *s++;
    out[i] = '\0';
    return i > 0;
}

static void peer_add(const char *name, const char *addr, int port) {
    if (!addr[0] || g_peer_count >= PEER_REGISTRY_MAX)
        return;
    for (int i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].name, name) == 0 || strcmp(g_peers[i].addr, addr) == 0)
            return; /* dedup by name or address */
    }
    peer_slot_t *s = &g_peers[g_peer_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name[0] ? name : addr);
    snprintf(s->addr, sizeof(s->addr), "%s", addr);
    s->port = port;
    s->success_ewma = 1.0; /* optimistic until measured */
}

void peer_registry_init(void) {
    if (!peer_registry_enabled())
        return;
    pthread_mutex_lock(&g_peer_mu);
    if (g_initialized) {
        pthread_mutex_unlock(&g_peer_mu);
        return;
    }
    char self[128] = "";
    gethostname(self, sizeof(self));
    char *dot = strchr(self, '.');
    if (dot)
        *dot = '\0';

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/bridge/fleet", home);
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *de;
            int port = peer_default_port();
            while ((de = readdir(d))) {
                const char *ext = strrchr(de->d_name, '.');
                if (!ext || strcmp(ext, ".host") != 0)
                    continue;
                char path[700];
                snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
                FILE *f = fopen(path, "r");
                if (!f)
                    continue;
                char name[64] = "", addr[128] = "", line[256];
                while (fgets(line, sizeof(line), f)) {
                    char v[128];
                    if (host_kv(line, "NAME", v, sizeof(v)))
                        snprintf(name, sizeof(name), "%s", v);
                    else if (host_kv(line, "ADDR", v, sizeof(v)))
                        snprintf(addr, sizeof(addr), "%s", v);
                }
                fclose(f);
                /* Derive name from filename if the file omitted NAME=. */
                if (!name[0]) {
                    snprintf(name, sizeof(name), "%.*s", (int)(ext - de->d_name), de->d_name);
                }
                /* Skip self and empty/loopback addresses. */
                if (self[0] && strcasecmp(name, self) == 0)
                    continue;
                if (!addr[0] || strncmp(addr, "127.", 4) == 0 || strcmp(addr, "localhost") == 0)
                    continue;
                peer_add(name, addr, port);
            }
            closedir(d);
        }
    }
    g_initialized = 1;
    int discovered = g_peer_count;
    pthread_mutex_unlock(&g_peer_mu);
    if (getenv("DSCO_DEBUG")) {
        fprintf(stderr, "[peer_registry] discovered %d fleet peer(s) for offload\n", discovered);
        for (int i = 0; i < discovered; i++)
            fprintf(stderr, "  peer %s @ %s:%d\n", g_peers[i].name, g_peers[i].addr,
                    g_peers[i].port);
    }
}

double peer_registry_score_components(double rtt_ewma_ms, double success_ewma, int inflight,
                                      bool available) {
    if (!available)
        return 1e9;
    double rtt = rtt_ewma_ms > 0 ? rtt_ewma_ms : 100.0; /* neutral LAN prior */
    double sr = success_ewma;
    if (sr < 0.0)
        sr = 0.0;
    if (sr > 1.0)
        sr = 1.0;
    /* Failure penalty dominates latency; in-flight load nudges spread. */
    double score = rtt + (1.0 - sr) * 5000.0 + (double)inflight * 200.0;
    return score;
}

double bandit_ucb_adjust(double base_score, long arm_pulls, long total_pulls, double explore_c) {
    if (explore_c <= 0.0 || base_score >= 1e9)
        return base_score; /* disabled, or a disqualified arm stays out */
    if (total_pulls < 0)
        total_pulls = 0;
    if (arm_pulls < 0)
        arm_pulls = 0;
    /* UCB1 exploration bonus, larger for under-observed arms. +1 avoids div-by-0
     * and log(0); an unobserved arm (arm_pulls=0) gets the full bonus. Since we
     * MINIMIZE cost, subtract the bonus to make rarely-tried arms competitive. */
    double bonus = explore_c * sqrt(log((double)total_pulls + 1.0) / ((double)arm_pulls + 1.0));
    return base_score - bonus;
}

bool peer_result_plausible(const char *result, size_t len) {
    if (!result || len == 0)
        return false;
    /* Skip leading whitespace; all-whitespace is implausible. */
    size_t i = 0;
    while (i < len &&
           (result[i] == ' ' || result[i] == '\t' || result[i] == '\n' || result[i] == '\r'))
        i++;
    if (i >= len)
        return false;
    /* An error envelope from the peer's /tool endpoint (e.g. the 403 body, or a
     * transport error) is not a real tool result — fall back to local. Real
     * offload-safe tool output is search/compute content, not {"error":...}. */
    if (len - i >= 8 && strncmp(result + i, "{\"error\"", 8) == 0)
        return false;
    return true;
}

int peer_hedge_decide(bool local_done, bool local_ok, bool remote_done, bool remote_ok) {
    if (local_ok)
        return 0; /* local succeeded — take it, don't wait on remote */
    if (remote_ok)
        return 1; /* remote succeeded and local hasn't — take remote */
    if (local_done && remote_done)
        return 2; /* both finished, both failed */
    return -1;    /* keep waiting */
}

/* Caller must hold g_peer_mu. */
static bool peer_available_locked(const peer_slot_t *s, time_t now) {
    if (s->tripped_until && now < s->tripped_until)
        return false;
    return true;
}

const char *peer_registry_pick(char *addr, size_t alen, int *port) {
    if (!peer_registry_enabled())
        return NULL;
    /* Backpressure: never pile more than this many concurrent offloads on one
     * peer (a big batch of offload-safe tools would otherwise flood it). */
    int max_inflight = 4;
    const char *cap = getenv("DSCO_OFFLOAD_MAX_INFLIGHT");
    if (cap && cap[0]) {
        int v = atoi(cap);
        if (v > 0)
            max_inflight = v;
    }
    /* Exploration strength (ms scale, matching the score). */
    double explore_c = 200.0;
    const char *ec = getenv("DSCO_OFFLOAD_EXPLORE");
    if (ec && ec[0])
        explore_c = atof(ec);

    pthread_mutex_lock(&g_peer_mu);
    time_t now = time(NULL);
    long total_pulls = 0;
    for (int i = 0; i < g_peer_count; i++)
        total_pulls += g_peers[i].total_calls;
    int best = -1;
    double best_score = 1e9;
    for (int i = 0; i < g_peer_count; i++) {
        peer_slot_t *s = &g_peers[i];
        bool avail = peer_available_locked(s, now) && s->inflight < max_inflight;
        double sc =
            peer_registry_score_components(s->rtt_ewma_ms, s->success_ewma, s->inflight, avail);
        /* UCB exploration: re-probe under-observed peers to catch recovery. */
        sc = bandit_ucb_adjust(sc, s->total_calls, total_pulls, explore_c);
        if (sc < best_score) {
            best_score = sc;
            best = i;
        }
    }
    const char *name = NULL;
    if (best >= 0 && best_score < 1e9) {
        peer_slot_t *s = &g_peers[best];
        s->inflight++; /* reserve — spreads load across a batch */
        if (addr && alen)
            snprintf(addr, alen, "%s", s->addr);
        if (port)
            *port = s->port;
        name = s->name;
    }
    pthread_mutex_unlock(&g_peer_mu);
    return name;
}

void peer_registry_done(const char *name, bool ok, double rtt_ms) {
    if (!name)
        return;
    pthread_mutex_lock(&g_peer_mu);
    for (int i = 0; i < g_peer_count; i++) {
        peer_slot_t *s = &g_peers[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->inflight > 0)
            s->inflight--;
        s->total_calls++;
        if (rtt_ms > 0)
            s->rtt_ewma_ms = s->rtt_ewma_ms > 0 ? 0.7 * s->rtt_ewma_ms + 0.3 * rtt_ms : rtt_ms;
        s->success_ewma = 0.75 * s->success_ewma + 0.25 * (ok ? 1.0 : 0.0);
        if (ok) {
            s->consec_failures = 0;
            s->tripped_until = 0;
        } else {
            s->total_failures++;
            s->consec_failures++;
            if (s->consec_failures >= PEER_TRIP_THRESHOLD) {
                long backoff = (long)PEER_TRIP_BASE_SECS * s->consec_failures;
                if (backoff > PEER_TRIP_MAX_SECS)
                    backoff = PEER_TRIP_MAX_SECS;
                s->tripped_until = time(NULL) + backoff;
            }
        }
        break;
    }
    pthread_mutex_unlock(&g_peer_mu);
}

/* ── Heartbeat: keep liveness/latency fresh ─────────────────────────────── */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void peer_probe_one(const char *name, const char *addr, int port) {
    const char *keystr = getenv("DSCO_NET_AUTH_KEY");
    const uint8_t *key = (keystr && keystr[0]) ? (const uint8_t *)keystr : NULL;
    size_t klen = key ? strlen(keystr) : 0;
    double t0 = now_ms();
    /* POST /health proves the TCP+TLS+HTTP round-trip succeeded; a non-NULL
     * body (even a 404) means the peer is reachable. */
#if PEER_NET_AVAILABLE
    char *resp = netsrv_client_post(addr, (uint16_t)port, "/health", "{}", key, klen, true);
#else
    (void)addr;
    (void)port;
    (void)key;
    (void)klen;
    char *resp = NULL;
#endif
    double rtt = now_ms() - t0;
    bool ok = resp != NULL;
    free(resp);
    peer_registry_done(name, ok, ok ? rtt : 0.0);
}

static void *peer_heartbeat_thread(void *arg) {
    (void)arg;
    long interval = 15;
    const char *env = getenv("DSCO_PEER_HEARTBEAT_SECS");
    if (env && env[0]) {
        long v = atol(env);
        if (v >= 3 && v <= 3600)
            interval = v;
    }
    for (;;) {
        /* Snapshot peer targets under lock, probe without holding it. */
        char names[PEER_REGISTRY_MAX][64];
        char addrs[PEER_REGISTRY_MAX][128];
        int ports[PEER_REGISTRY_MAX];
        int n = 0;
        pthread_mutex_lock(&g_peer_mu);
        for (int i = 0; i < g_peer_count && n < PEER_REGISTRY_MAX; i++) {
            snprintf(names[n], sizeof(names[n]), "%s", g_peers[i].name);
            snprintf(addrs[n], sizeof(addrs[n]), "%s", g_peers[i].addr);
            ports[n] = g_peers[i].port;
            n++;
        }
        pthread_mutex_unlock(&g_peer_mu);
        for (int i = 0; i < n; i++)
            peer_probe_one(names[i], addrs[i], ports[i]);
        for (long s = 0; s < interval; s++)
            sleep(1);
    }
    return NULL;
}

static void heartbeat_spawn_once(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, peer_heartbeat_thread, NULL) == 0)
        pthread_detach(t);
}

void peer_registry_start_heartbeat(void) {
    if (!peer_registry_enabled() || g_peer_count == 0)
        return;
    pthread_once(&g_hb_once, heartbeat_spawn_once);
}

int peer_registry_count(void) {
    pthread_mutex_lock(&g_peer_mu);
    int n = g_peer_count;
    pthread_mutex_unlock(&g_peer_mu);
    return n;
}

void peer_registry_render(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t pos = 0;
    int n = snprintf(out + pos, out_len - pos, "  %-14s %-20s %6s %8s %6s %5s %7s\n", "peer",
                     "addr", "port", "rtt(ms)", "ok%", "load", "score");
    if (n > 0)
        pos += (size_t)n;
    pthread_mutex_lock(&g_peer_mu);
    time_t now = time(NULL);
    for (int i = 0; i < g_peer_count && pos < out_len; i++) {
        peer_slot_t *s = &g_peers[i];
        bool avail = peer_available_locked(s, now);
        double sc =
            peer_registry_score_components(s->rtt_ewma_ms, s->success_ewma, s->inflight, avail);
        char score_buf[16];
        if (sc >= 1e9)
            snprintf(score_buf, sizeof(score_buf), "tripped");
        else
            snprintf(score_buf, sizeof(score_buf), "%.0f", sc);
        double ok_pct = s->total_calls > 0 ? s->success_ewma * 100.0 : 100.0;
        n = snprintf(out + pos, out_len - pos, "  %-14s %-20s %6d %8.0f %5.0f%% %5d %7s\n", s->name,
                     s->addr, s->port, s->rtt_ewma_ms, ok_pct, s->inflight, score_buf);
        if (n <= 0)
            break;
        pos += (size_t)n;
    }
    pthread_mutex_unlock(&g_peer_mu);
    if (pos < out_len)
        out[pos] = '\0';
    else if (out_len > 0)
        out[out_len - 1] = '\0';
}
