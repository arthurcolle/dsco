/*
 * orderbook.c — Hierarchical Packet/Resource Orderbook Implementation
 *
 * Matching engine with:
 *   - Price-time priority CLOB (L0-L1)
 *   - Frequent batch auction with uniform clearing (L2)
 *   - Constant-product AMM / bonding curve (L3-L4)
 *   - Hierarchical top-of-book propagation
 *   - Fixed-point arithmetic (no floats in the hot path)
 *
 * License: MIT
 */

#include "orderbook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifndef OB_LOG
#define OB_LOG(fmt, ...) /* silent by default */
#endif

/* ── Internal Book Structure ───────────────────────────────────────── */

struct ob_book {
    ob_resource_t     resource;
    ob_config_t       config;
    pthread_mutex_t   lock;

    /* Order storage (flat array, indexed by slot) */
    ob_order_t        orders[OB_MAX_ORDERS];
    bool              order_used[OB_MAX_ORDERS];
    int               order_count;
    uint64_t          next_order_id;

    /* Price levels — sorted arrays for bid (descending) and ask (ascending) */
    ob_price_level_t  bids[OB_MAX_PRICE_LEVELS];
    int               bid_count;
    ob_price_level_t  asks[OB_MAX_PRICE_LEVELS];
    int               ask_count;

    /* Batch auction state */
    ob_batch_state_t  batch;

    /* CFMM pool */
    ob_cfmm_pool_t    pool;

    /* Top of book cache */
    ob_top_of_book_t  tob;

    /* Rolling stats */
    ob_stats_t        stats;

    /* Fill output buffer */
    ob_fill_t         fill_buf[OB_MAX_FILLS_PER_MATCH];
    int               fill_count;
    uint64_t          next_trade_id;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static int find_order_slot(ob_book_t *book) {
    for (int i = 0; i < OB_MAX_ORDERS; i++) {
        if (!book->order_used[i]) return i;
    }
    return -1;
}

static ob_order_t *get_order(ob_book_t *book, uint64_t id) {
    for (int i = 0; i < OB_MAX_ORDERS; i++) {
        if (book->order_used[i] && book->orders[i].id == id)
            return &book->orders[i];
    }
    return NULL;
}

/* ── Price Level Management ────────────────────────────────────────── */

/* Find or insert a price level in sorted array.
 * Bids: descending (highest first). Asks: ascending (lowest first). */
static ob_price_level_t *find_or_insert_level(
    ob_price_level_t *levels, int *count, ob_price_t price, ob_side_t side)
{
    int n = *count;

    /* Find existing */
    for (int i = 0; i < n; i++) {
        if (levels[i].price == price) return &levels[i];
    }

    if (n >= OB_MAX_PRICE_LEVELS) return NULL;

    /* Find insertion point */
    int pos = n;
    for (int i = 0; i < n; i++) {
        if (side == OB_SIDE_BID) {
            if (price > levels[i].price) { pos = i; break; }
        } else {
            if (price < levels[i].price) { pos = i; break; }
        }
    }

    /* Shift */
    if (pos < n) {
        memmove(&levels[pos + 1], &levels[pos],
                (n - pos) * sizeof(ob_price_level_t));
    }

    /* Init new level */
    memset(&levels[pos], 0, sizeof(ob_price_level_t));
    levels[pos].price = price;
    (*count)++;

    return &levels[pos];
}

static void remove_level_if_empty(ob_price_level_t *levels, int *count, int idx) {
    if (idx < 0 || idx >= *count) return;
    if (levels[idx].total_quantity > 0 || levels[idx].order_count > 0) return;

    int n = *count;
    if (idx < n - 1) {
        memmove(&levels[idx], &levels[idx + 1],
                (n - 1 - idx) * sizeof(ob_price_level_t));
    }
    (*count)--;
}

/* ── Add Order to Book ─────────────────────────────────────────────── */

static void add_to_book(ob_book_t *book, ob_order_t *order) {
    ob_price_level_t *levels;
    int *count;

    if (order->side == OB_SIDE_BID) {
        levels = book->bids;
        count = &book->bid_count;
    } else {
        levels = book->asks;
        count = &book->ask_count;
    }

    ob_price_level_t *lvl = find_or_insert_level(levels, count, order->price, order->side);
    if (!lvl) {
        order->status = OB_ORDER_REJECTED;
        book->stats.orders_rejected++;
        return;
    }

    lvl->total_quantity += (order->quantity - order->filled);
    lvl->order_count++;

    /* Append to tail (time priority) */
    order->_prev = lvl->tail_order_id;
    order->_next = 0;
    if (lvl->tail_order_id) {
        ob_order_t *tail = get_order(book, lvl->tail_order_id);
        if (tail) tail->_next = order->id;
    }
    lvl->tail_order_id = order->id;
    if (!lvl->head_order_id) lvl->head_order_id = order->id;
}

/* ── Remove Order from Book ────────────────────────────────────────── */

static void remove_from_book(ob_book_t *book, ob_order_t *order) {
    ob_price_level_t *levels;
    int *count;

    if (order->side == OB_SIDE_BID) {
        levels = book->bids;
        count = &book->bid_count;
    } else {
        levels = book->asks;
        count = &book->ask_count;
    }

    /* Find level */
    int lvl_idx = -1;
    for (int i = 0; i < *count; i++) {
        if (levels[i].price == order->price) { lvl_idx = i; break; }
    }
    if (lvl_idx < 0) return;

    ob_price_level_t *lvl = &levels[lvl_idx];
    uint64_t remaining = order->quantity - order->filled;
    if (remaining > lvl->total_quantity)
        lvl->total_quantity = 0;
    else
        lvl->total_quantity -= remaining;
    lvl->order_count--;

    /* Unlink from list */
    if (order->_prev) {
        ob_order_t *prev = get_order(book, order->_prev);
        if (prev) prev->_next = order->_next;
    } else {
        lvl->head_order_id = order->_next;
    }
    if (order->_next) {
        ob_order_t *next = get_order(book, order->_next);
        if (next) next->_prev = order->_prev;
    } else {
        lvl->tail_order_id = order->_prev;
    }

    order->_next = order->_prev = 0;
    remove_level_if_empty(levels, count, lvl_idx);
}

/* ── Execute Fill ──────────────────────────────────────────────────── */

static void execute_fill(ob_book_t *book, ob_order_t *bid, ob_order_t *ask,
                          ob_price_t price, uint64_t qty) {
    if (book->fill_count >= OB_MAX_FILLS_PER_MATCH) return;

    ob_fill_t *fill = &book->fill_buf[book->fill_count++];
    fill->trade_id = ++book->next_trade_id;
    fill->bid_order_id = bid->id;
    fill->ask_order_id = ask->id;
    fill->buyer = bid->node;
    fill->seller = ask->node;
    fill->resource = book->resource;
    fill->price = price;
    fill->quantity = qty;
    fill->timestamp = now_us();

    bid->filled += qty;
    ask->filled += qty;

    if (bid->filled >= bid->quantity) bid->status = OB_ORDER_FILLED;
    else bid->status = OB_ORDER_PARTIAL;

    if (ask->filled >= ask->quantity) ask->status = OB_ORDER_FILLED;
    else ask->status = OB_ORDER_PARTIAL;

    /* Stats */
    book->stats.trades_executed++;
    book->stats.total_volume += qty;

    /* Update TOB cache */
    book->tob.last_price = price;
    book->tob.last_quantity = qty;
    book->tob.volume_24h += qty;
    book->tob.trade_count_24h++;

    /* Callback */
    if (book->config.on_fill) {
        book->config.on_fill(fill, book->config.callback_ctx);
    }
}

/* ── Update TOB ────────────────────────────────────────────────────── */

static void update_tob(ob_book_t *book) {
    book->tob.resource = book->resource;
    book->tob.best_bid = book->bid_count > 0 ? book->bids[0].price : OB_PRICE_ZERO;
    book->tob.best_ask = book->ask_count > 0 ? book->asks[0].price : OB_PRICE_MAX;

    book->tob.bid_depth = 0;
    for (int i = 0; i < book->bid_count; i++)
        book->tob.bid_depth += book->bids[i].total_quantity;

    book->tob.ask_depth = 0;
    for (int i = 0; i < book->ask_count; i++)
        book->tob.ask_depth += book->asks[i].total_quantity;

    book->tob.timestamp = now_us();

    if (book->config.on_tob_update) {
        book->config.on_tob_update(&book->tob, book->config.callback_ctx);
    }
}

/* ── Price-Time Priority Matching (CLOB) ──────────────────────────── */

static int match_price_time(ob_book_t *book) {
    book->fill_count = 0;
    int total_fills = 0;

    while (book->bid_count > 0 && book->ask_count > 0) {
        ob_price_level_t *best_bid = &book->bids[0];
        ob_price_level_t *best_ask = &book->asks[0];

        /* Check if bid >= ask (crossing) */
        if (best_bid->price < best_ask->price) break;

        /* Get head orders at each level */
        ob_order_t *bid = get_order(book, best_bid->head_order_id);
        ob_order_t *ask = get_order(book, best_ask->head_order_id);
        if (!bid || !ask) break;

        /* Execute at the resting order's price (maker gets their price) */
        /* Convention: if bid is aggressor, execute at ask price; vice versa */
        ob_price_t exec_price;
        if (bid->timestamp > ask->timestamp)
            exec_price = ask->price;  /* ask was resting */
        else
            exec_price = bid->price;  /* bid was resting */

        uint64_t bid_remaining = bid->quantity - bid->filled;
        uint64_t ask_remaining = ask->quantity - ask->filled;
        uint64_t fill_qty = bid_remaining < ask_remaining ? bid_remaining : ask_remaining;

        execute_fill(book, bid, ask, exec_price, fill_qty);
        total_fills++;

        /* Update level quantities */
        best_bid->total_quantity -= fill_qty;
        best_ask->total_quantity -= fill_qty;

        /* Remove filled orders from levels */
        if (bid->filled >= bid->quantity) {
            best_bid->head_order_id = bid->_next;
            if (bid->_next) {
                ob_order_t *next = get_order(book, bid->_next);
                if (next) next->_prev = 0;
            } else {
                best_bid->tail_order_id = 0;
            }
            best_bid->order_count--;
            book->stats.orders_filled++;
        }

        if (ask->filled >= ask->quantity) {
            best_ask->head_order_id = ask->_next;
            if (ask->_next) {
                ob_order_t *next = get_order(book, ask->_next);
                if (next) next->_prev = 0;
            } else {
                best_ask->tail_order_id = 0;
            }
            best_ask->order_count--;
            book->stats.orders_filled++;
        }

        /* Clean up empty levels */
        if (best_bid->total_quantity == 0 && best_bid->order_count == 0) {
            remove_level_if_empty(book->bids, &book->bid_count, 0);
        }
        if (best_ask->total_quantity == 0 && best_ask->order_count == 0) {
            remove_level_if_empty(book->asks, &book->ask_count, 0);
        }
    }

    if (total_fills > 0) update_tob(book);
    return total_fills;
}

/* ── Batch Auction Clear ───────────────────────────────────────────── */
/*
 * Uniform clearing price: find the price where cumulative supply >= cumulative demand.
 * All matched orders execute at the same price.
 * Based on Budish et al. (2015) frequent batch auction.
 */

static int match_batch_uniform(ob_book_t *book) {
    if (book->bid_count == 0 || book->ask_count == 0) return 0;

    book->fill_count = 0;

    /* Build cumulative demand curve (bids, descending price) */
    uint64_t cum_demand[OB_MAX_PRICE_LEVELS];
    uint64_t running = 0;
    for (int i = 0; i < book->bid_count; i++) {
        running += book->bids[i].total_quantity;
        cum_demand[i] = running;
    }

    /* Build cumulative supply curve (asks, ascending price) */
    uint64_t cum_supply[OB_MAX_PRICE_LEVELS];
    running = 0;
    for (int i = 0; i < book->ask_count; i++) {
        running += book->asks[i].total_quantity;
        cum_supply[i] = running;
    }

    /* Find clearing price: highest bid price where cum_supply >= cum_demand
     * at that price level. We sweep bid levels and ask levels together. */
    ob_price_t clearing_price = OB_PRICE_ZERO;
    uint64_t clearing_qty = 0;

    /* For each bid level (highest first), find total supply at or below that price */
    for (int b = 0; b < book->bid_count; b++) {
        ob_price_t bid_price = book->bids[b].price;
        uint64_t demand_at = cum_demand[b];

        /* Sum all ask levels with price <= bid_price */
        uint64_t supply_at = 0;
        for (int a = 0; a < book->ask_count; a++) {
            if (book->asks[a].price <= bid_price) {
                supply_at += book->asks[a].total_quantity;
            } else break; /* asks are sorted ascending */
        }

        if (supply_at > 0 && demand_at > 0) {
            uint64_t matchable = supply_at < demand_at ? supply_at : demand_at;
            if (matchable > clearing_qty) {
                clearing_qty = matchable;
                /* Clearing price = midpoint of marginal bid and ask */
                /* Find the marginal ask price */
                ob_price_t marginal_ask = book->asks[0].price;
                uint64_t acc = 0;
                for (int a = 0; a < book->ask_count; a++) {
                    acc += book->asks[a].total_quantity;
                    if (acc >= matchable) {
                        marginal_ask = book->asks[a].price;
                        break;
                    }
                }
                clearing_price = (bid_price + marginal_ask) / 2;
            }
        }
    }

    if (clearing_qty == 0) return 0;

    /* Execute fills at uniform clearing price */
    uint64_t remaining = clearing_qty;
    int bid_idx = 0;
    int ask_idx = 0;

    /* Walk bids (highest first) and asks (lowest first) */
    while (remaining > 0 && bid_idx < book->bid_count && ask_idx < book->ask_count) {
        ob_price_level_t *blvl = &book->bids[bid_idx];
        ob_price_level_t *alvl = &book->asks[ask_idx];

        if (blvl->price < alvl->price) break; /* no more crossing */

        /* Match orders at these levels */
        uint64_t order_id = blvl->head_order_id;
        while (order_id && remaining > 0) {
            ob_order_t *bid = get_order(book, order_id);
            if (!bid) break;
            uint64_t next_id = bid->_next;

            uint64_t bid_rem = bid->quantity - bid->filled;

            /* Find asks to match against */
            uint64_t ask_oid = alvl->head_order_id;
            while (ask_oid && bid_rem > 0 && remaining > 0) {
                ob_order_t *ask = get_order(book, ask_oid);
                if (!ask) break;
                uint64_t ask_next = ask->_next;

                uint64_t ask_rem = ask->quantity - ask->filled;
                uint64_t fill_qty = bid_rem < ask_rem ? bid_rem : ask_rem;
                if (fill_qty > remaining) fill_qty = remaining;

                execute_fill(book, bid, ask, clearing_price, fill_qty);
                bid_rem -= fill_qty;
                remaining -= fill_qty;

                if (ask->filled >= ask->quantity) {
                    book->stats.orders_filled++;
                }

                ask_oid = ask_next;
            }

            if (bid->filled >= bid->quantity) {
                book->stats.orders_filled++;
            }

            order_id = next_id;
        }

        /* Advance to next level if current is exhausted */
        if (blvl->total_quantity == 0) bid_idx++;
        if (alvl->total_quantity == 0) ask_idx++;

        /* Safety: if we made no progress, break */
        if (remaining == clearing_qty) break;
    }

    /* Update batch state */
    book->batch.clearing_price = clearing_price;
    book->batch.cleared_quantity = clearing_qty - remaining;
    book->batch.epoch++;
    book->batch.open_time = now_us();
    book->batch.pending_bids = 0;
    book->batch.pending_asks = 0;
    book->stats.batches_cleared++;

    /* Rebuild book: remove filled orders, recalculate levels */
    /* (Simplified: just update quantities. A full impl would compact.) */
    for (int i = 0; i < OB_MAX_ORDERS; i++) {
        if (!book->order_used[i]) continue;
        ob_order_t *o = &book->orders[i];
        if (o->status == OB_ORDER_FILLED) {
            remove_from_book(book, o);
        }
    }

    /* Callback */
    if (book->config.on_batch_clear) {
        book->config.on_batch_clear(&book->batch, book->fill_buf,
                                     book->fill_count, book->config.callback_ctx);
    }

    update_tob(book);
    return book->fill_count;
}

/* ── CFMM (Constant Product AMM) ──────────────────────────────────── */

int ob_cfmm_swap(ob_book_t *book, ob_side_t side, uint64_t amount,
                  ob_price_t min_price, uint64_t *filled, ob_price_t *exec_price) {
    pthread_mutex_lock(&book->lock);
    ob_cfmm_pool_t *p = &book->pool;

    if (p->reserve_credit == 0 || p->reserve_resource == 0) {
        pthread_mutex_unlock(&book->lock);
        return -1;
    }

    uint64_t fee = (amount * p->fee_bps) / 10000;
    uint64_t amount_after_fee = amount - fee;

    if (side == OB_SIDE_BID) {
        /* Buying resource with credits */
        /* dy = y * dx / (x + dx) */
        uint64_t numerator = p->reserve_resource * amount_after_fee;
        uint64_t denominator = p->reserve_credit + amount_after_fee;
        uint64_t resource_out = numerator / denominator;

        if (resource_out == 0) { pthread_mutex_unlock(&book->lock); return -1; }

        /* Check slippage */
        ob_price_t effective_price = OB_PRICE_MAKE(amount / resource_out, 0);
        if (min_price != OB_PRICE_ZERO && effective_price > min_price) {
            pthread_mutex_unlock(&book->lock);
            return -1; /* slippage too high */
        }

        p->reserve_credit += amount;
        p->reserve_resource -= resource_out;
        p->total_fees_collected += fee;

        if (filled) *filled = resource_out;
        if (exec_price) *exec_price = effective_price;

    } else {
        /* Selling resource for credits */
        /* dx = x * dy / (y + dy) */
        uint64_t numerator = p->reserve_credit * amount_after_fee;
        uint64_t denominator = p->reserve_resource + amount_after_fee;
        uint64_t credit_out = numerator / denominator;

        if (credit_out == 0) { pthread_mutex_unlock(&book->lock); return -1; }

        ob_price_t effective_price = OB_PRICE_MAKE(credit_out / amount, 0);
        if (min_price != OB_PRICE_ZERO && effective_price < min_price) {
            pthread_mutex_unlock(&book->lock);
            return -1;
        }

        p->reserve_resource += amount;
        p->reserve_credit -= credit_out;
        p->total_fees_collected += fee;

        if (filled) *filled = credit_out;
        if (exec_price) *exec_price = effective_price;
    }

    /* Update k */
    p->k = p->reserve_credit * p->reserve_resource;

    pthread_mutex_unlock(&book->lock);
    return 0;
}

int ob_cfmm_add_liquidity(ob_book_t *book, uint64_t credit, uint64_t resource,
                            uint64_t *shares_out) {
    pthread_mutex_lock(&book->lock);
    ob_cfmm_pool_t *p = &book->pool;

    uint64_t shares;
    if (p->lp_shares_total == 0) {
        /* First LP: shares = sqrt(credit * resource) approximated */
        uint64_t product = credit * resource;
        shares = 1;
        while (shares * shares < product) shares++;
        /* Back off if we overshot */
        if (shares * shares > product && shares > 0) shares--;
    } else {
        /* Proportional to existing pool */
        uint64_t share_c = (credit * p->lp_shares_total) / p->reserve_credit;
        uint64_t share_r = (resource * p->lp_shares_total) / p->reserve_resource;
        shares = share_c < share_r ? share_c : share_r;
    }

    p->reserve_credit += credit;
    p->reserve_resource += resource;
    p->k = p->reserve_credit * p->reserve_resource;
    p->lp_shares_total += shares;

    if (shares_out) *shares_out = shares;

    pthread_mutex_unlock(&book->lock);
    return 0;
}

int ob_cfmm_remove_liquidity(ob_book_t *book, uint64_t shares,
                               uint64_t *credit_out, uint64_t *resource_out) {
    pthread_mutex_lock(&book->lock);
    ob_cfmm_pool_t *p = &book->pool;

    if (shares > p->lp_shares_total) {
        pthread_mutex_unlock(&book->lock);
        return -1;
    }

    uint64_t c = (shares * p->reserve_credit) / p->lp_shares_total;
    uint64_t r = (shares * p->reserve_resource) / p->lp_shares_total;

    p->reserve_credit -= c;
    p->reserve_resource -= r;
    p->lp_shares_total -= shares;
    p->k = p->reserve_credit * p->reserve_resource;

    if (credit_out) *credit_out = c;
    if (resource_out) *resource_out = r;

    pthread_mutex_unlock(&book->lock);
    return 0;
}

ob_cfmm_pool_t ob_cfmm_get_pool(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_cfmm_pool_t p = book->pool;
    pthread_mutex_unlock(&book->lock);
    return p;
}

/* ── Public API: Lifecycle ─────────────────────────────────────────── */

ob_book_t *ob_book_create(ob_resource_t resource, const ob_config_t *config) {
    ob_book_t *book = calloc(1, sizeof(*book));
    if (!book) return NULL;

    book->resource = resource;
    book->config = *config;
    book->next_order_id = 1;
    book->next_trade_id = 1;
    pthread_mutex_init(&book->lock, NULL);

    /* Set default match mode based on level */
    if (config->match_mode == 0) {
        switch (config->level) {
            case OB_LEVEL_L0:
            case OB_LEVEL_L1: book->config.match_mode = OB_MATCH_PRICE_TIME; break;
            case OB_LEVEL_L2: book->config.match_mode = OB_MATCH_BATCH_UNIFORM; break;
            case OB_LEVEL_L3:
            case OB_LEVEL_L4: book->config.match_mode = OB_MATCH_CFMM; break;
        }
    }

    /* Init batch state */
    book->batch.interval_ms = config->batch_interval_ms > 0 ? config->batch_interval_ms : 1000;
    book->batch.open_time = now_us();

    /* Init CFMM pool if configured */
    if (config->enable_cfmm) {
        book->pool.resource = resource;
        book->pool.reserve_credit = config->cfmm_initial_credit;
        book->pool.reserve_resource = config->cfmm_initial_resource;
        book->pool.k = config->cfmm_initial_credit * config->cfmm_initial_resource;
        book->pool.fee_bps = config->cfmm_fee_bps > 0 ? config->cfmm_fee_bps : 30;
    }

    /* Init TOB */
    book->tob.resource = resource;
    book->tob.best_bid = OB_PRICE_ZERO;
    book->tob.best_ask = OB_PRICE_MAX;

    return book;
}

void ob_book_destroy(ob_book_t *book) {
    if (!book) return;
    pthread_mutex_destroy(&book->lock);
    free(book);
}

/* ── Public API: Order Management ──────────────────────────────────── */

uint64_t ob_submit(ob_book_t *book, const ob_order_t *order) {
    pthread_mutex_lock(&book->lock);

    /* Validate */
    if (book->config.min_stake > 0 && order->node.stake < book->config.min_stake) {
        pthread_mutex_unlock(&book->lock);
        return 0;
    }
    if (book->config.min_reputation > 0 && order->node.reputation < book->config.min_reputation) {
        pthread_mutex_unlock(&book->lock);
        return 0;
    }
    if (book->config.min_order_size > 0 && order->quantity < book->config.min_order_size) {
        pthread_mutex_unlock(&book->lock);
        return 0;
    }
    if (book->config.max_order_size > 0 && order->quantity > book->config.max_order_size) {
        pthread_mutex_unlock(&book->lock);
        return 0;
    }

    int slot = find_order_slot(book);
    if (slot < 0) {
        book->stats.orders_rejected++;
        pthread_mutex_unlock(&book->lock);
        return 0;
    }

    ob_order_t *o = &book->orders[slot];
    *o = *order;
    o->id = book->next_order_id++;
    o->status = OB_ORDER_OPEN;
    o->filled = 0;
    o->timestamp = now_us();
    o->batch_id = book->batch.epoch;
    o->_next = o->_prev = 0;
    book->order_used[slot] = true;
    book->order_count++;
    book->stats.orders_submitted++;

    /* Handle based on type */
    switch (o->type) {
        case OB_TYPE_MARKET: {
            /* Market orders: try immediate match against CFMM or book */
            if (book->config.enable_cfmm && book->pool.k > 0) {
                /* Try CFMM first for market orders */
                uint64_t filled_qty = 0;
                ob_price_t ep;
                int rc = ob_cfmm_swap(book, o->side, o->quantity, OB_PRICE_ZERO, &filled_qty, &ep);
                if (rc == 0 && filled_qty > 0) {
                    o->filled = filled_qty;
                    o->status = OB_ORDER_FILLED;
                    book->stats.orders_filled++;
                    pthread_mutex_unlock(&book->lock);
                    return o->id;
                }
            }
            /* Fall through to book */
            add_to_book(book, o);
            if (book->config.match_mode == OB_MATCH_PRICE_TIME)
                match_price_time(book);
            break;
        }

        case OB_TYPE_FOK: {
            /* Fill-or-kill: check if we can fill entirely, else reject */
            /* For now, add to book and try match, reject if not fully filled */
            add_to_book(book, o);
            if (book->config.match_mode == OB_MATCH_PRICE_TIME)
                match_price_time(book);
            if (o->filled < o->quantity) {
                remove_from_book(book, o);
                o->status = OB_ORDER_REJECTED;
                o->filled = 0;
                book->stats.orders_rejected++;
            }
            break;
        }

        case OB_TYPE_IOC: {
            /* Immediate-or-cancel: match what we can, cancel the rest */
            add_to_book(book, o);
            if (book->config.match_mode == OB_MATCH_PRICE_TIME)
                match_price_time(book);
            if (o->filled < o->quantity && o->filled > 0) {
                remove_from_book(book, o);
                o->status = OB_ORDER_PARTIAL;
                book->stats.orders_partial++;
            } else if (o->filled == 0) {
                remove_from_book(book, o);
                o->status = OB_ORDER_CANCELLED;
                book->stats.orders_cancelled++;
            }
            break;
        }

        case OB_TYPE_BATCH: {
            /* Just add to book, will be matched at next batch clear */
            add_to_book(book, o);
            if (o->side == OB_SIDE_BID) book->batch.pending_bids++;
            else book->batch.pending_asks++;
            break;
        }

        case OB_TYPE_LIMIT:
        case OB_TYPE_GTC:
        default: {
            add_to_book(book, o);
            /* Try immediate match for continuous mode */
            if (book->config.match_mode == OB_MATCH_PRICE_TIME)
                match_price_time(book);
            break;
        }
    }

    update_tob(book);
    uint64_t oid = o->id;
    pthread_mutex_unlock(&book->lock);
    return oid;
}

int ob_cancel(ob_book_t *book, uint64_t order_id) {
    pthread_mutex_lock(&book->lock);
    ob_order_t *o = get_order(book, order_id);
    if (!o || o->status == OB_ORDER_FILLED || o->status == OB_ORDER_CANCELLED) {
        pthread_mutex_unlock(&book->lock);
        return -1;
    }

    ob_order_status_t old = o->status;
    remove_from_book(book, o);
    o->status = OB_ORDER_CANCELLED;
    book->stats.orders_cancelled++;

    if (book->config.on_order_update)
        book->config.on_order_update(o, old, book->config.callback_ctx);

    update_tob(book);
    pthread_mutex_unlock(&book->lock);
    return 0;
}

int ob_modify(ob_book_t *book, uint64_t order_id, ob_price_t new_price, uint64_t new_qty) {
    pthread_mutex_lock(&book->lock);
    ob_order_t *o = get_order(book, order_id);
    if (!o || o->status == OB_ORDER_FILLED || o->status == OB_ORDER_CANCELLED) {
        pthread_mutex_unlock(&book->lock);
        return -1;
    }

    /* Cancel-replace: remove, modify, re-add */
    remove_from_book(book, o);
    if (new_price != OB_PRICE_ZERO) o->price = new_price;
    if (new_qty > 0) o->quantity = new_qty;
    o->timestamp = now_us(); /* loses time priority (standard for modify) */
    add_to_book(book, o);

    /* Try match in continuous mode */
    if (book->config.match_mode == OB_MATCH_PRICE_TIME)
        match_price_time(book);

    update_tob(book);
    pthread_mutex_unlock(&book->lock);
    return 0;
}

const ob_order_t *ob_get_order(ob_book_t *book, uint64_t order_id) {
    pthread_mutex_lock(&book->lock);
    const ob_order_t *o = get_order(book, order_id);
    pthread_mutex_unlock(&book->lock);
    return o;
}

/* ── Public API: Matching ──────────────────────────────────────────── */

int ob_match(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    int fills;
    switch (book->config.match_mode) {
        case OB_MATCH_PRICE_TIME: fills = match_price_time(book); break;
        case OB_MATCH_BATCH_UNIFORM: fills = match_batch_uniform(book); break;
        default: fills = match_price_time(book); break;
    }
    pthread_mutex_unlock(&book->lock);
    return fills;
}

int ob_batch_clear(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    int fills = match_batch_uniform(book);
    pthread_mutex_unlock(&book->lock);
    return fills;
}

int ob_tick(ob_book_t *book, uint64_t now_us_val) {
    pthread_mutex_lock(&book->lock);

    int fills = 0;

    /* Check batch interval */
    if (book->config.match_mode == OB_MATCH_BATCH_UNIFORM) {
        uint64_t elapsed_us = now_us_val - book->batch.open_time;
        uint64_t interval_us = (uint64_t)book->batch.interval_ms * 1000;
        if (elapsed_us >= interval_us) {
            fills = match_batch_uniform(book);
        } else {
            /* Update indicative clearing price */
            if (book->bid_count > 0 && book->ask_count > 0 &&
                book->bids[0].price >= book->asks[0].price) {
                book->batch.indicative_price =
                    (book->bids[0].price + book->asks[0].price) / 2;
            }
        }
    }

    /* Check order expiry */
    for (int i = 0; i < OB_MAX_ORDERS; i++) {
        if (!book->order_used[i]) continue;
        ob_order_t *o = &book->orders[i];
        if (o->expiry > 0 && now_us_val > o->expiry &&
            o->status != OB_ORDER_FILLED && o->status != OB_ORDER_CANCELLED) {
            remove_from_book(book, o);
            o->status = OB_ORDER_EXPIRED;
            book->stats.orders_expired++;
        }
    }

    pthread_mutex_unlock(&book->lock);
    return fills;
}

/* ── Public API: Market Data ───────────────────────────────────────── */

ob_top_of_book_t ob_get_tob(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_top_of_book_t tob = book->tob;
    pthread_mutex_unlock(&book->lock);
    return tob;
}

int ob_get_depth(ob_book_t *book, ob_side_t side,
                  ob_price_level_t *levels, int max_levels) {
    pthread_mutex_lock(&book->lock);
    ob_price_level_t *src = (side == OB_SIDE_BID) ? book->bids : book->asks;
    int count = (side == OB_SIDE_BID) ? book->bid_count : book->ask_count;
    int n = count < max_levels ? count : max_levels;
    memcpy(levels, src, n * sizeof(ob_price_level_t));
    pthread_mutex_unlock(&book->lock);
    return n;
}

ob_price_t ob_get_midpoint(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_price_t mid = OB_PRICE_ZERO;
    if (book->bid_count > 0 && book->ask_count > 0) {
        mid = (book->bids[0].price + book->asks[0].price) / 2;
    }
    pthread_mutex_unlock(&book->lock);
    return mid;
}

ob_price_t ob_get_spread(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_price_t spread = OB_PRICE_MAX;
    if (book->bid_count > 0 && book->ask_count > 0) {
        spread = book->asks[0].price - book->bids[0].price;
    }
    pthread_mutex_unlock(&book->lock);
    return spread;
}

ob_price_t ob_get_vwap(ob_book_t *book, uint64_t quantity) {
    pthread_mutex_lock(&book->lock);

    /* VWAP across ask levels for given buy quantity */
    ob_price_t total_value = 0;
    uint64_t total_qty = 0;
    uint64_t remaining = quantity;

    for (int i = 0; i < book->ask_count && remaining > 0; i++) {
        uint64_t avail = book->asks[i].total_quantity;
        uint64_t take = avail < remaining ? avail : remaining;
        total_value += book->asks[i].price * take;
        total_qty += take;
        remaining -= take;
    }

    ob_price_t vwap = total_qty > 0 ? total_value / total_qty : OB_PRICE_ZERO;
    pthread_mutex_unlock(&book->lock);
    return vwap;
}

ob_batch_state_t ob_get_batch_state(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_batch_state_t s = book->batch;
    pthread_mutex_unlock(&book->lock);
    return s;
}

ob_stats_t ob_get_stats(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    ob_stats_t s = book->stats;
    pthread_mutex_unlock(&book->lock);
    return s;
}

void ob_reset_stats(ob_book_t *book) {
    pthread_mutex_lock(&book->lock);
    memset(&book->stats, 0, sizeof(book->stats));
    pthread_mutex_unlock(&book->lock);
}

/* ── Hierarchical Exchange ─────────────────────────────────────────── */

struct ob_exchange {
    ob_exchange_config_t config;
    ob_book_t *books[OB_RESOURCE_COUNT][OB_MAX_LEVELS];
    pthread_mutex_t lock;
};

ob_exchange_t *ob_exchange_create(const ob_exchange_config_t *config) {
    ob_exchange_t *ex = calloc(1, sizeof(*ex));
    if (!ex) return NULL;

    ex->config = *config;
    pthread_mutex_init(&ex->lock, NULL);

    int levels = config->active_levels > 0 ? config->active_levels : 2;
    if (levels > OB_MAX_LEVELS) levels = OB_MAX_LEVELS;

    for (int r = 0; r < OB_RESOURCE_COUNT; r++) {
        for (int l = 0; l < levels; l++) {
            ob_config_t cfg = config->level_config[l];
            cfg.level = (ob_level_t)l;

            /* Set defaults per level */
            if (cfg.batch_interval_ms == 0) {
                switch (l) {
                    case 0: cfg.batch_interval_ms = 50; break;
                    case 1: cfg.batch_interval_ms = 100; break;
                    case 2: cfg.batch_interval_ms = 1000; break;
                    case 3: cfg.batch_interval_ms = 5000; break;
                    case 4: cfg.batch_interval_ms = 30000; break;
                }
            }

            ex->books[r][l] = ob_book_create((ob_resource_t)r, &cfg);
        }
    }

    return ex;
}

void ob_exchange_destroy(ob_exchange_t *ex) {
    if (!ex) return;
    for (int r = 0; r < OB_RESOURCE_COUNT; r++) {
        for (int l = 0; l < OB_MAX_LEVELS; l++) {
            if (ex->books[r][l]) ob_book_destroy(ex->books[r][l]);
        }
    }
    pthread_mutex_destroy(&ex->lock);
    free(ex);
}

ob_book_t *ob_exchange_get_book(ob_exchange_t *ex, ob_resource_t resource, ob_level_t level) {
    if (resource >= OB_RESOURCE_COUNT || level >= OB_MAX_LEVELS) return NULL;
    return ex->books[resource][level];
}

uint64_t ob_exchange_submit(ob_exchange_t *ex, const ob_order_t *order) {
    /* Auto-route to appropriate level based on quantity and node level */
    ob_level_t level = order->node.level;
    if (level >= OB_MAX_LEVELS) level = OB_LEVEL_L1;

    ob_book_t *book = ex->books[order->resource][level];
    if (!book) return 0;

    return ob_submit(book, order);
}

int ob_exchange_tick(ob_exchange_t *ex, uint64_t now_us_val) {
    int total_fills = 0;
    for (int r = 0; r < OB_RESOURCE_COUNT; r++) {
        for (int l = 0; l < OB_MAX_LEVELS; l++) {
            if (ex->books[r][l]) {
                total_fills += ob_tick(ex->books[r][l], now_us_val);
            }
        }
    }
    return total_fills;
}

ob_top_of_book_t ob_exchange_get_tob(ob_exchange_t *ex, ob_resource_t resource) {
    /* Return best TOB across all levels */
    ob_top_of_book_t best = {0};
    best.resource = resource;
    best.best_bid = OB_PRICE_ZERO;
    best.best_ask = OB_PRICE_MAX;

    for (int l = 0; l < OB_MAX_LEVELS; l++) {
        if (!ex->books[resource][l]) continue;
        ob_top_of_book_t tob = ob_get_tob(ex->books[resource][l]);
        if (tob.best_bid > best.best_bid) best.best_bid = tob.best_bid;
        if (tob.best_ask < best.best_ask) best.best_ask = tob.best_ask;
        best.bid_depth += tob.bid_depth;
        best.ask_depth += tob.ask_depth;
        best.volume_24h += tob.volume_24h;
        best.trade_count_24h += tob.trade_count_24h;
    }

    best.timestamp = now_us();
    return best;
}

ob_stats_t ob_exchange_get_stats(ob_exchange_t *ex) {
    ob_stats_t total = {0};
    for (int r = 0; r < OB_RESOURCE_COUNT; r++) {
        for (int l = 0; l < OB_MAX_LEVELS; l++) {
            if (!ex->books[r][l]) continue;
            ob_stats_t s = ob_get_stats(ex->books[r][l]);
            total.orders_submitted += s.orders_submitted;
            total.orders_filled += s.orders_filled;
            total.orders_partial += s.orders_partial;
            total.orders_cancelled += s.orders_cancelled;
            total.orders_expired += s.orders_expired;
            total.orders_rejected += s.orders_rejected;
            total.trades_executed += s.trades_executed;
            total.total_volume += s.total_volume;
            total.batches_cleared += s.batches_cleared;
        }
    }
    return total;
}

/* ── Serialization ─────────────────────────────────────────────────── */

int ob_tob_serialize(const ob_top_of_book_t *tob, uint8_t *buf, size_t buf_sz) {
    if (buf_sz < 88) return -1;
    size_t off = 0;

    memcpy(buf + off, &tob->resource, 4); off += 4;
    memcpy(buf + off, &tob->best_bid, 8); off += 8;
    memcpy(buf + off, &tob->best_ask, 8); off += 8;
    memcpy(buf + off, &tob->bid_depth, 8); off += 8;
    memcpy(buf + off, &tob->ask_depth, 8); off += 8;
    memcpy(buf + off, &tob->last_price, 8); off += 8;
    memcpy(buf + off, &tob->last_quantity, 8); off += 8;
    memcpy(buf + off, &tob->volume_24h, 8); off += 8;
    memcpy(buf + off, &tob->vwap_24h, 8); off += 8;
    memcpy(buf + off, &tob->trade_count_24h, 8); off += 8;
    memcpy(buf + off, &tob->timestamp, 8); off += 8;

    return (int)off;
}

int ob_tob_deserialize(const uint8_t *buf, size_t len, ob_top_of_book_t *tob) {
    if (len < 88) return -1;
    size_t off = 0;

    memcpy(&tob->resource, buf + off, 4); off += 4;
    memcpy(&tob->best_bid, buf + off, 8); off += 8;
    memcpy(&tob->best_ask, buf + off, 8); off += 8;
    memcpy(&tob->bid_depth, buf + off, 8); off += 8;
    memcpy(&tob->ask_depth, buf + off, 8); off += 8;
    memcpy(&tob->last_price, buf + off, 8); off += 8;
    memcpy(&tob->last_quantity, buf + off, 8); off += 8;
    memcpy(&tob->volume_24h, buf + off, 8); off += 8;
    memcpy(&tob->vwap_24h, buf + off, 8); off += 8;
    memcpy(&tob->trade_count_24h, buf + off, 8); off += 8;
    memcpy(&tob->timestamp, buf + off, 8); off += 8;

    return (int)off;
}

int ob_order_serialize(const ob_order_t *order, uint8_t *buf, size_t buf_sz) {
    if (buf_sz < 256) return -1;
    size_t off = 0;

    memcpy(buf + off, &order->id, 8); off += 8;
    memcpy(buf + off, &order->side, 4); off += 4;
    memcpy(buf + off, &order->type, 4); off += 4;
    memcpy(buf + off, &order->resource, 4); off += 4;
    memcpy(buf + off, &order->price, 8); off += 8;
    memcpy(buf + off, &order->quantity, 8); off += 8;
    memcpy(buf + off, &order->filled, 8); off += 8;
    memcpy(buf + off, &order->status, 4); off += 4;
    memcpy(buf + off, order->node.id, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
    memcpy(buf + off, &order->timestamp, 8); off += 8;
    memcpy(buf + off, &order->expiry, 8); off += 8;
    memcpy(buf + off, order->tag, OB_ORDER_TAG_LEN); off += OB_ORDER_TAG_LEN;
    memcpy(buf + off, order->capability, OB_PROVIDER_CAP_LEN); off += OB_PROVIDER_CAP_LEN;

    return (int)off;
}

int ob_order_deserialize(const uint8_t *buf, size_t len, ob_order_t *order) {
    if (len < 256) return -1;
    size_t off = 0;

    memset(order, 0, sizeof(*order));
    memcpy(&order->id, buf + off, 8); off += 8;
    memcpy(&order->side, buf + off, 4); off += 4;
    memcpy(&order->type, buf + off, 4); off += 4;
    memcpy(&order->resource, buf + off, 4); off += 4;
    memcpy(&order->price, buf + off, 8); off += 8;
    memcpy(&order->quantity, buf + off, 8); off += 8;
    memcpy(&order->filled, buf + off, 8); off += 8;
    memcpy(&order->status, buf + off, 4); off += 4;
    memcpy(order->node.id, buf + off, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
    memcpy(&order->timestamp, buf + off, 8); off += 8;
    memcpy(&order->expiry, buf + off, 8); off += 8;
    memcpy(order->tag, buf + off, OB_ORDER_TAG_LEN); off += OB_ORDER_TAG_LEN;
    memcpy(order->capability, buf + off, OB_PROVIDER_CAP_LEN); off += OB_PROVIDER_CAP_LEN;

    return (int)off;
}

int ob_fills_serialize(const ob_fill_t *fills, int n, uint8_t *buf, size_t buf_sz) {
    size_t per_fill = 8 + 8 + 8 + OB_NODE_ID_BYTES + OB_NODE_ID_BYTES + 4 + 8 + 8 + 8;
    if (buf_sz < 4 + (size_t)n * per_fill) return -1;

    size_t off = 0;
    int32_t count = n;
    memcpy(buf + off, &count, 4); off += 4;

    for (int i = 0; i < n; i++) {
        memcpy(buf + off, &fills[i].trade_id, 8); off += 8;
        memcpy(buf + off, &fills[i].bid_order_id, 8); off += 8;
        memcpy(buf + off, &fills[i].ask_order_id, 8); off += 8;
        memcpy(buf + off, fills[i].buyer.id, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
        memcpy(buf + off, fills[i].seller.id, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
        memcpy(buf + off, &fills[i].resource, 4); off += 4;
        memcpy(buf + off, &fills[i].price, 8); off += 8;
        memcpy(buf + off, &fills[i].quantity, 8); off += 8;
        memcpy(buf + off, &fills[i].timestamp, 8); off += 8;
    }

    return (int)off;
}

int ob_fills_deserialize(const uint8_t *buf, size_t len, ob_fill_t *fills, int *n) {
    if (len < 4) return -1;

    size_t off = 0;
    int32_t count;
    memcpy(&count, buf + off, 4); off += 4;

    size_t per_fill = 8 + 8 + 8 + OB_NODE_ID_BYTES + OB_NODE_ID_BYTES + 4 + 8 + 8 + 8;
    if (len < 4 + (size_t)count * per_fill) return -1;

    for (int i = 0; i < count && i < *n; i++) {
        memset(&fills[i], 0, sizeof(fills[i]));
        memcpy(&fills[i].trade_id, buf + off, 8); off += 8;
        memcpy(&fills[i].bid_order_id, buf + off, 8); off += 8;
        memcpy(&fills[i].ask_order_id, buf + off, 8); off += 8;
        memcpy(fills[i].buyer.id, buf + off, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
        memcpy(fills[i].seller.id, buf + off, OB_NODE_ID_BYTES); off += OB_NODE_ID_BYTES;
        memcpy(&fills[i].resource, buf + off, 4); off += 4;
        memcpy(&fills[i].price, buf + off, 8); off += 8;
        memcpy(&fills[i].quantity, buf + off, 8); off += 8;
        memcpy(&fills[i].timestamp, buf + off, 8); off += 8;
    }

    *n = count;
    return (int)off;
}
