#include "swarm.h"
#include "config.h"
#include "provider.h"
#include "router.h"
#include "llm.h"
#include "json_util.h"
#include "tui.h"
#include "pets.h"
#include "scheduler.h"
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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#include <sys/event.h> /* kqueue */
#endif

/* ── Forward declarations ─────────────────────────────────────────────── */

static void parse_child_cost_report(swarm_child_t *c);

static void swarm_export_child_credential(const char *model, const char *credential) {
    provider_export_child_process_credentials(model, credential);
}

static void swarm_export_child_credential_for_provider(const char *provider,
                                                       const char *credential) {
    provider_export_child_process_credentials_for_provider(provider, credential);
}

/* ── Bitset helpers ───────────────────────────────────────────────────── */

static inline void bitset_set(swarm_bitset_t *bs, int i) {
    if (i < 0 || i >= 64)
        return;
    if (!(bs->bits & (1ULL << i))) {
        bs->bits |= (1ULL << i);
        bs->count++;
    }
}

static inline void bitset_clear(swarm_bitset_t *bs, int i) {
    if (i < 0 || i >= 64)
        return;
    if (bs->bits & (1ULL << i)) {
        bs->bits &= ~(1ULL << i);
        bs->count--;
    }
}

static __attribute__((unused)) inline bool bitset_test(const swarm_bitset_t *bs, int i) {
    return i >= 0 && i < 64 && (bs->bits & (1ULL << i));
}

/* ── Completion queue (ring buffer) ───────────────────────────────────── */

static void cq_push(swarm_completion_q_t *q, int id) {
    if (q->count >= SWARM_MAX_CHILDREN)
        return; /* full — should never happen */
    q->ids[q->tail] = id;
    q->tail = (q->tail + 1) % SWARM_MAX_CHILDREN;
    q->count++;
}

static int cq_pop(swarm_completion_q_t *q) {
    if (q->count <= 0)
        return -1;
    int id = q->ids[q->head];
    q->head = (q->head + 1) % SWARM_MAX_CHILDREN;
    q->count--;
    return id;
}

/* ── kqueue helpers ───────────────────────────────────────────────────── */

#ifdef __APPLE__
static void kq_register_fd(int kq, int fd, int child_id) {
    if (kq < 0 || fd < 0)
        return;
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *)(intptr_t)child_id);
    kevent(kq, &ev, 1, NULL, 0, NULL);
}

static void kq_unregister_fd(int kq, int fd) {
    if (kq < 0 || fd < 0)
        return;
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);
}
#endif

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

    /* Find our own binary — needed to fork sub-agent processes */
    char self[4096];
    self[0] = '\0';

#ifdef __APPLE__
    /* macOS: _NSGetExecutablePath gives us the actual binary path */
    uint32_t size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) == 0) {
        /* Resolve symlinks to get canonical path */
        char *resolved = realpath(self, NULL);
        if (resolved) {
            s->dsco_path = resolved; /* already malloc'd by realpath */
        } else {
            s->dsco_path = safe_strdup(self);
        }
    } else
#endif
    {
        /* Linux: /proc/self/exe */
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len > 0) {
            self[len] = '\0';
            s->dsco_path = safe_strdup(self);
        } else {
            /* Last resort: try to find dsco relative to cwd or via PATH */
            char cwd[2048];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(self, sizeof(self), "%s/dsco", cwd);
                if (access(self, X_OK) == 0) {
                    s->dsco_path = safe_strdup(self);
                } else {
                    s->dsco_path = safe_strdup("dsco");
                }
            } else {
                s->dsco_path = safe_strdup("dsco");
            }
        }
    }

    /* Initialize fast-path structures */
    memset(&s->done_q, 0, sizeof(s->done_q));
    memset(&s->active, 0, sizeof(s->active));
    s->first_completion_time = 0;
#ifdef __APPLE__
    s->kq_fd = kqueue();
#else
    s->kq_fd = -1;
#endif
}

void swarm_destroy(swarm_t *s) {
    /* Kill all running children — SIGTERM first, then SIGKILL after grace period */
    bool signaled = false;
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING) {
            kill(-c->pid, SIGTERM); /* kill process group */
            signaled = true;
        }
    }

    /* Brief grace period only when a child was asked to stop. Completed
     * swarms/topology stages should not pay a fixed 200ms teardown tax. */
    if (signaled)
        usleep(200000);

    /* Force-kill any still running, then blocking reap to avoid zombies */
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING) {
            int status;
            pid_t w = waitpid(c->pid, &status, WNOHANG);
            if (w == 0) {
                /* Still alive — force kill */
                kill(-c->pid, SIGKILL);
                waitpid(c->pid, NULL, 0); /* blocking reap */
            }
        }
        /* Close pipes AFTER reap to prevent use-after-free in child_read */
        if (c->pipe_fd >= 0) {
            close(c->pipe_fd);
            c->pipe_fd = -1;
        }
        if (c->err_fd >= 0) {
            close(c->err_fd);
            c->err_fd = -1;
        }
        free(c->output);
        c->output = NULL;
        free(c->stream_buf);
        c->stream_buf = NULL;
    }
    free((void *)s->dsco_path);
    s->dsco_path = NULL;
#ifdef __APPLE__
    if (s->kq_fd >= 0) {
        close(s->kq_fd);
        s->kq_fd = -1;
    }
#endif
}

/* ── Post-spawn hook: register child with kqueue + bitset ─────────────── */

static void post_spawn_register(swarm_t *s, int child_id) {
    bitset_set(&s->active, child_id);
    /* Hatch a companion pet for this background agent. Seeded by the task so
     * the same task always gets the same creature. */
    {
        swarm_child_t *pc = &s->children[child_id];
        pet_roster_upsert(pet_roster_global(), child_id, -1, pc->task, pc->task, PET_ST_WORKING);
    }
#ifdef __APPLE__
    swarm_child_t *c = &s->children[child_id];
    kq_register_fd(s->kq_fd, c->pipe_fd, child_id);
    if (c->err_fd >= 0)
        kq_register_fd(s->kq_fd, c->err_fd, child_id);
#endif
}

/* ── Post-completion hook: update bitset, push to completion queue ────── */

static void post_complete(swarm_t *s, int child_id) {
    bitset_clear(&s->active, child_id);
    cq_push(&s->done_q, child_id);
    if (s->first_completion_time == 0)
        s->first_completion_time = now_sec();
    /* Update the pet's status + cost; the main loop drains DONE/ERROR pets and
     * fires a mini-notification (see drain_pet_notifications in agent.c). */
    {
        swarm_child_t *pc = &s->children[child_id];
        double cost = pc->reported_cost_usd > 0 ? pc->reported_cost_usd : pc->est_cost_usd;
        pet_roster_set_status(pet_roster_global(), child_id,
                              pc->status == SWARM_DONE ? PET_ST_DONE : PET_ST_ERROR, cost);
    }
#ifdef __APPLE__
    swarm_child_t *c = &s->children[child_id];
    kq_unregister_fd(s->kq_fd, c->pipe_fd);
    kq_unregister_fd(s->kq_fd, c->err_fd);
#endif
}

/* ── Spawn ────────────────────────────────────────────────────────────── */

int swarm_spawn(swarm_t *s, const char *task, const char *model) {
    return swarm_spawn_in_group(s, -1, task, model);
}

/* Optional per-spawn model-instance spec. The caller sets this immediately
 * before swarm_spawn*(); the freshly-forked child applies it as DSCO_* env so
 * it wraps a distinct model instance, then the parent clears it so it never
 * leaks to the next child. */
static struct {
    bool set;
    char effort[16];
    double temperature, top_p;
    int top_k, thinking_budget;
    char tool_choice[128];
    char *system_prompt;
} s_next_instance;

void swarm_set_next_instance(const char *effort, double temperature, double top_p, int top_k,
                             int thinking_budget, const char *tool_choice,
                             const char *system_prompt) {
    free(s_next_instance.system_prompt);
    memset(&s_next_instance, 0, sizeof(s_next_instance));
    s_next_instance.set = true;
    if (effort)
        snprintf(s_next_instance.effort, sizeof(s_next_instance.effort), "%s", effort);
    s_next_instance.temperature = temperature;
    s_next_instance.top_p = top_p;
    s_next_instance.top_k = top_k;
    s_next_instance.thinking_budget = thinking_budget;
    if (tool_choice)
        snprintf(s_next_instance.tool_choice, sizeof(s_next_instance.tool_choice), "%s",
                 tool_choice);
    if (system_prompt)
        s_next_instance.system_prompt = strdup(system_prompt);
}

/* Child-side: export the pending instance spec as env before execl. */
static void swarm_apply_instance_env(void) {
    if (!s_next_instance.set)
        return;
    char b[32];
    if (s_next_instance.effort[0])
        setenv("DSCO_EFFORT", s_next_instance.effort, 1);
    if (s_next_instance.temperature >= 0) {
        snprintf(b, sizeof b, "%.4f", s_next_instance.temperature);
        setenv("DSCO_TEMPERATURE", b, 1);
    }
    if (s_next_instance.top_p >= 0) {
        snprintf(b, sizeof b, "%.4f", s_next_instance.top_p);
        setenv("DSCO_TOP_P", b, 1);
    }
    if (s_next_instance.top_k > 0) {
        snprintf(b, sizeof b, "%d", s_next_instance.top_k);
        setenv("DSCO_TOP_K", b, 1);
    }
    if (s_next_instance.thinking_budget > 0) {
        snprintf(b, sizeof b, "%d", s_next_instance.thinking_budget);
        setenv("DSCO_THINKING_BUDGET", b, 1);
    }
    if (s_next_instance.tool_choice[0])
        setenv("DSCO_TOOL_CHOICE", s_next_instance.tool_choice, 1);
    if (s_next_instance.system_prompt)
        setenv("DSCO_SYSTEM_PROMPT", s_next_instance.system_prompt, 1);
}

/* Parent-side: drop the spec so it does not bleed into the next spawn. */
static void swarm_clear_next_instance(void) {
    if (!s_next_instance.set)
        return;
    free(s_next_instance.system_prompt);
    memset(&s_next_instance, 0, sizeof(s_next_instance));
}

int swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model) {
    if (s->child_count >= SWARM_MAX_CHILDREN)
        return -1;

    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    int id = s->child_count;

    /* Compute depth before fork so parent can record it */
    const char *cur_depth_env = getenv("DSCO_SWARM_DEPTH");
    int depth = cur_depth_env ? atoi(cur_depth_env) + 1 : 1;

    if (pid == 0) {
        /* ── Child process ── */
        close(stdout_pipe[0]);

        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO); /* merge stderr into stdout */
        close(stdout_pipe[1]);

        /* New process group for clean kill */
        setpgid(0, 0);

        /* Validate model against registry — LLMs sometimes hallucinate
           model names (e.g. "claude-3-5-sonnet-20241022" which is gone).
           Fall back to parent's model if the requested one is unknown. */
        const char *m = model ? model : s->default_model;
        if (m && m[0]) {
            const char *resolved = model_resolve_alias(m);
            if (resolved == m && !model_lookup(m)) {
                fprintf(stdout, "swarm: unknown model '%s', falling back to '%s'\n", m,
                        s->default_model);
                m = s->default_model;
            } else {
                m = resolved;
            }
        }
        if (!m || !m[0])
            m = s->default_model;
        const char *bin = s->dsco_path;

        /* Ensure child inherits the exact credential/auth mode the parent resolved.
         * Also resolve credentials for the child's model/provider — if the child
         * uses a different provider (e.g. openrouter), export that provider's key too. */
        swarm_export_child_credential(m, s->api_key);

        /* ── If the child's model routes to a different provider than the parent,
         * make sure that provider's key is also exported.  This handles the case
         * where the parent runs on Anthropic but the child model is on OpenRouter. */
        {
            const char *child_provider = provider_detect(m, s->api_key);
            const char *parent_provider = provider_detect(s->default_model, s->api_key);
            if (child_provider && parent_provider && strcmp(child_provider, parent_provider) != 0) {
                const char *child_key = provider_resolve_api_key(child_provider);
                if (child_key && child_key[0]) {
                    swarm_export_child_credential_for_provider(child_provider, child_key);
                } else {
                    /* No key for the child's provider — fall back to parent's model
                     * so the child can actually run instead of dying on credential error. */
                    fprintf(stdout,
                            "swarm: no credentials for provider '%s' (model '%s'), "
                            "falling back to parent model '%s'\n",
                            child_provider, m, s->default_model);
                    m = s->default_model;
                }
            }
        }

        /* ── Clear DSCO_EXEC so the child uses native dsco routing, not an
         * external CLI executor.  The parent may have DSCO_EXEC=claude or
         * DSCO_EXEC=codex set in its profile env; that must NOT leak into
         * sub-agent children — they are always native dsco processes. */
        unsetenv("DSCO_EXEC");

        /* Pass parent instance for lineage tracking */
        const char *parent_instance = getenv("DSCO_INSTANCE_ID");
        if (parent_instance && parent_instance[0]) {
            setenv("DSCO_PARENT_INSTANCE_ID", parent_instance, 1);
        }

        /* Mark this as a sub-agent and track depth for hierarchical swarms */
        setenv("DSCO_SUBAGENT", "1", 1);

        /* Propagate depth */
        char depth_str[16];
        snprintf(depth_str, sizeof(depth_str), "%d", depth);
        setenv("DSCO_SWARM_DEPTH", depth_str, 1);

        /* Propagate IPC DB path so children share the same IPC namespace */
        const char *ipc_db = getenv("DSCO_IPC_DB");
        if (ipc_db && ipc_db[0]) {
            setenv("DSCO_IPC_DB", ipc_db, 1);
        } else {
            /* Auto-create IPC DB path scoped to root ancestor */
            char ipc_path[512];
            snprintf(ipc_path, sizeof(ipc_path), "/tmp/dsco_ipc_%s.db",
                     parent_instance ? parent_instance : "orphan");
            setenv("DSCO_IPC_DB", ipc_path, 1);
        }

        /* Propagate remaining budget to child as a hard cap */
        if (s->swarm_budget_usd > 0) {
            double remaining = swarm_budget_remaining(s);
            int n_pending = 0;
            for (int ci = 0; ci < s->child_count; ci++)
                if (s->children[ci].status == SWARM_RUNNING)
                    n_pending++;
            double child_share = remaining / (n_pending + 1); /* +1 for this new child */
            if (child_share > 0) {
                char cb[32];
                snprintf(cb, sizeof(cb), "%.4f", child_share);
                setenv("DSCO_CHILD_BUDGET", cb, 1);
            }
        }

        /* Apply the per-agent model-instance spec (effort/temp/system-prompt…)
         * so this child wraps a distinct model instance. */
        swarm_apply_instance_env();

        /* Use execl with absolute path (not execlp which searches PATH) */
        setenv("DSCO_PROFILE", "worker", 1);
        setenv("DSCO_WORKER", "1", 1);
        execl(bin, bin, "--profile", "worker", "-m", m, task, NULL);

        /* If exec fails, write a clear error */
        fprintf(stdout, "swarm: exec failed for '%s': %s\n", bin, strerror(errno));
        _exit(127);
    }

    /* ── Parent ── */
    close(stdout_pipe[1]);

    /* The child has inherited the instance spec; drop it so the next spawn
     * starts clean. */
    swarm_clear_next_instance();

    /* Make pipe non-blocking */
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);

    swarm_child_t *c = &s->children[id];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->pid = pid;
    c->pipe_fd = stdout_pipe[0];
    c->err_fd = -1; /* merged into pipe_fd */
    c->status = SWARM_RUNNING;
    c->group_id = group_id;
    c->start_time = now_sec();
    c->depth = depth;

    snprintf(c->task, SWARM_LABEL_LEN, "%s", task);
    if (model)
        snprintf(c->model, sizeof(c->model), "%s", model);

    c->output_cap = 4096;
    c->output = safe_malloc(c->output_cap);
    c->output[0] = '\0';
    c->output_len = 0;

    c->stream_buf = safe_malloc(4096);
    c->stream_buf[0] = '\0';
    c->stream_buf_len = 0;

    s->child_count++;
    post_spawn_register(s, id);

    /* Add to group if specified */
    if (group_id >= 0 && group_id < s->group_count) {
        swarm_group_t *g = &s->groups[group_id];
        if (g->child_count < SWARM_MAX_CHILDREN) {
            g->child_ids[g->child_count++] = id;
        }
    }

    return id;
}

/* ── Provider-decoupled spawn ──────────────────────────────────────────
 * Spawns a dsco sub-agent forced to a specific native provider.
 * The child gets `--exec <provider> -m <model>` so it uses that
 * provider's API directly, completely independent of the parent. */

int swarm_spawn_provider(swarm_t *s, int group_id, const char *task, const char *model,
                         const char *provider) {
    if (!provider || !provider[0])
        return swarm_spawn_in_group(s, group_id, task, model);

    if (s->child_count >= SWARM_MAX_CHILDREN)
        return -1;

    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    int id = s->child_count;
    const char *cur_depth_env = getenv("DSCO_SWARM_DEPTH");
    int depth = cur_depth_env ? atoi(cur_depth_env) + 1 : 1;

    if (pid == 0) {
        /* ── Child ── */
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        setpgid(0, 0);

        const char *m = model ? model : s->default_model;
        const char *bin = s->dsco_path;

        /* Clear DSCO_EXEC — child is a native dsco process, not an external CLI */
        unsetenv("DSCO_EXEC");

        setenv("DSCO_SUBAGENT", "1", 1);
        char depth_str[16];
        snprintf(depth_str, sizeof(depth_str), "%d", depth);
        setenv("DSCO_SWARM_DEPTH", depth_str, 1);

        const char *parent_instance = getenv("DSCO_INSTANCE_ID");
        if (parent_instance && parent_instance[0])
            setenv("DSCO_PARENT_INSTANCE_ID", parent_instance, 1);

        const char *ipc_db = getenv("DSCO_IPC_DB");
        if (ipc_db && ipc_db[0]) {
            setenv("DSCO_IPC_DB", ipc_db, 1);
        } else {
            char ipc_path[512];
            snprintf(ipc_path, sizeof(ipc_path), "/tmp/dsco_ipc_%s.db",
                     parent_instance ? parent_instance : "orphan");
            setenv("DSCO_IPC_DB", ipc_path, 1);
        }

        if (s->swarm_budget_usd > 0) {
            double remaining = swarm_budget_remaining(s);
            int n_pending = 0;
            for (int ci = 0; ci < s->child_count; ci++)
                if (s->children[ci].status == SWARM_RUNNING)
                    n_pending++;
            double child_share = remaining / (n_pending + 1);
            if (child_share > 0) {
                char cb[32];
                snprintf(cb, sizeof(cb), "%.4f", child_share);
                setenv("DSCO_CHILD_BUDGET", cb, 1);
            }
        }

        /* Preserve the parent's resolved auth mode when pinning a provider. */
        const char *resolved_credential = NULL;
        if (s->api_key && s->api_key[0] &&
            strcmp(provider_detect(NULL, s->api_key), provider) == 0) {
            resolved_credential = s->api_key;
        } else {
            resolved_credential = provider_resolve_request_api_key(provider, s->api_key);
        }
        swarm_export_child_credential_for_provider(provider, resolved_credential);

        /* Key: --exec <provider> forces the child to use that provider's API */
        setenv("DSCO_PROFILE", "worker", 1);
        setenv("DSCO_WORKER", "1", 1);
        execl(bin, bin, "--profile", "worker", "--exec", provider, "-m", m, task, NULL);
        fprintf(stdout, "swarm: exec failed for '%s --exec %s': %s\n", bin, provider,
                strerror(errno));
        _exit(127);
    }

    /* ── Parent ── */
    close(stdout_pipe[1]);
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);

    swarm_child_t *c = &s->children[id];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->pid = pid;
    c->pipe_fd = stdout_pipe[0];
    c->err_fd = -1;
    c->status = SWARM_RUNNING;
    c->group_id = group_id;
    c->start_time = now_sec();
    c->depth = depth;
    c->executor = EXECUTOR_DSCO;
    snprintf(c->provider, sizeof(c->provider), "%s", provider);
    snprintf(c->task, SWARM_LABEL_LEN, "%s", task);
    if (model)
        snprintf(c->model, sizeof(c->model), "%s", model);

    c->output_cap = 4096;
    c->output = safe_malloc(c->output_cap);
    c->output[0] = '\0';
    c->output_len = 0;
    c->stream_buf = safe_malloc(4096);
    c->stream_buf[0] = '\0';
    c->stream_buf_len = 0;

    s->child_count++;
    post_spawn_register(s, id);

    if (group_id >= 0 && group_id < s->group_count) {
        swarm_group_t *g = &s->groups[group_id];
        if (g->child_count < SWARM_MAX_CHILDREN)
            g->child_ids[g->child_count++] = id;
    }

    return id;
}

/* ── Groups ───────────────────────────────────────────────────────────── */

int swarm_group_create(swarm_t *s, const char *name) {
    if (s->group_count >= SWARM_MAX_GROUPS)
        return -1;
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
    if (group_id < 0 || group_id >= s->group_count)
        return -1;
    int spawned = 0;
    for (int i = 0; i < task_count; i++) {
        int cid = swarm_spawn_in_group(s, group_id, tasks[i], model);
        if (cid >= 0)
            spawned++;
    }
    return spawned;
}

bool swarm_group_complete(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count)
        return true;
    swarm_group_t *g = &s->groups[group_id];
    for (int i = 0; i < g->child_count; i++) {
        swarm_child_t *c = &s->children[g->child_ids[i]];
        if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING)
            return false;
    }
    return true;
}

/* ── Group statistics helpers ────────────────────────────────────────── */

static int swarm_group_count_status(swarm_t *s, int group_id, swarm_status_t include_status) {
    if (group_id < 0 || group_id >= s->group_count)
        return 0;
    swarm_group_t *g = &s->groups[group_id];
    int count = 0;
    for (int i = 0; i < g->child_count; i++) {
        swarm_child_t *c = &s->children[g->child_ids[i]];
        if (c && c->status == include_status)
            count++;
    }
    return count;
}

int swarm_group_active_count(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count)
        return 0;
    swarm_group_t *g = &s->groups[group_id];
    int count = 0;
    for (int i = 0; i < g->child_count; i++) {
        swarm_child_t *c = &s->children[g->child_ids[i]];
        if (c && (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING))
            count++;
    }
    return count;
}

int swarm_group_done_count(swarm_t *s, int group_id) {
    return swarm_group_count_status(s, group_id, SWARM_DONE);
}

int swarm_group_error_count(swarm_t *s, int group_id) {
    return swarm_group_count_status(s, group_id, SWARM_ERROR);
}

int swarm_group_killed_count(swarm_t *s, int group_id) {
    return swarm_group_count_status(s, group_id, SWARM_KILLED);
}

double swarm_group_est_cost_usd(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count)
        return 0.0;
    swarm_group_t *g = &s->groups[group_id];
    double total = 0.0;
    for (int i = 0; i < g->child_count; i++) {
        swarm_child_t *c = &s->children[g->child_ids[i]];
        if (c)
            total += c->est_cost_usd;
    }
    return total;
}

double swarm_child_elapsed_sec(const swarm_child_t *c) {
    if (!c)
        return 0.0;
    double now = now_sec();
    return (c->end_time > 0 ? c->end_time : now) - c->start_time;
}

/* ── Read from child fd, append to output ─────────────────────────────── */

static void child_read(swarm_child_t *c, int fd, swarm_stream_cb cb, void *ctx) {
    char buf[SWARM_READ_BUF]; /* 64KB — 16x previous */
    ssize_t n;
    int chunks = 0;
    const int max_chunks_per_poll = 64;
    while (chunks < max_chunks_per_poll && (n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        chunks++;
        buf[n] = '\0';

        /* Grow output buffer if needed */
        size_t needed = c->output_len + (size_t)n + 1;
        while (needed > c->output_cap && c->output_cap < SWARM_MAX_OUTPUT) {
            c->output_cap *= 2;
            if (c->output_cap > SWARM_MAX_OUTPUT)
                c->output_cap = SWARM_MAX_OUTPUT;
            c->output = safe_realloc(c->output, c->output_cap);
        }

        if (needed <= c->output_cap) {
            memcpy(c->output + c->output_len, buf, n);
            c->output_len += n;
            c->output[c->output_len] = '\0';
        } else if (c->output_cap > c->output_len + 1) {
            /* Cap reached: append what still fits and keep draining fd. */
            size_t room = c->output_cap - c->output_len - 1;
            memcpy(c->output + c->output_len, buf, room);
            c->output_len += room;
            c->output[c->output_len] = '\0';
        }

        if (cb)
            cb(c->id, buf, n, ctx);
        c->status = SWARM_STREAMING;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        c->status = SWARM_ERROR;
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
        if (c->status != SWARM_RUNNING && c->status != SWARM_STREAMING)
            continue;

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

    int events = 0;
    if (nfds > 0) {
        int ret = poll(fds, nfds, timeout_ms);
        if (ret < 0)
            return ret;
        if (ret > 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].revents & (POLLIN | POLLHUP)) {
                    swarm_child_t *c = &s->children[fd_map[i]];
                    child_read(c, fds[i].fd, cb, ctx);
                    events++;
                }
            }
        }
    }

    /* Check for completed children — use bitset for O(1) skip of inactive */
    unsigned long long active_bits = s->active.bits;
    while (active_bits) {
        int i = __builtin_ctzll(active_bits); /* find lowest set bit */
        active_bits &= active_bits - 1;       /* clear it */

        swarm_child_t *c = &s->children[i];
        int wstatus;
        pid_t result = waitpid(c->pid, &wstatus, WNOHANG);
        if (result > 0) {
            /* Drain remaining output */
            if (c->pipe_fd >= 0)
                child_read(c, c->pipe_fd, cb, ctx);
            if (c->err_fd >= 0)
                child_read(c, c->err_fd, cb, ctx);

            close(c->pipe_fd);
            c->pipe_fd = -1;
            close(c->err_fd);
            c->err_fd = -1;

            c->end_time = now_sec();
            /* Preserve SWARM_KILLED if swarm_kill() already tagged this
             * child — a shell may catch SIGTERM and exit 0, which would
             * otherwise flip status to SWARM_DONE incorrectly. */
            bool already_killed = (c->status == SWARM_KILLED);
            if (WIFEXITED(wstatus)) {
                c->exit_code = WEXITSTATUS(wstatus);
                c->status = already_killed ? SWARM_KILLED
                          : (c->exit_code == 0) ? SWARM_DONE : SWARM_ERROR;
            } else {
                c->status = SWARM_KILLED;
                c->exit_code = -1;
            }

            /* Parse cost from external executor output on completion */
            if (c->executor != EXECUTOR_DSCO) {
                parse_child_cost_report(c);
            }

            /* Push to completion queue + clear active bit */
            post_complete(s, i);
        }
    }

    /* Enforce budget limits after polling */
    swarm_enforce_budgets(s);

    return events;
}

/* ── Fast completion primitives ───────────────────────────────────────── */

int swarm_completion_pop(swarm_t *s) {
    return cq_pop(&s->done_q);
}

int swarm_completion_pending(swarm_t *s) {
    return s->done_q.count;
}

int swarm_wait_any(swarm_t *s, int timeout_ms) {
    /* Check if something already completed */
    if (s->done_q.count > 0)
        return cq_pop(&s->done_q);

    /* Poll until something completes */
    double deadline = now_sec() + timeout_ms / 1000.0;
    while (s->active.count > 0) {
        int remaining_ms = (int)((deadline - now_sec()) * 1000);
        if (remaining_ms <= 0 && timeout_ms >= 0)
            break;
        if (remaining_ms < 0)
            remaining_ms = 0;

        swarm_poll_stream(s, remaining_ms < 50 ? remaining_ms : 50, s->stream_cb, s->stream_ctx);

        if (s->done_q.count > 0)
            return cq_pop(&s->done_q);
    }
    return -1;
}

int swarm_wait_n(swarm_t *s, int n, int *out_ids, int timeout_ms) {
    int collected = 0;
    double deadline = now_sec() + timeout_ms / 1000.0;

    while (collected < n && s->active.count > 0) {
        /* Drain already-queued completions first */
        while (collected < n && s->done_q.count > 0) {
            out_ids[collected++] = cq_pop(&s->done_q);
        }
        if (collected >= n)
            break;

        int remaining_ms = (int)((deadline - now_sec()) * 1000);
        if (remaining_ms <= 0 && timeout_ms >= 0)
            break;

        swarm_poll_stream(s, remaining_ms < 50 ? remaining_ms : 50, s->stream_cb, s->stream_ctx);
    }

    /* Drain any last completions */
    while (collected < n && s->done_q.count > 0)
        out_ids[collected++] = cq_pop(&s->done_q);

    return collected;
}

/* ── Status ───────────────────────────────────────────────────────────── */

swarm_child_t *swarm_get(swarm_t *s, int child_id) {
    if (child_id < 0 || child_id >= s->child_count)
        return NULL;
    return &s->children[child_id];
}

const char *swarm_status_str(swarm_status_t st) {
    switch (st) {
        case SWARM_PENDING:
            return "pending";
        case SWARM_RUNNING:
            return "running";
        case SWARM_STREAMING:
            return "streaming";
        case SWARM_DONE:
            return "done";
        case SWARM_ERROR:
            return "error";
        case SWARM_KILLED:
            return "killed";
    }
    return "unknown";
}

int swarm_active_count(swarm_t *s) {
    int count = 0;
    for (int i = 0; i < s->child_count; i++) {
        if (s->children[i].status == SWARM_RUNNING || s->children[i].status == SWARM_STREAMING)
            count++;
    }
    return count;
}

bool swarm_kill(swarm_t *s, int child_id) {
    swarm_child_t *c = swarm_get(s, child_id);
    if (!c)
        return false;
    if (c->status != SWARM_RUNNING && c->status != SWARM_STREAMING)
        return false;
    kill(-c->pid, SIGTERM);
    c->status = SWARM_KILLED;
    c->end_time = now_sec();
    return true;
}

void swarm_group_kill(swarm_t *s, int group_id) {
    if (group_id < 0 || group_id >= s->group_count)
        return;
    swarm_group_t *g = &s->groups[group_id];
    for (int i = 0; i < g->child_count; i++) {
        swarm_kill(s, g->child_ids[i]);
    }
}

/* ── Executor name helper ──────────────────────────────────────────────── */

const char *executor_type_name(executor_type_t t) {
    switch (t) {
        case EXECUTOR_DSCO:
            return "dsco";
        case EXECUTOR_CLAUDE:
            return "claude";
        case EXECUTOR_CODEX:
            return "codex";
    }
    return "unknown";
}

/* ── Executor detection ───────────────────────────────────────────────── */

static bool detect_binary(const char *name, char *out_path, size_t out_len) {
    /* Check common locations + PATH */
    const char *candidates[] = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL};

    /* First try which(1) for PATH-based lookup */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "which %s 2>/dev/null", name);
    FILE *f = popen(cmd, "r");
    if (f) {
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            /* Strip trailing newline */
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                line[--l] = '\0';
            if (l > 0 && access(line, X_OK) == 0) {
                snprintf(out_path, out_len, "%s", line);
                pclose(f);
                return true;
            }
        }
        pclose(f);
    }

    /* Fallback: check known paths */
    for (int i = 0; candidates[i]; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", candidates[i], name);
        if (access(path, X_OK) == 0) {
            snprintf(out_path, out_len, "%s", path);
            return true;
        }
    }
    return false;
}

static bool check_claude_auth(void) {
    /* Claude executor is usable with either Anthropic API key or Claude Code OAuth. */
    const char *key = provider_resolve_request_api_key("anthropic", NULL);
    return (key && key[0]);
}

static bool check_codex_auth(void) {
    /* Check if codex auth.json exists and has tokens */
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
    return (access(path, R_OK) == 0);
}

void swarm_detect_executors(swarm_t *s) {
    executor_registry_t *e = &s->executors;
    memset(e, 0, sizeof(*e));

    /* Detect Claude Code CLI */
    if (detect_binary("claude", e->claude_path, sizeof(e->claude_path))) {
        e->claude_available = check_claude_auth();
        if (e->claude_available) {
            snprintf(e->claude_model, sizeof(e->claude_model), "claude-sonnet-4-6");
        }
    }

    /* Detect OpenAI Codex CLI */
    if (detect_binary("codex", e->codex_path, sizeof(e->codex_path))) {
        e->codex_available = check_codex_auth();
        if (e->codex_available) {
            snprintf(e->codex_model, sizeof(e->codex_model), "gpt-5.3-codex-spark");
        }
    }
}

void swarm_prepare_executor_env(swarm_t *s, executor_type_t executor) {
    if (!s || executor != EXECUTOR_CLAUDE)
        return;

    /* Claude Code prefers ANTHROPIC_API_KEY over the logged-in subscription
       path. When dsco already resolved an OAuth/subscription credential,
       scrub the API-key overrides before exec so the Claude CLI stays on the
       user's Claude Code account instead of a low-balance API key. */
    const char *resolved = provider_resolve_request_api_key("anthropic", s->api_key);
    if (!resolved || !resolved[0] || !llm_anthropic_uses_claude_code_auth(resolved)) {
        return;
    }

    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("ANTHROPIC_AUTH_TOKEN");
    unsetenv("ANTHROPIC_BASE_URL");
    unsetenv("ANTHROPIC_MODEL");
    setenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN", resolved, 1);
    setenv("CLAUDE_CODE_OAUTH_TOKEN", resolved, 1);
}

/* ── External executor spawn ─────────────────────────────────────────── */

int swarm_spawn_executor(swarm_t *s, int group_id, const char *task, const char *model,
                         executor_type_t executor) {
    if (executor == EXECUTOR_DSCO) {
        return swarm_spawn_in_group(s, group_id, task, model);
    }

    if (s->child_count >= SWARM_MAX_CHILDREN)
        return -1;

    executor_registry_t *e = &s->executors;
    const char *bin = NULL;
    const char **argv = NULL;

    /* Build argv depending on executor type */
    const char *exec_argv[32];
    memset(exec_argv, 0, sizeof(exec_argv));

    if (executor == EXECUTOR_CLAUDE) {
        if (!e->claude_available)
            return -1;
        bin = e->claude_path;
        const char *m = (model && model[0]) ? model : e->claude_model;
        exec_argv[0] = bin;
        exec_argv[1] = "-p"; /* print mode — non-interactive */
        exec_argv[2] = "--output-format";
        exec_argv[3] = "json";
        exec_argv[4] = "--model";
        exec_argv[5] = m;
        exec_argv[6] = "--dangerously-skip-permissions";
        exec_argv[7] = "--no-session-persistence";
        exec_argv[8] = task;
        exec_argv[9] = NULL;
    } else if (executor == EXECUTOR_CODEX) {
        if (!e->codex_available)
            return -1;
        bin = e->codex_path;
        const char *m = (model && model[0]) ? model : e->codex_model;
        exec_argv[0] = bin;
        exec_argv[1] = "exec";
        exec_argv[2] = "--json";
        exec_argv[3] = "-m";
        exec_argv[4] = m;
        exec_argv[5] = "--dangerously-bypass-approvals-and-sandbox";
        exec_argv[6] = "--ephemeral";
        exec_argv[7] = task;
        exec_argv[8] = NULL;
    } else {
        return -1;
    }
    argv = exec_argv;

    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    int id = s->child_count;
    int depth = 1; /* external executors are always depth 1 from dsco's perspective */

    if (pid == 0) {
        /* ── Child process ── */
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        setpgid(0, 0);

        /* Propagate working directory */
        char cwd[2048];
        if (getcwd(cwd, sizeof(cwd)))
            chdir(cwd);

        swarm_prepare_executor_env(s, executor);

        execv(bin, (char *const *)argv);
        fprintf(stdout, "swarm: exec failed for '%s': %s\n", bin, strerror(errno));
        _exit(127);
    }

    /* ── Parent ── */
    close(stdout_pipe[1]);
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);

    swarm_child_t *c = &s->children[id];
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->pid = pid;
    c->pipe_fd = stdout_pipe[0];
    c->err_fd = -1;
    c->status = SWARM_RUNNING;
    c->group_id = group_id;
    c->start_time = now_sec();
    c->depth = depth;
    c->executor = executor;

    snprintf(c->task, SWARM_LABEL_LEN, "%s", task);
    if (model)
        snprintf(c->model, sizeof(c->model), "%s", model);
    else {
        const char *dm = (executor == EXECUTOR_CLAUDE) ? e->claude_model : e->codex_model;
        snprintf(c->model, sizeof(c->model), "%s", dm);
    }

    c->output_cap = 4096;
    c->output = safe_malloc(c->output_cap);
    c->output[0] = '\0';
    c->output_len = 0;
    c->stream_buf = safe_malloc(4096);
    c->stream_buf[0] = '\0';
    c->stream_buf_len = 0;

    s->child_count++;
    post_spawn_register(s, id);

    if (group_id >= 0 && group_id < s->group_count) {
        swarm_group_t *g = &s->groups[group_id];
        if (g->child_count < SWARM_MAX_CHILDREN) {
            g->child_ids[g->child_count++] = id;
        }
    }

    return id;
}

/* ── Budget system ────────────────────────────────────────────────────── */

void swarm_set_budget(swarm_t *s, double budget_usd) {
    s->swarm_budget_usd = budget_usd;
}

/* A child's tokens are "subsidized" — covered by a flat-rate subscription
 * (the $200/mo Claude Max / ChatGPT Codex plans) rather than metered credit —
 * when it runs through the Claude Code or Codex CLI executors. Their notional
 * API cost is tracked for visibility but does NOT draw the real-dollar budget.
 * Override the set via DSCO_SUBSIDIZED_EXECUTORS=claude,codex,dsco (dsco only
 * when the parent itself runs on an Anthropic/OpenAI OAuth subscription). */
bool swarm_child_is_subsidized(const swarm_child_t *c) {
    if (!c)
        return false;
    const char *ov = getenv("DSCO_SUBSIDIZED_EXECUTORS");
    if (ov && ov[0]) {
        const char *nm = executor_type_name(c->executor);
        return nm && strstr(ov, nm) != NULL;
    }
    return c->executor == EXECUTOR_CLAUDE || c->executor == EXECUTOR_CODEX;
}

double swarm_budget_remaining(swarm_t *s) {
    /* Always recompute the metered vs subsidized split, even when unlimited,
     * so status/reporting stays accurate. */
    double metered = 0, subsidized = 0;
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        double cost = c->reported_cost_usd > 0 ? c->reported_cost_usd : c->est_cost_usd;
        if (swarm_child_is_subsidized(c))
            subsidized += cost;
        else
            metered += cost;
    }
    s->spent_usd = metered;
    s->subsidized_usd = subsidized;
    if (s->swarm_budget_usd <= 0)
        return 1e9; /* unlimited */
    return s->swarm_budget_usd - metered;
}

double swarm_estimate_task_cost(swarm_t *s, const char *model) {
    (void)s;
    /* Use router EMA if available, else estimate from registry pricing */
    extern router_t g_router;
    router_model_stat_t *st = router_get_stats(&g_router, model);
    if (st && st->ema_cost_per_turn > 0) {
        return st->ema_cost_per_turn;
    }
    /* Fallback: estimate from registry (assume ~2k input + 500 output tokens) */
    const model_info_t *mi = model_lookup(model);
    if (mi) {
        return mi->input_price * 2000 / 1e6 + mi->output_price * 500 / 1e6;
    }
    return 0.01; /* safe default: $0.01 per task */
}

void swarm_enforce_budgets(swarm_t *s) {
    double metered_spent = 0, subsidized_spent = 0;
    for (int i = 0; i < s->child_count; i++) {
        swarm_child_t *c = &s->children[i];
        double cost = c->reported_cost_usd > 0 ? c->reported_cost_usd : c->est_cost_usd;
        bool subsidized = swarm_child_is_subsidized(c);
        if (subsidized)
            subsidized_spent += cost;
        else
            metered_spent += cost;

        /* Per-child real-dollar budget enforcement — skip subsidized children
         * since their flat-rate plan already covers the tokens; killing them
         * for "cost" would waste prepaid subscription capacity. */
        if (!subsidized && c->budget_usd > 0 && cost > c->budget_usd &&
            (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING)) {
            fprintf(stderr, "  %s⚠%s agent #%d over budget ($%.4f > $%.4f) — killing\n", TUI_YELLOW,
                    TUI_RESET, c->id, cost, c->budget_usd);
            swarm_kill(s, c->id);
        }
    }
    s->spent_usd = metered_spent;
    s->subsidized_usd = subsidized_spent;

    /* Global budget enforcement applies only to metered real-dollar spend. */
    if (s->swarm_budget_usd > 0 && metered_spent >= s->swarm_budget_usd) {
        fprintf(stderr,
                "  %s%sswarm budget exhausted: $%.4f / $%.4f metered "
                "(+$%.4f subsidized) — killing metered children%s\n",
                TUI_BOLD, TUI_RED, metered_spent, s->swarm_budget_usd, subsidized_spent, TUI_RESET);
        for (int i = 0; i < s->child_count; i++) {
            swarm_child_t *c = &s->children[i];
            /* Leave subsidized children running — they don't draw the budget. */
            if (swarm_child_is_subsidized(c))
                continue;
            if (c->status == SWARM_RUNNING || c->status == SWARM_STREAMING)
                swarm_kill(s, c->id);
        }
    }
}

/* Parse cost from executor JSON output.
 * Claude: {"total_cost_usd":0.01234, ...}
 * Codex: tracks tokens in --json output
 */
static void parse_child_cost_report(swarm_child_t *c) {
    if (!c->output || c->output_len == 0)
        return;

    /* Try to find total_cost_usd in the output (Claude JSON format) */
    const char *cost_str = strstr(c->output, "\"total_cost_usd\":");
    if (cost_str) {
        cost_str += 17; /* skip key */
        c->reported_cost_usd = strtod(cost_str, NULL);
        return;
    }
    /* Try "total_cost": (alternative key) */
    cost_str = strstr(c->output, "\"total_cost\":");
    if (cost_str) {
        cost_str += 13;
        c->reported_cost_usd = strtod(cost_str, NULL);
        return;
    }
    /* Codex: parse token usage and compute cost */
    const char *in_tok = strstr(c->output, "\"prompt_tokens\":");
    const char *out_tok = strstr(c->output, "\"completion_tokens\":");
    if (in_tok && out_tok) {
        int input_tokens = atoi(in_tok + 16);
        int output_tokens = atoi(out_tok + 20);
        c->est_input_tokens = input_tokens;
        c->est_output_tokens = output_tokens;
        const model_info_t *mi = model_lookup(c->model);
        if (mi) {
            c->reported_cost_usd =
                input_tokens * mi->input_price / 1e6 + output_tokens * mi->output_price / 1e6;
        }
    }
}

/* ── JSON output ──────────────────────────────────────────────────────── */

int swarm_status_json(swarm_t *s, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 4096);

    /* Refresh the metered-vs-subsidized spend split before reporting. */
    swarm_budget_remaining(s);

    jbuf_append(&b, "{\"swarm\":{\"children\":");
    jbuf_append_int(&b, s->child_count);
    jbuf_append(&b, ",\"active\":");
    jbuf_append_int(&b, swarm_active_count(s));
    jbuf_append(&b, ",\"groups\":");
    jbuf_append_int(&b, s->group_count);

    jbuf_append(&b, ",\"processes\":[");
    for (int i = 0; i < s->child_count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
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
        jbuf_append(&b, ",\"executor\":");
        jbuf_append_json_str(&b, executor_type_name(c->executor));
        jbuf_append(&b, ",\"subsidized\":");
        jbuf_append(&b, swarm_child_is_subsidized(c) ? "true" : "false");
        if (c->budget_usd > 0) {
            char bud[32];
            snprintf(bud, sizeof(bud), "%.6f", c->budget_usd);
            jbuf_append(&b, ",\"budget_usd\":");
            jbuf_append(&b, bud);
        }
        if (c->reported_cost_usd > 0) {
            char rc[32];
            snprintf(rc, sizeof(rc), "%.6f", c->reported_cost_usd);
            jbuf_append(&b, ",\"reported_cost_usd\":");
            jbuf_append(&b, rc);
        }
        jbuf_append(&b, "}");
    }

    /* Swarm budget summary. spent_usd is metered real-dollar (OpenRouter/API
     * credit) draw; subsidized_usd is notional cost covered by flat-rate
     * Claude Code / Codex plans (does NOT draw the budget). */
    {
        char spent[32], subs[32];
        snprintf(spent, sizeof(spent), "%.6f", s->spent_usd);
        snprintf(subs, sizeof(subs), "%.6f", s->subsidized_usd);
        jbuf_append(&b, "],\"budget\":{");
        if (s->swarm_budget_usd > 0) {
            char bud[32];
            snprintf(bud, sizeof(bud), "%.6f", s->swarm_budget_usd);
            jbuf_append(&b, "\"total_usd\":");
            jbuf_append(&b, bud);
            jbuf_append(&b, ",\"remaining_usd\":");
            char rem[32];
            snprintf(rem, sizeof(rem), "%.6f", s->swarm_budget_usd - s->spent_usd);
            jbuf_append(&b, rem);
            jbuf_append(&b, ",");
        }
        jbuf_append(&b, "\"spent_usd\":");
        jbuf_append(&b, spent);
        jbuf_append(&b, ",\"subsidized_usd\":");
        jbuf_append(&b, subs);
        jbuf_append(&b, "},\"group_details\":[");
    }

    /* Executor availability */
    jbuf_t exec_info;
    jbuf_init(&exec_info, 256);
    jbuf_append(&exec_info, "{\"dsco\":true");
    jbuf_appendf(&exec_info, ",\"claude\":%s", s->executors.claude_available ? "true" : "false");
    jbuf_appendf(&exec_info, ",\"codex\":%s", s->executors.codex_available ? "true" : "false");
    jbuf_append(&exec_info, "}");

    /* We'll inject this after group_details */
    char *exec_json = exec_info.data ? safe_strdup(exec_info.data) : safe_strdup("{}");
    jbuf_free(&exec_info);

    /* Continue with group_details (remove duplicate "],\"group_details\":[") */
    for (int i = 0; i < s->group_count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
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
    jbuf_append(&b, "],\"executors\":");
    jbuf_append(&b, exec_json);
    free(exec_json);
    jbuf_append(&b, "}}");

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
        if (i > 0)
            jbuf_append(&b, ",");
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
