/* ═══════════════════════════════════════════════════════════════════════════
 * Capsules — lossless-by-retrieval compaction artifacts (T1).
 * Spec: include/capsule.h + SOTA_FRAMES_2026-09-02.md PLAN #04.
 *
 * New module per AGENTS.md rule 3 (no inline growth of megafiles); wired
 * into the build via one Makefile SRC_NAMES entry. Depends only on:
 *   - context fabric (ctx_put / ctx_get) for span offload/retrieval
 *   - crypto sha256_hex (cwd key)
 *   - json_util jbuf + json_view (serialize/parse)
 * Failure posture: every entry point degrades to "advisory no-op" on
 * internal errors — a capsule must never block or crash a session.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "capsule.h"
#include "context_fabric.h"
#include "crypto.h"
#include "json_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CAPSULE_KEY_STR_MAX 256

/* ── lifecycle ─────────────────────────────────────────────────────────── */

void capsule_init(capsule_t *c, const char *cwd) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    if (cwd && cwd[0]) {
        snprintf(c->cwd, sizeof(c->cwd), "%s", cwd);
    } else {
        char buf[1024];
        if (getcwd(buf, sizeof(buf)))
            snprintf(c->cwd, sizeof(c->cwd), "%s", buf);
        else
            snprintf(c->cwd, sizeof(c->cwd), "%s", "unknown-cwd");
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(c->created, sizeof(c->created), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void capsule_free(capsule_t *c) {
    if (!c) return;
    for (int i = 0; i < CAPSULE_MAX_KEYS; i++) {
        free(c->keys[i]);
        c->keys[i] = NULL;
    }
    c->key_count = 0;
}

bool capsule_add_key(capsule_t *c, const char *ctxkey) {
    if (!c || !ctxkey || !ctxkey[0]) return false;
    if (c->key_count >= CAPSULE_MAX_KEYS) return false;
    if (strlen(ctxkey) >= CAPSULE_KEY_STR_MAX) return false;
    /* dedupe identical keys — same bytes collapse in the fabric anyway */
    for (int i = 0; i < c->key_count; i++)
        if (strcmp(c->keys[i], ctxkey) == 0) return true;
    c->keys[c->key_count] = safe_strdup(ctxkey);
    if (!c->keys[c->key_count]) return false;
    c->key_count++;
    return true;
}

/* ── JSON serialize / parse ─────────────────────────────────────────────── */

char *capsule_to_json(const capsule_t *c) {
    if (!c) return NULL;
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{");
    jbuf_append(&b, "\"cwd\":");
    jbuf_append_json_str(&b, c->cwd);
    jbuf_append(&b, ",\"created\":");
    jbuf_append_json_str(&b, c->created);
    jbuf_append(&b, ",\"keys\":[");
    for (int i = 0; i < c->key_count; i++) {
        if (i) jbuf_append(&b, ",");
        jbuf_append_json_str(&b, c->keys[i] ? c->keys[i] : "");
    }
    jbuf_append(&b, "],\"summary\":");
    jbuf_append_json_str(&b, c->summary);
    jbuf_append(&b, "}");
    char *out = safe_strdup(b.data ? b.data : "{}");
    jbuf_free(&b);
    return out;
}

bool capsule_from_json(const char *json, capsule_t *c) {
    if (!json || !json[0] || !c) return false;
    capsule_init(c, NULL);
    capsule_free(c);

    json_view_t *v = json_view_open(json);
    if (!v) return false;

    const char *cwd = json_view_str(v, "cwd");
    if (cwd && cwd[0])
        snprintf(c->cwd, sizeof(c->cwd), "%s", cwd);

    const char *summary = json_view_str(v, "summary");
    if (summary)
        snprintf(c->summary, sizeof(c->summary), "%s", summary);

    /* keys array: parse via json_view (borrowed) */
    bool ok = true;
    /* json_view gives flat key access; for the array use the raw getter. */
    char *keys_raw = json_get_raw(json, "keys");
    if (keys_raw) {
        /* Minimal, safe array scan: extract top-level quoted strings. */
        const char *p = keys_raw;
        while (*p && c->key_count < CAPSULE_MAX_KEYS) {
            const char *q1 = strchr(p, '"');
            if (!q1) break;
            const char *q2 = q1 + 1;
            while (*q2 && *q2 != '"') {
                if (*q2 == '\\' && q2[1]) q2++;  /* skip escaped char */
                q2++;
            }
            if (!*q2) { ok = false; break; }
            size_t n = (size_t)(q2 - q1 - 1);
            if (n > 0 && n < CAPSULE_KEY_STR_MAX) {
                char key[CAPSULE_KEY_STR_MAX];
                memcpy(key, q1 + 1, n);
                key[n] = '\0';
                /* unescape minimal \" and \\ */
                for (char *r = key; *r; r++) {
                    if (*r == '\\' && (r[1] == '"' || r[1] == '\\'))
                        memmove(r, r + 1, strlen(r));
                }
                if (!capsule_add_key(c, key)) { ok = false; break; }
            }
            p = q2 + 1;
        }
        free(keys_raw);
    }
    json_view_close(v);

    /* A capsule with no keys but a summary is still valid (lossy-only path). */
    if (!c->cwd[0]) ok = false;
    return ok;
}

/* ── cwd keying / persistence ───────────────────────────────────────────── */

int capsule_key_for_cwd(const char *cwd, char *out, size_t cap) {
    if (!cwd || !cwd[0] || !out || cap < 17) return -1;
    char hex[65];
    sha256_hex((const uint8_t *)cwd, strlen(cwd), hex);
    memcpy(out, hex, 16);
    out[16] = '\0';
    return 0;
}

int capsule_path_for_cwd(const char *cwd, char *out, size_t cap) {
    char key[32];
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    if (capsule_key_for_cwd(cwd, key, sizeof(key)) != 0) return -1;
    int n = snprintf(out, cap, "%s/.dsco/capsules/%s.json", home, key);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static bool capsule_ensure_dir(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.dsco", home);
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
    snprintf(path, sizeof(path), "%s/.dsco/capsules", home);
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

/* Atomic write (mkstemp + fsync + rename), same discipline as chronicle.c. */
static bool capsule_write_atomic(const char *path, const char *body) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return false;
    size_t len = strlen(body);
    const char *p = body;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(tmp); return false; }
        p += n; len -= (size_t)n;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return false; }
    if (close(fd) != 0) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

int capsule_save(const capsule_t *c) {
    if (!c || !c->cwd[0]) return -1;
    if (!capsule_ensure_dir()) return -1;
    char path[1200];
    if (capsule_path_for_cwd(c->cwd, path, sizeof(path)) != 0) return -1;
    char *json = capsule_to_json(c);
    if (!json) return -1;
    bool ok = capsule_write_atomic(path, json);
    free(json);
    return ok ? 0 : -1;
}

int capsule_load(const char *cwd, capsule_t *c) {
    if (!cwd || !cwd[0] || !c) return -1;
    char path[1200];
    if (capsule_path_for_cwd(cwd, path, sizeof(path)) != 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    return capsule_from_json(buf, c) ? 0 : -1;
}

/* ── compaction hook ────────────────────────────────────────────────────── */

int capsule_offload_span(capsule_t *c, const char *text, size_t len,
                         const char *provenance) {
    if (!c || !text || len == 0) return -1;
    ctx_broker_t *b = ctx_broker_default();
    if (!b) return -1;  /* caller degrades to lossy-only compaction */

    ctx_put_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.kind = CTX_KIND_RUN;
    opts.source = provenance ? provenance : "capsule";
    opts.tags = "capsule,compaction";
    opts.importance = 0.7;  /* dropped content was heavy enough to overflow */
    opts.embed = 1;         /* textual — index for semantic fault-in */

    ctxkey_t key;
    bool deduped = false;
    if (!ctx_put(b, text, len, &opts, &key, &deduped)) return -1;

    char keystr[CAPSULE_KEY_STR_MAX];
    if (ctxkey_format(&key, keystr, sizeof(keystr)) < 0) return -1;
    return capsule_add_key(c, keystr) ? 0 : -1;
}

/* ── session-start injection ────────────────────────────────────────────── */

char *capsule_inject_block(const char *cwd, size_t max_bytes) {
    if (!cwd || !cwd[0]) return NULL;
    capsule_t c;
    if (capsule_load(cwd, &c) != 0) return NULL;  /* advisory: silent no-op */

    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "[Session capsule — prior context from this directory]\n");
    if (c.summary[0]) {
        jbuf_append(&b, c.summary);
        jbuf_append(&b, "\n");
    }
    if (c.key_count > 0) {
        jbuf_append(&b, "Offloaded context (fault in on demand via ctx_get):\n");
        ctx_broker_t *fab = ctx_broker_default();
        for (int i = 0; i < c.key_count; i++) {
            char preview[96] = "";
            if (fab) {
                ctxkey_t k;
                if (ctxkey_parse(c.keys[i], &k)) {
                    size_t len = 0;
                    char *txt = ctx_get(fab, &k, &len);
                    if (txt) {
                        size_t take = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
                        memcpy(preview, txt, take);
                        preview[take] = '\0';
                        free(txt);
                    }
                }
            }
            jbuf_appendf(&b, "  %s%s%s\n", c.keys[i],
                         preview[0] ? "  — " : "", preview);
            if (b.len > max_bytes) { jbuf_append(&b, "  ...\n"); break; }
        }
    } else {
        jbuf_append(&b, "(no offloaded keys — summary only)\n");
    }
    capsule_free(&c);

    char *out = safe_strdup(b.data ? b.data : "");
    jbuf_free(&b);
    return out;
}
