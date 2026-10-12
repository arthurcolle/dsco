#include "goal_native.h"
#include "native_session_details.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    session_state_t *s = calloc(1, sizeof(*s));
    assert(s);
    char out[768];
    assert(!goal_native_format(s, out, sizeof(out)) && !out[0]);
    s->goal_status = DSCO_GOAL_ACTIVE;
    strcpy(s->goal_objective, "ship\033[31m\nverified runtime");
    s->goal_turn_limit = 8;
    s->goal_turns = 2;
    s->goal_token_budget = 6000;
    s->goal_queue.initialized = true;
    s->goal_queue.current_id = 12;
    s->goal_queue.revision = 7;
    s->goal_queue.task_count = 1;
    s->goal_queue.tasks[0].id = 12;
    s->goal_queue.tasks[0].status = GOAL_QUEUE_TASK_ACTIVE;
    strcpy(s->goal_queue.tasks[0].title, "test\nactual binary");
    assert(goal_native_format(s, out, sizeof(out)));
    assert(strstr(out, "Current task: test actual binary"));
    assert(strstr(out, "Turns: 2 / 8"));
    assert(strstr(out, "Token budget: set (6000 tokens)"));
    assert(!strchr(out, '\033'));
    native_session_details_t details = {.model = "fixture", .configuration = out};
    char rendered[4096];
    native_session_details_format(&details, rendered, sizeof(rendered));
    assert(strstr(rendered, "GOAL SEEKING") &&
           strstr(rendered, "Current task: test actual binary"));
    s->goal_queue.tasks[0].status = GOAL_QUEUE_TASK_COMPLETE;
    assert(goal_native_format(s, out, sizeof(out)) && strstr(out, "none leased"));
    s->goal_status = DSCO_GOAL_BLOCKED;
    strcpy(s->goal_reason, "needs approval");
    assert(goal_native_format(s, out, sizeof(out)) && strstr(out, "Reason: needs approval"));
    char tiny[4] = {'x','x','x','Z'};
    assert(!goal_native_format(s, tiny, 3) && tiny[2] == 0 && tiny[3] == 'Z');
    assert(!goal_native_format(s, NULL, 0));
    free(s);
    puts("goal_native: read-only snapshot, active lease, terminal, sanitization, bounds passed");
    return 0;
}
