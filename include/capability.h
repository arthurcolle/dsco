#ifndef DSCO_CAPABILITY_H
#define DSCO_CAPABILITY_H

#include <stdbool.h>
#include <stddef.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Capability-based tool gating (Deno-style permissions + lethal-trifecta flow
 * guard).
 *
 * Rationale: the legacy gate protected by *filename* (an immune "surface" list)
 * and by hardcoded per-tier tool blocklists. Both are the wrong axis. This
 * module gates by *capability* — what a tool can actually do — and adds the
 * one guard Deno lacks: it tracks taint flow so it can deny the lethal
 * conjunction (untrusted content in + secrets access + external egress),
 * which is the actual exfiltration event. Ordinary filesystem reads do not
 * taint the session as private-data access; otherwise routine source inspection
 * would immediately complete that leg of the guard.
 *
 * Lethal trifecta (Willison), as enforced here:
 *   1. access to private data      -> CAP_SECRETS
 *   2. exposure to untrusted input -> CAP_UNTRUSTED_IN
 *   3. ability to exfiltrate       -> CAP_NET | CAP_EXEC
 * All three reachable in one session == exfiltration risk.
 * ───────────────────────────────────────────────────────────────────────── */

typedef enum {
    CAP_NONE         = 0,
    CAP_FS_READ      = 1u << 0, /* reads ordinary local files/data               */
    CAP_FS_WRITE     = 1u << 1, /* creates/modifies/deletes local files          */
    CAP_NET          = 1u << 2, /* external network egress (exfil leg)           */
    CAP_EXEC         = 1u << 3, /* spawns a subprocess / shell (exfil leg)       */
    CAP_SECRETS      = 1u << 4, /* touches credentials/keys/env secrets          */
    CAP_UNTRUSTED_IN = 1u << 5, /* ingests untrusted content (web/MCP/ext tools) */
    CAP_CONTROL      = 1u << 6, /* mutates the gate / governance / self          */
} dsco_cap_t;

/* Union of the two "private data in" legs. */
#define CAP_PRIVATE_IN CAP_SECRETS
/* Union of the two "exfil out" legs. */
#define CAP_EGRESS (CAP_NET | CAP_EXEC)

/* Gate decision for a single tool call. */
typedef enum {
    CAP_DECISION_ALLOW = 0,  /* capabilities granted; run it                    */
    CAP_DECISION_APPROVE,    /* allowed but requires explicit user approval     */
    CAP_DECISION_DENY,       /* a required capability is not granted            */
} dsco_cap_decision_t;

/* Classify a tool call into its capability set. Input-aware: e.g. `bash`
 * inspects the command string to distinguish a read-only `grep` from a
 * network `curl`. `input_json` may be NULL for a conservative worst-case. */
unsigned dsco_caps_for_tool(const char *name, const char *input_json);

/* Deno-style grant check: is capability `cap` granted under trust `tier`?
 * Reads DSCO_ALLOW_{READ,WRITE,NET,RUN,SECRETS,CONTROL} overrides on top of
 * per-tier defaults. `tier` NULL/"" defaults to "standard". */
bool dsco_cap_granted(dsco_cap_t cap, const char *tier);

/* ── Per-session taint flow (the trifecta guard) ────────────────────────────
 * Production dispatch reserves classified in-flight effects at admission and
 * commits taint on successful completion. Direct notes support internal
 * secret consumers. State is process-global and thread-safe. */
void dsco_flow_reset(void);
void dsco_flow_note(unsigned caps);
bool dsco_flow_tainted_untrusted(void);
bool dsco_flow_accessed_private(void);

typedef struct {
    unsigned caps;
    unsigned long long epoch;
    bool active;
} dsco_flow_reservation_t;

/* True iff running a tool with `caps` would complete the lethal trifecta:
 * this call egresses AND the union of prior taint, in-flight reservations,
 * and this call's capabilities includes untrusted input and secret access. */
bool dsco_flow_would_exfiltrate(unsigned caps);

/* ── Top-level gate ─────────────────────────────────────────────────────────
 * The single hook tools_execute_for_tier() calls before running a tool.
 * Fills `reason` (may be NULL) on non-ALLOW. Does NOT mutate flow state;
 * use gate_reserve/flow_finish around production dispatch. */
dsco_cap_decision_t dsco_capability_gate(const char *name, const char *input_json,
                                         const char *tier, char *reason, size_t reason_len);
/* Atomically recheck flow and reserve effects before dispatch. Finish on every
 * return; failed calls release without latching their capabilities. */
dsco_cap_decision_t dsco_capability_gate_reserve(const char *name, const char *input_json,
                                                 const char *tier, char *reason, size_t reason_len,
                                                 dsco_flow_reservation_t *reservation);
void dsco_flow_finish(dsco_flow_reservation_t *reservation, bool succeeded);

/* Human-readable capability list for diagnostics / gate_status, e.g.
 * "fs_read|net". Writes into `out`. */
void dsco_capability_to_string(unsigned caps, char *out, size_t out_len);

#endif /* DSCO_CAPABILITY_H */
