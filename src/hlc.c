#include "hlc.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static pthread_mutex_t g_hlc_mu = PTHREAD_MUTEX_INITIALIZER;
static hlc_t g_hlc = {0, 0};

int64_t hlc_phys_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

hlc_t hlc_now(void) {
    int64_t phys = hlc_phys_now_ms();
    pthread_mutex_lock(&g_hlc_mu);
    if (phys > g_hlc.pt_ms) {
        g_hlc.pt_ms = phys;
        g_hlc.logical = 0;
    } else {
        g_hlc.logical++;
    }
    hlc_t out = g_hlc;
    pthread_mutex_unlock(&g_hlc_mu);
    return out;
}

hlc_t hlc_update(hlc_t remote, bool *did_clamp) {
    int64_t phys = hlc_phys_now_ms();
    int64_t max_remote = phys + 60000LL;
    bool clamped = false;
    if (remote.pt_ms > max_remote) {
        remote.pt_ms = max_remote;
        clamped = true;
    }

    pthread_mutex_lock(&g_hlc_mu);
    int64_t last_pt = g_hlc.pt_ms;
    uint32_t last_logical = g_hlc.logical;
    int64_t max_pt = phys;
    if (last_pt > max_pt)
        max_pt = last_pt;
    if (remote.pt_ms > max_pt)
        max_pt = remote.pt_ms;

    uint32_t logical;
    if (max_pt == last_pt && max_pt == remote.pt_ms) {
        logical = last_logical > remote.logical ? last_logical : remote.logical;
        logical++;
    } else if (max_pt == last_pt) {
        logical = last_logical + 1;
    } else if (max_pt == remote.pt_ms) {
        logical = remote.logical + 1;
    } else {
        logical = 0;
    }

    g_hlc.pt_ms = max_pt;
    g_hlc.logical = logical;
    hlc_t out = g_hlc;
    pthread_mutex_unlock(&g_hlc_mu);

    if (did_clamp)
        *did_clamp = clamped;
    return out;
}

int hlc_compare(hlc_t a, hlc_t b) {
    if (a.pt_ms < b.pt_ms)
        return -1;
    if (a.pt_ms > b.pt_ms)
        return 1;
    if (a.logical < b.logical)
        return -1;
    if (a.logical > b.logical)
        return 1;
    return 0;
}

void hlc_encode(hlc_t ts, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    snprintf(out, out_len, "%016llx:%08x", (unsigned long long)ts.pt_ms, ts.logical);
}

bool hlc_decode(const char *s, hlc_t *out) {
    if (!s || !out)
        return false;
    unsigned long long pt = 0;
    unsigned int logical = 0;
    if (sscanf(s, "%llx:%x", &pt, &logical) != 2)
        return false;
    out->pt_ms = (int64_t)pt;
    out->logical = (uint32_t)logical;
    return true;
}

uint64_t hlc_pack(hlc_t ts) {
    uint64_t logical = ts.logical < 0xffffu ? (uint64_t)ts.logical : 0xffffu;
    return ((uint64_t)ts.pt_ms << 16) | logical;
}

hlc_t hlc_unpack(uint64_t packed) {
    hlc_t ts;
    ts.pt_ms = (int64_t)(packed >> 16);
    ts.logical = (uint32_t)(packed & 0xffffu);
    return ts;
}

hlc_t hlc_peek(void) {
    pthread_mutex_lock(&g_hlc_mu);
    hlc_t out = g_hlc;
    pthread_mutex_unlock(&g_hlc_mu);
    return out;
}

void hlc_reset(void) {
    pthread_mutex_lock(&g_hlc_mu);
    memset(&g_hlc, 0, sizeof(g_hlc));
    pthread_mutex_unlock(&g_hlc_mu);
}
