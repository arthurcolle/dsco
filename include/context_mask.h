#ifndef DSCO_CONTEXT_MASK_H
#define DSCO_CONTEXT_MASK_H

#include "llm.h"
#include <stdbool.h>
#include <stddef.h>

/* Observation masking: shrink old tool observations in place to a stable,
 * keyed placeholder. Messages are never removed or reordered, so tool_use /
 * tool_result pairing is untouched and no integrity scan is needed. Agent
 * actions, reasoning and user text are never masked.
 *
 * Each masked payload is stored once in the content-addressed context store;
 * the placeholder carries its key so context_recall can page it back in. A
 * store failure leaves that one block unmasked and the pass continues. */

#define CONTEXT_MASK_PREFIX "[Earlier tool output cleared"

typedef struct {
    int results_masked, images_masked, inputs_masked, already_masked, store_failures;
    size_t bytes_before, bytes_after;
    size_t candidate_bytes; /* what a pass would free; reported even when deferred */
    bool deferred;          /* below min_batch_bytes: nothing changed */
} context_mask_stats_t;

typedef struct {
    int keep_results;   /* newest tool results left verbatim */
    int keep_images;    /* newest image observations left verbatim */
    size_t min_bytes;   /* smaller results are not worth a placeholder */
    /* Old tool-call inputs at least this large (e.g. write_file payloads that
     * already live on disk) are replaced by a keyed JSON stub; 0 disables.
     * Smaller actions always stay verbatim. */
    size_t min_input_bytes;
    /* Every edit invalidates the provider's cached prefix after it, so a pass
     * only runs when it frees at least this much (cf. Anthropic clear_at_least). */
    size_t min_batch_bytes;
    /* Screenshots are masked only once this many extra have accumulated
     * beyond keep_images, so masking happens in batches, not every turn. */
    int image_batch;
} context_mask_policy_t;

/* Defaults, overridable with DSCO_MASK_KEEP_RESULTS / DSCO_MASK_KEEP_IMAGES /
 * DSCO_MASK_MIN_BYTES / DSCO_MASK_MIN_BATCH_BYTES / DSCO_MASK_IMAGE_BATCH / DSCO_MASK_MIN_INPUT_BYTES. */
context_mask_policy_t context_mask_default_policy(void);

/* Returns true when anything was masked. */
bool context_mask_observations(conversation_t *conv, const context_mask_policy_t *policy,
                               context_mask_stats_t *stats);

/* True for a block produced by this module. */
bool context_mask_is_placeholder(const msg_content_t *c);

#endif
