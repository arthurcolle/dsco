#ifndef DSCO_STRUCTURED_PROCESS_H
#define DSCO_STRUCTURED_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SP_INTENT_CHAT = 0,
    SP_INTENT_CODE,
    SP_INTENT_REVIEW,
    SP_INTENT_RESEARCH,
    SP_INTENT_OPERATE,
    SP_INTENT_PROFILE,
    SP_INTENT_PLAN,
} sp_intent_t;

typedef struct {
    sp_intent_t intent;
    char intent_name[24];
    char risk[16];
    int confidence;
    int model_budget_pct;
    int background_budget_pct;
    int max_concurrency;
    int max_iterations;
    bool needs_model_gate;
    bool can_background;
} sp_classification_t;

const char *structured_process_schema_json(void);
int structured_process_schema_response_format_json(char *buf, size_t len);

bool structured_process_classify(const char *input, sp_classification_t *out);
int structured_process_synthesize_json(const char *input, char *buf, size_t len);
int structured_process_create_plan_from_json(const char *process_json);

#endif /* DSCO_STRUCTURED_PROCESS_H */
