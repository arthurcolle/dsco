#ifndef DSCO_RUNTIME_H
#define DSCO_RUNTIME_H

/* ── dsco runtime spine ──────────────────────────────────────────────────────
 * The activation-path structures from the DSCO Data Structure Specs (the
 * "Buildable 28", runtime track). Companion to dsco_ds.h (containers) and
 * dsco_rt.h (buf/frame/hist/ratelimit/credits). Every type carries its catalog
 * row. Concurrency-transport types (spsc/mpsc/seqlock/chan/cow) specify their
 * ordering per-entry; everything else is single-writer / caller-guarded.
 *
 *   dsco_arena     #8  ★RT  page-chain bump allocator, reset keeps pages
 *   dsco_spsc      #11 ★RT  single-producer/single-consumer ring (wait-free)
 *   dsco_mpsc      #12 ★RT  multi-producer/single-consumer intrusive queue
 *   dsco_tensorview#13 ★RT  activation descriptor + 40-byte wire desc
 *   dsco_ringplan  #15 ★RT  epoch'd pipeline topology
 *   dsco_tracering #17 ★RT  per-thread flight recorder
 *   dsco_chan      #19      blocking channel (mutex + condvars)
 *   dsco_seqlock   #20      snapshot for small POD stats
 *   dsco_kvblocks  #24      paged KV bookkeeping (milestone 2+)
 *   dsco_wal       #25      append-only log w/ torn-tail recovery
 *   dsco_cow       #26      copy-on-write snapshot swap
 *   dsco_swim      #27 NEW  membership w/ incarnations (fault chain)
 *   dsco_seqrec    #28 NEW  the serve endpoint's unit of work
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsco_ds.h" /* dsco_vec, dsco_queue */
#include "dsco_rt.h" /* dsco_buf, dsco_crc32 */

/* ── dsco_arena (#8 ★RT) ────────────────────────────────────────────────── */
typedef struct dsco_arena_page {
    struct dsco_arena_page *next;
    size_t used;
    size_t cap;
    /* page bytes follow this header in the same allocation */
} dsco_arena_page;

typedef struct {
    dsco_arena_page *first;
    dsco_arena_page *cur;
    size_t page_sz;
} dsco_arena;

bool dsco_arena_init(dsco_arena *a, size_t page_sz);
void *dsco_arena_alloc(dsco_arena *a, size_t n, size_t align);  /* NULL on OOM */
void *dsco_arena_alloc0(dsco_arena *a, size_t n, size_t align); /* zeroed */
void dsco_arena_reset(dsco_arena *a); /* used=0 on every page; keeps memory */
void dsco_arena_free(dsco_arena *a);

/* ── dsco_spsc (#11 ★RT): single-producer/single-consumer ring ──────────── */
typedef struct {
    _Alignas(64) atomic_uint_fast64_t head; /* consumer-owned */
    _Alignas(64) atomic_uint_fast64_t tail; /* producer-owned */
    _Alignas(64) void **slots;
    uint64_t mask;
} dsco_spsc;

bool dsco_spsc_init(dsco_spsc *r, size_t cap /* power of two */);
void dsco_spsc_free(dsco_spsc *r);
bool dsco_spsc_push(dsco_spsc *r, void *item); /* producer thread only; false=full */
void *dsco_spsc_pop(dsco_spsc *r);             /* consumer thread only; NULL=empty */
size_t dsco_spsc_len(const dsco_spsc *r);

/* ── dsco_mpsc (#12 ★RT): multi-producer/single-consumer intrusive queue ── */
typedef struct dsco_mpsc_node {
    _Atomic(struct dsco_mpsc_node *) next;
} dsco_mpsc_node;

typedef struct {
    _Atomic(dsco_mpsc_node *) head;
    dsco_mpsc_node *tail;
    dsco_mpsc_node stub;
} dsco_mpsc;

void dsco_mpsc_init(dsco_mpsc *q);
void dsco_mpsc_push(dsco_mpsc *q, dsco_mpsc_node *n); /* any thread */
dsco_mpsc_node *dsco_mpsc_pop(dsco_mpsc *q);          /* consumer only; NULL=empty/retry */

/* ── dsco_tensorview (#13 ★RT): activation descriptor ───────────────────── */
enum { DSCO_DT_F16 = 1, DSCO_DT_F32 = 2, DSCO_DT_BF16 = 3 };
#define DSCO_TV_DESC_LEN 40u

typedef struct {
    uint8_t dtype;
    uint8_t ndim; /* 1..4 */
    uint16_t _pad;
    uint32_t shape[4];  /* elems per dim; unused dims = 1 */
    uint32_t stride[4]; /* in ELEMENTS; canonical row-major required for wire */
    void *data;         /* not serialized */
} dsco_tensorview;

size_t dsco_tv_dtype_size(uint8_t dtype);
size_t dsco_tv_nbytes(const dsco_tensorview *t);
bool dsco_tv_is_contig(const dsco_tensorview *t);
/* Encode 40-byte descriptor + raw elements onto out. Fails if non-contiguous. */
bool dsco_tv_enc(dsco_buf *out, const dsco_tensorview *t);
/* Parse descriptor from body, copy elements into arena, point t->data at them. */
bool dsco_tv_dec(dsco_tensorview *t, const uint8_t *body, size_t len, dsco_arena *a);

/* ── dsco_ringplan (#15 ★RT): epoch'd pipeline topology ─────────────────── */
#define DSCO_MAX_STAGES 16
typedef struct {
    uint32_t node_id;
    uint16_t layer_lo;
    uint16_t layer_hi;
    uint32_t needs_mb;
    uint32_t avail_mb;
} dsco_stage;

typedef struct {
    uint32_t epoch;
    uint8_t n;
    dsco_stage s[DSCO_MAX_STAGES];
} dsco_ringplan;

bool dsco_ringplan_enc(dsco_buf *out, const dsco_ringplan *p);
bool dsco_ringplan_dec(dsco_ringplan *p, const uint8_t *body, size_t len);
int dsco_ringplan_stage_of(const dsco_ringplan *p, uint32_t node_id);  /* index or -1 */
uint32_t dsco_ringplan_next(const dsco_ringplan *p, uint32_t node_id); /* ring successor */

/* ── dsco_tracering (#17 ★RT): flight recorder ──────────────────────────── */
enum { DSCO_TRACE_ENTER = 0, DSCO_TRACE_EXIT = 1 };
typedef struct {
    uint64_t req_id;
    uint64_t t_ns;
    uint8_t stage;
    uint8_t node;
    uint8_t ev;
    uint8_t _pad[5];
} dsco_trace_rec; /* 24 bytes */

typedef struct {
    dsco_trace_rec *recs;
    uint64_t cap; /* power of two */
    uint64_t head;
} dsco_tracering;

bool dsco_tracering_init(dsco_tracering *t, size_t cap /* power of two */);
void dsco_tracering_free(dsco_tracering *t);
void dsco_trace(dsco_tracering *t, uint64_t req_id, uint8_t stage, uint8_t node, uint8_t ev);
size_t dsco_tracering_count(const dsco_tracering *t);  /* records currently held */
void dsco_trace_dump(const dsco_tracering *t, int fd); /* async-signal-safe: raw records */

/* ── dsco_chan (#19): blocking channel ──────────────────────────────────── */
enum { DSCO_CHAN_OK = 0, DSCO_CHAN_CLOSED = 1, DSCO_CHAN_TIMEOUT = 2 };
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    dsco_queue q;
    bool closed;
} dsco_chan;

bool dsco_chan_init(dsco_chan *c, size_t elem_size, size_t cap);
void dsco_chan_free(dsco_chan *c);
bool dsco_chan_send(dsco_chan *c, const void *elem, int64_t timeout_ns); /* <0 = forever */
int dsco_chan_recv(dsco_chan *c, void *out, int64_t timeout_ns);         /* DSCO_CHAN_* */
bool dsco_chan_try_send(dsco_chan *c, const void *elem);
int dsco_chan_try_recv(dsco_chan *c, void *out);
void dsco_chan_close(dsco_chan *c);

/* ── dsco_seqlock (#20): snapshot for small POD stats ───────────────────── */
typedef struct {
    _Atomic uint32_t seq;
} dsco_seqlock;

void dsco_seqlock_init(dsco_seqlock *sl);
void dsco_seqlock_write_begin(dsco_seqlock *sl); /* single writer */
void dsco_seqlock_write_end(dsco_seqlock *sl);
uint32_t dsco_seqlock_read_begin(const dsco_seqlock *sl);          /* spins until even */
bool dsco_seqlock_read_retry(const dsco_seqlock *sl, uint32_t s0); /* true → retry */

/* ── dsco_kvblocks (#24): paged KV bookkeeping ──────────────────────────── */
#define DSCO_KV_INVALID 0xFFFFFFFFu
typedef struct {
    dsco_vec freelist; /* uint32_t block ids */
    uint32_t total_blocks;
    uint32_t block_tokens; /* tokens per block */
} dsco_kvblocks;

typedef struct {
    dsco_vec blocks; /* uint32_t block ids, index = pos/block_tokens */
} dsco_kvseq;

bool dsco_kvblocks_init(dsco_kvblocks *kv, uint32_t total_blocks, uint32_t block_tokens);
void dsco_kvblocks_free(dsco_kvblocks *kv);
uint32_t dsco_kvblocks_alloc(dsco_kvblocks *kv); /* block id or DSCO_KV_INVALID */
void dsco_kvblocks_release(dsco_kvblocks *kv, uint32_t block);
void dsco_kvseq_init(dsco_kvseq *s);
void dsco_kvseq_free_seq(dsco_kvblocks *kv, dsco_kvseq *s); /* returns blocks to pool */
/* Ensure a block exists for `pos`, returning its block id + in-block offset. */
bool dsco_kvseq_append(dsco_kvblocks *kv, dsco_kvseq *s, uint32_t pos, uint32_t *block_out,
                       uint32_t *off_out);
bool dsco_kvseq_translate(const dsco_kvblocks *kv, const dsco_kvseq *s, uint32_t pos,
                          uint32_t *block_out, uint32_t *off_out);

/* ── dsco_wal (#25): append-only log ────────────────────────────────────── */
enum { DSCO_WAL_FSYNC_NONE = 0, DSCO_WAL_FSYNC_INTERVAL = 1, DSCO_WAL_FSYNC_ALWAYS = 2 };
typedef struct {
    int fd;
    size_t seg_max;
    int fsync_policy;
    uint64_t written;
} dsco_wal;

bool dsco_wal_open(dsco_wal *w, const char *path, size_t seg_max, int fsync_policy);
bool dsco_wal_append(dsco_wal *w, uint16_t type, const void *payload, uint32_t len);
void dsco_wal_flush(dsco_wal *w);
void dsco_wal_close(dsco_wal *w);
/* Replay a WAL file: invoke fn per intact record; stop + truncate at the first
 * torn/short/bad-CRC record. Returns the number of records replayed. */
size_t dsco_wal_replay(const char *path,
                       void (*fn)(uint16_t type, const void *payload, uint32_t len, void *ud),
                       void *ud);

/* ── dsco_cow (#26): copy-on-write snapshot swap ────────────────────────── */
typedef struct dsco_cow_node {
    _Atomic int rc;
    void *data;
} dsco_cow_node;

typedef struct {
    _Atomic(dsco_cow_node *) cur;
    void (*free_fn)(void *); /* frees a retired snapshot's data */
} dsco_cow;

bool dsco_cow_init(dsco_cow *c, void *initial, void (*free_fn)(void *));
/* Pin the current snapshot: returns its data and (via *holder) the pin token. */
const void *dsco_cow_acquire(dsco_cow *c, dsco_cow_node **holder);
void dsco_cow_release(dsco_cow *c, dsco_cow_node *holder);
void dsco_cow_publish(dsco_cow *c, void *new_data); /* single writer */
void dsco_cow_free(dsco_cow *c);

/* ── dsco_swim (#27 NEW): membership with incarnations ──────────────────── */
typedef enum { DSCO_M_ALIVE = 0, DSCO_M_SUSPECT = 1, DSCO_M_DEAD = 2 } dsco_mstate;
typedef enum {
    DSCO_SWIM_JOINED = 0,
    DSCO_SWIM_SUSPECT = 1,
    DSCO_SWIM_DEAD = 2,
    DSCO_SWIM_REFUTED = 3
} dsco_swim_ev;

typedef struct {
    uint32_t node_id;
    dsco_mstate state;
    uint32_t incarnation;
    uint64_t last_ack_ns;
    uint64_t suspect_since_ns;
} dsco_swim_member;

typedef struct dsco_swim {
    dsco_swim_member *m;
    size_t n, cap;
    uint64_t ping_timeout_ns;
    uint64_t suspect_timeout_ns;
    uint32_t self_id;
    uint32_t self_incarnation;
    void (*on_event)(dsco_swim_ev ev, uint32_t node_id, void *ud);
    void *ud;
} dsco_swim;

bool dsco_swim_init(dsco_swim *s, uint32_t self_id, uint64_t ping_timeout_ns,
                    uint64_t suspect_timeout_ns, void (*on_event)(dsco_swim_ev, uint32_t, void *),
                    void *ud);
void dsco_swim_free(dsco_swim *s);
bool dsco_swim_add(dsco_swim *s, uint32_t node_id, uint64_t now_ns); /* → JOINED */
void dsco_swim_ack(dsco_swim *s, uint32_t node_id, uint64_t now_ns); /* refresh liveness */
void dsco_swim_tick(dsco_swim *s, uint64_t now_ns);                  /* drive timeouts */
/* Incoming suspicion about ourselves: bump our incarnation and refute. */
void dsco_swim_on_suspect_self(dsco_swim *s, uint32_t incarnation);
dsco_mstate dsco_swim_state(const dsco_swim *s, uint32_t node_id);
uint32_t dsco_swim_incarnation(const dsco_swim *s);

/* ── dsco_seqrec (#28 NEW): serve endpoint unit of work ─────────────────── */
typedef enum {
    DSCO_SEQ_QUEUED = 0,
    DSCO_SEQ_PREFILL = 1,
    DSCO_SEQ_DECODE = 2,
    DSCO_SEQ_DONE = 3,
    DSCO_SEQ_FAILED = 4
} dsco_seqstate;

typedef struct {
    uint64_t req_id;
    dsco_seqstate state;
    dsco_vec prompt; /* uint32_t token ids */
    dsco_vec out;    /* uint32_t token ids */
    uint32_t pos;
    struct {
        float temp;
        float top_p;
        uint32_t top_k;
        uint64_t seed;
    } samp;
    dsco_vec kv_blocks; /* uint32_t block ids (milestone 2+) */
    int64_t deadline_ns;
    int sse_fd;
    dsco_arena *arena;
} dsco_seqrec;

bool dsco_seqrec_init(dsco_seqrec *r, uint64_t req_id, dsco_arena *arena);
void dsco_seqrec_free(dsco_seqrec *r);
bool dsco_seqrec_push_prompt(dsco_seqrec *r, uint32_t tok);
bool dsco_seqrec_emit(dsco_seqrec *r, uint32_t tok); /* append to out, pos++ */
void dsco_seqrec_set_state(dsco_seqrec *r, dsco_seqstate st);

#endif /* DSCO_RUNTIME_H */
