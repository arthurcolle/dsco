#ifndef DSCO_DIRECTIVE_STORE_H
#define DSCO_DIRECTIVE_STORE_H

#include <stdbool.h>
#include <stddef.h>

/* Persistent, versioned agent-authored prompt overlay. The overlay is advisory:
 * platform/developer instructions and workspace constitutional documents retain
 * precedence. Mutations are capability-gated by the caller. */
const char *dsco_directive_prompt(void);
void dsco_directive_prompt_invalidate(void);

bool dsco_directive_status(char *out, size_t out_len);
bool dsco_directive_set(const char *content, const char *reason, char *out, size_t out_len);
bool dsco_directive_clear(const char *reason, char *out, size_t out_len);
bool dsco_directive_history(char *out, size_t out_len);
bool dsco_directive_rollback(const char *version, const char *reason, char *out, size_t out_len);

/* Standing-objective bridge: extract the "## Autonomous Objective" section of
 * the persistent directive (whole directive if no marker), report standing
 * activation state, and deploy/resume a durable detached activation bound to
 * the current objective. Deploy is exec-gated by the caller. */
bool dsco_directive_objective(char *out, size_t out_len);
bool dsco_directive_standing_status(char *out, size_t out_len);
bool dsco_directive_standing_deploy(const char *program, const char *model,
                                    double budget_usd, char *out, size_t out_len);

#endif
