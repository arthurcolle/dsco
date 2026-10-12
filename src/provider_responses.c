#include "provider_responses.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *explicit_model(provider_t *p, session_state_t *session) {
    const char *model = session ? session->model : NULL;
    if (!model || !model[0])
        return NULL;
    const provider_profile_t *profile = provider_profile_find(p->name);
    if (!profile)
        return model;
    for (int i = -1; i < PROVIDER_PROFILE_MAX_ALIASES; i++) {
        const char *prefix = i < 0 ? profile->name : profile->aliases[i];
        if (!prefix)
            break;
        size_t n = strlen(prefix);
        if (strncmp(model, prefix, n) == 0 && (model[n] == '/' || model[n] == ':'))
            return model[n + 1] ? model + n + 1 : NULL;
    }
    return model;
}

static char *public_responses_request(provider_t *p, conversation_t *conv,
                                      session_state_t *session, int max_tokens,
                                      const char *credential) {
    (void)credential;
    const char *model = explicit_model(p, session);
    if (!model)
        return NULL;
    jbuf_t b;
    jbuf_init(&b, 16384);
    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, model);
    bool has_tools = provider_responses_append_context(&b, conv, session);
    jbuf_append(&b, ",\"max_output_tokens\":");
    jbuf_append_int(&b, max_tokens > 0 ? max_tokens : 1024);
    if (has_tools) {
        const char *choice = session && session->tool_choice[0] ? session->tool_choice : "auto";
        if (strcmp(choice, "any") == 0)
            choice = "required";
        if (strcmp(choice, "none") && strcmp(choice, "required"))
            choice = "auto";
        jbuf_append(&b, ",\"tool_choice\":");
        jbuf_append_json_str(&b, choice);
    }
    if (session && session->effort[0]) {
        jbuf_append(&b, ",\"reasoning\":{\"effort\":");
        jbuf_append_json_str(&b, session->effort);
        jbuf_append(&b, "}");
    }
    /* Cache identity is opt-in; never import a Claude or ChatGPT session ID. */
    if (session && session->prompt_cache_key[0]) {
        jbuf_append(&b, ",\"prompt_cache_key\":");
        jbuf_append_json_str(&b, session->prompt_cache_key);
    }
    jbuf_append(&b, ",\"store\":false,\"stream\":true}");
    return b.data;
}

static bool valid_key(const char *key) {
    return key && key[0] && !strchr(key, '\r') && !strchr(key, '\n');
}

static struct curl_slist *public_responses_headers(provider_t *p, const char *api_key) {
    (void)p;
    if (!valid_key(api_key))
        return NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    jbuf_t auth;
    jbuf_init(&auth, 256);
    jbuf_append(&auth, "Authorization: Bearer ");
    jbuf_append(&auth, api_key);
    headers = curl_slist_append(headers, auth.data);
    jbuf_free(&auth);
    return headers;
}

stream_result_t provider_public_responses_stream(
    provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb,
    stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb,
    stream_thinking_cb thinking_cb, void *cb_ctx) {
    if (!valid_key(api_key) || !request_json || !p || !p->api_url || !p->api_url[0]) {
        stream_result_t result = {0};
        result.parsed.stop_reason = safe_strdup(!valid_key(api_key) ? "missing_api_key"
                                                                  : "invalid_request");
        return result;
    }
    return provider_responses_stream_once(p, api_key, request_json, text_cb, tool_cb,
                                          tool_delta_cb, thinking_cb, cb_ctx);
}

static const char *custom_base(const provider_profile_t *profile) {
    char env_name[128];
    for (int i = -1; i < PROVIDER_PROFILE_MAX_ALIASES; i++) {
        const char *name = i < 0 ? profile->name : profile->aliases[i];
        if (!name)
            break;
        size_t len = strlen(name);
        if (len + sizeof("_API_BASE") > sizeof(env_name))
            continue;
        for (size_t j = 0; j < len; j++)
            env_name[j] = isalnum((unsigned char)name[j])
                              ? (char)toupper((unsigned char)name[j]) : '_';
        memcpy(env_name + len, "_API_BASE", sizeof("_API_BASE"));
        const char *value = getenv(env_name);
        if (value && value[0])
            return value;
    }
    return profile->transport_base_url;
}

provider_t *provider_public_responses_create(const provider_profile_t *profile) {
    if (!profile || profile->transport != PROVIDER_TRANSPORT_PUBLIC_RESPONSES)
        return NULL;
    provider_t *p = safe_malloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    p->name = profile->name;
    const char *base = custom_base(profile);
    jbuf_t url;
    jbuf_init(&url, 256);
    jbuf_append(&url, base ? base : "");
    while (url.len && url.data[url.len - 1] == '/')
        url.data[--url.len] = '\0';
    if (url.len && !(url.len >= 10 && strcmp(url.data + url.len - 10, "/responses") == 0))
        jbuf_append(&url, "/responses");
    p->data = url.data;
    p->api_url = url.data;
    p->build_request = public_responses_request;
    p->build_headers = public_responses_headers;
    p->stream = provider_public_responses_stream;
    return p;
}
