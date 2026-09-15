#!/usr/bin/env python3
"""Compare production FILE JSON escaping, including every non-NUL byte."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract(path, name, replacement):
    source = path.read_text()
    start = source.index("static void " + name + "(")
    end = source.index("\n}\n", start) + 3
    return source[start:end].replace(name, replacement, 1)


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <assert.h>
typedef void (*escape_fn)(FILE *, const char *);
__FUNCTIONS__
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void check(const char *s) {
    escape_fn funcs[]={before_main,after_main,before_mcp,after_mcp};
    char *data[4]={0};size_t sizes[4]={0};
    for(int k=0;k<4;k++){
        FILE*f=open_memstream(&data[k],&sizes[k]);assert(f);
        funcs[k](f,s);assert(!fclose(f));
        if(k)assert(sizes[0]==sizes[k]&&!memcmp(data[0],data[k],sizes[0]));
    }
    for(int k=0;k<4;k++)free(data[k]);
}
int main(int argc,char **argv) {
    (void)argv;
    char bytes[256];
    for(int i=1;i<256;i++)bytes[i-1]=(char)i;bytes[255]=0;check(bytes);check("");
    uint32_t state=73;char sample[4097];
    for(int n=0;n<600;n++){
        size_t len=(size_t)(n*37)%4097;
        for(size_t i=0;i<len;i++){state=state*1664525+1013904223;sample[i]=(char)(1+(state%255));}
        sample[len]=0;check(sample);
    }
    char *buffer=NULL;size_t len=0;FILE*f=open_memstream(&buffer,&len);
    after_main(f,NULL);assert(!fclose(f)&&len==0);free(buffer);
    puts("{\"equivalence_cases\":602,\"null_main_check\":true}");
    if(argc>1)return 0;
    escape_fn funcs[]={before_main,after_main,before_mcp,after_mcp};
    const char*names[]={"before_main","after_main","before_mcp","after_mcp"};
    const char*labels[]={"short","plain_256k","code_256k","utf8_256k","escapes_16k"};
    const char*patterns[]={"ordinary small message","abcdefghijklmnopqrstuvwxyz0123456789 ",
        "if (ready) {\n    emit(\"value\", path); // code\n}\n",
        "\xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x8c\x8d text ",
        "\"\\\n\t\r\b"};
    size_t lengths[]={32,262144,262144,262144,16384};
    FILE*sink=fopen("/dev/null","w");assert(sink);setvbuf(sink,NULL,_IOFBF,65536);
    for(int kind=0;kind<5;kind++){
        size_t size=lengths[kind],plen=strlen(patterns[kind]);
        char*s=malloc(size+1);assert(s);
        for(size_t i=0;i<size;i++)s[i]=patterns[kind][i%plen];s[size]=0;check(s);
        int count=kind==0?20000:kind==4?40:20;
        for(int batch=0;batch<15;batch++)for(int j=0;j<4;j++){
            int k=batch%2?3-j:j;
            double begin=now();for(int i=0;i<count;i++)funcs[k](sink,s);
            fflush(sink);double us=(now()-begin)*1e6/count;
            printf("{\"case\":\"%s\",\"function\":\"%s\",\"batch\":%d,\"us\":%.6f}\n",
                   labels[kind],names[k],batch,us);
        }
        free(s);
    }
    fclose(sink);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    functions, hashes = [], {}
    for variant, base in [("before", args.before_source), ("after", ROOT)]:
        for label, file, name in [("main", "src/main.c", "json_print_escaped"),
                                  ("mcp", "src/mcp_server.c", "json_escape_into")]:
            body = extract(base / file, name, variant + "_" + label)
            functions.append(body)
            hashes[variant + "_" + label] = hashlib.sha256(body.encode()).hexdigest()
    unit = out / "stdio_json.c"
    unit.write_text(HARNESS.replace("__FUNCTIONS__", "\n".join(functions)))
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_DARWIN_C_SOURCE",
               "-D_POSIX_C_SOURCE=200809L", "-O3", str(unit)]
    binary = out / "bench"
    subprocess.run(command + ["-o", str(binary)], check=True, timeout=30)
    run = subprocess.run([str(binary)], capture_output=True, text=True, check=True, timeout=60)
    (out / "raw.jsonl").write_text(run.stdout)
    records = [json.loads(line) for line in run.stdout.splitlines()]
    summaries = []
    for case in sorted({r["case"] for r in records[1:]}):
        row = {"case": case}
        for label in ["main", "mcp"]:
            old = statistics.median(r["us"] for r in records[1:] if r["case"] == case and r["function"] == "before_" + label)
            new = statistics.median(r["us"] for r in records[1:] if r["case"] == case and r["function"] == "after_" + label)
            row[label] = {"before_us": old, "after_us": new, "reduction_percent": 100 * (1 - new / old)}
        summaries.append(row)
    asan = out / "asan"
    subprocess.run(command + ["-O1", "-g", "-fsanitize=address,undefined", "-o", str(asan)], check=True, timeout=30)
    checked = subprocess.run([str(asan), "check"], capture_output=True, text=True, check=True, timeout=30)
    (out / "asan.stderr.txt").write_text(checked.stderr)
    result = {"scope": "production escaping functions; buffered FILE sink; not tool latency",
              "compiler": command, "function_sha256": hashes, "checks": records[0],
              "asan_ubsan_passed": True, "timings": summaries}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
