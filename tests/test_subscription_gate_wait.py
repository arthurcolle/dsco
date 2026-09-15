#!/usr/bin/env python3
"""Exercise real cross-process subscription locks, cooldowns, and bounded waits."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
HELPER = r'''
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "subscription_gate.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static volatile int interrupted;
static void cancel(int signum) { (void)signum; interrupted = 1; }
static double ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0+ts.tv_nsec/1e6; }
static int fd_count(void) { int n=0; for(int fd=0;fd<256;fd++) if(fcntl(fd,F_GETFD)>=0) n++; return n; }
int main(int argc,char **argv) {
    assert(argc>=3);
    const char *mode=argv[1],*scope=argv[2];
    long delay=argc>3?atol(argv[3]):0;
    int before=fd_count();
    if (!strcmp(mode,"precancel")) interrupted=1;
    if (!strcmp(mode,"cancel") || !strcmp(mode,"latecancel")) {
        struct sigaction action; memset(&action,0,sizeof(action));
        action.sa_handler=cancel; sigemptyset(&action.sa_mask);
        assert(sigaction(SIGALRM,&action,NULL)==0);
        ualarm(!strcmp(mode,"latecancel")?1500000:150000,0);
    }
    if (!strcmp(mode,"failopen")) { struct rlimit limit={0,0}; assert(setrlimit(RLIMIT_NOFILE,&limit)==0); }
    subscription_gate_t gate={.fd=-1}; long waited=-1;
    double started=ms(); int ok=subscription_gate_acquire(&gate,scope,&interrupted,&waited);
    int reason=errno; double acquired=ms(); int held=gate.held, acquired_fd=gate.fd;
    if (!strcmp(mode,"holder")) {
        assert(ok && held);
        printf("{\"ready\":true,\"acquired_ms\":%.6f}\n",acquired); fflush(stdout);
        assert(getchar()=='g');
    } else if(!strcmp(mode,"holdbrief") && held) usleep(30000);
    double before_release=ms(); subscription_gate_release(&gate,delay);
    double released=ms();
    if (!strcmp(mode,"repeat")) {
        assert(ok && held);
        for(int i=0;i<500;i++) {
            assert(subscription_gate_acquire(&gate,scope,&interrupted,&waited));
            assert(gate.held); subscription_gate_release(&gate,0);
            interrupted=1;
            assert(!subscription_gate_acquire(&gate,scope,&interrupted,&waited) && errno==EINTR);
            assert(gate.fd==-1 && !gate.held); interrupted=0;
        }
    }
    int after=fd_count();
    printf("{\"ok\":%s,\"held_at_return\":%s,\"fd_at_return\":%d,\"errno\":%d,\"waited_ms\":%ld,\"elapsed_ms\":%.6f,\"acquired_ms\":%.6f,\"before_release_ms\":%.6f,\"released_ms\":%.6f,\"fd_before\":%d,\"fd_after\":%d}\n",ok?"true":"false",held?"true":"false",acquired_fd,reason,waited,acquired-started,acquired,before_release,released,before,after);
    return 0;
}
'''

def scope_path(scope):
    value = 1469598103934665603
    for byte in scope.encode():
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return Path(os.environ.get('HOME') or '/tmp') / '.dsco' / f'openai-codex-gate-{value:016x}'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--before-source',type=Path)
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    helper=out/'helper.c'; helper.write_text(HELPER)
    compiler=shlex.split(os.environ.get('CC','cc'))
    builds={}
    for variant,source in [('after',ROOT/'src/subscription_gate.c')]+([('before',args.before_source.resolve())] if args.before_source else []):
        command=compiler+['-O1','-g','-std=c11','-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-I',str(ROOT/'include'),str(helper),str(source),'-o',str(out/variant)]
        subprocess.run(command,check=True,timeout=30)
        builds[variant]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compiler_argv':command}
    env=dict(os.environ,DSCO_CHATGPT_GLOBAL_GATE='1',DSCO_CHATGPT_MIN_INTERVAL_MS='0',DSCO_CHATGPT_GATE_MAX_WAIT_MS='1000')
    scopes=[]; children=[]; records={}
    def fresh():
        scope=f'dsco-gate-regression-{uuid.uuid4().hex}'; scopes.append(scope); return scope
    def launch(mode,scope,delay=0,variant='after'):
        child=subprocess.Popen([str(out/variant),mode,scope,str(delay)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        children.append(child); return child
    def collect(child):
        stdout,stderr=child.communicate(timeout=5)
        assert child.returncode==0,stderr
        result=json.loads(stdout)
        assert result['fd_before']==result['fd_after'],result
        return result
    def probe(mode,scope,variant='after'):
        return collect(launch(mode,scope,variant=variant))
    def holder(scope,delay=0):
        child=launch('holder',scope,delay)
        assert json.loads(child.stdout.readline())['ready']; return child
    def release(child):
        child.stdin.write('g\n'); child.stdin.flush(); return collect(child)
    def failed(result,reason):
        assert not result['ok'] and not result['held_at_return'] and result['fd_at_return']==-1,result
        assert result['errno']==reason,result
        assert abs(result['waited_ms']-result['elapsed_ms'])<20,result
    try:
        records['precancel']=probe('precancel',fresh()); failed(records['precancel'],errno.EINTR)
        assert records['precancel']['elapsed_ms']<100
        scope=fresh(); lock=holder(scope)
        records['lock_timeout']=probe('probe',scope); failed(records['lock_timeout'],errno.ETIMEDOUT)
        assert 950<=records['lock_timeout']['elapsed_ms']<1300
        records['lock_cancel']=probe('cancel',scope); failed(records['lock_cancel'],errno.EINTR)
        assert 100<=records['lock_cancel']['elapsed_ms']<350
        if args.before_source:
            records['before_lock_exceeds_budget']=probe('latecancel',scope,'before')
            assert records['before_lock_exceeds_budget']['elapsed_ms']>=1400
        release(lock)
        records['after_release']=probe('probe',scope)
        assert records['after_release']['ok'] and records['after_release']['held_at_return'] and records['after_release']['elapsed_ms']<250

        # Contention and cooldown share a single deadline. Timing out must not
        # erase the persisted cooldown needed by the following process.
        scope=fresh(); lock=holder(scope,700); contender=launch('probe',scope)
        time.sleep(.55); released=release(lock)
        records['combined_timeout']=collect(contender); failed(records['combined_timeout'],errno.ETIMEDOUT)
        assert 950<=records['combined_timeout']['elapsed_ms']<1300
        records['cooldown_after_timeout']=probe('probe',scope)
        assert records['cooldown_after_timeout']['ok'] and records['cooldown_after_timeout']['held_at_return']
        assert records['cooldown_after_timeout']['acquired_ms']>=released['before_release_ms']+695

        scope=fresh(); lock=holder(scope,650); released=release(lock)
        records['cooldown_cancel']=probe('cancel',scope); failed(records['cooldown_cancel'],errno.EINTR)
        records['cooldown_after_cancel']=probe('probe',scope)
        assert records['cooldown_after_cancel']['ok'] and records['cooldown_after_cancel']['held_at_return']
        assert records['cooldown_after_cancel']['acquired_ms']>=released['before_release_ms']+645

        scope=fresh(); processes=[launch('holdbrief',scope) for _ in range(4)]
        records['mutual_exclusion']=sorted([collect(child) for child in processes],key=lambda r:r['acquired_ms'])
        for result in records['mutual_exclusion']: assert result['ok'] and result['held_at_return']
        for previous,current in zip(records['mutual_exclusion'],records['mutual_exclusion'][1:]):
            assert current['acquired_ms']>=previous['before_release_ms']
        records['repeated_success_and_cancel']=probe('repeat',fresh())
        records['filesystem_failopen']=probe('failopen',fresh())
        assert records['filesystem_failopen']['ok'] and not records['filesystem_failopen']['held_at_return']
        if args.before_source:
            records['before_precancel_acquires']=probe('precancel',fresh(),'before')
            assert records['before_precancel_acquires']['ok'] and records['before_precancel_acquires']['held_at_return']
    finally:
        for child in children:
            if child.poll() is None: child.kill(); child.wait(timeout=5)
        # Only randomized gate files created by this run are removed. HOME and
        # saved gate/provider configuration are never changed.
        for scope in scopes: scope_path(scope).unlink(missing_ok=True)
    result={'passed':True,'sanitizers':['address','undefined'],'builds':builds,'records':records,'limits':['Real local advisory locks and wall-clock cooldown files; network filesystems and power-loss behavior are not exercised.','The total budget bounds lock and cooldown polling; a blocking kernel/filesystem call or scheduler delay can still overshoot wall-clock delivery.']}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'passed':True,'lock_timeout_ms':records['lock_timeout']['elapsed_ms'],'lock_cancel_ms':records['lock_cancel']['elapsed_ms'],'combined_timeout_ms':records['combined_timeout']['elapsed_ms'],'fd_checks_passed':True,'mutual_exclusion_processes':4},indent=2))

if __name__=='__main__': main()
