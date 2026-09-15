#ifndef DSCO_CODEX_TOOL_BRIDGE_H
#define DSCO_CODEX_TOOL_BRIDGE_H
#include "json_util.h"
/* Parse only. The caller returns tool_use to the agent's ordinary gated dispatch. */
const char *codex_tool_bridge_schema(void);
bool codex_tool_bridge_parse(const char *answer, const char *request, parsed_response_t *out);
#endif
