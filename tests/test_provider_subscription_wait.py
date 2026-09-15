#!/usr/bin/env python3
"""Compile the production Codex retry wrapper and test gate failure boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

HEAD = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "provider.h"
#include "subscription_gate.h"
volatile int g_interrupted;
static _Thread_local long s_provider_subscription_queue_ms;
static int gate_calls, stream_calls, releases, frees, allocations;
static int fixture_error, retries, allow_count, response_kind;
static bool has_account;
static long cooldown;
char *safe_strdup(const char *s) {char *p=strdup(s);assert(p);allocations++;return p;}
void json_free_response(parsed_response_t *r) {
 if(r->stop_reason){free(r->stop_reason);allocations--;}
 memset(r,0,sizeof(*r));frees++;
}
static long dsco_env_long(const char*n,long d,long lo,long hi){(void)n;(void)lo;(void)hi;return d;}
static int dcr_provider_request_max_retries(const char*n,int d){(void)n;(void)d;return retries;}
static bool openai_oauth_account_id(char*b,size_t z){if(has_account)snprintf(b,z,"fixture-account");return has_account;}
static bool chatgpt_refresh_oauth_after_401(const char**k){(void)k;return false;}
static bool provider_env_truthy(const char*s){(void)s;return false;}
static bool provider_event_retry(const char*a,const char*b,const char*c,long d){(void)a;(void)b;(void)c;(void)d;return true;}
static long chatgpt_retry_delay_clamp(double d){return (long)d;}
bool subscription_gate_acquire(subscription_gate_t*g,const char*scope,
 const volatile int*flag,long*waited){
 assert(flag==&g_interrupted);assert(!g->held&&g->fd==-1);
 assert(!strcmp(scope,has_account?"fixture-account":"fixture-key"));
 gate_calls++;*waited=75;
 if(gate_calls>allow_count){errno=fixture_error;return false;}
 g->held=true;g->fd=123;return true;
}
void subscription_gate_release(subscription_gate_t*g,long ms){assert(g->held);releases++;cooldown=ms;g->held=false;}
static stream_result_t chatgpt_native_stream_once(provider_t*p,const char*k,const char*r,
 stream_text_cb t,stream_tool_start_cb s,stream_tool_arg_delta_cb d,stream_thinking_cb h,void*x){
 (void)p;(void)t;(void)s;(void)d;(void)h;(void)x;
 assert(!strcmp(k,"fixture-key")&&!strcmp(r,"{}"));stream_calls++;
 stream_result_t out={0};
 if(response_kind==1 || (response_kind==4 && stream_calls>1)) {out.ok=true;out.http_status=200;}
 else {out.http_status=response_kind==5?503:429;out.retryable=response_kind!=2&&response_kind!=5;out.parsed.stop_reason=safe_strdup(response_kind==2?"credit_too_low":response_kind==5?"incomplete_stream":"rate_limit_exceeded");}
 return out;
}
'''

MAIN = r'''
static void reset(int error,int budget,int allowed,int response,bool account){
 assert(allocations==0);gate_calls=stream_calls=releases=frees=0;cooldown=-1;
 fixture_error=error;retries=budget;allow_count=allowed;response_kind=response;
 has_account=account;g_interrupted=0;s_provider_subscription_queue_ms=11;
}
static stream_result_t run(void){return chatgpt_native_stream(NULL,"fixture-key","{}",NULL,NULL,NULL,NULL,NULL);}
int main(void){
 int checks=0,budgets[]={-1,0,3,10},errors[]={ETIMEDOUT,EINTR};
 for(int e=0;e<2;e++)for(int b=0;b<4;b++)for(int a=0;a<2;a++){
  reset(errors[e],budgets[b],0,0,a);stream_result_t r=run();
  assert(!r.ok&&!r.retryable&&r.http_status==0);
  assert(!strcmp(r.parsed.stop_reason,e?"interrupted":"subscription_queue_timeout"));
  assert(gate_calls==1&&stream_calls==0&&releases==0);
  assert(s_provider_subscription_queue_ms==86);
  json_free_response(&r.parsed);assert(allocations==0);checks++;
 }
 for(int e=0;e<2;e++){
  reset(errors[e],3,1,0,true);stream_result_t r=run();
  assert(!r.ok&&!r.retryable&&r.http_status==0);
  assert(!strcmp(r.parsed.stop_reason,e?"interrupted":"subscription_queue_timeout"));
  assert(gate_calls==2&&stream_calls==1&&releases==1&&cooldown==1000);
  assert(s_provider_subscription_queue_ms==161&&frees==1);
  json_free_response(&r.parsed);assert(allocations==0);checks++;
 }
 reset(ETIMEDOUT,3,1,1,true);stream_result_t r=run();
 assert(r.ok&&gate_calls==1&&stream_calls==1&&releases==1&&cooldown==0);checks++;
 reset(ETIMEDOUT,3,1,2,true);r=run();
 assert(!r.ok&&!strcmp(r.parsed.stop_reason,"credit_too_low"));
 assert(gate_calls==1&&stream_calls==1&&releases==1&&cooldown==0);
 json_free_response(&r.parsed);checks++;
 reset(ETIMEDOUT,0,1,0,true);r=run();
 assert(!r.ok&&r.http_status==429&&gate_calls==1&&stream_calls==1&&releases==1);
 json_free_response(&r.parsed);checks++;
 reset(ETIMEDOUT,3,2,4,true);r=run();
 assert(r.ok&&gate_calls==2&&stream_calls==2&&releases==2&&cooldown==0);
 assert(frees==1&&allocations==0);checks++;
 reset(ETIMEDOUT,3,3,5,true);r=run();
 assert(!r.ok&&r.http_status==503&&!r.retryable);
 assert(gate_calls==1&&stream_calls==1&&releases==1&&cooldown==0);
 json_free_response(&r.parsed);assert(allocations==0);checks++;
 printf("{\"passed\":true,\"scenarios\":%d}\n",checks);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", type=Path, default=ROOT / "src/provider.c")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.provider.read_text()
    match = re.search(r'^static stream_result_t chatgpt_native_stream\([^;{]*\)\s*\{', source, re.M)
    assert match, "production wrapper not found"
    wrapper = source[match.start():source.index('\n}\n', match.start()) + 3]
    assert "subscription_queue_timeout" in wrapper, "gate timeout branch missing"
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output / "provider_gate_wrapper.c"
    fixture.write_text(HEAD + wrapper + MAIN)
    executable = args.output / "provider_gate_wrapper"
    command = ["cc", "-std=c11", "-D_DARWIN_C_SOURCE", "-g", "-O1",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I", str(ROOT / "include"), str(fixture), "-o", str(executable)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    cp = subprocess.run([str(executable.resolve())], capture_output=True, text=True, timeout=10)
    (args.output / "stderr.txt").write_text(cp.stderr)
    (args.output / "stdout.txt").write_text(cp.stdout)
    assert cp.returncode == 0, cp.stderr
    assert "ChatGPT subscription queue timed out after 0.07s (DSCO_CHATGPT_GATE_MAX_WAIT_MS)" in cp.stderr
    assert "ChatGPT subscription queue interrupted after 0.07s" in cp.stderr
    result = json.loads(cp.stdout)
    result.update({"provider_sha256": hashlib.sha256(source.encode()).hexdigest(),
                   "wrapper_sha256": hashlib.sha256(wrapper.encode()).hexdigest(),
                   "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
                   "compile_command": command, "sanitizers": ["address", "undefined"],
                   "scope": "actual production retry wrapper; stubbed gate and native stream"})
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
