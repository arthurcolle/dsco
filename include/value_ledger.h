#ifndef DSCO_VALUE_LEDGER_H
#define DSCO_VALUE_LEDGER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Value ledger — the receipt spine of the DSI Value Creation Engine.
 *
 * A receipt is the atomic record of a governed unit of work: what outcome was
 * produced, under what authority, at what cost, verified how, reusing what, and
 * what to do next. Receipts are append-only, content-addressed by their own
 * hash, and written to the durable Chronicle journal (record type
 * "value.receipt.v1") so they survive restarts and feed board metrics.
 *
 * This module never moves money, never grants authority, and never self-issues
 * a verification verdict — it records claims the caller supplies plus a
 * deterministic content hash. Verification truth remains the caller's evidence.
 */

typedef struct {
    const char *workload_id;      /* stable id for the workload class            */
    const char *principal;        /* "DSI" or a customer id                      */
    const char *outcome;          /* falsifiable terminal-state description      */
    const char *authority_json;   /* JSON array of granted capabilities          */
    const char *inputs_hash;      /* sha256 of inputs (optional)                 */
    const char *result_hash;      /* sha256 of result (optional)                 */
    const char *verify_method;    /* deterministic|held-out|human|none           */
    bool        verified;         /* caller-attested pass/fail                    */
    bool        autonomous;       /* completed without unplanned human rescue    */
    double      price_usd;        /* value billed/attributed                     */
    double      compute_cost_usd; /* variable inference/tool/infra cost          */
    double      human_minutes;    /* principal/human minutes consumed            */
    double      latency_ms;
    int         retries;
    double      recovery_ms;      /* time to restore a failed run (0 if none)    */
    const char *reuse_json;       /* JSON: {"tools":[],"policies":[],...}        */
    bool        incident;         /* an incident occurred                        */
    bool        rollback_verified;
    const char *next;             /* expand|distill|repair|retire                */
} value_receipt_t;

/* Emit a receipt to the durable journal. Fills version_out (65 bytes) with the
 * receipt's content hash if append succeeded. Returns false on argument error,
 * missing Chronicle run, or journal failure (no-op success is removed). */
bool value_ledger_emit(const value_receipt_t *r, char *version_out, size_t version_len);

/* Roll the current (or given) run's receipts into the board metrics JSON:
 * verified_work_value, gross_compute_margin, autonomous_completion_rate,
 * recovery_rate, capability_reuse, human_leverage, receipt counts. */
bool value_ledger_summary(const char *run_id, char *out, size_t out_len);

#endif
