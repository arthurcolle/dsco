#include "sequence_state.h"

#include <stdlib.h>
#include <string.h>

void dsco_sequence_state_init(dsco_sequence_state_t *seq,
                              const dsco_sampling_params_t *sampling,
                              dsco_kvblock_ref_t blocks) {
    if (!seq)
        return;
    memset(seq, 0, sizeof(*seq));
    if (sampling) {
        seq->sampling = *sampling;
    } else {
        seq->sampling.temperature = -1.0;
        seq->sampling.top_p = -1.0;
        seq->sampling.top_k = -1;
        seq->sampling.max_tokens = 0;
    }
    seq->blocks = blocks;
}

void dsco_sequence_state_free(dsco_sequence_state_t *seq) {
    if (!seq)
        return;
    free(seq->tokens);
    memset(seq, 0, sizeof(*seq));
}

bool dsco_sequence_state_append(dsco_sequence_state_t *seq, int token) {
    if (!seq)
        return false;
    if (seq->count == seq->capacity) {
        size_t next = seq->capacity ? seq->capacity * 2 : 32;
        int *tmp = realloc(seq->tokens, next * sizeof(*seq->tokens));
        if (!tmp)
            return false;
        seq->tokens = tmp;
        seq->capacity = next;
    }
    seq->tokens[seq->count++] = token;
    return true;
}

bool dsco_sequence_state_next(dsco_sequence_state_t *seq, int *out_token) {
    if (!seq || seq->position >= seq->count)
        return false;
    if (out_token)
        *out_token = seq->tokens[seq->position];
    seq->position++;
    return true;
}

void dsco_sequence_state_rewind(dsco_sequence_state_t *seq) {
    if (seq)
        seq->position = 0;
}
