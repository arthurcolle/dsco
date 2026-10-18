#include "context_mask.h"
#include "context_fabric.h"
#include "env_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_KEEP_RESULTS 8
#define DEFAULT_KEEP_IMAGES 3
#define DEFAULT_MIN_BYTES 1024
#define DEFAULT_MIN_BATCH_BYTES (80u * 1024u) /* ~20K tokens */
#define DEFAULT_IMAGE_BATCH 3
#define DEFAULT_MIN_INPUT_BYTES 4096
#define INPUT_STUB_KEY "_dsco_cleared_input"
#define TOOL_IMAGE_CAPTION "Tool observation."

context_mask_policy_t context_mask_default_policy(void) {
    context_mask_policy_t p = {
        .keep_results = (int)dsco_env_size("DSCO_MASK_KEEP_RESULTS", DEFAULT_KEEP_RESULTS, 0, 10000),
        .keep_images = (int)dsco_env_size("DSCO_MASK_KEEP_IMAGES", DEFAULT_KEEP_IMAGES, 0, 10000),
        .min_bytes = dsco_env_size("DSCO_MASK_MIN_BYTES", DEFAULT_MIN_BYTES, 0, 1u << 30),
        .min_batch_bytes = dsco_env_size("DSCO_MASK_MIN_BATCH_BYTES", DEFAULT_MIN_BATCH_BYTES, 0, 1u << 30),
        .image_batch = (int)dsco_env_size("DSCO_MASK_IMAGE_BATCH", DEFAULT_IMAGE_BATCH, 0, 10000),
        .min_input_bytes = dsco_env_size("DSCO_MASK_MIN_INPUT_BYTES", DEFAULT_MIN_INPUT_BYTES, 0, 1u << 30),
    };
    return p;
}

static bool is_type(const msg_content_t *c, const char *type) {
    return c && c->type && !strcmp(c->type, type);
}

bool context_mask_is_placeholder(const msg_content_t *c) {
    return c && c->text && !strncmp(c->text, CONTEXT_MASK_PREFIX, sizeof(CONTEXT_MASK_PREFIX) - 1);
}

/* Images the harness attached after a tool call, never user uploads. */
static bool tool_image_message(const message_t *m) {
    if (m->role != ROLE_USER) return false;
    bool image = false, caption = false;
    for (int j = 0; j < m->content_count; j++) {
        const msg_content_t *c = &m->content[j];
        if (is_type(c, "image") && c->image_data) image = true;
        else if (is_type(c, "text") && c->text && !strcmp(c->text, TOOL_IMAGE_CAPTION)) caption = true;
    }
    return image && caption;
}

static bool store(const char *bytes, size_t n, ctx_kind_t kind, const char *source, char *key, size_t cap) {
    ctx_broker_t *broker = ctx_broker_default();
    if (!broker) return false;
    ctx_put_opts_t opts = {.kind = kind, .source = source, .tags = "masked_observation", .embed = 0};
    ctxkey_t k;
    return ctx_put(broker, bytes, n, &opts, &k, NULL) && ctxkey_format(&k, key, cap) > 0;
}

/* Replace c->text with the placeholder; the original is freed only after the
 * replacement exists. */
static bool set_text(msg_content_t *c, const char *text) {
    char *copy = strdup(text);
    if (!copy) return false;
    free(c->text);
    c->text = copy;
    return true;
}

/* Which blocks a pass would mask, walking newest to oldest so "keep" means
 * the most recent N. apply=false only measures. */
static void mask_pass(conversation_t *conv, const context_mask_policy_t *p, bool apply,
                      context_mask_stats_t *stats) {
    int total_images = 0;
    for (int i = 0; i < conv->count; i++) total_images += tool_image_message(&conv->msgs[i]);
    bool images_due = total_images > p->keep_images + p->image_batch;
    int results_seen = 0, images_seen = 0, inputs_seen = 0;
    for (int i = conv->count - 1; i >= 0; i--) {
        message_t *m = &conv->msgs[i];
        bool tool_image = tool_image_message(m);
        if (tool_image) images_seen++;
        for (int j = m->content_count - 1; j >= 0; j--) {
            msg_content_t *c = &m->content[j];
            if (is_type(c, "tool_result")) {
                results_seen++;
                if (results_seen <= p->keep_results) continue;
                if (context_mask_is_placeholder(c)) { if (!apply) stats->already_masked++; continue; }
                /* A recall is already a bounded page the model asked for;
                 * masking it again sends the model straight back to recall. */
                if (c->tool_name && (!strcmp(c->tool_name, "context_recall") || !strcmp(c->tool_name, "ctx_get")))
                    continue;
                size_t n = c->text ? strlen(c->text) : 0;
                if (n < p->min_bytes) continue;
                if (!apply) { stats->candidate_bytes += n; continue; }
                char key[CTX_KEY_STR_MAX];
                if (!store(c->text, n, CTX_KIND_TOOL, c->tool_name ? c->tool_name : "tool_result", key, sizeof(key))) {
                    stats->store_failures++;
                    continue;
                }
                char placeholder[CTX_KEY_STR_MAX + 256];
                snprintf(placeholder, sizeof(placeholder),
                         CONTEXT_MASK_PREFIX " (%zu bytes%s%s). Reload with context_recall "
                         "{\"key\":\"%s\"}]",
                         n, c->tool_name ? " from " : "", c->tool_name ? c->tool_name : "", key);
                if (!set_text(c, placeholder)) { stats->store_failures++; continue; }
                stats->results_masked++;
                stats->bytes_before += n;
                stats->bytes_after += strlen(placeholder);
            } else if (is_type(c, "tool_use") && p->min_input_bytes) {
                inputs_seen++;
                if (inputs_seen <= p->keep_results || !c->tool_input) continue;
                if (strstr(c->tool_input, INPUT_STUB_KEY)) { if (!apply) stats->already_masked++; continue; }
                size_t n = strlen(c->tool_input);
                if (n < p->min_input_bytes) continue;
                if (!apply) { stats->candidate_bytes += n; continue; }
                char key[CTX_KEY_STR_MAX];
                if (!store(c->tool_input, n, CTX_KIND_TOOL, "tool_input", key, sizeof(key))) {
                    stats->store_failures++;
                    continue;
                }
                /* Must stay a JSON object: providers replay tool_use.input. */
                char stub[CTX_KEY_STR_MAX + 256];
                snprintf(stub, sizeof(stub),
                         "{\"" INPUT_STUB_KEY "\":\"" CONTEXT_MASK_PREFIX " (input, %zu bytes). "
                         "Reload with context_recall key %s]\"}", n, key);
                char *copy = strdup(stub);
                if (!copy) { stats->store_failures++; continue; }
                free(c->tool_input);
                c->tool_input = copy;
                stats->inputs_masked++;
                stats->bytes_before += n;
                stats->bytes_after += strlen(stub);
            } else if (tool_image && images_due && is_type(c, "image") && c->image_data) {
                if (images_seen <= p->keep_images) continue;
                size_t n = strlen(c->image_data);
                if (!apply) { stats->candidate_bytes += n; continue; }
                char key[CTX_KEY_STR_MAX];
                if (!store(c->image_data, n, CTX_KIND_BLOB, "tool_image_base64", key, sizeof(key))) {
                    stats->store_failures++;
                    continue;
                }
                char placeholder[CTX_KEY_STR_MAX + 256];
                snprintf(placeholder, sizeof(placeholder),
                         CONTEXT_MASK_PREFIX ": %s screenshot, %zu base64 bytes; take a new "
                         "screenshot to see the current screen. Stored as %s]",
                         c->image_media_type ? c->image_media_type : "image", n, key);
                char *type = strdup("text"), *text = strdup(placeholder);
                if (!type || !text) { free(type); free(text); stats->store_failures++; continue; }
                free(c->type); free(c->image_data); free(c->image_media_type); free(c->image_url);
                c->type = type; c->text = text;
                c->image_data = c->image_media_type = c->image_url = NULL;
                stats->images_masked++;
                stats->bytes_before += n;
                stats->bytes_after += strlen(placeholder);
            }
        }
    }
}

bool context_mask_observations(conversation_t *conv, const context_mask_policy_t *policy,
                               context_mask_stats_t *stats) {
    context_mask_stats_t local = {0};
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (!conv || conv->count < 2) return false;
    context_mask_policy_t p = policy ? *policy : context_mask_default_policy();
    mask_pass(conv, &p, false, stats);
    if (!stats->candidate_bytes) return false;
    if (stats->candidate_bytes < p.min_batch_bytes) { stats->deferred = true; return false; }
    mask_pass(conv, &p, true, stats);
    return stats->results_masked || stats->images_masked || stats->inputs_masked;
}
