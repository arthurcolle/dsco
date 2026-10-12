#include "transcript_view.h"
#include "presentation_json.h"
#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *transcript_view_snapshot(const conversation_t *conv) {
    jbuf_t text; jbuf_init(&text, 4096);
    if (!conv) return text.data;
    const size_t cap = 2U * 1024U * 1024U;
    for (int i = 0; i < conv->count; i++) {
        const message_t *message = &conv->msgs[i];
        for (int j = 0; j < message->content_count; j++) {
            const msg_content_t *part = &message->content[j];
            if (!part->type) continue;
            bool tool = !strcmp(part->type, "tool_use") || !strcmp(part->type, "tool_result");
            if (!tool && strcmp(part->type, "text")) continue; /* no hidden reasoning/media */
            const char *body = !strcmp(part->type, "tool_use") ? part->tool_input : part->text;
            if (!body) continue;
            if (text.len + strlen(body) + 256 > cap) {
                jbuf_append(&text, "\n[Transcript review limit reached; retained conversation is unchanged.]\n");
                goto done;
            }
            jbuf_appendf(&text, "\n── %s%s%s ──\n", tool ? "TOOL" : message->role == ROLE_USER ? "YOU" : "DSCO",
                         tool && part->tool_name ? " · " : "", tool && part->tool_name ? part->tool_name : "");
            char *pretty = tool ? presentation_json(body, true) : NULL;
            const char *shown = pretty ? pretty : body;
            size_t available = cap - text.len;
            size_t count = strlen(shown);
            if (count > available) {
                free(pretty);
                jbuf_append(&text, "[Formatted content exceeds review limit.]\n");
                goto done;
            }
            jbuf_append(&text, shown); jbuf_append(&text, "\n"); free(pretty);
        }
    }
done:
    for (char *p = text.data; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 32 && c != '\n' && c != '\t') || c == 127) *p = '?';
    }
    return text.data;
}

void transcript_view_show(void *context) {
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) return;
    char *text = transcript_view_snapshot(context);
    if (!text) return;
    size_t count = 1;
    for (char *p = text; *p; p++) if (*p == '\n') count++;
    const char **lines = calloc(count, sizeof(*lines));
    if (!lines) { free(text); return; }
    size_t n = 0; lines[n++] = text;
    for (char *p = text; *p; p++) if (*p == '\n') { *p = 0; lines[n++] = p+1; }
    /* Same terminal, temporary alternate screen; scrollback and composer survive. */
    fputs("\033[?1049h", stderr);
    tui_pager_t pager;
    tui_pager_init(&pager, lines, (int)n, "Transcript · Ctrl-T or q returns · / searches");
    tui_pager_run(&pager);
    fputs("\033[?1049l", stderr); fflush(stderr);
    free(lines); free(text);
}
