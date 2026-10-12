/* Unified-diff rendering for edit confirmations. See include/edit_diff.h.
 *
 * Line-based Myers O(ND) diff with a bounded edit-distance budget. Past the
 * budget we emit a single whole-range replacement hunk rather than burning
 * unbounded time on a pathological input. */

#include "edit_diff.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap on rendered output when the caller passes max_bytes = 0. */
#define EDIT_DIFF_DEFAULT_MAX 64000

/* Myers search budget: max edit distance (D) we will explore. Beyond this the
 * two texts are too dissimilar for a useful line diff. */
#define EDIT_DIFF_MAX_D 4000

/* Above this line count we skip Myers entirely (the V array is O(D) and the
 * trace is O(D^2) worst case). */
#define EDIT_DIFF_MAX_LINES 60000

typedef struct {
    char **line;   /* borrowed pointers into the owned buffer */
    int *len;      /* length of each line, excluding newline */
    int count;
    char *buf;     /* owned copy; lines point into it */
} ed_lines_t;

static void ed_lines_free(ed_lines_t *ls) {
    if (!ls)
        return;
    free(ls->line);
    free(ls->len);
    free(ls->buf);
    ls->line = NULL;
    ls->len = NULL;
    ls->buf = NULL;
    ls->count = 0;
}

/* Split text into lines. A trailing newline does NOT produce a final empty
 * line (so "a\n" is one line), matching diff(1) semantics. */
static bool ed_lines_split(const char *text, ed_lines_t *out) {
    memset(out, 0, sizeof(*out));
    if (!text)
        text = "";
    size_t n = strlen(text);
    out->buf = malloc(n + 1);
    if (!out->buf)
        return false;
    memcpy(out->buf, text, n + 1);

    int cap = 64;
    out->line = malloc((size_t)cap * sizeof(char *));
    out->len = malloc((size_t)cap * sizeof(int));
    if (!out->line || !out->len) {
        ed_lines_free(out);
        return false;
    }

    char *p = out->buf;
    char *end = out->buf + n;
    while (p <= end) {
        if (p == end) {
            /* Only a zero-length file has no lines; "a\n" already emitted "a". */
            break;
        }
        char *nl = memchr(p, '\n', (size_t)(end - p));
        int llen = nl ? (int)(nl - p) : (int)(end - p);
        if (out->count == cap) {
            int ncap = cap * 2;
            char **nline = realloc(out->line, (size_t)ncap * sizeof(char *));
            int *nlen = realloc(out->len, (size_t)ncap * sizeof(int));
            if (!nline || !nlen) {
                free(nline ? nline : out->line);
                free(nlen ? nlen : out->len);
                out->line = NULL;
                out->len = NULL;
                ed_lines_free(out);
                return false;
            }
            out->line = nline;
            out->len = nlen;
            cap = ncap;
        }
        /* Strip a trailing CR so CRLF files diff by logical line. */
        int keep = llen;
        if (keep > 0 && p[keep - 1] == '\r')
            keep--;
        out->line[out->count] = p;
        out->len[out->count] = keep;
        out->count++;
        if (!nl)
            break;
        p = nl + 1;
    }
    return true;
}

static bool ed_line_eq(const ed_lines_t *a, int ai, const ed_lines_t *b, int bi) {
    if (a->len[ai] != b->len[bi])
        return false;
    return memcmp(a->line[ai], b->line[bi], (size_t)a->len[ai]) == 0;
}

/* ── Myers O(ND) diff ─────────────────────────────────────────────────────
 * Produces a per-line opcode array over the two line vectors:
 *   ' ' = common, '-' = delete from old, '+' = insert from new.
 * The script is emitted in order; its length is N + M - 2*common. */

typedef struct {
    char *op;   /* 'c' common, 'd' delete(old), 'i' insert(new) */
    int *ai;    /* index into old lines (valid for 'c' and 'd') */
    int *bi;    /* index into new lines (valid for 'c' and 'i') */
    int count;
    int cap;
} ed_script_t;

static void ed_script_free(ed_script_t *s) {
    if (!s)
        return;
    free(s->op);
    free(s->ai);
    free(s->bi);
    memset(s, 0, sizeof(*s));
}

static bool ed_script_push(ed_script_t *s, char op, int ai, int bi) {
    if (s->count == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 128;
        char *nop = realloc(s->op, (size_t)ncap);
        int *na = realloc(s->ai, (size_t)ncap * sizeof(int));
        int *nb = realloc(s->bi, (size_t)ncap * sizeof(int));
        if (!nop || !na || !nb) {
            if (nop) s->op = nop;
            if (na) s->ai = na;
            if (nb) s->bi = nb;
            return false;
        }
        s->op = nop;
        s->ai = na;
        s->bi = nb;
        s->cap = ncap;
    }
    s->op[s->count] = op;
    s->ai[s->count] = ai;
    s->bi[s->count] = bi;
    s->count++;
    return true;
}

/* Recursive Myers is elegant but stack-hungry; use the iterative forward
 * search with a stored trace, then walk the trace backwards. Returns false
 * when the budget is exceeded or allocation fails. */
static bool ed_myers(const ed_lines_t *A, const ed_lines_t *B, ed_script_t *out) {
    int N = A->count, M = B->count;
    int max_d = N + M;
    if (max_d > EDIT_DIFF_MAX_D)
        max_d = EDIT_DIFF_MAX_D;

    int voff = max_d + 1;
    int vsize = 2 * max_d + 3;
    int *V = malloc((size_t)vsize * sizeof(int));
    if (!V)
        return false;
    /* trace[d] is a snapshot of V after pass d. */
    int **trace = calloc((size_t)max_d + 1, sizeof(int *));
    if (!trace) {
        free(V);
        return false;
    }

    for (int i = 0; i < vsize; i++)
        V[i] = 0;

    int found_d = -1;
    for (int d = 0; d <= max_d; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && V[voff + k - 1] < V[voff + k + 1]))
                x = V[voff + k + 1];
            else
                x = V[voff + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && ed_line_eq(A, x, B, y)) {
                x++;
                y++;
            }
            V[voff + k] = x;
            if (x >= N && y >= M) {
                int *snap_hit = malloc((size_t)vsize * sizeof(int));
                if (!snap_hit)
                    goto fail;
                memcpy(snap_hit, V, (size_t)vsize * sizeof(int));
                trace[d] = snap_hit;
                found_d = d;
                goto done;
            }
        }
        int *snap = malloc((size_t)vsize * sizeof(int));
        if (!snap)
            goto fail;
        memcpy(snap, V, (size_t)vsize * sizeof(int));
        trace[d] = snap;
    }

done:
    if (found_d < 0)
        goto fail;

    /* Backtrack through the trace, collecting the script in reverse. */
    {
        int x = N, y = M;
        ed_script_t rev = {0};
        for (int d = found_d; d > 0; d--) {
            /* trace[d-1] is V *after* pass d-1, which is what pass d read
             * when it chose this k. Mirror that same choice to find where we
             * came from: k+1 means we moved down (insert), k-1 means we moved
             * right (delete). */
            int *Vp = trace[d - 1];
            int k = x - y;
            int prev_k;
            if (k == -d || (k != d && Vp[voff + k - 1] < Vp[voff + k + 1]))
                prev_k = k + 1;
            else
                prev_k = k - 1;
            int prev_x = Vp[voff + prev_k];
            int prev_y = prev_x - prev_k;
            while (x > prev_x && y > prev_y) {
                x--;
                y--;
                if (!ed_script_push(&rev, 'c', x, y))
                    goto fail_rev;
            }
            if (x > prev_x) {
                x--;
                if (!ed_script_push(&rev, 'd', x, -1))
                    goto fail_rev;
            } else if (y > prev_y) {
                y--;
                if (!ed_script_push(&rev, 'i', -1, y))
                    goto fail_rev;
            }
        }
        while (x > 0 && y > 0) {
            x--;
            y--;
            if (!ed_script_push(&rev, 'c', x, y))
                goto fail_rev;
        }
        while (x > 0) {
            x--;
            if (!ed_script_push(&rev, 'd', x, -1))
                goto fail_rev;
        }
        while (y > 0) {
            y--;
            if (!ed_script_push(&rev, 'i', -1, y))
                goto fail_rev;
        }

        for (int i = rev.count - 1; i >= 0; i--) {
            if (!ed_script_push(out, rev.op[i], rev.ai[i], rev.bi[i])) {
                ed_script_free(&rev);
                goto fail;
            }
        }
        ed_script_free(&rev);
        goto ok;

fail_rev:
        ed_script_free(&rev);
        goto fail;
    }

ok:
    for (int d = 0; d <= max_d; d++)
        free(trace[d]);
    free(trace);
    free(V);
    return true;

fail:
    for (int d = 0; d <= max_d; d++)
        free(trace[d]);
    free(trace);
    free(V);
    ed_script_free(out);
    return false;
}

/* ── Bounded output buffer ────────────────────────────────────────────────
 * Grows until max_bytes, then stops accepting content and records that the
 * output was truncated. Never silently drops changes: the caller appends an
 * explicit marker. */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max;
    bool truncated;
    bool oom;
} ed_buf_t;

static void ed_buf_init(ed_buf_t *b, size_t max_bytes) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->max = max_bytes ? max_bytes : EDIT_DIFF_DEFAULT_MAX;
    b->truncated = false;
    b->oom = false;
}

static bool ed_buf_reserve(ed_buf_t *b, size_t extra) {
    if (b->oom)
        return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap : 1024;
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

static void ed_buf_append(ed_buf_t *b, const char *s, size_t n) {
    if (b->oom || b->truncated)
        return;
    if (b->len + n > b->max) {
        /* Fill up to the cap, then mark truncated. */
        size_t room = b->max > b->len ? b->max - b->len : 0;
        if (room && ed_buf_reserve(b, room)) {
            memcpy(b->data + b->len, s, room);
            b->len += room;
            b->data[b->len] = '\0';
        }
        b->truncated = true;
        return;
    }
    if (!ed_buf_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void ed_buf_puts(ed_buf_t *b, const char *s) {
    ed_buf_append(b, s, strlen(s));
}

static void ed_buf_printf(ed_buf_t *b, const char *fmt, ...) {
    if (b->oom || b->truncated)
        return;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        ed_buf_append(b, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        b->oom = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ed_buf_append(b, big, (size_t)n);
    free(big);
}

/* Emit one diff line with its prefix, preserving the original bytes. */
static void ed_emit_line(ed_buf_t *b, char prefix, const char *text, int len) {
    char p[1] = {prefix};
    ed_buf_append(b, p, 1);
    ed_buf_append(b, text, (size_t)len);
    ed_buf_puts(b, "\n");
}

/* ── Hunk grouping ────────────────────────────────────────────────────────
 * Walk the edit script and emit @@ hunks: each run of changes plus up to
 * `context` common lines on either side. Adjacent runs whose context windows
 * touch are merged into one hunk, as diff(1) does. */

static void ed_render_hunks(ed_buf_t *out, const ed_lines_t *A, const ed_lines_t *B,
                            const ed_script_t *s, int context) {
    int i = 0;
    while (i < s->count) {
        if (s->op[i] == 'c') {
            i++;
            continue;
        }
        /* Start of a change run. Back up over `context` common lines. */
        int start = i;
        int back = 0;
        while (start > 0 && s->op[start - 1] == 'c' && back < context) {
            start--;
            back++;
        }

        /* Extend forward while changes remain within 2*context of each other. */
        int end = i;
        for (;;) {
            while (end < s->count && s->op[end] != 'c')
                end++;
            int run = end;
            int fwd = 0;
            while (run < s->count && s->op[run] == 'c' && fwd < context * 2) {
                run++;
                fwd++;
            }
            if (run < s->count && s->op[run] != 'c') {
                end = run;
                continue;
            }
            int tail = end;
            int kept = 0;
            while (tail < s->count && s->op[tail] == 'c' && kept < context) {
                tail++;
                kept++;
            }
            end = tail;
            break;
        }

        /* Compute 1-based start lines and counts for the header. */
        int a_start = -1, b_start = -1, a_count = 0, b_count = 0;
        for (int k = start; k < end; k++) {
            if (s->op[k] == 'c' || s->op[k] == 'd') {
                if (a_start < 0)
                    a_start = s->ai[k];
                a_count++;
            }
            if (s->op[k] == 'c' || s->op[k] == 'i') {
                if (b_start < 0)
                    b_start = s->bi[k];
                b_count++;
            }
        }
        if (a_start < 0)
            a_start = (a_count == 0 && A->count) ? A->count : 0;
        if (b_start < 0)
            b_start = (b_count == 0 && B->count) ? B->count : 0;

        ed_buf_printf(out, "@@ -%d,%d +%d,%d @@\n", a_count ? a_start + 1 : a_start,
                      a_count, b_count ? b_start + 1 : b_start, b_count);

        for (int k = start; k < end; k++) {
            if (s->op[k] == 'c')
                ed_emit_line(out, ' ', A->line[s->ai[k]], A->len[s->ai[k]]);
            else if (s->op[k] == 'd')
                ed_emit_line(out, '-', A->line[s->ai[k]], A->len[s->ai[k]]);
            else
                ed_emit_line(out, '+', B->line[s->bi[k]], B->len[s->bi[k]]);
        }
        i = end;
    }
}

/* Fallback when Myers exceeds its budget: one hunk replacing everything that
 * differs between the common prefix and common suffix. Always correct, just
 * coarser than a minimal diff. */
static void ed_render_coarse(ed_buf_t *out, const ed_lines_t *A, const ed_lines_t *B) {
    int pre = 0;
    while (pre < A->count && pre < B->count && ed_line_eq(A, pre, B, pre))
        pre++;
    int suf = 0;
    while (suf < A->count - pre && suf < B->count - pre &&
           ed_line_eq(A, A->count - 1 - suf, B, B->count - 1 - suf))
        suf++;

    int a_lo = pre, a_hi = A->count - suf;
    int b_lo = pre, b_hi = B->count - suf;
    int a_count = a_hi - a_lo, b_count = b_hi - b_lo;

    ed_buf_printf(out, "@@ -%d,%d +%d,%d @@\n", a_count ? a_lo + 1 : a_lo, a_count,
                  b_count ? b_lo + 1 : b_lo, b_count);
    for (int k = a_lo; k < a_hi; k++)
        ed_emit_line(out, '-', A->line[k], A->len[k]);
    for (int k = b_lo; k < b_hi; k++)
        ed_emit_line(out, '+', B->line[k], B->len[k]);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

char *edit_diff_unified(const char *path, const char *old_text, const char *new_text,
                        int context_lines, size_t max_bytes) {
    if (!old_text)
        old_text = "";
    if (!new_text)
        new_text = "";
    if (context_lines < 0)
        context_lines = EDIT_DIFF_CONTEXT;

    if (strcmp(old_text, new_text) == 0) {
        char *empty = malloc(1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }

    ed_lines_t A, B;
    if (!ed_lines_split(old_text, &A))
        return NULL;
    if (!ed_lines_split(new_text, &B)) {
        ed_lines_free(&A);
        return NULL;
    }

    ed_buf_t out;
    ed_buf_init(&out, max_bytes);

    /* git-style a/ b/ prefixes read wrong on absolute paths ("a//tmp/x"),
     * so only apply them to relative paths. */
    const char *shown = (path && path[0]) ? path : "file";
    if (shown[0] == '/')
        ed_buf_printf(&out, "--- %s\n+++ %s\n", shown, shown);
    else
        ed_buf_printf(&out, "--- a/%s\n+++ b/%s\n", shown, shown);

    bool rendered = false;
    if (A.count <= EDIT_DIFF_MAX_LINES && B.count <= EDIT_DIFF_MAX_LINES) {
        ed_script_t script = {0};
        if (ed_myers(&A, &B, &script)) {
            ed_render_hunks(&out, &A, &B, &script, context_lines);
            rendered = true;
        }
        ed_script_free(&script);
    }
    if (!rendered)
        ed_render_coarse(&out, &A, &B);

    if (out.truncated)
        ed_buf_puts(&out, "\n[diff truncated: output limit reached]\n");

    ed_lines_free(&A);
    ed_lines_free(&B);

    if (out.oom) {
        free(out.data);
        return NULL;
    }
    if (!out.data) {
        char *empty = malloc(1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }
    return out.data;
}

bool edit_diff_stat(const char *old_text, const char *new_text,
                    int *out_added, int *out_removed, int *out_hunks) {
    if (out_added)
        *out_added = 0;
    if (out_removed)
        *out_removed = 0;
    if (out_hunks)
        *out_hunks = 0;
    if (!old_text)
        old_text = "";
    if (!new_text)
        new_text = "";
    if (strcmp(old_text, new_text) == 0)
        return false;

    ed_lines_t A, B;
    if (!ed_lines_split(old_text, &A))
        return false;
    if (!ed_lines_split(new_text, &B)) {
        ed_lines_free(&A);
        return false;
    }

    int added = 0, removed = 0, hunks = 0;
    bool counted = false;

    if (A.count <= EDIT_DIFF_MAX_LINES && B.count <= EDIT_DIFF_MAX_LINES) {
        ed_script_t s = {0};
        if (ed_myers(&A, &B, &s)) {
            bool in_change = false;
            for (int i = 0; i < s.count; i++) {
                if (s.op[i] == 'c') {
                    in_change = false;
                    continue;
                }
                if (!in_change) {
                    hunks++;
                    in_change = true;
                }
                if (s.op[i] == 'i')
                    added++;
                else
                    removed++;
            }
            counted = true;
        }
        ed_script_free(&s);
    }

    if (!counted) {
        int pre = 0;
        while (pre < A.count && pre < B.count && ed_line_eq(&A, pre, &B, pre))
            pre++;
        int suf = 0;
        while (suf < A.count - pre && suf < B.count - pre &&
               ed_line_eq(&A, A.count - 1 - suf, &B, B.count - 1 - suf))
            suf++;
        removed = A.count - suf - pre;
        added = B.count - suf - pre;
        hunks = (added || removed) ? 1 : 0;
    }

    ed_lines_free(&A);
    ed_lines_free(&B);

    if (out_added)
        *out_added = added;
    if (out_removed)
        *out_removed = removed;
    if (out_hunks)
        *out_hunks = hunks;
    return (added || removed);
}
