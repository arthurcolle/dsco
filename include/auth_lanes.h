#ifndef DSCO_AUTH_LANES_H
#define DSCO_AUTH_LANES_H

#include <stdbool.h>
#include <stddef.h>

/* A named auth profile is an account principal, not a model/provider alias.
 * Keep the accepted alphabet deliberately path-safe: profile names are used
 * below ~/.dsco/auth/<product>/ without shell expansion. */
bool dsco_auth_profile_name_valid(const char *profile);

/* Resolve and apply isolated homes for subscription CLIs.  An empty profile
 * means the CLI's normal default home; a non-empty profile means an isolated
 * principal below DSCO_{GROK,KIMI}_PROFILES_ROOT (or ~/.dsco/auth/...). */
bool dsco_auth_grok_home(const char *profile, char *out, size_t out_len);
bool dsco_auth_kimi_home(const char *profile, char *out, size_t out_len);
bool dsco_auth_grok_profile_ready(const char *profile);
bool dsco_auth_kimi_profile_ready(const char *profile);
bool dsco_auth_apply_grok_profile(const char *profile);
bool dsco_auth_apply_kimi_profile(const char *profile);

/* `dsco auth lanes` and explicit device-login handoffs. */
int dsco_auth_lanes_cli(int argc, char **argv);

#endif /* DSCO_AUTH_LANES_H */
