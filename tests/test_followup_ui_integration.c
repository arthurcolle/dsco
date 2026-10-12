/* Headless retained queue + real PTY composer fixture. No inference/windows.
 * Link like test_native_window_cache, excluding pixel_tui.o. */
#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
#include <signal.h>
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int failures, checks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
static native_followup_snapshot_t snap;
static native_followup_action_t action;
static atomic_bool publisher_done;
static void *publish(void *unused) {
    (void)unused;
    pixel_tui_set_followup_queue(&snap);
    atomic_store(&publisher_done, true);
    return NULL;
}
static void seed(void) {
    memset(&snap, 0, sizeof(snap));
    snap.revision=10; snap.count=2;
    snap.entries[0].id=42; snap.entries[0].revision=3;
    strcpy(snap.entries[0].text,"queued π");
    snap.entries[1].id=99; snap.entries[1].revision=7;
    strcpy(snap.entries[1].text,"second message");
}
static void resize_handler(int sig) { (void)sig; tui_composer_signal_resize(); }
static int composer_fixture(const char *path) {
    FILE *result=fopen(path,"w");
    if (!result) return 2;
    setenv("DSCO_PIXEL_TUI","1",1); setenv("DSCO_PIXEL_TUI_DPR","1",1);
    setenv("DSCO_PIXEL_TUI_ZOOM","0.65",1);
    setenv("DSCO_PIXEL_TUI_ANIMATIONS","0",1); setenv("DSCO_TUI_ANIMATIONS","0",1);
    tui_status_bar_t status; tui_status_bar_init(&status,"followup fixture");
    status.animations_enabled=false;
    if (!pixel_tui_session_begin(stderr,"followup fixture")) return 3;
    struct sigaction sa={.sa_handler=resize_handler}; sigemptyset(&sa.sa_mask); sigaction(SIGWINCH,&sa,NULL);
    pixel_tui_set_followup_queue(NULL); seed(); pixel_tui_set_followup_queue(&snap);
    tui_composer_set_action_wakeup(false);
    fprintf(result,"ready\n"); fflush(result);
    for (int i=0;i<3;++i) {
        char out[256]; memset(out,0x5a,sizeof(out));
        char *returned=tui_composer_read(&status,NULL,out,sizeof(out));
        fprintf(result,"return:%d:%s\n", returned==out,returned?returned:"<null>");
        while (pixel_tui_poll_followup_action(&action))
            fprintf(result,"action:%d:%llu:%llu:%s\n",action.kind,
                    (unsigned long long)action.id,(unsigned long long)action.expected_revision,action.text);
        fflush(result);
    }
    pixel_tui_session_end(stderr); tui_composer_clear_retained_draft(); tui_cleanup();
    pthread_mutex_destroy(&status.mutex); fclose(result); return 0;
}
int main(int argc,char **argv) {
    if(argc==2) return composer_fixture(argv[1]);
    FILE *sink=tmpfile(); CHECK(sink); if(!sink)return 2;
    pixel_tui_set_followup_queue(NULL); seed();
    pthread_t thread;
    session_lock();
    CHECK(pthread_create(&thread,NULL,publish,NULL)==0);
    struct timespec delay={.tv_nsec=20000000}; nanosleep(&delay,NULL);
    CHECK(!atomic_load(&publisher_done)); session_unlock(); pthread_join(thread,NULL);
    CHECK(atomic_load(&publisher_done));
    CHECK(s_followups.count==2 && s_followups.revision==10 && !s_followup_visible);
    strcpy(snap.entries[0].text,"caller changed its snapshot");
    CHECK(!strcmp(s_followups.entries[0].text,"queued π"));
    snap.revision=9; snap.count=0; pixel_tui_set_followup_queue(&snap);
    CHECK(s_followups.count==2 && s_followups.revision==10);
    snap.revision=10; pixel_tui_set_followup_queue(&snap); CHECK(s_followups.count==2);
    seed(); snap.revision=11; snap.entries[1].id=42;
    pixel_tui_set_followup_queue(&snap); CHECK(s_followups.revision==10);
    g_session=(pixel_session_t){.active=true,.animation_thread_started=true,.width=960,.height=640,
        .cols=96,.rows=32,.current_message=-1,.tty_out=sink};
    CHECK(pixel_tui_session_followup_key(sink,0x11));
    CHECK(pixel_tui_followup_focused());
    CHECK(pixel_tui_session_followup_key(sink,'\r'));
    CHECK(s_followup_id==42 && s_followup_revision==3);
    CHECK(pixel_tui_session_followup_key(sink,NATIVE_WINDOW_KEY_HOME));
    CHECK(pixel_tui_followup_insert_paste(sink,42,"new 🦉 ",strlen("new 🦉 ")));
    CHECK(pixel_tui_session_followup_key(sink,0x13));
    CHECK(pixel_tui_followup_action_pending());
    CHECK(pixel_tui_poll_followup_action(&action));
    CHECK(action.kind==NATIVE_FOLLOWUP_ACTION_REPLACE && action.id==42 && action.expected_revision==3);
    CHECK(!strcmp(action.text,"new 🦉 queued π"));
    CHECK(!pixel_tui_poll_followup_action(&action));
    seed(); snap.revision=12; snap.entries[0].revision=4; strcpy(snap.entries[0].text,"backend changed");
    pixel_tui_set_followup_queue(&snap);
    CHECK(!strcmp(s_followup_draft,"new 🦉 queued π"));
    pixel_tui_session_followup_key(sink,0x13); CHECK(!pixel_tui_followup_action_pending());
    pixel_tui_session_followup_key(sink,18); CHECK(!strcmp(s_followup_draft,"backend changed"));
    pixel_tui_session_followup_key(sink,0x04); CHECK(pixel_tui_poll_followup_action(&action));
    CHECK(action.kind==NATIVE_FOLLOWUP_ACTION_REMOVE && action.id==42 && action.expected_revision==4);
    pixel_tui_session_followup_key(sink,27);
    native_ui_rect_t r=session_windows_work_area(960,640,PIXEL_TUI_IDLE);
    CHECK(followup_pointer(0,r.x+r.width-25,r.y+76,false));
    snap.revision=13; snap.entries[0]=snap.entries[1]; snap.count=1;
    pixel_tui_set_followup_queue(&snap);
    CHECK(followup_pointer(0,r.x+r.width-25,r.y+76,true));
    CHECK(!pixel_tui_followup_action_pending()); /* pressed row cannot retarget */
    pixel_tui_session_followup_key(sink,0x04); CHECK(pixel_tui_poll_followup_action(&action));
    CHECK(action.id==99 && action.expected_revision==7);
    for(int i=0;i<FOLLOWUP_ACTION_CAP+1;++i) pixel_tui_session_followup_key(sink,0x04);
    CHECK(s_followup_count==FOLLOWUP_ACTION_CAP);
    while(pixel_tui_poll_followup_action(&action)) CHECK(action.id==99 && action.expected_revision==7);
    const int widths[]={320,960,1440}; const double scales[]={.65,1,1.3};
    for(int i=0;i<3;++i) {
        g_session.width=widths[i]; g_session.height=640;
        px_canvas_t *c=canvas_acquire_scaled(widths[i],640,scales[i]); CHECK(c);
        if(c) { fill_rect(c,0,0,widths[i],640,C_BG_TOP,1); draw_session_followups(c);
            native_ui_rect_t area=session_windows_work_area(widths[i],640,PIXEL_TUI_IDLE);
            CHECK(area.width>140 && area.height>134);
            px_color_t panel=c->pixels[(int)((area.y+2)*scales[i])*c->pixel_width+(int)((area.x+2)*scales[i])];
            px_color_t outside=c->pixels[c->pixel_width-1];
            CHECK(memcmp(&panel,&outside,sizeof(panel))!=0); free_canvas(c); }
    }
    g_session.active=false; pixel_tui_set_followup_queue(NULL);
    CHECK(!pixel_tui_followup_action_pending() && !pixel_tui_followup_focused());
    fclose(sink); printf("followup UI: %d checks, %d failures\n",checks,failures); return failures?1:0;
}
