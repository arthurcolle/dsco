#include "native_app.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID_MAX UINT64_C(9007199254740991)
static pthread_mutex_t app_mu=PTHREAD_MUTEX_INITIALIZER;
static native_app_t apps[NATIVE_APPS_MAX];
static int count;
static uint64_t next_id=1,revision=1;
static bool enabled;
static native_app_t *find(uint64_t id) {
    for(int i=0;i<count;i++)if(apps[i].id==id)return &apps[i];
    return NULL;
}
static bool bounded(const char *s,size_t cap) {return s && strnlen(s,cap)<cap;}
static bool valid_factory(const native_factory_t *f) {
    if(!f || !bounded(f->name,sizeof(f->name)) || !f->name[0] ||
       !bounded(f->title,sizeof(f->title)) || f->executor<NATIVE_EXEC_JSON ||
       f->executor>NATIVE_EXEC_TEXT_STATS)return false;
    for(const char *p=f->name;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return false;
    return true;
}
void native_apps_enable(void) {
    pthread_mutex_lock(&app_mu);memset(apps,0,sizeof(apps));count=0;next_id=1;revision++;enabled=true;pthread_mutex_unlock(&app_mu);
}
void native_apps_disable(void) {
    pthread_mutex_lock(&app_mu);enabled=false;memset(apps,0,sizeof(apps));count=0;revision++;pthread_mutex_unlock(&app_mu);
}
bool native_apps_enabled(void) {pthread_mutex_lock(&app_mu);bool on=enabled;pthread_mutex_unlock(&app_mu);return on;}
uint64_t native_apps_create(const native_factory_t *f,const char *title,const char *input,native_ui_rect_t rect) {
    if(!valid_factory(f)||!bounded(title,96)||!bounded(input,NATIVE_FACTORY_INPUT_MAX))return 0;
    pthread_mutex_lock(&app_mu);uint64_t id=0;
    if(enabled && count<NATIVE_APPS_MAX && next_id<APP_ID_MAX) {
        native_app_t *a=&apps[count++];memset(a,0,sizeof(*a));a->id=id=next_id++;a->generation=1;
        a->factory=*f;strcpy(a->title,title);strcpy(a->input,input);a->rect=rect;a->view_open=true;revision++;
    }
    pthread_mutex_unlock(&app_mu);return id;
}
bool native_apps_get(uint64_t id,native_app_t *out) {
    if(!out)return false;
    pthread_mutex_lock(&app_mu);native_app_t *a=enabled?find(id):NULL;if(a)*out=*a;pthread_mutex_unlock(&app_mu);return a!=NULL;
}
int native_apps_snapshot(native_app_t *out,size_t cap) {
    pthread_mutex_lock(&app_mu);int n=enabled?count:0;if(out){if((size_t)n>cap)n=(int)cap;memcpy(out,apps,(size_t)n*sizeof(*apps));}pthread_mutex_unlock(&app_mu);return n;
}
bool native_apps_sync(uint64_t id,const char *title,const char *input,native_ui_rect_t rect,bool open) {
    if(!bounded(title,96)||!bounded(input,NATIVE_FACTORY_INPUT_MAX))return false;
    pthread_mutex_lock(&app_mu);native_app_t *a=enabled?find(id):NULL;
    if(a) {
        bool edited=strcmp(a->input,input)!=0;
        if(edited || strcmp(a->title,title) || memcmp(&a->rect,&rect,sizeof(rect)) || a->view_open!=open) {
            if(edited){a->generation++;a->sequence=0;a->status=NATIVE_APP_READY;memset(&a->result,0,sizeof(a->result));}
            strcpy(a->input,input);strcpy(a->title,title);a->rect=rect;a->view_open=open;revision++;
        }
    }
    pthread_mutex_unlock(&app_mu);return a!=NULL;
}
uint64_t native_apps_begin(uint64_t id,uint64_t sequence) {
    pthread_mutex_lock(&app_mu);native_app_t *a=enabled?find(id):NULL;uint64_t gen=0;
    if(a && sequence){a->sequence=sequence;a->status=NATIVE_APP_RUNNING;memset(&a->result,0,sizeof(a->result));gen=a->generation;revision++;}
    pthread_mutex_unlock(&app_mu);return gen;
}
bool native_apps_complete(uint64_t id,uint64_t sequence,uint64_t generation,const native_factory_result_t *r) {
    if(!r || !bounded(r->output,sizeof(r->output)) || !bounded(r->input_sha256,sizeof(r->input_sha256)))return false;
    pthread_mutex_lock(&app_mu);native_app_t *a=enabled?find(id):NULL;
    bool ok=a && a->status==NATIVE_APP_RUNNING && sequence && a->sequence==sequence && a->generation==generation;
    if(ok){char hash[65];sha256_hex((const unsigned char*)a->input,strlen(a->input),hash);ok=!strcmp(hash,r->input_sha256);}
    if(ok){a->result=*r;a->status=r->ok?NATIVE_APP_DONE:NATIVE_APP_BLOCKED;revision++;}
    pthread_mutex_unlock(&app_mu);return ok;
}
void native_apps_clear(uint64_t id) {
    pthread_mutex_lock(&app_mu);native_app_t *a=enabled?find(id):NULL;
    if(a){a->generation++;a->sequence=0;a->status=NATIVE_APP_READY;memset(&a->result,0,sizeof(a->result));revision++;}pthread_mutex_unlock(&app_mu);
}
void native_apps_interrupt(void) {
    pthread_mutex_lock(&app_mu);
    if(enabled)for(int i=0;i<count;i++)if(apps[i].status==NATIVE_APP_RUNNING){apps[i].status=NATIVE_APP_INTERRUPTED;apps[i].sequence=0;revision++;}
    pthread_mutex_unlock(&app_mu);
}
char *native_apps_export(uint64_t *rev) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return NULL;
    pthread_mutex_lock(&app_mu);
    yyjson_mut_val *root=yyjson_mut_obj(d),*list=yyjson_mut_arr(d);yyjson_mut_doc_set_root(d,root);
    yyjson_mut_obj_add_str(d,root,"schema","dsco.native-apps.v1");
    yyjson_mut_obj_add_uint(d,root,"next_id",next_id);yyjson_mut_obj_add_val(d,root,"apps",list);
    for(int i=0;enabled && i<count;i++) {
        native_app_t *a=&apps[i];yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_arr_add_val(list,v);
        yyjson_mut_obj_add_uint(d,v,"id",a->id);yyjson_mut_obj_add_uint(d,v,"generation",a->generation);
        yyjson_mut_obj_add_strcpy(d,v,"factory",a->factory.name);yyjson_mut_obj_add_strcpy(d,v,"factory_title",a->factory.title);
        yyjson_mut_obj_add_int(d,v,"executor",a->factory.executor);yyjson_mut_obj_add_strcpy(d,v,"title",a->title);
        yyjson_mut_obj_add_strcpy(d,v,"input",a->input);yyjson_mut_obj_add_strcpy(d,v,"output",a->result.output);
        yyjson_mut_obj_add_strcpy(d,v,"input_sha256",a->result.input_sha256);yyjson_mut_obj_add_bool(d,v,"result_ok",a->result.ok);
        yyjson_mut_obj_add_uint(d,v,"elapsed_us",a->result.elapsed_us);yyjson_mut_obj_add_int(d,v,"status",a->status);
        yyjson_mut_obj_add_bool(d,v,"view_open",a->view_open);
        yyjson_mut_obj_add_int(d,v,"x",a->rect.x);yyjson_mut_obj_add_int(d,v,"y",a->rect.y);
        yyjson_mut_obj_add_int(d,v,"width",a->rect.width);yyjson_mut_obj_add_int(d,v,"height",a->rect.height);
    }
    if(rev)*rev=revision;
    /* Strings have been copied into the document; no I/O while app_mu is held. */
    pthread_mutex_unlock(&app_mu);char *json=yyjson_mut_write(d,0,NULL);yyjson_mut_doc_free(d);return json;
}
static bool exact_keys(yyjson_val *obj,const char *const *keys,size_t n) {
    if(!yyjson_is_obj(obj)||yyjson_obj_size(obj)!=n)return false;
    size_t i,max;yyjson_val *k,*v;uint64_t seen=0;
    yyjson_obj_foreach(obj,i,max,k,v){(void)v;size_t j;
        for(j=0;j<n;j++)if(yyjson_equals_str(k,keys[j]))break;
        if(j==n || (seen&(UINT64_C(1)<<j)))return false;
        seen|=UINT64_C(1)<<j;
    }return true;
}
static bool string_field(yyjson_val *o,const char *key,char *out,size_t cap) {
    yyjson_val *v=yyjson_obj_get(o,key);const char *s=yyjson_get_str(v);size_t n=yyjson_get_len(v);
    if(!s||n>=cap||memchr(s,0,n))return false;
    for(size_t i=0;i<n;i++)if(((unsigned char)s[i]<32 && s[i]!='\n' && s[i]!='\t') || (unsigned char)s[i]==127)return false;
    memcpy(out,s,n);out[n]=0;return true;
}
static bool number(yyjson_val *o,const char *key,uint64_t lo,uint64_t hi,uint64_t *out) {
    yyjson_val *v=yyjson_obj_get(o,key);uint64_t n=yyjson_get_uint(v);
    if(!yyjson_is_uint(v)||n<lo||n>hi)return false;*out=n;return true;
}
bool native_apps_import(const char *json,size_t len) {
    if(!json||!len||len>2u*1024u*1024u)return false;
    yyjson_doc *d=yyjson_read(json,len,0);if(!d)return false;
    native_app_t *next=calloc(NATIVE_APPS_MAX,sizeof(*next));if(!next){yyjson_doc_free(d);return false;}
    yyjson_val *root=yyjson_doc_get_root(d),*list=yyjson_obj_get(root,"apps");uint64_t high=0;
    const char *rk[]={"schema","next_id","apps"};
    bool ok=exact_keys(root,rk,3) && yyjson_equals_str(yyjson_obj_get(root,"schema"),"dsco.native-apps.v1") &&
        number(root,"next_id",1,APP_ID_MAX,&high) && yyjson_is_arr(list) && yyjson_arr_size(list)<=NATIVE_APPS_MAX;
    const char *ak[]={"id","generation","factory","factory_title","executor","title","input","output","input_sha256","result_ok","elapsed_us","status","view_open","x","y","width","height"};
    int n=0;size_t i,max;yyjson_val *v;
    if(ok)yyjson_arr_foreach(list,i,max,v) {
        native_app_t *a=&next[n];uint64_t exec=0,status=0,x=0,y=0,w=0,h=0;
        ok=exact_keys(v,ak,17) && number(v,"id",1,high-1,&a->id) && number(v,"generation",1,APP_ID_MAX,&a->generation) &&
            number(v,"executor",NATIVE_EXEC_JSON,NATIVE_EXEC_TEXT_STATS,&exec) && number(v,"status",NATIVE_APP_READY,NATIVE_APP_INTERRUPTED,&status) &&
            number(v,"elapsed_us",0,APP_ID_MAX,&a->result.elapsed_us) && number(v,"x",0,16384,&x) && number(v,"y",0,16384,&y) &&
            number(v,"width",0,16384,&w) && number(v,"height",0,16384,&h) &&
            string_field(v,"factory",a->factory.name,sizeof(a->factory.name)) && string_field(v,"factory_title",a->factory.title,sizeof(a->factory.title)) &&
            string_field(v,"title",a->title,sizeof(a->title)) && string_field(v,"input",a->input,sizeof(a->input)) &&
            string_field(v,"output",a->result.output,sizeof(a->result.output)) && string_field(v,"input_sha256",a->result.input_sha256,sizeof(a->result.input_sha256)) &&
            yyjson_is_bool(yyjson_obj_get(v,"view_open")) && yyjson_is_bool(yyjson_obj_get(v,"result_ok"));
        if(!ok)break;
        a->factory.executor=(native_executor_kind_t)exec;a->status=(native_app_status_t)status;
        a->rect=(native_ui_rect_t){(int)x,(int)y,(int)w,(int)h};a->view_open=yyjson_get_bool(yyjson_obj_get(v,"view_open"));
        a->result.ok=yyjson_get_bool(yyjson_obj_get(v,"result_ok"));
        ok=valid_factory(&a->factory);
        for(int j=0;j<n;j++)if(next[j].id==a->id)ok=false;
        if(a->status==NATIVE_APP_DONE || a->status==NATIVE_APP_BLOCKED) {
            char hash[65];sha256_hex((const unsigned char*)a->input,strlen(a->input),hash);
            if(strcmp(hash,a->result.input_sha256) || !a->result.output[0] || a->result.ok!=(a->status==NATIVE_APP_DONE))ok=false;
        } else if(a->result.output[0] || a->result.input_sha256[0] || a->result.ok || a->result.elapsed_us)ok=false;
        if(a->status==NATIVE_APP_RUNNING)a->status=NATIVE_APP_INTERRUPTED;
        if(!ok)break;n++;
    }
    if(ok){pthread_mutex_lock(&app_mu);memcpy(apps,next,sizeof(apps));count=n;next_id=high;revision++;enabled=true;pthread_mutex_unlock(&app_mu);}
    free(next);yyjson_doc_free(d);return ok;
}
