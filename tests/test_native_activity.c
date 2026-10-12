/* Headless native tool cadence and transition regressions. No window or provider. */
#define _DARWIN_C_SOURCE
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <zlib.h>
static _Atomic unsigned scheduler_waits;
static int fixture_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                  const struct timespec *deadline) {
    atomic_fetch_add(&scheduler_waits, 1);
    return pthread_cond_timedwait(cond, mutex, deadline);
}
static double fixed_mono;
static int fixture_clock_gettime(clockid_t clock, struct timespec *ts) {
    if (fixed_mono > 0 && clock == CLOCK_MONOTONIC) {
        ts->tv_sec=(time_t)fixed_mono;
        ts->tv_nsec=(long)((fixed_mono-ts->tv_sec)*1e9);
        return 0;
    }
    return clock_gettime(clock,ts);
}
#define clock_gettime fixture_clock_gettime
#define pthread_cond_timedwait fixture_cond_timedwait
#include "../src/pixel_tui.c"
#undef clock_gettime
#undef pthread_cond_timedwait
#include "vm.h"
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int failures;
#define CHECK(x, label) do { if (!(x)) { ++failures; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static void pause_ms(int ms) { struct timespec t={.tv_sec=ms/1000,.tv_nsec=(ms%1000)*1000000L}; nanosleep(&t,NULL); }
static void pixel_parity(void) {
    for (int scale=1; scale<=3; ++scale) {
        for (int tools=1; tools<=5; tools+=2) {
            FILE *out=tmpfile(); CHECK(out!=NULL,"parity sink"); if (!out) return;
            fixed_mono=1000;
            g_session=(pixel_session_t){.active=true,.width=801,.height=601,
                .backing_scale=scale,.surface_width=801*scale,.surface_height=601*scale,
                .cols=80,.rows=30,.state=PIXEL_TUI_EXECUTING,.current_message=-1,
                .animation_enabled=true,.animation_interval_ms=17,.patch_enabled=true,
                .input_active=true,.tty_out=out};
            ui_motion_init(&g_session.motion,false);
            for (int i=0;i<tools;i++) {
                g_session.tool_visuals[i]=(pixel_tool_visual_t){.used=true,.sequence=(uint64_t)i+1,
                    .status=PIXEL_OP_RUNNING,.started_s=998};
                strcpy(g_session.tool_visuals[i].name,"bash");
                strcpy(g_session.tool_visuals[i].preview,"sleep 3; echo complete");
            }
            CHECK(session_repaint(out,true),"full deck baseline");
            size_t bytes=(size_t)g_session.surface_width*g_session.surface_height*3U;
            uint8_t *before=malloc(bytes); CHECK(before!=NULL,"parity copy");
            if (!before) { fclose(out); return; }
            memcpy(before,g_session.prev_frame,bytes);
            CHECK(session_repaint_activity(out),"bounded deck patch accepted");
            if (memcmp(before,g_session.prev_frame,bytes)) {
                size_t first=0,last=0,count=0;
                for(size_t n=0;n<bytes;n++) if(before[n]!=g_session.prev_frame[n]) {
                    if(!count) first=n;last=n;count++;
                }
                fprintf(stderr,"parity scale=%d tools=%d first=(%zu,%zu) last=(%zu,%zu) changed=%zu rect=%d,%d %dx%d\n",
                    scale,tools,(first/3)%g_session.surface_width,(first/3)/g_session.surface_width,
                    (last/3)%g_session.surface_width,(last/3)/g_session.surface_width,count,
                    g_session.activity_rect.x,g_session.activity_rect.y,g_session.activity_rect.width,g_session.activity_rect.height);
            }
            CHECK(memcmp(before,g_session.prev_frame,bytes)==0,
                  "same-time patch equals full compositor pixels (no clipped shadow/alpha buildup)");
            fixed_mono+=.2;
            CHECK(session_repaint_activity(out),"next tool animation tick");
            CHECK(memcmp(before,g_session.prev_frame,bytes)!=0,"tool pixels actually move");
            native_ui_rect_t r=g_session.activity_rect;
            bool outside_ok=true;
            for (int y=0;y<g_session.surface_height;y++) {
                for (int x=0;x<g_session.surface_width;x++) {
                    if (x>=r.x*scale && x<(r.x+r.width)*scale &&
                        y>=r.y*scale && y<(r.y+r.height)*scale) continue;
                    size_t at=((size_t)y*g_session.surface_width+x)*3U;
                    if (memcmp(before+at,g_session.prev_frame+at,3)) outside_ok=false;
                }
            }
            CHECK(outside_ok,"tool tick never changes transcript, editor, or other regions");
            memcpy(before,g_session.prev_frame,bytes);
            CHECK(session_repaint_activity(out),"repeat identical tick");
            CHECK(memcmp(before,g_session.prev_frame,bytes)==0,"alpha does not accumulate");
            long position=ftell(out);
            g_session.modal.active=true;
            CHECK(!session_repaint_activity(out) && ftell(out)==position,"never paint over modal");
            g_session.modal.active=false; g_session.overlay_image_id=42;
            CHECK(!session_repaint_activity(out) && ftell(out)==position,"never paint over overlay");
            g_session.overlay_image_id=0; g_session.scene_image_id=42;
            CHECK(!session_repaint_activity(out) && ftell(out)==position,"never paint over retained scene");
            g_session.scene_image_id=0;
            g_session.surface_width++;
            CHECK(!session_repaint_activity(out) && ftell(out)==position,"stale resize baseline rejected");
            g_session.surface_width--;
            g_session.tool_visuals[0].status=PIXEL_OP_DONE;
            g_session.tool_visuals[0].elapsed_ms=2200;
            fixed_mono=1002;
            CHECK(!session_repaint_activity(out),"expired tool requires semantic reflow");
            CHECK(session_repaint(out,true),"completion reconciles transcript layout");
            CHECK(!g_session.structural_repaint_pending,"completion semantic work drained");
            free(before);free(g_session.prev_frame);free(g_session.activity_underlay);
            free(g_session.patch_buffer);fclose(out);g_session=(pixel_session_t){0};
        }
    }
    fixed_mono=0;
}
static void phase_without_input(pixel_tui_state_t phase) {
    FILE *sink=tmpfile();
    CHECK(sink!=NULL,"phase transport sink"); if (!sink) return;
    g_session=(pixel_session_t){.active=true,.width=801,.height=601,.backing_scale=1,
        .surface_width=801,.surface_height=601,.cols=80,.rows=30,.state=phase,
        .current_message=-1,.animation_enabled=true,.animation_interval_ms=17,
        .patch_enabled=true,.input_active=true,.tty_out=sink};
    ui_motion_init(&g_session.motion,false);
    atomic_store(&g_composer_input_active,true);
    strcpy(g_session.input,"draft stays ready without any mouse or keyboard events");
    g_session.input_cursor=strlen(g_session.input);
    CHECK(session_repaint(sink,true),"phase baseline paints");
    pixel_tui_perf_set_capture(true);pixel_tui_perf_reset();
    atomic_store(&scheduler_waits,0);
    session_lock();session_animation_start();session_unlock();
    pause_ms(350);
    session_lock();
    uLong first=crc32(0,g_session.prev_frame,(uInt)(801*80*3));
    session_unlock();
    pause_ms(450);
    session_lock();
    uLong second=crc32(0,g_session.prev_frame,(uInt)(801*80*3));
    g_session.animation_stop=true;pthread_cond_signal(&g_animation_cond);
    session_unlock();pthread_join(g_session.animation_thread,NULL);
    pixel_tui_perf_snapshot_t perf=pixel_tui_perf_snapshot();
    printf("phase %d: %llu frames, %u waits, header changed=%d without input\n",
        phase,(unsigned long long)perf.frames,atomic_load(&scheduler_waits),first!=second);
    CHECK(first!=second,"phase animation advances without wheel, keyboard or provider events");
    CHECK(perf.frames>=5,"phase animation has a sustained frame clock while editor is active");
    CHECK(atomic_load(&scheduler_waits)<=120,"phase clock does not poll at 1ms");
    CHECK(!strcmp(g_session.input,"draft stays ready without any mouse or keyboard events"),
          "phase animation preserves the follow-up draft");
    atomic_store(&g_animation_thread_fast,false);
    free(g_session.prev_frame);free(g_session.activity_underlay);free(g_session.patch_buffer);
    fclose(sink);g_session=(pixel_session_t){0};
}
static void trace_completion_without_input(void) {
    FILE *sink=tmpfile();CHECK(sink!=NULL,"trace UI sink");if(!sink)return;
    g_session=(pixel_session_t){.active=true,.width=801,.height=601,.backing_scale=1,
        .surface_width=801,.surface_height=601,.cols=80,.rows=30,
        .state=PIXEL_TUI_IDLE,.current_message=-1,.animation_enabled=false,
        .animation_interval_ms=17,.patch_enabled=true,.input_active=true,.tty_out=sink};
    ui_motion_init(&g_session.motion,true);native_trace_ui_reset();
    atomic_store(&g_session_active_fast,true);
    strcpy(g_session.input,"unsent draft");g_session.input_cursor=6;
    char result[8192];
    native_trace_ui_view_t denied;
    native_trace_ui_snapshot(&denied);
    const char *old_write=getenv("DSCO_ALLOW_WRITE");
    char *saved_write=old_write ? strdup(old_write) : NULL;
    setenv("DSCO_ALLOW_WRITE","0",1);
    CHECK(!native_trace_ui_activate(&denied,"trusted"),"UI recording obeys the real write gate");
    native_trace_ui_snapshot(&denied);
    CHECK(strstr(denied.detail,"DSCO_ALLOW_WRITE")!=NULL,"UI names the denied write grant");
    if(saved_write) {setenv("DSCO_ALLOW_WRITE",saved_write,1);free(saved_write);}
    else unsetenv("DSCO_ALLOW_WRITE");
    CHECK(tool_ui_trace("{\"action\":\"start\",\"duration_ms\":150}",result,sizeof(result)),"short trace starts");
    CHECK(session_repaint(sink,true),"trace countdown paints");
    CHECK(s_trace_ui_view.state==NATIVE_TRACE_UI_RECORDING,"initial countdown is visible");
    native_ui_rect_t before=s_trace_ui_rect;
    CHECK(session_repaint_composer(sink),"trace control composer patch");
    CHECK(s_trace_ui_rect.x==before.x && s_trace_ui_rect.y==before.y,
          "composer patches retain screen coordinates for diagnostic hit testing");
    session_lock();session_animation_start();session_unlock();pause_ms(950);
    session_lock();
    CHECK(s_trace_ui_view.state==NATIVE_TRACE_UI_ASK,"completion repaints with no input and reduced motion");
    CHECK(!strcmp(g_session.input,"unsent draft") && g_session.input_cursor==6,"completion preserves draft and cursor");
    CHECK(!native_trace_ui_action_pending(),"completion does not implicitly request inference");
    g_session.animation_stop=true;pthread_cond_signal(&g_animation_cond);
    session_unlock();pthread_join(g_session.animation_thread,NULL);
    native_trace_status_t status;native_trace_status(&status);native_trace_shutdown();unlink(status.path);
    atomic_store(&g_animation_thread_fast,false);
    atomic_store(&g_session_active_fast,false);
    free(g_session.prev_frame);free(g_session.activity_underlay);free(g_session.patch_buffer);
    fclose(sink);g_session=(pixel_session_t){0};native_trace_ui_reset();
}
static pixel_tool_visual_t *fixture_tool(uint64_t operation_id) {
    for (int i=0;i<PIXEL_TOOL_VIS_CAP;i++)
        if (g_session.tool_visuals[i].used &&
            g_session.tool_visuals[i].sequence==operation_id)
            return &g_session.tool_visuals[i];
    return NULL;
}
static void tool_lifecycle(void) {
    FILE *sink=tmpfile();CHECK(sink!=NULL,"tool lifecycle sink");if(!sink)return;
    /* Exercise producer publication independently of the compositor clock. */
    g_session=(pixel_session_t){.active=true,.current_message=-1,.tty_out=sink,
        .animation_thread_started=true,.transcript_scroll=12};
    ui_motion_init(&g_session.motion,true);
    strcpy(g_session.input,"keep this follow-up draft");g_session.input_cursor=7;
    uint64_t first=pixel_tui_session_tool_begin(sink,"bash","{\"command\":\"first\"}");
    uint64_t second=pixel_tui_session_tool_begin(sink,"bash","{\"command\":\"second\"}");
    pixel_message_t *first_row=session_find_tool_message(first,"bash");
    pixel_message_t *second_row=session_find_tool_message(second,"bash");
    CHECK(first && second && first!=second && first_row && second_row,
          "same-name calls have separate operation identities");
    CHECK(g_session.transcript_scroll==12,"tool starts do not jump to the transcript tail");
    pixel_tui_session_tool_end(sink,second+100,"bash",false,1,"unknown result");
    CHECK(first_row->tool_status<0 && second_row->tool_status<0,
          "unknown explicit completion cannot finish a same-name call");
    pixel_tui_session_tool_end(sink,first,"bash",true,25,"first result");
    CHECK(first_row->tool_status==1 && second_row->tool_status<0,
          "explicit completion updates only its own row");
    pixel_tui_session_tool_end(sink,first,"bash",false,30,"duplicate result");
    CHECK(first_row->tool_status==1 && !strcmp(first_row->text,"first result") &&
          second_row->tool_status<0 && fixture_tool(second)->status==PIXEL_OP_RUNNING,
          "duplicate completion preserves the result and concurrent call");
    uint64_t third=pixel_tui_session_tool_begin(sink,"bash","{}");
    pixel_message_t *third_row=session_find_tool_message(third,"bash");
    pixel_tui_session_tool_end(sink,third,"bash",true,5,"third result");
    CHECK(third_row->tool_status==1 && second_row->tool_status<0,
          "out-of-order completion leaves the earlier concurrent call running");
    pixel_tui_session_tool_end(sink,0,"bash",true,40,"legacy result");
    CHECK(second_row->tool_status==1 && fixture_tool(second)->status==PIXEL_OP_DONE,
          "legacy zero-ID completion still finds the latest same-name call");
    CHECK(g_session.transcript_scroll==12,"tool results preserve scrollback mode");
    CHECK(!strcmp(g_session.input,"keep this follow-up draft") && g_session.input_cursor==7,
          "tool lifecycle preserves the draft and cursor");
    CHECK(g_session.structural_repaint_pending,"lifecycle schedules a visible semantic update");
    CHECK(ftell(sink)==0,"tool publication does not block on full-frame transport");
    session_messages_free();
    g_session=(pixel_session_t){.active=true,.current_message=-1,.tty_out=sink,
        .animation_thread_started=true};
    ui_motion_init(&g_session.motion,true);
    uint64_t operations[PIXEL_TOOL_VIS_CAP];
    for (int i=0;i<PIXEL_TOOL_VIS_CAP;i++)
        operations[i]=pixel_tui_session_tool_begin(sink,"bash","{}");
    uint64_t completed=operations[PIXEL_TOOL_VIS_CAP-1];
    pixel_tui_session_tool_end(sink,completed,"bash",true,1,"done");
    uint64_t next=pixel_tui_session_tool_begin(sink,"bash","{}");
    CHECK(fixture_tool(operations[0]) && fixture_tool(next) && !fixture_tool(completed),
          "new calls reuse finished visual slots before evicting slow running calls");
    pixel_message_t *evicted_row=session_find_tool_message(operations[0],"bash");
    uint64_t overflow=pixel_tui_session_tool_begin(sink,"bash","{}");
    CHECK(!fixture_tool(operations[0]) && fixture_tool(overflow),
          "all-running visual overflow remains bounded");
    pixel_tui_session_tool_end(sink,operations[0],"bash",true,80,"slow result");
    CHECK(evicted_row->tool_status==1 && !strcmp(evicted_row->text,"slow result") &&
          session_find_tool_message(overflow,"bash")->tool_status<0,
          "evicted visual still completes the correct durable transcript row");
    session_messages_free();fclose(sink);g_session=(pixel_session_t){0};
}
int main(void) {
    native_windows_reset();
    tool_lifecycle();
    pixel_parity();
    phase_without_input(PIXEL_TUI_REASONING);
    phase_without_input(PIXEL_TUI_RESPONDING);
    trace_completion_without_input();
    FILE *sink=tmpfile();
    CHECK(sink != NULL,"owned transport sink"); if (!sink) return 1;
    native_windows_reset();
    g_session=(pixel_session_t){.active=true,.width=1120,.height=700,.backing_scale=2,
        .surface_width=2240,.surface_height=1400,.cols=112,.rows=35,
        .state=PIXEL_TUI_EXECUTING,.current_message=-1,.animation_enabled=true,
        .animation_interval_ms=17,.patch_enabled=true,.input_active=true,.tty_out=sink};
    ui_motion_init(&g_session.motion,false);
    atomic_store(&g_composer_input_active,true);
    atomic_store(&g_composer_accepting_input,true);
    atomic_store(&g_session_active_fast,true);
    snprintf(g_session.input,sizeof(g_session.input),"preserve this draft π");
    g_session.input_cursor=strlen(g_session.input);
    g_session.tool_visuals[0]=(pixel_tool_visual_t){.used=true,.sequence=1,.status=PIXEL_OP_RUNNING,.started_s=monotonic_s()-2};
    strcpy(g_session.tool_visuals[0].name,"bash");
    strcpy(g_session.tool_visuals[0].preview,"make test");
    pixel_message_t *message=session_new_message("USER",NULL);
    message_text_set_plain(message,"Keep the UI alive while a tool runs.");
    message->streaming=false; g_session.current_message=-1;
    session_repaint(sink,true);
    pause_ms(250);
    pixel_tui_perf_set_capture(true); pixel_tui_perf_reset();
    atomic_store(&scheduler_waits, 0);
    session_lock(); session_animation_start(); session_unlock();
    pause_ms(2500);
    session_lock(); g_session.animation_stop=true; pthread_cond_signal(&g_animation_cond); session_unlock();
    pthread_join(g_session.animation_thread,NULL);
    /* Retain the async scheduling flag for the transition publication check. */
    pixel_tui_perf_snapshot_t perf=pixel_tui_perf_snapshot();
    printf("tool scheduler waits: %u in 2.5 seconds\n", atomic_load(&scheduler_waits));
    CHECK(atomic_load(&scheduler_waits)<=300,
          "focused tool animation parks between useful frames instead of polling at 1ms");
    printf("{\"frames\":%llu,\"patch\":%llu,\"full\":%llu,\"frame_p95_ms\":%.3f,\"frame_max_ms\":%.3f,\"wire_bytes\":%llu}\n",
        (unsigned long long)perf.frames,(unsigned long long)perf.patch_frames,
        (unsigned long long)perf.full_frames,perf.frame_ms_p95,perf.frame_ms_max,
        (unsigned long long)perf.wire_bytes);
    CHECK(perf.patch_frames>=50,"focused followup composer must not freeze running-tool animation");
    CHECK(perf.full_frames<=3,"tool liveness must not continuously raster the full Retina surface");
    CHECK(!strcmp(g_session.input,"preserve this draft π"),"draft survives tool animation");
    uint32_t resident=g_session.image_ids[g_session.state];
    long before=ftell(sink);
    pixel_tui_session_set_state(sink,PIXEL_TUI_RESPONDING);
    CHECK(ftell(sink)==before,"phase publication cannot delete the visible frame before replacement");
    CHECK(g_session.image_ids[PIXEL_TUI_RESPONDING]==resident,"resident image survives phase transition");
    CHECK(g_session.structural_repaint_pending,"phase still schedules semantic repaint");
    g_session.animation_thread_started=false;
    atomic_store(&g_animation_thread_fast,false);
    free(g_session.prev_frame); g_session.prev_frame=NULL;
    free(g_session.activity_underlay); g_session.activity_underlay=NULL;
    session_messages_free();
    fclose(sink); g_session.tty_out=NULL; g_session.active=false;
    printf("native activity: %d failures\n",failures);
    return failures ? 1 : 0;
}
