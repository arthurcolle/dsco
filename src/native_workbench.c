#define _DARWIN_C_SOURCE 1
/* No-model native workbench. Separate entrypoint: no agent_run, providers,
 * credential resolution, generic tool execution or conversation submission. */
#include "native_task_service.h"
#include "native_windows.h"
#include "native_app.h"
#include "native_workbench_state.h"
#include "pixel_tui.h"
#include "pixel_tui_perf.h"
#include "tui.h"
#include "../vendor/yyjson.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static atomic_bool control_stop;
static int control_fd=-1;
static char socket_path[104];
static bool allowed_command(const char *json) {
    yyjson_doc *d=yyjson_read(json,strlen(json),0);
    if(!d)return false;
    yyjson_val *root=yyjson_doc_get_root(d);const char *action=yyjson_get_str(yyjson_obj_get(root,"action"));
    const char *allowed[]={"factory","define_factory","run_task","list","focus","move","resize","tile","cascade","close","hide","show","update",NULL};
    bool ok=false;
    for(int i=0;action&&allowed[i];i++)if(!strcmp(action,allowed[i]))ok=true;
    /* This endpoint cannot create arbitrary workflow/agent windows or bind
     * storage-backed buffers. update can change task text/geometry only. */
    if(ok && !strcmp(action,"update")) {
        size_t i,max;yyjson_val *key,*value;
        yyjson_obj_foreach(root,i,max,key,value) {
            (void)value;const char *k=yyjson_get_str(key);
            if(strcmp(k,"action")&&strcmp(k,"id")&&strcmp(k,"text")&&strcmp(k,"title"))ok=false;
        }
    }
    yyjson_doc_free(d);return ok;
}
static void process_command(const char *json,char *out,size_t cap) {
    if(native_workbench_state_command(json,out,cap)){pixel_tui_session_windows_changed(stdout);return;}
    yyjson_doc *probe=yyjson_read(json,strlen(json),0);
    bool stats=probe && yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(probe),"action"),"stats");
    if(probe)yyjson_doc_free(probe);
    if(stats) {
        native_windows_snapshot_t *snap=malloc(sizeof(*snap));
        if(!snap){snprintf(out,cap,"{\"ok\":false,\"error\":\"allocation_failed\"}");return;}
        native_windows_snapshot(snap);uint64_t hits=0,misses=0;size_t bytes=0;
        pixel_tui_window_cache_stats(&hits,&misses,&bytes);
        pixel_tui_perf_snapshot_t perf=pixel_tui_perf_snapshot();
        snprintf(out,cap,"{\"ok\":true,\"mode\":\"local-only\",\"model_dispatch_supported\":false,\"tasks_started\":%llu,\"tasks_completed\":%llu,\"stale_discarded\":%llu,\"coalesced\":%llu,\"window_cache_hits\":%llu,\"window_cache_misses\":%llu,\"cache_charged_bytes\":%zu,\"frames\":%llu,\"failed_frames\":%llu,\"frame_p95_ms\":%.3f,\"frame_max_ms\":%.3f,\"render_p95_ms\":%.3f,\"encode_p95_ms\":%.3f}",(unsigned long long)snap->local_task_started,(unsigned long long)snap->local_task_completed,(unsigned long long)snap->local_task_stale,(unsigned long long)snap->local_task_coalesced,(unsigned long long)hits,(unsigned long long)misses,bytes,(unsigned long long)perf.frames,(unsigned long long)perf.failed_frames,perf.frame_ms_p95,perf.frame_ms_max,perf.render_ms_p95,perf.encode_ms_p95);
        free(snap);return;
    }

    if(!allowed_command(json)){snprintf(out,cap,"{\"ok\":false,\"error\":\"workbench_command_not_allowed\"}");return;}
    native_windows_command(json,out,cap);
    pixel_tui_session_windows_changed(stdout);
}
static void *control_loop(void *unused) {
    (void)unused;
    char *input=malloc(32768),*output=malloc(262144);
    if(!input||!output){free(input);free(output);return NULL;}
    bool checkpoint_warning=false;
    while(!atomic_load(&control_stop)) {
        char err[128];bool saved=native_workbench_state_checkpoint(err,sizeof(err));
        if(!saved && !checkpoint_warning)fprintf(stderr,"Workbench state not saved: %s\n",err);
        checkpoint_warning=!saved;
        struct pollfd p={.fd=control_fd,.events=POLLIN};
        if(poll(&p,1,100)<=0)continue;
        int fd=accept(control_fd,NULL,NULL);if(fd<0)continue;
#ifdef __APPLE__
        uid_t uid;gid_t gid;
        if(getpeereid(fd,&uid,&gid)!=0 || uid!=geteuid()){close(fd);continue;}
#endif
        struct timeval timeout={.tv_sec=1};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
        size_t n=0;bool complete=false;
        while(n+1<32768) {
            ssize_t got=read(fd,input+n,32767-n);if(got<=0)break;n+=(size_t)got;
            char *end=memchr(input,'\n',n);if(end){n=(size_t)(end-input);complete=true;break;}
        }
        input[n]=0;
        if(complete && !memchr(input,0,n))process_command(input,output,262144);
        else snprintf(output,262144,"{\"ok\":false,\"error\":\"incomplete_or_oversized_request\"}");
        size_t len=strlen(output),sent=0;
        while(sent<len){ssize_t w=send(fd,output+sent,len-sent,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );if(w<=0)break;sent+=(size_t)w;}
        close(fd);
    }
    free(input);free(output);return NULL;
}
int native_workbench_run(void) {
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)){fprintf(stderr,"Native workbench requires a terminal; it never falls back to a model.\n");return 2;}
    setenv("DSCO_PIXEL_TUI_SESSION","1",1);setenv("DSCO_NATIVE_WINDOWS","1",1);
    native_windows_reset();setenv("DSCO_PIXEL_TUI","1",1);
    setenv("DSCO_NATIVE_LOCAL_ONLY","1",1);
    char state_error[128];
    if(!native_workbench_state_start(state_error,sizeof(state_error))){fprintf(stderr,"Workbench state refused; existing file preserved: %s\n",state_error);return 2;}
    pixel_tui_perf_reset();pixel_tui_perf_set_capture(true);
    pixel_tui_session_begin(stdout,"Local workbench · no model");
    if(!pixel_tui_session_active()){fprintf(stderr,"Native graphics session unavailable; no provider was contacted.\n");native_workbench_state_close();return 2;}
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    snprintf(socket_path,sizeof(socket_path),"/tmp/dsco-workbench-%lu-%ld.sock",(unsigned long)geteuid(),(long)getpid());
    snprintf(address.sun_path,sizeof(address.sun_path),"%s",socket_path);
    control_fd=socket(AF_UNIX,SOCK_STREAM,0);
    /* A new PID path is never unlinked preemptively; do not replace another
     * socket/symlink. Restrict filesystem access before bind. */
    mode_t old_mask=umask(0077);
    int bound=control_fd>=0?bind(control_fd,(struct sockaddr*)&address,sizeof(address)):-1;
    umask(old_mask);
    if(bound!=0 || chmod(socket_path,0600)!=0 || listen(control_fd,8)!=0){
        fprintf(stderr,"Workbench control socket failed: %s\n",strerror(errno));if(control_fd>=0)close(control_fd);if(bound==0)unlink(socket_path);pixel_tui_session_end(stdout);native_workbench_state_close();return 2;
    }
    atomic_store(&control_stop,false);pthread_t controller;
    if(pthread_create(&controller,NULL,control_loop,NULL)!=0){close(control_fd);unlink(socket_path);pixel_tui_session_end(stdout);native_workbench_state_close();return 2;}
    if(!native_task_service_start(stdout)){atomic_store(&control_stop,true);pthread_join(controller,NULL);close(control_fd);unlink(socket_path);pixel_tui_session_end(stdout);native_workbench_state_close();return 2;}
    char *reply=malloc(262144);
    native_workbench_state_restore();
    if(reply){if(native_apps_snapshot(NULL,0)==0)process_command("{\"action\":\"factory\",\"factory\":\"json\",\"text\":\"{\\\"hello\\\":\\\"native\\\",\\\"model_calls\\\":0}\",\"width\":640,\"height\":380}",reply,262144);free(reply);}
    fprintf(stderr,"DSCO_WORKBENCH_SOCKET=%s\n",socket_path);
    fprintf(stderr,"Local-only factories: json, sha256, stats. Ctrl-R runs; Input/Result switch views.\n");
    fprintf(stderr,"Esc returns to the local command line. Enter JSON commands, or /quit. No language model is connected.\n");
    fprintf(stderr,"Applications survive closing views. Commands: {\"action\":\"apps\"}, {\"action\":\"reopen\",\"app_id\":1}, {\"action\":\"checkpoint\"}.\n");
    tui_status_bar_t bar={0};
    snprintf(bar.model,sizeof(bar.model),"Local workbench");
    char line[32768];
    for(;;) {
        char *read=tui_composer_read(&bar,"Local command (no model)",line,sizeof(line));
        if(!read || !strcmp(line,"/quit") || !strcmp(line,"/exit"))break;
        if(!line[0])continue;
        char *out=malloc(262144);if(!out)break;
        process_command(line,out,262144);fprintf(stderr,"%s\n",out);free(out);
    }
    atomic_store(&control_stop,true);pthread_join(controller,NULL);close(control_fd);unlink(socket_path);
    native_task_service_stop();native_apps_interrupt();
    bool saved=native_workbench_state_checkpoint(state_error,sizeof(state_error));
    if(!saved)fprintf(stderr,"Workbench shutdown checkpoint failed: %s\n",state_error);
    native_workbench_state_close();pixel_tui_session_end(stdout);tui_cleanup();return saved?0:1;
}
