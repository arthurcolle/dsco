#include "swarm.h"
#include "json_util.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>

/* ── Time helper ──────────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void swarm_init(swarm_t *s, const char *api_key, const char *model) {
    memset(s, 0, sizeof(*s));
    s->api_key = api_key;
    s->default_model = model;

    /* Find our own binary */
    char self[4096];
    uint32_t size = sizeof(self);
    /* Try /proc/self/exe first, then fallback */
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len > 0) {
        self[len] = '\0';
        s->dsco_path = strdup(self);
    } else {
        /* macOS: use _NSGetExecutablePath if available, or fallback to PATH */
        s->dsco_path = strdup("dsco");
    }
    (void)size;
}

void swarm_destroy(swarm_t *s) {
    /* Kill all running children */
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING) {
            kill(c->pid, SIGTERM);
            waitpid(c->pid, NULL, WNOHANG);
        }
        if (c->pipe_fd >= 0) close(c->pipe_fd);
        if (c->err_fd >= 0) close(c->err_fd);
        free(c->output);
        free(c->stream_buf);
    }
    free((void *)s->dsco_path);
}

/* ── Spawn ────────────────────────────────────────────────────────────── */

int swarm_spawn(swarm_t *s, const char *task, const char *model) {
    return swarm_spawn_in_group(s, -1, task, model);
}

int swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model) {
    if (s->child_count >= SWARM_MAX_CHILDREN) return -1;

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return -1;
    }

    int id = s->child_count;

    if (pid == 0) {
        /* Child process */
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        const char *m = model ? model : s->default_model;
        const char *bin = s->dsco_path;
        const char *parent_instance = getenv("DSCO_INSTANCE_ID");
        if (parent_instance && parent_instance[0]) {
            setenv("DSCO_PARENT_INSTANCE_ID", parent_instance, 1);
        }

        /* exec dsco with the task as one-shot prompt */
        execlp(bin, bin, "-m", m, task, NULL);
        /* If exec fails */
        fprintf(stderr, "swarm: exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent */
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    /* Make pipes non-blocking */
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    swarm_child_t *c = &s->children[id];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->pid = pid;
    c->pipe_fd = stdout_pipe[0];
    c->err_fd = stderr_pipe[0];
    c->status = SWARM_RUNNING;
    c->group_id = group_id;
    c->start_time = now_sec();

    snprintf(c->task, SWARM_LABEL_LEN, "%s", task);
    if (model) snprintf(c->model, sizeof(c->model), "%s", model);

    c->output_cap = 4096;
    c->output = malloc(c->output_cap);
    c->output[0] = '\0';
    c->output_len = 0;

    c->stream_buf = malloc(4096);
    c->stream_buf[0] = '\0';
    c->stream_buf_len = 0;

    s->child_count++;

    /* Add to group if specified */
    if (group_id >= 0 && group_id < s->group_count) {
        swarm_group_t *g = &s->groups[group_id];
        if (g->child_count < SWARM_MAX_CHILDREN) {
            g->child_ids[g->child_count++] = id;
        }
    }

    return id;
}

/* ── Groups ───────────────────────────────────────────────────────────── */

int swarm_group_create(swarm_t *s, const char *name) {
    if (s->group_count >= SWARM_MAX_GROUPS) return -1;
    int id = s->group_count;
    swarm_group_t *g = &s->groups[id];
    memset(g, 0, sizeof(*g));
    g->id = id;
    g->active = true;
    snprintf(g->name, SWARM_GROUP_NAME_LEN, "%s", name ? name : "unnamed");
    s->group_count++;
    return id;
}

int swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count,
                          const char *model) {
    if (group_id < 0 || group_id >= s->group_count) return -1;
    int spawned = 0;
    for (int i = 0; i < task_count; i++) {
        int cid = swarm_spawn_in_group(s, group_id, tasks[i], model);
        if (cid >= 0) spawned++;
    }
    return spawned;
}

bool swarm_group_complete(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count) return true;
    swarm_group_t *g = &s->groups[group_id];
    for (int i = 0; i < g->child_count; i++) {
        swarm_child_t *c = &s->children[g->child_ids[i]];
        if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING)
            return false;
    }
    return true;
}

/* ── Read from child fd, append to output ─────────────────────────────── */

static void child_read(swarm_child_t *c, int fd, swarm_stream_cb cb, void *ctx) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';

        /* Grow output buffer if needed */
        while (c->output_len + (size_t)n + 1 > c->output_cap) {
            c->output_cap *= 2;
            if (c->output_cap > SWARM_MAX_OUTPUT) c->output_cap = SWARM_MAX_OUTPUT;
            c->output = realloc(c->output, c->output_cap);
        }

        if (c->output_len + (size_t)n < c->output_cap) {
            memcpy(c->output + c->output_len, buf, n);
            c->output_len += n;
            c->output[c->output_len] = '\0';
        }

        if (cb) cb(c->id, buf, n, ctx);
        c->status = SWARM_STREAMING;
    }
}

/* ── Polling ──────────────────────────────────────────────────────────── */

int swarm_poll(swarm_t *s, int timeout_ms) {
    return swarm_poll_stream(s, timeout_ms, s->stream_cb, s->stream_ctx);
}

int swarm_poll_stream(swarm_t *s, int timeout_ms, swarm_stream_cb cb, void *ctx) {
    /* Build poll array */
    struct pollfd fds[SWARM_MAX_CHILDREN * 2];
    int fd_map[SWARM_MAX_CHILDREN * 2]; /* maps pollfd index to child index */
    int nfds = 0;

    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        if (c->status != SWARM_RUNNING && c->status != SWARM_STREAMING) continue;

        if (c->pipe_fd >= 0) {
            fds[nfds].fd = c->pipe_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            fd_map[nfds] = i;
            nfds++;
        }
        if (c->err_fd >= 0) {
            fds[nfds].fd = c->err_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            fd_map[nfds] = i;
            nfds++;
        }
    }

    if (nfds == 0) return 0;

    int ret = poll(fds, nfds, timeout_ms);
    if (ret <= 0) return ret;

    int events = 0;
    for (int i = 0; i < nfds; i++) {
        if (fds[i].revents & (POLLIN | POLLHUP)) {
            swarm_child_t *c = &s->children[fd_map[i]];
            child_read(c, fds[i].fd, cb, ctx);
            events++;
        }
    }

    /* Check for completed children */
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        if (c->status != SWARM_RUNNING && c->status != SWARM_STREAMING) continue;

        int status;
        pid_t result = waitpid(c->pid, &status, WNOHANG);
        if (result > 0) {
            /* Drain remaining output */
            if (c->pipe_fd >= 0) child_read(c, c->pipe_fd, cb, ctx);
            if (c->err_fd >= 0) child_read(c, c->err_fd, cb, ctx);

            close(c->pipe_fd); c->pipe_fd = -1;
            close(c->err_fd); c->err_fd = -1;

            c->end_time = now_sec();
            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->status = (c->exit_code == 0) ? SWARM_DONE : SWARM_ERROR;
            } else {
                c->status = SWARM_KILLED;
                c->exit_code = -1;
            }
        }
    }

    return events;
}

/* ── Status ───────────────────────────────────────────────────────────── */

swarm_child_t *swarm_get(swarm_t *s, int child_id) {
    if (child_id < 0 || child_id >= s->child_count) return NULL;
    return &s->children[child_id];
}

const char *swarm_status_str(swarm_status_t st) {
    switch (st) {
        case SWARM_PENDING:   return "pending";
        case SWARM_RUNNING:   return "running";
        case SWARM_STREAMING: return "streaming";
        case SWARM_DONE:      return "done";
        case SWARM_ERROR:     return "error";
        case SWARM_KILLED:    return "killed";
    }
    return "unknown";
}

int swarm_active_count(swarm_t *s) {
    int count = 0;
    for (int i = 0; i < s->child_count; i++) {
        if (s->children[i].status == SWARM_RUNNING ||
            s->children[i].status == SWARM_STREAMING)
            count++;
    }
    return count;
}

bool swarm_kill(swarm_t *s, int child_id) {
    swarm_child_t *c = swarm_get(s, child_id);
    if (!c) return false;
    if (c->status != SWARM_RUNNING && c->status != SWARM_STREAMING) return false;
    kill(c->pid, SIGTERM);
    c->status = SWARM_KILLED;
    c->end_time = now_sec();
    return true;
}

void swarm_group_kill(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count) return;
    swarm_group_t *g = &s->groups[group_id];
    for (int i = 0; i < g->child_count; i++) {
        swarm_kill(s, g->child_ids[i]);
    }
}

/* ── JSON output ──────────────────────────────────────────────────────── */

int swarm_status_json(swarm_t *s, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"swarm\":{\"children\":");
    jbuf_append_int(&b, s->child_count);
    jbuf_append(&b, ",\"active\":");
    jbuf_append_int(&b, swarm_active_count(s));
    jbuf_append(&b, ",\"groups\":");
    jbuf_append_int(&b, s->group_count);

    jbuf_append(&b, ",\"processes\":[");
    for (int i = 0; i < s->child_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        swarm_child_t *c = &s->children[i];
        jbuf_append(&b, "{\"id\":");
        jbuf_append_int(&b, c->id);
        jbuf_append(&b, ",\"pid\":");
        jbuf_append_int(&b, (int)c->pid);
        jbuf_append(&b, ",\"status\":");
        jbuf_append_json_str(&b, swarm_status_str(c->status));
        jbuf_append(&b, ",\"task\":");
        jbuf_append_json_str(&b, c->task);
        if (c->group_id >= 0) {
            jbuf_append(&b, ",\"group\":");
            jbuf_append_int(&b, c->group_id);
        }
        double elapsed = (c->end_time > 0 ? c->end_time : now_sec()) - c->start_time;
        char elapsed_str[32];
        snprintf(elapsed_str, sizeof(elapsed_str), "%.1f", elapsed);
        jbuf_append(&b, ",\"elapsed_sec\":");
        jbuf_append(&b, elapsed_str);
        jbuf_append(&b, ",\"output_bytes\":");
        jbuf_append_int(&b, (int)c->output_len);
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "],\"group_details\":[");
    for (int i = 0; i < s->group_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        swarm_group_t *g = &s->groups[i];
        jbuf_append(&b, "{\"id\":");
        jbuf_append_int(&b, g->id);
        jbuf_append(&b, ",\"name\":");
        jbuf_append_json_str(&b, g->name);
        jbuf_append(&b, ",\"children\":");
        jbuf_append_int(&b, g->child_count);
        jbuf_append(&b, ",\"complete\":");
        jbuf_append(&b, swarm_group_complete(s, i) ? "true" : "false");
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

int swarm_child_output(swarm_t *s, int child_id, char *buf, size_t len) {
    swarm_child_t *c = swarm_get(s, child_id);
    if (!c) {
        snprintf(buf, len, "{\"error\":\"invalid child_id\"}");
        return (int)strlen(buf);
    }

    /* First, poll for any new data */
    swarm_poll(s, 0);

    jbuf_t b;
    jbuf_init(&b, c->output_len + 256);

    jbuf_append(&b, "{\"id\":");
    jbuf_append_int(&b, c->id);
    jbuf_append(&b, ",\"status\":");
    jbuf_append_json_str(&b, swarm_status_str(c->status));
    jbuf_append(&b, ",\"output\":");
    jbuf_append_json_str(&b, c->output ? c->output : "");
    jbuf_append(&b, "}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

int swarm_group_status_json(swarm_t *s, int group_id, char *buf, size_t len) {
    if (group_id < 0 || group_id >= s->group_count) {
        snprintf(buf, len, "{\"error\":\"invalid group_id\"}");
        return (int)strlen(buf);
    }

    swarm_poll(s, 0);

    swarm_group_t *g = &s->groups[group_id];
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"group\":");
    jbuf_append_json_str(&b, g->name);
    jbuf_append(&b, ",\"complete\":");
    jbuf_append(&b, swarm_group_complete(s, group_id) ? "true" : "false");
    jbuf_append(&b, ",\"children\":[");

    for (int i = 0; i < g->child_count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        swarm_child_t *c = &s->children[g->child_ids[i]];
        jbuf_append(&b, "{\"id\":");
        jbuf_append_int(&b, c->id);
        jbuf_append(&b, ",\"status\":");
        jbuf_append_json_str(&b, swarm_status_str(c->status));
        jbuf_append(&b, ",\"task\":");
        jbuf_append_json_str(&b, c->task);

        /* Last 200 chars of output as preview */
        if (c->output_len > 0) {
            const char *preview = c->output;
            size_t plen = c->output_len;
            if (plen > 200) {
                preview = c->output + c->output_len - 200;
                plen = 200;
            }
            jbuf_append(&b, ",\"output_tail\":");
            jbuf_append_json_str(&b, preview);
        }
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "]}");

    int written = (int)b.len < (int)len - 1 ? (int)b.len : (int)len - 1;
    memcpy(buf, b.data, written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}
