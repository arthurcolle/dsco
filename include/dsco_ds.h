#ifndef DSCO_DS_H
#define DSCO_DS_H

/* ── dsco generic data-structure toolkit ────────────────────────────────────
 * The reusable containers that were skipped during initial setup, so the many
 * hand-rolled, duplicated structures in the tree (three separate LRUs, a
 * bespoke chained tool-lookup hash, manual cap/count array-doubling, linear
 * scans over fixed `entries[N]+in_use` pools) have proper abstractions to sit
 * on. Every container returns false/NULL on allocation failure rather than
 * aborting, so it is safe to unit-test and reuse anywhere. None are internally
 * locked — callers that share an instance across threads guard it themselves. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── dsco_vec: growable typed array ─────────────────────────────────────────
 * Replaces the manual `data/len/cap` + doubling scattered across the tree
 * (conversation blocks, fallback models, peer snapshots, …). */
typedef struct {
    unsigned char *data;
    size_t len;  /* element count */
    size_t cap;  /* element capacity */
    size_t elem; /* bytes per element */
} dsco_vec;

void dsco_vec_init(dsco_vec *v, size_t elem_size);
void dsco_vec_free(dsco_vec *v);
/* Append a copy of *elem; returns a pointer to the stored slot, or NULL on OOM.
 * The returned pointer is invalidated by the next push/reserve. */
void *dsco_vec_push(dsco_vec *v, const void *elem);
void *dsco_vec_at(const dsco_vec *v, size_t i); /* NULL if out of range */
size_t dsco_vec_len(const dsco_vec *v);
void dsco_vec_pop(dsco_vec *v);   /* removes the last element */
void dsco_vec_clear(dsco_vec *v); /* len=0, keeps capacity */
bool dsco_vec_reserve(dsco_vec *v, size_t min_cap);

/* ── dsco_map: string-keyed hash map → void* value ──────────────────────────
 * Chained buckets, FNV-1a, auto-resize. Owns copies of keys; values are opaque
 * pointers the caller owns (pass a free_val to have them freed on delete/free).
 * Replaces the hand-rolled tool_map and the various strcmp-scan registries. */
typedef struct dsco_map dsco_map;

dsco_map *dsco_map_new(void);
void dsco_map_free(dsco_map *m, void (*free_val)(void *));
/* Insert or replace. Returns false on OOM. If replacing and free_val != NULL,
 * the previous value is freed. */
bool dsco_map_put(dsco_map *m, const char *key, void *val, void (*free_val)(void *));
void *dsco_map_get(const dsco_map *m, const char *key); /* NULL if absent */
bool dsco_map_has(const dsco_map *m, const char *key);
bool dsco_map_del(dsco_map *m, const char *key, void (*free_val)(void *));
size_t dsco_map_size(const dsco_map *m);
/* Visit every (key,value). Return non-zero from fn to stop early. */
void dsco_map_foreach(const dsco_map *m, int (*fn)(const char *key, void *val, void *ctx),
                      void *ctx);

/* ── dsco_lru: bounded string→bytes cache with O(1) eviction ────────────────
 * Hash index + intrusive recency list. Unifies the three ad-hoc LRUs
 * (tool_cache, spec_cache, pool-slot recency). Values are copied in/out. */
typedef struct dsco_lru dsco_lru;

dsco_lru *dsco_lru_new(size_t capacity);
void dsco_lru_free(dsco_lru *c);
/* Copy value bytes in (replacing any prior value for key). Evicts LRU if full.
 * Returns false on OOM. */
bool dsco_lru_put(dsco_lru *c, const char *key, const void *val, size_t len);
/* On hit, returns a pointer to the stored value (valid until the next mutating
 * call), sets *len, marks the key most-recently-used, returns true. */
const void *dsco_lru_get(dsco_lru *c, const char *key, size_t *len);
bool dsco_lru_del(dsco_lru *c, const char *key);
size_t dsco_lru_size(const dsco_lru *c);

/* ── dsco_heap: binary min-heap / priority queue ────────────────────────────
 * cmp(a,b) < 0 means a has higher priority (pops first). Missing entirely from
 * the tree — useful for timer deadlines, latency-ranked selection, top-K. */
typedef struct dsco_heap dsco_heap;

dsco_heap *dsco_heap_new(size_t elem_size, int (*cmp)(const void *a, const void *b));
void dsco_heap_free(dsco_heap *h);
bool dsco_heap_push(dsco_heap *h, const void *elem);
bool dsco_heap_pop(dsco_heap *h, void *out); /* min → out; false if empty */
bool dsco_heap_peek(const dsco_heap *h, void *out);
size_t dsco_heap_size(const dsco_heap *h);

/* ── dsco_bitset: dynamic bit vector ────────────────────────────────────────
 * Cleaner than the per-feature bool arrays (e.g. the 40 TUI feature flags). */
typedef struct {
    uint64_t *words;
    size_t nbits;
} dsco_bitset;

dsco_bitset *dsco_bitset_new(size_t nbits);
void dsco_bitset_free(dsco_bitset *b);
void dsco_bitset_set(dsco_bitset *b, size_t i);
void dsco_bitset_clr(dsco_bitset *b, size_t i);
bool dsco_bitset_test(const dsco_bitset *b, size_t i);
void dsco_bitset_set_all(dsco_bitset *b);
void dsco_bitset_clr_all(dsco_bitset *b);
size_t dsco_bitset_count(const dsco_bitset *b); /* population count */

/* ── dsco_queue: bounded FIFO of fixed-size elements (single-thread) ─────────
 * Fixed-capacity typed queue. Generalizes the streaming/output rings. (Named
 * dsco_queue rather than dsco_ring to avoid colliding with project.c's existing
 * byte-scrollback ring — itself a candidate to migrate onto this.) */
typedef struct {
    unsigned char *data;
    size_t elem;
    size_t cap;  /* max elements */
    size_t head; /* next pop */
    size_t count;
} dsco_queue;

bool dsco_queue_init(dsco_queue *q, size_t elem_size, size_t capacity);
void dsco_queue_free(dsco_queue *q);
bool dsco_queue_push(dsco_queue *q, const void *elem); /* false if full */
bool dsco_queue_pop(dsco_queue *q, void *out);         /* false if empty */
size_t dsco_queue_len(const dsco_queue *q);
bool dsco_queue_full(const dsco_queue *q);
bool dsco_queue_empty(const dsco_queue *q);

/* ── dsco_strpool: string interner → stable ids ─────────────────────────────
 * Maps repeated strings (tool names, provider names) to a small stable integer
 * id, so hot-path comparisons become int-equality instead of strcmp. */
typedef struct dsco_strpool dsco_strpool;

dsco_strpool *dsco_strpool_new(void);
void dsco_strpool_free(dsco_strpool *p);
/* Intern s; returns its id (>=0), assigning a new one if unseen. -1 on OOM. */
int dsco_strpool_intern(dsco_strpool *p, const char *s);
/* Look up without inserting: id if present, else -1. */
int dsco_strpool_find(const dsco_strpool *p, const char *s);
/* The interned string for an id (stable pointer), or NULL if out of range. */
const char *dsco_strpool_str(const dsco_strpool *p, int id);
size_t dsco_strpool_count(const dsco_strpool *p);

/* ════════════════════════════════════════════════════════════════════════════
 * Tier 2 — the next layer, grounded in real adoption sites in the tree.
 * (Arena allocation is NOT here: src/arena_alloc.c already provides scratch +
 * session arenas with temp scopes. Thread pooling is dsco_pool.c. These seven
 * fill the remaining gaps.)
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── dsco_deque: growable double-ended queue ────────────────────────────────
 * Push/pop at both ends in O(1) amortized. Generalizes dsco_queue (which is
 * fixed-capacity, back-in/front-out). For sliding windows (token/latency
 * history), work-stealing, and undo/redo rings. */
typedef struct {
    unsigned char *data;
    size_t elem;
    size_t cap;
    size_t head; /* logical index 0 lives here */
    size_t count;
} dsco_deque;

bool dsco_deque_init(dsco_deque *d, size_t elem_size);
void dsco_deque_free(dsco_deque *d);
bool dsco_deque_push_back(dsco_deque *d, const void *elem);
bool dsco_deque_push_front(dsco_deque *d, const void *elem);
bool dsco_deque_pop_front(dsco_deque *d, void *out); /* false if empty */
bool dsco_deque_pop_back(dsco_deque *d, void *out);  /* false if empty */
void *dsco_deque_at(const dsco_deque *d, size_t i);  /* 0 = front; NULL if oob */
size_t dsco_deque_len(const dsco_deque *d);

/* ── dsco_topk: bounded top-K by score ──────────────────────────────────────
 * Retains the K highest-scoring items seen, in O(log K) per offer, without
 * sorting or storing the whole stream. Replaces the hand-rolled O(n²)
 * selection/bubble passes that pick "top N tools by calls" (src/agent.c) and
 * fits latency-ranked provider/peer selection. Internally a size-K min-heap. */
typedef struct dsco_topk dsco_topk;

dsco_topk *dsco_topk_new(size_t k);
void dsco_topk_free(dsco_topk *t);
/* Consider (score,item). Keeps it only if among the K best so far. */
void dsco_topk_offer(dsco_topk *t, double score, void *item);
size_t dsco_topk_count(const dsco_topk *t);
/* Copy the retained items into out[]/scores[] (either may be NULL) in
 * DESCENDING score order, up to cap. Non-destructive. Returns #written. */
size_t dsco_topk_get(const dsco_topk *t, void **out, double *scores, size_t cap);

/* ── dsco_bloom: Bloom filter (probabilistic membership) ─────────────────────
 * Constant-memory "have I seen this?" with no false negatives and a tunable
 * false-positive rate. For cheap pre-filtering of seen tool-result / file
 * fingerprints before the exact (and costlier) cache check. */
typedef struct dsco_bloom dsco_bloom;

dsco_bloom *dsco_bloom_new(size_t expected_items, double fp_rate);
void dsco_bloom_free(dsco_bloom *b);
void dsco_bloom_add(dsco_bloom *b, const void *data, size_t len);
bool dsco_bloom_maybe(const dsco_bloom *b, const void *data,
                      size_t len); /* false ⇒ definitely absent */
void dsco_bloom_add_str(dsco_bloom *b, const char *s);
bool dsco_bloom_maybe_str(const dsco_bloom *b, const char *s);
size_t dsco_bloom_bits(const dsco_bloom *b); /* filter size in bits */

/* ── dsco_intmap: open-addressed uint64 → void* map ─────────────────────────
 * No per-entry allocation, cache-friendly linear probing. Complements the
 * string-keyed dsco_map for id-keyed lookups (strpool ids, node/session ids,
 * fingerprints). NULL is a legal value — use dsco_intmap_has to disambiguate. */
typedef struct dsco_intmap dsco_intmap;

dsco_intmap *dsco_intmap_new(void);
void dsco_intmap_free(dsco_intmap *m);
bool dsco_intmap_put(dsco_intmap *m, uint64_t key, void *val); /* false on OOM */
void *dsco_intmap_get(const dsco_intmap *m, uint64_t key);     /* NULL if absent */
bool dsco_intmap_has(const dsco_intmap *m, uint64_t key);
bool dsco_intmap_del(dsco_intmap *m, uint64_t key);
size_t dsco_intmap_size(const dsco_intmap *m);

/* ── dsco_ringbuf: byte FIFO stream buffer ──────────────────────────────────
 * Contiguous-logical byte queue with backpressure (writes past free space are
 * truncated, not overwritten). The general form of project.c's scrollback ring
 * — a clean migration target for streaming stdout/SSE buffering. */
typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t head;  /* next byte to read */
    size_t count; /* bytes currently buffered */
} dsco_ringbuf;

bool dsco_ringbuf_init(dsco_ringbuf *r, size_t cap);
void dsco_ringbuf_free(dsco_ringbuf *r);
size_t dsco_ringbuf_write(dsco_ringbuf *r, const void *data,
                          size_t len);                            /* bytes accepted (≤ free) */
size_t dsco_ringbuf_read(dsco_ringbuf *r, void *out, size_t max); /* consumes; returns #read */
size_t dsco_ringbuf_peek(const dsco_ringbuf *r, void *out, size_t max); /* non-consuming */
size_t dsco_ringbuf_len(const dsco_ringbuf *r);                         /* buffered bytes */
size_t dsco_ringbuf_avail(const dsco_ringbuf *r);                       /* free bytes */

/* ── dsco_uf: union-find / disjoint set ─────────────────────────────────────
 * Near-constant connectivity queries over N elements (path compression + union
 * by rank). For connected components of the mesh/cluster graph, dedup grouping,
 * and equivalence-class collapse. */
typedef struct dsco_uf dsco_uf;

dsco_uf *dsco_uf_new(size_t n);
void dsco_uf_free(dsco_uf *u);
size_t dsco_uf_find(dsco_uf *u, size_t x);          /* set representative */
bool dsco_uf_union(dsco_uf *u, size_t a, size_t b); /* true if two sets merged */
bool dsco_uf_connected(dsco_uf *u, size_t a, size_t b);
size_t dsco_uf_count(const dsco_uf *u); /* # of disjoint sets */

/* ── dsco_slab: fixed-capacity object pool with stable indices ───────────────
 * O(1) alloc/release from a freelist; indices stay valid until released. The
 * principled replacement for the pervasive `entries[N] + in_use/count` pools
 * (codex_app_directory, tool_metrics, swarm children, …). */
typedef struct dsco_slab dsco_slab;

dsco_slab *dsco_slab_new(size_t elem_size, size_t capacity);
void dsco_slab_free(dsco_slab *s);
/* Claim a slot: returns its stable index and (via *out, if non-NULL) its
 * zeroed storage pointer. Returns -1 when full. */
long dsco_slab_alloc(dsco_slab *s, void **out);
void dsco_slab_release(dsco_slab *s, size_t idx);   /* no-op if idx not live */
void *dsco_slab_at(const dsco_slab *s, size_t idx); /* NULL if free/oob */
bool dsco_slab_live(const dsco_slab *s, size_t idx);
size_t dsco_slab_used(const dsco_slab *s);
size_t dsco_slab_cap(const dsco_slab *s);

#endif /* DSCO_DS_H */
