#ifndef DSCO_JSON_UTIL_H
#define DSCO_JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* Dynamic string buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} jbuf_t;

/* ── Arena allocator ──────────────────────────────────────────────────── */

#define ARENA_CHUNK_SIZE (64 * 1024) /* 64KB per chunk */
#define ARENA_OVERSIZE (32 * 1024)   /* allocs > this go to separate chain */

typedef struct arena_chunk {
    struct arena_chunk *next;
    size_t cap;
    size_t used;
    char data[]; /* flexible array member */
} arena_chunk_t;

typedef struct arena_oversize {
    struct arena_oversize *next;
    void *ptr;
} arena_oversize_t;

typedef struct {
    arena_chunk_t *head;         /* linked list of chunks */
    arena_oversize_t *oversized; /* separate malloc chain for big allocs */
    size_t total_allocated;
} arena_t;

void arena_init(arena_t *a);
void *arena_alloc(arena_t *a, size_t size); /* 8-byte aligned bump */
char *arena_strdup(arena_t *a, const char *s);
void arena_reset(arena_t *a); /* rewind all chunks */
void arena_free(arena_t *a);  /* release all memory */

/* ── JSON schema validation ───────────────────────────────────────────── */

typedef struct {
    bool valid;
    char error[256];
    char field[64];
} json_validation_t;

json_validation_t json_validate_schema(const char *json, const char *schema_json);

/* Safe allocation helpers — abort on OOM rather than corrupt state */
void *safe_malloc(size_t size);
void *safe_realloc(void *ptr, size_t size);
/* Overflow-checked equivalent of realloc(ptr, count * elem_size). */
void *safe_reallocarray(void *ptr, size_t count, size_t elem_size);
char *safe_strdup(const char *s);

/* Use only with an assignable pointer lvalue; preserves its element type. */
#define DSCO_REALLOC_ARRAY(ptr, count) \
    ((ptr) = safe_reallocarray((ptr), (count), sizeof *(ptr)))

#define DSCO_ARRAY_LEN(array) (sizeof(array) / sizeof 0[array])

void jbuf_init(jbuf_t *b, size_t initial_cap);
void jbuf_free(jbuf_t *b);
void jbuf_reset(jbuf_t *b);
void jbuf_append(jbuf_t *b, const char *s);
void jbuf_append_len(jbuf_t *b, const char *s, size_t n);
void jbuf_appendf(jbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jbuf_append_char(jbuf_t *b, char c);
void jbuf_append_json_str(jbuf_t *b, const char *s);
void jbuf_append_int(jbuf_t *b, int v);

/* Parsed content block */
typedef struct {
    char *type;       /* "text", "tool_use", or "thinking" */
    char *text;       /* if type == "text" or "thinking" */
    char *tool_name;  /* if type == "tool_use" */
    char *tool_id;    /* if type == "tool_use" */
    char *tool_input; /* raw JSON string of input object */
} content_block_t;

typedef struct {
    content_block_t *blocks;
    int count;
    char *stop_reason;
} parsed_response_t;

bool json_parse_response(const char *json, parsed_response_t *out);
bool json_parse_response_arena(const char *json, parsed_response_t *out, arena_t *arena);
void json_free_response(parsed_response_t *r);

char *json_get_str(const char *json, const char *key);
char *json_get_raw(const char *json, const char *key);
int json_get_int(const char *json, const char *key, int def);
/* 64-bit variant: json_get_int truncates/clamps to int range; use this for
 * values that can legitimately exceed INT_MAX (timestamps, sequence numbers,
 * byte counts). */
long long json_get_i64(const char *json, const char *key, long long def);
bool json_get_bool(const char *json, const char *key, bool def);
double json_get_double(const char *json, const char *key, double def);

/* ── Parse-once view (yyjson-backed) ──────────────────────────────────────
 * json_get_* re-scan the whole document from the top on every call, which is
 * optimal for a single one-shot field but O(fields x size) when a handler pulls
 * many keys from one body (e.g. chat_prepare reads model/prompt/max_tokens/
 * temperature/top_p/top_k/seed/stop). Open a view once, then each accessor is a
 * hash lookup — measured ~66x faster than re-scanning per field. Returned
 * strings are BORROWED: valid only until json_view_close(). NULL-safe. */
typedef struct json_view json_view_t;
json_view_t *json_view_open(const char *json);
void json_view_close(json_view_t *v);
const char *json_view_str(json_view_t *v, const char *key); /* borrowed, no copy */
long long json_view_i64(json_view_t *v, const char *key, long long def);
double json_view_f64(json_view_t *v, const char *key, double def);
bool json_view_bool(json_view_t *v, const char *key, bool def);
/* True if the view parsed a JSON object (open returns non-NULL only then). */
bool json_view_ok(const json_view_t *v);

/* Strictly validate that the whole string is one balanced JSON object/array.
 * This is intentionally container-only because dsco structured outputs are
 * machine contracts, not scalar completions. */
bool json_is_valid_container(const char *json);

/* JSON array iteration helpers */
typedef void (*json_array_cb)(const char *element_start, void *ctx);
int json_array_foreach(const char *json, const char *key, json_array_cb cb, void *ctx);

#endif
