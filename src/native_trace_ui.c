#include "native_trace_ui.h"
#include "native_trace.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t delivered_generation;
static bool dispatching;
static char pending[32768], error[192];

static void make_view(native_trace_ui_view_t *view, const native_trace_status_t *trace) {
    *view=(native_trace_ui_view_t){.generation=trace->generation,
        .state=NATIVE_TRACE_UI_RECORD,.enabled=true};
    snprintf(view->label,sizeof(view->label),"Record UI issue");
    if(pending[0]) {
        view->state=NATIVE_TRACE_UI_QUEUED;view->enabled=false;
        snprintf(view->label,sizeof(view->label),"Trace queued");
        snprintf(view->detail,sizeof(view->detail),"AI will inspect the recording at its next turn. Your draft stays here.");
    } else if(trace->active) {
        int seconds=(int)ceil(fmax(0,trace->duration_ms-trace->elapsed_ms)/1000.0);
        view->state=NATIVE_TRACE_UI_RECORDING;view->enabled=false;
        snprintf(view->label,sizeof(view->label),"Recording %ds",seconds);
        snprintf(view->detail,sizeof(view->detail),"Reproduce the glitch now. Recording stops automatically; no screenshots or document text.");
    } else if(dispatching || (trace->generation && !trace->complete)) {
        view->state=NATIVE_TRACE_UI_SAVING;view->enabled=false;
        snprintf(view->label,sizeof(view->label),"Saving UI trace");
    } else if(trace->complete && !trace->write_failed && trace->generation!=delivered_generation) {
        view->state=NATIVE_TRACE_UI_ASK;
        snprintf(view->label,sizeof(view->label),"Ask AI about trace");
        snprintf(view->detail,sizeof(view->detail),"Recording saved. Click to ask AI to inspect it; your draft stays here.");
    } else if(error[0]) {
        snprintf(view->label,sizeof(view->label),"Retry UI recording");
        snprintf(view->detail,sizeof(view->detail),"%s",error);
    } else if(trace->write_failed) {
        snprintf(view->detail,sizeof(view->detail),"UI recording could not be saved. Click to try again.");
    }
}
void native_trace_ui_snapshot(native_trace_ui_view_t *view) {
    if(!view)return;
    pthread_mutex_lock(&mu);
    native_trace_status_t trace;native_trace_status(&trace);
    make_view(view,&trace);
    pthread_mutex_unlock(&mu);
}
bool native_trace_ui_activate(const native_trace_ui_view_t *view,const char *tier) {
    if(!view)return false;
    pthread_mutex_lock(&mu);
    native_trace_status_t trace;native_trace_status(&trace);
    native_trace_ui_view_t current;make_view(&current,&trace);
    if(!current.enabled || current.state!=view->state || current.generation!=view->generation) {
        pthread_mutex_unlock(&mu);return false;
    }
    if(current.state==NATIVE_TRACE_UI_ASK) {
        /* Quote the path as data; never turn artifact metadata into commands.
         * Read permissions and all subsequent tool actions remain governed. */
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
        if(!doc) {pthread_mutex_unlock(&mu);return false;}
        yyjson_mut_val *path=yyjson_mut_strcpy(doc,trace.path);
        yyjson_mut_doc_set_root(doc,path);
        char *quoted=path ? yyjson_mut_write(doc,0,NULL) : NULL;
        if(quoted)snprintf(pending,sizeof(pending),
            "[Native UI diagnostic request from the terminal Record UI issue control]\n"
            "The native UI is acting up. Inspect this completed local component timeline and help diagnose it.\n"
            "Trace path (JSON string): %s\n"
            "Recorded %.0f ms; %llu frame samples, %llu failed submissions, maximum frame work %.3f ms.\n"
            "Read the trace through the normal read_file tool. Compare expected animation intervals with "
            "component pixel changes, input, scheduler and frame events. Explain the evidence and any uncertainty. "
            "These are terminal submissions, not physical display acknowledgements; the trace contains no document text. "
            "Keep the current task and my draft intact. This click requests diagnosis and grants no additional authority.",
            quoted,trace.elapsed_ms,(unsigned long long)trace.frames,
            (unsigned long long)trace.failed_frames,trace.max_frame_ms);
        free(quoted);yyjson_mut_doc_free(doc);
        bool ok=pending[0]!=0;
        if(ok)delivered_generation=trace.generation;
        pthread_mutex_unlock(&mu);return ok;
    }
    dispatching=true;error[0]=0;
    pthread_mutex_unlock(&mu);
    char result[8192]={0};
    bool ok=tools_execute_for_tier("ui_trace","{\"action\":\"start\",\"duration_ms\":10000}",
        tier && *tier ? tier : "trusted",result,sizeof(result));
    pthread_mutex_lock(&mu);dispatching=false;
    if(!ok) {
        yyjson_doc *doc=yyjson_read(result,strlen(result),0);
        yyjson_val *root=doc ? yyjson_doc_get_root(doc) : NULL;
        const char *reason=yyjson_get_str(yyjson_obj_get(root,"reason"));
        if(!reason)reason=yyjson_get_str(yyjson_obj_get(root,"error"));
        if(!reason)reason=result[0] ? result : "recording unavailable";
        snprintf(error,sizeof(error),"Recording failed: %.100s. /ui trace 10s shows details.",reason);
        for(char *p=error;*p;p++)if((unsigned char)*p<32 || (unsigned char)*p==127)*p=' ';
        yyjson_doc_free(doc);
    }
    pthread_mutex_unlock(&mu);return ok;
}
bool native_trace_ui_action_pending(void) {
    pthread_mutex_lock(&mu);bool value=pending[0]!=0;pthread_mutex_unlock(&mu);return value;
}
bool native_trace_ui_pop_intent(char *out,size_t cap) {
    if(!out || !cap)return false;
    pthread_mutex_lock(&mu);
    bool ok=pending[0] && strlen(pending)<cap;
    if(ok) {memcpy(out,pending,strlen(pending)+1);pending[0]=0;}
    pthread_mutex_unlock(&mu);return ok;
}
void native_trace_ui_reset(void) {
    pthread_mutex_lock(&mu);pending[0]=error[0]=0;delivered_generation=0;
    pthread_mutex_unlock(&mu);
}
