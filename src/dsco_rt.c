#include "dsco_rt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── dsco_buf (#3) ──────────────────────────────────────────────────────── */

void dsco_buf_init(dsco_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void dsco_buf_free(dsco_buf *b) {
    if (!b)
        return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

bool dsco_buf_reserve(dsco_buf *b, size_t extra) {
    if (b->cap - b->len >= extra)
        return true;
    size_t need = b->len + extra;
    size_t nc = b->cap ? b->cap * 2 : 64;
    while (nc < need)
        nc *= 2;
    unsigned char *nd = realloc(b->data, nc);
    if (!nd)
        return false;
    b->data = nd;
    b->cap = nc;
    return true;
}

bool dsco_buf_append(dsco_buf *b, const void *data, size_t n) {
    if (n && !dsco_buf_reserve(b, n))
        return false;
    if (n && data)
        memcpy(b->data + b->len, data, n);
    b->len += n;
    return true;
}

bool dsco_buf_append_u8(dsco_buf *b, unsigned char c) {
    if (!dsco_buf_reserve(b, 1))
        return false;
    b->data[b->len++] = c;
    return true;
}

void *dsco_buf_reserve_ptr(dsco_buf *b, size_t n) {
    if (!dsco_buf_reserve(b, n))
        return NULL;
    return b->data + b->len;
}

void dsco_buf_commit(dsco_buf *b, size_t n) {
    if (b->len + n <= b->cap)
        b->len += n;
}

void dsco_buf_reset(dsco_buf *b) {
    b->len = 0;
}

size_t dsco_buf_len(const dsco_buf *b) {
    return b ? b->len : 0;
}

const unsigned char *dsco_buf_data(const dsco_buf *b) {
    return b ? b->data : NULL;
}

/* ── dsco_frame (#90 ★RT) ───────────────────────────────────────────────── */

uint32_t dsco_crc32(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool dsco_frame_encode(dsco_buf *out, uint16_t type, uint32_t seq, const void *payload,
                       uint32_t len) {
    if (!out || len > DSCO_FRAME_MAX_LEN)
        return false;
    unsigned char *slot = dsco_buf_reserve_ptr(out, DSCO_FRAME_HDR_LEN + len);
    if (!slot)
        return false;
    put_u16(slot + 0, (uint16_t)DSCO_FRAME_MAGIC);
    put_u16(slot + 2, type);
    put_u32(slot + 4, seq);
    put_u32(slot + 8, len);
    put_u32(slot + 12, dsco_crc32(payload, len));
    if (len && payload)
        memcpy(slot + DSCO_FRAME_HDR_LEN, payload, len);
    dsco_buf_commit(out, DSCO_FRAME_HDR_LEN + len);
    return true;
}

dsco_frame_status dsco_frame_decode(const void *buf, size_t avail, dsco_frame_header *hdr,
                                    const unsigned char **payload, size_t *consumed) {
    const unsigned char *p = (const unsigned char *)buf;
    if (avail < DSCO_FRAME_HDR_LEN)
        return DSCO_FRAME_INCOMPLETE;
    if (get_u16(p) != (uint16_t)DSCO_FRAME_MAGIC)
        return DSCO_FRAME_BAD;
    uint32_t len = get_u32(p + 8);
    if (len > DSCO_FRAME_MAX_LEN)
        return DSCO_FRAME_BAD;
    if (avail < (size_t)DSCO_FRAME_HDR_LEN + len)
        return DSCO_FRAME_INCOMPLETE;
    uint32_t crc = get_u32(p + 12);
    if (dsco_crc32(p + DSCO_FRAME_HDR_LEN, len) != crc)
        return DSCO_FRAME_BAD;
    if (hdr) {
        hdr->type = get_u16(p + 2);
        hdr->seq = get_u32(p + 4);
        hdr->len = len;
    }
    if (payload)
        *payload = p + DSCO_FRAME_HDR_LEN;
    if (consumed)
        *consumed = (size_t)DSCO_FRAME_HDR_LEN + len;
    return DSCO_FRAME_OK;
}

/* ── dsco_hist (#127 ★RT) ───────────────────────────────────────────────── */

#define HIST_SUB 16     /* linear sub-buckets per octave */
#define HIST_OCTAVES 64 /* exponents 0..63 */
#define HIST_NBUCKETS (HIST_SUB * HIST_OCTAVES)

struct dsco_hist {
    uint64_t buckets[HIST_NBUCKETS];
    uint64_t total;
    double vmin, vmax;
};

dsco_hist *dsco_hist_new(void) {
    dsco_hist *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->vmin = 0;
    h->vmax = 0;
    return h;
}

void dsco_hist_free(dsco_hist *h) {
    free(h);
}

static int hist_index(double v) {
    if (v <= 1.0)
        return 0; /* sub-unit values collapse to the first bucket */
    int e;
    (void)frexp(v, &e); /* v = m * 2^e, m in [0.5,1) → v in [2^(e-1), 2^e) */
    int oct = e - 1;
    if (oct < 0)
        oct = 0;
    if (oct >= HIST_OCTAVES)
        oct = HIST_OCTAVES - 1;
    double base = ldexp(1.0, oct); /* 2^oct */
    double width = base / HIST_SUB;
    int sub = (int)((v - base) / width);
    if (sub < 0)
        sub = 0;
    if (sub >= HIST_SUB)
        sub = HIST_SUB - 1;
    return oct * HIST_SUB + sub;
}

static double hist_bucket_mid(int idx) {
    int oct = idx / HIST_SUB;
    int sub = idx % HIST_SUB;
    double base = ldexp(1.0, oct);
    double width = base / HIST_SUB;
    return base + (sub + 0.5) * width;
}

void dsco_hist_add(dsco_hist *h, double value) {
    if (!h || value < 0 || !isfinite(value))
        return;
    if (h->total == 0)
        h->vmin = h->vmax = value;
    else {
        if (value < h->vmin)
            h->vmin = value;
        if (value > h->vmax)
            h->vmax = value;
    }
    h->buckets[hist_index(value)]++;
    h->total++;
}

uint64_t dsco_hist_count(const dsco_hist *h) {
    return h ? h->total : 0;
}

double dsco_hist_min(const dsco_hist *h) {
    return (h && h->total) ? h->vmin : 0;
}

double dsco_hist_max(const dsco_hist *h) {
    return (h && h->total) ? h->vmax : 0;
}

double dsco_hist_quantile(const dsco_hist *h, double q) {
    if (!h || h->total == 0)
        return 0;
    if (q <= 0)
        return h->vmin;
    if (q >= 1)
        return h->vmax;
    uint64_t target = (uint64_t)(q * (double)h->total);
    if (target >= h->total)
        target = h->total - 1;
    uint64_t cum = 0;
    for (int i = 0; i < HIST_NBUCKETS; i++) {
        cum += h->buckets[i];
        if (cum > target) {
            double est = hist_bucket_mid(i);
            if (est < h->vmin)
                est = h->vmin;
            if (est > h->vmax)
                est = h->vmax;
            return est;
        }
    }
    return h->vmax;
}

void dsco_hist_merge(dsco_hist *dst, const dsco_hist *src) {
    if (!dst || !src || src->total == 0)
        return;
    for (int i = 0; i < HIST_NBUCKETS; i++)
        dst->buckets[i] += src->buckets[i];
    if (dst->total == 0) {
        dst->vmin = src->vmin;
        dst->vmax = src->vmax;
    } else {
        if (src->vmin < dst->vmin)
            dst->vmin = src->vmin;
        if (src->vmax > dst->vmax)
            dst->vmax = src->vmax;
    }
    dst->total += src->total;
}

/* ── dsco_ratelimit (#83) ───────────────────────────────────────────────── */

void dsco_ratelimit_init(dsco_ratelimit *r, double rate_per_sec, double burst) {
    r->capacity = burst > 0 ? burst : 1;
    r->tokens = r->capacity;
    r->rate = rate_per_sec > 0 ? rate_per_sec : 0;
    r->last_ns = 0;
}

bool dsco_ratelimit_allow(dsco_ratelimit *r, double now_ns, double cost) {
    if (!r)
        return false;
    double elapsed = (now_ns - r->last_ns) / 1e9; /* ns → s */
    if (elapsed > 0) {
        r->tokens += elapsed * r->rate;
        if (r->tokens > r->capacity)
            r->tokens = r->capacity;
        r->last_ns = now_ns;
    }
    if (cost < 0)
        cost = 0;
    if (r->tokens >= cost) {
        r->tokens -= cost;
        return true;
    }
    return false;
}

double dsco_ratelimit_tokens(const dsco_ratelimit *r) {
    return r ? r->tokens : 0;
}

/* ── dsco_credits (#86 ★RT) ─────────────────────────────────────────────── */

void dsco_credits_init(dsco_credits *c, int64_t initial) {
    if (initial < 0)
        initial = 0;
    c->available = initial;
    c->granted_total = initial;
    c->consumed_total = 0;
}

bool dsco_credits_try_consume(dsco_credits *c, int64_t n) {
    if (!c || n < 0)
        return false;
    if (c->available < n)
        return false; /* backpressure */
    c->available -= n;
    c->consumed_total += n;
    return true;
}

void dsco_credits_grant(dsco_credits *c, int64_t n) {
    if (!c || n <= 0)
        return;
    c->available += n;
    c->granted_total += n;
}

int64_t dsco_credits_available(const dsco_credits *c) {
    return c ? c->available : 0;
}
