#include "native_windows.h"
#include "native_factory.h"
#include "native_task_service.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include "pixel_tui.h"
static int checks,failures;static atomic_int notifications;
/* Headless notification/executor barriers; no compositor or windows are opened. */
static pthread_mutex_t test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed=PTHREAD_COND_INITIALIZER;
static bool notify_hold,execute_hold,wait_hold;
static int callbacks,notify_active,bad_outputs,executions,active,peak;
static int wait_mode,wait_entries,wait_calls,timeouts,wakes,stop_done;
static int create_attempts,created,joined,fail_create=-1;
static bool fail_atexit;
static FILE *expected_output;
void pixel_tui_session_windows_changed(FILE *out){
 pthread_mutex_lock(&test_lock);
 callbacks++;notify_active++;if(expected_output && out!=expected_output)bad_outputs++;
 pthread_cond_broadcast(&test_changed);
 while(notify_hold)pthread_cond_wait(&test_changed,&test_lock);
 notify_active--;pthread_mutex_unlock(&test_lock);
 atomic_fetch_add(&notifications,1);
}
/* Compile the SAME service with test-local syscall/executor seams. This keeps
 * fault injection out of the production API and needs no Makefile changes. */
static int test_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg){
 if(create_attempts++==fail_create)return EAGAIN;
 int rc=pthread_create(thread,attr,fn,arg);if(!rc)created++;return rc;
}
static int test_join(pthread_t thread,void **result){
 int rc=pthread_join(thread,result);if(!rc)joined++;return rc;
}
static int test_atexit(void (*fn)(void)){return fail_atexit?-1:atexit(fn);}
static bool test_execute(const native_factory_t *factory,const char *input,native_factory_result_t *result){
 pthread_mutex_lock(&test_lock);executions++;active++;if(active>peak)peak=active;
 pthread_cond_broadcast(&test_changed);
 while(execute_hold)pthread_cond_wait(&test_changed,&test_lock);
 pthread_mutex_unlock(&test_lock);
 bool ok=native_factory_execute(factory,input,result);
 pthread_mutex_lock(&test_lock);active--;pthread_mutex_unlock(&test_lock);return ok;
}
static bool test_wait(native_window_task_t *request,int timeout_ms){
 pthread_mutex_lock(&test_lock);wait_calls++;
 if(wait_mode==1 && wait_hold){
  wait_entries++;pthread_cond_broadcast(&test_changed);
  while(wait_hold)pthread_cond_wait(&test_changed,&test_lock);
 }
 pthread_mutex_unlock(&test_lock);
 bool ok=native_windows_wait_task(request,timeout_ms);
 pthread_mutex_lock(&test_lock);
 if(!ok && timeout_ms>0)timeouts++;
 if(ok && wait_mode==2 && wait_hold){
  wait_entries++;pthread_cond_broadcast(&test_changed);
  while(wait_hold)pthread_cond_wait(&test_changed,&test_lock);
 }
 pthread_mutex_unlock(&test_lock);return ok;
}
static void test_wake(void){
 native_windows_wake_tasks();
 pthread_mutex_lock(&test_lock);wakes++;pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
}
bool injected_start(FILE *out);
void injected_stop(void);
#define native_task_service_start injected_start
#define native_task_service_stop injected_stop
#define pthread_create test_create
#define pthread_join test_join
#define atexit test_atexit
#define native_factory_execute test_execute
#define native_windows_wait_task test_wait
#define native_windows_wake_tasks test_wake
#include "../src/native_task_service.c"
#undef native_task_service_start
#undef native_task_service_stop
#undef pthread_create
#undef pthread_join
#undef atexit
#undef native_factory_execute
#undef native_windows_wait_task
#undef native_windows_wake_tasks
#define CHECK(x) do{checks++;if(!(x)){failures++;fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);}}while(0)
static uint64_t command(const char *json,bool expected){char *reply=malloc(262144);bool ok=native_windows_command(json,reply,262144);CHECK(ok==expected);yyjson_doc*d=yyjson_read(reply,strlen(reply),0);CHECK(d!=NULL);uint64_t id=yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d),"id"));yyjson_doc_free(d);free(reply);return id;}
static native_window_t view(uint64_t id){native_windows_snapshot_t *s=malloc(sizeof(*s));native_windows_snapshot(s);native_window_t w={0};for(int i=0;i<s->count;i++)if(s->windows[i].id==id)w=s->windows[i];free(s);return w;}
/* Every blocking test has a deadline; a broken lifecycle must fail, not hang. */
static void await_count(int *value,int target){
 struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=5;
 pthread_mutex_lock(&test_lock);
 while(*value<target){
  int rc=pthread_cond_timedwait(&test_changed,&test_lock,&until);
  if(rc){fprintf(stderr,"FAIL waiting for %d (got %d): %s\n",target,*value,strerror(rc));_Exit(1);}
 }
 pthread_mutex_unlock(&test_lock);
}
static void reset_test(void){
 native_windows_reset();
 pthread_mutex_lock(&test_lock);
 notify_hold=execute_hold=wait_hold=false;
 callbacks=notify_active=bad_outputs=executions=active=peak=0;
 wait_mode=wait_entries=wait_calls=timeouts=wakes=stop_done=0;
 create_attempts=created=joined=0;fail_create=-1;fail_atexit=false;
 expected_output=stderr;
 pthread_mutex_unlock(&test_lock);
}
static uint64_t queue_task(void){
 uint64_t id=command("{\"action\":\"factory\",\"factory\":\"sha256\",\"text\":\"abc\"}",true);
 CHECK(id!=0);CHECK(native_windows_submit_task(id));return id;
}
static void *stop_thread(void *production){
 if(production)native_task_service_stop();else injected_stop();
 pthread_mutex_lock(&test_lock);stop_done++;pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
 return NULL;
}
static pthread_t begin_stop(void *production){
 pthread_t thread;int rc=pthread_create(&thread,NULL,stop_thread,production);
 if(rc){fprintf(stderr,"FAIL creating stop thread: %s\n",strerror(rc));_Exit(1);}return thread;
}
static atomic_int lifecycle_errors;
static void *cycle_service(void *unused){
 (void)unused;
 for(int i=0;i<20;i++){if(!injected_start(stderr))atomic_fetch_add(&lifecycle_errors,1);injected_stop();}
 return NULL;
}
static void service_regressions(void){
 uint64_t ids[NATIVE_TASK_SERVICE_WORKERS*2];native_window_task_t request;
 pthread_t stopper;
 /* Failure before launch and at every partial-pool boundary leaves work intact. */
 reset_test();ids[0]=queue_task();fail_atexit=true;
 CHECK(!injected_start(stderr));CHECK(create_attempts==0);CHECK(view(ids[0]).task_pending);
 fail_atexit=false;
 for(int fail=0;fail<NATIVE_TASK_SERVICE_WORKERS;fail++){
  create_attempts=created=joined=0;fail_create=fail;
  CHECK(!injected_start(stderr));CHECK(create_attempts==fail+1);
  CHECK(created==fail && joined==fail);CHECK(executions==0 && callbacks==0);
  CHECK(view(ids[0]).task_pending);injected_stop();
 }
 fail_create=-1;CHECK(injected_start(stderr));await_count(&callbacks,1);injected_stop();
 CHECK(view(ids[0]).status==NATIVE_WINDOW_DONE);CHECK(created==joined);
 puts("  startup: atexit failure, every partial create failure, rollback and retry");

 /* The real linked service, not just the instrumented copy, overlaps workers. */
 reset_test();notify_hold=true;
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS*2;i++)ids[i]=queue_task();
 CHECK(native_task_service_start(NULL));await_count(&callbacks,NATIVE_TASK_SERVICE_WORKERS);
 for(int i=0;i<20;i++)CHECK(native_task_service_start(stdout));
 pthread_mutex_lock(&test_lock);
 CHECK(callbacks==NATIVE_TASK_SERVICE_WORKERS && notify_active==NATIVE_TASK_SERVICE_WORKERS);
 CHECK(bad_outputs==0);pthread_mutex_unlock(&test_lock);
 stopper=begin_stop((void*)1);
 /* A held notification must keep stop from returning. */
 struct timespec pause={0,20000000};nanosleep(&pause,NULL);
 pthread_mutex_lock(&test_lock);CHECK(stop_done==0);notify_hold=false;
 pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
 CHECK(pthread_join(stopper,NULL)==0);native_task_service_stop();
 puts("  linked service: bounded overlap, idempotent start/stop, output lifetime");

 /* Barrier inside execution proves parallel factories and stop without draining. */
 reset_test();execute_hold=true;
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS*2;i++)ids[i]=queue_task();
 CHECK(injected_start(NULL));await_count(&executions,NATIVE_TASK_SERVICE_WORKERS);
 for(int i=0;i<20;i++)CHECK(injected_start(stdout));
 CHECK(created==NATIVE_TASK_SERVICE_WORKERS);
 stopper=begin_stop(NULL);await_count(&wakes,1);
 pthread_mutex_lock(&test_lock);
 CHECK(stop_done==0 && active==NATIVE_TASK_SERVICE_WORKERS && peak==NATIVE_TASK_SERVICE_WORKERS);
 execute_hold=false;pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
 CHECK(pthread_join(stopper,NULL)==0);CHECK(created==joined);
 CHECK(executions==NATIVE_TASK_SERVICE_WORKERS && callbacks==NATIVE_TASK_SERVICE_WORKERS);
 CHECK(bad_outputs==0);
 int pending=0,done=0;
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS*2;i++){
  native_window_t w=view(ids[i]);pending+=w.task_pending;done+=w.status==NATIVE_WINDOW_DONE;CHECK(!w.task_running);
 }
 CHECK(pending==NATIVE_TASK_SERVICE_WORKERS && done==NATIVE_TASK_SERVICE_WORKERS);
 CHECK(injected_start(stderr));await_count(&callbacks,NATIVE_TASK_SERVICE_WORKERS*2);injected_stop();
 CHECK(executions==NATIVE_TASK_SERVICE_WORKERS*2 && peak==NATIVE_TASK_SERVICE_WORKERS);
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS*2;i++)CHECK(view(ids[i]).status==NATIVE_WINDOW_DONE);
 CHECK(!native_windows_take_task(&request));CHECK(created==joined);
 puts("  execution: bounded parallelism, in-flight join, queued work survives restart");

 /* Force stop's wake BEFORE any worker enters the non-sticky queue wait. */
 reset_test();wait_mode=1;wait_hold=true;
 CHECK(injected_start(stderr));await_count(&wait_entries,NATIVE_TASK_SERVICE_WORKERS);
 stopper=begin_stop(NULL);await_count(&wakes,1);
 pthread_mutex_lock(&test_lock);CHECK(stop_done==0);wait_hold=false;
 pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
 await_count(&stop_done,1);CHECK(pthread_join(stopper,NULL)==0);
 CHECK(timeouts==NATIVE_TASK_SERVICE_WORKERS && wait_calls==NATIVE_TASK_SERVICE_WORKERS);
 CHECK(created==joined);
 puts("  wake: stop-before-wait race terminates via timed fallback");

 /* A dequeue that races stop must still publish its captured result. */
 reset_test();wait_mode=2;wait_hold=true;ids[0]=queue_task();
 CHECK(injected_start(stderr));await_count(&wait_entries,1);
 stopper=begin_stop(NULL);await_count(&wakes,1);
 pthread_mutex_lock(&test_lock);CHECK(stop_done==0);wait_hold=false;
 pthread_cond_broadcast(&test_changed);pthread_mutex_unlock(&test_lock);
 CHECK(pthread_join(stopper,NULL)==0);
 CHECK(executions==1 && callbacks==1);CHECK(view(ids[0]).status==NATIVE_WINDOW_DONE);
 CHECK(!view(ids[0]).task_running && created==joined);
 puts("  stop/dequeue: captured request completes exactly once");

 reset_test();pthread_t callers[NATIVE_TASK_SERVICE_WORKERS];
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS;i++){
  int rc=pthread_create(&callers[i],NULL,cycle_service,NULL);
  if(rc){fprintf(stderr,"FAIL creating lifecycle caller: %s\n",strerror(rc));_Exit(1);}
 }
 for(int i=0;i<NATIVE_TASK_SERVICE_WORKERS;i++)CHECK(pthread_join(callers[i],NULL)==0);
 injected_stop();injected_stop();
 CHECK(atomic_load(&lifecycle_errors)==0);CHECK(created==joined);
 CHECK(executions==0 && callbacks==0);
 puts("  lifecycle: four concurrent callers, 80 start/stop cycles, all workers joined");
}
int main(void){
 alarm(30);
 native_factory_t f;native_factory_result_t r;
 native_windows_reset();CHECK(native_factory_lookup("sha256",&f));CHECK(native_factory_execute(&f,"abc",&r));CHECK(!strcmp(r.input_sha256,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
 CHECK(native_factory_lookup("json",&f));CHECK(native_factory_execute(&f,"{\"x\":[1,true]}",&r));CHECK(strstr(r.output,"\"root_type\":\"object\"")!=NULL);CHECK(!native_factory_execute(&f,"{bad",&r));CHECK(strstr(r.output,"byte_offset")!=NULL);
 char huge[NATIVE_FACTORY_INPUT_MAX+1];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;CHECK(!native_factory_execute(&f,huge,&r));
 command("{\"action\":\"define_factory\",\"factory\":\"audit\",\"title\":\"Payload audit\",\"executor\":\"json\"}",true);
 command("{\"action\":\"define_factory\",\"factory\":\"audit\",\"title\":\"Overwrite\",\"executor\":\"sha256\"}",false);
 command("{\"action\":\"define_factory\",\"factory\":\"shell\",\"title\":\"Denied\",\"executor\":\"bash\"}",false);
 uint64_t id=command("{\"action\":\"factory\",\"factory\":\"audit\",\"text\":\"{\\\"a\\\":1}\"}",true);CHECK(id!=0);native_window_t w=view(id);CHECK(w.kind==NATIVE_WINDOW_TASK && w.editor_active);
 CHECK(native_windows_key(18,0));CHECK(!native_windows_action_pending());
 for(int i=0;i<100;i++)CHECK(native_windows_key(18,0));
 native_window_task_t first,second;CHECK(native_windows_take_task(&first));CHECK(!native_windows_take_task(&second));CHECK(!strcmp(first.input,"{\"a\":1}"));
 char update[128];snprintf(update,sizeof(update),"{\"action\":\"update\",\"id\":%llu,\"text\":\"{\\\"a\\\":2}\"}",(unsigned long long)id);command(update,true);
 CHECK(native_factory_execute(&first.factory,first.input,&r));CHECK(!native_windows_complete_task(&first,&r));w=view(id);CHECK(!strcmp(w.text,"{\"a\":2}") && !w.task_output[0]);
 CHECK(native_windows_key(18,0));CHECK(native_windows_take_task(&second));command("{\"action\":\"hide\"}",true);
 CHECK(native_factory_execute(&second.factory,second.input,&r));CHECK(native_windows_complete_task(&second,&r));native_windows_snapshot_t *snap=malloc(sizeof(*snap));native_windows_snapshot(snap);CHECK(!snap->visible);CHECK(snap->local_task_coalesced==100);CHECK(snap->local_task_stale==1);free(snap);
 command("{\"action\":\"show\"}",true);w=view(id);CHECK(w.status==NATIVE_WINDOW_DONE && w.task_show_result && !w.editor_active);CHECK(!native_windows_action_pending());
 CHECK(native_windows_submit_task(id));CHECK(native_windows_take_task(&first));native_windows_reset();
 id=command("{\"action\":\"factory\",\"factory\":\"json\",\"text\":\"null\"}",true);CHECK(native_factory_execute(&first.factory,first.input,&r));CHECK(!native_windows_complete_task(&first,&r));w=view(id);CHECK(!strcmp(w.text,"null") && !w.task_output[0]);
 CHECK(native_windows_submit_task(id));CHECK(native_task_service_start(stderr));
 for(int i=0;i<1000;i++){w=view(id);if(w.status==NATIVE_WINDOW_DONE)break;struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
 w=view(id);CHECK(w.status==NATIVE_WINDOW_DONE);CHECK(!native_windows_action_pending());native_task_service_stop();CHECK(atomic_load(&notifications)>0);
 // Pointer Run uses a local job, never the conversation action queue.
 native_ui_rect_t button={w.rect.x+10,w.rect.y+w.rect.height-32,(w.rect.width-20)/4,24};CHECK(button.width>0);
 CHECK(native_windows_pointer(0,button.x+2,button.y+2,false));CHECK(native_windows_pointer(0,button.x+2,button.y+2,true));CHECK(!native_windows_action_pending());CHECK(native_windows_take_task(&first));
 // Closing a view cannot free or modify an already captured job input.
 char close[96];snprintf(close,sizeof(close),"{\"action\":\"close\",\"id\":%llu}",(unsigned long long)id);command(close,true);CHECK(!strcmp(first.input,"null"));CHECK(native_factory_execute(&first.factory,first.input,&r));CHECK(!native_windows_complete_task(&first,&r));
 service_regressions();
 native_windows_reset();printf("native_tasks: %d checks, %d failures; no model/provider calls\n",checks,failures);return failures?1:0;
}
