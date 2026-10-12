#ifndef DSCO_TIMEOUT_POLICY_H
#define DSCO_TIMEOUT_POLICY_H

/* Execution deadlines are distinct from connection/discovery and polling. */
#define DSCO_EXECUTION_DEFAULT_S 600
#define DSCO_EXECUTION_MAX_S 86400
int dsco_execution_default_s(void);
int dsco_execution_timeout_s(const char *tool, const char *input);
/* Durable operation lifetime: absent/zero = no deadline; negative = invalid.
 * Explicit caller or DSCO_COMMAND_TIMEOUT_S policy can impose a limit. */
int dsco_job_timeout_s(const char *input);
int dsco_mcp_call_timeout_ms(void);
int tool_timeout_for_input(const char *name, const char *input);

#endif
