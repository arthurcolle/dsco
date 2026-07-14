#ifndef DSCO_CODEX_USAGE_H
#define DSCO_CODEX_USAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Authoritative ChatGPT/Codex account usage reported by the installed Codex
 * app-server. This is intentionally separate from DSCO's billed-cost ledger. */
typedef struct {
    bool have_rate_limits;
    bool have_account_usage;

    char limit_id[64];
    char plan_type[32];
    int primary_used_percent;
    long long primary_window_minutes;
    time_t primary_resets_at;
    bool have_secondary;
    int secondary_used_percent;
    long long secondary_window_minutes;
    time_t secondary_resets_at;
    int reset_credits_available;

    char latest_usage_date[16];
    long long latest_daily_tokens;
    long long lifetime_tokens;
    long long peak_daily_tokens;
    long long longest_running_turn_seconds;
    long long current_streak_days;
    long long longest_streak_days;
} codex_usage_snapshot_t;

/* Parse JSONL emitted by `codex app-server --stdio`. Exported for deterministic
 * regression tests and for callers that already own an app-server connection. */
bool codex_usage_parse_jsonl(const char *jsonl, codex_usage_snapshot_t *out,
                             char *error, size_t error_len);

/* Query account/rateLimits/read and account/usage/read through the user's
 * installed, authenticated Codex app-server. DSCO_CODEX_BIN may override the
 * executable name/path. */
bool codex_usage_fetch(codex_usage_snapshot_t *out, char *error, size_t error_len);

#endif /* DSCO_CODEX_USAGE_H */
