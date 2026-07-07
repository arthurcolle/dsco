#ifndef DSCO_SEQUENCE_STATE_H
#define DSCO_SEQUENCE_STATE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct dsco_kvblock_ref {
    void *opaque;
} dsco_kvblock_ref_t;

typedef struct {
    double temperature;
    double top_p;
    int    top_k;
    int    max_tokens;
} dsco_sampling_params_t;

typedef struct {
    int                   *tokens;
    size_t                 count;
    size_t                 capacity;
    size_t                 position;
    dsco_sampling_params_t sampling;
    dsco_kvblock_ref_t     blocks;
} dsco_sequence_state_t;

void dsco_sequence_state_init(dsco_sequence_state_t *seq,
                              const dsco_sampling_params_t *sampling,
                              dsco_kvblock_ref_t blocks);
void dsco_sequence_state_free(dsco_sequence_state_t *seq);
bool dsco_sequence_state_append(dsco_sequence_state_t *seq, int token);
bool dsco_sequence_state_next(dsco_sequence_state_t *seq, int *out_token);
void dsco_sequence_state_rewind(dsco_sequence_state_t *seq);

#endif /* DSCO_SEQUENCE_STATE_H */
