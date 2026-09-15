#ifndef DSCO_TRACE_KG_H
#define DSCO_TRACE_KG_H
#include <stdio.h>
/* Read-only Chronicle projection. No raw prompts, arguments or results are
 * exported. Evidence is observed telemetry, not independently verified truth.
 * Streams deterministic JSONL; callers must discard output on nonzero return. */
int trace_kg_export(const char *database, const char *session, FILE *out);
int trace_kg_cli(int argc, char **argv);
#endif
