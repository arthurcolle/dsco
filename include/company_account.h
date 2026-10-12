#ifndef DSCO_COMPANY_ACCOUNT_H
#define DSCO_COMPANY_ACCOUNT_H
#include <stdbool.h>
/* First-party company identity, separate from model-provider credentials. */
int company_account_cli(int argc, char **argv);
/* New public sessions enroll; explicitly migrated operator installs can opt out.
 * This local UX check is NOT a hosted-service authorization boundary. */
bool company_account_enrolled(void);
int company_account_require(void);
#endif
