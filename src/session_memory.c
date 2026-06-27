#include "session_memory.h"
#include "json_util.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int g_sess_counter = 0;

static void make_session_id(char *buf, size_t len) {
    snprintf(buf, len, "sess_%lld_%d", (long long)now_sec(), g_sess_counter++);
}

static memory_tier_t tier_from_ttl(int ttl) {
    if (ttl == 0)
        return MEM_SEMANTIC;
    if (ttl <= SESSION_TTL_WORKING)
        return MEM_WORKING;
    return MEM_EPISODIC;
}

static bool session_ttl_expiry_enabled(void) {
    const char *e = getenv("DSCO_SESSION_TTL_EXPIRY");
    if (!e || !e[0])
        return false;
    if (e[0] == '1')
        return true;
    char tmp[16];
    size_t i = 0;
    for (; e[i] && i + 1 < sizeof(tmp); i++)
        tmp[i] = (char)tolower((unsigned char)e[i]);
    tmp[i] = '\0';
    return strcmp(tmp, "true") == 0 || strcmp(tmp, "yes") == 0;
}

static const char *tier_name(memory_tier_t t) {
    switch (t) {
        case MEM_WORKING:
            return "working";
        case MEM_EPISODIC:
            return "episodic";
        case MEM_SEMANTIC:
            return "semantic";
        default:
            return "episodic";
    }
}

static memory_tier_t tier_from_name(const char *name) {
    if (!name)
        return MEM_EPISODIC;
    if (strcmp(name, "working") == 0)
        return MEM_WORKING;
    if (strcmp(name, "semantic") == 0)
        return MEM_SEMANTIC;
    return MEM_EPISODIC;
}

/* Resolve the on-disk path.  $DSCO_SESSION_PATH overrides the default. */
static void build_db_path(char *buf, size_t len) {
    const char *override = getenv("DSCO_SESSION_PATH");
    if (override && *override) {
        snprintf(buf, len, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(buf, len, "%s/.dsco/memory/sessions.json", home);
}

/* mkdir -p for a path that ends with a filename: creates parent dirs. */
static void ensure_parent_dirs(const char *file_path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return;
    *slash = '\0';
    /* Walk and create each component */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0o700);
            *p = '/';
        }
    }
    mkdir(tmp, 0o700);
}

/* ── JSON serialization ────────────────────────────────────────────────── */

static void serialize_db(const session_db_t *db, jbuf_t *out) {
    jbuf_append(out, "{\"version\":1,\"kv\":[");
    bool first = true;
    for (int i = 0; i < db->kv_count; i++) {
        const session_kv_t *kv = &db->kv[i];
        if (!kv->active)
            continue;
        if (!first)
            jbuf_append(out, ",");
        first = false;
        jbuf_append(out, "{\"key\":");
        jbuf_append_json_str(out, kv->key);
        jbuf_append(out, ",\"value\":");
        jbuf_append_json_str(out, kv->value);
        jbuf_appendf(out,
                     ",\"tier\":\"%s\",\"ttl\":%d"
                     ",\"created_at\":%.3f,\"accessed_at\":%.3f"
                     ",\"access_count\":%d}",
                     tier_name(kv->tier), kv->ttl_seconds, kv->created_at, kv->accessed_at,
                     kv->access_count);
    }
    jbuf_append(out, "],\"sessions\":[");
    first = true;
    for (int i = 0; i < db->record_count; i++) {
        const session_record_t *rec = &db->records[i];
        if (!rec->active)
            continue;
        if (!first)
            jbuf_append(out, ",");
        first = false;
        jbuf_append(out, "{\"id\":");
        jbuf_append_json_str(out, rec->id);
        jbuf_append(out, ",\"task\":");
        jbuf_append_json_str(out, rec->task_text);
        jbuf_append(out, ",\"kv\":");
        jbuf_append_json_str(out, rec->key_values);
        jbuf_appendf(out,
                     ",\"tier\":\"%s\",\"created_at\":%.3f"
                     ",\"accessed_at\":%.3f,\"access_count\":%d}",
                     tier_name(rec->tier), rec->created_at, rec->accessed_at, rec->access_count);
    }
    jbuf_append(out, "]}");
}

/* ── JSON deserialization ──────────────────────────────────────────────── */

typedef struct {
    session_db_t *db;
} parse_ctx_t;

static void parse_kv_entry(const char *elem, void *ctx) {
    session_db_t *db = ((parse_ctx_t *)ctx)->db;
    if (db->kv_count >= SESSION_MAX_KV)
        return;

    /* Find inactive slot or extend array */
    int slot = -1;
    for (int i = 0; i < db->kv_count; i++) {
        if (!db->kv[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        slot = db->kv_count++;

    session_kv_t *kv = &db->kv[slot];
    memset(kv, 0, sizeof(*kv));

    char *key = json_get_str(elem, "key");
    char *val = json_get_str(elem, "value");
    char *tier = json_get_str(elem, "tier");
    if (!key || !val) {
        free(key);
        free(val);
        free(tier);
        db->kv_count--;
        return;
    }

    snprintf(kv->key, sizeof(kv->key), "%s", key);
    snprintf(kv->value, sizeof(kv->value), "%s", val);
    kv->tier = tier_from_name(tier);
    kv->ttl_seconds = json_get_int(elem, "ttl", 0);
    kv->created_at = json_get_double(elem, "created_at", now_sec());
    kv->accessed_at = json_get_double(elem, "accessed_at", now_sec());
    kv->access_count = json_get_int(elem, "access_count", 1);
    kv->active = true;

    free(key);
    free(val);
    free(tier);
}

static void parse_session_entry(const char *elem, void *ctx) {
    session_db_t *db = ((parse_ctx_t *)ctx)->db;
    if (db->record_count >= SESSION_MAX_RECORDS)
        return;

    int slot = -1;
    for (int i = 0; i < db->record_count; i++) {
        if (!db->records[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        slot = db->record_count++;

    session_record_t *rec = &db->records[slot];
    memset(rec, 0, sizeof(*rec));

    char *id = json_get_str(elem, "id");
    char *task = json_get_str(elem, "task");
    char *kv = json_get_str(elem, "kv");
    char *tier = json_get_str(elem, "tier");
    if (!id || !task) {
        free(id);
        free(task);
        free(kv);
        free(tier);
        db->record_count--;
        return;
    }

    snprintf(rec->id, sizeof(rec->id), "%s", id);
    snprintf(rec->task_text, sizeof(rec->task_text), "%s", task);
    snprintf(rec->key_values, sizeof(rec->key_values), "%s", kv ? kv : "{}");
    rec->tier = tier_from_name(tier);
    rec->created_at = json_get_double(elem, "created_at", now_sec());
    rec->accessed_at = json_get_double(elem, "accessed_at", now_sec());
    rec->access_count = json_get_int(elem, "access_count", 1);
    rec->active = true;

    free(id);
    free(task);
    free(kv);
    free(tier);
}

static int load_db(session_db_t *db) {
    FILE *f = fopen(db->db_path, "rb");
    if (!f)
        return 0; /* no file yet — fresh DB */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        fclose(f);
        return sz <= 0 ? 0 : -1;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    parse_ctx_t pctx = {.db = db};
    json_array_foreach(buf, "kv", parse_kv_entry, &pctx);
    json_array_foreach(buf, "sessions", parse_session_entry, &pctx);
    free(buf);
    return 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

int session_init(session_db_t *db, const char *task_text) {
    memset(db, 0, sizeof(*db));
    build_db_path(db->db_path, sizeof(db->db_path));
    ensure_parent_dirs(db->db_path);

    db->initialized = true;

    if (load_db(db) < 0) {
        db->initialized = false;
        return -1;
    }

    /* TTL expiry used to erase working context between pauses, which made
     * long-running interactive sessions feel like they were blanking out.
     * Retain by default; operators can opt back into TTL GC with
     * DSCO_SESSION_TTL_EXPIRY=1. */
    session_evict_expired(db);

    /* Register the current session */
    make_session_id(db->current_session_id, sizeof(db->current_session_id));

    if (db->record_count < SESSION_MAX_RECORDS) {
        int slot = db->record_count++;
        session_record_t *rec = &db->records[slot];
        memset(rec, 0, sizeof(*rec));
        snprintf(rec->id, sizeof(rec->id), "%s", db->current_session_id);
        if (task_text)
            snprintf(rec->task_text, sizeof(rec->task_text), "%s", task_text);
        snprintf(rec->key_values, sizeof(rec->key_values), "{}");
        rec->created_at = now_sec();
        rec->accessed_at = rec->created_at;
        rec->access_count = 1;
        rec->tier = MEM_EPISODIC;
        rec->active = true;
    }

    db->dirty = false;
    return 0;
}

void session_free(session_db_t *db) {
    memset(db, 0, sizeof(*db));
}

int session_flush(session_db_t *db) {
    return session_persist(db);
}

/* ── KV store ──────────────────────────────────────────────────────────── */

static session_kv_t *find_kv(session_db_t *db, const char *key) {
    for (int i = 0; i < db->kv_count; i++) {
        if (db->kv[i].active && strcmp(db->kv[i].key, key) == 0)
            return &db->kv[i];
    }
    return NULL;
}

int session_remember(session_db_t *db, const char *key, const char *value, int ttl_seconds) {
    if (!db || !db->initialized || !key || !value)
        return -1;

    session_kv_t *kv = find_kv(db, key);
    if (kv) {
        snprintf(kv->value, sizeof(kv->value), "%s", value);
        kv->ttl_seconds = ttl_seconds;
        kv->tier = tier_from_ttl(ttl_seconds);
        kv->accessed_at = now_sec();
        kv->access_count++;
        db->dirty = true;
        return 0;
    }

    /* Evict expired entries to make room if needed */
    if (db->kv_count >= SESSION_MAX_KV)
        session_evict_expired(db);
    if (db->kv_count >= SESSION_MAX_KV)
        return -1;

    /* Find a free slot (inactive gap) or append */
    int slot = -1;
    for (int i = 0; i < db->kv_count; i++) {
        if (!db->kv[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        slot = db->kv_count++;

    session_kv_t *nkv = &db->kv[slot];
    memset(nkv, 0, sizeof(*nkv));
    snprintf(nkv->key, sizeof(nkv->key), "%s", key);
    snprintf(nkv->value, sizeof(nkv->value), "%s", value);
    nkv->tier = tier_from_ttl(ttl_seconds);
    nkv->ttl_seconds = ttl_seconds;
    nkv->created_at = now_sec();
    nkv->accessed_at = nkv->created_at;
    nkv->access_count = 1;
    nkv->active = true;
    db->dirty = true;
    return 0;
}

const char *session_recall(session_db_t *db, const char *key) {
    if (!db || !db->initialized || !key)
        return NULL;

    session_kv_t *kv = find_kv(db, key);
    if (!kv)
        return NULL;

    /* Check TTL expiry only when explicitly enabled. Default behavior is
     * retention: TTL still classifies memory tiers, but does not erase context
     * after a wall-clock timeout. */
    if (session_ttl_expiry_enabled() && kv->ttl_seconds > 0) {
        double age = now_sec() - kv->created_at;
        if (age > (double)kv->ttl_seconds) {
            kv->active = false;
            db->dirty = true;
            return NULL;
        }
    }

    kv->accessed_at = now_sec();
    kv->access_count++;
    db->dirty = true;
    return kv->value;
}

/* ── Session history ───────────────────────────────────────────────────── */

/* Tokenize `text` into unique lowercase alphanumeric words.
   words[][32] must have room for max_words entries.
   Returns word count. */
static int tokenize_words(const char *text, char words[][32], int max_words) {
    int n = 0;
    const char *p = text;
    while (*p && n < max_words) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen == 0 || wlen > 31)
            continue;

        char word[32] = {0};
        for (size_t i = 0; i < wlen; i++)
            word[i] = (char)tolower((unsigned char)start[i]);

        /* skip if already seen */
        bool dup = false;
        for (int i = 0; i < n && !dup; i++)
            dup = (strcmp(words[i], word) == 0);
        if (!dup)
            snprintf(words[n++], 32, "%s", word);
    }
    return n;
}

/* Jaccard similarity on word sets of a and b. */
static double jaccard_similarity(const char *a, const char *b) {
    char wa[64][32], wb[64][32];
    int na = tokenize_words(a, wa, 64);
    int nb = tokenize_words(b, wb, 64);
    if (na == 0 && nb == 0)
        return 1.0;
    if (na == 0 || nb == 0)
        return 0.0;

    int inter = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (strcmp(wa[i], wb[j]) == 0) {
                inter++;
                break;
            }

    int uni = na + nb - inter;
    return uni > 0 ? (double)inter / (double)uni : 0.0;
}

int session_lookup_similar(session_db_t *db, const char *task, const session_record_t **out,
                           int k) {
    if (!db || !db->initialized || !task || !out || k <= 0)
        return 0;

    typedef struct {
        double score;
        int idx;
    } scored_t;
    scored_t scored[SESSION_MAX_RECORDS];
    int n = 0;

    for (int i = 0; i < db->record_count; i++) {
        const session_record_t *rec = &db->records[i];
        if (!rec->active)
            continue;
        if (strcmp(rec->id, db->current_session_id) == 0)
            continue;

        double s = jaccard_similarity(task, rec->task_text);
        if (s > 0.0) {
            scored[n].score = s;
            scored[n].idx = i;
            n++;
        }
    }

    /* Insertion-sort descending by score (N is small) */
    for (int i = 1; i < n; i++) {
        scored_t tmp = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < tmp.score) {
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = tmp;
    }

    int count = n < k ? n : k;
    for (int i = 0; i < count; i++)
        out[i] = &db->records[scored[i].idx];
    return count;
}

/* ── Tier promotion ────────────────────────────────────────────────────── */

int session_promote(session_db_t *db) {
    if (!db || !db->initialized)
        return 0;

    int promoted = 0;
    for (int i = 0; i < db->kv_count; i++) {
        session_kv_t *kv = &db->kv[i];
        if (!kv->active || kv->tier == MEM_SEMANTIC)
            continue;

        if (kv->access_count < SESSION_PROMOTE_THRESHOLD)
            continue;

        if (kv->tier == MEM_WORKING) {
            kv->tier = MEM_EPISODIC;
            kv->ttl_seconds = SESSION_TTL_EPISODIC;
        } else if (kv->tier == MEM_EPISODIC) {
            kv->tier = MEM_SEMANTIC;
            kv->ttl_seconds = SESSION_TTL_SEMANTIC;
        }
        promoted++;
        db->dirty = true;
    }
    return promoted;
}

/* ── Persistence ───────────────────────────────────────────────────────── */

/* Refresh the current session record's key_values snapshot. */
static void snapshot_current_session(session_db_t *db) {
    for (int i = 0; i < db->record_count; i++) {
        session_record_t *rec = &db->records[i];
        if (strcmp(rec->id, db->current_session_id) != 0)
            continue;

        jbuf_t snap;
        jbuf_init(&snap, 512);
        jbuf_append(&snap, "{");
        bool first = true;
        for (int j = 0; j < db->kv_count; j++) {
            const session_kv_t *kv = &db->kv[j];
            if (!kv->active)
                continue;
            if (!first)
                jbuf_append(&snap, ",");
            first = false;
            jbuf_append_json_str(&snap, kv->key);
            jbuf_append(&snap, ":");
            jbuf_append_json_str(&snap, kv->value);
        }
        jbuf_append(&snap, "}");
        snprintf(rec->key_values, sizeof(rec->key_values), "%s", snap.data);
        jbuf_free(&snap);
        rec->accessed_at = now_sec();
        break;
    }
}

int session_persist(session_db_t *db) {
    if (!db || !db->initialized)
        return -1;

    snapshot_current_session(db);

    jbuf_t buf;
    jbuf_init(&buf, 4096);
    serialize_db(db, &buf);

    /* Atomic write: write to .tmp then rename */
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", db->db_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        jbuf_free(&buf);
        return -1;
    }

    size_t written = fwrite(buf.data, 1, buf.len, f);
    fclose(f);
    jbuf_free(&buf);

    if (written == 0) {
        remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, db->db_path) != 0) {
        remove(tmp_path);
        return -1;
    }

    db->dirty = false;
    return 0;
}

int session_end(session_db_t *db) {
    if (!db || !db->initialized)
        return -1;
    session_promote(db);
    return session_persist(db);
}

int session_evict_expired(session_db_t *db) {
    if (!db || !db->initialized)
        return 0;
    if (!session_ttl_expiry_enabled())
        return 0;

    double t = now_sec();
    int evicted = 0;
    for (int i = 0; i < db->kv_count; i++) {
        session_kv_t *kv = &db->kv[i];
        if (!kv->active)
            continue;
        if (kv->ttl_seconds > 0 && (t - kv->created_at) > (double)kv->ttl_seconds) {
            kv->active = false;
            evicted++;
            db->dirty = true;
        }
    }
    return evicted;
}

/* ── Diagnostics ───────────────────────────────────────────────────────── */

int session_status_json(const session_db_t *db, char *buf, size_t len) {
    if (!db || !buf)
        return 0;

    int kv_active = 0, rec_active = 0;
    int by_tier[MEM_TIER_COUNT] = {0};

    for (int i = 0; i < db->kv_count; i++) {
        if (!db->kv[i].active)
            continue;
        kv_active++;
        if ((int)db->kv[i].tier < MEM_TIER_COUNT)
            by_tier[(int)db->kv[i].tier]++;
    }
    for (int i = 0; i < db->record_count; i++) {
        if (db->records[i].active)
            rec_active++;
    }

    return snprintf(buf, len,
                    "{\"kv_entries\":%d,\"session_records\":%d,"
                    "\"working\":%d,\"episodic\":%d,\"semantic\":%d,"
                    "\"current_session\":\"%s\",\"dirty\":%s}",
                    kv_active, rec_active, by_tier[MEM_WORKING], by_tier[MEM_EPISODIC],
                    by_tier[MEM_SEMANTIC], db->current_session_id, db->dirty ? "true" : "false");
}
