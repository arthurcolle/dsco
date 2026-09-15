#ifndef DSCO_DESKTOP_MACOS_H
#define DSCO_DESKTOP_MACOS_H

#include <stdbool.h>
#include <stddef.h>

#define DESKTOP_DESCRIPTION \
    "Inspect macOS desktop readiness, displays, and native windows; read bounded accessibility " \
    "snapshots; focus, move, or resize an exact window_id and pid. Status never prompts for " \
    "permissions. AX operations fail on ambiguous identity. Coordinates are global display " \
    "points. Permission prompts require the explicit request_permission action."

#define DESKTOP_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"list\",\"inspect\",\"snapshot\"," \
    "\"focus\",\"move\",\"resize\",\"set_bounds\",\"request_permission\"]}," \
    "\"window_id\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4294967295}," \
    "\"pid\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647}," \
    "\"x\":{\"type\":\"number\",\"minimum\":-100000,\"maximum\":100000}," \
    "\"y\":{\"type\":\"number\",\"minimum\":-100000,\"maximum\":100000}," \
    "\"width\":{\"type\":\"number\",\"minimum\":1,\"maximum\":100000}," \
    "\"height\":{\"type\":\"number\",\"minimum\":1,\"maximum\":100000}," \
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}," \
    "\"include_offscreen\":{\"type\":\"boolean\"}," \
    "\"include_titles\":{\"type\":\"boolean\"}," \
    "\"max_depth\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":8}," \
    "\"max_nodes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}," \
    "\"permission\":{\"type\":\"string\",\"enum\":[\"accessibility\",\"screen_recording\"]}" \
    "},\"required\":[\"action\"]}"

bool tool_desktop(const char *input_json, char *result, size_t result_len);

/* Shared process-local lock for focus-sensitive desktop input. Computer input
 * adapters can use this same lock; do not call tool_desktop while holding it. */
void desktop_input_lock(void);
void desktop_input_unlock(void);

#endif
