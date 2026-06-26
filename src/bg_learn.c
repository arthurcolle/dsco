#include "bg_learn.h"

#include "baseline.h"
#include "self_improve.h"
#include "tools.h"
#include "workspace.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_bg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_bg_thread;
static bool g_bg_thread_started = false;
static bool g_bg_running = false;
static bool g_bg_enabled = false;
static unsigned g_bg_interval_sec = 60;
static bg_learn_stats_t g_bg_stats = {0};

static bool env_truthy(const char *s) {
    if (!s || !*s)
        return false;
    char buf[16];
    size_t n = strlen(s);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = '\0';
    return strcmp(buf, "1") == 0 || strcmp(buf, "true") == 0 ||
           strcmp(buf, "yes") == 0 || strcmp(buf, "on") == 0;
}

static void refresh_env_locked(void) {
    const char *enabled = getenv("DSCO_BG_LEARN");
    if (enabled)
        g_bg_enabled = env_truthy(enabled);
    const char *interval = getenv("DSCO_BG_LEARN_INTERVAL");
    if (interval && *interval) {
        long v = strtol(interval, NULL, 10);
        if (v >= 1 && v <= 86400)
            g_bg_interval_sec = (unsigned)v;
    }
    g_bg_stats.enabled = g_bg_enabled;
    g_bg_stats.running = g_bg_running;
    g_bg_stats.interval_sec = g_bg_interval_sec;
}

static void skill_name_part(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t pos = 0;
    bool dash = false;
    for (const char *p = in ? in : ""; *p && pos + 1 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            out[pos++] = (char)tolower(c);
            dash = false;
        } else if (!dash && pos > 0) {
            out[pos++] = '-';
            dash = true;
        }
    }
    while (pos > 0 && out[pos - 1] == '-')
        pos--;
    if (pos == 0) {
        snprintf(out, out_len, "tool");
        return;
    }
    out[pos] = '\0';
}

static void edge_skill_name(const tools_cooc_edge_t *edge, char *out, size_t out_len) {
    char from[48];
    char to[48];
    skill_name_part(edge ? edge->from : "", from, sizeof(from));
    skill_name_part(edge ? edge->to : "", to, sizeof(to));
    snprintf(out, out_len, "auto-%s-%s", from, to);
}

static void edge_skill_body(const tools_cooc_edge_t *edge, char *out, size_t out_len) {
    snprintf(out, out_len,
             "# Auto Skill: %s -> %s\n\n"
             "<!-- dsco:auto-generated -->\n\n"
             "Mined by the background learner from the tool co-occurrence matrix. "
             "This skill is advisory and may be pruned during skill consolidation.\n\n"
             "## Pattern\n\n"
             "- Tool `%s` was frequently followed by `%s`.\n"
             "- Observed co-occurrence count: %u.\n\n"
             "## Use\n\n"
             "When `%s` appears in a workflow, consider whether `%s` is the next useful step.\n",
             edge->from, edge->to, edge->from, edge->to, edge->count, edge->from, edge->to);
}

void bg_learn_set_enabled(bool enabled) {
    pthread_mutex_lock(&g_bg_mu);
    g_bg_enabled = enabled;
    g_bg_stats.enabled = enabled;
    pthread_mutex_unlock(&g_bg_mu);
}

bool bg_learn_is_enabled(void) {
    pthread_mutex_lock(&g_bg_mu);
    bool enabled = g_bg_enabled;
    pthread_mutex_unlock(&g_bg_mu);
    return enabled;
}

int bg_learn_run_once(void) {
    tools_cooc_edge_t edges[8];
    int n = tools_cooc_top_edges(edges, 8);
    int created = 0;

    for (int i = 0; i < n; i++) {
        if (edges[i].count < 2)
            continue;
        char name[128];
        char body[2048];
        edge_skill_name(&edges[i], name, sizeof(name));
        if (dsco_workspace_skill_exists(name))
            continue;
        edge_skill_body(&edges[i], body, sizeof(body));
        if (dsco_workspace_create_skill(name, body, false) > 0) {
            created++;
            pthread_mutex_lock(&g_bg_mu);
            g_bg_stats.skills_created++;
            snprintf(g_bg_stats.last_skill_write, sizeof(g_bg_stats.last_skill_write), "%s", name);
            pthread_mutex_unlock(&g_bg_mu);
            baseline_log("bg_learn", "skill_created", name, "{\"source\":\"cooc\"}");
        }
    }

    if (g_self_improve.initialized)
        self_improve_consolidate(&g_self_improve);

    pthread_mutex_lock(&g_bg_mu);
    g_bg_stats.cycles++;
    g_bg_stats.last_run_epoch = (double)time(NULL);
    g_bg_stats.auto_skill_count = dsco_workspace_count_auto_skills();
    pthread_mutex_unlock(&g_bg_mu);
    return created;
}

static void *bg_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_bg_mu);
        bool running = g_bg_running;
        bool enabled = g_bg_enabled;
        unsigned interval = g_bg_interval_sec ? g_bg_interval_sec : 60;
        pthread_mutex_unlock(&g_bg_mu);
        if (!running)
            break;
        if (enabled)
            bg_learn_run_once();
        for (unsigned i = 0; i < interval; i++) {
            pthread_mutex_lock(&g_bg_mu);
            running = g_bg_running;
            pthread_mutex_unlock(&g_bg_mu);
            if (!running)
                break;
            sleep(1);
        }
    }
    return NULL;
}

bool bg_learn_start(void) {
    pthread_mutex_lock(&g_bg_mu);
    refresh_env_locked();
    if (g_bg_thread_started) {
        g_bg_running = true;
        g_bg_stats.running = true;
        pthread_mutex_unlock(&g_bg_mu);
        return true;
    }
    g_bg_running = true;
    g_bg_stats.running = true;
    int rc = pthread_create(&g_bg_thread, NULL, bg_thread_main, NULL);
    if (rc == 0) {
        g_bg_thread_started = true;
        pthread_mutex_unlock(&g_bg_mu);
        return true;
    }
    g_bg_running = false;
    g_bg_stats.running = false;
    pthread_mutex_unlock(&g_bg_mu);
    return false;
}

void bg_learn_stop(void) {
    pthread_mutex_lock(&g_bg_mu);
    bool join = g_bg_thread_started;
    g_bg_running = false;
    g_bg_stats.running = false;
    pthread_t thread = g_bg_thread;
    pthread_mutex_unlock(&g_bg_mu);

    if (join) {
        pthread_join(thread, NULL);
        pthread_mutex_lock(&g_bg_mu);
        g_bg_thread_started = false;
        memset(&g_bg_thread, 0, sizeof(g_bg_thread));
        pthread_mutex_unlock(&g_bg_mu);
    }
}

void bg_learn_stats(bg_learn_stats_t *out) {
    if (!out)
        return;
    pthread_mutex_lock(&g_bg_mu);
    refresh_env_locked();
    g_bg_stats.auto_skill_count = dsco_workspace_count_auto_skills();
    *out = g_bg_stats;
    pthread_mutex_unlock(&g_bg_mu);
}
