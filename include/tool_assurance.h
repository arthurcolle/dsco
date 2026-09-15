#ifndef DSCO_TOOL_ASSURANCE_H
#define DSCO_TOOL_ASSURANCE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOOL_ASSURANCE_ACCEPT = 0,
    TOOL_ASSURANCE_TOOL_CALL,
    TOOL_ASSURANCE_RETRY,
    TOOL_ASSURANCE_BLOCK,
} tool_assurance_decision_t;

typedef struct {
    bool required;
    bool disabled;
    bool attempted;
    bool satisfied;
    bool direct_preflight;
    unsigned retries;
    unsigned max_retries;
    char preferred_tool[64];
    char argument_name[32];
    char argument_value[192];
    char reason[96];
} tool_assurance_t;

/* Classify the user's requested outcome, independently of any provider or
 * executor adapter. Explicit runtime tool disabling always wins. */
void tool_assurance_begin(tool_assurance_t *state, const char *prompt,
                          bool tools_explicitly_disabled);

/* Tool-required turns buffer model prose until DSCO can prove that it is
 * grounded. This prevents a provider's false capability refusal from reaching
 * the user before the harness can recover. */
bool tool_assurance_should_hold_output(const tool_assurance_t *state);
bool tool_assurance_needs_required_choice(const tool_assurance_t *state);

void tool_assurance_note_preflight(tool_assurance_t *state, bool succeeded);
bool tool_assurance_response_claims_no_tools(const char *text);
tool_assurance_decision_t tool_assurance_observe(tool_assurance_t *state,
                                                 int tool_call_count,
                                                 const char *response_text);
void tool_assurance_retry_prompt(const tool_assurance_t *state, char *out, size_t out_len);

#endif
