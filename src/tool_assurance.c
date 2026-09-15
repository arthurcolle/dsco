#include "tool_assurance.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { ASSURANCE_TEXT_MAX = 4096 };

static void lower_copy(const char *src, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t used = 0;
    for (; src && src[used] && used + 1 < out_len; used++) {
        unsigned char c = (unsigned char)src[used];
        out[used] = c < 128 ? (char)tolower(c) : (char)c;
    }
    out[used] = '\0';
}

static bool word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static bool has_word(const char *text, const char *word) {
    if (!text || !word || !word[0])
        return false;
    size_t n = strlen(word);
    for (const char *p = text; (p = strstr(p, word)) != NULL; p++) {
        unsigned char before = p == text ? 0 : (unsigned char)p[-1];
        unsigned char after = (unsigned char)p[n];
        if (!word_char(before) && !word_char(after))
            return true;
    }
    return false;
}

static bool has_any_word(const char *text, const char *const *words) {
    for (size_t i = 0; words[i]; i++)
        if (has_word(text, words[i]))
            return true;
    return false;
}

static const char *find_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0])
        return NULL;
    size_t n = strlen(needle);
    for (const char *p = text; *p; p++)
        if (strncasecmp(p, needle, n) == 0)
            return p;
    return NULL;
}

static void trim_location(char *value) {
    if (!value || !value[0])
        return;
    char *start = value;
    while (*start && (isspace((unsigned char)*start) || *start == ':' || *start == '-'))
        start++;
    if (start != value)
        memmove(value, start, strlen(start) + 1);

    char *cut = strpbrk(value, "?\n\r;!");
    if (cut)
        *cut = '\0';
    size_t n = strlen(value);
    while (n > 0 && (isspace((unsigned char)value[n - 1]) || value[n - 1] == '.' ||
                     value[n - 1] == ',' || value[n - 1] == '"' || value[n - 1] == '\''))
        value[--n] = '\0';

    static const char *const suffixes[] = {
        " right now", " currently", " today", " tonight", " now", " please", NULL,
    };
    for (size_t i = 0; suffixes[i]; i++) {
        size_t suffix_len = strlen(suffixes[i]);
        n = strlen(value);
        if (n > suffix_len && strcasecmp(value + n - suffix_len, suffixes[i]) == 0) {
            value[n - suffix_len] = '\0';
            break;
        }
    }
    n = strlen(value);
    while (n > 0 && isspace((unsigned char)value[n - 1]))
        value[--n] = '\0';

    if (strcasecmp(value, "dc") == 0 || strcasecmp(value, "d.c") == 0 ||
        strcasecmp(value, "d.c.") == 0 || strcasecmp(value, "washington dc") == 0 ||
        strcasecmp(value, "washington d.c") == 0 ||
        strcasecmp(value, "washington d.c.") == 0)
        snprintf(value, 192, "%s", "Washington, DC, US");
}

static void extract_weather_location(const char *prompt, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    static const char *const markers[] = {
        " weather in ", " weather for ", " weather at ", " temperature in ",
        " temperature for ", " conditions in ", " conditions for ", NULL,
    };
    const char *location = NULL;
    size_t marker_len = 0;
    for (size_t i = 0; markers[i]; i++) {
        const char *hit = find_ci(prompt, markers[i]);
        if (hit) {
            location = hit + strlen(markers[i]);
            marker_len = strlen(markers[i]);
            break;
        }
    }
    (void)marker_len;
    if (!location)
        return;
    while (strncasecmp(location, "the ", 4) == 0)
        location += 4;
    snprintf(out, out_len, "%.*s", (int)(out_len - 1), location);
    trim_location(out);
}

static bool conceptual_weather_question(const char *text) {
    bool conceptual = strstr(text, "what is weather") || strstr(text, "define weather") ||
                      strstr(text, "explain weather") || strstr(text, "weather api") ||
                      strstr(text, "weather tool") || strstr(text, "weather schema") ||
                      strstr(text, "weather code") || strstr(text, "weather app");
    bool concrete = strstr(text, " weather in ") || strstr(text, " weather for ") ||
                    has_word(text, "current") || has_word(text, "today") ||
                    has_word(text, "tonight") || has_word(text, "tomorrow") ||
                    has_word(text, "forecast");
    return conceptual && !concrete;
}

void tool_assurance_begin(tool_assurance_t *state, const char *prompt,
                          bool tools_explicitly_disabled) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->disabled = tools_explicitly_disabled;
    state->max_retries = 2;
    if (tools_explicitly_disabled || !prompt || !prompt[0])
        return;

    char text[ASSURANCE_TEXT_MAX];
    lower_copy(prompt, text, sizeof(text));
    static const char *const weather_words[] = {
        "weather", "forecast", "temperature", "conditions", "humidity", "precipitation", NULL,
    };
    static const char *const request_words[] = {
        "gimme", "give", "get", "check", "show", "tell", "find", "lookup", "look", "what", "how", NULL,
    };
    bool weather = has_any_word(text, weather_words);
    bool place_or_time = strstr(text, " in ") || strstr(text, " for ") ||
                         strstr(text, " at ") || has_word(text, "current") ||
                         has_word(text, "today") || has_word(text, "tonight") ||
                         has_word(text, "tomorrow") || has_word(text, "now");
    if (weather && !conceptual_weather_question(text) &&
        (place_or_time || has_any_word(text, request_words))) {
        state->required = true;
        snprintf(state->preferred_tool, sizeof(state->preferred_tool), "%s", "weather");
        snprintf(state->reason, sizeof(state->reason), "%s", "live weather request");
        extract_weather_location(prompt, state->argument_value, sizeof(state->argument_value));
        if (state->argument_value[0] && !has_word(text, "tomorrow") &&
            !has_word(text, "forecast") && !has_word(text, "weekend") &&
            !has_word(text, "next")) {
            snprintf(state->argument_name, sizeof(state->argument_name), "%s", "location");
            state->direct_preflight = true;
        }
        return;
    }

    static const char *const live_words[] = {
        "current", "latest", "live", "realtime", "real-time", "today", "now", NULL,
    };
    static const char *const live_subjects[] = {
        "price", "quote", "stock", "crypto", "score", "schedule", "news", "traffic",
        "flight", "balance", "status", "availability", "rate", NULL,
    };
    bool live_fact = has_any_word(text, live_words) && has_any_word(text, live_subjects);
    bool web_lookup = strstr(text, "look up ") || strstr(text, "search for ") ||
                      strstr(text, "search the web") || strstr(text, "check online") ||
                      strstr(text, "browse for ") || strstr(text, "fetch from ");
    bool explicit_tool = (has_word(text, "tool") || has_word(text, "command") ||
                          has_word(text, "script")) &&
                         (has_word(text, "use") || has_word(text, "call") ||
                          has_word(text, "run") || has_word(text, "invoke") ||
                          has_word(text, "execute"));
    if (live_fact || web_lookup || explicit_tool) {
        state->required = true;
        snprintf(state->reason, sizeof(state->reason), "%s",
                 live_fact ? "changing external fact" :
                 (web_lookup ? "explicit external lookup" : "explicit tool execution"));
    }
}

bool tool_assurance_should_hold_output(const tool_assurance_t *state) {
    return state && state->required && !state->disabled;
}

bool tool_assurance_needs_required_choice(const tool_assurance_t *state) {
    return state && state->required && !state->disabled && !state->satisfied;
}

void tool_assurance_note_preflight(tool_assurance_t *state, bool succeeded) {
    if (!state)
        return;
    state->attempted = true;
    state->satisfied = succeeded;
}

bool tool_assurance_response_claims_no_tools(const char *text) {
    if (!text || !text[0])
        return false;
    char lower[ASSURANCE_TEXT_MAX];
    lower_copy(text, lower, sizeof(lower));
    static const char *const direct[] = {
        "no tools are available", "no tools available", "tools are not available",
        "tools aren't available", "tool definitions were not provided", "cannot use tools",
        "can't use tools", "unable to use tools", "do not have tools", "don't have tools",
        "cannot browse", "can't browse", "unable to browse", NULL,
    };
    for (size_t i = 0; direct[i]; i++)
        if (strstr(lower, direct[i]))
            return true;

    bool access_denial = strstr(lower, "do not have access") ||
                         strstr(lower, "don't have access") ||
                         strstr(lower, "cannot access") || strstr(lower, "can't access") ||
                         strstr(lower, "unable to access");
    bool capability = has_word(lower, "tool") || has_word(lower, "tools") ||
                      has_word(lower, "internet") || has_word(lower, "web") ||
                      strstr(lower, "live data") || strstr(lower, "real-time data") ||
                      strstr(lower, "realtime data");
    if (access_denial && capability)
        return true;
    return (strstr(lower, "enable") || strstr(lower, "provide")) &&
           (strstr(lower, "tool access") || strstr(lower, "browsing access"));
}

tool_assurance_decision_t tool_assurance_observe(tool_assurance_t *state,
                                                 int tool_call_count,
                                                 const char *response_text) {
    if (!state || !state->required || state->disabled)
        return TOOL_ASSURANCE_ACCEPT;
    if (tool_call_count > 0) {
        state->attempted = true;
        state->satisfied = true;
        return TOOL_ASSURANCE_TOOL_CALL;
    }

    bool refusal = tool_assurance_response_claims_no_tools(response_text);
    if (!refusal && state->satisfied)
        return TOOL_ASSURANCE_ACCEPT;
    if (state->retries < state->max_retries) {
        state->retries++;
        return TOOL_ASSURANCE_RETRY;
    }
    return TOOL_ASSURANCE_BLOCK;
}

void tool_assurance_retry_prompt(const tool_assurance_t *state, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    snprintf(out, out_len,
             "[DSCO capability assurance retry %u/%u]\n"
             "The prior response made no usable tool call and was withheld. Execute the request "
             "through the live attached capabilities now. If the direct capability is absent, "
             "call discover_tools and then invoke_tool using the returned schema. Do not discuss "
             "whether tools exist. Only a concrete tool result or policy error establishes a "
             "runtime boundary.%s%s",
             state ? state->retries : 0, state ? state->max_retries : 0,
             state && state->preferred_tool[0] ? " Preferred capability: " : "",
             state && state->preferred_tool[0] ? state->preferred_tool : "");
}
