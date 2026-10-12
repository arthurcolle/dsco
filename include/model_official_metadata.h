#ifndef DSCO_MODEL_OFFICIAL_METADATA_H
#define DSCO_MODEL_OFFICIAL_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Published limits are informational. They must not silently replace the
 * active provider/transport context cap or authorize model access. */
typedef struct {
    int context_tokens;
    int max_input_tokens;
    int max_output_tokens;
    time_t observed_at;
    char source[192];
} model_official_metadata_t;

/* Parse a bounded official Markdown response. Rejects a mismatched model ID.
 * Public for deterministic fixture tests; never parse third-party pages. */
bool model_official_metadata_parse(const char *model, const char *body,
                                   bool anthropic, model_official_metadata_t *out);

/* GET-only official vendor documentation; bounded TLS request, no auth or
 * inference. Returns false on unsupported model, missing data, or network
 * failure; leaves out zeroed. Call on explicit /model selection, not per token. */
bool model_official_metadata_refresh(const char *model,
                                     model_official_metadata_t *out);

#endif
