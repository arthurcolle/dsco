#include "jev_router.h"
#include "router.h"
#include "config.h"
#include "capability.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int calls,notes;static bool secrets=true;static int mode;static char first_model[128];
const model_info_t *openrouter_cache_lookup(const char *s){(void)s;return NULL;}
const model_info_t *codex_cache_lookup(const char *s){(void)s;return NULL;}
const char *provider_detect(const char *m,const char *k){(void)k;return strstr(m,"claude")?"anthropic":"openai";}
const char *provider_route_for_model(const char *m,const char *k,const char *pin){return pin?pin:provider_detect(m,k);}
bool provider_model_is_routable(const char *m,const char *key,const char *pin,const char **out){*out=provider_route_for_model(m,key,pin);return true;}
bool dsco_cap_granted(dsco_cap_t cap,const char *tier){if(cap!=CAP_SECRETS || strcmp(tier,"test-tier"))abort();return secrets;}
void dsco_flow_note(unsigned cap){if(cap!=CAP_SECRETS)abort();notes++;}
static void require(bool x,const char *s){if(!x){fprintf(stderr,"FAIL %s mode=%d\n",s,mode);exit(1);}}
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t cap){
 require(!strcmp(name,"curl_raw") && !strcmp(tier,"test-tier"),"governed transport and caller tier");calls++;
 require(notes==calls && !strstr(input,"SECRET_SENTINEL"),"credential not leaked to arguments");
 yyjson_doc *d=yyjson_read(input,strlen(input),0);require(d!=NULL,"argument JSON");
 const char *args=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d),"args"));
 require(args && strstr(args,"--max-time 2") && strstr(args,"--variable %TYPESAFE_API_KEY") && !strstr(args,"--location"),"bounded no-redirect transport");
 const char *start=strstr(args,"--data-raw '");require(start!=NULL,"quoted body");start+=12;
 char body[8192];size_t n=0;const char *c=start;
 while(*c && n+1<sizeof(body)){
  if(!strncmp(c,"'\\''",4)){body[n++]='\'';c+=4;continue;}
  if(*c=='\'')break;
  body[n++]=*c++;
 }
 body[n]=0;require(!strcmp(c,"' https://api.typesafe.ai/v1/systemone"),"only fixed API destination");
 yyjson_doc *b=yyjson_read(body,n,0);require(b!=NULL,"shell-quoted payload round trips");
 yyjson_val *root=yyjson_doc_get_root(b),*state=yyjson_obj_get(root,"state"),*list=yyjson_obj_get(state,"candidates");
 size_t count=yyjson_arr_size(list);require(count>=2 && count<=6,"bounded shortlist");
 require(yyjson_obj_size(state)==3,"no history/tool-result payload");
 const char *m=yyjson_get_str(yyjson_obj_get(yyjson_arr_get(list,0),"model"));snprintf(first_model,sizeof(first_model),"%s",m);
 if(mode==8)require(strstr(yyjson_get_str(yyjson_obj_get(state,"task")),"don't $(touch /tmp/NO)")!=NULL,"apostrophe/metacharacters remain task data");
 jbuf_t r;jbuf_init(&r,2048);
 jbuf_appendf(&r,"{\"model\":\"%s\",\"usage\":{\"input_tokens\":%s},\"answers\":{\"route\":{\"type\":\"choice\",\"choice\":\"%s\",\"confidence\":%s,\"probabilities\":{",mode==2?"wrong-model":JEV_ROUTER_MODEL,mode==7?"true":"900",mode==1?"stay":mode==3?"arbitrary-shell":"c0",mode==4?"true":"0.9");
 for(size_t i=0;i<count;i++)jbuf_appendf(&r,"%s\"c%zu\":%.8f",i?",":"",i,mode==1?(.1/count):(i==0?.9:.1/count));
 jbuf_appendf(&r,",\"stay\":%.8f}}}}",mode==1?.9:.1/count);
 snprintf(out,cap,"%s",mode==5?"malformed":r.data);jbuf_free(&r);yyjson_doc_free(b);yyjson_doc_free(d);
 return mode!=6;
}
static void scenario(int which){
 mode=which;calls=notes=0;setenv("TYPESAFE_API_KEY","SECRET_SENTINEL",1);
 jev_router_candidate_t p[2]={{.model="cheap",.tier=1,.input_price=1,.output_price=5,.latency_ms=100},{.model="strong",.tier=4,.input_price=5,.output_price=25,.latency_ms=1000}};
 char out[1024];int pick=-1;
 if(which==9)secrets=false;
 if(which==10)unsetenv("TYPESAFE_API_KEY");
 const char *task=which==8?"Please don't $(touch /tmp/NO); classify only.":"Explain a hash table.";
 bool ok=jev_router_pick(task,"current",p,2,&pick,out,sizeof(out),"test-tier");
 require(ok==(which==0 || which==1 || which==8),"expected decision validation");
 if(ok)require(pick==(which==1?-1:0),"choice mapped to existing slot");
 require(calls==(which==9 || which==10?0:1),"no retries and pre-credential denial");
 yyjson_doc *json=yyjson_read(out,strlen(out),0);require(json!=NULL,"receipt JSON");yyjson_doc_free(json);
 if(which==0){
  for(int i=1;i<32;i++)require(jev_router_pick(task,"current",p,2,&pick,out,sizeof(out),"test-tier"),"bounded attempt admitted");
  require(!jev_router_pick(task,"current",p,2,&pick,out,sizeof(out),"test-tier") && calls==32 && strstr(out,"attempt_limit"),"process attempt ceiling");
 }
}
static void routing(void){
 router_t r;router_init(&r,ROUTER_POLICY_JEV);char out[8192];mode=0;setenv("TYPESAFE_API_KEY","SECRET_SENTINEL",1);
 router_decision_t d=router_decide_for_task(&r,"Explain a hash table.","claude-sonnet-5",TASK_SIMPLE,0,0,0,true,NULL,"anthropic","test-tier");
 require(!d.should_switch && calls==0,"model lock blocks egress");
 d=router_decide_for_task(&r,"Explain a hash table.","claude-sonnet-5",TASK_SIMPLE,0,0,0,false,NULL,"anthropic","test-tier");
 require(calls==1 && !strcmp(d.model_id,first_model) && strstr(d.model_id,"claude"),"Jev picks actual provider-eligible model");
 router_to_json(&r,out,sizeof(out));require(strstr(out,"payload_sha256") && strstr(out,"\"jev\":"),"status contains receipt");
 unsetenv("TYPESAFE_API_KEY");
 d=router_decide_for_task(&r,"Explain a hash table.","claude-sonnet-5",TASK_SIMPLE,0,0,0,false,NULL,"anthropic","test-tier");
 require(!d.should_switch || !strcmp(provider_detect(d.model_id,NULL),"anthropic"),"fallback preserves provider pin");
 r.policy=ROUTER_POLICY_FIXED;
 d=router_decide_for_task(&r,"Explain a hash table.","claude-sonnet-5",TASK_SIMPLE,0,0,0,false,NULL,NULL,"test-tier");
 require(!d.should_switch && calls==1,"fixed mode sends no Jev call");router_destroy(&r);
}
int main(void){
 int cases=0;
 for(int i=0;i<220;i++){
  pid_t pid=fork();require(pid>=0,"fork owned isolated test");
  if(!pid){scenario(i%11);_exit(0);}int status=0;require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,"isolated response scenario");cases++;
 }
 routing();printf("jev_router: %d isolated validation scenarios plus native selection/lock/pin/fallback/status checks passed\n",cases);return 0;
}
