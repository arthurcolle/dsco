/* Pure extraction only: no filesystem, process, network, locale or model APIs. */
#include "workspace_intake.h"
#include "../vendor/yyjson.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_LIMIT 2048u
#define ROW_LIMIT 4096u
#define COL_LIMIT 256u
#define DEPTH_LIMIT 64u
#define META_LIMIT 4096u
#define SCHEMA "dsco.workspace.intake.v1"

typedef struct {
    size_t present, empty, min, max, emails, domains;
} column_t;
typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *root, *nodes, *edges, *documents, *summary, *rows;
    const char *error, *source, *format;
    size_t count, headings, goals, todos, checked, emails, domains;
    size_t records, columns, min_columns;
    column_t profile[COL_LIMIT];
    char *text;
    size_t used;
} intake_t;

static bool space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static bool alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool alnum(unsigned char c) { return alpha(c) || digit(c); }
static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n <= max && s[n]) n++;
    return n;
}
/* Strict UTF-8, no binary C0/C1 controls or directional override/isolate marks. */
static bool valid_text(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint32_t c = (unsigned char)s[i++], min = 0;
        size_t extra = 0;
        if (c >= 0x80) {
            if (c >= 0xc2 && c <= 0xdf) { extra = 1; c &= 31; min = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { extra = 2; c &= 15; min = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { extra = 3; c &= 7; min = 0x10000; }
            else return false;
            if (extra > n - i) return false;
            while (extra--) {
                unsigned char b = (unsigned char)s[i++];
                if ((b & 0xc0) != 0x80) return false;
                c = (c << 6) | (b & 63);
            }
            if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        }
        if ((c < 32 && c != 9 && c != 10 && c != 13) ||
            (c >= 0x7f && c <= 0x9f) || (c >= 0x202a && c <= 0x202e) ||
            (c >= 0x2066 && c <= 0x2069)) return false;
    }
    return true;
}
static bool failure(char *out, size_t cap, const char *code) {
    char msg[WORKSPACE_INTAKE_ERROR_CAP];
    int n = snprintf(msg, sizeof(msg),
        "{\"schema\":\"%s\",\"ok\":false,\"error\":{\"code\":\"%s\","
        "\"message\":\"Analysis rejected; no partial extraction returned\"}}", SCHEMA, code);
    if (out && cap) {
        out[0] = 0;
        if (n > 0 && (size_t)n < sizeof(msg) && (size_t)n < cap)
            memcpy(out, msg, (size_t)n + 1);
    }
    return false;
}
static void check(intake_t *s, bool ok) { if (!ok && !s->error) s->error = "allocation_failed"; }
static void strn(intake_t *s, yyjson_mut_val *v, const char *key, const char *text, size_t n) {
    check(s, yyjson_mut_obj_add_strncpy(s->doc, v, key, text, n));
}
static void str(intake_t *s, yyjson_mut_val *v, const char *key, const char *text) {
    strn(s, v, key, text, strlen(text));
}
static void num(intake_t *s, yyjson_mut_val *v, const char *key, size_t n) {
    check(s, yyjson_mut_obj_add_uint(s->doc, v, key, (uint64_t)n));
}
static yyjson_mut_val *array(intake_t *s, yyjson_mut_val *v, const char *key) {
    yyjson_mut_val *a = yyjson_mut_arr(s->doc);
    check(s, a && yyjson_mut_obj_add_val(s->doc, v, key, a));
    return a;
}
static void edge(intake_t *s, size_t from, size_t to, const char *relation) {
    yyjson_mut_val *e = yyjson_mut_arr_add_obj(s->doc, s->edges);
    check(s, e != NULL);
    num(s, e, "from", from); num(s, e, "to", to); str(s, e, "relation", relation);
    str(s, e, "basis", "observed");
}
static yyjson_mut_val *node(intake_t *s, const char *kind, const char *text, size_t n,
                            size_t parent, const char *relation) {
    if (s->error) return NULL;
    if (s->count >= NODE_LIMIT) { s->error = "node_limit"; return NULL; }
    size_t id = s->count++;
    yyjson_mut_val *v = yyjson_mut_arr_add_obj(s->doc, s->nodes);
    check(s, v != NULL);
    num(s, v, "id", id); str(s, v, "kind", kind); strn(s, v, "text", text, n);
    str(s, v, "basis", "observed");
    if (id) edge(s, parent, id, relation);
    return v;
}
static void appendn(intake_t *s, const char *p, size_t n) {
    if (s->error) return;
    if (n > WORKSPACE_INTAKE_MAX_OUTPUT - s->used) { s->error = "output_limit"; return; }
    memcpy(s->text + s->used, p, n); s->used += n; s->text[s->used] = 0;
}
static void append(intake_t *s, const char *p) { appendn(s, p, strlen(p)); }
static void number(intake_t *s, size_t n) {
    char b[32]; int k = snprintf(b, sizeof(b), "%zu", n);
    if (k > 0) appendn(s, b, (size_t)k);
}
/* Escape all ASCII punctuation, including entity introducers and fence markers.
 * Inline newlines/tabs are entities; source block lines are separately quoted.
 * Raw source can never close a fence, introduce HTML, links or a new heading. */
static void markdown(intake_t *s, const char *p, size_t n, bool block) {
    if (block) append(s, "> ");
    for (size_t i = 0; i < n && !s->error; i++) {
        unsigned char c = (unsigned char)p[i];
        if (block && (c == '\r' || c == '\n')) {
            if (c == '\r' && i + 1 < n && p[i + 1] == '\n') i++;
            append(s, "\n> ");
        } else if (alnum(c) || c == ' ' || c >= 0x80) appendn(s, p + i, 1);
        else {
            char b[16]; int k = snprintf(b, sizeof(b), "&#%u;", c);
            if (k > 0) appendn(s, b, (size_t)k);
        }
    }
}
static void begin_doc(intake_t *s, const char *title) {
    s->used = 0; s->text[0] = 0;
    append(s, "# "); append(s, title); append(s, "\n\nSource: ");
    markdown(s, s->source, strlen(s->source), false);
    append(s, "\n\nBasis: deterministic observed extraction; not semantic inference.\n\n");
}
static void end_doc(intake_t *s, const char *kind, const char *title) {
    if (s->error) return;
    yyjson_mut_val *v = yyjson_mut_arr_add_obj(s->doc, s->documents);
    check(s, v != NULL); str(s, v, "kind", kind); str(s, v, "title", title);
    strn(s, v, "text", s->text, s->used);
}

static bool domain_char(unsigned char c) { return alnum(c) || c == '-' || c == '.'; }
static bool local_char(unsigned char c) {
    return alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-' || c == '\'';
}
static bool domain_valid(const char *p, size_t n) {
    if (n < 4 || n > 253) return false;
    size_t start = 0, dots = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '.') {
            size_t len = i - start;
            if (!len || len > 63 || !alnum((unsigned char)p[start]) ||
                !alnum((unsigned char)p[i - 1])) return false;
            if (i == n) {
                if (len < 2) return false;
                bool has_alpha = false;
                for (size_t j = start; j < i; j++) if (alpha((unsigned char)p[j])) has_alpha = true;
                if (!has_alpha) return false;
            } else dots++;
            start = i + 1;
        }
    }
    return dots > 0;
}
static void location(intake_t *s, yyjson_mut_val *v, size_t offset, size_t row, size_t col) {
    num(s, v, row ? "cell_byte_offset" : "text_byte_offset", offset);
    if (row) { num(s, v, "row", row); num(s, v, "column", col); }
}
/* Offset is within the parent source/decoded JSON string or decoded CSV cell.
 * Email/domain matches are lexical occurrences only, not RFC mailbox validation. */
static void associations(intake_t *s, const char *p, size_t n, size_t parent,
                         size_t row, size_t col) {
    for (size_t i = 0; i < n && !s->error;) {
        if (!alnum((unsigned char)p[i]) ||
            (i && (local_char((unsigned char)p[i - 1]) || p[i - 1] == '@' ||
                   (unsigned char)p[i - 1] >= 0x80))) { i++; continue; }
        size_t at = i;
        while (at < n && local_char((unsigned char)p[at])) at++;
        bool email = at < n && p[at] == '@' && at - i <= 64 && p[at - 1] != '.';
        if (email) {
            for (size_t j = i + 1; j < at; j++) if (p[j] == '.' && p[j - 1] == '.') email = false;
        }
        size_t start = email ? at + 1 : i, end = start;
        while (end < n && domain_char((unsigned char)p[end])) end++;
        size_t stop = end;
        while (end > start && p[end - 1] == '.') end--;
        if (!domain_valid(p + start, end - start) ||
            (stop < n && (local_char((unsigned char)p[stop]) || p[stop] == '@' ||
                          (unsigned char)p[stop] >= 0x80))) {
            i = stop > i ? stop : i + 1; continue;
        }
        size_t email_id = 0;
        if (email) {
            email_id = s->count;
            yyjson_mut_val *v = node(s, "email_occurrence", p + i, end - i, parent, "observed_in");
            if (!v) return;
            location(s, v, i, row, col); s->emails++;
        }
        size_t domain_id = s->count;
        yyjson_mut_val *v = node(s, "domain_occurrence", p + start, end - start, parent, "observed_in");
        if (!v) return;
        location(s, v, start, row, col); s->domains++;
        if (email) edge(s, email_id, domain_id, "observed_email_domain");
        i = stop;
    }
}

/* Strict CSV state machine. A NULL state probes syntax/rectangularity only. */
static bool csv_parse(intake_t *s, const char *p, size_t n, bool *rectangular) {
    char *cell = s ? malloc(n + 1) : NULL;
    if (s && !cell) { s->error = "allocation_failed"; return false; }
    size_t pos = 0, rows = 0, first = 0, widest = 0, narrowest = COL_LIMIT;
    bool valid = true;
    while (pos < n && valid && (!s || !s->error)) {
        size_t col = 0;
        if (++rows > ROW_LIMIT) { if (s) s->error = "csv_row_limit"; valid = false; break; }
        for (;;) {
            size_t used = 0;
            if (++col > COL_LIMIT) { if (s) s->error = "csv_column_limit"; valid = false; break; }
            if (pos < n && p[pos] == '"') {
                pos++; bool closed = false;
                while (pos < n) {
                    char c = p[pos++];
                    if (c == '"') {
                        if (pos < n && p[pos] == '"') pos++;
                        else { closed = true; break; }
                    }
                    if (cell) cell[used] = c;
                    used++;
                }
                if (!closed || (pos < n && p[pos] != ',' && p[pos] != '\r' && p[pos] != '\n')) {
                    valid = false; break;
                }
            } else {
                while (pos < n && p[pos] != ',' && p[pos] != '\r' && p[pos] != '\n') {
                    if (p[pos] == '"') { valid = false; break; }
                    if (cell) cell[used] = p[pos];
                    used++; pos++;
                }
                if (!valid) break;
            }
            if (s) {
                cell[used] = 0;
                column_t *c = &s->profile[col - 1];
                if (!c->present || used < c->min) c->min = used;
                if (used > c->max) c->max = used;
                c->present++; if (!used) c->empty++;
                size_t emails = s->emails, domains = s->domains;
                associations(s, cell, used, 0, rows, col);
                c->emails += s->emails - emails; c->domains += s->domains - domains;
            }
            if (pos < n && p[pos] == ',') { pos++; continue; }
            if (pos < n) { char c = p[pos++]; if (c == '\r' && pos < n && p[pos] == '\n') pos++; }
            break;
        }
        if (!first) first = col;
        if (col > widest) widest = col;
        if (col < narrowest) narrowest = col;
        if (s && valid) check(s, yyjson_mut_arr_add_uint(s->doc, s->rows, col));
    }
    free(cell);
    if (rectangular) *rectangular = valid && rows >= 2 && first >= 2 && widest == narrowest;
    if (s) {
        s->records = rows; s->columns = widest; s->min_columns = rows ? narrowest : 0;
        if (!valid && !s->error) s->error = "invalid_csv";
    }
    return valid;
}
static bool suffix(const char *source, const char *ext) {
    size_t n = strlen(source), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        unsigned char c = (unsigned char)source[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)ext[i]) return false;
    }
    return true;
}
static size_t next_line(const char *p, size_t n, size_t start, size_t *end) {
    size_t i = start;
    while (i < n && p[i] != '\r' && p[i] != '\n') i++;
    *end = i;
    if (i < n) { char c = p[i++]; if (c == '\r' && i < n && p[i] == '\n') i++; }
    return i;
}
static void trim(const char **p, size_t *n) {
    while (*n && space((unsigned char)**p)) { (*p)++; (*n)--; }
    while (*n && space((unsigned char)(*p)[*n - 1])) (*n)--;
}
static void heading(intake_t *s, const char *p, size_t n, size_t level, size_t line) {
    yyjson_mut_val *v = node(s, "heading", p, n, 0, "observed_in");
    num(s, v, "level", level); num(s, v, "line", line); s->headings++;
}
static void markdown_parse(intake_t *s, const char *p, size_t n) {
    size_t start = 0, line = 0, fence_len = 0, prev_n = 0, prev_line = 0;
    const char *prev = NULL;
    char fence = 0;
    while (start < n && !s->error) {
        size_t end, next = next_line(p, n, start, &end); line++;
        const char *q = p + start; size_t len = end - start, indent = 0;
        while (indent < len && q[indent] == ' ') indent++;
        q += indent; len -= indent;
        if (indent < 4 && len >= 3 && (q[0] == '`' || q[0] == '~')) {
            size_t k = 1; while (k < len && q[k] == q[0]) k++;
            if (k >= 3) {
                if (!fence) {
                    /* Backtick info strings cannot contain a backtick. */
                    if (q[0] != '`' || !memchr(q + k, '`', len - k)) { fence = q[0]; fence_len = k; }
                } else if (q[0] == fence && k >= fence_len) {
                    size_t j = k; while (j < len && space((unsigned char)q[j])) j++;
                    if (j == len) fence = 0;
                }
                prev = NULL; start = next; continue;
            }
        }
        if (fence || indent >= 4 || (len && q[0] == '\t')) { prev = NULL; start = next; continue; }
        while (len && space((unsigned char)q[len - 1])) len--;
        size_t h = 0; while (h < len && q[h] == '#') h++;
        if (h && h <= 6 && (h == len || q[h] == ' ' || q[h] == '\t')) {
            const char *title = q + h; size_t title_n = len - h;
            trim(&title, &title_n);
            size_t k = title_n; while (k && title[k - 1] == '#') k--;
            if (k < title_n && k && space((unsigned char)title[k - 1])) { title_n = k; trim(&title, &title_n); }
            heading(s, title, title_n, h, line); prev = NULL; start = next; continue;
        }
        if (len && (q[0] == '=' || q[0] == '-')) {
            size_t k = 1; while (k < len && q[k] == q[0]) k++;
            if (k == len && prev) {
                heading(s, prev, prev_n, q[0] == '=' ? 1 : 2, prev_line);
                prev = NULL; start = next; continue;
            }
        }
        size_t k = 0;
        if (len > 1 && (q[0] == '-' || q[0] == '*' || q[0] == '+') && space((unsigned char)q[1])) k = 1;
        else {
            while (k < len && digit((unsigned char)q[k])) k++;
            if (!k || k >= len || (q[k] != '.' && q[k] != ')') ||
                k + 1 >= len || !space((unsigned char)q[k + 1])) k = 0;
            else k++;
        }
        bool list = k != 0, goal = false;
        while (k && k < len && space((unsigned char)q[k])) k++;
        if (list && len - k >= 3 && q[k] == '[' && q[k + 2] == ']' &&
            (q[k + 1] == ' ' || q[k + 1] == 'x' || q[k + 1] == 'X') &&
            (k + 3 == len || space((unsigned char)q[k + 3]))) {
            bool done = q[k + 1] != ' ';
            const char *text = q + k + 3; size_t size = len - k - 3; trim(&text, &size);
            yyjson_mut_val *v = node(s, "goal", text, size, 0, "observed_in");
            str(s, v, "marker", "checklist"); str(s, v, "status", done ? "checked" : "unchecked");
            num(s, v, "line", line); s->goals++; if (done) s->checked++; goal = true;
        }
        if (!goal) {
            for (size_t j = 0; j + 4 <= len; j++) {
                if (!memcmp(q + j, "TODO", 4) && (j == 0 || (!alnum((unsigned char)q[j - 1]) && q[j - 1] != '_')) &&
                    (j + 4 == len || q[j + 4] == ':' || q[j + 4] == '(' || space((unsigned char)q[j + 4]))) {
                    yyjson_mut_val *v = node(s, "goal", q, len, 0, "observed_in");
                    str(s, v, "marker", "TODO"); str(s, v, "status", "unspecified"); num(s, v, "line", line);
                    s->goals++; s->todos++; goal = true; break;
                }
            }
        }
        prev = len && !list && !goal ? q : NULL; prev_n = len; prev_line = line;
        start = next;
    }
}

static void json_tree(intake_t *s, yyjson_val *v, const char *key, size_t key_n,
                      size_t parent, size_t index, size_t depth) {
    if (s->error) return;
    if (depth > DEPTH_LIMIT) { s->error = "json_depth_limit"; return; }
    if ((key && !valid_text(key, key_n)) ||
        (yyjson_is_str(v) && !valid_text(yyjson_get_str(v), yyjson_get_len(v)))) {
        s->error = "invalid_text"; return;
    }
    const char *type = yyjson_is_raw(v) ? "number" : yyjson_get_type_desc(v);
    const char *text = type; size_t text_n = strlen(type);
    if (yyjson_is_str(v)) { text = yyjson_get_str(v); text_n = yyjson_get_len(v); }
    else if (yyjson_is_raw(v)) { text = yyjson_get_raw(v); text_n = yyjson_get_len(v); }
    else if (yyjson_is_bool(v)) { text = yyjson_get_bool(v) ? "true" : "false"; text_n = strlen(text); }
    size_t id = s->count;
    yyjson_mut_val *item = node(s, "json_value", text, text_n, parent, "contains");
    if (!item) return;
    str(s, item, "json_type", type); num(s, item, "depth", depth);
    if (key) strn(s, item, "key", key, key_n);
    else if (depth) num(s, item, "index", index);
    if (yyjson_is_str(v)) associations(s, text, text_n, id, 0, 0);
    if (yyjson_is_arr(v)) {
        num(s, item, "size", yyjson_arr_size(v));
        size_t i, count; yyjson_val *child;
        yyjson_arr_foreach(v, i, count, child) {
            json_tree(s, child, NULL, 0, id, i, depth + 1); if (s->error) return;
        }
    } else if (yyjson_is_obj(v)) {
        num(s, item, "size", yyjson_obj_size(v));
        size_t i, count; yyjson_val *k, *child;
        yyjson_obj_foreach(v, i, count, k, child) {
            /* Reject duplicate keys rather than implying last/first-wins semantics. */
            yyjson_obj_iter iter = yyjson_obj_iter_with(v);
            for (size_t j = 0; j < i; j++) {
                yyjson_val *other = yyjson_obj_iter_next(&iter);
                if (yyjson_get_len(k) == yyjson_get_len(other) &&
                    !memcmp(yyjson_get_str(k), yyjson_get_str(other), yyjson_get_len(k))) {
                    s->error = "duplicate_json_key"; return;
                }
            }
            json_tree(s, child, yyjson_get_str(k), yyjson_get_len(k), id, i, depth + 1);
            if (s->error) return;
        }
    }
}
static void metric(intake_t *s, const char *name, size_t n) {
    append(s, "- "); append(s, name); append(s, ": "); number(s, n); append(s, "\n");
}
static const char *mstr(yyjson_mut_val *v, const char *key) {
    const char *p = yyjson_mut_get_str(yyjson_mut_obj_get(v, key));
    return p ? p : "";
}
static void documents(intake_t *s, const char *p, size_t n, size_t lines, size_t words) {
    begin_doc(s, "Brief");
    append(s, "Format: "); append(s, s->format); append(s, "\n\n");
    metric(s, "Input bytes", n); metric(s, "Physical lines", lines); metric(s, "Whitespace-delimited tokens", words);
    metric(s, "Observed headings", s->headings); metric(s, "Observed goal markers", s->goals);
    append(s, "\nNo purpose, owner, deadline or relationship has been inferred.\n");
    end_doc(s, "brief", "Brief");

    begin_doc(s, "Goals"); append(s, "## Observed goal markers\n\n");
    size_t i, max; yyjson_mut_val *v;
    yyjson_mut_arr_foreach(s->nodes, i, max, v) {
        if (strcmp(mstr(v, "kind"), "goal")) continue;
        append(s, "- "); append(s, mstr(v, "marker")); append(s, " ("); append(s, mstr(v, "status"));
        append(s, ", line "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "line")));
        append(s, "): "); const char *t = mstr(v, "text"); markdown(s, t, strlen(t), false); append(s, "\n");
    }
    if (!s->goals) append(s, "No goal markers extracted.\n");
    append(s, "\n## Suggested goals (not observed)\n\nNone generated. Add your own suggestions here.\n");
    end_doc(s, "goals", "Goals");

    begin_doc(s, "Relationships");
    append(s, "Observed lexical email/domain occurrences only. These do not establish people, ownership, affiliation, communication or any inferred relationship.\n\n");
    yyjson_mut_arr_foreach(s->nodes, i, max, v) {
        const char *kind = mstr(v, "kind");
        if (strcmp(kind, "email_occurrence") && strcmp(kind, "domain_occurrence")) continue;
        append(s, "- Node "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "id")));
        append(s, " "); markdown(s, kind, strlen(kind), false); append(s, ": ");
        const char *t = mstr(v, "text"); markdown(s, t, strlen(t), false);
        if (yyjson_mut_obj_get(v, "row")) {
            append(s, " (record "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "row")));
            append(s, ", column "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "column"))); append(s, ")");
        }
        append(s, "\n");
    }
    if (!s->domains) append(s, "No lexical email/domain occurrences extracted.\n");
    end_doc(s, "relationships", "Relationships");

    begin_doc(s, "Analysis");
    append(s, "Graph IDs and edges describe observed containment/occurrences only. No executable content is interpreted.\n\n");
    if (!strcmp(s->format, "csv")) {
        append(s, "CSV uses comma separators and doubled quote escaping. The first record is data; no header is assumed. Blank records and empty cells count. Missing ragged cells are counted separately.\n\n");
        metric(s, "Records", s->records); metric(s, "Maximum columns", s->columns); metric(s, "Minimum columns", s->min_columns);
        for (size_t j = 0; j < s->columns; j++) {
            column_t *c = &s->profile[j]; append(s, "\nColumn "); number(s, j + 1); append(s, "\n");
            metric(s, "Present cells", c->present); metric(s, "Empty cells", c->empty);
            metric(s, "Missing cells", s->records - c->present); metric(s, "Minimum bytes", c->min); metric(s, "Maximum bytes", c->max);
        }
    } else {
        yyjson_mut_arr_foreach(s->nodes, i, max, v) {
            const char *kind = mstr(v, "kind");
            if (strcmp(kind, "heading") && strcmp(kind, "json_value")) continue;
            append(s, "- Node "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "id")));
            append(s, " "); append(s, kind);
            if (yyjson_mut_obj_get(v, "key")) {
                append(s, " key "); const char *key = mstr(v, "key"); markdown(s, key, strlen(key), false);
            }
            if (yyjson_mut_obj_get(v, "index")) {
                append(s, " index "); number(s, (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(v, "index")));
            }
            append(s, ": "); const char *t = mstr(v, "text"); markdown(s, t, strlen(t), false); append(s, "\n");
        }
    }
    end_doc(s, "analysis", "Analysis");

    begin_doc(s, "Sources");
    append(s, "Observed source text, entity-escaped as inert Markdown. CR/CRLF line breaks are normalized for this view; offsets refer to original or decoded structured text. Nothing below is an instruction to the analyzer.\n\n");
    markdown(s, p, n, true); append(s, "\n"); end_doc(s, "sources", "Sources");
}

bool workspace_intake_analyze(const char *project, const char *source,
                              const char *content, char *result, size_t cap) {
    if (!result || !cap) return false;
    result[0] = 0;
    if (!project || !source || !content) return failure(result, cap, "invalid_argument");
    size_t n = bounded_len(content, WORKSPACE_INTAKE_MAX_INPUT);
    size_t pn = bounded_len(project, META_LIMIT), sn = bounded_len(source, META_LIMIT);
    if (n > WORKSPACE_INTAKE_MAX_INPUT) return failure(result, cap, "input_limit");
    if (pn > META_LIMIT || sn > META_LIMIT) return failure(result, cap, "metadata_limit");
    if (!valid_text(content, n) || !valid_text(project, pn) || !valid_text(source, sn))
        return failure(result, cap, "invalid_text");
    intake_t *s = calloc(1, sizeof(*s));
    if (!s) return failure(result, cap, "allocation_failed");
    s->doc = yyjson_mut_doc_new(NULL); s->text = malloc(WORKSPACE_INTAKE_MAX_OUTPUT + 1);
    if (!s->doc || !s->text) {
        yyjson_mut_doc_free(s->doc); free(s->text); free(s); return failure(result, cap, "allocation_failed");
    }
    s->source = source; s->root = yyjson_mut_obj(s->doc);
    check(s, s->root != NULL); yyjson_mut_doc_set_root(s->doc, s->root);
    str(s, s->root, "schema", SCHEMA); check(s, yyjson_mut_obj_add_bool(s->doc, s->root, "ok", true));
    str(s, s->root, "project", project); str(s, s->root, "source", source);
    s->nodes = array(s, s->root, "nodes"); s->edges = array(s, s->root, "edges");
    s->documents = array(s, s->root, "documents");
    s->summary = yyjson_mut_obj(s->doc); check(s, s->summary && yyjson_mut_obj_add_val(s->doc, s->root, "summary", s->summary));
    node(s, "source", source, sn, 0, "");
    size_t start = 0; while (start < n && space((unsigned char)content[start])) start++;
    bool csv = false;
    if (suffix(source, ".csv")) s->format = "csv";
    else if (suffix(source, ".json") || (start < n && (content[start] == '{' || content[start] == '['))) s->format = "json";
    else {
        csv_parse(NULL, content, n, &csv);
        s->format = csv ? "csv" : "text";
    }
    if (!strcmp(s->format, "json")) {
        yyjson_doc *doc = yyjson_read(content, n, YYJSON_READ_NUMBER_AS_RAW);
        if (!doc) s->error = "invalid_json";
        else { json_tree(s, yyjson_doc_get_root(doc), NULL, 0, 0, 0, 0); yyjson_doc_free(doc); }
    } else if (!strcmp(s->format, "csv")) {
        yyjson_mut_val *c = yyjson_mut_obj(s->doc);
        check(s, c && yyjson_mut_obj_add_val(s->doc, s->root, "csv", c));
        s->rows = array(s, c, "row_columns");
        csv_parse(s, content, n, NULL);
        num(s, c, "records", s->records); num(s, c, "columns", s->columns);
        check(s, yyjson_mut_obj_add_bool(s->doc, c, "header_assumed", false));
        yyjson_mut_val *profiles = array(s, c, "column_profiles");
        for (size_t j = 0; j < s->columns && !s->error; j++) {
            column_t *p = &s->profile[j]; yyjson_mut_val *v = yyjson_mut_arr_add_obj(s->doc, profiles);
            check(s, v != NULL); num(s, v, "column", j + 1); num(s, v, "present", p->present);
            num(s, v, "empty", p->empty); num(s, v, "missing", s->records - p->present);
            num(s, v, "min_bytes", p->min); num(s, v, "max_bytes", p->max);
            num(s, v, "email_occurrences", p->emails); num(s, v, "domain_occurrences", p->domains);
        }
    } else {
        markdown_parse(s, content, n); associations(s, content, n, 0, 0, 0);
        if (s->headings || s->goals || suffix(source, ".md") || suffix(source, ".markdown")) s->format = "markdown";
    }
    size_t lines = 0, words = 0, offset = 0; bool word = false;
    while (offset < n) { size_t end; offset = next_line(content, n, offset, &end); lines++; }
    for (size_t j = 0; j < n; j++) {
        if (space((unsigned char)content[j])) word = false;
        else if (!word) { words++; word = true; }
    }
    str(s, s->summary, "format", s->format); num(s, s->summary, "bytes", n);
    num(s, s->summary, "lines", lines); num(s, s->summary, "whitespace_tokens", words);
    num(s, s->summary, "headings", s->headings); num(s, s->summary, "goals", s->goals);
    num(s, s->summary, "todo_markers", s->todos); num(s, s->summary, "checked_goals", s->checked);
    num(s, s->summary, "email_occurrences", s->emails); num(s, s->summary, "domain_occurrences", s->domains);
    num(s, s->summary, "nodes", s->count);
    if (!s->error) documents(s, content, n, lines, words);
    size_t out_n = 0; char *out = NULL;
    if (!s->error) { out = yyjson_mut_write(s->doc, 0, &out_n); if (!out) s->error = "allocation_failed"; }
    if (!s->error && out_n > WORKSPACE_INTAKE_MAX_OUTPUT) s->error = "output_limit";
    if (!s->error && out_n >= cap) s->error = "output_capacity";
    bool ok = !s->error;
    if (ok) memcpy(result, out, out_n + 1); else failure(result, cap, s->error);
    free(out); yyjson_mut_doc_free(s->doc); free(s->text); free(s);
    return ok;
}
