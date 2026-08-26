#include "route_catalog.h"

#include <stdlib.h>
#include <string.h>

static bool valid_record(const dsco_route_record_t *r) {
    return r && r->route_id[0] && r->model_id[0];
}

void dsco_route_catalog_init(dsco_route_catalog_t *catalog) {
    if (!catalog) return;
    memset(catalog, 0, sizeof(*catalog));
}

void dsco_route_catalog_clear(dsco_route_catalog_t *catalog) {
    if (!catalog) return;
    catalog->count = 0;
}

void dsco_route_catalog_destroy(dsco_route_catalog_t *catalog) {
    if (!catalog) return;
    free(catalog->records);
    memset(catalog, 0, sizeof(*catalog));
}

static bool reserve(dsco_route_catalog_t *catalog, size_t needed) {
    if (!catalog || needed > DSCO_ROUTE_CATALOG_CAP) return false;
    if (catalog->capacity >= needed) return true;
    size_t cap = catalog->capacity ? catalog->capacity * 2 : 64;
    if (cap > DSCO_ROUTE_CATALOG_CAP) cap = DSCO_ROUTE_CATALOG_CAP;
    dsco_route_record_t *p = realloc(catalog->records, cap * sizeof(*p));
    if (!p) return false;
    catalog->records = p;
    catalog->capacity = cap;
    return true;
}

bool dsco_route_catalog_add(dsco_route_catalog_t *catalog,
                            const dsco_route_record_t *record) {
    if (!catalog || !valid_record(record)) return false;
    dsco_route_record_t *existing = dsco_route_catalog_find(catalog, record->route_id);
    if (existing) {
        *existing = *record;
        return true;
    }
    if (catalog->count >= DSCO_ROUTE_CATALOG_CAP || !reserve(catalog, catalog->count + 1))
        return false;
    catalog->records[catalog->count++] = *record;
    return true;
}

dsco_route_record_t *dsco_route_catalog_find(dsco_route_catalog_t *catalog,
                                             const char *route_id) {
    if (!catalog || !route_id || !route_id[0]) return NULL;
    for (size_t i = 0; i < catalog->count; i++)
        if (strcmp(catalog->records[i].route_id, route_id) == 0)
            return &catalog->records[i];
    return NULL;
}

const dsco_route_record_t *dsco_route_catalog_find_const(const dsco_route_catalog_t *catalog,
                                                         const char *route_id) {
    return dsco_route_catalog_find((dsco_route_catalog_t *)catalog, route_id);
}

void dsco_route_catalog_report(dsco_route_catalog_t *catalog, const char *route_id,
                               bool ok, double latency_ms, time_t now) {
    dsco_route_record_t *r = dsco_route_catalog_find(catalog, route_id);
    if (!r) return;
    r->last_probe_at = now;
    r->total_requests++;
    if (latency_ms > 0) {
        r->latency_p50_ms = latency_ms;
        if (r->latency_p95_ms < latency_ms) r->latency_p95_ms = latency_ms;
    }
    if (ok) {
        r->state = DSCO_ROUTE_READY;
        r->total_failures = 0;
    } else {
        r->total_failures++;
        r->state = DSCO_ROUTE_DEGRADED;
    }
}

size_t dsco_route_catalog_count(const dsco_route_catalog_t *catalog) {
    return catalog ? catalog->count : 0;
}

const char *dsco_route_billing_name(dsco_route_billing_t billing) {
    switch (billing) {
    case DSCO_ROUTE_BILLING_SUBSCRIPTION: return "subscription";
    case DSCO_ROUTE_BILLING_API: return "api";
    case DSCO_ROUTE_BILLING_PREPAID: return "prepaid";
    case DSCO_ROUTE_BILLING_FREE: return "free";
    case DSCO_ROUTE_BILLING_METERED: return "metered";
    default: return "unknown";
    }
}

const char *dsco_route_state_name(dsco_route_state_t state) {
    switch (state) {
    case DSCO_ROUTE_CONFIGURED: return "configured";
    case DSCO_ROUTE_READY: return "ready";
    case DSCO_ROUTE_DEGRADED: return "degraded";
    case DSCO_ROUTE_TRIPPED: return "tripped";
    case DSCO_ROUTE_DISABLED: return "disabled";
    default: return "unknown";
    }
}
