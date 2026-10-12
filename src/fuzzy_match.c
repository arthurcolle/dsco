/* Multi-strategy edit matching. See include/fuzzy_match.h. */

#include "fuzzy_match.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Guard rails: these strategies are O(n*m) in lines, so cap the inputs they
 * will consider rather than letting a huge file stall the tool watchdog. */
#define FUZZY_MAX_HAYSTACK (8u * 1024u * 1024u)
#define FUZZY_MAX_LINES    200000

const char *fuzzy_strategy_name(fuzzy_strategy_t s) {
    switch (s) {
        case FUZZY_STRATEGY_EXACT: return "exact";
        case FUZZY_STRATEGY_LINE_TRIMMED: return "line-trimmed";
        case FUZZY_STRATEGY_WHITESPACE_NORMALIZED: return "whitespace-normalized";
        case FUZZY_STRATEGY_INDENT_FLEXIBLE: return "indent-flexible";
        case FUZZY_STRATEGY_ESCAPE_NORMALIZED: return "escape-normalized";
        case FUZZY_STRATEGY_TRIMMED_BOUNDARY: return "trimmed-boundary";
        case FUZZY_STRATEGY_BLOCK_ANCHOR: return "block-anchor";
        case FUZZY_STRATEGY_CONTEXT_AWARE: return "context-aware";
        case FUZZY_STRATEGY_NONE:
        case FUZZY_STRATEGY__COUNT:
        default: return "none";
    }
}

bool fuzzy_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("DSCO_EDIT_FUZZY");
        cached = 1;
        if (v && (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off")))
            cached = 0;
    }
    return cached != 0;
}

/* ── Similarity ───────────────────────────────────────────────────────────
 * Ratcliff/Obershelp: 2*M/T where M is the total size of matching regions
 * found by recursive longest-common-substring, T is the combined length.
 * This is what Python's difflib.SequenceMatcher.ratio() computes; the other
 * harnesses lean on it heavily, so matching its semantics keeps behavior
 * comparable while running orders of magnitude faster in C. */

static size_t fz_common_run(const char *a, size_t alen, const char *b, size_t blen,
                            size_t *abest, size_t *bbest, int depth) {
    /* Bounded recursion: pathological inputs degrade to a shorter match rather
     * than blowing the stack. */
    if (depth > 24 || !alen || !blen)
        return 0;
    size_t best = 0, ba = 0, bb = 0;
    /* Classic DP row over b for the longest common substring. */
    size_t *prev = calloc(blen + 1, sizeof(size_t));
    size_t *cur = calloc(blen + 1, sizeof(size_t));
    if (!prev || !cur) {
        free(prev);
        free(cur);
        return 0;
    }
    for (size_t i = 1; i <= alen; i++) {
        for (size_t j = 1; j <= blen; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) {
                    best = cur[j];
                    ba = i - best;
                    bb = j - best;
                }
            } else {
                cur[j] = 0;
            }
        }
        size_t *t = prev;
        prev = cur;
        cur = t;
        memset(cur, 0, (blen + 1) * sizeof(size_t));
    }
    free(prev);
    free(cur);
    if (!best)
        return 0;
    *abest = ba;
    *bbest = bb;
    return best;
}

static size_t fz_match_total(const char *a, size_t alen, const char *b, size_t blen, int depth) {
    size_t ai = 0, bi = 0;
    size_t run = fz_common_run(a, alen, b, blen, &ai, &bi, depth);
    if (!run)
        return 0;
    size_t total = run;
    total += fz_match_total(a, ai, b, bi, depth + 1);
    total += fz_match_total(a + ai + run, alen - ai - run, b + bi + run, blen - bi - run,
                            depth + 1);
    return total;
}

double fuzzy_similarity(const char *a, size_t alen, const char *b, size_t blen) {
    if (!a || !b)
        return 0.0;
    if (!alen && !blen)
        return 1.0;
    if (!alen || !blen)
        return 0.0;
    /* Cheap exit for identical strings, and a guard against quadratic blowup
     * on very long inputs (callers compare lines, not whole files). */
    if (alen == blen && memcmp(a, b, alen) == 0)
        return 1.0;
    if (alen > 4096 || blen > 4096)
        return 0.0;
    size_t m = fz_match_total(a, alen, b, blen, 0);
    return (2.0 * (double)m) / (double)(alen + blen);
}

/* ── Line indexing ────────────────────────────────────────────────────────
 * Strategies 2/4/6/7/8 all operate per line and must map a match back to byte
 * offsets in the ORIGINAL haystack. We index line starts once and reuse them,
 * which is what makes the normalized-position mapping exact rather than a
 * best-effort re-search (the failure mode that makes naive fuzzy edits corrupt
 * files). */

typedef struct {
    const char *base;
    size_t *start; /* byte offset of each line start */
    size_t *len;   /* line length excluding the newline */
    int count;
} fz_lines_t;

static void fz_lines_free(fz_lines_t *l) {
    if (!l)
        return;
    free(l->start);
    free(l->len);
    l->start = NULL;
    l->len = NULL;
    l->count = 0;
}

static bool fz_lines_index(const char *text, size_t tlen, fz_lines_t *out) {
    memset(out, 0, sizeof(*out));
    out->base = text;
    int cap = 64;
    out->start = malloc((size_t)cap * sizeof(size_t));
    out->len = malloc((size_t)cap * sizeof(size_t));
    if (!out->start || !out->len) {
        fz_lines_free(out);
        return false;
    }
    size_t pos = 0;
    for (;;) {
        if (out->count == cap) {
            int ncap = cap * 2;
            if (ncap > FUZZY_MAX_LINES)
                break;
            size_t *ns = realloc(out->start, (size_t)ncap * sizeof(size_t));
            size_t *nl = realloc(out->len, (size_t)ncap * sizeof(size_t));
            if (!ns || !nl) {
                if (ns) out->start = ns;
                if (nl) out->len = nl;
                fz_lines_free(out);
                return false;
            }
            out->start = ns;
            out->len = nl;
            cap = ncap;
        }
        const char *nl = memchr(text + pos, '\n', tlen - pos);
        size_t llen = nl ? (size_t)(nl - (text + pos)) : (tlen - pos);
        out->start[out->count] = pos;
        out->len[out->count] = llen;
        out->count++;
        if (!nl)
            break;
        pos += llen + 1;
        if (pos > tlen)
            break;
    }
    return true;
}

/* Trim helpers operating on a (ptr,len) slice without copying. */
static void fz_trim(const char **p, size_t *n) {
    while (*n && isspace((unsigned char)(*p)[0])) {
        (*p)++;
        (*n)--;
    }
    while (*n && isspace((unsigned char)(*p)[*n - 1]))
        (*n)--;
}

static void fz_ltrim(const char **p, size_t *n) {
    while (*n && ((*p)[0] == ' ' || (*p)[0] == '\t')) {
        (*p)++;
        (*n)--;
    }
}

static bool fz_slice_eq(const char *a, size_t an, const char *b, size_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* Collapse runs of space/tab to a single space; newlines preserved. */
static char *fz_collapse_ws(const char *s, size_t n, size_t *out_len) {
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    size_t w = 0;
    bool in_ws = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            if (!in_ws) {
                o[w++] = ' ';
                in_ws = true;
            }
        } else {
            in_ws = false;
            o[w++] = c;
        }
    }
    o[w] = '\0';
    if (out_len)
        *out_len = w;
    return o;
}

/* Turn literal backslash-escapes into the control characters they denote. */
static char *fz_unescape(const char *s, size_t n, size_t *out_len) {
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[i + 1];
            if (c == 'n') { o[w++] = '\n'; i++; continue; }
            if (c == 't') { o[w++] = '\t'; i++; continue; }
            if (c == 'r') { o[w++] = '\r'; i++; continue; }
        }
        o[w++] = s[i];
    }
    o[w] = '\0';
    if (out_len)
        *out_len = w;
    return o;
}

/* ── Strategies ───────────────────────────────────────────────────────────
 * Each returns the number of candidates found and, via out_start/out_end, the
 * byte span of the FIRST candidate in the original haystack. Returning the
 * count (not just the span) is what lets the driver refuse to guess when a
 * pattern is genuinely ambiguous. */

typedef int (*fz_strategy_fn)(const char *hay, size_t hlen, const char *pat, size_t plen,
                              size_t *out_start, size_t *out_end);

/* 1. Exact byte match. */
static int fz_s_exact(const char *hay, size_t hlen, const char *pat, size_t plen,
                      size_t *out_start, size_t *out_end) {
    if (!plen || plen > hlen)
        return 0;
    int count = 0;
    for (size_t i = 0; i + plen <= hlen; i++) {
        if (memcmp(hay + i, pat, plen) == 0) {
            if (!count) {
                *out_start = i;
                *out_end = i + plen;
            }
            count++;
            i += plen - 1;
        }
    }
    return count;
}

/* Shared driver for the per-line comparison strategies. `cmp_line` decides
 * whether a haystack line matches a pattern line; the span returned always
 * covers whole lines of the original text, so replacement never splices into
 * the middle of a line. */
typedef bool (*fz_line_cmp)(const char *hl, size_t hn, const char *pl, size_t pn);

static int fz_line_scan(const char *hay, size_t hlen, const char *pat, size_t plen,
                        fz_line_cmp cmp, size_t *out_start, size_t *out_end) {
    fz_lines_t H, P;
    if (!fz_lines_index(hay, hlen, &H))
        return 0;
    if (!fz_lines_index(pat, plen, &P)) {
        fz_lines_free(&H);
        return 0;
    }
    /* A trailing newline in the pattern yields a final empty line; ignore it so
     * "foo\n" and "foo" anchor identically. */
    int pc = P.count;
    if (pc > 1 && P.len[pc - 1] == 0)
        pc--;
    int count = 0;
    if (pc > 0 && pc <= H.count) {
        for (int i = 0; i + pc <= H.count; i++) {
            bool all = true;
            for (int k = 0; k < pc; k++) {
                if (!cmp(hay + H.start[i + k], H.len[i + k], pat + P.start[k], P.len[k])) {
                    all = false;
                    break;
                }
            }
            if (!all)
                continue;
            if (!count) {
                *out_start = H.start[i];
                size_t last = H.start[i + pc - 1] + H.len[i + pc - 1];
                /* Include the terminating newline when the pattern had one. */
                if (P.count > pc && last < hlen && hay[last] == '\n')
                    last++;
                *out_end = last;
            }
            count++;
            i += pc - 1;
        }
    }
    fz_lines_free(&H);
    fz_lines_free(&P);
    return count;
}

static bool fz_cmp_trimmed(const char *hl, size_t hn, const char *pl, size_t pn) {
    fz_trim(&hl, &hn);
    fz_trim(&pl, &pn);
    return fz_slice_eq(hl, hn, pl, pn);
}

static bool fz_cmp_lstripped(const char *hl, size_t hn, const char *pl, size_t pn) {
    fz_ltrim(&hl, &hn);
    fz_ltrim(&pl, &pn);
    /* Trailing whitespace still matters here; only indentation is ignored. */
    while (hn && isspace((unsigned char)hl[hn - 1])) hn--;
    while (pn && isspace((unsigned char)pl[pn - 1])) pn--;
    return fz_slice_eq(hl, hn, pl, pn);
}

/* 2. Per-line trimmed. */
static int fz_s_line_trimmed(const char *hay, size_t hlen, const char *pat, size_t plen,
                             size_t *s, size_t *e) {
    return fz_line_scan(hay, hlen, pat, plen, fz_cmp_trimmed, s, e);
}

/* 4. Indentation-flexible. */
static int fz_s_indent_flexible(const char *hay, size_t hlen, const char *pat, size_t plen,
                                size_t *s, size_t *e) {
    return fz_line_scan(hay, hlen, pat, plen, fz_cmp_lstripped, s, e);
}

/* 3. Whitespace-normalized: collapse runs of space/tab on both sides, match in
 * the normalized domain, then re-locate the span by scanning the original with
 * the same collapse applied incrementally. */
static int fz_s_ws_normalized(const char *hay, size_t hlen, const char *pat, size_t plen,
                              size_t *out_start, size_t *out_end) {
    size_t pn = 0;
    char *pnorm = fz_collapse_ws(pat, plen, &pn);
    if (!pnorm)
        return 0;
    if (!pn) {
        free(pnorm);
        return 0;
    }
    int count = 0;
    /* Walk candidate start offsets in the ORIGINAL text; compare by consuming
     * normalized characters. This avoids building a position map. */
    for (size_t i = 0; i < hlen; i++) {
        size_t hi = i, pi = 0;
        bool h_ws = false;
        while (pi < pn && hi < hlen) {
            char hc = hay[hi];
            if (hc == ' ' || hc == '\t') {
                if (!h_ws) {
                    if (pnorm[pi] != ' ')
                        break;
                    pi++;
                    h_ws = true;
                }
                hi++;
                continue;
            }
            h_ws = false;
            if (hc != pnorm[pi])
                break;
            hi++;
            pi++;
        }
        if (pi == pn) {
            if (!count) {
                *out_start = i;
                *out_end = hi;
            }
            count++;
            i = hi ? hi - 1 : i;
        }
    }
    free(pnorm);
    return count;
}

/* 5. Escape-normalized: the model wrote literal \n where a newline belongs. */
static int fz_s_escape_normalized(const char *hay, size_t hlen, const char *pat, size_t plen,
                                  size_t *s, size_t *e) {
    size_t un = 0;
    char *unesc = fz_unescape(pat, plen, &un);
    if (!unesc)
        return 0;
    int count = 0;
    if (un && un != plen) /* only meaningful if something actually changed */
        count = fz_s_exact(hay, hlen, unesc, un, s, e);
    free(unesc);
    return count;
}

/* 6. Trimmed boundary: only the first and last lines are whitespace-tolerant. */
static int fz_s_trimmed_boundary(const char *hay, size_t hlen, const char *pat, size_t plen,
                                 size_t *out_start, size_t *out_end) {
    const char *p = pat;
    size_t n = plen;
    fz_trim(&p, &n);
    if (!n || n == plen)
        return 0;
    return fz_s_exact(hay, hlen, p, n, out_start, out_end);
}

/* 7. Block anchor: first and last lines must match exactly (trimmed); the
 * interior only needs to be >=70% similar. This is the strategy that rescues
 * an edit whose middle was lightly reformatted but whose boundaries are
 * unambiguous. */
#define FZ_BLOCK_ANCHOR_MIN 0.70

static int fz_s_block_anchor(const char *hay, size_t hlen, const char *pat, size_t plen,
                             size_t *out_start, size_t *out_end) {
    fz_lines_t H, P;
    if (!fz_lines_index(hay, hlen, &H))
        return 0;
    if (!fz_lines_index(pat, plen, &P)) {
        fz_lines_free(&H);
        return 0;
    }
    int pc = P.count;
    if (pc > 1 && P.len[pc - 1] == 0)
        pc--;
    int count = 0;
    if (pc >= 2 && pc <= H.count) {
        const char *pf = pat + P.start[0];
        size_t pfn = P.len[0];
        fz_trim(&pf, &pfn);
        const char *plast = pat + P.start[pc - 1];
        size_t pln = P.len[pc - 1];
        fz_trim(&plast, &pln);

        for (int i = 0; i + pc <= H.count; i++) {
            const char *hf = hay + H.start[i];
            size_t hfn = H.len[i];
            fz_trim(&hf, &hfn);
            if (!fz_slice_eq(hf, hfn, pf, pfn))
                continue;
            const char *hl = hay + H.start[i + pc - 1];
            size_t hln = H.len[i + pc - 1];
            fz_trim(&hl, &hln);
            if (!fz_slice_eq(hl, hln, plast, pln))
                continue;

            double sim = 1.0;
            if (pc > 2) {
                size_t h_mid_start = H.start[i + 1];
                size_t h_mid_end = H.start[i + pc - 1];
                size_t p_mid_start = P.start[1];
                size_t p_mid_end = P.start[pc - 1];
                sim = fuzzy_similarity(hay + h_mid_start, h_mid_end - h_mid_start,
                                       pat + p_mid_start, p_mid_end - p_mid_start);
            }
            if (sim < FZ_BLOCK_ANCHOR_MIN)
                continue;
            if (!count) {
                *out_start = H.start[i];
                size_t last = H.start[i + pc - 1] + H.len[i + pc - 1];
                if (P.count > pc && last < hlen && hay[last] == '\n')
                    last++;
                *out_end = last;
            }
            count++;
            i += pc - 1;
        }
    }
    fz_lines_free(&H);
    fz_lines_free(&P);
    return count;
}

/* 8. Context-aware: at least half the lines must be >=80% similar. The most
 * permissive strategy, and deliberately last. */
#define FZ_CTX_LINE_MIN  0.80
#define FZ_CTX_BLOCK_MIN 0.50

static int fz_s_context_aware(const char *hay, size_t hlen, const char *pat, size_t plen,
                              size_t *out_start, size_t *out_end) {
    fz_lines_t H, P;
    if (!fz_lines_index(hay, hlen, &H))
        return 0;
    if (!fz_lines_index(pat, plen, &P)) {
        fz_lines_free(&H);
        return 0;
    }
    int pc = P.count;
    if (pc > 1 && P.len[pc - 1] == 0)
        pc--;
    int count = 0;
    if (pc > 0 && pc <= H.count) {
        for (int i = 0; i + pc <= H.count; i++) {
            int good = 0;
            for (int k = 0; k < pc; k++) {
                const char *hl = hay + H.start[i + k];
                size_t hn = H.len[i + k];
                const char *pl = pat + P.start[k];
                size_t pn = P.len[k];
                fz_trim(&hl, &hn);
                fz_trim(&pl, &pn);
                if (fuzzy_similarity(hl, hn, pl, pn) >= FZ_CTX_LINE_MIN)
                    good++;
            }
            if ((double)good < (double)pc * FZ_CTX_BLOCK_MIN)
                continue;
            if (!count) {
                *out_start = H.start[i];
                size_t last = H.start[i + pc - 1] + H.len[i + pc - 1];
                if (P.count > pc && last < hlen && hay[last] == '\n')
                    last++;
                *out_end = last;
            }
            count++;
            i += pc - 1;
        }
    }
    fz_lines_free(&H);
    fz_lines_free(&P);
    return count;
}

/* ── Chain driver ─────────────────────────────────────────────────────────
 * Try strategies strictest-first and stop at the first one that yields a
 * usable match. A strategy that finds multiple candidates when the caller
 * requires uniqueness is reported as ambiguous rather than guessed at: the
 * model gets told which strategy was ambiguous so it can add context. */

fuzzy_match_t fuzzy_find(const char *haystack, const char *needle,
                         bool allow_multiple, fuzzy_strategy_t max_strategy) {
    fuzzy_match_t m;
    memset(&m, 0, sizeof(m));
    if (!haystack || !needle || !needle[0])
        return m;

    size_t hlen = strlen(haystack);
    size_t plen = strlen(needle);
    if (hlen > FUZZY_MAX_HAYSTACK)
        max_strategy = FUZZY_STRATEGY_EXACT; /* stay cheap on very large files */
    if (!fuzzy_enabled() && max_strategy > FUZZY_STRATEGY_EXACT)
        max_strategy = FUZZY_STRATEGY_EXACT;

    static const struct {
        fuzzy_strategy_t id;
        fz_strategy_fn fn;
        double confidence;
    } chain[] = {
        {FUZZY_STRATEGY_EXACT, fz_s_exact, 1.00},
        {FUZZY_STRATEGY_LINE_TRIMMED, fz_s_line_trimmed, 0.97},
        {FUZZY_STRATEGY_WHITESPACE_NORMALIZED, fz_s_ws_normalized, 0.94},
        {FUZZY_STRATEGY_INDENT_FLEXIBLE, fz_s_indent_flexible, 0.91},
        {FUZZY_STRATEGY_ESCAPE_NORMALIZED, fz_s_escape_normalized, 0.88},
        {FUZZY_STRATEGY_TRIMMED_BOUNDARY, fz_s_trimmed_boundary, 0.85},
        {FUZZY_STRATEGY_BLOCK_ANCHOR, fz_s_block_anchor, 0.75},
        {FUZZY_STRATEGY_CONTEXT_AWARE, fz_s_context_aware, 0.60},
    };

    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        if (chain[i].id > max_strategy)
            break;
        size_t s = 0, e = 0;
        int n = chain[i].fn(haystack, hlen, needle, plen, &s, &e);

        /* An exact hit that starts mid-indentation is usually the model
         * under-indenting a whole line, not asking to splice into the middle
         * of one: "  x = 1" exact-matches the tail of "    x = 1", and
         * replacing that tail silently mangles the indentation. When the
         * pattern is itself line-shaped and a line-aligned strategy resolves
         * it unambiguously, prefer that interpretation. */
        if (n == 1 && chain[i].id == FUZZY_STRATEGY_EXACT && s > 0 &&
            haystack[s - 1] != '\n' && (haystack[s] == ' ' || haystack[s] == '\t')) {
            bool pat_is_line_shaped = (plen && (needle[0] == ' ' || needle[0] == '\t'));
            if (pat_is_line_shaped) {
                size_t ls = 0, le = 0;
                int ln = fz_s_line_trimmed(haystack, hlen, needle, plen, &ls, &le);
                if (ln == 1) {
                    m.found = true;
                    m.start = ls;
                    m.end = le;
                    m.occurrences = 1;
                    m.strategy = FUZZY_STRATEGY_LINE_TRIMMED;
                    m.confidence = 0.97;
                    return m;
                }
            }
        }
        if (n <= 0)
            continue;
        if (n > 1 && !allow_multiple) {
            /* Ambiguous at this strictness. Report it instead of guessing, and
             * do not fall through to a *more* permissive strategy — that would
             * only find more candidates. */
            m.found = false;
            m.occurrences = n;
            m.strategy = chain[i].id;
            m.confidence = chain[i].confidence;
            return m;
        }
        m.found = true;
        m.start = s;
        m.end = e;
        m.occurrences = n;
        m.strategy = chain[i].id;
        m.confidence = chain[i].confidence;
        return m;
    }
    return m;
}
