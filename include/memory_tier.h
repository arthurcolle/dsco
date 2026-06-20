#ifndef DSCO_MEMORY_TIER_H
#define DSCO_MEMORY_TIER_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Three-Tier Agent Memory System (Wings)
 *
 * Cognitive memory model with automatic consolidation:
 *   Working  → 60s half-life  (immediate context, high churn)
 *   Episodic → 3600s half-life (recent experiences, medium persistence)
 *   Semantic → no decay        (foundational knowledge, permanent)
 *
 * From the Overmind Soul v1.0 §5.1:
 *   "Agent-native data structures with 3-tier memory:
 *    Working (60s), Episodic (3600s), Semantic (no decay).
 *    Consolidation: memories move upward based on importance and
 *    access frequency."
 *
 * This enables agents to maintain context awareness while
 * automatically forgetting irrelevant details.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MEMTIER_MAX_ENTRIES      512
#define MEMTIER_KEY_LEN          128
#define MEMTIER_VALUE_LEN        4096
#define MEMTIER_TAG_LEN          64
#define MEMTIER_MAX_TAGS         8

/* ── Memory Tiers ─────────────────────────────────────────────────────── */

typedef enum {
    MEM_WORKING,     /* 60s half-life — scratchpad, current task context */
    MEM_EPISODIC,    /* 3600s half-life — recent results, interactions */
    MEM_SEMANTIC,    /* no decay — learned facts, skills, identities */
    MEM_TIER_COUNT
} memory_tier_t;

/* Half-life constants (seconds) */
#define MEM_WORKING_HALFLIFE    60.0
#define MEM_EPISODIC_HALFLIFE   3600.0
#define MEM_SEMANTIC_HALFLIFE   0.0    /* 0 = no decay */

/* ── Consolidation thresholds ─────────────────────────────────────────── */

#define MEM_CONSOLIDATION_ACCESS_COUNT  3   /* accesses to promote */
#define MEM_CONSOLIDATION_IMPORTANCE    0.7 /* importance to auto-promote */
#define MEM_CONSOLIDATION_INTERVAL      30  /* seconds between consolidation sweeps */

/* ── Memory Entry ─────────────────────────────────────────────────────── */

typedef struct {
    int            id;
    memory_tier_t  tier;
    char           key[MEMTIER_KEY_LEN];
    char           value[MEMTIER_VALUE_LEN];
    char           tags[MEMTIER_MAX_TAGS][MEMTIER_TAG_LEN];
    int            tag_count;
    double         importance;     /* 0.0 - 1.0 */
    double         strength;       /* current strength after decay [0.0, 1.0] */
    double         created_at;
    double         last_accessed;
    int            access_count;
    bool           pinned;         /* exempt from decay */
    bool           active;
    bool           has_embedding;  /* true if embedding cached in vecstore */
} memory_entry_t;

/* ── Memory Store ─────────────────────────────────────────────────────── */

typedef struct {
    memory_entry_t  entries[MEMTIER_MAX_ENTRIES];
    int             count;
    int             next_id;
    double          last_consolidation;
    bool            initialized;

    /* Per-tier counts */
    int             tier_count[MEM_TIER_COUNT];

    /* Statistics */
    int             total_stores;
    int             total_recalls;
    int             total_promotions;
    int             total_evictions;
    int             total_consolidations;
} memory_store_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void memory_store_init(memory_store_t *m);
void memory_store_destroy(memory_store_t *m);

/* ── Store & Recall ───────────────────────────────────────────────────── */

/* Store a memory. Returns entry ID or -1 on error.
   importance: 0.0-1.0, higher = more likely to be promoted/retained. */
int memory_store(memory_store_t *m, memory_tier_t tier,
                 const char *key, const char *value,
                 double importance);

/* Store with tags for semantic search. */
int memory_store_tagged(memory_store_t *m, memory_tier_t tier,
                        const char *key, const char *value,
                        double importance,
                        const char **tags, int tag_count);

/* Recall by key. Updates access count and last_accessed.
   Returns entry or NULL. */
const memory_entry_t *memory_recall(memory_store_t *m, const char *key);

/* Recall by tag. Returns matching entries. */
int memory_recall_by_tag(memory_store_t *m, const char *tag,
                         const memory_entry_t **out, int max);

/* Recall all entries in a tier. Returns count. */
int memory_recall_tier(memory_store_t *m, memory_tier_t tier,
                       const memory_entry_t **out, int max);

/* Search by substring in key or value. Returns count. */
int memory_search(memory_store_t *m, const char *query,
                  const memory_entry_t **out, int max);

/* ── Modification ─────────────────────────────────────────────────────── */

/* Update value of existing entry. */
bool memory_update(memory_store_t *m, const char *key, const char *value);

/* Delete an entry. */
bool memory_forget(memory_store_t *m, const char *key);

/* Pin an entry (exempt from decay). */
bool memory_pin(memory_store_t *m, const char *key);

/* Unpin an entry. */
bool memory_unpin(memory_store_t *m, const char *key);

/* Manually promote entry to next tier. */
bool memory_promote(memory_store_t *m, const char *key);

/* ── Decay & Consolidation ────────────────────────────────────────────── */

/* Apply decay to all entries. Evict entries below threshold.
   Returns number of entries evicted. */
int memory_decay_tick(memory_store_t *m, double threshold);

/* Run consolidation: promote frequently-accessed or important memories.
   Returns number of promotions. */
int memory_consolidate(memory_store_t *m);

/* Full maintenance pass (decay + consolidate). */
int memory_tick(memory_store_t *m);

/* ── Serialization ────────────────────────────────────────────────────── */

int memory_to_json(const memory_store_t *m, char *buf, size_t len);
int memory_tier_to_json(const memory_store_t *m, memory_tier_t tier,
                        char *buf, size_t len);
int memory_status_json(const memory_store_t *m, char *buf, size_t len);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *memory_tier_name(memory_tier_t t);
double memory_tier_halflife(memory_tier_t t);

/* Calculate current strength after decay.
   Returns 0.0 if fully decayed, 1.0 if just stored. */
double memory_calc_strength(memory_tier_t tier, double created_at,
                            double now);

/* ── §8: VFS Persistence ──────────────────────────────────────────── */

struct vfs_db;
typedef struct vfs_db vfs_db_t;

/* Connect memory system to VFS for semantic memory persistence */
void memory_store_set_vfs(vfs_db_t *vfs);

/* Persist all semantic-tier memories to VFS */
void memory_persist_semantic(memory_store_t *m);

/* Restore semantic memories from VFS. Returns count restored. */
int  memory_restore_semantic(memory_store_t *m);

/* ── §9: Embedding-backed Semantic Search ────────────────────────── */

struct vecstore;

/* Connect memory system to a vecstore for embedding storage */
void memory_store_set_vecstore(struct vecstore *vs);

/* Search by semantic similarity (falls back to substring if no embeddings).
   Returns count of matching entries written to out[]. */
int  memory_search_semantic(memory_store_t *m, const char *query,
                            const memory_entry_t **out, int max);

/* Attach a pre-computed embedding to a memory entry. */
bool memory_entry_set_embedding(memory_store_t *m, const char *key,
                                const float *vec, int dim);

#endif
