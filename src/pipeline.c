#include "pipeline.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Coroutine-based streaming pipeline engine
 *
 * Uses Simon Tatham's stackless coroutine technique (coroutine.h)
 * to chain data transform stages together. Each stage operates
 * on lines of text, yielding results to the next stage.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Helpers ──────────────────────────────────────────────────────────── */

static char **split_lines(const char *input, int *count) {
    int cap = 256;
    char **lines = malloc(cap * sizeof(char *));
    int n = 0;

    const char *p = input;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= cap) {
            cap *= 2;
            lines = realloc(lines, cap * sizeof(char *));
        }
        lines[n] = malloc(len + 1);
        memcpy(lines[n], p, len);
        lines[n][len] = '\0';
        n++;
        p = nl ? nl + 1 : p + len;
    }
    *count = n;
    return lines;
}

static void free_lines(char **lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

static char *join_lines(char **lines, int count, const char *sep) {
    size_t total = 0;
    size_t sep_len = strlen(sep);
    for (int i = 0; i < count; i++)
        total += strlen(lines[i]) + sep_len;
    total += 1;

    char *result = malloc(total);
    result[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(lines[i]);
        memcpy(result + pos, lines[i], len);
        pos += len;
        if (i < count - 1) {
            memcpy(result + pos, sep, sep_len);
            pos += sep_len;
        }
    }
    result[pos] = '\0';
    return result;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int cmp_str_r(const void *a, const void *b) {
    return -strcmp(*(const char **)a, *(const char **)b);
}

static int cmp_num(const void *a, const void *b) {
    double da = atof(*(const char **)a);
    double db = atof(*(const char **)b);
    return (da > db) - (da < db);
}

static bool simple_match(const char *line, const char *pattern) {
    /* Simple substring match, or regex if pattern starts with ^ or contains special chars */
    if (!pattern || !*pattern) return true;

    /* Try regex first */
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) == 0) {
        int result = regexec(&re, line, 0, NULL, 0);
        regfree(&re);
        return result == 0;
    }

    /* Fallback to substring */
    return strstr(line, pattern) != NULL;
}

/* ── Pipeline creation ───────────────────────────────────────────────── */

pipeline_t *pipeline_create(const char *input) {
    if (!input) return NULL;
    pipeline_t *p = calloc(1, sizeof(pipeline_t));
    if (!p) return NULL;
    p->input = strdup(input);
    if (!p->input) { free(p); return NULL; }
    p->lines = split_lines(input, &p->line_count);
    p->line_cap = p->line_count;
    return p;
}

void pipeline_free(pipeline_t *p) {
    if (!p) return;
    free_lines(p->lines, p->line_count);
    free(p->input);
    free(p);
}

void pipeline_add_stage(pipeline_t *p, pipe_stage_type_t type,
                        const char *arg) {
    if (p->stage_count >= PIPE_MAX_STAGES) return;
    pipe_stage_t *s = &p->stages[p->stage_count++];
    s->type = type;
    s->int_arg = 0;
    if (arg) {
        strncpy(s->arg, arg, PIPE_MAX_LINE_LEN - 1);
        s->arg[PIPE_MAX_LINE_LEN - 1] = '\0';
        /* Try to parse numeric arg */
        s->int_arg = atoi(arg);
    } else {
        s->arg[0] = '\0';
    }
}

void pipeline_add_stage_n(pipeline_t *p, pipe_stage_type_t type, int n) {
    if (p->stage_count >= PIPE_MAX_STAGES) return;
    pipe_stage_t *s = &p->stages[p->stage_count++];
    s->type = type;
    s->arg[0] = '\0';
    s->int_arg = n;
}

/* ── Stage execution ─────────────────────────────────────────────────── */

static void apply_stage(char ***lines, int *count, pipe_stage_t *stage) {
    char **in = *lines;
    int n = *count;

    switch (stage->type) {

    case PIPE_FILTER: {
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        for (int i = 0; i < n; i++) {
            if (simple_match(in[i], stage->arg))
                out[on++] = strdup(in[i]);
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_FILTER_V: {
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        for (int i = 0; i < n; i++) {
            if (!simple_match(in[i], stage->arg))
                out[on++] = strdup(in[i]);
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_MAP: {
        /* Simple s/pattern/replacement/ via regex */
        char *sep = strchr(stage->arg, '/');
        if (!sep) break;
        *sep = '\0';
        const char *pattern = stage->arg;
        const char *replacement = sep + 1;

        regex_t re;
        if (regcomp(&re, pattern, REG_EXTENDED) != 0) {
            *sep = '/';
            break;
        }

        for (int i = 0; i < n; i++) {
            regmatch_t match;
            if (regexec(&re, in[i], 1, &match, 0) == 0) {
                size_t pre_len = (size_t)match.rm_so;
                size_t post_off = (size_t)match.rm_eo;
                size_t rep_len = strlen(replacement);
                size_t post_len = strlen(in[i]) - post_off;
                char *new_line = malloc(pre_len + rep_len + post_len + 1);
                memcpy(new_line, in[i], pre_len);
                memcpy(new_line + pre_len, replacement, rep_len);
                memcpy(new_line + pre_len + rep_len, in[i] + post_off, post_len);
                new_line[pre_len + rep_len + post_len] = '\0';
                free(in[i]);
                in[i] = new_line;
            }
        }
        regfree(&re);
        *sep = '/';
        break;
    }

    case PIPE_SORT:
        qsort(in, (size_t)n, sizeof(char *), cmp_str);
        break;

    case PIPE_SORT_N:
        qsort(in, (size_t)n, sizeof(char *), cmp_num);
        break;

    case PIPE_SORT_R:
        qsort(in, (size_t)n, sizeof(char *), cmp_str_r);
        break;

    case PIPE_UNIQ: {
        if (n <= 1) break;
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        out[on++] = strdup(in[0]);
        for (int i = 1; i < n; i++) {
            if (strcmp(in[i], in[i-1]) != 0)
                out[on++] = strdup(in[i]);
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_UNIQ_C: {
        if (n == 0) break;
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        int cnt = 1;
        for (int i = 1; i <= n; i++) {
            if (i < n && strcmp(in[i], in[i-1]) == 0) {
                cnt++;
            } else {
                char buf[PIPE_MAX_LINE_LEN + 16];
                snprintf(buf, sizeof(buf), "%7d %s", cnt, in[i-1]);
                out[on++] = strdup(buf);
                cnt = 1;
            }
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_HEAD: {
        int limit = stage->int_arg > 0 ? stage->int_arg : 10;
        if (limit >= n) break;
        for (int i = limit; i < n; i++) free(in[i]);
        *count = limit;
        break;
    }

    case PIPE_TAIL: {
        int limit = stage->int_arg > 0 ? stage->int_arg : 10;
        if (limit >= n) break;
        int start = n - limit;
        char **out = malloc(limit * sizeof(char *));
        for (int i = 0; i < limit; i++) out[i] = strdup(in[start + i]);
        free_lines(in, n);
        *lines = out; *count = limit;
        break;
    }

    case PIPE_REVERSE: {
        for (int i = 0; i < n / 2; i++) {
            char *tmp = in[i];
            in[i] = in[n - 1 - i];
            in[n - 1 - i] = tmp;
        }
        break;
    }

    case PIPE_COUNT: {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", n);
        free_lines(in, n);
        *lines = malloc(sizeof(char *));
        (*lines)[0] = strdup(buf);
        *count = 1;
        break;
    }

    case PIPE_TRIM: {
        for (int i = 0; i < n; i++) {
            char *s = in[i];
            while (*s && isspace((unsigned char)*s)) s++;
            char *end = s + strlen(s);
            while (end > s && isspace((unsigned char)end[-1])) end--;
            size_t len = (size_t)(end - s);
            char *trimmed = malloc(len + 1);
            memcpy(trimmed, s, len);
            trimmed[len] = '\0';
            free(in[i]);
            in[i] = trimmed;
        }
        break;
    }

    case PIPE_UPPER:
        for (int i = 0; i < n; i++)
            for (char *p = in[i]; *p; p++) *p = (char)toupper((unsigned char)*p);
        break;

    case PIPE_LOWER:
        for (int i = 0; i < n; i++)
            for (char *p = in[i]; *p; p++) *p = (char)tolower((unsigned char)*p);
        break;

    case PIPE_PREFIX: {
        for (int i = 0; i < n; i++) {
            size_t plen = strlen(stage->arg);
            size_t llen = strlen(in[i]);
            char *new_line = malloc(plen + llen + 1);
            memcpy(new_line, stage->arg, plen);
            memcpy(new_line + plen, in[i], llen);
            new_line[plen + llen] = '\0';
            free(in[i]);
            in[i] = new_line;
        }
        break;
    }

    case PIPE_SUFFIX: {
        for (int i = 0; i < n; i++) {
            size_t llen = strlen(in[i]);
            size_t slen = strlen(stage->arg);
            char *new_line = malloc(llen + slen + 1);
            memcpy(new_line, in[i], llen);
            memcpy(new_line + llen, stage->arg, slen);
            new_line[llen + slen] = '\0';
            free(in[i]);
            in[i] = new_line;
        }
        break;
    }

    case PIPE_NUMBER: {
        for (int i = 0; i < n; i++) {
            char buf[PIPE_MAX_LINE_LEN + 16];
            snprintf(buf, sizeof(buf), "%4d  %s", i + 1, in[i]);
            free(in[i]);
            in[i] = strdup(buf);
        }
        break;
    }

    case PIPE_JOIN: {
        const char *sep = stage->arg[0] ? stage->arg : " ";
        char *joined = join_lines(in, n, sep);
        free_lines(in, n);
        *lines = malloc(sizeof(char *));
        (*lines)[0] = joined;
        *count = 1;
        break;
    }

    case PIPE_SPLIT: {
        char delim = stage->arg[0] ? stage->arg[0] : ',';
        char **out = malloc(n * 16 * sizeof(char *));
        int on = 0;
        for (int i = 0; i < n; i++) {
            char *tok = strtok(strdup(in[i]), (char[]){delim, '\0'});
            while (tok) {
                out[on++] = strdup(tok);
                tok = strtok(NULL, (char[]){delim, '\0'});
            }
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_CUT: {
        char delim = ',';
        int field = 0;
        /* arg format: "delimiter:field_index" or just "field_index" */
        if (strlen(stage->arg) >= 3 && stage->arg[1] == ':') {
            delim = stage->arg[0];
            field = atoi(stage->arg + 2);
        } else {
            field = stage->int_arg;
        }

        for (int i = 0; i < n; i++) {
            char *copy = strdup(in[i]);
            char delim_str[2] = {delim, '\0'};
            char *tok = strtok(copy, delim_str);
            int f = 0;
            char *found = NULL;
            while (tok) {
                if (f == field) { found = tok; break; }
                f++;
                tok = strtok(NULL, delim_str);
            }
            free(in[i]);
            in[i] = strdup(found ? found : "");
            free(copy);
        }
        break;
    }

    case PIPE_REGEX: {
        regex_t re;
        if (regcomp(&re, stage->arg, REG_EXTENDED) != 0) break;
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        for (int i = 0; i < n; i++) {
            regmatch_t match;
            if (regexec(&re, in[i], 1, &match, 0) == 0) {
                size_t mlen = (size_t)(match.rm_eo - match.rm_so);
                char *m = malloc(mlen + 1);
                memcpy(m, in[i] + match.rm_so, mlen);
                m[mlen] = '\0';
                out[on++] = m;
            }
        }
        regfree(&re);
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_REPLACE: {
        char *sep = strchr(stage->arg, '/');
        if (!sep) break;
        *sep = '\0';
        const char *old_str = stage->arg;
        const char *new_str = sep + 1;
        size_t old_len = strlen(old_str);
        size_t new_len = strlen(new_str);

        for (int i = 0; i < n; i++) {
            char *pos = strstr(in[i], old_str);
            if (pos) {
                size_t pre = (size_t)(pos - in[i]);
                size_t post = strlen(pos + old_len);
                char *new_line = malloc(pre + new_len + post + 1);
                memcpy(new_line, in[i], pre);
                memcpy(new_line + pre, new_str, new_len);
                memcpy(new_line + pre + new_len, pos + old_len, post + 1);
                free(in[i]);
                in[i] = new_line;
            }
        }
        *sep = '/';
        break;
    }

    case PIPE_TAKE_WHILE: {
        int end = 0;
        while (end < n && simple_match(in[end], stage->arg)) end++;
        for (int i = end; i < n; i++) free(in[i]);
        *count = end;
        break;
    }

    case PIPE_DROP_WHILE: {
        int start = 0;
        while (start < n && simple_match(in[start], stage->arg)) start++;
        char **out = malloc((n - start) * sizeof(char *));
        for (int i = start; i < n; i++) out[i - start] = strdup(in[i]);
        free_lines(in, n);
        *lines = out; *count = n - start;
        break;
    }

    case PIPE_FLATTEN:
        for (int i = 0; i < n; i++) {
            char *s = in[i];
            while (*s && isspace((unsigned char)*s)) s++;
            if (s != in[i]) {
                char *trimmed = strdup(s);
                free(in[i]);
                in[i] = trimmed;
            }
        }
        break;

    case PIPE_BLANK_REMOVE: {
        char **out = malloc(n * sizeof(char *));
        int on = 0;
        for (int i = 0; i < n; i++) {
            char *s = in[i];
            while (*s && isspace((unsigned char)*s)) s++;
            if (*s) out[on++] = strdup(in[i]);
        }
        free_lines(in, n);
        *lines = out; *count = on;
        break;
    }

    case PIPE_LENGTH:
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%zu", strlen(in[i]));
            free(in[i]);
            in[i] = strdup(buf);
        }
        break;

    case PIPE_HASH:
        for (int i = 0; i < n; i++) {
            char hex[65];
            sha256_hex((const uint8_t *)in[i], strlen(in[i]), hex);
            free(in[i]);
            in[i] = strdup(hex);
        }
        break;

    case PIPE_JSON_EXTRACT: {
        /* Very simple: extract "key": "value" from each line */
        char key_pattern[PIPE_MAX_LINE_LEN + 16];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", stage->arg);
        for (int i = 0; i < n; i++) {
            char *pos = strstr(in[i], key_pattern);
            if (pos) {
                pos += strlen(key_pattern);
                while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t')) pos++;
                if (*pos == '"') {
                    pos++;
                    char *end = strchr(pos, '"');
                    if (end) {
                        size_t vlen = (size_t)(end - pos);
                        char *val = malloc(vlen + 1);
                        memcpy(val, pos, vlen);
                        val[vlen] = '\0';
                        free(in[i]);
                        in[i] = val;
                        continue;
                    }
                }
                /* Numeric/bool value */
                char *end = pos;
                while (*end && *end != ',' && *end != '}' && *end != ' ') end++;
                size_t vlen = (size_t)(end - pos);
                char *val = malloc(vlen + 1);
                memcpy(val, pos, vlen);
                val[vlen] = '\0';
                free(in[i]);
                in[i] = val;
            } else {
                free(in[i]);
                in[i] = strdup("");
            }
        }
        break;
    }

    case PIPE_CSV_COLUMN: {
        int col = stage->int_arg;
        for (int i = 0; i < n; i++) {
            char *copy = strdup(in[i]);
            char *tok = strtok(copy, ",");
            int c = 0;
            char *found = NULL;
            while (tok) {
                /* Trim whitespace */
                while (*tok == ' ') tok++;
                if (c == col) { found = tok; break; }
                c++;
                tok = strtok(NULL, ",");
            }
            free(in[i]);
            in[i] = strdup(found ? found : "");
            free(copy);
        }
        break;
    }

    case PIPE_STATS: {
        int words = 0, chars = 0;
        for (int i = 0; i < n; i++) {
            chars += (int)strlen(in[i]);
            bool in_word = false;
            for (const char *p = in[i]; *p; p++) {
                if (isspace((unsigned char)*p)) { in_word = false; }
                else if (!in_word) { words++; in_word = true; }
            }
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "lines: %d\nwords: %d\nchars: %d",
                 n, words, chars);
        free_lines(in, n);
        *lines = split_lines(buf, count);
        break;
    }

    } /* end switch */
}

/* ── Execute pipeline ────────────────────────────────────────────────── */

char *pipeline_execute(pipeline_t *p) {
    char **lines = p->lines;
    int count = p->line_count;

    /* Detach lines from pipeline (stages will manage memory) */
    char **work = malloc(count * sizeof(char *));
    for (int i = 0; i < count; i++) work[i] = strdup(lines[i]);

    for (int s = 0; s < p->stage_count; s++) {
        apply_stage(&work, &count, &p->stages[s]);
    }

    char *result = join_lines(work, count, "\n");
    free_lines(work, count);
    return result;
}

/* ── Parse pipeline spec ─────────────────────────────────────────────── */

typedef struct {
    const char *name;
    pipe_stage_type_t type;
} stage_name_t;

static const stage_name_t STAGE_NAMES[] = {
    {"filter",       PIPE_FILTER},
    {"grep",         PIPE_FILTER},
    {"filter_v",     PIPE_FILTER_V},
    {"grep_v",       PIPE_FILTER_V},
    {"map",          PIPE_MAP},
    {"sed",          PIPE_MAP},
    {"sort",         PIPE_SORT},
    {"sort_n",       PIPE_SORT_N},
    {"sort_r",       PIPE_SORT_R},
    {"uniq",         PIPE_UNIQ},
    {"uniq_c",       PIPE_UNIQ_C},
    {"head",         PIPE_HEAD},
    {"tail",         PIPE_TAIL},
    {"reverse",      PIPE_REVERSE},
    {"rev",          PIPE_REVERSE},
    {"count",        PIPE_COUNT},
    {"wc",           PIPE_COUNT},
    {"trim",         PIPE_TRIM},
    {"upper",        PIPE_UPPER},
    {"lower",        PIPE_LOWER},
    {"prefix",       PIPE_PREFIX},
    {"suffix",       PIPE_SUFFIX},
    {"number",       PIPE_NUMBER},
    {"nl",           PIPE_NUMBER},
    {"join",         PIPE_JOIN},
    {"split",        PIPE_SPLIT},
    {"cut",          PIPE_CUT},
    {"regex",        PIPE_REGEX},
    {"replace",      PIPE_REPLACE},
    {"take_while",   PIPE_TAKE_WHILE},
    {"drop_while",   PIPE_DROP_WHILE},
    {"flatten",      PIPE_FLATTEN},
    {"blank_remove", PIPE_BLANK_REMOVE},
    {"compact",      PIPE_BLANK_REMOVE},
    {"length",       PIPE_LENGTH},
    {"hash",         PIPE_HASH},
    {"sha256",       PIPE_HASH},
    {"json_extract", PIPE_JSON_EXTRACT},
    {"jq",           PIPE_JSON_EXTRACT},
    {"csv_column",   PIPE_CSV_COLUMN},
    {"stats",        PIPE_STATS},
    {NULL, 0}
};

pipeline_t *pipeline_parse(const char *input, const char *spec) {
    if (!input || !spec) return NULL;
    pipeline_t *p = pipeline_create(input);
    if (!p) return NULL;

    /* Parse spec: "stage:arg|stage:arg|..." */
    char *spec_copy = strdup(spec);
    if (!spec_copy) { pipeline_free(p); return NULL; }
    char *save = NULL;
    char *token = strtok_r(spec_copy, "|", &save);

    while (token) {
        /* Enforce max stage count */
        if (p->stage_count >= PIPE_MAX_STAGES) break;
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token);
        while (end > token && end[-1] == ' ') *--end = '\0';

        /* Split name:arg */
        char *colon = strchr(token, ':');
        char *name = token;
        char *arg = NULL;
        if (colon) {
            *colon = '\0';
            arg = colon + 1;
        }

        /* Find stage type */
        for (const stage_name_t *sn = STAGE_NAMES; sn->name; sn++) {
            if (strcmp(sn->name, name) == 0) {
                if (arg)
                    pipeline_add_stage(p, sn->type, arg);
                else
                    pipeline_add_stage(p, sn->type, NULL);
                break;
            }
        }

        token = strtok_r(NULL, "|", &save);
    }

    free(spec_copy);
    return p;
}

char *pipeline_run(const char *input, const char *spec) {
    if (!input || !spec) return NULL;
    pipeline_t *p = pipeline_parse(input, spec);
    if (!p) return NULL;
    char *result = pipeline_execute(p);
    pipeline_free(p);
    return result;
}
