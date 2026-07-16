#ifndef DSCO_OPENROUTER_LANES_H
#define DSCO_OPENROUTER_LANES_H

#include <stdbool.h>
#include <stddef.h>

#define OR_LANE_MODEL_MAX 128
#define OR_LANE_UPSTREAM_MAX 64
#define OR_LANE_TAG_MAX 96

typedef struct {
    char model[OR_LANE_MODEL_MAX];
    char upstream[OR_LANE_UPSTREAM_MAX]; /* OpenRouter provider slug; empty = router-selected */
    char quantization[16];               /* endpoint quantization, when pinned */
    char tag[OR_LANE_TAG_MAX];           /* unique model@upstream/quantization identity */
    double input_price_per_m;
    double output_price_per_m;
    double quality;
    int context_window;
    int max_output;
    bool free;
    bool tool_capable;
} openrouter_lane_t;

typedef struct {
    int min_context;
    int min_output;
    double min_quality;
    double max_input_price_per_m;  /* 0 = no ceiling */
    double max_output_price_per_m; /* 0 = no ceiling */
    int max_models;                /* 0 = implementation default */
    int endpoints_per_model;       /* 0 = model lanes only; >0 fetches endpoint providers */
    bool require_tools;
    bool free_only;
    bool diversify_org;
    const char *task;
} openrouter_lane_query_t;

/* Build fully materialized model lanes from the live OpenRouter catalog. When
 * endpoints_per_model > 0, each model is further unrolled into concrete
 * OpenRouter upstream-provider lanes using the public /endpoints metadata. */
int openrouter_lanes_build(const openrouter_lane_query_t *query, openrouter_lane_t *out, int max);

/* Native catalog/planning tool. It does not invoke any model. */
bool tool_openrouter_lanes(const char *input, char *result, size_t rlen);

#endif /* DSCO_OPENROUTER_LANES_H */
