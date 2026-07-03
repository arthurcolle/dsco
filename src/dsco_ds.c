#include "dsco_ds.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ── dsco_vec ───────────────────────────────────────────────────────────── */

void dsco_vec_init(dsco_vec *v, size_t elem_size) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem_size ? elem_size : 1;
}

void dsco_vec_free(dsco_vec *v) {
    if (!v)
        return;
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}

bool dsco_vec_reserve(dsco_vec *v, size_t min_cap) {
    if (v->cap >= min_cap)
        return true;
    size_t nc = v->cap ? v->cap * 2 : 8;
    while (nc < min_cap)
        nc *= 2;
    unsigned char *nd = realloc(v->data, nc * v->elem);
    if (!nd)
        return false;
    v->data = nd;
    v->cap = nc;
    return true;
}

void *dsco_vec_push(dsco_vec *v, const void *elem) {
    if (v->len + 1 > v->cap && !dsco_vec_reserve(v, v->len + 1))
        return NULL;
    void *slot = v->data + v->len * v->elem;
    if (elem)
        memcpy(slot, elem, v->elem);
    else
        memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void *dsco_vec_at(const dsco_vec *v, size_t i) {
    if (i >= v->len)
        return NULL;
    return v->data + i * v->elem;
}

size_t dsco_vec_len(const dsco_vec *v) {
    return v->len;
}

void dsco_vec_pop(dsco_vec *v) {
    if (v->len)
        v->len--;
}

void dsco_vec_clear(dsco_vec *v) {
    v->len = 0;
}

/* ── dsco_map ───────────────────────────────────────────────────────────── */

typedef struct map_node {
    char *key;
    void *val;
    struct map_node *next;
} map_node;

struct dsco_map {
    map_node **buckets;
    size_t nbuckets;
    size_t size;
};

dsco_map *dsco_map_new(void) {
    dsco_map *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->nbuckets = 16;
    m->buckets = calloc(m->nbuckets, sizeof(map_node *));
    if (!m->buckets) {
        free(m);
        return NULL;
    }
    return m;
}

static void map_resize(dsco_map *m) {
    size_t nb = m->nbuckets * 2;
    map_node **nbuckets = calloc(nb, sizeof(map_node *));
    if (!nbuckets)
        return; /* stay at current size — correctness unaffected */
    for (size_t i = 0; i < m->nbuckets; i++) {
        map_node *n = m->buckets[i];
        while (n) {
            map_node *next = n->next;
            size_t b = fnv1a(n->key) & (nb - 1);
            n->next = nbuckets[b];
            nbuckets[b] = n;
            n = next;
        }
    }
    free(m->buckets);
    m->buckets = nbuckets;
    m->nbuckets = nb;
}

bool dsco_map_put(dsco_map *m, const char *key, void *val, void (*free_val)(void *)) {
    if (!m || !key)
        return false;
    size_t b = fnv1a(key) & (m->nbuckets - 1);
    for (map_node *n = m->buckets[b]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            if (free_val && n->val && n->val != val)
                free_val(n->val);
            n->val = val;
            return true;
        }
    }
    map_node *n = malloc(sizeof(*n));
    if (!n)
        return false;
    n->key = strdup(key);
    if (!n->key) {
        free(n);
        return false;
    }
    n->val = val;
    n->next = m->buckets[b];
    m->buckets[b] = n;
    m->size++;
    if (m->size > (m->nbuckets * 3) / 4)
        map_resize(m);
    return true;
}

void *dsco_map_get(const dsco_map *m, const char *key) {
    if (!m || !key)
        return NULL;
    size_t b = fnv1a(key) & (m->nbuckets - 1);
    for (map_node *n = m->buckets[b]; n; n = n->next)
        if (strcmp(n->key, key) == 0)
            return n->val;
    return NULL;
}

bool dsco_map_has(const dsco_map *m, const char *key) {
    if (!m || !key)
        return false;
    size_t b = fnv1a(key) & (m->nbuckets - 1);
    for (map_node *n = m->buckets[b]; n; n = n->next)
        if (strcmp(n->key, key) == 0)
            return true;
    return false;
}

bool dsco_map_del(dsco_map *m, const char *key, void (*free_val)(void *)) {
    if (!m || !key)
        return false;
    size_t b = fnv1a(key) & (m->nbuckets - 1);
    map_node **pp = &m->buckets[b];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            map_node *dead = *pp;
            *pp = dead->next;
            if (free_val && dead->val)
                free_val(dead->val);
            free(dead->key);
            free(dead);
            m->size--;
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

size_t dsco_map_size(const dsco_map *m) {
    return m ? m->size : 0;
}

void dsco_map_foreach(const dsco_map *m, int (*fn)(const char *, void *, void *), void *ctx) {
    if (!m || !fn)
        return;
    for (size_t i = 0; i < m->nbuckets; i++)
        for (map_node *n = m->buckets[i]; n; n = n->next)
            if (fn(n->key, n->val, ctx))
                return;
}

void dsco_map_free(dsco_map *m, void (*free_val)(void *)) {
    if (!m)
        return;
    for (size_t i = 0; i < m->nbuckets; i++) {
        map_node *n = m->buckets[i];
        while (n) {
            map_node *next = n->next;
            if (free_val && n->val)
                free_val(n->val);
            free(n->key);
            free(n);
            n = next;
        }
    }
    free(m->buckets);
    free(m);
}

/* ── dsco_lru ───────────────────────────────────────────────────────────── */

typedef struct lru_node {
    char *key;
    void *val;
    size_t vlen;
    struct lru_node *hnext;       /* hash chain */
    struct lru_node *prev, *next; /* recency list (head = MRU) */
} lru_node;

struct dsco_lru {
    lru_node **buckets;
    size_t nbuckets;
    lru_node *head, *tail;
    size_t size, cap;
};

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p ? p : 1;
}

dsco_lru *dsco_lru_new(size_t capacity) {
    if (capacity == 0)
        capacity = 1;
    dsco_lru *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->cap = capacity;
    c->nbuckets = next_pow2(capacity * 2);
    c->buckets = calloc(c->nbuckets, sizeof(lru_node *));
    if (!c->buckets) {
        free(c);
        return NULL;
    }
    return c;
}

static void lru_dll_unlink(dsco_lru *c, lru_node *n) {
    if (n->prev)
        n->prev->next = n->next;
    else
        c->head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        c->tail = n->prev;
    n->prev = n->next = NULL;
}

static void lru_dll_push_front(dsco_lru *c, lru_node *n) {
    n->prev = NULL;
    n->next = c->head;
    if (c->head)
        c->head->prev = n;
    c->head = n;
    if (!c->tail)
        c->tail = n;
}

static void lru_hash_remove(dsco_lru *c, lru_node *n) {
    size_t b = fnv1a(n->key) & (c->nbuckets - 1);
    lru_node **pp = &c->buckets[b];
    while (*pp) {
        if (*pp == n) {
            *pp = n->hnext;
            return;
        }
        pp = &(*pp)->hnext;
    }
}

static lru_node *lru_find(dsco_lru *c, const char *key) {
    size_t b = fnv1a(key) & (c->nbuckets - 1);
    for (lru_node *n = c->buckets[b]; n; n = n->hnext)
        if (strcmp(n->key, key) == 0)
            return n;
    return NULL;
}

const void *dsco_lru_get(dsco_lru *c, const char *key, size_t *len) {
    if (!c || !key)
        return NULL;
    lru_node *n = lru_find(c, key);
    if (!n)
        return NULL;
    lru_dll_unlink(c, n);
    lru_dll_push_front(c, n);
    if (len)
        *len = n->vlen;
    return n->val;
}

bool dsco_lru_put(dsco_lru *c, const char *key, const void *val, size_t len) {
    if (!c || !key)
        return false;
    lru_node *n = lru_find(c, key);
    if (n) {
        void *nv = malloc(len ? len : 1);
        if (!nv)
            return false;
        if (val && len)
            memcpy(nv, val, len);
        free(n->val);
        n->val = nv;
        n->vlen = len;
        lru_dll_unlink(c, n);
        lru_dll_push_front(c, n);
        return true;
    }
    n = calloc(1, sizeof(*n));
    if (!n)
        return false;
    n->key = strdup(key);
    n->val = malloc(len ? len : 1);
    if (!n->key || !n->val) {
        free(n->key);
        free(n->val);
        free(n);
        return false;
    }
    if (val && len)
        memcpy(n->val, val, len);
    n->vlen = len;
    size_t b = fnv1a(key) & (c->nbuckets - 1);
    n->hnext = c->buckets[b];
    c->buckets[b] = n;
    lru_dll_push_front(c, n);
    c->size++;
    if (c->size > c->cap && c->tail) {
        lru_node *ev = c->tail;
        lru_dll_unlink(c, ev);
        lru_hash_remove(c, ev);
        free(ev->key);
        free(ev->val);
        free(ev);
        c->size--;
    }
    return true;
}

bool dsco_lru_del(dsco_lru *c, const char *key) {
    if (!c || !key)
        return false;
    lru_node *n = lru_find(c, key);
    if (!n)
        return false;
    lru_dll_unlink(c, n);
    lru_hash_remove(c, n);
    free(n->key);
    free(n->val);
    free(n);
    c->size--;
    return true;
}

size_t dsco_lru_size(const dsco_lru *c) {
    return c ? c->size : 0;
}

void dsco_lru_free(dsco_lru *c) {
    if (!c)
        return;
    lru_node *n = c->head;
    while (n) {
        lru_node *next = n->next;
        free(n->key);
        free(n->val);
        free(n);
        n = next;
    }
    free(c->buckets);
    free(c);
}

/* ── dsco_heap ──────────────────────────────────────────────────────────── */

struct dsco_heap {
    unsigned char *data;
    size_t elem, len, cap;
    int (*cmp)(const void *, const void *);
};

dsco_heap *dsco_heap_new(size_t elem_size, int (*cmp)(const void *, const void *)) {
    if (!elem_size || !cmp)
        return NULL;
    dsco_heap *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->elem = elem_size;
    h->cmp = cmp;
    return h;
}

static void *heap_at(dsco_heap *h, size_t i) {
    return h->data + i * h->elem;
}

static void heap_swap(dsco_heap *h, size_t a, size_t b, unsigned char *tmp) {
    memcpy(tmp, heap_at(h, a), h->elem);
    memcpy(heap_at(h, a), heap_at(h, b), h->elem);
    memcpy(heap_at(h, b), tmp, h->elem);
}

bool dsco_heap_push(dsco_heap *h, const void *elem) {
    if (!h || !elem)
        return false;
    if (h->len + 1 > h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 8;
        unsigned char *nd = realloc(h->data, nc * h->elem);
        if (!nd)
            return false;
        h->data = nd;
        h->cap = nc;
    }
    unsigned char tmp[256];
    unsigned char *tp = h->elem <= sizeof(tmp) ? tmp : malloc(h->elem);
    if (!tp)
        return false;
    memcpy(heap_at(h, h->len), elem, h->elem);
    size_t i = h->len++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h->cmp(heap_at(h, i), heap_at(h, p)) < 0) {
            heap_swap(h, i, p, tp);
            i = p;
        } else {
            break;
        }
    }
    if (tp != tmp)
        free(tp);
    return true;
}

bool dsco_heap_pop(dsco_heap *h, void *out) {
    if (!h || h->len == 0)
        return false;
    if (out)
        memcpy(out, heap_at(h, 0), h->elem);
    h->len--;
    if (h->len == 0)
        return true;
    memcpy(heap_at(h, 0), heap_at(h, h->len), h->elem);
    unsigned char tmp[256];
    unsigned char *tp = h->elem <= sizeof(tmp) ? tmp : malloc(h->elem);
    if (!tp)
        return true; /* popped value already returned; heap order best-effort */
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < h->len && h->cmp(heap_at(h, l), heap_at(h, best)) < 0)
            best = l;
        if (r < h->len && h->cmp(heap_at(h, r), heap_at(h, best)) < 0)
            best = r;
        if (best == i)
            break;
        heap_swap(h, i, best, tp);
        i = best;
    }
    if (tp != tmp)
        free(tp);
    return true;
}

bool dsco_heap_peek(const dsco_heap *h, void *out) {
    if (!h || h->len == 0)
        return false;
    if (out)
        memcpy(out, h->data, h->elem);
    return true;
}

size_t dsco_heap_size(const dsco_heap *h) {
    return h ? h->len : 0;
}

void dsco_heap_free(dsco_heap *h) {
    if (!h)
        return;
    free(h->data);
    free(h);
}

/* ── dsco_bitset ────────────────────────────────────────────────────────── */

dsco_bitset *dsco_bitset_new(size_t nbits) {
    dsco_bitset *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    size_t nwords = (nbits + 63) / 64;
    if (nwords == 0)
        nwords = 1;
    b->words = calloc(nwords, sizeof(uint64_t));
    if (!b->words) {
        free(b);
        return NULL;
    }
    b->nbits = nbits;
    return b;
}

void dsco_bitset_free(dsco_bitset *b) {
    if (!b)
        return;
    free(b->words);
    free(b);
}

void dsco_bitset_set(dsco_bitset *b, size_t i) {
    if (b && i < b->nbits)
        b->words[i >> 6] |= (uint64_t)1 << (i & 63);
}

void dsco_bitset_clr(dsco_bitset *b, size_t i) {
    if (b && i < b->nbits)
        b->words[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

bool dsco_bitset_test(const dsco_bitset *b, size_t i) {
    if (!b || i >= b->nbits)
        return false;
    return (b->words[i >> 6] >> (i & 63)) & 1;
}

void dsco_bitset_set_all(dsco_bitset *b) {
    if (!b)
        return;
    size_t nwords = (b->nbits + 63) / 64;
    memset(b->words, 0xff, nwords * sizeof(uint64_t));
}

void dsco_bitset_clr_all(dsco_bitset *b) {
    if (!b)
        return;
    size_t nwords = (b->nbits + 63) / 64;
    memset(b->words, 0, nwords * sizeof(uint64_t));
}

size_t dsco_bitset_count(const dsco_bitset *b) {
    if (!b)
        return 0;
    size_t nwords = (b->nbits + 63) / 64, n = 0;
    for (size_t i = 0; i < nwords; i++)
        n += (size_t)__builtin_popcountll(b->words[i]);
    /* Mask any bits beyond nbits in the final word. */
    size_t rem = b->nbits & 63;
    if (rem) {
        uint64_t extra = b->words[nwords - 1] >> rem;
        n -= (size_t)__builtin_popcountll(extra);
    }
    return n;
}

/* ── dsco_queue ─────────────────────────────────────────────────────────── */

bool dsco_queue_init(dsco_queue *q, size_t elem_size, size_t capacity) {
    if (!q || !elem_size || !capacity)
        return false;
    q->data = malloc(elem_size * capacity);
    if (!q->data)
        return false;
    q->elem = elem_size;
    q->cap = capacity;
    q->head = 0;
    q->count = 0;
    return true;
}

void dsco_queue_free(dsco_queue *q) {
    if (!q)
        return;
    free(q->data);
    q->data = NULL;
    q->cap = q->count = q->head = 0;
}

bool dsco_queue_push(dsco_queue *q, const void *elem) {
    if (!q || q->count >= q->cap)
        return false;
    size_t idx = (q->head + q->count) % q->cap;
    memcpy(q->data + idx * q->elem, elem, q->elem);
    q->count++;
    return true;
}

bool dsco_queue_pop(dsco_queue *q, void *out) {
    if (!q || q->count == 0)
        return false;
    if (out)
        memcpy(out, q->data + q->head * q->elem, q->elem);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return true;
}

size_t dsco_queue_len(const dsco_queue *q) {
    return q ? q->count : 0;
}

bool dsco_queue_full(const dsco_queue *q) {
    return q && q->count >= q->cap;
}

bool dsco_queue_empty(const dsco_queue *q) {
    return !q || q->count == 0;
}

/* ── dsco_strpool ───────────────────────────────────────────────────────── */

typedef struct sp_node {
    char *str;
    int id;
    struct sp_node *next;
} sp_node;

struct dsco_strpool {
    sp_node **buckets;
    size_t nbuckets;
    char **byid;
    size_t nids, cap_ids;
};

dsco_strpool *dsco_strpool_new(void) {
    dsco_strpool *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->nbuckets = 64;
    p->buckets = calloc(p->nbuckets, sizeof(sp_node *));
    if (!p->buckets) {
        free(p);
        return NULL;
    }
    return p;
}

int dsco_strpool_find(const dsco_strpool *p, const char *s) {
    if (!p || !s)
        return -1;
    size_t b = fnv1a(s) & (p->nbuckets - 1);
    for (sp_node *n = p->buckets[b]; n; n = n->next)
        if (strcmp(n->str, s) == 0)
            return n->id;
    return -1;
}

int dsco_strpool_intern(dsco_strpool *p, const char *s) {
    if (!p || !s)
        return -1;
    size_t b = fnv1a(s) & (p->nbuckets - 1);
    for (sp_node *n = p->buckets[b]; n; n = n->next)
        if (strcmp(n->str, s) == 0)
            return n->id;
    /* grow id table if needed */
    if (p->nids + 1 > p->cap_ids) {
        size_t nc = p->cap_ids ? p->cap_ids * 2 : 16;
        char **nb = realloc(p->byid, nc * sizeof(char *));
        if (!nb)
            return -1;
        p->byid = nb;
        p->cap_ids = nc;
    }
    sp_node *n = malloc(sizeof(*n));
    if (!n)
        return -1;
    n->str = strdup(s);
    if (!n->str) {
        free(n);
        return -1;
    }
    n->id = (int)p->nids;
    n->next = p->buckets[b];
    p->buckets[b] = n;
    p->byid[p->nids] = n->str; /* shares the node's string; freed once in _free */
    p->nids++;
    return n->id;
}

const char *dsco_strpool_str(const dsco_strpool *p, int id) {
    if (!p || id < 0 || (size_t)id >= p->nids)
        return NULL;
    return p->byid[id];
}

size_t dsco_strpool_count(const dsco_strpool *p) {
    return p ? p->nids : 0;
}

void dsco_strpool_free(dsco_strpool *p) {
    if (!p)
        return;
    for (size_t i = 0; i < p->nbuckets; i++) {
        sp_node *n = p->buckets[i];
        while (n) {
            sp_node *next = n->next;
            free(n->str);
            free(n);
            n = next;
        }
    }
    free(p->buckets);
    free(p->byid);
    free(p);
}

/* ── shared hashing for tier-2 containers ───────────────────────────────────
 * A length-aware, seeded FNV-1a (the tier-1 fnv1a above is C-string only) and
 * a splitmix64 finalizer for integer keys. */
static uint64_t fnv1a_len(const void *data, size_t len, uint64_t seed) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* ── dsco_deque ─────────────────────────────────────────────────────────── */

bool dsco_deque_init(dsco_deque *d, size_t elem_size) {
    if (!d || !elem_size)
        return false;
    d->data = NULL;
    d->elem = elem_size;
    d->cap = 0;
    d->head = 0;
    d->count = 0;
    return true;
}

void dsco_deque_free(dsco_deque *d) {
    if (!d)
        return;
    free(d->data);
    d->data = NULL;
    d->cap = d->head = d->count = 0;
}

static bool deque_grow(dsco_deque *d) {
    size_t nc = d->cap ? d->cap * 2 : 8;
    unsigned char *nd = malloc(nc * d->elem);
    if (!nd)
        return false;
    /* Linearize: logical element i moves to slot i, head resets to 0. */
    for (size_t i = 0; i < d->count; i++) {
        size_t src = (d->head + i) % d->cap;
        memcpy(nd + i * d->elem, d->data + src * d->elem, d->elem);
    }
    free(d->data);
    d->data = nd;
    d->cap = nc;
    d->head = 0;
    return true;
}

bool dsco_deque_push_back(dsco_deque *d, const void *elem) {
    if (!d || !elem)
        return false;
    if (d->count == d->cap && !deque_grow(d))
        return false;
    size_t idx = (d->head + d->count) % d->cap;
    memcpy(d->data + idx * d->elem, elem, d->elem);
    d->count++;
    return true;
}

bool dsco_deque_push_front(dsco_deque *d, const void *elem) {
    if (!d || !elem)
        return false;
    if (d->count == d->cap && !deque_grow(d))
        return false;
    d->head = (d->head + d->cap - 1) % d->cap;
    memcpy(d->data + d->head * d->elem, elem, d->elem);
    d->count++;
    return true;
}

bool dsco_deque_pop_front(dsco_deque *d, void *out) {
    if (!d || d->count == 0)
        return false;
    if (out)
        memcpy(out, d->data + d->head * d->elem, d->elem);
    d->head = (d->head + 1) % d->cap;
    d->count--;
    return true;
}

bool dsco_deque_pop_back(dsco_deque *d, void *out) {
    if (!d || d->count == 0)
        return false;
    size_t idx = (d->head + d->count - 1) % d->cap;
    if (out)
        memcpy(out, d->data + idx * d->elem, d->elem);
    d->count--;
    return true;
}

void *dsco_deque_at(const dsco_deque *d, size_t i) {
    if (!d || i >= d->count)
        return NULL;
    return d->data + ((d->head + i) % d->cap) * d->elem;
}

size_t dsco_deque_len(const dsco_deque *d) {
    return d ? d->count : 0;
}

/* ── dsco_topk ──────────────────────────────────────────────────────────── */

typedef struct {
    double score;
    void *item;
} topk_entry;

struct dsco_topk {
    topk_entry *a; /* size-k min-heap: a[0] is the smallest retained score */
    size_t k;
    size_t n;
};

dsco_topk *dsco_topk_new(size_t k) {
    if (k == 0)
        k = 1;
    dsco_topk *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->a = malloc(k * sizeof(topk_entry));
    if (!t->a) {
        free(t);
        return NULL;
    }
    t->k = k;
    return t;
}

void dsco_topk_free(dsco_topk *t) {
    if (!t)
        return;
    free(t->a);
    free(t);
}

static void topk_sift_up(dsco_topk *t, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (t->a[i].score < t->a[p].score) {
            topk_entry tmp = t->a[i];
            t->a[i] = t->a[p];
            t->a[p] = tmp;
            i = p;
        } else
            break;
    }
}

static void topk_sift_down(dsco_topk *t, size_t i) {
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < t->n && t->a[l].score < t->a[s].score)
            s = l;
        if (r < t->n && t->a[r].score < t->a[s].score)
            s = r;
        if (s == i)
            break;
        topk_entry tmp = t->a[i];
        t->a[i] = t->a[s];
        t->a[s] = tmp;
        i = s;
    }
}

void dsco_topk_offer(dsco_topk *t, double score, void *item) {
    if (!t)
        return;
    if (t->n < t->k) {
        t->a[t->n].score = score;
        t->a[t->n].item = item;
        topk_sift_up(t, t->n);
        t->n++;
    } else if (score > t->a[0].score) {
        t->a[0].score = score;
        t->a[0].item = item;
        topk_sift_down(t, 0);
    }
}

size_t dsco_topk_count(const dsco_topk *t) {
    return t ? t->n : 0;
}

static int topk_cmp_desc(const void *x, const void *y) {
    double a = ((const topk_entry *)x)->score, b = ((const topk_entry *)y)->score;
    return (a < b) - (a > b); /* descending */
}

size_t dsco_topk_get(const dsco_topk *t, void **out, double *scores, size_t cap) {
    if (!t || t->n == 0 || cap == 0)
        return 0;
    topk_entry *tmp = malloc(t->n * sizeof(topk_entry));
    if (!tmp)
        return 0;
    memcpy(tmp, t->a, t->n * sizeof(topk_entry));
    qsort(tmp, t->n, sizeof(topk_entry), topk_cmp_desc);
    size_t w = t->n < cap ? t->n : cap;
    for (size_t i = 0; i < w; i++) {
        if (out)
            out[i] = tmp[i].item;
        if (scores)
            scores[i] = tmp[i].score;
    }
    free(tmp);
    return w;
}

/* ── dsco_bloom ─────────────────────────────────────────────────────────── */

struct dsco_bloom {
    uint64_t *words;
    size_t nbits; /* m */
    size_t nwords;
    unsigned k; /* # hash probes */
};

dsco_bloom *dsco_bloom_new(size_t expected_items, double fp_rate) {
    if (expected_items == 0)
        expected_items = 1;
    if (!(fp_rate > 0.0) || fp_rate >= 1.0)
        fp_rate = 0.01;
    /* m = -n ln(p) / (ln2)^2 ; k = round((m/n) ln2). */
    double ln2 = 0.6931471805599453;
    double m = -((double)expected_items) * log(fp_rate) / (ln2 * ln2);
    size_t nbits = (size_t)m;
    if (nbits < 64)
        nbits = 64;
    unsigned k = (unsigned)(((double)nbits / (double)expected_items) * ln2 + 0.5);
    if (k < 1)
        k = 1;
    if (k > 16)
        k = 16;
    dsco_bloom *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->nbits = nbits;
    b->nwords = (nbits + 63) / 64;
    b->k = k;
    b->words = calloc(b->nwords, sizeof(uint64_t));
    if (!b->words) {
        free(b);
        return NULL;
    }
    return b;
}

void dsco_bloom_free(dsco_bloom *b) {
    if (!b)
        return;
    free(b->words);
    free(b);
}

/* Kirsch–Mitzenmacher double hashing: probe_i = h1 + i*h2 (mod m). */
static void bloom_probe(const void *data, size_t len, uint64_t *h1, uint64_t *h2) {
    *h1 = fnv1a_len(data, len, 0);
    *h2 = fnv1a_len(data, len, 0x9e3779b97f4a7c15ULL) | 1ULL; /* nonzero stride */
}

void dsco_bloom_add(dsco_bloom *b, const void *data, size_t len) {
    if (!b)
        return;
    uint64_t h1, h2;
    bloom_probe(data, len, &h1, &h2);
    for (unsigned i = 0; i < b->k; i++) {
        size_t bit = (size_t)((h1 + (uint64_t)i * h2) % b->nbits);
        b->words[bit >> 6] |= (uint64_t)1 << (bit & 63);
    }
}

bool dsco_bloom_maybe(const dsco_bloom *b, const void *data, size_t len) {
    if (!b)
        return false;
    uint64_t h1, h2;
    bloom_probe(data, len, &h1, &h2);
    for (unsigned i = 0; i < b->k; i++) {
        size_t bit = (size_t)((h1 + (uint64_t)i * h2) % b->nbits);
        if (!((b->words[bit >> 6] >> (bit & 63)) & 1))
            return false;
    }
    return true;
}

void dsco_bloom_add_str(dsco_bloom *b, const char *s) {
    if (s)
        dsco_bloom_add(b, s, strlen(s));
}

bool dsco_bloom_maybe_str(const dsco_bloom *b, const char *s) {
    return s ? dsco_bloom_maybe(b, s, strlen(s)) : false;
}

size_t dsco_bloom_bits(const dsco_bloom *b) {
    return b ? b->nbits : 0;
}

/* ── dsco_intmap ────────────────────────────────────────────────────────── */

enum { IM_EMPTY = 0, IM_FULL = 1, IM_DEAD = 2 };

typedef struct {
    uint64_t key;
    void *val;
    unsigned char state;
} im_slot;

struct dsco_intmap {
    im_slot *slots;
    size_t cap;  /* power of two */
    size_t size; /* live entries */
    size_t used; /* live + tombstones (probe-sequence occupancy) */
};

dsco_intmap *dsco_intmap_new(void) {
    dsco_intmap *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cap = 16;
    m->slots = calloc(m->cap, sizeof(im_slot));
    if (!m->slots) {
        free(m);
        return NULL;
    }
    return m;
}

void dsco_intmap_free(dsco_intmap *m) {
    if (!m)
        return;
    free(m->slots);
    free(m);
}

static bool intmap_resize(dsco_intmap *m, size_t ncap) {
    im_slot *ns = calloc(ncap, sizeof(im_slot));
    if (!ns)
        return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].state != IM_FULL)
            continue;
        size_t j = mix64(m->slots[i].key) & (ncap - 1);
        while (ns[j].state == IM_FULL)
            j = (j + 1) & (ncap - 1);
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    m->used = m->size; /* tombstones cleared */
    return true;
}

bool dsco_intmap_put(dsco_intmap *m, uint64_t key, void *val) {
    if (!m)
        return false;
    if ((m->used + 1) * 10 >= m->cap * 7) {
        if (!intmap_resize(m, m->cap * 2))
            return false;
    }
    size_t j = mix64(key) & (m->cap - 1);
    long first_dead = -1;
    for (;;) {
        im_slot *s = &m->slots[j];
        if (s->state == IM_EMPTY) {
            if (first_dead >= 0)
                s = &m->slots[first_dead]; /* reuse a tombstone, don't grow `used` */
            else
                m->used++;
            s->key = key;
            s->val = val;
            s->state = IM_FULL;
            m->size++;
            return true;
        }
        if (s->state == IM_DEAD) {
            if (first_dead < 0)
                first_dead = (long)j;
        } else if (s->key == key) {
            s->val = val; /* replace */
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
}

static im_slot *intmap_find(const dsco_intmap *m, uint64_t key) {
    size_t j = mix64(key) & (m->cap - 1);
    for (;;) {
        im_slot *s = &m->slots[j];
        if (s->state == IM_EMPTY)
            return NULL;
        if (s->state == IM_FULL && s->key == key)
            return s;
        j = (j + 1) & (m->cap - 1);
    }
}

void *dsco_intmap_get(const dsco_intmap *m, uint64_t key) {
    if (!m)
        return NULL;
    im_slot *s = intmap_find(m, key);
    return s ? s->val : NULL;
}

bool dsco_intmap_has(const dsco_intmap *m, uint64_t key) {
    return m && intmap_find(m, key) != NULL;
}

bool dsco_intmap_del(dsco_intmap *m, uint64_t key) {
    if (!m)
        return false;
    im_slot *s = intmap_find(m, key);
    if (!s)
        return false;
    s->state = IM_DEAD; /* leave `used` — probe chains stay intact */
    s->val = NULL;
    m->size--;
    return true;
}

size_t dsco_intmap_size(const dsco_intmap *m) {
    return m ? m->size : 0;
}

/* ── dsco_ringbuf ───────────────────────────────────────────────────────── */

bool dsco_ringbuf_init(dsco_ringbuf *r, size_t cap) {
    if (!r || cap == 0)
        return false;
    r->buf = malloc(cap);
    if (!r->buf)
        return false;
    r->cap = cap;
    r->head = 0;
    r->count = 0;
    return true;
}

void dsco_ringbuf_free(dsco_ringbuf *r) {
    if (!r)
        return;
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->count = 0;
}

size_t dsco_ringbuf_write(dsco_ringbuf *r, const void *data, size_t len) {
    if (!r || !data)
        return 0;
    size_t space = r->cap - r->count;
    if (len > space)
        len = space;
    size_t tail = (r->head + r->count) % r->cap;
    size_t first = r->cap - tail;
    if (first > len)
        first = len;
    memcpy(r->buf + tail, data, first);
    if (len > first)
        memcpy(r->buf, (const unsigned char *)data + first, len - first);
    r->count += len;
    return len;
}

size_t dsco_ringbuf_peek(const dsco_ringbuf *r, void *out, size_t max) {
    if (!r || !out)
        return 0;
    if (max > r->count)
        max = r->count;
    size_t first = r->cap - r->head;
    if (first > max)
        first = max;
    memcpy(out, r->buf + r->head, first);
    if (max > first)
        memcpy((unsigned char *)out + first, r->buf, max - first);
    return max;
}

size_t dsco_ringbuf_read(dsco_ringbuf *r, void *out, size_t max) {
    if (!r)
        return 0;
    size_t got = dsco_ringbuf_peek(r, out, max);
    r->head = (r->head + got) % r->cap;
    r->count -= got;
    return got;
}

size_t dsco_ringbuf_len(const dsco_ringbuf *r) {
    return r ? r->count : 0;
}

size_t dsco_ringbuf_avail(const dsco_ringbuf *r) {
    return r ? r->cap - r->count : 0;
}

/* ── dsco_uf ────────────────────────────────────────────────────────────── */

struct dsco_uf {
    size_t *parent;
    unsigned char *rank;
    size_t n;
    size_t sets;
};

dsco_uf *dsco_uf_new(size_t n) {
    if (n == 0)
        n = 1;
    dsco_uf *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->parent = malloc(n * sizeof(size_t));
    u->rank = calloc(n, sizeof(unsigned char));
    if (!u->parent || !u->rank) {
        free(u->parent);
        free(u->rank);
        free(u);
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
        u->parent[i] = i;
    u->n = n;
    u->sets = n;
    return u;
}

void dsco_uf_free(dsco_uf *u) {
    if (!u)
        return;
    free(u->parent);
    free(u->rank);
    free(u);
}

size_t dsco_uf_find(dsco_uf *u, size_t x) {
    if (!u || x >= u->n)
        return (size_t)-1;
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]]; /* path halving */
        x = u->parent[x];
    }
    return x;
}

bool dsco_uf_union(dsco_uf *u, size_t a, size_t b) {
    if (!u || a >= u->n || b >= u->n)
        return false;
    size_t ra = dsco_uf_find(u, a), rb = dsco_uf_find(u, b);
    if (ra == rb)
        return false;
    if (u->rank[ra] < u->rank[rb]) {
        size_t t = ra;
        ra = rb;
        rb = t;
    }
    u->parent[rb] = ra;
    if (u->rank[ra] == u->rank[rb])
        u->rank[ra]++;
    u->sets--;
    return true;
}

bool dsco_uf_connected(dsco_uf *u, size_t a, size_t b) {
    if (!u || a >= u->n || b >= u->n)
        return false;
    return dsco_uf_find(u, a) == dsco_uf_find(u, b);
}

size_t dsco_uf_count(const dsco_uf *u) {
    return u ? u->sets : 0;
}

/* ── dsco_slab ──────────────────────────────────────────────────────────── */

struct dsco_slab {
    unsigned char *data;
    uint64_t *live;   /* bitset: slot occupied */
    size_t *freelist; /* stack of free indices */
    size_t free_top;  /* # entries on freelist */
    size_t elem;
    size_t cap;
    size_t used;
};

dsco_slab *dsco_slab_new(size_t elem_size, size_t capacity) {
    if (!elem_size || !capacity)
        return NULL;
    dsco_slab *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    size_t nwords = (capacity + 63) / 64;
    s->data = calloc(capacity, elem_size);
    s->live = calloc(nwords, sizeof(uint64_t));
    s->freelist = malloc(capacity * sizeof(size_t));
    if (!s->data || !s->live || !s->freelist) {
        free(s->data);
        free(s->live);
        free(s->freelist);
        free(s);
        return NULL;
    }
    /* Seed freelist high→low so the first alloc hands out index 0. */
    for (size_t i = 0; i < capacity; i++)
        s->freelist[i] = capacity - 1 - i;
    s->free_top = capacity;
    s->elem = elem_size;
    s->cap = capacity;
    return s;
}

void dsco_slab_free(dsco_slab *s) {
    if (!s)
        return;
    free(s->data);
    free(s->live);
    free(s->freelist);
    free(s);
}

long dsco_slab_alloc(dsco_slab *s, void **out) {
    if (!s || s->free_top == 0)
        return -1;
    size_t idx = s->freelist[--s->free_top];
    s->live[idx >> 6] |= (uint64_t)1 << (idx & 63);
    void *p = s->data + idx * s->elem;
    memset(p, 0, s->elem);
    s->used++;
    if (out)
        *out = p;
    return (long)idx;
}

void dsco_slab_release(dsco_slab *s, size_t idx) {
    if (!s || idx >= s->cap)
        return;
    if (!((s->live[idx >> 6] >> (idx & 63)) & 1))
        return; /* not live — ignore double release */
    s->live[idx >> 6] &= ~((uint64_t)1 << (idx & 63));
    s->freelist[s->free_top++] = idx;
    s->used--;
}

bool dsco_slab_live(const dsco_slab *s, size_t idx) {
    if (!s || idx >= s->cap)
        return false;
    return (s->live[idx >> 6] >> (idx & 63)) & 1;
}

void *dsco_slab_at(const dsco_slab *s, size_t idx) {
    if (!dsco_slab_live(s, idx))
        return NULL;
    return s->data + idx * s->elem;
}

size_t dsco_slab_used(const dsco_slab *s) {
    return s ? s->used : 0;
}

size_t dsco_slab_cap(const dsco_slab *s) {
    return s ? s->cap : 0;
}
