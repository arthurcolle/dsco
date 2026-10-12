#include "scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static int calls;
static int once(void *p){(void)p; calls++; return 0;}
static int yield(void *p){int *n=p; return --*n>0;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){scheduler_t s;double start=now();for(int j=0;j<10000;j++){sched_init(&s);for(int i=0;i<256;i++)assert(sched_spawn(&s,once,0,"bench",i%5)==i);while(sched_tick(&s)>0){}assert(s.total_dispatches==256);}printf("2560000 dispatches: %.6f seconds\n",now()-start);assert(calls==2560000);sched_init(&s);int n=500;int id=sched_spawn(&s,yield,&n,"yield",SCHED_PRIO_HIGH);int c=sched_spawn(&s,once,0,"cancel",SCHED_PRIO_CRITICAL);assert(sched_cancel(&s,c));assert(sched_task_get(&s,-1)==0);assert(sched_task_get(&s,256)==0);while(sched_tick(&s)>0){}assert(n==0);assert(sched_task_get(&s,id)->ticks==500);assert(sched_task_get(&s,c)->state==TASK_CANCELLED);assert(calls==2560000);sched_init(&s);int low=sched_spawn(&s,once,0,"low",SCHED_PRIO_LOW);int high=sched_spawn(&s,once,0,"high",SCHED_PRIO_HIGH);sched_tick(&s);assert(sched_task_get(&s,high)->state==TASK_COMPLETED);assert(sched_task_get(&s,low)->state==TASK_READY);puts("PASS lookup, priority, cancellation, yielding, capacity workloads");}
