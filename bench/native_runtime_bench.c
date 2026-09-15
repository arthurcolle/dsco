/* Native control-plane benchmark: production IPC, scheduler, and event loop.
 * Work items are deliberate no-op payloads; no provider/inference simulation. */
#include "ipc.h"
#include "scheduler.h"
#include "event_loop.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int cmp(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y);
}
static void metrics(const char *name,double *samples,int n,double total) {
    qsort(samples,n,sizeof(*samples),cmp);
    printf("\"%s\":{\"count\":%d,\"total_ms\":%.6f,\"ops_per_second\":%.3f,\"p50_ms\":%.6f,\"p95_ms\":%.6f}",
           name,n,total,n*1000.0/total,samples[n/2],samples[(n-1)*95/100]);
}
static int increment(void *ctx) { ++*(int *)ctx; return 0; }
static void callback(void *ctx) { ++*(int *)ctx; }

int main(int argc,char **argv) {
    assert(argc>=3);
    const char *mode=argv[1],*db=argv[2];
    int n=argc>3?atoi(argv[3]):1000; assert(n>0 && n<=100000);
    double start=ms();
    if (!strcmp(mode,"scheduler")) {
        double samples[100]; int completed=0;
        for (int batch=0;batch<100;batch++) {
            scheduler_t scheduler; sched_init(&scheduler); double begin=ms();
            for(int i=0;i<256;i++) assert(sched_spawn(&scheduler,increment,&completed,"native no-op",i%SCHED_PRIO_COUNT)>=0);
            while(sched_tick(&scheduler)>0) {}
            samples[batch]=ms()-begin; sched_destroy(&scheduler);
        }
        double total=ms()-start; assert(completed==25600);
        scheduler_t scheduler; sched_init(&scheduler); int cancelled_calls=0;
        task_id_t id=sched_spawn(&scheduler,increment,&cancelled_calls,"cancelled",SCHED_PRIO_NORMAL);
        int survivor_calls=0;
        assert(sched_spawn(&scheduler,increment,&survivor_calls,"survivor",SCHED_PRIO_LOW)>=0);
        assert(sched_cancel(&scheduler,id)); while(sched_tick(&scheduler)>0) {}
        assert(cancelled_calls==0 && survivor_calls==1);
        assert(sched_task_get(&scheduler,id)->state==TASK_CANCELLED);
        printf("{\"completed\":%d,\"dispatches_per_second\":%.3f,\"cancelled_task_executions\":%d,",completed,completed*1000.0/total,cancelled_calls);
        metrics("batch_256",samples,100,total);
        ev_loop_t *loop=ev_loop_new(); assert(loop); int callbacks=0; start=ms();
        for(int batch=0;batch<100;batch++) {
            for(int i=0;i<64;i++) assert(ev_defer(loop,callback,&callbacks)>=0);
            ev_loop_poll(loop,0);
        }
        assert(callbacks==6400); total=ms()-start;
        printf(",\"deferred_callbacks\":6400,\"callbacks_per_second\":%.3f}\n",6400000.0/total);
        ev_loop_free(loop); return 0;
    }
    char operator_id[64]; snprintf(operator_id,sizeof(operator_id),"operator-%d",getpid());
    const char *identity=(!strcmp(mode,"hold")||!strcmp(mode,"recover"))?"survivor":operator_id;
    assert(ipc_init(db,identity)); double init_ms=ms()-start;
    if (!strcmp(mode,"hot")) {
        assert(ipc_register(NULL,0,"benchmark","local"));
        double *samples=calloc(n,sizeof(double)); assert(samples); start=ms();
        for(int i=0;i<n;i++) {
            double begin=ms(); int id=ipc_task_submit("native no-op",i%5,0); assert(id>0);
            ipc_task_t task; assert(ipc_task_claim(&task)&&task.id==id);
            assert(ipc_task_start(id)); assert(ipc_task_complete(id,"done"));
            samples[i]=ms()-begin;
        }
        printf("{\"init_ms\":%.6f,",init_ms); metrics("task_lifecycle",samples,n,ms()-start);
        start=ms();
        for(int i=0;i<n;i++) {
            double begin=ms(); assert(ipc_send(identity,"steer","continue original objective"));
            ipc_message_t message; assert(ipc_recv(&message,1)==1); free(message.body);
            samples[i]=ms()-begin;
        }
        printf(","); metrics("message_send_receive_ack",samples,n,ms()-start); puts("}"); free(samples);
    } else if(!strcmp(mode,"two-queue")) {
        assert(ipc_agent_define("planner",NULL,0,"planner","","local"));
        assert(ipc_agent_define("executor",NULL,0,"executor","","local")); start=ms();
        for(int i=0;i<n;i++) assert(ipc_task_submit_to("planner","produce execution item",i%5,0)>0);
        ipc_shutdown(); assert(ipc_init(db,"planner"));
        for(int i=0;i<n;i++) {
            ipc_task_t task; assert(ipc_task_claim_targeted(&task));
            assert(ipc_task_start(task.id));
            assert(ipc_task_submit_to("executor","execute planned item",task.priority,task.id)>0);
            assert(ipc_task_complete(task.id,"plan delivered"));
        }
        ipc_shutdown(); assert(ipc_init(db,"executor"));
        for(int i=0;i<n;i++) {
            ipc_task_t task; assert(ipc_task_claim_targeted(&task)); assert(task.parent_task_id>0);
            assert(ipc_task_start(task.id)); assert(ipc_task_complete(task.id,"execution done"));
        }
        double total=ms()-start;
        printf("{\"planning_items\":%d,\"execution_items\":%d,\"total_ms\":%.6f,\"pipelines_per_second\":%.3f}\n",n,n,total,n*1000.0/total);
    } else if(!strcmp(mode,"setup")) {
        assert(ipc_agent_define("survivor",NULL,0,"durable benchmark","","local"));
        int id=ipc_task_submit_to("survivor","survive interruption",10,0); assert(id>0);
        printf("{\"task_id\":%d}\n",id);
    } else if(!strcmp(mode,"hold")) {
        assert(ipc_agent_activate()); ipc_task_t task; assert(ipc_task_claim_targeted(&task));
        assert(ipc_task_start(task.id));
        assert(ipc_checkpoint_save(identity,1,"{\"step\":1}","[]","{\"objective\":\"finish\"}","{\"phase\":\"claimed\"}"));
        printf("{\"pid\":%d,\"claimed_task\":%d,\"checkpoint\":1}\n",getpid(),task.id); fflush(stdout);
        for(;;) pause();
    } else if(!strcmp(mode,"steer")) {
        assert(ipc_send("survivor","interrupt","resume from checkpoint and finish"));
        puts("{\"message_persisted\":true}");
    } else if(!strcmp(mode,"recover")) {
        /* The driver has proved process death before expiring this lease. */
        start=ms(); int requeued=ipc_task_requeue_stale(0); assert(requeued==1);
        char *memory=NULL,*plan=NULL;
        assert(ipc_checkpoint_restore(identity,-1,&memory,NULL,&plan,NULL));
        assert(!strcmp(memory,"{\"step\":1}")&&!strcmp(plan,"{\"objective\":\"finish\"}")); free(memory); free(plan);
        ipc_message_t message; assert(ipc_recv(&message,1)==1); assert(!strcmp(message.topic,"interrupt")); free(message.body);
        ipc_task_t task; assert(ipc_task_claim_targeted(&task)); assert(!strcmp(task.target_agent_id,identity));
        assert(ipc_task_start(task.id)); assert(ipc_task_complete(task.id,"resumed and completed"));
        printf("{\"requeued\":%d,\"checkpoint_restored\":true,\"message_survived\":true,\"completed_task\":%d,\"recovery_ms\":%.6f,\"init_ms\":%.6f}\n",requeued,task.id,ms()-start,init_ms);
    } else { fprintf(stderr,"unknown mode\n"); return 2; }
    ipc_shutdown(); return 0;
}
