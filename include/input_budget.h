#ifndef DSCO_INPUT_BUDGET_H
#define DSCO_INPUT_BUDGET_H
#include <stdbool.h>

typedef struct {
    int limit;
    int before_tokens;
    int after_tokens;
    int reduced_fields;
    bool admitted;
    char reason[192];
} input_budget_result_t;

/* Local, conservative estimate, NOT a provider tokenizer or billing receipt.
 * Applies to the complete serialized request, including instructions/tools.
 * Never mutates conversation history. Only tool observations and historical
 * assistant text may be shortened; user text, instructions and tool calls stay.
 * On rejection the caller still owns the original request and must not send it. */
bool input_budget_apply(char **request, int context_window, int output_reserve,
                        input_budget_result_t *result);
int input_budget_estimate(const char *request);
/* Configured limit: zero means model-aware default, -1 means invalid setting. */
int input_budget_configured_limit(void);
int input_budget_effective_limit(int context_window, int output_reserve);
#endif
