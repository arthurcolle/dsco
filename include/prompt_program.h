#ifndef DSCO_PROMPT_PROGRAM_H
#define DSCO_PROMPT_PROGRAM_H
#include <stdbool.h>
#include <stddef.h>
/* Deterministic, local-only compilation. No tools, providers or authority changes.
 * JSON document is bounded to 256 KiB, 64 nodes, 64 KiB/node, 1 MiB total output.
 * On success caller owns *compiled_json; on failure it is NULL. */
bool prompt_program_compile(const char *source, size_t length, char **compiled_json,
                            char *error, size_t error_size);
int prompt_program_cli(int argc, char **argv);
#endif
