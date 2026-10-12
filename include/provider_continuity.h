#ifndef DSCO_PROVIDER_CONTINUITY_H
#define DSCO_PROVIDER_CONTINUITY_H
#include <stdbool.h>
bool provider_continuity_enabled(void);
/* Explicit opt-in, native subscription-only alternates; no metered promotion. */
const char *provider_continuity_provider(const char *model);
int provider_continuity_models(const char *current_provider, char models[][128], int cap);
#endif
