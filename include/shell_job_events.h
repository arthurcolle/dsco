#ifndef DSCO_SHELL_JOB_EVENTS_H
#define DSCO_SHELL_JOB_EVENTS_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
/* Single owner. Watch the directory (receipts are atomically replaced) and an
 * optional process. Never reaps/signals a process. Open before checking state. */
typedef struct shell_job_events shell_job_events_t;
shell_job_events_t *shell_job_events_open(const char *directory, pid_t process,
                                        bool observe_control_signals);
int shell_job_events_wait(shell_job_events_t *events, int timeout_ms);
void shell_job_events_close(shell_job_events_t *events);
const char *shell_job_events_backend(void);
uint64_t shell_job_events_wait_count(const shell_job_events_t *events);
uint64_t shell_job_events_wake_count(const shell_job_events_t *events);
#endif
