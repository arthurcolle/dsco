#include "guardian.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

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

/* ── T13: tenure election — atomic O_EXCL lease file ──────────────────────
 * Exactly one holder per guardian_run_id, guaranteed by the filesystem:
 * the second acquirer's O_EXCL create fails and it may not act. Succession
 * = release + re-acquire; stale ownership after crash requires an operator-
 * visible override (DSCO_GUARDIAN_FORCE_TENURE=1), never silent takeover. */
const char *guardian_role_name(int role);
bool guardian_tenure_acquire(guardian_state_t *g, const char *run_id) {
    if (!g || !run_id || !run_id[0]) return false;
    memset(g, 0, sizeof(*g));
    snprintf(g->covenant.run_id, sizeof(g->covenant.run_id), "%s", run_id);
    g->covenant.role = GUARDIAN_ROLE_TENURE;
    g->covenant.revocation_handle = (uint64_t)getpid() << 16 | (uint64_t)(time(NULL) & 0xFFFF);
    g->active = false;

    char path[512];
    const char *ov = getenv("DSCO_GUARDIAN_LEASE_PATH");
    if (ov && ov[0])
        snprintf(path, sizeof(path), "%s", ov);
    else
        snprintf(path, sizeof(path), "/tmp/dsco_guardian_%s.lease", run_id);

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); /* L2: atomic */
    if (fd < 0) {
        if (getenv("DSCO_GUARDIAN_FORCE_TENURE") && getenv("DSCO_GUARDIAN_FORCE_TENURE")[0] == '1') {
            unlink(path);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0) return false;
            fprintf(stderr, "[guardian] WARNING: forced tenure takeover of %s\n", run_id);
        } else {
            fprintf(stderr, "[guardian] split-brain fence tripped: %s already tenured\n", run_id);
            return false;
        }
    }
    dprintf(fd, "{\"run_id\":\"%s\",\"pid\":%d,\"issued\":%ld}\n", run_id, (int)getpid(), time(NULL));
    close(fd);
    g->active = true;
    return true;
}

void guardian_tenure_release(guardian_state_t *g) {
    if (!g || !guardian_holds_tenure(g)) return;
    char path[512];
    const char *ov = getenv("DSCO_GUARDIAN_LEASE_PATH");
    if (ov && ov[0]) snprintf(path, sizeof(path), "%s", ov);
    else snprintf(path, sizeof(path), "/tmp/dsco_guardian_%s.lease", g->covenant.run_id);
    unlink(path);
    g->active = false;
}

