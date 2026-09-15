#include "provider_transport.h"

CURLcode provider_transport_perform(CURLM **cache, CURL *easy, const bool *terminal) {
    if (!cache || !easy)
        return CURLE_FAILED_INIT;
    if (!*cache)
        *cache = curl_multi_init();
    if (!*cache || curl_multi_add_handle(*cache, easy) != CURLM_OK)
        return CURLE_FAILED_INIT;

    CURLcode result = CURLE_RECV_ERROR;
    for (;;) {
        int running = 0;
        if (curl_multi_perform(*cache, &running) != CURLM_OK)
            break;
        int remaining = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(*cache, &remaining))) {
            if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
                result = message->data.result;
                goto finished;
            }
        }
        if (!running)
            break;
        if (terminal && *terminal) {
            /* The parser already received the terminal event. If HTTP also
             * finished in this read, CURLMSG_DONE above preserves keepalive.
             * Otherwise close now: no grace timer, idle wait or replay, and no
             * reuse of a cancelled HTTP/2 stream's possibly damaged transport. */
            curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        int activity = 0;
        if (curl_multi_poll(*cache, NULL, 0, 100, &activity) != CURLM_OK)
            break;
    }
finished:
    curl_multi_remove_handle(*cache, easy);
    return result;
}
