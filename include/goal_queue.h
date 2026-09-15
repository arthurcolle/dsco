#ifndef DSCO_GOAL_QUEUE_H
#define DSCO_GOAL_QUEUE_H

/*
 * goal_queue.h — bounded two-queue controller for hierarchical goals
 *
 * Planning tasks may decompose into planning or work children.  Only planning
 * tasks enter plan_queue; only executable leaves enter work_queue.  A parent
 * returns to the planning queue after every child is terminal so it can verify,
 * recover, or decompose again.  The model receives one leased task at a time.
 */

#include "json_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GOAL_QUEUE_MAX_TASKS 32
#define GOAL_QUEUE_MAX_CHILDREN 12
#define GOAL_QUEUE_MAX_DEPTH 8
#define GOAL_QUEUE_TITLE_MAX 256
#define GOAL_QUEUE_ACCEPTANCE_MAX 384
#define GOAL_QUEUE_EVIDENCE_MAX 512
#define GOAL_QUEUE_REASON_MAX 256
#define GOAL_QUEUE_PROMPT_MAX 12288

#define GOAL_QUEUE_SCHEMA_VERSION 1
#define GOAL_QUEUE_TOOL_SCHEMA                                                        \
    "{\"type\":\"object\",\"properties\":{"                                  \
    "\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"decompose\"," \
    "\"checkpoint\",\"complete\",\"fail\",\"block\"],"                    \
    "\"description\":\"Exact fields: status=action only; decompose=action,task_id,revision,children (no evidence/reason); checkpoint or complete=action,task_id,revision,evidence; fail or block=action,task_id,revision,evidence,reason. Root must be decomposed before completion.\"}," \
    "\"task_id\":{\"type\":\"integer\",\"minimum\":1,"                    \
    "\"description\":\"Required for mutations: currently leased task from the latest controller context/current_id; never guess the next child ID\"}," \
    "\"revision\":{\"type\":\"integer\",\"minimum\":1,"                   \
    "\"description\":\"Required for mutations: exact controller revision, not goal revision. Leasing also increments it; never predict the next value.\"}," \
    "\"evidence\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":511," \
    "\"description\":\"Concise verification evidence; UTF-8 encoding must fit 511 bytes\"}," \
    "\"reason\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":255,"   \
    "\"description\":\"Required only for fail or block; UTF-8 encoding must fit 255 bytes\"}," \
    "\"children\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":8,"   \
    "\"items\":{\"type\":\"object\",\"properties\":{"                     \
    "\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":255},"  \
    "\"lane\":{\"type\":\"string\",\"enum\":[\"plan\",\"work\"]},"    \
    "\"acceptance\":{\"type\":\"string\",\"maxLength\":383},"             \
    "\"max_attempts\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8," \
    "\"description\":\"Lease ceiling; plan tasks require at least 2 so one review lease remains\"}" \
    "},\"required\":[\"title\",\"lane\"],\"additionalProperties\":false}}" \
    "},\"required\":[\"action\"],\"additionalProperties\":false}"

typedef enum {
    GOAL_QUEUE_LANE_PLAN = 0,
    GOAL_QUEUE_LANE_WORK = 1,
} goal_queue_lane_t;

typedef enum {
    GOAL_QUEUE_TASK_UNUSED = 0,
    GOAL_QUEUE_TASK_QUEUED,
    GOAL_QUEUE_TASK_ACTIVE,
    GOAL_QUEUE_TASK_WAITING,
    GOAL_QUEUE_TASK_COMPLETE,
    GOAL_QUEUE_TASK_FAILED,
    GOAL_QUEUE_TASK_BLOCKED,
} goal_queue_task_status_t;

typedef struct {
    int id;
    int parent_id;
    int depth;
    int child_count;
    int attempts;
    int max_attempts;
    goal_queue_lane_t lane;
    goal_queue_task_status_t status;
    char title[GOAL_QUEUE_TITLE_MAX];
    char acceptance[GOAL_QUEUE_ACCEPTANCE_MAX];
    char evidence[GOAL_QUEUE_EVIDENCE_MAX];
    char reason[GOAL_QUEUE_REASON_MAX];
} goal_queue_task_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool terminal_ready;
    uint64_t objective_hash;
    long long goal_started_at;
    int revision;
    int next_id;
    int root_id;
    int current_id;
    goal_queue_lane_t last_lane;
    int task_count;
    goal_queue_task_t tasks[GOAL_QUEUE_MAX_TASKS];
    int plan_queue[GOAL_QUEUE_MAX_TASKS];
    int plan_count;
    int work_queue[GOAL_QUEUE_MAX_TASKS];
    int work_count;
} goal_queue_t;

void goal_queue_init(goal_queue_t *queue);
void goal_queue_clear(goal_queue_t *queue);

/* Reset on a new/edited objective; retain state across pause/resume and load. */
bool goal_queue_sync(goal_queue_t *queue, const char *objective,
                     long long goal_started_at, bool active);

/* Lease the next task using fair alternation when both queues are non-empty. */
bool goal_queue_prepare(goal_queue_t *queue);

/* Build the model-facing next-action contract.  This calls prepare(). */
bool goal_queue_make_prompt(goal_queue_t *queue, const char *objective,
                            const char *criteria, char *out, size_t out_len);

/* Non-mutating context for request builders after prepare(). */
bool goal_queue_make_context(const goal_queue_t *queue, const char *objective,
                             const char *criteria, char *out, size_t out_len);

/* Strict model-facing state transition handler. */
bool goal_queue_apply(goal_queue_t *queue, const char *input_json,
                      char *out, size_t out_len);
bool goal_queue_status_json(const goal_queue_t *queue, char *out, size_t out_len);

bool goal_queue_terminal_ready(const goal_queue_t *queue);
bool goal_queue_root_blocked(const goal_queue_t *queue);
const char *goal_queue_root_evidence(const goal_queue_t *queue);
const char *goal_queue_root_reason(const goal_queue_t *queue);

/* Session JSON integration.  Active leases are requeued when loaded. */
void goal_queue_save_fields(jbuf_t *buf, const goal_queue_t *queue);
bool goal_queue_load_fields(goal_queue_t *queue, const char *session_json);

const char *goal_queue_lane_name(goal_queue_lane_t lane);
const char *goal_queue_task_status_name(goal_queue_task_status_t status);

#endif /* DSCO_GOAL_QUEUE_H */
