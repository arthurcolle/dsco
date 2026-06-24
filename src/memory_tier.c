#include "memory_tier.h"
#include "error.h"
#include "vecstore.h"
#include "tools.h"
#include "vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* §8: VFS persistence handle — set by memory_store_set_vfs() */
static vfs_db_t *g_mem_vfs = NULL;

/* §9: Vecstore handle for embedding-backed search — set by memory_store_set_vecstore() */
static vecstore_t *g_mem_vecstore = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 * Three-Tier Agent Memory System — Implementation
 *
 * Working (60s) → Episodic (3600s) → Semantic (permanent)
 * Automatic decay + consolidation for cognitive memory model.
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *TIER_NAMES[] = {"working", "episodic", "semantic"};

const char *memory_tier_name(memory_tier_t t) {
    return (t >= 0 && t < MEM_TIER_COUNT) ? TIER_NAMES[t] : "unknown";
}

double memory_tier_halflife(memory_tier_t t) {
    switch (t) {
        case MEM_WORKING:
            return MEM_WORKING_HALFLIFE;
        case MEM_EPISODIC:
            return MEM_EPISODIC_HALFLIFE;
        case MEM_SEMANTIC:
            return MEM_SEMANTIC_HALFLIFE;
        default:
            return 0;
    }
}

/* ── Decay Calculation ────────────────────────────────────────────────── */

double memory_calc_strength(memory_tier_t tier, double created_at, double now) {
    double halflife = memory_tier_halflife(tier);
    if (halflife <= 0)
        return 1.0; /* semantic = no decay */

    double age = now - created_at;
    if (age <= 0)
        return 1.0;

    /* Exponential decay: strength = 0.5^(age/halflife) = exp(-ln2 * age / halflife) */
    return exp(-0.693147 * age / halflife);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void memory_store_init(memory_store_t *m) {
    memset(m, 0, sizeof(*m));
    m->initialized = true;
}

void memory_store_destroy(memory_store_t *m) {
    memset(m, 0, sizeof(*m));
}

/* ── Store ────────────────────────────────────────────────────────────── */

static int find_slot(memory_store_t *m) {
    /* Find inactive slot */
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        if (!m->entries[i].active)
            return i;
    }
    /* If full, evict weakest entry */
    double weakest = 2.0;
    int weakest_idx = 0;
    double t = now_sec();
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        if (m->entries[i].pinned)
            continue;
        double s = memory_calc_strength(m->entries[i].tier, m->entries[i].created_at, t);
        if (s < weakest) {
            weakest = s;
            weakest_idx = i;
        }
    }
    m->entries[weakest_idx].active = false;
    m->tier_count[m->entries[weakest_idx].tier]--;
    m->count--;
    m->total_evictions++;
    return weakest_idx;
}

static memory_entry_t *find_by_key(memory_store_t *m, const char *key) {
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        if (m->entries[i].active && strcmp(m->entries[i].key, key) == 0)
            return &m->entries[i];
    }
    return NULL;
}

int memory_store(memory_store_t *m, memory_tier_t tier, const char *key, const char *value,
                 double importance) {
    return memory_store_tagged(m, tier, key, value, importance, NULL, 0);
}

int memory_store_tagged(memory_store_t *m, memory_tier_t tier, const char *key, const char *value,
                        double importance, const char **tags, int tag_count) {
    if (!m || !m->initialized || !key || !value)
        return -1;
    /* DIGESTIVE CONTRACT: tier must be a real tier; importance is a probability
     * in [0,1] — an out-of-range importance corrupts the consolidation gate
     * (promote at >=0.7) and the decay model. Fail closed. */
    DSCO_REQUIRE_RET(tier >= 0 && tier < MEM_TIER_COUNT, -1);
    DSCO_REQUIRE_RET(importance >= 0.0 && importance <= 1.0, -1);
    DSCO_REQUIRE_RET(tag_count >= 0, -1);

    /* Check if key already exists — update instead */
    memory_entry_t *existing = find_by_key(m, key);
    if (existing) {
        snprintf(existing->value, sizeof(existing->value), "%s", value);
        existing->importance = importance;
        existing->last_accessed = now_sec();
        existing->access_count++;
        return existing->id;
    }

    int slot = find_slot(m);
    /* CONSERVATION: find_slot returns -1 when full; never index with it. */
    DSCO_REQUIRE_RET(slot >= 0 && slot < MEMTIER_MAX_ENTRIES, -1);
    memory_entry_t *e = &m->entries[slot];
    memset(e, 0, sizeof(*e));

    e->id = m->next_id++;
    e->tier = tier;
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
    e->importance = importance;
    e->strength = 1.0;
    e->created_at = now_sec();
    e->last_accessed = e->created_at;
    e->access_count = 1;
    e->active = true;

    if (tags) {
        int tc = tag_count < MEMTIER_MAX_TAGS ? tag_count : MEMTIER_MAX_TAGS;
        for (int i = 0; i < tc; i++) {
            snprintf(e->tags[i], sizeof(e->tags[i]), "%s", tags[i]);
        }
        e->tag_count = tc;
    }

    m->count++;
    m->tier_count[tier]++;
    m->total_stores++;

    /* Auto-embed for episodic/semantic tiers if vecstore is wired up */
    if (g_mem_vecstore && tier != MEM_WORKING && value[0]) {
        int dim = 0;
        float *vec = tools_embed_text(value, &dim);
        if (vec && dim > 0) {
            vecstore_insert(g_mem_vecstore, key, vec, dim, NULL);
            e->has_embedding = true;
            free(vec);
        }
    }

    return e->id;
}

/* ── Recall ───────────────────────────────────────────────────────────── */

const memory_entry_t *memory_recall(memory_store_t *m, const char *key) {
    if (!m || !m->initialized || !key)
        return NULL;
    m->total_recalls++;

    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return NULL;

    e->last_accessed = now_sec();
    e->access_count++;
    e->strength = memory_calc_strength(e->tier, e->created_at, now_sec());
    return e;
}

int memory_recall_by_tag(memory_store_t *m, const char *tag, const memory_entry_t **out, int max) {
    if (!m || !m->initialized || !tag || !out)
        return 0;
    int count = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES && count < max; i++) {
        if (!m->entries[i].active)
            continue;
        for (int t = 0; t < m->entries[i].tag_count; t++) {
            if (strcmp(m->entries[i].tags[t], tag) == 0) {
                out[count++] = &m->entries[i];
                break;
            }
        }
    }
    return count;
}

int memory_recall_tier(memory_store_t *m, memory_tier_t tier, const memory_entry_t **out, int max) {
    if (!m || !m->initialized || !out)
        return 0;
    /* DIGESTIVE CONTRACT: tier must be a real tier; max must be non-negative or
     * the bounded copy loop below would underflow. */
    DSCO_REQUIRE_RET(tier >= 0 && tier < MEM_TIER_COUNT, 0);
    DSCO_REQUIRE_RET(max >= 0, 0);
    int count = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES && count < max; i++) {
        if (m->entries[i].active && m->entries[i].tier == tier)
            out[count++] = &m->entries[i];
    }
    return count;
}

int memory_search(memory_store_t *m, const char *query, const memory_entry_t **out, int max) {
    if (!m || !m->initialized || !query || !out)
        return 0;
    int count = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES && count < max; i++) {
        if (!m->entries[i].active)
            continue;
        if (strstr(m->entries[i].key, query) || strstr(m->entries[i].value, query)) {
            out[count++] = &m->entries[i];
        }
    }
    return count;
}

/* ── Modification ─────────────────────────────────────────────────────── */

bool memory_update(memory_store_t *m, const char *key, const char *value) {
    if (!m || !m->initialized || !key || !value)
        return false;
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    snprintf(e->value, sizeof(e->value), "%s", value);
    e->last_accessed = now_sec();
    return true;
}

bool memory_forget(memory_store_t *m, const char *key) {
    if (!m || !m->initialized || !key)
        return false;
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    m->tier_count[e->tier]--;
    m->count--;
    e->active = false;
    return true;
}

bool memory_pin(memory_store_t *m, const char *key) {
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    e->pinned = true;
    return true;
}

bool memory_unpin(memory_store_t *m, const char *key) {
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    e->pinned = false;
    return true;
}

bool memory_promote(memory_store_t *m, const char *key) {
    if (!m || !m->initialized || !key)
        return false;
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    if (e->tier >= MEM_SEMANTIC)
        return false; /* already at top */

    m->tier_count[e->tier]--;
    e->tier++;
    m->tier_count[e->tier]++;
    e->created_at = now_sec(); /* reset decay clock */
    e->strength = 1.0;
    m->total_promotions++;
    return true;
}

/* ── Decay & Consolidation ────────────────────────────────────────────── */

int memory_decay_tick(memory_store_t *m, double threshold) {
    if (!m || !m->initialized)
        return 0;
    double t = now_sec();
    int evicted = 0;

    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        memory_entry_t *e = &m->entries[i];
        if (!e->active || e->pinned)
            continue;

        e->strength = memory_calc_strength(e->tier, e->created_at, t);
        if (e->strength < threshold) {
            m->tier_count[e->tier]--;
            m->count--;
            e->active = false;
            m->total_evictions++;
            evicted++;
        }
    }
    return evicted;
}

int memory_consolidate(memory_store_t *m) {
    if (!m || !m->initialized)
        return 0;
    double t = now_sec();

    /* Only consolidate periodically */
    if (t - m->last_consolidation < MEM_CONSOLIDATION_INTERVAL)
        return 0;
    m->last_consolidation = t;
    m->total_consolidations++;

    int promotions = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        memory_entry_t *e = &m->entries[i];
        if (!e->active || e->tier >= MEM_SEMANTIC)
            continue;

        /* Promote if frequently accessed or high importance */
        bool should_promote = false;

        if (e->access_count >= MEM_CONSOLIDATION_ACCESS_COUNT && e->tier == MEM_WORKING) {
            should_promote = true;
        }
        if (e->importance >= MEM_CONSOLIDATION_IMPORTANCE && e->tier < MEM_SEMANTIC) {
            should_promote = true;
        }
        /* Episodic → Semantic: need more evidence */
        if (e->tier == MEM_EPISODIC && e->access_count >= MEM_CONSOLIDATION_ACCESS_COUNT * 2 &&
            e->importance >= MEM_CONSOLIDATION_IMPORTANCE) {
            should_promote = true;
        }

        if (should_promote) {
            m->tier_count[e->tier]--;
            e->tier++;
            m->tier_count[e->tier]++;
            e->created_at = t; /* reset decay */
            e->strength = 1.0;
            promotions++;
            m->total_promotions++;
        }
    }
    return promotions;
}

int memory_tick(memory_store_t *m) {
    int evicted = memory_decay_tick(m, 0.01);
    int promoted = memory_consolidate(m);
    return evicted + promoted;
}

/* ── Serialization ────────────────────────────────────────────────────── */

int memory_status_json(const memory_store_t *m, char *buf, size_t len) {
    if (!m || !buf)
        return 0;
    return snprintf(buf, len,
                    "{\"total_entries\":%d,"
                    "\"working\":%d,\"episodic\":%d,\"semantic\":%d,"
                    "\"total_stores\":%d,\"total_recalls\":%d,"
                    "\"total_promotions\":%d,\"total_evictions\":%d,"
                    "\"total_consolidations\":%d}",
                    m->count, m->tier_count[MEM_WORKING], m->tier_count[MEM_EPISODIC],
                    m->tier_count[MEM_SEMANTIC], m->total_stores, m->total_recalls,
                    m->total_promotions, m->total_evictions, m->total_consolidations);
}

int memory_to_json(const memory_store_t *m, char *buf, size_t len) {
    if (!m || !buf)
        return 0;
    double t = now_sec();
    int n = snprintf(buf, len, "{\"entries\":[");

    bool first = true;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES && (size_t)n < len - 512; i++) {
        const memory_entry_t *e = &m->entries[i];
        if (!e->active)
            continue;

        double strength = memory_calc_strength(e->tier, e->created_at, t);
        n += snprintf(buf + n, len - n,
                      "%s{\"id\":%d,\"tier\":\"%s\",\"key\":\"%s\","
                      "\"importance\":%.2f,\"strength\":%.4f,"
                      "\"access_count\":%d,\"pinned\":%s}",
                      first ? "" : ",", e->id, memory_tier_name(e->tier), e->key, e->importance,
                      strength, e->access_count, e->pinned ? "true" : "false");
        first = false;
    }
    n += snprintf(buf + n, len - n, "]}");
    return n;
}

int memory_tier_to_json(const memory_store_t *m, memory_tier_t tier, char *buf, size_t len) {
    if (!m || !buf)
        return 0;
    double t = now_sec();
    int n = snprintf(buf, len, "{\"tier\":\"%s\",\"entries\":[", memory_tier_name(tier));

    bool first = true;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES && (size_t)n < len - 512; i++) {
        const memory_entry_t *e = &m->entries[i];
        if (!e->active || e->tier != tier)
            continue;

        double strength = memory_calc_strength(e->tier, e->created_at, t);
        n +=
            snprintf(buf + n, len - n,
                     "%s{\"key\":\"%s\",\"value\":\"%.*s\",\"strength\":%.4f,"
                     "\"importance\":%.2f,\"accesses\":%d}",
                     first ? "" : ",", e->key, (int)(strlen(e->value) > 80 ? 80 : strlen(e->value)),
                     e->value, strength, e->importance, e->access_count);
        first = false;
    }
    n += snprintf(buf + n, len - n, "]}");
    return n;
}

/* ── §8: VFS Persistence for Semantic Memories ─────────────────────── */

void memory_store_set_vfs(vfs_db_t *vfs) {
    g_mem_vfs = vfs;
}

void memory_persist_semantic(memory_store_t *m) {
    if (!m || !m->initialized || !g_mem_vfs)
        return;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        memory_entry_t *e = &m->entries[i];
        if (!e->active || e->tier != MEM_SEMANTIC)
            continue;
        char val[2048];
        snprintf(val, sizeof(val),
                 "{\"value\":\"%.*s\",\"importance\":%.2f,"
                 "\"access_count\":%d,\"pinned\":%s}",
                 (int)(strlen(e->value) > 1500 ? 1500 : strlen(e->value)), e->value, e->importance,
                 e->access_count, e->pinned ? "true" : "false");
        vfs_kv_put_str(g_mem_vfs, "semantic_memory", e->key, val);
    }
}

int memory_restore_semantic(memory_store_t *m) {
    if (!m || !m->initialized || !g_mem_vfs)
        return 0;
    int count = 0;
    char **keys = vfs_kv_keys(g_mem_vfs, "semantic_memory", &count);
    if (!keys || count == 0)
        return 0;

    int restored = 0;
    for (int i = 0; i < count; i++) {
        if (find_by_key(m, keys[i])) {
            free(keys[i]);
            continue;
        }
        char *val = vfs_kv_get_str(g_mem_vfs, "semantic_memory", keys[i]);
        if (val) {
            memory_store(m, MEM_SEMANTIC, keys[i], val, 0.8);
            restored++;
            free(val);
        }
        free(keys[i]);
    }
    free(keys);
    return restored;
}

/* ── §9: Embedding-backed Semantic Search ──────────────────────────── */

void memory_store_set_vecstore(struct vecstore *vs) {
    g_mem_vecstore = (vecstore_t *)vs;
}

bool memory_entry_set_embedding(memory_store_t *m, const char *key, const float *vec, int dim) {
    if (!m || !key || !vec || dim <= 0 || !g_mem_vecstore)
        return false;
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;

    if (vecstore_insert(g_mem_vecstore, key, vec, dim, NULL)) {
        e->has_embedding = true;
        return true;
    }
    return false;
}

int memory_search_semantic(memory_store_t *m, const char *query, const memory_entry_t **out,
                           int max) {
    if (!m || !m->initialized || !query || !out || max <= 0)
        return 0;

    /* Try embedding-based search first */
    if (g_mem_vecstore && vecstore_count(g_mem_vecstore) > 0) {
        int dim = 0;
        float *qvec = tools_embed_text(query, &dim);
        if (qvec && dim > 0) {
            int count = 0;
            vecstore_result_t *results = calloc((size_t)max, sizeof(vecstore_result_t));
            if (results) {
                int found = vecstore_query(g_mem_vecstore, qvec, dim, results, max);

                for (int i = 0; i < found && count < max; i++) {
                    memory_entry_t *e = find_by_key(m, results[i].id);
                    if (e && e->active) {
                        e->last_accessed = now_sec();
                        e->access_count++;
                        e->strength = memory_calc_strength(e->tier, e->created_at, now_sec());
                        out[count++] = e;
                    }
                }

                vecstore_result_free(results, found);
                free(results);
            }
            free(qvec);
            if (count > 0)
                return count;
            /* Fall through to substring if no semantic matches */
        }
    }

    /* Fallback: substring search */
    return memory_search(m, query, out, max);
}
