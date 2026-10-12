#include "native_workbench_state.h"
#include "native_app.h"
#include "native_windows.h"
#include "../vendor/yyjson.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool persistent;
bool native_workbench_state_start(char *error,size_t cap) {
    const char *path=getenv("DSCO_NATIVE_WORKBENCH_STATE");char fallback[PATH_MAX];
    if(path && !strcmp(path,":memory:")){native_apps_enable();persistent=false;return true;}
    if(!path || !*path){const char *home=getenv("HOME");if(!home||snprintf(fallback,sizeof(fallback),"%s/.dsco/native-workbench.json",home)>=(int)sizeof(fallback)){snprintf(error,cap,"workbench_state_home_invalid");return false;}path=fallback;}
    persistent=native_app_store_open(path,error,cap);return persistent;
}
bool native_workbench_state_checkpoint(char *error,size_t cap) {
    native_windows_sync_apps();
    if(!persistent){if(error&&cap)error[0]=0;return true;}
    return native_app_store_checkpoint(error,cap);
}
void native_workbench_state_close(void){native_app_store_close();native_apps_disable();persistent=false;}
void native_workbench_state_restore(void) {
    native_app_t *apps=calloc(NATIVE_APPS_MAX,sizeof(*apps));char *out=malloc(262144);
    if(apps&&out){int count=native_apps_snapshot(apps,NATIVE_APPS_MAX);for(int i=0;i<count;i++)if(apps[i].view_open)native_windows_reopen_app(apps[i].id,out,262144);}
    free(apps);free(out);
}
bool native_workbench_state_command(const char *json,char *out,size_t cap) {
    yyjson_doc *d=yyjson_read(json,strlen(json),0);if(!d)return false;
    yyjson_val *o=yyjson_doc_get_root(d),*action=yyjson_obj_get(o,"action");
    bool list=yyjson_equals_str(action,"apps"),reopen=yyjson_equals_str(action,"reopen"),save=yyjson_equals_str(action,"checkpoint");
    if(!list&&!reopen&&!save){yyjson_doc_free(d);return false;}
    bool valid=yyjson_is_obj(o)&&yyjson_obj_size(o)==(reopen?2:1);
    if(reopen){yyjson_val *id=yyjson_obj_get(o,"app_id");valid=valid&&yyjson_is_uint(id)&&yyjson_get_uint(id)>0;}
    if(!valid)snprintf(out,cap,"{\"ok\":false,\"error\":\"invalid_application_command\"}");
    else if(reopen)native_windows_reopen_app(yyjson_get_uint(yyjson_obj_get(o,"app_id")),out,cap);
    else if(save){char err[128];bool ok=native_workbench_state_checkpoint(err,sizeof(err));snprintf(out,cap,"{\"ok\":%s,\"durable\":%s,\"error\":\"%s\"}",ok?"true":"false",persistent&&ok?"true":"false",err);}
    else {
        native_windows_sync_apps();char *state=native_apps_export(NULL);
        if(!state || strlen(state)+80>=cap)snprintf(out,cap,"{\"ok\":false,\"error\":\"application_reply_capacity\"}");
        else snprintf(out,cap,"{\"ok\":true,\"persistence_enabled\":%s,\"state\":%s}",persistent?"true":"false",state);
        free(state);
    }
    yyjson_doc_free(d);return true;
}
