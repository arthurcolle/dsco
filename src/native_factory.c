#include "native_factory.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static native_factory_t factories[NATIVE_FACTORY_MAX];
static int count;
static void defaults(void) {
    if(count)return;
    factories[0]=(native_factory_t){.name="json",.title="JSON inspector",.executor=NATIVE_EXEC_JSON};
    factories[1]=(native_factory_t){.name="sha256",.title="Text fingerprint",.executor=NATIVE_EXEC_SHA256};
    factories[2]=(native_factory_t){.name="stats",.title="Text statistics",.executor=NATIVE_EXEC_TEXT_STATS};
    count=3;
}
static bool valid_name(const char *s) {
    if(!s || !*s || strnlen(s,48)>=48)return false;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'))return false;
    return true;
}
void native_factory_reset(void){pthread_mutex_lock(&lock);count=0;defaults();pthread_mutex_unlock(&lock);}
bool native_factory_define(const char *name,const char *title,const char *executor) {
    if(!valid_name(name)||!title||!*title||strnlen(title,96)>=96||!executor)return false;
    for(const unsigned char *p=(const unsigned char*)title;*p;p++)if(*p<32||*p==127)return false;
    native_executor_kind_t kind=!strcmp(executor,"json")?NATIVE_EXEC_JSON:!strcmp(executor,"sha256")?NATIVE_EXEC_SHA256:!strcmp(executor,"stats")?NATIVE_EXEC_TEXT_STATS:0;
    if(!kind)return false;
    pthread_mutex_lock(&lock);defaults();bool ok=count<NATIVE_FACTORY_MAX;
    for(int i=0;i<count;i++)if(!strcmp(factories[i].name,name))ok=false;
    if(ok){native_factory_t *f=&factories[count++];memset(f,0,sizeof(*f));snprintf(f->name,sizeof(f->name),"%s",name);snprintf(f->title,sizeof(f->title),"%s",title);f->executor=kind;}
    pthread_mutex_unlock(&lock);return ok;
}
bool native_factory_lookup(const char *name,native_factory_t *out) {
    if(!name||!out)return false;
    pthread_mutex_lock(&lock);defaults();bool ok=false;
    for(int i=0;i<count;i++)if(!strcmp(factories[i].name,name)){*out=factories[i];ok=true;break;}
    pthread_mutex_unlock(&lock);return ok;
}
bool native_factory_execute(const native_factory_t *f,const char *input,native_factory_result_t *out) {
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(!f||!input||strnlen(input,NATIVE_FACTORY_INPUT_MAX)>=NATIVE_FACTORY_INPUT_MAX){snprintf(out->output,sizeof(out->output),"{\"ok\":false,\"error\":\"invalid_or_oversized_input\"}");return false;}
    size_t len=strlen(input);struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    sha256_hex((const unsigned char*)input,len,out->input_sha256);
    if(f->executor==NATIVE_EXEC_SHA256){out->ok=true;snprintf(out->output,sizeof(out->output),"{\"ok\":true,\"bytes\":%zu,\"sha256\":\"%s\"}",len,out->input_sha256);}
    else if(f->executor==NATIVE_EXEC_TEXT_STATS){
        size_t nl=0,tabs=0,ascii=0;
        for(size_t i=0;i<len;i++){nl+=input[i]=='\n';tabs+=input[i]=='\t';ascii+=(unsigned char)input[i]<128;}
        out->ok=true;snprintf(out->output,sizeof(out->output),"{\"ok\":true,\"bytes\":%zu,\"newline_bytes\":%zu,\"tab_bytes\":%zu,\"ascii_bytes\":%zu}",len,nl,tabs,ascii);
    } else if(f->executor==NATIVE_EXEC_JSON){
        yyjson_read_err err={0};yyjson_doc *doc=yyjson_read_opts((char*)input,len,0,NULL,&err);
        if(!doc)snprintf(out->output,sizeof(out->output),"{\"ok\":false,\"error\":\"invalid_json\",\"byte_offset\":%zu}",err.pos);
        else {
            yyjson_val *root=yyjson_doc_get_root(doc);
            const char *type=yyjson_is_obj(root)?"object":yyjson_is_arr(root)?"array":yyjson_is_str(root)?"string":yyjson_is_bool(root)?"boolean":yyjson_is_null(root)?"null":"number";
            size_t members=yyjson_is_obj(root)?yyjson_obj_size(root):yyjson_is_arr(root)?yyjson_arr_size(root):0;
            out->ok=true;snprintf(out->output,sizeof(out->output),"{\"ok\":true,\"bytes\":%zu,\"root_type\":\"%s\",\"root_members\":%zu,\"values\":%zu}",len,type,members,yyjson_doc_get_val_count(doc));yyjson_doc_free(doc);
        }
    } else snprintf(out->output,sizeof(out->output),"{\"ok\":false,\"error\":\"unsupported_executor\"}");
    clock_gettime(CLOCK_MONOTONIC,&b);int64_t ns=(b.tv_sec-a.tv_sec)*1000000000LL+b.tv_nsec-a.tv_nsec;out->elapsed_us=ns>0?(uint64_t)ns/1000:0;
    return out->ok;
}
