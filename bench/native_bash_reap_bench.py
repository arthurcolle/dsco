#!/usr/bin/env python3
"""Paired production-runner benchmark against an immutable tools.c snapshot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess

from native_bash_bench import HEADER, ROOT

MAIN = r'''
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int before_run(const char *,char *,size_t,int,const char *);
int after_run(const char *,char *,size_t,int,const char *);
void after_interrupt(int);
static double ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void *interrupt(void *p) {(void)p;usleep(20000);after_interrupt(1);return NULL;}
int main(int argc,char **argv) {
    assert(argc==2);char output[8192],prior[8192];
    const char *cases[]={"printf '%s' \"it's literal\" '$HOME' 'a; b'",
        "a=(one 'two three'); [[ ${#a[@]} == 2 ]] && printf '%s\\n' \"${a[@]}\"",
        "read -r a < <(printf 'hello\\n'); printf '%s' \"$a\"",
        "cat <<'EOF'\n$HOME 'literal' \"text\"\nEOF\n",
        "printf out; printf err >&2; exit 7","kill -TERM $$"};
    for(int i=0;i<6;i++) {
        int before=before_run(cases[i],prior,sizeof(prior),5,argv[1]);
        int after=after_run(cases[i],output,sizeof(output),5,argv[1]);
        assert(before==after && !strcmp(prior,output));
    }
    assert(after_run("printf '%s' \"$PWD\"",output,sizeof(output),5,argv[1])==0);
    assert(!strcmp(output,argv[1]));
    double start=ms();
    assert(after_run("sleep 30 & echo $!; wait",output,sizeof(output),1,argv[1])==124);
    double timeout_ms=ms()-start;int timeout_child=atoi(output);
    assert(timeout_ms<2000 && strstr(output,"wall timeout"));
    /* Closing pipes deliberately must retain the existing 100 ms grace and
     * terminate the process group if it remains alive past that grace. */
    start=ms();
    assert(after_run("sleep 30 >/dev/null 2>&1 & echo $!; exec >/dev/null 2>&1; wait",output,sizeof(output),5,argv[1])==137);
    double grace_ms=ms()-start;int grace_child=atoi(output);assert(grace_ms>=95 && grace_ms<500);
    /* An interruption arriving during EOF reaping is honored promptly. */
    pthread_t thread;assert(!pthread_create(&thread,NULL,interrupt,NULL));start=ms();
    assert(after_run("exec >/dev/null 2>&1; sleep 30",output,sizeof(output),5,argv[1])==130);
    double interrupt_ms=ms()-start;assert(interrupt_ms<500);
    assert(!pthread_join(thread,NULL));after_interrupt(0);
    printf("{\"verification\":true,\"timeout_ms\":%.6f,\"grace_ms\":%.6f,\"interrupt_ms\":%.6f,\"timeout_child\":%d,\"grace_child\":%d}\n",timeout_ms,grace_ms,interrupt_ms,timeout_child,grace_child);
    for(int test=0;test<2;test++) {
        const char *cmd=test?"exec >/dev/null 2>&1; sleep 0.01; exit 7":"pwd";
        for(int i=0;i<100;i++) {
            double times[2];
            for(int step=0;step<2;step++) {
                int which=(i+step)%2;start=ms();
                int status=(which?after_run:before_run)(cmd,output,sizeof(output),5,argv[1]);
                times[which]=ms()-start;assert(status==(test?7:0));
            }
            printf("{\"case\":\"%s\",\"before_ms\":%.6f,\"after_ms\":%.6f}\n",test?"eof_exit_race":"pwd",times[0],times[1]);
        }
    }
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    sources, hashes = [], {}
    for label, path in [("before", args.before), ("after", ROOT / "src/tools.c")]:
        source = path.read_text()
        anchor = source.index("/* ── Streaming subprocess runner")
        start = source.index("typedef struct {", anchor)
        end = source.index("/* Simple wrapper for tools", start)
        runner = source[start:end]
        hashes[label] = hashlib.sha256(runner.encode()).hexdigest()
        wrapper = f'''
int {label}_run(const char *cmd,char *out,size_t len,int timeout,const char *cwd) {{
    run_opts_t opts=RUN_OPTS_DEFAULT;opts.shell="bash";opts.cwd=cwd;
    opts.stream_to_tty=false;opts.wall_timeout_s=timeout;opts.idle_timeout_s=timeout;
    return run_cmd_ex(cmd,out,len,&opts);
}}
void {label}_interrupt(int value) {{g_interrupted=value;}}
'''
        unit = out / f"{label}.c"; unit.write_text(HEADER + runner + wrapper); sources.append(unit)
    unit = out / "main.c"; unit.write_text(MAIN); sources.append(unit)
    binary = out / "paired-reap"
    command = shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
        "-I", str(ROOT / "include"), *map(str, sources), str(ROOT / "src/tool_telemetry.c"), "-lpthread", "-o", str(binary)]
    subprocess.run(command, check=True, timeout=30)
    cwd = out / "cwd with spaces"; cwd.mkdir(exist_ok=True)
    run = subprocess.run([str(binary), str(cwd)], capture_output=True, text=True, timeout=60)
    (out / "samples.jsonl").write_text(run.stdout)
    (out / "stderr.txt").write_text(run.stderr)
    run.check_returncode()
    records = [json.loads(line) for line in run.stdout.splitlines()]
    for key in ["timeout_child", "grace_child"]:
        result = subprocess.run(["ps", "-p", str(records[0][key]), "-o", "stat="], capture_output=True, text=True)
        assert result.returncode or result.stdout.strip().startswith("Z"), f"descendant {key} still running"
    report = {"verification": records[0], "descendants_stopped": True, "runner_sha256": hashes, "compiler_argv": command,
        "scope": "100 alternating pairs per case; same direct Bash production runner, before/after reap change. Native execution only.", "cases": {}}
    for case in ["pwd", "eof_exit_race"]:
        rows = [r for r in records[1:] if r["case"] == case]; result = {"pairs": len(rows)}
        for label in ["before", "after"]:
            times = sorted(r[f"{label}_ms"] for r in rows)
            result[label] = {"p50_ms": statistics.median(times), "p95_ms": times[94], "p99_ms": times[98], "max_ms": times[-1],
                             "over_50ms": sum(t > 50 for t in times)}
        result["median_reduction_percent"] = 100 * (1 - result["after"]["p50_ms"] / result["before"]["p50_ms"])
        report["cases"][case] = result
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
