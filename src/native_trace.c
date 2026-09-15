#define _DARWIN_C_SOURCE
#include "native_trace.h"
#include "pixel_tui.h"
#include "tools.h"
#include "config.h"
#include "../vendor/yyjson.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TRACE_CAP (8u * 1024u * 1024u)
#define TRACE_COMPONENTS 96
static const char *const fields[NT_FIELDS] = {"x","y","width","height","revision",
    "bytes","cursor","scroll","status","visible","focused","dirty",
    "animation_expected","pixel_crc","count","pending","expected_interval_ms"};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static atomic_bool active;
static pthread_t writer;
static bool joinable, joining, stopping, complete, write_failed;
static const char *stop_reason;
static char *data, path[4096];
static size_t used, events, previous_count;
static int output_fd=-1;
static double started, duration, finished, max_frame_ms;
static uint64_t generation, frames, failed_frames;
static native_trace_component_t previous[TRACE_COMPONENTS];

static double now_ms(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec*1000.0+(double)ts.tv_nsec/1e6;
}
static bool scoped_write_grant(const char *value) {
    if(!value || !*value)return false;
    static const char *const booleans[]={"0","1","true","false","on","off","allow","deny"};
    for(size_t i=0;i<sizeof(booleans)/sizeof(*booleans);i++)if(!strcasecmp(value,booleans[i]))return false;
    return true;
}
bool native_trace_active(void) { return atomic_load_explicit(&active,memory_order_relaxed); }
void native_trace_status(native_trace_status_t *status) {
    if(!status)return;
    pthread_mutex_lock(&mu);
    *status=(native_trace_status_t){.generation=generation,.frames=frames,
        .failed_frames=failed_frames,.active=native_trace_active(),.complete=complete,
        .write_failed=write_failed,.duration_ms=duration,.max_frame_ms=max_frame_ms,
        .elapsed_ms=started>0 ? (finished>0 ? finished : now_ms())-started : 0};
    snprintf(status->path,sizeof(status->path),"%s",path);
    pthread_mutex_unlock(&mu);
}
static void request_stop(const char *reason) {
    if(!stopping)stop_reason=reason;
    atomic_store(&active,false);stopping=true;pthread_cond_signal(&ready);
}
static void append_doc(yyjson_mut_doc *doc) {
    if(!doc)return;
    char *line=yyjson_mut_write(doc,0,NULL);
    if(!line) {request_stop("allocation_failed");return;}
    size_t len=strlen(line);
    if(len+used+1>TRACE_CAP-1024)request_stop("size_limit");
    else {memcpy(data+used,line,len);used+=len;data[used++]='\n';events++;}
    free(line);
}
static yyjson_mut_doc *event_doc(const char *kind,yyjson_mut_val **root) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    *root=doc ? yyjson_mut_obj(doc) : NULL;
    if(!*root) {yyjson_mut_doc_free(doc);return NULL;}
    yyjson_mut_doc_set_root(doc,*root);
    yyjson_mut_obj_add_str(doc,*root,"event",kind);
    yyjson_mut_obj_add_uint(doc,*root,"seq",events);
    yyjson_mut_obj_add_real(doc,*root,"ms",round((now_ms()-started)*1000.0)/1000.0);
    return doc;
}
static void *finish_trace(void *unused) {
    (void)unused;pthread_mutex_lock(&mu);
    while(!stopping) {
        double left=started+duration-now_ms();
        if(left<=0) {request_stop("duration");break;}
        struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
        int64_t ns=(int64_t)(left*1e6)+deadline.tv_nsec;
        deadline.tv_sec+=(time_t)(ns/1000000000);deadline.tv_nsec=(long)(ns%1000000000);
        pthread_cond_timedwait(&ready,&mu,&deadline);
    }
    finished=now_ms();
    int len=snprintf(data+used,TRACE_CAP-used,
        "{\"event\":\"end\",\"ms\":%.3f,\"reason\":\"%s\",\"events\":%zu,"
        "\"frames\":%llu,\"failed_frames\":%llu,\"max_frame_ms\":%.3f}\n",
        finished-started,stop_reason,events,(unsigned long long)frames,
        (unsigned long long)failed_frames,max_frame_ms);
    if(len>0 && (size_t)len<TRACE_CAP-used)used+=(size_t)len;
    char *bytes=data;size_t count=used;int fd=output_fd;
    pthread_mutex_unlock(&mu);
    /* All file writes happen on this worker, never on the compositor thread. */
    size_t at=0;bool failed=false;
    while(at<count) {
        ssize_t n=write(fd,bytes+at,count-at);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {failed=true;break;}at+=(size_t)n;
    }
    if(close(fd))failed=true;
    pthread_mutex_lock(&mu);output_fd=-1;write_failed=failed;complete=true;
    free(data);data=NULL;pthread_mutex_unlock(&mu);return NULL;
}
void native_trace_shutdown(void) {
    pthread_mutex_lock(&mu);
    if(!joinable) {pthread_mutex_unlock(&mu);return;}
    if(!complete)request_stop("stopped");
    pthread_t thread=writer;joinable=false;joining=true;
    pthread_mutex_unlock(&mu);pthread_join(thread,NULL);
    pthread_mutex_lock(&mu);joining=false;pthread_mutex_unlock(&mu);
}
static bool reply(char *out,size_t cap,bool ok,const char *error) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o=doc ? yyjson_mut_obj(doc) : NULL;
    if(!o) {yyjson_mut_doc_free(doc);return false;}
    yyjson_mut_doc_set_root(doc,o);yyjson_mut_obj_add_bool(doc,o,"ok",ok);
    if(error)yyjson_mut_obj_add_str(doc,o,"error",error);
    yyjson_mut_obj_add_bool(doc,o,"active",native_trace_active());
    yyjson_mut_obj_add_bool(doc,o,"complete",complete);
    yyjson_mut_obj_add_bool(doc,o,"write_failed",write_failed);
    yyjson_mut_obj_add_str(doc,o,"path",path);
    yyjson_mut_obj_add_uint(doc,o,"events",events);yyjson_mut_obj_add_uint(doc,o,"bytes",used);
    yyjson_mut_obj_add_uint(doc,o,"frames",frames);
    yyjson_mut_obj_add_real(doc,o,"duration_ms",duration);
    char *json=yyjson_mut_write(doc,0,NULL);bool fits=json && strlen(json)<cap;
    if(fits)memcpy(out,json,strlen(json)+1);
    else if(cap)snprintf(out,cap,"{\"ok\":false,\"error\":\"output_too_small\"}");
    free(json);yyjson_mut_doc_free(doc);return ok && fits;
}
bool tool_ui_trace(const char *input,char *out,size_t cap) {
    if(!out || cap<256)return false;
    yyjson_doc *doc=input ? yyjson_read(input,strlen(input),0) : NULL;
    yyjson_val *o=doc ? yyjson_doc_get_root(doc) : NULL;
    const char *action=yyjson_get_str(yyjson_obj_get(o,"action"));
    const char *requested_path=yyjson_get_str(yyjson_obj_get(o,"path"));
    yyjson_val *d=yyjson_obj_get(o,"duration_ms");
    const char *error=NULL;
    if(!yyjson_is_obj(o) || !action || (strcmp(action,"start") && strcmp(action,"status") && strcmp(action,"stop")))
        error="expected_start_status_or_stop";
    if(!error && strlen(action)!=yyjson_get_len(yyjson_obj_get(o,"action")))error="invalid_action";
    if(!error) {
        size_t i,n;yyjson_val *key,*value;
        yyjson_obj_foreach(o,i,n,key,value) {
            const char *k=yyjson_get_str(key);
            if(strcmp(k,"action") && strcmp(k,"duration_ms") && strcmp(k,"path"))error="unknown_field";
            size_t j,m;yyjson_val *k2,*v2;
            yyjson_obj_foreach(o,j,m,k2,v2) { (void)v2;if(j==i)break;
                if(yyjson_equals_str(k2,k))error="duplicate_field"; }
            if(yyjson_get_len(key)!=strlen(k))error="invalid_field";
            (void)value;
        }
    }
    if(!error && d && (!yyjson_is_uint(d) || yyjson_get_uint(d)<1 || yyjson_get_uint(d)>600000))error="duration_out_of_range";
    if(!error && yyjson_obj_get(o,"path") && (!requested_path || strlen(requested_path)!=yyjson_get_len(yyjson_obj_get(o,"path")) || !*requested_path))error="invalid_path";
    if(!error && strcmp(action,"start") && (d || requested_path))error="start_options_only";
    if(!error && !strcmp(action,"start") && !pixel_tui_session_active())error="native_session_required";
    const char *write_scope=getenv("DSCO_ALLOW_WRITE");
    if(!error && !strcmp(action,"start") && !requested_path && scoped_write_grant(write_scope))
        error="explicit_path_required_for_scoped_write";
    if(!error && !strcmp(action,"stop"))native_trace_shutdown();
    pthread_mutex_lock(&mu);
    if(!error && !strcmp(action,"start")) {
        if(joining || (!complete && joinable))error="trace_already_running";
        else {
            if(joinable) {pthread_join(writer,NULL);joinable=false;}
            char candidate[sizeof(path)];
            if(requested_path)snprintf(candidate,sizeof(candidate),"%s",requested_path);
            else if(snprintf(candidate,sizeof(candidate),"%s/dsco-ui-%ld-XXXXXX.jsonl",getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",(long)getpid())>=(int)sizeof(candidate))error="path_too_long";
            if(requested_path && strlen(requested_path)>=sizeof(candidate))error="path_too_long";
            char *buffer=error ? NULL : malloc(TRACE_CAP);
            int fd=-1;
            if(!error && !buffer)error="allocation_failed";
            if(!error)fd=requested_path ? open(candidate,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600) : mkstemps(candidate,6);
            if(!error && fd<0)error="cannot_create_new_trace_file";
            if(!error) {
                fcntl(fd,F_SETFD,FD_CLOEXEC);
                strcpy(path,candidate);data=buffer;output_fd=fd;used=events=previous_count=0;
                started=now_ms();finished=0;generation++;
                duration=d ? (double)yyjson_get_uint(d) : 10000;
                stopping=complete=write_failed=false;frames=failed_frames=0;max_frame_ms=0;
                stop_reason="duration";
                used=(size_t)snprintf(data,TRACE_CAP,
                    "{\"event\":\"begin\",\"schema\":\"dsco.native_trace.v1\",\"pid\":%ld,"
                    "\"duration_ms\":%.0f,\"content\":\"metadata_and_pixel_checksums\","
                    "\"build\":\"%s\",\"presentation\":\"terminal_submission_not_display_ack\"}\n",(long)getpid(),duration,GIT_HASH);
                atomic_store(&active,true);
                if(pthread_create(&writer,NULL,finish_trace,NULL)) {
                    atomic_store(&active,false);free(data);data=NULL;close(fd);output_fd=-1;
                    unlink(path);error="trace_thread_failed";complete=true;write_failed=true;
                } else joinable=true;
            } else free(buffer);
        }
    }
    bool ok=reply(out,cap,!error,error);pthread_mutex_unlock(&mu);
    yyjson_doc_free(doc);return ok;
}
void native_trace_event(const char *kind,double a,double b,double c,double d) {
    if(!native_trace_active())return;
    pthread_mutex_lock(&mu);
    if(native_trace_active()) {
        yyjson_mut_val *o;yyjson_mut_doc *doc=event_doc(kind,&o);
        if(doc) {
            yyjson_mut_val *values=yyjson_mut_arr(doc);
            yyjson_mut_arr_add_real(doc,values,a);yyjson_mut_arr_add_real(doc,values,b);
            yyjson_mut_arr_add_real(doc,values,c);yyjson_mut_arr_add_real(doc,values,d);
            yyjson_mut_obj_add_val(doc,o,"values",values);append_doc(doc);yyjson_mut_doc_free(doc);
        }
    }
    pthread_mutex_unlock(&mu);
}
void native_trace_frame(const pixel_tui_frame_sample_t *s) {
    if(!native_trace_active() || !s || s->kind<PIXEL_TUI_FRAME_IDENTICAL || s->kind>PIXEL_TUI_FRAME_FAILED)return;
    pthread_mutex_lock(&mu);
    if(native_trace_active()) {
        frames++;failed_frames+=s->kind==PIXEL_TUI_FRAME_FAILED;
        if(s->frame_ms>max_frame_ms)max_frame_ms=s->frame_ms;
        yyjson_mut_val *o;yyjson_mut_doc *doc=event_doc("frame",&o);
        if(doc) {
            static const char *const kinds[]={"identical","patch","full","failed"};
            yyjson_mut_obj_add_str(doc,o,"kind",kinds[s->kind]);
            yyjson_mut_obj_add_real(doc,o,"frame_ms",s->frame_ms);
            yyjson_mut_obj_add_real(doc,o,"queue_ms",s->queue_ms);
            yyjson_mut_obj_add_real(doc,o,"render_ms",s->render_ms);
            yyjson_mut_obj_add_real(doc,o,"encode_ms",s->encode_ms);
            yyjson_mut_obj_add_real(doc,o,"upload_ms",s->upload_ms);
            yyjson_mut_obj_add_real(doc,o,"flush_ms",s->flush_ms);
            yyjson_mut_obj_add_uint(doc,o,"wire_bytes",s->wire_bytes);
            yyjson_mut_obj_add_uint(doc,o,"damage_rects",s->damage_rects);
            append_doc(doc);yyjson_mut_doc_free(doc);
        }
    }
    pthread_mutex_unlock(&mu);
}
void native_trace_components(const native_trace_component_t *components,size_t count) {
    if(!native_trace_active())return;
    if(count>TRACE_COMPONENTS)count=TRACE_COMPONENTS;
    pthread_mutex_lock(&mu);
    if(!native_trace_active()) {pthread_mutex_unlock(&mu);return;}
    for(size_t i=0;i<count && native_trace_active();i++) {
        const native_trace_component_t *c=&components[i],*old=NULL;
        for(size_t j=0;j<previous_count;j++)if(!strcmp(c->id,previous[j].id)) {old=&previous[j];break;}
        if(old && !memcmp(old->value,c->value,sizeof(c->value)))continue;
        yyjson_mut_val *o;yyjson_mut_doc *doc=event_doc("component_delta",&o);
        if(!doc)continue;
        yyjson_mut_obj_add_str(doc,o,"id",c->id);yyjson_mut_obj_add_str(doc,o,"op",old ? "change" : "add");
        yyjson_mut_val *delta=yyjson_mut_obj(doc);
        for(int f=0;f<NT_FIELDS;f++)if(!old || old->value[f]!=c->value[f]) {
            yyjson_mut_val *pair=yyjson_mut_arr(doc);
            if(old)yyjson_mut_arr_add_sint(doc,pair,old->value[f]);else yyjson_mut_arr_add_null(doc,pair);
            yyjson_mut_arr_add_sint(doc,pair,c->value[f]);yyjson_mut_obj_add_val(doc,delta,fields[f],pair);
        }
        yyjson_mut_obj_add_val(doc,o,"changes",delta);append_doc(doc);yyjson_mut_doc_free(doc);
    }
    for(size_t i=0;i<previous_count && native_trace_active();i++) {
        bool found=false;for(size_t j=0;j<count;j++)if(!strcmp(previous[i].id,components[j].id))found=true;
        if(found)continue;
        yyjson_mut_val *o;yyjson_mut_doc *doc=event_doc("component_delta",&o);
        if(doc) {yyjson_mut_obj_add_str(doc,o,"id",previous[i].id);yyjson_mut_obj_add_str(doc,o,"op","remove");append_doc(doc);yyjson_mut_doc_free(doc);}
    }
    memcpy(previous,components,count*sizeof(*components));previous_count=count;
    pthread_mutex_unlock(&mu);
}
bool native_trace_command(const char *tail,const char *tier,char *out,size_t cap) {
    if(!out || cap<256)return false;
    while(tail && isspace((unsigned char)*tail))tail++;
    char trimmed[128];size_t length=tail ? strlen(tail) : 0;
    if(length>=sizeof(trimmed)) {snprintf(out,cap,"Trace interval is too long.");return false;}
    if(length)memcpy(trimmed,tail,length);
    while(length && isspace((unsigned char)trimmed[length-1]))length--;
    trimmed[length]=0;tail=trimmed;
    char request[128];
    if(tail && (!strcmp(tail,"status") || !strcmp(tail,"stop")))
        snprintf(request,sizeof(request),"{\"action\":\"%s\"}",tail);
    else {
        char *end=NULL;double n=tail && *tail ? strtod(tail,&end) : 10;
        if(!tail || !*tail)end="s";
        while(end && isspace((unsigned char)*end))end++;
        double multiplier=end && (!strcmp(end,"ms") || !strcmp(end,"milliseconds")) ? 1 :
            end && (!*end || !strcmp(end,"s") || !strcmp(end,"sec") || !strcmp(end,"second") || !strcmp(end,"seconds")) ? 1000 :
            end && (!strcmp(end,"m") || !strcmp(end,"min") || !strcmp(end,"minute") || !strcmp(end,"minutes")) ? 60000 : 0;
        double ms=n*multiplier;
        if((tail && *tail && end==tail) || !isfinite(ms) || ms<1 || ms>600000) {
            snprintf(out,cap,"Usage: /ui trace 10s (or 500ms, 1m, status, stop). Maximum 10 minutes.");return false;
        }
        snprintf(request,sizeof(request),"{\"action\":\"start\",\"duration_ms\":%.0f}",ms);
    }
    bool ok=tools_execute_for_tier("ui_trace",request,tier,out,cap);
    if(ok) {
        yyjson_doc *doc=yyjson_read(out,strlen(out),0);
        yyjson_val *o=doc ? yyjson_doc_get_root(doc) : NULL;
        const char *file=yyjson_get_str(yyjson_obj_get(o,"path"));
        if(file && *file) {
            bool running=yyjson_is_true(yyjson_obj_get(o,"active"));
            bool done=yyjson_is_true(yyjson_obj_get(o,"complete"));
            bool failed=yyjson_is_true(yyjson_obj_get(o,"write_failed"));
            snprintf(out,cap,"%s (%.1f seconds; %llu frames).\nTrace: %s\n%s",
                failed ? "UI trace write failed" : running ? "Recording UI component changes" : done ? "UI trace complete" : "Finalizing UI trace",
                yyjson_get_num(yyjson_obj_get(o,"duration_ms"))/1000.0,
                (unsigned long long)yyjson_get_uint(yyjson_obj_get(o,"frames")),file,
                running ? "Reproduce the issue now. Recording stops automatically; /ui trace status checks completion." :
                failed ? "The file may be incomplete." : "The JSONL file contains component deltas and frame timing for inspection.");
        }
        yyjson_doc_free(doc);
    }
    return ok;
}
