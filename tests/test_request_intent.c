#include "request_intent.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    const char *direct[] = {
        "What is a mutex?",
        "Explain the CAP theorem.",
        "Derive the quadratic formula.",
        "Translate hello to French.",
        "Summarize this text: bounded queues prevent overload.",
        "Reply with exactly READY.",
        "Return READY.",
        "Write a haiku about schedulers.",
    };
    const char *agentic[] = {
        "What is the weather in DC?",
        "Who is the current CEO?",
        "Why did the last command fail?",
        "How do our workers look right now?",
        "Explain this: https://example.invalid/report",
        "What are the errors in the attached log?",
        "Fix the parser and test it.",
        "Inspect /tmp/output.json.",
        "Use tool read_file and summarize the result.",
        "Proceed.",
    };
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); i++) {
        if (!request_intent_prefers_direct(direct[i]))
            fprintf(stderr, "unexpected agentic classification: %s\n", direct[i]);
        assert(request_intent_prefers_direct(direct[i]));
    }
    for (size_t i = 0; i < sizeof(agentic) / sizeof(*agentic); i++) {
        if (request_intent_prefers_direct(agentic[i]))
            fprintf(stderr, "unexpected direct classification: %s\n", agentic[i]);
        assert(!request_intent_prefers_direct(agentic[i]));
    }
    assert(!request_intent_prefers_direct(NULL));
    assert(!request_intent_prefers_direct(""));
    printf("request intent: %zu direct + %zu agentic cases passed\n",
           sizeof(direct) / sizeof(*direct), sizeof(agentic) / sizeof(*agentic));
    return 0;
}
