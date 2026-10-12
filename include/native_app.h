#ifndef DSCO_NATIVE_APP_H
#define DSCO_NATIVE_APP_H
#include "native_factory.h"
#include "native_ui.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NATIVE_APPS_MAX 32
/* State owns no editor pointers or window IDs. A view is optional. */
typedef enum { NATIVE_APP_READY, NATIVE_APP_RUNNING, NATIVE_APP_BLOCKED,
               NATIVE_APP_DONE, NATIVE_APP_INTERRUPTED } native_app_status_t;
typedef struct {
    uint64_t id, generation, sequence;
    native_factory_t factory;
    char title[96], input[NATIVE_FACTORY_INPUT_MAX];
    native_factory_result_t result;
    native_app_status_t status;
    native_ui_rect_t rect;
    bool view_open;
} native_app_t;
/* Disabled outside explicit standalone workbench mode. Memory-only methods:
 * safe under the window mutex; never call back into windows or do file I/O. */
void native_apps_enable(void);
void native_apps_disable(void);
bool native_apps_enabled(void);
uint64_t native_apps_create(const native_factory_t *factory,const char *title,const char *input,native_ui_rect_t rect);
bool native_apps_get(uint64_t id,native_app_t *out);
int native_apps_snapshot(native_app_t *out,size_t count);
bool native_apps_sync(uint64_t id,const char *title,const char *input,native_ui_rect_t rect,bool view_open);
uint64_t native_apps_begin(uint64_t id,uint64_t sequence);
bool native_apps_complete(uint64_t id,uint64_t sequence,uint64_t generation,const native_factory_result_t *result);
void native_apps_clear(uint64_t id);
void native_apps_interrupt(void);
/* Caller frees encoded JSON. Import is all-or-nothing, bounded and strict;
 * running requests restore as interrupted and are NEVER automatically rerun. */
char *native_apps_export(uint64_t *revision);
bool native_apps_import(const char *json,size_t length);
/* Explicit standalone host persistence only. Must not run under model locks.
 * Parent directory must already exist and be owner-controlled; a held sidecar
 * lock prevents two workbenches from writing one store. No tool dispatch. */
bool native_app_store_open(const char *path,char *error,size_t cap);
bool native_app_store_checkpoint(char *error,size_t cap);
void native_app_store_close(void);
#endif
