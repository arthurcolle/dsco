#ifndef DSCO_ROLLOUT_H
#define DSCO_ROLLOUT_H

/* ── Cached trial-rollout trajectory optimizer ─────────────────────────────
 *
 * Finds optimal action trajectories by trial, with a transposition cache so
 * shared rollout prefixes are computed once, and a regime gate so REAL-WORLD
 * side effects are never speculatively replayed.
 *
 * Relationship to existing DSCO infra:
 *   - plan_optimizer.h  : ranks *topologies* for a task (coarse, one-shot).
 *   - plan_cache.h      : caches task -> best plan/topology (fuzzy replay).
 *   - rollout.h (this)  : searches the *action tree* to PRODUCE the trajectory
 *                         that plan_cache then stores and replays. It is the
 *                         generator that feeds plan_cache_store_json().
 *
 * Regime model (umbra / penumbra / antumbra):
 *   ROLLOUT_UMBRA     reversible / read-only  -> executed for real, cached.
 *   ROLLOUT_PENUMBRA  costly-but-reversible   -> simulated by world model;
 *                                                promoted to real exec only on
 *                                                the selected trajectory.
 *   ROLLOUT_ANTUMBRA  irreversible / external -> NEVER speculatively executed;
 *                                                estimated by model, emitted as
 *                                                a gated step requiring authority.
 *
 * Determinism & safety invariants:
 *   1. execute_fn is invoked ONLY for ROLLOUT_UMBRA actions during search.
 *   2. Every (state,action) transition is memoized in a content-addressed
 *      transposition cache keyed on (state_hash, action_key).
 *   3. The returned trajectory partitions steps into realized / needs_realize
 *      (penumbra) / needs_authority (antumbra) so the caller can stage them.
 *   4. Optional durable JSONL caching requires scoped namespace and state
 *      serialization callbacks; opaque pointer values are never persisted.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque user state handle. The engine treats state as an opaque blob plus a
 * caller-provided stable hash; it never introspects the bytes. Callback state
 * inputs are immutable/borrowed; clone before modifying them. */
typedef void *rollout_state_t;
typedef void *rollout_action_t;

typedef enum {
    ROLLOUT_UMBRA    = 0, /* reversible: real-execute + cache */
    ROLLOUT_PENUMBRA = 1, /* uncertain/costly-reversible: simulate */
    ROLLOUT_ANTUMBRA = 2, /* irreversible/consequential: gated, model-only */
} rollout_regime_t;

/* Transition outcome returned by execute/model callbacks. next_state ownership
 * transfers to the engine, which frees it via env.free_state_fn. */
typedef struct {
    rollout_state_t next_state;
    double          reward;
    char           *observation_json; /* optional; engine takes ownership, may be NULL */
} rollout_step_result_t;

/* Environment binding — the caller supplies the world. */
typedef struct {
    rollout_state_t initial_state;

    /* enumerate legal actions from a state; returns count, fills *out (engine
     * owns neither the array nor the actions — see action lifetime note). */
    int (*actions_fn)(rollout_state_t s, rollout_action_t *out, int max, void *ud);

    /* REAL execution — called only for ROLLOUT_UMBRA actions. */
    rollout_step_result_t (*execute_fn)(rollout_state_t s, rollout_action_t a, void *ud);

    /* SIMULATION — world model for penumbra/antumbra (no side effects). */
    rollout_step_result_t (*model_fn)(rollout_state_t s, rollout_action_t a, void *ud);

    rollout_regime_t (*regime_fn)(rollout_action_t a, void *ud);
    bool  (*is_terminal_fn)(rollout_state_t s, void *ud);

    /* stable content hashes (hex, NUL-terminated) into caller buffers. */
    void  (*state_hash_fn)(rollout_state_t s, char *out, size_t out_len, void *ud);
    void  (*action_key_fn)(rollout_action_t a, char *out, size_t out_len, void *ud);

    double (*action_cost_fn)(rollout_action_t a, void *ud); /* real cost units */

    /* lifecycle for engine-owned copies of state/action (may be NULL if the
     * caller guarantees immortality of states within a search). */
    rollout_state_t  (*clone_state_fn)(rollout_state_t s, void *ud);
    void             (*free_state_fn)(rollout_state_t s, void *ud);
    rollout_action_t (*clone_action_fn)(rollout_action_t a, void *ud);
    void             (*free_action_fn)(rollout_action_t a, void *ud);

    void *user_data;

    /* Real execution requires explicit authorization, including cache reuse.
     * Tool adapters MUST route execution through tools_execute_for_tier().
     * Returning false stops that transition. Model callbacks have no effects. */
    bool (*authorize_fn)(rollout_state_t s, rollout_action_t a,
                         rollout_regime_t regime, bool realizing, void *ud);
    /* Optional cooperative cancellation, checked before transitions. */
    bool (*cancelled_fn)(void *ud);
    /* Required together with cache_namespace when cfg.cache_path is set.
     * Serializer returns owned text; deserializer returns engine-owned state.
     * Scope/version namespace must include environment and principal identity.
     * Only trusted local cache files may be used; not an authenticated store. */
    char *(*serialize_state_fn)(rollout_state_t s, void *ud);
    rollout_state_t (*deserialize_state_fn)(const char *json, void *ud);
    const char *cache_namespace;
} rollout_env_t;

/* Search configuration. */
typedef struct {
    int    iterations;      /* MCTS iterations (default 400); iterations*depth <= 65536 */
    int    rollout_depth;   /* search horizon (default 12, maximum 128) */
    double c_uct;           /* exploration constant (default sqrt(2)) */
    double gamma;           /* discount (default 0.98) */
    int    max_actions;     /* cap on branching per node (default 32, maximum 256) */
    const char *cache_path; /* durable transposition cache (NULL = memory-only) */
    uint64_t rng_seed;      /* reproducible search */
} rollout_config_t;

void rollout_config_defaults(rollout_config_t *cfg);

/* One step of the winning trajectory. */
typedef struct {
    rollout_action_t action;   /* borrowed; valid until rollout_result_free */
    rollout_regime_t regime;
    double reward;
    int    visits;
    double q_value;
    bool   realized;           /* true iff regime==UMBRA (already executed) */
} rollout_traj_step_t;

typedef enum {
    ROLLOUT_STATUS_OK = 0, ROLLOUT_STATUS_CANCELLED,
    ROLLOUT_STATUS_DENIED, ROLLOUT_STATUS_CALLBACK_ERROR
} rollout_status_t;

typedef struct {
    rollout_traj_step_t *steps;
    int    step_count;
    double total_reward;

    /* staging partitions (indices into steps[]) */
    int   *needs_realization;  /* penumbra steps to execute for real next */
    int    needs_realization_count;
    int   *needs_authority;    /* antumbra steps requiring principal approval */
    int    needs_authority_count;

    /* cache telemetry */
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t real_executions;
    uint64_t simulated_executions;
    double   cost_paid;
    double   cost_saved;       /* attributed to authorized UMBRA cache hits */
    rollout_status_t status;
    uint64_t cache_errors;
    int iterations_done;
    void *engine_private;      /* owned; do not inspect/copy */
} rollout_result_t;

/* Run bounded discounted UCT search. Returns owned result, or NULL with errno
 * for invalid bindings/configuration or allocation failure. NULL free is safe.
 * Action clone/free must be paired; without them, caller actions MUST remain
 * alive through result_free. Clone/free state callbacks must also be paired.
 * env.user_data must remain valid through result_free. Cache failures are
 * reported and fall back to in-memory memoization; cancellation/denial stop.
 * Search callbacks for real tools remain governed by their supplied adapter. */
rollout_result_t *rollout_search(const rollout_env_t *env, const rollout_config_t *cfg);
void              rollout_result_free(rollout_result_t *r);

/* Serialize the winning trajectory to plan JSON suitable for
 * plan_cache_store_json(), so a discovered optimum is replayable across
 * sessions via the existing fuzzy plan cache. Returns malloc'd JSON. */
char *rollout_result_to_plan_json(const rollout_result_t *r, const char *task);

/* Realize only an action CURRENTLY classified PENUMBRA, after authorize_fn.
 * ANTUMBRA is always refused here. The caller owns returned state/observation;
 * failure/denial/cancellation returns {0} and sets errno. No automatic retries. */
rollout_step_result_t rollout_realize_step(const rollout_env_t *env,
                                           rollout_state_t s, rollout_action_t a);

#endif /* DSCO_ROLLOUT_H */
