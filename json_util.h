#ifndef DSCO_JSON_UTIL_H
#define DSCO_JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* Dynamic string buffer */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} jbuf_t;

void   jbuf_init(jbuf_t *b, size_t initial_cap);
void   jbuf_free(jbuf_t *b);
void   jbuf_reset(jbuf_t *b);
void   jbuf_append(jbuf_t *b, const char *s);
void   jbuf_append_len(jbuf_t *b, const char *s, size_t n);
void   jbuf_append_char(jbuf_t *b, char c);
void   jbuf_append_json_str(jbuf_t *b, const char *s);
void   jbuf_append_int(jbuf_t *b, int v);

/* Parsed content block */
typedef struct {
    char *type;          /* "text" or "tool_use" */
    char *text;          /* if type == "text" */
    char *tool_name;     /* if type == "tool_use" */
    char *tool_id;       /* if type == "tool_use" */
    char *tool_input;    /* raw JSON string of input object */
} content_block_t;

typedef struct {
    content_block_t *blocks;
    int              count;
    char            *stop_reason;
} parsed_response_t;

bool json_parse_response(const char *json, parsed_response_t *out);
void json_free_response(parsed_response_t *r);

char *json_get_str(const char *json, const char *key);
char *json_get_raw(const char *json, const char *key);
int   json_get_int(const char *json, const char *key, int def);
bool  json_get_bool(const char *json, const char *key, bool def);

/* JSON array iteration helpers */
typedef void (*json_array_cb)(const char *element_start, void *ctx);
int json_array_foreach(const char *json, const char *key, json_array_cb cb, void *ctx);

#endif
