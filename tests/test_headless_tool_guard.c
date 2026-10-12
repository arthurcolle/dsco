#include "headless_tool_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static parsed_response_t response(content_block_t *block) {
    parsed_response_t r = {.blocks = block, .count = 1, .stop_reason = "end_turn"};
    return r;
}
static void emitted(const char *text, void *ctx) { strcat((char *)ctx, text); }
int main(void) {
    headless_tool_guard_t g;
    char repair[768], choice[64] = "auto";
    content_block_t text = {.type="text", .text="Done; everything passed."};
    parsed_response_t r = response(&text);
    headless_tool_guard_begin(&g, "Fix parser.py", false, true);
    CHECK(headless_tool_guard_hold(&g));
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "any") == 0);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_RETRY);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_RETRY);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    CHECK(strstr(repair, "not verified") != NULL);

    headless_tool_guard_begin(&g, "Fix parser.py", false, true);
    snprintf(choice, sizeof(choice), "auto");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    content_block_t tool = {.type="tool_use", .tool_name="read_file", .tool_id="t1", .tool_input="{}"};
    r = response(&tool);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    CHECK(!g.assurance.satisfied);
    headless_tool_guard_note_result(&g, true, false);
    CHECK(g.assurance.satisfied);
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "auto") == 0);
    text.text = "I will edit it.\n<function=write_file>\n<parameter=path>a.py</parameter>\n</function>\n</tool_call>";
    r = response(&text);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_RETRY);
    CHECK(!g.assurance.satisfied);
    CHECK(strstr(repair, "No tool ran") != NULL);
    r = response(&tool);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    headless_tool_guard_note_result(&g, true, false);
    text.text = "The requested checks passed."; r = response(&text);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);

    CHECK(headless_tool_guard_has_markup("<tool_call>"));
    CHECK(headless_tool_guard_has_markup("  <function=bash>"));
    CHECK(!headless_tool_guard_has_markup("An inline `<function=bash>` example."));
    CHECK(!headless_tool_guard_has_markup("```xml\n<function=bash>\n</tool_call>\n```"));
    CHECK(!headless_tool_guard_has_markup("> <tool_call>\n> quotation"));
    CHECK(headless_tool_guard_has_markup("```xml\n<function=x>\n```\n<function=bash>"));

    headless_tool_guard_begin(&g, "What is a closure?", false, false);
    CHECK(!headless_tool_guard_hold(&g));
    text.text = "A closure captures its lexical environment."; r = response(&text);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    headless_tool_guard_begin(&g, "Fix code", true, true);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    snprintf(choice, sizeof(choice), "none");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "none") == 0);
    headless_tool_guard_begin(&g, "Fix code", false, true);
    snprintf(choice, sizeof(choice), "tool:read_file");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "tool:read_file") == 0);

    CHECK(headless_tool_guard_has_markup("Applying: <function=write_file>x</function></tool_call>"));
    CHECK(!headless_tool_guard_has_markup("````xml\n```\n<function=bash>\n```\n````"));
    content_block_t split[] = {{.type="text", .text="```xml\n"},
                              {.type="text", .text="<function=bash>\n</tool_call>\n```"}};
    parsed_response_t multi = {.blocks=split, .count=2};
    headless_tool_guard_begin(&g, "Fix code", false, true);
    headless_tool_guard_note_result(&g, true, false);
    CHECK(headless_tool_guard_observe(&g, &multi, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    split[0].text = "<func"; split[1].text = "tion=bash>x</function></tool_call>";
    CHECK(headless_tool_guard_observe(&g, &multi, repair, sizeof(repair)) == TOOL_ASSURANCE_RETRY);
    content_block_t mixed[] = {tool, {.type="text", .text="<function=write_file>x</function></tool_call>"}};
    multi.blocks = mixed;
    headless_tool_guard_begin(&g, "Fix code", false, true);
    CHECK(headless_tool_guard_observe(&g, &multi, repair, sizeof(repair)) == TOOL_ASSURANCE_RETRY);
    CHECK(!g.assurance.satisfied);

    headless_tool_guard_begin(&g, "Fix code", false, true);
    r = response(&tool);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    CHECK(!g.assurance.satisfied);
    headless_tool_guard_note_result(&g, false, true);
    text.text = "Done; everything passed."; r = response(&text);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    CHECK(g.assurance.retries == 0);
    headless_tool_guard_begin(&g, "Fix code", false, true);
    headless_tool_guard_note_result(&g, false, false);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    headless_tool_guard_begin(&g, "Explain this", true, false);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_ACCEPT);
    r = response(&tool);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);

    headless_tool_guard_begin(&g, "Fix code", false, true);
    snprintf(choice, sizeof(choice), "auto");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    headless_tool_guard_note_result(&g, true, false);
    snprintf(choice, sizeof(choice), "none");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "none") == 0);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    headless_tool_guard_begin(&g, "Fix code", false, true);
    choice[0] = '\0'; headless_tool_guard_prepare(&g, choice, sizeof(choice));
    headless_tool_guard_note_result(&g, true, false);
    snprintf(choice, sizeof(choice), "tool:read_file");
    headless_tool_guard_prepare(&g, choice, sizeof(choice));
    CHECK(strcmp(choice, "tool:read_file") == 0);
    headless_tool_guard_begin(&g, "Fix code", false, true);
    choice[0] = '\0'; headless_tool_guard_prepare(&g, choice, sizeof(choice));
    headless_tool_guard_note_result(&g, true, false);
    headless_tool_guard_prepare(&g, choice, sizeof(choice)); CHECK(!choice[0]);
    CHECK(headless_tool_guard_observe(&g, NULL, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    r = response(&text);

    /* Earlier success must not immunize an endlessly failing worker. */
    headless_tool_guard_begin(&g, "Fix code", false, true);
    headless_tool_guard_note_result(&g, true, false);
    for (int i = 0; i < 3; i++) headless_tool_guard_note_result(&g, false, false);
    CHECK(headless_tool_guard_stop_reason(&g) != NULL);
    CHECK(headless_tool_guard_observe(&g, &r, repair, sizeof(repair)) == TOOL_ASSURANCE_BLOCK);
    headless_tool_guard_begin(&g, "Inspect code", false, true);
    for (int i = 0; i < 7; i++)
        headless_tool_guard_note_execution(&g, "read_file", "same", "unchanged", true, false);
    CHECK(headless_tool_guard_stop_reason(&g) == NULL);
    headless_tool_guard_note_execution(&g, "read_file", "same", "unchanged", true, false);
    CHECK(headless_tool_guard_stop_reason(&g) != NULL);
    headless_tool_guard_note_execution(&g, "read_file", "same", "changed", true, false);
    CHECK(headless_tool_guard_stop_reason(&g) == NULL);
    headless_tool_guard_note_result(&g, false, false);
    headless_tool_guard_note_result(&g, false, false);
    headless_tool_guard_note_result(&g, true, false);
    CHECK(g.consecutive_failures == 0);
    for (int i = 0; i < 20; i++)
        headless_tool_guard_note_execution(&g, "bash", "append a receipt", "", true, false);
    CHECK(headless_tool_guard_stop_reason(&g) == NULL);

    char output[128] = "";
    headless_tool_guard_emit(&r, emitted, output);
    CHECK(strcmp(output, text.text) == 0);
    printf("PASS: %d headless tool guard checks\n", checks);
    return 0;
}
