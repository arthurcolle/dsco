#include "native_task_service.h"
#include "native_windows.h"
#include "pixel_tui.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
static pthread_mutex_t service_lock=PTHREAD_MUTEX_INITIALIZER;
/* Keep newly created workers idle until the whole pool is available. Workers
 * never take service_lock: start/stop hold it while joining the previous pool. */
static pthread_mutex_t launch_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t workers[NATIVE_TASK_SERVICE_WORKERS];
static unsigned worker_count;
static atomic_bool stopping;
static bool exit_registered;
static FILE *output;
static void *execute_loop(void *unused) {
    (void)unused;
    pthread_mutex_lock(&launch_lock);
    pthread_mutex_unlock(&launch_lock);
    while(!atomic_load(&stopping)) {
        native_window_task_t request;
        /* A wake before entering wait_task is not sticky. The finite wait is
         * the fallback for that race; stop is checked even with a busy queue. */
        if(!native_windows_wait_task(&request,100))continue;
        /* A dequeue racing with stop owns its request and must complete it. */
        native_factory_result_t result;
        native_factory_execute(&request.factory,request.input,&result);
        native_windows_complete_task(&request,&result);
        pixel_tui_session_windows_changed(output);
    }
    return NULL;
}
static void join_workers(void) {
    native_windows_wake_tasks();
    for(unsigned i=0;i<worker_count;i++)pthread_join(workers[i],NULL);
    worker_count=0;output=NULL;
}
bool native_task_service_start(FILE *out) {
    pthread_mutex_lock(&service_lock);
    if(worker_count){pthread_mutex_unlock(&service_lock);return true;}
    if(!exit_registered){
        if(atexit(native_task_service_stop)!=0){pthread_mutex_unlock(&service_lock);return false;}
        exit_registered=true;
    }
    pthread_mutex_lock(&launch_lock);
    output=out?out:stderr;atomic_store(&stopping,false);
    for(;worker_count<NATIVE_TASK_SERVICE_WORKERS;worker_count++) {
        if(pthread_create(&workers[worker_count],NULL,execute_loop,NULL)!=0) {
            atomic_store(&stopping,true);
            pthread_mutex_unlock(&launch_lock);
            join_workers();
            pthread_mutex_unlock(&service_lock);return false;
        }
    }
    pthread_mutex_unlock(&launch_lock);
    pthread_mutex_unlock(&service_lock);return true;
}
void native_task_service_stop(void) {
    pthread_mutex_lock(&service_lock);
    if(worker_count){atomic_store(&stopping,true);join_workers();}
    pthread_mutex_unlock(&service_lock);
}
