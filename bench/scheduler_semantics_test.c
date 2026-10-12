#include "scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
typedef struct { scheduler_t *s; int calls; } ctx_t;
static int done(void *p){ (void)p; return 0; }
static int fail(void *p){ (void)p; return -7; }
static int sleeper(void *p){ctx_t*c=p;if(!c->calls++){sched_sleep(c->s,5);return 1;}return 0;}
static int slow(void*p){(void)p;struct timespec t={0,3000000};nanosleep(&t,0);return 0;}
static int waiter(void*p){ctx_t*c=p;if(!c->calls++){sched_wait_task(c->s,0);return 1;}return 0;}
int main(void){scheduler_t s;sched_init(&s);int a=sched_spawn(&s,done,0,"done",SCHED_PRIO_LOW);ctx_t c={&s,0};int b=sched_spawn(&s,waiter,&c,"wait",SCHED_PRIO_HIGH);assert(sched_tick(&s)==2);assert(s.tasks[b].state==TASK_WAITING_TASK);assert(sched_tick(&s)==1);assert(sched_tick(&s)==0);assert(c.calls==2);assert(s.tasks[a].state==TASK_COMPLETED);
sched_init(&s);c.calls=0;a=sched_spawn(&s,sleeper,&c,"timer",SCHED_PRIO_HIGH);assert(sched_tick(&s)==1);assert(s.tasks[a].state==TASK_WAITING_TIMER);assert(sched_run(&s,1)==0);assert(c.calls==2);assert(s.tasks[a].completed_ms>=s.tasks[a].wake_time_ms);
sched_init(&s);a=sched_spawn(&s,slow,0,"clock",SCHED_PRIO_NORMAL);assert(sched_tick(&s)==0);assert(s.tasks[a].cpu_ms>=2);assert(s.tasks[a].completed_ms>=s.tasks[a].started_ms+2);
sched_init(&s);a=sched_spawn(&s,fail,0,"fail",SCHED_PRIO_NORMAL);assert(sched_run(&s,1)==0);assert(s.tasks[a].state==TASK_FAILED);assert(s.tasks[a].exit_code==-7);
sched_init(&s);for(int i=0;i<SCHED_MAX_TASKS;i++)assert(sched_spawn(&s,done,0,"full",SCHED_PRIO_NORMAL)==i);assert(sched_spawn(&s,done,0,"overflow",SCHED_PRIO_NORMAL)==TASK_INVALID);while(sched_tick(&s)){}assert(s.total_dispatches==SCHED_MAX_TASKS);
/* OODA directly sets wait states through the public task handles. */
sched_init(&s);a=sched_spawn(&s,done,0,"parent",SCHED_PRIO_NORMAL);b=sched_spawn(&s,done,0,"child",SCHED_PRIO_HIGH);s.tasks[b].state=TASK_WAITING_TASK;s.tasks[b].wait_task=a;assert(sched_run(&s,1)==0);assert(s.total_dispatches==2);puts("PASS dependencies, public wait states, timers, timing, failures, capacity");}
