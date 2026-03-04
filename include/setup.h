#ifndef DSCO_SETUP_H
#define DSCO_SETUP_H

#include <stdbool.h>
#include <stddef.h>

/* Active profile name ("default" if unset). */
const char *dsco_setup_profile_name(void);

/* Load saved env file into process environment. Returns number of vars loaded. */
int dsco_setup_load_saved_env(void);

/* Autopopulate saved env config from current process env.
 * Returns number of discovered keys (known + generic patterns). */
int dsco_setup_autopopulate(bool overwrite, bool include_generic,
                            char *summary, size_t summary_len);

/* Bootstrap once: if config file does not exist, create it from current env.
 * Returns 1 if created, 0 if skipped, -1 on error. */
int dsco_setup_bootstrap_from_env(char *summary, size_t summary_len);

/* Render setup key/report status to output buffer. Returns bytes written or -1. */
int dsco_setup_report(char *out, size_t out_len);

/* Current resolved path to the setup env file. */
const char *dsco_setup_env_path(void);

#endif
