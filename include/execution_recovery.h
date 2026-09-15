#ifndef DSCO_EXECUTION_RECOVERY_H
#define DSCO_EXECUTION_RECOVERY_H

#include <stdbool.h>
#include <stdio.h>

/* Read-only projection of a bounded, CRC-validated WAL snapshot. Never resumes
 * a task, runs a tool, modifies a receipt, or authorizes a retry. Exit codes:
 * 0 = complete evidence with no unresolved attempts; 1 = incomplete/invalid
 * evidence or I/O failure; 2 = invalid arguments; 3 = unresolved attempts. */
bool execution_recovery_valid_run_id(const char *run_id);
int execution_recovery_report(const char *journal_path, const char *run_id, FILE *out);

#endif
