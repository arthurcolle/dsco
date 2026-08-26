#include "durable_agents.h"
#include "ipc.h"
#include "json_util.h"
#include "tui.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void durable_agents_default_db_path(char *out, size_t len) {
    if (!out || len == 0)
        return;
    const char *env = getenv("DSCO_AGENTS_DB");
    if (env && env[0]) {
        snprintf(out, len, "%s", env);
        return;
    }
    env = getenv("DSCO_IPC_DB");
    if (env && env[0]) {
        snprintf(out, len, "%s", env);
        return;
    }
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, len, "%s/.dsco/agents/bus.db", home);
    else
        snprintf(out, len, "/tmp/dsco_agents_%d.db", (int)geteuid());
}

static void ensure_parent_dir(const char *path) {
    if (!path || !path[0])
        return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (dir[0])
                (void)mkdir(dir, 0700);
            *p = '/';
        }
    }
    (void)mkdir(dir, 0700);
}

static void agents_usage(FILE *out, const char *prog) {
    fprintf(out,
            "usage:\n"
            "  %s agents create <id> [--parent ID] [--role ROLE] [--model MODEL] [--toolkit JSON]\n"
            "       [--organization ID --deployment ID --role-id ID --policy-sha256 HEX]\n"
            "       [--capsule-sha256 HEX --graphsub-namespace NS --router-project ID]\n"
            "       [--capabilities JSON --budget-gsu N --budget-usd N]\n"
            "  %s agents list [--json]\n"
            "  %s agents status <id> [--json]\n"
            "  %s agents tui [--once] [--interval-ms N] [--limit N] [--plain]\n"
            "  %s agents watch [--interval-ms N] [--limit N] [--plain]\n"
            "  %s agents send --from ID [--to ID|all] [--topic TOPIC] <message...>\n"
            "  %s agents inbox <id> [--all] [--mark-read] [--limit N] [--json]\n"
            "  %s agents sent <id> [--limit N] [--json]\n"
            "  %s agents bus [--limit N] [--json]\n"
            "  %s agents db\n"
            "\n"
            "default durable bus: ~/.dsco/agents/bus.db (override DSCO_AGENTS_DB or DSCO_IPC_DB)\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static bool open_agents_db(const char *self_id) {
    char db[PATH_MAX];
    durable_agents_default_db_path(db, sizeof(db));
    ensure_parent_dir(db);
    return ipc_init(db, self_id && self_id[0] ? self_id : "agents-cli");
}

static const char *agent_status_name(ipc_agent_status_t s) {
    switch (s) {
        case IPC_AGENT_STARTING: return "starting";
        case IPC_AGENT_DURABLE:  return "durable";
        case IPC_AGENT_IDLE:     return "idle";
        case IPC_AGENT_WORKING:  return "working";
        case IPC_AGENT_DONE:     return "done";
        case IPC_AGENT_ERROR:    return "error";
        case IPC_AGENT_DEAD:     return "dead";
    }
    return "unknown";
}

static int parse_limit(int argc, char **argv, int def) {
    int limit = def;
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0) {
            int v = atoi(argv[i + 1]);
            if (v > 0 && v <= 256)
                limit = v;
        }
    }
    return limit;
}

static int parse_int_option(int argc, char **argv, const char *flag, int def, int min, int max) {
    int v = def;
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            int parsed = atoi(argv[i + 1]);
            if (parsed >= min && parsed <= max)
                v = parsed;
        }
    }
    return v;
}

static bool has_flag(int argc, char **argv, const char *flag) {
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

static const char *arg_value(int argc, char **argv, const char *flag, const char *def) {
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return def;
}

static const char *paint(bool color, const char *s) {
    return color ? s : "";
}

static char *join_message_args(int argc, char **argv, int start) {
    size_t need = 1;
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--to") == 0 || strcmp(argv[i], "--from") == 0 ||
                strcmp(argv[i], "--topic") == 0 || strcmp(argv[i], "--limit") == 0)
                i++;
            continue;
        }
        need += strlen(argv[i]) + 1;
    }
    char *body = safe_malloc(need);
    body[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--to") == 0 || strcmp(argv[i], "--from") == 0 ||
                strcmp(argv[i], "--topic") == 0 || strcmp(argv[i], "--limit") == 0)
                i++;
            continue;
        }
        if (body[0])
            strcat(body, " ");
        strcat(body, argv[i]);
    }
    return body;
}

static void one_line_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && n + 1 < dst_len;
         p++) {
        unsigned char c = *p;
        dst[n++] = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    dst[n] = '\0';
}

static void fit_copy(char *dst, size_t dst_len, const char *src, int width) {
    if (!dst || dst_len == 0)
        return;
    if (width < 1)
        width = 1;
    char flat[2048];
    one_line_copy(flat, sizeof(flat), src);
    size_t max = (size_t)width;
    size_t n = strlen(flat);
    if (n > max) {
        if (max > 1) {
            memcpy(dst, flat, max - 1);
            dst[max - 1] = '~';
            dst[max] = '\0';
        } else {
            dst[0] = '~';
            dst[1] = '\0';
        }
        return;
    }
    snprintf(dst, dst_len, "%s", flat);
}

static void format_age(double ts, char *out, size_t len) {
    if (!out || len == 0)
        return;
    if (ts <= 0) {
        snprintf(out, len, "-");
        return;
    }
    double age = difftime(time(NULL), (time_t)ts);
    if (age < 0)
        age = 0;
    if (age < 60)
        snprintf(out, len, "%.0fs", age);
    else if (age < 3600)
        snprintf(out, len, "%.0fm", age / 60.0);
    else if (age < 86400)
        snprintf(out, len, "%.0fh", age / 3600.0);
    else
        snprintf(out, len, "%.0fd", age / 86400.0);
}

static const char *agent_status_color(ipc_agent_status_t s) {
    switch (s) {
        case IPC_AGENT_STARTING:
            return TUI_BYELLOW;
        case IPC_AGENT_DURABLE:
            return TUI_BMAGENTA;
        case IPC_AGENT_IDLE:
            return TUI_BCYAN;
        case IPC_AGENT_WORKING:
            return TUI_BGREEN;
        case IPC_AGENT_DONE:
            return TUI_GREEN;
        case IPC_AGENT_ERROR:
        case IPC_AGENT_DEAD:
            return TUI_BRED;
    }
    return TUI_DIM;
}

static const char *task_status_name(ipc_task_status_t s) {
    switch (s) {
        case IPC_TASK_PENDING:
            return "pending";
        case IPC_TASK_ASSIGNED:
            return "assigned";
        case IPC_TASK_RUNNING:
            return "running";
        case IPC_TASK_DONE:
            return "done";
        case IPC_TASK_FAILED:
            return "failed";
    }
    return "unknown";
}

static void free_tasks(ipc_task_t *tasks, int n) {
    for (int i = 0; i < n; i++)
        free(tasks[i].result);
}

typedef struct {
    const char *db_path;
    int limit;
    bool color;
    bool watch;
    int interval_ms;
} agents_tui_opts_t;

static void free_messages(ipc_message_t *msgs, int n);

static void render_agents_tui(const agents_tui_opts_t *opts) {
    ipc_agent_info_t agents[512];
    ipc_message_t msgs[128];
    ipc_task_t tasks[128];
    int agent_count = ipc_list_agents(agents, 512);
    int msg_limit = opts->limit > 0 && opts->limit < 128 ? opts->limit : 12;
    int task_limit = opts->limit > 0 && opts->limit < 128 ? opts->limit : 12;
    int msg_count = ipc_list_bus(msgs, msg_limit);
    int task_count = ipc_task_list(NULL, tasks, task_limit);

    int durable = 0, live = 0, working = 0, idle = 0, done = 0, unhealthy = 0;
    for (int i = 0; i < agent_count; i++) {
        if (agents[i].status == IPC_AGENT_DURABLE)
            durable++;
        if (ipc_agent_alive(agents[i].id))
            live++;
        if (agents[i].status == IPC_AGENT_WORKING)
            working++;
        else if (agents[i].status == IPC_AGENT_IDLE)
            idle++;
        else if (agents[i].status == IPC_AGENT_DONE)
            done++;
        else if (agents[i].status == IPC_AGENT_ERROR || agents[i].status == IPC_AGENT_DEAD)
            unhealthy++;
    }

    int width = tui_term_width();
    if (width < 80)
        width = 80;
    int task_w = width - 88;
    if (task_w < 18)
        task_w = 18;
    if (task_w > 70)
        task_w = 70;

    printf("%s%sdsco realtime swarm%s  %s%s%s\n",
           paint(opts->color, TUI_BOLD), paint(opts->color, TUI_BCYAN),
           paint(opts->color, TUI_RESET), paint(opts->color, TUI_DIM),
           opts->db_path ? opts->db_path : "(default bus)", paint(opts->color, TUI_RESET));
    printf("agents=%d durable=%d live=%d working=%d idle=%d done=%d unhealthy=%d tasks=%d messages=%d",
           agent_count, durable, live, working, idle, done, unhealthy, task_count, msg_count);
    if (opts->watch)
        printf("  refresh=%dms  q/Ctrl-C quits", opts->interval_ms);
    printf("\n\n");

    printf("%s%-28s %-10s %-5s %-16s %-20s %-7s %s%s\n",
           paint(opts->color, TUI_DIM), "agent", "state", "depth", "role", "model", "age",
           "task", paint(opts->color, TUI_RESET));
    for (int i = 0; i < agent_count; i++) {
        char name[64], role[32], model[32], task[96], age[16], display_name[80];
        fit_copy(name, sizeof(name), agents[i].id, 24);
        fit_copy(role, sizeof(role), agents[i].role[0] ? agents[i].role : "-", 16);
        fit_copy(model, sizeof(model), agents[i].model[0] ? agents[i].model : "-", 20);
        fit_copy(task, sizeof(task), agents[i].current_task[0] ? agents[i].current_task : "-", task_w);
        format_age(agents[i].last_heartbeat > 0 ? agents[i].last_heartbeat : agents[i].started_at,
                   age, sizeof(age));

        int indent = agents[i].depth;
        if (indent < 0)
            indent = 0;
        if (indent > 8)
            indent = 8;
        int pos = 0;
        for (int j = 0; j < indent && pos + 2 < (int)sizeof(display_name); j++) {
            display_name[pos++] = ' ';
            display_name[pos++] = ' ';
        }
        snprintf(display_name + pos, sizeof(display_name) - (size_t)pos, "%s%s", 
                 ipc_agent_alive(agents[i].id) ? "*" : " ", name);

        printf("%-28s %s%-10s%s %-5d %-16s %-20s %-7s %s\n", display_name,
               paint(opts->color, agent_status_color(agents[i].status)),
               agent_status_name(agents[i].status), paint(opts->color, TUI_RESET),
               agents[i].depth, role, model, age, task);
    }
    if (agent_count == 0)
        printf("  %sno durable or live agents on this bus%s\n",
               paint(opts->color, TUI_DIM), paint(opts->color, TUI_RESET));

    printf("\n%srecent tasks%s\n", paint(opts->color, TUI_BOLD), paint(opts->color, TUI_RESET));
    for (int i = 0; i < task_count; i++) {
        char desc[96], who[32];
        fit_copy(desc, sizeof(desc), tasks[i].description, width - 36);
        fit_copy(who, sizeof(who), tasks[i].assigned_to[0] ? tasks[i].assigned_to : "-", 24);
        printf("#%-4d %-9s prio=%-3d %-24s %s\n", tasks[i].id,
               task_status_name(tasks[i].status), tasks[i].priority, who, desc);
    }
    if (task_count == 0)
        printf("  %sno tasks%s\n", paint(opts->color, TUI_DIM), paint(opts->color, TUI_RESET));

    printf("\n%srecent bus%s\n", paint(opts->color, TUI_BOLD), paint(opts->color, TUI_RESET));
    for (int i = 0; i < msg_count; i++) {
        char body[128], from[24], to[24], topic[24], age[16];
        fit_copy(body, sizeof(body), msgs[i].body ? msgs[i].body : "", width - 48);
        fit_copy(from, sizeof(from), msgs[i].from_agent[0] ? msgs[i].from_agent : "?", 20);
        fit_copy(to, sizeof(to), msgs[i].to_agent[0] ? msgs[i].to_agent : "all", 20);
        fit_copy(topic, sizeof(topic), msgs[i].topic[0] ? msgs[i].topic : "general", 20);
        format_age(msgs[i].created_at, age, sizeof(age));
        printf("#%-4d %-7s %-20s -> %-20s [%s] %s\n", msgs[i].id, age, from, to, topic,
               body);
    }
    if (msg_count == 0)
        printf("  %sno messages%s\n", paint(opts->color, TUI_DIM), paint(opts->color, TUI_RESET));

    free_messages(msgs, msg_count);
    free_tasks(tasks, task_count);
}

static volatile sig_atomic_t g_agents_tui_stop = 0;

static void agents_tui_sigint(int sig) {
    (void)sig;
    g_agents_tui_stop = 1;
}

static int agents_tui_command(int argc, char **argv, bool watch_default) {
    bool once = has_flag(argc, argv, "--once");
    bool plain = has_flag(argc, argv, "--plain") || getenv("NO_COLOR") != NULL;
    int interval_ms = parse_int_option(argc, argv, "--interval-ms", 500, 100, 10000);
    int limit = parse_limit(argc, argv, 12);

    char db[PATH_MAX];
    durable_agents_default_db_path(db, sizeof(db));
    if (!open_agents_db("agents-tui")) {
        fprintf(stderr, "dsco agents tui: could not open durable bus\n");
        return 1;
    }

    bool interactive = isatty(STDOUT_FILENO);
    bool watch = watch_default && !once && interactive;
    agents_tui_opts_t opts = {
        .db_path = db,
        .limit = limit,
        .color = !plain && interactive,
        .watch = watch,
        .interval_ms = interval_ms,
    };

    if (!watch) {
        render_agents_tui(&opts);
        ipc_shutdown();
        return 0;
    }

    struct termios orig, raw;
    bool raw_active = false;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &orig) == 0) {
        raw = orig;
        raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        raw_active = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = agents_tui_sigint;
    sigaction(SIGINT, &sa, &old_sa);

    g_agents_tui_stop = 0;
    fputs("\033[?1049h\033[?25l", stdout);
    fflush(stdout);
    while (!g_agents_tui_stop) {
        fputs("\033[2J\033[H", stdout);
        render_agents_tui(&opts);
        fflush(stdout);

        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        int rv = poll(&pfd, 1, interval_ms);
        if (rv > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q' || c == 27))
                break;
        }
    }
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    if (raw_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    sigaction(SIGINT, &old_sa, NULL);
    ipc_shutdown();
    return 0;
}

static void append_agent_json(jbuf_t *b, const ipc_agent_info_t *a) {
    jbuf_append(b, "{\"id\":");
    jbuf_append_json_str(b, a->id);
    jbuf_append(b, ",\"parent_id\":");
    jbuf_append_json_str(b, a->parent_id);
    jbuf_append(b, ",\"depth\":");
    jbuf_append_int(b, a->depth);
    jbuf_append(b, ",\"status\":");
    jbuf_append_json_str(b, agent_status_name(a->status));
    jbuf_append(b, ",\"role\":");
    jbuf_append_json_str(b, a->role);
    jbuf_append(b, ",\"model\":");
    jbuf_append_json_str(b, a->model);
    jbuf_append(b, ",\"toolkit\":");
    jbuf_append_json_str(b, a->toolkit);
    jbuf_append(b, ",\"organization_id\":");
    jbuf_append_json_str(b, a->organization_id);
    jbuf_append(b, ",\"deployment_id\":");
    jbuf_append_json_str(b, a->deployment_id);
    jbuf_append(b, ",\"role_id\":");
    jbuf_append_json_str(b, a->role_id);
    jbuf_append(b, ",\"policy_sha256\":");
    jbuf_append_json_str(b, a->policy_sha256);
    jbuf_append(b, ",\"capsule_sha256\":");
    jbuf_append_json_str(b, a->capsule_sha256);
    jbuf_append(b, ",\"graphsub_namespace\":");
    jbuf_append_json_str(b, a->graphsub_namespace);
    jbuf_append(b, ",\"router_project_id\":");
    jbuf_append_json_str(b, a->router_project_id);
    jbuf_append(b, ",\"capabilities\":");
    jbuf_append_json_str(b, a->capabilities);
    jbuf_append(b, ",\"budget_gsu\":");
    jbuf_appendf(b, "%.15g", a->budget_gsu);
    jbuf_append(b, ",\"budget_usd\":");
    jbuf_appendf(b, "%.15g", a->budget_usd);
    jbuf_append(b, ",\"pid\":");
    jbuf_append_int(b, a->pid);
    jbuf_append(b, ",\"alive\":");
    jbuf_append(b, ipc_agent_alive(a->id) ? "true" : "false");
    jbuf_append(b, "}");
}

static void print_agent_text(const ipc_agent_info_t *a) {
    printf("%-18s %-9s depth=%d parent=%s role=%s model=%s alive=%s\n",
           a->id, agent_status_name(a->status), a->depth,
           a->parent_id[0] ? a->parent_id : "-", a->role[0] ? a->role : "-",
           a->model[0] ? a->model : "-", ipc_agent_alive(a->id) ? "yes" : "no");
}

static void append_message_json(jbuf_t *b, const ipc_message_t *m) {
    jbuf_append(b, "{\"id\":");
    jbuf_append_int(b, m->id);
    jbuf_append(b, ",\"from\":");
    jbuf_append_json_str(b, m->from_agent);
    jbuf_append(b, ",\"to\":");
    jbuf_append_json_str(b, m->to_agent);
    jbuf_append(b, ",\"topic\":");
    jbuf_append_json_str(b, m->topic);
    jbuf_append(b, ",\"body\":");
    jbuf_append_json_str(b, m->body ? m->body : "");
    jbuf_append(b, ",\"created_at\":");
    jbuf_appendf(b, "%.3f", m->created_at);
    jbuf_append(b, ",\"read\":");
    jbuf_append(b, m->read ? "true" : "false");
    jbuf_append(b, "}");
}

static void free_messages(ipc_message_t *msgs, int n) {
    for (int i = 0; i < n; i++)
        free(msgs[i].body);
}

static int print_messages(ipc_message_t *msgs, int n, bool json) {
    if (json) {
        jbuf_t b;
        jbuf_init(&b, 4096);
        jbuf_append(&b, "[");
        for (int i = 0; i < n; i++) {
            if (i)
                jbuf_append(&b, ",");
            append_message_json(&b, &msgs[i]);
        }
        jbuf_append(&b, "]");
        puts(b.data);
        jbuf_free(&b);
    } else {
        for (int i = 0; i < n; i++) {
            printf("#%d %s -> %s [%s]%s %s\n", msgs[i].id,
                   msgs[i].from_agent[0] ? msgs[i].from_agent : "?",
                   msgs[i].to_agent[0] ? msgs[i].to_agent : "all",
                   msgs[i].topic[0] ? msgs[i].topic : "general",
                   msgs[i].read ? " read" : "", msgs[i].body ? msgs[i].body : "");
        }
    }
    free_messages(msgs, n);
    return 0;
}

int durable_agents_cli(int argc, char **argv) {
    if (argc < 3) {
        agents_usage(stderr, argv[0]);
        return 2;
    }
    const char *sub = argv[2];
    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0 || strcmp(sub, "help") == 0) {
        agents_usage(stdout, argv[0]);
        return 0;
    }
    if (strcmp(sub, "db") == 0) {
        char db[PATH_MAX];
        durable_agents_default_db_path(db, sizeof(db));
        puts(db);
        return 0;
    }

    if (strcmp(sub, "tui") == 0 || strcmp(sub, "watch") == 0)
        return agents_tui_command(argc - 3, argv + 3, true);

    if (strcmp(sub, "create") == 0) {
        if (argc < 4) {
            agents_usage(stderr, argv[0]);
            return 2;
        }
        const char *id = argv[3];
        const char *parent = arg_value(argc - 4, argv + 4, "--parent", "");
        const char *role = arg_value(argc - 4, argv + 4, "--role", "");
        const char *model = arg_value(argc - 4, argv + 4, "--model", "claude-fable-5");
        const char *toolkit = arg_value(argc - 4, argv + 4, "--toolkit", "*");
        ipc_agent_binding_t binding = {
            .organization_id = arg_value(argc - 4, argv + 4, "--organization", ""),
            .deployment_id = arg_value(argc - 4, argv + 4, "--deployment", ""),
            .role_id = arg_value(argc - 4, argv + 4, "--role-id", ""),
            .policy_sha256 = arg_value(argc - 4, argv + 4, "--policy-sha256", ""),
            .capsule_sha256 = arg_value(argc - 4, argv + 4, "--capsule-sha256", ""),
            .graphsub_namespace = arg_value(argc - 4, argv + 4, "--graphsub-namespace", ""),
            .router_project_id = arg_value(argc - 4, argv + 4, "--router-project", ""),
            .capabilities = arg_value(argc - 4, argv + 4, "--capabilities", "[]"),
            .budget_gsu = atof(arg_value(argc - 4, argv + 4, "--budget-gsu", "0")),
            .budget_usd = atof(arg_value(argc - 4, argv + 4, "--budget-usd", "0")),
        };
        bool has_binding = binding.organization_id[0] || binding.deployment_id[0] ||
                           binding.role_id[0] || binding.policy_sha256[0] ||
                           binding.capsule_sha256[0];
        if (has_binding && (!binding.organization_id[0] || !binding.deployment_id[0] ||
                            !binding.role_id[0] || strlen(binding.policy_sha256) != 64 ||
                            strlen(binding.capsule_sha256) != 64)) {
            fprintf(stderr, "dsco agents create: incomplete organization binding\n");
            return 2;
        }
        if (!open_agents_db("agents-cli")) {
            fprintf(stderr, "dsco agents: could not open durable bus\n");
            return 1;
        }
        int depth = 0;
        if (parent && parent[0]) {
            ipc_agent_info_t p;
            depth = ipc_get_agent(parent, &p) ? p.depth + 1 : 1;
        }
        bool ok = ipc_agent_define_bound(id, parent, depth, role, model, toolkit,
                                         has_binding ? &binding : NULL);
        ipc_shutdown();
        if (!ok) {
            fprintf(stderr, "dsco agents: failed to create %s\n", id);
            return 1;
        }
        printf("agent=%s status=durable parent=%s role=%s model=%s\n", id,
               parent && parent[0] ? parent : "-", role && role[0] ? role : "-",
               model && model[0] ? model : "-");
        return 0;
    }

    if (strcmp(sub, "list") == 0) {
        bool json = has_flag(argc - 3, argv + 3, "--json");
        if (!open_agents_db("agents-cli")) {
            fprintf(stderr, "dsco agents: could not open durable bus\n");
            return 1;
        }
        ipc_agent_info_t agents[256];
        int n = ipc_list_agents(agents, 256);
        if (json) {
            jbuf_t b;
            jbuf_init(&b, 4096);
            jbuf_append(&b, "[");
            for (int i = 0; i < n; i++) {
                if (i)
                    jbuf_append(&b, ",");
                append_agent_json(&b, &agents[i]);
            }
            jbuf_append(&b, "]");
            puts(b.data);
            jbuf_free(&b);
        } else {
            for (int i = 0; i < n; i++)
                print_agent_text(&agents[i]);
        }
        ipc_shutdown();
        return 0;
    }

    if (strcmp(sub, "status") == 0) {
        if (argc < 4) {
            agents_usage(stderr, argv[0]);
            return 2;
        }
        bool json = has_flag(argc - 4, argv + 4, "--json");
        if (!open_agents_db("agents-cli")) {
            fprintf(stderr, "dsco agents: could not open durable bus\n");
            return 1;
        }
        ipc_agent_info_t a;
        bool ok = ipc_get_agent(argv[3], &a);
        if (ok && json) {
            jbuf_t b;
            jbuf_init(&b, 1024);
            append_agent_json(&b, &a);
            puts(b.data);
            jbuf_free(&b);
        } else if (ok) {
            print_agent_text(&a);
        }
        ipc_shutdown();
        if (!ok) {
            fprintf(stderr, "dsco agents: unknown agent %s\n", argv[3]);
            return 1;
        }
        return 0;
    }

    if (strcmp(sub, "send") == 0) {
        const char *from = arg_value(argc - 3, argv + 3, "--from", NULL);
        const char *to = arg_value(argc - 3, argv + 3, "--to", "all");
        const char *topic = arg_value(argc - 3, argv + 3, "--topic", "general");
        if (!from || !from[0]) {
            fprintf(stderr, "dsco agents send: --from ID is required\n");
            return 2;
        }
        char *body = join_message_args(argc, argv, 3);
        if (!body[0]) {
            free(body);
            fprintf(stderr, "dsco agents send: message body is required\n");
            return 2;
        }
        if (!open_agents_db(from)) {
            free(body);
            fprintf(stderr, "dsco agents: could not open durable bus\n");
            return 1;
        }
        const char *target = (!to || strcmp(to, "all") == 0 || strcmp(to, "*") == 0) ? NULL : to;
        bool ok = ipc_send(target, topic, body);
        ipc_shutdown();
        printf("%s from=%s to=%s topic=%s\n", ok ? "sent" : "FAILED", from,
               target ? target : "all", topic);
        free(body);
        return ok ? 0 : 1;
    }

    if (strcmp(sub, "inbox") == 0 || strcmp(sub, "sent") == 0 || strcmp(sub, "bus") == 0) {
        bool json = has_flag(argc - 3, argv + 3, "--json");
        int limit = parse_limit(argc - 3, argv + 3, 32);
        ipc_message_t msgs[256];
        if (!open_agents_db("agents-cli")) {
            fprintf(stderr, "dsco agents: could not open durable bus\n");
            return 1;
        }
        int n = 0;
        if (strcmp(sub, "bus") == 0) {
            n = ipc_list_bus(msgs, limit);
        } else {
            if (argc < 4) {
                ipc_shutdown();
                agents_usage(stderr, argv[0]);
                return 2;
            }
            if (strcmp(sub, "inbox") == 0) {
                bool unread_only = !has_flag(argc - 4, argv + 4, "--all");
                bool mark_read = has_flag(argc - 4, argv + 4, "--mark-read");
                n = ipc_list_inbox(argv[3], unread_only, mark_read, msgs, limit);
            } else {
                n = ipc_list_sent(argv[3], msgs, limit);
            }
        }
        ipc_shutdown();
        return print_messages(msgs, n, json);
    }

    agents_usage(stderr, argv[0]);
    return 2;
}
