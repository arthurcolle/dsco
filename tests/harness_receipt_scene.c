/* Offline scene artifacts through the real native renderer. */
#include "pixel_tui.h"
#include "presentation_json.h"
#include "tui.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    if (mkdir(argv[1], 0755) && access(argv[1], F_OK))
        return 1;
    const char *edit = "{\"kind\":\"code_edit\",\"status\":\"written\",\"path\":\"src/"
                       "α.c\",\"before_sha256\":\"fixture-before\",\"after_sha256\":\"fixture-"
                       "after\",\"verified\":true,\"durable\":true,\"diff\":\"--- a/src/α.c\\n+++ "
                       "b/src/α.c\\n@@ -4 +4 @@\\n-return unknown;\\n+return measured;\\n\"}";
    const char *skill = "{\"kind\":\"skill\",\"action\":\"invoke\",\"name\":\"harness-"
                        "engineering\",\"path\":\".agents/skills/harness-engineering/"
                        "SKILL.md\",\"sha256\":\"fixture-content-digest\",\"instructions\":\"# "
                        "Harness engineering\\nRead the current snapshot. Verify the result. "
                        "Update evidence and priors.\"}";
    char *edit_view = presentation_json(edit, true), *skill_view = presentation_json(skill, false);
    if (!edit_view || !skill_view)
        return 1;
    pixel_tui_fixture_message_t messages[] = {
        {.role = "USER",
         .text = "Inspect the exact edit and selected skill. Preserve my Unicode draft."},
        {.role = "TOOL", .detail = "code_edit", .text = edit_view},
        {.role = "TOOL", .detail = "skill", .text = skill_view},
        {.role = "ASSISTANT",
         .text = "## Verified checkpoint\n\nThe version check passed and readback matched. The "
                 "previous skill revision is retained.\n\n- parent: complete\n- child: ready\n- "
                 "external usage: unknown"},
    };
    pixel_tui_fixture_t fixture = {
        .model = "offline fixture",
        .slot_name = "loopback proof",
        .state = PIXEL_TUI_IDLE,
        .messages = messages,
        .message_count = 4,
        .input = "Draft α · 雪 🦉",
        .input_cursor = strlen("Draft α · 雪 🦉"),
        .input_active = true,
        .turn = 3,
        .input_tokens = 30,
        .output_tokens = 15,
        .tools_used = 2,
        .cost_usd = 0,
        .context_percent = 2,
    };
    const int sizes[][2] = {{1120, 700}, {700, 600}, {1600, 900}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/native-%dx%d.ppm", argv[1], sizes[i][0], sizes[i][1]);
        pixel_tui_density_metrics_t metrics = {0};
        if (!pixel_tui_write_fixture_ppm(path, sizes[i][0], sizes[i][1], &fixture, &metrics))
            return 1;
        printf("scene %dx%d: %d visible messages, %zu visible characters\n", sizes[i][0],
               sizes[i][1], metrics.visible_messages, metrics.visible_chars);
        if (sizes[i][0] >= 1120 && metrics.visible_messages != 4)
            return 1;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/ansi.txt", argv[1]);
    FILE *out = fopen(path, "w");
    if (!out)
        return 1;
    fprintf(
        out,
        "USER\n%s\n\nTOOL code_edit\n%s\n\nTOOL skill\n%s\n\nASSISTANT\n%s\n\nDraft α · 雪 🦉\n",
        messages[0].text, edit_view, skill_view, messages[3].text);
    if (fclose(out))
        return 1;
    free(edit_view);
    free(skill_view);
    return 0;
}
