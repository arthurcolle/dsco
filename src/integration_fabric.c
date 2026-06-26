#include "integration_fabric.h"

#include <ctype.h>
#include <string.h>

static const dsco_integration_profile_t g_profiles[] = {
    {"gmail", "Gmail", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"google_calendar", "Google Calendar", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ, true, false},
    {"github", "GitHub", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"botmail", "Botmail Agent Email", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"document_qa", "Document QA", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"video_qa", "Video QA", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"web_search", "Web Search", DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT, true, false},
    {"cloudmail_mcp", "Cloudmail MCP", DSCO_INTEGRATION_SCOPE_PRIVATE_SERVICE,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_WRITE |
         DSCO_INTEGRATION_ACTION_SEND | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT |
         DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION,
     false, true},
    {"heat_mcp", "HEAT MCP", DSCO_INTEGRATION_SCOPE_PRIVATE_SERVICE,
     DSCO_INTEGRATION_ACTION_READ, false, true},
    {"email_remote", "Remote Email", DSCO_INTEGRATION_SCOPE_PRIVATE_SERVICE,
     DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_SEND |
         DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION,
     false, true},
    {"neon", "Neon", DSCO_INTEGRATION_SCOPE_INFRASTRUCTURE,
     DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_ADMIN, false, true},
    {"multi_agent", "Multi-agent", DSCO_INTEGRATION_SCOPE_AGENTIC,
     DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_ADMIN |
         DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION,
     false, true},
};

static void fold_name(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t j = 0;
    bool prev_sep = false;
    for (size_t i = 0; in && in[i] && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
            prev_sep = false;
        } else {
            if (!prev_sep && j + 1 < out_len)
                out[j++] = '_';
            prev_sep = true;
        }
    }
    out[j] = '\0';
}

static bool has_token(const char *folded, const char *needle) {
    return folded && needle && strstr(folded, needle) != NULL;
}

static const dsco_integration_profile_t *profile_by_id(const char *id) {
    for (size_t i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]); i++) {
        if (strcmp(g_profiles[i].id, id) == 0)
            return &g_profiles[i];
    }
    return NULL;
}

static const dsco_integration_profile_t *profile_for_folded(const char *folded) {
    if (!folded || !folded[0])
        return NULL;
    if (has_token(folded, "google_calendar") ||
        (has_token(folded, "google") && has_token(folded, "calendar")))
        return profile_by_id("google_calendar");
    if (has_token(folded, "gmail"))
        return profile_by_id("gmail");
    if (has_token(folded, "botmail") || has_token(folded, "agent_email"))
        return profile_by_id("botmail");
    if (has_token(folded, "document_qa") || has_token(folded, "documentqa"))
        return profile_by_id("document_qa");
    if (has_token(folded, "video_qa") || has_token(folded, "videoqa"))
        return profile_by_id("video_qa");
    if (has_token(folded, "github"))
        return profile_by_id("github");
    if (has_token(folded, "cloudmail_mcp") || has_token(folded, "cloudmail") ||
        has_token(folded, "email_mcp") || has_token(folded, "protonmail_bridge") ||
        has_token(folded, "proton_bridge") || has_token(folded, "protonmail") ||
        has_token(folded, "proton_mail"))
        return profile_by_id("cloudmail_mcp");
    if (has_token(folded, "heat_mcp") || has_token(folded, "asos") ||
        has_token(folded, "weather_regime") || has_token(folded, "portfolio_dashboard") ||
        has_token(folded, "manage_orders") || has_token(folded, "model_consensus"))
        return profile_by_id("heat_mcp");
    if (has_token(folded, "email_remote") || has_token(folded, "create_envelope") ||
        has_token(folded, "send_envelope"))
        return profile_by_id("email_remote");
    if (has_token(folded, "neon") || has_token(folded, "query_tuning") ||
        has_token(folded, "slow_queries"))
        return profile_by_id("neon");
    if (has_token(folded, "multi_agent") || has_token(folded, "spawn_agent") ||
        has_token(folded, "wait_agent") || has_token(folded, "resume_agent") ||
        has_token(folded, "close_agent") || has_token(folded, "send_input"))
        return profile_by_id("multi_agent");
    if (has_token(folded, "dsco_jina") || has_token(folded, "jina") ||
        has_token(folded, "web_search") || has_token(folded, "fetch_url") ||
        has_token(folded, "screenshot_url"))
        return profile_by_id("web_search");
    return NULL;
}

const dsco_integration_profile_t *dsco_integration_profiles(size_t *count) {
    if (count)
        *count = sizeof(g_profiles) / sizeof(g_profiles[0]);
    return g_profiles;
}

const dsco_integration_profile_t *dsco_integration_profile_for_server(const char *server_name) {
    char folded[512];
    fold_name(server_name, folded, sizeof(folded));
    return profile_for_folded(folded);
}

const dsco_integration_profile_t *dsco_integration_profile_for_tool(const char *tool_name) {
    char folded[512];
    fold_name(tool_name, folded, sizeof(folded));
    return profile_for_folded(folded);
}

static bool action_word(const char *folded, const char *word) {
    return has_token(folded, word);
}

unsigned dsco_integration_actions_for_tool(const char *tool_name) {
    char folded[512];
    fold_name(tool_name, folded, sizeof(folded));

    const dsco_integration_profile_t *profile = profile_for_folded(folded);
    unsigned actions = profile ? profile->default_actions : DSCO_INTEGRATION_ACTION_NONE;

    if (action_word(folded, "list") || action_word(folded, "read") ||
        action_word(folded, "search") || action_word(folded, "get") ||
        action_word(folded, "fetch") || action_word(folded, "extract") ||
        action_word(folded, "transcript") || action_word(folded, "availability") ||
        action_word(folded, "profile") || action_word(folded, "colors") ||
        action_word(folded, "dashboard") || action_word(folded, "classify") ||
        action_word(folded, "observations") || action_word(folded, "slow_queries"))
        actions |= DSCO_INTEGRATION_ACTION_READ;

    if (action_word(folded, "create") || action_word(folded, "update") ||
        action_word(folded, "modify") || action_word(folded, "bulk") ||
        action_word(folded, "apply") || action_word(folded, "archive") ||
        action_word(folded, "respond") || action_word(folded, "reply") ||
        action_word(folded, "add") || action_word(folded, "complete") ||
        action_word(folded, "manage") || action_word(folded, "prepare"))
        actions |= DSCO_INTEGRATION_ACTION_WRITE;

    if (action_word(folded, "send") || action_word(folded, "forward"))
        actions |= DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_SEND |
                   DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;

    if (action_word(folded, "delete") || action_word(folded, "trash") ||
        action_word(folded, "remove"))
        actions |= DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_DELETE |
                   DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;

    bool mutating_file =
        action_word(folded, "file") &&
        (actions & (DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_DELETE));
    if (action_word(folded, "branch") || action_word(folded, "repository") ||
        action_word(folded, "repo") || mutating_file || action_word(folded, "database") ||
        action_word(folded, "migration") || action_word(folded, "tuning") ||
        action_word(folded, "sql") || action_word(folded, "orders"))
        actions |= DSCO_INTEGRATION_ACTION_ADMIN;

    if (profile) {
        if (strcmp(profile->id, "gmail") == 0 || strcmp(profile->id, "botmail") == 0 ||
            strcmp(profile->id, "document_qa") == 0 || strcmp(profile->id, "video_qa") == 0 ||
            strcmp(profile->id, "web_search") == 0 || strcmp(profile->id, "github") == 0 ||
            strcmp(profile->id, "cloudmail_mcp") == 0)
            actions |= DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT;

        if (strcmp(profile->id, "google_calendar") == 0 &&
            (actions & (DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_DELETE)))
            actions |= DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;

        if ((strcmp(profile->id, "github") == 0 || strcmp(profile->id, "neon") == 0 ||
             strcmp(profile->id, "heat_mcp") == 0 || strcmp(profile->id, "cloudmail_mcp") == 0 ||
             strcmp(profile->id, "multi_agent") == 0) &&
            (actions & (DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_DELETE |
                        DSCO_INTEGRATION_ACTION_ADMIN)))
            actions |= DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;

        if (strcmp(profile->id, "botmail") == 0 && action_word(folded, "open_attachment"))
            actions |= DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;
    }

    if (actions & (DSCO_INTEGRATION_ACTION_SEND | DSCO_INTEGRATION_ACTION_DELETE |
                   DSCO_INTEGRATION_ACTION_ADMIN))
        actions |= DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;

    return actions;
}

bool dsco_integration_requires_confirmation(const char *tool_name) {
    return (dsco_integration_actions_for_tool(tool_name) &
            DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION) != 0;
}

bool dsco_integration_action_has(unsigned actions, dsco_integration_action_t action) {
    return (actions & (unsigned)action) != 0;
}

const char *dsco_integration_scope_name(dsco_integration_scope_t scope) {
    switch (scope) {
    case DSCO_INTEGRATION_SCOPE_PUBLIC_APP:
        return "public_app";
    case DSCO_INTEGRATION_SCOPE_PRIVATE_SERVICE:
        return "private_service";
    case DSCO_INTEGRATION_SCOPE_INFRASTRUCTURE:
        return "infrastructure";
    case DSCO_INTEGRATION_SCOPE_AGENTIC:
        return "agentic";
    case DSCO_INTEGRATION_SCOPE_UNKNOWN:
    default:
        return "unknown";
    }
}
