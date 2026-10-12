#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include "code_edit.h"
#include "crypto.h"
#include "edit_diff.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <copyfile.h>
#else
#include <sys/xattr.h>
#endif

#define FILE_LIMIT (4 * 1024 * 1024)
#define LINE_LIMIT 200000
#define EDIT_LIMIT 64

typedef struct {
    char *text;
    size_t len, *lines, count;
    struct stat stat;
    char hash[65];
    int fd;
    bool absent;
} snapshot_t;

static bool error(char *out, size_t size, const char *code, const char *detail) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"status\":\"error\",\"code\":");
    jbuf_append_json_str(&b, code);
    jbuf_append(&b, ",\"error\":");
    jbuf_append_json_str(&b, detail);
    jbuf_append(&b, "}");
    if (size)
        snprintf(out, size, "%s", b.len < size ? b.data : "{}");
    jbuf_free(&b);
    return false;
}

/* yyjson accepts duplicate keys; reject them and embedded NUL before any effect. */
static bool clean_json_at(yyjson_val *v, unsigned depth) {
    if (depth > 16)
        return false;
    if (yyjson_is_str(v))
        return strlen(yyjson_get_str(v)) == yyjson_get_len(v);
    if (yyjson_is_arr(v)) {
        size_t i, n;
        yyjson_val *item;
        if (yyjson_arr_size(v) > EDIT_LIMIT)
            return false;
        yyjson_arr_foreach(v, i, n, item) if (!clean_json_at(item, depth + 1)) return false;
    }
    if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 16)
            return false;
        size_t i, n;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, i, n, key, val) {
            if (!clean_json_at(key, depth + 1) || !clean_json_at(val, depth + 1))
                return false;
            size_t j, m;
            yyjson_val *other, *unused;
            yyjson_obj_foreach(v, j, m, other,
                               unused) if (j < i && !strcmp(yyjson_get_str(key),
                                                            yyjson_get_str(other))) return false;
        }
    }
    return true;
}
static bool clean_json(yyjson_val *v) {
    return clean_json_at(v, 0);
}

static bool fields(yyjson_val *obj, const char *const *allowed) {
    if (!yyjson_is_obj(obj))
        return false;
    size_t i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(obj, i, n, key, val) {
        bool found = false;
        for (size_t j = 0; allowed[j]; j++)
            if (!strcmp(yyjson_get_str(key), allowed[j]))
                found = true;
        if (!found)
            return false;
    }
    return true;
}

static bool utf8(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned c = (unsigned char)s[i++];
        if (!c)
            return false;
        if (c < 128)
            continue;
        unsigned need, min, value;
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            min = 0x80;
            value = c & 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            min = 0x800;
            value = c & 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            min = 0x10000;
            value = c & 7;
        } else
            return false;
        if (len - i < need)
            return false;
        while (need--) {
            unsigned d = (unsigned char)s[i++];
            if ((d & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (d & 63);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

static void release(snapshot_t *s) {
    if (s->fd >= 0)
        close(s->fd);
    free(s->text);
    free(s->lines);
}

static bool read_snapshot(int dir, const char *name, bool allow_absent, snapshot_t *s) {
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (s->fd < 0) {
        if (!(allow_absent && errno == ENOENT))
            return false;
        s->absent = true;
    } else if (fstat(s->fd, &s->stat) || !S_ISREG(s->stat.st_mode) || s->stat.st_nlink != 1 ||
               s->stat.st_size < 0 || s->stat.st_size > FILE_LIMIT) {
        errno = EINVAL;
        return false;
    }
    s->len = s->absent ? 0 : (size_t)s->stat.st_size;
    s->text = malloc(s->len + 1);
    if (!s->text)
        return false;
    size_t at = 0;
    while (at < s->len) {
        ssize_t n = pread(s->fd, s->text + at, s->len - at, (off_t)at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            errno = EIO;
            return false;
        }
        at += (size_t)n;
    }
    /* A growth during read is not a valid snapshot. */
    char extra;
    if (!s->absent && pread(s->fd, &extra, 1, (off_t)s->len) != 0) {
        errno = EAGAIN;
        return false;
    }
    s->text[s->len] = 0;
    if (!utf8(s->text, s->len)) {
        errno = EILSEQ;
        return false;
    }
    sha256_hex((const uint8_t *)s->text, s->len, s->hash);
    s->lines = malloc((LINE_LIMIT + 1) * sizeof(*s->lines));
    if (!s->lines)
        return false;
    for (size_t i = 0; i < s->len; i++) {
        if (!i || s->text[i - 1] == '\n') {
            if (s->count == LINE_LIMIT) {
                errno = EFBIG;
                return false;
            }
            s->lines[s->count++] = i;
        }
    }
    s->lines[s->count] = s->len;
    return true;
}

static int parent_dir(const char *path, char name[NAME_MAX + 1]) {
    if (!path || !*path || strlen(path) >= PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    const char *base = slash ? slash + 1 : copy;
    if (!*base || strlen(base) > NAME_MAX || !strcmp(base, ".") || !strcmp(base, "..")) {
        errno = EINVAL;
        return -1;
    }
    snprintf(name, NAME_MAX + 1, "%s", base);
    if (slash) {
        if (slash == copy)
            slash[1] = 0;
        else
            *slash = 0;
    }
    return open(slash ? copy : ".", O_RDONLY | O_DIRECTORY);
}

static void anchor(const snapshot_t *s, size_t line, char out[40]) {
    char hash[65];
    sha256_hex((const uint8_t *)s->text + s->lines[line], s->lines[line + 1] - s->lines[line],
               hash);
    snprintf(out, 40, "%zu:%.16s", line + 1, hash);
}

static bool anchor_index(const snapshot_t *s, const char *str, size_t *out) {
    if (!str || *str < '1' || *str > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long n = strtoul(str, &end, 10);
    if (errno || *end != ':' || !n || n > s->count)
        return false;
    char expected[40];
    anchor(s, n - 1, expected);
    if (strcmp(str, expected))
        return false;
    *out = n - 1;
    return true;
}

bool code_read_execute(const char *input, char *out, size_t size) {
    if (!input || strlen(input) > 32768)
        return error(out, size, "input_limit", "Read request exceeds 32768 bytes");
    const char *const keys[] = {"path", "start_line", "limit", "include_content", NULL};
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
    snapshot_t s = {.fd = -1};
    int dir = -1;
    jbuf_t b;
    jbuf_init(&b, 1024);
    const char *
        code = "invalid_input",
       *detail = "Expected path, start_line>=1, limit=1..200, and optional include_content boolean";
    bool ok = false;
    if (!clean_json(obj) || !fields(obj, keys))
        goto done;
    const char *path = yyjson_get_str(yyjson_obj_get(obj, "path"));
    yyjson_val *start_v = yyjson_obj_get(obj, "start_line"),
               *limit_v = yyjson_obj_get(obj, "limit");
    yyjson_val *content = yyjson_obj_get(obj, "include_content");
    if ((start_v && !yyjson_is_uint(start_v)) || (limit_v && !yyjson_is_uint(limit_v)) ||
        (content && !yyjson_is_bool(content)))
        goto done;
    uint64_t start = start_v ? yyjson_get_uint(start_v) : 1;
    uint64_t limit = limit_v ? yyjson_get_uint(limit_v) : 80;
    if (!start || !limit || limit > 200 || start > LINE_LIMIT + 1)
        goto done;
    char name[NAME_MAX + 1];
    dir = parent_dir(path, name);
    code = "unreadable_file";
    detail = "Read requires a single-link regular UTF-8 file <=4 MiB; symlinks and binary files "
             "are rejected";
    if (dir < 0 || !read_snapshot(dir, name, false, &s))
        goto done;
    if (start > s.count + 1) {
        code = "invalid_range";
        detail = "start_line exceeds the end of the file";
        goto done;
    }
    jbuf_append(&b, "{\"ok\":true,\"kind\":\"code_snapshot\",\"path\":");
    jbuf_append_json_str(&b, path);
    jbuf_appendf(&b, ",\"sha256\":\"%s\",\"bytes\":%zu,\"total_lines\":%zu,\"lines\":[", s.hash,
                 s.len, s.count);
    size_t begin = (size_t)start - 1, stop = begin + (size_t)limit;
    if (stop > s.count)
        stop = s.count;
    for (size_t i = begin; i < stop; i++) {
        char ref[40];
        anchor(&s, i, ref);
        if (i > begin)
            jbuf_append(&b, ",");
        jbuf_appendf(&b, "{\"line\":%zu,\"anchor\":\"%s\",\"text\":", i + 1, ref);
        size_t end = s.lines[i + 1];
        char saved = s.text[end];
        s.text[end] = 0;
        jbuf_append_json_str(&b, s.text + s.lines[i]);
        s.text[end] = saved;
        jbuf_append(&b, "}");
    }
    jbuf_appendf(&b, "],\"next_line\":%zu", stop < s.count ? stop + 1 : 0);
    if (yyjson_get_bool(content)) {
        if (s.len > 32768) {
            code = "content_limit";
            detail = "include_content is limited to 32768 bytes; use bounded line reads";
            goto done;
        }
        jbuf_append(&b, ",\"content\":");
        jbuf_append_json_str(&b, s.text);
    }
    jbuf_append(&b, "}");
    if (b.len >= size) {
        code = "result_limit";
        detail = "Snapshot does not fit the result buffer; reduce the line limit";
        goto done;
    }
    memcpy(out, b.data, b.len + 1);
    ok = true;
done:
    if (!ok)
        error(out, size, code, detail);
    release(&s);
    if (dir >= 0)
        close(dir);
    jbuf_free(&b);
    if (doc)
        yyjson_doc_free(doc);
    return ok;
}

typedef struct {
    size_t from, to;
    const char *text;
} edit_t;
static int compare_edits(const void *a, const void *b) {
    const edit_t *x = a, *y = b;
    return x->from < y->from ? -1 : x->from > y->from ? 1 : 0;
}

/* Directory lock serializes cooperating editors across rename. Other writers
 * must honor this protocol; arbitrary shell writes cannot be made CAS atomic. */
static bool commit(int dir, const char *name, const snapshot_t *before, const char *text,
                   size_t len, bool *written, bool *durable) {
    char temp[80];
    int fd = -1;
    for (unsigned i = 0; i < 32 && fd < 0; i++) {
        snprintf(temp, sizeof(temp), ".dsco-edit-%ld-%08x", (long)getpid(), arc4random());
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0 && errno != EEXIST)
            return false;
    }
    if (fd < 0)
        return false;
    bool ok = false;
    size_t at = 0;
    while (at < len) {
        ssize_t n = write(fd, text + at, len - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        at += (size_t)n;
    }
    if (!before->absent) {
#ifdef __APPLE__
        if (fcopyfile(before->fd, fd, NULL, COPYFILE_METADATA) != 0)
            goto done;
#else
        /* Reject metadata we cannot preserve instead of silently dropping it. */
        if (flistxattr(before->fd, NULL, 0) != 0)
            goto done;
        if (fchown(fd, before->stat.st_uid, before->stat.st_gid) ||
            fchmod(fd, before->stat.st_mode & 07777))
            goto done;
#endif
    }
    if (fsync(fd) != 0)
        goto done;
    snapshot_t current = {.fd = -1};
    bool same = read_snapshot(dir, name, before->absent, &current) &&
                current.absent == before->absent && !strcmp(current.hash, before->hash) &&
                (before->absent || (current.stat.st_dev == before->stat.st_dev &&
                                    current.stat.st_ino == before->stat.st_ino));
    release(&current);
    if (!same) {
        errno = EAGAIN;
        goto done;
    }
    if (before->absent) {
        if (linkat(dir, temp, dir, name, 0) != 0)
            goto done;
        unlinkat(dir, temp, 0);
    } else if (renameat(dir, temp, dir, name) != 0)
        goto done;
    *written = true;
    *durable = fsync(dir) == 0;
    snapshot_t after = {.fd = -1};
    char hash[65];
    sha256_hex((const uint8_t *)text, len, hash);
    ok = read_snapshot(dir, name, false, &after) && !strcmp(after.hash, hash);
    release(&after);
done:
    close(fd);
    unlinkat(dir, temp, 0);
    return ok;
}

bool code_edit_execute(const char *input, char *out, size_t size) {
    if (!input || strlen(input) > FILE_LIMIT * 6u + 32768u)
        return error(out, size, "input_limit", "Edit request exceeds its byte bound");
    const char *const keys[] = {"path", "expected_sha256", "edits", "content", "dry_run", NULL};
    const char *const edit_keys[] = {"kind", "start", "end", "new_text", NULL};
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
    snapshot_t s = {.fd = -1};
    int dir = -1;
    bool locked = false, ok = false;
    jbuf_t text, receipt;
    jbuf_init(&text, 1024);
    jbuf_init(&receipt, 1024);
    char *diff = NULL;
    const char *code = "invalid_input",
               *detail = "Require path, expected_sha256, and exactly one of edits or content";
    if (!clean_json(obj) || !fields(obj, keys))
        goto done;
    const char *path = yyjson_get_str(yyjson_obj_get(obj, "path"));
    const char *expected = yyjson_get_str(yyjson_obj_get(obj, "expected_sha256"));
    yyjson_val *ops = yyjson_obj_get(obj, "edits"), *content = yyjson_obj_get(obj, "content");
    yyjson_val *dry = yyjson_obj_get(obj, "dry_run");
    if (!expected || (!!ops == !!content) || (dry && !yyjson_is_bool(dry)) ||
        (content && !yyjson_is_str(content)))
        goto done;
    bool create = !strcmp(expected, "absent"), preview = yyjson_get_bool(dry);
    if (!create && strlen(expected) != 64)
        goto done;
    if (create && !content)
        goto done;
    char name[NAME_MAX + 1];
    dir = parent_dir(path, name);
    code = "unreadable_file";
    detail = "Requires an existing parent directory and a single-link regular UTF-8 file <=4 MiB";
    if (dir < 0)
        goto done;
    if (!preview) {
        if (flock(dir, LOCK_EX | LOCK_NB)) {
            code = "busy";
            detail = "Another cooperating editor holds the directory lock; retry after rereading";
            goto done;
        }
        locked = true;
    }
    if (!read_snapshot(dir, name, create, &s))
        goto done;
    code = "stale_snapshot";
    detail = "File version changed; reread code_read and rebuild all edits";
    if ((create && !s.absent) || (!create && (s.absent || strcmp(s.hash, expected))))
        goto done;
    code = "invalid_edits";
    detail = "Use 1..64 nonoverlapping edits against exact original line anchors; include exact "
             "newline bytes in new_text";
    size_t count = 0;
    if (content) {
        if (yyjson_get_len(content) > FILE_LIMIT)
            goto done;
        jbuf_append_len(&text, yyjson_get_str(content), yyjson_get_len(content));
        count = 1;
    } else {
        if (!yyjson_is_arr(ops) || !(count = yyjson_arr_size(ops)) || count > EDIT_LIMIT)
            goto done;
        edit_t edits[EDIT_LIMIT];
        size_t i, n;
        yyjson_val *op;
        yyjson_arr_foreach(ops, i, n, op) {
            if (!fields(op, edit_keys))
                goto done;
            const char *kind = yyjson_get_str(yyjson_obj_get(op, "kind"));
            const char *start = yyjson_get_str(yyjson_obj_get(op, "start"));
            yyjson_val *end_v = yyjson_obj_get(op, "end"), *new_v = yyjson_obj_get(op, "new_text");
            size_t a, z;
            if (!kind || !anchor_index(&s, start, &a) || (end_v && !yyjson_is_str(end_v)) ||
                (new_v && !yyjson_is_str(new_v)))
                goto done;
            if (!anchor_index(&s, end_v ? yyjson_get_str(end_v) : start, &z) || z < a)
                goto done;
            bool replace = !strcmp(kind, "replace"), del = !strcmp(kind, "delete");
            bool before = !strcmp(kind, "insert_before"), after = !strcmp(kind, "insert_after");
            if ((!replace && !del && !before && !after) || (del && new_v) || (!del && !new_v) ||
                ((before || after) && end_v))
                goto done;
            edits[i] = (edit_t){s.lines[a], s.lines[z + 1], del ? "" : yyjson_get_str(new_v)};
            if (before)
                edits[i].to = edits[i].from;
            if (after)
                edits[i].from = edits[i].to;
            if (strlen(edits[i].text) > FILE_LIMIT)
                goto done;
        }
        qsort(edits, count, sizeof(*edits), compare_edits);
        size_t cursor = 0;
        for (size_t i = 0; i < count; i++) {
            if (edits[i].from < cursor || (i && edits[i].from == edits[i - 1].from))
                goto done;
            if (text.len + edits[i].from - cursor + strlen(edits[i].text) > FILE_LIMIT)
                goto done;
            jbuf_append_len(&text, s.text + cursor, edits[i].from - cursor);
            jbuf_append(&text, edits[i].text);
            cursor = edits[i].to;
        }
        if (text.len + s.len - cursor > FILE_LIMIT)
            goto done;
        jbuf_append_len(&text, s.text + cursor, s.len - cursor);
    }
    if (!utf8(text.data, text.len))
        goto done;
    size_t final_lines = text.len ? 1 : 0;
    for (size_t i = 0; i + 1 < text.len; i++)
        if (text.data[i] == '\n')
            final_lines++;
    if (final_lines > LINE_LIMIT)
        goto done;
    char hash[65];
    sha256_hex((const uint8_t *)text.data, text.len, hash);
    bool changed = s.absent || strcmp(s.hash, hash);
    diff = edit_diff_unified(path, s.text, text.data, 3, 8000);
    if (!diff) {
        code = "out_of_memory";
        detail = "Unable to prepare diff; no bytes written";
        goto done;
    }
    /* Reserve the largest possible receipt before performing a mutation. */
    jbuf_append(&receipt, "{\"ok\":true,\"kind\":\"code_edit\",\"status\":");
    jbuf_append_json_str(&receipt, preview ? "preview" : changed ? "written" : "unchanged");
    jbuf_append(&receipt, ",\"path\":");
    jbuf_append_json_str(&receipt, path);
    jbuf_appendf(&receipt,
                 ",\"before_sha256\":\"%s\",\"after_sha256\":\"%s\",\"bytes\":%zu,\"edit_count\":%"
                 "zu,\"diff\":",
                 s.absent ? "absent" : s.hash, hash, text.len, count);
    jbuf_append_json_str(&receipt, diff);
    jbuf_append(&receipt, "}");
    if (receipt.len + 384 >= size) {
        code = "result_limit";
        detail = "Receipt cannot fit; no bytes written. Increase result buffer or narrow the edit";
        goto done;
    }
    bool written = false, durable = false;
    if (!preview && changed && !commit(dir, name, &s, text.data, text.len, &written, &durable)) {
        if (written) {
            receipt.data[--receipt.len] = 0;
            jbuf_append(&receipt, ",\"verified\":false,\"warning\":\"Write committed but readback "
                                  "failed; inspect the file before retrying\"}");
            /* No retryable failure after an already committed mutation. */
        } else {
            code = "commit_failed";
            detail = "Commit failed before replacement; reread before retrying";
            goto done;
        }
    } else {
        receipt.data[--receipt.len] = 0;
        jbuf_appendf(&receipt, ",\"verified\":%s,\"durable\":%s}", preview ? "false" : "true",
                     durable ? "true" : "false");
    }
    memcpy(out, receipt.data, receipt.len + 1);
    ok = true;
done:
    if (!ok)
        error(out, size, code, detail);
    release(&s);
    if (locked)
        flock(dir, LOCK_UN);
    if (dir >= 0)
        close(dir);
    jbuf_free(&text);
    jbuf_free(&receipt);
    free(diff);
    if (doc)
        yyjson_doc_free(doc);
    return ok;
}
