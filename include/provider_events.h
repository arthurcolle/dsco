#ifndef DSCO_PROVIDER_EVENTS_H
#define DSCO_PROVIDER_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One actual streaming HTTP attempt. Identity excludes URL credentials,
 * query, fragment and all request headers. Disabled capture is a cheap no-op. */
typedef struct {
    char *identity_json;
    char id[80];
    uint64_t body_bytes;
} provider_event_attempt_t;

bool provider_event_start(provider_event_attempt_t *attempt, const char *provider,
                          const char *request_json, const char *endpoint);
bool provider_event_body(provider_event_attempt_t *attempt, const void *bytes, size_t length);
/* Records HTTP transport completion, not application acceptance. raw_code and
 * effective_code distinguish curl termination after a protocol terminal event. */
bool provider_event_finish(provider_event_attempt_t *attempt, int raw_code,
                           int effective_code, long http_status, bool terminal_received,
                           bool protocol_error, const char *error_body_text);
bool provider_event_retry(const char *provider, const char *request_json,
                          const char *reason, long delay_ms);

#endif
