/* Environment sensing. See include/env_sense.h. */

#include "env_sense.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>

void env_sense_init(env_sense_state_t *st) {
    if (st)
        memset(st, 0, sizeof(*st));
}

bool env_sense_enabled(void) {
    const char *v = getenv("DSCO_ENV_SENSE");
    return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
           strcasecmp(v, "off") != 0;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Run a read-only command in `workdir`, capture first line of stdout.
 * Returns exit status, -1 on popen failure. */
static int probe(const char *workdir, const char *cmd, char *out, size_t out_len) {
    char full[1024];
    snprintf(full, sizeof(full), "cd '%s' 2>/dev/null && { %s ; } 2>/dev/null",
             workdir ? workdir : ".", cmd);
    FILE *p = popen(full, "r");
    if (!p)
        return -1;
    if (out && out_len) {
        out[0] = '\0';
        if (fgets(out, (int)out_len, p)) {
            size_t n = strlen(out);
            while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
                out[--n] = '\0';
        }
    }
    return pclose(p);
}

/* ── Sensor: git state ──────────────────────────────────────────────────── */

int env_sense_git(const char *workdir, env_proposal_t *out, int cap) {
    if (!out || cap < 1)
        return 0;
    int n = 0;
    char line[256];

    /* Not a repo? Nothing to sense. */
    if (probe(workdir, "git rev-parse --is-inside-work-tree", line, sizeof(line)) != 0 ||
        strcmp(line, "true") != 0)
        return 0;

    /* Large uncommitted drift: propose a checkpoint commit REVIEW (a prompt
     * task — committing is judgment, not mechanics: the model must decide
     * what belongs together and write the message). */
    if (n < cap &&
        probe(workdir, "git status --porcelain | wc -l | tr -d ' '", line, sizeof(line)) == 0) {
        int dirty = atoi(line);
        if (dirty >= 25) {
            env_proposal_t *p = &out[n];
            memset(p, 0, sizeof(*p));
            /* Fingerprint buckets by 25 so steady growth re-proposes only on
             * crossing another threshold, not every pass. */
            snprintf(p->fingerprint, sizeof(p->fingerprint), "git-dirty-%d", dirty / 25);
            snprintf(p->title, sizeof(p->title),
                     "Review %d uncommitted changes and group them into commits", dirty);
            snprintf(p->acceptance, sizeof(p->acceptance),
                     "Working tree reviewed; related changes committed with clear messages "
                     "or explicitly deferred with a reason.");
            n++;
        }
    }

    /* Merge conflicts are always work worth planting. */
    if (n < cap &&
        probe(workdir, "git diff --name-only --diff-filter=U | wc -l | tr -d ' '", line,
              sizeof(line)) == 0 &&
        atoi(line) > 0) {
        env_proposal_t *p = &out[n];
        memset(p, 0, sizeof(*p));
        snprintf(p->fingerprint, sizeof(p->fingerprint), "git-conflicts");
        snprintf(p->title, sizeof(p->title), "Resolve %d merge-conflicted file(s)", atoi(line));
        snprintf(p->acceptance, sizeof(p->acceptance), "No unmerged paths remain.");
        /* Acceptance commands run from the agent's cwd, which need not be the
         * sensed workdir — anchor explicitly or the check tests the wrong
         * repository. */
        snprintf(p->acceptance_cmd, sizeof(p->acceptance_cmd),
                 "cd '%s' && test \"$(git diff --name-only --diff-filter=U | wc -l | tr -d ' ')\" = 0",
                 workdir ? workdir : ".");
        n++;
    }
    return n;
}

/* ── Sensor: build health ───────────────────────────────────────────────── */

int env_sense_build(const char *workdir, env_proposal_t *out, int cap) {
    if (!out || cap < 1)
        return 0;
    char line[256];

    /* Only sense where there is a Makefile; other build systems can join
     * later. A dry-run answers "is anything out of date / broken" without
     * mutating (read-only rule). */
    char mk[1024];
    snprintf(mk, sizeof(mk), "%s/Makefile", workdir ? workdir : ".");
    struct stat st_mk;
    if (stat(mk, &st_mk) != 0)
        return 0;

    int rc = probe(workdir, "make -q 2>&1 | head -1", line, sizeof(line));
    /* make -q: exit 0 = up to date, 1 = work to do (normal), 2 = error.
     * Only a hard error is a finding — "needs rebuilding" is routine. */
    if (rc == -1)
        return 0;
    int exit_code = rc >= 256 ? rc / 256 : rc; /* popen status quirk tolerated */
    if (exit_code < 2)
        return 0;

    env_proposal_t *p = &out[0];
    memset(p, 0, sizeof(*p));
    snprintf(p->fingerprint, sizeof(p->fingerprint), "build-broken");
    snprintf(p->title, sizeof(p->title), "Build is broken: %.120s", line[0] ? line : "make -q exits 2");
    snprintf(p->acceptance, sizeof(p->acceptance), "make completes without error.");
    snprintf(p->acceptance_cmd, sizeof(p->acceptance_cmd), "make -q >/dev/null 2>&1 || make >/dev/null 2>&1");
    return 1;
}

/* ── The pass ───────────────────────────────────────────────────────────── */

static bool seen_before(env_sense_state_t *st, const char *fp) {
    for (int i = 0; i < st->seen_count; i++)
        if (strcmp(st->seen[i], fp) == 0)
            return true;
    return false;
}

static void mark_seen(env_sense_state_t *st, const char *fp) {
    if (st->seen_count < ENV_SENSE_SEEN_MAX) {
        snprintf(st->seen[st->seen_count], ENV_SENSE_FINGERPRINT_MAX, "%s", fp);
        st->seen_count++;
    } else {
        /* Ring: forget the oldest. A re-observed condition re-proposes after
         * 64 distinct findings — acceptable staleness for a session cache. */
        memmove(st->seen[0], st->seen[1],
                (ENV_SENSE_SEEN_MAX - 1) * ENV_SENSE_FINGERPRINT_MAX);
        snprintf(st->seen[ENV_SENSE_SEEN_MAX - 1], ENV_SENSE_FINGERPRINT_MAX, "%s", fp);
    }
}

/* Plant one proposal as a work-lane child of the root. Uses the queue's
 * internals directly (same translation unit family as goal_queue) via the
 * decompose-equivalent path: we synthesize the child like decompose does but
 * without consuming a plan lease — observations are additive, not part of the
 * model's decomposition contract. */
static bool plant(goal_queue_t *q, const env_proposal_t *p) {
    if (!q || !q->initialized || q->task_count >= GOAL_QUEUE_MAX_TASKS)
        return false;
    goal_queue_task_t *root = NULL;
    for (int i = 0; i < q->task_count; i++)
        if (q->tasks[i].id == q->root_id)
            root = &q->tasks[i];
    if (!root)
        return false;

    goal_queue_task_t *t = &q->tasks[q->task_count];
    memset(t, 0, sizeof(*t));
    t->id = q->next_id++;
    t->parent_id = q->root_id;
    t->depth = root->depth + 1;
    t->lane = GOAL_QUEUE_LANE_WORK;
    t->status = GOAL_QUEUE_TASK_QUEUED;
    t->max_attempts = 3;
    t->acceptance_exit = -1;
    snprintf(t->title, sizeof(t->title), "[sensed] %s", p->title);
    snprintf(t->acceptance, sizeof(t->acceptance), "%s", p->acceptance);
    if (p->work_unit_json[0])
        snprintf(t->work_unit_json, sizeof(t->work_unit_json), "%s", p->work_unit_json);
    if (p->acceptance_cmd[0])
        snprintf(t->acceptance_cmd, sizeof(t->acceptance_cmd), "%s", p->acceptance_cmd);
    q->task_count++;
    root->child_count++;

    /* Enqueue on the work lane. */
    if (q->work_count < GOAL_QUEUE_MAX_TASKS)
        q->work_queue[q->work_count++] = t->id;
    q->revision++;
    return true;
}

int env_sense_pass(env_sense_state_t *st, goal_queue_t *q, const char *workdir,
                   long long min_interval_ms) {
    if (!st || !q || !q->initialized)
        return 0;
    long long t = now_ms();
    if (st->last_pass_ms && t - st->last_pass_ms < min_interval_ms)
        return 0;
    st->last_pass_ms = t;
    st->passes++;

    env_proposal_t props[ENV_SENSE_MAX_PROPOSALS_PER_PASS * 2];
    int found = 0;
    found += env_sense_git(workdir, props + found,
                           (int)(sizeof(props) / sizeof(props[0])) - found);
    found += env_sense_build(workdir, props + found,
                             (int)(sizeof(props) / sizeof(props[0])) - found);

    int planted = 0;
    for (int i = 0; i < found && planted < ENV_SENSE_MAX_PROPOSALS_PER_PASS; i++) {
        if (seen_before(st, props[i].fingerprint))
            continue;
        if (plant(q, &props[i])) {
            mark_seen(st, props[i].fingerprint);
            planted++;
        }
    }
    st->planted_total += planted;
    return planted;
}
