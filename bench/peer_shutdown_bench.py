#!/usr/bin/env python3
"""Production peer discovery lifecycle with deterministic DNS descriptors, no network."""
import argparse
import json
import hashlib
from pathlib import Path
import statistics
import subprocess

ROOT=Path(__file__).resolve().parents[1]
HEAD=r'''
#include <dns_sd.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include "mesh.h"
struct fake_dns {int fd[2];};
static struct fake_dns refs[2];
static atomic_int ready,processed,freed;
static int register_fails,browse_fails,pipe_fails,thread_fails;
static DNSServiceErrorType fixture_register(DNSServiceRef *ref){
 if(register_fails){atomic_store(&ready,1);return kDNSServiceErr_Unknown;}
 assert(pipe(refs[0].fd)==0);*ref=(DNSServiceRef)&refs[0];return kDNSServiceErr_NoError;
}
static DNSServiceErrorType fixture_browse(DNSServiceRef *ref){
 if(browse_fails){atomic_store(&ready,1);return kDNSServiceErr_Unknown;}
 assert(pipe(refs[1].fd)==0);*ref=(DNSServiceRef)&refs[1];atomic_store(&ready,1);return kDNSServiceErr_NoError;
}
static int fixture_fd(DNSServiceRef ref){return ((struct fake_dns*)ref)->fd[0];}
static DNSServiceErrorType fixture_process(DNSServiceRef ref){
 char c;assert(read(fixture_fd(ref),&c,1)==1);atomic_fetch_add(&processed,1);return kDNSServiceErr_NoError;
}
static void fixture_free(DNSServiceRef ref){
 struct fake_dns *r=(struct fake_dns*)ref;close(r->fd[0]);close(r->fd[1]);atomic_fetch_add(&freed,1);
}
static int fixture_pipe(int *fds){if(pipe_fails){errno=EMFILE;return -1;}return pipe(fds);}
static int fixture_thread(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void*),void *v){
 if(thread_fails){thread_fails=0;atomic_store(&ready,1);return EAGAIN;}return pthread_create(t,a,fn,v);
}
#define DNSServiceRegister(ref,...) fixture_register(ref)
#define DNSServiceBrowse(ref,...) fixture_browse(ref)
#define DNSServiceRefSockFD(ref) fixture_fd(ref)
#define DNSServiceProcessResult(ref) fixture_process(ref)
#define DNSServiceRefDeallocate(ref) fixture_free(ref)
#define pipe(fds) fixture_pipe(fds)
#define pthread_create(t,a,fn,v) fixture_thread(t,a,fn,v)
int64_t audit_log(const char *tag,const char *msg){(void)tag;(void)msg;return 0;}
bool mesh_node_connect_interruptible(mesh_node_t*n,const char*h,uint16_t p,bool(*c)(void*),void*x){(void)n;(void)h;(void)p;(void)c;(void)x;return false;}
'''
MAIN=r'''
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void pause_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,NULL);}
static void await_count(atomic_int *v,int n){double end=now_ms()+2500;while(atomic_load(v)<n){assert(now_ms()<end);pause_ms(1);}}
static double cycle(int exercise_events){
 atomic_store(&ready,0);atomic_store(&processed,0);atomic_store(&freed,0);
 peer_bootstrap_init(NULL,7337);await_count(&ready,1);
#ifdef OPTIMIZED
 if(s_mdns_wake[0]>=0){
  assert(fcntl(s_mdns_wake[0],F_GETFD)&FD_CLOEXEC);
  assert(fcntl(s_mdns_wake[1],F_GETFD)&FD_CLOEXEC);
  assert(fcntl(s_mdns_wake[1],F_GETFL)&O_NONBLOCK);
 }
#endif
 if(exercise_events){
  assert(write(refs[0].fd[1],"r",1)==1);assert(write(refs[1].fd[1],"b",1)==1);
  await_count(&processed,2);
 }
 pause_ms(35);
 double start=now_ms();peer_bootstrap_stop();double elapsed=now_ms()-start;
 assert(atomic_load(&freed)==(register_fails?0:(browse_fails?1:2)));
 assert(s_sdref==NULL);peer_bootstrap_stop();
#ifdef OPTIMIZED
 assert(s_mdns_wake[0]==-1&&s_mdns_wake[1]==-1&&!s_mdns_started);
#endif
 return elapsed;
}
int main(int argc,char**argv){
 setenv("DSCO_PEERS"," ",1);
 int rounds=argc>1?atoi(argv[1]):8;
 for(int i=0;i<rounds;i++)printf("{\"case\":\"idle_stop\",\"round\":%d,\"elapsed_ms\":%.6f}\n",i,cycle(1));
 register_fails=1;printf("{\"case\":\"register_failure\",\"elapsed_ms\":%.6f}\n",cycle(0));register_fails=0;
 browse_fails=1;printf("{\"case\":\"browse_failure\",\"elapsed_ms\":%.6f}\n",cycle(0));browse_fails=0;
#ifdef OPTIMIZED
 pipe_fails=1;double fallback=cycle(1);pipe_fails=0;assert(fallback<1500);
 printf("{\"case\":\"wake_pipe_failure_fallback\",\"elapsed_ms\":%.6f}\n",fallback);
 thread_fails=1;register_fails=1;cycle(0);register_fails=0;
 printf("{\"case\":\"thread_creation_failure\",\"passed\":true}\n");
#endif
 dsco_waiter_destroy(&s_waiter);
}
'''


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--before',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--rounds',type=int,default=8)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    records={};hashes={};commands=[]
    for label,path in [('before',args.before),('after',ROOT/'src/peer_bootstrap.c')]:
        source=path.read_text();hashes[label]=hashlib.sha256(source.encode()).hexdigest()
        (out/(label+'-source.c')).write_text(source)
        unit=out/(label+'.c');unit.write_text(HEAD+'\n'+source+'\n'+MAIN)
        cmd=['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-DHAVE_LIBSODIUM','-O2','-I'+str(ROOT/'include'),'-I/opt/homebrew/include',str(unit),str(ROOT/'src/waiter.c'),'-lpthread','-o',str(out/label)]
        if label=='after':cmd.insert(1,'-DOPTIMIZED')
        subprocess.run(cmd,check=True);commands.append(cmd)
        result=subprocess.run([str(out/label),str(args.rounds)],capture_output=True,text=True,check=True,timeout=30)
        (out/(label+'.stdout')).write_text(result.stdout);(out/(label+'.stderr')).write_text(result.stderr)
        records[label]=[json.loads(line) for line in result.stdout.splitlines()]
        if label=='after':
            asan=cmd.copy();asan[1:1]=['-fsanitize=address,undefined','-g'];asan[-1]=str(out/'asan')
            subprocess.run(asan,check=True);commands.append(asan)
            r=subprocess.run([str(out/'asan'),'2'],capture_output=True,text=True,check=True,timeout=15)
            (out/'asan.stdout').write_text(r.stdout);(out/'asan.stderr').write_text(r.stderr)
    summary={}
    for label,rows in records.items():
        times=sorted(r['elapsed_ms'] for r in rows if r['case']=='idle_stop')
        summary[label]={'n':len(times),'median_ms':statistics.median(times),'min_ms':min(times),'max_ms':max(times)}
    result={'scope':'actual production lifecycle functions and OS waits; DNS service API supplied deterministic pipe descriptors; no inference/network',
            'hashes':hashes,'compiler_argv':commands,'records':records,'summary':summary,'asan_ubsan_passed':True}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
