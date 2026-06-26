#ifndef DSCO_CODEX_CACHE_H
#define DSCO_CODEX_CACHE_H

#include <stdbool.h>

#include "config.h"

/* Background Codex model catalog.
 *
 * Codex CLI already carries the ChatGPT/Codex model catalog in
 * `codex debug models`. DSCO indexes that output into memory and caches the raw
 * JSON at ~/.dsco/codex_models.json so ChatGPT-backed routing can follow the
 * same current default/reasoning metadata instead of stale hardcoded OpenAI API
 * defaults.
 */

typedef struct {
    const char *slug;
    const char *display_name;
    const char *default_reasoning_level;
    const char *visibility;
    int context_window;
    int max_context_window;
    int supported_in_api;
    int supports_thinking;
    int priority;
} codex_model_view_t;

typedef void (*codex_model_cb)(const codex_model_view_t *m, void *ud);

void codex_cache_init(void);
int codex_cache_count(void);
int codex_cache_load_sync(void);
int codex_cache_wait_ready(int timeout_ms);
int codex_cache_foreach(codex_model_cb cb, void *ud);

const model_info_t *codex_cache_lookup(const char *name);
const char *codex_cache_default_model(void);
const char *codex_cache_default_effort(const char *model);
bool codex_cache_model_supported(const char *model);

#endif /* DSCO_CODEX_CACHE_H */
