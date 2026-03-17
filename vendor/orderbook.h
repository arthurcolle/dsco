/*
 * orderbook.h — Hierarchical Packet/Resource Orderbook
 *
 * A multi-level matching engine for distributed resource markets.
 * Designed for planet-scale P2P networks (target: 70B nodes).
 *
 * Architecture:
 *   L0 - Device-local book (intra-personal, 7 devices)
 *   L1 - Neighborhood book (~1K nodes, 100ms batch clear)
 *   L2 - Regional book (~1M nodes, 1s batch clear)
 *   L3 - Continental book (~1B nodes, 5s batch clear)
 *   L4 - Global book (top-of-book only, rare clears)
 *
 * Order types:
 *   MARKET   - Execute immediately at best available price
 *   LIMIT    - Execute at specified price or better
 *   FOK      - Fill-or-kill, latency-sensitive
 *   GTC      - Good-til-cancelled, background tasks
 *   IOC      - Immediate-or-cancel, partial fills OK
 *   BATCH    - Accumulate for next batch clear
 *
 * Resource types:
 *   COMPUTE  - CPU cycles (normalized to benchmark seconds)
 *   GPU      - GPU compute (normalized)
 *   MEMORY   - RAM allocation
 *   STORAGE  - Persistent storage
 *   BANDWIDTH - Network throughput
 *   INFERENCE - LLM/ML inference tokens
 *   RELAY    - Packet relay/routing service
 *   CUSTOM   - Opaque provider-defined capability
 *
 * Matching:
 *   L0-L1: Continuous double auction (price-time priority)
 *   L2:    Frequent batch auction (uniform clearing price)
 *   L3-L4: CFMM bonding curve + batch clear for large orders
 *
 * License: MIT
 */

#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────── */

#define OB_MAX_LEVELS         5
#define OB_MAX_ORDERS         65536
#define OB_MAX_PRICE_LEVELS   4096
#define OB_MAX_RESOURCE_TYPES 64
#define OB_MAX_FILLS_PER_MATCH 256
#define OB_MAX_BOOK_DEPTH     128
#define OB_NODE_ID_BYTES      32
#define OB_ORDER_TAG_LEN      32
#define OB_PROVIDER_CAP_LEN   64

/* ── Fixed-Point Price ─────────────────────────────────────────────── */
/*
 * Prices are 64-bit fixed point: upper 32 bits integer, lower 32 fractional.
 * 1 unit = 1 compute-second on reference benchmark.
 * This gives sub-nano precision with range up to ~4 billion compute-seconds.
 */
typedef uint64_t ob_price_t;

#define OB_PRICE_ONE       ((ob_price_t)1ULL << 32)
#define OB_PRICE_ZERO      ((ob_price_t)0)
#define OB_PRICE_MAX       ((ob_price_t)UINT64_MAX)
#define OB_PRICE_MAKE(i,f) (((ob_price_t)(i) << 32) | (uint32_t)(f))
#define OB_PRICE_INT(p)    ((uint32_t)((p) >> 32))
#define OB_PRICE_FRAC(p)   ((uint32_t)((p) & 0xFFFFFFFF))

/* Multiply price * quantity (both fixed-point), return fixed-point */
static inline ob_price_t ob_price_mul(ob_price_t price, uint64_t qty) {
    /* Simple: treat qty as whole units for now */
    return price * qty;
}

/* ── Enums ─────────────────────────────────────────────────────────── */

typedef enum {
    OB_SIDE_BID = 0,   /* Buyer: wants to consume resource */
    OB_SIDE_ASK = 1,   /* Seller: wants to provide resource */
} ob_side_t;

typedef enum {
    OB_TYPE_MARKET = 0,
    OB_TYPE_LIMIT  = 1,
    OB_TYPE_FOK    = 2,   /* Fill-or-kill */
    OB_TYPE_GTC    = 3,   /* Good-til-cancelled */
    OB_TYPE_IOC    = 4,   /* Immediate-or-cancel */
    OB_TYPE_BATCH  = 5,   /* Accumulate for batch clear */
} ob_order_type_t;

typedef enum {
    OB_RESOURCE_COMPUTE    = 0,
    OB_RESOURCE_GPU        = 1,
    OB_RESOURCE_MEMORY     = 2,
    OB_RESOURCE_STORAGE    = 3,
    OB_RESOURCE_BANDWIDTH  = 4,
    OB_RESOURCE_INFERENCE  = 5,
    OB_RESOURCE_RELAY      = 6,
    OB_RESOURCE_CUSTOM     = 7,
    OB_RESOURCE_COUNT      = 8,
} ob_resource_t;

typedef enum {
    OB_LEVEL_L0 = 0,   /* Device-local */
    OB_LEVEL_L1 = 1,   /* Neighborhood (~1K) */
    OB_LEVEL_L2 = 2,   /* Regional (~1M) */
    OB_LEVEL_L3 = 3,   /* Continental (~1B) */
    OB_LEVEL_L4 = 4,   /* Global */
} ob_level_t;

typedef enum {
    OB_MATCH_PRICE_TIME  = 0,   /* CLOB: price-time priority */
    OB_MATCH_PRO_RATA    = 1,   /* Pro-rata at each price level */
    OB_MATCH_BATCH_UNIFORM = 2, /* Batch: uniform clearing price */
    OB_MATCH_CFMM        = 3,   /* Bonding curve AMM */
} ob_match_mode_t;

typedef enum {
    OB_ORDER_OPEN      = 0,
    OB_ORDER_PARTIAL   = 1,
    OB_ORDER_FILLED    = 2,
    OB_ORDER_CANCELLED = 3,
    OB_ORDER_EXPIRED   = 4,
    OB_ORDER_REJECTED  = 5,
} ob_order_status_t;

/* ── Node Identity ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t           id[OB_NODE_ID_BYTES];   /* 256-bit node ID (DHT key) */
    uint64_t          reputation;              /* earned by completing tasks */
    uint64_t          stake;                   /* committed collateral */
    uint32_t          latency_us;              /* measured RTT to us */
    ob_level_t        level;                   /* which level this node lives at */
} ob_node_ref_t;

/* ── Order ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t          id;
    ob_side_t         side;
    ob_order_type_t   type;
    ob_resource_t     resource;
    ob_price_t        price;              /* limit price (0 for market) */
    uint64_t          quantity;           /* units of resource */
    uint64_t          filled;             /* units filled so far */
    ob_order_status_t status;
    ob_node_ref_t     node;               /* who placed this order */
    uint64_t          timestamp;          /* microseconds since epoch */
    uint64_t          expiry;             /* 0 = no expiry (GTC) */
    char              tag[OB_ORDER_TAG_LEN]; /* user-defined tag */

    /* Provider capability descriptor (asks only) */
    char              capability[OB_PROVIDER_CAP_LEN];

    /* Batch tracking */
    uint64_t          batch_id;           /* which batch epoch this belongs to */

    /* Internal: linked list for price level */
    uint64_t          _next;              /* next order id at same price, 0=end */
    uint64_t          _prev;
} ob_order_t;

/* ── Price Level ───────────────────────────────────────────────────── */

typedef struct {
    ob_price_t        price;
    uint64_t          total_quantity;     /* sum of all orders at this level */
    int               order_count;
    uint64_t          head_order_id;      /* first order (time priority) */
    uint64_t          tail_order_id;      /* last order */
} ob_price_level_t;

/* ── Fill / Trade ──────────────────────────────────────────────────── */

typedef struct {
    uint64_t          trade_id;
    uint64_t          bid_order_id;
    uint64_t          ask_order_id;
    ob_node_ref_t     buyer;
    ob_node_ref_t     seller;
    ob_resource_t     resource;
    ob_price_t        price;              /* execution price */
    uint64_t          quantity;
    uint64_t          timestamp;
} ob_fill_t;

/* ── Top-of-Book Summary (propagated upward) ──────────────────────── */

typedef struct {
    ob_resource_t     resource;
    ob_price_t        best_bid;
    ob_price_t        best_ask;
    uint64_t          bid_depth;          /* total bid quantity */
    uint64_t          ask_depth;          /* total ask quantity */
    ob_price_t        last_price;         /* last trade price */
    uint64_t          last_quantity;
    uint64_t          volume_24h;         /* rolling 24h volume */
    ob_price_t        vwap_24h;           /* volume-weighted avg price */
    uint64_t          trade_count_24h;
    uint64_t          timestamp;
} ob_top_of_book_t;

/* ── CFMM Pool (for L3-L4 liquidity) ──────────────────────────────── */
/*
 * Constant-product AMM: x * y = k
 * x = reserve of credits, y = reserve of resource units
 * Provides guaranteed liquidity even in thin markets.
 */
typedef struct {
    ob_resource_t     resource;
    uint64_t          reserve_credit;     /* credit reserve */
    uint64_t          reserve_resource;   /* resource reserve */
    uint64_t          k;                  /* invariant (reserve_credit * reserve_resource) */
    uint64_t          fee_bps;            /* fee in basis points (e.g., 30 = 0.3%) */
    uint64_t          total_fees_collected;
    uint64_t          lp_shares_total;    /* total LP shares outstanding */
} ob_cfmm_pool_t;

/* ── Batch Auction State ───────────────────────────────────────────── */

typedef struct {
    uint64_t          epoch;              /* batch epoch number */
    uint64_t          open_time;          /* when this batch opened */
    uint64_t          close_time;         /* when it clears */
    uint32_t          interval_ms;        /* batch interval */
    int               pending_bids;
    int               pending_asks;
    uint64_t          total_bid_qty;
    uint64_t          total_ask_qty;
    ob_price_t        indicative_price;   /* real-time indicative clearing price */
    ob_price_t        clearing_price;     /* final clearing price (set at clear) */
    uint64_t          cleared_quantity;   /* quantity matched at clearing */
} ob_batch_state_t;

/* ── Book Statistics ───────────────────────────────────────────────── */

typedef struct {
    uint64_t          orders_submitted;
    uint64_t          orders_filled;
    uint64_t          orders_partial;
    uint64_t          orders_cancelled;
    uint64_t          orders_expired;
    uint64_t          orders_rejected;
    uint64_t          trades_executed;
    uint64_t          total_volume;
    ob_price_t        total_value;
    uint64_t          batches_cleared;
    double            avg_fill_time_us;
    double            avg_spread_bps;     /* average spread in basis points */
} ob_stats_t;

/* ── Callbacks ─────────────────────────────────────────────────────── */

typedef void (*ob_fill_fn)(const ob_fill_t *fill, void *ctx);
typedef void (*ob_order_fn)(const ob_order_t *order, ob_order_status_t old_status, void *ctx);
typedef void (*ob_batch_fn)(const ob_batch_state_t *batch, const ob_fill_t *fills, int nfills, void *ctx);
typedef void (*ob_tob_fn)(const ob_top_of_book_t *tob, void *ctx);

/* ── Book Configuration ────────────────────────────────────────────── */

typedef struct {
    ob_level_t        level;
    ob_match_mode_t   match_mode;         /* override per-level default */
    uint32_t          batch_interval_ms;  /* for batch/CFMM modes */
    uint64_t          min_order_size;     /* minimum quantity */
    uint64_t          max_order_size;     /* maximum quantity per order */
    uint64_t          min_stake;          /* minimum stake to post orders */
    uint64_t          min_reputation;     /* minimum reputation to post */
    bool              enable_cfmm;        /* enable bonding curve fallback */
    uint64_t          cfmm_initial_credit;
    uint64_t          cfmm_initial_resource;
    uint64_t          cfmm_fee_bps;
    /* Callbacks */
    ob_fill_fn        on_fill;
    ob_order_fn       on_order_update;
    ob_batch_fn       on_batch_clear;
    ob_tob_fn         on_tob_update;      /* propagate to parent level */
    void             *callback_ctx;
} ob_config_t;

/* ── Book Handle (opaque) ──────────────────────────────────────────── */

typedef struct ob_book ob_book_t;

/* ── Core API ──────────────────────────────────────────────────────── */

/* Lifecycle */
ob_book_t      *ob_book_create(ob_resource_t resource, const ob_config_t *config);
void            ob_book_destroy(ob_book_t *book);

/* Order management */
uint64_t        ob_submit(ob_book_t *book, const ob_order_t *order);
int             ob_cancel(ob_book_t *book, uint64_t order_id);
int             ob_modify(ob_book_t *book, uint64_t order_id,
                          ob_price_t new_price, uint64_t new_qty);
const ob_order_t *ob_get_order(ob_book_t *book, uint64_t order_id);

/* Matching */
int             ob_match(ob_book_t *book);           /* run continuous matching */
int             ob_batch_clear(ob_book_t *book);     /* force batch clear now */
int             ob_tick(ob_book_t *book, uint64_t now_us); /* time-driven: check batch intervals */

/* CFMM */
int             ob_cfmm_swap(ob_book_t *book, ob_side_t side, uint64_t amount,
                              ob_price_t min_price, uint64_t *filled, ob_price_t *exec_price);
int             ob_cfmm_add_liquidity(ob_book_t *book, uint64_t credit, uint64_t resource,
                                       uint64_t *shares_out);
int             ob_cfmm_remove_liquidity(ob_book_t *book, uint64_t shares,
                                          uint64_t *credit_out, uint64_t *resource_out);
ob_cfmm_pool_t ob_cfmm_get_pool(ob_book_t *book);

/* Market data */
ob_top_of_book_t ob_get_tob(ob_book_t *book);
int             ob_get_depth(ob_book_t *book, ob_side_t side,
                              ob_price_level_t *levels, int max_levels);
ob_price_t      ob_get_midpoint(ob_book_t *book);
ob_price_t      ob_get_spread(ob_book_t *book);
ob_price_t      ob_get_vwap(ob_book_t *book, uint64_t quantity); /* VWAP for given size */

/* Batch state */
ob_batch_state_t ob_get_batch_state(ob_book_t *book);

/* Statistics */
ob_stats_t      ob_get_stats(ob_book_t *book);
void            ob_reset_stats(ob_book_t *book);

/* ── Hierarchical Book (manages books across levels) ──────────────── */

typedef struct ob_exchange ob_exchange_t;

typedef struct {
    ob_config_t       level_config[OB_MAX_LEVELS];
    int               active_levels;      /* how many levels are active */
    uint64_t          tob_propagation_interval_us; /* how often to push TOB up */
} ob_exchange_config_t;

ob_exchange_t  *ob_exchange_create(const ob_exchange_config_t *config);
void            ob_exchange_destroy(ob_exchange_t *ex);

/* Get book for a specific resource at a specific level */
ob_book_t      *ob_exchange_get_book(ob_exchange_t *ex, ob_resource_t resource, ob_level_t level);

/* Submit order to appropriate level (auto-routes based on size/type) */
uint64_t        ob_exchange_submit(ob_exchange_t *ex, const ob_order_t *order);

/* Tick all books (call from event loop) */
int             ob_exchange_tick(ob_exchange_t *ex, uint64_t now_us);

/* Get consolidated top-of-book across levels for a resource */
ob_top_of_book_t ob_exchange_get_tob(ob_exchange_t *ex, ob_resource_t resource);

/* Get aggregate stats */
ob_stats_t      ob_exchange_get_stats(ob_exchange_t *ex);

/* ── Serialization (for network propagation) ──────────────────────── */

/* Serialize top-of-book to wire format (compact binary) */
int             ob_tob_serialize(const ob_top_of_book_t *tob, uint8_t *buf, size_t buf_sz);
int             ob_tob_deserialize(const uint8_t *buf, size_t len, ob_top_of_book_t *tob);

/* Serialize order to wire format */
int             ob_order_serialize(const ob_order_t *order, uint8_t *buf, size_t buf_sz);
int             ob_order_deserialize(const uint8_t *buf, size_t len, ob_order_t *order);

/* Serialize batch of fills */
int             ob_fills_serialize(const ob_fill_t *fills, int n, uint8_t *buf, size_t buf_sz);
int             ob_fills_deserialize(const uint8_t *buf, size_t len, ob_fill_t *fills, int *n);

#ifdef __cplusplus
}
#endif

#endif /* ORDERBOOK_H */
