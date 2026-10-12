#include "mcp_diagnostics.h"
#include "json_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONNECTION_LIMIT 128
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[64], state[32];
    long http_status;
    int tools;
    time_t observed_at;
} observations[CONNECTION_LIMIT];
static size_t count;

void mcp_diagnostics_record(const char *name, const char *state, long status, int tools) {
    if (!name || !*name || !state) return;
    pthread_mutex_lock(&lock);
    size_t i;
    for (i = 0; i < count; i++) if (!strcmp(observations[i].name, name)) break;
    if (i == count && count < CONNECTION_LIMIT) count++;
    if (i < CONNECTION_LIMIT) {
        snprintf(observations[i].name, sizeof(observations[i].name), "%s", name);
        snprintf(observations[i].state, sizeof(observations[i].state), "%s", state);
        observations[i].http_status = status;
        observations[i].tools = tools;
        observations[i].observed_at = time(NULL);
    }
    pthread_mutex_unlock(&lock);
}

char *mcp_diagnostics_json(void) {
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"schema\":\"dsco.mcp.connections.v1\",\"scope\":\"process_observations\",\"connections\":[");
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < count; i++) {
        if (i) jbuf_append(&b, ",");
        jbuf_append(&b, "{\"name\":"); jbuf_append_json_str(&b, observations[i].name);
        jbuf_append(&b, ",\"state\":"); jbuf_append_json_str(&b, observations[i].state);
        jbuf_appendf(&b, ",\"http_status\":%ld,\"tool_count\":%d,\"observed_at_unix\":%lld,\"next_step\":",
                     observations[i].http_status, observations[i].tools, (long long)observations[i].observed_at);
        const char *state = observations[i].state;
        const char *next = !strcmp(state, "excluded_by_filter") ? "Select this connection with --mcp-server; explicit filters are not overridden."
                         : !strcmp(state, "disabled") ? "Enable this connection only with operator authorization."
                         : !strcmp(state, "authentication_required") ? "Refresh the configured credential with the canonical issuer; do not weaken authentication."
                         : !strcmp(state, "forbidden") ? "Obtain the required account scope; do not bypass authorization."
                         : !strcmp(state, "connected") ? "Discover and load a namespaced tool, then invoke it through the normal capability gate."
                         : !strcmp(state, "connecting") ? "Connection initialization is in progress."
                         : "Check transport, endpoint and service availability; retry only within a bounded budget.";
        jbuf_append_json_str(&b, next); jbuf_append(&b, "}");
    }
    pthread_mutex_unlock(&lock);
    jbuf_append(&b, "]}");
    return b.data;
}

void mcp_diagnostics_append(char *json, size_t capacity) {
    if (!json || !capacity || strstr(json, "\"mcp_connections\":")) return;
    size_t len = strnlen(json, capacity);
    if (!len || len >= capacity || json[len-1] != '}') return;
    char *status = mcp_diagnostics_json();
    if (status && strlen(status) + 24 < capacity - len)
        snprintf(json + len - 1, capacity - len + 1, ",\"mcp_connections\":%s}", status);
    free(status);
}
