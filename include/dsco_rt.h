#ifndef DSCO_RT_H
#define DSCO_RT_H

/* ── dsco runtime / flow / observability primitives ─────────────────────────
 * Companion to dsco_ds.h (pure containers). These are the Tier-2 runtime
 * critical-path structures from the DSCO Data Structure Catalog — every one
 * carries its catalog row so a future "should we build X?" resolves by
 * citation, not vibes. Nothing here is internally locked; a shared instance is
 * guarded by its caller. Allocating types return false/NULL on OOM.
 *
 *   dsco_buf       — #3   T2      reserve/commit byte buffer
 *   dsco_frame     — #90  T2 ★RT  length+CRC framed codec (the activation wire)
 *   dsco_hist      — #127 T2 ★RT  log-bucketed percentiles, mergeable
 *   dsco_ratelimit — #83  T2      token bucket (429 discipline)
 *   dsco_credits   — #86  T2 ★RT  flow-control window (pipeline backpressure)
 *
 * Time is injected (callers pass `now`) so the time-dependent primitives are
 * deterministic and unit-testable without a wall clock. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── dsco_buf (#3): byte buffer with reserve/commit ─────────────────────────
 * The write-into-uninitialized-then-commit pattern used by SSE assembly and
 * frame encoding, distinct from dsco_vec's copy-in push. */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} dsco_buf;

void dsco_buf_init(dsco_buf *b);
void dsco_buf_free(dsco_buf *b);
bool dsco_buf_reserve(dsco_buf *b, size_t extra); /* ensure cap >= len+extra */
bool dsco_buf_append(dsco_buf *b, const void *data, size_t n);
bool dsco_buf_append_u8(dsco_buf *b, unsigned char c);
/* Reserve n writable bytes at the tail and return a pointer to them WITHOUT
 * advancing len; write into it, then dsco_buf_commit(n). Pointer is invalidated
 * by the next reserve/append. Returns NULL on OOM. */
void *dsco_buf_reserve_ptr(dsco_buf *b, size_t n);
void dsco_buf_commit(dsco_buf *b, size_t n); /* advance len (<= reserved) */
void dsco_buf_reset(dsco_buf *b);            /* len=0, keep cap */
size_t dsco_buf_len(const dsco_buf *b);
const unsigned char *dsco_buf_data(const dsco_buf *b);

/* ── dsco_frame (#90 ★RT): length-prefixed framed codec ─────────────────────
 * The activation wire, remote control, and fleet RPC framing. Wire layout,
 * little-endian: [magic u16][type u16][seq u32][len u32][crc32 u32][payload].
 * crc32 (IEEE) is over the payload only. Header is a fixed 16 bytes. */
#define DSCO_FRAME_MAGIC 0xD5C0u
#define DSCO_FRAME_HDR_LEN 16u
#define DSCO_FRAME_MAX_LEN (64u * 1024u * 1024u) /* reject absurd lengths */

typedef struct {
    uint16_t type;
    uint32_t seq;
    uint32_t len;
} dsco_frame_header;

typedef enum {
    DSCO_FRAME_OK = 0,     /* one full frame parsed */
    DSCO_FRAME_INCOMPLETE, /* need more bytes; call again when more arrive */
    DSCO_FRAME_BAD         /* magic/length/crc invalid — desync, drop the stream */
} dsco_frame_status;

/* Encode a frame onto `out`. Returns false on OOM (or len > DSCO_FRAME_MAX_LEN). */
bool dsco_frame_encode(dsco_buf *out, uint16_t type, uint32_t seq, const void *payload,
                       uint32_t len);
/* Try to parse one frame from `buf`/`avail`. On OK, fills *hdr, points *payload
 * into buf (not copied), and sets *consumed to the total bytes used. */
dsco_frame_status dsco_frame_decode(const void *buf, size_t avail, dsco_frame_header *hdr,
                                    const unsigned char **payload, size_t *consumed);
/* Standalone IEEE crc32 (also used internally). */
uint32_t dsco_crc32(const void *data, size_t n);

/* ── dsco_hist (#127 ★RT): log-bucketed percentile histogram ────────────────
 * Fixed log-linear buckets (per-octave, 16 linear sub-buckets → ~6% relative
 * error) so honest p50/p90/p99 are available from token one, and two nodes'
 * histograms merge exactly because the bucket layout is aligned. */
typedef struct dsco_hist dsco_hist;

dsco_hist *dsco_hist_new(void);
void dsco_hist_free(dsco_hist *h);
void dsco_hist_add(dsco_hist *h, double value); /* value >= 0 */
uint64_t dsco_hist_count(const dsco_hist *h);
double dsco_hist_min(const dsco_hist *h); /* NaN-free: 0 if empty */
double dsco_hist_max(const dsco_hist *h);
/* q in [0,1]; returns an estimate within the bucket's relative error, clamped
 * to [min,max]. q<=0 → min, q>=1 → max. Empty → 0. */
double dsco_hist_quantile(const dsco_hist *h, double q);
void dsco_hist_merge(dsco_hist *dst, const dsco_hist *src); /* aligned add */

/* ── dsco_ratelimit (#83): token bucket ─────────────────────────────────────
 * Provider 429 discipline and per-client serve limits. Time is injected in
 * nanoseconds so refill is deterministic and testable. */
typedef struct {
    double capacity; /* burst size (max tokens) */
    double tokens;   /* current tokens */
    double rate;     /* tokens per second */
    double last_ns;  /* timestamp of last refill (caller clock, ns) */
} dsco_ratelimit;

void dsco_ratelimit_init(dsco_ratelimit *r, double rate_per_sec, double burst);
/* Refill for the elapsed time since last call, then attempt to take `cost`
 * tokens. Returns true (allowed, tokens deducted) or false (throttled). */
bool dsco_ratelimit_allow(dsco_ratelimit *r, double now_ns, double cost);
double dsco_ratelimit_tokens(const dsco_ratelimit *r);

/* ── dsco_credits (#86 ★RT): flow-control credit window ─────────────────────
 * Credit-based backpressure per pipeline edge (chrysalis must not flood matrix):
 * the sender consumes credits to transmit; the receiver grants them as it
 * drains. try_consume fails (backpressure) when credits are exhausted. */
typedef struct {
    int64_t available;
    int64_t granted_total;  /* lifetime credits granted (for accounting) */
    int64_t consumed_total; /* lifetime credits consumed */
} dsco_credits;

void dsco_credits_init(dsco_credits *c, int64_t initial);
bool dsco_credits_try_consume(dsco_credits *c, int64_t n); /* false if insufficient */
void dsco_credits_grant(dsco_credits *c, int64_t n);       /* replenish */
int64_t dsco_credits_available(const dsco_credits *c);

#endif /* DSCO_RT_H */
