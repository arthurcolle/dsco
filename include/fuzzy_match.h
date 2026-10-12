#ifndef DSCO_FUZZY_MATCH_H
#define DSCO_FUZZY_MATCH_H

#include <stdbool.h>
#include <stddef.h>

/* Multi-strategy text matching for the edit write-path.
 *
 * dsco historically located `old_string` with a bare strstr(). Models — and
 * especially the smaller/cheaper models that make large cross-provider swarms
 * economical — routinely emit a near-miss: re-indented, whitespace-collapsed,
 * or carrying literal "\n" instead of a newline. Every near-miss was a hard
 * failure, so edit reliability (and therefore how far down the cost curve a
 * swarm lane stays usable) was gated on exact-match luck.
 *
 * This runs an ordered chain of increasingly permissive strategies, stopping
 * at the FIRST strategy that yields exactly one match. Ambiguity is never
 * silently resolved: if a strategy finds several candidates and the caller did
 * not ask for replace-all, the match fails and names the strategy, so the
 * model can disambiguate with more context.
 *
 * Chain order (strictest first) mirrors the OpenCode/Hermes design:
 *   1 exact              — byte-for-byte
 *   2 line-trimmed       — ignore per-line leading/trailing whitespace
 *   3 whitespace-normal  — collapse runs of spaces/tabs
 *   4 indent-flexible    — ignore leading indentation entirely
 *   5 escape-normalized  — literal \n, \t, \r become real control characters
 *   6 trimmed-boundary   — trim only the first and last line
 *   7 block-anchor       — first+last line match, middle >= 70% similar
 *   8 context-aware      — >= 50% of lines >= 80% similar
 */

typedef enum {
    FUZZY_STRATEGY_NONE = 0,
    FUZZY_STRATEGY_EXACT,
    FUZZY_STRATEGY_LINE_TRIMMED,
    FUZZY_STRATEGY_WHITESPACE_NORMALIZED,
    FUZZY_STRATEGY_INDENT_FLEXIBLE,
    FUZZY_STRATEGY_ESCAPE_NORMALIZED,
    FUZZY_STRATEGY_TRIMMED_BOUNDARY,
    FUZZY_STRATEGY_BLOCK_ANCHOR,
    FUZZY_STRATEGY_CONTEXT_AWARE,
    FUZZY_STRATEGY__COUNT
} fuzzy_strategy_t;

/* Human-readable strategy name, for tool results and telemetry. */
const char *fuzzy_strategy_name(fuzzy_strategy_t s);

typedef struct {
    bool found;
    size_t start;              /* byte offset of match start in haystack */
    size_t end;                /* byte offset one past match end */
    int occurrences;           /* how many candidates the winning strategy saw */
    fuzzy_strategy_t strategy; /* which strategy matched */
    double confidence;         /* 1.0 exact, decreasing with permissiveness */
} fuzzy_match_t;

/* Locate `needle` inside `haystack`.
 *
 * Returns the first strategy that produces a usable match. When
 * `allow_multiple` is false, a strategy producing more than one candidate is
 * rejected (reported via occurrences) rather than guessing. When true, the
 * first candidate is returned and `occurrences` reports the total.
 *
 * `max_strategy` caps how permissive the chain may become; pass
 * FUZZY_STRATEGY_CONTEXT_AWARE for the full chain or FUZZY_STRATEGY_EXACT to
 * force exact-only (the DSCO_EDIT_FUZZY=0 behavior). */
fuzzy_match_t fuzzy_find(const char *haystack, const char *needle,
                         bool allow_multiple, fuzzy_strategy_t max_strategy);

/* Whether the fuzzy chain is enabled for this process. Honors DSCO_EDIT_FUZZY:
 * "0"/"false"/"off" disables everything past exact matching. Cached. */
bool fuzzy_enabled(void);

/* Ratcliff/Obershelp similarity in [0,1]. Exposed for tests and for callers
 * that want to score their own candidates. */
double fuzzy_similarity(const char *a, size_t alen, const char *b, size_t blen);

#endif /* DSCO_FUZZY_MATCH_H */
