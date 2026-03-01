#ifndef DSCO_BASELINE_H
#define DSCO_BASELINE_H

#include <stdbool.h>

/* Initialize baseline storage for this process instance. */
bool baseline_start(const char *model, const char *mode);

/* Flush and close baseline storage. Safe to call multiple times. */
void baseline_stop(void);

/* Record a timeline event for the current instance. */
bool baseline_log(const char *category, const char *title,
                  const char *detail, const char *metadata_json);

/* Current instance id and SQLite file path. Empty string if not started. */
const char *baseline_instance_id(void);
const char *baseline_db_path(void);

/* Run a local HTTP timeline server (blocking). */
int baseline_serve_http(int port, const char *default_instance_filter);

#endif
