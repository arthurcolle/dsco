/* Template instantiated with exact production worker and collector sections. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "tools.h"
#include "tool_telemetry.h"
#include "capability.h"
#include "config.h"
volatile int g_interrupted;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static double cpu_ms(void) { struct rusage r; getrusage(RUSAGE_SELF,&r); return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0; }
static void pause_us(int us) { struct timespec t={.tv_sec=us/1000000,.tv_nsec=(us%1000000)*1000L};nanosleep(&t,NULL); }
@TYPES@
@HELPERS@
@WATCHDOG@
static const char *fixture, *mode;
static _Atomic int gate_calls, executed, collected;
static _Atomic bool auxiliary_stop;
static bool seen[CONCURRENT_TOOL_MAX];
static double work_done[CONCURRENT_TOOL_MAX], max_collection_tail;
static int fail_mod, creation_calls;
int tool_timeout_for(const char *name) { (void)name;return 30; }
static void dsco_strip_terminal_controls_inplace(char *s) { (void)s; }
bool tools_meta_is_read_only(const char *name,bool *found) { *found=true;return strcmp(name,"write_file")!=0; }
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *result,size_t capacity) {
    int index=0,delay=0;assert(sscanf(input,"{\"index\":%d,\"delay_us\":%d}",&index,&delay)==2);
    atomic_fetch_add(&gate_calls,1);
    if(dsco_capability_gate(name,input,tier,result,capacity)!=CAP_DECISION_ALLOW) {
        work_done[index]=now_ms();return false;
    }
    if(!strcmp(mode,"interrupt")) {
        while(!g_interrupted) pause_us(50);
        snprintf(result,capacity,"interrupted");work_done[index]=now_ms();return false;
    }
    if(!strcmp(mode,"timeout")) {
        tool_telemetry_timeout(TOOL_TIMEOUT_WALL);
        snprintf(result,capacity,"wall deadline");work_done[index]=now_ms();return false;
    }
    if(delay) pause_us(delay);
    int fd=open(fixture,O_RDONLY);assert(fd>=0);
    ssize_t n=read(fd,result,capacity-1);assert(n>0);result[n]='\0';assert(close(fd)==0);
    atomic_fetch_add(&executed,1);dsco_flow_note(dsco_caps_for_tool(name,input));
    work_done[index]=now_ms();return true;
}
@WORKER@
static int bench_thread_create(pthread_t *t,const pthread_attr_t *attr,void *(*fn)(void *),void *arg) {
    int call=creation_calls++;
    if(fail_mod && call%fail_mod==0) return EAGAIN;
    return pthread_create(t,attr,fn,arg);
}
#define pthread_create bench_thread_create
static void bench_collect(concurrent_tool_slot_t *slot,int index) {
    assert(!seen[index]);seen[index]=true;atomic_fetch_add(&collected,1);
    assert(slot->block_index==index && slot->batch_index==index);
    assert(slot->tool_id && atoi(slot->tool_id)==index);
    bool failure=!strcmp(mode,"denied")||!strcmp(mode,"interrupt")||!strcmp(mode,"timeout");
    assert(slot->ok==!failure);
    if(!failure) assert(!strncmp(slot->result,"local-file-read-proof:",22));
    if(!strcmp(mode,"denied")) assert(strstr(slot->result,"DSCO_ALLOW_WRITE=0"));
    if(!strcmp(mode,"interrupt")) assert(!strcmp(slot->result,"interrupted"));
    if(!strcmp(mode,"timeout")) assert(slot->was_timeout && slot->timeout_origin==TOOL_TIMEOUT_WALL && strstr(slot->result,"[timeout:"));
    double tail=(now_ms()-work_done[index])*1000.0;if(tail>max_collection_tail)max_collection_tail=tail;
}
static void *interrupt_later(void *unused) { (void)unused;pause_us(2000);g_interrupted=1;return NULL; }
#ifdef COMPLETION_CONDITION
static void *spurious_wakes(void *arg) {
    concurrent_tool_completion_t *completion=arg;
    while(!atomic_load(&auxiliary_stop)) {
        pthread_mutex_lock(&completion->mutex);pthread_cond_signal(&completion->ready);pthread_mutex_unlock(&completion->mutex);
        pause_us(50);
    }
    return NULL;
}
#endif
static void run_batch(int conc_count,int delay,double *total,double *cpu) {
    assert(conc_count<=CONCURRENT_TOOL_MAX);
    concurrent_tool_slot_t *conc_slots=calloc((size_t)conc_count,sizeof(*conc_slots));assert(conc_slots);
#ifdef COMPLETION_CONDITION
    concurrent_tool_completion_t conc_completion={.mutex=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};
#endif
    memset(seen,0,sizeof(seen));gate_calls=executed=collected=0;g_interrupted=0;creation_calls=0;auxiliary_stop=false;
    char inputs[CONCURRENT_TOOL_MAX][80], ids[CONCURRENT_TOOL_MAX][16];
    for(int ci=0;ci<conc_count;ci++) {
        snprintf(inputs[ci],sizeof(inputs[ci]),"{\"index\":%d,\"delay_us\":%d}",ci,delay+(ci%3)*(!strcmp(mode,"race")?37:0));
        snprintf(ids[ci],sizeof(ids[ci]),"%d",ci);
        concurrent_tool_slot_t *s=&conc_slots[ci];s->tool_name=!strcmp(mode,"denied")?"write_file":"read_file";
        s->tool_input=inputs[ci];s->tier="trusted";s->tool_id=ids[ci];s->block_index=ci;s->batch_index=ci;
        s->result=calloc(1,MAX_TOOL_RESULT);assert(s->result);
#ifdef COMPLETION_CONDITION
        s->completion=&conc_completion;
#endif
    }
    pthread_t auxiliary;bool have_auxiliary=false;
    if(!strcmp(mode,"interrupt")) { assert(bench_thread_create(&auxiliary,NULL,interrupt_later,NULL)==0);have_auxiliary=true; }
#ifdef COMPLETION_CONDITION
    if(!strcmp(mode,"spurious")) { assert(bench_thread_create(&auxiliary,NULL,spurious_wakes,&conc_completion)==0);have_auxiliary=true; }
#endif
    double start=now_ms(),cpu_start=cpu_ms();
    @LAUNCH@
    if(!strcmp(mode,"precompleted")) pause_us(10000);
    @COLLECTOR@
    *total=(now_ms()-start)*1000;*cpu=cpu_ms()-cpu_start;
    assert(atomic_load(&gate_calls)==conc_count && atomic_load(&collected)==conc_count);
    bool failure=!strcmp(mode,"denied")||!strcmp(mode,"interrupt")||!strcmp(mode,"timeout");
    assert(atomic_load(&executed)==(failure?0:conc_count));
    assert(s_wd_registry_count==0);
    if(have_auxiliary) { atomic_store(&auxiliary_stop,true);pthread_join(auxiliary,NULL); }
    free(conc_slots);
#ifdef COMPLETION_CONDITION
    assert(pthread_cond_destroy(&conc_completion.ready)==0);assert(pthread_mutex_destroy(&conc_completion.mutex)==0);
#endif
}
int main(int argc,char **argv) {
    assert(argc==5);fixture=argv[1];int count=atoi(argv[2]),delay=atoi(argv[3]);mode=argv[4];
    if(!strcmp(mode,"denied")||!strcmp(mode,"read_write_disabled"))setenv("DSCO_ALLOW_WRITE","0",1);
    fail_mod=!strcmp(mode,"thread_failure")?1:(!strcmp(mode,"mixed_failure")?2:0);
    if(!strcmp(mode,"spurious"))delay=4000;
    int loops=!strcmp(mode,"race")?200:1;
    double total=0,cpu=0;
    for(int i=0;i<loops;i++)run_batch(count,delay,&total,&cpu);
    printf("{\"passed\":true,\"total_ms\":%.6f,\"collection_tail_ms\":%.6f,\"cpu_ms\":%.6f,\"batches\":%d,\"gate_calls_per_batch\":%d,\"executed_per_batch\":%d}\n",total,max_collection_tail,cpu,loops,atomic_load(&gate_calls),atomic_load(&executed));
}
