/* extension/backend.h — Core Extension Hook System */
#ifndef DSCO_EXTENSION_BACKEND_H
#define DSCO_EXTENSION_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend category */
typedef enum {
    BACKEND_NUMERICAL = 1,
    BACKEND_CONCURRENCY,
    BACKEND_NETWORK,
    BACKEND_SERIALIZATION,
    BACKEND_GRAPH,
    BACKEND_CRYPTO,
    BACKEND_COUNT
} backend_category_t;

/* Backend status */
typedef enum {
    BACKEND_STATUS_UNLOADED = 0,
    BACKEND_STATUS_LOADED,
    BACKEND_STATUS_ERROR
} backend_status_t;

/* Generic backend interface */
typedef struct backend_interface {
    const char *name;
    backend_category_t category;
    backend_status_t status;
    void *impl;                    /* Opaque implementation pointer */
    int (*init)(void *impl);
    void (*shutdown)(void *impl);
    const char *(*version)(void *impl);
} backend_interface_t;

/* Registry */
int backend_register(backend_interface_t *backend);
backend_interface_t *backend_get(const char *name, backend_category_t cat);
int backend_load(const char *name, backend_category_t cat);
int backend_unload(const char *name, backend_category_t cat);

/* Skill requirement declaration */
typedef struct {
    backend_category_t category;
    const char *required_name;     /* NULL = any in category */
    int mandatory;
} backend_requirement_t;

#ifdef __cplusplus
}
#endif
#endif /* DSCO_EXTENSION_BACKEND_H */