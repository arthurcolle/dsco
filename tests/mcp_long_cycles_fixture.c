#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include <poll.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
static _Thread_local unsigned waits;
int measured_poll(struct pollfd *p,nfds_t n,int timeout) {++waits;return poll(p,n,timeout);}
#define poll measured_poll
#include "../src/mcp.c"
#undef poll
_Thread_local volatile int tl_tool_cancelled;
int dsco_env_int(const char *n,int d,int min,int max){(void)n;(void)min;(void)max;return d;}
static double clock_ms(clockid_t c){struct timespec t;assert(!clock_gettime(c,&t));return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void delay(int ms){struct timespec t={ms/1000,ms%1000*1000000L};while(nanosleep(&t,&t))assert(errno==EINTR);}
static int fd_count(void){int n=0;for(int i=0;i<1024;i++)if(fcntl(i,F_GETFD)>=0)++n;return n;}
typedef struct {int fd,delay_ms;} peer_t;
static void *reply(void *ctx){peer_t *p=ctx;delay(p->delay_ms);const char *b="{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":\"stale\"}\n{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":\"owned-long-cycle\"}\n";assert(write(p->fd,b,strlen(b))==(ssize_t)strlen(b));return NULL;}
int main(int argc,char **argv){assert(argc>1);setvbuf(stdout,NULL,_IOLBF,0);int initial=fd_count();
for(int i=1;i<argc;i++){int duration=atoi(argv[i]);assert(duration>0 && duration<=3600000);int a[2],b[2];assert(!pipe(a)&&!pipe(b));
mcp_server_t srv={.stdin_fd=a[1],.stdout_fd=b[0],.stdio_stream=mcp_stream_new()};assert(srv.stdio_stream);assert(fcntl(b[0],F_SETFL,O_NONBLOCK)==0);
peer_t p={b[1],duration};pthread_t thread;assert(!pthread_create(&thread,NULL,reply,&p));
waits=0;double start=clock_ms(CLOCK_MONOTONIC),cpu=clock_ms(CLOCK_THREAD_CPUTIME_ID);
char *r=send_rpc_request(&srv,"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\"}\n",duration+5000);
assert(r&&strstr(r,"owned-long-cycle")&&!strstr(r,"stale"));double elapsed=clock_ms(CLOCK_MONOTONIC)-start,used=clock_ms(CLOCK_THREAD_CPUTIME_ID)-cpu;free(r);assert(!pthread_join(thread,NULL));
assert(elapsed>=duration-10);unsigned count=waits;
close(a[0]);close(a[1]);close(b[0]);close(b[1]);mcp_stream_free(srv.stdio_stream);assert(fd_count()==initial);
printf("{\"lane\":\"mcp-stdio\",\"duration_ms\":%d,\"elapsed_ms\":%.3f,\"owner_cpu_ms\":%.3f,\"kernel_waits\":%u,\"fd_count\":%d,\"passed\":true}\n",duration,elapsed,used,count,initial);}
return 0;}
