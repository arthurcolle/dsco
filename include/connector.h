#ifndef DSCO_CONNECTOR_H
#define DSCO_CONNECTOR_H

#include <stdbool.h>
#include <stddef.h>

/* ── Future-proof baseline connector ──────────────────────────────────────
 * A single, domain- and transport-agnostic seam that the surrounding
 * construct plugs every external system into: today's Tool Management API,
 * tomorrow's blockchains, credit/settlement rails, interface systems,
 * robotics control, and direct neural/haptic channels over nanomaterials.
 *
 * The contract never changes. A backend is a "kind" that fills a vtable and
 * registers once; everything else (open → invoke/stream → describe → close)
 * is uniform JSON in / JSON out. New kinds extend reach without touching the
 * core, so the baseline survives the systems it will one day connect to. */

/* Capability bitmask — extend by adding flags, never by reordering. */
typedef enum {
    CONN_CAP_INVOKE    = 1u << 0,  /* request/response                       */
    CONN_CAP_STREAM    = 1u << 1,  /* incremental/streamed results           */
    CONN_CAP_SUBSCRIBE = 1u << 2,  /* event subscription (telemetry, chains) */
    CONN_CAP_TX        = 1u << 3,  /* signed/transactional ops (chain,credit)*/
    CONN_CAP_SENSE     = 1u << 4,  /* sensor / haptic / neural input         */
    CONN_CAP_ACTUATE   = 1u << 5,  /* actuation / output (robotics, haptic)  */
} conn_cap_t;

/* OSI layer(s) a kind operates at — the seam is not application-bound. A kind
 * advertises every layer it spans so callers can reason about reach (raw
 * frames at L2, packets at L3/L4, sessions at L5, encoded payloads at L7). */
typedef enum {
    OSI_L1_PHYSICAL     = 1u << 0,
    OSI_L2_DATALINK     = 1u << 1,
    OSI_L3_NETWORK      = 1u << 2,
    OSI_L4_TRANSPORT    = 1u << 3,
    OSI_L5_SESSION      = 1u << 4,
    OSI_L6_PRESENTATION = 1u << 5,
    OSI_L7_APPLICATION  = 1u << 6,
} conn_osi_t;

/* Result of an invocation. status: 0 ok, <0 transport, >0 domain/HTTP code. */
typedef struct {
    long   status;
    char  *body;            /* malloc'd JSON, caller frees via conn_result_free */
    char   error[256];
} conn_result_t;

void conn_result_free(conn_result_t *r);

/* Streaming sink: return false to stop early. */
typedef bool (*conn_chunk_cb)(const char *chunk, void *ctx);

/* The vtable a kind implements. Any fn may be NULL when unsupported; the
 * capabilities bitmask advertises what callers can rely on. */
typedef struct {
    const char *kind;          /* "tool", "chain", "credit", "robot", "neuro" */
    const char *description;
    unsigned    capabilities;  /* conn_cap_t bitmask */
    unsigned    osi_layers;    /* conn_osi_t bitmask — layers this kind spans */

    /* Parse config JSON (may be NULL/empty for defaults); return a private
     * handle or NULL on failure (write reason into err). */
    void *(*open)(const char *config_json, char *err, size_t errlen);

    /* method + params JSON → result. Required when CONN_CAP_INVOKE is set. */
    void  (*invoke)(void *self, const char *method, const char *params_json,
                    conn_result_t *out);

    /* Like invoke but delivers chunks via cb. Required for CONN_CAP_STREAM. */
    void  (*stream)(void *self, const char *method, const char *params_json,
                    conn_chunk_cb on_chunk, void *ctx, conn_result_t *out);

    /* Capability/manifest doc as malloc'd JSON (caller frees), or NULL. */
    char *(*describe)(void *self);

    /* JSON-Schema for a method's params object, malloc'd (caller frees), or
     * NULL when the type contract is unknown. Enables boundary validation so
     * type errors are caught at the seam, not deep inside a backend. */
    char *(*schema)(void *self, const char *method);

    /* Release the handle. */
    void  (*close)(void *self);
} connector_vtable_t;

/* Opaque live connection. */
typedef struct connector connector_t;

/* ── Registry (kinds) ─────────────────────────────────────────────────── */
int  connector_register(const connector_vtable_t *vt);   /* 0 ok, -1 full/dup */
const connector_vtable_t *connector_find(const char *kind);
int  connector_list(const connector_vtable_t **out, int max); /* count filled */
void connector_register_builtins(void);                  /* idempotent */

/* ── Lifecycle (instances) ────────────────────────────────────────────── */
connector_t *connector_open(const char *kind, const char *config_json,
                            char *err, size_t errlen);
void  connector_invoke(connector_t *c, const char *method,
                       const char *params_json, conn_result_t *out);
void  connector_stream(connector_t *c, const char *method,
                       const char *params_json,
                       conn_chunk_cb on_chunk, void *ctx, conn_result_t *out);
char *connector_describe(connector_t *c);                /* malloc'd, caller frees */
unsigned connector_capabilities(const connector_t *c);
void  connector_close(connector_t *c);

/* JSON-Schema for a method's params, malloc'd (caller frees), or NULL. */
char *connector_schema(connector_t *c, const char *method);

/* Validate params against the method's schema: 1 ok, 0 invalid (reason in err),
 * -1 when no schema is available. Used at the seam to reject type errors before
 * a backend ever sees them. */
int connector_validate(connector_t *c, const char *method,
                       const char *params_json, char *err, size_t errlen);

/* When enabled, connector_invoke validates params before dispatch and fails
 * fast on a type/required-field violation instead of calling the backend. */
void connector_set_validate(int on);

/* CLI entry for `dsco connect …`: kinds, describe <kind>, <kind> <method> k=v…
 * Returns a process exit code. */
int connector_cli(int argc, char **argv);

#endif /* DSCO_CONNECTOR_H */
