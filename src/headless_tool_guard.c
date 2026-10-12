#include "headless_tool_guard.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void headless_tool_guard_begin(headless_tool_guard_t *g, const char *prompt,
                              bool tools_disabled, bool require_execution) {
    memset(g, 0, sizeof(*g));
    g->forced = require_execution;
    g->runtime_disabled = tools_disabled;
    /* Headless callers have no human recovery turn. Permit two bounded repair
     * attempts before blocking, matching the guard's retry contract. */
    tool_assurance_begin_with_limit(&g->assurance, prompt, tools_disabled, 2);
    if (require_execution && !tools_disabled) {
        g->assurance.required = true;
        snprintf(g->assurance.reason, sizeof(g->assurance.reason),
                 "explicit_execution_contract");
    }
}

bool headless_tool_guard_hold(const headless_tool_guard_t *g) {
    return g && (g->forced || g->assurance.required || g->assurance.disabled);
}

void headless_tool_guard_prepare(headless_tool_guard_t *g, char *choice, size_t cap) {
    if (!g || !choice || !cap) return;
    g->assurance.disabled = g->runtime_disabled || strcmp(choice, "none") == 0;
    bool needed = headless_tool_guard_hold(g) && !g->assurance.disabled &&
                  !g->assurance.satisfied;
    if (needed && (!choice[0] || strcmp(choice, "auto") == 0)) {
        snprintf(g->saved_choice, sizeof(g->saved_choice), "%s", choice);
        snprintf(choice, cap, "any");
        g->owns_choice = true;
    } else if (!needed && g->owns_choice) {
        /* A later operator restriction takes precedence over our saved choice. */
        if (strcmp(choice, "any") == 0)
            snprintf(choice, cap, "%s", g->saved_choice);
        g->owns_choice = false;
    }
}

bool headless_tool_guard_has_markup(const char *text) {
    if (!text) return false;
    size_t fence_width = 0, inline_width = 0;
    char fence_char = 0;
    bool line_start = true, quote = false;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            line_start = true; quote = false; p++; continue;
        }
        if (line_start && (*p == ' ' || *p == '\t')) { p++; continue; }
        if (line_start && *p == '>') quote = true;
        if (line_start && (*p == '`' || *p == '~')) {
            size_t n = 0;
            while (p[n] == *p) n++;
            if (n >= 3 && !inline_width && !quote) {
                if (!fence_width) { fence_width = n; fence_char = *p; }
                else if (*p == fence_char && n >= fence_width) fence_width = 0;
                while (*p && *p != '\n') p++;
                continue;
            }
        }
        line_start = false;
        if (fence_width || quote) { p++; continue; }
        if (*p == '`') {
            size_t n = 0;
            while (p[n] == '`') n++;
            if (!inline_width) inline_width = n;
            else if (inline_width == n) inline_width = 0;
            p += n; continue;
        }
        if (!inline_width &&
            (strncmp(p, "<function=", 10) == 0 ||
             strncmp(p, "<tool_call>", 11) == 0 ||
             strncmp(p, "</tool_call>", 12) == 0)) return true;
        p++;
    }
    return false;
}

void headless_tool_guard_note_result(headless_tool_guard_t *g, bool success, bool denied) {
    if (!g) return;
    g->assurance.attempted = true;
    if (success) {
        g->assurance.satisfied = true;
        g->consecutive_failures = 0;
    } else {
        g->failed_calls++;
        if (g->consecutive_failures < 3) g->consecutive_failures++;
    }
    g->denied |= denied;
}

void headless_tool_guard_note_execution(headless_tool_guard_t *g, const char *name,
                                       const char *input, const char *result,
                                       bool success, bool denied) {
    if (!g) return;
    headless_tool_guard_note_result(g, success, denied);
    /* Identical output from a write/exec can still have real side effects.
     * Restrict repetition detection to known observational tools. */
    bool observational = name && (!strcmp(name, "read_file") || !strcmp(name, "Read") ||
        !strcmp(name, "list_directory") || !strcmp(name, "grep_files") ||
        !strcmp(name, "Grep") || !strcmp(name, "find_files") || !strcmp(name, "Glob"));
    if (!observational) { g->repeated_results = 0; return; }
    uint64_t signature = UINT64_C(14695981039346656037);
    const char *parts[] = {name, input, result};
    for (size_t i = 0; i < 3; i++) {
        const unsigned char *p = (const unsigned char *)(parts[i] ? parts[i] : "");
        while (*p) { signature ^= *p++; signature *= UINT64_C(1099511628211); }
        signature ^= 0xff; signature *= UINT64_C(1099511628211);
    }
    if (g->repeated_results && signature == g->last_result_signature) {
        if (g->repeated_results < 8) g->repeated_results++;
    } else g->repeated_results = 1;
    g->last_result_signature = signature;
}

const char *headless_tool_guard_stop_reason(const headless_tool_guard_t *g) {
    if (!g) return NULL;
    if (g->consecutive_failures >= 3)
        return "No progress: three consecutive tool failures; stopping before another retry. Inspect the tool errors and resume with a corrected action.";
    if (g->repeated_results >= 8)
        return "No progress: eight identical read-only tool calls returned identical results; stopping the repetition loop. Inspect the last result before resuming.";
    return NULL;
}

static tool_assurance_decision_t blocked(char *out, size_t cap, const char *reason) {
    if (out && cap) snprintf(out, cap, "%s", reason);
    return TOOL_ASSURANCE_BLOCK;
}

tool_assurance_decision_t headless_tool_guard_observe(
    headless_tool_guard_t *g, const parsed_response_t *response,
    char *repair, size_t repair_cap) {
    if (repair && repair_cap) repair[0] = '\0';
    if (!g || !response || response->count < 0 ||
        (response->count && !response->blocks))
        return blocked(repair, repair_cap, "Invalid tool-response state.");
    if (g->denied)
        return blocked(repair, repair_cap, "A tool was denied; no alternate execution is permitted.");
    const char *stop_reason = headless_tool_guard_stop_reason(g);
    if (stop_reason) return blocked(repair, repair_cap, stop_reason);
    int calls = 0;
    size_t text_size = 0;
    for (int i = 0; i < response->count; i++) {
        const content_block_t *b = &response->blocks[i];
        if (b->type && strcmp(b->type, "tool_use") == 0) {
            if (!b->tool_name || !b->tool_name[0] || !b->tool_id || !b->tool_id[0] ||
                !b->tool_input)
                return blocked(repair, repair_cap, "Malformed structured tool request.");
            calls++;
        }
        if (b->type && strcmp(b->type, "text") == 0 && b->text) {
            size_t n = strlen(b->text);
            if (n > SIZE_MAX - text_size - 1)
                return blocked(repair, repair_cap, "Tool response text is too large.");
            text_size += n;
        }
    }
    if (g->assurance.disabled && (g->forced || calls))
        return blocked(repair, repair_cap, "Tools are explicitly disabled; execution is blocked.");
    char *text = malloc(text_size + 1);
    if (!text) return blocked(repair, repair_cap, "Cannot validate tool response text.");
    size_t offset = 0;
    for (int i = 0; i < response->count; i++) {
        const content_block_t *b = &response->blocks[i];
        if (b->type && strcmp(b->type, "text") == 0 && b->text) {
            size_t n = strlen(b->text); memcpy(text + offset, b->text, n); offset += n;
        }
    }
    text[offset] = '\0';
    /* Raw markup is never converted to execution. Reject mixed malformed and
     * structured batches before dispatch rather than laundering a bad action. */
    bool malformed = (g->forced || g->assurance.required) &&
                     headless_tool_guard_has_markup(text);
    if (malformed) g->assurance.satisfied = false;
    if (!calls && !malformed && !g->assurance.satisfied && g->failed_calls) {
        free(text);
        return blocked(repair, repair_cap, "No tool succeeded; execution was not verified.");
    }
    bool was_satisfied = g->assurance.satisfied;
    tool_assurance_decision_t d = tool_assurance_observe(
        &g->assurance, malformed ? 0 : calls, text);
    free(text);
    if (d == TOOL_ASSURANCE_TOOL_CALL) {
        /* An API request is not proof that the gated dispatcher ran it. */
        g->assurance.satisfied = was_satisfied;
        return TOOL_ASSURANCE_ACCEPT;
    }
    if (d == TOOL_ASSURANCE_RETRY) {
        if (malformed && repair && repair_cap) {
            snprintf(repair, repair_cap,
                     "Your last response contained unexecuted tool markup in ordinary text. "
                     "No tool ran from that response. Retry the intended action using a complete "
                     "native tool call and the registered schema, including the tool-call opening "
                     "marker. Do not emit a bare <function=...> block or claim its result. "
                     "Do not work around a permission denial.");
        } else tool_assurance_retry_prompt(&g->assurance, repair, repair_cap);
    } else if (d == TOOL_ASSURANCE_BLOCK) {
        return blocked(repair, repair_cap,
                       "Required tool execution was not verified after bounded repair attempts.");
    }
    return d;
}

void headless_tool_guard_emit(const parsed_response_t *response,
                              void (*emit)(const char *, void *), void *ctx) {
    if (!response || !emit) return;
    for (int i = 0; i < response->count; i++) {
        const content_block_t *b = &response->blocks[i];
        if (b->type && strcmp(b->type, "text") == 0 && b->text)
            emit(b->text, ctx);
    }
}
