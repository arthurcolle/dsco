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

/* Block until a catalog is published or `timeout_ms` elapses. Returns the model
 * count (>0) once ready, or 0 on timeout. Use before enumerating from a
 * short-lived command so the background load has a chance to land. */
int openrouter_cache_wait_ready(int timeout_ms);

/* Synchronously load the catalog on the calling thread: publishes the on-disk
 * copy, then refreshes over the network when stale. Returns the resulting model
 * count. For CLI listing where waiting on the background thread is undesirable. */
int openrouter_cache_load_sync(void);

/* Read-only view of one indexed model, materialised for enumeration. */
typedef struct {
    const char *id;                 /* full slug, e.g. "openai/gpt-4o" */
    const char *name;               /* human display name (may be NULL) */
    const char *org;                /* provider prefix, e.g. "openai" */
    int    context_window;
    int    max_output;
    double input_price;             /* $ per 1M tokens */
    double output_price;
    double cache_read_price;
    double cache_write_price;
    int    supports_thinking;       /* 1 = exposes a reasoning parameter */
    int    multimodal;              /* 1 = accepts/produces non-text modalities */
    long   created;                 /* unix timestamp, 0 if unknown */
} or_model_view_t;

typedef void (*or_model_cb)(const or_model_view_t *m, void *ud);

/* Invoke `cb` once per indexed model (catalog order). Returns the number
 * visited (0 if the catalog is not yet loaded). */
int openrouter_cache_foreach(or_model_cb cb, void *ud);

#endif /* DSCO_OPENROUTER_CACHE_H */
