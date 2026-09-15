#include "peer_bootstrap.h"
#include "audit_log.h"
#include "waiter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>

#ifdef HAVE_LIBSODIUM
#include <dirent.h>
#include <sodium.h>
#include "mesh.h"

/* ── state ──────────────────────────────────────────────────────────────── */

static mesh_node_t *s_node = NULL;
static uint16_t s_port = 0;
static pthread_t s_thread;
static atomic_int s_running = 0;
static dsco_waiter_t s_waiter;
static atomic_int s_waiter_init = 0;

/* ── flat-file seed list ────────────────────────────────────────────────── */

static const char *seeds_path(void) {
    static char buf[4096];
    if (buf[0])
        return buf;
    const char *h = getenv("HOME");
    snprintf(buf, sizeof(buf), "%s/.dsco/peers.txt", h ? h : "/tmp");
    return buf;
}

static bool discovery_cancelled(void *ctx) {
    (void)ctx;
    return !atomic_load(&s_running);
}

static void try_connect(const char *host, uint16_t port) {
    if (!s_node || discovery_cancelled(NULL))
        return;
    char addr[256];
    snprintf(addr, sizeof(addr), "%s:%u", host, port);
    char msg[512];
    snprintf(msg, sizeof(msg), "peer_bootstrap: connecting %s", addr);
    audit_log("peer", msg);
    mesh_node_connect_interruptible(s_node, host, port, discovery_cancelled, NULL);
}

static void read_seed_file(void) {
    /* check DSCO_PEERS env var (comma-separated) */
    const char *env = getenv("DSCO_PEERS");
    if (env && env[0]) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", env);
        char *tok = strtok(buf, ",; \t\n");
        while (tok && atomic_load(&s_running)) {
            char host[256] = {0};
            int port = (int)s_port;
            /* parse host:port */
            char *colon = strrchr(tok, ':');
            if (colon) {
                size_t hlen = (size_t)(colon - tok);
                if (hlen < sizeof(host)) {
                    memcpy(host, tok, hlen);
                    host[hlen] = '\0';
                }
                port = atoi(colon + 1);
            } else {
                snprintf(host, sizeof(host), "%s", tok);
            }
            if (host[0] && port > 0)
                try_connect(host, (uint16_t)port);
            tok = strtok(NULL, ",; \t\n");
        }
        return;
    }

    /* flat file */
    FILE *f = fopen(seeds_path(), "r");
    if (!f)
        return;
    char line[512];
    while (atomic_load(&s_running) && fgets(line, sizeof(line), f)) {
        /* strip comment / newline */
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (!line[0])
            continue;

        char host[256] = {0};
        int port = (int)s_port;
        char *colon = strrchr(line, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - line);
            if (hlen < sizeof(host)) {
                memcpy(host, line, hlen);
                host[hlen] = '\0';
            }
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", line);
        }
        if (host[0] && port > 0)
            try_connect(host, (uint16_t)port);
    }
    fclose(f);
}

/* ── Read ~/bridge/fleet/ *.host and connect to mesh ─────────────────────── */
static void read_bridge_fleet(void) {
    if (!s_node)
        return;
    const char *home = getenv("HOME");
    if (!home)
        return;

    char fleet_dir[512];
    snprintf(fleet_dir, sizeof(fleet_dir), "%s/bridge/fleet", home);

    DIR *d = opendir(fleet_dir);
    if (!d)
        return;

    struct dirent *ent;
    while (atomic_load(&s_running) && (ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 6 || strcmp(ent->d_name + nl - 5, ".host") != 0)
            continue;

        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%s", fleet_dir, ent->d_name);

        FILE *f = fopen(fpath, "r");
        if (!f)
            continue;

        char addr[128] = {0}, name[64] = {0};
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[256];
            char *nl2 = strchr(line, '\n');
            if (nl2)
                *nl2 = '\0';
            if (sscanf(line, "%63[^=]=\"%255[^\"]\"", key, val) == 2) {
                if (strcmp(key, "ADDR") == 0)
                    snprintf(addr, sizeof(addr), "%s", val);
                else if (strcmp(key, "NAME") == 0)
                    snprintf(name, sizeof(name), "%s", val);
            }
        }
        fclose(f);

        if (!addr[0] || !addr[0])
            continue;

        /* Skip self (empty addr or same hostname) */
        char self_hostname[256] = {0};
        gethostname(self_hostname, sizeof(self_hostname));
        /* strip .local suffix for comparison */
        char *dot = strchr(self_hostname, '.');
        if (dot)
            *dot = '\0';
        if (strcmp(name, self_hostname) == 0)
            continue;

        /* Connect on default mesh port */
        try_connect(addr, s_port);
    }
    closedir(d);
}

/* ── mDNS advertisement (macOS dns_sd) ─────────────────────────────────── */

#ifdef __APPLE__
#include <dns_sd.h>
#include <poll.h>
#include <fcntl.h>
#include <arpa/inet.h>

static DNSServiceRef s_sdref = NULL;
static pthread_t s_mdns_thread;
static bool s_mdns_started = false;
static int s_mdns_wake[2] = {-1, -1};

static void mdns_wake_close(void) {
    for (int i = 0; i < 2; i++) {
        if (s_mdns_wake[i] >= 0) close(s_mdns_wake[i]);
        s_mdns_wake[i] = -1;
    }
}

static void mdns_wake_init(void) {
    if (pipe(s_mdns_wake) != 0) return;
    for (int i = 0; i < 2; i++) {
        if (fcntl(s_mdns_wake[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(s_mdns_wake[i], F_SETFL, O_NONBLOCK) < 0) {
            mdns_wake_close();
            return;
        }
    }
}

static __attribute__((unused)) void DNSSD_API mdns_cb(DNSServiceRef sdRef, DNSServiceFlags flags,
                                                      uint32_t interfaceIndex,
                                                      DNSServiceErrorType err, const char *fullname,
                                                      const char *hosttarget, uint16_t port_net,
                                                      uint16_t txtLen,
                                                      const unsigned char *txtRecord, void *ctx) {
    (void)sdRef;
    (void)flags;
    (void)interfaceIndex;
    (void)ctx;
    (void)txtLen;
    (void)txtRecord;
    (void)fullname;
    if (err != kDNSServiceErr_NoError)
        return;
    uint16_t port = ntohs(port_net);
    /* avoid connecting to ourselves */
    if (port == s_port) {
        /* could check hostname == our hostname to be sure */
    }
    try_connect(hosttarget, port);
}

static void *mdns_loop(void *arg) {
    (void)arg;
    /* Advertise _dsco._tcp */
    DNSServiceErrorType e = DNSServiceRegister(&s_sdref, 0, 0, NULL, "_dsco._tcp", NULL, NULL,
                                               htons(s_port), 0, NULL, NULL, NULL);
    if (e != kDNSServiceErr_NoError)
        return NULL;

    /* Browse for peers */
    DNSServiceRef browse_ref = NULL;
    e = DNSServiceBrowse(&browse_ref, 0, 0, "_dsco._tcp", NULL, NULL, NULL);

    while (atomic_load(&s_running)) {
        /* Stop wakes the same wait as DNS activity. Keep the old bounded
         * timeout only if the wake pipe could not be created. */
        struct pollfd fds[] = {
            {DNSServiceRefSockFD(s_sdref), POLLIN, 0},
            {browse_ref ? DNSServiceRefSockFD(browse_ref) : -1, POLLIN, 0},
            {s_mdns_wake[0], POLLIN, 0},
        };
        int ready = poll(fds, 3, s_mdns_wake[0] >= 0 ? -1 : 1000);
        if (!atomic_load(&s_running)) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN)
            DNSServiceProcessResult(s_sdref);
        if (browse_ref && (fds[1].revents & POLLIN))
            DNSServiceProcessResult(browse_ref);
        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }
    DNSServiceRefDeallocate(s_sdref);
    s_sdref = NULL;
    if (browse_ref)
        DNSServiceRefDeallocate(browse_ref);
    return NULL;
}
#endif /* __APPLE__ */

/* ── reseed / discovery thread ──────────────────────────────────────────── */

static void *discovery_thread(void *arg) {
    (void)arg;
    /* initial seed read */
    read_seed_file();
    read_bridge_fleet();

    /* Re-read every 60s. Interruptible: stop() wakes instantly, and the
     * thread makes exactly one wakeup per minute instead of 60. */
    while (atomic_load(&s_running)) {
        dsco_waiter_wait_ms(&s_waiter, 60000);
        if (!atomic_load(&s_running))
            break;
        read_seed_file();
        read_bridge_fleet();
    }
    return NULL;
}

/* ── public API ─────────────────────────────────────────────────────────── */

void peer_bootstrap_init(void *mesh_node, uint16_t port) {
    s_node = (mesh_node_t *)mesh_node;
    s_port = port;
    if (!atomic_exchange(&s_waiter_init, 1))
        dsco_waiter_init(&s_waiter);
    else
        dsco_waiter_reset(&s_waiter);
    atomic_store(&s_running, 1);

    audit_log("peer_bootstrap", "starting peer discovery");

#ifdef __APPLE__
    mdns_wake_init();
    s_mdns_started = pthread_create(&s_mdns_thread, NULL, mdns_loop, NULL) == 0;
    if (!s_mdns_started) mdns_wake_close();
#endif

    pthread_create(&s_thread, NULL, discovery_thread, NULL);
}

void peer_bootstrap_stop(void) {
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_running, 0);
    dsco_waiter_stop(&s_waiter); /* instant wakeup instead of ≤1s lag */
#ifdef __APPLE__
    if (s_mdns_wake[1] >= 0) {
        char byte = 1;
        ssize_t written;
        do { written = write(s_mdns_wake[1], &byte, 1); } while (written < 0 && errno == EINTR);
    }
#endif
    pthread_join(s_thread, NULL);
#ifdef __APPLE__
    if (s_mdns_started) pthread_join(s_mdns_thread, NULL);
    s_mdns_started = false;
    mdns_wake_close();
#endif
}

void peer_bootstrap_reseed(void) {
    if (s_node)
        read_seed_file();
    if (atomic_load(&s_waiter_init))
        dsco_waiter_signal(&s_waiter); /* also refresh bridge fleet promptly */
}

#else /* !HAVE_LIBSODIUM — stubs */

void peer_bootstrap_init(void *n, uint16_t p) {
    (void)n;
    (void)p;
}
void peer_bootstrap_stop(void) {}
void peer_bootstrap_reseed(void) {}

#endif /* HAVE_LIBSODIUM */
