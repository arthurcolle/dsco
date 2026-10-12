#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "async_runtime.h"
#include "async_join.h"
#include "scheduler_async.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define N 32
static double ms(clockid_t c){struct timespec t;assert(!clock_gettime(c,&t));return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void delay(int duration){struct timespec t={duration/1000,duration%1000*1000000L};while(nanosleep(&t,&t))assert(errno==EINTR);}
static int fd_count(void){int n=0;for(int i=0;i<1024;i++)if(fcntl(i,F_GETFD)>=0)++n;return n;}
typedef struct {async_runtime_t *r;async_handle_t h[N];int duration,control;bool cancel;} batch_t;
static void *publish(void *ctx){batch_t *b=ctx;
if(b->cancel){delay(b->duration/2);assert(!async_request_cancel(b->r,b->h[0]));assert(write(b->control,"c",1)==1);delay(b->duration-b->duration/2);}
else delay(b->duration);
for(int i=0;i<N;i++){async_result_t result={b->cancel&&i==0?ASYNC_CANCELLED:ASYNC_SUCCESS,b->cancel&&i==0?ECANCELED:0,(uint64_t)i};assert(!async_publish(b->r,b->h[i],result));assert(!async_release(b->r,b->h[i]));}return NULL;}
typedef struct {batch_t *b;int calls,cancel_seen;pthread_t owner;} observed_t;
static void complete(async_runtime_t *r,async_handle_t h,const async_result_t *v,void *ctx){(void)r;(void)h;observed_t *o=ctx;assert(pthread_equal(pthread_self(),o->owner));assert(v->value<N);o->calls++;}
static void cancelled(int fd,ev_mask_t events,void *ctx){assert(events&EV_READ);char c;assert(read(fd,&c,1)==1);observed_t *o=ctx;async_result_t v;assert(async_cancel_requested(o->b->r,o->b->h[0])==1);assert(async_read(o->b->r,o->b->h[0],&v)==0);o->cancel_seen++;ev_unwatch_fd(async_runtime_loop(o->b->r),fd);}
static void flush(async_runtime_t *r){for(;;){async_stats_t s=async_runtime_stats(r);if(!s.mailbox_depth&&!s.ready_depth)break;assert(async_step(r,0)>=0);}}
typedef struct {sched_async_t a;scheduler_t *s;int fd,duration,done;} task_t;
static int io_task(void *ctx){task_t *t=ctx;SCHED_ASYNC_BEGIN(&t->a);SCHED_AWAIT_FD(&t->a,t->s,t->fd,EV_READ);assert(sched_current_events(t->s)&EV_READ);t->done++;SCHED_ASYNC_END(&t->a);}
static int timer_task(void *ctx){task_t *t=ctx;SCHED_ASYNC_BEGIN(&t->a);SCHED_AWAIT_SLEEP(&t->a,t->s,t->duration);t->done++;SCHED_ASYNC_END(&t->a);}
static void *write_later(void *ctx){task_t *t=ctx;delay(t->duration);assert(write(t->fd,"x",1)==1);return NULL;}
int main(int argc,char **argv){assert(argc>2);setvbuf(stdout,NULL,_IOLBF,0);const char *lane=argv[1];int initial=fd_count();
async_runtime_t *r=NULL;scheduler_t s;
if(!strcmp(lane,"async")){r=async_runtime_new(N+1,2*N);assert(r);}else sched_init(&s);
async_handle_t stale=0;
for(int stage=2;stage<argc;stage++){int duration=atoi(argv[stage]);assert(duration>0 && duration<=3600000);
int before=fd_count();double elapsed,used;uint64_t waits=0,wakes=0,ticks=0;
if(r){int pipefd[2];assert(!pipe(pipefd));batch_t b={.r=r,.duration=duration,.control=pipefd[1],.cancel=(stage%2)==1};observed_t o={.b=&b,.owner=pthread_self()};
for(int i=0;i<N;i++){b.h[i]=async_cell_new(r);assert(b.h[i]&&b.h[i]!=stale);assert(!async_retain(r,b.h[i]));assert(async_subscribe(r,b.h[i],complete,&o));}
if(stale){async_result_t v;assert(async_read(r,stale,&v)==-1 && errno==ESTALE);}
async_join_t *j=async_join_new(r,b.h,N,N);assert(j);
if(b.cancel)assert(!ev_watch_fd(async_runtime_loop(r),pipefd[0],EV_READ,cancelled,&o));
ev_stats_t old=ev_loop_stats(async_runtime_loop(r));pthread_t producer;assert(!pthread_create(&producer,NULL,publish,&b));double start=ms(CLOCK_MONOTONIC),cpu=ms(CLOCK_THREAD_CPUTIME_ID);async_result_t result;
assert(!async_wait(r,async_join_output(j),&result));elapsed=ms(CLOCK_MONOTONIC)-start;used=ms(CLOCK_THREAD_CPUTIME_ID)-cpu;assert(!pthread_join(producer,NULL));flush(r);
assert(o.calls==N && o.cancel_seen==(b.cancel?1:0));assert(result.outcome==(b.cancel?ASYNC_FAILURE:ASYNC_SUCCESS));
ev_stats_t now=ev_loop_stats(async_runtime_loop(r));waits=now.polls-old.polls;wakes=now.wake_events-old.wake_events;
assert(waits<=N+4);async_join_free(j);stale=b.h[0];for(int i=0;i<N;i++)assert(!async_release(r,b.h[i]));flush(r);close(pipefd[0]);close(pipefd[1]);async_stats_t st=async_runtime_stats(r);assert(!st.cells_live&&!st.edges_live&&!st.mailbox_depth&&!st.ready_depth);
}else{int pipefd[2];assert(!pipe(pipefd));assert(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)==0);task_t tasks[N];task_id_t ids[N];pthread_t producer;task_t writer={.duration=duration,.fd=pipefd[1]};bool timer=!strcmp(lane,"timer");int count=timer?1:N;
uint64_t old=s.tick_count;
for(int i=0;i<count;i++){tasks[i]=(task_t){.s=&s,.fd=pipefd[0],.duration=duration};ids[i]=sched_spawn(&s,timer?timer_task:io_task,&tasks[i],lane,SCHED_PRIO_NORMAL);assert(ids[i]!=TASK_INVALID);}
if(!timer)assert(!pthread_create(&producer,NULL,write_later,&writer));double start=ms(CLOCK_MONOTONIC),cpu=ms(CLOCK_THREAD_CPUTIME_ID);
assert(!sched_run_until(&s,ids,(size_t)count));elapsed=ms(CLOCK_MONOTONIC)-start;used=ms(CLOCK_THREAD_CPUTIME_ID)-cpu;if(!timer)assert(!pthread_join(producer,NULL));
ticks=s.tick_count-old;assert(ticks==(uint64_t)count*2);for(int i=0;i<count;i++){assert(tasks[i].done==1);assert(sched_release(&s,ids[i]));}assert(!sched_get_stats(&s).tasks_total);close(pipefd[0]);close(pipefd[1]);}
assert(elapsed>=duration-10 && fd_count()==before);assert(used<100+duration*0.001);
printf("{\"lane\":\"%s\",\"duration_ms\":%d,\"elapsed_ms\":%.3f,\"owner_cpu_ms\":%.3f,\"kernel_waits\":%llu,\"wake_events\":%llu,\"scheduler_ticks\":%llu,\"fd_count\":%d,\"passed\":true}\n",lane,duration,elapsed,used,(unsigned long long)waits,(unsigned long long)wakes,(unsigned long long)ticks,before);}
if(r)assert(!async_runtime_free(r));else sched_destroy(&s);assert(fd_count()==initial);return 0;}
