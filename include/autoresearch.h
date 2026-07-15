#ifndef DSCO_AUTORESEARCH_H
#define DSCO_AUTORESEARCH_H

/* Karpathy-style bounded experiment loop.  The CLI owns durable manifests and
 * launches detached workers; each worker only permits one configured source
 * file to advance when an independently-run fixed evaluator improves it. */
int autoresearch_cli(int argc, char **argv);

#endif /* DSCO_AUTORESEARCH_H */
