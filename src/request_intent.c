#include "request_intent.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool is_word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static bool contains_marker_ci(const char *text, const char *needle) {
    if (!text || !needle || !needle[0])
        return false;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_len) {
            bool left_ok = !is_word_char((unsigned char)needle[0]) || p == text ||
                           !is_word_char((unsigned char)p[-1]);
            bool right_ok = !is_word_char((unsigned char)needle[needle_len - 1]) ||
                            !is_word_char((unsigned char)p[needle_len]);
            if (left_ok && right_ok)
                return true;
        }
    }
    return false;
}

static bool contains_any(const char *prompt, const char *const *markers) {
    for (size_t i = 0; markers[i]; i++)
        if (contains_marker_ci(prompt, markers[i]))
            return true;
    return false;
}

bool request_intent_prefers_direct(const char *prompt) {
    if (!prompt || !prompt[0])
        return false;

    /* Tool, filesystem, code, data, network, and side-effect requests always
     * retain the governed execution surface. These checks precede all direct
     * markers so a mixed request can never lose its tools. */
    static const char *const action_markers[] = {
        "fix ", "fix the", "patch ", "edit ", "modify ", "change ",
        "write file", "write to ", "create file", "delete ", "remove ",
        "rename ", "move ", "copy ", "repo", "codebase", "workspace",
        "run ", "test ", "compile", "build ", "benchmark", "profile ",
        "grep", "search ", "look up", "inspect", "debug", "trace ",
        "logs", " log ", "csv", "json file", "database", "sqlite",
        "git ", "commit", "deploy", "install", "download", "scrape",
        "browse", "open ", "read file", "read the file", "execute",
        "shell", "terminal", "command", "function call", "tool call",
        "use tool", "call tool", "api ", "endpoint", "curl ", "ssh ",
        NULL
    };
    if (contains_any(prompt, action_markers))
        return false;

    /* External, changing, or conversation-relative facts need evidence even
     * when the request is phrased as a conceptual question. */
    static const char *const context_markers[] = {
        "://", "www.", "~/", "./", "/users/", "/tmp/", "attached",
        "uploaded", "screenshot", "this file", "that file", "document",
        "last command", "previous command", "earlier response", "above",
        "my account", "my balance", "our balance", "our workers",
        "our fleet", "our nodes", "current ", "currently", "latest",
        "live ", "right now", "real-time", "realtime", "today",
        "tomorrow", "yesterday", "weather in", "weather for", "forecast",
        "price of", "stock price", "news", "schedule", "status of",
        NULL
    };
    if (contains_any(prompt, context_markers))
        return false;

    /* These forms are self-contained model work. The set is intentionally
     * explicit; unknown prompts stay on the agentic path. */
    static const char *const direct_markers[] = {
        "what is", "what are", "who is", "explain", "define", "why is",
        "why does", "how does", "compare", "summarize", "translate",
        "derive", "calculate", "solve ", "prove ", "intuition",
        "overview", "teach me", "mathematical theory", "math theory",
        "model how", "give me theory", "reply with", "respond with",
        "return exactly", "return ready", "answer with", "say exactly",
        "write a poem", "write a haiku", "draft a", "compose a",
        NULL
    };
    return contains_any(prompt, direct_markers);
}
