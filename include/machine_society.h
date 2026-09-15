#ifndef DSCO_MACHINE_SOCIETY_H
#define DSCO_MACHINE_SOCIETY_H

#include "json_util.h"
#include "swarm.h"

#include <stdbool.h>
#include <stdio.h>

#define MACHINE_SOCIETY_MAX_BRIEF_CLAIMS 16
#define MACHINE_SOCIETY_MAX_BRIEF_REPLIES 16
#define MACHINE_SOCIETY_MAX_TRACKED_CLAIMS 256
#define MACHINE_SOCIETY_CLAIM_ID_LEN 96

typedef struct {
    int claim_count;
    int reply_count;
    int dissent_count;
    int artifact_count;
    bool veto;
    bool next_round_requested;
    double confidence_sum;
    char claim_ids[MACHINE_SOCIETY_MAX_BRIEF_CLAIMS][MACHINE_SOCIETY_CLAIM_ID_LEN];
    char reply_targets[MACHINE_SOCIETY_MAX_BRIEF_REPLIES][MACHINE_SOCIETY_CLAIM_ID_LEN];
} machine_society_brief_stats_t;

typedef struct {
    int valid_briefs;
    int rejected_briefs;
    int oversized_briefs;
    int total_claims;
    int unique_claims;
    int revised_claims;
    int replies;
    int unresolved_signals;
    int invalid_reply_targets;
    double mean_confidence;
    char claim_ids[MACHINE_SOCIETY_MAX_TRACKED_CLAIMS][MACHINE_SOCIETY_CLAIM_ID_LEN];
    char claim_owners[MACHINE_SOCIETY_MAX_TRACKED_CLAIMS][64];
    int claim_rounds[MACHINE_SOCIETY_MAX_TRACKED_CLAIMS];
} machine_society_board_stats_t;

typedef struct {
    bool continue_deliberation;
    int marginal_claims;
    double voi_proxy;
    char reason[64];
} machine_society_round_decision_t;

typedef struct {
    const char *provider;
    const char *model;
    const char *correlation_group;
    int base_score;
    double quality;
    double reserve_cost_usd;
    double latency_sec;
    bool subsidized;
} machine_society_candidate_t;

/* A machine society is a bounded, operator-visible deliberation protocol.
 * Members never discover one another through ambient shared infrastructure:
 * the parent publishes a typed board between rounds and remains the sole
 * authority for spawning, budgeting, and termination. */

typedef struct {
    int round;
    int rounds;
    int member_count;
    int active;
    int done;
    int failed;
    double elapsed_sec;
    double remaining_sec;
    double time_budget_sec;
    double budget_usd;
    double estimated_metered_usd;
    double estimated_subsidized_usd;
    double reserved_metered_usd;
    double reserved_subsidized_usd;
    double committed_metered_usd;
    double accrued_metered_usd;
    double accrued_subsidized_usd;
    double estimated_unreported_metered_usd;
    int calibrated_reservations;
    int heuristic_reservations;
    int unpriced_subsidized_calls;
    double remaining_budget_usd;
    double uncommitted_budget_usd;
} machine_society_telemetry_t;

const char *machine_society_role_for_index(int index);

const char *machine_society_public_brief_schema_json(void);
bool machine_society_build_public_brief_schema(jbuf_t *out, const char *society_id,
                                               const char *member_id, int round);

bool machine_society_extract_public_brief(const char *raw, const char *expected_society_id,
                                          const char *expected_member_id, int expected_round,
                                          size_t max_bytes, jbuf_t *canonical,
                                          machine_society_brief_stats_t *stats, char *error,
                                          size_t error_len);

int machine_society_select_portfolio(const machine_society_candidate_t *candidates,
                                     int candidate_count, int max_members, double budget_usd,
                                     int *selected, double *objective_out);

void machine_society_append_member_prompt(jbuf_t *out, const char *task, const char *society_id,
                                          const char *member_id, const char *role,
                                          const char *provider, const char *model, int round,
                                          int rounds, const char *public_board);

void machine_society_append_public_board(jbuf_t *out, swarm_t *swarm, const int *member_ids,
                                         int message_count, int stable_member_count,
                                         size_t per_member_cap);

void machine_society_append_public_board_typed(
    jbuf_t *out, swarm_t *swarm, const int *child_ids, const int *member_indices,
    const int *message_rounds, int message_count, const char *society_id, size_t per_member_cap,
    machine_society_board_stats_t *stats);

void machine_society_append_chair_prompt(jbuf_t *out, const char *task, const char *society_id,
                                         const char *public_board, int rounds_completed,
                                         int member_count);

void machine_society_measure(swarm_t *swarm, const int *member_ids, int member_count, int round,
                             int rounds, double started_at, double now, double deadline_at,
                             double budget_usd, machine_society_telemetry_t *out);

bool machine_society_budget_allows(const machine_society_telemetry_t *telemetry,
                                   double next_estimate_usd, bool subsidized);

bool machine_society_should_continue(const machine_society_board_stats_t *current,
                                     const machine_society_board_stats_t *previous,
                                     int completed_round, int max_rounds, int min_rounds,
                                     int min_new_claims, double remaining_sec,
                                     double chair_reserve_sec,
                                     machine_society_round_decision_t *decision);

void machine_society_render_live(FILE *stream, const char *society_id,
                                 const machine_society_telemetry_t *telemetry);

#endif
