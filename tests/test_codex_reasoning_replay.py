#!/usr/bin/env python3
"""Exercise production SSE/conversation/Responses replay offline under sanitizers."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
provider = (ROOT / 'src/provider.c').read_text()
llm = (ROOT / 'src/llm.c').read_text()


def function(source, name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}\n', match.start()) + 3]


HEAD = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "config.h"
#include "provider.h"
#include "json_util.h"
#include "tool_grounding.h"
#define CHATGPT_REASONING_BLOCK_PREFIX "openai_codex_reasoning:"
static time_t provider_reset_max(time_t a,time_t b){return a>b?a:b;}
time_t provider_credit_reset_at_from_text(const char*t,time_t n){(void)t;return n;}
bool provider_chatgpt_429_is_quota(const char*s){(void)s;return false;}
static bool chatgpt_error_is_transient_rate_limit(const char*s){(void)s;return false;}
static long chatgpt_retry_after_ms_from_text(const char*s){(void)s;return 0;}
static const char *chatgpt_model_id(session_state_t*s){return s->model;}
int g_cheap_mode;
static const char *fixture_custom_prompt;
const char *llm_get_custom_system_prompt(void){return fixture_custom_prompt;}
static void provider_append_goal_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
static void provider_append_structured_output_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
const char *provider_claude_code_session_id(void){return "fixture-session";}
static bool chatgpt_append_tools(jbuf_t*b,conversation_t*c,session_state_t*s){(void)b;(void)c;(void)s;return false;}
/* Replay ownership is tested without advertised tool schemas. An actual wire
 * manifest reaching this stub is a fixture error, never silently discarded. */
void tool_grounding_append(jbuf_t*b,const char*tools){(void)b;assert(!tools||!*tools);}
static bool openai_parallel_tool_calls_enabled_for_provider(const char*p){(void)p;return true;}
const char *codex_cache_default_effort(const char*m){(void)m;return "high";}
const char *codex_cache_default_verbosity(const char*m){(void)m;return NULL;}
const char *dcr_reasoning_effort_normalize(const char*p,const char*m,const char*e,char*b,size_t n){(void)p;(void)m;(void)b;(void)n;return e;}
static _Thread_local char s_chatgpt_prompt_cache_key[128];
void dsco_strip_terminal_controls_inplace(char*s){(void)s;}
'''

MAIN = r'''
static void init(chatgpt_sse_state_t*s,const char*model){
 memset(s,0,sizeof(*s));jbuf_init(&s->line_buf,32);jbuf_init(&s->raw_body,32);
 jbuf_init(&s->text_accum,32);jbuf_init(&s->reasoning_accum,32);
 s->reasoning_model=model?safe_strdup(model):NULL;
}
static void cleanup(chatgpt_sse_state_t*s){
 chatgpt_free_output_blocks(s);free(s->reasoning_model);free(s->stop_reason);free(s->error_msg);
 jbuf_free(&s->line_buf);jbuf_free(&s->raw_body);jbuf_free(&s->text_accum);jbuf_free(&s->reasoning_accum);
}
static void item(chatgpt_sse_state_t*s,const char*kind,const char*raw){
 jbuf_t b;jbuf_init(&b,256);jbuf_append(&b,"data: {\"type\":");jbuf_append_json_str(&b,kind);
 jbuf_append(&b,",\"item\":");jbuf_append(&b,raw);jbuf_append(&b,"}");
 chatgpt_sse_process_line(s,b.data);jbuf_free(&b);
}
int main(void){
 const char *opaque="{\"id\":\"rs_fixture\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"opaque+/=\\\"escaped\"}";
 const char *tool="{\"type\":\"function_call\",\"name\":\"bash\",\"call_id\":\"call_fixture\",\"arguments\":\"{\\\"command\\\":\\\"pwd\\\"}\"}";
 chatgpt_sse_state_t state;init(&state,"fixture-model");
 state.trace_events_enabled=true;
 chatgpt_sse_process_line(&state,"data: {\"type\":\"response.created\"}");
 chatgpt_sse_process_line(&state,"data: {\"type\":\"response.in_progress\"}");
 chatgpt_sse_process_line(&state,"data: {\"type\":\"unrecognized-fixture-event\"}");
 assert(state.trace_events[0]==1&&state.trace_events[1]==1&&state.trace_events[9]==1);
 item(&state,"response.output_item.added",opaque);assert(state.output_block_count==0);
 item(&state,"response.output_item.done",opaque);
 item(&state,"response.output_item.done",opaque);assert(state.output_block_count==1);
 item(&state,"response.output_item.done",tool);
 chatgpt_sse_process_line(&state,"data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":7,\"output_tokens\":3}}}");
 assert(state.output_block_count==2 && state.tool_block_count==1);
 assert(state.trace_events[5]==1&&state.trace_events[6]==3&&state.trace_events[7]==1);
 parsed_response_t parsed={0};chatgpt_take_parsed_response(&state,&parsed);
 assert(!strcmp(parsed.stop_reason,"tool_use") && parsed.count==2);
 assert(!strcmp(parsed.blocks[0].text,opaque));
 conversation_t conv,copy;conv_init(&conv);conv_init(&copy);
 conv_add_assistant_raw(&conv,&parsed);conv_add_assistant_raw(&copy,&parsed);
 json_free_response(&parsed);cleanup(&state);
 assert(!strcmp(conv.msgs[0].content[0].text,opaque));
 assert(conv.msgs[0].content[0].text!=copy.msgs[0].content[0].text);
 assert(!content_block_is_sendable(&conv.msgs[0].content[0])); /* Anthropic isolation */
 conv_add_tool_result_named(&conv,"call_fixture","bash","fixture-result",false);
 session_state_t session={0};snprintf(session.model,sizeof(session.model),"fixture-model");
 snprintf(session.effort,sizeof(session.effort),"high");
 snprintf(session.prompt_cache_key,sizeof(session.prompt_cache_key),"stable-fixture-key");
 size_t full_request_bytes=0,cheap_request_bytes=0;
 for(int custom=0;custom<2;custom++)for(int cheap=0;cheap<2;cheap++){
  fixture_custom_prompt=custom?"fixture custom instructions":NULL;g_cheap_mode=cheap;
  char *profile_request=chatgpt_native_build_request(NULL,&conv,&session,100,NULL);
  char *instructions=json_get_str(profile_request,"instructions");
  jbuf_t expected;jbuf_init(&expected,1024);
  if(custom)jbuf_append(&expected,"fixture custom instructions\n\n");
  jbuf_append(&expected,cheap?SYSTEM_PROMPT_CHEAP:SYSTEM_PROMPT);
  assert(instructions&&!strcmp(instructions,expected.data));
  assert(strstr(profile_request,"\"reasoning\":{\"effort\":\"high\"}"));
  assert(strstr(profile_request,opaque)&&strstr(profile_request,"fixture-result"));
  if(!custom){if(cheap)cheap_request_bytes=strlen(profile_request);else full_request_bytes=strlen(profile_request);}
  free(instructions);free(profile_request);jbuf_free(&expected);
 }
 assert(cheap_request_bytes<full_request_bytes);
 printf("PASS: native Codex prompt profiles/custom prefix; request bytes full=%zu cheap=%zu\n",full_request_bytes,cheap_request_bytes);
 fixture_custom_prompt=NULL;g_cheap_mode=0;
 char *request=chatgpt_native_build_request(NULL,&conv,&session,100,NULL);
 assert(json_is_valid_container(request));
 const char *replayed=strstr(request,opaque);assert(replayed);
 const char *called=strstr(request,"\"type\":\"function_call\"");
 const char *result=strstr(request,"\"type\":\"function_call_output\"");
 assert(called && result && replayed<called && called<result);
 assert(strstr(request,"\"include\":[\"reasoning.encrypted_content\"]"));
 assert(strstr(request,"\"prompt_cache_key\":\"stable-fixture-key\""));
 assert(strstr(request,"\"reasoning\":{\"effort\":\"high\"}"));free(request);
 snprintf(session.model,sizeof(session.model),"different-model");
 request=chatgpt_native_build_request(NULL,&conv,&session,100,NULL);
 assert(!strstr(request,"opaque+/="));assert(strstr(request,"fixture-result"));free(request);
 jbuf_t other;jbuf_init(&other,256);bool first=true;
 chatgpt_append_tool_items(&other,&conv.msgs[0],&first,NULL); /* other Responses provider */
 assert(!strstr(other.data,"opaque+/="));assert(strstr(other.data,"function_call"));jbuf_free(&other);
 conv_free(&conv);assert(!strcmp(copy.msgs[0].content[0].text,opaque));conv_free(&copy);
 /* No plaintext-only item retained; other-provider SSE cannot create Codex state. */
 init(&state,"fixture-model");
 item(&state,"response.output_item.done","{\"type\":\"reasoning\",\"summary\":[{\"text\":\"not encrypted\"}]}");
 assert(state.output_block_count==0);cleanup(&state);
 init(&state,NULL);item(&state,"response.output_item.done",opaque);
 assert(state.output_block_count==0);cleanup(&state);
 /* Failed/abandoned stream owns and frees encrypted and tool blocks. */
 init(&state,"fixture-model");item(&state,"response.output_item.done",opaque);
 item(&state,"response.output_item.done",tool);cleanup(&state);
 puts("PASS: encrypted Codex state survives SSE, deep copies, tool results and native replay; model/provider isolation and ownership checked");
}
'''

start = provider.index('typedef struct {', provider.index('/* ── Responses API SSE parser'))
end = provider.index('} chatgpt_sse_state_t;', start) + len('} chatgpt_sse_state_t;')
parts = [HEAD, provider[start:end]]
for name in ['chatgpt_take_parsed_response', 'chatgpt_free_output_blocks', 'chatgpt_tool_announced',
             'chatgpt_announce_tool', 'chatgpt_import_usage', 'chatgpt_import_response_usage',
             'chatgpt_handle_event', 'chatgpt_sse_process_line', 'chatgpt_append_tool_items',
             'chatgpt_append_message_item', 'chatgpt_native_build_request']:
    parts.append(function(provider, name))
for name in ['conv_init', 'conv_free', 'conv_add', 'msg_add_content', 'conv_add_assistant_raw',
             'conv_add_tool_result_named', 'content_block_is_sendable']:
    parts.append(function(llm, name))
parts.append(MAIN)
with tempfile.TemporaryDirectory(prefix='dsco-reasoning-replay-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text('\n'.join(parts))
    subprocess.run(['cc', '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
                    '-fsanitize=address,undefined', '-g', '-I', str(ROOT / 'include'),
                    str(path / 'test.c'), str(ROOT / 'src/json_util.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
