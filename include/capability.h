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
 * The gate calls dsco_flow_note() after each tool runs to accumulate taint,
 * and dsco_flow_would_exfiltrate() before a call to check whether this call
 * closes the lethal triad. State is process-global and thread-safe. */
void dsco_flow_reset(void);
void dsco_flow_note(unsigned caps);
bool dsco_flow_tainted_untrusted(void);
bool dsco_flow_accessed_private(void);

/* True iff running a tool with `caps` would complete the lethal trifecta:
 * this call egresses AND the session has already ingested untrusted content
 * AND accessed private data. */
bool dsco_flow_would_exfiltrate(unsigned caps);

/* ── Top-level gate ─────────────────────────────────────────────────────────
 * The single hook tools_execute_for_tier() calls before running a tool.
 * Fills `reason` (may be NULL) on non-ALLOW. Does NOT mutate flow state;
 * the caller calls dsco_flow_note() after a successful run. */
dsco_cap_decision_t dsco_capability_gate(const char *name, const char *input_json,
                                         const char *tier, char *reason, size_t reason_len);

/* Human-readable capability list for diagnostics / gate_status, e.g.
 * "fs_read|net". Writes into `out`. */
void dsco_capability_to_string(unsigned caps, char *out, size_t out_len);

/* ── Granular decision features + hooks (classifier training) ────────────────
 * The rule gate above is a hand-tuned policy. To improve it, every decision is
 * described by a feature vector and delivered to an observation hook — a labeled
 * corpus you can train a classifier on. An advisory hook can then shadow the
 * rules (compare) or, in enforce mode, tighten/loosen them within safety bounds. */

/* Egress destination granularity — ordered by risk (LOCAL lowest). */
typedef enum {
    DSCO_EGRESS_NONE = 0,   /* no network at all                          */
    DSCO_EGRESS_LOCAL,      /* loopback (127/8, ::1, localhost)           */
    DSCO_EGRESS_TRUSTED,    /* operator-configured trusted host           */
    DSCO_EGRESS_LAN,        /* RFC1918 / CGNAT / link-local / .local      */
    DSCO_EGRESS_EXTERNAL,   /* a genuine external destination             */
    DSCO_EGRESS_OPAQUE,     /* network verb present, destination unparsed */
} dsco_egress_kind_t;

/* Classifier-ready feature vector for one gate decision. */
typedef struct {
    const char *tool;
    const char *tier;
    unsigned    caps;               /* CAP_* bitmask                        */
    bool        is_read, is_write, is_exec, is_net, is_spawn, is_control, is_secret_tool;
    bool        tainted_untrusted;  /* session ingested untrusted content   */
    bool        accessed_private;   /* session accessed secrets             */
    bool        input_has_secrets;  /* this call's input names secrets       */
    bool        shell_writes;       /* exec command mutates the filesystem   */
    dsco_egress_kind_t egress;
    char        dest_host[128];     /* best-effort parsed destination        */
    int         risk;               /* 0..100 heuristic risk score           */
} dsco_cap_features_t;

/* A completed gate decision, delivered to the observation hook. */
typedef struct {
    dsco_cap_features_t features;
    dsco_cap_decision_t decision;
    const char *category;    /* stable slug: "exfil-external","clean-spawn",… */
    const char *reason;      /* human-readable                               */
    int  advisor_vote;       /* -1 abstain/none, 0 deny, 1 allow             */
    int  advisor_conf;       /* 0..100                                       */
    bool advisor_overrode;   /* the advisor changed the rule decision         */
    double ts;               /* unix seconds                                 */
} dsco_cap_event_t;

/* Extract the feature vector without deciding (diagnostics / offline labeling). */
dsco_cap_features_t dsco_cap_extract_features(const char *name, const char *input_json,
                                              const char *tier);

/* Observation hook: fires on EVERY gate decision. If unset, a built-in NDJSON
 * logger appends to $DSCO_CAP_LOG when that env var is set. */
typedef void (*dsco_cap_hook_fn)(const dsco_cap_event_t *ev, void *user_data);
void dsco_cap_set_hook(dsco_cap_hook_fn fn, void *user_data);

/* Advisory classifier: returns 1 (allow), 0 (deny), or -1 (abstain), and may set
 * *confidence (0..100). Consulted per $DSCO_CAP_CLASSIFIER:
 *   unset/"off" -> ignored;  "shadow" -> logged only, never changes the outcome;
 *   "enforce"   -> may TIGHTEN (deny what the rule allowed) at conf>=70; may LOOSEN
 *                  (allow what the rule denied) only for non-security denials and
 *                  only when DSCO_CAP_CLASSIFIER_CAN_ALLOW=1. */
typedef int (*dsco_cap_advisor_fn)(const dsco_cap_features_t *f, int *confidence, void *user_data);
void dsco_cap_set_advisor(dsco_cap_advisor_fn fn, void *user_data);

#endif /* DSCO_CAPABILITY_H */
