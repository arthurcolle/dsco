#include "chronicle.h"
#include "agent_event.h"
#include "callbacks.h"
#include "json_util.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    if (!ctx || !data || len == 0) return;

    /* Fast path: hash whole 64-byte blocks directly from caller memory instead
     * of copying one byte at a time through ctx->data. Chronicle hashes every
     * event payload/blob, so large tool results and transcript blobs were
     * paying avoidable per-byte loop overhead here. */
    if (ctx->datalen != 0) {
        size_t fill = 64U - (size_t)ctx->datalen;
        if (fill > len) fill = len;
        memcpy(ctx->data + ctx->datalen, data, fill);
        ctx->datalen += (uint32_t)fill;
        data += fill;
        len -= fill;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }

    while (len >= 64U) {
        sha256_transform(ctx, data);
        ctx->bitlen += 512;
        data += 64U;
        len -= 64U;
    }

    if (len != 0) {
        memcpy(ctx->data, data, len);
        ctx->datalen = (uint32_t)len;
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
    char journal_path[PATH_MAX];
    int journal_fd;
    bool journal_enabled;
    char installation_id[37];
    char session_id[37];
    char instance_id[128];
    time_t started_at;
    unsigned long long seq;
    char prev_event_hash[65];
} chronicle_state_t;

static chronicle_state_t g_chronicle = {0};

static const char *nz(const char *s) { return s ? s : ""; }

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xffU] ^ (crc >> 8);
    return ~crc;
}

static bool write_all_fd(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

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
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    return true;
}

static bool path_has_unsafe_component(const char *path) {
    if (!path || !path[0]) return true;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 2 && start[0] == '.' && start[1] == '.') return true;
    }
    return false;
}

static bool path_is_safe_dir(const char *path) {
    if (path_has_unsafe_component(path)) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return false;
    return true;
}

static bool write_file_atomic_replace(const char *path, const char *body, mode_t mode) {
    if (!path || !path[0] || path_has_unsafe_component(path)) return false;
    if (!ensure_parent_dir(path)) return false;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof(dir), ".");
    else if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    if (!path_is_safe_dir(dir)) return false;

    struct stat st;
    if (lstat(path, &st) == 0 && (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) return false;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.XXXXXX", path, (long)getpid());
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    fchmod(fd, mode);
    const char *p = body ? body : "";
    size_t len = strlen(p);
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(tmp); return false; }
        p += n;
        len -= (size_t)n;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return false; }
    if (close(fd) != 0) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }

    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
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

static bool chronicle_journal_open(void);
static bool chronicle_run_manifest_write(const char *status);

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
    g_chronicle.journal_fd = -1;
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
    g_chronicle.started_at = now;
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
    chronicle_journal_open();
    return true;
}

static bool chronicle_name_has_suffix(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static char *chronicle_read_small_file(const char *path, size_t max_bytes) {
    if (!path || !path[0]) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n < 0 || (size_t)n > max_bytes) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

typedef struct {
    int enqueued;
    int pending;
    int retry_scheduled;
    int delivered;
    int dead_lettered;
    int unknown;
} chronicle_callback_counts_t;

static void chronicle_callback_counts(chronicle_callback_counts_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const char *run_dir = chronicle_run_dir();
    if (!run_dir || !run_dir[0]) return;
    char outbox[PATH_MAX];
    snprintf(outbox, sizeof(outbox), "%s/outbox", run_dir);
    DIR *d = opendir(outbox);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (chronicle_name_has_suffix(ent->d_name, ".json")) {
            out->enqueued++;
            continue;
        }
        if (!chronicle_name_has_suffix(ent->d_name, ".state")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", outbox, ent->d_name);
        char *st = chronicle_read_small_file(path, 64 * 1024);
        if (!st) { out->unknown++; continue; }
        if (strstr(st, "\"state\":\"pending\"")) out->pending++;
        else if (strstr(st, "\"state\":\"retry_scheduled\"")) out->retry_scheduled++;
        else if (strstr(st, "\"state\":\"delivered\"")) out->delivered++;
        else if (strstr(st, "\"state\":\"dead_lettered\"")) out->dead_lettered++;
        else out->unknown++;
        free(st);
    }
    closedir(d);
}

static void chronicle_emit_run_receipt(void) {
    chronicle_callback_counts_t cb;
    chronicle_callback_counts(&cb);
    chronicle_cost_totals_t cost;
    bool have_cost = chronicle_cost_totals_for_session(g_chronicle.session_id, &cost);
    if (!have_cost) memset(&cost, 0, sizeof(cost));

    long long ended_at = (long long)time(NULL);
    jbuf_t p;
    jbuf_init(&p, 768);
    jbuf_append(&p, "{\"schema\":\"dsco.run_receipt.v1\",\"status\":\"completed\"");
    jbuf_append(&p, ",\"run_id\":"); jbuf_append_json_str(&p, g_chronicle.session_id);
    jbuf_appendf(&p, ",\"started_at\":%lld,\"ended_at\":%lld", (long long)g_chronicle.started_at, ended_at);
    jbuf_appendf(&p, ",\"duration_ms\":%lld", g_chronicle.started_at ? (ended_at - (long long)g_chronicle.started_at) * 1000LL : 0LL);
    jbuf_appendf(&p, ",\"journal_records\":%llu", g_chronicle.seq);
    jbuf_appendf(&p, ",\"callbacks\":{\"enqueued\":%d,\"pending\":%d,\"retry_scheduled\":%d,\"delivered\":%d,\"dead_lettered\":%d,\"unknown\":%d}",
                 cb.enqueued, cb.pending, cb.retry_scheduled, cb.delivered, cb.dead_lettered, cb.unknown);
    jbuf_appendf(&p, ",\"cost\":{\"placeholder\":%s,\"usd_total\":%.8f,\"input_tokens\":%lld,\"output_tokens\":%lld,\"cache_read_tokens\":%lld,\"cache_write_tokens\":%lld,\"reasoning_tokens\":%lld,\"response_count\":%d}",
                 have_cost ? "false" : "true", cost.cost_usd, cost.input_tokens, cost.output_tokens,
                 cost.cache_read_tokens, cost.cache_write_tokens, cost.reasoning_tokens, cost.response_count);
    jbuf_append(&p, "}");
    agent_event_emit_simple("run.receipt", "ok", p.data, AGENT_EVENT_DURABLE | AGENT_EVENT_CALLBACK);
    jbuf_free(&p);
}

void chronicle_stop(void) {
    if (!g_chronicle.ready) return;
    static bool stopping = false;
    if (stopping) return;
    stopping = true;
    agent_event_emit_simple("run.completed", "ok", "{\"status\":\"completed\"}",
                            AGENT_EVENT_DURABLE | AGENT_EVENT_CALLBACK);
    chronicle_journal_run_end("completed", 0.0, 0); /* canonical run boundary; per-turn cost lives in CHECKPOINTs */
    chronicle_emit_run_receipt();
    chronicle_event("session.completed", NULL, NULL, NULL, "runtime", "dsco", NULL, "product_telemetry");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_chronicle.db, "UPDATE sessions SET ended_at=?1 WHERE session_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(st, 2, g_chronicle.session_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    if (g_chronicle.events_fp) { fflush(g_chronicle.events_fp); fclose(g_chronicle.events_fp); }
    chronicle_run_manifest_write("completed");
    if (g_chronicle.journal_fd >= 0) { fsync(g_chronicle.journal_fd); close(g_chronicle.journal_fd); }
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

const char *chronicle_run_id(void) { return g_chronicle.session_id; }
const char *chronicle_journal_path(void) { return g_chronicle.journal_path; }
const char *chronicle_run_dir(void) {
    static char dir[PATH_MAX];
    if (g_chronicle.journal_path[0]) {
        snprintf(dir, sizeof(dir), "%s", g_chronicle.journal_path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        return dir;
    }
    return "";
}


static uint32_t load_le32(const unsigned char b[4]) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* W1: canonical inline-vs-blob threshold. Records larger than this put the
 * payload in the blob store and journal only its sha256 ref. */
#define JOURNAL_INLINE_MAX (32ULL * 1024ULL)

/* W1: canonical RUN_START — emitted by chronicle_journal_open() with best-
 * effort goal/model/config provenance. Nulls are legal: the frame's presence
 * is what marks the run boundary. */
bool chronicle_journal_run_start(const char *goal, const char *model,
                                 const char *config_hash) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"goal\":");
    if (goal && goal[0]) jbuf_append_json_str(&b, goal); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"model\":");
    if (model && model[0]) jbuf_append_json_str(&b, model); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"config_hash\":");
    if (config_hash && config_hash[0]) jbuf_append_json_str(&b, config_hash); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"cwd\":");
    char cwd[PATH_MAX];
    jbuf_append_json_str(&b, getcwd(cwd, sizeof(cwd)) ? cwd : "");
    jbuf_append(&b, ",\"git_head\":");
    char head[65] = "";
    { /* best-effort: loose ref first, then packed-refs */
        char p[PATH_MAX];
        FILE *fp = NULL;
        snprintf(p, sizeof(p), ".git/HEAD");
        char hb[256] = "";
        if ((fp = fopen(p, "r")) && fgets(hb, sizeof(hb), fp)) {
            char *nl = strchr(hb, '\n'); if (nl) *nl = 0;
            fclose(fp); fp = NULL;
            if (strncmp(hb, "ref: ", 5) == 0) {
                snprintf(p, sizeof(p), ".git/%s", hb + 5);
                fp = fopen(p, "r"); /* loose ref; packed-refs left as best-effort miss */
            } else {
                snprintf(head, sizeof(head), "%s", hb); /* detached: HEAD holds the sha */
            }
        }
        if (fp) {
            if (fgets(head, 41, fp)) head[40] = 0;
            fclose(fp);
        }
    }
    jbuf_append_json_str(&b, head);
    jbuf_append(&b, "}");
    bool ok = chronicle_journal_append("RUN_START", b.data, true);
    jbuf_free(&b);
    return ok;
}

/* W1: canonical TURN_START — turn number plus prompt provenance. The prompt
 * is hashed always (replay idempotency key) and blob-ref'd when it exceeds
 * the inline threshold. This closes the P1.2 write-side gap: resume can now
 * reconstruct what the model was asked, not just that it was asked. */
bool chronicle_journal_turn_start(int turn, const char *prompt_text) {
    size_t n = prompt_text ? strlen(prompt_text) : 0;
    char p_sha[65];
    sha256_hex(prompt_text ? prompt_text : "", n, p_sha);
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"turn\":%d,\"prompt_bytes\":%zu,\"prompt_sha256\":", turn, n);
    jbuf_append_json_str(&b, p_sha);
    if (n > JOURNAL_INLINE_MAX) {
        char bsha[65] = "";
        chronicle_blob_put_text(prompt_text, "journal.turn_prompt", "local", bsha, sizeof(bsha));
        jbuf_append(&b, ",\"prompt_blob_sha256\":");
        jbuf_append_json_str(&b, bsha[0] ? bsha : "");
    }
    jbuf_append(&b, "}");
    bool ok = chronicle_journal_append("TURN_START", b.data, true);
    jbuf_free(&b);
    return ok;
}

/* W1: canonical CHECKPOINT — per-turn accounting snapshot; the fsynced
 * run-resume anchor between turns. */
bool chronicle_journal_checkpoint(int turn, double cost_usd, long long input_tokens,
                                  long long output_tokens, const char *stop_reason) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"turn\":%d,\"cost_usd\":%.8f,\"input_tokens\":%lld,"
                     "\"output_tokens\":%lld,\"stop_reason\":",
                 turn, cost_usd, input_tokens, output_tokens);
    jbuf_append_json_str(&b, stop_reason && stop_reason[0] ? stop_reason : "");
    jbuf_append(&b, "}");
    bool ok = chronicle_journal_append("CHECKPOINT", b.data, true);
    jbuf_free(&b);
    return ok;
}

/* W1: canonical RUN_END — terminal record; emitted from chronicle_stop()
 * before the manifest is sealed so the WAL is self-describing. */
bool chronicle_journal_run_end(const char *status, double cost_usd, long long turns) {
    jbuf_t b;
    jbuf_init(&b, 192);
    jbuf_append(&b, "{\"status\":");
    jbuf_append_json_str(&b, status && status[0] ? status : "completed");
    jbuf_appendf(&b, ",\"cost_usd\":%.8f,\"turns\":%lld}", cost_usd, turns);
    bool ok = chronicle_journal_append("RUN_END", b.data, true);
    jbuf_free(&b);
    return ok;
}

static int journal_print_file(const char *path, FILE *out, bool summary_only) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    unsigned long long n = 0, bad = 0;
    for (;;) {
        unsigned char hdr[8];
        size_t got = fread(hdr, 1, sizeof(hdr), fp);
        if (got == 0) break;
        if (got != sizeof(hdr)) { bad++; break; }
        uint32_t len = load_le32(hdr);
        uint32_t want_crc = load_le32(hdr + 4);
        if (len == 0 || len > (32U * 1024U * 1024U)) { bad++; break; }
        char *buf = (char *)malloc((size_t)len + 1);
        if (!buf) { bad++; break; }
        got = fread(buf, 1, len, fp);
        if (got != len) { free(buf); bad++; break; }
        buf[len] = 0;
        uint32_t got_crc = crc32_update(0, buf, len);
        if (got_crc != want_crc) { free(buf); bad++; break; }
        n++;
        if (!summary_only) fputs(buf, out);
        free(buf);
    }
    fclose(fp);
    if (summary_only) fprintf(out, "%llu records%s\n", n, bad ? " (truncated/corrupt tail ignored)" : "");
    return bad ? 1 : 0;
}

static void runs_dir_path(char *out, size_t out_len) {
    const char *override = getenv("DSCO_RUNS_DIR");
    if (override && override[0]) snprintf(out, out_len, "%s", override);
    else {
        const char *home = getenv("HOME");
        snprintf(out, out_len, "%s/.dsco/runs", home && home[0] ? home : ".");
    }
}

/* ── runs list / gc (Wave B P1.3 durable execution) ──────────────────── */

typedef struct {
    char run_id[64];
    char status[24];
    long long started_at;
    long long ended_at;
    long long records;   /* manifest seq; 0 when manifest absent */
    long long bytes;     /* journal.wal size */
} run_entry_t;

/* Read a small file into a malloc'd NUL-terminated buffer. NULL on failure. */
static char *read_small_file(const char *path, size_t max_bytes) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > max_bytes)
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t n = (size_t)st.st_size;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, n, fp);
    fclose(fp);
    if (got != n) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

static bool looks_like_run_id(const char *s) {
    /* 8-4-4-4-12 hex; conservative guard for gc deletion targets. */
    if (strlen(s) != 36) return false;
    for (int i = 0; s[i]; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* Populate one run entry from manifest.json + journal stat. */
static void runs_scan_one(const char *runs_dir, const char *name, run_entry_t *e) {
    memset(e, 0, sizeof(*e));
    snprintf(e->run_id, sizeof(e->run_id), "%s", name);
    snprintf(e->status, sizeof(e->status), "unknown");

    char mp[PATH_MAX], jp[PATH_MAX];
    snprintf(mp, sizeof(mp), "%s/%s/manifest.json", runs_dir, name);
    snprintf(jp, sizeof(jp), "%s/%s/journal.wal", runs_dir, name);

    char *mj = read_small_file(mp, 64 * 1024);
    if (mj) {
        char *status = json_get_str(mj, "status");
        if (status) {
            snprintf(e->status, sizeof(e->status), "%s", status);
            free(status);
        }
        e->started_at = json_get_i64(mj, "started_at", 0);
        e->ended_at = json_get_i64(mj, "ended_at", 0);
        e->records = json_get_i64(mj, "seq", 0);
        free(mj);
    }
    struct stat st;
    if (stat(jp, &st) == 0) e->bytes = (long long)st.st_size;
    if (e->started_at == 0 && stat(mp, &st) == 0) e->started_at = (long long)st.st_mtime;
}

static int runs_cmp_started_desc(const void *a, const void *b) {
    const run_entry_t *ra = (const run_entry_t *)a, *rb = (const run_entry_t *)b;
    if (rb->started_at != ra->started_at) return rb->started_at < ra->started_at ? -1 : 1;
    return strcmp(ra->run_id, rb->run_id);
}

static void runs_print_iso(long long epoch, char *out, size_t out_len) {
    if (epoch <= 0) { snprintf(out, out_len, "-"); return; }
    time_t t = (time_t)epoch;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) { snprintf(out, out_len, "-"); return; }
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* Human-readable byte count (<= 4 significant chars, e.g. 5.9K, 12M). */
static void runs_print_bytes(long long n, char *out, size_t out_len) {
    if (n >= 1024 * 1024) snprintf(out, out_len, "%.1fM", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024) snprintf(out, out_len, "%.1fK", (double)n / 1024.0);
    else snprintf(out, out_len, "%lldB", n);
}

/* Bounded recursive delete. Returns bytes unlinked, or -1 on error. */
static long long runs_rm_tree(const char *dir) {
    if (!path_is_safe_dir(dir)) return -1;
    long long freed = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof(p), "%s/%s", dir, ent->d_name) >= (int)sizeof(p)) continue;
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            long long sub = runs_rm_tree(p);
            if (sub < 0) { closedir(d); return -1; }
            freed += sub;
        } else {
            if (unlink(p) != 0) { closedir(d); return -1; }
            freed += (long long)st.st_size;
        }
    }
    closedir(d);
    if (rmdir(dir) != 0) return -1;
    return freed;
}

static int runs_cmd_list(const char *runs_dir, int argc, char **argv) {
    long limit = 20;
    bool json = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = strtol(argv[++i], NULL, 10);
            if (limit < 0) limit = 20;
        } else if (strcmp(argv[i], "--all") == 0) limit = 0;
        else { fprintf(stderr, "usage: dsco runs list [--limit N|--all] [--json]\n"); return 2; }
    }
    DIR *d = opendir(runs_dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "cannot open runs directory: %s\n", runs_dir);
        return 1;
    }
    size_t cap = 256, n = 0;
    run_entry_t *ents = (run_entry_t *)malloc(cap * sizeof(*ents));
    if (!ents) { closedir(d); fprintf(stderr, "out of memory\n"); return 1; }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char jp[PATH_MAX];
        if (snprintf(jp, sizeof(jp), "%s/%s/journal.wal", runs_dir, ent->d_name) >= (int)sizeof(jp)) continue;
        struct stat st;
        if (stat(jp, &st) != 0) continue; /* not a run dir */
        if (n == cap) {
            cap *= 2;
            run_entry_t *ne = (run_entry_t *)realloc(ents, cap * sizeof(*ents));
            if (!ne) { free(ents); closedir(d); fprintf(stderr, "out of memory\n"); return 1; }
            ents = ne;
        }
        runs_scan_one(runs_dir, ent->d_name, &ents[n++]);
    }
    closedir(d);
    qsort(ents, n, sizeof(*ents), runs_cmp_started_desc);
    size_t shown = (limit > 0 && (size_t)limit < n) ? (size_t)limit : n;
    if (json) {
        printf("{\"schema\":\"dsco.runs.list.v1\",\"total\":%zu,\"shown\":%zu}\n", n, shown);
        for (size_t i = 0; i < shown; i++) {
            run_entry_t *e = &ents[i];
            printf("{\"run_id\":\"%s\",\"status\":\"%s\",\"started_at\":%lld,"
                   "\"ended_at\":%lld,\"records\":%lld,\"bytes\":%lld}\n",
                   e->run_id, e->status, e->started_at, e->ended_at, e->records, e->bytes);
        }
    } else {
        printf("%-36s  %-10s  %-19s  %-7s  %5s  %7s\n",
               "RUN-ID", "STATUS", "STARTED", "DUR", "RECS", "SIZE");
        for (size_t i = 0; i < shown; i++) {
            run_entry_t *e = &ents[i];
            char started[24], size_s[16];
            runs_print_iso(e->started_at, started, sizeof(started));
            runs_print_bytes(e->bytes, size_s, sizeof(size_s));
            long long dur_s = (e->ended_at > 0 && e->started_at > 0) ? e->ended_at - e->started_at : -1;
            char dur_s_str[16];
            if (dur_s < 0) snprintf(dur_s_str, sizeof(dur_s_str), "-");
            else if (dur_s < 60) snprintf(dur_s_str, sizeof(dur_s_str), "%llds", dur_s);
            else if (dur_s < 3600) snprintf(dur_s_str, sizeof(dur_s_str), "%lldm", dur_s / 60);
            else snprintf(dur_s_str, sizeof(dur_s_str), "%lldh", dur_s / 3600);
            printf("%-36s  %-10s  %-19s  %-7s  %5lld  %7s\n",
                   e->run_id, e->status, started, dur_s_str, e->records, size_s);
        }
        if (n > shown) printf("… %zu more (use --limit/--all)\n", n - shown);
    }
    free(ents);
    return 0;
}

static int runs_cmd_gc(const char *runs_dir, int argc, char **argv) {
    long days = 30;
    bool dry_run = false, include_running = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--days") == 0 && i + 1 < argc) {
            days = strtol(argv[++i], NULL, 10);
            if (days < 0) days = 0;
        } else if (strcmp(argv[i], "--dry-run") == 0) dry_run = true;
        else if (strcmp(argv[i], "--include-running") == 0) include_running = true;
        else { fprintf(stderr, "usage: dsco runs gc [--days N] [--dry-run] [--include-running]\n"); return 2; }
    }
    DIR *d = opendir(runs_dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "cannot open runs directory: %s\n", runs_dir);
        return 1;
    }
    long long cutoff = (long long)time(NULL) - (long long)days * 86400;
    long long freed_bytes = 0;
    long scanned = 0, eligible = 0, deleted = 0, kept_running = 0, failed = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (!looks_like_run_id(ent->d_name)) continue; /* never touch non-run dirs */
        char rd[PATH_MAX];
        if (snprintf(rd, sizeof(rd), "%s/%s", runs_dir, ent->d_name) >= (int)sizeof(rd)) continue;
        scanned++;
        run_entry_t e;
        runs_scan_one(runs_dir, ent->d_name, &e);
        if (strcmp(e.status, "running") == 0 && !include_running) { kept_running++; continue; }
        if (e.started_at >= cutoff) continue;
        eligible++;
        if (dry_run) {
            printf("would delete %s (status=%s, started %lld)\n", ent->d_name, e.status, e.started_at);
            continue;
        }
        long long freed = runs_rm_tree(rd);
        if (freed < 0) { failed++; fprintf(stderr, "gc: failed to remove %s\n", ent->d_name); }
        else { deleted++; freed_bytes += freed; }
    }
    closedir(d);
    char size_s[16];
    runs_print_bytes(freed_bytes, size_s, sizeof(size_s));
    if (dry_run)
        printf("gc dry-run: %ld scanned, %ld eligible (older than %ld days), %ld running kept\n",
               scanned, eligible, days, kept_running);
    else
        printf("gc: %ld scanned, %ld deleted, %ld failed, %s freed, %ld running kept\n",
               scanned, deleted, failed, size_s, kept_running);
    return failed > 0 ? 1 : 0;
}

/* ── replay engine (Wave B P2.1 durable execution) ────────────────────── */

typedef struct {
    char *tool_id;
    bool matched;
} replay_call_ref_t;

/* Conservative idempotency classification (P2.3 will move this to the
 * registry; until every core tool is audited we default non-read tools to
 * "interrupted" = surface to model, never auto-re-run). */
static const char *replay_policy_for_tool(const char *tool) {
    static const char *idempotent[] = {
        "read_file", "page_file", "head_tail", "grep_files", "find_files",
        "Glob", "list_directory", "file_info", "file_hash", "word_count",
        "api_outline", "inspect_file", "symbol_refs", "ls", "cat", "stat",
        NULL
    };
    if (!tool) return "interrupted";
    for (int i = 0; idempotent[i]; i++)
        if (strcmp(tool, idempotent[i]) == 0) return "idempotent";
    return "interrupted";
}

/* Emit one replay step as a JSON line. `data_json` is the raw nested payload
 * object (already known to be JSON); emitted inline as "data". */
static void replay_emit_step(FILE *out, long long seq, long long wall_ms,
                             const char *type, const char *event_name,
                             const char *tool, const char *tool_id,
                             const char *policy, bool frontier,
                             const char *data_json) {
    if (!out) return; /* summary-only mode */
    fprintf(out, "{\"schema\":\"dsco.replay.step.v1\",\"seq\":%lld,\"wall_ms\":%lld,"
                 "\"type\":\"%s\",\"event\":\"%s\"",
            seq, wall_ms, type, event_name ? event_name : "");
    if (tool)     fprintf(out, ",\"tool\":\"%s\"", tool);
    if (tool_id)  fprintf(out, ",\"tool_id\":\"%s\"", tool_id);
    if (policy)   fprintf(out, ",\"replay_policy\":\"%s\"", policy);
    if (frontier) fprintf(out, ",\"frontier\":true");
    fprintf(out, ",\"data\":%s}\n", data_json && data_json[0] ? data_json : "null");
}

/* Pass 1: collect tool.result tool_ids into a growable set. */
static bool replay_collect_results(FILE *fp, replay_call_ref_t **out, size_t *out_n,
                                   bool *out_has_canonical) {
    size_t cap = 32, n = 0;
    bool has_canonical = false;
    replay_call_ref_t *set = (replay_call_ref_t *)malloc(cap * sizeof(*set));
    if (!set) return false;
    rewind(fp);
    for (;;) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint32_t len = load_le32(hdr), want_crc = load_le32(hdr + 4);
        if (len == 0 || len > (32U * 1024U * 1024U)) break;
        char *buf = (char *)malloc(len + 1);
        if (!buf) break;
        if (fread(buf, 1, len, fp) != len) { free(buf); break; }
        buf[len] = 0;
        if (crc32_update(0, buf, len) != want_crc) { free(buf); break; }
        char *type = json_get_str(buf, "type");
        if (type && (strcmp(type, "RUN_START") == 0 || strcmp(type, "TURN_START") == 0 ||
                     strcmp(type, "TOOL_CALL") == 0 || strcmp(type, "TOOL_RESULT") == 0 ||
                     strcmp(type, "CHECKPOINT") == 0 || strcmp(type, "RUN_END") == 0))
            has_canonical = true;
        if (type && strcmp(type, "tool.result") == 0) {
            /* lowercase agent-event: tool_id at depth 3 (frame.payload.payload) */
            char *env = json_get_raw(buf, "payload");
            char *data = env ? json_get_raw(env, "payload") : NULL;
            char *tid = data ? json_get_str(data, "tool_id") : NULL;
            free(env); free(data);
            if (tid) {
                if (n == cap) {
                    cap *= 2;
                    replay_call_ref_t *ns = (replay_call_ref_t *)realloc(set, cap * sizeof(*set));
                    if (!ns) { free(tid); free(type); free(buf); break; }
                    set = ns;
                }
                set[n].tool_id = tid;
                set[n].matched = false;
                n++;
            }
        } else if (type && strcmp(type, "TOOL_RESULT") == 0) {
            /* canonical W1: call_id at depth 2 (frame.payload) */
            char *env = json_get_raw(buf, "payload");
            char *tid = env ? json_get_str(env, "call_id") : NULL;
            free(env);
            if (tid && tid[0]) {
                if (n == cap) {
                    cap *= 2;
                    replay_call_ref_t *ns = (replay_call_ref_t *)realloc(set, cap * sizeof(*set));
                    if (!ns) { free(tid); free(type); free(buf); break; }
                    set = ns;
                }
                set[n].tool_id = tid;
                set[n].matched = false;
                n++;
            } else free(tid);
        }
        free(type);
        free(buf);
    }
    *out = set;
    *out_n = n;
    if (out_has_canonical) *out_has_canonical = has_canonical;
    return true;
}

static bool replay_id_in_set(replay_call_ref_t *set, size_t n, const char *id, size_t *idx) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(set[i].tool_id, id) == 0) { if (idx) *idx = i; return true; }
    }
    return false;
}

/* Reconstruct a run's steps from its journal. Returns false when the journal
 * cannot be opened at all; a torn/corrupt tail is reported via
 * summary->corrupt_tail rather than failing the whole replay. */
bool chronicle_replay_run(const char *runs_dir, const char *run_id, FILE *out,
                          chronicle_replay_summary_t *summary) {
    char jp[PATH_MAX];
    snprintf(jp, sizeof(jp), "%s/%s/journal.wal", runs_dir, run_id);
    memset(summary, 0, sizeof(*summary));
    FILE *fp = fopen(jp, "rb");
    if (!fp) return false;

    replay_call_ref_t *results = NULL;
    size_t n_results = 0;
    bool has_canonical = false;
    replay_collect_results(fp, &results, &n_results, &has_canonical);

    rewind(fp);
    for (;;) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint32_t len = load_le32(hdr), want_crc = load_le32(hdr + 4);
        if (len == 0 || len > (32U * 1024U * 1024U)) { summary->corrupt_tail = true; break; }
        char *buf = (char *)malloc(len + 1);
        if (!buf) { summary->corrupt_tail = true; break; }
        if (fread(buf, 1, len, fp) != len) { free(buf); summary->corrupt_tail = true; break; }
        buf[len] = 0;
        if (crc32_update(0, buf, len) != want_crc) { free(buf); summary->corrupt_tail = true; break; }
        summary->records++;

        long long seq = json_get_i64(buf, "seq", 0);
        long long wall = json_get_i64(buf, "wall_ms", 0);
        char *type = json_get_str(buf, "type");
        char *env = json_get_raw(buf, "payload");   /* depth 1 */
        bool canonical = type && (strcmp(type, "TOOL_CALL") == 0 || strcmp(type, "TOOL_RESULT") == 0 ||
                                  strcmp(type, "TURN_START") == 0 || strcmp(type, "CHECKPOINT") == 0 ||
                                  strcmp(type, "RUN_START") == 0 || strcmp(type, "RUN_END") == 0);
        /* Canonical W1 frames: env IS the event data. Lowercase agent-event
         * frames nest the event data one level deeper (env.payload). */
        char *data = canonical ? (env ? strdup(env) : NULL) : (env ? json_get_raw(env, "payload") : NULL);
        const char *dn = data && data[0] ? data : "null";

        if (type && strcmp(type, "tool.call") == 0) {
            char *tool = data ? json_get_str(data, "tool") : NULL;
            char *tid = data ? json_get_str(data, "tool_id") : NULL;
            if (has_canonical) {
                /* canonical TOOL_CALL is authoritative; lowercase is a mirror. */
                replay_emit_step(out, seq, wall, "record", "tool.call", tool, tid, NULL, false, dn);
            } else {
                bool frontier = false;
                if (tid) frontier = !replay_id_in_set(results, n_results, tid, NULL);
                if (frontier) summary->frontier_calls++;
                replay_emit_step(out, seq, wall, "tool.call", "tool.call", tool, tid,
                                 replay_policy_for_tool(tool), frontier, dn);
                summary->tool_calls++;
            }
            free(tool); free(tid);
        } else if (type && strcmp(type, "TOOL_CALL") == 0) {
            char *tool = env ? json_get_str(env, "tool") : NULL;
            char *tid = env ? json_get_str(env, "call_id") : NULL;
            if (tid && !tid[0]) { free(tid); tid = NULL; }
            bool frontier = false;
            if (tid) frontier = !replay_id_in_set(results, n_results, tid, NULL);
            if (frontier) summary->frontier_calls++;
            replay_emit_step(out, seq, wall, "tool.call", "TOOL_CALL", tool, tid,
                             replay_policy_for_tool(tool), frontier, dn);
            summary->tool_calls++;
            free(tool); free(tid);
        } else if (type && strcmp(type, "tool.result") == 0) {
            char *tool = data ? json_get_str(data, "tool") : NULL;
            char *tid = data ? json_get_str(data, "tool_id") : NULL;
            replay_emit_step(out, seq, wall, has_canonical ? "record" : "tool.result",
                             "tool.result", tool, tid, NULL, false, dn);
            if (!has_canonical) summary->tool_results++;
            free(tool); free(tid);
        } else if (type && strcmp(type, "TOOL_RESULT") == 0) {
            char *tool = env ? json_get_str(env, "tool") : NULL;
            char *tid = env ? json_get_str(env, "call_id") : NULL;
            if (tid && !tid[0]) { free(tid); tid = NULL; }
            replay_emit_step(out, seq, wall, "tool.result", "TOOL_RESULT", tool, tid,
                             NULL, false, dn);
            summary->tool_results++;
            free(tool); free(tid);
        } else if (type && strcmp(type, "turn.started") == 0) {
            replay_emit_step(out, seq, wall, has_canonical ? "record" : "turn",
                             "turn.started", NULL, NULL, NULL, false, dn);
            if (!has_canonical) summary->turns++;
        } else if (type && strcmp(type, "TURN_START") == 0) {
            replay_emit_step(out, seq, wall, "turn", "TURN_START", NULL, NULL, NULL, false, dn);
            summary->turns++;
        } else if (type && strcmp(type, "turn.checkpoint") == 0) {
            replay_emit_step(out, seq, wall, has_canonical ? "record" : "checkpoint",
                             "turn.checkpoint", NULL, NULL, NULL, false, dn);
            if (!has_canonical) {
                summary->checkpoints++;
                if (data) summary->cost_usd += json_get_double(data, "cost_usd", 0.0);
            }
        } else if (type && strcmp(type, "CHECKPOINT") == 0) {
            replay_emit_step(out, seq, wall, "checkpoint", "CHECKPOINT", NULL, NULL, NULL, false, dn);
            summary->checkpoints++;
            if (env) summary->cost_usd += json_get_double(env, "cost_usd", 0.0);
        } else if (type && strcmp(type, "run.started") == 0) {
            replay_emit_step(out, seq, wall, "run", "run.started", NULL, NULL, NULL, false, dn);
        } else if (type && strcmp(type, "run.completed") == 0) {
            replay_emit_step(out, seq, wall, "run", "run.completed", NULL, NULL, NULL, false, dn);
        } else if (type && strcmp(type, "RUN_START") == 0) {
            replay_emit_step(out, seq, wall, "run", "RUN_START", NULL, NULL, NULL, false, dn);
        } else if (type && strcmp(type, "RUN_END") == 0) {
            replay_emit_step(out, seq, wall, "run", "RUN_END", NULL, NULL, NULL, false, dn);
        } else {
            replay_emit_step(out, seq, wall, "record", type ? type : "unknown", NULL, NULL, NULL, false, dn);
        }
        free(type); free(env); free(data);
        free(buf);
    }
    fclose(fp);
    for (size_t i = 0; i < n_results; i++) free(results[i].tool_id);
    free(results);
    return true;
}

static int runs_cmd_replay(const char *runs_dir, int argc, char **argv) {
    if (argc < 4 || argv[3][0] == '-') {
        fprintf(stderr, "usage: dsco runs replay <run-id> [--summary]\n");
        return 2;
    }
    const char *run_id = argv[3];
    bool summary_only = false;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--summary") == 0) summary_only = true;
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 2; }
    }
    /* run-id guard: same shape rule as gc — replay is read-only but keep the
     * corpus path predictable. */
    if (!looks_like_run_id(run_id)) {
        fprintf(stderr, "not a run id: %s\n", run_id);
        return 2;
    }
    chronicle_replay_summary_t s;
    bool ok;
    if (summary_only) {
        ok = chronicle_replay_run(runs_dir, run_id, NULL, &s);
    } else {
        ok = chronicle_replay_run(runs_dir, run_id, stdout, &s);
    }
    if (!ok) { fprintf(stderr, "cannot open journal for run %s\n", run_id); return 1; }
    printf("{\"schema\":\"dsco.replay.summary.v1\",\"run_id\":\"%s\",\"records\":%lld,"
           "\"turns\":%lld,\"tool_calls\":%lld,\"tool_results\":%lld,"
           "\"frontier_calls\":%lld,\"checkpoints\":%lld,\"cost_usd\":%.6f,"
           "\"corrupt_tail\":%s}\n",
           run_id, s.records, s.turns, s.tool_calls, s.tool_results,
           s.frontier_calls, s.checkpoints, s.cost_usd, s.corrupt_tail ? "true" : "false");
    return 0;
}

int chronicle_runs_cli(int argc, char **argv) {
    const char *cmd = argc >= 3 ? argv[2] : "list";
    char runs[PATH_MAX];
    runs_dir_path(runs, sizeof(runs));
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        printf("Usage: dsco runs [list [--limit N|--all] [--json]|show <run-id>|check <run-id>|replay <run-id> [--summary]|gc [--days N] [--dry-run] [--include-running]]\n"
               "\n"
               "Inspect crash-safe Chronicle run journals without creating a new run.\n");
        return 0;
    }
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
        return runs_cmd_list(runs, argc, argv);
    if (strcmp(cmd, "show") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: dsco runs show <run-id>\n"); return 2; }
        char jp[PATH_MAX];
        snprintf(jp, sizeof(jp), "%s/%s/journal.wal", runs, argv[3]);
        return journal_print_file(jp, stdout, false) < 0 ? 1 : 0;
    }
    if (strcmp(cmd, "check") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: dsco runs check <run-id>\n"); return 2; }
        char jp[PATH_MAX];
        snprintf(jp, sizeof(jp), "%s/%s/journal.wal", runs, argv[3]);
        int rc = journal_print_file(jp, stdout, true);
        return rc < 0 ? 1 : rc;
    }
    if (strcmp(cmd, "gc") == 0)
        return runs_cmd_gc(runs, argc, argv);
    if (strcmp(cmd, "replay") == 0)
        return runs_cmd_replay(runs, argc, argv);
    fprintf(stderr, "usage: dsco runs [list|show <run-id>|check <run-id>|replay <run-id>|gc]\n");
    return 2;
}

static bool journal_disabled_env(void) {
    const char *v = getenv("DSCO_JOURNAL");
    return v && (strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0);
}

static bool chronicle_run_manifest_write(const char *status) {
    if (!g_chronicle.ready || !g_chronicle.journal_path[0]) return false;
    const char *run_dir = chronicle_run_dir();
    if (!run_dir || !run_dir[0] || !path_is_safe_dir(run_dir)) return false;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", run_dir);
    struct stat st;
    if (lstat(path, &st) == 0 && (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) return false;

    char cwd[PATH_MAX];
    const char *cwdp = getcwd(cwd, sizeof(cwd)) ? cwd : "";
    long long ended_at = 0;
    if (status && strcmp(status, "running") != 0) ended_at = (long long)time(NULL);

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\n  \"schema\": \"dsco.run_manifest.v1\",");
    jbuf_append(&b, "\n  \"run_id\": "); jbuf_append_json_str(&b, g_chronicle.session_id);
    jbuf_append(&b, ",\n  \"session_id\": "); jbuf_append_json_str(&b, g_chronicle.session_id);
    jbuf_append(&b, ",\n  \"installation_id\": "); jbuf_append_json_str(&b, g_chronicle.installation_id);
    jbuf_append(&b, ",\n  \"status\": "); jbuf_append_json_str(&b, status && status[0] ? status : "running");
    jbuf_appendf(&b, ",\n  \"started_at\": %lld", (long long)g_chronicle.started_at);
    if (ended_at > 0) jbuf_appendf(&b, ",\n  \"ended_at\": %lld", ended_at);
    else jbuf_append(&b, ",\n  \"ended_at\": null");
    jbuf_appendf(&b, ",\n  \"updated_at\": %lld", (long long)time(NULL));
    jbuf_append(&b, ",\n  \"schema_version\": "); jbuf_append_json_str(&b, CHRONICLE_SCHEMA_VERSION);
    jbuf_append(&b, ",\n  \"capture_mode\": "); jbuf_append_json_str(&b, mode_str(g_chronicle.mode));
    jbuf_append(&b, ",\n  \"instance_id\": "); jbuf_append_json_str(&b, g_chronicle.instance_id);
    jbuf_append(&b, ",\n  \"chronicle_root\": "); jbuf_append_json_str(&b, g_chronicle.root);
    jbuf_append(&b, ",\n  \"db_path\": "); jbuf_append_json_str(&b, g_chronicle.db_path);
    jbuf_append(&b, ",\n  \"event_log_path\": "); jbuf_append_json_str(&b, g_chronicle.event_log_path);
    jbuf_append(&b, ",\n  \"journal_path\": "); jbuf_append_json_str(&b, g_chronicle.journal_path);
    jbuf_append(&b, ",\n  \"cwd\": "); jbuf_append_json_str(&b, cwdp);
    jbuf_appendf(&b, ",\n  \"seq\": %llu", g_chronicle.seq);
    jbuf_append(&b, "\n}\n");

    bool ok = write_file_atomic_replace(path, b.data, 0644);
    jbuf_free(&b);
    return ok;
}

static bool chronicle_journal_open(void) {
    if (!g_chronicle.ready || journal_disabled_env()) return false;
    const char *home = getenv("HOME");
    char runs_root[PATH_MAX];
    const char *override = getenv("DSCO_RUNS_DIR");
    if (override && override[0]) snprintf(runs_root, sizeof(runs_root), "%s", override);
    else snprintf(runs_root, sizeof(runs_root), "%s/.dsco/runs", home && home[0] ? home : ".");
    snprintf(g_chronicle.journal_path, sizeof(g_chronicle.journal_path), "%s/%s/journal.wal", runs_root, g_chronicle.session_id);
    if (!ensure_parent_dir(g_chronicle.journal_path)) return false;
    const char *run_dir = chronicle_run_dir();
    if (!run_dir || !run_dir[0] || !path_is_safe_dir(run_dir)) return false;
    if (!chronicle_run_manifest_write("running")) return false;
    g_chronicle.journal_fd = open(g_chronicle.journal_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (g_chronicle.journal_fd < 0) return false;
    /* W1 startup durability: make the journal's directory entry durable before
     * any frame is appended. */
    int journal_dfd = open(run_dir, O_RDONLY);
    if (journal_dfd >= 0) { fsync(journal_dfd); close(journal_dfd); }
    g_chronicle.journal_enabled = true;
    jbuf_t p;
    jbuf_init(&p, 512);
    jbuf_append(&p, "{\"schema\":\"dsco.run_journal.v1\",\"run_id\":");
    jbuf_append_json_str(&p, g_chronicle.session_id);
    jbuf_append(&p, ",\"chronicle_root\":"); jbuf_append_json_str(&p, g_chronicle.root);
    jbuf_append(&p, ",\"db_path\":"); jbuf_append_json_str(&p, g_chronicle.db_path);
    jbuf_append(&p, ",\"cwd\":");
    char cwd[PATH_MAX];
    jbuf_append_json_str(&p, getcwd(cwd, sizeof(cwd)) ? cwd : "");
    jbuf_append(&p, "}");
    bool ok = agent_event_emit_simple("run.started", "ok", p.data,
                                      AGENT_EVENT_DURABLE | AGENT_EVENT_CALLBACK);
    jbuf_free(&p);
    if (ok) chronicle_journal_run_start(NULL, NULL, NULL); /* canonical run boundary */
    return ok;
}

/* W1 (durable execution): one writer at a time. Sequence allocation and both
 * write_all_fd() calls must be atomic with respect to parallel agent/worker
 * paths, which can otherwise interleave frames or race the shared seq. */
static pthread_mutex_t g_journal_write_mu = PTHREAD_MUTEX_INITIALIZER;

/* W1: the six canonical run-journal record types (dsco.run_journal.v1). */
static bool journal_type_is_canonical(const char *t) {
    static const char *const k_canonical[] = {
        "RUN_START", "TURN_START", "TOOL_CALL",
        "TOOL_RESULT", "CHECKPOINT", "RUN_END",
    };
    if (!t || !t[0]) return false;
    for (size_t i = 0; i < sizeof(k_canonical) / sizeof(k_canonical[0]); i++)
        if (strcmp(t, k_canonical[i]) == 0) return true;
    return false;
}

bool chronicle_journal_append(const char *record_type, const char *payload_json, bool durable) {
    if (!g_chronicle.ready || !g_chronicle.journal_enabled || g_chronicle.journal_fd < 0) return false;
    if (!record_type || !record_type[0]) return false;
    /* W1: canonical execution records may never rely on process exit to flush. */
    if (journal_type_is_canonical(record_type)) durable = true;
    jbuf_t rec;
    jbuf_init(&rec, 1024);
    bool ok = false;
    pthread_mutex_lock(&g_journal_write_mu);
    do {
        long long ms = 0;
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0) ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
        jbuf_append(&rec, "{\"v\":1,\"type\":"); jbuf_append_json_str(&rec, record_type);
        jbuf_append(&rec, ",\"run_id\":"); jbuf_append_json_str(&rec, g_chronicle.session_id);
        jbuf_appendf(&rec, ",\"seq\":%llu,\"wall_ms\":%lld,\"payload\":", ++g_chronicle.seq, ms);
        if (payload_json && json_is_valid_container(payload_json)) jbuf_append(&rec, payload_json);
        else { jbuf_append(&rec, "{\"text\":"); jbuf_append_json_str(&rec, nz(payload_json)); jbuf_append(&rec, "}"); }
        jbuf_append(&rec, "}\n");
        /* W1: the scanner rejects len==0 or len>32MB; never emit an
         * unreadable frame, and the length must stay within u32. */
        if (rec.len == 0 || rec.len > (32ULL * 1024ULL * 1024ULL)) break;
        uint32_t len = (uint32_t)rec.len;
        uint32_t crc = crc32_update(0, rec.data, rec.len);
        unsigned char hdr[8];
        hdr[0] = (unsigned char)(len & 0xff); hdr[1] = (unsigned char)((len >> 8) & 0xff);
        hdr[2] = (unsigned char)((len >> 16) & 0xff); hdr[3] = (unsigned char)((len >> 24) & 0xff);
        hdr[4] = (unsigned char)(crc & 0xff); hdr[5] = (unsigned char)((crc >> 8) & 0xff);
        hdr[6] = (unsigned char)((crc >> 16) & 0xff); hdr[7] = (unsigned char)((crc >> 24) & 0xff);
        ok = write_all_fd(g_chronicle.journal_fd, hdr, sizeof(hdr)) && write_all_fd(g_chronicle.journal_fd, rec.data, rec.len);
        if (ok && durable) ok = (fsync(g_chronicle.journal_fd) == 0);
    } while (0);
    pthread_mutex_unlock(&g_journal_write_mu);
    jbuf_free(&rec);
    return ok;
}

/* W1: canonical TOOL_CALL record (dsco.run_journal.v1). Callers must journal
 * this before tools_execute_for_tier(); durable=true forces fsync here so the
 * call is on disk before the tool may cause an external effect. call_id is the
 * replay/idempotency key (provider tool id). */
bool chronicle_journal_tool_call(const char *turn_id, const char *call_id,
                                 const char *tool, const char *input_json,
                                 const char *trust_tier, const char *trace_id) {
    if (!tool || !tool[0]) return false;
    char in_sha[65];
    sha256_hex(input_json ? input_json : "", input_json ? strlen(input_json) : 0, in_sha);
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"turn_id\":");
    if (turn_id && turn_id[0]) jbuf_append_json_str(&b, turn_id); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"call_id\":");
    jbuf_append_json_str(&b, call_id && call_id[0] ? call_id : "");
    jbuf_append(&b, ",\"tool\":"); jbuf_append_json_str(&b, tool);
    jbuf_append(&b, ",\"input\":");
    if (input_json && json_is_valid_container(input_json)) jbuf_append(&b, input_json);
    else { jbuf_append(&b, "{\"text\":"); jbuf_append_json_str(&b, nz(input_json)); jbuf_append(&b, "}"); }
    jbuf_append(&b, ",\"input_sha256\":"); jbuf_append_json_str(&b, in_sha);
    jbuf_append(&b, ",\"trust_tier\":");
    if (trust_tier && trust_tier[0]) jbuf_append_json_str(&b, trust_tier); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"trace_id\":");
    if (trace_id && trace_id[0]) jbuf_append_json_str(&b, trace_id); else jbuf_append(&b, "null");
    jbuf_append(&b, "}");
    bool ok = chronicle_journal_append("TOOL_CALL", b.data, true);
    jbuf_free(&b);
    return ok;
}

/* W1: canonical TOOL_RESULT record. Must be emitted on every exit path after
 * dispatch returns, including denied, timed-out, and failed calls. Full result
 * text is inlined up to 64KB; larger results go to a content-addressed blob
 * and only the blob reference is journaled. result_sha256 always hashes the
 * full text. */
bool chronicle_journal_tool_result(const char *turn_id, const char *call_id,
                                   const char *tool, bool ok, bool cached, bool timed_out,
                                   double elapsed_ms, const char *result_text) {
    size_t n = result_text ? strlen(result_text) : 0;
    char r_sha[65];
    sha256_hex(result_text ? result_text : "", n, r_sha);
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"turn_id\":");
    if (turn_id && turn_id[0]) jbuf_append_json_str(&b, turn_id); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"call_id\":");
    jbuf_append_json_str(&b, call_id && call_id[0] ? call_id : "");
    jbuf_append(&b, ",\"tool\":"); jbuf_append_json_str(&b, tool ? tool : "");
    jbuf_appendf(&b, ",\"ok\":%s,\"cached\":%s,\"timed_out\":%s,\"elapsed_ms\":%.3f",
                 ok ? "true" : "false", cached ? "true" : "false",
                 timed_out ? "true" : "false", elapsed_ms);
    jbuf_append(&b, ",\"result\":");
    if (n <= (64ULL * 1024ULL)) {
        jbuf_append_json_str(&b, result_text ? result_text : "");
    } else {
        char bsha[65] = "";
        chronicle_blob_put_text(result_text, "journal.tool_result", "local", bsha, sizeof(bsha));
        jbuf_append(&b, "{\"blob_sha256\":");
        jbuf_append_json_str(&b, bsha[0] ? bsha : "");
        jbuf_appendf(&b, ",\"byte_len\":%zu}", n);
    }
    jbuf_append(&b, ",\"result_sha256\":"); jbuf_append_json_str(&b, r_sha);
    jbuf_append(&b, "}");
    bool rc = chronicle_journal_append("TOOL_RESULT", b.data, true);
    jbuf_free(&b);
    return rc;
}

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
        jbuf_append(&p, ",\"redacted\":true");
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
    int cache_side_tokens = input_tokens + cache_read_tokens + cache_write_tokens;
    double cache_hit_ratio =
        cache_side_tokens > 0 ? (double)cache_read_tokens / (double)cache_side_tokens : 0.0;
    jbuf_append(&p, ",\"usage\":{");
    jbuf_appendf(&p, "\"input_tokens\":%d,\"output_tokens\":%d,\"cache_read_tokens\":%d,\"cache_write_tokens\":%d,\"reasoning_tokens\":%d,\"cache_hit_ratio\":%.6f}",
                 input_tokens, output_tokens, cache_read_tokens, cache_write_tokens,
                 reasoning_tokens, cache_hit_ratio);
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

static void chronicle_cost_totals_add_payload(chronicle_cost_totals_t *out, const char *payload_json) {
    if (!out || !payload_json) return;
    out->response_count++;
    out->cost_usd += json_get_double(payload_json, "cost_usd", 0.0);
    char *usage = json_get_raw(payload_json, "usage");
    if (usage) {
        out->input_tokens += json_get_int(usage, "input_tokens", 0);
        out->output_tokens += json_get_int(usage, "output_tokens", 0);
        out->cache_read_tokens += json_get_int(usage, "cache_read_tokens", 0);
        out->cache_write_tokens += json_get_int(usage, "cache_write_tokens", 0);
        out->reasoning_tokens += json_get_int(usage, "reasoning_tokens", 0);
        free(usage);
    }
}

static bool chronicle_cost_totals_query(const char *sql, const char *session_id,
                                        sqlite3_int64 start_time, sqlite3_int64 end_time,
                                        chronicle_cost_totals_t *out) {
    if (!g_chronicle.db || !out) return false;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_chronicle.db, sql, -1, &st, NULL) != SQLITE_OK) return false;
    if (session_id) sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
    else {
        sqlite3_bind_int64(st, 1, start_time);
        sqlite3_bind_int64(st, 2, end_time);
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        chronicle_cost_totals_add_payload(out, (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool chronicle_cost_totals_for_session(const char *session_id, chronicle_cost_totals_t *out) {
    const char *sid = (session_id && session_id[0]) ? session_id : g_chronicle.session_id;
    if (!sid || !sid[0]) return false;
    return chronicle_cost_totals_query(
        "SELECT COALESCE(payload_json,'{}') FROM events "
        "WHERE event_type='llm.response.completed' AND session_id=?1;",
        sid, 0, 0, out);
}

bool chronicle_cost_totals_today(chronicle_cost_totals_t *out) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    time_t start = mktime(&tmv);
    if (start == (time_t)-1) return false;
    return chronicle_cost_totals_query(
        "SELECT COALESCE(payload_json,'{}') FROM events "
        "WHERE event_type='llm.response.completed' AND wall_time>=?1 AND wall_time<?2;",
        NULL, (sqlite3_int64)start, (sqlite3_int64)(start + 86400), out);
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

char *chronicle_build_activity_html_ex(int limit, const char *session_filter,
                                       const char *type_filter,
                                       const char *search_filter) {
    (void)type_filter;
    (void)search_filter;
    if (limit <= 0 || limit > 5000)
        limit = 1000;

    jbuf_t b;
    jbuf_init(&b, 32768);
    jbuf_append(&b,
                "<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>DSCO Chronicle</title>"
                "<style>"
                ":root{--bg:#05070d;--panel:#0b1220;--panel2:#0f1a2d;--line:#21314d;--text:#e8f1ff;--muted:#8da2c0;--accent:#7dd3fc;--good:#34d399;--warn:#fbbf24;--bad:#fb7185;--violet:#a78bfa;--blue:#60a5fa}"
                "*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0%,#14213f 0,#05070d 34%,#03050a 100%);color:var(--text);font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Helvetica,Arial;min-height:100vh}"
                "header{position:sticky;top:0;z-index:10;padding:18px 24px;border-bottom:1px solid rgba(125,211,252,.22);background:rgba(5,7,13,.82);backdrop-filter:blur(16px)}"
                ".top{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}.brand{display:flex;align-items:center;gap:12px}.orb{width:42px;height:42px;border-radius:50%;background:conic-gradient(from 180deg,var(--accent),var(--violet),var(--good),var(--accent));box-shadow:0 0 34px rgba(125,211,252,.45)}"
                "h1{font-size:24px;margin:0;letter-spacing:.02em}.sub{color:var(--muted);font-size:12px;margin-top:3px}.pill{display:inline-flex;align-items:center;gap:6px;padding:6px 10px;border:1px solid var(--line);background:rgba(15,26,45,.78);border-radius:999px;color:#b9c8df;font-size:12px}.dot{width:8px;height:8px;border-radius:50%;background:var(--good);box-shadow:0 0 18px var(--good)}"
                ".controls{display:grid;grid-template-columns:2fr 1fr 1fr auto auto;gap:10px;margin-top:16px}.controls input,.controls select,.controls button{border:1px solid var(--line);background:rgba(11,18,32,.92);color:var(--text);border-radius:12px;padding:10px 12px}.controls button{cursor:pointer;background:linear-gradient(135deg,#155e75,#3730a3);font-weight:700}.controls button:hover{filter:brightness(1.14)}"
                "main{padding:22px 24px 50px;display:grid;grid-template-columns:minmax(0,1fr) 360px;gap:18px}.stats{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:12px;margin-bottom:16px}.stat{background:linear-gradient(180deg,rgba(15,26,45,.92),rgba(11,18,32,.82));border:1px solid var(--line);border-radius:18px;padding:14px;box-shadow:0 10px 30px rgba(0,0,0,.22)}.stat .k{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}.stat .v{font-size:28px;font-weight:800;margin-top:6px}.stat.good .v{color:var(--good)}.stat.warn .v{color:var(--warn)}.stat.bad .v{color:var(--bad)}"
                ".rail{position:sticky;top:154px;align-self:start;background:rgba(11,18,32,.82);border:1px solid var(--line);border-radius:18px;padding:14px;max-height:calc(100vh - 174px);overflow:auto}.rail h3{margin:4px 0 10px}.bar{height:8px;background:#0a1020;border-radius:999px;overflow:hidden;margin:8px 0 12px}.bar>i{display:block;height:100%;background:linear-gradient(90deg,var(--accent),var(--violet));width:0%}.legend{display:flex;justify-content:space-between;gap:10px;color:var(--muted);font-size:12px;margin:6px 0}.typebtn{display:flex;width:100%;align-items:center;justify-content:space-between;border:1px solid var(--line);background:#0b1426;color:var(--text);border-radius:12px;padding:8px 10px;margin:7px 0;cursor:pointer}.typebtn.active{border-color:var(--accent);box-shadow:0 0 0 1px rgba(125,211,252,.28) inset}.mini{font-size:11px;color:var(--muted)}"
                ".timeline{position:relative}.timeline:before{content:'';position:absolute;left:18px;top:0;bottom:0;width:2px;background:linear-gradient(var(--accent),rgba(167,139,250,.18))}.event{position:relative;margin:0 0 12px 42px;background:rgba(11,18,32,.86);border:1px solid var(--line);border-radius:18px;padding:13px 14px;box-shadow:0 10px 24px rgba(0,0,0,.20)}.event:before{content:'';position:absolute;left:-31px;top:18px;width:12px;height:12px;border-radius:50%;background:var(--accent);box-shadow:0 0 18px var(--accent)}"
                ".event.tool:before{background:var(--good);box-shadow:0 0 18px var(--good)}.event.llm:before{background:var(--violet);box-shadow:0 0 18px var(--violet)}.event.err:before{background:var(--bad);box-shadow:0 0 18px var(--bad)}"
                ".evhead{display:flex;align-items:flex-start;justify-content:space-between;gap:12px}.etype{font-weight:800}.meta{color:var(--muted);font-size:12px;margin-top:4px}.chips{display:flex;gap:6px;flex-wrap:wrap;margin-top:9px}.chip{font-size:11px;border:1px solid var(--line);border-radius:999px;padding:3px 7px;color:#c5d4eb;background:#101b2f}.hash{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;color:#9dccff}.payload{display:none;margin-top:10px;white-space:pre-wrap;word-break:break-word;background:#050914;border:1px solid #1b2943;border-radius:12px;padding:10px;font:12px ui-monospace,SFMono-Regular,Menlo,monospace;color:#dbeafe;max-height:360px;overflow:auto}.event.open .payload{display:block}.actions{display:flex;gap:8px;flex-wrap:wrap}.ghost{background:transparent;border:1px solid var(--line);color:#bdd0ea;border-radius:10px;padding:6px 8px;cursor:pointer}.ghost:hover{border-color:var(--accent);color:white}"
                ".empty{border:1px dashed var(--line);border-radius:18px;padding:40px;text-align:center;color:var(--muted);background:rgba(11,18,32,.55)}a{color:var(--accent);text-decoration:none}@media(max-width:980px){main{grid-template-columns:1fr}.rail{position:relative;top:auto}.stats{grid-template-columns:repeat(2,1fr)}.controls{grid-template-columns:1fr}.timeline:before{display:none}.event{margin-left:0}.event:before{display:none}}"
                "</style></head><body><header><div class='top'><div class='brand'><div class='orb'></div><div><h1>DSCO Chronicle</h1><div class='sub'>local-first provenance · blob-backed payloads · cost, replay, forensic timeline</div></div></div><div class='pill'><span class='dot'></span><span id='live'>live</span></div></div>");
    jbuf_append(&b,
                "<div class='controls'><input id='q' placeholder='Search event type, tool, model, hash, payload…' autofocus>"
                "<select id='type'><option value=''>all event types</option></select>"
                "<input id='session' placeholder='session filter' value='");
    html_escape(&b, session_filter ? session_filter : "");
    jbuf_append(&b,
                "'><button id='refresh'>Refresh</button><button id='auto'>Auto: on</button></div>"
                 "<div class='sub'>store: ");
    html_escape(&b, chronicle_root());
    jbuf_appendf(&b,
                 " · <a href='/chronicle.json'>json</a> · <a href='/'>baseline</a></div></header>"
                 "<main><section><div class='stats' id='stats'></div><div class='timeline' id='timeline'><div class='empty'>Loading Chronicle…</div></div></section>"
                 "<aside class='rail'><h3>Signal</h3><div id='signal'></div><h3>Event Types</h3><div id='types'></div><h3>Blob Links</h3><div class='mini' id='blobs'>No blobs selected yet.</div></aside></main>"
                 "<script>"
                 "const LIMIT=%d;let raw=[],filtered=[],auto=true,lastCount=0;"
                 "const $=id=>document.getElementById(id);"
                 "const esc=s=>String(s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));"
                 "const cls=e=>e.event_type.includes('tool')?'tool':e.event_type.includes('llm')?'llm':JSON.stringify(e.payload).includes('error')?'err':'';"
                 "const short=s=>s?String(s).slice(0,12)+'…':'';"
                 "function walk(o,a=[]){if(!o||typeof o!=='object')return a;for(const[k,v]of Object.entries(o)){if(k.includes('sha256')&&typeof v==='string'&&v.length===64)a.push(v);else if(typeof v==='object')walk(v,a)}return a}"
                 "async function load(){const sess=$('session').value.trim();const url='/chronicle.json'+(sess?'?session='+encodeURIComponent(sess):'');$('live').textContent='loading';const r=await fetch(url,{cache:'no-store'});const j=await r.json();raw=j.events||[];$('live').textContent='live · '+raw.length;render()}"
                 "function render(){const q=$('q').value.toLowerCase();const t=$('type').value;filtered=raw.filter(e=>(!t||e.event_type===t)&&(!q||JSON.stringify(e).toLowerCase().includes(q)));renderStats();renderTypes();renderTimeline();}"
                 "function renderStats(){const llm=raw.filter(e=>e.event_type.startsWith('llm.')).length,tools=raw.filter(e=>e.event_type.startsWith('tool.')).length,ctx=raw.filter(e=>e.event_type==='context.materialized').length,blobs=raw.reduce((n,e)=>n+walk(e.payload).length,0);$('stats').innerHTML=[['Events',raw.length,''],['Shown',filtered.length,'good'],['LLM',llm,''],['Tools',tools,'warn'],['Blobs',blobs,'bad']].map(x=>`<div class='stat ${x[2]}'><div class='k'>${x[0]}</div><div class='v'>${x[1]}</div></div>`).join('');const max=Math.max(1,raw.length);$('signal').innerHTML=`<div class='legend'><span>tool I/O</span><b>${tools}</b></div><div class='bar'><i style='width:${Math.min(100,tools/max*100)}%%'></i></div><div class='legend'><span>model events</span><b>${llm}</b></div><div class='bar'><i style='width:${Math.min(100,llm/max*100)}%%'></i></div><div class='legend'><span>contexts</span><b>${ctx}</b></div><div class='bar'><i style='width:${Math.min(100,ctx/max*100)}%%'></i></div>`}"
                 "function renderTypes(){const counts={};raw.forEach(e=>counts[e.event_type]=(counts[e.event_type]||0)+1);const keys=Object.keys(counts).sort((a,b)=>counts[b]-counts[a]);const sel=$('type'),cur=sel.value;sel.innerHTML='<option value=\"\">all event types</option>'+keys.map(k=>`<option value=\"${esc(k)}\">${esc(k)} (${counts[k]})</option>`).join('');sel.value=cur;$('types').innerHTML=keys.slice(0,18).map(k=>`<button class='typebtn ${cur===k?'active':''}' onclick=\"document.getElementById('type').value='${esc(k)}';render()\"><span>${esc(k)}</span><b>${counts[k]}</b></button>`).join('')}"
                 "function renderTimeline(){const el=$('timeline');if(!filtered.length){el.innerHTML='<div class=empty>No matching Chronicle events.</div>';return}el.innerHTML=filtered.slice(0,LIMIT).map((e,i)=>{const p=JSON.stringify(e.payload,null,2);const blobs=walk(e.payload);const when=new Date((e.wall_time||0)*1000).toLocaleString();return `<article class='event ${cls(e)}' data-i='${i}'><div class='evhead'><div><div class='etype'>${esc(e.event_type)}</div><div class='meta'>#${e.seq} · ${esc(when)} · ${esc(short(e.session_id))} · ${esc(e.actor_type)}/${esc(e.actor_id)}</div></div><div class='actions'><button class='ghost' onclick='toggle(${i})'>payload</button>${blobs.length?`<button class='ghost' onclick='showBlobs(${i})'>${blobs.length} blobs</button>`:''}</div></div><div class='chips'><span class='chip'>${esc(e.sensitivity||'')}</span>${e.trace_id?`<span class='chip'>trace ${esc(short(e.trace_id))}</span>`:''}${e.span_id?`<span class='chip'>span ${esc(short(e.span_id))}</span>`:''}${blobs.slice(0,2).map(h=>`<span class='chip hash'>${esc(short(h))}</span>`).join('')}</div><pre class='payload'>${esc(p)}</pre></article>`}).join('')}"
                 "window.toggle=i=>document.querySelector(`[data-i=\"${i}\"]`).classList.toggle('open');"
                 "window.showBlobs=i=>{const e=filtered[i],bs=walk(e.payload);$('blobs').innerHTML=bs.map(h=>`<div style='margin:8px 0'><a class='hash' href='/chronicle/blob/${h}' target='_blank'>${h}</a></div>`).join('')||'No blobs.'};"
                 "$('q').oninput=render;$('type').onchange=render;$('session').onkeydown=e=>{if(e.key==='Enter')load()};$('refresh').onclick=load;$('auto').onclick=()=>{auto=!auto;$('auto').textContent='Auto: '+(auto?'on':'off')};setInterval(()=>{if(auto)load()},5000);load();"
                 "</script></body></html>",
                 limit);
    return b.data;
}

char *chronicle_build_activity_html(int limit, const char *session_filter) {
    return chronicle_build_activity_html_ex(limit, session_filter, NULL, NULL);
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
