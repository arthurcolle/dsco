#ifndef DSCO_MODEL_CATALOG_REFRESH_H
#define DSCO_MODEL_CATALOG_REFRESH_H
/* Call before dispatch and before creating any runtime threads. */
void model_catalog_refresh_start(const char *executable);
int model_catalog_refresh_worker(void);
#endif
