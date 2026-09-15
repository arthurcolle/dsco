#define _DARWIN_C_SOURCE
#include "native_trace_ui.h"
#include "native_trace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool deny;
static unsigned calls;
bool pixel_tui_session_active(void) {return true;}
bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t cap) {
    assert(!strcmp(name,"ui_trace"));assert(!strcmp(tier,"untrusted"));
    assert(!strcmp(input,"{\"action\":\"start\",\"duration_ms\":10000}"));calls++;
    if(deny) {snprintf(out,cap,"{\"error\":\"DSCO_ALLOW_WRITE=0\"}");return false;}
    /* Exercise the real recorder and asynchronous finalization with a short
     * fixture deadline; the UI's requested ten seconds are checked above. */
    return tool_ui_trace("{\"action\":\"start\",\"duration_ms\":80}",out,cap);
}
static void pause_ms(int ms) {struct timespec t={.tv_nsec=ms*1000000L};nanosleep(&t,NULL);}
int main(void) {
    char dir[]="/tmp/dsco-trace-ui-XXXXXX";assert(mkdtemp(dir));setenv("TMPDIR",dir,1);
    unsetenv("DSCO_ALLOW_WRITE");native_trace_ui_reset();
    native_trace_ui_view_t initial,view;native_trace_ui_snapshot(&initial);
    assert(initial.state==NATIVE_TRACE_UI_RECORD && initial.enabled);
    deny=true;assert(!native_trace_ui_activate(&initial,"untrusted"));assert(calls==1);
    native_trace_ui_snapshot(&view);assert(strstr(view.detail,"DSCO_ALLOW_WRITE=0"));
    assert(!native_trace_active() && !native_trace_ui_action_pending());
    deny=false;assert(native_trace_ui_activate(&view,"untrusted"));assert(calls==2);
    assert(!native_trace_ui_activate(&initial,"untrusted") && calls==2);
    native_trace_ui_snapshot(&view);
    assert(view.state==NATIVE_TRACE_UI_RECORDING && !view.enabled);
    assert(!native_trace_ui_activate(&view,"untrusted"));
    assert(!native_trace_ui_action_pending());
    for(int i=0;i<100;i++) {
        native_trace_ui_snapshot(&view);if(view.state==NATIVE_TRACE_UI_ASK)break;pause_ms(10);
    }
    assert(view.state==NATIVE_TRACE_UI_ASK && view.enabled);
    assert(!native_trace_ui_action_pending()); /* Completion never sends to a provider. */
    native_trace_status_t trace;native_trace_status(&trace);
    double elapsed=trace.elapsed_ms;pause_ms(10);native_trace_status(&trace);
    assert(trace.complete && elapsed==trace.elapsed_ms && elapsed>=80);
    assert(!native_trace_ui_activate(&initial,"untrusted"));
    assert(native_trace_ui_activate(&view,"untrusted"));
    assert(!native_trace_ui_activate(&view,"untrusted"));
    assert(native_trace_ui_action_pending());
    native_trace_ui_snapshot(&view);assert(view.state==NATIVE_TRACE_UI_QUEUED && !view.enabled);
    char intent[32768],small[8];
    assert(!native_trace_ui_pop_intent(small,sizeof(small)) && native_trace_ui_action_pending());
    assert(native_trace_ui_pop_intent(intent,sizeof(intent)));
    assert(strstr(intent,trace.path) && strstr(intent,"normal read_file") && strstr(intent,"no additional authority"));
    assert(!native_trace_ui_pop_intent(intent,sizeof(intent)) && calls==2);
    native_trace_ui_snapshot(&view);assert(view.state==NATIVE_TRACE_UI_RECORD && view.enabled);
    native_trace_shutdown();unlink(trace.path);rmdir(dir);
    puts("PASS: trace UI gated start, visible denial, countdown, autonomous completion, explicit one-time handoff, stale clicks, retained elapsed time");
}
