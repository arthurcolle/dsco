#include "memory_tier.h"
#include "error.h"
#include "json_util.h"
#include "vecstore.h"
#include "tools.h"
#include "vfs.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static bool memory_class_valid(memory_class_t level);
static bool memory_class_allows_manual_promotion(const memory_entry_t *e);

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *TIER_NAMES[] = {"working", "episodic", "semantic"};
static const char *CLASS_NAMES[] = {"open", "held", "sealed", "umbral"};

const char *memory_tier_name(memory_tier_t t) {
    return (t >= 0 && t < MEM_TIER_COUNT) ? TIER_NAMES[t] : "unknown";
}

const char *memory_class_name(memory_class_t c) {
    return memory_class_valid(c) ? CLASS_NAMES[c] : "unknown";
}

bool memory_class_from_name(const char *s, memory_class_t *out) {
    if (!s || !out)
        return false;
    if (strcasecmp(s, "open") == 0 || strcasecmp(s, "l0") == 0 || strcmp(s, "0") == 0)
        *out = MEM_CLASS_OPEN;
    else if (strcasecmp(s, "held") == 0 || strcasecmp(s, "l1") == 0 || strcmp(s, "1") == 0)
        *out = MEM_CLASS_HELD;
    else if (strcasecmp(s, "sealed") == 0 || strcasecmp(s, "l2") == 0 || strcmp(s, "2") == 0)
        *out = MEM_CLASS_SEALED;
    else if (strcasecmp(s, "umbral") == 0 || strcasecmp(s, "l3") == 0 || strcmp(s, "3") == 0)
        *out = MEM_CLASS_UMBRAL;
    else
        return false;
    return true;
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
    int weakest_idx = -1;
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
    if (weakest_idx < 0)
        return -1;
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
        if (existing->classification >= MEM_CLASS_SEALED)
            existing->class_reviewed = false;
        return existing->id;
    }

    int slot = find_slot(m);
    /* CONSERVATION: find_slot returns -1 when full of pinned entries; never
     * index with it. */
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
    /* CLASSIFICATION gate (doctrine/CLASSIFICATION.md §5): SEALED material may
     * not cross into semantic memory, and UMBRAL may not leave working memory,
     * without an explicit review grant. Fail closed. */
    if (!memory_class_allows_manual_promotion(e))
        return false;
    bool consume_review = (e->classification == MEM_CLASS_UMBRAL) ||
                          (e->classification == MEM_CLASS_SEALED &&
                           e->tier == MEM_EPISODIC);
    /* CONSERVATION: both the source and destination tier indices must be in
     * range before we mutate the per-tier counters, or we corrupt the histogram
     * that drives consolidation decisions. */
    DSCO_REQUIRE(e->tier >= 0 && e->tier < MEM_TIER_COUNT - 1);

    m->tier_count[e->tier]--;
    e->tier++;
    m->tier_count[e->tier]++;
    e->created_at = now_sec(); /* reset decay clock */
    e->strength = 1.0;
    if (consume_review)
        e->class_reviewed = false; /* review grants exactly one gated promotion */
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

        /* Principled promotion (doctrine/MEMORY.md Rule 0): rank by the computable
         * keep_score in PROMOTION mode (importance dominates so an old, rarely-
         * re-accessed failure-lesson still survives) instead of a boolean OR of
         * thresholds. Relevance has no live query at sweep time, so it is neutral
         * (0.5). access_count acts as an evidence multiplier: a trace seen many
         * times is more believable (Dalio: track record). */
        bool should_promote = false;

        double base = memory_keep_score(e, MEM_KEEP_PROMOTION, 0.5, t);
        /* Evidence bonus: +0.05 per access, capped, so repeatedly-confirmed
         * traces clear the bar faster without letting noise alone promote. */
        double evidence = e->access_count * 0.05;
        if (evidence > 0.15) evidence = 0.15;
        double rank = base + evidence;
        if (rank > 1.0) rank = 1.0;

        /* Tier-graduated gate: episodic -> semantic is the durable commitment and
         * demands more evidence than working -> episodic (preserves the original
         * "more evidence for semantic" discipline, now as a threshold on rank). */
        double gate = (e->tier == MEM_EPISODIC)
                        ? MEM_KEEP_PROMOTE_GATE_SEMANTIC
                        : MEM_KEEP_PROMOTE_GATE_EPISODIC;
        if (rank >= gate)
            should_promote = true;

        /* CLASSIFICATION gate (doctrine/CLASSIFICATION.md §5): the sweep never
         * auto-promotes SEALED into semantic or UMBRAL out of working. A secret
         * does not enter permanent memory because it was accessed often —
         * that is the consolidation-leak failure mode, verbatim. */
        if (should_promote && !memory_class_promotable(e))
            should_promote = false;

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

/* ── Computable keep/promote score (doctrine/MEMORY.md Rule 0) ─────────────
 * score = w_r*recency + w_i*importance + w_v*relevance, clamped to [0,1].
 * PROMOTION weights importance heavily so an old failure-lesson still ranks;
 * RETRIEVAL weights recency. Grounded in Generative Agents + MemGPT, and in a
 * real test failure: a 4-day-old high-importance trace must not be discarded
 * just because recency decayed to 0. Additive; memory_consolidate() unchanged. */
double memory_keep_score(const memory_entry_t *e, memory_keep_mode_t mode,
                         double relevance, double now) {
    if (!e)
        return 0.0;

    double w_r, w_i, w_v;
    if (mode == MEM_KEEP_RETRIEVAL) {
        w_r = MEM_KEEP_RETRIEVAL_W_RECENCY;
        w_i = MEM_KEEP_RETRIEVAL_W_IMPORTANCE;
        w_v = MEM_KEEP_RETRIEVAL_W_RELEVANCE;
    } else {
        w_r = MEM_KEEP_PROMOTION_W_RECENCY;
        w_i = MEM_KEEP_PROMOTION_W_IMPORTANCE;
        w_v = MEM_KEEP_PROMOTION_W_RELEVANCE;
    }

    /* Recency on the episodic 1h half-life (matches keep_score.py). */
    double age = now - e->created_at;
    double recency = (age <= 0) ? 1.0 : exp(-0.693147 * age / MEM_EPISODIC_HALFLIFE);

    double imp = e->importance;
    if (imp < 0.0) imp = 0.0; else if (imp > 1.0) imp = 1.0;
    if (relevance < 0.0) relevance = 0.0; else if (relevance > 1.0) relevance = 1.0;

    double score = w_r * recency + w_i * imp + w_v * relevance;
    if (score < 0.0) score = 0.0; else if (score > 1.0) score = 1.0;
    return score;
}

int memory_tick(memory_store_t *m) {
    int evicted = memory_decay_tick(m, 0.01);
    int promoted = memory_consolidate(m);
    return evicted + promoted;
}

/* ── Classification (doctrine/CLASSIFICATION.md §5) ───────────────────── */

static bool memory_class_valid(memory_class_t level) {
    return level >= MEM_CLASS_OPEN && level <= MEM_CLASS_UMBRAL;
}

static bool memory_class_allows_manual_promotion(const memory_entry_t *e) {
    if (!e || !e->active || e->tier >= MEM_SEMANTIC || !memory_class_valid(e->classification))
        return false;

    switch (e->classification) {
    case MEM_CLASS_OPEN:
    case MEM_CLASS_HELD:
        return true;
    case MEM_CLASS_SEALED:
        return e->tier != MEM_EPISODIC || e->class_reviewed;
    case MEM_CLASS_UMBRAL:
        return e->class_reviewed;
    default:
        return false;
    }
}

bool memory_class_promotable(const memory_entry_t *e) {
    if (!e || !e->active || e->tier >= MEM_SEMANTIC || !memory_class_valid(e->classification))
        return false;
    switch (e->classification) {
    case MEM_CLASS_OPEN:
    case MEM_CLASS_HELD:
        return true; /* L0/L1 consolidate freely */
    case MEM_CLASS_SEALED:
        /* L2: automatic working->episodic is allowed; semantic is review-only
         * through memory_promote(), never by the sweep. */
        return e->tier == MEM_WORKING;
    case MEM_CLASS_UMBRAL:
        /* L3: the consolidation daemon never promotes existence-classified
         * material. Tier 0 promotion is manual and consumes a review grant. */
        return false;
    default:
        /* Unknown label: fail closed. An unlabeled-but-hot entry must not
         * consolidate on a default-permit path. */
        return false;
    }
}

bool memory_classify(memory_store_t *m, const char *key, memory_class_t level) {
    if (!m || !m->initialized || !key)
        return false;
    DSCO_REQUIRE_RET(memory_class_valid(level), false);
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    /* No in-band declassification (SECRECY_HARDENING §5): labels only go up
     * here. Downgrades run through the DECLASSIFY ritual out-of-band, which
     * rewrites the entry rather than lowering the label in place. */
    if (!memory_class_valid(e->classification))
        e->classification = MEM_CLASS_OPEN;
    if (level < e->classification)
        return false;
    /* Raising the label revokes any pending review grant: the grant was made
     * against the old level's rules. */
    if (level != e->classification)
        e->class_reviewed = false;
    e->classification = level;
    /* UMBRAL must not sit in episodic/semantic. If material is re-labeled L3
     * after it already consolidated, that is a live consolidation leak —
     * demote it back to working immediately and let session-close purge it. */
    if (level == MEM_CLASS_UMBRAL && e->tier != MEM_WORKING) {
        m->tier_count[e->tier]--;
        e->tier = MEM_WORKING;
        m->tier_count[MEM_WORKING]++;
        e->created_at = now_sec(); /* working-tier decay clock */
    }
    return true;
}

bool memory_classify_review(memory_store_t *m, const char *key) {
    if (!m || !m->initialized || !key)
        return false;
    memory_entry_t *e = find_by_key(m, key);
    if (!e)
        return false;
    /* Review grants are only meaningful for gated levels. */
    if (e->classification != MEM_CLASS_SEALED && e->classification != MEM_CLASS_UMBRAL)
        return false;
    e->class_reviewed = true;
    return true;
}

int memory_purge_umbral(memory_store_t *m) {
    if (!m || !m->initialized)
        return 0;
    int purged = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        memory_entry_t *e = &m->entries[i];
        if (!e->active || e->classification != MEM_CLASS_UMBRAL || e->tier != MEM_WORKING)
            continue;
        /* Pinning does not save non-promoted UMBRAL working memory: L3 TTL
         * discipline outranks decay exemption. Verified deletion = zero the
         * value bytes, not just the active flag. */
        m->tier_count[e->tier]--;
        m->count--;
        m->total_evictions++;
        memset(e, 0, sizeof(*e));
        purged++;
    }
    return purged;
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

static int memory_copy_json_buf(const jbuf_t *b, char *buf, size_t len) {
    size_t n = (b && b->data) ? b->len : 0;
    if (buf && len > 0) {
        size_t copy = n < len - 1 ? n : len - 1;
        if (copy > 0)
            memcpy(buf, b->data, copy);
        buf[copy] = '\0';
    }
    return n > (size_t)INT_MAX ? INT_MAX : (int)n;
}

int memory_to_json(const memory_store_t *m, char *buf, size_t len) {
    if (!m || !buf)
        return 0;
    double t = now_sec();
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"entries\":[");

    bool first = true;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        const memory_entry_t *e = &m->entries[i];
        if (!e->active)
            continue;

        double strength = memory_calc_strength(e->tier, e->created_at, t);
        if (!first)
            jbuf_append_char(&b, ',');
        jbuf_appendf(&b, "{\"id\":%d,\"tier\":", e->id);
        jbuf_append_json_str(&b, memory_tier_name(e->tier));
        jbuf_append(&b, ",\"key\":");
        jbuf_append_json_str(&b, e->key);
        jbuf_appendf(&b, ",\"importance\":%.2f,\"strength\":%.4f,"
                         "\"access_count\":%d,\"pinned\":%s,\"classification\":",
                     e->importance, strength, e->access_count, e->pinned ? "true" : "false");
        jbuf_append_json_str(&b, memory_class_name(e->classification));
        jbuf_appendf(&b, ",\"classification_level\":%d,\"class_reviewed\":%s}",
                     memory_class_valid(e->classification) ? (int)e->classification : -1,
                     e->class_reviewed ? "true" : "false");
        first = false;
    }
    jbuf_append(&b, "]}");
    int n = memory_copy_json_buf(&b, buf, len);
    jbuf_free(&b);
    return n;
}

int memory_tier_to_json(const memory_store_t *m, memory_tier_t tier, char *buf, size_t len) {
    if (!m || !buf)
        return 0;
    DSCO_REQUIRE_RET(tier >= 0 && tier < MEM_TIER_COUNT, 0);
    double t = now_sec();
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"tier\":");
    jbuf_append_json_str(&b, memory_tier_name(tier));
    jbuf_append(&b, ",\"entries\":[");

    bool first = true;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++) {
        const memory_entry_t *e = &m->entries[i];
        if (!e->active || e->tier != tier)
            continue;

        double strength = memory_calc_strength(e->tier, e->created_at, t);
        if (!first)
            jbuf_append_char(&b, ',');
        jbuf_append(&b, "{\"key\":");
        jbuf_append_json_str(&b, e->key);
        jbuf_append(&b, ",\"value\":");
        size_t value_len = strlen(e->value);
        char preview[81];
        size_t preview_len = value_len > 80 ? 80 : value_len;
        memcpy(preview, e->value, preview_len);
        preview[preview_len] = '\0';
        jbuf_append_json_str(&b, preview);
        jbuf_appendf(&b, ",\"strength\":%.4f,\"importance\":%.2f,\"accesses\":%d,"
                         "\"classification\":",
                     strength, e->importance, e->access_count);
        jbuf_append_json_str(&b, memory_class_name(e->classification));
        jbuf_appendf(&b, ",\"classification_level\":%d}",
                     memory_class_valid(e->classification) ? (int)e->classification : -1);
        first = false;
    }
    jbuf_append(&b, "]}");
    int n = memory_copy_json_buf(&b, buf, len);
    jbuf_free(&b);
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
        jbuf_t val;
        jbuf_init(&val, 2048);
        jbuf_append(&val, "{\"value\":");
        size_t value_len = strlen(e->value);
        char preview[1501];
        size_t preview_len = value_len > 1500 ? 1500 : value_len;
        memcpy(preview, e->value, preview_len);
        preview[preview_len] = '\0';
        jbuf_append_json_str(&val, preview);
        jbuf_appendf(&val, ",\"importance\":%.2f,\"access_count\":%d,\"pinned\":%s,"
                          "\"classification\":",
                     e->importance, e->access_count, e->pinned ? "true" : "false");
        jbuf_append_json_str(&val, memory_class_name(e->classification));
        jbuf_appendf(&val, ",\"classification_level\":%d}",
                     memory_class_valid(e->classification) ? (int)e->classification : -1);
        vfs_kv_put_str(g_mem_vfs, "semantic_memory", e->key, val.data ? val.data : "{}");
        jbuf_free(&val);
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
            char *stored_value = json_get_str(val, "value");
            const char *value = stored_value ? stored_value : val;
            double importance = json_get_double(val, "importance", 0.8);
            int id = memory_store(m, MEM_SEMANTIC, keys[i], value, importance);
            if (id >= 0) {
                memory_entry_t *e = find_by_key(m, keys[i]);
                if (e) {
                    e->access_count = json_get_int(val, "access_count", e->access_count);
                    e->pinned = json_get_bool(val, "pinned", e->pinned);
                    char *class_s = json_get_str(val, "classification");
                    memory_class_t c = MEM_CLASS_OPEN;
                    if (class_s && memory_class_from_name(class_s, &c))
                        e->classification = c;
                    free(class_s);
                }
                restored++;
            }
            free(stored_value);
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
