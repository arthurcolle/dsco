#ifndef DSCO_ENV_SENSE_H
#define DSCO_ENV_SENSE_H

#include <stdbool.h>

#include "goal_queue.h"

/* Environment sensing: the tree grows from observation.
 *
 * A goal tree fed only by operator commands is a to-do list; a living system
 * also notices. This pass runs cheap, read-only probes of the working
 * environment and proposes work units for what it finds — a build that no
 * longer compiles, a test binary older than its source, uncommitted changes
 * piling up past a threshold. Proposals land in the goal tree as ordinary
 * work-lane tasks with acceptance commands, so the mechanical dispatcher and
 * the retry policy treat them exactly like operator-decomposed work.
 *
 * Two hard rules keep this from becoming noise or a runaway:
 *
 *   1. OBSERVATIONS PROPOSE; the tree DISPOSES. A sensor never executes
 *      anything — it only creates queued tasks, capped per pass, deduplicated
 *      by observation fingerprint so the same finding is never planted twice.
 *
 *   2. Read-only probes. Sensors may stat, read, and run read-only commands;
 *      any mutation happens later, through the dispatcher, through the
 *      governance gate, as a normal work unit.
 *
 * Sensors are compiled in but off by default; DSCO_ENV_SENSE=1 enables the
 * pass (systems-agent posture may default it on at the call site). */

#define ENV_SENSE_MAX_PROPOSALS_PER_PASS 3
#define ENV_SENSE_FINGERPRINT_MAX 64
#define ENV_SENSE_SEEN_MAX 64

typedef struct {
    char fingerprint[ENV_SENSE_FINGERPRINT_MAX]; /* dedup key                */
    char title[GOAL_QUEUE_TITLE_MAX];
    char acceptance[GOAL_QUEUE_ACCEPTANCE_MAX];
    char work_unit_json[GOAL_QUEUE_RESULT_MAX];  /* optional typed unit      */
    char acceptance_cmd[GOAL_QUEUE_ACCEPT_CMD_MAX];
} env_proposal_t;

typedef struct {
    /* Fingerprints already planted this session, so a persistent condition
     * (e.g. the same dirty file) does not spawn a task per pass. */
    char seen[ENV_SENSE_SEEN_MAX][ENV_SENSE_FINGERPRINT_MAX];
    int seen_count;
    long long last_pass_ms;   /* wall clock of the last completed pass       */
    int passes;
    int planted_total;
} env_sense_state_t;

void env_sense_init(env_sense_state_t *st);

/* Whether sensing is enabled for this process (DSCO_ENV_SENSE truthy). */
bool env_sense_enabled(void);

/* Run one sensing pass over `workdir` (NULL = cwd). Proposals that survive
 * dedup are appended to `q` as children of the root task, work lane, queued.
 * Returns the number of tasks planted. Rate-limited internally: passes more
 * frequent than min_interval_ms apart are no-ops. */
int env_sense_pass(env_sense_state_t *st, goal_queue_t *q, const char *workdir,
                   long long min_interval_ms);

/* Individual sensors, exposed for tests. Each fills up to `cap` proposals
 * and returns the count. All probes are read-only. */
int env_sense_git(const char *workdir, env_proposal_t *out, int cap);
int env_sense_build(const char *workdir, env_proposal_t *out, int cap);

#endif /* DSCO_ENV_SENSE_H */
