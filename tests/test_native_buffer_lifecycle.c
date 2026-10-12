/* Reuse the governed read fixture, injecting deterministic interleavings after
 * a read has captured its reply. No real buffers, clipboard, UI or providers. */
#define main adapter_contract_main
#define tools_execute_for_tier fixture_buffer_read
#include "test_native_window_tool.c"
#undef tools_execute_for_tier
#undef main

static int interleave;
static native_windows_snapshot_t newest;

bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t cap) {
    bool ok=fixture_buffer_read(name,input,tier,out,cap);
    int action=interleave; interleave=0;
    if(ok && action) {
        native_windows_snapshot_t before=snapshot();
        uint64_t id=before.windows[0].id;
        if(action==1) {
            check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS,0),"human activates editor during delayed read");
            check(native_windows_insert_paste(id," newer saved edit",17),"human edits while refresh is in flight");
            native_window_save_request_t save;
            check(native_windows_take_save_request(&save),"human takes explicit save request");
            memset(source_revision,'b',64);
            strcpy(source_text,save.content);
            native_windows_finish_save(&save,true,source_revision,NULL);
        } else if(action==2) {
            char result2[2048];memset(source_revision,'c',64);strcpy(source_text,"newer refresh won");
            check(native_windows_bind_buffer(id,before.windows[0].title,source_text,BUFFER_ID,source_revision,"fixture",false,result2,sizeof(result2)),"second refresh completes first");
        } else if(action==3) {
            char q[256],r[2048];snprintf(q,sizeof(q),"{\"action\":\"update\",\"id\":%llu,\"title\":\"Human renamed during read\"}",(unsigned long long)id);
            check(native_windows_command(q,r,sizeof(r)),"human renames pane during read");
        } else if(action==4) {
            with_id("close",id,true);
        }
        newest=snapshot();
    }
    return ok;
}

static uint64_t open_text(const char *text) {
    reset();source_sensitive=false;snprintf(source_text,sizeof(source_text),"%s",text);
    success("{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"workspace\":\"fixture\"}");
    return snapshot().windows[0].id;
}
static native_window_t pane(uint64_t id) {
    native_windows_snapshot_t s=snapshot();
    for(int i=0;i<s.count;i++)if(s.windows[i].id==id)return s.windows[i];
    check(false,"requested pane exists");return s.windows[0];
}
static void save_locally(void) {
    native_window_save_request_t save;
    check(native_windows_take_save_request(&save),"explicit save available");
    strcpy(source_text,save.content);memset(source_revision,'d',64);
    native_windows_finish_save(&save,true,source_revision,NULL);
}
static void caret_visible(uint64_t id,const char *label) {
    native_window_t w=pane(id);int row=0,col=0;
    int columns=(w.rect.width-24)/8;if(columns<1)columns=1;if(columns>240)columns=240;
    int available=(w.rect.height-NATIVE_WINDOW_TITLE_HEIGHT-38)/18;if(available<1)available=1;
    check(native_window_editor_position(&w,columns,&row,&col),"caret has a valid UTF-8 position");
    if(!(row>=w.scroll && row<w.scroll+available))fprintf(stderr,"caret row=%d scroll=%d visible_rows=%d\n",row,w.scroll,available);
    check(row>=w.scroll && row<w.scroll+available,label);
}
static void test_same_revision(void) {
    uint64_t id=open_text("base π");
    check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS,0),"activate editor");
    check(native_windows_insert_paste(id," suffix",7),"append owned text");save_locally();
    check(native_windows_key(NATIVE_WINDOW_KEY_LEFT,NATIVE_WINDOW_MOD_SHIFT),"select one character");
    native_window_t before=pane(id);
    with_id("refresh",id,true);native_window_t after=pane(id);
    check(after.editor_active && !memcmp(&before.editor,&after.editor,sizeof(before.editor)),"unchanged refresh retains editing focus, selection and undo history");
    check(after.edit_generation==before.edit_generation && after.scroll==before.scroll,"unchanged refresh does not invalidate editor generation or viewport");
    check(native_windows_key(0x1a,0),"undo after no-op refresh");
    check(equal(pane(id).text,"base π") && pane(id).dirty,"saved edit can still be undone after no-op refresh");
}
static void test_stale(int action) {
    uint64_t id=open_text("old canonical");interleave=action;
    with_id("refresh",id,false);
    check(strstr(result,"buffer_changed_during_refresh")!=NULL,"late refresh reports a retryable changed-view conflict");
    unchanged(newest);
    check(equal(pane(id).text,source_text),"late old reply does not replace newer saved/refreshed text");
}
static void test_closed(void) {
    uint64_t id=open_text("old canonical");interleave=4;
    with_id("refresh",id,false);
    check(strstr(result,"buffer_window_not_found")!=NULL,"closed pane refresh is rejected before new-window allocation");
    unchanged(newest);check(snapshot().count==0,"stale refresh cannot resurrect a closed pane");
}
static void test_pending(void) {
    uint64_t id=open_text("base");check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS,0),"activate editor");
    check(native_windows_key(0xe2,0),"first byte of snowman retained");
    native_windows_snapshot_t before=snapshot();
    with_id("refresh",id,false);unchanged(before);
    check(native_windows_key(0x98,0) && native_windows_key(0x83,0),"complete pending UTF-8 input");
    check(equal(pane(id).text,"base☃"),"refresh did not drop partially entered Unicode");
}
static void test_title(void) {
    uint64_t id=open_text("base");interleave=3;with_id("refresh",id,true);
    check(equal(pane(id).title,"Human renamed during read"),"refresh preserves concurrent human pane title");
}
static void test_tab(void) {
    uint64_t first=open_text("first");
    strcpy(source_text,"second");
    success("{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"title\":\"Second buffer\",\"workspace\":\"fixture\"}");
    uint64_t second=snapshot().focused_id;
    check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS,0),"activate second buffer");
    check(native_windows_key(NATIVE_WINDOW_KEY_TAB,0),"cycle to first buffer");
    check(snapshot().focused_id==first && pane(first).editor_active && !pane(second).editor_active,"Tab focuses the selected buffer after z-order reordering");
    check(native_windows_key('Q',0),"type into newly focused buffer");
    check(equal(pane(first).text,"firstQ") && equal(pane(second).text,"second"),"typing after Tab edits only the selected buffer");
}
static bool check_initial_focus;
static void test_geometry(bool trailing) {
    char text[4096]="";
    for(int i=0;i<60;i++)strcat(text,"line 雪\n");
    if(!trailing)strcat(text,"tail");
    uint64_t id=open_text(text);check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS,0),"focus long buffer at EOF");
    if(!check_initial_focus)check(native_windows_key(NATIVE_WINDOW_KEY_END,NATIVE_WINDOW_MOD_CTRL),"explicitly navigate to EOF");
    caret_visible(id,"end-of-document caret is visible, including trailing newline");
    if(trailing)return;
    native_window_t before=pane(id);
    char q[256];snprintf(q,sizeof(q),"{\"action\":\"resize\",\"id\":%llu,\"width\":240,\"height\":160}",(unsigned long long)id);
    success(q);caret_visible(id,"caret stays visible after explicit window shrink");
    native_windows_set_work_area((native_ui_rect_t){0,0,280,210});caret_visible(id,"caret stays visible after terminal shrink");
    with_id("zoom",id,true);caret_visible(id,"caret stays visible after zoom");
    with_id("zoom",id,true);caret_visible(id,"caret stays visible after unzoom");
    success("{\"action\":\"tile\"}");caret_visible(id,"caret stays visible after tiling");
    success("{\"action\":\"cascade\"}");caret_visible(id,"caret stays visible after cascading");
    native_window_t after=pane(id);
    check(equal(before.text,after.text) && !memcmp(&before.editor,&after.editor,sizeof(before.editor)) && before.edit_generation==after.edit_generation,"all reflows preserve text, selection, undo ownership and edit generation");
}
int main(int argc,char **argv) {
    alarm(15);
    const char *test=argc>1?argv[1]:"all";
    if(equal(test,"same")||equal(test,"all"))test_same_revision();
    if(equal(test,"save-race")||equal(test,"all"))test_stale(1);
    if(equal(test,"refresh-race")||equal(test,"all"))test_stale(2);
    if(equal(test,"closed")||equal(test,"all"))test_closed();
    if(equal(test,"pending")||equal(test,"all"))test_pending();
    if(equal(test,"title")||equal(test,"all"))test_title();
    if(equal(test,"tab")||equal(test,"all"))test_tab();
    if(equal(test,"focus")||equal(test,"all")){check_initial_focus=true;test_geometry(false);check_initial_focus=false;}
    if(equal(test,"eof")||equal(test,"all"))test_geometry(true);
    if(equal(test,"resize")||equal(test,"all"))test_geometry(false);
    native_windows_reset();alarm(0);printf("native_buffer_lifecycle (%s): %u checks passed\n",test,checks);return 0;
}
