/* Typed tool results. See include/work_result.h. */

#include "work_result.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *work_result_kind_name(work_result_kind_t k) {
    switch (k) {
        case WORK_RESULT_TEXT:  return "text";
        case WORK_RESULT_EDIT:  return "edit";
        case WORK_RESULT_PATCH: return "patch";
        case WORK_RESULT_SPAWN: return "spawn";
        case WORK_RESULT_ERROR: return "error";
        case WORK_RESULT_NONE:
        default: return "none";
    }
}

void work_result_init(work_result_t *r, work_result_kind_t kind) {
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->ok = (kind != WORK_RESULT_ERROR);
}

void work_result_free(work_result_t *r) {
    if (!r)
        return;
    free(r->text);
    for (size_t i = 0; i < r->file_count; i++) {
        free(r->files[i].path);
        free(r->files[i].old_path);
        free(r->files[i].diff);
    }
    free(r->files);
    free(r->spawn_provider);
    free(r->spawn_model);
    free(r->error_code);
    free(r->error_detail);
    memset(r, 0, sizeof(*r));
}

bool work_result_set_text(work_result_t *r, const char *text) {
    if (!r)
        return false;
    char *copy = NULL;
    if (text) {
        size_t n = strlen(text);
        copy = malloc(n + 1);
        if (!copy)
            return false;
        memcpy(copy, text, n + 1);
    }
    free(r->text);
    r->text = copy;
    return true;
}

bool work_result_add_file(work_result_t *r, const char *path, int added, int removed,
                          int hunks, char *diff) {
    if (!r || !path) {
        free(diff);
        return false;
    }
    work_file_change_t *grown =
        realloc(r->files, (r->file_count + 1) * sizeof(*r->files));
    if (!grown) {
        free(diff);
        return false;
    }
    r->files = grown;

    work_file_change_t *f = &r->files[r->file_count];
    memset(f, 0, sizeof(*f));
    size_t n = strlen(path);
    f->path = malloc(n + 1);
    if (!f->path) {
        free(diff);
        return false;
    }
    memcpy(f->path, path, n + 1);
    f->added = added;
    f->removed = removed;
    f->hunks = hunks;
    f->diff = diff; /* ownership transferred */
    r->file_count++;

    r->total_added += added;
    r->total_removed += removed;
    r->total_hunks += hunks;
    return true;
}

/* ── JSON ─────────────────────────────────────────────────────────────────
 * Hand-rolled so this module stays dependency-free and linkable from the
 * places that cannot pull in the full json helpers. */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} wr_buf_t;

static bool wr_reserve(wr_buf_t *b, size_t extra) {
    if (b->oom)
        return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap : 512;
    while (ncap < need)
        ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void wr_puts(wr_buf_t *b, const char *s) {
    if (!s)
        return;
    size_t n = strlen(s);
    if (!wr_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void wr_printf(wr_buf_t *b, const char *fmt, int v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), fmt, v);
    wr_puts(b, tmp);
}

/* Minimal RFC 8259 string escaping. */
static void wr_json_str(wr_buf_t *b, const char *s) {
    wr_puts(b, "\"");
    if (!s) {
        wr_puts(b, "\"");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  wr_puts(b, "\\\""); break;
            case '\\': wr_puts(b, "\\\\"); break;
            case '\n': wr_puts(b, "\\n"); break;
            case '\r': wr_puts(b, "\\r"); break;
            case '\t': wr_puts(b, "\\t"); break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    wr_puts(b, esc);
                } else {
                    if (!wr_reserve(b, 1))
                        return;
                    b->data[b->len++] = (char)*p;
                    b->data[b->len] = '\0';
                }
        }
    }
    wr_puts(b, "\"");
}

char *work_result_to_json(const work_result_t *r) {
    if (!r)
        return NULL;
    wr_buf_t b = {0};
    wr_puts(&b, "{\"kind\":");
    wr_json_str(&b, work_result_kind_name(r->kind));
    wr_puts(&b, ",\"ok\":");
    wr_puts(&b, r->ok ? "true" : "false");

    if (r->kind == WORK_RESULT_EDIT || r->kind == WORK_RESULT_PATCH) {
        wr_printf(&b, ",\"added\":%d", r->total_added);
        wr_printf(&b, ",\"removed\":%d", r->total_removed);
        wr_printf(&b, ",\"hunks\":%d", r->total_hunks);
        wr_printf(&b, ",\"replacements\":%d", r->replacements);
        if (r->match_strategy) {
            wr_puts(&b, ",\"match_strategy\":");
            wr_json_str(&b, r->match_strategy);
            char conf[32];
            snprintf(conf, sizeof(conf), ",\"match_confidence\":%.2f", r->match_confidence);
            wr_puts(&b, conf);
        }
        wr_puts(&b, ",\"files\":[");
        for (size_t i = 0; i < r->file_count; i++) {
            if (i)
                wr_puts(&b, ",");
            wr_puts(&b, "{\"path\":");
            wr_json_str(&b, r->files[i].path);
            if (r->files[i].old_path) {
                wr_puts(&b, ",\"old_path\":");
                wr_json_str(&b, r->files[i].old_path);
            }
            wr_printf(&b, ",\"added\":%d", r->files[i].added);
            wr_printf(&b, ",\"removed\":%d", r->files[i].removed);
            wr_printf(&b, ",\"hunks\":%d", r->files[i].hunks);
            if (r->files[i].binary)
                wr_puts(&b, ",\"binary\":true");
            if (r->files[i].truncated)
                wr_puts(&b, ",\"truncated\":true");
            if (r->files[i].diff) {
                wr_puts(&b, ",\"diff\":");
                wr_json_str(&b, r->files[i].diff);
            }
            wr_puts(&b, "}");
        }
        wr_puts(&b, "]");
    }

    if (r->kind == WORK_RESULT_SPAWN) {
        wr_printf(&b, ",\"spawn_id\":%d", r->spawn_id);
        if (r->spawn_provider) {
            wr_puts(&b, ",\"provider\":");
            wr_json_str(&b, r->spawn_provider);
        }
        if (r->spawn_model) {
            wr_puts(&b, ",\"model\":");
            wr_json_str(&b, r->spawn_model);
        }
    }

    if (r->kind == WORK_RESULT_ERROR) {
        wr_puts(&b, ",\"error_code\":");
        wr_json_str(&b, r->error_code ? r->error_code : "unknown");
        if (r->error_detail) {
            wr_puts(&b, ",\"error_detail\":");
            wr_json_str(&b, r->error_detail);
        }
    }

    if (r->elapsed_us) {
        char t[48];
        snprintf(t, sizeof(t), ",\"elapsed_us\":%llu", (unsigned long long)r->elapsed_us);
        wr_puts(&b, t);
    }
    if (r->cost_usd > 0.0) {
        char c[48];
        snprintf(c, sizeof(c), ",\"cost_usd\":%.6f", r->cost_usd);
        wr_puts(&b, c);
    }

    if (r->text) {
        wr_puts(&b, ",\"text\":");
        wr_json_str(&b, r->text);
    }
    wr_puts(&b, "}");

    if (b.oom) {
        free(b.data);
        return NULL;
    }
    return b.data ? b.data : NULL;
}

/* ── Migration side-channel ─────────────────────────────────────────────── */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define WR_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define WR_TLS __thread
#else
#define WR_TLS /* single-threaded fallback */
#endif

static WR_TLS work_result_t s_pending;
static WR_TLS bool s_pending_set;

void work_result_publish(work_result_t *r) {
    if (!r)
        return;
    /* A tool publishing twice in one call replaces the earlier result rather
     * than leaking it. */
    if (s_pending_set)
        work_result_free(&s_pending);
    s_pending = *r;
    s_pending_set = true;
    /* Ownership moved; leave the caller's struct empty but freeable. */
    memset(r, 0, sizeof(*r));
}

bool work_result_take(work_result_t *out) {
    if (!out || !s_pending_set)
        return false;
    *out = s_pending;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending_set = false;
    return true;
}

void work_result_clear_pending(void) {
    if (s_pending_set) {
        work_result_free(&s_pending);
        s_pending_set = false;
    }
}
