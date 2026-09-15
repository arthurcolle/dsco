#ifndef DSCO_BROWSER_SESSION_H
#define DSCO_BROWSER_SESSION_H

#include <stdbool.h>
#include <stddef.h>

/* One owned Chrome instance per harness process; never attaches to user profiles.
 * All calls must enter through tools_execute_for_tier(). Observation ingests
 * untrusted page content. Navigation, interaction and arbitrary JS can egress.
 */
#define BROWSER_SESSION_DESCRIPTION \
    "Control an isolated owned Chrome browser over CDP. Launch defaults headless; " \
    "status reports ownership; tabs lists exact tab IDs. All other actions require " \
    "the returned session_id, and page actions require tab_id. Snapshot returns DOM " \
    "text, unique CSS selectors and accessibility nodes. Click/type require a CSS " \
    "selector matching exactly one visible element. Evaluate runs JavaScript. " \
    "Screenshot returns a PNG image content block. Close without tab_id closes the " \
    "owned browser and removes its temporary profile. Does not attach to user Chrome."

#define BROWSER_SESSION_SCHEMA \
    "{\"type\":\"object\",\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"launch\",\"status\",\"tabs\",\"navigate\",\"snapshot\",\"click\",\"type\",\"evaluate\",\"screenshot\",\"close\"]}," \
    "\"session_id\":{\"type\":\"string\"},\"tab_id\":{\"type\":\"string\"}," \
    "\"url\":{\"type\":\"string\"},\"selector\":{\"type\":\"string\"}," \
    "\"text\":{\"type\":\"string\"},\"expression\":{\"type\":\"string\"}," \
    "\"headless\":{\"type\":\"boolean\",\"default\":true}," \
    "\"offline\":{\"type\":\"boolean\",\"default\":false}," \
    "\"clear\":{\"type\":\"boolean\",\"default\":true}," \
    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":60000}," \
    "\"max_chars\":{\"type\":\"integer\",\"minimum\":256,\"maximum\":32768}" \
    "},\"required\":[\"action\"],\"additionalProperties\":false}"

bool tool_browser_session(const char *input_json, char *result, size_t result_len);

#endif
