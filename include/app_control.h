#ifndef DSCO_APP_CONTROL_H
#define DSCO_APP_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

/* Background computer use: drive one app's UI through macOS Accessibility
 * without moving the user's pointer, stealing focus or reading other apps.
 * Element ids (e1, e2, ...) come from the latest observe/find and stay valid
 * until the next observe/find. */

#define APP_CONTROL_DESCRIPTION \
    "Operate a macOS app in the background without touching the user's cursor or focus. " \
    "Target an app by pid or app name. observe lists actionable UI elements with ids and " \
    "window-relative frames; find ranks elements for a description (accessibility text first, " \
    "TypeSafe vision fallback on the window image); press/set_value/focus act on an element id; " \
    "type/key send keystrokes only to that app; menu picks a menu path like [\"File\",\"Save\"]; " \
    "screenshot attaches only that window. Prefer press/set_value/menu over type/key. " \
    "Re-observe after actions that change the UI."

#define APP_CONTROL_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"apps\",\"observe\",\"find\",\"press\"," \
    "\"set_value\",\"focus\",\"type\",\"key\",\"menu\",\"screenshot\"]}," \
    "\"pid\":{\"type\":\"integer\",\"minimum\":1}," \
    "\"app\":{\"type\":\"string\",\"description\":\"App name (alternative to pid)\"}," \
    "\"window_id\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Defaults to the app's focused or main window\"}," \
    "\"element\":{\"type\":\"string\",\"description\":\"Element id from observe/find, e.g. e12\"}," \
    "\"query\":{\"type\":\"string\",\"description\":\"find: what to locate, e.g. 'Save button'\"}," \
    "\"text\":{\"type\":\"string\",\"description\":\"set_value/type text\"}," \
    "\"keys\":{\"type\":\"string\",\"description\":\"key: combo like cmd+s, return, shift+tab\"}," \
    "\"path\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"menu: titles from the menu bar down\"}," \
    "\"max_elements\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":400}," \
    "\"include_text\":{\"type\":\"boolean\",\"description\":\"observe: include static text (default true)\"}," \
    "\"screenshot\":{\"type\":\"boolean\",\"description\":\"observe: also attach the window image (default false)\"}," \
    "\"vision\":{\"type\":\"boolean\",\"description\":\"find: allow TypeSafe vision fallback (default true when configured)\"}," \
    "\"min_confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}" \
    "},\"required\":[\"action\"]}"

#define APP_CONTROL_MAX_ELEMENTS 400

bool tool_app_control(const char *input_json, char *result, size_t result_len);

/* Pure scoring used by find; exposed for offline tests. Returns 0..1. */
double app_control_match_score(const char *query, const char *role, const char *label,
                               const char *value, bool enabled);

/* True for actions that change another app's state (press, type, ...). */
bool app_control_action_mutates(const char *action);

#endif
