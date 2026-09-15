#ifndef DSCO_PROMPT_BRANCH_H
#define DSCO_PROMPT_BRANCH_H
/* Versioned task content, not system/governance prompt mutation. JSON on stdin.
 * Local CLI authority is the OS principal; remote adapters must authenticate.
 * No provider execution, tools, implicit promotion, or external publication. */
int prompt_branch_cli(void);
#endif
