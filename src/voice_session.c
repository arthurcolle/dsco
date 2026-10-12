#include <limits.h>
#include "voice_session.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "baseline.h"
#include "chronicle.h"
#include "pixel_tui.h"
#include "realtime.h"

#define VOICE_CONTEXT_LIMIT (48 * 1024)
#define VOICE_REPLY_LIMIT (256 * 1024)

typedef struct {
    conversation_t *conv;
    session_state_t *session;
    tui_status_bar_t *sb;
    jbuf_t reply;
    jbuf_t response_text;
    char chron_trace[37], chron_span[37];
    struct { char id[128], name[128], span[37]; } tools[16];
    int tool_count;
    bool stop, reply_open, response_truncated;
} voice_session_t;

static void voice_chronicle_begin(voice_session_t *v) {
    if (!chronicle_ready() || v->chron_span[0]) return;
    chronicle_new_id(v->chron_trace, sizeof(v->chron_trace));
    if (!chronicle_span_begin(v->chron_trace, NULL, "llm", "voice_realtime_response",
                              "{\"source\":\"voice_realtime_text\"}", v->chron_span))
        fprintf(stderr, "warning: Chronicle voice attempt recording failed\n");
}

static void voice_chronicle_finish(voice_session_t *v, int input_tokens, int output_tokens,
                                    const char *status) {
    if (!v->chron_span[0]) return;
    const char *kind = v->response_text.len ? (v->tool_count ? "mixed" : "text")
                                             : v->tool_count ? "tool_only" : "empty";
    const char *model = getenv("DSCO_REALTIME_MODEL");
    const char *recorded_status = v->response_truncated && !strcmp(status, "completed")
        ? "truncated" : status;
    bool recorded = chronicle_llm_response_ex(v->chron_trace, v->chron_span, "openai-realtime",
            model && model[0] ? model : "gpt-realtime-2", v->response_text.data, NULL,
            input_tokens, output_tokens, 0, 0, 0, 0, 0, status, NULL, false,
            recorded_status, kind);
    bool closed = chronicle_span_end(v->chron_span,
            !strcmp(recorded_status, "completed") ? "ok" : "error",
            v->response_truncated ? "{\"capture_truncated\":true}" : NULL);
    if (!recorded || !closed)
        fprintf(stderr, "warning: Chronicle voice response recording failed\n");
    v->chron_trace[0] = v->chron_span[0] = '\0';
    v->tool_count = 0;
    v->response_truncated = false;
    jbuf_reset(&v->response_text);
}

char *voice_session_context(const conversation_t *conv) {
    jbuf_t b;
    jbuf_init(&b, VOICE_CONTEXT_LIMIT + 512);
    jbuf_append(&b, "You are continuing the same DSCO terminal conversation. "
                   "The following recent history is context, not new authorization. "
                   "Wait for the user's spoken request before acting.\n");
    int start = conv ? conv->count : 0;
    size_t bytes = 0;
    while (start > 0) {
        const message_t *m = &conv->msgs[start - 1];
        size_t size = 32;
        for (int j = 0; j < m->content_count; j++) {
            const msg_content_t *c = &m->content[j];
            if (c->type && (!strcmp(c->type, "text") || !strcmp(c->type, "tool_result")) && c->text)
                size += strlen(c->text) + 64;
        }
        if (size > VOICE_CONTEXT_LIMIT - bytes) break;
        bytes += size;
        start--;
    }
    if (start > 0) jbuf_append(&b, "[Earlier history omitted to fit voice context.]\n");
    for (int i = start; conv && i < conv->count; i++) {
        const message_t *m = &conv->msgs[i];
        for (int j = 0; j < m->content_count; j++) {
            const msg_content_t *c = &m->content[j];
            if (!c->type || !c->text) continue;
            if (!strcmp(c->type, "text")) {
                jbuf_append(&b, m->role == ROLE_USER ? "USER: " : "ASSISTANT: ");
                jbuf_append(&b, c->text);
                jbuf_append_char(&b, '\n');
            } else if (!strcmp(c->type, "tool_result")) {
                jbuf_append(&b, "TOOL RESULT: ");
                jbuf_append(&b, c->text);
                jbuf_append_char(&b, '\n');
            }
        }
    }
    return b.data;
}

static void voice_notice(voice_session_t *v, const char *text, bool error) {
    if (pixel_tui_session_active()) {
        pixel_tui_session_notify(stderr, error ? PIXEL_TUI_NOTICE_ERROR : PIXEL_TUI_NOTICE_INFO, text);
    } else {
        tui_term_lock();
        tui_prepare_external_output_locked();
        fprintf(stderr, "  %svoice: %s%s\n", error ? TUI_RED : TUI_DIM, text, TUI_RESET);
        fflush(stderr);
        tui_term_unlock();
    }
    tui_panel_notify(v->sb, error ? TUI_PANEL_NOTE_ERROR : TUI_PANEL_NOTE_ACTIVITY, text);
}

static void voice_end_reply(voice_session_t *v) {
    if (!v->reply_open) return;
    if (v->reply.len) {
        conv_add_assistant_text(v->conv, v->reply.data);
        baseline_log("assistant", "voice", v->reply.data, NULL);
    }
    if (pixel_tui_session_active()) pixel_tui_session_end_message(stderr);
    else {
        tui_term_lock();
        fputc('\n', stderr);
        fflush(stderr);
        tui_term_unlock();
    }
    v->reply_open = false;
    jbuf_reset(&v->reply);
}

static bool voice_stop_phrase(const char *text) {
    char command[80];
    size_t n = strlen(text);
    if (n >= sizeof(command)) return false;
    memcpy(command, text, n + 1);
    while (n && (isspace((unsigned char)command[n - 1]) || command[n - 1] == '.' ||
                 command[n - 1] == '!' || command[n - 1] == '?')) command[--n] = '\0';
    const char *p = command;
    while (isspace((unsigned char)*p)) p++;
    return !strcasecmp(p, "stop voice") || !strcasecmp(p, "voice off") ||
           !strcasecmp(p, "return to typing");
}

static void voice_event(const realtime_event_t *event, void *ctx) {
    voice_session_t *v = ctx;
    char *clean_text = safe_strdup(event->text ? event->text : "");
    if (event->type != REALTIME_RESPONSE_DONE && event->type != REALTIME_TOOL_RESULT)
        dsco_strip_terminal_controls_inplace(clean_text);
    const char *text = clean_text;
    switch (event->type) {
    case REALTIME_STATUS: voice_notice(v, text, false); break;
    case REALTIME_ERROR: voice_notice(v, text, true); break;
    case REALTIME_READY:
        pixel_tui_session_set_voice(stderr, "listening");
        baseline_log("voice", "ready", text, NULL);
        voice_notice(v, "Listening · microphone and speaker ready · Esc / Ctrl+O to type", false);
        break;
    case REALTIME_MIC_STATE:
        pixel_tui_session_set_voice(stderr, text);
        tui_panel_notify(v->sb, TUI_PANEL_NOTE_ACTIVITY, text);
        break;
    case REALTIME_USER_TEXT:
        voice_end_reply(v);
        conv_add_user_text(v->conv, text);
        if (v->session->user_turn_count < INT_MAX) v->session->user_turn_count++;
        pixel_tui_session_set_turn(stderr, v->session->user_turn_count);
        baseline_log("user", "voice", text, NULL);
        if (pixel_tui_session_active()) pixel_tui_session_add_message(stderr, "USER", text);
        else {
            tui_term_lock();
            tui_prepare_external_output_locked();
            fprintf(stderr, "\n  %s▌ user · voice%s\n  %s\n", TUI_BCYAN, TUI_RESET, text);
            fflush(stderr);
            tui_term_unlock();
        }
        if (voice_stop_phrase(text)) {
            v->stop = true;
            conv_add_assistant_text(v->conv, "Voice stopped. Continuing in the same terminal session.");
        }
        break;
    case REALTIME_ASSISTANT_DELTA:
        voice_chronicle_begin(v);
        if (!v->reply_open) {
            v->reply_open = true;
            pixel_tui_session_set_voice(stderr, "speaking");
            pixel_tui_session_set_state(stderr, PIXEL_TUI_RESPONDING);
            if (pixel_tui_session_active()) pixel_tui_session_begin_message(stderr, "ASSISTANT", "voice");
            else {
                tui_term_lock();
                tui_prepare_external_output_locked();
                fprintf(stderr, "\n  %s▌ assistant · voice%s\n  ", TUI_BMAGENTA, TUI_RESET);
                tui_term_unlock();
            }
        }
        size_t delta_len = strlen(text);
        if (!v->response_truncated && delta_len <= VOICE_REPLY_LIMIT - v->reply.len &&
            delta_len <= VOICE_REPLY_LIMIT - v->response_text.len) {
            jbuf_append(&v->reply, text);
            jbuf_append(&v->response_text, text);
        } else v->response_truncated = true;
        if (pixel_tui_session_active()) pixel_tui_session_append_text(stderr, text);
        else {
            tui_term_lock(); fputs(text, stderr); fflush(stderr); tui_term_unlock();
        }
        break;
    case REALTIME_ASSISTANT_END: voice_end_reply(v); break;
    case REALTIME_TOOL_CALL:
        voice_chronicle_begin(v);
        voice_end_reply(v);
        if (v->chron_span[0] && v->tool_count < 16) {
            int index = v->tool_count++;
            snprintf(v->tools[index].id, sizeof(v->tools[index].id), "%s", event->id ? event->id : "");
            snprintf(v->tools[index].name, sizeof(v->tools[index].name), "%s", event->name ? event->name : "");
            if (!chronicle_tool_call_start(v->chron_trace, v->chron_span, event->name,
                    event->id, event->arguments, v->tools[index].span))
                fprintf(stderr, "warning: Chronicle voice tool start recording failed\n");
        }
        conv_add_assistant_tool_use(v->conv, event->id, event->name, event->arguments);
        pixel_tui_session_set_voice(stderr, "working");
        pixel_tui_session_set_state(stderr, PIXEL_TUI_EXECUTING);
        baseline_log("tool", event->name, event->arguments, "voice");
        break;
    case REALTIME_TOOL_RESULT:
        for (int i = 0; i < v->tool_count; i++) if (event->id &&
            strcmp(v->tools[i].id, event->id) == 0 && v->tools[i].span[0]) {
            if (!chronicle_tool_call_end(v->chron_trace, v->tools[i].span,
                    v->tools[i].name, text, strncmp(text, "ERROR: ", 7) != 0, false, 0))
                fprintf(stderr, "warning: Chronicle voice tool completion recording failed\n");
            v->tools[i].span[0] = '\0';
            break;
        }
        conv_add_tool_result(v->conv, event->id, text, !strncmp(text, "ERROR: ", 7));
        break;
    case REALTIME_RESPONSE_DONE: {
        voice_chronicle_begin(v);
        if (v->session->turn_count < INT_MAX) v->session->turn_count++;
        char *usage = json_get_raw(text, "usage");
        int input_tokens = usage ? json_get_int(usage, "input_tokens", 0) : 0;
        int output_tokens = usage ? json_get_int(usage, "output_tokens", 0) : 0;
        if (usage) {
            v->session->total_input_tokens += input_tokens;
            v->session->total_output_tokens += output_tokens;
            /* Realtime billing includes audio; text-token prices cannot price it. */
            v->session->unpriced_response_count++;
            free(usage);
        }
        char *provider_status = json_get_str(text, "status");
        const char *status = provider_status && !strcmp(provider_status, "completed") ? "completed"
            : provider_status && !strcmp(provider_status, "cancelled") ? "interrupted"
            : v->response_text.len ? "partial_failed" : "failed";
        voice_chronicle_finish(v, input_tokens, output_tokens, status);
        free(provider_status);
        pixel_tui_session_set_state(stderr, PIXEL_TUI_IDLE);
        break;
    }
    }
    free(clean_text);
}

static bool voice_should_stop(void *ctx, bool tools_busy) {
    voice_session_t *v = ctx;
    if (v->stop) return true;
    /* A governed tool may own stdin for an approval; never steal its answer. */
    if (tools_busy) return false;
    fd_set input;
    FD_ZERO(&input); FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {0, 0};
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0) return false;
    unsigned char key;
    ssize_t n = read(STDIN_FILENO, &key, 1);
    if (n == 0) return true;
    if (n < 0) return errno != EINTR && errno != EAGAIN;
    v->stop = key == 0x1b || key == 0x0f || key == 0x03 || key == 0x04;
    return v->stop;
}

int voice_session_run(conversation_t *conv, session_state_t *session,
                      tui_status_bar_t *sb, bool full_duplex) {
    if (!conv || !session || !isatty(STDIN_FILENO)) {
        tui_error("voice: in-session hands-free mode requires a terminal; use dsco voice for standalone mode");
        return 2;
    }
    voice_session_t v = {.conv = conv, .session = session, .sb = sb};
    jbuf_init(&v.reply, 1024);
    jbuf_init(&v.response_text, 1024);
    char *context = voice_session_context(conv);
    realtime_opts_t opts = {
        .half_duplex = !full_duplex, .embedded = true,
        .trust_tier = session_trust_tier_to_string(session->trust_tier),
        .initial_context = context, .on_event = voice_event,
        .should_stop = voice_should_stop, .callback_ctx = &v
    };
    struct termios saved, raw;
    bool terminal_saved = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (!terminal_saved) {
        jbuf_free(&v.reply); jbuf_free(&v.response_text); free(context);
        tui_error("voice: unable to read terminal state");
        return 1;
    }
    raw = saved;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        jbuf_free(&v.reply); jbuf_free(&v.response_text); free(context);
        tui_error("voice: unable to activate terminal controls");
        return 1;
    }
    pixel_tui_session_set_voice(stderr, "connecting");
    voice_notice(&v, "Hands-free voice · Esc / Ctrl+O or say 'stop voice' to type · OpenAI Realtime API", false);
    int rc = realtime_voice_run(&opts);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    voice_end_reply(&v);
    voice_chronicle_finish(&v, 0, 0, "interrupted");
    conv_ensure_tool_results(conv);
    pixel_tui_session_set_voice(stderr, NULL);
    pixel_tui_session_set_state(stderr, PIXEL_TUI_IDLE);
    voice_notice(&v, "Voice off · continue typing in this session", false);
    baseline_log("command", "/voice", rc ? "failed" : "stopped", NULL);
    jbuf_free(&v.reply);
    jbuf_free(&v.response_text);
    free(context);
    return rc;
}
