/* Persistent offline corpus runner. Real tools gate, compactor, serializer and
 * SQLite broker; input/output paths belong to an isolated private workspace. */
#include "llm.h"
#include "tools.h"
#include "context_archive.h"
#include "json_util.h"
#include "vm.h"
#include "chronicle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The CLI entry point normally owns these inert runtime defaults. */
int g_cheap_mode=0;
vm_t g_vm;

static bool write_history(conversation_t *c, const char *path) {
    char *json = c->count ? context_archive_serialize(c->msgs,c->count) : strdup("[]");
    if (!json) return false;
    FILE *f=fopen(path,"w");
    bool ok=f && fwrite(json,1,strlen(json),f)==strlen(json);
    if (f && fclose(f)) ok=false;
    free(json);return ok;
}

int main(int argc,char **argv) {
    if(argc!=2) return 2;
    vm_init(&g_vm);
    if(!chronicle_start(&(chronicle_start_opts_t){.provider="offline-corpus",.model="none",.mode="metadata"})) return 3;
    char path[4096],out[4096],receipt[8192],second[8192];
    while(fgets(path,sizeof(path),stdin)) {
        path[strcspn(path,"\r\n")]=0;
        conversation_t c,loaded;conv_init(&c);conv_init(&loaded);
        session_state_t s={.trust_tier=DSCO_TRUST_TRUSTED};
        bool load=conv_load_ex(&c,NULL,path),ok=false,reload=false,stable=false,io=load,second_ok=false;
        receipt[0]=second[0]=0;
        if(load) {
            snprintf(out,sizeof(out),"%s/before.json",argv[1]);io=write_history(&c,out);
            tools_set_active_conversation(&c);tools_set_active_session(&s);
            if(io) ok=tools_execute_for_tier("context_compact","{\"aggressive\":true,\"keep_recent\":2}","trusted",receipt,sizeof(receipt));
            snprintf(out,sizeof(out),"%s/after.json",argv[1]);if(io) io=write_history(&c,out);
            snprintf(out,sizeof(out),"%s/saved.json",argv[1]);if(io) io=conv_save_ex(&c,NULL,out);
            if(io) reload=conv_load_ex(&loaded,NULL,out);
            snprintf(out,sizeof(out),"%s/reloaded.json",argv[1]);if(reload) reload=write_history(&loaded,out);
            if(io && reload) {
                tools_set_active_conversation(&loaded);
                second_ok=tools_execute_for_tier("context_compact","{\"aggressive\":true,\"keep_recent\":2}","trusted",second,sizeof(second));
                stable=!json_get_bool(second,"changed",true);
                snprintf(out,sizeof(out),"%s/second.json",argv[1]);io=write_history(&loaded,out);
            }
        }
        printf("{\"load\":%s,\"ok\":%s,\"io\":%s,\"reload\":%s,\"stable\":%s,\"second_ok\":%s,\"receipt\":%s,\"second_receipt\":%s}\n",load?"true":"false",ok?"true":"false",io?"true":"false",reload?"true":"false",stable?"true":"false",second_ok?"true":"false",receipt[0]?receipt:"null",second[0]?second:"null");
        fflush(stdout);tools_set_active_conversation(NULL);tools_set_active_session(NULL);
        conv_free(&c);conv_free(&loaded);
    }
    chronicle_stop();return 0;
}
