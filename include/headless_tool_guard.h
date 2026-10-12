#ifndef DSCO_HEADLESS_TOOL_GUARD_H
#define DSCO_HEADLESS_TOOL_GUARD_H

#include "json_util.h"
#include "tool_assurance.h"
#include <stdint.h>

typedef struct {
    tool_assurance_t assurance;
    bool forced;
    bool runtime_disabled;
    bool owns_choice;
    bool denied;
    unsigned failed_calls;
    unsigned consecutive_failures;
    unsigned repeated_results;
    uint64_t last_result_signature;
    char saved_choice[128];
} headless_tool_guard_t;

void headless_tool_guard_begin(headless_tool_guard_t *g, const char *prompt,
                              bool tools_disabled, bool require_execution);
bool headless_tool_guard_hold(const headless_tool_guard_t *g);
void headless_tool_guard_prepare(headless_tool_guard_t *g, char *choice, size_t cap);
bool headless_tool_guard_has_markup(const char *text);
void headless_tool_guard_note_result(headless_tool_guard_t *g, bool success, bool denied);
void headless_tool_guard_note_execution(headless_tool_guard_t *g, const char *name,
                                       const char *input, const char *result,
                                       bool success, bool denied);
const char *headless_tool_guard_stop_reason(const headless_tool_guard_t *g);
tool_assurance_decision_t headless_tool_guard_observe(
    headless_tool_guard_t *g, const parsed_response_t *response,
    char *repair, size_t repair_cap);
void headless_tool_guard_emit(const parsed_response_t *response,
                              void (*emit)(const char *, void *), void *ctx);

#endif
