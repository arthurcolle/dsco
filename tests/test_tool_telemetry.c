#include "tool_telemetry.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
static void *contention(void *p) {
    tool_timeout_origin_t expected=*(tool_timeout_origin_t *)p;
    for (int i=0;i<100000;i++) {
        tool_telemetry_reset();
        tool_telemetry_timeout(expected);
        assert(tool_telemetry_origin()==expected);
    }
    return NULL;
}
static void *worker(void *p) {
    (void)p;
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_NONE);
    tool_telemetry_timeout(TOOL_TIMEOUT_IDLE);
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_IDLE);
    return 0;
}
int main(void) {
    tool_telemetry_reset();
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_NONE);
    tool_telemetry_timeout(TOOL_TIMEOUT_WALL);
    tool_telemetry_timeout(TOOL_TIMEOUT_IDLE);
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_WALL);
    pthread_t t; assert(!pthread_create(&t,0,worker,0)); assert(!pthread_join(t,0));
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_WALL);
    assert(!strcmp(tool_timeout_origin_name(TOOL_TIMEOUT_IDLE),"harness_idle"));
    assert(!strcmp(tool_timeout_origin_name(TOOL_TIMEOUT_WALL),"harness_wall"));
    assert(!strcmp(tool_timeout_origin_name(TOOL_TIMEOUT_WATCHDOG),"tool_deadline"));
    tool_telemetry_reset(); assert(tool_telemetry_origin()==TOOL_TIMEOUT_NONE);
    pthread_t threads[8]; tool_timeout_origin_t expected[8];
    for (int i=0;i<8;i++) { expected[i]=i%2?TOOL_TIMEOUT_IDLE:TOOL_TIMEOUT_WALL; assert(!pthread_create(&threads[i],NULL,contention,&expected[i])); }
    for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_NONE);
}
