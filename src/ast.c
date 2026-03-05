#include "ast.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static char *read_file(const char *path, size_t *out_len) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* Cap maximum nodes to prevent unbounded growth */
#define AST_MAX_NODES 65536

static void ast_add_node(ast_file_t *f, ast_node_t *node) {
    if (f->count >= AST_MAX_NODES) return;
    if (f->count >= f->capacity) {
        int newcap = f->capacity ? f->capacity * 2 : 64;
        if (newcap > AST_MAX_NODES) newcap = AST_MAX_NODES;
        ast_node_t *tmp = realloc(f->nodes, newcap * sizeof(ast_node_t));
        if (!tmp) return;
        f->nodes = tmp;
        f->capacity = newcap;
    }
    f->nodes[f->count++] = *node;
}

static void ast_add_include(ast_file_t *f, const char *inc) {
    char **tmp = realloc(f->includes, (f->include_count + 1) * sizeof(char *));
    if (!tmp) return;
    f->includes = tmp;
    f->includes[f->include_count++] = strdup(inc);
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Count line number at position */
static int line_at(const char *start, const char *pos) {
    int line = 1;
    for (const char *p = start; p < pos; p++) {
        if (*p == '\n') line++;
    }
    return line;
}

/* Skip whitespace */
static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Skip a balanced brace block, return pointer after closing } */
static const char *skip_braces(const char *p) {
    if (*p != '{') return p;
    int depth = 1;
    p++;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        else if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        } else if (*p == '\'') {
            p++;
            if (*p == '\\') p++;
            if (*p) p++;
            if (*p == '\'') { /* skip closing quote */ }
        } else if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
        } else if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p++;
        }
        p++;
    }
    return p;
}

/* Extract identifier at position */
static char *extract_ident(const char *p) {
    const char *start = p;
    while (is_ident_char(*p)) p++;
    if (p == start) return NULL;
    int len = (int)(p - start);
    if (len > 4096) len = 4096; /* cap identifier length */
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

/* Estimate cyclomatic complexity from a function body */
static int estimate_complexity(const char *body, size_t len) {
    int cc = 1;
    for (size_t i = 0; i < len; i++) {
        if (body[i] == '"') {
            i++;
            while (i < len && body[i] != '"') { if (body[i] == '\\') i++; i++; }
            continue;
        }
        /* Look for keywords */
        if (i == 0 || !is_ident_char(body[i-1])) {
            if (starts_with(body + i, "if") && !is_ident_char(body[i+2])) cc++;
            else if (starts_with(body + i, "else") && !is_ident_char(body[i+4])) cc++;
            else if (starts_with(body + i, "for") && !is_ident_char(body[i+3])) cc++;
            else if (starts_with(body + i, "while") && !is_ident_char(body[i+5])) cc++;
            else if (starts_with(body + i, "case") && !is_ident_char(body[i+4])) cc++;
            else if (starts_with(body + i, "&&")) cc++;
            else if (starts_with(body + i, "||")) cc++;
            else if (starts_with(body + i, "?") && body[i] == '?') cc++;
        }
    }
    return cc;
}

/* ── Main parser ──────────────────────────────────────────────────────── */

ast_file_t *ast_parse_file(const char *path) {
    size_t file_len;
    char *src = read_file(path, &file_len);
    if (!src) return NULL;

    ast_file_t *f = calloc(1, sizeof(ast_file_t));
    f->filename = strdup(path);

    /* Line counting */
    bool in_block_comment = false;
    bool in_line_comment = false;
    bool has_code = false;
    int line_start = 0;

    for (size_t i = 0; i <= file_len; i++) {
        if (i == file_len || src[i] == '\n') {
            f->total_lines++;
            int line_len = (int)(i - line_start);
            if (line_len == 0 || (line_len == 1 && src[line_start] == '\r')) {
                f->blank_lines++;
            } else if (in_block_comment || in_line_comment) {
                f->comment_lines++;
            } else if (has_code) {
                f->code_lines++;
            } else {
                /* Check if line is only whitespace */
                bool only_ws = true;
                for (int j = line_start; j < (int)i; j++) {
                    if (!isspace((unsigned char)src[j])) { only_ws = false; break; }
                }
                if (only_ws) f->blank_lines++;
                else f->code_lines++;
            }
            line_start = (int)i + 1;
            in_line_comment = false;
            has_code = false;
            continue;
        }
        if (in_block_comment) {
            if (src[i] == '*' && i + 1 < file_len && src[i+1] == '/') {
                in_block_comment = false;
                i++;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < file_len) {
            if (src[i+1] == '/') { in_line_comment = true; continue; }
            if (src[i+1] == '*') { in_block_comment = true; i++; continue; }
        }
        if (!in_line_comment && !isspace((unsigned char)src[i])) has_code = true;
    }

    /* Parse declarations */
    const char *p = src;
    while (*p) {
        p = skip_space(p);
        if (!*p) break;

        /* Skip comments */
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        /* Preprocessor directives */
        if (*p == '#') {
            int ln = line_at(src, p);
            p++;
            p = skip_space(p);

            if (starts_with(p, "include")) {
                p += 7;
                p = skip_space(p);
                /* Extract include path */
                char delim = *p;
                if (delim == '"' || delim == '<') {
                    char end_delim = (delim == '"') ? '"' : '>';
                    p++;
                    const char *start = p;
                    while (*p && *p != end_delim && *p != '\n') p++;
                    int len = (int)(p - start);
                    char *inc = malloc(len + 1);
                    memcpy(inc, start, len);
                    inc[len] = '\0';

                    ast_node_t node = {0};
                    node.type = AST_INCLUDE;
                    node.name = inc;
                    node.include_path = strdup(inc);
                    node.line_start = ln;
                    node.line_end = ln;
                    ast_add_node(f, &node);
                    ast_add_include(f, inc);
                }
            } else if (starts_with(p, "define")) {
                p += 6;
                p = skip_space(p);
                char *name = extract_ident(p);
                if (name) {
                    ast_node_t node = {0};
                    node.type = AST_DEFINE;
                    node.name = name;
                    node.line_start = ln;

                    /* Skip to end of define (handle continuations) */
                    while (*p && *p != '\n') {
                        if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; }
                        p++;
                    }
                    node.line_end = line_at(src, p);
                    ast_add_node(f, &node);
                }
            }
            /* Skip rest of line */
            while (*p && *p != '\n') {
                if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; }
                p++;
            }
            continue;
        }

        /* Look for typedef, struct, enum, or function definitions */
        int ln = line_at(src, p);
        bool is_static = false;
        if (starts_with(p, "static") && !is_ident_char(p[6])) {
            is_static = true;
            p += 6;
            p = skip_space(p);
        }

        if (starts_with(p, "typedef") && !is_ident_char(p[7])) {
            p += 7;
            p = skip_space(p);

            /* typedef struct/enum */
            if (starts_with(p, "struct") || starts_with(p, "enum")) {
                bool is_enum = starts_with(p, "enum");
                p += is_enum ? 4 : 6;
                p = skip_space(p);

                /* Optional tag name */
                if (*p == '{' || is_ident_char(*p)) {
                    if (is_ident_char(*p)) {
                        while (is_ident_char(*p)) p++;
                        p = skip_space(p);
                    }
                    if (*p == '{') {
                        p = skip_braces(p);
                        p = skip_space(p);
                    }
                    /* Typedef name */
                    char *name = extract_ident(p);
                    if (name) {
                        ast_node_t node = {0};
                        node.type = is_enum ? AST_ENUM : AST_TYPEDEF;
                        node.name = name;
                        node.line_start = ln;
                        node.line_end = line_at(src, p);
                        ast_add_node(f, &node);
                        while (*p && *p != ';') p++;
                        if (*p) p++;
                        continue;
                    }
                }
            }
            /* Other typedef - skip to semicolon */
            while (*p && *p != ';') p++;
            if (*p) p++;
            continue;
        }

        if (starts_with(p, "struct") && !is_ident_char(p[6])) {
            p += 6;
            p = skip_space(p);
            char *name = extract_ident(p);
            if (name) {
                while (is_ident_char(*p)) p++;
                p = skip_space(p);
                if (*p == '{') {
                    ast_node_t node = {0};
                    node.type = AST_STRUCT;
                    node.name = name;
                    node.line_start = ln;
                    p = skip_braces(p);
                    node.line_end = line_at(src, p);
                    ast_add_node(f, &node);
                    while (*p && *p != ';') p++;
                    if (*p) p++;
                    continue;
                } else {
                    free(name);
                }
            }
        }

        if (starts_with(p, "enum") && !is_ident_char(p[4])) {
            p += 4;
            p = skip_space(p);
            char *name = extract_ident(p);
            if (name) {
                while (is_ident_char(*p)) p++;
                p = skip_space(p);
                if (*p == '{') {
                    ast_node_t node = {0};
                    node.type = AST_ENUM;
                    node.name = name;
                    node.line_start = ln;
                    p = skip_braces(p);
                    node.line_end = line_at(src, p);
                    ast_add_node(f, &node);
                    while (*p && *p != ';') p++;
                    if (*p) p++;
                    continue;
                } else {
                    free(name);
                }
            }
        }

        /* Try to detect function definitions:
         * pattern: [return_type] identifier ( params ) { body }
         * We look for: ident ( ... ) { */
        const char *decl_start = p;
        const char *return_type_start = p;

        /* Skip type specifiers */
        while (*p && *p != '(' && *p != '{' && *p != ';' && *p != '#') {
            if (*p == '/' && (*(p+1) == '/' || *(p+1) == '*')) break;
            p++;
        }

        if (*p == '(') {
            /* Backtrack to find function name */
            const char *paren = p;
            p--;
            while (p > decl_start && isspace((unsigned char)*p)) p--;
            const char *name_end = p + 1;
            while (p > decl_start && is_ident_char(*p)) p--;
            if (!is_ident_char(*p)) p++;
            const char *name_start = p;

            if (name_end > name_start) {
                /* Get return type */
                int rt_len = (int)(name_start - return_type_start);
                while (rt_len > 0 && isspace((unsigned char)return_type_start[rt_len - 1])) rt_len--;

                /* Skip past params */
                p = paren + 1;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
                const char *params_end = p;

                p = skip_space(p);

                if (*p == '{') {
                    /* It's a function definition */
                    const char *body_start = p;
                    const char *body_end = skip_braces(p);

                    ast_node_t node = {0};
                    node.type = AST_FUNCTION;
                    node.is_static = is_static;
                    node.line_start = ln;
                    node.line_end = line_at(src, body_end);

                    /* Name */
                    int nlen = (int)(name_end - name_start);
                    node.name = malloc(nlen + 1);
                    memcpy(node.name, name_start, nlen);
                    node.name[nlen] = '\0';

                    /* Return type */
                    if (rt_len > 0) {
                        node.return_type = malloc(rt_len + 1);
                        memcpy(node.return_type, return_type_start, rt_len);
                        node.return_type[rt_len] = '\0';
                    }

                    /* Params */
                    int plen = (int)(params_end - paren);
                    if (plen > 0 && plen < 500) {
                        node.params = malloc(plen + 1);
                        memcpy(node.params, paren, plen);
                        node.params[plen] = '\0';
                    }

                    /* Body preview */
                    size_t body_len = (size_t)(body_end - body_start);
                    int preview_len = body_len < 200 ? (int)body_len : 200;
                    node.body_preview = malloc(preview_len + 1);
                    memcpy(node.body_preview, body_start, preview_len);
                    node.body_preview[preview_len] = '\0';

                    /* Complexity */
                    node.complexity = estimate_complexity(body_start, body_len);

                    /* Check if this is a tool execution function */
                    if (starts_with(node.name, "tool_")) {
                        ast_node_t tool_node = {0};
                        tool_node.type = AST_TOOL_DEF;
                        tool_node.name = strdup(node.name + 5); /* strip tool_ prefix */
                        tool_node.line_start = node.line_start;
                        tool_node.line_end = node.line_end;
                        ast_add_node(f, &tool_node);
                    }

                    ast_add_node(f, &node);
                    p = body_end;
                    continue;
                }
            }
            p = paren + 1;
            /* Not a function def, skip to ; */
            while (*p && *p != ';' && *p != '{') p++;
            if (*p == '{') p = skip_braces(p);
            else if (*p) p++;
            continue;
        }

        /* Skip anything we don't recognize */
        if (*p == '{') {
            p = skip_braces(p);
        } else if (*p == ';') {
            p++;
        } else {
            p++;
        }
    }

    free(src);
    return f;
}

void ast_free_file(ast_file_t *f) {
    if (!f) return;
    for (int i = 0; i < f->count; i++) {
        free(f->nodes[i].name);
        free(f->nodes[i].return_type);
        free(f->nodes[i].params);
        free(f->nodes[i].body_preview);
        free(f->nodes[i].include_path);
        free(f->nodes[i].value);
    }
    free(f->nodes);
    for (int i = 0; i < f->include_count; i++) free(f->includes[i]);
    free(f->includes);
    free(f->filename);
    free(f);
}

/* ── Queries ──────────────────────────────────────────────────────────── */

ast_node_t *ast_find_function(ast_file_t *f, const char *name) {
    for (int i = 0; i < f->count; i++) {
        if (f->nodes[i].type == AST_FUNCTION && strcmp(f->nodes[i].name, name) == 0)
            return &f->nodes[i];
    }
    return NULL;
}

int ast_count_type(ast_file_t *f, ast_node_type_t type) {
    int count = 0;
    for (int i = 0; i < f->count; i++) {
        if (f->nodes[i].type == type) count++;
    }
    return count;
}

/* ── Project-level introspection ──────────────────────────────────────── */

static bool is_c_or_h(const char *name) {
    size_t n = strlen(name);
    if (n < 2) return false;
    return (name[n-2] == '.' && (name[n-1] == 'c' || name[n-1] == 'h'));
}

static void ast_collect_dir(ast_project_t *proj, const char *dir_path, int depth) {
    if (depth > 8) return;  /* prevent runaway recursion */
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;  /* skip hidden + . and .. */

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            ast_collect_dir(proj, path, depth + 1);
            continue;
        }

        if (!S_ISREG(st.st_mode) || !is_c_or_h(name)) continue;

        ast_file_t *af = ast_parse_file(path);
        if (!af) continue;

        ast_file_t **tmp = realloc(proj->files, (proj->file_count + 1) * sizeof(ast_file_t *));
        if (!tmp) { ast_free_file(af); continue; }
        proj->files = tmp;
        proj->files[proj->file_count++] = af;

        proj->total_lines += af->total_lines;
        proj->total_code_lines += af->code_lines;
        proj->total_functions += ast_count_type(af, AST_FUNCTION);
        proj->total_structs += ast_count_type(af, AST_STRUCT) +
                               ast_count_type(af, AST_TYPEDEF);
        proj->total_tools += ast_count_type(af, AST_TOOL_DEF);
    }

    closedir(dir);
}

ast_project_t *ast_introspect(const char *project_dir) {
    ast_project_t *proj = calloc(1, sizeof(ast_project_t));
    ast_collect_dir(proj, project_dir, 0);
    return proj;
}

void ast_free_project(ast_project_t *p) {
    if (!p) return;
    for (int i = 0; i < p->file_count; i++) ast_free_file(p->files[i]);
    free(p->files);
    free(p);
}

/* ── JSON output ──────────────────────────────────────────────────────── */

static const char *node_type_str(ast_node_type_t t) {
    switch (t) {
        case AST_FUNCTION:   return "function";
        case AST_STRUCT:     return "struct";
        case AST_TYPEDEF:    return "typedef";
        case AST_ENUM:       return "enum";
        case AST_INCLUDE:    return "include";
        case AST_DEFINE:     return "define";
        case AST_GLOBAL_VAR: return "global_var";
        case AST_TOOL_DEF:   return "tool_def";
    }
    return "unknown";
}

int ast_summary_json(ast_project_t *p, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"project\":{");
    jbuf_append(&b, "\"files\":");
    jbuf_append_int(&b, p->file_count);
    jbuf_append(&b, ",\"total_lines\":");
    jbuf_append_int(&b, p->total_lines);
    jbuf_append(&b, ",\"code_lines\":");
    jbuf_append_int(&b, p->total_code_lines);
    jbuf_append(&b, ",\"functions\":");
    jbuf_append_int(&b, p->total_functions);
    jbuf_append(&b, ",\"structs\":");
    jbuf_append_int(&b, p->total_structs);
    jbuf_append(&b, ",\"tools\":");
    jbuf_append_int(&b, p->total_tools);

    jbuf_append(&b, ",\"file_details\":[");
    for (int i = 0; i < p->file_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        ast_file_t *f = p->files[i];
        jbuf_append(&b, "{\"file\":");
        jbuf_append_json_str(&b, f->filename);
        jbuf_append(&b, ",\"lines\":");
        jbuf_append_int(&b, f->total_lines);
        jbuf_append(&b, ",\"code_lines\":");
        jbuf_append_int(&b, f->code_lines);
        jbuf_append(&b, ",\"comment_lines\":");
        jbuf_append_int(&b, f->comment_lines);
        jbuf_append(&b, ",\"blank_lines\":");
        jbuf_append_int(&b, f->blank_lines);
        jbuf_append(&b, ",\"functions\":");
        jbuf_append_int(&b, ast_count_type(f, AST_FUNCTION));
        jbuf_append(&b, ",\"structs\":");
        jbuf_append_int(&b, ast_count_type(f, AST_STRUCT));
        jbuf_append(&b, ",\"includes\":");
        jbuf_append_int(&b, f->include_count);
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

int ast_file_summary_json(ast_file_t *f, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 8192);

    jbuf_append(&b, "{\"file\":");
    jbuf_append_json_str(&b, f->filename);
    jbuf_append(&b, ",\"lines\":");
    jbuf_append_int(&b, f->total_lines);
    jbuf_append(&b, ",\"code_lines\":");
    jbuf_append_int(&b, f->code_lines);

    jbuf_append(&b, ",\"nodes\":[");
    for (int i = 0; i < f->count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        ast_node_t *n = &f->nodes[i];
        jbuf_append(&b, "{\"type\":");
        jbuf_append_json_str(&b, node_type_str(n->type));
        jbuf_append(&b, ",\"name\":");
        jbuf_append_json_str(&b, n->name);
        jbuf_append(&b, ",\"line_start\":");
        jbuf_append_int(&b, n->line_start);
        jbuf_append(&b, ",\"line_end\":");
        jbuf_append_int(&b, n->line_end);
        if (n->type == AST_FUNCTION) {
            if (n->return_type) {
                jbuf_append(&b, ",\"return_type\":");
                jbuf_append_json_str(&b, n->return_type);
            }
            if (n->params) {
                jbuf_append(&b, ",\"params\":");
                jbuf_append_json_str(&b, n->params);
            }
            jbuf_append(&b, ",\"is_static\":");
            jbuf_append(&b, n->is_static ? "true" : "false");
            jbuf_append(&b, ",\"complexity\":");
            jbuf_append_int(&b, n->complexity);
        }
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

int ast_function_list_json(ast_file_t *f, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"functions\":[");
    bool first = true;
    for (int i = 0; i < f->count; i++) {
        if (f->nodes[i].type != AST_FUNCTION) continue;
        if (!first) jbuf_append(&b, ",");
        first = false;
        ast_node_t *n = &f->nodes[i];
        jbuf_append(&b, "{\"name\":");
        jbuf_append_json_str(&b, n->name);
        if (n->return_type) {
            jbuf_append(&b, ",\"return_type\":");
            jbuf_append_json_str(&b, n->return_type);
        }
        jbuf_append(&b, ",\"lines\":[");
        jbuf_append_int(&b, n->line_start);
        jbuf_append(&b, ",");
        jbuf_append_int(&b, n->line_end);
        jbuf_append(&b, "],\"complexity\":");
        jbuf_append_int(&b, n->complexity);
        jbuf_append(&b, ",\"static\":");
        jbuf_append(&b, n->is_static ? "true" : "false");
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

/* ── Dependency graph ─────────────────────────────────────────────────── */

int ast_dependency_graph(ast_project_t *p, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"dependencies\":[");
    bool first = true;
    for (int i = 0; i < p->file_count; i++) {
        ast_file_t *f = p->files[i];
        for (int j = 0; j < f->include_count; j++) {
            /* Check if this include maps to a project file */
            for (int k = 0; k < p->file_count; k++) {
                const char *fname = strrchr(p->files[k]->filename, '/');
                fname = fname ? fname + 1 : p->files[k]->filename;
                if (strcmp(fname, f->includes[j]) == 0) {
                    if (!first) jbuf_append(&b, ",");
                    first = false;
                    const char *src_name = strrchr(f->filename, '/');
                    src_name = src_name ? src_name + 1 : f->filename;
                    jbuf_append(&b, "{\"from\":");
                    jbuf_append_json_str(&b, src_name);
                    jbuf_append(&b, ",\"to\":");
                    jbuf_append_json_str(&b, f->includes[j]);
                    jbuf_append(&b, "}");
                }
            }
        }
    }
    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

/* ── Call graph ───────────────────────────────────────────────────────── */

int ast_call_graph(ast_project_t *p, const char *func_name, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    /* Find the function */
    ast_node_t *target = NULL;
    for (int i = 0; i < p->file_count && !target; i++) {
        target = ast_find_function(p->files[i], func_name);
    }

    jbuf_append(&b, "{\"function\":");
    jbuf_append_json_str(&b, func_name);

    if (target && target->body_preview) {
        jbuf_append(&b, ",\"calls\":[");
        /* Scan body for function call patterns: ident( */
        const char *body = target->body_preview;
        bool first = true;
        for (const char *q = body; *q; q++) {
            if (is_ident_char(*q)) {
                const char *start = q;
                while (*q && is_ident_char(*q)) q++;
                const char *end = q;
                while (*q && isspace((unsigned char)*q)) q++;
                if (*q == '(') {
                    int nlen = (int)(end - start);
                    char callee[256];
                    if (nlen < (int)sizeof(callee)) {
                        memcpy(callee, start, nlen);
                        callee[nlen] = '\0';
                        /* Skip C keywords */
                        if (strcmp(callee, "if") && strcmp(callee, "for") &&
                            strcmp(callee, "while") && strcmp(callee, "switch") &&
                            strcmp(callee, "return") && strcmp(callee, "sizeof")) {
                            if (!first) jbuf_append(&b, ",");
                            first = false;
                            jbuf_append_json_str(&b, callee);
                        }
                    }
                }
            }
        }
        jbuf_append(&b, "]");
    }
    jbuf_append(&b, "}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}
