#include "process_capture.h"
#include "tool_content.h"
#include "../vendor/yyjson.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char executable[PATH_MAX];
static unsigned checks;
static const unsigned char binary_fixture[] = {
    'A', 0, 'B', 0xff, 'C', 0xc0, 0xaf, 0xed, 0xa0, 0x80,
    0xf4, 0x90, 0x80, 0x80, 0xe2, 0x82
};

static void write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n;
        size -= (size_t)n;
    }
}

static double now_ms(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

/* All spawned fixtures execute this same test binary directly, never a shell. */
static int fixture(int argc, char **argv) {
    const char *mode = argv[2];
    if (!strcmp(mode, "stdin")) {
        char byte;
        ssize_t n = read(STDIN_FILENO, &byte, 1);
        if (n != 0) return 21; /* Must be EOF, never the caller's MCP/request pipe. */
        write_all(STDOUT_FILENO, "stdout:isolated\n", 16);
        write_all(STDERR_FILENO, "stderr:captured\n", 16);
        return 0;
    }
    if (!strcmp(mode, "echo_input")) {
        char buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) write_all(STDOUT_FILENO, buf, (size_t)n);
        return n < 0 ? 22 : 0;
    }
    if (!strcmp(mode, "argv")) {
        assert(argc == 4);
        write_all(STDOUT_FILENO, argv[3], strlen(argv[3]));
        return 0;
    }
    if (!strcmp(mode, "fd_closed")) {
        assert(argc == 4);
        int fd = atoi(argv[3]);
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 24;
        write_all(STDOUT_FILENO, "fd-isolated\n", 12);
        return 0;
    }
    if (!strcmp(mode, "overflow")) {
        char buf[8192];
        memset(buf, 'x', sizeof(buf));
        for (int i = 0; i < 128; ++i) write_all(STDOUT_FILENO, buf, sizeof(buf));
        return 0;
    }
    if (!strcmp(mode, "unicode_stream")) {
        for (int i = 0; i < 2048; ++i) write_all(STDOUT_FILENO, "雪", 3);
        return 0;
    }
    if (!strcmp(mode, "binary") || !strcmp(mode, "binary_stream")) {
        int count = !strcmp(mode, "binary_stream") ? 100 : 1;
        for (int i = 0; i < count; ++i)
            write_all(STDOUT_FILENO, binary_fixture, sizeof(binary_fixture));
        return 0;
    }
    if (!strcmp(mode, "group") || !strcmp(mode, "orphan_group")) {
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        pid_t descendant = fork();
        assert(descendant >= 0);
        if (!descendant) { for (;;) pause(); }
        char ids[128];
        int n = snprintf(ids, sizeof(ids), "%ld %ld %ld\n", (long)getpid(), (long)descendant, (long)getpgrp());
        write_all(STDOUT_FILENO, ids, (size_t)n);
        if (!strcmp(mode, "orphan_group")) return 0;
        for (;;) pause();
    }
    if (!strcmp(mode, "exit7")) return 7;
    return 23;
}

static void capture_basics(void) {
    process_capture_t c;
    int caller_input[2];
    assert(pipe(caller_input) == 0);
    int saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    const char caller_request[] = "request-stays-with-caller";
    write_all(caller_input[1], caller_request, sizeof(caller_request) - 1);
    close(caller_input[1]);
    assert(dup2(caller_input[0], STDIN_FILENO) == STDIN_FILENO);
    close(caller_input[0]);
    char *isolated[] = {executable, "--fixture", "stdin", NULL};
    bool ok = process_capture(executable, isolated, 2000, 1024, &c);
    char unread[sizeof(caller_request)] = "";
    ssize_t count = read(STDIN_FILENO, unread, sizeof(unread) - 1);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    close(saved_stdin);
    assert(ok && c.exit_code == 0 && !c.timed_out && !c.truncated);
    assert(count == sizeof(caller_request) - 1 && !strcmp(unread, caller_request));
    assert(c.output && strstr(c.output, "stdout:isolated") && strstr(c.output, "stderr:captured"));
    process_capture_free(&c);
    ++checks;

    /* A daemon may legitimately enter with fd 0 closed. Pipe allocation then
     * reuses fd 0; child file actions must still leave /dev/null as stdin. */
    saved_stdin = dup(STDIN_FILENO);
    assert(saved_stdin >= 0);
    assert(close(STDIN_FILENO) == 0);
    ok = process_capture(executable, isolated, 2000, 1024, &c);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    close(saved_stdin);
    assert(ok && c.exit_code == 0);
    process_capture_free(&c);
    ++checks;

    char unicode[] = "Snow 雪 🚀 | literal $(never-a-shell) ' \" \\ and\nnewline";
    char *args[] = {executable, "--fixture", "argv", unicode, NULL};
    assert(process_capture(executable, args, 2000, 2048, &c));
    assert(c.length == strlen(unicode) && !memcmp(c.output, unicode, c.length));
    process_capture_free(&c);
    ++checks;

    const char request[] = "bounded request\n\0binary tail";
    char *echo[] = {executable, "--fixture", "echo_input", NULL};
    assert(process_capture_input(executable, echo, request, sizeof(request), 2000, 2048, &c));
    assert(c.length == sizeof(request) && !memcmp(c.output, request, sizeof(request)));
    process_capture_free(&c);
    assert(!process_capture_input(executable, echo, "", 65537, 100, 128, &c));
    assert(c.spawn_error == EINVAL);
    process_capture_free(&c);
    ++checks;

    char *exit7[] = {executable, "--fixture", "exit7", NULL};
    assert(!process_capture(executable, exit7, 2000, 1024, &c));
    assert(c.exit_code == 7 && !c.spawn_error && !c.timed_out);
    process_capture_free(&c);
    char *missing[] = {"/this/path/does/not/exist/dsco-surface-fixture", NULL};
    assert(!process_capture(missing[0], missing, 2000, 1024, &c));
    assert(c.spawn_error == ENOENT && c.output == NULL);
    process_capture_free(&c);
    ++checks;
}

static void assert_base64_equals(const char *encoded, const unsigned char *expected, size_t size) {
    assert(encoded && strlen(encoded) % 4 == 0);
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned accumulator = 0, bits = 0;
    size_t emitted = 0;
    bool padding = false;
    for (const char *p = encoded; *p; ++p) {
        if (*p == '=') { padding = true; continue; }
        assert(!padding);
        const char *digit = strchr(alphabet, *p);
        assert(digit);
        accumulator = (accumulator << 6) | (unsigned)(digit - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            assert(emitted < size && ((accumulator >> bits) & 255) == expected[emitted]);
            ++emitted;
        }
    }
    assert(emitted == size);
}

static void capture_overflow(void) {
    process_capture_t c;
    char *args[] = {executable, "--fixture", "overflow", NULL};
    assert(process_capture(executable, args, 3000, 31, &c));
    assert(c.truncated && c.length == 31 && strlen(c.output) == 31);
    for (size_t i = 0; i < c.length; ++i) assert(c.output[i] == 'x');
    process_capture_free(&c);
    assert(process_capture(executable, args, 3000, 0, &c));
    assert(c.truncated && c.length == 0 && c.output[0] == '\0');
    process_capture_free(&c);
    ++checks;

    char output[1032]; /* Capture cap=97: cuts after the first byte of a UTF-8 character. */
    char *unicode[] = {executable, "--fixture", "unicode_stream", NULL};
    assert(process_capture_json(executable, unicode, 3000, "Unicode output", output, sizeof(output)));
    yyjson_doc *doc = yyjson_read(output, strlen(output), 0);
    assert(doc); /* Includes strict UTF-8 validation at a truncation boundary. */
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_is_true(yyjson_obj_get(root, "ok")));
    assert(yyjson_is_true(yyjson_obj_get(root, "truncated")));
    const char *captured = yyjson_get_str(yyjson_obj_get(root, "output"));
    assert(captured && strlen(captured) > 0 && strlen(captured) % 3 == 0);
    unsigned char exact_prefix[97];
    for (size_t i = 0; i < sizeof(exact_prefix); ++i) exact_prefix[i] = ((const unsigned char *)"雪")[i % 3];
    assert_base64_equals(yyjson_get_str(yyjson_obj_get(root, "output_base64")), exact_prefix, sizeof(exact_prefix));
    yyjson_doc_free(doc);
    ++checks;

    char enormous_label[3000];
    memset(enormous_label, 'a', sizeof(enormous_label) - 1);
    enormous_label[sizeof(enormous_label) - 1] = '\0';
    assert(!process_capture_json(executable, args, 3000, enormous_label, output, sizeof(output)));
    doc = yyjson_read(output, strlen(output), 0);
    assert(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "truncated")));
    yyjson_doc_free(doc);
    char tiny[16];
    assert(!process_capture_json(executable, args, 3000, "tiny", tiny, sizeof(tiny)));
    doc = yyjson_read(tiny, strlen(tiny), 0);
    assert(doc);
    yyjson_doc_free(doc);
    ++checks;
}

static void capture_binary_json(void) {
    const char *modes[] = {"binary", "binary_stream"};
    for (int i = 0; i < 2; ++i) {
        char output[1032];
        char *args[] = {executable, "--fixture", (char *)modes[i], NULL};
        assert(process_capture_json(executable, args, 3000, "binary output", output, sizeof(output)));
        yyjson_doc *doc = yyjson_read(output, strlen(output), 0);
        assert(doc); /* Rejects illegal UTF-8, lone surrogates, and malformed JSON. */
        yyjson_val *root = yyjson_doc_get_root(doc);
        assert(yyjson_is_true(yyjson_obj_get(root, "ok")));
        assert(yyjson_get_bool(yyjson_obj_get(root, "truncated")) == (i == 1));
        yyjson_val *text = yyjson_obj_get(root, "output");
        assert(yyjson_get_len(text) >= 3 && !memcmp(yyjson_get_str(text), "A\0B", 3));
        unsigned char exact[97];
        size_t bytes = i ? sizeof(exact) : sizeof(binary_fixture);
        for (size_t j = 0; j < bytes; ++j) exact[j] = binary_fixture[j % sizeof(binary_fixture)];
        assert_base64_equals(yyjson_get_str(yyjson_obj_get(root, "output_base64")), exact, bytes);
        yyjson_doc_free(doc);
        ++checks;
    }
}

static void capture_descriptors(void) {
    /* Exercise every combination involving closed stdout/stderr, including all
     * three closed. Restore the test runner's descriptors before assertions. */
    for (int mask = 2; mask < 8; ++mask) {
        int saved[3];
        for (int fd = 0; fd < 3; ++fd) {
            saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 20);
            assert(saved[fd] >= 20);
        }
        for (int fd = 0; fd < 3; ++fd) if (mask & (1 << fd)) close(fd);
        char *args[] = {executable, "--fixture", "stdin", NULL};
        process_capture_t c;
        bool ok = process_capture(executable, args, 2000, 1024, &c);
        char *echo[] = {executable, "--fixture", "echo_input", NULL};
        process_capture_t input;
        bool input_ok = process_capture_input(executable, echo, "private request", 15, 2000, 1024, &input);
        for (int fd = 0; fd < 3; ++fd) {
            if (dup2(saved[fd], fd) != fd) _exit(25);
            close(saved[fd]);
        }
        assert(ok && c.exit_code == 0 && c.length == 32);
        assert(strstr(c.output, "stdout:isolated") && strstr(c.output, "stderr:captured"));
        if (!input_ok || input.length != 15)
            fprintf(stderr, "closed descriptor mask=%d: input_ok=%d length=%zu exit=%d spawn=%d\n",
                    mask, input_ok, input.length, input.exit_code, input.spawn_error);
        assert(input_ok && input.length == 15 && !memcmp(input.output, "private request", 15));
        process_capture_free(&c);
        process_capture_free(&input);
        ++checks;
    }

    /* A deliberately non-CLOEXEC descriptor must not leak into the helper. */
    int source[2];
    assert(pipe(source) == 0);
    int private_fd = fcntl(source[0], F_DUPFD, 96);
    assert(private_fd >= 96);
    close(source[0]);
    write_all(source[1], "parent-only", 11);
    close(source[1]);
    assert((fcntl(private_fd, F_GETFD) & FD_CLOEXEC) == 0);
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", private_fd);
    char *args[] = {executable, "--fixture", "fd_closed", descriptor, NULL};
    process_capture_t c;
    bool ok = process_capture(executable, args, 2000, 1024, &c);
    char sentinel[12] = "";
    ssize_t n = read(private_fd, sentinel, sizeof(sentinel) - 1);
    close(private_fd);
    if (!ok || c.exit_code != 0)
        fprintf(stderr, "inherited descriptor: ok=%d exit=%d spawn=%d\n", ok, c.exit_code, c.spawn_error);
    assert(ok && c.exit_code == 0 && !strcmp(c.output, "fd-isolated\n"));
    assert(n == 11 && !strcmp(sentinel, "parent-only"));
    process_capture_free(&c);
    ++checks;
}

static bool pid_gone(pid_t pid) {
    double deadline = now_ms() + 1500;
    do {
        if (kill(pid, 0) < 0 && errno == ESRCH) return true;
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    } while (now_ms() < deadline);
    return false;
}

static void capture_timeout(const char *mode) {
    /* An unrelated child must survive termination of the capture's group. */
    pid_t sibling = fork();
    assert(sibling >= 0);
    if (!sibling) { for (;;) pause(); }
    char *args[] = {executable, "--fixture", (char *)mode, NULL};
    process_capture_t c;
    double started = now_ms();
    bool ok = process_capture(executable, args, 500, 1024, &c);
    double elapsed = now_ms() - started;
    long direct = 0, descendant = 0, group = 0;
    int parsed = c.output ? sscanf(c.output, "%ld %ld %ld", &direct, &descendant, &group) : 0;
    bool sibling_alive = kill(sibling, 0) == 0;
    kill(sibling, SIGKILL);
    while (waitpid(sibling, NULL, 0) < 0 && errno == EINTR) {}
    bool parent_dead = parsed == 3 && pid_gone((pid_t)direct);
    bool descendant_dead = parsed == 3 && pid_gone((pid_t)descendant);
    /* Clean up only the fixture PIDs if the assertion is about to expose a bug. */
    if (parsed == 3 && !parent_dead) kill((pid_t)direct, SIGKILL);
    if (parsed == 3 && !descendant_dead) kill((pid_t)descendant, SIGKILL);
    assert(!ok && c.timed_out && !c.spawn_error);
    assert(parsed == 3 && direct == group && group != getpgrp());
    assert(elapsed >= 350 && elapsed < 3000);
    assert(parent_dead && descendant_dead && sibling_alive);
    process_capture_free(&c);
    ++checks;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int ready;
    bool release;
} rendezvous_t;

typedef struct {
    rendezvous_t *sync;
    const char *first, *second;
    tool_content_t *items;
} content_worker_t;

static void *queue_worker(void *ptr) {
    content_worker_t *w = ptr;
    assert(tool_content_take() == NULL);
    assert(tool_content_add_image_base64(w->first, "image/png"));
    assert(tool_content_add_image_base64(w->second, "image/jpeg"));
    pthread_mutex_lock(&w->sync->lock);
    ++w->sync->ready;
    pthread_cond_broadcast(&w->sync->changed);
    while (!w->sync->release) pthread_cond_wait(&w->sync->changed, &w->sync->lock);
    pthread_mutex_unlock(&w->sync->lock);
    w->items = tool_content_take();
    assert(w->items && w->items->next && !w->items->next->next);
    assert(!strcmp(w->items->data, w->first) && !strcmp(w->items->next->data, w->second));
    assert(tool_content_take() == NULL);
    return NULL;
}

static void content_threads(void) {
    tool_content_clear();
    assert(tool_content_add_image_base64("bWFpbg==", "image/png"));
    rendezvous_t sync = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, false};
    content_worker_t workers[] = {
        {&sync, "dDA=", "dDAy", NULL}, {&sync, "dDE=", "dDEy", NULL}, {&sync, "dDI=", "dDIy", NULL}
    };
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i) assert(pthread_create(&threads[i], NULL, queue_worker, &workers[i]) == 0);
    pthread_mutex_lock(&sync.lock);
    while (sync.ready < 3) pthread_cond_wait(&sync.changed, &sync.lock);
    tool_content_t *main_queue = tool_content_take();
    assert(main_queue && !main_queue->next && !strcmp(main_queue->data, "bWFpbg=="));
    assert(tool_content_merge(main_queue));
    sync.release = true;
    pthread_cond_broadcast(&sync.changed);
    pthread_mutex_unlock(&sync.lock);
    for (int i = 0; i < 3; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(tool_content_merge(workers[i].items));
    }
    assert(tool_content_add_image_base64("bGFzdA==", "image/png"));
    assert(!tool_content_add_image_base64("b3ZlcmZsb3c=", "image/png"));
    content_worker_t excess = {&sync, "b3ZlcjE=", "b3ZlcjI=", NULL};
    pthread_t excess_thread;
    assert(pthread_create(&excess_thread, NULL, queue_worker, &excess) == 0);
    assert(pthread_join(excess_thread, NULL) == 0);
    assert(!tool_content_merge(excess.items)); /* Full queue must report dropped items. */
    tool_content_t *all = tool_content_take(), *item = all;
    assert(item && !strcmp(item->data, "bWFpbg=="));
    item = item->next;
    for (int i = 0; i < 3; ++i) {
        assert(item && !strcmp(item->data, workers[i].first) && !strcmp(item->mime_type, "image/png"));
        item = item->next;
        assert(item && !strcmp(item->data, workers[i].second) && !strcmp(item->mime_type, "image/jpeg"));
        item = item->next;
    }
    assert(item && !strcmp(item->data, "bGFzdA==") && !item->next);
    tool_content_free(all);
    assert(tool_content_take() == NULL);
    pthread_cond_destroy(&sync.changed);
    pthread_mutex_destroy(&sync.lock);
    ++checks;
}

static void content_files_and_validation(void) {
    char path[] = "/tmp/dsco-surface-image-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const unsigned char bytes[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    write_all(fd, bytes, sizeof(bytes));
    assert(tool_content_add_image_file(path, "image/png"));
    tool_content_t *item = tool_content_take();
    assert(item && !item->next && !strcmp(item->data, "iVBORw0KGgo="));
    tool_content_free(item);
    assert(ftruncate(fd, 0) == 0);
    assert(!tool_content_add_image_file(path, "image/png"));
    assert(ftruncate(fd, 8 * 1024 * 1024 + 1) == 0);
    assert(!tool_content_add_image_file(path, "image/png"));
    close(fd);
    assert(unlink(path) == 0);
    assert(!tool_content_add_image_file(path, "image/png"));
    assert(!tool_content_add_image_base64("AA==", "text/plain"));
    assert(!tool_content_add_image_base64("", "image/png"));
    assert(!tool_content_add_image_base64("AAA", "image/png"));
    assert(!tool_content_add_image_base64("A!==", "image/png"));
    assert(!tool_content_add_image_base64("AA=A", "image/png"));
    assert(tool_content_take() == NULL);
    ++checks;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--fixture")) return fixture(argc, argv);
    assert(argc == 1);
    assert(realpath(argv[0], executable));
    capture_basics();
    capture_descriptors();
    capture_overflow();
    capture_binary_json();
    capture_timeout("group");
    capture_timeout("orphan_group");
    content_threads();
    content_files_and_validation();
    printf("surface transport: %u checks passed (owned process fixtures and thread content queues)\n", checks);
    return 0;
}
