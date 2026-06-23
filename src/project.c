#include "project.h"
#include "crypto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Forward — provided by project_mux.c. Defining it here as weak keeps the
 * spawn path optional (smoke-tests can link project.c standalone). */
__attribute__((weak)) int dsco_mux_spawn_worker(dsco_project_t *p, const char *api_key);
__attribute__((weak)) int dsco_mux_kill_worker(dsco_project_t *p);

/* ──────────────────────────────────────────────────────────────────────────
 *  Helpers
 * ────────────────────────────────────────────────────────────────────────── */

static int64_t now_unix(void) {
    return (int64_t)time(NULL);
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    if (n == 0)
        return -1;
    if (tmp[n - 1] == '/')
        tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/tmp";
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Ring buffer
 * ────────────────────────────────────────────────────────────────────────── */

void dsco_ring_init(dsco_ring_t *r, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->buf = (char *)calloc(1, cap);
    r->cap = r->buf ? cap : 0;
    pthread_mutex_init(&r->mu, NULL);
}

void dsco_ring_free(dsco_ring_t *r) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mu);
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = 0;
    r->wrapped = false;
    pthread_mutex_unlock(&r->mu);
    pthread_mutex_destroy(&r->mu);
}

size_t dsco_ring_write(dsco_ring_t *r, const char *data, size_t len) {
    if (!r || !r->buf || r->cap == 0 || !data || len == 0)
        return 0;
    pthread_mutex_lock(&r->mu);
    if (len >= r->cap) {
        /* keep only tail */
        memcpy(r->buf, data + (len - r->cap) + 1, r->cap - 1);
        r->head = r->cap - 1;
        r->tail = 0;
        r->wrapped = false;
        r->total_written += len;
        pthread_mutex_unlock(&r->mu);
        return len;
    }
    size_t first = r->cap - r->head;
    if (first >= len) {
        memcpy(r->buf + r->head, data, len);
        r->head += len;
        if (r->head == r->cap) {
            r->head = 0;
            r->wrapped = true;
        }
    } else {
        memcpy(r->buf + r->head, data, first);
        memcpy(r->buf, data + first, len - first);
        r->head = len - first;
        r->wrapped = true;
    }
    if (r->wrapped)
        r->tail = r->head; /* oldest byte moves with head */
    r->total_written += len;
    pthread_mutex_unlock(&r->mu);
    return len;
}

size_t dsco_ring_snapshot(dsco_ring_t *r, char *out, size_t out_cap) {
    if (!r || !out || out_cap == 0)
        return 0;
    pthread_mutex_lock(&r->mu);
    size_t n;
    if (!r->wrapped) {
        n = r->head;
        if (n >= out_cap)
            n = out_cap - 1;
        memcpy(out, r->buf, n);
    } else {
        size_t a = r->cap - r->tail;
        size_t b = r->head;
        size_t total = a + b;
        size_t skip = 0;
        if (total >= out_cap)
            skip = total - (out_cap - 1);
        n = 0;
        if (skip < a) {
            size_t take_a = a - skip;
            memcpy(out, r->buf + r->tail + skip, take_a);
            n += take_a;
            memcpy(out + n, r->buf, b);
            n += b;
        } else {
            size_t skip_b = skip - a;
            size_t take_b = b > skip_b ? b - skip_b : 0;
            memcpy(out, r->buf + skip_b, take_b);
            n = take_b;
        }
    }
    out[n] = '\0';
    pthread_mutex_unlock(&r->mu);
    return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Registry root
 * ────────────────────────────────────────────────────────────────────────── */

static char s_registry_root[PATH_MAX] = {0};

const char *dsco_project_registry_root(void) {
    if (s_registry_root[0] == '\0') {
        snprintf(s_registry_root, sizeof(s_registry_root), "%s/.dsco/projects", home_dir());
    }
    return s_registry_root;
}

int dsco_project_registry_ensure(void) {
    return mkdir_p(dsco_project_registry_root());
}

static void project_dir(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", dsco_project_registry_root(), id);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Persistence — project.toml (flat KV, our own minimal writer/reader)
 * ────────────────────────────────────────────────────────────────────────── */

static int write_toml(const dsco_project_t *p) {
    char dir[PATH_MAX], path[PATH_MAX];
    project_dir(p->id.id, dir, sizeof(dir));
    if (mkdir_p(dir) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s/project.toml", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "id = \"%s\"\n", p->id.id);
    fprintf(f, "name = \"%s\"\n", p->id.name);
    fprintf(f, "root = \"%s\"\n", p->id.root);
    fprintf(f, "workspace = \"%s\"\n", p->id.workspace);
    fprintf(f, "model = \"%s\"\n", p->model[0] ? p->model : "");
    fprintf(f, "created_at = %lld\n", (long long)p->created_at);
    fprintf(f, "started_at = %lld\n", (long long)p->started_at);
    fprintf(f, "token_cap = %llu\n", (unsigned long long)p->budget.token_cap);
    fprintf(f, "cents_cap = %llu\n", (unsigned long long)p->budget.cents_cap);
    fprintf(f, "tokens_used = %llu\n", (unsigned long long)p->budget.tokens_used);
    fprintf(f, "cents_spent = %llu\n", (unsigned long long)p->budget.cents_spent);
    fprintf(f, "allow_global_fs = %s\n", p->policy.allow_global_fs ? "true" : "false");
    fprintf(f, "allow_network = %s\n", p->policy.allow_network ? "true" : "false");
    fprintf(f, "allow_shell = %s\n", p->policy.allow_shell ? "true" : "false");
    fclose(f);
    return 0;
}

static void unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int read_toml(const char *id, dsco_project_t *p) {
    char dir[PATH_MAX], path[PATH_MAX];
    project_dir(id, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/project.toml", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        while (*k && isspace((unsigned char)*k))
            k++;
        char *ke = k + strlen(k);
        while (ke > k && isspace((unsigned char)ke[-1]))
            *--ke = '\0';
        while (*v && isspace((unsigned char)*v))
            v++;
        char *ve = v + strlen(v);
        while (ve > v && (isspace((unsigned char)ve[-1]) || ve[-1] == '\n'))
            *--ve = '\0';
        unquote(v);
        if (!strcmp(k, "id"))
            snprintf(p->id.id, sizeof(p->id.id), "%s", v);
        else if (!strcmp(k, "name"))
            snprintf(p->id.name, sizeof(p->id.name), "%s", v);
        else if (!strcmp(k, "root"))
            snprintf(p->id.root, sizeof(p->id.root), "%s", v);
        else if (!strcmp(k, "workspace"))
            snprintf(p->id.workspace, sizeof(p->id.workspace), "%s", v);
        else if (!strcmp(k, "model"))
            snprintf(p->model, sizeof(p->model), "%s", v);
        else if (!strcmp(k, "created_at"))
            p->created_at = (int64_t)strtoll(v, NULL, 10);
        else if (!strcmp(k, "started_at"))
            p->started_at = (int64_t)strtoll(v, NULL, 10);
        else if (!strcmp(k, "token_cap"))
            p->budget.token_cap = (uint64_t)strtoull(v, NULL, 10);
        else if (!strcmp(k, "cents_cap"))
            p->budget.cents_cap = (uint64_t)strtoull(v, NULL, 10);
        else if (!strcmp(k, "tokens_used"))
            p->budget.tokens_used = (uint64_t)strtoull(v, NULL, 10);
        else if (!strcmp(k, "cents_spent"))
            p->budget.cents_spent = (uint64_t)strtoull(v, NULL, 10);
        else if (!strcmp(k, "allow_global_fs"))
            p->policy.allow_global_fs = !strcmp(v, "true");
        else if (!strcmp(k, "allow_network"))
            p->policy.allow_network = !strcmp(v, "true");
        else if (!strcmp(k, "allow_shell"))
            p->policy.allow_shell = !strcmp(v, "true");
    }
    fclose(f);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  registry.idx — flat index of all known projects
 *  Format per line: ULID<TAB>NAME<TAB>ROOT<TAB>FLAGS<TAB>LAST_OPENED
 *  FLAGS: comma-separated. Currently just "archived" or empty.
 * ────────────────────────────────────────────────────────────────────────── */

static const char *index_path(void) {
    static char p[PATH_MAX] = {0};
    if (!p[0])
        snprintf(p, sizeof(p), "%s/registry.idx", dsco_project_registry_root());
    return p;
}

static int index_upsert(const dsco_project_t *p, bool archived) {
    if (dsco_project_registry_ensure() != 0)
        return -1;
    FILE *in = fopen(index_path(), "r");
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", index_path());
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in)
            fclose(in);
        return -1;
    }
    bool replaced = false;
    if (in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, p->id.id, strlen(p->id.id)) && line[strlen(p->id.id)] == '\t') {
                replaced = true;
                fprintf(out, "%s\t%s\t%s\t%s\t%lld\n", p->id.id, p->id.name, p->id.root,
                        archived ? "archived" : "", (long long)now_unix());
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!replaced) {
        fprintf(out, "%s\t%s\t%s\t%s\t%lld\n", p->id.id, p->id.name, p->id.root,
                archived ? "archived" : "", (long long)now_unix());
    }
    fclose(out);
    return rename(tmp, index_path());
}

/* ──────────────────────────────────────────────────────────────────────────
 *  CRUD
 * ────────────────────────────────────────────────────────────────────────── */

static void derive_name(const char *root, char *out, size_t cap) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", root);
    char *b = basename(tmp);
    snprintf(out, cap, "%s", (b && *b) ? b : "project");
}

int dsco_project_create(const char *root, const char *name, dsco_project_t **out) {
    if (!root || !out)
        return -1;
    if (dsco_project_registry_ensure() != 0)
        return -1;

    dsco_project_t *p = (dsco_project_t *)calloc(1, sizeof(*p));
    if (!p)
        return -1;

    /* normalize root to absolute */
    if (root[0] == '/') {
        snprintf(p->id.root, sizeof(p->id.root), "%s", root);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            free(p);
            return -1;
        }
        snprintf(p->id.root, sizeof(p->id.root), "%s/%s", cwd, root);
    }
    snprintf(p->id.workspace, sizeof(p->id.workspace), "%s/.dsco", p->id.root);

    if (name && *name) {
        snprintf(p->id.name, sizeof(p->id.name), "%s", name);
    } else {
        derive_name(p->id.root, p->id.name, sizeof(p->id.name));
    }

    uuid_v4(p->id.id);
    p->state = DSCO_PROJECT_IDLE;
    p->created_at = now_unix();
    p->worker_pid = -1;
    p->worker_in_fd = -1;
    p->worker_out_fd = -1;
    p->epoch = 1;
    /* sensible defaults */
    p->budget.token_cap = 2000000;
    p->budget.cents_cap = 5000; /* $50 */
    p->budget.rpm_cap = 60;
    p->policy.allow_global_fs = false;
    p->policy.allow_network = true;
    p->policy.allow_shell = true;
    p->policy.allow_mcp_default = true;
    snprintf(p->model, sizeof(p->model), "claude-opus-4-7");
    dsco_ring_init(&p->scrollback, DSCO_PROJECT_RING_BYTES);

    if (write_toml(p) != 0 || index_upsert(p, false) != 0) {
        dsco_project_free(p);
        return -1;
    }
    *out = p;
    return 0;
}

static int load_from_id(const char *id, dsco_project_t **out) {
    dsco_project_t *p = (dsco_project_t *)calloc(1, sizeof(*p));
    if (!p)
        return -1;
    if (read_toml(id, p) != 0) {
        free(p);
        return -1;
    }
    p->state = DSCO_PROJECT_IDLE;
    p->worker_pid = -1;
    p->worker_in_fd = -1;
    p->worker_out_fd = -1;
    p->epoch = 1;
    dsco_ring_init(&p->scrollback, DSCO_PROJECT_RING_BYTES);
    *out = p;
    return 0;
}

int dsco_project_open(const char *key, dsco_project_t **out) {
    if (!key || !out)
        return -1;
    if (dsco_project_registry_ensure() != 0)
        return -1;

    /* read index */
    FILE *f = fopen(index_path(), "r");
    if (!f)
        return -1;
    char line[4096];
    char hit_id[DSCO_PROJECT_ID_LEN] = {0};
    size_t klen = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        char *id = line;
        char *t1 = strchr(line, '\t');
        if (!t1)
            continue;
        *t1 = '\0';
        char *name = t1 + 1;
        char *t2 = strchr(name, '\t');
        if (!t2)
            continue;
        *t2 = '\0';
        char *root = t2 + 1;
        char *t3 = strchr(root, '\t');
        if (t3)
            *t3 = '\0';

        if (!strncmp(id, key, klen) || /* ULID/UUID prefix match */
            !strcmp(name, key) || !strcmp(root, key)) {
            snprintf(hit_id, sizeof(hit_id), "%s", id);
            break;
        }
    }
    fclose(f);
    if (!hit_id[0])
        return -1;
    return load_from_id(hit_id, out);
}

int dsco_project_save(const dsco_project_t *p) {
    if (!p)
        return -1;
    if (write_toml(p) != 0)
        return -1;
    return index_upsert(p, false);
}

int dsco_project_archive(const char *id) {
    if (!id)
        return -1;
    dsco_project_t *p = NULL;
    if (load_from_id(id, &p) != 0)
        return -1;
    int rc = index_upsert(p, true);
    dsco_project_free(p);
    return rc;
}

void dsco_project_free(dsco_project_t *p) {
    if (!p)
        return;
    dsco_ring_free(&p->scrollback);
    if (p->worker_in_fd >= 0)
        close(p->worker_in_fd);
    if (p->worker_out_fd >= 0)
        close(p->worker_out_fd);
    free(p);
}

int dsco_project_close(dsco_project_t *p) {
    if (!p)
        return -1;
    if (p->worker_pid > 0)
        dsco_project_kill(p);
    dsco_project_save(p);
    dsco_project_free(p);
    return 0;
}

int dsco_project_list(dsco_project_summary_t *out, int max, bool include_archived) {
    if (!out || max <= 0)
        return 0;
    FILE *f = fopen(index_path(), "r");
    if (!f)
        return 0;
    char line[4096];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        char *id = line;
        char *t1 = strchr(line, '\t');
        if (!t1)
            continue;
        *t1 = '\0';
        char *name = t1 + 1;
        char *t2 = strchr(name, '\t');
        if (!t2)
            continue;
        *t2 = '\0';
        char *root = t2 + 1;
        char *t3 = strchr(root, '\t');
        if (!t3)
            continue;
        *t3 = '\0';
        char *flags = t3 + 1;
        char *t4 = strchr(flags, '\t');
        if (t4)
            *t4 = '\0';
        bool archived = strstr(flags, "archived") != NULL;
        if (archived && !include_archived)
            continue;
        snprintf(out[n].id, sizeof(out[n].id), "%s", id);
        snprintf(out[n].name, sizeof(out[n].name), "%s", name);
        snprintf(out[n].root, sizeof(out[n].root), "%s", root);
        out[n].archived = archived;
        out[n].last_opened_at = t4 ? (int64_t)strtoll(t4 + 1, NULL, 10) : 0;
        n++;
    }
    fclose(f);
    return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Lifecycle
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_project_start(dsco_project_t *p, const char *api_key) {
    if (!p)
        return -1;
    if (p->state == DSCO_PROJECT_RUNNING)
        return 0;
    if (!dsco_mux_spawn_worker) {
        /* mux not linked — cannot spawn workers */
        return -1;
    }
    int rc = dsco_mux_spawn_worker(p, api_key);
    if (rc == 0) {
        p->state = DSCO_PROJECT_RUNNING;
        p->started_at = now_unix();
        p->epoch++;
    }
    return rc;
}

int dsco_project_pause(dsco_project_t *p) {
    if (!p || p->worker_pid <= 0)
        return -1;
    if (kill(p->worker_pid, SIGSTOP) != 0)
        return -1;
    p->state = DSCO_PROJECT_PAUSED;
    p->epoch++;
    return 0;
}

int dsco_project_resume(dsco_project_t *p) {
    if (!p || p->worker_pid <= 0)
        return -1;
    if (kill(p->worker_pid, SIGCONT) != 0)
        return -1;
    p->state = DSCO_PROJECT_RUNNING;
    p->epoch++;
    return 0;
}

int dsco_project_kill(dsco_project_t *p) {
    if (!p)
        return -1;
    if (dsco_mux_kill_worker) {
        int rc = dsco_mux_kill_worker(p);
        if (rc == 0) {
            p->state = DSCO_PROJECT_DEAD;
            p->epoch++;
        }
        return rc;
    }
    if (p->worker_pid > 0) {
        kill(p->worker_pid, SIGTERM);
        int status = 0;
        waitpid(p->worker_pid, &status, 0);
        p->worker_pid = -1;
    }
    if (p->worker_in_fd >= 0) {
        close(p->worker_in_fd);
        p->worker_in_fd = -1;
    }
    if (p->worker_out_fd >= 0) {
        close(p->worker_out_fd);
        p->worker_out_fd = -1;
    }
    p->state = DSCO_PROJECT_DEAD;
    p->epoch++;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  I/O surface
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_project_send_input(dsco_project_t *p, const char *line) {
    if (!p || p->worker_in_fd < 0 || !line)
        return -1;
    size_t n = strlen(line);
    if (write(p->worker_in_fd, line, n) < 0)
        return -1;
    if (n == 0 || line[n - 1] != '\n')
        if (write(p->worker_in_fd, "\n", 1) < 0)
            return -1;
    return 0;
}

int dsco_project_drain_output(dsco_project_t *p) {
    if (!p || p->worker_out_fd < 0)
        return 0;
    char buf[4096];
    int total = 0;
    for (;;) {
        ssize_t n = read(p->worker_out_fd, buf, sizeof(buf));
        if (n > 0) {
            dsco_ring_write(&p->scrollback, buf, (size_t)n);
            atomic_store_explicit(&p->has_unread, 1, memory_order_relaxed);
            total += (int)n;
            if (n < (ssize_t)sizeof(buf))
                break; /* drained for now */
        } else if (n == 0) {
            /* EOF — worker exited */
            p->state = DSCO_PROJECT_DEAD;
            p->epoch++;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }
    }
    return total;
}

int dsco_project_poll_fd(const dsco_project_t *p) {
    return p ? p->worker_out_fd : -1;
}

size_t dsco_project_snapshot(dsco_project_t *p, char *out, size_t out_cap) {
    if (!p || !out || out_cap == 0)
        return 0;
    return dsco_ring_snapshot(&p->scrollback, out, out_cap);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

double dsco_project_activity_bps(dsco_project_t *p) {
    if (!p)
        return 0.0;
    int64_t now = now_ms();
    int64_t dt_ms = (p->activity_last_sample_ms > 0) ? (now - p->activity_last_sample_ms) : 100;
    if (dt_ms <= 0)
        dt_ms = 1;
    p->activity_last_sample_ms = now;

    unsigned long long bytes =
        atomic_exchange_explicit(&p->activity_bytes, 0ULL, memory_order_acq_rel);
    double bps = (double)bytes * 1000.0 / (double)dt_ms;

    /* EWMA, ~1 second time constant */
    double alpha = (double)dt_ms / 1000.0;
    if (alpha > 1.0)
        alpha = 1.0;
    p->activity_ewma = p->activity_ewma * (1.0 - alpha) + bps * alpha;

    /* sparkline ring */
    p->activity_ring_head = (p->activity_ring_head + 1) & 15;
    p->activity_ring[p->activity_ring_head] = bps;
    return p->activity_ewma;
}

/* Returns the sparkline ring in chronological order (oldest at index 0). */
void dsco_project_activity_ring(dsco_project_t *p, double *out16) {
    if (!p || !out16)
        return;
    int head = p->activity_ring_head;
    for (int i = 0; i < 16; i++) {
        out16[i] = p->activity_ring[(head + 1 + i) & 15];
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Helpers
 * ────────────────────────────────────────────────────────────────────────── */

const char *dsco_project_state_name(dsco_project_state_t s) {
    switch (s) {
        case DSCO_PROJECT_IDLE:
            return "IDLE";
        case DSCO_PROJECT_RUNNING:
            return "RUNNING";
        case DSCO_PROJECT_PAUSED:
            return "PAUSED";
        case DSCO_PROJECT_QUARANTINED:
            return "QUARANTINED";
        case DSCO_PROJECT_CLOSED:
            return "CLOSED";
        case DSCO_PROJECT_DEAD:
            return "DEAD";
    }
    return "?";
}
