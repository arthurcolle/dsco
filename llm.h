#ifndef DSCO_LLM_H
#define DSCO_LLM_H

#include <stdbool.h>
#include <stddef.h>
#include "json_util.h"

typedef enum { ROLE_USER, ROLE_ASSISTANT } msg_role_t;

typedef struct {
    char *type;
    char *text;
    char *tool_name;
    char *tool_id;
    char *tool_input;
    bool  is_error;
} msg_content_t;

typedef struct {
    msg_role_t    role;
    msg_content_t *content;
    int            content_count;
} message_t;

typedef struct {
    message_t *msgs;
    int        count;
    int        cap;
} conversation_t;

typedef struct {
    int input_tokens;
    int output_tokens;
    int cache_creation_input_tokens;
    int cache_read_input_tokens;
} usage_t;

/* Streaming callback: called with text deltas as they arrive */
typedef void (*stream_text_cb)(const char *text, void *ctx);
/* Called when a tool_use block starts */
typedef void (*stream_tool_start_cb)(const char *name, const char *id, void *ctx);

/* Streaming result — accumulated content blocks + usage */
typedef struct {
    parsed_response_t  parsed;
    usage_t            usage;
    int                http_status;
    bool               ok;
} stream_result_t;

void  conv_init(conversation_t *c);
void  conv_free(conversation_t *c);
void  conv_add_user_text(conversation_t *c, const char *text);
void  conv_add_assistant_text(conversation_t *c, const char *text);
void  conv_add_assistant_tool_use(conversation_t *c, const char *tool_id,
                                   const char *tool_name, const char *tool_input);
void  conv_add_tool_result(conversation_t *c, const char *tool_id,
                           const char *result, bool is_error);
void  conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp);

void  conv_pop_last(conversation_t *c);

/* Trim old tool results to keep conversation under budget.
   Keeps last `keep_recent` messages intact, truncates tool_result
   text in older messages to max_chars. */
void  conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);

char *llm_build_request(conversation_t *c, const char *model, int max_tokens);

/* Semantic-aware request builder: uses BM25+TF-IDF to select relevant tools
   based on the latest user query, reducing request size and improving routing. */
char *llm_build_request_semantic(conversation_t *c, const char *model,
                                  int max_tokens, const char *query_hint);

/* Streaming API call. Calls text_cb with text deltas in real-time.
 * Returns accumulated result. Caller must json_free_response(&result.parsed). */
stream_result_t llm_stream(const char *api_key, const char *request_json,
                           stream_text_cb text_cb,
                           stream_tool_start_cb tool_cb,
                           void *cb_ctx);

#endif
