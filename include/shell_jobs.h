#ifndef DSCO_SHELL_JOBS_H
#define DSCO_SHELL_JOBS_H
#include <stdbool.h>
#include <stddef.h>
/* Called only behind the existing bash execution gate. */
bool shell_jobs_execute(const char *input, char *result, size_t rlen);
/* Private re-exec entry, before threads/provider/UI initialization. */
int shell_jobs_supervise(const char *job_id);
#endif
