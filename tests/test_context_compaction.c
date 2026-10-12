#include "context_compaction.h"
#include "json_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Test orchestration with a controlled governed archival sink. Real broker
 * archival/readback/recall is exercised by the binary and eviction fixtures. */
static conversation_t *active;
static int calls, mode;
bool tools_execute_for_tier(const char *name,const char *args,const char *tier,char *out,size_t len) {
    assert(!strcmp(name,"context_evict") && !strcmp(tier,"trusted"));
    assert(json_get_int(args,"keep_recent",0)==2 && json_get_int(args,"max_turns",0)==64);
    ++calls;
    if(mode==1) { snprintf(out,len,"{\"error\":\"owned-denial\"}"); return false; }
    if(mode==2) {
        free(active->msgs[1].content[0].text);active->msgs[1].content[0].text=strdup("owned archived prefix");
        snprintf(out,len,"{\"changed\":true,\"archive_failures\":1,\"assistant_messages_archived\":1}"); return false;
    }
    int archived=0;
    for(int i=1;i<active->count-2 && archived<64;++i) {
        msg_content_t *block=&active->msgs[i].content[0];
        if(active->msgs[i].role==ROLE_ASSISTANT && block->type && !strcmp(block->type,"text") && block->text && strlen(block->text)>1000) {
            free(block->text);block->text=strdup("[Archived assistant response; key=owned-fixture]"); ++archived;
        }
    }
    snprintf(out,len,"{\"changed\":%s,\"assistant_messages_archived\":%d,\"last_archive_key\":\"owned-fixture\"}",
        archived?"true":"false",archived); return true;
}
static char *large(void) { char *s=malloc(2001);assert(s);memset(s,'x',2000);s[2000]=0;return s; }
int main(void) {
    char report[2048];conversation_t c;conv_init(&c);active=&c;
    conv_add_user_text(&c,"preserved seed");
    char *text=large();for(int i=0;i<700;++i) conv_add_assistant_text(&c,text);free(text);
    conv_add_user_text(&c,"preserved latest instruction");conv_add_assistant_text(&c,"latest evidence");
    assert(context_compaction_run(&c,"{\"aggressive\":true,\"keep_recent\":2}","trusted",report,sizeof(report)));
    assert(calls==12 && json_get_int(report,"assistant_messages_archived",0)==700);
    assert(json_get_bool(report,"changed",false) && json_get_bool(report,"archive_verified",false));
    assert(!strcmp(c.msgs[0].content[0].text,"preserved seed"));
    assert(!strcmp(c.msgs[c.count-2].content[0].text,"preserved latest instruction"));
    assert(!strcmp(c.msgs[c.count-1].content[0].text,"latest evidence"));conv_free(&c);

    conv_init(&c);active=&c;conv_add_user_text(&c,"seed");text=large();conv_add_assistant_text(&c,text);
    conv_add_assistant_tool_use(&c,"owned","read_file","{}");conv_add_tool_result(&c,"owned",text,false);
    conv_add_user_text(&c,"latest user");conv_add_assistant_text(&c,"latest assistant");free(text);
    /* Moderate mode must archive too; no unverified trimming fallback. */
    mode=0;calls=0;
    assert(context_compaction_run(&c,"{\"keep_recent\":2,\"max_result_chars\":100}","trusted",report,sizeof(report)));
    assert(calls==2 && json_get_bool(report,"changed",false));
    assert(json_get_int(report,"preview_limit",0)==100);
    free(c.msgs[1].content[0].text);c.msgs[1].content[0].text=large();
    mode=1;calls=0;
    assert(!context_compaction_run(&c,"{\"aggressive\":true,\"keep_recent\":2}","trusted",report,sizeof(report)));
    assert(calls==1 && !json_get_bool(report,"changed",true));
    assert(strlen(c.msgs[1].content[0].text)==2000 && strlen(c.msgs[3].content[0].text)==2000);
    mode=2;calls=0;
    assert(!context_compaction_run(&c,"{\"aggressive\":true,\"keep_recent\":2}","trusted",report,sizeof(report)));
    assert(calls==1 && json_get_bool(report,"changed",false));
    assert(strlen(c.msgs[3].content[0].text)==2000); /* failure must skip projection */
    assert(!strcmp(c.msgs[2].content[0].type,"tool_use"));
    calls=0;
    assert(!context_compaction_run(&c,"{\"keep_recent\":\"wrong\"}","trusted",report,sizeof(report)) && calls==0);
    assert(!context_compaction_run(&c,"{\"aggressive\":true,\"aggressive\":false}","trusted",report,sizeof(report)) && calls==0);
    assert(!context_compaction_run(&c,"{}","trusted",report,512) && calls==0);
    conv_free(&c);
    puts("PASS compaction orchestration: 700 prose archives across batches, protected tail, denial, partial failure without projection, pre-mutation validation");
}
