#ifndef DSCO_ENV_CONFIG_H
#define DSCO_ENV_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Central typed environment configuration helpers.
 *
 * Contract:
 * - Empty/unset/invalid values return the supplied default.
 * - Numeric values are clamped to caller-supplied min/max bounds.
 * - Boolean truthy: 1,true,yes,on,y,t. Falsy: 0,false,no,off,n,f.
 * - Names are never logged here; callers decide whether a variable is sensitive.
 */

bool   dsco_env_truthy_str(const char *v);
bool   dsco_env_falsy_str(const char *v);
bool   dsco_env_bool(const char *name, bool def);
int    dsco_env_int(const char *name, int def, int min_v, int max_v);
long   dsco_env_long(const char *name, long def, long min_v, long max_v);
size_t dsco_env_size(const char *name, size_t def, size_t min_v, size_t max_v);
double dsco_env_double(const char *name, double def, double min_v, double max_v);
const char *dsco_env_str(const char *name, const char *def);

#endif /* DSCO_ENV_CONFIG_H */
