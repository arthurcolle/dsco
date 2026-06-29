#ifndef DSCO_WASM_CORE_H
#define DSCO_WASM_CORE_H

const char *dsco_wasm_version(void);
const char *dsco_wasm_exports_json(void);
const char *dsco_wasm_models_json(void);
const char *dsco_wasm_tools_json(void);
const char *dsco_wasm_route_explain(const char *model);
const char *dsco_wasm_tool_exec(const char *name, const char *input_json);
const char *dsco_wasm_session_reset(void);
const char *dsco_wasm_session_add(const char *role, const char *content);
const char *dsco_wasm_session_state(void);

#endif
