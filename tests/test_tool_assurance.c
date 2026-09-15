#include "tool_assurance.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void check(bool condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL %u: %s\n", checks, message);
        exit(1);
    }
}

static tool_assurance_t classify(const char *prompt, bool disabled) {
    tool_assurance_t state;
    tool_assurance_begin(&state, prompt, disabled);
    return state;
}

int main(void) {
    tool_assurance_t state = classify("gimme weather in DC", false);
    check(state.required, "literal user weather request requires live execution");
    check(!strcmp(state.preferred_tool, "weather"), "weather request selects weather capability");
    check(state.direct_preflight, "current weather with a location can run before the model");
    check(!strcmp(state.argument_name, "location"), "weather preflight uses location argument");
    check(!strcmp(state.argument_value, "Washington, DC, US"), "DC is normalized for geocoding");
    check(tool_assurance_should_hold_output(&state), "tool-required prose is held until grounded");
    check(tool_assurance_needs_required_choice(&state), "unresolved request forces tool choice");

    state = classify("What's the weather in Paris right now?", false);
    check(state.required && state.direct_preflight, "current international weather is preflightable");
    check(!strcmp(state.argument_value, "Paris"), "temporal suffix is removed from location");

    state = classify("Show me the forecast in DC tomorrow", false);
    check(state.required && !state.direct_preflight, "future forecast uses model-selected capability");

    check(!classify("What is weather?", false).required, "conceptual weather question stays tool-free");
    check(!classify("Explain the weather API design", false).required,
          "weather implementation discussion stays tool-free");
    check(classify("What is the latest NVDA stock price?", false).required,
          "changing market fact requires a tool");
    check(classify("Look up the current train schedule", false).required,
          "explicit lookup requires a tool");
    check(classify("Run the available tool for this calculation", false).required,
          "explicit tool instruction requires execution");
    check(!classify("Explain mutex ownership", false).required,
          "stable conceptual answer is not forced through a tool");

    state = classify("gimme weather in DC", true);
    check(state.disabled && !state.required, "explicit tool disable remains authoritative");
    check(!tool_assurance_should_hold_output(&state), "disabled tools do not engage assurance");

    state = classify("gimme weather in DC", false);
    tool_assurance_note_preflight(&state, true);
    check(state.attempted && state.satisfied, "successful harness preflight grounds the request");
    check(tool_assurance_observe(&state, 0, "It is 75 degrees and clear.") ==
              TOOL_ASSURANCE_ACCEPT,
          "clean final prose is accepted after concrete execution");

    state = classify("gimme weather in DC", false);
    tool_assurance_note_preflight(&state, false);
    check(state.attempted && !state.satisfied, "failed preflight records attempt without success");
    check(tool_assurance_observe(&state, 0, "I don't have access to tools.") ==
              TOOL_ASSURANCE_RETRY,
          "false capability refusal is withheld and retried");
    check(tool_assurance_needs_required_choice(&state), "retry still requires a tool call");
    check(tool_assurance_observe(&state, 1, NULL) == TOOL_ASSURANCE_TOOL_CALL,
          "provider tool call satisfies the runtime obligation");

    state = classify("check the live service status", false);
    check(tool_assurance_observe(&state, 0, "Here is an ungrounded guess") ==
              TOOL_ASSURANCE_RETRY,
          "plausible prose cannot replace required live evidence");
    check(tool_assurance_observe(&state, 0, "No tools are available") ==
              TOOL_ASSURANCE_RETRY,
          "second refusal receives the bounded final retry");
    check(tool_assurance_observe(&state, 0, "I cannot browse") == TOOL_ASSURANCE_BLOCK,
          "repeated adapter violation is blocked instead of shown");

    check(tool_assurance_response_claims_no_tools("I can't use tools in this environment."),
          "direct no-tool claim is recognized");
    check(tool_assurance_response_claims_no_tools("Please enable browsing access."),
          "tool-enablement question is recognized");
    check(!tool_assurance_response_claims_no_tools("Weather API error: HTTP 401"),
          "concrete execution error is not rewritten as missing tools");

    char retry[768];
    tool_assurance_retry_prompt(&state, retry, sizeof(retry));
    check(strstr(retry, "discover_tools") && strstr(retry, "invoke_tool"),
          "retry names the adapter-neutral discovery path");
    check(strstr(retry, "Do not discuss whether tools exist"),
          "retry prohibits another speculative capability discussion");

    printf("tool_assurance: %u checks passed\n", checks);
    return 0;
}
