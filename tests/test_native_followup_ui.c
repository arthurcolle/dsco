/* Offline real compositor + composer regression. No inference or desktop
 * surfaces. Link as test_native_session_details (exclude pixel_tui.o).
 * The PTY subtest owns its stdin, writes pixels only to tmpfile, and has a
 * hard deadline; no user's terminal, clipboard or buffers are touched. */
#define main native_windows_fixture_main
#include "test_pixel_native_windows.c"
#undef main
#include <sys/wait.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

static native_followup_snapshot_t view;
static native_followup_action_t action;
static native_followup_queue_t queue;
static FILE *sink;

static void key(int k) { CHECK(pixel_tui_session_followup_key(sink, k), "queue consumes input"); }
static void publish(void) {
    CHECK(native_followup_queue_snapshot(&queue, &view) == NATIVE_FOLLOWUP_OK, "backend snapshot");
    pixel_tui_set_followup_queue(&view);
}
static void setup(void) {
    pixel_tui_set_followup_queue(NULL);
    native_windows_reset();
    memset(&g_session, 0, sizeof(g_session));
    g_session.active = true;
    g_session.width = g_session.surface_width = 800;
    g_session.height = g_session.surface_height = 600;
    g_session.backing_scale = 1;
    g_session.cols = 100; g_session.rows = 50;
    g_session.current_message = -1;
    g_session.tty_out = sink;
    /* Defer paints to explicit draw calls; no compositor thread is launched. */
    g_session.animation_thread_started = true;
    ui_motion_init(&g_session.motion, true);
    snprintf(g_session.input, sizeof(g_session.input), "keep composer π draft");
    g_session.input_cursor = 5;
}

static void check_snapshot_actions(void) {
    memset(&view, 0, sizeof(view));
    view.revision = 100; view.count = 2;
    view.entries[0].id = 41; view.entries[0].revision = 9;
    view.entries[1].id = 42; view.entries[1].revision = 15;
    strcpy(view.entries[0].text, "first π 🦉\nsecond line");
    strcpy(view.entries[1].text, "second queued");
    pixel_tui_set_followup_queue(&view);
    CHECK(!pixel_tui_followup_focused() && !native_windows_visible(), "publication opens no view");
    strcpy(view.entries[0].text, "caller changed after publication");
    CHECK(!strcmp(s_followups.entries[0].text, "first π 🦉\nsecond line"), "synchronous owned text copy");
    view.revision = 99; pixel_tui_set_followup_queue(&view);
    CHECK(s_followups.revision == 100, "reject older snapshot");
    view.revision = 100; pixel_tui_set_followup_queue(&view);
    CHECK(strstr(s_followups.entries[0].text, "first"), "equal revision cannot rewrite snapshot");
    view.revision = 101; view.count = NATIVE_FOLLOWUP_QUEUE_CAP + 1;
    pixel_tui_set_followup_queue(&view);
    CHECK(s_followups.count == 2 && s_followups.revision == 100, "reject oversized snapshot atomically");
    view.count = 2; view.entries[1].id = 41;
    pixel_tui_set_followup_queue(&view);
    CHECK(s_followups.revision == 100, "reject duplicate IDs atomically");
    view.entries[1].id = 42;
    memset(view.entries[1].text, 'x', sizeof(view.entries[1].text));
    pixel_tui_set_followup_queue(&view);
    CHECK(s_followups.revision == 100, "reject unterminated entry atomically");

    key(17); key('\n'); key(1);
    const char *replacement = "edited π 🦉\nretained text";
    CHECK(pixel_tui_followup_insert_paste(sink, 41, replacement, strlen(replacement)), "UTF-8 multiline replacement");
    CHECK(native_buffer_editor_cursor_valid(&s_followup_editor, s_followup_draft), "valid UTF-8 caret");
    key(NATIVE_WINDOW_KEY_LEFT); key('!'); key(26); key(25);
    CHECK(strstr(s_followup_draft, "tex!t"), "caret movement and undo/redo use real editor");
    key(19);
    CHECK(pixel_tui_followup_action_pending() && !pixel_tui_poll_followup_action(NULL), "pending and null poll are non-consuming");
    CHECK(pixel_tui_poll_followup_action(&action), "poll replace");
    CHECK(action.kind == NATIVE_FOLLOWUP_ACTION_REPLACE && action.id == 41 && action.expected_revision == 9 &&
          !strcmp(action.text, s_followup_draft), "replace uses entry revision, not queue revision");
    CHECK(!pixel_tui_followup_action_pending(), "poll drains action");
    CHECK(!strcmp(g_session.input, "keep composer π draft") && g_session.input_cursor == 5,
          "queue editing preserves composer bytes/cursor");
    key(27); key(27); key(17); key('\n');
    CHECK(strstr(s_followup_draft, "tex!t"), "close and reopen retains edit");
    key(27); key('\t');
    for (int i = 0; i < FOLLOWUP_ACTION_CAP; ++i) key(4);
    key(4);
    CHECK(s_followup_count == FOLLOWUP_ACTION_CAP && strstr(s_followup_feedback, "full"), "bounded action queue fails visibly without overwrite");
    for (int i = 0; i < FOLLOWUP_ACTION_CAP; ++i) {
        CHECK(pixel_tui_poll_followup_action(&action) && action.kind == NATIVE_FOLLOWUP_ACTION_REMOVE &&
              action.id == 42 && action.expected_revision == 15 && !action.text[0], "FIFO remove action has stable CAS and no stale text");
    }
    CHECK(!pixel_tui_poll_followup_action(&action), "FIFO ends exactly at bound");
    key(4); key('\t'); key(4);
    CHECK(pixel_tui_poll_followup_action(&action) && action.id == 42, "ring wrap preserves first identity");
    CHECK(pixel_tui_poll_followup_action(&action) && action.id == 41, "ring wrap preserves second identity");
    key(4); pixel_tui_set_followup_queue(NULL);
    CHECK(!pixel_tui_followup_action_pending() && !pixel_tui_followup_focused() && !s_followup_id,
          "reset clears queue/actions/editor lifetime");
}

static void check_backend_cas(void) {
    CHECK(native_followup_queue_init(&queue), "backend init");
    uint64_t first, second;
    CHECK(native_followup_queue_push(&queue, "original", &first) == NATIVE_FOLLOWUP_OK, "first entry");
    CHECK(native_followup_queue_push(&queue, "untouched", &second) == NATIVE_FOLLOWUP_OK, "second entry");
    publish(); key(17); key('\n'); key(1);
    CHECK(pixel_tui_followup_insert_paste(sink, first, "replacement", 11), "backend edit");
    key(19); CHECK(pixel_tui_poll_followup_action(&action), "backend poll");
    CHECK(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_OK, "backend accepts UI CAS");
    publish();
    CHECK(!strcmp(s_followups.entries[0].text, "replacement") && s_followup_revision == s_followups.entries[0].revision,
          "acknowledged save advances editor CAS without discarding draft");
    key('!');
    CHECK(native_followup_queue_replace(&queue, first, 0, "external edit") == NATIVE_FOLLOWUP_OK, "concurrent backend edit");
    publish(); key(19);
    CHECK(!pixel_tui_followup_action_pending() && !strcmp(s_followup_draft, "replacement!"),
          "stale view retains edit and refuses overwrite");
    key(18);
    CHECK(!strcmp(s_followup_draft, "external edit"), "explicit reload refreshes conflicted editor");
    key(4); CHECK(pixel_tui_poll_followup_action(&action), "remove request");
    CHECK(native_followup_queue_remove(&queue, first, 0) == NATIVE_FOLLOWUP_OK, "backend consumes selected entry first");
    CHECK(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_NOT_FOUND, "late action never targets shifted next entry");
    publish();
    CHECK(s_followups.count == 1 && s_followups.entries[0].id == second && !strcmp(s_followup_draft, "external edit"),
          "consumption preserves retained view and unrelated queue item");
    pixel_tui_set_followup_queue(NULL);
    native_followup_queue_destroy(&queue);
}

static void check_pixels_pointer(void) {
    memset(&view, 0, sizeof(view)); view.revision = 1; view.count = 1;
    view.entries[0].id = 8; view.entries[0].revision = 1;
    strcpy(view.entries[0].text, "Visible queued π 🦉\nEditable multiline body");
    pixel_tui_set_followup_queue(&view);
    px_canvas_t *a = render_session_frame(800, 600, 1, 800, 600, "fixture", PIXEL_TUI_IDLE);
    native_ui_rect_t area = session_windows_work_area(800, 600, PIXEL_TUI_IDLE);
    CHECK(pixel_tui_session_pointer(sink, 0, cell_x(area.x + 40), cell_y(area.y + 10), false), "badge press consumed");
    CHECK(!pixel_tui_followup_focused(), "badge requires release");
    CHECK(pixel_tui_session_pointer(sink, 0, cell_x(area.x + 40), cell_y(area.y + 10), true), "badge release consumed");
    px_canvas_t *b = render_session_frame(800, 600, 1, 800, 600, "fixture", PIXEL_TUI_IDLE);
    CHECK(a && b && changed_rows(a, b, area.y, area.y + area.height) > 50, "queue paints visible content pixels");
    free_canvas(a); free_canvas(b);
    int x = area.x + area.width - 40, y = area.y + 76;
    CHECK(pixel_tui_session_pointer(sink, 0, cell_x(x), cell_y(y), false), "remove press");
    view.revision = 2; view.entries[0].id = 9; view.entries[0].revision = 2;
    pixel_tui_set_followup_queue(&view);
    CHECK(pixel_tui_session_pointer(sink, 0, cell_x(x), cell_y(y), true) && !pixel_tui_followup_action_pending(),
          "snapshot between press/release cannot retarget remove");
    CHECK(pixel_tui_session_pointer(sink, 0, cell_x(x), cell_y(y), false) &&
          pixel_tui_session_pointer(sink, 0, cell_x(x), cell_y(y), true), "remove control click");
    CHECK(pixel_tui_poll_followup_action(&action) && action.id == 9 && action.expected_revision == 2,
          "remove button produces typed stable action");
    key('\n');
    for (int i = 0; i < 2; ++i) {
        int w = i ? 1200 : 360, h = i ? 700 : 480;
        px_canvas_t *frame = render_session_frame(w, h, .65, (int)(w * .65), (int)(h * .65), "fixture", PIXEL_TUI_IDLE);
        CHECK(frame && frame->pixels && frame->pixel_width == (int)(w * .65), "narrow/wide editor renders at zoom .65");
        free_canvas(frame);
    }
    CHECK(!strcmp(g_session.input, "keep composer π draft") && g_session.input_cursor == 5, "pointer view preserves composer");
    pixel_tui_set_followup_queue(NULL);
}

static void *small_stack_copy(void *unused) {
    (void)unused;
    pixel_tui_set_followup_queue(&view);
    bool ok = pixel_tui_session_followup_key(sink, 17) &&
              pixel_tui_session_followup_key(sink, '\n') &&
              pixel_tui_session_followup_key(sink, 19) &&
              pixel_tui_poll_followup_action(&action);
    return (void *)(uintptr_t)ok;
}

static void check_long_text_small_stack(void) {
    memset(&view, 0, sizeof(view)); view.revision = 1; view.count = 1;
    view.entries[0].id = 501; view.entries[0].revision = 1;
    memset(view.entries[0].text, 'x', NATIVE_FOLLOWUP_TEXT_CAP - 1);
    view.entries[0].text[NATIVE_FOLLOWUP_TEXT_CAP - 1] = 0;
    pthread_attr_t attr; pthread_attr_init(&attr);
    CHECK(!pthread_attr_setstacksize(&attr, 128 * 1024), "small composer-sized stack configured");
    pthread_t thread;
    int started = pthread_create(&thread, &attr, small_stack_copy, NULL);
    CHECK(!started, "small-stack bridge thread starts");
    pthread_attr_destroy(&attr);
    void *result = NULL;
    if (!started) pthread_join(thread, &result);
    CHECK(result == (void *)(uintptr_t)1 && action.id == 501 &&
          strlen(action.text) == NATIVE_FOLLOWUP_TEXT_CAP - 1 && !strcmp(action.text, view.entries[0].text),
          "full 65535-byte snapshot edit and action copy fit 128 KiB thread stack without truncation");
    size_t cursor = s_followup_editor.cursor;
    CHECK(!pixel_tui_followup_insert_paste(sink, 501, "overflow", 8) &&
          s_followup_editor.cursor == cursor && strlen(s_followup_draft) == NATIVE_FOLLOWUP_TEXT_CAP - 1,
          "capacity rejection preserves full draft and cursor");
    CHECK(!pixel_tui_followup_insert_paste(sink, 999, "other", 5) && strlen(s_followup_draft) == NATIVE_FOLLOWUP_TEXT_CAP - 1,
          "paste cannot target a different retained entry");
    key(1); key(127); key(19);
    CHECK(!pixel_tui_followup_action_pending() && strstr(s_followup_feedback, "Empty"),
          "empty edit cannot accidentally remove or send original");
    pixel_tui_set_followup_queue(NULL);
}

static int pty_child(int report_fd) {
    alarm(12);
    atomic_store(&g_session_active_fast, true);
    tui_composer_set_action_wakeup(false); /* streaming reader policy */
    tui_status_bar_t bar;
    tui_status_bar_init(&bar, "offline fixture"); bar.animations_enabled = false;
    memset(&view, 0, sizeof(view)); view.revision = 1; view.count = 1;
    view.entries[0].id = 77; view.entries[0].revision = 1; strcpy(view.entries[0].text, "old");
    pixel_tui_set_followup_queue(&view);
    char line[256];
    write(report_fd, "R", 1);
    char *result = tui_composer_read(&bar, NULL, line, sizeof(line));
    if (result != line || line[0] || !pixel_tui_poll_followup_action(&action) ||
        action.kind != NATIVE_FOLLOWUP_ACTION_REPLACE || action.id != 77 || action.expected_revision != 1 ||
        strcmp(action.text, "new π\nline")) return 2;
    write(report_fd, "Y", 1);
    result = tui_composer_read(&bar, NULL, line, sizeof(line));
    /* Parent left the composer caret between a/b before entering the view. */
    if (result != line || strcmp(line, "aXb")) return 3;
    write(report_fd, "D", 1);
    return 0;
}

static bool wait_byte(int fd, char expected) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    char c = 0;
    return poll(&p, 1, 8000) > 0 && read(fd, &c, 1) == 1 && c == expected;
}
static void check_composer_pty(void) {
    int report[2], master = -1;
    CHECK(pipe(report) == 0, "PTY result pipe");
    struct winsize size = {.ws_row = 50, .ws_col = 100, .ws_xpixel = 800, .ws_ypixel = 600};
    pid_t pid = forkpty(&master, NULL, NULL, &size);
    CHECK(pid >= 0, "owned PTY fork");
    if (pid == 0) { close(report[0]); _exit(pty_child(report[1])); }
    close(report[1]);
    if (pid < 0) { close(report[0]); return; }
    bool ready = wait_byte(report[0], 'R');
    CHECK(ready, "PTY child ready");
    /* Wait for raw composer entry after the ready byte; pixels remain tmpfile. */
    struct timespec delay = {.tv_nsec = 150000000}; nanosleep(&delay, NULL);
    const char *input = "ab\033[D\021\r\001\033[200~new π\nline\033[201~\023";
    if (ready) write(master, input, strlen(input));
    bool yielded = wait_byte(report[0], 'Y');
    CHECK(yielded, "reader wakes on queue action with general action wake disabled and returns caller-owned empty buffer");
    const char resume[] = "\021\021X\r";
    if (yielded) write(master, resume, sizeof(resume) - 1);
    CHECK(yielded && wait_byte(report[0], 'D'), "draft and byte cursor survive action-only yield and queue closes");
    if (!yielded) kill(pid, SIGTERM);
    int status = 0; waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "PTY composer exits successfully");
    close(master); close(report[0]);
}

int main(void) {
    sink = tmpfile(); if (!sink) return 2;
    setup(); check_snapshot_actions(); check_backend_cas(); check_pixels_pointer();
    check_long_text_small_stack(); check_composer_pty();
    pixel_tui_set_followup_queue(NULL);
    g_session.active = false; g_session.animation_thread_started = false; g_session.tty_out = NULL;
    window_raster_clear(); fclose(sink);
    fprintf(stderr, "native_followup_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
