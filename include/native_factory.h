#ifndef DSCO_NATIVE_FACTORY_H
#define DSCO_NATIVE_FACTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NATIVE_FACTORY_MAX 8
#define NATIVE_FACTORY_INPUT_MAX 8192
#define NATIVE_FACTORY_OUTPUT_MAX 512

typedef enum { NATIVE_EXEC_JSON=1, NATIVE_EXEC_SHA256, NATIVE_EXEC_TEXT_STATS } native_executor_kind_t;
typedef struct { char name[48],title[96]; native_executor_kind_t executor; } native_factory_t;
typedef struct {
    bool ok;
    char output[NATIVE_FACTORY_OUTPUT_MAX];
    char input_sha256[65];
    uint64_t elapsed_us;
} native_factory_result_t;
/* Immutable descriptor aliases over three compiled pure executors. No shell,
 * filesystem, network, model, credential or arbitrary callback can be bound. */
void native_factory_reset(void);
bool native_factory_define(const char *name,const char *title,const char *executor);
bool native_factory_lookup(const char *name,native_factory_t *out);
bool native_factory_execute(const native_factory_t *factory,const char *input,native_factory_result_t *out);
#endif
