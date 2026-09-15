#ifndef DSCO_GOAL_H
#define DSCO_GOAL_H
#include "llm.h"
#include "json_util.h"

#define GOAL_PROMPT_SIZE 16384
#define GOAL_DEFAULT_TURNS 32
#define GOAL_NO_PROGRESS_LIMIT 3
#define GOAL_GET_SCHEMA "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
#define GOAL_UPDATE_SCHEMA "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"active\",\"complete\",\"blocked\"]},\"revision\":{\"type\":\"integer\"},\"evidence\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"}},\"required\":[\"status\",\"revision\",\"evidence\"],\"additionalProperties\":false}"

/* Goal metadata is not a capability grant or an independent verification verdict.
 * Owned by the agent thread. Neither model-facing handler is offload-safe. */
bool goal_is_active(const session_state_t *s);
void goal_clear(session_state_t *s);
void goal_account(session_state_t *s);
long long goal_tokens_used(const session_state_t *s);
int goal_parse_token_budget(const char *text);
bool goal_check_limits(session_state_t *s);
void goal_pause(session_state_t *s, const char *reason);
void goal_bootstrap_from_env(session_state_t *s);
bool goal_start(session_state_t *s, const char *objective, bool reset_accounting);
bool goal_should_auto_start(const char *text);
bool goal_prepare_turn(session_state_t *s);
void goal_make_autorun_prompt(session_state_t *s, char *out, size_t len);
void goal_make_runtime_context(const session_state_t *s, char *out, size_t len);
/* Count actual model request boundaries, not tool calls. Explicit resume resets
 * the request/no-progress segment; token usage never resets on resume/edit. */
void goal_turn_begin(session_state_t *s);
bool goal_continue(session_state_t *s, bool made_progress);
/* Slash commands are operator actions; the model may only report progress,
 * completion or a genuine blocker. Revisions reject stale updates. */
bool goal_command(session_state_t *s, const char *arg, char *out, size_t len, bool *changed);
bool goal_get(const session_state_t *s, const char *input, char *out, size_t len);
bool goal_update(session_state_t *s, const char *input, char *out, size_t len);
/* Promote a validated root terminal transition into the session goal in the
 * same governed tool call, avoiding a redundant model round trip. */
bool goal_commit_controller_terminal(session_state_t *s);
void goal_save_fields(jbuf_t *b, const session_state_t *s);
void goal_load_fields(session_state_t *s, const char *json);
/* Atomic snapshot of the existing conversation + goal, including empty chats. */
bool goal_checkpoint(conversation_t *conv, const session_state_t *s);
#endif
