/* Real voice adapter, conversation and compositor, with a bounded audio peer. */
#include "voice_session.h"
#include "realtime.h"
#include "pixel_tui.h"
#include "capability.h"
#include "native_composer.h"
#include "vm.h"
#include "chronicle.h"
#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static FILE *receipt;
static bool spoken_stop;

static void emit(const realtime_opts_t *o, realtime_event_type_t type, const char *text,
                 const char *name, const char *id, const char *arguments) {
    realtime_event_t event = {type, text, name, id, arguments};
    o->on_event(&event, o->callback_ctx);
}

int realtime_voice_run(const realtime_opts_t *o) {
    assert(o->embedded && o->half_duplex && !o->text_only && !o->no_tools);
    assert(!strcmp(o->trust_tier, "untrusted"));
    assert(strstr(o->initial_context, "Remember CONTEXT_CANARY"));
    assert(!strstr(o->initial_context, "PRIVATE_REASONING_CANARY"));
    assert(strlen(o->initial_context) < 49 * 1024);
    assert(dsco_flow_accessed_private() && dsco_flow_tainted_untrusted());
    emit(o, REALTIME_READY, "owned voice fixture", NULL, NULL, NULL);
    emit(o, REALTIME_USER_TEXT, "fixture spoken prompt π", NULL, NULL, NULL);
    emit(o, REALTIME_ASSISTANT_DELTA, "Voice reply ", NULL, NULL, NULL);
    emit(o, REALTIME_ASSISTANT_DELTA, "complete", NULL, NULL, NULL);
    emit(o, REALTIME_ASSISTANT_END, NULL, NULL, NULL, NULL);
    emit(o, REALTIME_TOOL_CALL, NULL, "cwd", "voice-tool-1", "{}");
    emit(o, REALTIME_TOOL_RESULT, "/owned/voice/directory", NULL, "voice-tool-1", NULL);
    emit(o, REALTIME_RESPONSE_DONE, "{\"status\":\"completed\",\"usage\":{\"input_tokens\":10,\"output_tokens\":4}}",
         NULL, NULL, NULL);
    emit(o, REALTIME_ASSISTANT_DELTA, "Voice failure fragment", NULL, NULL, NULL);
    emit(o, REALTIME_ASSISTANT_END, NULL, NULL, NULL, NULL);
    emit(o, REALTIME_RESPONSE_DONE, "{\"status\":\"failed\"}", NULL, NULL, NULL);
    if (getenv("DSCO_VOICE_FIXTURE_TRUNCATE")) {
        emit(o, REALTIME_ASSISTANT_DELTA, "VOICE_TRUNCATED_PREFIX", NULL, NULL, NULL);
        char *large = malloc(256 * 1024 + 1);
        assert(large);
        memset(large, 'X', 256 * 1024);
        large[256 * 1024] = '\0';
        emit(o, REALTIME_ASSISTANT_DELTA, large, NULL, NULL, NULL);
        free(large);
        emit(o, REALTIME_ASSISTANT_END, NULL, NULL, NULL, NULL);
        emit(o, REALTIME_RESPONSE_DONE, "{\"status\":\"completed\"}", NULL, NULL, NULL);
    }
    if (spoken_stop) {
        emit(o, REALTIME_USER_TEXT, "Stop voice.", NULL, NULL, NULL);
        assert(o->should_stop(o->callback_ctx, false));
    } else {
        fputs("VOICE_READY\n", receipt); fflush(receipt);
        for (int i = 0; i < 500; i++) {
            if (o->should_stop(o->callback_ctx, false)) return 0;
            usleep(10000);
        }
        assert(!"voice stop key did not reach the session");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    bool native = !strcmp(argv[1], "native");
    spoken_stop = !strcmp(argv[3], "spoken");
    receipt = fopen(argv[2], "w"); assert(receipt);
    char chron_root[512];
    assert(snprintf(chron_root, sizeof(chron_root), "%s/voice-chronicle", getenv("HOME")) < (int)sizeof(chron_root));
    setenv("DSCO_CHRONICLE_DIR", chron_root, 1);
    setenv("DSCO_CHRONICLE_MODE", "full-local", 1);
    setenv("DSCO_JOURNAL", "off", 1);
    assert(chronicle_start(&(chronicle_start_opts_t){.provider="fixture",.model="fixture"}));
    tui_status_bar_t sb;
    tui_status_bar_init(&sb, "voice fixture");
    if (native) assert(pixel_tui_session_begin(stderr, "voice fixture"));
    conversation_t conv; conv_init(&conv);
    session_state_t session;
    memset(&session, 0, sizeof(session));
    session.trust_tier = DSCO_TRUST_UNTRUSTED;
    conv_add_user_text(&conv, "Remember CONTEXT_CANARY");
    conv_add_assistant_text(&conv, "PRIVATE_REASONING_CANARY");
    free(conv.msgs[1].content[0].type);
    conv.msgs[1].content[0].type = safe_strdup("thinking");
    dsco_flow_reset(); dsco_flow_note(CAP_SECRETS | CAP_UNTRUSTED_IN);
    char line[128];
    assert(tui_composer_read(&sb, NULL, line, sizeof(line)) && !strcmp(line, "/voice realtime"));
    struct termios before, after;
    assert(!tcgetattr(STDIN_FILENO, &before));
    assert(!voice_session_run(&conv, &session, &sb, false));
    assert(!tcgetattr(STDIN_FILENO, &after));
    assert(before.c_lflag == after.c_lflag && before.c_iflag == after.c_iflag);
    assert(!native || pixel_tui_session_active());
    assert(dsco_flow_accessed_private() && dsco_flow_tainted_untrusted());
    assert(session.total_input_tokens == 10 && session.total_output_tokens == 4);
    assert(session.unpriced_response_count == 1);
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL;
    assert(sqlite3_open_v2(chronicle_db_path(), &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(payload_json,'$.output_kind'),"
        "json_extract(payload_json,'$.output_blob_sha256'),"
        "json_type(payload_json,'$.cost_usd'),"
        "json_extract(payload_json,'$.response_status'),span_id FROM events "
        "WHERE session_id=?1 AND event_type='llm.response.completed' ORDER BY seq", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, chronicle_session_id(), -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 0), "mixed"));
    const char *sha = (const char *)sqlite3_column_text(st, 1);
    char *voice_text = chronicle_read_blob_hex(sha, 128, NULL);
    assert(voice_text && !strcmp(voice_text, "Voice reply complete"));
    free(voice_text);
    assert(!strcmp((const char *)sqlite3_column_text(st, 2), "null"));
    assert(!strcmp((const char *)sqlite3_column_text(st, 3), "completed"));
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 3), "partial_failed"));
    sha = (const char *)sqlite3_column_text(st, 1);
    voice_text = chronicle_read_blob_hex(sha, 128, NULL);
    assert(voice_text && !strcmp(voice_text, "Voice failure fragment"));
    free(voice_text);
    if (getenv("DSCO_VOICE_FIXTURE_TRUNCATE")) {
        assert(sqlite3_step(st) == SQLITE_ROW);
        assert(!strcmp((const char *)sqlite3_column_text(st, 3), "truncated"));
        sha = (const char *)sqlite3_column_text(st, 1);
        voice_text = chronicle_read_blob_hex(sha, 128, NULL);
        assert(voice_text && !strcmp(voice_text, "VOICE_TRUNCATED_PREFIX"));
        free(voice_text);
        const char *span_id = (const char *)sqlite3_column_text(st, 4);
        sqlite3_stmt *span = NULL;
        assert(sqlite3_prepare_v2(db, "SELECT payload_json FROM spans WHERE span_id=?1", -1,
                                  &span, NULL) == SQLITE_OK);
        sqlite3_bind_text(span, 1, span_id, -1, SQLITE_TRANSIENT);
        assert(sqlite3_step(span) == SQLITE_ROW);
        assert(strstr((const char *)sqlite3_column_text(span, 0), "capture_truncated"));
        sqlite3_finalize(span);
    }
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM events WHERE session_id=?1 AND event_type IN "
        "('tool.call.created','tool.call.completed')", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, chronicle_session_id(), -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2);
    sqlite3_finalize(st); sqlite3_close(db);
    chronicle_stop();
    char *context = voice_session_context(&conv);
    assert(strstr(context, "fixture spoken prompt π") && strstr(context, "Voice reply complete") &&
           strstr(context, "/owned/voice/directory"));
    free(context);
    native_ui_scene_t scene;
    native_composer_model_t model = {.voice_phase = "listening", .columns = 40};
    assert(native_composer_build(&scene, 800, 84, &model));
    bool label = false, hint = false;
    for (int i = 0; i < scene.count; i++) {
        if (scene.nodes[i].key == NATIVE_COMPOSER_KEY_TITLE)
            label = strstr(scene.nodes[i].text, "Voice · listening") != NULL;
        if (scene.nodes[i].key == NATIVE_COMPOSER_KEY_HINT)
            hint = strstr(scene.nodes[i].text, "Ctrl+O voice off") != NULL;
    }
    assert(label && hint);
    fputs("VOICE_STOPPED\n", receipt); fflush(receipt);
    assert(tui_composer_read(&sb, NULL, line, sizeof(line)));
    fprintf(receipt, "DRAFT:%s\n", line); fflush(receipt);
    if (native) pixel_tui_session_end(stderr);
    conv_free(&conv); tui_cleanup(); tui_composer_clear_retained_draft();
    pthread_mutex_destroy(&sb.mutex); fclose(receipt);
    return 0;
}
