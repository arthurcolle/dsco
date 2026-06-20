#ifndef DSCO_OPENROUTER_CACHE_H
#define DSCO_OPENROUTER_CACHE_H

/* Background OpenRouter model catalog.
 *
 * On startup dsco loads the on-disk catalog (~/.dsco/openrouter_models.json)
 * into memory immediately, then — on a detached background thread — refreshes
 * it from https://openrouter.ai/api/v1/models when the cache is stale. This
 * keeps model metadata (context window, pricing, reasoning support) current
 * for *every* real slug without anything being hardcoded, so `-m <slug>` and
 * cost tracking work for new models the moment OpenRouter ships them.
 *
 * The lookup entry point lives in config.h (openrouter_cache_lookup) so the
 * inline model_lookup() can fall through to it; this header only exposes the
 * lifecycle hook.
 */

/* Spawn the background loader/refresher. Idempotent and non-blocking: returns
 * immediately after kicking off the thread. Safe to call once at startup. */
void openrouter_cache_init(void);

/* Number of models currently in the in-memory catalog (0 if not yet loaded). */
int openrouter_cache_count(void);

#endif /* DSCO_OPENROUTER_CACHE_H */
