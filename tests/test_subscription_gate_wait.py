#!/usr/bin/env python3
"""Exercise real cross-process subscription locks, cooldowns, and bounded waits."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
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
    long hold_ms=argc>4?atol(argv[4]):0;
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
    } else if(!strcmp(mode,"holdbrief") && held) {
        long duration=hold_ms>0?hold_ms:30;
        usleep((useconds_t)duration*1000);
    }
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
    printf("{\"ok\":%s,\"held_at_return\":%s,\"fd_at_return\":%d,\"slot\":%u,\"capacity\":%u,\"errno\":%d,\"waited_ms\":%ld,\"elapsed_ms\":%.6f,\"acquired_ms\":%.6f,\"before_release_ms\":%.6f,\"released_ms\":%.6f,\"fd_before\":%d,\"fd_after\":%d}\n",ok?"true":"false",held?"true":"false",acquired_fd,gate.slot,gate.capacity,reason,waited,acquired-started,acquired,before_release,released,before,after);
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
    parser.add_argument('--benchmark-matrix',action='store_true',
                        help='run an opt-in request-count/concurrency saturation sweep')
    parser.add_argument('--benchmark-requests',default='8,16,32,64',
                        help='comma-separated request counts for --benchmark-matrix')
    parser.add_argument('--benchmark-capacities',default='1,2,4,8,16,32,64',
                        help='comma-separated lane permit counts for --benchmark-matrix')
    parser.add_argument('--benchmark-hold-ms',type=int,default=100,
                        help='simulated service time per request (default: 100)')
    parser.add_argument('--benchmark-repeats',type=int,default=2,
                        help='repeats per matrix cell (default: 2, max: 5)')
    parser.add_argument('--benchmark-optimized',action='store_true',
                        help='use an O3 non-sanitized helper for large performance waves')
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    def positive_csv(value,label,maximum):
        try: values=sorted(set(int(item.strip()) for item in value.split(',') if item.strip()))
        except ValueError as error: parser.error(f'{label} must contain integers: {error}')
        if not values or values[0]<1 or values[-1]>maximum:
            parser.error(f'{label} values must be in 1..{maximum}')
        return values
    benchmark_requests=positive_csv(args.benchmark_requests,'--benchmark-requests',512)
    benchmark_capacities=positive_csv(args.benchmark_capacities,'--benchmark-capacities',512)
    if not 10<=args.benchmark_hold_ms<=5000:
        parser.error('--benchmark-hold-ms must be in 10..5000')
    if not 1<=args.benchmark_repeats<=5:
        parser.error('--benchmark-repeats must be in 1..5')
    helper=out/'helper.c'; helper.write_text(HELPER)
    compiler=shlex.split(os.environ.get('CC','cc'))
    builds={}
    variants=[('after',ROOT/'src/subscription_gate.c')]+([('before',args.before_source.resolve())] if args.before_source else [])
    if args.benchmark_matrix and args.benchmark_optimized:
        variants.append(('benchmark',ROOT/'src/subscription_gate.c'))
    for variant,source in variants:
        flags=(['-O3'] if variant=='benchmark' else ['-O1','-g','-fsanitize=address,undefined'])
        command=compiler+flags+['-std=c11','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),str(helper),str(source)]+([str(ROOT/'src/provider_admission.c')] if variant in ('after','benchmark') else [])+['-o',str(out/variant)]
        subprocess.run(command,check=True,timeout=30)
        builds[variant]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compiler_argv':command}
    env=dict(os.environ,DSCO_CHATGPT_GLOBAL_GATE='1',DSCO_CHATGPT_MIN_INTERVAL_MS='0',DSCO_CHATGPT_GATE_MAX_WAIT_MS='1000',DSCO_CHATGPT_LANE_CONCURRENCY='1')
    scopes=[]; children=[]; records={}
    def fresh():
        scope=f'dsco-gate-regression-{uuid.uuid4().hex}'; scopes.append(scope); return scope
    def launch(mode,scope,delay=0,variant='after',child_env=None,hold_ms=0):
        child=subprocess.Popen([str(out/variant),mode,scope,str(delay),str(hold_ms)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=child_env or env)
        children.append(child); return child
    def collect(child,timeout=5):
        stdout,stderr=child.communicate(timeout=timeout)
        assert child.returncode==0,stderr
        result=json.loads(stdout)
        assert result['fd_before']==result['fd_after'],result
        return result
    def probe(mode,scope,variant='after'):
        return collect(launch(mode,scope,variant=variant))
    def holder(scope,delay=0,child_env=None):
        child=launch('holder',scope,delay,child_env=child_env)
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
        default_env=dict(env); default_env.pop('DSCO_CHATGPT_LANE_CONCURRENCY')
        records['default_capacity']=collect(launch('probe',fresh(),child_env=default_env))
        assert records['default_capacity']['ok'] and records['default_capacity']['capacity']==4
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

        parallel_env=dict(env,DSCO_CHATGPT_LANE_CONCURRENCY='2')
        scope=fresh(); first=holder(scope,child_env=parallel_env)
        second=holder(scope,child_env=parallel_env)
        third=launch('probe',scope,child_env=parallel_env)
        records['parallel_permits']={'first_ready':True,'second_ready':True}
        records['parallel_third_timeout']=collect(third)
        failed(records['parallel_third_timeout'],errno.ETIMEDOUT)
        release(first); release(second)

        lane_a=fresh(); lane_b=fresh(); a=holder(lane_a); b=holder(lane_b)
        records['swimlane_isolation']={'lane_a_ready':True,'lane_b_ready':True}
        release(a); release(b)

        def timed_wave(capacity,request_count=8,hold_ms=30):
            # Raise only the fixture's wait ceiling so every queued request can
            # finish and expose the full latency distribution. Product limits
            # and provider behavior are not changed by this test setting.
            max_wait_ms=max(1000,request_count*hold_ms*2+5000)
            wave_env=dict(env,DSCO_CHATGPT_LANE_CONCURRENCY=str(capacity),
                          DSCO_CHATGPT_GATE_MAX_WAIT_MS=str(max_wait_ms))
            wave_scope=fresh(); started=time.monotonic()
            variant='benchmark' if args.benchmark_optimized else 'after'
            wave=[launch('holdbrief',wave_scope,variant=variant,child_env=wave_env,hold_ms=hold_ms)
                  for _ in range(request_count)]
            timeout=max(5,request_count*hold_ms/1000+5)
            completed=[collect(child,timeout=timeout) for child in wave]
            return (time.monotonic()-started)*1000,completed
        serial_ms,serial_wave=timed_wave(1)
        parallel_ms,parallel_wave=timed_wave(4)
        assert all(item['ok'] for item in serial_wave+parallel_wave)
        assert parallel_ms < serial_ms*.75,(serial_ms,parallel_ms)
        records['throughput_wave']={'requests':8,'hold_ms_each':30,
                                    'capacity_1_ms':serial_ms,
                                    'capacity_4_ms':parallel_ms,
                                    'speedup':serial_ms/parallel_ms}
        if args.benchmark_matrix:
            samples=[]
            for repeat in range(args.benchmark_repeats):
                capacities=(benchmark_capacities if repeat%2==0
                            else list(reversed(benchmark_capacities)))
                for request_count in benchmark_requests:
                    for capacity in capacities:
                        if capacity>request_count: continue
                        wall_ms,wave=timed_wave(capacity,request_count,
                                                args.benchmark_hold_ms)
                        waits=sorted(item['waited_ms'] for item in wave)
                        failures=sum(not item['ok'] for item in wave)
                        def percentile(fraction):
                            index=max(0,min(len(waits)-1,
                                            int((len(waits)-1)*fraction+.5)))
                            return waits[index]
                        samples.append({
                            'repeat':repeat+1,
                            'requests':request_count,
                            'capacity':capacity,
                            'hold_ms_each':args.benchmark_hold_ms,
                            'wall_ms':wall_ms,
                            'throughput_rps':request_count*1000/wall_ms,
                            'wait_p50_ms':percentile(.50),
                            'wait_p95_ms':percentile(.95),
                            'wait_p99_ms':percentile(.99),
                            'failures':failures,
                        })
            cells=[]
            for request_count in benchmark_requests:
                baseline=None
                for capacity in benchmark_capacities:
                    selected=[item for item in samples
                              if item['requests']==request_count and
                              item['capacity']==capacity]
                    if not selected: continue
                    wall_ms=statistics.median(item['wall_ms'] for item in selected)
                    throughput=statistics.median(item['throughput_rps'] for item in selected)
                    if capacity==1: baseline=throughput
                    cells.append({
                        'requests':request_count,
                        'capacity':capacity,
                        'median_wall_ms':wall_ms,
                        'median_throughput_rps':throughput,
                        'speedup_vs_capacity_1':throughput/baseline if baseline else None,
                        'worst_wait_p95_ms':max(item['wait_p95_ms'] for item in selected),
                        'worst_wait_p99_ms':max(item['wait_p99_ms'] for item in selected),
                        'failures':sum(item['failures'] for item in selected),
                    })
            records['benchmark_matrix']={
                'control':'capacity_1',
                'request_counts':benchmark_requests,
                'capacities':benchmark_capacities,
                'hold_ms_each':args.benchmark_hold_ms,
                'repeats':args.benchmark_repeats,
                'optimized_helper':args.benchmark_optimized,
                'samples':samples,
                'cells':cells,
                'limits':[
                    'This isolates real cross-process gate admission and queueing with deterministic simulated service; it does not invoke a provider.',
                    'Provider throttling, token throughput, response quality, and inference cost require a separate live-provider benchmark.',
                ],
            }
        records['repeated_success_and_cancel']=probe('repeat',fresh())
        records['filesystem_failopen']=probe('failopen',fresh())
        failed(records['filesystem_failopen'],records['filesystem_failopen']['errno'])
        assert records['filesystem_failopen']['errno'] in (errno.EIO,errno.EMFILE)
        if args.before_source:
            records['before_precancel_acquires']=probe('precancel',fresh(),'before')
            assert records['before_precancel_acquires']['ok'] and records['before_precancel_acquires']['held_at_return']
    finally:
        for child in children:
            if child.poll() is None: child.kill(); child.wait(timeout=5)
        # Only randomized gate files created by this run are removed. HOME and
        # saved gate/provider configuration are never changed.
        for scope in scopes:
            base=scope_path(scope); base.unlink(missing_ok=True)
            for slot in base.parent.glob(base.name+'-slot-*'): slot.unlink(missing_ok=True)
            import shutil; shutil.rmtree(str(base)+'.waiters',ignore_errors=True)
    result={'passed':True,'sanitizers':['address','undefined'],'builds':builds,'records':records,'limits':['Real local advisory locks and wall-clock cooldown files; network filesystems and power-loss behavior are not exercised.','The total budget bounds lock and cooldown polling; a blocking kernel/filesystem call or scheduler delay can still overshoot wall-clock delivery.']}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    summary={'passed':True,'lock_timeout_ms':records['lock_timeout']['elapsed_ms'],'lock_cancel_ms':records['lock_cancel']['elapsed_ms'],'combined_timeout_ms':records['combined_timeout']['elapsed_ms'],'fd_checks_passed':True,'serialized_processes_at_capacity_1':4,'parallel_permits_checked':2,'swimlane_isolation':True,'eight_request_speedup':records['throughput_wave']['speedup']}
    if args.benchmark_matrix:
        summary['benchmark_cells']=records['benchmark_matrix']['cells']
    print(json.dumps(summary,indent=2))

if __name__=='__main__': main()
