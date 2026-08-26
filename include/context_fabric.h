#ifndef DSCO_CONTEXT_FABRIC_H
#define DSCO_CONTEXT_FABRIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Context Fabric — unified content-addressable + semantic context store.
 *
 * The single broker every agent and orchestrator reads/writes context through.
 * Fronts the existing stores (vecstore for the semantic index, VFS KV for the
 * blob + metadata tables) behind ONE addressing scheme: the ctxkey.
 *
 * A ctxkey is a small, stable, sliceable handle:
 *
 *     ck:<kind>:<id>[#<slice>]
 *     ck:blob:9f3ac1…                  content-addressed, immutable, deduped
 *     ck:blob:9f3ac1…#b:0-2048         a byte span  — reference without loading
 *     ck:blob:9f3ac1…#l:120-160        a line span
 *     ck:mem:working/task-goal         a named (mutable) entry
 *     ck:scope:7c1e…                   a bundle of ctxkeys — the unit of delegation
 *
 * Immutable kinds (blob/file/msg/tool/run) are addressed by the sha256 of their
 * content, so the same bytes stored twice collapse to one entry and can be
 * referenced anywhere by a stable short handle. Named kinds (mem/plan/scope)
 * carry a caller-chosen id.
 *
 * This is the foundation slice: put / get / search / scope + pin. The windowing
 * invariant (automatic spill→stub→rehydrate) and the meta-orchestrator that
 * passes scopes instead of raw content compose over this layer.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CTX_ID_LEN       128  /* sha256 hex (64) or a name (mem/scope) */
#define CTX_KEY_STR_MAX  256  /* canonical "ck:kind:id#slice" form */

typedef enum {
    CTX_KIND_BLOB = 0,  /* content-addressed immutable bytes (default) */
    CTX_KIND_FILE,      /* snapshot of a file's contents */
    CTX_KIND_MSG,       /* a conversation message */
    CTX_KIND_TOOL,      /* a tool result */
    CTX_KIND_MEM,       /* a named memory-tier entry */
    CTX_KIND_PLAN,      /* a cached plan */
    CTX_KIND_RUN,       /* a chronicle run artifact */
    CTX_KIND_SCOPE,     /* a bundle of ctxkeys */
    CTX_KIND_COUNT
} ctx_kind_t;

typedef enum {
    CTX_SLICE_NONE = 0, /* whole object */
    CTX_SLICE_BYTES,    /* [start, end)  — end < 0 means "to end" */
    CTX_SLICE_LINES     /* [start, end]  — 1-based inclusive; end < 0 means "to end" */
} ctx_slice_kind_t;

typedef struct {
    ctx_kind_t       kind;
    char             id[CTX_ID_LEN]; /* content hash or name */
    ctx_slice_kind_t slice_kind;
    long             slice_start;
    long             slice_end;
} ctxkey_t;

/* ── ctxkey formatting / parsing ──────────────────────────────────────────
 * format() writes the canonical string form; returns bytes written (excl NUL)
 * or -1 on overflow. parse() fills *out from a canonical string; returns false
 * on malformed input. Both are pure and allocation-free. */
int  ctxkey_format(const ctxkey_t *key, char *out, size_t cap);
bool ctxkey_parse(const char *s, ctxkey_t *out);
const char *ctx_kind_name(ctx_kind_t kind);

/* ── Broker lifecycle ─────────────────────────────────────────────────────
 * ctx_broker_default() lazily opens the process-wide broker at
 * $DSCO_CONTEXT_DB (default ~/.dsco/context/fabric.db) and never fails to a
 * NULL that callers must special-case beyond the usual "store unavailable"
 * degrade. Explicit open/close is for tests and isolated scopes. */
typedef struct ctx_broker ctx_broker_t;
struct vfs_db; /* fwd decl; defined in vfs.h */

ctx_broker_t *ctx_broker_open(const char *db_path);
void          ctx_broker_close(ctx_broker_t *b);
ctx_broker_t *ctx_broker_default(void);

/* Open a broker over an existing, caller-owned vfs handle. The broker does
 * NOT take ownership and will not close it — this is how the fabric shares
 * the single process-wide db instead of opening a private file. */
ctx_broker_t *ctx_broker_open_vfs(struct vfs_db *vfs);

/* Attach the process-wide default broker to a shared, caller-owned vfs handle
 * so ctx_broker_default() rides the single process db (buckets ctx_blob /
 * ctx_meta / ctx_pin / ctx_scope / ctx_index) alongside every other store.
 * Call once at startup, before the first ctx_broker_default(). */
void ctx_broker_set_shared_vfs(struct vfs_db *vfs);

/* ── Put ──────────────────────────────────────────────────────────────────
 * Store bytes; returns a content-addressed key in *out_key. Idempotent: the
 * same bytes yield the same key and are stored once (out_deduped set true on a
 * hit). When opts->embed and the content is textual, it is indexed for
 * semantic search. opts may be NULL for defaults (kind=BLOB, embed=true). */
typedef struct {
    ctx_kind_t  kind;       /* default CTX_KIND_BLOB */
    const char *source;     /* provenance: file path, tool name, session id (optional) */
    const char *tags;       /* comma-separated tags (optional) */
    double      importance; /* 0..1 retention/ranking hint (default 0.5) */
    int         embed;      /* -1 auto (embed if textual), 0 never, 1 force */
} ctx_put_opts_t;

bool ctx_put(ctx_broker_t *b, const void *bytes, size_t n,
             const ctx_put_opts_t *opts, ctxkey_t *out_key, bool *out_deduped);

/* ── Get ──────────────────────────────────────────────────────────────────
 * Fetch (a slice of) content by key. Returns a malloc'd, NUL-terminated buffer
 * (caller frees) with the byte length in *out_len, or NULL if absent. The slice
 * embedded in the key is applied; pass a key with CTX_SLICE_NONE for the whole
 * object. */
char *ctx_get(ctx_broker_t *b, const ctxkey_t *key, size_t *out_len);

/* ── Search ───────────────────────────────────────────────────────────────
 * Semantic (embedding) search with a bounded lexical fallback when embeddings
 * are unavailable. Fans out over the index, ranks, dedups, and returns up to
 * `max` hits in caller-provided storage. kind_filter < 0 matches any kind. */
typedef struct {
    ctxkey_t key;
    float    score;
    size_t   size;
    char     source[128];
    char     preview[160];
} ctx_search_hit_t;

int ctx_search(ctx_broker_t *b, const char *query, int kind_filter,
               int max, ctx_search_hit_t *out);

/* ── Pin ──────────────────────────────────────────────────────────────────
 * Pinned keys survive compaction and are assembled into the window first.
 * These back the "pinned" retrieval tier the assembler will consume. */
bool ctx_pin(ctx_broker_t *b, const ctxkey_t *key);
bool ctx_unpin(ctx_broker_t *b, const ctxkey_t *key);
int  ctx_pinned_list(ctx_broker_t *b, ctxkey_t *out, int max);

/* ── Scope (delegation bundle) ────────────────────────────────────────────
 * A scope names a set of ctxkeys so an orchestrator can hand a child a single
 * ck:scope handle instead of inlining content. The child resolves it and
 * faults-in only the members it touches. */
bool ctx_scope_create(ctx_broker_t *b, const ctxkey_t *keys, int n, ctxkey_t *out_scope);
int  ctx_scope_resolve(ctx_broker_t *b, const ctxkey_t *scope, ctxkey_t *out, int max);

/* ── Stats ────────────────────────────────────────────────────────────────*/
typedef struct {
    int     blobs;
    int64_t bytes;
    int     vectors;
    int     pinned;
    int     scopes;
    bool    embeddings_available;
} ctx_stat_t;

ctx_stat_t ctx_broker_stat(ctx_broker_t *b);

#endif /* DSCO_CONTEXT_FABRIC_H */
