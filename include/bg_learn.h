#ifndef DSCO_BG_LEARN_H
#define DSCO_BG_LEARN_H

#include <stdbool.h>

typedef struct {
    bool enabled;
    bool running;
    unsigned interval_sec;
    unsigned cycles;
    unsigned skills_created;
    unsigned skills_pruned;
    int auto_skill_count;
    double last_run_epoch;
    char last_skill_write[128];
} bg_learn_stats_t;

void bg_learn_set_enabled(bool enabled);
bool bg_learn_is_enabled(void);
bool bg_learn_start(void);
void bg_learn_stop(void);
int bg_learn_run_once(void);
void bg_learn_stats(bg_learn_stats_t *out);

#endif /* DSCO_BG_LEARN_H */
