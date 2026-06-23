/* src/extension/backend.c — Core Backend Registry */
#include "extension/backend.h"
#include <string.h>
#include <stdlib.h>

#define MAX_BACKENDS 64

static backend_interface_t *registry[BACKEND_COUNT][MAX_BACKENDS];
static int registry_count[BACKEND_COUNT] = {0};

int backend_register(backend_interface_t *backend) {
    if (!backend || registry_count[backend->category] >= MAX_BACKENDS)
        return -1;
    registry[backend->category][registry_count[backend->category]++] = backend;
    backend->status = BACKEND_STATUS_LOADED;
    return 0;
}

backend_interface_t *backend_get(const char *name, backend_category_t cat) {
    for (int i = 0; i < registry_count[cat]; i++) {
        if (strcmp(registry[cat][i]->name, name) == 0)
            return registry[cat][i];
    }
    return NULL;
}

int backend_load(const char *name, backend_category_t cat) {
    backend_interface_t *b = backend_get(name, cat);
    if (!b)
        return -1;
    if (b->init)
        b->init(b->impl);
    b->status = BACKEND_STATUS_LOADED;
    return 0;
}

int backend_unload(const char *name, backend_category_t cat) {
    backend_interface_t *b = backend_get(name, cat);
    if (!b)
        return -1;
    if (b->shutdown)
        b->shutdown(b->impl);
    b->status = BACKEND_STATUS_UNLOADED;
    return 0;
}