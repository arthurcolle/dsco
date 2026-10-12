#define _DARWIN_C_SOURCE 1
#include "native_app.h"
#include "native_windows.h"
#include "native_workbench_state.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char reply[262144];
static uint64_t command(const char *s) {
    CHECK(native_windows_command(s,reply,sizeof(reply)));
    yyjson_doc*d=yyjson_read(reply,strlen(reply),0);CHECK(d);uint64_t id=yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d),"id"));yyjson_doc_free(d);return id;
}
static native_window_t window(uint64_t id) {
    native_windows_snapshot_t *s=malloc(sizeof(*s));CHECK(s);native_windows_snapshot(s);native_window_t w={0};
    for(int i=0;i<s->count;i++)if(s->windows[i].id==id)w=s->windows[i];free(s);return w;
}
static void act(const char *action,uint64_t id) {char json[128];snprintf(json,sizeof(json),"{\"action\":\"%s\",\"id\":%llu}",action,(unsigned long long)id);command(json);}
static uint64_t reopen(uint64_t id) {
    CHECK(native_windows_reopen_app(id,reply,sizeof(reply)));yyjson_doc*d=yyjson_read(reply,strlen(reply),0);CHECK(d);
    uint64_t w=yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d),"id"));yyjson_doc_free(d);return w;
}
static void edit(uint64_t id,const char *text) {
    char json[1024];snprintf(json,sizeof(json),"{\"action\":\"update\",\"id\":%llu,\"text\":\"%s\"}",(unsigned long long)id,text);command(json);
}
static native_factory_result_t execute(native_window_task_t *r) {native_factory_result_t result;native_factory_execute(&r->factory,r->input,&result);return result;}
static void malformed(const char *good,const char *key,const char *value) {
    yyjson_doc *d=yyjson_read(good,strlen(good),0);CHECK(d);yyjson_mut_doc *m=yyjson_doc_mut_copy(d,NULL);CHECK(m);
    yyjson_mut_val *root=yyjson_mut_doc_get_root(m),*a=yyjson_mut_arr_get(yyjson_mut_obj_get(root,"apps"),0);
    yyjson_mut_obj_remove_key(a,key);yyjson_mut_obj_add_strcpy(m,a,key,value);
    char *bad=yyjson_mut_write(m,0,NULL);CHECK(bad);CHECK(!native_apps_import(bad,strlen(bad)));free(bad);yyjson_mut_doc_free(m);yyjson_doc_free(d);
}
int main(void) {
    unsetenv("DSCO_ALLOW_WRITE");unsetenv("DSCO_ALLOW_READ");
    native_windows_reset();native_apps_enable();
    uint64_t w=command("{\"action\":\"factory\",\"factory\":\"sha256\",\"text\":\"abc\"}");
    native_window_t v=window(w);uint64_t app=v.app_id;CHECK(app);CHECK(strstr(reply,"app_id"));
    native_window_task_t r;native_app_t a;CHECK(native_windows_submit_task(w));CHECK(native_windows_take_task(&r));
    CHECK(r.app_id==app);act("close",w);CHECK(!window(w).id);CHECK(native_apps_get(app,&a)&&!a.view_open);
    native_factory_result_t result=execute(&r);CHECK(native_windows_complete_task(&r,&result));
    CHECK(native_apps_get(app,&a)&&a.status==NATIVE_APP_DONE&&!a.view_open);CHECK(!strcmp(a.input,"abc"));CHECK(!strcmp(a.result.input_sha256,result.input_sha256));
    uint64_t w2=reopen(app);CHECK(w2!=w);v=window(w2);CHECK(v.status==NATIVE_WINDOW_DONE);CHECK(!strcmp(v.task_output,result.output));CHECK(!strcmp(v.text,"abc"));CHECK(reopen(app)==w2);
    /* Closing/reopening while queued, then changing input fences the old completion. */
    edit(w2,"old");CHECK(native_windows_submit_task(w2));act("close",w2);w2=reopen(app);CHECK(native_windows_take_task(&r));
    edit(w2,"new");result=execute(&r);CHECK(!native_windows_complete_task(&r,&result));CHECK(native_apps_get(app,&a)&&a.status==NATIVE_APP_READY);CHECK(!a.result.output[0]);
    CHECK(native_windows_submit_task(w2));CHECK(native_windows_take_task(&r));result=execute(&r);CHECK(native_windows_complete_task(&r,&result));
    /* Genuine invalid JSON stays a blocked result, not a successful application. */
    uint64_t bad=command("{\"action\":\"factory\",\"factory\":\"json\",\"text\":\"{bad\"}");
    CHECK(native_windows_submit_task(bad));CHECK(native_windows_take_task(&r));result=execute(&r);CHECK(!result.ok);CHECK(native_windows_complete_task(&r,&result));
    CHECK(native_apps_get(window(bad).app_id,&a)&&a.status==NATIVE_APP_BLOCKED);
    CHECK(native_workbench_state_command("{\"action\":\"reopen\",\"app_id\":1,\"shell\":\"anything\"}",reply,sizeof(reply)));CHECK(strstr(reply,"invalid_application_command"));
    CHECK(!native_windows_command("{\"action\":\"update\",\"id\":2,\"kind\":\"workflow\"}",reply,sizeof(reply)));
    /* Strict all-or-nothing import, including duplicate keys and bounded strings. */
    char *good=native_apps_export(NULL);CHECK(good);CHECK(native_apps_import(good,strlen(good)));malformed(good,"executor","bash");malformed(good,"input_sha256","wrong");malformed(good,"status","done");
    const char *duplicate="{\"schema\":\"dsco.native-apps.v1\",\"schema\":\"dsco.native-apps.v1\",\"next_id\":1,\"apps\":[]}";
    char *before=native_apps_export(NULL);CHECK(before);CHECK(!native_apps_import(duplicate,strlen(duplicate)));
    char *after=native_apps_export(NULL);CHECK(after);CHECK(!strcmp(before,after));free(before);free(after);free(good);
    /* Real disk checkpoint, exclusive lock and restart with immutable alias binding. */
    char dir[]="/tmp/dsco-app-test-XXXXXX";CHECK(mkdtemp(dir));char path[512],err[128];snprintf(path,sizeof(path),"%s/state.json",dir);
    native_windows_reset();native_apps_disable();CHECK(native_app_store_open(path,err,sizeof(err)));
    CHECK(native_factory_define("fingerprint","Owned fingerprint","sha256"));
    w=command("{\"action\":\"factory\",\"factory\":\"fingerprint\",\"text\":\"saved λ\"}");app=window(w).app_id;
    CHECK(native_windows_submit_task(w));CHECK(native_windows_take_task(&r));result=execute(&r);CHECK(native_windows_complete_task(&r,&result));
    CHECK(native_app_store_checkpoint(err,sizeof(err)));struct stat st;CHECK(stat(path,&st)==0&&(st.st_mode&0777)==0600);
    pid_t pid=fork();CHECK(pid>=0);if(!pid){native_app_store_close();_exit(native_app_store_open(path,err,sizeof(err))?1:0);}int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    native_app_store_close();native_windows_reset();native_apps_disable();CHECK(native_app_store_open(path,err,sizeof(err)));
    CHECK(native_apps_get(app,&a)&&a.status==NATIVE_APP_DONE);CHECK(!strcmp(a.input,"saved λ"));CHECK(!strcmp(a.result.output,result.output));
    w=reopen(app);CHECK(native_windows_submit_task(w));CHECK(native_windows_take_task(&r));CHECK(r.factory.executor==NATIVE_EXEC_SHA256);CHECK(!strcmp(r.factory.name,"fingerprint"));
    /* A saved in-flight job restores interrupted, never replayed automatically. */
    CHECK(native_app_store_checkpoint(err,sizeof(err)));native_app_store_close();native_windows_reset();native_apps_disable();CHECK(native_app_store_open(path,err,sizeof(err)));
    CHECK(native_apps_get(app,&a)&&a.status==NATIVE_APP_INTERRUPTED);CHECK(!native_windows_take_task(&r));w=reopen(app);CHECK(window(w).status==NATIVE_WINDOW_BLOCKED);
    /* A symlink replacement cannot redirect writes, and failed checkpoints retain memory. */
    CHECK(native_app_store_checkpoint(err,sizeof(err))); /* unchanged revision must still validate destination */
    CHECK(unlink(path)==0);CHECK(symlink("/dev/null",path)==0);CHECK(!native_app_store_checkpoint(err,sizeof(err)));CHECK(native_apps_get(app,&a));CHECK(unlink(path)==0);CHECK(native_app_store_checkpoint(err,sizeof(err)));
    CHECK(unlink(path)==0);CHECK(native_app_store_checkpoint(err,sizeof(err)));CHECK(stat(path,&st)==0);
    native_app_store_close();FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("not JSON",f)>=0);CHECK(!fclose(f));CHECK(!native_app_store_open(path,err,sizeof(err)));CHECK(strstr(err,"invalid"));
    f=fopen(path,"r");CHECK(f);char bytes[32]={0};CHECK(fread(bytes,1,8,f)==8);fclose(f);CHECK(!strcmp(bytes,"not JSON"));
    CHECK(unlink(path)==0);setenv("DSCO_ALLOW_WRITE","0",1);CHECK(!native_app_store_open(path,err,sizeof(err)));CHECK(access(path,F_OK)!=0);unsetenv("DSCO_ALLOW_WRITE");
    /* Detached records are bounded independently of the 12 view slots. */
    native_windows_reset();native_apps_enable();
    for(int i=0;i<NATIVE_APPS_MAX;i++){w=command("{\"action\":\"factory\",\"factory\":\"stats\",\"text\":\"bounded\"}");act("close",w);}
    CHECK(native_apps_snapshot(NULL,0)==NATIVE_APPS_MAX);CHECK(!native_windows_command("{\"action\":\"factory\",\"factory\":\"stats\"}",reply,sizeof(reply)));CHECK(strstr(reply,"application_limit"));
    native_apps_disable();native_windows_reset();char lock[544];snprintf(lock,sizeof(lock),"%s.lock",path);unlink(lock);CHECK(rmdir(dir)==0);
    printf("native durable apps: %d checks passed\n",checks);return 0;
}
