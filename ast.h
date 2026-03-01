#ifndef DSCO_AST_H
#define DSCO_AST_H

#include <stdbool.h>
#include <stddef.h>

/* ── AST node types ───────────────────────────────────────────────────── */
typedef enum {
    AST_FUNCTION,
    AST_STRUCT,
    AST_TYPEDEF,
    AST_ENUM,
    AST_INCLUDE,
    AST_DEFINE,
    AST_GLOBAL_VAR,
    AST_TOOL_DEF,     /* tool_def_t registration */
} ast_node_type_t;

typedef struct {
    ast_node_type_t type;
    char *name;
    char *return_type;     /* functions */
    char *params;          /* functions: parameter list */
    char *body_preview;    /* first ~200 chars of body */
    char *include_path;    /* includes */
    char *value;           /* defines, typedefs */
    int  line_start;
    int  line_end;
    bool is_static;
    int  complexity;       /* cyclomatic complexity estimate */
} ast_node_t;

typedef struct {
    char        *filename;
    ast_node_t  *nodes;
    int          count;
    int          capacity;
    int          total_lines;
    int          code_lines;
    int          comment_lines;
    int          blank_lines;
    char       **includes;
    int          include_count;
} ast_file_t;

/* ── Parsing ──────────────────────────────────────────────────────────── */
ast_file_t *ast_parse_file(const char *path);
void        ast_free_file(ast_file_t *f);

/* ── Queries ──────────────────────────────────────────────────────────── */
ast_node_t *ast_find_function(ast_file_t *f, const char *name);
int         ast_count_type(ast_file_t *f, ast_node_type_t type);

/* ── Self-introspection ───────────────────────────────────────────────── */
typedef struct {
    ast_file_t **files;
    int          file_count;
    int          total_functions;
    int          total_structs;
    int          total_tools;
    int          total_lines;
    int          total_code_lines;
} ast_project_t;

ast_project_t *ast_introspect(const char *project_dir);
void           ast_free_project(ast_project_t *p);

/* ── Output formatting (fills buffer) ─────────────────────────────────── */
int ast_summary_json(ast_project_t *p, char *buf, size_t len);
int ast_file_summary_json(ast_file_t *f, char *buf, size_t len);
int ast_function_list_json(ast_file_t *f, char *buf, size_t len);

/* ── Dependency graph ─────────────────────────────────────────────────── */
int ast_dependency_graph(ast_project_t *p, char *buf, size_t len);

/* ── Call graph (finds function calls within bodies) ──────────────────── */
int ast_call_graph(ast_project_t *p, const char *func_name, char *buf, size_t len);

#endif
