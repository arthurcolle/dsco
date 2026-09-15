#define _DARWIN_C_SOURCE
#include "native_trace.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool session=true;
static unsigned dispatches;
static char dispatched[256], dispatched_tier[32];
bool pixel_tui_session_active(void) {return session;}
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t cap) {
    assert(!strcmp(name,"ui_trace"));dispatches++;
    snprintf(dispatched,sizeof(dispatched),"%s",input);snprintf(dispatched_tier,sizeof(dispatched_tier),"%s",tier);
    snprintf(out,cap,"{\"ok\":true}");return true;
}
static void pause_ms(int ms) {struct timespec t={.tv_nsec=ms*1000000L};nanosleep(&t,NULL);}
int main(void) {
    char dir[]="/tmp/dsco-trace-test-XXXXXX";assert(mkdtemp(dir));
    char path[512],request[1024],result[8192];snprintf(path,sizeof(path),"%s/timeline.jsonl",dir);
    snprintf(request,sizeof(request),"{\"action\":\"start\",\"duration_ms\":80,\"path\":\"%s\"}",path);
    session=false;assert(!tool_ui_trace(request,result,sizeof(result)));assert(access(path,F_OK));session=true;
    assert(tool_ui_trace(request,result,sizeof(result)));assert(native_trace_active());
    assert(!tool_ui_trace(request,result,sizeof(result)));assert(strstr(result,"trace_already_running"));
    native_trace_component_t c[2]={{.id="header"},{.id="window/1"}};
    c[0].value[NT_VISIBLE]=1;c[0].value[NT_PIXELS]=123;
    native_trace_components(c,2);native_trace_components(c,2); /* Duplicate snapshots produce no deltas. */
    c[0].value[NT_PIXELS]=456;c[1].value[NT_DIRTY]=1;native_trace_components(c,2);
    native_trace_components(c,1);
    native_trace_frame(&(pixel_tui_frame_sample_t){.kind=PIXEL_TUI_FRAME_PATCH,.frame_ms=4.25,.wire_bytes=99});
    for(int i=0;i<100 && native_trace_active();i++)pause_ms(10);
    assert(!native_trace_active());
    native_trace_shutdown();assert(tool_ui_trace("{\"action\":\"status\"}",result,sizeof(result)));
    assert(strstr(result,"\"complete\":true") && strstr(result,"\"write_failed\":false"));
    struct stat st;assert(!stat(path,&st));assert((st.st_mode&0777)==0600);
    FILE *f=fopen(path,"r");assert(f);char *line=NULL;size_t cap=0;unsigned adds=0,changes=0,removes=0,ends=0;
    while(getline(&line,&cap,f)>0) {
        yyjson_doc *d=yyjson_read(line,strlen(line),0);assert(d);
        yyjson_val *o=yyjson_doc_get_root(d);const char *op=yyjson_get_str(yyjson_obj_get(o,"op"));
        if(op) {adds+=!strcmp(op,"add");changes+=!strcmp(op,"change");removes+=!strcmp(op,"remove");}
        if(yyjson_equals_str(yyjson_obj_get(o,"event"),"end")) {
            ends++;assert(yyjson_equals_str(yyjson_obj_get(o,"reason"),"duration"));
            assert(yyjson_get_uint(yyjson_obj_get(o,"frames"))==1);
        }
        yyjson_doc_free(d);
    }
    free(line);fclose(f);assert(adds==2 && changes==2 && removes==1 && ends==1);
    assert(!tool_ui_trace(request,result,sizeof(result)));assert(strstr(result,"cannot_create_new_trace_file"));
    struct stat after;assert(!stat(path,&after) && after.st_size==st.st_size);
    assert(!tool_ui_trace("{\"action\":\"start\",\"duration_ms\":600001}",result,sizeof(result)));
    assert(!tool_ui_trace("{\"action\":\"status\",\"action\":\"start\"}",result,sizeof(result)));
    assert(!tool_ui_trace("{\"action\":\"start\\u0000status\"}",result,sizeof(result)));
    setenv("DSCO_ALLOW_WRITE","relative-directory",1);
    assert(!tool_ui_trace("{\"action\":\"start\"}",result,sizeof(result)));
    assert(strstr(result,"explicit_path_required_for_scoped_write"));unsetenv("DSCO_ALLOW_WRITE");
    assert(native_trace_command(" 10 seconds ","untrusted",result,sizeof(result)));
    assert(strstr(dispatched,"10000") && !strcmp(dispatched_tier,"untrusted"));
    assert(native_trace_command("500ms","trusted",result,sizeof(result)) && strstr(dispatched,"500"));
    assert(native_trace_command("2 minutes","trusted",result,sizeof(result)) && strstr(dispatched,"120000"));
    assert(native_trace_command("status","trusted",result,sizeof(result)) && strstr(dispatched,"status"));
    unsigned calls=dispatches;
    assert(!native_trace_command("1e999s","trusted",result,sizeof(result)) && dispatches==calls);
    assert(!native_trace_command("60 hours","trusted",result,sizeof(result)) && dispatches==calls);
    assert(!native_trace_command("s","trusted",result,sizeof(result)) && dispatches==calls);
    assert(!unlink(path));
    snprintf(request,sizeof(request),"{\"action\":\"start\",\"duration_ms\":10000,\"path\":\"%s\"}",path);
    assert(tool_ui_trace(request,result,sizeof(result)));
    for(int i=0;i<200000 && native_trace_active();i++)native_trace_event("bounded",i,1,2,3);
    assert(!native_trace_active());native_trace_shutdown();
    assert(!stat(path,&st) && st.st_size<=8*1024*1024);
    f=fopen(path,"r");assert(f);assert(!fseek(f,-512,SEEK_END));
    char tail[513]={0};assert(fread(tail,1,512,f)==512);fclose(f);
    assert(strstr(tail,"\"reason\":\"size_limit\""));
    assert(!unlink(path));assert(!rmdir(dir));
    puts("native trace: auto-stop, field deltas, deduplication/removal, frame accounting, private file, no overwrite, validation and governed duration dispatch passed");
}
