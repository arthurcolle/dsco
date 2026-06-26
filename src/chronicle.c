#include "chronicle.h"
#include "json_util.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CHRONICLE_SCHEMA_VERSION "chronicle.v1"

/* Small embedded SHA-256 implementation (public-domain style primitives).
 * Used for content addressing and tamper-evident event payload hashes without
 * adding a new dependency. */
typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[]) {
    uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    for (; i < 64; ++i) m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e,f,g) + k256[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t data[], size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }
}

static void sha256_hex(const void *data, size_t len, char out[65]) {
    uint8_t hash[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (data && len) sha256_update(&ctx, (const uint8_t *)data, len);
    sha256_final(&ctx, hash);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2] = hexd[hash[i] >> 4];
        out[i*2+1] = hexd[hash[i] & 15];
    }
    out[64] = '\0';
}

typedef struct {
    sqlite3 *db;
    FILE *events_fp;
    bool ready;
    chronicle_mode_t mode;
    char root[PATH_MAX];
    char db_path[PATH_MAX];
    char event_log_path[PATH_MAX];
    char installation_id[37];
    char session_id[37];
    char instance_id[128];
    unsigned long long seq;
    char prev_event_hash[65];
} chronicle_state_t;

static chronicle_state_t g_chronicle = {0};

static const char *nz(const char *s) { return s ? s : ""; }

static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool ensure_parent_dir(const char *file_path) {
    char tmp[PATH_MAX];
    size_t n = strlen(file_path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, file_path, n + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    return tmp[0] ? mkdir_p(tmp) : true;
}

static bool write_all_file(const char *path, const void *data, size_t len) {
    if (!ensure_parent_dir(path)) return false;
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return true;
        return false;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    return true;
}

void chronicle_new_id(char *out, size_t out_len) {
    unsigned char bytes[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t n = fread(bytes, 1, sizeof(bytes), f);
        fclose(f);
        if (n != sizeof(bytes)) memset(bytes, 0, sizeof(bytes));
    } else memset(bytes, 0, sizeof(bytes));
    if (bytes[0] == 0 && bytes[1] == 0) {
        struct timeval tv; gettimeofday(&tv, NULL);
        memcpy(bytes, &tv, sizeof(tv) < sizeof(bytes) ? sizeof(tv) : sizeof(bytes));
        bytes[0] ^= (unsigned char)getpid();
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
             bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
}

static chronicle_mode_t parse_mode(const char *s) {
    if (!s || !s[0]) return CHRONICLE_MODE_FULL_LOCAL;
    if (strcmp(s, "0") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0) return CHRONICLE_MODE_OFF;
    if (strcmp(s, "metadata") == 0) return CHRONICLE_MODE_METADATA;
    if (strcmp(s, "blackbox") == 0 || strcmp(s, "full") == 0 || strcmp(s, "forensic") == 0) return CHRONICLE_MODE_BLACKBOX;
    return CHRONICLE_MODE_FULL_LOCAL;
}

static const char *mode_str(chronicle_mode_t m) {
    switch (m) {
        case CHRONICLE_MODE_OFF: return "off";
        case CHRONICLE_MODE_METADATA: return "metadata";
        case CHRONICLE_MODE_FULL_LOCAL: return "full-local";
        case CHRONICLE_MODE_BLACKBOX: return "blackbox";
    }
    return "unknown";
}

static void resolve_root(char *out, size_t out_len) {
    const char *override = getenv("DSCO_CHRONICLE_DIR");
    if (override && override[0]) { snprintf(out, out_len, "%s", override); return; }
    const char *home = getenv("HOME");
    snprintf(out, out_len, "%s/.dsco/chronicle", home && home[0] ? home : ".");
}

static bool read_or_create_installation_id(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/installation_id", g_chronicle.root);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_chronicle.installation_id, sizeof(g_chronicle.installation_id), f)) {
            g_chronicle.installation_id[strcspn(g_chronicle.installation_id, "\r\n")] = '\0';
        }
        fclose(f);
        if (g_chronicle.installation_id[0]) return true;
    }
    chronicle_new_id(g_chronicle.installation_id, sizeof(g_chronicle.installation_id));
    if (!ensure_parent_dir(path)) return false;
    f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%s\n", g_chronicle.installation_id);
    fclose(f);
    return true;
}

static bool exec_sql(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_chronicle.db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "chronicle: sqlite error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static bool ensure_schema(void) {
    const char *schema =
        "CREATE TABLE IF NOT EXISTS sessions ("
        " session_id TEXT PRIMARY KEY, installation_id TEXT, instance_id TEXT, provider TEXT, model TEXT, mode TEXT,"
        " started_at INTEGER NOT NULL, ended_at INTEGER, root TEXT, policy_json TEXT);"
        "CREATE TABLE IF NOT EXISTS events ("
        " event_id TEXT PRIMARY KEY, installation_id TEXT NOT NULL, session_id TEXT NOT NULL,"
        " trace_id TEXT, span_id TEXT, parent_span_id TEXT, seq INTEGER NOT NULL, wall_time INTEGER NOT NULL,"
        " event_type TEXT NOT NULL, actor_type TEXT, actor_id TEXT, payload_json TEXT, payload_hash TEXT,"
        " sensitivity TEXT, prev_event_hash TEXT, event_hash TEXT, sync_state TEXT DEFAULT 'local');"
        "CREATE INDEX IF NOT EXISTS idx_chronicle_events_session_seq ON events(session_id, seq);"
        "CREATE INDEX IF NOT EXISTS idx_chronicle_events_type_time ON events(event_type, wall_time DESC);"
        "CREATE TABLE IF NOT EXISTS spans ("
        " span_id TEXT PRIMARY KEY, trace_id TEXT NOT NULL, parent_span_id TEXT, span_type TEXT NOT NULL, name TEXT,"
        " started_at INTEGER NOT NULL, ended_at INTEGER, status TEXT, payload_json TEXT);"
        "CREATE INDEX IF NOT EXISTS idx_chronicle_spans_trace ON spans(trace_id, started_at);"
        "CREATE TABLE IF NOT EXISTS blobs ("
        " sha256 TEXT PRIMARY KEY, byte_len INTEGER NOT NULL, content_type TEXT, logical_type TEXT,"
        " codec TEXT, encryption TEXT, sensitivity TEXT, local_path TEXT, created_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS edges ("
        " edge_id TEXT PRIMARY KEY, from_id TEXT NOT NULL, to_id TEXT NOT NULL, relation TEXT NOT NULL,"
        " confidence REAL, metadata_json TEXT, created_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS idx_chronicle_edges_from ON edges(from_id);"
        "CREATE INDEX IF NOT EXISTS idx_chronicle_edges_to ON edges(to_id);"
        "CREATE TABLE IF NOT EXISTS training_examples ("
        " example_id TEXT PRIMARY KEY, source_trace_id TEXT, source_span_ids TEXT, task_type TEXT, dataset_type TEXT,"
        " quality_score REAL, consent_state TEXT, redaction_state TEXT, input_blob TEXT, output_blob TEXT, label_blob TEXT,"
        " metadata_json TEXT, created_at INTEGER);";
    return exec_sql(schema);
}

bool chronicle_start(const chronicle_start_opts_t *opts) {
    if (g_chronicle.ready) {
        if (opts) {
            if (opts->instance_id && opts->instance_id[0])
                snprintf(g_chronicle.instance_id, sizeof(g_chronicle.instance_id), "%s", opts->instance_id);
            sqlite3_stmt *st = NULL;
            const char *sql = "UPDATE sessions SET instance_id=COALESCE(NULLIF(?1,''),instance_id),"
                              "provider=COALESCE(NULLIF(?2,''),provider),"
                              "model=COALESCE(NULLIF(?3,''),model),"
                              "mode=COALESCE(NULLIF(?4,''),mode) WHERE session_id=?5;";
            if (g_chronicle.db && sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, nz(opts->instance_id), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, nz(opts->provider), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, nz(opts->model), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, nz(opts->mode), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, g_chronicle.session_id, -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            jbuf_t p;
            jbuf_init(&p, 256);
            jbuf_append(&p, "{\"provider\":"); jbuf_append_json_str(&p, nz(opts->provider));
            jbuf_append(&p, ",\"model\":"); jbuf_append_json_str(&p, nz(opts->model));
            jbuf_append(&p, ",\"mode\":"); jbuf_append_json_str(&p, nz(opts->mode));
            jbuf_append(&p, ",\"instance_id\":"); jbuf_append_json_str(&p, nz(opts->instance_id));
            jbuf_append(&p, "}");
            chronicle_event("runtime.configured", NULL, NULL, NULL, "runtime", "dsco", p.data, "product_telemetry");
            jbuf_free(&p);
        }
        return true;
    }
    memset(&g_chronicle, 0, sizeof(g_chronicle));
    g_chronicle.mode = parse_mode(getenv("DSCO_CHRONICLE_MODE"));
    if (g_chronicle.mode == CHRONICLE_MODE_OFF) return false;
    resolve_root(g_chronicle.root, sizeof(g_chronicle.root));
    if (!mkdir_p(g_chronicle.root)) return false;
    if (!read_or_create_installation_id()) return false;
    chronicle_new_id(g_chronicle.session_id, sizeof(g_chronicle.session_id));
    if (opts && opts->instance_id) snprintf(g_chronicle.instance_id, sizeof(g_chronicle.instance_id), "%s", opts->instance_id);

    snprintf(g_chronicle.db_path, sizeof(g_chronicle.db_path), "%s/indexes/chronicle.sqlite", g_chronicle.root);
    if (!ensure_parent_dir(g_chronicle.db_path)) return false;
    if (sqlite3_open(g_chronicle.db_path, &g_chronicle.db) != SQLITE_OK) {
        fprintf(stderr, "chronicle: failed to open %s: %s\n", g_chronicle.db_path, sqlite3_errmsg(g_chronicle.db));
        if (g_chronicle.db) sqlite3_close(g_chronicle.db);
        return false;
    }
    sqlite3_busy_timeout(g_chronicle.db, 3000);
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");
    exec_sql("PRAGMA temp_store=MEMORY;");
    if (!ensure_schema()) { sqlite3_close(g_chronicle.db); g_chronicle.db = NULL; return false; }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(g_chronicle.event_log_path, sizeof(g_chronicle.event_log_path),
             "%s/events/%04d/%02d/%02d/session-%s.jsonl", g_chronicle.root,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, g_chronicle.session_id);
    if (!ensure_parent_dir(g_chronicle.event_log_path)) { sqlite3_close(g_chronicle.db); return false; }
    g_chronicle.events_fp = fopen(g_chronicle.event_log_path, "a");
    if (!g_chronicle.events_fp) { sqlite3_close(g_chronicle.db); return false; }
    g_chronicle.prev_event_hash[0] = '\0';
    g_chronicle.ready = true;
    setenv("DSCO_CHRONICLE_SESSION_ID", g_chronicle.session_id, 1);

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO sessions(session_id,installation_id,instance_id,provider,model,mode,started_at,root,policy_json)"
                      " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, g_chronicle.session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, g_chronicle.installation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, g_chronicle.instance_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, opts ? nz(opts->provider) : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, opts ? nz(opts->model) : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, opts ? nz(opts->mode) : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)now);
        sqlite3_bind_text(st, 8, g_chronicle.root, -1, SQLITE_TRANSIENT);
        char policy[256];
        snprintf(policy, sizeof(policy), "{\"capture_mode\":\"%s\",\"sync\":\"local-only\",\"training\":\"not-consented\"}", mode_str(g_chronicle.mode));
        sqlite3_bind_text(st, 9, policy, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    char payload[512];
    snprintf(payload, sizeof(payload), "{\"schema\":\"%s\",\"capture_mode\":\"%s\",\"provider\":", CHRONICLE_SCHEMA_VERSION, mode_str(g_chronicle.mode));
    jbuf_t b; jbuf_init(&b, 512); jbuf_append(&b, payload); jbuf_append_json_str(&b, opts ? nz(opts->provider) : "");
    jbuf_append(&b, ",\"model\":"); jbuf_append_json_str(&b, opts ? nz(opts->model) : "");
    jbuf_append(&b, ",\"mode\":"); jbuf_append_json_str(&b, opts ? nz(opts->mode) : "");
    jbuf_append(&b, "}");
    chronicle_event("session.started", NULL, NULL, NULL, "runtime", "dsco", b.data, "product_telemetry");
    jbuf_free(&b);
    return true;
}

void chronicle_stop(void) {
    if (!g_chronicle.ready) return;
    static bool stopping = false;
    if (stopping) return;
    stopping = true;
    chronicle_event("session.completed", NULL, NULL, NULL, "runtime", "dsco", NULL, "product_telemetry");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_chronicle.db, "UPDATE sessions SET ended_at=?1 WHERE session_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(st, 2, g_chronicle.session_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    if (g_chronicle.events_fp) { fflush(g_chronicle.events_fp); fclose(g_chronicle.events_fp); }
    if (g_chronicle.db) sqlite3_close(g_chronicle.db);
    memset(&g_chronicle, 0, sizeof(g_chronicle));
    stopping = false;
}

bool chronicle_ready(void) { return g_chronicle.ready; }
chronicle_mode_t chronicle_mode(void) { return g_chronicle.mode; }
const char *chronicle_installation_id(void) { return g_chronicle.installation_id; }
const char *chronicle_session_id(void) { return g_chronicle.session_id; }
const char *chronicle_root(void) { return g_chronicle.root; }
const char *chronicle_db_path(void) { return g_chronicle.db_path; }

static bool append_event_to_sqlite(const char *event_id, const char *trace_id, const char *span_id,
                                   const char *parent_span_id, const char *event_type,
                                   const char *actor_type, const char *actor_id,
                                   const char *payload_json, const char *payload_hash,
                                   const char *sensitivity, const char *prev_hash,
                                   const char *event_hash, sqlite3_int64 wall_time,
                                   unsigned long long seq) {
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO events(event_id,installation_id,session_id,trace_id,span_id,parent_span_id,seq,wall_time,"
                      "event_type,actor_type,actor_id,payload_json,payload_hash,sensitivity,prev_event_hash,event_hash)"
                      " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, g_chronicle.installation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, g_chronicle.session_id, -1, SQLITE_TRANSIENT);
    if (trace_id && trace_id[0]) sqlite3_bind_text(st, 4, trace_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
    if (span_id && span_id[0]) sqlite3_bind_text(st, 5, span_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
    if (parent_span_id && parent_span_id[0]) sqlite3_bind_text(st, 6, parent_span_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)seq);
    sqlite3_bind_int64(st, 8, wall_time);
    sqlite3_bind_text(st, 9, event_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, nz(actor_type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, nz(actor_id), -1, SQLITE_TRANSIENT);
    if (payload_json) sqlite3_bind_text(st, 12, payload_json, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 12);
    sqlite3_bind_text(st, 13, payload_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 14, nz(sensitivity), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, nz(prev_hash), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 16, event_hash, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool chronicle_event(const char *event_type, const char *trace_id, const char *span_id,
                     const char *parent_span_id, const char *actor_type, const char *actor_id,
                     const char *payload_json, const char *sensitivity) {
    if (!g_chronicle.ready || !event_type || !event_type[0]) return false;
    char event_id[37]; chronicle_new_id(event_id, sizeof(event_id));
    char payload_hash[65]; sha256_hex(payload_json ? payload_json : "", payload_json ? strlen(payload_json) : 0, payload_hash);
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    unsigned long long seq = ++g_chronicle.seq;

    jbuf_t line; jbuf_init(&line, 1024 + (payload_json ? strlen(payload_json) : 0));
    jbuf_append(&line, "{\"schema_version\":\"" CHRONICLE_SCHEMA_VERSION "\",");
    jbuf_append(&line, "\"event_id\":"); jbuf_append_json_str(&line, event_id);
    jbuf_append(&line, ",\"installation_id\":"); jbuf_append_json_str(&line, g_chronicle.installation_id);
    jbuf_append(&line, ",\"session_id\":"); jbuf_append_json_str(&line, g_chronicle.session_id);
    jbuf_append(&line, ",\"trace_id\":"); if (trace_id && trace_id[0]) jbuf_append_json_str(&line, trace_id); else jbuf_append(&line, "null");
    jbuf_append(&line, ",\"span_id\":"); if (span_id && span_id[0]) jbuf_append_json_str(&line, span_id); else jbuf_append(&line, "null");
    jbuf_append(&line, ",\"parent_span_id\":"); if (parent_span_id && parent_span_id[0]) jbuf_append_json_str(&line, parent_span_id); else jbuf_append(&line, "null");
    jbuf_append(&line, ",\"seq\":"); jbuf_appendf(&line, "%llu", seq);
    jbuf_append(&line, ",\"wall_time\":"); jbuf_appendf(&line, "%lld", (long long)now);
    jbuf_append(&line, ",\"event_type\":"); jbuf_append_json_str(&line, event_type);
    jbuf_append(&line, ",\"actor_type\":"); jbuf_append_json_str(&line, nz(actor_type));
    jbuf_append(&line, ",\"actor_id\":"); jbuf_append_json_str(&line, nz(actor_id));
    jbuf_append(&line, ",\"payload\":");
    if (payload_json && payload_json[0]) jbuf_append(&line, payload_json); else jbuf_append(&line, "{}");
    jbuf_append(&line, ",\"sensitivity\":"); jbuf_append_json_str(&line, nz(sensitivity));
    jbuf_append(&line, ",\"payload_hash\":"); jbuf_append_json_str(&line, payload_hash);
    jbuf_append(&line, ",\"prev_event_hash\":"); jbuf_append_json_str(&line, g_chronicle.prev_event_hash);
    jbuf_append(&line, "}");

    char event_hash[65]; sha256_hex(line.data, line.len, event_hash);
    append_event_to_sqlite(event_id, trace_id, span_id, parent_span_id, event_type, actor_type, actor_id,
                           payload_json, payload_hash, sensitivity, g_chronicle.prev_event_hash,
                           event_hash, now, seq);
    if (g_chronicle.events_fp) {
        fputs(line.data, g_chronicle.events_fp); fputc('\n', g_chronicle.events_fp); fflush(g_chronicle.events_fp);
    }
    snprintf(g_chronicle.prev_event_hash, sizeof(g_chronicle.prev_event_hash), "%s", event_hash);
    jbuf_free(&line);
    return true;
}

bool chronicle_blob_put(const void *data, size_t len, const char *logical_type, const char *content_type,
                        const char *sensitivity, char *sha_out, size_t sha_out_len) {
    if (!g_chronicle.ready) return false;
    if (!data && len) return false;
    char sha[65]; sha256_hex(data ? data : "", len, sha);
    if (sha_out && sha_out_len > 0) snprintf(sha_out, sha_out_len, "%s", sha);
    char rel[PATH_MAX];
    snprintf(rel, sizeof(rel), "blobs/sha256/%c%c/%c%c/%s.blob", sha[0], sha[1], sha[2], sha[3], sha);
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", g_chronicle.root, rel);
    if (!write_all_file(path, data ? data : "", len)) return false;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT OR IGNORE INTO blobs(sha256,byte_len,content_type,logical_type,codec,encryption,sensitivity,local_path,created_at)"
                      " VALUES(?1,?2,?3,?4,'raw','none',?5,?6,?7);";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)len);
        sqlite3_bind_text(st, 3, nz(content_type), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, nz(logical_type), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, nz(sensitivity), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, rel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)time(NULL));
        sqlite3_step(st); sqlite3_finalize(st);
    }
    return true;
}

bool chronicle_blob_put_text(const char *text, const char *logical_type, const char *sensitivity,
                             char *sha_out, size_t sha_out_len) {
    return chronicle_blob_put(text ? text : "", text ? strlen(text) : 0, logical_type, "text/plain; charset=utf-8", sensitivity, sha_out, sha_out_len);
}

bool chronicle_span_begin(const char *trace_id, const char *parent_span_id, const char *span_type,
                          const char *name, const char *payload_json, char *span_id_out) {
    if (!g_chronicle.ready || !trace_id || !trace_id[0] || !span_id_out) return false;
    chronicle_new_id(span_id_out, 37);
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO spans(span_id,trace_id,parent_span_id,span_type,name,started_at,status,payload_json)"
                      " VALUES(?1,?2,?3,?4,?5,?6,'running',?7);";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, span_id_out, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, trace_id, -1, SQLITE_TRANSIENT);
    if (parent_span_id && parent_span_id[0]) sqlite3_bind_text(st, 3, parent_span_id, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, nz(span_type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, nz(name), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
    if (payload_json) sqlite3_bind_text(st, 7, payload_json, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 7);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (ok) {
        jbuf_t p; jbuf_init(&p, 256);
        jbuf_append(&p, "{\"span_type\":"); jbuf_append_json_str(&p, nz(span_type));
        jbuf_append(&p, ",\"name\":"); jbuf_append_json_str(&p, nz(name));
        if (payload_json && payload_json[0]) { jbuf_append(&p, ",\"data\":"); jbuf_append(&p, payload_json); }
        jbuf_append(&p, "}");
        chronicle_event("span.started", trace_id, span_id_out, parent_span_id, "agent", "dsco", p.data, "product_telemetry");
        jbuf_free(&p);
    }
    return ok;
}

bool chronicle_span_end(const char *span_id, const char *status, const char *payload_json) {
    if (!g_chronicle.ready || !span_id || !span_id[0]) return false;
    char trace_id[64] = "";
    char parent[64] = "";
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(g_chronicle.db, "SELECT trace_id,COALESCE(parent_span_id,'') FROM spans WHERE span_id=?1;", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, span_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(q, 0);
            const char *p = (const char *)sqlite3_column_text(q, 1);
            snprintf(trace_id, sizeof(trace_id), "%s", nz(t));
            snprintf(parent, sizeof(parent), "%s", nz(p));
        }
        sqlite3_finalize(q);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_chronicle.db, "UPDATE spans SET ended_at=?1,status=?2,payload_json=COALESCE(?3,payload_json) WHERE span_id=?4;", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(st, 2, status ? status : "ok", -1, SQLITE_TRANSIENT);
    if (payload_json) sqlite3_bind_text(st, 3, payload_json, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, span_id, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (ok) {
        jbuf_t p; jbuf_init(&p, 256);
        jbuf_append(&p, "{\"status\":"); jbuf_append_json_str(&p, status ? status : "ok");
        if (payload_json && payload_json[0]) { jbuf_append(&p, ",\"data\":"); jbuf_append(&p, payload_json); }
        jbuf_append(&p, "}");
        chronicle_event("span.completed", trace_id, span_id, parent, "agent", "dsco", p.data, "product_telemetry");
        jbuf_free(&p);
    }
    return ok;
}

bool chronicle_edge(const char *from_id, const char *to_id, const char *relation,
                    double confidence, const char *metadata_json) {
    if (!g_chronicle.ready || !from_id || !to_id || !relation) return false;
    char edge_id[37]; chronicle_new_id(edge_id, sizeof(edge_id));
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO edges(edge_id,from_id,to_id,relation,confidence,metadata_json,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7);";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, from_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, to_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, relation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 5, confidence);
    if (metadata_json) sqlite3_bind_text(st, 6, metadata_json, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)time(NULL));
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool capture_full_payload(void) {
    return g_chronicle.mode == CHRONICLE_MODE_FULL_LOCAL || g_chronicle.mode == CHRONICLE_MODE_BLACKBOX;
}

bool chronicle_user_message(const char *trace_id, const char *span_id, const char *text) {
    if (!g_chronicle.ready) return false;
    char sha[65] = "";
    if (capture_full_payload()) chronicle_blob_put_text(text, "user_message", "private_user_content", sha, sizeof(sha));
    jbuf_t p; jbuf_init(&p, 256);
    jbuf_append(&p, "{\"byte_len\":"); jbuf_appendf(&p, "%zu", text ? strlen(text) : 0);
    jbuf_append(&p, ",\"blob_sha256\":"); if (sha[0]) jbuf_append_json_str(&p, sha); else jbuf_append(&p, "null");
    if (g_chronicle.mode == CHRONICLE_MODE_METADATA && text) {
        jbuf_append(&p, ",\"preview\":");
        char prev[121]; snprintf(prev, sizeof(prev), "%.*s", 120, text); jbuf_append_json_str(&p, prev);
    }
    jbuf_append(&p, "}");
    bool ok = chronicle_event("user.message", trace_id, span_id, NULL, "user", "local", p.data, "private_user_content");
    jbuf_free(&p); return ok;
}

bool chronicle_context_materialized(const char *trace_id, const char *span_id,
                                    const char *request_json, int estimated_tokens) {
    char sha[65] = "";
    if (capture_full_payload()) chronicle_blob_put(request_json ? request_json : "", request_json ? strlen(request_json) : 0,
                                                   "context.materialized.request_json", "application/json", "private_user_content", sha, sizeof(sha));
    jbuf_t p; jbuf_init(&p, 256);
    jbuf_append(&p, "{\"estimated_tokens\":"); jbuf_appendf(&p, "%d", estimated_tokens);
    jbuf_append(&p, ",\"request_blob_sha256\":"); if (sha[0]) jbuf_append_json_str(&p, sha); else jbuf_append(&p, "null");
    jbuf_append(&p, "}");
    bool ok = chronicle_event("context.materialized", trace_id, span_id, NULL, "agent", "dsco", p.data, "private_user_content");
    jbuf_free(&p); return ok;
}

bool chronicle_llm_request(const char *trace_id, const char *span_id, const char *provider,
                           const char *model, const char *request_json, int estimated_tokens) {
    char sha[65] = "";
    if (capture_full_payload()) chronicle_blob_put(request_json ? request_json : "", request_json ? strlen(request_json) : 0,
                                                   "llm.request.raw_json", "application/json", "private_user_content", sha, sizeof(sha));
    jbuf_t p; jbuf_init(&p, 512);
    jbuf_append(&p, "{\"provider\":"); jbuf_append_json_str(&p, nz(provider));
    jbuf_append(&p, ",\"model\":"); jbuf_append_json_str(&p, nz(model));
    jbuf_append(&p, ",\"estimated_input_tokens\":"); jbuf_appendf(&p, "%d", estimated_tokens);
    jbuf_append(&p, ",\"byte_len\":"); jbuf_appendf(&p, "%zu", request_json ? strlen(request_json) : 0);
    jbuf_append(&p, ",\"request_blob_sha256\":"); if (sha[0]) jbuf_append_json_str(&p, sha); else jbuf_append(&p, "null");
    jbuf_append(&p, "}");
    bool ok = chronicle_event("llm.request.created", trace_id, span_id, NULL, "agent", "dsco", p.data, "private_user_content");
    jbuf_free(&p); return ok;
}

bool chronicle_llm_delta(const char *trace_id, const char *span_id, const char *kind, const char *text) {
    if (!g_chronicle.ready || g_chronicle.mode != CHRONICLE_MODE_BLACKBOX) return false;
    char sha[65] = ""; chronicle_blob_put_text(text, kind && strcmp(kind, "thinking") == 0 ? "llm.response.thinking_delta" : "llm.response.text_delta", "model_output", sha, sizeof(sha));
    jbuf_t p; jbuf_init(&p, 256);
    jbuf_append(&p, "{\"kind\":"); jbuf_append_json_str(&p, nz(kind));
    jbuf_append(&p, ",\"byte_len\":"); jbuf_appendf(&p, "%zu", text ? strlen(text) : 0);
    jbuf_append(&p, ",\"blob_sha256\":"); jbuf_append_json_str(&p, sha);
    jbuf_append(&p, "}");
    bool ok = chronicle_event("llm.response.delta", trace_id, span_id, NULL, "model", "provider", p.data, "model_output");
    jbuf_free(&p); return ok;
}

bool chronicle_llm_response(const char *trace_id, const char *span_id, const char *provider,
                            const char *model, const char *output_text, const char *raw_response_json,
                            int input_tokens, int output_tokens, int cache_read_tokens,
                            int cache_write_tokens, int reasoning_tokens, double cost_usd,
                            double latency_ms, const char *finish_reason, const char *generation_id) {
    char out_sha[65] = "", raw_sha[65] = "";
    if (capture_full_payload()) {
        chronicle_blob_put_text(output_text, "llm.response.output_text", "model_output", out_sha, sizeof(out_sha));
        if (raw_response_json) chronicle_blob_put(raw_response_json, strlen(raw_response_json), "llm.response.raw_json", "application/json", "model_output", raw_sha, sizeof(raw_sha));
    }
    jbuf_t p; jbuf_init(&p, 512);
    jbuf_append(&p, "{\"provider\":"); jbuf_append_json_str(&p, nz(provider));
    jbuf_append(&p, ",\"model\":"); jbuf_append_json_str(&p, nz(model));
    jbuf_append(&p, ",\"usage\":{");
    jbuf_appendf(&p, "\"input_tokens\":%d,\"output_tokens\":%d,\"cache_read_tokens\":%d,\"cache_write_tokens\":%d,\"reasoning_tokens\":%d}",
                 input_tokens, output_tokens, cache_read_tokens, cache_write_tokens, reasoning_tokens);
    jbuf_append(&p, ",\"cost_usd\":"); jbuf_appendf(&p, "%.8f", cost_usd);
    jbuf_append(&p, ",\"latency_ms\":"); jbuf_appendf(&p, "%.3f", latency_ms);
    jbuf_append(&p, ",\"finish_reason\":"); jbuf_append_json_str(&p, nz(finish_reason));
    jbuf_append(&p, ",\"generation_id\":"); jbuf_append_json_str(&p, nz(generation_id));
    jbuf_append(&p, ",\"output_blob_sha256\":"); if (out_sha[0]) jbuf_append_json_str(&p, out_sha); else jbuf_append(&p, "null");
    jbuf_append(&p, ",\"raw_response_blob_sha256\":"); if (raw_sha[0]) jbuf_append_json_str(&p, raw_sha); else jbuf_append(&p, "null");
    jbuf_append(&p, "}");
    bool ok = chronicle_event("llm.response.completed", trace_id, span_id, NULL, "model", "provider", p.data, "model_output");
    jbuf_free(&p); return ok;
}

bool chronicle_tool_call_start(const char *trace_id, const char *parent_span_id, const char *tool_name,
                               const char *tool_id, const char *args_json, char *tool_span_id_out) {
    char arg_sha[65] = "";
    if (capture_full_payload()) chronicle_blob_put(args_json ? args_json : "", args_json ? strlen(args_json) : 0, "tool.args_json", "application/json", "private_user_content", arg_sha, sizeof(arg_sha));
    jbuf_t p; jbuf_init(&p, 512);
    jbuf_append(&p, "{\"tool_name\":"); jbuf_append_json_str(&p, nz(tool_name));
    jbuf_append(&p, ",\"tool_id\":"); jbuf_append_json_str(&p, nz(tool_id));
    jbuf_append(&p, ",\"args_blob_sha256\":"); if (arg_sha[0]) jbuf_append_json_str(&p, arg_sha); else jbuf_append(&p, "null");
    jbuf_append(&p, ",\"args_byte_len\":"); jbuf_appendf(&p, "%zu", args_json ? strlen(args_json) : 0);
    jbuf_append(&p, "}");
    bool ok = chronicle_span_begin(trace_id, parent_span_id, "tool", tool_name, p.data, tool_span_id_out);
    if (ok) chronicle_event("tool.call.created", trace_id, tool_span_id_out, parent_span_id, "agent", "dsco", p.data, "private_user_content");
    jbuf_free(&p); return ok;
}

bool chronicle_tool_call_end(const char *trace_id, const char *tool_span_id, const char *tool_name,
                             const char *result_text, bool ok, bool timeout, double latency_ms) {
    char res_sha[65] = "";
    if (capture_full_payload()) chronicle_blob_put_text(result_text, "tool.result_text", "tool_output", res_sha, sizeof(res_sha));
    jbuf_t p; jbuf_init(&p, 512);
    jbuf_append(&p, "{\"tool_name\":"); jbuf_append_json_str(&p, nz(tool_name));
    jbuf_append(&p, ",\"ok\":"); jbuf_append(&p, ok ? "true" : "false");
    jbuf_append(&p, ",\"timeout\":"); jbuf_append(&p, timeout ? "true" : "false");
    jbuf_append(&p, ",\"latency_ms\":"); jbuf_appendf(&p, "%.3f", latency_ms);
    jbuf_append(&p, ",\"result_byte_len\":"); jbuf_appendf(&p, "%zu", result_text ? strlen(result_text) : 0);
    jbuf_append(&p, ",\"result_blob_sha256\":"); if (res_sha[0]) jbuf_append_json_str(&p, res_sha); else jbuf_append(&p, "null");
    jbuf_append(&p, "}");
    chronicle_event("tool.call.completed", trace_id, tool_span_id, NULL, "tool", nz(tool_name), p.data, "tool_output");
    bool ret = chronicle_span_end(tool_span_id, timeout ? "timeout" : (ok ? "ok" : "error"), p.data);
    jbuf_free(&p); return ret;
}

char *chronicle_build_activity_json(int limit, const char *session_filter) {
    if (!g_chronicle.db) return safe_strdup("{\"events\":[]}");
    if (limit <= 0 || limit > 5000) limit = 500;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT event_id,session_id,trace_id,span_id,parent_span_id,seq,wall_time,event_type,actor_type,actor_id,"
                      "COALESCE(payload_json,'{}'),COALESCE(sensitivity,''),COALESCE(event_hash,'') FROM events "
                      "WHERE (?1 IS NULL OR session_id=?1) ORDER BY wall_time DESC, seq DESC LIMIT ?2;";
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) != SQLITE_OK) return safe_strdup("{\"error\":\"query failed\"}");
    if (session_filter && session_filter[0]) sqlite3_bind_text(st, 1, session_filter, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
    sqlite3_bind_int(st, 2, limit);
    jbuf_t b; jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"session_filter\":"); if (session_filter && session_filter[0]) jbuf_append_json_str(&b, session_filter); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"events\":[");
    bool first = true;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) jbuf_append(&b, ","); first = false;
        jbuf_append(&b, "{\"event_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,0)));
        jbuf_append(&b, ",\"session_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,1)));
        jbuf_append(&b, ",\"trace_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,2)));
        jbuf_append(&b, ",\"span_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,3)));
        jbuf_append(&b, ",\"parent_span_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,4)));
        jbuf_append(&b, ",\"seq\":"); jbuf_appendf(&b, "%lld", sqlite3_column_int64(st,5));
        jbuf_append(&b, ",\"wall_time\":"); jbuf_appendf(&b, "%lld", sqlite3_column_int64(st,6));
        jbuf_append(&b, ",\"event_type\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,7)));
        jbuf_append(&b, ",\"actor_type\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,8)));
        jbuf_append(&b, ",\"actor_id\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,9)));
        jbuf_append(&b, ",\"payload\":"); jbuf_append(&b, nz((const char *)sqlite3_column_text(st,10)));
        jbuf_append(&b, ",\"sensitivity\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,11)));
        jbuf_append(&b, ",\"event_hash\":"); jbuf_append_json_str(&b, nz((const char *)sqlite3_column_text(st,12)));
        jbuf_append(&b, "}");
    }
    sqlite3_finalize(st);
    jbuf_append(&b, "]}");
    return b.data;
}

static void html_escape(jbuf_t *b, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&': jbuf_append(b, "&amp;"); break;
            case '<': jbuf_append(b, "&lt;"); break;
            case '>': jbuf_append(b, "&gt;"); break;
            case '"': jbuf_append(b, "&quot;"); break;
            case '\'': jbuf_append(b, "&#39;"); break;
            default: jbuf_append_char(b, *s); break;
        }
    }
}

char *chronicle_build_activity_html(int limit, const char *session_filter) {
    char *json = chronicle_build_activity_json(limit, session_filter);
    jbuf_t b; jbuf_init(&b, 16384);
    jbuf_append(&b, "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                   "<title>DSCO Chronicle</title><style>body{font-family:ui-sans-serif,system-ui;margin:0;background:#090d14;color:#e8eef9}"
                   "header{padding:20px 28px;background:#101827;border-bottom:1px solid #26344d;position:sticky;top:0}main{padding:20px 28px}.card{background:#111a2a;border:1px solid #26344d;border-radius:12px;padding:14px;margin:12px 0}.chip{display:inline-block;background:#203a63;color:#9dccff;border-radius:999px;padding:2px 8px;font-size:12px}.muted{color:#91a0b8;font-size:13px}pre{white-space:pre-wrap;word-break:break-word;background:#070a10;border:1px solid #26344d;border-radius:8px;padding:10px;max-height:280px;overflow:auto}a{color:#8cc7ff}input,button{padding:8px;border-radius:8px;border:1px solid #344761;background:#0b111c;color:#e8eef9}button{background:#1e5aa8}</style></head><body>");
    jbuf_append(&b, "<header><h1>DSCO Chronicle</h1><div class='muted'>full activity recorder · local store: "); html_escape(&b, chronicle_root()); jbuf_append(&b, "</div>");
    jbuf_append(&b, "<form><input name='session' placeholder='session id' value='"); html_escape(&b, session_filter ? session_filter : ""); jbuf_append(&b, "'> <button>Filter</button> <a href='/chronicle.json'>json</a></form></header><main>");
    jbuf_append(&b, "<section class='card'><h2>Raw activity JSON</h2><p class='muted'>First product slice: complete event graph + blob hashes. UI projections come next.</p><pre>");
    html_escape(&b, json);
    jbuf_append(&b, "</pre></section></main></body></html>");
    free(json);
    return b.data;
}

char *chronicle_read_blob_hex(const char *sha256, size_t max_bytes, const char **content_type_out) {
    if (content_type_out) *content_type_out = "application/octet-stream";
    if (!g_chronicle.db || !sha256 || strlen(sha256) != 64) return NULL;
    for (const char *p = sha256; *p; p++) if (!isxdigit((unsigned char)*p)) return NULL;
    sqlite3_stmt *st = NULL;
    char rel[PATH_MAX] = "";
    static char ctype[128]; ctype[0] = '\0';
    if (sqlite3_prepare_v2(g_chronicle.db, "SELECT local_path,COALESCE(content_type,'application/octet-stream') FROM blobs WHERE sha256=?1;", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, sha256, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        snprintf(rel, sizeof(rel), "%s", nz((const char *)sqlite3_column_text(st,0)));
        snprintf(ctype, sizeof(ctype), "%s", nz((const char *)sqlite3_column_text(st,1)));
    }
    sqlite3_finalize(st);
    if (!rel[0] || strstr(rel, "..")) return NULL;
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", g_chronicle.root, rel);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    size_t n = (size_t)sz;
    if (max_bytes > 0 && n > max_bytes) n = max_bytes;
    char *buf = safe_malloc(n + 1);
    size_t r = fread(buf, 1, n, f); fclose(f); buf[r] = '\0';
    if (content_type_out) *content_type_out = ctype[0] ? ctype : "application/octet-stream";
    return buf;
}
