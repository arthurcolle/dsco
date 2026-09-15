/* Real production SQLite IPC: populated destination queues and competing processes. */
#include "ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
int main(int argc,char **argv) {
    assert(argc>=4);
    const char *mode=argv[1],*path=argv[2],*worker=argv[3];
    int n=argc>4?atoi(argv[4]):128;
    double begin=ms(); assert(ipc_init(path,worker)); double init_ms=ms()-begin;
    if (!strcmp(mode,"init")) { ipc_shutdown(); puts("{}"); return 0; }
    assert(ipc_register(NULL,0,"queue-benchmark","native"));
    if (!strcmp(mode,"race")) { puts("ready"); fflush(stdout); assert(getchar()=='g'); }
    printf("{\"init_ms\":%.6f,\"samples\":[",init_ms);
    int count=0;
    for (int i=0;i<n;i++) {
        ipc_task_t task;
        begin=ms();
        int found=!strcmp(mode,"targeted")?ipc_task_claim_targeted(&task):ipc_task_claim(&task);
        double elapsed=ms()-begin;
        if (!strcmp(mode,"race")&&!found) break;
        if (i) putchar(',');
        printf("{\"ms\":%.6f,\"id\":%d}",elapsed,found?task.id:0);
        if (found) { assert(ipc_task_start(task.id)); assert(ipc_task_complete(task.id,"done")); count++; }
    }
    printf("],\"claimed\":%d}\n",count);
    ipc_shutdown(); return 0;
}
