#ifndef DSCO_PROMPT_POOL_H
#define DSCO_PROMPT_POOL_H

#include <stdbool.h>
#include <stddef.h>

/* ── Prompt Pool — autonomous, ever-growing prompt suggestion cache ────────
 *
 * A persistent, deduplicated corpus of test prompts (target: 1000+ on first
 * run, growing daily) surfaced as rotating ghost text in the empty composer
 * box. Two ingestion paths:
 *
 *   1. Seed corpus — generated combinatorially on first init (subject ×
 *      template) so the pool crosses 1000 prompts immediately.
 *   2. Current events — a background refresher thread periodically fetches
 *      trending headlines (Hacker News Algolia API, keyless) and synthesizes
 *      prompts from them via rotating templates, appending novel ones.
 *
 * Storage: append-only JSONL at ~/.dsco/prompt_pool.jsonl
 *   {"t":<epoch>,"src":"seed|news|user","q":"<prompt>"}
 * Dedup: FNV-1a hash set over prompt text, loaded at init.
 *
 * Suggestion rotation is time-bucketed: the same prompt is returned for the
 * whole bucket (default 8s), so the ghost text a user sees is exactly what
 * Tab accepts — no read/accept race.
 *
 * Env gates:
 *   DSCO_PROMPT_POOL=0            disable entirely (no thread, no file)
 *   DSCO_PROMPT_POOL_REFRESH_S    news refresh cadence (default 3600, min 300)
 *   DSCO_PROMPT_POOL_ROTATE_S     ghost rotation bucket (default 8, min 2)
 */

/* Lifecycle. init loads the JSONL cache (seeding it if below the floor) and
 * starts the news refresher thread. Both idempotent. */
void prompt_pool_init(void);
void prompt_pool_shutdown(void);

/* Number of prompts currently in memory (0 if disabled/uninitialized). */
int prompt_pool_count(void);

/* Copy the current time-bucketed suggestion into out. Returns true when a
 * suggestion was written. Stable within a rotation bucket. */
bool prompt_pool_suggestion(char *out, size_t out_sz);

/* Rotation bucket id — changes when the ghost suggestion rotates. The
 * composer polls this on its idle tick to know when to repaint. */
unsigned prompt_pool_bucket(void);

/* Add one prompt (src: "seed"|"news"|"user"|...). Dedups; persists.
 * Returns true if the prompt was novel and added. */
bool prompt_pool_add(const char *prompt, const char *src);

/* Fetch headlines and synthesize prompts right now (blocking; also runs on
 * the background thread). Returns number of novel prompts added, -1 on
 * fetch failure. */
int prompt_pool_refresh_now(void);

/* Register the agent-facing `prompt_pool` tool (suggest/sample/add/stats/
 * refresh). Call before tools_init so the tool map includes it. */
void prompt_pool_register_tool(void);

#endif /* DSCO_PROMPT_POOL_H */
