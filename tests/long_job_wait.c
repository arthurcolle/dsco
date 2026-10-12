#define _DARWIN_C_SOURCE 1
#include "shell_job_events.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double ms(clockid_t c){struct timespec t;assert(!clock_gettime(c,&t));return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static int status(const char *path){yyjson_doc *d=yyjson_read_file(path,0,NULL,NULL);if(!d)return 1;yyjson_val *o=yyjson_doc_get_root(d);const char *s=yyjson_get_str(yyjson_obj_get(o,"status"));int rc=2;if(s&&(!strcmp(s,"running")||!strcmp(s,"starting")))rc=1;else if(s&&!strcmp(s,"completed")&&yyjson_get_int(yyjson_obj_get(o,"exit_code"))==0)rc=0;yyjson_doc_free(d);return rc;}
int main(int argc,char **argv){assert(argc==3);pid_t pid=(pid_t)atoi(argv[2]);char path[4096];snprintf(path,sizeof(path),"%s/status.json",argv[1]);shell_job_events_t *e=shell_job_events_open(argv[1],pid,false);assert(e);double start=ms(CLOCK_MONOTONIC),cpu=ms(CLOCK_THREAD_CPUTIME_ID);int rc;
while((rc=status(path))==1){if(kill(pid,0)<0 && errno==ESRCH){rc=2;break;}assert(shell_job_events_wait(e,-1)>=0);assert(shell_job_events_wait_count(e)<=32);}
printf("{\"lane\":\"job-observer\",\"elapsed_ms\":%.3f,\"owner_cpu_ms\":%.3f,\"kernel_waits\":%llu,\"wake_events\":%llu,\"backend\":\"%s\",\"passed\":%s}\n",ms(CLOCK_MONOTONIC)-start,ms(CLOCK_THREAD_CPUTIME_ID)-cpu,(unsigned long long)shell_job_events_wait_count(e),(unsigned long long)shell_job_events_wake_count(e),shell_job_events_backend(),rc==0?"true":"false");shell_job_events_close(e);return rc;}
