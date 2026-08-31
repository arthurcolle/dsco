#ifndef DSCO_USAGE_H
#define DSCO_USAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Per-session cost receipts (Wave B Plan #12 observability).
 *
 * A usage receipt is one append-only JSON line in
 * ~/.dsco/usage/usage.jsonl (override with DSCO_USAGE_DIR). Each line is
 * schema "dsco.usage_receipt.v1" and records the durable accounting facts
 * for one session: token buckets, the budget-accounted cost estimate, and
 * the model that served the run. The ledger is local-first evidence:
 * every run traces to a dollar. No prompts, outputs, or credentials are
 * ever written here.
 */

#define USAGE_LEDGER_FILENAME "usage.jsonl"
#define USAGE_RECEIPT_SCHEMA "dsco.usage_receipt.v1"

/* Fixed-capacity receipt record. Long enough for any UUID-like id
 * (36 + NUL) and any provider/model key; JSON escaping is applied at
 * serialization time so arbitrary content cannot break the line format. */
typedef struct {
    char session_id[128];
    long long timestamp_ms;      /* epoch milliseconds when recorded */
    long long tokens_in;
    long long tokens_out;
    long long cache_read_tokens; /* prompt-cache reads (may be 0) */
    long long cache_write_tokens;
    double cost_usd;             /* budget-accounted estimate (authoritative) */
    double provider_cost_usd;    /* raw provider-reported amount (may be 0) */
    double locally_derived_usd;  /* token-derived fallback amount (may be 0) */
    long long turns;             /* LLM responses included in this receipt */
    char model[128];             /* effective/requested model key */
    char provider[64];
} usage_receipt_t;

/* Strict id validation for any session-id that reaches a filename or is
 * used as a CLI filter: 1..127 chars of [A-Za-z0-9_-] only. Rejects '/',
 * '\\', "..", and whitespace so a CLI argument cannot escape the usage
 * directory. */
bool usage_session_id_valid(const char *session_id);

/* Absolute path of the usage ledger directory (DSCO_USAGE_DIR override)
 * and the ledger file itself. Returns false if neither can be resolved. */
bool usage_ledger_dir(char *out, size_t cap);
bool usage_ledger_path(char *out, size_t cap);

/* Serialize one receipt as a single JSONL line and append it to the
 * ledger, creating ~/.dsco/usage/ if needed. The record is fsync'd before
 * returning. Append-only: never rewrites or truncates existing lines.
 * Returns false on any filesystem failure. */
bool usage_receipt_record(const usage_receipt_t *receipt);

/* Read-only CLI: `dsco usage [session-id]`.
 * - no args: list recent receipts newest-first plus grand totals
 * - <id>:    receipts filtered to that session id plus its total
 * Exit codes: 0 found, 1 I/O or parse failure, 2 invalid id/syntax. */
int usage_cli(int argc, char **argv);

#endif
