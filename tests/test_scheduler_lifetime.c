#include "scheduler.h"
#include "ooda.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

static int done(void *ctx) { if(ctx) ++*(int *)ctx; return 0; }
static int yielding(void *ctx) { (void)ctx; return 1; }
static ooda_engine_t engine;
typedef struct { scheduler_t *s; task_id_t child; int consumed; } observer_t;
static int consume(void *ctx) {
    observer_t *o=ctx; sched_task_t *child=sched_task_get(o->s,o->child);
    assert(child && child->state==TASK_COMPLETED && child->exit_code==0);
    o->consumed++; return 0;
}
static int self_cancel(void *ctx) {
    scheduler_t *s=ctx; assert(sched_cancel(s,s->current));
    assert(!sched_release(s,s->current) && errno==EBUSY);
    sched_sleep(s,100000); sched_wait_fd(s,0); sched_wait_task(s,s->current);
    return 0;
}
static int stopped(void *ctx) { sched_stop(ctx); return 1; }
static int blocked(void *ctx) { sched_wait_fd(ctx,0); return 1; }
static int cycle(void *ctx) { scheduler_t *s=ctx; sched_wait_task(s,s->current);return 1; }
int main(void) {
    alarm(30); /* fixture bound, never production execution policy */
    scheduler_t s; sched_init(&s); int calls=0;
    for(int i=0;i<100000;i++) {
        assert(sched_run_sync(&s,done,&calls,"owned sync")==0);
        assert(sched_get_stats(&s).tasks_total==0);
    }
    assert(calls==100000 && s.task_count==1);
    task_id_t stale=0;
    assert(!sched_task_get(&s,stale) && !sched_cancel(&s,stale));
    task_id_t ids[SCHED_MAX_TASKS];
    for(int i=0;i<SCHED_MAX_TASKS;i++) ids[i]=sched_spawn(&s,done,NULL,"retained",SCHED_PRIO_NORMAL);
    assert(sched_run(&s,0)==0);
    assert(sched_spawn(&s,done,NULL,"full",SCHED_PRIO_NORMAL)==TASK_INVALID && errno==EAGAIN);
    assert(sched_release(&s,ids[0]));
    task_id_t reused=sched_spawn(&s,done,NULL,"reused",SCHED_PRIO_NORMAL);
    assert(reused>ids[SCHED_MAX_TASKS-1] && !sched_task_get(&s,ids[0]));
    assert(!sched_release(&s,reused) && errno==EBUSY);
    assert(sched_run_until(&s,&reused,1)==0 && sched_release(&s,reused));
    for(int i=1;i<SCHED_MAX_TASKS;i++) assert(sched_release(&s,ids[i]));
    for(int i=0;i<100000;i++) {
        task_id_t id=sched_spawn(&s,yielding,NULL,"cancel",SCHED_PRIO_HIGH);
        assert(id!=TASK_INVALID && sched_cancel(&s,id) && sched_release(&s,id));
        assert(s.run_queue_len[SCHED_PRIO_HIGH]==0);
    }
    assert(sched_spawn(&s,done,NULL,"bad",SCHED_PRIO_COUNT)==TASK_INVALID && errno==EINVAL);
    sched_destroy(&s); sched_init(&s);
    observer_t o={.s=&s};
    o.child=sched_spawn(&s,done,NULL,"source",SCHED_PRIO_HIGH);
    task_id_t observer=sched_spawn(&s,consume,&o,"observer",SCHED_PRIO_NORMAL);
    sched_task_t *t=sched_task_get(&s,observer);t->state=TASK_WAITING_TASK;t->wait_task=o.child;
    sched_tick(&s);assert(sched_release(&s,o.child));
    assert(sched_task_get(&s,o.child)); /* outcome pinned through delivery */
    assert(sched_run_until(&s,&observer,1)==0 && o.consumed==1);
    assert(!sched_task_get(&s,o.child));assert(sched_release(&s,observer));
    assert(sched_run_sync(&s,self_cancel,&s,"sticky cancel")==-2);
    assert(sched_run_sync(&s,stopped,&s,"stop")==-1 && errno==ECANCELED);
    assert(sched_run_sync(&s,cycle,&s,"deadlock")==-1 && errno==EDEADLK);
    assert(sched_get_stats(&s).tasks_total==0);
    task_id_t unrelated=sched_spawn(&s,blocked,&s,"unrelated wait",SCHED_PRIO_HIGH);
    task_id_t target=sched_spawn(&s,done,NULL,"target",SCHED_PRIO_NORMAL);
    assert(sched_run_until(&s,&target,1)==0);
    assert(sched_task_get(&s,unrelated)->state==TASK_WAITING_IO);
    assert(sched_release(&s,target));assert(sched_cancel(&s,unrelated));assert(sched_release(&s,unrelated));
    assert(sched_run_until(&s,&stale,1)==-1 && errno==ENOENT);
    s.next_id=INT64_MAX-1;
    task_id_t last=sched_spawn(&s,done,NULL,"last ID",SCHED_PRIO_NORMAL);
    assert(last==INT64_MAX-1 && sched_run_until(&s,&last,1)==0 && sched_release(&s,last));
    assert(sched_spawn(&s,done,NULL,"exhausted",SCHED_PRIO_NORMAL)==TASK_INVALID && errno==EOVERFLOW);
    sched_destroy(&s); sched_init(&s); ooda_engine_init(&engine);
    for(int i=0;i<10000;i++) {
        assert(ooda_begin(&engine)>=0);
        assert(ooda_observe(&engine,"owned local observation","fixture",0.9));
        assert(ooda_run_scheduled(&engine.current,&s)>=0);
        assert(engine.current.phase==OODA_PHASE_COMPLETE && sched_get_stats(&s).tasks_total==0);
    }
    assert(s.total_dispatches==40000 && s.task_count==4);
    for(int i=0;i<SCHED_MAX_TASKS-3;i++) ids[i]=sched_spawn(&s,yielding,NULL,"occupied",SCHED_PRIO_NORMAL);
    ooda_begin(&engine);
    assert(ooda_run_scheduled(&engine.current,&s)==-1);
    assert(sched_get_stats(&s).tasks_total==SCHED_MAX_TASKS-3);
    for(int i=0;i<SCHED_MAX_TASKS-3;i++){assert(sched_cancel(&s,ids[i]));assert(sched_release(&s,ids[i]));}
    assert(sched_get_stats(&s).tasks_total==0);
    sched_destroy(&s);
    puts("scheduler ownership: 100000 sync + 100000 cancel/release + 10000 actual OODA cycles; capacity, stale identity, await pins, cancellation, stop, deadlock, partial admission passed");
    return 0;
}
