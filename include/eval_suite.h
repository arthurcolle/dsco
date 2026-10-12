#ifndef DSCO_EVAL_SUITE_H
#define DSCO_EVAL_SUITE_H

/* `dsco eval run SUITE.json --out DIR`: offline JSON eval suites graded by
 * deterministic rules. Returns a process exit code. */
int eval_suite_cli(int argc, char **argv);

#endif
