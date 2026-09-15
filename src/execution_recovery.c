#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "execution_recovery.h"
#include "capability.h"
#include "json_util.h"
#include "../vendor/yyjson.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* Fail closed at finite resource limits; never silently evict an old attempt. */
#define MAX_ATTEMPTS 16384u
#define MAX_FRAME (32u * 1024u * 1024u)
#define MAX_SCAN (512u * 1024u * 1024u)

typedef struct {
    char id[37];
    char tool[128];
    char input_hash[65];
    char status[24];
    unsigned mask;
    bool effectful, started, terminal;
    unsigned stage;
} recovery_attempt_t;

bool execution_recovery_valid_run_id(const char *id) {
    if (!id || strlen(id) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') ||
                     (id[i] >= 'a' && id[i] <= 'f') ||
                     (id[i] >= 'A' && id[i] <= 'F'))) return false;
    }
    return true;
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool copy_field(yyjson_val *obj, const char *key, char *out, size_t cap) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    const char *s = yyjson_get_str(v);
    if (!s || yyjson_get_len(v) == 0 || yyjson_get_len(v) >= cap ||
        strlen(s) != yyjson_get_len(v)) return false;
    memcpy(out, s, yyjson_get_len(v) + 1);
    return true;
}

static bool unique_fields(yyjson_val *obj) {
    if (!yyjson_is_obj(obj)) return false;
    /* The v1 envelopes are small. Bound future/hostile object work as well. */
    if (yyjson_obj_size(obj) > 64) return false;
    size_t i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(obj, i, n, key, val) {
        if (yyjson_obj_getn(obj, yyjson_get_str(key), yyjson_get_len(key)) != val)
            return false;
    }
    return true;
}

static recovery_attempt_t *lookup(recovery_attempt_t *rows, const char *id) {
    uint32_t h = 2166136261u;
    for (const unsigned char *s = (const unsigned char *)id; *s; s++) h = (h ^ *s) * 16777619u;
    for (unsigned n = 0; n < MAX_ATTEMPTS; n++) {
        recovery_attempt_t *a = &rows[(h + n) % MAX_ATTEMPTS];
        if (!a->id[0] || !strcmp(a->id, id)) return a;
    }
    return NULL;
}

static const char *apply_attempt(recovery_attempt_t *rows, yyjson_val *p,
                                 const char *run_id, size_t *count) {
    recovery_attempt_t a = {0};
    char session[37];
    if (!unique_fields(p) || !yyjson_equals_str(yyjson_obj_get(p, "schema"), "execution.attempt.v1") ||
        !copy_field(p, "execution_id", a.id, sizeof(a.id)) || !execution_recovery_valid_run_id(a.id) ||
        !copy_field(p, "session_id", session, sizeof(session)) || strcmp(session, run_id) ||
        !copy_field(p, "tool", a.tool, sizeof(a.tool)) ||
        !copy_field(p, "input_sha256", a.input_hash, sizeof(a.input_hash)) || strlen(a.input_hash) != 64 ||
        !copy_field(p, "status", a.status, sizeof(a.status))) return "invalid_attempt";
    for (size_t i = 0; i < 64; i++)
        if (!((a.input_hash[i] >= '0' && a.input_hash[i] <= '9') ||
              (a.input_hash[i] >= 'a' && a.input_hash[i] <= 'f'))) return "invalid_attempt";
    yyjson_val *mask = yyjson_obj_get(p, "capability_mask");
    yyjson_val *effectful = yyjson_obj_get(p, "effectful");
    yyjson_val *started = yyjson_obj_get(p, "started");
    yyjson_val *terminal = yyjson_obj_get(p, "terminal");
    yyjson_val *admitted = yyjson_obj_get(p, "admitted");
    if (!yyjson_is_uint(mask) || yyjson_get_uint(mask) > 127 ||
        !yyjson_is_bool(effectful) || !yyjson_is_bool(started) ||
        !yyjson_is_bool(terminal) || !yyjson_is_bool(admitted)) return "invalid_attempt";
    a.mask = (unsigned)yyjson_get_uint(mask);
    a.effectful = yyjson_get_bool(effectful);
    a.started = yyjson_get_bool(started);
    a.terminal = yyjson_get_bool(terminal);
    if (a.effectful != ((a.mask & (CAP_FS_WRITE | CAP_NET | CAP_EXEC | CAP_CONTROL)) != 0))
        return "invalid_effect_class";
    bool admitted_value = yyjson_get_bool(admitted);
    if (!strcmp(a.status, "proposed")) a.stage = 1;
    else if (!strcmp(a.status, "admitted")) a.stage = 2;
    else if (!strcmp(a.status, "started")) a.stage = 3;
    else if (!strcmp(a.status, "denied") || !strcmp(a.status, "succeeded") ||
             !strcmp(a.status, "failed") || !strcmp(a.status, "cancelled") ||
             !strcmp(a.status, "effect_unknown")) a.stage = 4;
    else return "unknown_attempt_status";
    if (a.terminal != (a.stage == 4) ||
        (a.stage == 1 && (a.started || admitted_value)) ||
        (a.stage == 2 && (a.started || !admitted_value)) ||
        (a.stage == 3 && (!a.started || !admitted_value)) ||
        (a.started && !admitted_value) ||
        (!strcmp(a.status, "denied") && (a.started || admitted_value)) ||
        (!strcmp(a.status, "failed") && !admitted_value) ||
        (!strcmp(a.status, "effect_unknown") && !a.started)) return "invalid_attempt_state";
    recovery_attempt_t *old = lookup(rows, a.id);
    if (!old) return "attempt_limit";
    if (old->id[0]) {
        if (strcmp(old->tool, a.tool) || strcmp(old->input_hash, a.input_hash) ||
            old->mask != a.mask || old->terminal || a.stage <= old->stage ||
            (old->started && !a.started) ||
            (a.stage == 2 && old->stage != 1) ||
            (a.stage == 3 && old->stage != 2) ||
            (a.stage == 4 && (a.started != old->started ||
                              admitted_value != (old->stage >= 2))) ||
            (!strcmp(a.status, "succeeded") && old->stage != 3))
            return "invalid_attempt_transition";
    } else {
        if (a.stage != 1) return "missing_attempt_prefix";
        ++*count;
    }
    *old = a;
    return NULL;
}

int execution_recovery_report(const char *path, const char *run_id, FILE *out) {
    if (!path || !out || !execution_recovery_valid_run_id(run_id)) return 2;
    const char *error = NULL;
    recovery_attempt_t *rows = calloc(MAX_ATTEMPTS, sizeof(*rows));
    size_t count = 0, records = 0, good_bytes = 0, unresolved = 0, unknown = 0;
    uint64_t previous_sequence = 0;
    bool run_completed = false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    FILE *fp = NULL;
    if (!rows) error = "allocation_failed";
    else if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) error = "journal_unavailable";
    else if (st.st_size < 0 || (uint64_t)st.st_size > MAX_SCAN) error = "journal_size_limit";
    else if (!(fp = fdopen(fd, "rb"))) error = "journal_unavailable";
    if (!fp && fd >= 0) close(fd);
    /* Snapshot the length: a live producer cannot make inspection unbounded.
     * This does not assert that a process has died or an effect is absent. */
    while (!error && good_bytes < (size_t)st.st_size) {
        unsigned char hdr[8];
        if ((size_t)st.st_size - good_bytes < sizeof(hdr) || fread(hdr, 1, 8, fp) != 8) {
            error = "truncated_header"; break;
        }
        uint32_t len = le32(hdr), crc = le32(hdr + 4);
        if (!len || len > MAX_FRAME) { error = "frame_size_limit"; break; }
        if ((size_t)st.st_size - good_bytes - 8 < len) { error = "truncated_frame"; break; }
        char *frame = malloc((size_t)len + 1);
        if (!frame) { error = "allocation_failed"; break; }
        if (fread(frame, 1, len, fp) != len) error = "read_failed";
        else if ((uint32_t)crc32(0, (const unsigned char *)frame, len) != crc) error = "crc_mismatch";
        frame[len] = 0;
        yyjson_doc *doc = error ? NULL : yyjson_read(frame, len, 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (!error && !unique_fields(root)) error = "invalid_record";
        if (!error && (!yyjson_equals_str(yyjson_obj_get(root, "run_id"), run_id) ||
                      !yyjson_is_uint(yyjson_obj_get(root, "v")) ||
                      yyjson_get_uint(yyjson_obj_get(root, "v")) != 1)) error = "record_identity_mismatch";
        yyjson_val *sequence = yyjson_obj_get(root, "seq");
        if (!error && (!yyjson_is_uint(sequence) || yyjson_get_uint(sequence) <= previous_sequence))
            error = "invalid_record_sequence";
        if (!error) {
            yyjson_val *type = yyjson_obj_get(root, "type");
            if (!yyjson_is_str(type)) error = "invalid_record";
            else if (yyjson_equals_str(type, "execution.attempt.v1"))
                error = apply_attempt(rows, yyjson_obj_get(root, "payload"), run_id, &count);
            else if (yyjson_equals_str(type, "run.completed")) run_completed = true;
            previous_sequence = yyjson_get_uint(sequence);
        }
        yyjson_doc_free(doc);
        free(frame);
        if (!error) { good_bytes += 8u + len; records++; }
    }
    if (fp) fclose(fp);
    if (!error && !records) error = "empty_journal";
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"schema\":\"dsco.execution_recovery.v1\",\"run_id\":");
    jbuf_append_json_str(&b, run_id);
    jbuf_appendf(&b, ",\"read_only\":true,\"retry_authorized\":false,\"snapshot_only\":true,"
                 "\"journal_complete\":%s,\"run_completed_recorded\":%s,\"records\":%zu,"
                 "\"valid_prefix_bytes\":%zu,\"attempts_seen\":%zu,\"error\":",
                 error ? "false" : "true", run_completed ? "true" : "false", records, good_bytes, count);
    if (error) jbuf_append_json_str(&b, error); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"unresolved\":[");
    for (size_t i = 0; rows && i < MAX_ATTEMPTS; i++) {
        recovery_attempt_t *a = &rows[i];
        if (!a->id[0]) continue;
        bool effect_unknown = !strcmp(a->status, "effect_unknown") ||
            (a->started && a->effectful && strcmp(a->status, "succeeded"));
        if (a->terminal && !effect_unknown) continue;
        if (unresolved++) jbuf_append_char(&b, ',');
        if (effect_unknown) unknown++;
        jbuf_append(&b, "{\"execution_id\":"); jbuf_append_json_str(&b, a->id);
        jbuf_append(&b, ",\"tool\":"); jbuf_append_json_str(&b, a->tool);
        jbuf_append(&b, ",\"last_recorded_status\":"); jbuf_append_json_str(&b, a->status);
        jbuf_append(&b, ",\"recovery_state\":");
        jbuf_append_json_str(&b, effect_unknown ? "effect_unknown" : a->started ? "interrupted_read" : "not_started");
        jbuf_appendf(&b, ",\"requires_reconciliation\":%s,\"retry_authorized\":false}", effect_unknown ? "true" : "false");
    }
    jbuf_appendf(&b, "],\"unresolved_count\":%zu,\"effect_unknown_count\":%zu}\n", unresolved, unknown);
    bool written = fwrite(b.data, 1, b.len, out) == b.len;
    jbuf_free(&b);
    free(rows);
    return error || !written ? 1 : unresolved ? 3 : 0;
}
