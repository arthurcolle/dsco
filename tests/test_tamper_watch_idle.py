#!/usr/bin/env python3
"""macOS real-kqueue regression: idle CPU, vnode detection, process exit latency."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import statistics
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]
HELPER=r'''
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <sys/resource.h>
#include <time.h>
#include TAMPER_IMPLEMENTATION
static double cpu_ms(void) { struct rusage r; getrusage(RUSAGE_SELF,&r); return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0; }
static double wall_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
static void wiped(void *unused) { (void)unused; (void)write(STDOUT_FILENO,"wiped\n",6); }
int main(int argc,char **argv) {
    assert(argc>=2); const char *mode=argv[1];
    if(!strcmp(mode,"idle") || !strcmp(mode,"terminate")) {
        tamper_init(); assert(g.watch_running && g.have_code_hash);
    } else {
        assert(argc==3); snprintf(g.exe_path,sizeof(g.exe_path),"%s",argv[2]);
        pthread_mutex_init(&g.wiper_lock,NULL); assert(setup_kqueue_watch());
        tamper_register_wiper(wiped,NULL);
        if(!strcmp(mode,"prequeued")) {
            int fd=open(g.exe_path,O_WRONLY); assert(fd>=0); assert(write(fd,"mutation",8)==8); close(fd);
        }
        if(!strcmp(mode,"error")) close(g.kq_or_inotify);
        g.watch_running=true; assert(pthread_create(&g.watch_thread,NULL,watcher_thread,NULL)==0);
        pthread_detach(g.watch_thread);
    }
    puts("ready"); fflush(stdout);
    double cpu_begin=cpu_ms(),start=wall_ms();
    if(!strcmp(mode,"idle") || !strcmp(mode,"error")) {
        struct timespec duration={.tv_sec=0,.tv_nsec=600000000}; nanosleep(&duration,NULL);
        double wall=wall_ms()-start,cpu=cpu_ms()-cpu_begin;
        printf("{\"wall_ms\":%.6f,\"cpu_ms\":%.6f,\"cpu_percent\":%.6f}\n",wall,cpu,100*cpu/wall);
        return 0;
    }
    for(;;) pause();
}
'''

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--before-source',required=True,type=Path)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    assert os.uname().sysname=='Darwin','this regression exercises native macOS kqueue'
    helper=out/'helper.c';helper.write_text(HELPER)
    sodium=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libsodium'],text=True))
    builds={}
    for variant,source in [('before',a.before_source.resolve()),('after',ROOT/'src/tamper.c')]:
        command=shlex.split(os.environ.get('CC','cc'))+['-O2','-g','-std=c11','-D_DARWIN_C_SOURCE','-DHAVE_LIBSODIUM',f'-DTAMPER_IMPLEMENTATION="{source}"','-I',str(ROOT/'include'),str(helper),*sodium,'-lpthread','-o',str(out/variant)]
        subprocess.run(command,check=True,timeout=30)
        builds[variant]={'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compiler_argv':command}
    env=dict(os.environ,DSCO_DEBUG='1',DSCO_TAMPER_DESTRUCT='0')
    def start(variant,mode,path=None):
        argv=[str(out/variant),mode]+([str(path)] if path else [])
        child=subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        return child
    samples=[]
    for repeat in range(3):
        for variant in (['before','after'] if repeat%2==0 else ['after','before']):
            child=start(variant,'idle')
            stdout,stderr=child.communicate(timeout=3);assert child.returncode==0,stderr
            lines=stdout.splitlines();assert lines[0]=='ready';samples.append({'variant':variant,'repeat':repeat,**json.loads(lines[1])})
    assert statistics.median(x['cpu_percent'] for x in samples if x['variant']=='before')>60
    assert max(x['cpu_percent'] for x in samples if x['variant']=='after')<5
    detections=[]
    for mode in ['write','attributes','prequeued']:
        path=out/f'watched-{mode}';path.write_bytes(b'original\n')
        child=start('after','prequeued' if mode=='prequeued' else 'watch',path)
        try:
            first=child.stdout.readline();assert first.strip() in ['ready','wiped'],first
            begin=time.perf_counter()
            if mode=='write':
                with path.open('ab') as f:f.write(b'changed\n');f.flush()
            elif mode=='attributes':path.chmod(0o600 if path.stat().st_mode&0o777!=0o600 else 0o644)
            elif mode=='rename':path.rename(path.with_suffix('.renamed'))
            elif mode=='delete':path.unlink()
            # readline may already have buffered a rapid second line. Read
            # through the same file object after exit instead of bypassing
            # its buffer with communicate's raw descriptor reads.
            child.wait(timeout=1)
            stdout,stderr=child.stdout.read(),child.stderr.read()
            elapsed=(time.perf_counter()-begin)*1000
            assert child.returncode==1 and 'wiped' in first+stdout and '[TAMPER]' in stderr,(mode,child.returncode,stdout,stderr)
            detections.append({'event':mode,'response_ms':elapsed,'wiper_invoked':True,'exit_code':1,'stderr':stderr})
        finally:
            if child.poll() is None:child.kill();child.wait()
    # The guard is a detached process-lifetime watcher, with no join/stop API.
    stops=[]
    for repeat in range(5):
        child=start('after','terminate');assert child.stdout.readline().strip()=='ready'
        begin=time.perf_counter();child.send_signal(signal.SIGTERM);child.wait(timeout=1)
        stops.append((time.perf_counter()-begin)*1000);assert child.returncode==-signal.SIGTERM
    assert max(stops)<50,stops
    path=out/'watched-error';path.write_text('fixture')
    child=start('after','error',path);stdout,stderr=child.communicate(timeout=3)
    assert child.returncode==0,stderr
    error=json.loads(stdout.splitlines()[-1]);assert error['cpu_percent']<5
    summary={'passed':True,'builds':builds,'samples':samples,'idle_cpu_percent':{v:statistics.median(s['cpu_percent'] for s in samples if s['variant']==v) for v in ['before','after']},'mutations':detections,'termination_ms':stops,'invalid_descriptor_retry':error,'scope':'Real macOS kqueue. Idle/termination paths use public tamper_init on the helper executable. Mutation/error paths call the unchanged production watch setup and response on disposable ordinary files, preserving real vnode events and wipers; no live dsco process or installed binary is modified.'}
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({k:summary[k] for k in ['passed','idle_cpu_percent','termination_ms','invalid_descriptor_retry']},indent=2))
if __name__=='__main__':main()
