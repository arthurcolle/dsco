#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "cancel_wait.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#define N 32
static volatile int global_flag;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static int entered;
typedef struct {volatile int local;int fd,rc,error;} waiter_t;
static void *waiter(void *ctx){waiter_t *w=ctx;cancel_wait_t *wait=cancel_wait_open(&w->local,&global_flag);assert(wait);
pthread_mutex_lock(&gate);entered++;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&gate);
w->rc=cancel_wait_poll(wait,w->fd,POLLIN,-1);w->error=errno;cancel_wait_close(wait);return NULL;}
static atomic_bool stop;
static void *late_signal(void *ctx){volatile int *flag=ctx;while(!atomic_load(&stop))assert(!cancel_wait_signal(flag));return NULL;}
static int fd_count(void){int n=0;for(int i=0;i<1024;i++)if(fcntl(i,F_GETFD)>=0)++n;return n;}
int main(void){alarm(20);int initial=fd_count();volatile int flag=0;assert(!cancel_wait_signal(&flag));cancel_wait_t *wait=cancel_wait_open(&flag,NULL);assert(wait);assert(cancel_wait_poll(wait,-1,0,-1)==-1&&errno==ECANCELED);cancel_wait_close(wait);
flag=0;wait=cancel_wait_open(&flag,NULL);assert(wait);assert(!cancel_wait_signal(&flag));for(int i=0;i<100000;i++)assert(!cancel_wait_signal(&flag));assert(cancel_wait_poll(wait,-1,0,-1)==-1&&errno==ECANCELED);cancel_wait_close(wait);
waiter_t w[N]={0};pthread_t threads[N];int pipes[N][2];
for(int i=0;i<N;i++){assert(!pipe(pipes[i]));w[i].fd=pipes[i][0];assert(!pthread_create(&threads[i],NULL,waiter,&w[i]));}
pthread_mutex_lock(&gate);while(entered<N)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);
assert(!cancel_wait_signal(&w[0].local));assert(!pthread_join(threads[0],NULL));assert(w[0].rc==-1&&w[0].error==ECANCELED);
for(int i=1;i<N;i++){assert(!__atomic_load_n(&w[i].local,__ATOMIC_ACQUIRE));assert(write(pipes[i][1],"x",1)==1);assert(!pthread_join(threads[i],NULL));assert(w[i].rc==1);}
for(int i=0;i<N;i++){close(pipes[i][0]);close(pipes[i][1]);}
entered=0;
for(int i=0;i<N;i++){w[i]=(waiter_t){.fd=-1};assert(!pthread_create(&threads[i],NULL,waiter,&w[i]));}
pthread_mutex_lock(&gate);while(entered<N)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);
assert(!cancel_wait_signal(&global_flag));for(int i=0;i<N;i++){assert(!pthread_join(threads[i],NULL));assert(w[i].rc==-1&&w[i].error==ECANCELED);}
pthread_t producer;assert(!pthread_create(&producer,NULL,late_signal,(void *)&flag));for(int i=0;i<10000;i++){wait=cancel_wait_open(&flag,NULL);assert(wait);cancel_wait_close(wait);}atomic_store(&stop,true);assert(!pthread_join(producer,NULL));
flag=0;wait=cancel_wait_open(&flag,NULL);assert(wait);assert(cancel_wait_poll(wait,-1,0,20)==0);assert(cancel_wait_poll(wait,-1,0,0)==0);
int data[2];assert(!pipe(data));assert(write(data[1],"x",1)==1);assert(!cancel_wait_signal(&flag));assert(cancel_wait_poll(wait,data[0],POLLIN,0)==-1&&errno==ECANCELED);close(data[0]);close(data[1]);cancel_wait_close(wait);assert(fd_count()==initial);
puts("PASS cancellation wait: preposted and registration races, 32 targeted/broadcast waiters, 100000 coalesced signals, 10000 close/reuse races, deadline, cancellation precedence and no fd leak");return 0;}
