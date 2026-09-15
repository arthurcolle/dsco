#!/usr/bin/env python3
"""Compile the production streaming runner and compare wrapped/direct Bash.

Only terminal rendering callbacks are inert in this non-streaming harness.
The production spawn, process-group, poll, cwd, timeout, exit, and capture code
is compiled directly from tools.c. No inference or external services.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#define _DARWIN_C_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <spawn.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <assert.h>
#include "tool_telemetry.h"
extern char **environ;
static volatile sig_atomic_t g_interrupted;
#define TUI_DIM ""
#define TUI_RESET ""
static const char *md_lang_from_path(const char *path) {(void)path; return NULL;}
static void md_render_code_line(FILE *out,const char *line,const char *lang) {(void)out;(void)line;(void)lang;}
'''
FOOTER = r'''
static double milliseconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}
static int execute(const char *cmd,bool direct,const char *cwd,char *out,size_t len,int timeout) {
    run_opts_t opts=RUN_OPTS_DEFAULT; opts.stream_to_tty=false;
    opts.cwd=cwd; opts.wall_timeout_s=timeout; opts.idle_timeout_s=timeout;
    opts.shell=direct?"bash":NULL;
    char wrapped[16384]; size_t k=0;
    memcpy(wrapped,"bash -c '",9); k=9;
    for(const char *p=cmd;*p;p++) {
        if(*p=='\'') {memcpy(wrapped+k,"'\\''",4);k+=4;}
        else wrapped[k++]=*p;
    }
    wrapped[k++]='\'';wrapped[k]=0;
    return run_cmd_ex(direct?cmd:wrapped,out,len,&opts);
}
static void *interrupt_soon(void *unused) {
    (void)unused; usleep(100000);g_interrupted=1;return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2); char legacy[8192],direct[8192];
    const char *cases[]={
        "printf '%s\\n' \"it's literal\" '$HOME' 'a; b'",
        "items=(alpha 'beta gamma'); [[ ${#items[@]} == 2 ]] && printf '%s\\n' \"${items[@]}\"",
        "read -r value < <(printf 'process-substitution\\n'); printf '%s\\n' \"$value\"",
        "cat <<'EOF'\n$HOME 'double' \"quotes\" ; &\nEOF\n",
        "printf '%s\\n' \"$PWD\"; printf '%s' stderr >&2; exit 7",
        "printf '%s' \"$(printf nested)\""
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        int left=execute(cases[i],false,argv[1],legacy,sizeof(legacy),5);
        int right=execute(cases[i],true,argv[1],direct,sizeof(direct),5);
        assert(left==right && !strcmp(legacy,direct));
    }
    assert(execute("printf '%s' \"$PWD\"",true,argv[1],direct,sizeof(direct),5)==0);
    assert(!strcmp(direct,argv[1]));
    tool_telemetry_reset(); double begin=milliseconds();
    assert(execute("sleep 30 & echo $!; wait",true,argv[1],direct,sizeof(direct),1)==124);
    double timeout_ms=milliseconds()-begin;
    assert(timeout_ms<2500 && strstr(direct,"wall timeout"));
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_WALL);
    int descendant=atoi(direct);assert(descendant>0);
    pthread_t thread;assert(!pthread_create(&thread,NULL,interrupt_soon,NULL));begin=milliseconds();
    assert(execute("sleep 30",true,argv[1],direct,sizeof(direct),5)==130);
    double interrupt_ms=milliseconds()-begin;assert(interrupt_ms<2000);
    assert(!pthread_join(thread,NULL));g_interrupted=0;
    printf("{\"semantic_cases\":6,\"cwd_verified\":true,\"timeout_ms\":%.3f,\"interruption_ms\":%.3f,\"timeout_descendant\":%d}\n",timeout_ms,interrupt_ms,descendant);
    for(int i=0;i<100;i++) {
        double old_ms,new_ms;
        for(int j=0;j<2;j++) {
            bool use_direct=(i+j)%2; begin=milliseconds();
            assert(execute("pwd",use_direct,argv[1],direct,sizeof(direct),5)==0);
            double elapsed=milliseconds()-begin;if(use_direct)new_ms=elapsed;else old_ms=elapsed;
        }
        printf("{\"legacy_ms\":%.6f,\"direct_ms\":%.6f}\n",old_ms,new_ms);
    }
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/tools.c").read_text()
    anchor = source.index("/* ── Streaming subprocess runner")
    start = source.index("typedef struct {", anchor)
    end = source.index("/* Simple wrapper for tools", start)
    production = source[start:end]
    unit = out / "runner.c"; unit.write_text(HEADER + production + FOOTER)
    binary = out / "runner"
    command = shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-std=c11",
        "-D_POSIX_C_SOURCE=200809L", "-I", str(ROOT / "include"), str(unit),
        str(ROOT / "src/tool_telemetry.c"), "-lpthread", "-o", str(binary)]
    subprocess.run(command, check=True, timeout=30)
    cwd = out / "directory with spaces"; cwd.mkdir(exist_ok=True)
    run = subprocess.run([str(binary), str(cwd)], capture_output=True, text=True, timeout=30, check=True)
    (out / "stdout.jsonl").write_text(run.stdout)
    records = [json.loads(line) for line in run.stdout.splitlines()]
    check = subprocess.run(["ps", "-p", str(records[0]["timeout_descendant"]), "-o", "stat="], capture_output=True, text=True)
    assert check.returncode != 0 or check.stdout.strip().startswith("Z"), "timeout descendant still running"
    samples = records[1:]
    summary = {"verification": records[0], "timeout_descendant_not_running": True,
        "pairs": len(samples), "legacy_p50_ms": statistics.median(x["legacy_ms"] for x in samples),
        "direct_p50_ms": statistics.median(x["direct_ms"] for x in samples),
        "legacy_p95_ms": sorted(x["legacy_ms"] for x in samples)[94],
        "direct_p95_ms": sorted(x["direct_ms"] for x in samples)[94],
        "runner_sha256": hashlib.sha256(production.encode()).hexdigest(), "compiler_argv": command,
        "scope": "Isolated production streaming runner; alternated wrapped/direct Bash pwd, 100 pairs. No LLM or full tool-gate overhead."}
    summary["median_reduction_percent"] = 100 * (1 - summary["direct_p50_ms"] / summary["legacy_p50_ms"])
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
