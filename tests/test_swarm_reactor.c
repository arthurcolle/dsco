#include "swarm_reactor.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long long)t.tv_sec*1000+t.tv_nsec/1000000; }
static void signal_noop(int sig) { (void)sig; }
int main(void) {
    long long start=ms();
    assert(swarm_reactor_wait(NULL,0,0)==0);
    assert(ms()-start<100);
    start=ms(); assert(swarm_reactor_wait(NULL,0,-1)==0);
    assert(ms()-start>=80 && ms()-start<500);
    int fds[2]; assert(pipe(fds)==0);
    struct pollfd p={.fd=fds[0],.events=POLLIN};
    assert(write(fds[1],"x",1)==1);
    start=ms(); assert(swarm_reactor_wait(&p,1,10000)==1);
    assert(p.revents&POLLIN); assert(ms()-start<100);
    char c; assert(read(fds[0],&c,1)==1); close(fds[1]);
    assert(swarm_reactor_wait(&p,1,100)==1); assert(p.revents&POLLHUP); close(fds[0]);
    struct sigaction sa={0}; sa.sa_handler=signal_noop; sigaction(SIGUSR1,&sa,NULL);
    pid_t child=fork(); assert(child>=0);
    if(child==0) { struct timespec t={0,10000000}; for(int i=0;i<8;i++){nanosleep(&t,NULL);kill(getppid(),SIGUSR1);} _exit(0); }
    start=ms(); assert(swarm_reactor_wait(NULL,0,100)==0);
    assert(ms()-start>=80 && ms()-start<300);
    assert(waitpid(child,NULL,0)==child);
    puts("PASS reactor: zero timeout, bounded idle, readiness, EOF, interrupted deadline");
}
