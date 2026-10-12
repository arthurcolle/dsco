/* context_fabric.c — unified content-addressable + semantic context store.
 *
 * Fronts the existing stores behind one addressing scheme (ctxkey):
 *   - VFS KV buckets  ctx_blob / ctx_meta / ctx_pin / ctx_scope  (storage)
 *   - vecstore "ctx_index"                                        (semantic search)
 *   - tools_embed_text()                                          (in-process embeddings)
 *   - sha256_hex()                                                (content addressing)
 *
 * See include/context_fabric.h for the model. This is the foundation slice:
 * put / get / search / scope / pin. The windowing invariant and the
 * meta-orchestrator compose over this layer.
 */

#include "context_fabric.h"

#include "crypto.h"
#include "json_util.h"
#include "tools.h"
#include "vecstore.h"
#include "vfs.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Buckets */
#define CTX_BUCKET_BLOB  "ctx_blob"
#define CTX_BUCKET_META  "ctx_meta"
#define CTX_BUCKET_PIN   "ctx_pin"
#define CTX_BUCKET_SCOPE "ctx_scope"
#define CTX_VEC_COLL     "ctx_index"

#define CTX_EMBED_MAX_BYTES 8192  /* cap text sent to the embedder */
#define CTX_PREVIEW_BYTES   140

static void ctx_default_db_path(char *out, size_t cap);

struct ctx_broker {
    vfs_db_t       *vfs;
    vecstore_t     *vec;
    pthread_mutex_t mu;
    bool            owns_vfs;
    pthread_t reindex_thread;
    bool reindex_started, reindex_stop;
    int reindex_pending, reindex_failures;
    sqlite3        *fts; /* BM25 full-text index; NULL if FTS5 unavailable */
};

/* ── Lexical index (SQLite FTS5, BM25) ───────────────────────────────────
 * The prior lexical fallback was a linear substring scan over metadata
 * previews only — no ranking, no body search, capped scan. FTS5 gives real
 * retrieval: full document body indexed, BM25 ranking, porter stemming,
 * unicode61 tokenization, and the FTS5 query grammar (phrases, AND/OR/NOT,
 * NEAR, prefix*) for free. A sibling database next to the fabric's VFS file;
 * additive, and search falls back to full textual bodies if this is NULL
 * (e.g. a SQLite build without FTS5). */

static void ctx_fts_path(char *out, size_t cap) {
    char base[900];
    ctx_default_db_path(base, sizeof(base));
    snprintf(out, cap, "%s.fts", base);
}

static sqlite3 *ctx_fts_open_at(const char *base) {
    char path[1024];
    if(base && *base) {
        int n=snprintf(path,sizeof(path),"%s.fts",base);
        if(n<0 || (size_t)n>=sizeof(path))return NULL;
    } else ctx_fts_path(path, sizeof(path));
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    char *err = NULL;
    /* source/tags weighted above body at query time via bm25() column
     * weights; porter stemming folds run/running/ran together, unicode61
     * folds diacritics. */
    int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE IF NOT EXISTS ctx_fts USING fts5("
        "  ckey UNINDEXED, source, tags, body,"
        "  tokenize='porter unicode61 remove_diacritics 2'"
        ");", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    /* FTS UNINDEXED columns cannot support a B-tree lookup. Keep an ordinary
     * key->rowid index so replacement does not scan every document body.
     * Migrate the existing FTS rows once, atomically; keep the FTS schema and
     * its BM25/snippet behavior intact. */
    sqlite3_stmt *exists=NULL;
    bool mapped=false;
    if(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='ctx_fts_keys'",-1,&exists,NULL)==SQLITE_OK)
        mapped=sqlite3_step(exists)==SQLITE_ROW;
    sqlite3_finalize(exists);
    if(!mapped) {
        rc=sqlite3_exec(db,"BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS ctx_fts_keys(ckey TEXT PRIMARY KEY,fts_rowid INTEGER NOT NULL);"
            "INSERT OR REPLACE INTO ctx_fts_keys SELECT ckey,max(rowid) FROM ctx_fts GROUP BY ckey;"
            "COMMIT;",NULL,NULL,NULL);
        if(rc!=SQLITE_OK) { sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(db);return NULL; }
    }
    if(sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS ctx_fts_full(ckey TEXT PRIMARY KEY,bytes INTEGER NOT NULL)",NULL,NULL,NULL)!=SQLITE_OK) {
        sqlite3_close(db);return NULL;
    }
    return db;
}

/* Index (or reindex) one document. Idempotent on ckey. */
static bool ctx_fts_index(sqlite3 *fts, const char *ckey, const char *source,
                          const char *tags, const char *body) {
    if (!fts || !ckey) return false;
    if(sqlite3_exec(fts,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK) return false;
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(fts, "DELETE FROM ctx_fts WHERE rowid=(SELECT fts_rowid FROM ctx_fts_keys WHERE ckey=?1) AND ckey=?1;", -1, &del, NULL)!=SQLITE_OK) goto rollback;
    sqlite3_bind_text(del, 1, ckey, -1, SQLITE_TRANSIENT);
    int rc=sqlite3_step(del);sqlite3_finalize(del);
    if(rc!=SQLITE_DONE) goto rollback;
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(fts,
            "INSERT INTO ctx_fts(ckey, source, tags, body) VALUES (?1,?2,?3,?4);", -1,
            &ins, NULL) != SQLITE_OK)
        goto rollback;
    sqlite3_bind_text(ins, 1, ckey, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, source ? source : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, tags ? tags : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, body ? body : "", -1, SQLITE_TRANSIENT);
    rc=sqlite3_step(ins);
    sqlite3_finalize(ins);
    if(rc!=SQLITE_DONE) goto rollback;
    sqlite3_int64 rowid=sqlite3_last_insert_rowid(fts);
    sqlite3_stmt *map=NULL;
    if(sqlite3_prepare_v2(fts,"INSERT OR REPLACE INTO ctx_fts_keys(ckey,fts_rowid) VALUES(?1,?2)",-1,&map,NULL)!=SQLITE_OK) goto rollback;
    sqlite3_bind_text(map,1,ckey,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(map,2,rowid);
    rc=sqlite3_step(map);sqlite3_finalize(map);
    if(rc!=SQLITE_DONE)goto rollback;
    sqlite3_stmt *coverage=NULL;
    if(sqlite3_prepare_v2(fts,"INSERT OR REPLACE INTO ctx_fts_full(ckey,bytes) VALUES(?1,?2)",-1,&coverage,NULL)!=SQLITE_OK)goto rollback;
    sqlite3_bind_text(coverage,1,ckey,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(coverage,2,(sqlite3_int64)strlen(body?body:""));
    rc=sqlite3_step(coverage);sqlite3_finalize(coverage);
    if(rc!=SQLITE_DONE || sqlite3_exec(fts,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto rollback;
    return true;
rollback:
    sqlite3_exec(fts,"ROLLBACK",NULL,NULL,NULL);return false;
}

/* BM25 search; column weights favor source/tags over body 3x. FTS5's bm25()
 * returns more-negative-is-better; we negate so higher score is better,
 * matching ctx_search_hit_t's convention. Falls back to a raw LIKE scan when
 * the query does not parse as FTS5 syntax (e.g. bare code punctuation). */
static int ctx_fts_search(sqlite3 *fts, const char *query, int max,
                          char out_keys[][128], float *out_scores, char out_snip[][256]) {
    if (!fts || !query || !*query || max <= 0)
        return 0;
    const char *sql =
        "SELECT ckey, bm25(ctx_fts, 3.0, 2.0, 1.0) AS rank, "
        "snippet(ctx_fts, 3, '[', ']', '...', 12) "
        "FROM ctx_fts WHERE ctx_fts MATCH ?1 ORDER BY rank LIMIT ?2;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fts, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        double rank = sqlite3_column_double(st, 1);
        const unsigned char *snip = sqlite3_column_text(st, 2);
        if (!k)
            continue;
        snprintf(out_keys[n], 128, "%s", k);
        out_scores[n] = (float)(-rank); /* higher = better */
        snprintf(out_snip[n], 256, "%s", snip ? (const char *)snip : "");
        n++;
    }
    sqlite3_finalize(st);
    if (n == 0) {
        /* FTS5 syntax error (raw punctuation, unbalanced quotes) or no match:
         * degrade to a literal substring scan via LIKE, still ranked cheaply
         * by match position. */
        sqlite3_stmt *lk = NULL;
        if (sqlite3_prepare_v2(fts,
                "SELECT ckey, source, tags, substr(body,1,200) FROM ctx_fts "
                "WHERE body LIKE '%' || ?1 || '%' OR source LIKE '%' || ?1 || '%' "
                "OR tags LIKE '%' || ?1 || '%' LIMIT ?2;", -1, &lk, NULL) == SQLITE_OK) {
            sqlite3_bind_text(lk, 1, query, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(lk, 2, max);
            while (n < max && sqlite3_step(lk) == SQLITE_ROW) {
                const unsigned char *k = sqlite3_column_text(lk, 0);
                const unsigned char *body = sqlite3_column_text(lk, 3);
                if (!k)
                    continue;
                snprintf(out_keys[n], 128, "%s", k);
                out_scores[n] = 0.3f; /* below any real BM25-normalized hit */
                snprintf(out_snip[n], 256, "%s", body ? (const char *)body : "");
                n++;
            }
            sqlite3_finalize(lk);
        }
    }
    return n;
}

/* ── ctxkey formatting / parsing ──────────────────────────────────────────*/

static const char *const k_kind_names[CTX_KIND_COUNT] = {
    "blob", "file", "msg", "tool", "mem", "plan", "run", "scope"};

const char *ctx_kind_name(ctx_kind_t kind) {
    if (kind < 0 || kind >= CTX_KIND_COUNT)
        return "blob";
    return k_kind_names[kind];
}

static bool ctx_kind_from_name(const char *s, size_t n, ctx_kind_t *out) {
    for (int i = 0; i < CTX_KIND_COUNT; i++) {
        if (strlen(k_kind_names[i]) == n && strncmp(s, k_kind_names[i], n) == 0) {
            *out = (ctx_kind_t)i;
            return true;
        }
    }
    return false;
}

int ctxkey_format(const ctxkey_t *key, char *out, size_t cap) {
    if (!key || !out || cap == 0)
        return -1;
    int n;
    if (key->slice_kind == CTX_SLICE_BYTES)
        n = snprintf(out, cap, "ck:%s:%s#b:%ld-%ld", ctx_kind_name(key->kind), key->id,
                     key->slice_start, key->slice_end);
    else if (key->slice_kind == CTX_SLICE_LINES)
        n = snprintf(out, cap, "ck:%s:%s#l:%ld-%ld", ctx_kind_name(key->kind), key->id,
                     key->slice_start, key->slice_end);
    else
        n = snprintf(out, cap, "ck:%s:%s", ctx_kind_name(key->kind), key->id);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}

bool ctxkey_parse(const char *s, ctxkey_t *out) {
    if (!s || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->slice_kind = CTX_SLICE_NONE;
    if (strncmp(s, "ck:", 3) != 0)
        return false;
    s += 3;
    const char *colon = strchr(s, ':');
    if (!colon)
        return false;
    if (!ctx_kind_from_name(s, (size_t)(colon - s), &out->kind))
        return false;
    const char *id = colon + 1;
    const char *hash = strchr(id, '#');
    size_t id_len = hash ? (size_t)(hash - id) : strlen(id);
    if (id_len == 0 || id_len >= CTX_ID_LEN)
        return false;
    memcpy(out->id, id, id_len);
    out->id[id_len] = '\0';
    for(size_t i=0;i<id_len;i++)
        if((unsigned char)id[i]<=0x20 || id[i]=='"' || id[i]=='\\' || (unsigned char)id[i]==0x7f)return false;
    if (hash) {
        const char *range=hash+1;
        if((range[0]!='b' && range[0]!='l') || range[1]!=':' || !isdigit((unsigned char)range[2]))return false;
        char *end=NULL;errno=0;
        long start=strtol(range+2,&end,10);
        if(errno || start<0 || (range[0]=='l' && start<1) || !end || *end!='-')return false;
        const char *finish=end+1;long stop=-1;
        if(*finish) {
            if(strcmp(finish,"-1") && !isdigit((unsigned char)*finish))return false;
            errno=0;stop=strtol(finish,&end,10);
            if(errno || !end || *end || (stop<0 && stop!=-1) || (stop>=0 && stop<start))return false;
        }
        out->slice_kind=range[0]=='l'?CTX_SLICE_LINES:CTX_SLICE_BYTES;
        out->slice_start=start;out->slice_end=stop;
    }
    return true;
}

/* ── path helpers ─────────────────────────────────────────────────────────*/

static void ctx_mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    /* Final component too: the loop above only fires at embedded '/', so a
     * path like ~/.dsco/context never had its last segment created and
     * vfs_open(…, CREATE) failed with ENOENT on fresh HOMEs. mkdir is
     * idempotent (EEXIST swallowed). */
    mkdir(tmp, 0700);
}

static void ctx_default_db_path(char *out, size_t cap) {
    const char *override = getenv("DSCO_CONTEXT_DB");
    if (override && *override) {
        snprintf(out, cap, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(out, cap, "%s/.dsco/context/fabric.db", home);
}

/* ── content classification / preview ─────────────────────────────────────*/

static bool ctx_is_textual(const void *bytes, size_t n) {
    const unsigned char *p = bytes;
    size_t sample = n < 512 ? n : 512;
    if (sample == 0)
        return true;
    size_t printable = 0;
    for (size_t i = 0; i < sample; i++) {
        if (p[i] == 0)
            return false; /* NUL → binary */
        if (p[i] == '\n' || p[i] == '\t' || p[i] == '\r' || (p[i] >= 0x20 && p[i] < 0x7f) ||
            p[i] >= 0x80)
            printable++;
    }
    return printable * 100 >= sample * 85; /* ≥85% printable */
}

/* Copy a sanitized single-line preview (control chars → space) into `out`. */
static void ctx_make_preview(const void *bytes, size_t n, char *out, size_t cap) {
    const unsigned char *p = bytes;
    size_t lim = n < CTX_PREVIEW_BYTES ? n : CTX_PREVIEW_BYTES;
    if (lim >= cap)
        lim = cap - 1;
    size_t j = 0;
    for (size_t i = 0; i < lim; i++) {
        unsigned char c = p[i];
        out[j++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    out[j] = '\0';
}

/* ── metadata (JSON in ctx_meta + vecstore metadata) ──────────────────────*/

static char *ctx_build_meta_json(ctx_kind_t kind, const char *source, const char *tags,
                                 double importance, size_t size, const char *preview) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"kind\":%d,\"size\":%zu,\"importance\":%.3f,\"source\":", (int)kind, size,
                 importance);
    jbuf_append_json_str(&b, source ? source : "");
    jbuf_append(&b, ",\"tags\":");
    jbuf_append_json_str(&b, tags ? tags : "");
    jbuf_append(&b, ",\"preview\":");
    jbuf_append_json_str(&b, preview ? preview : "");
    jbuf_append(&b, "}");
    return b.data; /* caller frees */
}

/* A stale lexical prefix must not remain the only searchable evidence after
 * restart. Rebuild full bodies in an owned worker; shutdown joins that worker
 * before freeing either SQLite handle. Hold the broker lock for one object. */
static void *ctx_reindex_worker(void *arg) {
    ctx_broker_t *b=arg;
    pthread_mutex_lock(&b->mu);
    int count=0;char **keys=vfs_kv_keys(b->vfs,CTX_BUCKET_META,&count);
    b->reindex_pending=count;
    if(!keys && count) b->reindex_failures++;
    pthread_mutex_unlock(&b->mu);
    for(int i=0;keys && i<count;i++) {
        pthread_mutex_lock(&b->mu);
        if(b->reindex_stop){pthread_mutex_unlock(&b->mu);break;}
        bool indexed=false;sqlite3_stmt *mark=NULL;
        if(sqlite3_prepare_v2(b->fts,"SELECT 1 FROM ctx_fts_full WHERE ckey=?1",-1,&mark,NULL)==SQLITE_OK) {
            sqlite3_bind_text(mark,1,keys[i],-1,SQLITE_TRANSIENT);indexed=sqlite3_step(mark)==SQLITE_ROW;
        }
        sqlite3_finalize(mark);
        if(!indexed) {
            size_t bytes=0;char *body=vfs_kv_get(b->vfs,CTX_BUCKET_BLOB,keys[i],&bytes);
            char *meta=vfs_kv_get_str(b->vfs,CTX_BUCKET_META,keys[i]);
            char *source=meta?json_get_str(meta,"source"):NULL,*tags=meta?json_get_str(meta,"tags"):NULL;
            bool ok=body!=NULL;
            if(ok && ctx_is_textual(body,bytes) && !memchr(body,0,bytes)) {
                char *terminated=realloc(body,bytes+1);
                if(terminated){body=terminated;body[bytes]=0;ok=ctx_fts_index(b->fts,keys[i],source,tags,body);}
                else ok=false;
            }
            if(!ok)b->reindex_failures++;
            free(body);free(meta);free(source);free(tags);
        }
        b->reindex_pending--;pthread_mutex_unlock(&b->mu);
    }
    for(int i=0;keys && i<count;i++)free(keys[i]);free(keys);
    return NULL;
}
static void ctx_reindex_start(ctx_broker_t *b) {
    b->reindex_pending=-1;
    if(b->fts && pthread_create(&b->reindex_thread,NULL,ctx_reindex_worker,b)==0)b->reindex_started=true;
    else {b->reindex_pending=0;b->reindex_failures=1;}
}
void ctx_search_coverage(ctx_broker_t *b,int *pending,int *failures) {
    if(!b){if(pending)*pending=0;if(failures)*failures=1;return;}
    pthread_mutex_lock(&b->mu);
    if(pending)*pending=b->reindex_pending;
    if(failures)*failures=b->reindex_failures;
    pthread_mutex_unlock(&b->mu);
}

/* ── broker lifecycle ─────────────────────────────────────────────────────*/

ctx_broker_t *ctx_broker_open(const char *db_path) {
    char pathbuf[1024];
    if (!db_path) {
        ctx_default_db_path(pathbuf, sizeof(pathbuf));
        db_path = pathbuf;
    }
    /* ensure parent dir exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", db_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        ctx_mkdir_p(dir);
    }
    vfs_db_t *vfs = vfs_open(db_path);
    if (!vfs)
        return NULL;
    ctx_broker_t *b = calloc(1, sizeof(*b));
    if (!b) {
        vfs_close(vfs);
        return NULL;
    }
    b->vfs = vfs;
    b->owns_vfs = true;
    b->vec = vecstore_open(vfs, CTX_VEC_COLL); /* may be NULL → search degrades to lexical */
    b->fts = ctx_fts_open_at(vfs_database_path(vfs));
    pthread_mutex_init(&b->mu, NULL);
    ctx_reindex_start(b);
    return b;
}

void ctx_broker_close(ctx_broker_t *b) {
    if (!b)
        return;
    pthread_mutex_lock(&b->mu);b->reindex_stop=true;pthread_mutex_unlock(&b->mu);
    if(b->reindex_started)pthread_join(b->reindex_thread,NULL);
    if (b->vec)
        vecstore_close(b->vec);
    if (b->fts)
        sqlite3_close(b->fts);
    if (b->owns_vfs && b->vfs)
        vfs_close(b->vfs);
    pthread_mutex_destroy(&b->mu);
    free(b);
}

ctx_broker_t *ctx_broker_open_vfs(vfs_db_t *vfs) {
    if (!vfs)
        return NULL;
    ctx_broker_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->vfs = vfs;
    b->owns_vfs = false; /* shared handle — do not close on broker close */
    b->vec = vecstore_open(vfs, CTX_VEC_COLL);
    b->fts = ctx_fts_open_at(vfs_database_path(vfs));
    pthread_mutex_init(&b->mu, NULL);
    ctx_reindex_start(b);
    return b;
}

static pthread_mutex_t g_default_mu = PTHREAD_MUTEX_INITIALIZER;
static ctx_broker_t   *g_default_broker = NULL;
static vfs_db_t       *g_shared_vfs = NULL;

void ctx_broker_set_shared_vfs(vfs_db_t *vfs) {
    pthread_mutex_lock(&g_default_mu);
    g_shared_vfs = vfs;
    pthread_mutex_unlock(&g_default_mu);
}

ctx_broker_t *ctx_broker_default(void) {
    pthread_mutex_lock(&g_default_mu);
    if (!g_default_broker) {
        /* Prefer the shared process-wide vfs so the fabric lives in the one
         * db (~/.dsco/vfs.db) beside every other store; fall back to a private
         * file only when no shared handle was injected (tests, tools run before
         * main wiring). */
        const char *override=getenv("DSCO_CONTEXT_DB");
        g_default_broker=override && *override?ctx_broker_open(override):
            g_shared_vfs?ctx_broker_open_vfs(g_shared_vfs):ctx_broker_open(NULL);
    }
    ctx_broker_t *b = g_default_broker;
    pthread_mutex_unlock(&g_default_mu);
    return b;
}

void ctx_broker_shutdown(void) {
    pthread_mutex_lock(&g_default_mu);
    ctx_broker_t *b=g_default_broker;g_default_broker=NULL;g_shared_vfs=NULL;
    pthread_mutex_unlock(&g_default_mu);
    ctx_broker_close(b);
}

/* Embedding payload limits are per request, not a license to ignore the
 * document tail. Pool all UTF-8-safe chunks; reject partial or mismatched
 * results so lexical search remains the complete fallback. */
static float *ctx_embed_fulltext(const char *text,size_t bytes,int *out_dim) {
    *out_dim=0;float *mean=NULL;int dimension=0;
    for(size_t start=0;start<bytes;) {
        size_t count=bytes-start;if(count>CTX_EMBED_MAX_BYTES)count=CTX_EMBED_MAX_BYTES;
        while(count && start+count<bytes && ((unsigned char)text[start+count]&0xc0)==0x80)count--;
        if(!count){free(mean);return NULL;}
        char *chunk=malloc(count+1);if(!chunk){free(mean);return NULL;}
        memcpy(chunk,text+start,count);chunk[count]=0;
        int dim=0;float *part=tools_embed_text(chunk,&dim);free(chunk);
        if(!part || dim<=0 || (dimension && dim!=dimension)){free(part);free(mean);return NULL;}
        if(!mean){dimension=dim;mean=calloc((size_t)dim,sizeof(float));if(!mean){free(part);return NULL;}}
        for(int i=0;i<dim;i++) {
            if(!isfinite(part[i])){free(part);free(mean);return NULL;}
            mean[i]+=part[i]*(float)((double)count/(double)bytes);
        }
        free(part);start+=count;
    }
    *out_dim=dimension;return mean;
}

/* ── put ──────────────────────────────────────────────────────────────────*/

bool ctx_put(ctx_broker_t *b, const void *bytes, size_t n, const ctx_put_opts_t *opts,
             ctxkey_t *out_key, bool *out_deduped) {
    if (!b || !bytes || !out_key)
        return false;
    ctx_kind_t kind = opts ? opts->kind : CTX_KIND_BLOB;
    const char *source = opts ? opts->source : NULL;
    const char *tags = opts ? opts->tags : NULL;
    double importance = (opts && opts->importance > 0.0) ? opts->importance : 0.5;
    int embed_mode = opts ? opts->embed : -1;

    char cid[65];
    sha256_hex(bytes, n, cid);

    memset(out_key, 0, sizeof(*out_key));
    out_key->kind = kind;
    snprintf(out_key->id, sizeof(out_key->id), "%s", cid);
    out_key->slice_kind = CTX_SLICE_NONE;

    pthread_mutex_lock(&b->mu);

    /* A metadata row alone does not prove durable content. */
    char *existing=vfs_kv_get_str(b->vfs,CTX_BUCKET_META,cid);
    if(existing) {
        size_t stored_len=0;void *stored=vfs_kv_get(b->vfs,CTX_BUCKET_BLOB,cid,&stored_len);
        bool intact=stored && stored_len==n && !memcmp(stored,bytes,n);
        free(stored);free(existing);
        if(intact) {
            if(out_deduped)*out_deduped=true;
            pthread_mutex_unlock(&b->mu);return true;
        }
    }
    if (out_deduped)
        *out_deduped = false;

    bool ok = vfs_kv_put(b->vfs, CTX_BUCKET_BLOB, cid, bytes, n);
    if (ok) {
        char preview[CTX_PREVIEW_BYTES + 8];
        ctx_make_preview(bytes, n, preview, sizeof(preview));
        char *meta = ctx_build_meta_json(kind, source, tags, importance, n, preview);
        if (meta) {
            ok=vfs_kv_put_str(b->vfs, CTX_BUCKET_META, cid, meta);
            /* lexical index: full document body, real BM25 ranking. Text
             * only — indexing binary bytes as FTS text is meaningless and
             * wastes the index. */
            if (b->fts && ctx_is_textual(bytes, n)) {
                size_t flen = n; /* lexical search covers the entire stored text */
                char *ftext = malloc(flen + 1);
                if (ftext) {
                    memcpy(ftext, bytes, flen);
                    ftext[flen] = '\0';
                    if(!ctx_fts_index(b->fts,cid,source,tags,ftext))b->reindex_failures++;
                    free(ftext);
                } else b->reindex_failures++;
            }
            /* semantic index: embed textual content unless disabled */
            bool want_embed = (embed_mode == 1) || (embed_mode != 0 && ctx_is_textual(bytes, n));
            if (want_embed && b->vec) {
                /* Provider latency must not prevent independent context reads. */
                pthread_mutex_unlock(&b->mu);
                int dim=0;float *vec=ctx_embed_fulltext(bytes,n,&dim);
                pthread_mutex_lock(&b->mu);
                if(vec && dim>0)vecstore_insert(b->vec,cid,vec,dim,meta);
                free(vec);
            }
            free(meta);
        } else ok=false;
    }
    pthread_mutex_unlock(&b->mu);
    return ok;
}

/* ── get (with slice) ─────────────────────────────────────────────────────*/

static char *ctx_apply_slice(char *buf, size_t len, const ctxkey_t *key, size_t *out_len) {
    if (key->slice_kind == CTX_SLICE_NONE) {
        *out_len = len;
        return buf;
    }
    if (key->slice_kind == CTX_SLICE_BYTES) {
        long start = key->slice_start < 0 ? 0 : key->slice_start;
        long end = (key->slice_end < 0 || (size_t)key->slice_end > len) ? (long)len : key->slice_end;
        if (start > (long)len)
            start = (long)len;
        if (end < start)
            end = start;
        size_t span = (size_t)(end - start);
        char *slice = malloc(span + 1);
        if (!slice) {
            *out_len = 0;
            free(buf);return NULL; /* never expand a requested slice on OOM */
        }
        memcpy(slice, buf + start, span);
        slice[span] = '\0';
        free(buf);
        *out_len = span;
        return slice;
    }
    /* CTX_SLICE_LINES — 1-based inclusive */
    long want_start = key->slice_start < 1 ? 1 : key->slice_start;
    long want_end = key->slice_end; /* <0 means to end */
    long line = 1;
    size_t i = 0, span_start = 0, span_end = len;
    bool found_start = (want_start == 1);
    if (found_start)
        span_start = 0;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            line++;
            if (!found_start && line == want_start) {
                span_start = i + 1;
                found_start = true;
            }
            if (want_end >= 0 && line == want_end + 1) {
                span_end = i + 1;
                break;
            }
        }
    }
    if (!found_start) {
        char *empty = calloc(1, 1);
        free(buf);
        *out_len = 0;
        return empty;
    }
    size_t span = span_end - span_start;
    char *slice = malloc(span + 1);
    if (!slice) {
        *out_len = 0;
        free(buf);return NULL;
    }
    memcpy(slice, buf + span_start, span);
    slice[span] = '\0';
    free(buf);
    *out_len = span;
    return slice;
}

char *ctx_get(ctx_broker_t *b, const ctxkey_t *key, size_t *out_len) {
    if (!b || !key)
        return NULL;
    size_t len = 0;
    pthread_mutex_lock(&b->mu);
    char *buf = vfs_kv_get(b->vfs, CTX_BUCKET_BLOB, key->id, &len);
    pthread_mutex_unlock(&b->mu);
    if (!buf)
        return NULL;
    bool hashed=strlen(key->id)==64;
    for(size_t i=0;hashed && i<64;i++)if(!isxdigit((unsigned char)key->id[i]))hashed=false;
    if(hashed) {
        char actual[65];sha256_hex((const uint8_t *)buf,len,actual);
        if(strcmp(actual,key->id)){free(buf);if(out_len)*out_len=0;return NULL;}
    }
    /* ensure NUL-terminated for text callers (vfs returns raw bytes) */
    char *nt = realloc(buf, len + 1);
    if (!nt) {
        free(buf);
        return NULL;
    }
    nt[len] = '\0';
    size_t final = 0;
    char *result = ctx_apply_slice(nt, len, key, &final);
    if (out_len)
        *out_len = final;
    return result;
}

/* ── search ───────────────────────────────────────────────────────────────*/

static void ctx_hit_from_meta(ctx_search_hit_t *hit, const char *cid, const char *meta, float score) {
    memset(hit, 0, sizeof(*hit));
    hit->key.kind = CTX_KIND_BLOB;
    snprintf(hit->key.id, sizeof(hit->key.id), "%s", cid);
    hit->key.slice_kind = CTX_SLICE_NONE;
    hit->score = score;
    if (meta) {
        hit->key.kind = (ctx_kind_t)json_get_int(meta, "kind", CTX_KIND_BLOB);
        hit->size = (size_t)json_get_i64(meta, "size", 0);
        char *src = json_get_str(meta, "source");
        char *prev = json_get_str(meta, "preview");
        if (src) {
            snprintf(hit->source, sizeof(hit->source), "%s", src);
            free(src);
        }
        if (prev) {
            snprintf(hit->preview, sizeof(hit->preview), "%s", prev);
            free(prev);
        }
    }
}

/* Case-insensitive substring test with no intermediate buffer, so neither the
 * needle nor the haystack can be silently truncated by a fixed-size copy. */
static bool ctx_ci_contains(const char *hay, const char *needle) {
    if (!needle || !*needle)
        return true;
    if (!hay)
        return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *n = needle;
        while (*a && *n && tolower((unsigned char)*a) == tolower((unsigned char)*n)) {
            a++;
            n++;
        }
        if (!*n)
            return true;
    }
    return false;
}

static int ctx_search_lexical(ctx_broker_t *b, const char *query, int kind_filter, int max,
                              ctx_search_hit_t *out) {
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_META, &kcount);
    if (!keys)
        return 0;

    int found = 0;
    for (int i = 0; i < kcount && found < max; i++) {
        char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, keys[i]);
        if (!meta)
            continue;
        if (kind_filter >= 0 && json_get_int(meta, "kind", 0) != kind_filter) {
            free(meta);
            continue;
        }
        /* Match the full query against each metadata field independently — no
         * fixed-size concatenation, so neither query nor field is truncated. */
        char *src = json_get_str(meta, "source");
        char *tags = json_get_str(meta, "tags");
        char *prev = json_get_str(meta, "preview");
        bool hit = ctx_ci_contains(src, query) || ctx_ci_contains(tags, query) ||
                   ctx_ci_contains(prev, query);
        free(src);
        free(tags);
        free(prev);
        if (!hit) {
            ctxkey_t key={.kind=CTX_KIND_BLOB};
            snprintf(key.id,sizeof(key.id),"%s",keys[i]);
            size_t bytes=0;char *body=vfs_kv_get(b->vfs,CTX_BUCKET_BLOB,key.id,&bytes);
            bool valid=body!=NULL;
            if(valid && strlen(key.id)==64) {
                char hash[65];sha256_hex((const uint8_t *)body,bytes,hash);valid=!strcmp(hash,key.id);
            }
            if(valid && ctx_is_textual(body,bytes) && !memchr(body,0,bytes)) {
                char *terminated=realloc(body,bytes+1);
                if(terminated){body=terminated;body[bytes]=0;hit=ctx_ci_contains(body,query);}
            }
            free(body);
        }
        if (hit) {
            ctx_hit_from_meta(&out[found], keys[i], meta, 0.5f);
            found++;
        }
        free(meta);
    }
    for (int i = 0; i < kcount; i++)
        free(keys[i]);
    free(keys);
    return found;
}

int ctx_search(ctx_broker_t *b, const char *query, int kind_filter, int max, ctx_search_hit_t *out) {
    if (!b || !out || max <= 0)
        return 0;
    if (max > 64)
        max = 64;

    /* Provider work runs before taking the storage mutex. Broker lifetime
     * remains owned by the caller, as with every operation on this handle. */
    int query_dim=0;
    float *qvec=(b->vec && query && *query)?tools_embed_text(query,&query_dim):NULL;
    pthread_mutex_lock(&b->mu);

    /* ── Lane 1: semantic (vecstore cosine) ── */
    char sem_keys[192][128];
    int sem_n = 0;
    {
        if (qvec && query_dim > 0 && b->vec) {
            int cap = 192;
            vecstore_result_t *res = calloc((size_t)cap, sizeof(*res));
            int n = res ? vecstore_query(b->vec, qvec, query_dim, res, cap) : 0;
            for (int i = 0; i < n && sem_n < 192; i++) {
                if (!res[i].id)
                    continue;
                snprintf(sem_keys[sem_n], 128, "%s", res[i].id);
                sem_n++;
            }
            if (res)
                vecstore_result_free(res, n);
            free(res);
        }
        free(qvec);
    }

    /* ── Lane 2: lexical (FTS5 BM25) ── */
    char lex_keys[64][128];
    float lex_scores[64];
    char lex_snip[64][256];
    int lex_n = b->fts && query && *query
                    ? ctx_fts_search(b->fts, query, 64, lex_keys, lex_scores, lex_snip)
                    : 0;

    /* ── Reciprocal Rank Fusion, k=60 (standard constant; damps the winner-
     * take-all effect of a top-1 hit in either lane while still rewarding
     * consensus). Either lane may be empty — RRF degrades to the surviving
     * lane's own ranking, which is exactly what "hybrid with graceful
     * degradation" needs (no embeddings configured? BM25 alone. No FTS5?
     * semantic alone. Neither? the full-body scan below.) */
    typedef struct { char key[128]; double rrf; int lex_idx; } fused_t;
    fused_t fused[256];
    int fn = 0;
    for (int i = 0; i < sem_n; i++) {
        snprintf(fused[fn].key, 128, "%s", sem_keys[i]);
        fused[fn].rrf = 1.0 / (60.0 + i + 1);
        fused[fn].lex_idx = -1;
        fn++;
    }
    for (int i = 0; i < lex_n && fn < 256; i++) {
        int existing = -1;
        for (int j = 0; j < fn; j++)
            if (strcmp(fused[j].key, lex_keys[i]) == 0) {
                existing = j;
                break;
            }
        if (existing >= 0) {
            fused[existing].rrf += 1.0 / (60.0 + i + 1);
            fused[existing].lex_idx = i;
        } else {
            snprintf(fused[fn].key, 128, "%s", lex_keys[i]);
            fused[fn].rrf = 1.0 / (60.0 + i + 1);
            fused[fn].lex_idx = i;
            fn++;
        }
    }
    /* insertion sort: fn is small (<=256), stable, no qsort comparator noise */
    for (int i = 1; i < fn; i++) {
        fused_t key = fused[i];
        int j = i - 1;
        while (j >= 0 && fused[j].rrf < key.rrf) {
            fused[j + 1] = fused[j];
            j--;
        }
        fused[j + 1] = key;
    }

    int found = 0;
    for (int i = 0; i < fn && found < max; i++) {
        char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, fused[i].key);
        if (kind_filter >= 0 && meta && json_get_int(meta, "kind", 0) != kind_filter) {
            free(meta);
            continue;
        }
        ctx_hit_from_meta(&out[found], fused[i].key, meta, (float)fused[i].rrf);
        free(meta);
        found++;
    }

    if (found > 0) {
        pthread_mutex_unlock(&b->mu);
        return found;
    }

    /* Both real lanes empty (no embeddings, no FTS5, or truly no match):
     * search full textual bodies, including objects beyond the old scan cap. */
    found = ctx_search_lexical(b, query, kind_filter, max, out);
    pthread_mutex_unlock(&b->mu);
    return found;
}

/* ── pin ──────────────────────────────────────────────────────────────────*/

bool ctx_pin(ctx_broker_t *b, const ctxkey_t *key) {
    if (!b || !key)
        return false;
    char ks[CTX_KEY_STR_MAX];
    if (ctxkey_format(key, ks, sizeof(ks)) < 0)
        return false;
    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_put_str(b->vfs, CTX_BUCKET_PIN, ks, "1");
    pthread_mutex_unlock(&b->mu);
    return ok;
}

bool ctx_unpin(ctx_broker_t *b, const ctxkey_t *key) {
    if (!b || !key)
        return false;
    char ks[CTX_KEY_STR_MAX];
    if (ctxkey_format(key, ks, sizeof(ks)) < 0)
        return false;
    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_delete(b->vfs, CTX_BUCKET_PIN, ks);
    pthread_mutex_unlock(&b->mu);
    return ok;
}

int ctx_pinned_list(ctx_broker_t *b, ctxkey_t *out, int max) {
    if (!b || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&b->mu);
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_PIN, &kcount);
    int found = 0;
    for (int i = 0; i < kcount && found < max; i++) {
        if (keys[i] && ctxkey_parse(keys[i], &out[found]))
            found++;
    }
    for (int i = 0; i < kcount; i++)
        free(keys[i]);
    free(keys);
    pthread_mutex_unlock(&b->mu);
    return found;
}

/* ── scope ──────────────────────────────────────────────────────────────── */

bool ctx_scope_create(ctx_broker_t *b, const ctxkey_t *keys, int n, ctxkey_t *out_scope) {
    if (!b || !keys || n <= 0 || !out_scope)
        return false;
    /* Build a JSON array of member key strings; scope id = sha256 of the array. */
    jbuf_t arr;
    jbuf_init(&arr, 256);
    jbuf_append(&arr, "[");
    for (int i = 0; i < n; i++) {
        char ks[CTX_KEY_STR_MAX];
        if (ctxkey_format(&keys[i], ks, sizeof(ks)) < 0) {
            jbuf_free(&arr);
            return false;
        }
        if (i)
            jbuf_append(&arr, ",");
        jbuf_append_json_str(&arr, ks);
    }
    jbuf_append(&arr, "]");
    if (!arr.data)
        return false;

    char sid[65];
    sha256_hex((const uint8_t *)arr.data, strlen(arr.data), sid);

    memset(out_scope, 0, sizeof(*out_scope));
    out_scope->kind = CTX_KIND_SCOPE;
    snprintf(out_scope->id, sizeof(out_scope->id), "%s", sid);
    out_scope->slice_kind = CTX_SLICE_NONE;

    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_put_str(b->vfs, CTX_BUCKET_SCOPE, sid, arr.data);
    pthread_mutex_unlock(&b->mu);
    jbuf_free(&arr);
    return ok;
}

int ctx_scope_resolve(ctx_broker_t *b, const ctxkey_t *scope, ctxkey_t *out, int max) {
    if (!b || !scope || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&b->mu);
    char *arr = vfs_kv_get_str(b->vfs, CTX_BUCKET_SCOPE, scope->id);
    pthread_mutex_unlock(&b->mu);
    if (!arr)
        return 0;
    /* Parse the JSON array of key strings. Members are simple quoted strings
     * with no embedded quotes (ctxkeys are [a-z0-9:#-]), so a linear scan is
     * sufficient and avoids a full JSON parse. */
    int found = 0;
    const char *p = arr;
    while (found < max && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        if (!end)
            break;
        char ks[CTX_KEY_STR_MAX];
        size_t klen = (size_t)(end - p);
        if (klen < sizeof(ks)) {
            memcpy(ks, p, klen);
            ks[klen] = '\0';
            if (ctxkey_parse(ks, &out[found]))
                found++;
        }
        p = end + 1;
    }
    free(arr);
    return found;
}

/* ── stats ──────────────────────────────────────────────────────────────── */

ctx_stat_t ctx_broker_stat(ctx_broker_t *b) {
    ctx_stat_t st;
    memset(&st, 0, sizeof(st));
    if (!b)
        return st;
    pthread_mutex_lock(&b->mu);
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_META, &kcount);
    st.blobs = kcount;
    for (int i = 0; i < kcount; i++) {
        char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, keys[i]);
        if (meta) {
            st.bytes += json_get_i64(meta, "size", 0);
            free(meta);
        }
        free(keys[i]);
    }
    free(keys);
    int pcount = 0;
    char **pins = vfs_kv_keys(b->vfs, CTX_BUCKET_PIN, &pcount);
    st.pinned = pcount;
    for (int i = 0; i < pcount; i++)
        free(pins[i]);
    free(pins);
    int scount = 0;
    char **scopes = vfs_kv_keys(b->vfs, CTX_BUCKET_SCOPE, &scount);
    st.scopes = scount;
    for (int i = 0; i < scount; i++)
        free(scopes[i]);
    free(scopes);
    st.vectors = b->vec ? vecstore_count(b->vec) : 0;
    st.embeddings_available = (b->vec != NULL);
    pthread_mutex_unlock(&b->mu);
    return st;
}
