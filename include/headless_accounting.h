#ifndef DSCO_HEADLESS_ACCOUNTING_H
#define DSCO_HEADLESS_ACCOUNTING_H
#include "llm.h"
/* Account one provider response exactly once, including reported partial usage.
 * Returns false when a successful paid response cannot be priced safely.
 * Subscription billing never erases inference usage or its estimated value. */
bool headless_account_response(session_state_t *session, const char *provider,
                               const char *request_key, const stream_result_t *response);
/* Change provider/model defaults without erasing the run's spend history. */
void headless_session_retarget(session_state_t *session, const char *model);
#endif
