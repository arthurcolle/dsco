#ifndef DSCO_PROVIDER_TRANSPORT_H
#define DSCO_PROVIDER_TRANSPORT_H

#include <stdbool.h>
#include <curl/curl.h>

/* A provider-owned connection cache. Complete HTTP responses stay reusable;
 * semantically finished but still-open SSE transfers stop without waiting. */
CURLcode provider_transport_perform(CURLM **cache, CURL *easy, const bool *terminal);

#endif
