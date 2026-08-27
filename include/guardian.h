/* guardian.h — gestalt consciousness contract for swarm runs (T11)
 *
 * One Guardian per run. Not per node, not per group. The Guardian is a
 * small dedicated hierarchy (tenure / perception / veto / succession cells
 * sharing one guardian_run_id covenant); lane pools are leased muscle.
 *
 * Laws implemented here (docs/GUARDIAN_GESTALT_LAW.md):
 *  L1 every executing process must be able to name its Guardian;
 *     anything that cannot is culled, not adopted.
 *  L2 exactly one tenure cell per run at any instant (activation lease).
 *  L3 one-signal veto: cancellation propagates to all leased lanes.
 *  L4 succession requires full ledger handoff; two-actives = fence trip.
 */
#ifndef DSCO_GUARDIAN_H
#define DSCO_GUARDIAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUARDIAN_Covenant_ID_LEN 128

typedef enum {
    GUARDIAN_ROLE_NONE = 0,
    GUARDIAN_ROLE_TENURE,      /* holds the activation lease; ledger owner */
    GUARDIAN_ROLE_PERCEPTION,  /* ingests envelope streams -> global state */
    GUARDIAN_ROLE_VETO,        /* killswitch authority; budget/policy gates */
    GUARDIAN_ROLE_SUCCESSION,  /* steward of handoff + promotion */
} guardian_role_t;

typedef struct {
    char   run_id[GUARDIAN_Covenant_ID_LEN];  /* shared covenant identity */
    int32_t role;                             /* guardian_role_t */
    uint64_t budget_grant_usd_micros;         /* spend ceiling granted by principal */
    uint64_t revocation_handle;               /* nonce: ledger refuses pre-revocation writes */
} guardian_covenant_t;

typedef struct {
    guardian_covenant_t covenant;
    bool   active;                /* tenure cell only */
    uint64_t ledger_seq;          /* monotonic accounting sequence */
} guardian_state_t;

/* Covenant validation (L1): returns true iff the named run_id matches and
 * the role is a recognized organ. Workers failing this are culled upstream. */
bool guardian_covenant_valid(const guardian_covenant_t *c);

/* Tenure check (L2). True iff this instance believes it holds the sole lease.
 * Backed by activation_lease primitives at wiring time. */
bool guardian_holds_tenure(const guardian_state_t *g);

/* Bump-and-check ledger sequence (L4 monotonicity); rejects regressions. */
bool guardian_ledger_advance(guardian_state_t *g, uint64_t next_seq);

#endif /* DSCO_GUARDIAN_H */
