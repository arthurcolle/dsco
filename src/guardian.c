#include "guardian.h"
#include <string.h>

static const char *g_roles[] = {
    [GUARDIAN_ROLE_NONE] = "none",
    [GUARDIAN_ROLE_TENURE] = "tenure",
    [GUARDIAN_ROLE_PERCEPTION] = "perception",
    [GUARDIAN_ROLE_VETO] = "veto",
    [GUARDIAN_ROLE_SUCCESSION] = "succession",
};

bool guardian_covenant_valid(const guardian_covenant_t *c) {
    if (!c) return false;
    size_t rl = strnlen(c->run_id, sizeof(c->run_id));
    if (rl == 0 || rl >= sizeof(c->run_id)) return false;
    if (c->role <= GUARDIAN_ROLE_NONE || c->role > GUARDIAN_ROLE_SUCCESSION) return false;
    if (c->revocation_handle == 0) return false;
    return true;
}

bool guardian_holds_tenure(const guardian_state_t *g) {
    return g && g->active && g->covenant.role == GUARDIAN_ROLE_TENURE &&
           guardian_covenant_valid(&g->covenant);
}

bool guardian_ledger_advance(guardian_state_t *g, uint64_t next_seq) {
    if (!guardian_holds_tenure(g)) return false;
    if (next_seq <= g->ledger_seq) return false; /* split-brain fence */
    g->ledger_seq = next_seq;
    return true;
}

const char *guardian_role_name(int role); /* symbol for telemetry */
const char *guardian_role_name(int role) {
    if (role <= GUARDIAN_ROLE_NONE || role > GUARDIAN_ROLE_SUCCESSION) return "?";
    return g_roles[role];
}
