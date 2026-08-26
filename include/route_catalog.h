#ifndef DSCO_ROUTE_CATALOG_H
#define DSCO_ROUTE_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Logical routes are distinct from model developers, gateways, and serving
 * infrastructure. A route may be direct, gateway-mediated, or a gateway route
 * pinned to a specific inference provider. */
#define DSCO_ROUTE_CATALOG_CAP 2048
#define DSCO_ROUTE_ID_LEN 256
#define DSCO_ROUTE_NAME_LEN 128
#define DSCO_ROUTE_REGION_LEN 32
#define DSCO_ROUTE_SOURCE_LEN 64

typedef enum {
    DSCO_ROUTE_BILLING_UNKNOWN = 0,
    DSCO_ROUTE_BILLING_SUBSCRIPTION,
    DSCO_ROUTE_BILLING_API,
    DSCO_ROUTE_BILLING_PREPAID,
    DSCO_ROUTE_BILLING_FREE,
    DSCO_ROUTE_BILLING_METERED,
} dsco_route_billing_t;

typedef enum {
    DSCO_ROUTE_UNKNOWN = 0,
    DSCO_ROUTE_CONFIGURED,
    DSCO_ROUTE_READY,
    DSCO_ROUTE_DEGRADED,
    DSCO_ROUTE_TRIPPED,
    DSCO_ROUTE_DISABLED,
} dsco_route_state_t;

typedef struct {
    char route_id[DSCO_ROUTE_ID_LEN];
    char gateway[DSCO_ROUTE_NAME_LEN];
    char model_developer[DSCO_ROUTE_NAME_LEN];
    char model_id[DSCO_ROUTE_NAME_LEN];
    char inference_provider[DSCO_ROUTE_NAME_LEN];
    char endpoint_id[DSCO_ROUTE_NAME_LEN];
    char region[DSCO_ROUTE_REGION_LEN];
    char source[DSCO_ROUTE_SOURCE_LEN];
    char catalog_revision[DSCO_ROUTE_NAME_LEN];
    uint64_t capabilities;
    uint64_t data_policy;
    dsco_route_billing_t billing;
    dsco_route_state_t state;
    int context_tokens;
    int max_output_tokens;
    int max_concurrency;
    double input_usd_per_million;
    double output_usd_per_million;
    double latency_p50_ms;
    double latency_p95_ms;
    long total_requests;
    long total_failures;
    time_t retrieved_at;
    time_t last_probe_at;
} dsco_route_record_t;

typedef struct {
    dsco_route_record_t *records;
    size_t count;
    size_t capacity;
} dsco_route_catalog_t;

void dsco_route_catalog_init(dsco_route_catalog_t *catalog);
void dsco_route_catalog_destroy(dsco_route_catalog_t *catalog);
void dsco_route_catalog_clear(dsco_route_catalog_t *catalog);

/* Adds a normalized route. route_id is the deduplication key. Returns false on
 * invalid input or when the compiled logical-route cap is reached. */
bool dsco_route_catalog_add(dsco_route_catalog_t *catalog,
                            const dsco_route_record_t *record);
dsco_route_record_t *dsco_route_catalog_find(dsco_route_catalog_t *catalog,
                                             const char *route_id);
const dsco_route_record_t *dsco_route_catalog_find_const(const dsco_route_catalog_t *catalog,
                                                         const char *route_id);

/* Merge a health result without changing identity/catalog provenance. */
void dsco_route_catalog_report(dsco_route_catalog_t *catalog, const char *route_id,
                               bool ok, double latency_ms, time_t now);

size_t dsco_route_catalog_count(const dsco_route_catalog_t *catalog);
const char *dsco_route_billing_name(dsco_route_billing_t billing);
const char *dsco_route_state_name(dsco_route_state_t state);

#endif /* DSCO_ROUTE_CATALOG_H */
